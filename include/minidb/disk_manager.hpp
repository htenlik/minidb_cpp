#pragma once

#include "minidb/database_format.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <string>

namespace minidb {

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
    [[nodiscard]] PageId appendPage();
    void flush();

    void updateCatalogRootPageId(PageId pageId);
    void updateFreeListRootPageId(PageId pageId);

    [[nodiscard]] PageId pageCount() const noexcept { return pageCount_; }
    [[nodiscard]] const database_format::DatabaseHeader& databaseHeader() const noexcept {
        return databaseHeader_;
    }

private:
    std::string path_;
    std::fstream file_;
    PageId pageCount_ = 0;
    database_format::DatabaseHeader databaseHeader_{};

    void openOrCreate();
    void initializeDatabase();
    void loadAndValidateDatabaseHeader();
    void persistDatabaseHeader(const database_format::DatabaseHeader& header);
    void requireExistingDataPage(PageId pageId) const;
};

} // namespace minidb
