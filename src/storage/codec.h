#pragma once

#include <string>
#include <string_view>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"
#include "storage/engine.h"

// Text encoding of a table's files. Kept apart from the engine so it can be
// tested alone: this is where escaping bugs hide.
//
// schema.txt:
//   ledger-schema 1
//   <column> <TYPE> [PK] [NN] [DEF:<value> | DEFNULL]
//     PK implies NN, which is then not written. Attributes carrying a payload
//     use `KEY:<payload>` with the payload escaped like a text field plus
//     `\s` for spaces (escapeAttr), so a line stays space-separated.
//
// rows.txt (append-only):
//   ledger-rows 1
//   I <rowid>\t<field>\t<field>...     insertion (or new version after update)
//   D <rowid>                          tombstone
//
// Fields: Int/Float/Bool via Value::toText; NULL = `\N`; Text escaped
// (`\\` `\t` `\n` `\r`). An empty string is an empty field, distinct from `\N`.
namespace ledger::codec {

inline constexpr std::string_view kSchemaHeader = "ledger-schema 1";
inline constexpr std::string_view kRowsHeader = "ledger-rows 1";

std::string escapeText(std::string_view text);
Result<std::string> unescapeText(std::string_view field);  // Corruption on invalid escape

// Same as escapeText, with spaces written `\s`: for payloads inside a
// space-separated schema line.
std::string escapeAttr(std::string_view text);
Result<std::string> unescapeAttr(std::string_view field);

std::string encodeValue(const Value& value);
Result<Value> decodeValue(std::string_view field, DataType type);  // Corruption

// Full content of schema.txt.
std::string encodeSchema(const TableSchema& schema);
Result<TableSchema> decodeSchema(std::string_view tableName, std::string_view content);

// One line of rows.txt, without the trailing `\n`.
std::string encodeInsert(RowId id, const Row& row);
std::string encodeTombstone(RowId id);

struct Record {
    enum class Kind { Insert, Delete };
    Kind kind;
    RowId id;
    Row row;  // empty for Delete
};
Result<Record> decodeRecord(std::string_view line, const TableSchema& schema);

}  // namespace ledger::codec
