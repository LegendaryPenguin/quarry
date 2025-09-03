#include "../src/parser/lexer.hpp"

#include <string>
#include <vector>

#include "quarry/status.hpp"
#include "testing.hpp"

using namespace quarry;          // NOLINT: test-local convenience
using namespace quarry::parser;  // NOLINT

namespace {

// Kinds are compared as ints so CHECK_EQ can print both sides; an enum class has
// no operator<<.
int K(TokenKind kind) { return static_cast<int>(kind); }

std::vector<Token> Lex(const std::string& sql) {
    Result<std::vector<Token>> tokens = Tokenize(sql);
    if (!tokens.ok()) {
        testing::ReportFailure(__FILE__, __LINE__,
                               "Tokenize(" + sql + ") failed: " + tokens.status().ToString());
        // A single end-of-input token rather than an empty vector: the assertions
        // that follow index into the result, and a failed expectation should not
        // turn into an out-of-bounds read.
        return std::vector<Token>(8);
    }
    return tokens.take();
}

std::string LexError(const std::string& sql) {
    Result<std::vector<Token>> tokens = Tokenize(sql);
    if (tokens.ok()) {
        testing::ReportFailure(__FILE__, __LINE__, "Tokenize(" + sql + ") unexpectedly succeeded");
        return "";
    }
    CHECK(tokens.status().code() == Code::kSyntaxError);
    return tokens.status().message();
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

QUARRY_TEST(LexerEmitsEndOfInputForEmptyText) {
    std::vector<Token> tokens = Lex("   \n\t ");
    CHECK_EQ(tokens.size(), size_t{1});
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kEndOfInput));
}

QUARRY_TEST(LexerLowercasesKeywordsAndIdentifiers) {
    std::vector<Token> tokens = Lex("SELECT MyCol FROM MyTable");
    CHECK_EQ(tokens.size(), size_t{5});
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kKeyword));
    CHECK_EQ(tokens[0].text, std::string("select"));
    CHECK_EQ(K(tokens[1].kind), K(TokenKind::kIdentifier));
    CHECK_EQ(tokens[1].text, std::string("mycol"));
    CHECK_EQ(tokens[2].text, std::string("from"));
    CHECK_EQ(K(tokens[3].kind), K(TokenKind::kIdentifier));
    CHECK_EQ(tokens[3].text, std::string("mytable"));
}

QUARRY_TEST(LexerKeepsStringLiteralCase) {
    std::vector<Token> tokens = Lex("'MiXeD Case'");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kString));
    CHECK_EQ(tokens[0].text, std::string("MiXeD Case"));
}

QUARRY_TEST(LexerDecodesDoubledQuoteInsideString) {
    std::vector<Token> tokens = Lex("'it''s'");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kString));
    CHECK_EQ(tokens[0].text, std::string("it's"));

    // A string that is nothing but an escaped quote still closes correctly.
    std::vector<Token> only = Lex("''''");
    CHECK_EQ(K(only[0].kind), K(TokenKind::kString));
    CHECK_EQ(only[0].text, std::string("'"));

    std::vector<Token> empty = Lex("''");
    CHECK_EQ(K(empty[0].kind), K(TokenKind::kString));
    CHECK_EQ(empty[0].text, std::string(""));
}

QUARRY_TEST(LexerDistinguishesIntegerFromFloating) {
    std::vector<Token> tokens = Lex("42 3.5 1e3 2.5E-2");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kInteger));
    CHECK_EQ(tokens[0].int_value, int64_t{42});
    CHECK_EQ(K(tokens[1].kind), K(TokenKind::kDouble));
    CHECK_EQ(tokens[1].double_value, 3.5);
    CHECK_EQ(K(tokens[2].kind), K(TokenKind::kDouble));
    CHECK_EQ(tokens[2].double_value, 1000.0);
    CHECK_EQ(K(tokens[3].kind), K(TokenKind::kDouble));
    CHECK_EQ(tokens[3].double_value, 0.025);
}

QUARRY_TEST(LexerWidensOutOfRangeIntegerToDouble) {
    // One past int64 max: the spelling has no decimal point, but the value only
    // fits a double.
    std::vector<Token> tokens = Lex("9223372036854775808");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kDouble));
    CHECK(tokens[0].double_value > 9.2e18);

    std::vector<Token> at_max = Lex("9223372036854775807");
    CHECK_EQ(K(at_max[0].kind), K(TokenKind::kInteger));
    CHECK_EQ(at_max[0].int_value, int64_t{9223372036854775807});
}

QUARRY_TEST(LexerReadsEveryOperator) {
    std::vector<Token> tokens = Lex("= <> != < <= > >= + - * / ( ) , . ;");
    const TokenKind want[] = {
        TokenKind::kEq,       TokenKind::kNotEq,    TokenKind::kNotEq,
        TokenKind::kLess,     TokenKind::kLessEq,   TokenKind::kGreater,
        TokenKind::kGreaterEq, TokenKind::kPlus,    TokenKind::kMinus,
        TokenKind::kAsterisk, TokenKind::kSlash,    TokenKind::kLParen,
        TokenKind::kRParen,   TokenKind::kComma,    TokenKind::kDot,
        TokenKind::kSemicolon, TokenKind::kEndOfInput,
    };
    CHECK_EQ(tokens.size(), sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < tokens.size() && i < sizeof(want) / sizeof(want[0]); ++i) {
        CHECK_EQ(K(tokens[i].kind), K(want[i]));
    }
}

QUARRY_TEST(LexerTracksLineAndColumnAtTokenStart) {
    std::vector<Token> tokens = Lex("select a\n  from t");
    CHECK_EQ(tokens[0].line, 1);
    CHECK_EQ(tokens[0].column, 1);
    CHECK_EQ(tokens[1].line, 1);
    CHECK_EQ(tokens[1].column, 8);
    CHECK_EQ(tokens[2].line, 2);
    CHECK_EQ(tokens[2].column, 3);
    CHECK_EQ(tokens[3].line, 2);
    CHECK_EQ(tokens[3].column, 8);
    // End of input still carries a position, so "expected X, found end of input"
    // can point past the last token.
    CHECK_EQ(tokens[4].line, 2);
}

QUARRY_TEST(LexerSkipsComments) {
    std::vector<Token> tokens = Lex("select -- everything after this is a comment\n a /* and this */ , b");
    CHECK_EQ(tokens.size(), size_t{5});
    CHECK_EQ(tokens[0].text, std::string("select"));
    CHECK_EQ(tokens[1].text, std::string("a"));
    CHECK_EQ(K(tokens[2].kind), K(TokenKind::kComma));
    CHECK_EQ(tokens[3].text, std::string("b"));
    CHECK_EQ(tokens[1].line, 2);
}

QUARRY_TEST(LexerClassifiesReservedWordsOnly) {
    CHECK(IsReservedKeyword("select"));
    CHECK(IsReservedKeyword("where"));
    CHECK(IsReservedKeyword("lsm"));
    // Type names and aggregate names stay identifiers so that a column may be
    // called `count` or `text`.
    CHECK(!IsReservedKeyword("count"));
    CHECK(!IsReservedKeyword("text"));
    CHECK(!IsReservedKeyword("bigint"));

    std::vector<Token> tokens = Lex("count text");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kIdentifier));
    CHECK_EQ(K(tokens[1].kind), K(TokenKind::kIdentifier));
}

QUARRY_TEST(LexerRejectsUnterminatedString) {
    const std::string message = LexError("select 'abc");
    CHECK(Contains(message, "unterminated string literal"));
    CHECK(Contains(message, "line 1, column 8"));
}

QUARRY_TEST(LexerRejectsUnterminatedStringEndingInEscape) {
    // The trailing '' is an escaped quote, not a terminator, so this string is
    // still open at end of input.
    const std::string message = LexError("'abc''");
    CHECK(Contains(message, "unterminated string literal"));
}

QUARRY_TEST(LexerRejectsUnexpectedCharacter) {
    const std::string message = LexError("select a # b");
    CHECK(Contains(message, "unexpected character '#'"));
    CHECK(Contains(message, "line 1, column 10"));
}

QUARRY_TEST(LexerRejectsBangWithoutEquals) {
    const std::string message = LexError("a ! b");
    CHECK(Contains(message, "expected '=' after '!'"));
    CHECK(Contains(message, "column 3"));
}

QUARRY_TEST(LexerDescribesTokensForErrorMessages) {
    std::vector<Token> tokens = Lex("from x 1 'y' ,");
    CHECK_EQ(tokens[0].Describe(), std::string("keyword 'from'"));
    CHECK_EQ(tokens[1].Describe(), std::string("identifier 'x'"));
    CHECK_EQ(tokens[2].Describe(), std::string("integer literal '1'"));
    CHECK_EQ(tokens[3].Describe(), std::string("string literal 'y'"));
    CHECK_EQ(tokens[4].Describe(), std::string("','"));
    CHECK_EQ(tokens[5].Describe(), std::string("end of input"));
}

QUARRY_TEST(LexerHandlesIdentifiersWithDigitsAndUnderscores) {
    std::vector<Token> tokens = Lex("_a b1 c_2$");
    CHECK_EQ(K(tokens[0].kind), K(TokenKind::kIdentifier));
    CHECK_EQ(tokens[0].text, std::string("_a"));
    CHECK_EQ(tokens[1].text, std::string("b1"));
    CHECK_EQ(tokens[2].text, std::string("c_2$"));
}

QUARRY_TEST_MAIN()
