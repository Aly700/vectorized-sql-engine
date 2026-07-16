#include "plan/physical_plan.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace plan {
namespace {

const LogicalPlan& require_input(const LogicalPlan& logical) {
    if (!logical.input) {
        throw std::invalid_argument("logical plan node is missing its input");
    }
    return *logical.input;
}

const LogicalPlan& require_left(const LogicalPlan& logical) {
    if (!logical.left) {
        throw std::invalid_argument("logical join node is missing its left input");
    }
    return *logical.left;
}

const LogicalPlan& require_right(const LogicalPlan& logical) {
    if (!logical.right) {
        throw std::invalid_argument("logical join node is missing its right input");
    }
    return *logical.right;
}

std::optional<std::size_t> logical_residual_correlation_position(const LogicalPlan& logical);

std::optional<std::size_t> earlier_position(std::optional<std::size_t> left,
                                            std::optional<std::size_t> right) {
    if (!left.has_value()) {
        return right;
    }
    if (!right.has_value()) {
        return left;
    }
    return std::min(*left, *right);
}

std::optional<std::size_t> scalar_residual_correlation_position(const BoundScalarExpr& expression);

std::optional<std::size_t> predicate_residual_correlation_position(const BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return earlier_position(scalar_residual_correlation_position(predicate.comparison.left),
                                scalar_residual_correlation_position(predicate.comparison.right));
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_residual_correlation_position(predicate.null_check);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn: {
        auto position = scalar_residual_correlation_position(predicate.in_value);
        if (predicate.subquery == nullptr ||
            logical_residual_correlation_position(*predicate.subquery).has_value()) {
            position = earlier_position(position, predicate.operator_position);
        }
        return position;
    }
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists: {
        if (predicate.subquery == nullptr ||
            logical_residual_correlation_position(*predicate.subquery).has_value()) {
            return predicate.operator_position;
        }
        return std::nullopt;
    }
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return earlier_position(predicate_residual_correlation_position(*predicate.left),
                                predicate_residual_correlation_position(*predicate.right));
    }
    throw std::logic_error("unreachable predicate kind");
}

std::optional<std::size_t> scalar_residual_correlation_position(const BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<BoundColumnRef>(&expression.value)) {
        return column->outer_depth != 0 ? std::optional<std::size_t>{column->position} : std::nullopt;
    }
    if (const auto* subquery = std::get_if<BoundScalarSubquery>(&expression.value)) {
        if (subquery->plan == nullptr ||
            logical_residual_correlation_position(*subquery->plan).has_value()) {
            return subquery->position;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> logical_residual_correlation_position(const LogicalPlan& logical) {
    std::optional<std::size_t> position;
    for (const auto& column : logical.correlation_columns) {
        position = earlier_position(position, column.position);
    }
    for (const auto& projection : logical.projections) {
        position = earlier_position(position,
                                    scalar_residual_correlation_position(projection.expression));
    }
    for (const auto& predicate : logical.predicates) {
        position = earlier_position(position, predicate_residual_correlation_position(predicate));
    }
    if (logical.null_aware_predicate.has_value()) {
        position = earlier_position(
            position, predicate_residual_correlation_position(*logical.null_aware_predicate));
    }
    if (logical.input != nullptr) {
        position = earlier_position(position, logical_residual_correlation_position(*logical.input));
    }
    if (logical.left != nullptr) {
        position = earlier_position(position, logical_residual_correlation_position(*logical.left));
    }
    if (logical.right != nullptr) {
        position = earlier_position(position, logical_residual_correlation_position(*logical.right));
    }
    return position;
}

PhysicalPlan lower_impl(const LogicalPlan& logical) {
    if (logical.kind != LogicalKind::Join && logical.null_aware_predicate.has_value()) {
        throw std::invalid_argument(
            "non-join logical plan node owns a NullAwareAnti membership equality");
    }
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table, logical.binding_name);
    case LogicalKind::Join:
        if (logical.join_kind == JoinKind::NullAwareAnti) {
            if (!logical.null_aware_predicate.has_value()) {
                throw std::invalid_argument("NullAwareAnti join is missing its membership equality");
            }
            return PhysicalPlan::null_aware_anti(*logical.null_aware_predicate,
                                                 lower_impl(require_left(logical)),
                                                 lower_impl(require_right(logical)),
                                                 logical.predicates);
        }
        if (logical.null_aware_predicate.has_value()) {
            throw std::invalid_argument("non-NullAwareAnti join owns a membership equality");
        }
        return PhysicalPlan::join(logical.predicates,
                                  lower_impl(require_left(logical)),
                                  lower_impl(require_right(logical)),
                                  logical.join_kind);
    case LogicalKind::Filter:
        return PhysicalPlan::filter(logical.predicates, lower_impl(require_input(logical)));
    case LogicalKind::Project:
        return PhysicalPlan::project(logical.projections, lower_impl(require_input(logical)));
    case LogicalKind::Aggregate:
        return PhysicalPlan::aggregate(logical.group_keys,
                                       logical.aggregate_expressions,
                                       lower_impl(require_input(logical)));
    case LogicalKind::Window:
        return PhysicalPlan::window(logical.window_expressions, lower_impl(require_input(logical)));
    case LogicalKind::Distinct:
        return PhysicalPlan::distinct(lower_impl(require_input(logical)));
    case LogicalKind::Sort:
        return PhysicalPlan::sort(logical.sort_keys, lower_impl(require_input(logical)));
    case LogicalKind::Limit:
        return PhysicalPlan::limit(logical.limit_count, lower_impl(require_input(logical)));
    case LogicalKind::Explain:
        throw std::invalid_argument("EXPLAIN logical plans cannot be lowered to physical plans");
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace

PhysicalPlan lower_to_physical(const LogicalPlan& logical) {
    if (const auto position = logical_residual_correlation_position(logical); position.has_value()) {
        throw std::runtime_error(
            "vectorized execution does not support residual correlated subqueries at position " +
            std::to_string(*position));
    }
    return lower_impl(logical);
}

} // namespace plan
