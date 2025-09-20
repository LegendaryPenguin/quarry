// The binder: a parsed SELECT in, a fully resolved logical plan out.
//
// Everything here is about turning names into positions and catching the errors
// that only a schema can catch. The parser accepted the text; the binder decides
// whether it means anything.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "planner_internal.hpp"
#include "quarry/plan.hpp"

namespace quarry {
namespace {

using planner::AggregateOutputName;
using planner::AggregateResultType;
using planner::AggregateSpelling;
using planner::BindContext;
using planner::CloneExpr;
using planner::ContainsAggregate;
using planner::InferType;

LogicalPtr MakeNode(LogicalKind kind) {
    auto node = std::make_unique<LogicalNode>();
    node->kind = kind;
    return node;
}

// CanonicalAggName is the aggregate's spelling ignoring any alias, which is what
// lets `ORDER BY COUNT(*)` find the output of `COUNT(*) AS c`: the two expression
// nodes differ only in the alias the projection happened to carry.
std::string CanonicalAggName(const ast::Expression& agg) {
    ast::Expression bare;
    bare.kind = ast::ExprKind::kAggregate;
    bare.agg = agg.agg;
    if (agg.agg_argument) bare.agg_argument = CloneExpr(*agg.agg_argument);
    return AggregateOutputName(bare);
}

// OutputName is the column name a projection contributes to the result schema.
std::string OutputName(const ast::Expression& e) {
    if (!e.alias.empty()) return e.alias;
    if (e.kind == ast::ExprKind::kColumnRef) return e.column_name;
    if (e.kind == ast::ExprKind::kAggregate) return AggregateOutputName(e);
    return e.ToString();
}

// --- SELECT * expansion ---------------------------------------------------

Result<std::vector<ast::ExprPtr>> ExpandProjections(
    const std::vector<ast::ExprPtr>& projections, const Schema& schema) {
    std::vector<ast::ExprPtr> out;
    for (const auto& p : projections) {
        if (p == nullptr) return Status::Internal("null projection from the parser");
        if (p->kind == ast::ExprKind::kStar) {
            if (schema.empty()) {
                return Status::BindError("SELECT * on a table with no columns");
            }
            for (const auto& col : schema.columns()) {
                out.push_back(ast::Expression::ColumnRef(col.name));
            }
            continue;
        }
        out.push_back(CloneExpr(*p));
    }
    if (out.empty()) return Status::BindError("SELECT with no projections");
    return out;
}

// --- aggregate collection -------------------------------------------------

struct AggregateBinding {
    std::string canonical;    // spelling ignoring the alias
    std::string output_name;  // alias when one was given
};

// CollectAggregates walks an expression and registers every aggregate call,
// deduplicating by canonical spelling so `SELECT COUNT(*), COUNT(*)` produces one
// accumulator rather than two.
Status CollectAggregates(const ast::Expression& e, const Schema& input,
                         const BindContext& ctx, std::vector<Aggregate>& out,
                         std::vector<AggregateBinding>& bindings) {
    if (e.kind == ast::ExprKind::kAggregate) {
        auto type = InferType(e, input, ctx);  // rejects nesting and bad argument types
        if (!type.ok()) return type.status();

        std::string canonical = CanonicalAggName(e);
        for (auto& existing : bindings) {
            if (existing.canonical == canonical) {
                // An alias seen later wins, so `COUNT(*), COUNT(*) AS c` still has
                // a name an ORDER BY can reach.
                if (!e.alias.empty()) {
                    for (auto& agg : out) {
                        if (agg.output_name == existing.output_name) agg.output_name = e.alias;
                    }
                    existing.output_name = e.alias;
                }
                return Status::Ok();
            }
        }

        Aggregate agg;
        agg.kind = e.agg;
        agg.output_name = e.alias.empty() ? canonical : e.alias;
        if (e.agg_argument != nullptr && e.agg_argument->kind != ast::ExprKind::kStar) {
            if (e.agg_argument->kind != ast::ExprKind::kColumnRef) {
                return Status::NotImplemented(
                    "aggregate argument must be a column, got " + e.agg_argument->ToString());
            }
            auto idx = input.IndexOf(e.agg_argument->column_name);
            if (!idx.has_value()) {
                return planner::UnknownColumnError(ctx, e.agg_argument->column_name);
            }
            agg.column_index = *idx;
        }
        out.push_back(std::move(agg));
        bindings.push_back({canonical, out.back().output_name});
        return Status::Ok();
    }

    if (e.left) QUARRY_TRY(CollectAggregates(*e.left, input, ctx, out, bindings));
    if (e.right) QUARRY_TRY(CollectAggregates(*e.right, input, ctx, out, bindings));
    return Status::Ok();
}

// CheckGrouped enforces the rule that every bare column outside an aggregate must
// be grouped. Aggregate subtrees are not descended into: a column inside SUM() is
// consumed by the aggregate and is not a bare reference.
//
// allow_unresolved is set for ORDER BY, whose names may legitimately not be table
// columns at all: an ORDER BY can name an aggregate's output or a projection alias,
// neither of which exists in the aggregate's input. Resolving those is
// ResolveSortKey's job, and reporting an unknown name is its error to raise.
Status CheckGrouped(const ast::Expression& e, const Schema& input, const BindContext& ctx,
                    const std::vector<size_t>& group_by, const char* clause,
                    bool allow_unresolved) {
    if (e.kind == ast::ExprKind::kAggregate) return Status::Ok();

    if (e.kind == ast::ExprKind::kColumnRef) {
        auto idx = input.IndexOf(e.column_name);
        if (!idx.has_value()) {
            if (allow_unresolved) return Status::Ok();
            return planner::UnknownColumnError(ctx, e.column_name);
        }
        if (std::find(group_by.begin(), group_by.end(), *idx) != group_by.end()) {
            return Status::Ok();
        }
        std::string msg = "column '" + e.column_name + "' in the " + clause +
                          " is neither grouped nor aggregated";
        if (group_by.empty()) {
            msg += "; the query uses an aggregate with no GROUP BY, so every other "
                   "expression must also be an aggregate";
        } else {
            msg += "; add it to GROUP BY or wrap it in an aggregate";
        }
        return Status::BindError(msg);
    }

    if (e.left) QUARRY_TRY(CheckGrouped(*e.left, input, ctx, group_by, clause, allow_unresolved));
    if (e.right) QUARRY_TRY(CheckGrouped(*e.right, input, ctx, group_by, clause, allow_unresolved));
    return Status::Ok();
}

// RewriteOverAggregate re-expresses a projection in terms of the aggregate node's
// output, so the Project above it only ever sees plain column references. This is
// what lets the physical projection operator stay free of aggregate handling.
ast::ExprPtr RewriteOverAggregate(const ast::Expression& e,
                                  const std::vector<AggregateBinding>& bindings) {
    if (e.kind == ast::ExprKind::kAggregate) {
        std::string canonical = CanonicalAggName(e);
        std::string name = canonical;
        for (const auto& b : bindings) {
            if (b.canonical == canonical) name = b.output_name;
        }
        auto ref = ast::Expression::ColumnRef(name);
        ref->alias = e.alias;
        return ref;
    }
    if (e.kind == ast::ExprKind::kBinary) {
        auto out = ast::Expression::Binary(e.op, RewriteOverAggregate(*e.left, bindings),
                                           RewriteOverAggregate(*e.right, bindings));
        out->alias = e.alias;
        return out;
    }
    return CloneExpr(e);
}

// --- ORDER BY resolution --------------------------------------------------

// ResolveSortKey resolves one ORDER BY item against the schema flowing into the
// Sort, which is the aggregate's output when there is one. That placement is the
// reason `ORDER BY COUNT(*)` works at all: the value being sorted on does not
// exist below the aggregate.
Result<size_t> ResolveSortKey(const ast::Expression& e, const Schema& input,
                              const BindContext& ctx,
                              const std::vector<ast::ExprPtr>& bound_projections) {
    // Aggregates have already been rewritten into references to the aggregate
    // node's output by the time this runs, so a column reference is all that is
    // left to resolve.
    if (e.kind != ast::ExprKind::kColumnRef) {
        return Status::NotImplemented(
            "ORDER BY accepts a column, an aggregate, or a projection alias, got " +
            e.ToString());
    }
    const std::string& name = e.column_name;

    if (auto idx = input.IndexOf(name); idx.has_value()) return *idx;

    // A projection alias is not in the input schema, because the Project sits
    // above the Sort. Follow the alias to the expression it names.
    for (const auto& p : bound_projections) {
        if (p->alias == name && p->kind == ast::ExprKind::kColumnRef) {
            if (auto idx = input.IndexOf(p->column_name); idx.has_value()) return *idx;
        }
    }

    BindContext sort_ctx = ctx;
    sort_ctx.schema = &input;
    return planner::UnknownColumnError(sort_ctx, name);
}

// --- name scope -----------------------------------------------------------

// Scope is the set of columns a query's expressions may refer to.
//
// With no join it is just the table. With one it is both tables, and a bare name
// that exists in both is ambiguous rather than silently the left one — so the
// combined schema spells a duplicated name as `table.column` and every column
// reference is rewritten to that canonical form before anything else runs. Doing
// the rewrite once, here, is what lets the rest of the binder keep using
// Schema::IndexOf on a plain name.
struct Scope {
    const TableDef* left = nullptr;
    const TableDef* right = nullptr;  // null when there is no join
    Schema combined;
    std::string label;

    bool Duplicated(const std::string& name) const {
        return right != nullptr && left->schema.IndexOf(name).has_value() &&
               right->schema.IndexOf(name).has_value();
    }

    std::string Canonical(const TableDef& owner, const std::string& name) const {
        return Duplicated(name) ? owner.name + "." + name : name;
    }

    // Resolve maps a name as written — bare or `table.column` — onto the combined
    // schema's name for it.
    Result<std::string> Resolve(const std::string& name) const {
        BindContext ctx{label, &combined};

        if (size_t dot = name.find('.'); dot != std::string::npos) {
            std::string qualifier = name.substr(0, dot);
            std::string column = name.substr(dot + 1);
            for (const TableDef* table : {left, right}) {
                if (table == nullptr || table->name != qualifier) continue;
                if (!table->schema.IndexOf(column).has_value()) {
                    BindContext table_ctx{table->name, &table->schema};
                    return planner::UnknownColumnError(table_ctx, column);
                }
                return Canonical(*table, column);
            }
            return Status::BindError("unknown table qualifier '" + qualifier + "' in '" + name +
                                     "'; the tables in scope are: " + label);
        }

        bool in_left = left->schema.IndexOf(name).has_value();
        bool in_right = right != nullptr && right->schema.IndexOf(name).has_value();
        if (in_left && in_right) {
            return Status::BindError("column '" + name + "' is ambiguous; qualify it as '" +
                                     left->name + "." + name + "' or '" + right->name + "." +
                                     name + "'");
        }
        if (in_left) return Canonical(*left, name);
        if (in_right) return Canonical(*right, name);
        return planner::UnknownColumnError(ctx, name);
    }
};

// QualifyColumns rewrites every column reference in a subtree to its canonical
// name, failing on the first one that cannot be resolved.
Status QualifyColumns(ast::Expression& e, const Scope& scope) {
    if (e.kind == ast::ExprKind::kColumnRef) {
        auto resolved = scope.Resolve(e.column_name);
        if (!resolved.ok()) return resolved.status();
        e.column_name = resolved.take();
        return Status::Ok();
    }
    if (e.left) QUARRY_TRY(QualifyColumns(*e.left, scope));
    if (e.right) QUARRY_TRY(QualifyColumns(*e.right, scope));
    if (e.agg_argument) QUARRY_TRY(QualifyColumns(*e.agg_argument, scope));
    return Status::Ok();
}

}  // namespace

Result<LogicalPtr> Binder::BindSelect(const ast::SelectStatement& stmt) {
    // --- the tables ---
    auto resolve_table = [&](const std::string& name) -> Result<const TableDef*> {
        if (!catalog_.HasTable(name)) {
            std::vector<std::string> names = catalog_.TableNames();
            std::string list;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) list += ", ";
                list += names[i];
            }
            return Status::BindError("unknown table '" + name + "'; available tables: " +
                                     (list.empty() ? "(none)" : list));
        }
        return catalog_.GetTable(name);
    };

    auto left_result = resolve_table(stmt.from_table);
    if (!left_result.ok()) return left_result.status();
    const TableDef* table = left_result.value();

    Scope scope;
    scope.left = table;
    scope.label = table->name;

    if (stmt.join.has_value()) {
        auto right_result = resolve_table(stmt.join->table);
        if (!right_result.ok()) return right_result.status();
        scope.right = right_result.value();
        scope.label = table->name + " JOIN " + scope.right->name;
    }

    for (const Column& col : table->schema.columns()) {
        scope.combined.Add(Column(scope.Canonical(*table, col.name), col.type, col.nullable));
    }
    if (scope.right != nullptr) {
        for (const Column& col : scope.right->schema.columns()) {
            scope.combined.Add(
                Column(scope.Canonical(*scope.right, col.name), col.type, col.nullable));
        }
    }

    const Schema& table_schema = scope.combined;
    BindContext ctx{scope.label, &table_schema};

    // --- Scan, and a Join when there are two of them ---
    auto make_scan = [](const TableDef& def) {
        auto scan = MakeNode(LogicalKind::kScan);
        scan->table = def.name;
        scan->table_def = &def;
        scan->projection.resize(def.schema.size());
        for (size_t i = 0; i < def.schema.size(); ++i) scan->projection[i] = i;
        scan->output_schema = def.schema;
        scan->estimated_rows = static_cast<double>(planner::TotalRows(def));
        return scan;
    };

    LogicalPtr top = make_scan(*table);

    if (scope.right != nullptr) {
        const ast::JoinClause& join = *stmt.join;
        auto left_key = table->schema.IndexOf(join.left_column);
        if (!left_key.has_value()) {
            BindContext left_ctx{table->name, &table->schema};
            return planner::UnknownColumnError(left_ctx, join.left_column);
        }
        auto right_key = scope.right->schema.IndexOf(join.right_column);
        if (!right_key.has_value()) {
            BindContext right_ctx{scope.right->name, &scope.right->schema};
            return planner::UnknownColumnError(right_ctx, join.right_column);
        }

        TypeId left_type = table->schema.at(*left_key).type;
        TypeId right_type = scope.right->schema.at(*right_key).type;
        if (!planner::TypesComparable(left_type, right_type)) {
            return Status::TypeError(
                "cannot join " + table->name + "." + join.left_column + " of type " +
                TypeName(left_type) + " to " + scope.right->name + "." + join.right_column +
                " of type " + TypeName(right_type) + ": a join key must be comparable");
        }

        auto join_node = MakeNode(LogicalKind::kJoin);
        join_node->left_key = *left_key;
        join_node->right_key = *right_key;
        join_node->output_schema = scope.combined;
        LogicalPtr right_scan = make_scan(*scope.right);

        // Inner equi-join row estimate: the textbook one, left rows times right rows
        // divided by the larger of the two keys' distinct counts. It is wrong exactly
        // when the key distributions are skewed, which is most of the time, and it is
        // printed next to the measurement for that reason.
        double left_rows = top->estimated_rows;
        double right_rows = right_scan->estimated_rows;
        double left_distinct =
            static_cast<double>(planner::WholeTableStats(*table, *left_key).distinct_estimate);
        double right_distinct = static_cast<double>(
            planner::WholeTableStats(*scope.right, *right_key).distinct_estimate);
        double divisor = std::max({left_distinct, right_distinct, 1.0});
        join_node->estimated_rows = left_rows * right_rows / divisor;

        join_node->children.push_back(std::move(top));
        join_node->children.push_back(std::move(right_scan));
        top = std::move(join_node);
    }

    // --- canonicalize every column reference, once ---
    //
    // Resolution happens here and nowhere else, so each clause below is a plain
    // Schema::IndexOf against the combined schema. ORDER BY is deliberately left
    // out: its names may be projection aliases or aggregate outputs, which do not
    // exist in this scope at all, and ResolveSortKey resolves those later.
    ast::ExprPtr where;
    if (stmt.where != nullptr) {
        where = CloneExpr(*stmt.where);
        QUARRY_TRY(QualifyColumns(*where, scope));
    }

    auto expanded = ExpandProjections(stmt.projections, table_schema);
    if (!expanded.ok()) return expanded.status();
    std::vector<ast::ExprPtr> projections = expanded.take();
    for (auto& p : projections) QUARRY_TRY(QualifyColumns(*p, scope));

    // --- Filter (WHERE) ---
    if (where != nullptr) {
        if (ContainsAggregate(*where)) {
            return Status::NotImplemented(
                "aggregate " + where->ToString() +
                " appears in WHERE; filtering on an aggregate requires HAVING, which this "
                "engine does not support");
        }
        auto type = InferType(*where, table_schema, ctx);
        if (!type.ok()) return type.status();
        if (type.value() != TypeId::kBoolean && type.value() != TypeId::kInvalid) {
            return Status::TypeError("WHERE requires a boolean expression, got " +
                                     std::string(TypeName(type.value())) + " in " +
                                     where->ToString());
        }

        auto filter = MakeNode(LogicalKind::kFilter);
        filter->output_schema = top->output_schema;
        filter->estimated_rows =
            top->estimated_rows * planner::EstimateSelectivity(*where, *table, table_schema);
        filter->residual = std::move(where);
        filter->children.push_back(std::move(top));
        top = std::move(filter);
    }

    // --- GROUP BY ---
    std::vector<size_t> group_by;
    for (const auto& g : stmt.group_by) {
        if (g == nullptr) return Status::Internal("null GROUP BY expression");
        if (g->kind != ast::ExprKind::kColumnRef) {
            return Status::NotImplemented("GROUP BY accepts a column name, got " + g->ToString());
        }
        auto resolved = scope.Resolve(g->column_name);
        if (!resolved.ok()) return resolved.status();
        auto idx = table_schema.IndexOf(resolved.value());
        if (!idx.has_value()) return planner::UnknownColumnError(ctx, g->column_name);
        if (std::find(group_by.begin(), group_by.end(), *idx) == group_by.end()) {
            group_by.push_back(*idx);
        }
    }

    bool has_aggregate = false;
    for (const auto& p : projections) has_aggregate |= ContainsAggregate(*p);
    for (const auto& o : stmt.order_by) {
        if (o.expr == nullptr) return Status::Internal("null ORDER BY expression");
        has_aggregate |= ContainsAggregate(*o.expr);
    }

    std::vector<AggregateBinding> bindings;

    // --- Aggregate ---
    if (has_aggregate || !group_by.empty()) {
        std::vector<Aggregate> aggregates;
        for (const auto& p : projections) {
            QUARRY_TRY(CollectAggregates(*p, table_schema, ctx, aggregates, bindings));
        }
        for (const auto& o : stmt.order_by) {
            QUARRY_TRY(CollectAggregates(*o.expr, table_schema, ctx, aggregates, bindings));
        }
        for (const auto& p : projections) {
            QUARRY_TRY(CheckGrouped(*p, table_schema, ctx, group_by, "select list", false));
        }
        for (const auto& o : stmt.order_by) {
            QUARRY_TRY(CheckGrouped(*o.expr, table_schema, ctx, group_by, "ORDER BY", true));
        }

        auto agg_node = MakeNode(LogicalKind::kAggregate);
        agg_node->group_by = group_by;
        agg_node->aggregates = std::move(aggregates);

        Schema out;
        for (size_t idx : group_by) out.Add(table_schema.at(idx));
        for (const auto& agg : agg_node->aggregates) {
            TypeId in = agg.column_index.has_value() ? table_schema.at(*agg.column_index).type
                                                     : TypeId::kInt64;
            // COUNT never yields NULL; the others do when a group is all nulls.
            bool nullable = agg.kind != AggregateKind::kCount;
            out.Add(Column(agg.output_name, AggregateResultType(agg.kind, in), nullable));
        }
        agg_node->output_schema = out;

        // Group count estimate: the product of the grouped columns' distinct
        // estimates, capped at the input row count. The product over-states when
        // the columns are correlated, which is the usual direction for this error.
        double groups = 1.0;
        for (size_t idx : group_by) {
            ColumnStats stats = planner::WholeTableStats(*table, idx);
            double distinct = stats.distinct_estimate > 0
                                  ? static_cast<double>(stats.distinct_estimate)
                                  : 1.0;
            groups *= distinct;
        }
        agg_node->estimated_rows = group_by.empty() ? 1.0 : std::min(groups, top->estimated_rows);
        agg_node->children.push_back(std::move(top));
        top = std::move(agg_node);

        for (auto& p : projections) p = RewriteOverAggregate(*p, bindings);
    }

    // --- Sort (ORDER BY) ---
    //
    // Sort sits above the Aggregate and below the Project: above the Aggregate
    // because ORDER BY may name an aggregate result, and below the Project so
    // ORDER BY may also name a column the select list does not return.
    if (!stmt.order_by.empty()) {
        auto sort = MakeNode(LogicalKind::kSort);
        for (const auto& item : stmt.order_by) {
            ast::ExprPtr key = has_aggregate || !group_by.empty()
                                   ? RewriteOverAggregate(*item.expr, bindings)
                                   : CloneExpr(*item.expr);
            auto idx = ResolveSortKey(*key, top->output_schema, ctx, projections);
            if (!idx.ok()) return idx.status();
            sort->sort_keys.push_back(SortKey{idx.value(), item.descending});
        }
        sort->output_schema = top->output_schema;
        sort->estimated_rows = top->estimated_rows;
        sort->children.push_back(std::move(top));
        top = std::move(sort);
    }

    // --- Limit ---
    if (stmt.limit.has_value()) {
        auto limit = MakeNode(LogicalKind::kLimit);
        limit->limit = *stmt.limit;
        limit->output_schema = top->output_schema;
        limit->estimated_rows = std::min(top->estimated_rows, static_cast<double>(*stmt.limit));
        limit->children.push_back(std::move(top));
        top = std::move(limit);
    }

    // --- Project ---
    //
    // Always emitted, even for SELECT *, so that the result schema has exactly one
    // owner. A plan whose output shape sometimes comes from the scan and sometimes
    // from a projection is a plan where every consumer has to know which.
    {
        auto project = MakeNode(LogicalKind::kProject);
        const Schema& input = top->output_schema;
        BindContext project_ctx{ctx.table, &input};
        Schema out;
        for (const auto& p : projections) {
            auto type = InferType(*p, input, project_ctx);
            if (!type.ok()) return type.status();

            bool nullable = true;
            if (p->kind == ast::ExprKind::kColumnRef) {
                auto idx = input.IndexOf(p->column_name);
                if (idx.has_value()) nullable = input.at(*idx).nullable;
            }
            out.Add(Column(OutputName(*p), type.value(), nullable));
        }
        project->output_schema = out;
        project->estimated_rows = top->estimated_rows;
        project->projections.reserve(projections.size());
        for (auto& p : projections) project->projections.push_back(std::move(p));
        project->children.push_back(std::move(top));
        top = std::move(project);
    }

    return top;
}

}  // namespace quarry
