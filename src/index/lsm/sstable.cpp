#include "sstable.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "lsm_codec.hpp"

namespace quarry::lsm {
namespace {

// The footer is fixed width so it can be found from the end of the file without
// first knowing anything about the file's contents.
constexpr uint64_t kFooterFields = 9;
constexpr uint64_t kFooterSize = (kFooterFields + 2) * sizeof(uint64_t);  // + crc + magic
constexpr uint64_t kMagic = 0x51554152525931ull;  // "QUARRY1"

}  // namespace

// --------------------------------------------------------------------------
// BloomFilter
// --------------------------------------------------------------------------

BloomFilter BloomFilter::Create(uint64_t expected_entries, double target_fpr) {
    BloomFilter f;
    if (target_fpr <= 0.0 || target_fpr >= 1.0) target_fpr = 0.01;

    // An empty table still gets a filter, so that MayContain has a defined
    // answer and the reader needs no special case for a zero-bit array.
    const double n = static_cast<double>(std::max<uint64_t>(expected_entries, 1));
    const double ln2 = 0.6931471805599453;
    double bits_per_key = -std::log(target_fpr) / (ln2 * ln2);
    bits_per_key = std::min(bits_per_key, 40.0);  // beyond this the gain is noise

    f.num_bits_ = static_cast<uint64_t>(std::ceil(n * bits_per_key));
    f.num_bits_ = std::max<uint64_t>(f.num_bits_, 64);
    f.num_probes_ = static_cast<uint32_t>(std::lround(bits_per_key * ln2));
    f.num_probes_ = std::clamp<uint32_t>(f.num_probes_, 1, 30);
    f.bits_.assign((f.num_bits_ + 7) / 8, '\0');
    return f;
}

BloomFilter BloomFilter::FromBytes(std::string bits, uint32_t num_probes, uint64_t num_bits) {
    BloomFilter f;
    f.bits_ = std::move(bits);
    f.num_bits_ = num_bits;
    f.num_probes_ = num_probes;
    return f;
}

void BloomFilter::Add(uint64_t hash) {
    uint64_t h1 = hash;
    // Forcing the stride odd makes it coprime with any power-of-two-sized walk
    // and, more importantly, keeps it non-zero: a zero stride would set the same
    // bit k times and collapse the filter to a single probe.
    uint64_t h2 = (hash >> 33) | 1ull;
    for (uint32_t i = 0; i < num_probes_; ++i) {
        uint64_t bit = h1 % num_bits_;
        bits_[bit / 8] = static_cast<char>(static_cast<unsigned char>(bits_[bit / 8]) |
                                           (1u << (bit % 8)));
        h1 += h2;
    }
}

bool BloomFilter::MayContain(uint64_t hash) const {
    if (num_bits_ == 0 || bits_.empty()) return true;  // no filter means no skipping
    uint64_t h1 = hash;
    uint64_t h2 = (hash >> 33) | 1ull;
    for (uint32_t i = 0; i < num_probes_; ++i) {
        uint64_t bit = h1 % num_bits_;
        if ((static_cast<unsigned char>(bits_[bit / 8]) & (1u << (bit % 8))) == 0) return false;
        h1 += h2;
    }
    return true;
}

// --------------------------------------------------------------------------
// SsTableBuilder
// --------------------------------------------------------------------------

SsTableBuilder::SsTableBuilder(std::string path, uint64_t expected_entries, double target_fpr,
                               std::shared_ptr<IoCounters> io)
    : path_(std::move(path)),
      io_(std::move(io)),
      bloom_(BloomFilter::Create(expected_entries, target_fpr)) {}

Status SsTableBuilder::Open() {
    out_.open(path_, std::ios::binary | std::ios::trunc);
    if (!out_) return Status::IoError("cannot create sstable " + path_);
    return Status::Ok();
}

Status SsTableBuilder::WriteRaw(const std::string& bytes) {
    out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out_) return Status::IoError("short write to " + path_);
    io_->bytes_written += bytes.size();
    // Pages are charged once, in Finish, from the finished file's length. Charging
    // PagesFor() per write call would round every chunk up to a whole page and
    // report an order of magnitude more pages than the file occupies — which
    // would make the LSM's write amplification look far worse than it is. A table
    // is appended strictly sequentially, so ceil(file_size / page) is exactly the
    // number of distinct pages the write touched.
    return Status::Ok();
}

Status SsTableBuilder::Add(const std::string& key, const std::vector<uint64_t>& row_ids) {
    if (finished_) return Status::Internal("Add after Finish on " + path_);
    if (entry_count_ > 0 && key <= last_key_) {
        return Status::Internal("sstable keys must be strictly increasing");
    }

    if (chunk_entries_ == 0) chunk_first_key_ = key;

    PutVarint64(chunk_, key.size());
    chunk_.append(key);
    PutVarint64(chunk_, row_ids.size());
    for (uint64_t id : row_ids) PutVarint64(chunk_, id);

    last_key_ = key;
    ++entry_count_;
    row_id_count_ += row_ids.size();
    ++chunk_entries_;
    bloom_.Add(Hash64(key));

    if (chunk_entries_ >= kChunkEntries) return FlushChunk();
    return Status::Ok();
}

Status SsTableBuilder::FlushChunk() {
    if (chunk_entries_ == 0) return Status::Ok();

    index_.push_back({chunk_first_key_, offset_, chunk_.size()});

    std::string framed;
    framed.reserve(chunk_.size() + 8);
    PutFixed32(framed, static_cast<uint32_t>(chunk_.size()));
    framed.append(chunk_);
    PutFixed32(framed, Crc32(chunk_.data(), chunk_.size()));
    QUARRY_TRY(WriteRaw(framed));

    offset_ += framed.size();
    chunk_.clear();
    chunk_entries_ = 0;
    return Status::Ok();
}

Result<SsTableBuilder::Built> SsTableBuilder::Finish() {
    if (finished_) return Status::Internal("Finish called twice on " + path_);
    QUARRY_TRY(FlushChunk());
    finished_ = true;

    const uint64_t data_size = offset_;

    std::string index_block;
    for (const auto& e : index_) {
        PutVarint64(index_block, e.first_key.size());
        index_block.append(e.first_key);
        PutVarint64(index_block, e.offset);
        PutVarint64(index_block, e.payload_len);
    }
    const uint64_t index_offset = offset_;
    QUARRY_TRY(WriteRaw(index_block));
    offset_ += index_block.size();

    std::string bloom_block;
    PutVarint64(bloom_block, bloom_.num_bits());
    PutVarint64(bloom_block, bloom_.num_probes());
    bloom_block.append(bloom_.bits());
    const uint64_t bloom_offset = offset_;
    QUARRY_TRY(WriteRaw(bloom_block));
    offset_ += bloom_block.size();

    std::string footer;
    PutFixed64(footer, data_size);
    PutFixed64(footer, index_offset);
    PutFixed64(footer, index_block.size());
    PutFixed64(footer, bloom_offset);
    PutFixed64(footer, bloom_block.size());
    PutFixed64(footer, entry_count_);
    PutFixed64(footer, row_id_count_);
    PutFixed64(footer, Crc32(index_block.data(), index_block.size()));
    PutFixed64(footer, Crc32(bloom_block.data(), bloom_block.size()));
    PutFixed64(footer, Crc32(footer.data(), footer.size()));
    PutFixed64(footer, kMagic);
    QUARRY_TRY(WriteRaw(footer));
    offset_ += footer.size();

    out_.flush();
    if (!out_) return Status::IoError("flush failed for " + path_);
    out_.close();

    io_->pages_written += PagesFor(offset_);

    return Built{entry_count_, row_id_count_, offset_};
}

// --------------------------------------------------------------------------
// SsTable
// --------------------------------------------------------------------------

Result<std::unique_ptr<SsTable>> SsTable::Open(std::string path, std::shared_ptr<IoCounters> io) {
    auto table = std::unique_ptr<SsTable>(new SsTable());
    table->path_ = std::move(path);
    table->io_ = std::move(io);

    table->file_.open(table->path_, std::ios::binary);
    if (!table->file_) return Status::IoError("cannot open sstable " + table->path_);

    table->file_.seekg(0, std::ios::end);
    const auto end = table->file_.tellg();
    if (end < 0) return Status::IoError("cannot size " + table->path_);
    table->file_size_ = static_cast<uint64_t>(end);

    if (table->file_size_ < kFooterSize) {
        return Status::Corruption(table->path_ + ": shorter than a footer");
    }

    std::string footer;
    QUARRY_TRY(table->ReadAt(table->file_size_ - kFooterSize, kFooterSize, footer));

    const uint64_t magic = DecodeFixed64(footer.data() + (kFooterFields + 1) * 8);
    if (magic != kMagic) return Status::Corruption(table->path_ + ": bad footer magic");

    const uint64_t footer_crc = DecodeFixed64(footer.data() + kFooterFields * 8);
    if (footer_crc != Crc32(footer.data(), kFooterFields * 8)) {
        return Status::Corruption(table->path_ + ": footer checksum mismatch");
    }

    const uint64_t data_size = DecodeFixed64(footer.data() + 0 * 8);
    const uint64_t index_offset = DecodeFixed64(footer.data() + 1 * 8);
    const uint64_t index_size = DecodeFixed64(footer.data() + 2 * 8);
    const uint64_t bloom_offset = DecodeFixed64(footer.data() + 3 * 8);
    const uint64_t bloom_size = DecodeFixed64(footer.data() + 4 * 8);
    table->entry_count_ = DecodeFixed64(footer.data() + 5 * 8);
    table->row_id_count_ = DecodeFixed64(footer.data() + 6 * 8);
    const uint64_t index_crc = DecodeFixed64(footer.data() + 7 * 8);
    const uint64_t bloom_crc = DecodeFixed64(footer.data() + 8 * 8);

    // Every offset is checked against the real file length before it is used as a
    // seek target, so a corrupted footer cannot steer a read off the end of the
    // file or make the reader allocate an absurd buffer.
    const uint64_t limit = table->file_size_ - kFooterSize;
    if (index_offset != data_size || index_offset > limit || index_size > limit - index_offset ||
        bloom_offset != index_offset + index_size || bloom_offset > limit ||
        bloom_size > limit - bloom_offset || bloom_offset + bloom_size != limit) {
        return Status::Corruption(table->path_ + ": footer offsets inconsistent");
    }

    std::string index_block;
    QUARRY_TRY(table->ReadAt(index_offset, index_size, index_block));
    if (Crc32(index_block.data(), index_block.size()) != index_crc) {
        return Status::Corruption(table->path_ + ": index checksum mismatch");
    }

    std::string_view cursor(index_block);
    while (!cursor.empty()) {
        std::string_view key;
        uint64_t offset = 0, payload_len = 0;
        if (!GetLengthPrefixed(cursor, key) || !GetVarint64(cursor, offset) ||
            !GetVarint64(cursor, payload_len)) {
            return Status::Corruption(table->path_ + ": truncated index entry");
        }
        if (offset + 8 > data_size || payload_len > data_size - offset - 8) {
            return Status::Corruption(table->path_ + ": index entry points outside data");
        }
        table->index_.push_back({std::string(key), offset, payload_len});
    }

    std::string bloom_block;
    QUARRY_TRY(table->ReadAt(bloom_offset, bloom_size, bloom_block));
    if (Crc32(bloom_block.data(), bloom_block.size()) != bloom_crc) {
        return Status::Corruption(table->path_ + ": bloom checksum mismatch");
    }
    {
        std::string_view cur(bloom_block);
        uint64_t num_bits = 0, num_probes = 0;
        if (!GetVarint64(cur, num_bits) || !GetVarint64(cur, num_probes)) {
            return Status::Corruption(table->path_ + ": truncated bloom header");
        }
        if ((num_bits + 7) / 8 != cur.size()) {
            return Status::Corruption(table->path_ + ": bloom size disagrees with bit count");
        }
        table->bloom_ = BloomFilter::FromBytes(std::string(cur),
                                               static_cast<uint32_t>(num_probes), num_bits);
    }

    return table;
}

Status SsTable::ReadAt(uint64_t offset, uint64_t len, std::string& out) const {
    out.assign(len, '\0');
    if (len == 0) return Status::Ok();

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_) return Status::IoError("seek failed on " + path_);
    file_.read(out.data(), static_cast<std::streamsize>(len));
    if (file_.gcount() != static_cast<std::streamsize>(len)) {
        return Status::Corruption(path_ + ": short read, file truncated");
    }

    io_->bytes_read += len;
    io_->pages_read += PagesFor(len);
    return Status::Ok();
}

size_t SsTable::ChunkFor(const std::string& key) const {
    // The chunk that can hold key is the last one whose first key does not
    // exceed it; upper_bound then stepping back finds it in one comparison pass.
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
                               [](const std::string& k, const IndexEntry& e) {
                                   return k < e.first_key;
                               });
    if (it == index_.begin()) return index_.size();
    return static_cast<size_t>(std::distance(index_.begin(), it) - 1);
}

Status SsTable::ReadChunk(size_t chunk, std::vector<SsTableEntry>& out) const {
    out.clear();
    if (chunk >= index_.size()) return Status::Ok();

    const IndexEntry& e = index_[chunk];
    std::string framed;
    QUARRY_TRY(ReadAt(e.offset, e.payload_len + 8, framed));

    const uint32_t declared = DecodeFixed32(framed.data());
    if (declared != e.payload_len) {
        return Status::Corruption(path_ + ": chunk length disagrees with index");
    }
    const char* payload = framed.data() + 4;
    const uint32_t stored_crc = DecodeFixed32(framed.data() + 4 + e.payload_len);
    if (Crc32(payload, e.payload_len) != stored_crc) {
        return Status::Corruption(path_ + ": chunk checksum mismatch");
    }

    std::string_view cursor(payload, e.payload_len);
    while (!cursor.empty()) {
        std::string_view key;
        uint64_t count = 0;
        if (!GetLengthPrefixed(cursor, key) || !GetVarint64(cursor, count)) {
            return Status::Corruption(path_ + ": truncated chunk entry");
        }
        SsTableEntry entry;
        entry.key.assign(key);
        entry.row_ids.reserve(count);
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t id = 0;
            if (!GetVarint64(cursor, id)) {
                return Status::Corruption(path_ + ": truncated row id list");
            }
            entry.row_ids.push_back(id);
        }
        out.push_back(std::move(entry));
    }
    return Status::Ok();
}

bool SsTable::MayContain(const std::string& key) const {
    return bloom_.MayContain(Hash64(key));
}

Status SsTable::Lookup(const std::string& key, std::vector<uint64_t>& out, bool& found) const {
    found = false;
    const size_t chunk = ChunkFor(key);
    if (chunk >= index_.size()) return Status::Ok();

    std::vector<SsTableEntry> entries;
    QUARRY_TRY(ReadChunk(chunk, entries));
    for (const auto& e : entries) {
        if (e.key == key) {
            out.insert(out.end(), e.row_ids.begin(), e.row_ids.end());
            found = true;
            return Status::Ok();
        }
        if (e.key > key) break;  // entries are sorted, so the key is absent
    }
    return Status::Ok();
}

void SsTable::Iterator::LoadChunk(size_t chunk) {
    chunk_ = chunk;
    pos_ = 0;
    entries_.clear();
    valid_ = false;
    if (chunk >= table_->index_.size()) return;

    status_ = table_->ReadChunk(chunk, entries_);
    if (!status_.ok()) return;
    valid_ = !entries_.empty();
}

void SsTable::Iterator::SeekToFirst() {
    status_ = Status::Ok();
    LoadChunk(0);
}

void SsTable::Iterator::Seek(const std::string& key) {
    status_ = Status::Ok();
    size_t chunk = table_->ChunkFor(key);
    // A key below the whole table still has to start from the first chunk; the
    // scan wants the first entry >= key, not "nothing".
    if (chunk >= table_->index_.size()) chunk = 0;

    LoadChunk(chunk);
    while (valid_ && entries_[pos_].key < key) Next();
}

void SsTable::Iterator::Next() {
    if (!valid_) return;
    if (++pos_ < entries_.size()) return;
    LoadChunk(chunk_ + 1);
}

}  // namespace quarry::lsm
