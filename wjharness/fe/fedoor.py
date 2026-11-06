#!/usr/bin/env python3
"""fedoor.py <capdir> [--event fe_l1i_miss] [--top 30] [--callers N] [--tick-only | --part P] -- the DOORS: for every front-end miss sample of the server thread that
lands OUTSIDE our JIT tier, the function our code called to get there (the frame directly below the first `ours` frame in the
sample's call chain). Ranks the exits from generated code by the misses they cause. Samples whose chain reaches no `ours`
frame are counted apart (an excursion not entered from our code, or a chain broken in native code without frame pointers).
Native frames are named with PERFRAW_DSO_SYMS=1 (set here).
--part keeps one of feclass.PARTS ('tick work', 'tasks', 'idle wait', 'other'); --tick-only = --part 'tick work' (a sample
whose chain reaches MinecraftServer.tick). Before 2026-11-05 --tick-only dropped only samples under waitForTasks /
parkNanos, which kept the wait loop's clock reads as tick work (feclass.py)."""
import collections, os, sys
os.environ["PERFRAW_DSO_SYMS"] = "1"
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfraw, hotinsn, subprocess, bisect

a = sys.argv[1:]
opt = lambda n, dflt=None: a[a.index(n) + 1] if n in a else dflt
d = a[0]
EV = opt("--event", "fe_l1i_miss")
TOP = int(opt("--top", "30"))
import re
from feclass import classify
PART = "tick work" if "--tick-only" in a else opt("--part")
r = subprocess.run([sys.executable, paths.tool("servertid.py"), d], capture_output=True, text=True)
stid = int(r.stdout.strip().splitlines()[-1])
cap, syms, ph = perfraw.open_capture(d, want_chains=True, event=EV)
t0, t1 = perfraw.phase_window(ph, "ingame")
dumps = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f)) for f in os.listdir(d) if f.startswith("jit-") and f.endswith(".dump"))
ivs = hotinsn.load_jitdump(dumps[-1][1])
ist = [x[0] for x in ivs]


def name(ip):
    i = bisect.bisect_right(ist, ip) - 1
    if i >= 0 and ip < ivs[i][1]:
        nm = ivs[i][2]
        return hotinsn.kind_of(nm), nm
    n = syms.lookup(ip)
    if n is None:
        return "ctx", "?"
    s = n.decode("utf8", "replace")
    if s.startswith("DSO:"):
        p, _, f = s[4:].partition("!")
        return "native", os.path.basename(p) + "!" + (f[:100] or "?")
    return "unknown", s[:100]


doors, leafreg, nochain = collections.Counter(), collections.Counter(), collections.Counter()
callers = collections.defaultdict(collections.Counter)   # door -> the `ours` function that called it
tot = out = 0
for tid, ip, t, chain in cap.window(t0, t1):
    if tid != stid:
        continue
    if PART and classify([name(x)[1] for x in [ip] + [y for y in (chain or [])[1:] if y < perfraw.Symbols.CTX_FLOOR]]) != PART:
        continue
    tot += 1
    k, nm = name(ip)
    if k == "ours":
        continue
    out += 1
    leafreg[k] += 1
    frames = [ip] + [x for x in (chain or [])[1:] if x < perfraw.Symbols.CTX_FLOOR]
    prev = (k, nm)
    door = None
    for x in frames[1:]:
        kx, nx = name(x)
        if kx == "ours":
            door = prev
            callers[door[0] + " :: " + door[1]][nx] += 1
            break
        if kx != "ctx":
            prev = (kx, nx)
    if door is None:
        nochain[k + " :: " + nm] += 1
    else:
        doors[door[0] + " :: " + door[1]] += 1

print(f"{d}: server tid {stid}{' (' + PART + ' only)' if PART else ''}; {tot} {EV} samples, {out} ({100 * out / max(tot, 1):.1f}%) outside our tier")
print(f"  of those: {sum(doors.values())} reached through a call from our code, {sum(nochain.values())} with no `ours` frame in the chain")
print(f"\ntop {TOP} doors (the function our code called), share of ALL {EV} samples:")
for k, v in doors.most_common(TOP):
    print(f"  {100 * v / tot:6.2f}%  {k[:150]}")
NC = int(opt("--callers", "0"))
if NC:
    for k, v in doors.most_common(NC):
        print(f"\n  callers of {k[:120]} ({100 * v / tot:.2f}%):")
        for c, n in callers[k].most_common(6):
            print(f"      {100 * n / v:5.1f}%  {c[:140]}")
print(f"\ntop 12 leaves of samples with no `ours` caller:")
for k, v in nochain.most_common(12):
    print(f"  {100 * v / tot:6.2f}%  {k[:150]}")
