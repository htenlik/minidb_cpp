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

1. **Pager / fixed-size disk pages**  <- current
2. Row serialization + record storage
3. In-memory B+ tree
4. Persist B+ tree nodes into pages
5. SQL lexer + parser
6. Query executor
7. TCP server/client protocol
8. Benchmarks, tests, architecture documentation

## Current milestone

The pager owns a database file divided into fixed-size 4096-byte pages. It can create/open
a file, allocate pages, lazily load pages, track dirty pages, flush changes, and recover
written page data after reopening the file.

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
0 page(s)
minidb> .alloc
allocated page 0
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
