#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace minidb {

using Lsn = std::uint64_t;
inline constexpr Lsn INVALID_LSN = std::numeric_limits<Lsn>::max();

// A position in the global logical WAL byte stream. Historical code used the
// name WalOffset while the stream was stored in one file; keep the alias for
// on-disk/API compatibility, but it is not a segment-local file offset.
using WalOffset = Lsn;
inline constexpr WalOffset INVALID_WAL_OFFSET = std::numeric_limits<WalOffset>::max();

using WalSegmentId = std::uint64_t;
inline constexpr WalSegmentId INVALID_WAL_SEGMENT_ID = 0;

using TransactionId = std::uint64_t;
inline constexpr TransactionId INVALID_TRANSACTION_ID = 0;

using CheckpointId = std::uint64_t;
inline constexpr CheckpointId INVALID_CHECKPOINT_ID = 0;

enum class LogRecordType : std::uint16_t {
    Begin = 1,
    PageUpdate = 2,
    Commit = 3,
    Abort = 4,
    Compensation = 5,
    CheckpointBegin = 6,
    CheckpointEnd = 7,
    PageDeltaUpdate = 8,
};

enum class WalUpdateMode : std::uint8_t {
    FullPage,
    ByteRange,
    Adaptive,
};

[[nodiscard]] constexpr std::string_view walUpdateModeName(
    WalUpdateMode mode) noexcept {
    switch (mode) {
    case WalUpdateMode::FullPage: return "full-page";
    case WalUpdateMode::ByteRange: return "byte-range";
    case WalUpdateMode::Adaptive: return "adaptive";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool isValidLsn(Lsn lsn) noexcept {
    return lsn != INVALID_LSN;
}

[[nodiscard]] constexpr bool isValidLogRecordType(LogRecordType type) noexcept {
    const auto value = static_cast<std::uint16_t>(type);
    return value >= static_cast<std::uint16_t>(LogRecordType::Begin)
        && value <= static_cast<std::uint16_t>(LogRecordType::PageDeltaUpdate);
}

class WalFlushProvider {
public:
    virtual ~WalFlushProvider() = default;
    [[nodiscard]] virtual Lsn durableLsn() const noexcept = 0;
    [[nodiscard]] virtual bool containsLsn(Lsn lsn) const noexcept = 0;
    virtual void flushUpTo(Lsn lsn) = 0;
};

} // namespace minidb
