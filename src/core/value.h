#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "core/result.h"
#include "core/uuid.h"

namespace ledger {

enum class DataType {
    Null,   // type of the NULL value alone; never a column type
    Int,    // signed 64-bit integer
    Float,  // IEEE 754 double, finite (NaN/Inf rejected at construction)
    Text,   // byte string, bytewise comparison (no collation)
    Bool,
    Uuid,   // 128-bit RFC 4122 UUID, bytewise order (see core/uuid.h)
};

std::string_view dataTypeName(DataType type) noexcept;

// Result of a three-valued SQL comparison.
// Unknown: one of the operands is NULL. Neither true nor false — a WHERE that
// evaluates to Unknown rejects the row, but NOT Unknown is still Unknown.
enum class Ordering { Less, Equal, Greater, Unknown };

class Value {
public:
    // Named constructors: avoid the int/bool/double ambiguity of literals.
    static Value null() noexcept { return Value{std::monostate{}}; }
    static Value integer(std::int64_t v) noexcept { return Value{v}; }
    static Value text(std::string v) noexcept { return Value{std::move(v)}; }
    static Value boolean(bool v) noexcept { return Value{v}; }
    static Value uuid(Uuid v) noexcept { return Value{v}; }
    // Rejects NaN and ±Inf: they have no coherent SQL semantics and would make
    // a total order impossible.
    static Result<Value> real(double v);

    [[nodiscard]] DataType type() const noexcept;
    [[nodiscard]] bool isNull() const noexcept { return type() == DataType::Null; }

    // Typed access. Precondition: type() matches. Otherwise abort() (a bug,
    // not an expected error: the semantic layer must have checked before).
    [[nodiscard]] std::int64_t asInt() const;
    [[nodiscard]] double asFloat() const;
    [[nodiscard]] const std::string& asText() const;
    [[nodiscard]] bool asBool() const;
    [[nodiscard]] const Uuid& asUuid() const;

    // Text codec at the type level. Reversible: fromText(t, v.toText()) == v
    // for every non-NULL value. Encoding NULL and escaping separators are the
    // storage layer's responsibility, not Value's.
    //
    // fromText(DataType::Null, _) is an error: NULL is built with null().
    static Result<Value> fromText(DataType type, std::string_view text);
    [[nodiscard]] std::string toText() const;

    // SQL comparison.
    //  - NULL with anything          -> Unknown
    //  - Int vs Float                -> numeric comparison (see note below)
    //  - incompatible types          -> TypeError (never a silent false)
    //  - Text: bytewise order
    //  - Bool: false < true
    //
    // Int/Float note: an Int outside [-2^53, 2^53] converted to double loses
    // precision; the comparison may then be inexact. Accepted in v1.
    [[nodiscard]] static Result<Ordering> compare(const Value& lhs, const Value& rhs);

    // Strict structural equality (same type, same content). NULL == NULL here.
    // Useful for tests and primary-key uniqueness, NOT for WHERE.
    [[nodiscard]] bool operator==(const Value& other) const noexcept { return data_ == other.data_; }

private:
    using Storage = std::variant<std::monostate, std::int64_t, double, std::string, bool, Uuid>;
    explicit Value(Storage s) noexcept : data_(std::move(s)) {}

    Storage data_;
};

}  // namespace ledger
