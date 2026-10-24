#include "minidb/owned_rollback_cleanup.hpp"

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_lsn.hpp"

#include <algorithm>

namespace minidb {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw WalError(WalErrorKind::CorruptRecord, message);
}
std::span<const LogRecord> fromBegin(const std::vector<LogRecord>& records, Lsn begin) {
    const auto it = std::find_if(records.begin(), records.end(), [&](const auto& r) { return r.lsn == begin; });
    require(it != records.end() && it->type == LogRecordType::BeginOwned, "Owned cleanup lost its retained BEGIN");
    return std::span(records).subspan(static_cast<std::size_t>(it - records.begin()));
}
bool matchesOwnedImage(const std::function<bool(const DiskManager::Page&)>& matches,
                      PageId page, TransactionId owner,Lsn reserve,std::span<const LogRecord> history) {
    // Canonical append result plus the checked, exclusive append protocol is
    // evidence independent of the volatile confirmation bit. A nonzero image
    // must match a full reconstructed, same-owner logged state byte-for-byte.
    if (matches(DiskManager::Page{})) return true;
    for (const auto& r : history) {
        if (r.transactionId != owner || r.lsn <= reserve) continue;
        DiskManager::Page after{};
        PageId target = INVALID_PAGE_ID;
        bool existed = true, v2 = false;
        if (r.type == LogRecordType::PageUpdate) {
            const auto v = decodePageUpdateLogPayload(r.payload);
            target=v.pageId; existed=v.beforePageExisted; after=v.afterImage;
        } else if (r.type == LogRecordType::PageDeltaUpdate) {
            const auto v=decodePageDeltaUpdateLogPayload(r.payload);
            target=v.pageId; existed=v.beforePageExisted; applyPageDeltaAfter(after,v);
        } else if (r.type == LogRecordType::PageUpdateV2) {
            const auto v=decodePageUpdateV2LogPayload(r.payload);
            target=v.pageId; existed=v.beforePageExisted; after=v.afterImage; v2=true;
        } else if (r.type == LogRecordType::PageDeltaUpdateV2) {
            const auto v=decodePageDeltaUpdateV2LogPayload(r.payload);
            target=v.pageId; existed=v.beforePageExisted;
            applyPageDeltaAfter(after,{v.pageId,v.beforePageExisted,v.ranges}); v2=true;
        } else continue;
        if (target != page || existed) continue;
        if (v2) {
            require(supportsPersistentPageLsn(after), "Owned V2 after-image lacks its declared PageLSN slot");
            writePersistentPageLsn(after,r.lsn);
        }
        if (matches(after)) return true;
    }
    return false;
}
std::vector<PageId> freePages(DiskManager& disk) {
    BufferPoolManager pool(disk,2);
    return PageAllocator(pool,disk).freePageIds();
}
void validateExtentProvenance(const OwnedRollbackAnalysis& owner,
    const std::map<TransactionId,OwnedRollbackAnalysis>& transactions, std::uint64_t extent) {
    std::map<PageId,TransactionId> accounted;
    for (const auto& [id,t] : transactions) for (const auto& [page,entry] : t.reservations) {
        if (entry.reservation.reserveLsn>=owner.beginLsn
            && (id==owner.transactionId || t.phase==OwnedCleanupPhase::CommitRecorded)) {
            require(accounted.emplace(page,id).second,"Conflicting append provenance in physical extent");
        }
    }
    // Coverage is not ownership: each target still needs its exact owner chain
    // and physical-image proof. This rejects raw/unaccounted suffix extension.
    for (std::uint64_t page=owner.startPageCount;page<extent;++page)
        require(accounted.contains(static_cast<PageId>(page)),"Unaccounted physical extension during owned transaction");
}
DiskManager::Page proveMaterialized(DiskManager& disk, const OwnedRollbackAnalysis& owner,
    const OwnedReservationEvidence& evidence, std::span<const LogRecord> history) {
    const auto& r=evidence.reservation;
    require(owner.protocol == RollbackProtocol::OwnedAppend && owner.logicalUndoChainComplete
        && r.source == PageAllocationSource::Appended && r.expectedOldPageCount == r.pageId
        && r.pageId >= owner.startPageCount && r.pageId < disk.pageCount(), "Missing owned materialization provenance");
    require(disk.databaseHeader().catalogRootPageId != r.pageId, "Owned cleanup targets live catalog root");
    const auto members=freePages(disk);
    require(std::find(members.begin(),members.end(),r.pageId)==members.end(), "Owned cleanup target is already globally free");
    DiskManager::Page physical{};disk.readPhysicalPage(r.pageId,physical);
    require(matchesOwnedImage([&](const auto& image){return physical==image;},r.pageId,owner.transactionId,r.reserveLsn,history),
            "Unlogged reuse or ambiguous physical reservation image");
    return physical;
}
void validateCompleted(DiskManager& disk, const OwnedReservationEvidence& entry, LogManager& log) {
    if (entry.outcome == ReservationCleanupOutcome::AbsenceRecorded) {
        const auto progress=decodeCleanupProgress(log.readRecordAt(entry.progressLsn));
        require(disk.pageCount()==progress.observedExtent && entry.reservation.pageId>=disk.pageCount(),
                "Recorded absence contradicts physical extent");
        return;
    }
    const auto record=log.readRecordAt(entry.reclaimLsn);
    const auto payload=decodePageReclaim(record);
    DiskManager::Page physical{};disk.readPhysicalPage(payload.page,physical);
    const auto members=freePages(disk);
    require(physical==reclaimedPage(payload,entry.reclaimLsn)
        && std::count(members.begin(),members.end(),payload.page)==1,
        "Owned reclaim lacks exactly-once canonical free membership");
}
}

void OwnedRollbackCleanup::requireAuthority(const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease) {
    if (gate==nullptr || lease==nullptr || !lease->owns(*gate) || lease->mode()!=AccessMode::ReadWrite)
        throw std::logic_error("Owned cleanup requires a bound exclusive lifecycle lease");
}

void OwnedRollbackCleanup::validateReclaimEvidence(DiskManager& disk,const LogRecord& record,
    const OwnedRollbackAnalysis& owner,std::span<const LogRecord> history) {
    const auto payload=decodePageReclaim(record);
    const auto reservation=owner.reservations.find(payload.page);
    require(owner.logicalUndoChainComplete && reservation!=owner.reservations.end()
        && reservation->second.reclaimLsn==record.lsn,"Reclaim lacks completed-UNDO/reservation lineage");
    const auto reserve=reservation->second.reservation.reserveLsn;
    require(matchesOwnedImage([&](const auto& image){return readPersistentPageLsn(image)==payload.beforePageLsn
        && crc32c(image)==payload.beforeCrc;},payload.page,owner.transactionId,reserve,history),
        "Reclaim fingerprint is not a known owner append state");
    DiskManager::Page physical{};disk.readPhysicalPage(payload.page,physical);
    if (readPersistentPageLsn(physical)==payload.beforePageLsn && crc32c(physical)==payload.beforeCrc) {
        // When the preimage still exists, fingerprint equality is not enough.
        require(matchesOwnedImage([&](const auto& image){return image==physical;},payload.page,owner.transactionId,reserve,history),
            "Reclaim preimage has no exact materialization proof");
    }
    // Completed/later states are checked by unchanged PAGE_RECLAIM REDO and
    // transitive lineage. Never numerically skip an unknown later incarnation.
}

void OwnedRollbackCleanup::completeIntentTail(DiskManager& disk, LogManager& log, WalScanResult& scan,
    Lsn begin, const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease) {
    requireAuthority(gate,lease);
    const auto history=fromBegin(scan.records,begin);
    const auto transactions=analyzeOwnedRollbackHistory(history);
    // A terminal absence claim may not hide a materialized page. Its ID can
    // exist again only through a later, independently chained append reservation.
    // This is not a page-count-only ownership inference or a durable epoch.
    for (const auto& [id,owner] : transactions) {
        static_cast<void>(id);
        if (owner.phase!=OwnedCleanupPhase::AbortRecorded) continue;
        for (const auto& [page,entry] : owner.reservations) {
            if (entry.outcome!=ReservationCleanupOutcome::AbsenceRecorded || page>=disk.pageCount()) continue;
            const OwnedRollbackAnalysis* successor=nullptr;
            Lsn newest=owner.lastLsn;
            for (const auto& [otherId,other] : transactions) {
                static_cast<void>(otherId);
                const auto found=other.reservations.find(page);
                if (other.beginLsn>owner.lastLsn && found!=other.reservations.end()
                    && found->second.reservation.reserveLsn>newest) {
                    successor=&other;newest=found->second.reservation.reserveLsn;
                }
            }
            require(successor!=nullptr && !(successor->phase==OwnedCleanupPhase::AbortRecorded
                && (successor->protocol==RollbackProtocol::LegacyTruncate
                    || successor->reservations.at(page).outcome==ReservationCleanupOutcome::AbsenceRecorded)),
                "Terminal absence has an unaccounted physical page");
        }
    }
    for (const auto& [id,owner] : transactions) for (const auto& [page,entry] : owner.reservations) {
        static_cast<void>(page);
        if (!isValidLsn(entry.intentLsn) || isValidLsn(entry.reclaimLsn)) continue;
        const auto intent=decodeCleanupIntent(log.readRecordAt(entry.intentLsn));
        require(intent.reclaimLsn==scan.validBytes && log.lastValidOffset()==scan.validBytes,
                "Interrupted intent does not own the exact WAL tail");
        validateExtentProvenance(owner,transactions,disk.pageCount());
        const auto physical=proveMaterialized(disk,owner,entry,history);
        DiskManager::Page metadata{};disk.readPhysicalPage(0,metadata);
        require(crc32c(physical)==intent.reclaim.beforeCrc
            && readPersistentPageLsn(physical)==intent.reclaim.beforePageLsn
            && disk.pageCount()==intent.reclaim.pageCount
            && readPersistentPageLsn(metadata)==intent.reclaim.beforeMetadataLsn
            && disk.databaseHeader().freeListRootPageId==intent.reclaim.head,
            "Interrupted intent has stale physical preconditions");
        recoveryFailPoint("owned_tail_before_reclaim_append");
        const auto lsn=log.append({LogRecordType::PageReclaim,0,INVALID_LSN,encodePageReclaim(intent.reclaim)});
        require(lsn==intent.reclaimLsn,"Interrupted reclaim record boundary changed");
        recoveryFailPoint("owned_tail_after_reclaim_append");
        log.flushUpTo(lsn);recoveryFailPoint("owned_tail_after_reclaim_force");
        scan.records.push_back(log.readRecordAt(lsn));
        scan.validBytes=scan.fileBytes=log.lastValidOffset();
        return; // adjacency permits exactly one incomplete intent, at the tail
    }
}

void OwnedRollbackCleanup::finish(DiskManager& disk, LogManager& log, RecoveryTransactionEntry& transaction,
    const DatabaseAccessGate* gate, const DatabaseAccessGate::Lease* lease, RecoveryStats& stats) {
    requireAuthority(gate,lease);
    require(transaction.rollbackProtocol==RollbackProtocol::OwnedAppend
        && transaction.status==RecoveryTransactionStatus::Aborting, "Owned cleanup has no aborting owner");
    auto scan=log.scan();
    auto history=fromBegin(scan.records,transaction.beginLsn);
    const auto transactions=analyzeOwnedRollbackHistory(history);
    auto owner=transactions.at(transaction.transactionId);
    require(owner.logicalUndoChainComplete && isValidLsn(owner.cleanupBeginLsn), "Owned cleanup lacks completed user UNDO");
    disk.reloadDatabaseHeader();
    require(disk.pageCount()>=owner.startPageCount,"Owned cleanup cannot restore a missing extent");
    validateExtentProvenance(owner,transactions,disk.pageCount());
    for (auto& [page,entry] : owner.reservations) {
        if (isValidLsn(entry.progressLsn)) {validateCompleted(disk,entry,log);continue;}
        if (page < disk.pageCount()) {
            if (!isValidLsn(entry.intentLsn)) {
                const auto physical=proveMaterialized(disk,owner,entry,history);
                DiskManager::Page metadata{};disk.readPhysicalPage(0,metadata);
                const PageReclaimPayload payload{page,disk.databaseHeader().freeListRootPageId,disk.pageCount(),
                    readPersistentPageLsn(physical),readPersistentPageLsn(metadata),crc32c(physical)};
                recoveryFailPoint("owned_before_intent_append");
                const auto intent=log.append({LogRecordType::RollbackCleanupIntent,transaction.transactionId,
                    transaction.lastLsn,encodeCleanupIntent({entry.reservation.reserveLsn,cleanupReclaimLsn(log.lastValidOffset()),payload})});
                transaction.lastLsn=entry.intentLsn=intent;
                stats.undoWalBytes+=wal_record_layout::HEADER_SIZE+owned_rollback_layout::INTENT_SIZE;
                recoveryFailPoint("owned_after_intent_append");
                log.flushUpTo(intent);recoveryFailPoint("owned_after_intent_force");
                recoveryFailPoint("owned_before_reclaim_append");
                entry.reclaimLsn=log.append({LogRecordType::PageReclaim,0,INVALID_LSN,encodePageReclaim(payload)});
                require(entry.reclaimLsn==cleanupReclaimLsn(intent),"Owned reclaim association changed");
                stats.undoWalBytes+=wal_record_layout::HEADER_SIZE+page_reclaim_layout::SIZE;
                recoveryFailPoint("reclaim_after_append");
                log.flushUpTo(entry.reclaimLsn);recoveryFailPoint("reclaim_after_wal_force");
                const auto applied=redoPageReclaim(disk,log.readRecordAt(entry.reclaimLsn),[](PageId,Lsn){return false;});
                stats.databaseWrites+=applied.writes;stats.reclaimPageWrites+=applied.writes;++stats.reclaimRecords;
            } else {
                // Startup REDO already completed this exact associated record.
                require(isValidLsn(entry.reclaimLsn),"Owned intent was not completed before REDO");
            }
            recoveryFailPoint("reclaim_before_database_sync");disk.sync();++stats.databaseSyncCalls;
            recoveryFailPoint("reclaim_after_database_sync");
            entry.outcome=ReservationCleanupOutcome::ReclaimRecorded;
            validateCompleted(disk,entry,log);
            recoveryFailPoint("owned_after_reclaim_page_"+std::to_string(page));
        } else {
            require(!isValidLsn(entry.intentLsn),"Reclaim intent lost its physical extent");
            entry.outcome=ReservationCleanupOutcome::AbsenceRecorded;
        }
        recoveryFailPoint("owned_before_progress_append");
        entry.progressLsn=log.append({LogRecordType::RollbackCleanupProgress,transaction.transactionId,
            transaction.lastLsn,encodeCleanupProgress({entry.reservation.reserveLsn,entry.reclaimLsn,page,disk.pageCount()})});
        transaction.lastLsn=entry.progressLsn;
        stats.undoWalBytes+=wal_record_layout::HEADER_SIZE+owned_rollback_layout::PROGRESS_SIZE;
        recoveryFailPoint("owned_after_progress_append");log.flushUpTo(entry.progressLsn);
        recoveryFailPoint("owned_after_progress_force");
    }
    disk.sync();++stats.databaseSyncCalls;
    recoveryFailPoint("recovery_after_database_sync");
    recoveryFailPoint("recovery_before_abort_append");recoveryFailPoint("owned_before_abort_append");
    const auto abort=log.append({LogRecordType::Abort,transaction.transactionId,transaction.lastLsn,{}});
    recoveryFailPoint("recovery_after_abort_append");recoveryFailPoint("owned_after_abort_append");
    log.flushUpTo(abort);recoveryFailPoint("owned_after_abort_force");
    recoveryFailPoint("recovery_after_abort_fsync");recoveryFailPoint("recovery_after_abort_sync");
    transaction.lastLsn=abort;transaction.undoNextLsn=INVALID_LSN;
    transaction.status=RecoveryTransactionStatus::Aborted;transaction.hasDurableAbort=true;
    ++stats.abortedTransactions;stats.durableAbortObserved=true;
}
} // namespace minidb
