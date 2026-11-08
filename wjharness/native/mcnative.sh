#!/usr/bin/env bash
# mcnative.sh -- the STANDALONE native Minecraft reference, captured the way hwrun.sh captures ours, so the same readers
# (hwcensus, perfraw, mccmp) read both sides.
#
#   mcnative.sh [--stage sa262n] [--jdk 25 | --ikvm ARM] [--out DIR] [--warm-s 183] [--bench-s 60] [--no-perf]
#               [--taskset CPUS] [--java-opt OPT]... [--force] [--keep-game] [--agent-full]
#
# --ikvm ARM runs the same stage on IKVM over NATIVE CoreCLR instead of HotSpot (2026-11-08): ARM is a directory with
# jre/bin/java and an env.sh to source (or the path of another env file in it, for another IKVM configuration) ($WJ_STATE/arms/ikvm-clr10: ikvmcraft's served IKVM DLLs on Microsoft's CoreCLR
# 10.0.14, see its README.md). No HotSpot flags, no JVMTI agent, no jcmd: with perf, CoreCLR writes its own perf map
# (DOTNET_PerfMapEnabled=3) into the run directory as perf-<pid>.map, where the readers find HotSpot's. Boot and world
# load are minutes, not seconds, so the window and join waits are longer (MCN_WINDOW_S, MCN_JOIN_S).
#
# THE WORKLOAD is the stage mcstage.sh builds from the tar ours plays: the same mods, config, options.txt and save,
# copied fresh into <out>/game for every run (the save advances as it plays). Nothing under Prism is touched; Prism
# supplies libraries, LWJGL and asset objects, read-only.
#
# THE CAPTURE matches protocol Q60 (hwrun.sh): the window opens --warm-s after the world is joined (Q60 lands ~183 s
# after join on 262n, the default stage sa262n; ~198 s on 1.16.1's mcsr, stage sa) and lasts --bench-s; perf records hwrun's event set at hwrun's periods with frame-pointer call chains
# (-k 1 -g), the window is written to phases.json as the `ingame` phase in CLOCK_MONOTONIC, and game.log carries
# `[mcw] START` / `[mcw] END` around it, so hwcensus's tick denominator (window x 20 - logged "ticks behind") is the
# same rule on both sides. Java frames are named by `jcmd Compiler.perfmap` taken after the window (JDK 17+; under
# JDK 8 the capture is unnamed). -XX:+PreserveFramePointer is on whenever perf records, so -g walks compiled frames;
# it is part of the reference and stated in run.json.
#
# Lessons carried over from natrun.sh (each cost a run): Xwayland readiness gates on xdpyinfo AND a keymap query; one
# X client stays open so the rootful window does not flicker; LWJGL 3.2.2 is X11-only (WAYLAND_DISPLAY unset); with no
# window manager, focus with XSetInputFocus and send XTEST keys; MCSR Ranked's auth dialog is dismissed by Tab,Enter
# (Escape does not work) and its FAILED_AUTH line marks the modded main screen as built; the player is the offline
# UUID of "ikvmcraft", whose playerdata the save holds (another UUID spawns a fresh entity at world spawn).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
JDK=25; IKVM=""; OUT=""; WARM=""; BENCH=60; PERF=1; CPUS="$WJ_PCPUS"; JOPTS=(); FORCE=0; KEEP=0; AGMODE=",lite"; STAGE=sa262n
while [ $# -gt 0 ]; do
  case "$1" in
    --jdk) JDK=$2; shift ;;
    --ikvm) IKVM=$2; shift ;;
    --stage) STAGE=$2; shift ;;   # a stage mcstage.sh built: sa262n (26.2, no mods: the target), sa (1.16.1 + MCSR Ranked)
    --out) OUT=$2; shift ;;
    --warm-s) WARM=$2; shift ;;
    --bench-s) BENCH=$2; shift ;;
    --no-perf) PERF=0 ;;
    --taskset) CPUS=$2; shift ;;
    --java-opt) JOPTS+=("$2"); shift ;;
    --force) FORCE=1 ;;
    --keep-game) KEEP=1 ;;
    --agent-full) AGMODE="" ;;   # the agent also writes every nmethod's code bytes (mcinsn.py); larger output
    *) echo "mcnative: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
# Q60's offset after join is the seed's (hwrun's --warm-ms is fixed; the join lands at a different time per seed)
[ -n "$WARM" ] || { [ "$STAGE" = sa ] && WARM=198 || WARM=183; }
SA="${SA:-$WJ_STATE/native/$STAGE}"
[ -s "$SA/cp.txt" ] && [ -e "$SA/natives/liblwjgl.so" ] && [ -d "$SA/template/saves/New World" ] \
  || { echo "[mcn] VOID: no stage at $SA -- run: wj mcstage"; exit 1; }
# launch.env: MC_MAIN, MC_ASSET_INDEX, MC_GAME_VERSION, MC_VERSION_TYPE, MC_JOIN (mcsr | quickplay). A stage built before
# launch.env existed is the 1.16.1 MCSR one.
MC_MAIN=net.fabricmc.loader.impl.launch.knot.KnotClient; MC_ASSET_INDEX=1.16; MC_GAME_VERSION=1.16.1; MC_VERSION_TYPE=release; MC_JOIN=mcsr
[ -e "$SA/launch.env" ] && . "$SA/launch.env"
PH=$(need PRISM_HOME "asset objects are read from \$PRISM_HOME/assets")
if [ -n "$IKVM" ]; then
  # --ikvm ARM sources ARM/env.sh; --ikvm ARM/<file>.sh sources that file instead (another IKVM configuration, same arm)
  IKVMENV="$IKVM/env.sh"; [ -f "$IKVM" ] && { IKVMENV=$(realpath "$IKVM"); IKVM=$(dirname "$IKVMENV"); }
  IKVM=$(cd "$IKVM" 2>/dev/null && pwd) && [ -x "$IKVM/jre/bin/java" ] && [ -f "$IKVMENV" ] \
    || { echo "[mcn] VOID: --ikvm needs a directory with jre/bin/java and env.sh (or one of its env files)"; exit 1; }
  . "$IKVMENV"; JH=""; JAVA="$IKVM/jre/bin/java"; RUNTIME="ikvm-clr:$(basename "$IKVMENV" .sh)"; HEAPF=()
  WINDOW_S="${MCN_WINDOW_S:-900}"; JOIN_S="${MCN_JOIN_S:-1200}"
else
  JH=$(need "JAVA_HOME_$JDK" "the JDK this run uses (--jdk $JDK)")
  JAVA="$JH/bin/java"; RUNTIME="hotspot"; HEAPF=(-Xms6144m -Xmx6144m)
  WINDOW_S="${MCN_WINDOW_S:-180}"; JOIN_S="${MCN_JOIN_S:-360}"
fi
[ "$PERF" = 1 ] && pmu_require
OUT="${OUT:-$(wj_rundir "mcnative-$STAGE-$([ -n "$IKVM" ] && echo ikvmclr || echo "jdk$JDK")")}"; OUT=$(wj_outdir "$OUT") || exit 1; mkdir -p "$OUT"
LOG="$OUT/game.log"; : > "$LOG"
mono () { python3 -c 'import time; print(f"{time.monotonic():.6f}")'; }   # perf -k 1's clock
note () { echo "[mcn] $*" | tee -a "$OUT/mcn.txt"; }

if [ "$FORCE" = 0 ] && ! node -e 'import(process.argv[1]).then(m => { m.preflight({ log: console.log }); process.exit(0); })' \
     "$WJ_HARNESS/lib/preflight.mjs" > "$OUT/preflight.txt" 2>&1; then
  tail -5 "$OUT/preflight.txt"; note "VOID: preflight refused (--force to override)"; exit 1
fi

# A FREE display number: never attach to an X server some other session owns.
XN=20; while [ -e "/tmp/.X11-unix/X$XN" ] || [ -e "/tmp/.X$XN-lock" ]; do XN=$((XN + 1)); done
XDISP=":$XN"; XGEOM="${XGEOM:-1600x900}"

G="$OUT/game"
cp -a --reflink=auto "$SA/template" "$G"
note "out=$OUT stage=$STAGE ($MC_GAME_VERSION, join $MC_JOIN) runtime=$RUNTIME${IKVM:+ ($IKVM)} jdk=$JDK warm=${WARM}s bench=${BENCH}s perf=$PERF cpus=$CPUS display=$XDISP"
"$JAVA" -version > "$OUT/java-version.txt" 2>&1

setsid Xwayland "$XDISP" -geometry "$XGEOM" > "$OUT/xwayland.log" 2>&1 &
XPG=$!
PG=""; XPROP=""; PRPID=""
stop () {
  [ -n "$PRPID" ] && kill -INT "$PRPID" 2>/dev/null
  [ -n "$PG" ] && { kill -TERM -- -"$PG" 2>/dev/null; sleep 6; kill -9 -- -"$PG" 2>/dev/null; }
  [ -n "$XPROP" ] && kill -9 -- -"$XPROP" 2>/dev/null
  kill -9 -- -"$XPG" 2>/dev/null
  # Xwayland killed -9 leaves its lock and socket, and each run would take the next display number.
  sleep 0.5; [ "$(tr -d ' \n' < "/tmp/.X$XN-lock" 2>/dev/null)" = "$XPG" ] && rm -f "/tmp/.X$XN-lock" "/tmp/.X11-unix/X$XN"
  [ "$KEEP" = 1 ] || rm -rf "$G/saves" "$G/mods"   # 50 MB a run; logs/, config/ and crash-reports/ stay
}
void () { note "VOID: $*"; echo "{\"verdict\": \"void\", \"why\": \"$*\"}" > "$OUT/run.json"; stop; exit 1; }
# A crash in Fabric's or the game's main thread can leave the JVM alive (non-daemon threads), so kill -0 alone misses it.
alive () { kill -0 "$JPID" 2>/dev/null && ! grep -aqE 'Uncaught exception in thread "main"|---- Minecraft Crash Report' "$LOG"; }
ready=0
for i in $(seq 1 40); do
  DISPLAY="$XDISP" xdpyinfo >/dev/null 2>&1 && DISPLAY="$XDISP" xdotool getdisplaygeometry >/dev/null 2>&1 && { ready=1; break; }
  sleep 0.5
done
[ "$ready" = 1 ] || void "$XDISP never became usable"
sleep 2
DISPLAY="$XDISP" glxinfo -B 2>/dev/null | grep -q "OpenGL renderer" || void "no GL on $XDISP"
setsid env DISPLAY="$XDISP" xprop -root -spy >/dev/null 2>&1 &
XPROP=$!
DISPLAY="$XDISP" glxinfo -B 2>/dev/null | grep -E "OpenGL renderer|OpenGL core profile version" | sed 's/^/[mcn] /'

# With perf: frame pointers for -g, and j2dagent (j2d/jvmti, ,lite) for every nmethod's inlining-aware pc map, so
# mccmp expands each compiled frame into the Java methods HotSpot inlined there (perf's frames are physical only; on
# 2026-11-06 a mob's tickMovement read 72x ours because HotSpot had inlined it into its caller's frame).
# -XX:+DebugNonSafepoints records pc descriptors between safepoints too; it changes debug info, not code.
PFP=()
if [ "$PERF" = 1 ] && [ -n "$IKVM" ]; then
  # perf map only (3: no jitdump), tier in each name, written straight into the run directory
  export DOTNET_PerfMapEnabled=3 DOTNET_PerfMapShowOptimizationTiers=1 DOTNET_PerfMapJitDumpPath="$OUT"
elif [ "$PERF" = 1 ]; then
  AG="$WJ_STATE/arms/jvmti/libj2dagent.so"
  [ -e "$AG" ] || void "no JVMTI agent at $AG -- run: wj j2d/jvmti/build.sh"
  PFP=(-XX:+PreserveFramePointer -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints "-agentpath:$AG=$OUT/j2dagent.txt$AGMODE")
fi
# --quickPlaySingleplayer (1.20+) loads the world straight from the title screen: no keyboard, no focus. --userType is
# an argument 1.16.1's json passes and 26.x's no longer does.
GARGS=(--userType legacy); [ "$MC_JOIN" = quickplay ] && GARGS=(--quickPlaySingleplayer "New World")
cd "$G"
setsid env -u WAYLAND_DISPLAY DISPLAY="$XDISP" taskset -c "$CPUS" \
  "$JAVA" "${PFP[@]}" "${JOPTS[@]}" -Duser.language=en "${HEAPF[@]}" \
    -Djava.library.path="$SA/natives" -Dorg.lwjgl.librarypath="$SA/natives" \
    -cp "$(cat "$SA/cp.txt")" "$MC_MAIN" \
    --gameDir "$G" --assetsDir "$PH/assets" --assetIndex "$MC_ASSET_INDEX" --version "$MC_GAME_VERSION" \
    --username ikvmcraft --uuid 6378dd50-248d-3a1f-9061-91e0c45ce092 \
    --accessToken 0 --versionType "$MC_VERSION_TYPE" "${GARGS[@]}" \
  >> "$LOG" 2>&1 &
PG=$!
# The java pid, by THIS run's game dir in its command line (another JVM on the box -- Prism's -- must not match).
JPID=""
for i in $(seq 1 60); do
  for c in $(pgrep -x java 2>/dev/null); do
    tr '\0' '\n' < "/proc/$c/cmdline" 2>/dev/null | grep -qxF "$G" && { JPID=$c; break; }
  done
  [ -n "$JPID" ] && break; sleep 1
done
[ -n "$JPID" ] || void "java never started"
note "java pid=$JPID ($(head -1 "$OUT/java-version.txt"))"

WID=""
for i in $(seq 1 $((WINDOW_S / 2))); do
  WID=$(DISPLAY="$XDISP" xdotool search --name "Minecraft" 2>/dev/null | head -1)
  [ -n "$WID" ] && break
  alive || { grep -a -m1 -A8 "Crash Report\|Exception in thread\|Error:" "$LOG" | sed 's/^/[mcn]   /'; void "java exited before a window appeared"; }
  sleep 2
done
[ -n "$WID" ] || void "no Minecraft window"
JOIN=""
if [ "$MC_JOIN" = mcsr ]; then
for i in $(seq 1 150); do grep -aq FAILED_AUTH "$LOG" && break; alive || void "java exited at the main screen"; sleep 2; done
grep -aq FAILED_AUTH "$LOG" && note "main screen ready" || note "WARNING: no FAILED_AUTH line, replaying the menu anyway"
sleep 3
DISPLAY="$XDISP" xdotool windowfocus --sync "$WID" 2>/dev/null || DISPLAY="$XDISP" xdotool windowfocus "$WID" 2>/dev/null \
  || note "WARNING: windowfocus failed"
SHOT=0
shot () { SHOT=$((SHOT + 1)); DISPLAY="$XDISP" import -window root "$OUT/menu$(printf %02d $SHOT)-$1.png" 2>/dev/null; }
k () { DISPLAY="$XDISP" xdotool key --clearmodifiers "$1"; sleep "$2"; shot "$1"; }
shot start
# Tab,Enter: dismiss Ranked's auth dialog. Tab,Enter: Singleplayer. Tab,Down,Up: the first (only) world. Tab,Enter: Play.
k Tab 0.7; k Return 2.5
k Tab 0.7; k Return 3.0
k Tab 0.7; k Down 1.0; k Up 1.0
k Tab 0.7; k Return 2.0
for i in $(seq 1 180); do
  grep -aq "StateOutput State: inworld" "$LOG" && { JOIN=$(mono); break; }
  alive || void "java exited before joining"
  sleep 2
done
else
# quickplay: the integrated server logs the player joining, the same line ours' harness keys on (lib/seeds.mjs joinRx)
for i in $(seq 1 $((JOIN_S / 2))); do
  grep -aq "ikvmcraft joined the game" "$LOG" && { JOIN=$(mono); break; }
  alive || void "java exited before joining"
  sleep 2
done
fi
[ -n "$JOIN" ] || void "never reached in-world in $JOIN_S s"
note "in world; window opens in ${WARM}s"
sleep "$WARM"
alive || void "java exited during warm-up"

pick () { local best="" bestj=-1 t c j
  for t in /proc/"$JPID"/task/*; do
    c=$(cat "$t/comm" 2>/dev/null) || continue; [ "$c" = "$1" ] || continue
    j=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null) || continue
    [ "$j" -gt "$bestj" ] && { bestj=$j; best=$(basename "$t"); }
  done; echo "$best"; }
STID=$(pick "Server thread"); RTID=$(pick "Render thread")
[ -n "$STID" ] || void "no Server thread"
for t in /proc/"$JPID"/task/*; do echo "$(basename "$t") $(cat "$t/comm" 2>/dev/null)"; done > "$OUT/threads.txt"
echo "$STID" > "$OUT/server.tid"
note "Server thread tid=$STID, Render thread tid=$RTID"

if [ "$PERF" = 1 ]; then
  C="$WJ_PMU_CORE"; A="$WJ_PMU_ATOM"
  EV="${MCN_EVENTS:-instructions/period=2000003/u,cycles/period=2000003/u,$C/event=0xc4,umask=0x02,period=200003/up,$A/event=0xc4,umask=0xf9,period=200003/up,$C/event=0xc4,umask=0x02,period=400009/upp,$A/event=0xc4,umask=0xf9,period=400009/upp,$C/event=0xc4,umask=0x80,period=400009/u,$A/event=0xc4,umask=0xeb,period=400009/u,$C/event=0xd0,umask=0x81,period=2000003/u,$C/event=0xd0,umask=0x82,period=1000003/u,$C/event=0xc4,umask=0x11,period=2000003/u,$A/event=0xc4,umask=0x7e,period=2000003/u}"
  perf record -q -k 1 -g -e "$EV" -p "$JPID" -o "$OUT/perf.data" > "$OUT/perfrec.log" 2>&1 &
  PRPID=$!
  sleep 3
  kill -0 "$PRPID" 2>/dev/null || void "perf record exited: $(head -3 "$OUT/perfrec.log")"
fi
T0=$(mono); echo "[mcw] START" >> "$LOG"
sleep "$BENCH"
T1=$(mono); echo "[mcw] END" >> "$LOG"
alive || void "java exited inside the window"
INW=0
if [ "$MC_JOIN" = mcsr ]; then grep -a "StateOutput State:" "$LOG" | tail -1 | grep -q inworld && INW=1
else grep -aq "ikvmcraft left the game" "$LOG" || INW=1; fi
if [ "$PERF" = 1 ]; then
  if [ -n "$IKVM" ]; then
    [ -s "$OUT/perf-$JPID.map" ] && note "CoreCLR perf map: $(wc -l < "$OUT/perf-$JPID.map") symbols" \
      || note "WARNING: no CoreCLR perf map at $OUT/perf-$JPID.map -- JIT frames stay unnamed"
  elif [ -x "$JH/bin/jcmd" ] && [ "$JDK" -ge 17 ]; then
    "$JH/bin/jcmd" "$JPID" Compiler.perfmap "$OUT/perf-$JPID.map" > "$OUT/perfmap.log" 2>&1 \
      || note "WARNING: jcmd Compiler.perfmap failed: $(tail -1 "$OUT/perfmap.log")"
  else
    note "WARNING: JDK $JDK has no Compiler.perfmap -- Java frames stay unnamed"
  fi
  kill -INT "$PRPID" 2>/dev/null
  for i in $(seq 1 120); do kill -0 "$PRPID" 2>/dev/null || break; sleep 1; done
  PRPID=""
fi
SKIP=$(awk '/\[mcw\] START/{w=1} /\[mcw\] END/{w=0} w && /ticks behind/ { if (match($0, /or ([0-9]+) ticks behind/, m)) s += m[1] } END {print s+0}' "$LOG")
python3 - "$OUT" "$T0" "$T1" "$JOIN" "$JDK" "$JPID" "$STID" "${RTID:-0}" "$INW" "$SKIP" "$CPUS" "$PERF" "${JOPTS[*]:-}" "$RUNTIME" "$IKVM" <<'EOF'
import json, os, sys
out, t0, t1, join, jdk, pid, stid, rtid, inw, skip, cpus, perf, jopts, runtime, ikvm = sys.argv[1:]
t0, t1, join = float(t0), float(t1), float(join)
v = "ok" if inw == "1" else "left-world"
json.dump({"phases": [{"name": "ingame", "t0": t0, "t1": t1}], "join": join, "native": True},
          open(os.path.join(out, "phases.json"), "w"), indent=1)
json.dump({"verdict": v, "jdk": int(jdk), "java_version": open(os.path.join(out, "java-version.txt")).read().splitlines()[0],
           "pid": int(pid), "server_tid": int(stid), "render_tid": int(rtid), "window_s": t1 - t0,
           "window_after_join_s": t0 - join, "skipped_ticks": int(skip), "taskset": cpus, "perf": perf == "1",
           "preserve_frame_pointer": perf == "1" and runtime == "hotspot", "jvmti_agent": perf == "1" and runtime == "hotspot",
           "java_opts": jopts, "runtime": runtime, "ikvm_arm": ikvm or None,
           "stage": open(os.path.join(os.environ.get("SA") or os.path.join(os.environ["WJ_STATE"], "native", "sa"), "STAGE.txt")).read().splitlines()[-1]},
          open(os.path.join(out, "run.json"), "w"), indent=1)
print(f"[mcn] verdict: {v} | window {t1 - t0:.1f}s at +{t0 - join:.0f}s after join | {skip} ticks skipped")
EOF
stop
note "done: $OUT"
