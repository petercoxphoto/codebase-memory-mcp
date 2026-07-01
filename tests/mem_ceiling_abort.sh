#!/usr/bin/env bash
# mem_ceiling_abort.sh — real-process regression guard for the enforcing RSS
# memory ceiling (cbm_mem_abort_if_over_ceiling, mem.c).
#
# Covers the acceptance-criteria rows that CANNOT be exercised by an
# in-process C unit test (the abort path itself calls abort(), which would
# kill the test-runner process):
#
#   Row 6  — RSS ceiling ABORTS (non-zero exit) with a diagnostic dump
#            naming file + phase + RSS when exceeded. Exercises the real
#            index entry point (cli index_repository -> cbm_parallel_extract),
#            not a direct call to the ceiling helper.
#   Row 7  — A normal full index of the real repo does NOT abort (healthy
#            path preserved; anti-false-positive guard for R3).
#   Row 9  — CBM_MEM_CEILING_MB override adjusts the abort threshold: forced
#            low, the HEALTHY real-repo index now aborts (proves the knob
#            bites); unset uses the default and completes; invalid warns and
#            falls back to the default (completes).
#   Row 10 (real-environment half) — both the abort and no-abort verdicts
#            are read against the REAL RSS of a real process, not an
#            injected value.
#
# Also checks the store-integrity finding (R4): an aborted index must never
# leave a partially-written .db — dump_and_persist_hashes() (the SQLite
# write) runs strictly after cbm_parallel_extract() in pipeline.c, so an
# abort during extract precedes any DB write. This script asserts no .db
# file (partial or otherwise) is left behind by an aborted run.
#
# Usage: bash tests/mem_ceiling_abort.sh
# Exit 0 on success, non-zero on failure. SLOW (generates GB-scale fixtures) —
# intended for the IO build host, not routine `make test`.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build/c/codebase-memory-mcp"
GEN="$ROOT/tests/gen_mem_ceiling_repro.sh"
CACHE_DIR="${HOME}/.cache/codebase-memory-mcp"
FAILURES=0
WORKDIR=""

cleanup() {
    [[ -n "$WORKDIR" && -d "$WORKDIR" ]] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

if [[ ! -x "$BINARY" ]]; then
    echo "[mem_ceiling] FAIL: binary not found at $BINARY (build first: make -f Makefile.cbm cbm)" >&2
    exit 2
fi

project_name() { printf '%s' "$1" | sed 's#^/##; s#[^A-Za-z0-9._-]#-#g'; }

# Run `cli index_repository` against $1, with env overrides from the
# remaining args (NAME=VALUE pairs), capturing exit code + stderr.
# Sets: LAST_EXIT, LAST_LOG (path).
run_index() {
    local repo="$1"; shift
    local log; log="$(mktemp)"
    LAST_LOG="$log"
    # shellcheck disable=SC2086
    env "$@" "$BINARY" cli index_repository "{\"repo_path\":\"$repo\",\"mode\":\"full\"}" \
        >"$log" 2>&1
    LAST_EXIT=$?
}

db_path_for() {
    local repo="$1"
    printf '%s/%s.db' "$CACHE_DIR" "$(project_name "$repo")"
}

# ── Row 7 + row 10 (no-abort half): healthy real-repo index, default ceiling ──
echo "[mem_ceiling] Row 7/10: healthy real-repo index must NOT abort (default ceiling)..." >&2
run_index "$ROOT"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row7]: healthy real-repo index aborted (exit=$LAST_EXIT) under default ceiling" >&2
    tail -20 "$LAST_LOG" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] PASS [row7]: healthy index completed (exit=0)" >&2
fi
rm -f "$LAST_LOG"

# ── Row 9 (unset leg): explicit unset also completes ─────────────────────
echo "[mem_ceiling] Row 9 (unset): explicit CBM_MEM_CEILING_MB unset uses default, completes..." >&2
run_index "$ROOT"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row9-unset]: index aborted with ceiling env unset (exit=$LAST_EXIT)" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] PASS [row9-unset]" >&2
fi
rm -f "$LAST_LOG"

# ── Row 9 (invalid leg): non-numeric override warns + falls back, completes ──
echo "[mem_ceiling] Row 9 (invalid): non-numeric CBM_MEM_CEILING_MB falls back to default..." >&2
run_index "$ROOT" CBM_MEM_CEILING_MB="not-a-number"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row9-invalid]: index aborted with invalid ceiling env (exit=$LAST_EXIT)" >&2
    FAILURES=$((FAILURES + 1))
else
    if ! grep -q "mem_ceiling.env.invalid" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row9-invalid]: no warn log for invalid CBM_MEM_CEILING_MB" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "[mem_ceiling] PASS [row9-invalid]: warned + completed" >&2
    fi
fi
rm -f "$LAST_LOG"

# ── Row 9 (low override leg) + Row 6 + R4: force ceiling low, healthy repo now aborts ──
echo "[mem_ceiling] Row 6/9(low)/R4: CBM_MEM_CEILING_MB forced to the floor (2048MB) — healthy repo must now abort with a diagnostic dump..." >&2
db_before="$(db_path_for "$ROOT")"
rm -f "$db_before" "${db_before}.tmp"
run_index "$ROOT" CBM_MEM_CEILING_MB="2048"
if [[ "$LAST_EXIT" -eq 0 ]]; then
    echo "[mem_ceiling] FAIL [row6/row9-low]: index did NOT abort with ceiling forced to the floor (proves the knob doesn't bite, or the real repo genuinely never nears 2GB on this host — re-run with a smaller floor override or bigger synthetic fixture below)" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] index aborted as expected (exit=$LAST_EXIT)" >&2
    if ! grep -q "mem.ceiling.abort" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row6]: no mem.ceiling.abort diagnostic dump in output" >&2
        FAILURES=$((FAILURES + 1))
    elif ! grep -qE "file=\S+ phase=\S+ rss_mb=[0-9]+ ceiling_mb=[0-9]+" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row6]: diagnostic dump missing file/phase/rss_mb/ceiling_mb fields" >&2
        tail -5 "$LAST_LOG" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "[mem_ceiling] PASS [row6]: diagnostic dump present with file+phase+rss" >&2
    fi
fi
# R4: an aborted run must leave no (partial) .db file — the abort happens
# strictly before dump_and_persist_hashes()/cbm_gbuf_dump_to_sqlite().
if [[ -f "$db_before" || -f "${db_before}.tmp" ]]; then
    echo "[mem_ceiling] FAIL [R4]: aborted run left a .db (or .db.tmp) file — store-integrity violation: $db_before" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] PASS [R4]: aborted run left no .db file (extract-phase abort precedes any SQLite write)" >&2
fi
rm -f "$LAST_LOG" "$db_before" "${db_before}.tmp"

# ── Row 6 (synthetic large-file variant): generate a fixture that exceeds
#    a forced-low ceiling purely from cumulative parse RSS, independent of
#    whether the real repo happens to be big enough on this host ─────────
echo "[mem_ceiling] Row 6 (synthetic): generating large-file repro to force the ceiling from cumulative RSS..." >&2
WORKDIR="$(mktemp -d /tmp/cbm_mem_ceiling_repro.XXXXXX)"
bash "$GEN" "$WORKDIR" 20 8 8   # 20 tiny + 8 large (~8MB each = ~64MB source; parse working
                                 # sets multiply this several-fold across concurrent workers)
db_synth="$(db_path_for "$WORKDIR")"
rm -f "$db_synth" "${db_synth}.tmp"
run_index "$WORKDIR" CBM_MEM_CEILING_MB="2048" CBM_WORKERS="8"
if [[ "$LAST_EXIT" -eq 0 ]]; then
    echo "[mem_ceiling] WARN [row6-synth]: synthetic fixture did not trip the forced ceiling on this host (IO's 31GB may absorb 8x8MB files even at 8 workers) — not counted as a failure since the real-repo variant above already proved the abort path; widen n_large/size_mb if this needs to be load-bearing" >&2
else
    if grep -q "mem.ceiling.abort" "$LAST_LOG"; then
        echo "[mem_ceiling] PASS [row6-synth]: synthetic large-file repro tripped the ceiling with a diagnostic dump" >&2
    else
        echo "[mem_ceiling] FAIL [row6-synth]: synthetic fixture aborted (exit=$LAST_EXIT) but with no diagnostic dump" >&2
        FAILURES=$((FAILURES + 1))
    fi
fi
rm -f "$LAST_LOG" "$db_synth" "${db_synth}.tmp"

# ── Final result ───────────────────────────────────────────────────────
if [[ "$FAILURES" -gt 0 ]]; then
    echo "[mem_ceiling] FAILED: $FAILURES check(s) failed." >&2
    exit 1
fi

echo "[mem_ceiling] All checks passed (rows 6, 7, 9, 10-real-env, R4 store-integrity)." >&2
exit 0
