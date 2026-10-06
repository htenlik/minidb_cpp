#include "minidb/database_access_gate.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <utility>

namespace minidb {

DatabaseAccessGate::Lease::Lease(Lease&& other) noexcept
    : gate_(std::exchange(other.gate_, nullptr)), mode_(other.mode_), owner_(other.owner_) {}
DatabaseAccessGate::Lease& DatabaseAccessGate::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        reset();
        gate_ = std::exchange(other.gate_, nullptr);
        mode_ = other.mode_;
        owner_ = other.owner_;
    }
    return *this;
}
void DatabaseAccessGate::Lease::reset() noexcept {
    if (auto* gate = std::exchange(gate_, nullptr)) gate->release(mode_, owner_);
}
DatabaseAccessGate::Lease DatabaseAccessGate::acquireShared(const CancelProbe& cancelled) {
    return acquire(AccessMode::ReadOnly, cancelled);
}
DatabaseAccessGate::Lease DatabaseAccessGate::acquireExclusive(const CancelProbe& cancelled) {
    return acquire(AccessMode::ReadWrite, cancelled);
}

DatabaseAccessGate::Lease DatabaseAccessGate::acquire(AccessMode mode, const CancelProbe& cancelled) {
    const auto owner = std::this_thread::get_id();
    std::unique_lock lock(mutex_);
    const auto owned = owners_.find(owner);
    if (owned != owners_.end()
        && (mode == AccessMode::ReadWrite || owned->second.writer || stats_.waitingWriters != 0)) {
        throw std::logic_error("Database leases do not support upgrades or recursive exclusive access");
    }
    const auto eligible = [&] {
        return !stats_.writerActive && (mode == AccessMode::ReadOnly
            ? stats_.waitingWriters == 0 : stats_.activeReaders == 0);
    };
    auto& waiters = mode == AccessMode::ReadOnly ? stats_.waitingReaders : stats_.waitingWriters;
    auto& waits = mode == AccessMode::ReadOnly ? stats_.readerWaitCount : stats_.writerWaitCount;
    auto& waitNs = mode == AccessMode::ReadOnly ? stats_.readerWaitNs : stats_.writerWaitNs;
    const bool waiting = !eligible();
    const auto started = std::chrono::steady_clock::now();
    if (waiting) { ++waiters; ++waits; }
    const auto leaveWait = [&] {
        if (waiting) {
            --waiters;
            waitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        }
    };
    try {
        for (;;) {
            // Probes inspect cancellation/peer state only; never storage or gate state.
            if (stopped_ || (cancelled && cancelled())) throw AccessCancelled();
            if (eligible()) break;
            changed_.wait_for(lock, std::chrono::milliseconds(10));
        }
        auto& ownership = owners_[owner]; // Allocate before publishing the lease.
        leaveWait();
        if (mode == AccessMode::ReadOnly) {
            ++ownership.readers;
            ++stats_.activeReaders;
            ++stats_.sharedAcquisitions;
            stats_.peakConcurrentReaders = std::max(stats_.peakConcurrentReaders, stats_.activeReaders);
        } else {
            ownership.writer = true;
            stats_.writerActive = true;
            ++stats_.exclusiveAcquisitions;
        }
        return Lease(*this, mode, owner);
    } catch (...) {
        leaveWait();
        ++stats_.cancelledWaits;
        changed_.notify_all();
        throw;
    }
}

void DatabaseAccessGate::release(AccessMode mode, std::thread::id owner) noexcept {
    std::lock_guard lock(mutex_);
    auto found = owners_.find(owner);
    assert(found != owners_.end());
    if (mode == AccessMode::ReadOnly) {
        assert(found->second.readers > 0 && stats_.activeReaders > 0);
        --found->second.readers;
        --stats_.activeReaders;
    } else {
        assert(found->second.writer && stats_.writerActive);
        found->second.writer = false;
        stats_.writerActive = false;
    }
    if (found->second.readers == 0 && !found->second.writer) owners_.erase(found);
    changed_.notify_all();
}
void DatabaseAccessGate::shutdown() noexcept {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    changed_.notify_all();
}
DatabaseAccessStats DatabaseAccessGate::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}
void DatabaseAccessGate::resetStats() {
    std::lock_guard lock(mutex_);
    const auto readers = stats_.activeReaders;
    const auto writer = stats_.writerActive;
    const auto waitingReaders = stats_.waitingReaders;
    const auto waitingWriters = stats_.waitingWriters;
    stats_ = {};
    stats_.activeReaders = stats_.peakConcurrentReaders = readers;
    stats_.writerActive = writer;
    stats_.waitingReaders = waitingReaders;
    stats_.waitingWriters = waitingWriters;
}

} // namespace minidb
