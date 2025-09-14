#include "project.hpp"

#include <chrono>

#include "exec_util.hpp"

namespace quarry {

ProjectionOperator::ProjectionOperator(OperatorPtr child,
                                       std::vector<const ast::Expression*> projections,
                                       Schema output_schema)
    : child_(std::move(child)),
      projections_(std::move(projections)),
      output_schema_(std::move(output_schema)),
      eval_(child_ ? child_->output_schema() : Schema{}) {}

Result<std::unique_ptr<ProjectionOperator>> ProjectionOperator::Create(
    OperatorPtr child, std::vector<const ast::Expression*> projections) {
    if (!child) return Status::Internal("projection has no child");
    if (projections.empty()) {
        return Status::InvalidArgument("projection has no expressions");
    }

    ExpressionEvaluator probe(child->output_schema());
    Schema out;
    for (const ast::Expression* e : projections) {
        if (!e) return Status::Internal("projection expression is null");
        auto type = probe.ResultType(*e);
        if (!type.ok()) return type.status();
        // An untyped NULL literal has no type of its own; int64 is as arbitrary a
        // choice as any, and the column is all nulls either way.
        const TypeId t = type.value() == TypeId::kInvalid ? TypeId::kInt64 : type.value();
        out.Add(Column(ExpressionEvaluator::OutputName(*e), t, true));
    }

    return std::unique_ptr<ProjectionOperator>(
        new ProjectionOperator(std::move(child), std::move(projections), std::move(out)));
}

Status ProjectionOperator::Open() {
    child_done_ = false;
    input_ = Batch(child_->output_schema());
    return child_->Open();
}

Status ProjectionOperator::Next(Batch& out, bool& done) {
    done = false;
    QUARRY_TRY(PrepareOutput(out, output_schema_));
    if (child_done_) {
        done = true;
        return Status::Ok();
    }

    input_.Reset();
    bool child_done = false;
    QUARRY_TRY(child_->Next(input_, child_done));
    child_done_ = child_done;

    const size_t n = input_.selected_count();
    if (n == 0) {
        // Either end of stream or a batch a filter emptied; both are reported
        // honestly rather than by looping until rows appear, so a LIMIT above can
        // still stop the pipeline promptly.
        done = child_done;
        return Status::Ok();
    }

    const auto started = std::chrono::steady_clock::now();

    for (size_t c = 0; c < projections_.size(); ++c) {
        auto value = eval_.Evaluate(*projections_[c], input_);
        if (!value.ok()) return value.status();

        const Vector& src = value.value().vector();
        if (src.size() != n) {
            return Status::Internal("projection expression produced " +
                                    std::to_string(src.size()) + " values for " +
                                    std::to_string(n) + " rows");
        }
        Vector& dst = out.column(c);
        dst.Reserve(n);
        // src is compacted — slot i is the i-th live row — so a dense loop here is
        // correct and is the only place in this operator where one is.
        for (size_t i = 0; i < n; ++i) {
            QUARRY_TRY(CopyRow(dst, src, i));
        }
    }
    out.SetRowCount(n);

    ++stats_.batches;
    stats_.rows_in += n;
    stats_.rows_out += n;
    stats_.nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            started)
            .count());
    return Status::Ok();
}

Status ProjectionOperator::Close() { return child_ ? child_->Close() : Status::Ok(); }

}  // namespace quarry
