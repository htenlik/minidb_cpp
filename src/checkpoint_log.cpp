#include "minidb/checkpoint_log.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/database_format.hpp"

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
    throw WalError(WalErrorKind::CorruptRecord, "Record is not a checkpoint record");
}

} // namespace minidb
