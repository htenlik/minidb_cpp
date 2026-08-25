#include "minidb/database_server.hpp"
#include "minidb/recovery_log.hpp"
#include "minidb/sql_error.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <unordered_map>
#include <variant>

namespace {

using minidb::test::require;

void verifyTransactionChains(const std::vector<minidb::LogRecord>& records) {
    std::unordered_map<minidb::TransactionId, minidb::Lsn> previous;
    for (const auto& record : records) {
        minidb::validateTransactionRecordPayload(record);
        if (record.type == minidb::LogRecordType::Begin) {
            require(!previous.contains(record.transactionId),
                    "Transaction WAL chain has duplicate BEGIN");
        } else {
            require(previous.contains(record.transactionId)
                        && record.prevLsn == previous.at(record.transactionId),
                    "Transaction prevLSN chain is not exact");
        }
        previous[record.transactionId] = record.lsn;
    }
}

void testStatementTransactionsAndReopen() {
    minidb::test::TemporaryDatabase database("statement_transactions");
    std::size_t afterInsertRecords = 0;
    {
        minidb::net::DatabaseServer server(
            database.path().string(), {"127.0.0.1", 0, 8, 4, 2});
        auto& engine = server.sqlEngine();
        static_cast<void>(engine.execute(
            "CREATE TABLE users (id UINT32 PRIMARY KEY, name VARCHAR(64) NOT NULL)"));
        static_cast<void>(engine.execute("INSERT INTO users VALUES (1, 'alice')"));
        const auto committed = server.logManager().scan().records;
        verifyTransactionChains(committed);
        require(committed.back().type == minidb::LogRecordType::Commit
                    && server.logManager().durableLsn() >= committed.back().lsn,
                "SQL success was visible before durable COMMIT");
        afterInsertRecords = committed.size();

        const auto selected = engine.execute("SELECT * FROM users");
        require(std::get<minidb::sql::SelectResult>(selected).rows.size() == 1,
                "SELECT did not observe committed INSERT");
        require(server.logManager().scan().records.size() == afterInsertRecords,
                "SELECT generated transaction WAL");

        try {
            static_cast<void>(engine.execute("INSERT INTO users VALUES (1, 'duplicate')"));
            throw std::runtime_error("Duplicate INSERT unexpectedly succeeded");
        } catch (const minidb::sql::SqlExecutionError&) {
        }
        require(server.logManager().scan().records.size() == afterInsertRecords,
                "Zero-mutation failed statement generated WAL");
        require(std::get<minidb::sql::SelectResult>(
                    engine.execute("SELECT * FROM users")).rows.size() == 1,
                "Failed statement changed visible table contents");

        static_cast<void>(engine.execute(
            "UPDATE users SET id = 2, name = 'updated' WHERE id = 1"));
        static_cast<void>(engine.execute("DELETE FROM users WHERE id = 2"));
        require(std::get<minidb::sql::SelectResult>(
                    engine.execute("SELECT * FROM users")).rows.empty(),
                "UPDATE primary-key change and DELETE did not commit");
        server.catalog().validate();
    }
    {
        minidb::net::DatabaseServer server(
            database.path().string(), {"127.0.0.1", 0, 8, 3, 2});
        require(std::get<minidb::sql::SelectResult>(
                    server.sqlEngine().execute("SELECT * FROM users")).rows.empty(),
                "Committed statement sequence did not survive reopen");
        require(server.recoveryCoordinator().nextTransactionId() > 1,
                "Transaction IDs were not resumed above existing WAL history");
        server.catalog().validate();
    }
}

void testOriginalBeforeImageIsStable() {
    minidb::test::TemporaryDatabase database("stable_before_image");
    minidb::net::DatabaseServer server(
        database.path().string(), {"127.0.0.1", 0, 8, 2, 2});
    static_cast<void>(server.sqlEngine().execute(
        "CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
    for (std::uint32_t key = 0; key < 40; ++key) {
        static_cast<void>(server.sqlEngine().execute(
            "INSERT INTO items VALUES (" + std::to_string(key) + ", 'value')"));
    }
    const auto records = server.logManager().scan().records;
    verifyTransactionChains(records);
    std::unordered_map<minidb::TransactionId,
                       std::unordered_map<minidb::PageId, minidb::DiskManager::Page>> originals;
    for (const auto& record : records) {
        if (record.type != minidb::LogRecordType::PageUpdate) continue;
        const auto update = minidb::decodePageUpdateLogPayload(record.payload);
        const auto [position, inserted] = originals[record.transactionId].emplace(
            update.pageId, update.beforeImage);
        require(inserted || position->second == update.beforeImage,
                "Repeated PAGE_UPDATE changed the transaction's original before-image");
    }
    server.catalog().validate();
}

} // namespace

int main() {
    try {
        testStatementTransactionsAndReopen();
        testOriginalBeforeImageIsStable();
        std::cout << "transaction_recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transaction_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
