#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "quarry/status.hpp"

namespace quarry::parser {

enum class TokenKind : uint8_t {
    kEndOfInput,
    kIdentifier,
    kKeyword,
    kInteger,
    kDouble,
    kString,

    // Operators and punctuation get one kind each rather than a single kOperator
    // kind plus a lexeme comparison, so the parser dispatches on an enum and a
    // typo in an expected-token name is a compile error instead of a silent
    // never-matching branch.
    kEq,
    kNotEq,
    kLess,
    kLessEq,
    kGreater,
    kGreaterEq,
    kPlus,
    kMinus,
    kAsterisk,
    kSlash,
    kLParen,
    kRParen,
    kComma,
    kDot,
    kSemicolon,
};

const char* TokenKindName(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::kEndOfInput;

    // text is the lexeme: lowercased for identifiers and keywords, the decoded
    // contents for a string literal (so '' has already become '), and the source
    // spelling for everything else.
    std::string text;

    int64_t int_value = 0;
    double double_value = 0.0;

    // Both 1-based, and both point at the token's first character rather than at
    // wherever the lexer stopped, so an error message underlines the token the
    // user wrote.
    int line = 1;
    int column = 1;

    bool Is(TokenKind k) const { return kind == k; }

    // IsKeyword compares against an already-lowercased spelling; keywords are
    // normalized by the lexer so no call site has to case-fold.
    bool IsKeyword(std::string_view kw) const {
        return kind == TokenKind::kKeyword && text == kw;
    }

    // Describe renders the token the way an error message wants to name it.
    std::string Describe() const;
};

// IsReservedKeyword reports whether a lowercased word is lexed as a keyword
// rather than an identifier.
bool IsReservedKeyword(std::string_view lowercased);

class Lexer {
public:
    explicit Lexer(std::string_view sql) : sql_(sql) {}

    // Next returns the following token, or a syntax error naming the offending
    // position. End of input is a token (kEndOfInput), not an error, so the
    // parser can report "expected FROM, found end of input" uniformly.
    Result<Token> Next();

private:
    void SkipWhitespaceAndComments();
    Token Make(TokenKind kind, std::string text, int line, int column) const;
    Result<Token> LexNumber(int line, int column);
    Result<Token> LexString(int line, int column);
    Result<Token> LexWord(int line, int column);

    char Peek(size_t ahead = 0) const {
        return pos_ + ahead < sql_.size() ? sql_[pos_ + ahead] : '\0';
    }
    bool AtEnd() const { return pos_ >= sql_.size(); }
    void Advance();

    Status ErrorAt(int line, int column, const std::string& what) const;

    std::string_view sql_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

// Tokenize runs the lexer to end of input. The parser works over the whole
// vector because arbitrary lookahead is then free, and SQL statements are short
// enough that holding every token costs nothing worth optimizing.
Result<std::vector<Token>> Tokenize(std::string_view sql);

}  // namespace quarry::parser
