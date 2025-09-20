#include "quarry/catalog.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "catalog_format.hpp"

namespace quarry {

namespace {

constexpr const char* kCatalogFileName = "catalog.qcat";
constexpr const char* kCatalogTempName = "catalog.qcat.tmp";

std::string Errno(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + std::strerror(errno);
}

bool KnownType(uint8_t raw) {
    switch (static_cast<TypeId>(raw)) {
        case TypeId::kBoolean:
        case TypeId::kInt64:
        case TypeId::kDouble:
        case TypeId::kVarchar:
            return true;
        case TypeId::kInvalid:
        default:
            return false;
    }
}

bool KnownEncoding(uint8_t raw) {
    switch (static_cast<Encoding>(raw)) {
        case Encoding::kPlain:
        case Encoding::kDictionary:
        case Encoding::kRunLength:
            return true;
        default:
            return false;
    }
}

bool KnownIndexKind(uint8_t raw) {
    switch (static_cast<IndexKind>(raw)) {
        case IndexKind::kBTree:
        case IndexKind::kLsm:
            return true;
        default:
            return false;
    }
}

void WriteStats(std::vector<uint8_t>& out, const ColumnStats& s) {
    catfmt::PutU8(out, s.all_null ? 1u : 0u);
    catfmt::PutU64(out, s.null_count);
    catfmt::PutU64(out, s.distinct_estimate);
    catfmt::PutValue(out, s.min);
    catfmt::PutValue(out, s.max);
}

ColumnStats ReadStats(catfmt::Reader& r) {
    ColumnStats s;
    s.all_null = r.GetU8() != 0;
    s.null_count = r.GetU64();
    s.distinct_estimate = r.GetU64();
    s.min = r.GetValue();
    s.max = r.GetValue();
    return s;
}

// WriteFileAtomic is the whole durability story for the catalog: a temp file in
// the same directory, fsync of its contents, rename over the real name, then an
// fsync of the directory itself.
//
// The last step is the one that is easy to leave out and wrong to. rename() is
// atomic with respect to readers, but the directory entry it creates is not
// durable until the directory is synced, so a crash after a successful rename can
// leave the old name in place — or, on some file systems, neither name. Syncing
// the directory is what makes "the catalog on disk is either the old one or the
// new one" true across a power loss rather than only across a process crash.
Status WriteFileAtomic(const std::string& dir, const std::string& final_path,
                       const std::string& temp_path, const std::vector<uint8_t>& bytes) {
    int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return Status::IoError(Errno("open", temp_path));

    const uint8_t* p = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const Status s = Status::IoError(Errno("write", temp_path));
            ::close(fd);
            ::unlink(temp_path.c_str());
            return s;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }

    if (::fsync(fd) != 0) {
        const Status s = Status::IoError(Errno("fsync", temp_path));
        ::close(fd);
        ::unlink(temp_path.c_str());
        return s;
    }
    if (::close(fd) != 0) {
        const Status s = Status::IoError(Errno("close", temp_path));
        ::unlink(temp_path.c_str());
        return s;
    }

    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        const Status s = Status::IoError(Errno("rename", temp_path));
        ::unlink(temp_path.c_str());
        return s;
    }

    const int dir_fd = ::open(dir.c_str(), O_RDONLY);
    if (dir_fd < 0) return Status::IoError(Errno("open directory", dir));
    const bool synced = ::fsync(dir_fd) == 0;
    const std::string sync_error = synced ? std::string() : Errno("fsync directory", dir);
    ::close(dir_fd);
    if (!synced) return Status::IoError(sync_error);

    return Status::Ok();
}

Result<std::vector<uint8_t>> ReadWholeFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Status::IoError(Errno("open", path));

    std::vector<uint8_t> bytes;
    uint8_t buf[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            const Status s = Status::IoError(Errno("read", path));
            ::close(fd);
            return s;
        }
        if (n == 0) break;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    ::close(fd);
    return bytes;
}

}  // namespace

const char* IndexKindName(IndexKind k) {
    switch (k) {
        case IndexKind::kBTree: return "btree";
        case IndexKind::kLsm:   return "lsm";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// TableDef
// ---------------------------------------------------------------------------

uint64_t TableDef::row_count() const {
    uint64_t total = 0;
    for (const auto& seg : segments) total += seg.row_count;
    return total;
}

ColumnStats TableDef::ColumnStatistics(size_t column_index) const {
    ColumnStats merged;

    // all_null starts true and is cleared by the first segment that has a real
    // value, so a table with no segments reports "nothing but nulls here" rather
    // than reporting bounds that do not exist. The scan reads all_null before it
    // reads min and max, so this is the safe direction: it prunes nothing.
    merged.all_null = true;

    uint64_t distinct_sum = 0;
    uint64_t distinct_max = 0;
    bool have_bounds = false;

    for (const auto& seg : segments) {
        if (column_index >= seg.columns.size()) continue;  // schema/segment skew
        const ColumnStats& s = seg.columns[column_index].stats;

        merged.null_count += s.null_count;
        distinct_sum += s.distinct_estimate;
        distinct_max = std::max(distinct_max, s.distinct_estimate);

        if (s.all_null) continue;
        merged.all_null = false;

        if (!have_bounds) {
            merged.min = s.min;
            merged.max = s.max;
            have_bounds = true;
            continue;
        }
        if (!s.min.is_null() && (merged.min.is_null() || s.min < merged.min)) merged.min = s.min;
        if (!s.max.is_null() && (merged.max.is_null() || merged.max < s.max)) merged.max = s.max;
    }

    if (merged.all_null) {
        merged.min = Value::Null();
        merged.max = Value::Null();
    }

    // Merging bounds is exact; merging distinct counts is not, and this is the
    // one number in the catalog that is openly a guess.
    //
    // The true table-wide cardinality lies between max(per-segment counts) — if
    // every segment holds the same value set — and the sum of them — if the sets
    // are disjoint. Nothing in a per-segment min/max/count can distinguish the
    // two, and getting it right needs a mergeable sketch (HyperLogLog) that the
    // segment footer does not carry.
    //
    // The sum, clamped to the row count, is used. It is exact for the two cases
    // that matter most in practice — a key column and a time-ordered load, where
    // segments really are disjoint — and it over-estimates when segments overlap.
    // The clamp is what keeps the over-estimate bounded: cardinality can never
    // exceed the number of rows. An over-estimate makes equality predicates look
    // more selective than they are, which biases the optimizer toward an index
    // lookup; that is the cheaper mistake, since a too-eager index probe is slow
    // while a missed one can be a full scan of the table.
    const uint64_t rows = row_count();
    merged.distinct_estimate = std::max(distinct_max, std::min(distinct_sum, rows));

    return merged;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

Catalog::Catalog(std::string dir) : dir_(std::move(dir)) {}

std::string Catalog::CatalogPath() const {
    return (std::filesystem::path(dir_) / kCatalogFileName).string();
}

std::string Catalog::SegmentPath(const std::string& table, uint64_t segment_id) const {
    return (std::filesystem::path(dir_) / (table + "_seg" + std::to_string(segment_id) + ".qseg"))
        .string();
}

std::string Catalog::IndexPath(const std::string& table, const std::string& index_name) const {
    return (std::filesystem::path(dir_) / (table + "_idx_" + index_name + ".qidx")).string();
}

Status Catalog::Open() {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec && !std::filesystem::is_directory(dir_)) {
        return Status::IoError("create directory " + dir_ + ": " + ec.message());
    }

    const std::string path = CatalogPath();
    if (!std::filesystem::exists(path)) {
        // A directory with no catalog is a new database, which is the only case
        // where starting empty is correct.
        tables_.clear();
        next_segment_id_ = 1;
        return Status::Ok();
    }

    auto raw = ReadWholeFile(path);
    if (!raw.ok()) return raw.status();
    const std::vector<uint8_t> bytes = raw.take();

    const auto corrupt = [&path](const std::string& why) {
        // Deliberately not a fresh start. Every segment file on disk is named
        // only by this file; discarding it would leave them unreferenced and
        // silently report an empty database that is not empty.
        return Status::Corruption("catalog " + path + " is unreadable: " + why +
                                  " (refusing to start empty, which would orphan "
                                  "every segment file in the directory)");
    };

    if (bytes.size() < catfmt::kHeaderBytes) {
        return corrupt("shorter than the " + std::to_string(catfmt::kHeaderBytes) +
                       "-byte header");
    }
    if (std::memcmp(bytes.data(), catfmt::kMagic, sizeof(catfmt::kMagic)) != 0) {
        return corrupt("bad magic");
    }

    catfmt::Reader head(bytes.data() + sizeof(catfmt::kMagic),
                        catfmt::kHeaderBytes - sizeof(catfmt::kMagic));
    const uint32_t version = head.GetU32();
    const uint32_t reserved = head.GetU32();
    const uint64_t body_length = head.GetU64();
    const uint32_t body_crc = head.GetU32();

    if (version != catfmt::kVersion) {
        return corrupt("version " + std::to_string(version) + ", this build writes " +
                       std::to_string(catfmt::kVersion));
    }
    if (reserved != 0) return corrupt("reserved header word is not zero");
    if (body_length != bytes.size() - catfmt::kHeaderBytes) {
        return corrupt("body length " + std::to_string(body_length) + " but " +
                       std::to_string(bytes.size() - catfmt::kHeaderBytes) + " bytes follow");
    }

    const uint8_t* body = bytes.data() + catfmt::kHeaderBytes;
    const uint32_t actual_crc = catfmt::Crc32(body, body_length);
    if (actual_crc != body_crc) {
        return corrupt("crc32 mismatch (header says " + std::to_string(body_crc) + ", body is " +
                       std::to_string(actual_crc) + ")");
    }

    catfmt::Reader r(body, body_length);
    const uint64_t next_segment_id = r.GetU64();
    const uint32_t table_count = r.GetU32();

    std::vector<TableDef> tables;
    for (uint32_t t = 0; t < table_count && r.ok(); ++t) {
        TableDef def;
        def.name = r.GetString();

        const uint32_t column_count = r.GetU32();
        for (uint32_t c = 0; c < column_count && r.ok(); ++c) {
            Column col;
            col.name = r.GetString();
            const uint8_t type = r.GetU8();
            if (r.ok() && !KnownType(type)) {
                r.Fail("column " + col.name + " has unknown type byte " + std::to_string(type));
                break;
            }
            col.type = static_cast<TypeId>(type);
            col.nullable = r.GetU8() != 0;
            def.schema.Add(std::move(col));
        }

        const uint32_t segment_count = r.GetU32();
        for (uint32_t s = 0; s < segment_count && r.ok(); ++s) {
            SegmentMeta meta;
            meta.id = r.GetU64();
            meta.row_count = r.GetU64();
            meta.path = r.GetString();
            const uint32_t chunk_count = r.GetU32();
            for (uint32_t k = 0; k < chunk_count && r.ok(); ++k) {
                ColumnChunk chunk;
                const uint8_t type = r.GetU8();
                const uint8_t enc = r.GetU8();
                if (r.ok() && !KnownType(type)) {
                    r.Fail("segment " + std::to_string(meta.id) + " chunk " + std::to_string(k) +
                           " has unknown type byte " + std::to_string(type));
                    break;
                }
                if (r.ok() && !KnownEncoding(enc)) {
                    r.Fail("segment " + std::to_string(meta.id) + " chunk " + std::to_string(k) +
                           " has unknown encoding byte " + std::to_string(enc));
                    break;
                }
                chunk.type = static_cast<TypeId>(type);
                chunk.encoding = static_cast<Encoding>(enc);
                chunk.offset = r.GetU64();
                chunk.length = r.GetU64();
                chunk.raw_length = r.GetU64();
                chunk.stats = ReadStats(r);
                meta.columns.push_back(std::move(chunk));
            }
            def.segments.push_back(std::move(meta));
        }

        const uint32_t index_count = r.GetU32();
        for (uint32_t i = 0; i < index_count && r.ok(); ++i) {
            IndexDef idx;
            idx.name = r.GetString();
            idx.column_index = static_cast<size_t>(r.GetU64());
            const uint8_t kind = r.GetU8();
            if (r.ok() && !KnownIndexKind(kind)) {
                r.Fail("index " + idx.name + " has unknown kind byte " + std::to_string(kind));
                break;
            }
            idx.kind = static_cast<IndexKind>(kind);
            idx.path = r.GetString();
            def.indexes.push_back(std::move(idx));
        }

        tables.push_back(std::move(def));
    }

    if (!r.ok()) return corrupt(r.error());
    if (r.remaining() != 0) {
        return corrupt(std::to_string(r.remaining()) + " trailing byte(s) after the last table");
    }

    // Cross-check rather than trust: a next_segment_id below an id already in use
    // would hand out a duplicate and make a new segment overwrite an existing
    // file. Caught here because the writer of the duplicate would not notice.
    for (const auto& def : tables) {
        for (const auto& seg : def.segments) {
            if (seg.id >= next_segment_id) {
                return corrupt("next_segment_id " + std::to_string(next_segment_id) +
                               " is not past segment id " + std::to_string(seg.id) + " in table " +
                               def.name);
            }
        }
    }

    tables_ = std::move(tables);
    next_segment_id_ = next_segment_id;
    return Status::Ok();
}

Status Catalog::Save() const {
    std::vector<uint8_t> body;
    catfmt::PutU64(body, next_segment_id_);
    catfmt::PutU32(body, static_cast<uint32_t>(tables_.size()));

    for (const auto& def : tables_) {
        catfmt::PutString(body, def.name);

        catfmt::PutU32(body, static_cast<uint32_t>(def.schema.size()));
        for (const auto& col : def.schema.columns()) {
            catfmt::PutString(body, col.name);
            catfmt::PutU8(body, static_cast<uint8_t>(col.type));
            catfmt::PutU8(body, col.nullable ? 1u : 0u);
        }

        catfmt::PutU32(body, static_cast<uint32_t>(def.segments.size()));
        for (const auto& seg : def.segments) {
            catfmt::PutU64(body, seg.id);
            catfmt::PutU64(body, seg.row_count);
            catfmt::PutString(body, seg.path);
            catfmt::PutU32(body, static_cast<uint32_t>(seg.columns.size()));
            for (const auto& chunk : seg.columns) {
                catfmt::PutU8(body, static_cast<uint8_t>(chunk.type));
                catfmt::PutU8(body, static_cast<uint8_t>(chunk.encoding));
                catfmt::PutU64(body, chunk.offset);
                catfmt::PutU64(body, chunk.length);
                catfmt::PutU64(body, chunk.raw_length);
                WriteStats(body, chunk.stats);
            }
        }

        catfmt::PutU32(body, static_cast<uint32_t>(def.indexes.size()));
        for (const auto& idx : def.indexes) {
            catfmt::PutString(body, idx.name);
            catfmt::PutU64(body, static_cast<uint64_t>(idx.column_index));
            catfmt::PutU8(body, static_cast<uint8_t>(idx.kind));
            catfmt::PutString(body, idx.path);
        }
    }

    std::vector<uint8_t> file;
    file.reserve(catfmt::kHeaderBytes + body.size());
    file.insert(file.end(), catfmt::kMagic, catfmt::kMagic + sizeof(catfmt::kMagic));
    catfmt::PutU32(file, catfmt::kVersion);
    catfmt::PutU32(file, 0);
    catfmt::PutU64(file, static_cast<uint64_t>(body.size()));
    catfmt::PutU32(file, catfmt::Crc32(body.data(), body.size()));
    file.insert(file.end(), body.begin(), body.end());

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec && !std::filesystem::is_directory(dir_)) {
        return Status::IoError("create directory " + dir_ + ": " + ec.message());
    }

    return WriteFileAtomic(dir_, CatalogPath(),
                           (std::filesystem::path(dir_) / kCatalogTempName).string(), file);
}

Status Catalog::CreateTable(const std::string& name, Schema schema) {
    if (name.empty()) return Status::InvalidArgument("table name is empty");
    if (schema.empty()) return Status::InvalidArgument("table " + name + " has no columns");

    for (size_t i = 0; i < schema.size(); ++i) {
        for (size_t j = i + 1; j < schema.size(); ++j) {
            if (schema.at(i).name == schema.at(j).name) {
                return Status::InvalidArgument("table " + name + " declares column " +
                                               schema.at(i).name + " twice");
            }
        }
        if (schema.at(i).type == TypeId::kInvalid) {
            return Status::InvalidArgument("column " + schema.at(i).name + " has no type");
        }
    }

    if (HasTable(name)) return Status::InvalidArgument("table " + name + " already exists");

    TableDef def;
    def.name = name;
    def.schema = std::move(schema);
    tables_.push_back(std::move(def));
    return Status::Ok();
}

Status Catalog::DropTable(const std::string& name) {
    const auto it = std::find_if(tables_.begin(), tables_.end(),
                                 [&name](const TableDef& d) { return d.name == name; });
    if (it == tables_.end()) return Status::BindError("no such table: " + name);

    // Only the metadata goes. The segment and index files are left behind rather
    // than unlinked here, because Save is what makes the drop durable and a file
    // deleted before a successful Save would be a drop that half happened. The
    // files are unreachable either way; reclaiming them is a separate sweep.
    tables_.erase(it);
    return Status::Ok();
}

bool Catalog::HasTable(const std::string& name) const {
    return std::any_of(tables_.begin(), tables_.end(),
                       [&name](const TableDef& d) { return d.name == name; });
}

Result<const TableDef*> Catalog::GetTable(const std::string& name) const {
    for (const auto& def : tables_) {
        if (def.name == name) return &def;
    }
    return Status::BindError("no such table: " + name);
}

Result<TableDef*> Catalog::GetTableMutable(const std::string& name) {
    for (auto& def : tables_) {
        if (def.name == name) return &def;
    }
    return Status::BindError("no such table: " + name);
}

std::vector<std::string> Catalog::TableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& def : tables_) names.push_back(def.name);
    return names;
}

Status Catalog::AddSegment(const std::string& table, SegmentMeta meta) {
    auto found = GetTableMutable(table);
    if (!found.ok()) return found.status();
    TableDef* def = found.value();

    if (meta.columns.size() != def->schema.size()) {
        return Status::InvalidArgument("segment " + std::to_string(meta.id) + " for table " +
                                      table + " has " + std::to_string(meta.columns.size()) +
                                      " column chunk(s), schema has " +
                                      std::to_string(def->schema.size()));
    }
    for (size_t i = 0; i < meta.columns.size(); ++i) {
        if (meta.columns[i].type != def->schema.at(i).type) {
            return Status::TypeError("segment " + std::to_string(meta.id) + " column " +
                                     def->schema.at(i).name + " is " +
                                     TypeName(meta.columns[i].type) + ", schema says " +
                                     TypeName(def->schema.at(i).type));
        }
    }
    for (const auto& existing : def->segments) {
        if (existing.id == meta.id) {
            return Status::InvalidArgument("table " + table + " already has segment " +
                                           std::to_string(meta.id));
        }
    }

    // next_segment_id_ is advanced past whatever arrives rather than being the
    // only source of ids, so a segment whose id came from somewhere else can
    // never be handed out again.
    if (meta.id >= next_segment_id_) next_segment_id_ = meta.id + 1;

    def->segments.push_back(std::move(meta));
    return Status::Ok();
}

Status Catalog::AddIndex(const std::string& table, IndexDef def_in) {
    auto found = GetTableMutable(table);
    if (!found.ok()) return found.status();
    TableDef* def = found.value();

    if (def_in.name.empty()) return Status::InvalidArgument("index name is empty");
    if (def_in.column_index >= def->schema.size()) {
        return Status::BindError("index " + def_in.name + " names column " +
                                std::to_string(def_in.column_index) + " but table " + table +
                                " has " + std::to_string(def->schema.size()) + " column(s)");
    }
    for (const auto& existing : def->indexes) {
        if (existing.name == def_in.name) {
            return Status::InvalidArgument("index " + def_in.name + " already exists on table " +
                                           table);
        }
    }

    def->indexes.push_back(std::move(def_in));
    return Status::Ok();
}

}  // namespace quarry
