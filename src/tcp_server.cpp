#include "minidb/tcp_server.hpp"

#include "minidb/sql_error.hpp"
#include "minidb/sql_semantics.hpp"

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __APPLE__
#include <netinet/tcp_fsm.h>
#endif
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace minidb::net {
namespace {

std::string systemError(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

// FIN can arrive behind queued bytes. MSG_PEEK alone would keep such a closed
// peer registered forever while its SQL request waits for database access.
bool peerFinReceived(int descriptor) noexcept {
#if defined(__APPLE__)
    tcp_connection_info info{};
    socklen_t size = sizeof(info);
    return ::getsockopt(descriptor, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &size) == 0
        && (info.tcpi_state == TCPS_CLOSE_WAIT || info.tcpi_state == TCPS_CLOSED);
#elif defined(__linux__)
    tcp_info info{};
    socklen_t size = sizeof(info);
    return ::getsockopt(descriptor, IPPROTO_TCP, TCP_INFO, &info, &size) == 0
        && (info.tcpi_state == TCP_CLOSE_WAIT || info.tcpi_state == TCP_CLOSE);
#else
    static_cast<void>(descriptor);
    return false; // Other POSIX platforms retain poll/EOF/error detection.
#endif
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

TcpServer::TcpServer(
    ServerConfig config,
    sql::SqlEngine& engine,
    BufferPoolManager& bufferPool,
    DiskManager& diskManager)
    : config_(std::move(config)),
      engine_(engine) {
    static_cast<void>(bufferPool);
    static_cast<void>(diskManager);
    if (config_.host.empty() || config_.backlog <= 0
        || config_.bufferFrames == 0 || config_.lruK == 0 || config_.maxConnections == 0) {
        throw std::invalid_argument(
            "server host must be nonempty and backlog/buffer settings must be positive");
    }
}

TcpServer::~TcpServer() { close(); }

void TcpServer::start() {
    std::lock_guard lock(lifecycleMutex_);
    if (failed_) {
        throw std::runtime_error("TCP server requires restart after transaction cleanup failure");
    }
    if (listener_) {
        throw std::logic_error("TCP server is already listening");
    }
    if (stopping_) throw std::logic_error("TCP server cannot restart after shutdown");
    std::uint16_t port = 0;
    listener_ = createListener(config_, port);
    boundPort_ = port;
}

void TcpServer::serve(std::size_t connectionLimit) {
    std::unique_lock serving(serveMutex_);
    if (port() == 0) start();
    try {
        std::size_t served = 0;
        while (!stopping_ && (connectionLimit == 0 || served < connectionLimit)) {
            reapWorkers();
            int listener;
            { std::lock_guard lock(lifecycleMutex_); listener = listener_.get(); }
            pollfd readiness{listener, POLLIN, 0};
            const auto ready = ::poll(&readiness, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) throw NetworkError(systemError("poll listener"));
            if (ready == 0 || stopping_) continue;
            const auto descriptor = ::accept(listener, nullptr, nullptr);
            if (descriptor < 0 && (errno == EINTR || stopping_)) continue;
            if (descriptor < 0) throw NetworkError(systemError("accept"));
            Socket connection(descriptor);
            ++served;
            configureSocketForSafeWrites(descriptor);
            reapWorkers();
            const auto session = registerConnection(descriptor);
            if (!session) {
                sendProtocolFailure(descriptor, 0, "maximum concurrent sessions reached or server shutting down");
                continue;
            }
            try {
                auto worker = std::make_unique<Worker>(connection.release());
                auto* state = worker.get();
                std::lock_guard lock(lifecycleMutex_);
                workers_.push_back(std::move(worker));
                try {
                    state->thread = std::jthread([this, state, id = *session](std::stop_token stop) {
                        try { runConnection(state->connection.get(), id, stop); }
                        catch (const NetworkError&) {}
                        catch (const ProtocolError& error) {
                            sendProtocolFailure(state->connection.get(), error.requestId().value_or(0), error.what());
                        } catch (...) {
                            { std::lock_guard failure(lifecycleMutex_); if (!fatalError_) fatalError_ = std::current_exception(); }
                            failed_ = true;
                            requestStop();
                        }
                        state->done = true;
                        unregisterConnection(state->connection.get());
                    });
                } catch (...) { workers_.pop_back(); throw; }
            } catch (...) { unregisterConnection(descriptor); throw; }
        }
        joinWorkers();
        std::exception_ptr error;
        { std::lock_guard lock(lifecycleMutex_); error = fatalError_; }
        if (error) std::rethrow_exception(error);
    } catch (...) {
        requestStop(); joinWorkers(); throw;
    }
}

void TcpServer::serveConnection(int descriptor) {
    const auto session = registerConnection(descriptor);
    if (!session) {
        sendProtocolFailure(descriptor, 0, "maximum concurrent sessions reached or server shutting down");
        return;
    }
    try { runConnection(descriptor, *session); }
    catch (...) { unregisterConnection(descriptor); throw; }
    unregisterConnection(descriptor);
}

std::optional<SessionId> TcpServer::registerConnection(int descriptor) {
    std::lock_guard lock(lifecycleMutex_);
    if (failed_) {
        throw std::runtime_error("TCP server requires restart after transaction cleanup failure");
    }
    if (stopping_ || stats_.activeSessions >= config_.maxConnections) {
        ++stats_.connectionRejects;
        return std::nullopt;
    }
    if (nextSessionId_ == std::numeric_limits<SessionId>::max()) {
        throw std::overflow_error("TCP session identifier range exhausted");
    }
    const auto session = nextSessionId_++;
    if (!connections_.insert(descriptor).second) throw std::logic_error("Connection is already being served");
    ++stats_.activeSessions;
    ++stats_.sessionsAccepted;
    stats_.peakActiveSessions = std::max(stats_.peakActiveSessions, stats_.activeSessions);
    return session;
}

void TcpServer::unregisterConnection(int descriptor) noexcept {
    std::lock_guard lock(lifecycleMutex_);
    if (connections_.erase(descriptor) != 0) --stats_.activeSessions;
}

void TcpServer::runConnection(int descriptor, SessionId session, std::stop_token stop) {
    std::exception_ptr connectionError;
    bool observedDisconnect = false;
    try {
        engine_.transactionManager().setCancellationProbe(session, [&, descriptor, stop] {
            if (stopping_ || stop.stop_requested()) return true;
            pollfd peer{descriptor, POLLIN, 0};
            const auto ready = ::poll(&peer, 1, 0);
            bool disconnected = ready > 0 && (peer.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
            if (ready > 0 && (peer.revents & POLLIN) != 0) {
                char byte;
                const auto count = ::recv(descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                disconnected = disconnected || count == 0
                    || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
                if (count > 0) disconnected = disconnected || peerFinReceived(descriptor);
            }
            if (disconnected && !observedDisconnect) { observedDisconnect = true; ++disconnectWhileWaiting_; }
            return disconnected;
        });
        serveSession(descriptor, session);
    } catch (...) {
        connectionError = std::current_exception();
    }
    try {
        // The writer's exclusive lease remains held through durable rollback.
        engine_.closeSession(session, stopping_ ? SessionCloseReason::Shutdown : SessionCloseReason::Disconnect);
    } catch (...) {
        failed_ = true;
        requestStop();
        std::throw_with_nested(std::runtime_error(
            "TCP session transaction cleanup failed; server requires restart"));
    }
    if (connectionError) std::rethrow_exception(connectionError);
}

void TcpServer::serveSession(int descriptor, SessionId session) {
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
            const auto result = engine_.execute(source, session);
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
    requestStop();
    std::lock_guard serving(serveMutex_);
    joinWorkers();
    std::lock_guard lock(lifecycleMutex_);
    listener_.reset(); boundPort_ = 0;
}

void TcpServer::requestStop() noexcept {
    stopping_ = true;
    engine_.transactionManager().accessGate().shutdown();
    std::lock_guard lock(lifecycleMutex_);
    if (listener_) static_cast<void>(::shutdown(listener_.get(), SHUT_RDWR));
    for (auto descriptor : connections_) static_cast<void>(::shutdown(descriptor, SHUT_RDWR));
    for (auto& worker : workers_) worker->thread.request_stop();
}

void TcpServer::reapWorkers() {
    std::vector<std::unique_ptr<Worker>> finished;
    {
        std::lock_guard lock(lifecycleMutex_);
        for (auto iterator = workers_.begin(); iterator != workers_.end();) {
            if ((*iterator)->done) { finished.push_back(std::move(*iterator)); iterator = workers_.erase(iterator); }
            else ++iterator;
        }
    }
    // Join without the lifecycle mutex: cleanup may need to unregister a socket.
}
void TcpServer::joinWorkers() {
    std::vector<std::unique_ptr<Worker>> workers;
    { std::lock_guard lock(lifecycleMutex_); workers.swap(workers_); }
    for (auto& worker : workers) if (worker->thread.joinable()) worker->thread.join();
}
ServerConcurrencyStats TcpServer::concurrencyStats() const {
    std::lock_guard lock(lifecycleMutex_);
    auto result = stats_;
    result.disconnectWhileWaiting = disconnectWhileWaiting_.load();
    return result;
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
