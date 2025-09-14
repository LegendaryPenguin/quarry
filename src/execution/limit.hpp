#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quarry/operator.hpp"

namespace quarry {

// LimitOperator stops the pipeline once enough rows have been produced.
//
// This operator is the argument for pull-based execution. Because nothing runs
// until the root asks, LIMIT 10 over a billion-row table does not filter a
// billion rows and discard them — it asks its child once or twice and then stops
// asking, so the scan never opens the remaining segments. A push engine would have
// to thread cancellation back up the pipeline to get the same effect.
class LimitOperator : public PhysicalOperator {
public:
    LimitOperator(OperatorPtr child, uint64_t limit);

    const Schema& output_schema() const override { return child_->output_schema(); }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "Limit"; }
    std::vector<PhysicalOperator*> Children() override { return {child_.get()}; }

private:
    OperatorPtr child_;
    uint64_t limit_;
    uint64_t emitted_ = 0;
    bool child_done_ = false;
};

}  // namespace quarry
