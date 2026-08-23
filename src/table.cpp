#include "minidb/table.hpp"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace minidb {

Table Table::open(Pager& pager, TableDefinition definition) {
    static_cast<void>(encodeTableDefinition(definition));
    if (definition.heapMetadataPageId >= pager.pageCount()) {
        throw std::out_of_range("Table heap metadata PageId does not exist.");
    }
    auto heap = TupleStore::open(pager, definition.heapMetadataPageId);
    std::optional<PersistentBPlusTree> index;
    if (definition.primaryIndexMetadataPageId != INVALID_PAGE_ID) {
        if (definition.primaryIndexMetadataPageId >= pager.pageCount()) {
            throw std::out_of_range("Table primary-index metadata PageId does not exist.");
        }
        index.emplace(PersistentBPlusTree::open(
            pager, definition.primaryIndexMetadataPageId));
    }
    return Table(std::move(definition), std::move(heap), std::move(index));
}

IndexKey Table::primaryKeyOf(const RowValues& values) const {
    const auto columnIndex = definition_.schema.primaryKeyColumn();
    if (!columnIndex.has_value()) {
        throw std::logic_error("Table has no primary key.");
    }
    return std::get<std::uint32_t>(values[*columnIndex]);
}

PersistentBPlusTree& Table::requirePrimaryIndex() {
    if (!primaryIndex_) {
        throw std::logic_error("Operation requires a primary-key index.");
    }
    return *primaryIndex_;
}

const PersistentBPlusTree& Table::requirePrimaryIndex() const {
    if (!primaryIndex_) {
        throw std::logic_error("Operation requires a primary-key index.");
    }
    return *primaryIndex_;
}

RecordId Table::insert(const RowValues& values) {
    const auto encoded = TupleCodec::encode(definition_.schema, values);
    if (!primaryIndex_) {
        return heap_.insert(encoded);
    }

    const auto key = primaryKeyOf(values);
    if (primaryIndex_->find(key).has_value()) {
        throw std::invalid_argument("Duplicate primary key.");
    }
    const auto recordId = heap_.insert(encoded);
    try {
        if (!primaryIndex_->insert(key, recordId)) {
            heap_.erase(recordId);
            throw std::runtime_error("Primary-index insertion unexpectedly rejected a new key.");
        }
    } catch (...) {
        try {
            if (heap_.get(recordId) == encoded) {
                heap_.erase(recordId);
            }
        } catch (...) {
        }
        throw;
    }
    return recordId;
}

RowValues Table::get(RecordId recordId) const {
    return TupleCodec::decode(definition_.schema, heap_.get(recordId));
}

std::optional<TableRow> Table::findByPrimaryKey(IndexKey key) const {
    const auto& index = requirePrimaryIndex();
    const auto recordId = index.find(key);
    if (!recordId.has_value()) {
        return std::nullopt;
    }
    auto values = get(*recordId);
    if (primaryKeyOf(values) != key) {
        throw std::runtime_error("Primary index key disagrees with its tuple.");
    }
    return TableRow{*recordId, std::move(values)};
}

std::vector<TableRow> Table::scan() const {
    const auto tuples = heap_.scan();
    std::vector<TableRow> rows;
    rows.reserve(tuples.size());
    for (const auto& [recordId, bytes] : tuples) {
        rows.push_back(TableRow{
            recordId,
            TupleCodec::decode(definition_.schema, bytes),
        });
    }
    return rows;
}

bool Table::eraseByPrimaryKey(IndexKey key) {
    auto& index = requirePrimaryIndex();
    const auto recordId = index.find(key);
    if (!recordId.has_value()) {
        return false;
    }
    const auto oldBytes = heap_.get(*recordId);
    if (primaryKeyOf(TupleCodec::decode(definition_.schema, oldBytes)) != key) {
        throw std::runtime_error("Primary index key disagrees with tuple during deletion.");
    }
    if (!index.erase(key)) {
        throw std::runtime_error("Primary index lost a key during table deletion.");
    }
    try {
        heap_.erase(*recordId);
    } catch (...) {
        try {
            static_cast<void>(index.insert(key, *recordId));
        } catch (...) {
        }
        throw;
    }
    return true;
}

void Table::erase(RecordId recordId) {
    const auto oldBytes = heap_.get(recordId);
    const auto values = TupleCodec::decode(definition_.schema, oldBytes);
    if (!primaryIndex_) {
        heap_.erase(recordId);
        return;
    }
    const auto key = primaryKeyOf(values);
    const auto indexed = primaryIndex_->find(key);
    if (!indexed.has_value() || *indexed != recordId) {
        throw std::runtime_error("Primary index does not identify the requested table RID.");
    }
    if (!primaryIndex_->erase(key)) {
        throw std::runtime_error("Primary index lost a key during RID deletion.");
    }
    try {
        heap_.erase(recordId);
    } catch (...) {
        try {
            static_cast<void>(primaryIndex_->insert(key, recordId));
        } catch (...) {
        }
        throw;
    }
}

std::optional<RecordId> Table::updateByPrimaryKey(
    IndexKey oldKey,
    const RowValues& newValues) {
    auto& index = requirePrimaryIndex();
    const auto oldRecordId = index.find(oldKey);
    if (!oldRecordId.has_value()) {
        return std::nullopt;
    }
    const auto newBytes = TupleCodec::encode(definition_.schema, newValues);
    const auto newKey = primaryKeyOf(newValues);
    if (newKey != oldKey && index.find(newKey).has_value()) {
        throw std::invalid_argument("Updated primary key already exists.");
    }
    const auto oldBytes = heap_.get(*oldRecordId);

    if (heap_.tryUpdate(*oldRecordId, newBytes)) {
        if (newKey == oldKey) {
            return oldRecordId;
        }
        if (!index.erase(oldKey)) {
            static_cast<void>(heap_.tryUpdate(*oldRecordId, oldBytes));
            throw std::runtime_error("Primary index lost the old key during update.");
        }
        try {
            if (!index.insert(newKey, *oldRecordId)) {
                throw std::runtime_error("Primary index rejected the validated replacement key.");
            }
        } catch (...) {
            try {
                static_cast<void>(index.insert(oldKey, *oldRecordId));
                static_cast<void>(heap_.tryUpdate(*oldRecordId, oldBytes));
            } catch (...) {
            }
            throw;
        }
        return oldRecordId;
    }

    const auto newRecordId = heap_.insert(newBytes);
    bool replacementIndexed = false;
    bool oldIndexRemoved = false;
    try {
        if (newKey == oldKey) {
            if (!index.erase(oldKey)) {
                throw std::runtime_error("Primary index lost the old key during relocation.");
            }
            oldIndexRemoved = true;
            if (!index.insert(newKey, newRecordId)) {
                throw std::runtime_error("Primary index rejected relocated RID.");
            }
            replacementIndexed = true;
        } else {
            if (!index.insert(newKey, newRecordId)) {
                throw std::runtime_error("Primary index rejected the validated replacement key.");
            }
            replacementIndexed = true;
            if (!index.erase(oldKey)) {
                throw std::runtime_error("Primary index lost the old key during relocation.");
            }
            oldIndexRemoved = true;
        }
        heap_.erase(*oldRecordId);
        return newRecordId;
    } catch (...) {
        try {
            if (replacementIndexed) {
                static_cast<void>(index.erase(newKey));
            }
            if (oldIndexRemoved) {
                static_cast<void>(index.insert(oldKey, *oldRecordId));
            }
            heap_.erase(newRecordId);
        } catch (...) {
        }
        throw;
    }
}

void Table::validate() const {
    heap_.validate();
    const auto rows = scan();
    if (rows.size() != heap_.size()) {
        throw std::runtime_error("Decoded table row count disagrees with tuple heap.");
    }
    if (!primaryIndex_) {
        if (definition_.schema.primaryKeyColumn().has_value()
            || definition_.primaryIndexMetadataPageId != INVALID_PAGE_ID) {
            throw std::runtime_error("Unindexed table definition declares a primary key.");
        }
        return;
    }

    primaryIndex_->validate();
    if (!definition_.schema.primaryKeyColumn().has_value()
        || definition_.primaryIndexMetadataPageId != primaryIndex_->metadataPageId()) {
        throw std::runtime_error("Primary-index presence disagrees with table definition.");
    }
    if (primaryIndex_->size() != rows.size()) {
        throw std::runtime_error("Primary-index size disagrees with table heap size.");
    }

    std::unordered_map<IndexKey, RecordId> heapEntries;
    heapEntries.reserve(rows.size());
    for (const auto& row : rows) {
        const auto key = primaryKeyOf(row.values);
        if (!heapEntries.emplace(key, row.recordId).second) {
            throw std::runtime_error("Table heap contains duplicate primary keys.");
        }
    }
    const auto indexEntries = primaryIndex_->scanAll();
    if (indexEntries.size() != heapEntries.size()) {
        throw std::runtime_error("Primary-index scan disagrees with table heap count.");
    }
    std::unordered_set<std::uint64_t> indexedRids;
    for (const auto& entry : indexEntries) {
        const auto found = heapEntries.find(entry.key);
        if (found == heapEntries.end() || found->second != entry.recordId) {
            throw std::runtime_error("Primary index contains a dangling or mismatched entry.");
        }
        const auto encodedRid = (static_cast<std::uint64_t>(entry.recordId.pageId) << 16U)
            | entry.recordId.slotId;
        if (!indexedRids.insert(encodedRid).second) {
            throw std::runtime_error("Primary index references one RID more than once.");
        }
        const auto values = get(entry.recordId);
        if (primaryKeyOf(values) != entry.key) {
            throw std::runtime_error("Primary index key does not match decoded tuple primary key.");
        }
    }
}

} // namespace minidb
