#pragma once

#include "minidb/buffer_pool_manager.hpp"
#include "minidb/storage_error.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace minidb {

[[nodiscard]] inline ReadPageGuard requireReadPage(
    BufferPoolManager& bufferPool,
    PageId pageId,
    std::string_view operation) {
    auto guard = bufferPool.fetchPageRead(pageId);
    if (!guard.has_value()) {
        throw StorageError(
            StorageErrorKind::NoFrameAvailable,
            std::string(operation) + ": no buffer frame is available");
    }
    return std::move(*guard);
}

[[nodiscard]] inline WritePageGuard requireWritePage(
    BufferPoolManager& bufferPool,
    PageId pageId,
    std::string_view operation) {
    auto guard = bufferPool.fetchPageWrite(pageId);
    if (!guard.has_value()) {
        throw StorageError(
            StorageErrorKind::NoFrameAvailable,
            std::string(operation) + ": no buffer frame is available");
    }
    return std::move(*guard);
}

[[nodiscard]] inline WritePageGuard requireNewPage(
    BufferPoolManager& bufferPool,
    std::string_view operation) {
    auto guard = bufferPool.newPageWrite();
    if (!guard.has_value()) {
        throw StorageError(
            StorageErrorKind::NoFrameAvailable,
            std::string(operation) + ": no buffer frame is available");
    }
    return std::move(*guard);
}

} // namespace minidb
