#include "minidb/disk_manager.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

using minidb::DiskManager;
using minidb::test::require;

void testCreateAppendReadWriteAndReopen() {
    minidb::test::TemporaryDatabase database("disk_manager_round_trip");
    minidb::PageId pageId = minidb::INVALID_PAGE_ID;
    {
        DiskManager disk(database.path().string());
        require(disk.pageCount() == 1, "new DiskManager did not reserve metadata page 0");
        pageId = disk.appendPage();
        require(pageId == 1 && disk.pageCount() == 2,
                "first physical append did not create data page 1");
        DiskManager::Page page{};
        page[0] = std::byte{0xA5};
        page.back() = std::byte{0x5A};
        disk.writePage(pageId, page);
        DiskManager::Page read{};
        disk.readPage(pageId, read);
        require(read == page, "DiskManager page round trip changed bytes");
        disk.updateCatalogRootPageId(pageId);
        disk.updateFreeListRootPageId(pageId);
        disk.flush();
    }
    {
        DiskManager disk(database.path().string());
        require(disk.pageCount() == 2, "DiskManager page count did not survive reopen");
        require(disk.databaseHeader().catalogRootPageId == pageId
                    && disk.databaseHeader().freeListRootPageId == pageId,
                "DiskManager metadata updates did not survive reopen");
        DiskManager::Page read{};
        disk.readPage(pageId, read);
        require(read[0] == std::byte{0xA5} && read.back() == std::byte{0x5A},
                "DiskManager data did not survive reopen");
    }
}

void testProtectionAndInvalidIds() {
    minidb::test::TemporaryDatabase database("disk_manager_invalid_ids");
    DiskManager disk(database.path().string());
    DiskManager::Page page{};
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { disk.readPage(0, page); }, "DiskManager exposed metadata page for reading");
    minidb::test::requireThrows<std::invalid_argument>(
        [&] { disk.writePage(0, page); }, "DiskManager exposed metadata page for writing");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { disk.readPage(1, page); }, "DiskManager accepted nonexistent page");
    minidb::test::requireThrows<std::out_of_range>(
        [&] { disk.writePage(minidb::INVALID_PAGE_ID, page); },
        "DiskManager accepted invalid page ID");
}

void testCorruptFileSizeRejected() {
    const auto path = std::filesystem::temp_directory_path() / "minidb_disk_short.db";
    std::filesystem::remove(path);
    {
        std::ofstream output(path, std::ios::binary);
        output.put('x');
    }
    minidb::test::requireThrows<std::runtime_error>(
        [&] { DiskManager disk(path.string()); }, "short database file was accepted");
    std::filesystem::remove(path);
}

void testCorruptMagicRejected() {
    minidb::test::TemporaryDatabase database("disk_manager_bad_magic");
    {
        DiskManager disk(database.path().string());
    }
    {
        std::fstream file(database.path(), std::ios::in | std::ios::out | std::ios::binary);
        const char corrupt = 'X';
        file.seekp(0, std::ios::beg);
        file.write(&corrupt, 1);
    }
    minidb::test::requireThrows<std::runtime_error>(
        [&] { DiskManager disk(database.path().string()); },
        "DiskManager accepted corrupt database magic");
}

} // namespace

int main() {
    try {
        testCreateAppendReadWriteAndReopen();
        testProtectionAndInvalidIds();
        testCorruptFileSizeRejected();
        testCorruptMagicRejected();
        std::cout << "DiskManager tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DiskManager test failure: " << error.what() << '\n';
        return 1;
    }
}
