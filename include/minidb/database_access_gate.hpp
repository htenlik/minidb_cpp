#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace minidb {

enum class AccessMode { ReadOnly, ReadWrite };

class AccessCancelled : public std::runtime_error {
public:
    AccessCancelled() : std::runtime_error("Database access cancelled") {}
};

struct DatabaseAccessStats {
    std::uint64_t activeReaders = 0;
    std::uint64_t peakConcurrentReaders = 0;
    bool writerActive = false;
    std::uint64_t waitingReaders = 0;
    std::uint64_t waitingWriters = 0;
    std::uint64_t sharedAcquisitions = 0;
    std::uint64_t exclusiveAcquisitions = 0;
    std::uint64_t readerWaitCount = 0;
    std::uint64_t writerWaitCount = 0;
    std::uint64_t readerWaitNs = 0;
    std::uint64_t writerWaitNs = 0;
    std::uint64_t cancelledWaits = 0;
};

// One logical database resource. Leases may span requests and move between owners;
// the gate mutex is held only while updating counters, never during database work.
class DatabaseAccessGate {
public:
    using CancelProbe = std::function<bool()>;
    class Lease {
    public:
        Lease() = default;
        ~Lease() { reset(); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return gate_ != nullptr; }
        [[nodiscard]] AccessMode mode() const noexcept { return mode_; }
        [[nodiscard]] bool owns(const DatabaseAccessGate& gate) const noexcept { return gate_ == &gate; }
    private:
        friend class DatabaseAccessGate;
        Lease(DatabaseAccessGate& gate, AccessMode mode, std::thread::id owner)
            : gate_(&gate), mode_(mode), owner_(owner) {}
        DatabaseAccessGate* gate_ = nullptr;
        AccessMode mode_ = AccessMode::ReadOnly;
        std::thread::id owner_{};
    };

    [[nodiscard]] Lease acquireShared(const CancelProbe& cancelled = {});
    [[nodiscard]] Lease acquireExclusive(const CancelProbe& cancelled = {});
    // Cancels new/waiting acquisitions. Existing owners retain their leases until
    // normal cleanup, including durable writer rollback, has completed.
    void shutdown() noexcept;
    [[nodiscard]] DatabaseAccessStats stats() const;
    void resetStats();

private:
    struct Ownership { std::uint64_t readers = 0; bool writer = false; };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    DatabaseAccessStats stats_{};
    std::unordered_map<std::thread::id, Ownership> owners_;
    bool stopped_ = false;
    [[nodiscard]] Lease acquire(AccessMode mode, const CancelProbe& cancelled);
    void release(AccessMode mode, std::thread::id owner) noexcept;
};

} // namespace minidb
