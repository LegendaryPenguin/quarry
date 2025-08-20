#include "quarry/batch.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

namespace quarry {

// ---------------------------------------------------------------------------
// NullMask
// ---------------------------------------------------------------------------

void NullMask::Resize(size_t n) {
    bits_.resize((n + 63) / 64, 0);
}

void NullMask::Reset(size_t n) {
    bits_.assign((n + 63) / 64, 0);
    has_nulls_ = false;
}

size_t NullMask::CountNulls(size_t n) const {
    if (!has_nulls_) return 0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (IsNull(i)) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Vector
// ---------------------------------------------------------------------------

void Vector::Reserve(size_t n) {
    if (FixedWidth(type_)) {
        data_.reserve(n * TypeSize(type_));
    } else {
        // A guess at average string length. Wrong in both directions is fine —
        // this only avoids the first few reallocations.
        data_.reserve(n * 16);
        offsets_.reserve(n + 1);
    }
    nulls_.Resize(n);
}

void Vector::Clear() {
    data_.clear();
    offsets_.clear();
    size_ = 0;
    nulls_.Reset(0);
}

std::string_view Vector::GetString(size_t i) const {
    // offsets_ holds size()+1 entries, so a length is the difference between
    // consecutive offsets and no separate length array is needed.
    if (i + 1 >= offsets_.size()) return {};
    const uint32_t begin = offsets_[i];
    const uint32_t end = offsets_[i + 1];
    return std::string_view(reinterpret_cast<const char*>(data_.data()) + begin, end - begin);
}

void Vector::AppendString(std::string_view s) {
    if (offsets_.empty()) offsets_.push_back(0);
    data_.insert(data_.end(), s.begin(), s.end());
    offsets_.push_back(static_cast<uint32_t>(data_.size()));
    nulls_.Resize(++size_);
}

void Vector::AppendInt64(int64_t v) {
    const size_t at = data_.size();
    data_.resize(at + sizeof(int64_t));
    std::memcpy(data_.data() + at, &v, sizeof(v));
    nulls_.Resize(++size_);
}

void Vector::AppendDouble(double v) {
    const size_t at = data_.size();
    data_.resize(at + sizeof(double));
    std::memcpy(data_.data() + at, &v, sizeof(v));
    nulls_.Resize(++size_);
}

void Vector::AppendBoolean(bool v) {
    data_.push_back(v ? 1 : 0);
    nulls_.Resize(++size_);
}

void Vector::AppendNull() {
    // A null still occupies its slot. Skipping the slot would desynchronize this
    // column's indexes from every other column in the batch, which is a far worse
    // failure than the wasted bytes.
    switch (type_) {
        case TypeId::kInt64:   AppendInt64(0); break;
        case TypeId::kDouble:  AppendDouble(0.0); break;
        case TypeId::kBoolean: AppendBoolean(false); break;
        case TypeId::kVarchar: AppendString({}); break;
        default: return;
    }
    nulls_.SetNull(size_ - 1);
}

Value Vector::GetValue(size_t i) const {
    if (nulls_.IsNull(i)) return Value::Null();
    switch (type_) {
        case TypeId::kBoolean: return Value::Boolean(bool_data()[i] != 0);
        case TypeId::kInt64:   return Value::Int64(int64_data()[i]);
        case TypeId::kDouble:  return Value::Double(double_data()[i]);
        case TypeId::kVarchar: return Value::Varchar(std::string(GetString(i)));
        default:               return Value::Null();
    }
}

// ---------------------------------------------------------------------------
// Batch
// ---------------------------------------------------------------------------

Batch::Batch(const Schema& schema) {
    for (const auto& col : schema.columns()) {
        AddColumn(col);
    }
}

void Batch::AddColumn(const Column& col) {
    schema_.Add(col);
    columns_.push_back(std::make_unique<Vector>(col.type));
    columns_.back()->Reserve(kBatchSize);
}

void Batch::SetRowCount(size_t n) {
    row_count_ = n;
    for (auto& c : columns_) c->SetSize(n);
    // A changed row count invalidates any cached identity range.
    identity_.clear();
}

const std::vector<uint32_t>& Batch::Rows() const {
    if (has_selection_) return selection_;

    // Materialize 0..row_count-1 only when something actually asks. Operators on
    // an unfiltered batch mostly iterate by index and never call this.
    if (identity_.size() != row_count_) {
        identity_.resize(row_count_);
        for (size_t i = 0; i < row_count_; ++i) identity_[i] = static_cast<uint32_t>(i);
    }
    return identity_;
}

void Batch::SetSelection(std::vector<uint32_t> rows) {
    assert(std::is_sorted(rows.begin(), rows.end()) &&
           "selection must be ascending: aggregates and sort rely on physical order");
    selection_ = std::move(rows);
    has_selection_ = true;
}

void Batch::ClearSelection() {
    selection_.clear();
    has_selection_ = false;
}

void Batch::Reset() {
    for (auto& c : columns_) c->Clear();
    row_count_ = 0;
    ClearSelection();
    identity_.clear();
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

std::optional<size_t> Schema::IndexOf(const std::string& name) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name == name) return i;
    }
    return std::nullopt;
}

Schema Schema::Project(const std::vector<size_t>& indexes) const {
    std::vector<Column> out;
    out.reserve(indexes.size());
    for (size_t i : indexes) {
        if (i < columns_.size()) out.push_back(columns_[i]);
    }
    return Schema(std::move(out));
}

std::string Schema::ToString() const {
    std::ostringstream os;
    os << "(";
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (i) os << ", ";
        os << columns_[i].name << " " << TypeName(columns_[i].type);
        if (!columns_[i].nullable) os << " NOT NULL";
    }
    os << ")";
    return os.str();
}

bool Schema::operator==(const Schema& other) const {
    if (columns_.size() != other.columns_.size()) return false;
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name != other.columns_[i].name) return false;
        if (columns_[i].type != other.columns_[i].type) return false;
    }
    return true;
}

}  // namespace quarry
