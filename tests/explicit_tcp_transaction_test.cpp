#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "test_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {

using namespace minidb;
using namespace minidb::net;
using minidb::test::require;

ServerConfig config() {
    ServerConfig result;
    result.port = 0;
    result.bufferFrames = 2; // Force WAL-backed eviction between SQL requests.
    result.checkpointWalBytes = 0;
    return result;
}

void initialize(const std::string& path) {
    DatabaseServer database(path, config());
    static_cast<void>(database.sqlEngine().execute(
        "CREATE TABLE records (id UINT32 PRIMARY KEY, value VARCHAR(1500) NOT NULL)"));
    static_cast<void>(database.sqlEngine().execute(
        "INSERT INTO records VALUES (1, 'original')"));
}

sql::CommandResult command(sql::QueryResult result, sql::CommandKind expected) {
    require(std::holds_alternative<sql::CommandResult>(result),
            "transaction command returned SELECT result");
    const auto value = std::get<sql::CommandResult>(std::move(result));
    require(value.command == expected, "transaction command tag did not round-trip");
    if (expected == sql::CommandKind::Begin || expected == sql::CommandKind::Commit
        || expected == sql::CommandKind::Rollback) {
        require(value.affectedRows == 0 && !value.insertedRecordId.has_value(),
                "transaction command fabricated affected rows or RID");
    }
    return value;
}

std::vector<RowValues> rows(sql::QueryResult result) {
    require(std::holds_alternative<sql::SelectResult>(result), "expected SELECT result");
    return std::get<sql::SelectResult>(std::move(result)).rows;
}

// A real TCP server process makes connection lifetime and hard-crash tests
// independent of shared in-process engine objects. RAII always reaps the child.
class ChildServer {
public:
    ChildServer(const std::string& path, std::size_t connections,
                const char* failpoint = nullptr) {
        int pipeDescriptors[2];
        if (::pipe(pipeDescriptors) != 0) throw std::runtime_error("pipe failed");
        process_ = ::fork();
        if (process_ < 0) {
            ::close(pipeDescriptors[0]);
            ::close(pipeDescriptors[1]);
            throw std::runtime_error("fork failed");
        }
        if (process_ == 0) {
            ::close(pipeDescriptors[0]);
            ::alarm(30);
            try {
                DatabaseServer server(path, config());
                server.start();
                if (failpoint != nullptr) ::setenv("MINIDB_FAILPOINT", failpoint, 1);
                const auto port = server.port();
                const auto written = ::write(pipeDescriptors[1], &port, sizeof(port));
                ::close(pipeDescriptors[1]);
                if (written != static_cast<ssize_t>(sizeof(port))) ::_exit(91);
                server.serve(connections);
                server.close();
                ::_exit(0);
            } catch (const std::exception& error) {
                std::cerr << "child TCP server failure: " << error.what() << '\n';
                ::_exit(92);
            }
        }
        ::close(pipeDescriptors[1]);
        std::size_t consumed = 0;
        auto* destination = reinterpret_cast<unsigned char*>(&port_);
        while (consumed < sizeof(port_)) {
            const auto count = ::read(pipeDescriptors[0], destination + consumed,
                                      sizeof(port_) - consumed);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            consumed += static_cast<std::size_t>(count);
        }
        ::close(pipeDescriptors[0]);
        if (consumed != sizeof(port_)) {
            terminate();
            throw std::runtime_error("child server failed before publishing port");
        }
    }

    ~ChildServer() { terminate(); }
    ChildServer(const ChildServer&) = delete;
    ChildServer& operator=(const ChildServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }

    void expectExit(int code) {
        const auto status = wait();
        require(WIFEXITED(status) && WEXITSTATUS(status) == code,
                "server did not exit at expected durability boundary");
    }

    void hardCrash() {
        require(::kill(process_, SIGKILL) == 0, "could not kill server process");
        const auto status = wait();
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
                "server did not hard crash");
    }

private:
    pid_t process_ = -1;
    std::uint16_t port_ = 0;

    int wait() {
        int status = 0;
        pid_t result;
        do { result = ::waitpid(process_, &status, 0); } while (result < 0 && errno == EINTR);
        process_ = -1;
        if (result < 0) throw std::runtime_error("waitpid failed");
        return status;
    }

    void terminate() noexcept {
        if (process_ <= 0) return;
        static_cast<void>(::kill(process_, SIGKILL));
        int status;
        while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {}
        process_ = -1;
    }
};

MiniDbClient connect(std::uint16_t port) {
    MiniDbClient client("127.0.0.1", port);
    client.connect();
    client.handshake();
    return client;
}

void requireBaseline(const std::string& path) {
    DatabaseServer reopened(path, config());
    require(rows(reopened.sqlEngine().execute("SELECT * FROM records"))
                == std::vector<RowValues>{{1U, std::string("original")}},
            "transaction rollback did not restore baseline");
    reopened.catalog().validate();
    reopened.pageAllocator().validate();
}

void testMultiRequestCommit() {
    minidb::test::TemporaryDatabase database("explicit_tcp_commit");
    const auto path = database.path().string();
    initialize(path);
    ChildServer server(path, 1);
    auto client = connect(server.port());
    command(client.execute("BEGIN"), sql::CommandKind::Begin);
    command(client.execute("INSERT INTO records VALUES (2, 'inserted')"),
            sql::CommandKind::Insert);
    command(client.execute("UPDATE records SET value = 'changed' WHERE id = 1"),
            sql::CommandKind::Update);
    const std::vector<RowValues> expected{{1U, std::string("changed")},
                                          {2U, std::string("inserted")}};
    require(rows(client.execute("SELECT * FROM records")) == expected,
            "same TCP transaction could not read its own changes");
    command(client.execute("COMMIT"), sql::CommandKind::Commit);
    client.close();
    server.expectExit(0);
    DatabaseServer reopened(path, config());
    require(rows(reopened.sqlEngine().execute("SELECT * FROM records")) == expected,
            "multi-request TCP COMMIT did not survive reopen");
}

void testDisconnectRollback() {
    minidb::test::TemporaryDatabase database("explicit_tcp_disconnect");
    const auto path = database.path().string();
    initialize(path);
    ChildServer server(path, 2);
    {
        auto first = connect(server.port());
        command(first.execute("BEGIN TRANSACTION"), sql::CommandKind::Begin);
        command(first.execute("INSERT INTO records VALUES (2, 'discard')"),
                sql::CommandKind::Insert);
        command(first.execute("UPDATE records SET value = 'discard' WHERE id = 1"),
                sql::CommandKind::Update);
        first.close();
    }
    {
        auto second = connect(server.port());
        require(rows(second.execute("SELECT * FROM records"))
                    == std::vector<RowValues>{{1U, std::string("original")}},
                "next TCP session saw disconnected owner's mutations");
        minidb::test::requireThrows<RemoteSqlError>(
            [&] { static_cast<void>(second.execute("COMMIT")); },
            "next TCP session inherited disconnected owner's transaction");
        command(second.execute("BEGIN"), sql::CommandKind::Begin);
        command(second.execute("ROLLBACK"), sql::CommandKind::Rollback);
        second.close();
    }
    server.expectExit(0);
    requireBaseline(path);
}

Socket connectRaw(std::uint16_t port) {
    Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket) throw std::runtime_error("socket creation failed");
    configureSocketForSafeWrites(socket.get());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("raw TCP connect failed");
    }
    writeFrame(socket.get(), makeHelloFrame());
    const auto hello = readFrame(socket.get());
    require(hello.has_value() && hello->header.messageType == MessageType::HelloAck,
            "raw TCP handshake failed");
    return socket;
}

void testProtocolFailureRollback() {
    minidb::test::TemporaryDatabase database("explicit_tcp_protocol_error");
    const auto path = database.path().string();
    initialize(path);
    ChildServer server(path, 2);
    {
        auto socket = connectRaw(server.port());
        for (const auto& [id, source] : {
                 std::pair{1ULL, "BEGIN"},
                 std::pair{2ULL, "UPDATE records SET value = 'discard' WHERE id = 1"}}) {
            writeFrame(socket.get(), makeExecuteSqlFrame(id, source));
            const auto response = readFrame(socket.get());
            require(response.has_value()
                        && response->header.messageType == MessageType::CommandResult,
                    "raw transaction command failed");
        }
        writeFrame(socket.get(), makeHelloFrame()); // Invalid after handshake.
        const auto error = readFrame(socket.get());
        require(error.has_value() && error->header.messageType == MessageType::ErrorResponse,
                "malformed session did not return protocol failure");
    }
    auto client = connect(server.port());
    require(rows(client.execute("SELECT * FROM records"))
                == std::vector<RowValues>{{1U, std::string("original")}},
            "protocol failure did not rollback owning session");
    client.close();
    server.expectExit(0);
    requireBaseline(path);
}

void testOwnershipAndShutdown() {
    minidb::test::TemporaryDatabase database("explicit_session_owner");
    const auto path = database.path().string();
    initialize(path);
    for (const bool explicitClose : {false, true}) {
        {
            DatabaseServer server(path, config());
            auto& engine = server.sqlEngine();
            command(engine.execute("BEGIN", 101), sql::CommandKind::Begin);
            command(engine.execute("UPDATE records SET value = 'discard' WHERE id = 1", 101),
                    sql::CommandKind::Update);
            for (const auto* source : {"COMMIT", "ROLLBACK", "SELECT * FROM records"}) {
                minidb::test::requireThrows<sql::SqlExecutionError>(
                    [&] { static_cast<void>(engine.execute(source, 102)); },
                    "non-owner accessed explicit transaction");
            }
            engine.closeSession(102);
            require(rows(engine.execute("SELECT * FROM records", 101))
                        == std::vector<RowValues>{{1U, std::string("discard")}},
                    "unrelated session cleanup modified owner transaction");
            if (explicitClose) server.close();
        }
        requireBaseline(path);
    }
}

void testHardCrashBoundaries() {
    for (const auto* failpoint : {"", "before_commit_append", "after_commit_sync"}) {
        minidb::test::TemporaryDatabase database("explicit_tcp_crash");
        const auto path = database.path().string();
        initialize(path);
        const bool committed = std::string_view(failpoint) == "after_commit_sync";
        ChildServer server(path, 1, *failpoint == '\0' ? nullptr : failpoint);
        auto client = connect(server.port());
        command(client.execute("BEGIN"), sql::CommandKind::Begin);
        command(client.execute("UPDATE records SET value = 'changed' WHERE id = 1"),
                sql::CommandKind::Update);
        const std::string payload(1'000, 'x');
        for (std::uint32_t id = 2; id <= 20; ++id) {
            command(client.execute("INSERT INTO records VALUES (" + std::to_string(id)
                                       + ", '" + payload + "')"),
                    sql::CommandKind::Insert);
        }
        if (*failpoint == '\0') {
            server.hardCrash();
        } else {
            minidb::test::requireThrows<NetworkError>(
                [&] { static_cast<void>(client.execute("COMMIT")); },
                "commit crash unexpectedly sent successful response");
            server.expectExit(86);
        }
        client.close();
        DatabaseServer reopened(path, config());
        const auto actual = rows(reopened.sqlEngine().execute("SELECT * FROM records"));
        require(actual.size() == (committed ? 20U : 1U),
                "TCP transaction was partially recovered after process crash");
        require(actual.front() == RowValues{1U, std::string(committed ? "changed" : "original")},
                "TCP transaction changed baseline across wrong commit boundary");
        if (committed) {
            for (std::uint32_t id = 2; id <= 20; ++id) {
                require(rows(reopened.sqlEngine().execute("SELECT * FROM records WHERE id = "
                                                         + std::to_string(id)))
                            == std::vector<RowValues>{{id, payload}},
                        "committed TCP tuple payload or primary index changed");
            }
        }
        reopened.catalog().validate();
        reopened.pageAllocator().validate();
    }
}

} // namespace

int main() {
    try {
        testMultiRequestCommit();
        testDisconnectRollback();
        testProtocolFailureRollback();
        testOwnershipAndShutdown();
        testHardCrashBoundaries();
        std::cout << "explicit TCP transaction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "explicit TCP transaction test failure: " << error.what() << '\n';
        return 1;
    }
}
