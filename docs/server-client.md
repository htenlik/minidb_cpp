# MiniDB++ TCP server and client

MiniDB++ exposes its SQL engine through a POSIX TCP layer with bounded, joinable client
workers. Connections run concurrently; requests within each connection remain sequential.
Read-only database access may overlap. One database-wide exclusive writer excludes all
other database operations. See [concurrency-baseline.md](concurrency-baseline.md).

## Ownership and startup

`DatabaseServer` owns objects in dependency order (with the checkpoint control sidecar
opened before recovery):

```text
DiskManager -> LogManager -> RecoveryManager / RecoveryCoordinator
            -> BufferPoolManager -> CheckpointManager
            -> PageAllocator -> Catalog -> SqlEngine -> TcpServer
```

It opens or creates the database, uses the existing catalog bootstrap, then listens.
Reverse destruction keeps every referenced object alive for its consumer. The stable
database file remains the source of truth; the network layer does not maintain a shadow
database.

The default endpoint is `127.0.0.1:7432`. Port 0 is supported through the library for
collision-free tests, and `TcpServer::port()` reports the selected port.

## Server CLI

```bash
./build/minidb_server demo.db
./build/minidb_server demo.db --host 127.0.0.1 --port 7432 \
    --buffer-frames 128 --lru-k 2 --checkpoint-wal-bytes 67108864 \
    --checkpoint-statements 0 --wal-segment-bytes 16777216 --max-connections 16
```

`--buffer-frames` and `--lru-k` must both be positive. Defaults are 128 frames and
K=2. The complete supported workflow is regression-tested with three frames and also
passes the current two-frame stress workflow; one frame remains useful for individual
page operations but is not the guaranteed full-engine configuration.
`--max-connections` must be positive and defaults to 16. Excess connections receive an
ERROR_RESPONSE during handshake and close; idle connections count toward the cap.
Released capacity is reusable. Workers are never detached, and completed workers are
reaped. Concurrent reads wait for transient frame pressure in the production pool.
`--checkpoint-wal-bytes` is the approximate WAL growth after the last completed sharp
checkpoint (default 64 MiB), and `--checkpoint-statements` is an optional successful
mutating-statement count. Zero disables either trigger. Policy runs only after COMMIT;
there is no background or mandatory shutdown checkpoint. `--wal-segment-bytes` selects
the fixed WAL segment payload capacity (default 16 MiB). See
[checkpoints.md](checkpoints.md) and [wal-segments.md](wal-segments.md).

Startup prints the database path, bound address/actual port, and protocol version.
Binding another address is an explicit operator choice.

## Client CLI

Interactive mode keeps one connection open and accepts one statement per input line:

```bash
./build/minidb_client --host 127.0.0.1 --port 7432 --stats
```

One-shot execution is suitable for scripts:

```bash
./build/minidb_client --port 7432 --execute "SELECT * FROM users;" --stats
```

SELECT results are formatted as a table; `NULL` prints literally. Commands print their
kind and affected-row count. `--stats` adds the access path, rows examined/returned, and
index lookup count. Formatting exists only in the CLI—the client library returns the
structured `QueryResult`.

## Reproducible demo

Terminal 1:

```bash
./build/minidb_server demo.db --port 7432
```

Terminal 2:

```bash
./build/minidb_client --port 7432 --stats
```

Enter each statement on one line:

```sql
CREATE TABLE users (id UINT32 PRIMARY KEY, username VARCHAR(64) NOT NULL);
INSERT INTO users VALUES (1, 'alice');
INSERT INTO users VALUES (2, 'bob');
SELECT * FROM users;
SELECT username FROM users WHERE id = 2;
```

The final query reports `PrimaryKeyLookup` and one index lookup.

## Persistence and reconnects

By default each mutating statement is one implicit transaction. `BEGIN [TRANSACTION]`
defaults to an exclusive READ WRITE transaction owned by that connection; subsequent requests share
its transaction ID and WAL chain until `COMMIT` or `ROLLBACK`. Intermediate statement
success is not a durable commit. COMMIT forces WAL, not database pages; ROLLBACK uses
restartable CLR UNDO and forces ABORT before returning. `SELECT` reads the connection's
own changes without globally flushing. A mutation execution error rolls back the whole
scope; parser and SELECT errors leave it active.

`BEGIN [TRANSACTION] READ ONLY` retains shared access and permits other readers.
Mutations in that scope are rejected while leaving it active; no lock upgrade occurs.
READ WRITE transactions, including those currently only reading, exclude other sessions.
Results are fully materialized before an autocommit SELECT releases its shared lease;
TCP encoding never lazily reads storage afterward.

EOF and protocol failures release a read-only scope or roll back the writer before
releasing its exclusive lease. Disconnected gate waiters are cancelled. Graceful
`DatabaseServer::close()` stops admission, wakes socket/gate waits, joins cleanup, and
rolls back; it never commits implicitly.
If cleanup fails, the server stops accepting work and requires reopen/recovery. A hard
crash leaves startup recovery to undo the loser. A crash after COMMIT fsync but before
the response can leave a committed transaction without an acknowledgement. See
[transactions.md](transactions.md) and [recovery.md](recovery.md).

A client connection can carry many sequential requests. A normal SQL error does not end
the session. Clients may disconnect and reconnect with a new HELLO exchange; later clients
see the same database state. A clean frame-boundary disconnect is normal. A mid-frame
disconnect or malformed frame kills only that connection, and the accept loop proceeds to
the next client.

Transport loops handle short `recv`/`send` calls and `EINTR`. Apple platforms use
`SO_NOSIGPIPE`; platforms providing `MSG_NOSIGNAL` use it per send. Broken peers become
exceptions scoped to the connection rather than terminating the process.

## Client library

```cpp
minidb::net::MiniDbClient client("127.0.0.1", 7432);
client.connect();
client.handshake();
minidb::sql::QueryResult result = client.execute("SELECT * FROM users;");
client.close();
```

An overload accepts an explicit `uint64` request ID. Remote failures throw
`RemoteSqlError`, which carries the echoed request ID, stable category, message, and
optional source span.

## Security warning

Protocol v1 is plaintext and has **no TLS, authentication, or authorization**. Its
loopback default is intentional. Do not expose it to an untrusted network. Security needs
a separate reviewed design rather than homemade cryptography.

## Complexity

Frame I/O is O(payload bytes), codec work is O(encoded result size), and memory is
O(frame payload + the executor's materialized `QueryResult`). SQL execution keeps the
existing engine complexity. Connections and SQL requests are serial, so there is no
concurrent-query throughput claim.
