#pragma once

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/engine.h"
#include "storage/pk_index.h"

namespace ledger {

// Text-file engine: one directory per table under the database directory.
//
//   <base>/LOCK               present while a process has the database open
//   <base>/views.txt          `ledger-views 1`, then one `name<TAB>escaped sql` per view
//   <base>/<table>/schema.txt
//   <base>/<table>/rows.txt   append-only, see storage/codec.h
//
// Model: on first access, rows.txt is fully replayed into memory (map rowid
// -> Row); afterwards every write is appended to the file (flush, no fsync:
// we guard against a program crash, not a power failure) and applied in
// memory. When tombstones exceed a threshold, the file is rewritten without
// them (compaction, via temporary file + rename).
//
// A truncated last line (crash during an append) is dropped and reported in
// warnings(); any other anomaly is a Corruption.
//
// Thread-safe (one mutex for the whole database); single process (LOCK file).
class FileEngine final : public IStorageEngine {
public:
    // Creates the directory if needed, takes the lock. IoError if the database
    // is already open in another process.
    static Result<std::unique_ptr<FileEngine>> open(const std::filesystem::path& dir);
    ~FileEngine() override;

    FileEngine(const FileEngine&) = delete;
    FileEngine& operator=(const FileEngine&) = delete;

    Result<void> createTable(const TableSchema& schema) override;
    Result<void> dropTable(std::string_view table) override;
    Result<RowId> insert(std::string_view table, const Row& row) override;
    Result<void> scan(std::string_view table,
                      const std::function<bool(RowId, const Row&)>& visit) override;
    Result<void> update(std::string_view table, RowId id, const Row& row) override;
    Result<void> remove(std::string_view table, RowId id) override;
    Result<void> restore(std::string_view table, RowId id, const Row& row) override;
    [[nodiscard]] bool indexed(std::string_view table, std::size_t column) const noexcept override;
    Result<std::optional<std::pair<RowId, Row>>> lookup(std::string_view table, std::size_t column,
                                                        const Value& key) override;
    Result<std::optional<Value>> maxKey(std::string_view table, std::size_t column) override;
    Result<void> compact(std::string_view table) override;
    Result<std::vector<TableSchema>> loadSchemas() override;
    Result<void> saveViews(const std::vector<ViewDef>& views) override;
    Result<std::vector<ViewDef>> loadViews() override;
    Result<void> saveUsers(const std::vector<UserDef>& users) override;
    Result<std::vector<UserDef>> loadUsers() override;

    // Accumulated warnings (dropped truncated lines...). Cleared on call.
    std::vector<std::string> takeWarnings();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return dir_; }

    // Automatic compaction threshold: tombstones > kCompactMinTombstones
    // AND tombstones > live rows.
    static constexpr std::size_t kCompactMinTombstones = 1000;

private:
    explicit FileEngine(std::filesystem::path dir) : dir_(std::move(dir)) {}

    struct Table {
        TableSchema schema;
        std::map<RowId, Row> rows;
        RowId nextId = 1;
        std::size_t tombstones = 0;
        bool loaded = false;
        std::FILE* out = nullptr;  // rows.txt opened for append, nullptr until loaded
        TableIndexes indexes;             // rebuilt by loadRows
    };

    [[nodiscard]] std::filesystem::path tableDir(std::string_view table) const;
    Result<Table*> loaded(std::string_view table);  // loads rows.txt if needed
    Result<void> loadRows(Table& t);
    Result<void> appendLine(Table& t, const std::string& line);
    Result<void> rewrite(Table& t);  // actual compaction
    Result<void> maybeCompact(Table& t);
    static void closeFile(Table& t) noexcept;

    std::filesystem::path dir_;
    std::map<std::string, Table, std::less<>> tables_;  // every known table (schema loaded)
    std::vector<std::string> warnings_;
    std::mutex mu_;
};

}  // namespace ledger
