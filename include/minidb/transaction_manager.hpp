#pragma once

#include "minidb/database_access_gate.hpp"
#include "minidb/recovery.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace minidb {
class CheckpointManager;
using SessionId = std::uint64_t;
inline constexpr SessionId LOCAL_SESSION_ID = 0;
enum class TransactionState { Idle, Active };
enum class SessionCloseReason { Disconnect, Shutdown };

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

// Session state is serialized per session; queries from different sessions are
// independent. Only a holder of the exclusive database lease touches recovery.
class TransactionManager {
    struct SessionContext;
public:
    class SessionGuard {
        friend class TransactionManager;
        std::shared_ptr<SessionContext> context_;
        std::unique_lock<std::recursive_mutex> lock_;
        explicit SessionGuard(std::shared_ptr<SessionContext> context);
    public:
        SessionGuard(SessionGuard&&) noexcept = default;
        SessionGuard& operator=(SessionGuard&&) = delete;
    };
    TransactionManager(RecoveryCoordinator* recovery, CheckpointManager* checkpoints);
    ~TransactionManager();
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    [[nodiscard]] SessionGuard lockSession(SessionId session);
    void setCancellationProbe(SessionId session, DatabaseAccessGate::CancelProbe probe);
    void requireSession(SessionId session) const;
    void begin(SessionId session, AccessMode mode = AccessMode::ReadWrite);
    void commit(SessionId session);
    void rollback(SessionId session);
    [[nodiscard]] CheckpointId checkpoint(SessionId session, CheckpointMode mode);
    void beginMutation(SessionId session);
    void completeMutation(SessionId session);
    void failMutation(SessionId session);
    void beginRead(SessionId session);
    void completeRead(SessionId session);
    void failRead(SessionId session);
    void closeSession(SessionId session, SessionCloseReason reason = SessionCloseReason::Disconnect);
    void shutdown();

    [[nodiscard]] DatabaseAccessGate& accessGate() noexcept { return gate_; }
    [[nodiscard]] const DatabaseAccessGate& accessGate() const noexcept { return gate_; }
    [[nodiscard]] bool hasActiveExplicitTransaction() const noexcept { return activeCount_.load() != 0; }
    [[nodiscard]] bool hasActiveExplicitTransaction(SessionId session) const;
    [[nodiscard]] std::optional<AccessMode> accessMode(SessionId session) const;
    [[nodiscard]] TransactionState state() const noexcept {
        return hasActiveExplicitTransaction() ? TransactionState::Active : TransactionState::Idle;
    }
    // Writer diagnostics are snapshots. READ ONLY sessions have no WAL identity.
    [[nodiscard]] TransactionId activeTransactionId() const;
    [[nodiscard]] bool hasMaterializedWalBegin() const;
    [[nodiscard]] Lsn lastLsn() const;
    [[nodiscard]] std::size_t touchedPageCount() const;
    [[nodiscard]] std::uint64_t transactionWalBytes() const;
    [[nodiscard]] ExplicitTransactionStats stats() const;
    void resetStats();

private:
    struct SessionContext {
        std::recursive_mutex mutex;
        std::optional<AccessMode> explicitMode;
        std::optional<DatabaseAccessGate::Lease> lease;
        DatabaseAccessGate::CancelProbe cancelled;
        TransactionId writerId = INVALID_TRANSACTION_ID;
        std::uint64_t initialWalBytes = 0;
        bool closed = false;
        std::atomic<bool> cancelRequested{false};
    };
    struct WriterDiagnostics {
        TransactionId id = INVALID_TRANSACTION_ID;
        bool logged = false;
        Lsn last = INVALID_LSN;
        std::size_t touched = 0;
        std::uint64_t bytes = 0;
    };
    RecoveryCoordinator* recovery_;
    CheckpointManager* checkpoints_;
    DatabaseAccessGate gate_;
    mutable std::mutex sessionsMutex_;
    std::unordered_map<SessionId, std::shared_ptr<SessionContext>> sessions_;
    mutable std::mutex statsMutex_;
    ExplicitTransactionStats stats_{};
    WriterDiagnostics writer_{};
    std::atomic<std::size_t> activeCount_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> stopped_{false};

    [[nodiscard]] std::shared_ptr<SessionContext> sessionContext(SessionId session);
    [[nodiscard]] std::shared_ptr<SessionContext> findSession(SessionId session) const;
    void requireWriter(const SessionContext& session) const;
    void captureWriterStats(TransactionId id);
    void finishRollback(SessionContext& session);
    void endExplicit(SessionContext& session);
    void safeBoundary(SessionContext& session) noexcept;
    [[nodiscard]] static DatabaseAccessGate::CancelProbe cancellation(SessionContext& session);
};
} // namespace minidb
