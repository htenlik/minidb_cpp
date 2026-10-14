#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <iostream>
#include <random>

namespace {
using namespace minidb;
using test::require;

void codec() {
    const auto bytes = encodePageReserveLogPayload({0x01020304, PageAllocationSource::Appended, 0x01020304});
    const std::vector<std::byte> exact{std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1},
        std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    require(bytes == exact && static_cast<unsigned>(LogRecordType::PageReserve) == 13, "Pinned codec/ID changed");
    require(decodePageReserveLogPayload(bytes).pageId == 0x01020304, "Codec roundtrip");
    for (const auto offset : {0U, 4U, 6U, 7U, 8U, 16U, 23U}) {
        auto bad = bytes; bad[offset] ^= std::byte{0x80};
        test::requireThrows<WalError>([&] { static_cast<void>(decodePageReserveLogPayload(bad)); }, "Bad payload accepted");
    }
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        test::requireThrows<WalError>([&] { static_cast<void>(decodePageReserveLogPayload(std::span(bytes).first(length))); }, "Truncation accepted");
    }
    auto trailing = bytes; trailing.push_back(std::byte{0});
    test::requireThrows<WalError>([&] { static_cast<void>(decodePageReserveLogPayload(trailing)); }, "Trailing byte accepted");
    for (const auto page : {PageId{0}, INVALID_PAGE_ID}) {
        test::requireThrows<WalError>([&] { static_cast<void>(encodePageReserveLogPayload({page, PageAllocationSource::Appended, page})); }, "Invalid page accepted");
    }
    LogRecord record{LogRecordType::PageReserve, 7, 64, bytes, 128};
    validateTransactionRecordPayload(record);
    const auto encoded = encodeWalRecord(record, 128);
    require(encoded.size() == 72 && decodeWalRecord(encoded, 128) == record, "Record codec roundtrip");
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        test::requireThrows<WalError>([&] { static_cast<void>(decodeWalRecord(std::span(encoded).first(length), 128)); }, "Truncated record accepted");
    }
    auto badLength = encoded; badLength[wal_record_layout::TOTAL_LENGTH_OFFSET] ^= std::byte{1};
    test::requireThrows<WalError>([&] { static_cast<void>(decodeWalRecord(badLength, 128)); }, "Bad length accepted");
    for (auto prev : {INVALID_LSN, Lsn{0}, Lsn{128}, Lsn{129}}) {
        auto bad = record; bad.prevLsn = prev;
        test::requireThrows<WalError>([&] { validateTransactionRecordPayload(bad); }, "Bad prevLSN accepted");
    }
    record.transactionId = 0;
    test::requireThrows<WalError>([&] { validateTransactionRecordPayload(record); }, "System reservation accepted");
}

void corruptChains() {
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        test::TemporaryDatabase db("reservation_bad_chain");
        DiskManager disk(db.path().string());
        LogManager log(walPathForDatabase(db.path().string()));
        Lsn last = log.append({LogRecordType::Begin, 1, INVALID_LSN, encodeBeginLogPayload({1})});
        if (scenario == 0) last = log.append({LogRecordType::Commit, 1, last, {}});
        if (scenario == 1) last = INVALID_LSN; // wrong transaction with no BEGIN
        const auto payload = encodePageReserveLogPayload({1, PageAllocationSource::Appended, 1});
        if (scenario == 2) {
            last = log.append({LogRecordType::PageReserve, 1, last, payload});
        }
        if (scenario == 3) {
            static_cast<void>(log.append({LogRecordType::Begin, 2, INVALID_LSN, encodeBeginLogPayload({1})}));
        }
        if (scenario == 10) {
            static_cast<void>(log.append({LogRecordType::PageReserve, 1, last, payload}));
            last = log.append({LogRecordType::Begin, 2, INVALID_LSN, encodeBeginLogPayload({1})});
        }
        if (scenario == 11) {
            last = log.append({LogRecordType::PageReserve, 1, last,
                encodePageReserveLogPayload({2, PageAllocationSource::Appended, 2})});
        }
        auto bad = payload;
        if (scenario == 4) bad[8] = std::byte{2};
        if (scenario == 5) bad[4] = std::byte{2};
        if (scenario == 6) bad[6] = std::byte{2};
        if (scenario == 7) bad.push_back(std::byte{0});
        if (scenario == 8) bad.pop_back();
        static_cast<void>(log.append({LogRecordType::PageReserve,
            scenario == 9 ? INVALID_TRANSACTION_ID : scenario == 1 || scenario == 3 || scenario == 10
                ? TransactionId{2} : TransactionId{1}, last, bad}));
        log.flushAll();
        test::requireThrows<WalError>([&] { static_cast<void>(RecoveryManager(disk, log).recover()); }, "Invalid reservation chain accepted");
    }
}

void ledgerAndMismatch() {
    test::TemporaryDatabase db("reservation_ledger");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log);
    BufferPoolManager pool(disk, 8, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    const auto id = recovery.beginTransaction(); recovery.beginMutation(id);
    for (unsigned i = 0; i < 3; ++i) {
        { auto page = pool.newPageWrite(); require(page->pageId() == i + 1, "Wrong appended ID"); }
        recovery.prepareTransaction(id);
        const auto snapshot = recovery.transactionSnapshot().front();
        require(snapshot.reservations.size() == i + 1, "Ledger did not survive statement boundary");
        for (const auto& [page, entry] : snapshot.reservations) {
            const auto record = log.readRecordAt(entry.reserveLsn);
            require(record.transactionId == id && record.type == LogRecordType::PageReserve
                && entry.expectedOldPageCount == page && entry.materializedInRuntime
                && log.durableLsn() >= entry.reserveLsn, "Ledger provenance/durability mismatch");
        }
    }
    recovery.commitTransaction(id);
    require(recovery.transactionSnapshot().empty() && recovery.stats().pageReservationsCommitted == 3, "Completion retained ledger");
    const auto stats = RecoveryManager(disk, log).recover();
    require(stats.pageReservationsRecovered == 3 && stats.pageReservationsCommitted == 3 && stats.pagesRedone == 0, "Reservation performed page REDO");
    // Atomically reject a stale expected ID: no second physical append occurs.
    const auto expected = disk.pageCount();
    static_cast<void>(disk.appendPage());
    test::requireThrows<std::logic_error>([&] { static_cast<void>(disk.appendPage(expected)); }, "Stale physical ID accepted");
    require(disk.pageCount() == expected + 1, "Mismatch appended a different page");

    recovery.beginStatement();
    const auto reserved = disk.pageCount();
    ::setenv("MINIDB_THROWPOINT", "append_after_reservation_append", 1);
    test::requireThrows<std::runtime_error>([&] { static_cast<void>(pool.newPageWrite()); }, "Missing reservation failure");
    ::unsetenv("MINIDB_THROWPOINT");
    require(disk.pageCount() == reserved, "Reservation failure extended file");
    { auto page = pool.newPageWrite(); require(page->pageId() == reserved, "Reservation retry mismatch"); }
    require(recovery.transactionSnapshot().front().reservations.size() == 1, "Retry duplicated reservation");
    recovery.rollbackStatement();
    require(disk.pageCount() == reserved && recovery.stats().pageReservationsAborted == 1, "Retry rollback mismatch");
}

void reservationRetention() {
    test::TemporaryDatabase db("reservation_retention");
    PageId meta = 0;
    {
        DiskManager disk(db.path().string());
        LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
            LogOpenMode::EagerValidated, WalStorageMode::Segmented, 512);
        RecoveryCoordinator recovery(disk, log);
        BufferPoolManager pool(disk, 4, 2, &log, &recovery); recovery.attachBufferPool(pool);
        CheckpointControl control(db.path().string() + ".ckpt");
        CheckpointManager checkpoints(recovery, pool, disk, log, control, {}, {0, 0, CheckpointMode::Fuzzy});
        DatabaseAccessGate gate; checkpoints.attachAccessGate(gate);
        auto lease = gate.acquireExclusive();
        recovery.beginStatement();
        { auto page = pool.newPageWrite(); meta = page->pageId(); }
        recovery.prepareStatement();
        const auto entry = recovery.transactionSnapshot().front();
        require(entry.lastLsn == entry.reservations.at(meta).reserveLsn, "No-update ATT lastLSN is not reservation");
        for (int i = 0; i < 5; ++i) static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
        require(log.oldestRetainedLsn() <= entry.beginLsn
            && log.readRecordAt(entry.lastLsn).type == LogRecordType::PageReserve, "Active reservation reclaimed");
    }
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
        LogOpenMode::DeferredRecovery, WalStorageMode::Segmented, 512);
    CheckpointControl control(db.path().string() + ".ckpt");
    RecoveryManager manager(disk, log, &control);
    const auto stats = manager.recover();
    require(stats.checkpointUsed && stats.pageReservationsRecovered == 1 && disk.pageCount() == 1
        && manager.recoveredTransactions().front().reservations.contains(meta), "Checkpoint reservation recovery failed");
}

void model() {
    constexpr std::uint32_t seeds[] = {0x12B3A2, 0xC0FFEE, 0xA110C, 0xDE1A};
    std::uint64_t events = 0;
    for (auto seed : seeds) {
        std::mt19937 rng(seed);
        for (unsigned history = 0; history < 50; ++history) {
            const auto diagnostic = " seed=" + std::to_string(seed) + " history=" + std::to_string(history);
            test::TemporaryDatabase db("reservation_model");
            std::map<PageId, PageReservationEntry> expected;
            const bool committed = history % 3 == 0;
            const bool liveAbort = history % 3 == 1;
            const bool unmaterializedLast = !committed && history % 2 == 0;
            PageId physicalCount = 2;
            std::byte expectedByte{0};
            const auto storage = history % 2 ? WalStorageMode::LegacySingleFile : WalStorageMode::Segmented;
            {
                DiskManager disk(db.path().string()); static_cast<void>(disk.appendPage());
                LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
                    LogOpenMode::EagerValidated, storage, 16384);
                RecoveryCoordinator recovery(disk, log, 1,
                    static_cast<WalUpdateMode>(history % 3));
                recovery.beginStatement();
                // Existing-page updates ensure CLR traversal crosses reservations.
                DiskManager::Page before{}; recovery.notePageWriteIntent(1, before);
                for (unsigned operation = 0; operation < 100; ++operation) {
                    const auto page = disk.pageCount();
                    recovery.prepareForPhysicalPageAppend(TransactionId{1}, page);
                    const auto snapshot = recovery.transactionSnapshot().front();
                    const auto entry = snapshot.reservations.at(page);
                    require(entry.expectedOldPageCount == page && entry.source == PageAllocationSource::Appended
                        && !entry.materializedInRuntime && log.durableLsn() >= entry.reserveLsn,
                        "Model reserve state" + diagnostic + " operation=" + std::to_string(operation));
                    require(log.readRecordAt(entry.reserveLsn).transactionId == 1, "Model owner" + diagnostic);
                    expected.emplace(page, entry); ++events;
                    if (operation == 99 && unmaterializedLast) break;
                    require(disk.appendPage(page) == page, "Model physical mismatch" + diagnostic);
                    recovery.notePhysicalPageAppend(page); ++physicalCount;
                    require(recovery.transactionSnapshot().front().reservations.at(page).materializedInRuntime,
                        "Model materialization missing" + diagnostic);
                    expectedByte = static_cast<std::byte>(1U + rng() % 255U);
                    before[80] = expectedByte;
                    static_cast<void>(recovery.preparePageForWrite(1, before));
                }
                if (committed) recovery.commitStatement();
                else if (liveAbort) { recovery.rollbackStatement(); physicalCount = 2; }
                else log.flushAll(); // Persist a loser history; real hard crashes are tested separately.
                require(disk.pageCount() == physicalCount, "Model before restart extent" + diagnostic);
            }
            DiskManager disk(db.path().string());
            LogManager log(walPathForDatabase(db.path().string()), LogManager::DEFAULT_BUFFER_SIZE,
                LogOpenMode::DeferredRecovery, storage, 16384);
            RecoveryManager manager(disk, log);
            const auto result = manager.recover();
            require(manager.analysisTransactions().size() == 1, "Model analysis missing" + diagnostic);
            const auto& recovered = manager.recoveredTransactions().front();
            require(recovered.transactionId == 1 && recovered.reservations == expected,
                "Model durable ledger differs" + diagnostic);
            require(recovered.status == (committed ? RecoveryTransactionStatus::Committed : RecoveryTransactionStatus::Aborted)
                && disk.pageCount() == (committed ? physicalCount : 2U)
                && result.pageReservationsRecovered == 100, "Model terminal/extent mismatch" + diagnostic);
            const auto second = RecoveryManager(disk, log).recover();
            require(second.loserTransactions == 0, "Model repeated restart" + diagnostic);
            DiskManager::Page contents{}; disk.readPage(1, contents);
            require(contents[80] == (committed ? expectedByte : std::byte{0}), "Model page contents" + diagnostic);
        }
    }
    require(events == 20000, "Model event count");
    std::cout << "reservation_model events=" << events << " seeds=0x12B3A2,0xC0FFEE,0xA110C,0xDE1A histories=200\n";
}
} // namespace
int main() {
    try { codec(); corruptChains(); ledgerAndMismatch(); reservationRetention(); model(); return 0; }
    catch (const std::exception& error) {
        ::unsetenv("MINIDB_THROWPOINT"); std::cerr << error.what() << '\n'; return 1;
    }
}
