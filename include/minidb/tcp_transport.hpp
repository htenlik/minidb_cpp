#pragma once

#include "minidb/wire_protocol.hpp"

#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace minidb::net {

class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(int descriptor) noexcept : descriptor_(descriptor) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }
    [[nodiscard]] int release() noexcept;
    void reset(int descriptor = -1) noexcept;

private:
    int descriptor_ = -1;
};

void configureSocketForSafeWrites(int descriptor);
void writeAll(int descriptor, std::span<const std::byte> bytes);
void writeFrame(int descriptor, const Frame& frame);

// Returns nullopt only for an orderly EOF before any byte of a new frame header.
// EOF after a partial header/payload is a fatal ProtocolError.
[[nodiscard]] std::optional<Frame> readFrame(int descriptor);

} // namespace minidb::net
