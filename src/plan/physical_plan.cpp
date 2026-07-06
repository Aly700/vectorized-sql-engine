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
    case LogicalKind::Sort:
        return PhysicalPlan::sort(logical.sort_keys, lower_to_physical(require_input(logical)));
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace plan
