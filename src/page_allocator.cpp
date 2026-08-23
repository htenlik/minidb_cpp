#include "minidb/page_allocator.hpp"

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

void requireExistingDataPage(const Pager& pager, PageId pageId, const char* description) {
    if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
        || pageId >= pager.pageCount()) {
        throw std::runtime_error(std::string(description) + " is not an existing data page.");
    }
}

} // namespace

PageAllocator::PageAllocator(Pager& pager) : pager_(pager) {
    validate();
}

PageId PageAllocator::readNextFreePageId(PageId pageId) const {
    requireExistingDataPage(pager_, pageId, "Free-list page ID");
    const auto& page = pager_.getPage(pageId);
    const std::span<const std::byte> bytes(page);

    if (!std::equal(
            free_page_layout::MAGIC.begin(),
            free_page_layout::MAGIC.end(),
            bytes.begin() + free_page_layout::MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid free-page magic or page type.");
    }
    if (readUint32LittleEndian(bytes, free_page_layout::LAYOUT_VERSION_OFFSET)
        != free_page_layout::CURRENT_VERSION) {
        throw std::runtime_error("Unsupported free-page layout version.");
    }
    if (readUint32LittleEndian(bytes, free_page_layout::HEADER_SIZE_OFFSET)
        != free_page_layout::HEADER_SIZE) {
        throw std::runtime_error("Invalid free-page header size.");
    }
    if (!std::all_of(
            bytes.begin() + free_page_layout::RESERVED_OFFSET,
            bytes.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Free page contains nonzero reserved bytes.");
    }

    const auto nextPageId = readUint32LittleEndian(
        bytes, free_page_layout::NEXT_FREE_PAGE_ID_OFFSET);
    if (nextPageId != INVALID_PAGE_ID) {
        requireExistingDataPage(pager_, nextPageId, "Next free-page ID");
    }
    return nextPageId;
}

void PageAllocator::writeFreePage(PageId pageId, PageId nextPageId) {
    requireExistingDataPage(pager_, pageId, "Released page ID");
    if (nextPageId != INVALID_PAGE_ID) {
        requireExistingDataPage(pager_, nextPageId, "Next free-page ID");
    }

    auto& page = pager_.getPage(pageId);
    page.fill(std::byte{0});
    std::copy(
        free_page_layout::MAGIC.begin(),
        free_page_layout::MAGIC.end(),
        page.begin() + free_page_layout::MAGIC_OFFSET);
    writeUint32LittleEndian(
        page, free_page_layout::LAYOUT_VERSION_OFFSET, free_page_layout::CURRENT_VERSION);
    writeUint32LittleEndian(
        page, free_page_layout::HEADER_SIZE_OFFSET, free_page_layout::HEADER_SIZE);
    writeUint32LittleEndian(
        page, free_page_layout::NEXT_FREE_PAGE_ID_OFFSET, nextPageId);
    pager_.markDirty(pageId);
}

std::vector<PageId> PageAllocator::freePageIds() const {
    std::vector<PageId> result;
    std::unordered_set<PageId> visited;
    auto pageId = pager_.databaseHeader().freeListRootPageId;

    while (pageId != INVALID_PAGE_ID) {
        if (!visited.insert(pageId).second) {
            throw std::runtime_error("Free-list cycle or duplicate page detected.");
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
    const auto pageId = pager_.databaseHeader().freeListRootPageId;
    if (pageId == INVALID_PAGE_ID) {
        return pager_.allocatePage();
    }

    const auto nextPageId = readNextFreePageId(pageId);
    pager_.updateFreeListRootPageId(nextPageId);
    auto& page = pager_.getPage(pageId);
    page.fill(std::byte{0});
    pager_.markDirty(pageId);
    return pageId;
}

void PageAllocator::releasePage(PageId pageId) {
    requireExistingDataPage(pager_, pageId, "Released page ID");
    const auto freePages = freePageIds();
    if (std::find(freePages.begin(), freePages.end(), pageId) != freePages.end()) {
        throw std::runtime_error("Cannot release a page that is already free.");
    }

    const auto previousHead = pager_.databaseHeader().freeListRootPageId;
    writeFreePage(pageId, previousHead);
    pager_.updateFreeListRootPageId(pageId);
}

} // namespace minidb
