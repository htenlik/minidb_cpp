# Owned rollback WAL analysis (inactive)

The owned rollback extension currently provides codecs and a **side-effect-free
history validator only**. Production emits ordinary BEGIN, uses CLR UNDO and
global truncation, and retains its exclusive writer gate. There is no public
protocol switch. Recovery rejects owned records in its recovery window before
tail repair, page REDO, CLR generation or physical cleanup. This is not an owned
rollback implementation or a production activation gate being declared passed.

## Prototype-compatible bytes

The existing 48-byte WAL header, checksum, types 1–14, database v2 and all
checkpoint/page layouts are unchanged. These additional user-owned records have
nonzero TransactionId; all integers are little-endian. Common payload prefix:
offset 0:u16 version=1, 2:u16 exact payload size, 4:u32 flags=0.

| ID / record | Payload bytes | Remaining offsets:width and meaning |
| --- | ---: | --- |
| 15 BEGIN_OWNED | 24 | 8:8 original startPageCount; 16:8 reserved=0 |
| 16 ROLLBACK_CLEANUP_BEGIN | 24 | 8:8 owned BEGIN LSN (logical UNDO endpoint); 16:8 reservation count |
| 17 ROLLBACK_CLEANUP_INTENT | 72 | 8:8 reserveLSN; 16:8 associated system reclaimLSN; 24:48 unchanged PAGE_RECLAIM payload |
| 18 ROLLBACK_CLEANUP_PROGRESS | 40 | 8:8 reserveLSN; 16:8 reclaimLSN or INVALID_LSN for absence; 24:4 PageId; 28:4 reserved=0; 32:8 observed extent |

Complete record widths are 72, 72, 120 and 88 bytes. The nested reclaim fields
are page 24:4, prior free head 28:4, version 32:2, flags 34:2, reserved 36:4,
extent 40:8, before-pageLSN 48:8, before-metadataLSN 56:8, CRC32C 64:4,
reserved 68:4. See [the type-14 contract](crash-safe-page-reclamation.md).

The BEGIN prevLSN is INVALID_LSN. Other new records follow the owner's exact
lastLSN. A system PAGE_RECLAIM has TransactionId=0 and never becomes that user's
prevLSN. The intent explicitly names its immediately following record boundary;
`cleanupReclaimLsn()` is the single checked calculation. It does **not** append,
reserve a WAL position, or complete a missing record. An intent-only scanned tail
remains unfinished evidence. Restart completion of that tail is future work.

Decoders check identity, exact length/version/flags/reserved fields, legal page
counts/IDs, backward references, overflow and nested reclaim encoding. Encoders
reject invalid payload values instead of silently canonicalizing them. The real
WAL codecs/scanners still handle outer record boundaries, checksums, unsupported
future IDs, malformed lengths and torn tails. Older binaries cannot read IDs 15–18.

## Analysis and ownership evidence

`analyzeOwnedRollbackHistory()` accepts an existing scanner's complete, contiguous,
BEGIN-rooted record sequence. It is not a replacement for normal checkpoint-aware
recovery, a scanner, or an arbitrary retained-suffix validator. It returns owned
value snapshots and has no DiskManager, LogManager writer, gate or reclaimer.

Each transaction has explicit LegacyTruncate/OwnedAppend discrimination. Each
APPENDED reservation binds TransactionId, PageId, reserveLSN and expected old
extent through the validated user chain. Page updates must have matching existence
flags; an owned appended update needs its reservation. Preexisting updates,
including free-list reuse/release, remain separate before-image/CLR work. No new
reuse-source reservation or claim of complete reuse provenance is introduced.

`materializedInRuntime=true` exists only in the existing live append-confirmation
context. Analysis always reconstructs false, meaning **unknown after restart**,
not absent. A recorded absence outcome is also distinct from runtime confirmation.
Neither matching PageId, CRC, extent nor absence of pins supplies cleanup authority.

The validator checks exact transaction-local chain/boundary ownership, conflicts,
duplicate BEGIN/reservations, matching CLR owner/page/original update/undoNext and
the next required UNDO operation. It rejects skipping or repeating existing-page
UNDO, mixing legacy cleanup, mutation/COMMIT after cleanup and post-terminal records.

## Cleanup state machine: claims versus authority

```text
OwnedActive -> [Undoing through ordinary CLRs]
            -> LogicalUndoComplete (verified chain endpoint)
            -> CleanupStarted
            -> ReservationCleanupInProgress -> CleanupComplete
            -> AbortRecorded
```

The CLEANUP_BEGIN marker proves the **WAL-chain** endpoint only. It does not prove
that compensated page bytes have been synchronized. Each intent must identify a
unique owned reservation and exactly matching subsequent type-14 record. Reclaim
progress requires that association and matching extent; repeated completion fails.
Empty reservation sets complete at the marker.

There is one deliberate prototype exception: absent-page progress has INVALID
reclaimLSN and no intent, with PageId >= observed extent >= original extent. This
records an absence claim, not a physical reclaim. It must not be confused with
progress missing its required intent for a materialized page.

`AbortRecorded` means a complete terminal WAL record was supplied. The analyzer
cannot observe WAL fsync and does not label it durable. The future executor must
establish physical absence, exclusive lifecycle authority, logical image restoration,
page-0 synchronization and free-list completion **before** recording/forcing ABORT.
The validator neither issues certificates nor authorizes PAGE_RECLAIM alongside a
live transaction. It only checks that an explicitly associated proposed history is
structurally valid; physical reclamation remains disabled.

## Remaining execution boundary

Future work must bind an exclusive lease, preflight ownership-scoped unpinned
invalidation, complete/force user CLRs and data sync, recover interrupted intents,
publish each free page exactly once, and force final ABORT only after all progress
is durable. Page-0 updates must preserve unrelated metadata and PageLSN. Cleanup
must block checkpoints and retain its full required WAL until complete; current
writeback/retirement protections must not be replaced or bypassed.

This analysis does not certify physical CLR after-images, free-list membership,
reachability, tail completion or crash recovery for the new protocol. Those are
execution-stage proof obligations. Legacy recovery and its truncation/winner guards
are unchanged. No multiple-writer allocation, epochs, deferred release publication,
new page format, SQL, or wire changes are included.
