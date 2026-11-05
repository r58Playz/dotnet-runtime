#!/usr/bin/env python3
"""j2d/report.py <batchdir> [--out FILE.md] -- the three-way jbox2d comparison from one j2d/batch.sh batch.

Reads every <arm>-<set><round> run: window.json, counts (j2d/counts.py), and for set A the per-run ledger
(j2d/wledger.py for ours/teavm, j2d/hsledger.py for hs8/hs21). Every number is PER PHYSICS STEP over the same
steps (batches 2..12 = steps 801..3000 of the same deterministic scene) on the bench thread. Counts: median over
runs with the min-max range; ledger shares: the mean over runs of each run's per-step values."""
import collections, csv, glob, json, os, re, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
from counts import run_counts
import hotinsn

a = sys.argv[1:]
B = a[0]
outf = a[a.index('--out') + 1] if '--out' in a else os.path.join(B, 'REPORT.md')
ARMS = ['ours', 'teavm', 'tvfull60', 'tvsimple60', 'hs21', 'hs8']
LABEL = {'ours': 'ours (wasm JIT)', 'teavm': 'TeaVM (wasm, linear mem)', 'tvfull60': 'TeaVM FULL', 'tvsimple60': 'TeaVM SIMPLE (passes off)',
         'hs21': 'HotSpot JDK 21', 'hs8': 'HotSpot JDK 8u504'}

runs = collections.defaultdict(list)
for d in sorted(glob.glob(os.path.join(B, '*-[AB][0-9]*'))):
    if not os.path.isdir(d):
        continue
    m = re.match(r'(ours|teavm|tvfull60|tvsimple60|hs21|hs8)-([AB])(\d+)$', os.path.basename(d))
    if not m:
        continue
    c = run_counts(d)
    if c.get('ok'):
        runs[m.group(1)].append((m.group(2), d, c))

L = []
def p(s=''): L.append(s)
def med(xs): return statistics.median(xs) if xs else float('nan')
def rng(xs): return f'{min(xs):.3g}-{max(xs):.3g}' if xs else ''

# ---------------------------------------------------------------- 1. timing and counts
p(f'# jbox2d three-way comparison -- {os.path.basename(os.path.normpath(B))}')
p()
p("Per physics step, bench thread only, the window of each run's window.json (default: steps 801-3000, batches 2..12; "
  'the steady-state batches: steps 8,601-12,600, batches 41..60). Median over runs (range). '
  'All arms pinned to CPUs 0-7. Counts are PEBS/counter samples x fixed period inside the window; every event is held '
  'under the per-tick throttle (counts.py reports 0 in-window throttles or the row says so).')
p()
MET = [('ms/step (bench clock)', lambda c: c['ms_per_step'], 'AB', 1),
       ('instructions', lambda c: c['per_step']['instructions'], 'AB', 1e6),
       ('cycles', lambda c: c['per_step']['cycles'], 'AB', 1e6),
       ('IPC', lambda c: c['per_step']['instructions'] / c['per_step']['cycles'], 'AB', 1),
       ('GHz (cycles / bench time)', lambda c: c['ghz'], 'AB', 1),
       ('near calls', lambda c: c['per_step'].get('near_call'), 'A', 1e3),
       ('indirect branches', lambda c: c['per_step'].get('indirect_br'), 'B', 1e3),
       ('loads', lambda c: c['per_step'].get('loads'), 'A', 1e6),
       ('stores', lambda c: c['per_step'].get('stores'), 'B', 1e6),
       ('L1 load misses', lambda c: c['per_step'].get('l1_miss'), 'A', 1e3),
       ('L2 load misses', lambda c: c['per_step'].get('l2_miss'), 'A', 1e3),
       ('L3 load misses', lambda c: c['per_step'].get('l3_miss'), 'A', 1e3),
       ('dTLB walk cycles', lambda c: c['per_step'].get('dtlb_walk_cycles'), 'B', 1e3),
       ('dTLB walks completed', lambda c: c['per_step'].get('dtlb_walks'), 'B', 1e3),
       ('icache data stall cycles', lambda c: c['per_step'].get('icache_stall_cycles'), 'B', 1e3)]
unit = {1: '', 1e3: ' (k)', 1e6: ' (M)'}
arms = [x for x in ARMS if runs.get(x)]
p('| per step | ' + ' | '.join(LABEL[x] for x in arms) + ' | ours / TeaVM | ours / JDK 21 |')
p('|---|' + '---|' * (len(arms) + 2))
medv = {}
for name, f, sets, sc in MET:
    cells = []
    for x in arms:
        vs = [f(c) for s, d, c in runs[x] if s in sets and f(c) is not None]
        medv[(name, x)] = med(vs) if vs else None
        cells.append(f'{med(vs)/sc:.3g} ({rng([v/sc for v in vs])}) n={len(vs)}' if vs else '--')
    r1 = medv.get((name, 'ours')); r2 = medv.get((name, 'teavm')) or medv.get((name, 'tvfull60')); r3 = medv.get((name, 'hs21'))
    p(f'| {name}{unit[sc]} | ' + ' | '.join(cells) + f" | {r1/r2:.2f}x | {r1/r3:.2f}x |" if r1 and r2 and r3 else
      f'| {name}{unit[sc]} | ' + ' | '.join(cells) + ' | | |')
thr = {x: sum(sum(c['throttles'].values()) for s, d, c in runs[x]) for x in arms}
p()
p('In-window throttles of the bench thread, summed over runs: ' + ', '.join(f'{x} {thr[x]}' for x in arms) + '.')
cks = {x: sorted({c['checksum'] for s, d, c in runs[x] if c['checksum']}) for x in arms}
p('Checksums: ' + '; '.join(f'{x} {",".join(cks[x]) or "(stopped at batch 12: teavm-baseline is a 600-batch build)"}' for x in arms) + '.')
p()

# ---------------------------------------------------------------- ledgers
def ledger_rows(x, d):
    fn = os.path.join(d, 'hsledger-full.tsv' if x.startswith('hs') else 'wledger-full.tsv')
    if not os.path.exists(fn):
        return None
    return list(csv.DictReader(open(fn), delimiter='\t'))
led = {x: [r for r in (ledger_rows(x, d) for s, d, c in runs[x] if s == 'A') if r] for x in arms}
def agg(x, key, ev='cyc_per_step', filt=None):
    """mean over runs of the per-step sum of `ev` grouped by key(row)"""
    n = len(led[x]) or 1
    out = collections.Counter()
    for rows in led[x]:
        for r in rows:
            if filt and not filt(r):
                continue
            out[key(r)] += float(r.get(ev) or 0) / n
    return out

def kind_group(x, r):
    k = r['kind']
    if x.startswith('hs'):
        if k in ('hs:body', 'hs:entry', 'hs:stub'): return 'compiled program code'
        if k.startswith('hs:blob'): return 'VM stubs / interpreter'
        return 'VM runtime C++ (' + (r['fn'] or '?') + ')'
    if k in ('module', 'module-unmatched'): return 'compiled program code'
    if k == 'runtime-wasm': return 'AOT library + mono runtime (dotnet.native.wasm)' if x == 'ours' else 'runtime wasm'
    if k == 'jiterp': return 'jiterpreter traces (interpreted)'
    if k == 'liftoff': return 'Liftoff (baseline tier)'
    return 'native (V8 builtins, chromium)'

p('## 2. Where the cycles go, by code kind (M cycles / M instructions per step)')
p()
p(f'Ledger runs (set A): ' + ', '.join(f'{x} {len(led[x])}' for x in arms) + '.')
p()
for x in arms:
    if not led[x]:
        continue
    cy = agg(x, lambda r: kind_group(x, r)); ins = agg(x, lambda r: kind_group(x, r), 'ins_per_step')
    tot = sum(cy.values())
    p(f'**{LABEL[x]}**')
    p()
    p('| code | cycles | instr | share of cycles |')
    p('|---|---|---|---|')
    for k, v in cy.most_common():
        p(f'| {k} | {v/1e6:.3f} | {ins[k]/1e6:.3f} | {v/tot:.1%} |')
    p()

# ---------------------------------------------------------------- 3. x86 classes
p('## 3. x86 instruction classes, all arms (M per step) -- the common denominator')
p()
p('hotinsn.classify on the sampled instruction itself (live code bytes; HotSpot from the agent\'s copy). '
  'Rows with no code bytes (native / unmapped C++) are "unclassified".')
p()
grp_of = {}
for g, cl in hotinsn.GROUPS.items():
    for c_ in cl:
        grp_of[c_] = g
def xgroup(r):
    x86 = r['x86'] or ''
    if not x86 or x86 == 'x86?':
        return 'unclassified'
    return grp_of.get(x86, 'OTHER')
p('| group | ' + ' | '.join(f'{x} instr | {x} cyc' for x in arms if led[x]) + ' |')
p('|---|' + '---|---|' * len([x for x in arms if led[x]]))
gi = {x: agg(x, xgroup, 'ins_per_step') for x in arms if led[x]}
gc = {x: agg(x, xgroup) for x in arms if led[x]}
for g in list(hotinsn.GROUPS) + ['OTHER', 'unclassified']:
    p(f'| {g} | ' + ' | '.join(f'{gi[x][g]/1e6:.3f} | {gc[x][g]/1e6:.3f}' for x in arms if led[x]) + ' |')
p()
p('By class (instructions per step, M):')
p()
ci = {x: agg(x, lambda r: r['x86'] or 'unclassified', 'ins_per_step') for x in arms if led[x]}
cc = {x: agg(x, lambda r: r['x86'] or 'unclassified') for x in arms if led[x]}
allc = set()
for x in ci: allc |= set(ci[x])
p('| class | ' + ' | '.join(f'{x} instr | {x} cyc' for x in ci) + ' |')
p('|---|' + '---|---|' * len(ci))
for c_ in sorted(allc, key=lambda c_: -ci.get('ours', {}).get(c_, 0)):
    if max(ci[x].get(c_, 0) for x in ci) < 20e3:
        continue
    p(f'| {c_} | ' + ' | '.join(f'{ci[x].get(c_, 0)/1e6:.3f} | {cc[x].get(c_, 0)/1e6:.3f}' for x in ci) + ' |')
p()

# ---------------------------------------------------------------- 4. ours by construct, wasm op
if led.get('ours'):
    p('## 4. Ours, JIT-tier code by emitter construct and by wasm opcode class (M per step)')
    p()
    isjit = lambda r: r['kind'] == 'module'
    for title, keyf in (('emitter construct (wj.origin tag)', lambda r: r['tag'] or '?'), ('wasm opcode class', lambda r: r['op'] or '?')):
        cy = agg('ours', keyf, filt=isjit); ins = agg('ours', keyf, 'ins_per_step', filt=isjit)
        l1 = agg('ours', keyf, 'l1_miss_per_step', filt=isjit)
        tot = sum(cy.values())
        p(f'| {title} | cycles | instr | L1 miss (k) | share of JIT cycles |')
        p('|---|---|---|---|---|')
        for k, v in cy.most_common(30):
            p(f'| {k} | {v/1e6:.3f} | {ins[k]/1e6:.3f} | {l1[k]/1e3:.2f} | {v/tot:.1%} |')
        p()

# ---------------------------------------------------------------- 5. per Java method
def norm(x, r):
    if x.startswith('hs'):
        n = r['tag'] if r['kind'] in ('hs:body', 'hs:entry', 'hs:stub') else ''
        m = re.match(r'^(?:.*\.)?([^.]+)::(.+)$', n)
        return f'{m.group(1)}.{m.group(2)}' if m else ''
    n = r['fn'] or ''
    if x.startswith('t'):
        m = re.match(r'^meth_[a-z]+_([A-Za-z0-9$]+)_(.+)$', n)
        return f'{m.group(1)}.{m.group(2)}' if m else ''
    m = re.match(r'^.*? ([\w.$/]+):([^ ]+) \(', n)
    if m:
        return m.group(1).split('.')[-1].split('/')[-1] + '.' + m.group(2)
    m = re.match(r'^(?:IKVM_Java_|IKVM_Runtime_IKVM_Runtime_|corlib_)(.*)$', n)
    if m:
        toks = [t for t in m.group(1).replace('___', '__').split('_')]
        for i, t in enumerate(toks):
            if t[:1].isupper():
                cls = t
                j = i + 1
                if j < len(toks) and toks[j] == '' and j + 1 < len(toks):   # Comparable__Helper
                    cls = t + '__' + toks[j + 1]; j += 2
                return f'{cls}.{toks[j] if j < len(toks) else "?"}'
    return ''
p('## 5. Per Java method, at the ROOT of each compiled unit (M cycles per step)')
p()
p('A wasm function and a HotSpot nmethod are both a root with its inlinees folded in, so rows compare like with like. '
  'TeaVM\'s class library has its own Arrays.sort (Arrays.merge); ours and HotSpot run the JDK\'s ComparableTimSort.')
p()
mc = {x: agg(x, lambda r, x=x: norm(x, r)) for x in arms if led[x]}
mi = {x: agg(x, lambda r, x=x: norm(x, r), 'ins_per_step') for x in arms if led[x]}
keys = [k for k, _ in (mc.get('ours') or collections.Counter()).most_common(40) if k]
p('| method | ' + ' | '.join(f'{x} cyc | {x} ins' for x in mc) + ' |')
p('|---|' + '---|---|' * len(mc))
for k in keys[:30]:
    p(f'| {k} | ' + ' | '.join(f'{mc[x].get(k, 0)/1e6:.3f} | {mi[x].get(k, 0)/1e6:.3f}' for x in mc) + ' |')
p()
for x in mc:
    top = [k for k, _ in mc[x].most_common(12) if k]
    p(f'Top {x}: ' + ', '.join(f'{k} {mc[x][k]/1e6:.3f}' for k in top))
    p()

open(outf, 'w').write('\n'.join(L) + '\n')
print('\n'.join(L))
