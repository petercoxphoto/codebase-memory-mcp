#!/usr/bin/env bash
# gen_mem_ceiling_repro.sh — synthetic large-file repro generator for the
# memory-ceiling tests (mem_ceiling_abort.sh).
#
# Emits N_SMALL tiny C files (near-zero footprint, exercise the discover/sort
# machinery without contributing meaningfully to RSS) and N_LARGE large C
# files (each an array-initializer source of ~SIZE_MB, mirroring the diagnosis
# repro shape: "600 tiny + N large array-init C files (~7.6 MB each)"). The
# array-init shape parses fast (tree-sitter C grammar, one big initializer
# list) while still landing SIZE_MB of source bytes + a comparable parse
# working set in RSS per worker — the mechanism the 2026-07-01 incident
# diagnosed as "concurrent large-file parsing summing unbounded".
#
# Usage:
#   gen_mem_ceiling_repro.sh <outdir> <n_small> <n_large> <size_mb>
#
# Same dir drives both the file-cap tests (dial size_mb near CBM_MAX_FILE_MB)
# and the ceiling test (dial n_large * size_mb past the ceiling).
set -euo pipefail

OUTDIR="${1:?usage: gen_mem_ceiling_repro.sh <outdir> <n_small> <n_large> <size_mb>}"
N_SMALL="${2:?missing n_small}"
N_LARGE="${3:?missing n_large}"
SIZE_MB="${4:?missing size_mb}"

mkdir -p "$OUTDIR"

# ── Tiny files (near-zero RSS contribution) ──────────────────────────
for i in $(seq 1 "$N_SMALL"); do
    cat > "$OUTDIR/small_${i}.c" <<EOF
int small_fn_${i}(int x) { return x + ${i}; }
EOF
done

# ── Large array-init files (the RSS-driving fixture) ─────────────────
# Each element line is ~11 bytes ("    12345,\n"); compute repeat count to
# hit SIZE_MB. awk generates the body in one pass (much faster than a bash
# loop at multi-MB scale).
BYTES_PER_ELEM=11
ELEM_COUNT=$(( (SIZE_MB * 1024 * 1024) / BYTES_PER_ELEM ))

for i in $(seq 1 "$N_LARGE"); do
    {
        echo "/* synthetic large source — ${SIZE_MB}MB array-init fixture */"
        echo "static const int large_data_${i}[] = {"
        awk -v n="$ELEM_COUNT" -v seed="$i" 'BEGIN {
            for (j = 0; j < n; j++) {
                printf "    %d,\n", (j * 7 + seed) % 100000
            }
        }'
        echo "};"
        echo "int large_fn_${i}(void) { return large_data_${i}[0]; }"
    } > "$OUTDIR/large_${i}.c"
done

echo "generated: $N_SMALL small + $N_LARGE large (~${SIZE_MB}MB each) under $OUTDIR" >&2
