#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "storage/file_engine.h"

using namespace ledger;
namespace fs = std::filesystem;

namespace {

// Dossier de base jetable, unique par test, détruit à la sortie.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / ("ledger_test_" + std::string(name))) {
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

TableSchema schema() {
    return TableSchema{"t",
                       {ColumnSchema{"id", DataType::Int, true, true},
                        ColumnSchema{"name", DataType::Text, false, false},
                        ColumnSchema{"score", DataType::Float, false, false}}};
}

Row row(std::int64_t id, const char* name) {
    return Row{Value::integer(id), Value::text(name), Value::real(static_cast<double>(id) / 2).value()};
}

std::unique_ptr<FileEngine> openDb(const fs::path& dir) {
    auto r = FileEngine::open(dir);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r).value();
}

std::vector<std::pair<RowId, Row>> all(IStorageEngine& e, std::string_view table) {
    std::vector<std::pair<RowId, Row>> out;
    auto r = e.scan(table, [&](RowId id, const Row& rw) { out.emplace_back(id, rw); return true; });
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void writeFile(const fs::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

std::size_t countLines(const fs::path& p) {
    const std::string s = readFile(p);
    std::size_t n = 0;
    for (char c : s) n += (c == '\n');
    return n;
}

}  // namespace

// ---- cycle de vie ----------------------------------------------------------

TEST_CASE("FileEngine: open creates the directory and the LOCK, close removes the LOCK") {
    TempDir d("open");
    {
        auto e = openDb(d.path);
        CHECK(fs::is_directory(d.path));
        CHECK(fs::exists(d.path / "LOCK"));
        CHECK(e->directory() == d.path);
    }
    CHECK_FALSE(fs::exists(d.path / "LOCK"));
}

TEST_CASE("FileEngine: a second open on a locked database fails") {
    TempDir d("lock");
    auto first = openDb(d.path);
    auto second = FileEngine::open(d.path);
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == ErrorCode::IoError);
    CHECK(second.error().message.find("database is locked") == 0);
}

TEST_CASE("FileEngine: createTable writes schema.txt and an empty rows.txt") {
    TempDir d("create");
    auto e = openDb(d.path);
    REQUIRE(e->createTable(schema()).ok());
    CHECK(readFile(d.path / "t" / "schema.txt") ==
          "ledger-schema 1\nid INT PK\nname TEXT\nscore FLOAT\n");
    CHECK(readFile(d.path / "t" / "rows.txt") == "ledger-rows 1\n");
    CHECK(e->createTable(schema()).error().code == ErrorCode::AlreadyExists);
    CHECK(all(*e, "t").empty());
}

TEST_CASE("FileEngine: reserved Windows device names are refused as table names") {
    TempDir d("reserved");
    auto e = openDb(d.path);
    for (const char* name : {"con", "nul", "com1", "lpt9"}) {
        TableSchema s{name, {ColumnSchema{"a", DataType::Int, false, false}}};
        auto r = e->createTable(s);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::IoError);
    }
    TableSchema ok{"com10", {ColumnSchema{"a", DataType::Int, false, false}}};
    CHECK(e->createTable(ok).ok());
}

// ---- persistance -----------------------------------------------------------

TEST_CASE("FileEngine: rows survive close and reopen, rowids continue") {
    TempDir d("persist");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
        CHECK(e->insert("t", row(1, "alice")).value() == 1);
        CHECK(e->insert("t", row(2, "bob")).value() == 2);
    }
    CHECK(readFile(d.path / "t" / "rows.txt") ==
          "ledger-rows 1\nI 1\t1\talice\t0.5\nI 2\t2\tbob\t1\n");
    {
        auto e = openDb(d.path);
        const auto schemas = e->loadSchemas().value();
        REQUIRE(schemas.size() == 1);
        CHECK(schemas[0].name == "t");
        CHECK(schemas[0].columns.size() == 3);
        const auto rows = all(*e, "t");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == 1);
        CHECK(rows[0].second == row(1, "alice"));
        CHECK(rows[1].second == row(2, "bob"));
        CHECK(e->insert("t", row(3, "carol")).value() == 3);
    }
}

TEST_CASE("FileEngine: update appends a tombstone and a new version with the same rowid") {
    TempDir d("update");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
        REQUIRE(e->insert("t", row(1, "a")).ok());
        REQUIRE(e->update("t", 1, row(1, "A")).ok());
        CHECK(e->update("t", 9, row(1, "x")).error().code == ErrorCode::NotFound);
    }
    CHECK(readFile(d.path / "t" / "rows.txt") ==
          "ledger-rows 1\nI 1\t1\ta\t0.5\nD 1\nI 1\t1\tA\t0.5\n");
    auto e = openDb(d.path);
    const auto rows = all(*e, "t");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == 1);
    CHECK(rows[0].second == row(1, "A"));
}

TEST_CASE("FileEngine: remove appends a tombstone and the row is gone after reopen") {
    TempDir d("remove");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
        REQUIRE(e->insert("t", row(1, "a")).ok());
        REQUIRE(e->insert("t", row(2, "b")).ok());
        REQUIRE(e->remove("t", 1).ok());
        CHECK(e->remove("t", 1).error().code == ErrorCode::NotFound);
    }
    CHECK(readFile(d.path / "t" / "rows.txt").find("\nD 1\n") != std::string::npos);
    auto e = openDb(d.path);
    const auto rows = all(*e, "t");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == 2);
    CHECK(e->insert("t", row(3, "c")).value() == 3);  // 1 n'est jamais réutilisé
}

TEST_CASE("FileEngine: text with tabs, newlines and NULL round-trips through the file") {
    TempDir d("escape");
    const Row tricky{Value::integer(1), Value::text("a\tb\nc\\d\r"), Value::null()};
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
        REQUIRE(e->insert("t", tricky).ok());
    }
    CHECK(countLines(d.path / "t" / "rows.txt") == 2);  // en-tête + une ligne
    auto e = openDb(d.path);
    CHECK(all(*e, "t")[0].second == tricky);
}

TEST_CASE("FileEngine: dropTable removes the directory") {
    TempDir d("drop");
    auto e = openDb(d.path);
    REQUIRE(e->createTable(schema()).ok());
    REQUIRE(e->insert("t", row(1, "a")).ok());
    REQUIRE(e->dropTable("t").ok());
    CHECK_FALSE(fs::exists(d.path / "t"));
    CHECK(e->dropTable("t").error().code == ErrorCode::NotFound);
    CHECK(e->loadSchemas().value().empty());
    REQUIRE(e->createTable(schema()).ok());  // recréable
    CHECK(e->insert("t", row(1, "a")).value() == 1);
}

// ---- compaction ------------------------------------------------------------

TEST_CASE("FileEngine: explicit compact rewrites the file without tombstones") {
    TempDir d("compact");
    auto e = openDb(d.path);
    REQUIRE(e->createTable(schema()).ok());
    for (int i = 1; i <= 5; ++i) REQUIRE(e->insert("t", row(i, "x")).ok());
    REQUIRE(e->remove("t", 2).ok());
    REQUIRE(e->remove("t", 4).ok());
    REQUIRE(e->update("t", 5, row(5, "five")).ok());
    CHECK(countLines(d.path / "t" / "rows.txt") == 1 + 5 + 2 + 2);
    REQUIRE(e->compact("t").ok());
    CHECK(readFile(d.path / "t" / "rows.txt") ==
          "ledger-rows 1\nI 1\t1\tx\t0.5\nI 3\t3\tx\t1.5\nI 5\t5\tfive\t2.5\n");
    CHECK_FALSE(fs::exists(d.path / "t" / "rows.txt.tmp"));
    // Le fichier reste utilisable après compaction.
    CHECK(e->insert("t", row(6, "six")).value() == 6);
    CHECK(countLines(d.path / "t" / "rows.txt") == 4 + 1);
    auto rows = all(*e, "t");
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].second == row(6, "six"));
}

TEST_CASE("FileEngine: automatic compaction when tombstones dominate") {
    TempDir d("autocompact");
    auto e = openDb(d.path);
    REQUIRE(e->createTable(schema()).ok());
    const int n = static_cast<int>(FileEngine::kCompactMinTombstones) + 10;
    for (int i = 1; i <= n; ++i) REQUIRE(e->insert("t", row(i, "x")).ok());
    // Supprime toutes sauf une : tombstones > seuil et > lignes vivantes.
    for (int i = 1; i < n; ++i) REQUIRE(e->remove("t", static_cast<RowId>(i)).ok());
    // La compaction s'est déclenchée à la 1001e suppression (9 lignes vivantes
    // restaient) ; les 8 suppressions suivantes ont été ajoutées après.
    CHECK(countLines(d.path / "t" / "rows.txt") == 1 + 9 + 8);
    auto rows = all(*e, "t");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].first == static_cast<RowId>(n));
}

// ---- robustesse ------------------------------------------------------------

TEST_CASE("FileEngine: a truncated last line is ignored with a warning") {
    TempDir d("truncated");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
        REQUIRE(e->insert("t", row(1, "a")).ok());
        REQUIRE(e->insert("t", row(2, "b")).ok());
    }
    const fs::path rows = d.path / "t" / "rows.txt";
    writeFile(rows, readFile(rows) + "I 3\t3\tcar");  // append interrompu, pas de '\n'
    auto e = openDb(d.path);
    const auto got = all(*e, "t");
    REQUIRE(got.size() == 2);
    const auto warnings = e->takeWarnings();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("ignoring truncated last line 'I 3\t3\tcar'") != std::string::npos);
    CHECK(e->takeWarnings().empty());
    // La prochaine écriture repart proprement sur sa propre ligne.
    CHECK(e->insert("t", row(3, "c")).value() == 3);
    CHECK(all(*e, "t").size() == 3);
    e.reset();
    auto again = openDb(d.path);
    CHECK(again->takeWarnings().empty());
    CHECK(all(*again, "t").size() == 3);
}

TEST_CASE("FileEngine: corrupted rows.txt is reported with file and line") {
    TempDir d("corrupt");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
    }
    const fs::path rows = d.path / "t" / "rows.txt";

    SUBCASE("bad field") {
        writeFile(rows, "ledger-rows 1\nI 1\tone\ta\t0.5\n");
        auto e = openDb(d.path);
        auto r = e->scan("t", [](RowId, const Row&) { return true; });
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Corruption);
        CHECK(r.error().message.find("rows.txt:2: row 1, column 'id': invalid INT literal: 'one'") !=
              std::string::npos);
    }
    SUBCASE("tombstone for unknown row") {
        writeFile(rows, "ledger-rows 1\nD 7\n");
        auto e = openDb(d.path);
        auto r = e->scan("t", [](RowId, const Row&) { return true; });
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("rows.txt:2: tombstone for unknown row 7") != std::string::npos);
    }
    SUBCASE("duplicate live row") {
        writeFile(rows, "ledger-rows 1\nI 1\t1\ta\t0.5\nI 1\t1\tb\t0.5\n");
        auto e = openDb(d.path);
        auto r = e->scan("t", [](RowId, const Row&) { return true; });
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("rows.txt:3: duplicate live row 1") != std::string::npos);
    }
    SUBCASE("bad header") {
        writeFile(rows, "something else\n");
        auto e = openDb(d.path);
        auto r = e->scan("t", [](RowId, const Row&) { return true; });
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("rows.txt:1: missing or unknown header") != std::string::npos);
    }
    SUBCASE("empty file") {
        writeFile(rows, "");
        auto e = openDb(d.path);
        auto r = e->scan("t", [](RowId, const Row&) { return true; });
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("empty file (missing header)") != std::string::npos);
    }
}

TEST_CASE("FileEngine: corrupted schema.txt fails at open") {
    TempDir d("badschema");
    {
        auto e = openDb(d.path);
        REQUIRE(e->createTable(schema()).ok());
    }
    writeFile(d.path / "t" / "schema.txt", "ledger-schema 1\nid DATE\n");
    auto r = FileEngine::open(d.path);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Corruption);
    CHECK(r.error().message.find("schema.txt:2: unknown type 'DATE'") != std::string::npos);
    // Le verrou n'est pas laissé derrière.
    CHECK_FALSE(fs::exists(d.path / "LOCK"));
}

TEST_CASE("FileEngine: foreign directories without schema.txt are ignored") {
    TempDir d("foreign");
    fs::create_directories(d.path / "not_a_table");
    auto e = openDb(d.path);
    CHECK(e->loadSchemas().value().empty());
    CHECK(e->insert("not_a_table", Row{}).error().code == ErrorCode::NotFound);
}
