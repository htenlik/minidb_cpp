#pragma once

#include "minidb/database_access_gate.hpp"
#include "minidb/recovery.hpp"
#include <functional>

namespace minidb {
namespace test { class ReclaimFixture; }

namespace page_reclaim_layout {
inline constexpr std::uint16_t VERSION = 1;
inline constexpr std::size_t PAGE = 0, HEAD = 4, VERSION_OFFSET = 8, FLAGS = 10;
inline constexpr std::size_t RESERVED = 12, PAGE_COUNT = 16, BEFORE_PAGE_LSN = 24;
inline constexpr std::size_t BEFORE_METADATA_LSN = 32, BEFORE_CRC = 40, TAIL = 44;
inline constexpr std::size_t SIZE = 48;
}
struct PageReclaimPayload {
    PageId page = INVALID_PAGE_ID, head = INVALID_PAGE_ID;
    std::uint64_t pageCount = 0;
    Lsn beforePageLsn = INVALID_LSN, beforeMetadataLsn = INVALID_LSN;
    std::uint32_t beforeCrc = 0;
    bool operator==(const PageReclaimPayload&) const = default;
};
[[nodiscard]] std::vector<std::byte> encodePageReclaim(const PageReclaimPayload& value);
[[nodiscard]] PageReclaimPayload decodePageReclaim(const LogRecord& record);
[[nodiscard]] DiskManager::Page reclaimedPage(const PageReclaimPayload& value, Lsn lsn);

// There is deliberately no production issuer yet. A future trusted lifecycle
// authority must prove unreachability, not merely absence of pins. Tests issue
// certificates only for their explicitly tracked unreferenced physical pages.
class UnownedPageCertificate {
    friend class test::ReclaimFixture;
    friend class PageReclaimer;
    UnownedPageCertificate(const DiskManager& disk, PageId page, std::uint32_t crc,
                          TransactionId nextId, Lsn walEnd)
        : disk_(&disk), page_(page), crc_(crc), nextId_(nextId), walEnd_(walEnd) {}
    const DiskManager* disk_; // volatile certificate identity; never persisted
    PageId page_;
    std::uint32_t crc_;
    TransactionId nextId_;
    Lsn walEnd_;
};

class PageReclaimer {
public:
    PageReclaimer(DiskManager& disk, BufferPoolManager& pool, LogManager& log,
                  RecoveryCoordinator& recovery, DatabaseAccessGate& gate)
        : disk_(disk), pool_(pool), log_(log), recovery_(recovery), gate_(gate) {}
    [[nodiscard]] Lsn reclaim(const UnownedPageCertificate& certificate,
                             const DatabaseAccessGate::Lease& exclusive);
private:
    DiskManager& disk_;
    BufferPoolManager& pool_;
    LogManager& log_;
    RecoveryCoordinator& recovery_;
    DatabaseAccessGate& gate_;
};

// Recovery-only application: later-state evidence must identify the same target
// page in retained WAL. Returns writes and skips, never extends the database.
struct ReclaimRedoStats { std::uint64_t writes = 0, skips = 0; };
[[nodiscard]] ReclaimRedoStats redoPageReclaim(
    DiskManager& disk, const LogRecord& record,
    const std::function<bool(PageId, Lsn)>& knownLaterState);
} // namespace minidb
