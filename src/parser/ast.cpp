// Implementation of the AST declared in include/quarry/ast.hpp.
//
// It lives under src/parser/ rather than src/core/ because the AST is the
// parser's output type and nothing else constructs one: the binder only reads it.

#include "quarry/ast.hpp"

#include <string>

namespace quarry::ast {
namespace {

// Aggregate spelling is duplicated here rather than calling AggregateKindName
// (declared in quarry/operator.hpp, owned by the execution layer) so that the
// parser links on its own. Source of truth for the set itself is
// AggregateKind in include/quarry/operator.hpp.
const char* AggName(AggregateKind k) {
    switch (k) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum:   return "SUM";
        case AggregateKind::kAvg:   return "AVG";
        case AggregateKind::kMin:   return "MIN";
        case AggregateKind::kMax:   return "MAX";
    }
    return "?";
}

std::string LiteralText(const Value& v) {
    if (v.is_null()) return "NULL";
    if (v.type() == TypeId::kVarchar) return "'" + v.as_varchar() + "'";
    return v.ToString();
}

}  // namespace

const char* BinaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::kEq:  return "=";
        case BinaryOp::kNe:  return "<>";
        case BinaryOp::kLt:  return "<";
        case BinaryOp::kLe:  return "<=";
        case BinaryOp::kGt:  return ">";
        case BinaryOp::kGe:  return ">=";
        case BinaryOp::kAnd: return "AND";
        case BinaryOp::kOr:  return "OR";
        case BinaryOp::kAdd: return "+";
        case BinaryOp::kSub: return "-";
        case BinaryOp::kMul: return "*";
        case BinaryOp::kDiv: return "/";
    }
    return "?";
}

bool IsComparison(BinaryOp op) {
    switch (op) {
        case BinaryOp::kEq:
        case BinaryOp::kNe:
        case BinaryOp::kLt:
        case BinaryOp::kLe:
        case BinaryOp::kGt:
        case BinaryOp::kGe:
            return true;
        default:
            return false;
    }
}

CompareOp AsCompareOp(BinaryOp op) {
    switch (op) {
        case BinaryOp::kNe: return CompareOp::kNe;
        case BinaryOp::kLt: return CompareOp::kLt;
        case BinaryOp::kLe: return CompareOp::kLe;
        case BinaryOp::kGt: return CompareOp::kGt;
        case BinaryOp::kGe: return CompareOp::kGe;
        default:            return CompareOp::kEq;
    }
}

ExprPtr Expression::Literal(Value v) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::kLiteral;
    e->literal = std::move(v);
    return e;
}

ExprPtr Expression::ColumnRef(std::string name) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::kColumnRef;
    e->column_name = std::move(name);
    return e;
}

ExprPtr Expression::Binary(BinaryOp op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::kBinary;
    e->op = op;
    e->left = std::move(l);
    e->right = std::move(r);
    return e;
}

ExprPtr Expression::Aggregate(AggregateKind k, ExprPtr arg) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::kAggregate;
    e->agg = k;
    e->agg_argument = std::move(arg);
    return e;
}

ExprPtr Expression::Star() {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::kStar;
    return e;
}

std::string Expression::ToString() const {
    std::string out;
    switch (kind) {
        case ExprKind::kLiteral:
            out = LiteralText(literal);
            break;
        case ExprKind::kColumnRef:
            out = column_name;
            break;
        case ExprKind::kBinary:
            // Always parenthesized: the point of printing an expression is to show
            // how it was grouped, and precedence-aware printing hides exactly the
            // mistake the output is being read to find.
            out = "(" + (left ? left->ToString() : "?") + " " + BinaryOpName(op) + " " +
                  (right ? right->ToString() : "?") + ")";
            break;
        case ExprKind::kAggregate:
            out = std::string(AggName(agg)) + "(" +
                  (agg_argument ? agg_argument->ToString() : "*") + ")";
            break;
        case ExprKind::kStar:
            out = "*";
            break;
    }
    if (!alias.empty()) out += " AS " + alias;
    return out;
}

std::string SelectStatement::ToString() const {
    std::string out = "SELECT ";
    for (size_t i = 0; i < projections.size(); ++i) {
        if (i > 0) out += ", ";
        out += projections[i] ? projections[i]->ToString() : "?";
    }
    out += " FROM " + from_table;
    if (join.has_value()) {
        out += " INNER JOIN " + join->table + " ON " + join->left_column + " = " +
               join->right_column;
    }
    if (where) out += " WHERE " + where->ToString();
    if (!group_by.empty()) {
        out += " GROUP BY ";
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) out += ", ";
            out += group_by[i]->ToString();
        }
    }
    if (!order_by.empty()) {
        out += " ORDER BY ";
        for (size_t i = 0; i < order_by.size(); ++i) {
            if (i > 0) out += ", ";
            out += order_by[i].expr->ToString();
            out += order_by[i].descending ? " DESC" : " ASC";
        }
    }
    if (limit.has_value()) out += " LIMIT " + std::to_string(*limit);
    return out;
}

std::string Statement::ToString() const {
    switch (kind) {
        case StatementKind::kSelect:
            return select ? select->ToString() : "SELECT ?";
        case StatementKind::kCreateTable:
            return create_table ? "CREATE TABLE " + create_table->table + " " +
                                      create_table->schema.ToString()
                                : "CREATE TABLE ?";
        case StatementKind::kInsert: {
            if (!insert) return "INSERT ?";
            std::string out = "INSERT INTO " + insert->table;
            if (!insert->columns.empty()) {
                out += " (";
                for (size_t i = 0; i < insert->columns.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += insert->columns[i];
                }
                out += ")";
            }
            out += " VALUES ";
            for (size_t r = 0; r < insert->rows.size(); ++r) {
                if (r > 0) out += ", ";
                out += "(";
                for (size_t c = 0; c < insert->rows[r].size(); ++c) {
                    if (c > 0) out += ", ";
                    out += insert->rows[r][c]->ToString();
                }
                out += ")";
            }
            return out;
        }
        case StatementKind::kCreateIndex:
            if (!create_index) return "CREATE INDEX ?";
            return "CREATE INDEX " + create_index->name + " ON " + create_index->table +
                   " (" + create_index->column + ")" +
                   (create_index->lsm ? " USING LSM" : "");
        case StatementKind::kExplain:
            return std::string("EXPLAIN ") + (analyze ? "ANALYZE " : "") +
                   (explain_select ? explain_select->ToString() : "?");
    }
    return "?";
}

}  // namespace quarry::ast
