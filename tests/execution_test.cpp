// Tests for the vectorized execution operators.
//
// Every operator is driven by MockSource rather than by the storage layer, so a
// failure here is an operator bug and nothing else. MockSource also does the one
// thing a real scan does that is easy to forget: it hands out batches whose
// vectors physically contain rows that are NOT selected. An operator that iterates
// 0..row_count() instead of Rows() passes against dense input and fails here.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/catalog.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/storage.hpp"
#include "quarry/types.hpp"
#include "testing.hpp"

#include "../src/execution/aggregate.hpp"
#include "../src/execution/exec_util.hpp"
#include "../src/execution/expression.hpp"
#include "../src/execution/filter.hpp"
#include "../src/execution/hash_join.hpp"
#include "../src/execution/limit.hpp"
#include "../src/execution/project.hpp"
#include "../src/execution/scan.hpp"
#include "../src/execution/sort.hpp"

using namespace quarry;  // NOLINT: a test file is exactly where this is fine
using quarry::ast::BinaryOp;
using quarry::ast::Expression;

namespace {

using Row = std::vector<Value>;
using Rows = std::vector<Row>;

Value N() { return Value::Null(); }
Value I(int64_t v) { return Value::Int64(v); }
Value D(double v) { return Value::Double(v); }
Value S(const char* v) { return Value::Varchar(v); }

Schema MakeSchema(std::vector<Column> columns) { return Schema(std::move(columns)); }

std::ostream& operator<<(std::ostream& os, TypeId t) { return os << TypeName(t); }

// ---------------------------------------------------------------------------
// MockSource: a leaf operator that replays prepared batches.
// ---------------------------------------------------------------------------
class MockSource : public PhysicalOperator {
public:
    explicit MockSource(Schema schema) : schema_(std::move(schema)) {}

    // `selection`, when present, names which of the supplied rows are live. The
    // others are still written into the batch's vectors, exactly as a filter
    // upstream of this point would leave them.
    void AddBatch(Rows rows, std::optional<std::vector<uint32_t>> selection = std::nullopt) {
        batches_.push_back({std::move(rows), std::move(selection)});
    }

    const Schema& output_schema() const override { return schema_; }

    Status Open() override {
        position_ = 0;
        next_calls_ = 0;
        opened_ = true;
        return Status::Ok();
    }

    Status Next(Batch& out, bool& done) override {
        ++next_calls_;
        done = false;
        QUARRY_TRY(PrepareOutput(out, schema_));
        if (position_ >= batches_.size()) {
            done = true;
            return Status::Ok();
        }
        const Prepared& prepared = batches_[position_++];
        for (const Row& row : prepared.rows) {
            for (size_t c = 0; c < schema_.size(); ++c) {
                QUARRY_TRY(AppendValue(out.column(c), row[c]));
            }
        }
        out.SetRowCount(prepared.rows.size());
        if (prepared.selection) out.SetSelection(*prepared.selection);
        return Status::Ok();
    }

    Status Close() override {
        closed_ = true;
        return Status::Ok();
    }

    std::string Name() const override { return "MockSource"; }

    size_t next_calls() const { return next_calls_; }
    bool closed() const { return closed_; }
    bool opened() const { return opened_; }

private:
    struct Prepared {
        Rows rows;
        std::optional<std::vector<uint32_t>> selection;
    };

    Schema schema_;
    std::vector<Prepared> batches_;
    size_t position_ = 0;
    size_t next_calls_ = 0;
    bool closed_ = false;
    bool opened_ = false;
};

// ---------------------------------------------------------------------------
// Draining an operator into comparable text.
// ---------------------------------------------------------------------------

// Rendering a row as text rather than comparing Values is deliberate: NULL is not
// equal to NULL under Value::operator==, so a Value-wise comparison would silently
// accept a NULL where a number was expected.
std::string RowText(const Batch& batch, uint32_t row) {
    std::string out;
    for (size_t c = 0; c < batch.column_count(); ++c) {
        if (c) out += ",";
        out += batch.column(c).GetValue(row).ToString();
    }
    return out;
}

struct Drained {
    std::vector<std::string> rows;
    size_t batches = 0;
    size_t empty_batches = 0;  // returned with zero selected rows and done == false
};

Drained Drain(PhysicalOperator& op, const char* file, int line) {
    Drained result;
    Batch out;
    bool done = false;
    size_t guard = 0;
    while (!done) {
        if (++guard > 100000) {
            ::quarry::testing::ReportFailure(file, line, "operator did not terminate");
            break;
        }
        Status s = op.Next(out, done);
        if (!s.ok()) {
            ::quarry::testing::ReportFailure(file, line, "Next failed: " + s.ToString());
            break;
        }
        if (done && out.selected_count() == 0) break;
        ++result.batches;
        if (out.selected_count() == 0) {
            ++result.empty_batches;
            continue;
        }
        for (uint32_t row : out.Rows()) result.rows.push_back(RowText(out, row));
    }
    return result;
}

#define DRAIN(op) Drain((op), __FILE__, __LINE__)

void CheckRows(const Drained& got, const std::vector<std::string>& want, const char* file,
               int line) {
    if (got.rows == want) return;
    std::string message = "rows differ\n          got:  ";
    for (const auto& r : got.rows) message += "[" + r + "]";
    message += "\n          want: ";
    for (const auto& r : want) message += "[" + r + "]";
    ::quarry::testing::ReportFailure(file, line, message);
}

// Variadic because the expected rows are written as a braced list, whose commas
// would otherwise be read as macro argument separators.
#define CHECK_ROWS(got, ...) \
    CheckRows((got), std::vector<std::string>(__VA_ARGS__), __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// A segment reader over in-memory rows, so the scan can be tested without the
// storage layer.
// ---------------------------------------------------------------------------
class MockSegmentReader : public SegmentReader {
public:
    MockSegmentReader(Rows rows, std::vector<size_t> projection, size_t rows_per_batch)
        : rows_(std::move(rows)),
          projection_(std::move(projection)),
          rows_per_batch_(rows_per_batch) {}

    Status Open() override { return Status::Ok(); }

    Status Next(Batch& out, bool& done) override {
        done = false;
        const size_t end = std::min(rows_.size(), position_ + rows_per_batch_);
        for (size_t r = position_; r < end; ++r) {
            for (size_t c = 0; c < projection_.size(); ++c) {
                QUARRY_TRY(AppendValue(out.column(c), rows_[r][projection_[c]]));
                bytes_read_ += 8;
            }
        }
        out.SetRowCount(end - position_);
        position_ = end;
        done = position_ >= rows_.size();
        return Status::Ok();
    }

    Status Close() override { return Status::Ok(); }
    uint64_t bytes_read() const override { return bytes_read_; }

private:
    Rows rows_;
    std::vector<size_t> projection_;
    size_t rows_per_batch_;
    size_t position_ = 0;
    uint64_t bytes_read_ = 0;
};

// MakeSegment derives the zone map from the rows, the way a segment writer would.
SegmentMeta MakeSegment(uint64_t id, const Schema& schema, const Rows& rows) {
    SegmentMeta meta;
    meta.id = id;
    meta.row_count = rows.size();
    meta.path = "mem://" + std::to_string(id);
    for (size_t c = 0; c < schema.size(); ++c) {
        ColumnChunk chunk;
        chunk.type = schema.at(c).type;
        chunk.encoding = Encoding::kPlain;
        bool any = false;
        for (const Row& row : rows) {
            const Value& v = row[c];
            if (v.is_null()) {
                ++chunk.stats.null_count;
                continue;
            }
            if (!any) {
                chunk.stats.min = v;
                chunk.stats.max = v;
                any = true;
            } else {
                if (v < chunk.stats.min) chunk.stats.min = v;
                if (chunk.stats.max < v) chunk.stats.max = v;
            }
        }
        chunk.stats.all_null = !any;
        meta.columns.push_back(chunk);
    }
    return meta;
}

// SegmentSource owns the rows behind a set of segments and hands out readers,
// counting how many were actually opened — which is how pruning is proved.
class SegmentSource {
public:
    SegmentSource(Schema schema, size_t rows_per_batch = kBatchSize)
        : schema_(std::move(schema)), rows_per_batch_(rows_per_batch) {}

    void Add(Rows rows) {
        segments_.push_back(MakeSegment(segments_.size() + 1, schema_, rows));
        data_.push_back(std::move(rows));
    }

    const std::vector<SegmentMeta>& segments() const { return segments_; }
    size_t readers_opened() const { return readers_opened_; }
    const std::vector<size_t>& last_projection() const { return last_projection_; }

    SequentialScanOperator::ReaderFactory Factory() {
        return [this](const SegmentMeta& meta, const std::vector<size_t>& projection)
                   -> Result<std::unique_ptr<SegmentReader>> {
            ++readers_opened_;
            last_projection_ = projection;
            return std::unique_ptr<SegmentReader>(
                new MockSegmentReader(data_[meta.id - 1], projection, rows_per_batch_));
        };
    }

private:
    Schema schema_;
    size_t rows_per_batch_;
    std::vector<SegmentMeta> segments_;
    std::vector<Rows> data_;
    size_t readers_opened_ = 0;
    std::vector<size_t> last_projection_;
};

Aggregate Agg(AggregateKind kind, std::optional<size_t> column, const char* name) {
    Aggregate a;
    a.kind = kind;
    a.column_index = column;
    a.output_name = name;
    return a;
}

std::unique_ptr<MockSource> IntSource(const char* column_name = "a") {
    return std::make_unique<MockSource>(MakeSchema({Column(column_name, TypeId::kInt64)}));
}

}  // namespace

// ===========================================================================
// Scan
// ===========================================================================

QUARRY_TEST(ScanPrunesSegmentsWhoseZoneMapCannotMatch) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    SegmentSource source(schema);
    source.Add({{I(1)}, {I(2)}, {I(3)}});
    source.Add({{I(100)}, {I(101)}});
    source.Add({{I(200)}, {I(300)}});

    Predicate pred;
    pred.column_index = 0;
    pred.op = CompareOp::kGt;
    pred.constant = I(150);

    SequentialScanOperator scan(schema, source.segments(), {0}, {pred}, source.Factory());
    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());

    CHECK_ROWS(got, {"200", "300"});
    CHECK_EQ(scan.stats().segments_pruned, uint64_t{2});
    CHECK_EQ(scan.stats().segments_scanned, uint64_t{1});
    // Pruning has to mean "did not read", not "read and discarded".
    CHECK_EQ(source.readers_opened(), size_t{1});
}

QUARRY_TEST(ScanWithNoPredicateReadsEverySegment) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    SegmentSource source(schema);
    source.Add({{I(1)}});
    source.Add({{I(2)}});

    SequentialScanOperator scan(schema, source.segments(), {0}, {}, source.Factory());
    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());

    CHECK_ROWS(got, {"1", "2"});
    CHECK_EQ(scan.stats().segments_pruned, uint64_t{0});
    CHECK_EQ(scan.stats().segments_scanned, uint64_t{2});
}

QUARRY_TEST(ScanReadsOnlyProjectedColumns) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kVarchar),
                                      Column("c", TypeId::kDouble)});
    SegmentSource source(schema);
    source.Add({{I(1), S("x"), D(1.5)}, {I(2), S("y"), D(2.5)}});

    SequentialScanOperator scan(schema, source.segments(), {2, 0}, {}, source.Factory());
    CHECK_EQ(scan.output_schema().size(), size_t{2});
    CHECK_EQ(scan.output_schema().at(0).name, std::string("c"));
    CHECK_EQ(scan.output_schema().at(1).name, std::string("a"));

    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());
    CHECK_ROWS(got, {"1.500000,1", "2.500000,2"});
    CHECK_EQ(source.last_projection().size(), size_t{2});
}

QUARRY_TEST(ScanAppliesPushedPredicatesWithinASegment) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kInt64)});
    SegmentSource source(schema);
    source.Add({{I(1), I(10)}, {I(5), I(50)}, {N(), I(70)}, {I(9), I(90)}});

    Predicate pred;
    pred.column_index = 0;
    pred.op = CompareOp::kGe;
    pred.constant = I(5);

    SequentialScanOperator scan(schema, source.segments(), {0, 1}, {pred}, source.Factory());
    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());

    // The NULL row is dropped: a comparison against NULL is unknown, not true.
    CHECK_ROWS(got, {"5,50", "9,90"});
    CHECK_EQ(scan.stats().rows_in, uint64_t{4});
    CHECK_EQ(scan.stats().rows_out, uint64_t{2});
}

QUARRY_TEST(ScanOverMultipleBatchesPerSegment) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    SegmentSource source(schema, /*rows_per_batch=*/2);
    Rows rows;
    std::vector<std::string> want;
    for (int64_t i = 0; i < 7; ++i) {
        rows.push_back({I(i)});
        want.push_back(std::to_string(i));
    }
    source.Add(std::move(rows));

    SequentialScanOperator scan(schema, source.segments(), {0}, {}, source.Factory());
    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());
    CHECK_ROWS(got, want);
    CHECK_EQ(got.batches, size_t{4});
}

QUARRY_TEST(ScanOverNoSegmentsIsImmediatelyDone) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    SegmentSource source(schema);

    SequentialScanOperator scan(schema, source.segments(), {0}, {}, source.Factory());
    CHECK_OK(scan.Open());
    Batch out;
    bool done = false;
    CHECK_OK(scan.Next(out, done));
    CHECK(done);
    CHECK_EQ(out.selected_count(), size_t{0});
    CHECK_OK(scan.Close());
}

QUARRY_TEST(ScanRejectsAPredicateOnAnUnreadColumn) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kInt64)});
    SegmentSource source(schema);
    source.Add({{I(1), I(2)}});

    Predicate pred;
    pred.column_index = 1;  // not in the projection below
    pred.op = CompareOp::kEq;
    pred.constant = I(2);

    SequentialScanOperator scan(schema, source.segments(), {0}, {pred}, source.Factory());
    CHECK_NOT_OK(scan.Open());
    CHECK_OK(scan.Close());  // Close must be safe after a failed Open
}

QUARRY_TEST(ScanFromATableDef) {
    TableDef table;
    table.name = "t";
    table.schema = MakeSchema({Column("a", TypeId::kInt64)});
    SegmentSource source(table.schema);
    source.Add({{I(42)}});
    table.segments = source.segments();

    SequentialScanOperator scan(table, {0}, {}, source.Factory());
    CHECK_OK(scan.Open());
    const Drained got = DRAIN(scan);
    CHECK_OK(scan.Close());
    CHECK_ROWS(got, {"42"});
}

// ===========================================================================
// Filter
// ===========================================================================

QUARRY_TEST(FilterSelectsMatchingRowsAcrossBatches) {
    auto source = IntSource();
    source->AddBatch({{I(1)}, {I(10)}, {I(2)}});
    source->AddBatch({{I(20)}, {I(3)}});
    MockSource* raw = source.get();

    auto pred = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(5)));
    FilterOperator filter(std::move(source), pred.get());
    CHECK_OK(filter.Open());
    const Drained got = DRAIN(filter);
    CHECK_OK(filter.Close());

    CHECK_ROWS(got, {"10", "20"});
    CHECK(raw->closed());
    CHECK_EQ(filter.stats().rows_in, uint64_t{5});
    CHECK_EQ(filter.stats().rows_out, uint64_t{2});
}

QUARRY_TEST(FilterRejectingAWholeBatchReturnsZeroRowsAndKeepsGoing) {
    auto source = IntSource();
    source->AddBatch({{I(1)}, {I(2)}});  // nothing here passes
    source->AddBatch({{I(100)}});
    MockSource* raw = source.get();

    auto pred = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(50)));
    FilterOperator filter(std::move(source), pred.get());
    CHECK_OK(filter.Open());

    Batch out;
    bool done = false;
    CHECK_OK(filter.Next(out, done));
    // The contract: zero selected rows with done == false means "ask again", not
    // end of stream.
    CHECK_EQ(out.selected_count(), size_t{0});
    CHECK_EQ(out.row_count(), size_t{2});  // the rows are still physically there
    CHECK(!done);

    CHECK_OK(filter.Next(out, done));
    CHECK_EQ(out.selected_count(), size_t{1});
    CHECK(!done);

    CHECK_OK(filter.Next(out, done));
    CHECK(done);
    CHECK_OK(filter.Close());
    CHECK(raw->closed());
}

QUARRY_TEST(FilterIntersectsAnExistingSelection) {
    auto source = IntSource();
    // Rows 1 and 3 are dead on arrival; both would pass the predicate if the
    // filter forgot to look at the selection.
    source->AddBatch({{I(10)}, {I(20)}, {I(30)}, {I(40)}}, std::vector<uint32_t>{0, 2});
    MockSource* raw = source.get();

    auto pred = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(5)));
    FilterOperator filter(std::move(source), pred.get());
    CHECK_OK(filter.Open());
    const Drained got = DRAIN(filter);
    CHECK_OK(filter.Close());
    CHECK_ROWS(got, {"10", "30"});
    CHECK(raw->opened());
}

QUARRY_TEST(FilterOverEmptyInput) {
    auto source = IntSource();
    auto pred = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(0)));
    FilterOperator filter(std::move(source), pred.get());
    CHECK_OK(filter.Open());
    const Drained got = DRAIN(filter);
    CHECK_OK(filter.Close());
    CHECK_EQ(got.rows.size(), size_t{0});
}

// ===========================================================================
// The headline correctness rule
// ===========================================================================

QUARRY_TEST(OperatorsIgnoreRowsThatAreNotSelected) {
    // Physical rows 1 and 3 hold huge values and are NOT selected. Any operator
    // that loops 0..row_count() instead of over Rows() will fold them in, so each
    // of the numbers below is wrong in a recognizable way if that happens:
    // the sum becomes 2003 instead of 3, the count 4 instead of 2, the max 1000.
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});

    const auto fresh_source = [&]() {
        auto s = std::make_unique<MockSource>(schema);
        s->AddBatch({{I(1)}, {I(1000)}, {I(2)}, {I(1000)}}, std::vector<uint32_t>{0, 2});
        return s;
    };

    HashAggregateOperator agg(fresh_source(), {},
                              {Agg(AggregateKind::kCount, std::nullopt, "n"),
                               Agg(AggregateKind::kCount, 0, "n_a"),
                               Agg(AggregateKind::kSum, 0, "total"),
                               Agg(AggregateKind::kMax, 0, "biggest"),
                               Agg(AggregateKind::kAvg, 0, "mean")});
    CHECK_OK(agg.Open());
    CHECK_ROWS(DRAIN(agg), {"2,2,3,2,1.500000"});
    CHECK_OK(agg.Close());

    auto a = Expression::ColumnRef("a");
    auto project = ProjectionOperator::Create(fresh_source(), {a.get()});
    CHECK_OK(project.ok() ? Status::Ok() : project.status());
    CHECK_OK(project.value()->Open());
    const Drained projected = DRAIN(*project.value());
    CHECK_OK(project.value()->Close());
    CHECK_ROWS(projected, {"1", "2"});

    SortOperator sort(fresh_source(), {SortKey{0, true}});
    CHECK_OK(sort.Open());
    CHECK_ROWS(DRAIN(sort), {"2", "1"});
    CHECK_OK(sort.Close());

    LimitOperator limit(fresh_source(), 10);
    CHECK_OK(limit.Open());
    CHECK_ROWS(DRAIN(limit), {"1", "2"});
    CHECK_OK(limit.Close());

    // Both sides of a join must respect it too, build side included.
    auto build = std::make_unique<MockSource>(MakeSchema({Column("b", TypeId::kInt64)}));
    build->AddBatch({{I(1)}, {I(1000)}}, std::vector<uint32_t>{0});
    HashJoinOperator join(fresh_source(), std::move(build), 0, 0);
    CHECK_OK(join.Open());
    CHECK_ROWS(DRAIN(join), {"1,1"});
    CHECK_OK(join.Close());
}

// ===========================================================================
// Projection
// ===========================================================================

QUARRY_TEST(ProjectionEvaluatesExpressionsAndNamesColumns) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kDouble)}));
    source->AddBatch({{I(1), D(0.5)}, {I(2), D(1.5)}});
    source->AddBatch({{I(3), D(2.5)}});

    auto a = Expression::ColumnRef("a");
    auto doubled = Expression::Binary(BinaryOp::kMul, Expression::ColumnRef("a"),
                                      Expression::Literal(I(2)));
    doubled->alias = "twice_a";
    auto sum = Expression::Binary(BinaryOp::kAdd, Expression::ColumnRef("a"),
                                  Expression::ColumnRef("b"));

    auto created = ProjectionOperator::Create(std::move(source),
                                              {a.get(), doubled.get(), sum.get()});
    CHECK_OK(created.ok() ? Status::Ok() : created.status());
    ProjectionOperator& project = *created.value();

    CHECK_EQ(project.output_schema().size(), size_t{3});
    CHECK_EQ(project.output_schema().at(0).name, std::string("a"));
    CHECK_EQ(project.output_schema().at(1).name, std::string("twice_a"));
    CHECK_EQ(project.output_schema().at(1).type, TypeId::kInt64);
    CHECK_EQ(project.output_schema().at(2).type, TypeId::kDouble);

    CHECK_OK(project.Open());
    const Drained got = DRAIN(project);
    CHECK_OK(project.Close());
    CHECK_ROWS(got, {"1,2,1.500000", "2,4,3.500000", "3,6,5.500000"});
}

QUARRY_TEST(ProjectionOverEmptyInput) {
    auto source = IntSource();
    auto a = Expression::ColumnRef("a");
    auto created = ProjectionOperator::Create(std::move(source), {a.get()});
    CHECK_OK(created.ok() ? Status::Ok() : created.status());
    CHECK_OK(created.value()->Open());
    const Drained got = DRAIN(*created.value());
    CHECK_OK(created.value()->Close());
    CHECK_EQ(got.rows.size(), size_t{0});
}

QUARRY_TEST(ProjectionRejectsAnUnknownColumnAtPlanTime) {
    auto source = IntSource();
    auto missing = Expression::ColumnRef("nope");
    auto created = ProjectionOperator::Create(std::move(source), {missing.get()});
    CHECK(!created.ok());
    CHECK_EQ(static_cast<int>(created.status().code()), static_cast<int>(Code::kBindError));
}

// ===========================================================================
// Aggregate
// ===========================================================================

QUARRY_TEST(AggregateWithoutGroupByOverNullsFollowsSqlRules) {
    auto source = IntSource();
    source->AddBatch({{I(4)}, {N()}, {I(6)}, {N()}});

    HashAggregateOperator agg(std::move(source), {},
                              {Agg(AggregateKind::kCount, std::nullopt, "rows"),
                               Agg(AggregateKind::kCount, 0, "values"),
                               Agg(AggregateKind::kSum, 0, "total"),
                               Agg(AggregateKind::kAvg, 0, "mean"),
                               Agg(AggregateKind::kMin, 0, "lo"),
                               Agg(AggregateKind::kMax, 0, "hi")});
    CHECK_OK(agg.Open());
    // COUNT(*) counts all four rows; COUNT(a) counts the two non-null ones; AVG is
    // 10/2 and not 10/4.
    CHECK_ROWS(DRAIN(agg), {"4,2,10,5.000000,4,6"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(AggregateOverAnAllNullColumn) {
    auto source = IntSource();
    source->AddBatch({{N()}, {N()}});

    HashAggregateOperator agg(std::move(source), {},
                              {Agg(AggregateKind::kCount, std::nullopt, "rows"),
                               Agg(AggregateKind::kCount, 0, "values"),
                               Agg(AggregateKind::kSum, 0, "total"),
                               Agg(AggregateKind::kAvg, 0, "mean"),
                               Agg(AggregateKind::kMin, 0, "lo"),
                               Agg(AggregateKind::kMax, 0, "hi")});
    CHECK_OK(agg.Open());
    // COUNT is 0 but SUM, AVG, MIN and MAX are all NULL: an absent sum is not zero.
    CHECK_ROWS(DRAIN(agg), {"2,0,NULL,NULL,NULL,NULL"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(AggregateOverEmptyInputStillProducesOneRow) {
    auto source = IntSource();
    HashAggregateOperator agg(std::move(source), {},
                              {Agg(AggregateKind::kCount, std::nullopt, "rows"),
                               Agg(AggregateKind::kSum, 0, "total")});
    CHECK_OK(agg.Open());
    CHECK_ROWS(DRAIN(agg), {"0,NULL"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(GroupByPutsEveryNullInOneGroup) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("k", TypeId::kVarchar), Column("v", TypeId::kInt64)}));
    source->AddBatch({{S("a"), I(1)}, {N(), I(10)}, {S("b"), I(2)}});
    source->AddBatch({{N(), I(100)}, {S("a"), I(3)}, {N(), I(1000)}});

    HashAggregateOperator agg(std::move(source), {0},
                              {Agg(AggregateKind::kCount, std::nullopt, "n"),
                               Agg(AggregateKind::kSum, 1, "total")});
    CHECK_OK(agg.Open());
    // Grouping treats NULLs as equal even though comparison does not, so the three
    // null-keyed rows form a single group rather than three groups of one.
    CHECK_ROWS(DRAIN(agg), {"a,2,4", "NULL,3,1110", "b,1,2"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(GroupByOnMultipleColumns) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("region", TypeId::kVarchar), Column("year", TypeId::kInt64),
                    Column("amount", TypeId::kDouble)}));
    source->AddBatch({{S("east"), I(2020), D(1.0)},
                      {S("east"), I(2021), D(2.0)},
                      {S("west"), I(2020), D(4.0)}});
    source->AddBatch({{S("east"), I(2020), D(8.0)},
                      {S("west"), I(2020), N()},
                      {S("east"), N(), D(16.0)}});

    HashAggregateOperator agg(std::move(source), {0, 1},
                              {Agg(AggregateKind::kCount, std::nullopt, "n"),
                               Agg(AggregateKind::kSum, 2, "total")});
    CHECK_OK(agg.Open());
    CHECK_ROWS(DRAIN(agg), {"east,2020,2,9.000000", "east,2021,1,2.000000",
                            "west,2020,2,4.000000", "east,NULL,1,16.000000"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(GroupByOverEmptyInputProducesNoRows) {
    auto source = IntSource();
    HashAggregateOperator agg(std::move(source), {0},
                              {Agg(AggregateKind::kCount, std::nullopt, "n")});
    CHECK_OK(agg.Open());
    // Unlike a global aggregate, a grouped one has nothing to report.
    CHECK_EQ(DRAIN(agg).rows.size(), size_t{0});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(AggregateOverVarcharMinMaxAndDoubleSum) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("s", TypeId::kVarchar), Column("d", TypeId::kDouble)}));
    source->AddBatch({{S("pear"), D(1.25)}, {S("apple"), D(2.5)}, {S("quince"), N()}});

    HashAggregateOperator agg(std::move(source), {},
                              {Agg(AggregateKind::kMin, 0, "lo"),
                               Agg(AggregateKind::kMax, 0, "hi"),
                               Agg(AggregateKind::kSum, 1, "total"),
                               Agg(AggregateKind::kAvg, 1, "mean")});
    CHECK_EQ(agg.output_schema().at(0).type, TypeId::kVarchar);
    CHECK_EQ(agg.output_schema().at(2).type, TypeId::kDouble);
    CHECK_OK(agg.Open());
    CHECK_ROWS(DRAIN(agg), {"apple,quince,3.750000,1.875000"});
    CHECK_OK(agg.Close());
}

QUARRY_TEST(AggregateRejectsSumOfAVarcharColumn) {
    auto source = std::make_unique<MockSource>(MakeSchema({Column("s", TypeId::kVarchar)}));
    HashAggregateOperator agg(std::move(source), {}, {Agg(AggregateKind::kSum, 0, "total")});
    Status s = agg.Open();
    CHECK(!s.ok());
    CHECK_EQ(static_cast<int>(s.code()), static_cast<int>(Code::kTypeError));
    CHECK_OK(agg.Close());
}

QUARRY_TEST(AggregateEmitsManyGroupsAcrossSeveralBatches) {
    auto source = IntSource("k");
    // More distinct groups than fit in one output batch, so the emit path has to
    // resume where it left off.
    const int64_t groups = static_cast<int64_t>(kBatchSize) + 37;
    for (int64_t base = 0; base < groups; base += 500) {
        Rows rows;
        for (int64_t k = base; k < std::min(base + 500, groups); ++k) rows.push_back({I(k)});
        source->AddBatch(std::move(rows));
    }

    HashAggregateOperator agg(std::move(source), {0},
                              {Agg(AggregateKind::kCount, std::nullopt, "n")});
    CHECK_OK(agg.Open());
    const Drained got = DRAIN(agg);
    CHECK_OK(agg.Close());
    CHECK_EQ(got.rows.size(), static_cast<size_t>(groups));
    CHECK_EQ(got.batches, size_t{2});
    CHECK_EQ(got.rows.front(), std::string("0,1"));
}

// ===========================================================================
// Sort
// ===========================================================================

QUARRY_TEST(SortAscendingPutsNullsFirstAndDescendingPutsThemLast) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    const Rows rows = {{I(3)}, {N()}, {I(1)}, {I(2)}, {N()}};

    auto ascending = std::make_unique<MockSource>(schema);
    ascending->AddBatch(rows);
    SortOperator asc(std::move(ascending), {SortKey{0, false}});
    CHECK_OK(asc.Open());
    CHECK_ROWS(DRAIN(asc), {"NULL", "NULL", "1", "2", "3"});
    CHECK_OK(asc.Close());

    auto descending = std::make_unique<MockSource>(schema);
    descending->AddBatch(rows);
    SortOperator desc(std::move(descending), {SortKey{0, true}});
    CHECK_OK(desc.Open());
    // DOCUMENTED: a NULL sorts as if smaller than any value — SQLite's rule, which
    // the differential tests hold this engine to — so reversing the direction
    // reverses the whole order, nulls included.
    CHECK_ROWS(DRAIN(desc), {"3", "2", "1", "NULL", "NULL"});
    CHECK_OK(desc.Close());
}

QUARRY_TEST(SortByMultipleKeysWithMixedDirections) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("g", TypeId::kVarchar), Column("n", TypeId::kInt64)}));
    source->AddBatch({{S("b"), I(1)}, {S("a"), I(2)}, {S("b"), I(3)}});
    source->AddBatch({{S("a"), I(1)}, {S("b"), I(2)}});

    SortOperator sort(std::move(source), {SortKey{0, false}, SortKey{1, true}});
    CHECK_OK(sort.Open());
    CHECK_ROWS(DRAIN(sort), {"a,2", "a,1", "b,3", "b,2", "b,1"});
    CHECK_OK(sort.Close());
}

QUARRY_TEST(SortIsStableOnTies) {
    auto source = std::make_unique<MockSource>(
        MakeSchema({Column("k", TypeId::kInt64), Column("tag", TypeId::kVarchar)}));
    source->AddBatch({{I(1), S("first")}, {I(1), S("second")}, {I(1), S("third")}});

    SortOperator sort(std::move(source), {SortKey{0, false}});
    CHECK_OK(sort.Open());
    CHECK_ROWS(DRAIN(sort), {"1,first", "1,second", "1,third"});
    CHECK_OK(sort.Close());
}

QUARRY_TEST(SortSpansMoreRowsThanOneBatch) {
    auto source = IntSource();
    const int64_t total = static_cast<int64_t>(kBatchSize) * 2 + 11;
    // Fed in ascending order across several input batches and sorted descending, so
    // the result cannot accidentally be right.
    for (int64_t base = 0; base < total; base += 700) {
        Rows rows;
        for (int64_t i = base; i < std::min(base + 700, total); ++i) rows.push_back({I(i)});
        source->AddBatch(std::move(rows));
    }

    SortOperator sort(std::move(source), {SortKey{0, true}});
    CHECK_OK(sort.Open());
    const Drained got = DRAIN(sort);
    CHECK_OK(sort.Close());

    CHECK_EQ(got.rows.size(), static_cast<size_t>(total));
    CHECK_EQ(got.batches, size_t{3});
    CHECK_EQ(got.rows.front(), std::to_string(total - 1));
    CHECK_EQ(got.rows.back(), std::string("0"));
    bool ordered = true;
    for (size_t i = 1; i < got.rows.size(); ++i) {
        if (std::stoll(got.rows[i - 1]) <= std::stoll(got.rows[i])) ordered = false;
    }
    CHECK(ordered);
}

QUARRY_TEST(SortOverEmptyInput) {
    auto source = IntSource();
    SortOperator sort(std::move(source), {SortKey{0, false}});
    CHECK_OK(sort.Open());
    CHECK_EQ(DRAIN(sort).rows.size(), size_t{0});
    CHECK_OK(sort.Close());
}

// ===========================================================================
// Limit
// ===========================================================================

QUARRY_TEST(LimitStopsPullingFromItsChild) {
    auto source = IntSource();
    // A thousand batches of ten rows. A limit that works touches one of them.
    for (int b = 0; b < 1000; ++b) {
        Rows rows;
        for (int64_t i = 0; i < 10; ++i) rows.push_back({I(b * 10 + i)});
        source->AddBatch(std::move(rows));
    }
    MockSource* raw = source.get();

    LimitOperator limit(std::move(source), 5);
    CHECK_OK(limit.Open());
    const Drained got = DRAIN(limit);
    CHECK_OK(limit.Close());

    CHECK_ROWS(got, {"0", "1", "2", "3", "4"});
    // The point of pull-based execution: the child was asked once, not a thousand
    // times, and the other 9950 rows were never produced.
    CHECK_EQ(raw->next_calls(), size_t{1});
    CHECK(raw->closed());
}

QUARRY_TEST(LimitZeroNeverAsksTheChildAtAll) {
    auto source = IntSource();
    source->AddBatch({{I(1)}});
    MockSource* raw = source.get();

    LimitOperator limit(std::move(source), 0);
    CHECK_OK(limit.Open());
    const Drained got = DRAIN(limit);
    CHECK_OK(limit.Close());
    CHECK_EQ(got.rows.size(), size_t{0});
    CHECK_EQ(raw->next_calls(), size_t{0});
}

QUARRY_TEST(LimitLargerThanTheInputReturnsEverything) {
    auto source = IntSource();
    source->AddBatch({{I(1)}, {I(2)}});
    source->AddBatch({{I(3)}});

    LimitOperator limit(std::move(source), 100);
    CHECK_OK(limit.Open());
    CHECK_ROWS(DRAIN(limit), {"1", "2", "3"});
    CHECK_OK(limit.Close());
}

QUARRY_TEST(LimitTruncatesInTheMiddleOfABatch) {
    auto source = IntSource();
    source->AddBatch({{I(1)}, {I(2)}, {I(3)}, {I(4)}});

    LimitOperator limit(std::move(source), 3);
    CHECK_OK(limit.Open());
    CHECK_ROWS(DRAIN(limit), {"1", "2", "3"});
    CHECK_OK(limit.Close());
}

QUARRY_TEST(LimitOverEmptyInput) {
    auto source = IntSource();
    LimitOperator limit(std::move(source), 10);
    CHECK_OK(limit.Open());
    CHECK_EQ(DRAIN(limit).rows.size(), size_t{0});
    CHECK_OK(limit.Close());
}

// ===========================================================================
// Hash join
// ===========================================================================

QUARRY_TEST(HashJoinProducesTheCrossProductOfDuplicateKeys) {
    auto left = std::make_unique<MockSource>(
        MakeSchema({Column("id", TypeId::kInt64), Column("l", TypeId::kVarchar)}));
    left->AddBatch({{I(1), S("l1a")}, {I(1), S("l1b")}, {I(2), S("l2")}});
    left->AddBatch({{I(1), S("l1c")}, {I(3), S("l3")}});

    auto right = std::make_unique<MockSource>(
        MakeSchema({Column("fk", TypeId::kInt64), Column("r", TypeId::kVarchar)}));
    right->AddBatch({{I(1), S("r1a")}, {I(2), S("r2")}});
    right->AddBatch({{I(1), S("r1b")}});

    HashJoinOperator join(std::move(left), std::move(right), 0, 0);
    CHECK_EQ(join.output_schema().size(), size_t{4});
    CHECK_OK(join.Open());
    const Drained got = DRAIN(join);
    CHECK_OK(join.Close());

    // Three left rows with key 1 and two right rows with key 1 give six rows; key 2
    // gives one; key 3 matches nothing because this is an inner join.
    CHECK_ROWS(got, {"1,l1a,1,r1a", "1,l1a,1,r1b", "1,l1b,1,r1a", "1,l1b,1,r1b", "2,l2,2,r2",
                     "1,l1c,1,r1a", "1,l1c,1,r1b"});
}

QUARRY_TEST(HashJoinNeverMatchesNullKeys) {
    auto left = std::make_unique<MockSource>(
        MakeSchema({Column("id", TypeId::kInt64), Column("l", TypeId::kVarchar)}));
    left->AddBatch({{N(), S("lnull")}, {I(1), S("l1")}, {N(), S("lnull2")}});

    auto right = std::make_unique<MockSource>(
        MakeSchema({Column("fk", TypeId::kInt64), Column("r", TypeId::kVarchar)}));
    right->AddBatch({{N(), S("rnull")}, {I(1), S("r1")}});

    HashJoinOperator join(std::move(left), std::move(right), 0, 0);
    CHECK_OK(join.Open());
    // NULL = NULL is unknown, so the null rows join to nothing on either side.
    CHECK_ROWS(DRAIN(join), {"1,l1,1,r1"});
    CHECK_OK(join.Close());
}

QUARRY_TEST(HashJoinOnVarcharAndOnMixedNumericKeys) {
    auto left = std::make_unique<MockSource>(MakeSchema({Column("k", TypeId::kVarchar)}));
    left->AddBatch({{S("x")}, {S("y")}, {S("z")}});
    auto right = std::make_unique<MockSource>(MakeSchema({Column("k", TypeId::kVarchar)}));
    right->AddBatch({{S("y")}, {S("x")}, {S("x")}});

    HashJoinOperator strings(std::move(left), std::move(right), 0, 0);
    CHECK_OK(strings.Open());
    CHECK_ROWS(DRAIN(strings), {"x,x", "x,x", "y,y"});
    CHECK_OK(strings.Close());

    auto ints = std::make_unique<MockSource>(MakeSchema({Column("i", TypeId::kInt64)}));
    ints->AddBatch({{I(2)}, {I(3)}});
    auto doubles = std::make_unique<MockSource>(MakeSchema({Column("d", TypeId::kDouble)}));
    doubles->AddBatch({{D(2.0)}, {D(3.5)}});

    HashJoinOperator numeric(std::move(ints), std::move(doubles), 0, 0);
    CHECK_OK(numeric.Open());
    // 2 matches 2.0 after the same promotion a comparison would do; 3 does not
    // match 3.5.
    CHECK_ROWS(DRAIN(numeric), {"2,2.000000"});
    CHECK_OK(numeric.Close());
}

QUARRY_TEST(HashJoinWithAnEmptySideProducesNothing) {
    auto left = IntSource("id");
    left->AddBatch({{I(1)}, {I(2)}});
    auto right = IntSource("fk");
    HashJoinOperator empty_build(std::move(left), std::move(right), 0, 0);
    CHECK_OK(empty_build.Open());
    CHECK_EQ(DRAIN(empty_build).rows.size(), size_t{0});
    CHECK_OK(empty_build.Close());

    auto left2 = IntSource("id");
    auto right2 = IntSource("fk");
    right2->AddBatch({{I(1)}});
    HashJoinOperator empty_probe(std::move(left2), std::move(right2), 0, 0);
    CHECK_OK(empty_probe.Open());
    CHECK_EQ(DRAIN(empty_probe).rows.size(), size_t{0});
    CHECK_OK(empty_probe.Close());
}

QUARRY_TEST(HashJoinSpillsOneLeftRowsMatchesAcrossSeveralOutputBatches) {
    auto left = IntSource("id");
    left->AddBatch({{I(7)}});

    auto right = IntSource("fk");
    // One probe row matching more rows than fit in an output batch, which forces
    // the join to suspend and resume inside a single match list.
    const size_t matches = kBatchSize + 5;
    Rows rows;
    for (size_t i = 0; i < matches; ++i) rows.push_back({I(7)});
    right->AddBatch(std::move(rows));

    HashJoinOperator join(std::move(left), std::move(right), 0, 0);
    CHECK_OK(join.Open());
    const Drained got = DRAIN(join);
    CHECK_OK(join.Close());
    CHECK_EQ(got.rows.size(), matches);
    CHECK_EQ(got.batches, size_t{2});
    CHECK_EQ(got.rows.back(), std::string("7,7"));
}

// ===========================================================================
// A whole pipeline, to check the operators compose
// ===========================================================================

QUARRY_TEST(ScanFilterAggregateSortLimitPipeline) {
    const Schema schema = MakeSchema({Column("region", TypeId::kVarchar),
                                      Column("amount", TypeId::kInt64)});
    SegmentSource segments(schema, /*rows_per_batch=*/2);
    segments.Add({{S("east"), I(10)}, {S("west"), I(5)}, {S("east"), I(20)}});
    segments.Add({{S("north"), I(1)}, {S("west"), I(50)}, {S("east"), N()}});

    Predicate pushed;
    pushed.column_index = 1;
    pushed.op = CompareOp::kGe;
    pushed.constant = I(5);

    auto scan = std::make_unique<SequentialScanOperator>(schema, segments.segments(),
                                                         std::vector<size_t>{0, 1},
                                                         std::vector<Predicate>{pushed},
                                                         segments.Factory());
    auto residual = Expression::Binary(BinaryOp::kNe, Expression::ColumnRef("region"),
                                       Expression::Literal(S("north")));
    auto filter = std::make_unique<FilterOperator>(std::move(scan), residual.get());
    auto agg = std::make_unique<HashAggregateOperator>(
        std::move(filter), std::vector<size_t>{0},
        std::vector<Aggregate>{Agg(AggregateKind::kSum, 1, "total")});
    auto sort = std::make_unique<SortOperator>(std::move(agg),
                                               std::vector<SortKey>{SortKey{1, true}});
    LimitOperator limit(std::move(sort), 1);

    CHECK_OK(limit.Open());
    CHECK_ROWS(DRAIN(limit), {"west,55"});
    CHECK_OK(limit.Close());
}

QUARRY_TEST_MAIN()
