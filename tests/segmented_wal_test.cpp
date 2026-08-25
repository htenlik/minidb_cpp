#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_log.hpp"
#include "minidb/disk_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/segmented_wal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

using minidb::test::require;

minidb::LogRecord record(
    std::size_t payloadSize, std::uint64_t transactionId, minidb::Lsn previous) {
    return minidb::LogRecord{
        minidb::LogRecordType::PageUpdate,
        transactionId,
        previous,
        std::vector<std::byte>(payloadSize, static_cast<std::byte>(transactionId & 0xFFU)),
        minidb::INVALID_LSN,
    };
}

std::vector<std::filesystem::path> segmentFiles(const std::string& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".seg") result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void appendRaw(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Could not append segmented-WAL test bytes");
}

void testExactCodecs() {
    const minidb::WalSegmentHeader header{7, 0x0102030405060708ULL, 6,
        0x0102030405060708ULL, 16U * 1024U * 1024U, 0};
    const auto bytes = minidb::encodeWalSegmentHeader(header);
    require(bytes.size() == 64
                && std::equal(minidb::wal_segment_layout::MAGIC.begin(),
                              minidb::wal_segment_layout::MAGIC.end(), bytes.begin()),
            "Segment header magic/size changed");
    require(minidb::byte_codec::readUint32(bytes, 8) == 1
                && minidb::byte_codec::readUint32(bytes, 12) == 64
                && minidb::byte_codec::readUint64(bytes, 16) == 7
                && minidb::byte_codec::readUint64(bytes, 24) == header.startLsn
                && minidb::byte_codec::readUint64(bytes, 32) == 6
                && minidb::byte_codec::readUint64(bytes, 40) == header.startLsn
                && minidb::byte_codec::readUint32(bytes, 48) == 16U * 1024U * 1024U
                && minidb::byte_codec::readUint32(bytes, 52) == 0
                && minidb::byte_codec::readUint32(bytes, 60) == 0,
            "Segment header field offsets changed");
    require(minidb::decodeWalSegmentHeader(bytes) == header,
            "Segment header round trip changed fields");

    const minidb::WalSegmentManifest manifest{4096, 9, 64, 1234};
    const auto manifestBytes = minidb::encodeWalSegmentManifest(manifest);
    require(minidb::byte_codec::readUint32(manifestBytes, 16) == 4096
                && minidb::byte_codec::readUint64(manifestBytes, 24) == 9
                && minidb::byte_codec::readUint64(manifestBytes, 32) == 64
                && minidb::byte_codec::readUint64(manifestBytes, 40) == 1234
                && minidb::decodeWalSegmentManifest(manifestBytes) == manifest,
            "Segment manifest layout/round trip changed");

    for (const auto offset : {std::size_t{0}, std::size_t{8}, std::size_t{16},
                              std::size_t{24}, std::size_t{48}, std::size_t{56},
                              std::size_t{60}}) {
        auto corrupt = bytes;
        corrupt[offset] ^= std::byte{1};
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(minidb::decodeWalSegmentHeader(corrupt)); },
            "Corrupt segment header was accepted");
    }
}

void testRotationReopenAndLogicalContinuity() {
    minidb::test::TemporaryDatabase database("segmented_rotation");
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    constexpr std::uint32_t CAPACITY = 256;
    std::vector<minidb::Lsn> lsns;
    minidb::Lsn previous = minidb::INVALID_LSN;
    {
        minidb::LogManager log(legacy, 128, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        for (std::uint64_t index = 1; index <= 80; ++index) {
            previous = log.append(record(17 + index % 23, index, previous));
            lsns.push_back(previous);
            if (index % 7 == 0) log.flushUpTo(previous);
        }
        log.flushAll();
        log.validate();
        const auto stats = log.stats();
        require(stats.segmentRotations > 10 && stats.retainedSegments > 10
                    && stats.activeSegmentId == stats.retainedSegments,
                "Small segments did not rotate deterministically");
        const auto scan = log.scan();
        require(scan.records.size() == lsns.size(), "Segment scan lost records");
        for (std::size_t index = 1; index < scan.records.size(); ++index) {
            require(scan.records[index].lsn > scan.records[index - 1].lsn
                        && scan.records[index].prevLsn == scan.records[index - 1].lsn,
                    "Logical LSN/prevLSN chain broke across a segment boundary");
        }
        for (const auto& path : segmentFiles(log.segmentedPath())) {
            require(std::filesystem::file_size(path)
                        <= minidb::wal_segment_layout::HEADER_SIZE + CAPACITY,
                    "Segment exceeded its persisted capacity");
        }
    }
    {
        minidb::LogManager log(legacy, 128, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        const auto next = log.append(record(31, 1000, previous));
        require(next > lsns.back(), "Reopen rebased the global logical LSN");
        log.flushAll();
        require(log.scan().records.size() == lsns.size() + 1,
                "Reopened segmented WAL did not append exactly once");
    }
}

void testTailAndClosedSegmentPolicy() {
    minidb::test::TemporaryDatabase tailDatabase("segmented_tail");
    const auto tailLegacy = minidb::walPathForDatabase(tailDatabase.path().string());
    constexpr std::uint32_t CAPACITY = 256;
    {
        minidb::LogManager log(tailLegacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        const auto lsn = log.append(record(20, 1, minidb::INVALID_LSN));
        log.flushUpTo(lsn);
    }
    const std::array partial{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    auto files = segmentFiles(minidb::segmentedWalPathForLegacyWal(tailLegacy));
    appendRaw(files.back(), partial);
    {
        minidb::LogManager log(tailLegacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        require(log.hasTruncatedTail(), "Final segment short write was not detected");
        log.truncateToLastValidRecord();
        const auto lsn = log.append(record(21, 2, minidb::INVALID_LSN));
        log.flushUpTo(lsn);
    }

    minidb::test::TemporaryDatabase closedDatabase("segmented_closed_tail");
    const auto closedLegacy = minidb::walPathForDatabase(closedDatabase.path().string());
    {
        minidb::LogManager log(closedLegacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        minidb::Lsn previous = minidb::INVALID_LSN;
        for (std::uint64_t index = 1; index < 20; ++index) {
            previous = log.append(record(40, index, previous));
            log.flushUpTo(previous);
        }
    }
    files = segmentFiles(minidb::segmentedWalPathForLegacyWal(closedLegacy));
    require(files.size() > 1, "Closed-tail test did not rotate");
    appendRaw(files.front(), partial);
    minidb::test::requireThrows<minidb::WalError>(
        [&] { minidb::SegmentedWalStorage storage(
            minidb::segmentedWalPathForLegacyWal(closedLegacy), CAPACITY, false); },
        "Truncated closed segment was repaired instead of rejected");
}

void testReclamationAndLsnNonRebase() {
    minidb::test::TemporaryDatabase database("segmented_reclaim");
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    constexpr std::uint32_t CAPACITY = 256;
    minidb::Lsn floor = minidb::INVALID_LSN;
    minidb::Lsn maximum = minidb::INVALID_LSN;
    {
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        minidb::Lsn previous = minidb::INVALID_LSN;
        for (std::uint64_t index = 0; index < 60; ++index) {
            previous = log.append(record(33, index + 1, previous));
            if (index == 40) floor = previous;
            maximum = previous;
        }
        log.flushAll();
        const auto before = log.stats();
        log.rotateSegment();
        const auto reclaimed = log.reclaimSegmentsBefore(floor);
        const auto after = log.stats();
        require(reclaimed > 0 && after.segmentsDeleted > 0
                    && after.physicalWalBytes < before.physicalWalBytes
                    && !log.containsLsn(64),
                "Whole-segment reclamation did not remove obsolete history");
    }
    {
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        const auto next = log.append(record(9, 999, minidb::INVALID_LSN));
        require(next > maximum, "Reclamation/reopen rebased a logical LSN");
        log.flushAll();
    }
}

void testRetainedCheckpointFallbackAndControlRebuild() {
    minidb::test::TemporaryDatabase database("segmented_checkpoint_fallback");
    constexpr std::uint32_t CAPACITY = 256;
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    const auto controlPath = minidb::checkpointPathForDatabase(database.path().string());
    minidb::Lsn beginLsn = minidb::INVALID_LSN;
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        beginLsn = log.lastValidOffset();
        const auto begin = log.append(minidb::LogRecord{
            minidb::LogRecordType::CheckpointBegin, minidb::INVALID_TRANSACTION_ID,
            minidb::INVALID_LSN,
            minidb::encodeCheckpointBeginLogPayload({1, minidb::INVALID_LSN, beginLsn})});
        disk.sync();
        const auto endLsn = log.lastValidOffset();
        const auto recoveryStart = endLsn + minidb::wal_record_layout::HEADER_SIZE
            + minidb::checkpoint_end_log_layout::PAYLOAD_SIZE;
        static_cast<void>(log.append(minidb::LogRecord{
            minidb::LogRecordType::CheckpointEnd, minidb::INVALID_TRANSACTION_ID,
            minidb::INVALID_LSN,
            minidb::encodeCheckpointEndLogPayload({
                1, begin, disk.pageCount(), 1, recoveryStart})}));
        log.flushAll();
        log.rotateSegment();
        static_cast<void>(log.reclaimSegmentsBefore(beginLsn));
    }
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::DeferredRecovery,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        minidb::CheckpointControl control(controlPath);
        const auto recovered = minidb::RecoveryManager(disk, log, &control).recover();
        require(recovered.checkpointUsed && recovered.checkpointId == 1
                    && !recovered.fullScanFallback,
                "Lost control did not discover retained checkpoint WAL");
        require(control.select(log).slot.has_value(),
                "Retained checkpoint discovery did not rebuild control file");
    }
}

void testLegacyMigration() {
    minidb::test::TemporaryDatabase database("segmented_migration");
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    constexpr std::uint32_t CAPACITY = 256;
    std::vector<minidb::Lsn> original;
    minidb::Lsn previous = minidb::INVALID_LSN;
    {
        minidb::LogManager log(legacy, 64);
        for (std::uint64_t index = 1; index <= 20; ++index) {
            previous = log.append(record(12, index, previous));
            original.push_back(previous);
        }
        log.flushAll();
    }
    {
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::DeferredRecovery,
                               minidb::WalStorageMode::Auto, CAPACITY);
        require(log.legacyMigrationPending(), "Auto mode did not select legacy WAL");
        const auto scan = log.scan();
        log.completeRecoveryScan(scan);
        log.migrateLegacyToSegmented();
        require(log.isSegmented() && !std::filesystem::exists(legacy)
                    && std::filesystem::exists(log.segmentedPath()),
                "Legacy WAL migration did not publish/select segmented storage");
        const auto migrated = log.scan();
        require(migrated.records.size() == original.size(), "Migration lost WAL records");
        for (std::size_t index = 0; index < original.size(); ++index) {
            require(migrated.records[index].lsn == original[index],
                    "Migration changed a global logical LSN");
        }
    }
}

void testDiscoveryCorruptionAndFuzz() {
    minidb::test::TemporaryDatabase database("segmented_corruption");
    const auto legacy = minidb::walPathForDatabase(database.path().string());
    constexpr std::uint32_t CAPACITY = 256;
    {
        minidb::LogManager log(legacy, 64, minidb::LogOpenMode::EagerValidated,
                               minidb::WalStorageMode::Segmented, CAPACITY);
        minidb::Lsn previous = minidb::INVALID_LSN;
        for (std::uint64_t index = 1; index < 20; ++index) {
            previous = log.append(record(40, index, previous));
            log.flushUpTo(previous);
        }
    }
    auto files = segmentFiles(minidb::segmentedWalPathForLegacyWal(legacy));
    require(files.size() > 2, "Missing-segment test did not rotate enough");
    std::filesystem::remove(files[1]);
    minidb::test::requireThrows<minidb::WalError>(
        [&] { minidb::SegmentedWalStorage storage(
            minidb::segmentedWalPathForLegacyWal(legacy), CAPACITY, false); },
        "Missing required interior segment was accepted");

    constexpr std::uint32_t SEED = 0x11C20002U;
    constexpr std::size_t CASES = 5000;
    std::mt19937 random(SEED);
    std::size_t rejected = 0;
    for (std::size_t index = 0; index < CASES; ++index) {
        std::array<std::byte, 64> candidate{};
        for (auto& byte : candidate) byte = static_cast<std::byte>(random() & 0xFFU);
        try { static_cast<void>(minidb::decodeWalSegmentHeader(candidate)); }
        catch (const minidb::WalError&) { ++rejected; }
    }
    require(rejected == CASES, "Malformed segment-header fuzz corpus was accepted");
}

} // namespace

int main() {
    try {
        testExactCodecs();
        testRotationReopenAndLogicalContinuity();
        testTailAndClosedSegmentPolicy();
        testReclamationAndLsnNonRebase();
        testRetainedCheckpointFallbackAndControlRebuild();
        testLegacyMigration();
        testDiscoveryCorruptionAndFuzz();
        std::cout << "segmented_wal_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "segmented_wal_test failed: " << error.what() << '\n';
        return 1;
    }
}
