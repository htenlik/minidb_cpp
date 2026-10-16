# Crash-safe allocator reclamation

`PageReclaimer` is a restricted, synchronous allocator system operation. It inserts
one certified unreferenced physical page into the existing LIFO free list. It is not
called by SQL, ordinary `releasePage()`, or loser rollback. User rollback still uses
BEGIN.startPageCount truncation. There is still exactly one production SQL writer.

## Publication audit

| Existing path | Sequence / persistence boundary |
| --- | --- |
| releasePage(P) | reject pins/duplicates; dirty buffered Free Page P with next=H; update metadata root=P |
| metadata update | capture page-0 before-image; append/force user PAGE_UPDATE; immediately write page 0 |
| buffered persistence | prepare user update, force WAL, write Free Page; may happen later during eviction/flush |
| free-list reuse | validate head; update/force/write page 0 to pop head; clear allocated page through a write guard |
| system reclamation | one forced PAGE_RECLAIM; write both after-images; database fsync; only then finish |

Ordinary release's two writes are not physically atomic. Its changes remain part of
the user's WAL/CLR history and must not be silently replaced by non-undoable system
actions. The new operation completes an authorized publication independently through
REDO after either partial-write prefix.

## Eligibility and ownership contract

An `UnownedPageCertificate` has a private constructor. **No production issuer exists
yet.** Only the controlled test fixture currently issues certificates, for physical
pages it knows are not referenced by a logical structure. This is not a general
catalog/heap/index reachability proof, and absence of pins alone is never sufficient.
A future lifecycle authority must supply that proof before production integration.

Certificates bind the issuing DiskManager identity, next transaction ID, WAL end,
PageId and current page CRC. Intervening transactions/system operations or changed
bytes invalidate them. These are volatile authorization checks, not persistent
allocation generations. Under the shared database gate's exclusive lease, reclamation
requires no active recovery transaction and no pinned frames. Page 0, nonexistent IDs,
the live catalog root, duplicate members, malformed chains and stale certificates are
rejected. The fixture also rejects pages in its committed-reference model.

All allocator/checkpoint callers must use the same exclusive database gate. Raw
administrative DiskManager writes are not safe concurrent alternatives. Reclamation
cannot overlap a user transaction, including an otherwise safe between-statements
boundary; it is not a nested user/system transaction implementation.

## WAL format

Stable type **14 PAGE_RECLAIM**, TransactionId=0, prevLSN=INVALID_LSN. No BEGIN, COMMIT
or user PAGE_UPDATE is generated for this action. Other transaction records continue
rejecting ID zero; checkpoint records retain their separate system conventions.
The existing 48-byte record header/CRC32C is unchanged. Payload: **48 bytes**, making
the whole record **96 bytes**. Integers are explicitly little-endian:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | reclaimed PageId P |
| 4 | 4 | expected previous head H; INVALID_PAGE_ID means empty |
| 8 | 2 | payload version = 1 |
| 10 | 2 | flags = 0 |
| 12 | 4 | reserved = 0 |
| 16 | 8 | expected physical page count |
| 24 | 8 | target's before PageLSN |
| 32 | 8 | metadata's before PageLSN |
| 40 | 4 | CRC32C of target's complete stable pre-image |
| 44 | 4 | reserved = 0 |

Payload LSN references use the WAL INVALID_LSN=UINT64_MAX sentinel. Persistent page
slots continue encoding unknown LSN as zero. Valid before-LSNs precede this record
and are at least the WAL header boundary. Target/head ranges, distinct IDs, length,
version, flags, reserved bytes and system ownership are checked. Old WAL IDs/encodings
and database format v2 are unchanged; older binaries cannot read type 14.

The pre-image CRC detects a changed certified target; it is not a cryptographic
authorization proof or a page-generation protocol. Trusted issuance authorizes the
logical discard. Recovery does not run a general reachability traversal.

## Execution and two-page REDO

```text
validate certificate, exclusive idle context, pins and free-list membership
flush committed buffered pages; capture stable P / page-0 preconditions
mark system operation active (checkpoint-unsafe)
invalidate resident P / page 0
append PAGE_RECLAIM(P,H,preconditions) -> R
flushUpTo(R)
construct Free Page P: next=H, PageLSN=R, canonical reserved zeros
construct page 0 from valid current metadata: head=P, PageLSN=R
write/flush both pages, then fsync database
finish system operation, permitting checkpoints/new user transactions
```

The Free Page uses the existing 4096-byte layout: magic MDBFREEP, layout version 1,
header size 32, next pointer at 16, PageLSN at 20, remaining bytes zero. Metadata
serialization preserves catalog root, format, page size and other valid header fields.
DiskManager's cached header is updated by its physical page-0 write.

For each physical target independently:

1. PageLSN=R: validate the already-completed state and skip.
2. PageLSN>R: require retained WAL evidence affecting that same physical page and
   matching its full after-image or logged after-ranges, then preserve the later
   state. A numerical comparison or PageId match alone is insufficient.
3. Unknown PageLSN on a zero/opaque reused page: require a later full/delta WAL
   transition from this exact canonical Free Page to the exact current bytes before
   skipping. This covers a crash after reuse but before typed initialization.
4. Otherwise require the logged preconditions, build the after-image and assign R.

Chronological logical replay tracks the head and reclaimed-target images independently
of physical skip decisions. It rejects buried duplicates, inconsistent head/metadata
LSN expectations and stale pre-image fingerprints even when later physical writes
would hide the bad insertion. Normal update and CLR replay follows in WAL order.
After loser UNDO, the final physical free list is validated for canonical links,
membership and cycles. Reservations are not confused with system page updates.

This is recovery-level completion, **not physical two-page atomicity**. Tests invert
the write order with MINIDB_RECLAIM_PAGE0_FIRST; production leaves it unset. A failure
after entering the system operation poisons the coordinator until restart: new user
transactions and checkpoints are rejected, rather than advancing past incomplete work.

## Checkpoints, DPT and WAL retention

System effects are not falsely represented as user DPT entries. A successful operation
has written and fsynced both pages before checkpoint safety is restored. A checkpoint
cannot overlap incomplete reclamation under the shared exclusive-lease contract.
Scanned PAGE_RECLAIM records are independently REDO-eligible, bypassing user DPT
filtering. A later checkpoint may exclude/reclaim their WAL because their complete
effects are durable; subsequent reuse updates/CLRs preserve the necessary before-state.
No checkpoint, segment, ATT, DPT or checkpoint-control format changes are needed.
Lost-control discovery uses the existing authoritative retained checkpoint protocol.

## Tests and measurements

Tests pin type ID, offsets, record size and CRC32C; check duplicate/live/pinned/stale
eligibility; verify both write orders and repeated recovery crashes; preserve committed
typed reuse and zero/typed loser reuse through CLR UNDO in all WAL modes. Directed
chains are 30->20->10 and 50->40->30->20->10. Completed reclamation is tested with
sharp/fuzzy checkpoints, actual segment deletion and lost checkpoint control.

The model runs 20,000 logical reclaim/reuse operations across 100 histories with
seeds 0x123A3, 0xC0FFEE, 0xDE1A, 0xA110C. It hard-crashes 1,000 attempted reclaims at
seven prefixes, sometimes interrupts replay again, then compares every chain prefix
and final recovered membership. This is a serialized physical-fixture model, not a
multi-writer allocator or catalog reachability model.

`page_reclaim_benchmark` reports ordinary release, isolated/10/100 system reclaims,
reclaim+reuse, and interrupted recovery. Ordinary release includes commit, buffer
flush and an explicit data fsync for equivalent durable completion. System reclaim
cost is 96 WAL bytes, one WAL force, two direct page writes and one data fsync, plus
any preceding committed-frame flushes. Reuse has its separate user WAL costs.
Percentiles and throughput are local measurements, never performance assertions.

Replay's diagnostic shadow stores one page image per reclaimed target plus head
state. Opaque-reuse evidence may scan retained WAL; this is a conservative baseline,
not a bounded-memory or optimized recovery index. Torn-page repair, general ownership
epochs, automated loser reclamation and multi-writer publication remain out of scope.
