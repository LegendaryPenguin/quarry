#include "lsm_codec.hpp"

#include <cstring>

namespace quarry::lsm {
namespace {

// Tag values are part of the on-disk format: changing one silently reorders
// every existing SSTable, so they are spelled out rather than taken from TypeId.
constexpr uint8_t kTagNull = 0x00;
constexpr uint8_t kTagBoolean = 0x01;
constexpr uint8_t kTagInt64 = 0x02;
constexpr uint8_t kTagDouble = 0x03;
constexpr uint8_t kTagVarchar = 0x04;

void PutBigEndian64(std::string& dst, uint64_t u) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        dst.push_back(static_cast<char>((u >> shift) & 0xff));
    }
}

const uint32_t* Crc32Table() {
    static const uint32_t* table = [] {
        static uint32_t t[256];
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

std::string EncodeKey(const Value& v) {
    std::string out;
    if (v.is_null()) {
        out.push_back(static_cast<char>(kTagNull));
        return out;
    }
    switch (v.type()) {
        case TypeId::kBoolean:
            out.push_back(static_cast<char>(kTagBoolean));
            out.push_back(static_cast<char>(v.as_boolean() ? 1 : 0));
            return out;
        case TypeId::kInt64: {
            out.push_back(static_cast<char>(kTagInt64));
            uint64_t u = static_cast<uint64_t>(v.as_int64()) ^ 0x8000000000000000ull;
            PutBigEndian64(out, u);
            return out;
        }
        case TypeId::kDouble: {
            out.push_back(static_cast<char>(kTagDouble));
            double d = v.as_double();
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(bits));
            // Negative doubles have their magnitude ordering reversed under an
            // unsigned compare, so they are complemented wholesale; positives
            // just need their sign bit set to sort above all negatives.
            bits = (bits & 0x8000000000000000ull) ? ~bits : (bits | 0x8000000000000000ull);
            PutBigEndian64(out, bits);
            return out;
        }
        case TypeId::kVarchar: {
            out.push_back(static_cast<char>(kTagVarchar));
            out.append(v.as_varchar());
            return out;
        }
        default:
            out.push_back(static_cast<char>(kTagNull));
            return out;
    }
}

uint32_t Crc32(const void* data, size_t len) {
    const uint32_t* table = Crc32Table();
    const auto* p = static_cast<const unsigned char*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint64_t Hash64(std::string_view key) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : key) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

void PutVarint64(std::string& dst, uint64_t v) {
    while (v >= 0x80) {
        dst.push_back(static_cast<char>(static_cast<uint8_t>(v) | 0x80));
        v >>= 7;
    }
    dst.push_back(static_cast<char>(static_cast<uint8_t>(v)));
}

void PutFixed32(std::string& dst, uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        dst.push_back(static_cast<char>((v >> shift) & 0xff));
    }
}

void PutFixed64(std::string& dst, uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {
        dst.push_back(static_cast<char>((v >> shift) & 0xff));
    }
}

bool GetVarint64(std::string_view& src, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    size_t i = 0;
    while (i < src.size()) {
        auto byte = static_cast<uint8_t>(src[i++]);
        if (shift > 63) return false;
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            src.remove_prefix(i);
            out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool GetLengthPrefixed(std::string_view& src, std::string_view& out) {
    uint64_t len = 0;
    std::string_view cursor = src;
    if (!GetVarint64(cursor, len)) return false;
    if (len > cursor.size()) return false;
    out = cursor.substr(0, len);
    cursor.remove_prefix(len);
    src = cursor;
    return true;
}

uint32_t DecodeFixed32(const char* p) {
    // Decoded by shifts rather than a memcpy of the native word, so the on-disk
    // format stays little-endian regardless of the host's byte order.
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t DecodeFixed64(const char* p) {
    const auto* b = reinterpret_cast<const unsigned char*>(p);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<uint64_t>(b[i]);
    }
    return v;
}

}  // namespace quarry::lsm
