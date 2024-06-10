#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/value.h"

namespace ledger {

struct ColumnSchema {
    std::string name;  // minuscules
    DataType type;     // jamais DataType::Null
    bool primaryKey;
    bool notNull;      // toujours true si primaryKey
};

struct TableSchema {
    std::string name;  // minuscules
    std::vector<ColumnSchema> columns;  // jamais vide

    [[nodiscard]] std::optional<std::size_t> columnIndex(std::string_view column) const noexcept {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == column) return i;
        }
        return std::nullopt;
    }

    // Index de la colonne PRIMARY KEY, s'il y en a une (au plus une par table).
    [[nodiscard]] std::optional<std::size_t> primaryKeyIndex() const noexcept {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].primaryKey) return i;
        }
        return std::nullopt;
    }
};

}  // namespace ledger
