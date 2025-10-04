# Quarry benchmarks

Four benchmark executables, one per question the project makes a claim about. Each
is a plain `main()` that prints tables to stdout; there is no framework, no
configuration file, and no hidden state between runs. Measured output is in
[results.md](results.md).

Every benchmark here is built against the unmodified engine. Nothing under
`include/`, `src/`, or `tests/` is touched. Where a comparison needs something the
engine does not have — a tuple-at-a-time filter, a branch-free kernel, an index
scan that fetches rows — that code lives in `bench/` and is clearly labelled as
the contrast rather than as part of the engine.

## Reproducing

```sh
cd /path/to/quarry
cmake -S . -B build                   # defaults to RelWithDebInfo: -O2 -g
cmake --build build -j

./build/execution_bench
./build/storage_bench
./build/index_bench
./build/optimizer_bench
```

All four together take about 70 seconds on the machine recorded in results.md.
`index_bench` is the slow one (~45 s), most of it spent building the two indexes
and walking the index-versus-scan sweep.

Build type matters more than usual here. At `-O0` the vectorization comparison in
`execution_bench` is meaningless — an unoptimized build is the easiest way to
publish a speedup that is really just the optimizer being switched off. The
project's `CMakeLists.txt` defaults `CMAKE_BUILD_TYPE` to `RelWithDebInfo` and
sets `CMAKE_CXX_FLAGS_RELWITHDEBINFO` to `-O2 -g`, with no `-DNDEBUG`, so asserts
are live in the default build. Two variant builds were used to check whether that
matters; both are reported in results.md:

```sh
# asserts compiled out
cmake -S . -B build-ndebug -DCMAKE_CXX_FLAGS=-DNDEBUG && cmake --build build-ndebug -j

# cross-translation-unit inlining enabled
cmake -S . -B build-lto -DCMAKE_CXX_FLAGS=-flto -DCMAKE_EXE_LINKER_FLAGS=-flto \
  && cmake --build build-lto -j
```

## What each file measures

### `bench_util.hpp`

Timing and formatting, shared by all four.

- `Time(name, runs, work, body)` discards a warmup pass, then runs `body` `runs`
  times and keeps the whole distribution. Every timing in every table is reported
  as **min / median / max**, never as one sample, and every table carries a
  `max/min` spread column so a noisy row is visible as noisy. Quote the median.
- `Sink()` / `ClobberMemory()` stop `-O2` from deleting a loop whose result is
  unused. A benchmark where one side got deleted is the single easiest fake
  speedup in a project like this.
- `Table` computes its own column widths; `Rate`, `Bytes`, `Thousands`, `Fmt`
  format numbers that span from thousands of lookups per second to billions of
  rows per second.
- `Check` / `Take` / `Fail` abort loudly on a failed `Status`. A benchmark that
  swallows an error reports a time for work that did not happen.

No data generator lives here: `tests/datagen.hpp` already exists and is reused, so
the benchmarks and the differential tests describe the same rows. Where a
benchmark needs a shape datagen does not provide (a shuffled key permutation, a
column of a chosen group cardinality), it uses `datagen::Rng` — the same seeded
splitmix64 — so the data is still reproducible from a named algorithm.

### `bench_data.hpp`

Building tables without SQL, plus byte accounting.

- `WriteTable` creates a table and writes its segments through `FileSegmentWriter`,
  which is the same pair of calls `Database::FlushPending` makes with the parser
  removed. Loading a million rows as `INSERT ... VALUES` text costs more than every
  measurement in this directory combined, and none of that time is engine behaviour
  worth a number.
- `CountingSegmentReader` wraps `FileSegmentReader` and siphons its `bytes_read()`
  into a shared counter before the scan destroys it. This is how the projection and
  pruning benchmarks get an exact byte count instead of an assertion.
- `Drain` runs an operator to exhaustion and touches every surviving value, so
  nothing downstream of the scan can be optimized away.

### `execution_bench.cpp`

The vectorization question.

1. **Vectorized versus tuple-at-a-time**, 10% selective `<` on an int64 column.
   Six paths over the same 4,000,000-row column: the engine's `ApplyPredicate` over
   pre-materialized batches; the engine's full scan path including the batch fill; a
   row-at-a-time virtual predicate over the same columnar array; the same over a
   row-major copy; the same with `Value` boxing; and a branch-free type-specialized
   kernel as the ceiling. All six are checked to select the same number of rows
   before any timing is reported.

   The comparison that answers the claim is the engine kernel against the
   row-at-a-time loop **over the same columnar bytes**, which isolates the
   iteration model from the layout. Batches are single-column so both sides stream
   exactly 8 bytes per row; a vectorized path charged for materializing four
   columns while the scalar path reads one array would be rigged the other way.

2. **Batch size sweep** at 1, 8, 64, 256, 1024, 2048, 8192, 32768, in two forms:
   kernel-only over pre-materialized batches, and fill+kernel as a real scan pays
   it. The claim in `batch.hpp` is that 2048 int64 values (16 KiB) keeps a column in
   L1/L2 so a downstream operator can re-read it without going to memory, so the
   sweep includes a consumer stage that performs that re-read. "Stops helping" is
   defined as the smallest size within 2% of the peak, not eyeballed.

3. **Reused versus fresh batch** at `kBatchSize`, which `batch.hpp` claims is
   "worth a measurable amount on a long scan".

4. **Selectivity sweep** at 0.1%, 1%, 10%, 50%, 100%, for the engine kernel, the
   tuple path, and the branch-free kernel. The branch-free column is the control
   that separates branch misprediction from everything else.

### `storage_bench.cpp`

1. **Full scan throughput** for 2,000,000 rows in 123 segments, reported as
   rows/sec, on-disk MB/sec (bytes actually `pread`), and logical MB/sec
   (uncompressed row width). Both byte figures are given because they differ by
   the compression ratio and quoting only one is how a storage number gets
   inflated.
2. **Projection pushdown** — `bytes_read` for one column of nine against all nine,
   for four different single columns, measured through the counting reader.
3. **Segment pruning**, the section worth reading. The same 0.1%-selective query
   runs against `id` (strictly increasing, correlated with insertion order), `ts`
   (non-decreasing with jitter, what time-series data actually looks like), and
   `amount` (uniformly random). `segments_scanned` / `segments_pruned` and bytes
   read are reported for all three, against a no-predicate baseline.
4. **Encoding** — plain, dictionary and run-length, each forced on six column
   shapes chosen so every encoding is right for one and wrong for another.
   Encoded size and decode throughput for each, plus what the auto-chooser picked.
5. **GROUP BY throughput** at group cardinalities 2, 16, 256, 4096, 65536,
   1,000,000 over 1,000,000 rows, through the real `HashAggregateOperator`.

### `index_bench.cpp`

B+ tree versus LSM over 500,000 unique int64 keys inserted in shuffled order, and
index versus scan.

1. **Insert throughput and write amplification** — `Index::Stats::WriteAmplification()`
   for both, plus on-disk space amplification, node splits and compactions. One
   timed run each, because building is destructive and there is no distribution to
   report; this is labelled rather than dressed up as a median of one.
2. **Point lookup latency and read amplification** — both structures, keys present
   and keys absent. Two different read-amplification numbers are reported
   separately because conflating them is the easy mistake: `Index::Stats::pages_read`
   counts `FetchPage` calls (the tree's logical access pattern, invariant to pool
   size) while `BufferPool::Stats::reads` counts actual `pread`s. For the LSM,
   `sstables_consulted` and `bloom_rejections` per lookup.
3. **Range scan throughput** at widths 0.01% to 100%, with both structures checked
   to return the same number of row ids for every range.
4. **Index versus full scan** across ten selectivities from a single row to 50%,
   identifying the crossover. The index path here really fetches the matching rows
   out of the segments they live in — resolving a row id is not answering a query —
   so the number is one an optimizer could act on. Key ranges are centred in the
   key domain rather than anchored at its bottom, because an anchored range lets a
   zone map prune on `min` alone and hands the scan a win unrelated to the
   comparison.
5. **Buffer pool sensitivity** — the same 100,000-lookup workload with the pool at
   100%, 50%, 25% and 5% of the tree, for both the clock and LRU policies,
   reporting hit rate, evictions, physical reads per lookup and latency.

### `optimizer_bench.cpp`

Five queries over 1,000,000 rows, each run six times: all rules on, then each of
`predicate_pushdown`, `projection_pruning`, `constant_folding`, `index_selection`
and `segment_pruning` switched off individually. Time and bytes read are reported
for every cell, along with the optimizer's own `applied_rules()` list, the
projection width, the pushed predicate count, the segments left in the plan, and
the estimated versus actual scan row count.

Time is measured end to end through `Database::Execute`. Bytes cannot be: the
physical planner builds its segment readers internally and a finished `Execute()`
cannot be asked what it read. So the query is bound and optimized with the same
`Rules`, and the scan the optimizer produced — its pruned segment list, its
projection, its pushed predicates — is re-run through an instrumented reader. The
scan is the only operator that touches disk, so this is an exact byte count for
the plan the optimizer chose, not an estimate. Every configuration is also checked
to return the same number of result rows; a rule that changes the answer is a bug,
not a speedup.

## Honesty rules these follow

- Nothing is reported as a single sample.
- A path is never timed against another path until both are shown to produce the
  same answer.
- Where a number is a lower or upper bound rather than the engine's real cost — the
  kernel-only rows in `execution_bench`, the single build run in `index_bench` —
  the table or the surrounding note says so.
- Results that contradict a claim in the engine's own header comments are called
  out in results.md rather than buried. There are several.
