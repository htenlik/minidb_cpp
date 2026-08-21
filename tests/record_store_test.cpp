#include "minidb/record_store.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void testFirstInsertAndInvalidRecordIds() {
    minidb::test::TemporaryDatabase database("record_store_first_insert");
    minidb::Pager pager(database.path().string());
    minidb::RecordStore store(pager);

    minidb::test::require(store.empty(), "New RecordStore was not empty");
    minidb::test::require(
        store.headPageId() == minidb::INVALID_PAGE_ID,
        "Empty RecordStore had a head page");

    const auto row = minidb::test::makeRow(1);
    const auto recordId = store.insert(row);
    minidb::test::require(recordId.isValid(), "Insert returned an invalid RID");
    minidb::test::require(recordId == minidb::RecordId{1, 0}, "First RID was not (1, 0)");
    minidb::test::require(store.headPageId() == 1, "First insert did not establish page 1 as head");
    minidb::test::require(pager.pageCount() == 2, "First insert did not allocate one data page");
    minidb::test::require(store.get(recordId) == row, "RID did not resolve its inserted row");

    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(store.get(minidb::INVALID_RECORD_ID)); },
        "RecordStore accepted INVALID_RECORD_ID");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(store.get(minidb::RecordId{0, 0})); },
        "RecordStore accepted metadata page as a RID page");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(store.get(minidb::RecordId{pager.pageCount(), 0})); },
        "RecordStore accepted a nonexistent page ID");
    minidb::test::requireThrows<std::out_of_range>(
        [&] {
            static_cast<void>(store.get(minidb::RecordId{
                recordId.pageId,
                static_cast<minidb::SlotId>(minidb::record_page_layout::SLOT_CAPACITY)}));
        },
        "RecordStore accepted an invalid slot ID");
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(store.get(minidb::RecordId{recordId.pageId, 1})); },
        "RecordStore returned an unoccupied slot");

    const auto unrelatedPageId = pager.allocatePage();
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(store.get(minidb::RecordId{unrelatedPageId, 0})); },
        "RecordStore accepted a page outside its heap chain");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { minidb::RecordStore invalidHead(pager, 0); },
        "RecordStore accepted page 0 as a heap head");
}

void testMultiplePagesDeleteReuseScanAndPersistence() {
    minidb::test::TemporaryDatabase database("record_store_persistence");
    constexpr std::size_t EXTRA_ROWS = 3;
    const auto rowCount = minidb::record_page_layout::SLOT_CAPACITY + EXTRA_ROWS;

    std::vector<minidb::Row> rows;
    std::vector<minidb::RecordId> recordIds;
    minidb::PageId headPageId = minidb::INVALID_PAGE_ID;
    minidb::Row replacement = minidb::test::makeRow(900);
    minidb::Row updated = minidb::test::makeRow(901);
    minidb::RecordId reusedRecordId{};
    const auto deletedIndex = std::size_t{5};
    const auto updatedIndex = minidb::record_page_layout::SLOT_CAPACITY + 1;

    {
        minidb::Pager pager(database.path().string());
        minidb::RecordStore store(pager);

        for (std::size_t index = 0; index < rowCount; ++index) {
            rows.push_back(minidb::test::makeRow(static_cast<std::uint32_t>(index + 10)));
            recordIds.push_back(store.insert(rows.back()));
        }

        headPageId = store.headPageId();
        minidb::test::require(headPageId == 1, "Record heap did not start on page 1");
        minidb::test::require(pager.pageCount() == 3, "Capacity plus rows did not allocate two pages");

        for (std::size_t index = 0; index < rowCount; ++index) {
            const auto expectedPage = index < minidb::record_page_layout::SLOT_CAPACITY ? 1U : 2U;
            const auto expectedSlot = static_cast<minidb::SlotId>(
                index % minidb::record_page_layout::SLOT_CAPACITY);
            minidb::test::require(
                recordIds[index] == minidb::RecordId{expectedPage, expectedSlot},
                "Insert returned an unexpected multi-page RID");
            minidb::test::require(store.get(recordIds[index]) == rows[index],
                                  "Multi-page RID returned the wrong row");
        }

        minidb::RecordPage firstPage(pager, 1);
        minidb::RecordPage secondPage(pager, 2);
        minidb::test::require(firstPage.nextPageId() == 2, "First record page was not linked to second");
        minidb::test::require(
            secondPage.nextPageId() == minidb::INVALID_PAGE_ID,
            "Second record page did not terminate the chain");

        const auto initialScan = store.scan();
        minidb::test::require(initialScan.size() == rowCount, "Initial scan lost or duplicated rows");
        for (std::size_t index = 0; index < initialScan.size(); ++index) {
            minidb::test::require(initialScan[index].first == recordIds[index],
                                  "Initial scan RID order was not deterministic");
            minidb::test::require(initialScan[index].second == rows[index],
                                  "Initial scan returned the wrong row");
        }

        store.erase(recordIds[deletedIndex]);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(store.get(recordIds[deletedIndex])); },
            "Deleted RID remained readable");
        const auto afterDelete = store.scan();
        minidb::test::require(afterDelete.size() == rowCount - 1, "Delete did not remove row from scan");
        for (const auto& entry : afterDelete) {
            minidb::test::require(entry.first != recordIds[deletedIndex],
                                  "Deleted RID remained in scan");
        }

        reusedRecordId = store.insert(replacement);
        minidb::test::require(
            reusedRecordId == recordIds[deletedIndex],
            "Later insert did not reuse the first deleted slot");

        store.update(recordIds[updatedIndex], updated);
        minidb::test::require(store.get(recordIds[updatedIndex]) == updated,
                              "Update did not replace row in place");

        const auto beforeInvalidUpdate = store.get(recordIds[0]);
        minidb::Row oversized = beforeInvalidUpdate;
        oversized.username = std::string(minidb::row_layout::USERNAME_MAX_SIZE + 1, 'x');
        minidb::test::requireThrows<std::invalid_argument>(
            [&] { store.update(recordIds[0], oversized); },
            "RecordStore accepted an oversized update");
        minidb::test::require(store.get(recordIds[0]) == beforeInvalidUpdate,
                              "Invalid update corrupted the existing row");

        const auto finalScan = store.scan();
        minidb::test::require(finalScan.size() == rowCount, "Final scan lost or duplicated rows");
        for (std::size_t index = 0; index < finalScan.size(); ++index) {
            minidb::RecordId expectedRecordId = recordIds[index];
            minidb::Row expectedRow = rows[index];
            if (index == deletedIndex) {
                expectedRecordId = reusedRecordId;
                expectedRow = replacement;
            } else if (index == updatedIndex) {
                expectedRow = updated;
            }
            minidb::test::require(finalScan[index].first == expectedRecordId,
                                  "Final scan RID order was not page/slot order");
            minidb::test::require(finalScan[index].second == expectedRow,
                                  "Final scan returned unexpected row data");
        }

        pager.flushAll();
    }

    {
        minidb::Pager pager(database.path().string());
        minidb::RecordStore reopened(pager, headPageId);

        minidb::test::require(reopened.headPageId() == headPageId, "Reopened heap changed its head RID");
        minidb::test::require(reopened.get(recordIds[0]) == rows[0],
                              "Pre-close RID did not resolve after reopen");
        minidb::test::require(reopened.get(reusedRecordId) == replacement,
                              "Reused slot did not persist after reopen");
        minidb::test::require(reopened.get(recordIds[updatedIndex]) == updated,
                              "Updated row did not persist after reopen");

        const auto scan = reopened.scan();
        minidb::test::require(scan.size() == rowCount, "Reopened scan returned wrong row count");
        minidb::test::require(scan.front().first == minidb::RecordId{1, 0},
                              "Reopened scan did not start at first page/slot");
        minidb::test::require(scan.back().first == minidb::RecordId{2, 2},
                              "Reopened scan did not end at last page/slot");

        minidb::RecordPage firstPage(pager, 1);
        minidb::test::require(firstPage.nextPageId() == 2,
                              "Record-page chain did not persist after reopen");
    }
}

void testRecordPageChainCycleIsRejected() {
    minidb::test::TemporaryDatabase database("record_store_cycle");
    minidb::Pager pager(database.path().string());
    minidb::RecordStore store(pager);

    for (std::size_t index = 0; index <= minidb::record_page_layout::SLOT_CAPACITY; ++index) {
        static_cast<void>(store.insert(minidb::test::makeRow(static_cast<std::uint32_t>(index))));
    }

    minidb::RecordPage secondPage(pager, 2);
    secondPage.setNextPageId(store.headPageId());
    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordStore corrupted(pager, store.headPageId()); },
        "RecordStore accepted a cyclic record-page chain");
}

} // namespace

int main() {
    try {
        testFirstInsertAndInvalidRecordIds();
        testMultiplePagesDeleteReuseScanAndPersistence();
        testRecordPageChainCycleIsRejected();
        std::cout << "record_store_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "record_store_test failed: " << error.what() << '\n';
        return 1;
    }
}
