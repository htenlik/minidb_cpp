#include "minidb/database_server.hpp"

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
    if (error != std::errc{} || end != text.data() + text.size() || value > 65535U) {
        throw std::invalid_argument("invalid --port value");
    }
    return static_cast<std::uint16_t>(value);
}

void usage() {
    std::cerr << "usage: minidb_server DATABASE [--host ADDRESS] [--port PORT]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 2;
        }
        std::string databasePath = argv[1];
        minidb::net::ServerConfig config;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--host" && index + 1 < argc) {
                config.host = argv[++index];
            } else if (argument == "--port" && index + 1 < argc) {
                config.port = parsePort(argv[++index]);
            } else {
                usage();
                return 2;
            }
        }

        minidb::net::DatabaseServer server(databasePath, config);
        server.start();
        std::cout << "MiniDB++ database: " << databasePath << '\n'
                  << "listening: " << config.host << ':' << server.port() << '\n'
                  << "protocol: " << minidb::net::PROTOCOL_VERSION << std::endl;
        server.serve();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_server: " << error.what() << '\n';
        return 1;
    }
}
