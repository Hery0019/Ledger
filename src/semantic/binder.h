#pragma once

#include "core/result.h"
#include "semantic/bound.h"
#include "semantic/catalog.h"
#include "sql/ast.h"

namespace ledger {

// Checks and resolves a statement against the catalog. Modifies neither the
// AST nor the catalog (a bound CREATE/DROP is applied to the catalog only
// after successful execution, by the executor).
//
// Errors:
//  - NotFound            unknown table or column
//  - AlreadyExists       CREATE TABLE on an existing table
//  - TypeError           operand or value of an incompatible type
//  - ConstraintViolation NULL into a NOT NULL / PRIMARY KEY column
//  - SyntaxError         invalid shape the parser cannot detect (duplicate
//                        column, several PRIMARY KEYs, wrong value count,
//                        column reference inside VALUES)
// Errors coming from an expression are prefixed `line:col: `.
//
// Typing rules:
//  - + - * /  : Int×Int -> Int; numeric×numeric -> Float; otherwise TypeError
//  - = <> < <= > >= : numerics together, Text×Text, Bool×Bool -> Bool
//  - AND OR NOT : Bool only (no "1 is true")
//  - NULL literal: compatible with everything; the result has type Null if
//    both operands do, otherwise the other operand's type
//  - WHERE: Bool or Null
//  - INSERT / UPDATE: same type as the column, or Int into Float (cast
//    inserted). No Text <-> number conversion.
//  - every sub-expression without a column is folded to a constant (a data
//    error, e.g. 1/0, is then reported at bind time).
Result<BoundStatement> bind(const ast::Statement& stmt, const Catalog& catalog);

}  // namespace ledger
