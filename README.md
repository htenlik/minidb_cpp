# MiniDB++

A small educational database engine written from scratch in C++20.

The end goal is a TCP-accessible database that accepts a deliberately small SQL subset,
stores records persistently on disk, and uses a B+ tree index for efficient key lookup
and range scans.

## Architecture

```text
TCP client
    |
    v
TCP server
    |
    v
Lexer / Parser
    |
    v
Executor
    |
    +------> B+ Tree
    |           |
    v           v
Record Store <-> Pager
                |
                v
            minidb.db
```

## Milestones

1. **Pager / fixed-size disk pages** ✅
2. **Row serialization/deserialization** ✅
3. Record storage
4. In-memory B+ tree
5. Persist B+ tree nodes into pages
6. SQL lexer
7. SQL parser
8. Query executor
9. TCP server/client protocol
10. Benchmarks, tests, architecture documentation

**Milestone 2.5 — versioned database metadata page** ✅

## On-disk format

```text
MiniDB++ database file

Page 0: database metadata / file header
Page 1: allocatable storage
Page 2: allocatable storage
...
```

New databases begin with one 4096-byte metadata page, so the first normal allocation
returns page ID 1. Existing databases are validated for the MiniDB++ magic, format
version, page size, header size, and whole-page file size before normal pages are exposed.
See [docs/storage-format.md](docs/storage-format.md) for the exact version 1 byte layout.

## Row serialization

Rows contain a 32-bit ID, a username of at most 32 bytes, and an email address of at most
255 bytes. Every row serializes to exactly 294 bytes using this layout:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ID (`uint32_t`, little-endian) |
| 4 | 1 | Username length (`uint8_t`) |
| 5 | 32 | Username bytes, followed by zero padding |
| 37 | 2 | Email length (`uint16_t`, little-endian) |
| 39 | 255 | Email bytes, followed by zero padding |

Explicit serialization produces a stable, deterministic disk format. Writing a C++
`Row` object directly would instead persist implementation details such as `std::string`
pointers and capacity, along with potentially platform-dependent padding and byte order;
those bytes would not reconstruct valid strings in another process.

The 294-byte row layout and the database file format version are separate concerns. The
file header identifies the surrounding database format; row encoding describes how an
individual row is converted to bytes within a future record-storage layer.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/minidb demo.db
```

Example:

```text
minidb> .pages
1 page(s)
minidb> .alloc
allocated page 1
minidb> .flush
flushed
minidb> .quit
```

Restarting with the same file should show that the allocated page still exists.

## Why 4096-byte pages?

Databases generally group storage into fixed-size pages instead of performing arbitrary
small disk operations per record. A page becomes the unit for caching, indexing,
serialization, and disk I/O.

## Scope

This is intentionally not PostgreSQL or SQLite. The point is to implement enough of the
stack ourselves to demonstrate disk/page management, serialization, tree indexing,
query parsing/execution, networking, testing, and benchmarking.
