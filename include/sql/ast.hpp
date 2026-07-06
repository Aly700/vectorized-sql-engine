#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sql {

struct ColumnRef {
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

struct SelectItem {
    ScalarExpr expression;
    std::size_t position{0};
};

struct SelectQuery {
    std::vector<SelectItem> projection;
    std::string table;
    std::size_t table_position{0};
    std::optional<WhereClause> predicate;
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
        return column->name;
    }
    return std::to_string(std::get<IntLiteral>(expression).value);
}

} // namespace sql
