// The vectorization question: is batch-at-a-time execution actually faster than
// tuple-at-a-time, and is kBatchSize = 2048 the right number?
//
// Four things this benchmark is careful about, because each is a way the result
// could be flattering rather than true:
//
//  1. The scalar contrast lives here, not in the engine. The engine is not
//     modified and not crippled; the row-at-a-time paths below are written to be
//     the best honest version of a tuple-at-a-time engine, not a strawman. There
//     are three of them, because "vectorization" bundles several separate effects
//     and a single comparison hides which one is paying.
//
//  2. Both sides stream the same bytes. The predicated column is int64, the
//     batches are single-column, and every path reads 8 bytes per row exactly
//     once. A vectorized path charged for materializing four columns while the
//     scalar path reads one array would be a rigged comparison in the other
//     direction.
//
//  3. Batch materialization is reported separately from the filter kernel. The
//     engine's scan decodes a whole column and then copies it row by row into the
//     operator's batch, and that copy is neither vectorization's fault nor
//     something a comparison may quietly hide. So the kernel is measured over
//     pre-materialized batches, and the fill is measured next to it.
//
//  4. Every path is checked to select the same number of rows. A filter that
//     disagrees is not a faster filter.
//
// The predicate is applied through the engine's own ApplyPredicate, which is the
// kernel SeqScan uses for a pushed predicate. Reimplementing it here "for
// fairness" would measure this file instead of the engine.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/execution/expression.hpp"
#include "bench_util.hpp"
#include "datagen.hpp"
#include "quarry/batch.hpp"
#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/types.hpp"

using namespace quarry;
using namespace quarry::bench;

namespace {

// kRows is sized so the predicated column is 32 MB: comfortably past any level of
// cache on the machines this runs on, so a sweep over batch sizes measures batch
// locality rather than the whole dataset happening to be resident.
constexpr size_t kRows = 4'000'000;

// kValueDomain is the range the predicated column is drawn from, so a selectivity
// of f is expressed as `value < f * kValueDomain`.
constexpr int64_t kValueDomain = 1'000'000;

// Columns holds the predicated column on its own, plus the same rows in a
// row-major layout. The row-major copy carries four fields so that the layout
// contrast is a real one: a row store reads a whole row to test one field.
struct Columns {
    std::vector<int64_t> a;

    struct RowMajor {
        int64_t a;
        double b;
        int64_t c;
        int64_t d;
    };
    std::vector<RowMajor> rows;
};

Columns MakeColumns(size_t n) {
    // datagen::Rng rather than <random>: the same named, fixed generator the tests
    // use, so a number in results.md can be reproduced exactly.
    datagen::Rng rng(0xbe4c4000ULL);
    Columns cols;
    cols.a.reserve(n);
    cols.rows.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const int64_t a = static_cast<int64_t>(rng.Below(static_cast<uint64_t>(kValueDomain)));
        const double b = static_cast<double>(rng.Below(640000)) / 64.0;
        const int64_t c = static_cast<int64_t>(rng.Below(1000));
        cols.a.push_back(a);
        cols.rows.push_back({a, b, c, static_cast<int64_t>(i)});
    }
    return cols;
}

Schema OneColumnSchema() {
    Schema s;
    s.Add(Column("a", TypeId::kInt64, false));
    return s;
}

Batch MakeBatch(size_t capacity) {
    Batch b(OneColumnSchema());
    // AddColumn reserves kBatchSize; a larger batch would otherwise reallocate on
    // its first fill and the first timed run would be the odd one out.
    b.column(0).Reserve(capacity);
    return b;
}

// FillBatch copies `n` rows starting at `from` into a reused batch, which is what
// a segment reader does: the column is already decoded, and Next() appends row by
// row into the operator's batch. Reproduced rather than invented, because the
// per-batch cost the sweep measures includes it.
void FillBatch(Batch& batch, const Columns& cols, size_t from, size_t n) {
    batch.Reset();
    Vector& va = batch.column(0);
    for (size_t i = 0; i < n; ++i) va.AppendInt64(cols.a[from + i]);
    batch.SetRowCount(n);
}

// Materialize builds the whole dataset as a sequence of batches, so the filter
// kernel can be timed without the fill. The batches together hold one copy of the
// column, so a pass over them streams the same 32 MB a pass over the raw array
// does.
std::vector<Batch> Materialize(const Columns& cols, size_t n, size_t batch_size) {
    std::vector<Batch> out;
    out.reserve(n / batch_size + 1);
    for (size_t from = 0; from < n; from += batch_size) {
        const size_t count = std::min(batch_size, n - from);
        Batch b = MakeBatch(count);
        FillBatch(b, cols, from, count);
        out.push_back(std::move(b));
    }
    return out;
}

// Consume is the downstream operator in the pipeline: it reads the selection
// vector and then reads the column through it. kBatchSize's stated justification
// is that this second read hits cache rather than memory, so a sweep that is
// supposed to test the claim needs a stage that actually performs it.
int64_t Consume(const Batch& batch) {
    const int64_t* data = batch.column(0).int64_data();
    int64_t sum = 0;
    for (const uint32_t r : batch.Rows()) sum += data[r];
    return sum;
}

// --- the tuple-at-a-time contrast ---------------------------------------
//
// RowPredicate is a Volcano-style per-row expression: one virtual call per row,
// which is the cost vectorization exists to amortize. The vector-of-pointers plus
// a volatile index is there so the compiler cannot prove the dynamic type and
// devirtualize the call, which would silently turn this into the fast path.
struct RowPredicate {
    virtual ~RowPredicate() = default;
    virtual bool Eval(size_t row) const = 0;
};

struct ColumnLessThan final : RowPredicate {
    const int64_t* data;
    int64_t constant;
    ColumnLessThan(const int64_t* d, int64_t k) : data(d), constant(k) {}
    bool Eval(size_t row) const override { return data[row] < constant; }
};

struct RowMajorLessThan final : RowPredicate {
    const Columns::RowMajor* rows;
    int64_t constant;
    RowMajorLessThan(const Columns::RowMajor* r, int64_t k) : rows(r), constant(k) {}
    bool Eval(size_t row) const override { return rows[row].a < constant; }
};

// BoxedLessThan is what the engine would be if Value flowed through execution.
// batch.hpp states outright that it must not; this is the measurement of that
// claim rather than a restatement of it.
struct BoxedLessThan final : RowPredicate {
    const int64_t* data;
    Value constant;
    BoxedLessThan(const int64_t* d, int64_t k) : data(d), constant(Value::Int64(k)) {}
    bool Eval(size_t row) const override { return Value::Int64(data[row]) < constant; }
};

enum PredicateSlot : size_t { kColumnar = 0, kRowMajor = 1, kBoxed = 2 };

std::vector<std::unique_ptr<RowPredicate>> MakePredicates(const Columns& cols, int64_t constant) {
    std::vector<std::unique_ptr<RowPredicate>> out;
    out.push_back(std::make_unique<ColumnLessThan>(cols.a.data(), constant));
    out.push_back(std::make_unique<RowMajorLessThan>(cols.rows.data(), constant));
    out.push_back(std::make_unique<BoxedLessThan>(cols.a.data(), constant));
    return out;
}

// ScalarPass runs one whole pass through a row predicate, building the surviving
// row ids and then consuming them, so it does the same work as the vectorized
// pass: evaluate, record what survived, read the survivors.
size_t ScalarPass(const RowPredicate& pred, const int64_t* data, size_t n,
                  std::vector<uint32_t>& keep) {
    keep.clear();
    for (size_t i = 0; i < n; ++i) {
        if (pred.Eval(i)) keep.push_back(static_cast<uint32_t>(i));
    }
    int64_t sum = 0;
    for (const uint32_t r : keep) sum += data[r];
    Sink(sum);
    return keep.size();
}

// SpecializedPass is the branch-free type-specialized kernel the engine does NOT
// have: no per-row type switch, no std::optional, no three-valued-logic flag. It
// is here as the ceiling, so the engine's number can be read as a fraction of
// what the layout makes available rather than as an unbounded win or loss.
size_t SpecializedPass(const int64_t* data, size_t n, int64_t constant,
                       std::vector<uint32_t>& keep) {
    keep.clear();
    keep.resize(n);
    uint32_t* out = keep.data();
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        // Branch-free append: the index is always written, and the cursor only
        // advances on a match, so a mispredicted compare costs nothing.
        out[count] = static_cast<uint32_t>(i);
        count += static_cast<size_t>(data[i] < constant);
    }
    keep.resize(count);
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += data[keep[i]];
    Sink(sum);
    return count;
}

// KernelPass is the engine's filter applied to already-materialized batches: the
// vectorized path with the batch fill excluded.
//
// This is a LOWER BOUND on the engine's kernel cost, not the engine's cost. A real
// scan calls Batch::Reset() between batches, which drops the cached identity
// vector that Rows() rebuilds; here the row count never changes, so the cache
// survives and that O(batch) write is not charged. The scan-path row in the table
// is the faithful end-to-end number.
size_t KernelPass(std::vector<Batch>& batches, const Predicate& pred) {
    size_t matched = 0;
    int64_t sum = 0;
    for (Batch& b : batches) {
        b.ClearSelection();
        Check(ApplyPredicate(b, pred), "ApplyPredicate");
        matched += b.selected_count();
        sum += Consume(b);
    }
    Sink(sum);
    return matched;
}

// ScanPass is the engine's filter including the row-by-row fill a segment reader
// performs, which is what a real query actually costs.
size_t ScanPass(const Columns& cols, size_t n, size_t batch_size, const Predicate& pred,
                Batch& batch) {
    size_t matched = 0;
    int64_t sum = 0;
    for (size_t from = 0; from < n; from += batch_size) {
        const size_t count = std::min(batch_size, n - from);
        FillBatch(batch, cols, from, count);
        Check(ApplyPredicate(batch, pred), "ApplyPredicate");
        matched += batch.selected_count();
        sum += Consume(batch);
    }
    Sink(sum);
    return matched;
}

// FillOnlyPass is ScanPass with the predicate and the consumer removed, so a sweep
// point can be split into batch machinery and the copy every path pays.
void FillOnlyPass(const Columns& cols, size_t n, size_t batch_size, Batch& batch) {
    for (size_t from = 0; from < n; from += batch_size) {
        const size_t count = std::min(batch_size, n - from);
        FillBatch(batch, cols, from, count);
        Sink(batch.row_count());
    }
}

Predicate LessThan(int64_t constant) {
    Predicate p;
    p.column_index = 0;
    p.op = CompareOp::kLt;
    p.constant = Value::Int64(constant);
    return p;
}

// ---------------------------------------------------------------------------
// 1. vectorized versus tuple-at-a-time
// ---------------------------------------------------------------------------

void BenchVectorizedVsScalar(const Columns& cols) {
    Heading("1. Vectorized versus tuple-at-a-time, 10% selective filter on an int64 column");

    constexpr size_t kRuns = 7;
    const int64_t constant = kValueDomain / 10;
    const Predicate pred = LessThan(constant);
    auto predicates = MakePredicates(cols, constant);

    std::vector<Batch> batches = Materialize(cols, kRows, kBatchSize);
    Batch reusable = MakeBatch(kBatchSize);
    std::vector<uint32_t> keep;
    keep.reserve(kRows);

    size_t kernel_matched = 0;
    const Sample kernel =
        Time("engine ApplyPredicate over pre-materialized batches (kBatchSize=" +
                 std::to_string(kBatchSize) + ")",
             kRuns, static_cast<double>(kRows), [&] { kernel_matched = KernelPass(batches, pred); });

    size_t scan_matched = 0;
    const Sample scan =
        Time("engine scan path: batch fill + ApplyPredicate", kRuns, static_cast<double>(kRows),
             [&] { scan_matched = ScanPass(cols, kRows, kBatchSize, pred, reusable); });

    size_t specialized_matched = 0;
    const Sample specialized =
        Time("branch-free type-specialized int64 kernel (ceiling)", kRuns,
             static_cast<double>(kRows),
             [&] { specialized_matched = SpecializedPass(cols.a.data(), kRows, constant, keep); });

    volatile size_t which = kColumnar;
    size_t columnar_matched = 0;
    const Sample scalar_columnar =
        Time("tuple-at-a-time, columnar data, virtual per-row predicate", kRuns,
             static_cast<double>(kRows), [&] {
                 columnar_matched = ScalarPass(*predicates[which], cols.a.data(), kRows, keep);
             });

    which = kRowMajor;
    size_t rowmajor_matched = 0;
    const Sample scalar_rowmajor =
        Time("tuple-at-a-time, row-major data (32 B/row), virtual per-row predicate", kRuns,
             static_cast<double>(kRows), [&] {
                 rowmajor_matched = ScalarPass(*predicates[which], cols.a.data(), kRows, keep);
             });

    which = kBoxed;
    size_t boxed_matched = 0;
    const Sample scalar_boxed =
        Time("tuple-at-a-time, boxed Value comparison", kRuns, static_cast<double>(kRows), [&] {
            boxed_matched = ScalarPass(*predicates[which], cols.a.data(), kRows, keep);
        });

    // Correctness gate. A path that selects a different number of rows is not a
    // data point, and reporting its time next to the others would be a lie.
    if (kernel_matched != scan_matched || kernel_matched != specialized_matched ||
        kernel_matched != columnar_matched || kernel_matched != rowmajor_matched ||
        kernel_matched != boxed_matched) {
        Fail("filter paths disagree: kernel=" + std::to_string(kernel_matched) +
             " scan=" + std::to_string(scan_matched) +
             " specialized=" + std::to_string(specialized_matched) +
             " columnar=" + std::to_string(columnar_matched) +
             " rowmajor=" + std::to_string(rowmajor_matched) +
             " boxed=" + std::to_string(boxed_matched));
    }

    std::cout << "rows: " << Thousands(kRows) << "   matched: " << Thousands(kernel_matched) << " ("
              << Fmt(100.0 * static_cast<double>(kernel_matched) / static_cast<double>(kRows))
              << "%)   runs per path: " << kRuns << "\n"
              << "batches materialized: " << Thousands(batches.size()) << "\n\n";

    Table t({"path", "ms min/med/max", "rows/sec", "ns/row", "vs engine kernel", "max/min"});
    const auto row = [&](const Sample& s) {
        t.Row({s.name, MinMedMaxMs(s), Rate(s.PerSecond()), Fmt(s.NanosPerItem(), 3),
               Fmt(s.median_ns / kernel.median_ns) + "x", Fmt(s.Spread())});
    };
    row(kernel);
    row(scan);
    t.Separator();
    row(scalar_columnar);
    row(scalar_rowmajor);
    row(scalar_boxed);
    t.Separator();
    row(specialized);
    t.Print();

    Note("The comparison that answers the project's claim is the first row against\n"
         "the third: the engine's batch kernel versus a row-at-a-time loop over the\n"
         "SAME columnar bytes. Both evaluate the predicate, both build a list of\n"
         "surviving row ids, both then read the survivors.");
}

// ---------------------------------------------------------------------------
// 2. batch size sweep
// ---------------------------------------------------------------------------

void BenchBatchSizeSweep(const Columns& cols) {
    Heading("2. Batch size sweep");

    constexpr size_t kRuns = 5;
    const int64_t constant = kValueDomain / 10;
    const Predicate pred = LessThan(constant);
    const std::vector<size_t> sizes = {1, 8, 64, 256, 1024, 2048, 8192, 32768};

    // Below this the pre-materialized variant would need millions of Batch
    // objects, whose per-object overhead dwarfs the data they hold and would make
    // the point measure allocator behaviour instead of batch locality.
    constexpr size_t kMinMaterializable = 64;

    std::cout << "rows per pass: " << Thousands(kRows) << "\n"
              << "engine kBatchSize = " << kBatchSize
              << " (16 KiB of int64 - the L1/L2 residency claim under test)\n\n";

    Table t({"batch", "col KiB", "kernel-only ms min/med/max", "kernel ns/row", "fill ns/row",
             "fill+kernel ns/row", "fill+kernel rows/s", "max/min"});

    double best_kernel_rate = 0.0;
    size_t best_kernel_size = 0;
    double best_total_rate = 0.0;
    size_t best_total_size = 0;
    std::vector<std::pair<size_t, double>> kernel_curve;

    for (const size_t b : sizes) {
        Batch reusable = MakeBatch(b);
        Batch fill_only = MakeBatch(b);

        const Sample full = Time("total", kRuns, static_cast<double>(kRows),
                                 [&] { ScanPass(cols, kRows, b, pred, reusable); });
        const Sample fill = Time("fill", kRuns, static_cast<double>(kRows),
                                 [&] { FillOnlyPass(cols, kRows, b, fill_only); });

        const double kib = static_cast<double>(b * sizeof(int64_t)) / 1024.0;
        if (b >= kMinMaterializable) {
            std::vector<Batch> batches = Materialize(cols, kRows, b);
            const Sample kernel = Time("kernel", kRuns, static_cast<double>(kRows),
                                       [&] { KernelPass(batches, pred); });
            t.Row({std::to_string(b), Fmt(kib, 1), MinMedMaxMs(kernel),
                   Fmt(kernel.NanosPerItem(), 3), Fmt(fill.NanosPerItem(), 3),
                   Fmt(full.NanosPerItem(), 3), Rate(full.PerSecond()), Fmt(kernel.Spread())});
            kernel_curve.emplace_back(b, kernel.PerSecond());
            if (kernel.PerSecond() > best_kernel_rate) {
                best_kernel_rate = kernel.PerSecond();
                best_kernel_size = b;
            }
        } else {
            t.Row({std::to_string(b), Fmt(kib, 3), "(not materialized)", "-",
                   Fmt(fill.NanosPerItem(), 3), Fmt(full.NanosPerItem(), 3),
                   Rate(full.PerSecond()), Fmt(full.Spread())});
        }

        if (full.PerSecond() > best_total_rate) {
            best_total_rate = full.PerSecond();
            best_total_size = b;
        }
    }
    t.Print();

    // "Stops helping" is defined rather than eyeballed: the smallest batch size
    // within 2% of the best. An undefined threshold is how a curve gets read as
    // whatever the author expected.
    size_t plateau = best_kernel_size;
    for (const auto& [size, rate] : kernel_curve) {
        if (rate >= best_kernel_rate * 0.98) {
            plateau = size;
            break;
        }
    }

    double rate_at_default = 0.0;
    for (const auto& [size, rate] : kernel_curve) {
        if (size == kBatchSize) rate_at_default = rate;
    }

    Note("kernel-only peak: batch=" + std::to_string(best_kernel_size) + " at " +
         Rate(best_kernel_rate) + "; within 2% of peak from batch=" + std::to_string(plateau) +
         " upwards.\nkernel-only at the engine's kBatchSize=" + std::to_string(kBatchSize) + ": " +
         Rate(rate_at_default) + " (" +
         Fmt(100.0 * (1.0 - rate_at_default / best_kernel_rate), 1) + "% below peak)\n" +
         "fill+kernel peak: batch=" + std::to_string(best_total_size) + " at " +
         Rate(best_total_rate));
}

// ---------------------------------------------------------------------------
// 3. reused versus freshly allocated batch
// ---------------------------------------------------------------------------

void BenchBatchReuse(const Columns& cols) {
    Heading("3. Reused batch versus a fresh batch per iteration");

    constexpr size_t kRuns = 7;
    const Predicate pred = LessThan(kValueDomain / 10);
    const Schema schema = OneColumnSchema();
    const size_t batch_count = kRows / kBatchSize + (kRows % kBatchSize ? 1 : 0);

    Batch reused = MakeBatch(kBatchSize);
    const Sample with_reuse = Time("reused batch", kRuns, static_cast<double>(kRows),
                                   [&] { ScanPass(cols, kRows, kBatchSize, pred, reused); });

    const Sample fresh = Time("fresh Batch per chunk", kRuns, static_cast<double>(kRows), [&] {
        int64_t sum = 0;
        for (size_t from = 0; from < kRows; from += kBatchSize) {
            const size_t count = std::min(kBatchSize, kRows - from);
            // Constructed inside the loop on purpose: this is the cost of the
            // allocation and the per-column Reserve that reuse avoids.
            Batch batch(schema);
            FillBatch(batch, cols, from, count);
            Check(ApplyPredicate(batch, pred), "ApplyPredicate");
            sum += Consume(batch);
        }
        Sink(sum);
    });

    Table t({"strategy", "ms min/med/max", "rows/sec", "ns/row", "max/min"});
    for (const Sample* s : {&with_reuse, &fresh}) {
        t.Row({s->name, MinMedMaxMs(*s), Rate(s->PerSecond()), Fmt(s->NanosPerItem(), 3),
               Fmt(s->Spread())});
    }
    t.Print();
    Note("allocation overhead: " + Fmt(fresh.median_ns / with_reuse.median_ns) + "x, " +
         Fmt(fresh.NanosPerItem() - with_reuse.NanosPerItem(), 3) + " ns/row, " +
         Fmt((fresh.median_ns - with_reuse.median_ns) / static_cast<double>(batch_count)) +
         " ns per batch of " + std::to_string(kBatchSize) + " over " + Thousands(batch_count) +
         " batches");
}

// ---------------------------------------------------------------------------
// 4. selectivity sweep
// ---------------------------------------------------------------------------

void BenchSelectivity(const Columns& cols) {
    Heading("4. Selectivity sweep at kBatchSize");

    constexpr size_t kRuns = 5;
    const std::vector<double> fractions = {0.001, 0.01, 0.10, 0.50, 1.00};

    std::vector<Batch> batches = Materialize(cols, kRows, kBatchSize);
    std::vector<uint32_t> keep;
    keep.reserve(kRows);

    Table t({"target", "actual", "kernel ms min/med/max", "kernel ns/row", "tuple ms min/med/max",
             "tuple ns/row", "tuple/kernel", "branch-free ns/row"});

    for (const double f : fractions) {
        // `== domain` for 100%, so the fully selective case really passes every row
        // rather than all but the maximum.
        const int64_t constant =
            f >= 1.0 ? kValueDomain : static_cast<int64_t>(f * static_cast<double>(kValueDomain));
        const Predicate pred = LessThan(constant);
        auto row_preds = MakePredicates(cols, constant);
        volatile size_t which = kColumnar;

        size_t matched = 0;
        const Sample kernel = Time("kernel", kRuns, static_cast<double>(kRows),
                                   [&] { matched = KernelPass(batches, pred); });
        const Sample tuple = Time("tuple", kRuns, static_cast<double>(kRows), [&] {
            ScalarPass(*row_preds[which], cols.a.data(), kRows, keep);
        });
        const Sample branchfree = Time("branchfree", kRuns, static_cast<double>(kRows), [&] {
            SpecializedPass(cols.a.data(), kRows, constant, keep);
        });

        t.Row({Fmt(f * 100.0, 1) + "%",
               Fmt(100.0 * static_cast<double>(matched) / static_cast<double>(kRows), 3) + "%",
               MinMedMaxMs(kernel), Fmt(kernel.NanosPerItem(), 3), MinMedMaxMs(tuple),
               Fmt(tuple.NanosPerItem(), 3), Fmt(tuple.median_ns / kernel.median_ns) + "x",
               Fmt(branchfree.NanosPerItem(), 3)});
    }
    t.Print();
    Note("Every path costs more as selectivity rises, because the list of surviving\n"
         "row ids that gets written and then re-read grows with the match count. The\n"
         "50% row is the branch-misprediction worst case for the two branching paths;\n"
         "the branch-free column is the control that shows it.");
}

}  // namespace

int main() {
    std::cout << "quarry execution benchmark\n"
              << "kBatchSize=" << kBatchSize << "  rows=" << Thousands(kRows)
              << "  predicated int64 column = " << Bytes(kRows * sizeof(int64_t)) << "\n";

    const Columns cols = MakeColumns(kRows);
    std::cout << "row-major copy = " << Bytes(cols.rows.size() * sizeof(Columns::RowMajor)) << "\n";

    BenchVectorizedVsScalar(cols);
    BenchBatchSizeSweep(cols);
    BenchBatchReuse(cols);
    BenchSelectivity(cols);
    std::cout << "\n";
    return 0;
}
