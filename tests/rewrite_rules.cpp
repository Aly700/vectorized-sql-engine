#include "execution/interpreter.hpp"
#include "execution/vectorized.hpp"
#include "optimizer/rewrite.hpp"
#include "plan/logical_plan.hpp"
#include "sql/binder.hpp"

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
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

bool same_batch(const storage::ColumnarBatch& left, const storage::ColumnarBatch& right) {
    return format_batch(left) == format_batch(right);
}

bool trace_contains(const optimizer::RewriteTrace& trace, const std::string& rule_name) {
    for (const auto& fired : trace.fired_rules) {
        if (fired == rule_name) {
            return true;
        }
    }
    return false;
}

sql::ComparisonExpr comparison(sql::ScalarExpr left, sql::ComparisonOp op, sql::ScalarExpr right) {
    return sql::ComparisonExpr{std::move(left), op, std::move(right), 0};
}

sql::ScalarExpr column(std::string name) {
    return sql::ColumnRef{std::move(name), 0};
}

sql::ScalarExpr literal(std::int64_t value) {
    return sql::IntLiteral{value, 0};
}

optimizer::RewriteResult rewrite_sql(
    const std::string& sql,
    const execution::Catalog& catalog,
    const std::vector<std::reference_wrapper<const optimizer::Rule>>& rules) {
    const auto parsed = sql::parse_select(sql);
    const auto logical = sql::bind_select(parsed, catalog);
    return optimizer::rewrite_to_fixpoint(logical, rules);
}

void assert_rewrite_equivalent(const std::string& sql) {
    const auto catalog = make_catalog();
    const auto parsed = sql::parse_select(sql);
    const auto logical = sql::bind_select(parsed, catalog);
    const auto rewritten = optimizer::rewrite_to_fixpoint(logical, optimizer::default_rules());

    const auto unrewritten_oracle = execution::execute_interpreted(logical, catalog);
    const auto rewritten_oracle = execution::execute_interpreted(rewritten.plan, catalog);
    const auto rewritten_vectorized = execution::execute_vectorized(rewritten.plan, catalog);
    if (same_batch(unrewritten_oracle, rewritten_oracle) && same_batch(rewritten_oracle, rewritten_vectorized)) {
        return;
    }

    std::cerr << "rewrite equivalence failed\n"
              << "sql: " << sql << "\n"
              << "before plan:\n"
              << plan::to_string(logical) << "\n"
              << "after plan:\n"
              << plan::to_string(rewritten.plan) << "\n"
              << "unrewritten oracle: " << format_batch(unrewritten_oracle) << "\n"
              << "rewritten oracle:   " << format_batch(rewritten_oracle) << "\n"
              << "rewritten vector:   " << format_batch(rewritten_vectorized) << "\n";
    std::terminate();
}

void assert_constant_fold_rule_fires() {
    const auto catalog = make_catalog();
    const optimizer::ConstantFoldComparisonRule fold;
    const std::vector<std::reference_wrapper<const optimizer::Rule>> rules{std::cref(fold)};
    const auto rewritten = rewrite_sql("SELECT a FROM t WHERE 2 > 1 AND a = 2", catalog, rules);

    assert(trace_contains(rewritten.trace, "ConstantFoldComparisonRule"));
    assert(plan::to_string(rewritten.plan).find("lit(1) = lit(1)") != std::string::npos);
    assert_rewrite_equivalent("SELECT a FROM t WHERE 2 > 1 AND a = 2");
}

void assert_drop_always_true_rule_fires() {
    const auto catalog = make_catalog();
    const optimizer::ConstantFoldComparisonRule fold;
    const optimizer::DropAlwaysTrueFilterRule drop_true;
    const std::vector<std::reference_wrapper<const optimizer::Rule>> rules{std::cref(fold), std::cref(drop_true)};
    const auto rewritten = rewrite_sql("SELECT a FROM t WHERE 2 > 1 AND a = 2", catalog, rules);

    assert(trace_contains(rewritten.trace, "DropAlwaysTrueFilterRule"));
    const auto printed = plan::to_string(rewritten.plan);
    assert(printed.find("lit(1) = lit(1)") == std::string::npos);
    assert(printed.find("col(a) = lit(2)") != std::string::npos);
    assert_rewrite_equivalent("SELECT a FROM t WHERE 2 > 1 AND a = 2");
}

void assert_always_false_rule_fires() {
    const auto catalog = make_catalog();
    const optimizer::ConstantFoldComparisonRule fold;
    const optimizer::AlwaysFalseFilterRule always_false;
    const std::vector<std::reference_wrapper<const optimizer::Rule>> rules{std::cref(fold), std::cref(always_false)};
    const auto rewritten = rewrite_sql("SELECT a FROM t WHERE a = 2 AND 2 < 1", catalog, rules);

    assert(trace_contains(rewritten.trace, "AlwaysFalseFilterRule"));
    const auto printed = plan::to_string(rewritten.plan);
    assert(printed.find("col(a) = lit(2)") == std::string::npos);
    assert(printed.find("lit(1) = lit(0)") != std::string::npos);
    assert_rewrite_equivalent("SELECT a FROM t WHERE a = 2 AND 2 < 1");
}

void assert_merge_adjacent_filters_rule_fires() {
    const auto catalog = make_catalog();
    const auto inner = comparison(column("a"), sql::ComparisonOp::GreaterEqual, literal(2));
    const auto outer = comparison(column("b"), sql::ComparisonOp::Less, literal(40));
    auto logical = plan::LogicalPlan::project(
        {plan::Projection{"a", column("a")}},
        plan::LogicalPlan::filter({outer}, plan::LogicalPlan::filter({inner}, plan::LogicalPlan::scan("t"))));

    const optimizer::MergeAdjacentFiltersRule merge;
    const std::vector<std::reference_wrapper<const optimizer::Rule>> rules{std::cref(merge)};
    const auto rewritten = optimizer::rewrite_to_fixpoint(logical, rules);

    assert(trace_contains(rewritten.trace, "MergeAdjacentFiltersRule"));
    const auto printed = plan::to_string(rewritten.plan);
    assert(printed.find("Filter[col(a) >= lit(2) AND col(b) < lit(40)]") != std::string::npos);

    const auto unrewritten_oracle = execution::execute_interpreted(logical, catalog);
    const auto rewritten_oracle = execution::execute_interpreted(rewritten.plan, catalog);
    const auto rewritten_vectorized = execution::execute_vectorized(rewritten.plan, catalog);
    assert(same_batch(unrewritten_oracle, rewritten_oracle));
    assert(same_batch(rewritten_oracle, rewritten_vectorized));
}

void assert_all_true_filter_is_removed() {
    assert_rewrite_equivalent("SELECT a FROM t WHERE 2 > 1");

    const auto catalog = make_catalog();
    const auto rewritten = rewrite_sql("SELECT a FROM t WHERE 2 > 1", catalog, optimizer::default_rules());
    assert(trace_contains(rewritten.trace, "DropAlwaysTrueFilterRule"));
    assert(plan::to_string(rewritten.plan).find("Filter") == std::string::npos);
}

} // namespace

int main() {
    assert_constant_fold_rule_fires();
    assert_drop_always_true_rule_fires();
    assert_always_false_rule_fires();
    assert_merge_adjacent_filters_rule_fires();
    assert_all_true_filter_is_removed();
    return 0;
}
