#include "storage/file_engine.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#define LEDGER_GETPID _getpid
#else
#include <unistd.h>
#define LEDGER_GETPID getpid
#endif

#include "storage/codec.h"

namespace ledger {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kLockFile = "LOCK";
constexpr std::string_view kSchemaFile = "schema.txt";
constexpr std::string_view kRowsFile = "rows.txt";

Error ioError(const std::string& what, const fs::path& path) {
    return makeError(ErrorCode::IoError, what + ": " + path.string() + " (" + std::strerror(errno) + ")");
}

Error ioError(const std::string& what, const fs::path& path, const std::error_code& ec) {
    return makeError(ErrorCode::IoError, what + ": " + path.string() + " (" + ec.message() + ")");
}

Error notFound(std::string_view table) {
    return makeError(ErrorCode::NotFound, "unknown table '" + std::string(table) + "'");
}

Result<std::string> readWholeFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ioError("cannot read", path);
    std::ostringstream buf;
    buf << in.rdbuf();
    if (in.bad()) return ioError("read failed", path);
    return buf.str();
}

Result<void> writeWholeFile(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return ioError("cannot write", path);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) return ioError("write failed", path);
    return {};
}

// Noms de périphériques réservés par Windows : un dossier `con` ou `nul` est
// impossible à créer (ou pire, à supprimer). Refusés sur toutes les plateformes
// pour que les bases restent portables.
bool isReservedName(std::string_view name) {
    static constexpr std::array<std::string_view, 4> kBase{"con", "prn", "aux", "nul"};
    for (const auto r : kBase) {
        if (name == r) return true;
    }
    if (name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
        name[3] >= '1' && name[3] <= '9') {
        return true;
    }
    return false;
}

}  // namespace

// ---- ouverture / fermeture -------------------------------------------------

Result<std::unique_ptr<FileEngine>> FileEngine::open(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return ioError("cannot create database directory", dir, ec);

    const fs::path lock = dir / kLockFile;
    if (fs::exists(lock, ec)) {
        auto owner = readWholeFile(lock);
        return makeError(ErrorCode::IoError,
                         "database is locked: " + dir.string() + " (held by pid " +
                             (owner.ok() ? owner.value() : std::string("?")) + ")");
    }
    LEDGER_TRY_VOID(writeWholeFile(lock, std::to_string(LEDGER_GETPID())));

    std::unique_ptr<FileEngine> engine(new FileEngine(dir));
    LEDGER_TRY_VOID(engine->loadSchemas());
    return engine;
}

FileEngine::~FileEngine() {
    for (auto& [name, t] : tables_) closeFile(t);
    std::error_code ec;
    fs::remove(dir_ / kLockFile, ec);
}

void FileEngine::closeFile(Table& t) noexcept {
    if (t.out) {
        std::fclose(t.out);
        t.out = nullptr;
    }
}

fs::path FileEngine::tableDir(std::string_view table) const { return dir_ / fs::path(table); }

std::vector<std::string> FileEngine::takeWarnings() {
    const std::lock_guard<std::mutex> lock(mu_);
    return std::exchange(warnings_, {});
}

// ---- schémas ---------------------------------------------------------------

Result<std::vector<TableSchema>> FileEngine::loadSchemas() {
    const std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (tables_.contains(name)) continue;
        const fs::path schemaPath = entry.path() / kSchemaFile;
        if (!fs::exists(schemaPath)) continue;  // dossier étranger, ignoré
        LEDGER_TRY(content, readWholeFile(schemaPath));
        auto schema = codec::decodeSchema(name, content);
        if (!schema.ok()) {
            return makeError(ErrorCode::Corruption,
                             schemaPath.string() + ": " + schema.error().message);
        }
        Table t;
        t.schema = std::move(schema).value();
        tables_.emplace(name, std::move(t));
    }
    if (ec) return ioError("cannot list database directory", dir_, ec);

    std::vector<TableSchema> out;
    out.reserve(tables_.size());
    for (const auto& [name, t] : tables_) out.push_back(t.schema);
    return out;
}

Result<void> FileEngine::createTable(const TableSchema& schema) {
    const std::lock_guard<std::mutex> lock(mu_);
    if (tables_.contains(schema.name)) {
        return makeError(ErrorCode::AlreadyExists, "table '" + schema.name + "' already exists");
    }
    if (isReservedName(schema.name)) {
        return makeError(ErrorCode::IoError,
                         "'" + schema.name + "' is a reserved file name and cannot be a table name");
    }
    const fs::path dir = tableDir(schema.name);
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        return makeError(ErrorCode::AlreadyExists,
                         "directory already exists for table '" + schema.name + "': " + dir.string());
    }
    fs::create_directory(dir, ec);
    if (ec) return ioError("cannot create table directory", dir, ec);
    LEDGER_TRY_VOID(writeWholeFile(dir / kSchemaFile, codec::encodeSchema(schema)));
    LEDGER_TRY_VOID(writeWholeFile(dir / kRowsFile, std::string(codec::kRowsHeader) + "\n"));

    Table t;
    t.schema = schema;
    t.loaded = true;  // table neuve : rien à rejouer
    tables_.emplace(schema.name, std::move(t));
    return {};
}

Result<void> FileEngine::dropTable(std::string_view table) {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = tables_.find(table);
    if (it == tables_.end()) return notFound(table);
    closeFile(it->second);
    std::error_code ec;
    fs::remove_all(tableDir(table), ec);
    if (ec) return ioError("cannot remove table directory", tableDir(table), ec);
    tables_.erase(it);
    return {};
}

// ---- chargement des lignes -------------------------------------------------

Result<FileEngine::Table*> FileEngine::loaded(std::string_view table) {
    const auto it = tables_.find(table);
    if (it == tables_.end()) return notFound(table);
    Table& t = it->second;
    if (!t.loaded) {
        LEDGER_TRY_VOID(loadRows(t));
        t.loaded = true;
    }
    if (!t.out) {
        const fs::path path = tableDir(table) / kRowsFile;
        t.out = std::fopen(path.string().c_str(), "ab");
        if (!t.out) return ioError("cannot open for append", path);
    }
    return &t;
}

Result<void> FileEngine::loadRows(Table& t) {
    const fs::path path = tableDir(t.schema.name) / kRowsFile;
    LEDGER_TRY(content, readWholeFile(path));

    t.rows.clear();
    t.nextId = 1;
    t.tombstones = 0;

    // Dernière ligne sans '\n' = append interrompu : ignorée, signalée.
    std::string_view rest = content;
    if (!rest.empty() && rest.back() != '\n') {
        const std::size_t cut = rest.rfind('\n');
        const std::string_view partial = cut == std::string_view::npos ? rest : rest.substr(cut + 1);
        warnings_.push_back(path.string() + ": ignoring truncated last line '" +
                            std::string(partial.substr(0, 40)) + "'");
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(0, cut + 1);
        // On retire physiquement le fragment : sinon le prochain append se
        // collerait à sa suite et corromprait le fichier pour de bon.
        std::error_code ec;
        fs::resize_file(path, rest.size(), ec);
        if (ec) return ioError("cannot truncate partial line", path, ec);
    }

    std::size_t lineNo = 0;
    bool headerSeen = false;
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = rest.substr(nl + 1);
        ++lineNo;
        const std::string where = path.string() + ":" + std::to_string(lineNo) + ": ";

        if (!headerSeen) {
            if (line != codec::kRowsHeader) {
                return makeError(ErrorCode::Corruption, where + "missing or unknown header (expected '" +
                                                            std::string(codec::kRowsHeader) + "')");
            }
            headerSeen = true;
            continue;
        }
        if (line.empty()) continue;

        auto rec = codec::decodeRecord(line, t.schema);
        if (!rec.ok()) return makeError(ErrorCode::Corruption, where + rec.error().message);
        auto& r = rec.value();
        if (r.kind == codec::Record::Kind::Delete) {
            if (t.rows.erase(r.id) == 0) {
                return makeError(ErrorCode::Corruption,
                                 where + "tombstone for unknown row " + std::to_string(r.id));
            }
            ++t.tombstones;
        } else {
            // Une réinsertion du même id est la version post-update : le
            // tombstone qui la précède a déjà retiré l'ancienne.
            if (t.rows.contains(r.id)) {
                return makeError(ErrorCode::Corruption,
                                 where + "duplicate live row " + std::to_string(r.id));
            }
            t.rows.emplace(r.id, std::move(r.row));
            if (r.id >= t.nextId) t.nextId = r.id + 1;
        }
    }
    if (!headerSeen) {
        return makeError(ErrorCode::Corruption, path.string() + ": empty file (missing header)");
    }
    return {};
}

// ---- écriture --------------------------------------------------------------

Result<void> FileEngine::appendLine(Table& t, const std::string& line) {
    const fs::path path = tableDir(t.schema.name) / kRowsFile;
    if (std::fputs(line.c_str(), t.out) == EOF || std::fputc('\n', t.out) == EOF ||
        std::fflush(t.out) != 0) {
        return ioError("append failed", path);
    }
    return {};
}

Result<RowId> FileEngine::insert(std::string_view table, const Row& row) {
    const std::lock_guard<std::mutex> lock(mu_);
    LEDGER_TRY(t, loaded(table));
    if (row.size() != t->schema.columns.size()) {
        return makeError(ErrorCode::Internal, "insert: row has " + std::to_string(row.size()) +
                                                  " values, table '" + t->schema.name + "' has " +
                                                  std::to_string(t->schema.columns.size()) + " columns");
    }
    const RowId id = t->nextId;
    LEDGER_TRY_VOID(appendLine(*t, codec::encodeInsert(id, row)));
    t->rows.emplace(id, row);
    t->nextId = id + 1;
    return id;
}

Result<void> FileEngine::scan(std::string_view table,
                              const std::function<bool(RowId, const Row&)>& visit) {
    const std::lock_guard<std::mutex> lock(mu_);
    LEDGER_TRY(t, loaded(table));
    for (const auto& [id, row] : t->rows) {
        if (!visit(id, row)) break;
    }
    return {};
}

Result<void> FileEngine::update(std::string_view table, RowId id, const Row& row) {
    const std::lock_guard<std::mutex> lock(mu_);
    LEDGER_TRY(t, loaded(table));
    const auto it = t->rows.find(id);
    if (it == t->rows.end()) {
        return makeError(ErrorCode::NotFound, "row " + std::to_string(id) + " not found");
    }
    if (row.size() != t->schema.columns.size()) {
        return makeError(ErrorCode::Internal, "update: wrong number of values");
    }
    // Tombstone puis nouvelle version avec le même rowid.
    LEDGER_TRY_VOID(appendLine(*t, codec::encodeTombstone(id)));
    LEDGER_TRY_VOID(appendLine(*t, codec::encodeInsert(id, row)));
    it->second = row;
    ++t->tombstones;
    return maybeCompact(*t);
}

Result<void> FileEngine::remove(std::string_view table, RowId id) {
    const std::lock_guard<std::mutex> lock(mu_);
    LEDGER_TRY(t, loaded(table));
    const auto it = t->rows.find(id);
    if (it == t->rows.end()) {
        return makeError(ErrorCode::NotFound, "row " + std::to_string(id) + " not found");
    }
    LEDGER_TRY_VOID(appendLine(*t, codec::encodeTombstone(id)));
    t->rows.erase(it);
    ++t->tombstones;
    return maybeCompact(*t);
}

// ---- compaction ------------------------------------------------------------

Result<void> FileEngine::maybeCompact(Table& t) {
    if (t.tombstones > kCompactMinTombstones && t.tombstones > t.rows.size()) return rewrite(t);
    return {};
}

Result<void> FileEngine::compact(std::string_view table) {
    const std::lock_guard<std::mutex> lock(mu_);
    LEDGER_TRY(t, loaded(table));
    return rewrite(*t);
}

Result<void> FileEngine::rewrite(Table& t) {
    const fs::path path = tableDir(t.schema.name) / kRowsFile;
    const fs::path tmp = tableDir(t.schema.name) / "rows.txt.tmp";

    std::string content(codec::kRowsHeader);
    content += '\n';
    for (const auto& [id, row] : t.rows) {
        content += codec::encodeInsert(id, row);
        content += '\n';
    }
    LEDGER_TRY_VOID(writeWholeFile(tmp, content));

    // Le handle d'append pointe sur l'ancien fichier : on le ferme avant le
    // rename (obligatoire sous Windows), on le rouvrira à la demande.
    closeFile(t);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) return ioError("cannot replace rows file", path, ec);
    t.tombstones = 0;

    t.out = std::fopen(path.string().c_str(), "ab");
    if (!t.out) return ioError("cannot reopen for append", path);
    return {};
}

}  // namespace ledger
