#include "minidb/page_allocator.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view name)
        : path_(std::filesystem::temp_directory_path()
                / ("minidb_cpp_allocator_" + std::string(name) + "_"
                   + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count())
                   + ".db")) {}

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void requireThrows(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void testAppendReleaseAndLifoReuse() {
    TemporaryDatabase database("reuse");
    minidb::Pager pager(database.path().string());
    minidb::PageAllocator allocator(pager);

    const auto first = allocator.allocatePage();
    const auto second = allocator.allocatePage();
    require(first == 1 && second == 2, "Fresh allocation did not append normal data pages");
    const auto pageCount = pager.pageCount();

    allocator.releasePage(first);
    allocator.releasePage(second);
    require(allocator.freePageIds() == std::vector<minidb::PageId>({second, first}),
            "Released pages were not linked in LIFO order");
    require(allocator.allocatePage() == second, "Allocator did not reuse the free-list head");
    require(allocator.allocatePage() == first, "Allocator did not reuse the remaining page");
    require(pager.pageCount() == pageCount, "Reuse unexpectedly grew the database file");
    require(allocator.freePageIds().empty(), "Free list was not empty after both pages were reused");
}

void testFreeListPersistsAcrossReopen() {
    TemporaryDatabase database("reopen");
    minidb::PageId first = minidb::INVALID_PAGE_ID;
    minidb::PageId second = minidb::INVALID_PAGE_ID;
    {
        minidb::Pager pager(database.path().string());
        minidb::PageAllocator allocator(pager);
        first = allocator.allocatePage();
        second = allocator.allocatePage();
        allocator.releasePage(first);
        allocator.releasePage(second);
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        minidb::PageAllocator allocator(pager);
        require(pager.databaseHeader().freeListRootPageId == second,
                "Free-list root did not persist in database metadata");
        require(allocator.allocatePage() == second, "Reopened allocator did not pop persisted head");
        require(allocator.allocatePage() == first, "Reopened allocator lost persisted next link");
    }
}

void testInvalidReleaseAndDoubleFreeAreRejected() {
    TemporaryDatabase database("invalid_release");
    minidb::Pager pager(database.path().string());
    minidb::PageAllocator allocator(pager);
    const auto pageId = allocator.allocatePage();
    allocator.releasePage(pageId);

    requireThrows(
        [&] { allocator.releasePage(pageId); },
        "Allocator accepted a double release");
    requireThrows(
        [&] { allocator.releasePage(minidb::database_format::METADATA_PAGE_ID); },
        "Allocator accepted metadata page zero");
    requireThrows(
        [&] { allocator.releasePage(minidb::INVALID_PAGE_ID); },
        "Allocator accepted INVALID_PAGE_ID");
    requireThrows(
        [&] { allocator.releasePage(pager.pageCount()); },
        "Allocator accepted a nonexistent page");
}

void testReusedPageIsZeroed() {
    TemporaryDatabase database("zeroed");
    minidb::Pager pager(database.path().string());
    minidb::PageAllocator allocator(pager);
    const auto pageId = allocator.allocatePage();
    auto& page = pager.getPage(pageId);
    page.fill(std::byte{0xA5});
    pager.markDirty(pageId);

    allocator.releasePage(pageId);
    require(allocator.allocatePage() == pageId, "Released page was not reused");
    require(std::all_of(
                pager.getPage(pageId).begin(),
                pager.getPage(pageId).end(),
                [](std::byte value) { return value == std::byte{0}; }),
            "Reused page retained its free-page or former payload bytes");
}

} // namespace

int main() {
    testAppendReleaseAndLifoReuse();
    testFreeListPersistsAcrossReopen();
    testInvalidReleaseAndDoubleFreeAreRejected();
    testReusedPageIsZeroed();
    return 0;
}
