#include "minidb/record_page.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint16LittleEndian(
    minidb::Pager::Page& page,
    std::size_t offset,
    std::uint16_t value) {
    for (std::size_t index = 0; index < 2; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint32LittleEndian(
    minidb::Pager::Page& page,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

minidb::PageId allocateInitializedPage(minidb::Pager& pager) {
    const auto pageId = pager.allocatePage();
    minidb::RecordPage::initialize(pager, pageId);
    return pageId;
}

void testRecordPageOperationsAndCapacity() {
    static_assert(minidb::record_page_layout::SLOT_CAPACITY == 13);
    static_assert(minidb::record_page_layout::OCCUPANCY_SIZE == 2);
    static_assert(minidb::record_page_layout::SLOT_DATA_OFFSET == 34);
    static_assert(minidb::record_page_layout::UNUSED_SIZE == 240);

    minidb::test::TemporaryDatabase database("record_page_operations");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    minidb::RecordPage page(pager, pageId);

    minidb::test::require(page.liveCount() == 0, "New record page was not empty");
    minidb::test::require(page.hasFreeSlot(), "New record page had no free slot");
    minidb::test::require(
        page.nextPageId() == minidb::INVALID_PAGE_ID,
        "New record page had an unexpected next page");

    const auto original = minidb::test::makeRow(1);
    const auto firstSlot = page.insert(minidb::serializeRow(original));
    minidb::test::require(firstSlot == 0, "First insert did not use slot zero");
    minidb::test::require(firstSlot != minidb::INVALID_SLOT_ID, "Insert returned invalid slot");
    minidb::test::require(page.liveCount() == 1, "Insert did not update live count");
    minidb::test::require(
        minidb::deserializeRow(page.get(firstSlot)) == original,
        "Inserted row did not round trip through RecordPage");

    const auto replacement = minidb::test::makeRow(2);
    page.update(firstSlot, minidb::serializeRow(replacement));
    minidb::test::require(
        minidb::deserializeRow(page.get(firstSlot)) == replacement,
        "RecordPage update did not replace the row in place");

    page.erase(firstSlot);
    minidb::test::require(page.liveCount() == 0, "Erase did not decrement live count");
    minidb::test::require(!page.isOccupied(firstSlot), "Erased slot remained occupied");
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(page.get(firstSlot)); },
        "Erased slot remained readable");

    const auto reusedSlot = page.insert(minidb::serializeRow(original));
    minidb::test::require(reusedSlot == firstSlot, "Deleted slot was not reused");

    for (std::size_t index = 1; index < minidb::record_page_layout::SLOT_CAPACITY; ++index) {
        const auto slot = page.insert(minidb::serializeRow(
            minidb::test::makeRow(static_cast<std::uint32_t>(index + 10))));
        minidb::test::require(slot == index, "Page did not fill slots in increasing order");
    }

    minidb::test::require(
        page.liveCount() == minidb::record_page_layout::SLOT_CAPACITY,
        "Filled page did not reach its derived capacity");
    minidb::test::require(!page.hasFreeSlot(), "Filled page still reported free capacity");
    minidb::test::requireThrows<std::overflow_error>(
        [&] { static_cast<void>(page.insert(minidb::serializeRow(minidb::test::makeRow(999)))); },
        "RecordPage accepted capacity plus one rows");
}

void testInvalidSlotIdsAreRejected() {
    minidb::test::TemporaryDatabase database("record_page_invalid_slot");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    minidb::RecordPage page(pager, pageId);
    const auto invalidSlot =
        static_cast<minidb::SlotId>(minidb::record_page_layout::SLOT_CAPACITY);

    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(page.isOccupied(invalidSlot)); },
        "RecordPage accepted an invalid slot ID");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { static_cast<void>(page.get(invalidSlot)); },
        "RecordPage read an invalid slot ID");
}

void testCorruptMagicIsRejected() {
    minidb::test::TemporaryDatabase database("record_page_bad_magic");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    auto& bytes = pager.getPage(pageId);
    bytes[minidb::record_page_layout::MAGIC_OFFSET] = std::byte{'X'};
    pager.markDirty(pageId);

    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordPage corrupted(pager, pageId); },
        "RecordPage accepted corrupt magic");
}

void testUnsupportedVersionIsRejected() {
    minidb::test::TemporaryDatabase database("record_page_bad_version");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    auto& bytes = pager.getPage(pageId);
    writeUint32LittleEndian(
        bytes,
        minidb::record_page_layout::LAYOUT_VERSION_OFFSET,
        minidb::record_page_layout::CURRENT_VERSION + 1);
    pager.markDirty(pageId);

    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordPage corrupted(pager, pageId); },
        "RecordPage accepted an unsupported layout version");
}

void testInvalidLiveCountIsRejected() {
    minidb::test::TemporaryDatabase database("record_page_bad_count");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    auto& bytes = pager.getPage(pageId);
    writeUint16LittleEndian(
        bytes,
        minidb::record_page_layout::LIVE_COUNT_OFFSET,
        static_cast<std::uint16_t>(minidb::record_page_layout::SLOT_CAPACITY + 1));
    pager.markDirty(pageId);

    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordPage corrupted(pager, pageId); },
        "RecordPage accepted a live count beyond capacity");
}

void testLiveCountOccupancyMismatchIsRejected() {
    minidb::test::TemporaryDatabase database("record_page_bad_occupancy");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    auto& bytes = pager.getPage(pageId);
    writeUint16LittleEndian(bytes, minidb::record_page_layout::LIVE_COUNT_OFFSET, 1);
    pager.markDirty(pageId);

    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordPage corrupted(pager, pageId); },
        "RecordPage accepted inconsistent occupancy metadata");
}

void testOutOfCapacityOccupancyBitIsRejected() {
    minidb::test::TemporaryDatabase database("record_page_bad_bitmap_bit");
    minidb::Pager pager(database.path().string());
    const auto pageId = allocateInitializedPage(pager);
    auto& bytes = pager.getPage(pageId);
    const auto unusedBit = minidb::record_page_layout::SLOT_CAPACITY;
    const auto byteIndex = unusedBit / BITS_PER_BYTE;
    const auto bitIndex = unusedBit % BITS_PER_BYTE;
    bytes[minidb::record_page_layout::OCCUPANCY_OFFSET + byteIndex] =
        static_cast<std::byte>(1U << bitIndex);
    pager.markDirty(pageId);

    minidb::test::requireThrows<std::runtime_error>(
        [&] { minidb::RecordPage corrupted(pager, pageId); },
        "RecordPage accepted an occupied bit beyond capacity");
}

} // namespace

int main() {
    try {
        testRecordPageOperationsAndCapacity();
        testInvalidSlotIdsAreRejected();
        testCorruptMagicIsRejected();
        testUnsupportedVersionIsRejected();
        testInvalidLiveCountIsRejected();
        testLiveCountOccupancyMismatchIsRejected();
        testOutOfCapacityOccupancyBitIsRejected();
        std::cout << "record_page_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "record_page_test failed: " << error.what() << '\n';
        return 1;
    }
}
