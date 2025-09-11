#include "../src/index/btree/btree.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "quarry/buffer.hpp"
#include "testing.hpp"

using namespace quarry;

namespace {

std::string TempFile(const std::string& name) {
    std::string path = "/tmp/quarry_btree_" + name + ".db";
    std::remove(path.c_str());
    return path;
}

// Fixture: a pool and a tree over one file, so a test can drop and rebuild the
// tree to exercise reopen without re-deriving the plumbing.
struct Fixture {
    std::string path;
    std::unique_ptr<BufferPool> pool;
    std::unique_ptr<BTree> tree;

    Fixture(const std::string& name, TypeId key_type, size_t frames = 64,
            BufferPool::Policy policy = BufferPool::Policy::kLru)
        : path(TempFile(name)) {
        Reopen(key_type, frames, policy);
    }

    void Reopen(TypeId key_type, size_t frames = 64,
                BufferPool::Policy policy = BufferPool::Policy::kLru) {
        // Order matters: the tree must stop using the pool before the pool goes.
        tree.reset();
        pool.reset();
        pool = std::make_unique<BufferPool>(path, frames, policy);
        CHECK_OK(pool->Open());
        tree = std::make_unique<BTree>(pool.get(), key_type);
        CHECK_OK(tree->Open());
    }

    void Close() {
        if (tree) CHECK_OK(tree->Flush());
        tree.reset();
        if (pool) CHECK_OK(pool->Close());
        pool.reset();
    }
};

std::vector<uint64_t> Sorted(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<uint64_t> LookupAll(const BTree& tree, const Value& key) {
    std::vector<uint64_t> out;
    CHECK_OK(tree.Lookup(key, out));
    return out;
}

std::vector<uint64_t> RangeAll(const BTree& tree, const std::optional<Value>& lo,
                               const std::optional<Value>& hi) {
    std::vector<uint64_t> out;
    CHECK_OK(tree.RangeScan(lo, hi, out));
    return out;
}

// Keys that sort differently as text than as numbers, so a varchar test cannot
// accidentally pass on numeric ordering.
std::string StringKey(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key-%04d", i);
    return buf;
}

}  // namespace

QUARRY_TEST(EmptyTreeAnswersNothing) {
    Fixture f("empty", TypeId::kInt64);
    CHECK_EQ(f.tree->height(), uint32_t{1});
    CHECK(LookupAll(*f.tree, Value::Int64(5)).empty());
    CHECK(RangeAll(*f.tree, std::nullopt, std::nullopt).empty());
    f.Close();
}

QUARRY_TEST(PointLookupInt64) {
    Fixture f("int64", TypeId::kInt64);
    const int n = 2000;
    for (int i = 0; i < n; ++i) CHECK_OK(f.tree->Insert(Value::Int64(i * 3), uint64_t(i)));

    for (int i = 0; i < n; ++i) {
        auto got = LookupAll(*f.tree, Value::Int64(i * 3));
        CHECK_EQ(got.size(), size_t{1});
        if (got.size() == 1) CHECK_EQ(got[0], uint64_t(i));
    }
    CHECK(LookupAll(*f.tree, Value::Int64(1)).empty());
    CHECK(LookupAll(*f.tree, Value::Int64(-7)).empty());
    CHECK(LookupAll(*f.tree, Value::Int64(n * 3)).empty());

    CHECK_EQ(f.tree->stats().inserts, uint64_t(n));
    CHECK(f.tree->stats().node_splits > 0);
    f.Close();
}

QUARRY_TEST(PointLookupDouble) {
    Fixture f("double", TypeId::kDouble);
    const int n = 1500;
    for (int i = 0; i < n; ++i) {
        CHECK_OK(f.tree->Insert(Value::Double(static_cast<double>(i) * 0.5 - 100.0), uint64_t(i)));
    }
    for (int i = 0; i < n; ++i) {
        auto got = LookupAll(*f.tree, Value::Double(static_cast<double>(i) * 0.5 - 100.0));
        CHECK_EQ(got.size(), size_t{1});
        if (got.size() == 1) CHECK_EQ(got[0], uint64_t(i));
    }
    CHECK(LookupAll(*f.tree, Value::Double(0.25)).empty());
    f.Close();
}

QUARRY_TEST(PointLookupVarchar) {
    Fixture f("varchar", TypeId::kVarchar);
    const int n = 1200;
    for (int i = 0; i < n; ++i) CHECK_OK(f.tree->Insert(Value::Varchar(StringKey(i)), uint64_t(i)));

    for (int i = 0; i < n; ++i) {
        auto got = LookupAll(*f.tree, Value::Varchar(StringKey(i)));
        CHECK_EQ(got.size(), size_t{1});
        if (got.size() == 1) CHECK_EQ(got[0], uint64_t(i));
    }
    CHECK(LookupAll(*f.tree, Value::Varchar("key-9999")).empty());
    CHECK(LookupAll(*f.tree, Value::Varchar("")).empty());
    CHECK(f.tree->stats().node_splits > 0);
    f.Close();
}

QUARRY_TEST(VarcharKeyLengthIsBounded) {
    Fixture f("longkey", TypeId::kVarchar);
    CHECK_OK(f.tree->Insert(Value::Varchar(std::string(BTree::kMaxKeyLength, 'a')), 1));

    // One byte past the limit must be a clear error rather than a corrupt page.
    Status too_long = f.tree->Insert(Value::Varchar(std::string(BTree::kMaxKeyLength + 1, 'a')), 2);
    CHECK(!too_long.ok());
    CHECK(too_long.code() == Code::kInvalidArgument);

    auto got = LookupAll(*f.tree, Value::Varchar(std::string(BTree::kMaxKeyLength, 'a')));
    CHECK_EQ(got.size(), size_t{1});
    f.Close();
}

QUARRY_TEST(WrongKeyTypeAndNullAreRejected) {
    Fixture f("types", TypeId::kInt64);
    CHECK(f.tree->Insert(Value::Varchar("x"), 1).code() == Code::kTypeError);
    CHECK(f.tree->Insert(Value::Null(), 1).code() == Code::kInvalidArgument);
    std::vector<uint64_t> out;
    CHECK(f.tree->Lookup(Value::Double(1.0), out).code() == Code::kTypeError);
    CHECK(out.empty());
    f.Close();
}

QUARRY_TEST(RangeScanBoundsAndEmptyRange) {
    Fixture f("range", TypeId::kInt64);
    const int n = 500;
    for (int i = 0; i < n; ++i) CHECK_OK(f.tree->Insert(Value::Int64(i), uint64_t(i)));

    // Both bounds, inclusive on each end.
    auto closed = RangeAll(*f.tree, Value::Int64(100), Value::Int64(200));
    CHECK_EQ(closed.size(), size_t{101});
    CHECK_EQ(closed.front(), uint64_t{100});
    CHECK_EQ(closed.back(), uint64_t{200});
    // A range scan must come back in key order without re-descending, which is
    // what the leaf next-pointers are for.
    CHECK(std::is_sorted(closed.begin(), closed.end()));

    auto from = RangeAll(*f.tree, Value::Int64(480), std::nullopt);
    CHECK_EQ(from.size(), size_t{20});
    CHECK_EQ(from.front(), uint64_t{480});

    auto upto = RangeAll(*f.tree, std::nullopt, Value::Int64(9));
    CHECK_EQ(upto.size(), size_t{10});
    CHECK_EQ(upto.back(), uint64_t{9});

    auto everything = RangeAll(*f.tree, std::nullopt, std::nullopt);
    CHECK_EQ(everything.size(), size_t(n));

    // Empty: inside the key space, past the end, and inverted.
    CHECK(RangeAll(*f.tree, Value::Int64(1000), Value::Int64(2000)).empty());
    CHECK(RangeAll(*f.tree, Value::Int64(200), Value::Int64(100)).empty());
    CHECK_EQ(RangeAll(*f.tree, Value::Int64(42), Value::Int64(42)).size(), size_t{1});
    f.Close();
}

QUARRY_TEST(RangeScanVarcharBounds) {
    Fixture f("rangestr", TypeId::kVarchar);
    for (int i = 0; i < 400; ++i) CHECK_OK(f.tree->Insert(Value::Varchar(StringKey(i)), uint64_t(i)));

    auto slice = RangeAll(*f.tree, Value::Varchar(StringKey(100)), Value::Varchar(StringKey(149)));
    CHECK_EQ(slice.size(), size_t{50});
    CHECK_EQ(slice.front(), uint64_t{100});
    CHECK_EQ(slice.back(), uint64_t{149});

    // A bound that is not itself a key still has to cut in the right place.
    auto open_ended = RangeAll(*f.tree, Value::Varchar("key-0398x"), std::nullopt);
    CHECK_EQ(open_ended.size(), size_t{1});
    CHECK_EQ(open_ended.front(), uint64_t{399});
    f.Close();
}

QUARRY_TEST(DuplicateKeysReturnEveryRowId) {
    Fixture f("dups", TypeId::kInt64);

    // Few distinct keys and many rows each: the shape of a secondary index on a
    // low-cardinality column, where duplicate runs span whole leaves.
    const int distinct = 20, per_key = 300;
    for (int k = 0; k < distinct; ++k) {
        for (int r = 0; r < per_key; ++r) {
            CHECK_OK(f.tree->Insert(Value::Int64(k), uint64_t(k * 1000 + r)));
        }
    }
    CHECK(f.tree->stats().node_splits > 0);

    for (int k = 0; k < distinct; ++k) {
        auto got = Sorted(LookupAll(*f.tree, Value::Int64(k)));
        CHECK_EQ(got.size(), size_t(per_key));
        bool all_present = got.size() == size_t(per_key);
        for (int r = 0; r < per_key && all_present; ++r) {
            if (got[size_t(r)] != uint64_t(k * 1000 + r)) all_present = false;
        }
        CHECK(all_present);
    }

    // The same key repeated with the same row id is stored again; the index has no
    // uniqueness contract to enforce.
    CHECK_OK(f.tree->Insert(Value::Int64(0), 0));
    CHECK_EQ(LookupAll(*f.tree, Value::Int64(0)).size(), size_t(per_key + 1));

    auto range = RangeAll(*f.tree, Value::Int64(5), Value::Int64(6));
    CHECK_EQ(range.size(), size_t(per_key * 2));
    f.Close();
}

QUARRY_TEST(SplitsGrowTheTreeAndEveryKeyStillAnswers) {
    Fixture f("splits", TypeId::kInt64);
    const int n = 50000;
    for (int i = 0; i < n; ++i) CHECK_OK(f.tree->Insert(Value::Int64(i), uint64_t(i)));

    // 50k keys cannot fit in one or two levels at 4 KiB a node, so this asserts
    // that a root split happened and that the new level is reachable.
    CHECK(f.tree->height() >= 3);
    CHECK(f.tree->stats().node_splits > 0);
    std::printf("      height=%u node_splits=%llu pages_read=%llu pages_written=%llu\n",
                f.tree->height(),
                static_cast<unsigned long long>(f.tree->stats().node_splits),
                static_cast<unsigned long long>(f.tree->stats().pages_read),
                static_cast<unsigned long long>(f.tree->stats().pages_written));

    for (int i = 0; i < n; ++i) {
        auto got = LookupAll(*f.tree, Value::Int64(i));
        if (got.size() != 1 || got[0] != uint64_t(i)) {
            CHECK_EQ(got.size(), size_t{1});
            break;
        }
    }
    CHECK_EQ(RangeAll(*f.tree, std::nullopt, std::nullopt).size(), size_t(n));
    f.Close();
}

QUARRY_TEST(InsertionOrderDoesNotChangeTheAnswer) {
    // Ascending is the pathological case for a naive split point, descending
    // exercises the mirror image, and random is the realistic one.
    const int n = 8000;
    std::vector<int> keys(n);
    std::iota(keys.begin(), keys.end(), 0);

    struct Order {
        const char* name;
        std::vector<int> keys;
    };
    std::vector<Order> orders;
    orders.push_back({"asc", keys});
    std::vector<int> desc(keys.rbegin(), keys.rend());
    orders.push_back({"desc", desc});
    std::vector<int> shuffled = keys;
    std::mt19937 rng(12345);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    orders.push_back({"random", shuffled});

    for (const auto& order : orders) {
        Fixture f(std::string("order_") + order.name, TypeId::kInt64);
        for (int k : order.keys) CHECK_OK(f.tree->Insert(Value::Int64(k), uint64_t(k)));

        CHECK(f.tree->stats().node_splits > 0);
        auto all = RangeAll(*f.tree, std::nullopt, std::nullopt);
        CHECK_EQ(all.size(), size_t(n));
        // Row id equals key here, so an in-order scan must be exactly 0..n-1.
        bool in_order = all.size() == size_t(n);
        for (size_t i = 0; i < all.size() && in_order; ++i) {
            if (all[i] != uint64_t(i)) in_order = false;
        }
        CHECK(in_order);

        for (int k = 0; k < n; k += 7) {
            auto got = LookupAll(*f.tree, Value::Int64(k));
            if (got.size() != 1 || got[0] != uint64_t(k)) {
                CHECK_EQ(got.size(), size_t{1});
                break;
            }
        }
        f.Close();
    }
}

QUARRY_TEST(SurvivesCloseAndReopen) {
    Fixture f("reopen", TypeId::kVarchar);
    const int n = 5000;
    for (int i = 0; i < n; ++i) {
        CHECK_OK(f.tree->Insert(Value::Varchar(StringKey(i)), uint64_t(i)));
        CHECK_OK(f.tree->Insert(Value::Varchar(StringKey(i)), uint64_t(i + 100000)));
    }
    const uint32_t height_before = f.tree->height();
    CHECK(height_before >= 2);
    f.Close();

    // A fresh pool and a fresh tree over the same file: nothing is carried over in
    // memory, so anything found now came off disk.
    f.Reopen(TypeId::kVarchar);
    CHECK_EQ(f.tree->height(), height_before);
    CHECK_EQ(f.tree->stats().inserts, uint64_t{0});

    for (int i = 0; i < n; ++i) {
        auto got = Sorted(LookupAll(*f.tree, Value::Varchar(StringKey(i))));
        if (got.size() != 2 || got[0] != uint64_t(i) || got[1] != uint64_t(i + 100000)) {
            CHECK_EQ(got.size(), size_t{2});
            break;
        }
    }
    CHECK_EQ(RangeAll(*f.tree, std::nullopt, std::nullopt).size(), size_t(n * 2));

    // And it is still writable after a reopen.
    CHECK_OK(f.tree->Insert(Value::Varchar("zzz-after-reopen"), 999999));
    CHECK_EQ(LookupAll(*f.tree, Value::Varchar("zzz-after-reopen")).size(), size_t{1});
    f.Close();

    // Opening with the wrong key type must be refused rather than misread.
    BufferPool pool(f.path, 16, BufferPool::Policy::kLru);
    CHECK_OK(pool.Open());
    BTree mismatched(&pool, TypeId::kInt64);
    CHECK(mismatched.Open().code() == Code::kTypeError);
    CHECK_OK(pool.Close());
}

QUARRY_TEST(TinyBufferPoolForcesConstantEviction) {
    // Four frames cannot hold the meta page, the root, an internal node and a leaf
    // at once for long: every descent re-reads. If the tree ever cached a Page*
    // across a fetch, this is where it dies.
    Fixture f("tiny", TypeId::kInt64, 4, BufferPool::Policy::kClock);
    const int n = 4000;
    for (int i = 0; i < n; ++i) CHECK_OK(f.tree->Insert(Value::Int64((i * 37) % n), uint64_t(i)));

    CHECK(f.pool->stats().evictions > 0);
    CHECK(f.pool->stats().dirty_evictions > 0);
    CHECK(f.tree->height() >= 2);

    for (int i = 0; i < n; ++i) {
        auto got = LookupAll(*f.tree, Value::Int64((i * 37) % n));
        if (got.size() != 1) {
            CHECK_EQ(got.size(), size_t{1});
            break;
        }
    }
    CHECK_EQ(RangeAll(*f.tree, std::nullopt, std::nullopt).size(), size_t(n));
    f.Close();
}

QUARRY_TEST(DifferentialAgainstMultimapInt64) {
    Fixture f("diff_int", TypeId::kInt64, 16, BufferPool::Policy::kLru);
    std::multimap<int64_t, uint64_t> reference;

    std::mt19937_64 rng(987654321);
    // A key space far smaller than the insert count guarantees heavy duplication,
    // which is exactly where a split that mishandles equal keys loses rows.
    std::uniform_int_distribution<int64_t> key_dist(-500, 500);

    uint64_t next_row = 1;
    for (int step = 0; step < 20000; ++step) {
        const int action = step % 10;
        if (action < 7) {
            const int64_t key = key_dist(rng);
            CHECK_OK(f.tree->Insert(Value::Int64(key), next_row));
            reference.emplace(key, next_row);
            ++next_row;
        } else if (action < 9) {
            const int64_t key = key_dist(rng);
            std::vector<uint64_t> expected;
            auto [lo, hi] = reference.equal_range(key);
            for (auto it = lo; it != hi; ++it) expected.push_back(it->second);
            std::sort(expected.begin(), expected.end());

            auto got = Sorted(LookupAll(*f.tree, Value::Int64(key)));
            if (got != expected) {
                CHECK_EQ(got.size(), expected.size());
                CHECK(got == expected);
                break;
            }
        } else {
            int64_t a = key_dist(rng), b = key_dist(rng);
            if (a > b) std::swap(a, b);
            std::vector<uint64_t> expected;
            for (auto it = reference.lower_bound(a); it != reference.upper_bound(b); ++it) {
                expected.push_back(it->second);
            }
            std::sort(expected.begin(), expected.end());

            auto got = Sorted(RangeAll(*f.tree, Value::Int64(a), Value::Int64(b)));
            if (got != expected) {
                CHECK_EQ(got.size(), expected.size());
                CHECK(got == expected);
                break;
            }
        }
    }

    // Final full agreement, key by key and as one ordered scan.
    std::vector<uint64_t> all_expected;
    for (const auto& [key, row] : reference) all_expected.push_back(row);
    std::sort(all_expected.begin(), all_expected.end());
    CHECK(Sorted(RangeAll(*f.tree, std::nullopt, std::nullopt)) == all_expected);

    std::printf("      differential: %zu rows, height=%u splits=%llu\n", reference.size(),
                f.tree->height(),
                static_cast<unsigned long long>(f.tree->stats().node_splits));
    f.Close();
}

QUARRY_TEST(DifferentialAgainstMultimapVarchar) {
    Fixture f("diff_str", TypeId::kVarchar, 8, BufferPool::Policy::kClock);
    std::multimap<std::string, uint64_t> reference;

    std::mt19937_64 rng(13579);
    // Variable-length keys make the byte-based split point matter: a count-based
    // split would occasionally leave a half that does not fit its page.
    std::uniform_int_distribution<int> len_dist(1, 60);
    std::uniform_int_distribution<int> ch_dist('a', 'f');

    uint64_t next_row = 1;
    for (int step = 0; step < 12000; ++step) {
        std::string key(static_cast<size_t>(len_dist(rng)), 'a');
        for (char& c : key) c = static_cast<char>(ch_dist(rng));

        if (step % 3 != 2) {
            CHECK_OK(f.tree->Insert(Value::Varchar(key), next_row));
            reference.emplace(key, next_row);
            ++next_row;
        } else {
            std::vector<uint64_t> expected;
            auto [lo, hi] = reference.equal_range(key);
            for (auto it = lo; it != hi; ++it) expected.push_back(it->second);
            std::sort(expected.begin(), expected.end());

            auto got = Sorted(LookupAll(*f.tree, Value::Varchar(key)));
            if (got != expected) {
                CHECK(got == expected);
                break;
            }
        }
    }

    // An ordered scan must reproduce the reference's text ordering exactly.
    std::vector<uint64_t> expected_in_key_order;
    for (const auto& [key, row] : reference) expected_in_key_order.push_back(row);
    auto scanned = RangeAll(*f.tree, std::nullopt, std::nullopt);
    CHECK_EQ(scanned.size(), expected_in_key_order.size());
    // Equal keys may come back in row-id order, which is the tree's tiebreak, and
    // the reference's insertion order is the same thing here because row ids only
    // increase.
    CHECK(scanned == expected_in_key_order);

    std::printf("      varchar differential: %zu rows, height=%u splits=%llu\n",
                reference.size(), f.tree->height(),
                static_cast<unsigned long long>(f.tree->stats().node_splits));
    f.Close();
}

QUARRY_TEST(StatsAreAccounted) {
    Fixture f("stats", TypeId::kInt64, 32);
    for (int i = 0; i < 1000; ++i) CHECK_OK(f.tree->Insert(Value::Int64(i), uint64_t(i)));
    for (int i = 0; i < 50; ++i) CHECK(LookupAll(*f.tree, Value::Int64(i)).size() == 1);
    CHECK(RangeAll(*f.tree, Value::Int64(0), Value::Int64(10)).size() == 11);

    const auto s = f.tree->stats();
    CHECK_EQ(s.inserts, uint64_t{1000});
    CHECK_EQ(s.lookups, uint64_t{51});  // 50 point lookups and one range scan
    CHECK(s.pages_read > 0);
    CHECK(s.pages_written > 0);
    CHECK(s.node_splits > 0);

    // The LSM-only counters stay zero on this implementation.
    CHECK_EQ(s.compactions, uint64_t{0});
    CHECK_EQ(s.bloom_rejections, uint64_t{0});
    CHECK_EQ(s.sstables_consulted, uint64_t{0});

    CHECK(f.tree->Describe().find("btree") == 0);
    f.Close();
}

QUARRY_TEST_MAIN()
