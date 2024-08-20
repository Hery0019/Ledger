#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ledger {

// Categories of expected errors. These are "business" errors the caller is
// meant to handle, as opposed to bugs (which must fail loudly).
enum class ErrorCode {
    SyntaxError,          // malformed SQL
    TypeError,            // incompatible types, impossible conversion
    ConstraintViolation,  // PRIMARY KEY, NOT NULL...
    NotFound,             // unknown table / column
    AlreadyExists,        // CREATE TABLE on an existing table
    IoError,              // disk, permissions
    Corruption,           // unreadable data file
    Internal,             // broken invariant: should never happen
};

std::string_view errorCodeName(ErrorCode code) noexcept;

struct Error {
    ErrorCode code;
    std::string message;

    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

namespace detail {
[[noreturn]] inline void dieOnBadAccess(const char* what, const Error* err) {
    // Accessing the value of a failed Result is a programming bug, not an
    // expected error. No exception: stop right there with a usable message.
    std::fprintf(stderr, "ledger: fatal: %s", what);
    if (err) {
        std::fprintf(stderr, " (error: %.*s: %s)",
                     static_cast<int>(errorCodeName(err->code).size()),
                     errorCodeName(err->code).data(), err->message.c_str());
    }
    std::fputc('\n', stderr);
    std::abort();
}
}  // namespace detail

// Result<T> holds either a T or an Error. Never both, never neither.
//
// Usage:
//   Result<int> r = parseInt("42");
//   if (!r.ok()) return r.error();      // propagation
//   int v = std::move(r).value();       // extraction
//
// Calling value() on a failed Result abort()s: the design forces you to test
// ok() first. [[nodiscard]] prevents silently ignoring a returned Result.
template <typename T>
class [[nodiscard]] Result {
public:
    // Deliberately implicit: allows `return Error{...};` and `return value;`
    // in a function returning Result<T>.
    Result(T value) : data_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Error error) : data_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const& {
        if (!ok()) detail::dieOnBadAccess("value() called on failed Result", &std::get<Error>(data_));
        return std::get<T>(data_);
    }
    [[nodiscard]] T& value() & {
        if (!ok()) detail::dieOnBadAccess("value() called on failed Result", &std::get<Error>(data_));
        return std::get<T>(data_);
    }
    [[nodiscard]] T&& value() && {
        if (!ok()) detail::dieOnBadAccess("value() called on failed Result", &std::get<Error>(data_));
        return std::get<T>(std::move(data_));
    }

    [[nodiscard]] const Error& error() const& {
        if (ok()) detail::dieOnBadAccess("error() called on successful Result", nullptr);
        return std::get<Error>(data_);
    }
    [[nodiscard]] Error&& error() && {
        if (ok()) detail::dieOnBadAccess("error() called on successful Result", nullptr);
        return std::get<Error>(std::move(data_));
    }

    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return ok() ? std::get<T>(data_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for operations that produce no value.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;  // success
    Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const& {
        if (ok()) detail::dieOnBadAccess("error() called on successful Result", nullptr);
        return *error_;
    }
    [[nodiscard]] Error&& error() && {
        if (ok()) detail::dieOnBadAccess("error() called on successful Result", nullptr);
        return std::move(*error_);
    }

private:
    std::optional<Error> error_;
};

// Shorthand for building an error.
inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace ledger

// Rust-style `?` error propagation. Portable (no GNU extension).
//
//   LEDGER_TRY(v, parseInt(s));   // declares `v` of type T, or `return`s the error
//   LEDGER_TRY_VOID(writeFile());  // for Result<void>
//
// The enclosing function must return some Result<U>.
#define LEDGER_CONCAT_IMPL(a, b) a##b
#define LEDGER_CONCAT(a, b) LEDGER_CONCAT_IMPL(a, b)

#define LEDGER_TRY(name, expr)                                        \
    auto LEDGER_CONCAT(name, _result__) = (expr);                     \
    if (!LEDGER_CONCAT(name, _result__).ok())                         \
        return std::move(LEDGER_CONCAT(name, _result__)).error();     \
    auto name = std::move(LEDGER_CONCAT(name, _result__)).value()

#define LEDGER_TRY_VOID(expr)                                         \
    do {                                                              \
        auto LEDGER_CONCAT(tryv_, __LINE__) = (expr);                 \
        if (!LEDGER_CONCAT(tryv_, __LINE__).ok())                     \
            return std::move(LEDGER_CONCAT(tryv_, __LINE__)).error(); \
    } while (0)
