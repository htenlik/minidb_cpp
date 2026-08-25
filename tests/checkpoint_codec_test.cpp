#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_control.hpp"
#include "minidb/checkpoint_log.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>

namespace {
using minidb::test::require;

void testCheckpointPayloads() {
    const minidb::CheckpointBeginLogPayload begin{7, 123, 456};
    const auto beginBytes = minidb::encodeCheckpointBeginLogPayload(begin);
    require(beginBytes.size() == 32
                && minidb::byte_codec::readUint64(beginBytes, 0) == 7
                && minidb::byte_codec::readUint64(beginBytes, 8) == 123
                && minidb::byte_codec::readUint64(beginBytes, 16) == 456
                && minidb::byte_codec::readUint64(beginBytes, 24) == 0,
            "CHECKPOINT_BEGIN byte layout changed");
    require(minidb::decodeCheckpointBeginLogPayload(beginBytes) == begin,
            "CHECKPOINT_BEGIN did not round trip");

    const minidb::CheckpointEndLogPayload end{7, 456, 99, 42, 900};
    const auto endBytes = minidb::encodeCheckpointEndLogPayload(end);
    require(endBytes.size() == 48
                && minidb::byte_codec::readUint64(endBytes, 0) == 7
                && minidb::byte_codec::readUint64(endBytes, 8) == 456
                && minidb::byte_codec::readUint64(endBytes, 16) == 99
                && minidb::byte_codec::readUint64(endBytes, 24) == 42
                && minidb::byte_codec::readUint64(endBytes, 32) == 900
                && minidb::byte_codec::readUint64(endBytes, 40) == 0,
            "CHECKPOINT_END byte layout changed");
    require(minidb::decodeCheckpointEndLogPayload(endBytes) == end,
            "CHECKPOINT_END did not round trip");

    minidb::LogRecord system{minidb::LogRecordType::CheckpointBegin, 0,
                             minidb::INVALID_LSN, beginBytes, 456};
    minidb::validateCheckpointRecord(system);
    system.transactionId = 1;
    minidb::test::requireThrows<minidb::WalError>(
        [&] { minidb::validateCheckpointRecord(system); },
        "Checkpoint record accepted a transaction ID");
}

void testControlHeaderAndSlot() {
    const auto header = minidb::encodeCheckpointControlHeader();
    require(header.size() == 64
                && std::equal(minidb::checkpoint_control_layout::MAGIC.begin(),
                              minidb::checkpoint_control_layout::MAGIC.end(), header.begin())
                && minidb::byte_codec::readUint32(header, 8) == 1
                && minidb::byte_codec::readUint32(header, 12) == 64
                && minidb::byte_codec::readUint32(header, 16) == 64
                && minidb::byte_codec::readUint32(header, 20) == 2
                && std::all_of(header.begin() + 24, header.end(),
                               [](std::byte value) { return value == std::byte{0}; }),
            "Checkpoint-control header bytes changed");
    minidb::validateCheckpointControlHeader(header);

    const minidb::CheckpointSlot slot{9, 8, 700, 796, 55, 44, 796};
    const auto bytes = minidb::encodeCheckpointSlot(slot);
    require(bytes.size() == 64
                && minidb::byte_codec::readUint64(bytes, 0) == 9
                && minidb::byte_codec::readUint64(bytes, 8) == 8
                && minidb::byte_codec::readUint64(bytes, 16) == 700
                && minidb::byte_codec::readUint64(bytes, 24) == 796
                && minidb::byte_codec::readUint64(bytes, 32) == 55
                && minidb::byte_codec::readUint64(bytes, 40) == 44
                && minidb::byte_codec::readUint64(bytes, 48) == 796
                && minidb::byte_codec::readUint32(bytes, 56) == 0x474CB4A5U
                && minidb::byte_codec::readUint32(bytes, 60) == 0,
            "Checkpoint-control slot bytes/CRC changed");
    require(minidb::decodeCheckpointSlot(bytes) == slot,
            "Checkpoint-control slot did not round trip");
    auto corrupt = bytes;
    corrupt[31] ^= std::byte{1};
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::decodeCheckpointSlot(corrupt)); },
        "Checkpoint slot CRC did not reject corruption");
}
} // namespace

int main() {
    try {
        testCheckpointPayloads();
        testControlHeaderAndSlot();
        std::cout << "checkpoint_codec_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "checkpoint_codec_test failed: " << error.what() << '\n';
        return 1;
    }
}
