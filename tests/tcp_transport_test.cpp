#include "minidb/tcp_transport.hpp"
#include "test_utils.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace minidb::net;

std::array<Socket, 2> socketPair() {
    int descriptors[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    return {Socket(descriptors[0]), Socket(descriptors[1])};
}

void sendChunks(int descriptor, std::span<const std::byte> bytes, std::span<const std::size_t> chunks) {
    std::size_t offset = 0;
    std::size_t chunkIndex = 0;
    while (offset < bytes.size()) {
        const auto count = std::min(chunks[chunkIndex++ % chunks.size()], bytes.size() - offset);
        std::size_t written = 0;
        while (written < count) {
            const auto result = ::send(
                descriptor, bytes.data() + offset + written, count - written, 0);
            if (result > 0) {
                written += static_cast<std::size_t>(result);
            } else if (result < 0 && errno == EINTR) {
                continue;
            } else {
                throw std::runtime_error("fragmented send failed");
            }
        }
        offset += count;
    }
}

void testFragmentation() {
    for (const auto chunks : {
             std::array<std::size_t, 1>{1},
             std::array<std::size_t, 1>{7},
         }) {
        auto sockets = socketPair();
        const auto expected = makeExecuteSqlFrame(0xFFFFFFFFFFFFFFFFULL,
                                                   "SELECT * FROM fragmented");
        const auto bytes = encodeFrame(expected);
        sendChunks(sockets[0].get(), bytes, chunks);
        const auto actual = readFrame(sockets[1].get());
        minidb::test::require(actual.has_value() && *actual == expected,
                              "fragmented frame was not reconstructed exactly");
    }

    auto sockets = socketPair();
    const auto expected = makeExecuteSqlFrame(88, "irregular chunks");
    const auto bytes = encodeFrame(expected);
    const std::array<std::size_t, 7> irregular{3, 2, 19, 1, 5, 4, 11};
    sendChunks(sockets[0].get(), bytes, irregular);
    minidb::test::require(readFrame(sockets[1].get()) == expected,
                          "irregular header/payload fragmentation failed");
}

void testEofSemantics() {
    {
        auto sockets = socketPair();
        sockets[0].reset();
        minidb::test::require(!readFrame(sockets[1].get()).has_value(),
                              "clean frame-boundary EOF was not normal");
    }
    {
        auto sockets = socketPair();
        const auto frame = encodeFrame(makeExecuteSqlFrame(1, "SELECT * FROM x"));
        writeAll(sockets[0].get(), std::span(frame).first(11));
        sockets[0].reset();
        minidb::test::requireThrows<ProtocolError>(
            [&] { static_cast<void>(readFrame(sockets[1].get())); },
            "partial header EOF was accepted");
    }
    {
        auto sockets = socketPair();
        const auto frame = encodeFrame(makeExecuteSqlFrame(2, "SELECT * FROM x"));
        writeAll(sockets[0].get(), std::span(frame).first(FRAME_HEADER_SIZE + 3));
        sockets[0].reset();
        minidb::test::requireThrows<ProtocolError>(
            [&] { static_cast<void>(readFrame(sockets[1].get())); },
            "partial payload EOF was accepted");
    }
}

void testWriteAllAndBrokenPeer() {
    auto sockets = socketPair();
    WireBytes payload(200'000, std::byte{0xA5});
    std::optional<Frame> received;
    std::exception_ptr receiverError;
    std::thread receiver([&] {
        try {
            received = readFrame(sockets[1].get());
        } catch (...) {
            receiverError = std::current_exception();
        }
    });
    writeFrame(sockets[0].get(), Frame{
        FrameHeader{MessageType::ExecuteSql, 7, static_cast<std::uint32_t>(payload.size())},
        payload,
    });
    receiver.join();
    if (receiverError) std::rethrow_exception(receiverError);
    minidb::test::require(received.has_value() && received->payload == payload,
                          "writeAll failed to send the complete large frame");

    sockets[1].reset();
    minidb::test::requireThrows<NetworkError>(
        [&] { writeFrame(sockets[0].get(), makeHelloFrame()); },
        "broken peer did not report a contained network error");
}

} // namespace

int main() {
    try {
        testFragmentation();
        testEofSemantics();
        testWriteAllAndBrokenPeer();
        std::cout << "TCP transport tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TCP transport test failure: " << error.what() << '\n';
        return 1;
    }
}
