// Tests for the vectorized expression evaluator.
//
// The evaluator is where SQL's three-valued logic either is or is not implemented
// correctly, and a mistake there does not crash — it silently returns the wrong
// rows. So the truth tables are spelled out exhaustively rather than sampled.

#include <memory>
#include <string>
#include <vector>

#include "quarry/ast.hpp"
#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/types.hpp"
#include "testing.hpp"

#include "../src/execution/exec_util.hpp"
#include "../src/execution/expression.hpp"

using namespace quarry;  // NOLINT: a test file is exactly where this is fine
using quarry::ast::BinaryOp;
using quarry::ast::Expression;

namespace {

Schema MakeSchema(std::vector<Column> columns) { return Schema(std::move(columns)); }

// MakeBatch builds a batch row-wise from Values. Row-wise is the wrong shape for
// execution and the right shape for writing a test by hand, which is the only
// place it is used.
std::unique_ptr<Batch> MakeBatch(const Schema& schema,
                                const std::vector<std::vector<Value>>& rows) {
    auto batch = std::make_unique<Batch>(schema);
    for (const auto& row : rows) {
        for (size_t c = 0; c < schema.size(); ++c) {
            Status s = AppendValue(batch->column(c), row[c]);
            if (!s.ok()) {
                ::quarry::testing::ReportFailure(__FILE__, __LINE__,
                                                 "MakeBatch: " + s.ToString());
            }
        }
    }
    batch->SetRowCount(rows.size());
    return batch;
}

Value N() { return Value::Null(); }
Value I(int64_t v) { return Value::Int64(v); }
Value D(double v) { return Value::Double(v); }
Value S(const char* v) { return Value::Varchar(v); }
Value B(bool v) { return Value::Boolean(v); }

// Tri is a readable spelling of a three-valued result for the truth tables below.
enum class Tri { kTrue, kFalse, kNull };

Tri TriAt(const Vector& v, size_t i) {
    if (v.nulls().IsNull(i)) return Tri::kNull;
    return v.bool_data()[i] != 0 ? Tri::kTrue : Tri::kFalse;
}

const char* TriName(Tri t) {
    switch (t) {
        case Tri::kTrue:  return "TRUE";
        case Tri::kFalse: return "FALSE";
        default:          return "NULL";
    }
}

std::ostream& operator<<(std::ostream& os, Tri t) { return os << TriName(t); }

// CHECK_EQ prints both sides, so the enums it is used on need a spelling.
std::ostream& operator<<(std::ostream& os, TypeId t) { return os << TypeName(t); }

}  // namespace

QUARRY_TEST(LiteralsEvaluateToConstantColumns) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(1)}, {I(2)}, {I(3)}});
    ExpressionEvaluator eval(schema);

    auto lit = Expression::Literal(I(7));
    auto r = eval.Evaluate(*lit, *batch);
    CHECK_OK(r.ok() ? Status::Ok() : r.status());
    CHECK_EQ(r.value().size(), size_t{3});
    for (size_t i = 0; i < 3; ++i) CHECK_EQ(r.value().vector().int64_data()[i], int64_t{7});

    auto str = Expression::Literal(S("hello"));
    auto rs = eval.Evaluate(*str, *batch);
    CHECK_OK(rs.ok() ? Status::Ok() : rs.status());
    CHECK_EQ(std::string(rs.value().vector().GetString(2)), std::string("hello"));

    auto nul = Expression::Literal(N());
    auto rn = eval.Evaluate(*nul, *batch);
    CHECK_OK(rn.ok() ? Status::Ok() : rn.status());
    CHECK(rn.value().vector().nulls().IsNull(0));
}

QUARRY_TEST(ColumnRefsOfEveryType) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("d", TypeId::kDouble),
                                      Column("s", TypeId::kVarchar),
                                      Column("b", TypeId::kBoolean)});
    auto batch = MakeBatch(schema, {{I(5), D(2.5), S("x"), B(true)},
                                    {I(-5), D(-2.5), S("yy"), B(false)}});
    ExpressionEvaluator eval(schema);

    auto i = Expression::ColumnRef("i");
    auto ri = eval.Evaluate(*i, *batch);
    CHECK_OK(ri.ok() ? Status::Ok() : ri.status());
    CHECK_EQ(ri.value().vector().int64_data()[1], int64_t{-5});

    auto d = Expression::ColumnRef("d");
    auto rd = eval.Evaluate(*d, *batch);
    CHECK_OK(rd.ok() ? Status::Ok() : rd.status());
    CHECK_EQ(rd.value().vector().double_data()[0], 2.5);

    auto s = Expression::ColumnRef("s");
    auto rs = eval.Evaluate(*s, *batch);
    CHECK_OK(rs.ok() ? Status::Ok() : rs.status());
    CHECK_EQ(std::string(rs.value().vector().GetString(1)), std::string("yy"));

    auto b = Expression::ColumnRef("b");
    auto rb = eval.Evaluate(*b, *batch);
    CHECK_OK(rb.ok() ? Status::Ok() : rb.status());
    CHECK_EQ(rb.value().vector().bool_data()[0] != 0, true);

    auto missing = Expression::ColumnRef("nope");
    auto rm = eval.Evaluate(*missing, *batch);
    CHECK(!rm.ok());
    CHECK_EQ(static_cast<int>(rm.status().code()), static_cast<int>(Code::kBindError));
}

QUARRY_TEST(ComparisonsOnEveryType) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("d", TypeId::kDouble),
                                      Column("s", TypeId::kVarchar),
                                      Column("b", TypeId::kBoolean)});
    auto batch = MakeBatch(schema, {{I(1), D(1.0), S("apple"), B(false)},
                                    {I(2), D(2.5), S("banana"), B(true)},
                                    {I(3), D(3.5), S("cherry"), B(true)}});
    ExpressionEvaluator eval(schema);

    struct Case {
        const char* column;
        BinaryOp op;
        Value constant;
        std::vector<uint32_t> expected;
    };
    const std::vector<Case> cases = {
        {"i", BinaryOp::kEq, I(2), {1}},
        {"i", BinaryOp::kNe, I(2), {0, 2}},
        {"i", BinaryOp::kLt, I(3), {0, 1}},
        {"i", BinaryOp::kLe, I(2), {0, 1}},
        {"i", BinaryOp::kGt, I(1), {1, 2}},
        {"i", BinaryOp::kGe, I(3), {2}},
        {"d", BinaryOp::kGt, D(2.0), {1, 2}},
        {"s", BinaryOp::kLt, S("banana"), {0}},
        {"s", BinaryOp::kGe, S("banana"), {1, 2}},
        {"b", BinaryOp::kEq, B(true), {1, 2}},
    };

    for (const Case& c : cases) {
        auto expr = Expression::Binary(c.op, Expression::ColumnRef(c.column),
                                       Expression::Literal(c.constant));
        auto rows = eval.Select(*expr, *batch);
        CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
        CHECK_EQ(rows.value().size(), c.expected.size());
        if (rows.value().size() == c.expected.size()) {
            for (size_t i = 0; i < c.expected.size(); ++i) {
                CHECK_EQ(rows.value()[i], c.expected[i]);
            }
        }
    }
}

QUARRY_TEST(MixedIntAndDoubleComparison) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("d", TypeId::kDouble)});
    auto batch = MakeBatch(schema, {{I(2), D(1.5)}, {I(2), D(2.0)}, {I(3), D(2.5)}});
    ExpressionEvaluator eval(schema);

    // int column against a double literal
    auto a = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("i"),
                                Expression::Literal(D(2.5)));
    auto ra = eval.Select(*a, *batch);
    CHECK_OK(ra.ok() ? Status::Ok() : ra.status());
    CHECK_EQ(ra.value().size(), size_t{1});
    CHECK_EQ(ra.value()[0], uint32_t{2});

    // int column against a double column, both directions
    auto b = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("i"),
                                Expression::ColumnRef("d"));
    auto rb = eval.Select(*b, *batch);
    CHECK_OK(rb.ok() ? Status::Ok() : rb.status());
    CHECK_EQ(rb.value().size(), size_t{2});  // 2>1.5 and 3>2.5, but not 2>2.0
    CHECK_EQ(rb.value()[0], uint32_t{0});
    CHECK_EQ(rb.value()[1], uint32_t{2});

    auto c = Expression::Binary(BinaryOp::kEq, Expression::ColumnRef("d"),
                                Expression::ColumnRef("i"));
    auto rc = eval.Select(*c, *batch);
    CHECK_OK(rc.ok() ? Status::Ok() : rc.status());
    CHECK_EQ(rc.value().size(), size_t{1});
    CHECK_EQ(rc.value()[0], uint32_t{1});
}

QUARRY_TEST(ComparisonWithNullIsUnknownAndDoesNotPass) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(1)}, {N()}, {I(3)}});
    ExpressionEvaluator eval(schema);

    // Every operator, including <>, must drop the null row rather than keep it.
    for (BinaryOp op : {BinaryOp::kEq, BinaryOp::kNe, BinaryOp::kLt, BinaryOp::kLe,
                        BinaryOp::kGt, BinaryOp::kGe}) {
        auto expr = Expression::Binary(op, Expression::ColumnRef("i"),
                                       Expression::Literal(I(1)));
        auto rows = eval.Select(*expr, *batch);
        CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
        for (uint32_t row : rows.value()) CHECK_NE(row, uint32_t{1});

        auto values = eval.Evaluate(*expr, *batch);
        CHECK_OK(values.ok() ? Status::Ok() : values.status());
        CHECK(values.value().vector().nulls().IsNull(1));
    }

    // `i = NULL` is unknown for every row, so nothing passes at all.
    auto against_null = Expression::Binary(BinaryOp::kEq, Expression::ColumnRef("i"),
                                           Expression::Literal(N()));
    auto none = eval.Select(*against_null, *batch);
    CHECK_OK(none.ok() ? Status::Ok() : none.status());
    CHECK_EQ(none.value().size(), size_t{0});
}

QUARRY_TEST(ThreeValuedLogicTruthTables) {
    const Schema schema = MakeSchema({Column("a", TypeId::kBoolean),
                                      Column("b", TypeId::kBoolean)});
    // All nine combinations of TRUE, FALSE and UNKNOWN, in a fixed order.
    auto batch = MakeBatch(schema, {{B(true), B(true)},
                                    {B(true), B(false)},
                                    {B(true), N()},
                                    {B(false), B(true)},
                                    {B(false), B(false)},
                                    {B(false), N()},
                                    {N(), B(true)},
                                    {N(), B(false)},
                                    {N(), N()}});
    ExpressionEvaluator eval(schema);

    const std::vector<Tri> expected_and = {Tri::kTrue,  Tri::kFalse, Tri::kNull,
                                           Tri::kFalse, Tri::kFalse, Tri::kFalse,
                                           Tri::kNull,  Tri::kFalse, Tri::kNull};
    const std::vector<Tri> expected_or = {Tri::kTrue, Tri::kTrue, Tri::kTrue,
                                          Tri::kTrue, Tri::kFalse, Tri::kNull,
                                          Tri::kTrue, Tri::kNull, Tri::kNull};

    auto conj = Expression::Binary(BinaryOp::kAnd, Expression::ColumnRef("a"),
                                   Expression::ColumnRef("b"));
    auto ra = eval.Evaluate(*conj, *batch);
    CHECK_OK(ra.ok() ? Status::Ok() : ra.status());
    for (size_t i = 0; i < expected_and.size(); ++i) {
        CHECK_EQ(TriAt(ra.value().vector(), i), expected_and[i]);
    }

    auto disj = Expression::Binary(BinaryOp::kOr, Expression::ColumnRef("a"),
                                   Expression::ColumnRef("b"));
    auto ro = eval.Evaluate(*disj, *batch);
    CHECK_OK(ro.ok() ? Status::Ok() : ro.status());
    for (size_t i = 0; i < expected_or.size(); ++i) {
        CHECK_EQ(TriAt(ro.value().vector(), i), expected_or[i]);
    }

    // Only rows that are TRUE survive a filter: UNKNOWN is not TRUE.
    auto rows = eval.Select(*conj, *batch);
    CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
    CHECK_EQ(rows.value().size(), size_t{1});
    CHECK_EQ(rows.value()[0], uint32_t{0});

    auto or_rows = eval.Select(*disj, *batch);
    CHECK_OK(or_rows.ok() ? Status::Ok() : or_rows.status());
    // TRUE in five of the nine combinations; the two UNKNOWN results do not pass.
    CHECK_EQ(or_rows.value().size(), size_t{5});
}

QUARRY_TEST(ThreeValuedLogicWithANullLiteralOperand) {
    const Schema schema = MakeSchema({Column("a", TypeId::kBoolean)});
    auto batch = MakeBatch(schema, {{B(true)}, {B(false)}});
    ExpressionEvaluator eval(schema);

    // TRUE AND NULL is NULL; FALSE AND NULL is FALSE.
    auto conj = Expression::Binary(BinaryOp::kAnd, Expression::ColumnRef("a"),
                                   Expression::Literal(N()));
    auto ra = eval.Evaluate(*conj, *batch);
    CHECK_OK(ra.ok() ? Status::Ok() : ra.status());
    CHECK_EQ(TriAt(ra.value().vector(), 0), Tri::kNull);
    CHECK_EQ(TriAt(ra.value().vector(), 1), Tri::kFalse);

    // TRUE OR NULL is TRUE; FALSE OR NULL is NULL.
    auto disj = Expression::Binary(BinaryOp::kOr, Expression::ColumnRef("a"),
                                   Expression::Literal(N()));
    auto ro = eval.Evaluate(*disj, *batch);
    CHECK_OK(ro.ok() ? Status::Ok() : ro.status());
    CHECK_EQ(TriAt(ro.value().vector(), 0), Tri::kTrue);
    CHECK_EQ(TriAt(ro.value().vector(), 1), Tri::kNull);
}

QUARRY_TEST(IntegerArithmetic) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(7), I(2)}, {I(-7), I(2)}, {I(5), I(0)}, {I(9), N()}});
    ExpressionEvaluator eval(schema);

    const auto run = [&](BinaryOp op) {
        auto expr = Expression::Binary(op, Expression::ColumnRef("a"),
                                       Expression::ColumnRef("b"));
        auto r = eval.Evaluate(*expr, *batch);
        CHECK_OK(r.ok() ? Status::Ok() : r.status());
        return r;
    };

    auto add = run(BinaryOp::kAdd);
    CHECK_EQ(add.value().vector().type(), TypeId::kInt64);
    CHECK_EQ(add.value().vector().int64_data()[0], int64_t{9});
    auto sub = run(BinaryOp::kSub);
    CHECK_EQ(sub.value().vector().int64_data()[1], int64_t{-9});
    auto mul = run(BinaryOp::kMul);
    CHECK_EQ(mul.value().vector().int64_data()[0], int64_t{14});

    auto div = run(BinaryOp::kDiv);
    // Integer division truncates toward zero: 7/2 is 3 and -7/2 is -3.
    CHECK_EQ(div.value().vector().int64_data()[0], int64_t{3});
    CHECK_EQ(div.value().vector().int64_data()[1], int64_t{-3});
    // DOCUMENTED: division by zero yields NULL rather than failing the query.
    CHECK(div.value().vector().nulls().IsNull(2));
    // A NULL operand poisons the row for every arithmetic operator.
    CHECK(add.value().vector().nulls().IsNull(3));
    CHECK(div.value().vector().nulls().IsNull(3));
}

QUARRY_TEST(DoubleArithmeticAndPromotion) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("d", TypeId::kDouble)});
    auto batch = MakeBatch(schema, {{I(3), D(1.5)}, {I(4), D(0.0)}});
    ExpressionEvaluator eval(schema);

    auto mixed = Expression::Binary(BinaryOp::kAdd, Expression::ColumnRef("i"),
                                    Expression::ColumnRef("d"));
    auto r = eval.Evaluate(*mixed, *batch);
    CHECK_OK(r.ok() ? Status::Ok() : r.status());
    CHECK_EQ(r.value().vector().type(), TypeId::kDouble);
    CHECK_EQ(r.value().vector().double_data()[0], 4.5);

    // DOCUMENTED: float division by zero is NULL too, not infinity, so one bad row
    // cannot quietly poison a SUM.
    auto div = Expression::Binary(BinaryOp::kDiv, Expression::ColumnRef("i"),
                                  Expression::ColumnRef("d"));
    auto rd = eval.Evaluate(*div, *batch);
    CHECK_OK(rd.ok() ? Status::Ok() : rd.status());
    CHECK_EQ(rd.value().vector().double_data()[0], 2.0);
    CHECK(rd.value().vector().nulls().IsNull(1));
}

QUARRY_TEST(ArithmeticOnIncompatibleTypesIsATypeError) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("s", TypeId::kVarchar)});
    auto batch = MakeBatch(schema, {{I(1), S("a")}});
    ExpressionEvaluator eval(schema);

    auto bad = Expression::Binary(BinaryOp::kAdd, Expression::ColumnRef("i"),
                                  Expression::ColumnRef("s"));
    auto r = eval.Evaluate(*bad, *batch);
    CHECK(!r.ok());
    CHECK_EQ(static_cast<int>(r.status().code()), static_cast<int>(Code::kTypeError));

    auto cmp = Expression::Binary(BinaryOp::kLt, Expression::ColumnRef("i"),
                                  Expression::ColumnRef("s"));
    auto rc = eval.Evaluate(*cmp, *batch);
    CHECK(!rc.ok());
    CHECK_EQ(static_cast<int>(rc.status().code()), static_cast<int>(Code::kTypeError));

    auto logic = Expression::Binary(BinaryOp::kAnd, Expression::ColumnRef("i"),
                                    Expression::ColumnRef("i"));
    auto rl = eval.Evaluate(*logic, *batch);
    CHECK(!rl.ok());
    CHECK_EQ(static_cast<int>(rl.status().code()), static_cast<int>(Code::kTypeError));
}

QUARRY_TEST(NestedExpressionTree) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64), Column("b", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(1), I(10)}, {I(2), I(20)}, {I(3), I(30)}, {I(4), I(40)}});
    ExpressionEvaluator eval(schema);

    // (a * 10 = b) AND (a > 1 OR b < 0)
    auto left = Expression::Binary(
        BinaryOp::kEq,
        Expression::Binary(BinaryOp::kMul, Expression::ColumnRef("a"),
                           Expression::Literal(I(10))),
        Expression::ColumnRef("b"));
    auto right = Expression::Binary(
        BinaryOp::kOr,
        Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"), Expression::Literal(I(1))),
        Expression::Binary(BinaryOp::kLt, Expression::ColumnRef("b"), Expression::Literal(I(0))));
    auto expr = Expression::Binary(BinaryOp::kAnd, std::move(left), std::move(right));

    auto rows = eval.Select(*expr, *batch);
    CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
    CHECK_EQ(rows.value().size(), size_t{3});
    CHECK_EQ(rows.value()[0], uint32_t{1});
}

QUARRY_TEST(EvaluationHonoursAnExistingSelection) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(10)}, {I(20)}, {I(30)}, {I(40)}, {I(50)}});
    // Rows 1 and 3 survived an earlier filter; 0, 2 and 4 are physically present
    // but dead. An evaluator that looped 0..row_count() would resurrect them.
    batch->SetSelection({1, 3});

    ExpressionEvaluator eval(schema);
    auto expr = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(5)));
    auto rows = eval.Select(*expr, *batch);
    CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
    CHECK_EQ(rows.value().size(), size_t{2});
    CHECK_EQ(rows.value()[0], uint32_t{1});
    CHECK_EQ(rows.value()[1], uint32_t{3});

    // The compacted result has one slot per LIVE row, in selection order.
    auto values = eval.Evaluate(*Expression::ColumnRef("a"), *batch);
    CHECK_OK(values.ok() ? Status::Ok() : values.status());
    CHECK_EQ(values.value().size(), size_t{2});
    CHECK_EQ(values.value().vector().int64_data()[0], int64_t{20});
    CHECK_EQ(values.value().vector().int64_data()[1], int64_t{40});
}

QUARRY_TEST(EmptyBatchEvaluatesToNothing) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {});
    ExpressionEvaluator eval(schema);

    auto expr = Expression::Binary(BinaryOp::kGt, Expression::ColumnRef("a"),
                                   Expression::Literal(I(0)));
    auto rows = eval.Select(*expr, *batch);
    CHECK_OK(rows.ok() ? Status::Ok() : rows.status());
    CHECK_EQ(rows.value().size(), size_t{0});
}

QUARRY_TEST(PushedPredicateIntersectsTheExistingSelection) {
    const Schema schema = MakeSchema({Column("a", TypeId::kInt64)});
    auto batch = MakeBatch(schema, {{I(1)}, {I(2)}, {N()}, {I(4)}, {I(5)}});
    batch->SetSelection({0, 2, 3});

    Predicate pred;
    pred.column_index = 0;
    pred.op = CompareOp::kGe;
    pred.constant = I(2);
    CHECK_OK(ApplyPredicate(*batch, pred));

    // Row 4 was already dead, row 2 is NULL and therefore unknown, so only row 3
    // survives.
    CHECK_EQ(batch->selected_count(), size_t{1});
    CHECK_EQ(batch->Rows()[0], uint32_t{3});
    CHECK_EQ(batch->row_count(), size_t{5});  // nothing was physically removed
    CHECK_EQ(pred.ToString(schema), std::string("a >= 2"));
}

QUARRY_TEST(ResultTypesAreKnownBeforeEvaluation) {
    const Schema schema = MakeSchema({Column("i", TypeId::kInt64), Column("d", TypeId::kDouble),
                                      Column("s", TypeId::kVarchar)});
    ExpressionEvaluator eval(schema);

    const auto type_of = [&](const ast::ExprPtr& e) {
        auto t = eval.ResultType(*e);
        CHECK_OK(t.ok() ? Status::Ok() : t.status());
        return t.ok() ? t.value() : TypeId::kInvalid;
    };

    CHECK_EQ(type_of(Expression::ColumnRef("s")), TypeId::kVarchar);
    CHECK_EQ(type_of(Expression::Binary(BinaryOp::kAdd, Expression::ColumnRef("i"),
                                        Expression::Literal(I(1)))),
             TypeId::kInt64);
    CHECK_EQ(type_of(Expression::Binary(BinaryOp::kAdd, Expression::ColumnRef("i"),
                                        Expression::ColumnRef("d"))),
             TypeId::kDouble);
    CHECK_EQ(type_of(Expression::Binary(BinaryOp::kLt, Expression::ColumnRef("d"),
                                        Expression::Literal(I(1)))),
             TypeId::kBoolean);
}

QUARRY_TEST_MAIN()
