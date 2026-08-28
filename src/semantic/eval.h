#pragma once

#include <vector>

#include "core/result.h"
#include "core/row.h"
#include "semantic/bound.h"

namespace ledger {

// Rows produced by the enclosing SELECT's subqueries, indexed by slot (see
// BoundSelect::subqueries). Filled by the executor before evaluating rows.
using SubqueryRows = std::vector<std::vector<Row>>;

// Evaluates a bound expression on a row. The single implementation of
// operator semantics: used by the binder (constant folding) and by the
// executor (row by row).
//
// Precondition: the expression was produced by the binder, so the types are
// compatible. The only possible errors depend on the data:
//  - division by zero                       -> TypeError
//  - Int overflow (+ - * / negation)        -> TypeError, never a wrap
//  - non-finite Float result                -> TypeError (via Value::real)
//  - a scalar subquery with several rows    -> TypeError
//  - a subquery node with no `subs` table   -> Internal (executor bug)
//
// NULL semantics (SQL three-valued logic):
//  - arithmetic, comparison, negation: NULL if an operand is NULL;
//  - AND: false if an operand is false, else NULL if one is NULL, else true;
//  - OR: true if an operand is true, else NULL if one is NULL, else false;
//  - NOT NULL = NULL; x IS NULL is always a real boolean;
//  - x IN (...) : NULL if x is NULL, or if nothing matches and an item is NULL;
//  - x LIKE p   : NULL if either side is NULL;
//  - a scalar subquery with no row is NULL; EXISTS is never NULL.
Result<Value> eval(const BoundExpr& expr, const Row& row, const SubqueryRows* subs = nullptr);

// True if the expression references no column and no subquery (so it can be
// evaluated without a row, at bind time).
[[nodiscard]] bool isConstant(const BoundExpr& expr) noexcept;

// SQL LIKE matching (`%`, `_`), exposed for tests.
[[nodiscard]] bool likeMatch(std::string_view text, std::string_view pattern);

}  // namespace ledger
