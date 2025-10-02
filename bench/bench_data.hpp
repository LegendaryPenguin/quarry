#pragma once

// Building benchmark tables without going through SQL.
//
// The obvious way to load a benchmark table is INSERT ... VALUES, and the tests do
// exactly that. At benchmark row counts it is the wrong choice: rendering and
// re-parsing millions of rows of SQL text takes longer than every measurement in
// this directory combined, and none of that time is engine behaviour anyone wants
// a number for. So these helpers write segments through FileSegmentWriter and
// register them in the catalog — the same two calls Database::FlushPending makes,
// with the parser removed.
//
// The counting reader wrapper exists for the same reason the projection benchmark
// exists at all: SegmentReader::bytes_read() is the only direct evidence that
// reading one column of nine costs a ninth of the bytes, and the scan operator
// closes and destroys its readers, so the counter has to be siphoned off before
// the reader goes away.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/execution/scan.hpp"
#include "../src/storage/segment_reader.hpp"
#include "../src/storage/segment_writer.hpp"
#include "bench_util.hpp"
#include "quarry/batch.hpp"
#include "quarry/catalog.hpp"
#include "quarry/schema.hpp"
#include "quarry/types.hpp"

namespace quarry::bench {

// ColumnData is one column in whichever of the four physical types it has. Only
// the vector matching `type` is populated; `nulls` is empty when the column has
// none, so a column with no nulls costs no per-row bookkeeping to build.
struct ColumnData {
    TypeId type = TypeId::kInvalid;
    std::vector<int64_t> i64;
    std::vector<double> f64;
    std::vector<uint8_t> boolean;
    std::vector<std::string> str;
    std::vector<bool> nulls;

    size_t size() const {
        switch (type) {
            case TypeId::kInt64:   return i64.size();
            case TypeId::kDouble:  return f64.size();
            case TypeId::kBoolean: return boolean.size();
            case TypeId::kVarchar: return str.size();
            default:               return 0;
        }
    }

    bool IsNull(size_t row) const { return !nulls.empty() && nulls[row]; }
};

// AppendRowTo appends one row of a column into a Vector, preserving nullness.
inline void AppendRowTo(Vector& dst, const ColumnData& src, size_t row) {
    if (src.IsNull(row)) {
        dst.AppendNull();
        return;
    }
    switch (src.type) {
        case TypeId::kInt64:   dst.AppendInt64(src.i64[row]); break;
        case TypeId::kDouble:  dst.AppendDouble(src.f64[row]); break;
        case TypeId::kBoolean: dst.AppendBoolean(src.boolean[row] != 0); break;
        case TypeId::kVarchar: dst.AppendString(src.str[row]); break;
        default:               Fail("column has an invalid type");
    }
}

// MakeVector materializes a whole column as one Vector, used by the encoding
// benchmark, which works at the encoding layer rather than through a segment.
inline Vector MakeVector(const ColumnData& src) {
    Vector v(src.type);
    v.Reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) AppendRowTo(v, src, i);
    return v;
}

// FreshDirectory removes and recreates a directory, so a run never measures a
// table left behind by the previous one.
inline std::string FreshDirectory(const std::string& label) {
    const std::string path =
        (std::filesystem::temp_directory_path() / ("quarry-bench-" + label)).string();
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    if (ec) Fail("cannot create " + path + ": " + ec.message());
    return path;
}

// WriteTable creates `name` in the catalog and writes `columns` into segments of
// `rows_per_segment` rows each. Returns the number of segments written.
//
// Segment granularity is a parameter rather than a constant because it is the
// single knob that decides how precise zone-map pruning can be, and the pruning
// benchmark needs to vary it.
inline size_t WriteTable(Catalog& catalog, const std::string& name, const Schema& schema,
                         const std::vector<ColumnData>& columns, size_t rows_per_segment) {
    if (columns.size() != schema.size()) Fail("WriteTable: column count does not match the schema");
    const size_t rows = columns.empty() ? 0 : columns.front().size();
    for (const auto& c : columns) {
        if (c.size() != rows) Fail("WriteTable: columns have different lengths");
    }

    Check(catalog.CreateTable(name, schema), "CreateTable " + name);

    size_t segments = 0;
    uint64_t next_id = 1;
    for (size_t begin = 0; begin < rows; begin += rows_per_segment) {
        const size_t end = std::min(rows, begin + rows_per_segment);
        const uint64_t id = next_id++;

        FileSegmentWriter writer(catalog.SegmentPath(name, id), schema, id);

        // Appended in kBatchSize chunks rather than one giant batch, because that
        // is the shape the writer is written against and a single batch of a whole
        // segment would exercise a path nothing else uses.
        for (size_t at = begin; at < end; at += kBatchSize) {
            const size_t stop = std::min(end, at + kBatchSize);
            Batch batch(schema);
            for (size_t c = 0; c < columns.size(); ++c) {
                Vector& dst = batch.column(c);
                dst.Reserve(stop - at);
                for (size_t r = at; r < stop; ++r) AppendRowTo(dst, columns[c], r);
            }
            batch.SetRowCount(stop - at);
            Check(writer.Append(batch), "segment Append");
        }

        SegmentMeta meta = Take(writer.Finish(), "segment Finish");
        Check(catalog.AddSegment(name, std::move(meta)), "AddSegment");
        ++segments;
    }

    Check(catalog.Save(), "catalog Save");
    return segments;
}

// --- byte accounting -----------------------------------------------------

// ReadCounters is shared between a scan's readers and the benchmark that built
// them, so bytes survive the reader's destruction.
struct ReadCounters {
    uint64_t bytes_read = 0;
    uint64_t readers_opened = 0;

    void Reset() {
        bytes_read = 0;
        readers_opened = 0;
    }
};

// CountingSegmentReader delegates everything to a FileSegmentReader and reports
// the bytes it read into a shared counter when it is closed or destroyed.
class CountingSegmentReader final : public SegmentReader {
public:
    CountingSegmentReader(SegmentMeta meta, std::vector<size_t> projection, Schema schema,
                          ReadCounters* counters)
        : inner_(std::move(meta), std::move(projection), std::move(schema)), counters_(counters) {}

    ~CountingSegmentReader() override { Harvest(); }

    Status Open() override {
        ++counters_->readers_opened;
        return inner_.Open();
    }
    Status Next(Batch& out, bool& done) override { return inner_.Next(out, done); }
    Status Close() override {
        // Harvested before the inner close, and only once, so a scan that closes
        // explicitly and then destroys the reader does not double count.
        Harvest();
        return inner_.Close();
    }
    uint64_t bytes_read() const override { return inner_.bytes_read(); }

private:
    void Harvest() {
        if (harvested_) return;
        harvested_ = true;
        counters_->bytes_read += inner_.bytes_read();
    }

    FileSegmentReader inner_;
    ReadCounters* counters_;
    bool harvested_ = false;
};

// CountingFactory builds the ReaderFactory a SequentialScanOperator needs, wiring
// every reader it creates to one counter.
inline SequentialScanOperator::ReaderFactory CountingFactory(const Schema& table_schema,
                                                             ReadCounters* counters) {
    return [table_schema, counters](
               const SegmentMeta& segment,
               const std::vector<size_t>& projection) -> Result<std::unique_ptr<SegmentReader>> {
        return std::unique_ptr<SegmentReader>(
            new CountingSegmentReader(segment, projection, table_schema, counters));
    };
}

// DrainResult is what a benchmark wants back from running an operator tree: how
// much came out, and what the operators counted while producing it.
struct DrainResult {
    uint64_t rows = 0;
    uint64_t batches = 0;
    uint64_t segments_scanned = 0;
    uint64_t segments_pruned = 0;
};

// Drain runs an operator to exhaustion, touching every surviving value so that
// nothing downstream of the scan can be optimized away.
inline DrainResult Drain(PhysicalOperator& op) {
    Check(op.Open(), "operator Open");
    DrainResult out;
    Batch batch(op.output_schema());
    int64_t checksum = 0;
    for (;;) {
        bool done = false;
        Check(op.Next(batch, done), "operator Next");
        if (done) break;
        ++out.batches;
        out.rows += batch.selected_count();
        // One cheap read per surviving row of the first column: enough to stop the
        // compiler and the operator from skipping work, cheap enough not to be what
        // the benchmark measures.
        if (batch.column_count() > 0 && batch.column(0).type() == TypeId::kInt64) {
            const int64_t* data = batch.column(0).int64_data();
            for (const uint32_t r : batch.Rows()) checksum += data[r];
        } else {
            checksum += static_cast<int64_t>(batch.selected_count());
        }
    }
    Sink(checksum);
    Check(op.Close(), "operator Close");
    out.segments_scanned = op.stats().segments_scanned;
    out.segments_pruned = op.stats().segments_pruned;
    return out;
}

}  // namespace quarry::bench
