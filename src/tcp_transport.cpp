#include "minidb/tcp_transport.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace minidb::net {
namespace {

std::string systemError(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool readExact(
    int descriptor,
    std::span<std::byte> destination,
    bool allowCleanInitialEof) {
    std::size_t received = 0;
    while (received < destination.size()) {
        const auto result = ::recv(
            descriptor,
            destination.data() + received,
            destination.size() - received,
            0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            if (allowCleanInitialEof && received == 0) {
                return false;
            }
            throw ProtocolError("peer disconnected in the middle of a frame");
        }
        if (errno == EINTR) {
            continue;
        }
        throw NetworkError(systemError("recv"));
    }
    return true;
}

} // namespace

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : descriptor_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int Socket::release() noexcept {
    const auto result = descriptor_;
    descriptor_ = -1;
    return result;
}

void Socket::reset(int descriptor) noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
}

void configureSocketForSafeWrites(int descriptor) {
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        throw NetworkError(systemError("setsockopt(SO_NOSIGPIPE)"));
    }
#else
    static_cast<void>(descriptor);
#endif
}

void writeAll(int descriptor, std::span<const std::byte> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
        const auto result = ::send(
            descriptor,
            bytes.data() + sent,
            bytes.size() - sent,
            flags);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            throw NetworkError("send: peer accepted no bytes");
        }
        throw NetworkError(systemError("send"));
    }
}

void writeFrame(int descriptor, const Frame& frame) {
    const auto bytes = encodeFrame(frame);
    writeAll(descriptor, bytes);
}

std::optional<Frame> readFrame(int descriptor) {
    std::array<std::byte, FRAME_HEADER_SIZE> headerBytes{};
    if (!readExact(descriptor, headerBytes, true)) {
        return std::nullopt;
    }
    const auto header = decodeFrameHeader(headerBytes);
    WireBytes payload(header.payloadLength);
    if (!payload.empty()) {
        static_cast<void>(readExact(descriptor, payload, false));
    }
    return Frame{header, std::move(payload)};
}

} // namespace minidb::net
