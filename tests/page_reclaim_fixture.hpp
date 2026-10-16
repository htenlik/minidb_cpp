#pragma once
#include "minidb/page_reclaim.hpp"
#include "minidb/database_metadata_manager.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/slotted_page.hpp"
#include "test_utils.hpp"
#include <set>

namespace minidb::test {
class ReclaimFixture {
public:
    DiskManager disk;
    LogManager log;
    RecoveryCoordinator recovery;
    BufferPoolManager pool;
    DatabaseMetadataManager metadata;
    PageAllocator allocator;
    DatabaseAccessGate gate;
    DatabaseAccessGate::Lease lease;
    PageReclaimer reclaimer;
    std::set<PageId> live;
    explicit ReclaimFixture(const std::string& path, WalStorageMode storage = WalStorageMode::Segmented,
        WalUpdateMode mode = WalUpdateMode::Adaptive)
        : disk(path), log(walPathForDatabase(path), LogManager::DEFAULT_BUFFER_SIZE, LogOpenMode::EagerValidated, storage, 9000),
          recovery(disk, log, INVALID_TRANSACTION_ID, mode), pool(disk, 128, 2, &log, &recovery),
          metadata(disk, recovery, log), allocator(pool, disk, &metadata), lease(gate.acquireExclusive()),
          reclaimer(disk, pool, log, recovery, gate) { recovery.attachBufferPool(pool); }
    void initialize(unsigned extent = 128) {
        while (disk.pageCount() < extent) static_cast<void>(disk.appendPage());
        for (auto [page, next] : {std::pair<PageId,PageId>{10, INVALID_PAGE_ID}, {20, 10}})
            disk.writePhysicalPage(page, reclaimedPage({page, next, extent}, INVALID_LSN));
        disk.updateFreeListRootPageId(20); disk.sync();
    }
    UnownedPageCertificate certify(PageId page) {
        if (live.contains(page)) throw std::logic_error("Fixture page still referenced");
        DiskManager::Page bytes{}; disk.readPhysicalPage(page, bytes);
        return UnownedPageCertificate(disk, page, crc32c(bytes), recovery.nextTransactionId(), log.lastValidOffset());
    }
    Lsn reclaim(PageId page) { return reclaimer.reclaim(certify(page), lease); }
    PageId reuse(bool commit = true) {
        recovery.beginStatement();
        const auto page = allocator.allocatePage();
        {
            auto guard = requireWritePage(pool, page, "initialize reuse");
            SlottedPageView::initialize(guard.data(), page, disk.pageCount(), 127);
            SlottedPageView view(guard.data(), page, disk.pageCount());
            static_cast<void>(view.insert(TupleBytes(32, std::byte{0xA7})));
        }
        if (commit) { recovery.commitStatement(); live.insert(page); }
        else recovery.rollbackStatement();
        return page;
    }
};
inline RecoveryStats recoverReclaim(const std::string& path, WalStorageMode storage = WalStorageMode::Segmented) {
    DiskManager disk(path);
    LogManager log(walPathForDatabase(path), LogManager::DEFAULT_BUFFER_SIZE, LogOpenMode::DeferredRecovery, storage, 9000);
    CheckpointControl control(path + ".ckpt");
    return RecoveryManager(disk, log, &control).recover();
}
} // namespace minidb::test
