#include "doctest.h"

#include <string>
#include <vector>

#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Joins, table aliases and qualified column names.
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
    db.run("INSERT INTO orders VALUES (13, 9, 1.0)");  // orphan
    db.run("INSERT INTO orders VALUES (14, NULL, 2.0)");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
const Value N = Value::null();

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse FROM with aliases, joins and qualified names") {
    auto r = parse("SELECT c.name, o.total, o.* FROM customers AS c "
                   "JOIN orders o ON o.customer_id = c.id "
                   "LEFT OUTER JOIN orders o2 ON o2.id = o.id INNER JOIN customers c2 ON c2.id = c.id");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const auto& s = std::get<ast::Select>(r.value());
    CHECK(s.from.name == "customers");
    CHECK(s.from.alias == "c");
    REQUIRE(s.joins.size() == 3);
    CHECK(s.joins[0].kind == ast::JoinKind::Inner);
    CHECK(s.joins[0].table.alias == "o");
    CHECK(ast::exprToString(*s.joins[0].on) == "o.customer_id = c.id");
    CHECK(s.joins[1].kind == ast::JoinKind::Left);
    CHECK(s.joins[2].kind == ast::JoinKind::Inner);
    REQUIRE(s.items.size() == 3);
    const auto& c = std::get<ast::ColumnRef>(s.items[0].expr->node);
    CHECK(c.qualifier == "c");
    CHECK(c.name == "name");
    CHECK(s.items[2].expr == nullptr);
    CHECK(s.items[2].starOf == "o");

    r = parse("SELECT * FROM t");
    CHECK(std::get<ast::Select>(r.value()).from.alias == "t");  // alias defaults to the name

    auto e = parse("SELECT * FROM a JOIN b");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:23: expected 'ON', got 'end of input'");
    e = parse("SELECT * FROM a LEFT b ON x");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:22: expected 'JOIN', got identifier 'b'");
    e = parse("SELECT * FROM a WHERE t.* = 1");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:25: expected column name after '.' (t.* is only valid in the SELECT list), got '*'");
    e = parse("SELECT a. FROM t");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:11: expected identifier, got 'FROM'");
}

// ---- inner join ------------------------------------------------------------

TEST_CASE("INNER JOIN pairs matching rows, in left-then-right order") {
    Db db = shop();
    const auto r = db.run("SELECT c.name, o.id, o.total FROM customers c JOIN orders o ON o.customer_id = c.id");
    CHECK(r.columns == std::vector<std::string>{"name", "id", "total"});
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0] == Row{t("alice"), i(10), f(50.0)});
    CHECK(r.rows[1] == Row{t("alice"), i(11), f(25.5)});
    CHECK(r.rows[2] == Row{t("bob"), i(12), f(10.0)});
}

TEST_CASE("JOIN without INNER, with WHERE, ORDER BY and expressions across tables") {
    Db db = shop();
    const auto r = db.run("SELECT c.name, o.total * 2 AS dbl FROM customers c JOIN orders o ON c.id = o.customer_id "
                          "WHERE o.total > 20 ORDER BY dbl DESC");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("alice"), f(100.0)});
    CHECK(r.rows[1] == Row{t("alice"), f(51.0)});
}

TEST_CASE("SELECT * and t.* over a join expose every column, qualified names disambiguate") {
    Db db = shop();
    auto r = db.run("SELECT * FROM customers c JOIN orders o ON o.customer_id = c.id WHERE o.id = 12");
    CHECK(r.columns == std::vector<std::string>{"id", "name", "city", "id", "customer_id", "total"});
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0] == Row{i(2), t("bob"), t("lyon"), i(12), i(2), f(10.0)});

    r = db.run("SELECT o.*, c.name FROM customers c JOIN orders o ON o.customer_id = c.id WHERE c.id = 2");
    CHECK(r.columns == std::vector<std::string>{"id", "customer_id", "total", "name"});
    CHECK(r.rows[0] == Row{i(12), i(2), f(10.0), t("bob")});

    // Unambiguous column names need no qualifier.
    r = db.run("SELECT name, total FROM customers c JOIN orders o ON o.customer_id = c.id ORDER BY total LIMIT 1");
    CHECK(r.rows[0] == Row{t("bob"), f(10.0)});
}

// ---- left join -------------------------------------------------------------

TEST_CASE("LEFT JOIN keeps unmatched left rows with NULLs on the right") {
    Db db = shop();
    const auto r = db.run("SELECT c.name, o.id FROM customers c LEFT JOIN orders o ON o.customer_id = c.id ORDER BY c.id, o.id");
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{t("alice"), i(10)});
    CHECK(r.rows[1] == Row{t("alice"), i(11)});
    CHECK(r.rows[2] == Row{t("bob"), i(12)});
    CHECK(r.rows[3] == Row{t("carol"), N});
    // The classic "rows without a match" query.
    const auto none = db.run("SELECT c.name FROM customers c LEFT JOIN orders o ON o.customer_id = c.id WHERE o.id IS NULL");
    REQUIRE(none.rows.size() == 1);
    CHECK(none.rows[0] == Row{t("carol")});
}

TEST_CASE("a NULL join key never matches") {
    Db db = shop();
    const auto r = db.run("SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON c.id = o.customer_id ORDER BY o.id");
    REQUIRE(r.rows.size() == 5);
    CHECK(r.rows[3] == Row{i(13), N});  // orphan
    CHECK(r.rows[4] == Row{i(14), N});  // NULL key
}

// ---- several joins, self join, aggregates ----------------------------------

TEST_CASE("three-way join and a self join through aliases") {
    Db db = shop();
    db.run("CREATE TABLE cities (name TEXT PRIMARY KEY, country TEXT)");
    db.run("INSERT INTO cities VALUES ('paris', 'fr')");
    db.run("INSERT INTO cities VALUES ('lyon', 'fr')");
    const auto r = db.run("SELECT c.name, ci.country, o.total FROM customers c "
                          "JOIN cities ci ON ci.name = c.city JOIN orders o ON o.customer_id = c.id "
                          "ORDER BY o.total");
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0] == Row{t("bob"), t("fr"), f(10.0)});

    // Pairs of customers from the same city, without duplicates.
    db.run("INSERT INTO customers VALUES (4, 'dan', 'paris')");
    const auto pairs = db.run("SELECT a.name, b.name FROM customers a JOIN customers b ON a.city = b.city AND a.id < b.id");
    CHECK(pairs.columns == std::vector<std::string>{"name", "name"});
    REQUIRE(pairs.rows.size() == 1);
    CHECK(pairs.rows[0] == Row{t("alice"), t("dan")});
}

TEST_CASE("aggregates over a join") {
    Db db = shop();
    const auto r = db.run("SELECT c.name, count(o.id) AS orders, sum(o.total) AS spent "
                          "FROM customers c LEFT JOIN orders o ON o.customer_id = c.id "
                          "GROUP BY c.name ORDER BY spent DESC");
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0] == Row{t("alice"), i(2), f(75.5)});
    CHECK(r.rows[1] == Row{t("bob"), i(1), f(10.0)});
    CHECK(r.rows[2] == Row{t("carol"), i(0), N});
}

// ---- views and joins -------------------------------------------------------

TEST_CASE("views can contain joins and take part in joins") {
    Db db = shop();
    db.run("CREATE VIEW big_orders AS SELECT o.id AS order_id, c.name AS customer, o.total "
           "FROM orders o JOIN customers c ON c.id = o.customer_id WHERE o.total >= 20");
    auto r = db.run("SELECT * FROM big_orders ORDER BY order_id");
    CHECK(r.columns == std::vector<std::string>{"order_id", "customer", "total"});
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{i(10), t("alice"), f(50.0)});

    // A view on the right side of a LEFT JOIN keeps its own filter.
    r = db.run("SELECT c.name, b.order_id FROM customers c LEFT JOIN big_orders b ON b.customer = c.name ORDER BY c.id, b.order_id");
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{t("alice"), i(10)});
    CHECK(r.rows[1] == Row{t("alice"), i(11)});
    CHECK(r.rows[2] == Row{t("bob"), N});
    CHECK(r.rows[3] == Row{t("carol"), N});

    // Dependencies cover every joined source.
    CHECK(db.catalog.findView("big_orders")->sources == std::vector<std::string>{"orders", "customers"});
    CHECK(db.fail("DROP TABLE customers").message == "table 'customers' is used by view 'big_orders'");
    CHECK(db.fail("DROP TABLE orders").message == "table 'orders' is used by view 'big_orders'");
}

TEST_CASE("a view is aliased like a table") {
    Db db = shop();
    db.run("CREATE VIEW paris AS SELECT id, name FROM customers WHERE city = 'paris'");
    const auto r = db.run("SELECT p.name, o.total FROM paris p JOIN orders o ON o.customer_id = p.id ORDER BY o.total");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("alice"), f(25.5)});
    CHECK(db.fail("SELECT paris.name FROM paris p").message == "1:8: unknown table alias 'paris'");
}

// ---- errors ----------------------------------------------------------------

TEST_CASE("join binding errors") {
    Db db = shop();
    auto e = db.fail("SELECT id FROM customers c JOIN orders o ON o.customer_id = c.id");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "1:8: ambiguous column 'id' (use c.id or o.id)");
    e = db.fail("SELECT x.name FROM customers c JOIN orders o ON o.customer_id = c.id");
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "1:8: unknown table alias 'x'");
    CHECK(db.fail("SELECT c.nope FROM customers c JOIN orders o ON o.customer_id = c.id").message ==
          "1:8: unknown column 'c.nope'");
    CHECK(db.fail("SELECT nope FROM customers c JOIN orders o ON o.customer_id = c.id").message ==
          "1:8: unknown column 'nope'");
    CHECK(db.fail("SELECT * FROM customers c JOIN orders c ON c.id = c.id").message == "duplicate table alias 'c'");
    CHECK(db.fail("SELECT * FROM customers c JOIN nope n ON n.id = c.id").message == "unknown table or view 'nope'");
    e = db.fail("SELECT * FROM customers c JOIN orders o ON o.total");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:44: ON must be BOOL, got FLOAT");
    CHECK(db.fail("SELECT * FROM customers c JOIN orders o ON count(*) > 0").message ==
          "1:44: aggregate function 'count()' is not allowed here");
    CHECK(db.fail("SELECT x.* FROM customers c").message == "unknown table alias 'x'");
    // Single-source messages keep naming the source.
    CHECK(db.fail("SELECT nope FROM customers").message == "1:8: unknown column 'nope' in table 'customers'");
    CHECK(db.fail("SELECT c.nope FROM customers c").message == "1:8: unknown column 'c.nope'");
}
