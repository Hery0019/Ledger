#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/value.h"

namespace ledger {

struct ColumnSchema {
    std::string name;  // lowercase
    DataType type;     // never DataType::Null
    bool primaryKey;
    bool notNull;      // always true when primaryKey
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

}  // namespace ledger
