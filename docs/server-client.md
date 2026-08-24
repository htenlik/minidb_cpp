# MiniDB++ TCP server and client

Milestone 8 exposes the existing SQL engine through a small POSIX TCP layer. Production
database execution remains deliberately single-threaded: the server accepts one client,
serves all of that connection's requests serially, closes it, and only then accepts the
next client. Tests may run this loop in a background thread, but no two threads call the
storage engine concurrently.

## Ownership and startup

`DatabaseServer` owns objects in dependency order:

```text
DiskManager -> BufferPoolManager -> PageAllocator -> Catalog -> SqlEngine -> TcpServer
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
    --buffer-frames 128 --lru-k 2
```

`--buffer-frames` and `--lru-k` must both be positive. Defaults are 128 frames and
K=2. The complete supported workflow is regression-tested with three frames and also
passes the current two-frame stress workflow; one frame remains useful for individual
page operations but is not the guaranteed full-engine configuration.

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

After every successfully executed statement, the server materializes/encodes the result,
calls `BufferPoolManager::flushAll()` and then `DiskManager::flush()`, and only then sends
the success response. Operation guards have already left scope at this boundary. Thus a
successful response means dirty resident pages were written through the current C++ file
stream, and clean close/reopen tests are deterministic. This is not crash-safe durability:
the server does not attach the 11A WAL substrate to mutations; there is no database-file
`fsync`, transaction atomicity, or recovery, and unlogged dirty eviction may
persist parts of a multi-page change in any order.

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
