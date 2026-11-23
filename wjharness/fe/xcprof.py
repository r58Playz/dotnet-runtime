#!/usr/bin/env python3
"""xcprof.py <trace> <perf.map> [--from-ms WALL] [--to-ms WALL] [--thread SUBSTR] [--top N] [--incl] [--json]
-- where one thread's time goes, by function, from an Instruments "Time Profiler" capture (darwin; 1 ms samples with
user backtraces) and V8's `--perf-basic-prof` map (`/tmp/perf-<renderer pid>.map`, which V8 writes on macOS too: wasm
functions appear as `JS:<name>-<index>-turbofan|liftoff`). The macOS stand-in for perfraw's per-function split.

Frames Instruments symbolised (Chrome's own, libsystem) keep its name; JIT frames (hex names) resolve through the map,
the LAST entry covering the address winning (V8 reuses code space after a tier-up or a GC; the map is append-only).
The thread is the one with the most samples in the window unless --thread names one. --incl adds inclusive shares
(each function counted once per sample it appears anywhere in)."""
import sys, json, subprocess, argparse, re, bisect, datetime, collections
import xml.etree.ElementTree as ET

def export(trace, xpath):
    return subprocess.run(["xcrun", "xctrace", "export", "--input", trace] + (["--xpath", xpath] if xpath else ["--toc"]),
                          check=True, capture_output=True).stdout

def start_epoch_ms(trace):
    toc = export(trace, None).decode()
    return datetime.datetime.fromisoformat(re.search(r"<start-date>([^<]+)</start-date>", toc).group(1)).timestamp() * 1000

class PerfMap:
    def __init__(self, path):
        ents = []
        for i, line in enumerate(open(path, errors="replace")):
            p = line.rstrip("\n").split(" ", 2)
            if len(p) < 3: continue
            try: ents.append((int(p[0], 16), int(p[1], 16), i, p[2]))
            except ValueError: continue
        ents.sort()
        self.starts = [e[0] for e in ents]; self.ents = ents
        self.maxlen = max((e[1] for e in ents), default=0)
    def lookup(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        best = None
        while i >= 0 and a - self.starts[i] <= self.maxlen:
            s, n, order, name = self.ents[i]
            if s <= a < s + n and (best is None or order > best[0]): best = (order, name)
            i -= 1
        return best[1] if best else None

STRIP = re.compile(r"-\d+-(turbofan|liftoff)$")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace"); ap.add_argument("map")
    ap.add_argument("--from-ms", type=float); ap.add_argument("--to-ms", type=float)
    ap.add_argument("--thread"); ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--incl", action="store_true"); ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    pm = PerfMap(a.map)
    base = start_epoch_ms(a.trace)
    lo = (a.from_ms - base) * 1e6 if a.from_ms is not None else None
    hi = (a.to_ms - base) * 1e6 if a.to_ms is not None else None
    root = ET.fromstring(export(a.trace, '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'))
    ids = {}
    def reg(e):
        i = e.get("id")
        if i is not None: ids[i] = e
        for c in e: reg(c)
    D = lambda e: ids.get(e.get("ref"), e) if e is not None else None
    per = collections.defaultdict(list)
    for row in root.iter("row"):
        cs = list(row)
        for c in cs: reg(c)
        t, th, bt = D(cs[0]), D(cs[1]), D(cs[-1])
        tt = int(t.text)
        if (lo is not None and tt < lo) or (hi is not None and tt > hi) or bt is None: continue
        frames = []
        for f in bt.iter("frame"):
            f = D(f); nm = f.get("name") or ""; ad = int(f.get("addr", "0"), 16)
            if nm.startswith("0x") or not nm:
                r = pm.lookup(ad)
                nm = STRIP.sub("", r[3:] if r and r.startswith("JS:") else r) if r else f"?{ad:#x}"
            frames.append(nm)
        per[th.get("fmt")].append(frames)
    if not per: print("no samples in the window"); return
    th = next((k for k in per if a.thread and a.thread in k), None) or max(per, key=lambda k: len(per[k]))
    smp = per[th]; n = len(smp)
    selfc = collections.Counter(f[0] for f in smp if f)
    inc = collections.Counter()
    if a.incl:
        for f in smp: inc.update(set(f))
    rows = [{"function": k, "self": v / n, "incl": inc[k] / n if a.incl else None} for k, v in selfc.most_common(a.top)]
    unresolved = sum(v for k, v in selfc.items() if k.startswith("?")) / n
    if a.json:
        print(json.dumps({"thread": th, "samples": n, "unresolved_self": unresolved, "functions": rows})); return
    print(f"{th}: {n} samples, unresolved self {unresolved:.1%}")
    for r in rows:
        print(f"  {r['self']:6.1%}" + (f" {r['incl']:6.1%}" if a.incl else "") + f"  {r['function'][:110]}")

if __name__ == "__main__":
    main()
