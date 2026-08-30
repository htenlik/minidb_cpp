#include "minidb/byte_codec.hpp"
#include "minidb/checkpoint_log.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {
using minidb::test::require;

void testExactLayouts() {
    const minidb::FuzzyCheckpointBeginLogPayload begin{7, 1234};
    const auto beginBytes = minidb::encodeFuzzyCheckpointBeginLogPayload(begin);
    require(beginBytes.size() == minidb::fuzzy_checkpoint_begin_log_layout::PAYLOAD_SIZE
                && minidb::byte_codec::readUint16(beginBytes, 0) == 1
                && minidb::byte_codec::readUint16(beginBytes, 2) == 32
                && minidb::byte_codec::readUint32(beginBytes, 4) == 0
                && minidb::byte_codec::readUint64(beginBytes, 8) == 7
                && minidb::byte_codec::readUint64(beginBytes, 16) == 1234
                && minidb::byte_codec::readUint64(beginBytes, 24) == 0
                && minidb::decodeFuzzyCheckpointBeginLogPayload(beginBytes) == begin,
            "FUZZY_CHECKPOINT_BEGIN exact encoding is incorrect");

    const minidb::FuzzyCheckpointEndLogPayload end{
        7, 222, 99, 41,
        {{3, 400, minidb::INVALID_LSN}, {9, 800, minidb::INVALID_LSN}},
        {{10, minidb::CheckpointTransactionStatus::Active, 100, 200, 12}},
    };
    const auto bytes = minidb::encodeFuzzyCheckpointEndLogPayload(end);
    require(bytes.size() == 64 + 2 * 16 + 48
                && minidb::byte_codec::readUint16(bytes, 0) == 1
                && minidb::byte_codec::readUint16(bytes, 2) == 64
                && minidb::byte_codec::readUint16(bytes, 4) == 16
                && minidb::byte_codec::readUint16(bytes, 6) == 48
                && minidb::byte_codec::readUint64(bytes, 8) == 7
                && minidb::byte_codec::readUint64(bytes, 16) == 222
                && minidb::byte_codec::readUint64(bytes, 24) == 99
                && minidb::byte_codec::readUint64(bytes, 32) == 41
                && minidb::byte_codec::readUint32(bytes, 40) == 2
                && minidb::byte_codec::readUint32(bytes, 44) == 1
                && minidb::byte_codec::readUint32(bytes, 64) == 3
                && minidb::byte_codec::readUint64(bytes, 72) == 400
                && minidb::byte_codec::readUint64(bytes, 80) == 9
                && minidb::byte_codec::readUint64(bytes, 88) == 800
                && minidb::byte_codec::readUint64(bytes, 96) == 10
                && minidb::byte_codec::readUint16(bytes, 104) == 1
                && minidb::decodeFuzzyCheckpointEndLogPayload(bytes) == end,
            "FUZZY_CHECKPOINT_END exact encoding is incorrect");

    const auto empty = minidb::encodeFuzzyCheckpointEndLogPayload({7, 222, 99, 41, {}, {}});
    require(empty.size() == 64
                && minidb::byte_codec::readUint32(empty, 40) == 0
                && minidb::byte_codec::readUint32(empty, 44) == 0,
            "Empty fuzzy checkpoint lists do not have exact canonical bytes");

    const auto one = minidb::encodeFuzzyCheckpointEndLogPayload({
        1, 64, 10, 2, {{2, 64, minidb::INVALID_LSN}}, {},
    });
    const auto record = minidb::encodeWalRecord({
        minidb::LogRecordType::FuzzyCheckpointEnd,
        minidb::INVALID_TRANSACTION_ID,
        minidb::INVALID_LSN,
        one,
        minidb::INVALID_LSN,
    }, 64);
    require(record.size() == 128
                && minidb::byte_codec::readUint16(record, 6) == 12
                && minidb::byte_codec::readUint32(record, 40) == 0x2FCF2890U,
            "One-entry fuzzy checkpoint WAL bytes/CRC32C changed");
}

void testValidationFailures() {
    const auto valid = minidb::encodeFuzzyCheckpointEndLogPayload({
        1, 64, 10, 2, {{2, 64, minidb::INVALID_LSN}}, {},
    });
    for (const std::size_t offset : {std::size_t{0}, std::size_t{2}, std::size_t{4},
                                     std::size_t{6}, std::size_t{48}, std::size_t{68}}) {
        auto corrupt = valid;
        corrupt[offset] ^= std::byte{1};
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(minidb::decodeFuzzyCheckpointEndLogPayload(corrupt)); },
            "Corrupt fuzzy checkpoint field was accepted");
    }
    auto trailing = valid;
    trailing.push_back(std::byte{0});
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::decodeFuzzyCheckpointEndLogPayload(trailing)); },
        "Trailing fuzzy checkpoint bytes were accepted");
    minidb::test::requireThrows<minidb::WalError>(
        [] { static_cast<void>(minidb::encodeFuzzyCheckpointEndLogPayload({
            1, 64, 10, 2,
            {{3, 70, minidb::INVALID_LSN}, {2, 80, minidb::INVALID_LSN}}, {},
        })); },
        "Noncanonical DPT ordering was accepted");
    minidb::test::requireThrows<minidb::WalError>(
        [] { static_cast<void>(minidb::encodeFuzzyCheckpointEndLogPayload({
            1, 64, 10, 2, {},
            {{9, minidb::CheckpointTransactionStatus::Active, 200, 100, 10}},
        })); },
        "ATT lastLSN before beginLSN was accepted");
    minidb::test::requireThrows<minidb::WalError>(
        [] { static_cast<void>(minidb::encodeFuzzyCheckpointEndLogPayload({
            1, 64, 10, 2, {},
            {{9, static_cast<minidb::CheckpointTransactionStatus>(99), 100, 200, 10}},
        })); },
        "Unknown ATT status was accepted");
    minidb::test::requireThrows<minidb::WalError>(
        [] { static_cast<void>(minidb::encodeFuzzyCheckpointEndLogPayload({
            1, 64, 10, 2, {},
            {{9, minidb::CheckpointTransactionStatus::Active, 100, 200, 10},
             {9, minidb::CheckpointTransactionStatus::Active, 100, 200, 10}},
        })); },
        "Duplicate ATT TransactionId was accepted");
}

void testDeterministicCandidateFuzz() {
    constexpr std::uint64_t SEED = 0xD17A5EEDULL;
    constexpr std::size_t CANDIDATES = 10'000;
    std::mt19937_64 random(SEED);
    for (std::size_t candidate = 0; candidate < CANDIDATES; ++candidate) {
        const auto length = static_cast<std::size_t>(random() % 384);
        std::vector<std::byte> bytes(length);
        for (auto& value : bytes) value = static_cast<std::byte>(random() & 0xffU);
        try {
            const auto decoded = minidb::decodeFuzzyCheckpointEndLogPayload(bytes);
            const auto canonical = minidb::encodeFuzzyCheckpointEndLogPayload(decoded);
            require(canonical == bytes,
                    "Accepted fuzzy payload was not canonical at candidate "
                        + std::to_string(candidate));
        } catch (const minidb::WalError&) {
            // Rejection is the expected result for almost all random candidates.
        }
    }
}
} // namespace

int main() {
    try {
        testExactLayouts();
        testValidationFailures();
        testDeterministicCandidateFuzz();
        std::cout << "fuzzy_checkpoint_codec_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fuzzy_checkpoint_codec_test failed: " << error.what() << '\n';
        return 1;
    }
}
