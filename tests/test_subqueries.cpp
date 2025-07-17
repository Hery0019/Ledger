#include "doctest.h"

#include <string>
#include <vector>

#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Subqueries (IN, EXISTS, scalar) and UNION.
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

Db shop() {
    Db db;
    db.run("CREATE TABLE customers (id INT PRIMARY KEY, name TEXT NOT NULL, city TEXT)");
    db.run("CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total FLOAT)");
    db.run("INSERT INTO customers VALUES (1, 'alice', 'paris')");
    db.run("INSERT INTO customers VALUES (2, 'bob', 'lyon')");
    db.run("INSERT INTO customers VALUES (3, 'carol', NULL)");
    db.run("INSERT INTO orders VALUES (10, 1, 50.0)");
    db.run("INSERT INTO orders VALUES (11, 1, 25.5)");
    db.run("INSERT INTO orders VALUES (12, 2, 10.0)");
    db.run("INSERT INTO orders VALUES (13, NULL, 2.0)");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
const Value N = Value::null();

std::vector<Value> column(const std::vector<Row>& rows, std::size_t idx) {
    std::vector<Value> out;
    for (const auto& r : rows) out.push_back(r[idx]);
    return out;
}

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse subqueries and UNION") {
    auto r = parse("SELECT a FROM t WHERE a IN (SELECT b FROM u) AND EXISTS (SELECT 1 FROM v) "
                   "AND NOT EXISTS (SELECT 1 FROM w) AND a > (SELECT max(b) FROM u)");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const auto& s = std::get<ast::Select>(r.value());
    CHECK(ast::exprToString(*s.where) ==
          "a IN (SELECT ...) AND EXISTS (SELECT ...) AND NOT EXISTS (SELECT ...) AND a > (SELECT ...)");

    r = parse("SELECT a FROM t UNION ALL SELECT b FROM u UNION SELECT c FROM v ORDER BY a DESC LIMIT 3 OFFSET 1");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const auto& u = std::get<ast::Select>(r.value());
    REQUIRE(u.unions.size() == 2);
    CHECK(u.unions[0].all);
    CHECK_FALSE(u.unions[1].all);
    CHECK(u.unions[1].select->from.name == "v");
    CHECK(u.orderBy.size() == 1);  // moved to the head
    CHECK(u.limit == 3);
    CHECK(u.offset == 1);
    CHECK(u.unions[1].select->orderBy.empty());
    CHECK_FALSE(u.unions[1].select->limit.has_value());

    auto e = parse("SELECT a FROM t ORDER BY a UNION SELECT b FROM u");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message ==
          "1:28: expected end of query (ORDER BY, LIMIT and OFFSET must follow the last SELECT of a UNION), got 'UNION'");
    e = parse("SELECT a FROM t UNION 1");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:23: expected SELECT after UNION, got integer '1'");
    e = parse("SELECT a FROM t WHERE EXISTS (1)");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:31: expected SELECT after EXISTS (, got integer '1'");
    e = parse("CREATE VIEW v AS SELECT a FROM t UNION SELECT b FROM u");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "view 'v': UNION is not allowed in a view");
}

// ---- IN (SELECT ...) -------------------------------------------------------

TEST_CASE("IN and NOT IN with a subquery follow the list rules, NULLs included") {
    Db db = shop();
    CHECK(column(db.rows("SELECT name FROM customers WHERE id IN (SELECT customer_id FROM orders) ORDER BY id"), 0) ==
          std::vector<Value>{t("alice"), t("bob")});
    // The subquery yields a NULL: NOT IN can never be true.
    CHECK(db.rows("SELECT name FROM customers WHERE id NOT IN (SELECT customer_id FROM orders)").empty());
    CHECK(column(db.rows("SELECT name FROM customers WHERE id NOT IN "
                         "(SELECT customer_id FROM orders WHERE customer_id IS NOT NULL) ORDER BY id"), 0) ==
          std::vector<Value>{t("carol")});
    // With a filter, DISTINCT and aggregates inside.
    CHECK(column(db.rows("SELECT name FROM customers WHERE id IN "
                         "(SELECT DISTINCT customer_id FROM orders WHERE total > 20)"), 0) ==
          std::vector<Value>{t("alice")});
    CHECK(column(db.rows("SELECT id FROM orders WHERE total IN (SELECT max(total) FROM orders)"), 0) ==
          std::vector<Value>{i(10)});
    // Empty subquery: nothing is IN, everything is NOT IN.
    CHECK(db.rows("SELECT id FROM customers WHERE id IN (SELECT id FROM orders WHERE total > 1000)").empty());
    CHECK(db.rows("SELECT id FROM customers WHERE id NOT IN (SELECT id FROM orders WHERE total > 1000)").size() == 3);
}

// ---- EXISTS ----------------------------------------------------------------

TEST_CASE("EXISTS and NOT EXISTS") {
    Db db = shop();
    CHECK(db.rows("SELECT id FROM customers WHERE EXISTS (SELECT 1 FROM orders WHERE total > 40)").size() == 3);
    CHECK(db.rows("SELECT id FROM customers WHERE EXISTS (SELECT 1 FROM orders WHERE total > 400)").empty());
    CHECK(db.rows("SELECT id FROM customers WHERE NOT EXISTS (SELECT id FROM orders WHERE total > 400)").size() == 3);
    // EXISTS is a plain boolean expression.
    const auto r = db.run("SELECT EXISTS (SELECT 1 FROM orders WHERE customer_id = 2) AS bob_ordered, "
                          "NOT EXISTS (SELECT 1 FROM orders WHERE customer_id = 3) AS carol_did_not FROM customers WHERE id = 1");
    CHECK(r.rows[0] == Row{Value::boolean(true), Value::boolean(true)});
}

// ---- scalar subqueries -----------------------------------------------------

TEST_CASE("scalar subqueries in projection and WHERE") {
    Db db = shop();
    auto r = db.run("SELECT name, (SELECT max(total) FROM orders) AS best FROM customers WHERE id = 1");
    CHECK(r.columns == std::vector<std::string>{"name", "best"});
    CHECK(r.rows[0] == Row{t("alice"), f(50.0)});
    CHECK(column(db.rows("SELECT id FROM orders WHERE total > (SELECT avg(total) FROM orders) ORDER BY id"), 0) ==
          std::vector<Value>{i(10), i(11)});
    // No row -> NULL; several rows -> error.
    CHECK(db.rows("SELECT (SELECT total FROM orders WHERE id = 99) FROM customers WHERE id = 1")[0][0].isNull());
    auto e = db.fail("SELECT (SELECT total FROM orders) FROM customers");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "row 1: scalar subquery returned 4 rows; at most one is allowed");
    // Nested: a subquery inside a subquery.
    CHECK(column(db.rows("SELECT name FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total = "
                         "(SELECT min(total) FROM orders WHERE customer_id IS NOT NULL))"), 0) ==
          std::vector<Value>{t("bob")});
}

TEST_CASE("subquery binding errors") {
    Db db = shop();
    auto e = db.fail("SELECT * FROM customers WHERE id IN (SELECT id, total FROM orders)");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "1:31: subquery must return exactly 1 column, got 2");
    CHECK(db.fail("SELECT (SELECT * FROM orders) FROM customers").message ==
          "1:8: subquery must return exactly 1 column, got 3");
    e = db.fail("SELECT * FROM customers WHERE name IN (SELECT total FROM orders)");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:31: cannot compare TEXT with the subquery's FLOAT");
    // Not correlated: the outer table is not visible inside.
    e = db.fail("SELECT * FROM customers c WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id)");
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "in subquery: 1:86: unknown table alias 'c'");
    CHECK(db.fail("SELECT * FROM customers WHERE id IN (SELECT nope FROM orders)").message ==
          "in subquery: 1:45: unknown column 'nope' in table 'orders'");
    CHECK(db.fail("UPDATE customers SET city = (SELECT city FROM customers WHERE id = 1)").message ==
          "1:29: subqueries are only allowed inside a SELECT");
}

// ---- UNION -----------------------------------------------------------------

TEST_CASE("UNION removes duplicates, UNION ALL keeps them") {
    Db db = shop();
    auto r = db.run("SELECT city FROM customers UNION SELECT 'lyon' FROM orders");
    CHECK(r.columns == std::vector<std::string>{"city"});  // header from the first member
    CHECK(column(r.rows, 0) == std::vector<Value>{t("paris"), t("lyon"), N});
    r = db.run("SELECT city FROM customers UNION ALL SELECT 'lyon' FROM orders WHERE id = 10");
    CHECK(column(r.rows, 0) == std::vector<Value>{t("paris"), t("lyon"), N, t("lyon")});
}

TEST_CASE("UNION with ORDER BY, LIMIT/OFFSET and mixed numeric types") {
    Db db = shop();
    auto r = db.run("SELECT id AS n FROM customers UNION SELECT total FROM orders ORDER BY n DESC LIMIT 3 OFFSET 1");
    CHECK(r.columns == std::vector<std::string>{"n"});
    CHECK(column(r.rows, 0) == std::vector<Value>{f(25.5), f(10.0), i(3)});
    // Three members, aggregates in one of them.
    r = db.run("SELECT name FROM customers WHERE id = 1 UNION ALL SELECT name FROM customers WHERE id = 2 "
               "UNION ALL SELECT max(name) FROM customers ORDER BY name");
    CHECK(column(r.rows, 0) == std::vector<Value>{t("alice"), t("bob"), t("carol")});
}

TEST_CASE("UNION errors") {
    Db db = shop();
    auto e = db.fail("SELECT id, name FROM customers UNION SELECT id FROM orders");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "UNION members must have the same number of columns (2 and 1)");
    e = db.fail("SELECT name FROM customers UNION SELECT total FROM orders");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "UNION column 1 mixes TEXT and FLOAT");
    e = db.fail("SELECT name FROM customers UNION SELECT name FROM customers ORDER BY id");
    CHECK(e.message == "1:70: ORDER BY on a UNION must name an output column");
}
