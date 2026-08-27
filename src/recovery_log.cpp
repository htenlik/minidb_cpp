#include "minidb/recovery_log.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <limits>
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

std::vector<PageByteRange> computePageDelta(
    const DiskManager::Page& before,
    const DiskManager::Page& after) {
    return computePageDelta(before, after, {});
}

std::vector<PageByteRange> computePageDelta(
    const DiskManager::Page& before,
    const DiskManager::Page& after,
    std::span<const bool> requiredOffsets) {
    if (!requiredOffsets.empty() && requiredOffsets.size() != database_format::PAGE_SIZE) {
        throw std::invalid_argument("Required delta-offset mask has the wrong size");
    }
    const auto included = [&](std::size_t index) {
        return before[index] != after[index]
            || (!requiredOffsets.empty() && requiredOffsets[index]);
    };
    std::vector<PageByteRange> ranges;
    std::size_t offset = 0;
    while (offset < database_format::PAGE_SIZE) {
        if (!included(offset)) {
            ++offset;
            continue;
        }
        const auto begin = offset;
        while (offset < database_format::PAGE_SIZE && included(offset)) ++offset;
        const auto length = offset - begin;
        PageByteRange range;
        range.offset = static_cast<std::uint16_t>(begin);
        range.length = static_cast<std::uint16_t>(length);
        range.beforeBytes.assign(before.begin() + begin, before.begin() + offset);
        range.afterBytes.assign(after.begin() + begin, after.begin() + offset);
        ranges.push_back(std::move(range));
    }
    return ranges;
}

namespace {

void validateDeltaRanges(const PageDeltaUpdateLogPayload& payload, WalErrorKind kind) {
    using namespace page_delta_update_log_layout;
    const auto fail = [&](const char* message) {
        throw WalError(kind, message);
    };
    if (payload.pageId == INVALID_PAGE_ID) fail("PAGE_DELTA_UPDATE PageId is invalid");
    if (payload.ranges.empty() || payload.ranges.size() > MAX_RANGE_COUNT) {
        fail("PAGE_DELTA_UPDATE range count is invalid");
    }
    std::size_t previousEnd = 0;
    bool first = true;
    for (const auto& range : payload.ranges) {
        const auto offset = static_cast<std::size_t>(range.offset);
        const auto length = static_cast<std::size_t>(range.length);
        if (length == 0 || offset >= database_format::PAGE_SIZE
            || length > database_format::PAGE_SIZE - offset) {
            fail("PAGE_DELTA_UPDATE range is outside the page");
        }
        if (!first && offset <= previousEnd) {
            fail("PAGE_DELTA_UPDATE ranges are overlapping, adjacent, or out of order");
        }
        if (range.beforeBytes.size() != length || range.afterBytes.size() != length) {
            fail("PAGE_DELTA_UPDATE range byte counts are inconsistent");
        }
        if (!payload.beforePageExisted
            && !std::all_of(range.beforeBytes.begin(), range.beforeBytes.end(),
                [](std::byte value) { return value == std::byte{0}; })) {
            fail("New-page PAGE_DELTA_UPDATE before bytes are not zero");
        }
        previousEnd = offset + length;
        first = false;
    }
}

std::size_t checkedAdd(std::size_t left, std::size_t right, const char* message) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw WalError(WalErrorKind::InvalidArgument, message);
    }
    return left + right;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right, const char* message) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw WalError(WalErrorKind::InvalidArgument, message);
    }
    return left * right;
}

} // namespace

std::size_t pageDeltaUpdatePayloadEncodedSize(
    const PageDeltaUpdateLogPayload& payload) {
    using namespace page_delta_update_log_layout;
    validateDeltaRanges(payload, WalErrorKind::InvalidArgument);
    std::size_t payloadSize = HEADER_SIZE;
    const auto copies = payload.beforePageExisted ? std::size_t{2} : std::size_t{1};
    for (const auto& range : payload.ranges) {
        const auto dataBytes = checkedMultiply(
            static_cast<std::size_t>(range.length), copies,
            "PAGE_DELTA_UPDATE size calculation overflowed");
        payloadSize = checkedAdd(
            payloadSize, RANGE_HEADER_SIZE,
            "PAGE_DELTA_UPDATE size calculation overflowed");
        payloadSize = checkedAdd(
            payloadSize, dataBytes,
            "PAGE_DELTA_UPDATE size calculation overflowed");
        if (payloadSize > wal_record_layout::MAX_PAYLOAD_SIZE) {
            throw WalError(WalErrorKind::InvalidArgument,
                           "PAGE_DELTA_UPDATE payload is too large");
        }
    }
    if (payloadSize > std::numeric_limits<std::uint32_t>::max()) {
        throw WalError(WalErrorKind::InvalidArgument,
                       "PAGE_DELTA_UPDATE payload length is not encodable");
    }
    return payloadSize;
}

std::size_t pageDeltaUpdateRecordEncodedSize(
    const PageDeltaUpdateLogPayload& payload) {
    const auto payloadSize = pageDeltaUpdatePayloadEncodedSize(payload);
    const auto recordSize = checkedAdd(
        wal_record_layout::HEADER_SIZE, payloadSize,
        "PAGE_DELTA_UPDATE record size calculation overflowed");
    if (recordSize > wal_record_layout::MAX_RECORD_SIZE
        || recordSize > std::numeric_limits<std::uint32_t>::max()) {
        throw WalError(WalErrorKind::InvalidArgument,
                       "PAGE_DELTA_UPDATE record length is not encodable");
    }
    return recordSize;
}

AdaptivePageUpdateDecision selectAdaptivePageUpdateEncoding(
    const PageDeltaUpdateLogPayload& deltaPayload) {
    const auto deltaBytes = pageDeltaUpdateRecordEncodedSize(deltaPayload);
    std::size_t changedBytes = 0;
    for (const auto& range : deltaPayload.ranges) {
        changedBytes = checkedAdd(
            changedBytes, static_cast<std::size_t>(range.length),
            "PAGE_DELTA_UPDATE changed-byte count overflowed");
    }
    return AdaptivePageUpdateDecision{
        deltaBytes < FULL_PAGE_UPDATE_RECORD_SIZE
            ? LogRecordType::PageDeltaUpdate : LogRecordType::PageUpdate,
        FULL_PAGE_UPDATE_RECORD_SIZE,
        deltaBytes,
        deltaPayload.ranges.size(),
        changedBytes,
    };
}

std::vector<std::byte> encodePageDeltaUpdateLogPayload(
    const PageDeltaUpdateLogPayload& payload) {
    using namespace page_delta_update_log_layout;
    const auto payloadSize = pageDeltaUpdatePayloadEncodedSize(payload);
    std::vector<std::byte> bytes(payloadSize);
    byte_codec::writeUint32(bytes, PAGE_ID_OFFSET, payload.pageId);
    byte_codec::writeUint32(
        bytes, FLAGS_OFFSET, payload.beforePageExisted ? BEFORE_PAGE_EXISTED : 0U);
    byte_codec::writeUint32(
        bytes, PAGE_SIZE_OFFSET, static_cast<std::uint32_t>(database_format::PAGE_SIZE));
    byte_codec::writeUint32(
        bytes, RANGE_COUNT_OFFSET, static_cast<std::uint32_t>(payload.ranges.size()));
    byte_codec::writeUint16(bytes, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint16(
        bytes, HEADER_SIZE_OFFSET, static_cast<std::uint16_t>(HEADER_SIZE));
    std::size_t cursor = HEADER_SIZE;
    for (const auto& range : payload.ranges) {
        byte_codec::writeUint16(bytes, cursor + RANGE_OFFSET_OFFSET, range.offset);
        byte_codec::writeUint16(bytes, cursor + RANGE_LENGTH_OFFSET, range.length);
        cursor += RANGE_HEADER_SIZE;
        if (payload.beforePageExisted) {
            std::copy(range.beforeBytes.begin(), range.beforeBytes.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            cursor += range.beforeBytes.size();
        }
        std::copy(range.afterBytes.begin(), range.afterBytes.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += range.afterBytes.size();
    }
    return bytes;
}

PageDeltaUpdateLogPayload decodePageDeltaUpdateLogPayload(
    std::span<const std::byte> bytes) {
    using namespace page_delta_update_log_layout;
    if (bytes.size() < HEADER_SIZE) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "PAGE_DELTA_UPDATE payload header is truncated");
    }
    const auto flags = byte_codec::readUint32(bytes, FLAGS_OFFSET);
    const auto rangeCount = byte_codec::readUint32(bytes, RANGE_COUNT_OFFSET);
    if ((flags & ~BEFORE_PAGE_EXISTED) != 0
        || byte_codec::readUint32(bytes, PAGE_SIZE_OFFSET) != database_format::PAGE_SIZE
        || byte_codec::readUint16(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint16(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(bytes, RESERVED_OFFSET) != 0
        || rangeCount == 0 || rangeCount > MAX_RANGE_COUNT) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "PAGE_DELTA_UPDATE header fields are malformed");
    }
    PageDeltaUpdateLogPayload payload;
    payload.pageId = byte_codec::readUint32(bytes, PAGE_ID_OFFSET);
    payload.beforePageExisted = (flags & BEFORE_PAGE_EXISTED) != 0;
    payload.ranges.reserve(rangeCount);
    std::size_t cursor = HEADER_SIZE;
    for (std::uint32_t index = 0; index < rangeCount; ++index) {
        if (bytes.size() - cursor < RANGE_HEADER_SIZE) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "PAGE_DELTA_UPDATE range header is truncated");
        }
        PageByteRange range;
        range.offset = byte_codec::readUint16(bytes, cursor + RANGE_OFFSET_OFFSET);
        range.length = byte_codec::readUint16(bytes, cursor + RANGE_LENGTH_OFFSET);
        cursor += RANGE_HEADER_SIZE;
        const auto length = static_cast<std::size_t>(range.length);
        const auto copies = payload.beforePageExisted ? 2U : 1U;
        if (length == 0 || length > database_format::PAGE_SIZE
            || cursor > bytes.size()
            || length > (bytes.size() - cursor) / copies) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "PAGE_DELTA_UPDATE range data is truncated or invalid");
        }
        if (payload.beforePageExisted) {
            range.beforeBytes.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor += length;
        } else {
            range.beforeBytes.assign(length, std::byte{0});
        }
        range.afterBytes.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
        cursor += length;
        payload.ranges.push_back(std::move(range));
    }
    if (cursor != bytes.size()) {
        throw WalError(WalErrorKind::CorruptRecord,
                       "PAGE_DELTA_UPDATE payload has trailing bytes");
    }
    validateDeltaRanges(payload, WalErrorKind::CorruptRecord);
    return payload;
}

void applyPageDeltaAfter(
    DiskManager::Page& page,
    const PageDeltaUpdateLogPayload& payload) {
    validateDeltaRanges(payload, WalErrorKind::CorruptRecord);
    for (const auto& range : payload.ranges) {
        std::copy(range.afterBytes.begin(), range.afterBytes.end(),
                  page.begin() + range.offset);
    }
}

void applyPageDeltaBefore(
    DiskManager::Page& page,
    const PageDeltaUpdateLogPayload& payload) {
    validateDeltaRanges(payload, WalErrorKind::CorruptRecord);
    for (const auto& range : payload.ranges) {
        std::copy(range.beforeBytes.begin(), range.beforeBytes.end(),
                  page.begin() + range.offset);
    }
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
    case LogRecordType::PageDeltaUpdate:
        if (!isValidLsn(record.prevLsn)) {
            throw WalError(WalErrorKind::CorruptRecord,
                           "PAGE_DELTA_UPDATE record lacks prevLSN");
        }
        static_cast<void>(decodePageDeltaUpdateLogPayload(record.payload));
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
