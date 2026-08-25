#include "minidb/database_server.hpp"

namespace minidb::net {

DatabaseServer::DatabaseServer(std::string databasePath, ServerConfig config)
    : diskManager_(databasePath),
      logManager_(walPathForDatabase(databasePath), LogManager::DEFAULT_BUFFER_SIZE,
                  LogOpenMode::DeferredRecovery, WalStorageMode::Auto,
                  config.walSegmentBytes),
      checkpointControl_(checkpointPathForDatabase(databasePath)) {
    recoveryStats_ = RecoveryManager(
        diskManager_, logManager_, &checkpointControl_).recover();
    recovery_ = std::make_unique<RecoveryCoordinator>(
        diskManager_, logManager_, recoveryStats_.nextTransactionId);
    bufferPool_ = std::make_unique<BufferPoolManager>(
        diskManager_, config.bufferFrames, config.lruK, &logManager_, recovery_.get());
    recovery_->attachBufferPool(*bufferPool_);
    checkpoints_ = std::make_unique<CheckpointManager>(
        *recovery_, *bufferPool_, diskManager_, logManager_, checkpointControl_,
        recoveryStats_, CheckpointPolicy{config.checkpointWalBytes, config.checkpointStatements});
    if (logManager_.legacyMigrationPending()) {
        static_cast<void>(checkpoints_->checkpoint());
        logManager_.migrateLegacyToSegmented();
    }
    metadataManager_ = std::make_unique<DatabaseMetadataManager>(
        diskManager_, *recovery_, logManager_);
    allocator_ = std::make_unique<PageAllocator>(
        *bufferPool_, diskManager_, metadataManager_.get());

    if (diskManager_.databaseHeader().catalogRootPageId == INVALID_PAGE_ID) {
        recovery_->beginStatement();
        try {
            catalog_.emplace(Catalog::openOrCreate(
                *bufferPool_, diskManager_, *allocator_));
            recovery_->commitStatement();
        } catch (...) {
            if (recovery_->hasActiveStatement()) recovery_->rollbackStatement();
            throw;
        }
    } else {
        catalog_.emplace(Catalog::open(*bufferPool_, diskManager_, *allocator_));
    }
    engine_ = std::make_unique<sql::SqlEngine>(
        *catalog_, recovery_.get(), checkpoints_.get());
    server_ = std::make_unique<TcpServer>(
        std::move(config), *engine_, *bufferPool_, diskManager_);
}

} // namespace minidb::net
