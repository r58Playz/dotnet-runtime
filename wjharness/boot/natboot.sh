#!/usr/bin/env bash
# natboot.sh -- native HotSpot BOOT reference (startup study, 2026-10-13). Derived from nativemc/natrun.sh, boot only:
# launch KnotClient on the matched Prism instance, wait for FAILED_AUTH (the browser harness's titleReady), dump
# HotSpot's perf counters (class loading / JIT time) with jcmd, then stop. No world, no menu replay.
#   [JAVA_HOME=<jdk>] OUT=<dir> EXTRA_JAVA="..." wj natboot     (JAVA_HOME defaults to $JAVA_HOME_8)
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
S="${S:-$WJ_STATE/native/stage}"
INST=$(need PRISM_INSTANCE); GAME="$INST/minecraft"
RD=$(grep -m1 '^renderDistance:' "$GAME/options.txt" 2>/dev/null | cut -d: -f2 | tr -d '\r')
[ "$RD" = 12 ] || { echo "VOID: renderDistance=$RD"; exit 1; }
JH="${JAVA_HOME:-$(need JAVA_HOME_8)}"; JAVA="$JH/bin/java"; JCMD="$JH/bin/jcmd"
XDISP="${XDISP:-:21}"; OUT=$(wj_outdir "${OUT:?}") || exit 1; mkdir -p "$OUT"; CP="$(cat "$S/cp.single")"; NAT="$S/natives"
setsid Xwayland "$XDISP" -geometry 1600x900 >"$OUT/xwayland.log" 2>&1 & XPG=$!
for i in $(seq 1 40); do DISPLAY="$XDISP" xdpyinfo >/dev/null 2>&1 && DISPLAY="$XDISP" xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.5; done
sleep 2
setsid env DISPLAY="$XDISP" xprop -root -spy >/dev/null 2>&1 & XPROP=$!
cd "$GAME"
T0=$(date +%s.%N)
setsid env -u WAYLAND_DISPLAY DISPLAY="$XDISP" taskset -c ${NAT_TASKSET:-$WJ_BOOT_CPUS} "$JAVA" ${EXTRA_JAVA:-} -Duser.language=en -Xms6144m -Xmx6144m \
    -Djava.library.path="$NAT" -Dorg.lwjgl.librarypath="$NAT" \
    -cp "$CP" net.fabricmc.loader.impl.launch.knot.KnotClient \
    --gameDir "$GAME" --assetsDir "$P/assets" --assetIndex 1.16 --version 1.16.1 \
    --username ikvmcraft --uuid 6378dd50-248d-3a1f-9061-91e0c45ce092 \
    --accessToken 0 --userType legacy --versionType release >"$OUT/game.log" 2>&1 &
PG=$!
stop(){ kill -TERM -- -"$PG" 2>/dev/null; sleep 5; kill -9 -- -"$PG" 2>/dev/null; kill -9 -- -"$XPROP" 2>/dev/null; kill -9 -- -"$XPG" 2>/dev/null; }
JPID=""
for i in $(seq 1 60); do
  for c in $(pgrep -x java 2>/dev/null); do tr '\0' ' ' < "/proc/$c/cmdline" 2>/dev/null | grep -q "KnotClient" && { JPID=$c; break; }; done
  [ -n "$JPID" ] && break; sleep 0.5
done
[ -z "$JPID" ] && { echo "VOID: java never started"; stop; exit 1; }
for i in $(seq 1 600); do grep -q FAILED_AUTH "$OUT/game.log" 2>/dev/null && break; kill -0 "$JPID" 2>/dev/null || break; sleep 0.2; done
T1=$(date +%s.%N)
grep -q FAILED_AUTH "$OUT/game.log" && echo "titleReady $(echo "$T1 - $T0" | bc) s" | tee "$OUT/result.txt" || echo "VOID: no FAILED_AUTH" | tee "$OUT/result.txt"
"$JCMD" "$JPID" PerfCounter.print > "$OUT/perfcounters.txt" 2>&1
case "${EXTRA_JAVA:-}" in *LogTouchedMethods*) "$JCMD" "$JPID" VM.print_touched_methods > "$OUT/touched.txt" 2>&1;; esac
cat /proc/$JPID/status | grep -E 'VmHWM|VmRSS|Threads' >> "$OUT/result.txt"
awk '{print $14+$15}' /proc/$JPID/stat | xargs -I{} echo "cpu_ticks {}" >> "$OUT/result.txt"
stop
