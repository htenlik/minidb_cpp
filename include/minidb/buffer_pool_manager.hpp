#pragma once

#include "minidb/disk_manager.hpp"
#include "minidb/checkpoint_types.hpp"
#include "minidb/lru_k_replacer.hpp"
#include "minidb/page_guard.hpp"
#include "minidb/page_recovery.hpp"
#include "minidb/wal_types.hpp"

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace minidb {

struct BufferFrame {
    DiskManager::Page data{};
    PageId pageId = INVALID_PAGE_ID;
    std::uint32_t pinCount = 0;
    bool dirty = false;
    bool valid = false;
    Lsn pageLsn = INVALID_LSN;
    Lsn recLsn = INVALID_LSN;
};

struct BufferPoolStats {
    std::uint64_t pageRequests = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t physicalPageReads = 0;
    std::uint64_t physicalPageWrites = 0;
    std::uint64_t evictions = 0;
    std::uint64_t dirtyEvictions = 0;
    std::uint64_t pinOperations = 0;
    std::uint64_t unpinOperations = 0;
    std::uint64_t appendedPages = 0;
    std::uint64_t walFlushRequests = 0;
    std::uint64_t residentPages = 0;
    std::uint64_t pinnedFrames = 0;
    std::uint64_t evictableFrames = 0;
    std::uint64_t capacity = 0;
    std::uint64_t frameAvailabilityWaits = 0;
    std::uint64_t contentLatchWaits = 0;

    bool operator==(const BufferPoolStats&) const = default;
};

// Metadata is serialized; guards latch frame contents after pinning. DiskManager
// and this manager must outlive all guards. No frame-latch wait holds metadata.
class BufferPoolManager {
public:
    BufferPoolManager(
        DiskManager& diskManager,
        std::size_t frameCount,
        std::size_t k = 2,
        WalFlushProvider* walProvider = nullptr,
        PageRecoveryHook* recoveryHook = nullptr,
        bool waitForReadFrames = false);
    ~BufferPoolManager();

    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    [[nodiscard]] std::optional<ReadPageGuard> fetchPageRead(PageId pageId);
    [[nodiscard]] std::optional<WritePageGuard> fetchPageWrite(PageId pageId);
    [[nodiscard]] std::optional<WritePageGuard> newPageWrite();

    // Returns false for a nonresident data page; it never loads a page merely to flush.
    [[nodiscard]] bool flushPage(PageId pageId);
    void flushAll();

    [[nodiscard]] BufferPoolStats stats() const noexcept;
    void resetStats() noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
    [[nodiscard]] std::size_t residentPageCount() const noexcept;
    [[nodiscard]] bool isResident(PageId pageId) const noexcept;
    [[nodiscard]] std::optional<FrameId> frameIdForPage(PageId pageId) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> pinCount(PageId pageId) const noexcept;
    [[nodiscard]] std::optional<bool> isDirty(PageId pageId) const noexcept;
    [[nodiscard]] std::optional<Lsn> pageLsn(PageId pageId) const noexcept;
    [[nodiscard]] std::optional<Lsn> recLsn(PageId pageId) const noexcept;
    [[nodiscard]] std::vector<DirtyPageEntry> dirtyPageTableSnapshot() const;
    [[nodiscard]] std::optional<DiskManager::Page> residentPageCopy(PageId pageId) const;
    // Finalizes a transaction's resident page image after WAL append, including
    // both volatile and persistent PageLSN assignment.
    void prepareResidentPageForCommit(PageId pageId);
    [[nodiscard]] std::uint64_t totalPinCount() const noexcept;

    // Recovery/rollback invalidation never writes dirty contents.
    void discardPageForRecovery(PageId pageId);
    void discardPagesAtOrAboveForRecovery(PageId firstPageId);

    void validate() const;
    void validateReplacer() const;

private:
    friend class BasicPageGuard;

    DiskManager& diskManager_;
    // Recursive only for existing metadata helper/snapshot calls. All LRU-K
    // access is under this latch; the replacer itself has no redundant mutex.
    mutable std::recursive_mutex metadataLatch_;
    std::condition_variable_any frameReleased_;
    std::vector<BufferFrame> frames_;
    std::vector<std::unique_ptr<std::shared_mutex>> frameLatches_;
    std::unordered_map<PageId, FrameId> pageTable_;
    std::deque<FrameId> freeFrames_;
    LRUKReplacer replacer_;
    WalFlushProvider* walProvider_;
    PageRecoveryHook* recoveryHook_;
    BufferPoolStats stats_{};
    bool waitForReadFrames_;

    [[nodiscard]] std::optional<BasicPageGuard> fetchPage(PageId pageId, bool writable);
    [[nodiscard]] std::optional<FrameId> availableFrame() const;
    void flushVictimIfDirty(FrameId frameId);
    void ensureWalDurable(Lsn pageLsn);
    void ensureWalDurableBeforePageWrite(const BufferFrame& frame);
    void prepareFrameForWrite(BufferFrame& frame);
    void installPage(FrameId frameId, PageId pageId, DiskManager::Page page, bool dirty);
    void releasePin(FrameId frameId);
    void noteGuardWriteIntent(FrameId frameId, PageId pageId);
    [[nodiscard]] std::span<const std::byte, database_format::PAGE_SIZE> readData(
        FrameId frameId,
        PageId pageId) const;
    [[nodiscard]] std::span<std::byte, database_format::PAGE_SIZE> mutableData(
        FrameId frameId,
        PageId pageId);
    [[nodiscard]] Lsn guardPageLsn(FrameId frameId, PageId pageId) const;
    void setPageLsn(FrameId frameId, PageId pageId, Lsn pageLsn);
    [[nodiscard]] BufferFrame& requireGuardFrame(FrameId frameId, PageId pageId);
    [[nodiscard]] const BufferFrame& requireGuardFrame(FrameId frameId, PageId pageId) const;
};

} // namespace minidb
