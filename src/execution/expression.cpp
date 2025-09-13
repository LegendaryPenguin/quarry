#include "expression.hpp"

#include <cstdint>

#include "exec_util.hpp"

namespace quarry {
namespace {

using ast::BinaryOp;

bool IsLogical(BinaryOp op) { return op == BinaryOp::kAnd || op == BinaryOp::kOr; }

bool IsArithmetic(BinaryOp op) {
    switch (op) {
        case BinaryOp::kAdd:
        case BinaryOp::kSub:
        case BinaryOp::kMul:
        case BinaryOp::kDiv:
            return true;
        default:
            return false;
    }
}

bool IsNullLiteral(const ast::Expression& e) {
    return e.kind == ast::ExprKind::kLiteral && e.literal.is_null();
}

std::unique_ptr<Vector> NewVector(TypeId type, size_t n) {
    auto v = std::make_unique<Vector>(type);
    v->Reserve(n);
    return v;
}

// AllNull is the answer whenever an untyped NULL participates: the comparison or
// the arithmetic is unknown for every row, and no kernel needs to run.
std::unique_ptr<Vector> AllNull(TypeId type, size_t n) {
    auto v = NewVector(type, n);
    for (size_t i = 0; i < n; ++i) v->AppendNull();
    return v;
}

std::unique_ptr<Vector> MakeConstant(TypeId type, const Value& value, size_t n) {
    if (value.is_null()) return AllNull(type, n);
    auto v = NewVector(type, n);
    for (size_t i = 0; i < n; ++i) {
        switch (type) {
            case TypeId::kInt64:
                v->AppendInt64(value.type() == TypeId::kDouble
                                   ? static_cast<int64_t>(value.as_double())
                                   : value.as_int64());
                break;
            case TypeId::kDouble:  v->AppendDouble(value.AsNumeric()); break;
            case TypeId::kBoolean: v->AppendBoolean(value.as_boolean()); break;
            case TypeId::kVarchar: v->AppendString(value.as_varchar()); break;
            default:               v->AppendNull(); break;
        }
    }
    return v;
}

// PromoteToDouble widens an int64 vector so that a mixed int/double comparison
// becomes one tight double loop instead of a per-row type branch. One extra pass
// over 2048 values is cheaper than 2048 unpredictable branches.
std::unique_ptr<Vector> PromoteToDouble(const Vector& src) {
    const size_t n = src.size();
    auto out = NewVector(TypeId::kDouble, n);
    for (size_t i = 0; i < n; ++i) {
        if (src.nulls().IsNull(i)) {
            out->AppendNull();
        } else {
            out->AppendDouble(static_cast<double>(src.int64_data()[i]));
        }
    }
    return out;
}

bool Satisfies(int order, CompareOp op) {
    switch (op) {
        case CompareOp::kEq: return order == 0;
        case CompareOp::kNe: return order != 0;
        case CompareOp::kLt: return order < 0;
        case CompareOp::kLe: return order <= 0;
        case CompareOp::kGt: return order > 0;
        case CompareOp::kGe: return order >= 0;
    }
    return false;
}

template <typename T>
int Order(T a, T b) {
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

// CompareTyped is the comparison kernel: one loop, one type, no tree walking.
// Reader is a callable (const Vector&, size_t) -> T so the same loop serves
// packed arrays and string views.
template <typename Reader>
void CompareTyped(const Vector& l, const Vector& r, CompareOp op, Vector& out, Reader read) {
    const size_t n = l.size();
    if (!l.nulls().has_nulls() && !r.nulls().has_nulls()) {
        // The whole reason NullMask carries has_nulls(): a column declared NOT
        // NULL, or simply holding no nulls, pays nothing for null checking.
        for (size_t i = 0; i < n; ++i) {
            out.AppendBoolean(Satisfies(Order(read(l, i), read(r, i)), op));
        }
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (l.nulls().IsNull(i) || r.nulls().IsNull(i)) {
            out.AppendNull();
        } else {
            out.AppendBoolean(Satisfies(Order(read(l, i), read(r, i)), op));
        }
    }
}

Status CompareVectors(const Vector& l, const Vector& r, CompareOp op, Vector& out) {
    if (l.type() == TypeId::kInt64 && r.type() == TypeId::kInt64) {
        CompareTyped(l, r, op, out,
                     [](const Vector& v, size_t i) { return v.int64_data()[i]; });
        return Status::Ok();
    }
    const bool l_num = l.type() == TypeId::kInt64 || l.type() == TypeId::kDouble;
    const bool r_num = r.type() == TypeId::kInt64 || r.type() == TypeId::kDouble;
    if (l_num && r_num) {
        std::unique_ptr<Vector> lp, rp;
        if (l.type() == TypeId::kInt64) lp = PromoteToDouble(l);
        if (r.type() == TypeId::kInt64) rp = PromoteToDouble(r);
        CompareTyped(lp ? *lp : l, rp ? *rp : r, op, out,
                     [](const Vector& v, size_t i) { return v.double_data()[i]; });
        return Status::Ok();
    }
    if (l.type() == TypeId::kBoolean && r.type() == TypeId::kBoolean) {
        CompareTyped(l, r, op, out, [](const Vector& v, size_t i) {
            return static_cast<int>(v.bool_data()[i] != 0);
        });
        return Status::Ok();
    }
    if (l.type() == TypeId::kVarchar && r.type() == TypeId::kVarchar) {
        CompareTyped(l, r, op, out,
                     [](const Vector& v, size_t i) { return v.GetString(i); });
        return Status::Ok();
    }
    return Status::TypeError(std::string("cannot compare ") + TypeName(l.type()) + " with " +
                            TypeName(r.type()));
}

// Integer arithmetic wraps rather than trapping.
//
// Signed overflow is undefined behaviour in C++, and a query engine must not have
// undefined behaviour reachable from user data, so the arithmetic goes through
// unsigned where wrapping is defined. The documented consequence is that SUM and
// `a * b` wrap on overflow instead of erroring.
int64_t WrapAdd(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}
int64_t WrapSub(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}
int64_t WrapMul(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

void ArithInt64(const Vector& l, const Vector& r, BinaryOp op, Vector& out) {
    const size_t n = l.size();
    const bool check_nulls = l.nulls().has_nulls() || r.nulls().has_nulls();
    for (size_t i = 0; i < n; ++i) {
        if (check_nulls && (l.nulls().IsNull(i) || r.nulls().IsNull(i))) {
            out.AppendNull();
            continue;
        }
        const int64_t a = l.int64_data()[i];
        const int64_t b = r.int64_data()[i];
        switch (op) {
            case BinaryOp::kAdd: out.AppendInt64(WrapAdd(a, b)); break;
            case BinaryOp::kSub: out.AppendInt64(WrapSub(a, b)); break;
            case BinaryOp::kMul: out.AppendInt64(WrapMul(a, b)); break;
            default:
                // Integer division by zero yields NULL rather than an error.
                // Erroring would abort a whole query because one row in a million
                // had a zero divisor, and returning a sentinel would be a value
                // the user cannot distinguish from data. NULL already means "no
                // answer here". INT64_MIN / -1 is grouped with it because it is
                // the one division whose true result is unrepresentable.
                if (b == 0 || (a == INT64_MIN && b == -1)) {
                    out.AppendNull();
                } else {
                    // Truncation toward zero, as C++ and SQL integer division both
                    // specify: -7 / 2 is -3, not -4.
                    out.AppendInt64(a / b);
                }
                break;
        }
    }
}

void ArithDouble(const Vector& l, const Vector& r, BinaryOp op, Vector& out) {
    const size_t n = l.size();
    const bool check_nulls = l.nulls().has_nulls() || r.nulls().has_nulls();
    for (size_t i = 0; i < n; ++i) {
        if (check_nulls && (l.nulls().IsNull(i) || r.nulls().IsNull(i))) {
            out.AppendNull();
            continue;
        }
        const double a = l.double_data()[i];
        const double b = r.double_data()[i];
        switch (op) {
            case BinaryOp::kAdd: out.AppendDouble(a + b); break;
            case BinaryOp::kSub: out.AppendDouble(a - b); break;
            case BinaryOp::kMul: out.AppendDouble(a * b); break;
            default:
                // Float division by zero is NULL too, deliberately matching the
                // integer rule. An infinity propagating into a SUM turns one bad
                // row into one bad query result, which is harder to notice.
                if (b == 0.0) {
                    out.AppendNull();
                } else {
                    out.AppendDouble(a / b);
                }
                break;
        }
    }
}

// ThreeValued implements SQL's AND/OR over a pair of boolean vectors.
//
// The asymmetry is the whole subtlety: FALSE AND NULL is FALSE, because no value
// for the unknown side could make the conjunction true, while TRUE AND NULL is
// NULL. OR mirrors it.
void ThreeValued(const Vector& l, const Vector& r, BinaryOp op, Vector& out) {
    const size_t n = l.size();
    for (size_t i = 0; i < n; ++i) {
        const bool ln = l.nulls().IsNull(i);
        const bool rn = r.nulls().IsNull(i);
        const bool lv = !ln && l.bool_data()[i] != 0;
        const bool rv = !rn && r.bool_data()[i] != 0;

        if (op == BinaryOp::kAnd) {
            if ((!ln && !lv) || (!rn && !rv)) {
                out.AppendBoolean(false);
            } else if (ln || rn) {
                out.AppendNull();
            } else {
                out.AppendBoolean(true);
            }
        } else {
            if (lv || rv) {
                out.AppendBoolean(true);
            } else if (ln || rn) {
                out.AppendNull();
            } else {
                out.AppendBoolean(false);
            }
        }
    }
}

}  // namespace

Result<TypeId> ExpressionEvaluator::ResultType(const ast::Expression& expr) const {
    switch (expr.kind) {
        case ast::ExprKind::kLiteral:
            return expr.literal.type();  // kInvalid for the untyped NULL literal

        case ast::ExprKind::kColumnRef: {
            const auto idx = input_.IndexOf(expr.column_name);
            if (!idx) {
                return Status::BindError("unknown column '" + expr.column_name + "' in " +
                                         input_.ToString());
            }
            return input_.at(*idx).type;
        }

        case ast::ExprKind::kBinary: {
            if (!expr.left || !expr.right) {
                return Status::Internal("binary expression is missing an operand");
            }
            if (ast::IsComparison(expr.op) || IsLogical(expr.op)) {
                // The children are still type checked here, so `a + 'x' > 1` is
                // rejected before any kernel runs rather than part way through a
                // batch.
                auto lt = ResultType(*expr.left);
                if (!lt.ok()) return lt.status();
                auto rt = ResultType(*expr.right);
                if (!rt.ok()) return rt.status();
                return TypeId::kBoolean;
            }
            if (!IsArithmetic(expr.op)) {
                return Status::NotImplemented(std::string("binary operator ") +
                                              ast::BinaryOpName(expr.op));
            }
            auto lt = ResultType(*expr.left);
            if (!lt.ok()) return lt.status();
            auto rt = ResultType(*expr.right);
            if (!rt.ok()) return rt.status();

            const TypeId l = lt.value();
            const TypeId r = rt.value();
            // An untyped NULL adopts the other side's type; NULL + NULL is an
            // int64 column of nulls, which is as good a choice as any.
            if (l == TypeId::kInvalid && r == TypeId::kInvalid) return TypeId::kInt64;
            if (l == TypeId::kInvalid) return r == TypeId::kDouble ? TypeId::kDouble : r;
            if (r == TypeId::kInvalid) return l == TypeId::kDouble ? TypeId::kDouble : l;
            if (l == TypeId::kDouble || r == TypeId::kDouble) {
                if ((l != TypeId::kDouble && l != TypeId::kInt64) ||
                    (r != TypeId::kDouble && r != TypeId::kInt64)) {
                    return Status::TypeError(std::string("arithmetic on ") + TypeName(l) + " and " +
                                             TypeName(r));
                }
                return TypeId::kDouble;
            }
            if (l == TypeId::kInt64 && r == TypeId::kInt64) return TypeId::kInt64;
            return Status::TypeError(std::string("arithmetic on ") + TypeName(l) + " and " +
                                     TypeName(r));
        }

        case ast::ExprKind::kAggregate:
            return Status::Internal(
                "an aggregate cannot be evaluated per row; HashAggregateOperator owns it");

        case ast::ExprKind::kStar:
            return Status::Internal("'*' must be expanded by the binder before execution");
    }
    return Status::Internal("unknown expression kind");
}

Result<EvalResult> ExpressionEvaluator::Evaluate(const ast::Expression& expr,
                                                 const Batch& batch) const {
    if (batch.column_count() != input_.size()) {
        return Status::Internal("batch has " + std::to_string(batch.column_count()) +
                                " columns, evaluator was configured for " +
                                std::to_string(input_.size()));
    }
    const std::vector<uint32_t>& rows = batch.Rows();
    const size_t n = rows.size();

    switch (expr.kind) {
        case ast::ExprKind::kLiteral: {
            const TypeId t = expr.literal.is_null() ? TypeId::kInt64 : expr.literal.type();
            return EvalResult::Owned(MakeConstant(t, expr.literal, n));
        }

        case ast::ExprKind::kColumnRef: {
            const auto idx = input_.IndexOf(expr.column_name);
            if (!idx) {
                return Status::BindError("unknown column '" + expr.column_name + "' in " +
                                         input_.ToString());
            }
            const Vector& src = batch.column(*idx);
            if (!batch.has_selection()) {
                // No filter has run, so the column is already compacted and can be
                // handed out as-is.
                return EvalResult::Borrowed(src);
            }
            auto out = NewVector(src.type(), n);
            for (uint32_t row : rows) {
                QUARRY_TRY(CopyRow(*out, src, row));
            }
            return EvalResult::Owned(std::move(out));
        }

        case ast::ExprKind::kBinary: {
            if (!expr.left || !expr.right) {
                return Status::Internal("binary expression is missing an operand");
            }
            auto type = ResultType(expr);
            if (!type.ok()) return type.status();

            if (ast::IsComparison(expr.op)) {
                if (IsNullLiteral(*expr.left) || IsNullLiteral(*expr.right)) {
                    return EvalResult::Owned(AllNull(TypeId::kBoolean, n));
                }
                auto l = Evaluate(*expr.left, batch);
                if (!l.ok()) return l.status();
                auto r = Evaluate(*expr.right, batch);
                if (!r.ok()) return r.status();
                auto out = NewVector(TypeId::kBoolean, n);
                QUARRY_TRY(CompareVectors(l.value().vector(), r.value().vector(),
                                          ast::AsCompareOp(expr.op), *out));
                return EvalResult::Owned(std::move(out));
            }

            if (IsLogical(expr.op)) {
                auto l = Evaluate(*expr.left, batch);
                if (!l.ok()) return l.status();
                auto r = Evaluate(*expr.right, batch);
                if (!r.ok()) return r.status();

                // A bare NULL operand arrives typed as int64 all-nulls; rewriting
                // it to a boolean all-null keeps the kernel single-typed.
                std::unique_ptr<Vector> l_fix, r_fix;
                if (l.value().type() != TypeId::kBoolean) {
                    if (!IsNullLiteral(*expr.left)) {
                        return Status::TypeError(std::string("AND/OR needs BOOLEAN operands, got ") +
                                                 TypeName(l.value().type()));
                    }
                    l_fix = AllNull(TypeId::kBoolean, n);
                }
                if (r.value().type() != TypeId::kBoolean) {
                    if (!IsNullLiteral(*expr.right)) {
                        return Status::TypeError(std::string("AND/OR needs BOOLEAN operands, got ") +
                                                 TypeName(r.value().type()));
                    }
                    r_fix = AllNull(TypeId::kBoolean, n);
                }
                auto out = NewVector(TypeId::kBoolean, n);
                ThreeValued(l_fix ? *l_fix : l.value().vector(),
                            r_fix ? *r_fix : r.value().vector(), expr.op, *out);
                return EvalResult::Owned(std::move(out));
            }

            // Arithmetic.
            if (IsNullLiteral(*expr.left) || IsNullLiteral(*expr.right)) {
                return EvalResult::Owned(AllNull(type.value(), n));
            }
            auto l = Evaluate(*expr.left, batch);
            if (!l.ok()) return l.status();
            auto r = Evaluate(*expr.right, batch);
            if (!r.ok()) return r.status();

            auto out = NewVector(type.value(), n);
            if (type.value() == TypeId::kInt64) {
                ArithInt64(l.value().vector(), r.value().vector(), expr.op, *out);
            } else {
                std::unique_ptr<Vector> lp, rp;
                if (l.value().type() == TypeId::kInt64) lp = PromoteToDouble(l.value().vector());
                if (r.value().type() == TypeId::kInt64) rp = PromoteToDouble(r.value().vector());
                ArithDouble(lp ? *lp : l.value().vector(), rp ? *rp : r.value().vector(), expr.op,
                            *out);
            }
            return EvalResult::Owned(std::move(out));
        }

        case ast::ExprKind::kAggregate:
        case ast::ExprKind::kStar:
            return ResultType(expr).status();
    }
    return Status::Internal("unknown expression kind");
}

Result<std::vector<uint32_t>> ExpressionEvaluator::Select(const ast::Expression& expr,
                                                         const Batch& batch) const {
    auto value = Evaluate(expr, batch);
    if (!value.ok()) return value.status();

    const Vector& v = value.value().vector();
    if (v.type() != TypeId::kBoolean) {
        return Status::TypeError(std::string("a filter needs a BOOLEAN expression, got ") +
                                 TypeName(v.type()));
    }

    const std::vector<uint32_t>& rows = batch.Rows();
    std::vector<uint32_t> out;
    out.reserve(rows.size());
    const bool check_nulls = v.nulls().has_nulls();
    for (size_t i = 0; i < rows.size(); ++i) {
        // UNKNOWN is not TRUE: a row whose predicate is null does not pass.
        if (check_nulls && v.nulls().IsNull(i)) continue;
        if (v.bool_data()[i] != 0) out.push_back(rows[i]);
    }
    return out;
}

std::string ExpressionEvaluator::OutputName(const ast::Expression& expr) {
    if (!expr.alias.empty()) return expr.alias;
    if (expr.kind == ast::ExprKind::kColumnRef) return expr.column_name;
    return expr.ToString();
}

Status ApplyPredicate(Batch& batch, const Predicate& pred) {
    if (pred.column_index >= batch.column_count()) {
        return Status::Internal("pushed predicate names column " +
                                std::to_string(pred.column_index) + " but the batch has " +
                                std::to_string(batch.column_count()));
    }
    const Vector& col = batch.column(pred.column_index);
    const std::vector<uint32_t>& rows = batch.Rows();

    std::vector<uint32_t> keep;
    keep.reserve(rows.size());
    for (uint32_t row : rows) {
        bool unknown = false;
        if (EvalCompare(col, row, pred.op, pred.constant, unknown) && !unknown) {
            keep.push_back(row);
        }
    }
    batch.SetSelection(std::move(keep));
    return Status::Ok();
}

}  // namespace quarry
