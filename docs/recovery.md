# Crash recovery and implicit statement transactions

Each mutating `CREATE TABLE`, `INSERT`, `UPDATE`, or `DELETE` statement is one internal
atomic recovery unit. `SELECT` is read-only, and there is no
user-visible transaction grammar. MiniDB++ still executes serially with at most one
active mutation.

The default implementation uses physical full-page logging: every changed database page
is represented by its complete 4096-byte before- and after-image. An explicit opt-in
mode uses versioned physical byte ranges while retaining the same transaction and
durability semantics. Both are described in [wal-byte-range.md](wal-byte-range.md).
The full-page mode remains a correctness-first baseline that covers every guarded page
type at one interception point, but produces large WAL records.

## Transaction records

All integers are little-endian. The ordinary WAL record header, length validation, and
CRC32C described in [wal.md](wal.md) enclose these payloads.

### BEGIN payload (16 bytes)

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | database page count at logical statement start |
| 8 | 8 | reserved, zero |

The page count is captured before an allocation is possible. An in-memory transaction
context is created at the statement boundary, but BEGIN is appended lazily only when a
changed page must be logged. A successful zero-change update/delete and a semantic
failure before mutation therefore emit no transaction WAL.

### PAGE_UPDATE payload (8208 bytes)

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | `PageId` |
| 4 | 4 | flags; bit 0 is `BEFORE_PAGE_EXISTED` |
| 8 | 4 | page size, exactly 4096 |
| 12 | 4 | reserved, zero |
| 16 | 4096 | full original before-image |
| 4112 | 4096 | full current after-image |

A page whose ID is below the BEGIN page count existed before the statement. This
includes a free-list page reused for another purpose, so rollback restores its exact
Free Page encoding. A newly appended page has flag bit 0 clear and a canonical all-zero
before-image; loser cleanup removes it by truncating the file to the BEGIN page count.
Unknown flags, invalid IDs, wrong page size, nonzero reserved fields, and a nonzero new-
page before-image are rejected.

The first write intent captures an existing page's before-image once. If a page changes,
is logged/evicted, is reloaded, and changes again, records may contain `S0 -> S1` and
`S0 -> S2`. REDO order ends at S2; either record can restore S0 during idempotent UNDO.

COMMIT and ABORT have empty payloads. Their `prevLSN` completes the exact per-transaction
chain. Transaction IDs are monotonic `uint64_t` values; zero is invalid, and startup
chooses one above the maximum ID in retained WAL history.

## Runtime mutation and durability path

```text
Table / B+ tree / TupleStore
        |
        | guarded page mutation
        v
BufferPoolManager -- write intent --> RecoveryCoordinator --> LogManager
        |                                      |
             | prepare before physical write        | update-record LSN
        +---------------------------------------+
        |
        v
DiskManager
```

Before writable bytes are exposed, the buffer pool registers write intent and the
coordinator copies the original page if this is its first write. WAL I/O is never done
from a page-guard destructor: allocation, buffering, or file I/O may fail and must report
through an explicit operation.

Before dirty persistence, `preparePageForWrite` compares current bytes with the last
logged after-image. It appends the selected update record only when needed, advances
`prevLSN`, and returns its LSN. The volatile frame pageLSN then drives the rule that
WAL is fsynced before the database page is written. Dirty eviction is therefore STEAL-
safe. On frame reuse, reload, and restart pageLSN returns to `INVALID_LSN`; no persistent
page format was changed.

Commit preparation visits every touched resident page and logs its latest state.
Nonresident pages were necessarily prepared before eviction. Commit then appends
COMMIT and fsyncs WAL through that LSN. This durable COMMIT is the commit point and the
earliest time a success response may be sent. Database pages are not forced: committed
dirty pages may remain in memory and startup REDO reconstructs them after a crash.

Page 0 uses `DatabaseMetadataManager`, which captures and logs the complete metadata
page, forces its PAGE_UPDATE before the existing immediate physical write, and keeps
DiskManager transaction-agnostic. Catalog and free-list root changes cannot bypass WAL.

## In-process rollback

An exception after mutation requires every guard to have released its pin. The buffer
pool discards touched frames without flushing, existing pages are physically restored
from original images, appended pages are discarded and the file is truncated, and the
database file is fsynced. Only then is ABORT appended and fsynced. A durable ABORT thus
means rollback was already stable; recovery can skip that transaction. This strong,
expensive ordering avoids compensation log records while statement errors are uncommon.

## Startup recovery

Startup first selects a durable sharp checkpoint when possible, so production is ordered
as follows:

```text
DiskManager -> deferred LogManager -> checkpoint control -> tail repair -> analysis
            -> REDO -> UNDO -> database fsync
            -> recovered-loser ABORT fsync -> BufferPool -> PageAllocator -> Catalog
```

With a cross-valid control slot, analysis begins at its logical `recoveryStartOffset`;
no earlier transaction record is scanned or replayed. If control is unusable after
reclamation, retained WAL supplies the newest safe checkpoint base. Without a checkpoint
it begins at the oldest retained logical position (byte 64 for unreclaimed WAL). An
incomplete final record is truncated to the last valid record boundary. Interior
magic/version/length/checksum corruption is fatal and is never treated as a tail.
Analysis validates one BEGIN, exact same-transaction `prevLSN` chains, terminal-record
ordering, and the single-active-transaction model.

- A winner has durable COMMIT. Its full after-images or delta after-ranges are applied
  in ascending LSN order.
- A transaction with durable ABORT is skipped because rollback preceded that ABORT.
- A loser has BEGIN/PAGE_UPDATE records but no terminal record and must be the final
  active transaction. Existing-page original images or delta before-ranges are applied,
  then appended pages are truncated using BEGIN's page count.

Recovery runs REDO winners, then UNDO the tail loser, fsyncs the database, and finally
appends/fsyncs the loser's ABORT. Full-page replacement, repeated original before-images,
and delayed ABORT make crashes during REDO, UNDO, or completion safe to retry. Recovery
does no tuple, tree, catalog, or SQL interpretation.

## Failpoints, metrics, and limitations

`MINIDB_FAILPOINT` enables deterministic test-only `_exit(86)` points around record
append/force, database writes, COMMIT, rollback, and recovery phases. Production leaves
the variable unset. Subprocess tests therefore bypass buffer/log destructors and exercise
real reopen behavior.

Recovery statistics additionally report checkpoint-control presence/use, validation
failures, full-scan fallback, recovery start, and skipped/scanned WAL bytes. They report
scanned records/transactions, winners/aborts/losers,
REDO/UNDO/truncation/extension, database writes/syncs, repaired tail bytes, and analysis,
REDO, UNDO, and total nanoseconds. Runtime transaction statistics report logical begins,
commits/rollbacks/zero-write units, first-written pages, per-encoding update counts,
observed logical byte transitions, payload/total WAL bytes, represented bytes, range
counts, record-size samples, delta-computation time, commit fsyncs, and rollback writes.
Benchmarks derive amplification from those observed transitions.

There is a quiescent sharp checkpoint, documented in [checkpoints.md](checkpoints.md).
Obsolete whole WAL segments are now deleted after sharp checkpoints; see
[wal-segments.md](wal-segments.md). There is no archive/PITR, persistent pageLSN, fuzzy
dirty-page table, CLR, concurrent transaction, lock, MVCC, isolation, or crash-safe
group commit. A usable checkpoint bounds startup to its retained tail. A crash after COMMIT fsync but
before the response reaches a client is inherently ambiguous: the statement committed,
but the client must reconnect and query state. Wire request IDs are not deduplication
tokens.

This design is not ARIES. ARIES supports physiological logging, persistent pageLSNs,
repeating history, compensation records, checkpoints, fine-grained locking, and partial
rollback. MiniDB++ instead uses a serial physical baseline chosen for transparent
correctness and experimentation. See the full citation in [wal.md](wal.md).
