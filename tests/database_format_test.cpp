#include "minidb/database_format.hpp"
#include "minidb/pager.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view testName)
        : path_(std::filesystem::temp_directory_path()
                / ("minidb_cpp_" + std::string(testName) + "_"
                   + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count())
                   + ".db")) {}

    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void requireRuntimeErrorContaining(
    Function&& function,
    std::string_view expectedText,
    std::string_view failureMessage) {
    try {
        function();
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find(expectedText) != std::string_view::npos,
                "Error message did not describe the expected validation failure");
        return;
    }
    throw std::runtime_error(std::string(failureMessage));
}

template <typename Function>
void requireInvalidArgument(Function&& function, std::string_view failureMessage) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(failureMessage));
}

minidb::Pager::Page readMetadataPage(const std::filesystem::path& path) {
    minidb::Pager::Page page{};
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not open test database for reading");
    input.read(
        reinterpret_cast<char*>(page.data()),
        static_cast<std::streamsize>(page.size()));
    require(input.gcount() == static_cast<std::streamsize>(page.size()),
            "Could not read the complete metadata page");
    return page;
}

std::uint32_t readUint32LittleEndian(
    const minidb::Pager::Page& page,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(page[offset + index]) << (index * 8U);
    }
    return value;
}

void writeByte(const std::filesystem::path& path, std::size_t offset, std::byte value) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    require(static_cast<bool>(file), "Could not open test database for mutation");
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    const auto byte = std::to_integer<unsigned char>(value);
    file.write(reinterpret_cast<const char*>(&byte), 1);
    file.flush();
    require(static_cast<bool>(file), "Could not mutate test database byte");
}

void writeUint32LittleEndian(
    const std::filesystem::path& path,
    std::size_t offset,
    std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        writeByte(
            path,
            offset + index,
            static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void createValidDatabase(const std::filesystem::path& path) {
    minidb::Pager pager(path.string());
    require(pager.pageCount() == 1, "New database did not contain exactly one metadata page");
}

void testNewDatabaseMetadataLayout() {
    TemporaryDatabase database("new_metadata");
    {
        minidb::Pager pager(database.path().string());
        require(pager.pageCount() == 1, "New database page count was not one");
        require(pager.databaseHeader() == minidb::database_format::makeCurrentDatabaseHeader(),
                "New database exposed an unexpected header");
        require(pager.allocatePage() == 1, "First normal page ID was not one");

        requireInvalidArgument(
            [&] { static_cast<void>(pager.getPage(minidb::database_format::METADATA_PAGE_ID)); },
            "Metadata page was available as a normal page");
        requireInvalidArgument(
            [&] { pager.markDirty(minidb::database_format::METADATA_PAGE_ID); },
            "Metadata page could be marked dirty through the normal page API");
        requireInvalidArgument(
            [&] { pager.flush(minidb::database_format::METADATA_PAGE_ID); },
            "Metadata page could be flushed through the normal page API");
    }

    require(std::filesystem::file_size(database.path()) == 2 * minidb::Pager::PAGE_SIZE,
            "Database file did not contain metadata plus one allocated page");

    const auto metadata = readMetadataPage(database.path());
    require(std::equal(
                minidb::database_format::MAGIC.begin(),
                minidb::database_format::MAGIC.end(),
                metadata.begin() + minidb::database_format::MAGIC_OFFSET),
            "Metadata page did not contain the MiniDB++ magic");
    require(readUint32LittleEndian(metadata, minidb::database_format::FORMAT_VERSION_OFFSET)
                == minidb::database_format::CURRENT_VERSION,
            "Metadata page did not contain format version one");
    require(readUint32LittleEndian(metadata, minidb::database_format::PAGE_SIZE_OFFSET)
                == minidb::Pager::PAGE_SIZE,
            "Metadata page did not contain the compiled page size");
    require(readUint32LittleEndian(metadata, minidb::database_format::HEADER_SIZE_OFFSET)
                == minidb::database_format::HEADER_SIZE,
            "Metadata page did not contain the expected header size");
    require(readUint32LittleEndian(
                metadata,
                minidb::database_format::CATALOG_ROOT_PAGE_ID_OFFSET)
                == minidb::INVALID_PAGE_ID,
            "Catalog root placeholder was not initialized to INVALID_PAGE_ID");
    require(readUint32LittleEndian(
                metadata,
                minidb::database_format::FREE_LIST_ROOT_PAGE_ID_OFFSET)
                == minidb::INVALID_PAGE_ID,
            "Free-list root placeholder was not initialized to INVALID_PAGE_ID");
    require(std::all_of(
                metadata.begin() + minidb::database_format::RESERVED_OFFSET,
                metadata.begin() + minidb::database_format::HEADER_SIZE,
                [](std::byte value) { return value == std::byte{0}; }),
            "Reserved header bytes were not zero initialized");
}

void testExistingEmptyFileIsInitialized() {
    TemporaryDatabase database("empty_file");
    {
        std::ofstream empty(database.path(), std::ios::binary);
        require(static_cast<bool>(empty), "Could not create empty test database");
    }

    createValidDatabase(database.path());
    require(std::filesystem::file_size(database.path()) == minidb::Pager::PAGE_SIZE,
            "Existing empty file was not initialized with one metadata page");
}

void testMetadataAndNormalPagePersist() {
    TemporaryDatabase database("persistence");
    minidb::Pager::Page originalMetadata{};
    {
        minidb::Pager pager(database.path().string());
        originalMetadata = readMetadataPage(database.path());
        const auto pageId = pager.allocatePage();
        auto& page = pager.getPage(pageId);
        page.front() = std::byte{0x2A};
        page.back() = std::byte{0x7F};
        pager.markDirty(pageId);
        pager.flushAll();
    }

    {
        minidb::Pager pager(database.path().string());
        require(pager.pageCount() == 2, "Allocated page count did not persist");
        require(pager.databaseHeader() == minidb::database_format::makeCurrentDatabaseHeader(),
                "Database header did not persist across reopen");
        const auto& page = pager.getPage(1);
        require(page.front() == std::byte{0x2A}, "First data byte did not persist");
        require(page.back() == std::byte{0x7F}, "Last data byte did not persist");
    }

    require(readMetadataPage(database.path()) == originalMetadata,
            "Normal page writes modified the metadata page");
}

void testInvalidMagicIsRejected() {
    TemporaryDatabase database("invalid_magic");
    createValidDatabase(database.path());
    writeByte(database.path(), minidb::database_format::MAGIC_OFFSET, std::byte{'X'});

    requireRuntimeErrorContaining(
        [&] { minidb::Pager pager(database.path().string()); },
        "magic",
        "Database with invalid magic was accepted");
}

void testUnsupportedVersionIsRejected() {
    TemporaryDatabase database("unsupported_version");
    createValidDatabase(database.path());
    writeUint32LittleEndian(
        database.path(),
        minidb::database_format::FORMAT_VERSION_OFFSET,
        minidb::database_format::CURRENT_VERSION + 1);

    requireRuntimeErrorContaining(
        [&] { minidb::Pager pager(database.path().string()); },
        "Unsupported database format version",
        "Database with unsupported format version was accepted");
}

void testInvalidStoredPageSizeIsRejected() {
    TemporaryDatabase database("invalid_page_size");
    createValidDatabase(database.path());
    writeUint32LittleEndian(
        database.path(),
        minidb::database_format::PAGE_SIZE_OFFSET,
        static_cast<std::uint32_t>(minidb::Pager::PAGE_SIZE * 2));

    requireRuntimeErrorContaining(
        [&] { minidb::Pager pager(database.path().string()); },
        "does not match the compiled page size",
        "Database with invalid stored page size was accepted");
}

void testInvalidHeaderSizeIsRejected() {
    TemporaryDatabase database("invalid_header_size");
    createValidDatabase(database.path());
    writeUint32LittleEndian(
        database.path(),
        minidb::database_format::HEADER_SIZE_OFFSET,
        static_cast<std::uint32_t>(minidb::database_format::HEADER_SIZE + 1));

    requireRuntimeErrorContaining(
        [&] { minidb::Pager pager(database.path().string()); },
        "does not match the supported header size",
        "Database with invalid header size was accepted");
}

void testMisalignedFileSizeIsRejected() {
    TemporaryDatabase database("misaligned_size");
    {
        std::ofstream file(database.path(), std::ios::binary);
        require(static_cast<bool>(file), "Could not create malformed test database");
        file.put('\0');
    }

    requireRuntimeErrorContaining(
        [&] { minidb::Pager pager(database.path().string()); },
        "not a multiple of PAGE_SIZE",
        "Database with misaligned file size was accepted");
}

} // namespace

int main() {
    try {
        testNewDatabaseMetadataLayout();
        testExistingEmptyFileIsInitialized();
        testMetadataAndNormalPagePersist();
        testInvalidMagicIsRejected();
        testUnsupportedVersionIsRejected();
        testInvalidStoredPageSizeIsRejected();
        testInvalidHeaderSizeIsRejected();
        testMisalignedFileSizeIsRejected();
        std::cout << "database_format_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "database_format_test failed: " << error.what() << '\n';
        return 1;
    }
}
