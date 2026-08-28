#include "doctest.h"

#include <string>
#include <vector>

#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Scalar functions and CASE.
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
    // Evaluates one expression against a one-row table.
    Value eval(std::string_view expr) {
        const auto rows = run("SELECT " + std::string(expr) + " FROM one").rows;
        REQUIRE(rows.size() == 1);
        return rows[0][0];
    }
};

Db one() {
    Db db;
    db.run("CREATE TABLE one (n INT, x FLOAT, s TEXT, b BOOL, z TEXT)");
    db.run("INSERT INTO one VALUES (-7, 2.5, '  Caf\xC3\xA9 ', TRUE, NULL)");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }
const Value N = Value::null();

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse CASE in both forms") {
    auto text = [](std::string_view sql) {
        auto r = parse(sql);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        return ast::exprToString(*std::get<ast::Select>(r.value()).items[0].expr);
    };
    CHECK(text("SELECT CASE WHEN a > 1 THEN 'big' ELSE 'small' END FROM t") ==
          "CASE WHEN a > 1 THEN 'big' ELSE 'small' END");
    CHECK(text("SELECT CASE a WHEN 1 THEN 'one' WHEN 2 THEN 'two' END FROM t") ==
          "CASE a WHEN 1 THEN 'one' WHEN 2 THEN 'two' END");
    CHECK(text("SELECT CASE WHEN a THEN 1 END + 2 FROM t") == "CASE WHEN a THEN 1 END + 2");
    auto e = parse("SELECT CASE END FROM t");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:13: expected expression, got 'END'");
    e = parse("SELECT CASE WHEN a THEN 1 FROM t");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:27: expected 'END', got 'FROM'");
    e = parse("SELECT CASE WHEN a 1 END FROM t");
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == "1:20: expected 'THEN', got integer '1'");
}

// ---- text functions --------------------------------------------------------

TEST_CASE("upper, lower, length, trim") {
    Db db = one();
    CHECK(db.eval("upper(s)") == t("  CAF\xC3\xA9 "));  // ASCII only; the accented byte is untouched
    CHECK(db.eval("lower('ABC def')") == t("abc def"));
    CHECK(db.eval("length(s)") == i(7));                // code points, not bytes
    CHECK(db.eval("length('')") == i(0));
    CHECK(db.eval("trim(s)") == t("Caf\xC3\xA9"));
    CHECK(db.eval("upper(trim(s))") == t("CAF\xC3\xA9"));
    CHECK(db.eval("upper(z)").isNull());
    CHECK(db.eval("length(z)").isNull());
    CHECK(db.run("SELECT UPPER(s) FROM one").columns == std::vector<std::string>{"upper(s)"});
}

// ---- numeric functions -----------------------------------------------------

TEST_CASE("abs and round") {
    Db db = one();
    CHECK(db.eval("abs(n)") == i(7));
    CHECK(db.eval("abs(-x)") == f(2.5));
    CHECK(db.eval("abs(n)").type() == DataType::Int);
    CHECK(db.eval("round(x)") == f(3.0));
    CHECK(db.eval("round(2.4)") == f(2.0));
    CHECK(db.eval("round(-2.5)") == f(-3.0));  // away from zero
    CHECK(db.eval("round(n)") == f(-7.0));
    CHECK(db.eval("round(3.14159, 2)") == f(3.14));
    CHECK(db.eval("round(1234.5, -2)") == f(1200.0));
    CHECK(db.eval("round(x, 0)") == f(3.0));
    CHECK(db.eval("abs(nullif(n, -7))").isNull());
    CHECK(db.fail("SELECT abs(-9223372036854775807 - 1) FROM one").message ==
          "1:8: integer overflow in 'abs'");
}

// ---- NULL handling functions -----------------------------------------------

TEST_CASE("coalesce and nullif") {
    Db db = one();
    CHECK(db.eval("coalesce(z, s)") == t("  Caf\xC3\xA9 "));
    CHECK(db.eval("coalesce(z, z, 'fallback')") == t("fallback"));
    CHECK(db.eval("coalesce(z)").isNull());
    CHECK(db.eval("coalesce(NULL, n, 1)") == i(-7));
    CHECK(db.eval("coalesce(NULL, n, x)") == i(-7));  // mixed numeric: Float typed, Int value
    CHECK(db.eval("nullif(n, -7)").isNull());
    CHECK(db.eval("nullif(n, 0)") == i(-7));
    CHECK(db.eval("nullif(s, z)") == t("  Caf\xC3\xA9 "));
    CHECK(db.eval("nullif(z, 'x')").isNull());
    CHECK(db.eval("coalesce(nullif(n, -7), 42)") == i(42));
    CHECK(db.eval("coalesce(z, s) IS NOT NULL") == Value::boolean(true));
}

// ---- CASE ------------------------------------------------------------------

TEST_CASE("CASE searched and simple forms") {
    Db db = one();
    CHECK(db.eval("CASE WHEN n < 0 THEN 'negative' WHEN n = 0 THEN 'zero' ELSE 'positive' END") == t("negative"));
    CHECK(db.eval("CASE WHEN n > 0 THEN 'positive' END").isNull());
    CHECK(db.eval("CASE n WHEN -7 THEN 'seven' WHEN 0 THEN 'zero' ELSE 'other' END") == t("seven"));
    CHECK(db.eval("CASE n WHEN 1 THEN 'one' END").isNull());
    CHECK(db.eval("CASE z WHEN NULL THEN 'null' ELSE 'not matched' END") == t("not matched"));  // NULL = NULL is unknown
    CHECK(db.eval("CASE WHEN b THEN 1 ELSE 2.5 END") == i(1));  // mixed numeric result
    CHECK(db.eval("CASE WHEN z IS NULL THEN 0 ELSE length(z) END") == i(0));
    // Branches are only evaluated when reached: the ELSE would divide by zero.
    CHECK(db.eval("CASE WHEN n <> 0 THEN 10 / n ELSE 10 / (n + 7) END") == i(-1));
}

TEST_CASE("CASE and functions inside WHERE, ORDER BY, GROUP BY and aggregates") {
    Db db;
    db.run("CREATE TABLE p (id INT PRIMARY KEY, name TEXT NOT NULL, age INT)");
    db.run("INSERT INTO p VALUES (1, 'Ann', 30)");
    db.run("INSERT INTO p VALUES (2, 'bob', NULL)");
    db.run("INSERT INTO p VALUES (3, 'CARL', 70)");
    db.run("INSERT INTO p VALUES (4, 'dee', 12)");
    auto r = db.run("SELECT lower(name), CASE WHEN age >= 65 THEN 'senior' WHEN age >= 18 THEN 'adult' "
                    "WHEN age IS NULL THEN 'unknown' ELSE 'minor' END AS band FROM p ORDER BY band, id");
    REQUIRE(r.rows.size() == 4);
    CHECK(r.rows[0] == Row{t("ann"), t("adult")});
    CHECK(r.rows[1] == Row{t("dee"), t("minor")});
    CHECK(r.rows[2] == Row{t("carl"), t("senior")});
    CHECK(r.rows[3] == Row{t("bob"), t("unknown")});

    r = db.run("SELECT CASE WHEN age >= 18 THEN 'adult' ELSE 'other' END AS band, count(*), sum(coalesce(age, 0)) "
               "FROM p GROUP BY CASE WHEN age >= 18 THEN 'adult' ELSE 'other' END ORDER BY band");
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{t("adult"), i(2), i(100)});
    CHECK(r.rows[1] == Row{t("other"), i(2), i(12)});

    CHECK(db.run("SELECT id FROM p WHERE upper(name) LIKE '%A%' ORDER BY id").rows.size() == 2);
    CHECK(db.run("SELECT max(length(name)) FROM p").rows[0][0] == i(4));
}

// ---- errors ----------------------------------------------------------------

TEST_CASE("function and CASE binding errors") {
    Db db = one();
    auto e = db.fail("SELECT upper(n) FROM one");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(e.message == "1:8: upper() requires TEXT, got INT");
    CHECK(db.fail("SELECT length(s, s) FROM one").message == "1:8: length() takes 1 argument, got 2");
    CHECK(db.fail("SELECT round(x, 1, 2) FROM one").message == "1:8: round() takes 1 to 2 arguments, got 3");
    CHECK(db.fail("SELECT round(x, 'a') FROM one").message == "1:8: round() requires an INT digit count, got TEXT");
    CHECK(db.fail("SELECT coalesce() FROM one").message == "1:8: coalesce() takes at least 1 arguments, got 0");
    CHECK(db.fail("SELECT coalesce(n, s) FROM one").message == "1:8: coalesce() mixes INT and TEXT");
    CHECK(db.fail("SELECT nullif(n, s) FROM one").message == "1:8: nullif(): cannot compare INT with TEXT");
    CHECK(db.fail("SELECT abs(s) FROM one").message == "1:8: abs() requires INT or FLOAT, got TEXT");
    CHECK(db.fail("SELECT upper(*) FROM one").message == "1:8: upper(*) is not valid");
    CHECK(db.fail("SELECT CASE WHEN n THEN 1 END FROM one").message == "1:18: WHEN requires BOOL, got INT");
    CHECK(db.fail("SELECT CASE WHEN b THEN 1 ELSE 'x' END FROM one").message == "1:8: CASE mixes INT and TEXT");
    CHECK(db.fail("SELECT CASE s WHEN 1 THEN 1 END FROM one").message == "1:20: cannot compare TEXT with INT in CASE");
    CHECK(db.fail("SELECT frob(n) FROM one").message == "1:8: unknown function 'frob'");
}
