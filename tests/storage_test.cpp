// Tests for the segment file: writing one, reading parts of one, and refusing to
// return data from one that has been damaged.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "../src/storage/encoding.hpp"
#include "../src/storage/segment_reader.hpp"
#include "../src/storage/segment_writer.hpp"
#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/storage.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

std::string TempPath(const char* stem) {
    return std::string("/tmp/quarry_segment_") + stem + "_" + std::to_string(::getpid()) + ".seg";
}

std::vector<uint8_t> ReadWholeFile(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

void WriteWholeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

Schema MixedSchema() {
    Schema s;
    s.Add(Column("id", TypeId::kInt64));
    s.Add(Column("price", TypeId::kDouble));
    s.Add(Column("active", TypeId::kBoolean));
    s.Add(Column("name", TypeId::kVarchar));
    return s;
}

// One row of the mixed schema, written as explicit nulls so a test states exactly
// what it expects to come back.
struct Row {
    bool null_id = false;
    int64_t id = 0;
    bool null_price = false;
    double price = 0;
    bool null_active = false;
    bool active = false;
    bool null_name = false;
    std::string name;
};

void AppendRow(Batch& batch, const Row& r) {
    if (r.null_id) batch.column(0).AppendNull(); else batch.column(0).AppendInt64(r.id);
    if (r.null_price) batch.column(1).AppendNull(); else batch.column(1).AppendDouble(r.price);
    if (r.null_active) batch.column(2).AppendNull(); else batch.column(2).AppendBoolean(r.active);
    if (r.null_name) batch.column(3).AppendNull(); else batch.column(3).AppendString(r.name);
}

// Reads every projected column of a segment into flat Value lists, which is the
// shape the tests compare against.
Status ReadAllRows(const SegmentMeta& meta, const Schema& schema,
                   const std::vector<size_t>& projection,
                   std::vector<std::vector<Value>>& out, uint64_t& bytes_read) {
    FileSegmentReader reader(meta, projection, schema);
    QUARRY_TRY(reader.Open());

    out.assign(projection.size(), {});
    Batch batch;
    bool done = false;
    while (true) {
        QUARRY_TRY(reader.Next(batch, done));
        if (done) break;
        for (size_t c = 0; c < projection.size(); ++c) {
            const Vector& v = batch.column(c);
            for (size_t i = 0; i < batch.row_count(); ++i) out[c].push_back(v.GetValue(i));
        }
    }
    bytes_read = reader.bytes_read();
    QUARRY_TRY(reader.Close());
    return Status::Ok();
}

// Distinguishes a null from an empty string, which Value::ToString does not.
std::string Describe(const Value& v) { return v.is_null() ? "<null>" : "'" + v.ToString() + "'"; }

void ExpectValue(const Value& want, const Value& got, const std::string& what, int line) {
    if (want.is_null() != got.is_null() || (!want.is_null() && !(want == got))) {
        testing::ReportFailure(__FILE__, line,
                               what + ": got " + Describe(got) + ", expected " + Describe(want));
    }
}

#define EXPECT_VALUE(want, got, what) ExpectValue(want, got, what, __LINE__)

}  // namespace

QUARRY_TEST(SegmentRoundTripEveryTypeWithNullsAndEmptyStrings) {
    const std::string path = TempPath("roundtrip");
    const Schema schema = MixedSchema();

    const std::vector<Row> rows = {
        {false, 1, false, 1.5, false, true, false, "alpha"},
        {true, 0, true, 0, true, false, true, ""},           // every column null
        {false, -7, false, -0.25, false, false, false, ""},  // empty string, not null
        {false, 0, false, 0.0, false, true, false, std::string("with\0nul", 8)},
        {false, INT64_MAX, false, 1e300, false, false, true, ""},
        {false, INT64_MIN, false, -1e-300, false, true, false, std::string(500, 'z')},
    };

    SegmentMeta written;
    {
        Batch batch(schema);
        for (const auto& r : rows) AppendRow(batch, r);
        batch.SetRowCount(rows.size());

        FileSegmentWriter writer(path, schema, 17);
        CHECK_OK(writer.Append(batch));
        auto result = writer.Finish();
        CHECK(result.ok());
        if (!result.ok()) return;
        written = result.take();
    }

    CHECK_EQ(written.row_count, rows.size());
    CHECK_EQ(written.id, 17u);
    CHECK_EQ(written.columns.size(), 4u);

    std::vector<std::vector<Value>> got;
    uint64_t bytes_read = 0;
    CHECK_OK(ReadAllRows(written, schema, {0, 1, 2, 3}, got, bytes_read));

    CHECK_EQ(got.size(), 4u);
    for (const auto& column : got) CHECK_EQ(column.size(), rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const std::string at = "row " + std::to_string(i);
        EXPECT_VALUE(r.null_id ? Value::Null() : Value::Int64(r.id), got[0][i], at + " id");
        EXPECT_VALUE(r.null_price ? Value::Null() : Value::Double(r.price), got[1][i],
                     at + " price");
        EXPECT_VALUE(r.null_active ? Value::Null() : Value::Boolean(r.active), got[2][i],
                     at + " active");
        EXPECT_VALUE(r.null_name ? Value::Null() : Value::Varchar(r.name), got[3][i],
                     at + " name");
    }

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentStatisticsMatchTheData) {
    const std::string path = TempPath("stats");
    const Schema schema = MixedSchema();

    Batch batch(schema);
    AppendRow(batch, {false, 5, false, 2.5, false, true, false, "pear"});
    AppendRow(batch, {false, -3, true, 0, false, false, false, "apple"});
    AppendRow(batch, {false, 11, false, -1.0, false, true, true, ""});
    batch.SetRowCount(3);

    FileSegmentWriter writer(path, schema, 1);
    CHECK_OK(writer.Append(batch));
    auto result = writer.Finish();
    CHECK(result.ok());
    if (!result.ok()) return;
    const SegmentMeta meta = result.take();

    EXPECT_VALUE(Value::Int64(-3), meta.columns[0].stats.min, "id min");
    EXPECT_VALUE(Value::Int64(11), meta.columns[0].stats.max, "id max");
    CHECK_EQ(meta.columns[0].stats.null_count, 0u);
    CHECK(!meta.columns[0].stats.all_null);

    EXPECT_VALUE(Value::Double(-1.0), meta.columns[1].stats.min, "price min");
    EXPECT_VALUE(Value::Double(2.5), meta.columns[1].stats.max, "price max");
    CHECK_EQ(meta.columns[1].stats.null_count, 1u);

    EXPECT_VALUE(Value::Boolean(false), meta.columns[2].stats.min, "active min");
    EXPECT_VALUE(Value::Boolean(true), meta.columns[2].stats.max, "active max");

    EXPECT_VALUE(Value::Varchar("apple"), meta.columns[3].stats.min, "name min");
    EXPECT_VALUE(Value::Varchar("pear"), meta.columns[3].stats.max, "name max");
    CHECK_EQ(meta.columns[3].stats.null_count, 1u);
    CHECK_EQ(meta.columns[3].stats.distinct_estimate, 3u);  // "pear", "apple", and the null slot

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentAllNullColumnHasNoBounds) {
    const std::string path = TempPath("allnull");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));

    Batch batch(schema);
    for (int i = 0; i < 50; ++i) batch.column(0).AppendNull();
    batch.SetRowCount(50);

    FileSegmentWriter writer(path, schema, 2);
    CHECK_OK(writer.Append(batch));
    auto result = writer.Finish();
    CHECK(result.ok());
    if (!result.ok()) return;
    const SegmentMeta meta = result.take();

    CHECK(meta.columns[0].stats.all_null);
    CHECK(meta.columns[0].stats.min.is_null());
    CHECK(meta.columns[0].stats.max.is_null());
    CHECK_EQ(meta.columns[0].stats.null_count, 50u);

    // And the pruning decision that depends on it.
    CHECK(!CanMatch(meta.columns[0].stats, CompareOp::kEq, Value::Int64(0)));

    std::vector<std::vector<Value>> got;
    uint64_t bytes = 0;
    CHECK_OK(ReadAllRows(meta, schema, {0}, got, bytes));
    CHECK_EQ(got[0].size(), 50u);
    for (const auto& v : got[0]) CHECK(v.is_null());

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentMultiBatchAppendPreservesOrderAcrossBatchBoundaries) {
    const std::string path = TempPath("multibatch");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));
    schema.Add(Column("s", TypeId::kVarchar));

    // Deliberately more than kBatchSize in total and not a multiple of it, so the
    // reader's last batch is partial and the batch boundaries on the way in do not
    // line up with the ones on the way out.
    const size_t kRows = 3 * kBatchSize + 37;
    const size_t kPerAppend = 700;

    {
        FileSegmentWriter writer(path, schema, 3);
        size_t written = 0;
        while (written < kRows) {
            const size_t n = std::min(kPerAppend, kRows - written);
            Batch batch(schema);
            for (size_t i = 0; i < n; ++i) {
                const size_t row = written + i;
                if (row % 97 == 0) {
                    batch.column(0).AppendNull();
                } else {
                    batch.column(0).AppendInt64(static_cast<int64_t>(row));
                }
                batch.column(1).AppendString("r" + std::to_string(row));
            }
            batch.SetRowCount(n);
            CHECK_OK(writer.Append(batch));
            written += n;
        }
        auto result = writer.Finish();
        CHECK(result.ok());
        if (!result.ok()) return;
        CHECK_EQ(result.value().row_count, kRows);
    }

    auto file = ReadSegmentFile(path);
    CHECK(file.ok());
    if (!file.ok()) return;
    const SegmentFile sf = file.take();

    FileSegmentReader reader(sf.meta, {0, 1}, sf.schema);
    CHECK_OK(reader.Open());

    size_t seen = 0;
    size_t batches = 0;
    Batch out;
    bool done = false;
    while (true) {
        CHECK_OK(reader.Next(out, done));
        if (done) break;
        ++batches;
        CHECK(out.row_count() <= kBatchSize);
        for (size_t i = 0; i < out.row_count(); ++i) {
            const size_t row = seen + i;
            if (row % 97 == 0) {
                CHECK(out.column(0).nulls().IsNull(i));
            } else {
                CHECK_EQ(out.column(0).int64_data()[i], static_cast<int64_t>(row));
            }
            CHECK(out.column(1).GetString(i) == "r" + std::to_string(row));
        }
        seen += out.row_count();
    }
    CHECK_OK(reader.Close());

    CHECK_EQ(seen, kRows);
    CHECK_EQ(batches, 4u);

    // The output batch schema carries the table's column names, not positional
    // placeholders, which is what a projection pushed down from a plan needs.
    CHECK(out.schema().size() == 2);

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentAppendRespectsASelectionVector) {
    const std::string path = TempPath("selection");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));

    Batch batch(schema);
    for (int i = 0; i < 10; ++i) batch.column(0).AppendInt64(i);
    batch.SetRowCount(10);
    batch.SetSelection({1, 3, 5, 7});

    FileSegmentWriter writer(path, schema, 4);
    CHECK_OK(writer.Append(batch));
    auto result = writer.Finish();
    CHECK(result.ok());
    if (!result.ok()) return;
    const SegmentMeta meta = result.take();

    CHECK_EQ(meta.row_count, 4u);
    EXPECT_VALUE(Value::Int64(1), meta.columns[0].stats.min, "selected min");
    EXPECT_VALUE(Value::Int64(7), meta.columns[0].stats.max, "selected max");

    std::vector<std::vector<Value>> got;
    uint64_t bytes = 0;
    CHECK_OK(ReadAllRows(meta, schema, {0}, got, bytes));
    CHECK_EQ(got[0].size(), 4u);
    EXPECT_VALUE(Value::Int64(1), got[0][0], "selected row 0");
    EXPECT_VALUE(Value::Int64(3), got[0][1], "selected row 1");
    EXPECT_VALUE(Value::Int64(5), got[0][2], "selected row 2");
    EXPECT_VALUE(Value::Int64(7), got[0][3], "selected row 3");

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentEmptySegmentReadsAsZeroRows) {
    const std::string path = TempPath("empty");
    const Schema schema = MixedSchema();

    FileSegmentWriter writer(path, schema, 5);
    auto result = writer.Finish();
    CHECK(result.ok());
    if (!result.ok()) return;
    const SegmentMeta meta = result.take();
    CHECK_EQ(meta.row_count, 0u);

    auto file = ReadSegmentFile(path);
    CHECK(file.ok());
    if (!file.ok()) return;
    CHECK_EQ(file.value().meta.row_count, 0u);
    CHECK_EQ(file.value().meta.columns.size(), 4u);

    FileSegmentReader reader(meta, {0, 3}, schema);
    CHECK_OK(reader.Open());
    Batch out;
    bool done = false;
    CHECK_OK(reader.Next(out, done));
    CHECK(done);
    CHECK_EQ(out.row_count(), 0u);
    CHECK_OK(reader.Close());

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentProjectionReadsSubstantiallyFewerBytes) {
    const std::string path = TempPath("projection");

    // Five columns of equal width and high entropy, so the only thing that can
    // make one read smaller than another is reading fewer columns.
    Schema schema;
    for (int c = 0; c < 5; ++c) schema.Add(Column("c" + std::to_string(c), TypeId::kInt64));

    const size_t kRows = 4096;
    {
        std::mt19937_64 rng(99);
        Batch batch(schema);
        for (size_t i = 0; i < kRows; ++i) {
            for (int c = 0; c < 5; ++c) batch.column(c).AppendInt64(static_cast<int64_t>(rng()));
        }
        batch.SetRowCount(kRows);

        FileSegmentWriter writer(path, schema, 6);
        CHECK_OK(writer.Append(batch));
        auto result = writer.Finish();
        CHECK(result.ok());
        if (!result.ok()) return;
    }

    auto file = ReadSegmentFile(path);
    CHECK(file.ok());
    if (!file.ok()) return;
    const SegmentFile sf = file.take();

    std::vector<std::vector<Value>> all, one;
    uint64_t full_bytes = 0, projected_bytes = 0;
    CHECK_OK(ReadAllRows(sf.meta, sf.schema, {0, 1, 2, 3, 4}, all, full_bytes));
    CHECK_OK(ReadAllRows(sf.meta, sf.schema, {2}, one, projected_bytes));

    CHECK_EQ(all.size(), 5u);
    CHECK_EQ(one.size(), 1u);
    CHECK_EQ(one[0].size(), kRows);
    for (size_t i = 0; i < kRows; ++i) EXPECT_VALUE(all[2][i], one[0][i], "projected row " +
                                                                             std::to_string(i));

    const double ratio = static_cast<double>(projected_bytes) / static_cast<double>(full_bytes);
    std::cout << "       projection: 1 of 5 columns read " << projected_bytes << " of "
              << full_bytes << " bytes (" << ratio << ")\n";

    // One of five columns must cost about a fifth. The slack is the 32-byte header,
    // which is read either way.
    CHECK(ratio < 0.25);
    CHECK(projected_bytes > 0);

    // Two columns must cost about twice one column, which rules out a reader that
    // happens to read a fixed amount regardless of the projection.
    std::vector<std::vector<Value>> two;
    uint64_t two_bytes = 0;
    CHECK_OK(ReadAllRows(sf.meta, sf.schema, {1, 3}, two, two_bytes));
    CHECK(two_bytes > projected_bytes);
    CHECK(two_bytes < full_bytes);

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentReaderRejectsAProjectionOutsideTheSegment) {
    const std::string path = TempPath("badprojection");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));

    Batch batch(schema);
    batch.column(0).AppendInt64(1);
    batch.SetRowCount(1);

    FileSegmentWriter writer(path, schema, 7);
    CHECK_OK(writer.Append(batch));
    auto result = writer.Finish();
    CHECK(result.ok());
    if (!result.ok()) return;

    FileSegmentReader reader(result.take(), {0, 4}, schema);
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kInvalidArgument);

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentWriterRejectsAMismatchedBatch) {
    const std::string path = TempPath("mismatch");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));
    schema.Add(Column("s", TypeId::kVarchar));

    FileSegmentWriter writer(path, schema, 8);

    Schema wrong_width;
    wrong_width.Add(Column("n", TypeId::kInt64));
    Batch narrow(wrong_width);
    narrow.SetRowCount(0);
    const Status width = writer.Append(narrow);
    CHECK(!width.ok());
    CHECK(width.code() == Code::kInvalidArgument);

    Schema wrong_type;
    wrong_type.Add(Column("n", TypeId::kInt64));
    wrong_type.Add(Column("s", TypeId::kDouble));
    Batch mistyped(wrong_type);
    mistyped.SetRowCount(0);
    const Status type = writer.Append(mistyped);
    CHECK(!type.ok());
    CHECK(type.code() == Code::kTypeError);

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentWriterIsSpentAfterFinish) {
    const std::string path = TempPath("spent");
    Schema schema;
    schema.Add(Column("n", TypeId::kInt64));

    FileSegmentWriter writer(path, schema, 9);
    auto first = writer.Finish();
    CHECK(first.ok());

    Batch batch(schema);
    batch.SetRowCount(0);
    CHECK_NOT_OK(writer.Append(batch));

    auto second = writer.Finish();
    CHECK(!second.ok());
    CHECK(second.status().code() == Code::kInternal);

    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// Damaged files. The requirement is that damage becomes kCorruption, never data.
// ---------------------------------------------------------------------------

namespace {

// Writes a two-column segment and hands back both the metadata and the file bytes
// so a test can damage a specific region.
struct Fixture {
    std::string path;
    Schema schema;
    SegmentMeta meta;
    std::vector<uint8_t> bytes;
};

bool BuildFixture(const char* stem, Fixture& out) {
    out.path = TempPath(stem);
    out.schema.Add(Column("n", TypeId::kInt64));
    out.schema.Add(Column("s", TypeId::kVarchar));

    Batch batch(out.schema);
    for (int i = 0; i < 200; ++i) {
        batch.column(0).AppendInt64(i * 3);
        batch.column(1).AppendString("value-" + std::to_string(i));
    }
    batch.SetRowCount(200);

    FileSegmentWriter writer(out.path, out.schema, 100);
    if (!writer.Append(batch).ok()) return false;
    auto result = writer.Finish();
    if (!result.ok()) return false;
    out.meta = result.take();
    out.bytes = ReadWholeFile(out.path);
    return !out.bytes.empty();
}

}  // namespace

QUARRY_TEST(SegmentCorruptedChunkIsReportedNotReturned) {
    Fixture f;
    CHECK(BuildFixture("corrupt", f));
    if (f.bytes.empty()) return;

    // A single flipped bit in the middle of column 1's payload.
    std::vector<uint8_t> damaged = f.bytes;
    const uint64_t at = f.meta.columns[1].offset + f.meta.columns[1].length / 2;
    CHECK(at < damaged.size());
    damaged[at] ^= 0x40;
    WriteWholeFile(f.path, damaged);

    {
        FileSegmentReader reader(f.meta, {1}, f.schema);
        const Status s = reader.Open();
        CHECK(!s.ok());
        CHECK(s.code() == Code::kCorruption);

        // No rows may be produced from a chunk that failed its checksum.
        Batch out;
        bool done = false;
        CHECK_NOT_OK(reader.Next(out, done));
        CHECK_EQ(out.row_count(), 0u);
    }

    // The undamaged column is still readable: a checksum is per chunk, so one bad
    // column does not cost a query that never looks at it.
    {
        std::vector<std::vector<Value>> got;
        uint64_t bytes = 0;
        CHECK_OK(ReadAllRows(f.meta, f.schema, {0}, got, bytes));
        CHECK_EQ(got[0].size(), 200u);
        EXPECT_VALUE(Value::Int64(597), got[0][199], "last row of the intact column");
    }

    ::unlink(f.path.c_str());
}

QUARRY_TEST(SegmentCorruptedChunkChecksumFieldIsDetected) {
    Fixture f;
    CHECK(BuildFixture("corruptcrc", f));
    if (f.bytes.empty()) return;

    // Damage the stored checksum rather than the payload. A reader that only
    // checked a length would see nothing wrong here.
    std::vector<uint8_t> damaged = f.bytes;
    const uint64_t crc_at = f.meta.columns[0].offset + f.meta.columns[0].length - 4;
    damaged[crc_at] ^= 0xFF;
    WriteWholeFile(f.path, damaged);

    FileSegmentReader reader(f.meta, {0}, f.schema);
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kCorruption);

    ::unlink(f.path.c_str());
}

QUARRY_TEST(SegmentTruncatedFileIsRejected) {
    Fixture f;
    CHECK(BuildFixture("truncated", f));
    if (f.bytes.empty()) return;

    // Lose the footer entirely: the last eight bytes no longer hold a footer
    // offset, so metadata recovery must fail rather than read a plausible number.
    std::vector<uint8_t> cut(f.bytes.begin(), f.bytes.begin() + f.bytes.size() / 2);
    WriteWholeFile(f.path, cut);

    auto file = ReadSegmentFile(f.path);
    CHECK(!file.ok());
    if (!file.ok()) CHECK(file.status().code() == Code::kCorruption);

    // And a reader holding the metadata from before the truncation must not read
    // past the end of what is left.
    FileSegmentReader reader(f.meta, {0, 1}, f.schema);
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kCorruption);

    // Truncated to nothing at all.
    WriteWholeFile(f.path, {});
    auto empty = ReadSegmentFile(f.path);
    CHECK(!empty.ok());
    if (!empty.ok()) CHECK(empty.status().code() == Code::kCorruption);

    // Truncated to a header and nothing else.
    std::vector<uint8_t> header_only(f.bytes.begin(), f.bytes.begin() + segfmt::kHeaderBytes);
    WriteWholeFile(f.path, header_only);
    auto headerless = ReadSegmentFile(f.path);
    CHECK(!headerless.ok());
    if (!headerless.ok()) CHECK(headerless.status().code() == Code::kCorruption);

    ::unlink(f.path.c_str());
}

QUARRY_TEST(SegmentFooterOffsetSurvivesAndIsAuthoritative) {
    Fixture f;
    CHECK(BuildFixture("footer", f));
    if (f.bytes.empty()) return;

    // The last eight bytes point at the footer, and the footer reproduces the chunk
    // table the writer returned — offsets, lengths, encodings, and bounds — without
    // any part of the data region being read.
    const uint64_t footer_offset = GetU64(f.bytes.data() + f.bytes.size() - 8);
    CHECK(footer_offset >= segfmt::kHeaderBytes);
    CHECK(footer_offset < f.bytes.size() - segfmt::kFooterTrailerBytes + 1);
    CHECK(std::memcmp(f.bytes.data() + footer_offset, segfmt::kFooterMagic, 4) == 0);

    auto file = ReadSegmentFile(f.path);
    CHECK(file.ok());
    if (!file.ok()) return;
    const SegmentFile sf = file.take();

    CHECK_EQ(sf.meta.id, f.meta.id);
    CHECK_EQ(sf.meta.row_count, f.meta.row_count);
    CHECK_EQ(sf.meta.columns.size(), f.meta.columns.size());
    CHECK(sf.schema == f.schema);
    for (size_t c = 0; c < sf.meta.columns.size(); ++c) {
        const ColumnChunk& a = f.meta.columns[c];
        const ColumnChunk& b = sf.meta.columns[c];
        CHECK(a.type == b.type);
        CHECK(a.encoding == b.encoding);
        CHECK_EQ(a.offset, b.offset);
        CHECK_EQ(a.length, b.length);
        CHECK_EQ(a.raw_length, b.raw_length);
        CHECK_EQ(a.stats.null_count, b.stats.null_count);
        CHECK_EQ(a.stats.distinct_estimate, b.stats.distinct_estimate);
        CHECK(a.stats.all_null == b.stats.all_null);
        EXPECT_VALUE(a.stats.min, b.stats.min, "recovered min of column " + std::to_string(c));
        EXPECT_VALUE(a.stats.max, b.stats.max, "recovered max of column " + std::to_string(c));
    }

    // A footer offset that points outside the file is rejected rather than chased.
    const uint64_t bogus_offsets[] = {0, 7, static_cast<uint64_t>(f.bytes.size()) + 1,
                                      uint64_t{1} << 60};
    for (uint64_t bogus : bogus_offsets) {
        std::vector<uint8_t> damaged = f.bytes;
        for (int i = 0; i < 8; ++i) {
            damaged[damaged.size() - 8 + i] = static_cast<uint8_t>(bogus >> (8 * i));
        }
        WriteWholeFile(f.path, damaged);
        auto bad = ReadSegmentFile(f.path);
        CHECK(!bad.ok());
        if (!bad.ok()) CHECK(bad.status().code() == Code::kCorruption);
    }

    // A footer whose body has been edited fails its own checksum.
    {
        std::vector<uint8_t> damaged = f.bytes;
        damaged[footer_offset + 6] ^= 0x11;
        WriteWholeFile(f.path, damaged);
        auto bad = ReadSegmentFile(f.path);
        CHECK(!bad.ok());
        if (!bad.ok()) CHECK(bad.status().code() == Code::kCorruption);
    }

    // A damaged magic is rejected before anything else is believed.
    {
        std::vector<uint8_t> damaged = f.bytes;
        damaged[0] = 'X';
        WriteWholeFile(f.path, damaged);
        auto bad = ReadSegmentFile(f.path);
        CHECK(!bad.ok());
        if (!bad.ok()) CHECK(bad.status().code() == Code::kCorruption);
    }

    ::unlink(f.path.c_str());
}

QUARRY_TEST(SegmentFooterThatDisagreesWithItsPayloadIsRejected) {
    Fixture f;
    CHECK(BuildFixture("disagree", f));
    if (f.bytes.empty()) return;

    // Rewrite column 0's encoding byte in the footer to a different valid encoding
    // and repair the footer checksum, which is what a format-aware but incorrect
    // writer would produce. The payload itself still says what it is, so the reader
    // must notice the disagreement rather than decode with the wrong layout.
    std::vector<uint8_t> damaged = f.bytes;
    const uint64_t footer_offset = GetU64(damaged.data() + damaged.size() - 8);
    const uint64_t enc_at = footer_offset + 8 + 1;  // magic, column count, then type byte
    const uint8_t original = damaged[enc_at];
    damaged[enc_at] = original == static_cast<uint8_t>(Encoding::kPlain)
                          ? static_cast<uint8_t>(Encoding::kRunLength)
                          : static_cast<uint8_t>(Encoding::kPlain);

    const size_t footer_len = damaged.size() - segfmt::kFooterTrailerBytes - footer_offset;
    const uint32_t crc = encoding::Crc32(damaged.data() + footer_offset, footer_len);
    PutU32(damaged.data() + damaged.size() - segfmt::kFooterTrailerBytes, crc);
    WriteWholeFile(f.path, damaged);

    auto file = ReadSegmentFile(f.path);
    CHECK(file.ok());  // the footer is internally consistent now
    if (!file.ok()) return;
    const SegmentFile sf = file.take();

    FileSegmentReader reader(sf.meta, {0}, sf.schema);
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kCorruption);

    ::unlink(f.path.c_str());
}

QUARRY_TEST(SegmentReaderRejectsMetadataFromADifferentFile) {
    // Stale metadata is the realistic version of this failure: a catalog entry that
    // outlived the file it described. The header cross-check is what turns it into
    // an error rather than rows decoded at the wrong offsets.
    Fixture f;
    CHECK(BuildFixture("stale", f));
    if (f.bytes.empty()) return;

    const std::string other = TempPath("stale_other");
    {
        Schema schema;
        schema.Add(Column("n", TypeId::kInt64));
        schema.Add(Column("s", TypeId::kVarchar));
        Batch batch(schema);
        for (int i = 0; i < 20; ++i) {
            batch.column(0).AppendInt64(i);
            batch.column(1).AppendString("x");
        }
        batch.SetRowCount(20);
        FileSegmentWriter writer(other, schema, 999);
        CHECK_OK(writer.Append(batch));
        CHECK(writer.Finish().ok());
    }

    SegmentMeta stale = f.meta;
    stale.path = other;  // 200-row metadata pointed at a 20-row file
    FileSegmentReader reader(stale, {0}, f.schema);
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kCorruption);

    ::unlink(f.path.c_str());
    ::unlink(other.c_str());
}

QUARRY_TEST(SegmentReadBackWrittenBySeparateWriterInstance) {
    // The writer goes out of scope before anything reads the file, and the reader
    // is built only from what ReadSegmentFile recovers — no state is shared between
    // the two, which is the arrangement a catalog and a scan actually have.
    const std::string path = TempPath("separate");
    const size_t kRows = 1500;

    {
        Schema schema;
        schema.Add(Column("id", TypeId::kInt64));
        schema.Add(Column("city", TypeId::kVarchar));
        schema.Add(Column("flag", TypeId::kBoolean));

        FileSegmentWriter writer(path, schema, 4242);
        const char* cities[] = {"seattle", "austin", "boston"};
        for (int chunk = 0; chunk < 3; ++chunk) {
            Batch batch(schema);
            for (size_t i = 0; i < kRows / 3; ++i) {
                const size_t row = chunk * (kRows / 3) + i;
                batch.column(0).AppendInt64(static_cast<int64_t>(row));
                batch.column(1).AppendString(cities[row % 3]);
                batch.column(2).AppendBoolean(row % 2 == 0);
            }
            batch.SetRowCount(kRows / 3);
            CHECK_OK(writer.Append(batch));
        }
        CHECK(writer.Finish().ok());
    }

    auto file = ReadSegmentFile(path);
    CHECK(file.ok());
    if (!file.ok()) return;
    const SegmentFile sf = file.take();

    CHECK_EQ(sf.meta.id, 4242u);
    CHECK_EQ(sf.meta.row_count, kRows);
    CHECK(sf.schema.IndexOf("city").has_value());

    // The low-cardinality city column should have been dictionary encoded, which is
    // a decision taken over all three appends rather than per batch.
    CHECK(sf.meta.columns[1].encoding == Encoding::kDictionary);

    std::vector<std::vector<Value>> got;
    uint64_t bytes = 0;
    CHECK_OK(ReadAllRows(sf.meta, sf.schema, {1, 2}, got, bytes));
    CHECK_EQ(got[0].size(), kRows);
    const char* cities[] = {"seattle", "austin", "boston"};
    for (size_t row = 0; row < kRows; ++row) {
        EXPECT_VALUE(Value::Varchar(cities[row % 3]), got[0][row],
                     "city of row " + std::to_string(row));
        EXPECT_VALUE(Value::Boolean(row % 2 == 0), got[1][row],
                     "flag of row " + std::to_string(row));
    }

    ::unlink(path.c_str());
}

QUARRY_TEST(SegmentMissingFileIsAnIoErrorNotACrash) {
    auto file = ReadSegmentFile("/tmp/quarry_segment_definitely_absent.seg");
    CHECK(!file.ok());
    if (!file.ok()) CHECK(file.status().code() == Code::kIoError);

    SegmentMeta meta;
    meta.path = "/tmp/quarry_segment_definitely_absent.seg";
    FileSegmentReader reader(meta, {}, Schema());
    const Status s = reader.Open();
    CHECK(!s.ok());
    CHECK(s.code() == Code::kIoError);
}

QUARRY_TEST_MAIN()
