# Sharp checkpoints and bounded recovery

MiniDB++ uses a quiescent, or **sharp**, checkpoint. It is intentionally
simpler than ARIES fuzzy checkpointing: MiniDB++ has one serial mutating statement at
most, so checkpointing waits until no statement or rollback is active, requires zero
pinned buffer frames, and flushes every dirty frame. There is no transaction-table or
dirty-page-table snapshot, `recLSN`, or CLR. Persistent PageLSN is used independently
to skip already-present tail REDO updates.

The resulting invariant is:

> A durable, published `CHECKPOINT_END` means `database.db` physically contains every
> committed page state preceding that checkpoint, including page 0, allocator/free-list
> state, catalog pages, heaps, and indexes. Recovery may start at the byte immediately
> following that END record.

## Durability protocol

`CheckpointManager::checkpoint()` performs these ordered steps:

1. require no active statement/rollback and `BufferPoolManager::totalPinCount() == 0`;
2. allocate a monotonic nonzero `CheckpointId` and append `CHECKPOINT_BEGIN`;
3. call `BufferPoolManager::flushAll()` (which retains WAL-before-data ordering);
4. call `DiskManager::sync()` for the complete database file;
5. append `CHECKPOINT_END` and fsync WAL through it;
6. write the next generation into the inactive checkpoint-control slot and fsync the
   control file;
7. only after that final fsync treat the new checkpoint as authoritative;
8. rotate to a fresh segment and separately reclaim obsolete whole segments.

The checkpoint ID is consumed even if an attempt fails. On restart the next ID is one
greater than the maximum selected checkpoint or checkpoint record in the scanned tail.
IDs are not timestamps.

| Crash point | Recovery authority |
| --- | --- |
| before database sync | previous valid slot, or full WAL scan |
| after database sync but before durable END | previous slot; replay is idempotent |
| after durable END but before control fsync | previous slot; unreferenced END is harmless |
| during the new slot write | older CRC-valid slot survives |
| after control fsync | new slot and tail offset are authoritative |

## Checkpoint WAL records

Checkpoint records are system records: `TransactionId = 0` and `prevLSN = INVALID_LSN`.
All fields use little-endian encoding and the enclosing WAL record supplies its CRC32C.

`CHECKPOINT_BEGIN` type 6 has a 32-byte version-1 payload:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | checkpoint ID |
| 8 | 8 | previous authoritative checkpoint END LSN, or `INVALID_LSN` |
| 16 | 8 | logical WAL position at checkpoint start; equals this BEGIN's LSN |
| 24 | 8 | reserved, zero |

`CHECKPOINT_END` type 7 has a 48-byte version-1 payload:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | checkpoint ID |
| 8 | 8 | matching `CHECKPOINT_BEGIN` LSN |
| 16 | 8 | synchronized database page count |
| 24 | 8 | next transaction ID |
| 32 | 8 | recovery start LSN immediately after this complete END record |
| 40 | 8 | reserved, zero |

`WalOffset` remains a compatibility alias for `Lsn`; the recovery position may identify
logical end-of-WAL, where no record exists yet.

## Checkpoint-control sidecar

For `database.db`, `database.db.ckpt` locates a recovery start with constant-size I/O.
It contains no database tuples or pages. The file is exactly 192 bytes: one 64-byte
header followed by two independent 64-byte slots.

Header:

| Offset | Width | Field/value |
| ---: | ---: | --- |
| 0 | 8 | ASCII `MDBCKPT1` |
| 8 | 4 | format version 1 |
| 12 | 4 | header size 64 |
| 16 | 4 | slot size 64 |
| 20 | 4 | slot count 2 |
| 24 | 40 | reserved zero |

Each slot:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | nonzero monotonically increasing slot generation |
| 8 | 8 | checkpoint ID |
| 16 | 8 | checkpoint END LSN |
| 24 | 8 | recovery start offset |
| 32 | 8 | database page count |
| 40 | 8 | next transaction ID |
| 48 | 8 | logical WAL high-water mark when checkpoint completed |
| 56 | 4 | CRC32C |
| 60 | 4 | flags/reserved, zero |

The CRC is Castagnoli CRC32C over all 64 slot bytes with bytes 56–59 treated as zero.
Publication overwrites only the older/invalid slot with generation plus one and fsyncs
without destroying the other slot. A 64-byte write is not assumed atomic. At open, each
slot is decoded independently and candidates are tried by descending generation.

A CRC-valid slot is still untrusted. Its END LSN must identify a valid system
`CHECKPOINT_END`; ID, recovery offset, page count, and next transaction ID must match;
the recovery offset must equal the record end; and the referenced BEGIN must match. A
torn or mismatched newest slot falls back to the older cross-valid slot. If control is
absent/unusable after reclamation, recovery scans retained WAL for the newest complete
durable checkpoint pair and rebuilds the sidecar best-effort; byte 64 may no longer
exist. The sidecar is an optimization, not a source of database contents.

## Checkpoint-aware startup

Production `LogManager` uses deferred recovery. Segmented discovery validates the
retained manifest, headers, chain, and record boundaries; reclaimed historical files
are not read. Recovery then selects a cross-valid control slot and invokes strict CRC/length/
logical-LSN scanner from either `recoveryStartOffset` or the oldest retained LSN. A partial final record
is truncated. Tail winners retain ascending full-page REDO; the one possible tail loser
retains reverse before-image UNDO and appended-page truncation.

The next transaction ID is
`max(checkpoint.nextTransactionId, highest tail TransactionId + 1)`. Quiescence means no
transaction crosses the boundary. Checkpoint records in a stale-slot tail preserve
checkpoint-ID continuity but do not become authoritative without control publication.

## Policy, metrics, and boundary

The internal API is `CheckpointManager::checkpoint()`. There is no `CHECKPOINT` SQL
statement. Automatic policy is evaluated only after successful mutating commit.
`--checkpoint-wal-bytes N` uses post-checkpoint WAL growth (default 64 MiB), while
`--checkpoint-statements N` is an optional commit counter; zero disables either. There
is no mandatory shutdown checkpoint.

Metrics report attempts/completions/failures, dirty writes, forces/syncs, latency, WAL
growth, control selection/fallback, and skipped/scanned WAL bytes. Benchmarks
`checkpoint_latency` and `recovery_checkpoint_compare` expose the checkpoint-cost versus
recovery-work tradeoff without timing assertions.

Segmented WAL rotates after publication and retains the checkpoint base, one extra closed
predecessor, and every tail/active segment; older whole segments are deleted. Checkpoint
critical latency and reclamation latency/bytes are measured separately. It adds no
archive/PITR, fuzzy checkpoint, dirty-page table, CLR, finer-grained WAL, or concurrency.
PageLSN complements rather than replaces the scan boundary: checkpoints reduce WAL
analysis volume, while PageLSN reduces writes within the selected tail. See
[wal-segments.md](wal-segments.md) and [page-lsn.md](page-lsn.md).

The design is informed by C. Mohan et al., “ARIES: A Transaction Recovery Method
Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging,”
but is explicitly not an ARIES fuzzy checkpoint.
