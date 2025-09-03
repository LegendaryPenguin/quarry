#include "encoding.hpp"

#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace quarry {

const char* EncodingName(Encoding e) {
    switch (e) {
        case Encoding::kPlain:      return "plain";
        case Encoding::kDictionary: return "dictionary";
        case Encoding::kRunLength:  return "rle";
    }
    return "unknown";
}

namespace encoding {
namespace {

// Every multi-byte field goes through these. Reinterpreting a byte pointer at an
// arbitrary offset is undefined behaviour the sanitizers will (correctly) flag,
// and an explicit little-endian form keeps a segment file readable on a machine
// with the other byte order.
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

void PutBytes(std::vector<uint8_t>& out, const void* src, size_t n) {
    const auto* b = static_cast<const uint8_t*>(src);
    out.insert(out.end(), b, b + n);
}

// A fixed-width value is moved as its stored bit pattern rather than as a typed
// value, so a null slot's placeholder bytes survive a round trip untouched.
void PutFixed(std::vector<uint8_t>& out, const Vector& v, TypeId type, size_t i) {
    switch (type) {
        case TypeId::kBoolean:
            out.push_back(v.bool_data()[i]);
            break;
        case TypeId::kInt64:
            PutU64(out, static_cast<uint64_t>(v.int64_data()[i]));
            break;
        case TypeId::kDouble: {
            uint64_t bits = 0;
            const double d = v.double_data()[i];
            std::memcpy(&bits, &d, sizeof(bits));
            PutU64(out, bits);
            break;
        }
        default:
            break;
    }
}

void AppendFixedFromBytes(Vector& out, TypeId type, const uint8_t* p) {
    switch (type) {
        case TypeId::kBoolean:
            out.AppendBoolean(p[0] != 0);
            break;
        case TypeId::kInt64:
            out.AppendInt64(static_cast<int64_t>(GetU64(p)));
            break;
        case TypeId::kDouble: {
            const uint64_t bits = GetU64(p);
            double d = 0;
            std::memcpy(&d, &bits, sizeof(d));
            out.AppendDouble(d);
            break;
        }
        default:
            break;
    }
}

bool FixedEqual(const Vector& v, TypeId type, size_t a, size_t b) {
    switch (type) {
        case TypeId::kBoolean: return v.bool_data()[a] == v.bool_data()[b];
        case TypeId::kInt64:   return v.int64_data()[a] == v.int64_data()[b];
        case TypeId::kDouble: {
            // Compared as bit patterns, not as doubles: a run of NaNs is a run as
            // far as storage is concerned, and -0.0 must not be folded into 0.0
            // because decoding has to reproduce the input exactly.
            uint64_t x = 0, y = 0;
            const double da = v.double_data()[a], db = v.double_data()[b];
            std::memcpy(&x, &da, sizeof(x));
            std::memcpy(&y, &db, sizeof(y));
            return x == y;
        }
        default: return false;
    }
}

uint64_t FixedKey(const Vector& v, TypeId type, size_t i) {
    switch (type) {
        case TypeId::kBoolean: return v.bool_data()[i];
        case TypeId::kInt64:   return static_cast<uint64_t>(v.int64_data()[i]);
        case TypeId::kDouble: {
            uint64_t bits = 0;
            const double d = v.double_data()[i];
            std::memcpy(&bits, &d, sizeof(bits));
            return bits;
        }
        default: return 0;
    }
}

size_t NullWords(size_t count) { return (count + 63) / 64; }

// WritePrefix emits the self-describing header plus the null bitmap, so every
// encoding shares one representation of nullness and only the body differs.
void WritePrefix(std::vector<uint8_t>& out, Encoding enc, TypeId type, const Vector& v,
                 size_t count) {
    const bool has_nulls = v.nulls().has_nulls();
    out.push_back(static_cast<uint8_t>(enc));
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(has_nulls ? kFlagHasNulls : 0);
    out.push_back(0);
    PutU32(out, static_cast<uint32_t>(count));

    if (!has_nulls) return;

    const size_t words = NullWords(count);
    std::vector<uint64_t> bits(words, 0);
    for (size_t i = 0; i < count; ++i) {
        if (v.nulls().IsNull(i)) bits[i >> 6] |= (1ULL << (i & 63));
    }
    for (uint64_t w : bits) PutU64(out, w);
}

struct Header {
    Encoding encoding = Encoding::kPlain;
    TypeId type = TypeId::kInvalid;
    bool has_nulls = false;
    size_t count = 0;
    size_t body_offset = 0;
};

Status ReadHeader(const uint8_t* data, size_t len, Header& h) {
    if (len < kPrefixBytes) {
        return Status::Corruption("encoded chunk shorter than its header");
    }
    const uint8_t enc = data[0];
    if (enc > static_cast<uint8_t>(Encoding::kRunLength)) {
        return Status::Corruption("unknown encoding tag " + std::to_string(enc));
    }
    const uint8_t type = data[1];
    if (type == 0 || type > static_cast<uint8_t>(TypeId::kVarchar)) {
        return Status::Corruption("unknown type tag " + std::to_string(type));
    }
    h.encoding = static_cast<Encoding>(enc);
    h.type = static_cast<TypeId>(type);
    h.has_nulls = (data[2] & kFlagHasNulls) != 0;
    h.count = GetU32(data + 4);
    h.body_offset = kPrefixBytes;
    if (h.has_nulls) {
        const size_t mask_bytes = NullWords(h.count) * 8;
        if (len < kPrefixBytes + mask_bytes) {
            return Status::Corruption("encoded chunk truncated inside its null bitmap");
        }
        h.body_offset += mask_bytes;
    }
    return Status::Ok();
}

// Applies the stored bitmap after the values are in place, so a null slot keeps
// the exact bytes it had on the way in.
void ApplyNulls(const uint8_t* data, const Header& h, Vector& out) {
    if (!h.has_nulls) return;
    const uint8_t* mask = data + kPrefixBytes;
    for (size_t i = 0; i < h.count; ++i) {
        const uint64_t word = GetU64(mask + (i >> 6) * 8);
        if ((word >> (i & 63)) & 1ULL) out.nulls().SetNull(i);
    }
}

size_t PlainBodySize(TypeId type, const ColumnProfile& p) {
    if (FixedWidth(type)) return p.count * TypeSize(type);
    return 4 + 4 * (p.count + 1) + p.total_string_bytes;
}

size_t RleBodySize(TypeId type, const ColumnProfile& p) {
    if (FixedWidth(type)) return 4 + p.run_count * (TypeSize(type) + 4);
    return 4 + p.run_count * 8 + p.run_string_bytes;
}

size_t DictBodySize(const ColumnProfile& p) {
    return 8 + 4 * (p.distinct_count + 1) + p.distinct_string_bytes + 4 * p.count;
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t len) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

ColumnProfile Profile(const Vector& v, size_t count) {
    ColumnProfile p;
    p.count = count;
    if (count == 0) return p;

    const TypeId type = v.type();

    if (type == TypeId::kVarchar) {
        std::unordered_set<std::string> distinct;
        std::string_view prev;
        for (size_t i = 0; i < count; ++i) {
            const std::string_view s = v.GetString(i);
            p.total_string_bytes += s.size();
            if (i == 0 || s != prev) {
                ++p.run_count;
                p.run_string_bytes += s.size();
            }
            prev = s;
            if (p.distinct_exact) {
                if (distinct.size() >= kDistinctTrackLimit) {
                    p.distinct_exact = false;
                } else if (distinct.insert(std::string(s)).second) {
                    p.distinct_string_bytes += s.size();
                }
            }
        }
        p.distinct_count = p.distinct_exact ? distinct.size() : count;
        return p;
    }

    std::unordered_set<uint64_t> distinct;
    for (size_t i = 0; i < count; ++i) {
        if (i == 0 || !FixedEqual(v, type, i, i - 1)) ++p.run_count;
        if (p.distinct_exact) {
            if (distinct.size() >= kDistinctTrackLimit) {
                p.distinct_exact = false;
            } else {
                distinct.insert(FixedKey(v, type, i));
            }
        }
    }
    p.distinct_count = p.distinct_exact ? distinct.size() : count;
    return p;
}

Encoding Choose(TypeId type, const ColumnProfile& p) {
    if (p.count == 0 || p.run_count == 0) return Encoding::kPlain;

    const double avg_run = static_cast<double>(p.count) / static_cast<double>(p.run_count);

    size_t best = PlainBodySize(type, p);
    Encoding choice = Encoding::kPlain;

    if (avg_run >= kMinAverageRunLength) {
        const size_t rle = RleBodySize(type, p);
        if (rle < best) {
            best = rle;
            choice = Encoding::kRunLength;
        }
    }

    // Dictionary applies to varchar only: for a fixed-width type the codes are as
    // wide as the values, so it can only lose.
    if (type == TypeId::kVarchar && p.distinct_exact &&
        static_cast<double>(p.distinct_count) <
            kDictionaryMaxCardinality * static_cast<double>(p.count)) {
        const size_t dict = DictBodySize(p);
        if (dict < best) {
            best = dict;
            choice = Encoding::kDictionary;
        }
    }

    return choice;
}

Status Encode(const Vector& v, size_t count, Encoding enc, std::vector<uint8_t>& out,
              uint64_t& raw_length) {
    const TypeId type = v.type();
    if (type == TypeId::kInvalid) {
        return Status::InvalidArgument("cannot encode a column of invalid type");
    }
    if (count > v.size()) {
        return Status::InvalidArgument("encode count exceeds column size");
    }
    if (enc == Encoding::kDictionary && type != TypeId::kVarchar) {
        return Status::InvalidArgument("dictionary encoding applies to varchar only");
    }

    {
        ColumnProfile sizing;
        sizing.count = count;
        if (type == TypeId::kVarchar) {
            for (size_t i = 0; i < count; ++i) sizing.total_string_bytes += v.GetString(i).size();
        }
        raw_length = PlainBodySize(type, sizing);
    }

    WritePrefix(out, enc, type, v, count);

    switch (enc) {
        case Encoding::kPlain: {
            if (FixedWidth(type)) {
                for (size_t i = 0; i < count; ++i) PutFixed(out, v, type, i);
            } else {
                // Offsets are written in full, including the leading zero, so a
                // decoder reads lengths as plain subtractions with no special case
                // for the first value.
                size_t total = 0;
                for (size_t i = 0; i < count; ++i) total += v.GetString(i).size();
                PutU32(out, static_cast<uint32_t>(total));
                uint32_t at = 0;
                PutU32(out, at);
                for (size_t i = 0; i < count; ++i) {
                    at += static_cast<uint32_t>(v.GetString(i).size());
                    PutU32(out, at);
                }
                for (size_t i = 0; i < count; ++i) {
                    const std::string_view s = v.GetString(i);
                    PutBytes(out, s.data(), s.size());
                }
            }
            break;
        }

        case Encoding::kRunLength: {
            const size_t run_count_at = out.size();
            PutU32(out, 0);
            uint32_t runs = 0;
            size_t i = 0;
            while (i < count) {
                size_t j = i + 1;
                if (type == TypeId::kVarchar) {
                    const std::string_view s = v.GetString(i);
                    while (j < count && v.GetString(j) == s) ++j;
                    PutU32(out, static_cast<uint32_t>(s.size()));
                    PutBytes(out, s.data(), s.size());
                    PutU32(out, static_cast<uint32_t>(j - i));
                } else {
                    while (j < count && FixedEqual(v, type, j, i)) ++j;
                    PutFixed(out, v, type, i);
                    PutU32(out, static_cast<uint32_t>(j - i));
                }
                ++runs;
                i = j;
            }
            const uint32_t n = runs;
            out[run_count_at + 0] = static_cast<uint8_t>(n);
            out[run_count_at + 1] = static_cast<uint8_t>(n >> 8);
            out[run_count_at + 2] = static_cast<uint8_t>(n >> 16);
            out[run_count_at + 3] = static_cast<uint8_t>(n >> 24);
            break;
        }

        case Encoding::kDictionary: {
            // Codes are resolved through a hash map rather than by searching the
            // dictionary vector, so building a chunk with a wide dictionary stays
            // linear instead of quadratic.
            std::unordered_map<std::string, uint32_t> code_of;
            std::vector<std::string> dict;
            std::vector<uint32_t> codes(count, 0);
            size_t dict_bytes = 0;
            for (size_t i = 0; i < count; ++i) {
                const std::string s(v.GetString(i));
                auto [it, inserted] = code_of.emplace(s, static_cast<uint32_t>(dict.size()));
                if (inserted) {
                    dict.push_back(s);
                    dict_bytes += s.size();
                }
                codes[i] = it->second;
            }

            PutU32(out, static_cast<uint32_t>(dict.size()));
            PutU32(out, static_cast<uint32_t>(dict_bytes));
            uint32_t at = 0;
            PutU32(out, at);
            for (const auto& s : dict) {
                at += static_cast<uint32_t>(s.size());
                PutU32(out, at);
            }
            for (const auto& s : dict) PutBytes(out, s.data(), s.size());
            for (uint32_t c : codes) PutU32(out, c);
            break;
        }
    }

    return Status::Ok();
}

Status EncodeAuto(const Vector& v, size_t count, std::vector<uint8_t>& out, Encoding& chosen,
                  uint64_t& raw_length) {
    const ColumnProfile p = Profile(v, count);
    chosen = Choose(v.type(), p);
    return Encode(v, count, chosen, out, raw_length);
}

Status PeekEncoding(const uint8_t* data, size_t len, Encoding& out) {
    Header h;
    QUARRY_TRY(ReadHeader(data, len, h));
    out = h.encoding;
    return Status::Ok();
}

Status Decode(const uint8_t* data, size_t len, TypeId expected_type, Vector& out) {
    Header h;
    QUARRY_TRY(ReadHeader(data, len, h));
    if (h.type != expected_type) {
        return Status::Corruption(std::string("chunk type ") + TypeName(h.type) +
                                 " does not match expected " + TypeName(expected_type));
    }
    if (out.type() != expected_type) {
        return Status::InvalidArgument("output vector type does not match chunk type");
    }

    out.Clear();
    out.Reserve(h.count);

    const uint8_t* body = data + h.body_offset;
    const size_t body_len = len - h.body_offset;

    switch (h.encoding) {
        case Encoding::kPlain: {
            if (FixedWidth(h.type)) {
                const size_t w = TypeSize(h.type);
                if (body_len < h.count * w) {
                    return Status::Corruption("plain chunk body truncated");
                }
                for (size_t i = 0; i < h.count; ++i) {
                    AppendFixedFromBytes(out, h.type, body + i * w);
                }
            } else {
                if (body_len < 4 + 4 * (h.count + 1)) {
                    return Status::Corruption("plain varchar chunk offset table truncated");
                }
                const uint32_t total = GetU32(body);
                const uint8_t* offs = body + 4;
                const uint8_t* bytes = offs + 4 * (h.count + 1);
                if (body_len < 4 + 4 * (h.count + 1) + total) {
                    return Status::Corruption("plain varchar chunk byte region truncated");
                }
                for (size_t i = 0; i < h.count; ++i) {
                    const uint32_t begin = GetU32(offs + 4 * i);
                    const uint32_t end = GetU32(offs + 4 * (i + 1));
                    if (end < begin || end > total) {
                        return Status::Corruption("plain varchar chunk has a bad offset pair");
                    }
                    out.AppendString(std::string_view(reinterpret_cast<const char*>(bytes) + begin,
                                                      end - begin));
                }
            }
            break;
        }

        case Encoding::kRunLength: {
            if (body_len < 4) return Status::Corruption("rle chunk missing run count");
            const uint32_t runs = GetU32(body);
            size_t at = 4;
            size_t emitted = 0;
            for (uint32_t r = 0; r < runs; ++r) {
                if (h.type == TypeId::kVarchar) {
                    if (body_len < at + 4) return Status::Corruption("rle run header truncated");
                    const uint32_t slen = GetU32(body + at);
                    at += 4;
                    if (body_len < at + slen + 4) {
                        return Status::Corruption("rle run payload truncated");
                    }
                    const auto* chars = reinterpret_cast<const char*>(body + at);
                    at += slen;
                    const uint32_t rl = GetU32(body + at);
                    at += 4;
                    if (emitted + rl > h.count) {
                        return Status::Corruption("rle runs describe more rows than the header");
                    }
                    for (uint32_t k = 0; k < rl; ++k) {
                        out.AppendString(std::string_view(chars, slen));
                    }
                    emitted += rl;
                } else {
                    const size_t w = TypeSize(h.type);
                    if (body_len < at + w + 4) {
                        return Status::Corruption("rle run payload truncated");
                    }
                    const uint8_t* valp = body + at;
                    const uint32_t rl = GetU32(body + at + w);
                    at += w + 4;
                    if (emitted + rl > h.count) {
                        return Status::Corruption("rle runs describe more rows than the header");
                    }
                    for (uint32_t k = 0; k < rl; ++k) {
                        AppendFixedFromBytes(out, h.type, valp);
                    }
                    emitted += rl;
                }
            }
            if (emitted != h.count) {
                return Status::Corruption("rle runs describe fewer rows than the header");
            }
            break;
        }

        case Encoding::kDictionary: {
            if (h.type != TypeId::kVarchar) {
                return Status::Corruption("dictionary encoding on a fixed-width column");
            }
            if (body_len < 8) return Status::Corruption("dictionary chunk header truncated");
            const uint32_t dict_count = GetU32(body);
            const uint32_t dict_bytes = GetU32(body + 4);
            const size_t offs_at = 8;
            const size_t need = offs_at + 4 * (static_cast<size_t>(dict_count) + 1) + dict_bytes +
                                4 * h.count;
            if (body_len < need) return Status::Corruption("dictionary chunk truncated");
            const uint8_t* offs = body + offs_at;
            const uint8_t* chars = offs + 4 * (static_cast<size_t>(dict_count) + 1);
            const uint8_t* codes = chars + dict_bytes;
            for (size_t i = 0; i < h.count; ++i) {
                const uint32_t code = GetU32(codes + 4 * i);
                if (code >= dict_count) {
                    return Status::Corruption("dictionary code out of range");
                }
                const uint32_t begin = GetU32(offs + 4 * code);
                const uint32_t end = GetU32(offs + 4 * (code + 1));
                if (end < begin || end > dict_bytes) {
                    return Status::Corruption("dictionary entry has a bad offset pair");
                }
                out.AppendString(
                    std::string_view(reinterpret_cast<const char*>(chars) + begin, end - begin));
            }
            break;
        }
    }

    out.SetSize(h.count);
    ApplyNulls(data, h, out);
    return Status::Ok();
}

}  // namespace encoding
}  // namespace quarry
