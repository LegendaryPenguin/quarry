#include "scan.hpp"

#include <chrono>

#include "exec_util.hpp"
#include "expression.hpp"

namespace quarry {

SequentialScanOperator::SequentialScanOperator(Schema table_schema,
                                               std::vector<SegmentMeta> segments,
                                               std::vector<size_t> projection,
                                               std::vector<Predicate> predicates,
                                               ReaderFactory factory)
    : table_schema_(std::move(table_schema)),
      segments_(std::move(segments)),
      projection_(std::move(projection)),
      predicates_(std::move(predicates)),
      factory_(std::move(factory)) {
    if (projection_.empty()) {
        // An empty projection would mean reading no columns at all, which only a
        // COUNT(*) wants and which the planner expresses as a one-column
        // projection instead. Defaulting to every column keeps callers that do not
        // care from having to build the identity list.
        for (size_t i = 0; i < table_schema_.size(); ++i) projection_.push_back(i);
    }
    output_schema_ = table_schema_.Project(projection_);
}

SequentialScanOperator::SequentialScanOperator(const TableDef& table,
                                               std::vector<size_t> projection,
                                               std::vector<Predicate> predicates,
                                               ReaderFactory factory)
    : SequentialScanOperator(table.schema, table.segments, std::move(projection),
                             std::move(predicates), std::move(factory)) {}

Status SequentialScanOperator::Open() {
    if (!factory_) return Status::InvalidArgument("scan has no segment reader factory");

    // Pushed predicates name table columns, but the reader hands back only the
    // projected ones, so each predicate is rewritten to the position it will
    // occupy in the batch. Doing it once here rather than per batch also catches
    // the planner bug of pushing a predicate on a column it pruned away.
    batch_predicates_.clear();
    for (const Predicate& p : predicates_) {
        bool found = false;
        for (size_t i = 0; i < projection_.size(); ++i) {
            if (projection_[i] == p.column_index) {
                Predicate rewritten = p;
                rewritten.column_index = i;
                batch_predicates_.push_back(std::move(rewritten));
                found = true;
                break;
            }
        }
        if (!found) {
            return Status::InvalidArgument("predicate on column " +
                                           std::to_string(p.column_index) +
                                           " which the scan's projection does not read");
        }
    }

    next_segment_ = 0;
    exhausted_ = false;
    opened_ = true;
    return Status::Ok();
}

bool SequentialScanOperator::CanSegmentMatch(const SegmentMeta& segment) const {
    for (const Predicate& p : predicates_) {
        if (p.column_index >= segment.columns.size()) {
            // No statistics for that column means no basis to prune. Guessing
            // would risk dropping rows, which is the one failure mode pruning must
            // never have.
            continue;
        }
        if (!CanMatch(segment.columns[p.column_index].stats, p.op, p.constant)) return false;
    }
    return true;
}

Result<bool> SequentialScanOperator::AdvanceSegment() {
    while (next_segment_ < segments_.size()) {
        const SegmentMeta& meta = segments_[next_segment_++];
        if (!CanSegmentMatch(meta)) {
            ++stats_.segments_pruned;
            continue;
        }
        auto reader = factory_(meta, projection_);
        if (!reader.ok()) return reader.status();
        reader_ = reader.take();
        if (!reader_) return Status::Internal("segment reader factory returned null");
        QUARRY_TRY(reader_->Open());
        ++stats_.segments_scanned;
        return true;
    }
    return false;
}

Status SequentialScanOperator::Next(Batch& out, bool& done) {
    done = false;
    if (!opened_) return Status::Internal("Next() called before Open()");
    if (exhausted_) {
        // Clearing is what makes done = true mean "nothing to read": the batch may
        // still hold the rows returned by the previous call.
        out.Reset();
        done = true;
        return Status::Ok();
    }
    const auto started = std::chrono::steady_clock::now();

    while (true) {
        if (!reader_) {
            auto more = AdvanceSegment();
            if (!more.ok()) return more.status();
            if (!more.value()) {
                exhausted_ = true;
                out.Reset();
                done = true;
                stats_.nanos += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count());
                return Status::Ok();
            }
        }

        QUARRY_TRY(PrepareOutput(out, output_schema_));

        bool segment_done = false;
        QUARRY_TRY(reader_->Next(out, segment_done));

        if (segment_done) {
            // A reader may signal the end either with a final batch of rows or
            // with an empty one, so both are handled: release it now and let the
            // loop move to the next segment either way.
            QUARRY_TRY(reader_->Close());
            reader_.reset();
        }
        if (out.row_count() == 0) continue;

        for (const Predicate& p : batch_predicates_) {
            QUARRY_TRY(ApplyPredicate(out, p));
        }

        ++stats_.batches;
        stats_.rows_in += out.row_count();
        stats_.rows_out += out.selected_count();
        stats_.nanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        // Zero selected rows with done = false is deliberate and legal: a segment
        // whose rows all failed the predicate must not look like end of stream.
        return Status::Ok();
    }
}

Status SequentialScanOperator::Close() {
    // Safe to call after a failed Open or mid-scan, which is what LIMIT relies on.
    if (reader_) {
        Status s = reader_->Close();
        reader_.reset();
        if (!s.ok()) return s;
    }
    exhausted_ = true;
    return Status::Ok();
}

}  // namespace quarry
