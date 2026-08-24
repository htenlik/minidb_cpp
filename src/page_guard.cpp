#include "minidb/page_guard.hpp"

#include "minidb/buffer_pool_manager.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace minidb {

BasicPageGuard::BasicPageGuard(
    BufferPoolManager& manager,
    FrameId frameId,
    PageId pageId) noexcept
    : manager_(&manager), frameId_(frameId), pageId_(pageId) {}

BasicPageGuard::~BasicPageGuard() {
    if (manager_ == nullptr) return;
    try {
        drop();
    } catch (...) {
        std::terminate();
    }
}

BasicPageGuard::BasicPageGuard(BasicPageGuard&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)),
      frameId_(std::exchange(other.frameId_, INVALID_FRAME_ID)),
      pageId_(std::exchange(other.pageId_, INVALID_PAGE_ID)) {}

BasicPageGuard& BasicPageGuard::operator=(BasicPageGuard&& other) {
    if (this == &other) return *this;
    drop();
    manager_ = std::exchange(other.manager_, nullptr);
    frameId_ = std::exchange(other.frameId_, INVALID_FRAME_ID);
    pageId_ = std::exchange(other.pageId_, INVALID_PAGE_ID);
    return *this;
}

void BasicPageGuard::drop() {
    if (manager_ == nullptr) return;
    auto* manager = std::exchange(manager_, nullptr);
    const auto frameId = std::exchange(frameId_, INVALID_FRAME_ID);
    pageId_ = INVALID_PAGE_ID;
    manager->releasePin(frameId);
}

std::span<const std::byte, database_format::PAGE_SIZE> BasicPageGuard::data() const {
    if (manager_ == nullptr) throw std::logic_error("Page guard no longer owns a pin");
    return manager_->readData(frameId_, pageId_);
}

std::span<std::byte, database_format::PAGE_SIZE> BasicPageGuard::mutableData() {
    if (manager_ == nullptr) throw std::logic_error("Page guard no longer owns a pin");
    return manager_->mutableData(frameId_, pageId_);
}

} // namespace minidb
