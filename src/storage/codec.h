#pragma once

#include <string>
#include <string_view>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"
#include "storage/engine.h"

// Encodage texte des fichiers d'une table. Isolé du moteur pour être testé
// seul : c'est ici que se cachent les bugs d'échappement.
//
// schema.txt :
//   ledger-schema 1
//   <colonne> <TYPE> [PK] [NN]        (PK implique NN, qui n'est alors pas écrit)
//
// rows.txt (append-only) :
//   ledger-rows 1
//   I <rowid>\t<champ>\t<champ>...     insertion (ou nouvelle version après update)
//   D <rowid>                          tombstone
//
// Champs : Int/Float/Bool via Value::toText ; NULL = `\N` ; Text échappé
// (`\\` `\t` `\n` `\r`). Une chaîne vide est un champ vide, distinct de `\N`.
namespace ledger::codec {

inline constexpr std::string_view kSchemaHeader = "ledger-schema 1";
inline constexpr std::string_view kRowsHeader = "ledger-rows 1";

std::string escapeText(std::string_view text);
Result<std::string> unescapeText(std::string_view field);  // Corruption si échappement invalide

std::string encodeValue(const Value& value);
Result<Value> decodeValue(std::string_view field, DataType type);  // Corruption

// Contenu complet de schema.txt.
std::string encodeSchema(const TableSchema& schema);
Result<TableSchema> decodeSchema(std::string_view tableName, std::string_view content);

// Une ligne de rows.txt, sans le `\n` final.
std::string encodeInsert(RowId id, const Row& row);
std::string encodeTombstone(RowId id);

struct Record {
    enum class Kind { Insert, Delete };
    Kind kind;
    RowId id;
    Row row;  // vide pour Delete
};
Result<Record> decodeRecord(std::string_view line, const TableSchema& schema);

}  // namespace ledger::codec
