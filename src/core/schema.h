#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/value.h"

namespace ledger {

// REFERENCES parent(column): every non-NULL value of the column must exist
// in the parent's column, which is PRIMARY KEY or UNIQUE (so the check is an
// index lookup). Deleting a referenced parent row is refused, or deletes the
// referencing rows when `cascade` is set (ON DELETE CASCADE).
struct ForeignKey {
    std::string table;   // lowercase; may be the column's own table
    std::string column;  // lowercase
    bool cascade;
};

struct ColumnSchema {
    std::string name;  // lowercase
    DataType type;     // never DataType::Null
    bool primaryKey;
    bool notNull;      // always true when primaryKey
    // DEFAULT: the value used when an INSERT omits the column (a constant,
    // already converted to the column type). Absent = NULL.
    std::optional<Value> defaultValue;
    bool unique = false;  // UNIQUE: no two live rows share a non-NULL value
    // CHECK: a BOOL expression over the table's columns that every row must
    // not make FALSE (NULL passes). Kept as SQL text; empty = no constraint.
    std::string check;
    std::optional<ForeignKey> reference;

    // A constructor rather than aggregate initialization, so that adding a
    // constraint field never touches the many `ColumnSchema{...}` call sites.
    ColumnSchema(std::string n, DataType t, bool pk, bool nn, std::optional<Value> def = std::nullopt,
                 bool uq = false)
        : name(std::move(n)), type(t), primaryKey(pk), notNull(nn), defaultValue(std::move(def)), unique(uq) {}
};

struct TableSchema {
    std::string name;  // lowercase
    std::vector<ColumnSchema> columns;  // never empty

    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const noexcept {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == column) return i;
        }
        return std::nullopt;
    }

    // Index of the PRIMARY KEY column, if any (at most one per table).
    [[nodiscard]] std::optional<std::size_t> primaryKeyIndex() const noexcept {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].primaryKey) return i;
        }
        return std::nullopt;
    }
};

// A view is a named, stored SELECT. Only its text is persisted; it is parsed
// again whenever it is used. Views and tables share one namespace.
struct ViewDef {
    std::string name;  // lowercase
    std::string sql;   // the SELECT, verbatim (no trailing `;`)
};

}  // namespace ledger
