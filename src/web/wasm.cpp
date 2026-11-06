// WebAssembly entry point for the browser demo (web/). The page loads the
// Emscripten module and calls these C functions; everything runs in the
// visitor's tab, on an in-memory database, so nothing is sent or stored.
//
// A call to ledger_query is one session, exactly like a POST /query to
// ledgerd: same splitting, same JSON reply, same rollback of a transaction
// left open. Tables persist between calls until ledger_reset.

#include <memory>
#include <string>

#include "cli/database.h"
#include "server/handler.h"

namespace {

std::unique_ptr<ledger::Database>& database() {
    static std::unique_ptr<ledger::Database> db = ledger::Database::openInMemory();
    return db;
}

// Kept alive between calls: the returned pointer stays valid until the next
// ledger_query.
ledger::HttpReply lastReply;

}  // namespace

extern "C" {

// Runs `sql` (one or more ';'-separated statements) and returns the JSON body
// documented in server/handler.h.
const char* ledger_query(const char* sql) {
    lastReply = ledger::handleQuery(*database(), sql ? sql : "");
    return lastReply.body.c_str();
}

// HTTP-like status of the last ledger_query: 200, 400 or 500.
int ledger_status() { return lastReply.status; }

// Drops every table, view, user and index.
void ledger_reset() { database() = ledger::Database::openInMemory(); }

}  // extern "C"
