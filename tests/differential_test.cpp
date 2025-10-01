// Differential testing against SQLite.
//
// Every other test in this project asserts that Quarry does what its author
// believed SQL means. This one asserts that Quarry does what SQL actually means,
// by loading identical rows into Quarry and into SQLite and demanding identical
// answers to the same queries. It is the only test here that can catch a
// misunderstanding rather than a mistake — a NULL comparison that is wrong in a
// self-consistent way, an aggregate that ignores the wrong rows, an ORDER BY that
// is stable when it should not be — because it compares against an oracle instead
// of against expectations written by the same person who wrote the engine.
//
// Three things make the comparison trustworthy:
//
//  1. One source of truth for the data. tests/datagen.hpp renders the same
//     Dataset into both engines' INSERT statements, so "the two databases
//     disagree" can never mean "the two databases were loaded differently".
//
//  2. One renderer for the results. SQLite's output is converted into a
//     quarry::ResultSet carrying Quarry's own result schema, and then both sides
//     are rendered by quarry::ResultSet. Comparing text produced by two different
//     formatters would make every float a judgement call.
//
//  3. An explicit comparison mode per query, not a global one. A query with no
//     ORDER BY has no defined row order, so its rows are sorted before comparing;
//     a query with ORDER BY is compared positionally, because then the order IS
//     the answer. A LIMIT with no ORDER BY has a defined row COUNT and an
//     undefined row SET, and saying so in the case table is more honest than
//     quietly sorting and pretending the comparison was total.

#include <algorithm>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "datagen.hpp"
#include "quarry/database.hpp"
#include "testing.hpp"

#if defined(QUARRY_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace {

using quarry::Database;
using quarry::ResultSet;
using quarry::Status;
using quarry::TypeId;
using quarry::Value;

// ---------------------------------------------------------------------------
// The query list
// ---------------------------------------------------------------------------

// Compare says how a query's two result sets may legitimately be compared.
enum class Compare {
    // No ORDER BY: the row multiset is the answer, the order is not. Rows are
    // rendered and sorted on both sides before comparing.
    kUnordered,
    // ORDER BY present: compared positionally, because a reordering is a bug.
    kOrdered,
    // LIMIT without ORDER BY: which rows come back is genuinely unspecified in
    // both engines, so only the cardinality is asserted. Used sparingly, and
    // every use is named in the report rather than hidden behind a sort.
    kRowCountOnly,
};

struct Case {
    const char* sql;
    Compare compare;
    const char* note;  // what this case is for; printed on failure
};

// The list is data, so adding coverage is one line. Grouped by what is being
// exercised rather than by SQL shape, because the question a reader has is "is
// NULL handling covered", not "how many SELECTs are there".
const Case kCases[] = {
    // --- full scans and projection ---
    {"SELECT * FROM events", Compare::kUnordered, "full scan, all columns"},
    {"SELECT id FROM events", Compare::kUnordered, "single-column projection"},
    {"SELECT category, amount, quantity FROM events", Compare::kUnordered,
     "mixed-type projection including a nullable column"},

    // --- int64 predicates: every operator, selective and not ---
    {"SELECT id FROM events WHERE id = 4242", Compare::kUnordered, "int64 ="},
    {"SELECT id FROM events WHERE id <> 4242", Compare::kUnordered, "int64 <>, unselective"},
    {"SELECT id FROM events WHERE id < 200", Compare::kUnordered, "int64 <, selective"},
    {"SELECT id FROM events WHERE id <= 200", Compare::kUnordered, "int64 <=, boundary"},
    {"SELECT id FROM events WHERE id > 8800", Compare::kUnordered, "int64 >, selective"},
    {"SELECT id FROM events WHERE id >= 8800", Compare::kUnordered, "int64 >=, boundary"},
    {"SELECT id FROM events WHERE id >= 0", Compare::kUnordered,
     "int64 predicate matching every row: pruning must not fire"},
    {"SELECT id FROM events WHERE id > 1000000", Compare::kUnordered,
     "int64 predicate matching nothing: every segment prunable"},
    {"SELECT id, ts FROM events WHERE ts < 1700001000", Compare::kUnordered,
     "order-correlated column with ties"},
    {"SELECT id, status FROM events WHERE status = 3", Compare::kUnordered,
     "long-run column, equality"},
    {"SELECT id, status FROM events WHERE status >= 2", Compare::kUnordered,
     "long-run column, range"},

    // --- double predicates ---
    {"SELECT id, amount FROM events WHERE amount > 9900.0", Compare::kUnordered,
     "double >, selective on an unprunable column"},
    {"SELECT id, amount FROM events WHERE amount >= 9900.0", Compare::kUnordered, "double >="},
    {"SELECT id, amount FROM events WHERE amount < 25.5", Compare::kUnordered, "double <"},
    {"SELECT id, amount FROM events WHERE amount <= 25.5", Compare::kUnordered, "double <="},
    // 3676.593750 is a value the generator actually produces. A double equality
    // test against a literal that matches nothing would pass for the wrong reason:
    // two engines that both return no rows agree about nothing.
    {"SELECT id, amount FROM events WHERE amount = 3676.593750", Compare::kUnordered,
     "double equality on an exactly representable value that exists in the data"},
    {"SELECT id, amount FROM events WHERE amount <> 3676.593750", Compare::kUnordered,
     "double <>: the complement, which must exclude exactly that one row"},
    {"SELECT id, amount FROM events WHERE amount > 5000", Compare::kUnordered,
     "double column against an integer literal: the one implicit coercion"},

    // --- varchar predicates ---
    {"SELECT id, category FROM events WHERE category = 'alpha'", Compare::kUnordered,
     "dictionary-encoded column, equality"},
    {"SELECT id, category FROM events WHERE category <> 'alpha'", Compare::kUnordered,
     "dictionary-encoded column, inequality"},
    {"SELECT id, category FROM events WHERE category < 'charlie'", Compare::kUnordered,
     "varchar ordering comparison"},
    {"SELECT id, category FROM events WHERE category <= 'charlie'", Compare::kUnordered,
     "varchar <="},
    {"SELECT id, category FROM events WHERE category > 'golf'", Compare::kUnordered, "varchar >"},
    {"SELECT id, category FROM events WHERE category >= 'golf'", Compare::kUnordered, "varchar >="},
    {"SELECT id, category FROM events WHERE category = 'nonexistent'", Compare::kUnordered,
     "varchar predicate matching nothing"},
    {"SELECT id, region FROM events WHERE region = 'eu-central'", Compare::kUnordered,
     "run-length-encoded column, equality"},
    {"SELECT id, region FROM events WHERE region > 'eu-central'", Compare::kUnordered,
     "run-length-encoded column, range"},

    // --- boolean predicates ---
    {"SELECT id, active FROM events WHERE active = true", Compare::kUnordered, "boolean true"},
    {"SELECT id, active FROM events WHERE active = false", Compare::kUnordered, "boolean false"},
    {"SELECT id, active FROM events WHERE active <> true", Compare::kUnordered, "boolean <>"},

    // --- predicates on NULL-bearing columns ---
    // The engine has no IS NULL, so these are the closest reachable cases: a
    // comparison against a column that contains NULLs must drop the NULL rows,
    // because a comparison with NULL is unknown rather than false, and the two
    // are indistinguishable until the predicate is negated or OR'd.
    {"SELECT id, quantity FROM events WHERE quantity > 500000", Compare::kUnordered,
     "nullable int64 >: NULL rows must not match"},
    {"SELECT id, quantity FROM events WHERE quantity <= 500000", Compare::kUnordered,
     "nullable int64 <=: complement must also exclude NULL rows"},
    {"SELECT id, quantity FROM events WHERE quantity <> 0", Compare::kUnordered,
     "nullable int64 <>: must not resurrect NULL rows"},
    {"SELECT id, score FROM events WHERE score > 50.0", Compare::kUnordered,
     "nullable double >, higher null fraction"},
    {"SELECT id, score FROM events WHERE score < 50.0", Compare::kUnordered,
     "nullable double <, complement"},
    {"SELECT id, quantity, score FROM events WHERE quantity > 900000 OR score > 99.0",
     Compare::kUnordered, "OR over two NULL-bearing columns: unknown OR true is true"},
    {"SELECT id, quantity, score FROM events WHERE quantity > 900000 AND score > 99.0",
     Compare::kUnordered, "AND over two NULL-bearing columns: unknown AND true is unknown"},

    // --- AND / OR combinations ---
    {"SELECT id FROM events WHERE id > 100 AND id < 200", Compare::kUnordered,
     "AND of two ranges on one column"},
    {"SELECT id FROM events WHERE id < 100 OR id > 8900", Compare::kUnordered,
     "OR of two disjoint ranges: no segment is prunable for both sides"},
    {"SELECT id, category, amount FROM events WHERE category = 'alpha' AND amount > 5000.0",
     Compare::kUnordered, "AND across a varchar and a double"},
    {"SELECT id, category, status FROM events WHERE category = 'delta' OR status = 1",
     Compare::kUnordered, "OR across a varchar and an int64"},
    {"SELECT id FROM events WHERE id > 100 AND id < 5000 AND status = 2 AND active = true",
     Compare::kUnordered, "four-way AND"},
    {"SELECT id FROM events WHERE id < 50 OR status = 4 OR category = 'hotel'",
     Compare::kUnordered, "three-way OR"},
    {"SELECT id FROM events WHERE id > 1000 AND id < 2000 OR id > 8000 AND id < 8100",
     Compare::kUnordered, "AND binding tighter than OR"},
    {"SELECT id FROM events WHERE id > 4000 AND id > 4500 AND id > 5000", Compare::kUnordered,
     "redundant conjuncts on one column"},
    {"SELECT id FROM events WHERE id < 100 AND id > 8000", Compare::kUnordered,
     "contradictory conjuncts: empty result"},

    // --- aggregates without GROUP BY ---
    {"SELECT COUNT(*) FROM events", Compare::kUnordered, "COUNT(*) over the whole table"},
    {"SELECT COUNT(quantity) FROM events", Compare::kUnordered,
     "COUNT of a nullable column skips NULLs"},
    {"SELECT COUNT(score) FROM events", Compare::kUnordered, "COUNT of a nullable double"},
    {"SELECT SUM(amount) FROM events", Compare::kUnordered, "SUM over a double column"},
    {"SELECT SUM(quantity) FROM events", Compare::kUnordered,
     "SUM over a nullable int64 column"},
    {"SELECT SUM(status) FROM events", Compare::kUnordered, "SUM over int64 stays int64"},
    {"SELECT AVG(amount) FROM events", Compare::kUnordered, "AVG is always a double"},
    {"SELECT AVG(quantity) FROM events", Compare::kUnordered, "AVG over a nullable column"},
    {"SELECT MIN(id), MAX(id) FROM events", Compare::kUnordered, "MIN and MAX on int64"},
    {"SELECT MIN(amount), MAX(amount) FROM events", Compare::kUnordered, "MIN and MAX on double"},
    {"SELECT MIN(category), MAX(category) FROM events", Compare::kUnordered,
     "MIN and MAX on varchar"},
    {"SELECT MIN(quantity), MAX(quantity) FROM events", Compare::kUnordered,
     "MIN and MAX ignore NULLs"},
    {"SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM events",
     Compare::kUnordered, "all five aggregates in one query"},
    {"SELECT COUNT(*), SUM(amount), AVG(amount), MIN(id), MAX(id) FROM events WHERE id < 0",
     Compare::kUnordered,
     "aggregates over an empty input: COUNT is 0, the rest are NULL"},
    {"SELECT COUNT(*) FROM events WHERE category = 'alpha' AND active = true",
     Compare::kUnordered, "COUNT with a filter"},

    // --- aggregates with GROUP BY ---
    {"SELECT category, COUNT(*) FROM events GROUP BY category", Compare::kUnordered,
     "GROUP BY a low-cardinality varchar"},
    {"SELECT region, COUNT(*) FROM events GROUP BY region", Compare::kUnordered,
     "GROUP BY a run-length-encoded varchar"},
    {"SELECT status, COUNT(*) FROM events GROUP BY status", Compare::kUnordered,
     "GROUP BY an int64"},
    {"SELECT active, COUNT(*) FROM events GROUP BY active", Compare::kUnordered,
     "GROUP BY a boolean"},
    {"SELECT category, SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM events GROUP BY "
     "category",
     Compare::kUnordered, "every aggregate per group"},
    {"SELECT category, COUNT(quantity), SUM(quantity), AVG(quantity) FROM events GROUP BY "
     "category",
     Compare::kUnordered, "aggregates over a nullable column per group"},
    {"SELECT quantity, COUNT(*) FROM events WHERE id < 60 GROUP BY quantity",
     Compare::kUnordered,
     "GROUP BY a nullable column: all NULL keys must form one group"},
    {"SELECT category, region, COUNT(*) FROM events GROUP BY category, region",
     Compare::kUnordered, "two-column GROUP BY"},
    {"SELECT region, status, category, COUNT(*), SUM(amount) FROM events GROUP BY region, "
     "status, category",
     Compare::kUnordered, "three-column GROUP BY"},
    {"SELECT category, COUNT(*) FROM events WHERE amount > 5000.0 GROUP BY category",
     Compare::kUnordered, "GROUP BY after a filter"},
    {"SELECT category, COUNT(*) FROM events WHERE id < 0 GROUP BY category", Compare::kUnordered,
     "GROUP BY over an empty input produces no rows, not a NULL row"},

    // --- ORDER BY, one case per type, each with a unique tiebreaker ---
    // Every ordered case ends in a key that is unique, because a tie in the sort
    // key makes the row order unspecified again and the comparison would be
    // asserting something SQL does not promise.
    {"SELECT id FROM events ORDER BY id", Compare::kOrdered, "int64 ASC"},
    {"SELECT id FROM events ORDER BY id DESC", Compare::kOrdered, "int64 DESC"},
    {"SELECT id, ts FROM events ORDER BY ts ASC, id ASC", Compare::kOrdered,
     "int64 ASC with ties, broken by a unique key"},
    {"SELECT id, ts FROM events ORDER BY ts DESC, id ASC", Compare::kOrdered,
     "int64 DESC with ties"},
    {"SELECT id, amount FROM events ORDER BY amount ASC, id ASC", Compare::kOrdered,
     "double ASC"},
    {"SELECT id, amount FROM events ORDER BY amount DESC, id ASC", Compare::kOrdered,
     "double DESC"},
    {"SELECT id, category FROM events ORDER BY category ASC, id ASC", Compare::kOrdered,
     "varchar ASC"},
    {"SELECT id, category FROM events ORDER BY category DESC, id ASC", Compare::kOrdered,
     "varchar DESC"},
    {"SELECT id, active FROM events ORDER BY active ASC, id ASC", Compare::kOrdered,
     "boolean ASC"},
    {"SELECT id, active FROM events ORDER BY active DESC, id ASC", Compare::kOrdered,
     "boolean DESC"},
    {"SELECT id, quantity FROM events ORDER BY quantity ASC, id ASC", Compare::kOrdered,
     "nullable int64 ASC: where NULLs sort is a real dialect difference"},
    {"SELECT id, quantity FROM events ORDER BY quantity DESC, id ASC", Compare::kOrdered,
     "nullable int64 DESC"},
    {"SELECT id, score FROM events ORDER BY score ASC, id ASC", Compare::kOrdered,
     "nullable double ASC"},
    {"SELECT id, score FROM events ORDER BY score DESC, id ASC", Compare::kOrdered,
     "nullable double DESC"},
    {"SELECT id, category, amount FROM events WHERE category = 'bravo' ORDER BY amount DESC, id "
     "ASC",
     Compare::kOrdered, "ORDER BY after a filter"},
    {"SELECT category, COUNT(*) FROM events GROUP BY category ORDER BY category",
     Compare::kOrdered, "ORDER BY a group key"},
    {"SELECT category, SUM(amount) FROM events GROUP BY category ORDER BY category DESC",
     Compare::kOrdered, "ORDER BY a group key, descending"},

    // --- LIMIT ---
    {"SELECT id FROM events ORDER BY id LIMIT 10", Compare::kOrdered,
     "LIMIT with ORDER BY: both which rows and in what order are defined"},
    {"SELECT id FROM events ORDER BY id DESC LIMIT 1", Compare::kOrdered, "LIMIT 1"},
    {"SELECT id, amount FROM events ORDER BY amount DESC, id ASC LIMIT 25", Compare::kOrdered,
     "top-N on a double"},
    {"SELECT id FROM events ORDER BY id LIMIT 100000", Compare::kOrdered,
     "LIMIT larger than the table"},
    {"SELECT id FROM events WHERE id < 500 ORDER BY id LIMIT 0", Compare::kOrdered, "LIMIT 0"},
    {"SELECT id FROM events LIMIT 17", Compare::kRowCountOnly,
     "LIMIT without ORDER BY: the row count is defined, the rows are not"},
    {"SELECT id FROM events WHERE category = 'echo' LIMIT 5", Compare::kRowCountOnly,
     "LIMIT after a filter, no ORDER BY"},

    // --- single-column equi inner join ---
    // The dimension table deliberately omits two of the fact table's categories,
    // so an inner join that silently behaved like a left join would change the row
    // count and be caught here.
    {"SELECT id, amount, weight, tier FROM events INNER JOIN dims ON events.category = "
     "dims.category",
     Compare::kUnordered, "equi inner join, unqualified output columns"},
    {"SELECT id, weight FROM events INNER JOIN dims ON events.category = dims.category WHERE id "
     "< 300",
     Compare::kUnordered, "join with a filter on the probe side"},
    {"SELECT id, tier FROM events INNER JOIN dims ON events.category = dims.category WHERE "
     "weight > 20",
     Compare::kUnordered, "join with a filter on the build side"},
    {"SELECT tier, COUNT(*), SUM(amount) FROM events INNER JOIN dims ON events.category = "
     "dims.category GROUP BY tier",
     Compare::kUnordered, "aggregate over a join"},
    {"SELECT id, weight FROM events INNER JOIN dims ON events.category = dims.category ORDER BY "
     "id LIMIT 20",
     Compare::kOrdered, "ordered top-N over a join"},
};

// ---------------------------------------------------------------------------
// Divergence bookkeeping
// ---------------------------------------------------------------------------

// A divergence is a finding, not a nuisance. Everything recorded here is printed
// in a block at the end of the run and reflected in the exit status, so a real
// disagreement between the two engines cannot be mistaken for noise or lost in
// the middle of a long log.
struct Divergence {
    std::string sql;
    std::string note;
    std::string detail;
};

std::vector<Divergence>& Divergences() {
    static std::vector<Divergence> list;
    return list;
}

void RecordDivergence(const Case& c, const std::string& detail) {
    Divergences().push_back({c.sql, c.note, detail});
}

// TypeNotes collects places where SQLite's runtime type for an output column is
// not the class Quarry declared for it. These are not row mismatches — the values
// still compare equal after coercion — but a SUM that SQLite calls an integer and
// Quarry calls a double is worth seeing, so they are reported separately rather
// than dropped.
struct TypeNote {
    std::string sql;
    std::string detail;
};

std::vector<TypeNote>& TypeNotes() {
    static std::vector<TypeNote> list;
    return list;
}

#if defined(QUARRY_HAVE_SQLITE3)

// ---------------------------------------------------------------------------
// SQLite side
// ---------------------------------------------------------------------------

class Sqlite {
public:
    ~Sqlite() {
        if (handle_) sqlite3_close(handle_);
    }

    bool Open(std::string& error) {
        // In memory: the oracle has no durability requirements, and a file would
        // add a cleanup path that can fail for reasons unrelated to the test.
        int rc = sqlite3_open(":memory:", &handle_);
        if (rc != SQLITE_OK) {
            error = handle_ ? sqlite3_errmsg(handle_) : "sqlite3_open failed";
            return false;
        }
        return true;
    }

    bool Exec(const std::string& sql, std::string& error) {
        char* message = nullptr;
        int rc = sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &message);
        if (rc != SQLITE_OK) {
            error = message ? message : "sqlite3_exec failed";
            sqlite3_free(message);
            return false;
        }
        sqlite3_free(message);
        return true;
    }

    // Query runs `sql` and materializes the result into a quarry::ResultSet that
    // carries `schema`. The schema is Quarry's, on purpose: it is what decides how
    // each column is rendered, so both sides go through one formatter and a
    // float's text is never a judgement call.
    bool Query(const std::string& sql, const quarry::Schema& schema, ResultSet& out,
               std::vector<std::string>& type_notes, std::string& error) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            error = sqlite3_errmsg(handle_);
            if (stmt) sqlite3_finalize(stmt);
            return false;
        }

        int columns = sqlite3_column_count(stmt);
        if (columns != static_cast<int>(schema.size())) {
            std::ostringstream os;
            os << "column count differs: quarry " << schema.size() << ", sqlite " << columns;
            error = os.str();
            sqlite3_finalize(stmt);
            return false;
        }

        out.schema = schema;
        out.rows.clear();

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            std::vector<Value> row;
            row.reserve(static_cast<size_t>(columns));
            for (int c = 0; c < columns; ++c) {
                int native = sqlite3_column_type(stmt, c);
                if (native == SQLITE_NULL) {
                    row.push_back(Value::Null());
                    continue;
                }
                TypeId want = schema.at(static_cast<size_t>(c)).type;
                NoteTypeClass(schema, c, native, type_notes);
                switch (want) {
                    case TypeId::kInt64:
                        row.push_back(Value::Int64(sqlite3_column_int64(stmt, c)));
                        break;
                    case TypeId::kDouble:
                        row.push_back(Value::Double(sqlite3_column_double(stmt, c)));
                        break;
                    case TypeId::kBoolean:
                        row.push_back(Value::Boolean(sqlite3_column_int64(stmt, c) != 0));
                        break;
                    case TypeId::kVarchar: {
                        const unsigned char* text = sqlite3_column_text(stmt, c);
                        row.push_back(Value::Varchar(
                            text ? reinterpret_cast<const char*>(text) : ""));
                        break;
                    }
                    default:
                        error = "quarry result column has an invalid type";
                        sqlite3_finalize(stmt);
                        return false;
                }
            }
            out.rows.push_back(std::move(row));
        }

        if (rc != SQLITE_DONE) {
            error = sqlite3_errmsg(handle_);
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        return true;
    }

private:
    static void NoteTypeClass(const quarry::Schema& schema, int c, int native,
                              std::vector<std::string>& notes) {
        TypeId want = schema.at(static_cast<size_t>(c)).type;
        bool compatible = false;
        switch (native) {
            case SQLITE_INTEGER:
                // An integer is an acceptable runtime type for all three of
                // Quarry's non-string types: SQLite stores booleans as 0 and 1,
                // and it narrows a REAL-valued expression to INTEGER when the
                // value happens to be integral.
                compatible = want == TypeId::kInt64 || want == TypeId::kBoolean ||
                             want == TypeId::kDouble;
                break;
            case SQLITE_FLOAT:
                compatible = want == TypeId::kDouble;
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB:
                compatible = want == TypeId::kVarchar;
                break;
            default:
                compatible = true;
                break;
        }
        if (compatible) return;
        const char* native_name = native == SQLITE_INTEGER ? "INTEGER"
                                  : native == SQLITE_FLOAT ? "REAL"
                                  : native == SQLITE_TEXT  ? "TEXT"
                                                           : "BLOB";
        std::ostringstream os;
        os << "column " << c << " (" << schema.at(static_cast<size_t>(c)).name
           << "): quarry declares " << quarry::TypeName(want) << ", sqlite returned "
           << native_name;
        notes.push_back(os.str());
    }

    sqlite3* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

std::string FirstDifference(const std::vector<std::string>& left,
                            const std::vector<std::string>& right) {
    size_t limit = std::min(left.size(), right.size());
    for (size_t i = 0; i < limit; ++i) {
        if (left[i] == right[i]) continue;
        std::ostringstream os;
        os << "first difference at position " << i << "\n            quarry: " << left[i]
           << "\n            sqlite: " << right[i];
        return os.str();
    }
    std::ostringstream os;
    os << "common prefix of " << limit << " matches; quarry has " << left.size()
       << " entries and sqlite has " << right.size();
    return os.str();
}

// Compare asserts the two result sets agree under the mode the case declared, and
// records a Divergence when they do not. Returns true on agreement.
bool CompareResults(const Case& c, const ResultSet& quarry_rows, const ResultSet& sqlite_rows) {
    if (c.compare == Compare::kRowCountOnly) {
        if (quarry_rows.row_count() == sqlite_rows.row_count()) return true;
        std::ostringstream os;
        os << "row count: quarry " << quarry_rows.row_count() << ", sqlite "
           << sqlite_rows.row_count();
        RecordDivergence(c, os.str());
        return false;
    }

    if (quarry_rows.row_count() != sqlite_rows.row_count()) {
        std::ostringstream os;
        os << "row count: quarry " << quarry_rows.row_count() << ", sqlite "
           << sqlite_rows.row_count();
        // Still fall through to a content comparison, because knowing which rows
        // are extra or missing is what makes a count mismatch diagnosable.
        std::vector<std::string> left = quarry_rows.SortedRowStrings();
        std::vector<std::string> right = sqlite_rows.SortedRowStrings();
        os << "\n          " << FirstDifference(left, right);
        RecordDivergence(c, os.str());
        return false;
    }

    if (c.compare == Compare::kUnordered) {
        std::vector<std::string> left = quarry_rows.SortedRowStrings();
        std::vector<std::string> right = sqlite_rows.SortedRowStrings();
        if (left == right) return true;
        RecordDivergence(c, "sorted rows differ\n          " + FirstDifference(left, right));
        return false;
    }

    // Ordered: rendered positionally by the same renderer on both sides, so a
    // reordering shows up as a line difference at the position where it happened.
    std::vector<std::string> left = SplitLines(quarry_rows.ToString());
    std::vector<std::string> right = SplitLines(sqlite_rows.ToString());
    if (left == right) return true;
    RecordDivergence(c, "ordered rows differ\n          " + FirstDifference(left, right));
    return false;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// TempDir removes itself, including after a failed assertion, because a test that
// leaves a few hundred segment files behind on every run is a test people stop
// running.
class TempDir {
public:
    explicit TempDir(const std::string& label) {
        path_ = std::filesystem::temp_directory_path() /
                ("quarry-" + label + "-" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

}  // namespace

QUARRY_TEST(differential_against_sqlite) {
    quarry::datagen::Spec spec;
    // Nine thousand rows over 1024-row segments is nine segments: enough that a
    // selective predicate prunes most of them and an unselective one prunes none,
    // which is what makes the scan paths here distinguishable.
    spec.rows = 9000;
    quarry::datagen::Dataset data = quarry::datagen::Generate(spec);

    TempDir dir("differential");
    Database::Options options;
    options.rows_per_segment = 1024;
    Database db(dir.path(), options);

    Status opened = db.Open();
    CHECK_OK(opened);
    if (!opened.ok()) return;

    Sqlite sqlite;
    std::string error;
    if (!sqlite.Open(error)) {
        ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite open failed: " + error);
        return;
    }

    // --- load both engines from the one Dataset ---
    bool loaded = true;
    for (const std::string& ddl : quarry::datagen::QuarryDdl()) {
        auto created = db.Execute(ddl);
        if (!created.ok()) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__,
                                             "quarry DDL failed: " + created.status().ToString() +
                                                 "\n        " + ddl);
            loaded = false;
        }
    }
    for (const std::string& ddl : quarry::datagen::SqliteDdl()) {
        if (!sqlite.Exec(ddl, error)) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite DDL failed: " + error);
            loaded = false;
        }
    }
    if (!loaded) return;

    if (!sqlite.Exec("BEGIN", error)) {
        ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite BEGIN failed: " + error);
        return;
    }
    std::vector<std::string> quarry_inserts = quarry::datagen::QuarryInserts(data);
    std::vector<std::string> sqlite_inserts = quarry::datagen::SqliteInserts(data);
    for (size_t i = 0; i < quarry_inserts.size(); ++i) {
        auto inserted = db.Execute(quarry_inserts[i]);
        if (!inserted.ok()) {
            ::quarry::testing::ReportFailure(
                __FILE__, __LINE__,
                "quarry INSERT failed: " + inserted.status().ToString() +
                    "\n        statement " + std::to_string(i) + " of " +
                    std::to_string(quarry_inserts.size()));
            loaded = false;
            break;
        }
    }
    for (const std::string& insert : sqlite_inserts) {
        if (!sqlite.Exec(insert, error)) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite INSERT failed: " + error);
            loaded = false;
            break;
        }
    }
    if (!sqlite.Exec("COMMIT", error)) {
        ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite COMMIT failed: " + error);
        return;
    }
    if (!loaded) return;

    // A sanity check before the real comparison: if the two engines do not even
    // hold the same number of rows, every subsequent mismatch is a consequence of
    // that one and reporting them all would bury the cause.
    {
        auto quarry_count = db.Execute("SELECT COUNT(*) FROM events");
        if (!quarry_count.ok()) {
            ::quarry::testing::ReportFailure(
                __FILE__, __LINE__, "quarry COUNT(*) failed: " + quarry_count.status().ToString());
            return;
        }
        ResultSet sqlite_count;
        std::vector<std::string> notes;
        if (!sqlite.Query("SELECT COUNT(*) FROM events", quarry_count.value().schema, sqlite_count,
                          notes, error)) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__, "sqlite COUNT(*) failed: " + error);
            return;
        }
        Case load_case{"SELECT COUNT(*) FROM events", Compare::kUnordered,
                       "both engines loaded the same number of rows"};
        if (!CompareResults(load_case, quarry_count.value(), sqlite_count)) {
            ::quarry::testing::ReportFailure(__FILE__, __LINE__,
                                             "the two engines were not loaded identically; "
                                             "the query comparison below would be meaningless");
            return;
        }
    }

    size_t agreed = 0;
    size_t failed_to_run = 0;
    for (const Case& c : kCases) {
        auto quarry_result = db.Execute(c.sql);
        if (!quarry_result.ok()) {
            ++failed_to_run;
            ::quarry::testing::ReportFailure(
                __FILE__, __LINE__,
                std::string("quarry could not run the query\n        sql:  ") + c.sql +
                    "\n        for:  " + c.note + "\n        err:  " +
                    quarry_result.status().ToString());
            continue;
        }

        ResultSet sqlite_result;
        std::vector<std::string> notes;
        if (!sqlite.Query(c.sql, quarry_result.value().schema, sqlite_result, notes, error)) {
            // SQLite failing to run a query Quarry accepted is itself a finding:
            // either the query is not the standard SQL it looks like, or Quarry is
            // accepting something it should reject.
            RecordDivergence(c, "sqlite could not run this query: " + error);
            ::quarry::testing::ReportFailure(
                __FILE__, __LINE__,
                std::string("sqlite rejected a query quarry accepted\n        sql:  ") + c.sql +
                    "\n        err:  " + error);
            continue;
        }
        for (const std::string& note : notes) {
            // One note per column per query is enough; the loop above can emit the
            // same note once per row.
            bool seen = false;
            for (const TypeNote& existing : TypeNotes()) {
                if (existing.sql == c.sql && existing.detail == note) seen = true;
            }
            if (!seen) TypeNotes().push_back({c.sql, note});
        }

        if (CompareResults(c, quarry_result.value(), sqlite_result)) {
            ++agreed;
        } else {
            const Divergence& d = Divergences().back();
            ::quarry::testing::ReportFailure(
                __FILE__, __LINE__,
                std::string("quarry and sqlite disagree\n        sql:  ") + c.sql +
                    "\n        for:  " + c.note + "\n        " + d.detail);
        }
    }

    std::cout << "\n  " << agreed << " of " << std::size(kCases)
              << " queries agreed with sqlite";
    if (failed_to_run != 0) {
        std::cout << "; " << failed_to_run << " could not be run by quarry at all";
    }
    std::cout << "\n";

    if (!TypeNotes().empty()) {
        std::cout << "\n  TYPE NOTES (values agreed; the declared result type did not)\n";
        for (const TypeNote& note : TypeNotes()) {
            std::cout << "    " << note.detail << "\n      in: " << note.sql << "\n";
        }
    }

    if (!Divergences().empty()) {
        std::cout << "\n  DIVERGENCES FROM SQLITE (" << Divergences().size() << ")\n";
        for (const Divergence& d : Divergences()) {
            std::cout << "    sql:    " << d.sql << "\n    for:    " << d.note
                      << "\n    detail: " << d.detail << "\n\n";
        }
    } else {
        std::cout << "  no divergences from sqlite\n";
    }

    CHECK_OK(db.Close());
}

#else  // !QUARRY_HAVE_SQLITE3

QUARRY_TEST(differential_against_sqlite) {
    // Failing rather than passing-with-a-warning. The differential suite is the
    // most valuable test here, and a build that silently skipped it would report
    // green while testing nothing.
    ::quarry::testing::ReportFailure(__FILE__, __LINE__,
                                     "built without sqlite3; the differential suite cannot run. "
                                     "Install libsqlite3 and re-run cmake.");
}

#endif

QUARRY_TEST_MAIN()
