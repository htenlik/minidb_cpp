#pragma once

#include "minidb/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace minidb {

struct LogManagerStats {
    std::uint64_t recordsAppended = 0;
    std::uint64_t bytesAppended = 0;
    std::uint64_t bufferFlushes = 0;
    std::uint64_t physicalWrites = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t fsyncCalls = 0;
    std::uint64_t flushUpToCalls = 0;
    std::uint64_t bufferedBytes = 0;
    Lsn lastAppendedLsn = INVALID_LSN;
    Lsn durableLsn = INVALID_LSN;

    bool operator==(const LogManagerStats&) const = default;
};

class LogManager final : public WalFlushProvider {
public:
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;

    explicit LogManager(
        std::string walPath,
        std::size_t bufferCapacity = DEFAULT_BUFFER_SIZE);
    ~LogManager() override;

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    [[nodiscard]] Lsn append(LogRecord record);
    void flushUpTo(Lsn target) override;
    void flushAll();

    [[nodiscard]] Lsn durableLsn() const noexcept override { return durableLsn_; }
    [[nodiscard]] Lsn lastAppendedLsn() const noexcept { return lastAppendedLsn_; }
    [[nodiscard]] bool containsLsn(Lsn lsn) const noexcept override;
    [[nodiscard]] bool hasTruncatedTail() const noexcept { return truncatedTail_; }
    [[nodiscard]] std::uint64_t lastValidOffset() const noexcept { return nextLsn_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] WalScanResult scan() const;
    void validate() const;
    void truncateToLastValidRecord();

    [[nodiscard]] LogManagerStats stats() const noexcept;
    void resetStats() noexcept { stats_ = {}; }

private:
    std::string path_;
    int descriptor_ = -1;
    std::size_t bufferCapacity_;
    std::vector<std::byte> buffer_;
    std::uint64_t bufferStartOffset_ = wal_file_layout::HEADER_SIZE;
    std::uint64_t nextLsn_ = wal_file_layout::HEADER_SIZE;
    Lsn lastAppendedLsn_ = INVALID_LSN;
    Lsn durableLsn_ = INVALID_LSN;
    bool truncatedTail_ = false;
    std::unordered_set<Lsn> knownLsns_;
    LogManagerStats stats_{};

    void openOrCreate();
    void initializeNewWal();
    void loadExistingWal();
    void writeBuffer();
    void writeBytes(std::uint64_t offset, std::span<const std::byte> bytes);
    void syncWal();
    [[nodiscard]] WalScanResult scanIncludingBuffer() const;
};

} // namespace minidb
