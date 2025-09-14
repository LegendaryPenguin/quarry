#include "filter.hpp"

#include <chrono>

namespace quarry {

FilterOperator::FilterOperator(OperatorPtr child, const ast::Expression* residual)
    : child_(std::move(child)),
      residual_(residual),
      eval_(child_ ? child_->output_schema() : Schema{}) {}

Status FilterOperator::Open() {
    if (!child_) return Status::Internal("filter has no child");
    if (!residual_) return Status::InvalidArgument("filter has no predicate");
    child_done_ = false;
    return child_->Open();
}

Status FilterOperator::Next(Batch& out, bool& done) {
    done = false;
    if (child_done_) {
        // Every operator here reports end of stream on a call of its own rather
        // than attaching done = true to a batch that still has rows. The contract
        // permits both, and one convention is worth more than the extra call it
        // costs: a consumer that checks done before looking at the batch can never
        // be wrong.
        out.Reset();
        done = true;
        return Status::Ok();
    }
    bool child_done = false;
    QUARRY_TRY(child_->Next(out, child_done));
    child_done_ = child_done;
    if (child_done && out.selected_count() == 0) {
        done = true;
        return Status::Ok();
    }

    const auto started = std::chrono::steady_clock::now();

    // Select() iterates the batch's current Rows(), so the result is already the
    // intersection with whatever selection a scan or an upstream filter installed.
    auto keep = eval_.Select(*residual_, out);
    if (!keep.ok()) return keep.status();

    ++stats_.batches;
    stats_.rows_in += out.selected_count();
    stats_.rows_out += keep.value().size();
    out.SetSelection(keep.take());

    stats_.nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            started)
            .count());

    // A batch where nothing survived is returned with done = false on purpose. The
    // caller asks again; end of stream is the child's business, not the
    // predicate's.
    return Status::Ok();
}

Status FilterOperator::Close() { return child_ ? child_->Close() : Status::Ok(); }

}  // namespace quarry
