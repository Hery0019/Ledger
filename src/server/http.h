#pragma once

// Route wiring shared by the ledgerd binary and the HTTP tests. This is the
// only server file that pulls in the vendored cpp-httplib; handler.h, json.h
// and auth.h stay socket-free (and are tested without sockets).

#include <mutex>
#include <string>

#include "httplib.h"

#include "cli/database.h"
#include "server/auth.h"
#include "server/handler.h"
#include "server/json.h"

namespace ledger {

// POST /query : body = SQL script, reply = handleQuery's JSON (see handler.h).
// GET  /health: {"ok":true,"database":"<dir>"} — liveness probe.
//
// Authentication, postgres-style: as soon as the database has user accounts
// (CREATE USER), every endpoint requires HTTP Basic credentials matching one
// of them; without any account the database is open. 401 carries a
// WWW-Authenticate challenge so browsers and curl -u behave.
//
// `mtx` serializes everything: httplib serves each connection from its own
// thread, and neither Database::execute nor the catalog reads are
// thread-safe. One request at a time is the whole concurrency model.
inline void attachRoutes(httplib::Server& server, Database& db, std::mutex& mtx, AuthCache& cache) {
    const auto guard = [&db, &mtx, &cache](const httplib::Request& req, httplib::Response& res,
                                           const auto& serve) {
        const std::lock_guard<std::mutex> lock(mtx);
        if (!authorize(db.catalog(), cache, req.get_header_value("Authorization"))) {
            res.status = 401;
            res.set_header("WWW-Authenticate", R"(Basic realm="ledger")");
            res.set_content(
                R"j({"error":{"code":"Unauthorized","message":"user name and password required (HTTP Basic)"}})j",
                "application/json");
            return;
        }
        serve(res);
    };
    server.Post("/query", [&db, guard](const httplib::Request& req, httplib::Response& res) {
        guard(req, res, [&db, &req](httplib::Response& out) {
            const HttpReply reply = handleQuery(db, req.body);
            out.status = reply.status;
            out.set_content(reply.body, "application/json");
        });
    });
    server.Get("/health", [&db, guard](const httplib::Request& req, httplib::Response& res) {
        guard(req, res, [&db](httplib::Response& out) {
            out.set_content(R"({"ok":true,"database":)" + jsonString(db.directory().string()) + '}',
                            "application/json");
        });
    });
}

}  // namespace ledger
