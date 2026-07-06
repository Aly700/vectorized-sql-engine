#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/memo.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/logical_plan.hpp"
#include "sql/binder.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

execution::Catalog make_catalog() {
    storage::Int64Column a;
    for (auto value : {1, 2, 2, 3}) {
        a.append(value);
    }

    storage::Int64Column b;
    for (auto value : {10, 20, 30, 40}) {
        b.append(value);
    }

    storage::ColumnarBatch batch;
    batch.add_column("a", std::move(a));
    batch.add_column("b", std::move(b));

    execution::Catalog catalog;
    catalog.add_table("t", std::move(batch));
    return catalog;
}

std::string format_batch(const storage::ColumnarBatch& batch) {
    std::ostringstream out;
    const auto& column_order = batch.column_names();
    out << "columns=[";
    for (std::size_t i = 0; i < column_order.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << column_order[i];
    }
    out << "]; rows=[";
    for (std::size_t row = 0; row < batch.row_count(); ++row) {
        if (row != 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t col = 0; col < column_order.size(); ++col) {
            if (col != 0) {
                out << ",";
            }
            out << batch.column(column_order[col]).at(row);
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

optimizer::MemoExpression join_expression(optimizer::GroupId left, optimizer::GroupId right) {
    optimizer::MemoExpression expression;
    expression.kind = optimizer::MemoExpressionKind::Join;
    expression.children.push_back(left);
    expression.children.push_back(right);
    return expression;
}

void assert_memo_ingest_deduplicates_bound_plan_tree() {
    const auto catalog = make_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT a FROM t WHERE a = 2"), catalog);

    optimizer::Memo memo;
    const auto first_root = memo.insert(logical);
    const auto second_root = memo.insert(logical);

    assert(first_root == second_root);
    assert(memo.group_count() == 3);
    assert(plan::to_string(memo.extract(first_root)) == plan::to_string(logical));
    memo.assert_invariants();
}

void assert_memo_exploration_adds_equivalent_expressions() {
    const auto catalog = make_catalog();
    const auto logical = sql::bind_select(sql::parse_select("SELECT a FROM t WHERE 2 > 1"), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());

    assert(explored.reached_fixpoint);
    assert(explored.fired_rules.size() >= 2);
    const auto dump = memo.dump();
    assert(dump.find("ConstantFoldComparisonRule") == std::string::npos);
    assert(dump.find("Filter[lit(1) = lit(1)] children=[1]") != std::string::npos);
    assert(dump.find("GroupRef[1]") != std::string::npos);
    assert(plan::to_string(memo.extract(root)) == plan::to_string(logical));
    memo.assert_invariants();
}

void assert_memo_extracted_plan_matches_both_engines() {
    const auto catalog = make_catalog();
    const auto sql = "SELECT a FROM t WHERE 2 < 1 AND a = 2";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);
    const auto extracted = memo.extract(root);

    const auto oracle = execution::execute_interpreted(logical, catalog);
    const auto memo_oracle = execution::execute_interpreted(extracted, catalog);
    const auto memo_vectorized = execution::execute_vectorized(extracted, catalog);
    if (format_batch(oracle) == format_batch(memo_oracle) && format_batch(oracle) == format_batch(memo_vectorized)) {
        return;
    }

    std::cerr << "memo extracted plan mismatch\n"
              << "sql: " << sql << "\n"
              << "memo:\n"
              << memo.dump()
              << "extracted plan:\n"
              << plan::to_string(extracted) << "\n"
              << "oracle:          " << format_batch(oracle) << "\n"
              << "memo oracle:     " << format_batch(memo_oracle) << "\n"
              << "memo vectorized: " << format_batch(memo_vectorized) << "\n";
    std::terminate();
}

void assert_cross_group_duplicate_expression_merges_groups() {
    optimizer::Memo memo;
    const auto left = memo.insert(plan::LogicalPlan::scan("left_t"));
    const auto right = memo.insert(plan::LogicalPlan::scan("right_t"));

    const auto left_right = memo.insert_expression(join_expression(left, right));
    const auto right_left = memo.insert_expression(join_expression(right, left));
    assert(left_right != right_left);

    try {
        assert(memo.insert_equivalent(left_right, join_expression(right, left)));
    } catch (const std::logic_error& error) {
        std::cerr << "cross-group duplicate did not merge\n"
                  << "error: " << error.what() << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }

    memo.assert_invariants();
    const auto dump = memo.dump();
    const auto expected = "group " + std::to_string(right_left) + " -> representative " + std::to_string(left_right);
    if (dump.find(expected) == std::string::npos) {
        std::cerr << "merged group representative was not observable\n"
                  << "expected: " << expected << "\n"
                  << "memo dump:\n"
                  << dump;
        std::terminate();
    }
}

void assert_aliased_self_join_scans_remain_distinct_groups() {
    const auto catalog = make_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT x.a, y.a FROM t AS x JOIN t AS y ON x.a = y.a"), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto dump = memo.dump();
    if (memo.group_count() != 4 || dump.find("Scan[t AS x]") == std::string::npos ||
        dump.find("Scan[t AS y]") == std::string::npos) {
        std::cerr << "aliased self-join scans were not distinct memo groups\n"
                  << "plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << dump;
        std::terminate();
    }

    const auto extracted = memo.extract(root);
    assert(plan::to_string(extracted) == plan::to_string(logical));
    memo.assert_invariants();
}

} // namespace

int main() {
    assert_memo_ingest_deduplicates_bound_plan_tree();
    assert_memo_exploration_adds_equivalent_expressions();
    assert_memo_extracted_plan_matches_both_engines();
    assert_cross_group_duplicate_expression_merges_groups();
    assert_aliased_self_join_scans_remain_distinct_groups();
    return 0;
}
