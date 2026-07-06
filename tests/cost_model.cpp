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

void assert_order_by_keeps_join_alternatives_below_sort() {
    const auto catalog = make_skewed_catalog();
    const auto sql =
        "SELECT big.k, mid.k, tiny.k FROM big JOIN mid ON big.k = mid.k "
        "JOIN tiny ON mid.k = tiny.k ORDER BY tiny.k DESC, big.k ASC";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::GroupId root = 0;
    auto memo = explored_memo_for(logical, root);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{64, 512});
    const auto best = memo.extract_best(root, catalog);

    const auto expected_best =
        std::string("Sort[col(tiny.k) DESC, col(big.k) ASC]\n") +
        "  Project[big.k=col(big.k), mid.k=col(mid.k), tiny.k=col(tiny.k)]\n"
        "    Join[col(big.k) = col(mid.k)]\n"
        "      Scan[big]\n"
        "      Join[col(mid.k) = col(tiny.k)]\n"
        "        Scan[mid]\n"
        "        Scan[tiny]";

    std::cout << "order-by multi-join alternatives verified: alternatives=" << alternatives.plans.size()
              << " max_group_expressions=" << alternatives.max_group_expression_count
              << " hit_expression_bound=" << (alternatives.hit_expression_bound ? "yes" : "no")
              << " hit_plan_bound=" << (alternatives.hit_plan_bound ? "yes" : "no") << "\n";

    if (alternatives.plans.size() <= 1 || alternatives.hit_expression_bound || alternatives.hit_plan_bound ||
        plan::to_string(best) != expected_best || plan::to_string(best) == plan::to_string(logical)) {
        std::cerr << "ORDER BY blocked join alternatives below Sort\n"
                  << "sql: " << sql << "\n"
                  << "alternative count: " << alternatives.plans.size() << "\n"
                  << "ingested plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "best plan:\n"
                  << plan::to_string(best) << "\n"
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

void assert_aliased_self_join_costs_physical_table_stats() {
    const auto catalog = make_skewed_catalog();
    const auto sql = "SELECT x.k, y.k FROM big AS x JOIN big AS y ON x.k = y.k";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    const auto estimate = optimizer::estimate_cost(logical, catalog);
    if (estimate.rows != 1000.0 || estimate.cost != 4000.0) {
        std::cerr << "aliased self-join did not cost through physical table stats\n"
                  << "sql: " << sql << "\n"
                  << "plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "rows: " << estimate.rows << "\n"
                  << "cost: " << estimate.cost << "\n";
        std::terminate();
    }

    optimizer::GroupId root = 0;
    auto memo = explored_memo_for(logical, root);
    const auto best = memo.extract_best(root, catalog);
    const auto best_estimate = optimizer::estimate_cost(best, catalog);
    if (best_estimate.rows != 1000.0 || best_estimate.cost != 4000.0) {
        std::cerr << "extract_best did not preserve aliased physical stats lookup\n"
                  << "sql: " << sql << "\n"
                  << "best plan:\n"
                  << plan::to_string(best) << "\n"
                  << "rows: " << best_estimate.rows << "\n"
                  << "cost: " << best_estimate.cost << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }
}

void assert_aggregate_cost_uses_group_count() {
    const auto catalog = make_skewed_catalog();

    const auto grouped_sql = "SELECT k, COUNT(*) FROM big GROUP BY k";
    const auto grouped = sql::bind_select(sql::parse_select(grouped_sql), catalog);
    const auto grouped_estimate = optimizer::estimate_cost(grouped, catalog);
    if (grouped_estimate.rows != 1000.0 || grouped_estimate.cost != 3000.0) {
        std::cerr << "GROUP BY aggregate cost did not use input rows plus group count\n"
                  << "sql: " << grouped_sql << "\n"
                  << "plan:\n"
                  << plan::to_string(grouped) << "\n"
                  << "rows: " << grouped_estimate.rows << "\n"
                  << "cost: " << grouped_estimate.cost << "\n";
        std::terminate();
    }

    const auto global_sql = "SELECT COUNT(*) FROM tiny";
    const auto global = sql::bind_select(sql::parse_select(global_sql), catalog);
    const auto global_estimate = optimizer::estimate_cost(global, catalog);
    if (global_estimate.rows != 1.0 || global_estimate.cost != 5.0) {
        std::cerr << "global aggregate cost did not use one output group\n"
                  << "sql: " << global_sql << "\n"
                  << "plan:\n"
                  << plan::to_string(global) << "\n"
                  << "rows: " << global_estimate.rows << "\n"
                  << "cost: " << global_estimate.cost << "\n";
        std::terminate();
    }
}

void assert_group_by_does_not_block_join_transforms() {
    const auto catalog = make_skewed_catalog();
    const auto sql =
        "SELECT big.k, COUNT(*) FROM big JOIN mid ON big.k = mid.k JOIN tiny ON mid.k = tiny.k GROUP BY big.k";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::GroupId root = 0;
    auto memo = explored_memo_for(logical, root);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{64, 512});
    const auto best = memo.extract_best(root, catalog);

    const auto expected_best =
        std::string("Project[big.k=col(big.k), COUNT(*)=col(COUNT(*))]\n") +
        "  Aggregate[group_keys=[col(big.k)], aggregates=[COUNT(*)]]\n"
        "    Join[col(big.k) = col(mid.k)]\n"
        "      Scan[big]\n"
        "      Join[col(mid.k) = col(tiny.k)]\n"
        "        Scan[mid]\n"
        "        Scan[tiny]";

    std::cout << "group-by multi-join alternatives verified: alternatives=" << alternatives.plans.size()
              << " max_group_expressions=" << alternatives.max_group_expression_count
              << " hit_expression_bound=" << (alternatives.hit_expression_bound ? "yes" : "no")
              << " hit_plan_bound=" << (alternatives.hit_plan_bound ? "yes" : "no") << "\n";

    if (alternatives.plans.size() <= 1 || alternatives.hit_expression_bound || alternatives.hit_plan_bound ||
        plan::to_string(best) != expected_best || plan::to_string(best) == plan::to_string(logical)) {
        std::cerr << "GROUP BY blocked join alternatives below Aggregate\n"
                  << "sql: " << sql << "\n"
                  << "alternative count: " << alternatives.plans.size() << "\n"
                  << "ingested plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "best plan:\n"
                  << plan::to_string(best) << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }
}

} // namespace

int main() {
    assert_skewed_catalog_changes_join_order();
    assert_best_plan_is_enumerated_and_semantic();
    assert_order_by_keeps_join_alternatives_below_sort();
    assert_equal_stats_tie_is_deterministic();
    assert_aliased_self_join_costs_physical_table_stats();
    assert_aggregate_cost_uses_group_count();
    assert_group_by_does_not_block_join_transforms();
    return 0;
}
