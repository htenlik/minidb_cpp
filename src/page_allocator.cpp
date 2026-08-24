#include "minidb/page_allocator.hpp"

#include "minidb/page_access.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace minidb {
namespace {

void writeUint32LittleEndian(
    std::span<std::byte> output,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint32_t readUint32LittleEndian(
    std::span<const std::byte> input,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

void requireExistingDataPage(
    const DiskManager& diskManager,
    PageId pageId,
    const char* description) {
    if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
        || pageId >= diskManager.pageCount()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            std::string(description) + " is not an existing data page.");
    }
}

} // namespace

PageAllocator::PageAllocator(BufferPoolManager& bufferPool, DiskManager& diskManager)
    : bufferPool_(bufferPool), diskManager_(diskManager) {
    validate();
}

PageId PageAllocator::readNextFreePageId(PageId pageId) const {
    requireExistingDataPage(diskManager_, pageId, "Free-list page ID");
    const auto guard = requireReadPage(bufferPool_, pageId, "read free-list page");
    const auto bytes = guard.data();

    if (!std::equal(
            free_page_layout::MAGIC.begin(),
            free_page_layout::MAGIC.end(),
            bytes.begin() + free_page_layout::MAGIC_OFFSET)) {
        throw StorageError(StorageErrorKind::CorruptPage, "Invalid free-page magic or page type.");
    }
    if (readUint32LittleEndian(bytes, free_page_layout::LAYOUT_VERSION_OFFSET)
        != free_page_layout::CURRENT_VERSION) {
        throw StorageError(StorageErrorKind::CorruptPage, "Unsupported free-page layout version.");
    }
    if (readUint32LittleEndian(bytes, free_page_layout::HEADER_SIZE_OFFSET)
        != free_page_layout::HEADER_SIZE) {
        throw StorageError(StorageErrorKind::CorruptPage, "Invalid free-page header size.");
    }
    if (!std::all_of(
            bytes.begin() + free_page_layout::RESERVED_OFFSET,
            bytes.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw StorageError(
            StorageErrorKind::CorruptPage,
            "Free page contains nonzero reserved bytes.");
    }

    const auto nextPageId = readUint32LittleEndian(
        bytes, free_page_layout::NEXT_FREE_PAGE_ID_OFFSET);
    if (nextPageId != INVALID_PAGE_ID) {
        requireExistingDataPage(diskManager_, nextPageId, "Next free-page ID");
    }
    return nextPageId;
}

void PageAllocator::writeFreePage(PageId pageId, PageId nextPageId) {
    requireExistingDataPage(diskManager_, pageId, "Released page ID");
    if (nextPageId != INVALID_PAGE_ID) {
        requireExistingDataPage(diskManager_, nextPageId, "Next free-page ID");
    }

    auto guard = requireWritePage(bufferPool_, pageId, "write free-list page");
    const auto pins = bufferPool_.pinCount(pageId);
    if (!pins.has_value() || *pins != 1) {
        throw StorageError(
            StorageErrorKind::PinnedPageRelease,
            "Cannot release a page while another page guard still pins it.");
    }
    auto bytes = guard.data();
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    std::copy(
        free_page_layout::MAGIC.begin(),
        free_page_layout::MAGIC.end(),
        bytes.begin() + free_page_layout::MAGIC_OFFSET);
    writeUint32LittleEndian(
        bytes, free_page_layout::LAYOUT_VERSION_OFFSET, free_page_layout::CURRENT_VERSION);
    writeUint32LittleEndian(
        bytes, free_page_layout::HEADER_SIZE_OFFSET, free_page_layout::HEADER_SIZE);
    writeUint32LittleEndian(
        bytes, free_page_layout::NEXT_FREE_PAGE_ID_OFFSET, nextPageId);
}

std::vector<PageId> PageAllocator::freePageIds() const {
    std::vector<PageId> result;
    std::unordered_set<PageId> visited;
    auto pageId = diskManager_.databaseHeader().freeListRootPageId;

    while (pageId != INVALID_PAGE_ID) {
        if (!visited.insert(pageId).second) {
            throw StorageError(
                StorageErrorKind::CorruptPage,
                "Free-list cycle or duplicate page detected.");
        }
        result.push_back(pageId);
        pageId = readNextFreePageId(pageId);
    }
    return result;
}

void PageAllocator::validate() const {
    static_cast<void>(freePageIds());
}

PageId PageAllocator::allocatePage() {
    const auto pageId = diskManager_.databaseHeader().freeListRootPageId;
    if (pageId == INVALID_PAGE_ID) {
        auto guard = requireNewPage(bufferPool_, "append allocated page");
        const auto allocatedPageId = guard.pageId();
        auto bytes = guard.data();
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        return allocatedPageId;
    }

    const auto nextPageId = readNextFreePageId(pageId);
    diskManager_.updateFreeListRootPageId(nextPageId);
    {
        auto guard = requireWritePage(bufferPool_, pageId, "reuse free-list page");
        auto bytes = guard.data();
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
    }
    return pageId;
}

void PageAllocator::releasePage(PageId pageId) {
    requireExistingDataPage(diskManager_, pageId, "Released page ID");
    const auto freePages = freePageIds();
    if (std::find(freePages.begin(), freePages.end(), pageId) != freePages.end()) {
        throw StorageError(
            StorageErrorKind::InvalidPage,
            "Cannot release a page that is already free.");
    }

    const auto previousHead = diskManager_.databaseHeader().freeListRootPageId;
    writeFreePage(pageId, previousHead);
    diskManager_.updateFreeListRootPageId(pageId);
}

} // namespace minidb
