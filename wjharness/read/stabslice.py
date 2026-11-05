#!/usr/bin/env python3
"""stabslice.py <mcperf/hwrun rundir>... [--slice S] [--wins A-B,C-D,...] [--trace]

Where a Minecraft server tick stabilises, and whether a shorter window would measure the same thing (R418). Reads the
server thread's sampled instructions and cycles (any capture with fixed-period `instructions` / `cycles` events --
hwrun's census set, a mapping run) from world join (phases.json `world`.t1) to the window's end, and prints:
  --trace       instructions per tick in S-second slices from join (default S=10), `B` marking the bench window
  (default)     per window: M instructions and M cycles per tick for the full bench window and each --wins window
                (seconds after join; default 200-260,220-280,160-220,100-160)
Per tick = per second / 20: valid only where the server keeps 20 TPS (hwcensus checks "ticks behind" for the bench
window; a slice during the post-join catch-up reads high for that reason, which is the point of the trace).

Protocol Q60 came from this tool on R417's b1-b4 / t1 t2 / g1 g2 (j2d/r417/): the rate settles ~100 s after join, an
A/B needs a window starting ~200 s after join (160-220 lost the GC probe's +4.6%), and the 6,000-tick autosave lands
~300 s after join. Re-run it on a new batch when the workload changes (a new world, a new mod set) before trusting Q60.
"""
import collections, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
runs = [x for i, x in enumerate(a) if not x.startswith('--') and not (i > 0 and a[i - 1] in ('--slice', '--wins'))]
S = float(opt('--slice', 10))
WINS = [tuple(float(v) for v in w.split('-')) for w in opt('--wins', '200-260,220-280,160-220,100-160').split(',')]


def load(d):
    p = json.load(open(os.path.join(d, 'phases.json')))
    ph = {x['name']: x for x in p['phases']}
    tin, b0, b1 = ph['world']['t1'], ph['ingame']['t0'], ph['ingame']['t1']
    r = subprocess.run([sys.executable, paths.tool("servertid.py"), d], capture_output=True, text=True)
    tid = int(r.stdout.strip().split()[-1])
    at = perfread.attrs(os.path.join(d, 'perf.data'))
    kind = {}
    for i, x in enumerate(at):
        n = x['name']
        if x['freq']:
            continue
        if n.startswith('instructions') or '/instructions' in n:
            kind[i] = ('ins', x['period'])
        elif n.startswith('cycles') or '/cycles' in n:
            kind[i] = ('cyc', x['period'])
    ev = []
    for ai, ip, t, stid, ch, lbr in perfread.samples(os.path.join(d, 'perf.data'), tid=tid, t0=tin, t1=b1,
                                                     want_chain=False, want_lbr=False):
        k = kind.get(ai)
        if k:
            ev.append((t / 1e9 - tin, k[0], k[1]))
    return tin, b0, b1, ev


def rate(ev, lo, hi):
    c = collections.Counter()
    for t, k, per in ev:
        if lo <= t < hi:
            c[k] += per
    return c['ins'] / (hi - lo) / 20 / 1e6, c['cyc'] / (hi - lo) / 20 / 1e6


hdr = False
for d in runs:
    tin, b0, b1, ev = load(d)
    name = os.path.basename(os.path.normpath(d))
    if '--trace' in a:
        n = int((b1 - tin) // S) + 1
        cells = []
        for s in range(n):
            lo = s * S
            ins, _ = rate(ev, lo, lo + S)
            cells.append(f"{int(lo)}{'B' if tin + lo >= b0 else ''}:{ins:.1f}")
        print(f'{name}: join->bench {b0 - tin:.0f}s, bench {b1 - b0:.0f}s; M instr/tick per {S:.0f}s slice')
        print('  ' + ' '.join(cells))
        continue
    if not hdr:
        print(f"{'run':10s} {'full window':>17s} " + ' '.join(f'{f"{int(lo)}-{int(hi)}s":>17s}' for lo, hi in WINS))
        print(f"{'':10s} " + ' '.join(f"{'M ins':>8s} {'M cyc':>8s}" for _ in range(len(WINS) + 1)))
        hdr = True
    full = rate(ev, b0 - tin, b1 - tin)
    print(f'{name:10s} {full[0]:8.2f} {full[1]:8.2f} ' + ' '.join(f'{x[0]:8.2f} {x[1]:8.2f}' for x in (rate(ev, *w) for w in WINS)))
