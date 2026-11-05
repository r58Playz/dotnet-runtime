#!/bin/bash
# hwrun.sh <batchdir> <tag> [mcperf args...] -- ONE hardware-census arm (R312's hwcensus event set), then the
# server + client census. Waits for perf to exit before analysing (R313: reading perf.data while perf is still
# flushing reads "no samples in the window"). Retries a preflight refusal twice, forcing on the third attempt.
# Pass --jitdump among the mcperf args for memclass.py / callinfra.py (much larger capture).
#
# hwrun.sh <batchdir> --drain -- run every census still queued for <batchdir> (see QUEUED CENSUS). Call it once after
# a batch's LAST arm, before reading any <tag>.census.txt.
#
# PROTOCOL Q60 (R418): --warm-ms 160000 --bench-ms 60000 --no-save-quit, i.e. the window lands ~198-258 s after world
# join (the harness's settle takes ~38 s before the hold starts). Measured on R417's eight tick censuses: the server
# rate settles ~100 s after join but an A/B needs ~200 s (a 160-220 s window lost the GC probe's +4.6%); a 60 s window
# at 200-260 s reproduced it in instructions AND cycles with the same base spread as the old 120 s window; and the
# 6,000-tick autosave (~300 s after join, +17 M instr/tick for 10 s) sat INSIDE the old 220-340 s window. Override with
# HW_WARM_MS / HW_BENCH_MS (the old protocol: HW_WARM_MS=180000 HW_BENCH_MS=120000).
#
# QUEUED CENSUS (R418). The ~45 s census used to run between arms. It is now queued, and the NEXT arm runs it on the
# census CPUs ($WJ_CENSUS_CPUS, the last E-cores on a hybrid part; taskset, nice 19) once that arm's mcperf has passed preflight -- i.e. during its boot, more than five
# minutes before its window. It cannot run any earlier: preflight refuses any non-browser process above half a core,
# so a census still running when the next arm starts would cost a 3-minute retry. HW_CENSUS_SYNC=1 restores the old
# synchronous census.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
B=$(wj_outdir "$1") || exit 1; tag=$2; shift 2
mkdir -p "$B"
Q="$B/.hwcensus-queue"
note () { echo "$(date +%T) $*" >> "$B/RESULT"; }
census () {   # census <tag> [taskset cpus]
  local t=$1 pin=${2:-}
  local run=(nice -n 19); [ -n "$pin" ] && run=(taskset -c "$pin" nice -n 19)
  "${run[@]}" wj hwcensus "$B/$t" --top 60 > "$B/$t.census.txt" 2>&1
  "${run[@]}" wj hwcensus "$B/$t" --client --top 30 > "$B/$t.client.txt" 2>&1
  note "$t census: $(grep -E '^(instructions|near_call:p1|all_stores|cycles) ' "$B/$t.census.txt" | awk '{printf "%s=%s ", $1, $2}')"
}
drain () {   # drain [taskset cpus]: every queued census, in order; the queue is emptied first so a concurrent arm cannot repeat one
  [ -s "$Q" ] || return 0
  local list; list=$(cat "$Q"); : > "$Q"
  for t in $list; do census "$t" "${1:-}"; done
}
if [ "$tag" = "--drain" ]; then drain; exit 0; fi
wait_perf () { for _ in $(seq 1 120); do pgrep -x perf >/dev/null || { sleep 5; return 0; }; sleep 5; done; }
[ -n "${MCPERF_EVENT:-}" ] || pmu_require   # the default set below is raw Golden Cove / Gracemont codes; sets C and A
export MCPERF_EVENT="${MCPERF_EVENT:-instructions/period=2000003/u,cycles/period=2000003/u,$C/event=0xc4,umask=0x02,period=200003/up,$A/event=0xc4,umask=0xf9,period=200003/up,$C/event=0xc4,umask=0x02,period=400009/upp,$A/event=0xc4,umask=0xf9,period=400009/upp,$C/event=0xc4,umask=0x80,period=400009/u,$A/event=0xc4,umask=0xeb,period=400009/u,$C/event=0xd0,umask=0x81,period=2000003/u,$C/event=0xd0,umask=0x82,period=1000003/u,$C/event=0xc4,umask=0x11,period=2000003/u,$A/event=0xc4,umask=0x7e,period=2000003/u}"
unset MCPERF_PERIOD
WARM="${HW_WARM_MS:-160000}"; BENCH="${HW_BENCH_MS:-60000}"
wj ensure-server >/dev/null 2>&1 || { note "$tag: no dev server"; exit 1; }
wj killdaemons >> "$B/daemons.log" 2>&1
F=""
# HW_NOFORCE=1: never force. Keep retrying a preflight refusal (every 3 min, up to HW_TRIES, default 80 = 4 h) instead
# of forcing on the third attempt -- a forced arm on a desktop in use is ambient load in the measurement.
TRIES="${HW_TRIES:-80}"; [ -n "${HW_NOFORCE:-}" ] || TRIES=3
for attempt in $(seq 1 $TRIES); do
  [ -z "${HW_NOFORCE:-}" ] && [ $attempt = 3 ] && F="--force"
  wait_perf
  rm -rf "$B/$tag" "$B/$tag.out"
  timeout -k 30 3600 wj mcperf "$B/$tag" --warm-ms "$WARM" --bench-ms "$BENCH" --no-walk --no-save-quit $F "$@" \
    > "$B/$tag.out" 2>&1 &
  mp=$!
  # Past preflight (mcperf is recording) -> run the queued census on the E-cores while this arm boots.
  if [ -s "$Q" ] && [ -z "${HW_CENSUS_SYNC:-}" ]; then
    ( for _ in $(seq 1 300); do
        grep -qa "recording pid" "$B/$tag.out" 2>/dev/null && { drain "$WJ_CENSUS_CPUS"; exit 0; }
        grep -qa "refusing to measure\|verdict:" "$B/$tag.out" 2>/dev/null && exit 0
        kill -0 $mp 2>/dev/null || exit 0
        sleep 2
      done ) &
    dp=$!
  else
    dp=""
  fi
  wait $mp
  [ -n "$dp" ] && wait $dp
  if grep -qa "refusing to measure" "$B/$tag.out"; then note "$tag: preflight refused (attempt $attempt)"; sleep 180; continue; fi
  if grep -qa "verdict: never-started" "$B/$tag.out"; then NS=$(( ${NS:-0} + 1 )); note "$tag: never-started (attempt $attempt)"; [ $NS -ge 3 ] && break; continue; fi
  break
done
wait_perf
V=$(grep -a -o 'verdict: [a-z-]*' "$B/$tag.out" | tail -1)
note "$tag | $V | forced=${F:-no} | warm ${WARM} bench ${BENCH} | $(grep -a -o '!! fault [a-z-]* \[[^]]*\]' "$B/$tag.out" | head -1)"
case "$V" in *": ok"*) rm -rf "$B/$tag/browser-profile" ;; esac
if [ -n "${HW_CENSUS_SYNC:-}" ]; then
  census "$tag"
else
  echo "$tag" >> "$Q"
  note "$tag census queued (runs during the next arm's boot; after the last arm: wj hwrun $B --drain)"
fi
