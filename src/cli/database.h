#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "exec/executor.h"
#include "semantic/catalog.h"
#include "storage/file_engine.h"

namespace ledger {

// Assembles FileEngine + Catalog + Executor over a directory. This is the
// object the CLI manipulates; main.cpp only does input/output.
class Database {
public:
    // Opens (or creates) the database and loads the schemas into the catalog.
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path& dir);

    Result<QueryResult> execute(std::string_view sql) { return executor_.execute(sql); }

    [[nodiscard]] const Catalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] bool inTransaction() const noexcept { return executor_.inTransaction(); }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return engine_->directory(); }

    // Engine warnings (dropped truncated lines...), cleared on call.
    std::vector<std::string> takeWarnings() { return engine_->takeWarnings(); }

private:
    explicit Database(std::unique_ptr<FileEngine> engine)
        : engine_(std::move(engine)), executor_(*engine_, catalog_) {}

    std::unique_ptr<FileEngine> engine_;
    Catalog catalog_;
    Executor executor_;
};

// Where a database named on the command line lives.
//
//  - a bare name (`mydb`, no path separator) goes under the data root:
//    `<root>/mydb`, where <root> is the LEDGER_DATA_DIR environment variable
//    if set, otherwise `<defaultRoot>/data`. main.cpp passes the project
//    directory (the parent of the `build/` directory holding the executable)
//    so that `ledger mydb` opens the same database from any working
//    directory. This keeps every database in one place the repository
//    ignores;
//  - anything containing a path separator (`./here`, `C:\x\y`, `../db`) is
//    used as is.
//
// `envRoot` is the value of LEDGER_DATA_DIR (nullptr when unset).
std::filesystem::path resolveDatabasePath(std::string_view arg, const char* envRoot,
                                          const std::filesystem::path& defaultRoot = ".");

inline constexpr std::string_view kDefaultDataDir = "data";

// Splits a text into statements on the `;` characters that sit outside a
// '...' string and outside a `--` comment. The `;` is not kept. Empty
// statements (whitespace / comments only) are omitted. A trailing `;` is
// optional. A leading UTF-8 BOM is ignored.
//
// `sql` starts at the first useful character (leading whitespace and comments
// removed); `line` is its line (1-based) in the original text: a parser
// position `L:C` therefore maps to line `line + L - 1`.
struct ScriptStatement {
    std::string sql;
    std::size_t line;
};
std::vector<ScriptStatement> splitStatements(std::string_view text);

// True if the text ends with a complete statement (last `;` outside a string
// and outside a comment, followed only by whitespace / comments). Used by the
// REPL to know whether to keep reading.
[[nodiscard]] bool endsWithCompleteStatement(std::string_view text);

// How a result table is drawn.
//
//   plain : `+---+` ASCII borders, no colour — for pipes, files and tests.
//   fancy : rounded Unicode box drawing, ANSI colours (bold header, dim
//           NULLs, green/red booleans, right-aligned numbers) — for a
//           terminal that supports UTF-8 and VT sequences.
struct TableStyle {
    bool unicode = false;
    bool color = false;

    static constexpr TableStyle plain() noexcept { return {false, false}; }
    static constexpr TableStyle fancy() noexcept { return {true, true}; }
};

// ANSI escape sequences used by the fancy style, exposed so that main.cpp
// paints its prompt, errors and summaries with the same palette.
namespace ansi {
inline constexpr std::string_view reset = "\x1b[0m";
inline constexpr std::string_view bold = "\x1b[1m";
inline constexpr std::string_view dim = "\x1b[2m";
inline constexpr std::string_view italic = "\x1b[3m";
inline constexpr std::string_view red = "\x1b[31m";
inline constexpr std::string_view green = "\x1b[32m";
inline constexpr std::string_view yellow = "\x1b[33m";
inline constexpr std::string_view magenta = "\x1b[35m";
inline constexpr std::string_view cyan = "\x1b[36m";
inline constexpr std::string_view gray = "\x1b[90m";
}  // namespace ansi

// Aligned table of a SELECT result. Empty when there are no columns.
// Numbers are right-aligned, everything else left-aligned.
std::string formatTable(const QueryResult& result, TableStyle style = TableStyle::plain());

// Status line: "3 rows", "1 row affected", "ok".
std::string formatSummary(const QueryResult& result);

// The big LEDGER banner shown when the REPL starts. Block letters in the
// fancy style (with a colour gradient when colours are on), plain ASCII art
// otherwise. Always ends with a newline.
std::string formatLogo(TableStyle style);

}  // namespace ledger
