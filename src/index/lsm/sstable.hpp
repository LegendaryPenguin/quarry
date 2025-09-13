#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "quarry/status.hpp"

namespace quarry::lsm {

// IoCounters is shared by every component that touches the disk, so that the
// Index::Stats the comparison is built on reflects all of it — including the
// bytes a compaction rewrote. The counters live in one object held by shared_ptr
// because an SSTable outlives neither more nor less than the index that opened
// it, and a compaction deletes tables while their counts must survive.
struct IoCounters {
    uint64_t pages_read = 0;
    uint64_t pages_written = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
};

// Pages are the unit the B+ tree comparison is denominated in. 4 KiB matches the
// page size the buffer pool uses, so "pages read" means the same thing on both
// sides of the comparison even though the LSM does its own file IO.
constexpr uint64_t kPageSize = 4096;

inline uint64_t PagesFor(uint64_t bytes) { return (bytes + kPageSize - 1) / kPageSize; }

// BloomFilter answers "is this key definitely absent from the table" so a point
// lookup can skip opening a table it cannot contain. False positives cost a
// wasted chunk read; false negatives would be silent data loss, so the
// implementation never clears a bit and never consults fewer bits than it set.
//
// k probes are derived from one 64-bit hash by the two-hash technique:
// g_i(x) = h1 + i*h2, with h2 forced odd so that repeatedly adding it walks the
// whole bit array rather than a subgroup of it.
class BloomFilter {
public:
    // Sized from an expected entry count and a target false-positive rate using
    // the standard optimum: m/n = -ln(p)/(ln 2)^2 bits per key and
    // k = (m/n) * ln 2 hashes. At the 1% target the index asks for, that is
    // 9.6 bits and 6.6 hashes per key, rounded to 10 bits and 7 hashes — about a
    // 0.8% measured rate, for 1.25 bytes of RAM per key.
    static BloomFilter Create(uint64_t expected_entries, double target_fpr);

    // FromBytes rebuilds a filter read off disk. k travels with the bits because
    // a filter probed with the wrong k would produce false negatives.
    static BloomFilter FromBytes(std::string bits, uint32_t num_probes, uint64_t num_bits);

    void Add(uint64_t hash);
    bool MayContain(uint64_t hash) const;

    const std::string& bits() const { return bits_; }
    uint32_t num_probes() const { return num_probes_; }
    uint64_t num_bits() const { return num_bits_; }

private:
    std::string bits_;
    uint64_t num_bits_ = 0;
    uint32_t num_probes_ = 0;
};

// An entry is one key and every row id stored under it. Row ids are sorted and
// unique, which is what lets the merge across levels deduplicate by a linear
// pass instead of a set.
struct SsTableEntry {
    std::string key;
    std::vector<uint64_t> row_ids;
};

// Chunks of 16 entries are the unit of both the sparse index and the checksum.
//
// 16 is chosen so a chunk stays comfortably inside one 4 KiB page for the key
// widths this engine supports (an int64 key with a couple of row ids is well
// under 32 bytes, so a chunk is a few hundred bytes), which means a point lookup
// costs one seek and one page read rather than a scan. It also bounds the index
// at 1/16 of the key bytes, small enough to hold resident for every open table.
// A checksum per chunk rather than per file is what makes corruption detectable
// on a lookup that only ever reads one chunk.
constexpr size_t kChunkEntries = 16;

// SsTableBuilder writes an immutable sorted table. Keys must arrive in
// increasing order; the builder rejects anything else rather than producing a
// file whose sparse index silently lies about where a key is.
class SsTableBuilder {
public:
    struct Built {
        uint64_t entry_count = 0;
        uint64_t row_id_count = 0;
        uint64_t file_size = 0;
    };

    SsTableBuilder(std::string path, uint64_t expected_entries, double target_fpr,
                   std::shared_ptr<IoCounters> io);

    Status Open();
    Status Add(const std::string& key, const std::vector<uint64_t>& row_ids);
    Result<Built> Finish();

private:
    Status FlushChunk();
    Status WriteRaw(const std::string& bytes);

    struct IndexEntry {
        std::string first_key;
        uint64_t offset = 0;
        uint64_t payload_len = 0;
    };

    std::string path_;
    std::shared_ptr<IoCounters> io_;
    std::ofstream out_;
    BloomFilter bloom_;

    std::string chunk_;
    std::string chunk_first_key_;
    size_t chunk_entries_ = 0;

    std::vector<IndexEntry> index_;
    std::string last_key_;
    uint64_t offset_ = 0;
    uint64_t entry_count_ = 0;
    uint64_t row_id_count_ = 0;
    bool finished_ = false;
};

// SsTable reads an immutable table written by SsTableBuilder.
//
// Open loads the sparse index and the Bloom filter and verifies their checksums;
// data chunks are checksummed as they are read. Any mismatch is reported as
// Status::Corruption, never as a short or empty result, because an index that
// answered "no rows" for a damaged table would look exactly like a correct
// answer to the caller.
class SsTable {
public:
    static Result<std::unique_ptr<SsTable>> Open(std::string path,
                                                 std::shared_ptr<IoCounters> io);

    bool MayContain(const std::string& key) const;

    // Lookup appends the row ids under key. found distinguishes a key that is
    // absent from a key present with an empty row set.
    Status Lookup(const std::string& key, std::vector<uint64_t>& out, bool& found) const;

    // Iterator walks entries in key order, loading one chunk at a time so a
    // narrow range scan does not read the whole table.
    class Iterator {
    public:
        explicit Iterator(const SsTable* table) : table_(table) {}

        void SeekToFirst();
        void Seek(const std::string& key);
        void Next();

        bool Valid() const { return valid_; }
        const std::string& key() const { return entries_[pos_].key; }
        const std::vector<uint64_t>& row_ids() const { return entries_[pos_].row_ids; }
        const Status& status() const { return status_; }

    private:
        void LoadChunk(size_t chunk);

        const SsTable* table_;
        std::vector<SsTableEntry> entries_;
        size_t chunk_ = 0;
        size_t pos_ = 0;
        bool valid_ = false;
        Status status_;
    };

    uint64_t entry_count() const { return entry_count_; }
    uint64_t row_id_count() const { return row_id_count_; }
    uint64_t file_size() const { return file_size_; }
    const std::string& path() const { return path_; }

private:
    struct IndexEntry {
        std::string first_key;
        uint64_t offset = 0;
        uint64_t payload_len = 0;
    };

    Status ReadChunk(size_t chunk, std::vector<SsTableEntry>& out) const;
    Status ReadAt(uint64_t offset, uint64_t len, std::string& out) const;

    // ChunkFor returns the only chunk that could hold key, or index_.size() when
    // key sorts below every chunk's first key.
    size_t ChunkFor(const std::string& key) const;

    std::string path_;
    std::shared_ptr<IoCounters> io_;
    mutable std::ifstream file_;
    std::vector<IndexEntry> index_;
    BloomFilter bloom_;
    uint64_t entry_count_ = 0;
    uint64_t row_id_count_ = 0;
    uint64_t file_size_ = 0;
};

}  // namespace quarry::lsm
