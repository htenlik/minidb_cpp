#include "minidb/cli_format.hpp"
#include "minidb/minidb_client.hpp"
#include "minidb/tcp_server.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::uint16_t parsePort(std::string_view text) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()
        || value == 0 || value > 65535U) {
        throw std::invalid_argument("invalid --port value");
    }
    return static_cast<std::uint16_t>(value);
}

void usage() {
    std::cerr << "usage: minidb_client [--host ADDRESS] [--port PORT] "
                 "[--execute SQL] [--stats]\n";
}

void printRemoteError(const minidb::net::RemoteSqlError& error) {
    std::cerr << "error [request " << error.requestId() << "]: " << error.message();
    if (error.span().has_value()) {
        std::cerr << " at " << error.span()->begin.line << ':'
                  << error.span()->begin.column;
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        std::uint16_t port = minidb::net::DEFAULT_SERVER_PORT;
        std::string execute;
        bool stats = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--host" && index + 1 < argc) {
                host = argv[++index];
            } else if (argument == "--port" && index + 1 < argc) {
                port = parsePort(argv[++index]);
            } else if (argument == "--execute" && index + 1 < argc) {
                execute = argv[++index];
            } else if (argument == "--stats") {
                stats = true;
            } else {
                usage();
                return 2;
            }
        }

        minidb::net::MiniDbClient client(host, port);
        client.connect();
        client.handshake();
        if (!execute.empty()) {
            try {
                std::cout << minidb::cli::formatQueryResult(client.execute(execute), stats);
                return 0;
            } catch (const minidb::net::RemoteSqlError& error) {
                printRemoteError(error);
                return 1;
            }
        }

        std::string line;
        while (std::cout << "minidb> " << std::flush, std::getline(std::cin, line)) {
            if (line.empty()) continue;
            try {
                std::cout << minidb::cli::formatQueryResult(client.execute(line), stats);
            } catch (const minidb::net::RemoteSqlError& error) {
                printRemoteError(error);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_client: " << error.what() << '\n';
        return 1;
    }
}
