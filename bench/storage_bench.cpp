// Storage layer measurements: scan throughput, projection pushdown, segment
// pruning, encoding, and grouped aggregation.
//
// Everything here that could be asserted is measured instead. "Projection
// pushdown works" is a test; "reading one column of nine costs 11.7% of the bytes"
// is a benchmark, and only the second one can show that the saving is smaller than
// the column count suggests because every reader also reads a header and a footer.
//
// The pruning section is the one worth reading. Zone maps are usually presented as
// a free win, and they are not: they are a bet on the data being laid out so that
// a predicate's range falls inside few segments. The same query against a column
// correlated with insertion order and against a uniformly random column is run
// side by side here precisely so the case where pruning does nothing at all is on
// the record next to the case where it does everything.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/execution/aggregate.hpp"
#include "../src/storage/encoding.hpp"
#include "bench_data.hpp"
#include "bench_util.hpp"
#include "datagen.hpp"
#include "quarry/catalog.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"

using namespace quarry;
using namespace quarry::bench;

namespace {

constexpr size_t kRows = 2'000'000;
constexpr size_t kRowsPerSegment = 16384;  // the Database default
constexpr size_t kGroupRows = 1'000'000;

// Column positions in datagen's fact schema, named so a predicate below reads as
// the column it is about rather than as an index.
enum FactColumn : size_t {
    kId = 0,
    kTs = 1,
    kCategory = 2,
    kRegion = 3,
    kStatus = 4,
    kAmount = 5,
    kQuantity = 6,
    kScore = 7,
    kActive = 8,
};

std::vector<ColumnData> FactColumns(const datagen::Dataset& data) {
    std::vector<ColumnData> cols(9);
    cols[kId].type = TypeId::kInt64;
    cols[kTs].type = TypeId::kInt64;
    cols[kCategory].type = TypeId::kVarchar;
    cols[kRegion].type = TypeId::kVarchar;
    cols[kStatus].type = TypeId::kInt64;
    cols[kAmount].type = TypeId::kDouble;
    cols[kQuantity].type = TypeId::kInt64;
    cols[kScore].type = TypeId::kDouble;
    cols[kActive].type = TypeId::kBoolean;

    const size_t n = data.rows.size();
    cols[kId].i64.reserve(n);
    cols[kTs].i64.reserve(n);
    cols[kStatus].i64.reserve(n);
    cols[kQuantity].i64.reserve(n);
    cols[kAmount].f64.reserve(n);
    cols[kScore].f64.reserve(n);
    cols[kActive].boolean.reserve(n);
    cols[kCategory].str.reserve(n);
    cols[kRegion].str.reserve(n);

    for (const auto& r : data.rows) {
        cols[kId].i64.push_back(r.id);
        cols[kTs].i64.push_back(r.ts);
        cols[kCategory].str.push_back(r.category);
        cols[kRegion].str.push_back(r.region);
        cols[kStatus].i64.push_back(r.status);
        cols[kAmount].f64.push_back(r.amount);
        cols[kQuantity].i64.push_back(r.quantity);
        cols[kQuantity].nulls.push_back(r.quantity_null);
        cols[kScore].f64.push_back(r.score);
        cols[kScore].nulls.push_back(r.score_null);
        cols[kActive].boolean.push_back(r.active ? 1 : 0);
    }
    return cols;
}

// LogicalRowBytes is the in-memory width of one row of the fact schema, used as
// the denominator for a MB/sec figure that is about rows rather than about how
// well the encodings happened to compress.
double LogicalRowBytes(const datagen::Dataset& data) {
    double total = 0.0;
    for (const auto& r : data.rows) {
        total += static_cast<double>(6 * sizeof(int64_t) + 1);  // 4 int64, 2 double, 1 bool
        total += static_cast<double>(r.category.size() + r.region.size());
    }
    return total / static_cast<double>(data.rows.size());
}

std::vector<size_t> Identity(size_t n) {
    std::vector<size_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = i;
    return out;
}

Predicate Compare(size_t column, CompareOp op, Value constant) {
    Predicate p;
    p.column_index = column;
    p.op = op;
    p.constant = std::move(constant);
    return p;
}

// MakeScan builds a scan over the whole table with its own counter, so each
// configuration's bytes are attributable to it and nothing else.
std::unique_ptr<SequentialScanOperator> MakeScan(const TableDef& table,
                                                 std::vector<size_t> projection,
                                                 std::vector<Predicate> predicates,
                                                 ReadCounters* counters) {
    return std::make_unique<SequentialScanOperator>(table, std::move(projection),
                                                    std::move(predicates),
                                                    CountingFactory(table.schema, counters));
}

// ---------------------------------------------------------------------------
// 1. full column scan throughput
// ---------------------------------------------------------------------------

void BenchScanThroughput(const TableDef& table, double logical_row_bytes) {
    Heading("1. Full scan throughput");

    constexpr size_t kRuns = 5;
    ReadCounters counters;

    uint64_t rows = 0;
    uint64_t bytes = 0;
    const Sample all = Time("all 9 columns", kRuns, static_cast<double>(kRows), [&] {
        counters.Reset();
        auto scan = MakeScan(table, Identity(9), {}, &counters);
        rows = Drain(*scan).rows;
        bytes = counters.bytes_read;
    });

    ReadCounters one_counters;
    uint64_t one_bytes = 0;
    const Sample one = Time("1 int64 column (id)", kRuns, static_cast<double>(kRows), [&] {
        one_counters.Reset();
        auto scan = MakeScan(table, {kId}, {}, &one_counters);
        Drain(*scan);
        one_bytes = one_counters.bytes_read;
    });

    const double on_disk_mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double logical_mb =
        logical_row_bytes * static_cast<double>(kRows) / (1024.0 * 1024.0);

    std::cout << "rows: " << Thousands(kRows) << " in " << Thousands(table.segments.size())
              << " segments of " << Thousands(kRowsPerSegment) << "\n"
              << "rows produced per pass: " << Thousands(rows) << "\n"
              << "on-disk bytes for all columns: " << Bytes(bytes) << " ("
              << Fmt(static_cast<double>(bytes) / static_cast<double>(kRows), 1) << " B/row)\n"
              << "logical in-memory bytes:      " << Fmt(logical_mb) << " MiB ("
              << Fmt(logical_row_bytes, 1) << " B/row)\n\n";

    Table t({"projection", "ms min/med/max", "rows/sec", "on-disk MB/s", "logical MB/s",
             "bytes read", "max/min"});
    t.Row({all.name, MinMedMaxMs(all), Rate(all.PerSecond()),
           Fmt(on_disk_mb / (all.median_ns / 1e9)), Fmt(logical_mb / (all.median_ns / 1e9)),
           Bytes(bytes), Fmt(all.Spread())});
    t.Row({one.name, MinMedMaxMs(one), Rate(one.PerSecond()),
           Fmt(static_cast<double>(one_bytes) / (1024.0 * 1024.0) / (one.median_ns / 1e9)),
           Fmt(static_cast<double>(kRows * 8) / (1024.0 * 1024.0) / (one.median_ns / 1e9)),
           Bytes(one_bytes), Fmt(one.Spread())});
    t.Print();

    Note("The scan decodes each segment's projected columns in full at open time and\n"
         "then copies them row by row into the output batch, so these rates include a\n"
         "decode and a copy, not just a read. 'on-disk MB/s' uses bytes actually\n"
         "pread; 'logical MB/s' uses the uncompressed row width.");
}

// ---------------------------------------------------------------------------
// 2. projection pushdown
// ---------------------------------------------------------------------------

void BenchProjection(const TableDef& table) {
    Heading("2. Projection pushdown, measured in bytes rather than asserted");

    constexpr size_t kRuns = 5;

    struct Config {
        std::string name;
        std::vector<size_t> projection;
    };
    const std::vector<Config> configs = {
        {"1 of 9: id (int64)", {kId}},
        {"1 of 9: amount (double)", {kAmount}},
        {"1 of 9: category (varchar, dictionary)", {kCategory}},
        {"1 of 9: region (varchar, rle)", {kRegion}},
        {"3 of 9: id, status, amount", {kId, kStatus, kAmount}},
        {"9 of 9: everything", Identity(9)},
    };

    uint64_t full_bytes = 0;
    double full_ms = 0.0;
    std::vector<std::pair<std::string, std::pair<uint64_t, Sample>>> measured;

    for (const auto& cfg : configs) {
        ReadCounters counters;
        uint64_t bytes = 0;
        const Sample s = Time(cfg.name, kRuns, static_cast<double>(kRows), [&] {
            counters.Reset();
            auto scan = MakeScan(table, cfg.projection, {}, &counters);
            Drain(*scan);
            bytes = counters.bytes_read;
        });
        if (cfg.projection.size() == 9) {
            full_bytes = bytes;
            full_ms = s.median_ns / 1e6;
        }
        measured.emplace_back(cfg.name, std::make_pair(bytes, s));
    }

    Table t({"projection", "bytes read", "share of all 9", "B/row", "ms min/med/max",
             "share of full-scan time"});
    for (const auto& [name, pair] : measured) {
        const uint64_t bytes = pair.first;
        const Sample& s = pair.second;
        t.Row({name, Thousands(bytes),
               Fmt(100.0 * static_cast<double>(bytes) / static_cast<double>(full_bytes), 2) + "%",
               Fmt(static_cast<double>(bytes) / static_cast<double>(kRows), 2), MinMedMaxMs(s),
               Fmt(100.0 * (s.median_ns / 1e6) / full_ms, 1) + "%"});
    }
    t.Print();

    Note("A one-of-nine projection does not cost one ninth of the bytes, and the\n"
         "reason is visible in the numbers: every reader reads the 32-byte header and\n"
         "the footer regardless, and the nine columns are not the same size, so the\n"
         "share a column accounts for is its own size and not 1/9 of the total.");
}

// ---------------------------------------------------------------------------
// 3. segment pruning
// ---------------------------------------------------------------------------

void BenchPruning(const TableDef& table, const datagen::Dataset& data) {
    Heading("3. Segment pruning: correlated versus random columns");

    constexpr size_t kRuns = 5;
    const size_t target_rows = kRows / 1000;  // aim for 0.1% selectivity

    // The correlated case: id is 0..n-1 in insertion order, so a contiguous id
    // range falls inside a handful of consecutive segments and every other
    // segment's [min,max] excludes it.
    const int64_t id_lo = static_cast<int64_t>(kRows / 2);
    const int64_t id_hi = id_lo + static_cast<int64_t>(target_rows);

    // The uncorrelated case: amount is uniform random over [0, 10000), so every
    // segment's [min,max] covers nearly the whole domain and no segment can be
    // excluded, however narrow the predicate.
    const double amount_span = 10000.0 * static_cast<double>(target_rows) /
                               static_cast<double>(kRows);
    const double amount_lo = 5000.0;
    const double amount_hi = amount_lo + amount_span;

    // The realistic middle: ts is non-decreasing with per-row jitter, which is what
    // time-series data looks like. Segments overlap at their edges but not in bulk.
    const int64_t ts_lo = data.rows[kRows / 2].ts;
    const int64_t ts_hi = data.rows[std::min(kRows - 1, kRows / 2 + target_rows)].ts;

    struct Config {
        std::string name;
        size_t column;
        std::vector<Predicate> predicates;
    };
    const std::vector<Config> configs = {
        {"id in [" + std::to_string(id_lo) + ", " + std::to_string(id_hi) +
             ") - correlated with insertion order",
         kId,
         {Compare(kId, CompareOp::kGe, Value::Int64(id_lo)),
          Compare(kId, CompareOp::kLt, Value::Int64(id_hi))}},
        {"ts in a " + std::to_string(ts_hi - ts_lo) + "-tick window - non-decreasing with jitter",
         kTs,
         {Compare(kTs, CompareOp::kGe, Value::Int64(ts_lo)),
          Compare(kTs, CompareOp::kLt, Value::Int64(ts_hi))}},
        {"amount in a " + Fmt(amount_span, 2) + "-wide window - uniformly random",
         kAmount,
         {Compare(kAmount, CompareOp::kGe, Value::Double(amount_lo)),
          Compare(kAmount, CompareOp::kLt, Value::Double(amount_hi))}},
    };

    // The baseline every row below is compared against: the same projection with no
    // predicate at all, so "pruning saved this much" is a measured difference.
    ReadCounters base_counters;
    uint64_t base_bytes = 0;
    const Sample baseline = Time("no predicate", kRuns, static_cast<double>(kRows), [&] {
        base_counters.Reset();
        auto scan = MakeScan(table, {kId, kTs, kAmount}, {}, &base_counters);
        Drain(*scan);
        base_bytes = base_counters.bytes_read;
    });

    Table t({"predicate", "rows out", "selectivity", "segs scanned", "segs pruned", "pruned %",
             "bytes read", "vs no predicate", "ms min/med/max", "speedup"});
    t.Row({"(none, 3 columns projected)", Thousands(kRows), "100%",
           Thousands(table.segments.size()), "0", "0.0%", Thousands(base_bytes), "1.00x",
           MinMedMaxMs(baseline), "1.00x"});
    t.Separator();

    for (const auto& cfg : configs) {
        ReadCounters counters;
        DrainResult drained;
        uint64_t bytes = 0;
        const Sample s = Time(cfg.name, kRuns, static_cast<double>(kRows), [&] {
            counters.Reset();
            auto scan = MakeScan(table, {kId, kTs, kAmount}, cfg.predicates, &counters);
            drained = Drain(*scan);
            bytes = counters.bytes_read;
        });

        const uint64_t total_segs = drained.segments_scanned + drained.segments_pruned;
        t.Row({cfg.name, Thousands(drained.rows),
               Fmt(100.0 * static_cast<double>(drained.rows) / static_cast<double>(kRows), 4) + "%",
               Thousands(drained.segments_scanned), Thousands(drained.segments_pruned),
               Fmt(100.0 * static_cast<double>(drained.segments_pruned) /
                       static_cast<double>(total_segs == 0 ? 1 : total_segs),
                   1) +
                   "%",
               Thousands(bytes),
               Fmt(static_cast<double>(bytes) / static_cast<double>(base_bytes)) + "x",
               MinMedMaxMs(s), Fmt(baseline.median_ns / s.median_ns) + "x"});
    }
    t.Print();

    Note("This is the finding, and it is not a win: on the random column the zone map\n"
         "prunes nothing, reads every byte, and the query is no faster than the\n"
         "unfiltered scan - the predicate is pure extra work. Pruning is not a\n"
         "property of the engine, it is a property of the data's physical order, and\n"
         "an engine that claims it without saying so is overselling.");
}

// ---------------------------------------------------------------------------
// 4. encoding
// ---------------------------------------------------------------------------

void BenchEncoding(const datagen::Dataset& data) {
    Heading("4. Encoding: size and decode speed, on data suited and unsuited to each");

    constexpr size_t kRuns = 5;
    // One segment's worth, which is the unit an encoding decision is made over.
    const size_t n = std::min<size_t>(kRowsPerSegment, data.rows.size());

    // Six column shapes, chosen so that each encoding is the right answer for one
    // of them and the wrong answer for another.
    struct Shape {
        std::string name;
        ColumnData column;
    };
    std::vector<Shape> shapes;

    {
        ColumnData c;
        c.type = TypeId::kVarchar;
        for (size_t i = 0; i < n; ++i) c.str.push_back(data.rows[i].category);
        shapes.push_back({"varchar, 8 distinct, shuffled (dictionary's case)", std::move(c)});
    }
    {
        ColumnData c;
        c.type = TypeId::kVarchar;
        for (size_t i = 0; i < n; ++i) c.str.push_back(data.rows[i].region);
        shapes.push_back({"varchar, 4 distinct in 433-row runs (rle's case)", std::move(c)});
    }
    {
        // Unique strings: no runs and no repeats, so both alternatives must lose.
        ColumnData c;
        c.type = TypeId::kVarchar;
        for (size_t i = 0; i < n; ++i) c.str.push_back("row-" + std::to_string(i * 2654435761u));
        shapes.push_back({"varchar, all distinct (plain's case)", std::move(c)});
    }
    {
        ColumnData c;
        c.type = TypeId::kInt64;
        for (size_t i = 0; i < n; ++i) c.i64.push_back(data.rows[i].status);
        shapes.push_back({"int64, 5 distinct in 97-row runs (rle's case)", std::move(c)});
    }
    {
        ColumnData c;
        c.type = TypeId::kInt64;
        for (size_t i = 0; i < n; ++i) c.i64.push_back(data.rows[i].id);
        shapes.push_back({"int64, strictly increasing (no runs, no repeats)", std::move(c)});
    }
    {
        ColumnData c;
        c.type = TypeId::kDouble;
        for (size_t i = 0; i < n; ++i) c.f64.push_back(data.rows[i].amount);
        shapes.push_back({"double, uniform random (plain's case)", std::move(c)});
    }

    std::cout << "rows per column: " << Thousands(n)
              << " (one segment)\nsizes are the encoded chunk body including its 8-byte prefix\n\n";

    Table t({"column shape", "type", "auto-chosen", "plain B", "dict B", "rle B", "best/plain",
             "plain decode", "dict decode", "rle decode"});

    for (const auto& shape : shapes) {
        const Vector vec = MakeVector(shape.column);
        const encoding::ColumnProfile profile = encoding::Profile(vec, n);
        const Encoding chosen = encoding::Choose(shape.column.type, profile);

        std::vector<std::string> sizes(3, "n/a");
        std::vector<std::string> decodes(3, "n/a");
        double best_bytes = 0.0;
        double plain_bytes = 0.0;

        const Encoding candidates[3] = {Encoding::kPlain, Encoding::kDictionary,
                                        Encoding::kRunLength};
        for (size_t e = 0; e < 3; ++e) {
            std::vector<uint8_t> encoded;
            uint64_t raw_length = 0;
            const Status s = encoding::Encode(vec, n, candidates[e], encoded, raw_length);
            if (!s.ok()) continue;  // dictionary on a fixed-width type, by design

            sizes[e] = Thousands(encoded.size());
            if (candidates[e] == Encoding::kPlain) plain_bytes = static_cast<double>(encoded.size());
            if (best_bytes == 0.0 || static_cast<double>(encoded.size()) < best_bytes) {
                best_bytes = static_cast<double>(encoded.size());
            }

            Vector out(shape.column.type);
            const Sample d = Time("decode", kRuns, static_cast<double>(n), [&] {
                Check(encoding::Decode(encoded.data(), encoded.size(), shape.column.type, out),
                      "Decode");
                Sink(out.size());
            });
            decodes[e] = Rate(d.PerSecond());
        }

        t.Row({shape.name, TypeName(shape.column.type), EncodingName(chosen), sizes[0], sizes[1],
               sizes[2], plain_bytes == 0.0 ? "-" : Fmt(best_bytes / plain_bytes) + "x", decodes[0],
               decodes[1], decodes[2]});
    }
    t.Print();

    Note("Dictionary is varchar-only by construction: for a fixed-width type the\n"
         "codes are as wide as the values, so the entries are 'n/a' rather than\n"
         "slower. The decode rates are the number worth watching - the smallest\n"
         "encoding is not always the fastest to read back, and the auto-chooser\n"
         "optimizes size only.");
}

// ---------------------------------------------------------------------------
// 5. grouped aggregation
// ---------------------------------------------------------------------------

// GroupTable is a second table whose only purpose is to vary group cardinality
// independently of everything else: one int64 column per cardinality, plus a
// double to sum.
struct GroupTable {
    Schema schema;
    std::vector<ColumnData> columns;
    std::vector<size_t> cardinalities;
};

GroupTable MakeGroupTable(size_t rows) {
    GroupTable out;
    out.cardinalities = {2, 16, 256, 4096, 65536, 1'000'000};

    datagen::Rng rng(0x9a097000ULL);
    for (const size_t c : out.cardinalities) {
        out.schema.Add(Column("g" + std::to_string(c), TypeId::kInt64, false));
        ColumnData col;
        col.type = TypeId::kInt64;
        col.i64.reserve(rows);
        for (size_t i = 0; i < rows; ++i) {
            col.i64.push_back(static_cast<int64_t>(rng.Below(c)));
        }
        out.columns.push_back(std::move(col));
    }

    out.schema.Add(Column("v", TypeId::kDouble, false));
    ColumnData v;
    v.type = TypeId::kDouble;
    v.f64.reserve(rows);
    for (size_t i = 0; i < rows; ++i) v.f64.push_back(static_cast<double>(rng.Below(640000)) / 64.0);
    out.columns.push_back(std::move(v));
    return out;
}

void BenchGroupBy(const TableDef& table, const std::vector<size_t>& cardinalities) {
    Heading("5. GROUP BY throughput versus group cardinality");

    constexpr size_t kRuns = 3;
    const size_t value_column = cardinalities.size();  // "v" sits after the group columns

    std::cout << "rows: " << Thousands(kGroupRows)
              << "   aggregates: COUNT(*), SUM(v)   one group column per cardinality\n\n";

    Table t({"groups requested", "groups produced", "ms min/med/max", "rows/sec", "ns/row",
             "ns per group", "max/min"});

    for (size_t i = 0; i < cardinalities.size(); ++i) {
        std::vector<Aggregate> aggregates;
        {
            Aggregate count;
            count.kind = AggregateKind::kCount;
            count.output_name = "n";
            aggregates.push_back(count);
            Aggregate sum;
            sum.kind = AggregateKind::kSum;
            // The scan projects two columns, so inside the batch the value column is
            // at position 1 regardless of where it sits in the table.
            sum.column_index = 1;
            sum.output_name = "total";
            aggregates.push_back(sum);
        }

        ReadCounters counters;
        uint64_t groups = 0;
        const Sample s = Time("c=" + std::to_string(cardinalities[i]), kRuns,
                              static_cast<double>(kGroupRows), [&] {
                                  counters.Reset();
                                  auto scan = MakeScan(table, {i, value_column}, {}, &counters);
                                  HashAggregateOperator agg(std::move(scan), {0}, aggregates);
                                  groups = Drain(agg).rows;
                              });

        t.Row({Thousands(cardinalities[i]), Thousands(groups), MinMedMaxMs(s),
               Rate(s.PerSecond()), Fmt(s.NanosPerItem(), 2),
               Fmt(s.median_ns / static_cast<double>(groups == 0 ? 1 : groups), 1),
               Fmt(s.Spread())});
    }
    t.Print();

    Note("'ns per group' is total time divided by groups produced, so it is only\n"
         "meaningful at the high-cardinality end; at two groups it is an artefact.\n"
         "The number to read is ns/row, which is close to flat up to a few thousand\n"
         "groups and only degrades once the hash table and its per-group vector<Value>\n"
         "keys stop fitting in cache. The last row produces fewer groups than it\n"
         "requests because a million random draws over a million rows collide.");
}

}  // namespace

int main() {
    std::cout << "quarry storage benchmark\n";

    datagen::Spec spec;
    spec.rows = kRows;
    const datagen::Dataset data = datagen::Generate(spec);
    const double logical_row_bytes = LogicalRowBytes(data);

    const std::string dir = FreshDirectory("storage");
    Catalog catalog(dir);
    Check(catalog.Open(), "catalog Open");

    const std::vector<ColumnData> fact = FactColumns(data);
    const size_t segments =
        WriteTable(catalog, datagen::kFactTable, datagen::FactSchema(), fact, kRowsPerSegment);

    const GroupTable groups = MakeGroupTable(kGroupRows);
    WriteTable(catalog, "groups", groups.schema, groups.columns, kRowsPerSegment);

    const TableDef& events = *Take(catalog.GetTable(datagen::kFactTable), "GetTable events");
    const TableDef& group_table = *Take(catalog.GetTable("groups"), "GetTable groups");

    std::cout << "directory: " << dir << "\n"
              << "events: " << Thousands(kRows) << " rows in " << Thousands(segments)
              << " segments\n"
              << "groups: " << Thousands(kGroupRows) << " rows in "
              << Thousands(group_table.segments.size()) << " segments\n";

    BenchScanThroughput(events, logical_row_bytes);
    BenchProjection(events);
    BenchPruning(events, data);
    BenchEncoding(data);
    BenchGroupBy(group_table, groups.cardinalities);
    std::cout << "\n";
    return 0;
}
