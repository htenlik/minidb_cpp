#pragma once

#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/table_definition.hpp"
#include "minidb/tuple_codec.hpp"
#include "minidb/tuple_store.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace minidb {

struct TableRow {
    RecordId recordId;
    RowValues values;

    bool operator==(const TableRow&) const = default;
};

class Table {
public:
    [[nodiscard]] static Table open(Pager& pager, TableDefinition definition);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) noexcept = default;
    Table& operator=(Table&&) = delete;

    [[nodiscard]] const TableDefinition& definition() const noexcept { return definition_; }
    [[nodiscard]] TableId id() const noexcept { return definition_.tableId; }
    [[nodiscard]] const std::string& name() const noexcept { return definition_.name; }
    [[nodiscard]] const Schema& schema() const noexcept { return definition_.schema; }
    [[nodiscard]] std::uint64_t size() const { return heap_.size(); }
    [[nodiscard]] bool hasPrimaryKey() const noexcept { return primaryIndex_.has_value(); }

    [[nodiscard]] RecordId insert(const RowValues& values);
    [[nodiscard]] RowValues get(RecordId recordId) const;
    [[nodiscard]] std::optional<TableRow> findByPrimaryKey(IndexKey key) const;
    [[nodiscard]] std::vector<TableRow> scan() const;
    [[nodiscard]] bool eraseByPrimaryKey(IndexKey key);
    void erase(RecordId recordId);
    [[nodiscard]] std::optional<RecordId> updateByPrimaryKey(
        IndexKey oldKey,
        const RowValues& newValues);

    void validate() const;

private:
    TableDefinition definition_;
    TupleStore heap_;
    std::optional<PersistentBPlusTree> primaryIndex_;

    Table(
        TableDefinition definition,
        TupleStore heap,
        std::optional<PersistentBPlusTree> primaryIndex)
        : definition_(std::move(definition)),
          heap_(std::move(heap)),
          primaryIndex_(std::move(primaryIndex)) {}

    [[nodiscard]] IndexKey primaryKeyOf(const RowValues& values) const;
    [[nodiscard]] PersistentBPlusTree& requirePrimaryIndex();
    [[nodiscard]] const PersistentBPlusTree& requirePrimaryIndex() const;
};

} // namespace minidb
