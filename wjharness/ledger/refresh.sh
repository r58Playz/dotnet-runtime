#!/bin/bash
# Gap-ledger refresh: one mapping run in -> ours ledger, (optionally) HotSpot ledger, and the joined gap report out.
#   wj refresh <mcperf rundir> <outdir> [<natrun OUT dir with PrintAssembly + PERF_RECORD> <ticks>]
# The mapping run pair is perf2x/runG2.sh (ours + native; runtime built with the origin diagnostics, knobs off = normal
# codegen): instructions twice (fp chain / LBR call stack), cycles, mem_load_retired.{l1,l2,l3}_miss -- all PEBS, all
# with per-event periods kept under the box's 3000/s kernel.perf_event_max_sample_rate -- plus
#   --knob MONO_WASM_JIT_ORIGIN=1 --knob MONO_WASM_JIT_CAPTURE=1, --jitdump, plateau window, MC_TASKSET=0-7.
# Hold `systemd-inhibit --what=idle:sleep` around it: a suspend inside the window voids the run (g2's first attempt).
# Everything runs inside a 4 GiB cgroup: two earlier versions of this pipeline were taken by the KERNEL OOM killer
# on the 15 GB box (the per-unit cap is what keeps a mistake from reaching the desktop).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
W=$WJ_HARNESS
RUN=$1; OUT=$2; HS=${3:-}; HSTICKS=${4:-}
mkdir -p "$OUT"
step () { echo "$(date +%T) $*"; }
cap () { systemd-run --user --wait --quiet --pipe $(wj_setenv_args) -p MemoryMax=4G -p MemorySwapMax=0 --working-directory="$W" -- "$@"; }
step "server tid";       tid=$(cap python3 ledger/servertid2.py "$RUN" | tail -1); echo "$tid" > "$OUT/servertid"
step "captures";         cap python3 ledger/capture.py "$RUN/game.log" "$OUT/cap"
step "sampled set";      cap python3 ledger/sampled.py "$RUN" --tid "$tid" > "$OUT/sampled.txt"
step "d8 compile";       cap python3 ledger/d8x86.py "$OUT/cap" -j 3 --only "$OUT/sampled.txt" | tail -1
# --align: recover functions the browser's feedback inlining re-laid out (ledger.py's header; 2026-11-07, 26.2's
# getBlockState). Only functions with no exact match are aligned; 'aligned' is counted apart in record matching.
step "ours ledger";      cap python3 ledger/ledger.py "$RUN" "$OUT/cap" --tid "$tid" --out "$OUT/ours" --align > "$OUT/ours.txt"
head -12 "$OUT/ours.txt"
if [ -n "$HS" ]; then
  step "HotSpot ledger"; cap python3 ledger/hsledger.py "$HS" --ticks "$HSTICKS" --out "$OUT/hs" > "$OUT/hs.txt"
  head -4 "$OUT/hs.txt"
  step "gap";            python3 ledger/gap.py "$OUT/ours" "$OUT/hs" --top 60 --out "$OUT/GAP.md" --tsv "$OUT/gap" | head -26
fi
