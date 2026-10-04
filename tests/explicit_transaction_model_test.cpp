#include "minidb/database_server.hpp"
#include "minidb/sql_error.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <variant>

namespace {

using minidb::test::require;
using Model = std::map<std::uint32_t, std::int64_t>;
constexpr minidb::SessionId OWNER = 17;
constexpr minidb::SessionId OTHER = 23;
constexpr std::size_t OPERATIONS_PER_SEED = 5000;
constexpr std::uint64_t SEEDS[]{0x12A10001ULL, 0x12A10002ULL, 0x12A10003ULL, 0x12A10004ULL};

minidb::net::ServerConfig configuration(std::uint64_t seed) {
    minidb::net::ServerConfig result;
    result.port = 0;
    result.bufferFrames = 8;
    result.checkpointWalBytes = 0;
    result.walUpdateMode = static_cast<minidb::WalUpdateMode>(seed % 3);
    return result;
}

void expectExecutionError(minidb::sql::SqlEngine& engine, const std::string& source,
                          minidb::SessionId session = OWNER) {
    bool rejected = false;
    try {
        static_cast<void>(engine.execute(source, session));
    } catch (const minidb::sql::SqlExecutionError&) {
        rejected = true;
    }
    require(rejected, "Expected structured execution error: " + source);
}

void compareRows(minidb::sql::SqlEngine& engine, const Model& expected) {
    const auto result = engine.execute("SELECT id, amount FROM ledger", OWNER);
    Model observed;
    for (const auto& row : std::get<minidb::sql::SelectResult>(result).rows) {
        require(observed.emplace(std::get<std::uint32_t>(row[0]),
                                 std::get<std::int64_t>(row[1])).second,
                "Scan repeated a primary key");
    }
    require(observed == expected, "Visible data disagrees with transaction oracle");
}

void runSeed(std::uint64_t seed) {
    minidb::test::TemporaryDatabase database("explicit_transaction_model");
    Model committed{{0, 0}};
    Model visible = committed;
    std::mt19937_64 random(seed);
    bool active = false;
    minidb::TransactionId transactionId = minidb::INVALID_TRANSACTION_ID;
    {
        minidb::net::DatabaseServer server(database.path().string(), configuration(seed));
        auto& engine = server.sqlEngine();
        static_cast<void>(engine.execute(
            "CREATE TABLE ledger (id UINT32 PRIMARY KEY, amount INT64 NOT NULL)", OWNER));
        static_cast<void>(engine.execute("INSERT INTO ledger VALUES (0, 0)", OWNER));
        for (std::size_t operation = 0; operation < OPERATIONS_PER_SEED; ++operation) {
            const auto kind = random() % 12;
            const auto key = static_cast<std::uint32_t>(1 + random() % 32);
            const auto amount = static_cast<std::int64_t>(random() % 20001) - 10000;
            try {
                switch (kind) {
                case 0:
                    if (active) {
                        expectExecutionError(engine, "BEGIN");
                    } else {
                        static_cast<void>(engine.execute("BEGIN TRANSACTION", OWNER));
                        active = true;
                        transactionId = engine.transactionManager().activeTransactionId();
                        require(transactionId != minidb::INVALID_TRANSACTION_ID,
                                "BEGIN did not establish a transaction identity");
                        require(!engine.transactionManager().hasMaterializedWalBegin(),
                                "BEGIN wrote WAL before any mutation");
                    }
                    break;
                case 1:
                    if (active) {
                        static_cast<void>(engine.execute("COMMIT", OWNER));
                        committed = visible;
                        active = false;
                    } else {
                        expectExecutionError(engine, "COMMIT");
                    }
                    break;
                case 2:
                    if (active) {
                        static_cast<void>(engine.execute("ROLLBACK", OWNER));
                        visible = committed;
                        active = false;
                    } else {
                        expectExecutionError(engine, "ROLLBACK");
                    }
                    break;
                case 3: {
                    const auto source = "INSERT INTO ledger VALUES (" + std::to_string(key)
                        + ", " + std::to_string(amount) + ")";
                    if (visible.contains(key)) {
                        expectExecutionError(engine, source);
                        if (active) visible = committed;
                        active = false;
                    } else {
                        static_cast<void>(engine.execute(source, OWNER));
                        visible.emplace(key, amount);
                        if (!active) committed = visible;
                    }
                    break;
                }
                case 4: {
                    const auto result = engine.execute("UPDATE ledger SET amount = "
                        + std::to_string(amount) + " WHERE id = " + std::to_string(key), OWNER);
                    const auto found = visible.find(key);
                    require(std::get<minidb::sql::CommandResult>(result).affectedRows
                                == (found == visible.end() ? 0U : 1U),
                            "UPDATE affected-row count disagrees with oracle");
                    if (found != visible.end()) found->second = amount;
                    if (!active) committed = visible;
                    break;
                }
                case 5: {
                    const auto result = engine.execute(
                        "DELETE FROM ledger WHERE id = " + std::to_string(key), OWNER);
                    require(std::get<minidb::sql::CommandResult>(result).affectedRows
                                == visible.erase(key),
                            "DELETE affected-row count disagrees with oracle");
                    if (!active) committed = visible;
                    break;
                }
                case 6: {
                    const auto result = engine.execute(
                        "SELECT amount FROM ledger WHERE id = " + std::to_string(key), OWNER);
                    const auto& rows = std::get<minidb::sql::SelectResult>(result).rows;
                    const auto found = visible.find(key);
                    require(rows.size() == (found == visible.end() ? 0U : 1U),
                            "Read-your-writes lookup count disagrees with oracle");
                    if (found != visible.end()) {
                        require(std::get<std::int64_t>(rows[0][0]) == found->second,
                                "Read-your-writes value disagrees with oracle");
                    }
                    break;
                }
                case 7:
                    expectExecutionError(engine, random() % 2 == 0
                        ? "INSERT INTO ledger VALUES (0, 999)"
                        : "UPDATE ledger SET nonexistent = 1 WHERE id = 0");
                    if (active) visible = committed;
                    active = false;
                    break;
                case 8:
                    expectExecutionError(engine, "SELECT nonexistent FROM ledger");
                    break; // Read errors preserve the current explicit transaction.
                case 9:
                    engine.closeSession(OWNER);
                    if (active) visible = committed;
                    active = false;
                    break;
                case 10:
                    if (active) {
                        expectExecutionError(engine, "SELECT * FROM ledger", OTHER);
                        engine.closeSession(OTHER);
                    } else {
                        static_cast<void>(engine.execute("SELECT * FROM ledger", OTHER));
                    }
                    break;
                case 11: {
                    bool rejected = false;
                    try {
                        static_cast<void>(engine.execute("UPDATE ledger SET", OWNER));
                    } catch (const minidb::sql::SqlError&) {
                        rejected = true;
                    }
                    require(rejected, "Malformed statement unexpectedly executed");
                    break; // Parsing errors have not started a mutation.
                }
                }
                require(engine.transactionManager().hasActiveExplicitTransaction() == active,
                        "Actual transaction state differs from Idle/Active oracle");
                require(engine.transactionManager().activeTransactionId()
                            == (active ? transactionId : minidb::INVALID_TRANSACTION_ID),
                        "Transaction identity changed across statements or survived completion");
                require(server.bufferPool().stats().pinnedFrames == 0,
                        "SQL operation leaked a buffer pin");
                if (operation % 37 == 0) compareRows(engine, visible);
                if (operation % 251 == 0) {
                    server.catalog().validate();
                    server.pageAllocator().validate();
                }
            } catch (const std::exception& error) {
                throw std::runtime_error("seed=" + std::to_string(seed)
                    + " operation=" + std::to_string(operation) + " kind=" + std::to_string(kind)
                    + " key=" + std::to_string(key) + " active=" + std::to_string(active)
                    + ": " + error.what());
            }
        }
        engine.closeSession(OWNER);
        visible = committed;
        compareRows(engine, committed);
        server.catalog().validate();
        server.pageAllocator().validate();
    }
    minidb::net::DatabaseServer reopened(database.path().string(), configuration(seed));
    compareRows(reopened.sqlEngine(), committed);
    require(!reopened.sqlEngine().transactionManager().hasActiveExplicitTransaction(),
            "Reopen retained an active SQL transaction");
    reopened.catalog().validate();
    reopened.pageAllocator().validate();
}

} // namespace

int main() {
    try {
        for (const auto seed : SEEDS) runSeed(seed);
        std::cout << "explicit_transaction_model_test passed (20000 engine operations; "
                     "seeds 0x12A10001..0x12A10004)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "explicit_transaction_model_test failed: " << error.what() << '\n';
        return 1;
    }
}
