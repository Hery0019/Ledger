#include "doctest.h"

#include <string>

#include "storage/codec.h"

using namespace ledger;
using namespace ledger::codec;

namespace {

TableSchema schema() {
    return TableSchema{"t",
                       {ColumnSchema{"id", DataType::Int, true, true},
                        ColumnSchema{"name", DataType::Text, false, false},
                        ColumnSchema{"score", DataType::Float, false, false},
                        ColumnSchema{"ok", DataType::Bool, false, false}}};
}

Value f(double d) { return Value::real(d).value(); }

std::string errorOf(const Result<Record>& r) {
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Corruption);
    return r.error().message;
}

}  // namespace

// ---- text ------------------------------------------------------------------

TEST_CASE("escapeText / unescapeText round-trip") {
    const std::string cases[] = {"", "plain", "tab\there", "line\nbreak", "cr\rlf\n", "back\\slash",
                                 "\\N", "\\t literal", "caf\xC3\xA9", "\\\\\t\n\r"};
    for (const auto& s : cases) {
        CAPTURE(s);
        const std::string enc = escapeText(s);
        CHECK(enc.find('\t') == std::string::npos);
        CHECK(enc.find('\n') == std::string::npos);
        CHECK(enc.find('\r') == std::string::npos);
        CHECK(unescapeText(enc).value() == s);
    }
    CHECK(escapeText("a\tb") == "a\\tb");
    CHECK(escapeText("a\\b") == "a\\\\b");
}

TEST_CASE("unescapeText rejects invalid escapes") {
    CHECK_FALSE(unescapeText("abc\\").ok());
    CHECK_FALSE(unescapeText("\\x").ok());
    CHECK(unescapeText("\\x").error().code == ErrorCode::Corruption);
}

// ---- values ----------------------------------------------------------------

TEST_CASE("encodeValue / decodeValue round-trip for every type") {
    CHECK(encodeValue(Value::null()) == "\\N");
    CHECK(encodeValue(Value::integer(-42)) == "-42");
    CHECK(encodeValue(f(2.5)) == "2.5");
    CHECK(encodeValue(Value::boolean(true)) == "true");
    CHECK(encodeValue(Value::text("a\tb")) == "a\\tb");
    CHECK(encodeValue(Value::text("")) == "");

    CHECK(decodeValue("\\N", DataType::Int).value().isNull());
    CHECK(decodeValue("\\N", DataType::Text).value().isNull());
    CHECK(decodeValue("-42", DataType::Int).value() == Value::integer(-42));
    CHECK(decodeValue("2.5", DataType::Float).value() == f(2.5));
    CHECK(decodeValue("true", DataType::Bool).value() == Value::boolean(true));
    CHECK(decodeValue("a\\tb", DataType::Text).value() == Value::text("a\tb"));
    CHECK(decodeValue("", DataType::Text).value() == Value::text(""));
}

TEST_CASE("the text 'NULL' and the text '\\N' are not NULL") {
    // The text "NULL" is a word, not the absence of a value.
    CHECK(decodeValue("NULL", DataType::Text).value() == Value::text("NULL"));
    // The text "\N" (two characters) escapes to `\\N`, never to `\N`.
    CHECK(encodeValue(Value::text("\\N")) == "\\\\N");
    CHECK(decodeValue("\\\\N", DataType::Text).value() == Value::text("\\N"));
}

TEST_CASE("decodeValue reports corruption") {
    auto r = decodeValue("abc", DataType::Int);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Corruption);
    CHECK_FALSE(decodeValue("nan", DataType::Float).ok());
    CHECK_FALSE(decodeValue("maybe", DataType::Bool).ok());
    CHECK_FALSE(decodeValue("", DataType::Int).ok());
}

// ---- schema ----------------------------------------------------------------

TEST_CASE("encodeSchema produces the documented format") {
    CHECK(encodeSchema(schema()) ==
          "ledger-schema 1\n"
          "id INT PK\n"
          "name TEXT\n"
          "score FLOAT\n"
          "ok BOOL\n");
    TableSchema nn{"t", {ColumnSchema{"a", DataType::Text, false, true}}};
    CHECK(encodeSchema(nn) == "ledger-schema 1\na TEXT NN\n");
}

TEST_CASE("decodeSchema round-trips and applies PK => NN") {
    const auto s = decodeSchema("users", encodeSchema(schema())).value();
    CHECK(s.name == "users");
    REQUIRE(s.columns.size() == 4);
    CHECK(s.columns[0].name == "id");
    CHECK(s.columns[0].type == DataType::Int);
    CHECK(s.columns[0].primaryKey);
    CHECK(s.columns[0].notNull);
    CHECK(s.columns[1].type == DataType::Text);
    CHECK_FALSE(s.columns[1].notNull);
    CHECK(s.columns[2].type == DataType::Float);
    CHECK(s.columns[3].type == DataType::Bool);

    const auto nn = decodeSchema("t", "ledger-schema 1\na TEXT NN\n").value();
    CHECK(nn.columns[0].notNull);
    CHECK_FALSE(nn.columns[0].primaryKey);
}

TEST_CASE("decodeSchema tolerates a missing final newline") {
    CHECK(decodeSchema("t", "ledger-schema 1\na INT").value().columns.size() == 1);
}

TEST_CASE("decodeSchema rejects malformed files") {
    auto check = [](std::string_view content, std::string_view expected) {
        auto r = decodeSchema("t", content);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Corruption);
        CHECK(r.error().message == expected);
    };
    check("", "schema.txt: missing or unknown header (expected 'ledger-schema 1')");
    check("ledger-schema 2\na INT\n", "schema.txt: missing or unknown header (expected 'ledger-schema 1')");
    check("ledger-schema 1\n", "schema.txt: table has no columns");
    check("ledger-schema 1\na\n", "schema.txt:2: malformed column definition 'a'");
    check("ledger-schema 1\na INT PK NN\n", "schema.txt:2: malformed column definition 'a INT PK NN'");
    check("ledger-schema 1\na DATE\n", "schema.txt:2: unknown type 'DATE'");
    check("ledger-schema 1\na INT UNIQUE\n", "schema.txt:2: unknown constraint 'UNIQUE'");
    check("ledger-schema 1\na INT\na TEXT\n", "schema.txt:3: duplicate column 'a'");
}

// ---- records ---------------------------------------------------------------

TEST_CASE("encodeInsert / encodeTombstone") {
    const Row row{Value::integer(7), Value::text("a\tb"), Value::null(), Value::boolean(false)};
    CHECK(encodeInsert(7, row) == "I 7\t7\ta\\tb\t\\N\tfalse");
    CHECK(encodeTombstone(7) == "D 7");
}

TEST_CASE("decodeRecord round-trips inserts and reads tombstones") {
    const auto s = schema();
    const Row row{Value::integer(7), Value::text("x\ny"), f(-0.5), Value::boolean(true)};
    const auto rec = decodeRecord(encodeInsert(7, row), s).value();
    CHECK(rec.kind == Record::Kind::Insert);
    CHECK(rec.id == 7);
    CHECK(rec.row == row);

    const auto del = decodeRecord("D 12", s).value();
    CHECK(del.kind == Record::Kind::Delete);
    CHECK(del.id == 12);
    CHECK(del.row.empty());
}

TEST_CASE("decodeRecord: empty text in a single-column table") {
    TableSchema one{"t", {ColumnSchema{"a", DataType::Text, false, false}}};
    const Row row{Value::text("")};
    CHECK(encodeInsert(1, row) == "I 1\t");
    CHECK(decodeRecord("I 1\t", one).value().row == row);
}

TEST_CASE("decodeRecord reports corruption precisely") {
    const auto s = schema();
    CHECK(errorOf(decodeRecord("", s)) == "malformed record ''");
    CHECK(errorOf(decodeRecord("X 1\t1\ta\t1.0\ttrue", s)) == "malformed record 'X 1\t1\ta\t1.0\ttrue'");
    CHECK(errorOf(decodeRecord("I", s)) == "malformed record 'I'");
    CHECK(errorOf(decodeRecord("I x\t1\ta\t1.0\ttrue", s)) == "invalid rowid 'x'");
    CHECK(errorOf(decodeRecord("D -1", s)) == "invalid rowid '-1'");
    CHECK(errorOf(decodeRecord("I 1\t1\ta", s)) == "row 1: expected 4 fields, got 2");
    CHECK(errorOf(decodeRecord("I 1", s)) == "row 1: expected 4 fields, got 0");
    CHECK(errorOf(decodeRecord("I 1\t1\ta\t1.0\ttrue\textra", s)) == "row 1: expected 4 fields, got 5");
    CHECK(errorOf(decodeRecord("I 1\tone\ta\t1.0\ttrue", s)) ==
          "row 1, column 'id': invalid INT literal: 'one'");
    CHECK(errorOf(decodeRecord("I 1\t1\ta\\q\t1.0\ttrue", s)) ==
          "row 1, column 'name': invalid escape '\\q' in text field");
}
