#include "minidb/cli_format.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace minidb::cli {
namespace {

std::string valueText(const Value& value) {
    return std::visit([](const auto& item) -> std::string {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::monostate>) {
            return "NULL";
        } else if constexpr (std::is_same_v<Item, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::is_same_v<Item, std::string>) {
            return item;
        } else {
            return std::to_string(item);
        }
    }, value);
}

std::string_view commandName(sql::CommandKind kind) {
    switch (kind) {
    case sql::CommandKind::CreateTable: return "CREATE TABLE";
    case sql::CommandKind::Insert: return "INSERT";
    case sql::CommandKind::Update: return "UPDATE";
    case sql::CommandKind::Delete: return "DELETE";
    }
    return "COMMAND";
}

std::string_view accessName(sql::AccessPath path) {
    switch (path) {
    case sql::AccessPath::None: return "None";
    case sql::AccessPath::HeapScan: return "HeapScan";
    case sql::AccessPath::PrimaryKeyLookup: return "PrimaryKeyLookup";
    }
    return "Unknown";
}

const sql::ExecutionStats& statsOf(const sql::QueryResult& result) {
    return std::visit([](const auto& value) -> const sql::ExecutionStats& {
        return value.stats;
    }, result);
}

} // namespace

std::string formatQueryResult(const sql::QueryResult& result, bool includeStats) {
    std::ostringstream output;
    if (const auto* command = std::get_if<sql::CommandResult>(&result)) {
        output << commandName(command->command) << '\n';
        if (command->command == sql::CommandKind::CreateTable) {
            output << "OK\n";
        } else {
            output << command->affectedRows << " row"
                   << (command->affectedRows == 1 ? "" : "s") << " affected\n";
        }
    } else {
        const auto& selection = std::get<sql::SelectResult>(result);
        std::vector<std::vector<std::string>> rows;
        rows.reserve(selection.rows.size());
        std::vector<std::size_t> widths;
        widths.reserve(selection.columns.size());
        for (const auto& column : selection.columns) {
            widths.push_back(column.size());
        }
        for (const auto& row : selection.rows) {
            if (row.size() != selection.columns.size()) {
                throw std::invalid_argument("SELECT row width disagrees with its columns");
            }
            auto& formatted = rows.emplace_back();
            formatted.reserve(row.size());
            for (std::size_t index = 0; index < row.size(); ++index) {
                formatted.push_back(valueText(row[index]));
                widths[index] = std::max(widths[index], formatted.back().size());
            }
        }
        const auto writeRow = [&](const auto& cells) {
            for (std::size_t index = 0; index < cells.size(); ++index) {
                if (index != 0) output << " | ";
                output << cells[index] << std::string(widths[index] - cells[index].size(), ' ');
            }
            output << '\n';
        };
        writeRow(selection.columns);
        for (std::size_t index = 0; index < widths.size(); ++index) {
            if (index != 0) output << "-+-";
            output << std::string(widths[index], '-');
        }
        output << '\n';
        for (const auto& row : rows) writeRow(row);
        output << selection.rows.size() << " row"
               << (selection.rows.size() == 1 ? "" : "s") << '\n';
    }

    if (includeStats) {
        const auto& stats = statsOf(result);
        output << "access: " << accessName(stats.accessPath) << '\n'
               << "rows examined: " << stats.rowsExamined << '\n'
               << "rows returned: " << stats.rowsReturned << '\n'
               << "index lookups: " << stats.indexLookups << '\n';
    }
    return output.str();
}

} // namespace minidb::cli
