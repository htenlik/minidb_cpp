#pragma once

#include "minidb/catalog.hpp"
#include "minidb/sql_ast.hpp"
#include "minidb/sql_semantics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minidb { class CheckpointManager; class RecoveryCoordinator; }

namespace minidb::sql {

enum class AccessPath {
    None,
    HeapScan,
    PrimaryKeyLookup,
};

struct ExecutionStats {
    AccessPath accessPath = AccessPath::None;
    std::uint64_t rowsExamined = 0;
    std::uint64_t rowsReturned = 0;
    std::uint64_t indexLookups = 0;

    bool operator==(const ExecutionStats&) const = default;
};

enum class CommandKind {
    CreateTable,
    Insert,
    Update,
    Delete,
};

struct CommandResult {
    CommandKind command = CommandKind::CreateTable;
    std::uint64_t affectedRows = 0;
    std::optional<RecordId> insertedRecordId;
    ExecutionStats stats{};

    bool operator==(const CommandResult&) const = default;
};

struct SelectResult {
    std::vector<std::string> columns;
    std::vector<RowValues> rows;
    std::vector<RecordId> recordIds;
    ExecutionStats stats{};

    bool operator==(const SelectResult&) const = default;
};

using QueryResult = std::variant<CommandResult, SelectResult>;

class SqlExecutor {
public:
    explicit SqlExecutor(Catalog& catalog) : catalog_(catalog) {}

    [[nodiscard]] QueryResult execute(const Statement& statement);

private:
    Catalog& catalog_;
};

class SqlEngine {
public:
    explicit SqlEngine(
        Catalog& catalog,
        RecoveryCoordinator* recovery = nullptr,
        CheckpointManager* checkpoints = nullptr)
        : executor_(catalog), recovery_(recovery), checkpoints_(checkpoints) {}

    [[nodiscard]] QueryResult execute(std::string_view source);
    [[nodiscard]] QueryResult execute(const Statement& statement);

private:
    SqlExecutor executor_;
    RecoveryCoordinator* recovery_;
    CheckpointManager* checkpoints_;
};

} // namespace minidb::sql
