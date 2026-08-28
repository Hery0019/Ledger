#include "doctest.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "server/http.h"

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

TEST_CASE("HTTP round-trip: POST /query and GET /health over a real socket") {
    TempDb t("server_http");
    httplib::Server server;
    std::mutex mtx;
    attachRoutes(server, *t.db, mtx);
    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread serving([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    {
        httplib::Client client("127.0.0.1", port);

        const auto health = client.Get("/health");
        REQUIRE(health);
        CHECK(health->status == 200);
        CHECK(contains(health->body, R"("ok":true)"));

        const auto script = client.Post(
            "/query", "CREATE TABLE t (n INT); INSERT INTO t VALUES (7); SELECT n FROM t;", "text/plain");
        REQUIRE(script);
        CHECK(script->status == 200);
        CHECK(script->body ==
              R"({"results":[{"kind":"ddl"},{"kind":"dml","affected":1},)"
              R"({"kind":"select","columns":["n"],"rows":[[7]]}]})");

        const auto bad = client.Post("/query", "SELECT * FROM missing;", "text/plain");
        REQUIRE(bad);
        CHECK(bad->status == 400);
        CHECK(contains(bad->body, R"("code":"NotFound")"));
    }

    server.stop();
    serving.join();
}
