#include "quarry/buffer.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "testing.hpp"

using namespace quarry;

namespace {

// Each test gets its own file, removed up front, so a crashed run cannot leave
// state that makes the next run pass for the wrong reason.
std::string TempFile(const std::string& name) {
    std::string path = "/tmp/quarry_buffer_" + name + ".db";
    std::remove(path.c_str());
    return path;
}

void FillPage(Page* page, uint8_t marker) {
    for (size_t i = 0; i < kPageSize; ++i) page->data[i] = static_cast<uint8_t>(marker + i % 7);
}

bool PageHas(const Page* page, uint8_t marker) {
    for (size_t i = 0; i < kPageSize; ++i) {
        if (page->data[i] != static_cast<uint8_t>(marker + i % 7)) return false;
    }
    return true;
}

// Allocates count pages, stamping each with a recognizable pattern.
std::vector<PageId> Populate(BufferPool& pool, size_t count) {
    std::vector<PageId> ids;
    for (size_t i = 0; i < count; ++i) {
        auto page = pool.NewPage();
        CHECK(page.ok());
        if (!page.ok()) return ids;
        ids.push_back(page.value()->id);
        FillPage(page.value(), static_cast<uint8_t>(i + 1));
        CHECK_OK(pool.Unpin(ids.back(), true));
    }
    return ids;
}

}  // namespace

QUARRY_TEST(OpenCreatesFileAndAllocatesSequentialIds) {
    BufferPool pool(TempFile("open"), 8, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());

    auto first = pool.NewPage();
    CHECK(first.ok());
    CHECK_EQ(first.value()->id, PageId{1});
    CHECK_OK(pool.Unpin(1, true));

    auto second = pool.NewPage();
    CHECK(second.ok());
    CHECK_EQ(second.value()->id, PageId{2});
    CHECK_OK(pool.Unpin(2, true));

    CHECK_OK(pool.Close());
}

QUARRY_TEST(FetchOfUnallocatedPageFails) {
    BufferPool pool(TempFile("bounds"), 4, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());

    CHECK(!pool.FetchPage(1).ok());           // nothing allocated yet
    CHECK(!pool.FetchPage(kInvalidPage).ok());

    Populate(pool, 1);
    CHECK(pool.FetchPage(1).ok());
    CHECK_OK(pool.Unpin(1, false));
    CHECK(!pool.FetchPage(2).ok());

    CHECK_OK(pool.Close());
}

QUARRY_TEST(HitAndMissAccounting) {
    const std::string path = TempFile("hits");
    BufferPool pool(path, 8, BufferPool::Policy::kLru);
    CHECK_OK(pool.Open());
    auto ids = Populate(pool, 4);
    pool.ResetStats();

    // Every page is still resident, so these are hits and cost no reads.
    for (PageId id : ids) {
        auto page = pool.FetchPage(id);
        CHECK(page.ok());
        CHECK_OK(pool.Unpin(id, false));
    }
    CHECK_EQ(pool.stats().hits, uint64_t{4});
    CHECK_EQ(pool.stats().misses, uint64_t{0});
    CHECK_EQ(pool.stats().reads, uint64_t{0});

    CHECK_OK(pool.FlushAll());
    CHECK_OK(pool.Close());

    BufferPool reopened(path, 8, BufferPool::Policy::kLru);
    CHECK_OK(reopened.Open());
    for (PageId id : ids) {
        auto page = reopened.FetchPage(id);
        CHECK(page.ok());
        CHECK_OK(reopened.Unpin(id, false));
    }
    CHECK_EQ(reopened.stats().misses, uint64_t{4});
    CHECK_EQ(reopened.stats().reads, uint64_t{4});
    CHECK_EQ(reopened.stats().hits, uint64_t{0});
    CHECK_OK(reopened.Close());
}

QUARRY_TEST(PinnedPageIsNeverEvicted) {
    const std::string path = TempFile("pinned");
    BufferPool pool(path, 4, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());
    auto ids = Populate(pool, 16);

    // Hold page 1 pinned, then run far more traffic than the pool can hold.
    auto held = pool.FetchPage(ids[0]);
    CHECK(held.ok());
    Page* pinned = held.value();

    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 1; i < ids.size(); ++i) {
            auto page = pool.FetchPage(ids[i]);
            CHECK(page.ok());
            CHECK_OK(pool.Unpin(ids[i], false));
        }
    }

    // The pointer handed out before all that traffic must still describe page 1
    // with its original bytes: this is the invariant the whole pin count exists for.
    CHECK_EQ(pinned->id, ids[0]);
    CHECK_EQ(pinned->pin_count, uint32_t{1});
    CHECK(PageHas(pinned, 1));
    CHECK_OK(pool.Unpin(ids[0], false));
    CHECK_OK(pool.Close());
}

QUARRY_TEST(AllFramesPinnedReturnsError) {
    BufferPool pool(TempFile("exhausted"), 3, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());
    auto ids = Populate(pool, 6);

    for (size_t i = 0; i < 3; ++i) {
        auto page = pool.FetchPage(ids[i]);
        CHECK(page.ok());
    }
    pool.ResetStats();

    auto denied = pool.FetchPage(ids[3]);
    CHECK(!denied.ok());
    // A failed fetch must not have disturbed the pool: everything still pinned,
    // nothing evicted.
    CHECK_EQ(pool.stats().evictions, uint64_t{0});
    CHECK(!pool.NewPage().ok());

    for (size_t i = 0; i < 3; ++i) CHECK_OK(pool.Unpin(ids[i], false));

    // With a frame released the same fetch succeeds.
    auto allowed = pool.FetchPage(ids[3]);
    CHECK(allowed.ok());
    CHECK_OK(pool.Unpin(ids[3], false));
    CHECK_OK(pool.Close());
}

QUARRY_TEST(DirtyPageIsWrittenOnEvictionAndReadsBackCorrectly) {
    BufferPool pool(TempFile("dirty"), 2, BufferPool::Policy::kLru);
    CHECK_OK(pool.Open());

    auto ids = Populate(pool, 6);  // 6 pages through 2 frames: forced dirty eviction
    CHECK(pool.stats().dirty_evictions > 0);
    CHECK_EQ(pool.stats().writes, pool.stats().dirty_evictions);

    for (size_t i = 0; i < ids.size(); ++i) {
        auto page = pool.FetchPage(ids[i]);
        CHECK(page.ok());
        if (page.ok()) CHECK(PageHas(page.value(), static_cast<uint8_t>(i + 1)));
        CHECK_OK(pool.Unpin(ids[i], false));
    }

    // Modify through an eviction cycle and confirm the change, not the original,
    // is what comes back.
    auto page = pool.FetchPage(ids[0]);
    CHECK(page.ok());
    FillPage(page.value(), 99);
    CHECK_OK(pool.Unpin(ids[0], true));
    for (size_t i = 1; i < ids.size(); ++i) {
        CHECK(pool.FetchPage(ids[i]).ok());
        CHECK_OK(pool.Unpin(ids[i], false));
    }
    auto again = pool.FetchPage(ids[0]);
    CHECK(again.ok());
    if (again.ok()) CHECK(PageHas(again.value(), 99));
    CHECK_OK(pool.Unpin(ids[0], false));
    CHECK_OK(pool.Close());
}

QUARRY_TEST(CleanEvictionCostsNoWrite) {
    BufferPool pool(TempFile("clean"), 2, BufferPool::Policy::kLru);
    CHECK_OK(pool.Open());
    auto ids = Populate(pool, 4);
    CHECK_OK(pool.FlushAll());
    pool.ResetStats();

    for (PageId id : ids) {
        CHECK(pool.FetchPage(id).ok());
        CHECK_OK(pool.Unpin(id, false));
    }
    CHECK(pool.stats().evictions > 0);
    CHECK_EQ(pool.stats().dirty_evictions, uint64_t{0});
    CHECK_EQ(pool.stats().writes, uint64_t{0});
    CHECK_OK(pool.Close());
}

QUARRY_TEST(LruEvictsTheLeastRecentlyUsedPage) {
    BufferPool pool(TempFile("lru"), 3, BufferPool::Policy::kLru);
    CHECK_OK(pool.Open());
    auto ids = Populate(pool, 4);
    CHECK_OK(pool.FlushAll());

    // Make residency exactly {1, 2, 3} with 1 the least recently touched.
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pool.FetchPage(ids[i]).ok());
        CHECK_OK(pool.Unpin(ids[i], false));
    }
    CHECK(pool.FetchPage(ids[3]).ok());  // page 4 arrives, page 1 must go
    CHECK_OK(pool.Unpin(ids[3], false));
    pool.ResetStats();

    CHECK(pool.FetchPage(ids[0]).ok());  // the evicted one: a miss
    CHECK_OK(pool.Unpin(ids[0], false));
    CHECK_EQ(pool.stats().misses, uint64_t{1});

    // ...and the two pages touched after it were kept.
    pool.ResetStats();
    CHECK(pool.FetchPage(ids[2]).ok());
    CHECK_OK(pool.Unpin(ids[2], false));
    CHECK(pool.FetchPage(ids[3]).ok());
    CHECK_OK(pool.Unpin(ids[3], false));
    CHECK_EQ(pool.stats().hits, uint64_t{2});
    CHECK_EQ(pool.stats().misses, uint64_t{0});
    CHECK_OK(pool.Close());
}

QUARRY_TEST(ClockGivesASecondChanceThenEvicts) {
    const std::string path = TempFile("clock");
    {
        BufferPool builder(path, 8, BufferPool::Policy::kClock);
        CHECK_OK(builder.Open());
        Populate(builder, 5);
        CHECK_OK(builder.Close());
    }

    // A fresh pool gives the clock hand a known starting position, which is the
    // only way to assert the second chance rather than just "something was evicted".
    BufferPool pool(path, 3, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());
    for (PageId id = 1; id <= 3; ++id) {
        CHECK(pool.FetchPage(id).ok());
        CHECK_OK(pool.Unpin(id, false));
    }

    // Every frame is referenced, so the first sweep spends its second chances and
    // the second sweep takes the frame the hand started on: page 1.
    CHECK(pool.FetchPage(4).ok());
    CHECK_OK(pool.Unpin(4, false));
    CHECK_EQ(pool.stats().evictions, uint64_t{1});

    // Touch page 2 so it carries a reference bit into the next eviction.
    CHECK(pool.FetchPage(2).ok());
    CHECK_OK(pool.Unpin(2, false));

    CHECK(pool.FetchPage(5).ok());
    CHECK_OK(pool.Unpin(5, false));
    pool.ResetStats();

    // Page 2 was spared by its reference bit; page 3, which had none, was not.
    CHECK(pool.FetchPage(2).ok());
    CHECK_OK(pool.Unpin(2, false));
    CHECK_EQ(pool.stats().hits, uint64_t{1});
    CHECK_EQ(pool.stats().misses, uint64_t{0});

    pool.ResetStats();
    CHECK(pool.FetchPage(3).ok());
    CHECK_OK(pool.Unpin(3, false));
    CHECK_EQ(pool.stats().misses, uint64_t{1});
    CHECK_OK(pool.Close());
}

QUARRY_TEST(HitRateTracksPoolSizeAgainstWorkingSet) {
    const std::string path = TempFile("hitrate");
    const size_t working_set = 32;

    {
        BufferPool builder(path, working_set, BufferPool::Policy::kLru);
        CHECK_OK(builder.Open());
        Populate(builder, working_set);
        CHECK_OK(builder.Close());
    }

    auto sweep_hit_rate = [&](size_t frames) {
        BufferPool pool(path, frames, BufferPool::Policy::kLru);
        CHECK_OK(pool.Open());
        for (int pass = 0; pass < 10; ++pass) {
            for (PageId id = 1; id <= working_set; ++id) {
                CHECK(pool.FetchPage(id).ok());
                CHECK_OK(pool.Unpin(id, false));
            }
        }
        double rate = pool.stats().HitRate();
        CHECK_OK(pool.Close());
        return rate;
    };

    // A pool that holds the whole working set misses only on the first pass.
    const double roomy = sweep_hit_rate(working_set);
    // A pool smaller than the working set under a cyclic sweep is the classic LRU
    // worst case: every access evicts the page that is needed next.
    const double cramped = sweep_hit_rate(4);

    CHECK(roomy > 0.85);
    CHECK(cramped < 0.05);
    CHECK(roomy > cramped);
}

QUARRY_TEST(UnpinAccountingIsStrict) {
    BufferPool pool(TempFile("unpin"), 4, BufferPool::Policy::kClock);
    CHECK_OK(pool.Open());
    Populate(pool, 1);

    CHECK_NOT_OK(pool.Unpin(1, false));   // already released by Populate
    CHECK_NOT_OK(pool.Unpin(99, false));  // not resident at all

    // Two holders, one of which writes: the frame stays dirty and pinned until
    // both release.
    CHECK(pool.FetchPage(1).ok());
    CHECK(pool.FetchPage(1).ok());
    CHECK_OK(pool.Unpin(1, true));
    CHECK_OK(pool.Unpin(1, false));
    pool.ResetStats();
    CHECK_OK(pool.FlushPage(1));
    CHECK_EQ(pool.stats().writes, uint64_t{1});
    CHECK_OK(pool.FlushPage(1));  // no longer dirty
    CHECK_EQ(pool.stats().writes, uint64_t{1});
    CHECK_OK(pool.Close());
}

QUARRY_TEST(DataSurvivesCloseAndReopenUnderBothPolicies) {
    for (auto policy : {BufferPool::Policy::kClock, BufferPool::Policy::kLru}) {
        const std::string path = TempFile("persist");
        std::vector<PageId> ids;
        {
            BufferPool pool(path, 3, policy);
            CHECK_OK(pool.Open());
            ids = Populate(pool, 12);
            CHECK_OK(pool.Close());
        }
        BufferPool pool(path, 3, policy);
        CHECK_OK(pool.Open());
        for (size_t i = 0; i < ids.size(); ++i) {
            auto page = pool.FetchPage(ids[i]);
            CHECK(page.ok());
            if (page.ok()) CHECK(PageHas(page.value(), static_cast<uint8_t>(i + 1)));
            CHECK_OK(pool.Unpin(ids[i], false));
        }
        CHECK_OK(pool.Close());
    }
}

QUARRY_TEST_MAIN()
