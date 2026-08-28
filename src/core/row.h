#pragma once

#include <vector>

#include "core/value.h"

namespace ledger {

// Une ligne = une Value par colonne, dans l'ordre du schéma de la table.
using Row = std::vector<Value>;

}  // namespace ledger
