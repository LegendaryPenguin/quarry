#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "quarry/buffer.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"
#include "quarry/types.hpp"

namespace quarry {

// BTree is a disk-backed B+ tree over pages owned by a BufferPool.
//
// Nodes live only in pages. Every operation is a sequence of fetch, copy out,
// unpin, so no Page* is ever held across a call that could evict — with a pool
// smaller than the tree (the configuration bench/ uses) holding one would be a
// use-after-free the moment a descent touched a second level.
//
// Duplicate keys are ordinary: a secondary index on a non-unique column is the
// common case. They are handled by making the ordering key the pair
// (user key, row id). Composite keys are unique, so the tree itself is a plain
// unique-key B+ tree and a run of equal user keys may straddle a leaf boundary
// without any special case in split or descent.
class BTree final : public Index {
public:
    // Varchar keys are length-prefixed inside the page, and bounded so that a
    // node always has room for at least two entries and therefore can always
    // split. Without a bound, one pathological key could make a node unsplittable.
    static constexpr size_t kMaxKeyLength = 512;

    // pool must already be Open(). The tree does not own it: bench/ deliberately
    // runs one undersized pool to observe eviction behaviour.
    BTree(BufferPool* pool, TypeId key_type);

    // Open binds to the file behind the pool, creating the meta page and an empty
    // root leaf if the file is new, and otherwise adopting what is already there.
    // Separate from the constructor because it does I/O and I/O fails.
    Status Open();

    Status Insert(const Value& key, uint64_t row_id) override;
    Status Lookup(const Value& key, std::vector<uint64_t>& out) const override;
    Status RangeScan(const std::optional<Value>& lo,
                     const std::optional<Value>& hi,
                     std::vector<uint64_t>& out) const override;
    Status Flush() override;
    std::string Describe() const override;
    // Bytes are derived rather than counted separately: this tree only ever
    // transfers whole kPageSize pages, so pages * kPageSize is exact rather than
    // an estimate. An LSM cannot do this, which is why Stats carries both.
    Stats stats() const override {
        Stats s = stats_;
        s.bytes_read = s.pages_read * kPageSize;
        s.bytes_written = s.pages_written * kPageSize;
        s.logical_bytes = logical_bytes_;
        return s;
    }

    // Levels from root to leaf; 1 is a tree that is a single leaf. Exposed because
    // "did this actually split" is otherwise invisible from outside.
    uint32_t height() const { return height_; }

private:
    // Entry is one ordering key. Only the field matching key_type_ is meaningful;
    // a variant would cost an allocation per comparison in the inner loop of every
    // descent, and this struct is copied far more often than it is stored.
    struct Entry {
        int64_t i64 = 0;
        double f64 = 0.0;
        std::string str;
        uint64_t row_id = 0;
    };

    struct Node {
        bool leaf = true;
        std::vector<Entry> entries;
        // Leaf: unused. Internal: entries.size() + 1 child ids, where child i
        // holds composite keys in [entries[i-1], entries[i]).
        std::vector<PageId> children;
        // Leaf only: the right sibling, so a range scan walks leaves directly
        // instead of re-descending from the root for every step.
        PageId next = kInvalidPage;
    };

    struct Split {
        bool happened = false;
        Entry separator;
        PageId right = kInvalidPage;
    };

    bool Less(const Entry& a, const Entry& b) const;
    Status MakeEntry(const Value& key, uint64_t row_id, Entry& out) const;

    size_t EntrySize(const Entry& e) const;
    size_t NodeSize(const Node& n) const;

    void SerializeNode(const Node& n, uint8_t* page) const;
    Status DeserializeNode(const uint8_t* page, Node& out) const;

    Status ReadNode(PageId id, Node& out) const;
    Status WriteNode(PageId id, const Node& n);
    Status AllocNode(const Node& n, PageId& id);

    Status WriteMeta();

    Status InsertInto(PageId id, const Entry& e, Split& split);
    Status SplitNode(Node& left, Node& right, Entry& separator) const;

    // Finds the leaf that holds the first composite key >= target, or the leftmost
    // leaf when target is absent.
    Status FindLeaf(const Entry* target, PageId& leaf) const;

    // One scan engine behind both Lookup and RangeScan: walk leaves from the
    // start point, emitting row ids until an entry past the upper bound. Both
    // bounds are composite and inclusive; an absent bound is open.
    Status Scan(const Entry* lo, const Entry* hi, std::vector<uint64_t>& out) const;

    BufferPool* pool_;
    TypeId key_type_;
    PageId root_ = kInvalidPage;
    uint32_t height_ = 0;

    // Mutable because Lookup and RangeScan are const by contract yet still read
    // pages, and a counter that only counted writes would be useless for the
    // read-amplification comparison against the LSM.
    mutable Stats stats_;

    // Sum of stored entry sizes, so bytes_written / logical_bytes is the same
    // write-amplification ratio the LSM reports and the two are comparable.
    uint64_t logical_bytes_ = 0;
};

}  // namespace quarry
