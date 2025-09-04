#include "segment_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "encoding.hpp"

namespace quarry {
namespace {

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Value encoding for the footer's min/max bounds. The reader's counterpart is
// ReadValue in segment_reader.cpp; the two must change together.
enum ValueTag : uint8_t {
    kTagNull = 0,
    kTagBoolean = 1,
    kTagInt64 = 2,
    kTagDouble = 3,
    kTagVarchar = 4,
};

void PutValue(std::vector<uint8_t>& out, const Value& v) {
    if (v.is_null()) {
        out.push_back(kTagNull);
        return;
    }
    switch (v.type()) {
        case TypeId::kBoolean:
            out.push_back(kTagBoolean);
            out.push_back(v.as_boolean() ? 1 : 0);
            break;
        case TypeId::kInt64:
            out.push_back(kTagInt64);
            PutU64(out, static_cast<uint64_t>(v.as_int64()));
            break;
        case TypeId::kDouble: {
            out.push_back(kTagDouble);
            uint64_t bits = 0;
            const double d = v.as_double();
            std::memcpy(&bits, &d, sizeof(bits));
            PutU64(out, bits);
            break;
        }
        case TypeId::kVarchar: {
            out.push_back(kTagVarchar);
            const std::string& s = v.as_varchar();
            PutU32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
        default:
            out.push_back(kTagNull);
            break;
    }
}

// WriteFully loops because a single write() is allowed to be partial, and a short
// write that goes unnoticed produces a file whose footer offset points into the
// middle of a chunk.
Status WriteFully(int fd, const uint8_t* data, size_t len, const std::string& path) {
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IoError("write to " + path + " failed: " + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

}  // namespace

// ColumnAccum holds one column's rows until Finish, plus the statistics computed
// incrementally as rows arrive.
//
// Statistics are accumulated during Append rather than recomputed at Finish
// because min/max are the only thing a scan needs in order to skip this segment
// entirely, and a second pass over every value to produce them would double the
// write cost of the one structure that exists to avoid reads.
struct FileSegmentWriter::ColumnAccum {
    explicit ColumnAccum(TypeId type) : values(type) {}

    Vector values;
    ColumnStats stats;
    uint64_t rows = 0;

    void Observe(const Value& v) {
        ++rows;
        if (v.is_null()) {
            ++stats.null_count;
            return;
        }
        // A NaN is deliberately left out of the bounds. It compares false against
        // everything, so a row holding one can never satisfy a predicate, and
        // folding it into min or max would poison bounds that are otherwise exact.
        if (v.type() == TypeId::kDouble && std::isnan(v.as_double())) return;

        if (stats.min.is_null() || v < stats.min) stats.min = v;
        if (stats.max.is_null() || stats.max < v) stats.max = v;
    }
};

FileSegmentWriter::FileSegmentWriter(std::string path, Schema schema, uint64_t id)
    : path_(std::move(path)), schema_(std::move(schema)), id_(id) {
    for (const auto& col : schema_.columns()) {
        columns_.push_back(std::make_unique<ColumnAccum>(col.type));
    }
}

FileSegmentWriter::~FileSegmentWriter() = default;

Status FileSegmentWriter::Append(const Batch& batch) {
    if (finished_) {
        return Status::Internal("Append on a segment writer that has already finished");
    }
    if (batch.column_count() != schema_.size()) {
        return Status::InvalidArgument("batch has " + std::to_string(batch.column_count()) +
                                       " columns, segment schema has " +
                                       std::to_string(schema_.size()));
    }
    for (size_t c = 0; c < schema_.size(); ++c) {
        if (batch.column(c).type() != schema_.at(c).type) {
            return Status::TypeError(std::string("column ") + schema_.at(c).name + " is " +
                                     TypeName(schema_.at(c).type) + " but the batch supplied " +
                                     TypeName(batch.column(c).type()));
        }
    }

    // Iterating Rows() rather than 0..row_count means a batch that has been
    // filtered writes only its surviving rows. Reading densely here would silently
    // resurrect rows a filter had already rejected.
    const std::vector<uint32_t>& rows = batch.Rows();
    for (uint32_t r : rows) {
        for (size_t c = 0; c < schema_.size(); ++c) {
            const Vector& src = batch.column(c);
            ColumnAccum& acc = *columns_[c];
            const bool is_null = src.nulls().IsNull(r);

            // The stored bytes are copied even for a null slot, so a round trip
            // reproduces the input exactly rather than normalizing nulls to a
            // placeholder of the storage layer's choosing.
            switch (schema_.at(c).type) {
                case TypeId::kBoolean: acc.values.AppendBoolean(src.bool_data()[r] != 0); break;
                case TypeId::kInt64:   acc.values.AppendInt64(src.int64_data()[r]); break;
                case TypeId::kDouble:  acc.values.AppendDouble(src.double_data()[r]); break;
                case TypeId::kVarchar: acc.values.AppendString(src.GetString(r)); break;
                default:
                    return Status::InvalidArgument(std::string("column ") + schema_.at(c).name +
                                                   " has an unwritable type");
            }
            if (is_null) acc.values.nulls().SetNull(acc.values.size() - 1);
            acc.Observe(is_null ? Value::Null() : src.GetValue(r));
        }
    }
    row_count_ += rows.size();
    return Status::Ok();
}

Result<SegmentMeta> FileSegmentWriter::Finish() {
    if (finished_) {
        return Status::Internal("Finish called twice on the same segment writer");
    }
    finished_ = true;

    // Encode every column up front. Doing this before the file is created means a
    // failure during encoding leaves no partial segment on disk at all.
    std::vector<std::vector<uint8_t>> payloads(columns_.size());
    SegmentMeta meta;
    meta.id = id_;
    meta.row_count = row_count_;
    meta.path = path_;
    meta.columns.resize(columns_.size());

    for (size_t c = 0; c < columns_.size(); ++c) {
        ColumnAccum& acc = *columns_[c];
        const encoding::ColumnProfile profile = encoding::Profile(acc.values, row_count_);
        const Encoding enc = encoding::Choose(schema_.at(c).type, profile);

        uint64_t raw_length = 0;
        Status s = encoding::Encode(acc.values, row_count_, enc, payloads[c], raw_length);
        if (!s.ok()) return s;

        ColumnChunk& chunk = meta.columns[c];
        chunk.type = schema_.at(c).type;
        chunk.encoding = enc;
        chunk.raw_length = raw_length;
        chunk.stats = acc.stats;
        chunk.stats.distinct_estimate = profile.distinct_count;
        chunk.stats.all_null = row_count_ > 0 && acc.stats.null_count == row_count_;
        if (chunk.stats.all_null) {
            // min and max are meaningless for an all-null column, and leaving a
            // stale bound here is exactly the kind of thing a zone map would
            // happily prune against.
            chunk.stats.min = Value::Null();
            chunk.stats.max = Value::Null();
        }
    }

    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Status::IoError("cannot create segment " + path_ + ": " + std::strerror(errno));
    }

    struct FdGuard {
        int fd;
        ~FdGuard() { if (fd >= 0) ::close(fd); }
    } guard{fd};

    std::vector<uint8_t> header;
    header.insert(header.end(), segfmt::kMagic, segfmt::kMagic + sizeof(segfmt::kMagic));
    PutU32(header, segfmt::kVersion);
    PutU32(header, static_cast<uint32_t>(columns_.size()));
    PutU64(header, row_count_);
    PutU64(header, id_);
    if (header.size() != segfmt::kHeaderBytes) {
        return Status::Internal("segment header is " + std::to_string(header.size()) +
                                " bytes, expected " + std::to_string(segfmt::kHeaderBytes));
    }
    Status s = WriteFully(fd, header.data(), header.size(), path_);
    if (!s.ok()) return s;

    uint64_t at = segfmt::kHeaderBytes;
    for (size_t c = 0; c < columns_.size(); ++c) {
        const std::vector<uint8_t>& payload = payloads[c];
        const uint32_t crc = encoding::Crc32(payload.data(), payload.size());

        meta.columns[c].offset = at;
        meta.columns[c].length = payload.size() + segfmt::kChunkChecksumBytes;

        s = WriteFully(fd, payload.data(), payload.size(), path_);
        if (!s.ok()) return s;

        std::vector<uint8_t> crc_bytes;
        PutU32(crc_bytes, crc);
        s = WriteFully(fd, crc_bytes.data(), crc_bytes.size(), path_);
        if (!s.ok()) return s;

        at += meta.columns[c].length;
    }

    // Footer. Parsed by ParseFooter in segment_reader.cpp, which is the only other
    // place that knows this layout.
    std::vector<uint8_t> footer;
    footer.insert(footer.end(), segfmt::kFooterMagic,
                  segfmt::kFooterMagic + sizeof(segfmt::kFooterMagic));
    PutU32(footer, static_cast<uint32_t>(columns_.size()));
    for (size_t c = 0; c < columns_.size(); ++c) {
        const ColumnChunk& chunk = meta.columns[c];
        footer.push_back(static_cast<uint8_t>(chunk.type));
        footer.push_back(static_cast<uint8_t>(chunk.encoding));
        footer.push_back(chunk.stats.all_null ? 1 : 0);
        footer.push_back(0);
        PutU64(footer, chunk.offset);
        PutU64(footer, chunk.length);
        PutU64(footer, chunk.raw_length);
        PutU64(footer, chunk.stats.null_count);
        PutU64(footer, chunk.stats.distinct_estimate);
        PutValue(footer, chunk.stats.min);
        PutValue(footer, chunk.stats.max);

        // The column name is not part of ColumnChunk, but without it a reader has
        // no way to produce a Batch whose schema matches the table's, so it is
        // carried here and handed back by ReadSegmentFile.
        const std::string& name = schema_.at(c).name;
        PutU32(footer, static_cast<uint32_t>(name.size()));
        footer.insert(footer.end(), name.begin(), name.end());
    }

    const uint64_t footer_offset = at;
    const uint32_t footer_crc = encoding::Crc32(footer.data(), footer.size());
    PutU32(footer, footer_crc);
    PutU64(footer, footer_offset);

    s = WriteFully(fd, footer.data(), footer.size(), path_);
    if (!s.ok()) return s;

    return meta;
}

}  // namespace quarry
