#include "minidb/database_format.hpp"

#include "minidb/byte_codec.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace minidb::database_format {

DatabaseHeader makeCurrentDatabaseHeader() noexcept {
    return {};
}

void serializeDatabaseHeader(
    const DatabaseHeader& header,
    std::span<std::byte> metadataPage) {
    if (metadataPage.size() != PAGE_SIZE) {
        throw std::invalid_argument("Database metadata page has an invalid size.");
    }

    std::fill(metadataPage.begin(), metadataPage.end(), std::byte{0});
    std::copy(MAGIC.begin(), MAGIC.end(), metadataPage.begin() + MAGIC_OFFSET);
    byte_codec::writeUint32(metadataPage, FORMAT_VERSION_OFFSET, header.formatVersion);
    byte_codec::writeUint32(metadataPage, PAGE_SIZE_OFFSET, header.pageSize);
    byte_codec::writeUint32(metadataPage, HEADER_SIZE_OFFSET, header.headerSize);
    byte_codec::writeUint32(
        metadataPage,
        CATALOG_ROOT_PAGE_ID_OFFSET,
        header.catalogRootPageId);
    byte_codec::writeUint32(
        metadataPage,
        FREE_LIST_ROOT_PAGE_ID_OFFSET,
        header.freeListRootPageId);
}

DatabaseHeader deserializeDatabaseHeader(std::span<const std::byte> metadataPage) {
    if (metadataPage.size() != PAGE_SIZE) {
        throw std::runtime_error("Database metadata page has an invalid size.");
    }
    if (!std::equal(MAGIC.begin(), MAGIC.end(), metadataPage.begin() + MAGIC_OFFSET)) {
        throw std::runtime_error("Invalid database magic: file is not a MiniDB++ database.");
    }

    DatabaseHeader header{
        byte_codec::readUint32(metadataPage, FORMAT_VERSION_OFFSET),
        byte_codec::readUint32(metadataPage, PAGE_SIZE_OFFSET),
        byte_codec::readUint32(metadataPage, HEADER_SIZE_OFFSET),
        byte_codec::readUint32(metadataPage, CATALOG_ROOT_PAGE_ID_OFFSET),
        byte_codec::readUint32(metadataPage, FREE_LIST_ROOT_PAGE_ID_OFFSET),
    };

    if (header.formatVersion != LEGACY_VERSION
        && header.formatVersion != CURRENT_VERSION) {
        throw std::runtime_error(
            "Unsupported database format version " + std::to_string(header.formatVersion)
            + "; supported versions are " + std::to_string(LEGACY_VERSION)
            + " and " + std::to_string(CURRENT_VERSION) + ".");
    }
    if (header.pageSize != PAGE_SIZE) {
        throw std::runtime_error(
            "Database page size " + std::to_string(header.pageSize)
            + " does not match the compiled page size " + std::to_string(PAGE_SIZE) + ".");
    }
    if (header.headerSize != HEADER_SIZE) {
        throw std::runtime_error(
            "Database header size " + std::to_string(header.headerSize)
            + " does not match the supported header size " + std::to_string(HEADER_SIZE) + ".");
    }
    const auto encodedPageLsn = byte_codec::readUint64(metadataPage, PAGE_LSN_OFFSET);
    if ((header.formatVersion == LEGACY_VERSION && encodedPageLsn != 0)
        || encodedPageLsn == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("Database metadata page has an invalid PageLSN encoding.");
    }
    if (!std::all_of(
            metadataPage.begin() + PAGE_LSN_OFFSET + PAGE_LSN_SIZE,
            metadataPage.end(),
            [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("Database metadata page reserved bytes are not zero.");
    }

    return header;
}

} // namespace minidb::database_format
