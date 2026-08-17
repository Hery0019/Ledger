#include "cli/database.h"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "sql/parser.h"

namespace ledger {

// ---- Database --------------------------------------------------------------

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& dir) {
    LEDGER_TRY(engine, FileEngine::open(dir));
    std::unique_ptr<Database> db(new Database(std::move(engine)));
    LEDGER_TRY(schemas, db->engine_->loadSchemas());
    for (auto& s : schemas) LEDGER_TRY_VOID(db->catalog_.add(std::move(s)));
    // Views are stored in creation order, so each one's source already exists
    // when it is registered. A definition that no longer parses is a
    // corrupted views file, not a user error.
    LEDGER_TRY(views, db->engine_->loadViews());
    for (auto& v : views) {
        auto parsed = parse(v.sql);
        const auto* query = parsed.ok() ? std::get_if<ast::Select>(&parsed.value()) : nullptr;
        if (!query) {
            return makeError(ErrorCode::Corruption,
                             "views.txt: view '" + v.name + "': " +
                                 (parsed.ok() ? std::string("definition is not a SELECT")
                                              : parsed.error().message));
        }
        std::vector<std::string> sources{query->from.name};
        for (const auto& j : query->joins) sources.push_back(j.table.name);
        LEDGER_TRY_VOID(db->catalog_.addView(std::move(v), std::move(sources)));
    }
    LEDGER_TRY(users, db->engine_->loadUsers());
    for (auto& u : users) LEDGER_TRY_VOID(db->catalog_.addUser(std::move(u)));
    LEDGER_TRY(indexes, db->engine_->loadIndexes());
    for (auto& i : indexes) LEDGER_TRY_VOID(db->catalog_.addIndex(std::move(i)));
    return db;
}

std::filesystem::path resolveDatabasePath(std::string_view arg, const char* envRoot,
                                          const std::filesystem::path& defaultRoot) {
    const bool bareName = arg.find('/') == std::string_view::npos &&
                          arg.find('\\') == std::string_view::npos;
    if (!bareName) return std::filesystem::path(arg);
    const std::filesystem::path root = (envRoot && *envRoot)
                                           ? std::filesystem::path(envRoot)
                                           : defaultRoot / std::filesystem::path(kDefaultDataDir);
    return root / std::filesystem::path(arg);
}

std::filesystem::path projectRoot() {
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    exe = std::filesystem::path(buf);
#else
    std::error_code ec;
    exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return ".";
#endif
    const std::filesystem::path dir = exe.parent_path();
    const std::string name = dir.filename().string();
    if (name.rfind("build", 0) == 0) return dir.parent_path();
    return dir;
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

// Display width in UTF-8 code points, not bytes: "café" does not misalign.
std::size_t displayWidth(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char ch : s) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
    return n;
}

// A cell ready to draw: its text, whether it is a number and which colour to
// use. Alignment is decided per column (see formatTable), so that a NULL in
// a numeric column lines up with the numbers around it.
struct Cell {
    std::string text;
    bool isNull;
    bool numeric;
    std::string_view color;  // empty = default colour
};

Cell cellOf(const Value& v) {
    switch (v.type()) {
        case DataType::Null:  return {"NULL", true, false, ansi::dim};
        case DataType::Int:
        case DataType::Float: return {v.toText(), false, true, ansi::yellow};
        case DataType::Bool:  return {v.toText(), false, false, v.asBool() ? ansi::green : ansi::red};
        case DataType::Text:  return {v.toText(), false, false, {}};
        case DataType::Uuid:  return {v.toText(), false, false, ansi::cyan};
    }
    return {v.toText(), false, false, {}};
}

// The characters a border is drawn with.
struct BoxChars {
    std::string_view h, v;                 // horizontal, vertical
    std::string_view tl, tm, tr;           // top row: left corner, joint, right corner
    std::string_view ml, mm, mr;           // header separator
    std::string_view bl, bm, br;           // bottom row
};

constexpr BoxChars kAsciiBox{"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
constexpr BoxChars kUnicodeBox{"─", "│", "╭", "┬", "╮", "├", "┼", "┤", "╰", "┴", "╯"};

std::string repeat(std::string_view s, std::size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i) out += s;
    return out;
}

}  // namespace

std::string formatTable(const QueryResult& result, TableStyle style) {
    if (result.columns.empty()) return {};
    const std::size_t ncols = result.columns.size();
    const BoxChars& box = style.unicode ? kUnicodeBox : kAsciiBox;

    // Colour helpers: no-ops in plain style, so widths never include escapes.
    auto paint = [&](std::string_view code, std::string_view text) {
        std::string s;
        if (style.color && !code.empty()) s += code;
        s += text;
        if (style.color && !code.empty()) s += ansi::reset;
        return s;
    };
    auto border = [&](std::string_view text) { return paint(ansi::gray, text); };

    std::vector<std::vector<Cell>> cells;
    cells.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        std::vector<Cell> line;
        line.reserve(ncols);
        for (const auto& v : row) line.push_back(cellOf(v));
        cells.push_back(std::move(line));
    }

    // Column widths, and which columns are numeric (every non-NULL value is a
    // number): those are right-aligned, NULLs included.
    std::vector<std::size_t> width(ncols);
    std::vector<bool> rightAlign(ncols, false);
    for (std::size_t c = 0; c < ncols; ++c) {
        width[c] = displayWidth(result.columns[c]);
        bool sawNumber = false, sawOther = false;
        for (const auto& line : cells) {
            width[c] = std::max(width[c], displayWidth(line[c].text));
            if (line[c].numeric) sawNumber = true;
            else if (!line[c].isNull) sawOther = true;
        }
        rightAlign[c] = sawNumber && !sawOther;
    }

    auto rule = [&](std::string_view left, std::string_view joint, std::string_view right) {
        std::string s(left);
        for (std::size_t c = 0; c < ncols; ++c) {
            s += repeat(box.h, width[c] + 2);
            s += c + 1 < ncols ? joint : right;
        }
        return border(s) + "\n";
    };

    auto drawCell = [&](const Cell& cell, std::size_t w, bool right) {
        const std::string pad(w - displayWidth(cell.text), ' ');
        std::string s = " ";
        if (right) s += pad;
        s += paint(cell.color, cell.text);
        if (!right) s += pad;
        return s + " ";
    };

    std::string out = rule(box.tl, box.tm, box.tr);

    // Header: bold, coloured, always left-aligned.
    out += border(box.v);
    for (std::size_t c = 0; c < ncols; ++c) {
        const std::string pad(width[c] - displayWidth(result.columns[c]), ' ');
        out += " ";
        if (style.color) out += std::string(ansi::bold) + std::string(ansi::cyan);
        out += result.columns[c];
        if (style.color) out += ansi::reset;
        out += pad + " " + border(box.v);
    }
    out += "\n";
    out += rule(box.ml, box.mm, box.mr);

    for (const auto& line : cells) {
        out += border(box.v);
        for (std::size_t c = 0; c < ncols; ++c) {
            out += drawCell(line[c], width[c], rightAlign[c]) + border(box.v);
        }
        out += "\n";
    }
    out += rule(box.bl, box.bm, box.br);
    return out;
}

std::string formatLogo(TableStyle style) {
    // "ANSI Shadow" block letters: solid glyphs with a shadow drawn in
    // double-line box characters.
    static constexpr std::string_view kBlock[] = {
        "██╗     ███████╗██████╗  ██████╗ ███████╗██████╗ ",
        "██║     ██╔════╝██╔══██╗██╔════╝ ██╔════╝██╔══██╗",
        "██║     █████╗  ██║  ██║██║  ███╗█████╗  ██████╔╝",
        "██║     ██╔══╝  ██║  ██║██║   ██║██╔══╝  ██╔══██╗",
        "███████╗███████╗██████╔╝╚██████╔╝███████╗██║  ██║",
        "╚══════╝╚══════╝╚═════╝  ╚═════╝ ╚══════╝╚═╝  ╚═╝",
    };
    static constexpr std::string_view kAscii[] = {
        " _     _____ ____   ____ _____ ____  ",
        "| |   | ____|  _ \\ / ___| ____|  _ \\ ",
        "| |   |  _| | | | | |  _|  _| | |_) |",
        "| |___| |__| |_| | |_| | |___|  _ < ",
        "|_____|_____|____/ \\____|_____|_| \\_\\",
    };
    // Top-to-bottom gradient, 256-colour palette: bright cyan down to deep blue.
    static constexpr std::string_view kGradient[] = {
        "\x1b[38;5;51m", "\x1b[38;5;45m", "\x1b[38;5;39m",
        "\x1b[38;5;33m", "\x1b[38;5;27m", "\x1b[38;5;21m",
    };

    std::string out;
    if (style.unicode) {
        for (std::size_t i = 0; i < std::size(kBlock); ++i) {
            if (style.color) out += kGradient[i];
            out += kBlock[i];
            if (style.color) out += ansi::reset;
            out += '\n';
        }
    } else {
        for (const auto line : kAscii) {
            out += line;
            out += '\n';
        }
    }
    return out;
}

std::string formatSummary(const QueryResult& result) {
    switch (result.kind) {
        case ResultKind::Select:
            return std::to_string(result.rows.size()) + (result.rows.size() == 1 ? " row" : " rows");
        case ResultKind::Dml: {
            std::string s = std::to_string(result.affected) + (result.affected == 1 ? " row" : " rows") +
                            " affected";
            if (result.key) s += " (key " + result.key->toText() + ")";
            return s;
        }
        case ResultKind::Ddl:
            return "ok";
    }
    return {};
}

}  // namespace ledger
