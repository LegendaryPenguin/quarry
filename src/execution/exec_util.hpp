#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry {

// Helpers shared by the execution operators.
//
// They live together because each one encodes a decision that every operator has
// to make the same way — how an output batch is prepared, how a row is copied,
// how a group key becomes bytes. Three operators making that decision
// independently is three chances to disagree.

// A convention these operators all follow, which the PhysicalOperator contract
// permits but does not require: when Next() reports done = true, the output batch
// is left with zero selected rows. The contract allows done to arrive alongside a
// final batch of rows, and that is a standing invitation for a consumer to check
// done first and silently drop them. Clearing instead costs one extra Next() call
// per stream and makes "done means nothing to read" true everywhere.
//
// PrepareOutput readies a caller-supplied batch for one Next() call.
//
// Operators are handed the same Batch on every call so that a long scan does not
// allocate per batch, but an operator cannot assume the batch arrives already
// shaped: the first call usually receives a default-constructed one. This adds
// the columns when they are missing and clears the values when they are not.
Status PrepareOutput(Batch& out, const Schema& schema);

// CopyRow appends one physical row of `src` to `dst`, preserving nullness.
//
// Appending rather than scattering is what makes materializing operators
// (project, sort, join) able to build a compacted batch: the destination index is
// implied by append order, so the caller never has to track two cursors.
Status CopyRow(Vector& dst, const Vector& src, size_t row);

// AppendValue appends a scalar to a vector, converting between the two numeric
// types where needed. Used where a result is naturally a Value rather than a
// column — aggregate outputs and group keys — and nowhere in a per-row kernel.
Status AppendValue(Vector& dst, const Value& value);

// EncodeKeyInto appends one column value to a byte-comparable key buffer.
//
// Grouping and hash join both need a key that can live in an unordered_map, and
// both need NULL to be a distinct key that equals other NULLs — which Value's
// operator== deliberately refuses to do. Encoding sidesteps that by making
// nullness a leading byte rather than a comparison rule.
//
// `promote_numeric` writes an int64 column as a double so that an int64 key can
// join against a double key, which is the one implicit conversion the engine
// allows anywhere else.
void EncodeKeyInto(std::string& key, const Vector& vec, size_t row, bool promote_numeric);

// CompareRows orders two rows of the same-typed column.
//
// Returns negative, zero or positive. NULL sorts as if smaller than every
// non-null value — see SortOperator for why that direction was chosen.
int CompareRows(const Vector& a, size_t ra, const Vector& b, size_t rb);

// EvalCompare answers one comparison between a column slot and a constant,
// following SQL three-valued logic: `unknown` is set when either side is NULL and
// the boolean return is then meaningless.
bool EvalCompare(const Vector& vec, size_t row, CompareOp op, const Value& constant,
                 bool& unknown);

}  // namespace quarry
