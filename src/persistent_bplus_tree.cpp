#include "minidb/persistent_bplus_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace minidb {
namespace {

constexpr std::size_t BITS_PER_BYTE = 8;

void writeUint32(Pager::Page& output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
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

PersistentBPlusTree PersistentBPlusTree::create(
    Pager& pager,
    std::uint32_t leafMaxKeys,
    std::uint32_t internalMaxKeys) {
    validateLogicalCapacities(leafMaxKeys, internalMaxKeys);
    const auto metadataPageId = pager.allocatePage();
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
    static_cast<void>(tree.readMetadata());
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

} // namespace minidb
