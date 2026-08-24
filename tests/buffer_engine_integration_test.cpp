#include "minidb/byte_codec.hpp"
#include "minidb/catalog.hpp"
#include "minidb/page_access.hpp"
#include "minidb/pager.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/table_definition.hpp"
#include "minidb/tuple_codec.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using minidb::byte_codec::writeUint16;
using minidb::byte_codec::writeUint32;
using minidb::byte_codec::writeUint64;

void writeHeapMetadata(
    minidb::Pager& pager,
    minidb::PageId metadataPageId,
    minidb::PageId firstPageId,
    minidb::PageId lastPageId,
    std::uint64_t tupleCount) {
    auto& page = pager.getPage(metadataPageId);
    std::fill(page.begin(), page.end(), std::byte{0});
    using namespace minidb::tuple_heap_metadata_layout;
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint32(page, FIRST_PAGE_ID_OFFSET, firstPageId);
    writeUint32(page, LAST_PAGE_ID_OFFSET, lastPageId);
    writeUint64(page, TUPLE_COUNT_OFFSET, tupleCount);
    pager.markDirty(metadataPageId);
}

void buildLegacyDatabase(const std::filesystem::path& path) {
    minidb::Pager pager(path.string());
    const auto catalogMetadata = pager.allocatePage();
    const auto catalogHeapMetadata = pager.allocatePage();
    const auto catalogEntriesPage = pager.allocatePage();
    const auto tableHeapMetadata = pager.allocatePage();
    const auto tableDataPage = pager.allocatePage();
    const auto indexMetadata = pager.allocatePage();
    const auto indexLeaf = pager.allocatePage();
    const auto freePage = pager.allocatePage();

    const auto schema = minidb::Schema::create({
        {"id", minidb::DataType::UINT32, false, true, 0},
        {"name", minidb::DataType::VARCHAR, false, false, 64},
    });
    const minidb::TableDefinition definition{
        1, "legacy_users", schema, tableHeapMetadata, indexMetadata};
    const auto definitionBytes = minidb::encodeTableDefinition(definition);
    const auto tuple = minidb::TupleCodec::encode(
        schema, minidb::RowValues{std::uint32_t{7}, std::string{"legacy"}});

    writeHeapMetadata(
        pager, catalogHeapMetadata, catalogEntriesPage, catalogEntriesPage, 1);
    minidb::SlottedPageView::initialize(
        pager.getPage(catalogEntriesPage),
        catalogEntriesPage,
        pager.pageCount(),
        catalogHeapMetadata);
    minidb::SlottedPageView catalogEntries(
        pager.getPage(catalogEntriesPage), catalogEntriesPage, pager.pageCount());
    static_cast<void>(catalogEntries.insert(definitionBytes));
    pager.markDirty(catalogEntriesPage);

    writeHeapMetadata(pager, tableHeapMetadata, tableDataPage, tableDataPage, 1);
    minidb::SlottedPageView::initialize(
        pager.getPage(tableDataPage), tableDataPage, pager.pageCount(), tableHeapMetadata);
    minidb::SlottedPageView dataPage(
        pager.getPage(tableDataPage), tableDataPage, pager.pageCount());
    const auto slotId = dataPage.insert(tuple);
    pager.markDirty(tableDataPage);

    {
        auto& page = pager.getPage(indexMetadata);
        std::fill(page.begin(), page.end(), std::byte{0});
        using namespace minidb::persistent_index_metadata_layout;
        std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
        writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
        writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
        writeUint32(page, ROOT_PAGE_ID_OFFSET, indexLeaf);
        writeUint64(page, ENTRY_COUNT_OFFSET, 1);
        writeUint32(
            page, LEAF_MAX_KEYS_OFFSET, minidb::PersistentBPlusTree::PHYSICAL_LEAF_MAX_KEYS);
        writeUint32(
            page,
            INTERNAL_MAX_KEYS_OFFSET,
            minidb::PersistentBPlusTree::PHYSICAL_INTERNAL_MAX_KEYS);
        pager.markDirty(indexMetadata);
    }
    {
        auto& page = pager.getPage(indexLeaf);
        std::fill(page.begin(), page.end(), std::byte{0});
        using namespace minidb::persistent_bplus_leaf_layout;
        std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
        writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
        writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
        writeUint32(page, KEY_COUNT_OFFSET, 1);
        writeUint32(page, NEXT_LEAF_PAGE_ID_OFFSET, minidb::INVALID_PAGE_ID);
        writeUint32(page, PREVIOUS_LEAF_PAGE_ID_OFFSET, minidb::INVALID_PAGE_ID);
        writeUint32(page, entryOffset(0), 7);
        writeUint32(page, entryOffset(0) + KEY_SIZE, tableDataPage);
        writeUint16(page, entryOffset(0) + KEY_SIZE + RECORD_PAGE_ID_SIZE, slotId);
        pager.markDirty(indexLeaf);
    }
    {
        auto& page = pager.getPage(catalogMetadata);
        std::fill(page.begin(), page.end(), std::byte{0});
        using namespace minidb::catalog_metadata_layout;
        std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
        writeUint32(page, VERSION_OFFSET, CURRENT_VERSION);
        writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
        writeUint32(page, ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET, catalogHeapMetadata);
        writeUint64(page, NEXT_TABLE_ID_OFFSET, 2);
        writeUint64(page, TABLE_COUNT_OFFSET, 1);
        pager.markDirty(catalogMetadata);
    }
    {
        auto& page = pager.getPage(freePage);
        std::fill(page.begin(), page.end(), std::byte{0});
        using namespace minidb::free_page_layout;
        std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
        writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
        writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
        writeUint32(page, NEXT_FREE_PAGE_ID_OFFSET, minidb::INVALID_PAGE_ID);
        pager.markDirty(freePage);
    }
    pager.updateCatalogRootPageId(catalogMetadata);
    pager.updateFreeListRootPageId(freePage);
    pager.flushAll();
}

void testLegacyFormatCompatibility() {
    minidb::test::TemporaryDatabase database("buffer_legacy_compatibility");
    buildLegacyDatabase(database.path());
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = minidb::Catalog::open(
        storage.bufferPool, storage.diskManager, storage.allocator);
    minidb::sql::SqlEngine engine(catalog);
    const auto result = engine.execute(
        "SELECT name FROM legacy_users WHERE id = 7");
    const auto& selection = std::get<minidb::sql::SelectResult>(result);
    minidb::test::require(
        selection.rows.size() == 1
            && std::get<std::string>(selection.rows.front().front()) == "legacy",
        "Buffer-backed engine did not read the legacy-format row/index");
    minidb::test::requireThrows<std::runtime_error>(
        [&] {
            static_cast<void>(engine.execute(
                "INSERT INTO legacy_users VALUES (7, 'duplicate')"));
        },
        "Duplicate-key error path did not reject the insert");
    minidb::test::requireBufferClean(storage);
    catalog.validate();
    const auto expectedFree = storage.diskManager.databaseHeader().freeListRootPageId;
    minidb::test::require(
        storage.allocator.allocatePage() == expectedFree,
        "Buffer-backed allocator did not pop the legacy free-list page");
    storage.allocator.releasePage(expectedFree);
    minidb::test::requireBufferClean(storage);
}

void executeTinyPoolCycle(
    const std::filesystem::path& path,
    std::size_t frameCount,
    bool create) {
    minidb::test::TestStorage storage(path, frameCount, 2);
    auto catalog = create
        ? minidb::Catalog::openOrCreate(
              storage.bufferPool, storage.diskManager, storage.allocator)
        : minidb::Catalog::open(storage.bufferPool, storage.diskManager, storage.allocator);
    minidb::sql::SqlEngine engine(catalog);
    if (create) {
        static_cast<void>(engine.execute(
            "CREATE TABLE tiny (id UINT32 PRIMARY KEY, payload VARCHAR(4000) NOT NULL)"));
        for (std::uint32_t key = 0; key < 430; ++key) {
            static_cast<void>(engine.execute(
                "INSERT INTO tiny VALUES (" + std::to_string(key) + ", 'x')"));
            if (key % 50 == 0) minidb::test::requireBufferClean(storage);
        }
        const auto selected = std::get<minidb::sql::SelectResult>(
            engine.execute("SELECT payload FROM tiny WHERE id = 429"));
        minidb::test::require(selected.rows.size() == 1, "Tiny-pool PK lookup failed");
        const auto scanned = std::get<minidb::sql::SelectResult>(
            engine.execute("SELECT id FROM tiny WHERE payload = 'x'"));
        minidb::test::require(scanned.rows.size() == 430, "Tiny-pool heap scan failed");
        static_cast<void>(engine.execute("UPDATE tiny SET id = 1000 WHERE id = 1"));
        const std::string large(3'000, 'z');
        static_cast<void>(engine.execute(
            "UPDATE tiny SET payload = '" + large + "' WHERE id = 2"));
        for (std::uint32_t key = 3; key < 333; ++key) {
            static_cast<void>(engine.execute(
                "DELETE FROM tiny WHERE id = " + std::to_string(key)));
        }
        for (std::uint32_t key = 2'000; key < 2'050; ++key) {
            static_cast<void>(engine.execute(
                "INSERT INTO tiny VALUES (" + std::to_string(key) + ", 'reuse')"));
        }
    } else {
        const auto selected = std::get<minidb::sql::SelectResult>(
            engine.execute("SELECT payload FROM tiny WHERE id = 1000"));
        minidb::test::require(selected.rows.size() == 1, "Tiny-pool reopen lost PK change");
        static_cast<void>(engine.execute("DELETE FROM tiny WHERE id = 1000"));
        static_cast<void>(engine.execute("INSERT INTO tiny VALUES (3000, 'after_reopen')"));
    }
    catalog.validate();
    storage.allocator.validate();
    minidb::test::requireBufferClean(storage);
    storage.bufferPool.flushAll();
}

void testTinyPoolSqlAndReopen() {
    for (const std::size_t frames : {std::size_t{3}, std::size_t{2}}) {
        minidb::test::TemporaryDatabase database(
            frames == 3 ? "buffer_sql_three_frames" : "buffer_sql_two_frames");
        executeTinyPoolCycle(database.path(), frames, true);
        executeTinyPoolCycle(database.path(), frames, false);
    }
}

} // namespace

int main() {
    try {
        testLegacyFormatCompatibility();
        testTinyPoolSqlAndReopen();
        std::cout << "buffer_engine_integration_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "buffer_engine_integration_test failed: " << error.what() << '\n';
        return 1;
    }
}
