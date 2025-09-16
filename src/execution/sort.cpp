#include "sort.hpp"

#include <algorithm>
#include <chrono>

#include "exec_util.hpp"

namespace quarry {

SortOperator::SortOperator(OperatorPtr child, std::vector<SortKey> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {}

Status SortOperator::Open() {
    if (!child_) return Status::Internal("sort has no child");
    const Schema& in = child_->output_schema();
    for (const SortKey& k : keys_) {
        if (k.column_index >= in.size()) {
            return Status::Internal("ORDER BY names column " + std::to_string(k.column_index) +
                                    " but the input has " + std::to_string(in.size()));
        }
    }
    buffered_.clear();
    order_.clear();
    sorted_ = false;
    emit_position_ = 0;
    input_ = Batch(in);
    return child_->Open();
}

Status SortOperator::BufferInput() {
    const Schema& schema = child_->output_schema();

    while (true) {
        input_.Reset();
        bool child_done = false;
        QUARRY_TRY(child_->Next(input_, child_done));

        const std::vector<uint32_t>& rows = input_.Rows();
        if (input_.selected_count() > 0) {
            // Only the live rows are copied into the buffer. Copying the physical
            // rows instead would make the sort order rows a filter already removed.
            for (uint32_t row : rows) {
                if (buffered_.empty() || buffered_.back()->row_count() >= kBatchSize) {
                    buffered_.push_back(std::make_unique<Batch>(schema));
                }
                Batch* target = buffered_.back().get();
                for (size_t c = 0; c < schema.size(); ++c) {
                    QUARRY_TRY(CopyRow(target->column(c), input_.column(c), row));
                }
                const size_t at = target->row_count();
                target->SetRowCount(at + 1);
                order_.push_back(RowRef{static_cast<uint32_t>(buffered_.size() - 1),
                                        static_cast<uint32_t>(at)});
            }
            stats_.rows_in += input_.selected_count();
        }
        if (child_done) break;
    }
    return Status::Ok();
}

bool SortOperator::Less(const RowRef& a, const RowRef& b) const {
    const Batch& ba = *buffered_[a.batch];
    const Batch& bb = *buffered_[b.batch];
    for (const SortKey& k : keys_) {
        int c = CompareRows(ba.column(k.column_index), a.row, bb.column(k.column_index), b.row);
        if (c == 0) continue;
        if (k.descending) c = -c;
        return c < 0;
    }
    return false;  // equal on every key: stable_sort keeps the input order
}

Status SortOperator::Next(Batch& out, bool& done) {
    done = false;
    QUARRY_TRY(PrepareOutput(out, output_schema()));

    if (!sorted_) {
        const auto started = std::chrono::steady_clock::now();
        QUARRY_TRY(BufferInput());
        std::stable_sort(order_.begin(), order_.end(),
                         [this](const RowRef& a, const RowRef& b) { return Less(a, b); });
        sorted_ = true;
        stats_.nanos += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
    }

    if (emit_position_ >= order_.size()) {
        done = true;
        return Status::Ok();
    }

    const size_t end = std::min(order_.size(), emit_position_ + kBatchSize);
    const size_t columns = output_schema().size();
    for (size_t i = emit_position_; i < end; ++i) {
        const RowRef& ref = order_[i];
        const Batch& src = *buffered_[ref.batch];
        for (size_t c = 0; c < columns; ++c) {
            QUARRY_TRY(CopyRow(out.column(c), src.column(c), ref.row));
        }
    }
    out.SetRowCount(end - emit_position_);
    stats_.rows_out += end - emit_position_;
    ++stats_.batches;
    emit_position_ = end;
    return Status::Ok();
}

Status SortOperator::Close() {
    buffered_.clear();
    order_.clear();
    return child_ ? child_->Close() : Status::Ok();
}

}  // namespace quarry
