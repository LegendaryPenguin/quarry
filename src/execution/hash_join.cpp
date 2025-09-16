#include "hash_join.hpp"

#include <chrono>

#include "exec_util.hpp"

namespace quarry {

HashJoinOperator::HashJoinOperator(OperatorPtr left, OperatorPtr right, size_t left_key,
                                   size_t right_key)
    : left_(std::move(left)),
      right_(std::move(right)),
      left_key_(left_key),
      right_key_(right_key) {
    if (left_ && right_) {
        for (const Column& c : left_->output_schema().columns()) output_schema_.Add(c);
        for (const Column& c : right_->output_schema().columns()) output_schema_.Add(c);
    }
}

Status HashJoinOperator::Open() {
    if (!left_ || !right_) return Status::Internal("hash join is missing an input");

    const Schema& l = left_->output_schema();
    const Schema& r = right_->output_schema();
    if (left_key_ >= l.size() || right_key_ >= r.size()) {
        return Status::Internal("join key index is outside its input schema");
    }
    const TypeId lt = l.at(left_key_).type;
    const TypeId rt = r.at(right_key_).type;
    const bool numeric = (lt == TypeId::kInt64 || lt == TypeId::kDouble) &&
                         (rt == TypeId::kInt64 || rt == TypeId::kDouble);
    if (lt != rt && !numeric) {
        return Status::TypeError(std::string("cannot join a ") + TypeName(lt) + " key to a " +
                                 TypeName(rt) + " key");
    }
    promote_keys_ = lt != rt;

    build_batches_.clear();
    table_.clear();
    built_ = false;
    matches_ = nullptr;
    match_position_ = 0;
    probe_position_ = 0;
    probe_done_ = false;
    probe_batch_ = Batch(l);

    QUARRY_TRY(left_->Open());
    return right_->Open();
}

Status HashJoinOperator::BuildHashTable() {
    const auto started = std::chrono::steady_clock::now();
    const Schema& schema = right_->output_schema();
    Batch input(schema);
    std::string key;

    while (true) {
        input.Reset();
        bool done = false;
        QUARRY_TRY(right_->Next(input, done));

        const std::vector<uint32_t>& rows = input.Rows();
        if (input.selected_count() > 0) {
            const Vector& key_column = input.column(right_key_);
            for (uint32_t row : rows) {
                // A NULL build key can never match, so it is not even stored. This
                // is also why the key encoding's null marker is harmless here: no
                // null ever reaches the table.
                if (key_column.nulls().IsNull(row)) continue;

                if (build_batches_.empty() || build_batches_.back()->row_count() >= kBatchSize) {
                    build_batches_.push_back(std::make_unique<Batch>(schema));
                }
                Batch* target = build_batches_.back().get();
                for (size_t c = 0; c < schema.size(); ++c) {
                    QUARRY_TRY(CopyRow(target->column(c), input.column(c), row));
                }
                const size_t at = target->row_count();
                target->SetRowCount(at + 1);

                key.clear();
                EncodeKeyInto(key, key_column, row, promote_keys_);
                table_[key].push_back(RowRef{static_cast<uint32_t>(build_batches_.size() - 1),
                                             static_cast<uint32_t>(at)});
            }
            stats_.rows_in += input.selected_count();
        }
        if (done) break;
    }

    built_ = true;
    stats_.nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            started)
            .count());
    return Status::Ok();
}

Status HashJoinOperator::EmitPair(Batch& out, uint32_t left_row, const RowRef& right_row) {
    const size_t left_columns = left_->output_schema().size();
    for (size_t c = 0; c < left_columns; ++c) {
        QUARRY_TRY(CopyRow(out.column(c), probe_batch_.column(c), left_row));
    }
    const Batch& build = *build_batches_[right_row.batch];
    for (size_t c = 0; c < build.column_count(); ++c) {
        QUARRY_TRY(CopyRow(out.column(left_columns + c), build.column(c), right_row.row));
    }
    return Status::Ok();
}

Status HashJoinOperator::Next(Batch& out, bool& done) {
    done = false;
    QUARRY_TRY(PrepareOutput(out, output_schema_));
    if (!built_) QUARRY_TRY(BuildHashTable());

    const auto started = std::chrono::steady_clock::now();
    std::string key;
    size_t produced = 0;

    while (produced < kBatchSize) {
        if (matches_ != nullptr && match_position_ < matches_->size()) {
            QUARRY_TRY(EmitPair(out, current_left_row_, (*matches_)[match_position_++]));
            ++produced;
            continue;
        }
        matches_ = nullptr;

        const std::vector<uint32_t>& rows = probe_batch_.Rows();
        if (probe_position_ < rows.size()) {
            current_left_row_ = rows[probe_position_++];
            const Vector& key_column = probe_batch_.column(left_key_);
            if (key_column.nulls().IsNull(current_left_row_)) continue;  // NULL joins nothing

            key.clear();
            EncodeKeyInto(key, key_column, current_left_row_, promote_keys_);
            auto it = table_.find(key);
            if (it != table_.end()) {
                matches_ = &it->second;
                match_position_ = 0;
            }
            continue;
        }

        if (probe_done_) break;

        probe_batch_.Reset();
        bool left_done = false;
        QUARRY_TRY(left_->Next(probe_batch_, left_done));
        probe_done_ = left_done;
        probe_position_ = 0;
        stats_.rows_in += probe_batch_.selected_count();
    }

    out.SetRowCount(produced);
    stats_.rows_out += produced;
    if (produced > 0) ++stats_.batches;
    stats_.nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            started)
            .count());

    // Zero rows here really is end of stream: the loop above only exits empty once
    // the probe side is exhausted and no match list is still being drained.
    done = produced == 0;
    return Status::Ok();
}

Status HashJoinOperator::Close() {
    build_batches_.clear();
    table_.clear();
    matches_ = nullptr;
    Status ls = left_ ? left_->Close() : Status::Ok();
    Status rs = right_ ? right_->Close() : Status::Ok();
    return ls.ok() ? rs : ls;
}

}  // namespace quarry
