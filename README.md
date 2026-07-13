# Ledger

An embedded SQL engine in C++20 with plain-text storage. A learning project,
meant for personal use afterwards.

![Getting started: the banner, CREATE TABLE, INSERT and a styled SELECT](docs/getting-started.svg)

## Scope (v1)

- `CREATE TABLE` / `DROP TABLE` — types `INT`, `FLOAT`, `TEXT`, `BOOL`,
  `UUID` (RFC 4122, bytewise order; written as a `'xxxxxxxx-xxxx-...'` text
  literal, converted and validated at bind time; a `UUID PRIMARY KEY` that an
  INSERT omits — or sets to NULL — receives a fresh random version-4 UUID,
  the way AUTOINCREMENT serves INT keys);
  column constraints `PRIMARY KEY` (one per table, implies `NOT NULL`),
  `NOT NULL`, `DEFAULT <constant>`, `UNIQUE` (NULLs never collide),
  `CHECK (expr)` (a BOOL expression over the row; only FALSE rejects, NULL passes),
  `REFERENCES parent(column) [ON DELETE CASCADE]` (the parent column must be
  `PRIMARY KEY` or `UNIQUE`; deleting or re-keying a referenced parent row is
  refused unless the reference cascades; a referenced table cannot be dropped),
  `AUTOINCREMENT` (on `INT PRIMARY KEY`: an omitted or NULL key becomes the
  largest live key + 1, so 1 for an empty table).
- `INSERT INTO t [(cols)] VALUES (...)` — one row per statement.
- `SELECT * | t.* | expr [AS alias], ... FROM t [AS a] [[INNER|LEFT] JOIN u [AS b] ON expr ...]
  [WHERE expr] [GROUP BY expr, ...] [HAVING expr] [ORDER BY expr [ASC|DESC], ...]
  [LIMIT n]`. Columns may be qualified (`a.col`); unqualified names must be
  unambiguous. Views can contain joins and take part in joins.
- Aggregates `COUNT(*)`, `COUNT(x)`, `SUM`, `AVG`, `MIN`, `MAX` (NULLs skipped;
  SUM over INT stays INT and refuses to overflow).
- `UPDATE t SET col = expr, ... [WHERE expr]`, `DELETE FROM t [WHERE expr]`.
- `CREATE VIEW v AS SELECT cols FROM t [WHERE expr]` / `DROP VIEW v` — a view is
  a stored projection + filter over a table or another view; it can be
  queried like a table (with its own WHERE / ORDER BY / LIMIT) and is
  read-only. `ORDER BY` and `LIMIT` are not allowed inside a view definition.
  Dropping a table or view that another view reads from is refused.
- `SELECT DISTINCT ...`, `LIMIT n OFFSET m`.
- Scalar functions `UPPER`, `LOWER`, `LENGTH`, `TRIM`, `ABS`, `ROUND(x [, digits])`,
  `COALESCE(...)`, `NULLIF(a, b)`; `CASE [x] WHEN ... THEN ... [ELSE ...] END`.
- Uncorrelated subqueries: `x [NOT] IN (SELECT ...)`, `[NOT] EXISTS (SELECT ...)`,
  scalar `(SELECT ...)` (one column, at most one row). `UNION` / `UNION ALL`
  with a final `ORDER BY` on output column names.
- Expressions: `+ - * /`, comparisons, `AND OR NOT`, `IS [NOT] NULL`,
  `[NOT] IN (...)`, `[NOT] BETWEEN a AND b`, `[NOT] LIKE 'pattern'` (`%`, `_`),
  SQL three-valued logic. Case-insensitive identifiers.
- `CREATE USER name PASSWORD 'secret'` / `ALTER USER name PASSWORD 'new'` /
  `DROP USER name` — user accounts, stored as salted PBKDF2-HMAC-SHA256
  hashes in `users.txt`. They gate the HTTP server (see below); the CLI needs
  none, since whoever can read the files needs no password (they are plain
  text), exactly like the postgres OS user. `USER`, `PASSWORD` and `ALTER`
  are reserved words.
- `BEGIN [TRANSACTION]` / `COMMIT` / `ROLLBACK`: writes are applied at once and
  logged; `ROLLBACK` undoes them in reverse order. Atomic within the process,
  not across a crash. `CREATE` / `DROP` are refused inside a transaction; a
  session closing mid-transaction rolls it back.
- Every `PRIMARY KEY` and `UNIQUE` column has an in-memory index (rebuilt when a table is loaded):
  key uniqueness and `WHERE pk = value` on a table are answered without a
  scan. No user-defined indexes.
- No correlated subqueries.

![Joins, aggregates, a view and CASE](docs/queries.svg)

![A transaction rolled back, then a primary-key error](docs/transactions.svg)

The images above are generated from the real program output by
`tools/screenshots.sh` (`tools/ansi2svg.pl` turns the coloured terminal
output into SVG), so they always match the current build.

## Build and test

Toolchain: GCC ≥ 13 (or Clang), CMake ≥ 3.20, Ninja. On Windows, MSYS2 UCRT64.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ledger_tests          # doctest suite
```

With UBSan (no runtime dependency, also works with MinGW GCC):

```sh
cmake -S . -B build-san -G Ninja \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fsanitize-undefined-trap-on-error"
cmake --build build-san && ./build-san/ledger_tests
```

## Usage

```sh
./build/ledger mydb              # REPL: statements end with ';', .help for commands
./build/ledger mydb < script.sql # script mode: stops at the first error
```

A bare name like `mydb` is stored under the data root: `data/mydb` next to
the project (the parent of the `build/` directory holding the executable),
whatever the current directory, or `$LEDGER_DATA_DIR/mydb` if that variable is
set. The `data/` directory is ignored by git, so every database stays out of
the repository. An argument containing a path separator (`./mydb`,
`C:\dbs\mydb`) is used as is.

```
ledger> CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, score FLOAT);
ok
ledger> INSERT INTO users VALUES (1, 'Alice', 3.5);
1 row affected
ledger> SELECT * FROM users WHERE score > 1 ORDER BY name;
+----+-------+-------+
| id | name  | score |
+----+-------+-------+
| 1  | Alice | 3.5   |
+----+-------+-------+
1 row
```

In a terminal, results are drawn with rounded Unicode borders and colours
(bold headers, right-aligned numbers, dim `NULL`, green/red booleans):

```
╭────┬───────┬───────╮
│ id │ name  │ score │
├────┼───────┼───────┤
│  1 │ Alice │   3.5 │
│  2 │ Bob   │  NULL │
╰────┴───────┴───────╯
2 rows
```

When stdout is a pipe or a file the output is plain ASCII without escape
sequences (the `+---+` tables shown above). `NO_COLOR=1` forces plain output;
`LEDGER_STYLE=fancy` or `LEDGER_STYLE=plain` forces one style either way.

Exit codes: `0` ok, `1` SQL or database error, `2` usage error.

## HTTP server (ledgerd)

`ledgerd` serves one database over HTTP, so that applications in any language
can use it without linking the engine. The ledgerd process is the single
owner of the database (the `LOCK` file keeps everyone else out); clients
never touch the files — they POST SQL and read JSON back.

```sh
./build/ledgerd mydb                # http://127.0.0.1:5433, same data root as ledger
./build/ledgerd mydb --port 8080
```

- `POST /query` — body: one or more `;`-separated statements, run in order,
  stopping at the first error. Reply `200`: `{"results": [...]}` with one
  entry per statement — `{"kind":"select","columns":[...],"rows":[[...]]}`,
  `{"kind":"dml","affected":n}` or `{"kind":"ddl"}` — plus `"warnings"` when
  the engine reported any. An INSERT whose key the engine generated
  (AUTOINCREMENT, UUID PRIMARY KEY) also carries `"key"`, so the client
  learns it without a second query. On error, `400` (client mistake) or `500`
  (IO/corruption): `{"error":{"code":...,"message":...,"line":...},
  "results":[...]}` where `results` holds the statements already applied.
- `GET /health` — `{"ok":true,"database":"..."}`.

```sh
$ curl -s -X POST http://127.0.0.1:5433/query -d "SELECT * FROM users;"
{"results":[{"kind":"select","columns":["id","name","score"],"rows":[[1,"Alice",3.5],[2,"Bob",null]]}]}
```

SQL values map onto JSON as INT/FLOAT → number, TEXT → string, BOOL →
true/false, NULL → null. Requests are served one at a time, and a request is
a session: a transaction must begin and end within a single body
(`BEGIN; ...; COMMIT;`); a transaction left open — or broken by an error —
is rolled back, exactly like a REPL session closing mid-transaction.

**Authentication.** A database with user accounts requires HTTP Basic
credentials matching one of them on every request; without any account it is
open. The bootstrap is one statement, from the REPL or from a first request:

```sh
$ curl -s -X POST http://127.0.0.1:5433/query -d "CREATE USER alice PASSWORD 'S3cret!';"
{"results":[{"kind":"ddl"}]}
$ curl -s -X POST http://127.0.0.1:5433/query -d "SELECT 1;"          # 401 from now on
{"error":{"code":"Unauthorized","message":"user name and password required (HTTP Basic)"}}
$ curl -s -u alice:'S3cret!' -X POST http://127.0.0.1:5433/query -d "SELECT 1 AS one;"
{"results":[{"kind":"select","columns":["one"],"rows":[[1]]}]}
```

Every account is equal (no roles, no per-table grants in v1); `.users` lists
them in the REPL. Passwords travel in clear over plain HTTP: keep ledgerd on
localhost, or put TLS in front. And the storage itself is plain text — the
accounts protect the network door, not the files.

## On-disk format

One directory per database, one sub-directory per table:

```
data/mydb/
  LOCK                 present while a process has the database open
  views.txt            ledger-views 1   then  <view>\t<escaped SELECT>, one per line
  users/
    schema.txt         ledger-schema 1  then  <column> <TYPE> [PK] [NN] [UQ] [AI] [CHK:<expr>] [FK:<table>.<col> [CASCADE]] [DEF:<value>|DEFNULL]
    rows.txt           ledger-rows 1    then  I <rowid>\t<fields>  or  D <rowid>
```

`rows.txt` is append-only: an `UPDATE` writes a `D` tombstone followed by a new
version with the same rowid; the file is compacted automatically when
tombstones dominate. Fields are tab-separated, `NULL` is encoded as `\N`, text
is escaped (`\\ \t \n \r`).

## Architecture

Strict layers, downward dependencies only:

```
sql (lexer, parser, ast)  →  semantic (catalog, binder, eval)  →  exec  →  cli  →  server (ledgerd)
                                                 ↘  storage (IStorageEngine: FileEngine, MemoryEngine)
core: Result<T> (no exceptions), Value, Row, TableSchema
```
