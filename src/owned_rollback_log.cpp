#include "minidb/owned_rollback_log.hpp"
#include "minidb/byte_codec.hpp"

#include <algorithm>

namespace minidb {
namespace {
using namespace owned_rollback_layout;
void require(bool valid, const char* message, WalErrorKind kind = WalErrorKind::CorruptRecord) {
    if (!valid) throw WalError(kind, message);
}
bool reference(Lsn lsn) { return isValidLsn(lsn) && lsn >= wal_file_layout::HEADER_SIZE; }
bool extent(std::uint64_t count) { return count != 0 && count <= INVALID_PAGE_ID; }
void validate(OwnedBeginPayload v, WalErrorKind kind) {
    require(extent(v.startPageCount), "Invalid owned BEGIN extent", kind);
}
void validate(CleanupBeginPayload v, WalErrorKind kind) {
    require(reference(v.undoEnd) && v.reservationCount < INVALID_PAGE_ID,
            "Invalid cleanup BEGIN reference/count", kind);
}
void validate(const CleanupIntentPayload& v, WalErrorKind kind) {
    constexpr auto intentBytes = wal_record_layout::HEADER_SIZE + INTENT_SIZE;
    require(reference(v.reserveLsn) && reference(v.reclaimLsn)
        && v.reclaimLsn >= wal_file_layout::HEADER_SIZE + intentBytes
        && v.reserveLsn < v.reclaimLsn - intentBytes,
            "Invalid cleanup intent references", kind);
    static_cast<void>(cleanupReclaimLsn(v.reclaimLsn - intentBytes));
    const auto intent = v.reclaimLsn - intentBytes;
    require((!isValidLsn(v.reclaim.beforePageLsn) || v.reclaim.beforePageLsn < intent)
        && (!isValidLsn(v.reclaim.beforeMetadataLsn) || v.reclaim.beforeMetadataLsn < intent),
        "Cleanup intent before-state must precede the intent", kind);
    // Reuse the strict type-14 validator, including before-LSN ordering.
    const auto payload = encodePageReclaim(v.reclaim);
    static_cast<void>(decodePageReclaim({LogRecordType::PageReclaim, 0, INVALID_LSN, payload, v.reclaimLsn}));
}
void validate(CleanupProgressPayload v, WalErrorKind kind) {
    require(reference(v.reserveLsn) && v.page != 0 && v.page != INVALID_PAGE_ID && extent(v.observedExtent)
        && (isValidLsn(v.reclaimLsn)
            ? reference(v.reclaimLsn) && v.reserveLsn < v.reclaimLsn && v.page < v.observedExtent
            : v.page >= v.observedExtent), "Invalid cleanup progress references/page/extent", kind);
}
std::vector<std::byte> header(std::size_t size) {
    std::vector<std::byte> bytes(size);
    byte_codec::writeUint16(bytes, VERSION_OFFSET, VERSION);
    byte_codec::writeUint16(bytes, SIZE_OFFSET, static_cast<std::uint16_t>(size));
    return bytes;
}
void check(const LogRecord& r, LogRecordType type, std::size_t size, bool begin = false) {
    require(r.type == type && r.transactionId != 0 && reference(r.lsn) && r.payload.size() == size
        && r.lsn < INVALID_LSN - wal_record_layout::HEADER_SIZE - size
        && (begin ? !isValidLsn(r.prevLsn) : reference(r.prevLsn) && r.prevLsn < r.lsn),
        "Malformed owned rollback record identity/length/chain");
    require(byte_codec::readUint16(r.payload, VERSION_OFFSET) == VERSION
        && byte_codec::readUint16(r.payload, SIZE_OFFSET) == size
        && byte_codec::readUint32(r.payload, FLAGS) == 0, "Owned rollback version/size/flags");
}
}

Lsn cleanupReclaimLsn(Lsn intentLsn) {
    constexpr auto bytes = wal_record_layout::HEADER_SIZE + owned_rollback_layout::INTENT_SIZE;
    constexpr auto reclaimBytes = wal_record_layout::HEADER_SIZE + page_reclaim_layout::SIZE;
    require(reference(intentLsn) && intentLsn < INVALID_LSN - bytes - reclaimBytes,
            "Cleanup reclaim LSN overflow");
    return intentLsn + bytes;
}
std::vector<std::byte> encodeOwnedBegin(OwnedBeginPayload v) {
    validate(v, WalErrorKind::InvalidArgument);
    auto bytes = header(BEGIN_SIZE);
    byte_codec::writeUint64(bytes, START_PAGE_COUNT, v.startPageCount);
    return bytes;
}
OwnedBeginPayload decodeOwnedBegin(const LogRecord& r) {
    check(r, LogRecordType::BeginOwned, BEGIN_SIZE, true);
    require(byte_codec::readUint64(r.payload, BEGIN_RESERVED) == 0, "Owned BEGIN reserved bytes");
    const OwnedBeginPayload v{byte_codec::readUint64(r.payload, START_PAGE_COUNT)};
    validate(v, WalErrorKind::CorruptRecord);
    return v;
}
std::vector<std::byte> encodeCleanupBegin(CleanupBeginPayload v) {
    validate(v, WalErrorKind::InvalidArgument);
    auto bytes = header(CLEANUP_SIZE);
    byte_codec::writeUint64(bytes, UNDO_END, v.undoEnd);
    byte_codec::writeUint64(bytes, RESERVATION_COUNT, v.reservationCount);
    return bytes;
}
CleanupBeginPayload decodeCleanupBegin(const LogRecord& r) {
    check(r, LogRecordType::RollbackCleanupBegin, CLEANUP_SIZE);
    const CleanupBeginPayload v{byte_codec::readUint64(r.payload, UNDO_END), byte_codec::readUint64(r.payload, RESERVATION_COUNT)};
    validate(v, WalErrorKind::CorruptRecord);
    require(v.undoEnd < r.lsn, "Cleanup BEGIN forward reference");
    return v;
}
std::vector<std::byte> encodeCleanupIntent(const CleanupIntentPayload& v) {
    validate(v, WalErrorKind::InvalidArgument);
    auto bytes = header(INTENT_SIZE);
    byte_codec::writeUint64(bytes, RESERVE_LSN, v.reserveLsn);
    byte_codec::writeUint64(bytes, RECLAIM_LSN, v.reclaimLsn);
    const auto payload = encodePageReclaim(v.reclaim);
    std::copy(payload.begin(), payload.end(), bytes.begin() + RECLAIM_PAYLOAD);
    return bytes;
}
CleanupIntentPayload decodeCleanupIntent(const LogRecord& r) {
    check(r, LogRecordType::RollbackCleanupIntent, INTENT_SIZE);
    const auto reserve = byte_codec::readUint64(r.payload, RESERVE_LSN);
    const auto reclaim = byte_codec::readUint64(r.payload, RECLAIM_LSN);
    require(reference(reserve) && reserve < r.lsn && reclaim == cleanupReclaimLsn(r.lsn),
            "Cleanup intent reference/record boundary");
    LogRecord system{LogRecordType::PageReclaim, 0, INVALID_LSN,
        {r.payload.begin() + RECLAIM_PAYLOAD, r.payload.end()}, reclaim};
    const CleanupIntentPayload v{reserve, reclaim, decodePageReclaim(system)};
    validate(v, WalErrorKind::CorruptRecord);
    return v;
}
std::vector<std::byte> encodeCleanupProgress(CleanupProgressPayload v) {
    validate(v, WalErrorKind::InvalidArgument);
    auto bytes = header(PROGRESS_SIZE);
    byte_codec::writeUint64(bytes, RESERVE_LSN, v.reserveLsn);
    byte_codec::writeUint64(bytes, RECLAIM_LSN, v.reclaimLsn);
    byte_codec::writeUint32(bytes, PROGRESS_PAGE, v.page);
    byte_codec::writeUint64(bytes, OBSERVED_EXTENT, v.observedExtent);
    return bytes;
}
CleanupProgressPayload decodeCleanupProgress(const LogRecord& r) {
    check(r, LogRecordType::RollbackCleanupProgress, PROGRESS_SIZE);
    require(byte_codec::readUint32(r.payload, PROGRESS_RESERVED) == 0, "Cleanup progress reserved bytes");
    const CleanupProgressPayload v{byte_codec::readUint64(r.payload, RESERVE_LSN), byte_codec::readUint64(r.payload, RECLAIM_LSN),
        byte_codec::readUint32(r.payload, PROGRESS_PAGE), byte_codec::readUint64(r.payload, OBSERVED_EXTENT)};
    validate(v, WalErrorKind::CorruptRecord);
    require(v.reserveLsn < r.lsn && (!isValidLsn(v.reclaimLsn) || v.reclaimLsn < r.lsn), "Cleanup progress forward reference");
    return v;
}
} // namespace minidb
