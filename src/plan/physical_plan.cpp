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

} // namespace

PhysicalPlan lower_to_physical(const LogicalPlan& logical) {
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table, logical.binding_name);
    case LogicalKind::Join:
        if (logical.join_kind != JoinKind::Inner) {
            throw std::runtime_error("physical lowering does not support outer joins in phase 20a");
        }
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
