# MiniDB++ SQL lexer, parser, and AST

Milestone 6 implements a hand-written, database-independent front end. `minidb_sql`
links no Pager, Catalog, Table, Schema, or storage code:

```text
source -> Lexer -> vector<Token> -> Parser -> AST
                                      |
                                      +-- no semantic validation or execution
```

## Lexical rules

Unquoted identifiers use ASCII `[A-Za-z_][A-Za-z0-9_]*`. The lexer preserves their
source spelling; Catalog normalization remains a later semantic step. Keywords are
reserved and ASCII case-insensitive. The keyword set is:

```text
CREATE TABLE INSERT INTO VALUES SELECT FROM WHERE UPDATE SET DELETE
PRIMARY KEY NOT NULL UINT32 INT64 BOOLEAN VARCHAR TRUE FALSE AND OR
```

Punctuation/operators are `(`, `)`, `,`, `;`, `*`, `=`, `!=`, `<>`, `<`, `<=`, `>`,
`>=`, and `-`. Operators use maximal munch. Minus is separate from the following
integer token.

Spaces, tabs, CR, LF, and CRLF are ignored. `--` comments continue to newline or EOF;
`/* ... */` comments may span lines but do not nest. Unterminated block comments fail.

Strings use single quotes. Two quotes inside a string decode to one quote:

```sql
'it''s'  -- logical value: it's
```

Backslash has no special meaning. Newlines inside strings are accepted and retained.
String contents are bytes from the source; UTF-8 is not validated.

Integer tokens retain their digit spelling and are limited to 1024 digits as a
defensive lexical bound. The AST stores `{ negative, magnitude }`, so values including
`4294967295`, `-9223372036854775808`, and values beyond current semantic target ranges
never overflow during parsing. The semantic layer performs column-aware conversion.

Every token has an exact lexeme, decoded value where relevant, and a half-open source
span. Offsets are zero-based byte offsets; lines and columns are one-based. CRLF counts
as one newline.

## Grammar

Brackets mean optional, braces mean repetition, and terminal words are reserved
keywords.

```text
statement
    := (create_table | insert | select | update | delete) [';'] EOF

create_table
    := CREATE TABLE identifier '('
       column_definition {',' column_definition} ')'

column_definition
    := identifier type_specification {column_constraint}

type_specification
    := UINT32 | INT64 | BOOLEAN | VARCHAR '(' unsigned_integer ')'

column_constraint
    := PRIMARY KEY | NOT NULL | NULL

insert
    := INSERT INTO identifier
       ['(' identifier {',' identifier} ')']
       VALUES '(' literal {',' literal} ')'

select
    := SELECT ('*' | identifier {',' identifier})
       FROM identifier [WHERE expression]

update
    := UPDATE identifier SET assignment {',' assignment}
       [WHERE expression]

assignment
    := identifier '=' literal

delete
    := DELETE FROM identifier [WHERE expression]

literal
    := NULL | TRUE | FALSE | unsigned_integer | '-' unsigned_integer | string

expression       := or_expression
or_expression    := and_expression {OR and_expression}
and_expression   := not_expression {AND not_expression}
not_expression   := NOT not_expression | comparison
comparison       := primary [comparison_operator primary]
primary          := identifier | literal | '(' expression ')'

comparison_operator := '=' | '!=' | '<>' | '<' | '<=' | '>' | '>='
```

Precedence from highest to lowest is parentheses/primary, comparison, unary NOT, AND,
then OR. AND and OR associate left. NOT associates right. Comparisons are deliberately
non-associative: `a = b = c` is rejected; use `a = b AND b = c`.

`SELECT *` has a dedicated AST flag and cannot be mixed with named projections. Empty
or trailing-comma column/value/assignment lists are rejected. Duplicate INSERT columns
and UPDATE targets remain semantic concerns. Repeated PRIMARY KEY constraints and
duplicate or contradictory NULL/NOT NULL constraints are rejected as malformed syntax;
the parser otherwise does not apply Schema policy.

## AST and diagnostics

Statement nodes are `CreateTableStatement`, `InsertStatement`, `SelectStatement`,
`UpdateStatement`, and `DeleteStatement`. Expression nodes are identifier, literal,
unary NOT, and binary comparison/AND/OR nodes. Recursive children use `unique_ptr`, so
ownership is explicit and ASTs are move-only. Statements, expressions, columns,
assignments, types, and literals retain useful source spans. A deterministic debug
formatter supports structural tests and future executor debugging.

`SqlError` is an exception carrying `SqlErrorKind::Lexer` or `Parser`, a message, and a
source span. Its formatted text begins `line N, column M:`. Parsing stops safely at the
first failure; the parser does not attempt multi-error syntax recovery.

The parser accepts one optional trailing semicolon and then requires EOF. Multiple
statements or trailing garbage are rejected. Parenthesized and unary-NOT nesting is
limited to 128 levels; flat AND/OR chains do not consume this budget.

## Semantic boundary and unsupported syntax

The AST deliberately retains SQL literals rather than constructing MiniDB++ `Value`s,
and CREATE TABLE retains syntax-level types rather than constructing `Schema`. Table
existence, column existence, duplicate names, primary-key legality, nullability, type
ranges, and VARCHAR limits are handled by the semantic layer.

JOIN, qualified identifiers, aliases, projection expressions, functions, aggregates,
DISTINCT, GROUP BY, HAVING, ORDER BY, LIMIT/OFFSET, subqueries, UNION, multi-row VALUES,
INSERT ... SELECT, arithmetic, CREATE INDEX, DROP, ALTER, transactions, and
placeholders are unsupported by the grammar.

Lexing is O(source bytes), predictive parsing is O(token count), expression parsing is
linear in expression tokens, and AST memory is O(AST nodes/source payload). No database
I/O occurs.
