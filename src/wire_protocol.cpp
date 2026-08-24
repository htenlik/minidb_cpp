#include "minidb/wire_protocol.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace minidb::net {
namespace {

constexpr std::uint16_t COMMAND_CREATE_TABLE = 1;
constexpr std::uint16_t COMMAND_INSERT = 2;
constexpr std::uint16_t COMMAND_UPDATE = 3;
constexpr std::uint16_t COMMAND_DELETE = 4;
constexpr std::uint16_t ACCESS_NONE = 0;
constexpr std::uint16_t ACCESS_HEAP_SCAN = 1;
constexpr std::uint16_t ACCESS_PRIMARY_KEY = 2;
constexpr std::uint8_t VALUE_NULL = 0;
constexpr std::uint8_t VALUE_UINT32 = 1;
constexpr std::uint8_t VALUE_INT64 = 2;
constexpr std::uint8_t VALUE_BOOLEAN = 3;
constexpr std::uint8_t VALUE_VARCHAR = 4;
constexpr std::uint16_t FLAG_PRESENT = 0x0001;

bool knownMessageType(std::uint16_t value) noexcept {
    return value == static_cast<std::uint16_t>(MessageType::Hello)
        || value == static_cast<std::uint16_t>(MessageType::ExecuteSql)
        || value == static_cast<std::uint16_t>(MessageType::HelloAck)
        || value == static_cast<std::uint16_t>(MessageType::CommandResult)
        || value == static_cast<std::uint16_t>(MessageType::SelectResult)
        || value == static_cast<std::uint16_t>(MessageType::ErrorResponse);
}

class Writer {
public:
    explicit Writer(std::size_t maximum = MAX_FRAME_PAYLOAD) : maximum_(maximum) {}

    void byte(std::uint8_t value) {
        require(1);
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void uint16(std::uint16_t value) {
        require(2);
        bytes_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
        bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
    }

    void uint32(std::uint32_t value) {
        require(4);
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }

    void uint64(std::uint64_t value) {
        require(8);
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }

    void raw(std::span<const std::byte> bytes) {
        require(bytes.size());
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void string(std::string_view value) {
        if (value.size() > MAX_WIRE_STRING_BYTES
            || value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw ProtocolError("wire string exceeds protocol limit");
        }
        uint32(static_cast<std::uint32_t>(value.size()));
        raw(std::as_bytes(std::span<const char>(value.data(), value.size())));
    }

    [[nodiscard]] WireBytes finish() && { return std::move(bytes_); }

private:
    WireBytes bytes_;
    std::size_t maximum_;

    void require(std::size_t count) {
        if (count > maximum_ - bytes_.size()) {
            throw ProtocolError("encoded payload exceeds protocol maximum");
        }
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t byte() {
        require(1);
        return std::to_integer<std::uint8_t>(bytes_[cursor_++]);
    }

    [[nodiscard]] std::uint16_t uint16() {
        require(2);
        const auto value = static_cast<std::uint16_t>(byte()) << 8U;
        return static_cast<std::uint16_t>(value | byte());
    }

    [[nodiscard]] std::uint32_t uint32() {
        require(4);
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | byte();
        }
        return value;
    }

    [[nodiscard]] std::uint64_t uint64() {
        require(8);
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value = (value << 8U) | byte();
        }
        return value;
    }

    [[nodiscard]] std::string string() {
        const auto size = uint32();
        if (size > MAX_WIRE_STRING_BYTES) {
            throw ProtocolError("wire string exceeds protocol limit");
        }
        require(size);
        std::string value(size, '\0');
        for (std::size_t index = 0; index < size; ++index) {
            value[index] = static_cast<char>(byte());
        }
        return value;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - cursor_;
    }

    void finish() const {
        if (cursor_ != bytes_.size()) {
            throw ProtocolError("wire payload contains trailing bytes");
        }
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_ = 0;

    void require(std::size_t count) const {
        if (count > bytes_.size() - cursor_) {
            throw ProtocolError("wire payload is truncated");
        }
    }
};

std::uint16_t encodeCommandKind(sql::CommandKind kind) {
    switch (kind) {
    case sql::CommandKind::CreateTable: return COMMAND_CREATE_TABLE;
    case sql::CommandKind::Insert: return COMMAND_INSERT;
    case sql::CommandKind::Update: return COMMAND_UPDATE;
    case sql::CommandKind::Delete: return COMMAND_DELETE;
    }
    throw ProtocolError("unknown command kind");
}

sql::CommandKind decodeCommandKind(std::uint16_t value) {
    switch (value) {
    case COMMAND_CREATE_TABLE: return sql::CommandKind::CreateTable;
    case COMMAND_INSERT: return sql::CommandKind::Insert;
    case COMMAND_UPDATE: return sql::CommandKind::Update;
    case COMMAND_DELETE: return sql::CommandKind::Delete;
    default: throw ProtocolError("invalid command kind ID");
    }
}

std::uint16_t encodeAccessPath(sql::AccessPath path) {
    switch (path) {
    case sql::AccessPath::None: return ACCESS_NONE;
    case sql::AccessPath::HeapScan: return ACCESS_HEAP_SCAN;
    case sql::AccessPath::PrimaryKeyLookup: return ACCESS_PRIMARY_KEY;
    }
    throw ProtocolError("unknown access path");
}

sql::AccessPath decodeAccessPath(std::uint16_t value) {
    switch (value) {
    case ACCESS_NONE: return sql::AccessPath::None;
    case ACCESS_HEAP_SCAN: return sql::AccessPath::HeapScan;
    case ACCESS_PRIMARY_KEY: return sql::AccessPath::PrimaryKeyLookup;
    default: throw ProtocolError("invalid AccessPath ID");
    }
}

void encodeStats(Writer& writer, const sql::ExecutionStats& stats) {
    writer.uint16(encodeAccessPath(stats.accessPath));
    writer.uint16(0);
    writer.uint64(stats.rowsExamined);
    writer.uint64(stats.rowsReturned);
    writer.uint64(stats.indexLookups);
}

sql::ExecutionStats decodeStats(Reader& reader) {
    const auto path = decodeAccessPath(reader.uint16());
    if (reader.uint16() != 0) {
        throw ProtocolError("ExecutionStats reserved field is nonzero");
    }
    return sql::ExecutionStats{
        path,
        reader.uint64(),
        reader.uint64(),
        reader.uint64(),
    };
}

void encodeRecordId(Writer& writer, RecordId recordId) {
    if (!recordId.isValid()) {
        throw ProtocolError("cannot encode invalid RecordId");
    }
    writer.uint32(recordId.pageId);
    writer.uint16(recordId.slotId);
    writer.uint16(0);
}

RecordId decodeRecordId(Reader& reader) {
    const RecordId recordId{reader.uint32(), reader.uint16()};
    if (reader.uint16() != 0 || !recordId.isValid()) {
        throw ProtocolError("wire RecordId is invalid or reserved field is nonzero");
    }
    return recordId;
}

void encodeValue(Writer& writer, const Value& value) {
    std::visit(
        [&](const auto& item) {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) {
                writer.byte(VALUE_NULL);
            } else if constexpr (std::is_same_v<Item, std::uint32_t>) {
                writer.byte(VALUE_UINT32);
                writer.uint32(item);
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
                writer.byte(VALUE_INT64);
                writer.uint64(std::bit_cast<std::uint64_t>(item));
            } else if constexpr (std::is_same_v<Item, bool>) {
                writer.byte(VALUE_BOOLEAN);
                writer.byte(item ? 1 : 0);
            } else {
                writer.byte(VALUE_VARCHAR);
                writer.string(item);
            }
        },
        value);
}

Value decodeValue(Reader& reader) {
    switch (reader.byte()) {
    case VALUE_NULL: return std::monostate{};
    case VALUE_UINT32: return reader.uint32();
    case VALUE_INT64: return std::bit_cast<std::int64_t>(reader.uint64());
    case VALUE_BOOLEAN: {
        const auto value = reader.byte();
        if (value > 1) {
            throw ProtocolError("wire BOOLEAN is neither zero nor one");
        }
        return value == 1;
    }
    case VALUE_VARCHAR: return reader.string();
    default: throw ProtocolError("invalid wire Value tag");
    }
}

bool knownErrorCategory(std::uint16_t value) noexcept {
    return value >= static_cast<std::uint16_t>(ErrorCategory::Protocol)
        && value <= static_cast<std::uint16_t>(ErrorCategory::Internal);
}

std::uint32_t checkedLocation(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("source line or column exceeds wire range");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

RemoteSqlError::RemoteSqlError(std::uint64_t requestId, ErrorResponse response)
    : std::runtime_error(response.message),
      requestId_(requestId),
      response_(std::move(response)) {}

WireBytes encodeFrameHeader(const FrameHeader& header) {
    if (!knownMessageType(static_cast<std::uint16_t>(header.messageType))) {
        throw ProtocolError("unknown protocol message type", header.requestId);
    }
    if (header.payloadLength > MAX_FRAME_PAYLOAD) {
        throw ProtocolError("frame payload length exceeds protocol maximum", header.requestId);
    }
    Writer writer(FRAME_HEADER_SIZE);
    writer.raw(PROTOCOL_MAGIC);
    writer.uint16(PROTOCOL_VERSION);
    writer.uint16(static_cast<std::uint16_t>(header.messageType));
    writer.uint64(header.requestId);
    writer.uint32(header.payloadLength);
    writer.uint32(0);
    return std::move(writer).finish();
}

FrameHeader decodeFrameHeader(std::span<const std::byte> bytes) {
    if (bytes.size() != FRAME_HEADER_SIZE) {
        throw ProtocolError("frame header must contain exactly 24 bytes");
    }
    Reader reader(bytes);
    std::array<std::byte, PROTOCOL_MAGIC.size()> magic{};
    for (auto& value : magic) {
        value = static_cast<std::byte>(reader.byte());
    }
    const auto version = reader.uint16();
    const auto rawType = reader.uint16();
    const auto requestId = reader.uint64();
    const auto payloadLength = reader.uint32();
    const auto reserved = reader.uint32();
    reader.finish();
    if (magic != PROTOCOL_MAGIC) {
        throw ProtocolError("invalid MiniDB++ protocol magic", requestId);
    }
    if (version != PROTOCOL_VERSION) {
        throw ProtocolError("unsupported MiniDB++ protocol version", requestId);
    }
    if (!knownMessageType(rawType)) {
        throw ProtocolError("unknown protocol message type", requestId);
    }
    if (payloadLength > MAX_FRAME_PAYLOAD) {
        throw ProtocolError("frame payload length exceeds protocol maximum", requestId);
    }
    if (reserved != 0) {
        throw ProtocolError("frame reserved field must be zero", requestId);
    }
    return FrameHeader{static_cast<MessageType>(rawType), requestId, payloadLength};
}

WireBytes encodeFrame(const Frame& frame) {
    if (frame.payload.size() != frame.header.payloadLength) {
        throw ProtocolError("frame header payload length disagrees with payload");
    }
    auto bytes = encodeFrameHeader(frame.header);
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return bytes;
}

Frame decodeFrame(std::span<const std::byte> bytes) {
    if (bytes.size() < FRAME_HEADER_SIZE) {
        throw ProtocolError("complete frame is shorter than its header");
    }
    const auto header = decodeFrameHeader(bytes.first(FRAME_HEADER_SIZE));
    if (bytes.size() != FRAME_HEADER_SIZE + header.payloadLength) {
        throw ProtocolError("complete frame size disagrees with payload length", header.requestId);
    }
    return Frame{
        header,
        WireBytes(bytes.begin() + FRAME_HEADER_SIZE, bytes.end()),
    };
}

Frame makeHelloFrame() {
    return Frame{FrameHeader{MessageType::Hello, 0, 0}, {}};
}

Frame makeHelloAckFrame() {
    return Frame{FrameHeader{MessageType::HelloAck, 0, 0}, {}};
}

Frame makeExecuteSqlFrame(std::uint64_t requestId, std::string_view sqlSource) {
    if (sqlSource.size() > MAX_SQL_BYTES) {
        throw ProtocolError("SQL request exceeds protocol SQL byte limit", requestId);
    }
    WireBytes payload;
    payload.reserve(sqlSource.size());
    for (const unsigned char byte : sqlSource) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    return Frame{
        FrameHeader{
            MessageType::ExecuteSql,
            requestId,
            static_cast<std::uint32_t>(payload.size()),
        },
        std::move(payload),
    };
}

std::string decodeExecuteSqlPayload(const Frame& frame) {
    if (frame.header.messageType != MessageType::ExecuteSql
        || frame.payload.size() != frame.header.payloadLength) {
        throw ProtocolError("frame is not a valid EXECUTE_SQL request", frame.header.requestId);
    }
    if (frame.payload.size() > MAX_SQL_BYTES) {
        throw ProtocolError("SQL request exceeds protocol SQL byte limit", frame.header.requestId);
    }
    std::string source(frame.payload.size(), '\0');
    for (std::size_t index = 0; index < frame.payload.size(); ++index) {
        source[index] = static_cast<char>(std::to_integer<unsigned char>(frame.payload[index]));
    }
    return source;
}

WireBytes encodeCommandResultPayload(const sql::CommandResult& result) {
    Writer writer;
    writer.uint16(encodeCommandKind(result.command));
    writer.uint16(result.insertedRecordId.has_value() ? FLAG_PRESENT : 0);
    writer.uint64(result.affectedRows);
    encodeStats(writer, result.stats);
    if (result.insertedRecordId.has_value()) {
        encodeRecordId(writer, *result.insertedRecordId);
    }
    return std::move(writer).finish();
}

sql::CommandResult decodeCommandResultPayload(std::span<const std::byte> payload) {
    Reader reader(payload);
    const auto command = decodeCommandKind(reader.uint16());
    const auto flags = reader.uint16();
    if ((flags & static_cast<std::uint16_t>(~FLAG_PRESENT)) != 0) {
        throw ProtocolError("CommandResult contains unknown flags");
    }
    const auto affectedRows = reader.uint64();
    const auto stats = decodeStats(reader);
    std::optional<RecordId> recordId;
    if ((flags & FLAG_PRESENT) != 0) {
        recordId = decodeRecordId(reader);
    }
    reader.finish();
    return sql::CommandResult{command, affectedRows, recordId, stats};
}

WireBytes encodeSelectResultPayload(const sql::SelectResult& result) {
    if (result.columns.size() > MAX_WIRE_COLUMNS || result.rows.size() > MAX_WIRE_ROWS
        || (!result.recordIds.empty() && result.recordIds.size() != result.rows.size())) {
        throw ProtocolError("SelectResult counts exceed limits or RID count disagrees");
    }
    Writer writer;
    writer.uint32(static_cast<std::uint32_t>(result.columns.size()));
    writer.uint64(result.rows.size());
    writer.uint32(result.recordIds.empty() ? 0 : FLAG_PRESENT);
    encodeStats(writer, result.stats);
    for (const auto& column : result.columns) {
        writer.string(column);
    }
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); ++rowIndex) {
        if (!result.recordIds.empty()) {
            encodeRecordId(writer, result.recordIds[rowIndex]);
        }
        const auto& row = result.rows[rowIndex];
        if (row.size() != result.columns.size()) {
            throw ProtocolError("SelectResult row width disagrees with projection");
        }
        writer.uint32(static_cast<std::uint32_t>(row.size()));
        for (const auto& value : row) {
            encodeValue(writer, value);
        }
    }
    return std::move(writer).finish();
}

sql::SelectResult decodeSelectResultPayload(std::span<const std::byte> payload) {
    Reader reader(payload);
    const auto columnCount = reader.uint32();
    const auto rowCount = reader.uint64();
    const auto flags = reader.uint32();
    if (columnCount > MAX_WIRE_COLUMNS || rowCount > MAX_WIRE_ROWS
        || (flags & ~static_cast<std::uint32_t>(FLAG_PRESENT)) != 0) {
        throw ProtocolError("SelectResult count or flags are invalid");
    }
    auto stats = decodeStats(reader);
    sql::SelectResult result;
    result.stats = stats;
    result.columns.reserve(columnCount);
    for (std::size_t index = 0; index < columnCount; ++index) {
        result.columns.push_back(reader.string());
    }
    if (rowCount > reader.remaining()) {
        throw ProtocolError("SelectResult row count is impossible for remaining payload");
    }
    result.rows.reserve(static_cast<std::size_t>(rowCount));
    if ((flags & FLAG_PRESENT) != 0) {
        result.recordIds.reserve(static_cast<std::size_t>(rowCount));
    }
    for (std::uint64_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        if ((flags & FLAG_PRESENT) != 0) {
            result.recordIds.push_back(decodeRecordId(reader));
        }
        const auto valueCount = reader.uint32();
        if (valueCount != columnCount || valueCount > reader.remaining()) {
            throw ProtocolError("SelectResult row width is invalid");
        }
        RowValues row;
        row.reserve(valueCount);
        for (std::size_t index = 0; index < valueCount; ++index) {
            row.push_back(decodeValue(reader));
        }
        result.rows.push_back(std::move(row));
    }
    reader.finish();
    return result;
}

Frame encodeQueryResultFrame(std::uint64_t requestId, const sql::QueryResult& result) {
    return std::visit(
        [requestId](const auto& value) {
            using Result = std::decay_t<decltype(value)>;
            auto payload = [&] {
                if constexpr (std::is_same_v<Result, sql::CommandResult>) {
                    return encodeCommandResultPayload(value);
                } else {
                    return encodeSelectResultPayload(value);
                }
            }();
            return Frame{
                FrameHeader{
                    std::is_same_v<Result, sql::CommandResult>
                        ? MessageType::CommandResult
                        : MessageType::SelectResult,
                    requestId,
                    static_cast<std::uint32_t>(payload.size()),
                },
                std::move(payload),
            };
        },
        result);
}

sql::QueryResult decodeQueryResultFrame(const Frame& frame) {
    if (frame.payload.size() != frame.header.payloadLength) {
        throw ProtocolError("response frame payload length disagrees");
    }
    if (frame.header.messageType == MessageType::CommandResult) {
        return decodeCommandResultPayload(frame.payload);
    }
    if (frame.header.messageType == MessageType::SelectResult) {
        return decodeSelectResultPayload(frame.payload);
    }
    throw ProtocolError("frame is not a QueryResult response", frame.header.requestId);
}

WireBytes encodeErrorResponsePayload(const ErrorResponse& error) {
    Writer writer;
    writer.uint16(static_cast<std::uint16_t>(error.category));
    writer.uint16(error.span.has_value() ? FLAG_PRESENT : 0);
    writer.string(error.message);
    if (error.span.has_value()) {
        writer.uint64(error.span->begin.offset);
        writer.uint64(error.span->end.offset);
        writer.uint32(checkedLocation(error.span->begin.line));
        writer.uint32(checkedLocation(error.span->begin.column));
        writer.uint32(checkedLocation(error.span->end.line));
        writer.uint32(checkedLocation(error.span->end.column));
    }
    return std::move(writer).finish();
}

ErrorResponse decodeErrorResponsePayload(std::span<const std::byte> payload) {
    Reader reader(payload);
    const auto rawCategory = reader.uint16();
    const auto flags = reader.uint16();
    if (!knownErrorCategory(rawCategory)) {
        throw ProtocolError("invalid ErrorResponse category");
    }
    if ((flags & static_cast<std::uint16_t>(~FLAG_PRESENT)) != 0) {
        throw ProtocolError("ErrorResponse contains unknown flags");
    }
    ErrorResponse result{
        static_cast<ErrorCategory>(rawCategory),
        reader.string(),
        std::nullopt,
    };
    if ((flags & FLAG_PRESENT) != 0) {
        const auto beginOffset = reader.uint64();
        const auto endOffset = reader.uint64();
        const auto beginLine = reader.uint32();
        const auto beginColumn = reader.uint32();
        const auto endLine = reader.uint32();
        const auto endColumn = reader.uint32();
        if (beginLine == 0 || beginColumn == 0 || endLine == 0 || endColumn == 0
            || endOffset < beginOffset) {
            throw ProtocolError("ErrorResponse source span is invalid");
        }
        result.span = sql::SourceSpan{
            sql::SourceLocation{
                static_cast<std::size_t>(beginOffset), beginLine, beginColumn,
            },
            sql::SourceLocation{
                static_cast<std::size_t>(endOffset), endLine, endColumn,
            },
        };
    }
    reader.finish();
    return result;
}

Frame makeErrorFrame(std::uint64_t requestId, const ErrorResponse& error) {
    auto payload = encodeErrorResponsePayload(error);
    return Frame{
        FrameHeader{
            MessageType::ErrorResponse,
            requestId,
            static_cast<std::uint32_t>(payload.size()),
        },
        std::move(payload),
    };
}

} // namespace minidb::net
