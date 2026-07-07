#pragma once

#include "catalog/catalog.hpp"
#include "sql/ast.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace plan {

enum class LogicalKind { Scan, Join, Filter, Project, Aggregate, Distinct, Sort, Limit, Explain };
enum class OrderPermission { Deterministic, Arbitrary };

struct BoundColumnRef {
    std::string binding;
    std::string column;
    std::size_t position{0};
    catalog::ColumnType type{catalog::ColumnType::Int64};
};

struct BoundScalarExpr {
    using Value = std::variant<BoundColumnRef, sql::IntLiteral, sql::StringLiteral, sql::NullLiteral>;

    Value value;
    catalog::ColumnType type{catalog::ColumnType::Int64};

    BoundScalarExpr() = default;
    BoundScalarExpr(BoundColumnRef column) : value(std::move(column)), type(std::get<BoundColumnRef>(value).type) {}
    BoundScalarExpr(sql::IntLiteral literal) : value(literal), type(catalog::ColumnType::Int64) {}
    BoundScalarExpr(sql::StringLiteral literal) : value(std::move(literal)), type(catalog::ColumnType::String) {}
    BoundScalarExpr(sql::NullLiteral literal) : value(literal), type(catalog::ColumnType::Int64) {}
    BoundScalarExpr(Value value, catalog::ColumnType type) : value(std::move(value)), type(type) {}
};

struct BoundComparisonExpr {
    BoundScalarExpr left;
    sql::ComparisonOp op{sql::ComparisonOp::Equal};
    BoundScalarExpr right;
    std::size_t operator_position{0};
};

struct BoundPredicate {
    sql::PredicateKind kind{sql::PredicateKind::Comparison};
    BoundComparisonExpr comparison;
    BoundScalarExpr null_check;
    std::shared_ptr<BoundPredicate> left;
    std::shared_ptr<BoundPredicate> right;
    std::size_t operator_position{0};

    static BoundPredicate comparison_expr(BoundComparisonExpr comparison) {
        BoundPredicate predicate;
        predicate.kind = sql::PredicateKind::Comparison;
        predicate.comparison = std::move(comparison);
        return predicate;
    }

    static BoundPredicate null_check_expr(sql::PredicateKind kind,
                                          BoundScalarExpr expression,
                                          std::size_t position) {
        BoundPredicate predicate;
        predicate.kind = kind;
        predicate.null_check = std::move(expression);
        predicate.operator_position = position;
        return predicate;
    }

    static BoundPredicate binary(sql::PredicateKind kind,
                                 BoundPredicate left,
                                 BoundPredicate right,
                                 std::size_t position) {
        BoundPredicate predicate;
        predicate.kind = kind;
        predicate.left = std::make_shared<BoundPredicate>(std::move(left));
        predicate.right = std::make_shared<BoundPredicate>(std::move(right));
        predicate.operator_position = position;
        return predicate;
    }
};

struct Projection {
    std::string output_name;
    BoundScalarExpr expression;
    catalog::ColumnType type{catalog::ColumnType::Int64};
};

struct SortKey {
    BoundColumnRef column;
    sql::SortDirection direction{sql::SortDirection::Asc};
};

struct AggregateExpression {
    std::string output_name;
    sql::AggregateFunction function{sql::AggregateFunction::Count};
    std::optional<BoundColumnRef> argument;
    std::size_t position{0};
    catalog::ColumnType type{catalog::ColumnType::Int64};
};

struct LogicalPlan {
    LogicalKind kind{LogicalKind::Scan};
    OrderPermission order_permission{OrderPermission::Deterministic};
    // Scan nodes read the physical table named by `table` and emit bound
    // column identities qualified by `binding_name`.
    std::string table;
    std::string binding_name;
    std::vector<Projection> projections;
    std::vector<BoundColumnRef> group_keys;
    std::vector<AggregateExpression> aggregate_expressions;
    std::vector<SortKey> sort_keys;
    std::vector<BoundPredicate> predicates;
    std::size_t limit_count{0};
    std::shared_ptr<LogicalPlan> input;
    std::shared_ptr<LogicalPlan> left;
    std::shared_ptr<LogicalPlan> right;

    static LogicalPlan scan(std::string table) {
        return scan(table, table);
    }

    static LogicalPlan scan(std::string table, std::string binding_name) {
        LogicalPlan p;
        p.kind = LogicalKind::Scan;
        p.table = std::move(table);
        p.binding_name = std::move(binding_name);
        return p;
    }

    // Join output identity/order is deterministic: all columns from the left
    // child followed by all columns from the right child. Expressions in a
    // bound logical plan refer to those identities by binding and column name;
    // downstream layers must not re-resolve parsed SQL names.
    static LogicalPlan join(std::vector<BoundPredicate> predicates, LogicalPlan left, LogicalPlan right) {
        LogicalPlan p;
        p.kind = LogicalKind::Join;
        p.predicates = std::move(predicates);
        p.left = std::make_shared<LogicalPlan>(std::move(left));
        p.right = std::make_shared<LogicalPlan>(std::move(right));
        return p;
    }

    static LogicalPlan filter(std::vector<BoundPredicate> predicates, LogicalPlan child) {
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

    static LogicalPlan aggregate(std::vector<BoundColumnRef> group_keys,
                                 std::vector<AggregateExpression> aggregate_expressions,
                                 LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Aggregate;
        p.group_keys = std::move(group_keys);
        p.aggregate_expressions = std::move(aggregate_expressions);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan distinct(LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Distinct;
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan sort(std::vector<SortKey> sort_keys, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Sort;
        p.sort_keys = std::move(sort_keys);
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan limit(std::size_t count, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Limit;
        p.limit_count = count;
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }

    static LogicalPlan explain(LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Explain;
        p.input = std::make_shared<LogicalPlan>(std::move(child));
        return p;
    }
};

[[nodiscard]] std::string to_string(const LogicalPlan& logical);
[[nodiscard]] std::string to_string_annotated(
    const LogicalPlan& logical,
    const std::function<std::string(const LogicalPlan&)>& annotation);

} // namespace plan
