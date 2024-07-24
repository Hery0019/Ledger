#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

#include "cli/database.h"

using namespace ledger;

namespace {
std::vector<std::string> sqls(std::string_view text) {
    std::vector<std::string> out;
    for (const auto& s : splitStatements(text)) out.push_back(s.sql);
    return out;
}
}  // namespace

// ---- découpage -------------------------------------------------------------

TEST_CASE("splitStatements: basic splitting, trailing ; optional, empties dropped") {
    CHECK(sqls("SELECT 1; SELECT 2") == std::vector<std::string>{"SELECT 1", "SELECT 2"});
    CHECK(sqls("SELECT 1;") == std::vector<std::string>{"SELECT 1"});
    CHECK(sqls("SELECT 1;;;  ;") == std::vector<std::string>{"SELECT 1"});
    CHECK(sqls("").empty());
    CHECK(sqls("   \n\t ").empty());
    CHECK(sqls("-- only a comment\n").empty());
}

TEST_CASE("splitStatements: semicolons inside strings and comments are not separators") {
    CHECK(sqls("INSERT INTO t VALUES ('a;b'); SELECT 1") ==
          std::vector<std::string>{"INSERT INTO t VALUES ('a;b')", "SELECT 1"});
    CHECK(sqls("INSERT INTO t VALUES ('it''s;'); SELECT 1") ==
          std::vector<std::string>{"INSERT INTO t VALUES ('it''s;')", "SELECT 1"});
    CHECK(sqls("SELECT 1 -- ; not a separator\n; SELECT 2") ==
          std::vector<std::string>{"SELECT 1 -- ; not a separator\n", "SELECT 2"});
    CHECK(sqls("SELECT 1 -- unterminated comment ; still") ==
          std::vector<std::string>{"SELECT 1 -- unterminated comment ; still"});
}

TEST_CASE("splitStatements: an unterminated string swallows the rest") {
    CHECK(sqls("SELECT 'abc; SELECT 2") == std::vector<std::string>{"SELECT 'abc; SELECT 2"});
}

TEST_CASE("splitStatements: line numbers point at the first non-blank character") {
    const auto s = splitStatements("SELECT 1;\n\n  SELECT 2;\n-- c\nSELECT\n3");
    REQUIRE(s.size() == 3);
    CHECK(s[0].line == 1);
    CHECK(s[1].line == 3);
    CHECK(s[2].line == 5);
    CHECK(s[1].sql == "SELECT 2");
    CHECK(s[2].sql == "SELECT\n3");  // commentaire de tête retiré
}

TEST_CASE("endsWithCompleteStatement") {
    CHECK(endsWithCompleteStatement("SELECT 1;"));
    CHECK(endsWithCompleteStatement("SELECT 1;\n"));
    CHECK(endsWithCompleteStatement("SELECT 1; -- trailing comment\n"));
    CHECK(endsWithCompleteStatement("SELECT 1;\n-- comment\n"));
    CHECK_FALSE(endsWithCompleteStatement("SELECT 1"));
    CHECK_FALSE(endsWithCompleteStatement("SELECT 1; SELECT"));
    CHECK_FALSE(endsWithCompleteStatement("SELECT ';"));
    CHECK_FALSE(endsWithCompleteStatement("SELECT 'a;b"));
    CHECK_FALSE(endsWithCompleteStatement("SELECT 1 -- ;\n"));
    CHECK_FALSE(endsWithCompleteStatement(""));
}

// ---- affichage -------------------------------------------------------------

TEST_CASE("formatTable aligns columns and prints NULL") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"id", "name"};
    r.rows = {Row{Value::integer(1), Value::text("alice")},
              Row{Value::integer(22), Value::null()}};
    CHECK(formatTable(r) ==
          "+----+-------+\n"
          "| id | name  |\n"
          "+----+-------+\n"
          "| 1  | alice |\n"
          "| 22 | NULL  |\n"
          "+----+-------+\n");
}

TEST_CASE("formatTable counts UTF-8 code points, not bytes") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"x"};
    r.rows = {Row{Value::text("caf\xC3\xA9")}, Row{Value::text("abcd")}};
    CHECK(formatTable(r) ==
          "+------+\n"
          "| x    |\n"
          "+------+\n"
          "| caf\xC3\xA9 |\n"
          "| abcd |\n"
          "+------+\n");
}

TEST_CASE("formatTable with no rows still prints the header") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"a"};
    CHECK(formatTable(r) == "+---+\n| a |\n+---+\n+---+\n");
    CHECK(formatTable(QueryResult{}).empty());
}

TEST_CASE("formatSummary") {
    QueryResult sel;
    sel.kind = ResultKind::Select;
    sel.columns = {"a"};
    CHECK(formatSummary(sel) == "0 rows");
    sel.rows = {Row{Value::integer(1)}};
    CHECK(formatSummary(sel) == "1 row");
    sel.rows.push_back(Row{Value::integer(2)});
    CHECK(formatSummary(sel) == "2 rows");
    CHECK(formatSummary(QueryResult{{}, {}, 1, ResultKind::Dml}) == "1 row affected");
    CHECK(formatSummary(QueryResult{{}, {}, 0, ResultKind::Dml}) == "0 rows affected");
    CHECK(formatSummary(QueryResult{{}, {}, 3, ResultKind::Dml}) == "3 rows affected");
    CHECK(formatSummary(QueryResult{}) == "ok");
}

// ---- Database --------------------------------------------------------------

TEST_CASE("Database: open loads schemas, execute runs end to end, reopen keeps data") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ledger_test_cli_db";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        CHECK(db->directory() == dir);
        auto r = db->execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
        REQUIRE(r.ok());
        CHECK(r.value().kind == ResultKind::Ddl);
        r = db->execute("INSERT INTO t VALUES (1, 'x')");
        REQUIRE(r.ok());
        CHECK(r.value().kind == ResultKind::Dml);
        CHECK(r.value().affected == 1);
        CHECK(db->catalog().contains("t"));
        CHECK(db->takeWarnings().empty());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().contains("t"));
        auto r = db->execute("SELECT v FROM t");
        REQUIRE(r.ok());
        CHECK(r.value().kind == ResultKind::Select);
        REQUIRE(r.value().rows.size() == 1);
        CHECK(r.value().rows[0][0] == Value::text("x"));
        CHECK(db->execute("SELECT * FROM nope").error().code == ErrorCode::NotFound);
    }
    {
        auto reopened = Database::open(dir);
        CHECK(reopened.ok());  // le verrou a bien été relâché par la fermeture précédente
    }
    fs::remove_all(dir);
}
