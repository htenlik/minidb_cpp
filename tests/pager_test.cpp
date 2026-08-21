#include "minidb/pager.hpp"
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
int main() {
    const std::string path = "pager_test.db";
    std::filesystem::remove(path);
    {
        minidb::Pager pager(path);
        assert(pager.pageCount() == 0);
        const auto pageId = pager.allocatePage();
        assert(pageId == 0);
        assert(pager.pageCount() == 1);
        auto& page = pager.getPage(pageId);
        page[0] = std::byte{0x2A};
        page[4095] = std::byte{0x7F};
        pager.markDirty(pageId);
        pager.flushAll();
    }
    {
        minidb::Pager pager(path);
        assert(pager.pageCount() == 1);
        auto& page = pager.getPage(0);
        assert(page[0] == std::byte{0x2A});
        assert(page[4095] == std::byte{0x7F});
    }
    std::filesystem::remove(path);
    std::cout << "pager_test passed\n";
    return 0;
}
