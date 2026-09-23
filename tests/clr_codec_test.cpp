#include "minidb/byte_codec.hpp"
#include "minidb/recovery_log.hpp"
#include "test_utils.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>

namespace {

using minidb::test::require;

minidb::CompensationLogPayload representativePayload() {
    minidb::CompensationLogPayload payload;
    payload.pageId = 0x12345678U;
    payload.pageExisted = true;
    payload.pageSupportsLsn = true;
    payload.undoNextLsn = 6000;
    payload.compensatedUpdateLsn = 7000;
    for (std::size_t index = 0; index < payload.compensatedImage.size(); ++index) {
        payload.compensatedImage[index] = static_cast<std::byte>(index & 0xFFU);
    }
    return payload;
}

void testExactClrEncoding() {
    using namespace minidb::compensation_log_layout;
    const auto payload = representativePayload();
    const auto encoded = minidb::encodeCompensationLogPayload(payload);
    require(encoded.size() == PAYLOAD_SIZE && PAYLOAD_SIZE == 4136,
            "CLR payload width changed");
    require(minidb::byte_codec::readUint32(encoded, PAGE_ID_OFFSET) == 0x12345678U
                && minidb::byte_codec::readUint32(encoded, FLAGS_OFFSET)
                    == (PAGE_EXISTED | PAGE_SUPPORTS_LSN)
                && minidb::byte_codec::readUint32(encoded, PAGE_SIZE_OFFSET) == 4096
                && minidb::byte_codec::readUint16(encoded, VERSION_OFFSET) == 1
                && minidb::byte_codec::readUint16(encoded, HEADER_SIZE_OFFSET) == 40
                && minidb::byte_codec::readUint64(encoded, UNDO_NEXT_LSN_OFFSET) == 6000
                && minidb::byte_codec::readUint64(
                    encoded, COMPENSATED_UPDATE_LSN_OFFSET) == 7000
                && minidb::byte_codec::readUint64(encoded, RESERVED_OFFSET) == 0,
            "CLR fixed header is not exact little-endian encoding");
    require(encoded[PAGE_IMAGE_OFFSET] == std::byte{0}
                && encoded[PAGE_IMAGE_OFFSET + 1] == std::byte{1}
                && encoded.back() == std::byte{0xFF},
            "CLR compensation image offset changed");
    require(minidb::decodeCompensationLogPayload(encoded) == payload,
            "CLR payload did not round-trip");

    const minidb::LogRecord record{
        minidb::LogRecordType::Compensation, 17, 8000, encoded,
        minidb::INVALID_LSN,
    };
    const auto outer = minidb::encodeWalRecord(record, 9000);
    require(minidb::byte_codec::readUint16(
                outer, minidb::wal_record_layout::TYPE_OFFSET) == 5
                && minidb::byte_codec::readUint64(
                    outer, minidb::wal_record_layout::TRANSACTION_ID_OFFSET) == 17
                && minidb::byte_codec::readUint64(
                    outer, minidb::wal_record_layout::PREVIOUS_LSN_OFFSET) == 8000,
            "CLR outer WAL identity/chain encoding changed");
    require(minidb::byte_codec::readUint32(
                outer, minidb::wal_record_layout::CHECKSUM_OFFSET) == 0x568F9E4AU,
            "Representative CLR outer CRC32C changed");
    const auto decoded = minidb::decodeWalRecord(outer, 9000);
    minidb::validateTransactionRecordPayload(decoded);
    require(decoded.type == minidb::LogRecordType::Compensation,
            "CLR outer WAL record did not round-trip as type 5");
}

void testClrValidation() {
    auto encoded = minidb::encodeCompensationLogPayload(representativePayload());
    const auto rejects = [](const std::vector<std::byte>& candidate) {
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(minidb::decodeCompensationLogPayload(candidate)); },
            "Malformed CLR payload was accepted");
    };
    auto corrupt = encoded;
    minidb::byte_codec::writeUint16(
        corrupt, minidb::compensation_log_layout::VERSION_OFFSET, 2);
    rejects(corrupt);
    corrupt = encoded;
    minidb::byte_codec::writeUint16(
        corrupt, minidb::compensation_log_layout::HEADER_SIZE_OFFSET, 41);
    rejects(corrupt);
    corrupt = encoded;
    minidb::byte_codec::writeUint32(
        corrupt, minidb::compensation_log_layout::FLAGS_OFFSET, 0x80U);
    rejects(corrupt);
    corrupt = encoded;
    minidb::byte_codec::writeUint64(
        corrupt, minidb::compensation_log_layout::RESERVED_OFFSET, 1);
    rejects(corrupt);
    corrupt.push_back(std::byte{0});
    rejects(corrupt);
}

void testDeterministicFuzzCandidates() {
    constexpr std::uint64_t SEED = 0xC1A0C1A0ULL;
    constexpr std::size_t CANDIDATES = 10'000;
    std::mt19937_64 random(SEED);
    std::uniform_int_distribution<std::size_t> sizeDistribution(0, 5000);
    for (std::size_t candidate = 0; candidate < CANDIDATES; ++candidate) {
        std::vector<std::byte> bytes;
        if (candidate % 10 == 0) {
            minidb::CompensationLogPayload payload;
            payload.pageId = static_cast<minidb::PageId>((random() % 1000) + 1);
            payload.pageSupportsLsn = (random() & 1U) != 0;
            payload.undoNextLsn = 64 + (random() % 10'000);
            payload.compensatedUpdateLsn = payload.undoNextLsn + 1 + (random() % 10'000);
            for (auto& value : payload.compensatedImage) {
                value = static_cast<std::byte>(random() & 0xFFU);
            }
            bytes = minidb::encodeCompensationLogPayload(payload);
        } else {
            bytes.resize(sizeDistribution(random));
            for (auto& value : bytes) value = static_cast<std::byte>(random() & 0xFFU);
        }
        try {
            const auto decoded = minidb::decodeCompensationLogPayload(bytes);
            const auto canonical = minidb::encodeCompensationLogPayload(decoded);
            require(canonical == bytes, "Accepted CLR fuzz input was not canonical");
        } catch (const minidb::WalError&) {
        }
    }
}

} // namespace

int main() {
    try {
        testExactClrEncoding();
        testClrValidation();
        testDeterministicFuzzCandidates();
        std::cout << "clr_codec_test passed (10000 candidates, seed 0xC1A0C1A0)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "clr_codec_test failed: " << error.what() << '\n';
        return 1;
    }
}
