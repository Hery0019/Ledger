// ledger <dossier> : ouvre (ou crée) une base et exécute du SQL.
//
//  - stdin terminal  : REPL. Une instruction se termine par `;` (multi-lignes),
//                      commandes `.quit`, `.tables`, `.schema <table>`.
//  - stdin redirigé  : mode script. Tout stdin est lu, chaque instruction est
//                      exécutée ; arrêt à la première erreur (code 1).
//
// Codes de sortie : 0 ok, 1 erreur SQL / base, 2 erreur d'usage.

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
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

// Affiche une erreur. `line` est la ligne de début de l'instruction dans le
// script (0 = inconnue). Un message positionné `L:C: ...` (relatif à
// l'instruction) est réécrit en position absolue dans le script.
void printError(const Error& e, std::size_t line = 0) {
    std::cerr << "error [" << errorCodeName(e.code) << "]";
    std::string message = e.message;
    std::size_t l = 0, c = 0;
    char colon1 = 0, colon2 = 0;
    std::istringstream pos(message);
    if (line && (pos >> l >> colon1 >> c >> colon2) && colon1 == ':' && colon2 == ':' && l > 0) {
        std::string rest;
        std::getline(pos >> std::ws, rest);
        std::cerr << " at " << (line + l - 1) << ':' << c;
        message = rest;
    } else if (line) {
        std::cerr << " at line " << line;
    }
    std::cerr << ": " << message << '\n';
}

void printWarnings(Database& db) {
    for (const auto& w : db.takeWarnings()) std::cerr << "warning: " << w << '\n';
}

// Exécute une instruction et affiche le résultat. Renvoie false en cas d'erreur.
bool runOne(Database& db, std::string_view sql, std::size_t line) {
    auto r = db.execute(sql);
    if (!r.ok()) {
        printError(r.error(), line);
        return false;
    }
    std::cout << formatTable(r.value()) << formatSummary(r.value()) << '\n';
    printWarnings(db);
    return true;
}

// Commandes point du REPL. Renvoie true si `line` en était une (traitée).
bool dotCommand(Database& db, const std::string& line, bool& quit) {
    if (line.empty() || line[0] != '.') return false;
    std::istringstream in(line);
    std::string cmd, arg;
    in >> cmd >> arg;
    if (cmd == ".quit" || cmd == ".exit") {
        quit = true;
    } else if (cmd == ".tables") {
        for (const auto name : db.catalog().tableNames()) std::cout << name << '\n';
    } else if (cmd == ".schema") {
        const TableSchema* t = arg.empty() ? nullptr : db.catalog().find(arg);
        if (!t) {
            std::cerr << "error: unknown table '" << arg << "'\n";
        } else {
            std::cout << "CREATE TABLE " << t->name << " (";
            for (std::size_t i = 0; i < t->columns.size(); ++i) {
                const auto& c = t->columns[i];
                if (i) std::cout << ", ";
                std::cout << c.name << ' ' << dataTypeName(c.type);
                if (c.primaryKey) std::cout << " PRIMARY KEY";
                else if (c.notNull) std::cout << " NOT NULL";
            }
            std::cout << ");\n";
        }
    } else if (cmd == ".help") {
        std::cout << ".tables          liste les tables\n"
                     ".schema <table>  affiche la définition d'une table\n"
                     ".quit            quitte\n";
    } else {
        std::cerr << "error: unknown command '" << cmd << "' (try .help)\n";
    }
    return true;
}

int repl(Database& db) {
    std::cout << "ledger — base " << db.directory().string() << "\n"
              << "Terminez chaque instruction par ';'. .help pour les commandes.\n";
    std::string buffer;
    std::string line;
    bool quit = false;
    while (!quit) {
        std::cout << (buffer.empty() ? "ledger> " : "   ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && dotCommand(db, line, quit)) continue;
        buffer += line;
        buffer += '\n';
        if (!endsWithCompleteStatement(buffer)) continue;
        for (const auto& s : splitStatements(buffer)) runOne(db, s.sql, 0);
        buffer.clear();
    }
    std::cout << '\n';
    return 0;
}

int script(Database& db) {
    std::ostringstream all;
    all << std::cin.rdbuf();
    const std::string text = all.str();
    for (const auto& s : splitStatements(text)) {
        if (!runOne(db, s.sql, s.line)) return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ledger <database-directory>\n";
        return 2;
    }
    auto db = Database::open(argv[1]);
    if (!db.ok()) {
        printError(db.error());
        return 1;
    }
    printWarnings(*db.value());
    const bool interactive = LEDGER_ISATTY(LEDGER_FILENO(stdin)) != 0;
    return interactive ? repl(*db.value()) : script(*db.value());
}
