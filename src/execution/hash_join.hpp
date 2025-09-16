#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"

namespace quarry {

// HashJoinOperator is a single-column equi inner join.
//
// The right input is the build side and the left is the probe side. That choice is
// fixed here rather than decided per query: picking the smaller side belongs to the
// optimizer, which has the row estimates, and it expresses the decision by handing
// this operator its inputs in the order it wants them.
//
// Duplicate keys are a cross product of the matching groups on both sides, which
// is what SQL requires: three left rows and two right rows with the same key
// produce six output rows. NULL keys match nothing at all, on either side, because
// NULL = NULL is unknown rather than true.
class HashJoinOperator : public PhysicalOperator {
public:
    HashJoinOperator(OperatorPtr left, OperatorPtr right, size_t left_key, size_t right_key);

    const Schema& output_schema() const override { return output_schema_; }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "HashJoin"; }
    std::vector<PhysicalOperator*> Children() override { return {left_.get(), right_.get()}; }

private:
    struct RowRef {
        uint32_t batch;
        uint32_t row;
    };

    Status BuildHashTable();
    Status EmitPair(Batch& out, uint32_t left_row, const RowRef& right_row);

    OperatorPtr left_;
    OperatorPtr right_;
    size_t left_key_;
    size_t right_key_;
    Schema output_schema_;

    // promote_keys_ is set when one side's key is int64 and the other's is double.
    // Encoding both as double is the same implicit numeric conversion a comparison
    // would do, so `on a.id = b.price` behaves the same way as `where a.id = price`.
    bool promote_keys_ = false;

    std::vector<std::unique_ptr<Batch>> build_batches_;
    std::unordered_map<std::string, std::vector<RowRef>> table_;
    bool built_ = false;

    // --- probe state, which must survive across Next() calls because one left row
    // with many matches can fill several output batches ---
    Batch probe_batch_;
    size_t probe_position_ = 0;
    uint32_t current_left_row_ = 0;
    const std::vector<RowRef>* matches_ = nullptr;
    size_t match_position_ = 0;
    bool probe_done_ = false;
};

}  // namespace quarry
