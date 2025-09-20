// The optimizer: five rewrites, each one switchable and each one required to
// prove it did something.
//
// Rules run in a fixed order rather than to a fixpoint. The order is the one the
// rules' preconditions imply:
//
//   1. constant folding      - may delete a Filter outright, so run before the
//                              rules that inspect one
//   2. predicate pushdown    - produces the pushed_predicates that both segment
//                              pruning and index selection consume
//   3. projection pruning    - narrows the scan's column set, which index
//                              selection then costs against
//   4. segment pruning       - needs the pushed predicates; shrinks the row count
//   5. index selection       - costed last, against the post-pruning row count
//                              and the final column width
//
// A fixpoint loop would buy nothing here: no rule in this set creates an
// opportunity for a rule earlier in the order.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "planner_internal.hpp"
#include "quarry/plan.hpp"

namespace quarry {
namespace {

using planner::CloneExpr;
using planner::IsConstant;
using planner::Selectivity;
using planner::TotalRows;

// --- cost model -----------------------------------------------------------
//
// Costs are in units of "one column value touched", which is the only unit the
// planner can measure without a calibration run.
//
//   scan_cost  = rows * columns_projected
//   index_cost = log2(rows + 1) * kIndexProbeCost
//              + rows * selectivity * columns_projected * kRandomFetchCost
//
// kRandomFetchCost is the ratio between fetching a row the index named and
// reading the next row the scan was already streaming. Because the column width
// multiplies both the scan term and the fetch term, the comparison reduces to
// roughly `selectivity < 1 / kRandomFetchCost` once the logarithmic probe term is
// small — so kRandomFetchCost = 4 is the same statement as "an index is worth it
// below about 25% selectivity", stated once, in a constant, instead of hidden in
// a threshold somewhere else.
constexpr double kRandomFetchCost = 4.0;
constexpr double kIndexProbeCost = 2.0;

double ScanCost(double rows, size_t columns) {
    return rows * static_cast<double>(std::max<size_t>(columns, 1));
}

double IndexCost(double rows, size_t columns, double selectivity) {
    double levels = std::log2(rows + 1.0);
    double matched = rows * selectivity;
    return levels * kIndexProbeCost +
           matched * static_cast<double>(std::max<size_t>(columns, 1)) * kRandomFetchCost;
}

// --- plan navigation ------------------------------------------------------
//
// Every plan the binder produces is a chain: one child per node, one scan at the
// bottom. Chain returns it root-first, which lets the rules walk in either
// direction without recursion.
std::vector<LogicalNode*> Chain(LogicalNode* root) {
    std::vector<LogicalNode*> out;
    for (LogicalNode* n = root; n != nullptr;) {
        out.push_back(n);
        n = n->children.empty() ? nullptr : n->children.front().get();
    }
    return out;
}

// ContainsJoin walks the whole tree, not just the chain, because a join is the one
// node with two children.
bool ContainsJoin(const LogicalNode* n) {
    if (n == nullptr) return false;
    if (n->kind == LogicalKind::kJoin) return true;
    for (const auto& child : n->children) {
        if (ContainsJoin(child.get())) return true;
    }
    return false;
}

// FindScan returns the plan's single scan, or null when there is more than one.
//
// Every rule below rewrites relative to "the" scan: a predicate's column index, a
// projection's column set and a segment list all mean something different once two
// tables are in play, and a rule that quietly picked the left one would narrow the
// wrong scan's projection. Joins are therefore left unoptimized rather than
// optimized incorrectly, and that is a known gap rather than an oversight.
LogicalNode* FindScan(LogicalNode* root) {
    if (ContainsJoin(root)) return nullptr;
    for (LogicalNode* n : Chain(root)) {
        if (n->kind == LogicalKind::kScan) return n;
    }
    return nullptr;
}

// RemoveNode splices a single-child node out of the chain. parent is null when the
// node being removed is the root, in which case the caller takes the new root.
LogicalPtr DropNode(LogicalPtr node) {
    return std::move(node->children.front());
}

void RemoveFromChain(LogicalNode* root, LogicalNode* target, LogicalPtr& root_slot) {
    if (root == target) {
        root_slot = DropNode(std::move(root_slot));
        return;
    }
    for (LogicalNode* n : Chain(root)) {
        if (!n->children.empty() && n->children.front().get() == target) {
            n->children.front() = DropNode(std::move(n->children.front()));
            return;
        }
    }
}

// --- constant folding -----------------------------------------------------

std::optional<Value> EvalConst(const ast::Expression& e);

std::optional<Value> EvalCompare(ast::BinaryOp op, const Value& l, const Value& r) {
    if (l.is_null() || r.is_null()) return Value::Null();  // unknown, not false

    bool eq = false, lt = false;
    if (l.IsNumeric() && r.IsNumeric()) {
        eq = l.AsNumeric() == r.AsNumeric();
        lt = l.AsNumeric() < r.AsNumeric();
    } else if (l.type() == r.type()) {
        eq = l == r;
        lt = l < r;
    } else {
        return std::nullopt;  // not comparable; the binder already rejected it
    }

    switch (op) {
        case ast::BinaryOp::kEq: return Value::Boolean(eq);
        case ast::BinaryOp::kNe: return Value::Boolean(!eq);
        case ast::BinaryOp::kLt: return Value::Boolean(lt);
        case ast::BinaryOp::kLe: return Value::Boolean(lt || eq);
        case ast::BinaryOp::kGt: return Value::Boolean(!lt && !eq);
        case ast::BinaryOp::kGe: return Value::Boolean(!lt);
        default:                 return std::nullopt;
    }
}

std::optional<Value> EvalArithmetic(ast::BinaryOp op, const Value& l, const Value& r) {
    if (l.is_null() || r.is_null()) return Value::Null();
    if (!l.IsNumeric() || !r.IsNumeric()) return std::nullopt;

    bool integral = l.type() == TypeId::kInt64 && r.type() == TypeId::kInt64;
    double a = l.AsNumeric(), b = r.AsNumeric();
    if (op == ast::BinaryOp::kDiv && b == 0.0) {
        // Division by a literal zero is left alone rather than folded: folding it
        // would have to invent a result, and leaving it makes the failure happen
        // where the user can see which row was being evaluated.
        return std::nullopt;
    }
    double result = 0.0;
    switch (op) {
        case ast::BinaryOp::kAdd: result = a + b; break;
        case ast::BinaryOp::kSub: result = a - b; break;
        case ast::BinaryOp::kMul: result = a * b; break;
        case ast::BinaryOp::kDiv: result = a / b; break;
        default: return std::nullopt;
    }
    if (integral && op != ast::BinaryOp::kDiv) {
        return Value::Int64(static_cast<int64_t>(result));
    }
    if (integral && op == ast::BinaryOp::kDiv) {
        return Value::Int64(l.as_int64() / r.as_int64());
    }
    return Value::Double(result);
}

std::optional<Value> EvalConst(const ast::Expression& e) {
    if (e.kind == ast::ExprKind::kLiteral) return e.literal;
    if (e.kind != ast::ExprKind::kBinary || !e.left || !e.right) return std::nullopt;

    auto l = EvalConst(*e.left);
    auto r = EvalConst(*e.right);
    if (!l.has_value() || !r.has_value()) return std::nullopt;

    if (ast::IsComparison(e.op)) return EvalCompare(e.op, *l, *r);
    if (e.op == ast::BinaryOp::kAnd || e.op == ast::BinaryOp::kOr) {
        if (l->type() != TypeId::kBoolean || r->type() != TypeId::kBoolean) {
            return std::nullopt;
        }
        bool a = l->as_boolean(), b = r->as_boolean();
        return Value::Boolean(e.op == ast::BinaryOp::kAnd ? (a && b) : (a || b));
    }
    return EvalArithmetic(e.op, *l, *r);
}

bool IsBooleanLiteral(const ast::Expression& e, bool want) {
    return e.kind == ast::ExprKind::kLiteral && !e.literal.is_null() &&
           e.literal.type() == TypeId::kBoolean && e.literal.as_boolean() == want;
}

ast::ExprPtr Fold(const ast::Expression& e) {
    if (IsConstant(e)) {
        if (auto v = EvalConst(e); v.has_value()) {
            auto lit = ast::Expression::Literal(*v);
            lit->alias = e.alias;
            return lit;
        }
    }
    if (e.kind != ast::ExprKind::kBinary || !e.left || !e.right) return CloneExpr(e);

    ast::ExprPtr l = Fold(*e.left);
    ast::ExprPtr r = Fold(*e.right);

    // Absorbing and identity elements. `x AND TRUE` is x, and `x AND FALSE` is
    // FALSE even when x is a column reference, which is what turns a partially
    // constant WHERE into one the scan can short-circuit.
    if (e.op == ast::BinaryOp::kAnd) {
        if (IsBooleanLiteral(*l, false) || IsBooleanLiteral(*r, false)) {
            return ast::Expression::Literal(Value::Boolean(false));
        }
        if (IsBooleanLiteral(*l, true)) return r;
        if (IsBooleanLiteral(*r, true)) return l;
    }
    if (e.op == ast::BinaryOp::kOr) {
        if (IsBooleanLiteral(*l, true) || IsBooleanLiteral(*r, true)) {
            return ast::Expression::Literal(Value::Boolean(true));
        }
        if (IsBooleanLiteral(*l, false)) return r;
        if (IsBooleanLiteral(*r, false)) return l;
    }

    auto out = ast::Expression::Binary(e.op, std::move(l), std::move(r));
    out->alias = e.alias;
    return out;
}

// MarkUnsatisfiable gives a provably empty query something the scan can act on.
//
// CONTRACT FRICTION: LogicalNode has no "always false" flag, so the short-circuit
// is expressed in the only field a scan already consults — its segment list. A
// scan over zero segments reads nothing and returns nothing, which is exactly the
// semantics `WHERE 1 = 0` asks for.
void MarkUnsatisfiable(LogicalNode* scan) {
    TableDef pruned = *scan->table_def;
    planner::ScanPruning info;
    info.segments_total = pruned.segments.size();
    info.segments_pruned = pruned.segments.size();
    info.unsatisfiable = true;
    pruned.segments.clear();
    scan->table_def = planner::AdoptPrunedTable(std::move(pruned));
    scan->estimated_rows = 0.0;
    planner::RecordPruning(*scan, info);
}

bool FoldConstants(LogicalPtr& root) {
    bool changed = false;
    bool unsatisfiable = false;

    for (LogicalNode* n : Chain(root.get())) {
        if (n->kind == LogicalKind::kFilter && n->residual != nullptr) {
            std::string before = n->residual->ToString();
            ast::ExprPtr folded = Fold(*n->residual);
            if (folded->ToString() != before) changed = true;
            n->residual = std::move(folded);
            if (IsBooleanLiteral(*n->residual, false)) unsatisfiable = true;
        } else if (n->kind == LogicalKind::kProject) {
            for (auto& p : n->projections) {
                std::string before = p->ToString();
                ast::ExprPtr folded = Fold(*p);
                if (folded->ToString() != before) changed = true;
                p = std::move(folded);
            }
        }
    }

    // The scan is emptied before any Filter is removed, because removing a FALSE
    // filter without emptying the scan would turn "no rows" into "every row".
    bool short_circuited = false;
    if (unsatisfiable) {
        if (LogicalNode* scan = FindScan(root.get()); scan != nullptr) {
            MarkUnsatisfiable(scan);
            for (LogicalNode* n : Chain(root.get())) n->estimated_rows = 0.0;
            short_circuited = true;
            changed = true;
        }
    }

    // Collected first and acted on second, because removing a node invalidates the
    // chain the loop above is walking.
    std::vector<LogicalNode*> dead_filters;
    for (LogicalNode* n : Chain(root.get())) {
        if (n->kind != LogicalKind::kFilter || n->residual == nullptr) continue;
        if (IsBooleanLiteral(*n->residual, true)) dead_filters.push_back(n);
        if (short_circuited && IsBooleanLiteral(*n->residual, false)) dead_filters.push_back(n);
    }

    for (LogicalNode* filter : dead_filters) {
        // A filter whose residual is a constant decides nothing per row: TRUE keeps
        // every row, FALSE is already handled by emptying the scan.
        RemoveFromChain(root.get(), filter, root);
        changed = true;
    }

    return changed;
}

// --- predicate pushdown ---------------------------------------------------

bool PushPredicates(LogicalPtr& root) {
    LogicalNode* scan = FindScan(root.get());
    if (scan == nullptr || scan->table_def == nullptr) return false;

    LogicalNode* filter = nullptr;
    for (LogicalNode* n : Chain(root.get())) {
        if (!n->children.empty() && n->children.front()->kind == LogicalKind::kScan &&
            n->kind == LogicalKind::kFilter) {
            filter = n;
        }
    }
    if (filter == nullptr || filter->residual == nullptr) return false;

    // Predicate column indexes are TABLE relative, not scan-output relative: a zone
    // map and an index are both keyed by the table's column order, and projection
    // pruning is free to change the scan's output width afterwards.
    const Schema& table_schema = scan->table_def->schema;

    std::vector<const ast::Expression*> conjuncts;
    planner::SplitConjuncts(*filter->residual, conjuncts);

    std::vector<Predicate> pushed;
    std::vector<const ast::Expression*> residual_parts;
    for (const ast::Expression* c : conjuncts) {
        Predicate p;
        if (planner::TryAsPredicate(*c, table_schema, p)) {
            pushed.push_back(p);
        } else {
            residual_parts.push_back(c);
        }
    }
    if (pushed.empty()) return false;

    for (const Predicate& p : pushed) scan->pushed_predicates.push_back(p);

    double selectivity = 1.0;
    for (const Predicate& p : pushed) {
        selectivity *= Selectivity(p, planner::WholeTableStats(*scan->table_def, p.column_index),
                                   TotalRows(*scan->table_def));
    }
    scan->estimated_rows *= selectivity;

    if (residual_parts.empty()) {
        // Nothing left to evaluate per row. Leaving a Filter here with a
        // trivially-true residual would cost a pass over every batch to decide
        // nothing, so the node goes away.
        double rows = scan->estimated_rows;
        RemoveFromChain(root.get(), filter, root);
        for (LogicalNode* n : Chain(root.get())) {
            if (n->kind != LogicalKind::kScan) n->estimated_rows = std::min(n->estimated_rows, rows);
        }
        return true;
    }

    ast::ExprPtr rebuilt = CloneExpr(*residual_parts.front());
    for (size_t i = 1; i < residual_parts.size(); ++i) {
        rebuilt = ast::Expression::Binary(ast::BinaryOp::kAnd, std::move(rebuilt),
                                          CloneExpr(*residual_parts[i]));
    }
    filter->residual = std::move(rebuilt);
    filter->estimated_rows =
        scan->estimated_rows * std::pow(planner::kResidualSelectivity,
                                        static_cast<double>(residual_parts.size()));
    return true;
}

// --- projection pruning ---------------------------------------------------

void AddNeeded(std::vector<size_t>& needed, size_t index) {
    if (std::find(needed.begin(), needed.end(), index) == needed.end()) needed.push_back(index);
}

bool PruneProjection(LogicalPtr& root) {
    LogicalNode* scan = FindScan(root.get());
    if (scan == nullptr || scan->table_def == nullptr) return false;

    const Schema& table_schema = scan->table_def->schema;
    std::vector<LogicalNode*> chain = Chain(root.get());

    std::vector<size_t> needed;
    for (const Predicate& p : scan->pushed_predicates) AddNeeded(needed, p.column_index);

    // Walk upward from the scan. Only nodes below the first schema-redefining node
    // reference the scan's columns; an Aggregate or a Project publishes a schema of
    // its own, and everything above it is phrased in those terms instead.
    std::vector<LogicalNode*> region;  // scan-schema nodes, bottom up
    for (size_t i = chain.size(); i-- > 0;) {
        LogicalNode* n = chain[i];
        if (n->kind == LogicalKind::kScan) continue;

        if (n->kind == LogicalKind::kFilter) {
            std::vector<std::string> names;
            if (n->residual) planner::CollectColumnNames(*n->residual, names);
            for (const std::string& name : names) {
                if (auto idx = table_schema.IndexOf(name); idx.has_value()) AddNeeded(needed, *idx);
            }
            region.push_back(n);
            continue;
        }
        if (n->kind == LogicalKind::kSort) {
            for (const SortKey& key : n->sort_keys) AddNeeded(needed, key.column_index);
            region.push_back(n);
            continue;
        }
        if (n->kind == LogicalKind::kLimit) {
            region.push_back(n);
            continue;
        }
        if (n->kind == LogicalKind::kAggregate) {
            for (size_t idx : n->group_by) AddNeeded(needed, idx);
            for (const Aggregate& agg : n->aggregates) {
                if (agg.column_index.has_value()) AddNeeded(needed, *agg.column_index);
            }
            region.push_back(n);
            break;
        }
        if (n->kind == LogicalKind::kProject) {
            std::vector<std::string> names;
            for (const auto& p : n->projections) planner::CollectColumnNames(*p, names);
            for (const std::string& name : names) {
                if (auto idx = table_schema.IndexOf(name); idx.has_value()) AddNeeded(needed, *idx);
            }
            region.push_back(n);
            break;
        }
        break;  // a join, which this rule does not handle
    }

    std::sort(needed.begin(), needed.end());
    if (needed == scan->projection) return false;

    std::vector<size_t> remap(table_schema.size(), table_schema.size());
    for (size_t i = 0; i < needed.size(); ++i) remap[needed[i]] = i;

    scan->projection = needed;
    scan->output_schema = table_schema.Project(needed);

    for (LogicalNode* n : region) {
        switch (n->kind) {
            case LogicalKind::kFilter:
            case LogicalKind::kLimit:
                n->output_schema = n->children.front()->output_schema;
                break;
            case LogicalKind::kSort:
                for (SortKey& key : n->sort_keys) key.column_index = remap[key.column_index];
                n->output_schema = n->children.front()->output_schema;
                break;
            case LogicalKind::kAggregate:
                for (size_t& idx : n->group_by) idx = remap[idx];
                for (Aggregate& agg : n->aggregates) {
                    if (agg.column_index.has_value()) agg.column_index = remap[*agg.column_index];
                }
                break;
            case LogicalKind::kProject:
                break;  // name based, and its own output schema is unchanged
            default:
                break;
        }
    }
    return true;
}

// --- segment pruning ------------------------------------------------------

bool PruneSegments(LogicalPtr& root) {
    LogicalNode* scan = FindScan(root.get());
    if (scan == nullptr || scan->table_def == nullptr) return false;
    if (scan->pushed_predicates.empty() || scan->table_def->segments.empty()) return false;

    TableDef kept = *scan->table_def;
    kept.segments.clear();

    uint64_t rows_before = TotalRows(*scan->table_def);
    size_t total = scan->table_def->segments.size();

    for (const SegmentMeta& seg : scan->table_def->segments) {
        bool possible = true;
        for (const Predicate& p : scan->pushed_predicates) {
            if (p.column_index >= seg.columns.size()) continue;  // nothing to prune on
            if (!CanMatch(seg.columns[p.column_index].stats, p.op, p.constant)) {
                possible = false;
                break;
            }
        }
        if (possible) kept.segments.push_back(seg);
    }

    size_t pruned = total - kept.segments.size();
    if (pruned == 0) return false;

    uint64_t rows_after = TotalRows(kept);
    scan->table_def = planner::AdoptPrunedTable(std::move(kept));

    planner::ScanPruning info;
    info.segments_total = total;
    info.segments_pruned = pruned;
    planner::RecordPruning(*scan, info);

    if (rows_before > 0) {
        double scale = static_cast<double>(rows_after) / static_cast<double>(rows_before);
        for (LogicalNode* n : Chain(root.get())) n->estimated_rows *= scale;
    }
    return true;
}

// --- index selection ------------------------------------------------------

bool SelectIndex(LogicalPtr& root) {
    LogicalNode* scan = FindScan(root.get());
    if (scan == nullptr || scan->table_def == nullptr) return false;
    if (scan->pushed_predicates.empty() || scan->table_def->indexes.empty()) return false;
    if (!scan->chosen_index.empty()) return false;

    double rows = static_cast<double>(TotalRows(*scan->table_def));
    if (rows <= 0.0) return false;
    size_t columns = scan->projection.size();

    double best_cost = ScanCost(rows, columns);
    const IndexDef* best = nullptr;

    for (const IndexDef& index : scan->table_def->indexes) {
        for (const Predicate& p : scan->pushed_predicates) {
            if (p.column_index != index.column_index) continue;
            // An index answers equality and ranges. A `<>` predicate matches almost
            // everything by construction, so there is nothing for an index to skip.
            if (p.op == CompareOp::kNe) continue;

            double s = Selectivity(p, planner::WholeTableStats(*scan->table_def, p.column_index),
                                   TotalRows(*scan->table_def));
            double cost = IndexCost(rows, columns, s);
            if (cost < best_cost) {
                best_cost = cost;
                best = &index;
            }
        }
    }

    if (best == nullptr) return false;
    scan->chosen_index = best->name;
    scan->estimated_cost = best_cost;
    return true;
}

}  // namespace

Result<LogicalPtr> Optimizer::Optimize(LogicalPtr plan) {
    applied_.clear();
    planner::ResetPruningRegistry();

    if (plan == nullptr) return Status::InvalidArgument("Optimize called with a null plan");

    // A rule is recorded only when it changed the plan. "Ran and did nothing" and
    // "fired" are different facts, and only the second one is worth printing.
    if (rules_.constant_folding && FoldConstants(plan)) {
        applied_.push_back("constant_folding");
    }
    if (rules_.predicate_pushdown && PushPredicates(plan)) {
        applied_.push_back("predicate_pushdown");
    }
    if (rules_.projection_pruning && PruneProjection(plan)) {
        applied_.push_back("projection_pruning");
    }
    if (rules_.segment_pruning && PruneSegments(plan)) {
        applied_.push_back("segment_pruning");
    }
    if (rules_.index_selection && SelectIndex(plan)) {
        applied_.push_back("index_selection");
    }

    if (LogicalNode* scan = FindScan(plan.get()); scan != nullptr && scan->estimated_cost == 0.0) {
        scan->estimated_cost = ScanCost(scan->estimated_rows, scan->projection.size());
    }
    return plan;
}

}  // namespace quarry
