# Write-ahead log and recovery records

Milestone 11A adds a versioned sidecar write-ahead log, log sequence
numbers (LSNs), buffered append and `fsync` durability, safe scanning, and write-ahead
ordering in the buffer pool. Milestone 11B uses that substrate for full-page physical
before/after logging, implicit durable statement commit, and startup REDO/UNDO. It is
not ARIES; [recovery.md](recovery.md) defines the simpler protocol.

A bounded pool can evict dirty frames (the storage pressure behind a STEAL-style
design), while logical success need not force every data page immediately (the
motivation behind NO-FORCE). Those choices require log-before-data ordering plus
REDO/UNDO. 11A established the ordering substrate and 11B completes this serial,
statement-scoped baseline.

For database path `database.db`, active WAL now lives in `database.db.wal.d/`.
`database.db.wal` remains the readable legacy-v1 migration source. Exact segment and
manifest formats are in [wal-segments.md](wal-segments.md).
Database pages and WAL integers are little-endian; the independent TCP wire protocol is
big-endian. Neither WAL headers nor records are persisted as raw C++ structs.

## Legacy single-file header

A legacy single-file WAL starts with this 64-byte version-1 header. Segmented stores use
the separate manifest and per-segment headers documented in
[wal-segments.md](wal-segments.md).

| Offset | Width | Field | Version-1 value |
| ---: | ---: | --- | --- |
| 0 | 8 | magic bytes | ASCII `MDBWAL01` |
| 8 | 4 | WAL file format version | `1` |
| 12 | 4 | header size | `64` |
| 16 | 4 | database page size | `4096` |
| 20 | 4 | flags/reserved | `0` |
| 24 | 40 | reserved extension bytes | all zero |

Opening rejects truncated headers, unknown magic/version, a page-size mismatch, a wrong
header size, and nonzero reserved fields. A newly created header is itself written and
`fsync`ed before records are accepted.

## LSN and record layout

`Lsn` is an unsigned 64-bit integer. `INVALID_LSN` is `UINT64_MAX`. A record's LSN is
its byte position in the abstract global WAL stream. The first record has LSN 64 and
each following LSN is the previous position plus the previous total record length.
LSNs are monotonic across segment rotation, reclamation, migration, and reopen; they
never rebase and are not timestamps or segment-local offsets.

Each record has a fixed 48-byte header followed immediately by its opaque payload:

| Offset | Width | Field | Encoding/invariant |
| ---: | ---: | --- | --- |
| 0 | 4 | record magic | ASCII `MDBR` |
| 4 | 2 | record format version | little-endian `1` |
| 6 | 2 | record type ID | little-endian; table below |
| 8 | 4 | total record length | header plus payload |
| 12 | 4 | payload length | total length minus 48 |
| 16 | 8 | LSN | equals this record's global logical position |
| 24 | 8 | `TransactionId` | grouping identity reserved for 11B |
| 32 | 8 | previous LSN | earlier record or `INVALID_LSN` |
| 40 | 4 | checksum | CRC32C of the complete record with these four bytes zero |
| 44 | 4 | flags/reserved | zero in version 1 |
| 48 | variable | payload | interpreted by recovery according to record type |

Stable record type IDs are `1 BEGIN`, `2 PAGE_UPDATE`, `3 COMMIT`, `4 ABORT`,
`5 COMPENSATION`, `6 CHECKPOINT_BEGIN`, and `7 CHECKPOINT_END`. Production uses the
transaction records and both checkpoint records; COMPENSATION remains reserved.
`TransactionId` is `uint64_t`, with zero reserved
as invalid/system. Recovery validates exact per-transaction `prevLSN` chains. BEGIN is
16 bytes, PAGE_UPDATE is 8208 bytes, and COMMIT/ABORT payloads are empty; see
[recovery.md](recovery.md).

The maximum complete record is 1 MiB, so the maximum payload is 1,048,528 bytes. Length
checks happen before allocation. CRC32C uses the Castagnoli polynomial (reflected
`0x82F63B78`, initial/final XOR `0xFFFFFFFF`) and detects accidental corruption; it is
not an authentication mechanism.

## LogManager, buffering, and durability

`LogManager` separates record semantics from legacy/segmented physical storage. Its
default in-memory buffer is 64 KiB and is
configurable. `append()` serializes a record, assigns the next global logical LSN, and
usually copies the bytes into that buffer. Filling the buffer writes complete buffered
records to the operating system without claiming durability. A record larger than the
configured buffer first drains the buffer and is then written directly; valid large
records are not rejected merely because of the buffer setting.

`lastAppendedLsn()` and `durableLsn()` are deliberately different. Bytes copied to the
buffer, or written without a persistence barrier, are not durable. `flushUpTo(lsn)`
rejects an unknown LSN, writes pending bytes, calls POSIX `fsync`, and only then advances
the durable LSN. Because the whole current buffer is flushed, durability can advance
beyond the requested target to the latest appended record. Repeating a request already
covered by `durableLsn` is a no-op. There is no background logger or asynchronous/group
commit in 11A.

Standalone/eager opening validates and scans every complete record, reconstructing its
next, last-appended, and current durable positions. The standalone scanner validates
magic, versions/types, bounded and consistent lengths, logical LSN mapping, reserved fields,
`prevLSN`, and checksum. A short final header or a final record whose declared length
extends past EOF is reported as a truncated tail with the last complete byte boundary.
Interior corruption is rejected. Production startup uses the deferred, checkpoint-aware
path in [checkpoints.md](checkpoints.md), then applies the same strict scanner to the
selected tail. Opening never silently truncates; append remains
blocked until the caller deliberately invokes `truncateToLastValidRecord()`, which
truncates and `fsync`s.

Log statistics are independent from database-page I/O:

| Field | Meaning |
| --- | --- |
| `recordsAppended` / `bytesAppended` | logical records and complete encoded bytes accepted |
| `bufferFlushes` | nonempty in-memory buffer drains (not necessarily durability barriers) |
| `physicalWrites` / `bytesWritten` | successful WAL `pwrite` calls and bytes after any stats reset |
| `fsyncCalls` | successful WAL durability barriers |
| `flushUpToCalls` | requests, including already-durable no-ops |
| `bufferedBytes` | current not-yet-written buffer gauge |
| `lastAppendedLsn` / `durableLsn` | current logical/durable gauges |

Segmented mode additionally reports created/closed/deleted/retained segments, rotations,
directory syncs, active segment ID, oldest retained LSN, logical WAL end, current
physical retained bytes, and reclaimed bytes. Logical generated bytes never decrease;
physical retained bytes may drop after reclamation.

## Volatile pageLSN and write-ahead enforcement

Each `BufferFrame` now has volatile `pageLsn`, initialized to `INVALID_LSN` on a new
page, frame reuse, and disk reload. `WritePageGuard::setPageLsn()` requires a write pin,
a record known by the attached `WalFlushProvider`, and a value no older than the
frame's current LSN. Equal assignment is idempotent; regression and unknown/invalid LSNs
are rejected. The page LSN is not stored in any database-page byte in 11A.

For a dirty frame with a valid page LSN, the buffer pool enforces:

```text
durableWalLSN >= pageLSN
before
DiskManager writes that database page
```

The same centralized check runs for `flushPage`, dirty eviction, `flushAll`, and the
destructor's best-effort `flushAll`. A WAL-force error happens while the victim remains
valid and mapped; the database page is not written, its bytes/pageLSN remain unchanged,
and the frame is not reused. `flushAll` first finds the maximum valid LSN among dirty
frames, forces once through that point, then writes the pages. A clean frame never forces
WAL; it can retain a volatile LSN until reuse, when the LSN is reset.

The provider remains optional for legacy and low-level tests. With no provider,
unlogged dirty pages have `INVALID_LSN` and preserve Milestone 10 behavior. Production
`DatabaseServer` attaches LogManager and RecoveryCoordinator so page write intent and
pre-persistence preparation generate records centrally.

```text
RecoveryCoordinator -- record --> LogManager --> database.db.wal.d/
             |                              ^
             | page bytes + pageLSN         | force before page write
             v                              |
       WritePageGuard --> BufferPoolManager +--> DiskManager --> database.db
```

## Benchmarks

The standalone `wal_append_buffered`, `wal_append_flush_each`, `wal_batch_flush`,
`wal_segment_rotation`, and `wal_reclamation`
workloads measure the logging substrate, not transaction throughput. Configure payload,
batch, and buffer sizes with `--wal-payload-bytes`, `--wal-batch-size`, and
`--wal-buffer-bytes`. Results report records/s, payload bytes/s, append and force
p95/p99 latency, encoded WAL bytes written, buffer drains, physical writes, and fsyncs.
A batch size of 1, 10, or 100 is a synchronous experiment, not group commit.

## Current boundary

11B guarantees statement atomicity across tested process crashes: durable-COMMIT winners
are REDOed and a tail loser is undone. 11C.1 adds sharp checkpoints that bound normal
recovery analysis to the post-checkpoint tail. 11C.2 adds 16-MiB segments and safe
whole-segment deletion behind that boundary. Logical WAL generation continues to rise,
while retained physical bytes drop after checkpoints. There is no archive/PITR,
persistent pageLSN, fuzzy checkpoint, CLR, user transaction SQL, concurrency, or group
commit.

## Reference

C. Mohan, Don Haderle, Bruce Lindsay, Hamid Pirahesh, and Peter Schwarz. “ARIES: A
Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks
Using Write-Ahead Logging.” *ACM Transactions on Database Systems* 17(1), March 1992,
pp. 94–162. DOI: [10.1145/128765.128770](https://doi.org/10.1145/128765.128770).
See also the [IBM Research publication record](https://research.ibm.com/publications/aries-a-transaction-recovery-method-supporting-fine-granularity-locking-and-partial-rollbacks-using-write-ahead-logging).
