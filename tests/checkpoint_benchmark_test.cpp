#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <iostream>

namespace {
void requireEffectiveConfiguration(const std::vector<minidb::bench::BenchmarkResult>& results) {
    const auto& config = results.at(0).configuration;
    minidb::test::require(config.cacheMode == minidb::bench::CacheMode::Hot
        && config.warmupOperations == 0 && config.reopenInterval == 0
        && config.walBufferBytes == minidb::LogManager::DEFAULT_BUFFER_SIZE
        && config.checkpointWalBytes == 0 && config.checkpointStatements == 0,
        "Dedicated checkpoint experiment reported unused generic settings");
    const auto json = minidb::bench::resultsToJson(results);
    minidb::test::require(json.find("\"cache_mode\":\"hot\"") != std::string::npos
        && json.find("\"warmup_operations\":0") != std::string::npos
        && json.find("\"reopen_interval\":0") != std::string::npos
        && json.find("\"wal_buffer_bytes\":" + std::to_string(minidb::LogManager::DEFAULT_BUFFER_SIZE))
            != std::string::npos
        && json.find("\"checkpoint_enabled\":false") != std::string::npos,
        "Dedicated checkpoint JSON disagrees with effective configuration");
}
}

int main() {
    try {
        minidb::bench::BenchmarkConfig config;
        config.benchmark = "checkpoint_latency";
        config.rows = 8;
        config.operations = 4;
        config.bufferFrames = 16;
        config.cacheMode = minidb::bench::CacheMode::Reopen;
        config.reopenInterval = 1; config.warmupOperations = 7; config.walBufferBytes = 1;
        config.databasePath = (std::filesystem::temp_directory_path()
            / "minidb_checkpoint_benchmark_test.db").string();
        auto results = minidb::bench::runConfiguredBenchmarks(config);
        requireEffectiveConfiguration(results);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].checkpoint.checkpointsCompleted == 1
                    && results[0].timing.operationCount == 1,
                "Checkpoint latency benchmark smoke result is invalid");

        config.checkpointMode = minidb::CheckpointMode::Fuzzy;
        results = minidb::bench::runConfiguredBenchmarks(config);
        requireEffectiveConfiguration(results);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].checkpoint.fuzzyCheckpointsCompleted == 1
                    && results[0].checkpoint.databaseSyncs == 1
                    && results[0].checkpoint.fuzzyCheckpointDurabilitySyncs == 1
                    && results[0].checkpoint.dptEntriesCaptured != 0,
                "Fuzzy checkpoint latency benchmark smoke result is invalid");

        config.benchmark = "recovery_checkpoint_compare";
        config.operations = 12;
        config.checkpointMode = minidb::CheckpointMode::Sharp;
        results = minidb::bench::runConfiguredBenchmarks(config);
        requireEffectiveConfiguration(results);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].recovery.recovery.checkpointUsed
                    && results[0].recovery.recovery.walBytesSkipped > 0
                    && results[0].recovery.recovery.recordsAnalyzed
                        < results[0].recovery.fullScanRecovery.recordsAnalyzed,
                "Checkpoint recovery comparison did not measure bounded scanning");
        config.checkpointMode = minidb::CheckpointMode::Fuzzy;
        results = minidb::bench::runConfiguredBenchmarks(config);
        requireEffectiveConfiguration(results);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].recovery.recovery.checkpointMode
                        == minidb::CheckpointMode::Fuzzy
                    && results[0].recovery.recovery.checkpointDirtyPageCount != 0,
                "Fuzzy checkpoint recovery comparison did not use its DPT");

        config.benchmark = "checkpoint_retention";
        config.rows = 3;
        config.operations = 2;
        config.redoPersistedPercent = 0;
        results = minidb::bench::runConfiguredBenchmarks(config);
        requireEffectiveConfiguration(results);
        minidb::test::require(results.size() == 1 && results[0].validationPassed
                    && results[0].timing.operationCount == 3
                    && results[0].checkpoint.fuzzyCheckpointsCompleted == 3
                    && results[0].checkpoint.oldestRecLsn != minidb::INVALID_LSN,
                "Long-lived dirty-page retention benchmark is invalid");
        std::cout << "checkpoint_benchmark_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "checkpoint_benchmark_test failed: " << error.what() << '\n';
        return 1;
    }
}
