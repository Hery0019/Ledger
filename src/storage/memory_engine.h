#pragma once

#include <map>
#include <string>

#include "storage/engine.h"

namespace ledger {

// Moteur tout en mémoire, sans persistance. Sert de référence aux tests de
// l'exécuteur et de la CLI : même contrat que FileEngine, zéro disque.
class MemoryEngine final : public IStorageEngine {
public:
    Result<void> createTable(const TableSchema& schema) override;
    Result<void> dropTable(std::string_view table) override;
    Result<RowId> insert(std::string_view table, const Row& row) override;
    Result<void> scan(std::string_view table,
                      const std::function<bool(RowId, const Row&)>& visit) override;
    Result<void> update(std::string_view table, RowId id, const Row& row) override;
    Result<void> remove(std::string_view table, RowId id) override;
    Result<void> compact(std::string_view table) override;
    Result<std::vector<TableSchema>> loadSchemas() override;

private:
    struct Table {
        TableSchema schema;
        std::map<RowId, Row> rows;
        RowId nextId = 1;
    };
    Result<Table*> find(std::string_view table);

    std::map<std::string, Table, std::less<>> tables_;
};

}  // namespace ledger
