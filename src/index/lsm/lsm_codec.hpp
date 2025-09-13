#pragma once

// Byte-level primitives shared by the memtable, the SSTable and the manifest.
//
// The LSM works on encoded keys rather than on Value, because every layer below
// the Index interface has to compare keys that it has only as bytes: a sparse
// index entry read off disk, a Bloom filter probe, the k-way merge across
// levels. Encoding once at the edge means exactly one comparison function
// (memcmp) is trusted for ordering everywhere, instead of each layer rebuilding
// a typed comparison and drifting from the others.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "quarry/types.hpp"

namespace quarry::lsm {

// EncodeKey produces bytes whose lexicographic order matches Value::operator<
// within a single type.
//
// The leading tag byte means keys of different types never interleave, so an
// index holding one key type (the only way the engine uses it) sorts exactly as
// the typed comparison would. Across types the order is by tag, which is
// arbitrary but stable — and stability is what matters, since a range bound of a
// different type than the stored keys has no defined answer anyway.
//
// Int64 is stored sign-flipped big-endian so that negative keys sort below
// positive ones under memcmp; a raw two's-complement byte copy would put -1
// above everything. Double gets the equivalent IEEE-754 transform.
std::string EncodeKey(const Value& v);

// Crc32 is the standard reflected CRC-32 (the zlib/IEEE polynomial). Written
// here rather than taken from zlib so the engine keeps its no-dependency rule.
uint32_t Crc32(const void* data, size_t len);

// Hash64 is the single hash the Bloom filter derives all k probes from, via the
// two-hash technique. FNV-1a mixes the bytes and a splitmix64 finalizer spreads
// them, because FNV alone has weak high bits and the two-hash scheme takes its
// second hash from the high half.
uint64_t Hash64(std::string_view key);

void PutVarint64(std::string& dst, uint64_t v);
void PutFixed32(std::string& dst, uint32_t v);
void PutFixed64(std::string& dst, uint64_t v);

// GetVarint64 advances src past the value it decoded. Returns false on a
// truncated or over-long encoding, which a caller must surface as corruption.
bool GetVarint64(std::string_view& src, uint64_t& out);
bool GetLengthPrefixed(std::string_view& src, std::string_view& out);

uint32_t DecodeFixed32(const char* p);
uint64_t DecodeFixed64(const char* p);

}  // namespace quarry::lsm
