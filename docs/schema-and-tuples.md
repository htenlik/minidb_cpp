# Schema and canonical logical tuples

Milestone 5B adds a logical relational representation above opaque `TupleStore` bytes.
`SlottedPage` and `TupleStore` remain schema-free. All multi-byte persistent integers
below use little-endian encoding, and no C++ object or STL representation is persisted.

## Identifiers and types

Unquoted table and column identifiers use ASCII `[A-Za-z_][A-Za-z0-9_]*`, contain at
most 63 bytes, and are normalized to ASCII lowercase before duplicate detection or
persistence. Quoted and case-sensitive identifiers are not supported.

Persistent type IDs are stable and independent of C++ RTTI:

| ID | Type | Logical C++ value | Persistent field encoding |
| ---: | --- | --- | --- |
| 1 | `UINT32` | `std::uint32_t` | 4-byte unsigned little-endian |
| 2 | `INT64` | `std::int64_t` | 8-byte little-endian two's-complement bit pattern |
| 3 | `BOOLEAN` | `bool` | one byte: `0` false, `1` true |
| 4 | `VARCHAR(maxBytes)` | `std::string` | 4-byte length followed by exact bytes |

`Value` is `variant<monostate, uint32_t, int64_t, bool, string>`; `monostate` means
NULL. String contents are opaque bytes. No UTF-8 validation or normalization occurs,
and VARCHAR limits count bytes, not characters. A declared VARCHAR limit is 1–4000.

A schema has 1–1024 columns, unique normalized names, at most one primary key, and no
VARCHAR limit on fixed-width types. A Milestone 5B primary key must be non-nullable
`UINT32`, matching the persistent B+ tree key type. Schema objects are immutable after
validated construction.

## Schema encoding: version 1

Magic: ASCII `MDBSCHMA`.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `MDBSCHMA` |
| 8 | 4 | Schema encoding version (`1`) |
| 12 | 4 | Header size (`24`) |
| 16 | 2 | Column count |
| 18 | 2 | Flags/reserved (`0`) |
| 20 | 4 | Total encoded byte size |
| 24 | variable | Column definitions in schema order |

Each column definition begins with this fixed eight-byte prefix, followed immediately
by `nameLength` normalized name bytes:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| +0 | 2 | Name byte length |
| +2 | 1 | Stable data-type ID |
| +3 | 1 | Flags: bit 0 nullable, bit 1 primary key; other bits zero |
| +4 | 4 | VARCHAR maximum bytes, or zero for non-VARCHAR |
| +8 | variable | Normalized name bytes, not NUL-terminated |

Decoding applies the same identifier, type, nullability, primary-key, and duplicate-name
validation as fresh schema construction and rejects non-canonical or trailing bytes.

## Logical tuple encoding: version 1

Magic: ASCII `MDBTUPLE`. Tuple-format versioning is independent of database, page,
schema, and catalog versions.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `MDBTUPLE` |
| 8 | 4 | Tuple encoding version (`1`) |
| 12 | 4 | Header size (`24`) |
| 16 | 2 | Encoded column count |
| 18 | 2 | Null-bitmap byte count (`ceil(columnCount / 8)`) |
| 20 | 4 | Total encoded byte size |
| 24 | variable | Null bitmap |
| after bitmap | variable | Non-NULL column payloads in schema order |

Null-bitmap bit `i` is one when column `i` is NULL; unused high bits in its final byte
must be zero. NULL fields have no payload. Non-NULL payloads use the encodings in the
type table above. VARCHAR bytes are length-prefixed and are not NUL-terminated.

`TupleCodec::encode(schema, values)` validates the value count, exact logical types,
nullability, and byte limits before returning bytes. `decode` rejects bad magic/version,
schema count or bitmap disagreement, truncation, invalid booleans, oversized VARCHARs,
nonzero unused bitmap bits, and trailing bytes. Encoded tuples cannot exceed the
SlottedPage inline limit of 4040 bytes; overflow tuples are not implemented.

Encoding and decoding are O(column count + encoded bytes).
