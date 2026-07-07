#include "plan/physical_plan.hpp"

#include <stdexcept>

namespace plan {
namespace {

constexpr const char* kStringNotSupported =
    "vectorized execution does not support VARCHAR/string plans in phase 17a";

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

bool contains_string_scalar(const BoundScalarExpr& expression) {
    return expression.type == catalog::ColumnType::String;
}

bool contains_string_predicate(const BoundPredicate& predicate) {
    switch (predicate.kind) {
    case sql::PredicateKind::Comparison:
        return contains_string_scalar(predicate.comparison.left) || contains_string_scalar(predicate.comparison.right);
    case sql::PredicateKind::IsNull:
    case sql::PredicateKind::IsNotNull:
        return contains_string_scalar(predicate.null_check);
    case sql::PredicateKind::And:
    case sql::PredicateKind::Or:
        if (predicate.left == nullptr || predicate.right == nullptr) {
            throw std::logic_error("bound predicate is missing a child");
        }
        return contains_string_predicate(*predicate.left) || contains_string_predicate(*predicate.right);
    }
    throw std::logic_error("unreachable predicate kind");
}

bool contains_string_plan(const LogicalPlan& logical) {
    for (const auto& projection : logical.projections) {
        if (projection.type == catalog::ColumnType::String || contains_string_scalar(projection.expression)) {
            return true;
        }
    }
    for (const auto& key : logical.group_keys) {
        if (key.type == catalog::ColumnType::String) {
            return true;
        }
    }
    for (const auto& aggregate : logical.aggregate_expressions) {
        if (aggregate.type == catalog::ColumnType::String ||
            (aggregate.argument.has_value() && aggregate.argument->type == catalog::ColumnType::String)) {
            return true;
        }
    }
    for (const auto& key : logical.sort_keys) {
        if (key.column.type == catalog::ColumnType::String) {
            return true;
        }
    }
    for (const auto& predicate : logical.predicates) {
        if (contains_string_predicate(predicate)) {
            return true;
        }
    }

    switch (logical.kind) {
    case LogicalKind::Scan:
        return false;
    case LogicalKind::Join:
        return contains_string_plan(require_left(logical)) || contains_string_plan(require_right(logical));
    case LogicalKind::Filter:
    case LogicalKind::Project:
    case LogicalKind::Aggregate:
    case LogicalKind::Distinct:
    case LogicalKind::Sort:
    case LogicalKind::Limit:
        return contains_string_plan(require_input(logical));
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace

PhysicalPlan lower_to_physical(const LogicalPlan& logical) {
    if (contains_string_plan(logical)) {
        throw std::runtime_error(kStringNotSupported);
    }
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table, logical.binding_name);
    case LogicalKind::Join:
        return PhysicalPlan::join(logical.predicates,
                                  lower_to_physical(require_left(logical)),
                                  lower_to_physical(require_right(logical)));
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
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace plan
