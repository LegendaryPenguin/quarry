#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace quarry {

// TypeId is the physical type of a column.
//
// The set is deliberately small. Every type added here costs a branch in the
// filter kernels, a case in the aggregate kernels, and an encoding decision in
// the storage layer, so the engine carries four rather than the twenty a real
// SQL dialect needs.
enum class TypeId : uint8_t {
    kInvalid = 0,
    kBoolean,
    kInt64,
    kDouble,
    kVarchar,
};

const char* TypeName(TypeId id);

// FixedWidth reports whether values of this type occupy a constant number of
// bytes, which decides whether a column is stored as a packed array or as an
// offset table into a byte region.
inline bool FixedWidth(TypeId id) { return id != TypeId::kVarchar; }

// TypeSize is the in-memory width of a fixed-width type, in bytes. Zero for
// variable-width types.
inline size_t TypeSize(TypeId id) {
    switch (id) {
        case TypeId::kBoolean: return 1;
        case TypeId::kInt64:   return 8;
        case TypeId::kDouble:  return 8;
        default:               return 0;
    }
}

// Value is a single scalar, used for literals in the parser, for bounds in zone
// maps, and for results at the API edge. It is deliberately NOT what the
// execution engine moves around: a query that filters a million rows must not
// allocate a million of these. See Batch for the representation that matters.
class Value {
public:
    Value() = default;  // SQL NULL

    static Value Boolean(bool v) { return Value(v); }
    static Value Int64(int64_t v) { return Value(v); }
    static Value Double(double v) { return Value(v); }
    static Value Varchar(std::string v) { return Value(std::move(v)); }
    static Value Null() { return {}; }

    bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
    TypeId type() const;

    bool as_boolean() const { return std::get<bool>(data_); }
    int64_t as_int64() const { return std::get<int64_t>(data_); }
    double as_double() const { return std::get<double>(data_); }
    const std::string& as_varchar() const { return std::get<std::string>(data_); }

    // Numeric coercion for comparing an int64 column against a double literal,
    // which is the only implicit conversion the engine performs.
    bool IsNumeric() const;
    double AsNumeric() const;

    std::string ToString() const;

    // Comparison follows SQL semantics for NULL: any comparison involving NULL
    // is unknown rather than true or false, so these return false in both
    // directions and callers must check is_null() when that distinction matters.
    bool operator==(const Value& other) const;
    bool operator<(const Value& other) const;

private:
    template <typename T>
    explicit Value(T v) : data_(std::move(v)) {}

    std::variant<std::monostate, bool, int64_t, double, std::string> data_;
};

// CompareOp is the set of predicates a filter can push into a scan.
enum class CompareOp : uint8_t {
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
};

const char* CompareOpName(CompareOp op);

// Negate returns the operator that is true exactly when op is false, used when
// the optimizer flips a comparison to put the column on the left.
CompareOp Flip(CompareOp op);

}  // namespace quarry
