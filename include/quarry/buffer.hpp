#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quarry/status.hpp"

namespace quarry {

// kPageSize is the unit of transfer between the B+ tree and disk.
//
// 4 KiB matches the page size the OS and the SSD already work in, so a page write
// is one device operation rather than a read-modify-write of a larger block.
constexpr size_t kPageSize = 4096;

using PageId = uint64_t;
constexpr PageId kInvalidPage = 0;

// Page is one fixed-size block of bytes plus the bookkeeping the pool needs.
//
// The distinction this type exists to make: a PageId is a logical address that
// lives forever, while a Page is a physical frame that is only sometimes
// resident. The B+ tree deals in PageIds and asks the pool to materialize them;
// it never holds a pointer across a call that might evict.
struct Page {
    PageId id = kInvalidPage;
    uint8_t data[kPageSize] = {};

    // pin_count is how many callers currently hold this frame. A pinned page can
    // never be evicted — a dangling frame under an in-progress tree descent would
    // be a use-after-free with extra steps.
    uint32_t pin_count = 0;

    // dirty means the in-memory bytes differ from disk, so eviction must write
    // before reusing the frame.
    bool dirty = false;

    // referenced drives the clock replacement policy's second chance.
    bool referenced = false;
};

// BufferPool maps page ids to a bounded set of in-memory frames.
//
// Bounded is the point. A pool large enough to hold the whole file teaches
// nothing; the interesting behaviour — eviction choosing wrongly, a hot root page
// staying resident while leaves churn, write amplification from dirty eviction —
// only appears when the pool is smaller than the data. bench/ runs it
// deliberately undersized.
class BufferPool {
public:
    enum class Policy {
        kClock,  // second-chance; approximates LRU at a fraction of the bookkeeping
        kLru,    // exact recency, tracked by reordering the page table
    };

    BufferPool(std::string path, size_t frame_count, Policy policy);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Status Open();

    // FetchPage returns a pinned frame for id, reading from disk on a miss.
    // The caller MUST Unpin it. Returning a raw pointer rather than a handle is a
    // deliberate simplification, and the one place this code is easy to misuse.
    Result<Page*> FetchPage(PageId id);

    // NewPage allocates a page at the end of the file and returns it pinned.
    Result<Page*> NewPage();

    // Unpin releases a reference. Pass dirty = true if the bytes were modified;
    // getting that wrong loses writes with no error anywhere.
    Status Unpin(PageId id, bool dirty);

    // FlushPage writes one page if dirty. FlushAll is the checkpoint.
    Status FlushPage(PageId id);
    Status FlushAll();

    Status Close();

    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        uint64_t dirty_evictions = 0;  // the ones that cost a write
        uint64_t reads = 0;
        uint64_t writes = 0;

        double HitRate() const {
            uint64_t total = hits + misses;
            return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
        }
    };

    Stats stats() const { return stats_; }
    void ResetStats() { stats_ = {}; }

    size_t frame_count() const { return frames_.size(); }

private:
    Result<size_t> FindVictim();

    std::string path_;
    Policy policy_;
    int fd_ = -1;

    std::vector<Page> frames_;
    std::vector<PageId> frame_to_page_;
    // page_to_frame is a flat map; at the frame counts used here a vector scan or
    // a small hash both work, and the implementation picks one and says why.
    std::vector<std::pair<PageId, size_t>> page_to_frame_;

    size_t clock_hand_ = 0;
    PageId next_page_id_ = 1;
    Stats stats_;
};

}  // namespace quarry
