#pragma once

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/checkpoint_types.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/segmented_wal.hpp"
#include "minidb/tcp_transport.hpp"
#include "minidb/wal_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace minidb::net {

inline constexpr std::uint16_t DEFAULT_SERVER_PORT = 7432;

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = DEFAULT_SERVER_PORT;
    int backlog = 16;
    std::size_t bufferFrames = 128;
    std::size_t lruK = 2;
    std::uint64_t checkpointWalBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t checkpointStatements = 0;
    std::uint32_t walSegmentBytes = wal_segment_layout::DEFAULT_PAYLOAD_CAPACITY;
    WalUpdateMode walUpdateMode = WalUpdateMode::FullPage;
    CheckpointMode checkpointMode = CheckpointMode::Sharp;
    std::size_t maxConnections = 16;
};

struct ServerConcurrencyStats {
    std::uint64_t activeSessions = 0;
    std::uint64_t peakActiveSessions = 0;
    std::uint64_t sessionsAccepted = 0;
    std::uint64_t connectionRejects = 0;
    std::uint64_t disconnectWhileWaiting = 0;
};

class TcpServer {
public:
    TcpServer(
        ServerConfig config,
        sql::SqlEngine& engine,
        BufferPoolManager& bufferPool,
        DiskManager& diskManager);
    ~TcpServer();

    void start();
    // A zero connection limit serves indefinitely. Tests use a finite limit.
    void serve(std::size_t connectionLimit = 0);
    void serveConnection(int connectedDescriptor);
    void close() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return boundPort_.load(); }
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] ServerConcurrencyStats concurrencyStats() const;

private:
    ServerConfig config_;
    sql::SqlEngine& engine_;
    Socket listener_;
    std::atomic<std::uint16_t> boundPort_{0};
    SessionId nextSessionId_ = 1; // Protected by lifecycleMutex_.
    std::atomic<bool> failed_{false};
    std::atomic<bool> stopping_{false};
    mutable std::mutex lifecycleMutex_;
    std::mutex serveMutex_;
    ServerConcurrencyStats stats_{};
    std::atomic<std::uint64_t> disconnectWhileWaiting_{0};
    std::unordered_set<int> connections_;
    struct Worker {
        Socket connection;
        std::atomic<bool> done{false};
        std::jthread thread; // Destroy first: join before destroying socket/state.
        explicit Worker(int descriptor) : connection(descriptor) {}
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    std::exception_ptr fatalError_;

    void serveSession(int descriptor, SessionId session);
    [[nodiscard]] std::optional<SessionId> registerConnection(int descriptor);
    void runConnection(int descriptor, SessionId session, std::stop_token stop = {});
    void unregisterConnection(int descriptor) noexcept;
    void requestStop() noexcept;
    void reapWorkers();
    void joinWorkers();

    void sendProtocolFailure(
        int descriptor,
        std::uint64_t requestId,
        std::string message) const noexcept;
};

} // namespace minidb::net
