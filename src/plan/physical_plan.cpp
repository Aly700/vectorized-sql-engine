#include "plan/physical_plan.hpp"

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

bool scalar_has_residual_correlation(const BoundScalarExpr& expression);

bool predicate_has_residual_correlation(const BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return scalar_has_residual_correlation(predicate.comparison.left) ||
               scalar_has_residual_correlation(predicate.comparison.right);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_has_residual_correlation(predicate.null_check);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
        return scalar_has_residual_correlation(predicate.in_value) || predicate.subquery == nullptr ||
               !predicate.subquery->correlation_columns.empty();
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        return predicate.subquery == nullptr || !predicate.subquery->correlation_columns.empty();
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return predicate_has_residual_correlation(*predicate.left) ||
               predicate_has_residual_correlation(*predicate.right);
    }
    throw std::logic_error("unreachable predicate kind");
}

bool logical_has_residual_correlation(const LogicalPlan& logical);

bool scalar_has_residual_correlation(const BoundScalarExpr& expression) {
    if (const auto* column = std::get_if<BoundColumnRef>(&expression.value)) {
        return column->outer_depth != 0;
    }
    if (const auto* subquery = std::get_if<BoundScalarSubquery>(&expression.value)) {
        return subquery->plan == nullptr || logical_has_residual_correlation(*subquery->plan);
    }
    return false;
}

bool logical_has_residual_correlation(const LogicalPlan& logical) {
    if (!logical.correlation_columns.empty()) {
        return true;
    }
    for (const auto& projection : logical.projections) {
        if (scalar_has_residual_correlation(projection.expression)) {
            return true;
        }
    }
    for (const auto& predicate : logical.predicates) {
        if (predicate_has_residual_correlation(predicate)) {
            return true;
        }
        if ((predicate.kind == sql::PredicateKind::In || predicate.kind == sql::PredicateKind::NotIn ||
             predicate.kind == sql::PredicateKind::Exists || predicate.kind == sql::PredicateKind::NotExists) &&
            predicate.subquery != nullptr && logical_has_residual_correlation(*predicate.subquery)) {
            return true;
        }
    }
    return (logical.input != nullptr && logical_has_residual_correlation(*logical.input)) ||
           (logical.left != nullptr && logical_has_residual_correlation(*logical.left)) ||
           (logical.right != nullptr && logical_has_residual_correlation(*logical.right));
}

PhysicalPlan lower_impl(const LogicalPlan& logical) {
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table, logical.binding_name);
    case LogicalKind::Join:
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
    if (logical_has_residual_correlation(logical)) {
        throw std::runtime_error("vectorized execution does not support residual correlated subqueries");
    }
    return lower_impl(logical);
}

} // namespace plan
