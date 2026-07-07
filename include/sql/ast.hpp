#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sql {

struct ColumnRef {
    std::optional<std::string> qualifier;
    std::string name;
    std::size_t position{0};
};

struct IntLiteral {
    std::int64_t value{0};
    std::size_t position{0};
};

struct NullLiteral {
    std::size_t position{0};
};

struct StringLiteral {
    std::string value;
    std::size_t position{0};
};

using ScalarExpr = std::variant<ColumnRef, IntLiteral, StringLiteral, NullLiteral>;

enum class AggregateFunction { Count, Sum, Min, Max };

struct AggregateCall {
    AggregateFunction function{AggregateFunction::Count};
    std::size_t position{0};
    bool count_star{false};
    std::optional<ColumnRef> argument;
    bool nested_aggregate{false};
    AggregateFunction nested_function{AggregateFunction::Count};
    std::size_t nested_position{0};
};

using SelectExpr = std::variant<ScalarExpr, AggregateCall>;
using HavingExpr = std::variant<ColumnRef, IntLiteral, StringLiteral, NullLiteral, AggregateCall>;
using OrderByExpr = std::variant<ColumnRef, AggregateCall>;

enum class ComparisonOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct ComparisonExpr {
    ScalarExpr left;
    ComparisonOp op{ComparisonOp::Equal};
    ScalarExpr right;
    std::size_t operator_position{0};
};

enum class PredicateKind { Comparison, IsNull, IsNotNull, And, Or };

struct PredicateExpr {
    PredicateKind kind{PredicateKind::Comparison};
    ComparisonExpr comparison;
    ScalarExpr null_check;
    std::shared_ptr<PredicateExpr> left;
    std::shared_ptr<PredicateExpr> right;
    std::size_t operator_position{0};
    bool parenthesized{false};

    static PredicateExpr comparison_expr(ComparisonExpr comparison) {
        PredicateExpr predicate;
        predicate.kind = PredicateKind::Comparison;
        predicate.comparison = std::move(comparison);
        return predicate;
    }

    static PredicateExpr null_check_expr(PredicateKind kind, ScalarExpr expression, std::size_t position) {
        PredicateExpr predicate;
        predicate.kind = kind;
        predicate.null_check = std::move(expression);
        predicate.operator_position = position;
        return predicate;
    }

    static PredicateExpr binary(PredicateKind kind, PredicateExpr left, PredicateExpr right, std::size_t position) {
        PredicateExpr predicate;
        predicate.kind = kind;
        predicate.left = std::make_shared<PredicateExpr>(std::move(left));
        predicate.right = std::make_shared<PredicateExpr>(std::move(right));
        predicate.operator_position = position;
        return predicate;
    }
};

struct WhereClause {
    std::vector<PredicateExpr> conjuncts;
};

struct HavingComparisonExpr {
    HavingExpr left;
    ComparisonOp op{ComparisonOp::Equal};
    HavingExpr right;
    std::size_t operator_position{0};
};

struct HavingPredicateExpr {
    PredicateKind kind{PredicateKind::Comparison};
    HavingComparisonExpr comparison;
    HavingExpr null_check;
    std::shared_ptr<HavingPredicateExpr> left;
    std::shared_ptr<HavingPredicateExpr> right;
    std::size_t operator_position{0};
    bool parenthesized{false};

    static HavingPredicateExpr comparison_expr(HavingComparisonExpr comparison) {
        HavingPredicateExpr predicate;
        predicate.kind = PredicateKind::Comparison;
        predicate.comparison = std::move(comparison);
        return predicate;
    }

    static HavingPredicateExpr null_check_expr(PredicateKind kind, HavingExpr expression, std::size_t position) {
        HavingPredicateExpr predicate;
        predicate.kind = kind;
        predicate.null_check = std::move(expression);
        predicate.operator_position = position;
        return predicate;
    }

    static HavingPredicateExpr binary(PredicateKind kind,
                                      HavingPredicateExpr left,
                                      HavingPredicateExpr right,
                                      std::size_t position) {
        HavingPredicateExpr predicate;
        predicate.kind = kind;
        predicate.left = std::make_shared<HavingPredicateExpr>(std::move(left));
        predicate.right = std::make_shared<HavingPredicateExpr>(std::move(right));
        predicate.operator_position = position;
        return predicate;
    }
};

struct HavingClause {
    std::size_t position{0};
    std::vector<HavingPredicateExpr> conjuncts;
};

enum class SortDirection { Asc, Desc };

struct OrderByKey {
    OrderByExpr expression;
    SortDirection direction{SortDirection::Asc};
};

struct SelectItem {
    SelectExpr expression;
    std::size_t position{0};
    std::optional<std::string> alias;
    std::size_t alias_position{0};
};

struct JoinClause {
    std::string table;
    std::size_t table_position{0};
    std::optional<std::string> alias;
    std::size_t alias_position{0};
    std::vector<PredicateExpr> predicates;
};

struct SelectQuery {
    bool explain{false};
    bool distinct{false};
    std::vector<SelectItem> projection;
    std::string table;
    std::size_t table_position{0};
    std::optional<std::string> alias;
    std::size_t alias_position{0};
    std::vector<JoinClause> joins;
    std::optional<WhereClause> predicate;
    std::vector<ColumnRef> group_by;
    std::optional<HavingClause> having;
    std::vector<OrderByKey> order_by;
    std::optional<std::size_t> limit;
};

SelectQuery parse_select(const std::string& sql);

inline std::size_t expression_position(const ScalarExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return column->position;
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return literal->position;
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return literal->position;
    }
    return std::get<NullLiteral>(expression).position;
}

inline std::size_t expression_position(const SelectExpr& expression) {
    if (const auto* scalar = std::get_if<ScalarExpr>(&expression)) {
        return expression_position(*scalar);
    }
    return std::get<AggregateCall>(expression).position;
}

inline std::size_t expression_position(const HavingExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return column->position;
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return literal->position;
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return literal->position;
    }
    if (const auto* literal = std::get_if<NullLiteral>(&expression)) {
        return literal->position;
    }
    return std::get<AggregateCall>(expression).position;
}

inline std::size_t expression_position(const OrderByExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return column->position;
    }
    return std::get<AggregateCall>(expression).position;
}

inline std::string aggregate_function_name(AggregateFunction function) {
    switch (function) {
    case AggregateFunction::Count:
        return "COUNT";
    case AggregateFunction::Sum:
        return "SUM";
    case AggregateFunction::Min:
        return "MIN";
    case AggregateFunction::Max:
        return "MAX";
    }
    return "<unknown aggregate>";
}

inline std::string quote_string_literal(const std::string& value) {
    std::string quoted = "'";
    for (const auto ch : value) {
        quoted.push_back(ch);
        if (ch == '\'') {
            quoted.push_back('\'');
        }
    }
    quoted.push_back('\'');
    return quoted;
}

inline std::string output_name(const ScalarExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        if (column->qualifier.has_value()) {
            return *column->qualifier + "." + column->name;
        }
        return column->name;
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return std::to_string(literal->value);
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return quote_string_literal(literal->value);
    }
    return "NULL";
}

inline std::string output_name(const ColumnRef& column) {
    if (column.qualifier.has_value()) {
        return *column.qualifier + "." + column.name;
    }
    return column.name;
}

inline std::string output_name(const AggregateCall& aggregate) {
    auto name = aggregate_function_name(aggregate.function) + "(";
    if (aggregate.count_star) {
        name += "*";
    } else if (aggregate.argument.has_value()) {
        name += output_name(*aggregate.argument);
    } else if (aggregate.nested_aggregate) {
        name += aggregate_function_name(aggregate.nested_function) + "(...)";
    }
    name += ")";
    return name;
}

inline std::string output_name(const SelectExpr& expression) {
    if (const auto* scalar = std::get_if<ScalarExpr>(&expression)) {
        return output_name(*scalar);
    }
    return output_name(std::get<AggregateCall>(expression));
}

inline std::string output_name(const HavingExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return output_name(*column);
    }
    if (const auto* literal = std::get_if<IntLiteral>(&expression)) {
        return std::to_string(literal->value);
    }
    if (const auto* literal = std::get_if<StringLiteral>(&expression)) {
        return quote_string_literal(literal->value);
    }
    if (std::holds_alternative<NullLiteral>(expression)) {
        return "NULL";
    }
    return output_name(std::get<AggregateCall>(expression));
}

inline std::string output_name(const OrderByExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return output_name(*column);
    }
    return output_name(std::get<AggregateCall>(expression));
}

inline const std::string& binding_name(const SelectQuery& query) {
    return query.alias.has_value() ? *query.alias : query.table;
}

inline const std::string& binding_name(const JoinClause& join) {
    return join.alias.has_value() ? *join.alias : join.table;
}

} // namespace sql
