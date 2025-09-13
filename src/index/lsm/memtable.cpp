#include "memtable.hpp"

#include <algorithm>

namespace quarry::lsm {

void Memtable::Insert(const std::string& key, uint64_t row_id) {
    auto it = table_.find(key);
    if (it == table_.end()) {
        it = table_.emplace(key, std::vector<uint64_t>{}).first;
        approx_bytes_ += kNodeOverhead + key.size();
    }

    auto& ids = it->second;
    auto pos = std::lower_bound(ids.begin(), ids.end(), row_id);
    if (pos != ids.end() && *pos == row_id) return;

    ids.insert(pos, row_id);
    approx_bytes_ += sizeof(uint64_t);
    ++row_id_count_;
}

bool Memtable::Lookup(const std::string& key, std::vector<uint64_t>& out) const {
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    out.insert(out.end(), it->second.begin(), it->second.end());
    return true;
}

void Memtable::Clear() {
    table_.clear();
    approx_bytes_ = 0;
    row_id_count_ = 0;
}

}  // namespace quarry::lsm
