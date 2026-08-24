#pragma once

#include "minidb/schema.hpp"
#include "minidb/sql_ast.hpp"

#include <stdexcept>
#include <string>

namespace minidb::sql {

enum class SqlExecutionErrorKind {
    Semantic,
    Constraint,
    Execution,
};

class SqlExecutionError : public std::runtime_error {
public:
    SqlExecutionError(
        SqlExecutionErrorKind kind,
        std::string message,
        SourceSpan span);

    [[nodiscard]] SqlExecutionErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const SourceSpan& span() const noexcept { return span_; }

private:
    SqlExecutionErrorKind kind_;
    std::string message_;
    SourceSpan span_;
};

enum class TruthValue {
    False,
    True,
    Unknown,
};

[[nodiscard]] TruthValue truthNot(TruthValue value) noexcept;
[[nodiscard]] TruthValue truthAnd(TruthValue left, TruthValue right) noexcept;
[[nodiscard]] TruthValue truthOr(TruthValue left, TruthValue right) noexcept;

// Performs strict, target-aware SQL literal conversion. Integer magnitudes are
// accumulated with explicit bounds checks; no signed overflow or implicit coercion occurs.
[[nodiscard]] Value convertLiteral(
    const SqlLiteral& literal,
    const ColumnDefinition& column);

} // namespace minidb::sql
