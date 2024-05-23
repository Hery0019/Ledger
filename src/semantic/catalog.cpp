#include "semantic/catalog.h"

#include <utility>

namespace ledger {

std::optional<std::size_t> TableSchema::columnIndex(std::string_view column) const noexcept {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == column) return i;
    }
    return std::nullopt;
}

std::optional<std::size_t> TableSchema::primaryKeyIndex() const noexcept {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].primaryKey) return i;
    }
    return std::nullopt;
}

const TableSchema* Catalog::find(std::string_view table) const noexcept {
    const auto it = tables_.find(table);
    return it == tables_.end() ? nullptr : &it->second;
}

Result<void> Catalog::add(TableSchema schema) {
    if (contains(schema.name)) {
        return makeError(ErrorCode::AlreadyExists, "table '" + schema.name + "' already exists");
    }
    std::string key = schema.name;
    tables_.emplace(std::move(key), std::move(schema));
    return {};
}

Result<void> Catalog::remove(std::string_view table) {
    const auto it = tables_.find(table);
    if (it == tables_.end()) {
        return makeError(ErrorCode::NotFound, "unknown table '" + std::string(table) + "'");
    }
    tables_.erase(it);
    return {};
}

std::vector<std::string_view> Catalog::tableNames() const {
    std::vector<std::string_view> names;
    names.reserve(tables_.size());
    for (const auto& [name, schema] : tables_) names.push_back(name);
    return names;
}

}  // namespace ledger
