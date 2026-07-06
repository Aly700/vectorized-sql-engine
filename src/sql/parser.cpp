#include "sql/ast.hpp"
#include "sql/errors.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace sql {
namespace {

enum class TokenKind {
    Identifier,
    Integer,
    Comma,
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
           equals_keyword(text, "AND") || equals_keyword(text, "OR") || equals_keyword(text, "AS");
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
        expect_keyword("SELECT", "expected SELECT");

        SelectQuery query;
        query.projection = parse_projection();

        expect_keyword("FROM", "expected FROM after projection list");
        if (current_.kind != TokenKind::Identifier || is_reserved_keyword(current_.text)) {
            throw ParseError(current_.position, "expected table name");
        }
        query.table = current_.text;
        query.table_position = current_.position;
        advance();

        if (is_keyword("WHERE")) {
            advance();
            query.predicate = parse_where();
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
        auto expression = parse_scalar_expr("expected projection expression");
        return SelectItem{expression, expression_position(expression)};
    }

    WhereClause parse_where() {
        WhereClause where;
        where.conjuncts.push_back(parse_comparison());
        while (is_keyword("AND")) {
            advance();
            where.conjuncts.push_back(parse_comparison());
        }
        return where;
    }

    ComparisonExpr parse_comparison() {
        auto left = parse_scalar_expr("expected expression in comparison");
        const auto op_position = current_.position;
        const auto op = parse_comparison_op();
        auto right = parse_scalar_expr("expected expression in comparison");
        return ComparisonExpr{std::move(left), op, std::move(right), op_position};
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

    ScalarExpr parse_scalar_expr(const std::string& message) {
        if (current_.kind == TokenKind::Identifier && !is_reserved_keyword(current_.text)) {
            auto column = ColumnRef{current_.text, current_.position};
            advance();
            return column;
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

        throw ParseError(current_.position, message);
    }

    Lexer lexer_;
    Token current_;
};

} // namespace

SelectQuery parse_select(const std::string& input) {
    return Parser(input).parse_select();
}

} // namespace sql
