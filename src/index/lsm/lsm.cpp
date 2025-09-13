#include "lsm.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <iomanip>
#include <sstream>
#include <utility>

#include "lsm_codec.hpp"

namespace quarry::lsm {
namespace {

constexpr const char* kManifestName = "MANIFEST";
constexpr const char* kManifestTmpName = "MANIFEST.tmp";
constexpr const char* kManifestMagic = "QUARRYLSM1";

// MergeSource is one sorted stream of (key, row ids) entering the k-way merge.
// Both the memtable and an SSTable present themselves this way so the merge
// logic exists once and is shared by RangeScan and compaction — if those two
// disagreed about how overlapping keys combine, a compaction would change the
// answer to a query.
class MergeSource {
public:
    virtual ~MergeSource() = default;
    virtual bool Valid() const = 0;
    virtual const std::string& key() const = 0;
    virtual void AppendRowIds(std::vector<uint64_t>& out) const = 0;
    virtual void Next() = 0;
    virtual Status status() const = 0;
};

class MemtableSource final : public MergeSource {
public:
    MemtableSource(const Memtable::Table& table, const std::string* lower)
        : it_(lower ? table.lower_bound(*lower) : table.begin()), end_(table.end()) {}

    bool Valid() const override { return it_ != end_; }
    const std::string& key() const override { return it_->first; }
    void AppendRowIds(std::vector<uint64_t>& out) const override {
        out.insert(out.end(), it_->second.begin(), it_->second.end());
    }
    void Next() override { ++it_; }
    Status status() const override { return Status::Ok(); }

private:
    Memtable::Table::const_iterator it_;
    Memtable::Table::const_iterator end_;
};

class TableSource final : public MergeSource {
public:
    TableSource(const SsTable* table, const std::string* lower) : iter_(table) {
        if (lower) {
            iter_.Seek(*lower);
        } else {
            iter_.SeekToFirst();
        }
    }

    bool Valid() const override { return iter_.Valid(); }
    const std::string& key() const override { return iter_.key(); }
    void AppendRowIds(std::vector<uint64_t>& out) const override {
        const auto& ids = iter_.row_ids();
        out.insert(out.end(), ids.begin(), ids.end());
    }
    void Next() override { iter_.Next(); }
    Status status() const override { return iter_.status(); }

private:
    SsTable::Iterator iter_;
};

using EmitFn = std::function<Status(const std::string& key, std::vector<uint64_t>& row_ids)>;

// MergeAll walks every source in key order and calls emit once per distinct key
// with the union of that key's row ids.
//
// A binary heap keyed on the current key of each source makes the cost
// O(total_entries * log k) rather than O(total_entries * k); with a dozen tables
// the difference is small, but the heap is also what makes "advance every source
// sitting on the smallest key" a single obvious loop.
Status MergeAll(std::vector<std::unique_ptr<MergeSource>>& sources, const std::string* upper,
                const EmitFn& emit) {
    using HeapItem = std::pair<std::string, size_t>;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;

    for (size_t i = 0; i < sources.size(); ++i) {
        QUARRY_TRY(sources[i]->status());
        if (sources[i]->Valid()) heap.emplace(sources[i]->key(), i);
    }

    std::vector<uint64_t> row_ids;
    while (!heap.empty()) {
        const std::string key = heap.top().first;
        if (upper && key > *upper) break;

        row_ids.clear();
        while (!heap.empty() && heap.top().first == key) {
            const size_t idx = heap.top().second;
            heap.pop();
            sources[idx]->AppendRowIds(row_ids);
            sources[idx]->Next();
            QUARRY_TRY(sources[idx]->status());
            if (sources[idx]->Valid()) heap.emplace(sources[idx]->key(), idx);
        }

        // The same row id can legitimately arrive from several levels: an insert
        // that was flushed and then re-inserted, or a compaction output that
        // overlaps a table still being read. Collapsing duplicates here is what
        // makes a lookup's answer independent of how many times the data moved.
        std::sort(row_ids.begin(), row_ids.end());
        row_ids.erase(std::unique(row_ids.begin(), row_ids.end()), row_ids.end());

        QUARRY_TRY(emit(key, row_ids));
    }
    return Status::Ok();
}

}  // namespace

Result<std::unique_ptr<LsmIndex>> LsmIndex::Open(std::string dir, LsmOptions options) {
    auto index = std::unique_ptr<LsmIndex>(new LsmIndex());
    index->dir_ = std::move(dir);
    index->opts_ = options;
    if (index->opts_.level_fanout < 2) index->opts_.level_fanout = 2;

    std::error_code ec;
    std::filesystem::create_directories(index->dir_, ec);
    if (ec) return Status::IoError("cannot create " + index->dir_ + ": " + ec.message());

    QUARRY_TRY(index->LoadManifest());
    return index;
}

LsmIndex::~LsmIndex() {
    // A memtable that was never flushed is the one way data can vanish across a
    // reopen, and the Index interface has no Close for the caller to be told
    // about. Flushing from the destructor means destruction is the close, so
    // "write, destroy, reopen" finds everything. Failures cannot be reported
    // from here, which is why Flush is also public and the tests call it.
    if (!mem_.empty()) {
        Status s = FlushMemtable();
        if (s.ok()) {
            (void)WriteManifest();
        }
    }
}

std::string LsmIndex::TablePath(uint64_t file_number) const {
    std::ostringstream os;
    os << dir_ << "/sst-" << std::setfill('0') << std::setw(6) << file_number << ".sst";
    return os.str();
}

Status LsmIndex::Insert(const Value& key, uint64_t row_id) {
    if (key.is_null()) {
        // A NULL key has no position in an ordered index: it compares false
        // against everything, so it could never be found again by Lookup or
        // bounded by a range. Rejecting it beats storing an unreachable row.
        return Status::InvalidArgument("lsm index key must not be NULL");
    }

    std::string encoded = EncodeKey(key);
    mem_.Insert(encoded, row_id);
    ++inserts_;
    logical_bytes_ += encoded.size() + sizeof(uint64_t);

    if (mem_.ApproximateBytes() >= opts_.memtable_bytes) {
        QUARRY_TRY(FlushMemtable());
        QUARRY_TRY(MaybeCompact());
        QUARRY_TRY(WriteManifest());
    }
    return Status::Ok();
}

Status LsmIndex::Lookup(const Value& key, std::vector<uint64_t>& out) const {
    ++lookups_;
    if (key.is_null()) return Status::InvalidArgument("lsm index key must not be NULL");

    const std::string encoded = EncodeKey(key);
    const size_t start = out.size();

    mem_.Lookup(encoded, out);

    // Newest first, shallowest level first: level 0 holds the most recently
    // flushed memtables, so this is the order a reader would want if the index
    // had overwrite semantics, and it is the order the counters should describe.
    for (const auto& level : levels_) {
        for (const auto& ref : level) {
            if (!ref.table->MayContain(encoded)) {
                ++bloom_rejections_;
                continue;
            }
            ++sstables_consulted_;
            bool found = false;
            QUARRY_TRY(ref.table->Lookup(encoded, out, found));
        }
    }

    // A key can appear in the memtable and in any number of tables, so the row
    // ids collected above may repeat. Sorting the appended tail only — the
    // caller's existing contents are theirs — keeps Lookup's append contract.
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
    out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(start), out.end()), out.end());
    return Status::Ok();
}

Status LsmIndex::RangeScan(const std::optional<Value>& lo, const std::optional<Value>& hi,
                           std::vector<uint64_t>& out) const {
    std::string lo_encoded, hi_encoded;
    const std::string* lower = nullptr;
    const std::string* upper = nullptr;
    if (lo.has_value() && !lo->is_null()) {
        lo_encoded = EncodeKey(*lo);
        lower = &lo_encoded;
    }
    if (hi.has_value() && !hi->is_null()) {
        hi_encoded = EncodeKey(*hi);
        upper = &hi_encoded;
    }

    std::vector<std::unique_ptr<MergeSource>> sources;
    sources.push_back(std::make_unique<MemtableSource>(mem_.table(), lower));
    for (const auto& level : levels_) {
        for (const auto& ref : level) {
            // Every table has to be opened: a range has no single key to probe a
            // Bloom filter with, which is precisely why a range scan on an LSM
            // costs more than on a B+ tree.
            ++sstables_consulted_;
            sources.push_back(std::make_unique<TableSource>(ref.table.get(), lower));
        }
    }

    return MergeAll(sources, upper, [&out](const std::string&, std::vector<uint64_t>& ids) {
        out.insert(out.end(), ids.begin(), ids.end());
        return Status::Ok();
    });
}

Status LsmIndex::Flush() {
    if (!mem_.empty()) {
        QUARRY_TRY(FlushMemtable());
        QUARRY_TRY(MaybeCompact());
    }
    return WriteManifest();
}

Status LsmIndex::FlushMemtable() {
    if (mem_.empty()) return Status::Ok();

    const uint64_t file_number = next_file_++;
    const std::string path = TablePath(file_number);

    SsTableBuilder builder(path, mem_.KeyCount(), opts_.bloom_fpr, io_);
    QUARRY_TRY(builder.Open());
    for (const auto& [key, row_ids] : mem_.table()) {
        QUARRY_TRY(builder.Add(key, row_ids));
    }
    auto built = builder.Finish();
    if (!built.ok()) return built.status();

    auto opened = SsTable::Open(path, io_);
    if (!opened.ok()) return opened.status();

    if (levels_.empty()) levels_.emplace_back();
    levels_[0].insert(levels_[0].begin(), TableRef{opened.take()});
    mem_.Clear();
    return Status::Ok();
}

Status LsmIndex::MaybeCompact() {
    for (size_t level = 0; level < levels_.size(); ++level) {
        while (levels_[level].size() >= opts_.level_fanout) {
            QUARRY_TRY(CompactLevel(level));
        }
    }
    return Status::Ok();
}

Status LsmIndex::CompactLevel(size_t level) {
    // Size-tiered: the whole tier is merged into one table in the tier below, so
    // every byte of this level is rewritten exactly once per compaction and the
    // rewrite shows up in pages_written and bytes_written.
    uint64_t expected_entries = 0;
    std::vector<std::unique_ptr<MergeSource>> sources;
    std::vector<std::string> retired;
    for (const auto& ref : levels_[level]) {
        expected_entries += ref.table->entry_count();
        retired.push_back(ref.table->path());
        sources.push_back(std::make_unique<TableSource>(ref.table.get(), nullptr));
    }

    const uint64_t file_number = next_file_++;
    const std::string path = TablePath(file_number);
    SsTableBuilder builder(path, expected_entries, opts_.bloom_fpr, io_);
    QUARRY_TRY(builder.Open());

    Status build_status = MergeAll(
        sources, nullptr, [&builder](const std::string& key, std::vector<uint64_t>& ids) {
            return builder.Add(key, ids);
        });
    QUARRY_TRY(build_status);

    auto built = builder.Finish();
    if (!built.ok()) return built.status();

    auto opened = SsTable::Open(path, io_);
    if (!opened.ok()) return opened.status();

    // The merged tables are destroyed (closing their file handles) before their
    // files are unlinked, and the new table is in place first, so a crash between
    // the two leaves an index that still has every key — merely one with a stale
    // copy on disk that the next manifest write orphans.
    sources.clear();
    while (levels_.size() <= level + 1) levels_.emplace_back();
    levels_[level].clear();
    levels_[level + 1].insert(levels_[level + 1].begin(), TableRef{opened.take()});

    for (const auto& p : retired) {
        std::error_code ec;
        std::filesystem::remove(p, ec);  // a leftover file is waste, not an error
    }

    ++compactions_;
    return WriteManifest();
}

Status LsmIndex::WriteManifest() const {
    std::string body;
    body += kManifestMagic;
    body += "\n";
    body += "next_file " + std::to_string(next_file_) + "\n";
    body += "logical_bytes " + std::to_string(logical_bytes_) + "\n";
    body += "bytes_written " + std::to_string(io_->bytes_written) + "\n";
    body += "pages_written " + std::to_string(io_->pages_written) + "\n";
    body += "compactions " + std::to_string(compactions_) + "\n";
    for (size_t level = 0; level < levels_.size(); ++level) {
        for (const auto& ref : levels_[level]) {
            const std::string name =
                std::filesystem::path(ref.table->path()).filename().string();
            body += "table " + std::to_string(level) + " " + name + "\n";
        }
    }

    // The manifest names every live table, so a half-written one would make a
    // reopen either lose data or chase a file that is not there. The checksum
    // turns that into a reported corruption, and the rename makes the switch
    // atomic so there is no window in which the manifest is partial.
    std::string out = body;
    out += "crc " + std::to_string(Crc32(body.data(), body.size())) + "\n";

    const std::string tmp = dir_ + "/" + kManifestTmpName;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return Status::IoError("cannot write " + tmp);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        f.flush();
        if (!f) return Status::IoError("short write to " + tmp);
    }
    io_->bytes_written += out.size();
    io_->pages_written += PagesFor(out.size());

    std::error_code ec;
    std::filesystem::rename(tmp, dir_ + "/" + kManifestName, ec);
    if (ec) return Status::IoError("cannot install manifest: " + ec.message());
    return Status::Ok();
}

Status LsmIndex::LoadManifest() {
    const std::string path = dir_ + "/" + kManifestName;
    if (!std::filesystem::exists(path)) return Status::Ok();  // a fresh index

    std::ifstream f(path, std::ios::binary);
    if (!f) return Status::IoError("cannot read " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const size_t crc_pos = content.rfind("\ncrc ");
    if (crc_pos == std::string::npos) return Status::Corruption(path + ": no checksum line");
    const size_t body_len = crc_pos + 1;
    uint32_t stored = 0;
    {
        std::istringstream crc_line(content.substr(body_len));
        std::string tag;
        uint64_t value = 0;
        if (!(crc_line >> tag >> value) || tag != "crc") {
            return Status::Corruption(path + ": malformed checksum line");
        }
        stored = static_cast<uint32_t>(value);
    }
    if (Crc32(content.data(), body_len) != stored) {
        return Status::Corruption(path + ": checksum mismatch");
    }

    std::istringstream in(content.substr(0, body_len));
    std::string magic;
    if (!(in >> magic) || magic != kManifestMagic) {
        return Status::Corruption(path + ": unrecognised manifest");
    }

    std::vector<std::pair<size_t, std::string>> tables;
    std::string tag;
    while (in >> tag) {
        if (tag == "next_file") {
            in >> next_file_;
        } else if (tag == "logical_bytes") {
            in >> logical_bytes_;
        } else if (tag == "bytes_written") {
            in >> io_->bytes_written;
        } else if (tag == "pages_written") {
            in >> io_->pages_written;
        } else if (tag == "compactions") {
            in >> compactions_;
        } else if (tag == "table") {
            size_t level = 0;
            std::string name;
            if (!(in >> level >> name)) return Status::Corruption(path + ": bad table line");
            tables.emplace_back(level, name);
        } else {
            return Status::Corruption(path + ": unknown key '" + tag + "'");
        }
        if (in.fail()) return Status::Corruption(path + ": malformed value for '" + tag + "'");
    }

    for (const auto& [level, name] : tables) {
        auto opened = SsTable::Open(dir_ + "/" + name, io_);
        if (!opened.ok()) return opened.status();
        while (levels_.size() <= level) levels_.emplace_back();
        levels_[level].push_back(TableRef{opened.take()});
    }
    return Status::Ok();
}

std::string LsmIndex::Describe() const {
    std::ostringstream os;
    os << "lsm(dir=" << dir_ << ", compaction=size-tiered, fanout=" << opts_.level_fanout
       << ", memtable=" << mem_.ApproximateBytes() << "/" << opts_.memtable_bytes << "B"
       << ", levels=[";
    for (size_t i = 0; i < levels_.size(); ++i) {
        if (i) os << ",";
        os << levels_[i].size();
    }
    os << "])";
    return os.str();
}

Index::Stats LsmIndex::stats() const {
    Stats s;
    s.inserts = inserts_;
    s.lookups = lookups_;
    s.pages_read = io_->pages_read;
    s.pages_written = io_->pages_written;
    s.node_splits = 0;  // an LSM never splits a node; the B+ tree owns that number
    s.compactions = compactions_;
    s.bloom_rejections = bloom_rejections_;
    s.sstables_consulted = sstables_consulted_;
    return s;
}

}  // namespace quarry::lsm
