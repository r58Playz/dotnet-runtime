#!/usr/bin/env python3
"""lbredges.py <rundir> -- the call-edge profile of a wasm arm from a j2d/run set-L run (cycles with the branch stack
filtered to user calls, -j any_call,u; V8 jitdump recorded): every LBR call record in the window, from-IP and to-IP mapped
to the functions holding them, counted per tick. Writes <rundir>/edges.tsv (caller, callee, calls a tick: an estimate,
LBR entries x the cycles period / ticks -- a weight for ranking, not an exact count) and <rundir>/hot.tsv (function,
M cycles a tick). Names are the wasm functions' name-section names (TeaVM's meth_...), V8's suffixes stripped.
For split-teavm.py --mode edges / classedges / hot: profile a layout in which every call is a real call (--mode
function), or V8's inlining hides the edges it inlined."""
import collections, glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'lib')); import paths  # noqa: E402,F401
import perfread  # noqa: E402
from jitindex import JitIndex  # noqa: E402

D = sys.argv[1]
w = json.load(open(os.path.join(D, 'window.json')))
t0, t1 = w['window']; ticks = w['steps']
idx = JitIndex(max(glob.glob(os.path.join(D, 'jit-*.dump')), key=os.path.getsize))
at = perfread.attrs(os.path.join(D, 'perf.data'))
cyc = {i: x['period'] for i, x in enumerate(at) if 'cycles' in x['name']}
SUF = re.compile(r'^JS:|-\d+-(turbofan|liftoff)$')
names = {}
def fn(ip, t):
    r = idx.at(ip, t)
    if r < 0:
        return None
    n = names.get(r)
    if n is None:
        n = names[r] = SUF.sub('', idx.name(r))
    return n
edges = collections.Counter(); heat = collections.Counter(); tids = collections.Counter(); nlbr = 0
for s in perfread.samples(os.path.join(D, 'perf.data'), t0=t0, t1=t1, want_chain=False, want_lbr=True):
    ai, ip, t, tid, _, lbr = s[:6]
    if ai not in cyc:
        continue
    tids[tid] += 1
    heat[(tid, fn(ip, t))] += cyc[ai]
    for e in lbr or ():
        a_, b_ = fn(e[0], t), fn(e[1], t)
        if a_ and b_:
            edges[(tid, a_, b_)] += cyc[ai]; nlbr += 1
tid = tids.most_common(1)[0][0]          # the bench thread: the busiest in the window
with open(os.path.join(D, 'edges.tsv'), 'w') as f:
    for (t_, a_, b_), v in sorted(edges.items(), key=lambda kv: -kv[1]):
        if t_ == tid:
            f.write(f'{a_}\t{b_}\t{v / ticks:.1f}\n')
with open(os.path.join(D, 'hot.tsv'), 'w') as f:
    for (t_, n), v in sorted(heat.items(), key=lambda kv: -kv[1]):
        if t_ == tid and n:
            f.write(f'{n}\t{v / ticks / 1e6:.4f}\n')
ne = sum(1 for k in edges if k[0] == tid)
print(f'{D}: tid {tid}, {tids[tid]} cycles samples, {nlbr} call records mapped, {ne} distinct edges -> edges.tsv, hot.tsv')
