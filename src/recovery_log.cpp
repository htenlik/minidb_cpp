#include "minidb/recovery_log.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace minidb {

std::vector<std::byte> encodeBeginLogPayload(BeginLogPayload payload) {
    std::vector<std::byte> bytes(begin_log_layout::PAYLOAD_SIZE);
    byte_codec::writeUint64(
        bytes, begin_log_layout::START_PAGE_COUNT_OFFSET, payload.startPageCount);
    return bytes;
}

BeginLogPayload decodeBeginLogPayload(std::span<const std::byte> bytes) {
    if (bytes.size() != begin_log_layout::PAYLOAD_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "BEGIN payload has the wrong size");
    }
    if (byte_codec::readUint64(bytes, begin_log_layout::RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "BEGIN payload reserved bytes are nonzero");
    }
    const auto count = byte_codec::readUint64(
        bytes, begin_log_layout::START_PAGE_COUNT_OFFSET);
    if (count == 0 || count > INVALID_PAGE_ID) {
        throw WalError(WalErrorKind::CorruptRecord, "BEGIN start page count is invalid");
    }
    return BeginLogPayload{count};
}

std::vector<std::byte> encodePageUpdateLogPayload(
    const PageUpdateLogPayload& payload) {
    using namespace page_update_log_layout;
    if (payload.pageId == INVALID_PAGE_ID) {
        throw WalError(WalErrorKind::InvalidArgument, "PAGE_UPDATE PageId is invalid");
    }
    if (!payload.beforePageExisted
        && !std::all_of(payload.beforeImage.begin(), payload.beforeImage.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw WalError(
            WalErrorKind::InvalidArgument,
            "PAGE_UPDATE for a new page must use a zero before-image");
    }
    std::vector<std::byte> bytes(PAYLOAD_SIZE);
    byte_codec::writeUint32(bytes, PAGE_ID_OFFSET, payload.pageId);
    byte_codec::writeUint32(
        bytes, FLAGS_OFFSET, payload.beforePageExisted ? BEFORE_PAGE_EXISTED : 0U);
    byte_codec::writeUint32(
        bytes, PAGE_SIZE_OFFSET, static_cast<std::uint32_t>(database_format::PAGE_SIZE));
    std::copy(payload.beforeImage.begin(), payload.beforeImage.end(),
              bytes.begin() + BEFORE_IMAGE_OFFSET);
    std::copy(payload.afterImage.begin(), payload.afterImage.end(),
              bytes.begin() + AFTER_IMAGE_OFFSET);
    return bytes;
}

PageUpdateLogPayload decodePageUpdateLogPayload(std::span<const std::byte> bytes) {
    using namespace page_update_log_layout;
    if (bytes.size() != PAYLOAD_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord, "PAGE_UPDATE payload has the wrong size");
    }
    const auto flags = byte_codec::readUint32(bytes, FLAGS_OFFSET);
    if ((flags & ~BEFORE_PAGE_EXISTED) != 0
        || byte_codec::readUint32(bytes, PAGE_SIZE_OFFSET) != database_format::PAGE_SIZE
        || byte_codec::readUint32(bytes, RESERVED_OFFSET) != 0) {
        throw WalError(WalErrorKind::CorruptRecord, "PAGE_UPDATE header fields are malformed");
    }
    PageUpdateLogPayload payload;
    payload.pageId = byte_codec::readUint32(bytes, PAGE_ID_OFFSET);
    payload.beforePageExisted = (flags & BEFORE_PAGE_EXISTED) != 0;
    if (payload.pageId == INVALID_PAGE_ID) {
        throw WalError(WalErrorKind::CorruptRecord, "PAGE_UPDATE PageId is invalid");
    }
    std::copy_n(bytes.begin() + BEFORE_IMAGE_OFFSET, payload.beforeImage.size(),
                payload.beforeImage.begin());
    std::copy_n(bytes.begin() + AFTER_IMAGE_OFFSET, payload.afterImage.size(),
                payload.afterImage.begin());
    if (!payload.beforePageExisted
        && !std::all_of(payload.beforeImage.begin(), payload.beforeImage.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw WalError(WalErrorKind::CorruptRecord, "New-page before-image is not zero");
    }
    return payload;
}

void validateTransactionRecordPayload(const LogRecord& record) {
    if (record.transactionId == INVALID_TRANSACTION_ID) {
        throw WalError(WalErrorKind::CorruptRecord, "Transaction record uses transaction ID zero");
    }
    switch (record.type) {
    case LogRecordType::Begin:
        if (isValidLsn(record.prevLsn)) {
            throw WalError(WalErrorKind::CorruptRecord, "BEGIN record has a prevLSN");
        }
        static_cast<void>(decodeBeginLogPayload(record.payload));
        return;
    case LogRecordType::PageUpdate:
        if (!isValidLsn(record.prevLsn)) {
            throw WalError(WalErrorKind::CorruptRecord, "PAGE_UPDATE record lacks prevLSN");
        }
        static_cast<void>(decodePageUpdateLogPayload(record.payload));
        return;
    case LogRecordType::Commit:
    case LogRecordType::Abort:
        if (!record.payload.empty() || !isValidLsn(record.prevLsn)) {
            throw WalError(WalErrorKind::CorruptRecord, "Transaction terminator is malformed");
        }
        return;
    case LogRecordType::Compensation:
    case LogRecordType::CheckpointBegin:
    case LogRecordType::CheckpointEnd:
        throw WalError(WalErrorKind::CorruptRecord, "Unsupported WAL record type in recovery");
    }
    throw WalError(WalErrorKind::CorruptRecord, "Unknown WAL record type in recovery");
}

} // namespace minidb
