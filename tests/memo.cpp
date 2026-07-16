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

    storage::Int64Column t3_c;
    for (auto value : {200, 201, 201}) {
        t3_c.append(value);
    }
    storage::Int64Column t3_d;
    for (auto value : {1, 2, 3}) {
        t3_d.append(value);
    }
    storage::ColumnarBatch t3;
    t3.add_column("c", std::move(t3_c));
    t3.add_column("d", std::move(t3_d));
    catalog.add_table("t3", std::move(t3));
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

plan::BoundPredicate membership_equality(std::string left_binding,
                                         std::string left_column,
                                         std::string right_binding,
                                         std::string right_column);

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

bool contains_plan_text(const std::vector<plan::LogicalPlan>& alternatives, const std::string& text);

void assert_outer_joins_block_unsound_join_transforms() {
    const auto catalog = make_catalog();
    const auto commute_sql = "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a";
    const auto commute_logical = sql::bind_select(sql::parse_select(commute_sql), catalog);

    optimizer::Memo commute_memo;
    const auto commute_root = commute_memo.insert(commute_logical);
    const auto commute_explored =
        optimizer::explore_memo_to_fixpoint(commute_memo, optimizer::default_memo_rules());
    assert(commute_explored.reached_fixpoint);
    const auto commute_alternatives =
        commute_memo.extract_alternatives(commute_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (trace_contains(commute_explored.fired_rules, "JoinCommuteRule") ||
        contains_plan_text(commute_alternatives.plans, "LeftJoin[col(t1.a) = col(t2.a)]\n    Scan[t2]\n    Scan[t1]")) {
        std::cerr << "LEFT join commute guard failed\n"
                  << "sql: " << commute_sql << "\n"
                  << "trace: ";
        for (const auto& fired : commute_explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nlogical plan:\n"
                  << plan::to_string(commute_logical) << "\n"
                  << "memo dump:\n"
                  << commute_memo.dump();
        std::terminate();
    }

    const auto associate_sql =
        "SELECT t1.b, t2.c, t1_copy.b FROM t1 LEFT JOIN t2 ON t1.a = t2.a "
        "LEFT JOIN t1 AS t1_copy ON t2.a = t1_copy.a";
    const auto associate_logical = sql::bind_select(sql::parse_select(associate_sql), catalog);
    optimizer::Memo associate_memo;
    const auto associate_root = associate_memo.insert(associate_logical);
    const auto associate_explored =
        optimizer::explore_memo_to_fixpoint(associate_memo, optimizer::default_memo_rules());
    assert(associate_explored.reached_fixpoint);
    const auto associate_alternatives =
        associate_memo.extract_alternatives(associate_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (trace_contains(associate_explored.fired_rules, "JoinAssociateRule") ||
        associate_alternatives.plans.size() != 1) {
        std::cerr << "LEFT join associate guard failed\n"
                  << "sql: " << associate_sql << "\n"
                  << "alternative count: " << associate_alternatives.plans.size() << "\n"
                  << "trace: ";
        for (const auto& fired : associate_explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nlogical plan:\n"
                  << plan::to_string(associate_logical) << "\n"
                  << "memo dump:\n"
                  << associate_memo.dump();
        std::terminate();
    }

    const auto filter_sql =
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t2.c IS NULL";
    const auto filter_logical = sql::bind_select(sql::parse_select(filter_sql), catalog);
    optimizer::Memo filter_memo;
    const auto filter_root = filter_memo.insert(filter_logical);
    const auto filter_explored =
        optimizer::explore_memo_to_fixpoint(filter_memo, optimizer::default_memo_rules());
    assert(filter_explored.reached_fixpoint);
    const auto filter_alternatives =
        filter_memo.extract_alternatives(filter_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (trace_contains(filter_explored.fired_rules, "FilterIntoJoinRule") ||
        contains_plan_text(filter_alternatives.plans,
                           "LeftJoin[col(t1.a) = col(t2.a) AND col(t2.c) > lit(200)]") ||
        contains_plan_text(filter_alternatives.plans, "Filter[col(t2.c) > lit(200)]\n      Scan[t2]")) {
        std::cerr << "LEFT join filter-into-join guard failed\n"
                  << "sql: " << filter_sql << "\n"
                  << "trace: ";
        for (const auto& fired : filter_explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nlogical plan:\n"
                  << plan::to_string(filter_logical) << "\n"
                  << "memo dump:\n"
                  << filter_memo.dump();
        std::terminate();
    }

    commute_memo.assert_invariants();
    associate_memo.assert_invariants();
    filter_memo.assert_invariants();
}

void assert_left_join_to_inner_unlocks_guarded_transforms() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.b, t2.c, t3.d FROM t1 JOIN t2 ON t1.a = t2.a "
        "LEFT JOIN t3 ON t2.c = t3.c WHERE t3.d > 1";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    assert(explored.reached_fixpoint);
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{256, 4096});
    const auto best = memo.extract_best(root, catalog);
    const auto best_text = plan::to_string(best);

    if (!trace_contains(explored.fired_rules, "LeftJoinToInnerRule") ||
        !trace_contains(explored.fired_rules, "JoinCommuteRule") ||
        !trace_contains(explored.fired_rules, "JoinAssociateRule") ||
        !contains_plan_text(alternatives.plans, "Filter[col(t3.d) > lit(1)]\n    Join[") ||
        best_text.find("LeftJoin[") != std::string::npos ||
        !contains_plan_text(alternatives.plans, best_text)) {
        std::cerr << "LEFT-to-INNER simplification did not unlock guarded join transforms\n"
                  << "sql: " << sql << "\n"
                  << "trace: ";
        for (const auto& fired : explored.fired_rules) {
            std::cerr << fired << " ";
        }
        std::cerr << "\nalternative count: " << alternatives.plans.size() << "\n"
                  << "best plan:\n"
                  << best_text << "\n"
                  << "memo dump:\n"
                  << memo.dump();
        std::terminate();
    }

    const auto all_right_or_sql =
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a "
        "WHERE (t2.c > 200 OR t2.a = 2)";
    const auto all_right_or = sql::bind_select(sql::parse_select(all_right_or_sql), catalog);
    optimizer::Memo all_right_or_memo;
    const auto all_right_or_root = all_right_or_memo.insert(all_right_or);
    const auto all_right_or_explored =
        optimizer::explore_memo_to_fixpoint(all_right_or_memo, optimizer::default_memo_rules());
    assert(all_right_or_explored.reached_fixpoint);
    if (!trace_contains(all_right_or_explored.fired_rules, "LeftJoinToInnerRule") ||
        plan::to_string(all_right_or_memo.extract_best(all_right_or_root, catalog)).find("LeftJoin[") !=
            std::string::npos) {
        std::cerr << "all-right null-rejecting OR did not simplify LEFT join\n"
                  << "sql: " << all_right_or_sql << "\n"
                  << "memo dump:\n"
                  << all_right_or_memo.dump();
        std::terminate();
    }

    const std::vector<std::string> negative_sqls{
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t2.c IS NULL",
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE t1.b > 10",
        "SELECT t1.b, t2.c FROM t1 LEFT JOIN t2 ON t1.a = t2.a WHERE (t2.c > 200 OR t1.b > 10)",
    };
    for (const auto& negative_sql : negative_sqls) {
        const auto negative = sql::bind_select(sql::parse_select(negative_sql), catalog);
        optimizer::Memo negative_memo;
        const auto negative_root = negative_memo.insert(negative);
        const auto negative_explored =
            optimizer::explore_memo_to_fixpoint(negative_memo, optimizer::default_memo_rules());
        assert(negative_explored.reached_fixpoint);
        const auto negative_alternatives =
            negative_memo.extract_alternatives(negative_root, optimizer::AlternativeExtractionOptions{256, 4096});
        bool alternative_lost_left_join = false;
        for (const auto& alternative : negative_alternatives.plans) {
            alternative_lost_left_join =
                alternative_lost_left_join || plan::to_string(alternative).find("LeftJoin[") == std::string::npos;
        }
        if (trace_contains(negative_explored.fired_rules, "LeftJoinToInnerRule") || alternative_lost_left_join) {
            std::cerr << "non-proven null-rejection simplified a LEFT join\n"
                      << "sql: " << negative_sql << "\n"
                      << "memo dump:\n"
                      << negative_memo.dump();
            std::terminate();
        }
    }

    memo.assert_invariants();
    all_right_or_memo.assert_invariants();
}

bool contains_plan_text(const std::vector<plan::LogicalPlan>& alternatives, const std::string& text) {
    for (const auto& alternative : alternatives) {
        if (plan::to_string(alternative).find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void assert_top_level_subquery_decorrelation_rules_and_guards() {
    const auto catalog = make_catalog();
    struct PositiveCase {
        std::string sql;
        std::string rule;
        std::string join_text;
    };
    const std::vector<PositiveCase> positive_cases{
        {"SELECT a FROM t WHERE EXISTS (SELECT a FROM t1)", "ExistsToSemiJoinRule", "SemiJoin[]"},
        {"SELECT a FROM t WHERE NOT EXISTS (SELECT a FROM t1 WHERE a = 999)",
         "NotExistsToAntiJoinRule",
         "AntiJoin[]"},
        {"SELECT a FROM t WHERE a IN (SELECT a FROM t1)",
         "InToSemiJoinRule",
         "SemiJoin[col(t.a) = col(a)]"},
        {"SELECT a FROM t WHERE a NOT IN (SELECT a FROM t1)",
         "NotInToNullAwareAntiJoinRule",
         "NullAwareAntiJoin[candidates=[], membership=col(t.a) = col(a)]"},
        {"SELECT a FROM t WHERE a NOT IN (SELECT t.a FROM t WHERE t.a < 3)",
         "NotInToNullAwareAntiJoinRule",
         "NullAwareAntiJoin[candidates=[], membership=col(t.a) = col(__not_in_value_0)]"},
    };

    for (const auto& test : positive_cases) {
        const auto logical = sql::bind_select(sql::parse_select(test.sql), catalog);
        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives =
            memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
        const auto expected = format_batch(execution::execute_interpreted(logical, catalog));
        if (!trace_contains(explored.fired_rules, test.rule) ||
            !contains_plan_text(alternatives.plans, test.join_text)) {
            std::cerr << "decorrelation rule did not produce its guarded join alternative\n"
                      << "sql: " << test.sql << "\n"
                      << "expected rule: " << test.rule << "\n"
                      << "memo dump:\n" << memo.dump();
            std::terminate();
        }
        for (const auto& alternative : alternatives.plans) {
            const auto interpreted = execution::execute_interpreted(alternative, catalog);
            const auto vectorized = execution::execute_vectorized(alternative, catalog);
            if (format_batch(interpreted) != expected || format_batch(vectorized) != expected) {
                std::cerr << "decorrelated memo alternative changed oracle results\n"
                          << "sql: " << test.sql << "\n"
                          << "alternative:\n" << plan::to_string(alternative) << "\n"
                          << "memo dump:\n" << memo.dump();
                std::terminate();
            }
        }
        memo.assert_invariants();
    }

    const std::vector<std::string> blocked_sqls{
        "SELECT a FROM t WHERE a = 1 OR EXISTS (SELECT a FROM t1)",
        "SELECT a FROM t WHERE a = 1 OR a IN (SELECT a FROM t1)",
        "SELECT a FROM t WHERE a = 1 OR a NOT IN (SELECT a FROM t1)",
        "SELECT a FROM t WHERE (SELECT MAX(a) FROM t) NOT IN (SELECT a FROM t1)",
        "SELECT a FROM t WHERE a NOT IN (SELECT a FROM t1) AND "
        "a = (SELECT MAX(a) FROM t1)",
        "SELECT a FROM t WHERE a = (SELECT MAX(a) FROM t1) "
        "GROUP BY a HAVING a NOT IN (SELECT a FROM t1)",
        "SELECT a FROM t WHERE a = (SELECT MAX(a) FROM t1)",
    };
    for (const auto& sql_text : blocked_sqls) {
        const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives =
            memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 1024});
        if (trace_contains(explored.fired_rules, "ExistsToSemiJoinRule") ||
            trace_contains(explored.fired_rules, "NotExistsToAntiJoinRule") ||
            trace_contains(explored.fired_rules, "InToSemiJoinRule") ||
            trace_contains(explored.fired_rules, "NotInToNullAwareAntiJoinRule") ||
            contains_plan_text(alternatives.plans, "SemiJoin[") ||
            contains_plan_text(alternatives.plans, "AntiJoin[") ||
            contains_plan_text(alternatives.plans, "NullAwareAntiJoin[")) {
            std::cerr << "blocked subquery form was decorrelated\n"
                      << "sql: " << sql_text << "\n"
                      << "memo dump:\n" << memo.dump();
            std::terminate();
        }
    }

    const auto residual_sql =
        "SELECT a FROM t WHERE a > 1 AND EXISTS (SELECT a FROM t1)";
    const auto residual = sql::bind_select(sql::parse_select(residual_sql), catalog);
    optimizer::Memo residual_memo;
    const auto residual_root = residual_memo.insert(residual);
    const auto residual_explored =
        optimizer::explore_memo_to_fixpoint(residual_memo, optimizer::default_memo_rules());
    const auto residual_alternatives = residual_memo.extract_alternatives(
        residual_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (!trace_contains(residual_explored.fired_rules, "ExistsToSemiJoinRule") ||
        !trace_contains(residual_explored.fired_rules, "FilterIntoJoinRule") ||
        !contains_plan_text(residual_alternatives.plans,
                            "SemiJoin[]\n    Filter[col(t.a) > lit(1)]")) {
        std::cerr << "left-only residual did not push into the preserved side of SemiJoin\n"
                  << "sql: " << residual_sql << "\n"
                  << "memo dump:\n" << residual_memo.dump();
        std::terminate();
    }
    if (trace_contains(residual_explored.fired_rules, "JoinCommuteRule") ||
        trace_contains(residual_explored.fired_rules, "JoinAssociateRule") ||
        trace_contains(residual_explored.fired_rules, "LeftJoinToInnerRule")) {
        std::cerr << "SemiJoin crossed a forbidden join transform guard\n"
                  << "memo dump:\n" << residual_memo.dump();
        std::terminate();
    }

    const auto anti_sql =
        "SELECT a FROM t WHERE a > 1 AND NOT EXISTS (SELECT a FROM t1 WHERE a = 999)";
    const auto anti = sql::bind_select(sql::parse_select(anti_sql), catalog);
    optimizer::Memo anti_memo;
    const auto anti_root = anti_memo.insert(anti);
    const auto anti_explored =
        optimizer::explore_memo_to_fixpoint(anti_memo, optimizer::default_memo_rules());
    const auto anti_alternatives =
        anti_memo.extract_alternatives(anti_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (!trace_contains(anti_explored.fired_rules, "NotExistsToAntiJoinRule") ||
        !trace_contains(anti_explored.fired_rules, "FilterIntoJoinRule") ||
        !contains_plan_text(anti_alternatives.plans,
                            "AntiJoin[]\n    Filter[col(t.a) > lit(1)]") ||
        trace_contains(anti_explored.fired_rules, "JoinCommuteRule") ||
        trace_contains(anti_explored.fired_rules, "JoinAssociateRule") ||
        trace_contains(anti_explored.fired_rules, "LeftJoinToInnerRule")) {
        std::cerr << "AntiJoin transform guards or preserved-side pushdown failed\n"
                  << "sql: " << anti_sql << "\n"
                  << "memo dump:\n" << anti_memo.dump();
        std::terminate();
    }

    const auto null_aware_sql =
        "SELECT a FROM t WHERE a > 1 AND a NOT IN (SELECT a FROM t1 WHERE a = 999)";
    const auto null_aware = sql::bind_select(sql::parse_select(null_aware_sql), catalog);
    optimizer::Memo null_aware_memo;
    const auto null_aware_root = null_aware_memo.insert(null_aware);
    const auto null_aware_explored =
        optimizer::explore_memo_to_fixpoint(null_aware_memo, optimizer::default_memo_rules());
    const auto null_aware_alternatives = null_aware_memo.extract_alternatives(
        null_aware_root, optimizer::AlternativeExtractionOptions{128, 1024});
    if (!trace_contains(null_aware_explored.fired_rules, "NotInToNullAwareAntiJoinRule") ||
        !trace_contains(null_aware_explored.fired_rules, "FilterIntoJoinRule") ||
        !contains_plan_text(
            null_aware_alternatives.plans,
            "NullAwareAntiJoin[candidates=[], membership=col(t.a) = col(a)]\n"
            "    Filter[col(t.a) > lit(1)]") ||
        trace_contains(null_aware_explored.fired_rules, "JoinCommuteRule") ||
        trace_contains(null_aware_explored.fired_rules, "JoinAssociateRule") ||
        trace_contains(null_aware_explored.fired_rules, "LeftJoinToInnerRule")) {
        std::cerr << "NullAwareAnti transform guards or preserved-side pushdown failed\n"
                  << "sql: " << null_aware_sql << "\n"
                  << "memo dump:\n" << null_aware_memo.dump();
        std::terminate();
    }

    const auto right_only = plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{"t2", "c", 0},
        sql::ComparisonOp::Greater,
        sql::IntLiteral{200, 0},
        0,
    });
    const auto mixed = plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{"t1", "a", 0},
        sql::ComparisonOp::Equal,
        plan::BoundColumnRef{"t2", "a", 0},
        0,
    });
    const auto empty_reference = plan::BoundPredicate::null_check_expr(
        sql::PredicateKind::IsNull, plan::BoundScalarExpr{sql::IntLiteral{1, 0}}, 0);
    for (const auto kind :
         {plan::JoinKind::Semi, plan::JoinKind::Anti, plan::JoinKind::NullAwareAnti}) {
        for (const auto& pinned : {right_only, mixed, empty_reference}) {
            auto child = kind == plan::JoinKind::NullAwareAnti
                             ? plan::LogicalPlan::null_aware_anti(
                                   membership_equality("t1", "a", "t2", "a"),
                                   plan::LogicalPlan::scan("t1"),
                                   plan::LogicalPlan::scan("t2"))
                             : plan::LogicalPlan::join({},
                                                       plan::LogicalPlan::scan("t1"),
                                                       plan::LogicalPlan::scan("t2"),
                                                       kind);
            const auto logical = plan::LogicalPlan::filter(
                {pinned}, std::move(child));
            optimizer::Memo guard_memo;
            const auto guard_root = guard_memo.insert(logical);
            const auto guard_explored =
                optimizer::explore_memo_to_fixpoint(guard_memo, optimizer::default_memo_rules());
            const auto guard_alternatives = guard_memo.extract_alternatives(
                guard_root, optimizer::AlternativeExtractionOptions{128, 1024});
            if (trace_contains(guard_explored.fired_rules, "FilterIntoJoinRule") ||
                trace_contains(guard_explored.fired_rules, "JoinCommuteRule") ||
                trace_contains(guard_explored.fired_rules, "JoinAssociateRule") ||
                trace_contains(guard_explored.fired_rules, "LeftJoinToInnerRule") ||
                guard_alternatives.plans.size() != 1) {
                std::cerr << "non-left-only predicate crossed a preserved-left transform guard\n"
                          << "plan:\n" << plan::to_string(logical) << "\n"
                          << "memo dump:\n" << guard_memo.dump();
                std::terminate();
            }
        }
    }
}

void assert_correlated_subquery_decorrelation_rules_and_guards() {
    const auto catalog = make_catalog();
    struct PositiveCase {
        std::string sql;
        std::string rule;
        std::string join_text;
    };
    const std::vector<PositiveCase> positive_cases{
        {"SELECT a FROM t WHERE EXISTS (SELECT t1.b FROM t1 WHERE t1.a = t.a)",
         "CorrelatedExistsToSemiJoinRule", "SemiJoin["},
        {"SELECT a FROM t WHERE NOT EXISTS (SELECT t1.b FROM t1 WHERE t1.a = t.a)",
         "CorrelatedNotExistsToAntiJoinRule", "AntiJoin["},
        {"SELECT b FROM t WHERE b IN (SELECT t1.b FROM t1 WHERE t1.a = t.a)",
         "CorrelatedInToSemiJoinRule", "SemiJoin["},
        {"SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a = t.a)",
         "CorrelatedNotInToNullAwareAntiJoinRule", "NullAwareAntiJoin["},
        {"SELECT o.b FROM t AS o JOIN t1 AS shadow ON o.a = shadow.a "
         "WHERE o.b NOT IN (SELECT shadow.b FROM t1 AS shadow WHERE shadow.a = o.a)",
         "CorrelatedNotInToNullAwareAntiJoinRule",
         "membership=col(o.b) = col(__not_in_value_0)"},
        {"SELECT a FROM t WHERE EXISTS (SELECT t1.b FROM t1 WHERE t1.a = t.a AND t1.b > 10)",
         "CorrelatedExistsToSemiJoinRule", "Filter[col(t1.b) > lit(10)]"},
    };

    for (const auto& test : positive_cases) {
        const auto logical = sql::bind_select(sql::parse_select(test.sql), catalog);
        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 2048});
        const auto expected = format_batch(execution::execute_interpreted(logical, catalog));
        if (!trace_contains(explored.fired_rules, test.rule) ||
            !contains_plan_text(alternatives.plans, test.join_text)) {
            std::cerr << "correlated decorrelation rule did not produce its join alternative\n"
                      << "sql: " << test.sql << "\nexpected rule: " << test.rule
                      << "\nmemo dump:\n" << memo.dump();
            std::terminate();
        }
        bool checked_vectorized = false;
        for (const auto& alternative : alternatives.plans) {
            const auto text = plan::to_string(alternative);
            if (text.find(test.join_text) == std::string::npos || text.find("correlation=[") != std::string::npos) {
                continue;
            }
            const auto interpreted = execution::execute_interpreted(alternative, catalog);
            const auto vectorized = execution::execute_vectorized(alternative, catalog);
            if (format_batch(interpreted) != expected || format_batch(vectorized) != expected) {
                std::cerr << "correlated decorrelated alternative changed results\n"
                          << "sql: " << test.sql << "\nalternative:\n" << text << "\n";
                std::terminate();
            }
            checked_vectorized = true;
        }
        assert(checked_vectorized);
        memo.assert_invariants();
    }

    const std::vector<std::string> blocked_sqls{
        "SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 WHERE t1.a > t.a)",
        "SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 WHERE t1.a = t.a OR t1.b = 999)",
        "SELECT a FROM t WHERE EXISTS (SELECT t.a FROM t1)",
        "SELECT a FROM t WHERE a = (SELECT MAX(t.a) FROM t1)",
        "SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 GROUP BY t1.a HAVING t1.a = t.a)",
        "SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 JOIN t2 ON t1.a = t2.a AND t2.a = t.a)",
        "SELECT a FROM t WHERE EXISTS (SELECT t1.a FROM t1 ORDER BY t.a LIMIT 1)",
        "SELECT a FROM t WHERE EXISTS (SELECT MAX(t1.b) FROM t1 WHERE t1.a = t.a)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a > t.a)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a = t.a OR t1.b = 999)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t.a FROM t1)",
        "SELECT b FROM t WHERE b NOT IN (SELECT MAX(t.a) FROM t1)",
        "SELECT b FROM t WHERE b NOT IN (SELECT MAX(t1.b) FROM t1 GROUP BY t1.a HAVING t1.a = t.a)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 JOIN t2 ON t1.a = t2.a AND t2.a = t.a)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a = t.a ORDER BY t1.b LIMIT 1)",
        "SELECT b FROM t WHERE b NOT IN (SELECT t1.b FROM t1 WHERE t1.a = t.a AND "
        "t1.b = (SELECT MAX(t2.a) FROM t2))",
    };
    for (const auto& sql_text : blocked_sqls) {
        const auto logical = sql::bind_select(sql::parse_select(sql_text), catalog);
        optimizer::Memo memo;
        const auto root = memo.insert(logical);
        const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
        const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{128, 2048});
        const auto fired = trace_contains(explored.fired_rules, "CorrelatedExistsToSemiJoinRule") ||
                           trace_contains(explored.fired_rules, "CorrelatedNotExistsToAntiJoinRule") ||
                           trace_contains(explored.fired_rules, "CorrelatedInToSemiJoinRule") ||
                           trace_contains(explored.fired_rules,
                                          "CorrelatedNotInToNullAwareAntiJoinRule") ||
                           trace_contains(explored.fired_rules, "ExistsToSemiJoinRule") ||
                           trace_contains(explored.fired_rules, "NotExistsToAntiJoinRule") ||
                           trace_contains(explored.fired_rules, "InToSemiJoinRule") ||
                           trace_contains(explored.fired_rules, "NotInToNullAwareAntiJoinRule");
        if (fired || contains_plan_text(alternatives.plans, "SemiJoin[") ||
            contains_plan_text(alternatives.plans, "AntiJoin[") ||
            contains_plan_text(alternatives.plans, "NullAwareAntiJoin[")) {
            std::cerr << "blocked correlated shape was decorrelated\n"
                      << "sql: " << sql_text << "\nmemo dump:\n" << memo.dump();
            std::terminate();
        }
    }
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

void assert_subquery_subplans_are_structural_but_opaque_memo_fields() {
    const auto catalog = make_catalog();
    const auto first = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a IN (SELECT a FROM t1)"), catalog);
    const auto identical = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a IN ( SELECT a FROM t1 )"), catalog);
    const auto different = sql::bind_select(
        sql::parse_select("SELECT a FROM t WHERE a IN (SELECT a FROM t2)"), catalog);

    optimizer::Memo memo;
    const auto first_root = memo.insert(first);
    const auto identical_root = memo.insert(identical);
    assert(first_root == identical_root);
    assert(memo.group_count() == 3);

    const auto different_root = memo.insert(different);
    assert(different_root != first_root);
    assert(memo.group_count() == 5);

    const auto printed = plan::to_string(memo.extract(first_root));
    assert(printed.find("  Filter[col(t.a) IN subquery(IN subquery at position") != std::string::npos);
    assert(printed.find("\n    Subquery[IN subquery at position") != std::string::npos);
    assert(printed.find("\n      Project[a=col(t1.a)]\n        Scan[t1]") != std::string::npos);
    memo.assert_invariants();
}

void assert_semi_anti_join_kinds_are_distinct_memo_identity() {
    const auto semi = plan::LogicalPlan::join(
        {}, plan::LogicalPlan::scan("t1"), plan::LogicalPlan::scan("t2"), plan::JoinKind::Semi);
    const auto anti = plan::LogicalPlan::join(
        {}, plan::LogicalPlan::scan("t1"), plan::LogicalPlan::scan("t2"), plan::JoinKind::Anti);
    const auto null_aware = plan::LogicalPlan::null_aware_anti(
        membership_equality("t1", "a", "t2", "a"),
        plan::LogicalPlan::scan("t1"),
        plan::LogicalPlan::scan("t2"));

    optimizer::Memo memo;
    const auto semi_root = memo.insert(semi);
    const auto anti_root = memo.insert(anti);
    const auto null_aware_root = memo.insert(null_aware);
    assert(semi_root != anti_root);
    assert(semi_root != null_aware_root);
    assert(anti_root != null_aware_root);
    assert(memo.group_count() == 5);
    assert(memo.extract(semi_root).join_kind == plan::JoinKind::Semi);
    assert(memo.extract(anti_root).join_kind == plan::JoinKind::Anti);
    assert(memo.extract(null_aware_root).join_kind == plan::JoinKind::NullAwareAnti);
    const auto dump = memo.dump();
    assert(dump.find("SemiJoin[]") != std::string::npos);
    assert(dump.find("AntiJoin[]") != std::string::npos);
    assert(dump.find("NullAwareAntiJoin[") != std::string::npos);
    memo.assert_invariants();
}

plan::BoundPredicate membership_equality(std::string left_binding,
                                         std::string left_column,
                                         std::string right_binding,
                                         std::string right_column) {
    return plan::BoundPredicate::comparison_expr(plan::BoundComparisonExpr{
        plan::BoundColumnRef{std::move(left_binding), std::move(left_column), 0},
        sql::ComparisonOp::Equal,
        plan::BoundColumnRef{std::move(right_binding), std::move(right_column), 0},
        0,
    });
}

void assert_null_aware_membership_is_structural_memo_identity() {
    const auto first = plan::LogicalPlan::null_aware_anti(
        membership_equality("t1", "a", "t2", "a"),
        plan::LogicalPlan::scan("t1"),
        plan::LogicalPlan::scan("t2"));
    const auto second = plan::LogicalPlan::null_aware_anti(
        membership_equality("t1", "b", "t2", "c"),
        plan::LogicalPlan::scan("t1"),
        plan::LogicalPlan::scan("t2"));

    optimizer::Memo memo;
    const auto first_root = memo.insert(first);
    const auto second_root = memo.insert(second);
    assert(first_root != second_root);
    assert(memo.group_count() == 4);
    const auto first_extracted = memo.extract(first_root);
    const auto second_extracted = memo.extract(second_root);
    assert(first_extracted.join_kind == plan::JoinKind::NullAwareAnti);
    assert(first_extracted.null_aware_predicate.has_value());
    assert(second_extracted.null_aware_predicate.has_value());
    assert(plan::to_string(first_extracted) == plan::to_string(first));
    assert(plan::to_string(second_extracted) == plan::to_string(second));
    const auto alternatives = memo.extract_alternatives(first_root);
    assert(alternatives.plans.size() == 1);
    assert(plan::to_string(alternatives.plans.front()) == plan::to_string(first));
    const auto dump = memo.dump();
    assert(dump.find("NullAwareAntiJoin[candidates=[], membership=col(t1.a) = col(t2.a)]") !=
           std::string::npos);
    assert(dump.find("NullAwareAntiJoin[candidates=[], membership=col(t1.b) = col(t2.c)]") !=
           std::string::npos);
    memo.assert_invariants();
}

void assert_subquery_conjunct_moves_using_only_outer_references() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.b, t2.c FROM t1 JOIN t2 ON t1.a = t2.a "
        "WHERE t1.b IN (SELECT b FROM t)";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto alternatives = memo.extract_alternatives(root, optimizer::AlternativeExtractionOptions{64, 512});

    if (!trace_contains(explored.fired_rules, "FilterIntoJoinRule") ||
        !contains_plan_text(alternatives.plans,
                            "Join[col(t1.a) = col(t2.a)]\n"
                            "    Filter[col(t1.b) IN subquery(")) {
        std::cerr << "uncorrelated subquery conjunct did not move using its outer t1.b reference\n"
                  << "sql: " << sql << "\n"
                  << "plan:\n" << plan::to_string(logical) << "\n"
                  << "memo dump:\n" << memo.dump();
        std::terminate();
    }
    memo.assert_invariants();
}

void assert_explain_indents_opaque_subplans_under_owner() {
    const auto catalog = make_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("EXPLAIN SELECT a FROM t WHERE a IN (SELECT a FROM t1)"), catalog);
    const auto explained = execution::execute_interpreted(logical, catalog);

    std::string report;
    const auto& lines = explained.string_column("plan");
    for (std::size_t row = 0; row < explained.row_count(); ++row) {
        report += lines.at(row) + "\n";
    }
    assert(report.find("    Filter[col(t.a) IN subquery(") != std::string::npos);
    assert(report.find("      Subquery[IN subquery at position") != std::string::npos);
    assert(report.find("        Project[a=col(t1.a)]") != std::string::npos);
    assert(report.find("          Scan[t1]") != std::string::npos);
}

void assert_window_round_trips_and_is_a_filter_pushdown_barrier() {
    const auto catalog = make_catalog();
    const auto sql =
        "SELECT t1.b, t2.c, RANK() OVER (PARTITION BY t1.b ORDER BY t2.c) AS r "
        "FROM t1 JOIN t2 ON t1.a = t2.a";
    const auto logical = sql::bind_select(sql::parse_select(sql), catalog);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    assert(memo.insert(logical) == root);
    const auto different = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, t2.c, RANK() OVER (PARTITION BY t1.b ORDER BY t2.c DESC) AS r "
            "FROM t1 JOIN t2 ON t1.a = t2.a"),
        catalog);
    assert(memo.insert(different) != root);
    auto different_frame = memo.extract(root);
    assert(different_frame.input != nullptr && different_frame.input->kind == plan::LogicalKind::Window);
    different_frame.input->window_expressions.front().frame = sql::WindowFrame::RowsCumulative;
    assert(memo.insert(different_frame) != root);
    const auto explored = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto extracted = memo.extract(root);
    assert(plan::to_string(extracted) == plan::to_string(logical));
    const auto dump = memo.dump();
    assert(dump.find("Window[RANK() OVER (PARTITION BY t1.b ORDER BY t2.c ASC)]") != std::string::npos);
    assert(trace_contains(explored.fired_rules, "JoinCommuteRule"));

    const auto window = *logical.input;
    const auto rank_ref = plan::BoundScalarExpr{
        plan::BoundColumnRef{"", "RANK() OVER (PARTITION BY t1.b ORDER BY t2.c ASC)", 0},
        catalog::ColumnType::Int64};
    const auto one = plan::BoundScalarExpr{sql::IntLiteral{1, 0}};
    const auto filtered = plan::LogicalPlan::filter(
        {plan::BoundPredicate::comparison_expr(
            plan::BoundComparisonExpr{rank_ref, sql::ComparisonOp::Greater, one, 0})},
        window);

    optimizer::Memo barrier_memo;
    const auto barrier_root = barrier_memo.insert(filtered);
    const auto barrier_trace =
        optimizer::explore_memo_to_fixpoint(barrier_memo, optimizer::default_memo_rules());
    assert(barrier_memo.group(barrier_root).expressions.size() == 1);
    assert(barrier_memo.group(barrier_root).expressions.front().expression.kind ==
           optimizer::MemoExpressionKind::Filter);
    assert(!trace_contains(barrier_trace.fired_rules, "FilterIntoJoinRule"));
    assert(trace_contains(barrier_trace.fired_rules, "JoinCommuteRule"));
    barrier_memo.assert_invariants();
}

void assert_window_explain_contains_costed_node() {
    const auto catalog = make_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select("EXPLAIN SELECT a, ROW_NUMBER() OVER (ORDER BY b) AS rn FROM t"), catalog);
    const auto explained = execution::execute_interpreted(logical, catalog);
    std::string report;
    const auto& lines = explained.string_column("plan");
    for (std::size_t row = 0; row < explained.row_count(); ++row) {
        report += lines.at(row) + "\n";
    }
    assert(report.find("Window[ROW_NUMBER() OVER (ORDER BY b ASC)]") != std::string::npos);
    assert(report.find("Window[ROW_NUMBER() OVER (ORDER BY b ASC)] rows=") != std::string::npos);
}

void assert_order_sensitive_windows_pin_order_changing_join_transforms() {
    const auto catalog = make_catalog();
    const auto logical = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, t2.c, ROW_NUMBER() OVER () AS rn "
            "FROM t1 JOIN t2 ON t1.a = t2.a"),
        catalog);
    assert(logical.input != nullptr && logical.input->kind == plan::LogicalKind::Window);
    assert(logical.input->input != nullptr && logical.input->input->kind == plan::LogicalKind::Join);
    assert(logical.input->input->order_permission == plan::OrderPermission::Deterministic);

    optimizer::Memo memo;
    const auto root = memo.insert(logical);
    const auto trace = optimizer::explore_memo_to_fixpoint(memo, optimizer::default_memo_rules());
    const auto alternatives = memo.extract_alternatives(root);
    assert(!trace_contains(trace.fired_rules, "JoinCommuteRule"));
    assert(!trace_contains(trace.fired_rules, "JoinAssociateRule"));
    assert(alternatives.plans.size() == 1);
    memo.assert_invariants();

    const auto sum_logical = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, t2.c, SUM(t1.b) OVER () AS total "
            "FROM t1 JOIN t2 ON t1.a = t2.a"),
        catalog);
    assert(sum_logical.input != nullptr && sum_logical.input->kind == plan::LogicalKind::Window);
    assert(sum_logical.input->input != nullptr && sum_logical.input->input->kind == plan::LogicalKind::Join);
    assert(sum_logical.input->input->order_permission == plan::OrderPermission::Deterministic);

    optimizer::Memo sum_memo;
    const auto sum_root = sum_memo.insert(sum_logical);
    const auto sum_trace = optimizer::explore_memo_to_fixpoint(sum_memo, optimizer::default_memo_rules());
    assert(!trace_contains(sum_trace.fired_rules, "JoinCommuteRule"));
    assert(sum_memo.extract_alternatives(sum_root).plans.size() == 1);
    sum_memo.assert_invariants();

    const auto rows_logical = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, t2.c, t3.d, "
            "COUNT(*) OVER (ORDER BY t2.c ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_n "
            "FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c"),
        catalog);
    assert(rows_logical.input != nullptr && rows_logical.input->kind == plan::LogicalKind::Window);
    assert(rows_logical.input->input != nullptr && rows_logical.input->input->kind == plan::LogicalKind::Join);
    assert(rows_logical.input->input->order_permission == plan::OrderPermission::Deterministic);

    optimizer::Memo rows_memo;
    const auto rows_root = rows_memo.insert(rows_logical);
    const auto rows_trace =
        optimizer::explore_memo_to_fixpoint(rows_memo, optimizer::default_memo_rules());
    assert(!trace_contains(rows_trace.fired_rules, "JoinCommuteRule"));
    assert(!trace_contains(rows_trace.fired_rules, "JoinAssociateRule"));
    const auto rows_alternatives = rows_memo.extract_alternatives(rows_root);
    assert(rows_alternatives.plans.size() == 1);
    const auto rows_oracle = execution::execute_interpreted(rows_alternatives.plans.front(), catalog);
    const auto rows_vectorized = execution::execute_vectorized(rows_alternatives.plans.front(), catalog);
    assert(format_batch(rows_oracle) == format_batch(rows_vectorized));
    rows_memo.assert_invariants();

    const auto range_logical = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, t2.c, t3.d, COUNT(*) OVER (ORDER BY t2.c) AS running_n "
            "FROM t1 JOIN t2 ON t1.a = t2.a JOIN t3 ON t2.c = t3.c"),
        catalog);
    assert(range_logical.input != nullptr && range_logical.input->kind == plan::LogicalKind::Window);
    assert(range_logical.input->input != nullptr && range_logical.input->input->kind == plan::LogicalKind::Join);
    assert(range_logical.input->input->order_permission == plan::OrderPermission::Arbitrary);

    optimizer::Memo range_memo;
    const auto range_root = range_memo.insert(range_logical);
    const auto range_trace =
        optimizer::explore_memo_to_fixpoint(range_memo, optimizer::default_memo_rules());
    assert(trace_contains(range_trace.fired_rules, "JoinCommuteRule"));
    assert(trace_contains(range_trace.fired_rules, "JoinAssociateRule"));
    const auto range_alternatives = range_memo.extract_alternatives(range_root);
    assert(range_alternatives.plans.size() > 1);
    for (const auto& alternative : range_alternatives.plans) {
        const auto oracle = execution::execute_interpreted(alternative, catalog);
        const auto vectorized = execution::execute_vectorized(alternative, catalog);
        assert(format_batch(oracle) == format_batch(vectorized));
    }
    range_memo.assert_invariants();

    const auto grouped_sum = sql::bind_select(
        sql::parse_select(
            "SELECT t1.b, SUM(t2.c), SUM(SUM(t2.c)) OVER (ORDER BY t1.b) AS running_sum "
            "FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.b"),
        catalog);
    assert(grouped_sum.input != nullptr && grouped_sum.input->kind == plan::LogicalKind::Window);
    assert(grouped_sum.input->input != nullptr && grouped_sum.input->input->kind == plan::LogicalKind::Aggregate);
    assert(grouped_sum.input->input->input != nullptr &&
           grouped_sum.input->input->input->kind == plan::LogicalKind::Join);
    assert(grouped_sum.input->input->input->order_permission == plan::OrderPermission::Deterministic);

    optimizer::Memo grouped_sum_memo;
    const auto grouped_sum_root = grouped_sum_memo.insert(grouped_sum);
    const auto grouped_sum_trace =
        optimizer::explore_memo_to_fixpoint(grouped_sum_memo, optimizer::default_memo_rules());
    assert(!trace_contains(grouped_sum_trace.fired_rules, "JoinCommuteRule"));
    const auto grouped_sum_alternatives = grouped_sum_memo.extract_alternatives(grouped_sum_root);
    assert(grouped_sum_alternatives.plans.size() == 1);
    const auto grouped_sum_oracle =
        execution::execute_interpreted(grouped_sum_alternatives.plans.front(), catalog);
    const auto grouped_sum_vectorized =
        execution::execute_vectorized(grouped_sum_alternatives.plans.front(), catalog);
    assert(format_batch(grouped_sum_oracle) == format_batch(grouped_sum_vectorized));
    grouped_sum_memo.assert_invariants();
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
    assert_outer_joins_block_unsound_join_transforms();
    assert_left_join_to_inner_unlocks_guarded_transforms();
    assert_filter_through_aggregate_pushes_only_group_keys();
    assert_filter_through_aggregate_moves_group_key_or_but_pins_aggregate_or();
    assert_semi_anti_join_kinds_are_distinct_memo_identity();
    assert_null_aware_membership_is_structural_memo_identity();
    assert_top_level_subquery_decorrelation_rules_and_guards();
    assert_correlated_subquery_decorrelation_rules_and_guards();
    assert_subquery_subplans_are_structural_but_opaque_memo_fields();
    assert_subquery_conjunct_moves_using_only_outer_references();
    assert_explain_indents_opaque_subplans_under_owner();
    assert_window_round_trips_and_is_a_filter_pushdown_barrier();
    assert_window_explain_contains_costed_node();
    assert_order_sensitive_windows_pin_order_changing_join_transforms();
    return 0;
}
