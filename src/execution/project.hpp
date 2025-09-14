#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"

#include "expression.hpp"

namespace quarry {

// ProjectionOperator evaluates the SELECT list.
//
// It is the one place in the pipeline that deliberately materializes: the output
// batch holds exactly the surviving rows, compacted, with no selection vector.
// Everything below it can pass column data through untouched, so the copy happens
// once, at the point where the shape of the data actually changes.
class ProjectionOperator : public PhysicalOperator {
public:
    // Create resolves every expression's type up front, because output_schema()
    // must be answerable before the first batch and a type error is worth
    // reporting at plan time rather than halfway through a query.
    //
    // The expressions are borrowed from the logical plan, which outlives execution.
    static Result<std::unique_ptr<ProjectionOperator>> Create(
        OperatorPtr child, std::vector<const ast::Expression*> projections);

    const Schema& output_schema() const override { return output_schema_; }

    Status Open() override;
    Status Next(Batch& out, bool& done) override;
    Status Close() override;

    std::string Name() const override { return "Project"; }
    std::vector<PhysicalOperator*> Children() override { return {child_.get()}; }

private:
    ProjectionOperator(OperatorPtr child, std::vector<const ast::Expression*> projections,
                       Schema output_schema);

    OperatorPtr child_;
    std::vector<const ast::Expression*> projections_;
    Schema output_schema_;
    ExpressionEvaluator eval_;

    // The child writes here. Owned by this operator so that the input's selection
    // vector survives across the evaluation of several projection expressions.
    Batch input_;
    bool child_done_ = false;
};

}  // namespace quarry
