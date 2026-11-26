#!/usr/bin/env python3
"""xcmiss.py <trace> <jitdump> [--from-ms W --to-ms W] [--thread SUBSTR] [--func SUBSTR] [--top N]
-- where a CPU Counters SAMPLING mode's events land (j2d/run --xc-mode l1d_miss_sampling --xc-jit): the pmi-sample
table's exact PCs, one per N events of a counter, grouped by counter id and resolved through V8's jitdump. Without
--func: functions per counter; with --func: that function's instructions per counter (capstone, as fe/xchot.py).
Counter ids are the mode's own, in its trigger order (l1d_miss_sampling: 5 = L1D load misses, 6 = L1D store misses,
7 = L1D TLB misses -- checked 2026-11-26 by the sampled instruction: 5 is 99% loads, 6 74% stores, 7 98% loads + 1% stores;
do not identify them by population). The ids are printed with their counts."""
import sys, os, argparse, collections, importlib.util
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ledger"))
from jitindex import JitIndex
spec = importlib.util.spec_from_file_location("xcprof", os.path.join(HERE, "xcprof.py")); xcprof = importlib.util.module_from_spec(spec); spec.loader.exec_module(xcprof)

def samples(trace, lo, hi):
    root = ET.fromstring(xcprof.export(trace, '/trace-toc/run[@number="1"]/data/table[@schema="pmi-sample"]'))
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
        t, th, ctr, pc = D(cs[0]), D(cs[1]), D(cs[5]), D(cs[6])
        if t is None or th is None or pc is None or pc.text is None: continue
        tt = int(t.text)
        if (lo is not None and tt < lo) or (hi is not None and tt > hi): continue
        per[th.get("fmt")].append((int(ctr.text), int(pc.text) & ~3))
    return per

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace"); ap.add_argument("jitdump")
    ap.add_argument("--from-ms", type=float); ap.add_argument("--to-ms", type=float)
    ap.add_argument("--thread"); ap.add_argument("--func"); ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    base = xcprof.start_epoch_ms(a.trace)
    lo = (a.from_ms - base) * 1e6 if a.from_ms is not None else None
    hi = (a.to_ms - base) * 1e6 if a.to_ms is not None else None
    per = samples(a.trace, lo, hi)
    th = next((k for k in per if a.thread and a.thread in k), None) or max(per, key=lambda k: len(per[k]))
    ss = per[th]; idx = JitIndex(a.jitdump); BIG = 1 << 62
    byctr = collections.Counter(c for c, _ in ss)
    print(f"{th}: {len(ss)} samples; per counter id: " + ", ".join(f"{c}: {n}" for c, n in sorted(byctr.items())))
    fn = collections.defaultdict(collections.Counter); ins = collections.defaultdict(collections.Counter); rec = {}
    for c, pc in ss:
        r = idx.at(pc, BIG)
        name = idx.name(r) if r >= 0 else "(outside JIT code)"
        fn[c][name] += 1
        if r >= 0 and a.func and a.func in name:
            ins[c][pc - idx.start[r]] += 1; rec[idx.key(r)] = r
    if not a.func:
        for c in sorted(fn):
            print(f"-- counter {c}")
            for name, n in fn[c].most_common(a.top): print(f"  {n / byctr[c]:6.1%}  {name[:120]}")
        return
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.skipdata = True
    if not rec: print("no samples in", a.func); return
    r = max(rec.values(), key=lambda r: idx.size[r])
    code = idx.code(r)
    print(f"{idx.name(r)}: {len(code)} B")
    for x in md.disasm(code, 0):
        cnt = {c: ins[c].get(x.address, 0) for c in ins}
        if any(cnt.values()):
            print("  " + " ".join(f"{c}:{cnt[c]:4d}" for c in sorted(cnt)) + f"  {x.address:6x}  {x.mnemonic:8s} {x.op_str}")

if __name__ == "__main__":
    main()
