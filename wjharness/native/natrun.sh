#!/usr/bin/env bash
# Native OpenJDK Minecraft harness -- the counterpart of the browser harness's in-game window.
#
# WHY THIS EXISTS: every native number in this project is fps-derived, and native is GPU-bound, so each
# one is a lower bound compressed by a ceiling that has nothing to do with the JVM. The project's own
# metric is M instructions/frame on the client render thread (ours: 165.2). Native has no counterpart.
# perf stat -e instructions is indifferent to what limits the frame rate, so it answers directly.
#
# Differences from jvminline.sh, all deliberate:
#   * drives java directly (Prism's org.prismlauncher.EntryPoint reads params from stdin; KnotClient
#     takes them as argv), so the player identity is ours to set -- see --uuid below
#   * --uuid is the offline UUID of "ikvmcraft", which is the name of the save's playerdata file.
#     jvminline.sh used Prism's `bench` account (27b307e5-...), so it spawned a FRESH entity at world
#     spawn instead of restoring the saved position: a different workload, silently.
#   * replays the browser harness's MENU_STEPS with xdotool instead of --world auto-join, which MCSR
#     Ranked intercepts. The first Tab,Enter dismisses Ranked's auth dialog (Escape does NOT).
#   * reports instructions/frame, not fps.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
# STAGING lives in $WJ_STATE/native/stage (it was beside this script), NOT in /tmp: systemd-tmpfiles' stock `q /tmp 10d` deleted
# /tmp/nativemc on 2026-09-24, cp.single read back empty, and java died with "Could not find or load
# main class KnotClient" -- which then tore Xwayland down and read like a display failure (R308).
# cp.single = Prism's logged -cp minus NewLaunch.jar; natives = the lwjgl 3.2.2 *-natives-linux.jar .so's.
S="${S:-$WJ_STATE/native/stage}"
[ -s "$S/cp.single" ] && [ -e "$S/natives/liblwjgl.so" ] || { echo "[nat] VOID: staging missing under $S (cp.single / natives) -- see the comment above"; exit 1; }
INST=$(need PRISM_INSTANCE "the matched Prism instance, MCSRRanked-Linux-1.16.1-Basic")
GAME="$INST/minecraft"
# 2026-10-05: a manual session left renderDistance:32 in the instance's options.txt and the next JDK arm ran at 32 chunks
# (the integrated server's view distance follows it). Our side is 12; refuse anything else.
RD=$(grep -m1 '^renderDistance:' "$GAME/options.txt" 2>/dev/null | cut -d: -f2 | tr -d '\r')
[ "$RD" = "${NAT_RENDER_DISTANCE:-12}" ] || { echo "[nat] VOID: renderDistance=${RD:-?} in $GAME/options.txt, expected ${NAT_RENDER_DISTANCE:-12}"; exit 1; }
NAT="$S/natives"
JAVA="$(need JAVA_HOME_8 "the native reference is JDK 8")/bin/java"
XDISP="${XDISP:-:20}"; XGEOM="${XGEOM:-1600x900}"
WARM="${WARM:-180}"; BENCH="${BENCH:-120}"; JOINWAIT="${JOINWAIT:-300}"
OUT="${OUT:-$(wj_rundir natrun)}"; OUT=$(wj_outdir "$OUT") || exit 1; mkdir -p "$OUT"
STATE="$OUT/game.log"   # see inworld gate below
CP="$(cat "$S/cp.single")"

echo "[nat] out=$OUT warm=${WARM}s bench=${BENCH}s"

# PRISTINE WORLD EVERY RUN. A native run plays the Prism save in place and saves it back, so each run advanced the
# world (time of day was not locked: later runs measured NIGHT -- different mobs, different tick). Our browser side
# restores a fixed OPFS seed per run; this is the native equivalent. The previous run's working copy is kept as
# stage/world-trash/last (one copy), the source of truth is the ikvmcraft mcsrranked tar.
if [ "${NW_RESTORE:-1}" = 1 ]; then
  NW_TAR=$(need NW_TAR "the pristine world is restored from it every run (NW_RESTORE=0 skips)")
  [ -s "$NW_TAR" ] || { echo "[nat] VOID: no pristine world tar at $NW_TAR"; exit 1; }
  mkdir -p "$S/world-trash"; rm -rf "$S/world-trash/last"
  [ -d "$GAME/saves/New World" ] && mv "$GAME/saves/New World" "$S/world-trash/last"
  tar -xf "$NW_TAR" -C "$INST" "minecraft/saves/New World" || { echo "[nat] VOID: world restore failed"; exit 1; }
  echo "[nat] restored pristine New World from $(basename "$NW_TAR") ($(du -sh "$GAME/saves/New World" | cut -f1))"
fi

# A STALE wpstateout makes the in-world gate pass falsely (jvminline.sh's lesson).
: # nothing stale to clear: the gate reads this run's own fresh log

setsid Xwayland "$XDISP" -geometry "$XGEOM" >"$OUT/xwayland.log" 2>&1 &
XPG=$!

# READINESS, not a fixed sleep. GLFW init failed with GLFW_PLATFORM_ERROR [0x10008] on one run and
# succeeded on another with identical config: Xwayland compiles its XKB keymap asynchronously (see the
# xkbcomp warnings in xwayland.log) and glxinfo can answer before that finishes. GL being up is NOT
# evidence the display is usable -- gate on xdpyinfo AND a keymap query.
ready=0
for i in $(seq 1 40); do
  if DISPLAY="$XDISP" xdpyinfo >/dev/null 2>&1 && DISPLAY="$XDISP" xdotool getdisplaygeometry >/dev/null 2>&1; then
    ready=1; break
  fi
  sleep 0.5
done
[ "$ready" = 1 ] || { echo "[nat] VOID: $XDISP never became usable"; kill -9 -- -"$XPG" 2>/dev/null; exit 1; }
sleep 2   # margin past keymap compile
if ! DISPLAY="$XDISP" glxinfo -B 2>/dev/null | grep -q "OpenGL renderer"; then
  echo "[nat] VOID: no GL on $XDISP"; kill -9 -- -"$XPG" 2>/dev/null; exit 1
fi

# HOLD ONE X CLIENT OPEN for the whole run. A rootful Xwayland maps and unmaps its host window as
# clients come and go, so polling with a fresh xdotool every 2s makes the window flicker visibly on the
# desktop. One long-lived connection keeps it stable.
setsid env DISPLAY="$XDISP" xprop -root -spy >/dev/null 2>&1 &
XPROP=$!
DISPLAY="$XDISP" glxinfo -B 2>/dev/null | grep -E "OpenGL renderer|OpenGL core profile version" | sed 's/^/[nat] /'

# Run from the game dir: Lithium/Sodium write config/*.properties into the CWD, which put a stray config/ in the repo
# root when this was launched from there (2026-09-30; the files were the instance's defaults, so no workload change).
cd "$GAME"
# LWJGL 3.2.2 is X11-only: unset WAYLAND_DISPLAY so GLFW cannot pick the host Wayland socket.
setsid env -u WAYLAND_DISPLAY DISPLAY="$XDISP" MANGOHUD=1 \
  MANGOHUD_CONFIG="output_folder=$OUT,autostart_log=1,log_duration=7200,log_interval=0" \
  taskset -c ${NAT_TASKSET:-$WJ_PCPUS} mangohud "$JAVA" ${EXTRA_JAVA:-} -Duser.language=en -Xms6144m -Xmx6144m \
    -Djava.library.path="$NAT" -Dorg.lwjgl.librarypath="$NAT" \
    -cp "$CP" net.fabricmc.loader.impl.launch.knot.KnotClient \
    --gameDir "$GAME" --assetsDir "$P/assets" --assetIndex 1.16 --version 1.16.1 \
    --username ikvmcraft --uuid 6378dd50-248d-3a1f-9061-91e0c45ce092 \
    --accessToken 0 --userType legacy --versionType release \
  >"$OUT/game.log" 2>&1 &
PG=$!
stop(){ kill -TERM -- -"$PG" 2>/dev/null; sleep 8; kill -9 -- -"$PG" 2>/dev/null
        kill -9 -- -"$XPROP" 2>/dev/null; kill -9 -- -"$XPG" 2>/dev/null; }

# The java PID is a grandchild of setsid; find it by its own command line, and keep only real java.
JPID=""
for i in $(seq 1 60); do
  for c in $(pgrep -x java 2>/dev/null); do
    if tr '\0' ' ' < "/proc/$c/cmdline" 2>/dev/null | grep -q "KnotClient"; then JPID=$c; break; fi
  done
  [ -n "$JPID" ] && break; sleep 1
done
[ -z "$JPID" ] && { echo "[nat] VOID: java never started"; stop; exit 1; }
echo "[nat] java pid=$JPID"

# Wait for the game window, then replay the browser harness's MENU_STEPS.
WID=""
for i in $(seq 1 60); do
  WID=$(DISPLAY="$XDISP" xdotool search --name "Minecraft" 2>/dev/null | head -1)
  [ -n "$WID" ] && break
  if ! kill -0 "$JPID" 2>/dev/null; then
    echo "[nat] VOID: java exited before a window appeared"
    grep -m1 -A6 "Failed to initialize GLFW\|Crash Report" "$OUT/game.log" 2>/dev/null | sed 's/^/[nat]   /'
    stop; exit 2
  fi
  sleep 2
done
[ -z "$WID" ] && { echo "[nat] VOID: no Minecraft window"; stop; exit 1; }
# Gate on the SAME marker the browser harness calls titleReady: MCSR Ranked's auth failing is what
# means the modded main screen has finished building. A fixed sleep races it.
echo "[nat] window=$WID -- waiting for FAILED_AUTH (modded main screen)"
for i in $(seq 1 120); do grep -q FAILED_AUTH "$OUT/game.log" 2>/dev/null && break; sleep 2; done
grep -q FAILED_AUTH "$OUT/game.log" 2>/dev/null && echo "[nat] main screen ready" || echo "[nat] WARNING: no FAILED_AUTH, replaying anyway"
sleep 3
# NO WINDOW MANAGER on a bare Xwayland, so windowactivate (_NET_ACTIVE_WINDOW) is refused and every
# key press is dropped. Set input focus directly with XSetInputFocus, then send REAL XTEST events --
# `key --window` would use XSendEvent, which GLFW ignores like most toolkits.
DISPLAY="$XDISP" xdotool windowfocus --sync "$WID" 2>/dev/null || \
  DISPLAY="$XDISP" xdotool windowfocus "$WID" 2>/dev/null || echo "[nat] WARNING: windowfocus failed"
SHOT=0
shot(){ SHOT=$((SHOT+1)); DISPLAY="$XDISP" import -window root "$OUT/menu$(printf %02d $SHOT)-$1.png" 2>/dev/null; }
k(){ DISPLAY="$XDISP" xdotool key --clearmodifiers "$1"; sleep "$2"; shot "$1"; }
shot start
# Tab,Enter  dismiss MCSR Ranked "Player authentication data couldn't be loaded" (Escape does not work)
# Tab,Enter  Singleplayer -> Select World
# Tab,Down   focus the world list, select "New World"
# Tab,Enter  "Play Selected World" (Enter on the LIST does nothing in 1.16.1)
# WHICH ENTRY IS "New World": the list is sorted by level.dat LastPlayed, newest first, and a second save
# ("New World.drift-20260925") exists -- so whichever world was played last is on top, and a fixed Down alternates
# worlds run to run (g1/hs2 measured the drift world). Tab+Down lands on the SECOND entry; Down,Up on the first.
NWPOS=$(python3 "$(dirname "$0")/worldpos.py" "$GAME/saves" "New World" 2>"$OUT/worldpos.txt")
echo "[nat] New World is list entry $NWPOS ($(cat "$OUT/worldpos.txt"))"
case "$NWPOS" in 0|1) ;; *) echo "[nat] VOID: New World at unexpected list position '$NWPOS'"; stop; exit 1;; esac
NW_LD0=$(stat -c %Y "$GAME/saves/New World/level.dat")
k Tab 0.7; k Return 2.5
k Tab 0.7; k Return 3.0
k Tab 0.7; k Down   1.0
[ "$NWPOS" = 0 ] && k Up 1.0
k Tab 0.7; k Return 2.0

echo "[nat] waiting for inworld (up to ${JOINWAIT}s)"
w=0; while [ $w -lt "$JOINWAIT" ]; do grep -q "StateOutput State: inworld" "$STATE" 2>/dev/null && break; sleep 2; w=$((w+2)); done
if ! grep -q "StateOutput State: inworld" "$STATE" 2>/dev/null; then
  echo "[nat] VOID: never reached in-world in ${JOINWAIT}s"; cp "$STATE" "$OUT/" 2>/dev/null; stop; exit 1
fi
echo "[nat] in-world after ${w}s -> warm ${WARM}s"
sleep "$WARM"

# Attach to the TWO threads that matter by NAME, not --per-thread over the whole process: with ~90
# threads and two PMUs perf multiplexes and reports <not counted> / 19.93%-enabled garbage. Minecraft
# reuses the name "Render thread" for several threads, so take the one with the most CPU time.
pick(){ best=""; bestj=0
  for t in /proc/"$JPID"/task/*; do
    c=$(cat "$t/comm" 2>/dev/null) || continue
    [ "$c" = "$1" ] || continue
    j=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null) || continue
    if [ "${j:-0}" -ge "$bestj" ]; then bestj=$j; best=$(basename "$t"); fi
  done; echo "$best"; }
RTID=$(pick "Render thread"); STID=$(pick "Server thread")
echo "[nat] Render thread tid=$RTID   Server thread tid=$STID"
for t in /proc/"$JPID"/task/*; do echo "$(basename "$t") $(cat "$t/comm" 2>/dev/null)"; done > "$OUT/threads.txt"
[ -z "$RTID" ] && { echo "[nat] VOID: no Render thread"; stop; exit 1; }

echo "[nat] BENCH ${BENCH}s -- perf stat $WJ_PMU_CORE/instructions on tids $RTID,$STID"
date +%s > "$OUT/t1"

# PER-METHOD CENSUS. -e instructions makes samples instruction-weighted, which is the unit the
# comparison is in (ours: 165.2 M instr/frame). -t tags each stack with its thread so the Render thread
# can be isolated the way R184 isolates ours. Falls back to cpu (itimer) if the PMU is refused.
AP="${AP:-$WJ_STATE/native/async-profiler-2.9-linux-x64/profiler.sh}"
[ "${PROFILE:-1}" = 1 ] && [ ! -x "$AP" ] && echo "[nat] NOTE: no async-profiler at $AP -- no per-method census this run"
if [ "${PROFILE:-1}" = 1 ] && [ -x "$AP" ]; then
  ( "$AP" -d "$BENCH" -e instructions -t -o collapsed -f "$OUT/prof.collapsed" "$JPID" \
      > "$OUT/ap.log" 2>&1 || \
    "$AP" -d "$BENCH" -e cpu -t -o collapsed -f "$OUT/prof.collapsed" "$JPID" \
      >> "$OUT/ap.log" 2>&1 ) &
  APPID=$!
fi

# PERF_RECORD=1 (the gap ledger, R-ledger): a same-window sampling capture of the SERVER thread with the events and
# LBR mode our side uses (ledger/ledger.py), for hsledger.py to map onto -XX:+PrintAssembly blobs. Period and events
# overridable; -k 1 so times line up with the window.
if [ "${PERF_RECORD:-0}" = 1 ]; then
  # THROTTLING: kernel.perf_event_max_sample_rate had auto-lowered to 3000/s, and the native server thread runs in
  # bursts at ~1.9 G instr/s, so PEBS+LBR at period 200,003 / 1,000,003 throttled 18-23 k times (samples = 31% / 86%
  # of perf stat's count). Defaults now: no LBR, per-event periods in the event terms (~1.9 k samples/s in a burst).
  perf record -q -k 1 ${PERF_RECORD_LBR:+-b} \
    -e "${PERF_RECORD_EVENTS:-$WJ_PMU_CORE/instructions,period=2000003/upp,$WJ_PMU_CORE/cycles,period=4000037/upp}" \
    -t "$STID" -o "$OUT/perf.data" -- sleep "$BENCH" > "$OUT/perfrec.log" 2>&1 &
  PRPID=$!
fi

# PERF_EVENTS overrides the counted set (hwcensus: calls/loads/stores/branches per tick, same events as
# hwcensus.py reads off our side). natstat.py only reads instructions + task-clock, so keep both in it.
perf stat -e "${PERF_EVENTS:-$WJ_PMU_CORE/instructions/u,task-clock}" --per-thread -t "$RTID,$STID" -- sleep "$BENCH" 2>"$OUT/perf.txt"
date +%s > "$OUT/t2"
[ -n "${APPID:-}" ] && wait "$APPID" 2>/dev/null
[ -n "${PRPID:-}" ] && wait "$PRPID" 2>/dev/null
# the LAST state line, not the last 400 lines: a PrintAssembly arm buries the state lines under assembly
grep -a "StateOutput State:" "$STATE" | tail -1 | grep -q "inworld" && echo "[nat] still in-world at window close" || echo "[nat] WARNING: left the world"
echo "$BENCH" > "$OUT/bench_s"
stop
# POST-RUN WORLD CHECK: the measured world is the one whose level.dat LastPlayed moved; it must be New World.
python3 "$(dirname "$0")/worldpos.py" "$GAME/saves" "New World" > "$OUT/worldpos.after" 2>"$OUT/worldpos.after.txt"
if [ "$(cat "$OUT/worldpos.after")" = 0 ]; then echo "[nat] world check OK: New World was played ($(cat "$OUT/worldpos.after.txt"))"; else
  echo "[nat] VOID: world check -- New World is not the most recently played world after the run ($(cat "$OUT/worldpos.after.txt"))"; fi
echo "[nat] done: $OUT"
