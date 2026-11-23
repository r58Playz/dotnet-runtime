#!/usr/bin/env python3
"""xchot.py <trace> <jitdump> [--from-ms W --to-ms W] [--thread SUBSTR] [--func SUBSTR] [--top N] [--ctx N]
-- instruction-level hot spots on macOS: Instruments' Time Profiler leaf PCs (1 ms) mapped through V8's jitdump
(`--perf-prof`, which V8 writes on macOS too; `j2d/run --xcprof-jit` keeps one) to each function's arm64 code.

Without --func: functions by self samples (from the jitdump's names). With --func: the first function whose name holds
the substring (the one with most samples), disassembled with capstone (`~/.cache/wjharness-venv`: `python3 -m venv` +
`pip install capstone`), each instruction with its samples, runs of cold code elided, plus samples by mnemonic class.
A timer sample lands on the instruction the core was retiring or stalled at; a long-latency load usually charges the
instruction after it. The jitdump's own clock is not matched to the trace: the LAST code load covering a PC wins."""
import sys, os, argparse, collections, importlib.util
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ledger"))
from jitindex import JitIndex
spec = importlib.util.spec_from_file_location("xcprof", os.path.join(HERE, "xcprof.py")); xcprof = importlib.util.module_from_spec(spec); spec.loader.exec_module(xcprof)

def leaf_samples(trace, lo, hi, thread):
    root = ET.fromstring(xcprof.export(trace, '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'))
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
        f = next(iter(bt.iter("frame")), None)
        if f is None: continue
        f = D(f)
        per[th.get("fmt")].append(int(f.get("addr", "0"), 16) & ~3)   # Instruments reports addr+1; arm64 is 4-aligned
    th = next((k for k in per if thread and thread in k), None) or max(per, key=lambda k: len(per[k]))
    return th, per[th]

CLS = [("load", ("ldr", "ldur", "ldp", "ldrb", "ldrh", "ldrsw", "ldrsb", "ldrsh", "ldar", "ldxr", "ldaxr")),
       ("store", ("str", "stur", "stp", "strb", "strh", "stlr", "stxr", "stlxr")),
       ("branch", ("b", "bl", "blr", "br", "ret", "cbz", "cbnz", "tbz", "tbnz")),
       ("cmp", ("cmp", "cmn", "tst", "ccmp", "fcmp")), ("barrier", ("dmb", "dsb", "isb"))]
def cls(m):
    b = m.split(".")[0]
    for name, ms in CLS:
        if b in ms: return name
    return "fp/simd" if m.startswith("f") or m.startswith("s") and m in ("scvtf",) else "alu"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace"); ap.add_argument("jitdump")
    ap.add_argument("--from-ms", type=float); ap.add_argument("--to-ms", type=float)
    ap.add_argument("--thread"); ap.add_argument("--func"); ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--ctx", type=int, default=3)
    a = ap.parse_args()
    base = xcprof.start_epoch_ms(a.trace)
    lo = (a.from_ms - base) * 1e6 if a.from_ms is not None else None
    hi = (a.to_ms - base) * 1e6 if a.to_ms is not None else None
    th, pcs = leaf_samples(a.trace, lo, hi, a.thread)
    idx = JitIndex(a.jitdump)
    BIG = 1 << 62
    byrec = collections.Counter(); unres = 0
    hits = collections.defaultdict(collections.Counter)
    for pc in pcs:
        r = idx.at(pc, BIG)
        if r < 0: unres += 1; continue
        k = idx.key(r); byrec[k] += 1; hits[k][pc - idx.start[r]] += 1
    rec_of = {}
    for r in range(idx.n): rec_of[idx.key(r)] = r
    n = len(pcs)
    print(f"{th}: {n} samples, {unres} outside JIT code ({unres / max(n, 1):.1%})")
    if not a.func:
        agg = collections.Counter()
        for k, c in byrec.items(): agg[idx.name(rec_of[k])] += c
        for name, c in agg.most_common(a.top): print(f"  {c / n:6.1%}  {name[:120]}")
        return
    cand = [(c, k) for k, c in byrec.items() if a.func in idx.name(rec_of[k])]
    if not cand: print("no sampled code matches", a.func); return
    c, k = max(cand); r = rec_of[k]
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True   # V8 embeds constant pools and jump tables in arm64 code; decoding must not stop there
    code = idx.code(r); h = hits[k]
    print(f"{idx.name(r)}: {len(code)} B, {c} samples ({c / n:.1%} of the thread)")
    ins = list(md.disasm(code, 0))
    hot = {i for i, x in enumerate(ins) if h.get(x.address, 0)}
    show = set()
    for i in hot: show.update(range(max(0, i - a.ctx), min(len(ins), i + a.ctx + 1)))
    last = -2; bycls = collections.Counter()
    for i, x in enumerate(ins):
        s = h.get(x.address, 0); bycls[cls(x.mnemonic)] += s
        if i not in show: continue
        if i != last + 1: print("   ...")
        print(f"  {s:5d} {s / c:6.1%}  {x.address:6x}  {x.mnemonic:8s} {x.op_str}")
        last = i
    print("by class:", ", ".join(f"{k} {v / c:.0%}" for k, v in bycls.most_common()))

if __name__ == "__main__":
    main()
