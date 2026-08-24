#include "minidb/wire_protocol.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace minidb;
using namespace minidb::net;

template <typename Function>
void requireProtocolError(Function&& function, std::string_view context) {
    try {
        function();
    } catch (const ProtocolError&) {
        return;
    }
    throw std::runtime_error(std::string(context));
}

std::uint8_t octet(std::byte value) {
    return std::to_integer<std::uint8_t>(value);
}

void testHeaderAndFrames() {
    const FrameHeader header{MessageType::ExecuteSql, 0x0102030405060708ULL, 3};
    const auto bytes = encodeFrameHeader(header);
    const std::array<std::uint8_t, FRAME_HEADER_SIZE> expected{
        'M', 'D', 'B', 'P', 0, 1, 0, 2,
        1, 2, 3, 4, 5, 6, 7, 8,
        0, 0, 0, 3, 0, 0, 0, 0,
    };
    minidb::test::require(bytes.size() == expected.size(), "header width changed");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        minidb::test::require(octet(bytes[index]) == expected[index],
                              "header bytes/network order changed");
    }
    minidb::test::require(decodeFrameHeader(bytes) == header,
                          "frame header failed round-trip");
    for (const auto type : {
             MessageType::Hello, MessageType::ExecuteSql, MessageType::HelloAck,
             MessageType::CommandResult, MessageType::SelectResult,
             MessageType::ErrorResponse,
         }) {
        const FrameHeader known{type, 7, MAX_FRAME_PAYLOAD};
        minidb::test::require(decodeFrameHeader(encodeFrameHeader(known)) == known,
                              "known message type or maximum payload was rejected");
    }

    const auto hello = makeHelloFrame();
    minidb::test::require(hello.header.messageType == MessageType::Hello
                              && hello.header.requestId == 0 && hello.payload.empty()
                              && decodeFrame(encodeFrame(hello)) == hello,
                          "HELLO encoding is invalid");

    const auto execute = makeExecuteSqlFrame(99, "SELECT 1");
    minidb::test::require(decodeExecuteSqlPayload(decodeFrame(encodeFrame(execute)))
                              == "SELECT 1",
                          "EXECUTE_SQL did not preserve SQL bytes");
    minidb::test::require(execute.payload.size() == 8
                              && octet(execute.payload[0]) == 'S'
                              && octet(execute.payload[7]) == '1',
                          "EXECUTE_SQL payload is not the exact raw SQL byte sequence");
}

void testCommandResult() {
    const sql::CommandResult input{
        sql::CommandKind::Insert,
        1,
        RecordId{0x10203040U, 0x5060U},
        sql::ExecutionStats{sql::AccessPath::PrimaryKeyLookup, 2, 1, 1},
    };
    const auto payload = encodeCommandResultPayload(input);
    const std::array<std::uint8_t, 48> prefix{
        0, 2, 0, 1,
        0, 0, 0, 0, 0, 0, 0, 1,
        0, 2, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 0, 0, 0, 1,
        0, 0, 0, 0, 0, 0, 0, 1,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0, 0,
    };
    minidb::test::require(payload.size() == prefix.size(), "CommandResult layout width changed");
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        minidb::test::require(octet(payload[index]) == prefix[index],
                              "CommandResult byte layout changed");
    }
    minidb::test::require(decodeCommandResultPayload(payload) == input,
                          "CommandResult failed round-trip");
    const sql::QueryResult query = input;
    minidb::test::require(decodeQueryResultFrame(encodeQueryResultFrame(123, query)) == query,
                          "Command QueryResult failed round-trip");
}

void testSelectValuesAndError() {
    sql::SelectResult input{
        {"null", "u32", "i64", "bool", "text"},
        {{std::monostate{}, std::numeric_limits<std::uint32_t>::max(),
          std::numeric_limits<std::int64_t>::min(), true, std::string("MiniDB++")}},
        {RecordId{17, 9}},
        sql::ExecutionStats{sql::AccessPath::HeapScan, 7, 1, 0},
    };
    const auto payload = encodeSelectResultPayload(input);
    minidb::test::require(payload.size() == 124
                              && octet(payload[3]) == 5
                              && octet(payload[11]) == 1
                              && octet(payload[15]) == 1
                              && octet(payload[17]) == 1
                              && octet(payload[82]) == 0
                              && octet(payload[85]) == 17
                              && octet(payload[87]) == 9
                              && octet(payload[93]) == 5
                              && octet(payload[94]) == 0
                              && octet(payload[95]) == 1
                              && octet(payload[96]) == 0xFF
                              && octet(payload[100]) == 2
                              && octet(payload[101]) == 0x80
                              && octet(payload[109]) == 3
                              && octet(payload[110]) == 1
                              && octet(payload[111]) == 4
                              && octet(payload[115]) == 8,
                          "SelectResult/Value/RID exact wire layout changed");
    minidb::test::require(decodeSelectResultPayload(payload) == input,
                          "SelectResult values/RID/stats failed round-trip");
    const sql::QueryResult query = input;
    minidb::test::require(decodeQueryResultFrame(encodeQueryResultFrame(
                              std::numeric_limits<std::uint64_t>::max(), query)) == query,
                          "Select QueryResult failed round-trip");

    const ErrorResponse error{
        ErrorCategory::Parser,
        "expected expression",
        sql::SourceSpan{{4, 2, 3}, {9, 2, 8}},
    };
    const auto errorPayload = encodeErrorResponsePayload(error);
    minidb::test::require(errorPayload.size() == 59
                              && octet(errorPayload[1]) == 3
                              && octet(errorPayload[3]) == 1
                              && octet(errorPayload[7]) == 19
                              && octet(errorPayload[34]) == 4
                              && octet(errorPayload[42]) == 9,
                          "ErrorResponse exact byte layout changed");
    minidb::test::require(decodeErrorResponsePayload(errorPayload) == error,
                          "ErrorResponse category/message/span failed round-trip");
    const auto frame = makeErrorFrame(999, error);
    minidb::test::require(frame.header.requestId == 999
                              && frame.header.messageType == MessageType::ErrorResponse,
                          "ErrorResponse request ID/type changed");
    requireProtocolError(
        [] { static_cast<void>(encodeErrorResponsePayload(ErrorResponse{
            static_cast<ErrorCategory>(99), "bad", std::nullopt,
        })); },
        "unknown ErrorResponse category was encoded");
    requireProtocolError(
        [] { static_cast<void>(encodeErrorResponsePayload(ErrorResponse{
            ErrorCategory::Parser, "bad span", sql::SourceSpan{{5, 0, 1}, {4, 1, 1}},
        })); },
        "invalid source span was encoded");
}

void testMalformedInput() {
    auto header = encodeFrameHeader({MessageType::Hello, 55, 0});
    for (const std::size_t offset : {0U, 5U, 7U, 23U}) {
        auto corrupt = header;
        corrupt[offset] = std::byte{0xFF};
        requireProtocolError([&] { static_cast<void>(decodeFrameHeader(corrupt)); },
                             "corrupt frame header was accepted");
    }
    requireProtocolError(
        [&] { static_cast<void>(decodeFrameHeader(std::span(header).first(23))); },
        "short frame header was accepted");
    requireProtocolError(
        [] { static_cast<void>(encodeFrameHeader(
            {MessageType::Hello, 0, MAX_FRAME_PAYLOAD + 1})); },
        "oversized frame was encoded");

    auto invalidValue = encodeSelectResultPayload(sql::SelectResult{
        {"v"}, {{true}}, {}, {},
    });
    invalidValue.back() = std::byte{2};
    requireProtocolError([&] { static_cast<void>(decodeSelectResultPayload(invalidValue)); },
                         "invalid BOOLEAN was accepted");
    invalidValue.back() = std::byte{0};
    invalidValue[invalidValue.size() - 2] = std::byte{0xFF};
    requireProtocolError([&] { static_cast<void>(decodeSelectResultPayload(invalidValue)); },
                         "invalid Value tag was accepted");

    const auto valid = encodeCommandResultPayload(sql::CommandResult{});
    for (std::size_t size = 0; size < valid.size(); ++size) {
        requireProtocolError(
            [&] { static_cast<void>(decodeCommandResultPayload(
                std::span(valid).first(size))); },
            "truncated CommandResult was accepted");
    }
    auto trailing = valid;
    trailing.push_back(std::byte{0});
    requireProtocolError([&] { static_cast<void>(decodeCommandResultPayload(trailing)); },
                         "trailing CommandResult byte was accepted");
}

void testDeterministicFuzzInputs() {
    constexpr std::uint64_t seed = 0x8BADF00DULL;
    constexpr std::size_t iterations = 10'000;
    std::mt19937_64 random(seed);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto size = static_cast<std::size_t>(random() % 300U);
        WireBytes bytes(size);
        std::generate(bytes.begin(), bytes.end(), [&] {
            return static_cast<std::byte>(random() & 0xFFU);
        });
        const auto choice = random() % 4U;
        try {
            if (choice == 0) {
                static_cast<void>(decodeFrameHeader(bytes));
            } else if (choice == 1) {
                static_cast<void>(decodeFrame(bytes));
            } else if (choice == 2) {
                static_cast<void>(decodeSelectResultPayload(bytes));
            } else {
                static_cast<void>(decodeErrorResponsePayload(bytes));
            }
        } catch (const ProtocolError&) {
            // Rejection is the expected outcome for arbitrary bytes.
        }
    }
}

} // namespace

int main() {
    try {
        testHeaderAndFrames();
        testCommandResult();
        testSelectValuesAndError();
        testMalformedInput();
        testDeterministicFuzzInputs();
        std::cout << "wire protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wire protocol test failure: " << error.what() << '\n';
        return 1;
    }
}
