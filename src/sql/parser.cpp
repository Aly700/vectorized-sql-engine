#include "sql/ast.hpp"
#include "sql/errors.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
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
           equals_keyword(text, "EXPLAIN");
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
        SelectQuery query;
        if (is_keyword("EXPLAIN")) {
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

        if (current_.kind == TokenKind::Semicolon) {
            advance();
        }
        if (current_.kind != TokenKind::End) {
            throw ParseError(current_.position, "expected end of input after query");
        }

        return query;
    }

private:
    void advance() {
        current_ = lexer_.next();
    }

    bool is_keyword(std::string_view keyword) const {
        return current_.kind == TokenKind::Identifier && equals_keyword(current_.text, keyword);
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
        keys.push_back(parse_column_ref("expected GROUP BY column name"));
        while (current_.kind == TokenKind::Comma) {
            advance();
            keys.push_back(parse_column_ref("expected GROUP BY column name"));
        }
        return keys;
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
        if (current_.kind == TokenKind::LeftParen) {
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
        if (current_.kind == TokenKind::LeftParen) {
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
        auto left = parse_scalar_expr("expected expression in comparison");
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

        const auto op_position = current_.position;
        const auto op = parse_comparison_op();
        auto right = parse_scalar_expr("expected expression in comparison");
        return PredicateExpr::comparison_expr(ComparisonExpr{std::move(left), op, std::move(right), op_position});
    }

    HavingPredicateExpr parse_having_predicate_leaf() {
        auto left = parse_having_expr("expected expression in HAVING comparison");
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

        const auto op_position = current_.position;
        const auto op = parse_comparison_op();
        auto right = parse_having_expr("expected expression in HAVING comparison");
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

    ScalarExpr parse_scalar_expr(const std::string& message) {
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

    HavingExpr parse_having_expr(const std::string& message) {
        if (is_aggregate_function()) {
            return parse_aggregate_call();
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
            return parse_aggregate_call();
        }
        return parse_column_ref("expected ORDER BY column name");
    }

    SelectExpr parse_select_expr(const std::string& message) {
        if (is_aggregate_function()) {
            return parse_aggregate_call();
        }
        return parse_scalar_expr(message);
    }

    bool is_aggregate_function() const {
        return is_keyword("COUNT") || is_keyword("SUM") || is_keyword("MIN") || is_keyword("MAX");
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
            aggregate.nested_aggregate = true;
            aggregate.nested_function = nested.function;
            aggregate.nested_position = nested.position;
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
