// ledger <name-or-directory>: opens (or creates) a database and runs SQL.
//
// A bare name is stored under the data root (`data/` by default, or
// LEDGER_DATA_DIR); a path is used as is. See resolveDatabasePath.
//
//  - stdin is a terminal : REPL. A statement ends with `;` (multi-line),
//                          commands `.quit`, `.tables`, `.views`, `.schema <name>`.
//  - stdin is redirected : script mode. All of stdin is read, every statement
//                          is run; stops at the first error (exit code 1).
//
// Output style: when stdout is a terminal, results are drawn with Unicode
// box characters and ANSI colours; when it is a pipe or a file, plain ASCII
// without escapes. NO_COLOR (https://no-color.org) forces plain output.
//
// Exit codes: 0 ok, 1 SQL / database error, 2 usage error.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define LEDGER_ISATTY(fd) _isatty(fd)
#define LEDGER_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define LEDGER_ISATTY(fd) isatty(fd)
#define LEDGER_FILENO(f) fileno(f)
#endif

#include "cli/database.h"

using namespace ledger;

namespace {

// Everything the output layer needs to know about the terminal.
struct Console {
    TableStyle style = TableStyle::plain();

    // Paints `text` with `code` when colours are on.
    [[nodiscard]] std::string paint(std::string_view code, std::string_view text) const {
        if (!style.color) return std::string(text);
        return std::string(code) + std::string(text) + std::string(ansi::reset);
    }
};

// Detects a colour-capable terminal on stdout and, on Windows, switches the
// console to UTF-8 output with VT escape processing.
//
// Overrides: NO_COLOR=1 forces plain output; LEDGER_STYLE=plain|fancy forces
// one style regardless of what stdout is (fancy into a pipe is useful for
// `less -R` or for capturing a demo).
Console detectConsole() {
    Console c;
    const char* forced = std::getenv("LEDGER_STYLE");
    const char* noColor = std::getenv("NO_COLOR");
    const bool tty = LEDGER_ISATTY(LEDGER_FILENO(stdout)) != 0;
    bool fancy = tty && !(noColor && *noColor);
    if (forced && std::string_view(forced) == "plain") fancy = false;
    if (forced && std::string_view(forced) == "fancy") fancy = true;
    if (!fancy) return c;
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        // A real console: it must interpret VT sequences and UTF-8, or the
        // output would be garbage. If that fails, fall back to plain.
        if (!SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return c;
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
    c.style = TableStyle::fancy();
    return c;
}

// Prints an error. `line` is the statement's starting line in the script
// (0 = unknown). A positioned message `L:C: ...` (relative to the statement)
// is rewritten as an absolute position in the script.
void printError(const Console& con, const Error& e, std::size_t line = 0) {
    std::string message = e.message;
    std::string where;
    std::size_t l = 0, c = 0;
    char colon1 = 0, colon2 = 0;
    std::istringstream pos(message);
    if (line && (pos >> l >> colon1 >> c >> colon2) && colon1 == ':' && colon2 == ':' && l > 0) {
        std::string rest;
        std::getline(pos >> std::ws, rest);
        where = " at " + std::to_string(line + l - 1) + ':' + std::to_string(c);
        message = rest;
    } else if (line) {
        where = " at line " + std::to_string(line);
    }
    std::cerr << con.paint(ansi::red, "error") << con.paint(ansi::dim, " [" + std::string(errorCodeName(e.code)) + "]")
              << where << ": " << message << '\n';
}

void printWarnings(const Console& con, Database& db) {
    for (const auto& w : db.takeWarnings()) {
        std::cerr << con.paint(ansi::yellow, "warning") << ": " << w << '\n';
    }
}

// Runs one statement and prints the result. Returns false on error.
bool runOne(const Console& con, Database& db, std::string_view sql, std::size_t line) {
    auto r = db.execute(sql);
    if (!r.ok()) {
        printError(con, r.error(), line);
        return false;
    }
    std::cout << formatTable(r.value(), con.style) << con.paint(ansi::dim, formatSummary(r.value()))
              << '\n';
    printWarnings(con, db);
    return true;
}

// REPL dot commands. Returns true if `line` was one (and was handled).
bool dotCommand(const Console& con, Database& db, const std::string& line, bool& quit) {
    if (line.empty() || line[0] != '.') return false;
    std::istringstream in(line);
    std::string cmd, arg;
    in >> cmd >> arg;
    if (cmd == ".quit" || cmd == ".exit") {
        quit = true;
    } else if (cmd == ".tables") {
        for (const auto name : db.catalog().tableNames()) std::cout << name << '\n';
    } else if (cmd == ".views") {
        for (const auto name : db.catalog().viewNames()) std::cout << name << '\n';
    } else if (cmd == ".schema") {
        const TableSchema* t = arg.empty() ? nullptr : db.catalog().find(arg);
        const ViewEntry* v = arg.empty() ? nullptr : db.catalog().findView(arg);
        if (v) {
            std::cout << con.paint(ansi::cyan, "CREATE VIEW") << ' ' << v->def.name << ' '
                      << con.paint(ansi::cyan, "AS") << ' ' << v->def.sql << ";\n";
        } else if (!t) {
            std::cerr << con.paint(ansi::red, "error") << ": unknown table or view '" << arg << "'\n";
        } else {
            std::cout << con.paint(ansi::cyan, "CREATE TABLE") << ' ' << t->name << " (";
            for (std::size_t i = 0; i < t->columns.size(); ++i) {
                const auto& c = t->columns[i];
                if (i) std::cout << ", ";
                std::cout << c.name << ' ' << con.paint(ansi::magenta, dataTypeName(c.type));
                if (c.primaryKey) std::cout << ' ' << con.paint(ansi::dim, "PRIMARY KEY");
                else if (c.notNull) std::cout << ' ' << con.paint(ansi::dim, "NOT NULL");
            }
            std::cout << ");\n";
        }
    } else if (cmd == ".help") {
        const auto item = [&](std::string_view name, std::string_view what) {
            std::cout << "  " << con.paint(ansi::cyan, name)
                      << std::string(name.size() < 16 ? 16 - name.size() : 1, ' ') << what << '\n';
        };
        item(".tables", "list tables");
        item(".views", "list views");
        item(".schema <name>", "show a table's or a view's definition");
        item(".quit", "exit");
    } else {
        std::cerr << con.paint(ansi::red, "error") << ": unknown command '" << cmd << "' (try .help)\n";
    }
    return true;
}

int repl(const Console& con, Database& db) {
    std::cout << '\n' << formatLogo(con.style) << '\n'
              << con.paint(ansi::bold, "  embedded SQL, plain-text storage")
              << con.paint(ansi::dim, "  ·  ") << db.directory().string() << '\n'
              << con.paint(ansi::dim, "  End each statement with ';'. Type .help for commands.") << "\n\n";
    const std::string prompt = con.paint(ansi::green, "ledger") + con.paint(ansi::dim, "> ");
    const std::string more = con.paint(ansi::dim, "   ...> ");
    std::string buffer;
    std::string line;
    bool quit = false;
    while (!quit) {
        std::cout << (buffer.empty() ? prompt : more) << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && dotCommand(con, db, line, quit)) continue;
        buffer += line;
        buffer += '\n';
        if (!endsWithCompleteStatement(buffer)) continue;
        for (const auto& s : splitStatements(buffer)) runOne(con, db, s.sql, 0);
        buffer.clear();
    }
    std::cout << '\n';
    return 0;
}

int script(const Console& con, Database& db) {
    std::ostringstream all;
    all << std::cin.rdbuf();
    const std::string text = all.str();
    for (const auto& s : splitStatements(text)) {
        if (!runOne(con, db, s.sql, s.line)) return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Console con = detectConsole();
    if (argc != 2) {
        std::cerr << "usage: ledger <database-name | database-directory>\n"
                     "  a bare name is stored under data/ (or $LEDGER_DATA_DIR)\n";
        return 2;
    }
    auto db = Database::open(resolveDatabasePath(argv[1], std::getenv("LEDGER_DATA_DIR")));
    if (!db.ok()) {
        printError(con, db.error());
        return 1;
    }
    printWarnings(con, *db.value());
    const bool interactive = LEDGER_ISATTY(LEDGER_FILENO(stdin)) != 0;
    return interactive ? repl(con, *db.value()) : script(con, *db.value());
}
