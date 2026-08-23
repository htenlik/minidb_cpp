#include "minidb/persistent_bplus_tree.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint32(Pager::Page& output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint16(Pager::Page& output, std::size_t offset, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < 2; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

void writeUint64(Pager::Page& output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * BITS_PER_BYTE)) & 0xFFU);
    }
}

std::uint32_t readUint32(const Pager::Page& input, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(input[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

std::uint16_t readUint16(const Pager::Page& input, std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(input[offset + index])
            << (index * BITS_PER_BYTE));
    }
    return value;
}

std::uint64_t readUint64(const Pager::Page& input, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(input[offset + index])
            << (index * BITS_PER_BYTE);
    }
    return value;
}

void validateLogicalCapacities(std::uint32_t leafMaxKeys, std::uint32_t internalMaxKeys) {
    if (leafMaxKeys < PersistentBPlusTree::MIN_LOGICAL_MAX_KEYS
        || leafMaxKeys > PersistentBPlusTree::PHYSICAL_LEAF_MAX_KEYS) {
        throw std::invalid_argument("Persistent B+ tree leaf capacity is outside physical limits.");
    }
    if (internalMaxKeys < PersistentBPlusTree::MIN_LOGICAL_MAX_KEYS
        || internalMaxKeys > PersistentBPlusTree::PHYSICAL_INTERNAL_MAX_KEYS) {
        throw std::invalid_argument(
            "Persistent B+ tree internal capacity is outside physical limits.");
    }
}

} // namespace

struct PersistentBPlusTree::Metadata {
    PageId rootPageId = INVALID_PAGE_ID;
    std::uint64_t entryCount = 0;
    std::uint32_t leafMaxKeys = PHYSICAL_LEAF_MAX_KEYS;
    std::uint32_t internalMaxKeys = PHYSICAL_INTERNAL_MAX_KEYS;
};

struct PersistentBPlusTree::LeafNode {
    std::vector<IndexEntry> entries;
    PageId nextPageId = INVALID_PAGE_ID;
    PageId previousPageId = INVALID_PAGE_ID;
};

struct PersistentBPlusTree::InternalNode {
    std::vector<IndexKey> keys;
    std::vector<PageId> children;
};

struct PersistentBPlusTree::PathFrame {
    PageId pageId;
    std::size_t childIndex;
};

struct PersistentBPlusTree::SplitResult {
    PageId leftPageId;
    PageId rightPageId;
    IndexKey separator;
};

PersistentBPlusTree PersistentBPlusTree::create(
    Pager& pager,
    std::uint32_t leafMaxKeys,
    std::uint32_t internalMaxKeys) {
    validateLogicalCapacities(leafMaxKeys, internalMaxKeys);
    PageAllocator allocator(pager);
    const auto metadataPageId = allocator.allocatePage();
    PersistentBPlusTree tree(pager, metadataPageId);
    tree.writeMetadata(Metadata{
        INVALID_PAGE_ID,
        0,
        leafMaxKeys,
        internalMaxKeys,
    });
    return tree;
}

PersistentBPlusTree PersistentBPlusTree::open(Pager& pager, PageId metadataPageId) {
    PersistentBPlusTree tree(pager, metadataPageId);
    const auto metadata = tree.readMetadata();
    if (metadata.rootPageId != INVALID_PAGE_ID) {
        if (tree.isLeafPage(metadata.rootPageId)) {
            static_cast<void>(tree.readLeaf(metadata.rootPageId, metadata.leafMaxKeys));
        } else {
            static_cast<void>(
                tree.readInternal(metadata.rootPageId, metadata.internalMaxKeys));
        }
    }
    return tree;
}

PageId PersistentBPlusTree::rootPageId() const {
    return readMetadata().rootPageId;
}

std::uint64_t PersistentBPlusTree::size() const {
    return readMetadata().entryCount;
}

bool PersistentBPlusTree::empty() const {
    return size() == 0;
}

std::uint32_t PersistentBPlusTree::leafMaxKeys() const {
    return readMetadata().leafMaxKeys;
}

std::uint32_t PersistentBPlusTree::internalMaxKeys() const {
    return readMetadata().internalMaxKeys;
}

std::size_t PersistentBPlusTree::height() const {
    const auto metadata = readMetadata();
    PageId current = metadata.rootPageId;
    std::size_t result = 0;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree contains a cycle.");
        }
        ++result;
        if (isLeafPage(current)) {
            static_cast<void>(readLeaf(current, metadata.leafMaxKeys));
            break;
        }
        const auto internalNode = readInternal(current, metadata.internalMaxKeys);
        current = internalNode.children.front();
    }
    return result;
}

void PersistentBPlusTree::validateMetadataPageId() const {
    if (metadataPageId_ == database_format::METADATA_PAGE_ID
        || metadataPageId_ == INVALID_PAGE_ID
        || metadataPageId_ >= pager_.pageCount()) {
        throw std::out_of_range("Persistent B+ tree metadata page ID does not exist.");
    }
}

PersistentBPlusTree::Metadata PersistentBPlusTree::readMetadata() const {
    validateMetadataPageId();
    const auto& page = pager_.getPage(metadataPageId_);
    using namespace persistent_index_metadata_layout;

    if (!std::equal(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid persistent B+ tree metadata magic/type.");
    }
    const auto version = readUint32(page, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported persistent B+ tree metadata version "
            + std::to_string(version) + ".");
    }
    if (readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw std::runtime_error("Persistent B+ tree metadata has an invalid header size.");
    }
    if (!std::all_of(
            page.begin() + RESERVED_OFFSET,
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Persistent B+ tree metadata reserved bytes are not zero.");
    }

    Metadata metadata{
        readUint32(page, ROOT_PAGE_ID_OFFSET),
        readUint64(page, ENTRY_COUNT_OFFSET),
        readUint32(page, LEAF_MAX_KEYS_OFFSET),
        readUint32(page, INTERNAL_MAX_KEYS_OFFSET),
    };
    try {
        validateLogicalCapacities(metadata.leafMaxKeys, metadata.internalMaxKeys);
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("Persistent B+ tree metadata has invalid logical capacities.");
    }

    if (metadata.rootPageId == INVALID_PAGE_ID) {
        if (metadata.entryCount != 0) {
            throw std::runtime_error("Empty persistent B+ tree metadata has a nonzero size.");
        }
    } else {
        if (metadata.rootPageId == database_format::METADATA_PAGE_ID
            || metadata.rootPageId == metadataPageId_
            || metadata.rootPageId >= pager_.pageCount()) {
            throw std::runtime_error("Persistent B+ tree metadata has an invalid root page ID.");
        }
        if (metadata.entryCount == 0) {
            throw std::runtime_error("Nonempty persistent B+ tree metadata has a zero size.");
        }
    }
    return metadata;
}

void PersistentBPlusTree::writeMetadata(const Metadata& metadata) {
    validateLogicalCapacities(metadata.leafMaxKeys, metadata.internalMaxKeys);
    validateMetadataPageId();
    auto& page = pager_.getPage(metadataPageId_);
    using namespace persistent_index_metadata_layout;

    page.fill(std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint32(page, ROOT_PAGE_ID_OFFSET, metadata.rootPageId);
    writeUint64(page, ENTRY_COUNT_OFFSET, metadata.entryCount);
    writeUint32(page, LEAF_MAX_KEYS_OFFSET, metadata.leafMaxKeys);
    writeUint32(page, INTERNAL_MAX_KEYS_OFFSET, metadata.internalMaxKeys);
    pager_.markDirty(metadataPageId_);
}

bool PersistentBPlusTree::isLeafPage(PageId pageId) const {
    if (pageId == database_format::METADATA_PAGE_ID
        || pageId == INVALID_PAGE_ID
        || pageId == metadataPageId_
        || pageId >= pager_.pageCount()) {
        throw std::runtime_error("Persistent B+ tree references an invalid node page ID.");
    }
    const auto& page = pager_.getPage(pageId);
    if (std::equal(
            persistent_bplus_leaf_layout::MAGIC.begin(),
            persistent_bplus_leaf_layout::MAGIC.end(),
            page.begin() + persistent_bplus_leaf_layout::MAGIC_OFFSET)) {
        return true;
    }
    if (std::equal(
            persistent_bplus_internal_layout::MAGIC.begin(),
            persistent_bplus_internal_layout::MAGIC.end(),
            page.begin() + persistent_bplus_internal_layout::MAGIC_OFFSET)) {
        return false;
    }
    throw std::runtime_error("Persistent B+ tree node has an invalid magic/type.");
}

PersistentBPlusTree::LeafNode PersistentBPlusTree::readLeaf(
    PageId pageId,
    std::uint32_t logicalCapacity) const {
    if (!isLeafPage(pageId)) {
        throw std::runtime_error("Expected a persistent B+ tree leaf page.");
    }
    const auto& page = pager_.getPage(pageId);
    using namespace persistent_bplus_leaf_layout;

    const auto version = readUint32(page, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported persistent B+ tree leaf version " + std::to_string(version) + ".");
    }
    if (readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw std::runtime_error("Persistent B+ tree leaf has an invalid header size.");
    }
    const auto keyCount = readUint32(page, KEY_COUNT_OFFSET);
    if (keyCount == 0 || keyCount > logicalCapacity || keyCount > PHYSICAL_CAPACITY) {
        throw std::runtime_error("Persistent B+ tree leaf has an invalid key count.");
    }
    if (readUint32(page, RESERVED_OFFSET) != 0) {
        throw std::runtime_error("Persistent B+ tree leaf reserved field is not zero.");
    }

    const auto validateLink = [&](PageId link) {
        if (link == INVALID_PAGE_ID) {
            return;
        }
        if (link == database_format::METADATA_PAGE_ID
            || link == metadataPageId_
            || link == pageId
            || link >= pager_.pageCount()) {
            throw std::runtime_error("Persistent B+ tree leaf has an invalid sibling page ID.");
        }
    };

    LeafNode leaf;
    leaf.nextPageId = readUint32(page, NEXT_LEAF_PAGE_ID_OFFSET);
    leaf.previousPageId = readUint32(page, PREVIOUS_LEAF_PAGE_ID_OFFSET);
    validateLink(leaf.nextPageId);
    validateLink(leaf.previousPageId);
    leaf.entries.reserve(keyCount);

    for (std::size_t index = 0; index < keyCount; ++index) {
        const auto offset = entryOffset(index);
        const IndexEntry entry{
            readUint32(page, offset),
            RecordId{
                readUint32(page, offset + KEY_SIZE),
                readUint16(page, offset + KEY_SIZE + RECORD_PAGE_ID_SIZE),
            },
        };
        if (!entry.recordId.isValid()) {
            throw std::runtime_error("Persistent B+ tree leaf contains an invalid RecordId.");
        }
        if (!leaf.entries.empty() && leaf.entries.back().key >= entry.key) {
            throw std::runtime_error("Persistent B+ tree leaf keys are not strictly sorted.");
        }
        leaf.entries.push_back(entry);
    }

    if (!std::all_of(
            page.begin() + entryOffset(keyCount),
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Persistent B+ tree leaf unused bytes are not zero.");
    }
    return leaf;
}

void PersistentBPlusTree::writeLeaf(PageId pageId, const LeafNode& leaf) {
    if (leaf.entries.empty()
        || leaf.entries.size() > persistent_bplus_leaf_layout::PHYSICAL_CAPACITY) {
        throw std::logic_error("Cannot serialize a persistent B+ tree leaf with invalid size.");
    }
    auto& page = pager_.getPage(pageId);
    using namespace persistent_bplus_leaf_layout;
    page.fill(std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint32(page, KEY_COUNT_OFFSET, static_cast<std::uint32_t>(leaf.entries.size()));
    writeUint32(page, NEXT_LEAF_PAGE_ID_OFFSET, leaf.nextPageId);
    writeUint32(page, PREVIOUS_LEAF_PAGE_ID_OFFSET, leaf.previousPageId);

    for (std::size_t index = 0; index < leaf.entries.size(); ++index) {
        const auto offset = entryOffset(index);
        writeUint32(page, offset, leaf.entries[index].key);
        writeUint32(page, offset + KEY_SIZE, leaf.entries[index].recordId.pageId);
        writeUint16(
            page,
            offset + KEY_SIZE + RECORD_PAGE_ID_SIZE,
            leaf.entries[index].recordId.slotId);
    }
    pager_.markDirty(pageId);
}

PersistentBPlusTree::InternalNode PersistentBPlusTree::readInternal(
    PageId pageId,
    std::uint32_t logicalCapacity) const {
    if (isLeafPage(pageId)) {
        throw std::runtime_error("Expected a persistent B+ tree internal page.");
    }
    const auto& page = pager_.getPage(pageId);
    using namespace persistent_bplus_internal_layout;

    const auto version = readUint32(page, LAYOUT_VERSION_OFFSET);
    if (version != CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported persistent B+ tree internal version " + std::to_string(version) + ".");
    }
    if (readUint32(page, HEADER_SIZE_OFFSET) != HEADER_SIZE) {
        throw std::runtime_error("Persistent B+ tree internal page has an invalid header size.");
    }
    const auto keyCount = readUint32(page, KEY_COUNT_OFFSET);
    const auto childCount = readUint32(page, CHILD_COUNT_OFFSET);
    if (keyCount == 0 || keyCount > logicalCapacity || keyCount > PHYSICAL_CAPACITY) {
        throw std::runtime_error("Persistent B+ tree internal page has an invalid key count.");
    }
    if (childCount != keyCount + 1) {
        throw std::runtime_error("Persistent B+ tree internal child/key counts disagree.");
    }
    if (!std::all_of(
            page.begin() + RESERVED_OFFSET,
            page.begin() + HEADER_SIZE,
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Persistent B+ tree internal reserved bytes are not zero.");
    }

    InternalNode internalNode;
    internalNode.keys.reserve(keyCount);
    internalNode.children.reserve(childCount);
    for (std::size_t index = 0; index < childCount; ++index) {
        const auto childPageId = readUint32(page, childOffset(index));
        if (childPageId == database_format::METADATA_PAGE_ID
            || childPageId == INVALID_PAGE_ID
            || childPageId == metadataPageId_
            || childPageId == pageId
            || childPageId >= pager_.pageCount()) {
            throw std::runtime_error("Persistent B+ tree internal page has an invalid child ID.");
        }
        internalNode.children.push_back(childPageId);
        if (index < keyCount) {
            const auto key = readUint32(page, keyOffset(index));
            if (!internalNode.keys.empty() && internalNode.keys.back() >= key) {
                throw std::runtime_error(
                    "Persistent B+ tree internal separators are not strictly sorted.");
            }
            internalNode.keys.push_back(key);
        }
    }

    const auto usedSize = HEADER_SIZE + PAGE_ID_SIZE + (keyCount * KEY_CHILD_PAIR_SIZE);
    if (!std::all_of(
            page.begin() + usedSize,
            page.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Persistent B+ tree internal unused bytes are not zero.");
    }
    return internalNode;
}

void PersistentBPlusTree::writeInternal(
    PageId pageId,
    const InternalNode& internalNode) {
    if (internalNode.keys.empty()
        || internalNode.keys.size() > persistent_bplus_internal_layout::PHYSICAL_CAPACITY
        || internalNode.children.size() != internalNode.keys.size() + 1) {
        throw std::logic_error("Cannot serialize an invalid persistent B+ tree internal node.");
    }
    auto& page = pager_.getPage(pageId);
    using namespace persistent_bplus_internal_layout;
    page.fill(std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), page.begin() + MAGIC_OFFSET);
    writeUint32(page, LAYOUT_VERSION_OFFSET, CURRENT_VERSION);
    writeUint32(page, HEADER_SIZE_OFFSET, static_cast<std::uint32_t>(HEADER_SIZE));
    writeUint32(page, KEY_COUNT_OFFSET, static_cast<std::uint32_t>(internalNode.keys.size()));
    writeUint32(
        page,
        CHILD_COUNT_OFFSET,
        static_cast<std::uint32_t>(internalNode.children.size()));
    for (std::size_t index = 0; index < internalNode.children.size(); ++index) {
        writeUint32(page, childOffset(index), internalNode.children[index]);
        if (index < internalNode.keys.size()) {
            writeUint32(page, keyOffset(index), internalNode.keys[index]);
        }
    }
    pager_.markDirty(pageId);
}

PageId PersistentBPlusTree::findLeafPage(
    IndexKey key,
    const Metadata& metadata,
    std::vector<PathFrame>* path) const {
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return INVALID_PAGE_ID;
    }

    PageId current = metadata.rootPageId;
    std::unordered_set<PageId> visited;
    while (!isLeafPage(current)) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree contains a cycle.");
        }
        const auto internalNode = readInternal(current, metadata.internalMaxKeys);
        const auto position =
            std::upper_bound(internalNode.keys.begin(), internalNode.keys.end(), key);
        const auto childIndex =
            static_cast<std::size_t>(position - internalNode.keys.begin());
        if (path) {
            path->push_back(PathFrame{current, childIndex});
        }
        current = internalNode.children[childIndex];
    }
    static_cast<void>(readLeaf(current, metadata.leafMaxKeys));
    return current;
}

IndexKey PersistentBPlusTree::subtreeMinimum(
    PageId pageId,
    const Metadata& metadata) const {
    PageId current = pageId;
    std::unordered_set<PageId> visited;
    while (!isLeafPage(current)) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree contains a cycle.");
        }
        current = readInternal(current, metadata.internalMaxKeys).children.front();
    }
    return readLeaf(current, metadata.leafMaxKeys).entries.front().key;
}

PageId PersistentBPlusTree::allocateLeaf(const LeafNode& leaf) {
    const auto pageId = allocator_.allocatePage();
    writeLeaf(pageId, leaf);
    return pageId;
}

PageId PersistentBPlusTree::allocateInternal(const InternalNode& internalNode) {
    const auto pageId = allocator_.allocatePage();
    writeInternal(pageId, internalNode);
    return pageId;
}

PersistentBPlusTree::SplitResult PersistentBPlusTree::splitLeaf(
    PageId pageId,
    LeafNode leaf,
    const Metadata& metadata) {
    std::optional<LeafNode> oldNext;
    if (leaf.nextPageId != INVALID_PAGE_ID) {
        oldNext = readLeaf(leaf.nextPageId, metadata.leafMaxKeys);
        if (oldNext->previousPageId != pageId) {
            throw std::runtime_error("Persistent B+ tree leaf sibling links disagree.");
        }
    }

    const auto leftSize = (leaf.entries.size() + 1) / 2;
    LeafNode right;
    right.entries.assign(
        leaf.entries.begin() + static_cast<std::ptrdiff_t>(leftSize),
        leaf.entries.end());
    leaf.entries.erase(
        leaf.entries.begin() + static_cast<std::ptrdiff_t>(leftSize),
        leaf.entries.end());
    right.previousPageId = pageId;
    right.nextPageId = leaf.nextPageId;
    const auto rightPageId = allocateLeaf(right);
    leaf.nextPageId = rightPageId;
    writeLeaf(pageId, leaf);

    if (oldNext) {
        oldNext->previousPageId = rightPageId;
        writeLeaf(right.nextPageId, *oldNext);
    }
    return SplitResult{pageId, rightPageId, right.entries.front().key};
}

PersistentBPlusTree::SplitResult PersistentBPlusTree::splitInternal(
    PageId pageId,
    InternalNode internalNode) {
    const auto leftChildCount = (internalNode.children.size() + 1) / 2;
    const auto separator = internalNode.keys[leftChildCount - 1];

    InternalNode right;
    right.children.assign(
        internalNode.children.begin() + static_cast<std::ptrdiff_t>(leftChildCount),
        internalNode.children.end());
    right.keys.assign(
        internalNode.keys.begin() + static_cast<std::ptrdiff_t>(leftChildCount),
        internalNode.keys.end());
    internalNode.children.erase(
        internalNode.children.begin() + static_cast<std::ptrdiff_t>(leftChildCount),
        internalNode.children.end());
    internalNode.keys.erase(
        internalNode.keys.begin() + static_cast<std::ptrdiff_t>(leftChildCount - 1),
        internalNode.keys.end());

    const auto rightPageId = allocateInternal(right);
    writeInternal(pageId, internalNode);
    return SplitResult{pageId, rightPageId, separator};
}

bool PersistentBPlusTree::insert(IndexKey key, RecordId recordId) {
    if (!recordId.isValid()) {
        throw std::invalid_argument("Persistent B+ tree cannot index an invalid RecordId.");
    }

    auto metadata = readMetadata();
    if (metadata.entryCount == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Persistent B+ tree size has reached its maximum value.");
    }
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        metadata.rootPageId = allocateLeaf(LeafNode{{IndexEntry{key, recordId}}});
        metadata.entryCount = 1;
        writeMetadata(metadata);
        return true;
    }

    std::vector<PathFrame> path;
    const auto leafPageId = findLeafPage(key, metadata, &path);
    auto leaf = readLeaf(leafPageId, metadata.leafMaxKeys);
    const auto position = std::lower_bound(
        leaf.entries.begin(),
        leaf.entries.end(),
        key,
        [](const IndexEntry& entry, IndexKey candidate) { return entry.key < candidate; });
    if (position != leaf.entries.end() && position->key == key) {
        return false;
    }
    leaf.entries.insert(position, IndexEntry{key, recordId});

    std::optional<SplitResult> split;
    if (leaf.entries.size() <= metadata.leafMaxKeys) {
        writeLeaf(leafPageId, leaf);
    } else {
        split = splitLeaf(leafPageId, std::move(leaf), metadata);
    }

    while (split) {
        if (path.empty()) {
            metadata.rootPageId = allocateInternal(InternalNode{
                {split->separator},
                {split->leftPageId, split->rightPageId},
            });
            split.reset();
            break;
        }

        const auto frame = path.back();
        path.pop_back();
        auto parent = readInternal(frame.pageId, metadata.internalMaxKeys);
        if (frame.childIndex >= parent.children.size()
            || parent.children[frame.childIndex] != split->leftPageId) {
            throw std::runtime_error("Persistent B+ tree traversal path became inconsistent.");
        }
        parent.keys.insert(
            parent.keys.begin() + static_cast<std::ptrdiff_t>(frame.childIndex),
            split->separator);
        parent.children.insert(
            parent.children.begin() + static_cast<std::ptrdiff_t>(frame.childIndex + 1),
            split->rightPageId);

        if (parent.keys.size() <= metadata.internalMaxKeys) {
            writeInternal(frame.pageId, parent);
            split.reset();
        } else {
            split = splitInternal(frame.pageId, std::move(parent));
        }
    }

    ++metadata.entryCount;
    writeMetadata(metadata);
    return true;
}

std::size_t PersistentBPlusTree::minimumLeafKeys(const Metadata& metadata) noexcept {
    return (static_cast<std::size_t>(metadata.leafMaxKeys) + 1) / 2;
}

std::size_t PersistentBPlusTree::minimumInternalChildren(
    const Metadata& metadata) noexcept {
    return (static_cast<std::size_t>(metadata.internalMaxKeys) + 2) / 2;
}

void PersistentBPlusTree::rebuildInternalKeys(
    InternalNode& internalNode,
    const Metadata& metadata) const {
    if (internalNode.children.empty()) {
        throw std::logic_error("Persistent B+ tree internal node has no children.");
    }
    internalNode.keys.clear();
    internalNode.keys.reserve(internalNode.children.size() - 1);
    for (std::size_t index = 1; index < internalNode.children.size(); ++index) {
        const auto separator = subtreeMinimum(internalNode.children[index], metadata);
        if (!internalNode.keys.empty() && internalNode.keys.back() >= separator) {
            throw std::runtime_error("Persistent B+ tree child ranges are not strictly ordered.");
        }
        internalNode.keys.push_back(separator);
    }
}

void PersistentBPlusTree::refreshAncestorSeparators(
    const std::vector<PathFrame>& path,
    const Metadata& metadata) {
    for (auto position = path.rbegin(); position != path.rend(); ++position) {
        auto ancestor = readInternal(position->pageId, metadata.internalMaxKeys);
        if (position->childIndex >= ancestor.children.size()) {
            throw std::runtime_error("Persistent B+ tree traversal path became inconsistent.");
        }
        const auto previousKeys = ancestor.keys;
        rebuildInternalKeys(ancestor, metadata);
        if (ancestor.keys != previousKeys) {
            writeInternal(position->pageId, ancestor);
        }
    }
}

void PersistentBPlusTree::repairInternalAfterChildRemoval(
    PageId pageId,
    InternalNode internalNode,
    std::vector<PathFrame> path,
    Metadata& metadata) {
    if (internalNode.children.empty()) {
        throw std::logic_error("Persistent B+ tree internal node lost every child.");
    }
    if (internalNode.children.size() == 1) {
        internalNode.keys.clear();
    } else {
        rebuildInternalKeys(internalNode, metadata);
    }

    if (pageId == metadata.rootPageId) {
        if (internalNode.children.size() == 1) {
            metadata.rootPageId = internalNode.children.front();
            allocator_.releasePage(pageId);
        } else {
            writeInternal(pageId, internalNode);
        }
        return;
    }

    if (internalNode.children.size() >= minimumInternalChildren(metadata)) {
        writeInternal(pageId, internalNode);
        refreshAncestorSeparators(path, metadata);
        return;
    }
    if (path.empty()) {
        throw std::logic_error("Persistent B+ tree internal underflow has no parent path.");
    }

    const auto parentFrame = path.back();
    path.pop_back();
    auto parent = readInternal(parentFrame.pageId, metadata.internalMaxKeys);
    if (parentFrame.childIndex >= parent.children.size()
        || parent.children[parentFrame.childIndex] != pageId) {
        throw std::runtime_error("Persistent B+ tree traversal path became inconsistent.");
    }

    const auto minimumChildren = minimumInternalChildren(metadata);
    if (parentFrame.childIndex > 0) {
        const auto leftPageId = parent.children[parentFrame.childIndex - 1];
        auto left = readInternal(leftPageId, metadata.internalMaxKeys);
        if (left.children.size() > minimumChildren) {
            internalNode.children.insert(internalNode.children.begin(), left.children.back());
            left.children.pop_back();
            rebuildInternalKeys(left, metadata);
            rebuildInternalKeys(internalNode, metadata);
            writeInternal(leftPageId, left);
            writeInternal(pageId, internalNode);
            rebuildInternalKeys(parent, metadata);
            writeInternal(parentFrame.pageId, parent);
            refreshAncestorSeparators(path, metadata);
            return;
        }
    }

    if (parentFrame.childIndex + 1 < parent.children.size()) {
        const auto rightPageId = parent.children[parentFrame.childIndex + 1];
        auto right = readInternal(rightPageId, metadata.internalMaxKeys);
        if (right.children.size() > minimumChildren) {
            internalNode.children.push_back(right.children.front());
            right.children.erase(right.children.begin());
            rebuildInternalKeys(internalNode, metadata);
            rebuildInternalKeys(right, metadata);
            writeInternal(pageId, internalNode);
            writeInternal(rightPageId, right);
            rebuildInternalKeys(parent, metadata);
            writeInternal(parentFrame.pageId, parent);
            refreshAncestorSeparators(path, metadata);
            return;
        }
    }

    if (parentFrame.childIndex > 0) {
        const auto leftPageId = parent.children[parentFrame.childIndex - 1];
        auto left = readInternal(leftPageId, metadata.internalMaxKeys);
        left.children.insert(
            left.children.end(), internalNode.children.begin(), internalNode.children.end());
        rebuildInternalKeys(left, metadata);
        writeInternal(leftPageId, left);
        parent.children.erase(
            parent.children.begin() + static_cast<std::ptrdiff_t>(parentFrame.childIndex));
        allocator_.releasePage(pageId);
        repairInternalAfterChildRemoval(
            parentFrame.pageId, std::move(parent), std::move(path), metadata);
        return;
    }

    if (parent.children.size() < 2) {
        throw std::logic_error("Persistent B+ tree internal underflow has no sibling.");
    }
    const auto rightPageId = parent.children[1];
    auto right = readInternal(rightPageId, metadata.internalMaxKeys);
    internalNode.children.insert(
        internalNode.children.end(), right.children.begin(), right.children.end());
    rebuildInternalKeys(internalNode, metadata);
    writeInternal(pageId, internalNode);
    parent.children.erase(parent.children.begin() + 1);
    allocator_.releasePage(rightPageId);
    repairInternalAfterChildRemoval(
        parentFrame.pageId, std::move(parent), std::move(path), metadata);
}

void PersistentBPlusTree::rebalanceLeafAfterErase(
    PageId pageId,
    LeafNode leaf,
    std::vector<PathFrame> path,
    Metadata& metadata) {
    if (path.empty()) {
        throw std::logic_error("Persistent B+ tree leaf underflow has no parent path.");
    }
    const auto parentFrame = path.back();
    path.pop_back();
    auto parent = readInternal(parentFrame.pageId, metadata.internalMaxKeys);
    if (parentFrame.childIndex >= parent.children.size()
        || parent.children[parentFrame.childIndex] != pageId) {
        throw std::runtime_error("Persistent B+ tree traversal path became inconsistent.");
    }

    const auto minimumKeys = minimumLeafKeys(metadata);
    if (parentFrame.childIndex > 0) {
        const auto leftPageId = parent.children[parentFrame.childIndex - 1];
        auto left = readLeaf(leftPageId, metadata.leafMaxKeys);
        if (left.entries.size() > minimumKeys) {
            leaf.entries.insert(leaf.entries.begin(), left.entries.back());
            left.entries.pop_back();
            writeLeaf(leftPageId, left);
            writeLeaf(pageId, leaf);
            rebuildInternalKeys(parent, metadata);
            writeInternal(parentFrame.pageId, parent);
            refreshAncestorSeparators(path, metadata);
            return;
        }
    }

    if (parentFrame.childIndex + 1 < parent.children.size()) {
        const auto rightPageId = parent.children[parentFrame.childIndex + 1];
        auto right = readLeaf(rightPageId, metadata.leafMaxKeys);
        if (right.entries.size() > minimumKeys) {
            leaf.entries.push_back(right.entries.front());
            right.entries.erase(right.entries.begin());
            writeLeaf(pageId, leaf);
            writeLeaf(rightPageId, right);
            rebuildInternalKeys(parent, metadata);
            writeInternal(parentFrame.pageId, parent);
            refreshAncestorSeparators(path, metadata);
            return;
        }
    }

    if (parentFrame.childIndex > 0) {
        const auto leftPageId = parent.children[parentFrame.childIndex - 1];
        auto left = readLeaf(leftPageId, metadata.leafMaxKeys);
        left.entries.insert(left.entries.end(), leaf.entries.begin(), leaf.entries.end());
        left.nextPageId = leaf.nextPageId;
        if (leaf.nextPageId != INVALID_PAGE_ID) {
            auto next = readLeaf(leaf.nextPageId, metadata.leafMaxKeys);
            if (next.previousPageId != pageId) {
                throw std::runtime_error("Persistent B+ tree leaf sibling links disagree.");
            }
            next.previousPageId = leftPageId;
            writeLeaf(leaf.nextPageId, next);
        }
        writeLeaf(leftPageId, left);
        parent.children.erase(
            parent.children.begin() + static_cast<std::ptrdiff_t>(parentFrame.childIndex));
        allocator_.releasePage(pageId);
        repairInternalAfterChildRemoval(
            parentFrame.pageId, std::move(parent), std::move(path), metadata);
        return;
    }

    if (parent.children.size() < 2) {
        throw std::logic_error("Persistent B+ tree leaf underflow has no sibling.");
    }
    const auto rightPageId = parent.children[1];
    auto right = readLeaf(rightPageId, metadata.leafMaxKeys);
    leaf.entries.insert(leaf.entries.end(), right.entries.begin(), right.entries.end());
    leaf.nextPageId = right.nextPageId;
    if (right.nextPageId != INVALID_PAGE_ID) {
        auto next = readLeaf(right.nextPageId, metadata.leafMaxKeys);
        if (next.previousPageId != rightPageId) {
            throw std::runtime_error("Persistent B+ tree leaf sibling links disagree.");
        }
        next.previousPageId = pageId;
        writeLeaf(right.nextPageId, next);
    }
    writeLeaf(pageId, leaf);
    parent.children.erase(parent.children.begin() + 1);
    allocator_.releasePage(rightPageId);
    repairInternalAfterChildRemoval(
        parentFrame.pageId, std::move(parent), std::move(path), metadata);
}

bool PersistentBPlusTree::erase(IndexKey key) {
    auto metadata = readMetadata();
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return false;
    }

    std::vector<PathFrame> path;
    const auto leafPageId = findLeafPage(key, metadata, &path);
    auto leaf = readLeaf(leafPageId, metadata.leafMaxKeys);
    const auto position = std::lower_bound(
        leaf.entries.begin(),
        leaf.entries.end(),
        key,
        [](const IndexEntry& entry, IndexKey candidate) { return entry.key < candidate; });
    if (position == leaf.entries.end() || position->key != key) {
        return false;
    }
    leaf.entries.erase(position);

    if (leafPageId == metadata.rootPageId) {
        if (leaf.entries.empty()) {
            metadata.rootPageId = INVALID_PAGE_ID;
            allocator_.releasePage(leafPageId);
        } else {
            writeLeaf(leafPageId, leaf);
        }
    } else if (leaf.entries.size() >= minimumLeafKeys(metadata)) {
        writeLeaf(leafPageId, leaf);
        refreshAncestorSeparators(path, metadata);
    } else {
        rebalanceLeafAfterErase(
            leafPageId, std::move(leaf), std::move(path), metadata);
    }

    if (metadata.entryCount == 0) {
        throw std::logic_error("Persistent B+ tree metadata size underflowed.");
    }
    --metadata.entryCount;
    writeMetadata(metadata);
    return true;
}

std::optional<RecordId> PersistentBPlusTree::find(IndexKey key) const {
    const auto metadata = readMetadata();
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return std::nullopt;
    }
    const auto leafPageId = findLeafPage(key, metadata, nullptr);
    const auto leaf = readLeaf(leafPageId, metadata.leafMaxKeys);
    const auto position = std::lower_bound(
        leaf.entries.begin(),
        leaf.entries.end(),
        key,
        [](const IndexEntry& entry, IndexKey candidate) { return entry.key < candidate; });
    if (position == leaf.entries.end() || position->key != key) {
        return std::nullopt;
    }
    return position->recordId;
}

std::vector<IndexEntry> PersistentBPlusTree::rangeScan(
    IndexKey lowerInclusive,
    IndexKey upperInclusive) const {
    std::vector<IndexEntry> entries;
    const auto metadata = readMetadata();
    if (metadata.rootPageId == INVALID_PAGE_ID || lowerInclusive > upperInclusive) {
        return entries;
    }

    PageId current = findLeafPage(lowerInclusive, metadata, nullptr);
    PageId expectedPrevious = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    bool firstLeaf = true;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree leaf chain contains a cycle.");
        }
        const auto leaf = readLeaf(current, metadata.leafMaxKeys);
        if (!firstLeaf && leaf.previousPageId != expectedPrevious) {
            throw std::runtime_error("Persistent B+ tree leaf backward link is inconsistent.");
        }
        const auto position = firstLeaf
            ? std::lower_bound(
                  leaf.entries.begin(),
                  leaf.entries.end(),
                  lowerInclusive,
                  [](const IndexEntry& entry, IndexKey candidate) {
                      return entry.key < candidate;
                  })
            : leaf.entries.begin();
        for (auto entry = position; entry != leaf.entries.end(); ++entry) {
            if (entry->key > upperInclusive) {
                return entries;
            }
            entries.push_back(*entry);
        }
        firstLeaf = false;
        expectedPrevious = current;
        current = leaf.nextPageId;
    }
    return entries;
}

std::vector<IndexEntry> PersistentBPlusTree::scanAll() const {
    const auto metadata = readMetadata();
    std::vector<IndexEntry> entries;
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return entries;
    }
    if (metadata.entryCount > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("Persistent B+ tree is too large to materialize a scan.");
    }
    entries.reserve(static_cast<std::size_t>(metadata.entryCount));

    PageId current = findLeafPage(std::numeric_limits<IndexKey>::min(), metadata, nullptr);
    PageId expectedPrevious = INVALID_PAGE_ID;
    std::unordered_set<PageId> visited;
    while (current != INVALID_PAGE_ID) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree leaf chain contains a cycle.");
        }
        const auto leaf = readLeaf(current, metadata.leafMaxKeys);
        if (leaf.previousPageId != expectedPrevious) {
            throw std::runtime_error("Persistent B+ tree leaf backward link is inconsistent.");
        }
        entries.insert(entries.end(), leaf.entries.begin(), leaf.entries.end());
        expectedPrevious = current;
        current = leaf.nextPageId;
    }
    if (entries.size() != metadata.entryCount) {
        throw std::runtime_error("Persistent B+ tree scan count disagrees with metadata size.");
    }
    return entries;
}

std::vector<PageId> PersistentBPlusTree::reachableNodePageIds() const {
    const auto metadata = readMetadata();
    std::vector<PageId> result;
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return result;
    }

    std::vector<PageId> pending{metadata.rootPageId};
    std::unordered_set<PageId> visited;
    while (!pending.empty()) {
        const auto pageId = pending.back();
        pending.pop_back();
        if (!visited.insert(pageId).second) {
            throw std::runtime_error(
                "Persistent B+ tree graph contains a cycle or duplicate child page.");
        }
        result.push_back(pageId);
        if (!isLeafPage(pageId)) {
            const auto internalNode = readInternal(pageId, metadata.internalMaxKeys);
            pending.insert(
                pending.end(), internalNode.children.rbegin(), internalNode.children.rend());
        } else {
            static_cast<void>(readLeaf(pageId, metadata.leafMaxKeys));
        }
    }
    return result;
}

void PersistentBPlusTree::validate() const {
    const auto metadata = readMetadata();
    const auto freePages = allocator_.freePageIds();
    const std::unordered_set<PageId> freePageSet(freePages.begin(), freePages.end());
    if (freePageSet.contains(metadataPageId_)) {
        throw std::runtime_error("Persistent B+ tree metadata page is also marked free.");
    }
    if (metadata.rootPageId == INVALID_PAGE_ID) {
        return;
    }

    struct Bounds {
        IndexKey minimum;
        IndexKey maximum;
        std::uint64_t entries;
    };

    std::unordered_set<PageId> visited;
    std::vector<PageId> leaves;
    std::optional<std::size_t> leafDepth;
    std::function<Bounds(PageId, std::size_t, bool)> validateNode;
    validateNode = [&](PageId pageId, std::size_t depth, bool isRoot) -> Bounds {
        if (freePageSet.contains(pageId)) {
            throw std::runtime_error("Persistent B+ tree node page is also marked free.");
        }
        if (!visited.insert(pageId).second) {
            throw std::runtime_error(
                "Persistent B+ tree graph contains a cycle or duplicate child page.");
        }
        if (isLeafPage(pageId)) {
            const auto leaf = readLeaf(pageId, metadata.leafMaxKeys);
            const auto minimumKeys =
                (static_cast<std::size_t>(metadata.leafMaxKeys) + 1) / 2;
            if (!isRoot && leaf.entries.size() < minimumKeys) {
                throw std::runtime_error(
                    "Persistent B+ tree non-root leaf is below minimum occupancy.");
            }
            if (!leafDepth) {
                leafDepth = depth;
            } else if (*leafDepth != depth) {
                throw std::runtime_error("Persistent B+ tree leaves are at different depths.");
            }
            leaves.push_back(pageId);
            return Bounds{
                leaf.entries.front().key,
                leaf.entries.back().key,
                static_cast<std::uint64_t>(leaf.entries.size()),
            };
        }

        const auto internalNode = readInternal(pageId, metadata.internalMaxKeys);
        const auto minimumKeys = static_cast<std::size_t>(metadata.internalMaxKeys) / 2;
        if (!isRoot && internalNode.keys.size() < minimumKeys) {
            throw std::runtime_error(
                "Persistent B+ tree non-root internal page is below minimum occupancy.");
        }

        std::vector<Bounds> childBounds;
        childBounds.reserve(internalNode.children.size());
        for (const auto child : internalNode.children) {
            childBounds.push_back(validateNode(child, depth + 1, false));
        }
        std::uint64_t entryCount = childBounds.front().entries;
        for (std::size_t index = 1; index < childBounds.size(); ++index) {
            if (childBounds[index - 1].maximum >= childBounds[index].minimum) {
                throw std::runtime_error("Persistent B+ tree child key ranges overlap.");
            }
            if (internalNode.keys[index - 1] != childBounds[index].minimum) {
                throw std::runtime_error(
                    "Persistent B+ tree separator is not the right-subtree minimum.");
            }
            if (entryCount > std::numeric_limits<std::uint64_t>::max()
                    - childBounds[index].entries) {
                throw std::runtime_error("Persistent B+ tree entry count overflowed.");
            }
            entryCount += childBounds[index].entries;
        }
        return Bounds{
            childBounds.front().minimum,
            childBounds.back().maximum,
            entryCount,
        };
    };

    const auto bounds = validateNode(metadata.rootPageId, 0, true);
    if (bounds.entries != metadata.entryCount) {
        throw std::runtime_error("Persistent B+ tree entry count disagrees with metadata size.");
    }

    PageId current = leaves.front();
    PageId previous = INVALID_PAGE_ID;
    std::unordered_set<PageId> forwardVisited;
    for (const auto expected : leaves) {
        if (current != expected || !forwardVisited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree forward leaf chain disagrees with tree order.");
        }
        const auto leaf = readLeaf(current, metadata.leafMaxKeys);
        if (leaf.previousPageId != previous) {
            throw std::runtime_error("Persistent B+ tree backward leaf link is incorrect.");
        }
        previous = current;
        current = leaf.nextPageId;
    }
    if (current != INVALID_PAGE_ID || forwardVisited.size() != leaves.size()) {
        throw std::runtime_error("Persistent B+ tree forward leaf chain has extra pages or a cycle.");
    }

    current = leaves.back();
    PageId next = INVALID_PAGE_ID;
    std::unordered_set<PageId> backwardVisited;
    for (auto position = leaves.rbegin(); position != leaves.rend(); ++position) {
        if (current != *position || !backwardVisited.insert(current).second) {
            throw std::runtime_error("Persistent B+ tree backward leaf chain disagrees with tree order.");
        }
        const auto leaf = readLeaf(current, metadata.leafMaxKeys);
        if (leaf.nextPageId != next) {
            throw std::runtime_error("Persistent B+ tree forward leaf link is incorrect.");
        }
        next = current;
        current = leaf.previousPageId;
    }
    if (current != INVALID_PAGE_ID || backwardVisited.size() != leaves.size()) {
        throw std::runtime_error("Persistent B+ tree backward leaf chain has extra pages or a cycle.");
    }
}

} // namespace minidb
