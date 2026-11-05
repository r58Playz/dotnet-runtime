#!/usr/bin/env python3
"""gctime.py <chrome/game log> [--list PHASE] -- sgen pauses per phase, from the GC log (MONO_LOG_LEVEL=debug, MASK=gc;
mcperf/worldwait `--knob MONO_LOG_LEVEL=debug`, vcmp `--gclog`).

Each GC prints twice (the worker's "[MONO]" copy and the "[t...][MONO:Mono]" one); the prefixed copy is kept. Phases
come from the log's own lines, for any seed: start = "PreInit...", firstFrame = the first "Reloading ResourceManager",
title = FAILED_AUTH / "Overlay is removed!" / "Game took", join = "joined the game" / StateOutput inworld.
Per phase: minors (count, STW sum, mean, p50, max, promoted MB), majors (count, STW sum, reasons), a concurrent
major's finishing pauses (GC_MAJOR_CONCURRENT_FINISH's stw), and the share of
the phase's wall time spent stopped.
"""
import re, sys, statistics, collections

a = sys.argv[1:]
path = a[0]
LIST = a[a.index("--list") + 1] if "--list" in a else None
TS = re.compile(r"^\[\d+:\d+:\d{4}/(\d\d)(\d\d)(\d\d)\.(\d+):")
MARKS = [("start", r'"PreInit\.\.\."'), ("firstFrame", r"Reloading ResourceManager"),
         ("title", r"Update Status: FAILED_AUTH|Overlay is removed!|Game took [\d.]+ seconds to start"),
         ("join", r"joined the game|StateOutput State: inworld")]
GC = re.compile(r"\[t[0-9a-f]+\]\[MONO:Mono\] (GC_MINOR|GC_MAJOR|GC_MAJOR_CONCURRENT_FINISH): \(([^)]*)\) time ([\d.]+)ms, stw ([\d.]+)ms(?: promoted (\d+)K)?")
marks, gcs, last = {}, [], None
for line in open(path, errors="replace"):
    m = TS.match(line)
    if not m:
        continue
    t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1e6
    last = t
    for n, rx in MARKS:
        if n not in marks and re.search(rx, line):
            marks[n] = t
    g = GC.search(line)
    if g:
        gcs.append((t, g[1], g[2], float(g[3]), float(g[4]), int(g[5] or 0) / 1024))
if "start" not in marks:
    sys.exit("no PreInit line: not an app log")
bounds = [("boot: start -> first frame", "start", "firstFrame"), ("boot: first frame -> title", "firstFrame", "title"),
          ("world: title -> join", "title", "join"), ("in game: join -> end of log", "join", None)]
print(f"{path}: {len(gcs)} collections; marks " + ", ".join(f"{k} +{v - marks['start']:.1f}s" for k, v in marks.items()))
for name, a0, a1 in bounds:
    if a0 not in marks:
        continue
    t0 = marks[a0]; t1 = marks.get(a1, last) if a1 else last
    sel = [g for g in gcs if t0 <= g[0] < t1]
    mi = [g for g in sel if g[1] == "GC_MINOR"]; ma = [g for g in sel if g[1] == "GC_MAJOR"]
    cf = [g for g in sel if g[1] == "GC_MAJOR_CONCURRENT_FINISH"]
    wall = max(t1 - t0, 1e-9)
    stw = sum(g[4] for g in sel) / 1000
    print(f"\n{name}: {wall:.1f} s, stopped {stw:.2f} s ({100 * stw / wall:.1f}%)")
    if mi:
        p = sorted(g[4] for g in mi)
        print(f"  minors {len(mi):4d}  stw {sum(p) / 1000:6.2f} s  mean {statistics.mean(p):6.1f}  p50 {p[len(p) // 2]:6.1f}  max {p[-1]:6.1f} ms"
              f"  promoted {sum(g[5] for g in mi):7.1f} MB  ({60 * len(mi) / wall:.1f}/min)")
    if ma:
        r = collections.Counter(g[2] for g in ma)
        p = sorted(g[4] for g in ma)
        print(f"  majors {len(ma):4d}  stw {sum(p) / 1000:6.2f} s  mean {statistics.mean(p):6.1f}  max {p[-1]:6.1f} ms  reasons "
              + ", ".join(f"{k} x{v}" for k, v in r.most_common()))
    if cf:
        p = sorted(g[4] for g in cf)
        print(f"  conc finish {len(cf):3d}  stw {sum(p) / 1000:6.2f} s  mean {statistics.mean(p):6.1f}  max {p[-1]:6.1f} ms  (concurrent major's final pause)")
    if LIST and LIST in name:
        for g in sel:
            print(f"    +{g[0] - marks['start']:8.2f}s {g[1]:8s} {g[2]:16s} stw {g[4]:7.1f} ms  promoted {g[5]:6.1f} MB")
