#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/status.hpp"
#include "quarry/storage.hpp"
#include "quarry/types.hpp"

#include "memtable.hpp"
#include "sstable.hpp"

namespace quarry::lsm {

struct LsmOptions {
    // Deliberately small by default. The point of this index is to be measured
    // against the B+ tree on the same data, and a memtable large enough to hold
    // a test's whole key set would never flush, never compact, and report a write
    // amplification of 1.0 — a true number about a configuration nobody runs.
    size_t memtable_bytes = 64 * 1024;

    // Tables per level before they are merged into the next one. Four is the
    // usual size-tiered setting: it keeps read amplification bounded at roughly
    // four tables per level while merging rarely enough that each byte is
    // rewritten about log_4(n) times.
    size_t level_fanout = 4;

    double bloom_fpr = 0.01;
};

// LsmIndex is the log-structured counterpart to the B+ tree.
//
// Writes go to a memtable and are never updated in place; when the memtable
// outgrows its budget it is written out whole as an immutable level-0 SSTable.
// Reads therefore have to consult several tables, and compaction has to rewrite
// data that was already durable. Those two costs — read amplification and write
// amplification — are exactly what this index exists to measure against the B+
// tree, so every one of them is counted rather than hidden.
//
// Compaction is size-tiered, not leveled. Size-tiered merges a whole tier of
// similarly sized tables into one table a tier down, so each byte is rewritten
// about once per tier, O(log n) times in total. Leveled compaction would rewrite
// an overlapping slice of the next level on nearly every merge and cost several
// times the write amplification, in exchange for a read path that touches one
// table per level. Since the experiment's headline is the write-amplification
// gap against an in-place B+ tree, the variant that makes the LSM's write path
// look its best is the honest one to compare: a leveled result would flatter the
// B+ tree for free. The read amplification size-tiering gives up is itself worth
// measuring, and sstables_consulted reports it.
//
// Deletes are outside the Index contract, so there are no tombstones: a key seen
// at several levels contributes the union of its row ids, and a merge never has
// to decide that an older value is dead.
class LsmIndex final : public Index {
public:
    // Open adopts a directory. An existing manifest is replayed so the index is
    // usable over a directory a previous instance wrote.
    static Result<std::unique_ptr<LsmIndex>> Open(std::string dir, LsmOptions options = {});

    ~LsmIndex() override;

    LsmIndex(const LsmIndex&) = delete;
    LsmIndex& operator=(const LsmIndex&) = delete;

    Status Insert(const Value& key, uint64_t row_id) override;
    Status Lookup(const Value& key, std::vector<uint64_t>& out) const override;
    Status RangeScan(const std::optional<Value>& lo, const std::optional<Value>& hi,
                     std::vector<uint64_t>& out) const override;
    Status Flush() override;
    std::string Describe() const override;
    Stats stats() const override;

    // --- write amplification, which Index::Stats has no field for ---
    //
    // pages_written is in Stats, but pages are a lossy denominator: a 300-byte
    // flush and a 4000-byte flush both cost one page, so a ratio of pages would
    // overstate small writes. These report the byte figures the experiment wants.
    uint64_t bytes_written() const { return io_->bytes_written; }
    uint64_t bytes_read() const { return io_->bytes_read; }

    // LogicalBytes is the size of the data the caller handed over: for each
    // inserted pair, the encoded key plus the eight bytes of its row id. It is
    // the denominator of write amplification, so it counts what was inserted and
    // not what any particular layout made of it.
    uint64_t logical_bytes() const { return logical_bytes_; }

    double WriteAmplification() const {
        return logical_bytes_ == 0 ? 0.0
                                   : static_cast<double>(io_->bytes_written) /
                                         static_cast<double>(logical_bytes_);
    }

    size_t level_count() const { return levels_.size(); }
    size_t tables_at_level(size_t level) const {
        return level < levels_.size() ? levels_[level].size() : 0;
    }

private:
    LsmIndex() = default;

    struct TableRef {
        std::unique_ptr<SsTable> table;
    };

    Status FlushMemtable();
    Status MaybeCompact();
    Status CompactLevel(size_t level);
    Status WriteManifest() const;
    Status LoadManifest();
    std::string TablePath(uint64_t file_number) const;

    std::string dir_;
    LsmOptions opts_;
    Memtable mem_;

    // levels_[0] holds flushed memtables, newest first; deeper levels hold
    // compaction output. Newest-first matters for the read order even without
    // tombstones, because it is the order a reader with overwrite semantics
    // would need and the counters should reflect the real access pattern.
    std::vector<std::vector<TableRef>> levels_;

    uint64_t next_file_ = 1;
    uint64_t logical_bytes_ = 0;

    std::shared_ptr<IoCounters> io_ = std::make_shared<IoCounters>();

    // Counters a const Lookup must still be able to move.
    mutable uint64_t inserts_ = 0;
    mutable uint64_t lookups_ = 0;
    mutable uint64_t compactions_ = 0;
    mutable uint64_t bloom_rejections_ = 0;
    mutable uint64_t sstables_consulted_ = 0;
};

}  // namespace quarry::lsm
