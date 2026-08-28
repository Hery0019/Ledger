#include "semantic/catalog.h"

#include <algorithm>
#include <utility>

namespace ledger {

namespace {
Error nameTaken(const std::string& name, const char* what) {
    return makeError(ErrorCode::AlreadyExists, std::string(what) + " '" + name + "' already exists");
}
}  // namespace

// ---- tables ----------------------------------------------------------------

const TableSchema* Catalog::find(std::string_view table) const noexcept {
    const auto it = tables_.find(table);
    return it == tables_.end() ? nullptr : &it->second;
}

Result<void> Catalog::add(TableSchema schema) {
    if (contains(schema.name)) return nameTaken(schema.name, "table");
    if (findView(schema.name)) return nameTaken(schema.name, "view");
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

// ---- views -----------------------------------------------------------------

const ViewEntry* Catalog::findView(std::string_view view) const noexcept {
    for (const auto& v : views_) {
        if (v.def.name == view) return &v;
    }
    return nullptr;
}

Result<void> Catalog::addView(ViewDef def, std::vector<std::string> sources) {
    if (contains(def.name)) return nameTaken(def.name, "table");
    if (findView(def.name)) return nameTaken(def.name, "view");
    views_.push_back(ViewEntry{std::move(def), std::move(sources)});
    return {};
}

Result<void> Catalog::removeView(std::string_view view) {
    const auto it = std::find_if(views_.begin(), views_.end(),
                                 [&](const ViewEntry& v) { return v.def.name == view; });
    if (it == views_.end()) {
        return makeError(ErrorCode::NotFound, "unknown view '" + std::string(view) + "'");
    }
    views_.erase(it);
    return {};
}

std::vector<ViewDef> Catalog::views() const {
    std::vector<ViewDef> out;
    out.reserve(views_.size());
    for (const auto& v : views_) out.push_back(v.def);
    return out;
}

std::vector<std::string_view> Catalog::viewNames() const {
    std::vector<std::string_view> names;
    names.reserve(views_.size());
    for (const auto& v : views_) names.push_back(v.def.name);
    return names;
}

std::vector<std::string_view> Catalog::dependents(std::string_view name) const {
    std::vector<std::string_view> out;
    for (const auto& v : views_) {
        if (std::find(v.sources.begin(), v.sources.end(), name) != v.sources.end()) {
            out.push_back(v.def.name);
        }
    }
    return out;
}

}  // namespace ledger
