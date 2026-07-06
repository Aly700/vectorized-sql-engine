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

} // namespace

PhysicalPlan lower_to_physical(const LogicalPlan& logical) {
    switch (logical.kind) {
    case LogicalKind::Scan:
        return PhysicalPlan::scan(logical.table);
    case LogicalKind::Join:
        throw std::logic_error("vectorized inner join is not supported yet");
    case LogicalKind::Filter:
        return PhysicalPlan::filter(logical.predicates, lower_to_physical(require_input(logical)));
    case LogicalKind::Project:
        return PhysicalPlan::project(logical.projections, lower_to_physical(require_input(logical)));
    }
    throw std::logic_error("unreachable logical plan kind");
}

} // namespace plan
