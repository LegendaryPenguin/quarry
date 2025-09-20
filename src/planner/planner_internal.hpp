#pragma once

// Helpers shared by the binder, the optimizer, and EXPLAIN.
//
// None of this is in include/quarry/plan.hpp because none of it is part of the
// contract the rest of the engine depends on: callers hand the planner an AST and
// get a LogicalNode tree back, and how the tree is built is nobody else's
// business.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/catalog.hpp"
#include "quarry/operator.hpp"
#include "quarry/plan.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"

namespace quarry::planner {

// --- expression utilities -------------------------------------------------

// CloneExpr deep-copies an expression. The binder needs this because an
// ast::Expression owns its children through unique_ptr while the statement it
// came from is const: a plan that aliased the statement's nodes would dangle the
// moment the caller dropped the statement.
ast::ExprPtr CloneExpr(const ast::Expression& e);

// AggregateSpelling is the canonical text for an aggregate call, used as the
// output column name when the query gives no alias.
//
// Spelled here rather than through AggregateKindName (declared in
// include/quarry/operator.hpp) so the planner links without the execution layer.
// Source of truth for the set itself is AggregateKind in quarry/operator.hpp.
std::string AggregateSpelling(AggregateKind k);
std::string AggregateOutputName(const ast::Expression& agg);

// AggregateResultType duplicates the rule documented on Aggregate::ResultType in
// include/quarry/operator.hpp, for the same link-independence reason as above.
// Source of truth for the rule is that comment.
TypeId AggregateResultType(AggregateKind kind, TypeId input);

// ContainsAggregate reports whether an aggregate appears anywhere in a subtree,
// which is how both "aggregate in WHERE" and "nested aggregate" are detected.
bool ContainsAggregate(const ast::Expression& e);

// CollectColumnNames appends every column name referenced in a subtree, in
// encounter order and without deduplication.
void CollectColumnNames(const ast::Expression& e, std::vector<std::string>& out);

// IsConstant reports whether a subtree has no column or aggregate leaves, and so
// can be evaluated at plan time.
bool IsConstant(const ast::Expression& e);

// --- type checking --------------------------------------------------------

// BindContext carries what an error message needs in order to be useful: which
// table a name failed to resolve against, and what that table does have.
struct BindContext {
    std::string table;
    const Schema* schema = nullptr;
};

// UnknownColumnError names the typo and lists the alternatives, including a
// nearest-match suggestion when one is close. A bind error that says only
// "unknown column" forces the reader to go find the schema themselves.
Status UnknownColumnError(const BindContext& ctx, const std::string& name);

// TypesComparable implements the engine's one implicit conversion: int64 and
// double compare freely, everything else must match exactly. NULL (kInvalid on a
// literal) is comparable with anything, because the result is unknown rather
// than ill-typed.
bool TypesComparable(TypeId a, TypeId b);
bool IsNumericType(TypeId t);

// InferType type-checks an expression against a schema and returns its type.
Result<TypeId> InferType(const ast::Expression& e, const Schema& schema,
                         const BindContext& ctx);

// --- predicate extraction and selectivity ---------------------------------

// TryAsPredicate recognizes the column-op-constant shape, flipping
// constant-op-column into it. column_index is an index into `schema`.
bool TryAsPredicate(const ast::Expression& e, const Schema& schema, Predicate& out);

// SplitConjuncts flattens an AND tree into its leaves. OR is opaque: either side
// may be false for a given row, so neither side can be evaluated in isolation.
void SplitConjuncts(const ast::Expression& e, std::vector<const ast::Expression*>& out);

// Selectivity estimates the surviving fraction of rows for one predicate.
//
// kUnknownSelectivity values are the usual textbook guesses. They are named
// constants rather than inline literals because every one of them is a place the
// estimate can be wrong, and EXPLAIN ANALYZE exists to show when they are.
constexpr double kEqSelectivityFallback = 0.1;
constexpr double kRangeSelectivityFallback = 0.3;
constexpr double kResidualSelectivity = 0.25;

double Selectivity(const Predicate& pred, const ColumnStats& stats, uint64_t rows);

// WholeTableStats and TotalRows roll per-segment chunk statistics up to the whole
// table. They duplicate TableDef::ColumnStatistics and TableDef::row_count
// deliberately: segment pruning already walks the per-segment chunks, and an
// estimate derived from a different roll-up than the one pruning uses would let
// the two disagree about the same table.
ColumnStats WholeTableStats(const TableDef& table, size_t column_index);
uint64_t TotalRows(const TableDef& table);

// EstimateSelectivity estimates a whole WHERE clause against a table.
double EstimateSelectivity(const ast::Expression& e, const TableDef& table,
                           const Schema& schema);

// --- segment pruning side table -------------------------------------------

// ScanPruning is what segment pruning produced for one scan node.
//
// CONTRACT FRICTION: LogicalNode has no field for a surviving-segment list or a
// pruned count, yet the optimizer is required to "drop segments from the scan"
// and "record how many were eliminated". The compromise is to keep a pruned COPY
// of the TableDef — so `scan.table_def->segments` really does hold only the
// survivors, which is what the physical scan and EXPLAIN read — plus a side
// table holding the count. The side table is keyed by node address and is
// cleared at the start of every Optimize call, which bounds the lifetime of a
// stale entry to a single plan.
struct ScanPruning {
    size_t segments_total = 0;
    size_t segments_pruned = 0;

    // unsatisfiable marks a scan that constant folding proved can never produce a
    // row (WHERE 1 = 0). Such a scan keeps no segments at all.
    bool unsatisfiable = false;
};

void ResetPruningRegistry();
void RecordPruning(const LogicalNode& scan, const ScanPruning& info);
const ScanPruning* LookupPruning(const LogicalNode& scan);

// AdoptPrunedTable stores an optimizer-owned TableDef copy and returns a pointer
// to it, stable for as long as the registry is not reset.
const TableDef* AdoptPrunedTable(TableDef pruned);

// --- estimate carry-over into EXPLAIN ANALYZE ------------------------------
//
// CONTRACT FRICTION: Explain takes a LogicalNode and ExplainPhysical takes a
// PhysicalOperator, and nothing in either signature connects the two. Printing an
// estimate next to a measurement therefore needs the physical planner to leave a
// note behind as it lowers each node. Same lifetime rule as the pruning registry:
// cleared whenever the registry is reset.
void RecordEstimate(const PhysicalOperator* op, double estimated_rows);
const double* LookupEstimate(const PhysicalOperator* op);

}  // namespace quarry::planner
