#!/usr/bin/env python3
"""gap.py -- join the two ledgers into the itemised gap.

  gap.py <ours ledger prefix> <hs ledger prefix> [--out report.md] [--top N] [--tsv PREFIX]

Ours: ledger.py's <prefix>-full.tsv (+ -leafcallers.tsv). HotSpot: hsledger.py's <prefix>-full.tsv (+ -scopes.txt).
Both per tick of the server thread.

ATTRIBUTION RULES (each one exists because a naive join gave a wrong answer on g1):
  * Methods join on 'pkg.Class.method' (overloads merged, nested '/' -> '$').
  * Spellings of the SAME Java method that differ between the runtimes are unified: mono .ctor/.cctor = <init>/<clinit>;
    IKVM's `<default>name` (a default method's body on an interface helper) and `instancehelper_name` (Object's methods)
    are the Java method; IKVM maps java.lang.Math to System.Math (lower-cased first letter).
  * IKVM-AOT Java library code (wasm:IKVM_Java_<pkg>_<Class>_<method>_<args>) is charged to that Java METHOD, as HotSpot
    charges it; other runtime code is charged to the JIT call site that entered it (LBR call stack when recorded).
  * IKVM-generated code with no Java counterpart is MACHINERY by name: __<overridestub> (interface/bridge forwarding
    stubs -- their names embed the target, and the first parser read `AbstractReferenceSet:__<overridestub>java.util.Set
    ::contains` as `util.Set.contains`), __<>MHC, __<>DynamicBinder, AnonN, other __<...>, IKVM.*, object.DMH/BMH, the
    JIT entry thunk `entry`, and corlib (System.*) methods.
  * IDLE LOOP: ThreadExecutor.waitForTasks (class_1255.method_20813) and the GLFW time supplier (GLX.lambda$_initGlfw$2)
    are the inter-tick wait; reported apart from tick WORK because it grows as the tick gets faster.
  * Lambda classes are numbered differently on the two JVMs and are pooled per side.
  * An ours-only Java method that appears in a sampled HotSpot blob's scope chain (any inlining depth) is RUN by
    HotSpot, inlined, at a cost under its resolution; one that appears nowhere is genuinely not run there.
"""
import collections, csv, os, re, sys

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
ours_p, hs_p = a[0], a[1]
top = int(opt('--top', '40'))

JAVA = re.compile(r'([A-Za-z_][A-Za-z0-9_.$/]*)::?([A-Za-z_<][A-Za-z0-9_<>$]*)')
def mkey(s):
    if not s or s == 'None':
        return None
    if s == 'entry':
        return '(JIT entry thunks)'
    s = s.split(' (')[0]
    if '__<overridestub>' in s:
        cls = s.split('__<overridestub>')[0].rstrip(':').split(' ')[-1]
        return 'IKVM.overridestub:' + cls.replace('/', '$')
    s = s.replace(':.ctor', ':<init>').replace(':.cctor', ':<clinit>').replace(':<default>', ':').replace(':instancehelper_', ':')
    m = None
    for m in JAVA.finditer(s):
        pass
    if not m:
        return s.split(' ', 1)[1] if ' ' in s and 'System.' in s else s
    cls, meth = m.group(1).replace('/', '$'), m.group(2)
    if cls == 'System.Math':
        return 'java.lang.Math.' + meth[:1].lower() + meth[1:]
    k = cls + '.' + meth
    if '$$Lambda$' in k:
        return '(lambda adapters)'
    return k

# The inter-tick wait, by every name it is charged under. With tier 2 on (S0), waitForTasks (method_20813) and the
# time-supplier lambda are INLINED into their callers, so the runtime they call is charged to the JIT'd stubs it
# enters instead -- glfwGetTime, Unsafe.park, Unsafe.putObject (LockSupport.setBlocker) -- and the two names above
# read ~0 (b7: 0.04 M against g2's 3.65 M; the stubs carry ~4.5 M). The b4/sc site census (2026-10-01) shows the
# stubs' server-thread callers: putObject/park 100% from method_20813 / method_18857 (runTasks), glfwGetTime from
# the time check behind MinecraftServer.method_20415 (shouldKeepTicking, ~470 calls/tick) except one tier-0 site at
# ~2 calls/tick. So on the SERVER thread these are idle; on another thread they need not be.
IDLE = {'net.minecraft.class_1255.method_20813', 'com.mojang.blaze3d.platform.GLX.lambda$_initGlfw$2',
        'net.minecraft.class_1255.method_18857', 'net.minecraft.server.MinecraftServer.method_20415',
        'org.lwjgl.glfw.GLFW.glfwGetTime', 'sun.misc.Unsafe.park', 'sun.misc.Unsafe.putObject'}

def rows(path):
    with open(path) as f:
        r = csv.reader(f, delimiter='\t')
        hdr = next(r)
        for x in r:
            yield dict(zip(hdr, x))
def f_(r, col): return float(r.get(col) or 0)

SCAFF = {'PROLOGUE', 'FRAME', 'IC_PROLOGUE', 'EH_PUSH', 'EH_POP', 'EXIT', 'DISPATCH', 'BB_ILOFS', 'SLOTZERO',
         'ENSURE_FRAME', 'PIN', 'REF_LD', 'ADDR_ST', 'ADDR_LD', 'RETURN', 'BB_FALL', 'LANDING', 'THROW_SITES',
         'EPILOGUE', 'UNKNOWN', 'v8:prologue', 'v8:entry', 'v8:no-origin'}
CALLS = {'METHOD_CALL', 'DYN_CALL', 'HELPER_CALL', 'VCALL_PIC', 'AOT_IC'}
OPF = [
    (re.compile(r'call'), 'call arg/result setup'),
    (re.compile(r'^gc_safe_point$'), 'safepoint poll'),
    (re.compile(r'^cond_exc'), 'checks (cond_exc: null/bounds/overflow)'),
    (re.compile(r'^(check_this|bounds_check|not_null)'), 'checks (cond_exc: null/bounds/overflow)'),
    (re.compile(r'(load|^ld).*membase|^load'), 'memory loads'),
    (re.compile(r'(store|^st).*membase|^store'), 'memory stores'),
    (re.compile(r'^((int|long|float|r)_)?b(eq|ne|lt|gt|le|ge)|^br$|^br_reg|switch|^jump_table'), 'branches'),
    (re.compile(r'compare|_c(eq|ne|gt|lt|ge|le)(_un)?$|^c(eq|gt|lt)'), 'compares'),
    (re.compile(r'alloc|newarr|newobj'), 'allocation'),
    (re.compile(r'move|^vzero|^xzero|iconst|i8const|r4const|r8const|^dummy'), 'moves / constants'),
    (re.compile(r'memory_barrier|atomic'), 'atomics / barriers'),
    (re.compile(r'conv'), 'conversions'),
]
def op_family(o):
    for rx, nm in OPF:
        if rx.search(o):
            return nm
    return 'arithmetic / other'

def runtime_family(s):
    if s.startswith('wasm:mono_wasm_jit') or s.startswith('wasm:wj_') or 'jiterp' in s or s.startswith('wasm:interp_') \
            or s.startswith('wasm:mono_interp'):
        return 'runtime: JIT tier helpers / interpreter'
    if s.startswith('wasm:IKVM_Java_'):
        return 'runtime: Java library (IKVM AOT)'
    if s.startswith('wasm:IKVM_'):
        return 'runtime: IKVM runtime (AOT)'
    if s.startswith('wasm:corlib') or s.startswith('wasm:aot_'):
        return 'runtime: corlib (AOT)'
    if s.startswith('wasm:Builtin:') or s.startswith('wasm:JS:') or s == 'native':
        return 'runtime: V8 builtins / chromium / kernel'
    if s.startswith('wasm:'):
        return 'runtime: mono runtime / libc / emscripten'
    return 'runtime: other'

def our_group(r):
    k = r['kind']
    if k == 'nonjit':
        return runtime_family(r['symbol'])
    if k != 'jit':
        return 'jit: unmapped (' + k + ')'
    t = r['tag']
    if t in SCAFF:
        return 'scaffolding: ' + t
    if t in CALLS:
        return 'calls: ' + t
    if t.startswith('op:'):
        f = op_family(t[3:])
        if f == 'memory loads':
            fl = il_family(r.get('ilop'))
            if fl == DISPATCH_LOAD: return 'calls: dispatch loads (vtable / IMT / guards)'
            if fl == TYPECHECK_LOAD: return 'body: type-check loads (casts, array store checks)'
            if fl.startswith('locals/args'): return 'body: loads of locals kept in linear memory'
        return ('scaffolding: ' if f == 'safepoint poll' else 'body: ') + f
    return 'jit: ' + t

HS_FIELD = {'getfield', 'putfield', 'getstatic', 'putstatic', 'arraylength'}
def hs_bc_family(b):
    b = b or ''
    if b in ('getfield', 'putfield'): return 'field access'
    if b in ('getstatic', 'putstatic'): return 'static field access'
    if b.endswith('aload') or b.endswith('astore') or b == 'arraylength': return 'array access'
    if b.startswith('invoke'): return 'call (invoke*)'
    if b in ('checkcast', 'instanceof'): return 'type check'
    if b.startswith('if') or b in ('goto', 'tableswitch', 'lookupswitch'): return 'compare / branch'
    if b in ('new', 'newarray', 'anewarray', 'multianewarray'): return 'allocation'
    if b in ('monitorenter', 'monitorexit'): return 'monitor'
    return 'other bytecode'
def hs_group(r):
    c = r['class']
    if c in ('hs:body', 'hs:noscope'):
        f = hs_bc_family(r['bytecode'])
        if f == 'call (invoke*)': return 'calls: invoke bytecodes'
        if f in ('field access', 'static field access', 'array access'): return 'body: field / array access'
        if f == 'compare / branch': return 'body: compares / branches'
        if f == 'type check': return 'body: checks'
        if f == 'allocation': return 'body: allocation'
        return 'body: other bytecodes'
    return {'hs:entry': 'scaffolding: entry', 'hs:poll': 'scaffolding: safepoint poll', 'hs:stub': 'scaffolding: stubs',
            'hs:unmapped': 'runtime: libjvm / stubs / interpreter'}.get(c, c)

# IL opcode of a load, to the Java construct it implements (CIL opcode numbering; 0xFE00|b for prefixed ones).
# A load carries the IL offset of the instruction mono emitted it FOR, so a vtable/IMT/guard load sits at a callvirt
# and an array-store covariance check at a stelem.ref -- those are dispatch and type-check work, not field access.
def il_family(op):
    try: op = int(op)
    except Exception: return 'unknown'
    if op in (0x6F, 0x28, 0x29, 0x73, 0xFE06, 0xFE07): return 'call dispatch (vtable / IMT / guard loads)'
    if op in (0x74, 0x75, 0xA2, 0x8C, 0x79, 0xA5): return 'type check (cast, isinst, array store check, unbox)'
    if op in (0x7B, 0x7C, 0x7D): return 'field access'
    if op in (0x7E, 0x7F, 0x80): return 'static field access'
    if op in (0x8E, 0x8F) or 0x90 <= op <= 0x9A or op == 0xA3: return 'array access (load)'
    if 0x9B <= op <= 0xA1 or op == 0xA4: return 'array access (store)'
    if 0x2B <= op <= 0x45: return 'branch condition operand'
    if 0x46 <= op <= 0x57 or op in (0x71, 0x81): return 'indirect (ldind/ldobj: byref, IKVM helpers)'
    if 0x02 <= op <= 0x13 or op in (0xFE09, 0xFE0A, 0xFE0B, 0xFE0C, 0xFE0D, 0xFE0E): return 'locals/args in linear memory (address-taken, vtypes)'
    if op == 0xFFFF: return 'unknown'
    return 'other (arith / conv / stloc IL)'
DISPATCH_LOAD = 'call dispatch (vtable / IMT / guard loads)'
TYPECHECK_LOAD = 'type check (cast, isinst, array store check, unbox)'

ours = list(rows(ours_p + '-full.tsv'))
hs = list(rows(hs_p + '-full.tsv'))
scope_set = set()
sp = hs_p + '-scopes.txt'
if os.path.exists(sp):
    for l in open(sp):
        k = mkey(l.strip().replace('::', ':'))
        if k: scope_set.add(k)

# ---------------------------------------------------------------- method keys
hs_keys = {}
for r in hs:
    k = mkey(r['method'])
    if k:
        hs_keys[re.sub(r'[^a-z0-9]', '', k.lower())] = k
def aot_java_key(sym):
    body = sym[len('wasm:IKVM_Java_'):]
    parts = body.split('_')
    for n in range(len(parts), 1, -1):
        k = hs_keys.get(re.sub(r'[^a-z0-9]', '', ''.join(parts[:n]).lower()))
        if k:
            return k
    return None
def our_key(r):
    if r['kind'] == 'jit':
        return mkey(r['method'])
    if r['kind'] == 'nonjit':
        if r['symbol'].startswith('wasm:IKVM_Java_'):
            k = aot_java_key(r['symbol'])
            if k:
                return k
        return mkey(r['caller_method'])
    return mkey(r['symbol'])

O = collections.defaultdict(collections.Counter); OI = collections.defaultdict(collections.Counter)
H = collections.defaultdict(collections.Counter); HI = collections.defaultdict(collections.Counter)
for r in ours:
    k = our_key(r) or '(no JIT caller)'
    g = our_group(r)
    O[k][g] += f_(r, 'M_cycles_per_tick'); OI[k][g] += f_(r, 'M_instr_per_tick')
for r in hs:
    k = mkey(r['method']) or '(no blob caller)'
    g = hs_group(r)
    H[k][g] += f_(r, 'M_cycles_per_tick'); HI[k][g] += f_(r, 'M_instr_per_tick')
def tot(d, k): return sum(d[k].values()) if k in d else 0.0
OC = sum(sum(v.values()) for v in O.values()); OIt = sum(sum(v.values()) for v in OI.values())
HC = sum(sum(v.values()) for v in H.values()); HIt = sum(sum(v.values()) for v in HI.values())

def machinery_family(k):
    if k in IDLE: return 'idle loop (waitForTasks + time supplier)'
    if k.startswith('object.') and 'stelemref' in k: return 'mono array-store-check wrappers (virt_stelemref)'
    if k.startswith('object.PairwiseConvert'): return 'IKVM: MethodHandle / invokedynamic chains'
    if k.startswith('IKVM.overridestub:'): return 'IKVM: override/bridge forwarding stubs'
    if k.startswith('IKVM.Runtime.MethodHandleUtil') or k.startswith('object.DMH') or k.startswith('object.BMH') \
            or '__<>MHC' in k or 'java.lang.invoke' in k:
        return 'IKVM: MethodHandle / invokedynamic chains'
    if 'DynamicBinder' in k: return 'IKVM: lazy-link DynamicBinder'
    if re.search(r'(^|\.)Anon\d+\.', k) or '__<' in k: return 'IKVM: generated Anon / __<...> code'
    if k.startswith('IKVM.'): return 'IKVM: runtime helpers (casts, isInstance, natives)'
    if k.startswith('System.') or k.startswith('Microsoft.') or ' System.' in k: return 'corlib (JIT): IKVM runtime data structures'
    if k.startswith('object.'): return 'mono wrappers (object.*)'
    if k == '(JIT entry thunks)': return 'JIT entry thunks (interp -> JIT)'
    if k == '(lambda adapters)': return 'lambda adapters'
    if k.startswith('(no JIT caller)'): return 'runtime with no JIT frame on the stack'
    return None   # a Java method
_mf = machinery_family
def machinery_family(k):   # noqa: F811 -- extend with the forwarding-stub rule once H and the scope set exist
    f = _mf(k)
    if f: return f
    if 'forwarding_stub' in globals() and tot(O, k) > 0 and forwarding_stub(k):
        return 'IKVM: forwarders for inherited default methods'
    return None

keys = set(O) | set(H)
hs_meth_names = collections.defaultdict(set)
for k in H:
    if '.' in k: hs_meth_names[k.rsplit('.', 1)[1]].add(k)
for k in scope_set:
    if '.' in k: hs_meth_names[k.rsplit('.', 1)[1]].add(k)
def forwarding_stub(k):
    """IKVM materialises an inherited default method as a forwarder on the implementing class (ServerWorld gets
    method_22341, which calls the interface helper); HotSpot runs the interface's own method_22341. Intermediary
    names (method_NNNN) are shared by an override family only, so an ours-only C.method_NNNN that HotSpot never
    runs anywhere, while it does run some X.method_NNNN, is that forwarder."""
    if '.' not in k or tot(H, k) > 0 or k in scope_set: return False
    nm = k.rsplit('.', 1)[1]
    return nm.startswith('method_') and any(x != k for x in hs_meth_names.get(nm, ()))
def klass(k):
    if machinery_family(k): return 'machinery'
    if tot(O, k) > 0 and forwarding_stub(k): return 'machinery'
    if tot(O, k) > 0 and tot(H, k) > 0: return 'matched'
    if tot(O, k) > 0: return 'java-ours-only'
    return 'hs-only'
cls_of = {k: klass(k) for k in keys}

out = []; P = out.append
idle_o = sum(tot(O, k) for k in IDLE); idle_h = sum(tot(H, k) for k in IDLE)
P('# Gap ledger -- server tick (PEBS eventing IP on both sides)\n')
P('| per tick | ours | HotSpot | ratio |\n|---|---|---|---|')
P(f'| instructions | {OIt:.2f} M | {HIt:.2f} M | {OIt/HIt:.2f}x |')
P(f'| cycles | {OC:.2f} M | {HC:.2f} M | {OC/HC:.2f}x |')
P(f'| cycles, tick work only (minus idle loop) | {OC-idle_o:.2f} M | {HC-idle_h:.2f} M | {(OC-idle_o)/(HC-idle_h):.2f}x |\n')

# ---------------------------------------------------------------- the full table
EX = OC - HC
def coarse(g):
    if g.startswith('scaffolding') or g.startswith('calls') or g.startswith('body: call arg'): return 'call / frame / dispatch machinery'
    if g.startswith('body'): return 'method bodies'
    return 'runtime code the method calls'
lines = []
for clsname, title in (('matched', 'Java methods both run'), ('java-ours-only', 'Java methods HotSpot runs at ~0')):
    ks = [k for k in keys if cls_of[k] == clsname]
    co = collections.Counter(); ch = collections.Counter()
    for k in ks:
        for g, v in O[k].items(): co[coarse(g)] += v
        for g, v in H[k].items(): ch[coarse(g)] += v
    for b in ('method bodies', 'call / frame / dispatch machinery', 'runtime code the method calls'):
        lines.append((f'{title} ({len(ks)}): {b}', co[b], ch[b]))
fam = collections.Counter(); famh = collections.Counter(); famn = collections.Counter()
for k in keys:
    if cls_of[k] == 'machinery':
        f = machinery_family(k); fam[f] += tot(O, k); famh[f] += tot(H, k); famn[f] += 1
for f, v in fam.most_common():
    lines.append((f'machinery, no Java counterpart: {f} ({famn[f]})', v, famh[f]))
hso = [k for k in keys if cls_of[k] == 'hs-only']
lines.append((f'HotSpot-only methods ({len(hso)})', 0.0, sum(tot(H, k) for k in hso)))
P('## The full table: where the excess goes (M cycles per tick)\n')
P('| bucket | ours | HotSpot | excess | share of excess |\n|---|---|---|---|---|')
for nm, o, h in lines:
    P(f'| {nm} | {o:.2f} | {h:.2f} | {o-h:.2f} | {(o-h)/EX:.1%} |')
P(f'| **total** | **{OC:.2f}** | **{HC:.2f}** | **{EX:.2f}** | 100% |\n')

# Java methods HotSpot runs at ~0: does HotSpot run them at all?
jo = [k for k in keys if cls_of[k] == 'java-ours-only']
inl = [k for k in jo if k in scope_set]; nor = [k for k in jo if k not in scope_set]
if scope_set:
    P(f'Of the {len(jo)} Java methods HotSpot shows no samples for, **{len(inl)} ({sum(tot(O,k) for k in inl):.2f} M of our cycles) '
      f'appear in a sampled HotSpot blob\'s inlining scopes** -- HotSpot runs them, inlined, below its resolution -- and '
      f'{len(nor)} ({sum(tot(O,k) for k in nor):.2f} M) appear nowhere in sampled HotSpot code.\n')

# ---------------------------------------------------------------- by cause, all Java methods
P('### Java methods (both kinds), by cause (M cycles / M instructions per tick)\n')
P('| our cause | ours M cyc | ours M instr | | HotSpot cause | HS M cyc | HS M instr |\n|---|---|---|---|---|---|---|')
mo = collections.Counter(); mh = collections.Counter(); moi = collections.Counter(); mhi = collections.Counter()
for k in keys:
    if cls_of[k] in ('matched', 'java-ours-only', 'hs-only'):
        for g, v in O[k].items(): mo[g] += v
        for g, v in OI[k].items(): moi[g] += v
        for g, v in H[k].items(): mh[g] += v
        for g, v in HI[k].items(): mhi[g] += v
lo = sorted(mo.items(), key=lambda x: -x[1]); lh = sorted(mh.items(), key=lambda x: -x[1])
for i in range(max(len(lo), len(lh))):
    a_ = lo[i] if i < len(lo) else ('', 0); b_ = lh[i] if i < len(lh) else ('', 0)
    P(f'| {a_[0]} | {a_[1]:.2f} | {moi.get(a_[0],0):.2f} | | {b_[0]} | {b_[1]:.2f} | {mhi.get(b_[0],0):.2f} |')

# ---------------------------------------------------------------- memory loads, split
have_miss = 'M_l1_miss_per_tick' in (ours[0] if ours else {})
def is_our_load(r): return r['kind'] == 'jit' and r['tag'].startswith('op:') and op_family(r['tag'][3:]) == 'memory loads'
def is_hs_mem(r): return r['class'] in ('hs:body', 'hs:noscope') and hs_bc_family(r['bytecode']) in ('field access', 'static field access', 'array access')
cols = ['M_instr_per_tick', 'M_cycles_per_tick'] + (['M_l1_miss_per_tick', 'M_l2_miss_per_tick', 'M_l3_miss_per_tick'] if have_miss else [])
P('\n## Memory loads, split\n')
P('### By the Java construct the load implements (ours: IL opcode of the load; HotSpot: the bytecode)\n')
hdr = '| construct | ours instr | ours cyc |' + (' ours L1 / L2 / L3 miss |' if have_miss else '') + ' HS instr | HS cyc |' + (' HS L1 / L2 / L3 miss |' if have_miss else '')
P(hdr); P('|' + '---|' * hdr.count('|')[:-1] if False else '|' + '|'.join(['---'] * (hdr.count('|') - 1)) + '|')
og = collections.defaultdict(collections.Counter); hg = collections.defaultdict(collections.Counter)
for r in ours:
    if is_our_load(r):
        fm = il_family(r['ilop'])
        for c in cols: og[fm][c] += f_(r, c)
for r in hs:
    if is_hs_mem(r):
        fm = hs_bc_family(r['bytecode'])
        for c in cols: hg[fm][c] += f_(r, c)
fams = sorted(set(og) | set(hg), key=lambda f: -og[f]['M_cycles_per_tick'])
def misses(d): return f" {d['M_l1_miss_per_tick']:.3f} / {d['M_l2_miss_per_tick']:.3f} / {d['M_l3_miss_per_tick']:.3f} |" if have_miss else ''
for fm in fams:
    P(f"| {fm} | {og[fm]['M_instr_per_tick']:.2f} | {og[fm]['M_cycles_per_tick']:.2f} |{misses(og[fm])} {hg[fm]['M_instr_per_tick']:.2f} | {hg[fm]['M_cycles_per_tick']:.2f} |{misses(hg[fm])}")
P('\n(HotSpot field/array access counts every instruction of those bytecodes; ours counts only the load opcodes -- the'
  ' address arithmetic and checks around them are under branches/checks/other.)\n')

P('### By position: where a PEBS cycles sample lands (all JIT code / loads only, M cycles per tick)\n')
P('A retirement stall is charged to the oldest unretired instruction: cycles on the first instruction after a CALL are '
  'mostly the return / front-end bubble, on a branch target or function entry mostly fetch, and only MID-BLOCK cycles '
  'are the instruction\'s own (data) stall.\n')
P('| position | ours, all JIT code | ours, loads | HotSpot, all blob code | HotSpot, field/array |\n|---|---|---|---|---|')
po = collections.Counter(); pol = collections.Counter(); ph = collections.Counter(); phl = collections.Counter()
for r in ours:
    if r['kind'] == 'jit':
        po[r.get('pos', '')] += f_(r, 'M_cycles_per_tick')
        if is_our_load(r): pol[r.get('pos', '')] += f_(r, 'M_cycles_per_tick')
for r in hs:
    if r['class'] not in ('hs:unmapped',):
        ph[r.get('pos', '')] += f_(r, 'M_cycles_per_tick')
        if is_hs_mem(r): phl[r.get('pos', '')] += f_(r, 'M_cycles_per_tick')
for pz in ('mid-block', 'after-call', 'branch-target', 'function-entry', ''):
    if po[pz] or ph[pz]:
        P(f'| {pz or "(unclassified)"} | {po[pz]:.2f} | {pol[pz]:.2f} | {ph[pz]:.2f} | {phl[pz]:.2f} |')

if have_miss:
    P('\n### Load misses by cause, all code (M misses per tick)\n')
    P('| ours: cause | L1 miss | L2 miss | L3 miss | cycles | | HotSpot: cause | L1 miss | L2 miss | L3 miss | cycles |')
    P('|---|---|---|---|---|---|---|---|---|---|---|')
    mo_ = collections.defaultdict(collections.Counter); mh_ = collections.defaultdict(collections.Counter)
    for r in ours:
        g = our_group(r)
        for c in ('M_l1_miss_per_tick', 'M_l2_miss_per_tick', 'M_l3_miss_per_tick', 'M_cycles_per_tick'): mo_[g][c] += f_(r, c)
    for r in hs:
        g = hs_group(r)
        for c in ('M_l1_miss_per_tick', 'M_l2_miss_per_tick', 'M_l3_miss_per_tick', 'M_cycles_per_tick'): mh_[g][c] += f_(r, c)
    lo_ = sorted(mo_.items(), key=lambda x: -x[1]['M_l1_miss_per_tick'])[:22]
    lh_ = sorted(mh_.items(), key=lambda x: -x[1]['M_l1_miss_per_tick'])
    def mr(d): return f"{d['M_l1_miss_per_tick']:.3f} | {d['M_l2_miss_per_tick']:.3f} | {d['M_l3_miss_per_tick']:.3f} | {d['M_cycles_per_tick']:.2f}"
    for i in range(max(len(lo_), len(lh_))):
        a_ = lo_[i] if i < len(lo_) else None; b_ = lh_[i] if i < len(lh_) else None
        P(f"| {a_[0] if a_ else ''} | {mr(a_[1]) if a_ else ' | | | '} | | {b_[0] if b_ else ''} | {mr(b_[1]) if b_ else ' | | | '} |")
    to = {c: sum(d[c] for d in mo_.values()) for c in ('M_l1_miss_per_tick', 'M_l2_miss_per_tick', 'M_l3_miss_per_tick')}
    th = {c: sum(d[c] for d in mh_.values()) for c in ('M_l1_miss_per_tick', 'M_l2_miss_per_tick', 'M_l3_miss_per_tick')}
    P(f"| **total** | **{to['M_l1_miss_per_tick']:.3f}** | **{to['M_l2_miss_per_tick']:.3f}** | **{to['M_l3_miss_per_tick']:.3f}** | | | **total** | **{th['M_l1_miss_per_tick']:.3f}** | **{th['M_l2_miss_per_tick']:.3f}** | **{th['M_l3_miss_per_tick']:.3f}** | |")

    P('\n### Cycles that land on fetch boundaries, by cause (M cycles per tick)\n')
    P('| ours: cause | branch-target | function-entry | mid-block |\n|---|---|---|---|')
    pg = collections.defaultdict(collections.Counter)
    for r in ours:
        if r['kind'] == 'jit': pg[our_group(r)][r.get('pos', '')] += f_(r, 'M_cycles_per_tick')
    for g, d in sorted(pg.items(), key=lambda x: -(x[1]['branch-target'] + x[1]['function-entry']))[:14]:
        P(f"| {g} | {d['branch-target']:.2f} | {d['function-entry']:.2f} | {d['mid-block']:.2f} |")

# ---------------------------------------------------------------- leaf callers
lp = ours_p + '-leafcallers.tsv'
if os.path.exists(lp):
    lr = list(rows(lp))
    moved = sum(f_(r, 'M_instr_moved') for r in lr); totf = sum(f_(r, 'M_instr_fp') for r in lr)
    P(f'\n## Frame-pointer-less leaves\n\nSame run, the instructions event recorded twice (fp callchain / LBR call stack). '
      f'**{moved:.3f} of {totf:.3f} M instr/tick of runtime code ({moved/max(totf,1e-9):.1%}) is charged to a different JIT '
      f'caller by the fp chain.** This report uses the LBR attribution throughout. Largest movers:\n')
    P('| symbol | M instr moved | fp total | LBR total |\n|---|---|---|---|')
    for r in lr[:12]:
        P(f"| `{r['symbol'][:90]}` | {f_(r,'M_instr_moved'):.3f} | {f_(r,'M_instr_fp'):.3f} | {f_(r,'M_instr_lbr'):.3f} |")

# ---------------------------------------------------------------- top methods
P(f'\n## Top {top} methods by excess cycles (ours - HotSpot), per tick\n')
P('| method | class | ours M cyc | HS M cyc | excess | ours / HS M instr | where ours goes (top causes, M cyc) |\n|---|---|---|---|---|---|---|')
for k in sorted(keys, key=lambda k: -(tot(O, k) - tot(H, k)))[:top]:
    o, h = tot(O, k), tot(H, k)
    t3 = ', '.join(f'{g.split(": ",1)[-1]} {v:.2f}' for g, v in O[k].most_common(3))
    c = cls_of[k] + (' (idle)' if k in IDLE else '') + (' (HS: inlined)' if cls_of[k] == 'java-ours-only' and k in scope_set else '')
    P(f'| `{k}` | {c} | {o:.3f} | {h:.3f} | {o-h:.3f} | {tot(OI,k):.3f} / {tot(HI,k):.3f} | {t3} |')

txt = '\n'.join(out)
if opt('--out'):
    open(opt('--out'), 'w').write(txt + '\n')
if opt('--tsv'):
    with open(opt('--tsv') + '-bymethod.tsv', 'w') as f:
        f.write('method\tours_M_cyc\ths_M_cyc\tours_M_instr\ths_M_instr\tclass\tmachinery_family\tin_hs_scopes\tours_top_causes\n')
        for k in sorted(keys, key=lambda k: -(tot(O, k) - tot(H, k))):
            f.write(f'{k}\t{tot(O,k):.5f}\t{tot(H,k):.5f}\t{tot(OI,k):.5f}\t{tot(HI,k):.5f}\t{cls_of[k]}\t{machinery_family(k) or ""}\t'
                    f'{int(k in scope_set)}\t' + '; '.join(f'{g} {v:.3f}' for g, v in O[k].most_common(6)) + '\n')
print(txt)
