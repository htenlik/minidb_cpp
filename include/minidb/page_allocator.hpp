#pragma once

#include "minidb/pager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace minidb {

namespace free_page_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'F'},
    std::byte{'R'}, std::byte{'E'}, std::byte{'E'}, std::byte{'P'},
};
inline constexpr std::size_t LAYOUT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t HEADER_SIZE_OFFSET = LAYOUT_VERSION_OFFSET + 4;
inline constexpr std::size_t NEXT_FREE_PAGE_ID_OFFSET = HEADER_SIZE_OFFSET + 4;
inline constexpr std::size_t RESERVED_OFFSET = NEXT_FREE_PAGE_ID_OFFSET + 4;
inline constexpr std::size_t HEADER_SIZE = 32;
inline constexpr std::size_t RESERVED_SIZE = Pager::PAGE_SIZE - RESERVED_OFFSET;

static_assert(RESERVED_OFFSET == 20);
static_assert(HEADER_SIZE <= Pager::PAGE_SIZE);

} // namespace free_page_layout

// Allocates reusable data pages from a persisted LIFO free list, falling back to
// Pager's append-only allocation primitive when the list is empty.
class PageAllocator {
public:
    explicit PageAllocator(Pager& pager);

    [[nodiscard]] PageId allocatePage();
    void releasePage(PageId pageId);

    void validate() const;
    [[nodiscard]] std::vector<PageId> freePageIds() const;

private:
    Pager& pager_;

    [[nodiscard]] PageId readNextFreePageId(PageId pageId) const;
    void writeFreePage(PageId pageId, PageId nextPageId);
};

} // namespace minidb
