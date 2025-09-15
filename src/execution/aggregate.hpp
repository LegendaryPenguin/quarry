#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry {

// HashAggregateOperator groups rows by a key and folds each group's rows into
// accumulators.
//
// Hashing rather than sorting, because a hash aggregate is one pass over the input
// and a sort aggregate is O(n log n) plus a sort buffer, and grouping does not need
// the ordering a sort would pay for. The cost is that groups come out in an
// arbitrary order — insertion order here, which is arbitrary but at least
// reproducible, so tests and EXPLAIN output do not change run to run.
//
// This operator is blocking: no group's value is final until the last row has been
// seen, so the first Next() consumes the whole child.
class HashAggregateOperator : public PhysicalOperator {
public:
    // `group_by` indexes columns of the child's output schema. An empty list is a
    // global aggregate, which still produces exactly one row — including over
    // empty input, where SQL requires COUNT to be 0 rather than no row at all.
    HashAggregateOperator(OperatorPtr child, std::vector<size_t> group_by,
                          std::vector<Aggregate> aggregates);

    const Schema& output_schema() const override { return output_schema_; }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "HashAggregate"; }
    std::vector<PhysicalOperator*> Children() override { return {child_.get()}; }

private:
    // Accumulator is the running state for one aggregate within one group.
    //
    // All five aggregates share one struct rather than having a class each. The
    // state is three scalars and two Values; a virtual call per row per aggregate
    // would cost more than the unused fields do.
    struct Accumulator {
        // count is the number of values folded in: rows for COUNT(*), non-null
        // values for everything else. It is what makes an all-null group
        // distinguishable from a group with a zero sum.
        int64_t count = 0;
        int64_t int_sum = 0;
        double double_sum = 0.0;
        Value min;
        Value max;
    };

    struct Group {
        std::vector<Value> key;
        std::vector<Accumulator> accumulators;
    };

    Status BuildGroups();
    Status Accumulate(const Batch& batch, const std::vector<size_t>& group_ids);
    Status EmitGroup(const Group& group, Batch& out);

    OperatorPtr child_;
    std::vector<size_t> group_by_;
    std::vector<Aggregate> aggregates_;
    Schema output_schema_;

    Batch input_;
    std::vector<Group> groups_;
    std::unordered_map<std::string, size_t> group_index_;

    bool built_ = false;
    size_t emit_position_ = 0;
};

}  // namespace quarry
