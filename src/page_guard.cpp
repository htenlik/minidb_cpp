#include "minidb/page_guard.hpp"

#include "minidb/buffer_pool_manager.hpp"

#include <exception>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace minidb {
namespace {
// std::shared_mutex is not recursively lockable, even in shared mode. Multiple
// read guards on one frame in the same thread share one content lock while
// retaining separate pins. Guards must be acquired/released on the same thread.
using SharedContentLock = std::shared_lock<std::shared_mutex>;
thread_local std::unordered_map<std::shared_mutex*, std::weak_ptr<SharedContentLock>> readLocks;
thread_local std::unordered_set<std::shared_mutex*> writeLocks;
}

BasicPageGuard::BasicPageGuard(
    BufferPoolManager& manager,
    FrameId frameId,
    PageId pageId,
    bool writable)
    : manager_(&manager), frameId_(frameId), pageId_(pageId), ownerThread_(std::this_thread::get_id()) {
    try {
        auto& latch = *manager.frameLatches_.at(frameId);
        if (writeLocks.contains(&latch)) {
            throw std::logic_error("Frame content leases are not recursively writable");
        }
        if (writable) {
            if (const auto found = readLocks.find(&latch);
                found != readLocks.end() && !found->second.expired()) {
                throw std::logic_error("Release read guards before acquiring write access to the same frame");
            }
            auto& lock = latch_.emplace<2>(latch, std::defer_lock);
            if (!lock.try_lock()) {
                { std::lock_guard metadata(manager.metadataLatch_); ++manager.stats_.contentLatchWaits; }
                lock.lock();
            }
            writeLocks.insert(&latch);
            manager.noteGuardWriteIntent(frameId, pageId);
        } else {
            if (auto existing = readLocks[&latch].lock()) {
                latch_.emplace<1>(std::move(existing));
                return;
            }
            auto lock = std::make_shared<SharedContentLock>(latch, std::defer_lock);
            if (!lock->try_lock()) {
                { std::lock_guard metadata(manager.metadataLatch_); ++manager.stats_.contentLatchWaits; }
                lock->lock();
            }
            readLocks[&latch] = lock;
            latch_.emplace<1>(std::move(lock));
        }
    } catch (...) {
        if (auto* lock = std::get_if<2>(&latch_); lock != nullptr && lock->owns_lock()) {
            writeLocks.erase(lock->mutex());
        }
        latch_.emplace<0>();
        manager.releasePin(frameId);
        manager_ = nullptr;
        throw;
    }
}

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
      pageId_(std::exchange(other.pageId_, INVALID_PAGE_ID)), ownerThread_(other.ownerThread_),
      latch_(std::move(other.latch_)) {}

BasicPageGuard& BasicPageGuard::operator=(BasicPageGuard&& other) {
    if (this == &other) return *this;
    drop();
    manager_ = std::exchange(other.manager_, nullptr);
    frameId_ = std::exchange(other.frameId_, INVALID_FRAME_ID);
    pageId_ = std::exchange(other.pageId_, INVALID_PAGE_ID);
    ownerThread_ = other.ownerThread_;
    latch_ = std::move(other.latch_);
    return *this;
}

void BasicPageGuard::drop() {
    if (manager_ == nullptr) return;
    if (ownerThread_ != std::this_thread::get_id()) {
        throw std::logic_error("Page content guards must be released on the thread that acquired them");
    }
    if (auto* lock = std::get_if<1>(&latch_); lock != nullptr && lock->use_count() == 1) {
        readLocks.erase((*lock)->mutex());
    }
    if (auto* lock = std::get_if<2>(&latch_); lock != nullptr) writeLocks.erase(lock->mutex());
    latch_.emplace<0>(); // Unlock contents before touching pin/replacer metadata.
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

Lsn BasicPageGuard::pageLsn() const {
    if (manager_ == nullptr) throw std::logic_error("Page guard no longer owns a pin");
    return manager_->guardPageLsn(frameId_, pageId_);
}

void BasicPageGuard::setPageLsn(Lsn pageLsn) {
    if (manager_ == nullptr) throw std::logic_error("Page guard no longer owns a pin");
    manager_->setPageLsn(frameId_, pageId_, pageLsn);
}

} // namespace minidb
