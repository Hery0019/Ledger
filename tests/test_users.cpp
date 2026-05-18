#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <variant>

#include "cli/database.h"
#include "core/password.h"
#include "exec/executor.h"
#include "sql/parser.h"
#include "storage/memory_engine.h"

using namespace ledger;
namespace fs = std::filesystem;

namespace {

// Engine + catalog + executor over MemoryEngine, like the executor tests.
struct Fixture {
    MemoryEngine engine;
    Catalog catalog;
    Executor exec{engine, catalog};

    Result<QueryResult> run(std::string_view sql) { return exec.execute(sql); }
};

}  // namespace

// ---- parsing -----------------------------------------------------------------

TEST_CASE("parse CREATE USER / ALTER USER / DROP USER") {
    auto create = parse("CREATE USER alice PASSWORD 'S3cret!'");
    REQUIRE(create.ok());
    const auto* c = std::get_if<ast::CreateUser>(&create.value());
    REQUIRE(c);
    CHECK(c->name == "alice");
    CHECK(c->password == "S3cret!");  // NOT folded: passwords keep their case

    auto alter = parse("alter user Alice password 'new'");
    REQUIRE(alter.ok());
    const auto* a = std::get_if<ast::AlterUser>(&alter.value());
    REQUIRE(a);
    CHECK(a->name == "alice");  // identifier, folded
    CHECK(a->password == "new");

    auto drop = parse("DROP USER alice");
    REQUIRE(drop.ok());
    const auto* d = std::get_if<ast::DropUser>(&drop.value());
    REQUIRE(d);
    CHECK(d->name == "alice");
}

TEST_CASE("parse errors for user statements") {
    for (const auto* sql : {
             "CREATE USER alice",                  // missing PASSWORD
             "CREATE USER alice PASSWORD",         // missing string
             "CREATE USER alice PASSWORD secret",  // identifier, not a string
             "CREATE USER alice PASSWORD ''",      // empty password
             "CREATE USER 'alice' PASSWORD 'x'",   // name must be an identifier
             "ALTER USER alice",                   // missing PASSWORD
             "ALTER TABLE t",                      // only ALTER USER exists
             "DROP USER",                          // missing name
         }) {
        auto r = parse(sql);
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK(r.error().code == ErrorCode::SyntaxError);
    }
}

// ---- catalog -------------------------------------------------------------------

TEST_CASE("catalog user accounting") {
    Catalog c;
    CHECK_FALSE(c.hasUsers());
    REQUIRE(c.addUser(makeUser("bob", "pw")).ok());
    REQUIRE(c.addUser(makeUser("alice", "pw")).ok());
    CHECK(c.hasUsers());
    CHECK(c.findUser("alice") != nullptr);
    CHECK(c.findUser("carol") == nullptr);
    CHECK(c.addUser(makeUser("alice", "pw")).error().code == ErrorCode::AlreadyExists);
    CHECK(c.userNames() == std::vector<std::string_view>{"alice", "bob"});  // sorted
    REQUIRE(c.removeUser("bob").ok());
    CHECK(c.removeUser("bob").error().code == ErrorCode::NotFound);
    CHECK(c.replaceUser(makeUser("nobody", "pw")).error().code == ErrorCode::NotFound);
}

// ---- execution -----------------------------------------------------------------

TEST_CASE("CREATE USER stores a verifiable salted hash, never the password") {
    Fixture f;
    REQUIRE(f.run("CREATE USER alice PASSWORD 'S3cret!'").ok());
    const UserDef* u = f.catalog.findUser("alice");
    REQUIRE(u);
    CHECK(verifyPassword(*u, "S3cret!"));
    CHECK_FALSE(verifyPassword(*u, "wrong"));
    CHECK(u->hashHex.find("S3cret") == std::string::npos);
    // Persisted through the engine as well.
    auto stored = f.engine.loadUsers();
    REQUIRE(stored.ok());
    REQUIRE(stored.value().size() == 1);
    CHECK(verifyPassword(stored.value()[0], "S3cret!"));
}

TEST_CASE("duplicate, unknown and altered users") {
    Fixture f;
    REQUIRE(f.run("CREATE USER alice PASSWORD 'one'").ok());
    CHECK(f.run("CREATE USER alice PASSWORD 'two'").error().code == ErrorCode::AlreadyExists);
    CHECK(f.run("ALTER USER bob PASSWORD 'x'").error().code == ErrorCode::NotFound);
    CHECK(f.run("DROP USER bob").error().code == ErrorCode::NotFound);

    REQUIRE(f.run("ALTER USER alice PASSWORD 'two'").ok());
    CHECK_FALSE(verifyPassword(*f.catalog.findUser("alice"), "one"));
    CHECK(verifyPassword(*f.catalog.findUser("alice"), "two"));

    REQUIRE(f.run("DROP USER alice").ok());
    CHECK_FALSE(f.catalog.hasUsers());
    CHECK(f.engine.loadUsers().value().empty());
}

TEST_CASE("a user and a table may share a name") {
    Fixture f;
    REQUIRE(f.run("CREATE TABLE alice (n INT)").ok());
    REQUIRE(f.run("CREATE USER alice PASSWORD 'pw'").ok());
    CHECK(f.run("SELECT * FROM alice").ok());
}

TEST_CASE("user statements are refused inside a transaction") {
    Fixture f;
    REQUIRE(f.run("BEGIN").ok());
    CHECK_FALSE(f.run("CREATE USER alice PASSWORD 'pw'").ok());
    REQUIRE(f.run("ROLLBACK").ok());
    CHECK(f.run("CREATE USER alice PASSWORD 'pw'").ok());
}

// ---- persistence ----------------------------------------------------------------

TEST_CASE("users survive a database reopen (users.txt round-trip)") {
    const fs::path dir = fs::temp_directory_path() / "ledger_test_users_persist";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE USER alice PASSWORD 'S3cret!'").ok());
        REQUIRE(db->execute("CREATE USER bob PASSWORD 'other'").ok());
        REQUIRE(db->execute("DROP USER bob").ok());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(db->catalog().userNames() == std::vector<std::string_view>{"alice"});
        const UserDef* u = db->catalog().findUser("alice");
        REQUIRE(u);
        CHECK(verifyPassword(*u, "S3cret!"));
        // ALTER persists too.
        REQUIRE(db->execute("ALTER USER alice PASSWORD 'renewed'").ok());
    }
    {
        auto db = Database::open(dir).value();
        CHECK(verifyPassword(*db->catalog().findUser("alice"), "renewed"));
    }
    fs::remove_all(dir);
}

TEST_CASE("a corrupted users.txt is refused at open") {
    const fs::path dir = fs::temp_directory_path() / "ledger_test_users_corrupt";
    fs::remove_all(dir);
    {
        auto db = Database::open(dir).value();
        REQUIRE(db->execute("CREATE USER alice PASSWORD 'pw'").ok());
    }
    {
        std::ofstream(dir / "users.txt") << "ledger-users 1\nalice\tonly-two-fields\n";
    }
    auto reopened = Database::open(dir);
    REQUIRE_FALSE(reopened.ok());
    CHECK(reopened.error().code == ErrorCode::Corruption);
    fs::remove_all(dir);
}
