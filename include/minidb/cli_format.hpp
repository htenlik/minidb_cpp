#pragma once

#include "minidb/sql_executor.hpp"

#include <string>

namespace minidb::cli {

[[nodiscard]] std::string formatQueryResult(
    const sql::QueryResult& result,
    bool includeStats = false);

} // namespace minidb::cli
