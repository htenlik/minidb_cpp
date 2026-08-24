#include "minidb/database_server.hpp"

namespace minidb::net {

DatabaseServer::DatabaseServer(std::string databasePath, ServerConfig config)
    : diskManager_(databasePath),
      bufferPool_(diskManager_, config.bufferFrames, config.lruK),
      allocator_(bufferPool_, diskManager_),
      catalog_(Catalog::openOrCreate(bufferPool_, diskManager_, allocator_)),
      engine_(catalog_),
      server_(std::move(config), engine_, bufferPool_, diskManager_) {}

} // namespace minidb::net
