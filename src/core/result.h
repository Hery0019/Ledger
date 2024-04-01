#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ledger {

// Catégories d'erreurs attendues. Ce sont des erreurs "métier" que l'appelant
// est censé gérer, par opposition aux bugs (qui doivent faire échouer bruyamment).
enum class ErrorCode {
    SyntaxError,          // SQL mal formé
    TypeError,            // types incompatibles, conversion impossible
    ConstraintViolation,  // PRIMARY KEY, NOT NULL...
    NotFound,             // table / colonne inexistante
    AlreadyExists,        // CREATE TABLE sur une table existante
    IoError,              // disque, permissions
    Corruption,           // fichier de données illisible
    Internal,             // invariant violé : ne devrait jamais arriver
};

std::string_view errorCodeName(ErrorCode code) noexcept;

struct Error {
    ErrorCode code;
    std::string message;

    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

namespace detail {
[[noreturn]] inline void dieOnBadAccess(const char* what, const Error* err) {
    // Accéder à la valeur d'un Result en erreur est un bug de programmation,
    // pas une erreur attendue. On ne lance pas d'exception : on arrête net,
    // avec un message exploitable.
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

// Result<T> contient soit un T, soit une Error. Jamais les deux, jamais aucun.
//
// Usage :
//   Result<int> r = parseInt("42");
//   if (!r.ok()) return r.error();      // propagation
//   int v = std::move(r).value();       // extraction
//
// L'accès à value() sur un Result en erreur abort() : le design impose
// de tester ok() avant. [[nodiscard]] empêche d'ignorer silencieusement
// un Result retourné.
template <typename T>
class [[nodiscard]] Result {
public:
    // Implicite volontairement : permet `return Error{...};` et `return value;`
    // dans une fonction qui retourne Result<T>.
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

// Spécialisation pour les opérations qui ne produisent pas de valeur.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;  // succès
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

// Raccourci de construction d'erreur.
inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace ledger

// Propagation d'erreur à la Rust `?`. Portable (pas d'extension GNU).
//
//   LEDGER_TRY(v, parseInt(s));   // déclare `v` de type T, ou `return` l'erreur
//   LEDGER_TRY_VOID(writeFile());  // pour Result<void>
//
// La fonction englobante doit retourner un Result<U> quelconque.
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
