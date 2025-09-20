// Lowering a logical plan to executable operators.
//
// There is almost no decision making here, and that is the point: every choice
// about HOW to run a query was already made by the optimizer and recorded in a
// field. If this file ever has to decide something, that decision belongs upstream
// where it can be explained and tested without constructing an operator.
//
// LIFETIME: FilterOperator and ProjectionOperator borrow their expressions from
// the LogicalNode tree, because an ast::Expression owns its children through
// unique_ptr and cannot be copied. The logical plan therefore MUST outlive the
// operator tree this returns. That is the contract those two headers state, and it
// is restated here because this is the function that creates the dependency.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../execution/aggregate.hpp"
#include "../execution/filter.hpp"
#include "../execution/hash_join.hpp"
#include "../execution/limit.hpp"
#include "../execution/project.hpp"
#include "../execution/scan.hpp"
#include "../execution/sort.hpp"
#include "../storage/segment_reader.hpp"
#include "planner_internal.hpp"
#include "quarry/plan.hpp"

namespace quarry {
namespace {

Result<OperatorPtr> LowerNode(const LogicalNode& node, const Catalog& catalog, BufferPool* pool);

Result<std::vector<OperatorPtr>> LowerChildren(const LogicalNode& node, const Catalog& catalog,
                                               BufferPool* pool) {
    std::vector<OperatorPtr> out;
    out.reserve(node.children.size());
    for (const auto& child : node.children) {
        if (child == nullptr) return Status::Internal("logical node with a null child");
        auto lowered = LowerNode(*child, catalog, pool);
        if (!lowered.ok()) return lowered.status();
        out.push_back(lowered.take());
    }
    return out;
}

Result<OperatorPtr> LowerScan(const LogicalNode& node) {
    if (node.table_def == nullptr) {
        return Status::Internal("scan node for table '" + node.table + "' has no TableDef");
    }

    // A predicate can only be evaluated on a column the scan actually reads, so the
    // projection handed to the operator is the union of what the query needs and
    // what the pushed predicates need. Projection pruning already includes
    // predicated columns; this is the assertion of that invariant rather than a
    // second place that decides it.
    std::vector<size_t> projection = node.projection;
    for (const Predicate& p : node.pushed_predicates) {
        if (std::find(projection.begin(), projection.end(), p.column_index) == projection.end()) {
            return Status::Internal("pushed predicate on column #" +
                                    std::to_string(p.column_index) +
                                    " of '" + node.table + "' is outside the scan's projection");
        }
    }

    // The scan takes its segment list by value from the TableDef it is given, which
    // is the optimizer's pruned copy when segment pruning fired. Nothing here
    // re-derives the pruning decision.
    SequentialScanOperator::ReaderFactory factory =
        [](const SegmentMeta& segment,
           const std::vector<size_t>& columns) -> Result<std::unique_ptr<SegmentReader>> {
            return std::unique_ptr<SegmentReader>(
                new FileSegmentReader(segment, columns));
        };

    OperatorPtr op(new SequentialScanOperator(*node.table_def, std::move(projection),
                                              node.pushed_predicates, std::move(factory)));
    return op;
}

Result<OperatorPtr> LowerNode(const LogicalNode& node, const Catalog& catalog, BufferPool* pool) {
    auto children = LowerChildren(node, catalog, pool);
    if (!children.ok()) return children.status();
    std::vector<OperatorPtr> kids = children.take();

    auto wrong_arity = [&](size_t want) {
        return Status::Internal(std::string(LogicalKindName(node.kind)) + " node has " +
                                std::to_string(kids.size()) + " children, expected " +
                                std::to_string(want));
    };

    OperatorPtr built;
    switch (node.kind) {
        case LogicalKind::kScan: {
            if (!kids.empty()) return wrong_arity(0);
            auto op = LowerScan(node);
            if (!op.ok()) return op.status();
            built = op.take();
            break;
        }

        case LogicalKind::kFilter: {
            if (kids.size() != 1) return wrong_arity(1);
            if (node.residual == nullptr) {
                return Status::Internal("Filter node with no residual predicate");
            }
            built.reset(new FilterOperator(std::move(kids[0]), node.residual.get()));
            break;
        }

        case LogicalKind::kProject: {
            if (kids.size() != 1) return wrong_arity(1);
            std::vector<const ast::Expression*> exprs;
            exprs.reserve(node.projections.size());
            for (const auto& p : node.projections) {
                if (p == nullptr) return Status::Internal("Project node with a null expression");
                exprs.push_back(p.get());
            }
            auto op = ProjectionOperator::Create(std::move(kids[0]), std::move(exprs));
            if (!op.ok()) return op.status();
            built = op.take();
            break;
        }

        case LogicalKind::kAggregate: {
            if (kids.size() != 1) return wrong_arity(1);
            built.reset(new HashAggregateOperator(std::move(kids[0]), node.group_by,
                                                  node.aggregates));
            break;
        }

        case LogicalKind::kSort: {
            if (kids.size() != 1) return wrong_arity(1);
            built.reset(new SortOperator(std::move(kids[0]), node.sort_keys));
            break;
        }

        case LogicalKind::kLimit: {
            if (kids.size() != 1) return wrong_arity(1);
            built.reset(new LimitOperator(std::move(kids[0]), node.limit));
            break;
        }

        case LogicalKind::kJoin: {
            if (kids.size() != 2) return wrong_arity(2);
            // Inputs are handed over in the logical order, NOT reordered to put the
            // smaller side on the build input.
            //
            // CONTRACT FRICTION: HashJoinOperator builds on its right input and
            // concatenates its output schema as left ++ right, so swapping the inputs
            // to pick a cheaper build side would also permute the result columns and
            // invalidate the Join node's output_schema that the Project above was
            // bound against. Choosing the build side therefore has to be a logical
            // rewrite that swaps the schema too, which the optimizer does not do yet.
            built.reset(new HashJoinOperator(std::move(kids[0]), std::move(kids[1]),
                                             node.left_key, node.right_key));
            break;
        }
    }

    if (built == nullptr) return Status::Internal("unhandled logical node kind in lowering");

    // The estimate is recorded against the operator so that EXPLAIN ANALYZE can
    // print it beside the measured count; see the note in planner_internal.hpp.
    planner::RecordEstimate(built.get(), node.estimated_rows);
    return built;
}

}  // namespace

Result<OperatorPtr> PhysicalPlanner::Build(const LogicalNode& plan) {
    return LowerNode(plan, catalog_, pool_);
}

}  // namespace quarry
