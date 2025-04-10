#include "doctest.h"

#include <string>
#include <vector>

#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Aggregates, GROUP BY and HAVING, from the parser down to the executor.
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

Db sales() {
    Db db;
    db.run("CREATE TABLE sales (id INT PRIMARY KEY, region TEXT NOT NULL, amount FLOAT, qty INT)");
    db.run("INSERT INTO sales VALUES (1, 'north', 10.0, 2)");
    db.run("INSERT INTO sales VALUES (2, 'north', 20.0, 3)");
    db.run("INSERT INTO sales VALUES (3, 'south', 5.5, NULL)");
    db.run("INSERT INTO sales VALUES (4, 'south', NULL, 1)");
    db.run("INSERT INTO sales VALUES (5, 'east', 7.0, 4)");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
const Value N = Value::null();

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse function calls, GROUP BY and HAVING") {
    auto r = parse("SELECT region, count(*), SUM(amount) AS total FROM sales "
                   "WHERE qty > 0 GROUP BY region, qty HAVING count(*) > 1 ORDER BY total");
    REQUIRE(r.ok());
    const auto& s = std::get<ast::Select>(r.value());
    REQUIRE(s.items.size() == 3);
    const auto& count = std::get<ast::Call>(s.items[1].expr->node);
    CHECK(count.name == "count");
    CHECK(count.star);
    CHECK(count.args.empty());
    const auto& sum = std::get<ast::Call>(s.items[2].expr->node);
    CHECK(sum.name == "sum");
    CHECK_FALSE(sum.star);
    CHECK(sum.args.size() == 1);
    CHECK(s.items[2].alias == "total");
    REQUIRE(s.groupBy.size() == 2);
    CHECK(ast::exprToString(*s.groupBy[1]) == "qty");
    REQUIRE(s.having != nullptr);
    CHECK(ast::exprToString(*s.having) == "count(*) > 1");
    CHECK(ast::exprToString(*s.items[2].expr) == "sum(amount)");

    CHECK(ast::exprToString(*std::get<ast::Select>(parse("SELECT f(a, b + 1, 'x') FROM t").value()).items[0].expr) ==
          "f(a, b + 1, 'x')");
    CHECK(ast::exprToString(*std::get<ast::Select>(parse("SELECT f() FROM t").value()).items[0].expr) == "f()");
    auto e = parse("SELECT count( FROM t");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:15: expected expression, got 'FROM'");
    e = parse("SELECT a FROM t GROUP a");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:23: expected 'BY', got identifier 'a'");
}

// ---- whole-table aggregates ------------------------------------------------

TEST_CASE("aggregates over the whole table") {
    Db db = sales();
    const auto r = db.run("SELECT count(*), count(amount), sum(amount), avg(amount), min(amount), max(amount), "
                          "sum(qty), min(region), max(region) FROM sales");
    CHECK(r.columns == std::vector<std::string>{"count(*)", "count(amount)", "sum(amount)", "avg(amount)",
                                                "min(amount)", "max(amount)", "sum(qty)", "min(region)",
                                                "max(region)"});
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0] == Row{i(5), i(4), f(42.5), f(10.625), f(5.5), f(20.0), i(10), t("east"), t("south")});
}

TEST_CASE("aggregates on an empty input: one row, COUNT 0, others NULL") {
    Db db = sales();
    const auto r = db.run("SELECT count(*), sum(qty), avg(amount), min(region) FROM sales WHERE id > 100");
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0] == Row{i(0), N, N, N});
}

TEST_CASE("aggregate result types and Int/Float sums") {
    Db db = sales();
    auto r = db.run("SELECT sum(qty), sum(amount), avg(qty), count(*) FROM sales");
    CHECK(r.rows[0][0].type() == DataType::Int);
    CHECK(r.rows[0][1].type() == DataType::Float);
    CHECK(r.rows[0][2] == f(2.5));  // (2+3+1+4)/4, NULL skipped
    CHECK(r.rows[0][3].type() == DataType::Int);
    // Expressions around aggregates, and aggregates over expressions.
    r = db.run("SELECT sum(amount * qty) / count(qty), max(qty) - min(qty) AS spread FROM sales");
    CHECK(r.columns == std::vector<std::string>{"sum(amount * qty) / count(qty)", "spread"});
    CHECK(r.rows[0] == Row{f((20.0 + 60.0 + 28.0) / 4), i(3)});
}

TEST_CASE("sum() over Int refuses to overflow") {
    Db db;
    db.run("CREATE TABLE big (v INT)");
    db.run("INSERT INTO big VALUES (9223372036854775807)");
    db.run("INSERT INTO big VALUES (1)");
    const auto e = db.fail("SELECT sum(v) FROM big");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "row 2: integer overflow in sum()");
}

// ---- GROUP BY --------------------------------------------------------------

TEST_CASE("GROUP BY one column, groups in order of first appearance") {
    Db db = sales();
    const auto r = db.run("SELECT region, count(*), sum(amount) FROM sales GROUP BY region");
    CHECK(r.columns == std::vector<std::string>{"region", "count(*)", "sum(amount)"});
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[0] == Row{t("north"), i(2), f(30.0)});
    CHECK(r.rows[1] == Row{t("south"), i(2), f(5.5)});
    CHECK(r.rows[2] == Row{t("east"), i(1), f(7.0)});
}

TEST_CASE("GROUP BY several keys, NULL keys form their own group") {
    Db db = sales();
    db.run("INSERT INTO sales VALUES (6, 'south', 1.0, NULL)");
    const auto r = db.run("SELECT region, qty, count(*) FROM sales GROUP BY region, qty ORDER BY region, qty");
    REQUIRE(r.rows.size() == 5);
    CHECK(r.rows[0] == Row{t("east"), i(4), i(1)});
    CHECK(r.rows[1] == Row{t("north"), i(2), i(1)});
    CHECK(r.rows[2] == Row{t("north"), i(3), i(1)});
    CHECK(r.rows[3] == Row{t("south"), N, i(2)});  // NULL first in ASC
    CHECK(r.rows[4] == Row{t("south"), i(1), i(1)});
}

TEST_CASE("GROUP BY an expression, reused verbatim in the projection") {
    Db db = sales();
    const auto r = db.run("SELECT qty * 2 AS dbl, count(*) FROM sales WHERE qty IS NOT NULL GROUP BY qty * 2 ORDER BY dbl");
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{i(2), i(1)});
    CHECK(r.rows[3] == Row{i(8), i(1)});
}

TEST_CASE("HAVING filters groups and may use aggregates not projected") {
    Db db = sales();
    auto r = db.run("SELECT region FROM sales GROUP BY region HAVING count(*) > 1 ORDER BY region");
    CHECK(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("north")});
    CHECK(r.rows[1] == Row{t("south")});
    r = db.run("SELECT region, sum(amount) AS total FROM sales GROUP BY region HAVING sum(amount) >= 7 ORDER BY total DESC");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("north"), f(30.0)});
    CHECK(r.rows[1] == Row{t("east"), f(7.0)});
    // HAVING may also test a group key.
    r = db.run("SELECT region FROM sales GROUP BY region HAVING region <> 'east'");
    CHECK(r.rows.size() == 2);
}

TEST_CASE("ORDER BY and LIMIT apply to the groups") {
    Db db = sales();
    const auto r = db.run("SELECT region, count(*) AS n FROM sales GROUP BY region ORDER BY n DESC, region LIMIT 2");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("north"), i(2)});
    CHECK(r.rows[1] == Row{t("south"), i(2)});
    // ORDER BY an aggregate that is not projected.
    const auto r2 = db.run("SELECT region FROM sales GROUP BY region ORDER BY max(amount) DESC");
    CHECK(r2.rows[0] == Row{t("north")});
    CHECK(r2.rows[2] == Row{t("south")});
}

TEST_CASE("WHERE runs before grouping") {
    Db db = sales();
    const auto r = db.run("SELECT region, count(*) FROM sales WHERE amount > 6 GROUP BY region");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("north"), i(2)});
    CHECK(r.rows[1] == Row{t("east"), i(1)});
}

TEST_CASE("aggregates work through a view") {
    Db db = sales();
    db.run("CREATE VIEW paid AS SELECT region, amount FROM sales WHERE amount IS NOT NULL");
    const auto r = db.run("SELECT region, sum(amount) FROM paid GROUP BY region ORDER BY region");
    REQUIRE(r.rows.size() == 3);
    CHECK(r.rows[2] == Row{t("south"), f(5.5)});
}

// ---- errors ----------------------------------------------------------------

TEST_CASE("aggregate binding errors") {
    Db db = sales();
    auto e = db.fail("SELECT region, count(*) FROM sales");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "1:8: column 'region' must appear in GROUP BY or be used in an aggregate function");
    CHECK(db.fail("SELECT region, qty FROM sales GROUP BY region").message ==
          "1:16: column 'qty' must appear in GROUP BY or be used in an aggregate function");
    CHECK(db.fail("SELECT * FROM sales GROUP BY region").message ==
          "SELECT * cannot be used with GROUP BY or aggregate functions");
    CHECK(db.fail("SELECT count(*) FROM sales WHERE count(*) > 1").message ==
          "1:34: aggregate functions are not allowed in WHERE");
    CHECK(db.fail("UPDATE sales SET qty = max(qty)").message ==
          "1:24: aggregate function 'max()' is not allowed here");
    CHECK(db.fail("SELECT sum(count(*)) FROM sales").message == "1:8: aggregate functions cannot be nested");
    CHECK(db.fail("SELECT sum(*) FROM sales").message == "1:8: sum(*) is not valid; only count(*) is");
    CHECK(db.fail("SELECT sum(qty, amount) FROM sales").message == "1:8: sum() takes exactly one argument, got 2");
    CHECK(db.fail("SELECT count() FROM sales").message == "1:8: count() takes exactly one argument, got 0");
    e = db.fail("SELECT sum(region) FROM sales");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:8: sum() requires INT or FLOAT, got TEXT");
    CHECK(db.fail("SELECT frobnicate(qty) FROM sales").message == "1:8: unknown function 'frobnicate'");
    CHECK(db.fail("SELECT region FROM sales GROUP BY count(*)").message ==
          "1:35: aggregate functions are not allowed in GROUP BY");
    CHECK(db.fail("SELECT region FROM sales GROUP BY region HAVING region").message ==
          "1:49: HAVING must be BOOL, got TEXT");
    CHECK(db.fail("SELECT region FROM sales GROUP BY nope").message == "1:35: unknown column 'nope' in table 'sales'");
    CHECK(db.fail("CREATE VIEW v AS SELECT count(*) FROM sales").message ==
          "view 'v': aggregate functions are not allowed in a view");
    CHECK(db.fail("CREATE VIEW v AS SELECT region FROM sales GROUP BY region").message ==
          "view 'v': GROUP BY and HAVING are not allowed in a view");
}
