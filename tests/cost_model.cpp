#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/logical_plan.hpp"
#include "sql/binder.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

storage::ColumnarBatch make_single_key_batch(std::int64_t row_count) {
    storage::Int64Column key;
    for (std::int64_t value = 0; value < row_count; ++value) {
        key.append(value);
    }

    storage::ColumnarBatch batch;
    batch.add_column("k", std::move(key));
    return batch;
}

execution::Catalog make_skewed_catalog() {
    execution::Catalog catalog;
    catalog.add_table("big", make_single_key_batch(1000));
    catalog.add_table("mid", make_single_key_batch(100));
    catalog.add_table("tiny", make_single_key_batch(2));
    return catalog;
}

execution::Catalog make_equal_catalog() {
    execution::Catalog catalog;
    catalog.add_table("a", make_single_key_batch(10));
    catalog.add_table("b", make_single_key_batch(10));
    catalog.add_table("c", make_single_key_batch(10));
    return catalog;
}

std::vector<std::string> sorted_column_names(const storage::ColumnarBatch& batch) {
    auto names = batch.column_names();
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::vector<std::int64_t>> sorted_rows_by_column_identity(const storage::ColumnarBatch& batch) {
    const auto names = sorted_column_names(batch);
    std::vector<std::vector<std::int64_t>> rows;
    rows.reserve(batch.row_count());
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        std::vector<std::int64_t> values;
        values.reserve(names.size());
        for (const auto& name : names) {
            values.push_back(batch.column(name).at(row));
        }
        rows.push_back(std::move(values));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

bool same_sorted_bag(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return sorted_column_names(left) == sorted_column_names(right) &&
           sorted_rows_by_column_identity(left) == sorted_rows_by_column_identity(right);
}

bool contains_plan(const std::vector<plan::LogicalPlan>& alternatives, const plan::LogicalPlan& candidate) {
    const auto candidate_text = plan::to_string(candidate);
    return std::any_of(alternatives.begin(), alternatives.end(), [&](const auto& alternative) {
        return plan::to_string(alternative) == candidate_text;
    });
}

optimizer::Memo explored_memo_for(const plan::LogicalPlan& logical, optimizer::GroupId& root) {
    optimizer::Memo memo;
    root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);
    return memo;
}

void assert_skewed_catalog_changes_join_order() {
    const auto catalog = make_skewed_catalog();
    const auto sql =
        "SELECT big.k, mid.k, tiny.k FROM big JOIN mid ON big.k = mid.k JOIN tiny ON mid.k = tiny.k";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::GroupId root = 0;
    auto memo = explored_memo_for(logical, root);
    const auto best = memo.extract_best(root, catalog);
    const auto ingested_cost = optimizer::estimate_cost(logical, catalog);
    const auto best_cost = optimizer::estimate_cost(best, catalog);

    const auto expected_best =
        std::string("Project[big.k=col(big.k), mid.k=col(mid.k), tiny.k=col(tiny.k)]\n") +
        "  Join[col(big.k) = col(mid.k)]\n"
        "    Scan[big]\n"
        "    Join[col(mid.k) = col(tiny.k)]\n"
        "      Scan[mid]\n"
        "      Scan[tiny]";

    if (plan::to_string(best) != expected_best || !(best_cost.cost <= ingested_cost.cost)) {
        std::cerr << "cost model did not pick the expected skew-aware join order\n"
                  << "sql: " << sql << "\n"
                  << "ingested plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "ingested cost: " << ingested_cost.cost << "\n"
                  << "best plan:\n"
                  << plan::to_string(best) << "\n"
                  << "best cost: " << best_cost.cost << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }
}

void assert_best_plan_is_enumerated_and_semantic() {
    const auto catalog = make_skewed_catalog();
    const auto sql =
        "SELECT big.k, mid.k, tiny.k FROM big JOIN mid ON big.k = mid.k JOIN tiny ON mid.k = tiny.k";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::GroupId root = 0;
    auto memo = explored_memo_for(logical, root);
    const auto best = memo.extract_best(root, catalog);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{64, 512});
    assert(!alternatives.hit_expression_bound);
    assert(!alternatives.hit_plan_bound);

    const auto best_cost = optimizer::estimate_cost(best, catalog);
    auto min_alternative_cost = best_cost.cost;
    for (const auto& alternative : alternatives.plans) {
        min_alternative_cost = std::min(min_alternative_cost, optimizer::estimate_cost(alternative, catalog).cost);
    }

    const auto oracle = execution::execute_interpreted(logical, catalog);
    const auto best_oracle = execution::execute_interpreted(best, catalog);
    const auto best_vectorized = execution::execute_vectorized(best, catalog);
    if (!contains_plan(alternatives.plans, best) || best_cost.cost != min_alternative_cost ||
        !same_sorted_bag(oracle, best_oracle) || !same_sorted_bag(best_oracle, best_vectorized)) {
        std::cerr << "extract_best violated the equivalent-alternatives invariant\n"
                  << "sql: " << sql << "\n"
                  << "best plan:\n"
                  << plan::to_string(best) << "\n"
                  << "best cost: " << best_cost.cost << "\n"
                  << "min alternative cost: " << min_alternative_cost << "\n"
                  << "alternative count: " << alternatives.plans.size() << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }
}

void assert_equal_stats_tie_is_deterministic() {
    const auto catalog = make_equal_catalog();
    const auto sql = "SELECT a.k, b.k, c.k FROM a JOIN b ON a.k = b.k JOIN c ON b.k = c.k";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::GroupId first_root = 0;
    auto first_memo = explored_memo_for(logical, first_root);
    const auto first = first_memo.extract_best(first_root, catalog);

    optimizer::GroupId second_root = 0;
    auto second_memo = explored_memo_for(logical, second_root);
    const auto second = second_memo.extract_best(second_root, catalog);

    if (plan::to_string(first) != plan::to_string(second)) {
        std::cerr << "equal-stats extraction tie was not deterministic\n"
                  << "first plan:\n"
                  << plan::to_string(first) << "\n"
                  << "second plan:\n"
                  << plan::to_string(second) << "\n";
        std::terminate();
    }
}

} // namespace

int main() {
    assert_skewed_catalog_changes_join_order();
    assert_best_plan_is_enumerated_and_semantic();
    assert_equal_stats_tie_is_deterministic();
    return 0;
}
