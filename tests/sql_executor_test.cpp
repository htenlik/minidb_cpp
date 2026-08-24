#include "minidb/catalog.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/table.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using minidb::Catalog;
using minidb::DataType;
using minidb::RowValues;
using minidb::sql::AccessPath;
using minidb::sql::CommandKind;
using minidb::sql::CommandResult;
using minidb::sql::QueryResult;
using minidb::sql::SelectResult;
using minidb::sql::SqlEngine;
using minidb::sql::SqlExecutionError;
using minidb::sql::SqlExecutionErrorKind;

const CommandResult& command(const QueryResult& result, CommandKind expected) {
    minidb::test::require(std::holds_alternative<CommandResult>(result),
                          "SQL command did not return CommandResult");
    const auto& value = std::get<CommandResult>(result);
    minidb::test::require(value.command == expected, "SQL command kind was incorrect");
    return value;
}

const SelectResult& selection(const QueryResult& result) {
    minidb::test::require(std::holds_alternative<SelectResult>(result),
                          "SELECT did not return SelectResult");
    return std::get<SelectResult>(result);
}

void requireExecutionError(
    SqlEngine& engine,
    std::string_view source,
    SqlExecutionErrorKind expectedKind,
    std::string_view expectedMessage = {}) {
    try {
        static_cast<void>(engine.execute(source));
    } catch (const SqlExecutionError& error) {
        minidb::test::require(error.kind() == expectedKind
                                  && error.span().begin.line >= 1
                                  && error.span().begin.column >= 1,
                              "SQL execution error kind/span was incorrect");
        if (!expectedMessage.empty()) {
            minidb::test::require(error.message().find(expectedMessage) != std::string::npos,
                                  "SQL execution error lacked expected message");
        }
        return;
    }
    throw std::runtime_error("Semantically invalid SQL executed successfully");
}

void createUsers(SqlEngine& engine) {
    const auto result = engine.execute(
        "CREATE TABLE users ("
        "id UINT32 PRIMARY KEY, "
        "username VARCHAR(32) NOT NULL, "
        "email VARCHAR(255), "
        "score INT64, "
        "active BOOLEAN NOT NULL)");
    minidb::test::require(
        command(result, CommandKind::CreateTable).affectedRows == 0,
        "CREATE TABLE affected-row semantics were incorrect");
}

void testCreateAndSemanticValidation() {
    minidb::test::TemporaryDatabase database("sql_executor_create");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    SqlEngine engine(catalog);
    createUsers(engine);

    const auto definition = catalog.findTable("UsErS");
    minidb::test::require(definition.has_value()
                              && definition->name == "users"
                              && definition->schema.columnCount() == 5
                              && definition->schema.column(0).type == DataType::UINT32
                              && definition->schema.column(0).primaryKey
                              && !definition->schema.column(0).nullable
                              && definition->schema.column(1).varcharMaxBytes == 32
                              && !definition->schema.column(1).nullable
                              && definition->schema.column(2).nullable
                              && definition->schema.column(3).nullable
                              && definition->primaryIndexMetadataPageId
                                  != minidb::INVALID_PAGE_ID,
                          "CREATE TABLE did not produce the expected persistent definition");

    requireExecutionError(
        engine, "CREATE TABLE USERS (x UINT32)",
        SqlExecutionErrorKind::Constraint, "already exists");
    requireExecutionError(
        engine, "CREATE TABLE duplicate_columns (Name UINT32, name INT64)",
        SqlExecutionErrorKind::Semantic, "duplicate column");
    requireExecutionError(
        engine, "CREATE TABLE bad_pk (id VARCHAR(10) PRIMARY KEY)",
        SqlExecutionErrorKind::Semantic, "must use UINT32");
    requireExecutionError(
        engine, "CREATE TABLE two_pk (a UINT32 PRIMARY KEY, b UINT32 PRIMARY KEY)",
        SqlExecutionErrorKind::Semantic, "more than one");
    requireExecutionError(
        engine, "CREATE TABLE nullable_pk (id UINT32 PRIMARY KEY NULL)",
        SqlExecutionErrorKind::Semantic, "cannot be nullable");
    requireExecutionError(
        engine, "CREATE TABLE bad_varchar (value VARCHAR(0))",
        SqlExecutionErrorKind::Semantic, "between 1");
    minidb::test::require(catalog.tableCount() == 1,
                          "Rejected CREATE TABLE mutated the catalog");
    catalog.validate();
}

void testInsertProjectionWhereAndIndexStats() {
    minidb::test::TemporaryDatabase database("sql_executor_select");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    SqlEngine engine(catalog);
    createUsers(engine);

    command(
        engine.execute("INSERT INTO users VALUES (1, 'alice', 'a@x', 100, TRUE)"),
        CommandKind::Insert);
    const auto secondResult = engine.execute(
        "INSERT INTO users (active, username, id) VALUES (FALSE, 'bob', 2)");
    const auto& second = command(secondResult, CommandKind::Insert);
    minidb::test::require(second.affectedRows == 1 && second.insertedRecordId.has_value(),
                          "INSERT result omitted affected row/RID");

    const auto allResult = engine.execute("SELECT * FROM users");
    const auto& all = selection(allResult);
    minidb::test::require(
        all.columns == std::vector<std::string>{"id", "username", "email", "score", "active"}
            && all.rows.size() == 2
            && std::holds_alternative<std::monostate>(all.rows[1][2])
            && std::holds_alternative<std::monostate>(all.rows[1][3])
            && all.stats.accessPath == AccessPath::HeapScan
            && all.stats.rowsExamined == 2 && all.stats.rowsReturned == 2,
        "SELECT * or omitted nullable INSERT values were incorrect");

    const auto projectedResult = engine.execute(
        "SELECT USERNAME, id, ID FROM USERS WHERE id = 1");
    const auto& projected = selection(projectedResult);
    minidb::test::require(
        projected.columns == std::vector<std::string>{"username", "id", "id"}
            && projected.rows == std::vector<RowValues>{{std::string("alice"), 1U, 1U}}
            && projected.stats.accessPath == AccessPath::PrimaryKeyLookup
            && projected.stats.indexLookups == 1
            && projected.stats.rowsExamined == 1
            && projected.stats.rowsReturned == 1,
        "Projection order/duplicates or primary-key lookup stats were incorrect");

    const auto reversedResult = engine.execute("SELECT id FROM users WHERE 2 = id");
    const auto& reversed = selection(reversedResult);
    minidb::test::require(reversed.rows.size() == 1
                              && reversed.stats.accessPath == AccessPath::PrimaryKeyLookup
                              && reversed.stats.indexLookups == 1,
                          "Reversed PK equality did not use the index");

    const auto conjunctionResult = engine.execute(
        "SELECT id FROM users WHERE active = FALSE AND id = 2");
    const auto& conjunction = selection(conjunctionResult);
    minidb::test::require(conjunction.rows.size() == 1
                              && conjunction.stats.accessPath
                                  == AccessPath::PrimaryKeyLookup
                              && conjunction.stats.rowsExamined == 1,
                          "PK equality inside AND did not use the index");

    const auto heapResult = engine.execute(
        "SELECT id FROM users WHERE username = 'bob'");
    const auto& heap = selection(heapResult);
    minidb::test::require(heap.rows.size() == 1
                              && heap.stats.accessPath == AccessPath::HeapScan
                              && heap.stats.rowsExamined == 2,
                          "Non-indexed predicate did not use a heap scan");

    const auto compoundResult = engine.execute(
        "SELECT id FROM users WHERE "
        "(score >= 100 AND NOT active = FALSE) OR username <> 'alice'");
    minidb::test::require(selection(compoundResult).rows.size() == 2,
                          "Compound WHERE evaluation was incorrect");
    minidb::test::require(
        selection(engine.execute("SELECT id FROM users WHERE id > 0 AND id <= 1")).rows.size()
                == 1
            && selection(engine.execute("SELECT id FROM users WHERE id != 1")).rows.size() == 1
            && selection(engine.execute("SELECT id FROM users WHERE username < 'bob'"))
                   .rows.size() == 1
            && selection(engine.execute("SELECT id FROM users WHERE -2 < -1")).rows.size() == 2,
        "Numeric/string/literal comparison operators were incorrect");

    const auto nullComparison = engine.execute(
        "SELECT id FROM users WHERE email = NULL OR NULL = NULL");
    minidb::test::require(selection(nullComparison).rows.empty(),
                          "NULL comparison did not evaluate to UNKNOWN");
    minidb::test::require(selection(engine.execute("SELECT id FROM users WHERE TRUE")).rows.size()
                              == 2
                              && selection(engine.execute("SELECT id FROM users WHERE NULL"))
                                     .rows.empty(),
                          "Bare TRUE/NULL WHERE semantics were incorrect");

    requireExecutionError(
        engine, "SELECT nope FROM users", SqlExecutionErrorKind::Semantic,
        "column 'nope'");
    requireExecutionError(
        engine, "SELECT * FROM missing", SqlExecutionErrorKind::Semantic,
        "table 'missing'");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE username = 42",
        SqlExecutionErrorKind::Semantic, "incompatible");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE active < TRUE",
        SqlExecutionErrorKind::Semantic, "BOOLEAN");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE active < NULL",
        SqlExecutionErrorKind::Semantic, "BOOLEAN");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE id = score",
        SqlExecutionErrorKind::Semantic, "incompatible");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE username",
        SqlExecutionErrorKind::Semantic, "must be BOOLEAN");
    requireExecutionError(
        engine, "SELECT * FROM users WHERE 42",
        SqlExecutionErrorKind::Semantic, "must be BOOLEAN");
    catalog.validate();
}

void testInsertErrorsAndIntegerBoundaries() {
    minidb::test::TemporaryDatabase database("sql_executor_insert_errors");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    SqlEngine engine(catalog);
    static_cast<void>(engine.execute(
        "CREATE TABLE numbers (id UINT32 PRIMARY KEY, value INT64 NOT NULL, note VARCHAR(3))"));
    static_cast<void>(engine.execute(
        "INSERT INTO numbers VALUES (4294967295, -9223372036854775808, 'abc')"));
    static_cast<void>(engine.execute(
        "INSERT INTO numbers VALUES (0, 9223372036854775807, NULL)"));
    auto table = catalog.openTable("numbers");
    const auto before = table.scan();

    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (-1, 0, NULL)",
        SqlExecutionErrorKind::Constraint, "negative");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (4294967296, 0, NULL)",
        SqlExecutionErrorKind::Constraint, "out of range");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (1, 9223372036854775808, NULL)",
        SqlExecutionErrorKind::Constraint, "out of range");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (1, -9223372036854775809, NULL)",
        SqlExecutionErrorKind::Constraint, "out of range");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (1, 0, 'abcd')",
        SqlExecutionErrorKind::Constraint, "VARCHAR");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (1, TRUE, NULL)",
        SqlExecutionErrorKind::Semantic, "incompatible");
    requireExecutionError(
        engine, "INSERT INTO numbers (id, id, value) VALUES (1, 2, 0)",
        SqlExecutionErrorKind::Semantic, "duplicate INSERT");
    requireExecutionError(
        engine, "INSERT INTO numbers (id) VALUES (1)",
        SqlExecutionErrorKind::Constraint, "required column");
    requireExecutionError(
        engine, "INSERT INTO numbers (value) VALUES (1)",
        SqlExecutionErrorKind::Constraint, "required column");
    requireExecutionError(
        engine, "INSERT INTO numbers (missing, id, value) VALUES (1, 2, 0)",
        SqlExecutionErrorKind::Semantic, "does not exist");
    requireExecutionError(
        engine, "INSERT INTO numbers (id, value) VALUES (1)",
        SqlExecutionErrorKind::Semantic, "count");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (1, 2, NULL, TRUE)",
        SqlExecutionErrorKind::Semantic, "count");
    requireExecutionError(
        engine, "INSERT INTO numbers VALUES (0, 1, NULL)",
        SqlExecutionErrorKind::Constraint, "Duplicate primary key");
    minidb::test::require(catalog.openTable("numbers").scan() == before,
                          "Rejected INSERT mutated table contents");
    catalog.validate();
}

void testUpdateDeleteRelocationAndNoPrimaryKey() {
    minidb::test::TemporaryDatabase database("sql_executor_mutations");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    SqlEngine engine(catalog);
    static_cast<void>(engine.execute(
        "CREATE TABLE items (id UINT32 PRIMARY KEY, payload VARCHAR(4000) NOT NULL, "
        "enabled BOOLEAN NOT NULL)"));
    static_cast<void>(engine.execute("INSERT INTO items VALUES (1, 'short', TRUE)"));
    static_cast<void>(engine.execute(
        "INSERT INTO items VALUES (2, '" + std::string(3000, 'x') + "', FALSE)"));
    const auto original = selection(engine.execute("SELECT * FROM items WHERE id = 1"))
                              .recordIds.front();

    const auto updateResult = engine.execute(
        "UPDATE items SET id = 3, payload = '" + std::string(1200, 'r')
        + "' WHERE id = 1 AND enabled = TRUE");
    const auto& update = command(updateResult, CommandKind::Update);
    const auto changedResult = engine.execute("SELECT * FROM items WHERE id = 3");
    const auto& changed = selection(changedResult);
    minidb::test::require(
        update.affectedRows == 1
            && update.stats.accessPath == AccessPath::PrimaryKeyLookup
            && update.stats.indexLookups == 1 && update.stats.rowsExamined == 1
            && changed.rows.size() == 1
            && std::get<std::string>(changed.rows[0][1]).size() == 1200
            && changed.recordIds.front() != original,
        "PK update/large-tuple Table relocation was incorrect");

    const auto beforeInvalid = catalog.openTable("items").scan();
    requireExecutionError(
        engine, "UPDATE items SET id = 2 WHERE id = 3",
        SqlExecutionErrorKind::Constraint, "already exists");
    requireExecutionError(
        engine, "UPDATE items SET enabled = 'yes' WHERE id = 3",
        SqlExecutionErrorKind::Semantic, "incompatible");
    requireExecutionError(
        engine, "UPDATE items SET enabled = TRUE, ENABLED = FALSE WHERE id = 3",
        SqlExecutionErrorKind::Semantic, "duplicate UPDATE");
    requireExecutionError(
        engine, "UPDATE items SET id = 9",
        SqlExecutionErrorKind::Constraint, "duplicate primary key");
    minidb::test::require(catalog.openTable("items").scan() == beforeInvalid,
                          "Rejected UPDATE mutated table contents");
    requireExecutionError(
        engine, "DELETE FROM items WHERE payload = 42",
        SqlExecutionErrorKind::Semantic, "incompatible");
    minidb::test::require(catalog.openTable("items").scan() == beforeInvalid,
                          "Rejected DELETE mutated table contents");

    const auto heapUpdateResult = engine.execute(
        "UPDATE items SET enabled = FALSE WHERE payload >= 'r'");
    const auto& heapUpdate = command(heapUpdateResult, CommandKind::Update);
    minidb::test::require(heapUpdate.affectedRows == 2
                              && heapUpdate.stats.accessPath == AccessPath::HeapScan
                              && heapUpdate.stats.rowsExamined == 2,
                          "Heap-scan UPDATE stats/count were incorrect");

    const auto deleteResult = engine.execute("DELETE FROM items WHERE 3 = id");
    const auto& deletion = command(deleteResult, CommandKind::Delete);
    minidb::test::require(deletion.affectedRows == 1
                              && deletion.stats.accessPath == AccessPath::PrimaryKeyLookup
                              && deletion.stats.indexLookups == 1,
                          "PK DELETE stats/count were incorrect");
    const auto missingDelete = command(
        engine.execute("DELETE FROM items WHERE id = 999"), CommandKind::Delete);
    minidb::test::require(missingDelete.affectedRows == 0
                              && missingDelete.stats.rowsExamined == 0,
                          "Missing PK DELETE stats/count were incorrect");
    const auto heapDelete = command(
        engine.execute("DELETE FROM items WHERE payload >= 'x'"), CommandKind::Delete);
    minidb::test::require(heapDelete.affectedRows == 1
                              && heapDelete.stats.accessPath == AccessPath::HeapScan
                              && heapDelete.stats.rowsExamined == 1,
                          "Arbitrary-predicate DELETE stats/count were incorrect");

    static_cast<void>(engine.execute(
        "CREATE TABLE logs (message VARCHAR(4000) NOT NULL, visible BOOLEAN NOT NULL)"));
    static_cast<void>(engine.execute("INSERT INTO logs VALUES ('one', TRUE)"));
    static_cast<void>(engine.execute(
        "INSERT INTO logs VALUES ('" + std::string(3000, 'z') + "', FALSE)"));
    const auto logRid = selection(engine.execute("SELECT * FROM logs WHERE visible = TRUE"))
                            .recordIds.front();
    const auto noPkUpdate = command(
        engine.execute(
            "UPDATE logs SET message = '" + std::string(1100, 'q')
            + "' WHERE visible = TRUE"),
        CommandKind::Update);
    const auto updatedLog = selection(engine.execute("SELECT * FROM logs WHERE visible = TRUE"));
    minidb::test::require(noPkUpdate.affectedRows == 1
                              && updatedLog.recordIds.front() != logRid,
                          "No-PK Table update did not support cross-page relocation");
    const auto updateAll = command(
        engine.execute("UPDATE logs SET visible = TRUE"), CommandKind::Update);
    minidb::test::require(updateAll.affectedRows == 2
                              && updateAll.stats.accessPath == AccessPath::HeapScan
                              && updateAll.stats.rowsExamined == 2,
                          "UPDATE without WHERE did not target every row");
    const auto deleteLogs = command(engine.execute("DELETE FROM logs"), CommandKind::Delete);
    minidb::test::require(deleteLogs.affectedRows == 2
                              && selection(engine.execute("SELECT * FROM logs")).rows.empty(),
                          "No-PK delete-all semantics were incorrect");
    catalog.validate();
}

void testPrimaryIndexAccessAtScale() {
    minidb::test::TemporaryDatabase database("sql_executor_pk_scale");
    minidb::test::TestStorage storage(database.path(), 3, 2);
    auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
    SqlEngine engine(catalog);
    static_cast<void>(engine.execute(
        "CREATE TABLE indexed_users (id UINT32 PRIMARY KEY, username VARCHAR(32) NOT NULL, "
        "active BOOLEAN NOT NULL)"));
    for (std::uint32_t key = 0; key < 600; ++key) {
        static_cast<void>(engine.execute(
            "INSERT INTO indexed_users VALUES (" + std::to_string(key)
            + ", 'user" + std::to_string(key) + "', "
            + (key % 2U == 0 ? "TRUE" : "FALSE") + ")"));
    }
    const auto direct = selection(
        engine.execute("SELECT username FROM indexed_users WHERE id = 500"));
    minidb::test::require(direct.rows == std::vector<RowValues>{{std::string("user500")}}
                              && direct.stats.accessPath == AccessPath::PrimaryKeyLookup
                              && direct.stats.indexLookups == 1
                              && direct.stats.rowsExamined == 1,
                          "Scaled PK lookup did not use one index probe/candidate");
    const auto conjunction = selection(engine.execute(
        "SELECT id FROM indexed_users WHERE id = 500 AND active = TRUE"));
    minidb::test::require(conjunction.rows.size() == 1
                              && conjunction.stats.accessPath
                                  == AccessPath::PrimaryKeyLookup
                              && conjunction.stats.rowsExamined == 1,
                          "Scaled conjunctive PK lookup did not use the index");
    const auto control = selection(engine.execute(
        "SELECT id FROM indexed_users WHERE username = 'user500'"));
    minidb::test::require(control.rows.size() == 1
                              && control.stats.accessPath == AccessPath::HeapScan
                              && control.stats.rowsExamined == 600
                              && control.stats.indexLookups == 0,
                          "Scaled non-indexed control did not scan every row");
    catalog.validate();
}

void testReopenAndMultipleTables() {
    minidb::test::TemporaryDatabase database("sql_executor_reopen");
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = Catalog::openOrCreate(storage.bufferPool, storage.diskManager, storage.allocator);
        SqlEngine engine(catalog);
        static_cast<void>(engine.execute("CREATE TABLE a (id UINT32 PRIMARY KEY, text VARCHAR(20))"));
        static_cast<void>(engine.execute("CREATE TABLE b (flag BOOLEAN NOT NULL)"));
        static_cast<void>(engine.execute("INSERT INTO a VALUES (1, 'alpha')"));
        static_cast<void>(engine.execute("INSERT INTO b VALUES (TRUE)"));
        storage.bufferPool.flushAll();
    }
    {
        minidb::test::TestStorage storage(database.path(), 3, 2);
        auto catalog = Catalog::open(storage.bufferPool, storage.diskManager, storage.allocator);
        SqlEngine engine(catalog);
        const auto a = selection(engine.execute("SELECT text FROM A WHERE id = 1"));
        const auto b = selection(engine.execute("SELECT flag FROM b"));
        minidb::test::require(a.rows == std::vector<RowValues>{{std::string("alpha")}}
                                  && b.rows == std::vector<RowValues>{{true}}
                                  && a.stats.accessPath == AccessPath::PrimaryKeyLookup
                                  && b.stats.accessPath == AccessPath::HeapScan,
                              "Multiple-table contents/access paths failed after reopen");
        static_cast<void>(engine.execute("UPDATE a SET text = 'after' WHERE id = 1"));
        static_cast<void>(engine.execute("DELETE FROM b WHERE flag = TRUE"));
        catalog.validate();
    }
}

} // namespace

int main() {
    try {
        testCreateAndSemanticValidation();
        testInsertProjectionWhereAndIndexStats();
        testInsertErrorsAndIntegerBoundaries();
        testUpdateDeleteRelocationAndNoPrimaryKey();
        testReopenAndMultipleTables();
        testPrimaryIndexAccessAtScale();
        std::cout << "sql_executor_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sql_executor_test failed: " << error.what() << '\n';
        return 1;
    }
}
