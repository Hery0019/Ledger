#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "core/result.h"

namespace sqltxt {

enum class DataType {
    Null,   // type de la valeur NULL seule ; jamais un type de colonne
    Int,    // entier signé 64 bits
    Float,  // double IEEE 754, fini (NaN/Inf rejetés à la construction)
    Text,   // chaîne d'octets, comparaison bytewise (pas de collation)
    Bool,
};

std::string_view dataTypeName(DataType type) noexcept;

// Résultat d'une comparaison SQL à trois états.
// Unknown : l'un des opérandes est NULL. Ni vrai, ni faux — un WHERE qui
// évalue à Unknown rejette la ligne, mais NOT Unknown reste Unknown.
enum class Ordering { Less, Equal, Greater, Unknown };

class Value {
public:
    // Constructeurs nommés : évitent l'ambiguïté int/bool/double des littéraux.
    static Value null() noexcept { return Value{std::monostate{}}; }
    static Value integer(std::int64_t v) noexcept { return Value{v}; }
    static Value text(std::string v) noexcept { return Value{std::move(v)}; }
    static Value boolean(bool v) noexcept { return Value{v}; }
    // Rejette NaN et ±Inf : ils n'ont pas de sémantique SQL cohérente et
    // rendraient l'ordre total impossible.
    static Result<Value> real(double v);

    [[nodiscard]] DataType type() const noexcept;
    [[nodiscard]] bool isNull() const noexcept { return type() == DataType::Null; }

    // Accès typé. Précondition : type() correspond. Sinon abort() (bug, pas
    // erreur attendue : la couche sémantique doit avoir vérifié avant).
    [[nodiscard]] std::int64_t asInt() const;
    [[nodiscard]] double asFloat() const;
    [[nodiscard]] const std::string& asText() const;
    [[nodiscard]] bool asBool() const;

    // Codec texte au niveau du type. Réversible : fromText(t, v.toText()) == v
    // pour toute valeur non NULL. L'encodage de NULL et l'échappement des
    // séparateurs sont la responsabilité de la couche stockage, pas de Value.
    //
    // fromText(DataType::Null, _) est une erreur : NULL se construit via null().
    static Result<Value> fromText(DataType type, std::string_view text);
    [[nodiscard]] std::string toText() const;

    // Comparaison SQL.
    //  - NULL avec quoi que ce soit  -> Unknown
    //  - Int vs Float                -> comparaison numérique (voir note ci-dessous)
    //  - types incompatibles         -> TypeError (jamais un false silencieux)
    //  - Text : ordre bytewise
    //  - Bool : false < true
    //
    // Note Int/Float : un Int hors de [-2^53, 2^53] converti en double perd
    // de la précision ; la comparaison peut alors être inexacte. Assumé en v1.
    [[nodiscard]] static Result<Ordering> compare(const Value& lhs, const Value& rhs);

    // Égalité structurelle stricte (même type, même contenu). NULL == NULL ici.
    // Utile pour les tests et l'unicité de clé primaire, PAS pour WHERE.
    [[nodiscard]] bool operator==(const Value& other) const noexcept { return data_ == other.data_; }

private:
    using Storage = std::variant<std::monostate, std::int64_t, double, std::string, bool>;
    explicit Value(Storage s) noexcept : data_(std::move(s)) {}

    Storage data_;
};

}  // namespace sqltxt
