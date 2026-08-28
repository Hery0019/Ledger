#include "doctest.h"

#include <filesystem>
#include <memory>
#include <string>

#include "server/handler.h"

using namespace ledger;
namespace fs = std::filesystem;

namespace {

// Throwaway database directory, unique per test, destroyed on exit.
struct TempDb {
    fs::path path;
    std::unique_ptr<Database> db;
    explicit TempDb(const char* name) : path(fs::temp_directory_path() / ("ledger_test_" + std::string(name))) {
        fs::remove_all(path);
        db = Database::open(path).value();
    }
    ~TempDb() {
        db.reset();
        fs::remove_all(path);
    }
};

bool contains(const std::string& body, std::string_view needle) {
    return body.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("handleQuery: a script runs in order and returns one result per statement") {
    TempDb t("handler_script");
    const auto reply = handleQuery(*t.db,
                                   "CREATE TABLE users (id INT PRIMARY KEY, name TEXT);"
                                   "INSERT INTO users VALUES (1, 'Alice');"
                                   "SELECT * FROM users;");
    CHECK(reply.status == 200);
    CHECK(reply.body ==
          R"({"results":[{"kind":"ddl"},{"kind":"dml","affected":1},)"
          R"({"kind":"select","columns":["id","name"],"rows":[[1,"Alice"]]}]})");
}

TEST_CASE("handleQuery: an empty request is refused") {
    TempDb t("handler_empty");
    for (const auto* body : {"", "   \n", "-- just a comment\n"}) {
        const auto reply = handleQuery(*t.db, body);
        CHECK(reply.status == 400);
        CHECK(contains(reply.body, R"("code":"EmptyRequest")"));
    }
}

TEST_CASE("handleQuery: an error reports code, line, and keeps prior results") {
    TempDb t("handler_error");
    const auto reply = handleQuery(*t.db, "CREATE TABLE t (n INT);\nSELECT * FROM nope;");
    CHECK(reply.status == 400);
    CHECK(contains(reply.body, R"("code":"NotFound")"));
    CHECK(contains(reply.body, R"("line":2)"));
    // The CREATE ran before the failure and stays applied.
    CHECK(contains(reply.body, R"("results":[{"kind":"ddl"}])"));
    CHECK(handleQuery(*t.db, "SELECT * FROM t;").status == 200);
}

TEST_CASE("handleQuery: a transaction committed within the request persists") {
    TempDb t("handler_tx_ok");
    REQUIRE(handleQuery(*t.db, "CREATE TABLE t (n INT);").status == 200);
    const auto reply = handleQuery(*t.db,
                                   "BEGIN; INSERT INTO t VALUES (1); INSERT INTO t VALUES (2); COMMIT;");
    CHECK(reply.status == 200);
    CHECK_FALSE(t.db->inTransaction());
    const auto check = handleQuery(*t.db, "SELECT COUNT(*) AS n FROM t;");
    CHECK(contains(check.body, R"("rows":[[2]])"));
}

TEST_CASE("handleQuery: a transaction left open is rolled back and refused") {
    TempDb t("handler_tx_open");
    REQUIRE(handleQuery(*t.db, "CREATE TABLE t (n INT);").status == 200);
    const auto reply = handleQuery(*t.db, "BEGIN; INSERT INTO t VALUES (1);");
    CHECK(reply.status == 400);
    CHECK(contains(reply.body, R"("code":"TransactionOpen")"));
    CHECK_FALSE(t.db->inTransaction());
    const auto check = handleQuery(*t.db, "SELECT COUNT(*) AS n FROM t;");
    CHECK(contains(check.body, R"("rows":[[0]])"));
}

TEST_CASE("handleQuery: an error inside a transaction rolls it back") {
    TempDb t("handler_tx_error");
    REQUIRE(handleQuery(*t.db, "CREATE TABLE t (n INT PRIMARY KEY);").status == 200);
    const auto reply = handleQuery(*t.db,
                                   "BEGIN; INSERT INTO t VALUES (1); INSERT INTO t VALUES (1); COMMIT;");
    CHECK(reply.status == 400);
    CHECK(contains(reply.body, R"("code":"ConstraintViolation")"));
    CHECK_FALSE(t.db->inTransaction());
    const auto check = handleQuery(*t.db, "SELECT COUNT(*) AS n FROM t;");
    CHECK(contains(check.body, R"("rows":[[0]])"));
}
