#include "minidb/database_server.hpp"

namespace minidb::net {

DatabaseServer::DatabaseServer(std::string databasePath, ServerConfig config)
    : pager_(databasePath),
      allocator_(pager_),
      catalog_(Catalog::openOrCreate(pager_)),
      engine_(catalog_),
      server_(std::move(config), engine_, pager_) {}

} // namespace minidb::net
