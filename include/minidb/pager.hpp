#pragma once

#include "minidb/disk_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace minidb {

struct PagerStats {
    std::uint64_t pageRequests = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t physicalPageReads = 0;
    std::uint64_t physicalPageWrites = 0;
    std::uint64_t dirtyMarks = 0;
    std::uint64_t flushCalls = 0;
    std::uint64_t appendedPages = 0;
    std::uint64_t residentPages = 0;

    bool operator==(const PagerStats&) const = default;
};

class Pager {
public:
    static constexpr std::size_t PAGE_SIZE = DiskManager::PAGE_SIZE;
    using Page = DiskManager::Page;

    explicit Pager(const std::string& path);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    Page& getPage(PageId pageId);
    PageId allocatePage();
    void markDirty(PageId pageId);
    void flush(PageId pageId);
    void flushAll();
    void updateCatalogRootPageId(PageId pageId);
    void updateFreeListRootPageId(PageId pageId);

    [[nodiscard]] PagerStats stats() const noexcept;
    void resetStats() noexcept;
    [[nodiscard]] std::size_t residentPageCount() const noexcept { return cache_.size(); }

    [[nodiscard]] PageId pageCount() const noexcept { return diskManager_.pageCount(); }
    [[nodiscard]] const database_format::DatabaseHeader& databaseHeader() const noexcept {
        return diskManager_.databaseHeader();
    }

private:
    struct Frame {
        Page data{};
        bool dirty = false;
    };

    DiskManager diskManager_;
    std::unordered_map<PageId, std::unique_ptr<Frame>> cache_;
    PagerStats stats_{};

    static void requireDataPage(PageId pageId);
};

} // namespace minidb
