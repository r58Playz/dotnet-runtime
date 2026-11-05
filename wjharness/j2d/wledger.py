#!/usr/bin/env python3
"""j2d/wledger.py -- the jbox2d ledger for a WASM arm (ours or TeaVM): every PEBS sample of the bench thread inside
the window, per step, attributed to

  kind     module (a TurboFan function of a captured module whose live x86 matches d8's layout) | module-unmatched |
           liftoff | runtime-wasm (a NAMED wasm function not in the capture: dotnet.native.wasm for ours) | jiterp (an
           UNNAMED one: jiterpreter traces and interp-entry / jit_call wrappers) | native (outside every jitdump
           record: V8 builtins / chromium C++, unsymbolised)
  fn       the wasm function's name-section name
  op       the CLASS of the wasm instruction V8 compiled the sampled x86 from (j2d/wasmops.py), or v8:prologue /
           v8:entry (pcs before V8's first recorded position / positions inside the local declarations)
  tag      ours only: the emitter construct from the module's wj.origin section (MONO_WASM_JIT_ORIGIN)
  x86      hotinsn.classify of the sampled instruction itself (from the LIVE code bytes, any jitdump record)
  pos      mid-block / after-call / branch-target / function-entry (ledger.py's position classes)
  tier     the matched module's capture kind: m1 / m2 (tier 1 / tier 2), b (co-located group), l (lazy bank)

  wledger.py <rundir> <capdir> [--out PREFIX]
  wledger.py <rundir> <capdir> --tid N --window T0,T1 --steps S [--out PREFIX]     (a Minecraft mcperf mapping run:
             the server tid, the in-game window and the tick count from ledger/refresh.sh's outputs; per-"step" =
             per tick. Totals are samples x period of that thread in the window; of the two instructions streams a
             mapping run records (fp / LBR call stack) only the fp one is read.)

rundir: j2d/run.mjs set-A output (perf.data with PEBS instructions/cycles/l{1,2,3}_miss, jit-*.dump, window.json).
capdir: mods/*.wasm + x86/*.pkl (ledger/capture.py on the run's game.log for ours; the TeaVM module copied in;
        then ledger/d8x86.py). The chain and the layout gate are ledger/ledger.py's (masked_equal: the live code must
        equal d8's instruction for instruction except trailing immediates). For TeaVM the gate is what tells whether
        V8's feedback-driven inlining in the browser changed a function's layout: such a function reads
        module-unmatched rather than being mis-attributed.
  --addr          (a `perf record -d` run with mcperf's maps-benchStart/-benchEnd.txt snapshots) add a REGION column:
                  where each sampled load's DATA ADDRESS lies (j2d/addrregion.py: wasm linear memory split static /
                  shadow stack / heap, V8 trusted space, sandbox, pointer tables, native stack, ...). The miss events
                  plus mem_inst_retired.{all_loads,stlb_miss_loads} (kinds `loads`, `stlb_miss`) carry one; cycles and
                  instructions read region ''.
Writes PREFIX-full.tsv (one row per key, per-step value of every event) and PREFIX-summary.json."""
import bisect, collections, glob, json, os, pickle, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WJ = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread, wasmmod, wasmops
import addrregion
from jitindex import JitIndex
from counts import run_counts
import hotinsn

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
rundir, capdir = a[0], a[1]
outp = opt('--out', os.path.join(rundir, 'wledger'))

if opt('--tid'):
    tid = int(opt('--tid')); steps = float(opt('--steps'))
    t0, t1 = (float(x) for x in opt('--window').split(','))
    cnt0 = None
else:
    cnt0 = run_counts(rundir)
    if not cnt0['ok']:
        sys.exit(f'{rundir}: run not ok')
    tid, steps = cnt0['tid'], cnt0['steps']
    t0, t1 = json.load(open(os.path.join(rundir, 'window.json')))['window']
jpath = max(glob.glob(os.path.join(rundir, 'jit-*.dump')), key=os.path.getsize)
idx = JitIndex(jpath)
name_re = re.compile(r'^JS:(.*)-(\d+)-(turbofan|liftoff)$')

# ---------------------------------------------------------------- capture index
idx_path = os.path.join(capdir, 'index.tsv')
if not os.path.exists(idx_path):
    with open(idx_path + '.tmp', 'w') as f:
        for p in glob.glob(os.path.join(capdir, 'mods', '*.wasm')):
            sha = os.path.basename(p)[:-5]
            try:
                mm_ = wasmmod.parse(open(p, 'rb').read())
            except Exception:
                continue
            for fi, nm in mm_.names.items():
                f.write(f'{sha}\t{fi}\t{nm}\n')
    os.rename(idx_path + '.tmp', idx_path)
cap = collections.defaultdict(list)
for l in open(idx_path):
    sha, fi, nm = l.rstrip('\n').split('\t', 2)
    cap[(nm, int(fi))].append(sha)
ctier = {}   # capture kind per module: m1 / m2 (tier), b (co-located group), l (lazy bank); TeaVM has none
cp_ = os.path.join(capdir, 'capture.tsv')
if os.path.exists(cp_):
    for l in open(cp_):
        p_ = l.rstrip('\n').split('\t')
        if len(p_) >= 2:
            ctier[p_[0]] = p_[1]
tags = {}
tp = os.path.join(capdir, 'tags.tsv')
if os.path.exists(tp):
    for l in open(tp):
        k, v = l.rstrip('\n').split('\t'); tags[int(k)] = v

mods = {}
def module(sha):
    m = mods.get(sha)
    if m is None:
        b = open(os.path.join(capdir, 'mods', sha + '.wasm'), 'rb').read()
        m = mods[sha] = (wasmmod.parse(b), b, wasmops.imported_global_count(b))
    return m
pk = {}
def load_fn(sha, fi):
    d = pk.get(sha)
    if d is None:
        p = os.path.join(capdir, 'x86', sha + '.pkl')
        d = pk[sha] = pickle.load(open(p, 'rb')) if os.path.exists(p) else {}
    return d.get(fi)
opsc = {}
def ops_of(sha, fi):
    k = (sha, fi)
    v = opsc.get(k)
    if v is None:
        mm_, b, ng = module(sha)
        f = mm_.funcs[fi]
        offs, cls, _st = wasmops.body_ops(b, f['start'], f['end'], mm_.nfimports, ng)
        v = opsc[k] = (offs, cls)
    return v

def masked_equal(code, fn):
    """ledger.py's layout gate, verbatim in effect: identical instructions except a trailing imm32/rel32 (or the
    imm64 of a REX.W movabs)."""
    fc = fn['code']
    if len(code) < len(fc):
        return False
    for pc, ln in zip(fn['ipc'], fn['ilen']):
        a_ = fc[pc:pc + ln]; b_ = code[pc:pc + ln]
        if a_ == b_:
            continue
        tail = 8 if (ln == 10 and (a_[0] & 0xF8) == 0x48 and 0xB8 <= a_[1] <= 0xBF) else 4
        if ln > tail and a_[:ln - tail] == b_[:ln - tail]:
            continue
        return False
    return True

POS = {0: 'mid-block', 1: 'after-call', 2: 'branch-target', 3: 'function-entry'}
_PFX = {0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x64, 0x65, 0x36}
def position_classes(fn, live, base):
    code = fn['code']; ipc = fn['ipc']; ilen = fn['ilen']
    cls = {}; targets = set()
    for pc, ln in zip(ipc, ilen):
        b = code[pc:pc + ln]; i = 0
        while i < ln and (b[i] in _PFX or 0x40 <= b[i] <= 0x4F):
            i += 1
        if i >= ln:
            continue
        op = b[i]; nxt = pc + ln
        if op == 0xE8 or (op == 0xFF and i + 1 < ln and ((b[i + 1] >> 3) & 7) in (2, 3)):
            cls[nxt] = 1
        elif op == 0xE9 and ln - i == 5:
            targets.add(nxt + int.from_bytes(b[i + 1:i + 5], 'little', signed=True))
        elif op == 0xEB and ln - i == 2:
            targets.add(nxt + int.from_bytes(b[i + 1:i + 2], 'little', signed=True))
        elif 0x70 <= op <= 0x7F and ln - i == 2:
            targets.add(nxt + int.from_bytes(b[i + 1:i + 2], 'little', signed=True))
        elif op == 0x0F and i + 1 < ln and 0x80 <= b[i + 1] <= 0x8F and ln - i == 6:
            targets.add(nxt + int.from_bytes(b[i + 2:i + 6], 'little', signed=True))
    covered = set()
    for pc, ln in zip(ipc, ilen):
        covered.update(range(pc, pc + ln))
    for off in range(0, len(live) - 7, 8):
        if off in covered:
            continue
        v = int.from_bytes(live[off:off + 8], 'little') - base
        if 0 <= v < len(live):
            targets.add(v)
    for t in targets:
        cls.setdefault(t, 2)
    if ipc:
        cls[ipc[0]] = 3
    return cls

match = {}; match_stats = collections.Counter()
def matched(r):
    key = idx.key(r)
    if key in match:
        return match[key]
    res = None
    m = name_re.match(idx.name(r))
    if m and m.group(3) == 'turbofan':
        fi = int(m.group(2))
        cands = cap.get((m.group(1), fi), [])
        if cands:
            code = idx.code(r)
            for sha in cands:
                fn = load_fn(sha, fi)
                if fn is not None and masked_equal(code, fn):
                    fn = dict(fn); fn['pcls'] = position_classes(fn, code, idx.start[r]); del fn['code']
                    res = (sha, fi, fn); break
            match_stats['matched' if res else 'layout_mismatch'] += 1
        else:
            match_stats['not_captured'] += 1
    else:
        match_stats['liftoff_or_other'] += 1
    match[key] = res
    return res

dis = {}
def x86_at(r, off):
    k = idx.key(r)
    d = dis.get(k)
    if d is None:
        d = dis[k] = {o: (mn, ops) for o, mn, ops in hotinsn.disasm(idx._f, idx.foff[r], idx.size[r])}
    v = d.get(off)
    return hotinsn.classify(*v) if v else 'x86?'

def tagname(t): return tags.get(t, f'tag{t}')

def classify(ip, t):
    r = idx.at(ip, t)
    if r < 0:
        return ('native', '', '', '', '', '', '')
    off = ip - idx.start[r]
    x = x86_at(r, off)
    nm = idx.name(r)
    m = name_re.match(nm)
    mt = matched(r)
    if mt:
        sha, fi, fn = mt
        mm_, _b, _ng = module(sha)
        fname = mm_.names.get(fi, f'f{fi}')
        k = bisect.bisect_right(fn['ppc'], off) - 1
        og = mm_.origins.get(fi)
        tag = ''
        if k < 0:
            op = 'v8:prologue'
        else:
            wpos = fn['ppos'][k]
            ll = og['locals_len'] if og else None
            offs, cls = ops_of(sha, fi)
            if offs and wpos < offs[0]:
                op = 'v8:entry'
            else:
                j = bisect.bisect_right(offs, wpos) - 1
                op = cls[j] if j >= 0 else 'v8:entry'
            if og is not None:
                o = mm_.origin_at(fi, wpos)
                tag = 'v8:entry' if (ll is not None and wpos < ll) else (tagname(o[0]) if o else 'v8:no-origin')
        return ('module', fname, op, tag, x, POS[fn['pcls'].get(off, 0)], ctier.get(sha, ''))
    if m and m.group(1).startswith('wasm-function[') and (m.group(1), int(m.group(2))) not in cap:
        # an UNNAMED function outside the capture: dotnet.native.wasm carries a name section and every JIT module is
        # named by the emitter, so this is generated code -- jiterpreter traces / interp-entry and jit_call wrappers
        # (br_table-dispatched interpreter-frame code; 2026-10-03 ours-A1 wasm-function[5]), i.e. interpreted methods
        return ('jiterp', m.group(1), m.group(3), '', x, '', '')
    if m and m.group(3) == 'liftoff':
        return ('liftoff', m.group(1), '', '', x, '', '')
    if m and (m.group(1), int(m.group(2))) in cap:
        return ('module-unmatched', m.group(1), '', '', x, '', '')
    return ('runtime-wasm', m.group(1) if m else nm, '', '', x, '', '')

# ---------------------------------------------------------------- samples
at = perfread.attrs(os.path.join(rundir, 'perf.data'))
CALL_STACK = 1 << 11   # PERF_SAMPLE_BRANCH_CALL_STACK: the mapping runs' second instructions stream
def kind_of(x):
    n = x['name']
    if x['precise'] < 2:
        return None
    if 'instructions' in n: return None if x['bstype'] & CALL_STACK else 'ins'
    if 'cycles' in n: return 'cyc'
    for lv in ('l1_miss', 'l2_miss', 'l3_miss'):
        if lv in n: return lv
    if 'stlb_miss_loads' in n: return 'stlb_miss'
    if 'all_loads' in n: return 'loads'
    if 'near_call' in n: return 'calls'   # event set C: lands on the caller's call instruction (R432)
    return None
akind = {i: kind_of(x) for i, x in enumerate(at)}
ADDR = '--addr' in a
ADDR_KINDS = {'l1_miss', 'l2_miss', 'l3_miss', 'stlb_miss', 'loads'}
reg = addrregion.Regions(rundir) if ADDR else None
kinds = sorted({k for k in akind.values() if k})
cnt = {k: collections.Counter() for k in kinds}
nsamp = collections.Counter()
if ADDR:
    # two passes: the first finds the shadow stack (addresses of the emitter's frame constructs) and the native
    # stack (spill/reload addresses) so the second can name them
    samp = []
    for ai, ip, t, stid, chain, lbr, (ad, _ds) in perfread.samples(os.path.join(rundir, 'perf.data'), tid=tid, t0=t0,
                                                                     t1=t1, want_chain=False, want_lbr=False, want_addr=True):
        k = akind.get(ai)
        if k:
            key = classify(ip, t)
            d0 = ''
            if ad and k in ADDR_KINDS:
                # ':d0' = the sampled load's memory operand has NO displacement: on V8's x64 wasm code a wasm
                # `i32.load offset=0` -- an object's vtable word, a slot base -- as against a field/slot at an offset
                r_ = idx.at(ip, t)
                if r_ >= 0:
                    v_ = dis.get(idx.key(r_), {}).get(ip - idx.start[r_])
                    if v_:
                        mo = re.search(r'(-?0x[0-9a-f]+)?\(%', v_[1])
                        if mo and not mo.group(1):
                            d0 = ':d0'
            samp.append((k, key, ad if k in ADDR_KINDS else 0, d0)); nsamp[ai] += 1
            if ad and k in ADDR_KINDS:
                reg.observe(key[3], key[4], ad)
    reg.calibrate()
    for k, key, ad, d0 in samp:
        cnt[k][key + ((reg.name(ad) + d0) if ad else '',)] += 1
else:
  for ai, ip, t, stid, chain, lbr in perfread.samples(os.path.join(rundir, 'perf.data'), tid=tid, t0=t0, t1=t1,
                                                       want_chain=False, want_lbr=False):
    k = akind.get(ai)
    if k:
        cnt[k][classify(ip, t)] += 1; nsamp[ai] += 1
if cnt0 is not None:
    total = {'ins': cnt0['per_step']['instructions'], 'cyc': cnt0['per_step']['cycles'],
             'l1_miss': cnt0['per_step'].get('l1_miss', 0), 'l2_miss': cnt0['per_step'].get('l2_miss', 0),
             'l3_miss': cnt0['per_step'].get('l3_miss', 0)}
    if 'calls' in kinds:   # no counts.py total for event set C: samples x period over the window's steps
        total['calls'] = sum(nsamp[ai] * at[ai]['period'] for ai, k in akind.items() if k == 'calls') / steps
else:
    total = collections.Counter()
    for ai, k in akind.items():
        if k:
            total[k] += nsamp[ai] * at[ai]['period'] / steps
def val(kind, key):
    c = cnt.get(kind)
    s_ = sum(c.values()) if c else 0
    return c.get(key, 0) / s_ * total[kind] if s_ else 0.0
allk = set()
for c in cnt.values():
    allk |= set(c)
with open(outp + '-full.tsv', 'w') as f:
    f.write('kind\tfn\top\ttag\tx86\tpos\ttier\t' + ('region\t' if ADDR else '') + '\t'.join(f'{k}_per_step' for k in kinds) + '\n')
    for key in sorted(allk, key=lambda k: -cnt['cyc'].get(k, 0)):
        f.write('\t'.join(key) + '\t' + '\t'.join(f'{val(k, key):.2f}' for k in kinds) + '\n')
summ = dict(run=cnt0, tid=tid, window=[t0, t1], steps=steps, per_step=dict(total), match=dict(match_stats),
            regions=reg.describe() if ADDR else None,
            samples={k: sum(v.values()) for k, v in cnt.items()})
json.dump(summ, open(outp + '-summary.json', 'w'), indent=1)
by = collections.defaultdict(collections.Counter)
for k, v in cnt['cyc'].items():
    by['kind'][k[0]] += v
print(f'{rundir}: tid {tid}, samples {summ["samples"]}, match {dict(match_stats)}')
cs = sum(cnt['cyc'].values())
print('  cycles by kind:', ', '.join(f'{k} {v/cs:.1%}' for k, v in by['kind'].most_common()))
