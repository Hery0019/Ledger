#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

#include "exec/executor.h"
#include "storage/file_engine.h"
#include "storage/memory_engine.h"

using namespace ledger;

namespace {

// Test database: memory engine + catalog + executor, with a users table.
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
    std::size_t count(std::string_view sql) { return rows(sql).size(); }
};

Db seeded() {
    Db db;
    db.run("CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, score FLOAT, active BOOL)");
    db.run("INSERT INTO users VALUES (1, 'alice', 3.5, TRUE)");
    db.run("INSERT INTO users VALUES (2, 'bob', NULL, FALSE)");
    db.run("INSERT INTO users VALUES (3, 'carol', 1.0, NULL)");
    db.run("INSERT INTO users (id, name) VALUES (4, 'dave')");
    return db;
}

Value f(double d) { return Value::real(d).value(); }
Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value b(bool v) { return Value::boolean(v); }
const Value N = Value::null();

// Column `idx` of every row, to read a result at a glance.
std::vector<Value> column(const std::vector<Row>& rows, std::size_t idx) {
    std::vector<Value> out;
    for (const auto& r : rows) out.push_back(r[idx]);
    return out;
}

}  // namespace

// ---- DDL -------------------------------------------------------------------

TEST_CASE("exec CREATE TABLE registers the table in both engine and catalog") {
    Db db;
    const auto r = db.run("CREATE TABLE t (a INT)");
    CHECK(r.affected == 0);
    CHECK(r.rows.empty());
    CHECK(db.catalog.contains("t"));
    CHECK(db.engine.loadSchemas().value().size() == 1);
    CHECK(db.fail("CREATE TABLE t (a INT)").code == ErrorCode::AlreadyExists);
}

TEST_CASE("exec DROP TABLE removes it from both") {
    Db db;
    db.run("CREATE TABLE t (a INT)");
    db.run("DROP TABLE t");
    CHECK_FALSE(db.catalog.contains("t"));
    CHECK(db.engine.loadSchemas().value().empty());
    CHECK(db.fail("DROP TABLE t").code == ErrorCode::NotFound);
    CHECK(db.fail("SELECT * FROM t").code == ErrorCode::NotFound);
}

// ---- INSERT / SELECT -------------------------------------------------------

TEST_CASE("exec INSERT then SELECT * returns rows in insertion order") {
    Db db = seeded();
    const auto r = db.run("SELECT * FROM users");
    CHECK(r.columns == std::vector<std::string>{"id", "name", "score", "active"});
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{i(1), t("alice"), f(3.5), b(true)});
    CHECK(r.rows[1] == Row{i(2), t("bob"), N, b(false)});
    CHECK(r.rows[2] == Row{i(3), t("carol"), f(1.0), N});
    CHECK(r.rows[3] == Row{i(4), t("dave"), N, N});
    CHECK(db.run("INSERT INTO users VALUES (5, 'eve', 0, TRUE)").affected == 1);
}

TEST_CASE("exec SELECT projection and column headers") {
    Db db = seeded();
    const auto r = db.run("SELECT name, id FROM users WHERE id = 2");
    CHECK(r.columns == std::vector<std::string>{"name", "id"});
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0] == Row{t("bob"), i(2)});
    CHECK(r.affected == 0);
}

TEST_CASE("exec WHERE keeps only rows evaluating to TRUE (not NULL)") {
    Db db = seeded();
    CHECK(column(db.rows("SELECT id FROM users WHERE active"), 0) == std::vector<Value>{i(1)});
    CHECK(column(db.rows("SELECT id FROM users WHERE NOT active"), 0) == std::vector<Value>{i(2)});
    CHECK(column(db.rows("SELECT id FROM users WHERE active IS NULL"), 0) == std::vector<Value>{i(3), i(4)});
    CHECK(column(db.rows("SELECT id FROM users WHERE score > 2"), 0) == std::vector<Value>{i(1)});
    CHECK(db.count("SELECT id FROM users WHERE score = NULL") == 0);   // Unknown partout
    CHECK(db.count("SELECT id FROM users WHERE NULL") == 0);
    CHECK(db.count("SELECT id FROM users WHERE TRUE") == 4);
    CHECK(db.count("SELECT id FROM users WHERE FALSE") == 0);
    CHECK(column(db.rows("SELECT id FROM users WHERE name = 'carol' OR id * 2 = 8"), 0) ==
          std::vector<Value>{i(3), i(4)});
}

TEST_CASE("exec ORDER BY: stable, NULL first ascending and last descending") {
    Db db = seeded();
    CHECK(column(db.rows("SELECT id FROM users ORDER BY score"), 0) ==
          std::vector<Value>{i(2), i(4), i(3), i(1)});  // NULL(2), NULL(4) stable, then 1.0, 3.5
    CHECK(column(db.rows("SELECT id FROM users ORDER BY score DESC"), 0) ==
          std::vector<Value>{i(1), i(3), i(2), i(4)});
    CHECK(column(db.rows("SELECT id FROM users ORDER BY name DESC"), 0) ==
          std::vector<Value>{i(4), i(3), i(2), i(1)});
    CHECK(column(db.rows("SELECT id FROM users ORDER BY active ASC"), 0) ==
          std::vector<Value>{i(3), i(4), i(2), i(1)});  // NULL, NULL, false, true
}

TEST_CASE("exec ORDER BY on a column that is not projected") {
    Db db = seeded();
    const auto r = db.run("SELECT name FROM users ORDER BY id DESC");
    CHECK(r.columns == std::vector<std::string>{"name"});
    CHECK(column(r.rows, 0) == std::vector<Value>{t("dave"), t("carol"), t("bob"), t("alice")});
}

TEST_CASE("exec LIMIT applies after ORDER BY") {
    Db db = seeded();
    CHECK(column(db.rows("SELECT id FROM users ORDER BY id DESC LIMIT 2"), 0) ==
          std::vector<Value>{i(4), i(3)});
    CHECK(db.count("SELECT id FROM users LIMIT 0") == 0);
    CHECK(db.count("SELECT id FROM users LIMIT 100") == 4);
}

// ---- PRIMARY KEY -----------------------------------------------------------

TEST_CASE("exec INSERT rejects duplicate primary keys and writes nothing") {
    Db db = seeded();
    const auto e = db.fail("INSERT INTO users VALUES (2, 'dup', NULL, NULL)");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "duplicate primary key 2 in table 'users'");
    CHECK(db.count("SELECT * FROM users") == 4);
}

TEST_CASE("exec tables without primary key accept duplicates") {
    Db db;
    db.run("CREATE TABLE log (msg TEXT)");
    db.run("INSERT INTO log VALUES ('x')");
    db.run("INSERT INTO log VALUES ('x')");
    CHECK(db.count("SELECT * FROM log") == 2);
}

// ---- UPDATE ----------------------------------------------------------------

TEST_CASE("exec UPDATE modifies matching rows and reports the count") {
    Db db = seeded();
    CHECK(db.run("UPDATE users SET score = 9.5, active = TRUE WHERE id >= 3").affected == 2);
    const auto r = db.rows("SELECT id, score, active FROM users ORDER BY id");
    CHECK(r[2] == Row{i(3), f(9.5), b(true)});
    CHECK(r[3] == Row{i(4), f(9.5), b(true)});
    CHECK(r[0] == Row{i(1), f(3.5), b(true)});  // untouched
    CHECK(db.run("UPDATE users SET score = 0 WHERE id = 99").affected == 0);
}

TEST_CASE("exec UPDATE evaluates every assignment on the original row") {
    Db db;
    db.run("CREATE TABLE p (a INT, b INT)");
    db.run("INSERT INTO p VALUES (1, 2)");
    db.run("UPDATE p SET a = b, b = a");
    CHECK(db.rows("SELECT * FROM p")[0] == Row{i(2), i(1)});
    db.run("UPDATE p SET a = a + 10 WHERE b = 1");
    CHECK(db.rows("SELECT a FROM p")[0] == Row{i(12)});
}

TEST_CASE("exec UPDATE inserts the Int -> Float cast at runtime") {
    Db db = seeded();
    db.run("UPDATE users SET score = id WHERE id = 4");
    CHECK(db.rows("SELECT score FROM users WHERE id = 4")[0] == Row{f(4.0)});
}

TEST_CASE("exec UPDATE primary key: allowed when unique, rejected otherwise, nothing written") {
    Db db = seeded();
    CHECK(db.run("UPDATE users SET id = 10 WHERE id = 1").affected == 1);
    CHECK(column(db.rows("SELECT id FROM users ORDER BY id"), 0) ==
          std::vector<Value>{i(2), i(3), i(4), i(10)});

    auto e = db.fail("UPDATE users SET id = 3 WHERE id = 2");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "duplicate primary key 3 in table 'users'");

    // Collision between two rows modified by the same statement.
    e = db.fail("UPDATE users SET id = 42 WHERE id > 2");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "duplicate primary key 42 in table 'users'");
    CHECK(column(db.rows("SELECT id FROM users ORDER BY id"), 0) ==
          std::vector<Value>{i(2), i(3), i(4), i(10)});  // nothing moved

    // Reassigning one's own key is not a duplicate; neither is swapping two keys.
    CHECK(db.run("UPDATE users SET id = id").affected == 4);
    CHECK(db.run("UPDATE users SET id = id + 100").affected == 4);
    CHECK(column(db.rows("SELECT id FROM users ORDER BY id"), 0) ==
          std::vector<Value>{i(102), i(103), i(104), i(110)});
}

TEST_CASE("exec UPDATE: runtime NULL into a NOT NULL column is rejected before writing") {
    Db db;
    db.run("CREATE TABLE p (a TEXT NOT NULL, b TEXT)");
    db.run("INSERT INTO p VALUES ('x', NULL)");
    db.run("INSERT INTO p VALUES ('y', 'z')");
    const auto e = db.fail("UPDATE p SET a = b");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "row 1: column 'a' cannot be NULL");
    CHECK(column(db.rows("SELECT a FROM p"), 0) == std::vector<Value>{t("x"), t("y")});
}

// ---- DELETE ----------------------------------------------------------------

TEST_CASE("exec DELETE removes matching rows") {
    Db db = seeded();
    CHECK(db.run("DELETE FROM users WHERE score IS NULL").affected == 2);
    CHECK(column(db.rows("SELECT id FROM users"), 0) == std::vector<Value>{i(1), i(3)});
    CHECK(db.run("DELETE FROM users WHERE id = 99").affected == 0);
    CHECK(db.run("DELETE FROM users").affected == 2);
    CHECK(db.count("SELECT * FROM users") == 0);
    db.run("INSERT INTO users VALUES (1, 'again', NULL, NULL)");  // key 1 is free
    CHECK(db.count("SELECT * FROM users") == 1);
}

// ---- runtime data errors ---------------------------------------------------

TEST_CASE("exec: data errors on a row are reported with the rowid, nothing written") {
    Db db;
    db.run("CREATE TABLE p (a INT, b INT)");
    db.run("INSERT INTO p VALUES (1, 1)");
    db.run("INSERT INTO p VALUES (2, 0)");
    auto e = db.fail("SELECT * FROM p WHERE a / b > 0");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "row 2: division by zero");

    e = db.fail("UPDATE p SET a = a / b");
    CHECK(e.message == "row 2: division by zero");
    CHECK(db.rows("SELECT a FROM p")[0] == Row{i(1)});  // row 1 was not written

    db.run("INSERT INTO p VALUES (9223372036854775807, 1)");
    e = db.fail("SELECT * FROM p WHERE a + b > 0");
    CHECK(e.message == "row 3: integer overflow in '+'");
}

TEST_CASE("exec: parse and bind errors flow through execute(sql)") {
    Db db = seeded();
    CHECK(db.fail("SELEC 1").code == ErrorCode::SyntaxError);
    CHECK(db.fail("SELECT nope FROM users").code == ErrorCode::NotFound);
    CHECK(db.fail("SELECT * FROM users WHERE name + 1 > 0").code == ErrorCode::TypeError);
}

// ---- end to end on files ---------------------------------------------------

TEST_CASE("exec end-to-end on FileEngine: data survives reopen") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_exec_e2e";
    fs::remove_all(dir);
    {
        auto engine = FileEngine::open(dir).value();
        Catalog catalog;
        Executor exec(*engine, catalog);
        REQUIRE(exec.execute("CREATE TABLE notes (id INT PRIMARY KEY, body TEXT NOT NULL)").ok());
        REQUIRE(exec.execute("INSERT INTO notes VALUES (1, 'first\nline')").ok());
        REQUIRE(exec.execute("INSERT INTO notes VALUES (2, 'second')").ok());
        REQUIRE(exec.execute("UPDATE notes SET body = 'edited' WHERE id = 2").ok());
        REQUIRE(exec.execute("INSERT INTO notes VALUES (3, 'third')").ok());
        REQUIRE(exec.execute("DELETE FROM notes WHERE id = 1").ok());
    }
    {
        auto engine = FileEngine::open(dir).value();
        Catalog catalog;
        auto schemas = engine->loadSchemas().value();
        for (auto& s : schemas) REQUIRE(catalog.add(std::move(s)).ok());
        Executor exec(*engine, catalog);
        auto r = exec.execute("SELECT id, body FROM notes ORDER BY id");
        REQUIRE(r.ok());
        REQUIRE(r.value().rows.size() == 2);
        CHECK(r.value().rows[0] == Row{i(2), t("edited")});
        CHECK(r.value().rows[1] == Row{i(3), t("third")});
        CHECK(exec.execute("INSERT INTO notes VALUES (2, 'dup')").error().code ==
              ErrorCode::ConstraintViolation);
    }
    fs::remove_all(dir);
}
