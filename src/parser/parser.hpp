#pragma once

#include <string_view>

#include "quarry/ast.hpp"
#include "quarry/status.hpp"

namespace quarry::parser {

// kMaxExpressionDepth bounds the recursion a single expression can drive.
//
// A hand-written recursive-descent parser turns input nesting straight into
// stack frames, so untrusted text would otherwise be a stack-overflow vector:
// `((((...1...))))` with a few hundred thousand parens crashes the process
// before any semantic check runs. The limit is a parse error, not an abort, and
// is far above anything a person writes by hand.
inline constexpr int kMaxExpressionDepth = 64;

// Parse turns one SQL statement into an AST. An optional trailing semicolon is
// accepted; anything after it is an error, because silently ignoring trailing
// text would make a typo look like it worked.
//
// Failures are Status::SyntaxError and name what was found, what was expected,
// and the line and column. Nothing here throws.
Result<ast::Statement> Parse(std::string_view sql);

// ParseExpressionText parses a bare expression with no surrounding statement.
// Exposed because expression precedence is the part of the grammar most worth
// testing directly, without a SELECT wrapped around every case.
Result<ast::ExprPtr> ParseExpressionText(std::string_view text);

}  // namespace quarry::parser
