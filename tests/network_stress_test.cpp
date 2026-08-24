#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace {

using namespace minidb;
using namespace minidb::net;

sql::SelectResult selection(sql::QueryResult result) {
    if (!std::holds_alternative<sql::SelectResult>(result)) {
        throw std::runtime_error("stress SELECT returned a command result");
    }
    return std::get<sql::SelectResult>(std::move(result));
}

void testSequentialRequestAndReconnectStress() {
    constexpr std::size_t requestCount = 2'000;
    constexpr std::size_t reconnectCadence = 250;
    constexpr std::size_t connectionCount = requestCount / reconnectCadence;
    minidb::test::TemporaryDatabase database("network_stress");
    {
        DatabaseServer server(database.path().string(), ServerConfig{"127.0.0.1", 0, 8});
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
                    static_cast<void>(client.execute(
                        "INSERT INTO stress VALUES (" + std::to_string(requests)
                        + ", " + std::to_string(requests) + ", TRUE)"));
                } else {
                    const auto key = static_cast<std::uint32_t>((requests % 100) + 1);
                    switch (requests % 5) {
                    case 0:
                        static_cast<void>(client.execute(
                            "SELECT * FROM stress WHERE id = " + std::to_string(key)));
                        break;
                    case 1:
                        static_cast<void>(client.execute(
                            "SELECT id FROM stress WHERE value = " + std::to_string(key)));
                        break;
                    case 2:
                        static_cast<void>(client.execute(
                            "UPDATE stress SET active = FALSE WHERE id = " + std::to_string(key)));
                        break;
                    case 3:
                        static_cast<void>(client.execute(
                            "DELETE FROM stress WHERE id = " + std::to_string(key)));
                        break;
                    case 4:
                        try {
                            static_cast<void>(client.execute(
                                "INSERT INTO stress VALUES (" + std::to_string(key)
                                + ", " + std::to_string(key) + ", TRUE)"));
                        } catch (const RemoteSqlError& error) {
                            if (error.category() != ErrorCategory::Constraint) throw;
                        }
                        break;
                    }
                }
                    if (requests != 0 && requests % 100 == 0) {
                        const auto& all = selection(client.execute(
                            "SELECT * FROM stress WHERE active = TRUE"));
                        minidb::test::require(all.stats.accessPath == sql::AccessPath::HeapScan,
                                              "stress scan reported the wrong access path");
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
    }

    // Reopen the complete owner once after the eight client reconnects.
    {
        Pager pager(database.path().string());
        auto catalog = Catalog::open(pager);
        sql::SqlEngine engine(catalog);
        const auto& rows = selection(engine.execute("SELECT * FROM stress"));
        minidb::test::require(rows.rows.size() <= 100,
                              "stress workload produced impossible live-row count");
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
