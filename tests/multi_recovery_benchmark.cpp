#include "multi_recovery_fixture.hpp"
#include "minidb/checkpoint_manager.hpp"

#include <iostream>

namespace {
using namespace minidb;
void retention() {
    test::TemporaryDatabase db("att_retention_benchmark");
    DiskManager disk(db.path().string());
    const auto oldPage = disk.appendPage(), newerPage = disk.appendPage();
    LogManager log(walPathForDatabase(db.path().string()), 4096, LogOpenMode::EagerValidated,
                   WalStorageMode::Segmented, 9000);
    RecoveryCoordinator recovery(disk, log, 1, WalUpdateMode::ByteRange);
    BufferPoolManager pool(disk, 4, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    CheckpointControl control(db.path().string() + ".ckpt");
    CheckpointManager checkpoints(recovery, pool, disk, log, control, {}, {0, 0, CheckpointMode::Fuzzy});
    const auto old = recovery.beginTransaction();
    recovery.beginMutation(old);
    { auto page = pool.fetchPageWrite(oldPage); page->data()[80] = std::byte{1}; }
    recovery.prepareTransaction(old);
    const auto oldBegin = recovery.checkpointTransactions().front().beginLsn;
    for (std::size_t checkpoint = 0; checkpoint < 10; ++checkpoint) {
        for (std::size_t i = 0; i < 100; ++i) {
            const auto id = recovery.beginTransaction();
            recovery.beginMutation(id);
            { auto page = pool.fetchPageWrite(newerPage);
              byte_codec::writeUint64(page->data(), 80, checkpoint * 100 + i + 1); }
            recovery.commitTransaction(id);
        }
        pool.flushAll();
        static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, DatabaseAccessGate::Lease{}));
        test::require(checkpoints.stats().retentionFloorLsn == oldBegin, "Lost active BEGIN history");
    }
    const auto before = log.stats();
    const auto beforeFloor = checkpoints.stats().retentionFloorLsn;
    recovery.commitTransaction(old);
    static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy));
    const auto after = log.stats();
    test::require(checkpoints.stats().retentionFloorLsn > beforeFloor, "Retention floor did not advance");
    std::cout << "retention checkpoints=10 completed_newer=1000 floor_before=" << beforeFloor
              << " floor_after=" << checkpoints.stats().retentionFloorLsn
              << " segments_before=" << before.retainedSegments << " segments_after=" << after.retainedSegments
              << " bytes_before=" << before.physicalWalBytes << " bytes_after=" << after.physicalWalBytes << '\n';
}
} // namespace

int main() {
    try {
        for (const auto count : {1U, 10U, 100U, 1000U}) {
            minidb::test::MultiHistory history(count, 0xB12B2);
            history.create(count, true);
            const auto stats = history.recoverAndCheck();
            std::cout << "analysis tx=" << count << " ns=" << stats.analysisNs
                      << " records=" << stats.recordsAnalyzed << " peak_att=" << stats.peakRecoveryTransactionTableSize
                      << " index_bytes=" << stats.recoveryLsnIndexBytes << '\n';
        }
        for (const auto count : {1U, 2U, 4U, 8U, 16U}) {
            minidb::test::MultiHistory history(count, 0xB12B2);
            history.create(128);
            const auto stats = history.recoverAndCheck();
            std::cout << "undo losers=" << count << " updates=128 ns=" << stats.undoNs
                      << " clrs=" << stats.clrsAppended << " wal_bytes=" << stats.undoWalBytes
                      << " queue_pops=" << stats.undoQueuePops << " page_writes=" << stats.undoPageWrites << '\n';
        }
        for (const auto progress : {32U, 64U, 96U}) {
            minidb::test::MultiHistory history(8, 0xB12B2);
            history.create(128);
            history.crashUndo(progress);
            const auto stats = history.recoverAndCheck();
            minidb::test::require(stats.analyzedClrCount == progress && stats.clrsAppended == 128 - progress,
                                  "Restart repeated CLR work");
            std::cout << "restart losers=8 progress_percent=" << progress * 100 / 128
                      << " completed_clrs=" << stats.analyzedClrCount << " remaining=" << stats.clrsAppended
                      << " restart_ns=" << stats.totalNs << '\n';
        }
        retention();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multi_recovery_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
