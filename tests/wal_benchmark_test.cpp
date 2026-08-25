#include "minidb/benchmark.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
    try {
        const std::vector<std::pair<std::string, std::uint64_t>> cases{
            {"wal_append_buffered", 1},
            {"wal_append_flush_each", 12},
            {"wal_batch_flush", 4},
        };
        for (const auto& [name, expectedFsyncs] : cases) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.operations = 12;
            config.walPayloadBytes = 32;
            config.walBatchSize = 3;
            config.walBufferBytes = 128;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_wal_benchmark_" + name + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(
                results.size() == 1 && results[0].validationPassed
                    && results[0].storageBackend == "wal"
                    && results[0].timing.operationCount == config.operations
                    && results[0].wal.walRecords == config.operations
                    && results[0].wal.walPayloadBytes
                        == config.operations * config.walPayloadBytes
                    && results[0].wal.manager.recordsAppended == config.operations
                    && results[0].wal.manager.bytesWritten
                        == config.operations
                            * (minidb::wal_record_layout::HEADER_SIZE
                               + config.walPayloadBytes)
                    && results[0].wal.manager.fsyncCalls == expectedFsyncs
                    && results[0].wal.flushTiming.operationCount == expectedFsyncs,
                "standalone WAL benchmark metrics or batching semantics changed");
            const auto json = minidb::bench::resultsToJson(results);
            minidb::test::require(
                json.find("\"wal_records\":12") != std::string::npos
                    && json.find("\"wal_payload_bytes\":384") != std::string::npos
                    && json.find("\"wal_fsync_calls\":"
                        + std::to_string(expectedFsyncs)) != std::string::npos,
                "standalone WAL benchmark JSON metrics were incomplete");
        }
        for (const auto& name : {std::string{"wal_segment_rotation"},
                                 std::string{"wal_reclamation"}}) {
            minidb::bench::BenchmarkConfig config;
            config.benchmark = name;
            config.operations = 40;
            config.walPayloadBytes = 32;
            config.walBufferBytes = 128;
            config.walSegmentBytes = 160;
            config.databasePath = (std::filesystem::temp_directory_path()
                / ("minidb_wal_benchmark_" + name + ".db")).string();
            const auto results = minidb::bench::runConfiguredBenchmarks(config);
            minidb::test::require(
                results.size() == 1 && results[0].validationPassed
                    && results[0].storageBackend == "segmented_wal"
                    && results[0].wal.manager.segmentRotations > 0
                    && (name != "wal_reclamation"
                        || (results[0].wal.manager.segmentsDeleted > 0
                            && results[0].checkpoint.walBytesReclaimed > 0)),
                "segmented WAL benchmark did not expose rotation/reclamation metrics");
        }
        std::cout << "WAL benchmark tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WAL benchmark test failure: " << error.what() << '\n';
        return 1;
    }
}
