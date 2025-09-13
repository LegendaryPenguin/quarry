#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry {

// EvalResult holds an evaluated expression's values for one batch.
//
// It is either a vector the evaluator built or a borrowed reference to a column
// of the input batch. Borrowing matters: `SELECT a, b FROM t` evaluates two bare
// column references per batch, and copying a column to hand it back would double
// the memory traffic of the cheapest possible query.
//
// CONTRACT: the result is COMPACTED. It has exactly batch.selected_count()
// slots, and slot i corresponds to physical row batch.Rows()[i]. Expression
// results are therefore never indexed by a physical row id — which is the point,
// because an evaluator that returned physically-indexed results would invite
// exactly the dense 0..row_count() loop that silently resurrects filtered rows.
class EvalResult {
public:
    EvalResult() = default;

    static EvalResult Borrowed(const Vector& v) {
        EvalResult r;
        r.borrowed_ = &v;
        return r;
    }
    static EvalResult Owned(std::unique_ptr<Vector> v) {
        EvalResult r;
        r.owned_ = std::move(v);
        return r;
    }

    const Vector& vector() const { return owned_ ? *owned_ : *borrowed_; }
    size_t size() const { return vector().size(); }
    TypeId type() const { return vector().type(); }

private:
    const Vector* borrowed_ = nullptr;
    std::unique_ptr<Vector> owned_;
};

// ExpressionEvaluator evaluates an ast::Expression over a batch, column at a
// time.
//
// The schema is held by value rather than by reference because an evaluator
// outlives the plan node that configured it in no case that matters, but a
// dangling schema reference would be invisible until a column name lookup
// returned garbage.
class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(Schema input) : input_(std::move(input)) {}

    const Schema& input_schema() const { return input_; }

    // ResultType reports the type an expression produces without evaluating it,
    // so a projection's output schema is known before the first batch arrives.
    // kInvalid means "the untyped NULL literal", which adopts the type of
    // whatever it is combined with.
    Result<TypeId> ResultType(const ast::Expression& expr) const;

    Result<EvalResult> Evaluate(const ast::Expression& expr, const Batch& batch) const;

    // Select returns the PHYSICAL row indexes for which `expr` is TRUE, drawn
    // only from the rows already live in `batch`. Rows where the expression is
    // FALSE or UNKNOWN are excluded, which is what makes a comparison against
    // NULL drop a row rather than keep it.
    Result<std::vector<uint32_t>> Select(const ast::Expression& expr, const Batch& batch) const;

    // OutputName is the column name a projection gives an expression: its alias,
    // else the column name for a bare reference, else the expression text.
    static std::string OutputName(const ast::Expression& expr);

private:
    Schema input_;
};

// ApplyPredicate intersects a pushed predicate with a batch's existing
// selection. Separate from the expression evaluator because a Predicate is
// already in column-op-constant form and needs no tree walk — this is the kernel
// a scan runs.
Status ApplyPredicate(Batch& batch, const Predicate& pred);

}  // namespace quarry
