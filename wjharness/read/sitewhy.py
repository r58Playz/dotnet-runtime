#!/usr/bin/env python3
"""sitewhy.py <run dir | game.log> [--tier T] [--top N] [--no-phantoms] -- WHY tier-T virtual/interface calls are still calls.

Reads the MONO_WASM_JIT_SITE_COUNT census (W3 fields appended to each WASM_JIT_SITE line: the guarded-inline verdict
char and the emitter's devirt-arm outcome) from the LAST dump (= the in-game window with worldwait --dumps), and
prints calls/tick by kind (v class-virtual, i interface) x GI verdict x arm outcome, with the top callees per bucket.

GI verdict (method-to-ir wj_site_gi_note): N no record, C cold, P polymorphic, S self, V valuetype, B refused before,
G signature, X census-only, E EH clauses, Z size, O other check_inlining, L refused late, I inlined (the counted call
is the guard's FALLBACK), - not a GI candidate (final/non-virtual target, gshared, delegate, tailcall, ...).
Arm (emitter ic_tag): 0 no attempt, 1 no record, 2 cold, 3 poly, 4 poly>=90%, 5 sig, 6 no f-slot, 7 arm emitted, -1 not
lowered by the virtual branch; delegate sites (R405) 8 arm emitted, 9 no record, 10 thin, 11 refused. The census counter
is bumped BEFORE the call is lowered, so an arm site counts EVERY execution (hits are a direct call, still a call); only
a GI inline removes calls from the count (I = the counted call is the guard's FALLBACK). GI A (R405) = inline_method
aborted, L = over the cost cap. T (MONO_WASM_JIT_INLINE_COLD_CALL) = the call site is in an IL block that ends in a
throw, so it was deliberately not inlined (and charged a flat cost); such sites should carry ~0 calls.
--no-phantoms drops `icall:mono_tls_get_thread_extern` sites: R317's FAST_TLS lowers them to two loads AFTER the census
hook counts them (mini-wasm-emitter.inc), so they are not calls (~46 k "calls"/tick on 2026-11-05).
Promoted from scratchpad/legacy/wj/plan2x (R404-R467) on 2026-11-05.
"""
import collections, glob, os, re, sys

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
src = a[0]
log = src if os.path.isfile(src) else (glob.glob(os.path.join(src, '*.log')) + glob.glob(os.path.join(src, 'game.log')))[0]
tier = opt('--tier', '2')
top = int(opt('--top', 8))

GI = dict(N='no record', C='cold', P='poly', S='self', V='valuetype', B='refused before', G='sig', X='census only',
          E='EH clauses', Z='size', O='other check_inlining', L='refused late (cost)', A='refused late (abort)', J='BIMORPHIC inlined (fallback)',
          D='depth', n='NoInlining', s='S2 never ran at t1', r='does not return', c='class init',
          **{'1': 'inl: no record', '2': 'inl: cold', '3': 'inl: poly'}, I='INLINED (fallback)', T='cold call site (not inlined)', **{'-': 'not a GI candidate'})
ARM = {'0': 'no attempt', '1': 'no record', '2': 'cold', '3': 'poly', '4': 'poly>=90', '5': 'sig', '6': 'no f-slot',
       '7': 'ARM emitted', '8': 'delegate arm', '9': 'delegate no rec',
       '10': 'delegate thin', '11': 'delegate refused', '-1': 'not virtual-lowered'}

recs = collections.defaultdict(list)
end = None; skipped = 0; inside = False
with open(log, errors='replace') as f:
    for line in f:
        k = line.rfind('", source: ')
        if k >= 0:
            line = line[:k]
        if '[mcw] START' in line:
            inside = True
        elif '[mcw] END' in line:
            inside = False
            m = re.search(r'"wallMs":(\d+)', line)
            if m: end = int(m.group(1))
        elif inside and 'ticks behind' in line:
            m = re.search(r'or (\d+) ticks behind', line)
            if m: skipped += int(m.group(1))
        i = line.find('WASM_JIT_SITE\t')
        if i >= 0:
            p = line[i:].rstrip('\n').split('\t')
            if len(p) >= 13:
                recs[int(p[1])].append(dict(n=int(p[2]), tier=p[3], kind=p[4], caller=p[8], inl=p[9], callee=p[10], gi=p[11], arm=p[12]))
if not recs:
    sys.exit(f'{log}: no WASM_JIT_SITE lines with the W3 fields (SITE_COUNT=1, --dumps, a build with R404)')
rows = recs[max(recs)]
if '--no-phantoms' in a:
    rows = [r for r in rows if r['callee'] != 'icall:mono_tls_get_thread_extern']
ticks = (end / 1000.0 * 20 - skipped) if end else None
scale = 1.0 / ticks if ticks else 1.0
unit = 'calls/tick' if ticks else 'calls (window)'
tot_all = sum(r['n'] for r in rows)
print(f'{log}: dump {max(recs)}, {len(rows)} sites, {tot_all * scale:,.0f} {unit} all tiers/kinds' + (f', {ticks:.0f} ticks' if ticks else ''))

by_tk = collections.Counter()
for r in rows:
    by_tk[(r['tier'], r['kind'])] += r['n']
print('by tier/kind:', ', '.join(f't{t}{k} {v * scale:,.0f} ({100 * v / tot_all:.1f}%)' for (t, k), v in by_tk.most_common()))

sel = [r for r in rows if r['tier'] == tier and r['kind'] in ('v', 'i', 'd')]
tot = sum(r['n'] for r in sel)
print(f'\ntier {tier} virtual+interface+direct: {tot * scale:,.0f} {unit} = {100 * tot / tot_all:.1f}% of all calls\n')
for kind in ('i', 'v', 'd'):
    ks = [r for r in sel if r['kind'] == kind]
    kt = sum(r['n'] for r in ks)
    print(f'== kind {kind} ({dict(i="interface", v="class-virtual", d="direct")[kind]}): {kt * scale:,.0f} {unit} ({100 * kt / max(tot_all, 1):.1f}% of all calls)')
    cell = collections.Counter()
    cell_callees = collections.defaultdict(collections.Counter)
    for r in ks:
        key = (r['gi'], r['arm'])
        cell[key] += r['n']
        cell_callees[key][r['callee']] += r['n']
    for (g, arm), v in cell.most_common(14):
        print(f'  {v * scale:10,.0f}  {100 * v / max(kt, 1):5.1f}%  GI {GI.get(g, g):22s} arm {ARM.get(arm, arm):18s}')
        for cn, cv in cell_callees[(g, arm)].most_common(top if v > kt * 0.05 else 0):
            print(f'               {cv * scale:9,.0f}  {cn[:110]}')
    print()
