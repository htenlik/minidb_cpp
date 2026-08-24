#pragma once

#include "minidb/sql_executor.hpp"
#include "minidb/tcp_transport.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace minidb::net {

class MiniDbClient {
public:
    MiniDbClient(std::string host = "127.0.0.1", std::uint16_t port = 7432);

    void connect();
    void handshake();
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(socket_); }

    [[nodiscard]] sql::QueryResult execute(std::string_view sql);
    [[nodiscard]] sql::QueryResult execute(std::uint64_t requestId, std::string_view sql);

private:
    std::string host_;
    std::uint16_t port_;
    Socket socket_;
    bool handshaken_ = false;
    std::uint64_t nextRequestId_ = 1;
};

} // namespace minidb::net
