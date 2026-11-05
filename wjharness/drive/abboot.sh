#!/bin/bash
# abboot.sh <outdir> <rounds> <seed> <arm>... -- interleaved boot A/B with bootcheck.mjs (Play -> first frame -> title).
# arm = "tag|--knob K=V --knob ..." (the part after | is passed to bootcheck verbatim; may be empty).
# Round r runs the arms in order when r is odd and reversed when even (ABBA...), so an order effect cancels (R194).
# Writes <outdir>/<tag>-<r>.out and one TSV row per boot to <outdir>/boots.tsv:
#   tag round firstFrame_s title_s verdict   (seconds from Play; from bootcheck's marker lines)
# Run it in a transient systemd service (CLAUDE.md, "Long-running work").
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
O=$(wj_outdir "$1") || exit 1; R=$2; SEED=$3; shift 3
mkdir -p "$O"
wj ensure-server >/dev/null 2>&1 || { echo "no dev server" >> "$O/RESULT"; exit 1; }
[ -f "$O/boots.tsv" ] || printf "tag\tround\tfirstFrame\ttitle\tverdict\theapMB\n" > "$O/boots.tsv"
arms=("$@")
for r in $(seq 1 "$R"); do
  order=("${arms[@]}")
  if [ $((r % 2)) = 0 ]; then order=(); for ((i=${#arms[@]}-1; i>=0; i--)); do order+=("${arms[$i]}"); done; fi
  for arm in "${order[@]}"; do
    tag=${arm%%|*}; extra=${arm#*|}; [ "$extra" = "$arm" ] && extra=""
    wj killdaemons >/dev/null 2>&1
    out="$O/$tag-$r.out"
    # shellcheck disable=SC2086
    MC_TASKSET=${MC_TASKSET:-$WJ_BOOT_CPUS} timeout -k 30 900 wj bootcheck --out "$O" --seed "$SEED" --timeout 600 --tag "ab-$tag-$r" --keep-going $extra > "$out" 2>&1
    ff=$(awk '$2=="firstFrame"{print $1}' "$out" | tr -d s); ti=$(awk '$2=="titleReady"{print $1}' "$out" | tr -d s)
    v=$(grep -o "PASS-FAULTED\|PASS\|FAIL\|TIMEOUT\|STALL" "$out" | tail -1)
    hm=$(grep -o "memory at title (MB): wasm [0-9]* heap [0-9]*" "$out" | awk '{print $NF}')
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$tag" "$r" "${ff:-NA}" "${ti:-NA}" "${v:-NA}" "${hm:-NA}" >> "$O/boots.tsv"
    rm -rf "$O/profile-ab-$tag-$r"
  done
done
echo finished >> "$O/RESULT"
