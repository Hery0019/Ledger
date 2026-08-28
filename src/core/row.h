#pragma once

#include <vector>

#include "core/value.h"

namespace ledger {

// A row = one Value per column, in the table's schema order.
using Row = std::vector<Value>;

}  // namespace ledger
