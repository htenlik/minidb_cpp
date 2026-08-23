#pragma once

#include "minidb/database_format.hpp"
#include "minidb/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace minidb {

using TableId = std::uint64_t;
inline constexpr TableId INVALID_TABLE_ID = 0;

struct TableDefinition {
    TableId tableId;
    std::string name;
    Schema schema;
    PageId heapMetadataPageId;
    PageId primaryIndexMetadataPageId;

    bool operator==(const TableDefinition&) const = default;
};

namespace table_definition_layout {

using EncodingVersion = std::uint32_t;
inline constexpr EncodingVersion CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'T'},
    std::byte{'B'}, std::byte{'L'}, std::byte{'D'}, std::byte{'F'},
};
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t ENCODED_SIZE_OFFSET = 16;
inline constexpr std::size_t FLAGS_OFFSET = 20;
inline constexpr std::size_t TABLE_ID_OFFSET = 24;
inline constexpr std::size_t HEAP_METADATA_PAGE_ID_OFFSET = 32;
inline constexpr std::size_t PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET = 36;
inline constexpr std::size_t TABLE_NAME_LENGTH_OFFSET = 40;
inline constexpr std::size_t RESERVED_OFFSET = 42;
inline constexpr std::size_t SCHEMA_SIZE_OFFSET = 44;
inline constexpr std::size_t HEADER_SIZE = 48;

static_assert(HEADER_SIZE == 48);

} // namespace table_definition_layout

[[nodiscard]] TupleBytes encodeTableDefinition(const TableDefinition& definition);
[[nodiscard]] TableDefinition decodeTableDefinition(std::span<const std::byte> bytes);

} // namespace minidb
