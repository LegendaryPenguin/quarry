#include "quarry/types.hpp"

#include "quarry/status.hpp"

#include <cmath>
#include <sstream>

namespace quarry {

const char* TypeName(TypeId id) {
    switch (id) {
        case TypeId::kBoolean: return "BOOLEAN";
        case TypeId::kInt64:   return "BIGINT";
        case TypeId::kDouble:  return "DOUBLE";
        case TypeId::kVarchar: return "VARCHAR";
        default:               return "INVALID";
    }
}

TypeId Value::type() const {
    if (std::holds_alternative<bool>(data_))        return TypeId::kBoolean;
    if (std::holds_alternative<int64_t>(data_))     return TypeId::kInt64;
    if (std::holds_alternative<double>(data_))      return TypeId::kDouble;
    if (std::holds_alternative<std::string>(data_)) return TypeId::kVarchar;
    return TypeId::kInvalid;
}

bool Value::IsNumeric() const {
    return std::holds_alternative<int64_t>(data_) || std::holds_alternative<double>(data_);
}

double Value::AsNumeric() const {
    if (std::holds_alternative<int64_t>(data_)) {
        return static_cast<double>(std::get<int64_t>(data_));
    }
    return std::get<double>(data_);
}

std::string Value::ToString() const {
    if (is_null()) return "NULL";
    switch (type()) {
        case TypeId::kBoolean: return as_boolean() ? "true" : "false";
        case TypeId::kInt64:   return std::to_string(as_int64());
        case TypeId::kDouble: {
            // Fixed precision rather than the shortest round-trip form, so two
            // engines compared on the same query produce comparable text. The
            // differential tests against SQLite depend on this agreeing.
            std::ostringstream os;
            os.precision(6);
            os << std::fixed << as_double();
            return os.str();
        }
        case TypeId::kVarchar: return as_varchar();
        default:               return "?";
    }
}

bool Value::operator==(const Value& other) const {
    // NULL is not equal to anything, including another NULL. Returning false for
    // both directions is what keeps a GROUP BY from merging null groups by
    // accident; the aggregate operator handles null grouping explicitly instead.
    if (is_null() || other.is_null()) return false;

    // An int64 column compared against a double literal is the one implicit
    // conversion allowed, so that `WHERE price > 500` works whether price is
    // integral or not.
    if (IsNumeric() && other.IsNumeric() && type() != other.type()) {
        return AsNumeric() == other.AsNumeric();
    }
    if (type() != other.type()) return false;

    switch (type()) {
        case TypeId::kBoolean: return as_boolean() == other.as_boolean();
        case TypeId::kInt64:   return as_int64() == other.as_int64();
        case TypeId::kDouble:  return as_double() == other.as_double();
        case TypeId::kVarchar: return as_varchar() == other.as_varchar();
        default:               return false;
    }
}

bool Value::operator<(const Value& other) const {
    if (is_null() || other.is_null()) return false;

    if (IsNumeric() && other.IsNumeric() && type() != other.type()) {
        return AsNumeric() < other.AsNumeric();
    }
    if (type() != other.type()) return false;

    switch (type()) {
        case TypeId::kBoolean: return !as_boolean() && other.as_boolean();
        case TypeId::kInt64:   return as_int64() < other.as_int64();
        case TypeId::kDouble:  return as_double() < other.as_double();
        case TypeId::kVarchar: return as_varchar() < other.as_varchar();
        default:               return false;
    }
}

const char* CompareOpName(CompareOp op) {
    switch (op) {
        case CompareOp::kEq: return "=";
        case CompareOp::kNe: return "<>";
        case CompareOp::kLt: return "<";
        case CompareOp::kLe: return "<=";
        case CompareOp::kGt: return ">";
        case CompareOp::kGe: return ">=";
    }
    return "?";
}

CompareOp Flip(CompareOp op) {
    // Used when the optimizer rewrites `500 < price` into `price > 500` so the
    // predicate reaches the canonical column-op-constant shape that scans and
    // zone maps can use.
    switch (op) {
        case CompareOp::kLt: return CompareOp::kGt;
        case CompareOp::kLe: return CompareOp::kGe;
        case CompareOp::kGt: return CompareOp::kLt;
        case CompareOp::kGe: return CompareOp::kLe;
        default:             return op;  // = and <> are symmetric
    }
}

std::string Status::ToString() const {
    if (ok()) return "OK";
    const char* label = "error";
    switch (code_) {
        case Code::kSyntaxError:     label = "syntax error"; break;
        case Code::kBindError:       label = "bind error"; break;
        case Code::kTypeError:       label = "type error"; break;
        case Code::kNotImplemented:  label = "not implemented"; break;
        case Code::kIoError:         label = "io error"; break;
        case Code::kCorruption:      label = "corruption"; break;
        case Code::kInvalidArgument: label = "invalid argument"; break;
        case Code::kInternal:        label = "internal error"; break;
        default: break;
    }
    return std::string(label) + ": " + message_;
}

}  // namespace quarry
