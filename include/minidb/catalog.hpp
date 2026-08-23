#pragma once

#include "minidb/page_allocator.hpp"
#include "minidb/table_definition.hpp"
#include "minidb/tuple_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace minidb {

class Table;

namespace catalog_metadata_layout {

using LayoutVersion = std::uint32_t;
inline constexpr LayoutVersion CURRENT_VERSION = 1;
inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t MAGIC_SIZE = 8;
inline constexpr std::array<std::byte, MAGIC_SIZE> MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'C'},
    std::byte{'A'}, std::byte{'M'}, std::byte{'E'}, std::byte{'T'},
};
inline constexpr std::size_t VERSION_OFFSET = 8;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 12;
inline constexpr std::size_t ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET = 16;
inline constexpr std::size_t FLAGS_OFFSET = 20;
inline constexpr std::size_t NEXT_TABLE_ID_OFFSET = 24;
inline constexpr std::size_t TABLE_COUNT_OFFSET = 32;
inline constexpr std::size_t RESERVED_OFFSET = 40;
inline constexpr std::size_t HEADER_SIZE = 64;
inline constexpr std::size_t RESERVED_SIZE = HEADER_SIZE - RESERVED_OFFSET;

static_assert(HEADER_SIZE == 64);

} // namespace catalog_metadata_layout

class Catalog {
public:
    [[nodiscard]] static Catalog openOrCreate(Pager& pager);
    [[nodiscard]] static Catalog open(Pager& pager);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) noexcept = default;
    Catalog& operator=(Catalog&&) = delete;

    [[nodiscard]] PageId metadataPageId() const noexcept { return metadataPageId_; }
    [[nodiscard]] PageId entriesHeapMetadataPageId() const noexcept {
        return entries_.metadataPageId();
    }
    [[nodiscard]] std::uint64_t tableCount() const;
    [[nodiscard]] std::optional<TableDefinition> findTable(std::string_view name) const;
    [[nodiscard]] std::optional<TableDefinition> findTable(TableId tableId) const;
    [[nodiscard]] std::vector<TableDefinition> listTables() const;
    [[nodiscard]] Table createTable(std::string_view name, const Schema& schema);
    [[nodiscard]] Table openTable(std::string_view name) const;
    [[nodiscard]] Table openTable(TableId tableId) const;
    void validate() const;

private:
    struct Metadata {
        PageId entriesHeapMetadataPageId;
        TableId nextTableId;
        std::uint64_t tableCount;
    };

    Pager& pager_;
    PageId metadataPageId_;
    TupleStore entries_;

    Catalog(Pager& pager, PageId metadataPageId, TupleStore entries)
        : pager_(pager), metadataPageId_(metadataPageId), entries_(std::move(entries)) {}

    [[nodiscard]] Metadata readMetadata() const;
    void writeMetadata(const Metadata& metadata);
    [[nodiscard]] static Metadata readMetadataPage(Pager& pager, PageId metadataPageId);
    static void writeMetadataPage(
        Pager& pager,
        PageId metadataPageId,
        const Metadata& metadata);
};

} // namespace minidb
