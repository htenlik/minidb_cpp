#pragma once

#include <cstddef>

namespace minidb::sql {

struct SourceLocation {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;

    bool operator==(const SourceLocation&) const = default;
};

struct SourceSpan {
    SourceLocation begin{};
    SourceLocation end{};

    bool operator==(const SourceSpan&) const = default;
};

} // namespace minidb::sql
