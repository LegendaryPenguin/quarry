#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace quarry::lsm {

// Memtable is the LSM's write buffer: every insert lands here, and the table is
// flushed to a sorted SSTable once it outgrows a threshold.
//
// std::map rather than a skiplist. The two are equivalent for what this layer
// owes its caller — O(log n) insert and in-order iteration — so the choice comes
// down to what the extra machinery of a skiplist would buy. Its real advantage
// is concurrent, lock-free insertion against live readers, and this index is
// single-threaded (the Index interface has no concurrency contract and the
// engine drives queries on one thread), so that advantage is unpaid-for
// complexity. std::map also makes footprint accounting tractable: a node's cost
// is a known constant plus the key bytes, whereas a skiplist node's cost depends
// on a randomly chosen tower height.
//
// Keys are the encoded byte strings from lsm_codec, not Values, so the ordering
// here is the same memcmp ordering the SSTables and the merge use.
class Memtable {
public:
    // Row ids under one key are kept sorted and deduplicated. Deduplication is
    // safe because a row id identifies a row: inserting the same (key, row_id)
    // pair twice describes the same fact, so collapsing it keeps Insert
    // idempotent instead of letting a replayed insert inflate a lookup result.
    using Table = std::map<std::string, std::vector<uint64_t>>;

    void Insert(const std::string& key, uint64_t row_id);

    // Lookup appends to out and reports whether the key was present at all,
    // which the caller needs in order to distinguish "absent" from "present with
    // nothing new to add".
    bool Lookup(const std::string& key, std::vector<uint64_t>& out) const;

    // ApproximateBytes is what the owner polls to decide when to flush. It is
    // deliberately an estimate of heap footprint, not of serialized size: the
    // threshold exists to bound memory, and a red-black tree node costs far more
    // than the bytes it will occupy on disk. Under-counting it would let the
    // memtable grow several times past the configured budget.
    size_t ApproximateBytes() const { return approx_bytes_; }

    size_t KeyCount() const { return table_.size(); }
    size_t RowIdCount() const { return row_id_count_; }
    bool empty() const { return table_.empty(); }

    void Clear();

    const Table& table() const { return table_; }

private:
    // Per-node overhead of a red-black tree node: three pointers, the colour
    // word, and the empty vector's own three pointers, rounded to a plausible
    // allocator bucket. Approximate by construction — see ApproximateBytes.
    static constexpr size_t kNodeOverhead = 64;

    Table table_;
    size_t approx_bytes_ = 0;
    size_t row_id_count_ = 0;
};

}  // namespace quarry::lsm
