#!/usr/bin/env bash
# Regenerates the README screenshots in docs/ from the real program output.
# Run from the repository root after building: tools/screenshots.sh
set -uo pipefail

LEDGER=${LEDGER:-build/ledger.exe}
[ -x "$LEDGER" ] || LEDGER=build/ledger
DB=data/_screenshots
rm -rf "$DB"
mkdir -p docs

export LEDGER_STYLE=fancy LEDGER_ECHO=1

# The program exits 1 when a statement fails; the last capture shows an error
# on purpose, so exit codes are ignored here.
capture() {  # capture <title> <output.svg>  (SQL on stdin)
    "$LEDGER" "$DB" 2>&1 | perl tools/ansi2svg.pl "$1" > "docs/$2"
}

# 1. Banner, a table, inserts and a styled SELECT.
LEDGER_BANNER=1 capture "ledger — getting started" getting-started.svg <<'SQL'
CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL,
                    score FLOAT, active BOOL);
INSERT INTO users VALUES (1, 'Alice', 3.5, TRUE);
INSERT INTO users VALUES (2, 'Bob', NULL, FALSE);
INSERT INTO users VALUES (3, 'Café', 9.75, TRUE);
SELECT * FROM users ORDER BY score DESC;
SQL

# 2. Joins, aggregates, views, functions.
capture "ledger — joins, aggregates, views" queries.svg <<'SQL'
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, total FLOAT);
INSERT INTO orders VALUES (10, 1, 50.0);
INSERT INTO orders VALUES (11, 1, 25.5);
INSERT INTO orders VALUES (12, 3, 10.0);
SELECT u.name, count(o.id) AS orders, coalesce(sum(o.total), 0) AS spent
  FROM users u LEFT JOIN orders o ON o.user_id = u.id
  GROUP BY u.name ORDER BY spent DESC;
CREATE VIEW big_spenders AS
  SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id
  WHERE o.total >= 20;
SELECT upper(name) AS who,
       CASE WHEN total > 40 THEN 'large' ELSE 'medium' END AS size
  FROM big_spenders;
SQL

# 3. Transactions and an error.
capture "ledger — transactions and errors" transactions.svg <<'SQL'
BEGIN;
DELETE FROM orders WHERE total < 30;
SELECT count(*) AS remaining FROM orders;
ROLLBACK;
SELECT count(*) AS remaining FROM orders;
INSERT INTO users VALUES (1, 'Duplicate', NULL, TRUE);
SQL

rm -rf "$DB"
ls -la docs/*.svg
