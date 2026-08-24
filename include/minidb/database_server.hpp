#pragma once

#include "minidb/catalog.hpp"
#include "minidb/page_allocator.hpp"
#include "minidb/pager.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/tcp_server.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace minidb::net {

// Declaration order makes the ownership lifetime explicit: the socket server and
// SQL engine are destroyed before Catalog, which is destroyed before Pager.
class DatabaseServer {
public:
    DatabaseServer(std::string databasePath, ServerConfig config);

    void start() { server_.start(); }
    void serve(std::size_t connectionLimit = 0) { server_.serve(connectionLimit); }
    void close() noexcept { server_.close(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return server_.port(); }
    [[nodiscard]] TcpServer& tcpServer() noexcept { return server_; }
    [[nodiscard]] Pager& pager() noexcept { return pager_; }
    [[nodiscard]] PageAllocator& pageAllocator() noexcept { return allocator_; }
    [[nodiscard]] Catalog& catalog() noexcept { return catalog_; }

private:
    Pager pager_;
    PageAllocator allocator_;
    Catalog catalog_;
    sql::SqlEngine engine_;
    TcpServer server_;
};

} // namespace minidb::net
