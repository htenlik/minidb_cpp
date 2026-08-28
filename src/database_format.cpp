#include "minidb/database_format.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace minidb::database_format {
namespace {

void writeUint32LittleEndian(
    std::span<std::byte> output,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0; index < FORMAT_VERSION_SIZE; ++index) {
        output[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint32_t readUint32LittleEndian(
    std::span<const std::byte> input,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < FORMAT_VERSION_SIZE; ++index) {
        value |= std::to_integer<std::uint32_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t readUint64LittleEndian(
    std::span<const std::byte> input,
    std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < PAGE_LSN_SIZE; ++index) {
        value |= std::to_integer<std::uint64_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

} // namespace

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
    writeUint32LittleEndian(metadataPage, FORMAT_VERSION_OFFSET, header.formatVersion);
    writeUint32LittleEndian(metadataPage, PAGE_SIZE_OFFSET, header.pageSize);
    writeUint32LittleEndian(metadataPage, HEADER_SIZE_OFFSET, header.headerSize);
    writeUint32LittleEndian(
        metadataPage,
        CATALOG_ROOT_PAGE_ID_OFFSET,
        header.catalogRootPageId);
    writeUint32LittleEndian(
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
        readUint32LittleEndian(metadataPage, FORMAT_VERSION_OFFSET),
        readUint32LittleEndian(metadataPage, PAGE_SIZE_OFFSET),
        readUint32LittleEndian(metadataPage, HEADER_SIZE_OFFSET),
        readUint32LittleEndian(metadataPage, CATALOG_ROOT_PAGE_ID_OFFSET),
        readUint32LittleEndian(metadataPage, FREE_LIST_ROOT_PAGE_ID_OFFSET),
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
    const auto encodedPageLsn = readUint64LittleEndian(metadataPage, PAGE_LSN_OFFSET);
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
