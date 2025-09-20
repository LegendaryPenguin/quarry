// Optimizer tests.
//
// Every rule is exercised with the other four switched OFF. That is the only way
// to attribute a plan change to a rule: with all five on, a rule that does nothing
// is indistinguishable from a rule whose work another rule would have done anyway.
// Each test therefore asserts two things — the specific change, and that
// applied_rules() names exactly the one rule under test.
//
// The TableDef and SegmentMeta fixtures are built by hand rather than written and
// read back through src/storage, so the statistics the pruning and costing
// decisions are made from are exactly the ones the test chose.

#include <string>
#include <vector>

#include "../src/planner/planner_internal.hpp"
#include "quarry/ast.hpp"
#include "quarry/catalog.hpp"
#include "quarry/plan.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

// Column order for the `items` fixture: 0 id, 1 name, 2 price, 3 qty.
constexpr size_t kId = 0;
constexpr size_t kName = 1;
constexpr size_t kPrice = 2;
constexpr size_t kQty = 3;

Schema ItemsSchema() {
    Schema s;
    s.Add(Column("id", TypeId::kInt64, false));
    s.Add(Column("name", TypeId::kVarchar));
    s.Add(Column("price", TypeId::kDouble));
    s.Add(Column("qty", TypeId::kInt64));
    return s;
}

ColumnChunk Chunk(TypeId type, Value min, Value max, uint64_t distinct) {
    ColumnChunk c;
    c.type = type;
    c.stats.min = std::move(min);
    c.stats.max = std::move(max);
    c.stats.distinct_estimate = distinct;
    // null_count is zero on purpose: CanMatch refuses to prune a chunk that holds
    // any null, so a fixture with nulls could never demonstrate segment pruning.
    c.stats.null_count = 0;
    c.stats.all_null = false;
    return c;
}

// Segment builds one 100-row segment whose qty column spans [qty_lo, qty_hi]. The
// qty range is the only thing that varies between segments, so a pruning assertion
// has exactly one degree of freedom to explain it.
SegmentMeta Segment(uint64_t id, int64_t id_lo, int64_t qty_lo, int64_t qty_hi) {
    SegmentMeta seg;
    seg.id = id;
    seg.row_count = 100;
    seg.columns.push_back(
        Chunk(TypeId::kInt64, Value::Int64(id_lo), Value::Int64(id_lo + 99), 100));
    seg.columns.push_back(Chunk(TypeId::kVarchar, Value::Varchar("a"), Value::Varchar("z"), 10));
    seg.columns.push_back(Chunk(TypeId::kDouble, Value::Double(1.0), Value::Double(9.0), 50));
    seg.columns.push_back(Chunk(TypeId::kInt64, Value::Int64(qty_lo), Value::Int64(qty_hi), 50));
    return seg;
}

TableDef Items(bool with_indexes = false) {
    TableDef t;
    t.name = "items";
    t.schema = ItemsSchema();
    t.segments.push_back(Segment(1, 1, 0, 10));
    t.segments.push_back(Segment(2, 101, 100, 200));
    t.segments.push_back(Segment(3, 201, 300, 400));
    if (with_indexes) {
        t.indexes.push_back(IndexDef{"idx_id", kId, IndexKind::kBTree, ""});
        t.indexes.push_back(IndexDef{"idx_qty", kQty, IndexKind::kLsm, ""});
    }
    return t;
}

ast::ExprPtr Col(const std::string& name) { return ast::Expression::ColumnRef(name); }
ast::ExprPtr Int(int64_t v) { return ast::Expression::Literal(Value::Int64(v)); }
ast::ExprPtr Bin(ast::BinaryOp op, ast::ExprPtr l, ast::ExprPtr r) {
    return ast::Expression::Binary(op, std::move(l), std::move(r));
}

LogicalPtr Node(LogicalKind kind) {
    auto n = std::make_unique<LogicalNode>();
    n->kind = kind;
    return n;
}

LogicalPtr Scan(const TableDef& table) {
    auto scan = Node(LogicalKind::kScan);
    scan->table = table.name;
    scan->table_def = &table;
    scan->output_schema = table.schema;
    for (size_t i = 0; i < table.schema.size(); ++i) scan->projection.push_back(i);
    scan->estimated_rows = 300.0;
    return scan;
}

// Plan assembles Scan -> [Filter] -> Project over the given projected columns,
// which is the shape the binder produces for a single-table query.
LogicalPtr Plan(const TableDef& table, ast::ExprPtr where,
                const std::vector<std::string>& projected) {
    LogicalPtr top = Scan(table);
    if (where != nullptr) {
        auto filter = Node(LogicalKind::kFilter);
        filter->residual = std::move(where);
        filter->output_schema = top->output_schema;
        filter->estimated_rows = top->estimated_rows * 0.3;
        filter->children.push_back(std::move(top));
        top = std::move(filter);
    }
    auto project = Node(LogicalKind::kProject);
    Schema out;
    for (const std::string& name : projected) {
        project->projections.push_back(Col(name));
        auto idx = table.schema.IndexOf(name);
        CHECK(idx.has_value());
        if (idx.has_value()) out.Add(table.schema.at(*idx));
    }
    project->output_schema = out;
    project->estimated_rows = top->estimated_rows;
    project->children.push_back(std::move(top));
    return project;
}

// Only switches every rule off except the named one, so a test's claim about
// attribution is structural rather than a comment.
Optimizer::Rules Only(const std::string& rule) {
    Optimizer::Rules r;
    r.predicate_pushdown = rule == "predicate_pushdown";
    r.projection_pruning = rule == "projection_pruning";
    r.constant_folding = rule == "constant_folding";
    r.index_selection = rule == "index_selection";
    r.segment_pruning = rule == "segment_pruning";
    return r;
}

std::string Joined(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ",";
        out += v[i];
    }
    return out;
}

std::string Kinds(const LogicalNode& root) {
    std::string out;
    for (const LogicalNode* n = &root; n != nullptr;) {
        if (!out.empty()) out += "->";
        out += LogicalKindName(n->kind);
        n = n->children.empty() ? nullptr : n->children.front().get();
    }
    return out;
}

const LogicalNode* Find(const LogicalNode& root, LogicalKind kind) {
    for (const LogicalNode* n = &root; n != nullptr;) {
        if (n->kind == kind) return n;
        n = n->children.empty() ? nullptr : n->children.front().get();
    }
    return nullptr;
}

std::string ProjectedNames(const LogicalNode& scan) {
    std::string out;
    for (size_t i = 0; i < scan.projection.size(); ++i) {
        if (i > 0) out += ",";
        out += scan.table_def->schema.at(scan.projection[i]).name;
    }
    return out;
}

}  // namespace

// --- constant folding -----------------------------------------------------

QUARRY_TEST(FoldAlwaysTrueRemovesTheFilter) {
    TableDef table = Items();
    Optimizer opt(Only("constant_folding"));
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kEq, Int(1), Int(1)), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string("constant_folding"));

    // The scan is untouched: 1 = 1 constrains nothing.
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan != nullptr) CHECK_EQ(scan->table_def->segments.size(), static_cast<size_t>(3));
}

QUARRY_TEST(FoldAlwaysFalseEmptiesTheScan) {
    TableDef table = Items();
    Optimizer opt(Only("constant_folding"));
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kEq, Int(1), Int(0)), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string("constant_folding"));

    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    // The short circuit: a scan with no segments reads nothing and returns nothing.
    CHECK_EQ(scan->table_def->segments.size(), static_cast<size_t>(0));
    CHECK_EQ(scan->estimated_rows, 0.0);
    CHECK_EQ(result.value()->estimated_rows, 0.0);

    const planner::ScanPruning* pruning = planner::LookupPruning(*scan);
    CHECK(pruning != nullptr);
    if (pruning != nullptr) {
        CHECK(pruning->unsatisfiable);
        CHECK_EQ(pruning->segments_pruned, static_cast<size_t>(3));
    }
    CHECK(Explain(*result.value()).find("never true") != std::string::npos);
}

QUARRY_TEST(FoldSimplifiesAConjunctAway) {
    TableDef table = Items();
    auto where = Bin(ast::BinaryOp::kAnd, Bin(ast::BinaryOp::kGt, Col("qty"), Int(5)),
                     Bin(ast::BinaryOp::kEq, Int(2), Int(2)));
    Optimizer opt(Only("constant_folding"));
    auto result = opt.Optimize(Plan(table, std::move(where), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Filter->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string("constant_folding"));
    const LogicalNode* filter = Find(*result.value(), LogicalKind::kFilter);
    CHECK(filter != nullptr);
    if (filter != nullptr) {
        CHECK_EQ(filter->residual->ToString(), std::string("(qty > 5)"));
    }
}

QUARRY_TEST(FoldDoesNotRecordItselfWhenThereIsNothingToFold) {
    TableDef table = Items();
    Optimizer opt(Only("constant_folding"));
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kGt, Col("qty"), Int(5)), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Filter->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
}

// --- predicate pushdown ---------------------------------------------------

QUARRY_TEST(PushdownMovesEveryConjunctAndDropsTheFilter) {
    TableDef table = Items();
    auto where = Bin(ast::BinaryOp::kAnd, Bin(ast::BinaryOp::kGt, Col("qty"), Int(150)),
                     Bin(ast::BinaryOp::kLe, Col("id"), Int(250)));
    Optimizer opt(Only("predicate_pushdown"));
    auto result = opt.Optimize(Plan(table, std::move(where), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    // Nothing is left to evaluate per row, so the Filter is gone rather than left
    // behind holding a trivially-true residual.
    CHECK_EQ(Kinds(*result.value()), std::string("Project->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string("predicate_pushdown"));

    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    CHECK_EQ(scan->pushed_predicates.size(), static_cast<size_t>(2));
    CHECK_EQ(scan->pushed_predicates[0].column_index, kQty);
    CHECK_EQ(static_cast<int>(scan->pushed_predicates[0].op), static_cast<int>(CompareOp::kGt));
    CHECK_EQ(scan->pushed_predicates[1].column_index, kId);
    CHECK_EQ(static_cast<int>(scan->pushed_predicates[1].op), static_cast<int>(CompareOp::kLe));
    CHECK(scan->estimated_rows < 300.0);
}

QUARRY_TEST(PushdownFlipsConstantOnTheLeft) {
    TableDef table = Items();
    Optimizer opt(Only("predicate_pushdown"));
    // Written `150 < qty`; the canonical form is `qty > 150`.
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kLt, Int(150), Col("qty")), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("predicate_pushdown"));
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    CHECK_EQ(scan->pushed_predicates.size(), static_cast<size_t>(1));
    CHECK_EQ(scan->pushed_predicates[0].column_index, kQty);
    CHECK_EQ(static_cast<int>(scan->pushed_predicates[0].op), static_cast<int>(CompareOp::kGt));
    CHECK_EQ(scan->pushed_predicates[0].constant.as_int64(), static_cast<int64_t>(150));
}

QUARRY_TEST(PushdownLeavesAResidualForWhatDoesNotFit) {
    TableDef table = Items();
    // The second conjunct compares two columns, which no zone map or index can
    // answer, so it has to stay in a Filter.
    auto where = Bin(ast::BinaryOp::kAnd, Bin(ast::BinaryOp::kGt, Col("qty"), Int(150)),
                     Bin(ast::BinaryOp::kGt, Col("price"), Col("id")));
    Optimizer opt(Only("predicate_pushdown"));
    auto result = opt.Optimize(Plan(table, std::move(where), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Filter->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string("predicate_pushdown"));
    const LogicalNode* filter = Find(*result.value(), LogicalKind::kFilter);
    CHECK(filter != nullptr);
    if (filter != nullptr) CHECK_EQ(filter->residual->ToString(), std::string("(price > id)"));
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    if (scan != nullptr) CHECK_EQ(scan->pushed_predicates.size(), static_cast<size_t>(1));
}

QUARRY_TEST(PushdownDoesNotRecordItselfWhenNothingIsPushable) {
    TableDef table = Items();
    Optimizer opt(Only("predicate_pushdown"));
    auto result =
        opt.Optimize(Plan(table, Bin(ast::BinaryOp::kGt, Col("price"), Col("id")), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Filter->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    if (scan != nullptr) CHECK(scan->pushed_predicates.empty());
}

QUARRY_TEST(PushdownRefusesAnOr) {
    TableDef table = Items();
    // Either side may be false for a given row, so neither can be pushed on its own.
    auto where = Bin(ast::BinaryOp::kOr, Bin(ast::BinaryOp::kGt, Col("qty"), Int(150)),
                     Bin(ast::BinaryOp::kLt, Col("id"), Int(5)));
    Optimizer opt(Only("predicate_pushdown"));
    auto result = opt.Optimize(Plan(table, std::move(where), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
}

// --- projection pruning ---------------------------------------------------

QUARRY_TEST(PruneProjectionToExactlyTheColumnsUsed) {
    TableDef table = Items();
    Optimizer opt(Only("projection_pruning"));
    // SELECT id FROM items WHERE qty > 5 must read id and qty, and nothing else.
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kGt, Col("qty"), Int(5)), {"id"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("projection_pruning"));
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    CHECK_EQ(ProjectedNames(*scan), std::string("id,qty"));
    CHECK_EQ(scan->output_schema.size(), static_cast<size_t>(2));

    // Every node below the Project republishes the narrowed schema, because the
    // physical layer sizes its vectors from it.
    const LogicalNode* filter = Find(*result.value(), LogicalKind::kFilter);
    CHECK(filter != nullptr);
    if (filter != nullptr) CHECK_EQ(filter->output_schema.size(), static_cast<size_t>(2));
    // The query's own output shape is unchanged.
    CHECK_EQ(result.value()->output_schema.size(), static_cast<size_t>(1));
}

QUARRY_TEST(PruneProjectionKeepsPushedPredicateColumns) {
    TableDef table = Items();
    // Standing in for a run where pushdown already fired: the predicate column must
    // survive pruning even though no expression above the scan mentions it.
    LogicalPtr plan = Plan(table, nullptr, {"name"});
    LogicalNode* scan = plan->children.front().get();
    scan->pushed_predicates.push_back(Predicate{kQty, CompareOp::kGt, Value::Int64(150)});

    Optimizer opt(Only("projection_pruning"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("projection_pruning"));
    const LogicalNode* pruned = Find(*result.value(), LogicalKind::kScan);
    CHECK(pruned != nullptr);
    if (pruned != nullptr) CHECK_EQ(ProjectedNames(*pruned), std::string("name,qty"));
}

QUARRY_TEST(PruneProjectionDoesNotRecordItselfWhenEveryColumnIsUsed) {
    TableDef table = Items();
    Optimizer opt(Only("projection_pruning"));
    auto result = opt.Optimize(Plan(table, Bin(ast::BinaryOp::kGt, Col("qty"), Int(5)),
                                    {"id", "name", "price"}));
    CHECK(result.ok());
    if (!result.ok()) return;
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    if (scan != nullptr) CHECK_EQ(ProjectedNames(*scan), std::string("id,name,price,qty"));
}

QUARRY_TEST(PruneProjectionRemapsAggregateAndSortIndexes) {
    TableDef table = Items();

    // Scan -> Aggregate(group by name, SUM(qty)) -> Sort on the aggregate output.
    LogicalPtr scan = Scan(table);
    auto agg = Node(LogicalKind::kAggregate);
    agg->group_by.push_back(kName);
    Aggregate sum;
    sum.kind = AggregateKind::kSum;
    sum.column_index = kQty;
    sum.output_name = "SUM(qty)";
    agg->aggregates.push_back(sum);
    Schema agg_out;
    agg_out.Add(Column("name", TypeId::kVarchar));
    agg_out.Add(Column("SUM(qty)", TypeId::kInt64));
    agg->output_schema = agg_out;
    agg->estimated_rows = 10.0;
    agg->children.push_back(std::move(scan));

    auto sort = Node(LogicalKind::kSort);
    sort->sort_keys.push_back(SortKey{1, true});
    sort->output_schema = agg_out;
    sort->estimated_rows = 10.0;
    sort->children.push_back(std::move(agg));

    auto project = Node(LogicalKind::kProject);
    project->projections.push_back(Col("SUM(qty)"));
    project->output_schema = Schema({Column("SUM(qty)", TypeId::kInt64)});
    project->children.push_back(std::move(sort));

    Optimizer opt(Only("projection_pruning"));
    auto result = opt.Optimize(std::move(project));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("projection_pruning"));
    const LogicalNode* pruned = Find(*result.value(), LogicalKind::kScan);
    CHECK(pruned != nullptr);
    if (pruned != nullptr) CHECK_EQ(ProjectedNames(*pruned), std::string("name,qty"));

    const LogicalNode* bound_agg = Find(*result.value(), LogicalKind::kAggregate);
    CHECK(bound_agg != nullptr);
    if (bound_agg != nullptr) {
        // name moved from table position 1 to scan-output position 0, and qty from 3
        // to 1. A rule that narrowed the scan without remapping these would read the
        // wrong columns and still produce a plausible-looking answer.
        CHECK_EQ(bound_agg->group_by[0], static_cast<size_t>(0));
        CHECK(bound_agg->aggregates[0].column_index.has_value());
        if (bound_agg->aggregates[0].column_index.has_value()) {
            CHECK_EQ(*bound_agg->aggregates[0].column_index, static_cast<size_t>(1));
        }
    }

    // The Sort is above the Aggregate, so it addresses the aggregate's own schema
    // and must NOT be remapped.
    const LogicalNode* bound_sort = Find(*result.value(), LogicalKind::kSort);
    CHECK(bound_sort != nullptr);
    if (bound_sort != nullptr) {
        CHECK_EQ(bound_sort->sort_keys[0].column_index, static_cast<size_t>(1));
    }
}

// --- segment pruning ------------------------------------------------------

QUARRY_TEST(PruneSegmentsThatCannotMatch) {
    TableDef table = Items();
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    // qty ranges are [0,10], [100,200], [300,400]; only the third can hold a value
    // above 250.
    scan->pushed_predicates.push_back(Predicate{kQty, CompareOp::kGt, Value::Int64(250)});

    Optimizer opt(Only("segment_pruning"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("segment_pruning"));
    const LogicalNode* pruned = Find(*result.value(), LogicalKind::kScan);
    CHECK(pruned != nullptr);
    if (pruned == nullptr) return;
    CHECK_EQ(pruned->table_def->segments.size(), static_cast<size_t>(1));
    CHECK_EQ(pruned->table_def->segments[0].id, static_cast<uint64_t>(3));

    const planner::ScanPruning* info = planner::LookupPruning(*pruned);
    CHECK(info != nullptr);
    if (info != nullptr) {
        CHECK_EQ(info->segments_total, static_cast<size_t>(3));
        CHECK_EQ(info->segments_pruned, static_cast<size_t>(2));
        CHECK(!info->unsatisfiable);
    }
    // Two of three segments gone means a third of the rows survive. Compared with a
    // tolerance because the estimate is scaled by a ratio, not recounted.
    CHECK(pruned->estimated_rows > 99.9 && pruned->estimated_rows < 100.1);
    CHECK(Explain(*result.value()).find("segments=1/3 pruned=2") != std::string::npos);
}

QUARRY_TEST(PruneSegmentsDoesNotRecordItselfWhenEverySegmentMatches) {
    TableDef table = Items();
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    // Every segment's qty range reaches above 5.
    scan->pushed_predicates.push_back(Predicate{kQty, CompareOp::kGt, Value::Int64(5)});

    Optimizer opt(Only("segment_pruning"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
    const LogicalNode* kept = Find(*result.value(), LogicalKind::kScan);
    if (kept != nullptr) CHECK_EQ(kept->table_def->segments.size(), static_cast<size_t>(3));
}

QUARRY_TEST(PruneSegmentsUsesEverySegmentsOwnStats) {
    TableDef table = Items();
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    // An equality inside the middle segment's window and outside the other two.
    scan->pushed_predicates.push_back(Predicate{kQty, CompareOp::kEq, Value::Int64(150)});

    Optimizer opt(Only("segment_pruning"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    const LogicalNode* pruned = Find(*result.value(), LogicalKind::kScan);
    CHECK(pruned != nullptr);
    if (pruned == nullptr) return;
    CHECK_EQ(pruned->table_def->segments.size(), static_cast<size_t>(1));
    CHECK_EQ(pruned->table_def->segments[0].id, static_cast<uint64_t>(2));
}

// --- index selection ------------------------------------------------------

QUARRY_TEST(IndexChosenForASelectivePredicate) {
    TableDef table = Items(true);
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    // id has 100 distinct values per the stats, so an equality keeps about 1%:
    // index_cost = log2(301) * 2 + 300 * 0.01 * 4 * 4 = 64, against a scan cost of
    // 300 * 4 = 1200.
    scan->pushed_predicates.push_back(Predicate{kId, CompareOp::kEq, Value::Int64(42)});

    Optimizer opt(Only("index_selection"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string("index_selection"));
    const LogicalNode* chosen = Find(*result.value(), LogicalKind::kScan);
    CHECK(chosen != nullptr);
    if (chosen != nullptr) {
        CHECK_EQ(chosen->chosen_index, std::string("idx_id"));
        CHECK(chosen->estimated_cost < 1200.0);
    }
}

QUARRY_TEST(FullScanWinsForAnUnselectivePredicate) {
    TableDef table = Items(true);
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    // qty spans [0, 400] across the table, so `qty > 10` keeps nearly everything.
    // There is an index on qty, and it still loses: 300 random fetches cost more
    // than 300 sequential ones.
    scan->pushed_predicates.push_back(Predicate{kQty, CompareOp::kGt, Value::Int64(10)});

    Optimizer opt(Only("index_selection"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
    const LogicalNode* chosen = Find(*result.value(), LogicalKind::kScan);
    if (chosen != nullptr) CHECK(chosen->chosen_index.empty());
}

QUARRY_TEST(NoIndexConsideredForAnUnindexedColumn) {
    TableDef table = Items(true);
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    scan->pushed_predicates.push_back(Predicate{kPrice, CompareOp::kEq, Value::Double(3.0)});

    Optimizer opt(Only("index_selection"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
}

QUARRY_TEST(NotEqualNeverChoosesAnIndex) {
    TableDef table = Items(true);
    LogicalPtr plan = Plan(table, nullptr, {"id"});
    LogicalNode* scan = plan->children.front().get();
    scan->pushed_predicates.push_back(Predicate{kId, CompareOp::kNe, Value::Int64(42)});

    Optimizer opt(Only("index_selection"));
    auto result = opt.Optimize(std::move(plan));
    CHECK(result.ok());
    if (!result.ok()) return;
    CHECK_EQ(Joined(opt.applied_rules()), std::string(""));
}

// --- all rules together ---------------------------------------------------

QUARRY_TEST(AllRulesTogetherOnOneQuery) {
    TableDef table = Items(true);
    // SELECT name FROM items WHERE qty > 250 AND 1 = 1
    auto where = Bin(ast::BinaryOp::kAnd, Bin(ast::BinaryOp::kGt, Col("qty"), Int(250)),
                     Bin(ast::BinaryOp::kEq, Int(1), Int(1)));
    Optimizer opt;  // every rule on
    auto result = opt.Optimize(Plan(table, std::move(where), {"name"}));
    CHECK(result.ok());
    if (!result.ok()) return;

    CHECK_EQ(Kinds(*result.value()), std::string("Project->Scan"));
    CHECK_EQ(Joined(opt.applied_rules()),
             std::string("constant_folding,predicate_pushdown,projection_pruning,segment_pruning"));

    const LogicalNode* scan = Find(*result.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    CHECK_EQ(ProjectedNames(*scan), std::string("name,qty"));
    CHECK_EQ(scan->table_def->segments.size(), static_cast<size_t>(1));
    CHECK_EQ(scan->pushed_predicates.size(), static_cast<size_t>(1));
}

QUARRY_TEST(OptimizeRejectsANullPlan) {
    Optimizer opt;
    auto result = opt.Optimize(nullptr);
    CHECK(!result.ok());
    if (!result.ok()) {
        CHECK_EQ(static_cast<int>(result.status().code()),
                 static_cast<int>(Code::kInvalidArgument));
    }
}

QUARRY_TEST_MAIN()
