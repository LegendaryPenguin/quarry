#include "limit.hpp"

#include "quarry/batch.hpp"

namespace quarry {

LimitOperator::LimitOperator(OperatorPtr child, uint64_t limit)
    : child_(std::move(child)), limit_(limit) {}

Status LimitOperator::Open() {
    if (!child_) return Status::Internal("limit has no child");
    emitted_ = 0;
    child_done_ = false;
    return child_->Open();
}

Status LimitOperator::Next(Batch& out, bool& done) {
    done = false;
    if (emitted_ >= limit_ || child_done_) {
        // The child is never touched again. Not calling it is the entire point:
        // a test asserts on the child's call count for exactly this reason.
        out.Reset();
        done = true;
        return Status::Ok();
    }

    bool child_done = false;
    QUARRY_TRY(child_->Next(out, child_done));

    const uint64_t available = out.selected_count();
    const uint64_t remaining = limit_ - emitted_;
    if (available > remaining) {
        // Truncation reuses the selection vector rather than copying rows: the
        // prefix of Rows() is already in physical order, so keeping the first
        // `remaining` entries is a correct and allocation-light partial batch.
        const std::vector<uint32_t>& rows = out.Rows();
        std::vector<uint32_t> keep(rows.begin(), rows.begin() + static_cast<size_t>(remaining));
        out.SetSelection(std::move(keep));
        emitted_ = limit_;
    } else {
        emitted_ += available;
        child_done_ = child_done;
        done = child_done && out.selected_count() == 0;
    }

    ++stats_.batches;
    stats_.rows_in += available;
    stats_.rows_out += out.selected_count();
    return Status::Ok();
}

Status LimitOperator::Close() { return child_ ? child_->Close() : Status::Ok(); }

}  // namespace quarry
