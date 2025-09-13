#include "exec_util.hpp"

#include <cstring>
#include <optional>

namespace quarry {
namespace {

template <typename T>
int Order(T a, T b) {
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

bool OrderSatisfies(int order, CompareOp op) {
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

// OrderAgainstConstant returns nullopt when the two sides are not comparable at
// all, which is distinct from "unknown because of NULL": an incomparable pair
// cannot satisfy any operator, whereas NULL poisons the whole expression.
std::optional<int> OrderAgainstConstant(const Vector& vec, size_t row, const Value& c) {
    switch (vec.type()) {
        case TypeId::kInt64: {
            const int64_t v = vec.int64_data()[row];
            if (c.type() == TypeId::kInt64) return Order(v, c.as_int64());
            if (c.type() == TypeId::kDouble) return Order(static_cast<double>(v), c.as_double());
            return std::nullopt;
        }
        case TypeId::kDouble: {
            const double v = vec.double_data()[row];
            if (!c.IsNumeric()) return std::nullopt;
            return Order(v, c.AsNumeric());
        }
        case TypeId::kBoolean: {
            if (c.type() != TypeId::kBoolean) return std::nullopt;
            return Order<int>(vec.bool_data()[row] != 0, c.as_boolean() ? 1 : 0);
        }
        case TypeId::kVarchar: {
            if (c.type() != TypeId::kVarchar) return std::nullopt;
            const int cmp = vec.GetString(row).compare(c.as_varchar());
            return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
        }
        default:
            return std::nullopt;
    }
}

}  // namespace

Status PrepareOutput(Batch& out, const Schema& schema) {
    if (out.column_count() == 0 && schema.size() > 0) {
        for (const auto& col : schema.columns()) out.AddColumn(col);
    } else if (out.column_count() != schema.size()) {
        return Status::Internal("output batch has " + std::to_string(out.column_count()) +
                                " columns, operator produces " + std::to_string(schema.size()));
    }
    out.Reset();
    return Status::Ok();
}

Status CopyRow(Vector& dst, const Vector& src, size_t row) {
    if (dst.type() != src.type()) {
        return Status::Internal(std::string("cannot copy a ") + TypeName(src.type()) +
                                " value into a " + TypeName(dst.type()) + " column");
    }
    if (src.nulls().IsNull(row)) {
        dst.AppendNull();
        return Status::Ok();
    }
    switch (src.type()) {
        case TypeId::kInt64:   dst.AppendInt64(src.int64_data()[row]); break;
        case TypeId::kDouble:  dst.AppendDouble(src.double_data()[row]); break;
        case TypeId::kBoolean: dst.AppendBoolean(src.bool_data()[row] != 0); break;
        case TypeId::kVarchar: dst.AppendString(src.GetString(row)); break;
        default: return Status::Internal("cannot copy a value of invalid type");
    }
    return Status::Ok();
}

Status AppendValue(Vector& dst, const Value& value) {
    if (value.is_null()) {
        dst.AppendNull();
        return Status::Ok();
    }
    switch (dst.type()) {
        case TypeId::kInt64:
            if (!value.IsNumeric()) break;
            dst.AppendInt64(value.type() == TypeId::kInt64
                                ? value.as_int64()
                                : static_cast<int64_t>(value.as_double()));
            return Status::Ok();
        case TypeId::kDouble:
            if (!value.IsNumeric()) break;
            dst.AppendDouble(value.AsNumeric());
            return Status::Ok();
        case TypeId::kBoolean:
            if (value.type() != TypeId::kBoolean) break;
            dst.AppendBoolean(value.as_boolean());
            return Status::Ok();
        case TypeId::kVarchar:
            if (value.type() != TypeId::kVarchar) break;
            dst.AppendString(value.as_varchar());
            return Status::Ok();
        default:
            break;
    }
    return Status::Internal(std::string("cannot store a ") + TypeName(value.type()) +
                            " value in a " + TypeName(dst.type()) + " column");
}

void EncodeKeyInto(std::string& key, const Vector& vec, size_t row, bool promote_numeric) {
    if (vec.nulls().IsNull(row)) {
        // The null marker carries no payload, so every NULL in a column encodes
        // identically and therefore lands in one group.
        key.push_back('\0');
        return;
    }
    key.push_back('\1');

    const auto append_bytes = [&key](const void* p, size_t n) {
        key.append(static_cast<const char*>(p), n);
    };

    switch (vec.type()) {
        case TypeId::kInt64: {
            const int64_t v = vec.int64_data()[row];
            if (promote_numeric) {
                const double d = static_cast<double>(v);
                append_bytes(&d, sizeof(d));
            } else {
                append_bytes(&v, sizeof(v));
            }
            break;
        }
        case TypeId::kDouble: {
            double d = vec.double_data()[row];
            // -0.0 == 0.0 is true, so the two must encode identically or a GROUP BY
            // on a double column would split them into separate groups. Assigning
            // the positive literal replaces a negative zero's sign bit.
            if (d == 0.0) d = 0.0;
            append_bytes(&d, sizeof(d));
            break;
        }
        case TypeId::kBoolean: {
            key.push_back(vec.bool_data()[row] != 0 ? '\1' : '\0');
            break;
        }
        case TypeId::kVarchar: {
            const std::string_view s = vec.GetString(row);
            const uint32_t len = static_cast<uint32_t>(s.size());
            append_bytes(&len, sizeof(len));
            key.append(s);
            break;
        }
        default:
            break;
    }
}

int CompareRows(const Vector& a, size_t ra, const Vector& b, size_t rb) {
    const bool na = a.nulls().IsNull(ra);
    const bool nb = b.nulls().IsNull(rb);
    if (na || nb) {
        if (na && nb) return 0;
        return na ? -1 : 1;
    }
    switch (a.type()) {
        case TypeId::kInt64:
            if (b.type() == TypeId::kDouble) {
                return Order(static_cast<double>(a.int64_data()[ra]), b.double_data()[rb]);
            }
            return Order(a.int64_data()[ra], b.int64_data()[rb]);
        case TypeId::kDouble:
            if (b.type() == TypeId::kInt64) {
                return Order(a.double_data()[ra], static_cast<double>(b.int64_data()[rb]));
            }
            return Order(a.double_data()[ra], b.double_data()[rb]);
        case TypeId::kBoolean:
            return Order<int>(a.bool_data()[ra] != 0, b.bool_data()[rb] != 0);
        case TypeId::kVarchar: {
            const int c = a.GetString(ra).compare(b.GetString(rb));
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        default:
            return 0;
    }
}

bool EvalCompare(const Vector& vec, size_t row, CompareOp op, const Value& constant,
                 bool& unknown) {
    if (constant.is_null() || vec.nulls().IsNull(row)) {
        unknown = true;
        return false;
    }
    unknown = false;
    const std::optional<int> order = OrderAgainstConstant(vec, row, constant);
    if (!order) return false;
    return OrderSatisfies(*order, op);
}

// ---------------------------------------------------------------------------
// Out-of-line members of the execution contract in quarry/operator.hpp.
//
// They live in src/execution because they describe execution-layer objects, and
// because a Predicate's text and an Aggregate's result type are facts the
// operators themselves have to agree with.
// ---------------------------------------------------------------------------

std::string Predicate::ToString(const Schema& schema) const {
    const std::string name = column_index < schema.size()
                                 ? schema.at(column_index).name
                                 : "col" + std::to_string(column_index);
    return name + " " + CompareOpName(op) + " " + constant.ToString();
}

const char* AggregateKindName(AggregateKind k) {
    switch (k) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum:   return "SUM";
        case AggregateKind::kAvg:   return "AVG";
        case AggregateKind::kMin:   return "MIN";
        case AggregateKind::kMax:   return "MAX";
    }
    return "?";
}

TypeId Aggregate::ResultType(const Schema& input) const {
    switch (kind) {
        case AggregateKind::kCount:
            return TypeId::kInt64;
        case AggregateKind::kAvg:
            return TypeId::kDouble;
        case AggregateKind::kSum:
        case AggregateKind::kMin:
        case AggregateKind::kMax:
            break;
    }
    if (!column_index || *column_index >= input.size()) return TypeId::kInvalid;
    const TypeId in = input.at(*column_index).type;
    if (kind == AggregateKind::kSum) {
        // SUM keeps an integer column integral, so SUM over int64 can overflow.
        // Documented rather than checked: a widening to double would change the
        // answer for large exact integers, which is worse than a wrap nobody hits.
        return in == TypeId::kInt64 ? TypeId::kInt64 : TypeId::kDouble;
    }
    return in;  // MIN/MAX preserve the input type, including varchar
}

}  // namespace quarry
