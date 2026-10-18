#include "writeback_fixture.hpp"
#include "page_reclaim_fixture.hpp"
#include "minidb/checkpoint_log.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <sys/wait.h>
#include <thread>

namespace {
using namespace minidb;
using test::require;
void hardExit(pid_t child) {
    int status = 0;
    require(child > 0 && ::waitpid(child, &status, 0) == child
        && WIFEXITED(status) && WEXITSTATUS(status) == 86, "Child did not reach hard-crash boundary");
}

void tracker() {
    test::TemporaryDatabase db("writeback_tracker"), shadow("writeback_tracker_shadow");
    DiskManager disk(db.path().string());
    require(disk.writebackStats().physicalWriteEpoch == 1 && disk.writebackStats().pending(), "Initialization not tracked");
    test::StableImage image(db.path().string(), shadow.path().string()); image.attach(disk);
    require(disk.syncPendingWritebacks(), "Initial pending write not synced");
    require(!disk.syncPendingWritebacks(), "No-pending sync was not skipped");
    for (unsigned n = 0; n < 4; ++n) static_cast<void>(disk.appendPage());
    const auto start = disk.writebackStats();
    DiskManager::Page bytes{}; bytes[100] = std::byte{0x7A};
    disk.writePage(1, bytes); disk.writePhysicalPage(2, bytes);
    disk.updateCatalogRootPageId(1); disk.writePage(3, bytes);
    require(disk.writebackStats().physicalWriteEpoch == start.physicalWriteEpoch + 4, "Writes bypassed/doubled frontier");
    const auto beforeSync = disk.writebackStats();
    disk.flush();
    require(disk.writebackStats().synchronizedWriteEpoch == beforeSync.synchronizedWriteEpoch, "Stream flush advanced frontier");
    require(disk.syncPendingWritebacks() && !disk.syncPendingWritebacks(), "Batch conditional sync failed");
    const auto synced = disk.writebackStats();
    require(synced.synchronizedWriteEpoch == synced.physicalWriteEpoch
        && synced.databaseSyncsCompleted == start.databaseSyncsCompleted + 1, "Batch issued wrong fsync count");
    disk.writePhysicalPage(4, bytes);
    image.failNext = true;
    test::requireThrows<std::runtime_error>([&] { static_cast<void>(disk.syncPendingWritebacks()); }, "EIO accepted");
    require(disk.writebackStats().pending()
        && disk.writebackStats().synchronizedWriteEpoch == synced.synchronizedWriteEpoch
        && disk.writebackStats().databaseSyncFailures == synced.databaseSyncFailures + 1, "EIO advanced/reset frontier");
    require(disk.syncPendingWritebacks(), "EIO retry failed");
    auto epoch = disk.writebackStats().physicalWriteEpoch;
    disk.truncateToPageCount(3);
    require(disk.writebackStats().physicalWriteEpoch == epoch + 1, "Truncation untracked");
    disk.sync(); epoch = disk.writebackStats().physicalWriteEpoch;
    disk.writePhysicalPage(5, bytes); // Three zero-page appends, then one image.
    require(disk.writebackStats().physicalWriteEpoch == epoch + 4, "Recovery extension accounting wrong");
    disk.sync();
    const auto before = test::fileBytes(db.path().string());
    DiskManagerTestAccess::exhaustEpoch(disk);
    test::requireThrows<std::overflow_error>([&] { disk.writePage(1, bytes); }, "Write epoch wrapped");
    test::requireThrows<std::overflow_error>([&] { static_cast<void>(disk.appendPage()); }, "Append epoch wrapped");
    test::requireThrows<std::overflow_error>([&] { disk.truncateToPageCount(2); }, "Truncate epoch wrapped");
    require(test::fileBytes(db.path().string()) == before, "Overflow mutated database");
    test::TemporaryDatabase broken("writeback_failure"); DiskManager failed(broken.path().string());
    const auto id = failed.appendPage(); const auto old = failed.writebackStats().physicalWriteEpoch;
    DiskManagerTestAccess::breakStream(failed);
    test::requireThrows<std::runtime_error>([&] { failed.writePage(id, bytes); }, "Failed write accepted");
    require(failed.writebackStats().writeFailure && failed.writebackStats().physicalWriteEpoch == old, "Partial failure counted complete");
    test::requireThrows<std::runtime_error>([&] { failed.sync(); }, "Poisoned stream synchronized");
    test::TemporaryDatabase reopened("writeback_reopened");
    { DiskManager first(reopened.path().string()); static_cast<void>(first.appendPage()); }
    DiskManager second(reopened.path().string());
    require(second.writebackStats().initialSyncRequired && second.writebackStats().physicalWriteEpoch == 0,
        "Reopen fabricated a write or inferred stable storage");
    require(second.syncPendingWritebacks() && !second.syncPendingWritebacks(), "Initial open uncertainty not fenced");
}

void checkpointSnapshot(test::WritebackFixture& f) {
    const auto selected = f.control.select(f.log);
    require(selected.slot.has_value(), "No authoritative checkpoint");
    const auto record = f.log.readRecordAt(selected.slot->checkpointEndLsn);
    if (record.type == LogRecordType::FuzzyCheckpointEnd) {
        const auto snapshot = decodeFuzzyCheckpointEndLogPayload(record.payload);
        const auto live = f.pool.dirtyPageTableSnapshot();
        require(snapshot.dirtyPages.size() == live.size()
            && snapshot.activeTransactions == f.recovery.checkpointTransactions(), "Checkpoint DPT/ATT differs from live boundary");
        for (std::size_t i = 0; i < live.size(); ++i) {
            require(snapshot.dirtyPages[i].pageId == live[i].pageId
                && snapshot.dirtyPages[i].recLsn == live[i].recLsn
                && f.checkpoints.stats().retentionFloorLsn <= live[i].recLsn,
                "Snapshot lost dirty-period retention");
        }
    } else require(f.pool.dirtyPageTableSnapshot().empty(), "Sharp checkpoint left DPT");
    require(f.log.oldestRetainedLsn() <= f.checkpoints.stats().retentionFloorLsn,
        "Reclamation passed required retained floor");
}

void systemSync() {
    test::TemporaryDatabase db("frontier_reclaim_system_sync");
    test::ReclaimFixture f(db.path().string()); f.initialize();
    const auto before = f.disk.writebackStats();
    static_cast<void>(f.reclaim(30));
    const auto after = f.disk.writebackStats();
    require(!after.pending() && after.physicalWriteEpoch == before.physicalWriteEpoch + 2
        && after.databaseSyncsCompleted == before.databaseSyncsCompleted + 1,
        "PAGE_RECLAIM did not share physical frontier");
}

std::atomic<bool> syncEntered{false}, releaseSync{false}, writerFinished{false};
int blockedSync(int fd) {
    syncEntered = true;
    while (!releaseSync.load()) std::this_thread::yield();
    return ::fsync(fd);
}
void serialization() {
    syncEntered = false; releaseSync = false; writerFinished = false;
    test::TemporaryDatabase db("frontier_serialized_sync"); DiskManager disk(db.path().string());
    const auto page = disk.appendPage();
    DiskManagerTestAccess::syncFunction(disk, blockedSync);
    std::thread synchronizer([&] { disk.sync(); });
    while (!syncEntered.load()) std::this_thread::yield();
    std::atomic<bool> writerStarted{false};
    std::thread writer([&] { writerStarted = true; disk.writePage(page, {}); writerFinished = true; });
    while (!writerStarted.load()) std::this_thread::yield();
    const bool blocked = !writerFinished.load();
    releaseSync = true; synchronizer.join(); writer.join();
    require(blocked, "Physical writer passed in-flight sync");
    const auto state = disk.writebackStats();
    require(state.pending() && state.physicalWriteEpoch == state.synchronizedWriteEpoch + 1, "Concurrent later write claimed synchronized");
}

void fenceCases() {
    test::TemporaryDatabase db("frontier_fence"), shadow("frontier_fence_shadow");
    test::WritebackFixture f(db.path().string()); f.initialize();
    test::StableImage image(db.path().string(), shadow.path().string()); image.attach(f.disk);
    f.disk.sync();
    auto completed = f.disk.writebackStats().databaseSyncsCompleted;
    static_cast<void>(f.checkpoint());
    require(f.disk.writebackStats().databaseSyncsCompleted == completed, "Empty fuzzy checkpoint fsynced DB");
    f.update(1, 17);
    const auto dpt = f.pool.dirtyPageTableSnapshot();
    const auto writes = f.pool.stats().physicalPageWrites;
    for (unsigned i = 0; i < 4; ++i) static_cast<void>(f.checkpoint());
    require(f.pool.dirtyPageTableSnapshot() == dpt && f.pool.stats().physicalPageWrites == writes
        && f.disk.writebackStats().databaseSyncsCompleted == completed, "Dirty-only checkpoint stopped being fuzzy");
    for (const auto& entry : dpt) require(f.log.containsLsn(entry.recLsn), "Dirty required WAL deleted");
    require(f.pool.flushPage(1), "Page not resident");
    const auto persistent = f.pool.residentPageCopy(1).value();
    require(f.disk.writebackStats().pending() && f.pool.isDirty(1) == false, "Clean writeback not pending");
    static_cast<void>(f.checkpoint());
    require(!f.disk.writebackStats().pending() && f.disk.writebackStats().databaseSyncsCompleted == completed + 1
        && f.pool.residentPageCopy(1) == persistent, "Clean page fence/LSN bytes changed");
    f.root(6); // Direct metadata, not in resident DPT.
    require(f.disk.writebackStats().pending(), "Direct page-0 write bypassed tracker");
    auto generation = f.control.select(f.log).slot->generation;
    auto end = f.log.lastValidOffset(); auto deleted = f.log.stats().segmentsDeleted;
    const auto state = f.disk.writebackStats(); image.failNext = true;
    test::requireThrows<std::runtime_error>([&] { static_cast<void>(f.checkpoint()); }, "Checkpoint accepted failed fsync");
    require(f.control.select(f.log).slot->generation == generation && f.log.lastValidOffset() == end
        && f.log.stats().segmentsDeleted == deleted && f.disk.writebackStats().pending()
        && f.disk.writebackStats().synchronizedWriteEpoch == state.synchronizedWriteEpoch, "Failed fence advanced authority");
    static_cast<void>(f.checkpoint());
    require(f.control.select(f.log).slot->generation > generation && !f.disk.writebackStats().pending(), "Checkpoint retry failed");
    f.update(2, 19); const auto syncs = f.disk.writebackStats().databaseSyncsCompleted;
    static_cast<void>(f.checkpoint(CheckpointMode::Sharp));
    require(f.pool.dirtyPageTableSnapshot().empty() && !f.disk.writebackStats().pending()
        && f.disk.writebackStats().databaseSyncsCompleted == syncs + 1, "Sharp checkpoint duplicated/missed sync");
}

void originalLossRegression() {
    test::TemporaryDatabase db("frontier_original_loss"), shadow("frontier_original_shadow");
    {
        test::ReclaimFixture f(db.path().string()); f.initialize(); static_cast<void>(f.reclaim(30));
    }
    const auto child = ::fork();
    if (child == 0) {
        try {
            test::ReclaimFixture f(db.path().string());
            test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk);
            f.disk.sync(); static_cast<void>(f.reuse(true)); f.pool.flushAll();
            const auto image = f.pool.residentPageCopy(30).value();
            const auto lsn = readPersistentPageLsn(image);
            CheckpointControl control(db.path().string() + ".ckpt");
            CheckpointManager checkpoints(f.recovery, f.pool, f.disk, f.log, control, {});
            for (unsigned c = 0; c < 4; ++c) static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy));
            require(checkpoints.stats().fuzzyCheckpointDurabilitySyncs == 1 && !f.log.containsLsn(lsn)
                && checkpoints.stats().segmentsReclaimed != 0, "Original regression did not reclaim after fence");
            ::_exit(86);
        } catch (const std::exception& e) { std::cerr << e.what() << std::endl; ::_exit(99); }
    }
    hardExit(child);
    DiskManager::Page expected{};
    { DiskManager disk(db.path().string()); disk.readPhysicalPage(30, expected); }
    test::StableImage(db.path().string(), shadow.path().string()).restore();
    const auto recovery = test::recoverReclaim(db.path().string());
    DiskManager disk(db.path().string()); DiskManager::Page page{}; disk.readPhysicalPage(30, page);
    require(page == expected, "Original regression lost exact committed page bytes/PageLSN");
    require(ConstSlottedPageView(page, 30, disk.pageCount()).get(0) == TupleBytes(32, std::byte{0xA7}), "Original lost-write regression still loses committed page");
    require(recovery.checkpointUsed && recovery.checkpointMode == CheckpointMode::Fuzzy, "Original regression missed fuzzy recovery");
    std::cout << "original loss regression: P30 committed SlottedPage preserved after real deletion + modeled loss\n";
}

void lossMatrix() {
    unsigned cases = 0;
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto storage : {WalStorageMode::LegacySingleFile, WalStorageMode::Segmented})
       for (auto redo : {RedoPolicy::AlwaysRedo, RedoPolicy::PageLsnSelectiveRedo})
        for (unsigned placement = 0; placement < 3; ++placement) for (bool commit : {false, true}) {
            test::TemporaryDatabase db("frontier_loss_matrix"), shadow("frontier_loss_matrix_shadow");
            { test::WritebackFixture f(db.path().string(), mode, storage); f.initialize(); }
            auto child = ::fork();
            if (child == 0) {
                try {
                    test::WritebackFixture f(db.path().string(), mode, storage);
                    test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                    f.root(6); f.update(1, 0xBC, commit);
                    if (placement == 0) require(f.pool.flushPage(1), "Clean matrix page missing");
                    if (placement == 1) {
                        for (PageId id = 2; id <= 5; ++id) { auto guard = requireReadPage(f.pool, id, "evict pending page"); }
                        require(!f.pool.isResident(1), "Matrix page was not evicted");
                    }
                    auto dpt = f.pool.dirtyPageTableSnapshot(); const auto writes = f.pool.stats().physicalPageWrites;
                    for (unsigned c = 0; c < 4; ++c) { static_cast<void>(f.checkpoint()); checkpointSnapshot(f); }
                    require(f.pool.dirtyPageTableSnapshot() == dpt && f.pool.stats().physicalPageWrites == writes, "Fence flushed dirty frame");
                    require(!f.disk.writebackStats().pending(), "Fence left unsynchronized writes");
                    ::_exit(86);
                } catch (const std::exception& e) { std::cerr << "matrix " << placement << ": " << e.what() << std::endl; ::_exit(99); }
            }
            hardExit(child);
            DiskManager::Page metadata{};
            { DiskManager disk(db.path().string()); disk.readPhysicalPage(0, metadata); }
            test::StableImage(db.path().string(), shadow.path().string()).restore();
            test::WritebackFixture f(db.path().string(), mode, storage, 3, redo);
            f.verify({commit ? 0xBCU : 0U, 0, 0, 0, 0}, 6);
            DiskManager::Page afterMetadata{}; f.disk.readPhysicalPage(0, afterMetadata);
            require(afterMetadata == metadata, "Page-0 bytes/PageLSN changed across loss/recovery");
            require(f.startup.checkpointUsed, "Matrix did not select checkpoint"); ++cases;
        }
    std::cout << "lost-write/reopen matrix=" << cases << '\n';
}

void crashes() {
    unsigned cases = 0;
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto storage : {WalStorageMode::LegacySingleFile, WalStorageMode::Segmented})
       for (const auto* point : {"fuzzy_checkpoint_before_database_fence", "database_before_fsync",
            "database_after_fsync_syscall", "database_after_fsync", "fuzzy_checkpoint_after_database_fence", "fuzzy_checkpoint_after_begin_append",
            "fuzzy_checkpoint_after_end_append", "fuzzy_checkpoint_after_end_fsync", "fuzzy_checkpoint_before_control_publish",
            "checkpoint_mid_control_write", "fuzzy_checkpoint_after_control_sync", "fuzzy_checkpoint_before_reclamation",
            "wal_reclaim_after_manifest", "wal_reclaim_after_unlink", "fuzzy_checkpoint_after_reclamation"}) {
        if (storage != WalStorageMode::Segmented && std::string_view(point).starts_with("wal_reclaim")) continue;
        test::TemporaryDatabase db("frontier_crash"), shadow("frontier_crash_shadow");
        { test::WritebackFixture f(db.path().string(), mode, storage); f.initialize(); }
        const auto child = ::fork();
        if (child == 0) {
            try {
                test::WritebackFixture f(db.path().string(), mode, storage);
                test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                // Start with a real older authoritative generation and, for
                // segmented WAL, actual reclamation. A torn new control slot
                // must fall back without depending on already-deleted history.
                for (unsigned c = 0; c < 4; ++c) static_cast<void>(f.checkpoint());
                f.root(6); f.update(1, 0xCD); f.pool.flushAll();
                ::setenv("MINIDB_FAILPOINT", point, 1);
                for (unsigned c = 0; c < 4; ++c) static_cast<void>(f.checkpoint());
                ::_exit(99);
            } catch (const std::exception& e) { std::cerr << point << ": " << e.what() << std::endl; ::_exit(98); }
        }
        hardExit(child); test::StableImage(db.path().string(), shadow.path().string()).restore();
        test::WritebackFixture f(db.path().string(), mode, storage);
        f.verify({0xCD, 0, 0, 0, 0}, 6);
        const bool newControl = std::string_view(point) == "fuzzy_checkpoint_after_control_sync"
            || std::string_view(point) == "fuzzy_checkpoint_before_reclamation"
            || std::string_view(point).starts_with("wal_reclaim")
            || std::string_view(point) == "fuzzy_checkpoint_after_reclamation";
        require(f.startup.checkpointUsed && f.startup.checkpointGeneration == (newControl ? 5U : 4U),
            "Restart selected wrong checkpoint authority/fallback");
        ++cases;
       }
    std::cout << "hard-crash checkpoint matrix=" << cases << '\n';
}

void model() {
    unsigned total = 0, reopens = 0;
    for (auto seed : {0xF51CU, 0xC0FFEEU, 0xD0AB1EU, 0xE71C7U}) {
        std::mt19937 rng(seed);
        test::TemporaryDatabase db("frontier_model"), shadow("frontier_model_shadow");
        { test::WritebackFixture f(db.path().string()); f.initialize();
          test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync(); }
        std::array<unsigned, 5> expected{}; PageId head = INVALID_PAGE_ID;
        for (unsigned batch = 0; batch < 24; ++batch) {
            struct Operation { unsigned type, page, tag; };
            std::array<Operation, 250> operations{};
            for (auto& op : operations) {
                op = {static_cast<unsigned>(rng() % 14U),
                    static_cast<unsigned>(1U + rng() % 5U), static_cast<unsigned>(1U + rng() % 255U)};
                if (op.type == 1) expected[op.page - 1] = op.tag;
                if (op.type == 9) head = op.tag % 2 ? 6 : INVALID_PAGE_ID;
            }
            const auto child = ::fork();
            if (child == 0) {
                unsigned index = 0;
                try {
                    test::WritebackFixture f(db.path().string(), static_cast<WalUpdateMode>(batch % 3));
                    test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                    for (const auto& op : operations) {
                        auto before = f.disk.writebackStats(); const auto writes = f.pool.stats().physicalPageWrites;
                        unsigned directWrites = 0;
                        switch (op.type) {
                        case 0: case 10: case 12: case 13: { auto guard = requireReadPage(f.pool, op.page, "model load/pin"); break; }
                        case 1: f.update(op.page, op.tag); break;
                        case 2: static_cast<void>(f.pool.flushPage(op.page)); break;
                        case 3: f.pool.flushAll(); break;
                        case 4: f.disk.flush(); break;
                        case 5: static_cast<void>(f.disk.syncPendingWritebacks()); break;
                        case 6: {
                            const auto previous = f.disk.writebackStats(); stable.failNext = true;
                            test::requireThrows<std::runtime_error>([&] { f.disk.sync(); }, "Model failed fsync accepted");
                            require(f.disk.writebackStats().synchronizedWriteEpoch == previous.synchronizedWriteEpoch, "Model EIO advanced frontier"); break;
                        }
                        case 7: static_cast<void>(f.checkpoint()); checkpointSnapshot(f); break;
                        case 8: static_cast<void>(f.checkpoint(CheckpointMode::Sharp)); checkpointSnapshot(f); break;
                        case 9: f.root(op.tag % 2 ? 6 : INVALID_PAGE_ID); directWrites = 1; break;
                        case 11: f.log.rotateSegment(); break;
                        }
                        const auto after = f.disk.writebackStats();
                        require(after.physicalWriteEpoch == before.physicalWriteEpoch + directWrites
                            + f.pool.stats().physicalPageWrites - writes, "Model exact write epoch disagrees");
                        require(after.synchronizedWriteEpoch <= after.physicalWriteEpoch, "Sync frontier passed writes");
                        require(after.pending() == (after.physicalWriteEpoch != after.synchronizedWriteEpoch), "Model pending state disagrees");
                        for (const auto& entry : f.pool.dirtyPageTableSnapshot())
                            require(f.pool.isDirty(entry.pageId) == true && f.log.containsLsn(entry.recLsn), "Model dirty history lost");
                        ++index;
                    }
                    require(f.log.stats().segmentsDeleted != 0, "Model never deleted a WAL segment");
                    ::_exit(86);
                } catch (const std::exception& e) {
                    std::cerr << "seed=" << seed << " batch=" << batch << " op=" << index
                              << " type=" << (index < operations.size() ? operations[index].type : std::numeric_limits<unsigned>::max())
                              << ": " << e.what() << std::endl; ::_exit(99);
                }
            }
            hardExit(child);
            // Alternate actual process-only restart and loss of every write not
            // covered by the latest successful real fsync observed in the child.
            if (batch % 2 == 0) test::StableImage(db.path().string(), shadow.path().string()).restore();
            { test::WritebackFixture f(db.path().string()); f.verify(expected, head); }
            total += operations.size(); ++reopens;
        }
        std::cout << "seed=" << seed << " completed=6000\n";
    }
    require(total == 24000, "Model did not complete budget");
    std::cout << "lifecycle operations=" << total << " hard crashes/reopens=" << reopens << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--model") model();
        else if (argc == 2 && std::string_view(argv[1]) == "--concurrency") serialization();
        else { tracker(); serialization(); systemSync(); fenceCases(); originalLossRegression(); lossMatrix(); crashes(); }
        std::cout << "Physical writeback durability passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
