#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quarry/status.hpp"
#include "quarry/types.hpp"

namespace quarry::catfmt {

// The catalog's on-disk format, written explicitly field by field.
//
// Hand-rolled little-endian binary rather than anything derived from the C++
// struct layout. The catalog is the one file whose format has to stay readable
// across a rebuild of the engine: it names every segment file on disk, so a
// format that silently changed when a struct gained a field would turn an
// ordinary code change into unreadable data. Writing each field by name means
// the only way the format changes is deliberately, with kVersion bumped.
//
// Layout:
//   header (fixed 28 bytes)
//     char[8]  kMagic
//     u32      kVersion
//     u32      0  (reserved; a flags word that readers must see as zero)
//     u64      body length in bytes
//     u32      crc32 of the body
//   body
//     u64      next_segment_id
//     u32      table count
//     table* :
//       str                    name
//       u32 + column*          schema      (str name, u8 type, u8 nullable)
//       u32 + segment*         segments
//       u32 + index*           indexes
//     segment:
//       u64 id, u64 row_count, str path, u32 + chunk*
//     chunk:
//       u8 type, u8 encoding, u64 offset, u64 length, u64 raw_length, stats
//     stats:
//       u8 all_null, u64 null_count, u64 distinct_estimate, value min, value max
//     value:
//       u8 tag, then the payload the tag implies
//     str:
//       u32 length, then that many bytes
//
// The checksum covers the body only, and the body length is what bounds the
// read. A truncated file therefore fails on the length check before any field is
// interpreted, which is the difference between a reported corruption and a
// plausible-looking catalog with half its segments missing.

constexpr char kMagic[8] = {'Q', 'R', 'Y', 'C', 'A', 'T', 'L', 'G'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 28;

// Value tags are written rather than inferred from the column type, because a
// zone-map bound is legitimately NULL (an all-null segment) while its column is
// not, and because a tag that disagrees with the column type is a detectable
// corruption instead of a misread payload.
enum class ValueTag : uint8_t {
    kNull = 0,
    kBoolean = 1,
    kInt64 = 2,
    kDouble = 3,
    kVarchar = 4,
};

// Crc32 is the reflected IEEE polynomial, the same one the storage layer uses.
//
// Duplicated here on purpose: the catalog's checksum is part of the catalog's
// own on-disk contract, and sharing the storage codec's function would make a
// change in that layer able to invalidate every catalog already written.
uint32_t Crc32(const uint8_t* data, size_t len);

// --- writing ---

void PutU8(std::vector<uint8_t>& out, uint8_t v);
void PutU32(std::vector<uint8_t>& out, uint32_t v);
void PutU64(std::vector<uint8_t>& out, uint64_t v);
void PutString(std::vector<uint8_t>& out, const std::string& s);
void PutValue(std::vector<uint8_t>& out, const Value& v);

// --- reading ---
//
// Reader is a cursor over the body with a sticky failure. Every Get checks the
// remaining length, so a malformed file produces a kCorruption at the first bad
// field rather than reading past the buffer; the sticky flag means the caller can
// decode a whole record and check once instead of after every field.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool ok() const { return ok_; }
    size_t offset() const { return pos_; }
    size_t remaining() const { return len_ - pos_; }

    uint8_t GetU8();
    uint32_t GetU32();
    uint64_t GetU64();
    std::string GetString();
    Value GetValue();

    // Fail marks the cursor bad with a reason, so a semantic check (a type byte
    // that is not a TypeId, say) reports like a length failure does.
    void Fail(std::string why);

    const std::string& error() const { return error_; }

private:
    bool Want(size_t n);

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;
};

}  // namespace quarry::catfmt
