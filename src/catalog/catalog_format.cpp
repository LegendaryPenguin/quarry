#include "catalog_format.hpp"

#include <cstring>

namespace quarry::catfmt {

namespace {

// A generated table is faster, but a 256-entry static table is 1 KiB of data the
// catalog path touches once per save; the bitwise form keeps the constant and the
// polynomial in the same place where it can be read.
constexpr uint32_t kPoly = 0xEDB88320u;

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (kPoly & (~(crc & 1u) + 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void PutU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

void PutString(std::vector<uint8_t>& out, const std::string& s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void PutValue(std::vector<uint8_t>& out, const Value& v) {
    if (v.is_null()) {
        PutU8(out, static_cast<uint8_t>(ValueTag::kNull));
        return;
    }
    switch (v.type()) {
        case TypeId::kBoolean:
            PutU8(out, static_cast<uint8_t>(ValueTag::kBoolean));
            PutU8(out, v.as_boolean() ? 1u : 0u);
            break;
        case TypeId::kInt64:
            PutU8(out, static_cast<uint8_t>(ValueTag::kInt64));
            PutU64(out, static_cast<uint64_t>(v.as_int64()));
            break;
        case TypeId::kDouble: {
            // The bit pattern, not a decimal rendering. A zone-map bound written
            // as text and read back would compare differently from the value it
            // came from, and a bound that is off by one ulp in the wrong
            // direction prunes a segment that did contain matching rows.
            uint64_t bits = 0;
            const double d = v.as_double();
            std::memcpy(&bits, &d, sizeof(bits));
            PutU8(out, static_cast<uint8_t>(ValueTag::kDouble));
            PutU64(out, bits);
            break;
        }
        case TypeId::kVarchar:
            PutU8(out, static_cast<uint8_t>(ValueTag::kVarchar));
            PutString(out, v.as_varchar());
            break;
        default:
            PutU8(out, static_cast<uint8_t>(ValueTag::kNull));
            break;
    }
}

bool Reader::Want(size_t n) {
    if (!ok_) return false;
    if (len_ - pos_ < n) {
        Fail("truncated at offset " + std::to_string(pos_) + ", wanted " + std::to_string(n) +
             " byte(s)");
        return false;
    }
    return true;
}

void Reader::Fail(std::string why) {
    if (ok_) {
        ok_ = false;
        error_ = std::move(why);
    }
}

uint8_t Reader::GetU8() {
    if (!Want(1)) return 0;
    return data_[pos_++];
}

uint32_t Reader::GetU32() {
    if (!Want(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
}

uint64_t Reader::GetU64() {
    if (!Want(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
}

std::string Reader::GetString() {
    const uint32_t n = GetU32();
    if (!Want(n)) return {};
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
}

Value Reader::GetValue() {
    const uint8_t tag = GetU8();
    if (!ok_) return Value::Null();
    switch (static_cast<ValueTag>(tag)) {
        case ValueTag::kNull:
            return Value::Null();
        case ValueTag::kBoolean:
            return Value::Boolean(GetU8() != 0);
        case ValueTag::kInt64:
            return Value::Int64(static_cast<int64_t>(GetU64()));
        case ValueTag::kDouble: {
            const uint64_t bits = GetU64();
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            return Value::Double(d);
        }
        case ValueTag::kVarchar:
            return Value::Varchar(GetString());
    }
    Fail("unknown value tag " + std::to_string(tag) + " at offset " + std::to_string(pos_ - 1));
    return Value::Null();
}

}  // namespace quarry::catfmt
