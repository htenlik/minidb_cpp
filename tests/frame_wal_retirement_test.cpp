#include "writeback_fixture.hpp"
#include "page_reclaim_fixture.hpp"
#include "concurrency_utils.hpp"

#include <array>
#include <atomic>
#include <iostream>
#include <random>
#include <sys/wait.h>
#include <thread>

namespace minidb {
// Surgical corruption/testing access only; no public retirement authority.
struct BufferPoolTestAccess {
    template<class Function> static void change(BufferPoolManager& pool, PageId page, Function function) {
        std::lock_guard lock(pool.metadataLatch_);
        function(pool.frames_.at(pool.pageTable_.at(page)));
    }
    static void retire(BufferPoolManager& pool) { pool.retireCheckpointCoveredDependencies(); }
};
}

namespace {
using namespace minidb;
using test::require;
void hardExit(pid_t child) {
    int status = 0;
    require(child > 0 && ::waitpid(child, &status, 0) == child
        && WIFEXITED(status) && WEXITSTATUS(status) == 86, "Hard-crash boundary not reached");
}
void checkpoints(test::WritebackFixture& f, CheckpointMode mode) {
    for (unsigned n = 0; n < 4; ++n) static_cast<void>(f.checkpoint(mode));
}
void checkResident(test::WritebackFixture& f) {
    f.pool.validate();
    for (PageId id = 1; id <= 5; ++id) {
        if (!f.pool.isResident(id)) continue;
        const auto historical = *f.pool.pageLsn(id);
        const auto rec = *f.pool.recLsn(id);
        const bool dirty = *f.pool.isDirty(id);
        require(!*f.pool.pendingWalPreparation(id), "Completed model operation left pending preparation");
        if (isValidLsn(historical)) {
            require(readPersistentPageLsn(*f.pool.residentPageCopy(id)) == historical,
                "Volatile and persistent PageLSN disagree");
            if (dirty) require(!*f.pool.walDependencyRetired(id)
                && isValidLsn(rec) && rec <= historical && f.log.containsLsn(rec)
                && f.log.containsLsn(historical), "Dirty live dependency lost");
        }
        require(dirty || rec == INVALID_LSN, "Clean frame has recLSN");
    }
}
void directed() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto checkpoint : {CheckpointMode::Sharp, CheckpointMode::Fuzzy}) {
        test::TemporaryDatabase db("frame_retirement_directed");
        test::WritebackFixture f(db.path().string(), mode, WalStorageMode::Segmented, 5); f.initialize();
        f.update(1, 17); require(f.pool.flushPage(1), "Setup flush failed");
        const auto bytes = *f.pool.residentPageCopy(1); const auto old = *f.pool.pageLsn(1);
        const auto pending = f.disk.writebackStats();
        require(pending.pending() && !*f.pool.walDependencyRetired(1), "Writeback retired without authority");
        // Separate the dirty page's retained history plus conservative closed
        // predecessor from the historical record we expect to delete.
        f.log.rotateSegment(); f.log.rotateSegment();
        f.update(2, 18);
        const auto dpt = f.pool.dirtyPageTableSnapshot(); const auto before = f.pool.stats();
        const auto walStart = f.log.lastValidOffset();
        const auto oldSegment = std::filesystem::path(segmentedWalPathForDatabase(db.path().string()))
            / walSegmentFilename(1);
        checkpoints(f, checkpoint);
        require(*f.pool.walDependencyRetired(1) && !f.log.containsLsn(old)
            && f.log.stats().segmentsDeleted > 0 && !std::filesystem::exists(oldSegment), "No real historical segment deletion");
        require(*f.pool.residentPageCopy(1) == bytes && *f.pool.pageLsn(1) == old,
            "Retirement altered historical identity/bytes");
        require(f.pool.stats().cleanDependenciesRetired > before.cleanDependenciesRetired,
            "Clean dependency not retired");
        if (checkpoint == CheckpointMode::Fuzzy) {
            require(f.pool.dirtyPageTableSnapshot() == dpt && !*f.pool.walDependencyRetired(2)
                && f.pool.stats().physicalPageWrites == before.physicalPageWrites, "Fuzzy became sharp");
        } else require(f.pool.dirtyPageTableSnapshot().empty(), "Sharp left dirty state");
        // Only checkpoint records, no synthetic page update for retirement.
        for (const auto& record : f.log.scan().records)
            if (record.lsn >= walStart) require(record.transactionId == INVALID_TRANSACTION_ID, "Retirement generated transaction WAL");
        checkResident(f);
        {
            auto guard = requireReadPage(f.pool, 1, "historical read guard");
            require(guard.pageLsn() == old, "Read guard lost historical PageLSN");
        }
        f.recovery.beginStatement();
        { auto guard = requireWritePage(f.pool, 1, "pending after retirement");
          require(*f.pool.pendingWalPreparation(1), "Write intent not pending");
          require(SlottedPageView(guard.data(), 1, f.disk.pageCount()).tryUpdate(0, TupleBytes(32, std::byte{19})), "Update failed"); }
        test::requireThrows<std::logic_error>([&] { static_cast<void>(f.pool.dirtyPageTableSnapshot()); }, "Pending intent entered DPT");
        auto lease = f.gate.acquireExclusive();
        test::requireThrows<std::logic_error>([&] { static_cast<void>(f.checkpoints.checkpoint(CheckpointMode::Fuzzy, lease)); }, "Unsafe boundary checkpointed");
        lease.reset();
        f.recovery.commitStatement();
        require(*f.pool.pageLsn(1) > old && f.pool.recLsn(1) == f.pool.pageLsn(1)
            && !*f.pool.walDependencyRetired(1)
            && f.pool.stats().dependenciesRenewedAfterRetirement > 0, "Fresh dependency not established");
        require(f.pool.flushPage(1), "Fresh write failed");
        checkpoints(f, checkpoint); checkResident(f);
        f.update(1, 20, false); f.recovery.rollbackStatement();
        f.verify({19, 18, 0, 0, 0}, INVALID_PAGE_ID); checkResident(f);
    }
}

void negatives() {
    test::TemporaryDatabase db("frame_retirement_negatives");
    test::WritebackFixture f(db.path().string()); f.initialize(); f.update(1, 7);
    const auto lsn = *f.pool.pageLsn(1); const auto rec = *f.pool.recLsn(1);
    const auto writes = f.pool.stats().physicalPageWrites;
    const auto corrupt = [&](auto mutation, auto restore, const char* message) {
        BufferPoolTestAccess::change(f.pool, 1, mutation);
        test::requireThrows<std::logic_error>([&] { f.pool.validate(); }, message);
        test::requireThrows<std::logic_error>([&] { f.pool.flushAll(); }, message);
        require(f.pool.stats().physicalPageWrites == writes, "Corrupt dependency reached disk");
        BufferPoolTestAccess::change(f.pool, 1, restore);
    };
    corrupt([](auto& p) { p.pageLsn = 123456789; }, [&](auto& p) { p.pageLsn = lsn; }, "Missing dirty latest accepted");
    corrupt([](auto& p) { p.pageLsn = INVALID_LSN; }, [&](auto& p) { p.pageLsn = lsn; }, "Invalid dirty latest accepted");
    corrupt([](auto& p) { p.recLsn = INVALID_LSN; }, [&](auto& p) { p.recLsn = rec; }, "Missing dirty recLSN accepted");
    corrupt([](auto& p) { p.recLsn = 1; }, [&](auto& p) { p.recLsn = rec; }, "Unavailable dirty recLSN accepted");
    corrupt([&](auto& p) { p.recLsn = lsn + 1; }, [&](auto& p) { p.recLsn = rec; }, "Future recLSN accepted");
    corrupt([](auto& p) { p.walDependencyRetired = true; }, [](auto& p) { p.walDependencyRetired = false; }, "Dirty retired dependency accepted");
    // A lower missing dependency must not be hidden by another frame's max LSN.
    f.update(2, 9);
    BufferPoolTestAccess::change(f.pool, 1, [](auto& p) { p.pageLsn = 1; p.recLsn = 1; });
    test::requireThrows<std::logic_error>([&] { f.pool.flushAll(); }, "Maximum-only batch validation");
    require(f.pool.stats().physicalPageWrites == writes, "Invalid batch partially persisted");
    BufferPoolTestAccess::change(f.pool, 1, [&](auto& p) { p.pageLsn = lsn; p.recLsn = rec; });
    require(f.pool.flushPage(1), "Negative setup flush");
    test::requireThrows<std::logic_error>([&] { BufferPoolTestAccess::retire(f.pool); }, "Unsynced clean retired");
    require(!*f.pool.walDependencyRetired(1), "Failed reconciliation partially retired");
    // Failure of control publication does not authorize retirement, even after DB fsync.
    std::filesystem::create_directory(db.path().string() + ".ckpt");
    test::requireThrows<std::runtime_error>([&] { static_cast<void>(f.checkpoint()); }, "Bad control path accepted");
    require(!*f.pool.walDependencyRetired(1), "Failed control publication retired dependency");
    std::filesystem::remove(db.path().string() + ".ckpt");
    static_cast<void>(f.checkpoint()); require(*f.pool.walDependencyRetired(1), "Retry failed retirement");
}

void noOpAndMonotonicity() {
    test::TemporaryDatabase db("frame_retirement_noop");
    test::WritebackFixture f(db.path().string()); f.initialize();
    f.update(1, 8); f.pool.flushAll(); const auto old = *f.pool.pageLsn(1);
    // Keep a known older identity for the monotonicity test before reclamation.
    static_cast<void>(f.checkpoint());
    f.recovery.beginStatement();
    { auto guard = requireWritePage(f.pool, 1, "no-op guard"); }
    f.recovery.commitStatement();
    require(!*f.pool.isDirty(1) && *f.pool.walDependencyRetired(1) && f.pool.pageLsn(1) == old,
        "No-op intent renewed/invalidated historical state");
    f.update(1, 9); const auto latest = *f.pool.pageLsn(1);
    f.pool.flushAll(); static_cast<void>(f.checkpoint());
    require(f.log.containsLsn(old), "Monotonicity test needs retained older record");
    f.recovery.beginStatement();
    { auto guard = requireWritePage(f.pool, 1, "monotonic guard");
      test::requireThrows<std::invalid_argument>([&] { guard.setPageLsn(old); }, "Retirement allowed historical decrease"); }
    f.recovery.commitStatement(); require(f.pool.pageLsn(1) == latest, "No-op changed latest identity");
    checkResident(f);
}

void pendingWithoutPreparer() {
    class Wal final : public WalFlushProvider {
    public:
        Lsn durableLsn() const noexcept override { return 128; }
        bool containsLsn(Lsn lsn) const noexcept override { return lsn == 64 || lsn == 128; }
        void flushUpTo(Lsn) override {}
    } wal;
    test::TemporaryDatabase db("frame_pending_manual_wal"); DiskManager disk(db.path().string());
    const auto id = disk.appendPage(); BufferPoolManager pool(disk, 1, 2, &wal);
    { auto guard = requireWritePage(pool, id, "manual logged page"); guard.data()[80] = std::byte{1}; guard.setPageLsn(64); }
    pool.flushAll(); const auto before = pool.stats().physicalPageWrites;
    { auto guard = requireWritePage(pool, id, "unprepared manual page"); guard.data()[80] = std::byte{2}; }
    test::requireThrows<std::logic_error>([&] { pool.flushAll(); }, "Unprepared mutation used old WAL identity");
    test::requireThrows<std::logic_error>([&] { static_cast<void>(pool.flushPage(id)); }, "Unprepared single flush accepted");
    require(pool.stats().physicalPageWrites == before, "Unprepared mutation persisted");
    { auto guard = requireWritePage(pool, id, "manual preparation"); guard.setPageLsn(128); }
    pool.flushAll(); pool.validate();
}

void controlFallback() {
    for (bool removeControl : {false, true}) {
        test::TemporaryDatabase db("frame_retirement_control_loss");
        { test::WritebackFixture f(db.path().string()); f.initialize(); f.update(1, 57); f.pool.flushAll();
          checkpoints(f, CheckpointMode::Fuzzy); f.pool.validate();
          require(f.log.stats().segmentsDeleted > 0 && *f.pool.walDependencyRetired(1), "Control test did not retire/reclaim"); }
        const auto path = db.path().string() + ".ckpt";
        if (removeControl) std::filesystem::remove(path);
        else {
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            bool corrupted = false;
            for (unsigned slot = 0; slot < checkpoint_control_layout::SLOT_COUNT; ++slot) {
                const auto offset = checkpoint_control_layout::HEADER_SIZE + slot * checkpoint_slot_layout::SIZE;
                std::array<std::byte, checkpoint_slot_layout::SIZE> bytes{};
                file.seekg(static_cast<std::streamoff>(offset));
                file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                if (decodeCheckpointSlot(bytes).generation == 4) {
                    bytes[checkpoint_slot_layout::CHECKSUM_OFFSET] ^= std::byte{1};
                    file.seekp(static_cast<std::streamoff>(offset));
                    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); corrupted = true;
                }
            }
            require(corrupted && static_cast<bool>(file), "Did not corrupt latest control slot");
        }
        test::WritebackFixture f(db.path().string()); f.verify({57, 0, 0, 0, 0}, INVALID_PAGE_ID);
        require(f.startup.checkpointUsed && f.startup.checkpointId == (removeControl ? 4U : 3U)
            && f.startup.checkpointGeneration == (removeControl ? 1U : 3U),
            "Unexpected recovered checkpoint authority"); f.pool.validate();
    }
}

void reclamationFailures() {
    for (const auto* point : {"fuzzy_checkpoint_before_reclamation", "wal_reclaim_after_manifest", "wal_reclaim_after_unlink"}) {
        test::TemporaryDatabase db("frame_retirement_partial_reclaim");
        { test::WritebackFixture f(db.path().string()); f.initialize(); f.update(1, 24); f.pool.flushAll();
          const auto old = *f.pool.pageLsn(1);
          ::setenv("MINIDB_THROWPOINT", point, 1);
          for (unsigned n = 0; n < 5; ++n) static_cast<void>(f.checkpoint());
          ::unsetenv("MINIDB_THROWPOINT");
          require(f.checkpoints.stats().reclamationFailures > 0 && *f.pool.walDependencyRetired(1), "Failed reclamation lost authority");
          if (std::string_view(point) == "wal_reclaim_after_unlink")
              require(f.log.stats().segmentsDeleted > 0, "Partial failure did not physically delete");
          require(f.pool.pageLsn(1) == old, "Failure changed identity"); f.pool.validate(); }
        test::WritebackFixture reopened(db.path().string()); reopened.verify({24, 0, 0, 0, 0}, INVALID_PAGE_ID);
    }
}

void activeTransaction() {
    test::TemporaryDatabase db("frame_retirement_att");
    test::WritebackFixture f(db.path().string()); f.initialize(); f.update(1, 31, false);
    require(f.pool.flushPage(1), "Active transaction flush failed");
    const auto att = f.recovery.checkpointTransactions(); const auto lsn = *f.pool.pageLsn(1);
    checkpoints(f, CheckpointMode::Fuzzy);
    require(*f.pool.walDependencyRetired(1) && f.log.containsLsn(lsn)
        && f.recovery.checkpointTransactions() == att, "Frame retirement changed active transaction history");
    f.recovery.rollbackStatement(); f.verify({0, 0, 0, 0, 0}, INVALID_PAGE_ID);
}

void reuseCycles() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive}) {
        test::TemporaryDatabase db("frame_retirement_reuse");
        test::ReclaimFixture f(db.path().string(), WalStorageMode::Segmented, mode); f.initialize();
        CheckpointControl control(db.path().string() + ".ckpt");
        CheckpointManager manager(f.recovery, f.pool, f.disk, f.log, control, {});
        for (unsigned cycle = 0; cycle < 12; ++cycle) {
            f.live.erase(30); static_cast<void>(f.reclaim(30));
            require(!f.pool.isResident(30), "Reclaim retained stale resident image");
            require(f.reuse(false) == 30, "CLR reuse did not restore Free Page");
            require(f.reuse(true) == 30, "Typed reuse selected wrong page");
            f.pool.flushAll(); const auto bytes = *f.pool.residentPageCopy(30);
            for (unsigned c = 0; c < 4; ++c) static_cast<void>(manager.checkpoint(
                cycle % 2 ? CheckpointMode::Sharp : CheckpointMode::Fuzzy));
            require(*f.pool.walDependencyRetired(30) && *f.pool.residentPageCopy(30) == bytes,
                "Reuse/retirement changed type/LSN/bytes");
            f.pool.validate(); f.allocator.validate();
        }
    }
}

void concurrency() {
    test::TemporaryDatabase db("frame_retirement_readers");
    test::WritebackFixture f(db.path().string()); f.initialize();
    f.update(1, 34); f.pool.flushAll();
    test::ThreadErrors errors; std::atomic<bool> stop{false};
    std::vector<std::jthread> readers;
    for (unsigned t = 0; t < 6; ++t) readers.emplace_back([&] { errors.run([&] {
        while (!stop) {
            auto lease = f.gate.acquireShared();
            const auto page = requireReadPage(f.pool, 1, "concurrent historical reader");
            require(ConstSlottedPageView(page.data(), 1, f.disk.pageCount()).get(0) == TupleBytes(32, std::byte{34}), "Reader bytes changed");
            require(isValidLsn(page.pageLsn()), "Historical guard identity erased");
        }
    }); });
    for (unsigned n = 0; n < 30; ++n) { static_cast<void>(f.checkpoint(n % 2 ? CheckpointMode::Sharp : CheckpointMode::Fuzzy)); f.pool.validate(); }
    stop = true; readers.clear(); errors.rethrow();
    require(f.pool.stats().physicalPageWrites == 1 && f.pool.stats().cleanDependenciesRetired == 1, "Read/checkpoint path wrote/retired twice");
}

void crashMatrix() {
    unsigned cases = 0;
    const std::array points = {"database_before_fsync", "database_after_fsync_syscall",
        "fuzzy_checkpoint_after_begin_append", "fuzzy_checkpoint_after_end_append",
        "fuzzy_checkpoint_after_end_fsync", "checkpoint_mid_control_write", "fuzzy_checkpoint_after_control_sync",
        "checkpoint_before_frame_retirement", "checkpoint_after_frame_retirement",
        "fuzzy_checkpoint_before_reclamation", "wal_reclaim_after_manifest", "wal_reclaim_after_unlink",
        "before_page_update_append", "after_page_update_append", "after_commit_append",
        "after_page_update_wal_force", "after_database_page_write", "before_commit_append", "after_commit_sync",
        "recovery_after_clr_wal_force"};
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto redo : {RedoPolicy::AlwaysRedo, RedoPolicy::PageLsnSelectiveRedo})
       for (const auto* point : points) {
        test::TemporaryDatabase db("frame_retirement_crash"), shadow("frame_retirement_crash_shadow");
        { test::WritebackFixture f(db.path().string(), mode); f.initialize(); }
        const auto child = ::fork();
        if (child == 0) {
            try {
                test::WritebackFixture f(db.path().string(), mode);
                test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                f.update(1, 41); f.pool.flushAll(); checkpoints(f, CheckpointMode::Fuzzy);
                require(*f.pool.walDependencyRetired(1) && !f.log.containsLsn(*f.pool.pageLsn(1)), "Crash setup not retired/reclaimed");
                const std::string_view name(point);
                const bool user = name == "after_page_update_wal_force" || name == "after_database_page_write"
                    || name == "before_commit_append" || name == "after_commit_sync" || name == "recovery_after_clr_wal_force"
                    || name == "before_page_update_append" || name == "after_page_update_append" || name == "after_commit_append";
                if (user) {
                    ::setenv("MINIDB_FAILPOINT", point, 1);
                    f.update(1, 42, false);
                    if (name == "before_commit_append" || name == "after_commit_sync" || name == "after_commit_append") f.recovery.commitStatement();
                    else if (name == "recovery_after_clr_wal_force") f.recovery.rollbackStatement();
                    else f.pool.flushAll();
                } else {
                    f.update(1, 42); f.pool.flushAll();
                    ::setenv("MINIDB_FAILPOINT", point, 1); checkpoints(f, CheckpointMode::Fuzzy);
                }
                ::_exit(99);
            } catch (const std::exception& e) { std::cerr << point << ": " << e.what() << std::endl; ::_exit(98); }
        }
        hardExit(child); test::StableImage(db.path().string(), shadow.path().string()).restore();
        test::WritebackFixture f(db.path().string(), mode, WalStorageMode::Segmented, 3, redo);
        const std::string_view name(point);
        const bool loser = name == "after_page_update_wal_force" || name == "after_database_page_write"
            || name == "before_commit_append" || name == "recovery_after_clr_wal_force"
            || name == "before_page_update_append" || name == "after_page_update_append" || name == "after_commit_append";
        f.verify({loser ? 41U : 42U, 0, 0, 0, 0}, INVALID_PAGE_ID); f.pool.validate(); ++cases;
    }
    std::cout << "frame retirement hard-crash/lost-write cases=" << cases << '\n';
}

void metadataLoss() {
    for (auto mode : {WalUpdateMode::FullPage, WalUpdateMode::ByteRange, WalUpdateMode::Adaptive})
      for (auto redo : {RedoPolicy::AlwaysRedo, RedoPolicy::PageLsnSelectiveRedo}) {
        test::TemporaryDatabase db("retirement_page_zero"), shadow("retirement_page_zero_shadow");
        { test::WritebackFixture f(db.path().string(), mode); f.initialize(); }
        const auto child = ::fork();
        if (child == 0) {
            try {
                test::WritebackFixture f(db.path().string(), mode);
                test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                f.update(1, 61); f.pool.flushAll();
                f.recovery.beginStatement();
                f.metadata.updateFreeListRootPageId(6);
                // Physical metadata test: a legal existing PageId, not a
                // synthetic claim that page 5 is a real catalog structure.
                f.metadata.updateCatalogRootPageId(5);
                f.recovery.commitStatement();
                checkpoints(f, CheckpointMode::Fuzzy); f.pool.validate();
                require(f.log.stats().segmentsDeleted > 0, "Metadata history not reclaimed");
                ::_exit(86);
            } catch (const std::exception& e) { std::cerr << e.what() << std::endl; ::_exit(99); }
        }
        hardExit(child);
        DiskManager::Page expected{};
        { DiskManager disk(db.path().string()); disk.readPhysicalPage(0, expected); }
        test::StableImage(db.path().string(), shadow.path().string()).restore();
        test::WritebackFixture f(db.path().string(), mode, WalStorageMode::Segmented, 3, redo);
        f.verify({61, 0, 0, 0, 0}, 6);
        DiskManager::Page actual{}; f.disk.readPhysicalPage(0, actual);
        require(actual == expected && f.disk.databaseHeader().catalogRootPageId == 5,
            "Retirement bypassed direct page-0 physical durability/PageLSN");
    }
}

void model() {
    unsigned total = 0, restarts = 0;
    for (auto seed : {0xF2A1U, 0x51A7EU, 0xC1EA4U, 0x5EED9U}) {
        test::TemporaryDatabase db("frame_retirement_model"), shadow("frame_retirement_model_shadow");
        { test::WritebackFixture f(db.path().string()); f.initialize(); }
        std::mt19937 random(seed); std::array<unsigned, 5> expected{};
        for (unsigned batch = 0; batch < 30; ++batch) {
            struct Operation { unsigned type, page, tag; };
            std::array<Operation, 250> operations{};
            auto after = expected;
            for (auto& op : operations) {
                op = {static_cast<unsigned>(random() % 10), static_cast<unsigned>(1 + random() % 5), static_cast<unsigned>(1 + random() % 254)};
                if (op.type == 0) after[op.page - 1] = op.tag;
            }
            const auto child = ::fork();
            if (child == 0) {
                unsigned index = 0;
                try {
                    test::WritebackFixture f(db.path().string(), static_cast<WalUpdateMode>(batch % 3), WalStorageMode::Segmented, 3);
                    test::StableImage stable(db.path().string(), shadow.path().string()); stable.attach(f.disk); f.disk.sync();
                    auto values = expected;
                    for (const auto& op : operations) {
                        const auto before = f.pool.stats(); const auto frontier = f.disk.writebackStats();
                        const auto previousLsn = f.pool.pageLsn(op.page);
                        const auto previousRec = f.pool.recLsn(op.page);
                        const bool changed = values[op.page - 1] != op.tag;
                        switch (op.type) {
                        case 0: f.update(op.page, op.tag); values[op.page - 1] = op.tag; break;
                        case 1: static_cast<void>(f.pool.flushPage(op.page)); break;
                        case 2: f.pool.flushAll(); break;
                        case 3: static_cast<void>(f.disk.syncPendingWritebacks()); break;
                        case 4: static_cast<void>(f.checkpoint()); break;
                        case 5: static_cast<void>(f.checkpoint(CheckpointMode::Sharp)); break;
                        case 6: { auto guard = requireReadPage(f.pool, op.page, "lifecycle model read"); break; }
                        case 7: f.update(op.page, op.tag, false); f.recovery.rollbackStatement(); break;
                        case 8: f.recovery.beginStatement(); { auto guard = requireWritePage(f.pool, op.page, "model no-op intent"); }
                            f.recovery.commitStatement(); break;
                        case 9: f.log.rotateSegment(); break;
                        }
                        checkResident(f);
                        require(f.recovery.checkpointTransactions().empty(), "Completed operation retained ATT entry");
                        if (op.type == 0 && changed) {
                            require(!*f.pool.walDependencyRetired(op.page) && *f.pool.isDirty(op.page), "New write did not renew live dependency");
                            if (previousLsn && isValidLsn(*previousLsn))
                                require(*f.pool.pageLsn(op.page) > *previousLsn, "Model PageLSN not monotonic");
                            require(f.pool.recLsn(op.page) == (previousRec && isValidLsn(*previousRec)
                                ? previousRec : f.pool.pageLsn(op.page)), "Model dirty period restarted incorrectly");
                        }
                        if (op.type == 4 || op.type == 5) {
                            const auto selected = f.control.select(f.log);
                            require(selected.slot.has_value(), "Model checkpoint not authoritative");
                            for (PageId id = 1; id <= 5; ++id) if (f.pool.isResident(id) && isValidLsn(*f.pool.pageLsn(id)))
                                require(*f.pool.walDependencyRetired(id) == !*f.pool.isDirty(id), "Model retirement classification differs");
                            for (const auto& entry : f.pool.dirtyPageTableSnapshot()) require(
                                f.log.oldestRetainedLsn() <= entry.recLsn, "Model checkpoint discarded dirty history");
                        }
                        const auto state = f.disk.writebackStats();
                        if (op.type != 7) require(state.physicalWriteEpoch == frontier.physicalWriteEpoch
                            + f.pool.stats().physicalPageWrites - before.physicalPageWrites, "Retirement added physical writes");
                        for (PageId id = 1; id <= 5; ++id) if (f.pool.isResident(id)) {
                            const auto bytes = *f.pool.residentPageCopy(id);
                            require(ConstSlottedPageView(bytes, id, f.disk.pageCount()).get(0)
                                == TupleBytes(32, static_cast<std::byte>(values[id - 1])), "Model payload mismatch");
                        }
                        ++index;
                    }
                    require(f.log.stats().segmentsDeleted > 0, "Model had no real deletion");
                    ::_exit(86);
                } catch (const std::exception& e) {
                    std::cerr << "seed=" << seed << " batch=" << batch << " op=" << index
                        << " type=" << operations[std::min<std::size_t>(index, 249)].type << ": " << e.what() << std::endl; ::_exit(99);
                }
            }
            hardExit(child);
            if (batch % 2 == 0) test::StableImage(db.path().string(), shadow.path().string()).restore();
            { test::WritebackFixture f(db.path().string(), WalUpdateMode::Adaptive, WalStorageMode::Segmented, 3,
                batch % 2 ? RedoPolicy::AlwaysRedo : RedoPolicy::PageLsnSelectiveRedo); f.verify(after, INVALID_PAGE_ID); }
            expected = after; total += operations.size(); ++restarts;
        }
        std::cout << "seed=" << seed << " completed=7500\n";
    }
    require(total == 30000 && restarts == 120, "Model budget incomplete");
    std::cout << "frame lifecycle completed=" << total << " hard restarts=" << restarts << '\n';
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--model") model();
        else if (argc == 2 && std::string_view(argv[1]) == "--concurrency") concurrency();
        else { directed(); negatives(); noOpAndMonotonicity(); pendingWithoutPreparer(); controlFallback(); activeTransaction(); reuseCycles(); reclamationFailures(); crashMatrix(); metadataLoss(); }
        std::cout << "Frame WAL retirement passed\n";
    } catch (const std::exception& e) { ::unsetenv("MINIDB_THROWPOINT"); std::cerr << e.what() << '\n'; return 1; }
}
