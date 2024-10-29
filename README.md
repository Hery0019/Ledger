# Ledger

An embedded SQL engine in C++20 with plain-text storage. A learning project,
meant for personal use afterwards.

## Scope (v1)

- `CREATE TABLE` / `DROP TABLE` — types `INT`, `FLOAT`, `TEXT`, `BOOL`;
  constraints `PRIMARY KEY` (one per table, implies `NOT NULL`) and `NOT NULL`.
- `INSERT INTO t [(cols)] VALUES (...)` — one row per statement.
- `SELECT * | cols FROM t [WHERE expr] [ORDER BY col [ASC|DESC]] [LIMIT n]`.
- `UPDATE t SET col = expr, ... [WHERE expr]`, `DELETE FROM t [WHERE expr]`.
- Expressions: `+ - * /`, comparisons, `AND OR NOT`, `IS [NOT] NULL`,
  SQL three-valued logic. Case-insensitive identifiers.
- No joins, indexes or transactions.

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

A bare name like `mydb` is stored under the data root: `data/mydb` relative to
the current directory, or `$LEDGER_DATA_DIR/mydb` if that variable is set. The
`data/` directory is ignored by git, so every database stays out of the
repository. An argument containing a path separator (`./mydb`, `C:\dbs\mydb`)
is used as is.

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

Exit codes: `0` ok, `1` SQL or database error, `2` usage error.

## On-disk format

One directory per database, one sub-directory per table:

```
data/mydb/
  LOCK                 present while a process has the database open
  users/
    schema.txt         ledger-schema 1  then  <column> <TYPE> [PK|NN]
    rows.txt           ledger-rows 1    then  I <rowid>\t<fields>  or  D <rowid>
```

`rows.txt` is append-only: an `UPDATE` writes a `D` tombstone followed by a new
version with the same rowid; the file is compacted automatically when
tombstones dominate. Fields are tab-separated, `NULL` is encoded as `\N`, text
is escaped (`\\ \t \n \r`).

## Architecture

Strict layers, downward dependencies only:

```
sql (lexer, parser, ast)  →  semantic (catalog, binder, eval)  →  exec  →  cli
                                                 ↘  storage (IStorageEngine: FileEngine, MemoryEngine)
core: Result<T> (no exceptions), Value, Row, TableSchema
```
