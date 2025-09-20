#include "quarry/database.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../index/btree/btree.hpp"
#include "../index/lsm/lsm.hpp"
#include "../parser/parser.hpp"
#include "../storage/segment_writer.hpp"

namespace quarry {

// ---------------------------------------------------------------------------
// ResultSet
// ---------------------------------------------------------------------------

namespace {

// kFieldSeparator and the NULL rendering are the differential test's contract.
//
// The tests compare Quarry's text against SQLite's, so this format is chosen to
// be exactly what `sqlite3 -header -list -nullvalue NULL` emits: a header line of
// column names, one line per row, fields joined by a bare pipe with no padding.
// Padding or an aligned column layout would mean the comparison depended on the
// widest value in the result, which changes with the data.
//
// NULL renders as the four characters NULL rather than as an empty field, because
// an empty field is also what an empty string renders as, and a comparison that
// cannot tell those apart would pass on a query that confused them. Numbers come
// from Value::ToString, whose fixed six-digit double format exists for the same
// reason — the shortest round-trip form of a double differs between engines.
constexpr char kFieldSeparator = '|';

std::string RenderRow(const std::vector<Value>& row) {
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i) out.push_back(kFieldSeparator);
        out += row[i].ToString();
    }
    return out;
}

}  // namespace

std::string ResultSet::ToString() const {
    std::string out;

    for (size_t i = 0; i < schema.size(); ++i) {
        if (i) out.push_back(kFieldSeparator);
        out += schema.at(i).name;
    }
    out.push_back('\n');

    for (const auto& row : rows) {
        out += RenderRow(row);
        out.push_back('\n');
    }
    return out;
}

std::vector<std::string> ResultSet::SortedRowStrings() const {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const auto& row : rows) out.push_back(RenderRow(row));

    // Byte order, not value order. The point is a canonical sequence both engines
    // agree on, and comparing rendered rows is the only ordering available once
    // the rows are text; a numeric sort would have to parse them back and would
    // then disagree with SQLite's own sort for free.
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Pending inserts
// ---------------------------------------------------------------------------

namespace {

// Pending rows are buffered so that several small INSERTs become one segment.
//
// WHERE THIS LIVES, AND WHY IT IS UGLY: Database's members are fixed by the
// public header and there is no field to hold buffered rows in, so the buffer is
// kept in a side table keyed by the Database instance. The engine is
// single-threaded (no operator and no index takes a lock), so an unsynchronized
// map is sufficient; the entry is erased by Close and by the destructor. This is
// worked around rather than fixed because the header is frozen — it wants a
// member.
struct PendingRows {
    // Insertion order of tables is preserved so that a flush writes segments in
    // the order the data arrived, which keeps row ids (a row's ordinal position in
    // the table's segments) matching the order rows were inserted in.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::vector<Value>>> by_table;

    void Add(const std::string& table, std::vector<Value> row) {
        auto it = by_table.find(table);
        if (it == by_table.end()) {
            order.push_back(table);
            it = by_table.emplace(table, std::vector<std::vector<Value>>{}).first;
        }
        it->second.push_back(std::move(row));
    }

    bool empty() const { return by_table.empty(); }

    void Clear() {
        order.clear();
        by_table.clear();
    }
};

std::map<const Database*, PendingRows>& PendingRegistry() {
    static std::map<const Database*, PendingRows> registry;
    return registry;
}

PendingRows& PendingFor(const Database* db) { return PendingRegistry()[db]; }

// ---------------------------------------------------------------------------
// INSERT value handling
// ---------------------------------------------------------------------------

// EvalConstant folds the subset of expressions that can appear in a VALUES list.
//
// Deliberately not the full expression evaluator: a VALUES row has no input batch
// and therefore no columns, so anything that is not a constant is a bind error
// rather than something to defer to execution.
Result<Value> EvalConstant(const ast::Expression& expr) {
    switch (expr.kind) {
        case ast::ExprKind::kLiteral:
            return expr.literal;

        case ast::ExprKind::kBinary: {
            if (!expr.left || !expr.right) {
                return Status::Internal("binary expression with a missing side in VALUES");
            }
            auto left = EvalConstant(*expr.left);
            if (!left.ok()) return left.status();
            auto right = EvalConstant(*expr.right);
            if (!right.ok()) return right.status();

            const Value a = left.take();
            const Value b = right.take();

            switch (expr.op) {
                case ast::BinaryOp::kAdd:
                case ast::BinaryOp::kSub:
                case ast::BinaryOp::kMul:
                case ast::BinaryOp::kDiv:
                    break;
                default:
                    return Status::NotImplemented(
                        "only arithmetic is folded in VALUES, got " +
                        std::string(ast::BinaryOpName(expr.op)));
            }

            // Arithmetic on NULL is NULL, which is what lets `VALUES (1 + NULL)`
            // insert a null rather than fail.
            if (a.is_null() || b.is_null()) return Value::Null();
            if (!a.IsNumeric() || !b.IsNumeric()) {
                return Status::TypeError("cannot apply " + std::string(ast::BinaryOpName(expr.op)) +
                                         " to " + TypeName(a.type()) + " and " +
                                         TypeName(b.type()));
            }

            const bool integral = a.type() == TypeId::kInt64 && b.type() == TypeId::kInt64;
            if (integral && expr.op != ast::BinaryOp::kDiv) {
                const int64_t x = a.as_int64();
                const int64_t y = b.as_int64();
                switch (expr.op) {
                    case ast::BinaryOp::kAdd: return Value::Int64(x + y);
                    case ast::BinaryOp::kSub: return Value::Int64(x - y);
                    default:                  return Value::Int64(x * y);
                }
            }

            const double x = a.AsNumeric();
            const double y = b.AsNumeric();
            switch (expr.op) {
                case ast::BinaryOp::kAdd: return Value::Double(x + y);
                case ast::BinaryOp::kSub: return Value::Double(x - y);
                case ast::BinaryOp::kMul: return Value::Double(x * y);
                default:
                    if (y == 0.0) return Status::InvalidArgument("division by zero in VALUES");
                    // Integer division stays integral, matching what the engine
                    // does elsewhere; mixed operands promote to double.
                    if (integral) {
                        return Value::Int64(a.as_int64() / b.as_int64());
                    }
                    return Value::Double(x / y);
            }
        }

        case ast::ExprKind::kColumnRef:
            return Status::BindError("VALUES may not refer to column " + expr.column_name);
        case ast::ExprKind::kAggregate:
            return Status::BindError("VALUES may not contain an aggregate");
        case ast::ExprKind::kStar:
            return Status::BindError("VALUES may not contain *");
    }
    return Status::Internal("unhandled expression kind in VALUES");
}

// Coerce checks one value against its column and applies the single implicit
// conversion the engine allows. Rejecting here rather than at write time means a
// bad INSERT fails before any segment bytes exist.
Result<Value> Coerce(const Column& col, Value v) {
    if (v.is_null()) {
        if (!col.nullable) {
            return Status::TypeError("column " + col.name + " is NOT NULL");
        }
        return v;
    }
    if (v.type() == col.type) return v;

    if (col.type == TypeId::kDouble && v.type() == TypeId::kInt64) {
        return Value::Double(static_cast<double>(v.as_int64()));
    }
    if (col.type == TypeId::kInt64 && v.type() == TypeId::kDouble) {
        const double d = v.as_double();
        const int64_t rounded = static_cast<int64_t>(d);
        // Only an exact integer is accepted. Truncating silently would make
        // `INSERT ... VALUES (1.5)` into an int column look like it stored 1.5.
        if (static_cast<double>(rounded) != d) {
            return Status::TypeError("cannot store " + v.ToString() + " in int64 column " +
                                     col.name + " without losing precision");
        }
        return Value::Int64(rounded);
    }

    return Status::TypeError("cannot store a " + std::string(TypeName(v.type())) + " in column " +
                             col.name + " of type " + TypeName(col.type));
}

void AppendValue(Vector& vec, const Value& v) {
    if (v.is_null()) {
        vec.AppendNull();
        return;
    }
    switch (vec.type()) {
        case TypeId::kBoolean: vec.AppendBoolean(v.as_boolean()); break;
        case TypeId::kInt64:   vec.AppendInt64(v.as_int64()); break;
        case TypeId::kDouble:  vec.AppendDouble(v.as_double()); break;
        case TypeId::kVarchar: vec.AppendString(v.as_varchar()); break;
        default:               vec.AppendNull(); break;
    }
}

// NextSegmentId derives the next free id by looking at what the catalog already
// holds.
//
// The catalog owns a next_segment_id counter but does not expose it, so it is
// recomputed here. Correct because AddSegment refuses a duplicate id, so the worst
// a stale value could do is be rejected rather than overwrite a segment file.
uint64_t NextSegmentId(const Catalog& catalog) {
    uint64_t next = 1;
    for (const auto& name : catalog.TableNames()) {
        auto found = catalog.GetTable(name);
        if (!found.ok()) continue;
        for (const auto& seg : found.value()->segments) {
            next = std::max(next, seg.id + 1);
        }
    }
    return next;
}

// WriteSegment turns a block of rows into one segment file and its metadata.
Result<SegmentMeta> WriteSegment(const Catalog& catalog, const std::string& table,
                                 const Schema& schema, uint64_t segment_id,
                                 const std::vector<std::vector<Value>>& rows, size_t begin,
                                 size_t end) {
    const std::string path = catalog.SegmentPath(table, segment_id);
    FileSegmentWriter writer(path, schema, segment_id);

    Batch batch(schema);
    size_t at = begin;
    while (at < end) {
        const size_t take = std::min(kBatchSize, end - at);
        batch.Reset();
        for (size_t c = 0; c < schema.size(); ++c) {
            Vector& vec = batch.column(c);
            for (size_t r = 0; r < take; ++r) {
                AppendValue(vec, rows[at + r][c]);
            }
        }
        batch.SetRowCount(take);
        QUARRY_TRY(writer.Append(batch));
        at += take;
    }

    return writer.Finish();
}

// ---------------------------------------------------------------------------
// Statement helpers
//
// Free functions rather than Database methods: the public header fixes
// Database's interface and nothing private can be added to it. Each takes
// exactly the state it needs, which at least makes the dependencies explicit
// instead of reaching back through the object.
// ---------------------------------------------------------------------------

Status FlushPending(const Database* db, Catalog& catalog, const Database::Options& options) {
    PendingRows& pending = PendingFor(db);
    if (pending.empty()) return Status::Ok();

    const size_t rows_per_segment = options.rows_per_segment == 0 ? 1 : options.rows_per_segment;

    for (const auto& table : pending.order) {
        const auto it = pending.by_table.find(table);
        if (it == pending.by_table.end() || it->second.empty()) continue;

        auto found = catalog.GetTable(table);
        if (!found.ok()) return found.status();
        const Schema schema = found.value()->schema;
        const std::vector<std::vector<Value>>& rows = it->second;

        for (size_t begin = 0; begin < rows.size(); begin += rows_per_segment) {
            const size_t end = std::min(rows.size(), begin + rows_per_segment);
            const uint64_t id = NextSegmentId(catalog);
            auto meta = WriteSegment(catalog, table, schema, id, rows, begin, end);
            if (!meta.ok()) return meta.status();
            QUARRY_TRY(catalog.AddSegment(table, meta.take()));
        }
    }

    pending.Clear();

    // Saved immediately. A segment file whose metadata is not in the catalog is
    // unreachable data, and the window where that is true should be no longer
    // than the write itself.
    return catalog.Save();
}

Status RunInsert(const Database* db, Catalog& catalog, const Database::Options& options,
                 const ast::InsertStatement& stmt) {
    auto found = catalog.GetTable(stmt.table);
    if (!found.ok()) return found.status();
    const Schema schema = found.value()->schema;

    // The column list is resolved once, not per row.
    std::vector<size_t> targets;
    if (stmt.columns.empty()) {
        targets.resize(schema.size());
        for (size_t i = 0; i < schema.size(); ++i) targets[i] = i;
    } else {
        targets.reserve(stmt.columns.size());
        for (const auto& name : stmt.columns) {
            const auto at = schema.IndexOf(name);
            if (!at) {
                return Status::BindError("table " + stmt.table + " has no column " + name +
                                         "; it has " + schema.ToString());
            }
            if (std::find(targets.begin(), targets.end(), *at) != targets.end()) {
                return Status::BindError("column " + name + " appears twice in INSERT");
            }
            targets.push_back(*at);
        }
        // A column left out of the list gets NULL, so one that cannot hold NULL
        // has to be named. Checked before any row is evaluated.
        for (size_t i = 0; i < schema.size(); ++i) {
            if (schema.at(i).nullable) continue;
            if (std::find(targets.begin(), targets.end(), i) == targets.end()) {
                return Status::TypeError("column " + schema.at(i).name +
                                         " is NOT NULL and is not in the INSERT column list");
            }
        }
    }

    std::vector<std::vector<Value>> staged;
    staged.reserve(stmt.rows.size());

    for (const auto& row : stmt.rows) {
        if (row.size() != targets.size()) {
            return Status::BindError("INSERT has " + std::to_string(row.size()) +
                                     " value(s) for " + std::to_string(targets.size()) +
                                     " column(s)");
        }

        std::vector<Value> out(schema.size());  // default-constructed Value is NULL
        for (size_t i = 0; i < row.size(); ++i) {
            if (!row[i]) return Status::Internal("INSERT value expression is null");
            auto value = EvalConstant(*row[i]);
            if (!value.ok()) return value.status();
            auto coerced = Coerce(schema.at(targets[i]), value.take());
            if (!coerced.ok()) return coerced.status();
            out[targets[i]] = coerced.take();
        }
        staged.push_back(std::move(out));
    }

    // Staged first and committed second, so a statement that fails partway through
    // its VALUES list inserts nothing at all.
    PendingRows& pending = PendingFor(db);
    for (auto& row : staged) pending.Add(stmt.table, std::move(row));

    // Rows are buffered across statements so that segments come out the configured
    // size instead of one segment per INSERT: a table loaded by a thousand small
    // statements would otherwise get a thousand tiny segments, which inflates the
    // catalog and leaves zone-map pruning nothing to prune. The buffer is drained
    // when it holds a full segment, when a read-path statement needs to see the
    // rows, and on Close.
    const size_t rows_per_segment = options.rows_per_segment == 0 ? 1 : options.rows_per_segment;
    const auto it = pending.by_table.find(stmt.table);
    if (it != pending.by_table.end() && it->second.size() >= rows_per_segment) {
        QUARRY_TRY(FlushPending(db, catalog, options));
    }
    return Status::Ok();
}

Result<LogicalPtr> PlanSelect(const Catalog& catalog, const ast::SelectStatement& stmt,
                              const Optimizer::Rules& rules) {
    Binder binder(catalog);
    auto bound = binder.BindSelect(stmt);
    if (!bound.ok()) return bound.status();

    Optimizer optimizer(rules);
    return optimizer.Optimize(bound.take());
}

Status Materialize(PhysicalOperator& root, ResultSet& out) {
    QUARRY_TRY(root.Open());

    Batch batch(root.output_schema());
    Status run = Status::Ok();
    for (;;) {
        batch.Reset();
        bool done = false;
        run = root.Next(batch, done);
        if (!run.ok()) break;
        if (done) break;

        // Rows(), never a dense 0..row_count() loop: a filter leaves rejected rows
        // physically present in the vectors, and iterating them densely would
        // resurrect them into the result.
        for (const uint32_t r : batch.Rows()) {
            std::vector<Value> row;
            row.reserve(batch.column_count());
            for (size_t c = 0; c < batch.column_count(); ++c) {
                row.push_back(batch.column(c).GetValue(r));
            }
            out.rows.push_back(std::move(row));
        }
    }

    // Closed even on failure: Close is what releases descriptors and unpins pages,
    // and a failed query that left pages pinned would leave the pool unable to
    // find a victim for the next one.
    const Status closed = root.Close();
    return run.ok() ? closed : run;
}

Result<ResultSet> RunSelect(Catalog& catalog, BufferPool* pool,
                            const Database::Options& options,
                            const ast::SelectStatement& stmt) {
    auto planned = PlanSelect(catalog, stmt, options.rules);
    if (!planned.ok()) return planned.status();
    const LogicalPtr plan = planned.take();

    PhysicalPlanner planner(catalog, pool);
    auto built = planner.Build(*plan);
    if (!built.ok()) return built.status();
    const OperatorPtr root = built.take();

    ResultSet out;
    out.schema = root->output_schema();
    QUARRY_TRY(Materialize(*root, out));
    return out;
}

Result<std::string> ExplainText(const Database* db, Catalog& catalog, BufferPool* pool,
                                const Database::Options& options,
                                const ast::SelectStatement& stmt, bool analyze) {
    // Buffered rows are flushed first even though EXPLAIN reads nothing: the plan
    // is costed from segment statistics, so a plan produced while rows sat in the
    // buffer would describe a smaller table than the one the query would run on.
    QUARRY_TRY(FlushPending(db, catalog, options));

    auto planned = PlanSelect(catalog, stmt, options.rules);
    if (!planned.ok()) return planned.status();
    const LogicalPtr plan = planned.take();

    if (!analyze) return Explain(*plan, false);

    PhysicalPlanner planner(catalog, pool);
    auto built = planner.Build(*plan);
    if (!built.ok()) return built.status();
    const OperatorPtr root = built.take();

    // The result is deliberately discarded: ANALYZE is about the counters the run
    // leaves behind, and materializing is the only way to be sure the plan ran to
    // completion rather than stopping where a caller stopped reading.
    ResultSet sink;
    QUARRY_TRY(Materialize(*root, sink));
    return ExplainPhysical(*root, true);
}

// ParseExplainTarget accepts a bare SELECT or an EXPLAIN wrapped around one, so
// ExplainQuery("select ...") and ExplainQuery("explain select ...") both work.
// Insisting on one spelling would only make the shell and the tests disagree
// about which.
Result<std::unique_ptr<ast::SelectStatement>> ParseExplainTarget(const std::string& sql) {
    auto parsed = parser::Parse(sql);
    if (!parsed.ok()) return parsed.status();
    ast::Statement stmt = parsed.take();

    if (stmt.kind == ast::StatementKind::kSelect && stmt.select) return std::move(stmt.select);
    if (stmt.kind == ast::StatementKind::kExplain && stmt.explain_select) {
        return std::move(stmt.explain_select);
    }
    return Status::InvalidArgument("EXPLAIN needs a SELECT, got: " + sql);
}

Status PopulateIndex(PhysicalOperator& root, Index& index) {
    QUARRY_TRY(root.Open());

    uint64_t row_id = 0;
    Batch batch(root.output_schema());
    Status run = Status::Ok();
    for (;;) {
        batch.Reset();
        bool done = false;
        run = root.Next(batch, done);
        if (!run.ok()) break;
        if (done) break;

        const Vector& keys = batch.column(0);
        for (const uint32_t r : batch.Rows()) {
            const Value key = keys.GetValue(r);
            // A null key is counted but not stored. Nothing could look it up:
            // `WHERE c = NULL` is unknown for every row, so an entry under a null
            // key could only be reached by a range scan, which would then return
            // rows the equivalent comparison rejects.
            if (!key.is_null()) {
                const Status inserted = index.Insert(key, row_id);
                if (!inserted.ok()) {
                    run = inserted;
                    break;
                }
            }
            ++row_id;
        }
        if (!run.ok()) break;
    }

    const Status closed = root.Close();
    return run.ok() ? closed : run;
}

Status RunCreateIndex(const Database* db, Catalog& catalog, BufferPool* pool,
                      const Database::Options& options, const ast::CreateIndexStatement& stmt) {
    // Buffered rows have to be in segments first: an index built over only the
    // flushed rows would be missing entries and would then silently return fewer
    // rows than a sequential scan of the same predicate.
    QUARRY_TRY(FlushPending(db, catalog, options));

    auto found = catalog.GetTable(stmt.table);
    if (!found.ok()) return found.status();
    const TableDef& def = *found.value();

    const auto column = def.schema.IndexOf(stmt.column);
    if (!column) {
        return Status::BindError("table " + stmt.table + " has no column " + stmt.column +
                                 "; it has " + def.schema.ToString());
    }
    for (const auto& existing : def.indexes) {
        if (existing.name == stmt.name) {
            return Status::InvalidArgument("index " + stmt.name + " already exists on table " +
                                           stmt.table);
        }
    }

    IndexDef idx;
    idx.name = stmt.name;
    idx.column_index = *column;
    idx.kind = stmt.lsm ? IndexKind::kLsm : IndexKind::kBTree;
    idx.path = catalog.IndexPath(stmt.table, stmt.name);

    const TypeId key_type = def.schema.at(*column).type;
    const std::string table = stmt.table;

    // A row id is a row's ordinal position across the table's segments in catalog
    // order, which is exactly the order a sequential scan produces. That is the
    // definition the scan and the index share, and it is why index selection is
    // switched off for the scan below: an index-driven scan would emit rows in key
    // order and the ordinals would then name different rows.
    Optimizer::Rules rules = options.rules;
    rules.index_selection = false;

    ast::SelectStatement scan;
    scan.from_table = table;
    scan.projections.push_back(ast::Expression::ColumnRef(stmt.column));

    auto planned = PlanSelect(catalog, scan, rules);
    if (!planned.ok()) return planned.status();
    const LogicalPtr plan = planned.take();

    PhysicalPlanner planner(catalog, pool);
    auto built = planner.Build(*plan);
    if (!built.ok()) return built.status();
    const OperatorPtr root = built.take();

    if (idx.kind == IndexKind::kBTree) {
        // A pool of its own over the index's own file. The B+ tree's meta page is
        // at a fixed page id, so two trees sharing one file would each adopt the
        // other's root.
        BufferPool index_pool(idx.path, options.buffer_frames, options.buffer_policy);
        QUARRY_TRY(index_pool.Open());
        BTree tree(&index_pool, key_type);
        QUARRY_TRY(tree.Open());
        QUARRY_TRY(PopulateIndex(*root, tree));
        QUARRY_TRY(tree.Flush());
        QUARRY_TRY(index_pool.FlushAll());
        QUARRY_TRY(index_pool.Close());
    } else {
        auto opened = lsm::LsmIndex::Open(idx.path);
        if (!opened.ok()) return opened.status();
        const std::unique_ptr<lsm::LsmIndex> index = opened.take();
        QUARRY_TRY(PopulateIndex(*root, *index));
        QUARRY_TRY(index->Flush());
    }

    QUARRY_TRY(catalog.AddIndex(table, std::move(idx)));
    return catalog.Save();
}

}  // namespace

// ---------------------------------------------------------------------------
// Database lifecycle
// ---------------------------------------------------------------------------

Database::Database(std::string directory) : directory_(std::move(directory)) {}

Database::Database(std::string directory, Options options)
    : directory_(std::move(directory)), options_(std::move(options)) {}

Database::~Database() {
    if (open_) {
        // A destructor cannot report, but the alternative to trying is losing the
        // final partial segment outright, which is worse than an unreported error.
        Close();
    }
    PendingRegistry().erase(this);
}

Status Database::Open() {
    if (open_) return Status::InvalidArgument("database " + directory_ + " is already open");

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec && !std::filesystem::is_directory(directory_)) {
        return Status::IoError("create directory " + directory_ + ": " + ec.message());
    }

    catalog_ = std::make_unique<Catalog>(directory_);
    const Status catalog_status = catalog_->Open();
    if (!catalog_status.ok()) {
        // Nothing half-constructed is left behind: a Database whose catalog failed
        // to load must not be usable, because every path below trusts the catalog.
        catalog_.reset();
        return catalog_status;
    }

    // One pool over one file, which is what BufferPool is — it owns a descriptor
    // and hands out pages from it. Index B+ trees get their own pool over their
    // own file; see RunCreateIndex.
    pool_ = std::make_unique<BufferPool>(
        (std::filesystem::path(directory_) / "pages.qdb").string(), options_.buffer_frames,
        options_.buffer_policy);
    const Status pool_status = pool_->Open();
    if (!pool_status.ok()) {
        pool_.reset();
        catalog_.reset();
        return pool_status;
    }

    PendingFor(this).Clear();
    open_ = true;
    return Status::Ok();
}

Status Database::Close() {
    if (!open_) return Status::Ok();

    // The first failure is remembered but the rest of the shutdown still runs: a
    // failed flush must not also leave the pool's descriptor open and the catalog
    // unsaved.
    Status first = FlushPending(this, *catalog_, options_);

    const Status flushed = pool_->FlushAll();
    if (first.ok()) first = flushed;
    const Status closed = pool_->Close();
    if (first.ok()) first = closed;

    const Status saved = catalog_->Save();
    if (first.ok()) first = saved;

    PendingRegistry().erase(this);
    pool_.reset();
    catalog_.reset();
    open_ = false;
    return first;
}

// ---------------------------------------------------------------------------
// Database::Execute
// ---------------------------------------------------------------------------

Result<ResultSet> Database::Execute(const std::string& sql) {
    if (!open_) return Status::InvalidArgument("database is not open");

    auto parsed = parser::Parse(sql);
    if (!parsed.ok()) return parsed.status();
    const ast::Statement stmt = parsed.take();

    switch (stmt.kind) {
        case ast::StatementKind::kCreateTable: {
            if (!stmt.create_table) return Status::Internal("CREATE TABLE with no payload");
            QUARRY_TRY(catalog_->CreateTable(stmt.create_table->table, stmt.create_table->schema));
            QUARRY_TRY(catalog_->Save());
            return ResultSet{};
        }

        case ast::StatementKind::kInsert: {
            if (!stmt.insert) return Status::Internal("INSERT with no payload");
            QUARRY_TRY(RunInsert(this, *catalog_, options_, *stmt.insert));
            return ResultSet{};
        }

        case ast::StatementKind::kCreateIndex: {
            if (!stmt.create_index) return Status::Internal("CREATE INDEX with no payload");
            QUARRY_TRY(RunCreateIndex(this, *catalog_, pool_.get(), options_, *stmt.create_index));
            return ResultSet{};
        }

        case ast::StatementKind::kSelect: {
            if (!stmt.select) return Status::Internal("SELECT with no payload");
            QUARRY_TRY(FlushPending(this, *catalog_, options_));
            return RunSelect(*catalog_, pool_.get(), options_, *stmt.select);
        }

        case ast::StatementKind::kExplain: {
            if (!stmt.explain_select) return Status::Internal("EXPLAIN with no payload");
            auto text = ExplainText(this, *catalog_, pool_.get(), options_, *stmt.explain_select,
                                    stmt.analyze);
            if (!text.ok()) return text.status();

            // One row per line of plan text. A single row holding embedded newlines
            // prints the same through ToString, but it would make a test looking
            // for an operator name search inside one cell.
            ResultSet out;
            out.schema.Add(Column("plan", TypeId::kVarchar, false));
            std::istringstream lines(text.take());
            std::string line;
            while (std::getline(lines, line)) {
                out.rows.push_back({Value::Varchar(line)});
            }
            return out;
        }
    }
    return Status::Internal("unhandled statement kind");
}

Result<std::string> Database::ExplainQuery(const std::string& sql) {
    if (!open_) return Status::InvalidArgument("database is not open");
    auto select = ParseExplainTarget(sql);
    if (!select.ok()) return select.status();
    const std::unique_ptr<ast::SelectStatement> stmt = select.take();
    return ExplainText(this, *catalog_, pool_.get(), options_, *stmt, false);
}

Result<std::string> Database::ExplainAnalyze(const std::string& sql) {
    if (!open_) return Status::InvalidArgument("database is not open");
    auto select = ParseExplainTarget(sql);
    if (!select.ok()) return select.status();
    const std::unique_ptr<ast::SelectStatement> stmt = select.take();
    return ExplainText(this, *catalog_, pool_.get(), options_, *stmt, true);
}

}  // namespace quarry
