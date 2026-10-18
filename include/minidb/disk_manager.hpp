#pragma once

#include "minidb/database_format.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <string>
#include <cstdint>
#include <mutex>

namespace minidb {

struct PhysicalWritebackStats {
    std::uint64_t physicalWriteEpoch = 0;
    std::uint64_t synchronizedWriteEpoch = 0;
    std::uint64_t databaseSyncsCompleted = 0;
    std::uint64_t databaseSyncFailures = 0;
    bool writeFailure = false;
    // An opened file may contain unsynchronized writes from its previous
    // process. Do not fabricate a write epoch, or infer durability from open().
    bool initialSyncRequired = true;

    [[nodiscard]] bool pending() const noexcept {
        return writeFailure || initialSyncRequired
            || physicalWriteEpoch != synchronizedWriteEpoch;
    }
};

// Owns the database file and performs validated fixed-page physical I/O. It does not
// cache pages, choose eviction victims, or track dirty state.
class DiskManager {
public:
    static constexpr std::size_t PAGE_SIZE = database_format::PAGE_SIZE;
    using Page = std::array<std::byte, PAGE_SIZE>;

    explicit DiskManager(const std::string& path);
    ~DiskManager() = default;

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void readPage(PageId pageId, Page& output);
    void writePage(PageId pageId, const Page& page);
    // Raw physical primitive, not a transactional allocator. WAL-backed normal
    // append goes through BufferPoolManager's pre-extension recovery boundary.
    [[nodiscard]] PageId appendPage();
    // Check and append atomically under the I/O mutex; fail before writing if
    // an administrative append changed the extent since reservation.
    [[nodiscard]] PageId appendPage(PageId expectedPageId);
    void flush();
    void sync();
    // Atomically checks the physical frontier and fsyncs only if needed. True
    // means a successful fsync; a failed fsync never advances the frontier.
    [[nodiscard]] bool syncPendingWritebacks();
    [[nodiscard]] PhysicalWritebackStats writebackStats() const noexcept;

    // Recovery-only physical I/O includes page 0 and may extend the file. These
    // APIs deliberately bypass the normal "data pages only" boundary.
    void readPhysicalPage(PageId pageId, Page& output);
    void writePhysicalPage(PageId pageId, const Page& page);
    void truncateToPageCount(std::uint64_t pageCount);
    void reloadDatabaseHeader();

    void updateCatalogRootPageId(PageId pageId);
    void updateFreeListRootPageId(PageId pageId);

    [[nodiscard]] PageId pageCount() const noexcept {
        std::lock_guard lock(ioLatch_);
        return pageCount_;
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] database_format::DatabaseHeader databaseHeader() const noexcept {
        std::lock_guard lock(ioLatch_);
        return databaseHeader_;
    }

private:
    friend struct DiskManagerTestAccess;
    std::string path_;
    mutable std::recursive_mutex ioLatch_;
    std::fstream file_;
    PageId pageCount_ = 0;
    database_format::DatabaseHeader databaseHeader_{};
    PhysicalWritebackStats writeback_{};
    // Private syscall seam for deterministic fsync failures/storage-loss tests.
    int (*syncFunction_)(int) = nullptr;

    void openOrCreate();
    void initializeDatabase();
    void loadAndValidateDatabaseHeader();
    void persistDatabaseHeader(const database_format::DatabaseHeader& header);
    void requireExistingDataPage(PageId pageId) const;
    void requireExistingPhysicalPage(PageId pageId) const;
    void reopenFile();
    void requireWritable() const;
    void requireWriteEpochAvailable() const;
    void writePageBytes(PageId pageId, const Page& page, bool flushStream);
};

} // namespace minidb
