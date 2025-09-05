#include "segment_reader.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>

#include "encoding.hpp"
#include "segment_writer.hpp"

namespace quarry {
namespace {

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// Cursor is a bounds-checked walk over a buffer. Every footer field goes through
// it, because a footer is attacker-shaped data as far as this code is concerned:
// one unchecked length is the difference between kCorruption and a read past the
// end of the buffer.
class Cursor {
public:
    Cursor(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool Take(size_t n, const uint8_t** out) {
        if (len_ - at_ < n) return false;
        *out = data_ + at_;
        at_ += n;
        return true;
    }

    bool U8(uint8_t& out) {
        const uint8_t* p = nullptr;
        if (!Take(1, &p)) return false;
        out = *p;
        return true;
    }

    bool U32(uint32_t& out) {
        const uint8_t* p = nullptr;
        if (!Take(4, &p)) return false;
        out = GetU32(p);
        return true;
    }

    bool U64(uint64_t& out) {
        const uint8_t* p = nullptr;
        if (!Take(8, &p)) return false;
        out = GetU64(p);
        return true;
    }

    size_t remaining() const { return len_ - at_; }

private:
    const uint8_t* data_;
    size_t len_;
    size_t at_ = 0;
};

// Counterpart of PutValue in segment_writer.cpp.
bool ReadValue(Cursor& c, Value& out) {
    uint8_t tag = 0;
    if (!c.U8(tag)) return false;
    switch (tag) {
        case 0:
            out = Value::Null();
            return true;
        case 1: {
            uint8_t b = 0;
            if (!c.U8(b)) return false;
            out = Value::Boolean(b != 0);
            return true;
        }
        case 2: {
            uint64_t v = 0;
            if (!c.U64(v)) return false;
            out = Value::Int64(static_cast<int64_t>(v));
            return true;
        }
        case 3: {
            uint64_t bits = 0;
            if (!c.U64(bits)) return false;
            double d = 0;
            std::memcpy(&d, &bits, sizeof(d));
            out = Value::Double(d);
            return true;
        }
        case 4: {
            uint32_t len = 0;
            if (!c.U32(len)) return false;
            const uint8_t* p = nullptr;
            if (!c.Take(len, &p)) return false;
            out = Value::Varchar(std::string(reinterpret_cast<const char*>(p), len));
            return true;
        }
        default:
            return false;
    }
}

Status ReadAt(int fd, uint64_t offset, uint8_t* dst, size_t len, const std::string& path) {
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pread(fd, dst + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IoError("read from " + path + " failed: " + std::strerror(errno));
        }
        if (n == 0) {
            return Status::Corruption("segment " + path + " ends before offset " +
                                      std::to_string(offset + done));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

struct FdGuard {
    int fd = -1;
    ~FdGuard() { if (fd >= 0) ::close(fd); }
};

// ParseFooter mirrors the layout written at the end of FileSegmentWriter::Finish.
Status ParseFooter(const uint8_t* data, size_t len, uint32_t declared_columns, SegmentFile& out) {
    Cursor c(data, len);
    const uint8_t* magic = nullptr;
    if (!c.Take(sizeof(segfmt::kFooterMagic), &magic) ||
        std::memcmp(magic, segfmt::kFooterMagic, sizeof(segfmt::kFooterMagic)) != 0) {
        return Status::Corruption("segment footer magic is wrong");
    }
    uint32_t columns = 0;
    if (!c.U32(columns)) return Status::Corruption("segment footer is truncated");
    if (columns != declared_columns) {
        return Status::Corruption("segment footer declares " + std::to_string(columns) +
                                  " columns but the header declares " +
                                  std::to_string(declared_columns));
    }

    std::vector<Column> schema_columns;
    for (uint32_t i = 0; i < columns; ++i) {
        uint8_t type = 0, enc = 0, all_null = 0, pad = 0;
        if (!c.U8(type) || !c.U8(enc) || !c.U8(all_null) || !c.U8(pad)) {
            return Status::Corruption("segment footer is truncated in a column entry");
        }
        if (type == 0 || type > static_cast<uint8_t>(TypeId::kVarchar)) {
            return Status::Corruption("segment footer has an unknown type tag");
        }
        if (enc > static_cast<uint8_t>(Encoding::kRunLength)) {
            return Status::Corruption("segment footer has an unknown encoding tag");
        }

        ColumnChunk chunk;
        chunk.type = static_cast<TypeId>(type);
        chunk.encoding = static_cast<Encoding>(enc);
        chunk.stats.all_null = all_null != 0;
        if (!c.U64(chunk.offset) || !c.U64(chunk.length) || !c.U64(chunk.raw_length) ||
            !c.U64(chunk.stats.null_count) || !c.U64(chunk.stats.distinct_estimate)) {
            return Status::Corruption("segment footer is truncated in a column entry");
        }
        if (!ReadValue(c, chunk.stats.min) || !ReadValue(c, chunk.stats.max)) {
            return Status::Corruption("segment footer is truncated in a statistics bound");
        }

        uint32_t name_len = 0;
        const uint8_t* name = nullptr;
        if (!c.U32(name_len) || !c.Take(name_len, &name)) {
            return Status::Corruption("segment footer is truncated in a column name");
        }
        schema_columns.emplace_back(std::string(reinterpret_cast<const char*>(name), name_len),
                                    chunk.type);
        out.meta.columns.push_back(std::move(chunk));
    }

    out.schema = Schema(std::move(schema_columns));
    return Status::Ok();
}

}  // namespace

Result<SegmentFile> ReadSegmentFile(const std::string& path) {
    FdGuard guard;
    guard.fd = ::open(path.c_str(), O_RDONLY);
    if (guard.fd < 0) {
        return Status::IoError("cannot open segment " + path + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (::fstat(guard.fd, &st) != 0) {
        return Status::IoError("cannot stat segment " + path + ": " + std::strerror(errno));
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    if (size < segfmt::kHeaderBytes + segfmt::kFooterTrailerBytes) {
        return Status::Corruption("segment " + path + " is too short to hold a header and footer");
    }

    uint8_t header[segfmt::kHeaderBytes];
    QUARRY_TRY(ReadAt(guard.fd, 0, header, sizeof(header), path));
    if (std::memcmp(header, segfmt::kMagic, sizeof(segfmt::kMagic)) != 0) {
        return Status::Corruption("segment " + path + " does not start with the segment magic");
    }
    const uint32_t version = GetU32(header + 8);
    if (version != segfmt::kVersion) {
        return Status::Corruption("segment " + path + " has version " + std::to_string(version) +
                                  ", this build reads version " +
                                  std::to_string(segfmt::kVersion));
    }
    const uint32_t column_count = GetU32(header + 12);

    // The footer offset lives in the last eight bytes, so metadata costs one tail
    // read plus one seek no matter how large the segment is.
    uint8_t tail[8];
    QUARRY_TRY(ReadAt(guard.fd, size - 8, tail, sizeof(tail), path));
    const uint64_t footer_offset = GetU64(tail);
    if (footer_offset < segfmt::kHeaderBytes || footer_offset > size - segfmt::kFooterTrailerBytes) {
        return Status::Corruption("segment " + path + " has a footer offset of " +
                                  std::to_string(footer_offset) + " outside a file of " +
                                  std::to_string(size) + " bytes");
    }

    const uint64_t footer_len = size - segfmt::kFooterTrailerBytes - footer_offset;
    std::vector<uint8_t> footer(static_cast<size_t>(footer_len));
    if (footer_len > 0) {
        QUARRY_TRY(ReadAt(guard.fd, footer_offset, footer.data(), footer.size(), path));
    }
    uint8_t crc_bytes[4];
    QUARRY_TRY(ReadAt(guard.fd, size - segfmt::kFooterTrailerBytes, crc_bytes, sizeof(crc_bytes),
                      path));
    const uint32_t want_crc = GetU32(crc_bytes);
    const uint32_t have_crc = encoding::Crc32(footer.data(), footer.size());
    if (want_crc != have_crc) {
        return Status::Corruption("segment " + path + " footer checksum mismatch");
    }

    SegmentFile out;
    out.meta.path = path;
    out.meta.row_count = GetU64(header + 16);
    out.meta.id = GetU64(header + 24);
    QUARRY_TRY(ParseFooter(footer.data(), footer.size(), column_count, out));

    // Chunk ranges are validated here rather than at scan time so that a truncated
    // file is rejected by the call that produces the metadata, before any planner
    // has a chance to believe it.
    for (size_t i = 0; i < out.meta.columns.size(); ++i) {
        const ColumnChunk& chunk = out.meta.columns[i];
        if (chunk.length < segfmt::kChunkChecksumBytes ||
            chunk.offset < segfmt::kHeaderBytes ||
            chunk.offset > footer_offset ||
            chunk.length > footer_offset - chunk.offset) {
            return Status::Corruption("segment " + path + " column " + std::to_string(i) +
                                      " has a chunk range outside the data region");
        }
    }

    return out;
}

FileSegmentReader::FileSegmentReader(SegmentMeta meta, std::vector<size_t> projection,
                                     Schema schema)
    : meta_(std::move(meta)), projection_(std::move(projection)) {
    std::vector<Column> out;
    out.reserve(projection_.size());
    for (size_t idx : projection_) {
        if (idx >= meta_.columns.size()) continue;
        if (idx < schema.size()) {
            out.push_back(schema.at(idx));
        } else {
            out.emplace_back("c" + std::to_string(idx), meta_.columns[idx].type);
        }
    }
    output_schema_ = Schema(std::move(out));
}

FileSegmentReader::~FileSegmentReader() = default;

Status FileSegmentReader::Open() {
    if (open_) return Status::Internal("Open called twice on the same segment reader");

    for (size_t idx : projection_) {
        if (idx >= meta_.columns.size()) {
            return Status::InvalidArgument("projected column " + std::to_string(idx) +
                                           " does not exist in segment " +
                                           std::to_string(meta_.id));
        }
    }

    FdGuard guard;
    guard.fd = ::open(meta_.path.c_str(), O_RDONLY);
    if (guard.fd < 0) {
        return Status::IoError("cannot open segment " + meta_.path + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (::fstat(guard.fd, &st) != 0) {
        return Status::IoError("cannot stat segment " + meta_.path + ": " + std::strerror(errno));
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);

    // The header is the one thing read unconditionally. It is 32 bytes and it is
    // what turns "someone handed me stale metadata for a file that has since been
    // replaced" into an error instead of garbage rows.
    if (size < segfmt::kHeaderBytes) {
        return Status::Corruption("segment " + meta_.path + " is shorter than its header");
    }
    uint8_t header[segfmt::kHeaderBytes];
    QUARRY_TRY(ReadAt(guard.fd, 0, header, sizeof(header), meta_.path));
    bytes_read_ += sizeof(header);

    if (std::memcmp(header, segfmt::kMagic, sizeof(segfmt::kMagic)) != 0) {
        return Status::Corruption("segment " + meta_.path + " does not start with the segment magic");
    }
    if (GetU32(header + 8) != segfmt::kVersion) {
        return Status::Corruption("segment " + meta_.path + " has an unsupported version");
    }
    if (GetU32(header + 12) != meta_.columns.size()) {
        return Status::Corruption("segment " + meta_.path + " column count disagrees with metadata");
    }
    if (GetU64(header + 16) != meta_.row_count) {
        return Status::Corruption("segment " + meta_.path + " row count disagrees with metadata");
    }

    decoded_.clear();
    decoded_.reserve(projection_.size());

    for (size_t idx : projection_) {
        const ColumnChunk& chunk = meta_.columns[idx];
        if (chunk.length < segfmt::kChunkChecksumBytes ||
            chunk.offset > size ||
            chunk.length > size - chunk.offset) {
            return Status::Corruption("segment " + meta_.path + " column " + std::to_string(idx) +
                                      " chunk runs past the end of the file");
        }

        // One contiguous read of exactly this column's bytes. Nothing belonging to
        // an unprojected column is ever in this buffer.
        std::vector<uint8_t> buf(static_cast<size_t>(chunk.length));
        QUARRY_TRY(ReadAt(guard.fd, chunk.offset, buf.data(), buf.size(), meta_.path));
        bytes_read_ += chunk.length;

        const size_t payload_len = buf.size() - segfmt::kChunkChecksumBytes;
        const uint32_t want = GetU32(buf.data() + payload_len);
        const uint32_t have = encoding::Crc32(buf.data(), payload_len);
        if (want != have) {
            return Status::Corruption("segment " + meta_.path + " column " + std::to_string(idx) +
                                      " failed its checksum");
        }

        Encoding payload_encoding = Encoding::kPlain;
        QUARRY_TRY(encoding::PeekEncoding(buf.data(), payload_len, payload_encoding));
        if (payload_encoding != chunk.encoding) {
            return Status::Corruption("segment " + meta_.path + " column " + std::to_string(idx) +
                                      " is encoded as " + EncodingName(payload_encoding) +
                                      " but the footer says " + EncodingName(chunk.encoding));
        }

        decoded_.emplace_back(chunk.type);
        QUARRY_TRY(encoding::Decode(buf.data(), payload_len, chunk.type, decoded_.back()));
        if (decoded_.back().size() != meta_.row_count) {
            return Status::Corruption("segment " + meta_.path + " column " + std::to_string(idx) +
                                      " decoded " + std::to_string(decoded_.back().size()) +
                                      " rows, metadata says " + std::to_string(meta_.row_count));
        }
    }

    position_ = 0;
    open_ = true;
    return Status::Ok();
}

Status FileSegmentReader::Next(Batch& out, bool& done) {
    if (!open_) return Status::Internal("Next called before Open on a segment reader");

    if (out.column_count() == 0) {
        for (const auto& col : output_schema_.columns()) out.AddColumn(col);
    } else if (out.column_count() != output_schema_.size()) {
        return Status::InvalidArgument("output batch has " + std::to_string(out.column_count()) +
                                      " columns, the projection has " +
                                      std::to_string(output_schema_.size()));
    }

    out.Reset();
    if (position_ >= meta_.row_count) {
        done = true;
        out.SetRowCount(0);
        return Status::Ok();
    }

    const size_t n =
        static_cast<size_t>(std::min<uint64_t>(kBatchSize, meta_.row_count - position_));

    for (size_t c = 0; c < decoded_.size(); ++c) {
        const Vector& src = decoded_[c];
        Vector& dst = out.column(c);
        if (dst.type() != src.type()) {
            return Status::TypeError(std::string("output column ") + std::to_string(c) + " is " +
                                     TypeName(dst.type()) + " but the chunk is " +
                                     TypeName(src.type()));
        }
        for (size_t i = 0; i < n; ++i) {
            const size_t r = static_cast<size_t>(position_) + i;
            switch (src.type()) {
                case TypeId::kBoolean: dst.AppendBoolean(src.bool_data()[r] != 0); break;
                case TypeId::kInt64:   dst.AppendInt64(src.int64_data()[r]); break;
                case TypeId::kDouble:  dst.AppendDouble(src.double_data()[r]); break;
                case TypeId::kVarchar: dst.AppendString(src.GetString(r)); break;
                default: return Status::Internal("chunk has an undecodable type");
            }
            if (src.nulls().IsNull(r)) dst.nulls().SetNull(i);
        }
    }

    out.SetRowCount(n);
    position_ += n;
    done = false;
    return Status::Ok();
}

Status FileSegmentReader::Close() {
    // The file descriptor is already gone: Open reads what it needs and closes,
    // because a scan that holds a descriptor open per segment runs out of them long
    // before it runs out of memory.
    decoded_.clear();
    open_ = false;
    return Status::Ok();
}

}  // namespace quarry
