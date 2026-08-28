#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

#include "cli/database.h"
#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// BEGIN / COMMIT / ROLLBACK.
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

Db seeded() {
    Db db;
    db.run("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    db.run("INSERT INTO t VALUES (1, 'a')");
    db.run("INSERT INTO t VALUES (2, 'b')");
    db.run("INSERT INTO t VALUES (3, 'c')");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }

std::vector<Value> column(const std::vector<Row>& rows, std::size_t idx) {
    std::vector<Value> out;
    for (const auto& r : rows) out.push_back(r[idx]);
    return out;
}

}  // namespace

TEST_CASE("parse BEGIN / COMMIT / ROLLBACK") {
    CHECK(std::holds_alternative<ast::Begin>(parse("BEGIN").value()));
    CHECK(std::holds_alternative<ast::Begin>(parse("begin transaction;").value()));
    CHECK(std::holds_alternative<ast::Commit>(parse("COMMIT").value()));
    CHECK(std::holds_alternative<ast::Rollback>(parse("ROLLBACK;").value()));
    auto e = parse("BEGIN WORK");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:7: expected end of input, got identifier 'work'");
}

TEST_CASE("COMMIT keeps the changes, ROLLBACK undoes insert, update and delete in order") {
    Db db = seeded();
    CHECK_FALSE(db.exec.inTransaction());
    db.run("BEGIN");
    CHECK(db.exec.inTransaction());
    db.run("INSERT INTO t VALUES (4, 'd')");
    db.run("UPDATE t SET v = 'B' WHERE id = 2");
    db.run("DELETE FROM t WHERE id = 1");
    CHECK(column(db.rows("SELECT v FROM t ORDER BY id"), 0) == std::vector<Value>{t("B"), t("c"), t("d")});
    db.run("COMMIT");
    CHECK_FALSE(db.exec.inTransaction());
    CHECK(column(db.rows("SELECT v FROM t ORDER BY id"), 0) == std::vector<Value>{t("B"), t("c"), t("d")});

    db.run("BEGIN");
    db.run("DELETE FROM t");
    db.run("INSERT INTO t VALUES (9, 'z')");
    db.run("UPDATE t SET v = 'Z'");
    CHECK(column(db.rows("SELECT v FROM t"), 0) == std::vector<Value>{t("Z")});
    db.run("ROLLBACK");
    CHECK_FALSE(db.exec.inTransaction());
    const auto after = db.rows("SELECT id, v FROM t ORDER BY id");
    REQUIRE(after.size() == 3);
    CHECK(after[0] == Row{i(2), t("B")});
    CHECK(after[1] == Row{i(3), t("c")});
    CHECK(after[2] == Row{i(4), t("d")});
}

TEST_CASE("a row deleted then restored keeps its rowid; a rolled-back insert frees its key") {
    Db db = seeded();
    db.run("BEGIN");
    db.run("DELETE FROM t WHERE id = 2");
    db.run("INSERT INTO t VALUES (2, 'dup?')");  // the key is free while the row is deleted
    db.run("ROLLBACK");
    // The original row 2 is back and the temporary one is gone.
    CHECK(column(db.rows("SELECT v FROM t WHERE id = 2"), 0) == std::vector<Value>{t("b")});
    CHECK(db.rows("SELECT * FROM t").size() == 3);
    // Primary key still enforced after the rollback.
    CHECK(db.fail("INSERT INTO t VALUES (2, 'x')").code == ErrorCode::ConstraintViolation);
}

TEST_CASE("transaction state errors") {
    Db db = seeded();
    CHECK(db.fail("COMMIT").message == "no transaction in progress");
    CHECK(db.fail("ROLLBACK").message == "no transaction in progress");
    db.run("BEGIN");
    CHECK(db.fail("BEGIN").message == "a transaction is already in progress");
    auto e = db.fail("CREATE TABLE u (a INT)");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "CREATE, DROP and ALTER are not transactional; COMMIT or ROLLBACK first");
    CHECK(db.fail("DROP TABLE t").message == "CREATE, DROP and ALTER are not transactional; COMMIT or ROLLBACK first");
    CHECK(db.fail("CREATE VIEW v AS SELECT * FROM t").message ==
          "CREATE, DROP and ALTER are not transactional; COMMIT or ROLLBACK first");
    // A failed statement inside a transaction does not end it.
    CHECK(db.fail("INSERT INTO t VALUES (1, 'dup')").code == ErrorCode::ConstraintViolation);
    CHECK(db.exec.inTransaction());
    db.run("ROLLBACK");
    db.run("CREATE TABLE u (a INT)");  // allowed again
}

TEST_CASE("an executor destroyed mid-transaction rolls it back") {
    MemoryEngine engine;
    Catalog catalog;
    {
        Executor exec(engine, catalog);
        REQUIRE(exec.execute("CREATE TABLE t (id INT PRIMARY KEY)").ok());
        REQUIRE(exec.execute("INSERT INTO t VALUES (1)").ok());
        REQUIRE(exec.execute("BEGIN").ok());
        REQUIRE(exec.execute("INSERT INTO t VALUES (2)").ok());
        REQUIRE(exec.execute("DELETE FROM t WHERE id = 1").ok());
    }
    Executor exec(engine, catalog);
    const auto r = exec.execute("SELECT id FROM t");
    REQUIRE(r.ok());
    CHECK(column(r.value().rows, 0) == std::vector<Value>{i(1)});
}

TEST_CASE("transactions on FileEngine survive reopen after COMMIT, and rollback rewrites the log") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_tx";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (1, 'a')").ok());
        REQUIRE(db->execute("BEGIN").ok());
        CHECK(db->inTransaction());
        REQUIRE(db->execute("INSERT INTO t VALUES (2, 'b')").ok());
        REQUIRE(db->execute("COMMIT").ok());
        REQUIRE(db->execute("BEGIN").ok());
        REQUIRE(db->execute("UPDATE t SET v = 'X'").ok());
        REQUIRE(db->execute("DELETE FROM t WHERE id = 1").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (3, 'c')").ok());
        REQUIRE(db->execute("ROLLBACK").ok());
        CHECK_FALSE(db->inTransaction());
    }
    {
        auto db = Database::open(dir).value();
        auto r = db->execute("SELECT id, v FROM t ORDER BY id");
        REQUIRE(r.ok());
        REQUIRE(r.value().rows.size() == 2);
        CHECK(r.value().rows[0] == Row{i(1), t("a")});
        CHECK(r.value().rows[1] == Row{i(2), t("b")});
        // Rowid 1 kept its identity through delete + restore: an insert
        // after it still gets a fresh id (the replayed file agrees).
        REQUIRE(db->execute("INSERT INTO t VALUES (4, 'd')").ok());
        REQUIRE(db->execute("SELECT count(*) FROM t").value().rows[0][0] == i(3));
    }
    fs::remove_all(dir);
}
