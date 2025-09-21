#include "quarry/database.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "testing.hpp"

namespace fs = std::filesystem;

using quarry::Column;
using quarry::Database;
using quarry::ResultSet;
using quarry::Result;
using quarry::Schema;
using quarry::Status;
using quarry::TypeId;
using quarry::Value;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& label) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("quarry_db_" + label + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++));
        fs::remove_all(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.string(); }

private:
    fs::path path_;
};

// Options with a deliberately tiny segment so a few hundred rows span many
// segments. A single-segment table would make every pruning and reopen assertion
// below vacuously true.
Database::Options SmallSegments(size_t rows_per_segment = 64) {
    Database::Options options;
    options.rows_per_segment = rows_per_segment;
    options.buffer_frames = 16;
    return options;
}

// Run executes and reports the failure message at the caller's line, which is
// what makes a broken layer underneath show up as "SELECT ... failed: <reason>"
// rather than as an opaque assertion.
bool Run(Database& db, const std::string& sql, const char* file, int line,
         ResultSet* out = nullptr) {
    auto result = db.Execute(sql);
    if (!result.ok()) {
        ::quarry::testing::ReportFailure(file, line,
                                        sql + "\n          failed: " +
                                            result.status().ToString());
        return false;
    }
    if (out) *out = result.take();
    return true;
}

#define RUN(db, sql) Run((db), (sql), __FILE__, __LINE__)
#define RUN_INTO(db, sql, out) Run((db), (sql), __FILE__, __LINE__, &(out))

const char* kCreateReadings =
    "create table readings ("
    "  id int not null,"
    "  sensor text,"
    "  temp double,"
    "  ok boolean"
    ")";

// LoadReadings inserts `rows` rows in batches, so the data arrives through many
// statements the way a real load does and the segment boundaries do not line up
// with the statement boundaries.
bool LoadReadings(Database& db, int rows, const char* file, int line) {
    const char* sensors[] = {"alpha", "beta", "gamma", "delta"};
    for (int base = 0; base < rows; base += 10) {
        std::ostringstream sql;
        sql << "insert into readings values ";
        for (int i = base; i < std::min(base + 10, rows); ++i) {
            if (i != base) sql << ", ";
            sql << "(" << i << ", '" << sensors[i % 4] << "', " << (i % 50) << ".5, "
                << ((i % 3 == 0) ? "true" : "false") << ")";
        }
        if (!Run(db, sql.str(), file, line)) return false;
    }
    return true;
}

#define LOAD(db, rows) LoadReadings((db), (rows), __FILE__, __LINE__)

int64_t SingleInt(const ResultSet& rs) {
    if (rs.rows.size() != 1 || rs.rows[0].size() != 1) return -1;
    const Value& v = rs.rows[0][0];
    if (v.is_null()) return -1;
    return v.type() == TypeId::kDouble ? static_cast<int64_t>(v.as_double()) : v.as_int64();
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// ResultSet rendering. Independent of every other layer, and the format the
// differential tests compare against SQLite, so it is pinned exactly.
// ---------------------------------------------------------------------------

QUARRY_TEST(ResultSetToStringIsPipeSeparatedWithAHeader) {
    ResultSet rs;
    rs.schema.Add(Column("id", TypeId::kInt64, false));
    rs.schema.Add(Column("name", TypeId::kVarchar, true));
    rs.schema.Add(Column("ratio", TypeId::kDouble, true));
    rs.schema.Add(Column("ok", TypeId::kBoolean, true));

    rs.rows.push_back({Value::Int64(1), Value::Varchar("alpha"), Value::Double(0.5),
                       Value::Boolean(true)});
    rs.rows.push_back({Value::Int64(-2), Value::Null(), Value::Double(-1.0 / 3.0),
                       Value::Boolean(false)});
    rs.rows.push_back({Value::Int64(3), Value::Varchar(""), Value::Null(), Value::Null()});

    const std::string expected =
        "id|name|ratio|ok\n"
        "1|alpha|0.500000|true\n"
        "-2|NULL|-0.333333|false\n"
        "3||NULL|NULL\n";
    CHECK_EQ(rs.ToString(), expected);

    // NULL and the empty string must not render the same, or a query that
    // confused them would compare equal against SQLite.
    CHECK_NE(std::string("NULL"), std::string(""));
    CHECK_EQ(rs.row_count(), size_t{3});
}

QUARRY_TEST(ResultSetToStringOnAnEmptyResultIsJustTheHeader) {
    ResultSet rs;
    rs.schema.Add(Column("n", TypeId::kInt64, false));
    CHECK_EQ(rs.ToString(), std::string("n\n"));
    CHECK_EQ(rs.row_count(), size_t{0});
    CHECK_EQ(rs.SortedRowStrings().size(), size_t{0});
}

QUARRY_TEST(SortedRowStringsIsOrderIndependent) {
    ResultSet a;
    a.schema.Add(Column("x", TypeId::kInt64, false));
    a.schema.Add(Column("y", TypeId::kVarchar, true));
    a.rows.push_back({Value::Int64(2), Value::Varchar("b")});
    a.rows.push_back({Value::Int64(1), Value::Varchar("a")});
    a.rows.push_back({Value::Int64(1), Value::Varchar("a")});

    ResultSet b = a;
    std::reverse(b.rows.begin(), b.rows.end());

    CHECK(a.SortedRowStrings() == b.SortedRowStrings());
    // Duplicates are kept: two identical rows are a different result from one.
    CHECK_EQ(a.SortedRowStrings().size(), size_t{3});
    CHECK_NE(a.ToString(), b.ToString());

    const std::vector<std::string> sorted = a.SortedRowStrings();
    if (sorted.size() == 3) {
        CHECK_EQ(sorted[0], std::string("1|a"));
        CHECK_EQ(sorted[1], std::string("1|a"));
        CHECK_EQ(sorted[2], std::string("2|b"));
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

QUARRY_TEST(DatabaseOpenCreatesItsDirectory) {
    TempDir dir("open");
    CHECK(!fs::exists(dir.str()));

    Database db(dir.str(), SmallSegments());
    CHECK_OK(db.Open());
    CHECK(fs::is_directory(dir.str()));
    // A second Open is a mistake, not a no-op.
    CHECK_NOT_OK(db.Open());
    CHECK_OK(db.Close());
    // Close is idempotent, because the destructor also calls it.
    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseRejectsStatementsWhileClosed) {
    TempDir dir("closed");
    Database db(dir.str(), SmallSegments());
    auto before = db.Execute("select 1 from nothing");
    CHECK(!before.ok());
    CHECK(!db.ExplainQuery("select * from nothing").ok());
    CHECK(!db.ExplainAnalyze("select * from nothing").ok());
}

QUARRY_TEST(DatabaseReportsSyntaxAndBindErrors) {
    TempDir dir("errors");
    Database db(dir.str(), SmallSegments());
    CHECK_OK(db.Open());

    auto syntax = db.Execute("selct * from t");
    CHECK(!syntax.ok());
    if (!syntax.ok()) CHECK(syntax.status().code() == quarry::Code::kSyntaxError);

    auto unknown_table = db.Execute("select * from nope");
    CHECK(!unknown_table.ok());
    if (!unknown_table.ok()) CHECK(unknown_table.status().code() == quarry::Code::kBindError);

    CHECK(RUN(db, kCreateReadings));
    // A duplicate CREATE TABLE is rejected rather than silently redefining.
    CHECK(!db.Execute(kCreateReadings).ok());

    auto unknown_column = db.Execute("select nope from readings");
    CHECK(!unknown_column.ok());

    auto bad_insert = db.Execute("insert into readings values (1, 'a', 2.0)");
    CHECK(!bad_insert.ok());

    // A value of the wrong type is refused before any bytes are written.
    auto bad_type = db.Execute("insert into readings values ('x', 'a', 2.0, true)");
    CHECK(!bad_type.ok());

    // NOT NULL is enforced at insert time.
    auto null_id = db.Execute("insert into readings values (null, 'a', 2.0, true)");
    CHECK(!null_id.ok());

    // None of the failures above may have left rows behind.
    ResultSet count;
    CHECK(RUN_INTO(db, "select count(*) from readings", count));
    CHECK_EQ(SingleInt(count), int64_t{0});

    CHECK_OK(db.Close());
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

QUARRY_TEST(DatabaseInsertSpansSeveralSegments) {
    TempDir dir("segments");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 500));

    // The read path forces the buffered tail out, so by the time the count is
    // answered every row is in a segment.
    ResultSet count;
    CHECK(RUN_INTO(db, "select count(*) from readings", count));
    CHECK_EQ(SingleInt(count), int64_t{500});

    auto table = db.catalog().GetTable("readings");
    CHECK(table.ok());
    if (table.ok()) {
        CHECK_EQ(table.value()->row_count(), uint64_t{500});
        // 500 rows at 64 per segment is 8 segments, and a single segment would
        // make the pruning assertions elsewhere meaningless.
        CHECK(table.value()->segments.size() >= 7);
        for (const auto& seg : table.value()->segments) {
            CHECK(seg.row_count <= 64);
            CHECK(fs::exists(seg.path));
            CHECK_EQ(seg.columns.size(), size_t{4});
        }
        // Table-wide statistics come from merging those segments.
        const quarry::ColumnStats ids = table.value()->ColumnStatistics(0);
        CHECK_EQ(ids.min.as_int64(), int64_t{0});
        CHECK_EQ(ids.max.as_int64(), int64_t{499});
        CHECK_EQ(ids.null_count, uint64_t{0});
    }

    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseSelectWhereGroupByOrderByLimit) {
    TempDir dir("queries");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 500));

    ResultSet all;
    CHECK(RUN_INTO(db, "select * from readings", all));
    CHECK_EQ(all.row_count(), size_t{500});
    CHECK_EQ(all.schema.size(), size_t{4});

    ResultSet filtered;
    CHECK(RUN_INTO(db, "select id from readings where id < 10", filtered));
    CHECK_EQ(filtered.row_count(), size_t{10});

    ResultSet narrow;
    CHECK(RUN_INTO(db, "select id, sensor from readings where id >= 100 and id < 110", narrow));
    CHECK_EQ(narrow.row_count(), size_t{10});
    CHECK_EQ(narrow.schema.size(), size_t{2});

    ResultSet by_string;
    CHECK(RUN_INTO(db, "select count(*) from readings where sensor = 'alpha'", by_string));
    CHECK_EQ(SingleInt(by_string), int64_t{125});

    ResultSet grouped;
    CHECK(RUN_INTO(db, "select sensor, count(*) from readings group by sensor", grouped));
    CHECK_EQ(grouped.row_count(), size_t{4});
    for (const auto& line : grouped.SortedRowStrings()) {
        CHECK(Contains(line, "|125"));
    }

    ResultSet sums;
    CHECK(RUN_INTO(db, "select sensor, sum(id), min(id), max(id) from readings group by sensor",
                   sums));
    CHECK_EQ(sums.row_count(), size_t{4});
    CHECK_EQ(sums.schema.size(), size_t{4});

    ResultSet ordered;
    CHECK(RUN_INTO(db, "select id from readings order by id desc limit 5", ordered));
    CHECK_EQ(ordered.row_count(), size_t{5});
    if (ordered.row_count() == 5) {
        CHECK_EQ(ordered.rows[0][0].as_int64(), int64_t{499});
        CHECK_EQ(ordered.rows[4][0].as_int64(), int64_t{495});
    }

    ResultSet ascending;
    CHECK(RUN_INTO(db, "select id from readings where id > 400 order by id limit 3", ascending));
    CHECK_EQ(ascending.row_count(), size_t{3});
    if (ascending.row_count() == 3) {
        CHECK_EQ(ascending.rows[0][0].as_int64(), int64_t{401});
        CHECK_EQ(ascending.rows[2][0].as_int64(), int64_t{403});
    }

    // LIMIT with no ORDER BY has no defined row order, so only the count is
    // asserted. Asserting which rows came back would be asserting an accident.
    ResultSet limited;
    CHECK(RUN_INTO(db, "select id from readings limit 7", limited));
    CHECK_EQ(limited.row_count(), size_t{7});

    // A predicate no segment can satisfy returns nothing, not an error.
    ResultSet none;
    CHECK(RUN_INTO(db, "select id from readings where id > 100000", none));
    CHECK_EQ(none.row_count(), size_t{0});
    CHECK_EQ(none.ToString(), std::string("id\n"));

    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseAggregatesOverTheWholeTable) {
    TempDir dir("aggregates");
    Database db(dir.str(), SmallSegments(32));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 100));

    ResultSet rs;
    CHECK(RUN_INTO(db, "select count(*), sum(id), min(id), max(id) from readings", rs));
    CHECK_EQ(rs.row_count(), size_t{1});
    if (rs.row_count() == 1 && rs.rows[0].size() == 4) {
        CHECK_EQ(rs.rows[0][0].as_int64(), int64_t{100});
        CHECK_EQ(rs.rows[0][1].as_int64(), int64_t{4950});
        CHECK_EQ(rs.rows[0][2].as_int64(), int64_t{0});
        CHECK_EQ(rs.rows[0][3].as_int64(), int64_t{99});
    }

    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseHandlesNullsEndToEnd) {
    TempDir dir("nulls");
    Database db(dir.str(), SmallSegments(8));
    CHECK_OK(db.Open());
    CHECK(RUN(db, "create table t (id int not null, label text, amount double)"));

    CHECK(RUN(db, "insert into t values (1, 'a', 1.5), (2, null, null), (3, 'c', 3.5)"));
    // An explicit column list leaves the rest NULL.
    CHECK(RUN(db, "insert into t (id) values (4)"));

    ResultSet rs;
    CHECK(RUN_INTO(db, "select id, label, amount from t order by id", rs));
    CHECK_EQ(rs.row_count(), size_t{4});
    if (rs.row_count() == 4) {
        CHECK(rs.rows[1][1].is_null());
        CHECK(rs.rows[1][2].is_null());
        CHECK(rs.rows[3][1].is_null());
        CHECK_EQ(rs.rows[3][0].as_int64(), int64_t{4});
        CHECK(Contains(rs.ToString(), "2|NULL|NULL"));
    }

    // A column list that omits a NOT NULL column is rejected.
    CHECK(!db.Execute("insert into t (label) values ('x')").ok());

    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseSurvivesReopen) {
    TempDir dir("reopen");

    {
        Database db(dir.str(), SmallSegments(64));
        CHECK_OK(db.Open());
        CHECK(RUN(db, kCreateReadings));
        CHECK(LOAD(db, 500));
        // 500 is not a multiple of the 64-row segment size, so the tail of the
        // data is still in the insert buffer and only Close can save it. This is
        // the case a missing final flush loses.
        CHECK_OK(db.Close());
    }

    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());

    const std::vector<std::string> tables = db.catalog().TableNames();
    CHECK_EQ(tables.size(), size_t{1});
    if (!tables.empty()) CHECK_EQ(tables[0], std::string("readings"));

    auto table = db.catalog().GetTable("readings");
    CHECK(table.ok());
    if (table.ok()) {
        const Schema& schema = table.value()->schema;
        CHECK_EQ(schema.size(), size_t{4});
        CHECK_EQ(schema.at(0).name, std::string("id"));
        CHECK(schema.at(0).type == TypeId::kInt64);
        CHECK_EQ(schema.at(0).nullable, false);
        CHECK_EQ(schema.at(1).name, std::string("sensor"));
        CHECK(schema.at(1).type == TypeId::kVarchar);
        CHECK(schema.at(2).type == TypeId::kDouble);
        CHECK(schema.at(3).type == TypeId::kBoolean);
        CHECK_EQ(table.value()->row_count(), uint64_t{500});
    }

    ResultSet count;
    CHECK(RUN_INTO(db, "select count(*) from readings", count));
    CHECK_EQ(SingleInt(count), int64_t{500});

    ResultSet tail;
    CHECK(RUN_INTO(db, "select id, sensor, temp from readings where id > 495 order by id", tail));
    CHECK_EQ(tail.row_count(), size_t{4});
    if (tail.row_count() == 4) {
        CHECK_EQ(tail.rows[0][0].as_int64(), int64_t{496});
        CHECK_EQ(tail.rows[3][0].as_int64(), int64_t{499});
        CHECK_EQ(tail.rows[0][1].as_varchar(), std::string("alpha"));
        CHECK(tail.rows[0][2].as_double() == 46.5);
    }

    // Appending after a reopen must land in new segments alongside the old ones.
    CHECK(RUN(db, "insert into readings values (1000, 'omega', 0.5, true)"));
    ResultSet after;
    CHECK(RUN_INTO(db, "select count(*) from readings", after));
    CHECK_EQ(SingleInt(after), int64_t{501});
    CHECK_OK(db.Close());

    Database third(dir.str(), SmallSegments(64));
    CHECK_OK(third.Open());
    ResultSet final_count;
    CHECK(RUN_INTO(third, "select count(*) from readings", final_count));
    CHECK_EQ(SingleInt(final_count), int64_t{501});
    CHECK_OK(third.Close());
}

QUARRY_TEST(DatabaseWithACorruptCatalogRefusesToOpen) {
    TempDir dir("corrupt");
    {
        Database db(dir.str(), SmallSegments(16));
        CHECK_OK(db.Open());
        CHECK(RUN(db, kCreateReadings));
        CHECK(LOAD(db, 50));
        CHECK_OK(db.Close());
    }

    const fs::path catalog = fs::path(dir.str()) / "catalog.qcat";
    CHECK(fs::exists(catalog));
    {
        // Flip a byte deep in the body rather than truncating, so the failure is
        // the checksum and not the length.
        std::fstream file(catalog, std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(size - 4);
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x55);
        file.seekp(size - 4);
        file.write(&byte, 1);
    }

    Database db(dir.str(), SmallSegments(16));
    const Status status = db.Open();
    CHECK(!status.ok());
    if (!status.ok()) CHECK(status.code() == quarry::Code::kCorruption);
    // The segment files are all still there, which is the reason refusing to open
    // is the right behaviour: starting empty would have orphaned them.
    int segments = 0;
    for (const auto& entry : fs::directory_iterator(dir.str())) {
        if (entry.path().extension() == ".qseg") ++segments;
    }
    CHECK(segments > 0);
}

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------

QUARRY_TEST(DatabaseCreateBTreeIndexAndQueryThroughIt) {
    TempDir dir("btree");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 500));

    CHECK(RUN(db, "create index readings_by_id on readings (id)"));

    auto table = db.catalog().GetTable("readings");
    CHECK(table.ok());
    if (table.ok()) {
        CHECK_EQ(table.value()->indexes.size(), size_t{1});
        if (!table.value()->indexes.empty()) {
            const quarry::IndexDef& idx = table.value()->indexes[0];
            CHECK_EQ(idx.name, std::string("readings_by_id"));
            CHECK_EQ(idx.column_index, size_t{0});
            CHECK(idx.kind == quarry::IndexKind::kBTree);
            CHECK(fs::exists(idx.path));
        }
    }

    // A duplicate index name is rejected.
    CHECK(!db.Execute("create index readings_by_id on readings (id)").ok());
    // So is an index on a column that does not exist.
    CHECK(!db.Execute("create index bad on readings (nope)").ok());

    // The answer must not depend on whether the optimizer chose the index, which
    // is the whole point of having both access paths.
    ResultSet point;
    CHECK(RUN_INTO(db, "select id, sensor from readings where id = 317", point));
    CHECK_EQ(point.row_count(), size_t{1});
    if (point.row_count() == 1) {
        CHECK_EQ(point.rows[0][0].as_int64(), int64_t{317});
        CHECK_EQ(point.rows[0][1].as_varchar(), std::string("beta"));
    }

    ResultSet range;
    CHECK(RUN_INTO(db, "select id from readings where id >= 200 and id < 210 order by id", range));
    CHECK_EQ(range.row_count(), size_t{10});

    auto plan = db.ExplainQuery("select id from readings where id = 317");
    CHECK(plan.ok());
    if (plan.ok()) {
        // The optimizer is free to decline the index, so the assertion is on the
        // predicate reaching the scan rather than on the choice it made.
        CHECK(Contains(plan.value(), "Scan"));
    }

    // The index must still be in the catalog after a reopen.
    CHECK_OK(db.Close());
    Database reopened(dir.str(), SmallSegments(64));
    CHECK_OK(reopened.Open());
    auto again = reopened.catalog().GetTable("readings");
    CHECK(again.ok());
    if (again.ok()) CHECK_EQ(again.value()->indexes.size(), size_t{1});
    ResultSet still;
    CHECK(RUN_INTO(reopened, "select id from readings where id = 317", still));
    CHECK_EQ(still.row_count(), size_t{1});
    CHECK_OK(reopened.Close());
}

QUARRY_TEST(DatabaseCreateLsmIndexViaUsingLsm) {
    TempDir dir("lsm");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 500));

    CHECK(RUN(db, "create index readings_by_sensor on readings (sensor) using lsm"));

    auto table = db.catalog().GetTable("readings");
    CHECK(table.ok());
    if (table.ok() && !table.value()->indexes.empty()) {
        const quarry::IndexDef& idx = table.value()->indexes[0];
        CHECK(idx.kind == quarry::IndexKind::kLsm);
        CHECK_EQ(idx.column_index, size_t{1});
        CHECK(fs::exists(idx.path));
    }

    ResultSet rs;
    CHECK(RUN_INTO(db, "select count(*) from readings where sensor = 'gamma'", rs));
    CHECK_EQ(SingleInt(rs), int64_t{125});

    // Both kinds can coexist on one table, which is what makes them comparable.
    CHECK(RUN(db, "create index readings_by_id on readings (id)"));
    if (table.ok()) CHECK_EQ(table.value()->indexes.size(), size_t{2});

    CHECK_OK(db.Close());
}

QUARRY_TEST(DatabaseIndexBuiltBeforeTheDataStillSeesIt) {
    // The index is created on an empty table and rows are inserted afterwards. The
    // query must still be right, whether that is because the optimizer declined a
    // stale index or because the scan maintained it.
    TempDir dir("index_first");
    Database db(dir.str(), SmallSegments(32));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(RUN(db, "create index readings_by_id on readings (id)"));
    CHECK(LOAD(db, 200));

    ResultSet rs;
    CHECK(RUN_INTO(db, "select count(*) from readings where id = 150", rs));
    CHECK_EQ(SingleInt(rs), int64_t{1});
    ResultSet total;
    CHECK(RUN_INTO(db, "select count(*) from readings", total));
    CHECK_EQ(SingleInt(total), int64_t{200});
    CHECK_OK(db.Close());
}

// ---------------------------------------------------------------------------
// EXPLAIN
// ---------------------------------------------------------------------------

QUARRY_TEST(ExplainNamesTheOperatorsInThePlan) {
    TempDir dir("explain");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 300));

    auto plan = db.ExplainQuery(
        "select sensor, count(*) from readings where id > 10 group by sensor "
        "order by sensor limit 2");
    CHECK(plan.ok());
    if (plan.ok()) {
        const std::string& text = plan.value();
        CHECK(Contains(text, "Limit"));
        CHECK(Contains(text, "Sort"));
        CHECK(Contains(text, "Aggregate"));
        CHECK(Contains(text, "Scan"));
        CHECK(Contains(text, "readings"));
    }

    // EXPLAIN accepts both spellings, and the statement form routes to the same
    // text.
    auto wrapped = db.ExplainQuery("explain select id from readings where id > 10");
    CHECK(wrapped.ok());
    auto bare = db.ExplainQuery("select id from readings where id > 10");
    CHECK(bare.ok());
    if (wrapped.ok() && bare.ok()) CHECK_EQ(wrapped.value(), bare.value());

    ResultSet as_rows;
    CHECK(RUN_INTO(db, "explain select id from readings where id > 10", as_rows));
    CHECK(as_rows.row_count() > 0);
    CHECK_EQ(as_rows.schema.size(), size_t{1});
    CHECK(Contains(as_rows.ToString(), "Scan"));

    CHECK_OK(db.Close());
}

QUARRY_TEST(ExplainAnalyzeReportsMeasuredCounters) {
    TempDir dir("analyze");
    Database db(dir.str(), SmallSegments(64));
    CHECK_OK(db.Open());
    CHECK(RUN(db, kCreateReadings));
    CHECK(LOAD(db, 300));

    auto analyzed = db.ExplainAnalyze("select id from readings where id > 250");
    CHECK(analyzed.ok());
    if (analyzed.ok()) {
        CHECK(Contains(analyzed.value(), "Scan"));
        // ANALYZE has to differ from a plain EXPLAIN, or it did not measure
        // anything.
        auto plain = db.ExplainQuery("select id from readings where id > 250");
        CHECK(plain.ok());
        if (plain.ok()) CHECK_NE(analyzed.value(), plain.value());
    }

    ResultSet as_rows;
    CHECK(RUN_INTO(db, "explain analyze select id from readings where id > 250", as_rows));
    CHECK(as_rows.row_count() > 0);

    auto not_a_select = db.ExplainQuery("insert into readings values (1,'a',1.0,true)");
    CHECK(!not_a_select.ok());

    CHECK_OK(db.Close());
}

// ---------------------------------------------------------------------------
// Segment granularity
// ---------------------------------------------------------------------------

QUARRY_TEST(RowsPerSegmentIsRespected) {
    for (const size_t rows_per_segment : {1u, 7u, 1000u}) {
        TempDir sub("granularity_" + std::to_string(rows_per_segment));
        Database db(sub.str(), SmallSegments(rows_per_segment));
        CHECK_OK(db.Open());
        CHECK(RUN(db, kCreateReadings));
        CHECK(LOAD(db, 40));
        CHECK_OK(db.Close());

        Database reopened(sub.str(), SmallSegments(rows_per_segment));
        CHECK_OK(reopened.Open());
        auto table = reopened.catalog().GetTable("readings");
        CHECK(table.ok());
        if (table.ok()) {
            CHECK_EQ(table.value()->row_count(), uint64_t{40});
            for (const auto& seg : table.value()->segments) {
                CHECK(seg.row_count <= rows_per_segment);
                CHECK(seg.row_count > 0);
            }
            const size_t expected_min = (40 + rows_per_segment - 1) / rows_per_segment;
            CHECK(table.value()->segments.size() >= expected_min);
        }
        ResultSet count;
        CHECK(RUN_INTO(reopened, "select count(*) from readings", count));
        CHECK_EQ(SingleInt(count), int64_t{40});
        CHECK_OK(reopened.Close());
    }
}

QUARRY_TEST(SegmentIdsAreNeverReused) {
    TempDir dir("ids");
    Database db(dir.str(), SmallSegments(10));
    CHECK_OK(db.Open());
    CHECK(RUN(db, "create table a (x int not null)"));
    CHECK(RUN(db, "create table b (x int not null)"));
    CHECK(RUN(db, "insert into a values (1),(2),(3),(4),(5),(6),(7),(8),(9),(10),(11)"));
    CHECK(RUN(db, "insert into b values (1),(2),(3),(4),(5),(6),(7),(8),(9),(10),(11)"));
    ResultSet dummy;
    CHECK(RUN_INTO(db, "select count(*) from a", dummy));

    std::vector<uint64_t> ids;
    for (const auto& name : db.catalog().TableNames()) {
        auto table = db.catalog().GetTable(name);
        if (!table.ok()) continue;
        for (const auto& seg : table.value()->segments) ids.push_back(seg.id);
    }
    const size_t before = ids.size();
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    // Ids are unique across tables, because the segment file name is derived from
    // the id and a collision would have one table's segment overwrite another's.
    CHECK_EQ(ids.size(), before);
    CHECK(before >= 4);

    CHECK_OK(db.Close());
}

QUARRY_TEST_MAIN()
