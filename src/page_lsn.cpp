#include "minidb/page_lsn.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/slotted_page.hpp"
#include "minidb/tuple_store.hpp"

#include <algorithm>
#include <stdexcept>

namespace minidb {
namespace {

template <std::size_t Size>
bool hasMagic(
    std::span<const std::byte, database_format::PAGE_SIZE> page,
    const std::array<std::byte, Size>& magic) {
    return std::equal(magic.begin(), magic.end(), page.begin());
}

void requireUint32(
    std::span<const std::byte, database_format::PAGE_SIZE> page,
    std::size_t offset,
    std::uint32_t expected,
    const char* message) {
    if (byte_codec::readUint32(page, offset) != expected) {
        throw std::runtime_error(message);
    }
}

} // namespace

std::uint64_t encodePersistentPageLsn(Lsn lsn) {
    if (!isValidLsn(lsn)) return 0;
    if (lsn == 0) {
        throw std::invalid_argument("LSN zero is reserved by the persistent PageLSN encoding");
    }
    return lsn;
}

Lsn decodePersistentPageLsn(std::uint64_t encoded) {
    if (encoded == 0) return INVALID_LSN;
    if (encoded == INVALID_LSN) {
        throw std::runtime_error("Persistent PageLSN contains the in-memory invalid sentinel");
    }
    return encoded;
}

std::optional<PersistentPageLsnSlot> persistentPageLsnSlot(
    std::span<const std::byte, database_format::PAGE_SIZE> page) {
    if (hasMagic(page, database_format::MAGIC)) {
        requireUint32(page, database_format::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(database_format::HEADER_SIZE),
                      "Database metadata page has an invalid header size");
        const auto version = byte_codec::readUint32(page, database_format::FORMAT_VERSION_OFFSET);
        if (version != database_format::LEGACY_VERSION
            && version != database_format::CURRENT_VERSION) {
            throw std::runtime_error("Database metadata page has an unsupported format version");
        }
        if (version == database_format::LEGACY_VERSION) return std::nullopt;
        return PersistentPageLsnSlot{
            PersistentPageType::DatabaseMetadata, database_format::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, free_page_layout::MAGIC)) {
        requireUint32(page, free_page_layout::LAYOUT_VERSION_OFFSET,
                      free_page_layout::CURRENT_VERSION,
                      "Free page has an unsupported layout version");
        requireUint32(page, free_page_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(free_page_layout::HEADER_SIZE),
                      "Free page has an invalid header size");
        return PersistentPageLsnSlot{PersistentPageType::Free, free_page_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, slotted_page_layout::MAGIC)) {
        requireUint32(page, slotted_page_layout::LAYOUT_VERSION_OFFSET,
                      slotted_page_layout::CURRENT_VERSION,
                      "Slotted page has an unsupported layout version");
        requireUint32(page, slotted_page_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(slotted_page_layout::HEADER_SIZE),
                      "Slotted page has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::Slotted, slotted_page_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, tuple_heap_metadata_layout::MAGIC)) {
        requireUint32(page, tuple_heap_metadata_layout::LAYOUT_VERSION_OFFSET,
                      tuple_heap_metadata_layout::CURRENT_VERSION,
                      "Tuple heap metadata has an unsupported layout version");
        requireUint32(page, tuple_heap_metadata_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(tuple_heap_metadata_layout::HEADER_SIZE),
                      "Tuple heap metadata has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::TupleHeapMetadata,
            tuple_heap_metadata_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, persistent_index_metadata_layout::MAGIC)) {
        requireUint32(page, persistent_index_metadata_layout::LAYOUT_VERSION_OFFSET,
                      persistent_index_metadata_layout::CURRENT_VERSION,
                      "Index metadata has an unsupported layout version");
        requireUint32(page, persistent_index_metadata_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(persistent_index_metadata_layout::HEADER_SIZE),
                      "Index metadata has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::IndexMetadata,
            persistent_index_metadata_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, persistent_bplus_leaf_layout::MAGIC)) {
        const auto version = byte_codec::readUint32(
            page, persistent_bplus_leaf_layout::LAYOUT_VERSION_OFFSET);
        if (version == persistent_bplus_leaf_layout::LEGACY_VERSION) {
            requireUint32(page, persistent_bplus_leaf_layout::HEADER_SIZE_OFFSET,
                          static_cast<std::uint32_t>(
                              persistent_bplus_leaf_layout::LEGACY_HEADER_SIZE),
                          "Legacy B+ leaf has an invalid header size");
            return std::nullopt;
        }
        requireUint32(page, persistent_bplus_leaf_layout::LAYOUT_VERSION_OFFSET,
                      persistent_bplus_leaf_layout::CURRENT_VERSION,
                      "B+ leaf has an unsupported layout version");
        requireUint32(page, persistent_bplus_leaf_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(persistent_bplus_leaf_layout::HEADER_SIZE),
                      "B+ leaf has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::BPlusLeaf, persistent_bplus_leaf_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, persistent_bplus_internal_layout::MAGIC)) {
        requireUint32(page, persistent_bplus_internal_layout::LAYOUT_VERSION_OFFSET,
                      persistent_bplus_internal_layout::CURRENT_VERSION,
                      "B+ internal page has an unsupported layout version");
        requireUint32(page, persistent_bplus_internal_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(persistent_bplus_internal_layout::HEADER_SIZE),
                      "B+ internal page has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::BPlusInternal,
            persistent_bplus_internal_layout::PAGE_LSN_OFFSET};
    }
    if (hasMagic(page, catalog_metadata_layout::MAGIC)) {
        requireUint32(page, catalog_metadata_layout::VERSION_OFFSET,
                      catalog_metadata_layout::CURRENT_VERSION,
                      "Catalog metadata has an unsupported layout version");
        requireUint32(page, catalog_metadata_layout::HEADER_SIZE_OFFSET,
                      static_cast<std::uint32_t>(catalog_metadata_layout::HEADER_SIZE),
                      "Catalog metadata has an invalid header size");
        return PersistentPageLsnSlot{
            PersistentPageType::CatalogMetadata, catalog_metadata_layout::PAGE_LSN_OFFSET};
    }
    return std::nullopt;
}

bool supportsPersistentPageLsn(
    std::span<const std::byte, database_format::PAGE_SIZE> page) {
    return persistentPageLsnSlot(page).has_value();
}

Lsn readPersistentPageLsn(
    std::span<const std::byte, database_format::PAGE_SIZE> page) {
    const auto slot = persistentPageLsnSlot(page);
    if (!slot.has_value()) return INVALID_LSN;
    return decodePersistentPageLsn(byte_codec::readUint64(page, slot->offset));
}

void writePersistentPageLsn(
    std::span<std::byte, database_format::PAGE_SIZE> page,
    Lsn lsn) {
    const auto slot = persistentPageLsnSlot(page);
    if (!slot.has_value()) {
        throw std::invalid_argument("Page type does not support persistent PageLSN metadata");
    }
    byte_codec::writeUint64(page, slot->offset, encodePersistentPageLsn(lsn));
}

void clearPersistentPageLsn(
    std::span<std::byte, database_format::PAGE_SIZE> page) {
    const auto slot = persistentPageLsnSlot(page);
    if (slot.has_value()) byte_codec::writeUint64(page, slot->offset, 0);
}

} // namespace minidb
