#!/usr/bin/env python3
"""fesites.py <capdir> [--event fe_l1i_miss] [--cycles cycles] [--phase ingame] [--top 25] -- WHERE the front end misses: the precise
FRONTEND_RETIRED.* samples (PEBS: the instruction that arrived late) of the SERVER thread, by code region and by position
inside the function, next to the same thread's cycles samples from the same capture. Step 2 of j2d/fe (R525).

Capture: mcperf --jitdump with MCPERF_EVENT = cycles + ONE frontend_retired variant (they share MSR_PEBS_FRONTEND). Every
variant has the raw encoding hwcensus names fe_l1i_miss, so --event fe_l1i_miss selects whichever the run sampled.

Regions: ours (this JIT's modules), aot (dotnet.native.wasm's functions: corlib / IKVM AOT, runtime C), v8 (Builtin: /
Stub: entries in the jitdump), jumptable (an address in no jitdump function within 64 KB BEFORE a module's first function:
V8's per-code-space jump / far-jump tables), native:<dso> (chromium, libc, ...), unknown.
Positions (wasm functions): entry (offset 0), prologue (1..0x40), return site (the previous instruction is a call), branch
target (a jump in the same function lands there), straight (anything else -- fetched in sequence after a miss that began
mid-block, or a target this function's code does not name).
Footprint: distinct 4 KiB pages and 64 B lines among the samples -- a LOWER bound on the touched set, weighted toward hot code."""
import bisect, collections, os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfraw, hotinsn

a = sys.argv[1:]
opt = lambda n, d=None: a[a.index(n) + 1] if n in a else d
d = a[0]
EV = opt("--event", "fe_l1i_miss")
TOP = int(opt("--top", "25"))
r = subprocess.run([sys.executable, paths.tool("servertid.py"), d], capture_output=True, text=True)
stid = int(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() and r.stdout.strip().splitlines()[-1].isdigit() else None
if stid is None:
    sys.exit(f"no server tid: {r.stdout}{r.stderr}")

dumps = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f)) for f in os.listdir(d) if f.startswith("jit-") and f.endswith(".dump"))
ivs = hotinsn.load_jitdump(dumps[-1][1])
ist = [x[0] for x in ivs]
fh = open(dumps[-1][1], "rb")
# per code space, the lowest function start: a jump table sits just below it
starts_sorted = ist


def region(ip, syms):
    i = bisect.bisect_right(ist, ip) - 1
    if i >= 0 and ip < ivs[i][1]:
        return hotinsn.kind_of(ivs[i][2]), i
    j = bisect.bisect_right(ist, ip)
    if j < len(ivs) and ivs[j][0] - ip < 65536 and (i < 0 or ip - ivs[i][1] > 0):
        return "jumptable", None
    n = syms.lookup(ip)
    if n and n.startswith(b"DSO:"):
        p = n[4:].decode("utf8", "replace").split("!")[0]
        return "native:" + os.path.basename(p), None
    return "unknown", None


_dis = {}
def positions(i):
    if i in _dis:
        return _dis[i]
    ca, ce, nm, foff, cs = ivs[i]
    ins = hotinsn.disasm(fh, foff, cs)
    offs = [x[0] for x in ins]
    tgt = set()
    callret = set()
    for k, (o, mn, ops) in enumerate(ins):
        if mn.startswith("j"):
            m = re.match(r"^(0x)?([0-9a-f]+)\b", ops.strip())
            if m:
                tgt.add(int(m.group(2), 16))
        if mn.startswith("call") and k + 1 < len(ins):
            callret.add(ins[k + 1][0])
    _dis[i] = (offs, tgt, callret)
    return _dis[i]


def pos_of(ip, i):
    off = ip - ivs[i][0]
    if off == 0:
        return "entry"
    if off <= 0x40:
        return "prologue"
    offs, tgt, callret = positions(i)
    if off in callret:
        return "return site"
    if off in tgt:
        return "branch target"
    return "straight"


def load(ev):
    cap, syms, phases = perfraw.open_capture(d, want_chains=False, event=ev)
    t0, t1 = perfraw.phase_window(phases, opt("--phase", "ingame"))
    return [ip for tid, ip, t, _ in cap.window(t0, t1) if tid == stid], syms


miss, syms = load(EV)
cyc, _ = load(opt("--cycles", "cycles"))
print(f"{d}: server tid {stid}; {len(miss)} {EV} samples, {len(cyc)} cycles samples in the window")

def tally(ips):
    reg, posc, fn, pages, lines = collections.Counter(), collections.Counter(), collections.Counter(), collections.defaultdict(set), collections.defaultdict(set)
    for ip in ips:
        rg, i = region(ip, syms)
        reg[rg] += 1
        pages[rg].add(ip >> 12)
        lines[rg].add(ip >> 6)
        if i is not None:
            fn[i] += 1
    return reg, fn, pages, lines

mr, mfn, mpg, mln = tally(miss)
cr, cfn, cpg, cln = tally(cyc)
nm, nc = max(len(miss), 1), max(len(cyc), 1)
print(f"\n{'region':28s} {'miss %':>8s} {'cycles %':>9s} {'density':>8s} {'pages(miss)':>12s} {'pages(cyc)':>11s} {'lines(cyc)':>11s}")
for rg in sorted(set(mr) | set(cr), key=lambda k: -mr[k]):
    m, c = 100 * mr[rg] / nm, 100 * cr[rg] / nc
    print(f"{rg:28s} {m:8.2f} {c:9.2f} {m / c if c else float('nan'):8.2f} {len(mpg[rg]):12d} {len(cpg[rg]):11d} {len(cln[rg]):11d}")

pc = collections.Counter()
for ip in miss:
    rg, i = region(ip, syms)
    if i is not None and rg in ("ours", "aot"):
        pc[(rg, pos_of(ip, i))] += 1
print("\nposition of the late instruction, wasm functions (share of ALL miss samples):")
for (rg, p), n in sorted(pc.items(), key=lambda kv: -kv[1]):
    print(f"  {rg:5s} {p:14s} {100 * n / nm:7.2f}%")

print(f"\ntop {TOP} functions by miss samples (miss %, cycles %, region):")
for i, n in mfn.most_common(TOP):
    print(f"  {100 * n / nm:6.2f}  {100 * cfn[i] / nc:6.2f}  {hotinsn.kind_of(ivs[i][2]):5s} {ivs[i][2][:110]}")
conc = sorted(mfn.values(), reverse=True)
for k in (10, 100, 1000):
    print(f"top {k} functions hold {100 * sum(conc[:k]) / nm:.1f}% of miss samples ({len(conc)} functions missed)")
