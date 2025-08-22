#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"

namespace quarry {

// PhysicalOperator is one node in an executing query plan.
//
// Execution is pull-based and vectorized: the root is asked for a batch, and each
// operator asks its child for one. Pull was chosen over push because it makes
// LIMIT genuinely cheap — the root stops asking and no work happens upstream —
// and because a pull operator's state is just its own, which is far easier to
// reason about than a push pipeline's.
//
// The contract:
//   - Open() once before any Next().
//   - Next(out) fills `out` and sets `done`. A batch with zero selected rows and
//     done = false is legal and means "nothing survived this batch, ask again" —
//     a filter must be allowed to reject a whole batch without that meaning end
//     of stream.
//   - done = true implies an EMPTY output batch. The looser alternative, letting
//     a final batch of rows arrive alongside done, is a standing trap: every
//     consumer has to remember to drain it, and the one that forgets silently
//     loses the last rows of every query. Operators that have rows left report
//     them with done = false and signal exhaustion on the following call.
//   - Close() once, and it must be safe after a failed Open or a partial scan,
//     because LIMIT closes operators mid-stream.
class PhysicalOperator {
public:
    virtual ~PhysicalOperator() = default;

    // output_schema is the shape of the batches this operator produces. Known
    // before execution, since the planner uses it to size buffers.
    virtual const Schema& output_schema() const = 0;

    virtual Status Open() = 0;
    virtual Status Next(Batch& out, bool& done) = 0;
    virtual Status Close() = 0;

    // Name identifies the operator in EXPLAIN output and in profiles.
    virtual std::string Name() const = 0;

    // Children lets EXPLAIN and the profiler walk the tree without each operator
    // having to implement traversal.
    virtual std::vector<PhysicalOperator*> Children() { return {}; }

    // --- profiling counters, filled by the operators themselves ---
    // Kept on the base class so a profile covers every operator uniformly rather
    // than only the ones that remembered to instrument themselves.
    struct Stats {
        uint64_t batches = 0;
        uint64_t rows_in = 0;
        uint64_t rows_out = 0;
        uint64_t nanos = 0;

        // segments_pruned is only meaningful on a scan, but living here means
        // EXPLAIN ANALYZE prints one table rather than special-casing scans.
        uint64_t segments_scanned = 0;
        uint64_t segments_pruned = 0;
    };

    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

protected:
    Stats stats_;
};

using OperatorPtr = std::unique_ptr<PhysicalOperator>;

// Predicate is a comparison of one column against a constant.
//
// This is deliberately narrower than a general expression tree. A predicate in
// this shape can be pushed into a scan, checked against a zone map, and answered
// by an index — all three of which stop being possible the moment the left side
// can be an arbitrary expression. Anything more complex stays in a Filter
// operator above the scan, and the optimizer's job is to split a WHERE clause
// into the part that fits here and the part that does not.
struct Predicate {
    size_t column_index = 0;
    CompareOp op = CompareOp::kEq;
    Value constant;

    std::string ToString(const Schema& schema) const;
};

// AggregateKind is the supported aggregate set.
enum class AggregateKind : uint8_t {
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
};

const char* AggregateKindName(AggregateKind k);

struct Aggregate {
    AggregateKind kind = AggregateKind::kCount;

    // column_index is unused for COUNT(*), which is why it is optional rather
    // than a sentinel: a sentinel here invites an out-of-range read.
    std::optional<size_t> column_index;

    std::string output_name;

    // ResultType is the type the aggregate produces, which is not always the
    // input type: COUNT is always int64, AVG is always double, and SUM over an
    // int64 column stays int64 (and can therefore overflow, which is documented
    // rather than checked).
    TypeId ResultType(const Schema& input) const;
};

struct SortKey {
    size_t column_index = 0;
    bool descending = false;
};

}  // namespace quarry
