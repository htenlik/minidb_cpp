#include "minidb/catalog.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/page_access.hpp"
#include "minidb/persistent_bplus_tree.hpp"
#include "minidb/table.hpp"
#include "minidb/page_lsn.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace minidb {
namespace {

void requireExistingDataPage(
    const DiskManager& diskManager,
    PageId pageId,
    PageId excludedPageId,
    const char* description) {
    if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
        || pageId == excludedPageId || pageId >= diskManager.pageCount()) {
        throw std::runtime_error(std::string(description) + " is not an existing data page.");
    }
}

} // namespace

Catalog Catalog::openOrCreate(
    BufferPoolManager& bufferPool,
    DiskManager& diskManager,
    PageAllocator& allocator) {
    const auto rootPageId = diskManager.databaseHeader().catalogRootPageId;
    if (rootPageId != INVALID_PAGE_ID) {
        return open(bufferPool, diskManager, allocator);
    }

    const auto metadataPageId = allocator.allocatePage();
    auto entries = TupleStore::create(bufferPool, diskManager, allocator);
    writeMetadataPage(bufferPool, diskManager, metadataPageId, Metadata{
        entries.metadataPageId(),
        1,
        0,
    });
    allocator.updateCatalogRootPageId(metadataPageId);
    Catalog catalog(
        bufferPool, diskManager, allocator, metadataPageId, std::move(entries));
    catalog.validate();
    return catalog;
}

Catalog Catalog::open(
    BufferPoolManager& bufferPool,
    DiskManager& diskManager,
    PageAllocator& allocator) {
    const auto metadataPageId = diskManager.databaseHeader().catalogRootPageId;
    if (metadataPageId == INVALID_PAGE_ID) {
        throw std::runtime_error("Database has no initialized catalog.");
    }
    const auto metadata = readMetadataPage(bufferPool, diskManager, metadataPageId);
    auto entries = TupleStore::open(
        bufferPool, diskManager, allocator, metadata.entriesHeapMetadataPageId);
    Catalog catalog(
        bufferPool, diskManager, allocator, metadataPageId, std::move(entries));
    catalog.validate();
    return catalog;
}

Catalog::Metadata Catalog::readMetadataPage(
    BufferPoolManager& bufferPool,
    const DiskManager& diskManager,
    PageId metadataPageId) {
    requireExistingDataPage(
        diskManager, metadataPageId, INVALID_PAGE_ID, "Catalog metadata PageId");
    const auto guard = requireReadPage(bufferPool, metadataPageId, "read catalog metadata");
    const auto page = guard.data();
    using namespace catalog_metadata_layout;
    if (!std::equal(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid catalog metadata magic/type.");
    }
    if (byte_codec::readUint32(page, VERSION_OFFSET) != CURRENT_VERSION) {
        throw std::runtime_error("Unsupported catalog metadata version.");
    }
    if (byte_codec::readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(page, FLAGS_OFFSET) != 0
        || !std::all_of(
            page.begin() + PAGE_LSN_OFFSET + PAGE_LSN_SIZE,
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Catalog metadata header or reserved bytes are malformed.");
    }
    static_cast<void>(readPersistentPageLsn(page));
    Metadata metadata{
        byte_codec::readUint32(page, ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET),
        byte_codec::readUint64(page, NEXT_TABLE_ID_OFFSET),
        byte_codec::readUint64(page, TABLE_COUNT_OFFSET),
    };
    requireExistingDataPage(
        diskManager, metadata.entriesHeapMetadataPageId, metadataPageId,
        "Catalog entries heap metadata PageId");
    if (metadata.nextTableId == INVALID_TABLE_ID) {
        throw std::runtime_error("Catalog next TableId uses the invalid value.");
    }
    return metadata;
}

void Catalog::writeMetadataPage(
    BufferPoolManager& bufferPool,
    const DiskManager& diskManager,
    PageId metadataPageId,
    const Metadata& metadata) {
    requireExistingDataPage(
        diskManager, metadataPageId, INVALID_PAGE_ID, "Catalog metadata PageId");
    requireExistingDataPage(
        diskManager, metadata.entriesHeapMetadataPageId, metadataPageId,
        "Catalog entries heap metadata PageId");
    if (metadata.nextTableId == INVALID_TABLE_ID) {
        throw std::invalid_argument("Catalog next TableId cannot be zero.");
    }
    auto guard = requireWritePage(bufferPool, metadataPageId, "write catalog metadata");
    auto page = guard.data();
    using namespace catalog_metadata_layout;
    std::fill(page.begin(), page.end(), std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(page, VERSION_OFFSET, CURRENT_VERSION);
    byte_codec::writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    byte_codec::writeUint32(
        page, ENTRIES_HEAP_METADATA_PAGE_ID_OFFSET, metadata.entriesHeapMetadataPageId);
    byte_codec::writeUint64(page, NEXT_TABLE_ID_OFFSET, metadata.nextTableId);
    byte_codec::writeUint64(page, TABLE_COUNT_OFFSET, metadata.tableCount);
}

Catalog::Metadata Catalog::readMetadata() const {
    const auto metadata = readMetadataPage(bufferPool_, diskManager_, metadataPageId_);
    if (metadata.entriesHeapMetadataPageId != entries_.metadataPageId()) {
        throw std::runtime_error("Catalog metadata disagrees with its opened entries heap.");
    }
    return metadata;
}

void Catalog::writeMetadata(const Metadata& metadata) {
    if (metadata.entriesHeapMetadataPageId != entries_.metadataPageId()) {
        throw std::logic_error("Cannot redirect an opened catalog to another entries heap.");
    }
    writeMetadataPage(bufferPool_, diskManager_, metadataPageId_, metadata);
}

std::uint64_t Catalog::tableCount() const {
    return readMetadata().tableCount;
}

std::vector<TableDefinition> Catalog::listTables() const {
    static_cast<void>(readMetadata());
    const auto records = entries_.scan();
    std::vector<TableDefinition> tables;
    tables.reserve(records.size());
    for (const auto& [recordId, bytes] : records) {
        static_cast<void>(recordId);
        tables.push_back(decodeTableDefinition(bytes));
    }
    std::sort(
        tables.begin(), tables.end(),
        [](const TableDefinition& left, const TableDefinition& right) {
            return left.tableId < right.tableId;
        });
    return tables;
}

std::optional<TableDefinition> Catalog::findTable(std::string_view name) const {
    const auto normalized = normalizeIdentifier(name);
    for (auto& definition : listTables()) {
        if (definition.name == normalized) {
            return std::move(definition);
        }
    }
    return std::nullopt;
}

std::optional<TableDefinition> Catalog::findTable(TableId tableId) const {
    if (tableId == INVALID_TABLE_ID) {
        return std::nullopt;
    }
    for (auto& definition : listTables()) {
        if (definition.tableId == tableId) {
            return std::move(definition);
        }
    }
    return std::nullopt;
}

Table Catalog::createTable(std::string_view name, const Schema& schema) {
    const auto normalized = normalizeIdentifier(name);
    if (findTable(normalized).has_value()) {
        throw std::invalid_argument("Catalog already contains the normalized table name.");
    }
    auto metadata = readMetadata();
    if (metadata.nextTableId == std::numeric_limits<TableId>::max()
        || metadata.tableCount == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Catalog table identifier or count is exhausted.");
    }

    const auto hasPrimaryKey = schema.primaryKeyColumn().has_value();
    const TableDefinition sizingDefinition{
        metadata.nextTableId,
        normalized,
        schema,
        1,
        hasPrimaryKey ? PageId{2} : INVALID_PAGE_ID,
    };
    static_cast<void>(encodeTableDefinition(sizingDefinition));

    auto heap = TupleStore::create(bufferPool_, diskManager_, allocator_);
    PageId indexMetadataPageId = INVALID_PAGE_ID;
    if (hasPrimaryKey) {
        auto index = PersistentBPlusTree::create(
            bufferPool_, diskManager_, allocator_);
        indexMetadataPageId = index.metadataPageId();
    }
    TableDefinition definition{
        metadata.nextTableId,
        normalized,
        schema,
        heap.metadataPageId(),
        indexMetadataPageId,
    };
    const auto encoded = encodeTableDefinition(definition);
    static_cast<void>(entries_.insert(encoded));
    ++metadata.nextTableId;
    ++metadata.tableCount;
    writeMetadata(metadata);
    return Table::open(bufferPool_, diskManager_, allocator_, std::move(definition));
}

Table Catalog::openTable(std::string_view name) const {
    auto definition = findTable(name);
    if (!definition.has_value()) {
        throw std::out_of_range("Catalog table name does not exist.");
    }
    return Table::open(bufferPool_, diskManager_, allocator_, std::move(*definition));
}

Table Catalog::openTable(TableId tableId) const {
    auto definition = findTable(tableId);
    if (!definition.has_value()) {
        throw std::out_of_range("Catalog TableId does not exist.");
    }
    return Table::open(bufferPool_, diskManager_, allocator_, std::move(*definition));
}

void Catalog::validate() const {
    const auto metadata = readMetadata();
    if (diskManager_.databaseHeader().catalogRootPageId != metadataPageId_) {
        throw std::runtime_error("Database catalog root disagrees with opened Catalog.");
    }
    entries_.validate();
    const auto definitions = listTables();
    if (definitions.size() != metadata.tableCount
        || entries_.size() != metadata.tableCount) {
        throw std::runtime_error("Catalog table count disagrees with catalog entries.");
    }

    std::unordered_set<TableId> tableIds;
    std::unordered_set<std::string> tableNames;
    TableId largestTableId = INVALID_TABLE_ID;
    for (const auto& definition : definitions) {
        if (!tableIds.insert(definition.tableId).second) {
            throw std::runtime_error("Catalog contains duplicate TableIds.");
        }
        if (!tableNames.insert(definition.name).second) {
            throw std::runtime_error("Catalog contains duplicate normalized table names.");
        }
        largestTableId = std::max(largestTableId, definition.tableId);
    }
    if (!definitions.empty() && metadata.nextTableId <= largestTableId) {
        throw std::runtime_error("Catalog next TableId does not exceed assigned IDs.");
    }

    const auto freePages = allocator_.freePageIds();
    const std::unordered_set<PageId> freePageSet(freePages.begin(), freePages.end());
    std::unordered_set<PageId> ownedPages;
    const auto claim = [&](PageId pageId, const char* description) {
        if (pageId == database_format::METADATA_PAGE_ID || pageId == INVALID_PAGE_ID
            || pageId >= diskManager_.pageCount() || freePageSet.contains(pageId)
            || !ownedPages.insert(pageId).second) {
            throw std::runtime_error(std::string("Catalog storage ownership conflict: ")
                                     + description + ".");
        }
    };

    claim(metadataPageId_, "catalog metadata");
    claim(entries_.metadataPageId(), "catalog entries heap metadata");
    for (const auto pageId : entries_.reachablePageIds()) {
        claim(pageId, "catalog entries heap page");
    }
    for (const auto& definition : definitions) {
        auto table = Table::open(bufferPool_, diskManager_, allocator_, definition);
        claim(definition.heapMetadataPageId, "table heap metadata");
        auto tableHeap = TupleStore::open(
            bufferPool_, diskManager_, allocator_, definition.heapMetadataPageId);
        for (const auto pageId : tableHeap.reachablePageIds()) {
            claim(pageId, "table heap page");
        }
        if (definition.primaryIndexMetadataPageId != INVALID_PAGE_ID) {
            claim(definition.primaryIndexMetadataPageId, "primary index metadata");
            auto index = PersistentBPlusTree::open(
                bufferPool_, diskManager_, allocator_, definition.primaryIndexMetadataPageId);
            for (const auto pageId : index.reachableNodePageIds()) {
                claim(pageId, "primary index node");
            }
        }
        table.validate();
    }
}

} // namespace minidb
