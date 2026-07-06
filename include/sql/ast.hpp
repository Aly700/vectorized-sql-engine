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

enum class SortDirection { Asc, Desc };

struct OrderByKey {
    ColumnRef column;
    SortDirection direction{SortDirection::Asc};
};

struct SelectItem {
    ScalarExpr expression;
    std::size_t position{0};
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
    std::vector<OrderByKey> order_by;
};

SelectQuery parse_select(const std::string& sql);

inline std::size_t expression_position(const ScalarExpr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression)) {
        return column->position;
    }
    return std::get<IntLiteral>(expression).position;
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

inline const std::string& binding_name(const SelectQuery& query) {
    return query.alias.has_value() ? *query.alias : query.table;
}

inline const std::string& binding_name(const JoinClause& join) {
    return join.alias.has_value() ? *join.alias : join.table;
}

} // namespace sql
