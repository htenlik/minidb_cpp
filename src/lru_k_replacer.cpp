#include "minidb/lru_k_replacer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace minidb {

bool LRUKReplacer::CandidateLess::operator()(
    const Candidate& left,
    const Candidate& right) const noexcept {
    if (left.infiniteDistance != right.infiniteDistance) {
        return left.infiniteDistance > right.infiniteDistance;
    }
    if (left.priorityTimestamp != right.priorityTimestamp) {
        return left.priorityTimestamp < right.priorityTimestamp;
    }
    return left.frameId < right.frameId;
}

LRUKReplacer::LRUKReplacer(std::size_t frameCapacity, std::size_t k)
    : k_(k), entries_(frameCapacity) {
    if (frameCapacity == 0) throw std::invalid_argument("LRU-K capacity must be positive");
    if (frameCapacity > std::numeric_limits<FrameId>::max()) {
        throw std::invalid_argument("LRU-K capacity exceeds FrameId range");
    }
    if (k == 0) throw std::invalid_argument("LRU-K K must be positive");
}

LRUKReplacer::Entry& LRUKReplacer::requireEntry(FrameId frameId) {
    if (frameId >= entries_.size()) throw std::out_of_range("FrameId exceeds LRU-K capacity");
    return entries_[frameId];
}

const LRUKReplacer::Entry& LRUKReplacer::requireEntry(FrameId frameId) const {
    if (frameId >= entries_.size()) throw std::out_of_range("FrameId exceeds LRU-K capacity");
    return entries_[frameId];
}

LRUKReplacer::Candidate LRUKReplacer::candidateFor(FrameId frameId) const {
    const auto& entry = requireEntry(frameId);
    if (!entry.tracked || entry.history.empty()) {
        throw std::logic_error("Untracked frame has no LRU-K candidate");
    }
    return Candidate{entry.history.size() < k_, entry.history.front(), frameId};
}

void LRUKReplacer::eraseCandidate(FrameId frameId) {
    const auto erased = candidates_.erase(candidateFor(frameId));
    if (erased != 1) throw std::logic_error("LRU-K candidate index is inconsistent");
}

void LRUKReplacer::insertCandidate(FrameId frameId) {
    const auto [position, inserted] = candidates_.insert(candidateFor(frameId));
    static_cast<void>(position);
    if (!inserted) throw std::logic_error("Duplicate LRU-K candidate");
}

void LRUKReplacer::recordAccess(FrameId frameId) {
    auto& entry = requireEntry(frameId);
    if (entry.evictable) eraseCandidate(frameId);
    if (currentTimestamp_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("LRU-K logical timestamp exhausted");
    }
    ++currentTimestamp_;
    if (!entry.tracked) {
        entry.tracked = true;
        ++trackedCount_;
    }
    entry.history.push_back(currentTimestamp_);
    if (entry.history.size() > k_) entry.history.pop_front();
    if (entry.evictable) insertCandidate(frameId);
}

void LRUKReplacer::setEvictable(FrameId frameId, bool evictable) {
    auto& entry = requireEntry(frameId);
    if (!entry.tracked) throw std::logic_error("Cannot change untracked frame evictability");
    if (entry.evictable == evictable) return;
    if (entry.evictable) eraseCandidate(frameId);
    entry.evictable = evictable;
    if (entry.evictable) insertCandidate(frameId);
}

std::optional<FrameId> LRUKReplacer::victim() const noexcept {
    if (candidates_.empty()) return std::nullopt;
    return candidates_.begin()->frameId;
}

std::optional<FrameId> LRUKReplacer::evict() {
    const auto frameId = victim();
    if (!frameId.has_value()) return std::nullopt;
    if (!remove(*frameId)) throw std::logic_error("LRU-K victim disappeared");
    return frameId;
}

bool LRUKReplacer::remove(FrameId frameId) {
    auto& entry = requireEntry(frameId);
    if (!entry.tracked) return false;
    if (!entry.evictable) throw std::logic_error("Cannot remove a non-evictable frame");
    eraseCandidate(frameId);
    entry = {};
    --trackedCount_;
    return true;
}

bool LRUKReplacer::isTracked(FrameId frameId) const {
    return requireEntry(frameId).tracked;
}

bool LRUKReplacer::isEvictable(FrameId frameId) const {
    return requireEntry(frameId).evictable;
}

void LRUKReplacer::validate() const {
    if (k_ == 0 || entries_.empty()) throw std::logic_error("Invalid LRU-K configuration");
    std::size_t tracked = 0;
    std::size_t evictable = 0;
    std::unordered_set<FrameId> indexed;
    for (FrameId frameId = 0; frameId < entries_.size(); ++frameId) {
        const auto& entry = entries_[frameId];
        if (!entry.tracked) {
            if (entry.evictable || !entry.history.empty()) {
                throw std::logic_error("Untracked LRU-K entry has state");
            }
            continue;
        }
        ++tracked;
        if (entry.history.empty() || entry.history.size() > k_) {
            throw std::logic_error("LRU-K history length is invalid");
        }
        if (!std::is_sorted(entry.history.begin(), entry.history.end())
            || entry.history.back() > currentTimestamp_ || entry.history.front() == 0) {
            throw std::logic_error("LRU-K history timestamps are invalid");
        }
        if (entry.evictable) {
            ++evictable;
            if (candidates_.count(candidateFor(frameId)) != 1) {
                throw std::logic_error("Evictable LRU-K frame is absent from candidate index");
            }
        }
    }
    for (const auto& candidate : candidates_) {
        if (candidate.frameId >= entries_.size()
            || !indexed.insert(candidate.frameId).second
            || !entries_[candidate.frameId].tracked
            || !entries_[candidate.frameId].evictable
            || CandidateLess{}(candidate, candidateFor(candidate.frameId))
            || CandidateLess{}(candidateFor(candidate.frameId), candidate)) {
            throw std::logic_error("LRU-K candidate index entry is inconsistent");
        }
    }
    if (tracked != trackedCount_ || evictable != candidates_.size()) {
        throw std::logic_error("LRU-K accounting is inconsistent");
    }
}

} // namespace minidb
