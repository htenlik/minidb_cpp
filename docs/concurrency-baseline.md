# Concurrent readers and one writer

MiniDB++ accepts concurrent TCP sessions and permits overlapping read-only execution.
One database-wide shared/exclusive gate protects logical contents: many readers **or**
one write-capable transaction. This is coarse database locking, not a fine-grained lock
manager, row/predicate locking, or MVCC.
Use one engine/server instance per database file; this gate is process-local and does
not coordinate separate processes opening the same database.

## Access and isolation

| Operation | Database lease | Lifetime |
|---|---|---|
| Autocommit SELECT | shared | through complete result materialization |
| Autocommit mutation | exclusive | through durable commit/error rollback |
| BEGIN / BEGIN TRANSACTION / BEGIN READ WRITE | exclusive | until completion/disconnect |
| BEGIN READ ONLY / BEGIN TRANSACTION READ ONLY | shared | until completion/disconnect |
| Manual checkpoint | exclusive | through publication and reclamation |

BEGIN defaults to READ WRITE. Access mode cannot change during a transaction. INSERT,
UPDATE, DELETE, and CREATE in READ ONLY return execution errors without modifying WAL,
ending the transaction, or upgrading its lease. Multiple READ ONLY transactions can
coexist; their contents remain stable because writers cannot enter. A READ WRITE scope
excludes all other database operations and supports its existing read-your-writes rule.
Schedules are equivalent to serialization at the database boundary. There are no
fine-grained Strict 2PL, MVCC snapshots, selectable isolation levels, or savepoints.

`DatabaseAccessGate` uses a mutex and condition variable. Shared admission requires
no active writer **and no waiting writer**. Exclusive admission requires no readers or
writer. This preference also protects checkpoint acquisition from a continuing stream
of new readers. Writers are not FIFO; readers can wait under sustained writer demand.
Existing long-lived read-only leases can delay a writer indefinitely.

SQL never acquires a second database lease while already owning one and never upgrades.
Only one logical database resource is acquired, so this protocol cannot form a lock
cycle. Same-thread recursive exclusive acquisition/upgrades return errors. Direct gate
users must also avoid acquiring another lease while holding a transferred lease.

## Sessions, recovery, and checkpoints

SqlEngine owns one TransactionManager with independently serialized session contexts,
not one global optional transaction. Each context retains its access mode and movable
lease. READ ONLY has no RecoveryCoordinator context or WAL transaction ID. READ WRITE
alone owns the existing physical context, before-images, transaction ID, and prevLSN
chain. Exclusive access proves that at most one WAL-producing user transaction exists.
Writer ownership is checked at recovery boundaries; diagnostics are cached snapshots,
not concurrent reads of mutable recovery state.

The existing transaction-wide startPageCount/truncation remains safe precisely because
no other writer can append/reuse pages concurrently. Startup still handles at most one
user loser. No interleaved writer chains, transaction-local allocation rollback, or
multi-loser recovery were introduced.

COMMIT retains exclusion through WAL force and cleanup. ROLLBACK/disconnect retain it
through CLR compensation, database synchronization, and durable ABORT. READ ONLY
completion releases its shared lease without transaction WAL or completion page writes.
SELECT returns owned rows/values/RIDs; network encoding subsequently reads only that
materialized result. Read cache eviction may write an earlier committed dirty frame;
it does not generate a read-only mutation or enter mutable writer recovery state.

Manual checkpoints reject an active writer, and otherwise queue for exclusive access
behind READ ONLY scopes. Calling checkpoint while retaining a shared lease on the same
thread is a prohibited upgrade. Automatic writer thresholds remain pending during its
explicit scope and execute under the still-owned exclusive lease after completion.
Failed automatic attempts remain pending for the next writer boundary. Production
checkpoint ATT remains empty. Publication/reclamation cannot overlap SQL readers.

## Buffer and disk synchronization

The buffer metadata latch protects page table, free frames, pins, dirty/recLSN state,
statistics, and all LRU-K operations. The replacer has no redundant internal mutex.
This latch also serializes miss I/O and dirty victim preparation. Two simultaneous
misses cannot install separate copies of one page.

Every fixed frame has a `std::shared_mutex`: read guards hold shared content access;
write guards hold exclusive access. Fetch reserves a pin under metadata, releases
metadata, then waits for the content latch. Write intent/dirty marking happens only
after exclusive content ownership. Guard drop unlocks content **before** unpinning.
Thus no metadata owner waits for a frame latch while its owner needs metadata to drop.
Pinned reservations cannot be evicted, including those waiting for content access.

Repeated read guards for the same frame/thread share one content lock and retain
separate pins, avoiding undefined recursive locking of std::shared_mutex. Guards may
move within their acquiring thread; cross-thread release is rejected. Read-to-write
frame upgrades and recursive writes are rejected. Dirty flush/snapshot preparation
requires releasing the relevant guards; a flush cannot safely inspect bytes exposed
by an active writer. Tests cover this contract and unwind failed pin reservations.

Production concurrent reads wait for transient pin pressure when all frames are busy.
Standalone pools keep the existing optional NoFrameAvailable behavior by default.
Each storage read advances with one guard at a time, so normal reader pressure can
make progress. Do not retain external guards while calling a waiting read operation.

DiskManager protects its shared fstream position and all I/O/header/count state with
an internal latch. Header and statistics APIs return snapshots. The order is database
lease, checkpoint latch where applicable, buffer metadata, disk I/O; frame waits occur
outside buffer metadata. LRU-K/history selection and disk misses are still serialized
and may limit scaling. Frame latches are not structural B+ tree latch coupling.

LogManager is not a general concurrent appender. User append/commit/UNDO and checkpoint
work are serialized by exclusive access. Reader eviction may consult/force earlier
committed WAL only under the buffer metadata latch; no writer/checkpoint overlaps it.
Standalone WAL/recovery administrative APIs require quiescence or caller-owned exclusive
access. Catalog/Table/heap/index reads use per-operation decoded copies and cursors;
there are no mutable shared scan cursors or full-index mirrors. Their direct mutations
still require caller synchronization outside SqlEngine.

## TCP lifecycle

The accept loop owns joinable `std::jthread` workers, one per admitted connection.
`--max-connections` defaults to 16; excess connections receive a protocol error during
handshake and close. Finished workers are reaped, and released capacity is reusable.
Session IDs are allocated under the lifecycle mutex, never reused, and checked before
overflow. Requests remain sequential within one connection. A MiniDbClient object is
owned by one caller thread; separate clients execute concurrently.

Gate waiters inspect cancellation/peer state without consuming protocol bytes. Peer
checks occur at most 10 ms apart while waiting; shutdown notifies waiters immediately.
On macOS/Linux, TCP state also detects FIN behind unread queued bytes; other POSIX
platforms retain poll/EOF/error detection.
Disconnect cancels admission, releases a read-only lease, or rolls back the active
writer durably before releasing its exclusive lease. Cleanup failure stops service.
Embedded `closeSession` also signals cancellation before taking the session latch, so
closing a waiting session does not wait behind its blocked BEGIN.

Shutdown stops admission, cancels gate waiters, requests worker stop, and shuts down
client sockets to wake blocking I/O. Worker cleanup releases readers/rolls back the
writer; workers join before storage objects are destroyed. An in-flight statement or
fsync is allowed to finish. The low-level serveConnection API borrows an externally
owned socket/thread; its caller must join that thread before destroying the database.
Server instances cannot restart after close; construct a new instance to reopen.

## Diagnostics and verification

Gate snapshots report active/peak readers, writer state, shared/exclusive admissions,
current waiters, waiting admission counts and accumulated nanoseconds, and cancelled
acquisitions. Server snapshots report active/peak sessions, admissions, cap rejections,
and peer disconnects detected during access acquisition. Buffer snapshots add actual
frame-availability wait calls and failed content-latch try-lock counts. These counters
describe events, not SQL isolation levels or CPU utilization.

Tests prove eight overlapping READ ONLY transactions and real TCP autocommit overlap
through lease instrumentation. They cover fairness, writer exclusion, mutation policy,
waiting disconnects, durable writer cleanup, connection limits, shutdown, duplicate
loads, and guard contention. The immutable buffer model checks 128,000 operations over
eight threads/four frames with seeds 0x12B10001..0x12B10004. Separate ThreadSanitizer and
ASan/UBSan runs exercise the actual implementation.

See [transactions.md](transactions.md), [buffer-pool.md](buffer-pool.md),
[server-client.md](server-client.md), and [benchmarking.md](benchmarking.md).
