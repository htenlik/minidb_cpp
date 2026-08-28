# Bounded buffer pool and LRU-K

The single-threaded fixed-capacity BufferPoolManager and guards are the active backend
for PageAllocator, TupleStore,
PersistentBPlusTree, Catalog, Table, SQL, and TCP. The legacy Pager remains an explicitly
historical, unbounded v0.1.x cache used by fixed RecordStore regressions and low-level
comparison benchmarks.

## Pages, frames, and ownership

A `PageId` is a persistent 32-bit disk identity. A `FrameId` is a volatile 32-bit index
into the BufferPoolManager's fixed vector of `BufferFrame` objects. Frame metadata is
never persisted:

```text
BufferFrame
    4096 page bytes
    current PageId
    uint32 pin count
    dirty flag
    valid flag
    pageLSN (volatile; INVALID when unlogged)
```

Construction requires a positive frame capacity and positive LRU-K `K` (default 2).
Resident pages never exceed capacity. An in-memory `unordered_map<PageId, FrameId>`
provides expected constant-time resident lookup. A FIFO deque initially contains every
free FrameId in ascending order, making initial frame assignment deterministic.

```text
PageId -> page table -> FrameId -> BufferFrame bytes
                              -> pin/dirty/valid state
```

Page 0 and `INVALID_PAGE_ID` are never data frames.

## Why guards replace raw references

The legacy API is safe only because its cache never evicts:

```cpp
Page& page = pager.getPage(pageA);
pager.getPage(pageB);
pager.getPage(pageC);
```

With a bounded cache, loading B or C may reuse A's frame. A raw reference could then
silently name another page. A page's bytes may therefore be relied upon only while an
owning guard remains alive. The BufferPoolManager never exposes an unguarded `Page&`.

`BasicPageGuard` owns exactly one pin and provides immutable bytes. `ReadPageGuard`
retains immutable access. `WritePageGuard` provides mutable access. Guards are
non-copyable and movable. Move construction transfers ownership; move assignment first
releases the destination's existing pin and then transfers the source pin. `drop()` is
explicit and idempotent, and destruction releases a still-owned pin exactly once.
Moved-from guards are inert. The BufferPoolManager and DiskManager must outlive guards.

Acquiring a WritePageGuard marks its frame dirty immediately. Calling mutable `data()`
also marks it dirty, which matters when a pinned writer modifies a page after an
intervening flush. Read guards never dirty a page.

## Pin lifecycle and acquisition

Every successful fetch or new-page guard increments a frame pin count and the
`pinOperations` counter. A resident hit records another LRU-K access. While pin count is
positive, the frame is non-evictable. Dropping each guard decrements once; transition to
zero registers the frame as evictable. Underflow is an internal logic error.

`fetchPageRead()` and `fetchPageWrite()` behave as follows:

1. A resident page is a cache hit: record access, pin, and return a guard.
2. A miss uses the first free frame when available.
3. Otherwise, it asks LRU-K for an unpinned victim.
4. A dirty victim is written successfully before its frame identity can change.
5. The requested page is read, installed, pinned, and returned.
6. If every frame is pinned, the API returns empty `std::optional` (`NoFrameAvailable`)
   rather than waiting or evicting a pinned frame.

Invalid/nonexistent PageIds throw. A failed NoFrameAvailable request counts as a normal
page request and cache miss but not as a pin.

`newPageWrite()` first secures a free/evictable frame, then appends a zero-filled page
through DiskManager. It never allocates page 0 and never appends if all frames are
pinned. It is intentionally append-only; the 10B PageAllocator first consumes the
persistent free list and falls back to this path only when that list is empty. The
returned write guard owns the first pin and the frame is dirty.

## Engine migration and typed page views

Active storage objects retain only manager references and stable persistent identities
such as heap/index metadata PageIds. Each operation acquires a guard, constructs a
non-owning typed view or decodes a bounded logical node, copies the required result or
PageIds, then drops the guard. A typed view never owns a pin and must never outlive the
guard whose span created it. No active object stores `Page&`, `Page*`, or a long-lived
span into frame memory.

Read-only metadata, tuple lookup/scan, B+ lookup/range scan, and validators use read
guards. Mutations use write guards, whose acquisition supplies dirty semantics. B+ tree
descent records only `{parentPageId, childIndex}` frames; splits and merges use bounded
one-page logical copies and phase writes by PageId. Recursive validation decodes a node,
drops its guard, and only then visits copied child PageIds.

PageAllocator validates and walks the persisted LIFO free list one read guard at a time.
On release it acquires its own write guard and requires the page's pin count to be exactly
one; an externally pinned page raises `PinnedPageRelease` and is not linked as free. A
reused resident page is zeroed and dirtied before its next typed format is initialized,
so correctness never depends on eviction.

`NoFrameAvailable` is converted at the storage boundary to `StorageError` rather than
dereferencing an empty optional or falling back to Pager. With the supported single-
threaded ownership discipline it diagnoses either an undersized pool or a pin-lifetime
bug. Other focused categories are corrupt page, invalid page, and pinned-page release;
existing public exception behavior was otherwise preserved.

### Engine Migration Pin Budget

The implementation deliberately phases operations so the intended maximum is one pinned
frame at a time. The table is an implementation audit, not a future concurrency promise:

| Operation | Intended simultaneous pins | Discipline |
| --- | ---: | --- |
| TupleStore get / scan | 1 | copy tuple(s) and next PageId before advancing |
| TupleStore insert | 1 | read first-fit candidate, drop, reacquire/write/recheck |
| TupleStore unlink/reclaim | 1 | current, neighbors, metadata, release in separate scopes |
| B+ find / range scan | 1 | decode current node/leaf, copy next PageId, advance |
| B+ insert without split | 1 | logical path contains IDs/child indexes, not guards |
| B+ leaf/internal split | 1 | local bounded node copies; pages written in phases |
| B+ delete / borrow / merge | 1 | decode siblings/parent separately; release after all guards drop |
| Catalog lookup | 1 | catalog metadata and TupleStore accesses are sequential |
| Table insert/update/delete | 1 | heap and index calls are sequential logical coordination |

The complete SQL/storage workflow is guaranteed and tested with three frames and also
passes the current directed two-frame workflow. Individual one-page operations work with
one frame, but one frame is not advertised as the full-engine minimum. Selected tests
assert `pinnedFrames == 0` after storage, validation, SQL, and TCP operation boundaries.

## LRU-K policy

`LRUKReplacer` tracks FrameIds, never PageIds. It uses a monotonically increasing logical
timestamp, not wall-clock time, and keeps at most the most recent K access timestamps
for each resident frame.

At logical time `T`, a frame with K recorded references has backward K-distance:

```text
T - timestamp_of_Kth_most_recent_access
```

A frame with fewer than K references has infinite backward K-distance. Victim priority
is exact and deterministic:

1. infinite-distance candidates precede every finite candidate;
2. among infinite candidates, the earliest recorded access timestamp wins;
3. among finite candidates, the smallest Kth-most-recent timestamp (largest distance)
   wins;
4. equal timestamps use the smaller FrameId as the final tie-break.

K=1 makes every accessed frame finite and selects the least-recently accessed evictable
frame, matching LRU recency under this logical-time model.

Candidate ordering is maintained in `std::set`. `recordAccess()`, evictability changes,
removal, and eviction are O(log F), aside from constant/deque history work for configured
K; victim inspection is O(1). `size()` counts only evictable frames. The validator checks
history lengths/timestamps, tracked counts, candidate uniqueness, evictability, and
agreement between entry metadata and the ordered candidate index.

The deterministic scan-resistance trace makes a repeatedly referenced frame finite
while one-touch scan frames remain infinite; the one-touch frames are selected first.
This verifies policy behavior, not universal performance superiority.

MiniDB++ deliberately simplifies a production LRU-K manager: histories exist only for
currently resident frames and are forgotten on eviction; there is no nonresident/ghost
history, correlated-reference-period filtering, asynchronous I/O, prefetch, admission
policy, concurrency control, or adaptive K.

## Flush and eviction

- Clean eviction reuses a frame without writing it.
- Dirty eviction writes exactly 4096 bytes before page-table/replacer removal and reuse.
- `flushPage(pageId)` returns false for an existing nonresident page and never loads it.
  A dirty resident is written and cleaned; a clean resident performs no physical write.
  Pin state is unchanged.
- `flushAll()` writes every dirty resident, including pinned pages, but never evicts or
  unpins them.
- The destructor attempts `flushAll()` and suppresses errors because destructors cannot
  reliably report them. Explicit `flushAll()` is the error-reporting persistence API.

With an optional WAL provider, a dirty frame carrying a valid volatile `pageLSN` cannot
be written until the provider is durable through that LSN. Dirty eviction and
`flushPage()` force first and preserve the mapped dirty frame if forcing fails;
`flushAll()` forces the maximum required pageLSN once before its database-page writes.
The destructor uses the same path but retains its historical error-suppression policy.
Unlogged `INVALID_LSN` frames retain the low-level compatibility behavior. The frame
field resets on reuse/reload and is not persisted as frame metadata; active page formats
separately encode a persistent PageLSN in their bytes. The coordinator keeps the two
representations aligned for logged writes. See [wal.md](wal.md) and
[page-lsn.md](page-lsn.md).

## Statistics and validation

`BufferPoolStats` contains event counters:

| Field | Definition |
| --- | --- |
| `pageRequests` | Valid read/write fetch attempts; new-page creation is separate |
| `cacheHits` / `cacheMisses` | Resident/nonresident fetch attempts; a no-frame miss remains a miss |
| `physicalPageReads` | Completed DiskManager reads caused by fetch misses |
| `physicalPageWrites` | Completed writes caused by flush or dirty eviction; zero-page append is separate |
| `evictions` / `dirtyEvictions` | Completed frame identity replacements / those whose victims were dirty |
| `pinOperations` / `unpinOperations` | Successful guard pin acquisitions/releases |
| `appendedPages` | Successful `newPageWrite()` physical appends |
| `walFlushRequests` | WAL force calls initiated before logged database-page writes |

Current gauges are `residentPages`, `pinnedFrames`, `evictableFrames`, and `capacity`.
`resetStats()` clears event counters without clearing frames, histories, or gauges. Hit
ratio is derived as `cacheHits / pageRequests` and is zero for no requests; it is not
stored as mutable state.

`BufferPoolManager::validate()` checks bidirectional page-table/frame agreement, unique
PageIds, free-frame state, capacity, no page 0, valid disk bounds, replacer membership,
pin/evictability agreement, and resident/free/statistical gauge counts. Validation does
not mutate the pool.

## Benchmarks

The standalone family retains production-path separation:

- `buffer_sequential`: cyclic ascending reads across the page dataset;
- `buffer_random`: seeded reads within `--working-set` (zero means all pages);
- `buffer_hotset`: seeded reads within a warmed hot set;
- `buffer_scan_resistance`: 80% seeded hot-set reads and 20% sequential one-touch scan
  reads through the remaining dataset.

Use `--buffer-frames N` and `--lru-k 1|2|3`. Results include configuration, hit ratio,
physical I/O, eviction counts, and live gauges. Legacy Pager benchmarks remain available;
their unbounded and bounded memory configurations are not apples-to-apples engine tests.

```bash
./build-release/minidb_bench --benchmark buffer_scan_resistance \
  --pages 10000 --operations 50000 --working-set 32 --buffer-frames 64 --lru-k 1
./build-release/minidb_bench --benchmark buffer_scan_resistance \
  --pages 10000 --operations 50000 --working-set 32 --buffer-frames 64 --lru-k 2
```

The BufferPoolManager and migrated engine remain single-threaded. No mutex, latch, wait
queue, or blocking pin protocol exists. Guards express access mode and lifetime, not
synchronization. Production storage mutations assign PageLSNs through the recovery
coordinator rather than through storage-format code.

## Reference

Elizabeth J. O'Neil, Patrick E. O'Neil, and Gerhard Weikum. “The LRU-K Page
Replacement Algorithm for Database Disk Buffering.” *Proceedings of the 1993 ACM
SIGMOD International Conference on Management of Data*, Washington, D.C., 1993,
pp. 297–306. ACM. DOI: [10.1145/170035.170081](https://doi.org/10.1145/170035.170081).
