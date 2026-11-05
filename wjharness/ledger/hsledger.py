#!/usr/bin/env python3
"""hsledger.py -- the gap ledger, HotSpot side.

  hsledger.py <natrun OUT dir> --ticks N [--out PREFIX]

Needs a natrun arm with
  EXTRA_JAVA="-XX:+UnlockDiagnosticVMOptions -XX:+PrintAssembly -XX:+DebugNonSafepoints" LD_LIBRARY_PATH=$WJ_STATE/native/asm
  PERF_RECORD=1   (server-thread PEBS instructions + LBR cycles, the same events as ledger.py)
Every sampled instruction is mapped onto the PrintAssembly blob that holds it by ABSOLUTE address (the printed
addresses are the live code, R308) and labelled with its INNERMOST scope (the method whose bytecode it executes,
wherever C2 inlined it), bci, source line and the bytecode HotSpot prints, plus an overhead class:
  hs:entry (before the first scoped instruction), hs:poll ({poll}/{poll_return}), hs:stub ([Stub Code] /
  [Exception Handler] / [Deopt Handler Code]), hs:noscope (scope inherited from the nearest preceding instruction).
Samples outside any blob (libjvm, interpreter, itable/vtable stubs, adapters) are 'hs:unmapped' and charged to the
blob call site that entered them (the newest LBR branch whose source is in a blob).

MEMORY: a full PrintAssembly log is GBs / tens of millions of instructions. Pass 1 collects every sampled address
(IPs and LBR endpoints) from perf.data; pass 2 streams the log and keeps a blob only if one of those addresses
falls inside it, as two compact arrays (addresses, interned label ids). Blob address reuse: the LAST printed blob
covering an address wins (compiles print in order; the window is at the end of the run)."""
import array, bisect, collections, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import perfread

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
out = a[0]
outp = opt('--out', os.path.join(out, 'hsledger'))
ticks = float(opt('--ticks'))
data = os.path.join(out, 'perf.data')

# ---------------------------------------------------------------- pass 1: sampled addresses
addrs = set()
for ai, ip, t, tid, chain, lbr in perfread.samples(data, want_chain=False):
    addrs.add(ip)
    if lbr:
        for fr, to, cy, ty in lbr:
            addrs.add(fr); addrs.add(to)
sa = sorted(addrs); del addrs
print(f'{len(sa)} distinct sampled addresses', flush=True)

# ---------------------------------------------------------------- pass 2: blobs
# Blob headers: full -XX:+PrintAssembly prints `Decoding compiled method 0x...:` and names the method on a later
# `  # {method} {0x...} 'name' '(sig)' in 'pkg/Class'` line; -XX:CompileCommand=print prints `Compiled method (c2) ...`.
hdr = re.compile(r'^(?:Decoding compiled method 0x[0-9a-f]+:|Compiled method \((c\d)\)\s+\d+\s+\d+\s+\S*\s+\d\s+(\S+))')
mline = re.compile(r"^\s+# \{method\} \{0x[0-9a-f]+\} '([^']+)' '([^']*)' in '([^']+)'")
ins = re.compile(r'^\s+0x([0-9a-f]+): (\S+)\s*([^;]*?)\s*(?:;(.*))?$')
scope = re.compile(r'^;\s*-\s+(\S+)@(\d+)(?:\s+\(line (\d+)\))?')
bcre = re.compile(r'\*(\w+)')
labels = {}; lab_list = []
def intern(l):
    i = labels.get(l)
    if i is None:
        i = labels[l] = len(lab_list); lab_list.append(l)
    return i

blobs = []          # (lo, hi, name, addr array, label array, position-class array)
scope_methods = set()   # every method at ANY inlining depth in a sampled blob: "HotSpot runs it here, inlined"

POS = {0: 'mid-block', 1: 'after-call', 2: 'branch-target', 3: 'function-entry'}
hexaddr = re.compile(r'0x([0-9a-f]{6,})')
def finish(cur):
    if not cur or not cur['a']:
        return
    lo, hi = cur['a'][0], cur['a'][-1] + 16
    k = bisect.bisect_left(sa, lo)
    if k >= len(sa) or sa[k] >= hi:
        return      # nothing sampled in this blob
    raw = cur['raw']; n = len(raw)
    # SCOPE OF AN UNANNOTATED INSTRUCTION = the NEXT annotated one. A PcDesc describes the code up to its pc
    # (HotSpot's pc_desc_near returns the first PcDesc at or after an address), and PrintAssembly prints it after
    # the instruction that ends at that pc. The first version inherited BACKWARDS, which folded a small inlined
    # callee into whatever preceded it (57% of g1's sampled HotSpot instructions carry no annotation of their own).
    nxt = [None] * n; cur_sc = None
    for i in range(n - 1, -1, -1):
        sec, sc, cm, mn, opnd = raw[i]
        if sc:
            mb = bcre.search(cm)
            cur_sc = sc + ((mb.group(1) if mb else None),)
        nxt[i] = cur_sc
    seen_scope = False
    lab = array.array('I'); pcl = array.array('B')
    addr_ix = {a: i for i, a in enumerate(cur['a'])}
    after_call = set(); targets = set()
    for i, (sec, sc, cm, mn, opnd) in enumerate(raw):
        if mn.startswith('call') and i + 1 < n:
            after_call.add(i + 1)
        elif mn.startswith('j'):
            mt_ = hexaddr.search(opnd)
            if mt_:
                t_ = addr_ix.get(int(mt_.group(1), 16))
                if t_ is not None:
                    targets.add(t_)
    for i, (sec, sc, cm, mn, opnd) in enumerate(raw):
        pcl.append(3 if 'Verified Entry Point' in sec and i and 'Verified Entry Point' not in raw[i - 1][0]
                   else 1 if i in after_call else 2 if i in targets else 0)
        if 'Stub Code' in sec or 'Exception Handler' in sec or 'Deopt Handler' in sec:
            lab.append(intern(('hs:stub', cur['name'], -1, -1, None))); continue
        if sc:
            seen_scope = True
        if '{poll' in cm:
            s_ = nxt[i]
            lab.append(intern(('hs:poll', s_[0] if s_ else cur['name'], -1, -1, None))); continue
        if not seen_scope and not sc:
            # before the method's first PcDesc: the inline-cache check, stack bang, frame setup
            lab.append(intern(('hs:entry', cur['name'], -1, -1, None))); continue
        s_ = nxt[i]
        if s_ is None:
            lab.append(intern(('hs:noscope', cur['name'], -1, -1, None))); continue
        lab.append(intern(('hs:body' if sc else 'hs:noscope', s_[0], s_[1], s_[2], s_[3])))
    blobs.append((lo, hi, cur['name'], cur['a'], lab, pcl))
    scope_methods.update(cur['scopes'])

cur = None; last = None; section = ''; nblob = 0
with open(os.path.join(out, 'game.log'), errors='replace') as f:
    for line in f:
        m = hdr.match(line)
        if m:
            finish(cur); nblob += 1
            cur = dict(name=m.group(2) or '?', a=array.array('Q'), raw=[], scopes=set()); last = None; section = ''; continue
        if cur is None:
            continue
        if cur['name'] == '?' and '{method}' in line:
            mm = mline.match(line)
            if mm:
                cur['name'] = mm.group(3).replace('/', '.') + '::' + mm.group(1)
            continue
        if line.startswith('['):
            section = line.strip(); continue
        m = ins.match(line)
        if m:
            ad = int(m.group(1), 16)
            if cur['a'] and ad < cur['a'][-1]:
                finish(cur); cur = None; continue   # address went backwards: the blob ended without a header
            cur['a'].append(ad)
            last = [section, None, m.group(4) or '', m.group(2), m.group(3) or '']
            cur['raw'].append(last); continue
        s = line.strip()
        if last is not None and s.startswith(';'):
            ms = scope.match(s)
            if ms:
                cur['scopes'].add(ms.group(1))
                if last[1] is None:   # innermost scope is listed first
                    last[1] = (ms.group(1), int(ms.group(2)), int(ms.group(3)) if ms.group(3) else -1)
            else:
                last[2] += ' ' + s[1:]
        elif s and not s.startswith('0x') and last is not None and cur and len(cur['raw']) > 2 and line[:1] not in ' \t;[':
            finish(cur); cur = None; last = None   # non-assembly output: the blob is over
    finish(cur)
del sa
print(f'{nblob} blobs printed, {len(blobs)} hold a sampled address; {len(lab_list)} distinct labels', flush=True)
order = sorted(range(len(blobs)), key=lambda i: blobs[i][0])
starts = [blobs[i][0] for i in order]

def blob_at(ip):
    k = bisect.bisect_right(starts, ip) - 1
    best = None
    for j in range(k, max(k - 8, -1), -1):
        bi = order[j]
        lo, hi = blobs[bi][0], blobs[bi][1]
        if lo <= ip < hi and (best is None or bi > best):
            best = bi
    return best

def label(bi, ip):
    b = blobs[bi]
    k = bisect.bisect_right(b[3], ip) - 1
    if k < 0:
        return ('hs:entry', b[2], -1, -1, None, 'function-entry')
    return lab_list[b[4][k]] + (POS[b[5][k]],)

CALL_STACK = 1 << 11
def caller(lbr):
    """the blob call site that entered unmapped code: with an LBR CALL STACK, entry.from is the call instruction"""
    for fr, to, cy, ty in (lbr or ()):
        bi = blob_at(fr)
        if bi is not None:
            return label(bi, fr)
    return None

at = perfread.attrs(data)
def kind_of(x):
    n = x['name']
    if 'instructions' in n: return 'ins'
    if 'cycles' in n and x['precise'] >= 2: return 'cyc'
    for lv in ('l1_miss', 'l2_miss', 'l3_miss'):
        if lv in n: return lv
    return None
akind = {i: kind_of(x) for i, x in enumerate(at)}
kinds = sorted({k for k in akind.values() if k})
print('events:', ', '.join(f"{at[i]['name']} -> {k}" for i, k in akind.items() if k), flush=True)
period = {i: at[i]['period'] for i in range(len(at))}
cnt = {k: collections.Counter() for k in kinds}; nsamp = collections.Counter()
for ai, ip, t, tid, chain, lbr in perfread.samples(data, want_chain=False):
    k = akind.get(ai)
    if not k:
        continue
    nsamp[ai] += 1
    bi = blob_at(ip)
    if bi is not None:
        cnt[k][label(bi, ip)] += 1
    else:
        c = caller(lbr) if at[ai]['bstype'] & CALL_STACK else None
        cnt[k][('hs:unmapped', c[1] if c else None, -1, -1, None, '')] += 1

def total(kind): return sum(nsamp[i] * period[i] for i, k in akind.items() if k == kind)
per_tick = {k: total(k) / ticks for k in kinds}
def val(kind, key):
    c = cnt.get(kind)
    if not c: return 0.0
    return c.get(key, 0) / (sum(c.values()) or 1) * per_tick[kind] / 1e6
insc = cnt.get('ins', collections.Counter()); cyc = cnt.get('cyc', collections.Counter())
print(f'ticks {ticks:.0f}; per tick: ' + ', '.join(f'{k} {v/1e6:.3f} M' for k, v in per_tick.items())
      + f'; unmapped instr share {sum(v for k,v in insc.items() if k[0]=="hs:unmapped")/(sum(insc.values()) or 1):.1%}')
cls_i = collections.Counter(); cls_c = collections.Counter()
for k in set(insc) | set(cyc):
    cls_i[k[0]] += val('ins', k); cls_c[k[0]] += val('cyc', k)
print('by class (M instr/tick, M cycles/tick):')
for k in sorted(cls_i, key=lambda k: -cls_i[k]):
    print(f'  {cls_i[k]:7.3f} {cls_c[k]:7.3f}  {k}')
extra = [k for k in ('l1_miss', 'l2_miss', 'l3_miss') if k in cnt]
allk = set()
for c in cnt.values(): allk |= set(c)
with open(outp + '-full.tsv', 'w') as f:
    f.write('class\tM_instr_per_tick\tM_cycles_per_tick\tmethod\tbci\tline\tbytecode\tpos'
            + ''.join(f'\tM_{e}_per_tick' for e in extra) + '\n')
    for k in sorted(allk, key=lambda k: -cyc.get(k, 0)):
        f.write(f'{k[0]}\t{val("ins", k):.5f}\t{val("cyc", k):.5f}\t{k[1]}\t{k[2]}\t{k[3]}\t{k[4]}\t{k[5]}'
                + ''.join(f'\t{val(e, k):.5f}' for e in extra) + '\n')
with open(outp + '-scopes.txt', 'w') as f:
    f.write('\n'.join(sorted(scope_methods)) + '\n')
json.dump(dict(ticks=ticks, per_tick=per_tick, instr_per_tick=per_tick.get('ins', 0), cycles_per_tick=per_tick.get('cyc', 0),
               blobs=len(blobs)), open(outp + '-summary.json', 'w'), indent=1)
meth_c = collections.Counter()
for k in allk: meth_c[k[1]] += val('cyc', k)
print('top methods by cycles:')
for mth, v in meth_c.most_common(20):
    print(f'  {v:7.3f}  {mth}')
