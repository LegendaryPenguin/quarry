// quarry_shell: an interactive and scriptable front end for the engine.
//
// The shell exists for two reasons beyond convenience. First, it is how a reader
// confirms the project does what the README says without writing C++. Second, the
// dot-commands expose the things a columnar engine should be judged on — which
// segments were skipped, how the plan was rewritten, how far the optimizer's row
// estimate was from the truth — and those are invisible from a plain result table.
//
// Everything here goes through the public Database interface. The shell gets no
// privileged access to engine internals, so anything it can show, a user can get.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "quarry/database.hpp"
#include "quarry/status.hpp"

namespace {

using quarry::Database;
using quarry::ResultSet;
using quarry::Status;

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string Trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool StartsWithWord(const std::string& sql, const char* word) {
    std::string lowered = ToLower(Trim(sql));
    size_t n = std::strlen(word);
    if (lowered.size() < n) return false;
    if (lowered.compare(0, n, word) != 0) return false;
    return lowered.size() == n || !(std::isalnum(static_cast<unsigned char>(lowered[n])) ||
                                    lowered[n] == '_');
}

// Column width is computed over the rendered text, which is why display width and
// byte length are treated as the same thing here: the engine's four types never
// produce multi-byte output, and pretending otherwise would be a width calculation
// nothing in this project can exercise.
size_t DisplayWidth(const std::string& s) { return s.size(); }

void PadTo(std::string& out, const std::string& cell, size_t width, bool right_align) {
    size_t pad = width > DisplayWidth(cell) ? width - DisplayWidth(cell) : 0;
    if (right_align) {
        out.append(pad, ' ');
        out += cell;
    } else {
        out += cell;
        out.append(pad, ' ');
    }
}

// ---------------------------------------------------------------------------
// Statement splitting
// ---------------------------------------------------------------------------

// FindStatementEnd returns the index of the semicolon that terminates the first
// statement in `text`, or npos if the text so far is an incomplete statement.
//
// The scan is quote- and comment-aware. A semicolon inside a string literal does
// not end a statement, and getting that wrong would make
// `INSERT ... VALUES ('a;b')` unusable from a script. An unterminated literal
// reports npos, which in the REPL means "keep reading" — the correct behaviour for
// a multi-line string and a harmless one for a typo, since the next line either
// closes it or the user interrupts.
size_t FindStatementEnd(const std::string& text) {
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_string) {
            if (c == '\'') {
                // A doubled quote is an escaped quote, not the end of the literal.
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    ++i;
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '\'') {
            in_string = true;
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            size_t newline = text.find('\n', i);
            if (newline == std::string::npos) return std::string::npos;
            i = newline;
            continue;
        }
        if (c == ';') return i;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Result rendering
// ---------------------------------------------------------------------------

bool RightAligned(quarry::TypeId type) {
    return type == quarry::TypeId::kInt64 || type == quarry::TypeId::kDouble;
}

void PrintResult(const ResultSet& result, std::ostream& out) {
    const quarry::Schema& schema = result.schema;
    size_t columns = schema.size();
    if (columns == 0) return;

    std::vector<std::string> headers;
    std::vector<size_t> widths;
    headers.reserve(columns);
    widths.reserve(columns);
    for (size_t c = 0; c < columns; ++c) {
        headers.push_back(schema.at(c).name);
        widths.push_back(DisplayWidth(headers.back()));
    }

    // Two passes: one to measure, one to print. A streaming printer would have to
    // guess widths and would misalign exactly when the data is interesting.
    std::vector<std::vector<std::string>> cells;
    cells.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        std::vector<std::string> rendered;
        rendered.reserve(columns);
        for (size_t c = 0; c < columns; ++c) {
            rendered.push_back(c < row.size() ? row[c].ToString() : "");
            widths[c] = std::max(widths[c], DisplayWidth(rendered.back()));
        }
        cells.push_back(std::move(rendered));
    }

    std::string line;
    for (size_t c = 0; c < columns; ++c) {
        if (c != 0) line += " | ";
        PadTo(line, headers[c], widths[c], false);
    }
    out << line << "\n";

    line.clear();
    for (size_t c = 0; c < columns; ++c) {
        if (c != 0) line += "-+-";
        line.append(widths[c], '-');
    }
    out << line << "\n";

    for (const auto& row : cells) {
        line.clear();
        for (size_t c = 0; c < columns; ++c) {
            if (c != 0) line += " | ";
            PadTo(line, row[c], widths[c], RightAligned(schema.at(c).type));
        }
        out << line << "\n";
    }
}

// ---------------------------------------------------------------------------
// Shell
// ---------------------------------------------------------------------------

struct Options {
    std::string directory = "quarry-data";
    std::vector<std::string> scripts;   // -f
    std::vector<std::string> one_shots; // -e
    bool interactive = true;
    bool timer = true;
    Database::Options db;
};

class Shell {
public:
    Shell(Database& db, Options options) : db_(db), options_(std::move(options)) {}

    bool failed() const { return failed_; }

    // Feed appends input and runs whatever it now contains, consuming dot-commands
    // and SQL statements strictly in the order they appear. Interleaving matters:
    // a script that runs a query and then asks for .stats is asking about that
    // query, so processing all the SQL first and the dot-commands afterwards would
    // answer a different question.
    //
    // Returns false once .quit has been seen.
    bool Feed(const std::string& text) {
        buffer_ += text;
        if (!buffer_.empty() && buffer_.back() != '\n') buffer_ += '\n';

        while (true) {
            size_t first = buffer_.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                buffer_.clear();
                // Nothing left to run, so the .read nesting guard starts over.
                reads_since_idle_ = 0;
                return true;
            }
            buffer_.erase(0, first);

            if (buffer_[0] == '.') {
                // A dot-command is a whole line and is never part of a statement,
                // so it is delimited by a newline rather than a semicolon.
                size_t newline = buffer_.find('\n');
                if (newline == std::string::npos) return true;  // wait for the rest
                std::string line = Trim(buffer_.substr(0, newline));
                buffer_.erase(0, newline + 1);
                if (line.empty()) continue;
                if (!RunDotCommand(line)) return false;
                continue;
            }

            size_t end = FindStatementEnd(buffer_);
            if (end == std::string::npos) return true;  // incomplete; wait for more
            std::string statement = Trim(buffer_.substr(0, end));
            buffer_.erase(0, end + 1);
            if (!statement.empty()) RunStatement(statement);
        }
    }

    bool buffer_pending() const { return !Trim(buffer_).empty(); }

    void RunStatement(const std::string& sql) {
        auto started = std::chrono::steady_clock::now();

        if (StartsWithWord(sql, "select")) last_query_ = sql;

        auto result = db_.Execute(sql);
        double millis = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();

        if (!result.ok()) {
            ReportError(result.status());
            return;
        }

        const ResultSet& rows = result.value();
        if (rows.schema.size() != 0) {
            PrintResult(rows, std::cout);
            std::cout << "(" << rows.row_count() << (rows.row_count() == 1 ? " row" : " rows")
                      << ")";
        } else {
            std::cout << "OK";
        }
        if (options_.timer) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  %.3f ms", millis);
            std::cout << buf;
        }
        std::cout << "\n\n";
    }

    void PrintHelp(std::ostream& out) {
        out << "Dot-commands:\n"
               "  .tables                  list tables\n"
               "  .schema [table]          show column definitions\n"
               "  .explain <sql>           show the optimized plan without running it\n"
               "  .analyze <sql>           run the query and print estimated vs actual\n"
               "  .stats                   buffer pool hit rate, segments scanned/pruned\n"
               "  .timer on|off            show or hide statement timing\n"
               "  .read <file>             execute a SQL script\n"
               "  .help                    this list\n"
               "  .quit                    leave the shell\n"
               "\n"
               "SQL statements end with a semicolon and may span lines.\n";
    }

private:
    void ReportError(const Status& status) {
        // The Status message is printed and the shell keeps going. A query engine
        // whose front end exits on a typo is unusable, and the error text is the
        // product here: if it is not enough to act on, that is a defect in the
        // engine's messages, not something the shell should paper over.
        std::cout << "Error: " << status.ToString() << "\n\n";
        failed_ = true;
    }

    bool RunDotCommand(const std::string& line) {
        std::string command = line;
        std::string argument;
        size_t space = line.find_first_of(" \t");
        if (space != std::string::npos) {
            command = line.substr(0, space);
            argument = Trim(line.substr(space + 1));
        }
        command = ToLower(command);

        if (command == ".quit" || command == ".exit") return false;

        if (command == ".help") {
            PrintHelp(std::cout);
            std::cout << "\n";
            return true;
        }

        if (command == ".timer") {
            std::string value = ToLower(argument);
            if (value == "on") {
                options_.timer = true;
            } else if (value == "off") {
                options_.timer = false;
            } else {
                std::cout << "Error: .timer expects on or off\n\n";
                failed_ = true;
                return true;
            }
            std::cout << "timer " << (options_.timer ? "on" : "off") << "\n\n";
            return true;
        }

        if (command == ".tables") {
            std::vector<std::string> names = db_.catalog().TableNames();
            if (names.empty()) {
                std::cout << "(no tables)\n\n";
                return true;
            }
            for (const std::string& name : names) std::cout << name << "\n";
            std::cout << "\n";
            return true;
        }

        if (command == ".schema") {
            std::vector<std::string> names =
                argument.empty() ? db_.catalog().TableNames()
                                 : std::vector<std::string>{ToLower(argument)};
            for (const std::string& name : names) {
                auto table = db_.catalog().GetTable(name);
                if (!table.ok()) {
                    ReportError(table.status());
                    continue;
                }
                const quarry::TableDef* def = table.value();
                std::cout << "TABLE " << def->name << " (" << def->row_count() << " rows, "
                          << def->segments.size() << " segments)\n";
                for (const quarry::Column& col : def->schema.columns()) {
                    std::cout << "  " << col.name << " " << quarry::TypeName(col.type)
                              << (col.nullable ? "" : " NOT NULL") << "\n";
                }
                for (const quarry::IndexDef& index : def->indexes) {
                    std::cout << "  INDEX " << index.name << " on "
                              << def->schema.at(index.column_index).name << " ("
                              << quarry::IndexKindName(index.kind) << ")\n";
                }
                std::cout << "\n";
            }
            return true;
        }

        if (command == ".explain" || command == ".analyze") {
            if (argument.empty()) {
                std::cout << "Error: " << command << " expects a SQL statement\n\n";
                failed_ = true;
                return true;
            }
            if (!argument.empty() && argument.back() == ';') argument.pop_back();
            last_query_ = argument;

            auto started = std::chrono::steady_clock::now();
            auto plan = command == ".explain" ? db_.ExplainQuery(argument)
                                              : db_.ExplainAnalyze(argument);
            double millis = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
            if (!plan.ok()) {
                ReportError(plan.status());
                return true;
            }
            std::cout << plan.value();
            if (plan.value().empty() || plan.value().back() != '\n') std::cout << "\n";
            if (options_.timer) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.3f ms\n", millis);
                std::cout << buf;
            }
            std::cout << "\n";
            return true;
        }

        if (command == ".stats") {
            PrintStats();
            return true;
        }

        if (command == ".read") {
            if (argument.empty()) {
                std::cout << "Error: .read expects a file name\n\n";
                failed_ = true;
                return true;
            }
            std::ifstream file(argument);
            if (!file) {
                std::cout << "Error: cannot read " << argument << "\n\n";
                failed_ = true;
                return true;
            }
            std::ostringstream contents;
            contents << file.rdbuf();

            // The script is PREPENDED to the pending input, not appended. Appending
            // would run whatever already followed the .read line before the file it
            // named, which is the wrong order and silently so.
            if (++reads_since_idle_ > kMaxNestedReads) {
                std::cout << "Error: too many nested .read commands (" << kMaxNestedReads
                          << "); does a script read itself?\n\n";
                failed_ = true;
                reads_since_idle_ = 0;
                buffer_.clear();
                return true;
            }
            std::string text = contents.str();
            if (!text.empty() && text.back() != '\n') text += '\n';
            buffer_.insert(0, text);
            return true;
        }

        std::cout << "Error: unknown command " << command << " (try .help)\n\n";
        failed_ = true;
        return true;
    }

    void PrintStats() {
        // Buffer pool counters are read before anything else runs, because the
        // segment counters below need to execute a query and would otherwise be
        // reporting on their own measurement.
        quarry::BufferPool::Stats pool = db_.buffer_pool().stats();
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "buffer pool: %llu hits, %llu misses, hit rate %.1f%%\n"
                      "             %llu evictions (%llu dirty), %llu reads, %llu writes\n",
                      static_cast<unsigned long long>(pool.hits),
                      static_cast<unsigned long long>(pool.misses), pool.HitRate() * 100.0,
                      static_cast<unsigned long long>(pool.evictions),
                      static_cast<unsigned long long>(pool.dirty_evictions),
                      static_cast<unsigned long long>(pool.reads),
                      static_cast<unsigned long long>(pool.writes));
        std::cout << buf;

        if (last_query_.empty()) {
            std::cout << "segments:    no query has run yet\n\n";
            return;
        }

        // Segment counters live on the physical operators, which the public
        // Database interface does not hand out. EXPLAIN ANALYZE does report them,
        // so they are read back from its text. The query is re-executed to produce
        // that text, which is why the pool numbers above were captured first.
        auto analyzed = db_.ExplainAnalyze(last_query_);
        if (!analyzed.ok()) {
            std::cout << "segments:    unavailable (" << analyzed.status().ToString() << ")\n\n";
            return;
        }

        // Four numbers from two sources, because they can legitimately disagree
        // and collapsing them would hide the most interesting case. The optimizer
        // drops segments statically from the plan using the catalog's zone maps,
        // so those segments are never opened and the scan's own pruning counter
        // never sees them. Reporting only the scan-time counter would make static
        // pruning look like it never happened.
        long long kept = -1;
        long long static_pruned = -1;
        auto planned = db_.ExplainQuery(last_query_);
        if (planned.ok()) {
            kept = ScrapeCounter(planned.value(), "segments");
            static_pruned = ScrapeCounter(planned.value(), "pruned");
        }
        long long scanned = ScrapeCounter(analyzed.value(), "segments_scanned");
        long long runtime_pruned = ScrapeCounter(analyzed.value(), "segments_pruned");

        if (kept < 0 && scanned < 0) {
            std::cout << "segments:    not reported by EXPLAIN for this plan\n";
        } else {
            std::cout << "segments:    plan kept " << (kept < 0 ? 0 : kept) << ", pruned "
                      << (static_pruned < 0 ? 0 : static_pruned)
                      << " by zone map before the scan ran\n";
            std::cout << "             scan read " << (scanned < 0 ? 0 : scanned) << ", pruned "
                      << (runtime_pruned < 0 ? 0 : runtime_pruned) << " more while scanning\n";
        }
        std::cout << "             from EXPLAIN ANALYZE of: " << last_query_ << "\n\n";
    }

    // ScrapeCounter sums the integers attached to a named counter across every
    // line of EXPLAIN ANALYZE output, so a plan with several scans reports the
    // total. Returns -1 when the counter never appears, which is how "this plan
    // does not report it" stays distinguishable from "the count is zero".
    //
    // Reading a counter back out of formatted text is not how this should work.
    // The numbers live on PhysicalOperator::Stats, which the public Database
    // interface does not expose, so the alternative is no .stats command at all.
    // The match is deliberately strict — the exact counter name, then only
    // separator characters, then digits — because a loose match would silently
    // pick up a neighbouring number and report a confident wrong answer.
    static long long ScrapeCounter(const std::string& text, const std::string& marker) {
        long long total = -1;
        size_t at = text.find(marker);
        while (at != std::string::npos) {
            size_t i = at + marker.size();
            while (i < text.size() && (text[i] == '=' || text[i] == ':' || text[i] == ' ' ||
                                       text[i] == '\t')) {
                ++i;
            }
            if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
                if (total < 0) total = 0;
                total += std::strtoll(text.c_str() + i, nullptr, 10);
            }
            at = text.find(marker, at + marker.size());
        }
        return total;
    }

    // kMaxNestedReads bounds .read expansion. Prepending means a script that reads
    // itself would otherwise loop forever with no stack to overflow and nothing to
    // notice it.
    static constexpr int kMaxNestedReads = 64;

    Database& db_;
    Options options_;
    std::string buffer_;
    std::string last_query_;
    int reads_since_idle_ = 0;
    bool failed_ = false;
};

void PrintUsage(std::ostream& out) {
    out << "usage: quarry_shell [options] [database-directory]\n"
           "\n"
           "  -f, --file <path>        run a SQL script and exit\n"
           "  -e, --execute <sql>      run one statement and exit\n"
           "      --rows-per-segment N segment granularity (pruning precision)\n"
           "      --buffer-frames N    buffer pool size in pages\n"
           "      --buffer-policy P    clock or lru\n"
           "      --disable <rule>     turn off one optimizer rule; repeatable.\n"
           "                           rules: predicate_pushdown, projection_pruning,\n"
           "                           constant_folding, index_selection, segment_pruning\n"
           "  -h, --help               this message\n";
}

bool ParseArgs(int argc, char** argv, Options& options, bool& exit_now, int& exit_code) {
    bool directory_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needs_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "quarry_shell: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            PrintUsage(std::cout);
            exit_now = true;
            exit_code = 0;
            return true;
        }
        if (arg == "-f" || arg == "--file") {
            const char* value = needs_value("-f");
            if (!value) return false;
            options.scripts.push_back(value);
            options.interactive = false;
        } else if (arg == "-e" || arg == "--execute") {
            const char* value = needs_value("-e");
            if (!value) return false;
            options.one_shots.push_back(value);
            options.interactive = false;
        } else if (arg == "--rows-per-segment") {
            const char* value = needs_value("--rows-per-segment");
            if (!value) return false;
            options.db.rows_per_segment = std::strtoul(value, nullptr, 10);
        } else if (arg == "--buffer-frames") {
            const char* value = needs_value("--buffer-frames");
            if (!value) return false;
            options.db.buffer_frames = std::strtoul(value, nullptr, 10);
        } else if (arg == "--buffer-policy") {
            const char* value = needs_value("--buffer-policy");
            if (!value) return false;
            std::string policy = ToLower(value);
            if (policy == "clock") {
                options.db.buffer_policy = quarry::BufferPool::Policy::kClock;
            } else if (policy == "lru") {
                options.db.buffer_policy = quarry::BufferPool::Policy::kLru;
            } else {
                std::cerr << "quarry_shell: unknown buffer policy " << policy << "\n";
                return false;
            }
        } else if (arg == "--disable") {
            const char* value = needs_value("--disable");
            if (!value) return false;
            std::string rule = ToLower(value);
            quarry::Optimizer::Rules& rules = options.db.rules;
            if (rule == "predicate_pushdown") {
                rules.predicate_pushdown = false;
            } else if (rule == "projection_pruning") {
                rules.projection_pruning = false;
            } else if (rule == "constant_folding") {
                rules.constant_folding = false;
            } else if (rule == "index_selection") {
                rules.index_selection = false;
            } else if (rule == "segment_pruning") {
                rules.segment_pruning = false;
            } else {
                std::cerr << "quarry_shell: unknown optimizer rule " << rule << "\n";
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "quarry_shell: unknown option " << arg << "\n";
            return false;
        } else {
            if (directory_set) {
                std::cerr << "quarry_shell: more than one database directory given\n";
                return false;
            }
            options.directory = arg;
            directory_set = true;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    bool exit_now = false;
    int exit_code = 0;
    if (!ParseArgs(argc, argv, options, exit_now, exit_code)) {
        PrintUsage(std::cerr);
        return 2;
    }
    if (exit_now) return exit_code;

    Database db(options.directory, options.db);
    Status opened = db.Open();
    if (!opened.ok()) {
        std::cerr << "quarry_shell: cannot open " << options.directory << ": "
                  << opened.ToString() << "\n";
        return 1;
    }

    Shell shell(db, options);
    bool running = true;

    for (const std::string& sql : options.one_shots) {
        if (!running) break;
        std::string text = Trim(sql);
        if (text.empty()) continue;
        // -e is a convenience, so the trailing semicolon is optional. A dot-command
        // must not get one appended: it is terminated by the end of the line.
        if (text[0] != '.' && text.find(';') == std::string::npos) text += ";";
        running = shell.Feed(text);
    }

    for (const std::string& path : options.scripts) {
        if (!running) break;
        std::ifstream file(path);
        if (!file) {
            std::cerr << "quarry_shell: cannot read " << path << "\n";
            db.Close();
            return 1;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        running = shell.Feed(contents.str());
    }

    if (options.interactive) {
        std::cout << "quarry shell. .help for commands, .quit to leave.\n"
                  << "database: " << options.directory << "\n\n";
        std::string line;
        while (running) {
            std::cout << (shell.buffer_pending() ? "   ...> " : "quarry> ") << std::flush;
            if (!std::getline(std::cin, line)) {
                std::cout << "\n";
                break;
            }
            running = shell.Feed(line);
        }
    }

    Status closed = db.Close();
    if (!closed.ok()) {
        std::cerr << "quarry_shell: close failed: " << closed.ToString() << "\n";
        return 1;
    }

    // A non-interactive run exits non-zero if any statement failed, so a script
    // driving the shell from CI does not silently pass on a broken query.
    if (!options.interactive && shell.failed()) return 1;
    return 0;
}
