#include "storage/codec.h"

#include <charconv>
#include <vector>

namespace ledger::codec {

namespace {

constexpr std::string_view kNull = "\\N";

Error corruption(std::string what) { return makeError(ErrorCode::Corruption, std::move(what)); }

// Splits on a separator; "" gives [""] (one empty field), never [].
std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

Result<RowId> parseRowId(std::string_view text) {
    RowId id = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec != std::errc{} || ptr != text.data() + text.size() || text.empty()) {
        return corruption("invalid rowid '" + std::string(text) + "'");
    }
    return id;
}

std::optional<DataType> parseType(std::string_view name) {
    if (name == "INT") return DataType::Int;
    if (name == "FLOAT") return DataType::Float;
    if (name == "TEXT") return DataType::Text;
    if (name == "BOOL") return DataType::Bool;
    return std::nullopt;
}

}  // namespace

// ---- text ------------------------------------------------------------------

std::string escapeText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:   out += c; break;
        }
    }
    return out;
}

Result<std::string> unescapeText(std::string_view field) {
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] != '\\') {
            out += field[i];
            continue;
        }
        if (i + 1 >= field.size()) return corruption("dangling backslash in text field");
        switch (field[++i]) {
            case '\\': out += '\\'; break;
            case 't':  out += '\t'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            default:
                return corruption(std::string("invalid escape '\\") + field[i] + "' in text field");
        }
    }
    return out;
}

std::string escapeAttr(std::string_view text) {
    std::string out;
    for (const char c : escapeText(text)) {
        if (c == ' ') out += "\\s";
        else out += c;
    }
    return out;
}

Result<std::string> unescapeAttr(std::string_view field) {
    // One pass: `\s` is a space, everything else follows unescapeText.
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] != '\\') {
            out += field[i];
            continue;
        }
        if (i + 1 >= field.size()) return corruption("dangling backslash in attribute");
        switch (field[++i]) {
            case '\\': out += '\\'; break;
            case 't':  out += '\t'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 's':  out += ' '; break;
            default:
                return corruption(std::string("invalid escape '\\") + field[i] + "' in attribute");
        }
    }
    return out;
}

// ---- values ----------------------------------------------------------------

std::string encodeValue(const Value& value) {
    if (value.isNull()) return std::string(kNull);
    if (value.type() == DataType::Text) return escapeText(value.asText());
    return value.toText();
}

Result<Value> decodeValue(std::string_view field, DataType type) {
    if (field == kNull) return Value::null();
    if (type == DataType::Text) {
        LEDGER_TRY(text, unescapeText(field));
        return Value::text(std::move(text));
    }
    auto v = Value::fromText(type, field);
    if (!v.ok()) return corruption(v.error().message);
    return v;
}

// ---- schema ----------------------------------------------------------------

std::string encodeSchema(const TableSchema& schema) {
    std::string out(kSchemaHeader);
    out += '\n';
    for (const auto& c : schema.columns) {
        out += c.name;
        out += ' ';
        out += dataTypeName(c.type);
        if (c.primaryKey) out += " PK";
        else if (c.notNull) out += " NN";
        if (c.unique) out += " UQ";
        // A NULL default is its own flag, so that a TEXT default of "NULL" or
        // "\N" is never confused with it.
        if (c.defaultValue) {
            out += c.defaultValue->isNull() ? " DEFNULL" : " DEF:" + escapeAttr(c.defaultValue->toText());
        }
        out += '\n';
    }
    return out;
}

Result<TableSchema> decodeSchema(std::string_view tableName, std::string_view content) {
    auto lines = split(content, '\n');
    // A well-formed file ends with '\n': last element empty.
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty() || lines[0] != kSchemaHeader) {
        return corruption("schema.txt: missing or unknown header (expected '" +
                          std::string(kSchemaHeader) + "')");
    }

    TableSchema schema{std::string(tableName), {}};
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto parts = split(lines[i], ' ');
        const std::string where = "schema.txt:" + std::to_string(i + 1) + ": ";
        if (parts.size() < 2 || parts[0].empty()) {
            return corruption(where + "malformed column definition '" + std::string(lines[i]) + "'");
        }
        const auto type = parseType(parts[1]);
        if (!type) return corruption(where + "unknown type '" + std::string(parts[1]) + "'");
        ColumnSchema col{std::string(parts[0]), *type, false, false, std::nullopt};
        for (std::size_t p = 2; p < parts.size(); ++p) {
            const std::string_view flag = parts[p];
            if (flag == "PK") {
                col.primaryKey = col.notNull = true;
            } else if (flag == "NN") {
                col.notNull = true;
            } else if (flag == "UQ") {
                col.unique = true;
            } else if (flag == "DEFNULL") {
                col.defaultValue = Value::null();
            } else if (flag.starts_with("DEF:")) {
                LEDGER_TRY(text, unescapeAttr(flag.substr(4)));
                auto v = Value::fromText(col.type, text);
                if (!v.ok()) return corruption(where + "bad DEFAULT: " + v.error().message);
                col.defaultValue = std::move(v).value();
            } else {
                return corruption(where + "unknown constraint '" + std::string(flag) + "'");
            }
        }
        if (schema.columnIndex(col.name)) return corruption(where + "duplicate column '" + col.name + "'");
        schema.columns.push_back(std::move(col));
    }
    if (schema.columns.empty()) return corruption("schema.txt: table has no columns");
    return schema;
}

// ---- records ---------------------------------------------------------------

std::string encodeInsert(RowId id, const Row& row) {
    std::string out = "I " + std::to_string(id);
    for (const auto& v : row) {
        out += '\t';
        out += encodeValue(v);
    }
    return out;
}

std::string encodeTombstone(RowId id) { return "D " + std::to_string(id); }

Result<Record> decodeRecord(std::string_view line, const TableSchema& schema) {
    if (line.size() < 2 || line[1] != ' ' || (line[0] != 'I' && line[0] != 'D')) {
        return corruption("malformed record '" + std::string(line.substr(0, 40)) + "'");
    }
    const std::string_view rest = line.substr(2);

    if (line[0] == 'D') {
        LEDGER_TRY(id, parseRowId(rest));
        return Record{Record::Kind::Delete, id, {}};
    }

    const std::size_t tab = rest.find('\t');
    LEDGER_TRY(id, parseRowId(rest.substr(0, tab)));
    std::vector<std::string_view> fields;
    if (tab != std::string_view::npos) fields = split(rest.substr(tab + 1), '\t');
    if (fields.size() != schema.columns.size()) {
        return corruption("row " + std::to_string(id) + ": expected " +
                          std::to_string(schema.columns.size()) + " fields, got " +
                          std::to_string(fields.size()));
    }

    Row row;
    row.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        auto v = decodeValue(fields[i], schema.columns[i].type);
        if (!v.ok()) {
            return corruption("row " + std::to_string(id) + ", column '" + schema.columns[i].name +
                              "': " + v.error().message);
        }
        row.push_back(std::move(v).value());
    }
    return Record{Record::Kind::Insert, id, std::move(row)};
}

}  // namespace ledger::codec
