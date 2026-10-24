#pragma once

#include "minidb/page_reclaim.hpp"

namespace minidb {

// Prototype-compatible extension; existing BEGIN and types 1-14 are unchanged.
namespace owned_rollback_layout {
inline constexpr std::uint16_t VERSION = 1;
inline constexpr std::size_t VERSION_OFFSET = 0, SIZE_OFFSET = 2, FLAGS = 4;
inline constexpr std::size_t BEGIN_SIZE = 24, START_PAGE_COUNT = 8, BEGIN_RESERVED = 16;
inline constexpr std::size_t CLEANUP_SIZE = 24, UNDO_END = 8, RESERVATION_COUNT = 16;
inline constexpr std::size_t INTENT_SIZE = 72, RESERVE_LSN = 8, RECLAIM_LSN = 16;
inline constexpr std::size_t RECLAIM_PAYLOAD = 24;
inline constexpr std::size_t PROGRESS_SIZE = 40, PROGRESS_PAGE = 24, PROGRESS_RESERVED = 28, OBSERVED_EXTENT = 32;
static_assert(RECLAIM_PAYLOAD + page_reclaim_layout::SIZE == INTENT_SIZE);
}

struct OwnedBeginPayload {
    std::uint64_t startPageCount = 0;
    bool operator==(const OwnedBeginPayload&) const = default;
};
struct CleanupBeginPayload {
    Lsn undoEnd = INVALID_LSN;
    std::uint64_t reservationCount = 0;
    bool operator==(const CleanupBeginPayload&) const = default;
};
struct CleanupIntentPayload {
    Lsn reserveLsn = INVALID_LSN, reclaimLsn = INVALID_LSN;
    PageReclaimPayload reclaim;
    bool operator==(const CleanupIntentPayload&) const = default;
};
struct CleanupProgressPayload {
    Lsn reserveLsn = INVALID_LSN, reclaimLsn = INVALID_LSN;
    PageId page = INVALID_PAGE_ID;
    std::uint64_t observedExtent = 0;
    bool operator==(const CleanupProgressPayload&) const = default;
};

// Checked prototype association: the system reclaim must occupy the next WAL
// record boundary. This computes identity only; it reserves/appends nothing.
[[nodiscard]] Lsn cleanupReclaimLsn(Lsn intentLsn);
[[nodiscard]] std::vector<std::byte> encodeOwnedBegin(OwnedBeginPayload value);
[[nodiscard]] OwnedBeginPayload decodeOwnedBegin(const LogRecord& record);
[[nodiscard]] std::vector<std::byte> encodeCleanupBegin(CleanupBeginPayload value);
[[nodiscard]] CleanupBeginPayload decodeCleanupBegin(const LogRecord& record);
[[nodiscard]] std::vector<std::byte> encodeCleanupIntent(const CleanupIntentPayload& value);
[[nodiscard]] CleanupIntentPayload decodeCleanupIntent(const LogRecord& record);
[[nodiscard]] std::vector<std::byte> encodeCleanupProgress(CleanupProgressPayload value);
[[nodiscard]] CleanupProgressPayload decodeCleanupProgress(const LogRecord& record);

} // namespace minidb
