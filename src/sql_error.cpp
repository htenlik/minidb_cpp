#include "minidb/sql_error.hpp"

#include <utility>

namespace minidb::sql {
namespace {

std::string formatMessage(const std::string& message, const SourceSpan& span) {
    return "line " + std::to_string(span.begin.line)
        + ", column " + std::to_string(span.begin.column) + ": " + message;
}

} // namespace

SqlError::SqlError(SqlErrorKind kind, std::string message, SourceSpan span)
    : std::runtime_error(formatMessage(message, span)),
      kind_(kind),
      message_(std::move(message)),
      span_(span) {}

} // namespace minidb::sql
