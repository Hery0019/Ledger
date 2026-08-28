#pragma once

#include <string>
#include <string_view>

#include "core/value.h"
#include "exec/executor.h"

namespace ledger {

// Minimal JSON writers for the HTTP server. Only encoding is needed: requests
// carry raw SQL, so the server never parses JSON.

// Renders `text` as a JSON string literal, surrounding quotes included.
// `"` `\` and control characters are escaped per RFC 8259; other bytes are
// copied as is (TEXT values are stored as bytes and assumed to be UTF-8).
std::string jsonString(std::string_view text);

// NULL -> null, INT/FLOAT -> number (shortest round-trip form, which is
// always a valid JSON number since Value rejects NaN and infinities),
// BOOL -> true/false, TEXT -> string.
std::string toJson(const Value& value);

// SELECT -> {"kind":"select","columns":[...],"rows":[[...],...]}
// DML    -> {"kind":"dml","affected":n}
// DDL    -> {"kind":"ddl"}
std::string toJson(const QueryResult& result);

}  // namespace ledger
