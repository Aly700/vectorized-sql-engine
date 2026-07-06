#pragma once

#include "sql/ast.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plan {

enum class LogicalKind { Scan, Filter, Project };

struct Projection {
    std::string output_name;
    sql::ScalarExpr expression;
};

struct LogicalPlan {
    LogicalKind kind{LogicalKind::Scan};
    std::string table;
    std::vector<Projection> projections;
    std::vector<sql::ComparisonExpr> predicates;
    std::shared_ptr<LogicalPlan> input;

    static LogicalPlan scan(std::string table) {
        LogicalPlan p;
        p.kind = LogicalKind::Scan;
        p.table = std::move(table);
        return p;
    }

    static LogicalPlan filter(std::vector<sql::ComparisonExpr> predicates, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Filter;
        p.predicates = std::move(predicates);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan project(std::vector<Projection> projections, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Project;
        p.projections = std::move(projections);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }
};

} // namespace plan
