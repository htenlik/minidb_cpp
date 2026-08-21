#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace minidb {

using PageId = std::uint32_t;
inline constexpr PageId INVALID_PAGE_ID = std::numeric_limits<PageId>::max();

namespace database_format {

using FormatVersion = std::uint32_t;

inline constexpr std::size_t PAGE_SIZE = 4096;
inline constexpr PageId METADATA_PAGE_ID = 0;
inline constexpr FormatVersion CURRENT_VERSION = 1;

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'},
    std::byte{'I'},
    std::byte{'N'},
    std::byte{'I'},
    std::byte{'D'},
    std::byte{'B'},
    std::byte{'+'},
    std::byte{'+'},
};

inline constexpr std::size_t FORMAT_VERSION_OFFSET = MAGIC_OFFSET + MAGIC_SIZE;
inline constexpr std::size_t FORMAT_VERSION_SIZE = 4;
inline constexpr std::size_t PAGE_SIZE_OFFSET = FORMAT_VERSION_OFFSET + FORMAT_VERSION_SIZE;
inline constexpr std::size_t PAGE_SIZE_FIELD_SIZE = 4;
inline constexpr std::size_t HEADER_SIZE_OFFSET = PAGE_SIZE_OFFSET + PAGE_SIZE_FIELD_SIZE;
inline constexpr std::size_t HEADER_SIZE_FIELD_SIZE = 4;
inline constexpr std::size_t CATALOG_ROOT_PAGE_ID_OFFSET =
    HEADER_SIZE_OFFSET + HEADER_SIZE_FIELD_SIZE;
inline constexpr std::size_t PAGE_ID_FIELD_SIZE = 4;
inline constexpr std::size_t FREE_LIST_ROOT_PAGE_ID_OFFSET =
    CATALOG_ROOT_PAGE_ID_OFFSET + PAGE_ID_FIELD_SIZE;
inline constexpr std::size_t RESERVED_OFFSET =
    FREE_LIST_ROOT_PAGE_ID_OFFSET + PAGE_ID_FIELD_SIZE;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t RESERVED_SIZE = HEADER_SIZE - RESERVED_OFFSET;

struct DatabaseHeader {
    FormatVersion formatVersion = CURRENT_VERSION;
    std::uint32_t pageSize = static_cast<std::uint32_t>(PAGE_SIZE);
    std::uint32_t headerSize = static_cast<std::uint32_t>(HEADER_SIZE);
    PageId catalogRootPageId = INVALID_PAGE_ID;
    PageId freeListRootPageId = INVALID_PAGE_ID;

    bool operator==(const DatabaseHeader&) const = default;
};

[[nodiscard]] DatabaseHeader makeCurrentDatabaseHeader() noexcept;
void serializeDatabaseHeader(const DatabaseHeader& header, std::span<std::byte> metadataPage);
[[nodiscard]] DatabaseHeader deserializeDatabaseHeader(
    std::span<const std::byte> metadataPage);

} // namespace database_format
} // namespace minidb
