#include "minidb/byte_codec.hpp"
#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/page_lsn.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/slotted_page.hpp"
#include "minidb/tuple_store.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

namespace {

using minidb::test::require;

struct Case {
    minidb::DiskManager::Page page;
    std::size_t offset;
    minidb::PersistentPageType type;
};

template <std::size_t Size>
void setMagic(minidb::DiskManager::Page& page, const std::array<std::byte, Size>& magic) {
    std::copy(magic.begin(), magic.end(), page.begin());
}

std::vector<Case> supportedPages() {
    std::vector<Case> pages;
    {
        minidb::DiskManager::Page page{};
        minidb::database_format::serializeDatabaseHeader(
            minidb::database_format::makeCurrentDatabaseHeader(), page);
        pages.push_back({page, minidb::database_format::PAGE_LSN_OFFSET,
                         minidb::PersistentPageType::DatabaseMetadata});
    }
    const auto add = [&](const auto& magic, std::size_t versionOffset,
                         std::uint32_t version, std::size_t headerOffset,
                         std::uint32_t headerSize, std::size_t lsnOffset,
                         minidb::PersistentPageType type) {
        minidb::DiskManager::Page page{};
        setMagic(page, magic);
        minidb::byte_codec::writeUint32(page, versionOffset, version);
        minidb::byte_codec::writeUint32(page, headerOffset, headerSize);
        pages.push_back({page, lsnOffset, type});
    };
    add(minidb::free_page_layout::MAGIC,
        minidb::free_page_layout::LAYOUT_VERSION_OFFSET,
        minidb::free_page_layout::CURRENT_VERSION,
        minidb::free_page_layout::HEADER_SIZE_OFFSET,
        minidb::free_page_layout::HEADER_SIZE,
        minidb::free_page_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::Free);
    add(minidb::slotted_page_layout::MAGIC,
        minidb::slotted_page_layout::LAYOUT_VERSION_OFFSET,
        minidb::slotted_page_layout::CURRENT_VERSION,
        minidb::slotted_page_layout::HEADER_SIZE_OFFSET,
        minidb::slotted_page_layout::HEADER_SIZE,
        minidb::slotted_page_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::Slotted);
    add(minidb::tuple_heap_metadata_layout::MAGIC,
        minidb::tuple_heap_metadata_layout::LAYOUT_VERSION_OFFSET,
        minidb::tuple_heap_metadata_layout::CURRENT_VERSION,
        minidb::tuple_heap_metadata_layout::HEADER_SIZE_OFFSET,
        minidb::tuple_heap_metadata_layout::HEADER_SIZE,
        minidb::tuple_heap_metadata_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::TupleHeapMetadata);
    add(minidb::persistent_index_metadata_layout::MAGIC,
        minidb::persistent_index_metadata_layout::LAYOUT_VERSION_OFFSET,
        minidb::persistent_index_metadata_layout::CURRENT_VERSION,
        minidb::persistent_index_metadata_layout::HEADER_SIZE_OFFSET,
        minidb::persistent_index_metadata_layout::HEADER_SIZE,
        minidb::persistent_index_metadata_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::IndexMetadata);
    add(minidb::persistent_bplus_leaf_layout::MAGIC,
        minidb::persistent_bplus_leaf_layout::LAYOUT_VERSION_OFFSET,
        minidb::persistent_bplus_leaf_layout::CURRENT_VERSION,
        minidb::persistent_bplus_leaf_layout::HEADER_SIZE_OFFSET,
        minidb::persistent_bplus_leaf_layout::HEADER_SIZE,
        minidb::persistent_bplus_leaf_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::BPlusLeaf);
    add(minidb::persistent_bplus_internal_layout::MAGIC,
        minidb::persistent_bplus_internal_layout::LAYOUT_VERSION_OFFSET,
        minidb::persistent_bplus_internal_layout::CURRENT_VERSION,
        minidb::persistent_bplus_internal_layout::HEADER_SIZE_OFFSET,
        minidb::persistent_bplus_internal_layout::HEADER_SIZE,
        minidb::persistent_bplus_internal_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::BPlusInternal);
    add(minidb::catalog_metadata_layout::MAGIC,
        minidb::catalog_metadata_layout::VERSION_OFFSET,
        minidb::catalog_metadata_layout::CURRENT_VERSION,
        minidb::catalog_metadata_layout::HEADER_SIZE_OFFSET,
        minidb::catalog_metadata_layout::HEADER_SIZE,
        minidb::catalog_metadata_layout::PAGE_LSN_OFFSET,
        minidb::PersistentPageType::CatalogMetadata);
    return pages;
}

void testAllActivePageSlotsAndEncoding() {
    constexpr minidb::Lsn KNOWN = 0x0102030405060708ULL;
    for (auto& test : supportedPages()) {
        const auto slot = minidb::persistentPageLsnSlot(test.page);
        require(slot.has_value() && slot->offset == test.offset
                    && slot->pageType == test.type,
                "PageLSN accessor dispatched an active page incorrectly");
        require(minidb::readPersistentPageLsn(test.page) == minidb::INVALID_LSN,
                "Zero persistent PageLSN did not decode as INVALID_LSN");
        const auto before = test.page;
        minidb::writePersistentPageLsn(test.page, KNOWN);
        require(minidb::readPersistentPageLsn(test.page) == KNOWN,
                "Known persistent PageLSN did not round-trip");
        for (std::size_t byte = 0; byte < 8; ++byte) {
            require(test.page[test.offset + byte]
                        == static_cast<std::byte>((KNOWN >> (byte * 8U)) & 0xFFU),
                    "Persistent PageLSN is not exact little-endian bytes");
        }
        for (std::size_t byte = 0; byte < test.page.size(); ++byte) {
            if (byte < test.offset || byte >= test.offset + 8) {
                require(test.page[byte] == before[byte],
                        "PageLSN write corrupted an adjacent persisted field");
            }
        }
        minidb::clearPersistentPageLsn(test.page);
        require(minidb::readPersistentPageLsn(test.page) == minidb::INVALID_LSN,
                "Persistent PageLSN clear was not canonical");
    }
}

void testUnsupportedAndMalformedPages() {
    minidb::DiskManager::Page unknown{};
    require(!minidb::supportsPersistentPageLsn(unknown)
                && minidb::readPersistentPageLsn(unknown) == minidb::INVALID_LSN,
            "Unknown page magic was treated as PageLSN-aware");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { minidb::writePersistentPageLsn(unknown, 99); },
        "Unknown page accepted a persistent PageLSN write");

    auto malformed = supportedPages().front().page;
    minidb::byte_codec::writeUint32(
        malformed, minidb::database_format::HEADER_SIZE_OFFSET, 63);
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::persistentPageLsnSlot(malformed)); },
        "Malformed recognized page was not rejected");

    minidb::DiskManager::Page legacyLeaf{};
    setMagic(legacyLeaf, minidb::persistent_bplus_leaf_layout::MAGIC);
    minidb::byte_codec::writeUint32(
        legacyLeaf, minidb::persistent_bplus_leaf_layout::LAYOUT_VERSION_OFFSET,
        minidb::persistent_bplus_leaf_layout::LEGACY_VERSION);
    minidb::byte_codec::writeUint32(
        legacyLeaf, minidb::persistent_bplus_leaf_layout::HEADER_SIZE_OFFSET,
        minidb::persistent_bplus_leaf_layout::LEGACY_HEADER_SIZE);
    require(!minidb::supportsPersistentPageLsn(legacyLeaf),
            "Legacy B+ leaf incorrectly claimed an 8-byte PageLSN slot");
}

void testPersistentEncodingSentinels() {
    require(minidb::encodePersistentPageLsn(minidb::INVALID_LSN) == 0
                && minidb::decodePersistentPageLsn(0) == minidb::INVALID_LSN,
            "Persistent PageLSN invalid encoding changed");
    minidb::test::requireThrows<std::invalid_argument>(
        [] { static_cast<void>(minidb::encodePersistentPageLsn(0)); },
        "LSN zero was accepted as an ordinary persistent PageLSN");
    minidb::test::requireThrows<std::runtime_error>(
        [] { static_cast<void>(minidb::decodePersistentPageLsn(minidb::INVALID_LSN)); },
        "Persisted UINT64_MAX invalid sentinel was not rejected as malformed");
}

void testExactBytesSurviveDiskReopenForEveryPageType() {
    constexpr minidb::Lsn KNOWN = 0x8877665544332211ULL;
    minidb::test::TemporaryDatabase database("page_lsn_exact_disk_bytes");
    std::vector<std::pair<minidb::PageId, Case>> written;
    {
        minidb::DiskManager disk(database.path().string());
        auto pages = supportedPages();
        for (auto& test : pages) {
            minidb::writePersistentPageLsn(test.page, KNOWN);
            const auto pageId = test.type == minidb::PersistentPageType::DatabaseMetadata
                ? minidb::database_format::METADATA_PAGE_ID : disk.appendPage();
            disk.writePhysicalPage(pageId, test.page);
            written.emplace_back(pageId, test);
        }
        disk.sync();
    }
    {
        minidb::DiskManager disk(database.path().string());
        for (const auto& [pageId, test] : written) {
            minidb::DiskManager::Page page{};
            disk.readPhysicalPage(pageId, page);
            require(minidb::readPersistentPageLsn(page) == KNOWN,
                    "Persistent PageLSN changed across exact disk reopen");
            for (std::size_t byte = 0; byte < 8; ++byte) {
                require(page[test.offset + byte]
                            == static_cast<std::byte>((KNOWN >> (byte * 8U)) & 0xFFU),
                        "Reopened PageLSN bytes drifted from the documented offset");
            }
        }
    }
}

} // namespace

int main() {
    try {
        testAllActivePageSlotsAndEncoding();
        testUnsupportedAndMalformedPages();
        testPersistentEncodingSentinels();
        testExactBytesSurviveDiskReopenForEveryPageType();
        std::cout << "persistent PageLSN accessor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent PageLSN accessor test failure: " << error.what() << '\n';
        return 1;
    }
}
