#pragma once

#include "sql/ast.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace plan {

enum class LogicalKind { Scan, Join, Filter, Project };
enum class OrderPermission { Deterministic, Arbitrary };

struct BoundColumnRef {
    std::string table;
    std::string column;
    std::size_t position{0};
};

using BoundScalarExpr = std::variant<BoundColumnRef, sql::IntLiteral>;

struct BoundComparisonExpr {
    BoundScalarExpr left;
    sql::ComparisonOp op{sql::ComparisonOp::Equal};
    BoundScalarExpr right;
    std::size_t operator_position{0};
};

struct Projection {
    std::string output_name;
    BoundScalarExpr expression;
};

struct LogicalPlan {
    LogicalKind kind{LogicalKind::Scan};
    OrderPermission order_permission{OrderPermission::Deterministic};
    std::string table;
    std::vector<Projection> projections;
    std::vector<BoundComparisonExpr> predicates;
    std::shared_ptr<LogicalPlan> input;
    std::shared_ptr<LogicalPlan> left;
    std::shared_ptr<LogicalPlan> right;

    static LogicalPlan scan(std::string table) {
        LogicalPlan p;
        p.kind = LogicalKind::Scan;
        p.table = std::move(table);
        return p;
    }

    // Join output identity/order is deterministic: all columns from the left
    // child followed by all columns from the right child. Expressions in a
    // bound logical plan refer to those identities by table and column name;
    // downstream layers must not re-resolve parsed SQL names.
    static LogicalPlan join(std::vector<BoundComparisonExpr> predicates, LogicalPlan left, LogicalPlan right) {
        LogicalPlan p;
        p.kind = LogicalKind::Join;
        p.predicates = std::move(predicates);
        p.left = std::make_shared<LogicalPlan>(std::move(left));
        p.right = std::make_shared<LogicalPlan>(std::move(right));
        return p;
    }

    static LogicalPlan filter(std::vector<BoundComparisonExpr> predicates, LogicalPlan child) {
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

[[nodiscard]] std::string to_string(const LogicalPlan& logical);

} // namespace plan
