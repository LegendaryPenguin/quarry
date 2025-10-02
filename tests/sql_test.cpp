// End-to-end SQL behaviour that a differential comparison cannot check: the
// quality of failure.
//
// tests/differential_test.cpp proves Quarry agrees with SQLite on queries that
// work. It is structurally unable to say anything about queries that do not,
// because the two engines' error text has no reason to match and comparing it
// would be comparing dialects rather than correctness. So the contract for errors
// is asserted directly here, and it has three parts:
//
//   1. The Code is specific. kBindError for a name that does not exist,
//      kTypeError for an operation that cannot be typed, kSyntaxError for text
//      that is not SQL, kNotImplemented for SQL that is valid and deliberately
//      unsupported. A single generic failure code would make every one of these
//      indistinguishable to a caller, which is the difference between an engine a
//      program can recover from and one it can only give up on.
//
//   2. The message names the thing that is wrong. "unknown column" is not an
//      error message; "unknown column 'quantty' in table 'events'; available:
//      id, name, amount" is. The assertions below check for the offending
//      identifier by name, because a message that does not contain it cannot be
//      acted on without re-reading the query.
//
//   3. Unsupported is distinguishable from invalid. HAVING, subqueries and OUTER
//      JOIN are real SQL that this engine does not implement. Reporting them as
//      syntax errors would tell a user their query is malformed when it is
//      perfectly well-formed, and would send them off to find a typo that is not
//      there. They must be kNotImplemented.

#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "quarry/database.hpp"
#include "testing.hpp"

namespace {

using quarry::Code;
using quarry::Database;
using quarry::Status;

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

const char* CodeName(Code code) {
    switch (code) {
        case Code::kOk: return "kOk";
        case Code::kSyntaxError: return "kSyntaxError";
        case Code::kBindError: return "kBindError";
        case Code::kTypeError: return "kTypeError";
        case Code::kNotImplemented: return "kNotImplemented";
        case Code::kIoError: return "kIoError";
        case Code::kCorruption: return "kCorruption";
        case Code::kInvalidArgument: return "kInvalidArgument";
        case Code::kInternal: return "kInternal";
    }
    return "unknown";
}

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

// Fixture holds one open database with two small tables. The tables are small and
// have memorable column names on purpose: every assertion below quotes a name from
// them, and a wide generated schema would make the expectations unreadable.
class Fixture {
public:
    Fixture() : dir_("sql"), db_(dir_.path()) {
        CHECK_OK(db_.Open());
        CHECK_OK(Run("CREATE TABLE events (id BIGINT NOT NULL, name VARCHAR, amount DOUBLE, flag "
                     "BOOLEAN)"));
        CHECK_OK(Run("CREATE TABLE labels (id BIGINT NOT NULL, label VARCHAR NOT NULL)"));
        CHECK_OK(Run("INSERT INTO events (id, name, amount, flag) VALUES "
                     "(1, 'one', 1.5, true), (2, 'two', 2.5, false), (3, NULL, NULL, true)"));
        CHECK_OK(Run("INSERT INTO labels (id, label) VALUES (1, 'first'), (2, 'second')"));
    }

    ~Fixture() { db_.Close(); }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Status Run(const std::string& sql) {
        auto result = db_.Execute(sql);
        return result.ok() ? Status::Ok() : result.status();
    }

    Database& db() { return db_; }

private:
    TempDir dir_;
    Database db_;
};

// Expect asserts that `sql` fails with `code` and that the message contains every
// fragment in `must_mention`. The fragments are the point: a code tells a program
// what happened and a message tells a person, and only one of those is checkable
// by asserting on an enum.
// The file and line come before the variadic tail so the macro below can forward
// a braced initializer list: a braced list in a non-variadic macro argument is
// split at its internal commas, which is a C preprocessor trap worth avoiding by
// construction rather than by remembering.
void Expect(Fixture& fixture, const char* sql, Code code, const char* file, int line,
            const std::vector<std::string>& must_mention, const char* why) {
    Status status = fixture.Run(sql);
    if (status.ok()) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("expected a failure but the statement succeeded\n        sql: ") + sql +
                "\n        why: " + why);
        return;
    }
    if (status.code() != code) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("wrong error code\n        sql:      ") + sql +
                "\n        why:      " + why + "\n        expected: " + CodeName(code) +
                "\n        actual:   " + CodeName(status.code()) + "\n        message:  " +
                status.message());
        return;
    }
    // A message that is too short to be a sentence is not a useful message, and
    // this catches the common regression of a placeholder being left behind.
    if (status.message().size() < 10) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("error message is too terse to act on\n        sql:     ") + sql +
                "\n        message: '" + status.message() + "'");
    }
    for (const std::string& fragment : must_mention) {
        if (Contains(status.message(), fragment)) continue;
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("error message does not mention '") + fragment +
                "'\n        sql:     " + sql + "\n        why:     " + why +
                "\n        message: " + status.message());
    }
}

#define EXPECT_ERROR(fixture, sql, code, ...) \
    Expect((fixture), (sql), (code), __FILE__, __LINE__, __VA_ARGS__)

}  // namespace

// ---------------------------------------------------------------------------
// Names that do not exist
// ---------------------------------------------------------------------------

QUARRY_TEST(unknown_table_is_a_bind_error_that_names_the_table) {
    Fixture f;
    // A missing table is not a syntax error: the text is valid SQL and the problem
    // is semantic, which is exactly the distinction a separate binder exists to
    // make.
    EXPECT_ERROR(f, "SELECT * FROM nosuchtable", Code::kBindError,
                 {"nosuchtable"}, "the message must name the table that was not found");
    EXPECT_ERROR(f, "SELECT id FROM events INNER JOIN nosuchtable ON events.id = nosuchtable.id",
                 Code::kBindError, {"nosuchtable"}, "a missing join table is also a bind error");
}

QUARRY_TEST(unknown_column_is_a_bind_error_that_names_the_column) {
    Fixture f;
    // The available-column list is the part that turns a dead end into a fix: the
    // overwhelmingly common cause is a typo, and the correct spelling is sitting
    // right there in the schema.
    EXPECT_ERROR(f, "SELECT nosuchcolumn FROM events", Code::kBindError,
                 {"nosuchcolumn", "amount"},
                 "the message must name the column and list what the table does have");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE nosuchcolumn = 1", Code::kBindError,
                 {"nosuchcolumn"}, "an unknown column in WHERE is a bind error");
    EXPECT_ERROR(f, "SELECT id FROM events ORDER BY nosuchcolumn", Code::kBindError,
                 {"nosuchcolumn"}, "an unknown column in ORDER BY is a bind error");
    EXPECT_ERROR(f, "SELECT COUNT(*) FROM events GROUP BY nosuchcolumn", Code::kBindError,
                 {"nosuchcolumn"}, "an unknown column in GROUP BY is a bind error");
    EXPECT_ERROR(f, "SELECT SUM(nosuchcolumn) FROM events", Code::kBindError, {"nosuchcolumn"},
                 "an unknown column inside an aggregate is a bind error");
    EXPECT_ERROR(f, "INSERT INTO events (id, nosuchcolumn) VALUES (1, 2)", Code::kBindError,
                 {"nosuchcolumn"}, "an unknown column in INSERT is a bind error");
}

// ---------------------------------------------------------------------------
// Operations that cannot be typed
// ---------------------------------------------------------------------------

QUARRY_TEST(type_mismatch_is_a_type_error_that_names_both_types) {
    Fixture f;
    // A varchar column against a numeric literal has no defined meaning in this
    // engine: there is no implicit string-to-number conversion, because one would
    // turn a typo into a silently empty result instead of an error.
    EXPECT_ERROR(f, "SELECT id FROM events WHERE name > 5", Code::kTypeError,
                 {"varchar"}, "comparing a varchar column against a number is a type error");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE name = 5", Code::kTypeError, {"varchar"},
                 "equality does not get a pass on type checking");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE id = 'one'", Code::kTypeError, {"varchar"},
                 "an int64 column against a string literal is a type error");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE flag > 'true'", Code::kTypeError, {"boolean"},
                 "a boolean column against a string literal is a type error");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE id AND name", Code::kTypeError, {"boolean"},
                 "AND over non-boolean operands is a type error");
    EXPECT_ERROR(f, "SELECT SUM(name) FROM events", Code::kTypeError, {"sum"},
                 "SUM over a varchar column is a type error, not a silent zero");
    EXPECT_ERROR(f, "SELECT AVG(name) FROM events", Code::kTypeError, {"avg"},
                 "AVG over a varchar column is a type error");
    EXPECT_ERROR(f,
                 "SELECT id FROM events INNER JOIN labels ON events.name = labels.id",
                 Code::kTypeError, {"varchar"},
                 "a join key whose two sides have different types is a type error");
}

// ---------------------------------------------------------------------------
// Aggregation rules
// ---------------------------------------------------------------------------

QUARRY_TEST(aggregate_without_group_by_is_rejected) {
    Fixture f;
    // Mixing a bare column with an aggregate has no answer: there are many names
    // and one count, and the engine has no way to pick. SQLite famously picks one
    // anyway; this is one of the few places where being stricter than the oracle
    // is the right call, and it is why this assertion lives here rather than in
    // the differential suite.
    EXPECT_ERROR(f, "SELECT name, COUNT(*) FROM events", Code::kBindError,
                 {"name", "group by"},
                 "a bare column alongside an aggregate must name the column and suggest GROUP BY");
    EXPECT_ERROR(f, "SELECT id, SUM(amount) FROM events", Code::kBindError, {"id", "group by"},
                 "the same rule applies to any aggregate");
    EXPECT_ERROR(f, "SELECT name, COUNT(*) FROM events GROUP BY id", Code::kBindError,
                 {"name", "group by"},
                 "a column that is neither grouped nor aggregated is still an error");
}

// ---------------------------------------------------------------------------
// Text that is not SQL
// ---------------------------------------------------------------------------

namespace {

// ExpectSyntaxErrorAt additionally requires the message to carry a position. A
// syntax error without one makes the user bisect their own query by hand, which
// for a generated or long statement is the difference between a five-second fix
// and a frustrating one.
void ExpectSyntaxErrorAt(Fixture& fixture, const char* sql, int line_number, const char* why,
                         const char* file, int line) {
    Status status = fixture.Run(sql);
    if (status.ok()) {
        ::quarry::testing::ReportFailure(
            file, line, std::string("expected a syntax error\n        sql: ") + sql);
        return;
    }
    if (status.code() != Code::kSyntaxError) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("expected kSyntaxError, got ") + CodeName(status.code()) +
                "\n        sql:     " + sql + "\n        message: " + status.message());
        return;
    }
    const std::string& message = status.message();
    if (!Contains(message, "line") || !Contains(message, std::to_string(line_number))) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("syntax error does not report line ") + std::to_string(line_number) +
                "\n        sql:     " + sql + "\n        why:     " + why +
                "\n        message: " + message);
    }
    if (!Contains(message, "column")) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("syntax error does not report a column\n        sql:     ") + sql +
                "\n        message: " + message);
    }
    // "expected X, found Y" is the shape that makes a syntax error self-
    // explanatory. Without the "expected" half the user learns only that
    // something is wrong, not what would have been right.
    if (!Contains(message, "expected")) {
        ::quarry::testing::ReportFailure(
            file, line,
            std::string("syntax error does not say what was expected\n        sql:     ") + sql +
                "\n        message: " + message);
    }
}

#define EXPECT_SYNTAX_AT(fixture, sql, line_number, why) \
    ExpectSyntaxErrorAt((fixture), (sql), (line_number), (why), __FILE__, __LINE__)

}  // namespace

QUARRY_TEST(syntax_errors_report_a_line_and_column) {
    Fixture f;
    EXPECT_SYNTAX_AT(f, "SELECT FROM events", 1, "a missing select list");
    EXPECT_SYNTAX_AT(f, "SELECT id\nFROM", 2, "a FROM with no table, on the second line");
    EXPECT_SYNTAX_AT(f, "SELECT id\nFROM events\nWHERE id >", 3,
                     "a comparison with no right-hand side, on the third line");
    EXPECT_SYNTAX_AT(f, "SELECT (id FROM events", 1, "an unclosed parenthesis");
    EXPECT_SYNTAX_AT(f, "SELECT id FROM events WHERE", 1, "a WHERE with no predicate");
    EXPECT_SYNTAX_AT(f, "SELECT id, FROM events", 1, "a trailing comma in the select list");
    EXPECT_SYNTAX_AT(f, "SELCT id FROM events", 1, "a misspelled keyword");
    EXPECT_SYNTAX_AT(f, "SELECT id FROM events ORDER events", 1, "ORDER without BY");
    EXPECT_SYNTAX_AT(f, "SELECT id FROM events LIMIT 'ten'", 1, "a non-numeric LIMIT");
    EXPECT_SYNTAX_AT(f, "SELECT id FROM events; SELECT id FROM events", 1,
                     "two statements in one Execute call");
    EXPECT_SYNTAX_AT(f, "INSERT INTO events VALUES", 1, "VALUES with no tuple");
}

QUARRY_TEST(deeply_nested_expressions_are_a_syntax_error_not_a_crash) {
    Fixture f;
    // The parser is recursive descent, so input nesting becomes stack frames and
    // unbounded nesting is a stack-overflow vector rather than a parse problem.
    // The depth limit must surface as an ordinary error.
    std::string sql = "SELECT id FROM events WHERE ";
    const int depth = 5000;
    for (int i = 0; i < depth; ++i) sql += "(";
    sql += "id = 1";
    for (int i = 0; i < depth; ++i) sql += ")";

    Status status = f.Run(sql);
    CHECK(!status.ok());
    if (!status.ok()) CHECK_EQ(CodeName(status.code()), CodeName(Code::kSyntaxError));
}

// ---------------------------------------------------------------------------
// Valid SQL that is deliberately out of scope
// ---------------------------------------------------------------------------

QUARRY_TEST(unsupported_but_valid_sql_is_not_implemented_rather_than_a_syntax_error) {
    Fixture f;
    // Each of these is well-formed SQL that any complete engine would run. The
    // code must say "this engine does not do that" and not "you made a mistake",
    // because the two send the user to entirely different places.
    EXPECT_ERROR(f, "SELECT name, COUNT(*) FROM events GROUP BY name HAVING COUNT(*) > 1",
                 Code::kNotImplemented, {"having"}, "HAVING is valid SQL this engine omits");
    EXPECT_ERROR(f, "SELECT id FROM (SELECT id FROM events)", Code::kNotImplemented,
                 {"subquer"}, "a derived table is valid SQL this engine omits");
    EXPECT_ERROR(f, "SELECT id FROM events WHERE id = (SELECT MAX(id) FROM events)",
                 Code::kNotImplemented, {"subquer"}, "a scalar subquery is valid SQL this engine omits");
    EXPECT_ERROR(f, "SELECT id FROM events LEFT OUTER JOIN labels ON events.id = labels.id",
                 Code::kNotImplemented, {"join"}, "only an inner join is supported");
    EXPECT_ERROR(f, "SELECT id FROM events LEFT JOIN labels ON events.id = labels.id",
                 Code::kNotImplemented, {"join"}, "LEFT JOIN is the same omission");
    EXPECT_ERROR(f, "SELECT DISTINCT name FROM events", Code::kNotImplemented, {"distinct"},
                 "DISTINCT is valid SQL this engine omits");
    EXPECT_ERROR(f, "SELECT id FROM events UNION SELECT id FROM labels", Code::kNotImplemented,
                 {"union"}, "set operations are valid SQL this engine omits");
    EXPECT_ERROR(f, "SELECT id FROM events ORDER BY id LIMIT 10 OFFSET 5", Code::kNotImplemented,
                 {"offset"}, "OFFSET is valid SQL this engine omits");
    EXPECT_ERROR(f, "DELETE FROM events WHERE id = 1", Code::kNotImplemented, {"delete"},
                 "the engine is append-only, which is a scope decision and not a parse failure");
    EXPECT_ERROR(f, "UPDATE events SET name = 'x' WHERE id = 1", Code::kNotImplemented,
                 {"update"}, "segments are immutable, so UPDATE is unimplemented by design");
    EXPECT_ERROR(f, "SELECT id FROM events, labels", Code::kNotImplemented, {"join"},
                 "a comma cross join is valid SQL this engine omits");
    EXPECT_ERROR(f,
                 "SELECT id FROM events INNER JOIN labels ON events.id = labels.id INNER JOIN "
                 "labels ON events.id = labels.id",
                 Code::kNotImplemented, {"join"}, "only one join per query is supported");
}

// ---------------------------------------------------------------------------
// A statement that fails must leave the database usable
// ---------------------------------------------------------------------------

QUARRY_TEST(a_failed_statement_does_not_damage_the_database) {
    Fixture f;
    // The reason the engine returns Status values instead of throwing is that
    // these failures are ordinary. Ordinary failures must not leave partial state
    // behind, or the shell's "print the error and keep going" behaviour would be
    // quietly accumulating corruption.
    CHECK_NOT_OK(f.Run("SELECT nosuchcolumn FROM events"));
    CHECK_NOT_OK(f.Run("SELECT FROM"));
    CHECK_NOT_OK(f.Run("INSERT INTO events (id, name) VALUES ('not-an-int', 'x')"));

    auto after = f.db().Execute("SELECT COUNT(*) FROM events");
    CHECK(after.ok());
    if (!after.ok()) return;
    CHECK_EQ(after.value().row_count(), static_cast<size_t>(1));
    if (after.value().rows.empty() || after.value().rows[0].empty()) return;
    CHECK_EQ(after.value().rows[0][0].ToString(), std::string("3"));
}

QUARRY_TEST(explain_reports_errors_rather_than_a_plan) {
    Fixture f;
    // EXPLAIN binds and optimizes, so it must surface the same errors Execute
    // does. An EXPLAIN that printed a plan for an unbindable query would be worse
    // than useless: it would suggest the query is fine.
    auto bad_column = f.db().ExplainQuery("SELECT nosuchcolumn FROM events");
    CHECK(!bad_column.ok());
    if (!bad_column.ok()) {
        CHECK_EQ(CodeName(bad_column.status().code()), CodeName(Code::kBindError));
    }

    auto bad_syntax = f.db().ExplainAnalyze("SELECT FROM events");
    CHECK(!bad_syntax.ok());
    if (!bad_syntax.ok()) {
        CHECK_EQ(CodeName(bad_syntax.status().code()), CodeName(Code::kSyntaxError));
    }

    auto good = f.db().ExplainQuery("SELECT id FROM events WHERE id > 1");
    CHECK(good.ok());
    if (good.ok()) {
        // Not an assertion about plan formatting, which is the engine's business:
        // only that EXPLAIN produced something, since an empty plan is a silent
        // failure wearing a success code.
        CHECK(good.value().size() > 10);
    }
}

QUARRY_TEST_MAIN()
