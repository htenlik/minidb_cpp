# Write-ahead log foundation

Milestone 11A adds a versioned sidecar write-ahead log, byte-offset log sequence
numbers (LSNs), buffered append and `fsync` durability, safe scanning, and write-ahead
ordering in the buffer pool. It is a logging substrate inspired by WAL/ARIES principles;
it is **not** ARIES recovery and does not yet make production mutations crash safe.

A bounded pool can evict dirty frames (the storage pressure behind a STEAL-style
design), while logical success need not force every data page immediately (the
motivation behind NO-FORCE). Those choices require log-before-data ordering plus
REDO/UNDO for transaction recovery. 11A establishes only the ordering substrate; it
does not yet define transaction commit, STEAL/NO-FORCE recovery, or atomicity.

For database path `database.db`, the deterministic WAL path is `database.db.wal`.
Database pages and WAL integers are little-endian; the independent TCP wire protocol is
big-endian. Neither WAL headers nor records are persisted as raw C++ structs.

## File header

Every WAL starts with this 64-byte version-1 header:

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
its physical starting byte offset in the WAL, so the first record has LSN 64 and each
following LSN is the previous offset plus the previous total record length. LSNs are
monotonic, deterministic across reopen, and are not timestamps.

Each record has a fixed 48-byte header followed immediately by its opaque payload:

| Offset | Width | Field | Encoding/invariant |
| ---: | ---: | --- | --- |
| 0 | 4 | record magic | ASCII `MDBR` |
| 4 | 2 | record format version | little-endian `1` |
| 6 | 2 | record type ID | little-endian; table below |
| 8 | 4 | total record length | header plus payload |
| 12 | 4 | payload length | total length minus 48 |
| 16 | 8 | LSN | equals this record's physical offset |
| 24 | 8 | `TransactionId` | grouping identity reserved for 11B |
| 32 | 8 | previous LSN | earlier record or `INVALID_LSN` |
| 40 | 4 | checksum | CRC32C of the complete record with these four bytes zero |
| 44 | 4 | flags/reserved | zero in version 1 |
| 48 | variable | payload | uninterpreted by the 11A substrate |

Stable record type IDs are `1 BEGIN`, `2 PAGE_UPDATE`, `3 COMMIT`, `4 ABORT`,
`5 COMPENSATION`, `6 CHECKPOINT_BEGIN`, and `7 CHECKPOINT_END`. These names reserve the
format vocabulary; 11A does not generate semantic recovery records in the production
storage path. `TransactionId` is `uint64_t`, with zero reserved as invalid/system.
`prevLSN` supports a future per-transaction log chain but is not traversed for recovery
yet.

The maximum complete record is 1 MiB, so the maximum payload is 1,048,528 bytes. Length
checks happen before allocation. CRC32C uses the Castagnoli polynomial (reflected
`0x82F63B78`, initial/final XOR `0xFFFFFFFF`) and detects accidental corruption; it is
not an authentication mechanism.

## LogManager, buffering, and durability

`LogManager` owns one WAL descriptor. Its default in-memory buffer is 64 KiB and is
configurable. `append()` serializes a record, assigns the next physical-offset LSN, and
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

Opening an existing WAL validates and scans every complete record, reconstructing its
next, last-appended, and current durable positions. The standalone scanner validates
magic, versions/types, bounded and consistent lengths, physical LSN, reserved fields,
`prevLSN`, and checksum. A short final header or a final record whose declared length
extends past EOF is reported as a truncated tail with the last complete byte boundary.
Interior corruption is rejected. Opening never silently truncates; append remains
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

The provider is optional. With no provider, legacy/unlogged dirty pages have
`INVALID_LSN` and preserve Milestone 10 behavior. Production storage is intentionally
not wired to invent records, and the server does not create an empty/misleading WAL by
default. Tests and future recovery code explicitly append and associate an LSN.

```text
test/future recovery layer -- record --> LogManager --> database.db.wal
             |                              ^
             | page bytes + pageLSN         | force before page write
             v                              |
       WritePageGuard --> BufferPoolManager +--> DiskManager --> database.db
```

## Benchmarks

The standalone `wal_append_buffered`, `wal_append_flush_each`, and `wal_batch_flush`
workloads measure the logging substrate, not transaction throughput. Configure payload,
batch, and buffer sizes with `--wal-payload-bytes`, `--wal-batch-size`, and
`--wal-buffer-bytes`. Results report records/s, payload bytes/s, append and force
p95/p99 latency, encoded WAL bytes written, buffer drains, physical writes, and fsyncs.
A batch size of 1, 10, or 100 is a synchronous experiment, not group commit.

## Exact 11A boundary

11A guarantees validated clean-close/reopen WAL persistence and WAL-before-database
ordering only for dirty frames explicitly assigned a valid LSN. It does **not** provide
automatic production mutation logging, persistent pageLSNs, analysis/redo/undo,
checkpointing/truncation policy, statement atomicity, user transactions, WAL-based page
allocation recovery, group commit, or crash recovery. A crash can still leave the
database logically inconsistent. Milestone 11B must design recovery records, persist
the pageLSN information recovery needs, connect all mutation paths, inject crashes, and
implement REDO/UNDO before stronger durability claims are valid.

## Reference

C. Mohan, Don Haderle, Bruce Lindsay, Hamid Pirahesh, and Peter Schwarz. “ARIES: A
Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks
Using Write-Ahead Logging.” *ACM Transactions on Database Systems* 17(1), March 1992,
pp. 94–162. DOI: [10.1145/128765.128770](https://doi.org/10.1145/128765.128770).
See also the [IBM Research publication record](https://research.ibm.com/publications/aries-a-transaction-recovery-method-supporting-fine-granularity-locking-and-partial-rollbacks-using-write-ahead-logging).
