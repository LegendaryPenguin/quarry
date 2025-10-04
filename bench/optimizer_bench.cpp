// What each optimizer rule is actually worth, measured by running the same query
// with exactly one rule switched off.
//
// Two numbers per rule per query: wall time end to end through Database::Execute,
// and bytes read off disk. Both are needed, because they can disagree - a rule can
// save bytes and no time, or save time by producing fewer rows for the layers above
// without changing what the scan reads.
//
// How the bytes are obtained, stated plainly because it is the one place this
// benchmark does not use the engine's own execution path: Database's physical
// planner constructs its segment readers internally, so there is no way to ask a
// completed Execute() how many bytes it read. Instead the query is bound and
// optimized with the same Rules, and the SCAN the optimizer produced - its pruned
// segment list, its projection, its pushed predicates - is then run through an
// instrumented reader. The scan is the only operator that touches the disk, so this
// is an exact byte count for the plan the optimizer chose, not an estimate.
//
// A rule that turns out not to matter on this data is reported as not mattering.
// That is the point of running the sweep rather than asserting the rules fire.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/parser/parser.hpp"
#include "bench_data.hpp"
#include "bench_util.hpp"
#include "datagen.hpp"
#include "quarry/catalog.hpp"
#include "quarry/database.hpp"
#include "quarry/plan.hpp"

using namespace quarry;
using namespace quarry::bench;

// kDbDirectory is file scope because RunQuery constructs a fresh Database per rule
// configuration - Options are fixed at construction - and the directory is the one
// thing every one of them shares.
std::string kDbDirectory;

namespace {

constexpr size_t kRows = 1'000'000;
constexpr size_t kRowsPerSegment = 16384;

// RuleSet names one configuration of Optimizer::Rules: either everything on, or
// everything on except one.
struct RuleSet {
    std::string label;
    Optimizer::Rules rules;
};

std::vector<RuleSet> RuleSets() {
    std::vector<RuleSet> out;
    out.push_back({"all rules on", {}});

    Optimizer::Rules r;
    r = {};
    r.predicate_pushdown = false;
    out.push_back({"predicate_pushdown off", r});
    r = {};
    r.projection_pruning = false;
    out.push_back({"projection_pruning off", r});
    r = {};
    r.constant_folding = false;
    out.push_back({"constant_folding off", r});
    r = {};
    r.index_selection = false;
    out.push_back({"index_selection off", r});
    r = {};
    r.segment_pruning = false;
    out.push_back({"segment_pruning off", r});
    return out;
}

// FindScanNode walks to the single scan at the bottom of a plan. These queries have
// one table, so there is exactly one.
const LogicalNode* FindScanNode(const LogicalNode* node) {
    if (node == nullptr) return nullptr;
    if (node->kind == LogicalKind::kScan) return node;
    for (const auto& child : node->children) {
        if (const LogicalNode* found = FindScanNode(child.get())) return found;
    }
    return nullptr;
}

// PlanOutcome is what the optimizer did, independently of how long the query took.
struct PlanOutcome {
    std::vector<std::string> applied;
    size_t projection_columns = 0;
    size_t pushed_predicates = 0;
    size_t segments_in_plan = 0;
    size_t segments_total = 0;
    std::string chosen_index;
    double estimated_rows = 0.0;
    uint64_t bytes_read = 0;
    uint64_t scan_rows_out = 0;
};

// MeasurePlan binds and optimizes the query with `rules`, then runs the resulting
// scan through a counting reader.
//
// The logical plan has to outlive the operator: the scan copies its segment list
// but the plan owns the TableDef the segments were pruned into.
PlanOutcome MeasurePlan(const Catalog& catalog, const std::string& sql,
                        const Optimizer::Rules& rules, size_t segments_total) {
    ast::Statement stmt = Take(parser::Parse(sql), "parse " + sql);
    if (stmt.kind != ast::StatementKind::kSelect || !stmt.select) {
        Fail("optimizer benchmark queries must be SELECTs: " + sql);
    }

    Binder binder(catalog);
    LogicalPtr bound = Take(binder.BindSelect(*stmt.select), "bind " + sql);

    Optimizer optimizer(rules);
    LogicalPtr plan = Take(optimizer.Optimize(std::move(bound)), "optimize " + sql);

    PlanOutcome out;
    out.applied = optimizer.applied_rules();
    out.segments_total = segments_total;

    const LogicalNode* scan = FindScanNode(plan.get());
    if (scan == nullptr || scan->table_def == nullptr) Fail("no scan in the plan for " + sql);

    out.projection_columns = scan->projection.size();
    out.pushed_predicates = scan->pushed_predicates.size();
    out.segments_in_plan = scan->table_def->segments.size();
    out.chosen_index = scan->chosen_index;
    out.estimated_rows = scan->estimated_rows;

    ReadCounters counters;
    auto op = std::make_unique<SequentialScanOperator>(
        *scan->table_def, scan->projection, scan->pushed_predicates,
        CountingFactory(scan->table_def->schema, &counters));
    out.scan_rows_out = Drain(*op).rows;
    out.bytes_read = counters.bytes_read;
    return out;
}

std::string Join(const std::vector<std::string>& parts) {
    if (parts.empty()) return "(none)";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ",";
        out += parts[i];
    }
    return out;
}

struct Query {
    std::string label;
    std::string sql;
    std::string why;
};

void RunQuery(const Catalog& catalog, const Query& query, size_t segments_total) {
    constexpr size_t kRuns = 3;

    std::cout << "\n--- " << query.label << " ---\n"
              << "  " << query.sql << "\n"
              << "  " << query.why << "\n\n";

    // Verify the query answers the same thing under every configuration before any
    // timing is reported. A rule that changes the answer is a bug, not a speedup,
    // and a benchmark that does not check is the reason such bugs survive.
    size_t expected_rows = 0;
    bool have_expected = false;

    Table t({"configuration", "rules that fired", "ms min/med/max", "vs all-on", "bytes read",
             "vs all-on", "proj cols", "pushed preds", "segments in plan", "scan rows", "index",
             "est rows"});

    double baseline_ns = 0.0;
    uint64_t baseline_bytes = 0;

    for (const RuleSet& set : RuleSets()) {
        Database::Options options;
        options.rules = set.rules;
        options.rows_per_segment = kRowsPerSegment;

        Database run(kDbDirectory, options);
        Check(run.Open(), "Database Open");

        size_t rows = 0;
        const Sample s = Time(set.label, kRuns, 1.0, [&] {
            ResultSet result = Take(run.Execute(query.sql), "execute " + query.sql);
            rows = result.row_count();
            Sink(rows);
        });
        Check(run.Close(), "Database Close");

        if (!have_expected) {
            expected_rows = rows;
            have_expected = true;
        } else if (rows != expected_rows) {
            Fail("configuration '" + set.label + "' returned " + std::to_string(rows) +
                 " rows but the baseline returned " + std::to_string(expected_rows) + " for " +
                 query.sql);
        }

        const PlanOutcome plan = MeasurePlan(catalog, query.sql, set.rules, segments_total);

        if (baseline_ns == 0.0) {
            baseline_ns = s.median_ns;
            baseline_bytes = plan.bytes_read;
        }

        t.Row({set.label, Join(plan.applied), MinMedMaxMs(s), Fmt(s.median_ns / baseline_ns) + "x",
               Thousands(plan.bytes_read),
               Fmt(static_cast<double>(plan.bytes_read) /
                   static_cast<double>(baseline_bytes == 0 ? 1 : baseline_bytes)) +
                   "x",
               std::to_string(plan.projection_columns), std::to_string(plan.pushed_predicates),
               std::to_string(plan.segments_in_plan) + "/" + std::to_string(plan.segments_total),
               Thousands(plan.scan_rows_out),
               plan.chosen_index.empty() ? "-" : plan.chosen_index,
               Fmt(plan.estimated_rows, 0)});
    }
    t.Print();
    std::cout << "  result rows (identical under every configuration): "
              << Thousands(expected_rows) << "\n";
}

}  // namespace

int main() {
    std::cout << "quarry optimizer benchmark\n";

    datagen::Spec spec;
    spec.rows = kRows;
    const datagen::Dataset data = datagen::Generate(spec);

    kDbDirectory = FreshDirectory("optimizer");

    // Written directly rather than through INSERT, for the reason bench_data.hpp
    // gives: a million rows of SQL text would cost more than every measurement here.
    {
        Catalog catalog(kDbDirectory);
        Check(catalog.Open(), "catalog Open");

        std::vector<ColumnData> cols(9);
        const TypeId types[9] = {TypeId::kInt64,  TypeId::kInt64,  TypeId::kVarchar,
                                 TypeId::kVarchar, TypeId::kInt64, TypeId::kDouble,
                                 TypeId::kInt64,  TypeId::kDouble, TypeId::kBoolean};
        for (size_t c = 0; c < 9; ++c) cols[c].type = types[c];
        for (const auto& r : data.rows) {
            cols[0].i64.push_back(r.id);
            cols[1].i64.push_back(r.ts);
            cols[2].str.push_back(r.category);
            cols[3].str.push_back(r.region);
            cols[4].i64.push_back(r.status);
            cols[5].f64.push_back(r.amount);
            cols[6].i64.push_back(r.quantity);
            cols[6].nulls.push_back(r.quantity_null);
            cols[7].f64.push_back(r.score);
            cols[7].nulls.push_back(r.score_null);
            cols[8].boolean.push_back(r.active ? 1 : 0);
        }
        WriteTable(catalog, datagen::kFactTable, datagen::FactSchema(), cols, kRowsPerSegment);
    }

    // The index is created through SQL, because CREATE INDEX is the only way to
    // register one and the catalog has no public builder for it.
    {
        Database::Options options;
        options.rows_per_segment = kRowsPerSegment;
        Database db(kDbDirectory, options);
        Check(db.Open(), "Database Open for CREATE INDEX");
        Take(db.Execute("CREATE INDEX events_id ON events (id)"), "CREATE INDEX");
        Check(db.Close(), "Database Close");
    }

    Catalog catalog(kDbDirectory);
    Check(catalog.Open(), "catalog Open after index");
    const TableDef& events = *Take(catalog.GetTable(datagen::kFactTable), "GetTable events");
    const size_t segments_total = events.segments.size();

    std::cout << "rows: " << Thousands(kRows) << " in " << Thousands(segments_total)
              << " segments\nindexes: " << events.indexes.size() << " (B+ tree on id)\n";

    Database::Options base_options;
    base_options.rows_per_segment = kRowsPerSegment;
    Database db(kDbDirectory, base_options);
    Check(db.Open(), "Database Open");

    const std::vector<Query> queries = {
        {"Q1 selective range on a correlated column",
         "SELECT id, amount FROM events WHERE id >= 500000 AND id < 502000",
         "two of nine columns, a pushable range, and a range that zone maps can prune"},
        {"Q2 aggregate over a low-cardinality predicate",
         "SELECT COUNT(*), SUM(amount) FROM events WHERE status = 3",
         "status repeats in 97-row runs, so every segment holds every value and nothing "
         "can be pruned"},
        {"Q3 predicate that needs folding",
         "SELECT id FROM events WHERE id < 100 * 20 + 1000",
         "the constant is an arithmetic expression, so without folding it cannot become a "
         "pushable column-op-constant predicate"},
        {"Q4 point lookup with an index available",
         "SELECT id, amount FROM events WHERE id = 123456",
         "an index on id exists, so index_selection has something to choose"},
        {"Q5 wide projection, unprunable random column",
         "SELECT id, ts, category, region, status, amount FROM events WHERE amount >= 4999.0 "
         "AND amount < 5001.0",
         "six of nine columns on a uniformly random predicate: the case where neither "
         "pruning nor a narrow projection has much to work with"},
    };

    for (const Query& q : queries) RunQuery(catalog, q, segments_total);

    Check(db.Close(), "Database Close");

    Note("\nRead the two 'vs all-on' columns together. A configuration whose time and\n"
         "bytes are both 1.00x is a rule that did nothing on this data - which is a\n"
         "result, not a gap in the benchmark. The 'rules that fired' column is the\n"
         "optimizer's own report of which rewrites changed the plan, so a rule that is\n"
         "switched on but absent from that list never applied in the first place.");
    std::cout << "\n";
    return 0;
}
