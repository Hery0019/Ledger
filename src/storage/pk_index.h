#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"

namespace ledger {

// In-memory index on one column: key value -> live rowids. NULL keys are
// never indexed (a UNIQUE column may hold several NULLs; a PRIMARY KEY has
// none). A unique index (PRIMARY KEY, UNIQUE) holds at most one rowid per
// key and backs the uniqueness checks; a non-unique index (CREATE INDEX)
// accepts duplicates and only speeds lookups up. Keys share the column's
// type, so Value::compare gives a total order. Not persisted: rebuilt when
// the table's rows are loaded.
class ColumnIndex {
public:
    ColumnIndex(std::size_t column, bool unique) : column_(column), unique_(unique) {}

    [[nodiscard]] std::size_t column() const noexcept { return column_; }
    [[nodiscard]] bool unique() const noexcept { return unique_; }

    void add(RowId id, const Row& row) {
        if (!row[column_].isNull()) map_.emplace(row[column_], id);
    }
    // Removes one (key, rowid) entry: with duplicates allowed, the key alone
    // would not identify it.
    void remove(RowId id, const Row& row) {
        if (row[column_].isNull()) return;
        auto [lo, hi] = map_.equal_range(row[column_]);
        for (auto it = lo; it != hi; ++it) {
            if (it->second == id) {
                map_.erase(it);
                return;
            }
        }
    }
    void clear() { map_.clear(); }

    // First live row with this key — all there is, on a unique index.
    [[nodiscard]] std::optional<RowId> find(const Value& key) const {
        if (key.isNull()) return std::nullopt;
        const auto it = map_.find(key);
        return it == map_.end() ? std::nullopt : std::optional<RowId>(it->second);
    }

    // Every live row with this key, in insertion-independent (rowid) order.
    [[nodiscard]] std::vector<RowId> findAll(const Value& key) const {
        std::vector<RowId> out;
        if (key.isNull()) return out;
        auto [lo, hi] = map_.equal_range(key);
        for (auto it = lo; it != hi; ++it) out.push_back(it->second);
        std::sort(out.begin(), out.end());
        return out;
    }

    // Largest key, for AUTOINCREMENT-style "next value" questions.
    [[nodiscard]] std::optional<Value> maxKey() const {
        if (map_.empty()) return std::nullopt;
        return map_.rbegin()->first;
    }

private:
    struct Less {
        bool operator()(const Value& a, const Value& b) const {
            return Value::compare(a, b).value() == Ordering::Less;
        }
    };
    std::size_t column_;
    bool unique_;
    std::multimap<Value, RowId, Less> map_;
};

// Every index of one table: the PRIMARY KEY and each UNIQUE column (unique),
// plus any user-declared CREATE INDEX columns (non-unique).
class TableIndexes {
public:
    TableIndexes() = default;
    explicit TableIndexes(const TableSchema& schema) {
        for (std::size_t i = 0; i < schema.columns.size(); ++i) {
            if (schema.columns[i].primaryKey || schema.columns[i].unique) {
                indexes_.emplace_back(i, /*unique=*/true);
            }
        }
    }

    [[nodiscard]] const ColumnIndex* on(std::size_t column) const noexcept {
        for (const auto& idx : indexes_) {
            if (idx.column() == column) return &idx;
        }
        return nullptr;
    }
    [[nodiscard]] bool has(std::size_t column) const noexcept { return on(column) != nullptr; }

    // Declares an extra (non-unique) index; the caller fills it by re-adding
    // the live rows. False if the column already has one.
    bool addIndex(std::size_t column) {
        if (has(column)) return false;
        indexes_.emplace_back(column, /*unique=*/false);
        return true;
    }
    // Removes a non-unique index. False if there is none (schema-born unique
    // indexes are structural and never removed this way).
    bool removeIndex(std::size_t column) {
        for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
            if (it->column() == column && !it->unique()) {
                indexes_.erase(it);
                return true;
            }
        }
        return false;
    }

    void add(RowId id, const Row& row) {
        for (auto& idx : indexes_) idx.add(id, row);
    }
    void remove(RowId id, const Row& row) {
        for (auto& idx : indexes_) idx.remove(id, row);
    }
    // update = remove old + add new; keys may have changed.
    void replace(RowId id, const Row& oldRow, const Row& newRow) {
        remove(id, oldRow);
        add(id, newRow);
    }
    void clear() {
        for (auto& idx : indexes_) idx.clear();
    }

private:
    std::vector<ColumnIndex> indexes_;
};

}  // namespace ledger
