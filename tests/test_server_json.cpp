#include "doctest.h"

#include <string>

#include "server/json.h"

using namespace ledger;

// ---- strings -----------------------------------------------------------------

TEST_CASE("jsonString quotes and escapes") {
    CHECK(jsonString("") == R"("")");
    CHECK(jsonString("Alice") == R"("Alice")");
    CHECK(jsonString("a\"b") == R"("a\"b")");
    CHECK(jsonString("a\\b") == R"("a\\b")");
    CHECK(jsonString("a\nb\tc\rd") == R"("a\nb\tc\rd")");
    CHECK(jsonString("\b\f") == R"("\b\f")");
    CHECK(jsonString(std::string_view("\x01\x1f", 2)) == "\"\\u0001\\u001f\"");
    // Bytes >= 0x20 pass through untouched (UTF-8 stays UTF-8).
    CHECK(jsonString("é;{}") == R"("é;{}")");
}

// ---- values ------------------------------------------------------------------

TEST_CASE("toJson(Value) maps SQL types onto JSON types") {
    CHECK(toJson(Value::null()) == "null");
    CHECK(toJson(Value::integer(42)) == "42");
    CHECK(toJson(Value::integer(-7)) == "-7");
    CHECK(toJson(Value::boolean(true)) == "true");
    CHECK(toJson(Value::boolean(false)) == "false");
    CHECK(toJson(Value::text("it's")) == R"("it's")");
    CHECK(toJson(Value::real(3.5).value()) == "3.5");
    CHECK(toJson(Value::real(-0.25).value()) == "-0.25");
}

// ---- results -----------------------------------------------------------------

TEST_CASE("toJson(QueryResult) for DDL and DML") {
    QueryResult ddl;
    ddl.kind = ResultKind::Ddl;
    CHECK(toJson(ddl) == R"({"kind":"ddl"})");

    QueryResult dml;
    dml.kind = ResultKind::Dml;
    dml.affected = 3;
    CHECK(toJson(dml) == R"({"kind":"dml","affected":3})");

    // An INSERT that generated its key reports it.
    QueryResult withKey;
    withKey.kind = ResultKind::Dml;
    withKey.affected = 1;
    withKey.key = Value::integer(7);
    CHECK(toJson(withKey) == R"({"kind":"dml","affected":1,"key":7})");
    withKey.key = Value::fromText(DataType::Uuid, "550e8400-e29b-41d4-a716-446655440000").value();
    CHECK(toJson(withKey) ==
          R"({"kind":"dml","affected":1,"key":"550e8400-e29b-41d4-a716-446655440000"})");
}

TEST_CASE("toJson(QueryResult) for SELECT, NULLs included") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"id", "name", "score"};
    r.rows.push_back({Value::integer(1), Value::text("Alice"), Value::real(3.5).value()});
    r.rows.push_back({Value::integer(2), Value::text("Bob"), Value::null()});
    CHECK(toJson(r) ==
          R"({"kind":"select","columns":["id","name","score"],)"
          R"("rows":[[1,"Alice",3.5],[2,"Bob",null]]})");
}

TEST_CASE("toJson(QueryResult) for an empty SELECT") {
    QueryResult r;
    r.kind = ResultKind::Select;
    r.columns = {"n"};
    CHECK(toJson(r) == R"({"kind":"select","columns":["n"],"rows":[]})");
}
