# Compensation log records and restartable UNDO

MiniDB++ records recovery-time loser compensation in the WAL. A compensation log
record (CLR) is a physical, REDO-able description of the page after one original
update has been undone. A CLR is never itself undone. When UNDO reaches a CLR, it
continues at `undoNextLSN`; this makes completed recovery work durable across repeated
recovery crashes.

This is deliberately narrower than ARIES. MiniDB++ still permits one serial implicit
statement transaction, uses physical page logging, has no locks or physiological
operations, and does not expose user-managed transactions.

## Stable type and payload

The existing stable WAL type ID `5` is `COMPENSATION`. The record uses the ordinary
48-byte WAL header and CRC32C. Its `TransactionId` is the loser transaction ID, never
zero. Its `prevLSN` is the latest record in that transaction's WAL chain when recovery
appends the CLR.

The version-1 CLR payload is exactly 4136 bytes, little-endian:

| Offset | Width | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | compensated `PageId` | valid existing page |
| 4 | 4 | flags | bit 0 `PAGE_EXISTED`, bit 1 `PAGE_SUPPORTS_LSN`; no other bits |
| 8 | 4 | page size | exactly 4096 |
| 12 | 2 | payload version | `1` |
| 14 | 2 | header size | `40` |
| 16 | 8 | `undoNextLSN` | next original record/BEGIN, or `INVALID_LSN` |
| 24 | 8 | `compensatedUpdateLSN` | original update represented by this CLR |
| 32 | 8 | reserved | zero |
| 40 | 4096 | compensated page image | physical logical state after undo |

The compensation image uses one canonical full-page representation independent of
whether the original was `PAGE_UPDATE`, `PAGE_DELTA_UPDATE`, `PAGE_UPDATE_V2`, or
`PAGE_DELTA_UPDATE_V2`. For a PageLSN-aware page its PageLSN slot is normalized to zero
in the payload. Applying or redoing the CLR installs the CLR record LSN into that slot.
Legacy pages without a PageLSN slot remain replayable but cannot persist that metadata.

`compensatedUpdateLSN` must precede the CLR and identify a same-transaction update for
the same page. `undoNextLSN` must equal that update's `prevLSN` and point backward.
Unknown versions, flags, sizes, reserved bytes, invalid IDs, future/self references,
trailing bytes, and mismatched update references are corruption.
The pinned representative codec vector in `clr_codec_test` has outer CRC32C
`0x568F9E4A`; changing it requires an explicit persistent-format review.

## WAL chain and UNDO

For `BEGIN -> U1 -> U2`, interrupted recovery can produce:

```text
BEGIN -> U1 -> U2 -> CLR(U2) -> CLR(U1) -> ABORT
                    |             |
                    v             v
                   U1           BEGIN
                 undoNext       undoNext
```

Recovery analysis includes each CLR in the transaction's `lastLSN`. REDO treats a CLR
as page-affecting physical WAL. UNDO begins at the loser `lastLSN`; an update produces
a CLR and continues at the update's `prevLSN`, while a CLR is not inverted and instead
jumps directly to its `undoNextLSN`. Only after traversal and idempotent appended-page
truncation complete does recovery append and force ABORT.

For each existing-page compensation the exact durability order is:

1. construct the compensated logical page image;
2. append the CLR and obtain LSN `C`;
3. force WAL through `C`;
4. install `PageLSN = C` in the page when supported;
5. write and sync the compensated page;
6. continue at `undoNextLSN`.

A crash before the CLR is forced may repeat the original compensation. A crash after
the force but before the page write is repaired by CLR REDO. If the page write reached
disk, selective REDO skips it when `PageLSN >= C`. A crash after the final CLR but
before ABORT follows its terminating `undoNextLSN` and only completes ABORT.

Updates to pages appended after BEGIN do not emit meaningless page CLRs. Recovery skips
their page inverses and truncates to BEGIN's `startPageCount`. Truncation to the same
count is idempotent, so a crash after truncation but before ABORT safely repeats it.

## Checkpoints, DPT, and segments

A CLR is page-affecting for REDO and Dirty Page Table analysis; conservatively, a page
not already present enters the restart DPT at the CLR LSN. Sharp and fuzzy checkpoint
record formats are unchanged. Production checkpoints cannot overlap the one active
statement or recovery, so segment reclamation cannot run while a loser/CLR chain is
needed. After durable ABORT, a later checkpoint may advance the ordinary retention
floor past the obsolete loser history. Retained-WAL checkpoint discovery accepts CLRs,
including after checkpoint-control loss.

## Metrics and cost

Recovery reports user-update REDO separately from CLR REDO, PageLSN skips for each,
original UNDO records visited/compensated, CLRs encountered/appended, CLR-directed
skips, restart count, compensation page writes, and CLR WAL bytes. The canonical CLR
cost is 4184 encoded bytes (48-byte WAL header plus 4136-byte payload) per compensated
existing-page update. This intentionally favors a simple verifiable recovery record
over WAL volume. See [benchmarking.md](benchmarking.md) for the controlled workload.

CLRs make recovery progress restartable, not crash-atomic as a whole. There is still no
torn-page repair, archive/PITR, concurrent transaction recovery, or lock/MVCC protocol.
