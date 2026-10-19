# Durable appended-page reservations

A separate [PAGE_RECLAIM system primitive](crash-safe-page-reclamation.md) can publish
certified unreferenced pages independently. It is not connected to reservation-loser
cleanup: user rollback still truncates and free-list reuse ownership remains unresolved.
The [inactive owned rollback protocol](owned-rollback-protocol.md) adds analysis-only
codecs; it does not connect these reservations to production cleanup.

MiniDB++ logs transaction-owned APPENDED reservations before physical extension.
This records provenance, not transaction-local reclamation. SQL still permits only
one exclusive writer; rollback still truncates to the existing BEGIN page count.
Free-list reuse/release ownership and overlapping allocating writers are unresolved.

## Persistent encoding

`PAGE_RESERVE` is stable WAL record type **13**. Types 1–12 retain their bytes and
semantics. The existing 48-byte WAL header carries the nonzero owning TransactionId,
transaction-local prevLSN, global LSN and CRC32C. The payload is exactly 24 bytes;
the complete record is 72 bytes. All integers are explicitly little-endian:

| Offset | Width | Meaning |
| ---: | ---: | --- |
| 0 | 4 | reserved PageId, neither 0 nor INVALID_PAGE_ID |
| 4 | 2 | payload version = 1 |
| 6 | 1 | allocation source = 1 (APPENDED); other values rejected |
| 7 | 1 | flags = 0 |
| 8 | 8 | expected old physical page count, exactly equal to PageId |
| 16 | 8 | reserved = 0 |

Exact length, version, source, flags, reserved bytes and the count/ID relationship
are validated. Comparing a u64 count directly to a valid u32 PageId avoids narrowing
or increment overflow. Record decoding retains existing length/checksum protections;
recovery rejects zero transaction IDs, absent BEGIN, invalid/nonboundary/forward or
cross-transaction prevLSNs, duplicate/out-of-order reservations and post-terminal
records. Reservation IDs increase within a transaction and cannot precede its BEGIN
boundary. Active ownership conflicts are rejected for reservations and updates alike.
This is a forward WAL-type extension: older binaries cannot read type 13. Existing
histories without reservations remain supported; database format v2 is unchanged.

## Authoritative append protocol

```text
buffer metadata availability check, then unlock
  -> read expected next physical PageId
  -> recovery hook validates extent and bound ACTIVE transaction
  -> materialize BEGIN once (original startPageCount)
  -> append PAGE_RESERVE using transaction lastLSN
  -> retain reservation LSN in that transaction's ledger
  -> flushUpTo(reservation LSN): BEGIN + reservation become durable together
  -> buffer metadata availability recheck, existing dirty-victim preparation
  -> DiskManager::appendPage(expectedPageId): atomic check + append
  -> install zero frame, unlock buffer metadata
  -> runtime append confirmation
  -> normal guarded initialization, PAGE_UPDATE preparation and completion
```

The hook executes outside buffer metadata locking. Its extent query finishes before
holding the recovery context mutex; WAL force holds no DiskManager I/O mutex. Physical
append remains under the existing buffer metadata -> disk order. Runtime confirmation
runs outside buffer metadata. The exclusive writer lease remains the serialization
contract; these locks do not authorize simultaneous SQL allocators.

DiskManager rejects a changed expected ID under its I/O mutex **before** writing. It
never substitutes a different ID into an old reservation. Raw administrative appends
must not overlap a live WAL transaction. Educational Pager, setup and recovery-only
physical extension remain distinct paths, as described in
[durable-page-extension.md](durable-page-extension.md).

The first append combines lazy BEGIN and reservation in one force, not two. Each
subsequent append needs its own reservation force; already-durable BEGIN is not
duplicated. A failed pre-materialization attempt may retry the same pending ledger
entry without another record; a materialized reservation cannot append again.

## Ledger and lifecycle facts

Contexts retain a PageId-keyed map containing PageId, reserveLSN, APPENDED source,
expectedOldPageCount and `materializedInRuntime`. Owning TransactionId comes from the
context/WAL header. Statement preparation does not clear this map; transaction
completion does. Snapshots are owned values, not frame pointers or a persistent sidecar.

| Fact | Durable evidence |
| --- | --- |
| RESERVED | forced PAGE_RESERVE with the owning BEGIN/chain |
| MATERIALIZED | runtime callback after successful append only; no separate WAL record |
| INITIALIZED | valid page bytes/PAGE_UPDATE may provide evidence; no lifecycle record |
| COMMITTED | existing durable transaction COMMIT, not proof every reservation materialized |
| ABORTED | existing durable ABORT after legacy UNDO/truncation |

Recovery reconstructs reservation maps and terminal owner state from retained WAL.
It sets materializedInRuntime=false: this means **unknown after restart**, not that
the page was absent. A page currently inside file extent does not alone prove its
historical initialization or committed logical ownership. No durable materialization
counter or allocation epoch is invented.

## Recovery and retention

PAGE_RESERVE has no physical REDO and never enters the DPT. A reservation alone cannot
fabricate a missing page. Normal committed PAGE_UPDATE records reconstruct contents.
UNDO advances through reservation prevLSN without a page CLR. CLR validation similarly
skips reservations when locating the next existing-page update. At BEGIN, unchanged
legacy rollback truncates, synchronizes data, and appends/forces ABORT. Interrupted
UNDO/truncation converges using existing CLR/ABORT rules.

Safe fuzzy ATT captures the ordinary BEGIN/lastLSN, even when lastLSN is PAGE_RESERVE.
ATT's BEGIN retention floor keeps the whole required chain across segment rotation
and reclamation. Half-completed append is checkpoint-unsafe. Sharp checkpoints and
post-completion retention use existing rules/formats. Recovery provenance counters
cover transactions actually reconstructed by analysis; completed history excluded
by checkpoint scanning is not claimed as a reconstructed ledger.

## Diagnostics, tests and cost

Runtime counters: pageReservationsLogged/Committed/Aborted, reservationWalBytes
(72 per record), reservationWalForces (successful force calls for reservations).
Recovery counters: pageReservationsRecovered/Committed/Aborted. No failed-operation
counter is returned from a recovery call that throws, and no durable append-completion
count is claimed.

Tests pin payload/record bytes and corrupt chains, compare 20,000 reservations over
200 histories and four seeds (0x12B3A2, 0xC0FFEE, 0xA110C, 0xDE1A), check segmented
live ATT retention, and run 96 hard-crash cases plus the original orphan regression.
The model persists controlled winner/live-abort/loser histories; the separate fork/_exit
matrix supplies actual hard crashes, including interrupted UNDO and durable ABORT.

The controlled Release benchmark covers existing-page UPDATE, one/four appends and
allocation-heavy SQL INSERT in all three WAL modes. It reports reservation/total WAL
bytes, BEGIN/reservation/fsync counts, throughput and latency percentiles. One/four
appends now require 2/5 transaction WAL fsyncs including COMMIT, versus 2/2 under the
earlier BEGIN-only boundary (no eviction). No separate BEGIN+reservation force variant
is measured; its expected extra first force is not presented as measured performance.

## Remaining restrictions

Reservations alone do not solve loser-owned page reclamation, exactly-once free-list
insertion, shared page-0 metadata, deferred releases or cross-transaction reuse.
`T1 append; T2 append/COMMIT; T1 ROLLBACK` remains unsupported. The separate allocation
audit branch/red tests remain unmerged. There is no non-truncating rollback, multi-writer
SQL, free-list redesign, page-format change, WAL epoch scheme or torn-page repair here.
