#pragma once

#include <string>
#include <string_view>

#include "cli/database.h"

namespace ledger {

// One HTTP response, ready to send: a status code and a JSON body.
struct HttpReply {
    int status = 200;
    std::string body;
};

// Runs a request body (one or more ';'-separated SQL statements) against the
// database and builds the JSON reply. This is the whole server logic; the
// binary only adds the socket, the routes and a mutex (Database::execute must
// not be called from two threads at once).
//
// A request is a session. Statements run in order and stop at the first
// error; statements executed before the error have already been applied
// (writes are immediate outside a transaction), so the error reply carries
// their results too. A transaction must begin and end within a single
// request: if one is still open after the last statement — or when a
// statement fails mid-transaction — it is rolled back, exactly like a REPL
// session closing mid-transaction.
//
//   200  {"results":[...], "warnings":[...]}      warnings only when present
//   400  {"error":{"code","message","line"}, "results":[...], "warnings":[...]}
//   500  same shape, for IoError / Corruption / Internal
//
// Each entry of "results" is toJson(QueryResult); "line" (absent when
// unknown) is the failing statement's first line in the request body,
// 1-based. "code" is an errorCodeName, or one of the server's own:
// "EmptyRequest" (no statement in the body), "TransactionOpen" (transaction
// left open at the end of the request).
HttpReply handleQuery(Database& db, std::string_view script);

}  // namespace ledger
