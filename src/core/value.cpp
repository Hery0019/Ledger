#include "core/value.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ledger {

namespace {

[[noreturn]] void badAccess(DataType requested, DataType actual) {
    std::fprintf(stderr, "ledger: fatal: Value accessed as %.*s but holds %.*s\n",
                 static_cast<int>(dataTypeName(requested).size()), dataTypeName(requested).data(),
                 static_cast<int>(dataTypeName(actual).size()), dataTypeName(actual).data());
    std::abort();
}

bool isNumeric(DataType t) noexcept { return t == DataType::Int || t == DataType::Float; }

template <typename T>
Ordering orderOf(const T& a, const T& b) noexcept {
    if (a < b) return Ordering::Less;
    if (b < a) return Ordering::Greater;
    return Ordering::Equal;
}

// Parses a number with from_chars, requiring the WHOLE string to be consumed.
template <typename T>
Result<T> parseWhole(std::string_view text, DataType type) {
    T out{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec == std::errc::result_out_of_range) {
        return makeError(ErrorCode::TypeError,
                         "value out of range for " + std::string(dataTypeName(type)) + ": '" +
                             std::string(text) + "'");
    }
    if (ec != std::errc{} || ptr != end) {
        return makeError(ErrorCode::TypeError, "invalid " + std::string(dataTypeName(type)) +
                                                   " literal: '" + std::string(text) + "'");
    }
    return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

}  // namespace

std::string_view dataTypeName(DataType type) noexcept {
    switch (type) {
        case DataType::Null:  return "NULL";
        case DataType::Int:   return "INT";
        case DataType::Float: return "FLOAT";
        case DataType::Text:  return "TEXT";
        case DataType::Bool:  return "BOOL";
        case DataType::Uuid:  return "UUID";
    }
    return "UNKNOWN";
}

// ---- construction -----------------------------------------------------------

Result<Value> Value::real(double v) {
    if (!std::isfinite(v)) {
        return makeError(ErrorCode::TypeError, "FLOAT must be finite (got NaN or Inf)");
    }
    return Value{Storage{v}};
}

// ---- inspection -------------------------------------------------------------

DataType Value::type() const noexcept {
    switch (data_.index()) {
        case 0: return DataType::Null;
        case 1: return DataType::Int;
        case 2: return DataType::Float;
        case 3: return DataType::Text;
        case 4: return DataType::Bool;
        case 5: return DataType::Uuid;
        default: return DataType::Null;  // unreachable: the variant is never valueless here
    }
}

std::int64_t Value::asInt() const {
    if (auto* p = std::get_if<std::int64_t>(&data_)) return *p;
    badAccess(DataType::Int, type());
}

double Value::asFloat() const {
    if (auto* p = std::get_if<double>(&data_)) return *p;
    badAccess(DataType::Float, type());
}

const std::string& Value::asText() const {
    if (auto* p = std::get_if<std::string>(&data_)) return *p;
    badAccess(DataType::Text, type());
}

bool Value::asBool() const {
    if (auto* p = std::get_if<bool>(&data_)) return *p;
    badAccess(DataType::Bool, type());
}

const Uuid& Value::asUuid() const {
    if (auto* p = std::get_if<Uuid>(&data_)) return *p;
    badAccess(DataType::Uuid, type());
}

// ---- text codec -------------------------------------------------------------

Result<Value> Value::fromText(DataType type, std::string_view text) {
    switch (type) {
        case DataType::Null:
            return makeError(ErrorCode::Internal, "fromText called with DataType::Null");

        case DataType::Int: {
            LEDGER_TRY(v, parseWhole<std::int64_t>(text, type));
            return Value::integer(v);
        }

        case DataType::Float: {
            LEDGER_TRY(v, parseWhole<double>(text, type));
            return Value::real(v);  // filters NaN/Inf ("nan", "inf" are accepted by from_chars)
        }

        case DataType::Text:
            return Value::text(std::string(text));

        case DataType::Bool:
            if (equalsIgnoreCase(text, "true")) return Value::boolean(true);
            if (equalsIgnoreCase(text, "false")) return Value::boolean(false);
            return makeError(ErrorCode::TypeError,
                             "invalid BOOL literal: '" + std::string(text) + "'");

        case DataType::Uuid: {
            LEDGER_TRY(v, parseUuid(text));
            return Value::uuid(v);
        }
    }
    return makeError(ErrorCode::Internal, "fromText: unknown DataType");
}

std::string Value::toText() const {
    switch (type()) {
        case DataType::Null:
            return "NULL";
        case DataType::Int:
            return std::to_string(asInt());
        case DataType::Float: {
            // to_chars without a format = shortest representation that round-trips.
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), asFloat());
            return std::string(buf, ptr);
        }
        case DataType::Text:
            return asText();
        case DataType::Bool:
            return asBool() ? "true" : "false";
        case DataType::Uuid:
            return formatUuid(asUuid());
    }
    return {};
}

// ---- comparison -------------------------------------------------------------

Result<Ordering> Value::compare(const Value& lhs, const Value& rhs) {
    const DataType lt = lhs.type();
    const DataType rt = rhs.type();

    if (lt == DataType::Null || rt == DataType::Null) return Ordering::Unknown;

    if (lt == rt) {
        switch (lt) {
            case DataType::Int:   return orderOf(lhs.asInt(), rhs.asInt());
            case DataType::Float: return orderOf(lhs.asFloat(), rhs.asFloat());
            case DataType::Text:  return orderOf(lhs.asText(), rhs.asText());
            case DataType::Bool:  return orderOf(lhs.asBool(), rhs.asBool());
            case DataType::Uuid:  return orderOf(lhs.asUuid(), rhs.asUuid());
            case DataType::Null:  break;  // already handled
        }
    }

    if (isNumeric(lt) && isNumeric(rt)) {
        const double a = lt == DataType::Int ? static_cast<double>(lhs.asInt()) : lhs.asFloat();
        const double b = rt == DataType::Int ? static_cast<double>(rhs.asInt()) : rhs.asFloat();
        return orderOf(a, b);
    }

    return makeError(ErrorCode::TypeError, "cannot compare " + std::string(dataTypeName(lt)) +
                                               " with " + std::string(dataTypeName(rt)));
}

}  // namespace ledger
