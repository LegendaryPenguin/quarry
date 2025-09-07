// Tests for CanMatch, the zone-map pruning decision.
//
// The asymmetry here is the whole point. CanMatch returning true when nothing
// matches costs a wasted read. CanMatch returning false when something matches
// deletes rows from a query result with no error, no log line, and no way for the
// caller to notice. So the table-driven cases below pin down the exact answer for
// every operator, and the randomized tests check the one property that must never
// break: false implies a brute-force scan finds nothing.

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "../src/storage/segment_writer.hpp"
#include "quarry/batch.hpp"
#include "quarry/storage.hpp"
#include "quarry/types.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

const CompareOp kAllOps[] = {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt,
                            CompareOp::kLe, CompareOp::kGt, CompareOp::kGe};

ColumnStats Window(Value min, Value max, uint64_t null_count = 0) {
    ColumnStats s;
    s.min = std::move(min);
    s.max = std::move(max);
    s.null_count = null_count;
    return s;
}

ColumnStats IntWindow(int64_t lo, int64_t hi, uint64_t null_count = 0) {
    return Window(Value::Int64(lo), Value::Int64(hi), null_count);
}

ColumnStats AllNull(uint64_t rows) {
    ColumnStats s;
    s.null_count = rows;
    s.all_null = true;
    return s;
}

void ExpectCanMatch(const ColumnStats& stats, CompareOp op, const Value& constant, bool want,
                    const char* what, int line) {
    const bool got = CanMatch(stats, op, constant);
    if (got != want) {
        testing::ReportFailure(__FILE__, line,
                               std::string(what) + ": CanMatch([" + stats.min.ToString() + ", " +
                                   stats.max.ToString() + "] nulls=" +
                                   std::to_string(stats.null_count) + ", " + CompareOpName(op) +
                                   " " + constant.ToString() + ") returned " +
                                   (got ? "true" : "false") + ", expected " +
                                   (want ? "true" : "false"));
    }
}

#define EXPECT_MATCH(stats, op, constant, want, what) \
    ExpectCanMatch(stats, op, constant, want, what, __LINE__)

// The reference answer: does any single value satisfy the predicate under SQL
// semantics? Null values never satisfy a comparison, and a null constant makes
// every comparison unknown.
bool RowMatches(const Value& value, CompareOp op, const Value& constant) {
    if (value.is_null() || constant.is_null()) return false;
    switch (op) {
        case CompareOp::kEq: return value == constant;
        case CompareOp::kNe: return !(value == constant);
        case CompareOp::kLt: return value < constant;
        case CompareOp::kLe: return value < constant || value == constant;
        case CompareOp::kGt: return constant < value;
        case CompareOp::kGe: return constant < value || value == constant;
    }
    return false;
}

size_t CountMatches(const std::vector<Value>& values, CompareOp op, const Value& constant) {
    size_t n = 0;
    for (const auto& v : values) {
        if (RowMatches(v, op, constant)) ++n;
    }
    return n;
}

// StatsFor mirrors what a writer records: bounds over the non-null values only,
// NaN excluded because it cannot satisfy any comparison, and all_null tracked
// separately because the bounds are meaningless then.
ColumnStats StatsFor(const std::vector<Value>& values) {
    ColumnStats s;
    for (const auto& v : values) {
        if (v.is_null()) {
            ++s.null_count;
            continue;
        }
        if (v.type() == TypeId::kDouble && std::isnan(v.as_double())) continue;
        if (s.min.is_null() || v < s.min) s.min = v;
        if (s.max.is_null() || s.max < v) s.max = v;
    }
    s.all_null = !values.empty() && s.null_count == values.size();
    return s;
}

std::string TempPath(const char* stem) {
    return std::string("/tmp/quarry_zonemap_") + stem + "_" + std::to_string(::getpid()) + ".seg";
}

}  // namespace

QUARRY_TEST(ZoneMapIntWindowEveryOperator) {
    const ColumnStats s = IntWindow(10, 20);

    // Below the window.
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(5), false, "= below");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(5), true, "<> below");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(5), false, "< below");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(5), false, "<= below");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(5), true, "> below");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(5), true, ">= below");

    // Exactly at min.
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(10), true, "= at min");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(10), true, "<> at min");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(10), false, "< at min");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(10), true, "<= at min");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(10), true, "> at min");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(10), true, ">= at min");

    // Inside.
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(15), true, "= inside");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(15), true, "<> inside");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(15), true, "< inside");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(15), true, "<= inside");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(15), true, "> inside");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(15), true, ">= inside");

    // Exactly at max.
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(20), true, "= at max");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(20), true, "<> at max");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(20), true, "< at max");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(20), true, "<= at max");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(20), false, "> at max");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(20), true, ">= at max");

    // Above the window.
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(25), false, "= above");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(25), true, "<> above");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(25), true, "< above");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(25), true, "<= above");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(25), false, "> above");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(25), false, ">= above");
}

QUARRY_TEST(ZoneMapPointWindowIsTheOnlyCaseThatPrunesNotEqual) {
    const ColumnStats s = IntWindow(7, 7);

    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(7), false, "<> against a collapsed window");
    EXPECT_MATCH(s, CompareOp::kNe, Value::Int64(8), true, "<> against a different constant");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(7), true, "= against a collapsed window");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Int64(8), false, "= off a collapsed window");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(7), false, "< at the single value");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(7), false, "> at the single value");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(7), true, "<= at the single value");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(7), true, ">= at the single value");

    // A one-value window is still not enough to prune <> when the window is wider
    // than a point, so widening by one must flip the answer back.
    const ColumnStats wide = IntWindow(7, 8);
    EXPECT_MATCH(wide, CompareOp::kNe, Value::Int64(7), true, "<> against a two-value window");
}

QUARRY_TEST(ZoneMapNeverPrunesAChunkContainingNulls) {
    // Every operator, every constant position: a chunk with even one null is read.
    const ColumnStats s = IntWindow(10, 20, /*null_count=*/1);
    for (CompareOp op : kAllOps) {
        for (int64_t c : {-1000, 0, 5, 10, 15, 20, 25, 1000}) {
            EXPECT_MATCH(s, op, Value::Int64(c), true, "nullable chunk");
        }
    }
}

QUARRY_TEST(ZoneMapPrunesAllNullChunksForEveryOperator) {
    // An all-null chunk is the one case where a chunk with nulls is prunable: no
    // comparison is ever true, so there is nothing to lose.
    const ColumnStats s = AllNull(500);
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(s, op, Value::Int64(0), false, "all-null chunk");
        EXPECT_MATCH(s, op, Value::Varchar("anything"), false, "all-null chunk, varchar constant");
        EXPECT_MATCH(s, op, Value::Null(), false, "all-null chunk, null constant");
    }
}

QUARRY_TEST(ZoneMapIgnoresStaleBoundsOnAnAllNullChunk) {
    // A writer that forgot to clear the bounds must not be able to cause a prune
    // that depends on them: all_null is checked first and the bounds are not read.
    ColumnStats s = IntWindow(100, 200, 50);
    s.all_null = true;
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(s, op, Value::Int64(150), false, "all-null with stale bounds");
    }
}

QUARRY_TEST(ZoneMapNeverPrunesOnANullConstant) {
    const ColumnStats s = IntWindow(10, 20);
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(s, op, Value::Null(), true, "null constant");
    }
}

QUARRY_TEST(ZoneMapNeverPrunesWhenBoundsAreMissing) {
    // An empty chunk, or stats from a source that did not compute bounds. Nothing
    // is known, so nothing may be skipped.
    ColumnStats s;
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(s, op, Value::Int64(0), true, "no bounds at all");
    }

    ColumnStats half = Window(Value::Int64(5), Value::Null());
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(half, op, Value::Int64(0), true, "max missing");
    }

    ColumnStats other_half = Window(Value::Null(), Value::Int64(5));
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(other_half, op, Value::Int64(0), true, "min missing");
    }
}

QUARRY_TEST(ZoneMapNeverPrunesOnIncomparableTypes) {
    const ColumnStats s = Window(Value::Varchar("alpha"), Value::Varchar("omega"));
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(s, op, Value::Int64(5), true, "varchar bounds, int constant");
        EXPECT_MATCH(s, op, Value::Boolean(true), true, "varchar bounds, boolean constant");
    }

    const ColumnStats ints = IntWindow(10, 20);
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(ints, op, Value::Varchar("15"), true, "int bounds, varchar constant");
        EXPECT_MATCH(ints, op, Value::Boolean(false), true, "int bounds, boolean constant");
    }
}

QUARRY_TEST(ZoneMapCoercesIntegerBoundsAgainstADoubleConstant) {
    // The one implicit conversion the engine performs has to work here too, or a
    // `WHERE n > 10.5` would stop pruning on an integer column.
    const ColumnStats s = IntWindow(10, 20);
    EXPECT_MATCH(s, CompareOp::kGt, Value::Double(20.5), false, "> above an int window");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Double(19.5), true, "> inside an int window");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Double(9.5), false, "< below an int window");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Double(15.0), true, "= on an exact int value");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Double(15.5), true,
                 "= on a fractional constant inside the window");

    const ColumnStats d = Window(Value::Double(1.5), Value::Double(2.5));
    EXPECT_MATCH(d, CompareOp::kGe, Value::Int64(3), false, ">= above a double window");
    EXPECT_MATCH(d, CompareOp::kLe, Value::Int64(1), false, "<= below a double window");
    EXPECT_MATCH(d, CompareOp::kGe, Value::Int64(2), true, ">= inside a double window");
}

QUARRY_TEST(ZoneMapVarcharWindow) {
    const ColumnStats s = Window(Value::Varchar("carrot"), Value::Varchar("melon"));

    EXPECT_MATCH(s, CompareOp::kEq, Value::Varchar("apple"), false, "= before the window");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Varchar("kiwi"), true, "= inside the window");
    EXPECT_MATCH(s, CompareOp::kEq, Value::Varchar("zucchini"), false, "= after the window");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Varchar("carrot"), false, "< at min");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Varchar("melon"), false, "> at max");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Varchar(""), true, ">= the empty string");
    EXPECT_MATCH(s, CompareOp::kLt, Value::Varchar(""), false, "< the empty string");

    // An empty string is a legitimate bound, not a missing one.
    const ColumnStats empties = Window(Value::Varchar(""), Value::Varchar(""));
    EXPECT_MATCH(empties, CompareOp::kEq, Value::Varchar(""), true, "= the empty string");
    EXPECT_MATCH(empties, CompareOp::kNe, Value::Varchar(""), false, "<> the empty string");
    EXPECT_MATCH(empties, CompareOp::kGt, Value::Varchar("a"), false, "> on an all-empty chunk");
}

QUARRY_TEST(ZoneMapBooleanWindow) {
    const ColumnStats all_false = Window(Value::Boolean(false), Value::Boolean(false));
    EXPECT_MATCH(all_false, CompareOp::kEq, Value::Boolean(true), false, "= true on all-false");
    EXPECT_MATCH(all_false, CompareOp::kEq, Value::Boolean(false), true, "= false on all-false");
    EXPECT_MATCH(all_false, CompareOp::kNe, Value::Boolean(false), false, "<> false on all-false");
    EXPECT_MATCH(all_false, CompareOp::kGt, Value::Boolean(false), false, "> false on all-false");

    const ColumnStats mixed = Window(Value::Boolean(false), Value::Boolean(true));
    EXPECT_MATCH(mixed, CompareOp::kEq, Value::Boolean(true), true, "= true on mixed");
    EXPECT_MATCH(mixed, CompareOp::kNe, Value::Boolean(true), true, "<> true on mixed");
    EXPECT_MATCH(mixed, CompareOp::kLt, Value::Boolean(true), true, "< true on mixed");
    EXPECT_MATCH(mixed, CompareOp::kGt, Value::Boolean(false), true, "> false on mixed");
}

QUARRY_TEST(ZoneMapHandlesNaNAndInfinities) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // NaN in a bound makes every ordered comparison false, which would read as a
    // prune. Bounds like that must disable pruning instead.
    const ColumnStats poisoned = Window(Value::Double(nan), Value::Double(5.0));
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(poisoned, op, Value::Double(1.0), true, "NaN min");
    }
    const ColumnStats poisoned_max = Window(Value::Double(0.0), Value::Double(nan));
    for (CompareOp op : kAllOps) {
        EXPECT_MATCH(poisoned_max, op, Value::Double(1.0), true, "NaN max");
    }

    // A NaN constant satisfies nothing, so pruning on it is correct.
    const ColumnStats clean = Window(Value::Double(0.0), Value::Double(10.0));
    EXPECT_MATCH(clean, CompareOp::kEq, Value::Double(nan), false, "= NaN");
    EXPECT_MATCH(clean, CompareOp::kLt, Value::Double(nan), false, "< NaN");
    EXPECT_MATCH(clean, CompareOp::kGt, Value::Double(nan), false, "> NaN");

    const ColumnStats unbounded = Window(Value::Double(-inf), Value::Double(inf));
    EXPECT_MATCH(unbounded, CompareOp::kEq, Value::Double(1e300), true, "= inside an infinite window");
    EXPECT_MATCH(unbounded, CompareOp::kGt, Value::Double(inf), false, "> +inf");
    EXPECT_MATCH(unbounded, CompareOp::kLt, Value::Double(-inf), false, "< -inf");
}

QUARRY_TEST(ZoneMapIntegerBoundaryValues) {
    const ColumnStats s = IntWindow(INT64_MIN, INT64_MAX);
    EXPECT_MATCH(s, CompareOp::kLt, Value::Int64(INT64_MIN), false, "< INT64_MIN");
    EXPECT_MATCH(s, CompareOp::kGt, Value::Int64(INT64_MAX), false, "> INT64_MAX");
    EXPECT_MATCH(s, CompareOp::kGe, Value::Int64(INT64_MIN), true, ">= INT64_MIN");
    EXPECT_MATCH(s, CompareOp::kLe, Value::Int64(INT64_MAX), true, "<= INT64_MAX");

    const ColumnStats tiny = IntWindow(INT64_MAX - 1, INT64_MAX);
    EXPECT_MATCH(tiny, CompareOp::kEq, Value::Int64(INT64_MAX), true, "= INT64_MAX");
    EXPECT_MATCH(tiny, CompareOp::kEq, Value::Int64(0), false, "= 0 near the top of the range");
}

// ---------------------------------------------------------------------------
// The property that matters: a prune never hides a match.
// ---------------------------------------------------------------------------

QUARRY_TEST(ZoneMapDifferentialAgainstBruteForce) {
    std::mt19937_64 rng(0xBEEF);

    size_t pruned = 0, total = 0;

    for (int iter = 0; iter < 4000; ++iter) {
        const size_t n = rng() % 40;
        const int type_pick = static_cast<int>(rng() % 4);
        const int null_rate = static_cast<int>(rng() % 5);  // 0 means no nulls
        const int64_t span = 1 + static_cast<int64_t>(rng() % 30);

        std::vector<Value> values;
        for (size_t i = 0; i < n; ++i) {
            if (null_rate != 0 && (rng() % 5) < static_cast<uint64_t>(null_rate)) {
                values.push_back(Value::Null());
                continue;
            }
            switch (type_pick) {
                case 0:
                    values.push_back(
                        Value::Int64(static_cast<int64_t>(rng() % static_cast<uint64_t>(span))));
                    break;
                case 1:
                    values.push_back(Value::Double(
                        static_cast<double>(rng() % static_cast<uint64_t>(span)) / 4.0));
                    break;
                case 2:
                    values.push_back(Value::Boolean((rng() & 1) != 0));
                    break;
                default:
                    values.push_back(
                        Value::Varchar("s" + std::to_string(rng() % static_cast<uint64_t>(span))));
                    break;
            }
        }

        const ColumnStats stats = StatsFor(values);

        for (int probe = 0; probe < 12; ++probe) {
            const CompareOp op = kAllOps[rng() % 6];

            // Constants are drawn from a wider range than the data, and sometimes
            // from the wrong type, so the probes land inside, on, and outside the
            // window as well as in the incomparable case.
            Value constant;
            const int constant_kind = static_cast<int>(rng() % 10);
            if (constant_kind == 0) {
                constant = Value::Null();
            } else if (constant_kind == 1) {
                constant = Value::Varchar("s" + std::to_string(rng() % 40));
            } else if (constant_kind == 2) {
                constant = Value::Boolean((rng() & 1) != 0);
            } else if (constant_kind == 3) {
                constant = Value::Double(
                    static_cast<double>(static_cast<int64_t>(rng() % 80) - 20) / 4.0);
            } else {
                constant = Value::Int64(static_cast<int64_t>(rng() % 80) - 20);
            }

            ++total;
            const bool can = CanMatch(stats, op, constant);
            const size_t matches = CountMatches(values, op, constant);
            if (!can) {
                ++pruned;
                if (matches != 0) {
                    testing::ReportFailure(
                        __FILE__, __LINE__,
                        "pruned a chunk with " + std::to_string(matches) + " matching rows: [" +
                            stats.min.ToString() + ", " + stats.max.ToString() + "] nulls=" +
                            std::to_string(stats.null_count) + " " + CompareOpName(op) + " " +
                            constant.ToString());
                }
            }
        }
    }

    // A pruning rule that never prunes satisfies the safety property trivially, so
    // the test also insists the mechanism is doing something.
    CHECK(pruned > total / 100);
}

QUARRY_TEST(ZoneMapDifferentialOnWriterProducedStats) {
    // The same property, but with statistics produced by the real writer rather
    // than recomputed in the test, so a disagreement between the two shows up as a
    // failure here instead of as a silently wrong production result.
    std::mt19937_64 rng(0x5EED);
    const std::string path = TempPath("writer_stats");

    size_t pruned = 0;

    for (int iter = 0; iter < 150; ++iter) {
        Schema schema;
        schema.Add(Column("n", TypeId::kInt64));
        schema.Add(Column("s", TypeId::kVarchar));
        schema.Add(Column("d", TypeId::kDouble));

        const size_t n = rng() % 50;
        const int null_rate = static_cast<int>(rng() % 5);
        const int64_t span = 1 + static_cast<int64_t>(rng() % 20);

        std::vector<Value> ints, strs, dbls;
        Batch batch(schema);
        for (size_t i = 0; i < n; ++i) {
            const bool null_row =
                null_rate != 0 && (rng() % 5) < static_cast<uint64_t>(null_rate);
            if (null_row) {
                batch.column(0).AppendNull();
                batch.column(1).AppendNull();
                batch.column(2).AppendNull();
                ints.push_back(Value::Null());
                strs.push_back(Value::Null());
                dbls.push_back(Value::Null());
                continue;
            }
            const int64_t iv = static_cast<int64_t>(rng() % static_cast<uint64_t>(span));
            const std::string sv = "s" + std::to_string(rng() % static_cast<uint64_t>(span));
            const double dv = static_cast<double>(iv) / 2.0;
            batch.column(0).AppendInt64(iv);
            batch.column(1).AppendString(sv);
            batch.column(2).AppendDouble(dv);
            ints.push_back(Value::Int64(iv));
            strs.push_back(Value::Varchar(sv));
            dbls.push_back(Value::Double(dv));
        }
        batch.SetRowCount(n);

        FileSegmentWriter writer(path, schema, static_cast<uint64_t>(iter));
        CHECK_OK(writer.Append(batch));
        auto result = writer.Finish();
        CHECK(result.ok());
        if (!result.ok()) continue;
        const SegmentMeta meta = result.take();

        const std::vector<std::vector<Value>*> columns = {&ints, &strs, &dbls};
        for (size_t c = 0; c < columns.size(); ++c) {
            for (int probe = 0; probe < 8; ++probe) {
                const CompareOp op = kAllOps[rng() % 6];
                Value constant;
                if (c == 1) {
                    constant = Value::Varchar("s" + std::to_string(rng() % 30));
                } else if (c == 2) {
                    constant = Value::Double(
                        static_cast<double>(static_cast<int64_t>(rng() % 60) - 10) / 4.0);
                } else {
                    constant = Value::Int64(static_cast<int64_t>(rng() % 40) - 10);
                }

                const bool can = CanMatch(meta.columns[c].stats, op, constant);
                const size_t matches = CountMatches(*columns[c], op, constant);
                if (!can) {
                    ++pruned;
                    if (matches != 0) {
                        testing::ReportFailure(__FILE__, __LINE__,
                                               "writer stats pruned a chunk with " +
                                                   std::to_string(matches) +
                                                   " matching rows on column " +
                                                   std::to_string(c));
                    }
                }
            }
        }
    }

    CHECK(pruned > 0);
    ::unlink(path.c_str());
}

QUARRY_TEST_MAIN()
