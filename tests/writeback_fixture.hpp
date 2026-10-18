#pragma once

#include "test_utils.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/page_reclaim.hpp"
#include "minidb/slotted_page.hpp"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace minidb {
// No public production fault-injection API; only this test friend can replace
// the fsync syscall or reach an otherwise infeasible uint64 exhaustion boundary.
struct DiskManagerTestAccess {
    static void syncFunction(DiskManager& disk, int (*function)(int)) {
        std::lock_guard lock(disk.ioLatch_);
        disk.syncFunction_ = function;
    }
    static void exhaustEpoch(DiskManager& disk) {
        std::lock_guard lock(disk.ioLatch_);
        disk.writeback_.physicalWriteEpoch = UINT64_MAX;
    }
    static void breakStream(DiskManager& disk) {
        std::lock_guard lock(disk.ioLatch_);
        disk.file_.close();
    }
};
}

namespace minidb::test {
inline std::vector<char> fileBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read test stable image");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// This sidecar exists ONLY in tests. Each successful real DB fsync snapshots
// precisely the file covered by that call, while DiskManager still holds its
// I/O lock. Hard-exit/lost-write tests restore this snapshot before real recovery.
class StableImage {
public:
    std::string database;
    std::string shadow;
    bool failNext = false;
    static inline thread_local StableImage* active = nullptr;

    StableImage(std::string databasePath, std::string shadowPath)
        : database(std::move(databasePath)), shadow(std::move(shadowPath)) {}
    void attach(DiskManager& disk) {
        active = this;
        DiskManagerTestAccess::syncFunction(disk, &syncFile);
    }
    static int syncFile(int descriptor) {
        require(active != nullptr, "Missing test sync observer");
        if (active->failNext) {
            active->failNext = false;
            errno = EIO;
            return -1;
        }
        const auto result = ::fsync(descriptor);
        if (result == 0) {
            std::filesystem::copy_file(active->database, active->shadow,
                std::filesystem::copy_options::overwrite_existing);
        }
        return result;
    }
    void restore() const {
        require(std::filesystem::exists(shadow), "No successful sync snapshot");
        std::filesystem::copy_file(shadow, database,
            std::filesystem::copy_options::overwrite_existing);
    }
};

struct WritebackFixture {
    DiskManager disk;
    LogManager log;
    CheckpointControl control;
    RecoveryStats startup;
    RecoveryCoordinator recovery;
    BufferPoolManager pool;
    DatabaseMetadataManager metadata;
    CheckpointManager checkpoints;
    DatabaseAccessGate gate;

    explicit WritebackFixture(const std::string& path,
        WalUpdateMode mode = WalUpdateMode::Adaptive,
        WalStorageMode storage = WalStorageMode::Segmented,
        std::size_t frames = 3, RedoPolicy redo = RedoPolicy::PageLsnSelectiveRedo)
        : disk(path), log(walPathForDatabase(path), 4096, LogOpenMode::DeferredRecovery, storage, 9000),
          control(path + ".ckpt"), startup(RecoveryManager(disk, log, &control, false, redo).recover()),
          recovery(disk, log, startup.nextTransactionId, mode),
          pool(disk, frames, 2, &log, &recovery), metadata(disk, recovery, log),
          checkpoints(recovery, pool, disk, log, control, startup) {
        recovery.attachBufferPool(pool);
        checkpoints.attachAccessGate(gate);
    }
    void initialize(unsigned extent = 8) {
        while (disk.pageCount() < extent) static_cast<void>(disk.appendPage());
        for (PageId id = 1; id <= 5; ++id) {
            DiskManager::Page page{};
            SlottedPageView::initialize(page, id, disk.pageCount(), 7);
            static_cast<void>(SlottedPageView(page, id, disk.pageCount()).insert(TupleBytes(32, std::byte{0})));
            disk.writePhysicalPage(id, page);
        }
        disk.writePhysicalPage(6, reclaimedPage({6, INVALID_PAGE_ID, disk.pageCount()}, INVALID_LSN));
        disk.sync();
    }
    void update(PageId id, unsigned tag, bool commit = true) {
        recovery.beginStatement();
        {
            auto guard = requireWritePage(pool, id, "frontier update");
            SlottedPageView page(guard.data(), id, disk.pageCount());
            require(page.tryUpdate(0, TupleBytes(32, static_cast<std::byte>(tag))), "In-page update failed");
        }
        if (commit) recovery.commitStatement();
        else recovery.prepareStatement();
    }
    void root(PageId id) {
        recovery.beginStatement();
        metadata.updateFreeListRootPageId(id);
        recovery.commitStatement();
    }
    CheckpointId checkpoint(CheckpointMode mode = CheckpointMode::Fuzzy) {
        auto lease = gate.acquireExclusive();
        return checkpoints.checkpoint(mode, lease);
    }
    void verify(const std::array<unsigned, 5>& tags, PageId head) {
        for (PageId id = 1; id <= 5; ++id) {
            auto guard = requireReadPage(pool, id, "verify frontier data");
            require(ConstSlottedPageView(guard.data(), id, disk.pageCount()).get(0)
                    == TupleBytes(32, static_cast<std::byte>(tags[id - 1])), "Recovered committed bytes differ");
        }
        require(disk.databaseHeader().freeListRootPageId == head, "Recovered metadata root differs");
        PageAllocator allocator(pool, disk);
        allocator.validate();
        require(allocator.freePageIds() == (head == INVALID_PAGE_ID
            ? std::vector<PageId>{} : std::vector<PageId>{6}), "Recovered free membership differs");
    }
};
}
