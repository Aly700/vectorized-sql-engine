#pragma once

#include "sql/ast.hpp"

#include <memory>
#include <string>
#include <vector>

namespace plan {

enum class LogicalKind { Scan, Filter, Project };

struct LogicalPlan {
    LogicalKind kind{LogicalKind::Scan};
    std::string table;
    std::vector<std::string> columns;
    sql::Predicate predicate;
    std::shared_ptr<LogicalPlan> input;

    static LogicalPlan scan(std::string table) {
        LogicalPlan p;
        p.kind = LogicalKind::Scan;
        p.table = std::move(table);
        return p;
    }

    static LogicalPlan filter(sql::Predicate pred, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Filter;
        p.predicate = std::move(pred);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan project(std::vector<std::string> columns, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Project;
        p.columns = std::move(columns);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }
};

inline LogicalPlan lower_to_logical(const sql::SelectQuery& query) {
    auto plan = LogicalPlan::scan(query.table);
    if (query.predicate.has_value()) {
        plan = LogicalPlan::filter(*query.predicate, std::move(plan));
    }
    return LogicalPlan::project(query.projection, std::move(plan));
}

} // namespace plan
