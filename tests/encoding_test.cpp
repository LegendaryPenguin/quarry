// Tests for the per-chunk encodings.
//
// The property under test everywhere here is that decoding reproduces the input
// exactly — every value, every null, and the distinction between a null and an
// empty string. An encoding that is merely close is worse than no encoding at
// all, because the loss is invisible until a query returns the wrong answer.

#include "../src/storage/encoding.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/types.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

// A column written as explicit optionals, so a test states exactly which slots
// are null and nothing is inferred from a sentinel value.
template <typename T>
struct Slot {
    bool null = false;
    T value{};
};

Vector MakeInt64(const std::vector<Slot<int64_t>>& slots) {
    Vector v(TypeId::kInt64);
    for (const auto& s : slots) {
        v.AppendInt64(s.value);
        if (s.null) v.nulls().SetNull(v.size() - 1);
    }
    return v;
}

Vector MakeDouble(const std::vector<Slot<double>>& slots) {
    Vector v(TypeId::kDouble);
    for (const auto& s : slots) {
        v.AppendDouble(s.value);
        if (s.null) v.nulls().SetNull(v.size() - 1);
    }
    return v;
}

Vector MakeBool(const std::vector<Slot<bool>>& slots) {
    Vector v(TypeId::kBoolean);
    for (const auto& s : slots) {
        v.AppendBoolean(s.value);
        if (s.null) v.nulls().SetNull(v.size() - 1);
    }
    return v;
}

Vector MakeVarchar(const std::vector<Slot<std::string>>& slots) {
    Vector v(TypeId::kVarchar);
    for (const auto& s : slots) {
        v.AppendString(s.value);
        if (s.null) v.nulls().SetNull(v.size() - 1);
    }
    return v;
}

// Compares both the values and the nullness of every slot. Comparing only the
// values would pass an encoding that dropped the null bitmap entirely.
void ExpectSameColumn(const Vector& want, const Vector& got, const char* what) {
    CHECK_EQ(want.size(), got.size());
    CHECK(want.type() == got.type());
    if (want.size() != got.size()) return;

    for (size_t i = 0; i < want.size(); ++i) {
        const bool wn = want.nulls().IsNull(i);
        const bool gn = got.nulls().IsNull(i);
        if (wn != gn) {
            std::string msg = std::string(what) + ": row " + std::to_string(i) + " nullness is " +
                              (gn ? "null" : "not null") + ", expected " +
                              (wn ? "null" : "not null");
            testing::ReportFailure(__FILE__, __LINE__, msg);
            continue;
        }
        switch (want.type()) {
            case TypeId::kBoolean:
                if (want.bool_data()[i] != got.bool_data()[i]) {
                    testing::ReportFailure(__FILE__, __LINE__,
                                           std::string(what) + ": boolean row " +
                                               std::to_string(i) + " differs");
                }
                break;
            case TypeId::kInt64:
                if (want.int64_data()[i] != got.int64_data()[i]) {
                    testing::ReportFailure(__FILE__, __LINE__,
                                           std::string(what) + ": int64 row " +
                                               std::to_string(i) + " is " +
                                               std::to_string(got.int64_data()[i]) + ", expected " +
                                               std::to_string(want.int64_data()[i]));
                }
                break;
            case TypeId::kDouble: {
                // Compared bitwise: a round trip that turned -0.0 into 0.0, or
                // one NaN payload into another, has not reproduced the input.
                const double a = want.double_data()[i], b = got.double_data()[i];
                if (std::memcmp(&a, &b, sizeof(double)) != 0) {
                    testing::ReportFailure(__FILE__, __LINE__,
                                           std::string(what) + ": double row " +
                                               std::to_string(i) + " differs bitwise");
                }
                break;
            }
            case TypeId::kVarchar:
                if (want.GetString(i) != got.GetString(i)) {
                    testing::ReportFailure(__FILE__, __LINE__,
                                           std::string(what) + ": varchar row " +
                                               std::to_string(i) + " is '" +
                                               std::string(got.GetString(i)) + "', expected '" +
                                               std::string(want.GetString(i)) + "'");
                }
                break;
            default:
                break;
        }
    }
}

void RoundTrip(const Vector& in, Encoding enc, const char* what) {
    std::vector<uint8_t> bytes;
    uint64_t raw_length = 0;
    CHECK_OK(encoding::Encode(in, in.size(), enc, bytes, raw_length));

    Encoding seen = Encoding::kPlain;
    CHECK_OK(encoding::PeekEncoding(bytes.data(), bytes.size(), seen));
    CHECK(seen == enc);

    Vector out(in.type());
    CHECK_OK(encoding::Decode(bytes.data(), bytes.size(), in.type(), out));
    ExpectSameColumn(in, out, what);
}

void RoundTripAllApplicable(const Vector& in, const char* what) {
    RoundTrip(in, Encoding::kPlain, what);
    RoundTrip(in, Encoding::kRunLength, what);
    if (in.type() == TypeId::kVarchar) RoundTrip(in, Encoding::kDictionary, what);
}

Encoding AutoChoice(const Vector& v) {
    return encoding::Choose(v.type(), encoding::Profile(v, v.size()));
}

}  // namespace

QUARRY_TEST(EncodingRoundTripInt64WithNulls) {
    const Vector v = MakeInt64({{false, 1},
                                {true, 0},
                                {false, -1},
                                {false, 9'000'000'000'000LL},
                                {true, 777},
                                {false, 0},
                                {false, INT64_MIN},
                                {false, INT64_MAX}});
    RoundTripAllApplicable(v, "int64 with nulls");
}

QUARRY_TEST(EncodingRoundTripDoubleWithNullsAndSpecials) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Vector v = MakeDouble({{false, 0.0},
                                 {false, -0.0},
                                 {true, 1.5},
                                 {false, 3.25},
                                 {false, -inf},
                                 {false, inf},
                                 {false, nan},
                                 {false, 1e-300}});
    RoundTripAllApplicable(v, "double with specials");
}

QUARRY_TEST(EncodingRoundTripBoolean) {
    const Vector v = MakeBool({{false, true},
                               {false, true},
                               {true, false},
                               {false, false},
                               {false, true},
                               {true, true}});
    RoundTripAllApplicable(v, "boolean");
}

QUARRY_TEST(EncodingRoundTripVarcharNullsAndEmpty) {
    // An empty string and a null occupy the same zero bytes of payload, so this is
    // the case where a lost null bitmap would be indistinguishable from a bug.
    const Vector v = MakeVarchar({{false, ""},
                                  {true, ""},
                                  {false, "a"},
                                  {false, ""},
                                  {true, "ignored"},
                                  {false, std::string("em\0bedded", 9)},
                                  {false, std::string(300, 'x')}});
    RoundTripAllApplicable(v, "varchar with nulls and empties");
}

QUARRY_TEST(EncodingRoundTripEmptyChunk) {
    RoundTripAllApplicable(MakeInt64({}), "empty int64");
    RoundTripAllApplicable(MakeVarchar({}), "empty varchar");
    CHECK(AutoChoice(MakeInt64({})) == Encoding::kPlain);
}

QUARRY_TEST(EncodingRoundTripSingleValueChunk) {
    RoundTripAllApplicable(MakeInt64({{false, 42}}), "single int64");
    RoundTripAllApplicable(MakeVarchar({{false, "only"}}), "single varchar");
    RoundTripAllApplicable(MakeBool({{false, true}}), "single boolean");
    RoundTripAllApplicable(MakeInt64({{true, 0}}), "single null");
}

QUARRY_TEST(EncodingRoundTripAllNullChunk) {
    std::vector<Slot<int64_t>> ints(100);
    for (auto& s : ints) s.null = true;
    RoundTripAllApplicable(MakeInt64(ints), "all-null int64");

    std::vector<Slot<std::string>> strs(100);
    for (auto& s : strs) s.null = true;
    const Vector v = MakeVarchar(strs);
    RoundTripAllApplicable(v, "all-null varchar");

    // An all-null column is one run of one placeholder value, so run-length wins
    // outright; the point of the assertion is that the choice is made from the
    // data and not from the type.
    CHECK(AutoChoice(v) == Encoding::kRunLength);
}

QUARRY_TEST(EncodingSelectionPicksDictionaryForLowCardinalityStrings) {
    // Four distinct cities over 400 rows, deliberately interleaved so there are no
    // runs for run-length to exploit. Dictionary is the only encoding that can win
    // here, which is exactly the case it exists for.
    const char* cities[] = {"seattle", "san francisco", "new york", "austin"};
    std::vector<Slot<std::string>> slots;
    for (int i = 0; i < 400; ++i) slots.push_back({false, cities[i % 4]});
    const Vector v = MakeVarchar(slots);

    CHECK(AutoChoice(v) == Encoding::kDictionary);
    RoundTripAllApplicable(v, "low-cardinality strings");

    // And the win is real, not just predicted.
    std::vector<uint8_t> dict_bytes, plain_bytes;
    uint64_t raw = 0;
    CHECK_OK(encoding::Encode(v, v.size(), Encoding::kDictionary, dict_bytes, raw));
    CHECK_OK(encoding::Encode(v, v.size(), Encoding::kPlain, plain_bytes, raw));
    CHECK(dict_bytes.size() < plain_bytes.size());
}

QUARRY_TEST(EncodingSelectionPicksRunLengthForLongRuns) {
    std::vector<Slot<int64_t>> slots;
    for (int run = 0; run < 20; ++run) {
        for (int i = 0; i < 100; ++i) slots.push_back({false, run});
    }
    const Vector v = MakeInt64(slots);

    CHECK(AutoChoice(v) == Encoding::kRunLength);
    RoundTripAllApplicable(v, "long int64 runs");

    std::vector<uint8_t> rle_bytes, plain_bytes;
    uint64_t raw = 0;
    CHECK_OK(encoding::Encode(v, v.size(), Encoding::kRunLength, rle_bytes, raw));
    CHECK_OK(encoding::Encode(v, v.size(), Encoding::kPlain, plain_bytes, raw));
    CHECK(rle_bytes.size() < plain_bytes.size());
}

QUARRY_TEST(EncodingSelectionAvoidsDictionaryWhenEveryValueIsDistinct) {
    std::vector<Slot<std::string>> slots;
    for (int i = 0; i < 500; ++i) slots.push_back({false, "row-" + std::to_string(i)});
    const Vector v = MakeVarchar(slots);

    CHECK(AutoChoice(v) != Encoding::kDictionary);
    CHECK(AutoChoice(v) == Encoding::kPlain);

    // Forced anyway, it must still decode correctly: the selection heuristic is an
    // optimization, never a correctness precondition.
    RoundTripAllApplicable(v, "all-distinct strings");
}

QUARRY_TEST(EncodingSelectionAvoidsRunLengthForAlternatingValues) {
    std::vector<Slot<int64_t>> slots;
    for (int i = 0; i < 500; ++i) slots.push_back({false, i % 2});
    const Vector v = MakeInt64(slots);

    // Average run length is 1, so run-length would double the size.
    CHECK(AutoChoice(v) == Encoding::kPlain);
}

QUARRY_TEST(EncodingDictionaryRejectedForFixedWidth) {
    const Vector v = MakeInt64({{false, 1}, {false, 1}});
    std::vector<uint8_t> bytes;
    uint64_t raw = 0;
    const Status s = encoding::Encode(v, v.size(), Encoding::kDictionary, bytes, raw);
    CHECK(!s.ok());
    CHECK(s.code() == Code::kInvalidArgument);
}

QUARRY_TEST(EncodingRawLengthIsThePlainBodySize) {
    const Vector ints = MakeInt64({{false, 1}, {false, 2}, {false, 3}});
    std::vector<uint8_t> bytes;
    uint64_t raw = 0;
    CHECK_OK(encoding::Encode(ints, ints.size(), Encoding::kRunLength, bytes, raw));
    CHECK_EQ(raw, 24u);

    const Vector strs = MakeVarchar({{false, "ab"}, {false, "cde"}});
    bytes.clear();
    CHECK_OK(encoding::Encode(strs, strs.size(), Encoding::kPlain, bytes, raw));
    // 4 bytes of total length, three offsets, five payload bytes.
    CHECK_EQ(raw, 4u + 12u + 5u);
}

QUARRY_TEST(EncodingDecodeRejectsTruncatedAndMislabeledBlobs) {
    const Vector v = MakeVarchar({{false, "alpha"}, {false, "beta"}, {false, "gamma"}});
    std::vector<uint8_t> bytes;
    uint64_t raw = 0;
    CHECK_OK(encoding::Encode(v, v.size(), Encoding::kPlain, bytes, raw));

    Vector out(TypeId::kVarchar);
    for (size_t cut = 1; cut < bytes.size(); ++cut) {
        const Status s = encoding::Decode(bytes.data(), bytes.size() - cut, TypeId::kVarchar, out);
        // A prefix must either be rejected or, if it happens to still describe a
        // consistent shorter body, decode to something self-consistent. It must
        // never read beyond what it was given, which is what the sanitizer build
        // of this loop is checking.
        if (s.ok()) CHECK(out.size() <= v.size());
    }

    // A blob decoded as the wrong type is a footer/payload disagreement and must
    // be reported, not reinterpreted.
    Vector wrong(TypeId::kInt64);
    const Status s = encoding::Decode(bytes.data(), bytes.size(), TypeId::kInt64, wrong);
    CHECK(!s.ok());
    CHECK(s.code() == Code::kCorruption);
}

QUARRY_TEST(EncodingRandomizedRoundTripAllEncodings) {
    std::mt19937_64 rng(0xC0FFEE);

    for (int iter = 0; iter < 300; ++iter) {
        const size_t n = rng() % 600;
        const int type_pick = static_cast<int>(rng() % 4);
        const int null_rate = static_cast<int>(rng() % 4);   // 0 means no nulls
        const int cardinality = 1 + static_cast<int>(rng() % 50);
        const int run_len = 1 + static_cast<int>(rng() % 30);

        auto is_null = [&] { return null_rate != 0 && (rng() % 4) < static_cast<uint64_t>(null_rate); };

        switch (type_pick) {
            case 0: {
                std::vector<Slot<int64_t>> slots;
                int64_t current = static_cast<int64_t>(rng());
                for (size_t i = 0; i < n; ++i) {
                    if (i % static_cast<size_t>(run_len) == 0) {
                        current = static_cast<int64_t>(rng() % static_cast<uint64_t>(cardinality));
                    }
                    slots.push_back({is_null(), current});
                }
                RoundTripAllApplicable(MakeInt64(slots), "random int64");
                break;
            }
            case 1: {
                std::vector<Slot<double>> slots;
                double current = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (i % static_cast<size_t>(run_len) == 0) {
                        current = static_cast<double>(rng() % 1000) / 8.0;
                    }
                    slots.push_back({is_null(), current});
                }
                RoundTripAllApplicable(MakeDouble(slots), "random double");
                break;
            }
            case 2: {
                std::vector<Slot<bool>> slots;
                bool current = false;
                for (size_t i = 0; i < n; ++i) {
                    if (i % static_cast<size_t>(run_len) == 0) current = (rng() & 1) != 0;
                    slots.push_back({is_null(), current});
                }
                RoundTripAllApplicable(MakeBool(slots), "random boolean");
                break;
            }
            default: {
                std::vector<Slot<std::string>> slots;
                std::string current;
                for (size_t i = 0; i < n; ++i) {
                    if (i % static_cast<size_t>(run_len) == 0) {
                        const uint64_t pick = rng() % static_cast<uint64_t>(cardinality);
                        // Every few values is the empty string, which is the one
                        // varchar value that shares a representation with a null.
                        current = (pick % 7 == 0) ? std::string()
                                                  : std::string(1 + pick % 11, 'a' + (pick % 26));
                    }
                    slots.push_back({is_null(), current});
                }
                RoundTripAllApplicable(MakeVarchar(slots), "random varchar");
                break;
            }
        }
    }
}

QUARRY_TEST(EncodingAutoSelectionAlwaysRoundTrips) {
    std::mt19937_64 rng(42);
    for (int iter = 0; iter < 200; ++iter) {
        const size_t n = rng() % 400;
        const int cardinality = 1 + static_cast<int>(rng() % 20);
        std::vector<Slot<std::string>> slots;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t pick = rng() % static_cast<uint64_t>(cardinality);
            slots.push_back({(rng() % 10) == 0, "v" + std::to_string(pick)});
        }
        const Vector v = MakeVarchar(slots);

        std::vector<uint8_t> bytes;
        Encoding chosen = Encoding::kPlain;
        uint64_t raw = 0;
        CHECK_OK(encoding::EncodeAuto(v, v.size(), bytes, chosen, raw));

        Encoding seen = Encoding::kPlain;
        CHECK_OK(encoding::PeekEncoding(bytes.data(), bytes.size(), seen));
        CHECK(seen == chosen);

        Vector out(TypeId::kVarchar);
        CHECK_OK(encoding::Decode(bytes.data(), bytes.size(), TypeId::kVarchar, out));
        ExpectSameColumn(v, out, "auto-selected varchar");
    }
}

QUARRY_TEST(EncodingNameCoversEveryEncoding) {
    CHECK(std::string(EncodingName(Encoding::kPlain)) == "plain");
    CHECK(std::string(EncodingName(Encoding::kDictionary)) == "dictionary");
    CHECK(std::string(EncodingName(Encoding::kRunLength)) == "rle");
}

QUARRY_TEST_MAIN()
