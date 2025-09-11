#include "btree.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace quarry {
namespace {

// The meta page is always the first page of the file, so a reopen can find the
// root without a side file.
constexpr PageId kMetaPage = 1;
constexpr uint32_t kMagic = 0x51425431;  // "QBT1"

constexpr uint8_t kLeafTag = 1;
constexpr uint8_t kInternalTag = 2;

// Common node header: tag, key type, entry count, padding, leaf sibling.
constexpr size_t kNodeHeaderSize = 16;

void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

uint16_t Get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, sizeof(v)); return v; }
uint32_t Get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, sizeof(v)); return v; }
uint64_t Get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, sizeof(v)); return v; }

}  // namespace

BTree::BTree(BufferPool* pool, TypeId key_type) : pool_(pool), key_type_(key_type) {}

// --- keys ------------------------------------------------------------------

bool BTree::Less(const Entry& a, const Entry& b) const {
    switch (key_type_) {
        case TypeId::kInt64:
            if (a.i64 != b.i64) return a.i64 < b.i64;
            break;
        case TypeId::kDouble:
            if (a.f64 != b.f64) return a.f64 < b.f64;
            break;
        default:
            if (a.str != b.str) return a.str < b.str;
            break;
    }
    // The row id is part of the ordering key, which is what makes every composite
    // key unique and keeps duplicate user keys out of the split logic.
    return a.row_id < b.row_id;
}

Status BTree::MakeEntry(const Value& key, uint64_t row_id, Entry& out) const {
    if (key.is_null()) {
        return Status::InvalidArgument("a B+ tree key cannot be NULL");
    }
    if (key.type() != key_type_) {
        return Status::TypeError(std::string("index key is ") + TypeName(key_type_) +
                                 ", got " + TypeName(key.type()));
    }
    out = Entry{};
    out.row_id = row_id;
    switch (key_type_) {
        case TypeId::kInt64:
            out.i64 = key.as_int64();
            break;
        case TypeId::kDouble:
            out.f64 = key.as_double();
            break;
        case TypeId::kVarchar:
            if (key.as_varchar().size() > kMaxKeyLength) {
                return Status::InvalidArgument("varchar index key of " +
                                               std::to_string(key.as_varchar().size()) +
                                               " bytes exceeds the " +
                                               std::to_string(kMaxKeyLength) + " byte maximum");
            }
            out.str = key.as_varchar();
            break;
        default:
            return Status::NotImplemented(std::string("B+ tree keys of type ") +
                                          TypeName(key_type_));
    }
    return Status::Ok();
}

// --- page layout -----------------------------------------------------------

size_t BTree::EntrySize(const Entry& e) const {
    // row id, plus the key: fixed width, or a two-byte length prefix and bytes.
    if (key_type_ == TypeId::kVarchar) return sizeof(uint16_t) + e.str.size() + sizeof(uint64_t);
    return sizeof(uint64_t) + sizeof(uint64_t);
}

size_t BTree::NodeSize(const Node& n) const {
    size_t size = kNodeHeaderSize;
    if (!n.leaf) size += sizeof(uint64_t);  // the leftmost child
    for (const auto& e : n.entries) {
        size += EntrySize(e);
        if (!n.leaf) size += sizeof(uint64_t);
    }
    return size;
}

void BTree::SerializeNode(const Node& n, uint8_t* page) const {
    std::memset(page, 0, kPageSize);
    page[0] = n.leaf ? kLeafTag : kInternalTag;
    page[1] = static_cast<uint8_t>(key_type_);
    Put16(page + 2, static_cast<uint16_t>(n.entries.size()));
    Put32(page + 4, 0);
    Put64(page + 8, n.next);

    size_t at = kNodeHeaderSize;
    if (!n.leaf) {
        Put64(page + at, n.children[0]);
        at += sizeof(uint64_t);
    }
    for (size_t i = 0; i < n.entries.size(); ++i) {
        const Entry& e = n.entries[i];
        if (key_type_ == TypeId::kVarchar) {
            Put16(page + at, static_cast<uint16_t>(e.str.size()));
            at += sizeof(uint16_t);
            std::memcpy(page + at, e.str.data(), e.str.size());
            at += e.str.size();
        } else if (key_type_ == TypeId::kInt64) {
            Put64(page + at, static_cast<uint64_t>(e.i64));
            at += sizeof(uint64_t);
        } else {
            uint64_t bits;
            std::memcpy(&bits, &e.f64, sizeof(bits));
            Put64(page + at, bits);
            at += sizeof(uint64_t);
        }
        Put64(page + at, e.row_id);
        at += sizeof(uint64_t);
        if (!n.leaf) {
            Put64(page + at, n.children[i + 1]);
            at += sizeof(uint64_t);
        }
    }
}

Status BTree::DeserializeNode(const uint8_t* page, Node& out) const {
    const uint8_t tag = page[0];
    if (tag != kLeafTag && tag != kInternalTag) {
        return Status::Corruption("B+ tree node has tag " + std::to_string(tag));
    }
    if (page[1] != static_cast<uint8_t>(key_type_)) {
        return Status::Corruption("B+ tree node key type does not match the index");
    }

    out = Node{};
    out.leaf = tag == kLeafTag;
    out.next = Get64(page + 8);
    const size_t count = Get16(page + 2);

    size_t at = kNodeHeaderSize;
    // Every read of a length or an offset is bounds-checked against the page:
    // a corrupt count must become a Status, not a walk off the end of the frame.
    auto need = [&](size_t bytes) { return at + bytes <= kPageSize; };

    if (!out.leaf) {
        if (!need(sizeof(uint64_t))) return Status::Corruption("truncated B+ tree node");
        out.children.push_back(Get64(page + at));
        at += sizeof(uint64_t);
    }
    out.entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Entry e;
        if (key_type_ == TypeId::kVarchar) {
            if (!need(sizeof(uint16_t))) return Status::Corruption("truncated B+ tree key");
            const size_t len = Get16(page + at);
            at += sizeof(uint16_t);
            if (len > kMaxKeyLength || !need(len)) {
                return Status::Corruption("B+ tree key length " + std::to_string(len) +
                                          " does not fit the page");
            }
            e.str.assign(reinterpret_cast<const char*>(page + at), len);
            at += len;
        } else {
            if (!need(sizeof(uint64_t))) return Status::Corruption("truncated B+ tree key");
            const uint64_t bits = Get64(page + at);
            at += sizeof(uint64_t);
            if (key_type_ == TypeId::kInt64) {
                e.i64 = static_cast<int64_t>(bits);
            } else {
                std::memcpy(&e.f64, &bits, sizeof(e.f64));
            }
        }
        if (!need(sizeof(uint64_t))) return Status::Corruption("truncated B+ tree entry");
        e.row_id = Get64(page + at);
        at += sizeof(uint64_t);
        out.entries.push_back(std::move(e));

        if (!out.leaf) {
            if (!need(sizeof(uint64_t))) return Status::Corruption("truncated B+ tree child");
            out.children.push_back(Get64(page + at));
            at += sizeof(uint64_t);
        }
    }
    return Status::Ok();
}

// --- page access -----------------------------------------------------------

Status BTree::ReadNode(PageId id, Node& out) const {
    auto fetched = pool_->FetchPage(id);
    if (!fetched.ok()) return fetched.status();
    ++stats_.pages_read;

    // The node is copied out and the frame released before returning, so a caller
    // can recurse or allocate without any risk that this page is evicted beneath
    // a pointer it still holds.
    Status parsed = DeserializeNode(fetched.value()->data, out);
    Status unpinned = pool_->Unpin(id, false);
    return parsed.ok() ? unpinned : parsed;
}

Status BTree::WriteNode(PageId id, const Node& n) {
    auto fetched = pool_->FetchPage(id);
    if (!fetched.ok()) return fetched.status();
    ++stats_.pages_read;

    SerializeNode(n, fetched.value()->data);
    ++stats_.pages_written;
    return pool_->Unpin(id, true);
}

Status BTree::AllocNode(const Node& n, PageId& id) {
    auto fetched = pool_->NewPage();
    if (!fetched.ok()) return fetched.status();

    Page* page = fetched.value();
    id = page->id;
    SerializeNode(n, page->data);
    ++stats_.pages_written;
    return pool_->Unpin(id, true);
}

Status BTree::WriteMeta() {
    auto fetched = pool_->FetchPage(kMetaPage);
    if (!fetched.ok()) return fetched.status();

    uint8_t* p = fetched.value()->data;
    Put32(p, kMagic);
    p[4] = static_cast<uint8_t>(key_type_);
    Put32(p + 8, height_);
    Put64(p + 16, root_);
    ++stats_.pages_written;
    return pool_->Unpin(kMetaPage, true);
}

Status BTree::Open() {
    if (root_ != kInvalidPage) return Status::InvalidArgument("index is already open");

    auto meta = pool_->FetchPage(kMetaPage);
    if (meta.ok()) {
        const uint8_t* p = meta.value()->data;
        const uint32_t magic = Get32(p);
        const auto stored_type = static_cast<TypeId>(p[4]);
        const uint32_t height = Get32(p + 8);
        const PageId root = Get64(p + 16);
        QUARRY_TRY(pool_->Unpin(kMetaPage, false));

        if (magic != kMagic) {
            return Status::Corruption("file is not a quarry B+ tree index");
        }
        if (stored_type != key_type_) {
            return Status::TypeError(std::string("index on disk has key type ") +
                                     TypeName(stored_type) + ", opened as " +
                                     TypeName(key_type_));
        }
        if (root == kInvalidPage || height == 0) {
            return Status::Corruption("B+ tree meta page has no root");
        }
        root_ = root;
        height_ = height;
        return Status::Ok();
    }

    // The pool reports no such page only when the file does not yet contain one,
    // which is the signal that this is a fresh index rather than an I/O failure.
    if (meta.status().code() != Code::kInvalidArgument) return meta.status();

    auto fresh_meta = pool_->NewPage();
    if (!fresh_meta.ok()) return fresh_meta.status();
    const PageId meta_id = fresh_meta.value()->id;
    QUARRY_TRY(pool_->Unpin(meta_id, true));
    if (meta_id != kMetaPage) {
        return Status::Internal("B+ tree must own the first page of its file");
    }

    Node root;
    root.leaf = true;
    QUARRY_TRY(AllocNode(root, root_));
    height_ = 1;
    return WriteMeta();
}

// --- splitting -------------------------------------------------------------

Status BTree::SplitNode(Node& left, Node& right, Entry& separator) const {
    const size_t n = left.entries.size();
    if (n < (left.leaf ? 2u : 3u)) {
        return Status::Internal("B+ tree node too small to split");
    }

    // Split on cumulative bytes rather than entry count: with variable-length
    // keys an even count split can leave one half still over the page limit.
    size_t payload = 0;
    for (const auto& e : left.entries) {
        payload += EntrySize(e) + (left.leaf ? 0 : sizeof(uint64_t));
    }

    size_t idx = 0, running = 0;
    for (size_t i = 0; i < n; ++i) {
        running += EntrySize(left.entries[i]) + (left.leaf ? 0 : sizeof(uint64_t));
        if (running * 2 >= payload) {
            idx = i;
            break;
        }
    }
    const size_t max_idx = left.leaf ? n - 1 : n - 2;
    idx = std::clamp(idx, size_t{1}, max_idx);

    right = Node{};
    right.leaf = left.leaf;

    if (left.leaf) {
        // The separator is a copy of the right node's first key, so a descent that
        // lands exactly on it goes right.
        separator = left.entries[idx];
        right.entries.assign(std::make_move_iterator(left.entries.begin() + static_cast<long>(idx)),
                             std::make_move_iterator(left.entries.end()));
        left.entries.resize(idx);
        right.next = left.next;  // the caller links left to right once right has an id
    } else {
        // An internal split pushes the median key up instead of copying it: it is
        // already represented by the two children it separates.
        separator = std::move(left.entries[idx]);
        right.entries.assign(
            std::make_move_iterator(left.entries.begin() + static_cast<long>(idx) + 1),
            std::make_move_iterator(left.entries.end()));
        right.children.assign(left.children.begin() + static_cast<long>(idx) + 1,
                              left.children.end());
        left.entries.resize(idx);
        left.children.resize(idx + 1);
    }

    if (NodeSize(left) > kPageSize || NodeSize(right) > kPageSize) {
        return Status::Internal("B+ tree split produced a node that still overflows");
    }
    return Status::Ok();
}

Status BTree::InsertInto(PageId id, const Entry& e, Split& split) {
    Node node;
    QUARRY_TRY(ReadNode(id, node));

    const auto less = [this](const Entry& a, const Entry& b) { return Less(a, b); };

    if (node.leaf) {
        const auto pos = std::lower_bound(node.entries.begin(), node.entries.end(), e, less);
        node.entries.insert(pos, e);
    } else {
        // upper_bound: an entry equal to a separator belongs in the subtree to its
        // right, which is the same rule the separator was created under.
        const size_t child_idx = static_cast<size_t>(
            std::upper_bound(node.entries.begin(), node.entries.end(), e, less) -
            node.entries.begin());

        Split child_split;
        QUARRY_TRY(InsertInto(node.children[child_idx], e, child_split));
        if (!child_split.happened) return Status::Ok();  // this node is unchanged

        node.entries.insert(node.entries.begin() + static_cast<long>(child_idx),
                            std::move(child_split.separator));
        node.children.insert(node.children.begin() + static_cast<long>(child_idx) + 1,
                             child_split.right);
    }

    if (NodeSize(node) <= kPageSize) return WriteNode(id, node);

    Node right;
    Entry separator;
    QUARRY_TRY(SplitNode(node, right, separator));

    PageId right_id = kInvalidPage;
    QUARRY_TRY(AllocNode(right, right_id));
    if (node.leaf) node.next = right_id;
    QUARRY_TRY(WriteNode(id, node));

    ++stats_.node_splits;
    split.happened = true;
    split.separator = std::move(separator);
    split.right = right_id;
    return Status::Ok();
}

Status BTree::Insert(const Value& key, uint64_t row_id) {
    if (root_ == kInvalidPage) return Status::Internal("index used before Open()");

    Entry e;
    QUARRY_TRY(MakeEntry(key, row_id, e));

    Split split;
    QUARRY_TRY(InsertInto(root_, e, split));
    ++stats_.inserts;

    if (!split.happened) return Status::Ok();

    // The old root stays where it is and a new level appears above it, which is
    // why the root page id is stored in the meta page rather than being fixed.
    Node new_root;
    new_root.leaf = false;
    new_root.entries.push_back(std::move(split.separator));
    new_root.children = {root_, split.right};

    PageId new_root_id = kInvalidPage;
    QUARRY_TRY(AllocNode(new_root, new_root_id));
    root_ = new_root_id;
    ++height_;
    return WriteMeta();
}

// --- reads -----------------------------------------------------------------

Status BTree::FindLeaf(const Entry* target, PageId& leaf) const {
    PageId id = root_;
    const auto less = [this](const Entry& a, const Entry& b) { return Less(a, b); };

    for (uint32_t level = 0; level <= height_; ++level) {
        Node node;
        QUARRY_TRY(ReadNode(id, node));
        if (node.leaf) {
            leaf = id;
            return Status::Ok();
        }
        const size_t child_idx =
            target == nullptr
                ? 0
                : static_cast<size_t>(
                      std::upper_bound(node.entries.begin(), node.entries.end(), *target, less) -
                      node.entries.begin());
        id = node.children[child_idx];
    }
    // Descending further than the recorded height means a cycle in the child
    // pointers; reporting it beats looping forever.
    return Status::Corruption("B+ tree descent exceeded the tree height");
}

Status BTree::Scan(const Entry* lo, const Entry* hi, std::vector<uint64_t>& out) const {
    PageId leaf = kInvalidPage;
    QUARRY_TRY(FindLeaf(lo, leaf));

    while (leaf != kInvalidPage) {
        Node node;
        QUARRY_TRY(ReadNode(leaf, node));
        for (const Entry& e : node.entries) {
            // The descent lands on the leaf that may contain lo; entries before it
            // in that same leaf still have to be skipped.
            if (lo != nullptr && Less(e, *lo)) continue;
            if (hi != nullptr && Less(*hi, e)) return Status::Ok();
            out.push_back(e.row_id);
        }
        leaf = node.next;
    }
    return Status::Ok();
}

Status BTree::Lookup(const Value& key, std::vector<uint64_t>& out) const {
    if (root_ == kInvalidPage) return Status::Internal("index used before Open()");

    Entry lo, hi;
    QUARRY_TRY(MakeEntry(key, 0, lo));
    QUARRY_TRY(MakeEntry(key, std::numeric_limits<uint64_t>::max(), hi));
    ++stats_.lookups;
    return Scan(&lo, &hi, out);
}

Status BTree::RangeScan(const std::optional<Value>& lo,
                        const std::optional<Value>& hi,
                        std::vector<uint64_t>& out) const {
    if (root_ == kInvalidPage) return Status::Internal("index used before Open()");

    Entry lo_entry, hi_entry;
    if (lo.has_value()) QUARRY_TRY(MakeEntry(*lo, 0, lo_entry));
    // The upper bound is inclusive on the user key, so it must sit above every
    // possible row id for that key.
    if (hi.has_value()) {
        QUARRY_TRY(MakeEntry(*hi, std::numeric_limits<uint64_t>::max(), hi_entry));
    }
    ++stats_.lookups;
    return Scan(lo.has_value() ? &lo_entry : nullptr, hi.has_value() ? &hi_entry : nullptr, out);
}

Status BTree::Flush() {
    QUARRY_TRY(WriteMeta());
    return pool_->FlushAll();
}

std::string BTree::Describe() const {
    return std::string("btree(key=") + TypeName(key_type_) +
           ", height=" + std::to_string(height_) + ", root=" + std::to_string(root_) +
           ", splits=" + std::to_string(stats_.node_splits) + ")";
}

}  // namespace quarry
