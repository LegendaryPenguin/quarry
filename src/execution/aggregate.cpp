#include "aggregate.hpp"

#include <algorithm>
#include <chrono>

#include "exec_util.hpp"

namespace quarry {
namespace {

bool NumericType(TypeId t) { return t == TypeId::kInt64 || t == TypeId::kDouble; }

}  // namespace

HashAggregateOperator::HashAggregateOperator(OperatorPtr child, std::vector<size_t> group_by,
                                             std::vector<Aggregate> aggregates)
    : child_(std::move(child)),
      group_by_(std::move(group_by)),
      aggregates_(std::move(aggregates)) {
    const Schema& in = child_ ? child_->output_schema() : Schema{};
    // Group columns first, then aggregates: the order the SQL `GROUP BY a, b`
    // writes them, so a projection above can address them positionally.
    for (size_t idx : group_by_) {
        if (idx < in.size()) output_schema_.Add(in.at(idx));
    }
    for (const Aggregate& agg : aggregates_) {
        const std::string name =
            agg.output_name.empty()
                ? std::string(AggregateKindName(agg.kind)) +
                      (agg.column_index ? "(" + std::to_string(*agg.column_index) + ")" : "(*)")
                : agg.output_name;
        // Every aggregate output is nullable: SUM, MIN and MAX of an all-null
        // group are NULL even when the input column was declared NOT NULL.
        output_schema_.Add(Column(name, agg.ResultType(in), true));
    }
}

Status HashAggregateOperator::Open() {
    if (!child_) return Status::Internal("aggregate has no child");

    const Schema& in = child_->output_schema();
    for (size_t idx : group_by_) {
        if (idx >= in.size()) {
            return Status::Internal("GROUP BY names column " + std::to_string(idx) +
                                    " but the input has " + std::to_string(in.size()));
        }
    }
    for (const Aggregate& agg : aggregates_) {
        if (agg.kind == AggregateKind::kCount && !agg.column_index) continue;  // COUNT(*)
        if (!agg.column_index) {
            return Status::InvalidArgument(std::string(AggregateKindName(agg.kind)) +
                                           " requires a column argument");
        }
        if (*agg.column_index >= in.size()) {
            return Status::Internal(std::string(AggregateKindName(agg.kind)) + " names column " +
                                    std::to_string(*agg.column_index) + " but the input has " +
                                    std::to_string(in.size()));
        }
        const TypeId t = in.at(*agg.column_index).type;
        if ((agg.kind == AggregateKind::kSum || agg.kind == AggregateKind::kAvg) &&
            !NumericType(t)) {
            return Status::TypeError(std::string(AggregateKindName(agg.kind)) + " of a " +
                                     TypeName(t) + " column");
        }
    }

    groups_.clear();
    group_index_.clear();
    built_ = false;
    emit_position_ = 0;
    input_ = Batch(in);
    return child_->Open();
}

Status HashAggregateOperator::BuildGroups() {
    const auto started = std::chrono::steady_clock::now();

    if (group_by_.empty()) {
        // A global aggregate has exactly one group, created before any input is
        // read so that COUNT(*) over an empty table still returns a row with 0.
        Group g;
        g.accumulators.resize(aggregates_.size());
        groups_.push_back(std::move(g));
    }

    std::string key;
    std::vector<size_t> group_ids;
    while (true) {
        input_.Reset();
        bool child_done = false;
        QUARRY_TRY(child_->Next(input_, child_done));

        const std::vector<uint32_t>& rows = input_.Rows();
        if (input_.selected_count() > 0) {
            group_ids.clear();
            group_ids.reserve(rows.size());

            if (group_by_.empty()) {
                group_ids.assign(rows.size(), 0);
            } else {
                for (uint32_t row : rows) {
                    key.clear();
                    for (size_t col : group_by_) {
                        EncodeKeyInto(key, input_.column(col), row, /*promote_numeric=*/false);
                    }
                    auto it = group_index_.find(key);
                    if (it == group_index_.end()) {
                        Group g;
                        g.accumulators.resize(aggregates_.size());
                        g.key.reserve(group_by_.size());
                        for (size_t col : group_by_) {
                            g.key.push_back(input_.column(col).GetValue(row));
                        }
                        groups_.push_back(std::move(g));
                        it = group_index_.emplace(key, groups_.size() - 1).first;
                    }
                    group_ids.push_back(it->second);
                }
            }

            QUARRY_TRY(Accumulate(input_, group_ids));
            stats_.rows_in += input_.selected_count();
        }
        if (child_done) break;
    }

    built_ = true;
    stats_.nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            started)
            .count());
    return Status::Ok();
}

Status HashAggregateOperator::Accumulate(const Batch& batch, const std::vector<size_t>& group_ids) {
    const std::vector<uint32_t>& rows = batch.Rows();

    // One pass per aggregate rather than one pass per row with an inner loop over
    // aggregates: each pass then touches a single column and a single accumulator
    // field, which is the whole reason the batch is columnar.
    for (size_t a = 0; a < aggregates_.size(); ++a) {
        const Aggregate& agg = aggregates_[a];

        if (agg.kind == AggregateKind::kCount && !agg.column_index) {
            // COUNT(*) counts rows, nulls included. It is the one aggregate that
            // never looks at a column.
            for (size_t i = 0; i < rows.size(); ++i) {
                ++groups_[group_ids[i]].accumulators[a].count;
            }
            continue;
        }

        const Vector& col = batch.column(*agg.column_index);
        const bool check_nulls = col.nulls().has_nulls();

        for (size_t i = 0; i < rows.size(); ++i) {
            const uint32_t row = rows[i];
            // Every aggregate except COUNT(*) ignores nulls entirely, so a group
            // of nothing but nulls ends with count == 0 and therefore a NULL
            // result for SUM, AVG, MIN and MAX — but 0 for COUNT.
            if (check_nulls && col.nulls().IsNull(row)) continue;

            Accumulator& acc = groups_[group_ids[i]].accumulators[a];
            ++acc.count;
            switch (agg.kind) {
                case AggregateKind::kCount:
                    break;
                case AggregateKind::kSum:
                case AggregateKind::kAvg:
                    if (col.type() == TypeId::kInt64) {
                        const int64_t v = col.int64_data()[row];
                        // Wrapping, for the same reason the arithmetic kernels wrap:
                        // signed overflow must not be undefined behaviour.
                        acc.int_sum = static_cast<int64_t>(static_cast<uint64_t>(acc.int_sum) +
                                                           static_cast<uint64_t>(v));
                        acc.double_sum += static_cast<double>(v);
                    } else {
                        const double v = col.double_data()[row];
                        acc.double_sum += v;
                    }
                    break;
                case AggregateKind::kMin:
                case AggregateKind::kMax: {
                    const Value v = col.GetValue(row);
                    Value& slot = agg.kind == AggregateKind::kMin ? acc.min : acc.max;
                    if (slot.is_null()) {
                        slot = v;
                    } else if (agg.kind == AggregateKind::kMin ? (v < slot) : (slot < v)) {
                        slot = v;
                    }
                    break;
                }
            }
        }
    }
    return Status::Ok();
}

Status HashAggregateOperator::EmitGroup(const Group& group, Batch& out) {
    for (size_t k = 0; k < group.key.size(); ++k) {
        QUARRY_TRY(AppendValue(out.column(k), group.key[k]));
    }
    const size_t base = group.key.size();
    for (size_t a = 0; a < aggregates_.size(); ++a) {
        const Aggregate& agg = aggregates_[a];
        const Accumulator& acc = group.accumulators[a];
        Vector& dst = out.column(base + a);

        switch (agg.kind) {
            case AggregateKind::kCount:
                dst.AppendInt64(acc.count);
                break;
            case AggregateKind::kSum:
                if (acc.count == 0) {
                    dst.AppendNull();
                } else if (dst.type() == TypeId::kInt64) {
                    dst.AppendInt64(acc.int_sum);
                } else {
                    dst.AppendDouble(acc.double_sum);
                }
                break;
            case AggregateKind::kAvg:
                // AVG is the sum over the count of NON-NULL values, so nulls lower
                // neither the numerator nor the denominator.
                if (acc.count == 0) {
                    dst.AppendNull();
                } else {
                    dst.AppendDouble(acc.double_sum / static_cast<double>(acc.count));
                }
                break;
            case AggregateKind::kMin:
                QUARRY_TRY(AppendValue(dst, acc.min));
                break;
            case AggregateKind::kMax:
                QUARRY_TRY(AppendValue(dst, acc.max));
                break;
        }
    }
    return Status::Ok();
}

Status HashAggregateOperator::Next(Batch& out, bool& done) {
    done = false;
    QUARRY_TRY(PrepareOutput(out, output_schema_));
    if (!built_) QUARRY_TRY(BuildGroups());

    if (emit_position_ >= groups_.size()) {
        done = true;
        return Status::Ok();
    }

    const size_t end = std::min(groups_.size(), emit_position_ + kBatchSize);
    for (size_t g = emit_position_; g < end; ++g) {
        QUARRY_TRY(EmitGroup(groups_[g], out));
    }
    out.SetRowCount(end - emit_position_);
    stats_.rows_out += end - emit_position_;
    ++stats_.batches;
    emit_position_ = end;
    return Status::Ok();
}

Status HashAggregateOperator::Close() { return child_ ? child_->Close() : Status::Ok(); }

}  // namespace quarry
