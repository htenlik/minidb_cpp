#include "minidb/benchmark.hpp"

#include "minidb/catalog.hpp"
#include "minidb/byte_codec.hpp"
#include "minidb/database_server.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/minidb_client.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_access.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/table.hpp"
#include "minidb/tuple_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>

namespace minidb::bench {
namespace {

using Clock = std::chrono::steady_clock;

template <typename Operation>
void measure(std::vector<std::uint64_t>& latencies, Operation&& operation) {
    const auto start = Clock::now();
    operation();
    const auto stop = Clock::now();
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()));
}

std::uint64_t totalLatency(const std::vector<std::uint64_t>& latencies) {
    return std::accumulate(latencies.begin(), latencies.end(), std::uint64_t{0});
}

RecoveryBenchmarkMetrics::Distribution summarizeDistribution(
    const std::vector<std::uint64_t>& samples) {
    if (samples.empty()) return {};
    const auto total = std::accumulate(samples.begin(), samples.end(), std::uint64_t{0});
    return RecoveryBenchmarkMetrics::Distribution{
        samples.size(),
        static_cast<double>(total) / static_cast<double>(samples.size()),
        nearestRankPercentile(samples, 50),
        nearestRankPercentile(samples, 95),
        nearestRankPercentile(samples, 99),
        *std::max_element(samples.begin(), samples.end()),
    };
}

void removeDatabase(const BenchmarkConfig& config) {
    std::error_code error;
    std::filesystem::remove(config.databasePath, error);
    if (error) throw std::runtime_error("could not remove prior benchmark database");
    std::filesystem::remove(walPathForDatabase(config.databasePath), error);
    if (error) throw std::runtime_error("could not remove prior benchmark WAL");
    std::filesystem::remove_all(segmentedWalPathForDatabase(config.databasePath), error);
    if (error) throw std::runtime_error("could not remove prior segmented benchmark WAL");
    std::filesystem::remove_all(
        segmentedWalPathForDatabase(config.databasePath) + ".tmp", error);
    if (error) throw std::runtime_error("could not remove prior WAL migration temporary");
    std::filesystem::remove(checkpointPathForDatabase(config.databasePath), error);
    if (error) throw std::runtime_error("could not remove prior benchmark checkpoint control");
}

std::uint64_t walPhysicalBytes(const std::string& databasePath) {
    std::uint64_t total = 0;
    std::error_code error;
    const auto legacy = walPathForDatabase(databasePath);
    if (std::filesystem::is_regular_file(legacy, error)) {
        total += std::filesystem::file_size(legacy, error);
        if (error) throw std::runtime_error("could not inspect legacy benchmark WAL");
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("could not inspect legacy benchmark WAL");
    }
    error.clear();
    const auto directory = segmentedWalPathForDatabase(databasePath);
    if (std::filesystem::is_directory(directory, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) total += entry.file_size();
        }
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("could not inspect segmented benchmark WAL");
    }
    return total;
}

void cleanupDatabase(const BenchmarkConfig& config) {
    if (!config.keepDatabase) removeDatabase(config);
}

StorageMetrics storageMetrics(Pager& pager) {
    return StorageMetrics{
        pager.pageCount(),
        static_cast<std::uint64_t>(pager.pageCount()) * Pager::PAGE_SIZE,
        0,
        pager.residentPageCount(),
    };
}

StorageMetrics storageMetrics(
    const DiskManager& diskManager,
    const BufferPoolManager& bufferPool,
    const PageAllocator& allocator) {
    return StorageMetrics{
        diskManager.pageCount(),
        static_cast<std::uint64_t>(diskManager.pageCount()) * DiskManager::PAGE_SIZE,
        allocator.freePageIds().size(),
        bufferPool.residentPageCount(),
    };
}

BenchmarkResult finish(
    std::string name,
    const BenchmarkConfig& config,
    std::vector<std::uint64_t> latencies,
    PagerStats pager,
    StorageMetrics storageBefore,
    StorageMetrics storageAfter,
    double averageRowsExamined = 0.0,
    double averageIndexLookups = 0.0,
    BufferPoolStats buffer = {}) {
    BenchmarkResult result;
    result.storageBackend = name.starts_with("pager_") ? "legacy_pager" : "buffer_pool";
    result.benchmark = std::move(name);
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(latencies, totalLatency(latencies));
    result.pager = pager;
    result.buffer = buffer;
    result.storageBefore = storageBefore;
    result.storageAfter = storageAfter;
    result.averageRowsExamined = averageRowsExamined;
    result.averageIndexLookups = averageIndexLookups;
    result.environment = currentEnvironment();
    result.validationPassed = true;
    return result;
}

std::uint64_t workingSet(std::uint64_t configured, std::uint64_t available) {
    return configured == 0 ? available : std::min(configured, available);
}

RecordId syntheticRecordId(std::uint64_t value) {
    constexpr std::uint64_t slotsPerPage = INVALID_SLOT_ID;
    return RecordId{
        static_cast<PageId>((value / slotsPerPage) + 1U),
        static_cast<SlotId>(value % slotsPerPage),
    };
}

TupleBytes tuplePayload(
    std::uint64_t seed,
    std::uint64_t index,
    std::string_view distribution,
    std::uint64_t variation = 0) {
    std::uint64_t size = 0;
    const auto mixedClass = (seed + index + variation) % 4U;
    const auto selected = distribution == "mixed"
        ? mixedClass
        : distribution == "small" ? 0U : distribution == "medium" ? 1U : 2U;
    if (selected == 0) size = 1U + ((seed + index * 17U + variation) % 64U);
    else if (selected == 1) size = 129U + ((seed + index * 31U + variation) % 384U);
    else if (selected == 2) size = 513U + ((seed + index * 47U + variation) % 988U);
    else size = 17U + ((seed + index * 23U + variation) % 112U);
    TupleBytes bytes(static_cast<std::size_t>(size));
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        bytes[offset] = static_cast<std::byte>((seed + index + variation + offset) & 0xFFU);
    }
    return bytes;
}

const sql::SelectResult& selectResult(const sql::QueryResult& result) {
    if (!std::holds_alternative<sql::SelectResult>(result)) {
        throw std::runtime_error("benchmark SELECT returned a command result");
    }
    return std::get<sql::SelectResult>(result);
}

BenchmarkResult runWal(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    auto manager = std::make_unique<LogManager>(
        walPathForDatabase(config.databasePath),
        static_cast<std::size_t>(config.walBufferBytes));
    manager->resetStats();

    std::vector<std::uint64_t> operationLatencies;
    std::vector<std::uint64_t> appendLatencies;
    std::vector<std::uint64_t> flushLatencies;
    operationLatencies.reserve(static_cast<std::size_t>(config.operations));
    appendLatencies.reserve(static_cast<std::size_t>(config.operations));
    Lsn previous = INVALID_LSN;
    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        LogRecord record;
        record.type = LogRecordType::PageUpdate;
        record.transactionId = 1;
        record.prevLsn = previous;
        record.payload.assign(
            static_cast<std::size_t>(config.walPayloadBytes),
            static_cast<std::byte>((config.seed + operation) & 0xFFU));
        measure(appendLatencies, [&] { previous = manager->append(std::move(record)); });
        auto operationLatency = appendLatencies.back();
        const bool force = name == "wal_append_flush_each"
            || (name == "wal_batch_flush" && (operation + 1U) % config.walBatchSize == 0);
        if (force) {
            measure(flushLatencies, [&] { manager->flushUpTo(previous); });
            operationLatency += flushLatencies.back();
        }
        operationLatencies.push_back(operationLatency);
    }
    if (manager->durableLsn() != manager->lastAppendedLsn()) {
        measure(flushLatencies, [&] { manager->flushAll(); });
        operationLatencies.back() += flushLatencies.back();
    }

    manager->validate();
    const auto scanned = manager->scan();
    const auto expectedBytes = wal_file_layout::HEADER_SIZE
        + config.operations * (wal_record_layout::HEADER_SIZE + config.walPayloadBytes);
    if (scanned.records.size() != config.operations
        || manager->durableLsn() != manager->lastAppendedLsn()
        || scanned.fileBytes != expectedBytes) {
        throw std::runtime_error("WAL benchmark validation failed");
    }
    const auto operationTotal = totalLatency(operationLatencies);
    const auto payloadBytes = config.operations * config.walPayloadBytes;
    const auto seconds = static_cast<double>(operationTotal) / 1'000'000'000.0;
    BenchmarkResult result;
    result.benchmark = name;
    result.storageBackend = "wal";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(operationLatencies, operationTotal);
    result.wal.walRecords = config.operations;
    result.wal.walPayloadBytes = payloadBytes;
    result.wal.payloadBytesPerSecond = seconds == 0.0
        ? 0.0 : static_cast<double>(payloadBytes) / seconds;
    result.wal.manager = manager->stats();
    result.wal.appendTiming = summarizeTimings(appendLatencies, totalLatency(appendLatencies));
    result.wal.flushTiming = summarizeTimings(flushLatencies, totalLatency(flushLatencies));
    result.environment = currentEnvironment();
    result.validationPassed = true;
    manager.reset();
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runWalSegmentRotation(const BenchmarkConfig& config) {
    removeDatabase(config);
    LogManager manager(
        walPathForDatabase(config.databasePath),
        static_cast<std::size_t>(config.walBufferBytes),
        LogOpenMode::EagerValidated,
        WalStorageMode::Segmented,
        config.walSegmentBytes);
    manager.resetStats();
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    Lsn previous = INVALID_LSN;
    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        measure(latencies, [&] {
            previous = manager.append(LogRecord{
                LogRecordType::PageUpdate,
                operation + 1,
                previous,
                std::vector<std::byte>(
                    static_cast<std::size_t>(config.walPayloadBytes),
                    static_cast<std::byte>((config.seed + operation) & 0xFFU)),
            });
            manager.flushUpTo(previous);
        });
    }
    manager.validate();
    const auto stats = manager.stats();
    if (stats.segmentRotations == 0 || manager.scan().records.size() != config.operations) {
        throw std::runtime_error("WAL segment-rotation benchmark did not rotate/validate");
    }
    BenchmarkResult result;
    result.benchmark = "wal_segment_rotation";
    result.storageBackend = "segmented_wal";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(latencies, totalLatency(latencies));
    result.wal.walRecords = config.operations;
    result.wal.walPayloadBytes = config.operations * config.walPayloadBytes;
    result.wal.manager = stats;
    result.wal.appendTiming = result.timing;
    result.environment = currentEnvironment();
    result.validationPassed = true;
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runWalReclamation(const BenchmarkConfig& config) {
    removeDatabase(config);
    LogManager manager(
        walPathForDatabase(config.databasePath),
        static_cast<std::size_t>(config.walBufferBytes),
        LogOpenMode::EagerValidated,
        WalStorageMode::Segmented,
        config.walSegmentBytes);
    std::vector<Lsn> positions;
    positions.reserve(static_cast<std::size_t>(config.operations));
    Lsn previous = INVALID_LSN;
    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        previous = manager.append(LogRecord{
            LogRecordType::PageUpdate,
            operation + 1,
            previous,
            std::vector<std::byte>(
                static_cast<std::size_t>(config.walPayloadBytes),
                static_cast<std::byte>((config.seed + operation) & 0xFFU)),
        });
        positions.push_back(previous);
    }
    manager.flushAll();
    manager.rotateSegment();
    const auto before = manager.stats();
    const auto floor = positions[positions.size() * 3 / 4];
    std::vector<std::uint64_t> latency;
    std::uint64_t reclaimed = 0;
    measure(latency, [&] { reclaimed = manager.reclaimSegmentsBefore(floor); });
    manager.validate();
    const auto after = manager.stats();
    if (reclaimed == 0 || after.segmentsDeleted == 0
        || after.physicalWalBytes >= before.physicalWalBytes) {
        throw std::runtime_error("WAL reclamation benchmark did not reclaim segments");
    }
    BenchmarkResult result;
    result.benchmark = "wal_reclamation";
    result.storageBackend = "segmented_wal";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(latency, totalLatency(latency));
    result.wal.walRecords = config.operations;
    result.wal.walPayloadBytes = config.operations * config.walPayloadBytes;
    result.wal.physicalWalBytesBefore = before.physicalWalBytes;
    result.wal.manager = after;
    result.checkpoint.segmentsReclaimed = after.segmentsDeleted;
    result.checkpoint.walBytesReclaimed = reclaimed;
    result.checkpoint.reclamationDurationNs = totalLatency(latency);
    result.environment = currentEnvironment();
    result.validationPassed = true;
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runPager(const BenchmarkConfig& config, std::string name) {
    removeDatabase(config);
    {
        Pager setup(config.databasePath);
        for (std::uint64_t index = 0; index < config.pages; ++index) {
            const auto pageId = setup.allocatePage();
            auto& page = setup.getPage(pageId);
            page[0] = static_cast<std::byte>(pageId & 0xFFU);
            setup.markDirty(pageId);
        }
        setup.flushAll();
    }

    std::mt19937_64 random(config.seed);
    auto pager = std::make_unique<Pager>(config.databasePath);
    const auto setSize = workingSet(config.workingSet, config.pages);
    if (name == "pager_hot") {
        for (std::uint64_t index = 0; index < setSize; ++index) {
            static_cast<void>(pager->getPage(static_cast<PageId>(index + 1U)));
        }
    }
    const auto storageBefore = storageMetrics(*pager);
    pager->resetStats();

    PagerStats accumulated{};
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    std::uint64_t checksum = 0;
    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        if (config.cacheMode == CacheMode::Reopen && operation != 0
            && operation % config.reopenInterval == 0) {
            accumulated = accumulatePagerStats(accumulated, pager->stats());
            pager = std::make_unique<Pager>(config.databasePath);
            pager->resetStats();
        }
        std::uint64_t index = operation % setSize;
        if (name == "pager_random") index = random() % setSize;
        const auto pageId = static_cast<PageId>(index + 1U);
        measure(latencies, [&] { checksum += std::to_integer<std::uint8_t>(pager->getPage(pageId)[0]); });
    }
    accumulated = accumulatePagerStats(accumulated, pager->stats());
    if (checksum == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("impossible pager benchmark checksum");
    }
    for (std::uint64_t index = 0; index < std::min<std::uint64_t>(config.pages, 8); ++index) {
        const auto pageId = static_cast<PageId>(index + 1U);
        if (pager->getPage(pageId)[0] != static_cast<std::byte>(pageId & 0xFFU)) {
            throw std::runtime_error("pager benchmark validation failed");
        }
    }
    const auto storageAfter = storageMetrics(*pager);
    auto result = finish(
        name, config, std::move(latencies), accumulated, storageBefore, storageAfter);
    pager.reset();
    cleanupDatabase(config);
    return result;
}

StorageMetrics bufferStorageMetrics(
    const DiskManager& diskManager,
    const BufferPoolManager& bufferPool) {
    return StorageMetrics{
        diskManager.pageCount(),
        static_cast<std::uint64_t>(diskManager.pageCount()) * DiskManager::PAGE_SIZE,
        0,
        bufferPool.residentPageCount(),
    };
}

BenchmarkResult runBuffer(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    {
        DiskManager setup(config.databasePath);
        for (std::uint64_t index = 0; index < config.pages; ++index) {
            const auto pageId = setup.appendPage();
            DiskManager::Page page{};
            page[0] = static_cast<std::byte>(pageId & 0xFFU);
            page[1] = static_cast<std::byte>((pageId * 7U) & 0xFFU);
            setup.writePage(pageId, page);
        }
    }

    auto disk = std::make_unique<DiskManager>(config.databasePath);
    auto pool = std::make_unique<BufferPoolManager>(
        *disk,
        static_cast<std::size_t>(config.bufferFrames),
        static_cast<std::size_t>(config.lruK));
    const auto configuredSet = workingSet(config.workingSet, config.pages);
    const auto requestedHotSet = config.workingSet == 0
        ? std::max<std::uint64_t>(1, std::min<std::uint64_t>(
            config.pages, std::max<std::uint64_t>(1, config.bufferFrames / 2U)))
        : configuredSet;
    const auto hotSet = name == "buffer_scan_resistance" && config.pages > 1
        ? std::min<std::uint64_t>(requestedHotSet, config.pages - 1U)
        : requestedHotSet;

    if (config.cacheMode == CacheMode::Hot
        && (name == "buffer_hotset" || name == "buffer_scan_resistance")) {
        for (std::uint64_t index = 0; index < config.warmupOperations; ++index) {
            auto guard = pool->fetchPageRead(static_cast<PageId>((index % hotSet) + 1U));
            if (!guard.has_value()) throw std::runtime_error("buffer warmup lacked a frame");
        }
    }
    const auto storageBefore = bufferStorageMetrics(*disk, *pool);
    pool->resetStats();

    BufferPoolStats accumulated{};
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    std::mt19937_64 random(config.seed);
    std::uint64_t checksum = 0;
    std::uint64_t scanCursor = hotSet < config.pages ? hotSet : 0;

    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        if (config.cacheMode == CacheMode::Reopen && operation != 0
            && operation % config.reopenInterval == 0) {
            accumulated = accumulateBufferStats(accumulated, pool->stats());
            pool.reset();
            disk.reset();
            disk = std::make_unique<DiskManager>(config.databasePath);
            pool = std::make_unique<BufferPoolManager>(
                *disk,
                static_cast<std::size_t>(config.bufferFrames),
                static_cast<std::size_t>(config.lruK));
            pool->resetStats();
        }

        std::uint64_t pageIndex = operation % config.pages;
        if (name == "buffer_random") {
            pageIndex = random() % configuredSet;
        } else if (name == "buffer_hotset") {
            pageIndex = random() % hotSet;
        } else if (name == "buffer_scan_resistance") {
            if (operation % 5U == 4U) {
                pageIndex = scanCursor;
                scanCursor = hotSet < config.pages
                    ? hotSet + ((scanCursor - hotSet + 1U) % (config.pages - hotSet))
                    : (scanCursor + 1U) % config.pages;
            } else {
                pageIndex = random() % hotSet;
            }
        }
        const auto pageId = static_cast<PageId>(pageIndex + 1U);
        measure(latencies, [&] {
            auto guard = pool->fetchPageRead(pageId);
            if (!guard.has_value()) throw std::runtime_error("buffer benchmark lacked a frame");
            checksum += std::to_integer<std::uint8_t>(guard->data()[0]);
        });
    }
    accumulated = accumulateBufferStats(accumulated, pool->stats());
    pool->validate();
    if (checksum == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("impossible buffer benchmark checksum");
    }
    const auto storageAfter = bufferStorageMetrics(*disk, *pool);
    auto result = finish(
        name,
        config,
        std::move(latencies),
        {},
        storageBefore,
        storageAfter,
        0.0,
        0.0,
        accumulated);
    pool.reset();
    disk.reset();
    cleanupDatabase(config);
    return result;
}

struct TreeContext {
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<BufferPoolManager> bufferPool;
    std::unique_ptr<PageAllocator> allocator;
    std::optional<PersistentBPlusTree> tree;
    PageId metadataPageId = INVALID_PAGE_ID;
};

TreeContext createTree(const BenchmarkConfig& config) {
    TreeContext context;
    context.disk = std::make_unique<DiskManager>(config.databasePath);
    context.bufferPool = std::make_unique<BufferPoolManager>(
        *context.disk, config.bufferFrames, config.lruK);
    context.allocator = std::make_unique<PageAllocator>(*context.bufferPool, *context.disk);
    context.tree.emplace(PersistentBPlusTree::create(
        *context.bufferPool, *context.disk, *context.allocator));
    context.metadataPageId = context.tree->metadataPageId();
    return context;
}

void reopenTree(TreeContext& context, const BenchmarkConfig& config) {
    context.tree.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    context.disk = std::make_unique<DiskManager>(config.databasePath);
    context.bufferPool = std::make_unique<BufferPoolManager>(
        *context.disk, config.bufferFrames, config.lruK);
    context.allocator = std::make_unique<PageAllocator>(*context.bufferPool, *context.disk);
    context.tree.emplace(PersistentBPlusTree::open(
        *context.bufferPool, *context.disk, *context.allocator, context.metadataPageId));
}

void populateTree(TreeContext& context, std::uint64_t rows, std::uint64_t seed, bool randomOrder) {
    std::vector<IndexKey> keys(static_cast<std::size_t>(rows));
    std::iota(keys.begin(), keys.end(), IndexKey{0});
    if (randomOrder) {
        std::mt19937_64 random(seed);
        std::shuffle(keys.begin(), keys.end(), random);
    }
    for (const auto key : keys) {
        if (!context.tree->insert(key, syntheticRecordId(key))) {
            throw std::runtime_error("B+ benchmark setup inserted a duplicate key");
        }
    }
    context.bufferPool->flushAll();
}

BenchmarkResult runBplus(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    auto context = createTree(config);
    std::mt19937_64 random(config.seed);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    BufferPoolStats accumulated{};
    StorageMetrics storageBefore{};

    if (name == "bplus_insert_sequential" || name == "bplus_insert_random") {
        std::vector<IndexKey> keys(static_cast<std::size_t>(config.operations));
        std::iota(keys.begin(), keys.end(), IndexKey{0});
        if (name == "bplus_insert_random") std::shuffle(keys.begin(), keys.end(), random);
        storageBefore = storageMetrics(*context.disk, *context.bufferPool, *context.allocator);
        context.bufferPool->resetStats();
        for (const auto key : keys) {
            measure(latencies, [&] {
                if (!context.tree->insert(key, syntheticRecordId(key))) {
                    throw std::runtime_error("B+ insertion benchmark saw duplicate key");
                }
            });
        }
    } else {
        populateTree(context, config.rows, config.seed, true);
        if (config.cacheMode == CacheMode::Reopen) reopenTree(context, config);
        const auto setSize = workingSet(config.workingSet, config.rows);
        for (std::uint64_t index = 0;
             config.cacheMode == CacheMode::Hot && index < config.warmupOperations; ++index) {
            static_cast<void>(context.tree->find(static_cast<IndexKey>(index % setSize)));
        }
        storageBefore = storageMetrics(*context.disk, *context.bufferPool, *context.allocator);
        context.bufferPool->resetStats();
        std::vector<IndexKey> liveKeys(static_cast<std::size_t>(config.rows));
        std::iota(liveKeys.begin(), liveKeys.end(), IndexKey{0});
        IndexKey nextKey = static_cast<IndexKey>(config.rows);
        for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
            if (config.cacheMode == CacheMode::Reopen && operation != 0
                && operation % config.reopenInterval == 0) {
                accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
                reopenTree(context, config);
                context.bufferPool->resetStats();
            }
            if (name == "bplus_find_hit") {
                const auto key = static_cast<IndexKey>(random() % setSize);
                measure(latencies, [&] {
                    if (!context.tree->find(key).has_value()) {
                        throw std::runtime_error("B+ successful lookup missed");
                    }
                });
            } else if (name == "bplus_find_miss") {
                const auto key = static_cast<IndexKey>(config.rows + 1U + (random() % setSize));
                measure(latencies, [&] {
                    if (context.tree->find(key).has_value()) {
                        throw std::runtime_error("B+ unsuccessful lookup found a key");
                    }
                });
            } else if (name == "bplus_range") {
                const auto lower = static_cast<IndexKey>(random() % setSize);
                const auto upper = static_cast<IndexKey>(std::min<std::uint64_t>(
                    config.rows - 1U, static_cast<std::uint64_t>(lower) + 31U));
                measure(latencies, [&] {
                    const auto entries = context.tree->rangeScan(lower, upper);
                    if (entries.empty()) throw std::runtime_error("B+ range scan was empty");
                });
            } else {
                const auto choice = random() % 100U;
                if (choice < 60U && !liveKeys.empty()) {
                    const auto key = liveKeys[random() % liveKeys.size()];
                    measure(latencies, [&] {
                        if (!context.tree->find(key).has_value()) {
                            throw std::runtime_error("B+ mixed lookup missed live key");
                        }
                    });
                } else if (choice < 80U || liveKeys.empty()) {
                    const auto key = nextKey++;
                    measure(latencies, [&] {
                        if (!context.tree->insert(key, syntheticRecordId(key))) {
                            throw std::runtime_error("B+ mixed insert failed");
                        }
                    });
                    liveKeys.push_back(key);
                } else {
                    const auto index = static_cast<std::size_t>(random() % liveKeys.size());
                    const auto key = liveKeys[index];
                    measure(latencies, [&] {
                        if (!context.tree->erase(key)) {
                            throw std::runtime_error("B+ mixed erase failed");
                        }
                    });
                    liveKeys[index] = liveKeys.back();
                    liveKeys.pop_back();
                }
            }
        }
    }
    accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
    context.tree->validate();
    const auto storageAfter = storageMetrics(
        *context.disk, *context.bufferPool, *context.allocator);
    auto result = finish(
        name, config, std::move(latencies), {}, storageBefore, storageAfter,
        0.0, 0.0, accumulated);
    context.tree.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    cleanupDatabase(config);
    return result;
}

struct TupleContext {
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<BufferPoolManager> bufferPool;
    std::unique_ptr<PageAllocator> allocator;
    std::optional<TupleStore> store;
    PageId metadataPageId = INVALID_PAGE_ID;
};

TupleContext createTupleStore(const BenchmarkConfig& config) {
    TupleContext context;
    context.disk = std::make_unique<DiskManager>(config.databasePath);
    context.bufferPool = std::make_unique<BufferPoolManager>(
        *context.disk, config.bufferFrames, config.lruK);
    context.allocator = std::make_unique<PageAllocator>(*context.bufferPool, *context.disk);
    context.store.emplace(TupleStore::create(
        *context.bufferPool, *context.disk, *context.allocator));
    context.metadataPageId = context.store->metadataPageId();
    return context;
}

void reopenTupleStore(TupleContext& context, const BenchmarkConfig& config) {
    context.store.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    context.disk = std::make_unique<DiskManager>(config.databasePath);
    context.bufferPool = std::make_unique<BufferPoolManager>(
        *context.disk, config.bufferFrames, config.lruK);
    context.allocator = std::make_unique<PageAllocator>(*context.bufferPool, *context.disk);
    context.store.emplace(TupleStore::open(
        *context.bufferPool, *context.disk, *context.allocator, context.metadataPageId));
}

std::vector<RecordId> populateTuples(
    TupleContext& context,
    const BenchmarkConfig& config,
    std::uint64_t count) {
    std::vector<RecordId> recordIds;
    recordIds.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto tuple = tuplePayload(config.seed, index, config.tupleSizes);
        recordIds.push_back(context.store->insert(tuple));
    }
    context.bufferPool->flushAll();
    return recordIds;
}

BenchmarkResult runTuple(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    auto context = createTupleStore(config);
    const auto setupCount = name == "tuple_erase"
        ? std::max(config.rows, config.operations) : config.rows;
    std::vector<RecordId> recordIds;
    if (name != "tuple_insert") recordIds = populateTuples(context, config, setupCount);
    if (config.cacheMode == CacheMode::Reopen && name != "tuple_insert") {
        reopenTupleStore(context, config);
    }
    const auto setSize = name == "tuple_insert" ? 1U
        : workingSet(config.workingSet, recordIds.size());
    for (std::uint64_t index = 0;
         config.cacheMode == CacheMode::Hot && name == "tuple_lookup"
             && index < config.warmupOperations; ++index) {
        static_cast<void>(context.store->get(recordIds[index % setSize]));
    }
    const auto storageBefore = storageMetrics(
        *context.disk, *context.bufferPool, *context.allocator);
    context.bufferPool->resetStats();
    BufferPoolStats accumulated{};
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    std::mt19937_64 random(config.seed);
    std::vector<std::optional<TupleBytes>> model;
    if (name == "tuple_fragmentation") {
        model.reserve(recordIds.size());
        for (std::size_t index = 0; index < recordIds.size(); ++index) {
            model.push_back(tuplePayload(config.seed, index, config.tupleSizes));
        }
    }

    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        if (config.cacheMode == CacheMode::Reopen && operation != 0
            && operation % config.reopenInterval == 0 && name != "tuple_insert") {
            accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
            reopenTupleStore(context, config);
            context.bufferPool->resetStats();
        }
        if (name == "tuple_insert") {
            const auto tuple = tuplePayload(config.seed, operation, config.tupleSizes);
            measure(latencies, [&] { recordIds.push_back(context.store->insert(tuple)); });
        } else if (name == "tuple_lookup") {
            const auto index = static_cast<std::size_t>(random() % setSize);
            measure(latencies, [&] {
                if (context.store->get(recordIds[index]).empty()) {
                    throw std::runtime_error("TupleStore lookup returned empty tuple");
                }
            });
        } else if (name == "tuple_update") {
            const auto index = static_cast<std::size_t>(operation % setSize);
            const auto original = context.store->get(recordIds[index]);
            auto replacement = original;
            replacement[0] = static_cast<std::byte>(operation & 0xFFU);
            measure(latencies, [&] {
                if (!context.store->tryUpdate(recordIds[index], replacement)) {
                    throw std::runtime_error("TupleStore same-size update failed");
                }
            });
        } else if (name == "tuple_erase") {
            measure(latencies, [&] { context.store->erase(recordIds[operation]); });
        } else if (name == "tuple_scan") {
            measure(latencies, [&] {
                if (context.store->scan().size() != recordIds.size()) {
                    throw std::runtime_error("TupleStore scan count changed");
                }
            });
        } else {
            const auto index = static_cast<std::size_t>(operation % model.size());
            const auto choice = operation % 3U;
            if (choice == 0 && model[index].has_value()) {
                measure(latencies, [&] { context.store->erase(recordIds[index]); });
                model[index].reset();
            } else if (choice == 1 && !model[index].has_value()) {
                auto tuple = tuplePayload(config.seed, index, config.tupleSizes, operation + 1U);
                measure(latencies, [&] { recordIds[index] = context.store->insert(tuple); });
                model[index] = std::move(tuple);
            } else if (model[index].has_value()) {
                auto replacement = *model[index];
                replacement[0] = static_cast<std::byte>((operation + 7U) & 0xFFU);
                measure(latencies, [&] {
                    if (!context.store->tryUpdate(recordIds[index], replacement)) {
                        throw std::runtime_error("fragmentation update failed");
                    }
                });
                model[index] = std::move(replacement);
            } else {
                measure(latencies, [&] { static_cast<void>(context.store->scan()); });
            }
        }
    }
    accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
    context.store->validate();
    if (name == "tuple_fragmentation") {
        for (std::size_t index = 0; index < model.size(); ++index) {
            if (model[index].has_value()
                && context.store->get(recordIds[index]) != *model[index]) {
                throw std::runtime_error("fragmentation model payload mismatch");
            }
        }
    }
    const auto storageAfter = storageMetrics(
        *context.disk, *context.bufferPool, *context.allocator);
    auto result = finish(
        name, config, std::move(latencies), {}, storageBefore, storageAfter,
        0.0, 0.0, accumulated);
    context.store.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    cleanupDatabase(config);
    return result;
}

struct SqlContext {
    std::unique_ptr<DiskManager> disk;
    std::unique_ptr<BufferPoolManager> bufferPool;
    std::unique_ptr<PageAllocator> allocator;
    std::optional<Catalog> catalog;
    std::unique_ptr<sql::SqlEngine> engine;

    explicit SqlContext(const BenchmarkConfig& config)
        : disk(std::make_unique<DiskManager>(config.databasePath)),
          bufferPool(std::make_unique<BufferPoolManager>(
              *disk, config.bufferFrames, config.lruK)),
          allocator(std::make_unique<PageAllocator>(*bufferPool, *disk)) {
        catalog.emplace(Catalog::openOrCreate(*bufferPool, *disk, *allocator));
        engine = std::make_unique<sql::SqlEngine>(*catalog);
    }
};

void reopenSql(SqlContext& context, const BenchmarkConfig& config) {
    context.engine.reset();
    context.catalog.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    context.disk = std::make_unique<DiskManager>(config.databasePath);
    context.bufferPool = std::make_unique<BufferPoolManager>(
        *context.disk, config.bufferFrames, config.lruK);
    context.allocator = std::make_unique<PageAllocator>(
        *context.bufferPool, *context.disk);
    context.catalog.emplace(Catalog::openOrCreate(
        *context.bufferPool, *context.disk, *context.allocator));
    context.engine = std::make_unique<sql::SqlEngine>(*context.catalog);
}

void createUsers(sql::SqlEngine& engine) {
    static_cast<void>(engine.execute(
        "CREATE TABLE users (id UINT32 PRIMARY KEY, username VARCHAR(64) NOT NULL, "
        "score INT64, active BOOLEAN NOT NULL)"));
}

void insertUser(sql::SqlEngine& engine, std::uint64_t key) {
    static_cast<void>(engine.execute(
        "INSERT INTO users VALUES (" + std::to_string(key) + ", 'user"
        + std::to_string(key) + "', " + std::to_string(key) + ", TRUE)"));
}

void populateUsers(sql::SqlEngine& engine, std::uint64_t rows) {
    for (std::uint64_t key = 0; key < rows; ++key) insertUser(engine, key);
}

std::pair<std::uint64_t, std::uint64_t> executionCounters(const sql::QueryResult& result) {
    return std::visit([](const auto& value) {
        return std::pair{value.stats.rowsExamined, value.stats.indexLookups};
    }, result);
}

BenchmarkResult runSql(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    SqlContext context(config);
    createUsers(*context.engine);
    if (name != "sql_insert") populateUsers(*context.engine, config.rows);
    context.bufferPool->flushAll();
    if (config.cacheMode == CacheMode::Reopen) reopenSql(context, config);
    const auto setSize = workingSet(config.workingSet, config.rows);
    if (config.cacheMode == CacheMode::Hot
        && (name == "sql_pk_lookup" || name == "sql_heap_scan")) {
        for (std::uint64_t index = 0; index < config.warmupOperations; ++index) {
            const auto key = index % setSize;
            const auto query = name == "sql_pk_lookup"
                ? "SELECT username FROM users WHERE id = " + std::to_string(key)
                : "SELECT id FROM users WHERE username = 'user" + std::to_string(key) + "'";
            static_cast<void>(context.engine->execute(query));
        }
    }
    const auto storageBefore = storageMetrics(
        *context.disk, *context.bufferPool, *context.allocator);
    context.bufferPool->resetStats();
    BufferPoolStats accumulated{};
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    std::mt19937_64 random(config.seed);
    std::vector<std::uint32_t> liveKeys(static_cast<std::size_t>(config.rows));
    std::iota(liveKeys.begin(), liveKeys.end(), 0U);
    std::uint64_t nextKey = config.rows;
    std::uint64_t rowsExamined = 0;
    std::uint64_t indexLookups = 0;

    for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
        if (config.cacheMode == CacheMode::Reopen && operation != 0
            && operation % config.reopenInterval == 0) {
            accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
            reopenSql(context, config);
            context.bufferPool->resetStats();
        }
        sql::QueryResult result;
        if (name == "sql_pk_lookup") {
            const auto key = random() % setSize;
            measure(latencies, [&] { result = context.engine->execute(
                "SELECT username FROM users WHERE id = " + std::to_string(key)); });
            if (selectResult(result).rows.size() != 1) throw std::runtime_error("SQL PK miss");
        } else if (name == "sql_heap_scan") {
            const auto key = random() % setSize;
            measure(latencies, [&] { result = context.engine->execute(
                "SELECT id FROM users WHERE username = 'user" + std::to_string(key) + "'"); });
            if (selectResult(result).rows.size() != 1) throw std::runtime_error("SQL scan miss");
        } else if (name == "sql_insert") {
            const auto key = operation;
            measure(latencies, [&] { result = context.engine->execute(
                "INSERT INTO users VALUES (" + std::to_string(key) + ", 'user"
                + std::to_string(key) + "', " + std::to_string(key) + ", TRUE)"); });
        } else if (name == "sql_update") {
            const auto key = operation % setSize;
            measure(latencies, [&] { result = context.engine->execute(
                "UPDATE users SET score = " + std::to_string(operation + 1U)
                + " WHERE id = " + std::to_string(key)); });
        } else if (name == "sql_delete") {
            const auto key = operation % setSize;
            measure(latencies, [&] { result = context.engine->execute(
                "DELETE FROM users WHERE id = " + std::to_string(key)); });
        } else {
            unsigned pkRead = 70, scanRead = 80, insertLimit = 90, updateLimit = 95;
            if (name == "mixed_read_heavy") {
                pkRead = 90; scanRead = 95; insertLimit = 97; updateLimit = 99;
            } else if (name == "mixed_write_heavy") {
                pkRead = 45; scanRead = 50; insertLimit = 70; updateLimit = 85;
            }
            const auto choice = static_cast<unsigned>(random() % 100U);
            if (choice < pkRead && !liveKeys.empty()) {
                const auto key = liveKeys[random() % liveKeys.size()];
                measure(latencies, [&] { result = context.engine->execute(
                    "SELECT username FROM users WHERE id = " + std::to_string(key)); });
            } else if (choice < scanRead && !liveKeys.empty()) {
                const auto key = liveKeys[random() % liveKeys.size()];
                measure(latencies, [&] { result = context.engine->execute(
                    "SELECT id FROM users WHERE username = 'user" + std::to_string(key) + "'"); });
            } else if (choice < insertLimit || liveKeys.empty()) {
                const auto key = nextKey++;
                measure(latencies, [&] { result = context.engine->execute(
                    "INSERT INTO users VALUES (" + std::to_string(key) + ", 'user"
                    + std::to_string(key) + "', " + std::to_string(key) + ", TRUE)"); });
                liveKeys.push_back(static_cast<std::uint32_t>(key));
            } else if (choice < updateLimit) {
                const auto key = liveKeys[random() % liveKeys.size()];
                measure(latencies, [&] { result = context.engine->execute(
                    "UPDATE users SET active = FALSE WHERE id = " + std::to_string(key)); });
            } else {
                const auto index = static_cast<std::size_t>(random() % liveKeys.size());
                const auto key = liveKeys[index];
                measure(latencies, [&] { result = context.engine->execute(
                    "DELETE FROM users WHERE id = " + std::to_string(key)); });
                liveKeys[index] = liveKeys.back();
                liveKeys.pop_back();
            }
        }
        const auto [examined, lookups] = executionCounters(result);
        rowsExamined += examined;
        indexLookups += lookups;
    }
    accumulated = accumulateBufferStats(accumulated, context.bufferPool->stats());
    context.catalog->validate();
    if (name == "sql_insert" && context.catalog->openTable("users").size() != config.operations) {
        throw std::runtime_error("SQL insert benchmark size validation failed");
    }
    if ((name == "sql_mixed" || name == "mixed_read_heavy" || name == "mixed_write_heavy")
        && context.catalog->openTable("users").size() != liveKeys.size()) {
        throw std::runtime_error("SQL mixed benchmark model size mismatch");
    }
    const auto storageAfter = storageMetrics(
        *context.disk, *context.bufferPool, *context.allocator);
    auto result = finish(
        name, config, std::move(latencies), {}, storageBefore, storageAfter,
        static_cast<double>(rowsExamined) / static_cast<double>(config.operations),
        static_cast<double>(indexLookups) / static_cast<double>(config.operations),
        accumulated);
    context.engine.reset();
    context.catalog.reset();
    context.allocator.reset();
    context.bufferPool.reset();
    context.disk.reset();
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runTcp(const BenchmarkConfig& config, const std::string& name) {
    removeDatabase(config);
    {
        SqlContext context(config);
        createUsers(*context.engine);
        if (name != "tcp_insert") populateUsers(*context.engine, config.rows);
        context.bufferPool->flushAll();
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    BufferPoolStats accumulated{};
    StorageMetrics initialStorage{};
    StorageMetrics finalStorage{};
    bool capturedInitialStorage = false;
    std::uint64_t rowsExamined = 0;
    std::uint64_t indexLookups = 0;
    std::mt19937_64 random(config.seed);
    std::vector<std::uint32_t> liveKeys(static_cast<std::size_t>(
        name == "tcp_insert" ? 0 : config.rows));
    std::iota(liveKeys.begin(), liveKeys.end(), 0U);
    std::uint64_t nextKey = name == "tcp_insert" ? 0 : config.rows;
    std::uint64_t completed = 0;
    const auto batchSize = config.cacheMode == CacheMode::Reopen
        ? config.reopenInterval : config.operations;

    while (completed < config.operations) {
        const auto batchEnd = std::min(config.operations, completed + batchSize);
        net::DatabaseServer server(
            config.databasePath,
            net::ServerConfig{
                "127.0.0.1",
                0,
                8,
                static_cast<std::size_t>(config.bufferFrames),
                static_cast<std::size_t>(config.lruK),
                config.checkpointWalBytes,
                config.checkpointStatements,
                config.walSegmentBytes,
                config.walUpdateMode});
        server.start();
        std::exception_ptr serverError;
        std::thread serverThread([&] {
            try { server.serve(1); }
            catch (...) { serverError = std::current_exception(); }
        });
        std::exception_ptr clientError;
        try {
            net::MiniDbClient client("127.0.0.1", server.port());
            client.connect(); client.handshake();
            if (completed == 0) {
                for (std::uint64_t warmup = 0;
                     warmup < config.warmupOperations && !liveKeys.empty(); ++warmup) {
                    static_cast<void>(client.execute(
                        "SELECT username FROM users WHERE id = "
                        + std::to_string(liveKeys[warmup % liveKeys.size()])));
                }
            }
            if (!capturedInitialStorage) {
                initialStorage = storageMetrics(
                    server.diskManager(), server.bufferPool(), server.pageAllocator());
                capturedInitialStorage = true;
            }
            server.bufferPool().resetStats();
            for (; completed < batchEnd; ++completed) {
                sql::QueryResult result;
                if (name == "tcp_pk_lookup") {
                    const auto setSize = workingSet(config.workingSet, config.rows);
                    const auto key = random() % setSize;
                    measure(latencies, [&] { result = client.execute(
                        "SELECT username FROM users WHERE id = " + std::to_string(key)); });
                } else if (name == "tcp_heap_scan") {
                    const auto setSize = workingSet(config.workingSet, config.rows);
                    const auto key = random() % setSize;
                    measure(latencies, [&] { result = client.execute(
                        "SELECT id FROM users WHERE username = 'user" + std::to_string(key) + "'"); });
                } else if (name == "tcp_insert") {
                    const auto key = nextKey++;
                    measure(latencies, [&] { result = client.execute(
                        "INSERT INTO users VALUES (" + std::to_string(key) + ", 'user"
                        + std::to_string(key) + "', " + std::to_string(key) + ", TRUE)"); });
                    liveKeys.push_back(static_cast<std::uint32_t>(key));
                } else {
                    const auto choice = random() % 100U;
                    if (choice < 70U && !liveKeys.empty()) {
                        const auto key = liveKeys[random() % liveKeys.size()];
                        measure(latencies, [&] { result = client.execute(
                            "SELECT username FROM users WHERE id = " + std::to_string(key)); });
                    } else if (choice < 80U && !liveKeys.empty()) {
                        const auto key = liveKeys[random() % liveKeys.size()];
                        measure(latencies, [&] { result = client.execute(
                            "SELECT id FROM users WHERE username = 'user" + std::to_string(key) + "'"); });
                    } else if (choice < 90U || liveKeys.empty()) {
                        const auto key = nextKey++;
                        measure(latencies, [&] { result = client.execute(
                            "INSERT INTO users VALUES (" + std::to_string(key) + ", 'user"
                            + std::to_string(key) + "', " + std::to_string(key) + ", TRUE)"); });
                        liveKeys.push_back(static_cast<std::uint32_t>(key));
                    } else if (choice < 95U) {
                        const auto key = liveKeys[random() % liveKeys.size()];
                        measure(latencies, [&] { result = client.execute(
                            "UPDATE users SET active = FALSE WHERE id = " + std::to_string(key)); });
                    } else {
                        const auto index = static_cast<std::size_t>(random() % liveKeys.size());
                        const auto key = liveKeys[index];
                        measure(latencies, [&] { result = client.execute(
                            "DELETE FROM users WHERE id = " + std::to_string(key)); });
                        liveKeys[index] = liveKeys.back(); liveKeys.pop_back();
                    }
                }
                const auto [examined, lookups] = executionCounters(result);
                rowsExamined += examined; indexLookups += lookups;
            }
            client.close();
        } catch (...) {
            clientError = std::current_exception();
        }
        serverThread.join();
        if (serverError) std::rethrow_exception(serverError);
        if (clientError) std::rethrow_exception(clientError);
        accumulated = accumulateBufferStats(accumulated, server.bufferPool().stats());
        server.catalog().validate();
        finalStorage = storageMetrics(
            server.diskManager(), server.bufferPool(), server.pageAllocator());
    }

    auto result = finish(
        name, config, std::move(latencies), {}, initialStorage, finalStorage,
        static_cast<double>(rowsExamined) / static_cast<double>(config.operations),
        static_cast<double>(indexLookups) / static_cast<double>(config.operations),
        accumulated);
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runTransactional(
    const BenchmarkConfig& config,
    const std::string& name) {
    removeDatabase(config);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(config.operations));
    BenchmarkResult result;
    {
        net::DatabaseServer server(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK),
                              config.checkpointWalBytes,
                              config.checkpointStatements,
                              config.walSegmentBytes,
                              config.walUpdateMode});
        auto& engine = server.sqlEngine();
        const bool rawWalWorkload = name == "txn_wal_delta_friendly"
            || name == "txn_wal_fragmentation";
        PageId rawWalPage = INVALID_PAGE_ID;
        std::uint64_t setupRows = 0;
        if (rawWalWorkload) {
            server.recoveryCoordinator().beginStatement();
            rawWalPage = server.pageAllocator().allocatePage();
            server.recoveryCoordinator().commitStatement();
        } else {
            createUsers(engine);
            setupRows = name == "txn_insert" ? 0U
                : name == "txn_bplus_insert" ? config.rows
                : std::max(config.rows, config.operations);
            populateUsers(engine, setupRows);
        }
        server.bufferPool().flushAll();
        const auto before = storageMetrics(
            server.diskManager(), server.bufferPool(), server.pageAllocator());
        server.bufferPool().resetStats();
        server.logManager().resetStats();
        server.recoveryCoordinator().resetStats();
        server.checkpointManager().resetStats();
        for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
            if (rawWalWorkload) {
                measure(latencies, [&] {
                    server.recoveryCoordinator().beginStatement();
                    {
                        auto page = requireWritePage(
                            server.bufferPool(), rawWalPage, "run WAL encoding workload");
                        if (name == "txn_wal_fragmentation") {
                            for (std::size_t offset = 0; offset < page.data().size();
                                 offset += 2) {
                                page.data()[offset] ^= std::byte{0x5A};
                            }
                        } else {
                            constexpr std::size_t BEGIN = 128;
                            constexpr std::size_t LENGTH = 16;
                            for (std::size_t offset = BEGIN; offset < BEGIN + LENGTH;
                                 ++offset) {
                                page.data()[offset] ^= std::byte{0x33};
                            }
                        }
                    }
                    server.recoveryCoordinator().commitStatement();
                });
            } else if (name == "txn_insert" || name == "txn_bplus_insert") {
                const auto key = name == "txn_insert" ? operation : setupRows + operation;
                measure(latencies, [&] { static_cast<void>(engine.execute(
                    "INSERT INTO users VALUES (" + std::to_string(key)
                    + ", 'user" + std::to_string(key) + "', "
                    + std::to_string(key) + ", TRUE)")); });
            } else if (name == "txn_update") {
                measure(latencies, [&] { static_cast<void>(engine.execute(
                    "UPDATE users SET score = " + std::to_string(operation + 1000U)
                    + " WHERE id = " + std::to_string(operation) )); });
            } else if (name == "txn_varchar_update") {
                measure(latencies, [&] { static_cast<void>(engine.execute(
                    "UPDATE users SET username = 'updated_"
                    + std::to_string(operation) + "' WHERE id = "
                    + std::to_string(operation))); });
            } else if (name == "txn_delete") {
                measure(latencies, [&] { static_cast<void>(engine.execute(
                    "DELETE FROM users WHERE id = " + std::to_string(operation))); });
            } else {
                const auto key = operation % setupRows;
                if (operation % 3U == 0) {
                    measure(latencies, [&] { static_cast<void>(engine.execute(
                        "UPDATE users SET active = FALSE WHERE id = "
                        + std::to_string(key))); });
                } else if (operation % 3U == 1) {
                    const auto inserted = setupRows + operation;
                    measure(latencies, [&] { static_cast<void>(engine.execute(
                        "INSERT INTO users VALUES (" + std::to_string(inserted)
                        + ", 'mixed', 1, TRUE)")); });
                } else {
                    measure(latencies, [&] { static_cast<void>(engine.execute(
                        "DELETE FROM users WHERE id = " + std::to_string(key))); });
                }
            }
        }
        server.catalog().validate();
        server.pageAllocator().validate();
        const auto after = storageMetrics(
            server.diskManager(), server.bufferPool(), server.pageAllocator());
        result = finish(
            name, config, std::move(latencies), {}, before, after,
            0.0, 0.0, server.bufferPool().stats());
        result.storageBackend = "buffer_pool_" + std::string(walUpdateModeName(
            config.walUpdateMode)) + "_wal";
        result.wal.manager = server.logManager().stats();
        result.wal.walRecords = result.wal.manager.recordsAppended;
        result.recovery.transactions = server.recoveryCoordinator().stats();
        result.checkpoint = server.checkpointManager().stats();
        result.recovery.walBytes = result.wal.manager.bytesAppended;
        result.recovery.logicalChangedBytes =
            result.recovery.transactions.logicalBytesChanged;
        if (result.recovery.logicalChangedBytes != 0) {
            result.recovery.payloadAmplification =
                static_cast<double>(result.recovery.transactions.walUpdatePayloadBytes)
                / static_cast<double>(result.recovery.logicalChangedBytes);
            result.recovery.totalWalAmplification =
                static_cast<double>(result.recovery.walBytes)
                / static_cast<double>(result.recovery.logicalChangedBytes);
        }
        result.recovery.loggingAmplification = result.recovery.totalWalAmplification;
        result.recovery.updateRecordBytes = summarizeDistribution(
            result.recovery.transactions.updateRecordBytes);
        result.recovery.rangesPerDelta = summarizeDistribution(
            result.recovery.transactions.deltaRangeCounts);
        const auto checkpoint = server.checkpointControl().select(server.logManager());
        const auto recoveryStart = checkpoint.slot.has_value()
            ? checkpoint.slot->recoveryStartOffset : server.logManager().oldestRetainedLsn();
        result.recovery.recovery.checkpointUsed = checkpoint.slot.has_value();
        result.recovery.recovery.recoveryStartOffset = recoveryStart;
        result.recovery.recovery.walBytesSkipped = recoveryStart
            - server.logManager().oldestRetainedLsn();
        result.recovery.recovery.walBytesScanned =
            server.logManager().lastValidOffset() - recoveryStart;
    }
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runRecoveryBenchmark(
    const BenchmarkConfig& config,
    const std::string& name) {
    removeDatabase(config);
    {
        net::DatabaseServer server(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK),
                              config.checkpointWalBytes,
                              config.checkpointStatements,
                              config.walSegmentBytes,
                              config.walUpdateMode});
        createUsers(server.sqlEngine());
        populateUsers(server.sqlEngine(), config.operations);
        if (name == "recovery_loser") {
            server.recoveryCoordinator().beginStatement();
            const auto pageId = server.pageAllocator().allocatePage();
            {
                auto page = requireWritePage(
                    server.bufferPool(), pageId, "prepare recovery loser benchmark");
                page.data()[0] = std::byte{0xA5};
            }
            server.bufferPool().flushAll();
        }
    }
    const auto walBytes = walPhysicalBytes(config.databasePath);
    std::vector<std::uint64_t> latency;
    RecoveryStats recovery;
    {
        DiskManager disk(config.databasePath);
        LogManager log(walPathForDatabase(config.databasePath), LogManager::DEFAULT_BUFFER_SIZE,
                       LogOpenMode::DeferredRecovery, WalStorageMode::Auto,
                       config.walSegmentBytes);
        CheckpointControl control(checkpointPathForDatabase(config.databasePath));
        measure(latency, [&] {
            recovery = RecoveryManager(disk, log, &control).recover();
        });
    }
    {
        net::DatabaseServer validation(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK),
                              config.checkpointWalBytes,
                              config.checkpointStatements,
                              config.walSegmentBytes,
                              config.walUpdateMode});
        validation.catalog().validate();
    }
    BenchmarkResult result;
    result.benchmark = name;
    result.storageBackend = "physical_recovery";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(latency, totalLatency(latency));
    result.recovery.recovery = recovery;
    result.recovery.walBytes = walBytes;
    result.environment = currentEnvironment();
    result.validationPassed = true;
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runCheckpointLatency(const BenchmarkConfig& config) {
    removeDatabase(config);
    BenchmarkResult result;
    std::vector<std::uint64_t> latency;
    {
        net::DatabaseServer server(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK), 0, 0,
                              config.walSegmentBytes,
                              config.walUpdateMode,
                              config.checkpointMode});
        static_cast<void>(server.checkpointManager().checkpoint(CheckpointMode::Sharp));
        server.checkpointManager().resetStats();
        server.bufferPool().resetStats();
        if (config.operations != 0) {
            server.recoveryCoordinator().beginStatement();
            for (std::uint64_t page = 0; page < config.operations; ++page) {
                const auto pageId = server.pageAllocator().allocatePage();
                auto guard = requireWritePage(
                    server.bufferPool(), pageId, "prepare checkpoint latency dirty page");
                guard.data()[128] = static_cast<std::byte>((page + 1) & 0xffU);
            }
            server.recoveryCoordinator().commitStatement();
        }
        measure(latency, [&] {
            static_cast<void>(server.checkpointManager().checkpoint(config.checkpointMode));
        });
        server.catalog().validate();
        server.pageAllocator().validate();
        result = finish("checkpoint_latency", config, latency, {}, {},
                        storageMetrics(server.diskManager(), server.bufferPool(),
                                       server.pageAllocator()),
                        0.0, 0.0, server.bufferPool().stats());
        result.storageBackend = config.checkpointMode == CheckpointMode::Sharp
            ? "sharp_checkpoint" : "fuzzy_checkpoint";
        result.checkpoint = server.checkpointManager().stats();
        result.wal.manager = server.logManager().stats();
    }
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runCheckpointRecoveryComparison(const BenchmarkConfig& config) {
    removeDatabase(config);
    constexpr std::uint64_t TAIL = 10;
    {
        net::DatabaseServer server(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK), 0, 0,
                              config.walSegmentBytes,
                              config.walUpdateMode,
                              config.checkpointMode});
        createUsers(server.sqlEngine());
        populateUsers(server.sqlEngine(), config.operations);
        static_cast<void>(server.checkpointManager().checkpoint(config.checkpointMode));
        for (std::uint64_t index = 0; index < TAIL; ++index) {
            const auto key = config.operations + index;
            static_cast<void>(server.sqlEngine().execute(
                "INSERT INTO users VALUES (" + std::to_string(key)
                + ", 'tail', 1, TRUE)"));
        }
    }
    RecoveryStats full;
    std::vector<std::uint64_t> fullLatency;
    {
        DiskManager disk(config.databasePath);
        LogManager log(walPathForDatabase(config.databasePath), LogManager::DEFAULT_BUFFER_SIZE,
                       LogOpenMode::DeferredRecovery, WalStorageMode::Auto,
                       config.walSegmentBytes);
        measure(fullLatency, [&] { full = RecoveryManager(disk, log, nullptr, true).recover(); });
    }
    RecoveryStats bounded;
    std::vector<std::uint64_t> boundedLatency;
    {
        DiskManager disk(config.databasePath);
        LogManager log(walPathForDatabase(config.databasePath), LogManager::DEFAULT_BUFFER_SIZE,
                       LogOpenMode::DeferredRecovery, WalStorageMode::Auto,
                       config.walSegmentBytes);
        CheckpointControl control(checkpointPathForDatabase(config.databasePath));
        measure(boundedLatency, [&] { bounded = RecoveryManager(disk, log, &control).recover(); });
    }
    {
        net::DatabaseServer validation(
            config.databasePath,
            net::ServerConfig{"127.0.0.1", 0, 8,
                              static_cast<std::size_t>(config.bufferFrames),
                              static_cast<std::size_t>(config.lruK), 0, 0,
                              config.walSegmentBytes,
                              config.walUpdateMode});
        validation.catalog().validate();
    }
    BenchmarkResult result;
    result.benchmark = "recovery_checkpoint_compare";
    result.storageBackend = config.checkpointMode == CheckpointMode::Sharp
        ? "sharp_checkpoint_recovery" : "fuzzy_checkpoint_recovery";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(boundedLatency, totalLatency(boundedLatency));
    result.recovery.recovery = bounded;
    result.recovery.fullScanRecovery = full;
    result.recovery.walBytes = walPhysicalBytes(config.databasePath);
    result.environment = currentEnvironment();
    result.validationPassed = bounded.checkpointUsed
        && bounded.checkpointMode == config.checkpointMode
        && (config.checkpointMode == CheckpointMode::Fuzzy
            || bounded.recordsAnalyzed < full.recordsAnalyzed);
    cleanupDatabase(config);
    return result;
}

BenchmarkResult runPageLsnRecoveryComparison(const BenchmarkConfig& config) {
    removeDatabase(config);
    PageId pageId = INVALID_PAGE_ID;
    {
        DiskManager disk(config.databasePath);
        pageId = disk.appendPage();
        DiskManager::Page initial{};
        std::copy(free_page_layout::MAGIC.begin(), free_page_layout::MAGIC.end(),
                  initial.begin());
        byte_codec::writeUint32(
            initial, free_page_layout::LAYOUT_VERSION_OFFSET,
            free_page_layout::CURRENT_VERSION);
        byte_codec::writeUint32(
            initial, free_page_layout::HEADER_SIZE_OFFSET,
            free_page_layout::HEADER_SIZE);
        byte_codec::writeUint32(
            initial, free_page_layout::NEXT_FREE_PAGE_ID_OFFSET, INVALID_PAGE_ID);
        disk.writePhysicalPage(pageId, initial);
        disk.sync();

        LogManager log(
            walPathForDatabase(config.databasePath),
            static_cast<std::size_t>(config.walBufferBytes));
        RecoveryCoordinator coordinator(
            disk, log, INVALID_TRANSACTION_ID, config.walUpdateMode);
        BufferPoolManager pool(
            disk, static_cast<std::size_t>(std::max<std::uint64_t>(2, config.bufferFrames)),
            static_cast<std::size_t>(config.lruK), &log, &coordinator);
        coordinator.attachBufferPool(pool);
        const auto persistedUpdates = (config.operations * config.redoPersistedPercent) / 100U;
        for (std::uint64_t operation = 0; operation < config.operations; ++operation) {
            coordinator.beginStatement();
            {
                auto guard = requireWritePage(pool, pageId, "PageLSN recovery benchmark update");
                byte_codec::writeUint64(guard.data(), 64, config.seed + operation + 1U);
            }
            coordinator.commitStatement();
            if (persistedUpdates != 0 && operation + 1U == persistedUpdates) {
                static_cast<void>(pool.flushPage(pageId));
            }
        }
        if (persistedUpdates < config.operations) pool.discardPageForRecovery(pageId);
        log.flushAll();
        disk.sync();
    }

    const auto selectiveDatabase = config.databasePath + ".selective";
    const auto alwaysDatabase = config.databasePath + ".always";
    const auto sourceWal = walPathForDatabase(config.databasePath);
    const auto selectiveWal = walPathForDatabase(selectiveDatabase);
    const auto alwaysWal = walPathForDatabase(alwaysDatabase);
    std::filesystem::copy_file(
        config.databasePath, selectiveDatabase,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        config.databasePath, alwaysDatabase,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        sourceWal, selectiveWal, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        sourceWal, alwaysWal, std::filesystem::copy_options::overwrite_existing);

    RecoveryStats selective;
    RecoveryStats always;
    std::vector<std::uint64_t> selectiveLatency;
    {
        DiskManager disk(selectiveDatabase);
        LogManager log(selectiveWal, LogManager::DEFAULT_BUFFER_SIZE,
                       LogOpenMode::DeferredRecovery);
        measure(selectiveLatency, [&] {
            selective = RecoveryManager(
                disk, log, nullptr, false,
                RedoPolicy::PageLsnSelectiveRedo).recover();
        });
    }
    std::vector<std::uint64_t> alwaysLatency;
    {
        DiskManager disk(alwaysDatabase);
        LogManager log(alwaysWal, LogManager::DEFAULT_BUFFER_SIZE,
                       LogOpenMode::DeferredRecovery);
        measure(alwaysLatency, [&] {
            always = RecoveryManager(
                disk, log, nullptr, false, RedoPolicy::AlwaysRedo).recover();
        });
    }
    DiskManager::Page selectivePage{};
    DiskManager::Page alwaysPage{};
    {
        DiskManager disk(selectiveDatabase);
        disk.readPhysicalPage(pageId, selectivePage);
    }
    {
        DiskManager disk(alwaysDatabase);
        disk.readPhysicalPage(pageId, alwaysPage);
    }

    BenchmarkResult result;
    result.benchmark = "recovery_page_lsn_compare";
    result.storageBackend = "persistent_page_lsn_selective_redo";
    result.seed = config.seed;
    result.configuration = config;
    result.timing = summarizeTimings(
        selectiveLatency, totalLatency(selectiveLatency));
    result.recovery.recovery = selective;
    // This existing comparison slot is the AlwaysRedo control for this benchmark.
    result.recovery.fullScanRecovery = always;
    result.recovery.walBytes = walPhysicalBytes(config.databasePath);
    result.environment = currentEnvironment();
    result.validationPassed = selectivePage == alwaysPage
        && selective.pageLsnChecks == config.operations
        && always.pagesRedone == config.operations;

    std::error_code error;
    std::filesystem::remove(selectiveDatabase, error);
    std::filesystem::remove(alwaysDatabase, error);
    std::filesystem::remove(selectiveWal, error);
    std::filesystem::remove(alwaysWal, error);
    cleanupDatabase(config);
    return result;
}

std::string canonicalName(const std::string& name) {
    if (name == "pager") return "pager_random";
    if (name == "buffer") return "buffer_random";
    if (name == "bplus") return "bplus_find_hit";
    if (name == "tuple") return "tuple_lookup";
    if (name == "sql") return "sql_pk_lookup";
    if (name == "tcp") return "tcp_pk_lookup";
    if (name == "mixed") return "sql_mixed";
    if (name == "wal") return "wal_append_buffered";
    return name;
}

BenchmarkResult runOne(const BenchmarkConfig& config, std::string name) {
    name = canonicalName(name);
    if (name == "wal_segment_rotation") return runWalSegmentRotation(config);
    if (name == "wal_reclamation") return runWalReclamation(config);
    if (name.starts_with("wal_")) return runWal(config, name);
    if (name.starts_with("txn_")) return runTransactional(config, name);
    if (name == "recovery_full_scan" || name == "recovery_loser") {
        return runRecoveryBenchmark(config, name);
    }
    if (name == "checkpoint_latency") return runCheckpointLatency(config);
    if (name == "recovery_checkpoint_compare") {
        return runCheckpointRecoveryComparison(config);
    }
    if (name == "recovery_page_lsn_compare") {
        return runPageLsnRecoveryComparison(config);
    }
    if (name.starts_with("pager_")) return runPager(config, std::move(name));
    if (name.starts_with("buffer_")) return runBuffer(config, name);
    if (name.starts_with("bplus_")) return runBplus(config, name);
    if (name.starts_with("tuple_")) return runTuple(config, name);
    if (name.starts_with("tcp_")) return runTcp(config, name);
    return runSql(config, name);
}

std::vector<std::string> suiteNames() {
    return {
        "pager_sequential", "pager_random", "buffer_random", "bplus_find_hit", "tuple_lookup",
        "sql_pk_lookup", "sql_heap_scan", "sql_mixed", "tcp_pk_lookup",
        "wal_append_buffered",
    };
}

} // namespace

std::vector<BenchmarkResult> runConfiguredBenchmarks(const BenchmarkConfig& configuration) {
    std::vector<std::string> names;
    BenchmarkConfig effective = configuration;
    if (!configuration.suite.empty()) {
        names = suiteNames();
        if (configuration.suite == "quick") {
            effective.rows = std::min<std::uint64_t>(effective.rows, 32);
            effective.operations = std::min<std::uint64_t>(effective.operations, 24);
            effective.pages = std::min<std::uint64_t>(effective.pages, 24);
            effective.warmupOperations = std::min<std::uint64_t>(effective.warmupOperations, 8);
            effective.reopenInterval = std::min<std::uint64_t>(effective.reopenInterval, 12);
            effective.bufferFrames = std::min<std::uint64_t>(effective.bufferFrames, 8);
            if (effective.workingSet != 0) {
                effective.workingSet = std::min(effective.workingSet, effective.rows);
            }
        }
    } else if (configuration.benchmark == "sql_pk_vs_heap") {
        names = {"sql_pk_lookup", "sql_heap_scan"};
    } else {
        names = {configuration.benchmark};
    }

    std::vector<BenchmarkResult> results;
    results.reserve(names.size() * effective.repetitions);
    for (std::uint32_t repetition = 1; repetition <= effective.repetitions; ++repetition) {
        for (const auto& name : names) {
            auto result = runOne(effective, name);
            result.repetition = repetition;
            results.push_back(std::move(result));
        }
    }
    return results;
}

} // namespace minidb::bench
