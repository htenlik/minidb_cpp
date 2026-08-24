# MiniDB++ SQL semantic analysis and execution

Milestone 7 connects the database-independent syntax tree to the persistent relational
layer without changing the parser grammar:

```text
SQL -> Lexer -> Parser -> syntax AST
                              |
                              v
                    binding and conversion
                              |
                              v
                         SqlExecutor
                         /         \
                    Catalog       Table
                                  /    \
                           TupleStore  primary B+ tree
```

`minidb_sql` still links no database code. `minidb_sql_semantics` provides target-aware
literal conversion and SQL truth values, while `minidb_sql_execution` depends on the
semantic, Catalog, and Table layers. Parser nodes are never rewritten into
database-specific objects.

## Public execution model

`SqlExecutor(Catalog&).execute(const Statement&)` executes a parsed statement.
`SqlEngine(Catalog&).execute(string_view)` is the convenience path that parses and then
executes one statement. Both return `QueryResult`, a variant of:

- `CommandResult`: command kind, affected-row count, optional inserted RID, and stats;
- `SelectResult`: normalized projected column names, logical `RowValues`, source RIDs,
  and stats.

`ExecutionStats` records `AccessPath::{None, HeapScan, PrimaryKeyLookup}`,
`rowsExamined`, `rowsReturned`, and `indexLookups`. CREATE and INSERT use `None`;
SELECT, UPDATE, and DELETE report their actual discovery path.

`SqlExecutionError` carries `Semantic`, `Constraint`, or `Execution`, a message, and a
half-open source span. Expected SQL mistakes are translated at the SQL boundary;
unexpected storage or corruption errors are classified as execution failures.

## Binding and conversions

Table and column identifiers retain their AST spelling, then resolve through the
existing ASCII case-insensitive `normalizeIdentifier` rules. Bound WHERE nodes replace
column names with schema column indexes once per statement. The bound tree contains
column, literal, unary-NOT, and binary comparison/AND/OR nodes and retains source spans.

CREATE TABLE constructs `ColumnDefinition`s and delegates final validation and
persistent creation to `Schema` and `Catalog`. An unconstrained non-PK column is
nullable; a PK is implicitly non-null unless explicitly declared `NULL`, which is an
error. `VARCHAR(n)` requires `1 <= n <= 4000`. Only `UINT32` primary keys are supported.

INSERT without a column list uses schema order and requires exactly one value per
column. An explicit list is normalized and resolved, rejects duplicate columns, and
preserves its order relative to VALUES. Omitted nullable columns become NULL; omitted
non-null columns fail because DEFAULT values do not exist.

Literal conversion is strict:

| SQL literal | Compatible column | Rule |
| --- | --- | --- |
| `NULL` | nullable column | becomes `std::monostate` |
| `TRUE` / `FALSE` | `BOOLEAN` | no numeric/string coercion |
| string | `VARCHAR(n)` | source bytes must be at most `n` |
| nonnegative integer | `UINT32` | range `0..4294967295` |
| integer | `INT64` | range `-9223372036854775808..9223372036854775807` |

Integer magnitudes are accumulated digit by digit into an unsigned intermediate with a
pre-multiplication bound check. Signed overflow, wrapping, `stoll`, and implicit
coercion are avoided. Semantic validation completes before INSERT mutates storage and
before UPDATE/DELETE begin target discovery.

## WHERE expressions and NULL

WHERE uses three values: `TRUE`, `FALSE`, and `UNKNOWN`. A row is retained only for
`TRUE`. Bare BOOLEAN columns/literals are allowed; bare NULL is UNKNOWN. Bare VARCHAR
or integer expressions are semantic errors.

All comparisons involving NULL are UNKNOWN, including `NULL = NULL` and
`column = NULL`. The grammar does not yet provide `IS NULL`, so `column = NULL` never
selects a row.

```text
NOT TRUE = FALSE       NOT FALSE = TRUE       NOT UNKNOWN = UNKNOWN

FALSE AND anything = FALSE
TRUE  AND x        = x
UNKNOWN AND TRUE   = UNKNOWN
UNKNOWN AND FALSE  = FALSE

TRUE  OR anything = TRUE
FALSE OR x        = x
UNKNOWN OR FALSE  = UNKNOWN
UNKNOWN OR TRUE   = TRUE
```

Evaluation short-circuits `FALSE AND ...` and `TRUE OR ...`. Comparisons require the
same declared type. `UINT32` and `INT64` are not implicitly mixed. VARCHAR comparison
is byte-wise lexicographic regardless of declared maximum length. BOOLEAN permits only
equality/inequality. Identifier-versus-integer-literal binding converts the literal to
the identifier's numeric type; the reversed form works identically. Literal-only
integers are compared as normalized sign/magnitude values without machine overflow.

## SELECT and access paths

`SELECT *` expands schema order. Named projections resolve once, retain query order,
and may repeat a column (`SELECT id, id ...`). Result column names are normalized schema
names.

For a `UINT32` primary-key table, an equality between the PK column and an integer
literal uses `Table::findByPrimaryKey`. The equality may be reversed and may occur in
any branch connected solely by AND. The complete predicate is still evaluated against
the candidate. No equality is extracted through OR or NOT. Every other predicate, and
every query without WHERE, uses one deterministic heap scan.

```text
SELECT username FROM users WHERE id = 42
        |
        v
bind users, username, and id
        |
        v
extract UINT32 primary-key equality
        |
        v
persistent B+ tree -> RID -> TupleStore -> decode -> WHERE -> projection
```

An indexed miss examines zero rows; an indexed hit examines at most one. A heap scan
increments `rowsExamined` for every decoded table row.

## UPDATE and DELETE

UPDATE resolves every target, rejects duplicate normalized assignments, converts all
literal values once, and binds WHERE before discovery. Discovery is a distinct phase:
matching `TableRow` snapshots are collected before mutation. Replacement rows and
primary-key uniqueness are prevalidated. Mutation then calls `Table::update(RID, ...)`,
which delegates indexed changes to existing coordinated PK logic. In-page updates may
retain their RID; larger tuples may relocate through Table, which repairs the primary
index and returns the new RID. No-PK tables use the same Table boundary rather than
bypassing it.

DELETE likewise snapshots all matching rows before calling `Table::erase(RID)`. Table
keeps heap and primary-index state coordinated. INSERT reports one affected row, UPDATE
and DELETE report the number of matches successfully mutated, and CREATE reports zero.

## Persistence, complexity, and limits

SqlEngine holds only a Catalog reference; persistent truth remains in Catalog, Table,
TupleStore, and B+ tree pages. A fresh engine can continue after clean flush, close, and
reopen.

- Binding is O(expression nodes × schema column lookup); current schema lookup is
  linear in column count.
- CREATE performs linear Catalog lookup plus persistent table creation.
- INSERT performs Catalog lookup, tuple insertion, and optional B+ tree lookup/insert.
- PK SELECT is O(Catalog tables + B+ tree height + tuple fetch + predicate/projection).
- Arbitrary SELECT is O(Catalog tables + N × predicate/projection cost).
- PK UPDATE/DELETE use one index lookup plus mutation; arbitrary forms use O(N)
  discovery plus the selected mutations.

This is a specific access-path rule, not a planner or optimizer. There is no transaction
manager or WAL. Multi-page catalog/table operations are not crash atomic, and a
multi-row UPDATE or DELETE can partially complete after an unexpected storage failure.
All predictable binding, type, and constraint checks are performed before mutation,
but this is not statement atomicity.

Unsupported features remain those outside the Milestone 6 grammar: JOIN, aggregation,
GROUP BY, ORDER BY, LIMIT, aliases, subqueries, schema changes, secondary indexes,
transactions, networking, and query planning.
