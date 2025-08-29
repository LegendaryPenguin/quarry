#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"

namespace quarry {

// IndexKind selects which structure backs an index, so the two can be compared
// on identical data and identical queries.
enum class IndexKind : uint8_t {
    kBTree,
    kLsm,
};

const char* IndexKindName(IndexKind k);

struct IndexDef {
    std::string name;
    size_t column_index = 0;
    IndexKind kind = IndexKind::kBTree;
    std::string path;
};

// TableDef is everything persisted about one table.
struct TableDef {
    std::string name;
    Schema schema;
    std::vector<SegmentMeta> segments;
    std::vector<IndexDef> indexes;

    uint64_t row_count() const;

    // Stats across all segments, used by the optimizer for selectivity estimates
    // and by the scan for whole-table pruning.
    ColumnStats ColumnStatistics(size_t column_index) const;
};

// Catalog is the persistent record of what exists.
//
// It is written as a single file rewritten atomically rather than as incremental
// edits. Catalogs here are kilobytes, a rewrite is one write and one rename, and
// the alternative — a log of catalog mutations — is a second recovery problem to
// get right for no benefit at this scale. The tradeoff is stated because it is
// exactly the decision that would be wrong at a larger one.
class Catalog {
public:
    explicit Catalog(std::string dir);

    // Open loads an existing catalog or starts an empty one. A catalog that
    // exists but fails to parse is an error rather than a fresh start: silently
    // discarding it would orphan every segment file on disk.
    Status Open();

    Status CreateTable(const std::string& name, Schema schema);
    Status DropTable(const std::string& name);

    bool HasTable(const std::string& name) const;
    Result<const TableDef*> GetTable(const std::string& name) const;
    Result<TableDef*> GetTableMutable(const std::string& name);

    std::vector<std::string> TableNames() const;

    Status AddSegment(const std::string& table, SegmentMeta meta);
    Status AddIndex(const std::string& table, IndexDef def);

    // Save persists the catalog. Callers that mutate through GetTableMutable must
    // call this; nothing detects a forgotten save, which is the cost of the
    // rewrite-the-world approach.
    Status Save() const;

    const std::string& dir() const { return dir_; }

    // SegmentPath and IndexPath centralize the on-disk layout so nothing else
    // constructs these names by hand.
    std::string SegmentPath(const std::string& table, uint64_t segment_id) const;
    std::string IndexPath(const std::string& table, const std::string& index_name) const;

private:
    std::string CatalogPath() const;

    std::string dir_;
    std::vector<TableDef> tables_;
    uint64_t next_segment_id_ = 1;
};

}  // namespace quarry
