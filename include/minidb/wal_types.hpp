#pragma once

#include <cstdint>
#include <limits>

namespace minidb {

using Lsn = std::uint64_t;
inline constexpr Lsn INVALID_LSN = std::numeric_limits<Lsn>::max();

using TransactionId = std::uint64_t;
inline constexpr TransactionId INVALID_TRANSACTION_ID = 0;

enum class LogRecordType : std::uint16_t {
    Begin = 1,
    PageUpdate = 2,
    Commit = 3,
    Abort = 4,
    Compensation = 5,
    CheckpointBegin = 6,
    CheckpointEnd = 7,
};

[[nodiscard]] constexpr bool isValidLsn(Lsn lsn) noexcept {
    return lsn != INVALID_LSN;
}

[[nodiscard]] constexpr bool isValidLogRecordType(LogRecordType type) noexcept {
    const auto value = static_cast<std::uint16_t>(type);
    return value >= static_cast<std::uint16_t>(LogRecordType::Begin)
        && value <= static_cast<std::uint16_t>(LogRecordType::CheckpointEnd);
}

class WalFlushProvider {
public:
    virtual ~WalFlushProvider() = default;
    [[nodiscard]] virtual Lsn durableLsn() const noexcept = 0;
    [[nodiscard]] virtual bool containsLsn(Lsn lsn) const noexcept = 0;
    virtual void flushUpTo(Lsn lsn) = 0;
};

} // namespace minidb
