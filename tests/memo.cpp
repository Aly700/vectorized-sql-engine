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
#include <vector>

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

    storage::Int64Column t1_a;
    for (auto value : {1, 2, 2}) {
        t1_a.append(value);
    }
    storage::Int64Column t1_b;
    for (auto value : {10, 20, 30}) {
        t1_b.append(value);
    }
    storage::ColumnarBatch t1;
    t1.add_column("a", std::move(t1_a));
    t1.add_column("b", std::move(t1_b));
    catalog.add_table("t1", std::move(t1));

    storage::Int64Column t2_a;
    for (auto value : {2, 2, 3}) {
        t2_a.append(value);
    }
    storage::Int64Column t2_c;
    for (auto value : {200, 201, 300}) {
        t2_c.append(value);
    }
    storage::ColumnarBatch t2;
    t2.add_column("a", std::move(t2_a));
    t2.add_column("c", std::move(t2_c));
    catalog.add_table("t2", std::move(t2));
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
            const auto& column = batch.column(column_order[col]);
            if (column.is_null(row)) {
                out << "NULL";
            } else {
                out << column.at(row);
            }
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

bool trace_contains(const std::vector<std::string>& trace, const std::string& rule_name) {
    for (const auto& fired : trace) {
        if (fired == rule_name) {
            return true;
        }
    }
    return false;
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

void assert_string_literal_and_int_literal_are_distinct_memo_expressions() {
    const auto catalog = make_catalog();
    const auto int_literal = sql::bind_select(sql::parse_select("SELECT 1 AS x FROM t"), catalog);
    const auto string_literal = sql::bind_select(sql::parse_select("SELECT '1' AS x FROM t"), catalog);

    optimizer::Memo memo;
    const auto int_root = memo.insert(int_literal);
    const auto string_root = memo.insert(string_literal);
    if (int_root == string_root) {
        std::cerr << "typed literal memo expressions collided\n"
                  << "int plan:\n"
                  << plan::to_string(int_literal) << "\n"
                  << "string plan:\n"
                  << plan::to_string(string_literal) << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }

    const auto dump = memo.dump();
    assert(dump.find("Project[x=lit(1)]") != std::string::npos);
    assert(dump.find("Project[x=lit('1')]") != std::string::npos);
    memo.assert_invariants();
}

void assert_having_filter_round_trips_through_memo() {
    const auto catalog = make_catalog();
    const auto logical =
        sql::bind_select(sql::parse_select("SELECT a FROM t GROUP BY a HAVING SUM(b) > 20"), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto dump = memo.dump();
    if (dump.find("Filter[col(SUM(b)) > lit(20)]") == std::string::npos ||
        dump.find("Aggregate[group_keys=[col(t.a)], aggregates=[SUM(b)=col(t.b)]]") == std::string::npos) {
        std::cerr << "HAVING filter was not represented as a memo filter over aggregate output\n"
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

void assert_filter_into_join_adds_pushdown_alternative() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE t1.b = 20 AND t2.c > 200 AND t1.b < t2.c";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);

    const auto dump = memo.dump();
    if (!trace_contains(explored.fired_rules, "FilterIntoJoinRule") ||
        dump.find("Filter[col(t1.b) = lit(20)]") == std::string::npos ||
        dump.find("Filter[col(t2.c) > lit(200)]") == std::string::npos ||
        dump.find("Join[col(t1.a) = col(t2.a) AND col(t1.b) < col(t2.c)]") == std::string::npos) {
        std::cerr << "filter-into-join pushdown alternative was not represented\n"
                  << "sql: " << sql << "\n"
                  << "root group: " << root << "\n"
                  << "trace: ";
        for (const auto& fired : explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nlogical plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << dump;
        std::terminate();
    }

    memo.assert_invariants();
}

void assert_filter_into_join_moves_one_side_or_but_not_mixed_side_or() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE (t1.b = 20 OR t1.b = 30) AND (t1.b = 20 OR t2.c = 201)";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);

    const auto dump = memo.dump();
    const std::string left_only_or = "Filter[(col(t1.b) = lit(20) OR col(t1.b) = lit(30))]";
    const std::string mixed_or = "(col(t1.b) = lit(20) OR col(t2.c) = lit(201))";
    const std::string illegal_join_or =
        "Join[col(t1.a) = col(t2.a) AND (col(t1.b) = lit(20) OR col(t2.c) = lit(201))]";

    if (!trace_contains(explored.fired_rules, "FilterIntoJoinRule") ||
        dump.find(left_only_or) == std::string::npos ||
        dump.find("Filter[" + mixed_or + "]") == std::string::npos ||
        dump.find(illegal_join_or) != std::string::npos) {
        std::cerr << "OR predicate mobility did not move only whole one-side trees\n"
                  << "sql: " << sql << "\n"
                  << "root group: " << root << "\n"
                  << "trace: ";
        for (const auto& fired : explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nlogical plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << dump;
        std::terminate();
    }

    memo.assert_invariants();
}

bool contains_plan_text(const std::vector<plan::LogicalPlan>& alternatives, const std::string& text) {
    for (const auto& alternative : alternatives) {
        if (plan::to_string(alternative).find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void assert_filter_through_aggregate_pushes_only_group_keys() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING t1.a = 2 AND COUNT(*) > 1";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
    assert(!alternatives.hit_expression_bound);
    assert(!alternatives.hit_plan_bound);

    const auto pushed_group_key =
        std::string("Filter[col(COUNT(*)) > lit(1)]\n") +
        "    Aggregate[group_keys=[col(t1.a)], aggregates=[COUNT(*)]]\n"
        "      Filter[col(t1.a) = lit(2)]";
    const auto illegal_aggregate_output_push =
        std::string("Aggregate[group_keys=[col(t1.a)], aggregates=[COUNT(*)]]\n") +
        "      Filter[col(COUNT(*)) > lit(1)]";

    if (!trace_contains(explored.fired_rules, "FilterThroughAggregateRule") ||
        !contains_plan_text(alternatives.plans, pushed_group_key) ||
        contains_plan_text(alternatives.plans, illegal_aggregate_output_push)) {
        std::cerr << "filter-through-aggregate did not push only grouping-key predicates\n"
                  << "sql: " << sql << "\n"
                  << "trace: ";
        for (const auto& fired : explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nalternative count: " << alternatives.plans.size() << "\n"
                  << "logical plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }

    memo.assert_invariants();
}

void assert_filter_through_aggregate_moves_group_key_or_but_pins_aggregate_or() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.a, COUNT(*) FROM t1 JOIN t2 ON t1.a = t2.a "
        "GROUP BY t1.a HAVING (t1.a = 1 OR t1.a = 2) AND (t1.a = 1 OR COUNT(*) > 1)";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
    assert(!alternatives.hit_expression_bound);
    assert(!alternatives.hit_plan_bound);

    const auto pushed_group_key_or =
        std::string("Filter[(col(t1.a) = lit(1) OR col(COUNT(*)) > lit(1))]\n") +
        "    Aggregate[group_keys=[col(t1.a)], aggregates=[COUNT(*)]]\n"
        "      Filter[(col(t1.a) = lit(1) OR col(t1.a) = lit(2))]";
    const auto illegal_aggregate_output_or_push =
        std::string("Aggregate[group_keys=[col(t1.a)], aggregates=[COUNT(*)]]\n") +
        "      Filter[(col(t1.a) = lit(1) OR col(COUNT(*)) > lit(1))]";

    if (!trace_contains(explored.fired_rules, "FilterThroughAggregateRule") ||
        !contains_plan_text(alternatives.plans, pushed_group_key_or) ||
        contains_plan_text(alternatives.plans, illegal_aggregate_output_or_push)) {
        std::cerr << "filter-through-aggregate did not move only grouping-key OR predicates\n"
                  << "sql: " << sql << "\n"
                  << "trace: ";
        for (const auto& fired : explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nalternative count: " << alternatives.plans.size() << "\n"
                  << "logical plan:\n"
                  << plan::to_string(logical) << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }

    memo.assert_invariants();
}

} // namespace

int main() {
    assert_memo_ingest_deduplicates_bound_plan_tree();
    assert_memo_exploration_adds_equivalent_expressions();
    assert_memo_extracted_plan_matches_both_engines();
    assert_cross_group_duplicate_expression_merges_groups();
    assert_aliased_self_join_scans_remain_distinct_groups();
    assert_string_literal_and_int_literal_are_distinct_memo_expressions();
    assert_having_filter_round_trips_through_memo();
    assert_filter_into_join_adds_pushdown_alternative();
    assert_filter_into_join_moves_one_side_or_but_not_mixed_side_or();
    assert_filter_through_aggregate_pushes_only_group_keys();
    assert_filter_through_aggregate_moves_group_key_or_but_pins_aggregate_or();
    return 0;
}
