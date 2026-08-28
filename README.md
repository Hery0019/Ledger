# Ledger

Moteur SQL embarqué en C++20, stockage en fichiers texte. Projet d'apprentissage,
destiné ensuite à un usage personnel.

## Périmètre (v1)

- `CREATE TABLE` / `DROP TABLE` — types `INT`, `FLOAT`, `TEXT`, `BOOL` ;
  contraintes `PRIMARY KEY` (une par table, implique `NOT NULL`) et `NOT NULL`.
- `INSERT INTO t [(cols)] VALUES (...)` — une ligne par instruction.
- `SELECT * | cols FROM t [WHERE expr] [ORDER BY col [ASC|DESC]] [LIMIT n]`.
- `UPDATE t SET col = expr, ... [WHERE expr]`, `DELETE FROM t [WHERE expr]`.
- Expressions : `+ - * /`, comparaisons, `AND OR NOT`, `IS [NOT] NULL`,
  logique à trois états SQL. Identifiants insensibles à la casse.
- Pas de jointures, d'index ni de transactions.

## Compiler et tester

Toolchain : GCC ≥ 13 (ou Clang), CMake ≥ 3.20, Ninja. Sous Windows, MSYS2 UCRT64.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ledger_tests          # suite doctest
```

Avec UBSan (sans dépendance runtime, fonctionne aussi avec GCC MinGW) :

```sh
cmake -S . -B build-san -G Ninja \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fsanitize-undefined-trap-on-error"
cmake --build build-san && ./build-san/ledger_tests
```

## Utiliser

```sh
./build/ledger mabase            # REPL : instructions terminées par ';', .help
./build/ledger mabase < script.sql   # mode script : arrêt à la première erreur
```

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

Codes de sortie : `0` ok, `1` erreur SQL ou base, `2` erreur d'usage.

## Format sur disque

Un dossier par base, un sous-dossier par table :

```
mabase/
  LOCK                 présent tant qu'un processus a la base ouverte
  users/
    schema.txt         ledger-schema 1  puis  <colonne> <TYPE> [PK|NN]
    rows.txt           ledger-rows 1    puis  I <rowid>\t<champs>  ou  D <rowid>
```

`rows.txt` est append-only : un `UPDATE` écrit un tombstone `D` puis une
nouvelle version avec le même rowid ; le fichier est compacté automatiquement
quand les tombstones dominent. Champs séparés par tabulation, `NULL` encodé
`\N`, texte échappé (`\\ \t \n \r`).

## Architecture

Couches strictes, dépendances descendantes uniquement :

```
sql (lexer, parser, ast)  →  semantic (catalog, binder, eval)  →  exec  →  cli
                                                 ↘  storage (IStorageEngine : FileEngine, MemoryEngine)
core : Result<T> (pas d'exceptions), Value, Row, TableSchema
```
