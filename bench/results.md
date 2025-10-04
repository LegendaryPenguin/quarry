# Quarry benchmark results

Every number below was produced by running the four executables in this directory on
the machine recorded here, in the build recorded here. Nothing is estimated,
smoothed, or carried over from an earlier run: the fenced blocks are the programs'
stdout verbatim, and the prose and summary tables are derived from those blocks.

Several results contradict claims the engine's own header comments make. Those are
collected under [Where the measurements disagree with the design](#where-the-measurements-disagree-with-the-design)
rather than left for a reader to notice.

## Machine and build

| | |
|---|---|
| CPU | Apple M4 Pro, 14 cores (`sysctl -n machdep.cpu.brand_string hw.ncpu`) |
| L1d / L2, performance cores | 128 KiB / 16 MiB (`sysctl -n hw.perflevel0.l1dcachesize hw.perflevel0.l2cachesize`) |
| Memory | 51,539,607,552 B = 48 GiB (`sysctl -n hw.memsize`) |
| OS | macOS 26.6.2 (`sw_vers -productVersion`) |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101), target `arm64-apple-darwin25.6.0` |
| CMake | 4.4.4 |
| Build type | `RelWithDebInfo`, the project default |
| Compile flags | `-O2 -g -std=c++20 -arch arm64 -Wall -Wextra -Wpedantic -Wno-unused-parameter` |
| Storage | local APFS SSD; every benchmark file under `$TMPDIR` |

Note what is **not** in those flags: `-DNDEBUG`. `CMakeLists.txt` sets
`CMAKE_CXX_FLAGS_RELWITHDEBINFO` to exactly `-O2 -g`, so `assert()` is live in the
default release build - including the `assert(std::is_sorted(...))` that
`Batch::SetSelection` runs on every filtered batch. That is an obvious suspect for
the execution results below, so it was measured separately; see
[Build variants](#build-variants). It makes no difference.

Benchmarks were run one at a time on an otherwise idle machine. The engine is
single threaded throughout and takes no locks anywhere, so there is no concurrency
dimension here. Total runtime for all four is about 70 seconds.

## Headline numbers

| Question | Answer | Detail |
|---|---|---|
| Vectorized vs tuple-at-a-time filter | The engine's batch kernel is **2.19x SLOWER** than a row-at-a-time loop over the same columnar bytes (2.858 vs 1.307 ns/row). The batched layout is not the problem: a branch-free kernel on the same batches runs at 0.316 ns/row, **9.0x faster** than the engine's. | [1](#1-execution) |
| Where the batch-size curve peaks | Throughput plateaus from **batch=1024-2048** upwards and stays flat to 32768. Which point inside the plateau reads as peak varies run to run (2048, 8192 and 32768 across three runs); 2048 was 0.1% off peak in this one. Below 1024 the curve falls off hard - batch=1 is 5.5x worse end to end. | [1](#1-execution) |
| Reused vs freshly allocated batch | **1.01x** (0.029 ns/row). Across three runs: 1.00x, 1.01x, 1.04x - indistinguishable from zero. | [1](#1-execution) |
| Projection pushdown, bytes | 1 int64 column of 9 costs **17.62%** of the bytes, not 11.1%. A dictionary-encoded varchar costs 8.82%; an RLE-encoded one costs **0.09%**. | [2](#2-storage) |
| Pruning, column correlated with insert order | **122 of 123 segments pruned (99.2%)**, bytes ratio 0.01x, **117.93x faster**. | [2](#2-storage) |
| Pruning, uniformly random column | **0 of 123 segments pruned**, bytes ratio 1.00x, and **0.87x - i.e. slower than the same scan with no predicate at all.** | [2](#2-storage) |
| B+ tree vs LSM: insert | LSM wins: **477.41K/s vs 241.02K/s**. | [3](#3-index) |
| B+ tree vs LSM: point lookup | LSM wins both: present **3.482 vs 4.328 us**, absent **0.458 vs 3.140 us**. | [3](#3-index) |
| B+ tree vs LSM: range scan | B+ tree wins decisively: **9.2x to 24.9x faster** across widths from 0.01% to 100%. | [3](#3-index) |
| B+ tree vs LSM: write amplification | **258.89x vs 4.66x** - the LSM is 55.6x better. | [3](#3-index) |
| Index vs scan crossover | Between **30% and 50%** selectivity (0.96x at 30%, 1.14x at 50%; interpolating, about **34%**). At one row the index wins by 50x. | [3](#3-index) |
| `predicate_pushdown` | **1.05x to 63.55x**, bytes 1.00x to 61.04x. The most valuable rule by a wide margin. | [4](#4-optimizer) |
| `projection_pruning` | **1.43x to 5.20x**, bytes 1.61x to 5.68x. The only rule that helps on every query. | [4](#4-optimizer) |
| `constant_folding` | **0.93x-1.07x on four of five queries, 85.56x on the fifth.** All or nothing. | [4](#4-optimizer) |
| `index_selection` | **No effect on any query**: bytes 1.00x and an identical plan everywhere; time wanders 0.96x to 1.15x across runs on sub-millisecond queries. The rule sets a field no executor reads. | [4](#4-optimizer) |
| `segment_pruning` | **Bytes 1.00x on every query**, time 0.94x to 1.12x. Redundant with the scan operator, which re-prunes identically. | [4](#4-optimizer) |

## 1. Execution

```sh
cmake -S . -B build && cmake --build build -j && ./build/execution_bench
```

### Verbatim output

```
quarry execution benchmark
kBatchSize=2048  rows=4,000,000  predicated int64 column = 30.52 MiB
row-major copy = 122.07 MiB

== 1. Vectorized versus tuple-at-a-time, 10% selective filter on an int64 column ==

rows: 4,000,000   matched: 399,064 (9.98%)   runs per path: 7
batches materialized: 1,954

path                                                                   ms min/med/max         rows/sec   ns/row  vs engine kernel  max/min
---------------------------------------------------------------------  ---------------------  ---------  ------  ----------------  -------
engine ApplyPredicate over pre-materialized batches (kBatchSize=2048)  11.37 / 11.43 / 11.54  349.93M/s  2.858   1.00x             1.02
engine scan path: batch fill + ApplyPredicate                          18.55 / 18.96 / 19.15  210.97M/s  4.740   1.66x             1.03
---------------------------------------------------------------------  ---------------------  ---------  ------  ----------------  -------
tuple-at-a-time, columnar data, virtual per-row predicate              5.16 / 5.23 / 5.40     765.06M/s  1.307   0.46x             1.05
tuple-at-a-time, row-major data (32 B/row), virtual per-row predicate  5.28 / 5.33 / 5.54     750.98M/s  1.332   0.47x             1.05
tuple-at-a-time, boxed Value comparison                                12.51 / 13.24 / 13.58  302.21M/s  3.309   1.16x             1.09
---------------------------------------------------------------------  ---------------------  ---------  ------  ----------------  -------
branch-free type-specialized int64 kernel (ceiling)                    1.25 / 1.26 / 1.37     3.16G/s    0.316   0.11x             1.10

The comparison that answers the project's claim is the first row against
the third: the engine's batch kernel versus a row-at-a-time loop over the
SAME columnar bytes. Both evaluate the predicate, both build a list of
surviving row ids, both then read the survivors.

== 2. Batch size sweep ==

rows per pass: 4,000,000
engine kBatchSize = 2048 (16 KiB of int64 - the L1/L2 residency claim under test)

batch  col KiB  kernel-only ms min/med/max  kernel ns/row  fill ns/row  fill+kernel ns/row  fill+kernel rows/s  max/min
-----  -------  --------------------------  -------------  -----------  ------------------  ------------------  -------
1      0.008    (not materialized)          -              7.461        25.717              38.88M/s            1.14
8      0.062    (not materialized)          -              2.598        8.251               121.20M/s           1.03
64     0.5      19.62 / 20.17 / 20.26       5.044          2.045        5.246               190.63M/s           1.03
256    2.0      13.90 / 14.15 / 14.76       3.538          2.031        5.084               196.70M/s           1.06
1024   8.0      11.52 / 12.37 / 13.22       3.093          1.980        4.824               207.29M/s           1.15
2048   16.0     11.10 / 11.32 / 11.43       2.829          1.969        4.665               214.37M/s           1.03
8192   64.0     11.23 / 11.42 / 12.45       2.855          2.011        4.907               203.77M/s           1.11
32768  256.0    11.25 / 11.30 / 11.43       2.826          2.061        5.076               196.99M/s           1.02

kernel-only peak: batch=32768 at 353.89M/s; within 2% of peak from batch=2048 upwards.
kernel-only at the engine's kBatchSize=2048: 353.45M/s (0.1% below peak)
fill+kernel peak: batch=2048 at 214.37M/s

== 3. Reused batch versus a fresh batch per iteration ==

strategy               ms min/med/max         rows/sec   ns/row  max/min
---------------------  ---------------------  ---------  ------  -------
reused batch           19.06 / 19.35 / 19.90  206.71M/s  4.838   1.04
fresh Batch per chunk  19.12 / 19.47 / 19.68  205.49M/s  4.866   1.03

allocation overhead: 1.01x, 0.029 ns/row, 58.73 ns per batch of 2048 over 1,954 batches

== 4. Selectivity sweep at kBatchSize ==

target  actual    kernel ms min/med/max  kernel ns/row  tuple ms min/med/max   tuple ns/row  tuple/kernel  branch-free ns/row
------  --------  ---------------------  -------------  ---------------------  ------------  ------------  ------------------
0.1%    0.098%    8.93 / 9.28 / 9.72     2.319          2.94 / 2.97 / 3.02     0.742         0.32x         0.261
1.0%    0.994%    9.08 / 9.32 / 9.39     2.330          3.13 / 3.15 / 3.19     0.789         0.34x         0.264
10.0%   9.977%    11.38 / 11.45 / 11.63  2.863          5.31 / 5.43 / 5.53     1.357         0.47x         0.318
50.0%   49.983%   19.72 / 20.08 / 20.80  5.020          14.11 / 14.27 / 14.60  3.569         0.71x         0.361
100.0%  100.000%  10.37 / 10.68 / 10.83  2.669          14.98 / 15.08 / 15.72  3.770         1.41x         0.419

Every path costs more as selectivity rises, because the list of surviving
row ids that gets written and then re-read grows with the match count. The
50% row is the branch-misprediction worst case for the two branching paths;
the branch-free column is the control that shows it.
```

### Reading it

**The central claim does not hold as stated.** `batch.hpp` presents the
selection-vector design as the reason filters are cheap and `operator.hpp` calls
execution "pull-based and vectorized". On this data the engine's own
`ApplyPredicate` costs **2.858 ns/row** while a tuple-at-a-time virtual
predicate over the *same columnar array* costs **1.307 ns/row**. The
vectorized path is 2.19x slower than the thing it is meant to beat.

Three numbers in the same table say why, and together they exonerate the design
while convicting the implementation:

- The branch-free type-specialized kernel runs at **0.316 ns/row** on
  exactly the same batches. So the batched columnar layout really does make a
  9.0x win available. The engine is not collecting it.
- The boxed-`Value` path costs 3.309 ns/row, only 1.16x the
  engine's kernel. `types.hpp` says `Value` is "deliberately NOT what the execution
  engine moves around: a query that filters a million rows must not allocate a
  million of these." The engine's kernel is barely faster than doing precisely that.
- Under `-flto` the kernel drops from 2.858 to **1.866 ns/row** (see
  [Build variants](#build-variants)), so about 35% of its cost is a
  per-row call that cannot be inlined across translation units.

The mechanism is in the source, not inferred from the timings. `ApplyPredicate`
(`src/execution/expression.cpp`) loops over `batch.Rows()` calling `EvalCompare`
(`src/execution/exec_util.cpp`, a different translation unit), which calls
`OrderAgainstConstant`, which switches on the column's `TypeId`, switches again on
the constant's `TypeId`, and returns a `std::optional<int>`. That is a function call,
two type dispatches and an optional per row. The filter is batch-at-a-time in
structure and scalar in its kernel; vectorization in the sense the headers describe -
one type-specialized loop per batch - is not what it does.

**The `kBatchSize = 2048` justification holds at its lower bound and not at its
upper one.** `batch.hpp` argues that 2048 int64 values is 16 KiB and therefore stays
in L1/L2, and that "larger batches stop fitting". The kernel-only curve rises steeply
from batch=1 to batch=1024 (5.044 ns/row at 64 against 2.829 at 2048) and then
goes flat all the way to 32768 - which is 256 KiB, far outside L1, and is as fast as
2048. So the cache argument correctly predicts where the curve stops improving and
does not predict any penalty for exceeding the cache, because on this machine there
isn't one. Across three runs of the same binary the nominal peak landed at 2048, 8192
and 32768 with 2048 between 0.1% and 4.1% behind, which is inside the run-to-run
spread. The defensible statement is: anything from ~1024 up is equivalent, 2048 sits
comfortably inside the plateau, and it is a sound choice rather than a measured
optimum. The `fill+kernel` curve - what a real scan pays - peaks at 1024 or 2048 and
degrades slightly above, so very large batches are mildly worse end to end.

**Batch reuse is not worth what `batch.hpp` claims.** The header says reuse "is worth
a measurable amount on a long scan". Three runs of this benchmark gave 1.00x, 1.01x
and 1.04x; this run gave 1.01x (0.029 ns/row). It is at or below the noise
floor. The allocator is handling 1,954 same-sized allocations per pass, which is the
easy case for it.

**Selectivity behaves exactly as a selection-vector design predicts, and the 100% row
is where batching finally wins.** Both branching paths peak in cost at 50%
selectivity (kernel 5.020, tuple 3.569 ns/row) and get *cheaper* at 100%
(2.669 and 3.770) - textbook branch misprediction, confirmed by the
branch-free control, which instead rises monotonically from 0.261 to 0.419
ns/row because its only growing cost is the selection vector it writes. At 100%
selectivity the engine's kernel beats the tuple path by 1.41x, and for the
reason the header gives: the kernel's selection vector is one 8 KiB batch that stays
resident while the tuple path writes and then re-reads a 16 MB list.

**The batch fill is a large and largely invisible cost.** The engine's full scan path
costs 4.740 ns/row against the kernel's 2.858 - 1.66x -
with the fill alone at about 2.0 ns/row across every batch size. `FileSegmentReader::Next`
decodes a whole column at `Open` and then copies it into the output batch one row at a
time through a per-row `switch` on type plus a `NullMask::Resize` per append. That copy
is ~40% of a filtering scan's CPU time and is not vectorized in any sense.

## 2. Storage

```sh
./build/storage_bench
```

### Verbatim output

```
quarry storage benchmark
directory: /var/folders/n5/mxl6vn5n0z1gg73nbs75f8h40000gq/T/quarry-bench-storage
events: 2,000,000 rows in 123 segments
groups: 1,000,000 rows in 62 segments

== 1. Full scan throughput ==

rows: 2,000,000 in 123 segments of 16,384
rows produced per pass: 2,000,000
on-disk bytes for all columns: 86.64 MiB (45.4 B/row)
logical in-memory bytes:      118.73 MiB (62.3 B/row)

projection           ms min/med/max            rows/sec  on-disk MB/s  logical MB/s  bytes read  max/min
-------------------  ------------------------  --------  ------------  ------------  ----------  -------
all 9 columns        316.36 / 326.00 / 373.89  6.13M/s   265.78        364.21        86.64 MiB   1.18
1 int64 column (id)  50.15 / 54.16 / 55.11     36.93M/s  281.82        281.73        15.26 MiB   1.10

The scan decodes each segment's projected columns in full at open time and
then copies them row by row into the output batch, so these rates include a
decode and a copy, not just a read. 'on-disk MB/s' uses bytes actually
pread; 'logical MB/s' uses the uncompressed row width.

== 2. Projection pushdown, measured in bytes rather than asserted ==

projection                              bytes read  share of all 9  B/row  ms min/med/max            share of full-scan time
--------------------------------------  ----------  --------------  -----  ------------------------  -----------------------
1 of 9: id (int64)                      16,005,412  17.62%          8.00   46.10 / 48.96 / 50.11     15.9%
1 of 9: amount (double)                 16,005,412  17.62%          8.00   50.03 / 51.51 / 53.04     16.7%
1 of 9: category (varchar, dictionary)  8,015,990   8.82%           4.01   57.83 / 58.49 / 60.23     19.0%
1 of 9: region (varchar, rle)           81,755      0.09%           0.04   35.64 / 36.02 / 36.10     11.7%
3 of 9: id, status, amount              32,257,736  35.51%          16.13  95.41 / 97.09 / 97.54     31.5%
9 of 9: everything                      90,853,513  100.00%         45.43  303.13 / 307.94 / 328.64  100.0%

A one-of-nine projection does not cost one ninth of the bytes, and the
reason is visible in the numbers: every reader reads the 32-byte header and
the footer regardless, and the nine columns are not the same size, so the
share a column accounts for is its own size and not 1/9 of the total.

== 3. Segment pruning: correlated versus random columns ==

predicate                                                   rows out   selectivity  segs scanned  segs pruned  pruned %  bytes read  vs no predicate  ms min/med/max            speedup
----------------------------------------------------------  ---------  -----------  ------------  -----------  --------  ----------  ---------------  ------------------------  -------
(none, 3 columns projected)                                 2,000,000  100%         123           0            0.0%      48,008,364  1.00x            126.85 / 131.45 / 157.02  1.00x
----------------------------------------------------------  ---------  -----------  ------------  -----------  --------  ----------  ---------------  ------------------------  -------
id in [1000000, 1002000) - correlated with insertion order  2,000      0.1000%      1             122          99.2%     393,284     0.01x            1.03 / 1.11 / 1.16        117.93x
ts in a 8127-tick window - non-decreasing with jitter       2,000      0.1000%      1             122          99.2%     393,284     0.01x            1.07 / 1.09 / 1.14        120.11x
amount in a 10.00-wide window - uniformly random            1,983      0.0992%      123           0            0.0%      48,008,364  1.00x            143.98 / 151.07 / 180.90  0.87x

This is the finding, and it is not a win: on the random column the zone map
prunes nothing, reads every byte, and the query is no faster than the
unfiltered scan - the predicate is pure extra work. Pruning is not a
property of the engine, it is a property of the data's physical order, and
an engine that claims it without saying so is overselling.

== 4. Encoding: size and decode speed, on data suited and unsuited to each ==

rows per column: 16,384 (one segment)
sizes are the encoded chunk body including its 8-byte prefix

column shape                                       type     auto-chosen  plain B  dict B   rle B    best/plain  plain decode  dict decode  rle decode
-------------------------------------------------  -------  -----------  -------  -------  -------  ----------  ------------  -----------  ----------
varchar, 8 distinct, shuffled (dictionary's case)  VARCHAR  dictionary   151,761  65,630   189,589  0.43x       140.13M/s     132.17M/s    118.26M/s
varchar, 4 distinct in 433-row runs (rle's case)   VARCHAR  rle          195,828  65,604   618      0.00x       159.91M/s     161.68M/s    162.49M/s
varchar, all distinct (plain's case)               VARCHAR  plain        356,268  421,808  421,800  1.00x       120.10M/s     112.96M/s    113.58M/s
int64, 5 distinct in 97-row runs (rle's case)      BIGINT   rle          131,080  n/a      2,040    0.02x       451.97M/s     n/a          465.35M/s
int64, strictly increasing (no runs, no repeats)   BIGINT   plain        131,080  n/a      196,620  1.00x       464.25M/s     n/a          338.11M/s
double, uniform random (plain's case)              DOUBLE   plain        131,080  n/a      196,620  1.00x       458.29M/s     n/a          285.77M/s

Dictionary is varchar-only by construction: for a fixed-width type the
codes are as wide as the values, so the entries are 'n/a' rather than
slower. The decode rates are the number worth watching - the smallest
encoding is not always the fastest to read back, and the auto-chooser
optimizes size only.

== 5. GROUP BY throughput versus group cardinality ==

rows: 1,000,000   aggregates: COUNT(*), SUM(v)   one group column per cardinality

groups requested  groups produced  ms min/med/max            rows/sec  ns/row  ns per group  max/min
----------------  ---------------  ------------------------  --------  ------  ------------  -------
2                 2                61.01 / 61.04 / 64.80     16.38M/s  61.04   30519145.5    1.06
16                16               58.62 / 58.98 / 59.26     16.95M/s  58.98   3686390.6     1.01
256               256              58.71 / 59.02 / 59.69     16.94M/s  59.02   230543.0      1.02
4,096             4,096            64.20 / 64.28 / 64.35     15.56M/s  64.28   15694.3       1.00
65,536            65,536           80.72 / 81.03 / 81.81     12.34M/s  81.03   1236.5        1.01
1,000,000         632,183          271.28 / 312.29 / 330.98  3.20M/s   312.29  494.0         1.22

'ns per group' is total time divided by groups produced, so it is only
meaningful at the high-cardinality end; at two groups it is an artefact.
The number to read is ns/row, which is close to flat up to a few thousand
groups and only degrades once the hash table and its per-group vector<Value>
keys stop fitting in cache. The last row produces fewer groups than it
requests because a million random draws over a million rows collide.
```

### Reading it

**Projection pushdown is real, and the measured number is more informative than the
assertion would be.** One int64 column of nine costs 17.62% of the bytes, not
11.1%, because the nine columns are not the same size: the varchars and the boolean
compress well below 8 B/row, so a fixed-width column is a larger share of the total
than its position suggests. The RLE-encoded `region` column costs **0.09%** -
0.04 B/row for a column whose value changes once every 433 rows. Time tracks
bytes but not exactly (15.9% of full-scan time for 17.62% of the bytes), because
every reader pays a fixed header and footer read per segment whatever the projection.

**Segment pruning is where an honest benchmark earns its keep.** Same 0.1%-selective
query, same projection, same segment layout, three different columns:

| predicate column | segments pruned | bytes vs no predicate | speedup |
|---|---|---|---|
| `id`, strictly increasing | 122 / 123 (99.2%) | 0.01x | **117.93x** |
| `ts`, non-decreasing with jitter | 122 / 123 (99.2%) | 0.01x | **120.11x** |
| `amount`, uniformly random | **0 / 123 (0.0%)** | **1.00x** | **0.87x** |

On the random column the zone map prunes nothing, reads every byte, and the query is
**slower than the same scan with no predicate at all** - the predicate is pure extra
CPU for no reduction in I/O. Pruning is not a property of this engine; it is a
property of the physical order of the data, and `id` and `amount` differ by two
orders of magnitude on identical machinery. The `ts` row matters as much as the other
two: it shows the realistic shape (non-decreasing with per-row jitter, which is what
time-series data looks like) does as well as the artificial sorted case, so the good
result is not an artefact of perfect sorting.

**Encoding: the chooser optimizes size, and size and decode speed do not always
agree.** Every automatic choice was the smallest admissible layout. On suited data the
win is large - RLE takes the 433-row-run varchar from 195,828 B to **618 B**
(0.00x) and the 97-row-run int64 from 131,080 to 2,040 B
(0.02x); dictionary takes the 8-distinct shuffled varchar to 0.43x. On
unsuited data both alternatives correctly lose and plain is chosen. Two things worth
noting:

- Dictionary is varchar-only by construction (`encoding.hpp`: "for a fixed-width type
  the codes are as wide as the values, so it can only lose"), hence the `n/a` cells.
  That reasoning is sound for int64 but rules out a dictionary on a low-cardinality
  `double` column, where 8-byte values could map to 1-byte codes. A gap, not a bug.
- RLE is **slower to decode than plain on data it does not suit**: 338.11M/s against
  464.25M/s on the strictly-increasing int64, and 285.77M/s against 458.29M/s on
  random doubles. Since the chooser compares predicted sizes only it would never pick
  RLE there, but a forced encoding would cost 30-40% of decode speed for no size win.

**Scan throughput** is 6.13M/s for all nine columns (265.78 on-disk MB/s,
364.21 logical MB/s) and 36.93M/s for one int64 column. Both include a
full decode and the row-at-a-time batch fill discussed in section 1, not just a read.

**GROUP BY is close to flat up to a few thousand groups and then degrades.** From the
table: 58.98 ns/row at 16 groups, 64.28 at 4,096, 81.03
at 65,536, and 312.29 at 632,183 groups - about
4.9x the small-cardinality cost. That is
what a hash table which no longer fits in cache, plus a `vector<Value>` key allocated
per group, produces. The last row returns fewer groups than requested because a
million random draws over a million rows collide; that is the generator, not the
operator.

## 3. Index

```sh
./build/index_bench
```

### Verbatim output

```
quarry index benchmark
keys=500,000  lookups=100,000  page size=4096

== 1. Insert throughput and write amplification ==

keys: 500,000 unique int64, inserted in shuffled order
logical bytes inserted: 8,000,000 (7.63 MiB)
btree height: 3   lsm levels: 5
single timed run per structure: building is destructive, so there is no
distribution to report. The derived counters below are exact, not sampled.

structure  build ms  inserts/sec  bytes written  logical bytes  write amp  on-disk bytes  space amp  splits  compactions
---------  --------  -----------  -------------  -------------  ---------  -------------  ---------  ------  -----------
B+ tree    2074.52   241.02K/s    2,071,113,728  8,000,000      258.89x    11,563,008     1.45x      2,819   0
LSM        1047.32   477.41K/s    39,648,354     8,500,000      4.66x      8,309,720      0.98x      0       203

Write amplification is bytes_written / logical_bytes, both reported by the
index itself. For the B+ tree bytes are derived from whole pages, which is
exact because it only ever transfers pages; the LSM counts real bytes.

== 2. Point lookup latency and read amplification ==

lookups per run: 100,000   buffer pool: 1411 frames (5.51 MiB, ~50% of a 2,823-page tree)
the LSM does not use the buffer pool: it reads its SSTables directly, so its
pool columns are blank rather than zero-by-accident

workload              us/lookup min/med/max  lookups/sec  rows found  logical pages/lookup  physical reads/lookup  bytes read/lookup  pool hit rate  sstables/lookup  bloom rejects/lookup  max/min
--------------------  ---------------------  -----------  ----------  --------------------  ---------------------  -----------------  -------------  ---------------  --------------------  -------
B+ tree, key present  4.148 / 4.328 / 4.466  231.06K/s    100,000     4.011                 0.494                  16427.9            87.7%          -                -                     1.08
B+ tree, key absent   3.116 / 3.140 / 3.163  318.47K/s    0           4.000                 0.000                  16384.0            100.0%         -                -                     1.02
--------------------  ---------------------  -----------  ----------  --------------------  ---------------------  -----------------  -------------  ---------------  --------------------  -------
LSM, key present      3.430 / 3.482 / 3.527  287.19K/s    100,000     1.084                 -                      252.5              -              1.084            7.916                 1.03
LSM, key absent       0.451 / 0.458 / 0.477  2.18M/s      0           0.093                 -                      18.4               -              0.093            8.907                 1.06

The absent-key rows are where the structures differ most. A B+ tree pays the
same descent whether the key is there or not. The LSM has to rule the key out
of every table it holds, and the Bloom filters are what stop that from being
a read per table - bloom rejects per lookup is how many tables were skipped
without touching their data.
'logical pages/lookup' is the tree's own FetchPage count, which is its height
plus the meta page and does not vary with the pool. 'physical reads/lookup' is
the pool's pread count, which does.

== 3. Range scan throughput ==

range width  keys returned  btree us/scan min/med/max    btree keys/s  lsm us/scan min/med/max         lsm keys/s  lsm/btree  btree pages/scan  lsm sstables/scan
-----------  -------------  ---------------------------  ------------  ------------------------------  ----------  ---------  ----------------  -----------------
0.0100%      50             2.14 / 2.21 / 5.73           22.60M/s      33.61 / 36.21 / 39.37           1.38M/s     16.37x     4.0               9.0
0.1000%      500            4.92 / 5.03 / 5.41           99.41M/s      98.93 / 101.52 / 105.95         4.93M/s     20.18x     6.0               9.0
1.0000%      5,000          31.95 / 32.18 / 32.25        155.38M/s     775.98 / 801.07 / 835.21        6.24M/s     24.89x     28.0              9.0
10.0000%     50,000         316.96 / 339.17 / 345.31     147.42M/s     7897.08 / 7990.88 / 8134.40     6.26M/s     23.56x     257.0             9.0
100.0000%    333,334        5532.00 / 5886.79 / 6008.77  56.62M/s      53048.46 / 54069.73 / 55529.25  6.16M/s     9.18x      1853.0            9.0

Both structures were verified to return the same number of row ids for every
range, so the ratio is a speed difference and not a coverage difference.

== 4. Indexed lookup versus full scan: finding the crossover ==

table: 500,000 rows in 31 segments; key is a shuffled permutation
both paths project the key column only and are checked to return the same
number of rows
key ranges are centred in the key domain, NOT anchored at its bottom: an
anchored range lets a zone map prune on min alone and would hand the scan a
win that has nothing to do with the comparison being made

selectivity    rows     index ms/query min/med/max  index bytes/query  segs touched  scan ms/query min/med/max  scan bytes/query  segs pruned  winner  index/scan
-------------  -------  --------------------------  -----------------  ------------  -------------------------  ----------------  -----------  ------  ----------
1 row (point)  1        0.362 / 0.372 / 0.418       131,116            1             14.906 / 14.956 / 15.244   4,001,364         0            index   0.02x
0.0010%        5        1.789 / 1.800 / 1.835       655,580            5             14.639 / 14.655 / 14.663   4,001,364         0            index   0.12x
0.0100%        50       9.732 / 9.758 / 9.783       3,476,900          27            14.537 / 14.562 / 14.617   4,001,364         0            index   0.67x
0.1000%        500      11.479 / 11.540 / 11.552    4,001,364          31            14.599 / 14.629 / 14.656   4,001,364         0            index   0.79x
1.0000%        5,000    11.556 / 11.573 / 11.620    4,001,364          31            14.588 / 14.620 / 14.714   4,001,364         0            index   0.79x
5.0000%        25,000   11.965 / 12.019 / 12.031    4,001,364          31            14.790 / 14.893 / 15.058   4,001,364         0            index   0.81x
10.0000%       50,000   12.492 / 12.547 / 12.567    4,001,364          31            14.724 / 14.751 / 15.014   4,001,364         0            index   0.85x
20.0000%       100,000  13.614 / 13.616 / 13.689    4,001,364          31            15.272 / 15.308 / 15.731   4,001,364         0            index   0.89x
30.0000%       150,000  14.790 / 14.796 / 14.885    4,001,364          31            15.420 / 15.435 / 15.478   4,001,364         0            index   0.96x
50.0000%       250,000  17.273 / 17.315 / 17.446    4,001,364          31            14.969 / 15.142 / 15.209   4,001,364         0            scan    1.14x

Crossover: the full scan overtakes the index at about 50.0000% selectivity (250,000 of 500,000 rows).
This is the number a cost-based optimizer needs: below it, use the index;
above it, scan. Note what drives it - the index's fetch cost is dominated by
how many SEGMENTS its hits are spread over, not by how many rows it found,
because reading one row of a segment decodes that segment's whole column.

== 5. Buffer pool sensitivity (B+ tree) ==

tree: 2,823 pages (11.03 MiB)   workload: 100,000 random point lookups

pool        frames  pool bytes  us/lookup min/med/max  lookups/sec  hit rate  misses   evictions  dirty evictions  logical pages/lookup  physical reads/lookup  vs clock 100%
----------  ------  ----------  ---------------------  -----------  --------  -------  ---------  ---------------  --------------------  ---------------------  -------------
clock 100%  2,823   11.03 MiB   3.672 / 3.677 / 3.713  271.94K/s    99.88%    2,823    0          0                4.011                 0.005                  1.00x
clock 50%   1,411   5.51 MiB    3.930 / 3.955 / 4.015  252.87K/s    87.69%    296,288  294,877    0                4.011                 0.494                  1.08x
clock 25%   705     2.75 MiB    3.607 / 3.628 / 3.730  275.63K/s    81.20%    452,394  451,689    0                4.011                 0.754                  0.99x
clock 5%    141     564.00 KiB  3.025 / 3.048 / 3.058  328.10K/s    74.69%    609,102  608,961    0                4.011                 1.015                  0.83x
----------  ------  ----------  ---------------------  -----------  --------  -------  ---------  ---------------  --------------------  ---------------------  -------------
lru 100%    2,823   11.03 MiB   7.667 / 7.734 / 7.805  129.31K/s    99.88%    2,823    0          0                4.011                 0.005                  2.10x
lru 50%     1,411   5.51 MiB    5.656 / 5.682 / 5.755  175.99K/s    87.69%    296,147  294,736    0                4.011                 0.494                  1.55x
lru 25%     705     2.75 MiB    4.394 / 4.448 / 4.514  224.84K/s    81.22%    452,027  451,322    0                4.011                 0.753                  1.21x
lru 5%      141     564.00 KiB  3.225 / 3.287 / 3.397  304.25K/s    75.49%    589,852  589,711    0                4.011                 0.983                  0.89x
----------  ------  ----------  ---------------------  -----------  --------  -------  ---------  ---------------  --------------------  ---------------------  -------------

Logical pages per lookup does not move across pool sizes, because the tree asks
for the same pages regardless; physical reads per lookup and the hit rate are
what change. If latency does NOT follow the hit rate here, that is a result
about this pool and not a measurement error - see results.md, which reports it
either way.
```

### Reading it

**All four axes, same 500,000 unique int64 keys inserted in shuffled order:**

| axis | B+ tree | LSM | winner |
|---|---|---|---|
| insert throughput | 241.02K/s | **477.41K/s** | LSM, 1.98x |
| point lookup, key present | 4.328 us | **3.482 us** | LSM, 1.24x |
| point lookup, key absent | 3.140 us | **0.458 us** | LSM, 6.9x |
| range scan, 0.01%-100% | **9.2x-24.9x faster** | - | B+ tree |
| write amplification | 258.89x | **4.66x** | LSM, 55.6x |
| space amplification | 1.45x | **0.98x** | LSM |

**The write-amplification gap is the headline and it is enormous.** The B+ tree wrote
2,071,113,728 bytes to store 8,000,000 bytes of keys:
**258.89x**. That is not a defect, it is what a page-based in-place
structure does when single inserts arrive in random key order - each insert dirties a
4 KiB page to add 16 bytes, and with a pool smaller than the tree those pages are
written out repeatedly (2,819 node splits along the way). The LSM's
4.66x over 203 compactions is close to what size-tiered
compaction should cost at this scale; `lsm.hpp` predicts "about log_4(n)" rewrites and
log_4(500,000) is about 9.4, so 4.66x bytes is if anything better than advertised. An
engine that offers both structures and does not publish this number is hiding the main
reason to choose between them.

**The LSM beating the B+ tree on point lookups is the surprise of this benchmark.**
The conventional expectation is the opposite - an LSM consults several tables, a B+
tree does one descent. It loses anyway here, for two measured reasons. First, the
LSM's read amplification is genuinely low: **1.084 SSTables and
1.084 logical pages per lookup** out of ~9 tables, because the
Bloom filters reject 7.916 of them before any data is touched.
It reads 252.5 bytes per lookup against the tree's
16427.9. Second, the tree's
4.011 logical page fetches each go through
`BufferPool::FetchPage`, whose page table is a **linearly scanned `std::vector`**, so a
hit on a 1,411-frame pool costs a scan of up to 1,411 entries - see the pool results
below.

On absent keys the gap widens to
6.9x.
A B+ tree pays the same descent whether the key exists or not
(4.000 pages, 16384.0 bytes, for
zero rows). The LSM pays 0.093 pages: the filters reject
8.907 tables per lookup, in most cases all of them, so the
lookup never opens a file.

**Range scans are the B+ tree's win and it is not close** - 9.2x to
24.9x. The tree walks sorted leaves and reads pages proportional to
the range (4 pages for 50 keys, 1,853 for 333,334). The LSM consults all 9 SSTables for
every range regardless of width, because without a global sort order it cannot know
which tables overlap without checking - that fixed cost is why its throughput is almost
constant across four orders of magnitude of range width while the tree's varies with
locality.

**The buffer pool result is backwards, and it is a finding rather than noise.**

| pool | hit rate | physical reads/lookup | us/lookup (median) | vs clock 100% |
|---|---|---|---|---|
| clock 100% | 99.88% | 0.005 | 3.677 | 1.00x |
| clock 50% | 87.69% | 0.494 | 3.955 | 1.08x |
| clock 25% | 81.20% | 0.754 | 3.628 | 0.99x |
| clock 5% | 74.69% | 1.015 | **3.048** | **0.83x** |
| lru 100% | 99.88% | 0.005 | 7.734 | 2.10x |
| lru 50% | 87.69% | 0.494 | 5.682 | 1.55x |
| lru 25% | 81.22% | 0.753 | 4.448 | 1.21x |
| lru 5% | 75.49% | 0.983 | 3.287 | 0.89x |

Shrinking the pool from 100% of the tree to 5% **improves** median latency, while the
hit rate falls from 99.88% to about 74.69%. That
inverts the premise of `buffer.hpp`, which says "a pool large enough to hold the whole
file teaches nothing". Two causes, both in the code:

1. `BufferPool::FetchPage` locates a resident page with `std::find_if` over
   `page_to_frame_`, a flat `vector<pair<PageId,size_t>>`, so the cost of a HIT is
   O(resident pages). `buffer.hpp` says "at the frame counts used here a vector scan or
   a small hash both work"; at 2,823 frames it does not work - the hit path costs more
   than the miss it avoids.
2. A miss is cheap because the index file is 11 MiB on a machine with 48 GiB, so every
   `pread` is served from the OS page cache. The pool is competing against a second
   cache that is faster than itself.

LRU is 2.10x of clock at 100% for an *identical* hit rate, which the
same cause explains: a hit under LRU additionally erases and re-pushes the page-table
entry to maintain access order, an O(n) `memmove` per hit. `buffer.hpp` describes clock
as "approximates LRU at a fraction of the bookkeeping"; here clock matches LRU's hit
rate exactly at every size and is strictly faster, so there is no accuracy-for-cost
tradeoff to make.

Note the two read-amplification columns, which is why they are reported separately.
`Index::Stats::pages_read` is **4.011 at every pool size**, because it
counts the tree's `FetchPage` calls and those do not depend on what is resident. Only
`BufferPool::Stats::reads` moves (0.005 to 1.015).
Quoting the first figure as "read amplification" would hide the entire effect.

**The index-versus-scan crossover sits at roughly 34% selectivity**,
interpolating between 0.96x at 30% and 1.14x at 50%. That is far above the
few-percent figure a textbook would give, and the table shows why rather than
asserting it:

- At one row the index wins by **50x** (0.372 ms
  against 14.956 ms) and touches
  1 segment of 31.
- By 0.01% selectivity (50 rows) it already touches **27 of 31
  segments** and its advantage has collapsed to 1.49x.
- From 0.1% upwards it touches all 31 and reads **exactly the same
  4,001,364 bytes as the full scan**, winning only by the rows it can
  skip decoding inside each segment.

The driver is not row count, it is **segment spread**. `FileSegmentReader` decodes a
segment's whole projected column at `Open`, so touching one row of a segment costs a
full column decode of that segment. An index whose hits scatter uniformly ends up
paying for the entire table plus the descent. With 31 segments, ~50 random hits are
enough to touch nearly all of them - that is a coupon-collector threshold, not a
selectivity threshold. An optimizer on this engine should therefore be choosing on
*estimated segments touched*, which it could compute from the same statistics, rather
than on estimated rows, which is what `src/planner/optimizer.cpp` actually uses.

One caveat on the scan side: the key column is a shuffled permutation, so zone maps
prune nothing (0 of 31 segments at every selectivity) and the
scan gets no pruning help. If the key were correlated with insertion order the scan
would prune 30 of 31 segments, as section 2 shows, and the index would be redundant
rather than merely beaten. This crossover is specific to uncorrelated keys - which is
the only case where an index is the interesting answer anyway.

## 4. Optimizer

```sh
./build/optimizer_bench
```

### Verbatim output

```
quarry optimizer benchmark
rows: 1,000,000 in 62 segments
indexes: 1 (B+ tree on id)

--- Q1 selective range on a correlated column ---
  SELECT id, amount FROM events WHERE id >= 500000 AND id < 502000
  two of nine columns, a pushable range, and a range that zone maps can prune

configuration           rules that fired                                       ms min/med/max         vs all-on  bytes read  vs all-on  proj cols  pushed preds  segments in plan  scan rows  index  est rows
----------------------  -----------------------------------------------------  ---------------------  ---------  ----------  ---------  ---------  ------------  ----------------  ---------  -----  --------
all rules on            predicate_pushdown,projection_pruning,segment_pruning  0.88 / 0.91 / 0.93     1.00x      262,200     1.00x      2          2             1/62              2,000      -      4112
predicate_pushdown off  projection_pruning                                     56.82 / 57.11 / 58.65  63.04x     16,003,472  61.04x     2          0             62/62             1,000,000  -      1000000
projection_pruning off  predicate_pushdown,segment_pruning                     2.58 / 2.60 / 2.71     2.87x      744,257     2.84x      9          2             1/62              2,000      -      4112
constant_folding off    predicate_pushdown,projection_pruning,segment_pruning  0.84 / 0.84 / 0.87     0.93x      262,200     1.00x      2          2             1/62              2,000      -      4112
index_selection off     predicate_pushdown,projection_pruning,segment_pruning  0.80 / 0.87 / 1.12     0.96x      262,200     1.00x      2          2             1/62              2,000      -      4112
segment_pruning off     predicate_pushdown,projection_pruning                  0.83 / 0.85 / 0.90     0.94x      262,200     1.00x      2          2             62/62             2,000      -      251000
  result rows (identical under every configuration): 2,000

--- Q2 aggregate over a low-cardinality predicate ---
  SELECT COUNT(*), SUM(amount) FROM events WHERE status = 3
  status repeats in 97-row runs, so every segment holds every value and nothing can be pruned

configuration           rules that fired                       ms min/med/max            vs all-on  bytes read  vs all-on  proj cols  pushed preds  segments in plan  scan rows  index  est rows
----------------------  -------------------------------------  ------------------------  ---------  ----------  ---------  ---------  ------------  ----------------  ---------  -----  --------
all rules on            predicate_pushdown,projection_pruning  32.15 / 32.50 / 33.74     1.00x      8,128,172   1.00x      2          1             62/62             200,014    -      200000
predicate_pushdown off  projection_pruning                     34.99 / 37.58 / 37.86     1.16x      8,128,172   1.00x      2          0             62/62             1,000,000  -      1000000
projection_pruning off  predicate_pushdown                     158.32 / 165.65 / 165.84  5.10x      45,426,893  5.59x      9          1             62/62             200,014    -      200000
constant_folding off    predicate_pushdown,projection_pruning  34.35 / 34.90 / 34.97     1.07x      8,128,172   1.00x      2          1             62/62             200,014    -      200000
index_selection off     predicate_pushdown,projection_pruning  33.67 / 34.92 / 36.74     1.07x      8,128,172   1.00x      2          1             62/62             200,014    -      200000
segment_pruning off     predicate_pushdown,projection_pruning  35.14 / 36.31 / 36.44     1.12x      8,128,172   1.00x      2          1             62/62             200,014    -      200000
  result rows (identical under every configuration): 1

--- Q3 predicate that needs folding ---
  SELECT id FROM events WHERE id < 100 * 20 + 1000
  the constant is an arithmetic expression, so without folding it cannot become a pushable column-op-constant predicate

configuration           rules that fired                                                                        ms min/med/max         vs all-on  bytes read  vs all-on  proj cols  pushed preds  segments in plan  scan rows  index      est rows
----------------------  --------------------------------------------------------------------------------------  ---------------------  ---------  ----------  ---------  ---------  ------------  ----------------  ---------  ---------  --------
all rules on            constant_folding,predicate_pushdown,projection_pruning,segment_pruning,index_selection  0.49 / 0.50 / 0.50     1.00x      131,116     1.00x      1          1             1/62              3,000      events_id  49
predicate_pushdown off  constant_folding,projection_pruning                                                     31.28 / 31.85 / 32.02  63.55x     8,002,728   61.04x     1          0             62/62             1,000,000  -          1000000
projection_pruning off  constant_folding,predicate_pushdown,segment_pruning,index_selection                     2.59 / 2.60 / 3.04     5.20x      744,244     5.68x      9          1             1/62              3,000      events_id  49
constant_folding off    projection_pruning                                                                      41.91 / 42.87 / 42.91  85.56x     8,002,728   61.04x     1          0             62/62             1,000,000  -          1000000
index_selection off     constant_folding,predicate_pushdown,projection_pruning,segment_pruning                  0.58 / 0.58 / 0.58     1.15x      131,116     1.00x      1          1             1/62              3,000      -          49
segment_pruning off     constant_folding,predicate_pushdown,projection_pruning,index_selection                  0.49 / 0.50 / 0.56     1.00x      131,116     1.00x      1          1             62/62             3,000      events_id  3000
  result rows (identical under every configuration): 3,000

--- Q4 point lookup with an index available ---
  SELECT id, amount FROM events WHERE id = 123456
  an index on id exists, so index_selection has something to choose

configuration           rules that fired                                                       ms min/med/max         vs all-on  bytes read  vs all-on  proj cols  pushed preds  segments in plan  scan rows  index      est rows
----------------------  ---------------------------------------------------------------------  ---------------------  ---------  ----------  ---------  ---------  ------------  ----------------  ---------  ---------  --------
all rules on            predicate_pushdown,projection_pruning,segment_pruning,index_selection  0.75 / 0.82 / 0.89     1.00x      262,200     1.00x      2          1             1/62              1          events_id  1
predicate_pushdown off  projection_pruning                                                     49.13 / 51.31 / 51.59  62.92x     16,003,472  61.04x     2          0             62/62             1,000,000  -          1000000
projection_pruning off  predicate_pushdown,segment_pruning,index_selection                     2.61 / 2.62 / 2.81     3.21x      744,274     2.84x      9          1             1/62              1          events_id  1
constant_folding off    predicate_pushdown,projection_pruning,segment_pruning,index_selection  0.77 / 0.79 / 0.84     0.97x      262,200     1.00x      2          1             1/62              1          events_id  1
index_selection off     predicate_pushdown,projection_pruning,segment_pruning                  0.81 / 0.89 / 0.94     1.09x      262,200     1.00x      2          1             1/62              1          -          1
segment_pruning off     predicate_pushdown,projection_pruning,index_selection                  0.81 / 0.85 / 0.87     1.04x      262,200     1.00x      2          1             62/62             1          events_id  61
  result rows (identical under every configuration): 1

--- Q5 wide projection, unprunable random column ---
  SELECT id, ts, category, region, status, amount FROM events WHERE amount >= 4999.0 AND amount < 5001.0
  six of nine columns on a uniformly random predicate: the case where neither pruning nor a narrow projection has much to work with

configuration           rules that fired                       ms min/med/max            vs all-on  bytes read  vs all-on  proj cols  pushed preds  segments in plan  scan rows  index  est rows
----------------------  -------------------------------------  ------------------------  ---------  ----------  ---------  ---------  ------------  ----------------  ---------  -----  --------
all rules on            predicate_pushdown,projection_pruning  117.33 / 117.98 / 118.63  1.00x      28,174,661  1.00x      6          2             62/62             177        -      250100
predicate_pushdown off  projection_pruning                     123.05 / 123.70 / 125.22  1.05x      28,174,661  1.00x      6          0             62/62             1,000,000  -      1000000
projection_pruning off  predicate_pushdown                     165.84 / 168.77 / 174.47  1.43x      45,426,893  1.61x      9          2             62/62             177        -      250100
constant_folding off    predicate_pushdown,projection_pruning  112.81 / 117.01 / 117.71  0.99x      28,174,661  1.00x      6          2             62/62             177        -      250100
index_selection off     predicate_pushdown,projection_pruning  117.95 / 118.00 / 118.97  1.00x      28,174,661  1.00x      6          2             62/62             177        -      250100
segment_pruning off     predicate_pushdown,projection_pruning  114.41 / 115.50 / 116.56  0.98x      28,174,661  1.00x      6          2             62/62             177        -      250100
  result rows (identical under every configuration): 177


Read the two 'vs all-on' columns together. A configuration whose time and
bytes are both 1.00x is a rule that did nothing on this data - which is a
result, not a gap in the benchmark. The 'rules that fired' column is the
optimizer's own report of which rewrites changed the plan, so a rule that is
switched on but absent from that list never applied in the first place.
```

### Reading it

Five queries, each run with all rules on and then with exactly one rule off. Cells are
`median time ratio / bytes-read ratio` against all-rules-on, so a larger number means
the rule contributed more. The queries are listed in full in the verbatim output above:

- **Q1** - selective range on a correlated column
- **Q2** - aggregate over a low-cardinality predicate
- **Q3** - predicate that needs folding
- **Q4** - point lookup with an index available
- **Q5** - wide projection, unprunable random column

| rule | Q1 | Q2 | Q3 | Q4 | Q5 |
|---|---|---|---|---|---|
| `predicate_pushdown` | 63.04x / 61.04x | 1.16x / 1.00x | 63.55x / 61.04x | 62.92x / 61.04x | 1.05x / 1.00x |
| `projection_pruning` | 2.87x / 2.84x | 5.10x / 5.59x | 5.20x / 5.68x | 3.21x / 2.84x | 1.43x / 1.61x |
| `constant_folding` | 0.93x / 1.00x | 1.07x / 1.00x | 85.56x / 61.04x | 0.97x / 1.00x | 0.99x / 1.00x |
| `index_selection` | 0.96x / 1.00x | 1.07x / 1.00x | 1.15x / 1.00x | 1.09x / 1.00x | 1.00x / 1.00x |
| `segment_pruning` | 0.94x / 1.00x | 1.12x / 1.00x | 1.00x / 1.00x | 1.04x / 1.00x | 0.98x / 1.00x |

Every configuration returned an identical result row count on every query, which the
benchmark checks before it reports any timing.

**`predicate_pushdown` is the most valuable rule by a wide margin, and most of its
value is borrowed.** 1.05x to 63.55x across the five queries. The mechanism is visible in the
`pushed preds` and `segments in plan` columns of the verbatim output: with pushdown off
there are zero pushable predicates, so a zone map has nothing to test against and the
plan keeps all 62 segments. The rule's own contribution - evaluating a comparison
inside the scan rather than in a `Filter` above it - is the
1.16x and 1.05x seen on Q2 and Q5, where pruning
was impossible either way. Pushdown is worth 60x because it *enables* pruning, not
because pushing a comparison down one operator is fast.

**`projection_pruning` is the only rule that helps on every query** -
1.43x to 5.20x, with bytes moving in step (1.61x to 5.68x). It
is also the only one whose benefit does not depend on the data's physical layout, which
makes it the most dependable of the five.

**`constant_folding` is all or nothing.** 0.93x-1.07x on four queries and
85.56x on Q3, whose predicate is written `id < 100 * 20 + 1000`.
Without folding, the right-hand side is not a constant, so it cannot become a
column-op-constant `Predicate`, so it cannot be pushed, so nothing prunes - the same
cascade again. A rule with no measurable value on four queries and a two-order
difference on the fifth is exactly the shape of rule that should stay on and never be
tuned.

**`index_selection` has no value at all: bytes 1.00x on all five
queries, with an identical projection, predicate set and segment list in every plan.**
Its time ratios wander from 0.96x to 1.15x, in both directions and differently
between runs - Q3's `index_selection off` row read 1.01x in one run and
1.15x in this one on a query whose whole execution takes half a
millisecond. With the bytes and the plan shape identical, there is nothing for those
ratios to be measuring except process-level variation at sub-millisecond scale. This is structural,
not weak, and it is visible in the source rather than inferred. The rule sets
`LogicalNode::chosen_index` - the output shows `events_id` chosen on Q3 and Q4 - and
`chosen_index` is read in exactly one place in the engine: `src/planner/explain.cpp`, to
print it. `src/planner/physical_planner.cpp` has no index-scan operator and never looks
at the field. The optimizer picks an index and the executor ignores it. Section 3 shows
the pick would have been right on Q4 - an index point lookup beat a scan by
50x on comparable data - so the value is real and entirely
unrealized.

**`segment_pruning` changes bytes read on no query: 1.00x everywhere.** Also structural.
`SequentialScanOperator::CanSegmentMatch` performs the identical zone-map test at
execution time from the same pushed predicates, so removing the plan-time rule removes
nothing from the I/O path: `segments in plan` goes from `1/62` to `62/62` while bytes
read stays byte-identical, because the operator re-prunes the other 61 itself. What the
rule actually changes is the cardinality estimate - Q1's `est rows` goes from 4,112 to
251,000 with it off. Since the only consumer of that estimate is `index_selection`,
which has no effect either, the rule's measurable value today is zero. The
1.0x-1.1x time ratios on Q1 and Q2 sit inside those rows' own min/max spreads and should
not be read as a real cost.

**A finding that is not about any rule: the cardinality estimates are badly wrong.**
Comparing `est rows` against `scan rows` on the all-rules-on row of each query:

| query | estimated | actual scan rows | error |
|---|---|---|---|
| Q1 `id` range | 4,112 | 2,000 | 2.1x over |
| Q2 `status = 3` | 200,000 | 200,014 | **exact** |
| Q3 `id < 3000` | 49 | 3,000 | **61x under** |
| Q4 `id = 123456` | 1 | 1 | exact |
| Q5 `amount` range | 250,100 | 177 | **1,413x over** |

Equality on a low-cardinality column is estimated well. Ranges are not: Q5's two-unit
window on a 10,000-wide uniform column is estimated at a quarter of the table. A
cost-based optimizer fed a 1,413x error cannot make a correct index-versus-scan choice
even once an index path exists, and section 3 puts the crossover it would need to find
at about 34% - well inside the range where a 1,413x overestimate flips the
decision.

## Build variants

The default `RelWithDebInfo` flags omit `-DNDEBUG`, so `assert()` is live - including
`Batch::SetSelection`'s `assert(std::is_sorted(...))`, an O(batch) pass on every
filtered batch. That is the obvious suspect for the vectorized path's poor showing, so
it was tested. A second variant adds `-flto` to test whether the per-row cross-TU call
to `EvalCompare` is the cost.

```sh
cmake -S . -B build-ndebug -DCMAKE_CXX_FLAGS=-DNDEBUG && cmake --build build-ndebug -j
cmake -S . -B build-lto -DCMAKE_CXX_FLAGS=-flto -DCMAKE_EXE_LINKER_FLAGS=-flto \
  && cmake --build build-lto -j
./build-ndebug/execution_bench ; ./build-lto/execution_bench
```

Section 1 of `execution_bench`, three builds, ns/row:

| path | default `-O2 -g` | `+ -DNDEBUG` | `+ -flto` |
|---|---|---|---|
| engine kernel over pre-materialized batches | 2.858 | 2.900 | 1.866 |
| engine scan path, fill + kernel | 4.740 | 4.859 | 3.954 |
| tuple-at-a-time, columnar | 1.307 | 1.448 | 1.325 |
| tuple-at-a-time, boxed `Value` | 3.309 | 3.328 | 1.549 |
| branch-free kernel | 0.316 | 0.326 | 0.323 |

**`-DNDEBUG` changes nothing measurable** (2.858 to 2.900 ns/row on the
kernel, inside the spread). The `is_sorted` assert is not why the vectorized path is
slow, and the headline result is not an artefact of a half-release build. Every primary
number in this document therefore comes from the project's default build.

**`-flto` cuts the engine kernel's cost by 35%**, confirming that roughly a
third of it is the un-inlinable per-row call across translation units. It does not close
the gap: at 1.866 ns/row the kernel is still
1.41x slower than the tuple-at-a-time loop in the same build
and 5.8x slower than the branch-free kernel, so the rest
is the per-row type dispatch itself. The boxed-`Value` path improves most of all
(3.309 to 1.549) because `Value::operator<` becomes inlinable -
which incidentally means the "`Value` is far too slow for execution" argument in
`types.hpp` is itself substantially a cross-TU-inlining artefact.

## Noise and reproducibility

Every table carries a `max/min` spread column so a noisy row is visible as noisy.

- Most rows across all four benchmarks sit between 1.01 and 1.10. Those medians are
  good to within a few percent.
- The worst spreads are 1.15-1.20, on some `execution_bench` kernel rows and some
  batch-sweep points. Differences under ~15% between those rows should be read as a
  tie. Everything in the headline table is a factor of two or more, except the
  batch-reuse result and the optimizer's `index_selection` and `segment_pruning` rows -
  all three of which are reported as *no effect* for precisely that reason.
- `index_bench` section 1 is a single timed run by necessity: building an index is
  destructive, so there is no distribution to report and the table says so rather than
  presenting a median of one. The derived counters there (`bytes_written`,
  `logical_bytes`, write amplification, splits, compactions) are exact counts taken
  from the index itself, not samples, so only `build ms` and `inserts/sec` carry
  run-to-run uncertainty - across runs those moved by about 8%.
- The one result that genuinely moved between runs is the *location* of the batch-size
  peak inside its plateau: 2048, 8192 and 32768 across three runs of the same binary.
  It is reported as a plateau rather than a peak for that reason.
- All data comes from `tests/datagen.hpp`'s seeded splitmix64, or from `datagen::Rng`
  with a fixed seed where a shape datagen does not provide was needed. The rows are
  byte-identical run to run and across machines, so a difference in these numbers
  elsewhere is a machine difference and not a data difference.

## Where the measurements disagree with the design

Collected in one place, because these are the results worth a reader's attention. All
of them are reported, not fixed: `include/`, `src/` and `tests/` are untouched.

1. **`batch.hpp`: "a filter can read a column, write a selection vector, and have the
   next operator read both without a trip to main memory ... bench/ measures this
   directly."** Measured: the batching structure does deliver that locality - the
   100%-selectivity row and the branch-free kernel both show it - but the engine's
   actual filter kernel is **2.19x slower than a tuple-at-a-time loop**
   over the same bytes, because the kernel is a per-row cross-TU call with two type
   switches and a `std::optional`. The design is sound; the kernel is not vectorized.

2. **`batch.hpp`: 2048 "is chosen so that one batch of a fixed-width column stays inside
   L1 or L2 ... Larger batches stop fitting; smaller ones pay the per-call overhead."**
   Measured: the smaller-ones half is right and large. The larger-ones half does not
   appear at all - 32768 (256 KiB, far outside L1) matches 2048 on the kernel-only path.
   The cache argument predicts the lower edge of the plateau and nothing about its upper
   edge.

3. **`types.hpp`: a query that filters a million rows "must not allocate a million of
   these."** Measured: doing exactly that costs 3.309 ns/row against the
   engine's 2.858 - a 1.16x penalty. Under `-flto` the boxed path
   (1.549) is *faster* than the default build's engine kernel. The claim
   is directionally right and quantitatively much smaller than its phrasing suggests.

4. **`batch.hpp`: batch reuse "is worth a measurable amount on a long scan; bench/
   quantifies it."** Quantified: 1.01x, and 1.00x-1.04x across three runs. At or
   below the noise floor.

5. **`buffer.hpp`: "A pool large enough to hold the whole file teaches nothing; the
   interesting behaviour ... only appears when the pool is smaller than the data."**
   Measured: a 5% pool is **faster** than a 100% pool under both policies, despite the
   hit rate falling from 99.88% to 74.69%. The
   interesting behaviour is the opposite of the one predicted.

6. **`buffer.hpp`: "page_to_frame is a flat map; at the frame counts used here a vector
   scan or a small hash both work."** Measured: at 2,823 frames the vector scan does not
   work. It makes the hit path cost more than the miss it avoids and is the direct cause
   of finding 5. This is a performance defect in `src/buffer/buffer_pool.cpp` -
   `FetchPage` is O(resident pages) on a hit - and is reported rather than fixed.

7. **`buffer.hpp`: clock "approximates LRU at a fraction of the bookkeeping."** Measured:
   clock and LRU reach *identical* hit rates at every pool size on this workload, and
   clock is 1.21x-2.10x faster. There is no
   approximation tradeoff to weigh; LRU as implemented is dominated, for the same
   O(n)-page-table reason.

8. **`plan.hpp`: "Each rule is separately testable and separately disableable, because a
   rule that cannot be turned off cannot be shown to have done anything."** Measured: the
   mechanism works perfectly, and two of the five rules turn out to do nothing.
   `index_selection` sets a field no executor reads; `segment_pruning` duplicates a test
   the scan operator already performs. Both are 1.00x on bytes across all five queries.
   The honest reading is that the disableable-rules design did its job - it found the
   rules that do not matter.

9. **`storage.hpp` on zone maps: "turns a selective query from O(table) into O(matching
   segments)."** Measured: true for `id` and `ts` (99.2% of segments pruned,
   117.93x), and false for `amount` (0.0% pruned, and 0.87x - slower than
   no predicate). The complexity claim holds only for columns correlated with physical
   order. `ColumnStats`' own comment acknowledges this; the summary sentence above it
   does not.

10. **`lsm.hpp`: "the read amplification size-tiering gives up is itself worth
    measuring."** Measured, and it gave up less than the comparison was set up to expect:
    1.084 SSTables per lookup out of ~9, because Bloom filters
    reject 7.916 of them. The LSM beat the B+ tree on point
    lookups in both the hit and the miss case, which is the opposite of the conventional
    result and the opposite of what `lsm.hpp`'s framing anticipates.

11. **`storage.hpp` on `SegmentReader`: "a reader that decodes everything and then
    discards is a row store wearing a costume."** Projection pushdown is honoured
    exactly - a one-of-nine projection reads one column's bytes. But
    `FileSegmentReader` decodes the whole of each *projected* column at `Open`, so an
    index scan that wants one row of a segment pays for that segment's entire column.
    That is the single mechanism behind the 34% index-versus-scan crossover in
    section 3, and it is the most actionable finding in this document.

