#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry {

// kBatchSize is how many rows move between operators at once.
//
// The number is chosen so that one batch of a fixed-width column stays inside L1
// or L2: 2048 int64 values is 16 KiB, so a filter can read a column, write a
// selection vector, and have the next operator read both without a trip to main
// memory. Larger batches stop fitting; smaller ones pay the per-call overhead
// that vectorization exists to amortize. bench/ measures this directly.
constexpr size_t kBatchSize = 2048;

// NullMask tracks which slots in a vector are SQL NULL.
//
// Stored as a separate bitmap rather than a sentinel value in the data, because
// there is no int64 that is not also a legitimate value, and because a filter on
// a column with no nulls should not pay to check for them. has_nulls() lets the
// kernels take a branch-free path in the common case.
class NullMask {
public:
    void Resize(size_t n);
    void Reset(size_t n);

    bool IsNull(size_t i) const {
        return has_nulls_ && (bits_[i >> 6] >> (i & 63)) & 1ULL;
    }

    void SetNull(size_t i) {
        bits_[i >> 6] |= (1ULL << (i & 63));
        has_nulls_ = true;
    }

    // has_nulls is a fast-path hint: false guarantees no nulls, true only means
    // one was set at some point.
    bool has_nulls() const { return has_nulls_; }

    size_t CountNulls(size_t n) const;

private:
    std::vector<uint64_t> bits_;
    bool has_nulls_ = false;
};

// Vector is one column's worth of values for a single batch.
//
// Fixed-width types live in a packed byte array reinterpreted on access, which
// is what makes a scan a contiguous read. Variable-width types use an offset
// table into one shared byte buffer, so a batch of strings is two allocations
// rather than one per value.
class Vector {
public:
    explicit Vector(TypeId type) : type_(type) {}

    TypeId type() const { return type_; }
    size_t size() const { return size_; }

    void Reserve(size_t n);
    void Clear();

    // --- fixed-width access ---
    // These are the hot accessors. They do no bounds checking in release builds
    // on purpose: the loops that call them are driven by a count the caller owns,
    // and a bounds check per value is a measurable fraction of a filter's cost.
    int64_t* int64_data() { return reinterpret_cast<int64_t*>(data_.data()); }
    const int64_t* int64_data() const { return reinterpret_cast<const int64_t*>(data_.data()); }

    double* double_data() { return reinterpret_cast<double*>(data_.data()); }
    const double* double_data() const { return reinterpret_cast<const double*>(data_.data()); }

    uint8_t* bool_data() { return data_.data(); }
    const uint8_t* bool_data() const { return data_.data(); }

    // --- variable-width access ---
    std::string_view GetString(size_t i) const;
    void AppendString(std::string_view s);

    // --- appends for fixed-width, used by scans and by test fixtures ---
    void AppendInt64(int64_t v);
    void AppendDouble(double v);
    void AppendBoolean(bool v);
    void AppendNull();

    // Value-level access for the API edge and for error messages. Not for use
    // inside execution kernels; it allocates for varchar.
    Value GetValue(size_t i) const;

    NullMask& nulls() { return nulls_; }
    const NullMask& nulls() const { return nulls_; }

    void SetSize(size_t n) { size_ = n; }

private:
    TypeId type_;
    size_t size_ = 0;

    // Fixed-width payload, or the string byte region for varchar.
    std::vector<uint8_t> data_;

    // Offsets into data_ for varchar: offsets_[i]..offsets_[i+1] is value i, so
    // the array holds size()+1 entries and a length is a subtraction rather than
    // a stored field.
    std::vector<uint32_t> offsets_;

    NullMask nulls_;
};

// Batch is a horizontal slice of a result set: one Vector per output column plus
// a selection vector naming the rows still live.
//
// The selection vector is the reason filters are cheap. A filter does not move
// or copy column data; it writes the indexes that passed into selection_, and
// every downstream operator iterates that instead of the full range. A chain of
// three filters therefore touches the column data once each and never memcpys a
// row. The cost is that every kernel must respect selection_ — reading a vector
// densely after a filter has run is the single easiest way to produce a wrong
// answer in this design, so Rows() exists to make the correct iteration obvious.
class Batch {
public:
    Batch() = default;
    explicit Batch(const Schema& schema);

    const Schema& schema() const { return schema_; }

    size_t column_count() const { return columns_.size(); }
    Vector& column(size_t i) { return *columns_[i]; }
    const Vector& column(size_t i) const { return *columns_[i]; }

    // row_count is the number of rows PHYSICALLY present in the vectors,
    // regardless of how many are selected.
    size_t row_count() const { return row_count_; }
    void SetRowCount(size_t n);

    // selected_count is the number of live rows. Equal to row_count when no
    // filter has run.
    size_t selected_count() const {
        return has_selection_ ? selection_.size() : row_count_;
    }

    bool has_selection() const { return has_selection_; }

    // Rows returns the indexes a kernel must visit, in order. When no filter has
    // run this is materialized lazily as 0..row_count-1, so the common case does
    // not pay for a selection vector it does not need.
    const std::vector<uint32_t>& Rows() const;

    // SetSelection installs the surviving row indexes. Must be a subset of
    // 0..row_count-1 and must be ascending, because the aggregate and sort
    // operators rely on encountering rows in physical order.
    void SetSelection(std::vector<uint32_t> rows);
    void ClearSelection();

    // Reset empties the batch for reuse. Reusing a batch across loop iterations
    // rather than allocating a fresh one is worth a measurable amount on a long
    // scan; bench/ quantifies it.
    void Reset();

    void AddColumn(const Column& col);

private:
    Schema schema_;
    std::vector<std::unique_ptr<Vector>> columns_;
    size_t row_count_ = 0;

    std::vector<uint32_t> selection_;
    bool has_selection_ = false;

    // Lazily filled with 0..row_count-1 when Rows() is called with no selection.
    mutable std::vector<uint32_t> identity_;
};

}  // namespace quarry
