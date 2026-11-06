#!/usr/bin/env python3
"""fepart.py <capdir> [--event fe_l1i_miss] -- split the server thread's samples into tick work, tasks run between ticks,
the idle wait's own machinery, and other (feclass.py has the definitions and the 2026-11-05 correction), for the
front-end event and for cycles, with the region split of the tick-work misses."""
import bisect, collections, os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfraw, hotinsn
from feclass import classify, PARTS

a = sys.argv[1:]
opt = lambda n, dflt=None: a[a.index(n) + 1] if n in a else dflt
d = a[0]
EV = opt("--event", "fe_l1i_miss")
r = subprocess.run([sys.executable, paths.tool("servertid.py"), d], capture_output=True, text=True)
stid = int(r.stdout.strip().splitlines()[-1])
dumps = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f)) for f in os.listdir(d) if f.startswith("jit-") and f.endswith(".dump"))
ivs = hotinsn.load_jitdump(dumps[-1][1])
ist = [x[0] for x in ivs]


def fn(ip):
    i = bisect.bisect_right(ist, ip) - 1
    return ivs[i][2] if i >= 0 and ip < ivs[i][1] else None


def run(ev):
    cap, syms, ph = perfraw.open_capture(d, want_chains=True, event=ev)
    t0, t1 = perfraw.phase_window(ph, "ingame")
    part, reg = collections.Counter(), collections.defaultdict(collections.Counter)
    for tid, ip, t, chain in cap.window(t0, t1):
        if tid != stid:
            continue
        k = classify([fn(x) for x in [ip] + list(chain or [])[1:] if x < perfraw.Symbols.CTX_FLOOR])
        part[k] += 1
        n = fn(ip)
        rg = hotinsn.kind_of(n) if n else ("native" if (syms.lookup(ip) or b"").startswith(b"DSO:") else "other")
        reg[k][rg] += 1
    return part, reg


mp, mr = run(EV)
cp, cr = run("cycles")
mt, ct = max(sum(mp.values()), 1), max(sum(cp.values()), 1)
print(f"{d}: server tid {stid}; {sum(mp.values())} {EV}, {sum(cp.values())} cycles samples")
print(f"{'':12s} {'misses %':>9s} {'cycles %':>9s} {'misses/cycle vs mean':>21s}")
for k in PARTS:
    m, c = 100 * mp[k] / mt, 100 * cp[k] / ct
    print(f"{k:12s} {m:9.2f} {c:9.2f} {m / c if c else float('nan'):21.2f}")
print("\ntick work only, by region (share of tick-work misses / of tick-work cycles):")
tm, tc = max(mp["tick work"], 1), max(cp["tick work"], 1)
for rg in sorted(set(mr["tick work"]) | set(cr["tick work"]), key=lambda x: -mr["tick work"][x]):
    print(f"  {rg:8s} {100 * mr['tick work'][rg] / tm:7.2f} {100 * cr['tick work'][rg] / tc:7.2f}")
