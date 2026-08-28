#pragma once

// Route wiring shared by the ledgerd binary and the HTTP tests. This is the
// only server file that pulls in the vendored cpp-httplib; handler.h and
// json.h stay socket-free (and are tested without sockets).

#include <mutex>
#include <string>

#include "httplib.h"

#include "cli/database.h"
#include "server/handler.h"
#include "server/json.h"

namespace ledger {

// POST /query : body = SQL script, reply = handleQuery's JSON (see handler.h).
// GET  /health: {"ok":true,"database":"<dir>"} — liveness probe.
//
// `mtx` serializes the queries: httplib serves each connection from its own
// thread and Database::execute is not thread-safe. One request at a time is
// the whole concurrency model.
inline void attachRoutes(httplib::Server& server, Database& db, std::mutex& mtx) {
    server.Post("/query", [&db, &mtx](const httplib::Request& req, httplib::Response& res) {
        HttpReply reply;
        {
            const std::lock_guard<std::mutex> lock(mtx);
            reply = handleQuery(db, req.body);
        }
        res.status = reply.status;
        res.set_content(reply.body, "application/json");
    });
    // directory() is fixed at open: no lock needed.
    server.Get("/health", [&db](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"ok":true,"database":)" + jsonString(db.directory().string()) + '}',
                        "application/json");
    });
}

}  // namespace ledger
