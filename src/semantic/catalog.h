#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/password.h"
#include "core/result.h"
#include "core/schema.h"

namespace ledger {

// A registered view: its definition plus the names of what it reads from
// (tables or other views, FROM and JOINs), used for dependency checks on DROP.
struct ViewEntry {
    ViewDef def;
    std::vector<std::string> sources;
};

// The set of known schemas and views. In memory, single process: the storage
// layer fills it at startup (reading the schema.txt files and views.txt) and
// keeps it up to date after each effective CREATE/DROP. The catalog itself
// never touches the disk.
//
// Tables and views share one namespace. Pointers returned by find() stay valid
// as long as the table is not removed (std::map never moves its nodes);
// pointers returned by findView() are invalidated by any view change.
class Catalog {
public:
    // ---- tables ------------------------------------------------------------

    [[nodiscard]] const TableSchema* find(std::string_view table) const noexcept;
    [[nodiscard]] bool contains(std::string_view table) const noexcept { return find(table) != nullptr; }

    Result<void> add(TableSchema schema);          // AlreadyExists (table or view)
    Result<void> remove(std::string_view table);   // NotFound

    [[nodiscard]] std::vector<std::string_view> tableNames() const;  // sorted
    // Every (child table, column) whose REFERENCES targets `table`, the
    // table's self-references included. Sorted by child name.
    [[nodiscard]] std::vector<std::pair<const TableSchema*, std::size_t>> referencing(std::string_view table) const;
    [[nodiscard]] std::size_t size() const noexcept { return tables_.size(); }

    // ---- views -------------------------------------------------------------

    [[nodiscard]] const ViewEntry* findView(std::string_view view) const noexcept;

    // `sources` are the tables and views the SELECT reads from; the caller
    // (the binder) has already parsed and validated the definition.
    Result<void> addView(ViewDef def, std::vector<std::string> sources);  // AlreadyExists (table or view)
    Result<void> removeView(std::string_view view);         // NotFound

    // Creation order, which is also a valid load order (a view is always
    // created after what it reads from).
    [[nodiscard]] std::vector<ViewDef> views() const;
    [[nodiscard]] std::vector<std::string_view> viewNames() const;

    // Names of the views that read directly from `name` (table or view).
    [[nodiscard]] std::vector<std::string_view> dependents(std::string_view name) const;

    // True if `name` is a table or a view.
    [[nodiscard]] bool hasName(std::string_view name) const noexcept {
        return contains(name) || findView(name) != nullptr;
    }

    // ---- users ---------------------------------------------------------------
    //
    // Accounts gate the HTTP server; they live in their own namespace (a user
    // and a table may share a name). Pointers returned by findUser() are
    // invalidated by any user change.

    [[nodiscard]] const UserDef* findUser(std::string_view name) const noexcept;

    Result<void> addUser(UserDef user);              // AlreadyExists
    Result<void> replaceUser(UserDef user);          // NotFound (ALTER USER)
    Result<void> removeUser(std::string_view name);  // NotFound

    [[nodiscard]] std::vector<UserDef> users() const;              // sorted by name
    [[nodiscard]] std::vector<std::string_view> userNames() const; // sorted
    [[nodiscard]] bool hasUsers() const noexcept { return !users_.empty(); }

private:
    // std::less<>: find() by string_view without building a std::string.
    std::map<std::string, TableSchema, std::less<>> tables_;
    std::vector<ViewEntry> views_;
    std::map<std::string, UserDef, std::less<>> users_;
};

}  // namespace ledger
