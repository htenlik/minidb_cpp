#pragma once

#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_log.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/recovery.hpp"
#include "minidb/slotted_page.hpp"
#include "test_utils.hpp"

#include <random>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace minidb::test {

// Real codecs and disk pages, not an in-memory recovery simulation. Every
// simultaneously active transaction owns a distinct preallocated page.
struct MultiHistory {
    TemporaryDatabase database{"multi_recovery"};
    std::vector<DiskManager::Page> original;
    std::vector<DiskManager::Page> final;
    std::vector<Lsn> begins;
    std::vector<Lsn> lasts;
    std::vector<std::vector<Lsn>> updates;
    std::vector<RecoveryTransactionStatus> statuses;
    bool segmented;
    std::uint64_t seed;

    explicit MultiHistory(std::size_t transactions, std::uint64_t rngSeed, bool segments = true)
        : original(transactions), final(transactions), begins(transactions), lasts(transactions),
          updates(transactions), statuses(transactions, RecoveryTransactionStatus::Active),
          segmented(segments), seed(rngSeed) {}

    std::string path() const { return database.path().string(); }
    LogManager openLog(LogOpenMode mode = LogOpenMode::EagerValidated) const {
        return LogManager(walPathForDatabase(path()), LogManager::DEFAULT_BUFFER_SIZE,
                          mode, segmented ? WalStorageMode::Segmented : WalStorageMode::LegacySingleFile,
                          9000);
    }

    void create(std::size_t updateCount, bool mixed = false, bool checkpoint = false) {
        DiskManager disk(path());
        const auto metadata = disk.appendPage();
        for (std::size_t i = 0; i < original.size(); ++i) {
            const auto page = disk.appendPage();
            SlottedPageView::initialize(original[i], page, page + 1, metadata);
            final[i] = original[i];
            disk.writePage(page, original[i]);
        }
        disk.sync();
        auto log = openLog();
        for (std::size_t i = 0; i < original.size(); ++i) {
            begins[i] = lasts[i] = log.append({LogRecordType::Begin, i + 1, INVALID_LSN,
                encodeBeginLogPayload({disk.pageCount()})});
        }
        std::mt19937_64 random(seed);
        for (std::size_t step = 0; step < updateCount; ++step) {
            const auto i = step < original.size() ? step : random() % original.size();
            byte_codec::writeUint64(final[i], 80, random());
            PageDeltaUpdateV2LogPayload payload;
            payload.pageId = static_cast<PageId>(i + 2);
            payload.beforePageExisted = true;
            payload.beforePageLsn = INVALID_LSN;
            std::array<bool, database_format::PAGE_SIZE> touched{};
            std::fill(touched.begin() + 80, touched.begin() + 88, true);
            payload.ranges = computePageDelta(original[i], final[i], touched);
            lasts[i] = log.append({LogRecordType::PageDeltaUpdateV2, i + 1, lasts[i],
                                  encodePageDeltaUpdateV2LogPayload(payload)});
            updates[i].push_back(lasts[i]);
            if (step % 7 == 0) {
                log.flushUpTo(lasts[i]); // optional STEAL after WAL force
                auto stolen = final[i];
                writePersistentPageLsn(stolen, lasts[i]);
                disk.writePhysicalPage(static_cast<PageId>(i + 2), stolen);
            }
        }
        if (checkpoint) {
            const auto begin = log.append({LogRecordType::FuzzyCheckpointBegin, 0, INVALID_LSN,
                                           encodeFuzzyCheckpointBeginLogPayload({1, INVALID_LSN})});
            std::vector<CheckpointTransactionEntry> att;
            std::vector<DirtyPageEntry> dpt;
            for (std::size_t i = 0; i < original.size(); ++i) {
                att.push_back({i + 1, CheckpointTransactionStatus::Active, begins[i], lasts[i], disk.pageCount()});
                if (!updates[i].empty()) dpt.push_back({static_cast<PageId>(i + 2), updates[i].front()});
            }
            const auto end = log.append({LogRecordType::FuzzyCheckpointEnd, 0, INVALID_LSN,
                encodeFuzzyCheckpointEndLogPayload({1, begin, disk.pageCount(), original.size() + 1, dpt, att})});
            log.flushUpTo(end);
            CheckpointControl control(path() + ".ckpt");
            control.publish({1, 1, end, begin, disk.pageCount(), original.size() + 1, log.lastValidOffset()});
        }
        log.flushAll();
        if (mixed) {
            for (std::size_t i = 0; i < original.size(); ++i) {
                if (i % 4 < 2) {
                    lasts[i] = log.append({LogRecordType::Commit, i + 1, lasts[i], {}});
                    statuses[i] = RecoveryTransactionStatus::Committed;
                } else if (i % 4 == 2) {
                    for (auto update = updates[i].rbegin(); update != updates[i].rend(); ++update) {
                        const auto target = log.readRecordAt(*update);
                        lasts[i] = log.append({LogRecordType::Compensation, i + 1, lasts[i],
                            encodeCompensationLogPayload({static_cast<PageId>(i + 2), true, true,
                                target.prevLsn, target.lsn, original[i]})});
                    }
                    lasts[i] = log.append({LogRecordType::Abort, i + 1, lasts[i], {}});
                    statuses[i] = RecoveryTransactionStatus::Aborted;
                }
            }
        }
        log.flushAll();
        disk.sync();
    }

    RecoveryStats recoverAndCheck(bool useCheckpoint = false) {
        DiskManager disk(path());
        auto log = openLog(LogOpenMode::DeferredRecovery);
        CheckpointControl control(path() + ".ckpt");
        RecoveryManager manager(disk, log, useCheckpoint ? &control : nullptr);
        const auto stats = manager.recover();
        require(stats.analysisTransactions == original.size(), "ATT size mismatch seed=" + std::to_string(seed));
        require(stats.nextTransactionId == original.size() + 1, "Incorrect recovered next transaction ID");
        for (std::size_t i = 0; i < original.size(); ++i) {
            DiskManager::Page page{};
            disk.readPhysicalPage(static_cast<PageId>(i + 2), page);
            const auto lsn = readPersistentPageLsn(page);
            require(isValidLsn(lsn), "Missing recovered PageLSN");
            const auto record = log.readRecordAt(lsn);
            require(record.transactionId == i + 1, "PageLSN crosses transaction ownership");
            clearPersistentPageLsn(page);
            require(page == (statuses[i] == RecoveryTransactionStatus::Committed ? final[i] : original[i]),
                    "Wrong recovered bytes seed=" + std::to_string(seed) + " tx=" + std::to_string(i + 1));
            const auto& entry = manager.recoveredTransactions()[i];
            require(entry.transactionId == i + 1
                    && entry.status == (statuses[i] == RecoveryTransactionStatus::Committed
                        ? RecoveryTransactionStatus::Committed : RecoveryTransactionStatus::Aborted),
                    "Incorrect terminal transaction status");
            require(log.readRecordAt(entry.lastLsn).type == (entry.hasDurableCommit
                    ? LogRecordType::Commit : LogRecordType::Abort), "Incorrect terminal lastLSN");
        }
        const auto again = RecoveryManager(disk, log, useCheckpoint ? &control : nullptr).recover();
        require(again.loserTransactions == 0 && again.clrsAppended == 0, "Recovery is not idempotent");
        return stats;
    }

    void crashUndo(std::size_t clrs, const char* failpoint = nullptr) {
        const auto child = ::fork();
        require(child >= 0, "fork failed");
        if (child == 0) {
            if (failpoint) ::setenv("MINIDB_FAILPOINT", failpoint, 1);
            else ::setenv("MINIDB_RECOVERY_CRASH_AFTER_CLRS", std::to_string(clrs).c_str(), 1);
            DiskManager disk(path());
            auto log = openLog(LogOpenMode::DeferredRecovery);
            static_cast<void>(RecoveryManager(disk, log).recover());
            ::_exit(92);
        }
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
                "Recovery did not hit crash point");
    }
};
} // namespace minidb::test
