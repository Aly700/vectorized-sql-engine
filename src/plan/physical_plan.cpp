#include "plan/physical_plan.hpp"

#include <stdexcept>
#include <variant>

namespace plan {
namespace {

bool scalar_contains_subquery(const BoundScalarExpr& expression) {
    return std::holds_alternative<BoundScalarSubquery>(expression.value);
}

bool predicate_contains_subquery(const BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return scalar_contains_subquery(predicate.comparison.left) ||
               scalar_contains_subquery(predicate.comparison.right);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return scalar_contains_subquery(predicate.null_check);
    case sql::PredicateKind::In:
    case sql::PredicateKind::NotIn:
    case sql::PredicateKind::Exists:
    case sql::PredicateKind::NotExists:
        return true;
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::invalid_argument("bound predicate is missing a child");
        }
        return predicate_contains_subquery(*predicate.left) || predicate_contains_subquery(*predicate.right);
    }
    throw std::logic_error("unreachable predicate kind");
}

bool contains_subquery(const LogicalPlan& logical) {
    for (const auto& projection : logical.projections) {
        if (scalar_contains_subquery(projection.expression)) {
            return true;
        }
    }
    for (const auto& predicate : logical.predicates) {
        if (predicate_contains_subquery(predicate)) {
            return true;
        }
    }
    return (logical.input != nullptr && contains_subquery(*logical.input)) ||
           (logical.left != nullptr && contains_subquery(*logical.left)) ||
           (logical.right != nullptr && contains_subquery(*logical.right));
}

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

} // namespace

PhysicalPlan lower_to_physical(const LogicalPlan& logical) {
    if (contains_subquery(logical)) {
        throw std::invalid_argument(kVectorizedSubqueryNotSupported);
    }
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table, logical.binding_name);
    case LogicalKind::Join:
        return PhysicalPlan::join(logical.predicates,
                                  lower_to_physical(require_left(logical)),
                                  lower_to_physical(require_right(logical)),
                                  logical.join_kind);
    case LogicalKind::Filter:
        return PhysicalPlan::filter(logical.predicates, lower_to_physical(require_input(logical)));
    case LogicalKind::Project:
        return PhysicalPlan::project(logical.projections, lower_to_physical(require_input(logical)));
    case LogicalKind::Aggregate:
        return PhysicalPlan::aggregate(logical.group_keys,
                                       logical.aggregate_expressions,
                                       lower_to_physical(require_input(logical)));
    case LogicalKind::Distinct:
        return PhysicalPlan::distinct(lower_to_physical(require_input(logical)));
    case LogicalKind::Sort:
        return PhysicalPlan::sort(logical.sort_keys, lower_to_physical(require_input(logical)));
    case LogicalKind::Limit:
        return PhysicalPlan::limit(logical.limit_count, lower_to_physical(require_input(logical)));
    case LogicalKind::Explain:
        throw std::invalid_argument("EXPLAIN logical plans cannot be lowered to physical plans");
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace plan
