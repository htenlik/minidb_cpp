#pragma once

#include "minidb/buffer_pool_types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <vector>

namespace minidb {

// Single-threaded LRU-K replacement bookkeeping over FrameIds. Candidate ordering is
// maintained incrementally, so ordinary policy operations are logarithmic in frames.
class LRUKReplacer {
public:
    explicit LRUKReplacer(std::size_t frameCapacity, std::size_t k = 2);

    void recordAccess(FrameId frameId);
    void setEvictable(FrameId frameId, bool evictable);
    [[nodiscard]] std::optional<FrameId> victim() const noexcept;
    [[nodiscard]] std::optional<FrameId> evict();
    [[nodiscard]] bool remove(FrameId frameId);

    [[nodiscard]] std::size_t size() const noexcept { return candidates_.size(); }
    [[nodiscard]] std::size_t trackedSize() const noexcept { return trackedCount_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t k() const noexcept { return k_; }
    [[nodiscard]] bool isTracked(FrameId frameId) const;
    [[nodiscard]] bool isEvictable(FrameId frameId) const;
    void validate() const;

private:
    struct Entry {
        std::deque<std::uint64_t> history;
        bool tracked = false;
        bool evictable = false;
    };

    struct Candidate {
        bool infiniteDistance = false;
        std::uint64_t priorityTimestamp = 0;
        FrameId frameId = INVALID_FRAME_ID;
    };

    struct CandidateLess {
        bool operator()(const Candidate& left, const Candidate& right) const noexcept;
    };

    std::size_t k_;
    std::uint64_t currentTimestamp_ = 0;
    std::size_t trackedCount_ = 0;
    std::vector<Entry> entries_;
    std::set<Candidate, CandidateLess> candidates_;

    [[nodiscard]] Entry& requireEntry(FrameId frameId);
    [[nodiscard]] const Entry& requireEntry(FrameId frameId) const;
    [[nodiscard]] Candidate candidateFor(FrameId frameId) const;
    void eraseCandidate(FrameId frameId);
    void insertCandidate(FrameId frameId);
};

} // namespace minidb
