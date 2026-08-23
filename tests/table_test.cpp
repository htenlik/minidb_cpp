#include "minidb/catalog.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/table.hpp"
#include "minidb/tuple_store.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

minidb::Schema userSchema(std::uint32_t nameLimit = 4000) {
    return minidb::Schema::create({
        {"id", minidb::DataType::UINT32, false, true, 0},
        {"score", minidb::DataType::INT64, false, false, 0},
        {"active", minidb::DataType::BOOLEAN, false, false, 0},
        {"name", minidb::DataType::VARCHAR, true, false, nameLimit},
    });
}

minidb::RowValues userRow(
    std::uint32_t id,
    std::string name,
    std::int64_t score = -7,
    bool active = true) {
    return {id, score, active, std::move(name)};
}

void testPrimaryTableInsertLookupEraseAndReopen() {
    minidb::test::TemporaryDatabase database("table_primary");
    minidb::RecordId firstRid{};
    minidb::TableId tableId = minidb::INVALID_TABLE_ID;
    {
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        auto table = catalog.createTable("Users", userSchema(64));
        tableId = table.id();
        firstRid = table.insert(userRow(42, "alice"));
        const auto minimumRid = table.insert(userRow(0, "minimum"));
        const auto maximumRid = table.insert(userRow(
            std::numeric_limits<std::uint32_t>::max(), "maximum"));
        minidb::test::require(table.get(firstRid) == userRow(42, "alice"),
                              "Direct RID lookup did not decode the row");
        minidb::test::require(table.findByPrimaryKey(42)->recordId == firstRid
                                  && table.findByPrimaryKey(0)->recordId == minimumRid
                                  && table.findByPrimaryKey(
                                         std::numeric_limits<std::uint32_t>::max())->recordId
                                      == maximumRid,
                              "Primary-key lookup did not resolve expected RIDs");
        minidb::test::requireThrows<std::invalid_argument>(
            [&] { static_cast<void>(table.insert(userRow(42, "duplicate"))); },
            "Duplicate primary key was accepted");
        minidb::test::require(table.size() == 3 && table.scan().size() == 3,
                              "Table size/scan count was incorrect");
        minidb::test::require(table.eraseByPrimaryKey(0)
                                  && !table.eraseByPrimaryKey(123456),
                              "Primary-key erase semantics were incorrect");
        minidb::test::require(!table.findByPrimaryKey(0).has_value(),
                              "Erased primary key remained visible");
        table.validate();
        catalog.validate();
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::open(pager);
        auto table = catalog.openTable(tableId);
        minidb::test::require(table.findByPrimaryKey(42)->recordId == firstRid,
                              "Primary-key RID did not survive reopen");
        const auto afterReopen = table.insert(userRow(7, "after"));
        minidb::test::require(table.findByPrimaryKey(7)->recordId == afterReopen,
                              "Insertion after reopen did not update the persistent index");
        table.validate();
        catalog.validate();
    }
}

void testNoPrimaryKeyTableAndIsolation() {
    minidb::test::TemporaryDatabase database("table_no_pk");
    minidb::Pager pager(database.path().string());
    auto catalog = minidb::Catalog::openOrCreate(pager);
    const auto schema = minidb::Schema::create({
        {"text", minidb::DataType::VARCHAR, true, false, 32},
    });
    auto first = catalog.createTable("first", schema);
    auto second = catalog.createTable("second", schema);
    const minidb::RowValues duplicate{std::string("same")};
    const auto firstRid = first.insert(duplicate);
    static_cast<void>(first.insert(duplicate));
    const auto secondRid = second.insert({std::string("other")});
    minidb::test::require(first.size() == 2 && second.size() == 1,
                          "No-PK tables did not remain isolated or allow duplicates");
    minidb::test::requireThrows<std::logic_error>(
        [&] { static_cast<void>(first.findByPrimaryKey(1)); },
        "No-PK table performed a primary-key lookup");
    first.erase(firstRid);
    minidb::test::require(first.size() == 1
                              && std::get<std::string>(second.get(secondRid)[0]) == "other",
                          "RID erase affected another table");
    first.validate();
    second.validate();
    catalog.validate();
}

void testInPageAndRelocatingUpdates() {
    minidb::test::TemporaryDatabase database("table_updates");
    minidb::PageId metadataPageId = minidb::INVALID_PAGE_ID;
    minidb::RecordId relocatedRid{};
    {
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        auto table = catalog.createTable("users", userSchema());
        metadataPageId = table.definition().heapMetadataPageId;
        const auto originalRid = table.insert(userRow(1, "short"));
        static_cast<void>(table.insert(userRow(2, std::string(3500, 'x'))));

        const auto sameKey = table.updateByPrimaryKey(1, userRow(1, "slightly_larger", 99, false));
        minidb::test::require(sameKey.has_value() && *sameKey == originalRid,
                              "In-page update changed the RID");
        const auto changedKey = table.updateByPrimaryKey(
            1, userRow(3, std::string(20, 'a'), 100, true));
        minidb::test::require(changedKey.has_value() && *changedKey == originalRid
                                  && !table.findByPrimaryKey(1).has_value()
                                  && table.findByPrimaryKey(3)->recordId == originalRid,
                              "In-page primary-key update did not preserve RID/index mapping");

        const auto beforeDuplicate = table.findByPrimaryKey(3)->values;
        minidb::test::requireThrows<std::invalid_argument>(
            [&] {
                static_cast<void>(table.updateByPrimaryKey(
                    3, userRow(2, "duplicate-target")));
            },
            "Update accepted a duplicate replacement primary key");
        minidb::test::require(table.findByPrimaryKey(3)->values == beforeDuplicate,
                              "Rejected duplicate-key update mutated the old row");

        const auto relocation = table.updateByPrimaryKey(
            3, userRow(4, std::string(1200, 'r'), -100, false));
        minidb::test::require(relocation.has_value() && *relocation != originalRid,
                              "No-space table update did not relocate to a new RID");
        relocatedRid = *relocation;
        minidb::test::require(!table.findByPrimaryKey(3).has_value()
                                  && table.findByPrimaryKey(4)->recordId == relocatedRid,
                              "Relocating update did not replace the primary-index mapping");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { static_cast<void>(table.get(originalRid)); },
            "Old RID remained live after relocating update");
        table.validate();
        catalog.validate();
        pager.flushAll();
    }
    {
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::open(pager);
        auto table = catalog.openTable("users");
        minidb::test::require(table.definition().heapMetadataPageId == metadataPageId
                                  && table.findByPrimaryKey(4)->recordId == relocatedRid
                                  && std::get<std::string>(
                                         table.findByPrimaryKey(4)->values[3]).size() == 1200,
                              "Relocating update did not survive reopen");
        table.validate();
    }
}

void testTableIntegrityCorruptionDetection() {
    {
        minidb::test::TemporaryDatabase database("table_missing_index");
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        auto table = catalog.createTable("users", userSchema(32));
        static_cast<void>(table.insert(userRow(7, "seven")));
        auto index = minidb::PersistentBPlusTree::open(
            pager, table.definition().primaryIndexMetadataPageId);
        minidb::test::require(index.erase(7), "Integrity fixture could not erase index key");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { table.validate(); },
            "Table validator accepted a live PK tuple missing its index entry");
    }
    {
        minidb::test::TemporaryDatabase database("table_mismatched_index");
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        auto table = catalog.createTable("users", userSchema(32));
        const auto rid = table.insert(userRow(8, "eight"));
        auto index = minidb::PersistentBPlusTree::open(
            pager, table.definition().primaryIndexMetadataPageId);
        minidb::test::require(index.erase(8) && index.insert(9, rid),
                              "Integrity fixture could not create mismatched index key");
        minidb::test::requireThrows<std::runtime_error>(
            [&] { table.validate(); },
            "Table validator accepted an index key that disagrees with tuple PK");
    }
    {
        minidb::test::TemporaryDatabase database("table_dangling_index");
        minidb::Pager pager(database.path().string());
        auto catalog = minidb::Catalog::openOrCreate(pager);
        auto table = catalog.createTable("users", userSchema(32));
        const auto rid = table.insert(userRow(10, "ten"));
        auto heap = minidb::TupleStore::open(pager, table.definition().heapMetadataPageId);
        heap.erase(rid);
        minidb::test::requireThrows<std::runtime_error>(
            [&] { table.validate(); },
            "Table validator accepted an index RID targeting a missing heap tuple");
    }
}

} // namespace

int main() {
    try {
        testPrimaryTableInsertLookupEraseAndReopen();
        testNoPrimaryKeyTableAndIsolation();
        testInPageAndRelocatingUpdates();
        testTableIntegrityCorruptionDetection();
        std::cout << "table_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "table_test failed: " << error.what() << '\n';
        return 1;
    }
}
