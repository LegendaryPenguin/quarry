#include "../src/parser/parser.hpp"

#include <string>

#include "quarry/ast.hpp"
#include "quarry/status.hpp"
#include "testing.hpp"

using namespace quarry;  // NOLINT: test-local convenience
using quarry::parser::Parse;
using quarry::parser::ParseExpressionText;

namespace {

// Enum fields are compared as ints so CHECK_EQ can print both sides.
int I(ast::ExprKind k) { return static_cast<int>(k); }
int I(ast::BinaryOp op) { return static_cast<int>(op); }
int I(AggregateKind k) { return static_cast<int>(k); }
int I(TypeId t) { return static_cast<int>(t); }
int I(ast::StatementKind k) { return static_cast<int>(k); }

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Rejected asserts that a statement fails with a syntax error that carries a
// position, and returns the message so the caller can assert on its wording.
// Every malformed-input test goes through here, so "has a line and column" is
// checked once for all of them.
std::string Rejected(const std::string& sql) {
    Result<ast::Statement> parsed = Parse(sql);
    if (parsed.ok()) {
        testing::ReportFailure(__FILE__, __LINE__, "expected a syntax error for: " + sql);
        return "";
    }
    const Status& status = parsed.status();
    if (status.code() != Code::kSyntaxError) {
        testing::ReportFailure(__FILE__, __LINE__,
                               "expected kSyntaxError for " + sql + ", got " + status.ToString());
    }
    const std::string& message = status.message();
    if (!Contains(message, "line ") || !Contains(message, "column ")) {
        testing::ReportFailure(__FILE__, __LINE__,
                               "error for " + sql + " lacks a position: " + message);
    }
    return message;
}

std::string ExprString(const std::string& text) {
    Result<ast::ExprPtr> expr = ParseExpressionText(text);
    if (!expr.ok()) {
        testing::ReportFailure(__FILE__, __LINE__,
                               "failed to parse expression " + text + ": " +
                                   expr.status().message());
        return "";
    }
    return expr.value()->ToString();
}

}  // namespace

// PARSE_OR_RETURN binds a parsed statement or fails the test and returns, so that
// every following assertion can dereference the statement's unique_ptrs without
// each one re-checking for null.
#define PARSE_OR_RETURN(name, sql)                                                   \
    auto name##_result = Parse(sql);                                                 \
    if (!name##_result.ok()) {                                                       \
        ::quarry::testing::ReportFailure(__FILE__, __LINE__,                         \
                                         std::string("parse failed for ") + (sql) +  \
                                             ": " + name##_result.status().message()); \
        return;                                                                      \
    }                                                                                \
    const ::quarry::ast::Statement& name = name##_result.value()

// ---------------------------------------------------------------------------
// CREATE TABLE
// ---------------------------------------------------------------------------

QUARRY_TEST(ParsesCreateTableWithEveryTypeSpelling) {
    PARSE_OR_RETURN(stmt,
                    "CREATE TABLE Events ("
                    "  a BIGINT NOT NULL, b INT, c INTEGER,"
                    "  d DOUBLE, e FLOAT, f REAL,"
                    "  g VARCHAR(32), h TEXT, i VARCHAR,"
                    "  j BOOLEAN, k BOOL NOT NULL)");

    CHECK_EQ(I(stmt.kind), I(ast::StatementKind::kCreateTable));
    CHECK(stmt.create_table != nullptr);
    CHECK_EQ(stmt.create_table->table, std::string("events"));

    const Schema& schema = stmt.create_table->schema;
    CHECK_EQ(schema.size(), size_t{11});
    CHECK_EQ(schema.at(0).name, std::string("a"));
    CHECK_EQ(I(schema.at(0).type), I(TypeId::kInt64));
    CHECK(!schema.at(0).nullable);
    CHECK_EQ(I(schema.at(1).type), I(TypeId::kInt64));
    CHECK(schema.at(1).nullable);
    CHECK_EQ(I(schema.at(2).type), I(TypeId::kInt64));
    CHECK_EQ(I(schema.at(3).type), I(TypeId::kDouble));
    CHECK_EQ(I(schema.at(4).type), I(TypeId::kDouble));
    CHECK_EQ(I(schema.at(5).type), I(TypeId::kDouble));
    CHECK_EQ(I(schema.at(6).type), I(TypeId::kVarchar));
    CHECK_EQ(I(schema.at(7).type), I(TypeId::kVarchar));
    CHECK_EQ(I(schema.at(8).type), I(TypeId::kVarchar));
    CHECK_EQ(I(schema.at(9).type), I(TypeId::kBoolean));
    CHECK_EQ(I(schema.at(10).type), I(TypeId::kBoolean));
    CHECK(!schema.at(10).nullable);

    // Column order is the positional contract the engine relies on.
    const std::optional<size_t> last_index = schema.IndexOf("k");
    CHECK(last_index.has_value());
    CHECK_EQ(*last_index, size_t{10});
}

QUARRY_TEST(RejectsUnknownTypeName) {
    const std::string message = Rejected("CREATE TABLE t (a DECIMAL)");
    CHECK(Contains(message, "unknown type name 'decimal'"));
    CHECK(Contains(message, "column 19"));
}

QUARRY_TEST(RejectsTrailingCommaInColumnList) {
    const std::string message = Rejected("CREATE TABLE t (a BIGINT, b INT,)");
    CHECK(Contains(message, "expected a column name"));
    CHECK(Contains(message, "')'"));
}

QUARRY_TEST(RejectsPartialNotNull) {
    const std::string message = Rejected("CREATE TABLE t (a BIGINT NOT)");
    CHECK(Contains(message, "expected NULL"));
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

QUARRY_TEST(ParsesInsertWithMultipleRows) {
    PARSE_OR_RETURN(stmt, "INSERT INTO Metrics VALUES (1, 2.5, 'Ok'), (2, 3.5, 'Bad')");

    CHECK_EQ(I(stmt.kind), I(ast::StatementKind::kInsert));
    CHECK_EQ(stmt.insert->table, std::string("metrics"));
    CHECK(stmt.insert->columns.empty());
    CHECK_EQ(stmt.insert->rows.size(), size_t{2});
    CHECK_EQ(stmt.insert->rows[0].size(), size_t{3});

    const ast::Expression& first = *stmt.insert->rows[0][0];
    CHECK_EQ(I(first.kind), I(ast::ExprKind::kLiteral));
    CHECK_EQ(first.literal.as_int64(), int64_t{1});
    CHECK_EQ(stmt.insert->rows[0][1]->literal.as_double(), 2.5);
    CHECK_EQ(stmt.insert->rows[0][2]->literal.as_varchar(), std::string("Ok"));
    CHECK_EQ(stmt.insert->rows[1][0]->literal.as_int64(), int64_t{2});
}

QUARRY_TEST(ParsesInsertWithExplicitColumnsAndNegativesAndNull) {
    PARSE_OR_RETURN(stmt, "insert into t (Id, Score, Flag) values (-7, -1.5, NULL)");

    CHECK_EQ(stmt.insert->columns.size(), size_t{3});
    CHECK_EQ(stmt.insert->columns[0], std::string("id"));
    CHECK_EQ(stmt.insert->columns[2], std::string("flag"));

    // Unary minus on a literal is folded into the constant rather than becoming a
    // subtraction node.
    const ast::Expression& negative = *stmt.insert->rows[0][0];
    CHECK_EQ(I(negative.kind), I(ast::ExprKind::kLiteral));
    CHECK_EQ(negative.literal.as_int64(), int64_t{-7});
    CHECK_EQ(stmt.insert->rows[0][1]->literal.as_double(), -1.5);
    CHECK(stmt.insert->rows[0][2]->literal.is_null());
}

QUARRY_TEST(RejectsTrailingCommaInInsertColumnList) {
    const std::string message = Rejected("INSERT INTO t (a, b,) VALUES (1, 2)");
    CHECK(Contains(message, "expected a column name"));
}

QUARRY_TEST(RejectsInsertWithoutValues) {
    const std::string message = Rejected("INSERT INTO t (a)");
    CHECK(Contains(message, "expected VALUES"));
    CHECK(Contains(message, "end of input"));
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

QUARRY_TEST(ParsesSelectStar) {
    PARSE_OR_RETURN(stmt, "SELECT * FROM T");
    CHECK_EQ(I(stmt.kind), I(ast::StatementKind::kSelect));
    CHECK_EQ(stmt.select->projections.size(), size_t{1});
    CHECK_EQ(I(stmt.select->projections[0]->kind), I(ast::ExprKind::kStar));
    CHECK_EQ(stmt.select->from_table, std::string("t"));
    CHECK(stmt.select->where == nullptr);
    CHECK(!stmt.select->limit.has_value());
    CHECK(!stmt.select->join.has_value());
}

QUARRY_TEST(ParsesProjectionsWithAliasAndArithmetic) {
    PARSE_OR_RETURN(stmt, "SELECT a, b AS Renamed, a + b * 2 AS Total FROM t");
    const auto& projections = stmt.select->projections;
    CHECK_EQ(projections.size(), size_t{3});

    CHECK_EQ(I(projections[0]->kind), I(ast::ExprKind::kColumnRef));
    CHECK_EQ(projections[0]->column_name, std::string("a"));
    CHECK(projections[0]->alias.empty());

    CHECK_EQ(projections[1]->column_name, std::string("b"));
    CHECK_EQ(projections[1]->alias, std::string("renamed"));

    CHECK_EQ(I(projections[2]->kind), I(ast::ExprKind::kBinary));
    CHECK_EQ(I(projections[2]->op), I(ast::BinaryOp::kAdd));
    CHECK_EQ(projections[2]->alias, std::string("total"));
    CHECK_EQ(I(projections[2]->right->op), I(ast::BinaryOp::kMul));
}

QUARRY_TEST(ParsesQualifiedColumnReferences) {
    PARSE_OR_RETURN(stmt, "SELECT T.A FROM t WHERE t.b = 1");
    CHECK_EQ(stmt.select->projections[0]->column_name, std::string("t.a"));
    CHECK_EQ(stmt.select->where->left->column_name, std::string("t.b"));
}

QUARRY_TEST(CountStarDiffersFromCountColumn) {
    PARSE_OR_RETURN(star, "SELECT COUNT(*) FROM t");
    PARSE_OR_RETURN(column, "SELECT COUNT(x) FROM t");

    const ast::Expression& star_agg = *star.select->projections[0];
    CHECK_EQ(I(star_agg.kind), I(ast::ExprKind::kAggregate));
    CHECK_EQ(I(star_agg.agg), I(AggregateKind::kCount));
    // COUNT(*) is a null argument, which is how a row count is distinguished from
    // a count of non-null values in one column.
    CHECK(star_agg.agg_argument == nullptr);

    const ast::Expression& column_agg = *column.select->projections[0];
    CHECK_EQ(I(column_agg.kind), I(ast::ExprKind::kAggregate));
    CHECK_EQ(I(column_agg.agg), I(AggregateKind::kCount));
    CHECK(column_agg.agg_argument != nullptr);
    CHECK_EQ(I(column_agg.agg_argument->kind), I(ast::ExprKind::kColumnRef));
    CHECK_EQ(column_agg.agg_argument->column_name, std::string("x"));

    CHECK_NE(star_agg.ToString(), column_agg.ToString());
}

QUARRY_TEST(ParsesEveryAggregateKind) {
    PARSE_OR_RETURN(stmt, "SELECT SUM(a), AVG(a), MIN(a), MAX(a), COUNT(a) FROM t");
    const auto& projections = stmt.select->projections;
    CHECK_EQ(projections.size(), size_t{5});
    CHECK_EQ(I(projections[0]->agg), I(AggregateKind::kSum));
    CHECK_EQ(I(projections[1]->agg), I(AggregateKind::kAvg));
    CHECK_EQ(I(projections[2]->agg), I(AggregateKind::kMin));
    CHECK_EQ(I(projections[3]->agg), I(AggregateKind::kMax));
    CHECK_EQ(I(projections[4]->agg), I(AggregateKind::kCount));
}

QUARRY_TEST(AggregateOverAnExpression) {
    PARSE_OR_RETURN(stmt, "SELECT SUM(a * 2) FROM t");
    const ast::Expression& agg = *stmt.select->projections[0];
    CHECK_EQ(I(agg.agg), I(AggregateKind::kSum));
    CHECK_EQ(I(agg.agg_argument->kind), I(ast::ExprKind::kBinary));
    CHECK_EQ(I(agg.agg_argument->op), I(ast::BinaryOp::kMul));
}

QUARRY_TEST(RejectsStarArgumentForNonCountAggregate) {
    const std::string message = Rejected("SELECT SUM(*) FROM t");
    CHECK(Contains(message, "SUM(*) is not valid"));
}

QUARRY_TEST(ParsesGroupByWithAggregate) {
    PARSE_OR_RETURN(stmt, "SELECT region, COUNT(*) FROM sales GROUP BY region");
    CHECK_EQ(stmt.select->group_by.size(), size_t{1});
    CHECK_EQ(I(stmt.select->group_by[0]->kind), I(ast::ExprKind::kColumnRef));
    CHECK_EQ(stmt.select->group_by[0]->column_name, std::string("region"));
}

QUARRY_TEST(ParsesOrderByDirectionsAndLimit) {
    PARSE_OR_RETURN(stmt, "SELECT a FROM t ORDER BY a DESC, b ASC, c LIMIT 10");
    const auto& order = stmt.select->order_by;
    CHECK_EQ(order.size(), size_t{3});
    CHECK(order[0].descending);
    CHECK_EQ(order[0].expr->column_name, std::string("a"));
    CHECK(!order[1].descending);
    // No direction means ascending.
    CHECK(!order[2].descending);
    CHECK(stmt.select->limit.has_value());
    CHECK_EQ(*stmt.select->limit, uint64_t{10});
}

QUARRY_TEST(ParsesFullSelectWithJoin) {
    PARSE_OR_RETURN(stmt,
                    "SELECT o.id, c.name FROM orders INNER JOIN customers "
                    "ON orders.customer_id = customers.id "
                    "WHERE o.total > 100 GROUP BY c.name ORDER BY c.name DESC LIMIT 5");

    CHECK_EQ(stmt.select->from_table, std::string("orders"));
    CHECK(stmt.select->join.has_value());
    CHECK_EQ(stmt.select->join->table, std::string("customers"));
    CHECK_EQ(stmt.select->join->left_column, std::string("customer_id"));
    CHECK_EQ(stmt.select->join->right_column, std::string("id"));
    CHECK(stmt.select->where != nullptr);
    CHECK_EQ(I(stmt.select->where->op), I(ast::BinaryOp::kGt));
    CHECK_EQ(stmt.select->group_by.size(), size_t{1});
    CHECK_EQ(stmt.select->order_by.size(), size_t{1});
    CHECK_EQ(*stmt.select->limit, uint64_t{5});
}

QUARRY_TEST(JoinConditionIsOrderIndependent) {
    // The AST attributes each side to a table, so writing the joined table first
    // must produce the same clause.
    PARSE_OR_RETURN(forward, "SELECT * FROM a INNER JOIN b ON a.x = b.y");
    PARSE_OR_RETURN(reversed, "SELECT * FROM a INNER JOIN b ON b.y = a.x");
    CHECK_EQ(forward.select->join->left_column, std::string("x"));
    CHECK_EQ(forward.select->join->right_column, std::string("y"));
    CHECK_EQ(reversed.select->join->left_column, std::string("x"));
    CHECK_EQ(reversed.select->join->right_column, std::string("y"));
}

QUARRY_TEST(RejectsJoinWithoutOn) {
    const std::string message = Rejected("SELECT * FROM a INNER JOIN b WHERE x = 1");
    CHECK(Contains(message, "expected ON"));
    CHECK(Contains(message, "keyword 'where'"));
}

QUARRY_TEST(ParsesStringComparisonPreservingLiteralCase) {
    PARSE_OR_RETURN(stmt, "SELECT Foo FROM Bar WHERE Baz = 'MiXeD'");
    CHECK_EQ(stmt.select->projections[0]->column_name, std::string("foo"));
    CHECK_EQ(stmt.select->from_table, std::string("bar"));
    CHECK_EQ(stmt.select->where->left->column_name, std::string("baz"));
    CHECK_EQ(stmt.select->where->right->literal.as_varchar(), std::string("MiXeD"));
}

QUARRY_TEST(ParsesEscapedQuoteInsideWhereLiteral) {
    PARSE_OR_RETURN(stmt, "SELECT * FROM t WHERE name = 'O''Hara'");
    CHECK_EQ(stmt.select->where->right->literal.as_varchar(), std::string("O'Hara"));
}

QUARRY_TEST(ParsesBooleanLiterals) {
    PARSE_OR_RETURN(stmt, "SELECT * FROM t WHERE flag = TRUE");
    const Value& literal = stmt.select->where->right->literal;
    CHECK_EQ(I(literal.type()), I(TypeId::kBoolean));
    CHECK(literal.as_boolean());
}

QUARRY_TEST(AcceptsTrailingSemicolon) {
    PARSE_OR_RETURN(stmt, "SELECT * FROM t;");
    CHECK_EQ(stmt.select->from_table, std::string("t"));
}

// ---------------------------------------------------------------------------
// CREATE INDEX, EXPLAIN
// ---------------------------------------------------------------------------

QUARRY_TEST(ParsesCreateIndexDefaultingToBTree) {
    PARSE_OR_RETURN(stmt, "CREATE INDEX IdxA ON Events (Ts)");
    CHECK_EQ(I(stmt.kind), I(ast::StatementKind::kCreateIndex));
    CHECK_EQ(stmt.create_index->name, std::string("idxa"));
    CHECK_EQ(stmt.create_index->table, std::string("events"));
    CHECK_EQ(stmt.create_index->column, std::string("ts"));
    CHECK(!stmt.create_index->lsm);
}

QUARRY_TEST(ParsesCreateIndexUsingLsm) {
    PARSE_OR_RETURN(stmt, "CREATE INDEX i ON t (c) USING LSM");
    CHECK(stmt.create_index->lsm);
}

QUARRY_TEST(RejectsUnknownIndexStructure) {
    const std::string message = Rejected("CREATE INDEX i ON t (c) USING HASH");
    CHECK(Contains(message, "expected LSM"));
}

QUARRY_TEST(ParsesExplainAndExplainAnalyze) {
    PARSE_OR_RETURN(plain, "EXPLAIN SELECT a FROM t");
    CHECK_EQ(I(plain.kind), I(ast::StatementKind::kExplain));
    CHECK(!plain.analyze);
    CHECK(plain.explain_select != nullptr);
    CHECK_EQ(plain.explain_select->from_table, std::string("t"));
    CHECK(plain.select == nullptr);

    PARSE_OR_RETURN(analyzed, "EXPLAIN ANALYZE SELECT a FROM t WHERE a > 1");
    CHECK(analyzed.analyze);
    CHECK(analyzed.explain_select->where != nullptr);
}

QUARRY_TEST(RejectsExplainOfNonSelect) {
    const std::string message = Rejected("EXPLAIN INSERT INTO t VALUES (1)");
    CHECK(Contains(message, "expected SELECT after EXPLAIN"));
}

// ---------------------------------------------------------------------------
// Precedence
// ---------------------------------------------------------------------------

QUARRY_TEST(AndBindsTighterThanOr) {
    CHECK_EQ(ExprString("a = 1 OR b = 2 AND c = 3"),
             std::string("((a = 1) OR ((b = 2) AND (c = 3)))"));

    Result<ast::ExprPtr> expr = ParseExpressionText("a = 1 OR b = 2 AND c = 3");
    CHECK(expr.ok());
    if (!expr.ok()) return;
    const ast::Expression& root = *expr.value();
    CHECK_EQ(I(root.op), I(ast::BinaryOp::kOr));
    CHECK_EQ(I(root.left->op), I(ast::BinaryOp::kEq));
    CHECK_EQ(I(root.right->op), I(ast::BinaryOp::kAnd));
    CHECK_EQ(root.right->left->left->column_name, std::string("b"));
}

QUARRY_TEST(MultiplicationBindsTighterThanAddition) {
    CHECK_EQ(ExprString("1 + 2 * 3"), std::string("(1 + (2 * 3))"));
    CHECK_EQ(ExprString("(1 + 2) * 3"), std::string("((1 + 2) * 3)"));
    CHECK_EQ(ExprString("1 * 2 + 3"), std::string("((1 * 2) + 3)"));
    CHECK_EQ(ExprString("8 / 4 / 2"), std::string("((8 / 4) / 2)"));
    CHECK_EQ(ExprString("1 - 2 - 3"), std::string("((1 - 2) - 3)"));
}

QUARRY_TEST(ArithmeticBindsTighterThanComparison) {
    CHECK_EQ(ExprString("a + 1 = b * 2"), std::string("((a + 1) = (b * 2))"));
    CHECK_EQ(ExprString("a < 1 AND b >= 2"), std::string("((a < 1) AND (b >= 2))"));
    CHECK_EQ(ExprString("a <> 1 OR b != 2"), std::string("((a <> 1) OR (b <> 2))"));
}

QUARRY_TEST(ParenthesesOverrideLogicalPrecedence) {
    CHECK_EQ(ExprString("(a = 1 OR b = 2) AND c = 3"),
             std::string("(((a = 1) OR (b = 2)) AND (c = 3))"));
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

QUARRY_TEST(RejectsMissingFrom) {
    const std::string message = Rejected("SELECT a, b");
    CHECK(Contains(message, "expected FROM"));
    CHECK(Contains(message, "found end of input"));
}

QUARRY_TEST(RejectsSelectWithNoProjections) {
    const std::string message = Rejected("SELECT FROM t");
    CHECK(Contains(message, "expected an expression"));
    CHECK(Contains(message, "keyword 'from'"));
    CHECK(Contains(message, "column 8"));
}

QUARRY_TEST(RejectsUnclosedParenthesis) {
    const std::string message = Rejected("SELECT (a + 1 FROM t");
    CHECK(Contains(message, "expected ')' to close a parenthesized expression"));
    CHECK(Contains(message, "keyword 'from'"));
}

QUARRY_TEST(RejectsUnclosedCreateTableParenthesis) {
    const std::string message = Rejected("CREATE TABLE t (a BIGINT");
    CHECK(Contains(message, "expected ')' after the column list"));
}

QUARRY_TEST(RejectsUnterminatedStringLiteral) {
    const std::string message = Rejected("SELECT * FROM t WHERE a = 'abc");
    CHECK(Contains(message, "unterminated string literal"));
    CHECK(Contains(message, "column 27"));
}

QUARRY_TEST(RejectsLimitWithoutANumber) {
    const std::string message = Rejected("SELECT a FROM t LIMIT");
    CHECK(Contains(message, "expected a non-negative integer after LIMIT"));

    const std::string not_a_number = Rejected("SELECT a FROM t LIMIT x");
    CHECK(Contains(not_a_number, "expected a non-negative integer after LIMIT"));
    CHECK(Contains(not_a_number, "identifier 'x'"));

    const std::string negative = Rejected("SELECT a FROM t LIMIT -1");
    CHECK(Contains(negative, "expected a non-negative integer after LIMIT"));
}

QUARRY_TEST(RejectsBareGroupBy) {
    const std::string message = Rejected("SELECT a FROM t GROUP BY");
    CHECK(Contains(message, "expected an expression"));
    CHECK(Contains(message, "end of input"));

    const std::string missing_by = Rejected("SELECT a FROM t GROUP a");
    CHECK(Contains(missing_by, "expected BY"));
}

QUARRY_TEST(RejectsBareOrderBy) {
    const std::string message = Rejected("SELECT a FROM t ORDER BY");
    CHECK(Contains(message, "expected an expression"));
}

QUARRY_TEST(RejectsStarMixedWithOtherProjections) {
    CHECK(Contains(Rejected("SELECT *, a FROM t"), "cannot be combined"));
    CHECK(Contains(Rejected("SELECT a, * FROM t"), "cannot be combined"));
}

QUARRY_TEST(RejectsTextAfterTheStatement) {
    const std::string message = Rejected("SELECT * FROM t; SELECT * FROM u");
    CHECK(Contains(message, "expected end of statement"));
}

QUARRY_TEST(RejectsUnsupportedNotOperator) {
    const std::string message = Rejected("SELECT * FROM t WHERE NOT a = 1");
    CHECK(Contains(message, "NOT is not supported"));
}

QUARRY_TEST(RejectsUnknownFunction) {
    const std::string message = Rejected("SELECT lower(a) FROM t");
    CHECK(Contains(message, "unknown function 'lower'"));
}

QUARRY_TEST(RejectsStatementThatStartsWithGarbage) {
    const std::string message = Rejected("DELETE FROM t");
    CHECK(Contains(message, "expected a statement"));
}

QUARRY_TEST(RejectsEmptyInput) {
    const std::string message = Rejected("");
    CHECK(Contains(message, "expected a statement"));
    CHECK(Contains(message, "end of input"));
}

QUARRY_TEST(RejectsDanglingOperator) {
    CHECK(Contains(Rejected("SELECT a + FROM t"), "expected an expression"));
    CHECK(Contains(Rejected("SELECT a FROM t WHERE a = "), "expected an expression"));
    CHECK(Contains(Rejected("SELECT a FROM t WHERE a AND"), "expected an expression"));
}

// ---------------------------------------------------------------------------
// Resource bounds
// ---------------------------------------------------------------------------

QUARRY_TEST(DeeplyNestedParenthesesAreRejectedNotFatal) {
    // Untrusted text must not be able to turn nesting into stack frames without
    // limit. The expected outcome is an error, and the test passing at all is the
    // assertion that the process survived.
    std::string sql = "SELECT * FROM t WHERE a = ";
    sql.append(100000, '(');
    sql += "1";
    sql.append(100000, ')');

    Result<ast::Statement> parsed = Parse(sql);
    CHECK(!parsed.ok());
    if (!parsed.ok()) {
        CHECK_EQ(static_cast<int>(parsed.status().code()), static_cast<int>(Code::kSyntaxError));
        CHECK(Contains(parsed.status().message(), "nests deeper than the limit"));
    }
}

QUARRY_TEST(DeeplyNestedUnaryMinusIsRejectedNotFatal) {
    std::string expr(100000, '-');
    expr += "1";
    Result<ast::ExprPtr> parsed = ParseExpressionText(expr);
    CHECK(!parsed.ok());
}

QUARRY_TEST(NestingJustInsideTheLimitStillParses) {
    const int depth = quarry::parser::kMaxExpressionDepth;
    std::string expr;
    expr.append(static_cast<size_t>(depth), '(');
    expr += "1";
    expr.append(static_cast<size_t>(depth), ')');
    Result<ast::ExprPtr> parsed = ParseExpressionText(expr);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        CHECK_EQ(I(parsed.value()->kind), I(ast::ExprKind::kLiteral));
    }
}

QUARRY_TEST(LongFlatExpressionParses) {
    // Length is not depth: a flat chain of 20000 terms is parsed by the loops in
    // each precedence level and must not hit the nesting limit.
    std::string expr = "1";
    for (int i = 0; i < 20000; ++i) expr += " + 1";
    Result<ast::ExprPtr> parsed = ParseExpressionText(expr);
    CHECK(parsed.ok());
}

QUARRY_TEST(LongProjectionAndValuesListsParse) {
    std::string select = "SELECT c0";
    for (int i = 1; i < 2000; ++i) select += ", c" + std::to_string(i);
    select += " FROM t";
    Result<ast::Statement> parsed = Parse(select);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        CHECK_EQ(parsed.value().select->projections.size(), size_t{2000});
    }

    std::string insert = "INSERT INTO t VALUES (1)";
    for (int i = 0; i < 2000; ++i) insert += ", (1)";
    Result<ast::Statement> rows = Parse(insert);
    CHECK(rows.ok());
    if (rows.ok()) {
        CHECK_EQ(rows.value().insert->rows.size(), size_t{2001});
    }
}

// ---------------------------------------------------------------------------
// Round-tripping, as a cheap whole-AST assertion
// ---------------------------------------------------------------------------

QUARRY_TEST(StatementToStringShowsTheParsedShape) {
    PARSE_OR_RETURN(stmt,
                    "SELECT COUNT(*) AS n FROM sales WHERE region = 'US' AND amount > 10 "
                    "GROUP BY region ORDER BY region DESC LIMIT 3");
    CHECK_EQ(stmt.ToString(),
             std::string("SELECT COUNT(*) AS n FROM sales WHERE "
                         "((region = 'US') AND (amount > 10)) GROUP BY region "
                         "ORDER BY region DESC LIMIT 3"));
}

QUARRY_TEST_MAIN()
