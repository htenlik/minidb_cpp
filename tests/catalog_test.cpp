#include "minidb/byte_codec.hpp"
#include "minidb/catalog.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

minidb::Schema primarySchema() {
    return minidb::Schema::create({
        {"id", minidb::DataType::UINT32, false, true, 0},
        {"name", minidb::DataType::VARCHAR, false, false, 32},
    });
}

minidb::Schema heapSchema() {
    return minidb::Schema::create({
        {"message", minidb::DataType::VARCHAR, true, false, 100},
        {"visible", minidb::DataType::BOOLEAN, false, false, 0},
    });
}

void testBootstrapLayoutAndReopen() {
    minidb::test::TemporaryDatabase database("catalog_bootstrap");
    minidb::PageId catalogPageId = minidb::INVALID_PAGE_ID;
    minidb::PageId entriesHeapId = minidb::INVALID_PAGE_ID;
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        minidb::test::require(
            storage.diskManager.databaseHeader().catalogRootPageId == minidb::INVALID_PAGE_ID,
            "New database unexpectedly bootstrapped a catalog");
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        catalogPageId = catalog.metadataPageId();
        entriesHeapId = catalog.entriesHeapMetadataPageId();
        minidb::test::require(catalogPageId == 1 && entriesHeapId == 2,
                              "Catalog bootstrap did not allocate through PageAllocator");
        minidb::test::require(storage.diskManager.databaseHeader().catalogRootPageId == catalogPageId,
                              "Catalog root was not installed in database metadata");
        minidb::test::require(catalog.tableCount() == 0 && catalog.listTables().empty(),
                              "New catalog was not empty");

        const auto page = minidb::test::readPageCopy(storage, catalogPageId);
        using namespace minidb::catalog_metadata_layout;
        minidb::test::require(std::equal(MAGIC.begin(), MAGIC.end(), page.begin()),
                              "Catalog metadata magic was not encoded");
        minidb::test::require(
            minidb::byte_codec::readUint32(page, VERSION_OFFSET) == 1
                && minidb::byte_codec::readUint32(page, HEADER_SIZE_OFFSET) == 64
                && minidb::byte_codec::readUint32(
                       page, ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET) == entriesHeapId
                && minidb::byte_codec::readUint64(page, NEXT_TABLE_ID_OFFSET) == 1
                && minidb::byte_codec::readUint64(page, TABLE_COUNT_OFFSET) == 0,
            "Catalog metadata fields were not encoded little-endian");
        catalog.validate();
        storage.bufferPool.flushAll();
    }
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::open(storage.bufferPool, storage.diskManager, storage.allocator);
        minidb::test::require(catalog.metadataPageId() == catalogPageId
                                  && catalog.entriesHeapMetadataPageId() == entriesHeapId,
                              "Catalog stable identities did not survive reopen");
        catalog.validate();
    }
}

void testTableDefinitionBinaryLayout() {
    const minidb::TableDefinition definition{
        0x0102030405060708ULL,
        "users",
        primarySchema(),
        0x11223344U,
        0x55667788U,
    };
    const auto bytes = minidb::encodeTableDefinition(definition);
    using namespace minidb::table_definition_layout;
    minidb::test::require(std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin()),
                          "Table-definition magic was not encoded");
    minidb::test::require(
        minidb::byte_codec::readUint32(bytes, VERSION_OFFSET) == 1
            && minidb::byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) == 48
            && minidb::byte_codec::readUint32(bytes, ENCODED_SIZE_OFFSET) == bytes.size()
            && minidb::byte_codec::readUint64(bytes, TABLE_ID_OFFSET) == definition.tableId
            && minidb::byte_codec::readUint32(bytes, HEAP_METADATA_PAGE_ID_OFFSET)
                == definition.heapMetadataPageId
            && minidb::byte_codec::readUint32(bytes, PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET)
                == definition.primaryIndexMetadataPageId
            && minidb::byte_codec::readUint16(bytes, TABLE_NAME_LENGTH_OFFSET) == 5,
        "Table-definition fixed fields were encoded incorrectly");
    minidb::test::require(
        std::string{
            static_cast<char>(std::to_integer<unsigned char>(bytes[48])),
            static_cast<char>(std::to_integer<unsigned char>(bytes[49])),
            static_cast<char>(std::to_integer<unsigned char>(bytes[50])),
            static_cast<char>(std::to_integer<unsigned char>(bytes[51])),
            static_cast<char>(std::to_integer<unsigned char>(bytes[52]))} == "users",
        "Table name bytes were not stored directly after the header");
    minidb::test::require(minidb::decodeTableDefinition(bytes) == definition,
                          "Table definition did not round trip");

    auto corrupt = bytes;
    corrupt[0] = std::byte{'X'};
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::decodeTableDefinition(corrupt)); },
        "Bad table-definition magic was accepted");
    corrupt = bytes;
    minidb::byte_codec::writeUint32(corrupt, VERSION_OFFSET, 2);
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::decodeTableDefinition(corrupt)); },
        "Unsupported table-definition version was accepted");
    corrupt = bytes;
    minidb::byte_codec::writeUint32(corrupt, SCHEMA_SIZE_OFFSET, 1);
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::decodeTableDefinition(corrupt)); },
        "Malformed table-definition lengths were accepted");
}

void testCreateFindListAndPersistence() {
    minidb::test::TemporaryDatabase database("catalog_tables");
    minidb::TableId usersId = minidb::INVALID_TABLE_ID;
    minidb::TableId logsId = minidb::INVALID_TABLE_ID;
    minidb::PageId usersHeap = minidb::INVALID_PAGE_ID;
    minidb::PageId usersIndex = minidb::INVALID_PAGE_ID;
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        auto users = catalog.createTable("Users", primarySchema());
        auto logs = catalog.createTable("LOGS", heapSchema());
        usersId = users.id();
        logsId = logs.id();
        usersHeap = users.definition().heapMetadataPageId;
        usersIndex = users.definition().primaryIndexMetadataPageId;
        minidb::test::require(usersId == 1 && logsId == 2,
                              "Catalog TableIds were not monotonic from one");
        minidb::test::require(users.name() == "users" && logs.name() == "logs",
                              "Catalog did not persist normalized table names");
        minidb::test::require(users.hasPrimaryKey() && !logs.hasPrimaryKey(),
                              "Catalog created incorrect primary-index presence");
        minidb::test::require(usersHeap != logs.definition().heapMetadataPageId
                                  && usersIndex != minidb::INVALID_PAGE_ID,
                              "Independent tables shared heap metadata or lost the index");
        minidb::test::requireThrows<std::invalid_argument>(
            [&] { static_cast<void>(catalog.createTable("USERS", primarySchema())); },
            "Duplicate normalized table name was accepted");
        const auto listed = catalog.listTables();
        minidb::test::require(listed.size() == 2 && listed[0].tableId == usersId
                                  && listed[1].tableId == logsId,
                              "Catalog list was not ordered by ascending TableId");
        minidb::test::require(catalog.findTable("uSeRs")->schema == primarySchema()
                                  && catalog.findTable(logsId)->schema == heapSchema(),
                              "Catalog lookup lost a persisted schema");
        catalog.validate();
        storage.bufferPool.flushAll();
    }
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        minidb::test::require(catalog.tableCount() == 2,
                              "Catalog table count did not survive reopen");
        const auto users = catalog.findTable(usersId);
        minidb::test::require(users.has_value()
                                  && users->heapMetadataPageId == usersHeap
                                  && users->primaryIndexMetadataPageId == usersIndex,
                              "Table storage identities did not survive reopen");
        auto logs = catalog.openTable("logs");
        minidb::test::require(!logs.hasPrimaryKey(),
                              "No-PK table reopened with an index");
        auto third = catalog.createTable("audit", heapSchema());
        minidb::test::require(third.id() == 3, "next TableId did not persist across reopen");
        catalog.validate();
    }
}

template <typename Mutator>
void requireCatalogMetadataCorruption(std::string_view name, Mutator&& mutate) {
    minidb::test::TemporaryDatabase database(name);
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    minidb::test::mutatePage(storage, catalog.metadataPageId(), [&](auto page) {
        mutate(storage, catalog, page);
    });
    minidb::test::requireThrows<std::runtime_error>(
        [&] { static_cast<void>(minidb::Catalog::open(storage.bufferPool, storage.diskManager, storage.allocator)); },
        "Catalog accepted corrupt metadata");
}

void testCatalogCorruptionDetection() {
    requireCatalogMetadataCorruption("catalog_bad_magic", [](auto&, auto&, auto& page) {
        page[minidb::catalog_metadata_layout::MAGIC_OFFSET] = std::byte{'X'};
    });
    requireCatalogMetadataCorruption("catalog_bad_version", [](auto&, auto&, auto& page) {
        minidb::byte_codec::writeUint32(
            page, minidb::catalog_metadata_layout::VERSION_OFFSET, 2);
    });
    requireCatalogMetadataCorruption("catalog_dangling_entries", [](auto& storage, auto&, auto& page) {
        minidb::byte_codec::writeUint32(
            page,
            minidb::catalog_metadata_layout::ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET,
            storage.diskManager.pageCount() + 5);
    });
    requireCatalogMetadataCorruption("catalog_count_mismatch", [](auto&, auto&, auto& page) {
        minidb::byte_codec::writeUint64(
            page, minidb::catalog_metadata_layout::TABLE_COUNT_OFFSET, 1);
    });

    minidb::test::TemporaryDatabase database("catalog_bad_definition");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    static_cast<void>(catalog.createTable("users", primarySchema()));
    auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
    const auto entry = entries.scan().front();
    auto corrupt = entry.second;
    corrupt[minidb::table_definition_layout::MAGIC_OFFSET] = std::byte{'X'};
    minidb::test::require(entries.tryUpdate(entry.first, corrupt),
                          "Catalog corruption fixture could not update entry in place");
    minidb::test::requireThrows<std::runtime_error>(
        [&] { catalog.validate(); },
        "Catalog accepted an invalid table definition");
}

void testCatalogEntryIdentityAndReferenceCorruption() {
    {
        minidb::test::TemporaryDatabase database("catalog_duplicate_id");
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        static_cast<void>(catalog.createTable("alpha", heapSchema()));
        static_cast<void>(catalog.createTable("bravo", heapSchema()));
        auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
        auto records = entries.scan();
        auto duplicate = records[1].second;
        const auto firstDefinition = minidb::decodeTableDefinition(records[0].second);
        minidb::byte_codec::writeUint64(
            duplicate, minidb::table_definition_layout::TABLE_ID_OFFSET,
            firstDefinition.tableId);
        minidb::test::require(entries.tryUpdate(records[1].first, duplicate),
                              "Duplicate-ID corruption fixture update failed");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { catalog.validate(); },
            "Catalog accepted duplicate TableIds");
    }
    {
        minidb::test::TemporaryDatabase database("catalog_duplicate_name");
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        static_cast<void>(catalog.createTable("alpha", heapSchema()));
        static_cast<void>(catalog.createTable("bravo", heapSchema()));
        auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
        auto records = entries.scan();
        auto duplicate = records[1].second;
        const std::string replacement = "alpha";
        for (std::size_t index = 0; index < replacement.size(); ++index) {
            duplicate[minidb::table_definition_layout::HEADER_SIZE + index] =
                static_cast<std::byte>(static_cast<unsigned char>(replacement[index]));
        }
        minidb::test::require(entries.tryUpdate(records[1].first, duplicate),
                              "Duplicate-name corruption fixture update failed");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { catalog.validate(); },
            "Catalog accepted duplicate normalized table names");
    }
    {
        minidb::test::TemporaryDatabase database("catalog_dangling_heap");
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        static_cast<void>(catalog.createTable("alpha", heapSchema()));
        auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
        auto record = entries.scan().front();
        minidb::byte_codec::writeUint32(
            record.second,
            minidb::table_definition_layout::HEAP_METADATA_PAGE_ID_OFFSET,
            storage.diskManager.pageCount() + 10);
        minidb::test::require(entries.tryUpdate(record.first, record.second),
                              "Dangling-heap corruption fixture update failed");
        minidb::test::requireThrows<std::out_of_range>(
            [&] { catalog.validate(); },
            "Catalog accepted dangling table heap metadata");
    }
    {
        minidb::test::TemporaryDatabase database("catalog_dangling_index");
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        static_cast<void>(catalog.createTable("alpha", primarySchema()));
        auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
        auto record = entries.scan().front();
        minidb::byte_codec::writeUint32(
            record.second,
            minidb::table_definition_layout::PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET,
            storage.diskManager.pageCount() + 10);
        minidb::test::require(entries.tryUpdate(record.first, record.second),
                              "Dangling-index corruption fixture update failed");
        minidb::test::requireThrows<std::out_of_range>(
            [&] { catalog.validate(); },
            "Catalog accepted dangling primary-index metadata");
    }
    {
        minidb::test::TemporaryDatabase database("catalog_index_schema_mismatch");
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = minidb::Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        static_cast<void>(catalog.createTable("alpha", primarySchema()));
        auto entries = minidb::TupleStore::open(storage.bufferPool, storage.diskManager, storage.allocator, catalog.entriesHeapMetadataPageId());
        auto record = entries.scan().front();
        minidb::byte_codec::writeUint32(
            record.second,
            minidb::table_definition_layout::PRIMARY_INDEX_METADATA_PAGE_ID_OFFSET,
            minidb::INVALID_PAGE_ID);
        minidb::test::require(entries.tryUpdate(record.first, record.second),
                              "Index/schema mismatch corruption fixture update failed");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { catalog.validate(); },
            "Catalog accepted primary-index presence inconsistent with schema");
    }
}

} // namespace

int main() {
    try {
        testBootstrapLayoutAndReopen();
        testTableDefinitionBinaryLayout();
        testCreateFindListAndPersistence();
        testCatalogCorruptionDetection();
        testCatalogEntryIdentityAndReferenceCorruption();
        std::cout << "catalog_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "catalog_test failed: " << error.what() << '\n';
        return 1;
    }
}
