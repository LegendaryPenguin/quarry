#!/usr/bin/env bash
#
# demo.sh — build Quarry, load a generated dataset, and walk through the claims
# the project makes, one at a time, showing the evidence for each.
#
# This is the script to run first. Every step prints what it is about to prove
# before it proves it, and the SQL it runs is printed too, so nothing here is a
# black box: each claim can be re-checked by hand in the shell afterwards.
#
# Environment:
#   QUARRY_BUILD_DIR    build directory            (default: ./build)
#   QUARRY_DEMO_DIR     scratch directory          (default: a fresh mktemp dir)
#   QUARRY_DEMO_ROWS    rows in the fact table     (default: 20000)
#   QUARRY_KEEP         set to 1 to keep the databases after the run

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${QUARRY_BUILD_DIR:-$ROOT/build}"
ROWS="${QUARRY_DEMO_ROWS:-20000}"

# Segment granularity is the knob the pruning steps depend on. 2048 rows per
# segment over 20000 rows is ten segments, which is enough that a selective
# predicate can skip most of them and few enough that the counts stay readable.
ROWS_PER_SEGMENT=2048

# A deliberately small buffer pool. A pool large enough to hold the whole index
# would make the hit rate 100% and prove nothing about replacement.
BUFFER_FRAMES=64

if [[ -n "${QUARRY_DEMO_DIR:-}" ]]; then
  WORK="$QUARRY_DEMO_DIR"
  mkdir -p "$WORK"
else
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/quarry-demo.XXXXXX")"
fi

cleanup() {
  if [[ "${QUARRY_KEEP:-0}" != "1" && -z "${QUARRY_DEMO_DIR:-}" ]]; then
    rm -rf "$WORK"
  else
    printf '\ndatabases left in %s\n' "$WORK"
  fi
}
trap cleanup EXIT

step_name=""
FAILED_STEPS=()

on_error() {
  printf '\n!! the demo stopped during: %s\n' "${step_name:-setup}" >&2
  printf '!! the databases are in %s\n' "$WORK" >&2
  trap - EXIT
}
trap on_error ERR

# ---------------------------------------------------------------------------
# Presentation
# ---------------------------------------------------------------------------

rule() { printf '%s\n' "------------------------------------------------------------------------"; }

section() {
  step_name="$1"
  printf '\n'
  rule
  printf '%s\n' "$1"
  rule
  shift
  for line in "$@"; do printf '  %s\n' "$line"; done
  printf '\n'
}

# run_sql prints the script it is about to execute and then executes it, so the
# output below is always next to the input that produced it.
DB_MAIN="$WORK/events.db"
run_sql() {
  local db="$1" file="$2"
  printf '  $ quarry_shell %s -f %s\n' "$(basename "$db")" "$(basename "$file")"
  sed 's/^/  | /' "$file"
  printf '\n'
  # A failing step is reported and the walkthrough continues. Stopping dead would
  # hide every later step behind the first unimplemented one, and the point of a
  # narrated demo is to show what does work alongside what does not. The exit
  # status at the end still reflects every failure.
  if ! "$BUILD_DIR/quarry_shell" \
    --rows-per-segment "$ROWS_PER_SEGMENT" \
    --buffer-frames "$BUFFER_FRAMES" \
    -f "$file" "$db"; then
    printf '\n  !! this step reported errors (shown above)\n'
    FAILED_STEPS+=("$step_name")
  fi
}

# run_sql_with_flags is the same thing with extra shell options, used by the step
# that turns an optimizer rule off to show what the rule was doing.
run_sql_with_flags() {
  local db="$1" file="$2"
  shift 2
  printf '  $ quarry_shell %s %s -f %s\n' "$(basename "$db")" "$*" "$(basename "$file")"
  if ! "$BUILD_DIR/quarry_shell" \
    --rows-per-segment "$ROWS_PER_SEGMENT" \
    --buffer-frames "$BUFFER_FRAMES" \
    "$@" -f "$file" "$db"; then
    printf '\n  !! this step reported errors (shown above)\n'
    FAILED_STEPS+=("$step_name")
  fi
}

# ---------------------------------------------------------------------------
# 0. Build
# ---------------------------------------------------------------------------

section "Step 0 — build" \
  "Nothing below means anything if it was measured at -O0, so the build is a" \
  "RelWithDebInfo build with optimization on."

cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target quarry_shell
printf '\nbuilt %s\n' "$BUILD_DIR/quarry_shell"

# ---------------------------------------------------------------------------
# 1. Generate and load
# ---------------------------------------------------------------------------

section "Step 1 — generate data with properties worth testing" \
  "The columns are chosen to make each storage decision visible:" \
  "  id       increases with insertion order  -> zone maps are nearly perfect" \
  "  category eight distinct strings          -> dictionary encoding should win" \
  "  region   changes every few hundred rows  -> run-length encoding should win" \
  "  amount   uniform random                  -> pruning is useless, on purpose" \
  "  quantity 15% NULL, score 30% NULL        -> null handling is exercised" \
  "A column that cannot be pruned is as important as one that can: without it," \
  "the pruning numbers later have nothing to be compared against."

if ! command -v python3 >/dev/null 2>&1; then
  printf 'demo.sh needs python3 to emit the load script\n' >&2
  exit 1
fi

LOAD_SQL="$WORK/load.sql"

# The emitter mirrors tests/datagen.hpp: same splitmix64, same column shapes. The
# tests keep their generator in C++ so the benchmarks can share it; the demo keeps
# its own so that running the demo does not depend on a second build target.
python3 - "$ROWS" > "$LOAD_SQL" <<'PYTHON'
import sys

rows = int(sys.argv[1])
MASK = (1 << 64) - 1
CATEGORIES = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"]
REGIONS = ["us-east", "us-west", "eu-central", "ap-south"]
REGION_RUN = 433
STATUS_RUN = 97

state = 0x5EED1234


def nxt():
    global state
    state = (state + 0x9E3779B97F4A7C15) & MASK
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return z ^ (z >> 31)


def below(bound):
    return nxt() % bound


out = []
out.append(
    "CREATE TABLE events (id BIGINT NOT NULL, ts BIGINT NOT NULL, "
    "category VARCHAR NOT NULL, region VARCHAR NOT NULL, status BIGINT NOT NULL, "
    "amount DOUBLE NOT NULL, quantity BIGINT, score DOUBLE, active BOOLEAN NOT NULL);"
)
out.append("CREATE TABLE dims (category VARCHAR NOT NULL, weight BIGINT NOT NULL, tier VARCHAR NOT NULL);")

dims = []
for c in range(6):
    tier = "gold" if c % 3 == 0 else ("silver" if c % 3 == 1 else "bronze")
    dims.append("('%s', %d, '%s')" % (CATEGORIES[c], 10 + c * 7, tier))
out.append("INSERT INTO dims (category, weight, tier) VALUES " + ", ".join(dims) + ";")

ts = 1700000000
batch = []
BATCH = 500
for i in range(rows):
    ts += below(9)
    category = CATEGORIES[below(8)]
    region = REGIONS[(i // REGION_RUN) % 4]
    status = (i // STATUS_RUN) % 5
    amount = below(640000) / 64.0
    quantity = "NULL" if below(10000) < 1500 else str(below(1000000))
    score = "NULL" if below(10000) < 3000 else "%.6f" % (below(6400) / 64.0)
    active = "true" if below(4) != 0 else "false"
    batch.append(
        "(%d, %d, '%s', '%s', %d, %.6f, %s, %s, %s)"
        % (i, ts, category, region, status, amount, quantity, score, active)
    )
    if len(batch) == BATCH:
        out.append(
            "INSERT INTO events (id, ts, category, region, status, amount, quantity, score, active) "
            "VALUES " + ", ".join(batch) + ";"
        )
        batch = []
if batch:
    out.append(
        "INSERT INTO events (id, ts, category, region, status, amount, quantity, score, active) "
        "VALUES " + ", ".join(batch) + ";"
    )

print("\n".join(out))
PYTHON

printf 'wrote %s rows of load SQL to %s (%s)\n' "$ROWS" "$LOAD_SQL" \
  "$(wc -c < "$LOAD_SQL" | tr -d ' ') bytes"

printf '\nloading...\n'
"$BUILD_DIR/quarry_shell" --rows-per-segment "$ROWS_PER_SEGMENT" \
  --buffer-frames "$BUFFER_FRAMES" -f "$LOAD_SQL" "$DB_MAIN" | tail -n 3

cat > "$WORK/01-schema.sql" <<'SQL'
.tables
.schema events
SQL
run_sql "$DB_MAIN" "$WORK/01-schema.sql"

# ---------------------------------------------------------------------------
# 2. A plain scan
# ---------------------------------------------------------------------------

section "Step 2 — a plain scan touches every segment" \
  "The baseline. With no predicate there is nothing to prune, so the plan must" \
  "keep every segment and the scan must read every one. The .stats line below" \
  "is the control for Step 3: without a run where nothing is skipped, the" \
  "numbers in the next step would be unfalsifiable."

cat > "$WORK/02-scan.sql" <<'SQL'
SELECT COUNT(*) FROM events;
SELECT id, category, region, amount FROM events ORDER BY id LIMIT 5;
.stats
SQL
run_sql "$DB_MAIN" "$WORK/02-scan.sql"

# ---------------------------------------------------------------------------
# 3. Zone-map pruning
# ---------------------------------------------------------------------------

section "Step 3 — a selective predicate skips segments without reading them" \
  "id increases with insertion order, so each segment's recorded [min,max] for" \
  "id covers a narrow, almost disjoint range. A predicate that cannot be" \
  "satisfied anywhere in that range lets the plan drop the segment before the" \
  "scan opens it, so not a single value in it is decoded. Compare the segment" \
  "counts here against Step 2's: same table, same scan, almost all of them gone." \
  "" \
  "Then the same query against amount, which is uniform random: every segment's" \
  "[min,max] covers nearly the whole domain, so nothing is prunable. Same" \
  "selectivity, opposite outcome. That contrast is the honest version of this" \
  "claim — pruning is a property of the data's layout, not of the engine being" \
  "clever."

cat > "$WORK/03a-prune-good.sql" <<'SQL'
SELECT COUNT(*) FROM events WHERE id > 19000;
.stats
SQL
run_sql "$DB_MAIN" "$WORK/03a-prune-good.sql"

cat > "$WORK/03b-prune-useless.sql" <<'SQL'
SELECT COUNT(*) FROM events WHERE amount > 9500.0;
.stats
SQL
run_sql "$DB_MAIN" "$WORK/03b-prune-useless.sql"

# ---------------------------------------------------------------------------
# 4. EXPLAIN: predicate pushdown and projection pruning
# ---------------------------------------------------------------------------

section "Step 4 — EXPLAIN shows predicate pushdown and projection pruning" \
  "The query asks for two of nine columns and filters on one of them. Two" \
  "rewrites should be visible in the plan:" \
  "  predicate pushdown  — the comparison is evaluated inside the scan, so the" \
  "                        zone map can use it, instead of by a Filter above it" \
  "  projection pruning  — the scan reads only id and amount, so seven columns'" \
  "                        worth of bytes are never touched" \
  "Both are then shown again with the rule turned off. A rule that cannot be" \
  "disabled cannot be shown to have done anything, so the second plan is the" \
  "proof rather than the first."

cat > "$WORK/04-explain.sql" <<'SQL'
.explain SELECT id, amount FROM events WHERE id > 19500
SQL
run_sql "$DB_MAIN" "$WORK/04-explain.sql"

printf '  ... and with both rewrites disabled, for comparison:\n\n'
run_sql_with_flags "$DB_MAIN" "$WORK/04-explain.sql" \
  --disable predicate_pushdown --disable projection_pruning

# ---------------------------------------------------------------------------
# 5. EXPLAIN ANALYZE: estimate versus reality
# ---------------------------------------------------------------------------

section "Step 5 — EXPLAIN ANALYZE prints the estimate next to the truth" \
  "The optimizer's row counts come from segment statistics, so they are" \
  "estimates and some of them are wrong. Printing estimated and actual side by" \
  "side is the point: a cost model whose errors are hidden is a cost model" \
  "nobody can improve." \
  "" \
  "Watch the two predicates below. id > 19500 is a range on a column whose" \
  "min/max per segment is tight, so the estimate should be close. The predicate" \
  "on category is equality on a dictionary-encoded column where the estimate" \
  "rests on a distinct-value guess, so it has more room to be wrong."

cat > "$WORK/05-analyze.sql" <<'SQL'
.analyze SELECT id, amount FROM events WHERE id > 19500
.analyze SELECT id, amount FROM events WHERE category = 'delta'
SQL
run_sql "$DB_MAIN" "$WORK/05-analyze.sql"

# ---------------------------------------------------------------------------
# 6. Aggregation
# ---------------------------------------------------------------------------

section "Step 6 — aggregation, including the NULL rules" \
  "All five aggregates, grouped and ungrouped. The row worth checking is" \
  "COUNT(*) against COUNT(quantity): quantity is 15% NULL, and COUNT of a" \
  "column must skip NULLs while COUNT(*) must not. SUM and AVG over the same" \
  "column must skip them too, which is why AVG is not SUM divided by COUNT(*)."

cat > "$WORK/06-aggregate.sql" <<'SQL'
SELECT COUNT(*), COUNT(quantity), SUM(quantity), AVG(quantity), MIN(quantity), MAX(quantity) FROM events;
SELECT category, COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM events GROUP BY category ORDER BY category;
SELECT region, status, COUNT(*) FROM events GROUP BY region, status ORDER BY region, status;
SQL
run_sql "$DB_MAIN" "$WORK/06-aggregate.sql"

# ---------------------------------------------------------------------------
# 7. Join
# ---------------------------------------------------------------------------

section "Step 7 — a single-column equi inner join" \
  "dims holds six of the eight categories, so an inner join must drop the rows" \
  "whose category has no match. If the row count below equalled the fact" \
  "table's, the join would be behaving like a left join and the result would be" \
  "wrong in a way that is easy to miss." \
  "The plan is printed too, so the build side it chose is visible: the small" \
  "table should be built and the large one probed, not the other way round."

cat > "$WORK/07-join.sql" <<'SQL'
SELECT COUNT(*) FROM events;
SELECT COUNT(*) FROM events INNER JOIN dims ON events.category = dims.category;
SELECT tier, COUNT(*), SUM(amount) FROM events INNER JOIN dims ON events.category = dims.category GROUP BY tier ORDER BY tier;
.explain SELECT id, weight FROM events INNER JOIN dims ON events.category = dims.category
SQL
run_sql "$DB_MAIN" "$WORK/07-join.sql"

# ---------------------------------------------------------------------------
# 8. B+ tree versus LSM
# ---------------------------------------------------------------------------

section "Step 8 — the same query over a B+ tree and over an LSM index" \
  "Two databases, identical data, identical query. The only difference is which" \
  "structure backs the index on id. Each database loads the same generated SQL," \
  "so a difference below is a difference between the structures and not between" \
  "two datasets." \
  "" \
  "What to look for: the B+ tree should answer a point lookup in a fixed number" \
  "of page reads, while the LSM may consult several sorted runs and reject most" \
  "of them with a bloom filter. The buffer pool is deliberately 64 frames, so" \
  "neither index fits entirely in memory and replacement actually happens." \
  "" \
  "Read the plan line first. If both plans say SeqScan, the optimizer did not" \
  "choose either index and there is nothing to compare: the two timings below" \
  "would then be two measurements of the same sequential scan. That is a result" \
  "too, and the plan is the only place it is visible — which is exactly why this" \
  "step prints the plan instead of only the elapsed time."

DB_LSM="$WORK/events-lsm.db"
printf '  loading the second database (same SQL, same rows)...\n'
"$BUILD_DIR/quarry_shell" --rows-per-segment "$ROWS_PER_SEGMENT" \
  --buffer-frames "$BUFFER_FRAMES" -f "$LOAD_SQL" "$DB_LSM" | tail -n 1

cat > "$WORK/08a-btree.sql" <<'SQL'
CREATE INDEX idx_id ON events (id);
.schema events
.analyze SELECT id, amount FROM events WHERE id = 17321
.analyze SELECT id, amount FROM events WHERE id > 19000 AND id < 19100
.stats
SQL
printf '\n  --- B+ tree ---\n\n'
run_sql "$DB_MAIN" "$WORK/08a-btree.sql"

cat > "$WORK/08b-lsm.sql" <<'SQL'
CREATE INDEX idx_id ON events (id) USING LSM;
.schema events
.analyze SELECT id, amount FROM events WHERE id = 17321
.analyze SELECT id, amount FROM events WHERE id > 19000 AND id < 19100
.stats
SQL
printf '\n  --- LSM ---\n\n'
run_sql "$DB_LSM" "$WORK/08b-lsm.sql"

# ---------------------------------------------------------------------------

step_name=""
rule
if [[ ${#FAILED_STEPS[@]} -eq 0 ]]; then
  printf 'demo complete: every step ran clean.\n'
else
  printf 'demo complete, but %d step(s) reported errors:\n' "${#FAILED_STEPS[@]}"
  for failed in "${FAILED_STEPS[@]}"; do printf '  - %s\n' "$failed"; done
fi
printf '\nTo keep the databases and poke at them yourself:\n'
printf '  QUARRY_KEEP=1 %s\n' "${BASH_SOURCE[0]}"
printf '  %s <the directory it prints>/events.db\n' "$BUILD_DIR/quarry_shell"
printf '\nTo check the answers against SQLite rather than against this script:\n'
printf '  %s/scripts/run-differential.sh\n' "$ROOT"
rule

# FAILED_STEPS is empty on a clean run, so this is zero only when every step ran.
exit $(( ${#FAILED_STEPS[@]} > 0 ? 1 : 0 ))
