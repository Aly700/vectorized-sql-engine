#include "sql/ast.hpp"
#include "sql/errors.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sql {
namespace {

enum class TokenKind {
    Identifier,
    Integer,
    String,
    Comma,
    Dot,
    LeftParen,
    RightParen,
    Star,
    Semicolon,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    End,
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::string text;
    std::size_t position{0};
};

bool is_ident_start(unsigned char ch) {
    return std::isalpha(ch) || ch == '_';
}

bool is_ident_continue(unsigned char ch) {
    return std::isalnum(ch) || ch == '_';
}

char upper_ascii(char ch) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
}

bool equals_keyword(std::string_view text, std::string_view keyword) {
    if (text.size() != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (upper_ascii(text[i]) != keyword[i]) {
            return false;
        }
    }
    return true;
}

bool is_reserved_keyword(std::string_view text) {
    return equals_keyword(text, "SELECT") || equals_keyword(text, "FROM") || equals_keyword(text, "WHERE") ||
           equals_keyword(text, "AND") || equals_keyword(text, "OR") || equals_keyword(text, "AS") ||
           equals_keyword(text, "JOIN") || equals_keyword(text, "INNER") || equals_keyword(text, "ON") ||
           equals_keyword(text, "LEFT") || equals_keyword(text, "RIGHT") || equals_keyword(text, "OUTER") ||
           equals_keyword(text, "ORDER") || equals_keyword(text, "BY") || equals_keyword(text, "ASC") ||
           equals_keyword(text, "DESC") || equals_keyword(text, "GROUP") || equals_keyword(text, "COUNT") ||
           equals_keyword(text, "SUM") || equals_keyword(text, "MIN") || equals_keyword(text, "MAX") ||
           equals_keyword(text, "HAVING") || equals_keyword(text, "DISTINCT") || equals_keyword(text, "LIMIT") ||
           equals_keyword(text, "NULL") || equals_keyword(text, "IS") || equals_keyword(text, "NOT") ||
           equals_keyword(text, "IN") || equals_keyword(text, "EXISTS") || equals_keyword(text, "EXPLAIN");
}

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    Token next() {
        while (offset_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[offset_]))) {
            ++offset_;
        }
        if (offset_ == input_.size()) {
            return Token{TokenKind::End, "", offset_};
        }

        const auto position = offset_;
        const auto ch = input_[offset_];
        if (ch == '\'') {
            ++offset_;
            std::string value;
            while (offset_ < input_.size()) {
                const auto current = input_[offset_];
                if (current == '\'') {
                    if (offset_ + 1 < input_.size() && input_[offset_ + 1] == '\'') {
                        value.push_back('\'');
                        offset_ += 2;
                        continue;
                    }
                    ++offset_;
                    return Token{TokenKind::String, std::move(value), position};
                }
                value.push_back(current);
                ++offset_;
            }
            throw ParseError(position, "unterminated string literal");
        }

        if (is_ident_start(static_cast<unsigned char>(ch))) {
            ++offset_;
            while (offset_ < input_.size() && is_ident_continue(static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            return Token{TokenKind::Identifier, std::string(input_.substr(position, offset_ - position)), position};
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '-' && offset_ + 1 < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[offset_ + 1])))) {
            ++offset_;
            while (offset_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            return Token{TokenKind::Integer, std::string(input_.substr(position, offset_ - position)), position};
        }

        ++offset_;
        switch (ch) {
        case ',':
            return Token{TokenKind::Comma, ",", position};
        case '.':
            return Token{TokenKind::Dot, ".", position};
        case '(':
            return Token{TokenKind::LeftParen, "(", position};
        case ')':
            return Token{TokenKind::RightParen, ")", position};
        case '*':
            return Token{TokenKind::Star, "*", position};
        case ';':
            return Token{TokenKind::Semicolon, ";", position};
        case '=':
            return Token{TokenKind::Equal, "=", position};
        case '<':
            if (offset_ < input_.size() && input_[offset_] == '=') {
                ++offset_;
                return Token{TokenKind::LessEqual, "<=", position};
            }
            if (offset_ < input_.size() && input_[offset_] == '>') {
                ++offset_;
                return Token{TokenKind::NotEqual, "<>", position};
            }
            return Token{TokenKind::Less, "<", position};
        case '>':
            if (offset_ < input_.size() && input_[offset_] == '=') {
                ++offset_;
                return Token{TokenKind::GreaterEqual, ">=", position};
            }
            return Token{TokenKind::Greater, ">", position};
        default:
            throw ParseError(position, std::string("unexpected character '") + ch + "'");
        }
    }

    Token peek() const {
        auto copy = *this;
        return copy.next();
    }

private:
    std::string_view input_;
    std::size_t offset_{0};
};

class Parser {
public:
    explicit Parser(std::string_view input) : lexer_(input) {
        advance();
    }

    SelectQuery parse_select() {
        return parse_select_query(false);
    }

private:
    SelectQuery parse_select_query(bool nested) {
        SelectQuery query;
        if (!nested && is_keyword("EXPLAIN")) {
            query.explain = true;
            advance();
        }
        expect_keyword("SELECT", "expected SELECT");

        if (is_keyword("DISTINCT")) {
            query.distinct = true;
            advance();
        }
        query.projection = parse_projection();

        expect_keyword("FROM", "expected FROM after projection list");
        parse_table_reference(query.table, query.table_position, query.alias, query.alias_position, "expected table name");

        while (is_keyword("INNER") || is_keyword("JOIN") || is_keyword("LEFT") || is_keyword("RIGHT")) {
            query.joins.push_back(parse_join());
        }

        if (is_keyword("WHERE")) {
            advance();
            query.predicate = parse_where();
        }

        if (is_keyword("GROUP")) {
            query.group_by = parse_group_by();
        }

        if (is_keyword("HAVING")) {
            query.having = parse_having();
        }

        if (is_keyword("ORDER")) {
            query.order_by = parse_order_by();
        }

        if (is_keyword("LIMIT")) {
            query.limit = parse_limit();
        }

        if (!nested && current_.kind == TokenKind::Semicolon) {
            advance();
        }
        if ((!nested && current_.kind != TokenKind::End) || (nested && current_.kind != TokenKind::RightParen)) {
            if (nested) {
                throw ParseError(current_.position, "expected ')' after subquery");
            }
            throw ParseError(current_.position, "expected end of input after query");
        }

        return query;
    }

    void advance() {
        current_ = lexer_.next();
    }

    bool is_keyword(std::string_view keyword) const {
        return current_.kind == TokenKind::Identifier && equals_keyword(current_.text, keyword);
    }

    bool next_is_keyword(std::string_view keyword) const {
        const auto next = lexer_.peek();
        return next.kind == TokenKind::Identifier && equals_keyword(next.text, keyword);
    }

    void expect_keyword(std::string_view keyword, const std::string& message) {
        if (!is_keyword(keyword)) {
            throw ParseError(current_.position, message);
        }
        advance();
    }

    void expect_token(TokenKind kind, const std::string& message) {
        if (current_.kind != kind) {
            throw ParseError(current_.position, message);
        }
        advance();
    }

    void parse_table_reference(std::string& table,
                               std::size_t& table_position,
                               std::optional<std::string>& alias,
                               std::size_t& alias_position,
                               const std::string& table_message) {
        if (current_.kind != TokenKind::Identifier || is_reserved_keyword(current_.text)) {
            throw ParseError(current_.position, table_message);
        }

        table = current_.text;
        table_position = current_.position;
        advance();

        if (is_keyword("AS")) {
            advance();
            if (current_.kind != TokenKind::Identifier || is_reserved_keyword(current_.text)) {
                throw ParseError(current_.position, "expected alias after AS");
            }
            alias = current_.text;
            alias_position = current_.position;
            advance();
            return;
        }

        if (current_.kind == TokenKind::Identifier && !is_reserved_keyword(current_.text)) {
            alias = current_.text;
            alias_position = current_.position;
            advance();
        }
    }

    std::vector<SelectItem> parse_projection() {
        std::vector<SelectItem> projection;
        projection.push_back(parse_select_item());
        while (current_.kind == TokenKind::Comma) {
            advance();
            projection.push_back(parse_select_item());
        }
        if (!is_keyword("FROM")) {
            throw ParseError(current_.position, "expected ',' or FROM after projection expression");
        }
        return projection;
    }

    SelectItem parse_select_item() {
        auto expression = parse_select_expr("expected projection expression");
        SelectItem item;
        item.expression = expression;
        item.position = expression_position(expression);
        if (is_keyword("AS")) {
            advance();
            if (current_.kind != TokenKind::Identifier || is_reserved_keyword(current_.text)) {
                throw ParseError(current_.position, "expected select item alias after AS");
            }
            item.alias = current_.text;
            item.alias_position = current_.position;
            advance();
        }
        return item;
    }

    WhereClause parse_where() {
        WhereClause where;
        where.conjuncts = parse_predicate_conjuncts();
        return where;
    }

    HavingClause parse_having() {
        HavingClause having;
        having.position = current_.position;
        expect_keyword("HAVING", "expected HAVING");
        having.conjuncts = parse_having_predicate_conjuncts();
        return having;
    }

    std::vector<OrderByKey> parse_order_by() {
        expect_keyword("ORDER", "expected ORDER");
        expect_keyword("BY", "expected BY after ORDER");

        std::vector<OrderByKey> keys;
        keys.push_back(parse_order_by_key());
        while (current_.kind == TokenKind::Comma) {
            advance();
            keys.push_back(parse_order_by_key());
        }
        return keys;
    }

    std::size_t parse_limit() {
        expect_keyword("LIMIT", "expected LIMIT");
        if (current_.kind != TokenKind::Integer) {
            throw ParseError(current_.position, "expected non-negative integer after LIMIT");
        }
        if (!current_.text.empty() && current_.text.front() == '-') {
            throw ParseError(current_.position, "LIMIT must be a non-negative integer");
        }

        std::size_t value = 0;
        const auto* begin = current_.text.data();
        const auto* end = current_.text.data() + current_.text.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec == std::errc::result_out_of_range || ptr != end) {
            throw ParseError(current_.position, "LIMIT literal out of range");
        }
        advance();
        return value;
    }

    std::vector<ColumnRef> parse_group_by() {
        expect_keyword("GROUP", "expected GROUP");
        expect_keyword("BY", "expected BY after GROUP");

        std::vector<ColumnRef> keys;
        keys.push_back(parse_group_by_key());
        while (current_.kind == TokenKind::Comma) {
            advance();
            keys.push_back(parse_group_by_key());
        }
        return keys;
    }

    ColumnRef parse_group_by_key() {
        if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window functions are only supported as whole SELECT items");
        }
        if (is_aggregate_function()) {
            const auto position = current_.position;
            (void)parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(position, "window functions are only supported as whole SELECT items");
            }
            throw ParseError(position, "expected GROUP BY column name");
        }
        return parse_column_ref("expected GROUP BY column name");
    }

    OrderByKey parse_order_by_key() {
        auto expression = parse_order_by_expr();
        auto direction = SortDirection::Asc;
        if (is_keyword("ASC")) {
            advance();
        } else if (is_keyword("DESC")) {
            direction = SortDirection::Desc;
            advance();
        }
        return OrderByKey{std::move(expression), direction};
    }

    JoinClause parse_join() {
        auto kind = JoinKind::Inner;
        if (is_keyword("INNER")) {
            advance();
            expect_keyword("JOIN", "expected JOIN after INNER");
        } else if (is_keyword("LEFT")) {
            kind = JoinKind::Left;
            advance();
            if (is_keyword("OUTER")) {
                advance();
                expect_keyword("JOIN", "expected JOIN after OUTER");
            } else {
                expect_keyword("JOIN", "expected JOIN after LEFT");
            }
        } else if (is_keyword("RIGHT")) {
            kind = JoinKind::Right;
            advance();
            if (is_keyword("OUTER")) {
                advance();
                expect_keyword("JOIN", "expected JOIN after OUTER");
            } else {
                expect_keyword("JOIN", "expected JOIN after RIGHT");
            }
        } else {
            expect_keyword("JOIN", "expected JOIN");
        }

        JoinClause join;
        join.kind = kind;
        parse_table_reference(join.table,
                              join.table_position,
                              join.alias,
                              join.alias_position,
                              "expected table name after JOIN");

        expect_keyword("ON", "expected ON after JOIN table");
        join.predicates = parse_predicate_conjuncts();
        return join;
    }

    std::vector<PredicateExpr> parse_predicate_conjuncts() {
        auto expression = parse_boolean_expression();
        std::vector<PredicateExpr> conjuncts;
        append_top_level_conjunct(std::move(expression), conjuncts);
        return conjuncts;
    }

    PredicateExpr parse_boolean_expression() {
        return parse_boolean_or();
    }

    PredicateExpr parse_boolean_or() {
        auto left = parse_boolean_and();
        while (is_keyword("OR")) {
            const auto position = current_.position;
            advance();
            left = PredicateExpr::binary(PredicateKind::Or, std::move(left), parse_boolean_and(), position);
        }
        return left;
    }

    PredicateExpr parse_boolean_and() {
        auto left = parse_boolean_primary();
        while (is_keyword("AND")) {
            const auto position = current_.position;
            advance();
            left = PredicateExpr::binary(PredicateKind::And, std::move(left), parse_boolean_primary(), position);
        }
        return left;
    }

    PredicateExpr parse_boolean_primary() {
        if (current_.kind == TokenKind::LeftParen && !next_is_keyword("SELECT")) {
            advance();
            auto expression = parse_boolean_expression();
            expect_token(TokenKind::RightParen, "expected ')' after boolean expression");
            expression.parenthesized = true;
            return expression;
        }
        return parse_predicate_leaf();
    }

    static void append_top_level_conjunct(PredicateExpr expression, std::vector<PredicateExpr>& conjuncts) {
        if (expression.kind == PredicateKind::And && !expression.parenthesized) {
            append_top_level_conjunct(std::move(*expression.left), conjuncts);
            append_top_level_conjunct(std::move(*expression.right), conjuncts);
            return;
        }
        expression.parenthesized = false;
        conjuncts.push_back(std::move(expression));
    }

    std::vector<HavingPredicateExpr> parse_having_predicate_conjuncts() {
        auto expression = parse_having_boolean_expression();
        std::vector<HavingPredicateExpr> conjuncts;
        append_having_top_level_conjunct(std::move(expression), conjuncts);
        return conjuncts;
    }

    HavingPredicateExpr parse_having_boolean_expression() {
        return parse_having_boolean_or();
    }

    HavingPredicateExpr parse_having_boolean_or() {
        auto left = parse_having_boolean_and();
        while (is_keyword("OR")) {
            const auto position = current_.position;
            advance();
            left = HavingPredicateExpr::binary(PredicateKind::Or,
                                               std::move(left),
                                               parse_having_boolean_and(),
                                               position);
        }
        return left;
    }

    HavingPredicateExpr parse_having_boolean_and() {
        auto left = parse_having_boolean_primary();
        while (is_keyword("AND")) {
            const auto position = current_.position;
            advance();
            left = HavingPredicateExpr::binary(PredicateKind::And,
                                               std::move(left),
                                               parse_having_boolean_primary(),
                                               position);
        }
        return left;
    }

    HavingPredicateExpr parse_having_boolean_primary() {
        if (current_.kind == TokenKind::LeftParen && !next_is_keyword("SELECT")) {
            advance();
            auto expression = parse_having_boolean_expression();
            expect_token(TokenKind::RightParen, "expected ')' after boolean expression");
            expression.parenthesized = true;
            return expression;
        }
        return parse_having_predicate_leaf();
    }

    static void append_having_top_level_conjunct(HavingPredicateExpr expression,
                                                 std::vector<HavingPredicateExpr>& conjuncts) {
        if (expression.kind == PredicateKind::And && !expression.parenthesized) {
            append_having_top_level_conjunct(std::move(*expression.left), conjuncts);
            append_having_top_level_conjunct(std::move(*expression.right), conjuncts);
            return;
        }
        expression.parenthesized = false;
        conjuncts.push_back(std::move(expression));
    }

    PredicateExpr parse_predicate_leaf() {
        if (is_keyword("EXISTS") || (is_keyword("NOT") && next_is_keyword("EXISTS"))) {
            auto kind = PredicateKind::Exists;
            if (is_keyword("NOT")) {
                kind = PredicateKind::NotExists;
                advance();
            }
            const auto position = current_.position;
            expect_keyword("EXISTS", "expected EXISTS");
            return PredicateExpr::exists_expr(kind, parse_subquery("expected subquery after EXISTS").query, position);
        }

        auto left = parse_scalar_expr("expected expression in comparison", true);
        if (is_keyword("IS")) {
            const auto position = current_.position;
            advance();
            auto kind = PredicateKind::IsNull;
            if (is_keyword("NOT")) {
                kind = PredicateKind::IsNotNull;
                advance();
            }
            expect_keyword("NULL", "expected NULL after IS");
            return PredicateExpr::null_check_expr(kind, std::move(left), position);
        }

        if (is_keyword("IN") || (is_keyword("NOT") && next_is_keyword("IN"))) {
            auto kind = PredicateKind::In;
            if (is_keyword("NOT")) {
                kind = PredicateKind::NotIn;
                advance();
            }
            const auto position = current_.position;
            expect_keyword("IN", "expected IN after NOT");
            auto subquery = parse_subquery("expected subquery after IN");
            return PredicateExpr::in_expr(kind, std::move(left), std::move(subquery.query), position);
        }

        const auto op_position = current_.position;
        const auto op = parse_comparison_op();
        auto right = parse_scalar_expr("expected expression in comparison", true);
        return PredicateExpr::comparison_expr(ComparisonExpr{std::move(left), op, std::move(right), op_position});
    }

    HavingPredicateExpr parse_having_predicate_leaf() {
        if (is_keyword("EXISTS") || (is_keyword("NOT") && next_is_keyword("EXISTS"))) {
            auto kind = PredicateKind::Exists;
            if (is_keyword("NOT")) {
                kind = PredicateKind::NotExists;
                advance();
            }
            const auto position = current_.position;
            expect_keyword("EXISTS", "expected EXISTS");
            return HavingPredicateExpr::exists_expr(
                kind, parse_subquery("expected subquery after EXISTS").query, position);
        }

        auto left = parse_having_expr("expected expression in HAVING comparison", true);
        if (is_keyword("IS")) {
            const auto position = current_.position;
            advance();
            auto kind = PredicateKind::IsNull;
            if (is_keyword("NOT")) {
                kind = PredicateKind::IsNotNull;
                advance();
            }
            expect_keyword("NULL", "expected NULL after IS");
            return HavingPredicateExpr::null_check_expr(kind, std::move(left), position);
        }

        if (is_keyword("IN") || (is_keyword("NOT") && next_is_keyword("IN"))) {
            auto kind = PredicateKind::In;
            if (is_keyword("NOT")) {
                kind = PredicateKind::NotIn;
                advance();
            }
            const auto position = current_.position;
            expect_keyword("IN", "expected IN after NOT");
            auto subquery = parse_subquery("expected subquery after IN");
            return HavingPredicateExpr::in_expr(kind, std::move(left), std::move(subquery.query), position);
        }

        const auto op_position = current_.position;
        const auto op = parse_comparison_op();
        auto right = parse_having_expr("expected expression in HAVING comparison", true);
        return HavingPredicateExpr::comparison_expr(
            HavingComparisonExpr{std::move(left), op, std::move(right), op_position});
    }

    ComparisonOp parse_comparison_op() {
        switch (current_.kind) {
        case TokenKind::Equal:
            advance();
            return ComparisonOp::Equal;
        case TokenKind::NotEqual:
            advance();
            return ComparisonOp::NotEqual;
        case TokenKind::Less:
            advance();
            return ComparisonOp::Less;
        case TokenKind::LessEqual:
            advance();
            return ComparisonOp::LessEqual;
        case TokenKind::Greater:
            advance();
            return ComparisonOp::Greater;
        case TokenKind::GreaterEqual:
            advance();
            return ComparisonOp::GreaterEqual;
        default:
            throw ParseError(current_.position, "expected comparison operator");
        }
    }

    ColumnRef parse_column_ref(const std::string& message) {
        if (current_.kind == TokenKind::Identifier && !is_reserved_keyword(current_.text)) {
            auto first = current_;
            advance();
            if (current_.kind == TokenKind::Dot) {
                advance();
                if (current_.kind != TokenKind::Identifier || is_reserved_keyword(current_.text)) {
                    throw ParseError(current_.position, "expected column name after qualifier");
                }
                auto column = ColumnRef{first.text, current_.text, first.position};
                advance();
                return column;
            }
            auto column = ColumnRef{std::nullopt, first.text, first.position};
            return column;
        }

        throw ParseError(current_.position, message);
    }

    ScalarSubquery parse_subquery(const std::string& message) {
        if (current_.kind != TokenKind::LeftParen || !next_is_keyword("SELECT")) {
            throw ParseError(current_.position, message);
        }
        const auto position = current_.position;
        advance();
        auto query = std::make_shared<SelectQuery>(parse_select_query(true));
        expect_token(TokenKind::RightParen, "expected ')' after subquery");
        return ScalarSubquery{std::move(query), position};
    }

    ScalarExpr parse_scalar_expr(const std::string& message, bool allow_subquery = false) {
        if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window functions are only supported as whole SELECT items");
        }
        if (is_aggregate_function()) {
            const auto position = current_.position;
            (void)parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(position, "window functions are only supported as whole SELECT items");
            }
            throw ParseError(position, message);
        }
        if (allow_subquery && current_.kind == TokenKind::LeftParen && next_is_keyword("SELECT")) {
            return parse_subquery(message);
        }
        if (is_keyword("NULL")) {
            auto literal = NullLiteral{current_.position};
            advance();
            return literal;
        }

        if (current_.kind == TokenKind::Identifier && !is_reserved_keyword(current_.text)) {
            return parse_column_ref(message);
        }

        if (current_.kind == TokenKind::Integer) {
            std::int64_t value = 0;
            const auto* begin = current_.text.data();
            const auto* end = current_.text.data() + current_.text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec == std::errc::result_out_of_range || ptr != end) {
                throw ParseError(current_.position, "integer literal out of range");
            }
            auto literal = IntLiteral{value, current_.position};
            advance();
            return literal;
        }

        if (current_.kind == TokenKind::String) {
            auto literal = StringLiteral{current_.text, current_.position};
            advance();
            return literal;
        }

        throw ParseError(current_.position, message);
    }

    HavingExpr parse_having_expr(const std::string& message, bool allow_subquery = false) {
        if (allow_subquery && current_.kind == TokenKind::LeftParen && next_is_keyword("SELECT")) {
            return parse_subquery(message);
        }
        if (is_aggregate_function()) {
            auto aggregate = parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(aggregate.position,
                                 "window functions are only supported as whole SELECT items");
            }
            return aggregate;
        }
        if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window functions are only supported as whole SELECT items");
        }
        if (is_keyword("NULL")) {
            auto literal = NullLiteral{current_.position};
            advance();
            return literal;
        }
        if (current_.kind == TokenKind::Identifier && !is_reserved_keyword(current_.text)) {
            return parse_column_ref(message);
        }
        if (current_.kind == TokenKind::Integer) {
            std::int64_t value = 0;
            const auto* begin = current_.text.data();
            const auto* end = current_.text.data() + current_.text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec == std::errc::result_out_of_range || ptr != end) {
                throw ParseError(current_.position, "integer literal out of range");
            }
            auto literal = IntLiteral{value, current_.position};
            advance();
            return literal;
        }
        if (current_.kind == TokenKind::String) {
            auto literal = StringLiteral{current_.text, current_.position};
            advance();
            return literal;
        }
        throw ParseError(current_.position, message);
    }

    OrderByExpr parse_order_by_expr() {
        if (is_aggregate_function()) {
            auto aggregate = parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(aggregate.position,
                                 "window functions are only supported as whole SELECT items");
            }
            return aggregate;
        }
        if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window functions are only supported as whole SELECT items");
        }
        return parse_column_ref("expected ORDER BY column name");
    }

    SelectExpr parse_select_expr(const std::string& message) {
        if (is_aggregate_function()) {
            auto aggregate = parse_aggregate_call();
            if (is_keyword("OVER")) {
                return parse_aggregate_window_call(std::move(aggregate));
            }
            return aggregate;
        }
        if (is_ranking_window_function()) {
            return parse_ranking_window_call();
        }
        return parse_scalar_expr(message);
    }

    bool is_aggregate_function() const {
        return is_keyword("COUNT") || is_keyword("SUM") || is_keyword("MIN") || is_keyword("MAX");
    }

    bool is_ranking_window_function() const {
        if (!(is_keyword("ROW_NUMBER") || is_keyword("RANK") || is_keyword("DENSE_RANK"))) {
            return false;
        }
        return lexer_.peek().kind == TokenKind::LeftParen;
    }

    bool is_window_frame_keyword() const {
        return is_keyword("ROWS") || is_keyword("RANGE") || is_keyword("GROUPS") ||
               is_keyword("BETWEEN") || is_keyword("UNBOUNDED") || is_keyword("CURRENT");
    }

    WindowInputExpr parse_window_input_expr(const std::string& message) {
        if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window expressions must be whole SELECT items");
        }
        if (is_aggregate_function()) {
            auto aggregate = parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(aggregate.position, "window expressions must be whole SELECT items");
            }
            return aggregate;
        }
        return parse_column_ref(message);
    }

    WindowOrderKey parse_window_order_key() {
        auto expression = parse_window_input_expr("expected window ORDER BY expression");
        auto direction = SortDirection::Asc;
        if (is_keyword("ASC")) {
            advance();
        } else if (is_keyword("DESC")) {
            direction = SortDirection::Desc;
            advance();
        }
        return WindowOrderKey{std::move(expression), direction};
    }

    WindowFrame parse_window_frame() {
        const auto frame = is_keyword("ROWS") ? WindowFrame::RowsCumulative
                                               : WindowFrame::RangeCumulative;
        advance();

        const auto consume = [&](const std::string& keyword) {
            if (!is_keyword(keyword)) {
                throw ParseError(current_.position, "unsupported frame");
            }
            advance();
        };
        consume("BETWEEN");
        consume("UNBOUNDED");
        consume("PRECEDING");
        consume("AND");
        consume("CURRENT");
        consume("ROW");
        return frame;
    }

    WindowCall parse_window_specification(WindowCall window) {
        window.over_position = current_.position;
        expect_keyword("OVER", "expected OVER after window function");
        expect_token(TokenKind::LeftParen, "expected '(' after OVER");

        if (is_keyword("PARTITION")) {
            advance();
            expect_keyword("BY", "expected BY after PARTITION");
            window.partition_by.push_back(parse_window_input_expr("expected window PARTITION BY expression"));
            while (current_.kind == TokenKind::Comma) {
                advance();
                window.partition_by.push_back(parse_window_input_expr("expected window PARTITION BY expression"));
            }
        }

        if (is_keyword("ORDER")) {
            advance();
            expect_keyword("BY", "expected BY after ORDER");
            window.order_by.push_back(parse_window_order_key());
            while (current_.kind == TokenKind::Comma) {
                advance();
                window.order_by.push_back(parse_window_order_key());
            }
        }

        if (is_keyword("ROWS") || is_keyword("RANGE")) {
            window.frame_position = current_.position;
            window.explicit_frame = parse_window_frame();
            if (current_.kind != TokenKind::RightParen) {
                throw ParseError(current_.position, "unsupported frame");
            }
        } else if (is_window_frame_keyword()) {
            throw ParseError(current_.position, "unsupported frame");
        }
        expect_token(TokenKind::RightParen, "expected ')' after window specification");
        return window;
    }

    WindowCall parse_ranking_window_call() {
        WindowCall window;
        window.position = current_.position;
        if (is_keyword("ROW_NUMBER")) {
            window.function = WindowFunction::RowNumber;
        } else if (is_keyword("RANK")) {
            window.function = WindowFunction::Rank;
        } else if (is_keyword("DENSE_RANK")) {
            window.function = WindowFunction::DenseRank;
        } else {
            throw ParseError(current_.position, "expected ranking window function");
        }
        advance();
        expect_token(TokenKind::LeftParen, "expected '(' after window function");
        expect_token(TokenKind::RightParen, "ranking window functions do not accept arguments");
        if (!is_keyword("OVER")) {
            throw ParseError(current_.position, "expected OVER after window function");
        }
        return parse_window_specification(std::move(window));
    }

    static WindowFunction aggregate_window_function(AggregateFunction function) {
        switch (function) {
        case AggregateFunction::Count:
            return WindowFunction::Count;
        case AggregateFunction::Sum:
            return WindowFunction::Sum;
        case AggregateFunction::Min:
            return WindowFunction::Min;
        case AggregateFunction::Max:
            return WindowFunction::Max;
        }
        throw std::logic_error("unreachable aggregate window function");
    }

    WindowCall parse_aggregate_window_call(AggregateCall aggregate) {
        WindowCall window;
        window.function = aggregate_window_function(aggregate.function);
        window.position = aggregate.position;
        window.count_star = aggregate.count_star;
        if (aggregate.nested_call != nullptr) {
            window.argument = *aggregate.nested_call;
        } else if (aggregate.argument.has_value()) {
            window.argument = *aggregate.argument;
        }
        return parse_window_specification(std::move(window));
    }

    AggregateFunction parse_aggregate_function() {
        if (is_keyword("COUNT")) {
            advance();
            return AggregateFunction::Count;
        }
        if (is_keyword("SUM")) {
            advance();
            return AggregateFunction::Sum;
        }
        if (is_keyword("MIN")) {
            advance();
            return AggregateFunction::Min;
        }
        if (is_keyword("MAX")) {
            advance();
            return AggregateFunction::Max;
        }
        throw ParseError(current_.position, "expected aggregate function");
    }

    AggregateCall parse_aggregate_call() {
        const auto position = current_.position;
        const auto function = parse_aggregate_function();
        expect_token(TokenKind::LeftParen, "expected '(' after aggregate function");

        AggregateCall aggregate;
        aggregate.function = function;
        aggregate.position = position;

        if (function == AggregateFunction::Count && current_.kind == TokenKind::Star) {
            aggregate.count_star = true;
            advance();
            expect_token(TokenKind::RightParen, "expected ')' after aggregate argument");
            return aggregate;
        }

        if (current_.kind == TokenKind::Star) {
            throw ParseError(current_.position, "expected aggregate argument column");
        }

        if (is_aggregate_function()) {
            const auto nested = parse_aggregate_call();
            if (is_keyword("OVER")) {
                throw ParseError(nested.position, "window expressions must be whole SELECT items");
            }
            aggregate.nested_aggregate = true;
            aggregate.nested_function = nested.function;
            aggregate.nested_position = nested.position;
            aggregate.nested_call = std::make_shared<AggregateCall>(nested);
        } else if (is_ranking_window_function()) {
            throw ParseError(current_.position, "window expressions must be whole SELECT items");
        } else {
            aggregate.argument = parse_column_ref("expected aggregate argument column");
        }

        expect_token(TokenKind::RightParen, "expected ')' after aggregate argument");
        return aggregate;
    }

    Lexer lexer_;
    Token current_;
};

} // namespace

SelectQuery parse_select(const std::string& input) {
    return Parser(input).parse_select();
}

} // namespace sql
