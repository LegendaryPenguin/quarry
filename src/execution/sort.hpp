#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"

namespace quarry {

// SortOperator buffers its entire input, orders it, and streams the result back out.
//
// Sorting is blocking by nature — the first output row is not knowable until the
// last input row has been seen — so this is the one operator that holds the whole
// result set. It holds it as a list of batches and sorts a list of (batch, row)
// references rather than moving row payloads, so a wide table's strings are copied
// once on the way in and never again during the sort itself.
//
// NULL ORDERING: a NULL sorts as if it were smaller than every non-null value.
// ASC therefore puts nulls first and DESC puts them last, so reversing the
// direction reverses the whole order, nulls included. The SQL standard leaves this
// implementation-defined and the dialects disagree — PostgreSQL's default is the
// opposite — so the choice follows SQLite, which is what tests/differential_test
// compares this engine against. It is stated here and asserted in the tests rather
// than left to be discovered.
//
// Equal keys keep their input order: the sort is stable, so `ORDER BY a` over rows
// that tie on `a` is reproducible instead of depending on the sort's internals.
class SortOperator : public PhysicalOperator {
public:
    SortOperator(OperatorPtr child, std::vector<SortKey> keys);

    const Schema& output_schema() const override { return child_->output_schema(); }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "Sort"; }
    std::vector<PhysicalOperator*> Children() override { return {child_.get()}; }

private:
    // RowRef points at a buffered row. Two 32-bit fields rather than a pointer pair
    // because the sort moves these around and a smaller element means more of the
    // index array in cache.
    struct RowRef {
        uint32_t batch;
        uint32_t row;
    };

    Status BufferInput();
    bool Less(const RowRef& a, const RowRef& b) const;

    OperatorPtr child_;
    std::vector<SortKey> keys_;

    Batch input_;
    std::vector<std::unique_ptr<Batch>> buffered_;
    std::vector<RowRef> order_;

    bool sorted_ = false;
    size_t emit_position_ = 0;
};

}  // namespace quarry
