#pragma once

#include "minidb/sql_ast.hpp"
#include "minidb/sql_error.hpp"
#include "minidb/sql_token.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace minidb::sql {

class Parser {
public:
    static constexpr std::size_t MAX_EXPRESSION_NESTING = 128;

    explicit Parser(std::vector<Token> tokens);

    [[nodiscard]] static Statement parse(std::string_view source);
    [[nodiscard]] Statement parseStatement();

private:
    std::vector<Token> tokens_;
    std::size_t current_ = 0;

    [[nodiscard]] const Token& peek() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] bool check(TokenKind kind) const noexcept;
    const Token& advance() noexcept;
    bool match(TokenKind kind) noexcept;
    const Token& consume(TokenKind kind, std::string message);
    [[noreturn]] void failAt(const Token& token, std::string message) const;

    [[nodiscard]] CreateTableStatement parseCreateTable();
    [[nodiscard]] InsertStatement parseInsert();
    [[nodiscard]] SelectStatement parseSelect();
    [[nodiscard]] UpdateStatement parseUpdate();
    [[nodiscard]] DeleteStatement parseDelete();
    [[nodiscard]] ColumnSpecification parseColumnSpecification();
    [[nodiscard]] SqlTypeSpecification parseTypeSpecification();
    [[nodiscard]] std::vector<std::string> parseIdentifierList(std::string context);
    [[nodiscard]] std::vector<SqlLiteral> parseLiteralList();
    [[nodiscard]] SqlLiteral parseLiteral();

    [[nodiscard]] std::unique_ptr<Expression> parseExpression(std::size_t depth = 0);
    [[nodiscard]] std::unique_ptr<Expression> parseOr(std::size_t depth);
    [[nodiscard]] std::unique_ptr<Expression> parseAnd(std::size_t depth);
    [[nodiscard]] std::unique_ptr<Expression> parseNot(std::size_t depth);
    [[nodiscard]] std::unique_ptr<Expression> parseComparison(std::size_t depth);
    [[nodiscard]] std::unique_ptr<Expression> parsePrimary(std::size_t depth);
    void requireNestingAvailable(const Token& token, std::size_t depth) const;
};

} // namespace minidb::sql
