#include "doctest.h"

#include <string>
#include <vector>

#include "exec/executor.h"
#include "semantic/eval.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// IN, BETWEEN, LIKE, DISTINCT and OFFSET.
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

Db people() {
    Db db;
    db.run("CREATE TABLE people (id INT PRIMARY KEY, name TEXT NOT NULL, age INT, city TEXT)");
    db.run("INSERT INTO people VALUES (1, 'alice', 30, 'paris')");
    db.run("INSERT INTO people VALUES (2, 'bob', 25, 'lyon')");
    db.run("INSERT INTO people VALUES (3, 'carol', NULL, 'paris')");
    db.run("INSERT INTO people VALUES (4, 'dan', 40, NULL)");
    db.run("INSERT INTO people VALUES (5, 'al_ice', 30, 'paris')");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }

std::vector<Value> ids(Db& db, std::string_view where) {
    std::vector<Value> out;
    for (const auto& r : db.rows("SELECT id FROM people WHERE " + std::string(where) + " ORDER BY id")) {
        out.push_back(r[0]);
    }
    return out;
}

std::string text(std::string_view sql) {
    auto r = parse(sql);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return ast::exprToString(*std::get<ast::Select>(r.value()).where);
}

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse IN, BETWEEN, LIKE and their NOT forms") {
    CHECK(text("SELECT * FROM t WHERE a IN (1, 2, 3)") == "a IN (1, 2, 3)");
    CHECK(text("SELECT * FROM t WHERE a NOT IN ('x')") == "a NOT IN ('x')");
    CHECK(text("SELECT * FROM t WHERE a BETWEEN 1 AND 10") == "a BETWEEN 1 AND 10");
    CHECK(text("SELECT * FROM t WHERE a NOT BETWEEN b + 1 AND c * 2") == "a NOT BETWEEN b + 1 AND c * 2");
    CHECK(text("SELECT * FROM t WHERE n LIKE 'a%'") == "n LIKE 'a%'");
    CHECK(text("SELECT * FROM t WHERE n NOT LIKE '_b'") == "n NOT LIKE '_b'");
    // BETWEEN's AND is not a logical AND; a logical AND can still follow.
    CHECK(text("SELECT * FROM t WHERE a BETWEEN 1 AND 2 AND b") == "a BETWEEN 1 AND 2 AND b");
    CHECK(text("SELECT * FROM t WHERE NOT a IN (1)") == "NOT a IN (1)");  // NOT (a IN (1))
    CHECK(text("SELECT * FROM t WHERE a + 1 IN (2)") == "a + 1 IN (2)");

    auto e = parse("SELECT * FROM t WHERE a IN ()");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:29: expected expression, got ')'");
    e = parse("SELECT * FROM t WHERE a BETWEEN 1");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:34: expected 'AND', got 'end of input'");
    e = parse("SELECT * FROM t WHERE a IN (1) = b");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:32: comparison operators cannot be chained; use parentheses");
}

TEST_CASE("parse DISTINCT, LIMIT/OFFSET") {
    auto s = std::get<ast::Select>(parse("SELECT DISTINCT a FROM t LIMIT 5 OFFSET 10").value());
    CHECK(s.distinct);
    CHECK(s.limit == 5);
    CHECK(s.offset == 10);
    s = std::get<ast::Select>(parse("SELECT a FROM t OFFSET 3").value());
    CHECK_FALSE(s.distinct);
    CHECK_FALSE(s.limit.has_value());
    CHECK(s.offset == 3);
    auto e = parse("SELECT a FROM t OFFSET -1");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:24: expected non-negative integer after OFFSET, got '-'");
    e = parse("CREATE VIEW v AS SELECT a FROM t OFFSET 1");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "view 'v': ORDER BY, LIMIT and OFFSET are not allowed in a view");
}

// ---- LIKE matching ---------------------------------------------------------

TEST_CASE("likeMatch: % and _ wildcards, case-sensitive") {
    CHECK(likeMatch("alice", "alice"));
    CHECK_FALSE(likeMatch("alice", "Alice"));
    CHECK(likeMatch("alice", "a%"));
    CHECK(likeMatch("alice", "%e"));
    CHECK(likeMatch("alice", "%lic%"));
    CHECK(likeMatch("alice", "%"));
    CHECK(likeMatch("", "%"));
    CHECK_FALSE(likeMatch("", "_"));
    CHECK(likeMatch("a", "_"));
    CHECK(likeMatch("alice", "a_ice"));
    CHECK_FALSE(likeMatch("alice", "a_ce"));
    CHECK(likeMatch("alice", "%%a%%"));
    CHECK(likeMatch("aXbXc", "a%b%c"));
    CHECK_FALSE(likeMatch("aXbXc", "a%c%b"));
    CHECK(likeMatch("abc", "a%_c"));
    CHECK_FALSE(likeMatch("ac", "a%_c"));
    CHECK_FALSE(likeMatch("alice", ""));
}

// ---- semantics -------------------------------------------------------------

TEST_CASE("IN and NOT IN, with NULL rules") {
    Db db = people();
    CHECK(ids(db, "id IN (1, 3, 99)") == std::vector<Value>{i(1), i(3)});
    CHECK(ids(db, "city IN ('paris')") == std::vector<Value>{i(1), i(3), i(5)});
    CHECK(ids(db, "city NOT IN ('paris')") == std::vector<Value>{i(2)});  // NULL city: unknown, dropped
    CHECK(ids(db, "age IN (30, 40)") == std::vector<Value>{i(1), i(4), i(5)});
    CHECK(ids(db, "age IN (30, NULL)") == std::vector<Value>{i(1), i(5)});
    CHECK(ids(db, "age NOT IN (30, NULL)").empty());  // never true: the NULL item makes non-matches unknown
    CHECK(ids(db, "age NOT IN (30)") == std::vector<Value>{i(2), i(4)});
    CHECK(ids(db, "id IN (id)") == std::vector<Value>{i(1), i(2), i(3), i(4), i(5)});
    CHECK(ids(db, "id + 1 IN (2, 4)") == std::vector<Value>{i(1), i(3)});
    CHECK(ids(db, "30 IN (age, 25)") == std::vector<Value>{i(1), i(5)});  // bob: 30 IN (25, 25) is false
}

TEST_CASE("BETWEEN is inclusive and NULL-aware") {
    Db db = people();
    CHECK(ids(db, "age BETWEEN 25 AND 30") == std::vector<Value>{i(1), i(2), i(5)});
    CHECK(ids(db, "age NOT BETWEEN 25 AND 30") == std::vector<Value>{i(4)});
    CHECK(ids(db, "name BETWEEN 'b' AND 'd'") == std::vector<Value>{i(2), i(3)});
    CHECK(ids(db, "age BETWEEN 40 AND 25").empty());
    CHECK(ids(db, "id BETWEEN 2 AND 2") == std::vector<Value>{i(2)});
}

TEST_CASE("LIKE and NOT LIKE") {
    Db db = people();
    CHECK(ids(db, "name LIKE 'a%'") == std::vector<Value>{i(1), i(5)});
    CHECK(ids(db, "name LIKE '%o%'") == std::vector<Value>{i(2), i(3)});
    CHECK(ids(db, "name LIKE '___'") == std::vector<Value>{i(2), i(4)});
    CHECK(ids(db, "name NOT LIKE '%a%'") == std::vector<Value>{i(2)});
    CHECK(ids(db, "city LIKE 'p%'") == std::vector<Value>{i(1), i(3), i(5)});
    CHECK(ids(db, "city NOT LIKE 'p%'") == std::vector<Value>{i(2)});  // NULL city dropped
    CHECK(ids(db, "name LIKE name") == std::vector<Value>{i(1), i(2), i(3), i(4), i(5)});
}

TEST_CASE("predicates fold when constant and mix with other operators") {
    Db db = people();
    CHECK(ids(db, "1 IN (1, 2) AND age IS NOT NULL") == std::vector<Value>{i(1), i(2), i(4), i(5)});
    CHECK(ids(db, "'abc' LIKE 'a%' AND id = 1") == std::vector<Value>{i(1)});
    CHECK(ids(db, "5 BETWEEN 1 AND 3").empty());
    CHECK(ids(db, "NOT (age BETWEEN 26 AND 35)") == std::vector<Value>{i(2), i(4)});
    const auto r = db.run("SELECT id IN (1, 2) AS small, name LIKE 'a%' AS a_name FROM people WHERE id = 1");
    CHECK(r.columns == std::vector<std::string>{"small", "a_name"});
    CHECK(r.rows[0] == Row{Value::boolean(true), Value::boolean(true)});
}

TEST_CASE("predicate type errors") {
    Db db = people();
    auto e = db.fail("SELECT * FROM people WHERE name IN (1, 2)");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:37: cannot compare TEXT with INT in IN list");
    CHECK(db.fail("SELECT * FROM people WHERE age BETWEEN 'a' AND 'z'").message ==
          "1:28: cannot compare INT with TEXT in BETWEEN");
    CHECK(db.fail("SELECT * FROM people WHERE age LIKE '3%'").message == "1:28: LIKE requires TEXT, got INT");
    CHECK(db.fail("SELECT * FROM people WHERE name LIKE 3").message == "1:28: LIKE requires TEXT, got INT");
    CHECK(db.fail("SELECT * FROM people WHERE name IN (nope)").message ==
          "1:37: unknown column 'nope' in table 'people'");
}

// ---- DISTINCT / OFFSET -----------------------------------------------------

TEST_CASE("DISTINCT drops duplicate result rows, keeping the first") {
    Db db = people();
    auto r = db.run("SELECT DISTINCT city FROM people");
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0] == Row{t("paris")});
    CHECK(r.rows[1] == Row{t("lyon")});
    CHECK(r.rows[2] == Row{Value::null()});  // NULLs are one value
    r = db.run("SELECT DISTINCT city, age FROM people ORDER BY city, age");
    // (paris,30) appears twice (rows 1 and 5) and is kept once.
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{Value::null(), i(40)});
    CHECK(r.rows[1] == Row{t("lyon"), i(25)});
    CHECK(r.rows[2] == Row{t("paris"), Value::null()});
    CHECK(r.rows[3] == Row{t("paris"), i(30)});
    CHECK(db.rows("SELECT DISTINCT age FROM people WHERE age = 30").size() == 1);
    // DISTINCT with aggregates and joins works on the projected rows.
    CHECK(db.rows("SELECT DISTINCT count(*) FROM people GROUP BY city").size() == 2);  // 3, 1, 1
}

TEST_CASE("OFFSET skips rows after ORDER BY, before LIMIT") {
    Db db = people();
    CHECK(ids(db, "TRUE") == std::vector<Value>{i(1), i(2), i(3), i(4), i(5)});
    auto r = db.rows("SELECT id FROM people ORDER BY id OFFSET 2");
    REQUIRE(r.size() == 3);
    CHECK(r[0] == Row{i(3)});
    r = db.rows("SELECT id FROM people ORDER BY id LIMIT 2 OFFSET 1");
    REQUIRE(r.size() == 2);
    CHECK(r[0] == Row{i(2)});
    CHECK(r[1] == Row{i(3)});
    CHECK(db.rows("SELECT id FROM people OFFSET 10").empty());
    CHECK(db.rows("SELECT id FROM people ORDER BY id DESC LIMIT 1 OFFSET 4")[0] == Row{i(1)});
}
