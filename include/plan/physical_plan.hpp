#pragma once

#include "plan/logical_plan.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plan {

enum class PhysicalKind { Scan, Join, Filter, Project, Sort };

struct PhysicalPlan {
    PhysicalKind kind{PhysicalKind::Scan};
    // Physical scans read `table` and expose output identities under
    // `binding_name`, matching the bound logical scan.
    std::string table;
    std::string binding_name;
    std::vector<Projection> projections;
    std::vector<SortKey> sort_keys;
    std::vector<BoundComparisonExpr> predicates;
    std::shared_ptr<PhysicalPlan> input;
    std::shared_ptr<PhysicalPlan> left;
    std::shared_ptr<PhysicalPlan> right;

    static PhysicalPlan scan(std::string table) {
        return scan(table, table);
    }

    static PhysicalPlan scan(std::string table, std::string binding_name) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Scan;
        p.table = std::move(table);
        p.binding_name = std::move(binding_name);
        return p;
    }

    static PhysicalPlan filter(std::vector<BoundComparisonExpr> predicates, PhysicalPlan child) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Filter;
        p.predicates = std::move(predicates);
        p.input = std::make_shared<PhysicalPlan>(std::move(child));
        return p;
    }

    static PhysicalPlan join(std::vector<BoundComparisonExpr> predicates,
                             PhysicalPlan left,
                             PhysicalPlan right) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Join;
        p.predicates = std::move(predicates);
        p.left = std::make_shared<PhysicalPlan>(std::move(left));
        p.right = std::make_shared<PhysicalPlan>(std::move(right));
        return p;
    }

    static PhysicalPlan project(std::vector<Projection> projections, PhysicalPlan child) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Project;
        p.projections = std::move(projections);
        p.input = std::make_shared<PhysicalPlan>(std::move(child));
        return p;
    }

    static PhysicalPlan sort(std::vector<SortKey> sort_keys, PhysicalPlan child) {
        PhysicalPlan p;
        p.kind = PhysicalKind::Sort;
        p.sort_keys = std::move(sort_keys);
        p.input = std::make_shared<PhysicalPlan>(std::move(child));
        return p;
    }
};

PhysicalPlan lower_to_physical(const LogicalPlan& logical);

} // namespace plan
