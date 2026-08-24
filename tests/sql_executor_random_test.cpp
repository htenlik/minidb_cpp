#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::size_t OPERATIONS_PER_SEED = 2000;
constexpr std::size_t REOPEN_CADENCE = 200;
constexpr std::uint64_t SEEDS[]{0x70000001ULL, 0x7EEDC0DEULL};

struct ModelRow {
    std::int64_t balance;
    bool active;
    std::optional<std::string> note;

    bool operator==(const ModelRow&) const = default;
};

using Model = std::map<std::uint32_t, ModelRow>;

const minidb::sql::CommandResult& command(const minidb::sql::QueryResult& result) {
    return std::get<minidb::sql::CommandResult>(result);
}

const minidb::sql::SelectResult& selection(const minidb::sql::QueryResult& result) {
    return std::get<minidb::sql::SelectResult>(result);
}

std::string randomText(std::mt19937_64& random) {
    std::size_t length = static_cast<std::size_t>(random() % 33U);
    if (random() % 20U == 0) {
        length = 600U + static_cast<std::size_t>(random() % 601U);
    }
    std::string text(length, 'a');
    for (std::size_t index = 0; index < length; ++index) {
        text[index] = static_cast<char>('a' + ((random() + index) % 26U));
    }
    return text;
}

ModelRow randomRow(std::mt19937_64& random) {
    ModelRow row{
        static_cast<std::int64_t>(random() % 20001U) - 10000,
        (random() & 1U) != 0,
        std::nullopt,
    };
    if (random() % 4U != 0) {
        row.note = randomText(random);
    }
    return row;
}

std::string sqlString(const std::optional<std::string>& value) {
    return value.has_value() ? "'" + *value + "'" : "NULL";
}

minidb::RowValues values(std::uint32_t key, const ModelRow& row) {
    return {
        key,
        row.balance,
        row.active,
        row.note.has_value() ? minidb::Value{*row.note} : minidb::Value{std::monostate{}},
    };
}

void requireStructuredError(
    minidb::sql::SqlEngine& engine,
    const std::string& source,
    minidb::sql::SqlExecutionErrorKind expected) {
    try {
        static_cast<void>(engine.execute(source));
    } catch (const minidb::sql::SqlExecutionError& error) {
        minidb::test::require(error.kind() == expected,
                              "Random SQL failure had wrong structured error kind");
        return;
    }
    throw std::runtime_error("Random SQL expected an execution error");
}

void compareAll(minidb::sql::SqlEngine& engine, const Model& model) {
    const auto query = engine.execute("SELECT * FROM accounts");
    const auto& result = selection(query);
    minidb::test::require(result.rows.size() == model.size()
                              && result.stats.accessPath == minidb::sql::AccessPath::HeapScan
                              && result.stats.rowsExamined == model.size(),
                          "Random SQL full scan count/stats disagreed with model");
    Model observed;
    for (const auto& row : result.rows) {
        const auto key = std::get<std::uint32_t>(row[0]);
        const auto note = std::holds_alternative<std::monostate>(row[3])
            ? std::optional<std::string>{}
            : std::optional<std::string>{std::get<std::string>(row[3])};
        minidb::test::require(
            observed.emplace(key, ModelRow{
                std::get<std::int64_t>(row[1]),
                std::get<bool>(row[2]),
                note,
            }).second,
            "Random SQL scan returned duplicate primary key");
    }
    minidb::test::require(observed == model,
                          "Random SQL full scan contents disagreed with model");
}

void runOperation(
    minidb::sql::SqlEngine& engine,
    Model& model,
    std::mt19937_64& random,
    std::size_t operation) {
    const auto kind = random() % 9U;
    const auto key = static_cast<std::uint32_t>(random() % 401U);
    std::string source;
    if (kind == 0) {
        const auto row = randomRow(random);
        source = "INSERT INTO accounts VALUES (" + std::to_string(key) + ", "
            + std::to_string(row.balance) + ", " + (row.active ? "TRUE" : "FALSE")
            + ", " + sqlString(row.note) + ")";
        if (model.contains(key)) {
            requireStructuredError(
                engine, source, minidb::sql::SqlExecutionErrorKind::Constraint);
        } else {
            minidb::test::require(command(engine.execute(source)).affectedRows == 1,
                                  "Random INSERT affected-row count was not one");
            model.emplace(key, row);
        }
    } else if (kind == 1) {
        source = "SELECT * FROM accounts WHERE id = " + std::to_string(key);
        const auto query = engine.execute(source);
        const auto& result = selection(query);
        const auto expected = model.find(key);
        minidb::test::require(
            result.rows.size() == (expected == model.end() ? 0U : 1U)
                && result.stats.accessPath == minidb::sql::AccessPath::PrimaryKeyLookup
                && result.stats.indexLookups == 1 && result.stats.rowsExamined <= 1,
            "Random PK SELECT result/stats disagreed with model");
        if (expected != model.end()) {
            minidb::test::require(result.rows.front() == values(key, expected->second),
                                  "Random PK SELECT values disagreed with model");
        }
    } else if (kind == 2) {
        const auto replacement = randomRow(random);
        source = "UPDATE accounts SET balance = " + std::to_string(replacement.balance)
            + ", active = " + (replacement.active ? "TRUE" : "FALSE")
            + ", note = " + sqlString(replacement.note)
            + " WHERE id = " + std::to_string(key);
        const auto affected = command(engine.execute(source));
        const auto found = model.find(key);
        minidb::test::require(
            affected.affectedRows == (found == model.end() ? 0U : 1U)
                && affected.stats.accessPath == minidb::sql::AccessPath::PrimaryKeyLookup,
            "Random PK UPDATE result/stats disagreed with model");
        if (found != model.end()) {
            found->second = replacement;
        }
    } else if (kind == 3) {
        const auto newKey = static_cast<std::uint32_t>(random() % 401U);
        source = "UPDATE accounts SET id = " + std::to_string(newKey)
            + " WHERE id = " + std::to_string(key);
        const auto found = model.find(key);
        if (found != model.end() && newKey != key && model.contains(newKey)) {
            requireStructuredError(
                engine, source, minidb::sql::SqlExecutionErrorKind::Constraint);
        } else {
            const auto affected = command(engine.execute(source));
            minidb::test::require(
                affected.affectedRows == (found == model.end() ? 0U : 1U),
                "Random PK-changing UPDATE count disagreed with model");
            if (found != model.end() && newKey != key) {
                auto row = found->second;
                model.erase(found);
                model.emplace(newKey, std::move(row));
            }
        }
    } else if (kind == 4) {
        source = "DELETE FROM accounts WHERE id = " + std::to_string(key);
        const auto affected = command(engine.execute(source));
        const auto expected = model.erase(key);
        minidb::test::require(affected.affectedRows == expected
                                  && affected.stats.accessPath
                                      == minidb::sql::AccessPath::PrimaryKeyLookup,
                              "Random PK DELETE result/stats disagreed with model");
    } else if (kind == 5) {
        source = "SELECT note, id, id FROM accounts WHERE " + std::to_string(key)
            + " = id";
        const auto query = engine.execute(source);
        const auto& result = selection(query);
        const auto found = model.find(key);
        minidb::test::require(result.rows.size() == (found == model.end() ? 0U : 1U)
                                  && result.columns
                                      == std::vector<std::string>{"note", "id", "id"},
                              "Random projection result disagreed with model");
        if (found != model.end()) {
            const minidb::Value note = found->second.note.has_value()
                ? minidb::Value{*found->second.note}
                : minidb::Value{std::monostate{}};
            minidb::test::require(
                result.rows.front() == minidb::RowValues{note, key, key},
                "Random projection values/order disagreed with model");
        }
    } else if (kind == 6) {
        const auto active = (random() & 1U) != 0;
        source = std::string("SELECT id FROM accounts WHERE active = ")
            + (active ? "TRUE" : "FALSE");
        const auto query = engine.execute(source);
        const auto& result = selection(query);
        std::size_t expected = 0;
        for (const auto& [modelKey, row] : model) {
            static_cast<void>(modelKey);
            expected += row.active == active ? 1U : 0U;
        }
        minidb::test::require(result.rows.size() == expected
                                  && result.stats.accessPath
                                      == minidb::sql::AccessPath::HeapScan
                                  && result.stats.rowsExamined == model.size(),
                              "Random filtered scan disagreed with model");
    } else if (kind == 7) {
        source = "UPDATE accounts SET note = NULL WHERE active = FALSE";
        const auto affected = command(engine.execute(source));
        std::size_t expected = 0;
        for (auto& [modelKey, row] : model) {
            static_cast<void>(modelKey);
            if (!row.active) {
                row.note.reset();
                ++expected;
            }
        }
        minidb::test::require(affected.affectedRows == expected
                                  && affected.stats.rowsExamined == model.size(),
                              "Random multi-row UPDATE disagreed with model");
    } else {
        source = "DELETE FROM accounts WHERE balance < -9000";
        const auto affected = command(engine.execute(source));
        std::size_t expected = 0;
        for (auto iterator = model.begin(); iterator != model.end();) {
            if (iterator->second.balance < -9000) {
                iterator = model.erase(iterator);
                ++expected;
            } else {
                ++iterator;
            }
        }
        minidb::test::require(affected.affectedRows == expected,
                              "Random multi-row DELETE disagreed with model");
    }
    static_cast<void>(operation);
}

void runSeed(std::uint64_t seed) {
    minidb::test::TemporaryDatabase database("sql_random_" + std::to_string(seed));
    std::mt19937_64 random(seed);
    Model model;
    for (std::size_t batchStart = 0; batchStart < OPERATIONS_PER_SEED;
         batchStart += REOPEN_CADENCE) {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        minidb::sql::SqlEngine engine(catalog);
        if (!catalog.findTable("accounts").has_value()) {
            static_cast<void>(engine.execute(
                "CREATE TABLE accounts (id UINT32 PRIMARY KEY, balance INT64 NOT NULL, "
                "active BOOLEAN NOT NULL, note VARCHAR(1500))"));
        }
        const auto batchEnd = std::min(batchStart + REOPEN_CADENCE, OPERATIONS_PER_SEED);
        for (std::size_t operation = batchStart; operation < batchEnd; ++operation) {
            try {
                runOperation(engine, model, random, operation);
                if (operation % 50U == 0) {
                    compareAll(engine, model);
                    auto table = catalog.openTable("accounts");
                    table.validate();
                    auto& allocator = storage.allocator;
                    allocator.validate();
                    minidb::test::requireBufferClean(storage);
                }
                if (operation % 200U == 0) {
                    catalog.validate();
                    minidb::test::requireBufferClean(storage);
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    "seed=" + std::to_string(seed)
                    + " operation=" + std::to_string(operation)
                    + ": " + error.what());
            }
        }
        compareAll(engine, model);
        catalog.validate();
        minidb::test::requireBufferClean(storage);
        storage.bufferPool.flushAll();
    }

    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = minidb::Catalog::open(storage.bufferPool, storage.diskManager, storage.allocator);
    minidb::sql::SqlEngine engine(catalog);
    compareAll(engine, model);
    catalog.validate();
    minidb::test::requireBufferClean(storage);
}

} // namespace

int main() {
    try {
        for (const auto seed : SEEDS) {
            runSeed(seed);
        }
        std::cout << "sql_executor_random_test passed "
                  << "(4000 operations, seeds 0x70000001/0x7EEDC0DE, "
                  << "reopen cadence 200, 18 mid-workload reopens)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_executor_random_test failed: " << error.what() << '\n';
        return 1;
    }
}
