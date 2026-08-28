#include "minidb/byte_codec.hpp"
#include "minidb/recovery_log.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

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

minidb::DiskManager::Page filledPage(std::byte value) {
    minidb::DiskManager::Page page{};
    page.fill(value);
    return page;
}

void requireRange(
    const minidb::PageByteRange& range,
    std::uint16_t offset,
    std::uint16_t length,
    std::string_view message) {
    require(range.offset == offset && range.length == length, message);
}

void testCanonicalPageDeltaComputation() {
    const minidb::DiskManager::Page zero{};
    auto changed = zero;
    require(minidb::computePageDelta(zero, changed).empty(),
            "Identical pages produced a delta");

    changed[0] = std::byte{1};
    auto ranges = minidb::computePageDelta(zero, changed);
    require(ranges.size() == 1, "First-byte change did not produce one range");
    requireRange(ranges[0], 0, 1, "First-byte range is incorrect");

    changed = zero;
    changed.back() = std::byte{2};
    ranges = minidb::computePageDelta(zero, changed);
    require(ranges.size() == 1, "Last-byte change did not produce one range");
    requireRange(ranges[0], 4095, 1, "Last-byte range is incorrect");

    changed = zero;
    for (std::size_t offset = 20; offset < 120; ++offset) changed[offset] = std::byte{3};
    for (std::size_t offset = 200; offset < 207; ++offset) changed[offset] = std::byte{4};
    ranges = minidb::computePageDelta(zero, changed);
    require(ranges.size() == 2, "Separated changes did not produce two ranges");
    requireRange(ranges[0], 20, 100, "Continuous range was not merged canonically");
    requireRange(ranges[1], 200, 7, "Second separated range is incorrect");

    changed = filledPage(std::byte{0xFF});
    ranges = minidb::computePageDelta(zero, changed);
    require(ranges.size() == 1, "Whole-page change was fragmented");
    requireRange(ranges[0], 0, 4096, "Whole-page range is incorrect");

    changed = zero;
    for (std::size_t offset = 0; offset < changed.size(); offset += 2) {
        changed[offset] = std::byte{0x5A};
    }
    ranges = minidb::computePageDelta(zero, changed);
    require(ranges.size() == minidb::page_delta_update_log_layout::MAX_RANGE_COUNT,
            "Alternating bytes did not produce maximum canonical range count");
    requireRange(ranges.front(), 0, 1, "First alternating range is incorrect");
    requireRange(ranges.back(), 4094, 1, "Last alternating range is incorrect");

    std::array<bool, minidb::database_format::PAGE_SIZE> required{};
    required[10] = true;
    required[11] = true;
    ranges = minidb::computePageDelta(zero, zero, required);
    require(ranges.size() == 1, "Required unchanged offsets were omitted");
    requireRange(ranges[0], 10, 2, "Required offsets did not merge canonically");
    require(std::all_of(ranges[0].beforeBytes.begin(), ranges[0].beforeBytes.end(),
                        [](std::byte byte) { return byte == std::byte{0}; })
                && ranges[0].beforeBytes == ranges[0].afterBytes,
            "Required reverted bytes were encoded incorrectly");
}

minidb::PageDeltaUpdateLogPayload representativeDelta(bool existing = true) {
    minidb::PageDeltaUpdateLogPayload payload;
    payload.pageId = 0x01020304U;
    payload.beforePageExisted = existing;
    payload.ranges.push_back(minidb::PageByteRange{
        0x0102U,
        2,
        {std::byte{0xAA}, std::byte{0xBB}},
        {std::byte{0xCC}, std::byte{0xDD}},
    });
    return payload;
}

minidb::AdaptivePageUpdateDecision decisionForPages(
    const minidb::DiskManager::Page& before,
    const minidb::DiskManager::Page& after,
    bool beforeExisted = true) {
    return minidb::selectAdaptivePageUpdateEncoding({
        7, beforeExisted, minidb::computePageDelta(before, after),
    });
}

void testAdaptiveSelectionExamplesAndBoundary() {
    const minidb::DiskManager::Page before{};
    auto after = before;
    after[17] = std::byte{1};
    require(decisionForPages(before, after).recordType
                == minidb::LogRecordType::PageDeltaUpdate,
            "Adaptive selector did not choose delta for one changed byte");

    after = before;
    std::fill(after.begin() + 100, after.begin() + 164, std::byte{2});
    require(decisionForPages(before, after).recordType
                == minidb::LogRecordType::PageDeltaUpdate,
            "Adaptive selector did not choose delta for a small contiguous run");

    after.fill(std::byte{0xFF});
    auto decision = decisionForPages(before, after);
    require(decision.recordType == minidb::LogRecordType::PageUpdate
                && decision.fullPageRecordBytes < decision.deltaRecordBytes,
            "Adaptive selector did not choose the exact smaller whole-page encoding");

    after = before;
    for (std::size_t offset = 0; offset < after.size(); offset += 2) {
        after[offset] = std::byte{0xA5};
    }
    decision = decisionForPages(before, after);
    require(decision.recordType == minidb::LogRecordType::PageUpdate
                && decision.rangeCount == 2048,
            "Adaptive selector did not choose full-page for fragmented changes");

    const auto contiguousDecision = [&](std::size_t length) {
        auto candidate = before;
        std::fill_n(candidate.begin(), length, std::byte{0x7F});
        return decisionForPages(before, candidate);
    };
    const auto below = contiguousDecision(4089);
    const auto tie = contiguousDecision(4090);
    const auto above = contiguousDecision(4091);
    require(below.deltaRecordBytes + 2 == below.fullPageRecordBytes
                && below.recordType == minidb::LogRecordType::PageDeltaUpdate,
            "Adaptive selector's below-boundary result is incorrect");
    require(tie.deltaRecordBytes == tie.fullPageRecordBytes
                && tie.isTie()
                && tie.recordType == minidb::LogRecordType::PageUpdate,
            "Adaptive exact-size tie did not prefer full-page");
    require(above.deltaRecordBytes == above.fullPageRecordBytes + 2
                && above.recordType == minidb::LogRecordType::PageUpdate,
            "Adaptive selector's above-boundary result is incorrect");

    const auto newPage = decisionForPages(before, filledPage(std::byte{0x33}), false);
    require(newPage.recordType == minidb::LogRecordType::PageDeltaUpdate,
            "Adaptive new-page selection did not use its one-image delta layout");
    const auto fragmentedNewPage = decisionForPages(before, after, false);
    require(fragmentedNewPage.recordType == minidb::LogRecordType::PageUpdate,
            "Adaptive fragmented new-page selection assumed delta always wins");
}

void testAdaptiveCandidateSizesMatchEncodedOutput() {
    constexpr std::uint64_t SEED = 0x11D20051ULL;
    std::mt19937_64 random(SEED);
    for (std::size_t candidate = 0; candidate < 2'000; ++candidate) {
        auto before = filledPage(static_cast<std::byte>(random() & 0xFFU));
        auto after = before;
        const auto begin = static_cast<std::size_t>(random() % after.size());
        const auto maximum = after.size() - begin;
        const auto length = 1U + static_cast<std::size_t>(random() % maximum);
        for (std::size_t offset = begin; offset < begin + length; ++offset) {
            after[offset] ^= static_cast<std::byte>(1U + (random() & 0xFFU));
        }
        const bool existed = (candidate % 3U) != 0;
        if (!existed) before.fill(std::byte{0});
        const minidb::PageDeltaUpdateLogPayload delta{
            9, existed, minidb::computePageDelta(before, after),
        };
        const auto decision = minidb::selectAdaptivePageUpdateEncoding(delta);
        const auto encodedDelta = minidb::encodePageDeltaUpdateLogPayload(delta);
        const auto encodedFull = minidb::encodePageUpdateLogPayload({
            9, existed, before, after,
        });
        require(decision.fullPageRecordBytes
                    == minidb::wal_record_layout::HEADER_SIZE + encodedFull.size()
                    && decision.deltaRecordBytes
                    == minidb::wal_record_layout::HEADER_SIZE + encodedDelta.size()
                    && minidb::pageDeltaUpdatePayloadEncodedSize(delta)
                    == encodedDelta.size()
                    && minidb::pageDeltaUpdateRecordEncodedSize(delta)
                    == decision.deltaRecordBytes,
                "Adaptive calculated candidate size drifted from encoded output (seed 0x11D20051)");
    }
}

void testRandomizedAdaptiveMinimumInvariant() {
    constexpr std::uint64_t SEED = 0x11D2A11ULL;
    constexpr std::size_t PAIRS = 10'000;
    std::mt19937_64 random(SEED);
    for (std::size_t candidate = 0; candidate < PAIRS; ++candidate) {
        minidb::DiskManager::Page before{};
        minidb::DiskManager::Page after{};
        for (std::size_t offset = 0; offset < before.size(); ++offset) {
            before[offset] = static_cast<std::byte>(random() & 0xFFU);
        }
        after = before;
        switch (candidate % 5U) {
        case 0: {
            const auto mutations = 1U + static_cast<unsigned>(random() % 32U);
            for (unsigned index = 0; index < mutations; ++index) {
                const auto offset = static_cast<std::size_t>(random() % after.size());
                after[offset] ^= std::byte{0x5A};
            }
            break;
        }
        case 1: {
            const auto begin = static_cast<std::size_t>(random() % after.size());
            const auto length = 1U + static_cast<std::size_t>(
                random() % (after.size() - begin));
            for (std::size_t offset = begin; offset < begin + length; ++offset) {
                after[offset] ^= std::byte{0x33};
            }
            break;
        }
        case 2: {
            const auto threshold = static_cast<unsigned>(random() % 101U);
            for (auto& byte : after) {
                if ((random() % 100U) < threshold) byte ^= std::byte{0xC3};
            }
            break;
        }
        case 3:
            for (std::size_t offset = candidate & 1U; offset < after.size(); offset += 2) {
                after[offset] ^= std::byte{0xA5};
            }
            break;
        case 4:
            for (auto& byte : after) byte ^= std::byte{0xFF};
            break;
        }
        if (before == after) after[candidate % after.size()] ^= std::byte{1};
        const minidb::PageDeltaUpdateLogPayload delta{
            11, true, minidb::computePageDelta(before, after),
        };
        const auto decision = minidb::selectAdaptivePageUpdateEncoding(delta);
        const auto chosen = decision.recordType == minidb::LogRecordType::PageUpdate
            ? decision.fullPageRecordBytes : decision.deltaRecordBytes;
        require(chosen <= decision.fullPageRecordBytes
                    && chosen <= decision.deltaRecordBytes,
                "Adaptive minimum-size invariant failed (seed 0x11D2A11)");
        require(!decision.isTie()
                    || decision.recordType == minidb::LogRecordType::PageUpdate,
                "Adaptive randomized tie did not prefer full-page (seed 0x11D2A11)");
    }
}

void testExactDeltaPayloadAndWalCodec() {
    const auto payload = representativeDelta();
    const auto encoded = minidb::encodePageDeltaUpdateLogPayload(payload);
    require(encoded.size() == 32, "Representative delta payload size changed");
    require(minidb::byte_codec::readUint32(encoded, 0) == 0x01020304U
                && minidb::byte_codec::readUint32(encoded, 4) == 1
                && minidb::byte_codec::readUint32(encoded, 8) == 4096
                && minidb::byte_codec::readUint32(encoded, 12) == 1
                && minidb::byte_codec::readUint16(encoded, 16) == 1
                && minidb::byte_codec::readUint16(encoded, 18) == 24
                && minidb::byte_codec::readUint32(encoded, 20) == 0,
            "Delta payload header is not exact little-endian encoding");
    require(minidb::byte_codec::readUint16(encoded, 24) == 0x0102U
                && minidb::byte_codec::readUint16(encoded, 26) == 2
                && encoded[28] == std::byte{0xAA}
                && encoded[29] == std::byte{0xBB}
                && encoded[30] == std::byte{0xCC}
                && encoded[31] == std::byte{0xDD},
            "Delta range descriptor/data encoding changed");
    require(minidb::decodePageDeltaUpdateLogPayload(encoded) == payload,
            "Existing-page delta payload did not round-trip");

    auto newPagePayload = representativeDelta(false);
    newPagePayload.ranges[0].beforeBytes.assign(2, std::byte{0});
    const auto newPageEncoded = minidb::encodePageDeltaUpdateLogPayload(newPagePayload);
    require(newPageEncoded.size() == 30
                && newPageEncoded[28] == std::byte{0xCC}
                && newPageEncoded[29] == std::byte{0xDD},
            "New-page delta encoded meaningless before bytes");
    require(minidb::decodePageDeltaUpdateLogPayload(newPageEncoded) == newPagePayload,
            "New-page delta payload did not reconstruct zero before bytes");

    minidb::LogRecord record{
        minidb::LogRecordType::PageDeltaUpdate,
        0x0102030405060708ULL,
        64,
        encoded,
        minidb::INVALID_LSN,
    };
    const auto walBytes = minidb::encodeWalRecord(record, 128);
    require(walBytes.size() == 80
                && minidb::byte_codec::readUint16(walBytes, 6) == 8
                && minidb::byte_codec::readUint32(walBytes, 12) == 32,
            "PAGE_DELTA_UPDATE outer record layout/type changed");
    require(minidb::byte_codec::readUint32(walBytes, 40) == 0x037DC09EU,
            "PAGE_DELTA_UPDATE representative CRC32C changed");
    const auto decodedRecord = minidb::decodeWalRecord(walBytes, 128);
    minidb::validateTransactionRecordPayload(decodedRecord);
    require(decodedRecord.type == minidb::LogRecordType::PageDeltaUpdate
                && minidb::decodePageDeltaUpdateLogPayload(decodedRecord.payload) == payload,
            "PAGE_DELTA_UPDATE did not round-trip through the WAL codec");
}

void expectMalformed(std::vector<std::byte> bytes, std::string_view message) {
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::decodePageDeltaUpdateLogPayload(bytes)); },
        message);
}

void testMalformedDeltaPayloads() {
    const auto valid = minidb::encodePageDeltaUpdateLogPayload(representativeDelta());
    for (const auto offset : {std::size_t{4}, std::size_t{8}, std::size_t{16},
                              std::size_t{18}, std::size_t{20}}) {
        auto bytes = valid;
        bytes[offset] ^= std::byte{0x80};
        expectMalformed(std::move(bytes), "Malformed delta fixed-header field was accepted");
    }
    auto bytes = valid;
    minidb::byte_codec::writeUint32(bytes, 12, 0xFFFFFFFFU);
    expectMalformed(std::move(bytes), "Huge delta range count was accepted");

    bytes = valid;
    minidb::byte_codec::writeUint16(bytes, 26, 0);
    expectMalformed(std::move(bytes), "Zero-length delta range was accepted");
    bytes = valid;
    minidb::byte_codec::writeUint16(bytes, 24, 4095);
    minidb::byte_codec::writeUint16(bytes, 26, 2);
    expectMalformed(std::move(bytes), "Out-of-page delta range was accepted");

    auto twoRanges = representativeDelta();
    twoRanges.ranges.push_back(minidb::PageByteRange{
        300, 1, {std::byte{1}}, {std::byte{2}}});
    const auto canonical = minidb::encodePageDeltaUpdateLogPayload(twoRanges);
    bytes = canonical;
    minidb::byte_codec::writeUint16(bytes, 32, 260);
    expectMalformed(std::move(bytes), "Adjacent delta ranges were accepted");
    bytes = canonical;
    minidb::byte_codec::writeUint16(bytes, 32, 259);
    expectMalformed(std::move(bytes), "Overlapping delta ranges were accepted");
    bytes = canonical;
    minidb::byte_codec::writeUint16(bytes, 32, 1);
    expectMalformed(std::move(bytes), "Out-of-order delta ranges were accepted");

    bytes = valid;
    bytes.pop_back();
    expectMalformed(std::move(bytes), "Truncated delta after data was accepted");
    bytes = valid;
    bytes.erase(bytes.begin() + 28);
    expectMalformed(std::move(bytes), "Truncated delta before data was accepted");
    bytes = valid;
    bytes.push_back(std::byte{0});
    expectMalformed(std::move(bytes), "Trailing delta data was accepted");
}

void testDirectPhysicalRedoUndo() {
    auto original = filledPage(std::byte{0x11});
    auto updated = original;
    for (std::size_t offset = 0; offset < updated.size(); ++offset) {
        if ((offset % 7) == 0 || (offset >= 2000 && offset < 3000)) {
            updated[offset] = static_cast<std::byte>((offset * 31U) & 0xFFU);
        }
    }
    const minidb::PageDeltaUpdateLogPayload payload{
        7, true, minidb::computePageDelta(original, updated)};
    auto recoveryPage = original;
    minidb::applyPageDeltaAfter(recoveryPage, payload);
    require(recoveryPage == updated, "Physical delta REDO did not reproduce all 4096 bytes");
    minidb::applyPageDeltaBefore(recoveryPage, payload);
    require(recoveryPage == original, "Physical delta UNDO did not reproduce all 4096 bytes");
}

void testDeterministicDeltaDecoderFuzz() {
    constexpr std::uint64_t SEED = 0x11D10001ULL;
    constexpr std::size_t CANDIDATES = 10'000;
    std::mt19937_64 random(SEED);
    for (std::size_t candidate = 0; candidate < CANDIDATES; ++candidate) {
        minidb::DiskManager::Page before{};
        minidb::DiskManager::Page after{};
        for (std::size_t offset = 0; offset < after.size(); ++offset) {
            before[offset] = static_cast<std::byte>(random() & 0xFFU);
            after[offset] = before[offset];
        }
        const auto mutationCount = 1U + static_cast<unsigned>(random() % 48U);
        for (unsigned mutation = 0; mutation < mutationCount; ++mutation) {
            const auto offset = static_cast<std::size_t>(random() % after.size());
            after[offset] ^= static_cast<std::byte>(1U + (random() & 0xFFU));
        }
        if (before == after) after[0] ^= std::byte{1};
        minidb::PageDeltaUpdateLogPayload payload{
            static_cast<minidb::PageId>(1U + (random() & 0xFFFFU)),
            true,
            minidb::computePageDelta(before, after),
        };
        auto encoded = minidb::encodePageDeltaUpdateLogPayload(payload);
        if ((candidate % 3U) == 0U) {
            const auto offset = static_cast<std::size_t>(random() % encoded.size());
            encoded[offset] ^= static_cast<std::byte>(1U << (random() % 8U));
            try {
                const auto decoded = minidb::decodePageDeltaUpdateLogPayload(encoded);
                auto page = before;
                minidb::applyPageDeltaAfter(page, decoded);
            } catch (const minidb::WalError&) {
                // Safe rejection is the expected outcome for most mutated candidates.
            } catch (const std::runtime_error&) {
                // Low-level bounded codec failures are also controlled rejection.
            }
        } else {
            require(minidb::decodePageDeltaUpdateLogPayload(encoded) == payload,
                    "Valid fuzz-generated delta failed round-trip (seed 0x11D10001)");
        }
    }
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

void testPageLsnAwarePayloadsAndAdaptiveBoundary() {
    minidb::PageUpdateV2LogPayload full;
    full.pageId = 0x01020304U;
    full.beforePageExisted = true;
    full.beforePageLsn = 0x0102030405060708ULL;
    full.beforeImage[0] = std::byte{0xA1};
    full.afterImage[4095] = std::byte{0xB2};
    const auto encodedFull = minidb::encodePageUpdateV2LogPayload(full);
    require(encodedFull.size() == minidb::page_update_v2_log_layout::PAYLOAD_SIZE
                && encodedFull.size() == 8216,
            "PAGE_UPDATE_V2 payload size changed");
    require(minidb::byte_codec::readUint16(
                encodedFull, minidb::page_update_v2_log_layout::VERSION_OFFSET) == 1
                && minidb::byte_codec::readUint16(
                    encodedFull, minidb::page_update_v2_log_layout::HEADER_SIZE_OFFSET) == 24
                && minidb::byte_codec::readUint64(
                    encodedFull,
                    minidb::page_update_v2_log_layout::BEFORE_PAGE_LSN_OFFSET)
                    == full.beforePageLsn,
            "PAGE_UPDATE_V2 header or beforePageLsn encoding is incorrect");
    require(minidb::decodePageUpdateV2LogPayload(encodedFull) == full,
            "PAGE_UPDATE_V2 payload did not round-trip");

    minidb::PageDeltaUpdateV2LogPayload delta;
    delta.pageId = 9;
    delta.beforePageExisted = true;
    delta.beforePageLsn = 777;
    delta.ranges.push_back(minidb::PageByteRange{
        100, 2,
        {std::byte{0x11}, std::byte{0x22}},
        {std::byte{0x33}, std::byte{0x44}},
    });
    const auto encodedDelta = minidb::encodePageDeltaUpdateV2LogPayload(delta);
    require(encodedDelta.size() == 40
                && minidb::byte_codec::readUint64(
                    encodedDelta,
                    minidb::page_delta_update_v2_log_layout::BEFORE_PAGE_LSN_OFFSET) == 777,
            "PAGE_DELTA_UPDATE_V2 exact encoding is incorrect");
    require(minidb::decodePageDeltaUpdateV2LogPayload(encodedDelta) == delta,
            "PAGE_DELTA_UPDATE_V2 payload did not round-trip");

    minidb::DiskManager::Page before{};
    const auto decision = [&](std::size_t length) {
        auto after = before;
        std::fill_n(after.begin(), length, std::byte{0x5A});
        return minidb::selectAdaptivePageUpdateV2Encoding({
            11, true, 64, minidb::computePageDelta(before, after),
        });
    };
    require(decision(4089).recordType == minidb::LogRecordType::PageDeltaUpdateV2,
            "V2 Adaptive selector missed the below-tie delta");
    const auto tie = decision(4090);
    require(tie.isTie() && tie.recordType == minidb::LogRecordType::PageUpdateV2,
            "V2 Adaptive exact-size tie did not choose full-page");
    require(decision(4091).recordType == minidb::LogRecordType::PageUpdateV2,
            "V2 Adaptive selector missed the above-tie full page");

    minidb::test::requireThrows<minidb::WalError>(
        [] {
            static_cast<void>(minidb::encodePageUpdateV2LogPayload({
                1, false, 99, {}, {},
            }));
        },
        "New PAGE_UPDATE_V2 accepted a beforePageLsn");
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
        testCanonicalPageDeltaComputation();
        testExactDeltaPayloadAndWalCodec();
        testAdaptiveSelectionExamplesAndBoundary();
        testAdaptiveCandidateSizesMatchEncodedOutput();
        testRandomizedAdaptiveMinimumInvariant();
        testMalformedDeltaPayloads();
        testDirectPhysicalRedoUndo();
        testDeterministicDeltaDecoderFuzz();
        testMalformedPayloads();
        testPageLsnAwarePayloadsAndAdaptiveBoundary();
        testCommitAndAbortHaveExactEmptyPayloads();
        std::cout << "recovery_log_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery_log_test failed: " << error.what() << '\n';
        return 1;
    }
}
