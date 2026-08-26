#include "minidb/database_server.hpp"
#include "minidb/segmented_wal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace {

using minidb::test::require;

minidb::net::ServerConfig config(
    std::uint32_t segmentBytes, std::size_t frames = 4) {
    minidb::net::ServerConfig result;
    result.port = 0;
    result.bufferFrames = frames;
    result.lruK = 2;
    result.checkpointWalBytes = 0;
    result.checkpointStatements = 0;
    result.walSegmentBytes = segmentBytes;
    return result;
}

std::size_t rowCount(minidb::net::DatabaseServer& server, const std::string& table) {
    return std::get<minidb::sql::SelectResult>(
        server.sqlEngine().execute("SELECT * FROM " + table)).rows.size();
}

void testCrossSegmentCommitAndReopen() {
    minidb::test::TemporaryDatabase database("wal_cross_segment_commit");
    constexpr std::uint32_t SEGMENT_BYTES = 12U * 1024U;
    const auto path = database.path().string();
    {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 2));
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE docs (id UINT32 PRIMARY KEY, body VARCHAR(3000) NOT NULL)"));
        for (std::uint32_t key = 0; key < 8; ++key) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO docs VALUES (" + std::to_string(key) + ", '"
                + std::string(2500, static_cast<char>('a' + key)) + "')"));
        }
    }

    minidb::SegmentedWalStorage storage(
        minidb::segmentedWalPathForDatabase(path), SEGMENT_BYTES, false);
    std::map<minidb::TransactionId, std::set<minidb::WalSegmentId>> locations;
    for (const auto& record : storage.scan().records) {
        for (const auto& segment : storage.segments()) {
            if (record.lsn >= segment.header.startLsn
                && record.lsn < segment.logicalEndLsn) {
                locations[record.transactionId].insert(segment.header.segmentId);
                break;
            }
        }
    }
    require(std::any_of(locations.begin(), locations.end(), [](const auto& entry) {
                return entry.first != minidb::INVALID_TRANSACTION_ID
                    && entry.second.size() > 1;
            }),
            "No committed transaction crossed the forced segment boundary");
    {
        minidb::net::DatabaseServer reopened(path, config(SEGMENT_BYTES, 3));
        require(rowCount(reopened, "docs") == 8,
                "Cross-segment durable COMMIT did not survive reopen/REDO");
        reopened.catalog().validate();
    }
}

[[noreturn]] void loserChild(const std::string& path, std::uint32_t segmentBytes) {
    ::setenv("MINIDB_FAILPOINT", "before_commit_append", 1);
    minidb::net::DatabaseServer server(path, config(segmentBytes, 2));
    static_cast<void>(server.sqlEngine().execute(
        "INSERT INTO docs VALUES (99, '" + std::string(2500, 'z') + "')"));
    ::_exit(90);
}

void testCrossSegmentLoserUndo() {
    minidb::test::TemporaryDatabase database("wal_cross_segment_loser");
    constexpr std::uint32_t SEGMENT_BYTES = 12U * 1024U;
    const auto path = database.path().string();
    {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 2));
        static_cast<void>(server.sqlEngine().execute(
            "CREATE TABLE docs (id UINT32 PRIMARY KEY, body VARCHAR(3000) NOT NULL)"));
        static_cast<void>(server.checkpointManager().checkpoint());
    }
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) loserChild(path, SEGMENT_BYTES);
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Cross-segment loser child missed pre-COMMIT failpoint");
    {
        minidb::net::DatabaseServer recovered(path, config(SEGMENT_BYTES, 3));
        require(rowCount(recovered, "docs") == 0
                    && recovered.startupRecoveryStats().loserTransactions == 1,
                "Recovery did not UNDO the cross-segment loser");
        recovered.catalog().validate();
    }
}

[[noreturn]] void reclaimedHistoryLoserChild(
    const std::string& path, std::uint32_t segmentBytes, std::uint32_t key) {
    ::setenv("MINIDB_FAILPOINT", "before_commit_append", 1);
    minidb::net::DatabaseServer server(path, config(segmentBytes, 1));
    static_cast<void>(server.sqlEngine().execute(
        "INSERT INTO items VALUES (" + std::to_string(key) + ", 'loser')"));
    ::_exit(90);
}

void testManyCheckpointReclamationCyclesAndLostControl() {
    minidb::test::TemporaryDatabase database("wal_many_reclaim_cycles");
    constexpr std::uint32_t SEGMENT_BYTES = 40U * 1024U;
    const auto path = database.path().string();
    std::uint32_t nextKey = 0;
    std::uint64_t logicalHighWater = 0;
    for (std::size_t cycle = 0; cycle < 8; ++cycle) {
        minidb::net::DatabaseServer server(path, config(SEGMENT_BYTES, 4));
        if (cycle == 0) {
            static_cast<void>(server.sqlEngine().execute(
                "CREATE TABLE items (id UINT32 PRIMARY KEY, value VARCHAR(64) NOT NULL)"));
        }
        for (std::size_t row = 0; row < 12; ++row) {
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO items VALUES (" + std::to_string(nextKey++) + ", 'value')"));
        }
        static_cast<void>(server.checkpointManager().checkpoint());
        const auto stats = server.logManager().stats();
        require(stats.retainedSegments <= 5,
                "Periodic checkpointing did not bound retained segment count");
        require(stats.logicalWalEnd > logicalHighWater,
                "Checkpoint/reclamation cycle rebased the logical WAL high-water mark");
        logicalHighWater = stats.logicalWalEnd;
        server.catalog().validate();
    }

    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) reclaimedHistoryLoserChild(path, SEGMENT_BYTES, nextKey);
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 86,
            "Post-reclamation loser child missed pre-COMMIT failpoint");

    std::filesystem::remove(minidb::checkpointPathForDatabase(path));
    {
        minidb::net::DatabaseServer recovered(path, config(SEGMENT_BYTES, 4));
        require(recovered.startupRecoveryStats().checkpointUsed
                    && !recovered.startupRecoveryStats().fullScanFallback
                    && recovered.startupRecoveryStats().loserTransactions == 1
                    && rowCount(recovered, "items") == nextKey,
                "Retained-WAL checkpoint discovery did not undo the post-cycle loser");
        require(recovered.checkpointControl().select(recovered.logManager()).slot.has_value(),
                "Control loss fallback did not rebuild checkpoint metadata");
        recovered.catalog().validate();
        recovered.pageAllocator().validate();
    }
}

} // namespace

int main() {
    try {
        testCrossSegmentCommitAndReopen();
        testCrossSegmentLoserUndo();
        testManyCheckpointReclamationCyclesAndLostControl();
        std::cout << "segmented_wal_recovery_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "segmented_wal_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
