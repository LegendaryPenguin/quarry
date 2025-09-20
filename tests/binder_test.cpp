// Binder tests.
//
// Two halves. The first checks that a legal query produces the tree shape and the
// schemas the rest of the engine is entitled to assume. The second checks the
// rejections, and asserts on the message as well as the code: an error that does
// not name the thing that was wrong is only marginally better than no error.

#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/catalog.hpp"
#include "quarry/plan.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

ColumnChunk Chunk(TypeId type, Value min, Value max, uint64_t distinct) {
    ColumnChunk c;
    c.type = type;
    c.stats.min = std::move(min);
    c.stats.max = std::move(max);
    c.stats.distinct_estimate = distinct;
    c.stats.null_count = 0;
    c.stats.all_null = false;
    return c;
}

// Fixture owns the catalog so the TableDef pointers the binder stores stay valid
// for as long as the plan does.
struct Fixture {
    Catalog catalog{"/nonexistent-binder-test"};

    Fixture() {
        Schema schema;
        schema.Add(Column("id", TypeId::kInt64, false));
        schema.Add(Column("name", TypeId::kVarchar));
        schema.Add(Column("price", TypeId::kDouble));
        schema.Add(Column("qty", TypeId::kInt64));
        // CreateTable is in-memory until Save, so none of this touches the disk.
        CHECK_OK(catalog.CreateTable("items", schema));
        CHECK_OK(catalog.CreateTable("orders", Schema({Column("id", TypeId::kInt64)})));

        auto table = catalog.GetTableMutable("items");
        CHECK(table.ok());
        for (int s = 0; s < 2; ++s) {
            SegmentMeta seg;
            seg.id = static_cast<uint64_t>(s + 1);
            seg.row_count = 100;
            seg.columns.push_back(Chunk(TypeId::kInt64, Value::Int64(1 + s * 100),
                                        Value::Int64(100 + s * 100), 100));
            seg.columns.push_back(
                Chunk(TypeId::kVarchar, Value::Varchar("a"), Value::Varchar("z"), 10));
            seg.columns.push_back(
                Chunk(TypeId::kDouble, Value::Double(1.0), Value::Double(9.0), 50));
            seg.columns.push_back(Chunk(TypeId::kInt64, Value::Int64(0), Value::Int64(400), 40));
            table.value()->segments.push_back(std::move(seg));
        }
    }
};

ast::ExprPtr Col(const std::string& name) { return ast::Expression::ColumnRef(name); }
ast::ExprPtr Int(int64_t v) { return ast::Expression::Literal(Value::Int64(v)); }
ast::ExprPtr Dbl(double v) { return ast::Expression::Literal(Value::Double(v)); }
ast::ExprPtr Str(const std::string& v) {
    return ast::Expression::Literal(Value::Varchar(v));
}
ast::ExprPtr Bin(ast::BinaryOp op, ast::ExprPtr l, ast::ExprPtr r) {
    return ast::Expression::Binary(op, std::move(l), std::move(r));
}
ast::ExprPtr Agg(AggregateKind k, ast::ExprPtr arg, const std::string& alias = "") {
    auto e = ast::Expression::Aggregate(k, std::move(arg));
    e->alias = alias;
    return e;
}

bool Mentions(const Status& s, const std::string& needle) {
    return s.message().find(needle) != std::string::npos;
}

// Kinds flattens a chain plan top down, which is what the shape assertions compare
// against. Printing the chain rather than asserting node by node means a failure
// shows the whole wrong tree at once.
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

std::string SchemaNames(const Schema& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0) out += ",";
        out += s.at(i).name;
    }
    return out;
}

}  // namespace

QUARRY_TEST(BindProjectOverScan) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.projections.push_back(Col("price"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    CHECK_EQ(Kinds(*plan.value()), std::string("Project->Scan"));
    CHECK_EQ(SchemaNames(plan.value()->output_schema), std::string("id,price"));
    CHECK_EQ(static_cast<int>(plan.value()->output_schema.at(1).type),
             static_cast<int>(TypeId::kDouble));

    const LogicalNode* scan = Find(*plan.value(), LogicalKind::kScan);
    CHECK(scan != nullptr);
    if (scan == nullptr) return;
    // The binder does not prune; it hands the optimizer the whole table and lets
    // projection pruning be a rule that can be switched off and observed.
    CHECK_EQ(scan->projection.size(), static_cast<size_t>(4));
    CHECK_EQ(SchemaNames(scan->output_schema), std::string("id,name,price,qty"));
    CHECK_EQ(scan->estimated_rows, 200.0);
}

QUARRY_TEST(BindStarExpandsToTableColumns) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(ast::Expression::Star());

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    CHECK_EQ(plan.value()->projections.size(), static_cast<size_t>(4));
    CHECK_EQ(SchemaNames(plan.value()->output_schema), std::string("id,name,price,qty"));
    for (const auto& p : plan.value()->projections) {
        CHECK(p->kind == ast::ExprKind::kColumnRef);
    }
}

QUARRY_TEST(BindFilterBetweenScanAndProject) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.where = Bin(ast::BinaryOp::kGt, Col("qty"), Int(300));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    CHECK_EQ(Kinds(*plan.value()), std::string("Project->Filter->Scan"));
    const LogicalNode* filter = Find(*plan.value(), LogicalKind::kFilter);
    CHECK(filter != nullptr);
    if (filter == nullptr) return;
    // The estimate comes from the segment statistics, not from a constant: qty
    // spans [0, 400], so `qty > 300` keeps about a quarter of the rows.
    CHECK(filter->estimated_rows < 200.0);
    CHECK(filter->estimated_rows > 0.0);
    CHECK_EQ(SchemaNames(filter->output_schema), std::string("id,name,price,qty"));
}

QUARRY_TEST(BindAggregatePlacesSortAboveAggregate) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("name"));
    stmt.projections.push_back(Agg(AggregateKind::kCount, nullptr, "c"));
    stmt.group_by.push_back(Col("name"));
    stmt.order_by.push_back({Col("c"), true});
    stmt.limit = 5;

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    CHECK_EQ(Kinds(*plan.value()), std::string("Project->Limit->Sort->Aggregate->Scan"));

    const LogicalNode* agg = Find(*plan.value(), LogicalKind::kAggregate);
    CHECK(agg != nullptr);
    if (agg == nullptr) return;
    CHECK_EQ(agg->group_by.size(), static_cast<size_t>(1));
    CHECK_EQ(agg->group_by[0], static_cast<size_t>(1));  // name
    CHECK_EQ(agg->aggregates.size(), static_cast<size_t>(1));
    CHECK_EQ(agg->aggregates[0].output_name, std::string("c"));
    CHECK(!agg->aggregates[0].column_index.has_value());  // COUNT(*) reads no column
    CHECK_EQ(SchemaNames(agg->output_schema), std::string("name,c"));
    CHECK_EQ(static_cast<int>(agg->output_schema.at(1).type), static_cast<int>(TypeId::kInt64));

    const LogicalNode* sort = Find(*plan.value(), LogicalKind::kSort);
    CHECK(sort != nullptr);
    if (sort == nullptr) return;
    // Sorting on the aggregate's output is only expressible because Sort is above
    // the Aggregate: column 1 of the aggregate's schema does not exist below it.
    CHECK_EQ(sort->sort_keys.size(), static_cast<size_t>(1));
    CHECK_EQ(sort->sort_keys[0].column_index, static_cast<size_t>(1));
    CHECK(sort->sort_keys[0].descending);

    const LogicalNode* limit = Find(*plan.value(), LogicalKind::kLimit);
    CHECK(limit != nullptr);
    if (limit != nullptr) CHECK_EQ(limit->limit, static_cast<uint64_t>(5));
    CHECK_EQ(SchemaNames(plan.value()->output_schema), std::string("name,c"));
}

QUARRY_TEST(BindOrderByUnaliasedAggregate) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("name"));
    stmt.projections.push_back(Agg(AggregateKind::kSum, Col("qty")));
    stmt.group_by.push_back(Col("name"));
    stmt.order_by.push_back({Agg(AggregateKind::kSum, Col("qty")), false});

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    const LogicalNode* agg = Find(*plan.value(), LogicalKind::kAggregate);
    CHECK(agg != nullptr);
    if (agg == nullptr) return;
    // One accumulator, not two: the ORDER BY names the same aggregate the select
    // list already asked for.
    CHECK_EQ(agg->aggregates.size(), static_cast<size_t>(1));
    CHECK_EQ(agg->aggregates[0].output_name, std::string("SUM(qty)"));

    const LogicalNode* sort = Find(*plan.value(), LogicalKind::kSort);
    CHECK(sort != nullptr);
    if (sort != nullptr) CHECK_EQ(sort->sort_keys[0].column_index, static_cast<size_t>(1));
}

QUARRY_TEST(BindOrderByProjectionAlias) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    auto aliased = Col("qty");
    aliased->alias = "quantity";
    stmt.projections.push_back(std::move(aliased));
    stmt.order_by.push_back({Col("quantity"), false});

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;
    const LogicalNode* sort = Find(*plan.value(), LogicalKind::kSort);
    CHECK(sort != nullptr);
    if (sort != nullptr) CHECK_EQ(sort->sort_keys[0].column_index, static_cast<size_t>(3));
}

QUARRY_TEST(BindAllowsIntToDoubleComparison) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    // qty is int64 and the literal is a double: the one implicit conversion the
    // engine performs.
    stmt.where = Bin(ast::BinaryOp::kGe, Col("qty"), Dbl(2.5));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) {
        CHECK_EQ(plan.status().message(), std::string("(expected success)"));
    }
}

QUARRY_TEST(BindAllowsVarcharComparedToVarchar) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.where = Bin(ast::BinaryOp::kEq, Col("name"), Str("widget"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) CHECK_EQ(plan.status().message(), std::string("(expected success)"));
}

QUARRY_TEST(RejectUnknownTableAndListsWhatExists) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "widgets";
    stmt.projections.push_back(ast::Expression::Star());

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "widgets"));
    CHECK(Mentions(plan.status(), "items"));
    CHECK(Mentions(plan.status(), "orders"));
}

QUARRY_TEST(RejectUnknownColumnAndListsTheColumns) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("pricee"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "pricee"));
    CHECK(Mentions(plan.status(), "did you mean 'price'"));
    CHECK(Mentions(plan.status(), "id, name, price, qty"));
}

QUARRY_TEST(RejectBareColumnMixedWithAggregate) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("name"));
    stmt.projections.push_back(Agg(AggregateKind::kCount, nullptr));
    // No GROUP BY.

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "name"));
    CHECK(Mentions(plan.status(), "GROUP BY"));
}

QUARRY_TEST(RejectColumnNotInGroupBy) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("price"));
    stmt.projections.push_back(Agg(AggregateKind::kCount, nullptr));
    stmt.group_by.push_back(Col("name"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "price"));
}

QUARRY_TEST(RejectGroupByColumnOutOfScope) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Agg(AggregateKind::kCount, nullptr));
    stmt.group_by.push_back(Col("region"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "region"));
    CHECK(Mentions(plan.status(), "id, name, price, qty"));
}

QUARRY_TEST(RejectVarcharComparedToInt) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.where = Bin(ast::BinaryOp::kEq, Col("name"), Int(5));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kTypeError));
    CHECK(Mentions(plan.status(), "VARCHAR"));
    CHECK(Mentions(plan.status(), "BIGINT"));
    CHECK(Mentions(plan.status(), "name"));
}

QUARRY_TEST(RejectSumOverVarchar) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Agg(AggregateKind::kSum, Col("name")));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kTypeError));
    CHECK(Mentions(plan.status(), "name"));
}

QUARRY_TEST(RejectNestedAggregate) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(
        Agg(AggregateKind::kSum, Agg(AggregateKind::kCount, Col("qty"))));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "nested aggregate"));
    CHECK(Mentions(plan.status(), "COUNT(qty)"));
}

QUARRY_TEST(RejectAggregateInWhereAndSaysHaving) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("name"));
    stmt.where = Bin(ast::BinaryOp::kGt, Agg(AggregateKind::kCount, Col("qty")), Int(1));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    // NotImplemented rather than BindError: the query is meaningful SQL, it just
    // needs a clause this engine does not have.
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kNotImplemented));
    CHECK(Mentions(plan.status(), "HAVING"));
    CHECK(Mentions(plan.status(), "COUNT(qty)"));
}

QUARRY_TEST(RejectNonBooleanWhere) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.where = Col("qty");

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kTypeError));
    CHECK(Mentions(plan.status(), "WHERE"));
}

QUARRY_TEST(RejectUnknownOrderByColumn) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.order_by.push_back({Col("nope"), false});

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "nope"));
}

QUARRY_TEST(ExplainRendersTheBoundTree) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.where = Bin(ast::BinaryOp::kGt, Col("qty"), Int(300));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;

    std::string text = Explain(*plan.value());
    CHECK(text.find("Project id") != std::string::npos);
    CHECK(text.find("  Filter (qty > 300)") != std::string::npos);
    CHECK(text.find("    Scan items") != std::string::npos);
    CHECK(text.find("est_rows=") != std::string::npos);

    std::string with_stats = Explain(*plan.value(), true);
    CHECK(with_stats.find("est_cost=") != std::string::npos);
}

QUARRY_TEST(BindJoinProducesAJoinNodeOverTwoScans) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("items.id"));
    stmt.join = ast::JoinClause{"orders", "id", "id"};

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) {
        CHECK_EQ(plan.status().message(), std::string("(expected success)"));
        return;
    }

    const LogicalNode* join = Find(*plan.value(), LogicalKind::kJoin);
    CHECK(join != nullptr);
    if (join == nullptr) return;
    CHECK_EQ(join->children.size(), static_cast<size_t>(2));
    CHECK_EQ(join->children[0]->table, std::string("items"));
    CHECK_EQ(join->children[1]->table, std::string("orders"));
    CHECK_EQ(join->left_key, static_cast<size_t>(0));
    CHECK_EQ(join->right_key, static_cast<size_t>(0));
    // `id` exists on both sides, so the combined schema qualifies it rather than
    // publishing two columns with the same name.
    CHECK_EQ(SchemaNames(join->output_schema),
             std::string("items.id,name,price,qty,orders.id"));
    CHECK_EQ(SchemaNames(plan.value()->output_schema), std::string("items.id"));
}

QUARRY_TEST(RejectUnknownJoinTable) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("items.id"));
    stmt.join = ast::JoinClause{"nosuchtable", "id", "id"};

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "nosuchtable"));
}

QUARRY_TEST(RejectJoinKeysOfDifferentTypes) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("items.id"));
    // items.name is varchar and orders.id is int64.
    stmt.join = ast::JoinClause{"orders", "name", "id"};

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kTypeError));
    CHECK(Mentions(plan.status(), "VARCHAR"));
    CHECK(Mentions(plan.status(), "BIGINT"));
    CHECK(Mentions(plan.status(), "items.name"));
}

QUARRY_TEST(RejectAmbiguousColumnAcrossJoinedTables) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("id"));
    stmt.join = ast::JoinClause{"orders", "id", "id"};

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "ambiguous"));
    CHECK(Mentions(plan.status(), "items.id"));
    CHECK(Mentions(plan.status(), "orders.id"));
}

QUARRY_TEST(BindQualifiedColumnWithoutAJoin) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("items.price"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(plan.ok());
    if (!plan.ok()) return;
    // The qualifier resolves, and the output column keeps its bare name: with one
    // table in scope there is nothing for a qualifier to disambiguate.
    CHECK_EQ(SchemaNames(plan.value()->output_schema), std::string("price"));
}

QUARRY_TEST(RejectUnknownTableQualifier) {
    Fixture f;
    ast::SelectStatement stmt;
    stmt.from_table = "items";
    stmt.projections.push_back(Col("widgets.price"));

    Binder binder(f.catalog);
    auto plan = binder.BindSelect(stmt);
    CHECK(!plan.ok());
    if (plan.ok()) return;
    CHECK_EQ(static_cast<int>(plan.status().code()), static_cast<int>(Code::kBindError));
    CHECK(Mentions(plan.status(), "widgets"));
}

QUARRY_TEST_MAIN()
