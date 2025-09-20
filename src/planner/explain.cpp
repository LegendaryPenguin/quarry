// EXPLAIN rendering for both plan representations.
//
// The one thing this output exists to show is the gap between what the optimizer
// believed and what the query did. Estimates and measurements are therefore
// printed adjacent on the same line rather than in separate sections: a reader
// comparing "rows=12" in one block against "rows=1,400,000" in another has to hold
// both in their head, and that is exactly the comparison they came for.

#include <cstdio>
#include <string>
#include <vector>

#include "planner_internal.hpp"
#include "quarry/operator.hpp"
#include "quarry/plan.hpp"

namespace quarry {
namespace {

std::string FormatRows(double rows) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", rows);
    return buf;
}

// Predicates are formatted here rather than through Predicate::ToString so that
// EXPLAIN links without the execution layer. Source of truth for the shape being
// printed is Predicate in include/quarry/operator.hpp.
std::string FormatPredicate(const Predicate& p, const Schema& schema) {
    std::string name = p.column_index < schema.size() ? schema.at(p.column_index).name
                                                      : "#" + std::to_string(p.column_index);
    return name + " " + CompareOpName(p.op) + " " + p.constant.ToString();
}

std::string FormatProjection(const LogicalNode& scan) {
    const Schema* table_schema = scan.table_def != nullptr ? &scan.table_def->schema : nullptr;
    std::string out = "[";
    for (size_t i = 0; i < scan.projection.size(); ++i) {
        if (i > 0) out += ", ";
        size_t idx = scan.projection[i];
        if (table_schema != nullptr && idx < table_schema->size()) {
            out += table_schema->at(idx).name;
        } else {
            out += "#" + std::to_string(idx);
        }
    }
    return out + "]";
}

std::string DescribeScan(const LogicalNode& n) {
    std::string out = "Scan " + n.table;
    out += " projection=" + FormatProjection(n);

    if (!n.pushed_predicates.empty() && n.table_def != nullptr) {
        out += " pushed=[";
        for (size_t i = 0; i < n.pushed_predicates.size(); ++i) {
            if (i > 0) out += " AND ";
            out += FormatPredicate(n.pushed_predicates[i], n.table_def->schema);
        }
        out += "]";
    }
    if (!n.chosen_index.empty()) out += " index=" + n.chosen_index;

    if (const planner::ScanPruning* pruning = planner::LookupPruning(n); pruning != nullptr) {
        out += " segments=" + std::to_string(pruning->segments_total - pruning->segments_pruned) +
               "/" + std::to_string(pruning->segments_total) +
               " pruned=" + std::to_string(pruning->segments_pruned);
        if (pruning->unsatisfiable) out += " (predicate is never true)";
    } else if (n.table_def != nullptr) {
        out += " segments=" + std::to_string(n.table_def->segments.size());
    }
    return out;
}

std::string DescribeAggregate(const LogicalNode& n) {
    std::string out = "Aggregate";
    const Schema* input = n.children.empty() ? nullptr : &n.children.front()->output_schema;
    if (!n.group_by.empty()) {
        out += " group=[";
        for (size_t i = 0; i < n.group_by.size(); ++i) {
            if (i > 0) out += ", ";
            size_t idx = n.group_by[i];
            out += (input != nullptr && idx < input->size()) ? input->at(idx).name
                                                            : "#" + std::to_string(idx);
        }
        out += "]";
    }
    out += " aggs=[";
    for (size_t i = 0; i < n.aggregates.size(); ++i) {
        if (i > 0) out += ", ";
        out += n.aggregates[i].output_name;
    }
    out += "]";
    return out;
}

std::string DescribeSort(const LogicalNode& n) {
    std::string out = "Sort ";
    const Schema* input = n.children.empty() ? nullptr : &n.children.front()->output_schema;
    for (size_t i = 0; i < n.sort_keys.size(); ++i) {
        if (i > 0) out += ", ";
        size_t idx = n.sort_keys[i].column_index;
        out += (input != nullptr && idx < input->size()) ? input->at(idx).name
                                                        : "#" + std::to_string(idx);
        out += n.sort_keys[i].descending ? " DESC" : " ASC";
    }
    return out;
}

void Render(const LogicalNode& n, int depth, bool with_stats, std::string& out) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
    out += n.Describe();
    out += "  (est_rows=" + FormatRows(n.estimated_rows);
    if (with_stats) {
        // Measured counters live on the physical operators; a logical node never
        // runs, so the most this half of the pair can add is the cost that drove the
        // decisions. ExplainPhysical is where estimate meets measurement.
        out += " est_cost=" + FormatRows(n.estimated_cost);
    }
    out += ")\n";
    for (const auto& child : n.children) {
        if (child != nullptr) Render(*child, depth + 1, with_stats, out);
    }
}

void RenderPhysical(PhysicalOperator& op, int depth, bool with_stats, std::string& out) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
    out += op.Name();

    const double* estimate = planner::LookupEstimate(&op);
    const PhysicalOperator::Stats& s = op.stats();
    if (with_stats) {
        // Estimate first, measurement second, on the same line and in that order,
        // so a scan of the output reads as a column of predictions next to a column
        // of outcomes.
        out += "  (est_rows=";
        out += estimate != nullptr ? FormatRows(*estimate) : "?";
        out += " actual_rows=" + std::to_string(s.rows_out);
        out += " rows_in=" + std::to_string(s.rows_in);
        out += " batches=" + std::to_string(s.batches);
        out += " nanos=" + std::to_string(s.nanos);
        if (s.segments_scanned != 0 || s.segments_pruned != 0) {
            out += " segments_scanned=" + std::to_string(s.segments_scanned);
            out += " segments_pruned=" + std::to_string(s.segments_pruned);
        }
        out += ")";
    } else if (estimate != nullptr) {
        out += "  (est_rows=" + FormatRows(*estimate) + ")";
    }
    out += "\n";

    for (PhysicalOperator* child : op.Children()) {
        if (child != nullptr) RenderPhysical(*child, depth + 1, with_stats, out);
    }
}

}  // namespace

const char* LogicalKindName(LogicalKind k) {
    switch (k) {
        case LogicalKind::kScan:      return "Scan";
        case LogicalKind::kFilter:    return "Filter";
        case LogicalKind::kProject:   return "Project";
        case LogicalKind::kAggregate: return "Aggregate";
        case LogicalKind::kSort:      return "Sort";
        case LogicalKind::kLimit:     return "Limit";
        case LogicalKind::kJoin:      return "Join";
    }
    return "?";
}

std::string LogicalNode::Describe() const {
    switch (kind) {
        case LogicalKind::kScan:
            return DescribeScan(*this);
        case LogicalKind::kFilter:
            return "Filter " + (residual != nullptr ? residual->ToString() : std::string("(none)"));
        case LogicalKind::kProject: {
            std::string out = "Project ";
            for (size_t i = 0; i < projections.size(); ++i) {
                if (i > 0) out += ", ";
                out += projections[i] != nullptr ? projections[i]->ToString() : "?";
            }
            return out;
        }
        case LogicalKind::kAggregate:
            return DescribeAggregate(*this);
        case LogicalKind::kSort:
            return DescribeSort(*this);
        case LogicalKind::kLimit:
            return "Limit " + std::to_string(limit);
        case LogicalKind::kJoin:
            return "Join on #" + std::to_string(left_key) + " = #" + std::to_string(right_key);
    }
    return "?";
}

std::string Explain(const LogicalNode& root, bool with_stats) {
    std::string out;
    Render(root, 0, with_stats, out);
    return out;
}

std::string ExplainPhysical(PhysicalOperator& root, bool with_stats) {
    std::string out;
    RenderPhysical(root, 0, with_stats, out);
    return out;
}

}  // namespace quarry
