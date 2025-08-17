#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "quarry/types.hpp"

namespace quarry {

// Column is one column's declared shape.
struct Column {
    std::string name;
    TypeId type = TypeId::kInvalid;
    bool nullable = true;

    Column() = default;
    Column(std::string n, TypeId t, bool null_ok = true)
        : name(std::move(n)), type(t), nullable(null_ok) {}
};

// Schema is an ordered column list with name lookup.
//
// Lookup is linear rather than a hash map. Schemas here have single-digit column
// counts, a linear scan over a contiguous vector beats a hash probe at that size,
// and it keeps column order — which the engine depends on, since a Batch's
// vectors are positional — as the single source of truth.
class Schema {
public:
    Schema() = default;
    explicit Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

    size_t size() const { return columns_.size(); }
    bool empty() const { return columns_.empty(); }

    const Column& at(size_t i) const { return columns_[i]; }
    const std::vector<Column>& columns() const { return columns_; }

    void Add(Column c) { columns_.push_back(std::move(c)); }

    // IndexOf returns the position of a column by name, or nullopt. Comparison is
    // case-sensitive: the parser lowercases identifiers before they reach here, so
    // a mismatch at this layer is a bug rather than a user error.
    std::optional<size_t> IndexOf(const std::string& name) const;

    // Project builds a schema from a subset of this one's columns, by index.
    Schema Project(const std::vector<size_t>& indexes) const;

    std::string ToString() const;

    bool operator==(const Schema& other) const;

private:
    std::vector<Column> columns_;
};

}  // namespace quarry
