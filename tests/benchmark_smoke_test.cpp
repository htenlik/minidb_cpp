#include "minidb/benchmark.hpp"
#include "minidb/checkpoint_control.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void testTransactionalCheckpointMode() {
    for (const auto mode : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
        for (const bool triggered : {true, false}) {
            minidb::test::TemporaryDatabase database("benchmark_transactional_checkpoint");
            minidb::bench::BenchmarkConfig config;
            config.benchmark = "txn_insert";
            config.databasePath = database.path().string();
            config.operations = 4;
            config.warmupOperations = 0;
            config.checkpointMode = mode;
            config.checkpointWalBytes = 0;
            config.checkpointStatements = triggered ? 1 : 1000;
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            const auto& result = results.at(0);
            const auto& checkpoints = result.checkpoint;
            const auto expected = triggered ? config.operations : 0;
            const bool sharp = mode == minidb::CheckpointMode::Sharp;
            minidb::test::require(results.size() == 1 && result.validationPassed
                && result.configuration.checkpointMode == mode
                && result.timing.operationCount == config.operations
                && result.recovery.transactions.transactionsCommitted == config.operations,
                "Transactional checkpoint benchmark configuration/counters changed");
            std::cout << "txn_insert requested=" << minidb::checkpointModeName(mode)
                      << " triggered=" << triggered
                      << " sharp=" << checkpoints.sharpCheckpointsCompleted
                      << " fuzzy=" << checkpoints.fuzzyCheckpointsCompleted << '\n';
            // runTransactional resets statistics after setup; only measured
            // autocommits contribute to these automatic checkpoint counts.
            minidb::test::require(checkpoints.checkpointsCompleted == expected
                && checkpoints.sharpCheckpointsCompleted == (sharp ? expected : 0)
                && checkpoints.fuzzyCheckpointsCompleted == (sharp ? 0 : expected)
                && checkpoints.checkpointFailures == 0,
                "Transactional benchmark executed a different checkpoint mode than reported");
            const auto json = minidb::bench::resultsToJson(results);
            const auto modeField = json.find("\"checkpoint_mode\":");
            const auto expectedMode = std::string("\"checkpoint_mode\":\"")
                + std::string(minidb::checkpointModeName(mode)) + '"';
            minidb::test::require(modeField != std::string::npos
                && json.compare(modeField, expectedMode.size(), expectedMode) == 0
                && json.find("\"sharp_checkpoints\":"
                    + std::to_string(checkpoints.sharpCheckpointsCompleted)) != std::string::npos
                && json.find("\"fuzzy_checkpoints\":"
                    + std::to_string(checkpoints.fuzzyCheckpointsCompleted)) != std::string::npos,
                "Transactional benchmark JSON mode disagrees with execution");
        }
    }
}

void testRelatedCheckpointModes() {
    bool matched = true;
    for (const auto* name : {"tcp_insert", "recovery_full_scan", "recovery_loser"}) {
        for (const auto mode : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
            minidb::test::TemporaryDatabase database("benchmark_related_checkpoint");
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.databasePath = database.path().string();
            config.rows = 3;
            config.operations = 3;
            config.warmupOperations = 0;
            config.checkpointMode = mode;
            config.checkpointWalBytes = 0;
            config.checkpointStatements = 1;
            config.keepDatabase = true;
            const auto result = minidb::bench::runConfiguredBenchmarks(config).at(0);
            minidb::test::require(result.validationPassed
                && result.configuration.checkpointMode == mode,
                "Related benchmark checkpoint configuration changed");
            minidb::LogManager log(minidb::walPathForDatabase(config.databasePath),
                minidb::LogManager::DEFAULT_BUFFER_SIZE, minidb::LogOpenMode::EagerValidated,
                minidb::WalStorageMode::Auto);
            const auto selected = minidb::CheckpointControl(
                minidb::checkpointPathForDatabase(config.databasePath)).select(log);
            minidb::test::require(selected.slot.has_value(),
                "Related benchmark did not publish an automatic checkpoint");
            const auto end = log.readRecordAt(selected.slot->checkpointEndLsn);
            const auto expected = mode == minidb::CheckpointMode::Sharp
                ? minidb::LogRecordType::CheckpointEnd : minidb::LogRecordType::FuzzyCheckpointEnd;
            std::cout << name << " requested=" << minidb::checkpointModeName(mode)
                      << " persisted=" << (end.type == minidb::LogRecordType::FuzzyCheckpointEnd
                          ? "fuzzy" : "sharp") << '\n';
            matched = matched && end.type == expected;
            if (config.benchmark != "tcp_insert") {
                matched = matched && result.recovery.recovery.checkpointUsed
                    && result.recovery.recovery.checkpointMode == mode;
            }
        }
    }
    minidb::test::require(matched,
        "Related benchmarks executed a different checkpoint mode than reported");
}

} // namespace

int main() {
    try {
        testRelatedCheckpointModes();
        testTransactionalCheckpointMode();
        const std::vector<std::string> families{
            "pager", "buffer", "bplus", "tuple", "sql", "tcp", "mixed", "wal",
        };
        for (const auto& family : families) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = family;
            config.rows = 8;
            config.operations = 6;
            config.pages = 8;
            config.workingSet = 4;
            config.warmupOperations = 2;
            config.reopenInterval = 3;
            config.bufferFrames = 3;
            config.seed = 20260901;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_benchmark_smoke_" + family + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            const auto expectedBackend = family == "pager" ? "legacy_pager"
                : family == "wal" ? "wal" : "buffer_pool";
            minidb::test::require(
                results.size() == 1 && results[0].validationPassed
                    && results[0].storageBackend == expectedBackend
                    && results[0].timing.operationCount == config.operations
                    && (family == "wal"
                        ? results[0].wal.walRecords == config.operations
                            && results[0].wal.manager.fsyncCalls == 1
                        : results[0].storageAfter.databasePages > 0
                            && results[0].storageAfter.databaseBytes
                                == results[0].storageAfter.databasePages
                                    * minidb::Pager::PAGE_SIZE),
                "benchmark family smoke result was incomplete");
        }
        minidb::bench::BenchmarkConfig recovery;
        recovery.benchmark = "recovery_clr_resume";
        recovery.operations = 6;
        recovery.redoPersistedPercent = 50;
        recovery.walSegmentBytes = 16 * 1024;
        recovery.databasePath = (std::filesystem::temp_directory_path()
            / "minidb_benchmark_smoke_clr.db").string();
        const auto clr = minidb::bench::runConfiguredBenchmarks(recovery);
        minidb::test::require(
            clr.size() == 1 && clr[0].validationPassed
                && clr[0].storageBackend == "physical_clr_restartable_undo"
                && clr[0].recovery.clrWalBytes
                    == recovery.operations
                        * (minidb::wal_record_layout::HEADER_SIZE
                           + minidb::compensation_log_layout::PAYLOAD_SIZE)
                && clr[0].recovery.recovery.undoUserRecordsCompensated == 3,
            "CLR restartable-UNDO benchmark smoke result was incomplete");
        std::cout << "benchmark family smoke tests passed\n";
        for (const auto* name : {"transaction_implicit", "transaction_explicit", "transaction_rollback",
                                 "transaction_recovery", "transaction_checkpoint"}) {
            for (auto mode : {minidb::CheckpointMode::Sharp, minidb::CheckpointMode::Fuzzy}) {
                minidb::test::TemporaryDatabase database(name);
                minidb::bench::BenchmarkConfig transaction;
                transaction.databasePath = database.path().string();
                transaction.benchmark = name;
                transaction.operations = 10;
                transaction.checkpointWalBytes = 1024;
                transaction.checkpointMode = mode;
                transaction.walSegmentBytes = 16 * 1024;
                const auto measured = minidb::bench::runConfiguredBenchmarks(transaction);
                minidb::test::require(measured.size() == 1 && measured[0].validationPassed,
                                      "Transaction benchmark failed validation");
                const auto& result = measured[0];
                minidb::test::require(result.storageBackend == "buffer_pool"
                    && result.configuration.rows == 1 && result.configuration.warmupOperations == 0,
                    "Transaction benchmark reported unused generic configuration");
                if (transaction.benchmark != "transaction_checkpoint") {
                    minidb::test::require(result.configuration.checkpointWalBytes == 0
                        && result.configuration.checkpointStatements == 0,
                        "Transaction comparison reported checkpoints enabled when disabled");
                }
                if (transaction.benchmark == "transaction_implicit") {
                    minidb::test::require(result.transaction.commitFsyncs == 10,
                                          "Implicit benchmark did not commit every mutation");
                } else if (transaction.benchmark == "transaction_explicit") {
                    minidb::test::require(result.transaction.commitFsyncs == 1
                        && result.transaction.originalBeforeImageBytes > 0,
                        "Explicit benchmark did not measure one durable commit");
                } else if (transaction.benchmark == "transaction_rollback"
                           || transaction.benchmark == "transaction_recovery") {
                    minidb::test::require(result.recovery.recovery.clrsAppended == 10,
                                          "Rollback/recovery benchmark CLR count incorrect");
                }
                minidb::test::require(minidb::bench::resultsToJson(measured).find("\"transaction\":")
                                          != std::string::npos,
                                      "Transaction benchmark JSON metrics missing");
                minidb::test::require(minidb::bench::resultsToJson(measured).find("\"pages_truncated\":")
                                          != std::string::npos,
                                      "Rollback truncation metric missing from JSON");
            }
        }
        for (const auto name : {"concurrency_pk_read", "concurrency_heap_read", "concurrency_read_only",
                               "concurrency_serial_read", "concurrency_writer_exclusion", "concurrency_buffer_reads"}) {
            minidb::test::TemporaryDatabase database(name);
            minidb::bench::BenchmarkConfig config;
            config.databasePath = database.path().string(); config.benchmark = name;
            config.rows = 32; config.operations = 24; config.clientThreads = 4; config.bufferFrames = 3;
            config.writerHoldMs = 1;
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(results.size() == 1 && results[0].validationPassed
                && results[0].timing.operationCount == (config.benchmark == "concurrency_writer_exclusion" ? 4U : 24U)
                && results[0].concurrency.server.peakActiveSessions >= 4,
                "Concurrency benchmark did not measure/validate connected clients");
            minidb::test::require(minidb::bench::resultsToJson(results).find("\"concurrency\":") != std::string::npos,
                "Concurrency benchmark JSON metrics missing");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark smoke test failure: " << error.what() << '\n';
        return 1;
    }
}
