#include "minidb/byte_codec.hpp"
#include "minidb/wal.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using minidb::test::require;

class TemporaryWal {
public:
    explicit TemporaryWal(std::string_view name)
        : database_(name), path_(minidb::walPathForDatabase(database_.path().string())) {}
    ~TemporaryWal() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    minidb::test::TemporaryDatabase database_;
    std::string path_;
};

void writeFile(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Could not create scanner test WAL");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Could not write scanner test WAL");
}

std::vector<std::byte> makeWal(const std::vector<std::size_t>& payloadSizes) {
    const auto header = minidb::encodeWalFileHeader();
    std::vector<std::byte> bytes(header.begin(), header.end());
    minidb::Lsn previous = minidb::INVALID_LSN;
    for (std::size_t index = 0; index < payloadSizes.size(); ++index) {
        minidb::LogRecord record;
        record.type = minidb::LogRecordType::PageUpdate;
        record.transactionId = 7;
        record.prevLsn = previous;
        record.payload.assign(payloadSizes[index], static_cast<std::byte>(index + 1));
        const auto lsn = static_cast<minidb::Lsn>(bytes.size());
        const auto encoded = minidb::encodeWalRecord(record, lsn);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        previous = lsn;
    }
    return bytes;
}

void requireCorruptRecord(
    std::string_view name,
    const std::function<void(std::vector<std::byte>&)>& mutate) {
    TemporaryWal wal(name);
    auto bytes = makeWal({8, 9});
    mutate(bytes);
    writeFile(wal.path(), bytes);
    minidb::test::requireThrows<minidb::WalError>(
        [&] { static_cast<void>(minidb::scanWalFile(wal.path())); },
        "Corrupt WAL record was accepted by scanner");
}

void testHeaderAndRecordCorruption() {
    for (const auto offset : {std::size_t{0}, std::size_t{8}, std::size_t{12},
                              std::size_t{16}, std::size_t{20}, std::size_t{24}}) {
        TemporaryWal wal("scanner_header_" + std::to_string(offset));
        auto bytes = makeWal({4});
        bytes[offset] ^= std::byte{1};
        writeFile(wal.path(), bytes);
        minidb::test::requireThrows<minidb::WalError>(
            [&] { static_cast<void>(minidb::scanWalFile(wal.path())); },
            "Scanner accepted corrupt WAL file header");
    }

    requireCorruptRecord("scanner_magic", [](auto& bytes) { bytes[64] ^= std::byte{1}; });
    requireCorruptRecord("scanner_version", [](auto& bytes) { bytes[68] ^= std::byte{1}; });
    requireCorruptRecord("scanner_type", [](auto& bytes) {
        minidb::byte_codec::writeUint16(bytes, 70, 99);
    });
    requireCorruptRecord("scanner_lsn", [](auto& bytes) {
        minidb::byte_codec::writeUint64(bytes, 80, 65);
    });
    requireCorruptRecord("scanner_flags", [](auto& bytes) {
        minidb::byte_codec::writeUint32(bytes, 108, 1);
    });
    requireCorruptRecord("scanner_checksum", [](auto& bytes) { bytes[104] ^= std::byte{1}; });
    requireCorruptRecord("scanner_interior_payload", [](auto& bytes) { bytes[113] ^= std::byte{1}; });
}

void testTruncatedFinalRecordBoundaries() {
    const auto complete = makeWal({5, 7, 13});
    const auto firstLength = 48 + 5;
    const auto secondLength = 48 + 7;
    const auto thirdOffset = 64 + firstLength + secondLength;
    for (const auto retained : {std::size_t{1}, std::size_t{20}, std::size_t{47},
                                std::size_t{48}, std::size_t{55}}) {
        TemporaryWal wal("scanner_tail_" + std::to_string(retained));
        auto truncated = complete;
        truncated.resize(thirdOffset + retained);
        writeFile(wal.path(), truncated);
        const auto result = minidb::scanWalFile(wal.path());
        require(result.truncatedTail && result.records.size() == 2
                    && result.validBytes == thirdOffset,
                "Scanner did not isolate a partial final record at its last complete boundary");
    }
}

void testMaliciousLengthsRejectedBeforeAllocation() {
    requireCorruptRecord("scanner_small_total", [](auto& bytes) {
        minidb::byte_codec::writeUint32(bytes, 72, 47);
    });
    requireCorruptRecord("scanner_huge_total", [](auto& bytes) {
        minidb::byte_codec::writeUint32(bytes, 72, 0xFFFFFFFFU);
    });
    requireCorruptRecord("scanner_huge_payload", [](auto& bytes) {
        minidb::byte_codec::writeUint32(bytes, 76, 0xFFFFFFFFU);
    });
    requireCorruptRecord("scanner_length_mismatch", [](auto& bytes) {
        minidb::byte_codec::writeUint32(bytes, 76, 2);
    });
    requireCorruptRecord("scanner_max_plus_one", [](auto& bytes) {
        minidb::byte_codec::writeUint32(
            bytes,
            72,
            static_cast<std::uint32_t>(minidb::wal_record_layout::MAX_RECORD_SIZE + 1));
        minidb::byte_codec::writeUint32(
            bytes,
            76,
            static_cast<std::uint32_t>(minidb::wal_record_layout::MAX_PAYLOAD_SIZE + 1));
    });
}

void testDeterministicFuzzLikeDecode() {
    constexpr std::uint32_t SEED = 0x11A0F00DU;
    constexpr std::size_t CASES = 10'000;
    std::mt19937 random(SEED);
    std::size_t decoded = 0;
    for (std::size_t index = 0; index < CASES; ++index) {
        if (index % 97 == 0) {
            minidb::LogRecord record;
            record.type = minidb::LogRecordType::Begin;
            record.transactionId = index + 1;
            record.payload.resize(random() % 1024U, std::byte{0x5A});
            const auto encoded = minidb::encodeWalRecord(record, 64);
            static_cast<void>(minidb::decodeWalRecord(encoded, 64));
            ++decoded;
            continue;
        }
        std::vector<std::byte> candidate(random() % 2049U);
        for (auto& byte : candidate) {
            byte = static_cast<std::byte>(random() & 0xFFU);
        }
        try {
            static_cast<void>(minidb::decodeWalRecord(candidate, 64));
            ++decoded;
        } catch (const std::exception&) {
        }
    }
    require(decoded >= CASES / 97,
            "Fuzz-like WAL decode did not exercise generated valid records");
}

} // namespace

int main() {
    try {
        testHeaderAndRecordCorruption();
        testTruncatedFinalRecordBoundaries();
        testMaliciousLengthsRejectedBeforeAllocation();
        testDeterministicFuzzLikeDecode();
        std::cout << "wal_scanner_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wal_scanner_test failed: " << error.what() << '\n';
        return 1;
    }
}
