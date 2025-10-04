// B+ tree versus LSM, and index versus full scan.
//
// Both indexes are built over the same keys in the same order and queried with the
// same key sequence, so every difference below is structural rather than a
// difference in what they were asked to do.
//
// Two things this benchmark refuses to do:
//
//  1. Run with everything cached. An index benchmark whose working set fits in the
//     buffer pool measures an in-memory tree, which is not what either structure
//     is for. The pool is therefore swept down to 5% of the tree, and the 100% row
//     is included only as the upper bound it is.
//
//  2. Compare an index lookup against a scan without materializing rows. Resolving
//     a row id is not answering a query. The index path here really does fetch the
//     matching rows out of the segments they live in, which is what makes the
//     crossover below a number an optimizer could act on.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../src/index/btree/btree.hpp"
#include "../src/index/lsm/lsm.hpp"
#include "bench_data.hpp"
#include "bench_util.hpp"
#include "datagen.hpp"
#include "quarry/buffer.hpp"
#include "quarry/catalog.hpp"
#include "quarry/storage.hpp"

using namespace quarry;
using namespace quarry::bench;

namespace {

// kKeys is sized so the B+ tree is several megabytes: large enough that a 5% pool
// genuinely thrashes, small enough that the LSM's 64 KiB memtable compacts a
// realistic number of times inside a few seconds.
constexpr size_t kKeys = 500'000;

// kLookups is the point-lookup workload size. Large enough that the per-lookup
// figure is not dominated by whichever few pages happened to be resident.
constexpr size_t kLookups = 100'000;

constexpr size_t kTableRows = 500'000;
constexpr size_t kRowsPerSegment = 16384;

// Keys and the query sequence, generated once so both structures see the same
// thing. Keys are a shuffled permutation of 0..n-1: unique (so a lookup returns
// exactly one row id), dense (so a range of keys has an exactly known selectivity),
// and in random order (so insertion order tells an index nothing).
struct Workload {
    std::vector<int64_t> keys;      // insertion order
    std::vector<int64_t> probes;    // lookup order, all present
    std::vector<int64_t> absent;    // keys guaranteed not present, for the Bloom filter
};

Workload MakeWorkload(size_t n, size_t lookups) {
    datagen::Rng rng(0x1bdee7000ULL);
    Workload w;
    w.keys.resize(n);
    for (size_t i = 0; i < n; ++i) w.keys[i] = static_cast<int64_t>(i);
    // Fisher-Yates with the shared generator, so the permutation is reproducible.
    for (size_t i = n; i > 1; --i) {
        const size_t j = static_cast<size_t>(rng.Below(i));
        std::swap(w.keys[i - 1], w.keys[j]);
    }

    w.probes.reserve(lookups);
    w.absent.reserve(lookups);
    for (size_t i = 0; i < lookups; ++i) {
        w.probes.push_back(static_cast<int64_t>(rng.Below(n)));
        // Above the key domain, so every one of these misses in both structures.
        w.absent.push_back(static_cast<int64_t>(n + rng.Below(n)));
    }
    return w;
}

// --- B+ tree fixture -----------------------------------------------------
//
// The pool and the tree are built together because the tree does not own its pool
// and the pool must outlive it; bundling them is what stops a benchmark from
// accidentally destroying the pool first.
struct BTreeFixture {
    std::string path;
    std::unique_ptr<BufferPool> pool;
    std::unique_ptr<BTree> tree;

    BTreeFixture(std::string file, size_t frames, BufferPool::Policy policy)
        : path(std::move(file)) {
        pool = std::make_unique<BufferPool>(path, frames, policy);
        Check(pool->Open(), "buffer pool Open");
        tree = std::make_unique<BTree>(pool.get(), TypeId::kInt64);
        Check(tree->Open(), "btree Open");
    }

    ~BTreeFixture() {
        if (tree) Check(tree->Close(), "btree Close");
        if (pool) Check(pool->Close(), "pool Close");
    }

    BTreeFixture(const BTreeFixture&) = delete;
    BTreeFixture& operator=(const BTreeFixture&) = delete;
};

uint64_t FileBytes(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

// ---------------------------------------------------------------------------
// 1. insert throughput and write amplification
// ---------------------------------------------------------------------------

struct BuildResult {
    Sample timing;
    Index::Stats stats;
    uint64_t on_disk_bytes = 0;
    uint32_t btree_height = 0;
    size_t lsm_levels = 0;
};

BuildResult BuildBTree(const Workload& w, const std::string& dir, size_t frames) {
    BuildResult out;
    // One run, not several: building the index is destructive, so a repeated
    // measurement would have to rebuild from a different starting state each time.
    // The spread is therefore unavailable here and the single figure is labelled as
    // such rather than dressed up as a median of one.
    const std::string path = dir + "/btree_build.qidx";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    out.timing = Time("btree insert", 1, static_cast<double>(w.keys.size()),
                      [&] {
                          BTreeFixture fx(path, frames, BufferPool::Policy::kClock);
                          for (size_t i = 0; i < w.keys.size(); ++i) {
                              Check(fx.tree->Insert(Value::Int64(w.keys[i]), i), "btree Insert");
                          }
                          Check(fx.tree->Flush(), "btree Flush");
                          out.stats = fx.tree->stats();
                          out.btree_height = fx.tree->height();
                      },
                      /*warmup=*/0);
    out.on_disk_bytes = FileBytes(path);
    return out;
}

BuildResult BuildLsm(const Workload& w, const std::string& dir) {
    BuildResult out;
    const std::string path = dir + "/lsm_build";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);

    out.timing = Time("lsm insert", 1, static_cast<double>(w.keys.size()),
                      [&] {
                          auto index = Take(lsm::LsmIndex::Open(path), "lsm Open");
                          for (size_t i = 0; i < w.keys.size(); ++i) {
                              Check(index->Insert(Value::Int64(w.keys[i]), i), "lsm Insert");
                          }
                          Check(index->Flush(), "lsm Flush");
                          out.stats = index->stats();
                          out.lsm_levels = index->level_count();
                          Check(index->Close(), "lsm Close");
                      },
                      /*warmup=*/0);

    // Directory size rather than one file: an LSM's footprint is its whole set of
    // SSTables plus the manifest.
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
        if (entry.is_regular_file()) out.on_disk_bytes += FileBytes(entry.path().string());
    }
    return out;
}

void ReportBuild(const BuildResult& btree, const BuildResult& lsm, uint64_t logical) {
    Heading("1. Insert throughput and write amplification");

    std::cout << "keys: " << Thousands(kKeys)
              << " unique int64, inserted in shuffled order\nlogical bytes inserted: "
              << Thousands(logical) << " (" << Bytes(logical) << ")\n"
              << "btree height: " << btree.btree_height << "   lsm levels: " << lsm.lsm_levels
              << "\nsingle timed run per structure: building is destructive, so there is no\n"
              << "distribution to report. The derived counters below are exact, not sampled.\n\n";

    Table t({"structure", "build ms", "inserts/sec", "bytes written", "logical bytes",
             "write amp", "on-disk bytes", "space amp", "splits", "compactions"});
    const auto row = [&](const std::string& name, const BuildResult& b) {
        t.Row({name, Fmt(b.timing.median_ns / 1e6), Rate(b.timing.PerSecond()),
               Thousands(b.stats.bytes_written), Thousands(b.stats.logical_bytes),
               Fmt(b.stats.WriteAmplification()) + "x", Thousands(b.on_disk_bytes),
               Fmt(static_cast<double>(b.on_disk_bytes) /
                   static_cast<double>(b.stats.logical_bytes == 0 ? 1 : b.stats.logical_bytes)) +
                   "x",
               Thousands(b.stats.node_splits), Thousands(b.stats.compactions)});
    };
    row("B+ tree", btree);
    row("LSM", lsm);
    t.Print();

    Note("Write amplification is bytes_written / logical_bytes, both reported by the\n"
         "index itself. For the B+ tree bytes are derived from whole pages, which is\n"
         "exact because it only ever transfers pages; the LSM counts real bytes.");
}

// ---------------------------------------------------------------------------
// 2. point lookup and read amplification
// ---------------------------------------------------------------------------

void BenchPointLookups(const Workload& w, const std::string& dir, size_t btree_pages) {
    Heading("2. Point lookup latency and read amplification");

    constexpr size_t kRuns = 5;
    const std::string btree_path = dir + "/btree_build.qidx";
    const std::string lsm_path = dir + "/lsm_build";

    // 50% of the tree, so neither structure is answering out of a fully resident
    // pool. The sweep in section 5 covers the rest of the range.
    const size_t frames = std::max<size_t>(16, btree_pages / 2);

    struct Outcome {
        Sample timing;
        Index::Stats stats;
        BufferPool::Stats pool;
        uint64_t found = 0;
    };

    // Two different read-amplification numbers, and conflating them would be the
    // easy mistake here. Index::Stats::pages_read counts FetchPage CALLS, which is
    // the tree's logical access pattern and does not change with pool size.
    // BufferPool::Stats::reads counts actual preads, which is the physical figure.

    const auto run_btree = [&](const std::vector<int64_t>& probes, const std::string& label) {
        Outcome o;
        BTreeFixture fx(btree_path, frames, BufferPool::Policy::kClock);
        std::vector<uint64_t> out;
        out.reserve(8);
        o.timing = Time(label, kRuns, static_cast<double>(probes.size()), [&] {
            uint64_t found = 0;
            for (const int64_t k : probes) {
                out.clear();
                Check(fx.tree->Lookup(Value::Int64(k), out), "btree Lookup");
                found += out.size();
            }
            Sink(found);
            o.found = found;
        });
        o.stats = fx.tree->stats();
        o.pool = fx.pool->stats();
        return o;
    };

    const auto run_lsm = [&](const std::vector<int64_t>& probes, const std::string& label) {
        Outcome o;
        auto index = Take(lsm::LsmIndex::Open(lsm_path), "lsm Open");
        std::vector<uint64_t> out;
        out.reserve(8);
        o.timing = Time(label, kRuns, static_cast<double>(probes.size()), [&] {
            uint64_t found = 0;
            for (const int64_t k : probes) {
                out.clear();
                Check(index->Lookup(Value::Int64(k), out), "lsm Lookup");
                found += out.size();
            }
            Sink(found);
            o.found = found;
        });
        o.stats = index->stats();
        Check(index->Close(), "lsm Close");
        return o;
    };

    const Outcome bt_hit = run_btree(w.probes, "btree, keys present");
    const Outcome bt_miss = run_btree(w.absent, "btree, keys absent");
    const Outcome lsm_hit = run_lsm(w.probes, "lsm, keys present");
    const Outcome lsm_miss = run_lsm(w.absent, "lsm, keys absent");

    std::cout << "lookups per run: " << Thousands(kLookups) << "   buffer pool: " << frames
              << " frames (" << Bytes(frames * kPageSize) << ", ~50% of a "
              << Thousands(btree_pages) << "-page tree)\n"
              << "the LSM does not use the buffer pool: it reads its SSTables directly, so its\n"
              << "pool columns are blank rather than zero-by-accident\n\n";

    Table t({"workload", "us/lookup min/med/max", "lookups/sec", "rows found",
             "logical pages/lookup", "physical reads/lookup", "bytes read/lookup", "pool hit rate",
             "sstables/lookup", "bloom rejects/lookup", "max/min"});

    const auto row = [&](const std::string& name, const Outcome& o, bool is_lsm) {
        const double per = static_cast<double>(kLookups);
        // Counters accumulate over warmup plus every timed run, so they are divided
        // by the total number of lookups performed and not by one run's worth.
        const double total_lookups = static_cast<double>(o.stats.lookups);
        const double pages = total_lookups == 0 ? 0.0
                                               : static_cast<double>(o.stats.pages_read) /
                                                     total_lookups;
        const double bytes = total_lookups == 0 ? 0.0
                                                : static_cast<double>(o.stats.bytes_read) /
                                                      total_lookups;
        t.Row({name,
               Fmt(o.timing.min_ns / 1e3 / per, 3) + " / " + Fmt(o.timing.median_ns / 1e3 / per, 3) +
                   " / " + Fmt(o.timing.max_ns / 1e3 / per, 3),
               Rate(o.timing.PerSecond()), Thousands(o.found), Fmt(pages, 3),
               is_lsm ? "-"
                      : Fmt(total_lookups == 0
                                ? 0.0
                                : static_cast<double>(o.pool.reads) / total_lookups,
                            3),
               Fmt(bytes, 1), is_lsm ? "-" : Fmt(100.0 * o.pool.HitRate(), 1) + "%",
               is_lsm ? Fmt(static_cast<double>(o.stats.sstables_consulted) / total_lookups, 3)
                      : "-",
               is_lsm ? Fmt(static_cast<double>(o.stats.bloom_rejections) / total_lookups, 3) : "-",
               Fmt(o.timing.Spread())});
    };
    row("B+ tree, key present", bt_hit, false);
    row("B+ tree, key absent", bt_miss, false);
    t.Separator();
    row("LSM, key present", lsm_hit, true);
    row("LSM, key absent", lsm_miss, true);
    t.Print();

    Note("The absent-key rows are where the structures differ most. A B+ tree pays the\n"
         "same descent whether the key is there or not. The LSM has to rule the key out\n"
         "of every table it holds, and the Bloom filters are what stop that from being\n"
         "a read per table - bloom rejects per lookup is how many tables were skipped\n"
         "without touching their data.\n"
         "'logical pages/lookup' is the tree's own FetchPage count, which is its height\n"
         "plus the meta page and does not vary with the pool. 'physical reads/lookup' is\n"
         "the pool's pread count, which does.");
}

// ---------------------------------------------------------------------------
// 3. range scan throughput
// ---------------------------------------------------------------------------

void BenchRangeScans(const std::string& dir, size_t btree_pages) {
    Heading("3. Range scan throughput");

    constexpr size_t kRuns = 5;
    const std::string btree_path = dir + "/btree_build.qidx";
    const std::string lsm_path = dir + "/lsm_build";
    const size_t frames = std::max<size_t>(16, btree_pages / 2);

    // Widths as a fraction of the key space, so the table shows whether either
    // structure has a per-range fixed cost that a narrow scan cannot amortize.
    const std::vector<double> widths = {0.0001, 0.001, 0.01, 0.1, 1.0};

    Table t({"range width", "keys returned", "btree us/scan min/med/max", "btree keys/s",
             "lsm us/scan min/med/max", "lsm keys/s", "lsm/btree", "btree pages/scan",
             "lsm sstables/scan"});

    BTreeFixture bt(btree_path, frames, BufferPool::Policy::kClock);
    auto lsm_index = Take(lsm::LsmIndex::Open(lsm_path), "lsm Open");

    for (const double width : widths) {
        const int64_t span = std::max<int64_t>(1, static_cast<int64_t>(width * kKeys));
        const int64_t lo = static_cast<int64_t>(kKeys / 3);
        const int64_t hi = std::min<int64_t>(static_cast<int64_t>(kKeys) - 1, lo + span - 1);
        // Enough repetitions that a narrow scan is not timed against clock
        // granularity; the work figure is multiplied to match.
        const size_t reps = width <= 0.001 ? 200 : (width <= 0.01 ? 20 : 2);
        const double expected = static_cast<double>(hi - lo + 1);

        std::vector<uint64_t> out;
        out.reserve(static_cast<size_t>(expected) + 1);

        const Index::Stats bt_before = bt.tree->stats();
        size_t bt_rows = 0;
        const Sample bt_s = Time("btree range", kRuns, expected * static_cast<double>(reps), [&] {
            size_t total = 0;
            for (size_t r = 0; r < reps; ++r) {
                out.clear();
                Check(bt.tree->RangeScan(Value::Int64(lo), Value::Int64(hi), out), "btree range");
                total += out.size();
            }
            Sink(total);
            bt_rows = out.size();
        });
        const Index::Stats bt_after = bt.tree->stats();

        const Index::Stats lsm_before = lsm_index->stats();
        size_t lsm_rows = 0;
        const Sample lsm_s = Time("lsm range", kRuns, expected * static_cast<double>(reps), [&] {
            size_t total = 0;
            for (size_t r = 0; r < reps; ++r) {
                out.clear();
                Check(lsm_index->RangeScan(Value::Int64(lo), Value::Int64(hi), out), "lsm range");
                total += out.size();
            }
            Sink(total);
            lsm_rows = out.size();
        });
        const Index::Stats lsm_after = lsm_index->stats();

        if (bt_rows != lsm_rows) {
            Fail("range scans disagree: btree returned " + std::to_string(bt_rows) +
                 " and lsm returned " + std::to_string(lsm_rows));
        }

        // Scans performed = runs + one warmup, each doing `reps` range scans.
        const double scans = static_cast<double>((kRuns + 1) * reps);
        // Timings are divided by reps so the column is the cost of one range scan
        // rather than of the repetition loop a narrow scan needed to be measurable.
        const auto per_scan = [reps](const Sample& s) {
            const double r = static_cast<double>(reps);
            return Fmt(s.min_ns / 1e3 / r, 2) + " / " + Fmt(s.median_ns / 1e3 / r, 2) + " / " +
                   Fmt(s.max_ns / 1e3 / r, 2);
        };
        t.Row({Fmt(width * 100.0, 4) + "%", Thousands(bt_rows), per_scan(bt_s),
               Rate(bt_s.PerSecond()), per_scan(lsm_s), Rate(lsm_s.PerSecond()),
               Fmt(lsm_s.median_ns / bt_s.median_ns) + "x",
               Fmt(static_cast<double>(bt_after.pages_read - bt_before.pages_read) / scans, 1),
               Fmt(static_cast<double>(lsm_after.sstables_consulted -
                                       lsm_before.sstables_consulted) /
                       scans,
                   1)});
    }
    t.Print();

    Note("Both structures were verified to return the same number of row ids for every\n"
         "range, so the ratio is a speed difference and not a coverage difference.");
}

// ---------------------------------------------------------------------------
// 4. index versus full scan, and the crossover
// ---------------------------------------------------------------------------

// RowLocator maps a row id to the segment it lives in. A row id is a row's ordinal
// position across the table's segments in catalog order, which is the definition the
// scan and the index share.
class RowLocator {
public:
    explicit RowLocator(const TableDef& table) {
        uint64_t at = 0;
        for (const auto& seg : table.segments) {
            starts_.push_back(at);
            at += seg.row_count;
        }
        total_ = at;
    }

    size_t SegmentOf(uint64_t row_id) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), row_id);
        return static_cast<size_t>(it - starts_.begin()) - 1;
    }
    uint64_t StartOf(size_t segment) const { return starts_[segment]; }
    uint64_t total() const { return total_; }

private:
    std::vector<uint64_t> starts_;
    uint64_t total_ = 0;
};

// FetchByRowId is an index scan's second half: given the row ids an index returned,
// read those rows out of the segments they live in.
//
// It is worth noticing how expensive this is in this engine, because it is the
// reason the crossover lands where it does: FileSegmentReader decodes a segment's
// whole projected column at Open, so touching ONE row of a segment costs a full
// column decode of that segment. An index that scatters its hits across every
// segment therefore pays for the whole table anyway, plus the index descent.
uint64_t FetchByRowId(const TableDef& table, const RowLocator& locator,
                      std::vector<uint64_t> row_ids, const std::vector<size_t>& projection,
                      ReadCounters* counters, size_t* segments_touched) {
    std::sort(row_ids.begin(), row_ids.end());
    uint64_t checksum = 0;
    *segments_touched = 0;

    size_t at = 0;
    while (at < row_ids.size()) {
        const size_t segment = locator.SegmentOf(row_ids[at]);
        const uint64_t base = locator.StartOf(segment);
        const uint64_t end = base + table.segments[segment].row_count;

        size_t stop = at;
        while (stop < row_ids.size() && row_ids[stop] < end) ++stop;
        ++*segments_touched;

        CountingSegmentReader reader(table.segments[segment], projection, table.schema, counters);
        Check(reader.Open(), "reader Open");

        uint64_t position = 0;
        size_t cursor = at;
        Batch batch;
        for (;;) {
            bool done = false;
            Check(reader.Next(batch, done), "reader Next");
            if (done) break;
            const uint64_t batch_end = position + batch.row_count();
            const int64_t* keys = batch.column(0).int64_data();
            while (cursor < stop && row_ids[cursor] - base < batch_end) {
                checksum += keys[row_ids[cursor] - base - position];
                ++cursor;
            }
            position = batch_end;
            if (cursor >= stop) {
                // Early exit once the segment's wanted rows are all read, which is
                // the only part of the fetch an index actually makes cheaper.
                break;
            }
        }
        Check(reader.Close(), "reader Close");
        at = stop;
    }
    return checksum;
}

void BenchIndexVsScan(const TableDef& table, const std::string& dir, size_t btree_pages) {
    Heading("4. Indexed lookup versus full scan: finding the crossover");

    constexpr size_t kRuns = 3;
    const std::string btree_path = dir + "/table_key.qidx";
    const RowLocator locator(table);

    // The index is built over the table's key column, which holds a shuffled
    // permutation of 0..rows-1. A key range of width w therefore selects exactly w
    // rows, scattered uniformly over the segments - which is the case where a zone
    // map cannot help and an index is the only thing that could.
    {
        std::error_code ec;
        std::filesystem::remove(btree_path, ec);
        BTreeFixture fx(btree_path, std::max<size_t>(64, btree_pages / 2),
                        BufferPool::Policy::kClock);
        ReadCounters counters;
        auto scan = std::make_unique<SequentialScanOperator>(
            table, std::vector<size_t>{0}, std::vector<Predicate>{},
            CountingFactory(table.schema, &counters));
        Check(scan->Open(), "scan Open");
        Batch batch(scan->output_schema());
        uint64_t row_id = 0;
        for (;;) {
            bool done = false;
            Check(scan->Next(batch, done), "scan Next");
            if (done) break;
            const int64_t* keys = batch.column(0).int64_data();
            for (size_t i = 0; i < batch.row_count(); ++i) {
                Check(fx.tree->Insert(Value::Int64(keys[i]), row_id++), "index build Insert");
            }
        }
        Check(scan->Close(), "scan Close");
    }

    const size_t frames = std::max<size_t>(64, btree_pages / 2);
    BTreeFixture fx(btree_path, frames, BufferPool::Policy::kClock);

    const std::vector<double> fractions = {0.0,  0.00001, 0.0001, 0.001, 0.01,
                                           0.05, 0.10,    0.20,   0.30,  0.50};

    std::cout << "table: " << Thousands(table.row_count()) << " rows in "
              << Thousands(table.segments.size()) << " segments; key is a shuffled permutation\n"
              << "both paths project the key column only and are checked to return the same\n"
              << "number of rows\n"
              << "key ranges are centred in the key domain, NOT anchored at its bottom: an\n"
              << "anchored range lets a zone map prune on min alone and would hand the scan a\n"
              << "win that has nothing to do with the comparison being made\n\n";

    Table t({"selectivity", "rows", "index ms/query min/med/max", "index bytes/query",
             "segs touched", "scan ms/query min/med/max", "scan bytes/query", "segs pruned",
             "winner", "index/scan"});

    double crossover = -1.0;
    for (const double f : fractions) {
        // f == 0 is the point lookup: one key, one row.
        const int64_t count = f == 0.0 ? 1 : std::max<int64_t>(1, static_cast<int64_t>(
                                                                     f * static_cast<double>(
                                                                             table.row_count())));
        const int64_t rows = static_cast<int64_t>(table.row_count());
        const int64_t lo = std::max<int64_t>(0, (rows - count) / 2);
        const int64_t hi = std::min<int64_t>(rows - 1, lo + count - 1);
        // A narrow query has to be repeated to be measurable at all against clock
        // granularity; the work count is scaled to match so the rates stay comparable.
        const size_t reps = count <= 10 ? 50 : (count <= 5000 ? 5 : 1);

        ReadCounters index_counters;
        size_t index_rows = 0;
        size_t segments_touched = 0;
        const Sample index_s =
            Time("index", kRuns, static_cast<double>(count) * static_cast<double>(reps), [&] {
                index_counters.Reset();
                uint64_t checksum = 0;
                for (size_t r = 0; r < reps; ++r) {
                    std::vector<uint64_t> ids;
                    Check(fx.tree->RangeScan(Value::Int64(lo), Value::Int64(hi), ids),
                          "index RangeScan");
                    index_rows = ids.size();
                    checksum += FetchByRowId(table, locator, std::move(ids), {0}, &index_counters,
                                             &segments_touched);
                }
                Sink(checksum);
            });

        ReadCounters scan_counters;
        DrainResult scan_result;
        const Sample scan_s =
            Time("scan", kRuns, static_cast<double>(count) * static_cast<double>(reps), [&] {
                scan_counters.Reset();
                for (size_t r = 0; r < reps; ++r) {
                    std::vector<Predicate> preds;
                    Predicate ge;
                    ge.column_index = 0;
                    ge.op = CompareOp::kGe;
                    ge.constant = Value::Int64(lo);
                    Predicate le;
                    le.column_index = 0;
                    le.op = CompareOp::kLe;
                    le.constant = Value::Int64(hi);
                    preds.push_back(ge);
                    preds.push_back(le);
                    auto scan = std::make_unique<SequentialScanOperator>(
                        table, std::vector<size_t>{0}, std::move(preds),
                        CountingFactory(table.schema, &scan_counters));
                    scan_result = Drain(*scan);
                }
            });

        if (index_rows != scan_result.rows) {
            Fail("index and scan disagree at selectivity " + Fmt(f, 5) + ": index=" +
                 std::to_string(index_rows) + " scan=" + std::to_string(scan_result.rows));
        }

        const bool index_wins = index_s.median_ns < scan_s.median_ns;
        if (!index_wins && crossover < 0.0) {
            crossover = static_cast<double>(count) / static_cast<double>(table.row_count());
        }

        const auto per_query = [reps](const Sample& s) {
            const double r = static_cast<double>(reps);
            return Fmt(s.min_ns / 1e6 / r, 3) + " / " + Fmt(s.median_ns / 1e6 / r, 3) + " / " +
                   Fmt(s.max_ns / 1e6 / r, 3);
        };
        t.Row({f == 0.0 ? "1 row (point)" : Fmt(f * 100.0, 4) + "%", Thousands(index_rows),
               per_query(index_s), Thousands(index_counters.bytes_read / reps),
               Thousands(segments_touched), per_query(scan_s),
               Thousands(scan_counters.bytes_read / reps), Thousands(scan_result.segments_pruned),
               index_wins ? "index" : "scan", Fmt(index_s.median_ns / scan_s.median_ns) + "x"});
    }
    t.Print();

    if (crossover < 0.0) {
        Note("No crossover inside the range measured: the index won everywhere up to 50%\n"
             "selectivity, which means the sweep did not go far enough to find it.");
    } else {
        Note("Crossover: the full scan overtakes the index at about " +
             Fmt(crossover * 100.0, 4) + "% selectivity (" +
             Thousands(static_cast<uint64_t>(crossover * static_cast<double>(table.row_count()))) +
             " of " + Thousands(table.row_count()) +
             " rows).\nThis is the number a cost-based optimizer needs: below it, use the index;\n"
             "above it, scan. Note what drives it - the index's fetch cost is dominated by\n"
             "how many SEGMENTS its hits are spread over, not by how many rows it found,\n"
             "because reading one row of a segment decodes that segment's whole column.");
    }
}

// ---------------------------------------------------------------------------
// 5. buffer pool sensitivity
// ---------------------------------------------------------------------------

void BenchPoolSensitivity(const Workload& w, const std::string& dir, size_t btree_pages) {
    Heading("5. Buffer pool sensitivity (B+ tree)");

    constexpr size_t kRuns = 5;
    const std::string btree_path = dir + "/btree_build.qidx";

    struct Config {
        std::string label;
        double share;
    };
    const std::vector<Config> configs = {
        {"100%", 1.0}, {"50%", 0.5}, {"25%", 0.25}, {"5%", 0.05}};

    std::cout << "tree: " << Thousands(btree_pages) << " pages (" << Bytes(btree_pages * kPageSize)
              << ")   workload: " << Thousands(kLookups) << " random point lookups\n\n";

    Table t({"pool", "frames", "pool bytes", "us/lookup min/med/max", "lookups/sec", "hit rate",
             "misses", "evictions", "dirty evictions", "logical pages/lookup",
             "physical reads/lookup", "vs clock 100%"});

    double baseline_ns = 0.0;
    for (const auto& policy :
         {std::make_pair("clock", BufferPool::Policy::kClock),
          std::make_pair("lru", BufferPool::Policy::kLru)}) {
        for (const auto& cfg : configs) {
            const size_t frames =
                std::max<size_t>(8, static_cast<size_t>(cfg.share * static_cast<double>(
                                                                        btree_pages)));
            BTreeFixture fx(btree_path, frames, policy.second);
            std::vector<uint64_t> out;
            out.reserve(8);

            const Sample s =
                Time(cfg.label, kRuns, static_cast<double>(kLookups), [&] {
                    uint64_t found = 0;
                    for (const int64_t k : w.probes) {
                        out.clear();
                        Check(fx.tree->Lookup(Value::Int64(k), out), "Lookup");
                        found += out.size();
                    }
                    Sink(found);
                });

            const BufferPool::Stats pool = fx.pool->stats();
            const Index::Stats stats = fx.tree->stats();
            if (baseline_ns == 0.0) baseline_ns = s.median_ns;

            const double per = static_cast<double>(kLookups);
            t.Row({std::string(policy.first) + " " + cfg.label, Thousands(frames),
                   Bytes(frames * kPageSize),
                   Fmt(s.min_ns / 1e3 / per, 3) + " / " + Fmt(s.median_ns / 1e3 / per, 3) + " / " +
                       Fmt(s.max_ns / 1e3 / per, 3),
                   Rate(s.PerSecond()), Fmt(100.0 * pool.HitRate(), 2) + "%",
                   Thousands(pool.misses), Thousands(pool.evictions),
                   Thousands(pool.dirty_evictions),
                   Fmt(static_cast<double>(stats.pages_read) /
                           static_cast<double>(stats.lookups == 0 ? 1 : stats.lookups),
                       3),
                   Fmt(static_cast<double>(pool.reads) /
                           static_cast<double>(stats.lookups == 0 ? 1 : stats.lookups),
                       3),
                   Fmt(s.median_ns / baseline_ns) + "x"});
        }
        t.Separator();
    }
    t.Print();

    Note("Logical pages per lookup does not move across pool sizes, because the tree asks\n"
         "for the same pages regardless; physical reads per lookup and the hit rate are\n"
         "what change. If latency does NOT follow the hit rate here, that is a result\n"
         "about this pool and not a measurement error - see results.md, which reports it\n"
         "either way.");
}

// ---------------------------------------------------------------------------

// MakeKeyTable builds the table the crossover benchmark queries: a shuffled key
// permutation plus a payload, so a key range selects a known number of rows that
// are spread uniformly over the segments.
std::vector<ColumnData> MakeKeyTable(const Workload& w, size_t rows) {
    std::vector<ColumnData> cols(2);
    cols[0].type = TypeId::kInt64;
    cols[1].type = TypeId::kDouble;
    cols[0].i64.reserve(rows);
    cols[1].f64.reserve(rows);
    for (size_t i = 0; i < rows; ++i) {
        cols[0].i64.push_back(w.keys[i]);
        cols[1].f64.push_back(static_cast<double>(w.keys[i]) / 64.0);
    }
    return cols;
}

Schema KeyTableSchema() {
    Schema s;
    s.Add(Column("key", TypeId::kInt64, false));
    s.Add(Column("payload", TypeId::kDouble, false));
    return s;
}

}  // namespace

int main() {
    std::cout << "quarry index benchmark\n"
              << "keys=" << Thousands(kKeys) << "  lookups=" << Thousands(kLookups)
              << "  page size=" << kPageSize << "\n";

    const Workload workload = MakeWorkload(kKeys, kLookups);
    const std::string dir = FreshDirectory("index");

    const BuildResult btree = BuildBTree(workload, dir, 1024);
    const BuildResult lsm = BuildLsm(workload, dir);
    const size_t btree_pages = static_cast<size_t>(btree.on_disk_bytes / kPageSize);

    ReportBuild(btree, lsm, btree.stats.logical_bytes);
    BenchPointLookups(workload, dir, btree_pages);
    BenchRangeScans(dir, btree_pages);

    // The crossover needs a real table, which is a separate catalog in the same
    // directory tree.
    const std::string table_dir = FreshDirectory("index-table");
    Catalog catalog(table_dir);
    Check(catalog.Open(), "catalog Open");
    WriteTable(catalog, "keyed", KeyTableSchema(), MakeKeyTable(workload, kTableRows),
               kRowsPerSegment);
    const TableDef& keyed = *Take(catalog.GetTable("keyed"), "GetTable keyed");
    BenchIndexVsScan(keyed, dir, btree_pages);

    BenchPoolSensitivity(workload, dir, btree_pages);
    std::cout << "\n";
    return 0;
}
