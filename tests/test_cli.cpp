#include "doctest.h"

#include <algorithm>
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

// ---- splitting -------------------------------------------------------------

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

TEST_CASE("splitStatements: a leading UTF-8 BOM is ignored") {
    CHECK(sqls("\xEF\xBB\xBFSELECT 1;") == std::vector<std::string>{"SELECT 1"});
    const auto s = splitStatements("\xEF\xBB\xBF\nSELECT 1;");
    REQUIRE(s.size() == 1);
    CHECK(s[0].line == 2);
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
    CHECK(s[2].sql == "SELECT\n3");  // leading comment removed
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

// ---- display ---------------------------------------------------------------

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
          "|  1 | alice |\n"
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

TEST_CASE("formatTable right-aligns numbers in every style") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"n", "x"};
    r.rows = {Row{Value::integer(7), Value::real(12.5).value()},
              Row{Value::integer(1234), Value::null()}};
    // A NULL in a numeric column follows the column's alignment.
    CHECK(formatTable(r) ==
          "+------+------+\n"
          "| n    | x    |\n"
          "+------+------+\n"
          "|    7 | 12.5 |\n"
          "| 1234 | NULL |\n"
          "+------+------+\n");
    // A column holding only NULLs, or mixed text, stays left-aligned.
    r.rows = {Row{Value::null(), Value::text("ab")}, Row{Value::integer(5), Value::text("c")}};
    CHECK(formatTable(r) ==
          "+------+----+\n"
          "| n    | x  |\n"
          "+------+----+\n"
          "| NULL | ab |\n"
          "|    5 | c  |\n"
          "+------+----+\n");
}

TEST_CASE("formatTable fancy: Unicode borders, coloured cells, widths unaffected by escapes") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"id", "ok"};
    r.rows = {Row{Value::integer(1), Value::boolean(true)}, Row{Value::null(), Value::boolean(false)}};

    const TableStyle unicodeOnly{true, false};
    CHECK(formatTable(r, unicodeOnly) ==
          "╭──────┬───────╮\n"
          "│ id   │ ok    │\n"
          "├──────┼───────┤\n"
          "│    1 │ true  │\n"
          "│ NULL │ false │\n"
          "╰──────┴───────╯\n");

    const std::string fancy = formatTable(r, TableStyle::fancy());
    const std::string R(ansi::reset), G(ansi::gray);
    // Header is bold cyan, numbers yellow, NULL dim, booleans green/red;
    // the padding is computed on the raw text, not on the escapes.
    CHECK(fancy.find(G + "╭──────┬───────╮" + R + "\n") == 0);
    CHECK(fancy.find(std::string(ansi::bold) + std::string(ansi::cyan) + "id" + R + "   ") != std::string::npos);
    CHECK(fancy.find("    " + std::string(ansi::yellow) + "1" + R + " ") != std::string::npos);
    CHECK(fancy.find(std::string(ansi::dim) + "NULL" + R) != std::string::npos);
    CHECK(fancy.find(std::string(ansi::green) + "true" + R + "  ") != std::string::npos);
    CHECK(fancy.find(std::string(ansi::red) + "false" + R + " ") != std::string::npos);
    // Text cells keep the default colour.
    QueryResult txt;
    txt.kind = ResultKind::Select;
    txt.columns = {"s"};
    txt.rows = {Row{Value::text("abc")}};
    CHECK(formatTable(txt, TableStyle::fancy()).find(G + "│" + R + " abc " + G + "│" + R) != std::string::npos);
}

TEST_CASE("formatTable with no rows still prints the header") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"a"};
    CHECK(formatTable(r) == "+---+\n| a |\n+---+\n+---+\n");
    CHECK(formatTable(QueryResult{}).empty());
}

TEST_CASE("formatLogo: block letters in fancy style, ASCII art in plain style") {
    const std::string plain = formatLogo(TableStyle::plain());
    CHECK(plain.find("|_____|_____|____/") != std::string::npos);
    CHECK(plain.find('\x1b') == std::string::npos);
    CHECK(plain.back() == '\n');
    CHECK(std::count(plain.begin(), plain.end(), '\n') == 5);

    const std::string blocks = formatLogo(TableStyle{true, false});
    CHECK(blocks.find("███████╗") != std::string::npos);
    CHECK(blocks.find('\x1b') == std::string::npos);
    CHECK(std::count(blocks.begin(), blocks.end(), '\n') == 6);
    // Every line of the block logo has the same display width.
    std::size_t width = 0;
    std::size_t start = 0;
    for (std::size_t nl = blocks.find('\n'); nl != std::string::npos; nl = blocks.find('\n', start)) {
        std::size_t n = 0;
        for (std::size_t i = start; i < nl; ++i) n += (static_cast<unsigned char>(blocks[i]) & 0xC0) != 0x80;
        if (width == 0) width = n;
        CHECK(n == width);
        start = nl + 1;
    }

    const std::string fancy = formatLogo(TableStyle::fancy());
    CHECK(fancy.find("\x1b[38;5;51m") == 0);
    CHECK(fancy.find(std::string(ansi::reset) + "\n") != std::string::npos);
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

// ---- database location -----------------------------------------------------

TEST_CASE("resolveDatabasePath: bare names go under the data root") {
    namespace fs = std::filesystem;
    CHECK(resolveDatabasePath("mydb", nullptr) == fs::path("data") / "mydb");
    CHECK(resolveDatabasePath("mydb", "") == fs::path("data") / "mydb");
    CHECK(resolveDatabasePath("mydb", "/srv/ledger") == fs::path("/srv/ledger") / "mydb");
    // Anything that looks like a path is used as is.
    CHECK(resolveDatabasePath("./mydb", nullptr) == fs::path("./mydb"));
    CHECK(resolveDatabasePath("../elsewhere/db", "/srv/ledger") == fs::path("../elsewhere/db"));
    CHECK(resolveDatabasePath("sub\\db", nullptr) == fs::path("sub\\db"));
    CHECK(resolveDatabasePath("C:\\x\\db", nullptr) == fs::path("C:\\x\\db"));
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
        CHECK(reopened.ok());  // the lock was released by the previous close
    }
    fs::remove_all(dir);
}
