#!/usr/bin/env python3
"""xctrace.py <trace> [--from-ms WALL] [--to-ms WALL] [--json] -- per-thread hardware counters from an Instruments
"CPU Counters" capture (darwin; `xcrun xctrace record --template 'CPU Counters'`), the macOS stand-in for hwrun's perf
census.

The template's default mode ("bottlenecks") stamps samples with the PMCs of the CORE the sample was taken on
(cumulative per core, not per thread: two threads sampled on one core continue each other's counts). Indices were
calibrated on an M4 P-core (2026-11-22) with two loops of known instruction count: [2] = instructions, [3] = cycles (a
6-instruction store-reload loop of 4e9 iterations read 25.9e9 / 8.5e9; a 3-instruction mul chain of 2e9 read
6.7e9 / 4.1e9, i.e. a 2-cycle mul). A running thread is sampled every 10 ms by kperf's timer (kdebug 0xc,0x1,0x1).

A thread's counts are summed over CLEAN pairs only: two consecutive timer samples of that thread on the same core,
<= 11 ms apart, with no other sample on that core between them (so no other thread of the process ran there; another
process preempting it inside the 10 ms would be counted, a residual this cannot see). `coverage` is the clean time
over the window: a bench thread that ran the whole window reads ~1.0, and its per-window totals are the clean sums
scaled by 1 / coverage.

WALL times are epoch milliseconds (Date.now()), matched against the trace's start-date plus each sample's offset.
Without bounds, the whole capture. Prints threads by cycles, most first."""
import sys, json, subprocess, argparse, re, datetime
import xml.etree.ElementTree as ET

INSTR, CYC = 2, 3

def export(trace, xpath):
    return subprocess.run(["xcrun", "xctrace", "export", "--input", trace] + (["--xpath", xpath] if xpath else ["--toc"]),
                          check=True, capture_output=True).stdout

def start_epoch_ms(trace):
    toc = export(trace, None).decode()
    m = re.search(r"<start-date>([^<]+)</start-date>", toc)
    return datetime.datetime.fromisoformat(m.group(1)).timestamp() * 1000

TIMER = (0xc, 0x1, 0x1)

def samples(trace):
    """[(t_ns, core, thread, code, counters)] sorted by time"""
    xml = export(trace, '/trace-toc/run[@number="1"]/data/table[@schema="kdebug-counters-with-kdebug-sample"]')
    root = ET.fromstring(xml)
    ids = {}
    def reg(e):
        i = e.get("id")
        if i is not None: ids[i] = e
        for c in e: reg(c)
    def deref(e): return ids.get(e.get("ref"), e) if e is not None else None
    out = []
    for row in root.iter("row"):
        cells = list(row)
        for c in cells: reg(c)
        t, th, core, pm = deref(cells[0]), deref(cells[1]), deref(cells[2]), deref(cells[-1])
        if pm is None or not pm.text or th is None or core is None: continue
        code = tuple(int(deref(x).text) for x in cells[6:9])
        out.append((int(t.text), int(core.text), th.get("fmt"), code, [int(x) for x in pm.text.split()]))
    out.sort(key=lambda s: s[0])
    return out

def clean_pairs(rows, lo, hi):
    """per thread: {instructions, cycles, clean_ns, pairs}"""
    last = {}   # core -> previous sample
    acc = {}
    for r in rows:
        t, core, th, code, v = r
        p = last.get(core)
        last[core] = r
        if p is None or p[2] != th or p[3] != TIMER or code != TIMER: continue
        if t - p[0] > 11_000_000: continue
        if (lo is not None and p[0] < lo) or (hi is not None and t > hi): continue
        a = acc.setdefault(th, {"instructions": 0, "cycles": 0, "clean_ns": 0, "pairs": 0})
        a["instructions"] += v[INSTR] - p[4][INSTR]; a["cycles"] += v[CYC] - p[4][CYC]
        a["clean_ns"] += t - p[0]; a["pairs"] += 1
    return acc

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace"); ap.add_argument("--from-ms", type=float); ap.add_argument("--to-ms", type=float)
    ap.add_argument("--json", action="store_true"); ap.add_argument("--top", type=int, default=6)
    a = ap.parse_args()
    base = start_epoch_ms(a.trace)
    lo = (a.from_ms - base) * 1e6 if a.from_ms is not None else None
    hi = (a.to_ms - base) * 1e6 if a.to_ms is not None else None
    rs = samples(a.trace)
    if lo is None: lo = rs[0][0] if rs else 0
    if hi is None: hi = rs[-1][0] if rs else 0
    win_ns = hi - lo
    rows = []
    for name, x in clean_pairs(rs, lo, hi).items():
        cov = x["clean_ns"] / win_ns if win_ns > 0 else 0
        rows.append({"thread": name, "pairs": x["pairs"], "coverage": cov, "clean_s": x["clean_ns"] / 1e9,
                     "instructions": x["instructions"] / cov if cov else 0, "cycles": x["cycles"] / cov if cov else 0,
                     "clean_instructions": x["instructions"], "clean_cycles": x["cycles"]})
    rows.sort(key=lambda r: -r["cycles"])
    for r in rows: r["ipc"] = r["instructions"] / r["cycles"] if r["cycles"] else None
    if a.json:
        print(json.dumps({"trace_start_ms": base, "from_ms": a.from_ms, "to_ms": a.to_ms, "window_s": win_ns / 1e9, "threads": rows[:a.top]}))
        return
    for r in rows[:a.top]:
        print(f"{r['cycles']/1e9:9.3f} G cyc {r['instructions']/1e9:9.3f} G ins  IPC {r['ipc'] or 0:5.2f}  coverage {r['coverage']:5.2f}  {r['pairs']:6d} pairs  {r['thread'][:70]}")

if __name__ == "__main__":
    main()
