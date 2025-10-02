#include "parser.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lexer.hpp"

namespace quarry::parser {
namespace {

using ast::BinaryOp;
using ast::ExprPtr;
using ast::Expression;

std::string Position(const Token& t) {
    return "line " + std::to_string(t.line) + ", column " + std::to_string(t.column);
}

// Well-formed SQL that this engine deliberately does not implement.
//
// Reported as kNotImplemented rather than kSyntaxError, which is not pedantry: a
// syntax error sends the reader hunting for a typo, and there is no typo. The
// statement is valid SQL and the engine simply does not support it. The SQL
// surface here is narrow on purpose, so hitting its edge is expected and should
// say so.
//
// Recognition is contextual rather than by reserving these words. Reserving
// `having` or `left` would make them unusable as column names, which is a real
// cost paid for an error message; the parser already holds the lexeme at the
// point it gives up, so it can recognize the construct without the lexer's help.
std::optional<Status> UnsupportedConstruct(const Token& found, std::string_view expectation) {
    const std::string& w = found.text;

    // Leading with "not supported" rather than with the construct name avoids a
    // subject-verb agreement problem: the names are a mix of singular and plural
    // ("HAVING", "outer joins"), and "<name> is not supported" reads wrong for half
    // of them.
    auto ni = [&](std::string_view construct, std::string_view detail) {
        return Status::NotImplemented("not supported at " + Position(found) + ": " +
                                      std::string(construct) + "; " + std::string(detail));
    };

    if (w == "having") {
        return ni("HAVING", "filter after aggregation is outside the supported subset");
    }
    if (w == "distinct") {
        return ni("DISTINCT", "use GROUP BY over the same columns instead");
    }
    if (w == "union" || w == "intersect" || w == "except") {
        return ni("set operations (UNION/INTERSECT/EXCEPT)",
                  "only a single SELECT block is supported");
    }
    if (w == "offset") {
        return ni("OFFSET", "LIMIT is supported, OFFSET is not");
    }
    if (w == "left" || w == "right" || w == "full" || w == "outer") {
        return ni("outer joins", "only a single-column equi INNER JOIN is supported");
    }
    if (w == "delete" || w == "update") {
        return ni("UPDATE and DELETE",
                  "the engine is append-oriented; segments are immutable once written");
    }
    if (w == "with") {
        return ni("common table expressions", "only a single SELECT block is supported");
    }
    // A subquery announces itself as SELECT appearing where a value was wanted.
    if (w == "select" && expectation.find("expression") != std::string_view::npos) {
        return ni("subqueries", "an expression may not contain a SELECT");
    }
    // A derived table is an open paren where a table name was wanted.
    if (w == "(" && expectation.find("table name") != std::string_view::npos) {
        return ni("derived tables (a subquery in FROM)", "FROM must name a table");
    }
    if ((w == "inner" || w == "join" || w == "cross") &&
        expectation.find("end of statement") != std::string_view::npos) {
        return ni("more than one join", "a query may contain at most one INNER JOIN");
    }
    // A comma after the FROM table is the old cross-join spelling.
    if (w == "," && expectation.find("end of statement") != std::string_view::npos) {
        return ni("comma cross joins", "use INNER JOIN ... ON instead");
    }
    return std::nullopt;
}

// Every message has the same shape — where, what was wanted, what was there —
// so that a user reading two different errors does not have to re-learn the
// format, and so a test can assert on the position substring.
Status Expected(const Token& found, const std::string& expectation) {
    // Check for a recognizable unsupported construct before calling it a syntax
    // error, so the one message a user is most likely to hit is the useful one.
    if (std::optional<Status> unsupported = UnsupportedConstruct(found, expectation)) {
        return *unsupported;
    }
    return Status::SyntaxError("syntax error at " + Position(found) + ": expected " +
                               expectation + ", found " + found.Describe());
}

Status ErrorAt(const Token& at, const std::string& what) {
    return Status::SyntaxError("syntax error at " + Position(at) + ": " + what);
}

std::optional<AggregateKind> AggregateNamed(std::string_view word) {
    if (word == "count") return AggregateKind::kCount;
    if (word == "sum") return AggregateKind::kSum;
    if (word == "avg") return AggregateKind::kAvg;
    if (word == "min") return AggregateKind::kMin;
    if (word == "max") return AggregateKind::kMax;
    return std::nullopt;
}

const char* AggregateSpelling(AggregateKind k) {
    switch (k) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum:   return "SUM";
        case AggregateKind::kAvg:   return "AVG";
        case AggregateKind::kMin:   return "MIN";
        case AggregateKind::kMax:   return "MAX";
    }
    return "aggregate";
}

std::optional<TypeId> TypeNamed(std::string_view word) {
    if (word == "bigint" || word == "int" || word == "integer") return TypeId::kInt64;
    if (word == "double" || word == "float" || word == "real") return TypeId::kDouble;
    if (word == "varchar" || word == "text") return TypeId::kVarchar;
    if (word == "boolean" || word == "bool") return TypeId::kBoolean;
    return std::nullopt;
}

std::optional<BinaryOp> ComparisonFor(TokenKind kind) {
    switch (kind) {
        case TokenKind::kEq:        return BinaryOp::kEq;
        case TokenKind::kNotEq:     return BinaryOp::kNe;
        case TokenKind::kLess:      return BinaryOp::kLt;
        case TokenKind::kLessEq:    return BinaryOp::kLe;
        case TokenKind::kGreater:   return BinaryOp::kGt;
        case TokenKind::kGreaterEq: return BinaryOp::kGe;
        default:                    return std::nullopt;
    }
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Result<ast::Statement> ParseStatement();
    Result<ExprPtr> ParseWholeExpression();

private:
    // --- token access ---
    const Token& Peek(size_t ahead = 0) const {
        const size_t i = pos_ + ahead;
        // The vector always ends with kEndOfInput, so clamping yields that token
        // and every lookahead is in bounds without a special case per call site.
        return tokens_[i < tokens_.size() ? i : tokens_.size() - 1];
    }
    const Token& Cur() const { return Peek(); }
    void Advance() {
        if (pos_ + 1 < tokens_.size()) ++pos_;
    }

    bool Match(TokenKind kind) {
        if (!Cur().Is(kind)) return false;
        Advance();
        return true;
    }
    bool MatchKeyword(std::string_view kw) {
        if (!Cur().IsKeyword(kw)) return false;
        Advance();
        return true;
    }
    Status Consume(TokenKind kind, const std::string& expectation) {
        if (!Cur().Is(kind)) return Expected(Cur(), expectation);
        Advance();
        return Status::Ok();
    }
    Status ConsumeKeyword(std::string_view kw) {
        if (!Cur().IsKeyword(kw)) {
            std::string upper(kw);
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return Expected(Cur(), upper);
        }
        Advance();
        return Status::Ok();
    }
    Result<std::string> ConsumeIdentifier(const std::string& role) {
        if (!Cur().Is(TokenKind::kIdentifier)) return Expected(Cur(), role);
        std::string name = Cur().text;
        Advance();
        return name;
    }

    // --- statements ---
    Result<std::unique_ptr<ast::SelectStatement>> ParseSelect();
    Result<std::unique_ptr<ast::CreateTableStatement>> ParseCreateTable();
    Result<std::unique_ptr<ast::CreateIndexStatement>> ParseCreateIndex();
    Result<std::unique_ptr<ast::InsertStatement>> ParseInsert();
    Status ParseProjections(std::vector<ExprPtr>& out);
    Status ParseJoin(ast::SelectStatement& select);
    Result<Column> ParseColumnDefinition();
    Result<std::vector<ExprPtr>> ParseValueRow();

    // --- expressions, lowest precedence first ---
    Result<ExprPtr> ParseExpression() { return ParseOr(); }
    Result<ExprPtr> ParseOr();
    Result<ExprPtr> ParseAnd();
    Result<ExprPtr> ParseComparison();
    Result<ExprPtr> ParseAdditive();
    Result<ExprPtr> ParseMultiplicative();
    Result<ExprPtr> ParseUnary();
    Result<ExprPtr> ParsePrimary();
    Result<ExprPtr> ParseAggregate();

    // Qualified column references keep the qualifier in column_name ("t.a"),
    // because the AST has one name field and dropping the table part would make
    // `a.id = b.id` indistinguishable from `id = id` for the binder.
    Result<std::string> ParseQualifiedName();

    Status EnterDepth(const Token& at) {
        if (++depth_ > kMaxExpressionDepth) {
            return ErrorAt(at, "expression nests deeper than the limit of " +
                                   std::to_string(kMaxExpressionDepth));
        }
        return Status::Ok();
    }
    void LeaveDepth() { --depth_; }

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    int depth_ = 0;
};

Result<ast::Statement> Parser::ParseStatement() {
    ast::Statement stmt;

    // Statements the engine does not implement are caught here rather than
    // falling through to a confusing "expected SELECT" further down.
    if (std::optional<Status> unsupported = UnsupportedConstruct(Cur(), "a statement")) {
        return *unsupported;
    }

    if (Cur().IsKeyword("select")) {
        Result<std::unique_ptr<ast::SelectStatement>> select = ParseSelect();
        if (!select.ok()) return select.status();
        stmt.kind = ast::StatementKind::kSelect;
        stmt.select = select.take();
    } else if (Cur().IsKeyword("explain")) {
        Advance();
        stmt.kind = ast::StatementKind::kExplain;
        stmt.analyze = MatchKeyword("analyze");
        if (!Cur().IsKeyword("select")) return Expected(Cur(), "SELECT after EXPLAIN");
        Result<std::unique_ptr<ast::SelectStatement>> select = ParseSelect();
        if (!select.ok()) return select.status();
        stmt.explain_select = select.take();
    } else if (Cur().IsKeyword("insert")) {
        Result<std::unique_ptr<ast::InsertStatement>> insert = ParseInsert();
        if (!insert.ok()) return insert.status();
        stmt.kind = ast::StatementKind::kInsert;
        stmt.insert = insert.take();
    } else if (Cur().IsKeyword("create")) {
        Advance();
        if (Cur().IsKeyword("table")) {
            Result<std::unique_ptr<ast::CreateTableStatement>> create = ParseCreateTable();
            if (!create.ok()) return create.status();
            stmt.kind = ast::StatementKind::kCreateTable;
            stmt.create_table = create.take();
        } else if (Cur().IsKeyword("index")) {
            Result<std::unique_ptr<ast::CreateIndexStatement>> create = ParseCreateIndex();
            if (!create.ok()) return create.status();
            stmt.kind = ast::StatementKind::kCreateIndex;
            stmt.create_index = create.take();
        } else {
            return Expected(Cur(), "TABLE or INDEX after CREATE");
        }
    } else {
        return Expected(Cur(), "a statement (SELECT, INSERT, CREATE, or EXPLAIN)");
    }

    Match(TokenKind::kSemicolon);
    if (!Cur().Is(TokenKind::kEndOfInput)) {
        return Expected(Cur(), "end of statement");
    }
    return stmt;
}

Result<ExprPtr> Parser::ParseWholeExpression() {
    Result<ExprPtr> expr = ParseExpression();
    if (!expr.ok()) return expr.status();
    if (!Cur().Is(TokenKind::kEndOfInput)) return Expected(Cur(), "end of expression");
    return expr;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

Status Parser::ParseProjections(std::vector<ExprPtr>& out) {
    for (;;) {
        if (Cur().Is(TokenKind::kAsterisk)) {
            const Token& star = Cur();
            Advance();
            // SELECT * is represented as a single kStar projection, so mixing it
            // with other items has no representation and is rejected here rather
            // than silently losing one of them.
            if (!out.empty() || Cur().Is(TokenKind::kComma)) {
                return ErrorAt(star, "SELECT * cannot be combined with other projections");
            }
            out.push_back(Expression::Star());
            return Status::Ok();
        }

        Result<ExprPtr> expr = ParseExpression();
        if (!expr.ok()) return expr.status();
        ExprPtr e = expr.take();

        if (MatchKeyword("as")) {
            Result<std::string> alias = ConsumeIdentifier("an alias name after AS");
            if (!alias.ok()) return alias.status();
            e->alias = alias.take();
        }
        out.push_back(std::move(e));

        if (!Match(TokenKind::kComma)) return Status::Ok();
    }
}

Status Parser::ParseJoin(ast::SelectStatement& select) {
    ast::JoinClause join;
    Result<std::string> table = ConsumeIdentifier("a table name after JOIN");
    if (!table.ok()) return table.status();
    join.table = table.take();

    QUARRY_TRY(ConsumeKeyword("on"));

    const Token& left_token = Cur();
    Result<std::string> left = ParseQualifiedName();
    if (!left.ok()) return left.status();
    QUARRY_TRY(Consume(TokenKind::kEq, "'=' in a join condition"));
    Result<std::string> right = ParseQualifiedName();
    if (!right.ok()) return right.status();

    // The AST stores each side already attributed to a table, so the qualifier is
    // consumed here: `ON b.id = a.id` and `ON a.id = b.id` must produce the same
    // clause. An unqualified side is taken positionally.
    std::string left_name = left.take();
    std::string right_name = right.take();
    const std::string prefix = join.table + ".";
    const bool left_is_join_side = left_name.rfind(prefix, 0) == 0;
    const bool right_is_join_side = right_name.rfind(prefix, 0) == 0;
    if (left_is_join_side && right_is_join_side) {
        return ErrorAt(left_token,
                       "both sides of the join condition refer to " + join.table);
    }
    if (left_is_join_side) std::swap(left_name, right_name);

    auto strip_qualifier = [](const std::string& name) {
        const size_t dot = name.find('.');
        return dot == std::string::npos ? name : name.substr(dot + 1);
    };
    join.left_column = strip_qualifier(left_name);
    join.right_column = strip_qualifier(right_name);
    select.join = std::move(join);
    return Status::Ok();
}

Result<std::unique_ptr<ast::SelectStatement>> Parser::ParseSelect() {
    QUARRY_TRY(ConsumeKeyword("select"));

    // DISTINCT has to be caught here. It is not a reserved word, so it would
    // otherwise be consumed as the first projection's column name and the failure
    // would surface much later as "expected FROM" pointing at the real column.
    if (std::optional<Status> unsupported = UnsupportedConstruct(Cur(), "a statement")) {
        return *unsupported;
    }

    auto select = std::make_unique<ast::SelectStatement>();

    QUARRY_TRY(ParseProjections(select->projections));

    QUARRY_TRY(ConsumeKeyword("from"));
    Result<std::string> table = ConsumeIdentifier("a table name after FROM");
    if (!table.ok()) return table.status();
    select->from_table = table.take();

    if (Cur().IsKeyword("inner") || Cur().IsKeyword("join")) {
        const bool had_inner = MatchKeyword("inner");
        if (had_inner) {
            QUARRY_TRY(ConsumeKeyword("join"));
        } else {
            Advance();
        }
        QUARRY_TRY(ParseJoin(*select));
    }

    if (MatchKeyword("where")) {
        Result<ExprPtr> where = ParseExpression();
        if (!where.ok()) return where.status();
        select->where = where.take();
    }

    if (MatchKeyword("group")) {
        QUARRY_TRY(ConsumeKeyword("by"));
        for (;;) {
            Result<ExprPtr> key = ParseExpression();
            if (!key.ok()) return key.status();
            select->group_by.push_back(key.take());
            if (!Match(TokenKind::kComma)) break;
        }
    }

    if (MatchKeyword("order")) {
        QUARRY_TRY(ConsumeKeyword("by"));
        for (;;) {
            Result<ExprPtr> key = ParseExpression();
            if (!key.ok()) return key.status();
            ast::OrderByItem item;
            item.expr = key.take();
            if (MatchKeyword("desc")) {
                item.descending = true;
            } else {
                MatchKeyword("asc");
            }
            select->order_by.push_back(std::move(item));
            if (!Match(TokenKind::kComma)) break;
        }
    }

    if (MatchKeyword("limit")) {
        if (!Cur().Is(TokenKind::kInteger)) {
            return Expected(Cur(), "a non-negative integer after LIMIT");
        }
        select->limit = static_cast<uint64_t>(Cur().int_value);
        Advance();
    }

    return std::move(select);
}

// ---------------------------------------------------------------------------
// CREATE
// ---------------------------------------------------------------------------

Result<Column> Parser::ParseColumnDefinition() {
    Result<std::string> name = ConsumeIdentifier("a column name");
    if (!name.ok()) return name.status();

    const Token& type_token = Cur();
    if (!type_token.Is(TokenKind::kIdentifier)) {
        return Expected(type_token, "a type name");
    }
    std::optional<TypeId> type = TypeNamed(type_token.text);
    if (!type.has_value()) {
        return ErrorAt(type_token, "unknown type name '" + type_token.text + "'");
    }
    const bool is_varchar = *type == TypeId::kVarchar;
    Advance();

    // A declared VARCHAR length is accepted and discarded: storage is
    // length-prefixed, so the number carries no information the engine can use,
    // and rejecting it would make ordinary DDL fail to parse.
    if (is_varchar && Match(TokenKind::kLParen)) {
        if (!Cur().Is(TokenKind::kInteger)) {
            return Expected(Cur(), "a length after VARCHAR(");
        }
        Advance();
        QUARRY_TRY(Consume(TokenKind::kRParen, "')' after a VARCHAR length"));
    }

    bool nullable = true;
    if (MatchKeyword("not")) {
        QUARRY_TRY(ConsumeKeyword("null"));
        nullable = false;
    } else if (MatchKeyword("null")) {
        nullable = true;
    }

    return Column(name.take(), *type, nullable);
}

Result<std::unique_ptr<ast::CreateTableStatement>> Parser::ParseCreateTable() {
    QUARRY_TRY(ConsumeKeyword("table"));
    auto create = std::make_unique<ast::CreateTableStatement>();

    Result<std::string> name = ConsumeIdentifier("a table name");
    if (!name.ok()) return name.status();
    create->table = name.take();

    QUARRY_TRY(Consume(TokenKind::kLParen, "'(' before the column list"));
    for (;;) {
        Result<Column> column = ParseColumnDefinition();
        if (!column.ok()) return column.status();
        create->schema.Add(column.take());
        if (!Match(TokenKind::kComma)) break;
    }
    QUARRY_TRY(Consume(TokenKind::kRParen, "')' after the column list"));

    return std::move(create);
}

Result<std::unique_ptr<ast::CreateIndexStatement>> Parser::ParseCreateIndex() {
    QUARRY_TRY(ConsumeKeyword("index"));
    auto create = std::make_unique<ast::CreateIndexStatement>();

    Result<std::string> name = ConsumeIdentifier("an index name");
    if (!name.ok()) return name.status();
    create->name = name.take();

    QUARRY_TRY(ConsumeKeyword("on"));
    Result<std::string> table = ConsumeIdentifier("a table name after ON");
    if (!table.ok()) return table.status();
    create->table = table.take();

    QUARRY_TRY(Consume(TokenKind::kLParen, "'(' before the indexed column"));
    Result<std::string> column = ConsumeIdentifier("a column name");
    if (!column.ok()) return column.status();
    create->column = column.take();
    QUARRY_TRY(Consume(TokenKind::kRParen, "')' after the indexed column"));

    if (MatchKeyword("using")) {
        QUARRY_TRY(ConsumeKeyword("lsm"));
        create->lsm = true;
    }

    return std::move(create);
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------

Result<std::vector<ExprPtr>> Parser::ParseValueRow() {
    QUARRY_TRY(Consume(TokenKind::kLParen, "'(' before a VALUES row"));
    std::vector<ExprPtr> row;
    for (;;) {
        Result<ExprPtr> value = ParseExpression();
        if (!value.ok()) return value.status();
        row.push_back(value.take());
        if (!Match(TokenKind::kComma)) break;
    }
    QUARRY_TRY(Consume(TokenKind::kRParen, "')' after a VALUES row"));
    return std::move(row);
}

Result<std::unique_ptr<ast::InsertStatement>> Parser::ParseInsert() {
    QUARRY_TRY(ConsumeKeyword("insert"));
    QUARRY_TRY(ConsumeKeyword("into"));
    auto insert = std::make_unique<ast::InsertStatement>();

    Result<std::string> table = ConsumeIdentifier("a table name after INTO");
    if (!table.ok()) return table.status();
    insert->table = table.take();

    if (Match(TokenKind::kLParen)) {
        for (;;) {
            Result<std::string> column = ConsumeIdentifier("a column name");
            if (!column.ok()) return column.status();
            insert->columns.push_back(column.take());
            if (!Match(TokenKind::kComma)) break;
        }
        QUARRY_TRY(Consume(TokenKind::kRParen, "')' after the column list"));
    }

    QUARRY_TRY(ConsumeKeyword("values"));
    for (;;) {
        Result<std::vector<ExprPtr>> row = ParseValueRow();
        if (!row.ok()) return row.status();
        insert->rows.push_back(row.take());
        if (!Match(TokenKind::kComma)) break;
    }

    return std::move(insert);
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

Result<ExprPtr> Parser::ParseOr() {
    Result<ExprPtr> left = ParseAnd();
    if (!left.ok()) return left.status();
    ExprPtr node = left.take();
    while (MatchKeyword("or")) {
        Result<ExprPtr> right = ParseAnd();
        if (!right.ok()) return right.status();
        node = Expression::Binary(BinaryOp::kOr, std::move(node), right.take());
    }
    return std::move(node);
}

Result<ExprPtr> Parser::ParseAnd() {
    Result<ExprPtr> left = ParseComparison();
    if (!left.ok()) return left.status();
    ExprPtr node = left.take();
    while (MatchKeyword("and")) {
        Result<ExprPtr> right = ParseComparison();
        if (!right.ok()) return right.status();
        node = Expression::Binary(BinaryOp::kAnd, std::move(node), right.take());
    }
    return std::move(node);
}

Result<ExprPtr> Parser::ParseComparison() {
    Result<ExprPtr> left = ParseAdditive();
    if (!left.ok()) return left.status();
    ExprPtr node = left.take();
    for (;;) {
        std::optional<BinaryOp> op = ComparisonFor(Cur().kind);
        if (!op.has_value()) break;
        Advance();
        Result<ExprPtr> right = ParseAdditive();
        if (!right.ok()) return right.status();
        node = Expression::Binary(*op, std::move(node), right.take());
    }
    return std::move(node);
}

Result<ExprPtr> Parser::ParseAdditive() {
    Result<ExprPtr> left = ParseMultiplicative();
    if (!left.ok()) return left.status();
    ExprPtr node = left.take();
    for (;;) {
        BinaryOp op;
        if (Cur().Is(TokenKind::kPlus)) {
            op = BinaryOp::kAdd;
        } else if (Cur().Is(TokenKind::kMinus)) {
            op = BinaryOp::kSub;
        } else {
            break;
        }
        Advance();
        Result<ExprPtr> right = ParseMultiplicative();
        if (!right.ok()) return right.status();
        node = Expression::Binary(op, std::move(node), right.take());
    }
    return std::move(node);
}

Result<ExprPtr> Parser::ParseMultiplicative() {
    Result<ExprPtr> left = ParseUnary();
    if (!left.ok()) return left.status();
    ExprPtr node = left.take();
    for (;;) {
        BinaryOp op;
        if (Cur().Is(TokenKind::kAsterisk)) {
            op = BinaryOp::kMul;
        } else if (Cur().Is(TokenKind::kSlash)) {
            op = BinaryOp::kDiv;
        } else {
            break;
        }
        Advance();
        Result<ExprPtr> right = ParseUnary();
        if (!right.ok()) return right.status();
        node = Expression::Binary(op, std::move(node), right.take());
    }
    return std::move(node);
}

Result<ExprPtr> Parser::ParseUnary() {
    if (Cur().IsKeyword("not")) {
        // The AST has no unary operator, so NOT cannot be represented. Saying so
        // beats a message about an unexpected keyword.
        return ErrorAt(Cur(), "NOT is not supported; use <> or a comparison instead");
    }

    if (Cur().Is(TokenKind::kPlus)) {
        Advance();
        return ParseUnary();  // unary plus is identity, so it leaves no node
    }
    if (Cur().Is(TokenKind::kMinus)) {
        const Token& minus = Cur();
        Advance();
        QUARRY_TRY(EnterDepth(minus));
        Result<ExprPtr> operand = ParseUnary();
        LeaveDepth();
        if (!operand.ok()) return operand.status();
        ExprPtr e = operand.take();
        // Folding negation into a numeric literal keeps `-1` a constant, which is
        // what makes it pushable as a Predicate instead of needing evaluation.
        if (e->kind == ast::ExprKind::kLiteral && e->literal.IsNumeric()) {
            if (e->literal.type() == TypeId::kInt64 &&
                e->literal.as_int64() != INT64_MIN) {
                return Expression::Literal(Value::Int64(-e->literal.as_int64()));
            }
            if (e->literal.type() == TypeId::kInt64) {
                // Negating INT64_MIN would be undefined; the magnitude only fits a
                // double, which is also what the unnegated literal would have been.
                return Expression::Literal(
                    Value::Double(-static_cast<double>(e->literal.as_int64())));
            }
            return Expression::Literal(Value::Double(-e->literal.as_double()));
        }
        return Expression::Binary(BinaryOp::kSub, Expression::Literal(Value::Int64(0)),
                                  std::move(e));
    }
    return ParsePrimary();
}

Result<ExprPtr> Parser::ParseAggregate() {
    const Token& name_token = Cur();
    const AggregateKind kind = *AggregateNamed(name_token.text);
    Advance();
    Advance();  // '(', already confirmed by the caller

    ExprPtr argument;
    if (Cur().Is(TokenKind::kAsterisk)) {
        if (kind != AggregateKind::kCount) {
            return ErrorAt(Cur(), std::string(AggregateSpelling(kind)) +
                                      "(*) is not valid; only COUNT(*) is");
        }
        Advance();
        // COUNT(*) is a null argument rather than a kStar child: it counts rows,
        // not a column, and leaving the argument null means no downstream pass has
        // to special-case a star expression it cannot evaluate.
    } else {
        QUARRY_TRY(EnterDepth(name_token));
        Result<ExprPtr> inner = ParseExpression();
        LeaveDepth();
        if (!inner.ok()) return inner.status();
        argument = inner.take();
    }
    QUARRY_TRY(Consume(TokenKind::kRParen, std::string("')' to close ") +
                                               AggregateSpelling(kind)));
    return Expression::Aggregate(kind, std::move(argument));
}

Result<std::string> Parser::ParseQualifiedName() {
    Result<std::string> first = ConsumeIdentifier("a column name");
    if (!first.ok()) return first.status();
    std::string name = first.take();
    if (Match(TokenKind::kDot)) {
        Result<std::string> second = ConsumeIdentifier("a column name after '.'");
        if (!second.ok()) return second.status();
        name += "." + second.take();
    }
    return name;
}

Result<ExprPtr> Parser::ParsePrimary() {
    const Token& token = Cur();
    switch (token.kind) {
        case TokenKind::kInteger:
            Advance();
            return Expression::Literal(Value::Int64(token.int_value));
        case TokenKind::kDouble:
            Advance();
            return Expression::Literal(Value::Double(token.double_value));
        case TokenKind::kString: {
            // Deliberately not lowercased: case is data, unlike an identifier.
            std::string text = token.text;
            Advance();
            return Expression::Literal(Value::Varchar(std::move(text)));
        }
        case TokenKind::kLParen: {
            QUARRY_TRY(EnterDepth(token));
            Advance();
            Result<ExprPtr> inner = ParseExpression();
            LeaveDepth();
            if (!inner.ok()) return inner.status();
            QUARRY_TRY(Consume(TokenKind::kRParen, "')' to close a parenthesized expression"));
            return inner;
        }
        case TokenKind::kKeyword:
            if (token.IsKeyword("true") || token.IsKeyword("false")) {
                const bool v = token.IsKeyword("true");
                Advance();
                return Expression::Literal(Value::Boolean(v));
            }
            if (token.IsKeyword("null")) {
                Advance();
                return Expression::Literal(Value::Null());
            }
            return Expected(token, "an expression");
        case TokenKind::kIdentifier: {
            if (Peek(1).Is(TokenKind::kLParen) && AggregateNamed(token.text).has_value()) {
                return ParseAggregate();
            }
            if (Peek(1).Is(TokenKind::kLParen)) {
                return ErrorAt(token, "unknown function '" + token.text + "'");
            }
            Result<std::string> name = ParseQualifiedName();
            if (!name.ok()) return name.status();
            return Expression::ColumnRef(name.take());
        }
        default:
            return Expected(token, "an expression");
    }
}

}  // namespace

Result<ast::Statement> Parse(std::string_view sql) {
    Result<std::vector<Token>> tokens = Tokenize(sql);
    if (!tokens.ok()) return tokens.status();
    Parser parser(tokens.take());
    return parser.ParseStatement();
}

Result<ast::ExprPtr> ParseExpressionText(std::string_view text) {
    Result<std::vector<Token>> tokens = Tokenize(text);
    if (!tokens.ok()) return tokens.status();
    Parser parser(tokens.take());
    return parser.ParseWholeExpression();
}

}  // namespace quarry::parser
