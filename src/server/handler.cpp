#include "server/handler.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "server/json.h"

namespace ledger {

namespace {

// Errors the client can fix are 400; broken disk or broken invariants are 500.
int statusFor(ErrorCode code) {
    switch (code) {
        case ErrorCode::IoError:
        case ErrorCode::Corruption:
        case ErrorCode::Internal:
            return 500;
        default:
            return 400;
    }
}

void appendWarnings(std::string& body, const std::vector<std::string>& warnings) {
    if (warnings.empty()) return;
    body += R"(,"warnings":[)";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        if (i) body += ',';
        body += jsonString(warnings[i]);
    }
    body += ']';
}

// `results` is the already comma-joined JSON of the statements that ran.
HttpReply errorReply(int status, std::string_view code, std::string_view message, std::size_t line,
                     const std::string& results, const std::vector<std::string>& warnings) {
    std::string body = R"({"error":{"code":)" + jsonString(code) + R"(,"message":)" + jsonString(message);
    if (line) body += R"(,"line":)" + std::to_string(line);
    body += R"(},"results":[)" + results + ']';
    appendWarnings(body, warnings);
    body += '}';
    return {status, std::move(body)};
}

void rollbackIfOpen(Database& db) {
    if (!db.inTransaction()) return;
    // ROLLBACK on an open transaction cannot fail for a client-side reason;
    // an IoError here would leave nothing better to do than report the
    // original error anyway.
    const auto r = db.execute("ROLLBACK");
    (void)r.ok();
}

}  // namespace

HttpReply handleQuery(Database& db, std::string_view script) {
    const auto statements = splitStatements(script);
    if (statements.empty()) {
        return errorReply(400, "EmptyRequest", "no SQL statement in the request body", 0, "", {});
    }
    std::string results;
    std::vector<std::string> warnings;
    for (const auto& s : statements) {
        auto r = db.execute(s.sql);
        for (auto& w : db.takeWarnings()) warnings.push_back(std::move(w));
        if (!r.ok()) {
            rollbackIfOpen(db);
            return errorReply(statusFor(r.error().code), errorCodeName(r.error().code),
                              r.error().message, s.line, results, warnings);
        }
        if (!results.empty()) results += ',';
        results += toJson(r.value());
    }
    if (db.inTransaction()) {
        rollbackIfOpen(db);
        return errorReply(400, "TransactionOpen",
                          "transaction still open at the end of the request; rolled back "
                          "(send BEGIN ... COMMIT within a single request)",
                          0, results, warnings);
    }
    std::string body = R"({"results":[)" + results + ']';
    appendWarnings(body, warnings);
    body += '}';
    return {200, std::move(body)};
}

}  // namespace ledger
