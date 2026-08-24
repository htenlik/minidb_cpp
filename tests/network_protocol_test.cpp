#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "minidb/tcp_transport.hpp"
#include "test_utils.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace minidb;
using namespace minidb::net;

const sql::CommandResult& command(const sql::QueryResult& result, sql::CommandKind kind) {
    minidb::test::require(std::holds_alternative<sql::CommandResult>(result),
                          "remote command returned the wrong result variant");
    const auto& value = std::get<sql::CommandResult>(result);
    minidb::test::require(value.command == kind, "remote command kind changed");
    return value;
}

sql::SelectResult selection(sql::QueryResult result) {
    minidb::test::require(std::holds_alternative<sql::SelectResult>(result),
                          "remote SELECT returned the wrong result variant");
    return std::get<sql::SelectResult>(std::move(result));
}

Socket connectRaw(std::uint16_t port) {
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket) throw std::runtime_error("test socket creation failed");
    configureSocketForSafeWrites(socket.get());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("test loopback connect failed");
    }
    return socket;
}

template <typename Work>
void runServer(
    const std::string& databasePath,
    std::size_t connections,
    Work&& work) {
    DatabaseServer server(databasePath, ServerConfig{"127.0.0.1", 0, 8});
    server.start();
    std::exception_ptr serverError;
    std::thread serverThread([&] {
        try {
            server.serve(connections);
        } catch (...) {
            serverError = std::current_exception();
        }
    });
    std::exception_ptr workError;
    try {
        work(server.port());
    } catch (...) {
        workError = std::current_exception();
    }
    serverThread.join();
    if (serverError) std::rethrow_exception(serverError);
    if (workError) std::rethrow_exception(workError);
}

void testEndToEndAndErrors() {
    minidb::test::TemporaryDatabase database("network_e2e");
    runServer(database.path().string(), 1, [&](std::uint16_t port) {
        MiniDbClient client("127.0.0.1", port);
        client.connect();
        client.handshake();
        command(client.execute(100,
            "CREATE TABLE users (id UINT32 PRIMARY KEY, "
            "name VARCHAR(64) NOT NULL, active BOOLEAN NOT NULL)"),
            sql::CommandKind::CreateTable);
        const auto first = command(client.execute(999,
            "INSERT INTO users VALUES (1, 'alice', TRUE)"), sql::CommandKind::Insert);
        minidb::test::require(first.insertedRecordId.has_value(),
                              "remote INSERT omitted its RecordId");
        command(client.execute(std::numeric_limits<std::uint64_t>::max(),
            "INSERT INTO users VALUES (2, 'bob', FALSE)"), sql::CommandKind::Insert);

        const auto& all = selection(client.execute("SELECT * FROM users"));
        minidb::test::require(
            all.columns == std::vector<std::string>{"id", "name", "active"},
            "end-to-end SELECT column metadata changed");
        minidb::test::require(all.rows == std::vector<RowValues>{
                                  {1U, std::string("alice"), true},
                                  {2U, std::string("bob"), false},
                              },
                              "end-to-end SELECT row values changed");
        minidb::test::require(all.recordIds.size() == 2,
                              "end-to-end SELECT source RIDs changed");

        const auto& indexed = selection(client.execute(
            "SELECT name FROM users WHERE id = 2"));
        minidb::test::require(
            indexed.rows == std::vector<RowValues>{{std::string("bob")}}
                && indexed.stats.accessPath == sql::AccessPath::PrimaryKeyLookup
                && indexed.stats.indexLookups == 1
                && indexed.stats.rowsExamined <= 1,
            "primary-key stats/result did not survive the wire protocol");
        const auto& scanned = selection(client.execute(
            "SELECT name FROM users WHERE name = 'bob'"));
        minidb::test::require(scanned.stats.accessPath == sql::AccessPath::HeapScan,
                              "heap-scan stats did not survive the wire protocol");

        command(client.execute("UPDATE users SET active = TRUE WHERE id = 2"),
                sql::CommandKind::Update);
        command(client.execute("DELETE FROM users WHERE id = 1"),
                sql::CommandKind::Delete);
        const auto& finalRows = selection(client.execute("SELECT * FROM users"));
        minidb::test::require(finalRows.rows == std::vector<RowValues>{
                                  {2U, std::string("bob"), true}},
                              "remote UPDATE/DELETE result changed");

        const auto expectError = [&](std::uint64_t requestId,
                                     std::string_view source,
                                     ErrorCategory expected) {
            try {
                static_cast<void>(client.execute(requestId, source));
            } catch (const RemoteSqlError& error) {
                minidb::test::require(error.requestId() == requestId
                                          && error.category() == expected
                                          && !error.message().empty()
                                          && error.span().has_value()
                                          && error.span()->begin.line >= 1
                                          && error.span()->begin.column >= 1,
                                      "remote SQL diagnostic lost structured fields");
                static_cast<void>(client.execute("SELECT * FROM users"));
                return;
            }
            throw std::runtime_error("invalid remote SQL did not fail");
        };
        expectError(301, "SELECT @ FROM users", ErrorCategory::Lexer);
        expectError(302, "SELECT FROM users", ErrorCategory::Parser);
        expectError(303, "SELECT * FROM missing", ErrorCategory::Semantic);
        expectError(304, "SELECT missing FROM users", ErrorCategory::Semantic);
        expectError(305, "INSERT INTO users VALUES (4294967296, 'x', TRUE)",
                    ErrorCategory::Constraint);
        expectError(306, "INSERT INTO users VALUES (2, 'duplicate', TRUE)",
                    ErrorCategory::Constraint);
        expectError(307,
                    "INSERT INTO users VALUES (3, 'this name is deliberately much longer than sixty-four bytes to fail validation', TRUE)",
                    ErrorCategory::Constraint);
        client.close();
    });
}

void testHandshakeAndBadClientIsolation() {
    minidb::test::TemporaryDatabase database("network_bad_clients");
    runServer(database.path().string(), 7, [&](std::uint16_t port) {
        {
            auto socket = connectRaw(port);
            writeFrame(socket.get(), makeExecuteSqlFrame(41, "SELECT * FROM x"));
            const auto response = readFrame(socket.get());
            minidb::test::require(response.has_value()
                                      && response->header.messageType == MessageType::ErrorResponse
                                      && response->header.requestId == 41
                                      && decodeErrorResponsePayload(response->payload).category
                                          == ErrorCategory::Protocol,
                                  "EXECUTE-before-HELLO was not rejected structurally");
        }
        {
            auto socket = connectRaw(port);
            auto bytes = encodeFrame(makeHelloFrame());
            bytes[5] = std::byte{2};
            writeAll(socket.get(), bytes);
            const auto response = readFrame(socket.get());
            minidb::test::require(response.has_value()
                                      && decodeErrorResponsePayload(response->payload).category
                                          == ErrorCategory::Protocol,
                                  "unsupported protocol version was not rejected");
        }
        {
            auto socket = connectRaw(port);
            auto bytes = encodeFrame(makeHelloFrame());
            bytes[0] = std::byte{'X'};
            writeAll(socket.get(), bytes);
            const auto response = readFrame(socket.get());
            minidb::test::require(response.has_value()
                                      && decodeErrorResponsePayload(response->payload).category
                                          == ErrorCategory::Protocol,
                                  "invalid magic was not rejected");
        }
        {
            auto socket = connectRaw(port);
            auto bytes = encodeFrameHeader({MessageType::ExecuteSql, 77,
                                             MAX_FRAME_PAYLOAD});
            bytes[16] = std::byte{0};
            bytes[17] = std::byte{4};
            bytes[18] = std::byte{0};
            bytes[19] = std::byte{1};
            writeAll(socket.get(), bytes);
            const auto response = readFrame(socket.get());
            minidb::test::require(response.has_value()
                                      && response->header.requestId == 77
                                      && decodeErrorResponsePayload(response->payload).category
                                          == ErrorCategory::Protocol,
                                  "over-limit declared payload was not rejected before allocation");
        }
        {
            auto socket = connectRaw(port);
            const auto bytes = encodeFrame(makeHelloFrame());
            writeAll(socket.get(), std::span(bytes).first(10));
            socket.reset();
        }
        {
            auto socket = connectRaw(port);
            writeFrame(socket.get(), makeHelloFrame());
            minidb::test::require(readFrame(socket.get()).has_value(),
                                  "valid HELLO did not receive HELLO_ACK");
            writeFrame(socket.get(), makeHelloFrame());
            const auto response = readFrame(socket.get());
            minidb::test::require(response.has_value()
                                      && decodeErrorResponsePayload(response->payload).category
                                          == ErrorCategory::Protocol,
                                  "repeated HELLO was not a fatal protocol error");
        }
        {
            MiniDbClient good("127.0.0.1", port);
            good.connect();
            good.handshake();
            command(good.execute("CREATE TABLE healthy (id UINT32 PRIMARY KEY)"),
                    sql::CommandKind::CreateTable);
            minidb::test::require(selection(good.execute("SELECT * FROM healthy")).rows.empty(),
                                  "bad clients damaged the subsequent healthy client");
            good.close();
        }
    });
}

void testSequentialClientsAndReopen() {
    minidb::test::TemporaryDatabase database("network_reopen");
    runServer(database.path().string(), 2, [&](std::uint16_t port) {
        MiniDbClient first("127.0.0.1", port);
        first.connect(); first.handshake();
        static_cast<void>(first.execute("CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(32))"));
        static_cast<void>(first.execute("INSERT INTO items VALUES (1, 'one')"));
        first.close();

        MiniDbClient second("127.0.0.1", port);
        second.connect(); second.handshake();
        minidb::test::require(selection(second.execute("SELECT * FROM items")).rows.size() == 1,
                              "second sequential client did not see first client state");
        static_cast<void>(second.execute("INSERT INTO items VALUES (2, 'two')"));
        second.close();
    });
    runServer(database.path().string(), 1, [&](std::uint16_t port) {
        MiniDbClient third("127.0.0.1", port);
        third.connect(); third.handshake();
        const auto& rows = selection(third.execute("SELECT * FROM items"));
        minidb::test::require(rows.rows == std::vector<RowValues>{
                                  {1U, std::string("one")},
                                  {2U, std::string("two")}},
                              "server/database owner reopen lost committed state");
        static_cast<void>(third.execute("UPDATE items SET value = 'TWO' WHERE id = 2"));
        third.close();
    });
    runServer(database.path().string(), 1, [&](std::uint16_t port) {
        MiniDbClient fourth("127.0.0.1", port);
        fourth.connect(); fourth.handshake();
        minidb::test::require(selection(fourth.execute(
                                  "SELECT value FROM items WHERE id = 2")).rows
                                  == std::vector<RowValues>{{std::string("TWO")}},
                              "second server reopen lost remote UPDATE");
        fourth.close();
    });
}

void testLargeResultBoundary() {
    minidb::test::TemporaryDatabase database("network_result_limit");
    runServer(database.path().string(), 1, [&](std::uint16_t port) {
        MiniDbClient client("127.0.0.1", port);
        client.connect(); client.handshake();
        static_cast<void>(client.execute(
            "CREATE TABLE payloads (id UINT32 PRIMARY KEY, value VARCHAR(4000) NOT NULL)"));
        const std::string value(3900, 'x');
        for (std::uint32_t id = 0; id < 70; ++id) {
            static_cast<void>(client.execute(
                "INSERT INTO payloads VALUES (" + std::to_string(id) + ", '" + value + "')"));
        }
        const auto& below = selection(client.execute("SELECT * FROM payloads WHERE id < 60"));
        minidb::test::require(below.rows.size() == 60,
                              "valid near-limit result did not arrive intact");
        try {
            static_cast<void>(client.execute(8080, "SELECT * FROM payloads"));
        } catch (const RemoteSqlError& error) {
            minidb::test::require(error.requestId() == 8080
                                      && error.category() == ErrorCategory::Execution,
                                  "oversized result returned the wrong structured error");
            minidb::test::require(selection(client.execute(
                                      "SELECT id FROM payloads WHERE id = 1")).rows.size() == 1,
                                  "oversized result made the connection unusable");
            client.close();
            return;
        }
        throw std::runtime_error("over-limit materialized result was not rejected");
    });
}

} // namespace

int main() {
    try {
        testEndToEndAndErrors();
        testHandshakeAndBadClientIsolation();
        testSequentialClientsAndReopen();
        testLargeResultBoundary();
        std::cout << "network protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "network protocol test failure: " << error.what() << '\n';
        return 1;
    }
}
