#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cli/database.h"
#include "exec/executor.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "storage/file_engine.h"
#include "storage/memory_engine.h"

using namespace ledger;

// Views end to end: parser, catalog, binder expansion, executor, persistence.
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
    db.run("CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, score FLOAT, active BOOL)");
    db.run("INSERT INTO users VALUES (1, 'alice', 3.5, TRUE)");
    db.run("INSERT INTO users VALUES (2, 'bob', NULL, FALSE)");
    db.run("INSERT INTO users VALUES (3, 'carol', 1.0, TRUE)");
    db.run("INSERT INTO users VALUES (4, 'dave', 9.0, TRUE)");
    db.run("CREATE VIEW active_users AS SELECT id, name, score FROM users WHERE active");
    return db;
}

Value i(std::int64_t v) { return Value::integer(v); }
Value t(const char* s) { return Value::text(s); }
Value f(double d) { return Value::real(d).value(); }

std::vector<Value> column(const std::vector<Row>& rows, std::size_t idx) {
    std::vector<Value> out;
    for (const auto& r : rows) out.push_back(r[idx]);
    return out;
}

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST_CASE("parse CREATE VIEW keeps the SELECT verbatim, without the trailing ;") {
    auto r = parse("CREATE VIEW v AS SELECT a, b FROM t WHERE a > 1 ;");
    REQUIRE(r.ok());
    const auto* v = std::get_if<ast::CreateView>(&r.value());
    REQUIRE(v != nullptr);
    CHECK(v->name == "v");
    CHECK(v->queryText == "SELECT a, b FROM t WHERE a > 1");
    CHECK(v->query.from.name == "t");
    REQUIRE(v->query.items.size() == 2);
    CHECK(std::get<ast::ColumnRef>(v->query.items[1].expr->node).name == "b");
    REQUIRE(v->query.where != nullptr);

    r = parse("create view V as select * from T");
    REQUIRE(r.ok());
    CHECK(std::get<ast::CreateView>(r.value()).queryText == "select * from T");
    CHECK(std::get<ast::CreateView>(r.value()).name == "v");

    r = parse("CREATE VIEW v AS SELECT *\n  FROM t\n  -- comment\n  WHERE x;");
    REQUIRE(r.ok());
    CHECK(std::get<ast::CreateView>(r.value()).queryText == "SELECT *\n  FROM t\n  -- comment\n  WHERE x");
}

TEST_CASE("parse DROP VIEW and CREATE/DROP errors") {
    auto r = parse("DROP VIEW v");
    REQUIRE(r.ok());
    CHECK(std::get<ast::DropView>(r.value()).name == "v");

    auto e = [](std::string_view sql) {
        auto p = parse(sql);
        REQUIRE_FALSE(p.ok());
        return p.error().message;
    };
    CHECK(e("CREATE VIEW v AS SELECT * FROM t ORDER BY a") ==
          "view 'v': ORDER BY and LIMIT are not allowed in a view");
    CHECK(e("CREATE VIEW v AS SELECT * FROM t LIMIT 1") ==
          "view 'v': ORDER BY and LIMIT are not allowed in a view");
    CHECK(e("CREATE VIEW v SELECT * FROM t") == "1:15: expected 'AS', got 'SELECT'");
    CHECK(e("CREATE VIEW v AS INSERT INTO t VALUES (1)") == "1:18: expected SELECT after AS, got 'INSERT'");
    CHECK(e("CREATE INDEX i") == "1:8: expected 'TABLE' or 'VIEW', got identifier 'index'");
    CHECK(e("DROP v") == "1:6: expected 'TABLE' or 'VIEW', got identifier 'v'");
    CHECK(e("CREATE VIEW AS SELECT * FROM t") == "1:13: expected identifier, got 'AS'");
}

TEST_CASE("VIEW and AS are keywords") {
    auto toks = tokenize("view as").value();
    CHECK(toks[0].kind == TokenKind::KwView);
    CHECK(toks[1].kind == TokenKind::KwAs);
    CHECK(toks[1].isKeyword());
    CHECK(tokenKindName(TokenKind::KwView) == "VIEW");
}

TEST_CASE("tokens carry their byte offset") {
    auto toks = tokenize("ab  cd\n'x'").value();
    CHECK(toks[0].offset == 0);
    CHECK(toks[1].offset == 4);
    CHECK(toks[2].offset == 7);
    CHECK(toks[3].offset == 10);  // End
}

// ---- catalog ---------------------------------------------------------------

TEST_CASE("Catalog: views share the table namespace and track their source") {
    Catalog c;
    REQUIRE(c.add(TableSchema{"t", {ColumnSchema{"a", DataType::Int, false, false}}}).ok());
    REQUIRE(c.addView(ViewDef{"v", "SELECT * FROM t"}, {"t"}).ok());
    REQUIRE(c.addView(ViewDef{"w", "SELECT * FROM v"}, {"v"}).ok());

    CHECK(c.hasName("t"));
    CHECK(c.hasName("v"));
    CHECK_FALSE(c.hasName("nope"));
    CHECK(c.find("v") == nullptr);  // a view is not a table
    REQUIRE(c.findView("v") != nullptr);
    CHECK(c.findView("v")->sources == std::vector<std::string>{"t"});
    CHECK(c.viewNames() == std::vector<std::string_view>{"v", "w"});  // creation order
    CHECK(c.dependents("t") == std::vector<std::string_view>{"v"});
    CHECK(c.dependents("v") == std::vector<std::string_view>{"w"});
    CHECK(c.dependents("w").empty());

    CHECK(c.add(TableSchema{"v", {ColumnSchema{"a", DataType::Int, false, false}}}).error().code ==
          ErrorCode::AlreadyExists);
    CHECK(c.addView(ViewDef{"t", "SELECT * FROM t"}, {"t"}).error().message == "table 't' already exists");
    CHECK(c.addView(ViewDef{"v", "SELECT * FROM t"}, {"t"}).error().message == "view 'v' already exists");
    CHECK(c.removeView("nope").error().code == ErrorCode::NotFound);
    REQUIRE(c.removeView("w").ok());
    CHECK(c.views().size() == 1);
    CHECK(c.views()[0].sql == "SELECT * FROM t");
}

// ---- querying views --------------------------------------------------------

TEST_CASE("SELECT * FROM view exposes the view's columns and applies its filter") {
    Db db = seeded();
    const auto r = db.run("SELECT * FROM active_users");
    CHECK(r.columns == std::vector<std::string>{"id", "name", "score"});
    CHECK(column(r.rows, 0) == std::vector<Value>{i(1), i(3), i(4)});
    CHECK(r.rows[0] == Row{i(1), t("alice"), f(3.5)});
}

TEST_CASE("a query on a view composes its own WHERE, ORDER BY and LIMIT") {
    Db db = seeded();
    CHECK(column(db.rows("SELECT id FROM active_users WHERE score > 2 ORDER BY score DESC"), 0) ==
          std::vector<Value>{i(4), i(1)});
    CHECK(column(db.rows("SELECT name FROM active_users ORDER BY id DESC LIMIT 2"), 0) ==
          std::vector<Value>{t("dave"), t("carol")});
    CHECK(db.rows("SELECT id FROM active_users WHERE name = 'bob'").empty());  // filtered by the view
}

TEST_CASE("columns hidden by the view are not reachable") {
    Db db = seeded();
    auto e = db.fail("SELECT active FROM active_users");
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "1:8: unknown column 'active' in view 'active_users'");
    CHECK(db.fail("SELECT id FROM active_users WHERE active").message ==
          "1:35: unknown column 'active' in view 'active_users'");
    CHECK(db.fail("SELECT id FROM active_users ORDER BY active").message ==
          "1:38: unknown column 'active' in view 'active_users'");
}

TEST_CASE("a view over a view composes filters and projections") {
    Db db = seeded();
    db.run("CREATE VIEW top AS SELECT name FROM active_users WHERE score >= 3");
    const auto r = db.run("SELECT * FROM top ORDER BY name");
    CHECK(r.columns == std::vector<std::string>{"name"});
    CHECK(column(r.rows, 0) == std::vector<Value>{t("alice"), t("dave")});
    CHECK(db.fail("SELECT id FROM top").message == "1:8: unknown column 'id' in view 'top'");
}

TEST_CASE("a view without a filter or projection is a plain alias") {
    Db db = seeded();
    db.run("CREATE VIEW everyone AS SELECT * FROM users");
    const auto r = db.run("SELECT * FROM everyone WHERE NOT active");
    CHECK(r.columns == std::vector<std::string>{"id", "name", "score", "active"});
    CHECK(column(r.rows, 1) == std::vector<Value>{t("bob")});
}

TEST_CASE("a view may expose computed, aliased columns") {
    Db db = seeded();
    db.run("CREATE VIEW scored AS SELECT id, score * 2 AS dbl, name FROM users WHERE score IS NOT NULL");
    const auto r = db.run("SELECT dbl, name FROM scored WHERE dbl > 5 ORDER BY dbl DESC");
    CHECK(r.columns == std::vector<std::string>{"dbl", "name"});
    REQUIRE(r.rows.size() == 2);
    CHECK(r.rows[0] == Row{f(18.0), t("dave")});
    CHECK(r.rows[1] == Row{f(7.0), t("alice")});
    // A computed column without alias cannot be referenced later: refused.
    auto e = db.fail("CREATE VIEW bad AS SELECT id, score * 2 FROM users");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "view 'bad': column 2 (score * 2) needs an alias (use AS)");
    e = db.fail("CREATE VIEW bad AS SELECT id, name AS id FROM users");
    CHECK(e.message == "view 'bad': duplicate column name 'id'");
}

TEST_CASE("a view reflects later changes to its table") {
    Db db = seeded();
    db.run("INSERT INTO users VALUES (5, 'eve', 0.5, TRUE)");
    db.run("UPDATE users SET active = FALSE WHERE id = 1");
    CHECK(column(db.rows("SELECT id FROM active_users"), 0) == std::vector<Value>{i(3), i(4), i(5)});
}

// ---- DDL rules -------------------------------------------------------------

TEST_CASE("CREATE VIEW validates its SELECT against the catalog") {
    Db db = seeded();
    auto e = db.fail("CREATE VIEW v AS SELECT * FROM nope");
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "unknown table or view 'nope'");
    e = db.fail("CREATE VIEW v AS SELECT nope FROM users");
    CHECK(e.message == "1:25: unknown column 'nope' in table 'users'");
    e = db.fail("CREATE VIEW v AS SELECT id FROM users WHERE name + 1 > 0");
    CHECK(e.code == ErrorCode::TypeError);
    CHECK(db.catalog.findView("v") == nullptr);  // nothing registered
}

TEST_CASE("names are shared between tables and views") {
    Db db = seeded();
    CHECK(db.fail("CREATE VIEW users AS SELECT * FROM users").message == "'users' already exists");
    CHECK(db.fail("CREATE TABLE active_users (a INT)").message == "'active_users' already exists");
    CHECK(db.fail("CREATE VIEW active_users AS SELECT * FROM users").message ==
          "'active_users' already exists");
}

TEST_CASE("views are read-only") {
    Db db = seeded();
    auto e = db.fail("INSERT INTO active_users VALUES (9, 'x', 1.0)");
    CHECK(e.code == ErrorCode::SyntaxError);
    CHECK(e.message == "'active_users' is a view; views are read-only");
    CHECK(db.fail("UPDATE active_users SET name = 'x'").message == "'active_users' is a view; views are read-only");
    CHECK(db.fail("DELETE FROM active_users").message == "'active_users' is a view; views are read-only");
    CHECK(db.rows("SELECT * FROM users").size() == 4);
}

TEST_CASE("DROP TABLE / DROP VIEW are refused while a view depends on the target") {
    Db db = seeded();
    db.run("CREATE VIEW top AS SELECT name FROM active_users WHERE score >= 3");

    auto e = db.fail("DROP TABLE users");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "table 'users' is used by view 'active_users'");
    e = db.fail("DROP VIEW active_users");
    CHECK(e.code == ErrorCode::ConstraintViolation);
    CHECK(e.message == "view 'active_users' is used by view 'top'");

    // Dropping in dependency order works.
    db.run("DROP VIEW top");
    db.run("DROP VIEW active_users");
    db.run("DROP TABLE users");
    CHECK(db.catalog.viewNames().empty());
    CHECK_FALSE(db.catalog.contains("users"));
}

TEST_CASE("DROP with the wrong keyword is explained") {
    Db db = seeded();
    CHECK(db.fail("DROP TABLE active_users").message == "'active_users' is a view; use DROP VIEW");
    CHECK(db.fail("DROP VIEW users").message == "'users' is a table; use DROP TABLE");
    auto e = db.fail("DROP VIEW nope");
    CHECK(e.code == ErrorCode::NotFound);
    CHECK(e.message == "unknown view 'nope'");
}

TEST_CASE("DROP VIEW removes it from the catalog and the engine") {
    Db db = seeded();
    db.run("DROP VIEW active_users");
    CHECK(db.catalog.findView("active_users") == nullptr);
    CHECK(db.engine.loadViews().value().empty());
    CHECK(db.fail("SELECT * FROM active_users").code == ErrorCode::NotFound);
    db.run("CREATE VIEW active_users AS SELECT id FROM users");  // name is free again
    CHECK(db.engine.loadViews().value().size() == 1);
}

// ---- persistence -----------------------------------------------------------

TEST_CASE("views survive close and reopen on FileEngine, in creation order") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_views";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE TABLE t (id INT PRIMARY KEY, kind TEXT NOT NULL, note TEXT)").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (1, 'a', 'x')").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (2, 'b', NULL)").ok());
        REQUIRE(db->execute("INSERT INTO t VALUES (3, 'a', 'multi\nline')").ok());
        REQUIRE(db->execute("CREATE VIEW kind_a AS SELECT id, note FROM t\n  WHERE kind = 'a'").ok());
        REQUIRE(db->execute("CREATE VIEW noted AS SELECT id FROM kind_a WHERE note IS NOT NULL").ok());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().viewNames() == std::vector<std::string_view>{"kind_a", "noted"});
        CHECK(db->catalog().findView("kind_a")->def.sql == "SELECT id, note FROM t\n  WHERE kind = 'a'");
        auto r = db->execute("SELECT * FROM noted ORDER BY id");
        REQUIRE(r.ok());
        CHECK(column(r.value().rows, 0) == std::vector<Value>{i(1), i(3)});
        CHECK(db->execute("DROP TABLE t").error().code == ErrorCode::ConstraintViolation);
        REQUIRE(db->execute("DROP VIEW noted").ok());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().viewNames() == std::vector<std::string_view>{"kind_a"});
    }
    fs::remove_all(dir);
}

TEST_CASE("FileEngine: views.txt format and corruption") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_views_file";
    fs::remove_all(dir);
    {
        auto e = FileEngine::open(dir).value();
        CHECK(e->loadViews().value().empty());  // no file yet
        REQUIRE(e->saveViews({ViewDef{"v", "SELECT *\tFROM t"}, ViewDef{"w", "SELECT * FROM v"}}).ok());
    }
    std::ifstream in(dir / "views.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(content == "ledger-views 1\nv\tSELECT *\\tFROM t\nw\tSELECT * FROM v\n");
    {
        auto e = FileEngine::open(dir).value();
        const auto views = e->loadViews().value();
        REQUIRE(views.size() == 2);
        CHECK(views[0].name == "v");
        CHECK(views[0].sql == "SELECT *\tFROM t");
        CHECK(views[1].name == "w");
    }
    {
        std::ofstream out(dir / "views.txt", std::ios::binary | std::ios::trunc);
        out << "ledger-views 1\nbroken line without tab\n";
    }
    {
        auto e = FileEngine::open(dir).value();
        auto r = e->loadViews();
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Corruption);
        CHECK(r.error().message.find("views.txt:2: malformed view line") != std::string::npos);
    }
    fs::remove_all(dir);
}

TEST_CASE("Database::open rejects a views.txt whose definition no longer parses") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_views_bad";
    fs::remove_all(dir);
    {
        auto e = FileEngine::open(dir).value();
        REQUIRE(e->saveViews({ViewDef{"v", "SELEC oops"}}).ok());
    }
    auto db = Database::open(dir);
    REQUIRE_FALSE(db.ok());
    CHECK(db.error().code == ErrorCode::Corruption);
    CHECK(db.error().message.find("views.txt: view 'v'") == 0);
    CHECK_FALSE(fs::exists(dir / "LOCK"));
    fs::remove_all(dir);
}
