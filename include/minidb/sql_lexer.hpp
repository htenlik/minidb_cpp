#pragma once

#include "minidb/sql_error.hpp"
#include "minidb/sql_token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace minidb::sql {

class Lexer {
public:
    static constexpr std::size_t MAX_INTEGER_DIGITS = 1024;

    explicit Lexer(std::string_view source) : source_(source) {}

    [[nodiscard]] std::vector<Token> tokenize();

private:
    std::string_view source_;
    std::size_t current_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    bool previousWasCarriageReturn_ = false;

    [[nodiscard]] bool atEnd() const noexcept { return current_ >= source_.size(); }
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;
    [[nodiscard]] SourceLocation location() const noexcept;
    char advance() noexcept;
    bool match(char expected) noexcept;
    void skipIgnored();
    [[nodiscard]] Token scanIdentifier(SourceLocation begin);
    [[nodiscard]] Token scanInteger(SourceLocation begin);
    [[nodiscard]] Token scanString(SourceLocation begin);
    [[nodiscard]] Token makeToken(TokenKind kind, SourceLocation begin) const;
    [[noreturn]] void fail(std::string message, SourceLocation begin) const;
};

} // namespace minidb::sql
