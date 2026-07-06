#pragma once

#include "plan/logical_plan.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plan {

enum class PhysicalKind { Scan, Filter, Project };

struct PhysicalPlan {
    PhysicalKind kind{PhysicalKind::Scan};
    std::string table;
    std::vector<Projection> projections;
    std::vector<BoundComparisonExpr> predicates;
    std::shared_ptr<PhysicalPlan> input;

    static PhysicalPlan scan(std::string table) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Scan;
        p.table = std::move(table);
        return p;
    }

    static PhysicalPlan filter(std::vector<BoundComparisonExpr> predicates, PhysicalPlan child) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Filter;
        p.predicates = std::move(predicates);
        p.input = std::make_shared<PhysicalPlan>(std::move(child));
        return p;
    }

    static PhysicalPlan project(std::vector<Projection> projections, PhysicalPlan child) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Project;
        p.projections = std::move(projections);
        p.input = std::make_shared<PhysicalPlan>(std::move(child));
        return p;
    }
};

PhysicalPlan lower_to_physical(const LogicalPlan& logical);

} // namespace plan
