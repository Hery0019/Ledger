#pragma once

#include <map>
#include <string>

#include "storage/engine.h"
#include "storage/pk_index.h"

namespace ledger {

// All-in-memory engine, no persistence. Reference implementation for the
// executor and CLI tests: same contract as FileEngine, zero disk.
class MemoryEngine final : public IStorageEngine {
public:
    Result<void> createTable(const TableSchema& schema) override;
    Result<void> dropTable(std::string_view table) override;
    Result<RowId> insert(std::string_view table, const Row& row) override;
    Result<void> scan(std::string_view table,
                      const std::function<bool(RowId, const Row&)>& visit) override;
    Result<void> update(std::string_view table, RowId id, const Row& row) override;
    Result<void> remove(std::string_view table, RowId id) override;
    Result<void> restore(std::string_view table, RowId id, const Row& row) override;
    [[nodiscard]] bool indexed(std::string_view table, std::size_t column) const noexcept override;
    Result<std::optional<std::pair<RowId, Row>>> lookup(std::string_view table, std::size_t column,
                                                        const Value& key) override;
    Result<std::optional<Value>> maxKey(std::string_view table, std::size_t column) override;
    Result<void> compact(std::string_view table) override;
    Result<std::vector<TableSchema>> loadSchemas() override;
    Result<void> saveViews(const std::vector<ViewDef>& views) override;
    Result<std::vector<ViewDef>> loadViews() override;
    Result<void> saveUsers(const std::vector<UserDef>& users) override;
    Result<std::vector<UserDef>> loadUsers() override;

private:
    struct Table {
        TableSchema schema;
        std::map<RowId, Row> rows;
        RowId nextId = 1;
        TableIndexes indexes;
    };
    Result<Table*> find(std::string_view table);

    std::map<std::string, Table, std::less<>> tables_;
    std::vector<ViewDef> views_;
    std::vector<UserDef> users_;
};

}  // namespace ledger
