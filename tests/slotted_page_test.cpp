#include "minidb/slotted_page.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

minidb::TupleBytes makeTuple(std::size_t size, std::uint8_t seed) {
    minidb::TupleBytes tuple(size);
    for (std::size_t index = 0; index < size; ++index) {
        tuple[index] = static_cast<std::byte>((seed + index * 17U) & 0xFFU);
    }
    return tuple;
}

std::uint16_t readUint16(const minidb::DiskManager::Page& page, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(page[offset]))
        | static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(page[offset + 1]) << 8U);
}

std::uint32_t readUint32(const minidb::DiskManager::Page& page, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

void writeUint16(minidb::DiskManager::Page& page, std::size_t offset, std::uint16_t value) {
    for (std::size_t index = 0; index < 2; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void writeUint32(minidb::DiskManager::Page& page, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

struct PageFixture {
    explicit PageFixture(std::string_view) {
        minidb::SlottedPageView::initialize(
            bytes, pageId, pageCount, heapMetadataPageId);
    }

    minidb::DiskManager::Page bytes{};
    minidb::PageId heapMetadataPageId = 1;
    minidb::PageId pageId = 2;
    minidb::PageId pageCount = 3;
};

void testPhysicalLayoutAndEmptyPage() {
    static_assert(minidb::slotted_page_layout::HEADER_SIZE == 48);
    static_assert(minidb::slotted_page_layout::SLOT_SIZE == 8);
    static_assert(minidb::slotted_page_layout::MAX_SLOT_COUNT == 506);
    static_assert(minidb::slotted_page_layout::MAX_TUPLE_SIZE == 4040);

    PageFixture fixture("slotted_layout");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto& bytes = fixture.bytes;
    minidb::test::require(
        std::equal(
            minidb::slotted_page_layout::MAGIC.begin(),
            minidb::slotted_page_layout::MAGIC.end(),
            bytes.begin()),
        "Slotted-page magic was not encoded");
    minidb::test::require(
        readUint32(bytes, minidb::slotted_page_layout::LAYOUT_VERSION_OFFSET) == 1,
        "Slotted-page version was not encoded little-endian");
    minidb::test::require(
        readUint32(bytes, minidb::slotted_page_layout::HEADER_SIZE_OFFSET) == 48,
        "Slotted-page header size was not encoded");
    minidb::test::require(page.slotCount() == 0 && page.liveCount() == 0,
                          "New slotted page was not empty");
    minidb::test::require(
        page.lowerBoundary() == minidb::slotted_page_layout::HEADER_SIZE
            && page.upperBoundary() == minidb::database_format::PAGE_SIZE,
        "New slotted page had incorrect free-space boundaries");
    minidb::test::require(
        page.freeSpace() == minidb::database_format::PAGE_SIZE - minidb::slotted_page_layout::HEADER_SIZE,
        "New slotted page reported incorrect free space");
    minidb::test::require(page.heapMetadataPageId() == fixture.heapMetadataPageId,
                          "Slotted page did not persist its owning heap metadata ID");
    minidb::test::require(page.nextPageId() == minidb::INVALID_PAGE_ID
                              && page.previousPageId() == minidb::INVALID_PAGE_ID,
                          "New slotted page had unexpected links");
    page.validate();
}

void testInsertGetSlotDirectoryAndStableIds() {
    PageFixture fixture("slotted_insert");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto first = makeTuple(3, 1);
    const auto second = makeTuple(71, 2);
    const auto firstSlot = page.insert(first);
    const auto secondSlot = page.insert(second);

    minidb::test::require(firstSlot == 0 && secondSlot == 1,
                          "New tuples did not receive increasing SlotIds");
    minidb::test::require(page.get(firstSlot) == first && page.get(secondSlot) == second,
                          "Inserted tuple bytes did not round trip");
    const auto& bytes = fixture.bytes;
    const auto firstDirectoryOffset = minidb::slotted_page_layout::slotOffset(firstSlot);
    const auto secondDirectoryOffset = minidb::slotted_page_layout::slotOffset(secondSlot);
    minidb::test::require(firstDirectoryOffset == 4088 && secondDirectoryOffset == 4080,
                          "Slot directory did not grow backward from page end");
    minidb::test::require(
        readUint16(
            bytes,
            firstDirectoryOffset + minidb::slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET)
            == minidb::slotted_page_layout::HEADER_SIZE,
        "Slot zero stored an incorrect tuple offset");
    minidb::test::require(
        readUint16(
            bytes,
            secondDirectoryOffset + minidb::slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET)
            == second.size(),
        "Slot one stored an incorrect tuple length");
    minidb::test::require(
        readUint16(bytes, secondDirectoryOffset + minidb::slotted_page_layout::SLOT_FLAGS_OFFSET)
            == minidb::slotted_page_layout::SLOT_LIVE,
        "Slot live state was not encoded explicitly");
    page.validate();
}

void testDeleteCompactionAndSlotReuse() {
    PageFixture fixture("slotted_compaction");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto first = makeTuple(100, 1);
    const auto middle = makeTuple(57, 2);
    const auto last = makeTuple(83, 3);
    const auto firstSlot = page.insert(first);
    const auto middleSlot = page.insert(middle);
    const auto lastSlot = page.insert(last);
    const auto slotCountBefore = page.slotCount();
    const auto upperBefore = page.upperBoundary();
    const auto lastOffsetBefore = readUint16(
        fixture.bytes,
        minidb::slotted_page_layout::slotOffset(lastSlot));

    page.erase(middleSlot);
    minidb::test::require(!page.isOccupied(middleSlot), "Deleted slot remained occupied");
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(page.get(middleSlot)); },
        "Deleted tuple remained readable");
    minidb::test::require(page.get(firstSlot) == first && page.get(lastSlot) == last,
                          "Compaction changed surviving tuple bytes");
    const auto lastOffsetAfter = readUint16(
        fixture.bytes,
        minidb::slotted_page_layout::slotOffset(lastSlot));
    minidb::test::require(lastOffsetAfter < lastOffsetBefore,
                          "Compaction did not move payload across the deleted gap");
    minidb::test::require(lastSlot == 2, "Compaction renumbered a surviving SlotId");

    const auto replacement = makeTuple(29, 9);
    const auto freeBefore = page.freeSpace();
    const auto reused = page.insert(replacement);
    minidb::test::require(reused == middleSlot, "Insertion did not reuse the first free SlotId");
    minidb::test::require(page.slotCount() == slotCountBefore
                              && page.upperBoundary() == upperBefore,
                          "Reused slot incorrectly grew the directory");
    minidb::test::require(page.freeSpace() == freeBefore - replacement.size(),
                          "Reused slot consumed unexpected directory space");
    page.validate();
}

void testUpdateCasesAndFailureAtomicity() {
    PageFixture fixture("slotted_update");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto slot = page.insert(makeTuple(100, 1));
    const auto same = makeTuple(100, 2);
    minidb::test::require(page.tryUpdate(slot, same) && page.get(slot) == same,
                          "Same-size update failed");
    const auto smaller = makeTuple(17, 3);
    minidb::test::require(page.tryUpdate(slot, smaller) && page.get(slot) == smaller,
                          "Smaller update failed");
    const auto larger = makeTuple(600, 4);
    minidb::test::require(page.tryUpdate(slot, larger) && page.get(slot) == larger,
                          "Larger in-page update failed");

    const auto secondSlot = page.insert(makeTuple(3400, 5));
    const auto original = page.get(slot);
    const auto tooLargeForRemainingSpace = makeTuple(700, 6);
    minidb::test::require(!page.tryUpdate(slot, tooLargeForRemainingSpace),
                          "Oversized in-page update unexpectedly succeeded");
    minidb::test::require(page.get(slot) == original,
                          "Failed update partially mutated the original tuple");
    minidb::test::require(page.get(secondSlot) == makeTuple(3400, 5),
                          "Failed update corrupted a neighboring tuple");
    page.validate();
}

void testCapacityBoundariesAndTupleValidation() {
    PageFixture fixture("slotted_capacity");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto largest = makeTuple(minidb::slotted_page_layout::MAX_TUPLE_SIZE, 7);
    const auto slot = page.insert(largest);
    minidb::test::require(page.get(slot) == largest, "Largest inline tuple did not round trip");
    minidb::test::require(page.freeSpace() == 0 && !page.canFit(1),
                          "Largest tuple did not exactly fill payload plus directory capacity");
    minidb::test::requireThrows<std::overflow_error>(
        [&] { static_cast<void>(page.insert(makeTuple(1, 1))); },
        "Full slotted page accepted capacity plus one");

    PageFixture invalidFixture("slotted_invalid_tuple");
    minidb::SlottedPageView invalidPage(invalidFixture.bytes, invalidFixture.pageId, invalidFixture.pageCount);
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(invalidPage.insert({})); },
        "Slotted page accepted a zero-length tuple");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] {
            static_cast<void>(invalidPage.insert(
                makeTuple(minidb::slotted_page_layout::MAX_TUPLE_SIZE + 1, 2)));
        },
        "Slotted page accepted a tuple larger than its inline maximum");
}

void testDirectedFragmentationStress() {
    PageFixture fixture("slotted_fragmentation");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    std::vector<std::optional<minidb::TupleBytes>> expected;
    expected.reserve(30);
    for (std::size_t index = 0; index < 30; ++index) {
        auto tuple = makeTuple(20 + (index * 13U) % 61U, static_cast<std::uint8_t>(index));
        const auto slot = page.insert(tuple);
        minidb::test::require(slot == index, "Fragmentation fixture SlotId was unexpected");
        expected.emplace_back(std::move(tuple));
    }
    for (std::size_t index = 1; index < expected.size(); index += 2) {
        page.erase(static_cast<minidb::SlotId>(index));
        expected[index].reset();
        page.validate();
    }
    for (std::size_t index = 0; index < expected.size(); index += 4) {
        auto replacement = makeTuple(
            expected[index]->size() + 35,
            static_cast<std::uint8_t>(100 + index));
        minidb::test::require(
            page.tryUpdate(static_cast<minidb::SlotId>(index), replacement),
            "Fragmentation stress larger update unexpectedly failed");
        expected[index] = std::move(replacement);
    }
    for (std::size_t index = 1; index < expected.size(); index += 2) {
        auto replacement = makeTuple(25 + index, static_cast<std::uint8_t>(200 + index));
        const auto slot = page.insert(replacement);
        minidb::test::require(slot == index, "Fragmentation stress did not reuse deleted SlotId");
        expected[index] = std::move(replacement);
    }
    for (std::size_t index = 2; index < expected.size(); index += 5) {
        page.erase(static_cast<minidb::SlotId>(index));
        expected[index].reset();
    }
    page.compact();
    page.validate();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index]) {
            minidb::test::require(
                page.get(static_cast<minidb::SlotId>(index)) == *expected[index],
                "Fragmentation stress changed a surviving RID or payload");
        } else {
            minidb::test::require(
                !page.isOccupied(static_cast<minidb::SlotId>(index)),
                "Fragmentation stress resurrected a deleted slot");
        }
    }
}

template <typename Mutator>
void requireCorruptionRejected(std::string_view name, Mutator&& mutate, bool insertTwo = false) {
    PageFixture fixture(name);
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    static_cast<void>(page.insert(makeTuple(20, 1)));
    if (insertTwo) {
        static_cast<void>(page.insert(makeTuple(20, 2)));
    }
    auto& bytes = fixture.bytes;
    mutate(fixture, bytes);
    minidb::test::requireThrows<std::runtime_error>(
        [&] {
            minidb::ConstSlottedPageView corrupted(
                fixture.bytes, fixture.pageId, fixture.pageCount);
        },
        "Slotted page accepted corrupted persistent state");
}

void testCorruptionValidation() {
    requireCorruptionRejected("slotted_bad_magic", [](auto&, auto& bytes) {
        bytes[minidb::slotted_page_layout::MAGIC_OFFSET] = std::byte{'X'};
    });
    requireCorruptionRejected("slotted_bad_version", [](auto&, auto& bytes) {
        writeUint32(
            bytes,
            minidb::slotted_page_layout::LAYOUT_VERSION_OFFSET,
            minidb::slotted_page_layout::CURRENT_VERSION + 1);
    });
    requireCorruptionRejected("slotted_bad_header", [](auto&, auto& bytes) {
        writeUint32(
            bytes,
            minidb::slotted_page_layout::HEADER_SIZE_OFFSET,
            minidb::slotted_page_layout::HEADER_SIZE + 1);
    });
    requireCorruptionRejected("slotted_bad_slot_count", [](auto&, auto& bytes) {
        writeUint16(
            bytes,
            minidb::slotted_page_layout::SLOT_COUNT_OFFSET,
            minidb::slotted_page_layout::MAX_SLOT_COUNT + 1);
    });
    requireCorruptionRejected("slotted_bad_live_count", [](auto&, auto& bytes) {
        writeUint16(bytes, minidb::slotted_page_layout::LIVE_COUNT_OFFSET, 0);
    });
    requireCorruptionRejected("slotted_bad_boundaries", [](auto&, auto& bytes) {
        writeUint16(bytes, minidb::slotted_page_layout::LOWER_BOUNDARY_OFFSET, 4000);
        writeUint16(bytes, minidb::slotted_page_layout::UPPER_BOUNDARY_OFFSET, 3000);
    });
    requireCorruptionRejected("slotted_tuple_outside", [](auto&, auto& bytes) {
        writeUint16(
            bytes,
            minidb::slotted_page_layout::slotOffset(0)
                + minidb::slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET,
            1);
    });
    requireCorruptionRejected("slotted_tuple_directory_overlap", [](auto&, auto& bytes) {
        writeUint16(
            bytes,
            minidb::slotted_page_layout::slotOffset(0)
                + minidb::slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET,
            4080);
    });
    requireCorruptionRejected("slotted_payload_overlap", [](auto&, auto& bytes) {
        const auto firstOffset = readUint16(
            bytes,
            minidb::slotted_page_layout::slotOffset(0)
                + minidb::slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET);
        writeUint16(
            bytes,
            minidb::slotted_page_layout::slotOffset(1)
                + minidb::slotted_page_layout::SLOT_TUPLE_OFFSET_OFFSET,
            firstOffset);
    }, true);
    requireCorruptionRejected("slotted_dangling_next", [](auto& fixture, auto& bytes) {
        writeUint32(
            bytes,
            minidb::slotted_page_layout::NEXT_PAGE_ID_OFFSET,
            fixture.pageCount + 10);
    });

    PageFixture fixture("slotted_malformed_free_slot");
    minidb::SlottedPageView page(fixture.bytes, fixture.pageId, fixture.pageCount);
    const auto slot = page.insert(makeTuple(20, 1));
    page.erase(slot);
    auto& bytes = fixture.bytes;
    writeUint16(
        bytes,
        minidb::slotted_page_layout::slotOffset(slot)
            + minidb::slotted_page_layout::SLOT_TUPLE_LENGTH_OFFSET,
        1);
    minidb::test::requireThrows<std::runtime_error>(
        [&] {
            minidb::ConstSlottedPageView corrupted(
                fixture.bytes, fixture.pageId, fixture.pageCount);
        },
        "Slotted page accepted a malformed free slot");
}

} // namespace

int main() {
    try {
        testPhysicalLayoutAndEmptyPage();
        testInsertGetSlotDirectoryAndStableIds();
        testDeleteCompactionAndSlotReuse();
        testUpdateCasesAndFailureAtomicity();
        testCapacityBoundariesAndTupleValidation();
        testDirectedFragmentationStress();
        testCorruptionValidation();
        std::cout << "slotted_page_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "slotted_page_test failed: " << error.what() << '\n';
        return 1;
    }
}
