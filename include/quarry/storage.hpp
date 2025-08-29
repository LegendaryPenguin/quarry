#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry {

// Encoding is how a column's values are stored on disk within a segment.
enum class Encoding : uint8_t {
    kPlain = 0,       // packed values, or offsets plus a byte region for varchar
    kDictionary,      // codes into a per-segment dictionary; for low-cardinality strings
    kRunLength,       // (value, run) pairs; for sorted or highly repetitive columns
};

const char* EncodingName(Encoding e);

// ColumnStats are the per-segment summaries that make pruning possible.
//
// min and max are the whole point of the segment layout. A predicate that cannot
// be satisfied anywhere in [min, max] lets the scan skip the segment without
// reading a single value, which turns a selective query from O(table) into
// O(matching segments). null_count lets IS NULL answer the same way.
struct ColumnStats {
    Value min;
    Value max;
    uint64_t null_count = 0;
    uint64_t distinct_estimate = 0;

    // all_null is tracked separately because min and max are meaningless then,
    // and a zone map that compared against a null bound would prune wrongly.
    bool all_null = false;
};

// ColumnChunk is one column's bytes inside one segment.
struct ColumnChunk {
    TypeId type = TypeId::kInvalid;
    Encoding encoding = Encoding::kPlain;

    // Byte range within the segment file.
    uint64_t offset = 0;
    uint64_t length = 0;

    // Uncompressed size, so a reader can size its buffer before decoding.
    uint64_t raw_length = 0;

    ColumnStats stats;
};

// SegmentMeta is everything a scan needs to decide whether to open a segment.
//
// Kept separate from the segment's data and loaded with the catalog, because the
// decision to skip a segment must not require reading it.
struct SegmentMeta {
    uint64_t id = 0;
    uint64_t row_count = 0;
    std::string path;
    std::vector<ColumnChunk> columns;
};

// CanMatch decides whether a predicate could be satisfied by any row in a
// segment, given only its statistics.
//
// Returning true when the answer is actually no is merely slow. Returning false
// when the answer is yes silently drops rows from a result, which is why the
// null handling here is conservative: a segment containing nulls is never pruned
// on a comparison, because SQL comparison with NULL is unknown rather than false.
bool CanMatch(const ColumnStats& stats, CompareOp op, const Value& constant);

// SegmentWriter builds one immutable segment from appended batches.
//
// Segments are immutable once written. That is what lets statistics be computed
// once and trusted forever, and it is why the engine is append-oriented: an
// in-place update would invalidate the min/max that pruning depends on.
class SegmentWriter {
public:
    virtual ~SegmentWriter() = default;

    virtual Status Append(const Batch& batch) = 0;

    // Finish writes the segment and returns its metadata. The writer is spent
    // afterwards.
    virtual Result<SegmentMeta> Finish() = 0;
};

// SegmentReader streams a segment's rows as batches, reading only the columns
// asked for.
//
// projection is a list of column indexes into the table schema. Passing fewer
// columns must actually read fewer bytes — that is the entire reason for a
// columnar layout, and a reader that decodes everything and then discards is a
// row store wearing a costume.
class SegmentReader {
public:
    virtual ~SegmentReader() = default;

    virtual Status Open() = 0;

    // Next fills `out` and sets `done`.
    //
    // The convention, pinned here because a caller that assumes the other one
    // silently drops the last batch: a call that produces rows ALWAYS sets
    // done = false, and exhaustion is reported by a separate later call that
    // produces zero rows and sets done = true. A reader never has to distinguish
    // "the final batch" from "one past the end", and a caller never has to handle
    // rows and done together.
    virtual Status Next(Batch& out, bool& done) = 0;

    virtual Status Close() = 0;

    // bytes_read is what proves projection pushdown works. bench/ asserts on it.
    virtual uint64_t bytes_read() const = 0;
};

// Index is a secondary access path from a key to row positions.
//
// Deliberately minimal: point lookup and range scan. Anything richer would make
// the B+ tree and the LSM prototype harder to compare, and comparing them is the
// reason both exist.
class Index {
public:
    virtual ~Index() = default;

    virtual Status Insert(const Value& key, uint64_t row_id) = 0;

    // Lookup appends every row id stored under key. Appends rather than returns
    // so a caller scanning many keys reuses one vector.
    virtual Status Lookup(const Value& key, std::vector<uint64_t>& out) const = 0;

    // RangeScan appends row ids for keys in [lo, hi]. An absent bound is
    // unbounded on that side.
    virtual Status RangeScan(const std::optional<Value>& lo,
                             const std::optional<Value>& hi,
                             std::vector<uint64_t>& out) const = 0;

    virtual Status Flush() = 0;

    virtual std::string Describe() const = 0;

    // --- counters for the B+ tree versus LSM comparison ---
    struct Stats {
        uint64_t inserts = 0;
        uint64_t lookups = 0;
        uint64_t pages_read = 0;
        uint64_t pages_written = 0;
        uint64_t node_splits = 0;

        // LSM-only; zero on the B+ tree.
        uint64_t compactions = 0;
        uint64_t bloom_rejections = 0;
        uint64_t sstables_consulted = 0;
    };

    virtual Stats stats() const = 0;
};

}  // namespace quarry
