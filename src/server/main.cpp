// ledgerd <name-or-directory> [--host <addr>] [--port <n>]: serves a database
// over HTTP, so that applications in any language can use it without linking
// the engine — they POST SQL to /query and read JSON back.
//
// The ledgerd process is the single owner of the database (the LOCK file
// keeps everyone else out); clients never touch the files. Requests are
// served one at a time. A transaction must begin and end within a single
// request (see handler.h).
//
// Authentication: a database with user accounts (CREATE USER name PASSWORD
// '...') requires HTTP Basic credentials matching one of them on every
// request; a database without accounts is open. Passwords travel in clear
// (plain HTTP): keep it on localhost, or put TLS in front.
//
// Exit codes: 0 ok (SIGINT/SIGTERM shuts down cleanly), 1 database or bind
// error, 2 usage error.

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "server/http.h"

using namespace ledger;

namespace {

httplib::Server* runningServer = nullptr;

void stopServer(int) {
    if (runningServer) runningServer->stop();
}

int usage() {
    std::cerr << "usage: ledgerd <database-name | database-directory> [--host <addr>] [--port <n>]\n"
                 "  a bare name is stored under data/ (or $LEDGER_DATA_DIR)\n"
                 "  defaults: --host 127.0.0.1, --port 5433\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dbArg;
    std::string host = "127.0.0.1";
    int port = 5433;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            char* end = nullptr;
            const long n = std::strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || n < 1 || n > 65535) return usage();
            port = static_cast<int>(n);
        } else if (dbArg.empty() && !arg.empty() && arg[0] != '-') {
            dbArg = arg;
        } else {
            return usage();
        }
    }
    if (dbArg.empty()) return usage();

    auto db = Database::open(resolveDatabasePath(dbArg, std::getenv("LEDGER_DATA_DIR"), projectRoot()));
    if (!db.ok()) {
        std::cerr << "error [" << errorCodeName(db.error().code) << "]: " << db.error().message << '\n';
        return 1;
    }
    for (const auto& w : db.value()->takeWarnings()) std::cerr << "warning: " << w << '\n';

    httplib::Server server;
    std::mutex mtx;
    AuthCache cache;
    attachRoutes(server, *db.value(), mtx, cache);
    runningServer = &server;
    std::signal(SIGINT, stopServer);
    std::signal(SIGTERM, stopServer);

    const std::size_t accounts = db.value()->catalog().userNames().size();
    std::cerr << "ledgerd: serving " << db.value()->directory().string() << " on http://" << host
              << ':' << port << " (POST /query, GET /health)\n";
    if (accounts) {
        std::cerr << "ledgerd: " << accounts << " user account(s), HTTP Basic authentication required\n";
    } else {
        std::cerr << "ledgerd: no user accounts - open access"
                  << " (CREATE USER name PASSWORD '...' to require authentication)\n";
    }
    if (!server.listen(host, port)) {
        std::cerr << "error [IoError]: cannot listen on " << host << ':' << port << '\n';
        return 1;
    }
    return 0;
}
