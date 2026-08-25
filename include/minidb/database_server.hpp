#pragma once

#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/buffer_pool_manager.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/tcp_server.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_manager.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace minidb::net {

// Declaration order makes the ownership lifetime explicit: the socket server and
// SQL engine are destroyed before Catalog/PageAllocator, then the buffer pool flushes
// before its DiskManager is destroyed.
class DatabaseServer {
public:
    DatabaseServer(std::string databasePath, ServerConfig config);

    void start() { server_->start(); }
    void serve(std::size_t connectionLimit = 0) { server_->serve(connectionLimit); }
    void close() noexcept { server_->close(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return server_->port(); }
    [[nodiscard]] TcpServer& tcpServer() noexcept { return *server_; }
    [[nodiscard]] DiskManager& diskManager() noexcept { return diskManager_; }
    [[nodiscard]] LogManager& logManager() noexcept { return logManager_; }
    [[nodiscard]] RecoveryCoordinator& recoveryCoordinator() noexcept { return *recovery_; }
    [[nodiscard]] CheckpointManager& checkpointManager() noexcept { return *checkpoints_; }
    [[nodiscard]] CheckpointControl& checkpointControl() noexcept { return checkpointControl_; }
    [[nodiscard]] BufferPoolManager& bufferPool() noexcept { return *bufferPool_; }
    [[nodiscard]] PageAllocator& pageAllocator() noexcept { return *allocator_; }
    [[nodiscard]] Catalog& catalog() noexcept { return *catalog_; }
    [[nodiscard]] sql::SqlEngine& sqlEngine() noexcept { return *engine_; }
    [[nodiscard]] const RecoveryStats& startupRecoveryStats() const noexcept {
        return recoveryStats_;
    }

private:
    DiskManager diskManager_;
    LogManager logManager_;
    CheckpointControl checkpointControl_;
    RecoveryStats recoveryStats_{};
    std::unique_ptr<RecoveryCoordinator> recovery_;
    std::unique_ptr<BufferPoolManager> bufferPool_;
    std::unique_ptr<CheckpointManager> checkpoints_;
    std::unique_ptr<DatabaseMetadataManager> metadataManager_;
    std::unique_ptr<PageAllocator> allocator_;
    std::optional<Catalog> catalog_;
    std::unique_ptr<sql::SqlEngine> engine_;
    std::unique_ptr<TcpServer> server_;
};

} // namespace minidb::net
