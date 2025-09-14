#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/catalog.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"

namespace quarry {

// SequentialScanOperator reads a table's segments in order, skipping the ones a
// predicate proves empty.
//
// Segment pruning is the reason this operator is not a loop over rows. Its
// per-segment decision is made from statistics alone, so a selective query over
// many segments reads bytes proportional to the matching segments rather than to
// the table. stats().segments_pruned versus segments_scanned is how a test or an
// EXPLAIN ANALYZE proves that actually happened, rather than taking the layout's
// word for it.
class SequentialScanOperator : public PhysicalOperator {
public:
    // ReaderFactory opens a reader over one segment, restricted to the projected
    // columns.
    //
    // Injected rather than constructed here so that the scan does not depend on a
    // particular on-disk format: the planner passes the real segment reader, and a
    // test passes an in-memory one. That is also what keeps this operator
    // testable before any storage format is final.
    using ReaderFactory = std::function<Result<std::unique_ptr<SegmentReader>>(
        const SegmentMeta& segment, const std::vector<size_t>& projection)>;

    // `projection` is a list of column indexes into `table_schema`. `predicates`
    // name columns by TABLE index as well, and every predicated column must also
    // appear in the projection — a predicate cannot be evaluated on a column the
    // scan was told not to read.
    SequentialScanOperator(Schema table_schema, std::vector<SegmentMeta> segments,
                           std::vector<size_t> projection, std::vector<Predicate> predicates,
                           ReaderFactory factory);

    SequentialScanOperator(const TableDef& table, std::vector<size_t> projection,
                           std::vector<Predicate> predicates, ReaderFactory factory);

    const Schema& output_schema() const override { return output_schema_; }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "SeqScan"; }

private:
    // AdvanceSegment opens the next segment that statistics cannot rule out,
    // counting the ones it skips. Returns false when the segments are exhausted.
    Result<bool> AdvanceSegment();

    // CanSegmentMatch is the zone-map test for all pushed predicates at once.
    bool CanSegmentMatch(const SegmentMeta& segment) const;

    Schema table_schema_;
    std::vector<SegmentMeta> segments_;
    std::vector<size_t> projection_;
    std::vector<Predicate> predicates_;
    ReaderFactory factory_;

    Schema output_schema_;

    // Predicates rewritten to name columns by their position in the output batch,
    // since the reader hands back only projected columns.
    std::vector<Predicate> batch_predicates_;

    size_t next_segment_ = 0;
    std::unique_ptr<SegmentReader> reader_;
    bool opened_ = false;
    bool exhausted_ = false;
};

}  // namespace quarry
