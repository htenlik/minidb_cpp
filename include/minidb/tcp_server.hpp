#pragma once

#include "minidb/pager.hpp"
#include "minidb/sql_executor.hpp"
#include "minidb/tcp_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace minidb::net {

inline constexpr std::uint16_t DEFAULT_SERVER_PORT = 7432;

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = DEFAULT_SERVER_PORT;
    int backlog = 16;
};

class TcpServer {
public:
    TcpServer(ServerConfig config, sql::SqlEngine& engine, Pager& pager);

    void start();
    // A zero connection limit serves indefinitely. Tests use a finite limit.
    void serve(std::size_t connectionLimit = 0);
    void serveConnection(int connectedDescriptor);
    void close() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return boundPort_; }
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }

private:
    ServerConfig config_;
    sql::SqlEngine& engine_;
    Pager& pager_;
    Socket listener_;
    std::uint16_t boundPort_ = 0;

    void sendProtocolFailure(
        int descriptor,
        std::uint64_t requestId,
        std::string message) const noexcept;
};

} // namespace minidb::net
