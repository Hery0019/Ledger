#pragma once

#include <cstddef>
#include <map>
#include <optional>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"

namespace ledger {

// In-memory index on a table's PRIMARY KEY column: key value -> rowid. Shared
// by the engines; each keeps one per table. Keys are never NULL (PK implies
// NOT NULL) and share the column's type, so Value::compare gives a total
// order. Not persisted: rebuilt when the table's rows are loaded.
class PkIndex {
public:
    // No PRIMARY KEY: the index stays empty and `column()` is nullopt.
    explicit PkIndex(const TableSchema& schema = TableSchema{}) : column_(schema.primaryKeyIndex()) {}

    [[nodiscard]] std::optional<std::size_t> column() const noexcept { return column_; }

    void add(RowId id, const Row& row) {
        if (column_) map_.emplace(row[*column_], id);
    }
    void remove(const Row& row) {
        if (column_) map_.erase(row[*column_]);
    }
    // update = remove old + add new; the key may have changed.
    void replace(RowId id, const Row& oldRow, const Row& newRow) {
        remove(oldRow);
        add(id, newRow);
    }
    void clear() { map_.clear(); }

    [[nodiscard]] std::optional<RowId> find(const Value& key) const {
        if (key.isNull()) return std::nullopt;
        const auto it = map_.find(key);
        return it == map_.end() ? std::nullopt : std::optional<RowId>(it->second);
    }

private:
    struct Less {
        bool operator()(const Value& a, const Value& b) const {
            return Value::compare(a, b).value() == Ordering::Less;
        }
    };
    std::optional<std::size_t> column_;
    std::map<Value, RowId, Less> map_;
};

}  // namespace ledger
