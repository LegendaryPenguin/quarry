#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/buffer.hpp"
#include "quarry/catalog.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"

namespace quarry {

// The logical plan sits between the bound AST and the physical operators.
//
// It exists as a separate representation because the optimizer's rewrites are
// natural on a tree of relational operators and awkward on either of the other
// two. Pushing a predicate down is "move this node below that node"; doing the
// same thing on an AST means rewriting a WHERE clause into a FROM clause, and
// doing it on physical operators means rebuilding objects that already own
// execution state.

enum class LogicalKind : uint8_t {
    kScan,
    kFilter,
    kProject,
    kAggregate,
    kSort,
    kLimit,
    kJoin,
};

const char* LogicalKindName(LogicalKind k);

struct LogicalNode;
using LogicalPtr = std::unique_ptr<LogicalNode>;

struct LogicalNode {
    LogicalKind kind;
    std::vector<LogicalPtr> children;

    // output_schema is filled by the binder and maintained by every rewrite. A
    // rewrite that changes which columns flow through MUST update it, because the
    // physical layer trusts it to size vectors.
    Schema output_schema;

    // --- kScan ---
    std::string table;
    const TableDef* table_def = nullptr;

    // pushed_predicates are the predicates the scan itself will evaluate, in the
    // canonical column-op-constant form that zone maps and indexes can also use.
    std::vector<Predicate> pushed_predicates;

    // projection is the set of table column indexes the scan must actually read.
    // Narrowing this is what makes a columnar layout pay off, so it is a first
    // class field rather than something inferred later.
    std::vector<size_t> projection;

    // chosen_index names the index the scan will use, if the optimizer selected
    // one. Empty means a sequential scan over segments.
    std::string chosen_index;

    // --- kFilter ---
    // The residual predicate: everything the WHERE clause asked for that could
    // NOT be reduced to a pushable Predicate.
    ast::ExprPtr residual;

    // --- kProject ---
    std::vector<ast::ExprPtr> projections;

    // --- kAggregate ---
    std::vector<size_t> group_by;
    std::vector<Aggregate> aggregates;

    // --- kSort ---
    std::vector<SortKey> sort_keys;

    // --- kLimit ---
    uint64_t limit = 0;

    // --- kJoin ---
    size_t left_key = 0;
    size_t right_key = 0;

    // --- cost model inputs, filled by the optimizer ---
    // estimated_rows drives index selection and, for a join, build-side choice.
    // It is an estimate from segment statistics, not a measurement, and the
    // optimizer is honest about being wrong: EXPLAIN ANALYZE prints estimated
    // next to actual so the gap is visible rather than hidden.
    double estimated_rows = 0.0;
    double estimated_cost = 0.0;

    std::string Describe() const;
};

// Explain renders a plan tree as indented text. When `stats` is true it also
// prints each operator's measured counters beside its estimates.
std::string Explain(const LogicalNode& root, bool with_stats = false);
std::string ExplainPhysical(PhysicalOperator& root, bool with_stats = false);

// Binder turns a parsed statement into a logical plan with every name resolved
// and every type checked.
//
// Separating this from parsing is what lets an unknown column produce a bind
// error that names the column and lists what the table does have, rather than a
// syntax error pointing at a character offset.
class Binder {
public:
    explicit Binder(const Catalog& catalog) : catalog_(catalog) {}

    Result<LogicalPtr> BindSelect(const ast::SelectStatement& stmt);

private:
    const Catalog& catalog_;
};

// Optimizer applies rewrites to a bound plan. Each rule is separately testable
// and separately disableable, because a rule that cannot be turned off cannot be
// shown to have done anything.
class Optimizer {
public:
    struct Rules {
        bool predicate_pushdown = true;
        bool projection_pruning = true;
        bool constant_folding = true;
        bool index_selection = true;
        bool segment_pruning = true;
    };

    // Two constructors rather than one with a defaulted argument: `Rules rules = {}`
    // inside the class definition would need Rules's default member initializers
    // before the enclosing class is complete, which C++ does not allow.
    Optimizer() = default;
    explicit Optimizer(Rules rules) : rules_(rules) {}

    Result<LogicalPtr> Optimize(LogicalPtr plan);

    // AppliedRules reports which rewrites actually fired on the last Optimize
    // call, so a test can assert a rule changed the plan instead of asserting
    // only that the result still runs.
    const std::vector<std::string>& applied_rules() const { return applied_; }

private:
    Rules rules_;
    std::vector<std::string> applied_;
};

// PhysicalPlanner lowers a logical plan to executable operators.
class PhysicalPlanner {
public:
    PhysicalPlanner(const Catalog& catalog, BufferPool* pool)
        : catalog_(catalog), pool_(pool) {}

    Result<OperatorPtr> Build(const LogicalNode& plan);

private:
    const Catalog& catalog_;
    BufferPool* pool_;
};

}  // namespace quarry
