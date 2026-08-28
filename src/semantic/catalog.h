#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/schema.h"

namespace ledger {

// The set of known schemas. In memory, single process: the storage layer
// fills it at startup (reading the schema.txt files) and keeps it up to date
// after each effective CREATE/DROP. The catalog itself never touches the disk.
//
// Pointers returned by find() stay valid as long as the table is not removed
// (std::map never moves its nodes).
class Catalog {
public:
    [[nodiscard]] const TableSchema* find(std::string_view table) const noexcept;
    [[nodiscard]] bool contains(std::string_view table) const noexcept { return find(table) != nullptr; }

    Result<void> add(TableSchema schema);          // AlreadyExists
    Result<void> remove(std::string_view table);   // NotFound

    [[nodiscard]] std::vector<std::string_view> tableNames() const;  // sorted
    [[nodiscard]] std::size_t size() const noexcept { return tables_.size(); }

private:
    // std::less<>: find() by string_view without building a std::string.
    std::map<std::string, TableSchema, std::less<>> tables_;
};

}  // namespace ledger
