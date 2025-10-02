#!/usr/bin/env bash
#
# run-differential.sh — build and run the differential suite against SQLite, then
# print a summary that says whether Quarry and SQLite agree.
#
# The exit status is the verdict: zero only if every query agreed. The summary is
# for a person; the status is for CI.
#
# Environment:
#   QUARRY_BUILD_DIR   build directory              (default: ./build)
#   QUARRY_ASAN        set to 1 to configure a sanitized build and run it too
#   QUARRY_FILTER      substring filter passed to the test binary

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${QUARRY_BUILD_DIR:-$ROOT/build}"

rule() { printf '%s\n' "========================================================================"; }

# Logs live inside the build directory rather than next to it, so a run leaves
# nothing behind in the source tree.
mkdir -p "$BUILD_DIR"
CONFIGURE_LOG="$BUILD_DIR/differential.configure.log"
BUILD_LOG="$BUILD_DIR/differential.build.log"
OUTPUT="$BUILD_DIR/differential.run.log"

CMAKE_ARGS=()
if [[ "${QUARRY_ASAN:-0}" == "1" ]]; then
  # The differential suite is the best sanitizer workload in the project: it runs
  # every operator over a dataset big enough to span segments, so an out-of-bounds
  # read in a decode kernel has somewhere to be caught.
  CMAKE_ARGS+=(-DQUARRY_ASAN=ON)
  printf 'configuring with address and undefined-behaviour sanitizers\n'
fi

rule
printf 'building differential_test\n'
rule

if ! cmake -S "$ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}" > "$CONFIGURE_LOG" 2>&1; then
  printf '\nCONFIGURE FAILED. Last 30 lines:\n\n'
  tail -n 30 "$CONFIGURE_LOG"
  exit 1
fi

# sqlite3 is found by CMake, not by this script. If it was missing, the configure
# step above printed a warning and the test itself fails loudly rather than
# skipping, so a missing oracle can never look like a pass.
if grep -q 'sqlite3 not found' "$CONFIGURE_LOG"; then
  printf '\nsqlite3 was not found by cmake. The differential suite cannot run.\n'
  printf 'Install libsqlite3 and its headers, then re-run this script.\n'
  exit 1
fi

if ! cmake --build "$BUILD_DIR" --target differential_test > "$BUILD_LOG" 2>&1; then
  printf '\nBUILD FAILED. Errors:\n\n'
  grep -E 'error:|Error' "$BUILD_LOG" | head -n 40
  printf '\n(full log: %s)\n' "$BUILD_LOG"
  exit 1
fi

rule
printf 'running differential_test\n'
rule
printf '\n'

set +e
"$BUILD_DIR/differential_test" ${QUARRY_FILTER:+"$QUARRY_FILTER"} 2>&1 | tee "$OUTPUT"
status="${PIPESTATUS[0]}"
set -e

printf '\n'
rule
printf 'summary\n'
rule

# The test binary prints a one-line agreement count and, when anything differs, a
# DIVERGENCES block. Both are surfaced here so the verdict does not require
# scrolling back through a long log.
agreement="$(grep -E 'queries agreed with sqlite' "$OUTPUT" | tail -n 1)"
if [[ -n "$agreement" ]]; then
  printf '%s\n' "$(printf '%s' "$agreement" | sed 's/^ *//')"
fi

failures="$(grep -cE '^  FAIL ' "$OUTPUT" || true)"
printf 'assertion failures: %s\n' "${failures:-0}"

if grep -q 'DIVERGENCES FROM SQLITE' "$OUTPUT"; then
  printf '\ndivergences were reported. Each one is a finding, not noise:\n\n'
  sed -n '/DIVERGENCES FROM SQLITE/,$p' "$OUTPUT"
fi

if grep -q 'TYPE NOTES' "$OUTPUT"; then
  printf '\ntype notes (values matched, declared result types did not):\n\n'
  sed -n '/TYPE NOTES/,/^$/p' "$OUTPUT"
fi

printf '\n'
if [[ "$status" -eq 0 ]]; then
  printf 'PASS — quarry agreed with sqlite on every query.\n'
else
  printf 'FAIL — see above. Full log: %s\n' "$OUTPUT"
fi
rule
exit "$status"
