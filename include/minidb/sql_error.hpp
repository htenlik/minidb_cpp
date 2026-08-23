#pragma once

#include "minidb/sql_source.hpp"

#include <stdexcept>
#include <string>

namespace minidb::sql {

enum class SqlErrorKind {
    Lexer,
    Parser,
};

class SqlError : public std::runtime_error {
public:
    SqlError(SqlErrorKind kind, std::string message, SourceSpan span);

    [[nodiscard]] SqlErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const SourceSpan& span() const noexcept { return span_; }

private:
    SqlErrorKind kind_;
    std::string message_;
    SourceSpan span_;
};

} // namespace minidb::sql
