#include "quarry/catalog.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "testing.hpp"

namespace fs = std::filesystem;

using quarry::Catalog;
using quarry::Column;
using quarry::ColumnChunk;
using quarry::ColumnStats;
using quarry::Encoding;
using quarry::IndexDef;
using quarry::IndexKind;
using quarry::Schema;
using quarry::SegmentMeta;
using quarry::TypeId;
using quarry::Value;

namespace {

// TempDir removes itself, so a failing assertion in the middle of a test does not
// leave a directory behind that the next run would then load as an existing
// database and fail differently.
class TempDir {
public:
    explicit TempDir(const std::string& label) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("quarry_catalog_" + label + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.string(); }
    fs::path path() const { return path_; }

private:
    fs::path path_;
};

Schema PeopleSchema() {
    Schema s;
    s.Add(Column("id", TypeId::kInt64, false));
    s.Add(Column("name", TypeId::kVarchar, true));
    s.Add(Column("score", TypeId::kDouble, true));
    s.Add(Column("active", TypeId::kBoolean, false));
    return s;
}

ColumnStats Int64Stats(int64_t lo, int64_t hi, uint64_t nulls, uint64_t distinct) {
    ColumnStats s;
    s.min = Value::Int64(lo);
    s.max = Value::Int64(hi);
    s.null_count = nulls;
    s.distinct_estimate = distinct;
    return s;
}

// A segment whose chunks carry distinguishable values in every field, so a
// round-trip that drops or transposes one is visible rather than coincidentally
// equal.
SegmentMeta MakeSegment(uint64_t id, uint64_t rows, int64_t id_lo, int64_t id_hi,
                        const std::string& name_lo, const std::string& name_hi, double score_lo,
                        double score_hi) {
    SegmentMeta meta;
    meta.id = id;
    meta.row_count = rows;
    meta.path = "/tmp/people_seg" + std::to_string(id) + ".qseg";

    ColumnChunk id_chunk;
    id_chunk.type = TypeId::kInt64;
    id_chunk.encoding = Encoding::kRunLength;
    id_chunk.offset = 32;
    id_chunk.length = 1024 + id;
    id_chunk.raw_length = rows * 8;
    id_chunk.stats = Int64Stats(id_lo, id_hi, 0, rows);
    meta.columns.push_back(id_chunk);

    ColumnChunk name_chunk;
    name_chunk.type = TypeId::kVarchar;
    name_chunk.encoding = Encoding::kDictionary;
    name_chunk.offset = id_chunk.offset + id_chunk.length;
    name_chunk.length = 2048;
    name_chunk.raw_length = rows * 12;
    name_chunk.stats.min = Value::Varchar(name_lo);
    name_chunk.stats.max = Value::Varchar(name_hi);
    name_chunk.stats.null_count = 3;
    name_chunk.stats.distinct_estimate = 7;
    meta.columns.push_back(name_chunk);

    ColumnChunk score_chunk;
    score_chunk.type = TypeId::kDouble;
    score_chunk.encoding = Encoding::kPlain;
    score_chunk.offset = name_chunk.offset + name_chunk.length;
    score_chunk.length = rows * 8 + 4;
    score_chunk.raw_length = rows * 8;
    score_chunk.stats.min = Value::Double(score_lo);
    score_chunk.stats.max = Value::Double(score_hi);
    score_chunk.stats.null_count = 1;
    score_chunk.stats.distinct_estimate = rows / 2;
    meta.columns.push_back(score_chunk);

    // An all-null chunk exercises the one case where min and max must not be
    // trusted, and the flag that says so has to survive the round-trip.
    ColumnChunk active_chunk;
    active_chunk.type = TypeId::kBoolean;
    active_chunk.encoding = Encoding::kRunLength;
    active_chunk.offset = score_chunk.offset + score_chunk.length;
    active_chunk.length = 16;
    active_chunk.raw_length = rows;
    active_chunk.stats.all_null = true;
    active_chunk.stats.null_count = rows;
    meta.columns.push_back(active_chunk);

    return meta;
}

bool SameValue(const Value& a, const Value& b) {
    if (a.is_null() != b.is_null()) return false;
    if (a.is_null()) return true;
    if (a.type() != b.type()) return false;
    return a.ToString() == b.ToString();
}

bool SameStats(const ColumnStats& a, const ColumnStats& b) {
    return a.all_null == b.all_null && a.null_count == b.null_count &&
           a.distinct_estimate == b.distinct_estimate && SameValue(a.min, b.min) &&
           SameValue(a.max, b.max);
}

bool SameChunk(const ColumnChunk& a, const ColumnChunk& b) {
    return a.type == b.type && a.encoding == b.encoding && a.offset == b.offset &&
           a.length == b.length && a.raw_length == b.raw_length && SameStats(a.stats, b.stats);
}

std::string CatalogFile(const TempDir& dir) { return (dir.path() / "catalog.qcat").string(); }

}  // namespace

QUARRY_TEST(CatalogRoundTripsEverything) {
    TempDir dir("roundtrip");

    SegmentMeta seg_a = MakeSegment(1, 100, -5, 400, "alice", "mallory", -1.5, 99.25);
    SegmentMeta seg_b = MakeSegment(2, 250, 401, 900, "nadia", "zoe", 100.0, 1000.125);

    {
        Catalog catalog(dir.str());
        CHECK_OK(catalog.Open());
        CHECK_OK(catalog.CreateTable("people", PeopleSchema()));

        Schema events;
        events.Add(Column("ts", TypeId::kInt64, false));
        events.Add(Column("kind", TypeId::kVarchar, true));
        CHECK_OK(catalog.CreateTable("events", events));

        CHECK_OK(catalog.AddSegment("people", seg_a));
        CHECK_OK(catalog.AddSegment("people", seg_b));

        IndexDef by_id;
        by_id.name = "people_by_id";
        by_id.column_index = 0;
        by_id.kind = IndexKind::kBTree;
        by_id.path = catalog.IndexPath("people", "people_by_id");
        CHECK_OK(catalog.AddIndex("people", by_id));

        IndexDef by_name;
        by_name.name = "people_by_name";
        by_name.column_index = 1;
        by_name.kind = IndexKind::kLsm;
        by_name.path = catalog.IndexPath("people", "people_by_name");
        CHECK_OK(catalog.AddIndex("people", by_name));

        CHECK_OK(catalog.Save());
    }

    Catalog reopened(dir.str());
    CHECK_OK(reopened.Open());

    const std::vector<std::string> names = reopened.TableNames();
    CHECK_EQ(names.size(), size_t{2});
    if (names.size() == 2) {
        CHECK_EQ(names[0], std::string("people"));
        CHECK_EQ(names[1], std::string("events"));
    }

    auto found = reopened.GetTable("people");
    CHECK(found.ok());
    if (!found.ok()) return;
    const quarry::TableDef& people = *found.value();

    CHECK(people.schema == PeopleSchema());
    CHECK_EQ(people.schema.size(), size_t{4});
    // Schema::operator== ignores nullability, so it is checked by hand: a column
    // that came back nullable when it was declared NOT NULL would otherwise pass.
    CHECK_EQ(people.schema.at(0).nullable, false);
    CHECK_EQ(people.schema.at(1).nullable, true);
    CHECK_EQ(people.schema.at(3).nullable, false);

    CHECK_EQ(people.row_count(), uint64_t{350});
    CHECK_EQ(people.segments.size(), size_t{2});
    if (people.segments.size() == 2) {
        const SegmentMeta* expected[2] = {&seg_a, &seg_b};
        for (size_t s = 0; s < 2; ++s) {
            const SegmentMeta& got = people.segments[s];
            const SegmentMeta& want = *expected[s];
            CHECK_EQ(got.id, want.id);
            CHECK_EQ(got.row_count, want.row_count);
            CHECK_EQ(got.path, want.path);
            CHECK_EQ(got.columns.size(), want.columns.size());
            for (size_t c = 0; c < got.columns.size() && c < want.columns.size(); ++c) {
                CHECK(SameChunk(got.columns[c], want.columns[c]));
            }
        }
    }

    CHECK_EQ(people.indexes.size(), size_t{2});
    if (people.indexes.size() == 2) {
        CHECK_EQ(people.indexes[0].name, std::string("people_by_id"));
        CHECK_EQ(people.indexes[0].column_index, size_t{0});
        CHECK(people.indexes[0].kind == IndexKind::kBTree);
        CHECK_EQ(people.indexes[0].path, reopened.IndexPath("people", "people_by_id"));

        CHECK_EQ(people.indexes[1].name, std::string("people_by_name"));
        CHECK_EQ(people.indexes[1].column_index, size_t{1});
        CHECK(people.indexes[1].kind == IndexKind::kLsm);
    }

    // The next id must be past everything already written, or a new segment would
    // be handed an id whose file already exists.
    SegmentMeta fresh = MakeSegment(3, 10, 901, 910, "a", "b", 0.0, 1.0);
    CHECK_OK(reopened.AddSegment("people", fresh));
    CHECK_OK(reopened.Save());
    Catalog third(dir.str());
    CHECK_OK(third.Open());
    auto third_people = third.GetTable("people");
    CHECK(third_people.ok());
    if (third_people.ok()) CHECK_EQ(third_people.value()->segments.size(), size_t{3});
}

QUARRY_TEST(CatalogDoublePrecisionSurvivesExactly) {
    TempDir dir("doubles");

    // A value that no fixed-precision decimal rendering reproduces. If the bound
    // were serialized as text, the reloaded max would be below the real maximum
    // and a zone map would prune a segment that does contain matching rows.
    const double awkward = 0.1 + 0.2;

    {
        Catalog catalog(dir.str());
        CHECK_OK(catalog.Open());
        Schema s;
        s.Add(Column("x", TypeId::kDouble, true));
        CHECK_OK(catalog.CreateTable("t", s));

        SegmentMeta meta;
        meta.id = 1;
        meta.row_count = 2;
        meta.path = "t_seg1.qseg";
        ColumnChunk chunk;
        chunk.type = TypeId::kDouble;
        chunk.encoding = Encoding::kPlain;
        chunk.stats.min = Value::Double(-awkward);
        chunk.stats.max = Value::Double(awkward);
        meta.columns.push_back(chunk);
        CHECK_OK(catalog.AddSegment("t", meta));
        CHECK_OK(catalog.Save());
    }

    Catalog reopened(dir.str());
    CHECK_OK(reopened.Open());
    auto t = reopened.GetTable("t");
    CHECK(t.ok());
    if (!t.ok()) return;
    const ColumnStats& stats = t.value()->segments.at(0).columns.at(0).stats;
    CHECK(stats.max.as_double() == awkward);
    CHECK(stats.min.as_double() == -awkward);
}

QUARRY_TEST(CatalogMissingFileStartsEmpty) {
    TempDir dir("missing");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    CHECK_EQ(catalog.TableNames().size(), size_t{0});
}

QUARRY_TEST(CatalogOpenCreatesItsDirectory) {
    TempDir dir("mkdir");
    const std::string nested = (dir.path() / "a" / "b").string();
    Catalog catalog(nested);
    CHECK_OK(catalog.Open());
    CHECK_OK(catalog.CreateTable("t", PeopleSchema()));
    CHECK_OK(catalog.Save());
    CHECK(fs::exists(fs::path(nested) / "catalog.qcat"));
}

QUARRY_TEST(CatalogCorruptFileIsAnErrorNotAFreshStart) {
    // Each case damages a saved catalog differently. Every one must come back as a
    // failure: a catalog that quietly reported zero tables would leave every
    // segment file on disk unreferenced and the data silently gone.
    struct Case {
        const char* label;
        std::function<void(const std::string& path, std::vector<char>& bytes)> damage;
    };

    const std::vector<Case> cases = {
        {"flipped-body-byte",
         [](const std::string&, std::vector<char>& bytes) {
             bytes[bytes.size() - 5] = static_cast<char>(bytes[bytes.size() - 5] ^ 0x40);
         }},
        {"bad-magic",
         [](const std::string&, std::vector<char>& bytes) { bytes[2] = 'X'; }},
        {"wrong-version",
         [](const std::string&, std::vector<char>& bytes) { bytes[8] = 99; }},
        {"truncated-body",
         [](const std::string&, std::vector<char>& bytes) { bytes.resize(bytes.size() - 7); }},
        {"truncated-to-nothing",
         [](const std::string&, std::vector<char>& bytes) { bytes.resize(3); }},
        {"appended-garbage",
         [](const std::string&, std::vector<char>& bytes) {
             for (int i = 0; i < 9; ++i) bytes.push_back(static_cast<char>(0xAB));
         }},
    };

    for (const auto& c : cases) {
        TempDir dir(std::string("corrupt_") + c.label);
        {
            Catalog catalog(dir.str());
            CHECK_OK(catalog.Open());
            CHECK_OK(catalog.CreateTable("people", PeopleSchema()));
            CHECK_OK(catalog.AddSegment("people", MakeSegment(1, 64, 0, 63, "a", "z", 0.0, 1.0)));
            CHECK_OK(catalog.Save());
        }

        const std::string path = CatalogFile(dir);
        std::vector<char> bytes;
        {
            std::ifstream in(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        CHECK(bytes.size() > 32);
        c.damage(path, bytes);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        Catalog reopened(dir.str());
        const quarry::Status status = reopened.Open();
        if (status.ok()) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__,
                                             std::string("damaged catalog (") + c.label +
                                                 ") opened successfully");
        } else {
            CHECK(status.code() == quarry::Code::kCorruption);
        }
        // And it must not have left a half-loaded catalog behind either.
        CHECK_EQ(reopened.TableNames().size(), size_t{0});
    }
}

QUARRY_TEST(CatalogRejectsDuplicateAndMalformedCreateTable) {
    TempDir dir("create");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());

    CHECK_OK(catalog.CreateTable("people", PeopleSchema()));
    CHECK_NOT_OK(catalog.CreateTable("people", PeopleSchema()));
    // Rejected even with a different shape: the name is what collides.
    Schema other;
    other.Add(Column("x", TypeId::kInt64, true));
    CHECK_NOT_OK(catalog.CreateTable("people", other));

    CHECK_NOT_OK(catalog.CreateTable("", other));
    CHECK_NOT_OK(catalog.CreateTable("empty", Schema()));

    Schema dup;
    dup.Add(Column("x", TypeId::kInt64, true));
    dup.Add(Column("x", TypeId::kVarchar, true));
    CHECK_NOT_OK(catalog.CreateTable("dup", dup));

    Schema untyped;
    untyped.Add(Column("x", TypeId::kInvalid, true));
    CHECK_NOT_OK(catalog.CreateTable("untyped", untyped));

    CHECK_EQ(catalog.TableNames().size(), size_t{1});
}

QUARRY_TEST(CatalogDropTable) {
    TempDir dir("drop");
    {
        Catalog catalog(dir.str());
        CHECK_OK(catalog.Open());
        CHECK_OK(catalog.CreateTable("people", PeopleSchema()));
        CHECK_OK(catalog.CreateTable("events", PeopleSchema()));
        CHECK_OK(catalog.AddSegment("people", MakeSegment(1, 32, 0, 31, "a", "z", 0.0, 1.0)));
        CHECK_OK(catalog.Save());

        CHECK_OK(catalog.DropTable("people"));
        CHECK(!catalog.HasTable("people"));
        CHECK(catalog.HasTable("events"));
        CHECK_NOT_OK(catalog.DropTable("people"));
        CHECK_NOT_OK(catalog.DropTable("never_existed"));
        CHECK_OK(catalog.Save());
    }

    Catalog reopened(dir.str());
    CHECK_OK(reopened.Open());
    CHECK_EQ(reopened.TableNames().size(), size_t{1});
    CHECK(!reopened.HasTable("people"));

    // The id of the dropped table's segment is still consumed. Reusing it would
    // point a new segment at a file the drop did not delete.
    SegmentMeta fresh = MakeSegment(0, 8, 0, 7, "a", "b", 0.0, 1.0);
    fresh.id = 1;
    CHECK_NOT_OK(reopened.AddSegment("people", fresh));
    CHECK_OK(reopened.CreateTable("people", PeopleSchema()));
}

QUARRY_TEST(CatalogUnknownLookupsAreBindErrors) {
    TempDir dir("unknown");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    CHECK_OK(catalog.CreateTable("people", PeopleSchema()));

    auto missing = catalog.GetTable("nope");
    CHECK(!missing.ok());
    if (!missing.ok()) {
        CHECK(missing.status().code() == quarry::Code::kBindError);
        CHECK(missing.status().message().find("nope") != std::string::npos);
    }

    auto missing_mut = catalog.GetTableMutable("nope");
    CHECK(!missing_mut.ok());
    if (!missing_mut.ok()) CHECK(missing_mut.status().code() == quarry::Code::kBindError);

    CHECK(!catalog.HasTable("nope"));

    const quarry::Status add_seg =
        catalog.AddSegment("nope", MakeSegment(1, 1, 0, 0, "a", "a", 0.0, 0.0));
    CHECK(add_seg.code() == quarry::Code::kBindError);

    IndexDef idx;
    idx.name = "i";
    idx.column_index = 0;
    CHECK(catalog.AddIndex("nope", idx).code() == quarry::Code::kBindError);

    // An index on a column that does not exist is the same class of mistake.
    idx.column_index = 99;
    CHECK(catalog.AddIndex("people", idx).code() == quarry::Code::kBindError);
}

QUARRY_TEST(CatalogRejectsSegmentsThatDoNotMatchTheSchema) {
    TempDir dir("skew");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    CHECK_OK(catalog.CreateTable("people", PeopleSchema()));

    SegmentMeta short_seg = MakeSegment(1, 10, 0, 9, "a", "z", 0.0, 1.0);
    short_seg.columns.pop_back();
    CHECK_NOT_OK(catalog.AddSegment("people", short_seg));

    SegmentMeta wrong_type = MakeSegment(1, 10, 0, 9, "a", "z", 0.0, 1.0);
    wrong_type.columns[0].type = TypeId::kVarchar;
    CHECK(catalog.AddSegment("people", wrong_type).code() == quarry::Code::kTypeError);

    CHECK_OK(catalog.AddSegment("people", MakeSegment(1, 10, 0, 9, "a", "z", 0.0, 1.0)));
    CHECK_NOT_OK(catalog.AddSegment("people", MakeSegment(1, 10, 0, 9, "a", "z", 0.0, 1.0)));

    IndexDef idx;
    idx.name = "by_id";
    idx.column_index = 0;
    CHECK_OK(catalog.AddIndex("people", idx));
    CHECK_NOT_OK(catalog.AddIndex("people", idx));
}

QUARRY_TEST(ColumnStatisticsMergesSegmentStats) {
    TempDir dir("merge");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    CHECK_OK(catalog.CreateTable("people", PeopleSchema()));

    // Two hand-built segments with known, deliberately overlapping-in-one-column
    // and disjoint-in-another statistics.
    SegmentMeta a = MakeSegment(1, 100, -5, 400, "alice", "mallory", -1.5, 99.25);
    SegmentMeta b = MakeSegment(2, 250, 401, 900, "nadia", "zoe", 100.0, 1000.125);
    CHECK_OK(catalog.AddSegment("people", a));
    CHECK_OK(catalog.AddSegment("people", b));

    auto found = catalog.GetTable("people");
    CHECK(found.ok());
    if (!found.ok()) return;
    const quarry::TableDef& people = *found.value();
    CHECK_EQ(people.row_count(), uint64_t{350});

    const ColumnStats ids = people.ColumnStatistics(0);
    CHECK_EQ(ids.min.as_int64(), int64_t{-5});
    CHECK_EQ(ids.max.as_int64(), int64_t{900});
    CHECK_EQ(ids.null_count, uint64_t{0});
    CHECK_EQ(ids.all_null, false);
    // Per-segment estimates are 100 and 250 and the row count is 350, so the sum
    // is exactly representable and the clamp does not bite.
    CHECK_EQ(ids.distinct_estimate, uint64_t{350});

    const ColumnStats names = people.ColumnStatistics(1);
    CHECK_EQ(names.min.as_varchar(), std::string("alice"));
    CHECK_EQ(names.max.as_varchar(), std::string("zoe"));
    CHECK_EQ(names.null_count, uint64_t{6});
    CHECK_EQ(names.distinct_estimate, uint64_t{14});

    const ColumnStats scores = people.ColumnStatistics(2);
    CHECK(scores.min.as_double() == -1.5);
    CHECK(scores.max.as_double() == 1000.125);
    CHECK_EQ(scores.null_count, uint64_t{2});
    // 50 + 125 distinct over 350 rows: still under the clamp.
    CHECK_EQ(scores.distinct_estimate, uint64_t{175});

    // Both segments are all-null in this column, so the merge must stay all_null
    // and must not offer bounds.
    const ColumnStats active = people.ColumnStatistics(3);
    CHECK_EQ(active.all_null, true);
    CHECK(active.min.is_null());
    CHECK(active.max.is_null());
    CHECK_EQ(active.null_count, uint64_t{350});
}

QUARRY_TEST(ColumnStatisticsEdgeCases) {
    TempDir dir("merge_edges");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());

    Schema s;
    s.Add(Column("x", TypeId::kInt64, true));
    CHECK_OK(catalog.CreateTable("t", s));

    auto empty = catalog.GetTable("t");
    CHECK(empty.ok());
    if (empty.ok()) {
        // No segments: all_null with no bounds, which prunes nothing.
        const ColumnStats stats = empty.value()->ColumnStatistics(0);
        CHECK_EQ(stats.all_null, true);
        CHECK(stats.min.is_null());
        CHECK_EQ(empty.value()->row_count(), uint64_t{0});
    }

    const auto add = [&](uint64_t id, uint64_t rows, bool all_null, int64_t lo, int64_t hi,
                         uint64_t nulls, uint64_t distinct) {
        SegmentMeta meta;
        meta.id = id;
        meta.row_count = rows;
        meta.path = "t_seg" + std::to_string(id);
        ColumnChunk chunk;
        chunk.type = TypeId::kInt64;
        chunk.encoding = Encoding::kPlain;
        if (all_null) {
            chunk.stats.all_null = true;
            chunk.stats.null_count = rows;
        } else {
            chunk.stats = Int64Stats(lo, hi, nulls, distinct);
        }
        meta.columns.push_back(chunk);
        CHECK_OK(catalog.AddSegment("t", meta));
    };

    // An all-null segment first, so the merge has to ignore its meaningless
    // bounds instead of seeding min and max from them.
    add(1, 10, true, 0, 0, 0, 0);
    add(2, 10, false, 50, 60, 2, 5);
    add(3, 10, false, -100, -90, 0, 4);

    auto t = catalog.GetTable("t");
    CHECK(t.ok());
    if (!t.ok()) return;
    const ColumnStats stats = t.value()->ColumnStatistics(0);
    CHECK_EQ(stats.all_null, false);
    CHECK_EQ(stats.min.as_int64(), int64_t{-100});
    CHECK_EQ(stats.max.as_int64(), int64_t{60});
    CHECK_EQ(stats.null_count, uint64_t{12});
    CHECK_EQ(stats.distinct_estimate, uint64_t{9});

    // A column index past the schema is answered rather than read out of bounds.
    const ColumnStats none = t.value()->ColumnStatistics(42);
    CHECK_EQ(none.all_null, true);
}

QUARRY_TEST(ColumnStatisticsDistinctEstimateIsClampedToRowCount) {
    TempDir dir("merge_clamp");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    Schema s;
    s.Add(Column("x", TypeId::kInt64, true));
    CHECK_OK(catalog.CreateTable("t", s));

    // Two segments of 10 rows each claiming 10 distinct values apiece. If they
    // fully overlap the truth is 10, if they are disjoint it is 20, and the sum
    // happens to equal the row count here — the point of the case is the next one,
    // where the sum would exceed it.
    for (uint64_t id = 1; id <= 3; ++id) {
        SegmentMeta meta;
        meta.id = id;
        meta.row_count = 10;
        meta.path = "t_seg" + std::to_string(id);
        ColumnChunk chunk;
        chunk.type = TypeId::kInt64;
        chunk.encoding = Encoding::kPlain;
        chunk.stats = Int64Stats(0, 9, 0, 10);
        meta.columns.push_back(chunk);
        CHECK_OK(catalog.AddSegment("t", meta));
    }

    auto t = catalog.GetTable("t");
    CHECK(t.ok());
    if (!t.ok()) return;
    const ColumnStats stats = t.value()->ColumnStatistics(0);
    CHECK_EQ(t.value()->row_count(), uint64_t{30});
    CHECK_EQ(stats.distinct_estimate, uint64_t{30});

    // Now one more segment whose own estimate exceeds its row count (a sketch can
    // do that). The table-wide answer must still never exceed the table's rows.
    SegmentMeta liar;
    liar.id = 4;
    liar.row_count = 2;
    liar.path = "t_seg4";
    ColumnChunk chunk;
    chunk.type = TypeId::kInt64;
    chunk.encoding = Encoding::kPlain;
    chunk.stats = Int64Stats(100, 101, 0, 1000);
    liar.columns.push_back(chunk);
    CHECK_OK(catalog.AddSegment("t", liar));

    const ColumnStats after = t.value()->ColumnStatistics(0);
    CHECK_EQ(t.value()->row_count(), uint64_t{32});
    // The sum (1030) is clamped to the row count, but never below the largest
    // single-segment estimate, which is itself above the row count here.
    CHECK_EQ(after.distinct_estimate, uint64_t{1000});
}

QUARRY_TEST(CatalogSaveIsAtomicAndLeavesNoTempFile) {
    TempDir dir("atomic");
    Catalog catalog(dir.str());
    CHECK_OK(catalog.Open());
    CHECK_OK(catalog.CreateTable("people", PeopleSchema()));
    CHECK_OK(catalog.Save());
    CHECK_OK(catalog.AddSegment("people", MakeSegment(1, 10, 0, 9, "a", "z", 0.0, 1.0)));
    CHECK_OK(catalog.Save());

    CHECK(fs::exists(fs::path(dir.str()) / "catalog.qcat"));
    CHECK(!fs::exists(fs::path(dir.str()) / "catalog.qcat.tmp"));

    // A second save over an existing file must replace it, not append to it.
    Catalog reopened(dir.str());
    CHECK_OK(reopened.Open());
    auto t = reopened.GetTable("people");
    CHECK(t.ok());
    if (t.ok()) CHECK_EQ(t.value()->segments.size(), size_t{1});
}

QUARRY_TEST(CatalogPathsAreDistinctAndRooted) {
    Catalog catalog("/tmp/quarry_paths");
    const std::string seg = catalog.SegmentPath("people", 7);
    const std::string idx = catalog.IndexPath("people", "by_id");
    CHECK(seg.find("/tmp/quarry_paths") == 0);
    CHECK(idx.find("/tmp/quarry_paths") == 0);
    CHECK_NE(seg, idx);
    CHECK_NE(seg, catalog.SegmentPath("people", 8));
    CHECK_NE(seg, catalog.SegmentPath("events", 7));
    CHECK_NE(idx, catalog.IndexPath("people", "by_name"));
    CHECK_EQ(catalog.dir(), std::string("/tmp/quarry_paths"));
}

QUARRY_TEST(IndexKindNameIsStable) {
    CHECK_EQ(std::string(quarry::IndexKindName(IndexKind::kBTree)), std::string("btree"));
    CHECK_EQ(std::string(quarry::IndexKindName(IndexKind::kLsm)), std::string("lsm"));
}

QUARRY_TEST_MAIN()
