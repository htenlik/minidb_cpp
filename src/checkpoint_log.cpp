#include "minidb/checkpoint_log.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/database_format.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace minidb {

std::vector<std::byte> encodeCheckpointBeginLogPayload(
    const CheckpointBeginLogPayload& payload) {
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || payload.walStartOffset < wal_file_layout::HEADER_SIZE
        || payload.walStartOffset == INVALID_WAL_OFFSET) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid CHECKPOINT_BEGIN payload");
    }
    std::vector<std::byte> bytes(checkpoint_begin_log_layout::PAYLOAD_SIZE);
    byte_codec::writeUint64(bytes, checkpoint_begin_log_layout::CHECKPOINT_ID_OFFSET,
                            payload.checkpointId);
    byte_codec::writeUint64(bytes, checkpoint_begin_log_layout::PREVIOUS_END_LSN_OFFSET,
                            payload.previousCheckpointEndLsn);
    byte_codec::writeUint64(bytes, checkpoint_begin_log_layout::WAL_START_OFFSET,
                            payload.walStartOffset);
    return bytes;
}

CheckpointBeginLogPayload decodeCheckpointBeginLogPayload(
    std::span<const std::byte> bytes) {
    if (bytes.size() != checkpoint_begin_log_layout::PAYLOAD_SIZE
        || byte_codec::readUint64(bytes, checkpoint_begin_log_layout::RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "Malformed CHECKPOINT_BEGIN payload");
    }
    CheckpointBeginLogPayload payload{
        byte_codec::readUint64(bytes, checkpoint_begin_log_layout::CHECKPOINT_ID_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_begin_log_layout::PREVIOUS_END_LSN_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_begin_log_layout::WAL_START_OFFSET),
    };
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || payload.walStartOffset < wal_file_layout::HEADER_SIZE
        || payload.walStartOffset == INVALID_WAL_OFFSET) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid CHECKPOINT_BEGIN fields");
    }
    return payload;
}

std::vector<std::byte> encodeCheckpointEndLogPayload(
    const CheckpointEndLogPayload& payload) {
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(payload.checkpointBeginLsn)
        || payload.databasePageCount == 0
        || payload.nextTransactionId == INVALID_TRANSACTION_ID
        || payload.recoveryStartOffset == INVALID_WAL_OFFSET) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid CHECKPOINT_END payload");
    }
    std::vector<std::byte> bytes(checkpoint_end_log_layout::PAYLOAD_SIZE);
    byte_codec::writeUint64(bytes, checkpoint_end_log_layout::CHECKPOINT_ID_OFFSET,
                            payload.checkpointId);
    byte_codec::writeUint64(bytes, checkpoint_end_log_layout::BEGIN_LSN_OFFSET,
                            payload.checkpointBeginLsn);
    byte_codec::writeUint64(bytes, checkpoint_end_log_layout::DATABASE_PAGE_COUNT_OFFSET,
                            payload.databasePageCount);
    byte_codec::writeUint64(bytes, checkpoint_end_log_layout::NEXT_TRANSACTION_ID_OFFSET,
                            payload.nextTransactionId);
    byte_codec::writeUint64(bytes, checkpoint_end_log_layout::RECOVERY_START_OFFSET,
                            payload.recoveryStartOffset);
    return bytes;
}

CheckpointEndLogPayload decodeCheckpointEndLogPayload(std::span<const std::byte> bytes) {
    if (bytes.size() != checkpoint_end_log_layout::PAYLOAD_SIZE
        || byte_codec::readUint64(bytes, checkpoint_end_log_layout::RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "Malformed CHECKPOINT_END payload");
    }
    CheckpointEndLogPayload payload{
        byte_codec::readUint64(bytes, checkpoint_end_log_layout::CHECKPOINT_ID_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_end_log_layout::BEGIN_LSN_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_end_log_layout::DATABASE_PAGE_COUNT_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_end_log_layout::NEXT_TRANSACTION_ID_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_end_log_layout::RECOVERY_START_OFFSET),
    };
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(payload.checkpointBeginLsn)
        || payload.databasePageCount == 0
        || payload.databasePageCount > INVALID_PAGE_ID
        || payload.nextTransactionId == INVALID_TRANSACTION_ID
        || payload.recoveryStartOffset == INVALID_WAL_OFFSET) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid CHECKPOINT_END fields");
    }
    return payload;
}

std::vector<std::byte> encodeFuzzyCheckpointBeginLogPayload(
    const FuzzyCheckpointBeginLogPayload& payload) {
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || (isValidLsn(payload.previousCheckpointEndLsn)
            && payload.previousCheckpointEndLsn < wal_file_layout::HEADER_SIZE)) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid FUZZY_CHECKPOINT_BEGIN payload");
    }
    std::vector<std::byte> bytes(fuzzy_checkpoint_begin_log_layout::PAYLOAD_SIZE);
    byte_codec::writeUint16(bytes, fuzzy_checkpoint_begin_log_layout::VERSION_OFFSET,
                            fuzzy_checkpoint_begin_log_layout::CURRENT_VERSION);
    byte_codec::writeUint16(bytes, fuzzy_checkpoint_begin_log_layout::HEADER_SIZE_OFFSET,
                            fuzzy_checkpoint_begin_log_layout::PAYLOAD_SIZE);
    byte_codec::writeUint64(bytes, fuzzy_checkpoint_begin_log_layout::CHECKPOINT_ID_OFFSET,
                            payload.checkpointId);
    byte_codec::writeUint64(bytes, fuzzy_checkpoint_begin_log_layout::PREVIOUS_END_LSN_OFFSET,
                            payload.previousCheckpointEndLsn);
    return bytes;
}

FuzzyCheckpointBeginLogPayload decodeFuzzyCheckpointBeginLogPayload(
    std::span<const std::byte> bytes) {
    using namespace fuzzy_checkpoint_begin_log_layout;
    if (bytes.size() != PAYLOAD_SIZE
        || byte_codec::readUint16(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint16(bytes, HEADER_SIZE_OFFSET) != PAYLOAD_SIZE
        || byte_codec::readUint32(bytes, FLAGS_OFFSET) != 0
        || byte_codec::readUint64(bytes, RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "Malformed FUZZY_CHECKPOINT_BEGIN payload");
    }
    FuzzyCheckpointBeginLogPayload payload{
        byte_codec::readUint64(bytes, CHECKPOINT_ID_OFFSET),
        byte_codec::readUint64(bytes, PREVIOUS_END_LSN_OFFSET),
    };
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || (isValidLsn(payload.previousCheckpointEndLsn)
            && payload.previousCheckpointEndLsn < wal_file_layout::HEADER_SIZE)) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid FUZZY_CHECKPOINT_BEGIN fields");
    }
    return payload;
}

namespace {

void validateDirtyPages(const std::vector<DirtyPageEntry>& entries, WalErrorKind kind) {
    PageId previous = 0;
    bool first = true;
    for (const auto& entry : entries) {
        if (entry.pageId == database_format::METADATA_PAGE_ID
            || entry.pageId == INVALID_PAGE_ID || !isValidLsn(entry.recLsn)
            || entry.recLsn < wal_file_layout::HEADER_SIZE
            || (!first && entry.pageId <= previous)) {
            throw WalError(kind, "Invalid or noncanonical fuzzy-checkpoint DPT");
        }
        previous = entry.pageId;
        first = false;
    }
}

void validateActiveTransactions(
    const std::vector<CheckpointTransactionEntry>& entries,
    WalErrorKind kind) {
    TransactionId previous = INVALID_TRANSACTION_ID;
    for (const auto& entry : entries) {
        if (entry.transactionId == INVALID_TRANSACTION_ID
            || entry.status != CheckpointTransactionStatus::Active
            || !isValidLsn(entry.beginLsn) || !isValidLsn(entry.lastLsn)
            || entry.beginLsn < wal_file_layout::HEADER_SIZE
            || entry.lastLsn < entry.beginLsn
            || entry.startPageCount == 0 || entry.startPageCount > INVALID_PAGE_ID
            || (previous != INVALID_TRANSACTION_ID && entry.transactionId <= previous)) {
            throw WalError(kind, "Invalid or noncanonical fuzzy-checkpoint ATT");
        }
        previous = entry.transactionId;
    }
}

} // namespace

std::vector<std::byte> encodeFuzzyCheckpointEndLogPayload(
    const FuzzyCheckpointEndLogPayload& payload) {
    using namespace fuzzy_checkpoint_end_log_layout;
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(payload.checkpointBeginLsn)
        || payload.checkpointBeginLsn < wal_file_layout::HEADER_SIZE
        || payload.databasePageCount == 0 || payload.databasePageCount > INVALID_PAGE_ID
        || payload.nextTransactionId == INVALID_TRANSACTION_ID
        || payload.dirtyPages.size() > std::numeric_limits<std::uint32_t>::max()
        || payload.activeTransactions.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid FUZZY_CHECKPOINT_END payload");
    }
    validateDirtyPages(payload.dirtyPages, WalErrorKind::InvalidArgument);
    validateActiveTransactions(payload.activeTransactions, WalErrorKind::InvalidArgument);
    const auto dptBytes = payload.dirtyPages.size() * DPT_ENTRY_SIZE;
    const auto attBytes = payload.activeTransactions.size() * ATT_ENTRY_SIZE;
    if (dptBytes > wal_record_layout::MAX_PAYLOAD_SIZE - HEADER_SIZE
        || attBytes > wal_record_layout::MAX_PAYLOAD_SIZE - HEADER_SIZE - dptBytes) {
        throw WalError(WalErrorKind::InvalidArgument, "FUZZY_CHECKPOINT_END payload is too large");
    }
    std::vector<std::byte> bytes(HEADER_SIZE + dptBytes + attBytes);
    byte_codec::writeUint16(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint16(bytes, HEADER_SIZE_OFFSET, HEADER_SIZE);
    byte_codec::writeUint16(bytes, DPT_ENTRY_SIZE_OFFSET, DPT_ENTRY_SIZE);
    byte_codec::writeUint16(bytes, ATT_ENTRY_SIZE_OFFSET, ATT_ENTRY_SIZE);
    byte_codec::writeUint64(bytes, CHECKPOINT_ID_OFFSET, payload.checkpointId);
    byte_codec::writeUint64(bytes, BEGIN_LSN_OFFSET, payload.checkpointBeginLsn);
    byte_codec::writeUint64(bytes, DATABASE_PAGE_COUNT_OFFSET, payload.databasePageCount);
    byte_codec::writeUint64(bytes, NEXT_TRANSACTION_ID_OFFSET, payload.nextTransactionId);
    byte_codec::writeUint32(bytes, DPT_COUNT_OFFSET,
                            static_cast<std::uint32_t>(payload.dirtyPages.size()));
    byte_codec::writeUint32(bytes, ATT_COUNT_OFFSET,
                            static_cast<std::uint32_t>(payload.activeTransactions.size()));
    std::size_t cursor = HEADER_SIZE;
    for (const auto& entry : payload.dirtyPages) {
        byte_codec::writeUint32(bytes, cursor + fuzzy_checkpoint_dpt_entry_layout::PAGE_ID_OFFSET,
                                entry.pageId);
        byte_codec::writeUint64(bytes, cursor + fuzzy_checkpoint_dpt_entry_layout::REC_LSN_OFFSET,
                                entry.recLsn);
        cursor += DPT_ENTRY_SIZE;
    }
    for (const auto& entry : payload.activeTransactions) {
        byte_codec::writeUint64(
            bytes, cursor + fuzzy_checkpoint_att_entry_layout::TRANSACTION_ID_OFFSET,
            entry.transactionId);
        byte_codec::writeUint16(bytes, cursor + fuzzy_checkpoint_att_entry_layout::STATUS_OFFSET,
                                static_cast<std::uint16_t>(entry.status));
        byte_codec::writeUint64(bytes, cursor + fuzzy_checkpoint_att_entry_layout::BEGIN_LSN_OFFSET,
                                entry.beginLsn);
        byte_codec::writeUint64(bytes, cursor + fuzzy_checkpoint_att_entry_layout::LAST_LSN_OFFSET,
                                entry.lastLsn);
        byte_codec::writeUint64(
            bytes, cursor + fuzzy_checkpoint_att_entry_layout::START_PAGE_COUNT_OFFSET,
            entry.startPageCount);
        cursor += ATT_ENTRY_SIZE;
    }
    return bytes;
}

FuzzyCheckpointEndLogPayload decodeFuzzyCheckpointEndLogPayload(
    std::span<const std::byte> bytes) {
    using namespace fuzzy_checkpoint_end_log_layout;
    if (bytes.size() < HEADER_SIZE
        || bytes.size() > wal_record_layout::MAX_PAYLOAD_SIZE
        || byte_codec::readUint16(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint16(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint16(bytes, DPT_ENTRY_SIZE_OFFSET) != DPT_ENTRY_SIZE
        || byte_codec::readUint16(bytes, ATT_ENTRY_SIZE_OFFSET) != ATT_ENTRY_SIZE
        || !std::all_of(bytes.begin() + RESERVED_OFFSET, bytes.begin() + HEADER_SIZE,
                        [](std::byte value) { return value == std::byte{0}; })) {
        throw WalError(WalErrorKind::CorruptRecord, "Malformed FUZZY_CHECKPOINT_END header");
    }
    const auto dptCount = byte_codec::readUint32(bytes, DPT_COUNT_OFFSET);
    const auto attCount = byte_codec::readUint32(bytes, ATT_COUNT_OFFSET);
    const auto available = bytes.size() - HEADER_SIZE;
    if (dptCount > available / DPT_ENTRY_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "FUZZY_CHECKPOINT_END DPT count is excessive");
    }
    const auto dptBytes = static_cast<std::size_t>(dptCount) * DPT_ENTRY_SIZE;
    if (attCount > (available - dptBytes) / ATT_ENTRY_SIZE
        || HEADER_SIZE + dptBytes + static_cast<std::size_t>(attCount) * ATT_ENTRY_SIZE
            != bytes.size()) {
        throw WalError(WalErrorKind::CorruptRecord, "FUZZY_CHECKPOINT_END size is inconsistent");
    }
    FuzzyCheckpointEndLogPayload payload;
    payload.checkpointId = byte_codec::readUint64(bytes, CHECKPOINT_ID_OFFSET);
    payload.checkpointBeginLsn = byte_codec::readUint64(bytes, BEGIN_LSN_OFFSET);
    payload.databasePageCount = byte_codec::readUint64(bytes, DATABASE_PAGE_COUNT_OFFSET);
    payload.nextTransactionId = byte_codec::readUint64(bytes, NEXT_TRANSACTION_ID_OFFSET);
    std::size_t cursor = HEADER_SIZE;
    payload.dirtyPages.reserve(dptCount);
    for (std::uint32_t index = 0; index < dptCount; ++index) {
        if (byte_codec::readUint32(
                bytes, cursor + fuzzy_checkpoint_dpt_entry_layout::RESERVED_OFFSET) != 0) {
            throw WalError(WalErrorKind::CorruptRecord, "Nonzero fuzzy-checkpoint DPT reserved field");
        }
        payload.dirtyPages.push_back({
            byte_codec::readUint32(
                bytes, cursor + fuzzy_checkpoint_dpt_entry_layout::PAGE_ID_OFFSET),
            byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_dpt_entry_layout::REC_LSN_OFFSET),
            INVALID_LSN,
        });
        cursor += DPT_ENTRY_SIZE;
    }
    payload.activeTransactions.reserve(attCount);
    for (std::uint32_t index = 0; index < attCount; ++index) {
        if (byte_codec::readUint16(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::RESERVED16_OFFSET) != 0
            || byte_codec::readUint32(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::RESERVED32_OFFSET) != 0
            || byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::RESERVED64_OFFSET) != 0) {
            throw WalError(WalErrorKind::CorruptRecord, "Nonzero fuzzy-checkpoint ATT reserved field");
        }
        payload.activeTransactions.push_back({
            byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::TRANSACTION_ID_OFFSET),
            static_cast<CheckpointTransactionStatus>(byte_codec::readUint16(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::STATUS_OFFSET)),
            byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::BEGIN_LSN_OFFSET),
            byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::LAST_LSN_OFFSET),
            byte_codec::readUint64(
                bytes, cursor + fuzzy_checkpoint_att_entry_layout::START_PAGE_COUNT_OFFSET),
        });
        cursor += ATT_ENTRY_SIZE;
    }
    if (payload.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(payload.checkpointBeginLsn)
        || payload.checkpointBeginLsn < wal_file_layout::HEADER_SIZE
        || payload.databasePageCount == 0 || payload.databasePageCount > INVALID_PAGE_ID
        || payload.nextTransactionId == INVALID_TRANSACTION_ID) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid FUZZY_CHECKPOINT_END fields");
    }
    validateDirtyPages(payload.dirtyPages, WalErrorKind::CorruptRecord);
    validateActiveTransactions(payload.activeTransactions, WalErrorKind::CorruptRecord);
    return payload;
}

void validateCheckpointRecord(const LogRecord& record) {
    if (record.transactionId != INVALID_TRANSACTION_ID || isValidLsn(record.prevLsn)) {
        throw WalError(WalErrorKind::CorruptRecord, "Checkpoint WAL record is not a system record");
    }
    if (record.type == LogRecordType::CheckpointBegin) {
        const auto payload = decodeCheckpointBeginLogPayload(record.payload);
        if (payload.walStartOffset != record.lsn) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "CHECKPOINT_BEGIN start offset disagrees with its LSN");
        }
        return;
    }
    if (record.type == LogRecordType::CheckpointEnd) {
        static_cast<void>(decodeCheckpointEndLogPayload(record.payload));
        return;
    }
    if (record.type == LogRecordType::FuzzyCheckpointBegin) {
        static_cast<void>(decodeFuzzyCheckpointBeginLogPayload(record.payload));
        return;
    }
    if (record.type == LogRecordType::FuzzyCheckpointEnd) {
        static_cast<void>(decodeFuzzyCheckpointEndLogPayload(record.payload));
        return;
    }
    throw WalError(WalErrorKind::CorruptRecord, "Record is not a checkpoint record");
}

} // namespace minidb
