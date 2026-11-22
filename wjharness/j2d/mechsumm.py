#!/usr/bin/env python3
"""mechsumm.py <census game.log>... -- per mctick census run (`--dumps`, window = 4000 ticks between the dumps at the
window's edges; `wj j2d/mechsumm`):
the image prefix's outcomes a tick ([wasm-jit img-rt], cumulative: last minus first dump), and calls a tick (last
SITE_COUNT=2 dump, every form) into the helpers plan items C1/C2/C3/C5 target."""
import sys, re, collections
T = 4000.0
HELPERS = ['GetTypeWrapperFromObject', 'getClassFromObject', 'CompareAndSwap', 'VolatileRead', 'VolatileWrite',
           'ves_icall_array_new_specific', 'arraycopy', 'currentThread', 'IsInstanceOf', 'isOwnConstant', 'copyOfSameType']
for log in sys.argv[1:]:
    rt = []
    sites = collections.defaultdict(list)
    for line in open(log, errors='replace'):
        m = re.search(r'\[wasm-jit img-rt\] entered=(\d+) hit=(\d+) no_array=(\d+) no_entry=(\d+) not_live=(\d+)', line)
        if m:
            rt.append([int(x) for x in m.groups()])
        i = line.find('WASM_JIT_SITE\t')
        if i >= 0:
            p = line[i:].rstrip('\n').split('\t')
            if len(p) >= 18 and p[1].isdigit():
                sites[int(p[1])].append(p)
    print(f'== {log}')
    if len(rt) >= 2:
        d = [b - a for a, b in zip(rt[0], rt[-1])]
        e = d[0] or 1
        print(f'  image prefix a tick: entered {d[0]/T:.0f}  hit {d[1]/T:.0f} ({100*d[1]/e:.1f}%)  no_array {d[2]/T:.0f} ({100*d[2]/e:.1f}%)  '
              f'no_entry {d[3]/T:.0f} ({100*d[3]/e:.1f}%)  not_live {d[4]/T:.0f}  | sum check {d[1]+d[2]+d[3]+d[4]-d[0]:+d}')
    else:
        print(f'  image prefix: {len(rt)} img-rt line(s)')
    if sites:
        seq = max(sites)
        cal = collections.Counter()
        for p in sites[seq]:
            n = sum(int(x) for x in p[17].split(',')[6:12])
            cal[p[10].split(' (')[0]] += n
        tot = sum(cal.values())
        print(f'  calls a tick (all forms, last dump): {tot/T/1e3:.1f} k')
        for h in HELPERS:
            hits = [(k, v) for k, v in cal.items() if h in k]
            s = sum(v for _, v in hits)
            if s:
                top = sorted(hits, key=lambda kv: -kv[1])[:2]
                print(f'    {h:30s} {s/T/1e3:6.2f} k   ' + '; '.join(f'{k[-60:]} {v/T/1e3:.2f}k' for k, v in top))
