#!/usr/bin/env python3
"""armsumm.py <rundir> [--since <n>] -- per-arm mean (min-max) of the j2d/counts one-liners (<arm>-<n>.counts) in a run
directory: ms/tick, instructions, cycles, IPC, L1/L2/L3 load misses, near calls, loads, and each arm against `base`.
--since n: only runs numbered >= n (a later batch in the same directory). `wj j2d/armsumm`."""
import re, sys, glob, os, statistics as st
d = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else os.path.dirname(os.path.abspath(__file__))
since = int(sys.argv[sys.argv.index('--since') + 1]) if '--since' in sys.argv else 0
pat = {
    'ms': r'([\d.]+) ms/step', 'instr': r'instr ([\d.]+) M', 'cyc': r'cyc ([\d.]+) M', 'ipc': r'IPC ([\d.]+)',
    'l1k': r'l1_miss=([\d.]+)k', 'l2k': r'l2_miss=([\d.]+)k', 'l3k': r'l3_miss=([\d.]+)k',
    'callk': r'near_call=([\d.]+)k', 'loadk': r'loads=([\d.]+)k',
}
arms = {}
for f in sorted(glob.glob(os.path.join(d, '*.counts'))):
    m = re.match(r'(.+)-(\d+)\.counts$', os.path.basename(f))
    if not m or int(m.group(2)) < since:
        continue
    txt = open(f).read()
    row = {}
    for k, p in pat.items():
        mm = re.search(p, txt)
        if mm:
            row[k] = float(mm.group(1))
    if 'ms' in row:
        arms.setdefault(m.group(1), []).append((int(m.group(2)), row))
base = None
for arm in sorted(arms, key=lambda a: (a != 'base', a)):
    rows = [r for _, r in sorted(arms[arm])]
    out = [f'{arm:6s} n={len(rows)} runs={",".join(str(n) for n, _ in sorted(arms[arm]))}']
    means = {}
    for k in pat:
        v = [r[k] for r in rows if k in r]
        if not v:
            continue
        means[k] = st.mean(v)
        out.append(f'{k} {means[k]:.3f} ({min(v):.3f}-{max(v):.3f})')
    if arm == 'base':
        base = means
    elif base:
        out.append('| vs base: ' + ' '.join(f'{k} {100 * (means[k] / base[k] - 1):+.1f}%' for k in ('ms', 'cyc', 'instr', 'l1k', 'l2k', 'callk') if k in means and k in base and base[k]))
    print('  '.join(out))
