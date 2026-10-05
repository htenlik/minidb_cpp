#pragma once

#include "minidb/recovery.hpp"

#include <cstdint>
#include <optional>

namespace minidb {

class CheckpointManager;
using SessionId = std::uint64_t;
inline constexpr SessionId LOCAL_SESSION_ID = 0;

enum class TransactionState { Idle, Active };

struct ExplicitTransactionStats {
    std::uint64_t explicitTransactionsBegun = 0;
    std::uint64_t explicitTransactionsCommitted = 0;
    std::uint64_t explicitTransactionsRolledBack = 0;
    std::uint64_t explicitTransactionsAutoRolledBackOnError = 0;
    std::uint64_t implicitTransactionsCommitted = 0;
    std::uint64_t readOnlyExplicitTransactions = 0;
    std::uint64_t disconnectRollbacks = 0;
    std::uint64_t shutdownRollbacks = 0;
    std::uint64_t explicitTransactionStatements = 0;
    std::uint64_t explicitTransactionWalBytes = 0;
    std::uint64_t originalBeforeImagePages = 0;
    std::uint64_t originalBeforeImageBytes = 0;
    std::uint64_t peakTransactionRecoveryBytes = 0;
};

// Owns SQL transaction lifetime and session policy. RecoveryCoordinator owns the
// physical transaction state, shared unchanged by every statement in the scope.
class TransactionManager {
public:
    TransactionManager(RecoveryCoordinator* recovery, CheckpointManager* checkpoints)
        : recovery_(recovery), checkpoints_(checkpoints) {}
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;
    TransactionManager(TransactionManager&&) = delete;
    TransactionManager& operator=(TransactionManager&&) = delete;

    void requireSession(SessionId session) const;
    void begin(SessionId session);
    void commit(SessionId session);
    void rollback(SessionId session);
    void beginMutation(SessionId session);
    void completeMutation(SessionId session);
    void failMutation(SessionId session);
    void completeRead(SessionId session);
    void closeSession(SessionId session);
    void shutdown();

    [[nodiscard]] bool hasActiveExplicitTransaction() const noexcept { return active_.has_value(); }
    [[nodiscard]] TransactionState state() const noexcept {
        return active_ ? TransactionState::Active : TransactionState::Idle;
    }
    [[nodiscard]] TransactionId activeTransactionId() const noexcept;
    [[nodiscard]] bool hasMaterializedWalBegin() const noexcept;
    [[nodiscard]] Lsn lastLsn() const noexcept;
    [[nodiscard]] std::size_t touchedPageCount() const noexcept;
    [[nodiscard]] std::uint64_t transactionWalBytes() const noexcept;
    [[nodiscard]] const ExplicitTransactionStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }

private:
    struct TransactionContext {
        SessionId owner;
        std::uint64_t initialWalBytes;
    };
    RecoveryCoordinator* recovery_;
    CheckpointManager* checkpoints_;
    std::optional<TransactionContext> active_;
    ExplicitTransactionStats stats_{};
    bool failed_ = false;

    void requireActive(SessionId session) const;
    void captureMemoryStats();
    void finishRollback();
    void safeBoundary() noexcept;
};

} // namespace minidb
