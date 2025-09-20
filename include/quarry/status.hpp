#pragma once

#include <string>
#include <utility>
#include <variant>

namespace quarry {

// Status carries a failure reason instead of an exception.
//
// Exceptions are avoided on the query path deliberately. A query engine fails
// for ordinary, expected reasons — a column that does not exist, a type that
// cannot be compared, a page that cannot be read — and those are control flow,
// not emergencies. Making them values means every call site that can fail says
// so in its signature, which matters most in the execution operators where a
// missed error turns into a silently wrong answer rather than a crash.
enum class Code {
    kOk = 0,
    kSyntaxError,      // the text is not valid SQL
    kBindError,        // valid SQL that refers to something nonexistent
    kTypeError,        // an operation on incompatible types
    kNotImplemented,   // deliberately outside the supported subset
    kIoError,          // the file system said no
    kCorruption,       // on-disk bytes failed a check
    kInvalidArgument,
    kInternal,         // an invariant this code owns was violated
};

class Status {
public:
    Status() = default;

    static Status Ok() { return {}; }
    static Status SyntaxError(std::string m) { return {Code::kSyntaxError, std::move(m)}; }
    static Status BindError(std::string m) { return {Code::kBindError, std::move(m)}; }
    static Status TypeError(std::string m) { return {Code::kTypeError, std::move(m)}; }
    static Status NotImplemented(std::string m) { return {Code::kNotImplemented, std::move(m)}; }
    static Status IoError(std::string m) { return {Code::kIoError, std::move(m)}; }
    static Status Corruption(std::string m) { return {Code::kCorruption, std::move(m)}; }
    static Status InvalidArgument(std::string m) { return {Code::kInvalidArgument, std::move(m)}; }
    static Status Internal(std::string m) { return {Code::kInternal, std::move(m)}; }

    bool ok() const { return code_ == Code::kOk; }
    Code code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string ToString() const;

private:
    Status(Code c, std::string m) : code_(c), message_(std::move(m)) {}

    Code code_ = Code::kOk;
    std::string message_;
};

// Result<T> is a value or the reason there isn't one.
template <typename T>
class Result {
public:
    Result(T value) : payload_(std::move(value)) {}            // NOLINT: implicit is the point
    Result(Status status) : payload_(std::move(status)) {}      // NOLINT

    bool ok() const { return std::holds_alternative<T>(payload_); }

    const Status& status() const { return std::get<Status>(payload_); }

    T& value() { return std::get<T>(payload_); }
    const T& value() const { return std::get<T>(payload_); }

    T&& take() { return std::move(std::get<T>(payload_)); }

private:
    std::variant<T, Status> payload_;
};

// QUARRY_TRY propagates a failure out of the current function, so a long chain
// of fallible calls reads as a sequence of statements rather than nested ifs.
#define QUARRY_TRY(expr)                        \
    do {                                        \
        ::quarry::Status _s = (expr);           \
        if (!_s.ok()) return _s;                \
    } while (false)

// QUARRY_ASSIGN binds a Result's value or returns its Status.
//
// The temporary is named with __COUNTER__ rather than __LINE__. Two levels of
// indirection are needed either way, because `_res_##__LINE__` pastes the literal
// text "__LINE__" instead of the number — but __LINE__ still collides when two
// uses appear on one line, which is legal code that should not fail to compile.
// __COUNTER__ is unique per expansion. It is a compiler extension rather than
// standard C++, and is supported by clang, gcc, and msvc.
#define QUARRY_CONCAT_INNER(a, b) a##b
#define QUARRY_CONCAT(a, b) QUARRY_CONCAT_INNER(a, b)

#define QUARRY_ASSIGN_IMPL(lhs, expr, tmp)      \
    auto tmp = (expr);                          \
    if (!tmp.ok()) return tmp.status();         \
    lhs = tmp.take()

#define QUARRY_ASSIGN(lhs, expr) \
    QUARRY_ASSIGN_IMPL(lhs, expr, QUARRY_CONCAT(_quarry_res_, __COUNTER__))

}  // namespace quarry
