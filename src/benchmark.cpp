#include "minidb/benchmark.hpp"

#include "minidb/wire_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef MINIDB_BUILD_TYPE
#define MINIDB_BUILD_TYPE "unknown"
#endif

#ifndef MINIDB_GIT_COMMIT
#define MINIDB_GIT_COMMIT "unknown"
#endif

namespace minidb::bench {
namespace {

constexpr std::uint64_t MAX_CONFIG_COUNT = 10'000'000;

std::uint64_t parseUnsigned(std::string_view text, std::string_view option) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires an unsigned integer");
    }
    return value;
}

std::string_view requireValue(
    std::span<const std::string_view> arguments,
    std::size_t& index,
    std::string_view option) {
    if (++index >= arguments.size()) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    return arguments[index];
}

void validateConfig(const BenchmarkConfig& config) {
    if (config.benchmark.empty() == config.suite.empty()) {
        throw std::invalid_argument("specify exactly one of --benchmark or --suite");
    }
    if (config.rows == 0 || config.operations == 0 || config.pages == 0
        || config.repetitions == 0 || config.reopenInterval == 0
        || config.bufferFrames == 0 || config.lruK == 0 || config.walPayloadBytes == 0
        || config.walBatchSize == 0 || config.walBufferBytes == 0) {
        throw std::invalid_argument(
            "rows, operations, pages, repetitions, reopen interval, buffer frames, and K "
            "and WAL payload, batch, and buffer sizes must be positive");
    }
    if (config.rows > MAX_CONFIG_COUNT || config.operations > MAX_CONFIG_COUNT
        || config.pages > MAX_CONFIG_COUNT || config.warmupOperations > MAX_CONFIG_COUNT
        || config.workingSet > MAX_CONFIG_COUNT || config.bufferFrames > MAX_CONFIG_COUNT
        || config.lruK > MAX_CONFIG_COUNT || config.walBatchSize > MAX_CONFIG_COUNT
        || config.walBufferBytes > MAX_CONFIG_COUNT || config.repetitions > 100) {
        throw std::invalid_argument("benchmark configuration exceeds safety limits");
    }
    if (config.walPayloadBytes > wal_record_layout::MAX_PAYLOAD_SIZE) {
        throw std::invalid_argument("WAL payload exceeds the maximum record payload");
    }
    if (!config.suite.empty() && config.suite != "quick" && config.suite != "baseline") {
        throw std::invalid_argument("suite must be 'quick' or 'baseline'");
    }
    if (config.cacheMode == CacheMode::Reopen && config.reopenInterval == 0) {
        throw std::invalid_argument("reopen mode requires a positive reopen interval");
    }
    if (config.tupleSizes != "small" && config.tupleSizes != "medium"
        && config.tupleSizes != "large" && config.tupleSizes != "mixed") {
        throw std::invalid_argument("tuple sizes must be small, medium, large, or mixed");
    }
    if (!config.benchmark.empty()) {
        const auto supported = supportedBenchmarkNames();
        if (std::find(supported.begin(), supported.end(), config.benchmark)
            == supported.end()) {
            throw std::invalid_argument("unknown benchmark '" + config.benchmark + "'");
        }
    }
}

void writePagerJson(std::ostringstream& output, const PagerStats& pager) {
    output << "{\"page_requests\":" << pager.pageRequests
           << ",\"cache_hits\":" << pager.cacheHits
           << ",\"cache_misses\":" << pager.cacheMisses
           << ",\"physical_reads\":" << pager.physicalPageReads
           << ",\"physical_writes\":" << pager.physicalPageWrites
           << ",\"dirty_marks\":" << pager.dirtyMarks
           << ",\"flush_calls\":" << pager.flushCalls
           << ",\"appended_pages\":" << pager.appendedPages
           << ",\"resident_pages\":" << pager.residentPages << '}';
}

void writeStorageJson(std::ostringstream& output, const StorageMetrics& storage) {
    output << "{\"database_pages\":" << storage.databasePages
           << ",\"database_bytes\":" << storage.databaseBytes
           << ",\"free_pages\":" << storage.freePages
           << ",\"resident_pages\":" << storage.residentPages << '}';
}

void writeBufferJson(std::ostringstream& output, const BufferPoolStats& buffer) {
    const auto hitRatio = buffer.pageRequests == 0 ? 0.0
        : static_cast<double>(buffer.cacheHits) / static_cast<double>(buffer.pageRequests);
    output << "{\"page_requests\":" << buffer.pageRequests
           << ",\"cache_hits\":" << buffer.cacheHits
           << ",\"cache_misses\":" << buffer.cacheMisses
           << ",\"hit_ratio\":" << hitRatio
           << ",\"physical_reads\":" << buffer.physicalPageReads
           << ",\"physical_writes\":" << buffer.physicalPageWrites
           << ",\"evictions\":" << buffer.evictions
           << ",\"dirty_evictions\":" << buffer.dirtyEvictions
           << ",\"pin_operations\":" << buffer.pinOperations
           << ",\"unpin_operations\":" << buffer.unpinOperations
           << ",\"appended_pages\":" << buffer.appendedPages
           << ",\"wal_flush_requests\":" << buffer.walFlushRequests
           << ",\"resident_pages\":" << buffer.residentPages
           << ",\"pinned_frames\":" << buffer.pinnedFrames
           << ",\"evictable_frames\":" << buffer.evictableFrames
           << ",\"capacity\":" << buffer.capacity << '}';
}

} // namespace

ParseResult parseArguments(std::span<const std::string_view> arguments) {
    ParseResult result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            result.helpRequested = true;
        } else if (argument == "--benchmark") {
            result.config.benchmark = requireValue(arguments, index, argument);
        } else if (argument == "--suite") {
            result.config.suite = requireValue(arguments, index, argument);
        } else if (argument == "--rows") {
            result.config.rows = parseUnsigned(requireValue(arguments, index, argument), argument);
        } else if (argument == "--operations") {
            result.config.operations = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--pages") {
            result.config.pages = parseUnsigned(requireValue(arguments, index, argument), argument);
        } else if (argument == "--working-set") {
            result.config.workingSet = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--warmup") {
            result.config.warmupOperations = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--reopen-interval") {
            result.config.reopenInterval = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--buffer-frames") {
            result.config.bufferFrames = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--lru-k") {
            result.config.lruK = parseUnsigned(requireValue(arguments, index, argument), argument);
        } else if (argument == "--wal-payload-bytes") {
            result.config.walPayloadBytes = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--wal-batch-size") {
            result.config.walBatchSize = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--wal-buffer-bytes") {
            result.config.walBufferBytes = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--checkpoint-wal-bytes") {
            result.config.checkpointWalBytes = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--checkpoint-statements") {
            result.config.checkpointStatements = parseUnsigned(
                requireValue(arguments, index, argument), argument);
        } else if (argument == "--seed") {
            result.config.seed = parseUnsigned(requireValue(arguments, index, argument), argument);
        } else if (argument == "--repetitions") {
            const auto repetitions = parseUnsigned(
                requireValue(arguments, index, argument), argument);
            if (repetitions > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("repetitions exceeds uint32 range");
            }
            result.config.repetitions = static_cast<std::uint32_t>(repetitions);
        } else if (argument == "--db") {
            result.config.databasePath = requireValue(arguments, index, argument);
        } else if (argument == "--json" || argument == "--output") {
            result.config.jsonPath = requireValue(arguments, index, argument);
        } else if (argument == "--tuple-sizes") {
            result.config.tupleSizes = requireValue(arguments, index, argument);
        } else if (argument == "--mode") {
            const auto mode = requireValue(arguments, index, argument);
            if (mode == "hot") result.config.cacheMode = CacheMode::Hot;
            else if (mode == "reopen") result.config.cacheMode = CacheMode::Reopen;
            else throw std::invalid_argument("mode must be 'hot' or 'reopen'");
        } else if (argument == "--retain-db") {
            result.config.keepDatabase = true;
        } else {
            throw std::invalid_argument("unknown benchmark option '" + std::string(argument) + "'");
        }
    }
    if (!result.helpRequested) validateConfig(result.config);
    return result;
}

std::string usageText() {
    return
        "Usage: minidb_bench (--benchmark NAME | --suite quick|baseline) [options]\n"
        "  --rows N              dataset row count (default 1000)\n"
        "  --operations N        measured operation count (default 1000)\n"
        "  --pages N             pager dataset pages (default 1000)\n"
        "  --working-set N       lookup working set; 0 means full dataset\n"
        "  --warmup N            untimed warmup operations (default 100)\n"
        "  --mode hot|reopen     retain or periodically recreate database owner\n"
        "  --reopen-interval N   operations between reopen events (default 250)\n"
        "  --buffer-frames N     bounded buffer frame capacity (default 64)\n"
        "  --lru-k N             LRU-K history length (default 2)\n"
        "  --wal-payload-bytes N WAL payload bytes per record (default 128)\n"
        "  --wal-batch-size N    records per synchronous batch flush (default 10)\n"
        "  --wal-buffer-bytes N  in-memory WAL buffer capacity (default 65536)\n"
        "  --checkpoint-wal-bytes N automatic checkpoint WAL-growth bytes; 0 disables\n"
        "  --checkpoint-statements N automatic checkpoint commit count; 0 disables\n"
        "  --tuple-sizes MODE    small, medium, large, or mixed\n"
        "  --seed N              deterministic seed (default 12345)\n"
        "  --repetitions N       repeat each workload (default 1)\n"
        "  --db PATH             benchmark database path\n"
        "  --json PATH           write deterministic JSON report\n"
        "  --retain-db           keep generated database after the run\n"
        "  --help                show this help\n";
}

std::vector<std::string> supportedBenchmarkNames() {
    return {
        "pager", "pager_sequential", "pager_random", "pager_hot",
        "buffer", "buffer_sequential", "buffer_random", "buffer_hotset",
        "buffer_scan_resistance",
        "bplus", "bplus_insert_sequential", "bplus_insert_random",
        "bplus_find_hit", "bplus_find_miss", "bplus_range", "bplus_mixed",
        "tuple", "tuple_insert", "tuple_lookup", "tuple_update", "tuple_erase",
        "tuple_scan", "tuple_fragmentation",
        "sql", "sql_pk_lookup", "sql_heap_scan", "sql_insert", "sql_update",
        "sql_delete", "sql_mixed", "sql_pk_vs_heap",
        "tcp", "tcp_pk_lookup", "tcp_heap_scan", "tcp_insert", "tcp_mixed",
        "mixed", "mixed_read_heavy", "mixed_write_heavy",
        "wal", "wal_append_buffered", "wal_append_flush_each", "wal_batch_flush",
        "txn_insert", "txn_update", "txn_delete", "txn_mixed", "recovery_full_scan",
        "checkpoint_latency", "recovery_checkpoint_compare",
    };
}

std::uint64_t nearestRankPercentile(
    std::vector<std::uint64_t> samples,
    unsigned percent) {
    if (samples.empty()) throw std::invalid_argument("percentile requires samples");
    if (percent == 0 || percent > 100) {
        throw std::invalid_argument("percentile must be between 1 and 100");
    }
    std::sort(samples.begin(), samples.end());
    const auto count = static_cast<std::uint64_t>(samples.size());
    const auto rank = ((static_cast<std::uint64_t>(percent) * count) + 99U) / 100U;
    return samples[static_cast<std::size_t>(rank - 1U)];
}

TimingMetrics summarizeTimings(
    const std::vector<std::uint64_t>& latenciesNanoseconds,
    std::uint64_t totalNanoseconds) {
    if (latenciesNanoseconds.empty()) {
        throw std::invalid_argument("timing summary requires at least one operation");
    }
    const auto [minimum, maximum] = std::minmax_element(
        latenciesNanoseconds.begin(), latenciesNanoseconds.end());
    const auto operations = static_cast<std::uint64_t>(latenciesNanoseconds.size());
    const auto seconds = static_cast<double>(totalNanoseconds) / 1'000'000'000.0;
    return TimingMetrics{
        operations,
        totalNanoseconds,
        seconds == 0.0 ? 0.0 : static_cast<double>(operations) / seconds,
        static_cast<double>(totalNanoseconds) / static_cast<double>(operations),
        nearestRankPercentile(latenciesNanoseconds, 50),
        nearestRankPercentile(latenciesNanoseconds, 95),
        nearestRankPercentile(latenciesNanoseconds, 99),
        *minimum,
        *maximum,
    };
}

PagerStats accumulatePagerStats(PagerStats accumulated, const PagerStats& next) {
    accumulated.pageRequests += next.pageRequests;
    accumulated.cacheHits += next.cacheHits;
    accumulated.cacheMisses += next.cacheMisses;
    accumulated.physicalPageReads += next.physicalPageReads;
    accumulated.physicalPageWrites += next.physicalPageWrites;
    accumulated.dirtyMarks += next.dirtyMarks;
    accumulated.flushCalls += next.flushCalls;
    accumulated.appendedPages += next.appendedPages;
    accumulated.residentPages = std::max(accumulated.residentPages, next.residentPages);
    return accumulated;
}

BufferPoolStats accumulateBufferStats(
    BufferPoolStats accumulated,
    const BufferPoolStats& next) {
    accumulated.pageRequests += next.pageRequests;
    accumulated.cacheHits += next.cacheHits;
    accumulated.cacheMisses += next.cacheMisses;
    accumulated.physicalPageReads += next.physicalPageReads;
    accumulated.physicalPageWrites += next.physicalPageWrites;
    accumulated.evictions += next.evictions;
    accumulated.dirtyEvictions += next.dirtyEvictions;
    accumulated.pinOperations += next.pinOperations;
    accumulated.unpinOperations += next.unpinOperations;
    accumulated.appendedPages += next.appendedPages;
    accumulated.walFlushRequests += next.walFlushRequests;
    accumulated.residentPages = std::max(accumulated.residentPages, next.residentPages);
    accumulated.pinnedFrames = std::max(accumulated.pinnedFrames, next.pinnedFrames);
    accumulated.evictableFrames = std::max(
        accumulated.evictableFrames, next.evictableFrames);
    accumulated.capacity = std::max(accumulated.capacity, next.capacity);
    return accumulated;
}

std::string escapeJson(std::string_view value) {
    std::ostringstream output;
    output << std::hex << std::uppercase;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character);
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string resultsToJson(const std::vector<BenchmarkResult>& results) {
    std::ostringstream output;
    output << std::setprecision(6) << "{\"schema_version\":1,\"results\":[";
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (index != 0) output << ',';
        const auto& result = results[index];
        const auto& config = result.configuration;
        output << "{\"benchmark\":\"" << escapeJson(result.benchmark)
               << "\",\"storage_backend\":\"" << escapeJson(result.storageBackend)
               << "\",\"seed\":" << result.seed
               << ",\"repetition\":" << result.repetition
               << ",\"configuration\":{\"rows\":" << config.rows
               << ",\"operations\":" << config.operations
               << ",\"pages\":" << config.pages
               << ",\"working_set\":" << config.workingSet
               << ",\"warmup_operations\":" << config.warmupOperations
               << ",\"reopen_interval\":" << config.reopenInterval
               << ",\"repetitions\":" << config.repetitions
               << ",\"buffer_frames\":" << config.bufferFrames
               << ",\"lru_k\":" << config.lruK
               << ",\"wal_payload_bytes\":" << config.walPayloadBytes
               << ",\"wal_batch_size\":" << config.walBatchSize
               << ",\"wal_buffer_bytes\":" << config.walBufferBytes
               << ",\"checkpoint_wal_bytes\":" << config.checkpointWalBytes
               << ",\"checkpoint_statements\":" << config.checkpointStatements
               << ",\"checkpoint_enabled\":"
               << ((config.checkpointWalBytes != 0 || config.checkpointStatements != 0)
                   ? "true" : "false")
               << ",\"cache_mode\":\""
               << (config.cacheMode == CacheMode::Hot ? "hot" : "reopen")
               << "\",\"tuple_sizes\":\"" << escapeJson(config.tupleSizes) << "\"}"
               << ",\"timing\":{\"operation_count\":" << result.timing.operationCount
               << ",\"total_ns\":" << result.timing.totalNanoseconds
               << ",\"ops_per_second\":" << result.timing.operationsPerSecond
               << ",\"mean_ns\":" << result.timing.meanNanoseconds
               << ",\"p50_ns\":" << result.timing.p50Nanoseconds
               << ",\"p95_ns\":" << result.timing.p95Nanoseconds
               << ",\"p99_ns\":" << result.timing.p99Nanoseconds
               << ",\"min_ns\":" << result.timing.minimumNanoseconds
               << ",\"max_ns\":" << result.timing.maximumNanoseconds << '}'
               << ",\"pager\":";
        writePagerJson(output, result.pager);
        output << ",\"buffer\":";
        writeBufferJson(output, result.buffer);
        output << ",\"wal\":{\"wal_records\":" << result.wal.walRecords
               << ",\"wal_payload_bytes\":" << result.wal.walPayloadBytes
               << ",\"payload_bytes_per_second\":"
               << result.wal.payloadBytesPerSecond
               << ",\"wal_bytes_written\":" << result.wal.manager.bytesWritten
               << ",\"wal_fsync_calls\":" << result.wal.manager.fsyncCalls
               << ",\"wal_buffer_flushes\":" << result.wal.manager.bufferFlushes
               << ",\"wal_physical_writes\":" << result.wal.manager.physicalWrites
               << ",\"wal_flush_up_to_calls\":" << result.wal.manager.flushUpToCalls
               << ",\"last_appended_lsn\":" << result.wal.manager.lastAppendedLsn
               << ",\"durable_lsn\":" << result.wal.manager.durableLsn
               << ",\"append_p95_ns\":" << result.wal.appendTiming.p95Nanoseconds
               << ",\"append_p99_ns\":" << result.wal.appendTiming.p99Nanoseconds
               << ",\"flush_count\":" << result.wal.flushTiming.operationCount
               << ",\"flush_p95_ns\":" << result.wal.flushTiming.p95Nanoseconds
               << ",\"flush_p99_ns\":" << result.wal.flushTiming.p99Nanoseconds << '}';
        output << ",\"recovery\":{\"transactions_begun\":"
               << result.recovery.transactions.transactionsBegun
               << ",\"transactions_committed\":"
               << result.recovery.transactions.transactionsCommitted
               << ",\"page_update_records\":"
               << result.recovery.transactions.pageUpdateRecords
               << ",\"full_page_image_bytes\":"
               << result.recovery.transactions.fullPageImageBytes
               << ",\"wal_bytes\":" << result.recovery.walBytes
               << ",\"logical_changed_bytes\":" << result.recovery.logicalChangedBytes
               << ",\"logging_amplification\":" << result.recovery.loggingAmplification
               << ",\"records_analyzed\":" << result.recovery.recovery.recordsAnalyzed
               << ",\"pages_redone\":" << result.recovery.recovery.pagesRedone
               << ",\"pages_undone\":" << result.recovery.recovery.pagesUndone
               << ",\"analysis_ns\":" << result.recovery.recovery.analysisNs
               << ",\"redo_ns\":" << result.recovery.recovery.redoNs
               << ",\"undo_ns\":" << result.recovery.recovery.undoNs
               << ",\"total_ns\":" << result.recovery.recovery.totalNs
               << ",\"checkpoint_used\":"
               << (result.recovery.recovery.checkpointUsed ? "true" : "false")
               << ",\"wal_bytes_skipped\":" << result.recovery.recovery.walBytesSkipped
               << ",\"wal_bytes_scanned\":" << result.recovery.recovery.walBytesScanned
               << ",\"full_scan_records_analyzed\":"
               << result.recovery.fullScanRecovery.recordsAnalyzed
               << ",\"full_scan_wal_bytes_scanned\":"
               << result.recovery.fullScanRecovery.walBytesScanned
               << ",\"full_scan_total_ns\":"
               << result.recovery.fullScanRecovery.totalNs << '}';
        output << ",\"checkpoint\":{\"checkpoint_count\":"
               << result.checkpoint.checkpointsCompleted
               << ",\"checkpoint_total_ns\":" << result.checkpoint.checkpointDurationNs
               << ",\"checkpoint_max_ns\":" << result.checkpoint.checkpointMaxDurationNs
               << ",\"dirty_pages_flushed\":" << result.checkpoint.dirtyPagesFlushed
               << ",\"database_writes\":" << result.checkpoint.databaseWrites
               << ",\"wal_forces\":" << result.checkpoint.walForces
               << ",\"database_syncs\":" << result.checkpoint.databaseSyncs
               << ",\"control_file_syncs\":" << result.checkpoint.controlFileSyncs
               << '}';
        output << ",\"storage\":{\"before\":";
        writeStorageJson(output, result.storageBefore);
        output << ",\"after\":";
        writeStorageJson(output, result.storageAfter);
        output << '}' << ",\"execution\":{\"average_rows_examined\":"
               << result.averageRowsExamined
               << ",\"average_index_lookups\":" << result.averageIndexLookups << '}'
               << ",\"environment\":{\"version_context\":\""
               << escapeJson(result.environment.versionContext)
               << "\",\"git_commit\":\"" << escapeJson(result.environment.gitCommit)
               << "\",\"compiler\":\"" << escapeJson(result.environment.compiler)
               << "\",\"build_type\":\"" << escapeJson(result.environment.buildType)
               << "\",\"platform\":\"" << escapeJson(result.environment.platform)
               << "\",\"cpp_standard\":" << result.environment.cppStandard
               << ",\"page_size\":" << result.environment.pageSize
               << ",\"protocol_version\":" << result.environment.protocolVersion
               << ",\"hardware_concurrency\":"
               << result.environment.hardwareConcurrency << '}'
               << ",\"validation_passed\":"
               << (result.validationPassed ? "true" : "false") << '}';
    }
    output << "]}\n";
    return output.str();
}

std::string formatHuman(const BenchmarkResult& result) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2)
           << "benchmark: " << result.benchmark << " (repetition " << result.repetition << ")\n"
           << "storage backend: " << result.storageBackend << '\n'
           << "seed/mode: " << result.seed << '/'
           << (result.configuration.cacheMode == CacheMode::Hot ? "hot" : "reopen") << '\n'
           << "environment: " << result.environment.versionContext << ", git "
           << result.environment.gitCommit << ", " << result.environment.compiler << ", "
           << result.environment.buildType << ", " << result.environment.platform << '\n'
           << "format: C++" << result.environment.cppStandard << ", page size "
           << result.environment.pageSize << ", protocol "
           << result.environment.protocolVersion << '\n'
           << "operations: " << result.timing.operationCount << '\n'
           << "total: " << (static_cast<double>(result.timing.totalNanoseconds) / 1'000'000.0)
           << " ms\nthroughput: " << result.timing.operationsPerSecond << " ops/s\n"
           << "latency ns mean/p50/p95/p99/min/max: " << result.timing.meanNanoseconds
           << '/' << result.timing.p50Nanoseconds << '/' << result.timing.p95Nanoseconds
           << '/' << result.timing.p99Nanoseconds << '/' << result.timing.minimumNanoseconds
           << '/' << result.timing.maximumNanoseconds << '\n'
           << "pager requests/hits/misses/reads/writes/resident: "
           << result.pager.pageRequests << '/' << result.pager.cacheHits << '/'
           << result.pager.cacheMisses << '/' << result.pager.physicalPageReads << '/'
           << result.pager.physicalPageWrites << '/' << result.pager.residentPages << '\n'
           << "pager dirty marks/flush calls/appended pages: " << result.pager.dirtyMarks
           << '/' << result.pager.flushCalls << '/' << result.pager.appendedPages << '\n'
           << "buffer requests/hits/misses/hit ratio: " << result.buffer.pageRequests << '/'
           << result.buffer.cacheHits << '/' << result.buffer.cacheMisses << '/'
           << (result.buffer.pageRequests == 0 ? 0.0
               : static_cast<double>(result.buffer.cacheHits)
                   / static_cast<double>(result.buffer.pageRequests)) << '\n'
           << "buffer reads/writes/evictions/dirty evictions/resident/capacity: "
           << result.buffer.physicalPageReads << '/' << result.buffer.physicalPageWrites
           << '/' << result.buffer.evictions << '/' << result.buffer.dirtyEvictions << '/'
           << result.buffer.residentPages << '/' << result.buffer.capacity << '\n'
           << "wal records/payload bytes/written bytes/fsyncs/buffer flushes: "
           << result.wal.walRecords << '/' << result.wal.walPayloadBytes << '/'
           << result.wal.manager.bytesWritten << '/' << result.wal.manager.fsyncCalls << '/'
           << result.wal.manager.bufferFlushes << '\n'
           << "wal payload bytes/s, append p95/p99 ns, flush p95/p99 ns: "
           << result.wal.payloadBytesPerSecond << '/'
           << result.wal.appendTiming.p95Nanoseconds << '/'
           << result.wal.appendTiming.p99Nanoseconds << '/'
           << result.wal.flushTiming.p95Nanoseconds << '/'
           << result.wal.flushTiming.p99Nanoseconds << '\n'
           << "txn begun/committed/page updates/full-image bytes: "
           << result.recovery.transactions.transactionsBegun << '/'
           << result.recovery.transactions.transactionsCommitted << '/'
           << result.recovery.transactions.pageUpdateRecords << '/'
           << result.recovery.transactions.fullPageImageBytes << '\n'
           << "recovery analyzed/redone/undone/total ns: "
           << result.recovery.recovery.recordsAnalyzed << '/'
           << result.recovery.recovery.pagesRedone << '/'
           << result.recovery.recovery.pagesUndone << '/'
           << result.recovery.recovery.totalNs << '\n'
           << "recovery checkpoint/skipped/scanned, full-scan records/ns: "
           << (result.recovery.recovery.checkpointUsed ? 1 : 0) << '/'
           << result.recovery.recovery.walBytesSkipped << '/'
           << result.recovery.recovery.walBytesScanned << '/'
           << result.recovery.fullScanRecovery.recordsAnalyzed << '/'
           << result.recovery.fullScanRecovery.totalNs << '\n'
           << "checkpoint count/dirty pages/total ns/max ns/control fsyncs: "
           << result.checkpoint.checkpointsCompleted << '/'
           << result.checkpoint.dirtyPagesFlushed << '/'
           << result.checkpoint.checkpointDurationNs << '/'
           << result.checkpoint.checkpointMaxDurationNs << '/'
           << result.checkpoint.controlFileSyncs << '\n'
           << "WAL/logical bytes and amplification: " << result.recovery.walBytes << '/'
           << result.recovery.logicalChangedBytes << '/'
           << result.recovery.loggingAmplification << '\n'
           << "storage before pages/bytes/free/resident: "
           << result.storageBefore.databasePages << '/' << result.storageBefore.databaseBytes
           << '/' << result.storageBefore.freePages << '/'
           << result.storageBefore.residentPages << '\n'
           << "storage after pages/bytes/free/resident: "
           << result.storageAfter.databasePages << '/' << result.storageAfter.databaseBytes
           << '/' << result.storageAfter.freePages << '/'
           << result.storageAfter.residentPages << '\n'
           << "database growth pages/bytes: "
           << (result.storageAfter.databasePages - result.storageBefore.databasePages) << '/'
           << (result.storageAfter.databaseBytes - result.storageBefore.databaseBytes) << '\n'
           << "execution avg rows examined/index lookups: " << result.averageRowsExamined
           << '/' << result.averageIndexLookups << '\n'
           << "validation: " << (result.validationPassed ? "passed" : "failed") << '\n';
    return output.str();
}

EnvironmentMetadata currentEnvironment() {
#if defined(__APPLE__)
    constexpr std::string_view platform = "macOS";
#elif defined(__linux__)
    constexpr std::string_view platform = "Linux";
#elif defined(_WIN32)
    constexpr std::string_view platform = "Windows";
#else
    constexpr std::string_view platform = "unknown";
#endif
    const std::string buildType = std::string(MINIDB_BUILD_TYPE).empty()
        ? "unspecified" : MINIDB_BUILD_TYPE;
    return EnvironmentMetadata{
        "v0.1.0 frozen MVP + Milestone 11C.1 sharp checkpoints",
        MINIDB_GIT_COMMIT,
        __VERSION__,
        buildType,
        std::string(platform),
        __cplusplus,
        Pager::PAGE_SIZE,
        net::PROTOCOL_VERSION,
        std::thread::hardware_concurrency(),
    };
}

} // namespace minidb::bench
