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

[[nodiscard]] std::vector<std::byte> encodeBeginLogPayload(BeginLogPayload payload);
[[nodiscard]] BeginLogPayload decodeBeginLogPayload(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encodePageUpdateLogPayload(
    const PageUpdateLogPayload& payload);
[[nodiscard]] PageUpdateLogPayload decodePageUpdateLogPayload(
    std::span<const std::byte> bytes);

void validateTransactionRecordPayload(const LogRecord& record);

} // namespace minidb
