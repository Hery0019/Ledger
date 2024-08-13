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

// Aligned ASCII table of a SELECT result. Empty when there are no columns.
std::string formatTable(const QueryResult& result);

// Status line: "3 rows", "1 row affected", "ok".
std::string formatSummary(const QueryResult& result);

}  // namespace ledger
