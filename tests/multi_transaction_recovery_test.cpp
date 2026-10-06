#include "multi_recovery_fixture.hpp"
#include "minidb/database_server.hpp"

#include <iostream>

namespace {
using namespace minidb;
using test::require;

void model() {
    std::uint64_t operations = 0;
    for (const auto seed : {0x12B201ULL, 0xC0FFEEULL, 0xA11CEULL, 0xDE1AULL}) {
        for (std::size_t history = 0; history < 8; ++history) {
            test::MultiHistory fixture(16, seed + history, history % 2 == 0);
            try {
                fixture.create(1600, true, history % 2 == 0);
                const auto stats = fixture.recoverAndCheck(history % 2 == 0);
                require(stats.analysisWinners == 8 && stats.analysisAborted == 4
                        && stats.analysisLosers == 4 && stats.transactionsCompletedDuringUndo == 4,
                        "Interleaved classification mismatch");
            } catch (const std::exception& error) {
                throw std::runtime_error("Model seed=" + std::to_string(fixture.seed)
                    + " history=" + std::to_string(history)
                    + " operation=" + std::to_string(fixture.operationIndex) + ": " + error.what());
            }
            operations += 1600;
        }
    }
    std::cout << "real-WAL model updates=" << operations << " seeds=4 histories=32\n";
}

void loserCountsAndRestart() {
    for (const auto count : {1U, 2U, 4U, 8U, 16U}) {
        test::MultiHistory fixture(count, 1200 + count);
        fixture.create(count * 8);
        const auto stats = fixture.recoverAndCheck();
        require(stats.loserTransactions == count && stats.clrsAppended == count * 8
                && stats.undoQueuePeak == count, "Incorrect multi-loser queue behavior");
    }
    test::MultiHistory fixture(8, 2244);
    fixture.create(128);
    fixture.crashUndo(9);
    fixture.crashUndo(13);
    fixture.crashUndo(0, "recovery_after_abort_sync"); // one finished, others still live
    const auto stats = fixture.recoverAndCheck();
    require(stats.analysisAborted >= 1 && stats.undoRestartCount >= 1,
            "Restart did not distinguish completed and partially compensated losers");
    auto log = fixture.openLog();
    std::set<Lsn> compensated;
    for (const auto& record : log.scan().records) {
        if (record.type == LogRecordType::Compensation) {
            require(compensated.insert(decodeCompensationLogPayload(record.payload).compensatedUpdateLsn).second,
                    "Restart repeated already compensated update");
        }
    }
    require(compensated.size() == 128, "Not all loser updates compensated exactly once");
}

void corruptChains() {
    for (int corruption = 0; corruption < 7; ++corruption) {
        test::TemporaryDatabase db("multi_chain_corrupt");
        DiskManager disk(db.path().string());
        static_cast<void>(disk.appendPage());
        LogManager log(walPathForDatabase(db.path().string()));
        const auto begin1 = log.append({LogRecordType::Begin, 1, INVALID_LSN, encodeBeginLogPayload({2})});
        const auto begin2 = log.append({LogRecordType::Begin, 2, INVALID_LSN, encodeBeginLogPayload({2})});
        DiskManager::Page before{}, after{};
        after[80] = std::byte{8};
        const auto update = log.append({LogRecordType::PageUpdate, 1, begin1,
            encodePageUpdateLogPayload({1, true, before, after})});
        const auto next = log.lastValidOffset();
        const auto badPrevious = corruption == 0 ? begin2 : corruption == 1 ? next
            : corruption == 2 ? begin1 + 1 : corruption == 3 ? next + 1000 : update;
        if (corruption == 1 || corruption == 3) {
            test::requireThrows<WalError>([&] {
                static_cast<void>(log.append({LogRecordType::Commit, 1, badPrevious, {}}));
            }, "WAL encoder accepted forward/self-cycle prevLSN");
            continue;
        }
        if (corruption == 5) {
            test::requireThrows<WalError>([&] {
                static_cast<void>(encodeCompensationLogPayload({1, true, false, update, update, before}));
            }, "CLR encoder accepted self-cycle undoNextLSN");
            continue;
        }
        if (corruption < 4) {
            static_cast<void>(log.append({LogRecordType::Commit, 1, badPrevious, {}}));
        } else {
            const auto badUndo = corruption == 4 ? begin2 : corruption == 5 ? update : begin1 + 1;
            static_cast<void>(log.append({LogRecordType::Compensation, 1, update,
                encodeCompensationLogPayload({1, true, false, badUndo, update, before})}));
        }
        log.flushAll();
        test::requireThrows<WalError>([&] { static_cast<void>(RecoveryManager(disk, log).recover()); },
                                     "Invalid interleaved chain accepted");
    }
}

void keyedRuntimeAndRetention() {
    test::TemporaryDatabase db("live_att_retention");
    DiskManager disk(db.path().string());
    const auto p1 = disk.appendPage(), p2 = disk.appendPage(), p3 = disk.appendPage();
    LogManager log(walPathForDatabase(db.path().string()), 4096, LogOpenMode::EagerValidated,
                   WalStorageMode::Segmented, 9000);
    RecoveryCoordinator recovery(disk, log, 1, WalUpdateMode::ByteRange);
    BufferPoolManager pool(disk, 4, 2, &log, &recovery);
    recovery.attachBufferPool(pool);
    CheckpointControl control(db.path().string() + ".ckpt");
    CheckpointManager checkpoints(recovery, pool, disk, log, control, {}, {0, 0, CheckpointMode::Fuzzy});
    DatabaseAccessGate gate;
    checkpoints.attachAccessGate(gate);
    auto lease = gate.acquireExclusive();
    std::vector<TransactionId> ids;
    for (const auto page : {p1, p2, p3}) {
        const auto id = recovery.beginTransaction();
        ids.push_back(id);
        recovery.beginMutation(id);
        {
            auto guard = pool.fetchPageWrite(page);
            guard->data()[80] = std::byte{1};
        }
        test::requireThrows<std::logic_error>([&] {
            static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
        }, "Checkpoint captured unfinished mutation");
        recovery.prepareTransaction(id);
        static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
    }
    const auto oldest = recovery.checkpointTransactions().front().beginLsn;
    pool.flushAll(); // DPT no longer retains the original BEGIN, ATT must do it.
    for (int i = 0; i < 12; ++i) {
        const auto unrelated = recovery.beginTransaction();
        recovery.commitTransaction(unrelated); // unlogged context, excluded from ATT
        static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
        require(log.oldestRetainedLsn() <= oldest && checkpoints.stats().retentionFloorLsn == oldest,
                "Active BEGIN segment was reclaimed");
    }
    require(recovery.checkpointTransactions().size() == 3, "Runtime table lost concurrent contexts");
    recovery.commitTransaction(ids[0]);
    static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
    require(checkpoints.stats().retentionFloorLsn == recovery.checkpointTransactions().front().beginLsn,
            "Retention did not advance to next active chain");
    recovery.rollbackTransaction(ids[1]);
    recovery.commitTransaction(ids[2]);
    static_cast<void>(checkpoints.checkpoint(CheckpointMode::Fuzzy, lease));
    require(checkpoints.stats().retentionFloorLsn > oldest && recovery.checkpointTransactions().empty(),
            "Completion did not release ATT retention constraint");
}

void corruptAttAndIds() {
    for (int corruption = 0; corruption < 4; ++corruption) {
        test::MultiHistory history(2, 88);
        history.create(16);
        DiskManager disk(history.path());
        auto log = history.openLog();
        const auto begin = log.append({LogRecordType::FuzzyCheckpointBegin, 0, INVALID_LSN,
                                      encodeFuzzyCheckpointBeginLogPayload({1, INVALID_LSN})});
        auto firstBegin = history.begins[0];
        auto firstLast = history.lasts[0];
        if (corruption == 0) ++firstBegin; // not a record boundary
        if (corruption == 1) firstLast = history.lasts[1]; // wrong transaction
        if (corruption == 2) firstLast = history.begins[0]; // stale checkpoint chain endpoint
        const auto nextId = corruption == 3 ? TransactionId{2} : TransactionId{3};
        const auto end = log.append({LogRecordType::FuzzyCheckpointEnd, 0, INVALID_LSN,
            encodeFuzzyCheckpointEndLogPayload({1, begin, disk.pageCount(), nextId, {}, {
                {1, CheckpointTransactionStatus::Active, firstBegin, firstLast, disk.pageCount()},
                {2, CheckpointTransactionStatus::Active, history.begins[1], history.lasts[1], disk.pageCount()},
            }})});
        log.flushAll();
        CheckpointControl control(history.path() + ".ckpt");
        control.publish({1, 1, end, begin, disk.pageCount(), nextId, log.lastValidOffset()});
        test::requireThrows<WalError>([&] { static_cast<void>(RecoveryManager(disk, log, &control).recover()); },
                                     "Invalid checkpoint ATT history accepted");
    }
    test::TemporaryDatabase db("recovery_tx_overflow");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryCoordinator recovery(disk, log, std::numeric_limits<TransactionId>::max());
    test::requireThrows<std::overflow_error>([&] { static_cast<void>(recovery.beginTransaction()); },
                                           "Overflowed runtime transaction allocator accepted");
    static_cast<void>(log.append({LogRecordType::Begin, 0, INVALID_LSN, encodeBeginLogPayload({1})}));
    log.flushAll();
    test::requireThrows<WalError>([&] { static_cast<void>(RecoveryManager(disk, log).recover()); },
                                  "Zero user transaction ID accepted by recovery");
}

void partialLosersAfterDurableAbort() {
    test::TemporaryDatabase db("multi_abort_partial");
    {
        DiskManager disk(db.path().string());
        for (int i = 0; i < 4; ++i) static_cast<void>(disk.appendPage());
        LogManager log(walPathForDatabase(db.path().string()));
        DiskManager::Page before{}, after{};
        after[80] = std::byte{42};
        std::vector<Lsn> previous(5), begins(5);
        for (TransactionId id = 1; id <= 3; ++id) {
            begins[id] = previous[id] = log.append({LogRecordType::Begin, id, INVALID_LSN, encodeBeginLogPayload({5})});
            previous[id] = log.append({LogRecordType::PageUpdate, id, previous[id],
                encodePageUpdateLogPayload({static_cast<PageId>(id), true, before, after})});
        }
        const auto second = log.append({LogRecordType::PageUpdate, 2, previous[2],
            encodePageUpdateLogPayload({2, true, before, after})});
        previous[2] = log.append({LogRecordType::Compensation, 2, second,
            encodeCompensationLogPayload({2, true, false, previous[2], second, before})});
        begins[4] = previous[4] = log.append({LogRecordType::Begin, 4, INVALID_LSN, encodeBeginLogPayload({5})});
        previous[4] = log.append({LogRecordType::PageUpdate, 4, previous[4],
            encodePageUpdateLogPayload({4, true, before, after})});
        log.flushAll();
    }
    const auto child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::setenv("MINIDB_FAILPOINT", "recovery_after_abort_sync", 1);
        DiskManager disk(db.path().string());
        LogManager log(walPathForDatabase(db.path().string()));
        static_cast<void>(RecoveryManager(disk, log).recover());
        ::_exit(92);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Partial completion crash missed");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    RecoveryManager recovery(disk, log);
    const auto stats = recovery.recover();
    require(stats.analysisAborted == 1 && stats.analysisAbortingTransactions == 1
            && stats.analysisActiveTransactions == 2 && stats.clrsAppended == 3,
            "Durable ABORT/partial/untouched loser classification failed");
    for (PageId pageId = 1; pageId <= 4; ++pageId) {
        DiskManager::Page page{};
        disk.readPage(pageId, page);
        require(page[80] == std::byte{0}, "Partial multi-loser restart lost original page");
    }
}

void unsupportedPhysicalHistories() {
    for (const bool overlappingAllocation : {false, true}) {
        test::TemporaryDatabase db("multi_physical_boundary");
        DiskManager disk(db.path().string());
        static_cast<void>(disk.appendPage());
        LogManager log(walPathForDatabase(db.path().string()));
        DiskManager::Page before{}, after{};
        after[80] = std::byte{1};
        const auto t1 = log.append({LogRecordType::Begin, 1, INVALID_LSN, encodeBeginLogPayload({2})});
        static_cast<void>(log.append({LogRecordType::PageUpdate, 1, t1,
            encodePageUpdateLogPayload({1, true, before, after})}));
        if (overlappingAllocation) static_cast<void>(disk.appendPage());
        const auto t2 = log.append({LogRecordType::Begin, 2, INVALID_LSN,
                                   encodeBeginLogPayload({disk.pageCount()})});
        static_cast<void>(log.append({LogRecordType::PageUpdate, 2, t2,
            encodePageUpdateLogPayload({overlappingAllocation ? PageId{2} : PageId{1}, true, before, after})}));
        log.flushAll();
        const auto walEnd = log.lastValidOffset();
        test::requireThrows<WalError>([&] { static_cast<void>(RecoveryManager(disk, log).recover()); },
                                     "Unsupported conflicting/allocation history accepted");
        require(log.lastValidOffset() == walEnd, "Rejected history appended compensation");
    }
}

void historicalWinnerAllocation() {
    test::TemporaryDatabase db("multi_prior_allocation");
    DiskManager disk(db.path().string());
    LogManager log(walPathForDatabase(db.path().string()));
    DiskManager::Page before{}, after{};
    after[80] = std::byte{42};
    const auto first = log.append({LogRecordType::Begin, 1, INVALID_LSN, encodeBeginLogPayload({1})});
    const auto winnerPage = disk.appendPage();
    const auto update = log.append({LogRecordType::PageUpdate, 1, first,
                                  encodePageUpdateLogPayload({winnerPage, false, before, after})});
    static_cast<void>(log.append({LogRecordType::Commit, 1, update, {}}));
    const auto p2 = disk.appendPage(), p3 = disk.appendPage();
    for (TransactionId id = 2; id <= 3; ++id) {
        const auto begin = log.append({LogRecordType::Begin, id, INVALID_LSN,
                                      encodeBeginLogPayload({disk.pageCount()})});
        static_cast<void>(log.append({LogRecordType::PageUpdate, id, begin,
            encodePageUpdateLogPayload({id == 2 ? p2 : p3, true, before, after})}));
    }
    log.flushAll();
    const auto stats = RecoveryManager(disk, log).recover();
    require(stats.analysisWinners == 1 && stats.analysisLosers == 2 && stats.pagesTruncated == 0,
            "Prior completed allocation was mistaken for overlapping loser allocation");
    DiskManager::Page page{};
    disk.readPage(winnerPage, page);
    require(page[80] == std::byte{42}, "Prior allocating winner was lost");
}

void sqlLiveAtt() {
    for (const auto terminal : {0, 1, 2}) {
        for (const bool loseControl : {false, true}) {
            test::TemporaryDatabase db("sql_live_att");
            net::ServerConfig config;
            config.port = 0; config.bufferFrames = 32;
            config.checkpointWalBytes = 0; config.checkpointStatements = 0;
            config.checkpointMode = CheckpointMode::Fuzzy; config.walSegmentBytes = 9000;
            config.walUpdateMode = WalUpdateMode::Adaptive;
            {
                net::DatabaseServer server(db.path().string(), config);
                static_cast<void>(server.sqlEngine().execute("CREATE TABLE t (id UINT32 PRIMARY KEY, value VARCHAR(32))"));
                static_cast<void>(server.sqlEngine().execute("INSERT INTO t VALUES (1, 'before')"));
                static_cast<void>(server.checkpointManager().checkpoint(CheckpointMode::Sharp));
            }
            const auto child = ::fork();
            require(child >= 0, "fork failed");
            if (child == 0) {
                net::DatabaseServer server(db.path().string(), config);
                auto& sql = server.sqlEngine();
                static_cast<void>(sql.execute("BEGIN"));
                static_cast<void>(sql.execute("UPDATE t SET value = 'during' WHERE id = 1"));
                static_cast<void>(sql.execute("UPDATE t SET value = 'checkpoint' WHERE id = 1"));
                static_cast<void>(sql.transactionManager().checkpoint(LOCAL_SESSION_ID, CheckpointMode::Fuzzy));
                const auto selection = server.checkpointControl().select(server.logManager());
                const auto att = decodeFuzzyCheckpointEndLogPayload(
                    server.logManager().readRecordAt(selection.slot->checkpointEndLsn).payload).activeTransactions;
                require(att.size() == 1, "SQL checkpoint omitted active writer");
                static_cast<void>(sql.execute("UPDATE t SET value = 'after' WHERE id = 1"));
                if (terminal == 1) static_cast<void>(sql.execute("COMMIT"));
                if (terminal == 2) static_cast<void>(sql.execute("ROLLBACK"));
                server.logManager().flushAll();
                ::_exit(91);
            }
            int status = 0;
            require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 91,
                    "SQL ATT crash setup failed");
            if (loseControl) std::filesystem::remove(db.path().string() + ".ckpt");
            net::DatabaseServer reopened(db.path().string(), config);
            const auto rows = std::get<sql::SelectResult>(reopened.sqlEngine().execute("SELECT * FROM t")).rows;
            require(rows.size() == 1 && std::get<std::string>(rows[0][1]) == (terminal == 1 ? "after" : "before"),
                    "SQL ATT winner/loser/abort recovered wrong row");
            require(reopened.startupRecoveryStats().checkpointActiveTransactionCount == 1,
                    "Recovery did not seed live checkpoint ATT");
        }
    }
}
} // namespace

int main() {
    try {
        loserCountsAndRestart(); corruptChains(); corruptAttAndIds(); partialLosersAfterDurableAbort();
        unsupportedPhysicalHistories();
        historicalWinnerAllocation();
        keyedRuntimeAndRetention(); sqlLiveAtt(); model();
        std::cout << "multi_transaction_recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multi_transaction_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
