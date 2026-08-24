# MiniDB++ wire protocol v1

Protocol v1 is a deterministic, framed binary protocol. It transports SQL source and
structured executor results; it does not transport formatted tables or C++ object
representations. All multi-byte wire integers are **big-endian (network byte order)**.
This deliberately differs from MiniDB++ persistent page formats, which are
little-endian.

## Frame header

Every message begins with this fixed 24-byte header:

| Offset | Width | Field | Encoding / requirement |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `MDBP` |
| 4 | 2 | protocol version | unsigned; currently `1` |
| 6 | 2 | message type | explicit ID below |
| 8 | 8 | request ID | unsigned |
| 16 | 4 | payload length | unsigned; at most 262,144 |
| 20 | 4 | reserved | must be zero |

Message type IDs are stable protocol constants:

| ID | Direction | Message |
| ---: | --- | --- |
| 1 | client → server | `HELLO` |
| 2 | client → server | `EXECUTE_SQL` |
| 101 | server → client | `HELLO_ACK` |
| 102 | server → client | `COMMAND_RESULT` |
| 103 | server → client | `SELECT_RESULT` |
| 104 | server → client | `ERROR_RESPONSE` |

The receiver reads and validates the complete header before allocating its bounded
payload buffer. Unknown magic, versions, message IDs, nonzero reserved bits, and payload
lengths over the limit are rejected.

## Lifecycle and request IDs

A connection begins with an empty `HELLO` carrying request ID 0. The server replies with
an empty `HELLO_ACK`, also ID 0. `EXECUTE_SQL` is legal only after this exchange. Each SQL
request carries a client-selected `uint64` request ID; the single corresponding result or
SQL error echoes it exactly. Requests are processed sequentially. Request IDs reserve no
server-side state and need not be consecutive in v1.

`EXECUTE_SQL` uses the frame payload itself as the SQL byte sequence—there is no redundant
inner length. SQL is limited to 65,536 bytes and is handed unchanged to `SqlEngine`.

## Limits

| Item | v1 limit |
| --- | ---: |
| frame payload (request or response) | 262,144 bytes |
| SQL source | 65,536 bytes |
| one encoded string | 65,536 bytes |
| projected columns | 1,024 |
| materialized result rows | 65,536 |

The frame header is additional to the payload limit. A materialized result that cannot be
encoded within the payload limit produces an `Execution` error; rows are never silently
truncated. The smaller-than-16-MiB limit is deliberate: the educational engine currently
materializes both the executor result and the response and should have a practical,
easily tested memory ceiling.

## Shared encodings

Length-prefixed strings are `uint32 byteLength` followed by exactly that many bytes.

`RecordId` occupies 8 bytes:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | `PageId` |
| 4 | 2 | `SlotId` |
| 6 | 2 | reserved; zero |

Invalid RIDs are rejected.

`ExecutionStats` occupies 28 bytes:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | access-path ID |
| 2 | 2 | reserved; zero |
| 4 | 8 | rows examined |
| 12 | 8 | rows returned |
| 20 | 8 | index lookups |

Access-path IDs are 0 `None`, 1 `HeapScan`, and 2 `PrimaryKeyLookup`.

Values begin with a one-byte tag:

| Tag | Logical type | Bytes following tag |
| ---: | --- | --- |
| 0 | `NULL` | none |
| 1 | `UINT32` | 4-byte unsigned integer |
| 2 | `INT64` | 8-byte two's-complement bit pattern |
| 3 | `BOOLEAN` | one byte, exactly 0 or 1 |
| 4 | `VARCHAR` | `uint32` byte length, then bytes |

Invalid tags, malformed booleans, excessive lengths, truncation, and trailing bytes are
protocol errors.

## Result payloads

### `COMMAND_RESULT`

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | command ID |
| 2 | 2 | flags; bit 0 means inserted RID present |
| 4 | 8 | affected-row count |
| 12 | 28 | `ExecutionStats` |
| 40 | 0 or 8 | optional `RecordId` |

Command IDs are 1 `CreateTable`, 2 `Insert`, 3 `Update`, and 4 `Delete`. Unknown flag
bits are rejected.

### `SELECT_RESULT`

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | projected-column count |
| 4 | 8 | row count |
| 12 | 4 | flags; bit 0 means every row carries a source RID |
| 16 | 28 | `ExecutionStats` |
| 44 | variable | column names, each as a length-prefixed string |
| variable | variable | rows |

Each row contains an optional 8-byte `RecordId`, then a `uint32` value count, then that
many tagged Values. The value count must equal the projected-column count. Projection,
row, and value order are preserved.

## Error payload

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | error-category ID |
| 2 | 2 | flags; bit 0 means source span present |
| 4 | variable | length-prefixed message |
| variable | 0 or 32 | optional source span |

The source span is: start offset (`uint64`), end offset (`uint64`), start line
(`uint32`), start column (`uint32`), end line (`uint32`), and end column (`uint32`). Lines
and columns are one-based. Category IDs are 1 `Protocol`, 2 `Lexer`, 3 `Parser`,
4 `Semantic`, 5 `Constraint`, 6 `Execution`, and 7 `Internal`.

Lexer, parser, semantic, constraint, and ordinary execution errors are nonfatal: another
SQL request may follow on the same handshaken connection. Framing violations and malformed
binary payloads are fatal to that connection; the server attempts a structured Protocol
error when the stream is still safe, then closes it. An unexpected internal database
failure returns a sanitized Internal error and closes the connection.

## Example exchange

```text
client  HELLO          request=0   payload=empty
server  HELLO_ACK      request=0   payload=empty
client  EXECUTE_SQL    request=42  payload="SELECT * FROM users"
server  SELECT_RESULT  request=42  payload=structured columns/rows/stats
client  EXECUTE_SQL    request=43  payload="SELECT * FROM missing"
server  ERROR_RESPONSE request=43  category=Semantic, with source span
```

## Version 1 limitations

There is one materialized response per statement: no row streaming, cursors, pipelining,
prepared statements, compression, authentication, or TLS. The protocol is plaintext and
intended only for trusted local development. A later reviewed version must introduce any
security or streaming features rather than extending v1 implicitly.
