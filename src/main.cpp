#include "minidb/pager.hpp"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    const std::string dbPath = (argc >= 2) ? argv[1] : "minidb.db";
    try {
        minidb::Pager pager(dbPath);
        std::cout << "MiniDB++\nDatabase: " << dbPath << "\nPage size: " << minidb::Pager::PAGE_SIZE << " bytes\nType .help for commands.\n\n";
        std::string line;
        while (true) {
            std::cout << "minidb> ";
            if (!std::getline(std::cin, line)) break;
            if (line == ".quit" || line == ".exit") break;
            if (line == ".help") { std::cout << ".pages        show allocated page count\n.alloc        allocate one zero-filled page\n.flush        flush dirty pages to disk\n.quit         exit\n"; continue; }
            if (line == ".pages") { std::cout << pager.pageCount() << " page(s)\n"; continue; }
            if (line == ".alloc") { const auto id = pager.allocatePage(); std::cout << "allocated page " << id << "\n"; continue; }
            if (line == ".flush") { pager.flushAll(); std::cout << "flushed\n"; continue; }
            if (line.empty()) continue;
            std::cout << "Unknown command. SQL parser comes in a later milestone.\n";
        }
        pager.flushAll();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
}
