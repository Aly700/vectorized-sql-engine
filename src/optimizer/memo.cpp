#include "optimizer/memo.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace optimizer {
namespace {

void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

void hash_string(std::size_t& seed, const std::string& value) {
    hash_combine(seed, std::hash<std::string>{}(value));
}

void hash_logical_plan(std::size_t& seed, const plan::LogicalPlan& logical);
bool logical_plan_equal(const plan::LogicalPlan& left, const plan::LogicalPlan& right);

void hash_scalar(std::size_t& seed, const plan::BoundScalarExpr& expression) {
    // Embedded subplans are opaque structural fields until a proof-bearing
    // decorrelation rule explicitly lifts one into a SEMI/ANTI right child.
    // Hash their immutable trees recursively so residual forms retain identity.
    hash_combine(seed, static_cast<std::size_t>(expression.type));
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        hash_combine(seed, 1);
        hash_string(seed, column->binding);
        hash_string(seed, column->column);
        hash_combine(seed, static_cast<std::size_t>(column->type));
        return;
    }

    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        hash_combine(seed, 2);
        hash_combine(seed, std::hash<std::int64_t>{}(literal->value));
        return;
    }

    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        hash_combine(seed, 3);
        hash_string(seed, literal->value);
        return;
    }
    if (std::holds_alternative<sql::NullLiteral>(expression.value)) {
        hash_combine(seed, 4);
        return;
    }
    const auto& subquery = std::get<plan::BoundScalarSubquery>(expression.value);
    if (subquery.plan == nullptr) {
        throw std::logic_error("bound scalar subquery is missing its logical plan");
    }
    hash_combine(seed, 5);
    hash_logical_plan(seed, *subquery.plan);
}

void hash_comparison(std::size_t& seed, const plan::BoundComparisonExpr& comparison) {
    hash_scalar(seed, comparison.left);
    hash_combine(seed, static_cast<std::size_t>(comparison.op));
    hash_scalar(seed, comparison.right);
}

void hash_predicate(std::size_t& seed, const plan::BoundPredicate& predicate) {
    hash_combine(seed, static_cast<std::size_t>(predicate.kind));
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        hash_comparison(seed, predicate.comparison);
        return;
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        hash_scalar(seed, predicate.null_check);
        return;
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        hash_scalar(seed, predicate.in_value);
        if (predicate.subquery == nullptr) {
            throw std::logic_error("bound IN predicate is missing its subquery");
        }
        hash_logical_plan(seed, *predicate.subquery);
        return;
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        if (predicate.subquery == nullptr) {
            throw std::logic_error("bound EXISTS predicate is missing its subquery");
        }
        hash_logical_plan(seed, *predicate.subquery);
        return;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        hash_predicate(seed, *predicate.left);
        hash_predicate(seed, *predicate.right);
        return;
    }
    throw std::logic_error("unreachable predicate kind");
}

void hash_projection(std::size_t& seed, const plan::Projection& projection) {
    hash_string(seed, projection.output_name);
    hash_scalar(seed, projection.expression);
}

void hash_column(std::size_t& seed, const plan::BoundColumnRef& column) {
    hash_string(seed, column.binding);
    hash_string(seed, column.column);
    hash_combine(seed, static_cast<std::size_t>(column.type));
}

void hash_aggregate_expression(std::size_t& seed, const plan::AggregateExpression& aggregate) {
    hash_string(seed, aggregate.output_name);
    hash_combine(seed, static_cast<std::size_t>(aggregate.function));
    hash_combine(seed, static_cast<std::size_t>(aggregate.type));
    hash_combine(seed, aggregate.argument.has_value() ? 1 : 0);
    if (aggregate.argument.has_value()) {
        hash_column(seed, *aggregate.argument);
    }
}

void hash_sort_key(std::size_t& seed, const plan::SortKey& key) {
    hash_column(seed, key.column);
    hash_combine(seed, static_cast<std::size_t>(key.direction));
}

void hash_logical_child(std::size_t& seed, const std::shared_ptr<plan::LogicalPlan>& child) {
    hash_combine(seed, child == nullptr ? 0 : 1);
    if (child != nullptr) {
        hash_logical_plan(seed, *child);
    }
}

void hash_logical_plan(std::size_t& seed, const plan::LogicalPlan& logical) {
    hash_combine(seed, static_cast<std::size_t>(logical.kind));
    hash_combine(seed, static_cast<std::size_t>(logical.order_permission));
    hash_combine(seed, static_cast<std::size_t>(logical.join_kind));
    hash_string(seed, logical.table);
    hash_string(seed, logical.binding_name);
    hash_combine(seed, logical.limit_count);

    hash_combine(seed, logical.projections.size());
    for (const auto& projection : logical.projections) {
        hash_projection(seed, projection);
    }
    hash_combine(seed, logical.group_keys.size());
    for (const auto& key : logical.group_keys) {
        hash_column(seed, key);
    }
    hash_combine(seed, logical.aggregate_expressions.size());
    for (const auto& aggregate : logical.aggregate_expressions) {
        hash_aggregate_expression(seed, aggregate);
    }
    hash_combine(seed, logical.sort_keys.size());
    for (const auto& key : logical.sort_keys) {
        hash_sort_key(seed, key);
    }
    hash_combine(seed, logical.predicates.size());
    for (const auto& predicate : logical.predicates) {
        hash_predicate(seed, predicate);
    }

    hash_logical_child(seed, logical.input);
    hash_logical_child(seed, logical.left);
    hash_logical_child(seed, logical.right);
}

std::size_t structural_hash(const MemoExpression& expression) {
    std::size_t seed = 0;
    hash_combine(seed, static_cast<std::size_t>(expression.kind));
    hash_combine(seed, static_cast<std::size_t>(expression.order_permission));
    hash_combine(seed, static_cast<std::size_t>(expression.join_kind));
    hash_string(seed, expression.table);
    hash_string(seed, expression.binding_name);

    hash_combine(seed, expression.projections.size());
    for (const auto& projection : expression.projections) {
        hash_projection(seed, projection);
    }

    hash_combine(seed, expression.group_keys.size());
    for (const auto& key : expression.group_keys) {
        hash_column(seed, key);
    }

    hash_combine(seed, expression.aggregate_expressions.size());
    for (const auto& aggregate : expression.aggregate_expressions) {
        hash_aggregate_expression(seed, aggregate);
    }

    hash_combine(seed, expression.sort_keys.size());
    for (const auto& key : expression.sort_keys) {
        hash_sort_key(seed, key);
    }

    hash_combine(seed, expression.predicates.size());
    for (const auto& predicate : expression.predicates) {
        hash_predicate(seed, predicate);
    }

    hash_combine(seed, expression.limit_count);
    hash_combine(seed, expression.children.size());
    for (const auto child : expression.children) {
        hash_combine(seed, static_cast<std::size_t>(child));
    }

    return seed;
}

bool scalar_equal(const plan::BoundScalarExpr& left, const plan::BoundScalarExpr& right) {
    if (left.type != right.type || left.value.index() != right.value.index()) {
        return false;
    }
    if (const auto* left_column = std::get_if<plan::BoundColumnRef>(&left.value)) {
        const auto& right_column = std::get<plan::BoundColumnRef>(right.value);
        return left_column->binding == right_column.binding && left_column->column == right_column.column &&
               left_column->type == right_column.type;
    }
    if (const auto* left_literal = std::get_if<sql::IntLiteral>(&left.value)) {
        return left_literal->value == std::get<sql::IntLiteral>(right.value).value;
    }
    if (const auto* left_literal = std::get_if<sql::StringLiteral>(&left.value)) {
        return left_literal->value == std::get<sql::StringLiteral>(right.value).value;
    }
    if (std::holds_alternative<sql::NullLiteral>(left.value)) {
        return true;
    }
    const auto& left_subquery = std::get<plan::BoundScalarSubquery>(left.value);
    const auto& right_subquery = std::get<plan::BoundScalarSubquery>(right.value);
    if (left_subquery.plan == nullptr || right_subquery.plan == nullptr) {
        return left_subquery.plan == right_subquery.plan;
    }
    return logical_plan_equal(*left_subquery.plan, *right_subquery.plan);
}

bool comparison_equal(const plan::BoundComparisonExpr& left, const plan::BoundComparisonExpr& right) {
    return scalar_equal(left.left, right.left) && left.op == right.op && scalar_equal(left.right, right.right);
}

bool predicate_equal(const plan::BoundPredicate& left, const plan::BoundPredicate& right) {
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
    case sql::PredicateKind::Comparison:
        return comparison_equal(left.comparison, right.comparison);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_equal(left.null_check, right.null_check);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        if (!scalar_equal(left.in_value, right.in_value)) {
            return false;
        }
        if (left.subquery == nullptr || right.subquery == nullptr) {
            return left.subquery == right.subquery;
        }
        return logical_plan_equal(*left.subquery, *right.subquery);
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        if (left.subquery == nullptr || right.subquery == nullptr) {
            return left.subquery == right.subquery;
        }
        return logical_plan_equal(*left.subquery, *right.subquery);
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (left.left == nullptr || left.right == nullptr || right.left == nullptr || right.right == nullptr) {
            return false;
        }
        return predicate_equal(*left.left, *right.left) && predicate_equal(*left.right, *right.right);
    }
    throw std::logic_error("unreachable predicate kind");
}

bool projection_equal(const plan::Projection& left, const plan::Projection& right) {
    return left.output_name == right.output_name && left.type == right.type && scalar_equal(left.expression, right.expression);
}

bool column_equal(const plan::BoundColumnRef& left, const plan::BoundColumnRef& right) {
    return left.binding == right.binding && left.column == right.column && left.type == right.type;
}

bool aggregate_expression_equal(const plan::AggregateExpression& left, const plan::AggregateExpression& right) {
    if (left.output_name != right.output_name || left.function != right.function ||
        left.type != right.type || left.argument.has_value() != right.argument.has_value()) {
        return false;
    }
    if (!left.argument.has_value()) {
        return true;
    }
    return column_equal(*left.argument, *right.argument);
}

bool sort_key_equal(const plan::SortKey& left, const plan::SortKey& right) {
    return column_equal(left.column, right.column) && left.direction == right.direction;
}

template <typename T, typename Equal>
bool vector_equal(const std::vector<T>& left, const std::vector<T>& right, Equal equal) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!equal(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

bool logical_child_equal(const std::shared_ptr<plan::LogicalPlan>& left,
                         const std::shared_ptr<plan::LogicalPlan>& right) {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    return logical_plan_equal(*left, *right);
}

bool logical_plan_equal(const plan::LogicalPlan& left, const plan::LogicalPlan& right) {
    return left.kind == right.kind && left.order_permission == right.order_permission &&
           left.join_kind == right.join_kind && left.table == right.table && left.binding_name == right.binding_name &&
           left.limit_count == right.limit_count &&
           vector_equal(left.projections, right.projections, projection_equal) &&
           vector_equal(left.group_keys, right.group_keys, column_equal) &&
           vector_equal(left.aggregate_expressions, right.aggregate_expressions, aggregate_expression_equal) &&
           vector_equal(left.sort_keys, right.sort_keys, sort_key_equal) &&
           vector_equal(left.predicates, right.predicates, predicate_equal) &&
           logical_child_equal(left.input, right.input) && logical_child_equal(left.left, right.left) &&
           logical_child_equal(left.right, right.right);
}

bool structural_equal(const MemoExpression& left, const MemoExpression& right) {
    return left.kind == right.kind && left.order_permission == right.order_permission &&
           left.join_kind == right.join_kind && left.table == right.table && left.binding_name == right.binding_name &&
           vector_equal(left.projections, right.projections, projection_equal) &&
           vector_equal(left.group_keys, right.group_keys, column_equal) &&
           vector_equal(left.aggregate_expressions, right.aggregate_expressions, aggregate_expression_equal) &&
           vector_equal(left.sort_keys, right.sort_keys, sort_key_equal) &&
           vector_equal(left.predicates, right.predicates, predicate_equal) && left.limit_count == right.limit_count &&
           left.children == right.children;
}

const plan::LogicalPlan& require_input(const plan::LogicalPlan& logical) {
    if (!logical.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *logical.input;
}

const plan::LogicalPlan& require_left(const plan::LogicalPlan& logical) {
    if (!logical.left) {
        throw std::invalid_argument("logical join node is missing its left input");
    }
    return *logical.left;
}

const plan::LogicalPlan& require_right(const plan::LogicalPlan& logical) {
    if (!logical.right) {
        throw std::invalid_argument("logical join node is missing its right input");
    }
    return *logical.right;
}

std::string expression_to_string(const plan::BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<plan::BoundColumnRef>(&expression.value)) {
        if (column->binding.empty()) {
            return "col(" + column->column + ")";
        }
        return "col(" + column->binding + "." + column->column + ")";
    }
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        return "lit(" + std::to_string(literal->value) + ")";
    }
    if (const auto* literal = std::get_if<sql::StringLiteral>(&expression.value)) {
        return "lit(" + sql::quote_string_literal(literal->value) + ")";
    }
    if (std::holds_alternative<sql::NullLiteral>(expression.value)) {
        return "lit(NULL)";
    }
    return "subquery(" + std::get<plan::BoundScalarSubquery>(expression.value).name + ")";
}

std::string column_to_string(const plan::BoundColumnRef& column) {
    if (column.binding.empty()) {
        return "col(" + column.column + ")";
    }
    return "col(" + column.binding + "." + column.column + ")";
}

std::string comparison_op_to_string(sql::ComparisonOp op) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return "=";
    case sql::ComparisonOp::NotEqual:
        return "<>";
    case sql::ComparisonOp::Less:
        return "<";
    case sql::ComparisonOp::LessEqual:
        return "<=";
    case sql::ComparisonOp::Greater:
        return ">";
    case sql::ComparisonOp::GreaterEqual:
        return ">=";
    }
    throw std::logic_error("unreachable comparison operator");
}

std::string comparison_to_string(const plan::BoundComparisonExpr& comparison) {
    return expression_to_string(comparison.left) + " " + comparison_op_to_string(comparison.op) + " " +
           expression_to_string(comparison.right);
}

std::string predicate_to_string(const plan::BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return comparison_to_string(predicate.comparison);
    case sql::PredicateKind::IsNull:
        return expression_to_string(predicate.null_check) + " IS NULL";
    case sql::PredicateKind::IsNotNull:
        return expression_to_string(predicate.null_check) + " IS NOT NULL";
    case sql::PredicateKind::In:
        return expression_to_string(predicate.in_value) + " IN subquery(" + predicate.subquery_name + ")";
    case sql::PredicateKind::NotIn:
        return expression_to_string(predicate.in_value) + " NOT IN subquery(" + predicate.subquery_name + ")";
    case sql::PredicateKind::Exists:
        return "EXISTS subquery(" + predicate.subquery_name + ")";
    case sql::PredicateKind::NotExists:
        return "NOT EXISTS subquery(" + predicate.subquery_name + ")";
    case sql::PredicateKind::And:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound AND predicate is missing a child");
        }
        return "(" + predicate_to_string(*predicate.left) + " AND " + predicate_to_string(*predicate.right) + ")";
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound OR predicate is missing a child");
        }
        return "(" + predicate_to_string(*predicate.left) + " OR " + predicate_to_string(*predicate.right) + ")";
    }
    throw std::logic_error("unreachable predicate kind");
}

void append_predicates(std::ostringstream& out, const std::vector<plan::BoundPredicate>& predicates) {
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (i != 0) {
            out << " AND ";
        }
        out << predicate_to_string(predicates[i]);
    }
}

std::string sort_direction_to_string(sql::SortDirection direction) {
    switch (direction) {
    case sql::SortDirection::Asc:
        return "ASC";
    case sql::SortDirection::Desc:
        return "DESC";
    }
    throw std::logic_error("unreachable sort direction");
}

std::string sort_key_to_string(const plan::SortKey& key) {
    return column_to_string(key.column) + " " + sort_direction_to_string(key.direction);
}

std::string join_kind_to_string(plan::JoinKind kind) {
    switch (kind) {
    case plan::JoinKind::Inner:
        return "Join";
    case plan::JoinKind::Left:
        return "LeftJoin";
    case plan::JoinKind::Semi:
        return "SemiJoin";
    case plan::JoinKind::Anti:
        return "AntiJoin";
    }
    throw std::logic_error("unreachable join kind");
}

void append_sort_keys(std::ostringstream& out, const std::vector<plan::SortKey>& sort_keys) {
    for (std::size_t i = 0; i < sort_keys.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << sort_key_to_string(sort_keys[i]);
    }
}

void append_group_keys(std::ostringstream& out, const std::vector<plan::BoundColumnRef>& group_keys) {
    for (std::size_t i = 0; i < group_keys.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << column_to_string(group_keys[i]);
    }
}

std::string aggregate_expression_to_string(const plan::AggregateExpression& aggregate) {
    auto text = aggregate.output_name;
    if (aggregate.argument.has_value()) {
        text += "=" + column_to_string(*aggregate.argument);
    }
    return text;
}

void append_aggregate_expressions(std::ostringstream& out,
                                  const std::vector<plan::AggregateExpression>& aggregates) {
    for (std::size_t i = 0; i < aggregates.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << aggregate_expression_to_string(aggregates[i]);
    }
}

void append_children(std::ostringstream& out, const std::vector<GroupId>& children) {
    out << " children=[";
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << children[i];
    }
    out << "]";
}

std::string memo_expression_to_string(const MemoExpression& expression) {
    std::ostringstream out;
    switch (expression.kind) {
    case MemoExpressionKind::Scan:
        out << "Scan[" << expression.table;
        if (expression.binding_name != expression.table) {
            out << " AS " << expression.binding_name;
        }
        out << "]";
        return out.str();
    case MemoExpressionKind::Join:
        out << join_kind_to_string(expression.join_kind) << "[";
        append_predicates(out, expression.predicates);
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Filter:
        out << "Filter[";
        append_predicates(out, expression.predicates);
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Project:
        out << "Project[";
        for (std::size_t i = 0; i < expression.projections.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << expression.projections[i].output_name << "="
                << expression_to_string(expression.projections[i].expression);
        }
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Aggregate:
        out << "Aggregate[group_keys=[";
        append_group_keys(out, expression.group_keys);
        out << "], aggregates=[";
        append_aggregate_expressions(out, expression.aggregate_expressions);
        out << "]]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Distinct:
        out << "Distinct";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Sort:
        out << "Sort[";
        append_sort_keys(out, expression.sort_keys);
        out << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::Limit:
        out << "Limit[" << expression.limit_count << "]";
        append_children(out, expression.children);
        return out.str();
    case MemoExpressionKind::GroupRef:
        out << "GroupRef[" << expression.children.front() << "]";
        return out.str();
    }
    throw std::logic_error("unreachable memo expression kind");
}

MemoExpression group_ref_expression(GroupId representative,
                                    plan::OrderPermission order_permission = plan::OrderPermission::Deterministic) {
    MemoExpression expression;
    expression.kind = MemoExpressionKind::GroupRef;
    expression.order_permission = order_permission;
    expression.children.push_back(representative);
    return expression;
}

// Deterministic logical costing constants. The estimator is intentionally simple:
// scans use exact catalog row counts; filters multiply input rows by recursive
// predicate-tree selectivity. Comparison leaves use one fixed selectivity per
// comparison kind, AND multiplies, and OR uses inclusion-exclusion
// s1 + s2 - s1*s2. Equi-joins use
// |L| * |R| / max(distinct(left_key), distinct(right_key)); non-equi joins use
// the cross product; LEFT joins then clamp the row estimate to at least the
// preserved left input cardinality. Distinct counts are heuristics derived from row counts:
// scan columns start with distinct=row_count and later operators clamp them to
// the estimated output rows. String columns intentionally reuse the same
// row-count math in Phase 17a; value-width and collation-aware CPU costing are
// future work. Estimates are clamped to finite non-negative values; winner ties
// use exact double equality and lower expression index.
constexpr double kMaxEstimate = 1.0e100;
constexpr double kEqualitySelectivity = 0.10;
constexpr double kNotEqualSelectivity = 0.90;
constexpr double kInequalitySelectivity = 1.0 / 3.0;

struct RelationEstimate {
    double rows{0.0};
    double cost{0.0};
    std::map<std::string, double> distinct_by_column;
    plan::LogicalPlan plan;
};

RelationEstimate estimate_logical_relation(const plan::LogicalPlan& logical, const catalog::Catalog& catalog);

struct EquiJoinKeyEstimate {
    std::string left_key;
    std::string right_key;
    double left_distinct{0.0};
    double right_distinct{0.0};
};

double clamp_estimate(double value) {
    if (std::isnan(value) || value <= 0.0) {
        return 0.0;
    }
    if (!std::isfinite(value) || value > kMaxEstimate) {
        return kMaxEstimate;
    }
    return value;
}

double safe_add(double left, double right) {
    return clamp_estimate(left + right);
}

double scalar_subquery_cost(const plan::BoundScalarExpr& expression, const catalog::Catalog& catalog) {
    const auto* subquery = std::get_if<plan::BoundScalarSubquery>(&expression.value);
    if (subquery == nullptr) {
        return 0.0;
    }
    if (subquery->plan == nullptr) {
        throw std::logic_error("bound scalar subquery is missing its logical plan");
    }
    return estimate_logical_relation(*subquery->plan, catalog).cost;
}

double predicate_subquery_cost(const plan::BoundPredicate& predicate, const catalog::Catalog& catalog) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return safe_add(scalar_subquery_cost(predicate.comparison.left, catalog),
                        scalar_subquery_cost(predicate.comparison.right, catalog));
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_subquery_cost(predicate.null_check, catalog);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        if (predicate.subquery == nullptr) {
            throw std::logic_error("bound IN predicate is missing its subquery");
        }
        return safe_add(scalar_subquery_cost(predicate.in_value, catalog),
                        estimate_logical_relation(*predicate.subquery, catalog).cost);
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        if (predicate.subquery == nullptr) {
            throw std::logic_error("bound EXISTS predicate is missing its subquery");
        }
        return estimate_logical_relation(*predicate.subquery, catalog).cost;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return safe_add(predicate_subquery_cost(*predicate.left, catalog),
                        predicate_subquery_cost(*predicate.right, catalog));
    }
    throw std::logic_error("unreachable predicate kind");
}

double predicates_subquery_cost(const std::vector<plan::BoundPredicate>& predicates,
                                const catalog::Catalog& catalog) {
    double cost = 0.0;
    for (const auto& predicate : predicates) {
        cost = safe_add(cost, predicate_subquery_cost(predicate, catalog));
    }
    return cost;
}

double projections_subquery_cost(const std::vector<plan::Projection>& projections,
                                 const catalog::Catalog& catalog) {
    double cost = 0.0;
    for (const auto& projection : projections) {
        cost = safe_add(cost, scalar_subquery_cost(projection.expression, catalog));
    }
    return cost;
}

double safe_multiply(double left, double right) {
    if (left <= 0.0 || right <= 0.0) {
        return 0.0;
    }
    if (left > kMaxEstimate / right) {
        return kMaxEstimate;
    }
    return clamp_estimate(left * right);
}

double safe_divide(double numerator, double denominator) {
    if (numerator <= 0.0) {
        return 0.0;
    }
    if (denominator <= 0.0) {
        return kMaxEstimate;
    }
    return clamp_estimate(numerator / denominator);
}

catalog::TableSchema require_costed_table_schema(const std::string& table, const catalog::Catalog& catalog) {
    auto schema = catalog.find_table_schema(table);
    if (!schema.has_value()) {
        throw std::invalid_argument("missing schema for costed table '" + table + "'");
    }
    if (!schema->row_count.has_value()) {
        throw std::invalid_argument("missing row-count statistics for costed table '" + table + "'");
    }
    return std::move(*schema);
}

std::string column_key(const std::string& table, const std::string& column) {
    if (table.empty()) {
        return column;
    }
    return table + "." + column;
}

std::string column_key(const plan::BoundColumnRef& column) {
    return column_key(column.binding, column.column);
}

std::optional<std::int64_t> literal_value(const plan::BoundScalarExpr& expression) {
    if (const auto* literal = std::get_if<sql::IntLiteral>(&expression.value)) {
        return literal->value;
    }
    return std::nullopt;
}

bool compare_values(std::int64_t left, sql::ComparisonOp op, std::int64_t right) {
    switch (op) {
    case sql::ComparisonOp::Equal:
        return left == right;
    case sql::ComparisonOp::NotEqual:
        return left != right;
    case sql::ComparisonOp::Less:
        return left < right;
    case sql::ComparisonOp::LessEqual:
        return left <= right;
    case sql::ComparisonOp::Greater:
        return left > right;
    case sql::ComparisonOp::GreaterEqual:
        return left >= right;
    }
    throw std::logic_error("unreachable comparison operator");
}

double comparison_selectivity(const plan::BoundComparisonExpr& predicate) {
    const auto left_literal = literal_value(predicate.left);
    const auto right_literal = literal_value(predicate.right);
    if (left_literal.has_value() && right_literal.has_value()) {
        return compare_values(*left_literal, predicate.op, *right_literal) ? 1.0 : 0.0;
    }

    switch (predicate.op) {
    case sql::ComparisonOp::Equal:
        return kEqualitySelectivity;
    case sql::ComparisonOp::NotEqual:
        return kNotEqualSelectivity;
    case sql::ComparisonOp::Less:
    case sql::ComparisonOp::LessEqual:
    case sql::ComparisonOp::Greater:
    case sql::ComparisonOp::GreaterEqual:
        return kInequalitySelectivity;
    }
    throw std::logic_error("unreachable comparison operator");
}

double predicate_selectivity(const plan::BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return comparison_selectivity(predicate.comparison);
    case sql::PredicateKind::IsNull:
        return kEqualitySelectivity;
    case sql::PredicateKind::IsNotNull:
        return kNotEqualSelectivity;
    case sql::PredicateKind::In:
    case sql::PredicateKind::Exists:
        return kEqualitySelectivity;
    case sql::PredicateKind::NotIn:
    case sql::PredicateKind::NotExists:
        return kNotEqualSelectivity;
    case sql::PredicateKind::And: {
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound AND predicate is missing a child");
        }
        return safe_multiply(predicate_selectivity(*predicate.left), predicate_selectivity(*predicate.right));
    }
    case sql::PredicateKind::Or: {
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound OR predicate is missing a child");
        }
        const auto left = predicate_selectivity(*predicate.left);
        const auto right = predicate_selectivity(*predicate.right);
        return clamp_estimate(left + right - left * right);
    }
    }
    throw std::logic_error("unreachable predicate kind");
}

double combined_selectivity(const std::vector<plan::BoundPredicate>& predicates,
                            std::optional<std::size_t> skipped_predicate = std::nullopt) {
    double selectivity = 1.0;
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        if (skipped_predicate.has_value() && *skipped_predicate == i) {
            continue;
        }
        selectivity = safe_multiply(selectivity, predicate_selectivity(predicates[i]));
    }
    return selectivity;
}

double distinct_for(const RelationEstimate& estimate, const plan::BoundColumnRef& column) {
    const auto it = estimate.distinct_by_column.find(column_key(column));
    if (it == estimate.distinct_by_column.end()) {
        throw std::logic_error("cost model could not find a distinct estimate for column '" + column_key(column) + "'");
    }
    return it->second;
}

std::optional<EquiJoinKeyEstimate> usable_equi_join_key(const plan::BoundPredicate& predicate,
                                                        const RelationEstimate& left,
                                                        const RelationEstimate& right) {
    if (predicate.kind != sql::PredicateKind::Comparison) {
        return std::nullopt;
    }

    const auto& comparison = predicate.comparison;
    if (comparison.op != sql::ComparisonOp::Equal) {
        return std::nullopt;
    }

    const auto* left_column = std::get_if<plan::BoundColumnRef>(&comparison.left.value);
    const auto* right_column = std::get_if<plan::BoundColumnRef>(&comparison.right.value);
    if (left_column == nullptr || right_column == nullptr) {
        return std::nullopt;
    }

    const auto left_key = column_key(*left_column);
    const auto right_key = column_key(*right_column);
    const auto left_has_left = left.distinct_by_column.contains(left_key);
    const auto left_has_right = left.distinct_by_column.contains(right_key);
    const auto right_has_left = right.distinct_by_column.contains(left_key);
    const auto right_has_right = right.distinct_by_column.contains(right_key);

    if (left_has_left && right_has_right) {
        return EquiJoinKeyEstimate{left_key, right_key, distinct_for(left, *left_column), distinct_for(right, *right_column)};
    }
    if (left_has_right && right_has_left) {
        return EquiJoinKeyEstimate{right_key, left_key, distinct_for(left, *right_column), distinct_for(right, *left_column)};
    }
    return std::nullopt;
}

RelationEstimate scan_estimate(const std::string& table,
                               const std::string& binding_name,
                               plan::OrderPermission order_permission,
                               const catalog::Catalog& catalog) {
    const auto schema = require_costed_table_schema(table, catalog);
    RelationEstimate estimate;
    estimate.rows = clamp_estimate(static_cast<double>(*schema.row_count));
    estimate.cost = estimate.rows;
    for (const auto& column : schema.columns) {
        estimate.distinct_by_column[column_key(binding_name, column.name)] = estimate.rows <= 0.0 ? 0.0 : estimate.rows;
    }
    estimate.plan = plan::LogicalPlan::scan(table, binding_name);
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate filter_estimate(const std::vector<plan::BoundPredicate>& predicates,
                                 RelationEstimate child,
                                 plan::OrderPermission order_permission,
                                 const catalog::Catalog& catalog) {
    const auto selectivity = combined_selectivity(predicates);
    RelationEstimate estimate;
    estimate.rows = safe_multiply(child.rows, selectivity);
    // An uncorrelated embedded subplan is materialized once by the owning
    // operator, never once per input row and never as a memo child.
    estimate.cost = safe_add(safe_add(child.cost, child.rows), predicates_subquery_cost(predicates, catalog));
    for (const auto& [column, distinct] : child.distinct_by_column) {
        estimate.distinct_by_column[column] = std::min(safe_multiply(distinct, selectivity), estimate.rows);
    }
    estimate.plan = plan::LogicalPlan::filter(predicates, std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate project_estimate(const std::vector<plan::Projection>& projections,
                                  RelationEstimate child,
                                  plan::OrderPermission order_permission,
                                  const catalog::Catalog& catalog) {
    RelationEstimate estimate;
    estimate.rows = child.rows;
    // Project is cost-neutral for join-order choice in this logical slice.
    estimate.cost = safe_add(child.cost, projections_subquery_cost(projections, catalog));
    for (const auto& projection : projections) {
        if (const auto* column = std::get_if<plan::BoundColumnRef>(&projection.expression.value)) {
            const auto key = column_key(*column);
            const auto it = child.distinct_by_column.find(key);
            if (it != child.distinct_by_column.end()) {
                estimate.distinct_by_column[key] = std::min(it->second, estimate.rows);
                estimate.distinct_by_column[column_key("", projection.output_name)] = std::min(it->second, estimate.rows);
            }
        } else {
            estimate.distinct_by_column[column_key("", projection.output_name)] = estimate.rows == 0.0 ? 0.0 : 1.0;
        }
    }
    estimate.plan = plan::LogicalPlan::project(projections, std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate aggregate_estimate(const std::vector<plan::BoundColumnRef>& group_keys,
                                    const std::vector<plan::AggregateExpression>& aggregate_expressions,
                                    RelationEstimate child,
                                    plan::OrderPermission order_permission) {
    RelationEstimate estimate;
    if (group_keys.empty()) {
        estimate.rows = 1.0;
    } else {
        double group_rows = 1.0;
        for (const auto& key : group_keys) {
            group_rows = safe_multiply(group_rows, distinct_for(child, key));
        }
        estimate.rows = std::min(child.rows, group_rows);
    }

    // Aggregate performs one linear input pass plus one output pass over the
    // first-appearance group vector. Global aggregation returns one group even
    // for empty input so COUNT(*) can produce zero and SUM/MIN/MAX can produce
    // NULL under the aggregate result contract.
    estimate.cost = safe_add(safe_add(child.cost, child.rows), estimate.rows);
    for (const auto& key : group_keys) {
        const auto key_name = column_key(key);
        estimate.distinct_by_column[key_name] =
            std::min(distinct_for(child, key), estimate.rows);
    }
    for (const auto& aggregate : aggregate_expressions) {
        estimate.distinct_by_column[column_key("", aggregate.output_name)] = estimate.rows;
    }
    estimate.plan = plan::LogicalPlan::aggregate(group_keys, aggregate_expressions, std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate distinct_estimate(RelationEstimate child, plan::OrderPermission order_permission) {
    RelationEstimate estimate;
    double distinct_rows = child.rows;
    if (child.plan.kind == plan::LogicalKind::Project) {
        distinct_rows = 1.0;
        for (const auto& projection : child.plan.projections) {
            if (const auto* column = std::get_if<plan::BoundColumnRef>(&projection.expression.value)) {
                const auto it = child.distinct_by_column.find(column_key(*column));
                distinct_rows = safe_multiply(distinct_rows, it == child.distinct_by_column.end() ? child.rows : it->second);
            } else {
                distinct_rows = safe_multiply(distinct_rows, child.rows == 0.0 ? 0.0 : 1.0);
            }
        }
        distinct_rows = std::min(child.rows, distinct_rows);
    }

    estimate.rows = distinct_rows;
    // Distinct performs one input pass plus one pass over the first-appearance
    // output-row vector. The hash table is lookup-only and does not define order.
    estimate.cost = safe_add(safe_add(child.cost, child.rows), estimate.rows);
    for (const auto& [column, distinct] : child.distinct_by_column) {
        estimate.distinct_by_column[column] = std::min(distinct, estimate.rows);
    }
    estimate.plan = plan::LogicalPlan::distinct(std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate sort_estimate(const std::vector<plan::SortKey>& sort_keys,
                               RelationEstimate child,
                               plan::OrderPermission order_permission) {
    RelationEstimate estimate;
    estimate.rows = child.rows;
    // Sort cost is documented for this phase as input cost plus n*log2(n).
    // Empty and single-row inputs add no local ordering work.
    const auto local_cost = estimate.rows <= 1.0 ? 0.0 : clamp_estimate(estimate.rows * std::log2(estimate.rows));
    estimate.cost = safe_add(child.cost, local_cost);
    estimate.distinct_by_column = std::move(child.distinct_by_column);
    estimate.plan = plan::LogicalPlan::sort(sort_keys, std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate limit_estimate(std::size_t limit_count, RelationEstimate child, plan::OrderPermission order_permission) {
    RelationEstimate estimate;
    estimate.rows = std::min(static_cast<double>(limit_count), child.rows);
    // Limit only trims the already-produced stream; it does not add local work
    // to this logical cost model.
    estimate.cost = child.cost;
    for (const auto& [column, distinct] : child.distinct_by_column) {
        estimate.distinct_by_column[column] = std::min(distinct, estimate.rows);
    }
    estimate.plan = plan::LogicalPlan::limit(limit_count, std::move(child.plan));
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate join_estimate(const std::vector<plan::BoundPredicate>& predicates,
                               RelationEstimate left,
                               RelationEstimate right,
                               plan::JoinKind join_kind,
                               plan::OrderPermission order_permission,
                               const catalog::Catalog& catalog) {
    std::optional<EquiJoinKeyEstimate> equi_key;
    std::optional<std::size_t> equi_predicate_index;
    for (std::size_t i = 0; i < predicates.size(); ++i) {
        equi_key = usable_equi_join_key(predicates[i], left, right);
        if (equi_key.has_value()) {
            equi_predicate_index = i;
            break;
        }
    }

    const auto cross_rows = safe_multiply(left.rows, right.rows);
    const auto base_rows = equi_key.has_value()
                               ? safe_divide(cross_rows, std::max({1.0, equi_key->left_distinct, equi_key->right_distinct}))
                               : cross_rows;
    const auto residual_selectivity = combined_selectivity(predicates, equi_predicate_index);

    RelationEstimate estimate;
    const auto inner_rows = safe_multiply(base_rows, residual_selectivity);
    // Semi/Anti cardinality is deliberately heuristic until match-frequency
    // statistics exist. `s=0.5` is the documented probability that a left row
    // has at least one TRUE match; Anti uses its exact complement. Both kinds
    // emit only left rows, never pair multiplicity.
    constexpr double semi_match_selectivity = 0.5;
    if (join_kind == plan::JoinKind::Semi) {
        estimate.rows = safe_multiply(left.rows, semi_match_selectivity);
    } else if (join_kind == plan::JoinKind::Anti) {
        estimate.rows = safe_multiply(left.rows, 1.0 - semi_match_selectivity);
    } else {
        estimate.rows = join_kind == plan::JoinKind::Left ? std::max(inner_rows, left.rows) : inner_rows;
    }
    // Equi-joins model a linear right build plus left probe. Keyless Semi/Anti
    // (EXISTS/NOT EXISTS) likewise materialize the right once and test only its
    // emptiness. A residual-only join retains nested-loop pair work.
    const auto keyless_existence =
        (join_kind == plan::JoinKind::Semi || join_kind == plan::JoinKind::Anti) && predicates.empty();
    const auto local_cost =
        equi_key.has_value() || keyless_existence ? safe_add(left.rows, right.rows) : cross_rows;
    estimate.cost = safe_add(safe_add(safe_add(left.cost, right.cost), local_cost),
                             predicates_subquery_cost(predicates, catalog));
    estimate.distinct_by_column = left.distinct_by_column;
    if (join_kind == plan::JoinKind::Inner || join_kind == plan::JoinKind::Left) {
        estimate.distinct_by_column.insert(right.distinct_by_column.begin(), right.distinct_by_column.end());
    }
    for (auto& [_, distinct] : estimate.distinct_by_column) {
        distinct = std::min(distinct, estimate.rows);
    }
    if (equi_key.has_value() && join_kind == plan::JoinKind::Inner) {
        const auto joined_distinct = std::min({equi_key->left_distinct, equi_key->right_distinct, estimate.rows});
        estimate.distinct_by_column[equi_key->left_key] = joined_distinct;
        estimate.distinct_by_column[equi_key->right_key] = joined_distinct;
    }

    estimate.plan = plan::LogicalPlan::join(predicates, std::move(left.plan), std::move(right.plan), join_kind);
    estimate.plan.order_permission = order_permission;
    return estimate;
}

RelationEstimate estimate_logical_relation(const plan::LogicalPlan& logical, const catalog::Catalog& catalog) {
    switch (logical.kind) {
    case plan::LogicalKind::Scan:
        return scan_estimate(logical.table, logical.binding_name, logical.order_permission, catalog);
    case plan::LogicalKind::Filter:
        return filter_estimate(logical.predicates,
                               estimate_logical_relation(require_input(logical), catalog),
                               logical.order_permission,
                               catalog);
    case plan::LogicalKind::Project:
        return project_estimate(logical.projections,
                                estimate_logical_relation(require_input(logical), catalog),
                                logical.order_permission,
                                catalog);
    case plan::LogicalKind::Aggregate:
        return aggregate_estimate(logical.group_keys,
                                  logical.aggregate_expressions,
                                  estimate_logical_relation(require_input(logical), catalog),
                                  logical.order_permission);
    case plan::LogicalKind::Distinct:
        return distinct_estimate(estimate_logical_relation(require_input(logical), catalog), logical.order_permission);
    case plan::LogicalKind::Sort:
        return sort_estimate(logical.sort_keys,
                             estimate_logical_relation(require_input(logical), catalog),
                             logical.order_permission);
    case plan::LogicalKind::Limit:
        return limit_estimate(logical.limit_count,
                              estimate_logical_relation(require_input(logical), catalog),
                              logical.order_permission);
    case plan::LogicalKind::Join:
        return join_estimate(logical.predicates,
                             estimate_logical_relation(require_left(logical), catalog),
                             estimate_logical_relation(require_right(logical), catalog),
                             logical.join_kind,
                             logical.order_permission,
                             catalog);
    case plan::LogicalKind::Explain:
        throw std::invalid_argument("EXPLAIN logical plans cannot be costed directly");
    }
    throw std::logic_error("unreachable logical plan kind");
}

struct Winner {
    RelationEstimate estimate;
    std::size_t expression_index{0};
};

bool is_better_winner(const Winner& candidate, const Winner& incumbent) {
    if (candidate.estimate.cost < incumbent.estimate.cost) {
        return true;
    }
    if (candidate.estimate.cost > incumbent.estimate.cost) {
        return false;
    }
    return candidate.expression_index < incumbent.expression_index;
}

class BestExtractor {
public:
    BestExtractor(const Memo& memo, const catalog::Catalog& catalog)
        : memo_(memo), catalog_(catalog), winners_(memo.group_count() + 1) {}

    [[nodiscard]] RelationEstimate extract(GroupId id) {
        std::vector<GroupId> stack;
        return best_for_group(id, stack).estimate;
    }

private:
    [[nodiscard]] Winner best_for_group(GroupId id, std::vector<GroupId>& stack) {
        id = memo_.representative(id);
        if (winners_.at(static_cast<std::size_t>(id)).has_value()) {
            return *winners_.at(static_cast<std::size_t>(id));
        }
        if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
            throw std::logic_error("memo best extraction encountered a cycle");
        }

        stack.push_back(id);
        const auto& memo_group = memo_.group(id);
        std::optional<Winner> best;
        for (std::size_t i = 0; i < memo_group.expressions.size(); ++i) {
            Winner candidate{estimate_expression(memo_group.expressions[i].expression, stack), i};
            if (!best.has_value() || is_better_winner(candidate, *best)) {
                best = std::move(candidate);
            }
        }
        stack.pop_back();

        if (!best.has_value()) {
            throw std::logic_error("memo best extraction found an empty group");
        }
        winners_.at(static_cast<std::size_t>(id)) = *best;
        return *best;
    }

    [[nodiscard]] RelationEstimate estimate_expression(const MemoExpression& expression, std::vector<GroupId>& stack) {
        switch (expression.kind) {
        case MemoExpressionKind::Scan:
            return scan_estimate(expression.table, expression.binding_name, expression.order_permission, catalog_);
        case MemoExpressionKind::Filter:
            return filter_estimate(expression.predicates,
                                   best_for_group(expression.children.at(0), stack).estimate,
                                   expression.order_permission,
                                   catalog_);
        case MemoExpressionKind::Project:
            return project_estimate(expression.projections,
                                    best_for_group(expression.children.at(0), stack).estimate,
                                    expression.order_permission,
                                    catalog_);
        case MemoExpressionKind::Aggregate:
            return aggregate_estimate(expression.group_keys,
                                      expression.aggregate_expressions,
                                      best_for_group(expression.children.at(0), stack).estimate,
                                      expression.order_permission);
        case MemoExpressionKind::Distinct:
            return distinct_estimate(best_for_group(expression.children.at(0), stack).estimate,
                                     expression.order_permission);
        case MemoExpressionKind::Sort:
            return sort_estimate(expression.sort_keys,
                                 best_for_group(expression.children.at(0), stack).estimate,
                                 expression.order_permission);
        case MemoExpressionKind::Limit:
            return limit_estimate(expression.limit_count,
                                  best_for_group(expression.children.at(0), stack).estimate,
                                  expression.order_permission);
        case MemoExpressionKind::Join:
            return join_estimate(expression.predicates,
                                 best_for_group(expression.children.at(0), stack).estimate,
                                 best_for_group(expression.children.at(1), stack).estimate,
                                 expression.join_kind,
                                 expression.order_permission,
                                 catalog_);
        case MemoExpressionKind::GroupRef:
            return best_for_group(expression.children.at(0), stack).estimate;
        }
        throw std::logic_error("unreachable memo expression kind");
    }

    const Memo& memo_;
    const catalog::Catalog& catalog_;
    std::vector<std::optional<Winner>> winners_;
};

} // namespace

GroupId Memo::insert(const plan::LogicalPlan& logical) {
    // Embedded subplans stay inside predicates/scalars by default. Structural
    // helpers include their full trees, while ordinary relational ingest follows
    // only input/left/right. Phase 21b rules explicitly call insert() only after
    // proving a top-level EXISTS/IN leaf can be lifted to SEMI/ANTI algebra.
    MemoExpression expression;
    expression.order_permission = logical.order_permission;
    switch (logical.kind) {
    case plan::LogicalKind::Scan:
        expression.kind = MemoExpressionKind::Scan;
        expression.table = logical.table;
        expression.binding_name = logical.binding_name;
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Filter:
        expression.kind = MemoExpressionKind::Filter;
        expression.predicates = logical.predicates;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Project:
        expression.kind = MemoExpressionKind::Project;
        expression.projections = logical.projections;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Aggregate:
        expression.kind = MemoExpressionKind::Aggregate;
        expression.group_keys = logical.group_keys;
        expression.aggregate_expressions = logical.aggregate_expressions;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Distinct:
        expression.kind = MemoExpressionKind::Distinct;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Sort:
        expression.kind = MemoExpressionKind::Sort;
        expression.sort_keys = logical.sort_keys;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Limit:
        expression.kind = MemoExpressionKind::Limit;
        expression.limit_count = logical.limit_count;
        expression.children.push_back(insert(require_input(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Join:
        expression.kind = MemoExpressionKind::Join;
        expression.join_kind = logical.join_kind;
        expression.predicates = logical.predicates;
        expression.children.push_back(insert(require_left(logical)));
        expression.children.push_back(insert(require_right(logical)));
        return insert_expression(std::move(expression));
    case plan::LogicalKind::Explain:
        throw std::invalid_argument("EXPLAIN logical plans cannot be inserted into the memo");
    }
    throw std::logic_error("unreachable logical plan kind");
}

GroupId Memo::insert_expression(MemoExpression expression) {
    canonicalize_expression_references(expression);
    validate_expression(expression);
    if (const auto existing = find_existing(expression); existing != 0) {
        return existing;
    }

    const auto id = static_cast<GroupId>(groups_.size() + 1);
    representatives_.push_back(id);
    groups_.push_back(MemoGroup{id, {GroupExpression{std::move(expression)}}});
    index_expression(groups_.back().expressions.front().expression, id);
    assert_invariants();
    return id;
}

bool Memo::insert_equivalent(GroupId group_id, MemoExpression expression) {
    if (!has_group(group_id)) {
        throw std::out_of_range("unknown memo group");
    }
    group_id = representative(group_id);
    canonicalize_expression_references(expression);
    validate_expression(expression);
    validate_no_cycle_from(group_id, expression);

    if (const auto existing = find_existing(expression); existing != 0) {
        const auto existing_representative = representative(existing);
        if (existing_representative == group_id) {
            return false;
        }
        merge_groups(group_id, existing_representative);
        return true;
    }

    auto& target = groups_.at(index_for(group_id));
    target.expressions.push_back(GroupExpression{std::move(expression)});
    index_expression(target.expressions.back().expression, group_id);
    assert_invariants();
    return true;
}

bool Memo::insert_group_ref_equivalent(GroupId group_id, GroupId equivalent_group) {
    if (!has_group(group_id) || !has_group(equivalent_group)) {
        throw std::out_of_range("unknown memo group");
    }
    if (group_id == equivalent_group) {
        return false;
    }

    const auto order_permission = group(group_id).expressions.front().expression.order_permission;
    auto reference = group_ref_expression(representative(equivalent_group), order_permission);
    return insert_equivalent(group_id, std::move(reference));
}

const MemoGroup& Memo::group(GroupId id) const {
    return groups_.at(index_for(representative(id)));
}

plan::LogicalPlan Memo::extract(GroupId id) const {
    assert_invariants();
    std::vector<GroupId> stack;
    return extract(id, stack);
}

plan::LogicalPlan Memo::extract_best(GroupId id, const catalog::Catalog& catalog) const {
    assert_invariants();
    BestExtractor extractor(*this, catalog);
    return extractor.extract(id).plan;
}

AlternativeExtractionResult Memo::extract_alternatives(GroupId id, AlternativeExtractionOptions options) const {
    assert_invariants();
    if (options.max_expressions_per_group == 0 || options.max_plans == 0) {
        throw std::invalid_argument("memo alternative extraction bounds must be non-zero");
    }

    AlternativeExtractionResult result;
    std::vector<GroupId> stack;
    result.plans = extract_alternatives_for_group(id, options, result, stack);
    if (result.plans.size() > options.max_plans) {
        result.plans.resize(options.max_plans);
        result.hit_plan_bound = true;
    }
    return result;
}

std::string Memo::dump() const {
    assert_invariants();
    std::ostringstream out;
    for (const auto& memo_group : groups_) {
        const auto rep = representative(memo_group.id);
        if (rep != memo_group.id) {
            out << "group " << memo_group.id << " -> representative " << rep << ":\n";
        } else {
            out << "group " << memo_group.id << ":\n";
        }
        for (std::size_t i = 0; i < memo_group.expressions.size(); ++i) {
            out << "  expr " << i << ": " << memo_expression_to_string(memo_group.expressions[i].expression) << "\n";
        }
    }
    return out.str();
}

void Memo::assert_invariants() const {
    if (representatives_.size() != groups_.size() + 1 || representatives_.front() != 0) {
        throw std::logic_error("memo representative table cardinality is inconsistent");
    }
    for (GroupId id = 1; id <= groups_.size(); ++id) {
        const auto parent = representatives_.at(static_cast<std::size_t>(id));
        if (!has_group(parent)) {
            throw std::logic_error("memo representative points at an unknown group");
        }
        const auto rep = representative(id);
        if (!has_group(rep) || representative(rep) != rep) {
            throw std::logic_error("memo representative chain is not rooted");
        }
        if (rep > id) {
            throw std::logic_error("memo representative winner is not deterministic");
        }
    }

    std::size_t expression_count = 0;
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        const auto expected_id = static_cast<GroupId>(i + 1);
        if (groups_[i].id != expected_id) {
            throw std::logic_error("memo group id does not match deterministic vector position");
        }
        if (groups_[i].expressions.empty()) {
            throw std::logic_error("memo group has no expressions");
        }

        if (!is_representative(groups_[i].id)) {
            if (groups_[i].expressions.size() != 1) {
                throw std::logic_error("memo non-representative group must contain one alias expression");
            }
            const auto& alias = groups_[i].expressions.front().expression;
            if (alias.kind != MemoExpressionKind::GroupRef || alias.children.size() != 1 ||
                alias.children.front() != representative(groups_[i].id)) {
                throw std::logic_error("memo non-representative group does not point at its representative");
            }
            validate_expression(alias);
            validate_no_cycle_from(groups_[i].id, alias);
            continue;
        }

        for (const auto& expression : groups_[i].expressions) {
            validate_expression(expression.expression);
            validate_no_cycle_from(groups_[i].id, expression.expression);
            for (const auto child : expression.expression.children) {
                if (child != representative(child)) {
                    throw std::logic_error("memo representative expression has a non-canonical child reference");
                }
            }
            const auto owner = find_existing(expression.expression);
            if (owner != groups_[i].id) {
                throw std::logic_error("memo structural index does not point back to expression owner");
            }
            ++expression_count;
        }
    }

    std::size_t indexed_count = 0;
    for (const auto& bucket : expression_index_) {
        for (const auto& indexed : bucket.second) {
            if (structural_hash(indexed.first) != bucket.first) {
                throw std::logic_error("memo structural index bucket hash is inconsistent");
            }
            if (!is_representative(indexed.second)) {
                throw std::logic_error("memo structural index owner is not a representative group");
            }
            const auto& owner = group(indexed.second);
            const auto found = std::find_if(owner.expressions.begin(), owner.expressions.end(), [&](const auto& item) {
                return structural_equal(item.expression, indexed.first);
            });
            if (found == owner.expressions.end()) {
                throw std::logic_error("memo structural index references a missing expression");
            }
            ++indexed_count;
        }
    }

    if (indexed_count != expression_count) {
        throw std::logic_error("memo structural index cardinality does not match group expressions");
    }
}

bool Memo::has_group(GroupId id) const {
    return id != 0 && id <= groups_.size();
}

std::size_t Memo::index_for(GroupId id) const {
    if (!has_group(id)) {
        throw std::out_of_range("unknown memo group");
    }
    return static_cast<std::size_t>(id - 1);
}

GroupId Memo::representative(GroupId id) const {
    if (!has_group(id)) {
        throw std::out_of_range("unknown memo group");
    }
    auto current = id;
    std::vector<GroupId> seen;
    while (representatives_.at(static_cast<std::size_t>(current)) != current) {
        if (std::find(seen.begin(), seen.end(), current) != seen.end()) {
            throw std::logic_error("memo representative cycle detected");
        }
        seen.push_back(current);
        current = representatives_.at(static_cast<std::size_t>(current));
        if (!has_group(current)) {
            throw std::logic_error("memo representative points at an unknown group");
        }
    }
    return current;
}

bool Memo::is_representative(GroupId id) const {
    return representative(id) == id;
}

GroupId Memo::find_existing(const MemoExpression& expression) const {
    auto canonical = expression;
    canonicalize_expression_references(canonical);
    const auto hash = structural_hash(canonical);
    const auto it = expression_index_.find(hash);
    if (it == expression_index_.end()) {
        return 0;
    }
    for (const auto& candidate : it->second) {
        if (structural_equal(candidate.first, canonical)) {
            return representative(candidate.second);
        }
    }
    return 0;
}

bool Memo::reaches(GroupId from, GroupId target) const {
    std::vector<GroupId> stack;
    return reaches(from, target, stack);
}

bool Memo::reaches(GroupId from, GroupId target, std::vector<GroupId>& stack) const {
    if (from == target) {
        return true;
    }
    if (!has_group(from)) {
        throw std::out_of_range("unknown memo group");
    }
    if (std::find(stack.begin(), stack.end(), from) != stack.end()) {
        return false;
    }

    stack.push_back(from);
    for (const auto& expression : group(from).expressions) {
        for (const auto child : expression.expression.children) {
            if (reaches(child, target, stack)) {
                stack.pop_back();
                return true;
            }
        }
    }
    stack.pop_back();
    return false;
}

plan::LogicalPlan Memo::extract(GroupId id, std::vector<GroupId>& stack) const {
    id = representative(id);
    const auto& memo_group = group(id);
    if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
        throw std::logic_error("memo extraction encountered a cycle");
    }

    stack.push_back(id);
    const auto& expression = memo_group.expressions.front().expression;
    auto result = extract_expression(expression, stack);
    stack.pop_back();
    return result;
}

plan::LogicalPlan Memo::extract_expression(const MemoExpression& expression, std::vector<GroupId>& stack) const {
    switch (expression.kind) {
    case MemoExpressionKind::Scan: {
        auto result = plan::LogicalPlan::scan(expression.table, expression.binding_name);
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Filter: {
        auto result = plan::LogicalPlan::filter(expression.predicates, extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Project: {
        auto result = plan::LogicalPlan::project(expression.projections, extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Aggregate: {
        auto result = plan::LogicalPlan::aggregate(expression.group_keys,
                                                  expression.aggregate_expressions,
                                                  extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Distinct: {
        auto result = plan::LogicalPlan::distinct(extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Sort: {
        auto result = plan::LogicalPlan::sort(expression.sort_keys, extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Limit: {
        auto result = plan::LogicalPlan::limit(expression.limit_count, extract(expression.children.at(0), stack));
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::Join: {
        auto result = plan::LogicalPlan::join(expression.predicates,
                                             extract(expression.children.at(0), stack),
                                             extract(expression.children.at(1), stack),
                                             expression.join_kind);
        result.order_permission = expression.order_permission;
        return result;
    }
    case MemoExpressionKind::GroupRef: {
        return extract(expression.children.at(0), stack);
    }
    }
    throw std::logic_error("unreachable memo expression kind");
}

void Memo::validate_expression(const MemoExpression& expression) const {
    for (const auto child : expression.children) {
        if (!has_group(child)) {
            throw std::logic_error("memo expression references an unknown child group");
        }
    }

    switch (expression.kind) {
    case MemoExpressionKind::Scan:
        if (expression.table.empty() || expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || !expression.children.empty()) {
            throw std::logic_error("malformed memo scan expression");
        }
        return;
    case MemoExpressionKind::Filter:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo filter expression");
        }
        return;
    case MemoExpressionKind::Project:
        if (!expression.table.empty() || !expression.binding_name.empty() || expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo project expression");
        }
        return;
    case MemoExpressionKind::Aggregate:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            (expression.group_keys.empty() && expression.aggregate_expressions.empty()) || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo aggregate expression");
        }
        return;
    case MemoExpressionKind::Distinct:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo distinct expression");
        }
        return;
    case MemoExpressionKind::Sort:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo sort expression");
        }
        return;
    case MemoExpressionKind::Limit:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.children.size() != 1) {
            throw std::logic_error("malformed memo limit expression");
        }
        return;
    case MemoExpressionKind::Join:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            expression.limit_count != 0 || expression.children.size() != 2) {
            throw std::logic_error("malformed memo join expression");
        }
        return;
    case MemoExpressionKind::GroupRef:
        if (!expression.table.empty() || !expression.binding_name.empty() || !expression.projections.empty() ||
            !expression.group_keys.empty() || !expression.aggregate_expressions.empty() || !expression.sort_keys.empty() ||
            !expression.predicates.empty() || expression.limit_count != 0 || expression.children.size() != 1) {
            throw std::logic_error("malformed memo group reference expression");
        }
        return;
    }
    throw std::logic_error("unreachable memo expression kind");
}

void Memo::validate_no_cycle_from(GroupId group_id, const MemoExpression& expression) const {
    for (const auto child : expression.children) {
        if (child == group_id || reaches(child, group_id)) {
            throw std::logic_error("memo expression would create a cycle among groups");
        }
    }
}

void Memo::index_expression(const MemoExpression& expression, GroupId owner) {
    expression_index_[structural_hash(expression)].push_back({expression, representative(owner)});
}

void Memo::canonicalize_expression_references(MemoExpression& expression) const {
    for (auto& child : expression.children) {
        child = representative(child);
    }
}

void Memo::canonicalize_child_references() {
    for (auto& memo_group : groups_) {
        for (auto& expression : memo_group.expressions) {
            canonicalize_expression_references(expression.expression);
        }
    }
}

void Memo::deduplicate_group_expressions(GroupId group_id) {
    group_id = representative(group_id);
    auto& expressions = groups_.at(index_for(group_id)).expressions;
    std::vector<GroupExpression> unique;
    unique.reserve(expressions.size());
    for (auto& candidate : expressions) {
        const auto already_present =
            std::find_if(unique.begin(), unique.end(), [&](const auto& existing) {
                return structural_equal(existing.expression, candidate.expression);
            }) != unique.end();
        if (!already_present) {
            unique.push_back(std::move(candidate));
        }
    }
    if (unique.empty()) {
        throw std::logic_error("memo representative group cannot become empty");
    }
    expressions = std::move(unique);
}

void Memo::mark_non_representative_groups() {
    for (auto& memo_group : groups_) {
        const auto rep = representative(memo_group.id);
        if (rep == memo_group.id) {
            continue;
        }
        const auto order_permission = group(rep).expressions.front().expression.order_permission;
        memo_group.expressions = {GroupExpression{group_ref_expression(rep, order_permission)}};
    }
}

void Memo::rebuild_expression_index() {
    expression_index_.clear();
    for (const auto& memo_group : groups_) {
        if (!is_representative(memo_group.id)) {
            continue;
        }
        for (const auto& expression : memo_group.expressions) {
            const auto hash = structural_hash(expression.expression);
            auto& bucket = expression_index_[hash];
            for (const auto& candidate : bucket) {
                if (structural_equal(candidate.first, expression.expression) &&
                    representative(candidate.second) != memo_group.id) {
                    throw std::logic_error("memo structural duplicate remains across representative groups");
                }
            }
            bucket.push_back({expression.expression, memo_group.id});
        }
    }
}

bool Memo::merge_representatives_once(GroupId first, GroupId second) {
    first = representative(first);
    second = representative(second);
    if (first == second) {
        return false;
    }

    const auto winner = std::min(first, second);
    const auto loser = std::max(first, second);
    if (reaches(winner, loser) || reaches(loser, winner)) {
        throw std::logic_error("memo group merge would create a cycle");
    }

    auto& winner_group = groups_.at(index_for(winner));
    auto& loser_group = groups_.at(index_for(loser));
    winner_group.expressions.insert(winner_group.expressions.end(),
                                    std::make_move_iterator(loser_group.expressions.begin()),
                                    std::make_move_iterator(loser_group.expressions.end()));
    loser_group.expressions.clear();
    representatives_.at(static_cast<std::size_t>(loser)) = winner;
    return true;
}

void Memo::merge_groups(GroupId first, GroupId second) {
    if (!merge_representatives_once(first, second)) {
        return;
    }
    normalize_after_merge();
    assert_invariants();
}

void Memo::normalize_after_merge() {
    bool changed = false;
    do {
        changed = false;
        mark_non_representative_groups();
        canonicalize_child_references();
        for (const auto& memo_group : groups_) {
            if (is_representative(memo_group.id)) {
                deduplicate_group_expressions(memo_group.id);
            }
        }

        expression_index_.clear();
        for (const auto& memo_group : groups_) {
            if (!is_representative(memo_group.id)) {
                continue;
            }
            for (const auto& expression : memo_group.expressions) {
                validate_expression(expression.expression);
                validate_no_cycle_from(memo_group.id, expression.expression);
                const auto hash = structural_hash(expression.expression);
                auto& bucket = expression_index_[hash];
                for (const auto& candidate : bucket) {
                    const auto owner = representative(candidate.second);
                    if (owner != memo_group.id && structural_equal(candidate.first, expression.expression)) {
                        merge_representatives_once(owner, memo_group.id);
                        changed = true;
                        break;
                    }
                }
                if (changed) {
                    break;
                }
                bucket.push_back({expression.expression, memo_group.id});
            }
            if (changed) {
                break;
            }
        }
    } while (changed);

    mark_non_representative_groups();
    canonicalize_child_references();
    for (const auto& memo_group : groups_) {
        if (is_representative(memo_group.id)) {
            deduplicate_group_expressions(memo_group.id);
        }
    }
    rebuild_expression_index();
}

std::vector<plan::LogicalPlan> Memo::extract_alternatives_for_group(
    GroupId id,
    const AlternativeExtractionOptions& options,
    AlternativeExtractionResult& result,
    std::vector<GroupId>& stack) const {
    id = representative(id);
    if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
        throw std::logic_error("memo alternative extraction encountered a cycle");
    }

    const auto& memo_group = group(id);
    result.max_group_expression_count = std::max(result.max_group_expression_count, memo_group.expressions.size());
    const auto expression_count = std::min(options.max_expressions_per_group, memo_group.expressions.size());
    if (expression_count < memo_group.expressions.size()) {
        result.hit_expression_bound = true;
    }

    std::vector<plan::LogicalPlan> alternatives;
    stack.push_back(id);
    for (std::size_t i = 0; i < expression_count; ++i) {
        auto expression_alternatives =
            extract_alternatives_for_expression(memo_group.expressions[i].expression, options, result, stack);
        for (auto& alternative : expression_alternatives) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            alternatives.push_back(std::move(alternative));
        }
        if (alternatives.size() >= options.max_plans) {
            result.hit_plan_bound = true;
            break;
        }
    }
    stack.pop_back();
    return alternatives;
}

std::vector<plan::LogicalPlan> Memo::extract_alternatives_for_expression(
    const MemoExpression& expression,
    const AlternativeExtractionOptions& options,
    AlternativeExtractionResult& result,
    std::vector<GroupId>& stack) const {
    switch (expression.kind) {
    case MemoExpressionKind::Scan: {
        auto scan = plan::LogicalPlan::scan(expression.table, expression.binding_name);
        scan.order_permission = expression.order_permission;
        return {std::move(scan)};
    }
    case MemoExpressionKind::GroupRef:
        return extract_alternatives_for_group(expression.children.at(0), options, result, stack);
    case MemoExpressionKind::Filter: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto filter = plan::LogicalPlan::filter(expression.predicates, std::move(child));
            filter.order_permission = expression.order_permission;
            alternatives.push_back(std::move(filter));
        }
        return alternatives;
    }
    case MemoExpressionKind::Project: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto project = plan::LogicalPlan::project(expression.projections, std::move(child));
            project.order_permission = expression.order_permission;
            alternatives.push_back(std::move(project));
        }
        return alternatives;
    }
    case MemoExpressionKind::Aggregate: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto aggregate =
                plan::LogicalPlan::aggregate(expression.group_keys, expression.aggregate_expressions, std::move(child));
            aggregate.order_permission = expression.order_permission;
            alternatives.push_back(std::move(aggregate));
        }
        return alternatives;
    }
    case MemoExpressionKind::Distinct: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto distinct = plan::LogicalPlan::distinct(std::move(child));
            distinct.order_permission = expression.order_permission;
            alternatives.push_back(std::move(distinct));
        }
        return alternatives;
    }
    case MemoExpressionKind::Sort: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto sort = plan::LogicalPlan::sort(expression.sort_keys, std::move(child));
            sort.order_permission = expression.order_permission;
            alternatives.push_back(std::move(sort));
        }
        return alternatives;
    }
    case MemoExpressionKind::Limit: {
        auto children = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        alternatives.reserve(children.size());
        for (auto& child : children) {
            if (alternatives.size() >= options.max_plans) {
                result.hit_plan_bound = true;
                break;
            }
            auto limit = plan::LogicalPlan::limit(expression.limit_count, std::move(child));
            limit.order_permission = expression.order_permission;
            alternatives.push_back(std::move(limit));
        }
        return alternatives;
    }
    case MemoExpressionKind::Join: {
        auto left_plans = extract_alternatives_for_group(expression.children.at(0), options, result, stack);
        auto right_plans = extract_alternatives_for_group(expression.children.at(1), options, result, stack);
        std::vector<plan::LogicalPlan> alternatives;
        for (const auto& left : left_plans) {
            for (const auto& right : right_plans) {
                if (alternatives.size() >= options.max_plans) {
                    result.hit_plan_bound = true;
                    return alternatives;
                }
                auto join = plan::LogicalPlan::join(expression.predicates, left, right, expression.join_kind);
                join.order_permission = expression.order_permission;
                alternatives.push_back(std::move(join));
            }
        }
        return alternatives;
    }
    }
    throw std::logic_error("unreachable memo expression kind");
}

CostEstimate estimate_cost(const plan::LogicalPlan& logical, const catalog::Catalog& catalog) {
    const auto estimate = estimate_logical_relation(logical, catalog);
    return CostEstimate{estimate.rows, estimate.cost};
}

} // namespace optimizer
