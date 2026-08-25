#include "minidb/buffer_pool_manager.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace minidb {

BufferPoolManager::BufferPoolManager(
    DiskManager& diskManager,
    std::size_t frameCount,
    std::size_t k,
    WalFlushProvider* walProvider,
    PageRecoveryHook* recoveryHook)
    : diskManager_(diskManager),
      frames_(frameCount),
      replacer_(frameCount, k),
      walProvider_(walProvider),
      recoveryHook_(recoveryHook) {
    if (frameCount == 0) throw std::invalid_argument("Buffer pool capacity must be positive");
    if (frameCount > std::numeric_limits<FrameId>::max()) {
        throw std::invalid_argument("Buffer pool capacity exceeds FrameId range");
    }
    pageTable_.reserve(frameCount);
    for (FrameId frameId = 0; frameId < frameCount; ++frameId) {
        freeFrames_.push_back(frameId);
    }
}

BufferPoolManager::~BufferPoolManager() {
    try {
        flushAll();
    } catch (...) {
    }
}

std::optional<FrameId> BufferPoolManager::availableFrame() const {
    if (!freeFrames_.empty()) return freeFrames_.front();
    return replacer_.victim();
}

void BufferPoolManager::flushVictimIfDirty(FrameId frameId) {
    auto& frame = frames_.at(frameId);
    if (!frame.valid) return;
    if (frame.pinCount != 0) {
        throw std::logic_error("Selected buffer victim is not evictable");
    }
    if (!frame.dirty) return;
    prepareFrameForWrite(frame);
    ensureWalDurableBeforePageWrite(frame);
    diskManager_.writePage(frame.pageId, frame.data);
    frame.dirty = false;
    ++stats_.physicalPageWrites;
}

void BufferPoolManager::prepareFrameForWrite(BufferFrame& frame) {
    if (recoveryHook_ == nullptr) return;
    const auto lsn = recoveryHook_->preparePageForWrite(frame.pageId, frame.data);
    if (isValidLsn(lsn)) frame.pageLsn = lsn;
}

void BufferPoolManager::ensureWalDurable(Lsn pageLsn) {
    if (!isValidLsn(pageLsn)) return;
    if (walProvider_ == nullptr) {
        throw std::logic_error("A page has a valid pageLSN without a WAL provider");
    }
    if (!walProvider_->containsLsn(pageLsn)) {
        throw std::logic_error("A pageLSN no longer identifies a known WAL record");
    }
    const auto durable = walProvider_->durableLsn();
    if (isValidLsn(durable) && durable >= pageLsn) return;
    ++stats_.walFlushRequests;
    walProvider_->flushUpTo(pageLsn);
}

void BufferPoolManager::ensureWalDurableBeforePageWrite(const BufferFrame& frame) {
    ensureWalDurable(frame.pageLsn);
}

void BufferPoolManager::installPage(
    FrameId frameId,
    PageId pageId,
    DiskManager::Page page,
    bool dirty) {
    auto& frame = frames_.at(frameId);
    const bool wasValid = frame.valid;
    const bool wasDirty = wasValid && frame.dirty;
    if (wasDirty) throw std::logic_error("Dirty victim was not flushed before reuse");

    if (wasValid) {
        if (!replacer_.remove(frameId)) {
            throw std::logic_error("Victim frame was absent from LRU-K replacer");
        }
        if (pageTable_.erase(frame.pageId) != 1) {
            throw std::logic_error("Victim frame was absent from page table");
        }
        ++stats_.evictions;
    } else {
        if (freeFrames_.empty() || freeFrames_.front() != frameId) {
            throw std::logic_error("Free-frame queue is inconsistent");
        }
        freeFrames_.pop_front();
    }

    frame.data = std::move(page);
    frame.pageId = pageId;
    frame.pinCount = 1;
    frame.dirty = dirty;
    frame.valid = true;
    frame.pageLsn = INVALID_LSN;
    const auto [position, inserted] = pageTable_.emplace(pageId, frameId);
    static_cast<void>(position);
    if (!inserted) throw std::logic_error("Page already exists in buffer page table");
    replacer_.recordAccess(frameId);
    replacer_.setEvictable(frameId, false);
    ++stats_.pinOperations;
}

std::optional<BasicPageGuard> BufferPoolManager::fetchPage(PageId pageId, bool writable) {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata");
    }
    if (pageId == INVALID_PAGE_ID || pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("Page ID does not exist");
    }

    ++stats_.pageRequests;
    if (const auto found = pageTable_.find(pageId); found != pageTable_.end()) {
        ++stats_.cacheHits;
        auto& frame = frames_[found->second];
        if (frame.pinCount == std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("Buffer frame pin count exhausted");
        }
        if (frame.pinCount == 0) replacer_.setEvictable(found->second, false);
        ++frame.pinCount;
        replacer_.recordAccess(found->second);
        if (writable) {
            if (recoveryHook_ != nullptr) {
                recoveryHook_->notePageWriteIntent(pageId, frame.data);
            }
            frame.dirty = true;
        }
        ++stats_.pinOperations;
        return BasicPageGuard(*this, found->second, pageId);
    }

    ++stats_.cacheMisses;
    const auto frameId = availableFrame();
    if (!frameId.has_value()) return std::nullopt;
    const bool dirtyVictim = frames_[*frameId].valid && frames_[*frameId].dirty;
    flushVictimIfDirty(*frameId);
    DiskManager::Page page{};
    diskManager_.readPage(pageId, page);
    ++stats_.physicalPageReads;
    if (writable && recoveryHook_ != nullptr) {
        recoveryHook_->notePageWriteIntent(pageId, page);
    }
    installPage(*frameId, pageId, std::move(page), writable);
    if (dirtyVictim) ++stats_.dirtyEvictions;
    return BasicPageGuard(*this, *frameId, pageId);
}

std::optional<ReadPageGuard> BufferPoolManager::fetchPageRead(PageId pageId) {
    auto guard = fetchPage(pageId, false);
    if (!guard.has_value()) return std::nullopt;
    return ReadPageGuard(std::move(*guard));
}

std::optional<WritePageGuard> BufferPoolManager::fetchPageWrite(PageId pageId) {
    auto guard = fetchPage(pageId, true);
    if (!guard.has_value()) return std::nullopt;
    return WritePageGuard(std::move(*guard));
}

std::optional<WritePageGuard> BufferPoolManager::newPageWrite() {
    const auto frameId = availableFrame();
    if (!frameId.has_value()) return std::nullopt;
    const bool dirtyVictim = frames_[*frameId].valid && frames_[*frameId].dirty;
    flushVictimIfDirty(*frameId);
    const auto pageId = diskManager_.appendPage();
    DiskManager::Page page{};
    if (recoveryHook_ != nullptr) recoveryHook_->notePageWriteIntent(pageId, page);
    installPage(*frameId, pageId, std::move(page), true);
    if (dirtyVictim) ++stats_.dirtyEvictions;
    ++stats_.appendedPages;
    return WritePageGuard(BasicPageGuard(*this, *frameId, pageId));
}

bool BufferPoolManager::flushPage(PageId pageId) {
    if (pageId == database_format::METADATA_PAGE_ID) {
        throw std::invalid_argument("Page 0 is reserved for database metadata");
    }
    if (pageId == INVALID_PAGE_ID || pageId >= diskManager_.pageCount()) {
        throw std::out_of_range("Page ID does not exist");
    }
    const auto found = pageTable_.find(pageId);
    if (found == pageTable_.end()) return false;
    auto& frame = frames_[found->second];
    if (frame.dirty) {
        prepareFrameForWrite(frame);
        ensureWalDurableBeforePageWrite(frame);
        diskManager_.writePage(pageId, frame.data);
        frame.dirty = false;
        ++stats_.physicalPageWrites;
    }
    return true;
}

void BufferPoolManager::flushAll() {
    for (auto& frame : frames_) {
        if (frame.valid && frame.dirty) prepareFrameForWrite(frame);
    }
    Lsn maximumPageLsn = INVALID_LSN;
    for (const auto& frame : frames_) {
        if (!frame.valid || !frame.dirty || !isValidLsn(frame.pageLsn)) continue;
        if (!isValidLsn(maximumPageLsn) || frame.pageLsn > maximumPageLsn) {
            maximumPageLsn = frame.pageLsn;
        }
    }
    ensureWalDurable(maximumPageLsn);
    for (auto& frame : frames_) {
        if (!frame.valid || !frame.dirty) continue;
        diskManager_.writePage(frame.pageId, frame.data);
        frame.dirty = false;
        ++stats_.physicalPageWrites;
    }
    diskManager_.flush();
}

std::optional<DiskManager::Page> BufferPoolManager::residentPageCopy(PageId pageId) const {
    const auto found = pageTable_.find(pageId);
    if (found == pageTable_.end()) return std::nullopt;
    return frames_[found->second].data;
}

std::uint64_t BufferPoolManager::totalPinCount() const noexcept {
    std::uint64_t total = 0;
    for (const auto& frame : frames_) total += frame.valid ? frame.pinCount : 0;
    return total;
}

void BufferPoolManager::discardPageForRecovery(PageId pageId) {
    const auto found = pageTable_.find(pageId);
    if (found == pageTable_.end()) return;
    const auto frameId = found->second;
    auto& frame = frames_[frameId];
    if (frame.pinCount != 0) {
        throw std::logic_error("Cannot discard a pinned buffer page during recovery");
    }
    if (!replacer_.remove(frameId)) {
        throw std::logic_error("Recovery discard could not remove buffer frame");
    }
    pageTable_.erase(found);
    frame = {};
    freeFrames_.push_back(frameId);
}

void BufferPoolManager::discardPagesAtOrAboveForRecovery(PageId firstPageId) {
    std::vector<PageId> pages;
    for (const auto& [pageId, frameId] : pageTable_) {
        static_cast<void>(frameId);
        if (pageId >= firstPageId) pages.push_back(pageId);
    }
    for (const auto pageId : pages) discardPageForRecovery(pageId);
}

void BufferPoolManager::releasePin(FrameId frameId) {
    auto& frame = frames_.at(frameId);
    if (!frame.valid || frame.pinCount == 0) {
        throw std::logic_error("Buffer frame pin count underflow");
    }
    --frame.pinCount;
    ++stats_.unpinOperations;
    if (frame.pinCount == 0) replacer_.setEvictable(frameId, true);
}

BufferFrame& BufferPoolManager::requireGuardFrame(FrameId frameId, PageId pageId) {
    auto& frame = frames_.at(frameId);
    if (!frame.valid || frame.pageId != pageId || frame.pinCount == 0) {
        throw std::logic_error("Page guard no longer identifies a pinned frame");
    }
    return frame;
}

const BufferFrame& BufferPoolManager::requireGuardFrame(FrameId frameId, PageId pageId) const {
    const auto& frame = frames_.at(frameId);
    if (!frame.valid || frame.pageId != pageId || frame.pinCount == 0) {
        throw std::logic_error("Page guard no longer identifies a pinned frame");
    }
    return frame;
}

std::span<const std::byte, database_format::PAGE_SIZE> BufferPoolManager::readData(
    FrameId frameId,
    PageId pageId) const {
    return requireGuardFrame(frameId, pageId).data;
}

std::span<std::byte, database_format::PAGE_SIZE> BufferPoolManager::mutableData(
    FrameId frameId,
    PageId pageId) {
    auto& frame = requireGuardFrame(frameId, pageId);
    frame.dirty = true;
    return frame.data;
}

BufferPoolStats BufferPoolManager::stats() const noexcept {
    auto result = stats_;
    result.residentPages = pageTable_.size();
    result.evictableFrames = replacer_.size();
    result.capacity = frames_.size();
    result.pinnedFrames = static_cast<std::uint64_t>(std::count_if(
        frames_.begin(), frames_.end(), [](const BufferFrame& frame) {
            return frame.valid && frame.pinCount > 0;
        }));
    return result;
}

bool BufferPoolManager::isResident(PageId pageId) const noexcept {
    return pageTable_.contains(pageId);
}

std::optional<FrameId> BufferPoolManager::frameIdForPage(PageId pageId) const noexcept {
    const auto found = pageTable_.find(pageId);
    return found == pageTable_.end() ? std::nullopt : std::optional<FrameId>(found->second);
}

std::optional<std::uint32_t> BufferPoolManager::pinCount(PageId pageId) const noexcept {
    const auto frameId = frameIdForPage(pageId);
    return frameId.has_value() ? std::optional<std::uint32_t>(frames_[*frameId].pinCount)
                               : std::nullopt;
}

std::optional<bool> BufferPoolManager::isDirty(PageId pageId) const noexcept {
    const auto frameId = frameIdForPage(pageId);
    return frameId.has_value() ? std::optional<bool>(frames_[*frameId].dirty) : std::nullopt;
}

std::optional<Lsn> BufferPoolManager::pageLsn(PageId pageId) const noexcept {
    const auto frameId = frameIdForPage(pageId);
    return frameId.has_value() ? std::optional<Lsn>(frames_[*frameId].pageLsn) : std::nullopt;
}

Lsn BufferPoolManager::guardPageLsn(FrameId frameId, PageId pageId) const {
    return requireGuardFrame(frameId, pageId).pageLsn;
}

void BufferPoolManager::setPageLsn(FrameId frameId, PageId pageId, Lsn pageLsn) {
    auto& frame = requireGuardFrame(frameId, pageId);
    if (!isValidLsn(pageLsn)) {
        throw std::invalid_argument("WritePageGuard cannot assign INVALID_LSN");
    }
    if (walProvider_ == nullptr || !walProvider_->containsLsn(pageLsn)) {
        throw std::invalid_argument("pageLSN must identify a record appended by the WAL provider");
    }
    if (isValidLsn(frame.pageLsn) && pageLsn < frame.pageLsn) {
        throw std::invalid_argument("pageLSN cannot move backward");
    }
    frame.pageLsn = pageLsn;
}

void BufferPoolManager::validate() const {
    replacer_.validate();
    if (pageTable_.size() > frames_.size()) {
        throw std::logic_error("Buffer pool exceeds configured capacity");
    }
    std::unordered_set<FrameId> freeSet;
    for (const auto frameId : freeFrames_) {
        if (frameId >= frames_.size() || !freeSet.insert(frameId).second) {
            throw std::logic_error("Free-frame queue contains invalid or duplicate frame");
        }
    }
    std::unordered_set<PageId> pages;
    std::size_t validCount = 0;
    for (FrameId frameId = 0; frameId < frames_.size(); ++frameId) {
        const auto& frame = frames_[frameId];
        if (!frame.valid) {
            if (frame.pageId != INVALID_PAGE_ID || frame.pinCount != 0 || frame.dirty
                || isValidLsn(frame.pageLsn)
                || !freeSet.contains(frameId) || replacer_.isTracked(frameId)) {
                throw std::logic_error("Invalid/free buffer frame has live state");
            }
            continue;
        }
        ++validCount;
        if (frame.pageId == database_format::METADATA_PAGE_ID
            || frame.pageId == INVALID_PAGE_ID || frame.pageId >= diskManager_.pageCount()
            || !pages.insert(frame.pageId).second) {
            throw std::logic_error("Valid buffer frame has illegal or duplicate PageId");
        }
        const auto mapped = pageTable_.find(frame.pageId);
        if (mapped == pageTable_.end() || mapped->second != frameId
            || freeSet.contains(frameId) || !replacer_.isTracked(frameId)) {
            throw std::logic_error("Buffer frame/page-table/replacer mapping is inconsistent");
        }
        if ((frame.pinCount > 0 && replacer_.isEvictable(frameId))
            || (frame.pinCount == 0 && !replacer_.isEvictable(frameId))) {
            throw std::logic_error("Buffer pin count disagrees with replacer evictability");
        }
        if (isValidLsn(frame.pageLsn)
            && (walProvider_ == nullptr || !walProvider_->containsLsn(frame.pageLsn))) {
            throw std::logic_error("Buffer frame pageLSN is unknown to its WAL provider");
        }
    }
    for (const auto& [pageId, frameId] : pageTable_) {
        if (frameId >= frames_.size() || !frames_[frameId].valid
            || frames_[frameId].pageId != pageId) {
            throw std::logic_error("Page table points to an invalid buffer frame");
        }
    }
    if (validCount != pageTable_.size() || validCount + freeSet.size() != frames_.size()) {
        throw std::logic_error("Buffer resident/free frame accounting is inconsistent");
    }
    const auto snapshot = stats();
    if (snapshot.residentPages != validCount
        || snapshot.evictableFrames != replacer_.size()
        || snapshot.capacity != frames_.size()) {
        throw std::logic_error("Buffer statistics gauges are inconsistent");
    }
}

} // namespace minidb
