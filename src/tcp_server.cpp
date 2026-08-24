#include "minidb/tcp_server.hpp"

#include "minidb/sql_error.hpp"
#include "minidb/sql_semantics.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace minidb::net {
namespace {

std::string systemError(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

ErrorCategory categoryFor(sql::SqlErrorKind kind) {
    return kind == sql::SqlErrorKind::Lexer ? ErrorCategory::Lexer : ErrorCategory::Parser;
}

ErrorCategory categoryFor(sql::SqlExecutionErrorKind kind) {
    switch (kind) {
    case sql::SqlExecutionErrorKind::Semantic: return ErrorCategory::Semantic;
    case sql::SqlExecutionErrorKind::Constraint: return ErrorCategory::Constraint;
    case sql::SqlExecutionErrorKind::Execution: return ErrorCategory::Execution;
    }
    return ErrorCategory::Internal;
}

Socket createListener(const ServerConfig& config, std::uint16_t& boundPort) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const auto service = std::to_string(config.port);
    addrinfo* rawAddresses = nullptr;
    const auto lookup = ::getaddrinfo(config.host.c_str(), service.c_str(), &hints, &rawAddresses);
    if (lookup != 0) {
        throw NetworkError(std::string("getaddrinfo: ") + ::gai_strerror(lookup));
    }
    struct AddressGuard {
        addrinfo* value;
        ~AddressGuard() { ::freeaddrinfo(value); }
    } guard{rawAddresses};

    std::string lastError = "no bind candidate";
    for (auto* address = rawAddresses; address != nullptr; address = address->ai_next) {
        Socket socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!socket) {
            lastError = systemError("socket");
            continue;
        }
        const int reuse = 1;
        static_cast<void>(::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
        try {
            configureSocketForSafeWrites(socket.get());
        } catch (const NetworkError& error) {
            lastError = error.what();
            continue;
        }
        if (::bind(socket.get(), address->ai_addr, address->ai_addrlen) < 0) {
            lastError = systemError("bind");
            continue;
        }
        if (::listen(socket.get(), config.backlog) < 0) {
            lastError = systemError("listen");
            continue;
        }
        sockaddr_storage local{};
        socklen_t localSize = sizeof(local);
        if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &localSize) < 0) {
            throw NetworkError(systemError("getsockname"));
        }
        if (local.ss_family == AF_INET) {
            boundPort = ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            boundPort = ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
        } else {
            throw NetworkError("listener has unsupported address family");
        }
        return socket;
    }
    throw NetworkError("cannot listen on " + config.host + ':' + service + ": " + lastError);
}

} // namespace

TcpServer::TcpServer(ServerConfig config, sql::SqlEngine& engine, Pager& pager)
    : config_(std::move(config)), engine_(engine), pager_(pager) {
    if (config_.host.empty() || config_.backlog <= 0) {
        throw std::invalid_argument("server host must be nonempty and backlog must be positive");
    }
}

void TcpServer::start() {
    if (listener_) {
        throw std::logic_error("TCP server is already listening");
    }
    listener_ = createListener(config_, boundPort_);
}

void TcpServer::serve(std::size_t connectionLimit) {
    if (!listener_) {
        start();
    }
    std::size_t served = 0;
    while (connectionLimit == 0 || served < connectionLimit) {
        int descriptor;
        do {
            descriptor = ::accept(listener_.get(), nullptr, nullptr);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            throw NetworkError(systemError("accept"));
        }
        Socket connection(descriptor);
        try {
            configureSocketForSafeWrites(connection.get());
            serveConnection(connection.get());
        } catch (const NetworkError&) {
            // A broken client affects only its own connection.
        } catch (const ProtocolError& error) {
            sendProtocolFailure(
                connection.get(), error.requestId().value_or(0), error.what());
        }
        ++served;
    }
}

void TcpServer::serveConnection(int descriptor) {
    const auto hello = readFrame(descriptor);
    if (!hello.has_value()) {
        return;
    }
    if (hello->header.messageType != MessageType::Hello
        || hello->header.requestId != 0 || !hello->payload.empty()) {
        throw ProtocolError("first client frame must be an empty HELLO with request ID 0",
                            hello->header.requestId);
    }
    writeFrame(descriptor, makeHelloAckFrame());

    while (true) {
        const auto request = readFrame(descriptor);
        if (!request.has_value()) {
            return;
        }
        if (request->header.messageType != MessageType::ExecuteSql) {
            throw ProtocolError("post-handshake client frame must be EXECUTE_SQL",
                                request->header.requestId);
        }
        const auto requestId = request->header.requestId;
        const auto source = decodeExecuteSqlPayload(*request);
        try {
            const auto result = engine_.execute(source);
            pager_.flushAll();
            try {
                writeFrame(descriptor, encodeQueryResultFrame(requestId, result));
            } catch (const ProtocolError&) {
                writeFrame(descriptor, makeErrorFrame(requestId, ErrorResponse{
                    ErrorCategory::Execution,
                    "query result exceeds protocol v1 response limits",
                    std::nullopt,
                }));
            }
        } catch (const sql::SqlError& error) {
            writeFrame(descriptor, makeErrorFrame(requestId, ErrorResponse{
                categoryFor(error.kind()), error.message(), error.span(),
            }));
        } catch (const sql::SqlExecutionError& error) {
            writeFrame(descriptor, makeErrorFrame(requestId, ErrorResponse{
                categoryFor(error.kind()), error.message(), error.span(),
            }));
        } catch (const std::exception&) {
            writeFrame(descriptor, makeErrorFrame(requestId, ErrorResponse{
                ErrorCategory::Internal,
                "internal database execution failure",
                std::nullopt,
            }));
            return;
        }
    }
}

void TcpServer::close() noexcept {
    listener_.reset();
    boundPort_ = 0;
}

void TcpServer::sendProtocolFailure(
    int descriptor,
    std::uint64_t requestId,
    std::string message) const noexcept {
    try {
        writeFrame(descriptor, makeErrorFrame(requestId, ErrorResponse{
            ErrorCategory::Protocol, std::move(message), std::nullopt,
        }));
    } catch (const std::exception&) {
        // The stream may already be unusable; closing the connection is sufficient.
    }
}

} // namespace minidb::net
