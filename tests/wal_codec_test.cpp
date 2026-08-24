#include "minidb/byte_codec.hpp"
#include "minidb/wal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using minidb::test::require;

void testExactFileHeader() {
    const auto header = minidb::encodeWalFileHeader();
    require(header.size() == 64, "WAL header size changed");
    require(std::equal(
                minidb::wal_file_layout::MAGIC.begin(),
                minidb::wal_file_layout::MAGIC.end(),
                header.begin()),
            "WAL header magic bytes changed");
    require(minidb::byte_codec::readUint32(header, 8) == 1,
            "WAL header version bytes changed");
    require(minidb::byte_codec::readUint32(header, 12) == 64,
            "WAL header-size bytes changed");
    require(minidb::byte_codec::readUint32(header, 16) == 4096,
            "WAL page-size bytes changed");
    require(std::all_of(
                header.begin() + 20,
                header.end(),
                [](std::byte value) { return value == std::byte{0}; }),
            "WAL header reserved bytes are not canonical zeroes");
    minidb::validateWalFileHeader(header);

    for (const auto offset : {std::size_t{0}, std::size_t{8}, std::size_t{12},
                              std::size_t{16}, std::size_t{20}, std::size_t{24}}) {
        auto corrupt = header;
        corrupt[offset] ^= std::byte{1};
        minidb::test::requireThrows<minidb::WalError>(
            [&] { minidb::validateWalFileHeader(corrupt); },
            "Corrupt WAL file header field was accepted");
    }
}

void testCrc32cVectors() {
    require(minidb::crc32c({}) == 0, "CRC32C empty vector changed");
    constexpr std::string_view text = "123456789";
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    require(minidb::crc32c(bytes) == 0xE3069283U,
            "CRC32C Castagnoli known vector changed");
}

void testExactRecordLayoutAndCorruption() {
    minidb::LogRecord record;
    record.type = minidb::LogRecordType::PageUpdate;
    record.transactionId = 0x0102030405060708ULL;
    record.prevLsn = 64;
    record.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    constexpr minidb::Lsn LSN = 128;
    const auto encoded = minidb::encodeWalRecord(record, LSN);

    require(encoded.size() == 51, "Representative WAL record length changed");
    require(std::equal(
                minidb::wal_record_layout::MAGIC.begin(),
                minidb::wal_record_layout::MAGIC.end(),
                encoded.begin()),
            "WAL record magic bytes changed");
    require(minidb::byte_codec::readUint16(encoded, 4) == 1
                && minidb::byte_codec::readUint16(encoded, 6) == 2,
            "WAL version/type bytes changed");
    require(minidb::byte_codec::readUint32(encoded, 8) == 51
                && minidb::byte_codec::readUint32(encoded, 12) == 3,
            "WAL record length bytes changed");
    require(minidb::byte_codec::readUint64(encoded, 16) == LSN
                && minidb::byte_codec::readUint64(encoded, 24)
                    == 0x0102030405060708ULL
                && minidb::byte_codec::readUint64(encoded, 32) == 64,
            "WAL record identity/chain bytes changed");
    require(minidb::byte_codec::readUint32(encoded, 44) == 0,
            "WAL record flags are not canonical zero");
    require(encoded[48] == std::byte{0xAA} && encoded[50] == std::byte{0xCC},
            "WAL payload offset changed");
    const auto checksum = minidb::byte_codec::readUint32(encoded, 40);
    require(checksum == 0xD10F1319U,
            "Representative WAL record CRC32C bytes changed");

    auto decoded = minidb::decodeWalRecord(encoded, LSN);
    record.lsn = LSN;
    require(decoded == record, "WAL record round trip changed logical fields");

    for (const auto offset : {std::size_t{5}, std::size_t{24}, std::size_t{48}}) {
        auto corrupt = encoded;
        corrupt[offset] ^= std::byte{1};
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(minidb::decodeWalRecord(corrupt, LSN)); },
            "Header/payload bit corruption escaped the WAL checksum");
    }
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::decodeWalRecord(encoded, LSN + 1)); },
        "WAL decoder accepted an LSN/physical-offset mismatch");
}

void testLimitsAndLsnBoundaries() {
    minidb::LogRecord record;
    record.type = minidb::LogRecordType::Begin;
    record.payload.resize(minidb::wal_record_layout::MAX_PAYLOAD_SIZE);
    require(
        minidb::encodeWalRecord(record, minidb::wal_file_layout::HEADER_SIZE).size()
            == minidb::wal_record_layout::MAX_RECORD_SIZE,
        "Maximum WAL record was rejected");
    record.payload.push_back(std::byte{0});
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::encodeWalRecord(record, 64)); },
        "Oversized WAL record was accepted");
    record.payload.clear();
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::encodeWalRecord(record, minidb::INVALID_LSN)); },
        "INVALID_LSN was accepted as a record offset");
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::encodeWalRecord(record, 63)); },
        "LSN inside the WAL file header was accepted");
    const auto large = std::numeric_limits<minidb::Lsn>::max() - 4096;
    require(minidb::byte_codec::readUint64(
                minidb::encodeWalRecord(record, large),
                minidb::wal_record_layout::LSN_OFFSET) == large,
            "Large unsigned LSN did not encode exactly");
}

} // namespace

int main() {
    try {
        testExactFileHeader();
        testCrc32cVectors();
        testExactRecordLayoutAndCorruption();
        testLimitsAndLsnBoundaries();
        std::cout << "wal_codec_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wal_codec_test failed: " << error.what() << '\n';
        return 1;
    }
}
