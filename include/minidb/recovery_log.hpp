#pragma once

#include "minidb/disk_manager.hpp"
#include "minidb/wal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace minidb {

namespace begin_log_layout {

inline constexpr std::size_t START_PAGE_COUNT_OFFSET = 0;
inline constexpr std::size_t RESERVED_OFFSET = 8;
inline constexpr std::size_t PAYLOAD_SIZE = 16;

} // namespace begin_log_layout

namespace page_update_log_layout {

inline constexpr std::uint32_t BEFORE_PAGE_EXISTED = 1U;
inline constexpr std::size_t PAGE_ID_OFFSET = 0;
inline constexpr std::size_t FLAGS_OFFSET = 4;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 8;
inline constexpr std::size_t RESERVED_OFFSET = 12;
inline constexpr std::size_t BEFORE_IMAGE_OFFSET = 16;
inline constexpr std::size_t AFTER_IMAGE_OFFSET =
    BEFORE_IMAGE_OFFSET + database_format::PAGE_SIZE;
inline constexpr std::size_t PAYLOAD_SIZE =
    AFTER_IMAGE_OFFSET + database_format::PAGE_SIZE;

static_assert(PAYLOAD_SIZE == 8208);

} // namespace page_update_log_layout

namespace page_delta_update_log_layout {

inline constexpr std::uint32_t BEFORE_PAGE_EXISTED = 1U;
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t PAGE_ID_OFFSET = 0;
inline constexpr std::size_t FLAGS_OFFSET = 4;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 8;
inline constexpr std::size_t RANGE_COUNT_OFFSET = 12;
inline constexpr std::size_t VERSION_OFFSET = 16;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 18;
inline constexpr std::size_t RESERVED_OFFSET = 20;
inline constexpr std::size_t HEADER_SIZE = 24;
inline constexpr std::size_t RANGE_OFFSET_OFFSET = 0;
inline constexpr std::size_t RANGE_LENGTH_OFFSET = 2;
inline constexpr std::size_t RANGE_HEADER_SIZE = 4;
inline constexpr std::size_t MAX_RANGE_COUNT =
    (database_format::PAGE_SIZE + 1U) / 2U;

static_assert(database_format::PAGE_SIZE <= 65535U);

} // namespace page_delta_update_log_layout

namespace page_update_v2_log_layout {

inline constexpr std::uint32_t BEFORE_PAGE_EXISTED = 1U;
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t PAGE_ID_OFFSET = 0;
inline constexpr std::size_t FLAGS_OFFSET = 4;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 8;
inline constexpr std::size_t VERSION_OFFSET = 12;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 14;
inline constexpr std::size_t BEFORE_PAGE_LSN_OFFSET = 16;
inline constexpr std::size_t HEADER_SIZE = 24;
inline constexpr std::size_t BEFORE_IMAGE_OFFSET = HEADER_SIZE;
inline constexpr std::size_t AFTER_IMAGE_OFFSET =
    BEFORE_IMAGE_OFFSET + database_format::PAGE_SIZE;
inline constexpr std::size_t PAYLOAD_SIZE =
    AFTER_IMAGE_OFFSET + database_format::PAGE_SIZE;

static_assert(PAYLOAD_SIZE == 8216);

} // namespace page_update_v2_log_layout

namespace page_delta_update_v2_log_layout {

inline constexpr std::uint32_t BEFORE_PAGE_EXISTED = 1U;
inline constexpr std::uint16_t CURRENT_VERSION = 1;
inline constexpr std::size_t PAGE_ID_OFFSET = 0;
inline constexpr std::size_t FLAGS_OFFSET = 4;
inline constexpr std::size_t PAGE_SIZE_OFFSET = 8;
inline constexpr std::size_t RANGE_COUNT_OFFSET = 12;
inline constexpr std::size_t VERSION_OFFSET = 16;
inline constexpr std::size_t HEADER_SIZE_OFFSET = 18;
inline constexpr std::size_t RESERVED_OFFSET = 20;
inline constexpr std::size_t BEFORE_PAGE_LSN_OFFSET = 24;
inline constexpr std::size_t HEADER_SIZE = 32;
inline constexpr std::size_t RANGE_OFFSET_OFFSET = 0;
inline constexpr std::size_t RANGE_LENGTH_OFFSET = 2;
inline constexpr std::size_t RANGE_HEADER_SIZE = 4;
inline constexpr std::size_t MAX_RANGE_COUNT =
    page_delta_update_log_layout::MAX_RANGE_COUNT;

} // namespace page_delta_update_v2_log_layout

struct BeginLogPayload {
    std::uint64_t startPageCount = 0;
    bool operator==(const BeginLogPayload&) const = default;
};

struct PageUpdateLogPayload {
    PageId pageId = INVALID_PAGE_ID;
    bool beforePageExisted = false;
    DiskManager::Page beforeImage{};
    DiskManager::Page afterImage{};
    bool operator==(const PageUpdateLogPayload&) const = default;
};

struct PageByteRange {
    std::uint16_t offset = 0;
    std::uint16_t length = 0;
    std::vector<std::byte> beforeBytes;
    std::vector<std::byte> afterBytes;
    bool operator==(const PageByteRange&) const = default;
};

struct PageDeltaUpdateLogPayload {
    PageId pageId = INVALID_PAGE_ID;
    bool beforePageExisted = false;
    std::vector<PageByteRange> ranges;
    bool operator==(const PageDeltaUpdateLogPayload&) const = default;
};

struct PageUpdateV2LogPayload {
    PageId pageId = INVALID_PAGE_ID;
    bool beforePageExisted = false;
    Lsn beforePageLsn = INVALID_LSN;
    DiskManager::Page beforeImage{};
    DiskManager::Page afterImage{};
    bool operator==(const PageUpdateV2LogPayload&) const = default;
};

struct PageDeltaUpdateV2LogPayload {
    PageId pageId = INVALID_PAGE_ID;
    bool beforePageExisted = false;
    Lsn beforePageLsn = INVALID_LSN;
    std::vector<PageByteRange> ranges;
    bool operator==(const PageDeltaUpdateV2LogPayload&) const = default;
};

inline constexpr std::size_t FULL_PAGE_UPDATE_RECORD_SIZE =
    wal_record_layout::HEADER_SIZE + page_update_log_layout::PAYLOAD_SIZE;
inline constexpr std::size_t FULL_PAGE_UPDATE_V2_RECORD_SIZE =
    wal_record_layout::HEADER_SIZE + page_update_v2_log_layout::PAYLOAD_SIZE;

struct AdaptivePageUpdateDecision {
    LogRecordType recordType = LogRecordType::PageUpdate;
    std::size_t fullPageRecordBytes = 0;
    std::size_t deltaRecordBytes = 0;
    std::size_t rangeCount = 0;
    std::size_t changedByteCount = 0;

    [[nodiscard]] bool isTie() const noexcept {
        return fullPageRecordBytes == deltaRecordBytes;
    }
};

[[nodiscard]] std::vector<std::byte> encodeBeginLogPayload(BeginLogPayload payload);
[[nodiscard]] BeginLogPayload decodeBeginLogPayload(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodePageUpdateLogPayload(
    const PageUpdateLogPayload& payload);
[[nodiscard]] PageUpdateLogPayload decodePageUpdateLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodePageUpdateV2LogPayload(
    const PageUpdateV2LogPayload& payload);
[[nodiscard]] PageUpdateV2LogPayload decodePageUpdateV2LogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<PageByteRange> computePageDelta(
    const DiskManager::Page& before,
    const DiskManager::Page& after);
[[nodiscard]] std::vector<PageByteRange> computePageDelta(
    const DiskManager::Page& before,
    const DiskManager::Page& after,
    std::span<const bool> requiredOffsets);
[[nodiscard]] std::vector<std::byte> encodePageDeltaUpdateLogPayload(
    const PageDeltaUpdateLogPayload& payload);
[[nodiscard]] std::size_t pageDeltaUpdatePayloadEncodedSize(
    const PageDeltaUpdateLogPayload& payload);
[[nodiscard]] std::size_t pageDeltaUpdateRecordEncodedSize(
    const PageDeltaUpdateLogPayload& payload);
[[nodiscard]] AdaptivePageUpdateDecision selectAdaptivePageUpdateEncoding(
    const PageDeltaUpdateLogPayload& deltaPayload);
[[nodiscard]] PageDeltaUpdateLogPayload decodePageDeltaUpdateLogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodePageDeltaUpdateV2LogPayload(
    const PageDeltaUpdateV2LogPayload& payload);
[[nodiscard]] PageDeltaUpdateV2LogPayload decodePageDeltaUpdateV2LogPayload(
    std::span<const std::byte> bytes);
[[nodiscard]] std::size_t pageDeltaUpdateV2PayloadEncodedSize(
    const PageDeltaUpdateV2LogPayload& payload);
[[nodiscard]] std::size_t pageDeltaUpdateV2RecordEncodedSize(
    const PageDeltaUpdateV2LogPayload& payload);
[[nodiscard]] AdaptivePageUpdateDecision selectAdaptivePageUpdateV2Encoding(
    const PageDeltaUpdateV2LogPayload& deltaPayload);
void applyPageDeltaAfter(
    DiskManager::Page& page,
    const PageDeltaUpdateLogPayload& payload);
void applyPageDeltaBefore(
    DiskManager::Page& page,
    const PageDeltaUpdateLogPayload& payload);

void validateTransactionRecordPayload(const LogRecord& record);

} // namespace minidb
