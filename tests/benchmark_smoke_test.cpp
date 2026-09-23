#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
    try {
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark smoke test failure: " << error.what() << '\n';
        return 1;
    }
}
