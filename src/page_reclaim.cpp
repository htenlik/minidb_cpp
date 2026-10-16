#include "minidb/page_reclaim.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/buffer_pool_manager.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_lsn.hpp"
#include <algorithm>
#include <cstdlib>

namespace minidb {
namespace {
void requirePayload(const PageReclaimPayload& v, WalErrorKind kind) {
    if (v.page == 0 || v.page == INVALID_PAGE_ID || v.page >= v.pageCount
        || v.pageCount > INVALID_PAGE_ID || v.head == v.page
        || (v.head != INVALID_PAGE_ID && (v.head == 0 || v.head >= v.pageCount))) {
        throw WalError(kind, "Invalid PAGE_RECLAIM target/head/extent");
    }
}
[[noreturn]] void corrupt(const char* message) { throw WalError(WalErrorKind::CorruptRecord, message); }
}

std::vector<std::byte> encodePageReclaim(const PageReclaimPayload& v) {
    using namespace page_reclaim_layout;
    requirePayload(v, WalErrorKind::InvalidArgument);
    std::vector<std::byte> out(SIZE);
    byte_codec::writeUint32(out, PAGE, v.page); byte_codec::writeUint32(out, HEAD, v.head);
    byte_codec::writeUint16(out, VERSION_OFFSET, VERSION);
    byte_codec::writeUint64(out, PAGE_COUNT, v.pageCount);
    byte_codec::writeUint64(out, BEFORE_PAGE_LSN, v.beforePageLsn);
    byte_codec::writeUint64(out, BEFORE_METADATA_LSN, v.beforeMetadataLsn);
    byte_codec::writeUint32(out, BEFORE_CRC, v.beforeCrc);
    return out;
}

PageReclaimPayload decodePageReclaim(const LogRecord& r) {
    using namespace page_reclaim_layout;
    const auto& b = r.payload;
    if (r.type != LogRecordType::PageReclaim || r.transactionId != INVALID_TRANSACTION_ID
        || isValidLsn(r.prevLsn) || !isValidLsn(r.lsn) || r.lsn < wal_file_layout::HEADER_SIZE
        || b.size() != SIZE) corrupt("Malformed PAGE_RECLAIM ownership/length");
    if (byte_codec::readUint16(b, VERSION_OFFSET) != VERSION
        || byte_codec::readUint16(b, FLAGS) != 0 || byte_codec::readUint32(b, RESERVED) != 0
        || byte_codec::readUint32(b, TAIL) != 0) corrupt("PAGE_RECLAIM version/flags/reserved");
    PageReclaimPayload v{byte_codec::readUint32(b, PAGE), byte_codec::readUint32(b, HEAD),
        byte_codec::readUint64(b, PAGE_COUNT), byte_codec::readUint64(b, BEFORE_PAGE_LSN),
        byte_codec::readUint64(b, BEFORE_METADATA_LSN), byte_codec::readUint32(b, BEFORE_CRC)};
    requirePayload(v, WalErrorKind::CorruptRecord);
    if ((isValidLsn(v.beforePageLsn) && (v.beforePageLsn < wal_file_layout::HEADER_SIZE || v.beforePageLsn >= r.lsn))
        || (isValidLsn(v.beforeMetadataLsn) && (v.beforeMetadataLsn < wal_file_layout::HEADER_SIZE || v.beforeMetadataLsn >= r.lsn)))
        corrupt("PAGE_RECLAIM invalid before-LSN");
    return v;
}

DiskManager::Page reclaimedPage(const PageReclaimPayload& v, Lsn lsn) {
    using namespace free_page_layout;
    DiskManager::Page page{};
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin());
    byte_codec::writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(page, HEADER_SIZE_OFFSET, HEADER_SIZE);
    byte_codec::writeUint32(page, NEXT_FREE_PAGE_ID_OFFSET, v.head);
    writePersistentPageLsn(page, lsn);
    return page;
}

ReclaimRedoStats redoPageReclaim(DiskManager& disk, const LogRecord& record,
    const std::function<bool(PageId, Lsn)>& knownLaterState) {
    const auto v = decodePageReclaim(record);
    if (disk.pageCount() < v.pageCount) corrupt("PAGE_RECLAIM cannot extend a missing physical extent");
    DiskManager::Page metadataBefore{}; disk.readPhysicalPage(0, metadataBefore);
    const auto metadataLsn = readPersistentPageLsn(metadataBefore);
    if ((!isValidLsn(metadataLsn) || metadataLsn <= record.lsn)
        && database_format::deserializeDatabaseHeader(metadataBefore).catalogRootPageId == v.page) {
        corrupt("PAGE_RECLAIM targets the live catalog root");
    }
    ReclaimRedoStats stats;
    const auto apply = [&](PageId id) {
        DiskManager::Page page{}; disk.readPhysicalPage(id, page);
        const auto current = readPersistentPageLsn(page);
        if (!isValidLsn(current) && knownLaterState(id, INVALID_LSN)) {
            ++stats.skips; return;
        }
        if (isValidLsn(current) && current > record.lsn) {
            if (!knownLaterState(id, current)) corrupt("PAGE_RECLAIM unknown later page identity");
            ++stats.skips; return;
        }
        if (id == v.page) {
            const auto after = reclaimedPage(v, record.lsn);
            if (current == record.lsn) {
                if (page != after) corrupt("PAGE_RECLAIM malformed completed Free Page");
                ++stats.skips; return;
            }
            if (current != v.beforePageLsn || crc32c(page) != v.beforeCrc) corrupt("PAGE_RECLAIM target precondition changed");
            page = after;
        } else {
            auto header = database_format::deserializeDatabaseHeader(page);
            if (header.formatVersion != database_format::CURRENT_VERSION) corrupt("PAGE_RECLAIM requires database v2");
            if (current == record.lsn) {
                if (header.freeListRootPageId != v.page) corrupt("PAGE_RECLAIM completed metadata has wrong head");
                ++stats.skips; return;
            }
            if (current != v.beforeMetadataLsn || header.freeListRootPageId != v.head) corrupt("PAGE_RECLAIM metadata precondition changed");
            header.freeListRootPageId = v.page;
            database_format::serializeDatabaseHeader(header, page);
            writePersistentPageLsn(page, record.lsn);
        }
        disk.writePhysicalPage(id, page); disk.flush(); ++stats.writes;
        recoveryFailPoint(id == 0 ? "reclaim_after_metadata_write" : "reclaim_after_free_write");
    };
    // Test-only ordering inversion exercises the other valid two-write prefix.
    const bool metadataFirst = std::getenv("MINIDB_RECLAIM_PAGE0_FIRST") != nullptr;
    apply(metadataFirst ? 0 : v.page); apply(metadataFirst ? v.page : 0);
    return stats;
}

Lsn PageReclaimer::reclaim(const UnownedPageCertificate& certificate,
                         const DatabaseAccessGate::Lease& lease) {
    if (!lease.owns(gate_) || lease.mode() != AccessMode::ReadWrite
        || recovery_.hasActiveStatement() || !recovery_.checkpointSafe()
        || pool_.totalPinCount() != 0) throw std::logic_error("Reclaim requires idle exclusive ownership and no pins");
    if (certificate.disk_ != &disk_ || certificate.nextId_ != recovery_.nextTransactionId()
        || certificate.walEnd_ != log_.lastValidOffset()) {
        throw std::logic_error("Eligibility certificate crossed a transaction/system-operation boundary");
    }
    const auto page = certificate.page_;
    if (page == 0 || page == INVALID_PAGE_ID || page >= disk_.pageCount()
        || page == disk_.databaseHeader().catalogRootPageId) throw std::invalid_argument("Ineligible reclaimed page");
    pool_.flushAll(); // persist committed frames before obtaining the stable pre-state
    PageAllocator allocator(pool_, disk_);
    const auto members = allocator.freePageIds();
    if (std::find(members.begin(), members.end(), page) != members.end()) throw std::logic_error("Page already free");
    DiskManager::Page before{}, metadata{};
    disk_.readPhysicalPage(page, before); disk_.readPhysicalPage(0, metadata);
    if (crc32c(before) != certificate.crc_) throw std::logic_error("Stale unowned-page certificate");
    if (disk_.databaseHeader().formatVersion != database_format::CURRENT_VERSION) throw std::logic_error("Reclaim requires database v2");
    const PageReclaimPayload payload{page, disk_.databaseHeader().freeListRootPageId,
        disk_.pageCount(), readPersistentPageLsn(before), readPersistentPageLsn(metadata), crc32c(before)};
    recovery_.beginAllocatorSystemOperation();
    // Failure from here poisons the coordinator until restart. No checkpoint may
    // advance past an incompletely persisted system operation.
    pool_.discardPageForRecovery(page); pool_.discardPageForRecovery(0);
    recoveryFailPoint("reclaim_before_append");
    const auto lsn = log_.append({LogRecordType::PageReclaim, INVALID_TRANSACTION_ID, INVALID_LSN,
        encodePageReclaim(payload)});
    recoveryFailPoint("reclaim_after_append");
    log_.flushUpTo(lsn); recoveryFailPoint("reclaim_after_wal_force");
    static_cast<void>(redoPageReclaim(disk_, {LogRecordType::PageReclaim, 0, INVALID_LSN,
        encodePageReclaim(payload), lsn}, [](PageId, Lsn) { return false; }));
    recoveryFailPoint("reclaim_before_database_sync"); disk_.sync();
    recoveryFailPoint("reclaim_after_database_sync");
    recovery_.finishAllocatorSystemOperation();
    return lsn;
}
} // namespace minidb
