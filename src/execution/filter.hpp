#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/operator.hpp"

#include "expression.hpp"

namespace quarry {

// FilterOperator applies the residual predicate — whatever the WHERE clause asked
// for that could not be pushed into the scan.
//
// It never moves column data. It narrows the batch's selection vector in place and
// passes the same batch through, so stacking filters costs one pass over the
// surviving rows each and zero copies.
class FilterOperator : public PhysicalOperator {
public:
    // `residual` is borrowed: the logical plan owns the expression tree and
    // outlives execution. Copying an ast::Expression is not possible anyway, since
    // its children are unique_ptr.
    FilterOperator(OperatorPtr child, const ast::Expression* residual);

    const Schema& output_schema() const override { return child_->output_schema(); }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "Filter"; }
    std::vector<PhysicalOperator*> Children() override { return {child_.get()}; }

private:
    OperatorPtr child_;
    const ast::Expression* residual_;
    ExpressionEvaluator eval_;
    bool child_done_ = false;
};

}  // namespace quarry
