#pragma once

#include <string_view>

#include "core/result.h"
#include "sql/ast.h"

namespace ledger {

// Parses a complete SQL statement (lexer + parser) and produces its AST.
//
//  - CREATE VIEW name AS SELECT ... keeps the SELECT text verbatim; ORDER BY
//    and LIMIT are refused inside a view;
//  - one statement per call; the trailing `;` is optional, anything after it
//    is an error (the CLI takes care of splitting multi-statement input);
//  - every error is a positioned SyntaxError `line:col: expected X, got Y`;
//    we stop at the first one, no recovery;
//  - literals are converted to Value here (Value::fromText), so an
//    out-of-range integer is reported with its position;
//  - `-5` stays Unary(Neg, 5): constant folding is the binder's job.
//
// Expression precedence, weakest to strongest:
//   OR  <  AND  <  NOT  <  comparison (= <> < <= > >=, IS [NOT] NULL;
//   non-associative: `a = b = c` is rejected)  <  + -  <  * /  <  unary -.
Result<ast::Statement> parse(std::string_view sql);

}  // namespace ledger
