#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "core/schema.h"

namespace ledger {

// In-memory index on one column: key value -> rowid. NULL keys are never
// indexed (a UNIQUE column may hold several NULLs; a PRIMARY KEY has none).
// Keys share the column's type, so Value::compare gives a total order. Not
// persisted: rebuilt when the table's rows are loaded.
class ColumnIndex {
public:
    explicit ColumnIndex(std::size_t column) : column_(column) {}

    [[nodiscard]] std::size_t column() const noexcept { return column_; }

    void add(RowId id, const Row& row) {
        if (!row[column_].isNull()) map_.emplace(row[column_], id);
    }
    void remove(const Row& row) {
        if (!row[column_].isNull()) map_.erase(row[column_]);
    }
    void clear() { map_.clear(); }

    [[nodiscard]] std::optional<RowId> find(const Value& key) const {
        if (key.isNull()) return std::nullopt;
        const auto it = map_.find(key);
        return it == map_.end() ? std::nullopt : std::optional<RowId>(it->second);
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
    std::map<Value, RowId, Less> map_;
};

// Every index of one table: the PRIMARY KEY and each UNIQUE column.
class TableIndexes {
public:
    TableIndexes() = default;
    explicit TableIndexes(const TableSchema& schema) {
        for (std::size_t i = 0; i < schema.columns.size(); ++i) {
            if (schema.columns[i].primaryKey || schema.columns[i].unique) indexes_.emplace_back(i);
        }
    }

    [[nodiscard]] const ColumnIndex* on(std::size_t column) const noexcept {
        for (const auto& idx : indexes_) {
            if (idx.column() == column) return &idx;
        }
        return nullptr;
    }
    [[nodiscard]] bool has(std::size_t column) const noexcept { return on(column) != nullptr; }

    void add(RowId id, const Row& row) {
        for (auto& idx : indexes_) idx.add(id, row);
    }
    void remove(const Row& row) {
        for (auto& idx : indexes_) idx.remove(row);
    }
    // update = remove old + add new; keys may have changed.
    void replace(RowId id, const Row& oldRow, const Row& newRow) {
        remove(oldRow);
        add(id, newRow);
    }
    void clear() {
        for (auto& idx : indexes_) idx.clear();
    }

private:
    std::vector<ColumnIndex> indexes_;
};

}  // namespace ledger
