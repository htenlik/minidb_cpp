#include "minidb/page_allocator.hpp"
#include "minidb/page_access.hpp"
#include "minidb/storage_error.hpp"

#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
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

void writeUint32(
    minidb::BufferPoolManager& bufferPool,
    minidb::PageId pageId,
    std::size_t offset,
    std::uint32_t value) {
    auto guard = minidb::requireWritePage(bufferPool, pageId, "corrupt free page");
    auto page = guard.data();
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void writeFileUint32(
    const std::filesystem::path& path,
    std::size_t offset,
    std::uint32_t value) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    require(static_cast<bool>(file), "Could not open database for corruption");
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    std::array<unsigned char, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.flush();
    require(static_cast<bool>(file), "Could not persist database corruption");
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
    minidb::test::TestStorage storage(database.path());
    auto& allocator = storage.allocator;

    const auto first = allocator.allocatePage();
    const auto second = allocator.allocatePage();
    require(first == 1 && second == 2, "Fresh allocation did not append normal data pages");
    const auto pageCount = storage.diskManager.pageCount();

    allocator.releasePage(first);
    allocator.releasePage(second);
    require(allocator.freePageIds() == std::vector<minidb::PageId>({second, first}),
            "Released pages were not linked in LIFO order");
    require(allocator.allocatePage() == second, "Allocator did not reuse the free-list head");
    require(allocator.allocatePage() == first, "Allocator did not reuse the remaining page");
    require(storage.diskManager.pageCount() == pageCount, "Reuse unexpectedly grew the database file");
    require(allocator.freePageIds().empty(), "Free list was not empty after both pages were reused");
}

void testFreeListPersistsAcrossReopen() {
    TemporaryDatabase database("reopen");
    minidb::PageId first = minidb::INVALID_PAGE_ID;
    minidb::PageId second = minidb::INVALID_PAGE_ID;
    {
        minidb::test::TestStorage storage(database.path());
        auto& allocator = storage.allocator;
        first = allocator.allocatePage();
        second = allocator.allocatePage();
        allocator.releasePage(first);
        allocator.releasePage(second);
        storage.bufferPool.flushAll();
    }
    {
        minidb::test::TestStorage storage(database.path());
        auto& allocator = storage.allocator;
        require(storage.diskManager.databaseHeader().freeListRootPageId == second,
                "Free-list root did not persist in database metadata");
        require(allocator.allocatePage() == second, "Reopened allocator did not pop persisted head");
        require(allocator.allocatePage() == first, "Reopened allocator lost persisted next link");
    }
}

void testInvalidReleaseAndDoubleFreeAreRejected() {
    TemporaryDatabase database("invalid_release");
    minidb::test::TestStorage storage(database.path());
    auto& allocator = storage.allocator;
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
        [&] { allocator.releasePage(storage.diskManager.pageCount()); },
        "Allocator accepted a nonexistent page");
}

void testReusedPageIsZeroed() {
    TemporaryDatabase database("zeroed");
    minidb::test::TestStorage storage(database.path());
    auto& allocator = storage.allocator;
    const auto pageId = allocator.allocatePage();
    {
        auto guard = minidb::requireWritePage(storage.bufferPool, pageId, "seed allocated page");
        std::fill(guard.data().begin(), guard.data().end(), std::byte{0xA5});
    }

    allocator.releasePage(pageId);
    require(allocator.allocatePage() == pageId, "Released page was not reused");
    const auto guard = minidb::requireReadPage(storage.bufferPool, pageId, "read reused page");
    require(std::all_of(
                guard.data().begin(), guard.data().end(),
                [](std::byte value) { return value == std::byte{0}; }),
            "Reused page retained its free-page or former payload bytes");
}

template <typename Mutator>
void requireCorruptFreePageRejected(std::string_view name, Mutator&& mutate) {
    TemporaryDatabase database(name);
    minidb::test::TestStorage storage(database.path());
    auto& allocator = storage.allocator;
    const auto pageId = allocator.allocatePage();
    allocator.releasePage(pageId);
    mutate(storage, pageId);
    requireThrows([&] { allocator.validate(); }, "Allocator accepted a corrupt free page");
}

void testInvalidFreeListRootsAreRejected() {
    for (const auto root : {minidb::database_format::METADATA_PAGE_ID, minidb::PageId{99}}) {
        TemporaryDatabase database(root == 0 ? "root_zero" : "root_beyond_file");
        {
            minidb::test::TestStorage storage(database.path());
            static_cast<void>(storage.allocator.allocatePage());
            storage.bufferPool.flushAll();
        }
        writeFileUint32(
            database.path(),
            minidb::database_format::FREE_LIST_ROOT_PAGE_ID_OFFSET,
            root);
        minidb::DiskManager diskManager(database.path().string());
        minidb::BufferPoolManager bufferPool(diskManager, 8);
        requireThrows(
            [&] { static_cast<void>(minidb::PageAllocator(bufferPool, diskManager)); },
            "Allocator accepted an invalid persisted free-list root");
    }
}

void testFreePageCorruptionIsRejected() {
    requireCorruptFreePageRejected("bad_magic", [](auto& storage, auto pageId) {
        auto guard = minidb::requireWritePage(storage.bufferPool, pageId, "corrupt magic");
        auto page = guard.data();
        page[minidb::free_page_layout::MAGIC_OFFSET] = std::byte{'X'};
    });
    requireCorruptFreePageRejected("bad_version", [](auto& storage, auto pageId) {
        writeUint32(
            storage.bufferPool,
            pageId,
            minidb::free_page_layout::LAYOUT_VERSION_OFFSET,
            minidb::free_page_layout::CURRENT_VERSION + 1);
    });
    requireCorruptFreePageRejected("bad_header", [](auto& storage, auto pageId) {
        writeUint32(
            storage.bufferPool,
            pageId,
            minidb::free_page_layout::HEADER_SIZE_OFFSET,
            minidb::free_page_layout::HEADER_SIZE + 1);
    });
    requireCorruptFreePageRejected("next_zero", [](auto& storage, auto pageId) {
        writeUint32(
            storage.bufferPool,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            minidb::database_format::METADATA_PAGE_ID);
    });
    requireCorruptFreePageRejected("next_beyond", [](auto& storage, auto pageId) {
        writeUint32(
            storage.bufferPool,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            storage.diskManager.pageCount() + 10);
    });
    requireCorruptFreePageRejected("self_loop", [](auto& storage, auto pageId) {
        writeUint32(
            storage.bufferPool,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            pageId);
    });
    requireCorruptFreePageRejected("reserved_bytes", [](auto& storage, auto pageId) {
        auto guard = minidb::requireWritePage(storage.bufferPool, pageId, "corrupt reserved byte");
        auto page = guard.data();
        page[minidb::free_page_layout::RESERVED_OFFSET] = std::byte{1};
    });

    TemporaryDatabase database("two_page_cycle");
    minidb::test::TestStorage storage(database.path());
    auto& allocator = storage.allocator;
    const auto first = allocator.allocatePage();
    const auto second = allocator.allocatePage();
    allocator.releasePage(first);
    allocator.releasePage(second);
    writeUint32(
        storage.bufferPool,
        first,
        minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
        second);
    requireThrows([&] { allocator.validate(); }, "Allocator accepted a multi-page free-list cycle");
}

void testPinnedReleaseAndEvictedFreeList() {
    TemporaryDatabase database("pinned_and_evicted");
    minidb::test::TestStorage storage(database.path(), 2, 2);
    std::vector<minidb::PageId> pages;
    for (std::size_t index = 0; index < 24; ++index) {
        pages.push_back(storage.allocator.allocatePage());
    }

    auto retained = minidb::requireWritePage(
        storage.bufferPool, pages.front(), "retain page during release test");
    requireThrows(
        [&] { storage.allocator.releasePage(pages.front()); },
        "Allocator released a page retained by another guard");
    require(storage.diskManager.databaseHeader().freeListRootPageId == minidb::INVALID_PAGE_ID,
            "Failed pinned release changed the free-list root");
    retained.drop();

    for (const auto pageId : pages) {
        storage.allocator.releasePage(pageId);
        storage.bufferPool.validate();
    }
    storage.bufferPool.flushAll();
    minidb::test::requireBufferClean(storage);

    const auto expected = std::vector<minidb::PageId>(pages.rbegin(), pages.rend());
    require(storage.allocator.freePageIds() == expected,
            "Tiny-pool free-list traversal lost LIFO order under eviction");
    for (const auto expectedPageId : expected) {
        require(storage.allocator.allocatePage() == expectedPageId,
                "Tiny-pool free-list pop returned the wrong PageId");
        storage.bufferPool.validate();
    }
    require(storage.allocator.freePageIds().empty(),
            "Tiny-pool free list was not empty after every pop");
    minidb::test::requireBufferClean(storage);
}

void testNoFrameAvailableIsReportedWithoutAllocating() {
    TemporaryDatabase database("no_frame_available");
    minidb::test::TestStorage storage(database.path(), 1, 2);
    const auto pageId = storage.allocator.allocatePage();
    const auto pageCount = storage.diskManager.pageCount();
    auto retained = minidb::requireReadPage(
        storage.bufferPool, pageId, "retain sole frame");
    try {
        static_cast<void>(storage.allocator.allocatePage());
        throw std::runtime_error("Allocator accepted allocation with no evictable frame");
    } catch (const minidb::StorageError& error) {
        require(
            error.kind() == minidb::StorageErrorKind::NoFrameAvailable,
            "Allocator reported the wrong storage error for frame exhaustion");
    }
    require(
        storage.diskManager.pageCount() == pageCount,
        "Failed buffered allocation appended an unreachable disk page");
    retained.drop();
    minidb::test::requireBufferClean(storage);
}

void testEvictedFreeListPersistsAcrossTinyPoolReopens() {
    TemporaryDatabase database("evicted_reopen");
    std::vector<minidb::PageId> pages;
    {
        minidb::test::TestStorage storage(database.path(), 2, 2);
        for (std::size_t index = 0; index < 24; ++index) {
            pages.push_back(storage.allocator.allocatePage());
        }
        for (const auto pageId : pages) {
            storage.allocator.releasePage(pageId);
        }
        storage.allocator.validate();
        storage.bufferPool.flushAll();
        minidb::test::requireBufferClean(storage);
    }

    const std::vector<minidb::PageId> expected(pages.rbegin(), pages.rend());
    {
        minidb::test::TestStorage storage(database.path(), 2, 2);
        require(
            storage.allocator.freePageIds() == expected,
            "Reopened tiny-pool allocator lost the evicted free-list order");
        for (std::size_t index = 0; index < 7; ++index) {
            require(
                storage.allocator.allocatePage() == expected[index],
                "Reopened tiny-pool allocator popped the wrong free page");
        }
        storage.bufferPool.flushAll();
        minidb::test::requireBufferClean(storage);
    }

    {
        minidb::test::TestStorage storage(database.path(), 2, 2);
        const std::vector<minidb::PageId> remaining(expected.begin() + 7, expected.end());
        require(
            storage.allocator.freePageIds() == remaining,
            "Second reopen did not preserve the partially consumed free list");
        for (const auto pageId : remaining) {
            require(
                storage.allocator.allocatePage() == pageId,
                "Second reopen popped a free page out of LIFO order");
        }
        require(storage.allocator.freePageIds().empty(),
                "Reopened free list was not empty after every page was reused");
        minidb::test::requireBufferClean(storage);
    }
}

} // namespace

int main() {
    testAppendReleaseAndLifoReuse();
    testFreeListPersistsAcrossReopen();
    testInvalidReleaseAndDoubleFreeAreRejected();
    testReusedPageIsZeroed();
    testInvalidFreeListRootsAreRejected();
    testFreePageCorruptionIsRejected();
    testPinnedReleaseAndEvictedFreeList();
    testNoFrameAvailableIsReportedWithoutAllocating();
    testEvictedFreeListPersistsAcrossTinyPoolReopens();
    return 0;
}
