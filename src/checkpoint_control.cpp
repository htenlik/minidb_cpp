#include "minidb/checkpoint_control.hpp"

#include "minidb/byte_codec.hpp"
#include "minidb/log_manager.hpp"
#include "minidb/recovery.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace minidb {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    [[nodiscard]] int get() const noexcept { return value_; }
private:
    int value_;
};

[[noreturn]] void throwIo(const std::string& operation) {
    throw WalError(WalErrorKind::Io, operation + ": " + std::strerror(errno));
}

void readExact(int descriptor, std::uint64_t offset, std::span<std::byte> output) {
    std::size_t completed = 0;
    while (completed < output.size()) {
        const auto count = ::pread(descriptor, output.data() + completed,
                                   output.size() - completed,
                                   static_cast<off_t>(offset + completed));
        if (count < 0) { if (errno == EINTR) continue; throwIo("Could not read checkpoint control"); }
        if (count == 0) throw WalError(WalErrorKind::Io, "Unexpected checkpoint-control EOF");
        completed += static_cast<std::size_t>(count);
    }
}

void writeExact(int descriptor, std::uint64_t offset, std::span<const std::byte> bytes,
                CheckpointControlStats& stats) {
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        const auto count = ::pwrite(descriptor, bytes.data() + completed,
                                    bytes.size() - completed,
                                    static_cast<off_t>(offset + completed));
        if (count < 0) { if (errno == EINTR) continue; throwIo("Could not write checkpoint control"); }
        if (count == 0) throw WalError(WalErrorKind::Io, "Checkpoint-control write made no progress");
        completed += static_cast<std::size_t>(count);
        stats.bytesWritten += static_cast<std::uint64_t>(count);
    }
}

std::uint64_t fileSize(int descriptor) {
    struct stat value {};
    if (::fstat(descriptor, &value) != 0) throwIo("Could not inspect checkpoint control");
    return value.st_size < 0 ? 0 : static_cast<std::uint64_t>(value.st_size);
}

std::uint32_t slotChecksum(std::span<const std::byte> bytes) {
    std::array<std::byte, checkpoint_slot_layout::SIZE> copy{};
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    std::fill_n(copy.begin() + checkpoint_slot_layout::CHECKSUM_OFFSET, 4, std::byte{0});
    return crc32c(copy);
}

bool crossValidate(const CheckpointSlot& slot, const LogManager& log) {
    if (slot.walFileSizeAtCheckpoint < slot.recoveryStartOffset
        || slot.recoveryStartOffset > log.lastValidOffset()) return false;
    const auto endRecord = log.readRecordAt(slot.checkpointEndLsn);
    if (endRecord.lsn != slot.checkpointEndLsn
        || endRecord.type != LogRecordType::CheckpointEnd) return false;
    validateCheckpointRecord(endRecord);
    const auto end = decodeCheckpointEndLogPayload(endRecord.payload);
    const auto encodedEndSize = wal_record_layout::HEADER_SIZE + endRecord.payload.size();
    if (end.checkpointId != slot.checkpointId
        || end.recoveryStartOffset != slot.recoveryStartOffset
        || end.databasePageCount != slot.databasePageCount
        || end.nextTransactionId != slot.nextTransactionId
        || endRecord.lsn + encodedEndSize != slot.recoveryStartOffset) return false;
    const auto beginRecord = log.readRecordAt(end.checkpointBeginLsn);
    if (beginRecord.lsn != end.checkpointBeginLsn
        || beginRecord.type != LogRecordType::CheckpointBegin) return false;
    validateCheckpointRecord(beginRecord);
    const auto begin = decodeCheckpointBeginLogPayload(beginRecord.payload);
    return begin.checkpointId == slot.checkpointId;
}

} // namespace

std::string checkpointPathForDatabase(std::string_view databasePath) {
    return std::string(databasePath) + ".ckpt";
}

std::array<std::byte, checkpoint_control_layout::HEADER_SIZE>
encodeCheckpointControlHeader() {
    std::array<std::byte, checkpoint_control_layout::HEADER_SIZE> bytes{};
    std::copy(checkpoint_control_layout::MAGIC.begin(), checkpoint_control_layout::MAGIC.end(),
              bytes.begin());
    byte_codec::writeUint32(bytes, checkpoint_control_layout::VERSION_OFFSET,
                            checkpoint_control_layout::CURRENT_VERSION);
    byte_codec::writeUint32(bytes, checkpoint_control_layout::HEADER_SIZE_OFFSET,
                            checkpoint_control_layout::HEADER_SIZE);
    byte_codec::writeUint32(bytes, checkpoint_control_layout::SLOT_SIZE_OFFSET,
                            checkpoint_control_layout::SLOT_SIZE);
    byte_codec::writeUint32(bytes, checkpoint_control_layout::SLOT_COUNT_OFFSET,
                            checkpoint_control_layout::SLOT_COUNT);
    return bytes;
}

void validateCheckpointControlHeader(std::span<const std::byte> bytes) {
    using namespace checkpoint_control_layout;
    if (bytes.size() != HEADER_SIZE
        || !std::equal(MAGIC.begin(), MAGIC.end(), bytes.begin() + MAGIC_OFFSET)
        || byte_codec::readUint32(bytes, VERSION_OFFSET) != CURRENT_VERSION
        || byte_codec::readUint32(bytes, HEADER_SIZE_OFFSET) != HEADER_SIZE
        || byte_codec::readUint32(bytes, SLOT_SIZE_OFFSET) != SLOT_SIZE
        || byte_codec::readUint32(bytes, SLOT_COUNT_OFFSET) != SLOT_COUNT
        || !std::all_of(bytes.begin() + RESERVED_OFFSET, bytes.end(),
                        [](std::byte value) { return value == std::byte{0}; })) {
        throw WalError(WalErrorKind::CorruptHeader, "Invalid checkpoint-control header");
    }
}

std::array<std::byte, checkpoint_slot_layout::SIZE>
encodeCheckpointSlot(const CheckpointSlot& slot) {
    if (slot.generation == 0 || slot.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(slot.checkpointEndLsn)
        || slot.recoveryStartOffset == INVALID_WAL_OFFSET
        || slot.databasePageCount == 0
        || slot.nextTransactionId == INVALID_TRANSACTION_ID
        || slot.walFileSizeAtCheckpoint < slot.recoveryStartOffset) {
        throw WalError(WalErrorKind::InvalidArgument, "Invalid checkpoint-control slot");
    }
    std::array<std::byte, checkpoint_slot_layout::SIZE> bytes{};
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::GENERATION_OFFSET, slot.generation);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::CHECKPOINT_ID_OFFSET, slot.checkpointId);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::CHECKPOINT_END_LSN_OFFSET,
                            slot.checkpointEndLsn);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::RECOVERY_START_OFFSET,
                            slot.recoveryStartOffset);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::DATABASE_PAGE_COUNT_OFFSET,
                            slot.databasePageCount);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::NEXT_TRANSACTION_ID_OFFSET,
                            slot.nextTransactionId);
    byte_codec::writeUint64(bytes, checkpoint_slot_layout::WAL_FILE_SIZE_OFFSET,
                            slot.walFileSizeAtCheckpoint);
    byte_codec::writeUint32(bytes, checkpoint_slot_layout::CHECKSUM_OFFSET, slotChecksum(bytes));
    return bytes;
}

CheckpointSlot decodeCheckpointSlot(std::span<const std::byte> bytes) {
    if (bytes.size() != checkpoint_slot_layout::SIZE
        || byte_codec::readUint32(bytes, checkpoint_slot_layout::FLAGS_OFFSET) != 0
        || byte_codec::readUint32(bytes, checkpoint_slot_layout::CHECKSUM_OFFSET)
            != slotChecksum(bytes)) {
        throw WalError(WalErrorKind::CorruptRecord, "Invalid checkpoint-control slot");
    }
    CheckpointSlot slot{
        byte_codec::readUint64(bytes, checkpoint_slot_layout::GENERATION_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::CHECKPOINT_ID_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::CHECKPOINT_END_LSN_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::RECOVERY_START_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::DATABASE_PAGE_COUNT_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::NEXT_TRANSACTION_ID_OFFSET),
        byte_codec::readUint64(bytes, checkpoint_slot_layout::WAL_FILE_SIZE_OFFSET),
    };
    if (slot.generation == 0 || slot.checkpointId == INVALID_CHECKPOINT_ID
        || !isValidLsn(slot.checkpointEndLsn)
        || slot.recoveryStartOffset == INVALID_WAL_OFFSET
        || slot.databasePageCount == 0
        || slot.nextTransactionId == INVALID_TRANSACTION_ID
        || slot.walFileSizeAtCheckpoint < slot.recoveryStartOffset) {
        throw WalError(WalErrorKind::CorruptRecord, "Malformed checkpoint-control fields");
    }
    return slot;
}

CheckpointSelection CheckpointControl::select(const LogManager& logManager) const {
    CheckpointSelection result;
    const auto descriptor = ::open(path_.c_str(), O_RDONLY);
    if (descriptor < 0) {
        if (errno == ENOENT) return result;
        throwIo("Could not open checkpoint control");
    }
    const FileDescriptor owned(descriptor);
    result.controlFilePresent = true;
    if (fileSize(owned.get()) != checkpoint_control_layout::FILE_SIZE) {
        ++result.validationFailures;
        return result;
    }
    try {
        std::array<std::byte, checkpoint_control_layout::HEADER_SIZE> header{};
        readExact(owned.get(), 0, header);
        validateCheckpointControlHeader(header);
    } catch (const WalError&) {
        ++result.validationFailures;
        return result;
    }
    std::vector<CheckpointSlot> candidates;
    for (std::size_t index = 0; index < checkpoint_control_layout::SLOT_COUNT; ++index) {
        std::array<std::byte, checkpoint_slot_layout::SIZE> bytes{};
        readExact(owned.get(), checkpoint_control_layout::HEADER_SIZE
                               + index * checkpoint_control_layout::SLOT_SIZE, bytes);
        if (std::all_of(bytes.begin(), bytes.end(),
                        [](std::byte value) { return value == std::byte{0}; })) continue;
        try { candidates.push_back(decodeCheckpointSlot(bytes)); }
        catch (const WalError&) { ++result.validationFailures; }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.generation > right.generation;
    });
    for (const auto& slot : candidates) {
        try {
            if (crossValidate(slot, logManager)) { result.slot = slot; break; }
        } catch (const WalError&) {
        }
        ++result.validationFailures;
    }
    return result;
}

void CheckpointControl::initializeFileIfNeeded() {
    const auto descriptor = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (descriptor < 0) throwIo("Could not create checkpoint control");
    const FileDescriptor owned(descriptor);
    bool initialize = fileSize(owned.get()) != checkpoint_control_layout::FILE_SIZE;
    if (!initialize) {
        try {
            std::array<std::byte, checkpoint_control_layout::HEADER_SIZE> existing{};
            readExact(owned.get(), 0, existing);
            validateCheckpointControlHeader(existing);
        } catch (const WalError&) {
            initialize = true;
        }
    }
    if (initialize) {
        std::array<std::byte, checkpoint_control_layout::FILE_SIZE> bytes{};
        const auto header = encodeCheckpointControlHeader();
        std::copy(header.begin(), header.end(), bytes.begin());
        if (::ftruncate(owned.get(), static_cast<off_t>(bytes.size())) != 0) {
            throwIo("Could not resize checkpoint control");
        }
        writeExact(owned.get(), 0, bytes, stats_);
        if (::fsync(owned.get()) != 0) throwIo("Could not fsync checkpoint-control header");
        ++stats_.fsyncCalls;
    }
}

void CheckpointControl::publish(const CheckpointSlot& slot) {
    initializeFileIfNeeded();
    const auto descriptor = ::open(path_.c_str(), O_RDWR);
    if (descriptor < 0) throwIo("Could not open checkpoint control for publication");
    const FileDescriptor owned(descriptor);
    std::array<std::byte, checkpoint_control_layout::HEADER_SIZE> header{};
    readExact(owned.get(), 0, header);
    validateCheckpointControlHeader(header);

    std::array<std::optional<CheckpointSlot>, checkpoint_control_layout::SLOT_COUNT> current{};
    for (std::size_t index = 0; index < current.size(); ++index) {
        std::array<std::byte, checkpoint_slot_layout::SIZE> bytes{};
        readExact(owned.get(), checkpoint_control_layout::HEADER_SIZE
                               + index * checkpoint_control_layout::SLOT_SIZE, bytes);
        try { current[index] = decodeCheckpointSlot(bytes); } catch (const WalError&) {}
    }
    std::size_t target = 0;
    if (!current[0].has_value()) target = 0;
    else if (!current[1].has_value()) target = 1;
    else target = current[0]->generation <= current[1]->generation ? 0 : 1;

    const auto bytes = encodeCheckpointSlot(slot);
    const auto offset = checkpoint_control_layout::HEADER_SIZE
        + target * checkpoint_control_layout::SLOT_SIZE;
    writeExact(owned.get(), offset,
               std::span<const std::byte>(bytes).first(bytes.size() / 2), stats_);
    recoveryFailPoint("checkpoint_mid_control_write");
    writeExact(owned.get(), offset + bytes.size() / 2,
               std::span<const std::byte>(bytes).subspan(bytes.size() / 2), stats_);
    ++stats_.slotWrites;
    if (::fsync(owned.get()) != 0) throwIo("Could not fsync checkpoint control");
    ++stats_.fsyncCalls;
}

} // namespace minidb
