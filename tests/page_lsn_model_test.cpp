#include "minidb/byte_codec.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/recovery.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <vector>

namespace {

using minidb::test::require;

minidb::DiskManager::Page initialPage() {
    minidb::DiskManager::Page page{};
    std::copy(minidb::free_page_layout::MAGIC.begin(),
              minidb::free_page_layout::MAGIC.end(), page.begin());
    minidb::byte_codec::writeUint32(
        page, minidb::free_page_layout::LAYOUT_VERSION_OFFSET,
        minidb::free_page_layout::CURRENT_VERSION);
    minidb::byte_codec::writeUint32(
        page, minidb::free_page_layout::HEADER_SIZE_OFFSET,
        minidb::free_page_layout::HEADER_SIZE);
    minidb::byte_codec::writeUint32(
        page, minidb::free_page_layout::NEXT_FREE_PAGE_ID_OFFSET,
        minidb::INVALID_PAGE_ID);
    return page;
}

void runModel(std::uint64_t seed) {
    constexpr std::size_t PAGE_COUNT = 8;
    constexpr std::size_t OPERATIONS = 4'000;
    constexpr std::size_t VALUE_OFFSET = 64;
    minidb::test::TemporaryDatabase database("page_lsn_model");
    std::array<minidb::PageId, PAGE_COUNT> pageIds{};
    std::array<minidb::DiskManager::Page, PAGE_COUNT> logical{};
    std::array<minidb::DiskManager::Page, PAGE_COUNT> persisted{};
    std::mt19937_64 random(seed);

    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(minidb::walPathForDatabase(database.path().string()));
        for (std::size_t index = 0; index < PAGE_COUNT; ++index) {
            pageIds[index] = disk.appendPage();
            logical[index] = initialPage();
            persisted[index] = logical[index];
            disk.writePhysicalPage(pageIds[index], persisted[index]);
        }

        for (std::size_t operation = 0; operation < OPERATIONS; ++operation) {
            const auto forcedTail = operation >= OPERATIONS - PAGE_COUNT;
            const auto pageIndex = forcedTail
                ? operation - (OPERATIONS - PAGE_COUNT)
                : static_cast<std::size_t>(random() % PAGE_COUNT);
            auto before = logical[pageIndex];
            auto after = before;
            const auto value = random() ^ (seed + operation);
            minidb::byte_codec::writeUint64(after, VALUE_OFFSET, value);
            auto normalizedBefore = before;
            auto normalizedAfter = after;
            minidb::clearPersistentPageLsn(normalizedBefore);
            minidb::clearPersistentPageLsn(normalizedAfter);
            const auto ranges = minidb::computePageDelta(normalizedBefore, normalizedAfter);
            require(!ranges.empty(), "Model generated a no-op update");

            const auto transactionId = static_cast<minidb::TransactionId>(operation + 1);
            const auto beginLsn = log.append(minidb::LogRecord{
                minidb::LogRecordType::Begin,
                transactionId,
                minidb::INVALID_LSN,
                minidb::encodeBeginLogPayload({disk.pageCount()}),
                minidb::INVALID_LSN,
            });
            const auto updateLsn = log.append(minidb::LogRecord{
                minidb::LogRecordType::PageDeltaUpdateV2,
                transactionId,
                beginLsn,
                minidb::encodePageDeltaUpdateV2LogPayload({
                    pageIds[pageIndex], true,
                    minidb::readPersistentPageLsn(before), ranges,
                }),
                minidb::INVALID_LSN,
            });
            minidb::writePersistentPageLsn(after, updateLsn);
            static_cast<void>(log.append(minidb::LogRecord{
                minidb::LogRecordType::Commit,
                transactionId,
                updateLsn,
                {},
                minidb::INVALID_LSN,
            }));
            logical[pageIndex] = after;

            const bool persist = forcedTail
                ? (pageIndex % 2U == 0U)
                : ((random() % 3U) == 0U);
            if (persist) {
                persisted[pageIndex] = after;
                disk.writePhysicalPage(pageIds[pageIndex], after);
            }
        }
        log.flushAll();
        disk.sync();
    }

    minidb::RecoveryStats selective;
    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        selective = minidb::RecoveryManager(
            disk, log, nullptr, false,
            minidb::RedoPolicy::PageLsnSelectiveRedo).recover();
        for (std::size_t index = 0; index < PAGE_COUNT; ++index) {
            minidb::DiskManager::Page recovered{};
            disk.readPhysicalPage(pageIds[index], recovered);
            require(recovered == logical[index],
                    "Selective PageLSN model diverged from logical state; seed="
                        + std::to_string(seed));
        }
    }
    require(selective.pageLsnChecks == OPERATIONS
                && selective.redoSkippedByPageLsn > 0
                && selective.redoAppliedAfterPageLsnCheck > 0,
            "Model did not exercise both selective skip and apply paths; seed="
                + std::to_string(seed));

    {
        minidb::DiskManager disk(database.path().string());
        minidb::LogManager log(
            minidb::walPathForDatabase(database.path().string()),
            minidb::LogManager::DEFAULT_BUFFER_SIZE,
            minidb::LogOpenMode::DeferredRecovery);
        const auto always = minidb::RecoveryManager(
            disk, log, nullptr, false, minidb::RedoPolicy::AlwaysRedo).recover();
        require(always.pagesRedone == OPERATIONS,
                "AlwaysRedo model did not replay every winner update");
        for (std::size_t index = 0; index < PAGE_COUNT; ++index) {
            minidb::DiskManager::Page recovered{};
            disk.readPhysicalPage(pageIds[index], recovered);
            require(recovered == logical[index],
                    "AlwaysRedo model diverged from selective state");
        }
    }
}

} // namespace

int main() {
    try {
        for (const auto seed : {0x11D30001ULL, 0x11D30002ULL, 0x11D30003ULL}) {
            runModel(seed);
        }
        std::cout << "PageLSN selective-REDO model passed 12,000 operations\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PageLSN model test failure: " << error.what() << '\n';
        return 1;
    }
}
