#pragma once

#include "minidb/database_format.hpp"
#include "minidb/wal_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace minidb {

enum class PersistentPageType : std::uint8_t {
    DatabaseMetadata,
    Free,
    Slotted,
    TupleHeapMetadata,
    IndexMetadata,
    BPlusLeaf,
    BPlusInternal,
    CatalogMetadata,
};

struct PersistentPageLsnSlot {
    PersistentPageType pageType;
    std::size_t offset;
    bool operator==(const PersistentPageLsnSlot&) const = default;
};

// Persistent encoding deliberately differs from the in-memory INVALID_LSN sentinel.
// Zero means "unknown/no PageLSN"; valid WAL records always begin above zero.
[[nodiscard]] std::uint64_t encodePersistentPageLsn(Lsn lsn);
[[nodiscard]] Lsn decodePersistentPageLsn(std::uint64_t encoded);

// Unknown page magic returns nullopt. A recognized but malformed page throws.
[[nodiscard]] std::optional<PersistentPageLsnSlot> persistentPageLsnSlot(
    std::span<const std::byte, database_format::PAGE_SIZE> page);
[[nodiscard]] bool supportsPersistentPageLsn(
    std::span<const std::byte, database_format::PAGE_SIZE> page);
[[nodiscard]] Lsn readPersistentPageLsn(
    std::span<const std::byte, database_format::PAGE_SIZE> page);
void writePersistentPageLsn(
    std::span<std::byte, database_format::PAGE_SIZE> page,
    Lsn lsn);
void clearPersistentPageLsn(
    std::span<std::byte, database_format::PAGE_SIZE> page);

} // namespace minidb
