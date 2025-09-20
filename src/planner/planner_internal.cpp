#include "planner_internal.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace quarry::planner {
namespace {

// Levenshtein distance, bounded by the lengths involved. Column names here are
// identifiers, so the quadratic table is a few hundred bytes at worst and the
// cost is paid only on the error path.
size_t EditDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({cur[j - 1] + 1, prev[j] + 1, sub});
        }
        prev = cur;
    }
    return prev[b.size()];
}

std::string JoinNames(const Schema& schema) {
    std::string out;
    for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) out += ", ";
        out += schema.at(i).name;
    }
    return out.empty() ? "(none)" : out;
}

// The registry behind ScanPruning. A free function holding a function-local
// static keeps the lifetime well defined without a global constructor.
struct Registry {
    std::map<const LogicalNode*, ScanPruning> pruning;
    std::vector<std::unique_ptr<TableDef>> owned_tables;
    std::map<const PhysicalOperator*, double> estimates;
};

Registry& Reg() {
    static Registry reg;
    return reg;
}

}  // namespace

ast::ExprPtr CloneExpr(const ast::Expression& e) {
    auto out = std::make_unique<ast::Expression>();
    out->kind = e.kind;
    out->literal = e.literal;
    out->column_name = e.column_name;
    out->op = e.op;
    out->agg = e.agg;
    out->alias = e.alias;
    if (e.left) out->left = CloneExpr(*e.left);
    if (e.right) out->right = CloneExpr(*e.right);
    if (e.agg_argument) out->agg_argument = CloneExpr(*e.agg_argument);
    return out;
}

std::string AggregateSpelling(AggregateKind k) {
    switch (k) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum:   return "SUM";
        case AggregateKind::kAvg:   return "AVG";
        case AggregateKind::kMin:   return "MIN";
        case AggregateKind::kMax:   return "MAX";
    }
    return "?";
}

std::string AggregateOutputName(const ast::Expression& agg) {
    if (!agg.alias.empty()) return agg.alias;
    std::string arg = "*";
    if (agg.agg_argument && agg.agg_argument->kind == ast::ExprKind::kColumnRef) {
        arg = agg.agg_argument->column_name;
    } else if (agg.agg_argument) {
        arg = agg.agg_argument->ToString();
    }
    return AggregateSpelling(agg.agg) + "(" + arg + ")";
}

TypeId AggregateResultType(AggregateKind kind, TypeId input) {
    switch (kind) {
        case AggregateKind::kCount: return TypeId::kInt64;
        case AggregateKind::kAvg:   return TypeId::kDouble;
        case AggregateKind::kSum:   return input == TypeId::kDouble ? TypeId::kDouble
                                                                   : TypeId::kInt64;
        case AggregateKind::kMin:
        case AggregateKind::kMax:   return input;
    }
    return TypeId::kInvalid;
}

bool ContainsAggregate(const ast::Expression& e) {
    if (e.kind == ast::ExprKind::kAggregate) return true;
    if (e.left && ContainsAggregate(*e.left)) return true;
    if (e.right && ContainsAggregate(*e.right)) return true;
    if (e.agg_argument && ContainsAggregate(*e.agg_argument)) return true;
    return false;
}

void CollectColumnNames(const ast::Expression& e, std::vector<std::string>& out) {
    if (e.kind == ast::ExprKind::kColumnRef) out.push_back(e.column_name);
    if (e.left) CollectColumnNames(*e.left, out);
    if (e.right) CollectColumnNames(*e.right, out);
    if (e.agg_argument) CollectColumnNames(*e.agg_argument, out);
}

bool IsConstant(const ast::Expression& e) {
    switch (e.kind) {
        case ast::ExprKind::kLiteral:
            return true;
        case ast::ExprKind::kColumnRef:
        case ast::ExprKind::kAggregate:
        case ast::ExprKind::kStar:
            return false;
        case ast::ExprKind::kBinary:
            return e.left && e.right && IsConstant(*e.left) && IsConstant(*e.right);
    }
    return false;
}

Status UnknownColumnError(const BindContext& ctx, const std::string& name) {
    std::string msg = "unknown column '" + name + "'";
    if (!ctx.table.empty()) msg += " in table '" + ctx.table + "'";

    if (ctx.schema != nullptr) {
        // A suggestion is offered only when it is close enough to be plausible;
        // a "did you mean" that is three edits away is noise.
        const std::string* best = nullptr;
        size_t best_distance = 0;
        for (const auto& col : ctx.schema->columns()) {
            size_t d = EditDistance(name, col.name);
            if (best == nullptr || d < best_distance) {
                best = &col.name;
                best_distance = d;
            }
        }
        if (best != nullptr && best_distance <= std::max<size_t>(2, name.size() / 3)) {
            msg += "; did you mean '" + *best + "'?";
        }
        msg += " available columns: " + JoinNames(*ctx.schema);
    }
    return Status::BindError(msg);
}

bool IsNumericType(TypeId t) {
    return t == TypeId::kInt64 || t == TypeId::kDouble;
}

bool TypesComparable(TypeId a, TypeId b) {
    if (a == TypeId::kInvalid || b == TypeId::kInvalid) return true;  // NULL literal
    if (a == b) return true;
    return IsNumericType(a) && IsNumericType(b);
}

Result<TypeId> InferType(const ast::Expression& e, const Schema& schema,
                         const BindContext& ctx) {
    switch (e.kind) {
        case ast::ExprKind::kLiteral:
            // A NULL literal has no type of its own; kInvalid here means "unknown",
            // and TypesComparable lets it meet anything.
            return e.literal.is_null() ? TypeId::kInvalid : e.literal.type();

        case ast::ExprKind::kColumnRef: {
            auto idx = schema.IndexOf(e.column_name);
            if (!idx.has_value()) return UnknownColumnError(ctx, e.column_name);
            return schema.at(*idx).type;
        }

        case ast::ExprKind::kStar:
            return Status::BindError("'*' is only legal as SELECT * or COUNT(*)");

        case ast::ExprKind::kAggregate: {
            if (e.agg_argument == nullptr) {
                if (e.agg != AggregateKind::kCount) {
                    return Status::BindError(AggregateSpelling(e.agg) +
                                            " requires an argument; only COUNT(*) may omit one");
                }
                return TypeId::kInt64;
            }
            if (ContainsAggregate(*e.agg_argument)) {
                std::string inner = e.agg_argument->ToString();
                return Status::BindError("aggregate " + AggregateSpelling(e.agg) +
                                        "() contains a nested aggregate " + inner +
                                        "; aggregates cannot be nested");
            }
            if (e.agg_argument->kind == ast::ExprKind::kStar) {
                if (e.agg != AggregateKind::kCount) {
                    return Status::BindError(AggregateSpelling(e.agg) +
                                            "(*) is not meaningful; only COUNT(*) is");
                }
                return TypeId::kInt64;
            }
            auto arg = InferType(*e.agg_argument, schema, ctx);
            if (!arg.ok()) return arg.status();
            TypeId in = arg.value();
            bool needs_number = e.agg == AggregateKind::kSum || e.agg == AggregateKind::kAvg;
            if (needs_number && !IsNumericType(in)) {
                return Status::TypeError(AggregateSpelling(e.agg) + "(" +
                                        e.agg_argument->ToString() + ") requires a numeric column, but '" +
                                        e.agg_argument->column_name + "' is " + TypeName(in));
            }
            return AggregateResultType(e.agg, in);
        }

        case ast::ExprKind::kBinary: {
            if (e.left == nullptr || e.right == nullptr) {
                return Status::Internal("binary expression with a missing operand");
            }
            auto l = InferType(*e.left, schema, ctx);
            if (!l.ok()) return l.status();
            auto r = InferType(*e.right, schema, ctx);
            if (!r.ok()) return r.status();
            TypeId lt = l.value(), rt = r.value();

            if (ast::IsComparison(e.op)) {
                if (!TypesComparable(lt, rt)) {
                    return Status::TypeError(std::string("cannot compare ") + TypeName(lt) +
                                             " to " + TypeName(rt) + " in " + e.ToString());
                }
                return TypeId::kBoolean;
            }
            if (e.op == ast::BinaryOp::kAnd || e.op == ast::BinaryOp::kOr) {
                for (TypeId t : {lt, rt}) {
                    if (t != TypeId::kBoolean && t != TypeId::kInvalid) {
                        return Status::TypeError(std::string(ast::BinaryOpName(e.op)) +
                                                 " requires boolean operands, got " + TypeName(t) +
                                                 " in " + e.ToString());
                    }
                }
                return TypeId::kBoolean;
            }
            // Arithmetic.
            for (TypeId t : {lt, rt}) {
                if (!IsNumericType(t) && t != TypeId::kInvalid) {
                    return Status::TypeError(std::string("arithmetic operator ") +
                                             ast::BinaryOpName(e.op) +
                                             " requires numeric operands, got " + TypeName(t) +
                                             " in " + e.ToString());
                }
            }
            if (lt == TypeId::kDouble || rt == TypeId::kDouble) return TypeId::kDouble;
            return TypeId::kInt64;
        }
    }
    return Status::Internal("unhandled expression kind");
}

bool TryAsPredicate(const ast::Expression& e, const Schema& schema, Predicate& out) {
    if (e.kind != ast::ExprKind::kBinary || !ast::IsComparison(e.op)) return false;
    if (e.left == nullptr || e.right == nullptr) return false;

    const ast::Expression* col = e.left.get();
    const ast::Expression* lit = e.right.get();
    bool flipped = false;
    if (col->kind != ast::ExprKind::kColumnRef) {
        std::swap(col, lit);
        flipped = true;
    }
    if (col->kind != ast::ExprKind::kColumnRef) return false;
    if (lit->kind != ast::ExprKind::kLiteral) return false;

    auto idx = schema.IndexOf(col->column_name);
    if (!idx.has_value()) return false;

    // A NULL constant never produces a pushable predicate: `x = NULL` is unknown
    // for every row, and a zone map comparison against NULL would prune wrongly.
    if (lit->literal.is_null()) return false;

    TypeId col_type = schema.at(*idx).type;
    if (!TypesComparable(col_type, lit->literal.type())) return false;

    out.column_index = *idx;
    out.op = flipped ? Flip(ast::AsCompareOp(e.op)) : ast::AsCompareOp(e.op);
    out.constant = lit->literal;
    return true;
}

void SplitConjuncts(const ast::Expression& e, std::vector<const ast::Expression*>& out) {
    if (e.kind == ast::ExprKind::kBinary && e.op == ast::BinaryOp::kAnd && e.left && e.right) {
        SplitConjuncts(*e.left, out);
        SplitConjuncts(*e.right, out);
        return;
    }
    out.push_back(&e);
}

double Selectivity(const Predicate& pred, const ColumnStats& stats, uint64_t rows) {
    if (rows == 0) return 0.0;

    double distinct = static_cast<double>(stats.distinct_estimate);
    switch (pred.op) {
        case CompareOp::kEq:
            if (distinct >= 1.0) return 1.0 / distinct;
            return kEqSelectivityFallback;
        case CompareOp::kNe:
            if (distinct >= 1.0) return 1.0 - 1.0 / distinct;
            return 1.0 - kEqSelectivityFallback;
        default:
            break;
    }

    // Range: interpolate linearly inside [min, max] when both bounds and the
    // constant are numeric. Linear interpolation assumes a uniform distribution,
    // which is wrong for skewed data; it is used because a histogram is the only
    // thing that would do better and the segment format does not carry one.
    if (stats.all_null || stats.min.is_null() || stats.max.is_null() ||
        !stats.min.IsNumeric() || !stats.max.IsNumeric() || !pred.constant.IsNumeric()) {
        return kRangeSelectivityFallback;
    }
    double lo = stats.min.AsNumeric();
    double hi = stats.max.AsNumeric();
    double c = pred.constant.AsNumeric();
    if (hi <= lo) return c >= lo && c <= hi ? 1.0 : 0.0;

    double below = (c - lo) / (hi - lo);
    below = std::clamp(below, 0.0, 1.0);
    switch (pred.op) {
        case CompareOp::kLt:
        case CompareOp::kLe: return below;
        case CompareOp::kGt:
        case CompareOp::kGe: return 1.0 - below;
        default:             return kRangeSelectivityFallback;
    }
}

namespace {

// Whole-table statistics, rolled up from the per-segment chunk stats rather than
// read from TableDef::ColumnStatistics, because segment pruning already walks
// those chunks and the two estimates must not be allowed to disagree.
ColumnStats RollUp(const TableDef& table, size_t column_index) {
    ColumnStats out;
    out.all_null = true;
    uint64_t rows = 0;
    for (const auto& seg : table.segments) {
        rows += seg.row_count;
        if (column_index >= seg.columns.size()) continue;
        const ColumnStats& s = seg.columns[column_index].stats;
        out.null_count += s.null_count;
        // distinct_estimate takes the per-segment maximum rather than the sum.
        // The sum would treat segments as disjoint in value space and so
        // over-state distinctness, which makes an equality predicate look more
        // selective than it is and biases index selection towards the index. The
        // maximum biases towards the sequential scan, which is the safe default.
        out.distinct_estimate = std::max(out.distinct_estimate, s.distinct_estimate);
        if (s.all_null) continue;
        out.all_null = false;
        if (out.min.is_null() || s.min < out.min) out.min = s.min;
        if (out.max.is_null() || out.max < s.max) out.max = s.max;
    }
    if (out.distinct_estimate > rows) out.distinct_estimate = rows;
    return out;
}

}  // namespace

double EstimateSelectivity(const ast::Expression& e, const TableDef& table,
                           const Schema& schema) {
    std::vector<const ast::Expression*> conjuncts;
    SplitConjuncts(e, conjuncts);

    // Conjuncts are assumed independent, so selectivities multiply. Correlated
    // predicates make this an under-estimate, which is the classic reason a plan
    // under-estimates a row count; EXPLAIN ANALYZE shows the gap.
    double result = 1.0;
    for (const ast::Expression* c : conjuncts) {
        Predicate p;
        if (TryAsPredicate(*c, schema, p)) {
            result *= Selectivity(p, RollUp(table, p.column_index), TotalRows(table));
        } else if (IsConstant(*c)) {
            continue;  // constant folding, not guesswork, decides these
        } else {
            result *= kResidualSelectivity;
        }
    }
    return std::clamp(result, 0.0, 1.0);
}

ColumnStats WholeTableStats(const TableDef& table, size_t column_index) {
    return RollUp(table, column_index);
}

uint64_t TotalRows(const TableDef& table) {
    uint64_t rows = 0;
    for (const auto& seg : table.segments) rows += seg.row_count;
    return rows;
}

void ResetPruningRegistry() {
    Reg().pruning.clear();
    Reg().owned_tables.clear();
    Reg().estimates.clear();
}

void RecordEstimate(const PhysicalOperator* op, double estimated_rows) {
    Reg().estimates[op] = estimated_rows;
}

const double* LookupEstimate(const PhysicalOperator* op) {
    auto it = Reg().estimates.find(op);
    return it == Reg().estimates.end() ? nullptr : &it->second;
}

void RecordPruning(const LogicalNode& scan, const ScanPruning& info) {
    Reg().pruning[&scan] = info;
}

const ScanPruning* LookupPruning(const LogicalNode& scan) {
    auto it = Reg().pruning.find(&scan);
    return it == Reg().pruning.end() ? nullptr : &it->second;
}

const TableDef* AdoptPrunedTable(TableDef pruned) {
    Reg().owned_tables.push_back(std::make_unique<TableDef>(std::move(pruned)));
    return Reg().owned_tables.back().get();
}

}  // namespace quarry::planner
