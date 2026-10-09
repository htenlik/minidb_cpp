# Durable BEGIN before physical page extension

MiniDB++ retains one database-wide exclusive SQL writer and the existing 16-byte
BEGIN payload containing `startPageCount`. This fix does not introduce allocation
ownership, new WAL types, or a non-truncating rollback protocol.

## Original crash window

Previously, `newPageWrite()` appended a zero page before the coordinator necessarily
materialized BEGIN. Terminating after append but before page initialization or
statement preparation could leave a physical page with neither a recoverable
transaction boundary nor free-list membership. Startup could not identify a loser.

## Authoritative boundary and ordering

Normal transactional extension now follows:

```text
bound ACTIVE recovery context, original startPageCount retained
    -> prepareForPhysicalPageAppend()
    -> append existing-format BEGIN once, if needed
    -> flushUpTo(BEGIN LSN), only if BEGIN is not already durable
    -> return from recovery hook
    -> acquire buffer metadata, recheck available frame
    -> prepare/flush dirty victim using existing WAL-before-data protocol
    -> DiskManager::appendPage()
    -> install zero buffer frame and create write guard
    -> initialize/change page; normal PAGE_UPDATE preparation and COMMIT
```

The coordinator validates that the named/bound context exists, is ACTIVE, and is not
rolling back. The argument-free bridge identifies the bound physical mutator; a
TransactionId-keyed overload is also available. It marks the boundary unsafe for
checkpointing before materializing or forcing BEGIN. An existing BEGIN is never
duplicated and its transaction-local lastLSN is not reset.

`LogManager::flushUpTo()` supplies the actual WAL fsync. The hook compares durableLSN
against BEGIN before calling it; subsequent appends do not force BEGIN again. It may
flush more buffered WAL than BEGIN alone, but it does not force database pages.
Failure before successful force prevents extension. An all-zero appended page is a
physical mutation even if no PAGE_UPDATE is generated: its durable BEGIN now permits
startup/live rollback to truncate it safely under exclusive-writer ownership.

No blanket eager BEGIN was added to SQL BEGIN or write intent. Existing-page updates
still materialize BEGIN lazily during update preparation; read-only/zero-change scopes
that do not append remain zero-WAL when previously supported.

## Append-path audit

| Path | Transactional extension boundary |
|---|---|
| Heap/index/catalog allocation | PageAllocator -> requireNewPage -> newPageWrite -> hook |
| Table creation, INSERT and relocation | Delegates to the heap/index/catalog paths above |
| Direct newPageWrite with recovery attached | Same hook; no allocator dependency |
| Free-list reuse | Does not extend; existing metadata/page WAL protocol is unchanged |
| Legacy Pager | Non-WAL educational API using raw DiskManager append; no SQL recovery context |
| Benchmark/test direct DiskManager append | Setup/preallocation outside a transaction |
| Recovery writePhysicalPage extension | Recovery-only REDO path for already durable histories |
| DatabaseMetadataManager writePhysicalPage(0) | Existing page 0, not file extension |

Raw DiskManager APIs remain physical primitives, not transaction-aware allocation
APIs. Administrative callers must not bypass the buffer/recovery boundary for live
WAL transactions. DiskManager stays independent of LogManager and transaction policy.

## Lock ordering

Buffer metadata is briefly acquired to check availability, then released before the
hook. The hook holds the recovery context mutex and accesses WAL, **not** buffer
metadata or DiskManager. It releases that mutex before the buffer reacquires metadata
and appends under the existing metadata -> disk order. Availability is rechecked
because raw concurrent buffer callers might consume a frame during the force.

Existing dirty eviction remains metadata -> recovery preparation -> WAL, followed by
disk I/O. Commit preparation releases context metadata before invoking buffer methods;
live rollback discards frames before holding its UNDO context lock. No new reverse
context -> buffer wait is introduced by the append hook. SQL admission/session locking
continues to serialize writer privileges; this change does not make raw concurrent
allocator operations safe.

## Recovery, checkpoints and compatibility

An abandoned append now has a durable BEGIN. Existing CLR UNDO follows its chain and
truncates to the original boundary, then forces ABORT. That remains a global-file
operation and is safe only with the existing exclusive writer restriction. Existing
PageLSN assignment, selective REDO, WAL modes, legacy histories and database formats
are unchanged. Live fuzzy ATT captures the newly materialized BEGIN only after a
successful statement is prepared; half-completed append remains checkpoint-unsafe.

The unsupported overlapping history remains:
`T1 allocates P101; T2 allocates/commits P102; T1 rolls back`.
Durable BEGIN does not provide per-page ownership or prevent legacy truncation from
destroying P102. The separate audit branch and its red interleaved test are preserved
unchanged and are not part of this branch's CTest suite.

## Verification and cost

Focused tests observe the physical WAL BEGIN and durableLSN while file extent is
still unchanged. A second thread obtains buffer metadata during the hook, testing
the latch boundary. Tests cover buffered/already-durable BEGIN, owner rejection,
ABORTING rejection, exactly one BEGIN, prevLSN preservation, reuse rollback and live ATT.

Hard subprocess tests cover before append/force, after force, immediately before/after
extension, after installation/zero initialization, before/after PAGE_UPDATE, before
COMMIT and after durable COMMIT. Eleven points x three WAL modes x two storage modes
give 66 cases, plus the exact original empty-database/P1 regression. Final recovered
state, not temporarily unlinked pages in an active transaction, determines success.
SQL tests include 100 append/INSERT/ROLLBACK cycles, catalog/allocator/index validation,
read-only zero WAL, one-BEGIN explicit inserts, and committed close/reopen structures.

`durable_page_append_benchmark` compares existing-page UPDATE, first append, and four
appends for FullPage/ByteRange/Adaptive, with 20 samples per combination. It excludes
WAL header-creation fsync and reports mutation/allocation and commit p95 nanoseconds.
With eight frames and no eviction, expected transaction WAL fsync counts are 1 for
existing-page UPDATE and 2 for either append workload: one first-BEGIN force and one
COMMIT force. Latencies are machine-specific; extra first-allocation durability is
not free. No timing threshold or universal speed claim is made.

## Remaining boundary

Transaction-owned reservation/reuse/release provenance, restartable allocator
compensation, safe shared-free-list publication and overlapping allocation rollback
remain separate work. This fix is not a power-loss/torn-page recovery guarantee,
allocation epoch protocol, multi-writer implementation, or replacement for the
existing writer gate.
