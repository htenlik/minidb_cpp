#pragma once

#include "minidb/database_format.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

namespace minidb {

class Pager {
public:
    static constexpr std::size_t PAGE_SIZE = database_format::PAGE_SIZE;
    using Page = std::array<std::byte, PAGE_SIZE>;

    explicit Pager(const std::string& path);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    Page& getPage(PageId pageId);
    PageId allocatePage();
    void markDirty(PageId pageId);
    void flush(PageId pageId);
    void flushAll();
    void updateFreeListRootPageId(PageId pageId);

    [[nodiscard]] PageId pageCount() const noexcept { return pageCount_; }
    [[nodiscard]] const database_format::DatabaseHeader& databaseHeader() const noexcept {
        return databaseHeader_;
    }

private:
    struct Frame {
        Page data{};
        bool dirty = false;
    };

    std::string path_;
    std::fstream file_;
    PageId pageCount_ = 0;
    database_format::DatabaseHeader databaseHeader_{};
    std::unordered_map<PageId, std::unique_ptr<Frame>> cache_;

    void openOrCreate();
    void initializeDatabase();
    void loadAndValidateDatabaseHeader();
    void persistDatabaseHeader(const database_format::DatabaseHeader& header);
    void loadPageFromDisk(PageId pageId, Frame& frame);
    static void requireDataPage(PageId pageId);
};

} // namespace minidb
