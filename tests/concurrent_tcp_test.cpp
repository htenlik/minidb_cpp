#include "minidb/database_server.hpp"
#include "minidb/minidb_client.hpp"
#include "concurrency_utils.hpp"

#include <arpa/inet.h>
#include <barrier>
#include <iostream>
#include <latch>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace {
using namespace minidb;
using namespace minidb::net;
using minidb::test::require;
using minidb::test::await;

ServerConfig configuration(std::size_t maximum = 16) {
    ServerConfig config;
    config.port = 0; config.bufferFrames = 4; config.maxConnections = maximum;
    config.checkpointWalBytes = 0;
    return config;
}
class Fixture {
public:
    minidb::test::TemporaryDatabase path{"concurrent_tcp"};
    DatabaseServer database;
    minidb::test::ThreadErrors errors;
    std::jthread listener;
    explicit Fixture(std::size_t maximum = 16, bool dataset = false)
        : database(path.path().string(), configuration(maximum)) {
        auto& engine = database.sqlEngine();
        static_cast<void>(engine.execute("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(1024))"));
        const auto rows = dataset ? 128 : 1;
        for (int row = 1; row <= rows; ++row) {
            static_cast<void>(engine.execute("INSERT INTO t VALUES (" + std::to_string(row)
                + ", '" + (dataset ? std::string(256, 'a') : std::string("original")) + "')"));
        }
        static_cast<void>(database.checkpointManager().checkpoint());
        database.start();
        listener = std::jthread([this] { errors.run([&] { database.serve(); }); });
    }
    ~Fixture() { database.close(); if (listener.joinable()) listener.join(); }
    void close() { database.close(); if (listener.joinable()) listener.join(); errors.rethrow(); }
    MiniDbClient client() {
        MiniDbClient result("127.0.0.1", database.port()); result.connect(); result.handshake(); return result;
    }
    DatabaseAccessGate& gate() { return database.sqlEngine().transactionManager().accessGate(); }
};
Socket rawClient(std::uint16_t port) {
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    require(static_cast<bool>(socket), "client socket failed");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "raw client connect failed");
    configureSocketForSafeWrites(socket.get());
    writeFrame(socket.get(), makeHelloFrame());
    const auto hello = readFrame(socket.get());
    require(hello && hello->header.messageType == MessageType::HelloAck, "raw handshake failed");
    return socket;
}
std::string selected(sql::QueryResult result) {
    return std::get<std::string>(std::get<sql::SelectResult>(result).rows.at(0).at(0));
}
void noResponse(int descriptor) {
    pollfd ready{descriptor, POLLIN, 0};
    require(::poll(&ready, 1, 0) == 0, "Blocked request responded before database lease became available");
}

void concurrentReaders() {
    Fixture fixture(16, true);
    fixture.gate().resetStats();
    minidb::test::ThreadErrors errors;
    std::latch begun(8), release(1);
    std::vector<std::jthread> readers;
    for (int thread = 0; thread < 8; ++thread) readers.emplace_back([&, thread] { errors.run([&] {
        auto client = fixture.client();
        static_cast<void>(client.execute("BEGIN READ ONLY"));
        begun.count_down(); release.wait();
        for (int operation = 0; operation < 12; ++operation) {
            const auto key = 1 + (operation + thread) % 128;
            require(selected(client.execute("SELECT value FROM t WHERE id = " + std::to_string(key)))
                        == std::string(256, 'a'), "Concurrent READ ONLY bytes changed");
        }
        static_cast<void>(client.execute(thread % 2 ? "COMMIT" : "ROLLBACK")); client.close();
    }); });
    begun.wait();
    require(fixture.gate().stats().activeReaders == 8
        && fixture.database.tcpServer().concurrencyStats().activeSessions == 8,
        "Real TCP read-only sessions did not coexist");
    release.count_down();
    for (auto& reader : readers) reader.join();
    errors.rethrow();
    await([&] { return fixture.database.tcpServer().concurrencyStats().activeSessions == 0; }, "Read-only sessions leaked");

    fixture.gate().resetStats(); readers.clear();
    std::barrier start(8);
    for (int thread = 0; thread < 8; ++thread) readers.emplace_back([&, thread] { errors.run([&] {
        auto client = fixture.client(); start.arrive_and_wait();
        for (int operation = 0; operation < 40; ++operation) {
            const auto key = 1 + (operation * 17 + thread) % 128;
            require(selected(client.execute("SELECT value FROM t WHERE id = " + std::to_string(key)))
                        == std::string(256, 'a'), "Concurrent autocommit SELECT changed RID/value association");
        }
        client.close();
    }); });
    for (auto& reader : readers) reader.join(); errors.rethrow();
    require(fixture.gate().stats().peakConcurrentReaders > 1,
            "Autocommit SELECT did not actually overlap according to lease instrumentation");
    std::cout << "TCP autocommit SELECT peak readers=" << fixture.gate().stats().peakConcurrentReaders << '\n';
    fixture.close();
}
void exclusionDisconnectAndCancellation() {
    Fixture fixture;
    auto owner = fixture.client();
    for (const auto terminal : {"COMMIT", "ROLLBACK", "DISCONNECT"}) {
        static_cast<void>(owner.execute("BEGIN"));
        static_cast<void>(owner.execute("UPDATE t SET value = 'pending' WHERE id = 1"));
        auto waiting = rawClient(fixture.database.port());
        writeFrame(waiting.get(), makeExecuteSqlFrame(77, "SELECT value FROM t WHERE id = 1"));
        await([&] { return fixture.gate().stats().waitingReaders == 1; }, "Writer did not block TCP reader");
        noResponse(waiting.get());
        if (std::string_view(terminal) == "DISCONNECT") owner.close();
        else static_cast<void>(owner.execute(terminal));
        const auto response = readFrame(waiting.get());
        require(response && selected(decodeQueryResultFrame(*response)) == "pending",
                "TCP reader saw uncommitted/intermediate rollback state");
        waiting.reset();
        if (!owner.connected()) owner = fixture.client();
    }
    static_cast<void>(owner.execute("BEGIN"));
    static_cast<void>(owner.execute("UPDATE t SET value = 'cancelled' WHERE id = 1"));
    for (const auto query : {"SELECT * FROM t", "BEGIN"}) {
        auto waiting = rawClient(fixture.database.port());
        writeFrame(waiting.get(), makeExecuteSqlFrame(88, query));
        await([&] { const auto stats = fixture.gate().stats(); return stats.waitingReaders + stats.waitingWriters == 1; },
              "TCP gate waiter was not queued");
        waiting.reset();
        await([&] { const auto stats = fixture.gate().stats(); return stats.waitingReaders + stats.waitingWriters == 0; },
              "Disconnected socket leaked a database waiter");
    }
    require(fixture.database.tcpServer().concurrencyStats().disconnectWhileWaiting >= 2,
            "Waiting disconnect metric missing");
    owner.close();
    auto fresh = fixture.client();
    require(selected(fresh.execute("SELECT value FROM t WHERE id = 1")) == "pending",
            "Disconnect exposed writer changes before durable rollback");
    require(fixture.database.recoveryCoordinator().lastRollbackStats().durableAbortObserved,
            "Disconnected writer did not reach durable ABORT");
    fresh.close(); fixture.close();
}
void connectionLimit() {
    Fixture fixture(2);
    auto first = fixture.client(), second = fixture.client();
    MiniDbClient third("127.0.0.1", fixture.database.port()); third.connect();
    minidb::test::requireThrows<RemoteSqlError>([&] { third.handshake(); }, "Connection limit was ignored");
    require(fixture.database.tcpServer().concurrencyStats().connectionRejects == 1, "Rejection counter incorrect");
    first.close();
    await([&] { return fixture.database.tcpServer().concurrencyStats().activeSessions == 1; }, "Closed capacity was not released");
    auto replacement = fixture.client();
    require(selected(replacement.execute("SELECT value FROM t WHERE id = 1")) == "original", "Connection capacity not reusable");
    second.close(); replacement.close(); third.close(); fixture.close();
}
void shutdown() {
    for (bool writer : {true, false}) {
        minidb::test::TemporaryDatabase db("concurrent_shutdown_reopen");
        {
            DatabaseServer server(db.path().string(), configuration());
            static_cast<void>(server.sqlEngine().execute("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(32))"));
            static_cast<void>(server.sqlEngine().execute("INSERT INTO t VALUES (1, 'original')"));
            server.start(); minidb::test::ThreadErrors errors;
            std::jthread listener([&] { errors.run([&] { server.serve(); }); });
            MiniDbClient owner("127.0.0.1", server.port()); owner.connect(); owner.handshake();
            static_cast<void>(owner.execute(writer ? "BEGIN" : "BEGIN READ ONLY"));
            if (writer) static_cast<void>(owner.execute("UPDATE t SET value = 'uncommitted' WHERE id = 1"));
            auto waiting = rawClient(server.port());
            writeFrame(waiting.get(), makeExecuteSqlFrame(90, writer ? "SELECT * FROM t" : "BEGIN"));
            await([&] { auto stats = server.sqlEngine().transactionManager().accessGate().stats();
                return stats.waitingReaders + stats.waitingWriters == 1; }, "Shutdown waiter was not blocked");
            server.close(); listener.join(); errors.rethrow();
            const auto stats = server.sqlEngine().transactionManager().accessGate().stats();
            require(stats.activeReaders == 0 && !stats.writerActive && stats.waitingReaders == 0 && stats.waitingWriters == 0,
                    "Shutdown leaked active leases/waiters");
            require(server.tcpServer().concurrencyStats().activeSessions == 0, "Shutdown did not join session cleanup");
        }
        DatabaseServer reopened(db.path().string(), configuration());
        require(selected(reopened.sqlEngine().execute("SELECT value FROM t WHERE id = 1")) == "original",
                "Shutdown writer rollback was not durable");
        reopened.catalog().validate(); reopened.pageAllocator().validate();
    }
}
}
int main() {
    try { concurrentReaders(); exclusionDisconnectAndCancellation(); connectionLimit(); shutdown();
        std::cout << "concurrent_tcp_test passed (8 clients, real overlap, waiting disconnects, writer rollback, connection cap, shutdown)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
