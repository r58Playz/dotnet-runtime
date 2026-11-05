#!/usr/bin/env python3
"""sampled.py <rundir> --tid N [--window t0,t1] -> prints the JIT function names (jitdump name, '-<idx>-<tier>'
stripped) that the server thread's samples hit, at the IP or anywhere on the callchain. d8x86.py --only takes this
list, so only the sampled part of the tier is compiled offline (a 911 KB module the run never tiered up is what
pushed the full compile into the OOM killer)."""
import collections, glob, json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread
from jitindex import JitIndex
from perfraw import phase_window

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
d = a[0]; tid = int(opt('--tid'))
if opt('--window'):
    t0, t1 = (float(x) for x in opt('--window').split(','))
else:
    t0, t1 = phase_window(json.load(open(os.path.join(d, 'phases.json'))), 'ingame')
idx = JitIndex(max(glob.glob(os.path.join(d, 'jit-*.dump')), key=os.path.getsize))
name_re = re.compile(r'^JS:(.*)-(\d+)-(turbofan|liftoff)$')
hit = collections.Counter()
for ai, ip, t, stid, chain, lbr in perfread.samples(os.path.join(d, 'perf.data'), tid=tid, t0=t0, t1=t1, want_lbr=False):
    for x in (ip,) + tuple(c - 1 for c in chain[1:] if c < perfread.CTX_FLOOR):
        r = idx.at(x, t)
        if r >= 0:
            m = name_re.match(idx.name(r))
            if m:
                hit[m.group(1)] += 1
for nm, _ in hit.most_common():
    print(nm)
print(f'{len(hit)} sampled function names', file=sys.stderr)
