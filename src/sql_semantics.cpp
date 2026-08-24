#include "minidb/sql_semantics.hpp"

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

namespace minidb::sql {
namespace {

std::string formatMessage(const std::string& message, const SourceSpan& span) {
    return "line " + std::to_string(span.begin.line)
        + ", column " + std::to_string(span.begin.column) + ": " + message;
}

[[noreturn]] void fail(
    SqlExecutionErrorKind kind,
    std::string message,
    SourceSpan span) {
    throw SqlExecutionError(kind, std::move(message), span);
}

std::uint64_t accumulateMagnitude(
    std::string_view digits,
    std::uint64_t maximum,
    const SqlLiteral& literal,
    std::string_view typeName) {
    std::uint64_t value = 0;
    for (const char digit : digits) {
        const auto component = static_cast<std::uint64_t>(digit - '0');
        if (value > (maximum - component) / 10U) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "integer literal is out of range for " + std::string(typeName),
                literal.span);
        }
        value = value * 10U + component;
    }
    return value;
}

std::string typeName(DataType type) {
    switch (type) {
    case DataType::UINT32: return "UINT32";
    case DataType::INT64: return "INT64";
    case DataType::BOOLEAN: return "BOOLEAN";
    case DataType::VARCHAR: return "VARCHAR";
    }
    return "unknown type";
}

} // namespace

SqlExecutionError::SqlExecutionError(
    SqlExecutionErrorKind kind,
    std::string message,
    SourceSpan span)
    : std::runtime_error(formatMessage(message, span)),
      kind_(kind),
      message_(std::move(message)),
      span_(span) {}

TruthValue truthNot(TruthValue value) noexcept {
    switch (value) {
    case TruthValue::False: return TruthValue::True;
    case TruthValue::True: return TruthValue::False;
    case TruthValue::Unknown: return TruthValue::Unknown;
    }
    return TruthValue::Unknown;
}

TruthValue truthAnd(TruthValue left, TruthValue right) noexcept {
    if (left == TruthValue::False || right == TruthValue::False) {
        return TruthValue::False;
    }
    if (left == TruthValue::True && right == TruthValue::True) {
        return TruthValue::True;
    }
    return TruthValue::Unknown;
}

TruthValue truthOr(TruthValue left, TruthValue right) noexcept {
    if (left == TruthValue::True || right == TruthValue::True) {
        return TruthValue::True;
    }
    if (left == TruthValue::False && right == TruthValue::False) {
        return TruthValue::False;
    }
    return TruthValue::Unknown;
}

Value convertLiteral(const SqlLiteral& literal, const ColumnDefinition& column) {
    if (std::holds_alternative<NullLiteral>(literal.value)) {
        if (!column.nullable) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "column '" + column.name + "' cannot be NULL",
                literal.span);
        }
        return std::monostate{};
    }

    if (const auto* boolean = std::get_if<BooleanLiteral>(&literal.value)) {
        if (column.type != DataType::BOOLEAN) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "BOOLEAN literal is incompatible with " + typeName(column.type)
                    + " column '" + column.name + "'",
                literal.span);
        }
        return boolean->value;
    }

    if (const auto* string = std::get_if<StringLiteral>(&literal.value)) {
        if (column.type != DataType::VARCHAR) {
            fail(
                SqlExecutionErrorKind::Semantic,
                "string literal is incompatible with " + typeName(column.type)
                    + " column '" + column.name + "'",
                literal.span);
        }
        if (string->value.size() > column.varcharMaxBytes) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "value for column '" + column.name + "' exceeds VARCHAR("
                    + std::to_string(column.varcharMaxBytes) + ")",
                literal.span);
        }
        return string->value;
    }

    const auto* integer = std::get_if<IntegerLiteral>(&literal.value);
    if (integer == nullptr) {
        fail(SqlExecutionErrorKind::Execution, "unsupported SQL literal", literal.span);
    }
    if (column.type == DataType::UINT32) {
        if (integer->negative) {
            fail(
                SqlExecutionErrorKind::Constraint,
                "negative integer literal is out of range for UINT32",
                literal.span);
        }
        return static_cast<std::uint32_t>(accumulateMagnitude(
            integer->magnitude,
            std::numeric_limits<std::uint32_t>::max(),
            literal,
            "UINT32"));
    }
    if (column.type == DataType::INT64) {
        constexpr auto positiveMaximum =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        constexpr auto negativeMaximum = positiveMaximum + 1U;
        const auto magnitude = accumulateMagnitude(
            integer->magnitude,
            integer->negative ? negativeMaximum : positiveMaximum,
            literal,
            "INT64");
        if (!integer->negative) {
            return static_cast<std::int64_t>(magnitude);
        }
        if (magnitude == negativeMaximum) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(magnitude);
    }
    fail(
        SqlExecutionErrorKind::Semantic,
        "integer literal is incompatible with " + typeName(column.type)
            + " column '" + column.name + "'",
        literal.span);
}

} // namespace minidb::sql
