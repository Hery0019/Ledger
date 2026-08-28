#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

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

    // Rewrites the rows file without tombstones. The engine may also trigger
    // it on its own.
    virtual Result<void> compact(std::string_view table) = 0;

    // Schemas of every existing table, to fill the Catalog at startup.
    virtual Result<std::vector<TableSchema>> loadSchemas() = 0;

    // Views are stored as one small list, rewritten whole on every change
    // (creation order is preserved: it is also the load order).
    virtual Result<void> saveViews(const std::vector<ViewDef>& views) = 0;
    virtual Result<std::vector<ViewDef>> loadViews() = 0;
};

}  // namespace ledger
