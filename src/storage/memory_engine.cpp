#include "storage/memory_engine.h"

namespace ledger {

Result<MemoryEngine::Table*> MemoryEngine::find(std::string_view table) {
    const auto it = tables_.find(table);
    if (it == tables_.end()) {
        return makeError(ErrorCode::NotFound, "unknown table '" + std::string(table) + "'");
    }
    return &it->second;
}

Result<void> MemoryEngine::createTable(const TableSchema& schema) {
    if (tables_.contains(schema.name)) {
        return makeError(ErrorCode::AlreadyExists, "table '" + schema.name + "' already exists");
    }
    tables_.emplace(schema.name, Table{schema, {}, 1, TableIndexes(schema)});
    return {};
}

Result<void> MemoryEngine::dropTable(std::string_view table) {
    LEDGER_TRY_VOID(find(table));
    tables_.erase(tables_.find(table));
    return {};
}

Result<RowId> MemoryEngine::insert(std::string_view table, const Row& row) {
    LEDGER_TRY(t, find(table));
    if (row.size() != t->schema.columns.size()) {
        return makeError(ErrorCode::Internal, "insert: row has " + std::to_string(row.size()) +
                                                  " values, table '" + t->schema.name + "' has " +
                                                  std::to_string(t->schema.columns.size()) + " columns");
    }
    const RowId id = t->nextId++;
    t->rows.emplace(id, row);
    t->indexes.add(id, row);
    return id;
}

Result<void> MemoryEngine::scan(std::string_view table,
                                const std::function<bool(RowId, const Row&)>& visit) {
    LEDGER_TRY(t, find(table));
    for (const auto& [id, row] : t->rows) {
        if (!visit(id, row)) break;
    }
    return {};
}

Result<void> MemoryEngine::update(std::string_view table, RowId id, const Row& row) {
    LEDGER_TRY(t, find(table));
    const auto it = t->rows.find(id);
    if (it == t->rows.end()) {
        return makeError(ErrorCode::NotFound, "row " + std::to_string(id) + " not found");
    }
    if (row.size() != t->schema.columns.size()) {
        return makeError(ErrorCode::Internal, "update: wrong number of values");
    }
    t->indexes.replace(id, it->second, row);
    it->second = row;
    return {};
}

Result<void> MemoryEngine::remove(std::string_view table, RowId id) {
    LEDGER_TRY(t, find(table));
    const auto it = t->rows.find(id);
    if (it == t->rows.end()) {
        return makeError(ErrorCode::NotFound, "row " + std::to_string(id) + " not found");
    }
    t->indexes.remove(it->second);
    t->rows.erase(it);
    return {};
}

Result<void> MemoryEngine::restore(std::string_view table, RowId id, const Row& row) {
    LEDGER_TRY(t, find(table));
    if (t->rows.contains(id)) {
        return makeError(ErrorCode::AlreadyExists, "row " + std::to_string(id) + " is live");
    }
    if (row.size() != t->schema.columns.size()) {
        return makeError(ErrorCode::Internal, "restore: wrong number of values");
    }
    t->rows.emplace(id, row);
    t->indexes.add(id, row);
    if (id >= t->nextId) t->nextId = id + 1;
    return {};
}

bool MemoryEngine::indexed(std::string_view table, std::size_t column) const noexcept {
    const auto it = tables_.find(table);
    return it != tables_.end() && it->second.indexes.has(column);
}

Result<std::optional<std::pair<RowId, Row>>> MemoryEngine::lookup(std::string_view table, std::size_t column,
                                                                  const Value& key) {
    LEDGER_TRY(t, find(table));
    const ColumnIndex* index = t->indexes.on(column);
    if (!index) return makeError(ErrorCode::Internal, "lookup on a column without an index");
    const auto id = index->find(key);
    if (!id) return std::optional<std::pair<RowId, Row>>{};
    return std::optional<std::pair<RowId, Row>>{std::pair{*id, t->rows.at(*id)}};
}

Result<std::optional<Value>> MemoryEngine::maxKey(std::string_view table, std::size_t column) {
    LEDGER_TRY(t, find(table));
    const ColumnIndex* index = t->indexes.on(column);
    if (!index) return makeError(ErrorCode::Internal, "maxKey on a column without an index");
    return index->maxKey();
}

Result<void> MemoryEngine::compact(std::string_view table) {
    LEDGER_TRY_VOID(find(table));
    return {};  // nothing to compact in memory
}

Result<std::vector<TableSchema>> MemoryEngine::loadSchemas() {
    std::vector<TableSchema> out;
    for (const auto& [name, t] : tables_) out.push_back(t.schema);
    return out;
}

Result<void> MemoryEngine::saveViews(const std::vector<ViewDef>& views) {
    views_ = views;
    return {};
}

Result<std::vector<ViewDef>> MemoryEngine::loadViews() { return views_; }

}  // namespace ledger
