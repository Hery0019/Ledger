#include "cli/database.h"

#include <algorithm>
#include <utility>

namespace ledger {

// ---- Database --------------------------------------------------------------

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& dir) {
    LEDGER_TRY(engine, FileEngine::open(dir));
    std::unique_ptr<Database> db(new Database(std::move(engine)));
    LEDGER_TRY(schemas, db->engine_->loadSchemas());
    for (auto& s : schemas) LEDGER_TRY_VOID(db->catalog_.add(std::move(s)));
    return db;
}

// ---- splitting -------------------------------------------------------------

namespace {

bool isBlank(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Walks `text`, reporting every significant `;` (outside strings, outside
// comments) through `onSemicolon(pos)`. Returns true if we end outside a
// string.
template <typename F>
bool scanSemicolons(std::string_view text, F onSemicolon) {
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') ++i;  // escaped ''
                else inString = false;
            }
        } else if (c == '\'') {
            inString = true;
        } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            const std::size_t nl = text.find('\n', i);
            if (nl == std::string_view::npos) break;
            i = nl;
        } else if (c == ';') {
            onSemicolon(i);
        }
    }
    return !inString;
}

// True if `s` contains only whitespace and comments.
bool isEmptyStatement(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (isBlank(s[i])) continue;
        if (s[i] == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const std::size_t nl = s.find('\n', i);
            if (nl == std::string_view::npos) return true;
            i = nl;
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

std::vector<ScriptStatement> splitStatements(std::string_view text) {
    std::vector<ScriptStatement> out;
    // Leading UTF-8 BOM: Notepad and PowerShell add one; it is not SQL.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t start = 0;

    // Adds text[start, end) if it holds a statement; its line is that of its
    // first useful character.
    auto emit = [&](std::size_t end) {
        const std::string_view piece = text.substr(start, end - start);
        if (!isEmptyStatement(piece)) {
            // Skip leading whitespace and comments: the reported line is the
            // SQL's own.
            std::size_t first = 0;
            for (;;) {
                while (first < piece.size() && isBlank(piece[first])) ++first;
                if (first + 1 < piece.size() && piece[first] == '-' && piece[first + 1] == '-') {
                    const std::size_t nl = piece.find('\n', first);
                    first = nl == std::string_view::npos ? piece.size() : nl + 1;
                    continue;
                }
                break;
            }
            const auto upto = text.begin() + static_cast<std::ptrdiff_t>(start + first);
            const auto line = 1 + static_cast<std::size_t>(std::count(text.begin(), upto, '\n'));
            // The stored SQL starts at the first useful character: the
            // parser's `line:col` positions are then relative to it.
            out.push_back(ScriptStatement{std::string(piece.substr(first)), line});
        }
        start = end + 1;
    };

    scanSemicolons(text, emit);
    if (start < text.size()) emit(text.size());  // last piece without `;`
    return out;
}

bool endsWithCompleteStatement(std::string_view text) {
    std::size_t lastSemicolon = std::string_view::npos;
    const bool closed = scanSemicolons(text, [&](std::size_t pos) { lastSemicolon = pos; });
    if (!closed || lastSemicolon == std::string_view::npos) return false;
    return isEmptyStatement(text.substr(lastSemicolon + 1));
}

// ---- display ---------------------------------------------------------------

namespace {

std::string cell(const Value& v) { return v.isNull() ? "NULL" : v.toText(); }

// Display width in UTF-8 code points, not bytes: "café" does not misalign.
std::size_t displayWidth(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char ch : s) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
    return n;
}

}  // namespace

std::string formatTable(const QueryResult& result) {
    if (result.columns.empty()) return {};
    const std::size_t ncols = result.columns.size();

    std::vector<std::vector<std::string>> cells;
    cells.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        std::vector<std::string> line;
        line.reserve(ncols);
        for (const auto& v : row) line.push_back(cell(v));
        cells.push_back(std::move(line));
    }

    std::vector<std::size_t> width(ncols);
    for (std::size_t c = 0; c < ncols; ++c) width[c] = displayWidth(result.columns[c]);
    for (const auto& line : cells) {
        for (std::size_t c = 0; c < ncols; ++c) width[c] = std::max(width[c], displayWidth(line[c]));
    }

    auto separator = [&] {
        std::string s = "+";
        for (std::size_t c = 0; c < ncols; ++c) s += std::string(width[c] + 2, '-') + "+";
        return s + "\n";
    };
    auto formatLine = [&](const std::vector<std::string>& line) {
        std::string s = "|";
        for (std::size_t c = 0; c < ncols; ++c) {
            s += ' ';
            s += line[c];
            s += std::string(width[c] - displayWidth(line[c]) + 1, ' ');
            s += '|';
        }
        return s + "\n";
    };

    std::string out = separator() + formatLine(result.columns) + separator();
    for (const auto& line : cells) out += formatLine(line);
    out += separator();
    return out;
}

std::string formatSummary(const QueryResult& result) {
    switch (result.kind) {
        case ResultKind::Select:
            return std::to_string(result.rows.size()) + (result.rows.size() == 1 ? " row" : " rows");
        case ResultKind::Dml:
            return std::to_string(result.affected) + (result.affected == 1 ? " row" : " rows") +
                   " affected";
        case ResultKind::Ddl:
            return "ok";
    }
    return {};
}

}  // namespace ledger
