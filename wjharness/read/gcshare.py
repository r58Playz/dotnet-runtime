#!/usr/bin/env python3
"""How much of the in-game window is stop-the-world GC?

Reads a Chrome stderr log captured with --verbose-console (mono logs GC at DEBUG, which the runtime maps to
console.debug, which Chrome files as VERBOSE -- so without --v=1 there are NO GC lines and it looks like
there were no collections). Correlates against the benchStart/benchEnd markers so the answer is about the
MEASURED window, not the whole session (boot + world generation dominate a whole-session total and are not
what the fps number is about).

Line shape:
  [t13f1a198][MONO:Mono] GC_MINOR: (Nursery full) time 43.92ms, stw 43.93ms promoted 18698K
      major size: 249216K in use: 243476K empty reserved: 0K los size: 154798K in use: 151529K

Usage: gcshare.py <log> [<log>...]
"""
import re
import sys

# Chrome stderr prefixes each console line with [pid:tid:MMDD/HHMMSS.uuuuuu:LEVEL:CONSOLE:n]; the timestamp
# is what lets a GC be placed inside or outside the measurement window.
TS = re.compile(r"^\[[\d:]+[:/]?(\d{4})/(\d{2})(\d{2})(\d{2})\.(\d+)")
GC = re.compile(
    r"GC_(MINOR|MAJOR)[^\"]*?time (\d+\.\d+)ms, stw (\d+\.\d+)ms"
    r"(?:.*?promoted (\d+)K)?(?:.*?major size: (\d+)K in use: (\d+)K)?(?:.*?los size: (\d+)K in use: (\d+)K)?"
)


def secs(line):
    m = TS.match(line)
    if not m:
        return None
    _, hh, mm, ss, us = m.groups()
    return int(hh) * 3600 + int(mm) * 60 + int(ss) + int(us) / 1e6


for path in sys.argv[1:]:
    lines = open(path, errors="replace").read().split("\n")
    t0 = t1 = None
    events = []
    for ln in lines:
        if "benchStart" in ln and t0 is None:
            t0 = secs(ln)
        if "benchEnd" in ln and t1 is None:
            t1 = secs(ln)
        m = GC.search(ln)
        if m:
            events.append((secs(ln), m.groups()))

    if not events:
        print(f"{path}: NO GC lines — was the run captured with --verbose-console?")
        continue

    def report(label, sel):
        n_min = sum(1 for _, g in sel if g[0] == "MINOR")
        n_maj = sum(1 for _, g in sel if g[0] == "MAJOR")
        stw = sum(float(g[2]) for _, g in sel)
        prom = sum(int(g[3]) for _, g in sel if g[3])
        worst = max((float(g[2]) for _, g in sel), default=0.0)
        print(f"  {label:22s} minor={n_min:5d} major={n_maj:4d} stw={stw / 1000:8.2f}s "
              f"worst={worst:7.2f}ms promoted={prom / 1024:8.1f}MB")
        return stw

    print(f"{path}")
    report("whole session", events)
    if t0 is not None and t1 is not None:
        win = [(t, g) for t, g in events if t is not None and t0 <= t <= t1]
        stw = report("in-game window", win)
        dur = (t1 - t0) * 1000.0
        print(f"  window {dur / 1000:.1f}s -> STW is {100.0 * stw / dur:.1f}% of it")
        # A frame at 30 fps has 33.3ms. Count collections that alone exceed a frame budget.
        over = [float(g[2]) for _, g in win if float(g[2]) > 33.3]
        print(f"  collections whose STW alone exceeds a 30fps frame (33.3ms): {len(over)}"
              + (f", total {sum(over) / 1000:.2f}s" if over else ""))
        # steady-state heap occupancy at the last collection in the window
        tail = [g for _, g in win if g[5]]
        if tail:
            g = tail[-1]
            print(f"  at window end: major in use {int(g[5]) / 1024:.0f}MB, los in use "
                  f"{int(g[7]) / 1024:.0f}MB" if g[7] else "")
    else:
        print("  (no benchStart/benchEnd timestamps found — window share not computable)")
