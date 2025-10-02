#pragma once

// Shared benchmark plumbing: timing, aggregation, and table output.
//
// Three decisions worth stating, because each one is a way a benchmark gets
// published wrong:
//
//  1. Every measurement is repeated and reported as min/median/max. A single
//     sample on a laptop is a measurement of whatever the OS was doing at that
//     moment. The median is the number to quote; min is the floor the machine is
//     capable of; max is the evidence of how noisy the run was. A result whose
//     max is several times its min should be treated as a range, not a number.
//
//  2. Warmup runs are discarded. The first pass over a fresh buffer faults in
//     pages, fills the page cache, and populates the branch predictor, so
//     including it conflates one-time cost with steady-state throughput.
//
//  3. Sink() exists because clang at -O2 will delete a loop whose result is
//     unused, and a vectorization benchmark that reports a speedup because one
//     side was deleted is the single easiest fake result in this whole directory.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace quarry::bench {

// Sink forces a value to be materialized. The empty asm block claims to read the
// value, which stops the optimizer proving the producing computation dead.
template <typename T>
inline void Sink(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline void ClobberMemory() { asm volatile("" : : : "memory"); }

// Sample is one timing distribution plus an optional work count, so a result can
// be reported as a rate without the call site re-deriving it.
struct Sample {
    std::string name;
    size_t runs = 0;
    double min_ns = 0.0;
    double median_ns = 0.0;
    double max_ns = 0.0;

    // Units of work per run: rows filtered, rows scanned, lookups performed.
    // Rates are computed from the median, which is the figure worth quoting.
    double work_per_run = 0.0;

    double MedianUs() const { return median_ns / 1e3; }
    double MedianMs() const { return median_ns / 1e6; }

    // Spread is max/min. Above about 2 the result is noise-dominated and should be
    // reported as a range in prose rather than as a point value.
    double Spread() const { return min_ns == 0.0 ? 0.0 : max_ns / min_ns; }

    double PerSecond() const {
        return median_ns == 0.0 ? 0.0 : work_per_run * 1e9 / median_ns;
    }
    double NanosPerItem() const {
        return work_per_run == 0.0 ? 0.0 : median_ns / work_per_run;
    }
};

// Time runs `body` warmup+runs times and keeps the distribution of the timed
// portion. `work_per_run` is recorded, not measured: only the caller knows
// whether a run processed rows, lookups, or bytes.
template <typename Body>
Sample Time(const std::string& name, size_t runs, double work_per_run, Body&& body,
            size_t warmup = 1) {
    for (size_t i = 0; i < warmup; ++i) body();

    std::vector<double> timings;
    timings.reserve(runs);
    for (size_t i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto end = std::chrono::steady_clock::now();
        timings.push_back(
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }

    std::sort(timings.begin(), timings.end());
    Sample s;
    s.name = name;
    s.runs = runs;
    s.work_per_run = work_per_run;
    s.min_ns = timings.front();
    s.max_ns = timings.back();
    // Even-count median is the lower of the two middles rather than their mean, so
    // every reported figure is an observation that actually happened.
    s.median_ns = timings[timings.size() / 2];
    return s;
}

// --- table output --------------------------------------------------------
//
// Column widths are computed from the content, so a table never has a value
// running into its neighbour and never has a hard-coded width that a larger
// number silently breaks.
class Table {
public:
    explicit Table(std::vector<std::string> headers) : headers_(std::move(headers)) {}

    void Row(std::vector<std::string> cells) { rows_.push_back(std::move(cells)); }

    // Separator inserts a horizontal rule, used to group related rows inside one
    // table rather than splitting it into several.
    void Separator() { rows_.push_back({kRule}); }

    void Print(std::ostream& os = std::cout) const {
        std::vector<size_t> width(headers_.size(), 0);
        for (size_t c = 0; c < headers_.size(); ++c) width[c] = headers_[c].size();
        for (const auto& row : rows_) {
            if (IsRule(row)) continue;
            for (size_t c = 0; c < row.size() && c < width.size(); ++c) {
                width[c] = std::max(width[c], row[c].size());
            }
        }

        PrintRow(os, headers_, width);
        PrintRule(os, width);
        for (const auto& row : rows_) {
            if (IsRule(row)) {
                PrintRule(os, width);
            } else {
                PrintRow(os, row, width);
            }
        }
    }

private:
    static constexpr const char* kRule = "\x01rule";
    static bool IsRule(const std::vector<std::string>& row) {
        return row.size() == 1 && row[0] == kRule;
    }

    static void PrintRow(std::ostream& os, const std::vector<std::string>& row,
                         const std::vector<size_t>& width) {
        for (size_t c = 0; c < width.size(); ++c) {
            const std::string& cell = c < row.size() ? row[c] : kEmpty();
            os << cell;
            // No padding after the final column, so trailing whitespace never ends
            // up in captured output.
            if (c + 1 < width.size()) {
                os << std::string(width[c] - cell.size() + 2, ' ');
            }
        }
        os << "\n";
    }

    static void PrintRule(std::ostream& os, const std::vector<size_t>& width) {
        for (size_t c = 0; c < width.size(); ++c) {
            os << std::string(width[c], '-');
            if (c + 1 < width.size()) os << "  ";
        }
        os << "\n";
    }

    static const std::string& kEmpty() {
        static const std::string empty;
        return empty;
    }

    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;
};

// --- number formatting ---------------------------------------------------

inline std::string Fmt(double v, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string Fmt(uint64_t v) { return std::to_string(v); }

// Thousands renders an integer with separators, because segment and row counts
// here run to seven digits and a wall of digits is unreadable in a results table.
inline std::string Thousands(uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    const size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i >= lead) && ((i - lead) % 3 == 0)) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

// Rate renders a per-second figure with a magnitude suffix. Benchmarks here span
// thousands of lookups per second to billions of rows per second, and a single
// fixed unit makes one end of that range unreadable.
inline std::string Rate(double per_second) {
    if (per_second >= 1e9) return Fmt(per_second / 1e9) + "G/s";
    if (per_second >= 1e6) return Fmt(per_second / 1e6) + "M/s";
    if (per_second >= 1e3) return Fmt(per_second / 1e3) + "K/s";
    return Fmt(per_second) + "/s";
}

inline std::string Bytes(uint64_t bytes) {
    if (bytes >= (1ull << 30)) return Fmt(static_cast<double>(bytes) / (1ull << 30)) + " GiB";
    if (bytes >= (1ull << 20)) return Fmt(static_cast<double>(bytes) / (1ull << 20)) + " MiB";
    if (bytes >= (1ull << 10)) return Fmt(static_cast<double>(bytes) / (1ull << 10)) + " KiB";
    return std::to_string(bytes) + " B";
}

// MinMedMax is the standard three-number cell, so every timing in every table is
// reported the same way and a reader never has to guess which one a bare number is.
inline std::string MinMedMax(const Sample& s, double scale, int decimals = 2) {
    return Fmt(s.min_ns / scale, decimals) + " / " + Fmt(s.median_ns / scale, decimals) + " / " +
           Fmt(s.max_ns / scale, decimals);
}

inline std::string MinMedMaxMs(const Sample& s) { return MinMedMax(s, 1e6); }
inline std::string MinMedMaxUs(const Sample& s) { return MinMedMax(s, 1e3); }

// --- section headers -----------------------------------------------------

inline void Heading(const std::string& text, std::ostream& os = std::cout) {
    os << "\n== " << text << " ==\n\n";
}

inline void Note(const std::string& text, std::ostream& os = std::cout) {
    os << "\n" << text << "\n";
}

// Fail aborts loudly. A benchmark that silently swallows a failed query reports a
// time for work that did not happen, which is worse than no number at all.
[[noreturn]] inline void Fail(const std::string& what) {
    std::cerr << "\nBENCHMARK ERROR: " << what << "\n";
    std::exit(1);
}

// Check unwraps a quarry Status-like object, aborting with its message.
template <typename StatusLike>
void Check(const StatusLike& s, const std::string& what) {
    if (!s.ok()) Fail(what + ": " + s.ToString());
}

// Take unwraps a Result, aborting on failure. The Result is taken by value
// because Result::take() moves out of it, which a const reference cannot do.
template <typename T, template <typename> class ResultLike>
T Take(ResultLike<T> r, const std::string& what) {
    if (!r.ok()) Fail(what + ": " + r.status().ToString());
    return r.take();
}

}  // namespace quarry::bench
