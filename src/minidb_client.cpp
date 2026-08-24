#include "minidb/minidb_client.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace minidb::net {
namespace {

Socket connectSocket(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* rawAddresses = nullptr;
    const auto service = std::to_string(port);
    const auto lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &rawAddresses);
    if (lookup != 0) {
        throw NetworkError(std::string("getaddrinfo: ") + ::gai_strerror(lookup));
    }
    struct AddressGuard {
        addrinfo* value;
        ~AddressGuard() { ::freeaddrinfo(value); }
    } guard{rawAddresses};

    std::string lastError = "no connection candidate";
    for (auto* address = rawAddresses; address != nullptr; address = address->ai_next) {
        Socket socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!socket) {
            lastError = std::strerror(errno);
            continue;
        }
        try {
            configureSocketForSafeWrites(socket.get());
        } catch (const NetworkError& error) {
            lastError = error.what();
            continue;
        }
        if (::connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
            return socket;
        }
        lastError = std::strerror(errno);
    }
    throw NetworkError("cannot connect to " + host + ':' + service + ": " + lastError);
}

} // namespace

MiniDbClient::MiniDbClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {
    if (host_.empty() || port_ == 0) {
        throw std::invalid_argument("client host must be nonempty and port must be nonzero");
    }
}

void MiniDbClient::connect() {
    if (socket_) {
        throw std::logic_error("client is already connected");
    }
    socket_ = connectSocket(host_, port_);
    handshaken_ = false;
}

void MiniDbClient::handshake() {
    if (!socket_) {
        throw std::logic_error("client is not connected");
    }
    writeFrame(socket_.get(), makeHelloFrame());
    const auto response = readFrame(socket_.get());
    if (!response.has_value()) {
        throw NetworkError("server disconnected during handshake");
    }
    if (response->header.messageType == MessageType::ErrorResponse) {
        throw RemoteSqlError(
            response->header.requestId, decodeErrorResponsePayload(response->payload));
    }
    if (response->header.messageType != MessageType::HelloAck
        || response->header.requestId != 0 || !response->payload.empty()) {
        throw ProtocolError("server returned an invalid HELLO_ACK");
    }
    handshaken_ = true;
}

void MiniDbClient::close() noexcept {
    socket_.reset();
    handshaken_ = false;
}

sql::QueryResult MiniDbClient::execute(std::string_view source) {
    const auto requestId = nextRequestId_++;
    return execute(requestId, source);
}

sql::QueryResult MiniDbClient::execute(std::uint64_t requestId, std::string_view source) {
    if (!socket_ || !handshaken_) {
        throw std::logic_error("client must connect and handshake before executing SQL");
    }
    writeFrame(socket_.get(), makeExecuteSqlFrame(requestId, source));
    const auto response = readFrame(socket_.get());
    if (!response.has_value()) {
        throw NetworkError("server disconnected before sending a SQL response");
    }
    if (response->header.requestId != requestId) {
        throw ProtocolError("server response request ID does not match request");
    }
    if (response->header.messageType == MessageType::ErrorResponse) {
        throw RemoteSqlError(requestId, decodeErrorResponsePayload(response->payload));
    }
    return decodeQueryResultFrame(*response);
}

} // namespace minidb::net
