#pragma once

#include "minidb/sql_executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb::net {

using WireBytes = std::vector<std::byte>;

inline constexpr std::array<std::byte, 4> PROTOCOL_MAGIC{
    std::byte{'M'}, std::byte{'D'}, std::byte{'B'}, std::byte{'P'},
};
inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
inline constexpr std::size_t FRAME_HEADER_SIZE = 24;
inline constexpr std::uint32_t MAX_FRAME_PAYLOAD = 256U * 1024U;
inline constexpr std::uint32_t MAX_SQL_BYTES = 64U * 1024U;
inline constexpr std::uint32_t MAX_WIRE_STRING_BYTES = 64U * 1024U;
inline constexpr std::uint32_t MAX_WIRE_COLUMNS = 1024;
inline constexpr std::uint32_t MAX_WIRE_ROWS = 65536;

enum class MessageType : std::uint16_t {
    Hello = 1,
    ExecuteSql = 2,
    HelloAck = 101,
    CommandResult = 102,
    SelectResult = 103,
    ErrorResponse = 104,
};

struct FrameHeader {
    MessageType messageType = MessageType::Hello;
    std::uint64_t requestId = 0;
    std::uint32_t payloadLength = 0;

    bool operator==(const FrameHeader&) const = default;
};

struct Frame {
    FrameHeader header{};
    WireBytes payload;

    bool operator==(const Frame&) const = default;
};

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(
        std::string message,
        std::optional<std::uint64_t> requestId = std::nullopt)
        : std::runtime_error(std::move(message)), requestId_(requestId) {}

    [[nodiscard]] const std::optional<std::uint64_t>& requestId() const noexcept {
        return requestId_;
    }

private:
    std::optional<std::uint64_t> requestId_;
};

enum class ErrorCategory : std::uint16_t {
    Protocol = 1,
    Lexer = 2,
    Parser = 3,
    Semantic = 4,
    Constraint = 5,
    Execution = 6,
    Internal = 7,
};

struct ErrorResponse {
    ErrorCategory category = ErrorCategory::Internal;
    std::string message;
    std::optional<sql::SourceSpan> span;

    bool operator==(const ErrorResponse&) const = default;
};

class RemoteSqlError : public std::runtime_error {
public:
    RemoteSqlError(std::uint64_t requestId, ErrorResponse response);

    [[nodiscard]] std::uint64_t requestId() const noexcept { return requestId_; }
    [[nodiscard]] ErrorCategory category() const noexcept { return response_.category; }
    [[nodiscard]] const std::string& message() const noexcept { return response_.message; }
    [[nodiscard]] const std::optional<sql::SourceSpan>& span() const noexcept {
        return response_.span;
    }

private:
    std::uint64_t requestId_;
    ErrorResponse response_;
};

[[nodiscard]] WireBytes encodeFrameHeader(const FrameHeader& header);
[[nodiscard]] FrameHeader decodeFrameHeader(std::span<const std::byte> bytes);
[[nodiscard]] WireBytes encodeFrame(const Frame& frame);
[[nodiscard]] Frame decodeFrame(std::span<const std::byte> bytes);

[[nodiscard]] Frame makeHelloFrame();
[[nodiscard]] Frame makeHelloAckFrame();
[[nodiscard]] Frame makeExecuteSqlFrame(std::uint64_t requestId, std::string_view sql);
[[nodiscard]] std::string decodeExecuteSqlPayload(const Frame& frame);

[[nodiscard]] WireBytes encodeCommandResultPayload(const sql::CommandResult& result);
[[nodiscard]] sql::CommandResult decodeCommandResultPayload(
    std::span<const std::byte> payload);
[[nodiscard]] WireBytes encodeSelectResultPayload(const sql::SelectResult& result);
[[nodiscard]] sql::SelectResult decodeSelectResultPayload(
    std::span<const std::byte> payload);
[[nodiscard]] Frame encodeQueryResultFrame(
    std::uint64_t requestId,
    const sql::QueryResult& result);
[[nodiscard]] sql::QueryResult decodeQueryResultFrame(const Frame& frame);

[[nodiscard]] WireBytes encodeErrorResponsePayload(const ErrorResponse& error);
[[nodiscard]] ErrorResponse decodeErrorResponsePayload(
    std::span<const std::byte> payload);
[[nodiscard]] Frame makeErrorFrame(std::uint64_t requestId, const ErrorResponse& error);

} // namespace minidb::net
