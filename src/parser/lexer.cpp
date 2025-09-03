#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <string>

namespace quarry::parser {
namespace {

// The reserved set holds only the words that drive statement structure. Type
// names (BIGINT, VARCHAR, ...) and aggregate names (COUNT, SUM, ...) are
// deliberately left as identifiers: reserving them would make `count` or `text`
// unusable as a column name, and the parser can recognize both from context —
// a type name only ever appears in a column definition, and an aggregate name
// only counts as one when an open paren follows it.
constexpr std::array<std::string_view, 29> kKeywords = {
    "analyze", "and",   "as",     "asc",   "by",     "create", "desc",
    "explain", "false", "from",   "group", "index",  "inner",  "insert",
    "into",    "join",  "limit",  "lsm",   "not",    "null",   "on",
    "or",      "order", "select", "table", "true",   "using",  "values",
    "where",
};

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string Quoted(std::string_view s) {
    return "'" + std::string(s) + "'";
}

}  // namespace

const char* TokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::kEndOfInput: return "end of input";
        case TokenKind::kIdentifier: return "identifier";
        case TokenKind::kKeyword:    return "keyword";
        case TokenKind::kInteger:    return "integer literal";
        case TokenKind::kDouble:     return "floating-point literal";
        case TokenKind::kString:     return "string literal";
        case TokenKind::kEq:         return "'='";
        case TokenKind::kNotEq:      return "'<>'";
        case TokenKind::kLess:       return "'<'";
        case TokenKind::kLessEq:     return "'<='";
        case TokenKind::kGreater:    return "'>'";
        case TokenKind::kGreaterEq:  return "'>='";
        case TokenKind::kPlus:       return "'+'";
        case TokenKind::kMinus:      return "'-'";
        case TokenKind::kAsterisk:   return "'*'";
        case TokenKind::kSlash:      return "'/'";
        case TokenKind::kLParen:     return "'('";
        case TokenKind::kRParen:     return "')'";
        case TokenKind::kComma:      return "','";
        case TokenKind::kDot:        return "'.'";
        case TokenKind::kSemicolon:  return "';'";
    }
    return "token";
}

std::string Token::Describe() const {
    switch (kind) {
        case TokenKind::kEndOfInput:
            return "end of input";
        case TokenKind::kIdentifier:
            return "identifier " + Quoted(text);
        case TokenKind::kKeyword:
            return "keyword " + Quoted(text);
        case TokenKind::kString:
            return "string literal " + Quoted(text);
        case TokenKind::kInteger:
        case TokenKind::kDouble:
            return std::string(TokenKindName(kind)) + " " + Quoted(text);
        default:
            return TokenKindName(kind);
    }
}

bool IsReservedKeyword(std::string_view lowercased) {
    return std::binary_search(kKeywords.begin(), kKeywords.end(), lowercased);
}

void Lexer::Advance() {
    if (AtEnd()) return;
    if (sql_[pos_] == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    ++pos_;
}

Status Lexer::ErrorAt(int line, int column, const std::string& what) const {
    return Status::SyntaxError("syntax error at line " + std::to_string(line) +
                               ", column " + std::to_string(column) + ": " + what);
}

Token Lexer::Make(TokenKind kind, std::string text, int line, int column) const {
    Token t;
    t.kind = kind;
    t.text = std::move(text);
    t.line = line;
    t.column = column;
    return t;
}

void Lexer::SkipWhitespaceAndComments() {
    for (;;) {
        while (!AtEnd() && std::isspace(static_cast<unsigned char>(Peek())) != 0) Advance();
        if (Peek() == '-' && Peek(1) == '-') {
            while (!AtEnd() && Peek() != '\n') Advance();
            continue;
        }
        if (Peek() == '/' && Peek(1) == '*') {
            Advance();
            Advance();
            // An unterminated block comment is not reported: the remaining text is
            // commented out, so the parser's "unexpected end of input" is both
            // accurate and more useful than a second error about the comment.
            while (!AtEnd() && !(Peek() == '*' && Peek(1) == '/')) Advance();
            if (!AtEnd()) {
                Advance();
                Advance();
            }
            continue;
        }
        return;
    }
}

Result<Token> Lexer::LexNumber(int line, int column) {
    const size_t start = pos_;
    while (IsDigit(Peek())) Advance();

    bool is_float = false;
    if (Peek() == '.') {
        is_float = true;
        Advance();
        while (IsDigit(Peek())) Advance();
    }
    if (Peek() == 'e' || Peek() == 'E') {
        const char sign = Peek(1);
        const size_t digit_offset = (sign == '+' || sign == '-') ? 2 : 1;
        if (IsDigit(Peek(digit_offset))) {
            is_float = true;
            Advance();
            if (sign == '+' || sign == '-') Advance();
            while (IsDigit(Peek())) Advance();
        }
    }

    std::string text(sql_.substr(start, pos_ - start));

    if (!is_float) {
        int64_t v = 0;
        const char* begin = text.data();
        const char* end = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(begin, end, v);
        if (ec == std::errc() && ptr == end) {
            Token t = Make(TokenKind::kInteger, std::move(text), line, column);
            t.int_value = v;
            return t;
        }
        // Out of int64 range. Widening to double rather than failing matches what
        // the engine can actually store, and keeps a literal like 1e100's integer
        // spelling from being a syntax error.
    }

    // strtod rather than from_chars: libc++ only grew floating-point from_chars
    // recently, and the fallback is not worth a configure-time check here.
    const char* cstr = text.c_str();
    char* parse_end = nullptr;
    const double d = std::strtod(cstr, &parse_end);
    if (parse_end != cstr + text.size()) {
        return ErrorAt(line, column, "malformed numeric literal " + Quoted(text));
    }
    Token t = Make(TokenKind::kDouble, std::move(text), line, column);
    t.double_value = d;
    return t;
}

Result<Token> Lexer::LexString(int line, int column) {
    Advance();  // opening quote
    std::string value;
    for (;;) {
        if (AtEnd()) {
            return ErrorAt(line, column, "unterminated string literal");
        }
        if (Peek() == '\'') {
            if (Peek(1) == '\'') {
                value.push_back('\'');
                Advance();
                Advance();
                continue;
            }
            Advance();  // closing quote
            return Make(TokenKind::kString, std::move(value), line, column);
        }
        value.push_back(Peek());
        Advance();
    }
}

Result<Token> Lexer::LexWord(int line, int column) {
    std::string word;
    while (IsIdentPart(Peek())) {
        // Lowercasing happens here, once, because the schema layer compares names
        // case-sensitively and expects normalized input.
        word.push_back(Lower(Peek()));
        Advance();
    }
    const TokenKind kind =
        IsReservedKeyword(word) ? TokenKind::kKeyword : TokenKind::kIdentifier;
    return Make(kind, std::move(word), line, column);
}

Result<Token> Lexer::Next() {
    SkipWhitespaceAndComments();

    const int line = line_;
    const int column = column_;

    if (AtEnd()) return Make(TokenKind::kEndOfInput, "", line, column);

    const char c = Peek();

    if (IsDigit(c)) return LexNumber(line, column);
    if (c == '\'') return LexString(line, column);
    if (IsIdentStart(c)) return LexWord(line, column);

    auto single = [&](TokenKind kind) {
        Advance();
        return Make(kind, std::string(1, c), line, column);
    };

    switch (c) {
        case '=': return single(TokenKind::kEq);
        case '+': return single(TokenKind::kPlus);
        case '-': return single(TokenKind::kMinus);
        case '*': return single(TokenKind::kAsterisk);
        case '/': return single(TokenKind::kSlash);
        case '(': return single(TokenKind::kLParen);
        case ')': return single(TokenKind::kRParen);
        case ',': return single(TokenKind::kComma);
        case '.': return single(TokenKind::kDot);
        case ';': return single(TokenKind::kSemicolon);
        case '<':
            Advance();
            if (Peek() == '>') {
                Advance();
                return Make(TokenKind::kNotEq, "<>", line, column);
            }
            if (Peek() == '=') {
                Advance();
                return Make(TokenKind::kLessEq, "<=", line, column);
            }
            return Make(TokenKind::kLess, "<", line, column);
        case '>':
            Advance();
            if (Peek() == '=') {
                Advance();
                return Make(TokenKind::kGreaterEq, ">=", line, column);
            }
            return Make(TokenKind::kGreater, ">", line, column);
        case '!':
            Advance();
            if (Peek() == '=') {
                Advance();
                return Make(TokenKind::kNotEq, "!=", line, column);
            }
            return ErrorAt(line, column, "expected '=' after '!'");
        default:
            break;
    }

    return ErrorAt(line, column, "unexpected character '" + std::string(1, c) + "'");
}

Result<std::vector<Token>> Tokenize(std::string_view sql) {
    Lexer lexer(sql);
    std::vector<Token> tokens;
    for (;;) {
        Result<Token> tok = lexer.Next();
        if (!tok.ok()) return tok.status();
        const bool done = tok.value().kind == TokenKind::kEndOfInput;
        tokens.push_back(tok.take());
        if (done) return tokens;
    }
}

}  // namespace quarry::parser
