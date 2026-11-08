#!/usr/bin/env python3
"""servertid2.py <rundir> -- servertid.py's rule (the tid that runs the tick scheduler, not realize_glenv) on an LBR
capture, which perfraw refuses. Counts in-game instruction samples per tid, then names the top tids' JIT symbols."""
import bisect, collections, glob, json, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread
from perfraw import phase_window, jitdump_ranges
d = sys.argv[1]
t0, t1 = phase_window(json.load(open(os.path.join(d, 'phases.json'))), 'ingame')
jit = max(glob.glob(os.path.join(d, 'jit-*.dump')), key=os.path.getsize)
rows = sorted(jitdump_ranges(jit)); st = [r[0] for r in rows]
# The same names as native/servertid.py: 1.16.1's intermediary names and 26.x's Mojang names (26.x ships unobfuscated).
# Without the 26.x ones no thread matched on 262n and the busiest thread won -- the renderer's MAIN thread (tid == pid,
# wink's GL), which read as 212 M instructions a tick, all non-JIT (2026-11-07). The CLIENT side keeps the pre-wink GL
# names (realize_glenv, gl4es) for older captures.
SERVER = re.compile(r"ServerTickScheduler|net\.minecraft\.class_3218|selectTicks|class_1937:method_8398"
                    r"|net\.minecraft\.server\.level\.ServerLevel:|net\.minecraft\.world\.ticks\.LevelTicks:|ServerChunkCache:")
CLIENT = re.compile(r"realize_glenv|gl4es|wink_present|gex_wgpu|class_761|WorldRenderer"
                    r"|net\.minecraft\.client\.renderer\.LevelRenderer:")
at = perfread.attrs(os.path.join(d, 'perf.data'))
ia = {i for i, x in enumerate(at) if 'instructions' in x['name']}
cnt = collections.Counter(); sc = collections.Counter(); cc = collections.Counter()
for ai, ip, t, tid, ch, lbr in perfread.samples(os.path.join(d, 'perf.data'), t0=t0, t1=t1, want_chain=False, want_lbr=False):
    if ai not in ia: continue
    cnt[tid] += 1
    k = bisect.bisect_right(st, ip) - 1
    if k >= 0 and rows[k][0] <= ip < rows[k][1]:
        nm = rows[k][2].decode('utf8', 'replace')
        if SERVER.search(nm): sc[tid] += 1
        if CLIENT.search(nm): cc[tid] += 1
for tid, n in cnt.most_common(4):
    print(tid, n, 'server-sym', sc[tid], 'client-sym', cc[tid])
best = max(cnt.most_common(4), key=lambda x: sc[x[0]] - cc[x[0]])[0]
print(best)
