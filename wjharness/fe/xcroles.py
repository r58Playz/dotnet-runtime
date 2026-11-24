#!/usr/bin/env python3
"""xcroles.py <trace> <jitdump> [--thread SUBSTR] [--from-ms W --to-ms W] [--top N] [--show FUNC] [--selftest]
-- where one thread's JIT-code samples go BY ROLE of the instruction, on V8's arm64 wasm code (macOS: Time Profiler leaf
PCs through the jitdump, as fe/xchot.py). A static pass over each sampled function's code labels every instruction:

  entry      offset 0 up to the first control-flow instruction that is not the stack check's `b.ls` (V8's frame, the
             stack check, our frame base and the argument pin stores ahead of the first test)
  exit       the straight run that ends in `ret` (back to the nearest branch target)
  call       a direct (`bl`) or indirect (`blr`) call outside the trap tail, the straight run before it (argument setup,
             call_indirect's bounds / signature / target loads), at most 24 instructions back to a branch target
  ret-land   the 2 instructions after a call: where the return lands and a timer sample after a long call skids to
  trap       the out-of-line trap tail (the run of `bl`s at the end) -- should read ~0
  glob-load  a load addressed `[base, xN]` where xN was just built by mov/movk (a constant linear address: S3 CHA words,
             the s.g poll, statics, class records)
  spill      a load / store addressed off sp or x29 (V8's spills and reloads)
  load / store / cmp-br / alu   everything else, by mnemonic

Roles are a CRUDE partition: a `call` run also holds whatever body work sits in the same straight run. Use it to
compare arms and builds, not as an exact ledger. --selftest checks the labels on a synthetic function."""
import sys, os, argparse, collections, importlib.util
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ledger"))
spec = importlib.util.spec_from_file_location("xchot", os.path.join(HERE, "xchot.py")); xchot = importlib.util.module_from_spec(spec); spec.loader.exec_module(xchot)

CBR = ("b.", "cbz", "cbnz", "tbz", "tbnz")
def is_cbr(m): return m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz")
def is_flow(m): return is_cbr(m) or m in ("b", "br", "ret", "bl", "blr")

def target(x):
    try: return int(x.op_str.split("#")[-1], 16)
    except ValueError: return None

def roles(ins):
    """ins: list of (address, mnemonic, op_str). Returns a role per instruction."""
    n = len(ins); r = [None] * n
    # the trap tail: the trailing run of bl (and anything after it)
    t = n
    while t > 0 and ins[t - 1][1] in ("bl", "blr", "brk", "udf", ".byte") or (t > 0 and ins[t - 1][2] == "xzr"):
        t -= 1
    for i in range(t, n): r[i] = "trap"
    tail_start = ins[t][0] if t < n else None
    labels = set()
    for a, m, o in ins[:t]:
        if is_cbr(m) or m == "b":
            try:
                tg = int(o.split("#")[-1], 16)
                if tail_start is None or tg < tail_start: labels.add(tg)
            except ValueError: pass
    # entry
    i = 0
    while i < t:
        a, m, o = ins[i]
        if is_flow(m) and not (m == "b.ls" and i < 16): break
        r[i] = "entry"; i += 1
    addr_ix = {a: k for k, (a, _, _) in enumerate(ins)}
    for k in range(t):
        a, m, o = ins[k]
        if m == "ret":
            j = k
            while j >= 0 and r[j] is None:
                r[j] = "exit"
                if ins[j][0] in labels: break
                j -= 1
        elif m in ("bl", "blr"):
            r[k] = "call"
            for d in (1, 2):
                if k + d < t and r[k + d] is None: r[k + d] = "ret-land"
            j = k - 1
            while j >= 0 and k - j <= 24 and r[j] is None and not is_flow(ins[j][1]):
                r[j] = "call"
                if ins[j][0] in labels: break
                j -= 1
    for k in range(t):
        if r[k] is not None: continue
        a, m, o = ins[k]
        c = xchot.cls(m)
        if c in ("load", "store") and ("[sp" in o or "[x29" in o): r[k] = "spill"
        elif c == "load" and o.count(",") >= 2 and "#" not in o.split("[", 1)[1]:
            idx = o.split("[", 1)[1].split(",")[1].strip(" ]")
            built = False
            for j in range(k - 1, max(-1, k - 5), -1):
                pm, po = ins[j][1], ins[j][2]
                if po.split(",")[0].strip() in (idx, "w" + idx[1:]) and pm in ("mov", "movk", "movz"): built = True; break
            r[k] = "glob-load" if built else "load"
        elif c in ("branch", "cmp"): r[k] = "cmp-br"
        elif c in ("load", "store"): r[k] = c
        else: r[k] = "alu"
    return r

def selftest():
    prog = [(0, "mov", "x16, #0x30"), (4, "stp", "x7, x16, [sp, #-0x20]!"), (8, "ldur", "x1, [x26, #-0xe0]"), (12, "cmp", "sp, x1"),
            (16, "b.ls", "#0x60"), (20, "str", "w0, [x8, #4]"), (24, "cbz", "w0, #0x34"),
            (28, "mov", "x11, #0xcdf4"), (32, "movk", "x11, #0x2e25, lsl #16"), (36, "ldr", "w11, [x1, x11]"),
            (40, "ldr", "w10, [x1, x9]"), (44, "ldr", "x2, [sp, #0x10]"), (48, "blr", "x13"), (52, "and", "w0, w0, #0xff"),
            (56, "add", "w0, w0, #1"), (60, "mov", "x5, x6"), (64, "ldp", "x29, x30, [sp, #0x10]"), (68, "ret", ""),
            (72, "bl", "#0x-100"), (76, "bl", "#0x-100")]
    got = roles(prog)
    want = ["entry"] * 6 + ["cmp-br", "call", "call", "call", "call", "call", "call", "ret-land", "ret-land", "exit", "exit", "exit", "trap", "trap"]
    assert got == want, list(zip([p[1] for p in prog], got, want))
    print("xcroles selftest ok")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace", nargs="?"); ap.add_argument("jitdump", nargs="?")
    ap.add_argument("--from-ms", type=float); ap.add_argument("--to-ms", type=float)
    ap.add_argument("--thread"); ap.add_argument("--top", type=int, default=15); ap.add_argument("--show")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest: return selftest()
    base = xchot.xcprof.start_epoch_ms(a.trace)
    lo = (a.from_ms - base) * 1e6 if a.from_ms is not None else None
    hi = (a.to_ms - base) * 1e6 if a.to_ms is not None else None
    th, pcs = xchot.leaf_samples(a.trace, lo, hi, a.thread)
    idx = xchot.JitIndex(a.jitdump)
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.skipdata = True
    hits = collections.defaultdict(collections.Counter); unres = 0
    for pc in pcs:
        r = idx.at(pc, 1 << 62)
        if r < 0: unres += 1; continue
        hits[r][pc - idx.start[r]] += 1
    tot = collections.Counter(); perfn = collections.defaultdict(collections.Counter); nsamp = 0
    for r, h in hits.items():
        ins = [(x.address, x.mnemonic, x.op_str) for x in md.disasm(idx.code(r), 0)]
        rl = roles(ins); at = {a_: k for k, (a_, _, _) in enumerate(ins)}
        name = idx.name(r)
        for off, c in h.items():
            k = at.get(off)
            role = rl[k] if k is not None else "?"
            tot[role] += c; perfn[name][role] += c; nsamp += c
        if a.show and a.show in name:
            for k, (ad, m, o) in enumerate(ins):
                if h.get(ad): print(f"  {h[ad]:5d} {rl[k]:9s} {ad:6x} {m:8s} {o}")
    n = len(pcs)
    print(f"{th}: {n} samples, {unres} outside JIT code ({unres / max(n, 1):.1%}); JIT samples by role:")
    for k, v in tot.most_common(): print(f"  {k:10s} {v:7d} {v / max(nsamp, 1):6.1%}")
    print(f"top {a.top} functions (share of JIT samples; role shares within the function):")
    for name, c in sorted(perfn.items(), key=lambda kv: -sum(kv[1].values()))[:a.top]:
        s = sum(c.values())
        print(f"  {s / nsamp:5.1%} {name[3:90]}")
        print("        " + "  ".join(f"{k} {v / s:.0%}" for k, v in c.most_common(6)))

if __name__ == "__main__":
    main()
