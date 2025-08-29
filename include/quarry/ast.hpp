#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/operator.hpp"
#include "quarry/schema.hpp"
#include "quarry/types.hpp"

namespace quarry::ast {

// The AST is the parser's output and the binder's input. It is deliberately
// untyped: a ColumnRef here is a name, not an index, and resolving it is the
// binder's job. Keeping parse and resolution separate means a syntax error and an
// unknown-column error come from different places and can say different things.

struct Expression;
using ExprPtr = std::unique_ptr<Expression>;

enum class ExprKind : uint8_t {
    kLiteral,
    kColumnRef,
    kBinary,
    kAggregate,
    kStar,       // only legal in SELECT * and COUNT(*)
};

enum class BinaryOp : uint8_t {
    // comparisons
    kEq, kNe, kLt, kLe, kGt, kGe,
    // logical
    kAnd, kOr,
    // arithmetic, supported in projections only
    kAdd, kSub, kMul, kDiv,
};

const char* BinaryOpName(BinaryOp op);

// IsComparison and AsCompareOp let the optimizer recognize the subset of binary
// operators that can become a pushable Predicate.
bool IsComparison(BinaryOp op);
CompareOp AsCompareOp(BinaryOp op);

struct Expression {
    ExprKind kind;

    // kLiteral
    Value literal;

    // kColumnRef: the name as written, lowercased by the lexer.
    std::string column_name;

    // kBinary
    BinaryOp op = BinaryOp::kEq;
    ExprPtr left;
    ExprPtr right;

    // kAggregate
    AggregateKind agg = AggregateKind::kCount;
    ExprPtr agg_argument;  // null for COUNT(*)

    // alias is the AS name in a projection, empty if absent.
    std::string alias;

    std::string ToString() const;

    static ExprPtr Literal(Value v);
    static ExprPtr ColumnRef(std::string name);
    static ExprPtr Binary(BinaryOp op, ExprPtr l, ExprPtr r);
    static ExprPtr Aggregate(AggregateKind k, ExprPtr arg);
    static ExprPtr Star();
};

struct OrderByItem {
    ExprPtr expr;
    bool descending = false;
};

// JoinClause covers the one join form the engine supports: a single-column equi
// inner join. Narrow on purpose — a general join predicate would mean a general
// expression evaluator in the join operator, and the point of the join here is to
// exercise hash build and probe, not to be complete.
struct JoinClause {
    std::string table;
    std::string left_column;   // qualified against the outer table
    std::string right_column;  // qualified against `table`
};

struct SelectStatement {
    std::vector<ExprPtr> projections;   // a single kStar means SELECT *
    std::string from_table;
    std::optional<JoinClause> join;
    ExprPtr where;                      // null if absent
    std::vector<ExprPtr> group_by;
    std::vector<OrderByItem> order_by;
    std::optional<uint64_t> limit;

    std::string ToString() const;
};

struct CreateTableStatement {
    std::string table;
    Schema schema;
};

struct InsertStatement {
    std::string table;
    // Explicit column list, empty when the statement omits it and the values are
    // positional against the table's declared order.
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> rows;
};

struct CreateIndexStatement {
    std::string name;
    std::string table;
    std::string column;
    // Which structure backs it. Exposed in SQL so the B+ tree and the LSM can be
    // compared without recompiling.
    bool lsm = false;
};

enum class StatementKind : uint8_t {
    kSelect,
    kCreateTable,
    kInsert,
    kCreateIndex,
    kExplain,
};

struct Statement {
    StatementKind kind;

    std::unique_ptr<SelectStatement> select;
    std::unique_ptr<CreateTableStatement> create_table;
    std::unique_ptr<InsertStatement> insert;
    std::unique_ptr<CreateIndexStatement> create_index;

    // EXPLAIN wraps a select. analyze additionally runs it and reports per-operator
    // counters rather than only the plan shape.
    std::unique_ptr<SelectStatement> explain_select;
    bool analyze = false;

    std::string ToString() const;
};

}  // namespace quarry::ast
