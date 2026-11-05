#!/bin/bash
# j2d/prep.sh <rundir> -- the wasm ledger for one set-A run: ours (capture.py -> sampled set -> d8x86 -> wledger) or
# teavm (the shared $WJ_STATE/arms/cap-teavm capdir). Everything runs in a 4 GiB cgroup (ledger/refresh.sh's rule: two earlier
# ledger pipelines were taken by the kernel OOM killer on this 15 GB box).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
W=$WJ_HARNESS
D=$(realpath "$1")
# off the CPUs a measurement is pinned to (the E-cores on a hybrid part; the census CPUs otherwise)
cap () { systemd-run --user --wait --quiet --pipe $(wj_setenv_args) -p MemoryMax=4G -p MemorySwapMax=0 -p AllowedCPUs="${WJ_ECPUS:-$WJ_CENSUS_CPUS}" --working-directory="$W" -- nice -n 10 "$@"; }
arm=$(python3 -c "import json;print(json.load(open('$D/window.json'))['arm'])")
wj counts "$D" --json > /dev/null
if [ "$arm" = ours ]; then
  read tid t0 t1 < <(python3 -c "import json;c=json.load(open('$D/counts.json'));w=json.load(open('$D/window.json'))['window'];print(c['tid'],w[0],w[1])")
  cap python3 ledger/capture.py "$D/game.log" "$D/cap" | tail -1
  cap python3 ledger/sampled.py "$D" --tid "$tid" --window "$t0,$t1" > "$D/cap/sampled.txt"
  cap python3 ledger/d8x86.py "$D/cap" -j 3 --only "$D/cap/sampled.txt" | tail -1
  cap python3 j2d/wledger.py "$D" "$D/cap"
else
  # each TeaVM build is its own module (the 60-batch builds differ from the baseline): its own capdir, mods only --
  # V8's feedback-driven inlining keeps the live code from ever matching d8 (R415), so no d8 compile is needed
  C=$WJ_STATE/arms/cap-teavm; [ -d "$WJ_STATE/arms/cap-$arm" ] && C=$WJ_STATE/arms/cap-$arm
  cap python3 j2d/wledger.py "$D" "$C"
fi
