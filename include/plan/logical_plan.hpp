#pragma once

#include "catalog/catalog.hpp"
#include "sql/ast.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace plan {

struct LogicalPlan;

enum class LogicalKind { Scan, Join, Filter, Project, Aggregate, Window, Distinct, Sort, Limit, Explain };
enum class OrderPermission { Deterministic, Arbitrary };
enum class JoinKind { Inner, Left, Semi, Anti, NullAwareAnti };

struct BoundColumnRef {
    std::string binding;
    std::string column;
    std::size_t position{0};
    catalog::ColumnType type{catalog::ColumnType::Int64};
    // Zero identifies the current query block. Positive values identify the
    // lexical distance to an enclosing query block (1 = immediate parent).
    std::size_t outer_depth{0};
};

struct BoundScalarSubquery {
    std::shared_ptr<const LogicalPlan> plan;
    std::size_t position{0};
    std::string name;
};

struct BoundScalarExpr {
    using Value = std::variant<BoundColumnRef,
                               sql::IntLiteral,
                               sql::StringLiteral,
                               sql::NullLiteral,
                               BoundScalarSubquery>;

    Value value;
    catalog::ColumnType type{catalog::ColumnType::Int64};

    BoundScalarExpr() = default;
    BoundScalarExpr(BoundColumnRef column) : value(std::move(column)), type(std::get<BoundColumnRef>(value).type) {}
    BoundScalarExpr(sql::IntLiteral literal) : value(literal), type(catalog::ColumnType::Int64) {}
    BoundScalarExpr(sql::StringLiteral literal) : value(std::move(literal)), type(catalog::ColumnType::String) {}
    BoundScalarExpr(sql::NullLiteral literal) : value(literal), type(catalog::ColumnType::Int64) {}
    BoundScalarExpr(BoundScalarSubquery subquery, catalog::ColumnType type)
        : value(std::move(subquery)), type(type) {}
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
    BoundScalarExpr in_value;
    std::shared_ptr<const LogicalPlan> subquery;
    std::string subquery_name;
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

    static BoundPredicate in_expr(sql::PredicateKind kind,
                                  BoundScalarExpr value,
                                  std::shared_ptr<const LogicalPlan> subquery,
                                  std::size_t position,
                                  std::string name) {
        BoundPredicate predicate;
        predicate.kind = kind;
        predicate.in_value = std::move(value);
        predicate.subquery = std::move(subquery);
        predicate.operator_position = position;
        predicate.subquery_name = std::move(name);
        return predicate;
    }

    static BoundPredicate exists_expr(sql::PredicateKind kind,
                                      std::shared_ptr<const LogicalPlan> subquery,
                                      std::size_t position,
                                      std::string name) {
        BoundPredicate predicate;
        predicate.kind = kind;
        predicate.subquery = std::move(subquery);
        predicate.operator_position = position;
        predicate.subquery_name = std::move(name);
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

struct WindowExpression {
    std::string output_name;
    sql::WindowFunction function{sql::WindowFunction::RowNumber};
    bool count_star{false};
    std::optional<BoundColumnRef> argument;
    std::vector<BoundColumnRef> partition_keys;
    std::vector<SortKey> order_keys;
    sql::WindowFrame frame{sql::WindowFrame::WholePartition};
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
    std::vector<WindowExpression> window_expressions;
    std::vector<SortKey> sort_keys;
    std::vector<BoundPredicate> predicates;
    // Structural only for NullAwareAnti. `predicates` identify the candidate
    // right rows; this equality carries NOT IN's distinct UNKNOWN semantics.
    std::optional<BoundPredicate> null_aware_predicate;
    // Set only on a bound subplan root. Empty is the structural Phase 21a
    // dispatch; non-empty means execution requires the listed outer values.
    std::vector<BoundColumnRef> correlation_columns;
    JoinKind join_kind{JoinKind::Inner};
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

    // INNER/LEFT output identity/order is deterministic: all columns from the
    // left child followed by all columns from the right child. SEMI/ANTI and
    // NULL-aware ANTI emit exactly the left child's identities and order;
    // right identities exist only while evaluating join predicates.
    // Expressions in a bound logical plan refer to identities by binding and
    // column name; downstream layers must not re-resolve parsed SQL names.
    static LogicalPlan join(std::vector<BoundPredicate> predicates,
                            LogicalPlan left,
                            LogicalPlan right,
                            JoinKind join_kind = JoinKind::Inner) {
        if (join_kind == JoinKind::NullAwareAnti) {
            throw std::invalid_argument(
                "NullAwareAnti must be constructed with an explicit membership equality");
        }
        LogicalPlan p;
        p.kind = LogicalKind::Join;
        p.join_kind = join_kind;
        p.predicates = std::move(predicates);
        p.left = std::make_shared<LogicalPlan>(std::move(left));
        p.right = std::make_shared<LogicalPlan>(std::move(right));
        return p;
    }

    static LogicalPlan null_aware_anti(BoundPredicate membership,
                                       LogicalPlan left,
                                       LogicalPlan right,
                                       std::vector<BoundPredicate> candidate_predicates = {}) {
        if (membership.kind != sql::PredicateKind::Comparison ||
            membership.comparison.op != sql::ComparisonOp::Equal) {
            throw std::invalid_argument("NullAwareAnti membership predicate must be an equality");
        }
        if (membership.comparison.left.type != membership.comparison.right.type) {
            throw std::invalid_argument("NullAwareAnti membership equality must have matching types");
        }
        LogicalPlan p;
        p.kind = LogicalKind::Join;
        p.join_kind = JoinKind::NullAwareAnti;
        p.predicates = std::move(candidate_predicates);
        p.null_aware_predicate = std::move(membership);
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

    static LogicalPlan window(std::vector<WindowExpression> window_expressions, LogicalPlan child) {
        LogicalPlan p;
        p.kind = LogicalKind::Window;
        p.window_expressions = std::move(window_expressions);
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
