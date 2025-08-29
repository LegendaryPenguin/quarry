#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/catalog.hpp"
#include "quarry/plan.hpp"
#include "quarry/status.hpp"

namespace quarry {

// ResultSet is a fully materialized query result.
//
// Materializing is the right choice at the API boundary and the wrong one inside
// the engine. A caller wants a table it can print or compare; an operator wants a
// stream it can stop consuming. So the streaming interface stays internal and this
// exists only at the edge, where result sizes are the caller's problem.
struct ResultSet {
    Schema schema;
    std::vector<std::vector<Value>> rows;

    size_t row_count() const { return rows.size(); }

    // ToString renders the result as a stable, sorted-by-nothing text table. Used
    // by the shell and by the differential tests, which compare Quarry's output
    // against SQLite's as text — so the formatting here is load-bearing and must
    // not casually change.
    std::string ToString() const;

    // SortedRowStrings renders each row to a string and sorts them, so two
    // engines can be compared on queries without a deterministic ORDER BY. A
    // result set with no ORDER BY has no guaranteed row order, and comparing it
    // positionally would produce failures that are nothing but a different but
    // equally correct order.
    std::vector<std::string> SortedRowStrings() const;
};

// Database is the public entry point: open a directory, run SQL against it.
class Database {
public:
    struct Options {
        // buffer_frames bounds index memory. Deliberately small by default so
        // that eviction happens during ordinary use rather than only in a
        // benchmark contrived to provoke it.
        size_t buffer_frames = 256;
        BufferPool::Policy buffer_policy = BufferPool::Policy::kClock;

        // rows_per_segment decides segment granularity, which trades pruning
        // precision against per-segment overhead. The default is small enough
        // that a modest test table spans several segments, because a single
        // segment would make zone-map pruning untestable.
        size_t rows_per_segment = 16384;

        Optimizer::Rules rules;
    };

    explicit Database(std::string directory);
    Database(std::string directory, Options options);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status Open();
    Status Close();

    // Execute runs one statement. DDL and INSERT return an empty ResultSet.
    Result<ResultSet> Execute(const std::string& sql);

    // ExplainQuery returns the plan text without running it; ExplainAnalyze runs
    // it and annotates each operator with measured counters next to its estimates.
    Result<std::string> ExplainQuery(const std::string& sql);
    Result<std::string> ExplainAnalyze(const std::string& sql);

    Catalog& catalog() { return *catalog_; }
    BufferPool& buffer_pool() { return *pool_; }

private:
    std::string directory_;
    Options options_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<BufferPool> pool_;
    bool open_ = false;
};

}  // namespace quarry
