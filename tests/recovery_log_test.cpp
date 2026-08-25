#include "minidb/byte_codec.hpp"
#include "minidb/recovery_log.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <iostream>

namespace {

using minidb::test::require;

void testBeginPayload() {
    const auto encoded = minidb::encodeBeginLogPayload({0x01020304ULL});
    require(encoded.size() == 16, "BEGIN payload size is not 16 bytes");
    require(encoded[0] == std::byte{0x04} && encoded[3] == std::byte{0x01}
                && encoded[7] == std::byte{0},
            "BEGIN start page count is not little-endian");
    require(std::all_of(encoded.begin() + 8, encoded.end(),
                        [](std::byte value) { return value == std::byte{0}; }),
            "BEGIN reserved bytes are not zero");
    require(minidb::decodeBeginLogPayload(encoded).startPageCount
                == 0x01020304ULL,
            "BEGIN payload did not round-trip");
}

void testPageUpdatePayload() {
    minidb::PageUpdateLogPayload payload;
    payload.pageId = 0x01020304U;
    payload.beforePageExisted = true;
    payload.beforeImage[0] = std::byte{0xA1};
    payload.beforeImage.back() = std::byte{0xA2};
    payload.afterImage[0] = std::byte{0xB1};
    payload.afterImage.back() = std::byte{0xB2};
    const auto encoded = minidb::encodePageUpdateLogPayload(payload);
    require(encoded.size() == 8208, "PAGE_UPDATE payload size is not 8208 bytes");
    require(encoded[0] == std::byte{0x04} && encoded[3] == std::byte{0x01},
            "PAGE_UPDATE PageId is not little-endian");
    require(minidb::byte_codec::readUint32(encoded, 4) == 1
                && minidb::byte_codec::readUint32(encoded, 8) == 4096
                && minidb::byte_codec::readUint32(encoded, 12) == 0,
            "PAGE_UPDATE fixed header is incorrect");
    require(encoded[16] == std::byte{0xA1} && encoded[4111] == std::byte{0xA2}
                && encoded[4112] == std::byte{0xB1} && encoded[8207] == std::byte{0xB2},
            "PAGE_UPDATE images use incorrect offsets");
    require(minidb::decodePageUpdateLogPayload(encoded) == payload,
            "PAGE_UPDATE payload did not round-trip");
}

void testMalformedPayloads() {
    minidb::PageUpdateLogPayload payload;
    payload.pageId = 1;
    payload.beforeImage[0] = std::byte{1};
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::encodePageUpdateLogPayload(payload)); },
        "New page accepted a nonzero before-image");
    auto encoded = minidb::encodePageUpdateLogPayload(
        minidb::PageUpdateLogPayload{1, false, {}, {}});
    minidb::byte_codec::writeUint32(encoded, 12, 1);
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::decodePageUpdateLogPayload(encoded)); },
        "PAGE_UPDATE accepted nonzero reserved bytes");
}

void testCommitAndAbortHaveExactEmptyPayloads() {
    for (const auto type : {minidb::LogRecordType::Commit, minidb::LogRecordType::Abort}) {
        minidb::LogRecord record{type, 9, 64, {}, minidb::INVALID_LSN};
        const auto encoded = minidb::encodeWalRecord(record, 128);
        require(minidb::byte_codec::readUint32(
                    encoded, minidb::wal_record_layout::PAYLOAD_LENGTH_OFFSET) == 0
                    && encoded.size() == minidb::wal_record_layout::HEADER_SIZE,
                "COMMIT/ABORT record did not encode an exact empty payload");
        const auto decoded = minidb::decodeWalRecord(encoded, 128);
        minidb::validateTransactionRecordPayload(decoded);
        require(decoded.type == type && decoded.payload.empty(),
                "COMMIT/ABORT empty payload did not round-trip through CRC record codec");
    }
}

} // namespace

int main() {
    try {
        testBeginPayload();
        testPageUpdatePayload();
        testMalformedPayloads();
        testCommitAndAbortHaveExactEmptyPayloads();
        std::cout << "recovery_log_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_log_test failed: " << error.what() << '\n';
        return 1;
    }
}
