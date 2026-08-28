#include "server/json.h"

#include <cstdio>

namespace ledger {

std::string jsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string toJson(const Value& value) {
    switch (value.type()) {
        case DataType::Null:
            return "null";
        case DataType::Text:
            return jsonString(value.asText());
        case DataType::Int:
        case DataType::Float:
        case DataType::Bool:
            // toText() already emits valid JSON for these: decimal integers,
            // shortest round-trip floats (never NaN/Inf), true/false.
            return value.toText();
    }
    return "null";
}

std::string toJson(const QueryResult& result) {
    switch (result.kind) {
        case ResultKind::Ddl:
            return R"({"kind":"ddl"})";
        case ResultKind::Dml:
            return R"({"kind":"dml","affected":)" + std::to_string(result.affected) + '}';
        case ResultKind::Select:
            break;
    }
    std::string out = R"({"kind":"select","columns":[)";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i) out += ',';
        out += jsonString(result.columns[i]);
    }
    out += R"(],"rows":[)";
    for (std::size_t r = 0; r < result.rows.size(); ++r) {
        if (r) out += ',';
        out += '[';
        for (std::size_t c = 0; c < result.rows[r].size(); ++c) {
            if (c) out += ',';
            out += toJson(result.rows[r][c]);
        }
        out += ']';
    }
    out += "]}";
    return out;
}

}  // namespace ledger
