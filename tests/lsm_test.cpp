// Tests for the LSM index.
//
// The emphasis is on the two things that would make the B+ tree versus LSM
// comparison a lie rather than a measurement: a silently wrong answer (a Bloom
// false negative, a merge that loses a level, corruption read back as data), and
// an under-reported counter. Several tests therefore assert on Index::Stats as
// well as on the rows.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../src/index/lsm/lsm.hpp"
#include "../src/index/lsm/lsm_codec.hpp"
#include "quarry/storage.hpp"
#include "testing.hpp"

using quarry::Status;
using quarry::Value;
using quarry::lsm::LsmIndex;
using quarry::lsm::LsmOptions;

namespace {

// A fresh directory per test, so a failure leaves nothing behind for the next.
class TempDir {
public:
    explicit TempDir(const std::string& label) {
        static int counter = 0;
        path_ = (std::filesystem::temp_directory_path() /
                 ("quarry-lsm-" + label + "-" + std::to_string(counter++)))
                    .string();
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

std::unique_ptr<LsmIndex> OpenIndex(const std::string& dir, LsmOptions opts) {
    auto result = LsmIndex::Open(dir, opts);
    if (!result.ok()) {
        std::cerr << "        open failed: " << result.status().ToString() << "\n";
        return nullptr;
    }
    return result.take();
}

// Small enough that a few hundred keys produce several tables and at least one
// compaction, which is the state every interesting assertion needs.
LsmOptions SmallOptions() {
    LsmOptions o;
    o.memtable_bytes = 2048;
    o.level_fanout = 4;
    return o;
}

std::vector<uint64_t> LookupAll(const LsmIndex& index, const Value& key) {
    std::vector<uint64_t> out;
    Status s = index.Lookup(key, out);
    if (!s.ok()) {
        std::cerr << "        lookup failed: " << s.ToString() << "\n";
        out.clear();
    }
    return out;
}

std::string Join(const std::vector<uint64_t>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s + "]";
}

std::vector<std::string> SsTablePaths(const std::string& dir) {
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".sst") paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

}  // namespace

QUARRY_TEST(LsmLookupFindsEveryInt64Key) {
    TempDir dir("int64");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    constexpr int64_t kKeys = 2000;
    for (int64_t i = 0; i < kKeys; ++i) {
        CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i) + 1));
    }
    CHECK_OK(index->Flush());

    for (int64_t i = 0; i < kKeys; ++i) {
        auto ids = LookupAll(*index, Value::Int64(i));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() == 1) CHECK_EQ(ids[0], static_cast<uint64_t>(i) + 1);
    }
    CHECK_EQ(index->stats().inserts, static_cast<uint64_t>(kKeys));
}

QUARRY_TEST(LsmLookupFindsEveryDoubleKey) {
    TempDir dir("double");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    // Spans negative, zero and positive so the order-preserving double encoding
    // is exercised on both sides of the sign bit.
    std::vector<double> keys;
    for (int i = -500; i < 500; ++i) keys.push_back(static_cast<double>(i) * 1.5);
    for (size_t i = 0; i < keys.size(); ++i) {
        CHECK_OK(index->Insert(Value::Double(keys[i]), i + 1));
    }
    CHECK_OK(index->Flush());

    for (size_t i = 0; i < keys.size(); ++i) {
        auto ids = LookupAll(*index, Value::Double(keys[i]));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() == 1) CHECK_EQ(ids[0], i + 1);
    }

    // The encoding must also order correctly, not merely round-trip: a range over
    // the negatives alone must not pick up a positive.
    std::vector<uint64_t> negatives;
    CHECK_OK(index->RangeScan(std::nullopt, Value::Double(-0.75), negatives));
    CHECK_EQ(negatives.size(), size_t{500});
}

QUARRY_TEST(LsmLookupFindsEveryVarcharKey) {
    TempDir dir("varchar");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    std::vector<std::string> keys;
    for (int i = 0; i < 1500; ++i) keys.push_back("key-" + std::to_string(i * 7919));
    for (size_t i = 0; i < keys.size(); ++i) {
        CHECK_OK(index->Insert(Value::Varchar(keys[i]), i + 1));
    }
    CHECK_OK(index->Flush());

    for (size_t i = 0; i < keys.size(); ++i) {
        auto ids = LookupAll(*index, Value::Varchar(keys[i]));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() == 1) CHECK_EQ(ids[0], i + 1);
    }
}

QUARRY_TEST(LsmFindsKeysSpreadAcrossMemtableAndTables) {
    TempDir dir("spread");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    constexpr int64_t kKeys = 1200;
    for (int64_t i = 0; i < kKeys; ++i) {
        CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i)));
    }

    // No Flush: the newest keys are still in the memtable while older ones are in
    // level-0 tables and in whatever compaction produced.
    CHECK(index->tables_at_level(0) + index->tables_at_level(1) > 0);

    for (int64_t i = 0; i < kKeys; ++i) {
        auto ids = LookupAll(*index, Value::Int64(i));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() == 1) CHECK_EQ(ids[0], static_cast<uint64_t>(i));
    }
    CHECK(index->stats().sstables_consulted > 0);
}

QUARRY_TEST(LsmDuplicateKeysReturnAllRowIds) {
    TempDir dir("dups");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    // Row ids for one key are spread deliberately: some land in an early table,
    // some in a later one, some stay in the memtable. All three must come back.
    for (uint64_t round = 0; round < 6; ++round) {
        for (int64_t key = 0; key < 200; ++key) {
            CHECK_OK(index->Insert(Value::Int64(key), round * 1000 + static_cast<uint64_t>(key)));
        }
    }

    for (int64_t key = 0; key < 200; ++key) {
        auto ids = LookupAll(*index, Value::Int64(key));
        std::vector<uint64_t> expected;
        for (uint64_t round = 0; round < 6; ++round) {
            expected.push_back(round * 1000 + static_cast<uint64_t>(key));
        }
        std::sort(expected.begin(), expected.end());
        CHECK_EQ(Join(ids), Join(expected));
    }
}

QUARRY_TEST(LsmRangeScanMergesAcrossMemtableAndTableBoundary) {
    TempDir dir("range");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    // Even keys first, then odd keys, so neighbouring keys end up in different
    // tables and a correct scan has to interleave two sources rather than
    // concatenate them.
    constexpr int64_t kMax = 1000;
    for (int64_t i = 0; i < kMax; i += 2) {
        CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i)));
    }
    for (int64_t i = 1; i < kMax; i += 2) {
        CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i)));
    }

    std::vector<uint64_t> got;
    CHECK_OK(index->RangeScan(Value::Int64(100), Value::Int64(299), got));
    std::vector<uint64_t> expected;
    for (int64_t i = 100; i <= 299; ++i) expected.push_back(static_cast<uint64_t>(i));
    CHECK_EQ(Join(got), Join(expected));

    // Unbounded on both sides returns everything, still in key order.
    std::vector<uint64_t> all;
    CHECK_OK(index->RangeScan(std::nullopt, std::nullopt, all));
    CHECK_EQ(all.size(), static_cast<size_t>(kMax));
    CHECK(std::is_sorted(all.begin(), all.end()));

    // A one-sided bound.
    std::vector<uint64_t> tail;
    CHECK_OK(index->RangeScan(Value::Int64(990), std::nullopt, tail));
    CHECK_EQ(tail.size(), size_t{10});

    // An empty range is empty, not everything.
    std::vector<uint64_t> none;
    CHECK_OK(index->RangeScan(Value::Int64(5000), Value::Int64(6000), none));
    CHECK_EQ(none.size(), size_t{0});
}

QUARRY_TEST(LsmCompactionRunsAndKeepsEveryKey) {
    TempDir dir("compact");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    constexpr int64_t kKeys = 8000;
    for (int64_t i = 0; i < kKeys; ++i) {
        CHECK_OK(index->Insert(Value::Int64(i * 3), static_cast<uint64_t>(i) + 1));
    }
    CHECK_OK(index->Flush());

    const auto s = index->stats();
    CHECK(s.compactions > 0);
    CHECK(s.pages_written > 0);

    for (int64_t i = 0; i < kKeys; ++i) {
        auto ids = LookupAll(*index, Value::Int64(i * 3));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() == 1) CHECK_EQ(ids[0], static_cast<uint64_t>(i) + 1);
    }

    std::vector<uint64_t> all;
    CHECK_OK(index->RangeScan(std::nullopt, std::nullopt, all));
    CHECK_EQ(all.size(), static_cast<size_t>(kKeys));
}

QUARRY_TEST(LsmBloomFilterRejectsAbsentKeysWithoutFalseNegatives) {
    TempDir dir("bloom");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    // Present keys are even; probes on odd keys are guaranteed absent.
    constexpr int64_t kKeys = 6000;
    for (int64_t i = 0; i < kKeys; ++i) {
        CHECK_OK(index->Insert(Value::Int64(i * 2), static_cast<uint64_t>(i) + 1));
    }
    CHECK_OK(index->Flush());

    const uint64_t rejections_before = index->stats().bloom_rejections;
    for (int64_t i = 0; i < kKeys; ++i) {
        auto ids = LookupAll(*index, Value::Int64(i * 2 + 1));
        CHECK_EQ(ids.size(), size_t{0});
    }
    const uint64_t rejections = index->stats().bloom_rejections - rejections_before;
    CHECK(rejections > 0);

    // The false-negative check, done exhaustively: every present key must still
    // be found after all those absent probes. A filter bug that cleared a bit
    // would show up here as a missing row and nowhere else, since a rejected
    // table is simply skipped — the failure mode is silence, not an error.
    for (int64_t i = 0; i < kKeys; ++i) {
        auto ids = LookupAll(*index, Value::Int64(i * 2));
        CHECK_EQ(ids.size(), size_t{1});
        if (ids.size() != 1) {
            std::cerr << "        false negative on key " << (i * 2) << "\n";
            break;
        }
    }

    const uint64_t probes = static_cast<uint64_t>(kKeys);
    std::cout << "     bloom: " << rejections << " rejections over " << probes
              << " absent-key lookups\n";
}

QUARRY_TEST(LsmBloomFilterFalsePositiveRateIsNearTarget) {
    // Exercises the filter directly, which is the only way to separate its own
    // false-positive rate from how many tables a lookup happened to touch.
    constexpr uint64_t kEntries = 50000;
    auto filter = quarry::lsm::BloomFilter::Create(kEntries, 0.01);

    for (uint64_t i = 0; i < kEntries; ++i) {
        filter.Add(quarry::lsm::Hash64("present-" + std::to_string(i)));
    }

    uint64_t false_negatives = 0;
    for (uint64_t i = 0; i < kEntries; ++i) {
        if (!filter.MayContain(quarry::lsm::Hash64("present-" + std::to_string(i)))) {
            ++false_negatives;
        }
    }
    CHECK_EQ(false_negatives, uint64_t{0});

    uint64_t false_positives = 0;
    constexpr uint64_t kProbes = 100000;
    for (uint64_t i = 0; i < kProbes; ++i) {
        if (filter.MayContain(quarry::lsm::Hash64("absent-" + std::to_string(i)))) {
            ++false_positives;
        }
    }
    const double rate = static_cast<double>(false_positives) / static_cast<double>(kProbes);
    std::printf("     bloom: %.4f%% false positives (%llu/%llu), %u probes, %.2f bits/key\n",
                rate * 100.0, static_cast<unsigned long long>(false_positives),
                static_cast<unsigned long long>(kProbes), filter.num_probes(),
                static_cast<double>(filter.num_bits()) / static_cast<double>(kEntries));

    // A rate far above the 1% target would mean the two-hash derivation is
    // correlating its probes rather than that the sizing formula is wrong.
    CHECK(rate < 0.03);
}

QUARRY_TEST(LsmSurvivesReopen) {
    TempDir dir("reopen");
    constexpr int64_t kKeys = 3000;

    {
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;
        for (int64_t i = 0; i < kKeys; ++i) {
            CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i) + 1));
        }
        CHECK_OK(index->Flush());
    }

    {
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;
        for (int64_t i = 0; i < kKeys; ++i) {
            auto ids = LookupAll(*index, Value::Int64(i));
            CHECK_EQ(ids.size(), size_t{1});
            if (ids.size() == 1) CHECK_EQ(ids[0], static_cast<uint64_t>(i) + 1);
        }

        // Writing more after a reopen must not lose what was already there.
        for (int64_t i = kKeys; i < kKeys + 500; ++i) {
            CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i) + 1));
        }
    }

    {
        // No explicit Flush above: destruction is the close, so the memtable that
        // was live at destruction must have reached disk.
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;
        std::vector<uint64_t> all;
        CHECK_OK(index->RangeScan(std::nullopt, std::nullopt, all));
        CHECK_EQ(all.size(), static_cast<size_t>(kKeys) + 500);
    }
}

QUARRY_TEST(LsmMatchesMultimapUnderRandomOperations) {
    TempDir dir("diff");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    // A fixed seed: a differential test that cannot be replayed is a flake
    // report rather than a bug report.
    std::mt19937_64 rng(0x5eed1234);
    std::multimap<int64_t, uint64_t> model;

    std::uniform_int_distribution<int> op(0, 9);
    std::uniform_int_distribution<int64_t> key_dist(-300, 300);
    uint64_t next_row_id = 1;
    int failures_reported = 0;

    for (int step = 0; step < 6000 && failures_reported < 5; ++step) {
        const int choice = op(rng);
        if (choice < 6) {
            const int64_t key = key_dist(rng);
            const uint64_t row_id = next_row_id++;
            CHECK_OK(index->Insert(Value::Int64(key), row_id));
            model.emplace(key, row_id);
        } else if (choice < 9) {
            const int64_t key = key_dist(rng);
            auto got = LookupAll(*index, Value::Int64(key));

            std::vector<uint64_t> expected;
            auto range = model.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) expected.push_back(it->second);
            std::sort(expected.begin(), expected.end());

            if (Join(got) != Join(expected)) {
                ++failures_reported;
                CHECK_EQ(Join(got), Join(expected));
            }
        } else {
            int64_t lo = key_dist(rng), hi = key_dist(rng);
            if (lo > hi) std::swap(lo, hi);
            const bool open_lo = (rng() % 8) == 0;
            const bool open_hi = (rng() % 8) == 0;

            std::vector<uint64_t> got;
            Status s = index->RangeScan(open_lo ? std::nullopt : std::optional<Value>(Value::Int64(lo)),
                                        open_hi ? std::nullopt : std::optional<Value>(Value::Int64(hi)),
                                        got);
            CHECK_OK(s);

            // The index returns keys in order and row ids ascending within a key,
            // so the model has to be grouped the same way before comparing.
            std::map<int64_t, std::vector<uint64_t>> grouped;
            for (const auto& [k, v] : model) {
                if (!open_lo && k < lo) continue;
                if (!open_hi && k > hi) continue;
                grouped[k].push_back(v);
            }
            std::vector<uint64_t> expected;
            for (auto& [k, ids] : grouped) {
                std::sort(ids.begin(), ids.end());
                expected.insert(expected.end(), ids.begin(), ids.end());
            }

            if (Join(got) != Join(expected)) {
                ++failures_reported;
                CHECK_EQ(Join(got), Join(expected));
            }
        }
    }

    // A final full agreement check after everything has been flushed and
    // compacted, which is where a merge bug hides.
    CHECK_OK(index->Flush());
    for (int64_t key = -300; key <= 300; ++key) {
        auto got = LookupAll(*index, Value::Int64(key));
        std::vector<uint64_t> expected;
        auto range = model.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) expected.push_back(it->second);
        std::sort(expected.begin(), expected.end());
        CHECK_EQ(Join(got), Join(expected));
    }
    CHECK(index->stats().compactions > 0);
}

QUARRY_TEST(LsmReportsCorruptionRatherThanWrongData) {
    TempDir dir("corrupt");
    {
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;
        for (int64_t i = 0; i < 400; ++i) {
            CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i) + 1));
        }
        CHECK_OK(index->Flush());
    }

    auto paths = SsTablePaths(dir.path());
    CHECK(!paths.empty());
    if (paths.empty()) return;

    // Flip a byte inside the first data chunk of the first table. The footer,
    // the sparse index and the Bloom filter are untouched, so the table still
    // opens and the lookup still reaches the data — which is exactly the case
    // where silently returning the damaged bytes would be worst.
    {
        std::fstream f(paths.front(), std::ios::binary | std::ios::in | std::ios::out);
        CHECK(f.is_open());
        f.seekg(8);
        char byte = 0;
        f.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5a);
        f.seekp(8);
        f.write(&byte, 1);
        f.flush();
    }

    {
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;

        // Some key in the damaged chunk must produce a Corruption, and no lookup
        // may return a row that was never inserted.
        bool saw_corruption = false;
        for (int64_t i = 0; i < 400 && !saw_corruption; ++i) {
            std::vector<uint64_t> out;
            Status s = index->Lookup(Value::Int64(i), out);
            if (s.code() == quarry::Code::kCorruption) {
                saw_corruption = true;
            } else {
                CHECK_OK(s);
                for (uint64_t id : out) CHECK(id >= 1 && id <= 400);
            }
        }
        CHECK(saw_corruption);
    }

    // A table truncated past its footer must fail at open rather than present
    // itself as an empty table.
    {
        std::filesystem::resize_file(paths.front(), 32);
        auto result = LsmIndex::Open(dir.path(), SmallOptions());
        CHECK(!result.ok());
        if (!result.ok()) CHECK_EQ(static_cast<int>(result.status().code()),
                                   static_cast<int>(quarry::Code::kCorruption));
    }
}

QUARRY_TEST(LsmCorruptManifestIsReported) {
    TempDir dir("manifest");
    {
        auto index = OpenIndex(dir.path(), SmallOptions());
        CHECK(index != nullptr);
        if (!index) return;
        for (int64_t i = 0; i < 300; ++i) {
            CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i)));
        }
        CHECK_OK(index->Flush());
    }

    {
        const std::string manifest = dir.path() + "/MANIFEST";
        std::fstream f(manifest, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(f.is_open());
        f.seekp(2);
        char c = 'X';
        f.write(&c, 1);
    }

    auto result = LsmIndex::Open(dir.path(), SmallOptions());
    CHECK(!result.ok());
    if (!result.ok()) {
        CHECK_EQ(static_cast<int>(result.status().code()),
                 static_cast<int>(quarry::Code::kCorruption));
    }
}

QUARRY_TEST(LsmReportsWriteAmplification) {
    TempDir dir("writeamp");
    LsmOptions opts;
    opts.memtable_bytes = 8 * 1024;
    opts.level_fanout = 4;

    auto index = OpenIndex(dir.path(), opts);
    CHECK(index != nullptr);
    if (!index) return;

    constexpr int64_t kKeys = 50000;
    std::mt19937_64 rng(42);
    for (int64_t i = 0; i < kKeys; ++i) {
        CHECK_OK(index->Insert(Value::Int64(static_cast<int64_t>(rng() % 1000000)),
                               static_cast<uint64_t>(i) + 1));
    }
    CHECK_OK(index->Flush());

    const auto s = index->stats();
    const double amp = index->WriteAmplification();

    std::printf(
        "     write amplification: %.2fx  (logical %llu B, written %llu B, "
        "%llu pages written, %llu compactions, %llu pages read)\n",
        amp, static_cast<unsigned long long>(index->logical_bytes()),
        static_cast<unsigned long long>(index->bytes_written()),
        static_cast<unsigned long long>(s.pages_written),
        static_cast<unsigned long long>(s.compactions),
        static_cast<unsigned long long>(s.pages_read));
    std::cout << "     " << index->Describe() << "\n";

    // Above one by construction: compaction rewrote data that was already on
    // disk. A reported 1.0 would mean the compaction bytes went uncounted, which
    // is the specific dishonesty this index must not commit.
    CHECK(s.compactions > 0);
    CHECK(amp > 1.0);
}

QUARRY_TEST(LsmRejectsNullKeys) {
    TempDir dir("null");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    CHECK_NOT_OK(index->Insert(Value::Null(), 1));
    std::vector<uint64_t> out;
    CHECK_NOT_OK(index->Lookup(Value::Null(), out));
}

QUARRY_TEST(LsmStatsCountLookupsAndTablesConsulted) {
    TempDir dir("stats");
    auto index = OpenIndex(dir.path(), SmallOptions());
    CHECK(index != nullptr);
    if (!index) return;

    for (int64_t i = 0; i < 2000; ++i) {
        CHECK_OK(index->Insert(Value::Int64(i), static_cast<uint64_t>(i)));
    }
    CHECK_OK(index->Flush());

    const auto before = index->stats();
    for (int64_t i = 0; i < 100; ++i) {
        std::vector<uint64_t> out;
        CHECK_OK(index->Lookup(Value::Int64(i), out));
    }
    const auto after = index->stats();

    CHECK_EQ(after.lookups - before.lookups, uint64_t{100});
    CHECK(after.pages_read > before.pages_read);
    // Read amplification: a point lookup that found its key still had to look at
    // more than one table, which is the LSM's structural cost.
    CHECK(after.sstables_consulted > before.sstables_consulted);
    CHECK_EQ(after.node_splits, uint64_t{0});
}

QUARRY_TEST_MAIN()
