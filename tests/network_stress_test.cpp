#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace minidb;
using namespace minidb::net;

sql::SelectResult selection(sql::QueryResult result) {
    if (!std::holds_alternative<sql::SelectResult>(result)) {
        throw std::runtime_error("stress SELECT returned a command result");
    }
    return std::get<sql::SelectResult>(std::move(result));
}

sql::CommandResult command(sql::QueryResult result) {
    if (!std::holds_alternative<sql::CommandResult>(result)) {
        throw std::runtime_error("stress command returned a SELECT result");
    }
    return std::get<sql::CommandResult>(std::move(result));
}

void testSequentialRequestAndReconnectStress() {
    constexpr std::size_t requestCount = 2'000;
    constexpr std::size_t reconnectCadence = 250;
    constexpr std::size_t connectionCount = requestCount / reconnectCadence;
    minidb::test::TemporaryDatabase database("network_stress");
    std::array<bool, 101> live{};
    std::array<bool, 101> active{};
    {
        DatabaseServer server(
            database.path().string(), ServerConfig{"127.0.0.1", 0, 8, 3, 2});
        server.start();
        std::exception_ptr serverError;
        std::thread serverThread([&] {
            try { server.serve(connectionCount); }
            catch (...) { serverError = std::current_exception(); }
        });

        std::exception_ptr workError;
        try {
            MiniDbClient client("127.0.0.1", server.port());
            std::size_t requests = 0;
            for (std::size_t connection = 0; connection < connectionCount; ++connection) {
                client.connect();
                client.handshake();
                for (std::size_t local = 0; local < reconnectCadence; ++local, ++requests) {
                if (requests == 0) {
                    static_cast<void>(client.execute(
                        "CREATE TABLE stress (id UINT32 PRIMARY KEY, value INT64, active BOOLEAN)"));
                } else if (requests <= 100) {
                    minidb::test::require(command(client.execute(
                        "INSERT INTO stress VALUES (" + std::to_string(requests)
                        + ", " + std::to_string(requests) + ", TRUE)"))
                                              .affectedRows == 1,
                                          "modeled initial INSERT affected-row mismatch");
                    live[requests] = true;
                    active[requests] = true;
                } else {
                    const auto key = static_cast<std::uint32_t>(((requests / 5) % 100) + 1);
                    switch (requests % 5) {
                    case 0: {
                        const auto result = selection(client.execute(
                            "SELECT * FROM stress WHERE id = " + std::to_string(key)));
                        minidb::test::require(result.rows.size() == (live[key] ? 1U : 0U),
                                              "modeled PK lookup cardinality mismatch");
                        if (live[key]) {
                            minidb::test::require(
                                result.rows[0] == RowValues{
                                    key, static_cast<std::int64_t>(key), active[key]},
                                "modeled PK lookup payload mismatch");
                        }
                        break;
                    }
                    case 1: {
                        const auto result = selection(client.execute(
                            "SELECT id FROM stress WHERE value = " + std::to_string(key)));
                        minidb::test::require(result.rows.size() == (live[key] ? 1U : 0U),
                                              "modeled heap lookup cardinality mismatch");
                        if (live[key]) {
                            minidb::test::require(result.rows[0] == RowValues{key},
                                                  "modeled heap lookup payload mismatch");
                        }
                        break;
                    }
                    case 2: {
                        const auto result = command(client.execute(
                            "UPDATE stress SET active = FALSE WHERE id = " + std::to_string(key)));
                        minidb::test::require(result.affectedRows == (live[key] ? 1U : 0U),
                                              "modeled UPDATE affected-row mismatch");
                        if (live[key]) active[key] = false;
                        break;
                    }
                    case 3: {
                        const auto result = command(client.execute(
                            "DELETE FROM stress WHERE id = " + std::to_string(key)));
                        minidb::test::require(result.affectedRows == (live[key] ? 1U : 0U),
                                              "modeled DELETE affected-row mismatch");
                        live[key] = false;
                        active[key] = false;
                        break;
                    }
                    case 4:
                        try {
                            minidb::test::require(command(client.execute(
                                "INSERT INTO stress VALUES (" + std::to_string(key)
                                + ", " + std::to_string(key) + ", TRUE)"))
                                                      .affectedRows == 1 && !live[key],
                                                  "modeled replacement INSERT mismatch");
                            live[key] = true;
                            active[key] = true;
                        } catch (const RemoteSqlError& error) {
                            if (error.category() != ErrorCategory::Constraint || !live[key]) throw;
                        }
                        break;
                    }
                }
                    if (requests != 0 && requests % 100 == 0) {
                        const auto& all = selection(client.execute(
                            "SELECT * FROM stress WHERE active = TRUE"));
                        std::vector<std::uint32_t> actualKeys;
                        for (const auto& row : all.rows) {
                            actualKeys.push_back(std::get<std::uint32_t>(row[0]));
                        }
                        std::sort(actualKeys.begin(), actualKeys.end());
                        std::vector<std::uint32_t> expectedKeys;
                        for (std::uint32_t candidate = 1; candidate <= 100; ++candidate) {
                            if (live[candidate] && active[candidate]) {
                                expectedKeys.push_back(candidate);
                            }
                        }
                        minidb::test::require(all.stats.accessPath == sql::AccessPath::HeapScan,
                                              "stress scan reported the wrong access path");
                        minidb::test::require(actualKeys == expectedKeys,
                                              "periodic stress scan disagreed with model");
                    }
                }
                client.close();
            }
        } catch (...) {
            workError = std::current_exception();
        }
        serverThread.join();
        if (serverError) std::rethrow_exception(serverError);
        if (workError) std::rethrow_exception(workError);
        minidb::test::require(
            server.bufferPool().stats().pinnedFrames == 0,
            "TCP request processing leaked page pins");
        server.bufferPool().validate();
    }

    // Reopen the complete owner once after the eight client reconnects.
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = Catalog::open(
            storage.bufferPool, storage.diskManager, storage.allocator);
        sql::SqlEngine engine(catalog);
        const auto& rows = selection(engine.execute("SELECT * FROM stress"));
        std::array<bool, 101> reopenedLive{};
        std::array<bool, 101> reopenedActive{};
        for (const auto& row : rows.rows) {
            const auto key = std::get<std::uint32_t>(row[0]);
            minidb::test::require(key <= 100 && !reopenedLive[key]
                                      && std::get<std::int64_t>(row[1]) == key,
                                  "reopened stress row was invalid or duplicated");
            reopenedLive[key] = true;
            reopenedActive[key] = std::get<bool>(row[2]);
        }
        minidb::test::require(reopenedLive == live && reopenedActive == active,
                              "reopened database disagreed with stress model");
        catalog.validate();
    }
}

} // namespace

int main() {
    try {
        testSequentialRequestAndReconnectStress();
        std::cout << "network stress tests passed (2019 SQL requests, 8 connections, 1 reopen)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "network stress test failure: " << error.what() << '\n';
        return 1;
    }
}
