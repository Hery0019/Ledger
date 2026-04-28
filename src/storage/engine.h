#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <string_view>
#include <vector>

#include "core/password.h"
#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"

namespace ledger {

// Row identifier, unique within its table, monotonic, never reused. An update
// keeps the rowid: the row keeps its identity.
using RowId = std::uint64_t;

// Storage engine contract. The executor only knows this interface;
// FileEngine (text files) and MemoryEngine (tests) implement it.
//
// The engine does NOT check value types (that is the binder's job); it only
// checks that the Row has the right number of columns (otherwise Internal:
// a caller bug, not an expected error).
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    virtual Result<void> createTable(const TableSchema& schema) = 0;   // AlreadyExists
    virtual Result<void> dropTable(std::string_view table) = 0;        // NotFound

    virtual Result<RowId> insert(std::string_view table, const Row& row) = 0;

    // Walks the live rows by ascending rowid. `visit` returns false to stop.
    // No copy of the whole table: that is why a callback is used instead of
    // a vector.
    // Precondition: do not modify the table during the walk.
    virtual Result<void> scan(std::string_view table,
                              const std::function<bool(RowId, const Row&)>& visit) = 0;

    virtual Result<void> update(std::string_view table, RowId id, const Row& row) = 0;  // NotFound
    virtual Result<void> remove(std::string_view table, RowId id) = 0;                  // NotFound

    // Re-inserts a row under a rowid that was removed earlier (transaction
    // rollback of a DELETE). The rowid must not be live. AlreadyExists otherwise.
    virtual Result<void> restore(std::string_view table, RowId id, const Row& row) = 0;

    // Primary-key index. Every table with a PRIMARY KEY has an in-memory
    // index on it, maintained by the engine on every write and rebuilt when
    // the table is loaded. `lookup` answers "which live row has this key"
    // without a scan; it must only be called for the PK column (Internal
    // otherwise). NULL never matches.
    [[nodiscard]] virtual bool indexed(std::string_view table, std::size_t column) const noexcept = 0;
    virtual Result<std::optional<std::pair<RowId, Row>>> lookup(std::string_view table, std::size_t column,
                                                                const Value& key) = 0;
    // Largest live key of an indexed column (AUTOINCREMENT); nullopt when the
    // table has no row with a value there. Internal on a column without index.
    virtual Result<std::optional<Value>> maxKey(std::string_view table, std::size_t column) = 0;

    // Rewrites the rows file without tombstones. The engine may also trigger
    // it on its own.
    virtual Result<void> compact(std::string_view table) = 0;

    // Schemas of every existing table, to fill the Catalog at startup.
    virtual Result<std::vector<TableSchema>> loadSchemas() = 0;

    // Views are stored as one small list, rewritten whole on every change
    // (creation order is preserved: it is also the load order).
    virtual Result<void> saveViews(const std::vector<ViewDef>& views) = 0;
    virtual Result<std::vector<ViewDef>> loadViews() = 0;

    // User accounts, same shape as views: one small list, rewritten whole.
    // Only the salted hashes travel here; passwords never reach the engine.
    virtual Result<void> saveUsers(const std::vector<UserDef>& users) = 0;
    virtual Result<std::vector<UserDef>> loadUsers() = 0;
};

}  // namespace ledger
