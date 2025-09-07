#include "quarry/storage.hpp"

#include <cmath>

namespace quarry {
namespace {

// The engine's only implicit conversion is int64 against double, so a predicate
// whose constant is of an unrelated type tells the zone map nothing and must not
// be allowed to prune. Returning "not comparable" here is what keeps a varchar
// bound from being measured against an integer literal.
bool Comparable(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return false;
    if (a.type() == b.type()) return true;
    return a.IsNumeric() && b.IsNumeric();
}

// A NaN bound would make every ordered test below false, which reads as "prune".
// The writer keeps NaNs out of min and max for exactly this reason; this check
// exists because stats can also arrive from a file written by another version.
bool UsableBound(const Value& v) {
    if (v.is_null()) return false;
    if (v.type() == TypeId::kDouble && std::isnan(v.as_double())) return false;
    return true;
}

bool LessOrEqual(const Value& a, const Value& b) { return a < b || a == b; }

}  // namespace

bool CanMatch(const ColumnStats& stats, CompareOp op, const Value& constant) {
    // An all-null chunk has no row that can satisfy any comparison: every test
    // against NULL is unknown, and unknown rows are not returned. This is the one
    // case where a chunk containing nulls is safely pruned, and it is safe only
    // because there is nothing else in it. min and max are meaningless here and
    // are deliberately not consulted.
    if (stats.all_null) return false;

    // A NULL constant makes every comparison unknown, so nothing matches. Pruning
    // would be defensible, but a NULL on the right of a comparison means the
    // planner handed down something it should have folded away, and guessing at
    // intent in the one place where a wrong guess silently drops rows is a bad
    // trade. Scanning a few chunks is cheaper than being wrong.
    if (constant.is_null()) return true;

    // A chunk that contains any null is never pruned on a comparison.
    //
    // The bounds here are computed over the non-null values only, so pruning on
    // them would in fact be sound for the non-null rows — but it depends on that
    // invariant holding in every writer that ever produces these stats, forever,
    // including ones that compute min/max before nulls are known. The cost of the
    // conservative rule is reading chunks that could have been skipped on a
    // nullable column; the cost of the aggressive one is a query that silently
    // returns too few rows. Only one of those is recoverable.
    if (stats.null_count > 0) return true;

    if (!UsableBound(stats.min) || !UsableBound(stats.max)) return true;
    if (!Comparable(stats.min, constant) || !Comparable(stats.max, constant)) return true;

    switch (op) {
        case CompareOp::kEq:
            // Satisfiable only if the constant falls inside the closed window.
            return LessOrEqual(stats.min, constant) && LessOrEqual(constant, stats.max);

        case CompareOp::kNe:
            // Unsatisfiable only when every value in the chunk is the constant,
            // which the stats can prove only when the window has collapsed to a
            // point equal to it. Any wider window contains something else.
            return !(stats.min == stats.max && stats.min == constant);

        case CompareOp::kLt:
            return stats.min < constant;

        case CompareOp::kLe:
            return LessOrEqual(stats.min, constant);

        case CompareOp::kGt:
            return constant < stats.max;

        case CompareOp::kGe:
            return LessOrEqual(constant, stats.max);
    }

    // An operator this build does not know about must not be pruned on.
    return true;
}

}  // namespace quarry
