#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/database.h"
#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/codec.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Column constraints: DEFAULT, UNIQUE, CHECK, REFERENCES, AUTOINCREMENT.
namespace {

struct Db {
    MemoryEngine engine;
    Catalog catalog;
    Executor exec{engine, catalog};

    QueryResult run(std::string_view sql) {
        auto r = exec.execute(sql);
        REQUIRE_MESSAGE(r.ok(), "unexpected error for [" << sql << "]: "
                                    << (r.ok() ? "" : r.error().message));
        return std::move(r).value();
    }
    Error fail(std::string_view sql) {
        auto r = exec.execute(sql);
        REQUIRE_MESSAGE(!r.ok(), "expected an error for [" << sql << "]");
        return r.error();
    }
    std::vector<Row> rows(std::string_view sql) { return run(sql).rows; }
};

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
Value b(bool v) { return Value::boolean(v); }
const Value N = Value::null();

}  // namespace

// ---- DEFAULT ---------------------------------------------------------------

TEST_CASE("DEFAULT fills omitted columns on INSERT") {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY, status TEXT DEFAULT 'new', qty INT DEFAULT 1 + 1, "
           "price FLOAT DEFAULT 5, flag BOOL DEFAULT TRUE, note TEXT DEFAULT NULL, plain TEXT)");
    db.run("INSERT INTO t (id) VALUES (1)");
    db.run("INSERT INTO t (id, status, qty) VALUES (2, 'old', 9)");
    db.run("INSERT INTO t VALUES (3, 'x', 0, 1.5, FALSE, 'n', 'p')");  // explicit values win
    const auto r = db.rows("SELECT * FROM t ORDER BY id");
    REQUIRE(r.size() == 3);
    CHECK(r[0] == Row{i(1), t("new"), i(2), f(5.0), b(true), N, N});  // 1 + 1 folded, 5 -> 5.0
    CHECK(r[1] == Row{i(2), t("old"), i(9), f(5.0), b(true), N, N});
    CHECK(r[2] == Row{i(3), t("x"), i(0), f(1.5), b(false), t("n"), t("p")});
    CHECK(db.catalog.find("t")->columns[1].defaultValue == t("new"));
    CHECK_FALSE(db.catalog.find("t")->columns[6].defaultValue.has_value());
}

TEST_CASE("DEFAULT combines with NOT NULL and PRIMARY KEY") {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL DEFAULT 'anon', n INT DEFAULT 0 NOT NULL)");
    db.run("INSERT INTO t (id) VALUES (1)");
    CHECK(db.rows("SELECT name, n FROM t")[0] == Row{t("anon"), i(0)});
    // An explicit NULL is still refused: DEFAULT only applies to omitted columns.
    CHECK(db.fail("INSERT INTO t (id, name) VALUES (2, NULL)").code == ErrorCode::ConstraintViolation);
}

TEST_CASE("DEFAULT errors at CREATE TABLE") {
    Db db;
    auto e = db.fail("CREATE TABLE t (a INT DEFAULT 'x')");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:31: column 'a' expects INT, got TEXT");
    e = db.fail("CREATE TABLE t (a INT NOT NULL DEFAULT NULL)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "1:40: column 'a' cannot be NULL");
    e = db.fail("CREATE TABLE t (a INT DEFAULT b)");
    CHECK(e.message == "1:31: column reference 'b' is not allowed here");
    e = db.fail("CREATE TABLE t (a INT DEFAULT 1 DEFAULT 2)");
    CHECK(e.message == "1:33: expected a single DEFAULT, got 'DEFAULT'");
    e = db.fail("CREATE TABLE t (a INT DEFAULT 1 / 0)");
    CHECK(e.message == "1:31: division by zero");
    CHECK(db.fail("CREATE TABLE t (a INT DEFAULT count(*))").message ==
          "1:31: aggregate function 'count()' is not allowed here");
}

// ---- UNIQUE ----------------------------------------------------------------

TEST_CASE("UNIQUE refuses duplicate non-NULL values on INSERT and UPDATE, allows NULLs") {
    Db db;
    db.run("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE, code INT UNIQUE NOT NULL DEFAULT 0)");
    db.run("INSERT INTO u VALUES (1, 'a@x', 1)");
    db.run("INSERT INTO u VALUES (2, 'b@x', 2)");
    db.run("INSERT INTO u VALUES (3, NULL, 3)");
    db.run("INSERT INTO u VALUES (4, NULL, 4)");  // several NULLs are fine
    auto e = db.fail("INSERT INTO u VALUES (5, 'a@x', 5)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "duplicate value a@x for UNIQUE column 'email' in table 'u'");
    CHECK(db.fail("INSERT INTO u VALUES (5, 'c@x', 2)").message ==
          "duplicate value 2 for UNIQUE column 'code' in table 'u'");
    db.run("INSERT INTO u (id, email) VALUES (5, 'c@x')");  // code takes DEFAULT 0
    CHECK(db.fail("INSERT INTO u (id, email) VALUES (6, 'd@x')").message ==
          "duplicate value 0 for UNIQUE column 'code' in table 'u'");  // the DEFAULT collides too
    CHECK(db.rows("SELECT * FROM u").size() == 5);

    // UPDATE: against other rows, against rows modified in the same statement,
    // and a row keeping its own value is fine.
    db.run("UPDATE u SET email = 'a@x' WHERE id = 1");
    CHECK(db.fail("UPDATE u SET email = 'a@x' WHERE id = 2").message ==
          "duplicate value a@x for UNIQUE column 'email' in table 'u'");
    CHECK(db.fail("UPDATE u SET code = 9 WHERE id > 2").message ==
          "duplicate value 9 for UNIQUE column 'code' in table 'u'");
    db.run("UPDATE u SET email = NULL WHERE id IN (1, 2)");  // NULLs never collide
    db.run("UPDATE u SET code = code + 10");                 // all distinct, all move at once
    CHECK(db.rows("SELECT * FROM u WHERE code = 11").size() == 1);
    CHECK(db.catalog.find("u")->columns[1].unique);
}

TEST_CASE("UNIQUE columns are indexed: point lookups and reopen") {
    Db db;
    db.run("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE)");
    db.run("INSERT INTO u VALUES (1, 'a@x')");
    CHECK(db.engine.indexed("u", 1));
    CHECK(db.engine.lookup("u", 1, t("a@x")).value()->first == 1);
    CHECK(db.rows("SELECT id FROM u WHERE email = 'a@x'")[0] == Row{i(1)});
    CHECK(db.rows("SELECT id FROM u WHERE 'zz' = email").empty());

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_unique";
    fs::remove_all(dir);
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("CREATE TABLE u (id INT PRIMARY KEY, email TEXT UNIQUE)").ok());
        REQUIRE(d->execute("INSERT INTO u VALUES (1, 'a@x')").ok());
    }
    {
        auto d = Database::open(dir).value();
        CHECK(d->catalog().find("u")->columns[1].unique);
        CHECK(d->execute("INSERT INTO u VALUES (2, 'a@x')").error().code == ErrorCode::ConstraintViolation);
        REQUIRE(d->execute("INSERT INTO u VALUES (2, 'b@x')").ok());
    }
    fs::remove_all(dir);
    CHECK(codec::encodeSchema(TableSchema{"u", {ColumnSchema{"e", DataType::Text, false, true, std::nullopt, true}}}) ==
          "ledger-schema 1\ne TEXT NN UQ\n");
    CHECK(db.fail("CREATE TABLE v (a INT UNIQUE UNIQUE)").message == "1:30: expected a single UNIQUE constraint, got 'UNIQUE'");
}

// ---- CHECK -----------------------------------------------------------------

TEST_CASE("CHECK refuses rows that make the expression FALSE; NULL passes") {
    Db db;
    db.run("CREATE TABLE p (id INT PRIMARY KEY, age INT CHECK (age >= 0 AND age < 150), "
           "kind TEXT CHECK ( kind IN ('a', 'b') ), lo INT, hi INT CHECK (hi > lo))");
    db.run("INSERT INTO p VALUES (1, 30, 'a', 1, 2)");
    db.run("INSERT INTO p VALUES (2, NULL, NULL, NULL, 5)");  // unknown is not FALSE
    auto e = db.fail("INSERT INTO p VALUES (3, -1, 'a', 1, 2)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "CHECK constraint on column 'age' failed: age >= 0 AND age < 150");
    CHECK(db.fail("INSERT INTO p VALUES (3, 1, 'c', 1, 2)").message ==
          "CHECK constraint on column 'kind' failed: kind IN ('a', 'b')");
    CHECK(db.fail("INSERT INTO p VALUES (3, 1, 'a', 5, 2)").message ==
          "CHECK constraint on column 'hi' failed: hi > lo");  // a CHECK may read other columns
    CHECK(db.fail("INSERT INTO p (id, age) VALUES (3, 200)").message ==
          "CHECK constraint on column 'age' failed: age >= 0 AND age < 150");
    CHECK(db.rows("SELECT * FROM p").size() == 2);

    db.run("UPDATE p SET age = age + 1 WHERE id = 1");
    e = db.fail("UPDATE p SET age = age * 10");
    CHECK(e.message == "row 1: CHECK constraint on column 'age' failed: age >= 0 AND age < 150");
    CHECK(db.fail("UPDATE p SET lo = 10 WHERE id = 1").message ==
          "row 1: CHECK constraint on column 'hi' failed: hi > lo");  // changing the other side counts too
    CHECK(db.rows("SELECT age FROM p WHERE id = 1")[0] == Row{i(31)});  // nothing written
    CHECK(db.catalog.find("p")->columns[1].check == "age >= 0 AND age < 150");
    CHECK(db.catalog.find("p")->columns[0].check.empty());
}

TEST_CASE("CHECK errors at CREATE TABLE") {
    Db db;
    auto e = db.fail("CREATE TABLE t (a INT CHECK (a + 1))");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:30: CHECK on column 'a' must be BOOL, got INT");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK (b > 0))").message == "1:30: unknown column 'b' in table 't'");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK (count(*) > 0))").message ==
          "1:30: aggregate functions are not allowed in CHECK");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK (a IN (SELECT a FROM t)))").message ==
          "1:30: subqueries are only allowed inside a SELECT");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK (a > 0) CHECK (a < 9))").message ==
          "1:37: expected a single CHECK constraint, got 'CHECK'");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK a > 0)").message == "1:29: expected '(', got identifier 'a'");
    CHECK(db.fail("CREATE TABLE t (a INT CHECK (a > 'x'))").code == ErrorCode::TypeError);
}

TEST_CASE("CHECK is persisted in schema.txt and survives reopen") {
    using namespace ledger::codec;
    TableSchema s{"t", {ColumnSchema{"a", DataType::Int, false, false}, ColumnSchema{"b", DataType::Text, false, true}}};
    s.columns[0].check = "a > 0 AND a <> 7";
    s.columns[1].check = "b LIKE 'x\\t%'";
    const std::string encoded = encodeSchema(s);
    CHECK(encoded == "ledger-schema 1\n"
                     "a INT CHK:a\\s>\\s0\\sAND\\sa\\s<>\\s7\n"
                     "b TEXT NN CHK:b\\sLIKE\\s'x\\\\t%'\n");
    const auto back = decodeSchema("t", encoded).value();
    CHECK(back.columns[0].check == "a > 0 AND a <> 7");
    CHECK(back.columns[1].check == "b LIKE 'x\\t%'");
    CHECK(decodeSchema("t", "ledger-schema 1\na INT CHK:\n").error().code == ErrorCode::Corruption);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_check";
    fs::remove_all(dir);
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("CREATE TABLE t (id INT PRIMARY KEY, n INT CHECK (n BETWEEN 1 AND 5))").ok());
    }
    {
        auto d = Database::open(dir).value();
        CHECK(d->catalog().find("t")->columns[1].check == "n BETWEEN 1 AND 5");
        CHECK(d->execute("INSERT INTO t VALUES (1, 9)").error().code == ErrorCode::ConstraintViolation);
        REQUIRE(d->execute("INSERT INTO t VALUES (1, 3)").ok());
    }
    // A hand-edited schema whose CHECK no longer makes sense is reported as
    // corruption by the first write, not by opening the database.
    {
        std::ofstream(dir / "t" / "schema.txt", std::ios::trunc | std::ios::binary)
            << "ledger-schema 1\nid INT PK\nn INT CHK:zz\\s>\\s0\n";
        auto d = Database::open(dir).value();
        auto r = d->execute("INSERT INTO t VALUES (2, 3)");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Corruption);
        CHECK(r.error().message == "CHECK on column 'n' of table 't': 1:1: unknown column 'zz' in table 't'");
    }
    fs::remove_all(dir);
}

// ---- REFERENCES ------------------------------------------------------------

TEST_CASE("REFERENCES: INSERT and UPDATE need an existing parent; NULL passes") {
    Db db;
    db.run("CREATE TABLE dept (id INT PRIMARY KEY, code TEXT UNIQUE)");
    db.run("CREATE TABLE emp (id INT PRIMARY KEY, dept INT REFERENCES dept(id), "
           "dcode TEXT REFERENCES dept(code), boss INT REFERENCES emp(id))");
    db.run("INSERT INTO dept VALUES (1, 'a')");
    db.run("INSERT INTO dept VALUES (2, 'b')");
    db.run("INSERT INTO emp VALUES (10, 1, 'a', NULL)");
    db.run("INSERT INTO emp VALUES (11, 2, NULL, 10)");  // self-reference
    auto e = db.fail("INSERT INTO emp VALUES (12, 3, 'a', 10)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "foreign key on column 'dept': no row in 'dept' with id = 3");
    CHECK(db.fail("INSERT INTO emp VALUES (12, 1, 'zz', 10)").message ==
          "foreign key on column 'dcode': no row in 'dept' with code = zz");
    CHECK(db.fail("INSERT INTO emp VALUES (12, 1, 'a', 99)").message ==
          "foreign key on column 'boss': no row in 'emp' with id = 99");
    CHECK(db.fail("INSERT INTO emp VALUES (12, 1, 'a', 12)").message ==
          "foreign key on column 'boss': no row in 'emp' with id = 12");  // not even itself
    db.run("UPDATE emp SET dept = 2 WHERE id = 10");
    CHECK(db.fail("UPDATE emp SET dept = 7 WHERE id = 10").message ==
          "row 1: foreign key on column 'dept': no row in 'dept' with id = 7");
    db.run("UPDATE emp SET boss = NULL");

    // Changing a referenced key is refused while a row points at it.
    CHECK(db.fail("UPDATE dept SET id = 5 WHERE id = 2").message == "row 2: id = 2 is referenced by emp.dept");
    db.run("UPDATE dept SET code = 'c' WHERE id = 2");  // 'b' was not referenced
    CHECK(db.fail("UPDATE dept SET code = 'z' WHERE id = 1").message ==
          "row 1: code = a is referenced by emp.dcode");
    db.run("UPDATE dept SET id = id");  // unchanged keys are fine
    CHECK(db.catalog.find("emp")->columns[1].reference->table == "dept");
    CHECK_FALSE(db.catalog.find("emp")->columns[1].reference->cascade);
}

TEST_CASE("REFERENCES on DELETE: restrict by default, ON DELETE CASCADE chains") {
    Db db;
    db.run("CREATE TABLE a (id INT PRIMARY KEY)");
    db.run("CREATE TABLE b (id INT PRIMARY KEY, a INT REFERENCES a(id) ON DELETE CASCADE)");
    db.run("CREATE TABLE c (id INT PRIMARY KEY, b INT REFERENCES b(id) ON DELETE CASCADE)");
    db.run("CREATE TABLE d (id INT PRIMARY KEY, a INT REFERENCES a(id))");
    db.run("INSERT INTO a VALUES (1)");
    db.run("INSERT INTO a VALUES (2)");
    db.run("INSERT INTO b VALUES (10, 1)");
    db.run("INSERT INTO b VALUES (11, 1)");
    db.run("INSERT INTO b VALUES (12, 2)");
    db.run("INSERT INTO c VALUES (100, 10)");
    db.run("INSERT INTO c VALUES (101, 12)");
    db.run("INSERT INTO d VALUES (1000, 2)");

    auto e = db.fail("DELETE FROM a WHERE id = 2");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "row 2 of 'a' is referenced by d.a (row 1)");
    CHECK(db.rows("SELECT * FROM a").size() == 2);  // nothing was cascaded either

    CHECK(db.run("DELETE FROM a WHERE id = 1").affected == 1);  // direct rows only
    CHECK(db.rows("SELECT id FROM b ORDER BY id") == std::vector<Row>{Row{i(12)}});
    CHECK(db.rows("SELECT id FROM c") == std::vector<Row>{Row{i(101)}});

    // A cascade is undone by ROLLBACK like any other write.
    db.run("BEGIN");
    db.run("DELETE FROM d");
    CHECK(db.run("DELETE FROM a").affected == 1);
    CHECK(db.rows("SELECT * FROM c").empty());
    db.run("ROLLBACK");
    CHECK(db.rows("SELECT id FROM c") == std::vector<Row>{Row{i(101)}});
    CHECK(db.rows("SELECT id FROM d") == std::vector<Row>{Row{i(1000)}});

    // Self-reference: rows deleted by the same statement do not restrict.
    db.run("CREATE TABLE emp (id INT PRIMARY KEY, boss INT REFERENCES emp(id))");
    db.run("INSERT INTO emp VALUES (1, NULL)");
    db.run("INSERT INTO emp VALUES (2, 1)");
    db.run("INSERT INTO emp VALUES (3, 2)");
    CHECK(db.fail("DELETE FROM emp WHERE id = 1").message == "row 1 of 'emp' is referenced by emp.boss (row 2)");
    CHECK(db.fail("DELETE FROM emp WHERE id <= 2").message == "row 2 of 'emp' is referenced by emp.boss (row 3)");
    CHECK(db.run("DELETE FROM emp WHERE id >= 2").affected == 2);  // 3 -> 2 both go
    CHECK(db.run("DELETE FROM emp").affected == 1);
}

TEST_CASE("REFERENCES errors at CREATE TABLE and DROP TABLE") {
    Db db;
    db.run("CREATE TABLE p (id INT PRIMARY KEY, u TEXT UNIQUE, plain INT)");
    db.run("CREATE VIEW pv AS SELECT id FROM p");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES nope(id))").message ==
          "column 'p' REFERENCES nope(id): unknown table 'nope'");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES pv(id))").message ==
          "column 'p' REFERENCES pv(id): 'pv' is a view");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES p(nope))").message ==
          "column 'p' REFERENCES p(nope): unknown column 'nope' in table 'p'");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES p(plain))").message ==
          "column 'p' REFERENCES p(plain): the referenced column must be PRIMARY KEY or UNIQUE");
    auto e = db.fail("CREATE TABLE c (p TEXT REFERENCES p(id))");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "column 'p' REFERENCES p(id): type TEXT does not match INT");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES p(id) ON DELETE SET)").message ==
          "1:50: expected 'CASCADE', got 'SET'");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES p(id) REFERENCES p(id))").message ==
          "1:40: expected a single REFERENCES constraint, got 'REFERENCES'");
    CHECK(db.fail("CREATE TABLE c (p INT REFERENCES p)").message == "1:35: expected '(', got ')'");

    db.run("CREATE TABLE c (id INT PRIMARY KEY, p INT REFERENCES p(id), me INT REFERENCES c(id))");
    db.run("DROP VIEW pv");
    CHECK(db.fail("DROP TABLE p").message == "table 'p' is referenced by c.p");
    db.run("DROP TABLE c");  // a self-reference does not hold a table back
    db.run("DROP TABLE p");
}

TEST_CASE("REFERENCES is persisted in schema.txt and survives reopen") {
    using namespace ledger::codec;
    TableSchema s{"t", {ColumnSchema{"a", DataType::Int, false, false}, ColumnSchema{"b", DataType::Int, false, true}}};
    s.columns[0].reference = ForeignKey{"parent", "id", false};
    s.columns[1].reference = ForeignKey{"t", "a", true};
    const std::string encoded = encodeSchema(s);
    CHECK(encoded == "ledger-schema 1\na INT FK:parent.id\nb INT NN FK:t.a CASCADE\n");
    const auto back = decodeSchema("t", encoded).value();
    CHECK(back.columns[0].reference->table == "parent");
    CHECK(back.columns[0].reference->column == "id");
    CHECK_FALSE(back.columns[0].reference->cascade);
    CHECK(back.columns[1].reference->cascade);
    CHECK(decodeSchema("t", "ledger-schema 1\na INT FK:parent\n").error().code == ErrorCode::Corruption);
    CHECK(decodeSchema("t", "ledger-schema 1\na INT CASCADE\n").error().message ==
          "schema.txt:2: CASCADE without a REFERENCES target");

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_references";
    fs::remove_all(dir);
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("CREATE TABLE p (id INT PRIMARY KEY)").ok());
        REQUIRE(d->execute("CREATE TABLE c (id INT PRIMARY KEY, p INT REFERENCES p(id) ON DELETE CASCADE)").ok());
        REQUIRE(d->execute("INSERT INTO p VALUES (1)").ok());
        REQUIRE(d->execute("INSERT INTO c VALUES (1, 1)").ok());
    }
    {
        auto d = Database::open(dir).value();
        CHECK(d->catalog().find("c")->columns[1].reference->cascade);
        CHECK(d->execute("INSERT INTO c VALUES (2, 2)").error().code == ErrorCode::ConstraintViolation);
        REQUIRE(d->execute("DELETE FROM p").ok());
        CHECK(d->execute("SELECT * FROM c").value().rows.empty());
    }
    fs::remove_all(dir);
}

// ---- AUTOINCREMENT ---------------------------------------------------------

TEST_CASE("AUTOINCREMENT hands out max + 1 when the key is omitted or NULL") {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)");
    db.run("INSERT INTO t (name) VALUES ('a')");       // 1
    db.run("INSERT INTO t VALUES (NULL, 'b')");        // 2
    db.run("INSERT INTO t VALUES (10, 'c')");          // explicit values are kept
    db.run("INSERT INTO t (name) VALUES ('d')");       // 11
    CHECK(db.rows("SELECT id, name FROM t ORDER BY id") ==
          std::vector<Row>{Row{i(1), t("a")}, Row{i(2), t("b")}, Row{i(10), t("c")}, Row{i(11), t("d")}});
    db.run("DELETE FROM t WHERE id = 11");
    db.run("INSERT INTO t (name) VALUES ('e')");       // 11 again: max + 1, not a counter
    CHECK(db.rows("SELECT id FROM t WHERE name = 'e'") == std::vector<Row>{Row{i(11)}});
    CHECK(db.fail("INSERT INTO t VALUES (11, 'dup')").code == ErrorCode::ConstraintViolation);
    db.run("INSERT INTO t VALUES (-5, 'neg')");
    db.run("DELETE FROM t WHERE id > 0");
    db.run("INSERT INTO t (name) VALUES ('f')");       // only -5 left: starts back at 1
    CHECK(db.rows("SELECT id FROM t WHERE name = 'f'") == std::vector<Row>{Row{i(1)}});
    db.run("INSERT INTO t VALUES (9223372036854775807, 'top')");
    auto e = db.fail("INSERT INTO t (name) VALUES ('g')");
    CHECK(e.code == ErrorCode::TypeError);  // like any integer overflow
    CHECK(e.message == "AUTOINCREMENT column 'id' is exhausted");
    CHECK(db.catalog.find("t")->columns[0].autoIncrement);

    // A rolled back INSERT frees its key.
    db.run("DELETE FROM t");
    db.run("BEGIN");
    db.run("INSERT INTO t (name) VALUES ('x')");  // 1
    db.run("ROLLBACK");
    db.run("INSERT INTO t (name) VALUES ('y')");
    CHECK(db.rows("SELECT id FROM t") == std::vector<Row>{Row{i(1)}});
}

TEST_CASE("AUTOINCREMENT errors at CREATE TABLE; persisted as AI") {
    Db db;
    CHECK(db.fail("CREATE TABLE t (id TEXT PRIMARY KEY AUTOINCREMENT)").message ==
          "column 'id': AUTOINCREMENT requires INT PRIMARY KEY");
    CHECK(db.fail("CREATE TABLE t (id INT AUTOINCREMENT)").message ==
          "column 'id': AUTOINCREMENT requires INT PRIMARY KEY");
    CHECK(db.fail("CREATE TABLE t (id INT PRIMARY KEY AUTOINCREMENT DEFAULT 1)").message ==
          "column 'id': AUTOINCREMENT and DEFAULT are exclusive");
    CHECK(db.fail("CREATE TABLE t (id INT AUTOINCREMENT AUTOINCREMENT PRIMARY KEY)").message ==
          "1:38: expected a single AUTOINCREMENT, got 'AUTOINCREMENT'");
    db.run("CREATE TABLE t (id INT AUTOINCREMENT PRIMARY KEY)");  // any order
    CHECK(codec::encodeSchema(*db.catalog.find("t")) == "ledger-schema 1\nid INT PK AI\n");
    CHECK(codec::decodeSchema("t", "ledger-schema 1\nid INT PK AI\n").value().columns[0].autoIncrement);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_autoincrement";
    fs::remove_all(dir);
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("CREATE TABLE t (id INT PRIMARY KEY AUTOINCREMENT, v INT)").ok());
        REQUIRE(d->execute("INSERT INTO t (v) VALUES (1)").ok());
        REQUIRE(d->execute("INSERT INTO t (v) VALUES (2)").ok());
    }
    {
        auto d = Database::open(dir).value();
        REQUIRE(d->execute("INSERT INTO t (v) VALUES (3)").ok());
        CHECK(d->execute("SELECT id FROM t WHERE v = 3").value().rows == std::vector<Row>{Row{i(3)}});
    }
    fs::remove_all(dir);
}

TEST_CASE("DEFAULT is persisted in schema.txt and survives reopen") {
    using namespace ledger::codec;
    TableSchema s{"t", {ColumnSchema{"a", DataType::Text, false, false, t("two words\tand\\tab")},
                        ColumnSchema{"b", DataType::Int, false, true, i(-3)},
                        ColumnSchema{"c", DataType::Float, false, false, N},
                        ColumnSchema{"d", DataType::Bool, true, true, std::nullopt}}};
    const std::string encoded = encodeSchema(s);
    CHECK(encoded == "ledger-schema 1\n"
                     "a TEXT DEF:two\\swords\\tand\\\\tab\n"
                     "b INT NN DEF:-3\n"
                     "c FLOAT DEFNULL\n"
                     "d BOOL PK\n");
    const auto back = decodeSchema("t", encoded).value();
    CHECK(back.columns[0].defaultValue == t("two words\tand\\tab"));
    CHECK(back.columns[1].defaultValue == i(-3));
    CHECK(back.columns[2].defaultValue == N);
    CHECK_FALSE(back.columns[3].defaultValue.has_value());
    // A TEXT default that reads "NULL" stays text.
    const auto weird = decodeSchema("t", "ledger-schema 1\na TEXT DEF:NULL\nb TEXT DEF:\\\\N\n").value();
    CHECK(weird.columns[0].defaultValue == t("NULL"));
    CHECK(weird.columns[1].defaultValue == t("\\N"));
    CHECK(decodeSchema("t", "ledger-schema 1\na INT DEF:x\n").error().code == ErrorCode::Corruption);
    CHECK(decodeSchema("t", "ledger-schema 1\na INT NN DEF:1 FOO\n").error().message ==
          "schema.txt:2: unknown constraint 'FOO'");

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_defaults";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY, tag TEXT DEFAULT 'a b', n INT DEFAULT 7)").ok());
    }
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("INSERT INTO t (id) VALUES (1)").ok());
        CHECK(db->execute("SELECT tag, n FROM t").value().rows[0] == Row{t("a b"), i(7)});
    }
    fs::remove_all(dir);
}
