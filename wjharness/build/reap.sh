#!/bin/bash
# reap.sh [--apply] [--days N] [--runs-older-than DAYS] -- delete what nothing owns any more. `wj reap`.
#
# The layout makes most of this a rule rather than a judgement: everything a driver writes lives in $WJ_RUNS and is
# disposable by construction, $WJ_STATE (seeds, references, built arms, the native stage, the log corpus) and
# $WJ_NOTES (the findings log) are never candidates at any depth, and the code is in git. What still needs care is the
# state that lives OUTSIDE $WJ_HOME:
#
#   * /tmp is a RAM-backed tmpfs and --perf-basic-prof writes a ~240 MB /tmp/perf-<pid>.map per capture. 100 stale
#     maps left 2.7 G free and runs died at PAGE LOAD, which read as a browser bug (R280f). Only dead pids' maps go:
#     every reader uses the copy inside the capture directory.
#   * short-lived /tmp/wj-* profiles (lib/browser.mjs) and V8 isolate logs (--perf-basic-prof implies --log; the
#     launchers pass --no-logfile-per-isolate --logfile=+, this sweeps anything launched without it).
#
# Inside $WJ_RUNS: browser profiles (~1 GB each, kept on a FAILED run because they hold the crash-time OPFS) older
# than --days, 0-byte captures, and with --runs-older-than whole run directories older than that.
#
# DRY RUN BY DEFAULT. Pass --apply to delete. Run it twice: the second run must be a no-op.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"

APPLY=0; DAYS=3; RUNDAYS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1 ;;
    --days)  DAYS="$2"; shift ;;
    --runs-older-than) RUNDAYS="$2"; shift ;;
    *) echo "usage: wj reap [--apply] [--days N] [--runs-older-than DAYS]" >&2; exit 2 ;;
  esac
  shift
done

SEEDS="$WJ_STATE/seeds"
[ -d "$SEEDS/seed" ] || { echo "reap: REFUSING -- $SEEDS/seed is missing. Restore or rebuild it (wj seedbuild) before" >&2
                          echo "      any cleanup; its absence is the signature of the mistake this script exists to avoid." >&2; exit 1; }

total=0
say () { printf '%s\n' "$*"; }
# GNU and BSD (macOS) alike: realpath -m, du -sb and numfmt are GNU-only, and a guard whose realpath fails matches nothing.
rpath () { realpath -m "$1" 2>/dev/null || python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
bytes () { local b; [ -e "$1" ] || { echo 0; return; }
           b=$(du -sb --apparent-size "$1" 2>/dev/null | cut -f1) || b=""
           [ -n "$b" ] || b=$(( $(du -sk "$1" 2>/dev/null | cut -f1) * 1024 )); echo "${b:-0}"; }
human () { numfmt --to=iec "$1" 2>/dev/null || awk -v b="$1" 'BEGIN { split("B K M G T", u); i = 1; while (b >= 1024 && i < 5) { b /= 1024; i++ } printf (i == 1 ? "%d%s\n" : "%.1f%s\n"), b, u[i] }'; }
drop () {                                   # drop <path> <why>
  local p="$1" why="$2" b
  [ -e "$p" ] || return 0
  case "$(rpath "$p")/" in
    "$WJ_STATE"/*|"$WJ_NOTES"/*|"$WJ_HARNESS"/*) say "  REFUSING (state/notes/code): $p"; return 0 ;;
  esac
  b=$(bytes "$p"); total=$((total + b))
  if [ "$APPLY" = 1 ]; then rm -rf -- "$p"; say "  removed  $(human "$b")  $p  [$why]"
  else                       say "  would rm $(human "$b")  $p  [$why]"; fi
}
old () { [ -z "$(find "$1" -maxdepth 0 -mtime -"$2" 2>/dev/null)" ]; }   # old <path> <days>

say "=== /tmp perf symbol maps (dead pids only) ==="
for m in /tmp/perf-*.map; do
  [ -e "$m" ] || continue
  pid="${m##*/perf-}"; pid="${pid%.map}"
  case "$pid" in ''|*[!0-9]*) continue ;; esac
  if kill -0 "$pid" 2>/dev/null; then say "  keeping (pid $pid alive) $m"; else drop "$m" "dead pid $pid"; fi
done

say "=== harness browsers still alive ==="
# Found by command line, kept only if comm is a browser, never self (`pgrep -f` matches the searcher: CLAUDE.md).
for p in $(pgrep -f 'user-data-dir=' 2>/dev/null); do
  [ "$p" = "$$" ] && continue
  c=$(ps -p "$p" -o comm= 2>/dev/null); case "$c" in chrom*|google-chrome*) ;; *) continue ;; esac
  cl=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
  case "$cl" in *"--user-data-dir=$WJ_RUNS/"*|*"--user-data-dir=/tmp/wj-"*) say "  alive: pid $p $(echo "$cl" | grep -o -- '--user-data-dir=[^ ]*')" ;; esac
done

say "=== browser profiles older than ${DAYS}d ==="
while IFS= read -r -d '' d; do
  old "$d" "$DAYS" && drop "$d" "profile older than ${DAYS}d"
done < <(find "$WJ_RUNS" -maxdepth 3 -type d \( -name 'profile-*' -o -name 'profile' -o -name 'browser-profile' -o -name 'worldwait-profile-*' \) -prune -print0 2>/dev/null)
for d in /tmp/wj-*; do [ -d "$d" ] && old "$d" 1 && drop "$d" "lib/browser.mjs temp profile"; done

say "=== v8 isolate logs ==="
find "$WJ_RUNS" "$WJ_REPO" -maxdepth 3 -name 'isolate-*-v8.log' -print0 2>/dev/null |
  while IFS= read -r -d '' f; do drop "$f" "v8 isolate log"; done

say "=== zero-byte captures ==="
find "$WJ_RUNS" -maxdepth 4 \( -name 'perf.data' -o -name '*.data' \) -size 0 -print0 2>/dev/null |
  while IFS= read -r -d '' f; do drop "$f" "0-byte capture"; drop "${f%.data}.map" "map orphaned by a 0-byte capture"; done

if [ -n "$RUNDAYS" ]; then
  say "=== run directories older than ${RUNDAYS}d ==="
  for d in "$WJ_RUNS"/*/; do
    d=${d%/}; [ -d "$d" ] || continue
    old "$d" "$RUNDAYS" && drop "$d" "run older than ${RUNDAYS}d"
  done
fi

say
if [ "$APPLY" = 1 ]; then say "reaped $(human "$total")"
else say "would reap $(human "$total")   (re-run with --apply)"; fi
df -h "$WJ_HOME" /tmp | grep -v ^Filesystem
