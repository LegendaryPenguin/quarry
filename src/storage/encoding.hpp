#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"
#include "quarry/types.hpp"

namespace quarry {

// The encoding layer turns one whole column of a segment into a self-describing
// byte blob and back.
//
// Self-describing matters even though the footer already records the encoding:
// it means a decoder can be handed a blob with no side table and still be
// correct, and it means a footer that disagrees with its payload is detectable
// rather than silently misread as another encoding's layout.

namespace encoding {

// Fixed prefix written in front of every encoded chunk:
//   u8  encoding
//   u8  type
//   u8  flags (bit 0: a null bitmap follows the prefix)
//   u8  reserved, zero
//   u32 row count
constexpr size_t kPrefixBytes = 8;

constexpr uint8_t kFlagHasNulls = 1u << 0;

// Measured properties of a column, gathered in one pass so the encoding choice
// is made from the data rather than from the column's declared type.
struct ColumnProfile {
    size_t count = 0;

    // run_count is the number of maximal equal-adjacent-value runs. Nulls
    // participate using their stored placeholder value, which is safe because
    // the null bitmap is stored separately and restores nullness exactly.
    size_t run_count = 0;

    // distinct_count is exact up to kDistinctTrackLimit, past which tracking is
    // abandoned and the column is treated as fully distinct. A column with more
    // than a million distinct values is not a dictionary candidate anyway, so
    // the cap costs nothing in selection quality and bounds the memory a single
    // Finish() can take.
    size_t distinct_count = 0;
    bool distinct_exact = true;

    // Total bytes of string payload; zero for fixed-width types.
    size_t total_string_bytes = 0;

    // Bytes of string payload counting each run once, which is what a run-length
    // body would store.
    size_t run_string_bytes = 0;

    // Bytes of distinct string payload, which is the dictionary's size.
    size_t distinct_string_bytes = 0;
};

constexpr size_t kDistinctTrackLimit = 1u << 20;

// Thresholds for automatic selection. Both are deliberately conservative: a
// wrong choice is never incorrect, only larger or slower, so the bar for
// leaving kPlain is "clearly cheaper", not "possibly cheaper".
//
// kDictionaryMaxCardinality is a fraction of the row count. At 1/2, a column
// whose values repeat even twice on average becomes a candidate; the final
// decision still compares real encoded sizes, so this only gates the work of
// computing the dictionary size.
constexpr double kDictionaryMaxCardinality = 0.5;

// kMinAverageRunLength of 4 is where a run pair (value + u32 length) starts
// beating four packed values for the narrowest fixed-width type that matters
// here (8 bytes): 4 values = 32 bytes, one run = 12 bytes.
constexpr double kMinAverageRunLength = 4.0;

ColumnProfile Profile(const Vector& v, size_t count);

// Choose picks the encoding for a profiled column. Candidates are admitted by
// the thresholds above and then compared on exact predicted encoded size, so
// the answer is the smallest of the admissible layouts.
Encoding Choose(TypeId type, const ColumnProfile& profile);

// Encode writes v's first `count` rows into out, appending to whatever is there.
// raw_length receives the size the plain body would have occupied, which is what
// a reader uses to size a decode buffer.
Status Encode(const Vector& v, size_t count, Encoding enc, std::vector<uint8_t>& out,
              uint64_t& raw_length);

// EncodeAuto profiles, chooses, and encodes in one call, reporting the choice.
Status EncodeAuto(const Vector& v, size_t count, std::vector<uint8_t>& out, Encoding& chosen,
                  uint64_t& raw_length);

// Decode rebuilds a column from bytes produced by Encode. out is cleared first.
// A blob whose declared type disagrees with expected_type, or whose body is
// shorter than its own header claims, is reported as corruption rather than
// trusted.
Status Decode(const uint8_t* data, size_t len, TypeId expected_type, Vector& out);

// PeekEncoding reads the encoding byte without decoding, so a caller can check
// the footer against the payload.
Status PeekEncoding(const uint8_t* data, size_t len, Encoding& out);

// Crc32 is the IEEE 802.3 polynomial, used to checksum each chunk and the
// footer. It is not a security property: it exists to turn a bit flip or a
// partial write into a kCorruption instead of a plausible-looking wrong answer.
uint32_t Crc32(const uint8_t* data, size_t len);

}  // namespace encoding
}  // namespace quarry
