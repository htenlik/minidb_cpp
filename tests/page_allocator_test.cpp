#include "minidb/page_allocator.hpp"

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
    minidb::Pager& pager,
    minidb::PageId pageId,
    std::size_t offset,
    std::uint32_t value) {
    auto& page = pager.getPage(pageId);
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    pager.markDirty(pageId);
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

template <typename Mutator>
void requireCorruptFreePageRejected(std::string_view name, Mutator&& mutate) {
    TemporaryDatabase database(name);
    minidb::Pager pager(database.path().string());
    minidb::PageAllocator allocator(pager);
    const auto pageId = allocator.allocatePage();
    allocator.releasePage(pageId);
    mutate(pager, pageId);
    requireThrows([&] { allocator.validate(); }, "Allocator accepted a corrupt free page");
}

void testInvalidFreeListRootsAreRejected() {
    for (const auto root : {minidb::database_format::METADATA_PAGE_ID, minidb::PageId{99}}) {
        TemporaryDatabase database(root == 0 ? "root_zero" : "root_beyond_file");
        {
            minidb::Pager pager(database.path().string());
            static_cast<void>(pager.allocatePage());
            pager.flushAll();
        }
        writeFileUint32(
            database.path(),
            minidb::database_format::FREE_LIST_ROOT_PAGE_ID_OFFSET,
            root);
        minidb::Pager pager(database.path().string());
        requireThrows(
            [&] { static_cast<void>(minidb::PageAllocator(pager)); },
            "Allocator accepted an invalid persisted free-list root");
    }
}

void testFreePageCorruptionIsRejected() {
    requireCorruptFreePageRejected("bad_magic", [](auto& pager, auto pageId) {
        auto& page = pager.getPage(pageId);
        page[minidb::free_page_layout::MAGIC_OFFSET] = std::byte{'X'};
        pager.markDirty(pageId);
    });
    requireCorruptFreePageRejected("bad_version", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::free_page_layout::LAYOUT_VERSION_OFFSET,
            minidb::free_page_layout::CURRENT_VERSION + 1);
    });
    requireCorruptFreePageRejected("bad_header", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::free_page_layout::HEADER_SIZE_OFFSET,
            minidb::free_page_layout::HEADER_SIZE + 1);
    });
    requireCorruptFreePageRejected("next_zero", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            minidb::database_format::METADATA_PAGE_ID);
    });
    requireCorruptFreePageRejected("next_beyond", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            pager.pageCount() + 10);
    });
    requireCorruptFreePageRejected("self_loop", [](auto& pager, auto pageId) {
        writeUint32(
            pager,
            pageId,
            minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
            pageId);
    });
    requireCorruptFreePageRejected("reserved_bytes", [](auto& pager, auto pageId) {
        auto& page = pager.getPage(pageId);
        page[minidb::free_page_layout::RESERVED_OFFSET] = std::byte{1};
        pager.markDirty(pageId);
    });

    TemporaryDatabase database("two_page_cycle");
    minidb::Pager pager(database.path().string());
    minidb::PageAllocator allocator(pager);
    const auto first = allocator.allocatePage();
    const auto second = allocator.allocatePage();
    allocator.releasePage(first);
    allocator.releasePage(second);
    writeUint32(
        pager,
        first,
        minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
        second);
    requireThrows([&] { allocator.validate(); }, "Allocator accepted a multi-page free-list cycle");
}

} // namespace

int main() {
    testAppendReleaseAndLifoReuse();
    testFreeListPersistsAcrossReopen();
    testInvalidReleaseAndDoubleFreeAreRejected();
    testReusedPageIsZeroed();
    testInvalidFreeListRootsAreRejected();
    testFreePageCorruptionIsRejected();
    return 0;
}
