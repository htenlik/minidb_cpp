#include "minidb/log_manager.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using minidb::test::require;

class TemporaryWal {
public:
    explicit TemporaryWal(std::string_view name)
        : database_(name), path_(minidb::walPathForDatabase(database_.path().string())) {}
    ~TemporaryWal() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    minidb::test::TemporaryDatabase database_;
    std::string path_;
};

minidb::LogRecord record(
    minidb::LogRecordType type,
    std::size_t payloadSize,
    std::byte fill,
    minidb::TransactionId transactionId = 1,
    minidb::Lsn prevLsn = minidb::INVALID_LSN) {
    return minidb::LogRecord{type, transactionId, prevLsn,
                             std::vector<std::byte>(payloadSize, fill)};
}

void appendRaw(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("Could not append test WAL bytes");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Could not persist test WAL bytes");
}

void testBufferedAppendFlushAndLargeRecord() {
    TemporaryWal wal("wal_manager_buffered");
    minidb::LogManager manager(wal.path(), 128);
    manager.resetStats();
    const auto first = manager.append(record(minidb::LogRecordType::Begin, 3, std::byte{1}));
    const auto second = manager.append(record(
        minidb::LogRecordType::PageUpdate, 7, std::byte{2}, 1, first));
    require(first == 64 && second == 64 + 48 + 3,
            "LogManager assigned incorrect byte-offset LSNs");
    require(manager.durableLsn() == minidb::INVALID_LSN,
            "Buffered append was incorrectly considered durable");
    require(manager.stats().bufferedBytes == (48 + 3 + 48 + 7),
            "LogManager buffered-byte gauge is incorrect");
    manager.flushUpTo(first);
    require(manager.durableLsn() == second,
            "flushUpTo did not document whole-buffer durability semantics");
    const auto afterFlush = manager.stats();
    require(afterFlush.bufferFlushes == 1 && afterFlush.fsyncCalls == 1
                && afterFlush.recordsAppended == 2,
            "LogManager flush/fsync/append statistics are incorrect");
    manager.flushUpTo(first);
    require(manager.stats().fsyncCalls == 1,
            "Already-durable flushUpTo performed another fsync");

    const auto large = manager.append(record(
        minidb::LogRecordType::PageUpdate, 1024, std::byte{3}, 2));
    require(large == second + 48 + 7 && manager.durableLsn() == second,
            "Large direct record changed durableLSN before fsync");
    require(manager.stats().bufferedBytes == 0,
            "Record larger than WAL buffer was not written directly");
    manager.flushAll();
    require(manager.durableLsn() == large && manager.stats().fsyncCalls == 2,
            "Large direct record did not become durable on flushAll");
    manager.validate();
}

void testReopenAndExactContinuation() {
    TemporaryWal wal("wal_manager_reopen");
    minidb::Lsn first = minidb::INVALID_LSN;
    minidb::Lsn second = minidb::INVALID_LSN;
    {
        minidb::LogManager manager(wal.path(), 64);
        first = manager.append(record(minidb::LogRecordType::Begin, 5, std::byte{4}));
        second = manager.append(record(
            minidb::LogRecordType::Commit, 0, std::byte{0}, 9, first));
        manager.flushAll();
    }
    minidb::Lsn third = minidb::INVALID_LSN;
    {
        minidb::LogManager manager(wal.path(), 64);
        require(manager.lastAppendedLsn() == second && manager.durableLsn() == second,
                "Reopened LogManager did not reconstruct final durable LSN");
        const auto scanned = manager.scan();
        require(scanned.records.size() == 2 && scanned.records[0].lsn == first,
                "Reopened LogManager did not scan exact prior records");
        third = manager.append(record(
            minidb::LogRecordType::Begin, 11, std::byte{5}, 10));
        require(third == scanned.validBytes,
                "Reopened LogManager did not continue at exact record boundary");
        manager.flushAll();
    }
    {
        minidb::LogManager manager(wal.path());
        require(manager.scan().records.size() == 3
                    && manager.lastAppendedLsn() == third,
                "Second WAL reopen lost the appended continuation");
        manager.validate();
    }
}

void testTruncatedTailRequiresExplicitTruncate() {
    TemporaryWal wal("wal_manager_tail");
    minidb::Lsn second = minidb::INVALID_LSN;
    {
        minidb::LogManager manager(wal.path());
        const auto first = manager.append(record(minidb::LogRecordType::Begin, 4, std::byte{6}));
        second = manager.append(record(
            minidb::LogRecordType::Commit, 4, std::byte{7}, 1, first));
        manager.flushAll();
    }
    const std::vector<std::byte> partial(17, std::byte{0xAB});
    appendRaw(wal.path(), partial);
    {
        minidb::LogManager manager(wal.path());
        require(manager.hasTruncatedTail() && manager.scan().records.size() == 2,
                "LogManager did not expose the incomplete final WAL tail");
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(manager.append(record(
                minidb::LogRecordType::Begin, 0, std::byte{0}))); },
            "LogManager appended past an unhandled truncated tail");
        manager.truncateToLastValidRecord();
        require(!manager.hasTruncatedTail(), "Explicit WAL tail truncation did not clear state");
        const auto next = manager.append(record(
            minidb::LogRecordType::Abort, 2, std::byte{8}, 1, second));
        require(next > second, "Post-truncation WAL append did not remain monotonic");
        manager.flushAll();
    }
    require(!minidb::scanWalFile(wal.path()).truncatedTail,
            "Explicitly truncated/reappended WAL still has a partial tail");
}

} // namespace

int main() {
    try {
        testBufferedAppendFlushAndLargeRecord();
        testReopenAndExactContinuation();
        testTruncatedTailRequiresExplicitTruncate();
        std::cout << "log_manager_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "log_manager_test failed: " << error.what() << '\n';
        return 1;
    }
}
