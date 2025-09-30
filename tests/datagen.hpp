#pragma once

// A deterministic data generator shared by the differential tests and the
// benchmarks.
//
// One generator, one source of truth, two engines. The differential tests load
// Quarry and SQLite from the same Dataset by rendering it twice, so a mismatch in
// a query result can never be blamed on the two databases having been given
// different rows.
//
// The column set is chosen to exercise specific machinery rather than to look
// like a plausible business table:
//
//   id        int64, strictly increasing      correlated with insertion order, so
//                                             zone-map pruning on it is nearly
//                                             perfect
//   ts        int64, non-decreasing           the same property with ties and
//                                             jitter, which is what real
//                                             time-series data looks like
//   category  varchar, 8 distinct values      low cardinality, so dictionary
//                                             encoding should win
//   region    varchar, long runs              changes once every few hundred
//                                             rows, so run-length encoding
//                                             should win
//   status    int64, long runs                the same, for a fixed-width column
//   amount    double, uniform random          deliberately unprunable: every
//                                             segment's [min,max] covers nearly
//                                             the whole domain, which is the
//                                             honest contrast to id
//   quantity  int64, uniform, nullable        a controlled null fraction on a
//                                             fixed-width column
//   score     double, uniform, nullable       a higher null fraction, so a
//                                             segment can be entirely null
//   active    boolean                         the fourth type
//
// Floating point values are restricted to multiples of 1/64 on purpose. Both
// engines must agree on SUM and AVG, and a sum of arbitrary doubles depends on
// summation order, which two engines have no reason to share. Multiples of 1/64
// in this magnitude range add exactly in IEEE double, so the agreement is a
// property of the data rather than a tolerance in the comparison.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quarry/schema.hpp"
#include "quarry/types.hpp"

namespace quarry::datagen {

// Rng is splitmix64: a named, fixed algorithm rather than <random>, because the
// standard library's distributions are not specified to produce the same
// sequence across implementations and a generator whose output depends on the
// toolchain is not a fixture, it is a surprise.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    uint64_t Next() {
        state_ += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Below returns a value in [0, bound). Modulo bias is irrelevant here: the
    // bounds are tiny next to 2^64 and the generator exists to make varied data,
    // not to be a statistically sound sampler.
    uint64_t Below(uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }

    // Chance returns true with probability `fraction`, resolved against a
    // 10000-step grid so a 0.15 null fraction means 15.00% and not whatever a
    // float comparison happened to produce.
    bool Chance(double fraction) {
        if (fraction <= 0.0) return false;
        if (fraction >= 1.0) return true;
        auto threshold = static_cast<uint64_t>(fraction * 10000.0 + 0.5);
        return Below(10000) < threshold;
    }

private:
    uint64_t state_;
};

struct Spec {
    uint64_t seed = 0x5eed1234ULL;
    size_t rows = 12000;

    // region changes once every region_run_length rows, which is what makes RLE
    // the right encoding for it.
    size_t region_run_length = 433;
    size_t status_run_length = 97;

    // Only this many of the available category names are used, so cardinality is
    // tunable independently of row count.
    size_t category_cardinality = 8;

    double quantity_null_fraction = 0.15;
    double score_null_fraction = 0.30;

    // Categories present in the fact table but absent from the dimension table,
    // so an INNER JOIN actually drops rows instead of being a disguised scan.
    size_t dim_missing_categories = 2;
};

struct Row {
    int64_t id = 0;
    int64_t ts = 0;
    std::string category;
    std::string region;
    int64_t status = 0;
    double amount = 0.0;

    int64_t quantity = 0;
    bool quantity_null = false;

    double score = 0.0;
    bool score_null = false;

    bool active = false;
};

// DimRow is the join's build side: one row per category that survives into the
// dimension table.
struct DimRow {
    std::string category;
    int64_t weight = 0;
    std::string tier;
};

struct Dataset {
    Spec spec;
    std::vector<Row> rows;
    std::vector<DimRow> dims;
};

inline const char* const* CategoryNames() {
    static const char* names[] = {"alpha", "bravo",   "charlie", "delta",
                                  "echo",  "foxtrot", "golf",    "hotel"};
    return names;
}
inline constexpr size_t kMaxCategories = 8;

inline const char* const* RegionNames() {
    static const char* names[] = {"us-east", "us-west", "eu-central", "ap-south"};
    return names;
}
inline constexpr size_t kRegionCount = 4;

// kFactTable and kDimTable are the two table names both engines use. Neither is
// a reserved word in Quarry's lexer or in SQLite.
inline constexpr const char* kFactTable = "events";
inline constexpr const char* kDimTable = "dims";

inline Schema FactSchema() {
    Schema s;
    s.Add(Column("id", TypeId::kInt64, false));
    s.Add(Column("ts", TypeId::kInt64, false));
    s.Add(Column("category", TypeId::kVarchar, false));
    s.Add(Column("region", TypeId::kVarchar, false));
    s.Add(Column("status", TypeId::kInt64, false));
    s.Add(Column("amount", TypeId::kDouble, false));
    s.Add(Column("quantity", TypeId::kInt64, true));
    s.Add(Column("score", TypeId::kDouble, true));
    s.Add(Column("active", TypeId::kBoolean, false));
    return s;
}

inline Schema DimSchema() {
    Schema s;
    s.Add(Column("category", TypeId::kVarchar, false));
    s.Add(Column("weight", TypeId::kInt64, false));
    s.Add(Column("tier", TypeId::kVarchar, false));
    return s;
}

inline Dataset Generate(const Spec& spec = {}) {
    Dataset data;
    data.spec = spec;

    size_t categories = spec.category_cardinality;
    if (categories == 0) categories = 1;
    if (categories > kMaxCategories) categories = kMaxCategories;

    Rng rng(spec.seed);
    data.rows.reserve(spec.rows);

    int64_t ts = 1'700'000'000;
    for (size_t i = 0; i < spec.rows; ++i) {
        Row r;
        r.id = static_cast<int64_t>(i);

        // Non-decreasing with jitter: successive segments' [min,max] barely
        // overlap, which is the shape pruning is good at without being the
        // unrealistic case of a perfectly sorted column.
        ts += static_cast<int64_t>(rng.Below(9));
        r.ts = ts;

        r.category = CategoryNames()[rng.Below(categories)];

        size_t region_run = spec.region_run_length == 0 ? 1 : spec.region_run_length;
        r.region = RegionNames()[(i / region_run) % kRegionCount];

        size_t status_run = spec.status_run_length == 0 ? 1 : spec.status_run_length;
        r.status = static_cast<int64_t>((i / status_run) % 5);

        // Multiples of 1/64 in [0, 10000).
        r.amount = static_cast<double>(rng.Below(640000)) / 64.0;

        r.quantity_null = rng.Chance(spec.quantity_null_fraction);
        r.quantity = r.quantity_null ? 0 : static_cast<int64_t>(rng.Below(1000000));

        r.score_null = rng.Chance(spec.score_null_fraction);
        r.score = r.score_null ? 0.0 : static_cast<double>(rng.Below(6400)) / 64.0;

        r.active = rng.Below(4) != 0;

        data.rows.push_back(std::move(r));
    }

    size_t missing = spec.dim_missing_categories;
    if (missing >= categories) missing = categories - 1;
    for (size_t c = 0; c + missing < categories; ++c) {
        DimRow d;
        d.category = CategoryNames()[c];
        d.weight = static_cast<int64_t>(10 + c * 7);
        d.tier = (c % 3 == 0) ? "gold" : ((c % 3 == 1) ? "silver" : "bronze");
        data.dims.push_back(std::move(d));
    }

    return data;
}

// --- SQL rendering -------------------------------------------------------
//
// Both engines are loaded from the same Dataset by these two pairs of functions.
// The only differences are the type names in the DDL and nothing at all in the
// values, which is deliberate: a value that renders differently for the two
// engines is a value that could silently diverge.

namespace detail {

inline std::string QuoteString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// FormatDouble prints the fixed six-decimal form that Value::ToString uses, so
// the text inserted into each engine and the text compared afterwards agree.
inline std::string FormatDouble(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

inline void AppendFactValues(const Row& r, std::string& out) {
    out += "(";
    out += std::to_string(r.id);
    out += ", ";
    out += std::to_string(r.ts);
    out += ", ";
    out += QuoteString(r.category);
    out += ", ";
    out += QuoteString(r.region);
    out += ", ";
    out += std::to_string(r.status);
    out += ", ";
    out += FormatDouble(r.amount);
    out += ", ";
    out += r.quantity_null ? "NULL" : std::to_string(r.quantity);
    out += ", ";
    out += r.score_null ? "NULL" : FormatDouble(r.score);
    out += ", ";
    out += r.active ? "true" : "false";
    out += ")";
}

inline void AppendDimValues(const DimRow& d, std::string& out) {
    out += "(";
    out += QuoteString(d.category);
    out += ", ";
    out += std::to_string(d.weight);
    out += ", ";
    out += QuoteString(d.tier);
    out += ")";
}

inline std::vector<std::string> Inserts(const Dataset& data, size_t rows_per_statement) {
    if (rows_per_statement == 0) rows_per_statement = 1;
    std::vector<std::string> out;

    for (size_t i = 0; i < data.rows.size(); i += rows_per_statement) {
        size_t end = i + rows_per_statement;
        if (end > data.rows.size()) end = data.rows.size();

        std::string stmt = "INSERT INTO ";
        stmt += kFactTable;
        stmt += " (id, ts, category, region, status, amount, quantity, score, active) VALUES ";
        for (size_t j = i; j < end; ++j) {
            if (j != i) stmt += ", ";
            AppendFactValues(data.rows[j], stmt);
        }
        out.push_back(std::move(stmt));
    }

    if (!data.dims.empty()) {
        std::string stmt = "INSERT INTO ";
        stmt += kDimTable;
        stmt += " (category, weight, tier) VALUES ";
        for (size_t j = 0; j < data.dims.size(); ++j) {
            if (j != 0) stmt += ", ";
            AppendDimValues(data.dims[j], stmt);
        }
        out.push_back(std::move(stmt));
    }

    return out;
}

}  // namespace detail

inline std::vector<std::string> QuarryDdl() {
    return {
        "CREATE TABLE events ("
        "id BIGINT NOT NULL, "
        "ts BIGINT NOT NULL, "
        "category VARCHAR NOT NULL, "
        "region VARCHAR NOT NULL, "
        "status BIGINT NOT NULL, "
        "amount DOUBLE NOT NULL, "
        "quantity BIGINT, "
        "score DOUBLE, "
        "active BOOLEAN NOT NULL)",

        "CREATE TABLE dims ("
        "category VARCHAR NOT NULL, "
        "weight BIGINT NOT NULL, "
        "tier VARCHAR NOT NULL)",
    };
}

inline std::vector<std::string> SqliteDdl() {
    return {
        "CREATE TABLE events ("
        "id INTEGER NOT NULL, "
        "ts INTEGER NOT NULL, "
        "category TEXT NOT NULL, "
        "region TEXT NOT NULL, "
        "status INTEGER NOT NULL, "
        "amount REAL NOT NULL, "
        "quantity INTEGER, "
        "score REAL, "
        "active INTEGER NOT NULL)",

        "CREATE TABLE dims ("
        "category TEXT NOT NULL, "
        "weight INTEGER NOT NULL, "
        "tier TEXT NOT NULL)",
    };
}

// QuarryInserts and SqliteInserts render identical text today. They are separate
// entry points anyway, because the moment one dialect needs an escape the other
// does not, the call sites should not have to change.
inline std::vector<std::string> QuarryInserts(const Dataset& data,
                                              size_t rows_per_statement = 500) {
    return detail::Inserts(data, rows_per_statement);
}

inline std::vector<std::string> SqliteInserts(const Dataset& data,
                                              size_t rows_per_statement = 500) {
    return detail::Inserts(data, rows_per_statement);
}

}  // namespace quarry::datagen
