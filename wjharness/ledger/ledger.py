#!/usr/bin/env python3
"""ledger.py -- the gap ledger, our side: every sampled server-thread instruction and cycle attributed to
(emitter construct | mono opcode, source method, IL offset, IL opcode); runtime/AOT code charged to the JIT call
site that entered it.

  ledger.py <rundir> <capdir> --tid N [--window t0,t1] [--ticks N] [--out PREFIX]

rundir: an mcperf capture taken with MONO_WASM_JIT_ORIGIN=1 MONO_WASM_JIT_CAPTURE=1, --jitdump, and
        MCPERF_EVENT=cpu_core/instructions/upp,cpu_core/cycles/upp MCPERF_PERIOD=2000003
capdir: capture.py + d8x86.py output for that run's game.log (d8x86 --only <sampled.py output> is enough)

THE CHAIN, each link exact and each checked here:
  1. sample ip -> jitdump record live at the sample's time (records are time-ordered; addresses are reused)
  2. record -> (module, function) whose d8 15.3.76.13 x86 has the IDENTICAL LAYOUT: every instruction the same
     length and the same bytes except a trailing immediate (see masked_equal). Unmatched records are a row.
  3. pc -> wasm body offset: the d8 --wasm-all-source-positions table (a position covers every pc up to the next)
  4. wasm offset -> origin: the module's wj.origin transitions
Instructions come from PEBS instruction samples, cycles from PEBS cycles samples (cpu_core/cycles/upp: the kernel
aliases it to a PEBS cycles event, so each sample's eventing IP is the instruction retiring when the counter
overflowed -- stalls land on the instruction that holds up retirement). LBR-block cycle attribution was tried
first and is BIASED (g0: getEntry 6.6% of instructions by PEBS but 0.7% of LBR cycles; WasmLiftoffFrameSetup at
CPI 102) -- a 32-branch window ending at a random cycle over-weights blocks before slow code, and user-only LBR
folds kernel time into the next user block -- so it is not implemented here; an imprecise capture gives no cycles.
MEMORY: the jitdump index is typed arrays (jitindex.py) and captured functions load on demand; the kernel OOM
killer took two earlier versions of this pipeline on the 15 GB box.
"""
import bisect, collections, glob, json, os, pickle, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import wasmmod, perfread
from jitindex import JitIndex

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
rundir, capdir = a[0], a[1]
tid = int(opt('--tid'))
outp = opt('--out', os.path.join(capdir, 'ledger'))

jpath = max(glob.glob(os.path.join(rundir, 'jit-*.dump')), key=os.path.getsize)
idx = JitIndex(jpath)
name_re = re.compile(r'^JS:(.*)-(\d+)-(turbofan|liftoff)$')

# ---------------------------------------------------------------- captured functions, on demand
idx_path = os.path.join(capdir, 'index.tsv')
if not os.path.exists(idx_path):
    with open(idx_path + '.tmp', 'w') as f:
        for p in glob.glob(os.path.join(capdir, 'mods', '*.wasm')):
            sha = os.path.basename(p)[:-5]
            try:
                mm = wasmmod.parse(open(p, 'rb').read())
            except Exception:
                continue
            for fi, nm in mm.names.items():
                f.write(f'{sha}\t{fi}\t{nm}\n')
    os.rename(idx_path + '.tmp', idx_path)
cap = collections.defaultdict(list)   # (name, funcidx) -> [sha]
for l in open(idx_path):
    sha, fi, nm = l.rstrip('\n').split('\t', 2)
    cap[(nm, int(fi))].append(sha)

# The tier each captured module was emitted at: capture kind m1 / m2 (a pre-plan2x capture says just 'm'), b = a
# co-located group, l = a lazy stub bank.
ctier = {}
cp_ = os.path.join(capdir, 'capture.tsv')
if os.path.exists(cp_):
    for l in open(cp_):
        p_ = l.rstrip('\n').split('\t')
        if len(p_) >= 2:
            ctier[p_[0]] = {'m1': '1', 'm2': '2', 'm': '?'}.get(p_[1], p_[1])

def load_fn(sha, fi):
    p = os.path.join(capdir, 'x86', sha + '.pkl')
    if not os.path.exists(p):
        return None
    return pickle.load(open(p, 'rb')).get(fi)

mods = {}
def module(sha):
    m = mods.get(sha)
    if m is None:
        m = mods[sha] = wasmmod.parse(open(os.path.join(capdir, 'mods', sha + '.wasm'), 'rb').read())
    return m
tags = {}
tp = os.path.join(capdir, 'tags.tsv')
if os.path.exists(tp):
    for l in open(tp):
        k, v = l.rstrip('\n').split('\t'); tags[int(k)] = v
def tagname(t): return tags.get(t, f'tag{t}')

def masked_equal(code, fn):
    """Instruction by instruction: identical except in the TRAILING immediate (imm32/rel32, or the imm64 of a
    10-byte REX.W movabs). What legitimately differs between the two processes lives exactly there: call/jmp rel32
    into the module's own jump table (placement), movabs of process addresses, and call_indirect's canonical
    signature ids (process-wide, assigned in canonicalisation order). Opcodes, ModRM, registers, displacements and
    every instruction LENGTH must agree, so the layout -- what the pc -> position transfer depends on -- is
    identical. Jump-table data words are excluded (d8x86.py keeps them out of ipc)."""
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

# POSITION CLASSES. A PEBS cycles sample charges a retirement stall to the oldest unretired instruction, so cycles on
# the first instruction after a CALL are mostly the return / front-end bubble, and cycles on a branch target or a
# function's first instruction are mostly fetch; only MID-BLOCK cycles are the instruction's own (data) stalls. Decoded
# from the d8 bytes (call/jmp/jcc encodings) plus the live jump-table words (br_table targets).
POS = {0: 'mid-block', 1: 'after-call', 2: 'branch-target', 3: 'function-entry'}
_PFX = {0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x64, 0x65, 0x36}
def position_classes(fn, live, base):
    code = fn['code']; ipc = fn['ipc']; ilen = fn['ilen']
    cls = {}
    targets = set()
    for pc, ln in zip(ipc, ilen):
        b = code[pc:pc + ln]; i = 0
        while i < ln and (b[i] in _PFX or 0x40 <= b[i] <= 0x4F):
            i += 1
        if i >= ln:
            continue
        op = b[i]
        nxt = pc + ln
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
    # jump tables: 8-byte absolute addresses in the LIVE code, in the gaps between instructions
    covered = set()
    for pc, ln in zip(ipc, ilen):
        covered.update(range(pc, pc + ln))
    size = len(live)
    for off in range(0, size - 7, 8):
        if off in covered:
            continue
        v = int.from_bytes(live[off:off + 8], 'little') - base
        if 0 <= v < size:
            targets.add(v)
    for t in targets:
        cls.setdefault(t, 2)
    if ipc:
        cls[ipc[0]] = 3
    return cls

def is_jit_name(nm):
    """A wasm-JIT function is named by mono's full method name, `Klass:method (sig)`; dotnet.native.wasm's are C or
    AOT-mangled identifiers with no spaces. 'entry' = a JIT module's interp->JIT entry thunk."""
    return (' (' in nm and nm.endswith(')')) or nm.startswith('wasmjit') or nm == 'entry'

match = {}
match_stats = collections.Counter()
def matched(r):
    key = idx.key(r)
    if key in match:
        return match[key]
    res = None
    m = name_re.match(idx.name(r))
    if m and m.group(3) == 'turbofan' and (m.group(1) == 'entry' or m.group(1).startswith('wasm-function[')):
        match_stats['name_not_unique'] += 1   # every module's entry thunk is 'entry'; lazy-bank stubs are unnamed
    elif m and m.group(3) == 'turbofan':
        fi = int(m.group(2))
        cands = cap.get((m.group(1), fi), [])
        if cands:
            code = idx.code(r)
            for sha in cands:
                fn = load_fn(sha, fi)
                if fn is not None and masked_equal(code, fn):
                    fn['pcls'] = position_classes(fn, code, idx.start[r])
                    del fn['code']   # only the layout, the position table and the classes are needed from here on
                    res = (sha, fi, fn); break
            match_stats['matched' if res else ('cand_mismatch' if is_jit_name(m.group(1)) else 'cand_mismatch_nonjit')] += 1
        else:
            match_stats['no_capture_jit' if is_jit_name(m.group(1)) else 'aot_or_runtime'] += 1
    else:
        match_stats['liftoff_or_other'] += 1
    match[key] = res
    return res

_ocache = {}
def origin_of(mt, off):
    k = (id(mt[2]), off)
    v = _ocache.get(k)
    if v is None:
        v = _ocache[k] = _origin_of(mt, off)
    return v

def pos_of(mt, off):
    return POS[mt[2]['pcls'].get(off, 0)]

def _origin_of(mt, off):
    """(construct-or-opcode tag name, source method, il, ilop) at function offset `off`."""
    sha, fi, fn = mt
    k = bisect.bisect_right(fn['ppc'], off) - 1
    mod = module(sha)
    if k < 0:
        return ('v8:prologue', mod.names.get(fi), -1, None)   # pcs before V8's first recorded position
    wpos = fn['ppos'][k]
    og = mod.origins.get(fi)
    if og is not None and wpos < og['locals_len']:
        # V8 charges its own function-entry code -- frame build, stack check, zeroing of locals TurboFan must
        # initialise -- to a position inside the local declarations (g0: every no-origin position was one)
        return ('v8:entry', mod.names.get(fi), -1, None)
    o = mod.origin_at(fi, wpos)
    if o is None:
        return ('v8:no-origin', mod.names.get(fi), -1, None)
    tag, me, il, op = o
    meths = og['meths']
    mname = mod.names.get(fi) if me == 0 else (meths[me] if me < len(meths) else '?inl')
    return (tagname(tag), mname, il, op)

# ---------------------------------------------------------------- window + ticks
ph = json.load(open(os.path.join(rundir, 'phases.json')))
if opt('--window'):
    t0, t1 = (float(x) for x in opt('--window').split(','))
else:
    from perfraw import phase_window
    t0, t1 = phase_window(ph, 'ingame')
span = t1 - t0
if opt('--ticks'):
    ticks = float(opt('--ticks'))
else:
    skipped = 0
    with open(os.path.join(rundir, 'game.log'), errors='replace') as f:
        inside = False
        for l in f:
            if '[mcw] START' in l: inside = True
            elif '[mcw] END' in l: inside = False
            elif inside and 'ticks behind' in l:
                m = re.search(r'or (\d+) ticks behind', l)
                if m: skipped += int(m.group(1))
    ticks = span * 20.0 - skipped
print(f'window {span:.1f}s, ticks {ticks:.0f}, tid {tid}, jitdump {os.path.basename(jpath)} ({idx.n} records), '
      f'{len(cap)} captured (name, index) keys', flush=True)

# ---------------------------------------------------------------- samples
at = perfread.attrs(os.path.join(rundir, 'perf.data'))
CALL_STACK = 1 << 11   # PERF_SAMPLE_BRANCH_CALL_STACK: the LBR holds the live call stack (from = call site)
def kind_of(x):
    n = x['name']
    if 'instructions' in n:
        return 'ins_lbr' if x['bstype'] & CALL_STACK else 'ins'
    if 'cycles' in n and x['precise'] >= 2:
        return 'cyc'
    for lv in ('l1_miss', 'l2_miss', 'l3_miss'):
        if lv in n and 'frontend' not in n:
            return lv
    # FRONTEND_RETIRED.* is PEBS on the retiring instruction the front end delayed; one per run (they share
    # MSR_PEBS_FRONTEND), so S5 takes three runs.
    for fe, lab in (('frontend_retired.l1i_miss', 'fe_l1i'), ('frontend_retired.l2_miss', 'fe_l2'),
                    ('frontend_retired.itlb_miss', 'fe_itlb'), ('frontend_retired.stlb_miss', 'fe_stlb'),
                    ('frontend_retired.unknown_branch', 'fe_unkbr'), ('frontend_retired.dsb_miss', 'fe_dsb'),
                    ('frontend_retired.latency_ge', 'fe_lat')):
        if fe in n:
            return lab
    return None
akind = {i: kind_of(x) for i, x in enumerate(at)}
kinds = sorted({k for k in akind.values() if k})
if 'cyc' not in kinds:
    print('!! no PRECISE cycles event in this capture: instructions only (LBR cycles are biased, see the header)')
if 'ins' not in kinds and 'ins_lbr' in kinds:
    for i, k in akind.items():
        if k == 'ins_lbr': akind[i] = 'ins'
    kinds = sorted({k for k in akind.values() if k})
print('events:', ', '.join(f"{at[i]['name']} -> {k}" for i, k in akind.items() if k), flush=True)
period = {i: at[i]['period'] for i in range(len(at))}
cnt = {k: collections.Counter() for k in kinds}
nsamp = collections.Counter()
ins = cnt.get('ins', collections.Counter()); cyc = cnt.get('cyc', collections.Counter())

def nonjit_name(r):
    if r >= 0:
        m = name_re.match(idx.name(r))
        return 'wasm:' + (m.group(1) if m else idx.name(r))
    return 'native'

def caller_of(chain, t, lbr=None, use_lbr=False):
    """origin of the first JIT call site on the stack. With an LBR CALL STACK, entry.from IS the call instruction
    (exact even for a frame-pointer-less leaf); with an fp chain it is return address - 1, and a leaf that never set
    up a frame loses its immediate caller -- the next frame up gets the charge."""
    if use_lbr and lbr:
        for fr, to, cy, ty in lbr:
            r = idx.at(fr, t)
            if r < 0:
                continue
            mt = matched(r)
            if mt:
                return origin_of(mt, fr - idx.start[r])
        # no JIT frame inside the 32-deep LBR stack (deep MH chains, longjmp-style unwinds): fall back to the fp chain
    for ra in chain[1:]:
        if ra >= perfread.CTX_FLOOR:
            continue
        r = idx.at(ra - 1, t)
        if r < 0:
            continue
        mt = matched(r)
        if mt:
            return origin_of(mt, ra - 1 - idx.start[r])
    return None

def classify(ip, t, chain, lbr=None, use_lbr=False):
    r = idx.at(ip, t)
    mt = matched(r) if r >= 0 else None
    if mt:
        return ('jit',) + origin_of(mt, ip - idx.start[r]) + (pos_of(mt, ip - idx.start[r]), ctier.get(mt[0], '?'))
    m = name_re.match(idx.name(r)) if r >= 0 else None
    if m and is_jit_name(m.group(1)):
        if m.group(3) == 'liftoff':
            return ('jit-liftoff', m.group(1), None, -1, None)
        if m.group(1) == 'entry':
            return ('jit-entry-thunk', m.group(1), None, -1, None)
        return ('jit-unmatched' if (m.group(1), int(m.group(2))) in cap else 'jit-uncaptured', m.group(1), None, -1, None)
    c = caller_of(chain, t, lbr, use_lbr)
    return ('nonjit', nonjit_name(r)) + ((c[0], c[1]) if c else (None, None))

for ai, ip, t, stid, chain, lbr in perfread.samples(os.path.join(rundir, 'perf.data'), tid=tid, t0=t0, t1=t1):
    k = akind.get(ai)
    if not k:
        continue
    nsamp[ai] += 1
    cnt[k][classify(ip, t, chain, lbr, bool(at[ai]['bstype'] & CALL_STACK))] += 1

# ---------------------------------------------------------------- report
def total(kind):
    return sum(nsamp[i] * period[i] for i, k in akind.items() if k == kind)
I = total('ins'); C = total('cyc')
per_tick = {k: total(k) / ticks for k in kinds}
print('per tick:', ', '.join(f'{k} {v/1e6:.3f} M' for k, v in per_tick.items()))
def nsamples(kind): return sum(nsamp[i] for i, k in akind.items() if k == kind)
print(f'instructions {I/ticks/1e6:.2f} M/tick ({nsamples("ins")} samples), cycles {C/ticks/1e6:.2f} M/tick '
      f'({nsamples("cyc")} samples)')
print('record matching:', dict(match_stats))
isum = sum(ins.values()) or 1; csum = sum(cyc.values()) or 1
def ins_m(n): return n / isum * I / ticks / 1e6
def cyc_m(c): return c / csum * C / ticks / 1e6
kinds_i = collections.Counter(); kinds_c = collections.Counter()
for k, v in ins.items(): kinds_i[k[0]] += v
for k, v in cyc.items(): kinds_c[k[0]] += v
tier_i = collections.Counter(); tier_c = collections.Counter()
for k, v in ins.items():
    if k[0] == 'jit': tier_i[k[6]] += v
for k, v in cyc.items():
    if k[0] == 'jit': tier_c[k[6]] += v
print('JIT code by tier (M instr/tick, M cycles/tick):', ', '.join(f'{t}: {ins_m(tier_i.get(t,0)):.2f} / {cyc_m(tier_c.get(t,0)):.2f}'
      for t in sorted(set(tier_i) | set(tier_c))))
print('by kind (M instr/tick, M cycles/tick):')
for k in sorted(set(kinds_i) | set(kinds_c), key=lambda k: -kinds_i.get(k, 0)):
    print(f'  {ins_m(kinds_i.get(k,0)):7.3f} {cyc_m(kinds_c.get(k,0)):7.3f}  {k}')

def tagkey(k):
    return k[1] if k[0] == 'jit' else (k[0] + (':caller ' + str(k[2]) if k[0] == 'nonjit' else ''))
by_i = collections.Counter(); by_c = collections.Counter()
for k, v in ins.items(): by_i[tagkey(k)] += v
for k, v in cyc.items(): by_c[tagkey(k)] += v
rows = sorted(((ins_m(by_i.get(k, 0)), cyc_m(by_c.get(k, 0)), k) for k in set(by_i) | set(by_c)), key=lambda r: -r[1])
with open(outp + '-bytag.tsv', 'w') as f:
    f.write('M_instr_per_tick\tM_cycles_per_tick\ttag\n')
    for r in rows:
        f.write(f'{r[0]:.4f}\t{r[1]:.4f}\t{r[2]}\n')
print('\nTOP by tag, by cycles (M instr/tick, M cycles/tick):')
for r in rows[:45]:
    print(f'  {r[0]:7.3f} {r[1]:7.3f}  {r[2]}')

# per-key value of event kind k in M per tick: share of that kind's samples x that kind's per-tick total
def val(kind, key):
    c = cnt.get(kind)
    if not c:
        return 0.0
    s_ = sum(c.values()) or 1
    return c.get(key, 0) / s_ * per_tick[kind] / 1e6
# Cycles and memory events use the LBR call stack when it is there; the fp instructions stream keeps the old caller
# attribution so the two can be compared below.
extra = [k for k in ('ins_lbr', 'l1_miss', 'l2_miss', 'l3_miss', 'fe_l1i', 'fe_l2', 'fe_itlb', 'fe_stlb', 'fe_unkbr',
                     'fe_dsb', 'fe_lat') if k in cnt]
# The M_instr column takes the LBR-attributed stream when there is one (exact callers for runtime code); the fp
# stream stays as an extra column for the comparison.
INSK = 'ins_lbr' if 'ins_lbr' in cnt else 'ins'
if INSK == 'ins_lbr':
    extra = ['ins'] + [e for e in extra if e != 'ins_lbr']
with open(outp + '-full.tsv', 'w') as f:
    # kind: jit (tag/method/il/ilop/pos are the origin); jit-* (symbol = the function); nonjit (symbol = runtime/AOT
    # code; caller_tag / caller_method = the JIT call site that entered it). M per tick for each event kind.
    f.write('kind\tM_instr_per_tick\tM_cycles_per_tick\ttag\tmethod\til\tilop\tsymbol\tcaller_tag\tcaller_method\tpos'
            + ''.join(f'\tM_{e}_per_tick' for e in extra) + '\ttier\n')
    allk = set()
    for c in cnt.values():
        allk |= set(c)
    for k in sorted(allk, key=lambda k: -cyc.get(k, 0)):
        kind = k[0]
        if kind == 'jit':
            cols = (k[1], k[2], k[3], k[4], '', '', '', k[5])
        elif kind == 'nonjit':
            cols = ('', '', '', '', k[1], k[2], k[3], '')
        else:
            cols = ('', '', '', '', k[1], '', '', '')
        f.write(f'{kind}\t{val(INSK, k):.5f}\t{val("cyc", k):.5f}\t' + '\t'.join('' if c is None else str(c) for c in cols)
                + ''.join(f'\t{val(e, k):.5f}' for e in extra) + '\t' + (k[6] if kind == 'jit' else '') + '\n')

# FP vs LBR CALLER for runtime code: the same instructions event recorded twice, once per call-graph mode. A runtime
# symbol whose caller distribution differs between the two is being misattributed by the fp chain; the share that
# moves is the frame-pointer-less-leaf error, measured rather than argued.
if 'ins' in cnt and 'ins_lbr' in cnt:
    def callers(c):
        d = collections.defaultdict(collections.Counter)
        for k, v in c.items():
            if k[0] == 'nonjit':
                d[k[1]][(k[2], k[3])] += v
        return d
    fpc, lbc = callers(cnt['ins']), callers(cnt['ins_lbr'])
    fps = sum(cnt['ins'].values()) or 1; lbs = sum(cnt['ins_lbr'].values()) or 1
    moved = tot_nj = 0.0; rows_ = []
    for sym in set(fpc) | set(lbc):
        a_ = {k: v / fps * per_tick['ins'] / 1e6 for k, v in fpc.get(sym, {}).items()}
        b_ = {k: v / lbs * per_tick['ins_lbr'] / 1e6 for k, v in lbc.get(sym, {}).items()}
        ta, tb = sum(a_.values()), sum(b_.values())
        if not ta or not tb:
            continue
        overlap = sum(min(a_.get(k, 0) / ta, b_.get(k, 0) / tb) for k in set(a_) | set(b_))
        mv = (ta + tb) / 2 * (1 - overlap)
        moved += mv; tot_nj += (ta + tb) / 2
        rows_.append((mv, ta, tb, sym))
    rows_.sort(reverse=True)
    with open(outp + '-leafcallers.tsv', 'w') as f:
        f.write('M_instr_moved\tM_instr_fp\tM_instr_lbr\tsymbol\n')
        for r_ in rows_:
            f.write(f'{r_[0]:.5f}\t{r_[1]:.5f}\t{r_[2]:.5f}\t{r_[3]}\n')
    print(f'\nFP vs LBR caller attribution of runtime code: {moved:.3f} of {tot_nj:.3f} M instr/tick changes caller '
          f'({moved/max(tot_nj,1e-9):.1%}); top movers:')
    for r_ in rows_[:12]:
        print(f'  {r_[0]:7.3f} moved  (fp {r_[1]:.3f} / lbr {r_[2]:.3f})  {r_[3][:100]}')
json.dump(dict(window=[t0, t1], ticks=ticks, instr_per_tick=I / ticks, cycles_per_tick=C / ticks,
               per_tick={k: v for k, v in per_tick.items()},
               match=dict(match_stats), kinds_instr={k: ins_m(v) for k, v in kinds_i.items()},
               kinds_cycles={k: cyc_m(v) for k, v in kinds_c.items()}), open(outp + '-summary.json', 'w'), indent=1)
print(f'\nwrote {outp}-bytag.tsv, -full.tsv, -summary.json')
