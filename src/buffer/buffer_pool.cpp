#include "quarry/buffer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace quarry {
namespace {

// Page ids are one-based so that kInvalidPage can be zero and an uninitialized
// child pointer in a tree node is detectably bogus rather than "the first page".
off_t OffsetOf(PageId id) {
    return static_cast<off_t>((id - 1) * kPageSize);
}

std::string Errno(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

}  // namespace

BufferPool::BufferPool(std::string path, size_t frame_count, Policy policy)
    : path_(std::move(path)), policy_(policy) {
    // Frames are allocated once and never resized. FetchPage hands out raw
    // Page*, so any reallocation of this vector would dangle every outstanding
    // pointer; keeping the storage fixed for the pool's lifetime is what makes
    // the raw-pointer interface safe at all.
    frames_.resize(frame_count);
    frame_to_page_.assign(frame_count, kInvalidPage);
    page_to_frame_.reserve(frame_count);
}

BufferPool::~BufferPool() {
    // Best effort: a destructor cannot report an I/O failure, and leaking the
    // descriptor is strictly worse than losing the error text.
    if (fd_ >= 0) {
        FlushAll();
        ::close(fd_);
        fd_ = -1;
    }
}

Status BufferPool::Open() {
    if (fd_ >= 0) return Status::InvalidArgument("buffer pool already open");
    if (frames_.empty()) return Status::InvalidArgument("buffer pool needs at least one frame");

    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) return Status::IoError(Errno("open " + path_));

    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        fd_ = -1;
        return Status::IoError(Errno("fstat " + path_));
    }

    const auto size = static_cast<uint64_t>(st.st_size);
    if (size % kPageSize != 0) {
        ::close(fd_);
        fd_ = -1;
        return Status::Corruption(path_ + " is not a whole number of pages");
    }

    // The file length is the allocation map: everything below it exists, and the
    // next id continues from there. That is what lets a reopened tree keep
    // allocating without a separately persisted free-space structure.
    next_page_id_ = size / kPageSize + 1;
    return Status::Ok();
}

Status BufferPool::Close() {
    if (fd_ < 0) return Status::Ok();
    Status flushed = FlushAll();
    if (::close(fd_) != 0 && flushed.ok()) flushed = Status::IoError(Errno("close " + path_));
    fd_ = -1;

    // Drop residency so a reopened pool cannot serve bytes from a previous
    // incarnation of the file.
    for (auto& frame : frames_) frame = Page{};
    std::fill(frame_to_page_.begin(), frame_to_page_.end(), kInvalidPage);
    page_to_frame_.clear();
    clock_hand_ = 0;
    return flushed;
}

Result<size_t> BufferPool::FindVictim() {
    // A never-used frame is always the best choice: no write, no eviction, no
    // policy bookkeeping.
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frame_to_page_[i] == kInvalidPage) return i;
    }

    size_t victim = frames_.size();
    if (policy_ == Policy::kLru) {
        // page_to_frame_ is kept in access order, least-recent first, so the
        // first unpinned entry from the front is the exact LRU victim. The pool
        // holds few enough frames that this linear structure is also the fastest
        // way to answer the page-table lookup, so one vector serves both roles
        // and there is no separate list to keep consistent.
        for (const auto& [page, frame] : page_to_frame_) {
            if (frames_[frame].pin_count == 0) {
                victim = frame;
                break;
            }
        }
    } else {
        // Clock: at most two sweeps. The first clears reference bits, so a frame
        // that survives on second chance cannot survive the sweep after it, and
        // the scan therefore terminates.
        const size_t limit = frames_.size() * 2;
        for (size_t step = 0; step < limit; ++step) {
            size_t candidate = clock_hand_;
            clock_hand_ = (clock_hand_ + 1) % frames_.size();

            if (frames_[candidate].pin_count != 0) continue;
            if (frames_[candidate].referenced) {
                frames_[candidate].referenced = false;
                continue;
            }
            victim = candidate;
            break;
        }
    }

    if (victim == frames_.size()) {
        // Refusing is the only safe answer. Evicting a pinned frame would hand
        // the holder of that Page* someone else's bytes.
        return Status::Internal("all " + std::to_string(frames_.size()) +
                                " buffer pool frames are pinned");
    }

    Page& frame = frames_[victim];
    if (frame.dirty) {
        QUARRY_TRY(FlushPage(frame.id));
        ++stats_.dirty_evictions;
    }
    ++stats_.evictions;

    auto it = std::find_if(page_to_frame_.begin(), page_to_frame_.end(),
                           [&](const auto& e) { return e.first == frame.id; });
    if (it != page_to_frame_.end()) page_to_frame_.erase(it);

    frame_to_page_[victim] = kInvalidPage;
    frame.id = kInvalidPage;
    frame.dirty = false;
    frame.referenced = false;
    return victim;
}

Result<Page*> BufferPool::FetchPage(PageId id) {
    if (fd_ < 0) return Status::IoError("buffer pool is not open");
    if (id == kInvalidPage || id >= next_page_id_) {
        return Status::InvalidArgument("no such page: " + std::to_string(id));
    }

    auto it = std::find_if(page_to_frame_.begin(), page_to_frame_.end(),
                           [&](const auto& e) { return e.first == id; });
    if (it != page_to_frame_.end()) {
        ++stats_.hits;
        const size_t frame = it->second;

        // Recency is recorded on every access, not on unpin, because a page can
        // be fetched twice before either reference is released.
        if (policy_ == Policy::kLru) {
            auto entry = *it;
            page_to_frame_.erase(it);
            page_to_frame_.push_back(entry);
        }
        frames_[frame].referenced = true;
        ++frames_[frame].pin_count;
        return &frames_[frame];
    }

    ++stats_.misses;
    QUARRY_ASSIGN(size_t frame, FindVictim());

    Page& page = frames_[frame];
    ssize_t n = ::pread(fd_, page.data, kPageSize, OffsetOf(id));
    if (n < 0) return Status::IoError(Errno("read page " + std::to_string(id)));
    if (static_cast<size_t>(n) < kPageSize) {
        // A short read means the page was allocated by extending the file but
        // never written; the rest is logically zero.
        std::memset(page.data + n, 0, kPageSize - static_cast<size_t>(n));
    }
    ++stats_.reads;

    page.id = id;
    page.pin_count = 1;
    page.dirty = false;
    page.referenced = true;
    frame_to_page_[frame] = id;
    page_to_frame_.emplace_back(id, frame);
    return &page;
}

Result<Page*> BufferPool::NewPage() {
    if (fd_ < 0) return Status::IoError("buffer pool is not open");

    const PageId id = next_page_id_;
    // The file is extended before the frame is handed out so that the on-disk
    // length always covers every id the pool has issued. Without that, a crash
    // between allocation and first flush would make the id unreachable on reopen.
    if (::ftruncate(fd_, OffsetOf(id) + static_cast<off_t>(kPageSize)) != 0) {
        return Status::IoError(Errno("extend " + path_));
    }
    ++next_page_id_;

    QUARRY_ASSIGN(size_t frame, FindVictim());

    Page& page = frames_[frame];
    std::memset(page.data, 0, kPageSize);
    page.id = id;
    page.pin_count = 1;
    // A fresh page is dirty on purpose: its zeroed content is meaningful state
    // that a caller may never touch (an empty node), and losing it would make
    // the page read back as whatever the file happened to hold.
    page.dirty = true;
    page.referenced = true;
    frame_to_page_[frame] = id;
    page_to_frame_.emplace_back(id, frame);
    return &page;
}

Status BufferPool::Unpin(PageId id, bool dirty) {
    auto it = std::find_if(page_to_frame_.begin(), page_to_frame_.end(),
                           [&](const auto& e) { return e.first == id; });
    if (it == page_to_frame_.end()) {
        return Status::InvalidArgument("unpin of non-resident page " + std::to_string(id));
    }

    Page& page = frames_[it->second];
    if (page.pin_count == 0) {
        return Status::Internal("unpin of unpinned page " + std::to_string(id));
    }
    --page.pin_count;

    // Dirtiness accumulates: one writer among several readers still means the
    // frame must be written back.
    page.dirty = page.dirty || dirty;
    return Status::Ok();
}

Status BufferPool::FlushPage(PageId id) {
    if (fd_ < 0) return Status::IoError("buffer pool is not open");

    auto it = std::find_if(page_to_frame_.begin(), page_to_frame_.end(),
                           [&](const auto& e) { return e.first == id; });
    if (it == page_to_frame_.end()) return Status::Ok();

    Page& page = frames_[it->second];
    if (!page.dirty) return Status::Ok();

    ssize_t n = ::pwrite(fd_, page.data, kPageSize, OffsetOf(id));
    if (n < 0 || static_cast<size_t>(n) != kPageSize) {
        return Status::IoError(Errno("write page " + std::to_string(id)));
    }
    ++stats_.writes;
    page.dirty = false;
    return Status::Ok();
}

Status BufferPool::FlushAll() {
    if (fd_ < 0) return Status::IoError("buffer pool is not open");

    Status first_failure;
    for (const auto& [page, frame] : page_to_frame_) {
        Status s = FlushPage(page);
        if (!s.ok() && first_failure.ok()) first_failure = s;
    }
    return first_failure;
}

}  // namespace quarry
