#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

using ScalarExpr = std::variant<ColumnRef, IntLiteral>;

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
using HavingExpr = std::variant<ColumnRef, IntLiteral, AggregateCall>;
using OrderByExpr = std::variant<ColumnRef, AggregateCall>;

enum class ComparisonOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct ComparisonExpr {
    ScalarExpr left;
    ComparisonOp op{ComparisonOp::Equal};
    ScalarExpr right;
    std::size_t operator_position{0};
};

struct WhereClause {
    std::vector<ComparisonExpr> conjuncts;
};

struct HavingComparisonExpr {
    HavingExpr left;
    ComparisonOp op{ComparisonOp::Equal};
    HavingExpr right;
    std::size_t operator_position{0};
};

struct HavingClause {
    std::size_t position{0};
    std::vector<HavingComparisonExpr> conjuncts;
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
    std::vector<ComparisonExpr> predicates;
};

struct SelectQuery {
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
};

SelectQuery parse_select(const std::string& sql);

inline std::size_t expression_position(const ScalarExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return column->position;
    }
    return std::get<IntLiteral>(expression).position;
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

inline std::string output_name(const ScalarExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        if (column->qualifier.has_value()) {
            return *column->qualifier + "." + column->name;
        }
        return column->name;
    }
    return std::to_string(std::get<IntLiteral>(expression).value);
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
