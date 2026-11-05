#!/usr/bin/env python3
"""j2d/hsledger.py -- the jbox2d ledger for a HOTSPOT arm (j2d/run.mjs hs8 / hs21, set A, with j2d/jvmti/j2dagent).

  hsledger.py <rundir> [--out PREFIX]

Reads <rundir>/j2dagent.txt (the JVMTI agent: every nmethod's code bytes and its inlining-aware pc -> (method, bci)
map; every stub/interpreter blob) and the bench thread's PEBS samples inside window.json's window (counts.py picks the
thread: busiest by cycles). The agent replaces ledger/hsledger.py's -XX:+PrintAssembly, which delays C2 enough to
leave JDK 21 69% outside compiled code in this 1 s window (the agent's own cost is not measurable: 0.36 ms/step).

Each sample is labelled like j2d/wledger.py's rows (kind fn op tag x86 pos):
  kind  hs:body (an nmethod pc with a scope) | hs:entry (bci < 0: IC check, stack bang, frame, sync entry) |
        hs:stub (past the last PcDesc: call/exception/deopt stubs) | hs:blob:<name> (interpreter, stubs, adapters) |
        hs:unmapped (libjvm C++ and anything else)
  fn    the INNERMOST method (where C2 inlined it), op its bytecode's class at the bci, tag the ROOT method (the
        nmethod's own -- the unit a wasm function is, with its inlinees folded in; j2d/report.py joins on it)
  x86   hotinsn.classify of the sampled instruction, disassembled from the copied code bytes
SCOPE: a PcDesc describes the code up to its pc (pc_desc_near), so an instruction takes the first PcDesc with pc > ip.
The nmethod is the one covering ip whose load is the latest at or before the sample, not unloaded before it."""
import bisect, collections, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WJ = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread
from counts import run_counts
import hotinsn

a = sys.argv[1:]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
rundir = a[0]
outp = opt('--out', os.path.join(rundir, 'hsledger'))
cnt0 = run_counts(rundir)
tid = cnt0['tid']
t0, t1 = json.load(open(os.path.join(rundir, 'window.json')))['window']
data = os.path.join(rundir, 'perf.data')

# ---------------------------------------------------------------- the agent's records
names = {}; bcodes = {}
nm_list = []          # [t, addr, size, mid, code(bytes), pcs(list), frames(list), t_unload]
by_addr = {}
stubs = []            # (addr, end, t, name)
cur = None
with open(os.path.join(rundir, 'j2dagent.txt'), errors='replace') as f:
    for line in f:
        c = line[:1]
        if c == 'P':
            _, pc, fr = line.split(' ', 2)
            cur[5].append(int(pc, 16))
            cur[6].append(tuple(tuple(int(v) for v in x.split(':')) for x in fr.strip().split(',')))
        elif c == 'C':
            cur[4] = bytes.fromhex(line[2:].strip())
        elif c == 'M':
            _, t, ad, sz, mid = line.split()
            cur = [int(t) / 1e9, int(ad, 16), int(sz), int(mid), b'', [], [], None]
            nm_list.append(cur); by_addr.setdefault(cur[1], []).append(cur)
        elif c == 'N':
            _, i, n = line.rstrip('\n').split(' ', 2); names[int(i)] = n
        elif c == 'B':
            _, i, h = line.split(); bcodes[int(i)] = bytes.fromhex(h)
        elif c == 'U':
            _, t, ad = line.split()
            for r in by_addr.get(int(ad, 16), []):
                if r[7] is None and r[0] <= int(t) / 1e9:
                    r[7] = int(t) / 1e9
        elif c == 'S':
            _, t, ad, sz, n = line.rstrip('\n').split(' ', 4)
            stubs.append((int(ad, 16), int(ad, 16) + int(sz), int(t) / 1e9, n))
for r in nm_list:   # pcs arrive in PcDesc order; sort defensively, frames with them
    if r[5] != sorted(r[5]):
        z = sorted(zip(r[5], r[6])); r[5] = [p for p, _ in z]; r[6] = [f_ for _, f_ in z]
nm_list.sort(key=lambda r: r[1])
starts = [r[1] for r in nm_list]
stubs.sort()
sstarts = [s[0] for s in stubs]

def nmethod_at(ip, t):
    k = bisect.bisect_right(starts, ip) - 1
    best = None
    for j in range(k, max(k - 64, -1), -1):
        r = nm_list[j]
        if r[1] <= ip < r[1] + r[2] and r[0] <= t and (r[7] is None or r[7] > t) and (best is None or r[0] > best[0]):
            best = r
    return best

def stub_at(ip):
    k = bisect.bisect_right(sstarts, ip) - 1
    for j in range(k, max(k - 16, -1), -1):
        if stubs[j][0] <= ip < stubs[j][1]:
            return stubs[j][3]
    return None

dis = {}
def x86_at(r, off):
    k = id(r)
    d = dis.get(k)
    if d is None:
        fd, tmp = tempfile.mkstemp(suffix='.bin')
        os.write(fd, r[4]); os.close(fd)
        try:
            p = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '--no-show-raw-insn', tmp],
                               capture_output=True)
        finally:
            os.unlink(tmp)
        d = {}
        for ln in p.stdout.decode('utf8', 'replace').split('\n'):
            m = re.match(r'\s+([0-9a-f]+):\s+(.*)$', ln)
            if m:
                s = m.group(2).strip()
                if s and not s.startswith('('):
                    parts = s.split(None, 1)
                    d[int(m.group(1), 16)] = (parts[0], parts[1] if len(parts) > 1 else '')
        dis[k] = d
    v = d.get(off)
    return hotinsn.classify(*v) if v else 'x86?'

def opclass(op):
    if op is None: return ''
    if op <= 0x14: return 'const'
    if op <= 0x2d: return 'local'
    if op <= 0x35: return 'array'
    if op <= 0x4e: return 'local'
    if op <= 0x56: return 'array'
    if op <= 0x5f: return 'other'
    if op <= 0x77: return 'f.arith' if (op - 0x60) % 4 >= 2 else 'i.arith'
    if op <= 0x84: return 'i.arith'
    if op <= 0x93: return 'conv'
    if op <= 0x98: return 'cmp'
    if op <= 0xa7 or op in (0xaa, 0xab, 0xc6, 0xc7, 0xc8): return 'branch'
    if 0xac <= op <= 0xb1: return 'return'
    if 0xb2 <= op <= 0xb5: return 'field'
    if 0xb6 <= op <= 0xba: return 'call'
    if op in (0xbb, 0xbc, 0xbd, 0xc5): return 'alloc'
    if op == 0xbe: return 'array'
    if op in (0xc0, 0xc1): return 'typecheck'
    if op in (0xc2, 0xc3): return 'monitor'
    return 'other'

def short(n):
    """Lorg/jbox2d/collision/AABB;::combine(...)V -> org.jbox2d.collision.AABB::combine"""
    m = re.match(r'^L?([^;]*);?::([^(]*)', n)
    return (m.group(1).replace('/', '.') + '::' + m.group(2)) if m else n

maps = sorted((st, en, os.path.basename(fn)) for _pid, st, en, _po, fn in perfread.mmaps(data) if fn and not fn.startswith('['))
mstarts = [m_[0] for m_ in maps]
def dso_at(ip):
    k = bisect.bisect_right(mstarts, ip) - 1
    return maps[k][2] if k >= 0 and maps[k][0] <= ip < maps[k][1] else ''

def label(ip, t):
    r = nmethod_at(ip, t)
    if r is None:
        s = stub_at(ip)
        return ('hs:blob:' + s if s else 'hs:unmapped', dso_at(ip) if not s else '', '', '', '', '')
    off = ip - r[1]
    x = x86_at(r, off)
    root = short(names.get(r[3], '?'))     # the nmethod's own method: what a wasm FUNCTION is (inlinees folded in)
    k = bisect.bisect_right(r[5], ip)
    if k >= len(r[5]):
        return ('hs:stub', root, '', root, x, '')
    mid, bci = r[6][k][0]
    nm = short(names.get(mid, '?'))
    if bci < 0:
        return ('hs:entry', nm, 'entry', root, x, '')
    bc = bcodes.get(mid)
    op = bc[bci] if bc is not None and bci < len(bc) else None
    return ('hs:body', nm, opclass(op), root, x, '')

at = perfread.attrs(data)
def kind_of(x):
    n = x['name']
    if x['precise'] < 2: return None
    if 'instructions' in n: return 'ins'
    if 'cycles' in n: return 'cyc'
    for lv in ('l1_miss', 'l2_miss', 'l3_miss'):
        if lv in n: return lv
    return None
akind = {i: kind_of(x) for i, x in enumerate(at)}
kinds = sorted({k for k in akind.values() if k})
cnt = {k: collections.Counter() for k in kinds}
for ai, ip, t, stid, chain, lbr in perfread.samples(data, tid=tid, t0=t0, t1=t1, want_chain=False, want_lbr=False):
    k = akind.get(ai)
    if k:
        cnt[k][label(ip, t / 1e9)] += 1
total = {'ins': cnt0['per_step']['instructions'], 'cyc': cnt0['per_step']['cycles'],
         'l1_miss': cnt0['per_step'].get('l1_miss', 0), 'l2_miss': cnt0['per_step'].get('l2_miss', 0),
         'l3_miss': cnt0['per_step'].get('l3_miss', 0)}
def val(kind, key):
    c = cnt.get(kind); s_ = sum(c.values()) if c else 0
    return c.get(key, 0) / s_ * total[kind] if s_ else 0.0
allk = set()
for c in cnt.values(): allk |= set(c)
with open(outp + '-full.tsv', 'w') as f:
    f.write('kind\tfn\top\ttag\tx86\tpos\t' + '\t'.join(f'{k}_per_step' for k in kinds) + '\n')
    for key in sorted(allk, key=lambda k: -cnt['cyc'].get(k, 0)):
        f.write('\t'.join(str(x) for x in key) + '\t' + '\t'.join(f'{val(k, key):.2f}' for k in kinds) + '\n')
json.dump(dict(run=cnt0, nmethods=len(nm_list), stubs=len(stubs), samples={k: sum(v.values()) for k, v in cnt.items()}),
          open(outp + '-summary.json', 'w'), indent=1)
cs = sum(cnt['cyc'].values())
byk = collections.Counter()
for k, v in cnt['cyc'].items(): byk[k[0] if not k[0].startswith('hs:blob:') else 'hs:blob'] += v
print(f'{rundir}: tid {tid}, {len(nm_list)} nmethods, {len(stubs)} blobs; samples '
      f'{ {k: sum(v.values()) for k, v in cnt.items()} }; cycles by kind: '
      + ', '.join(f'{k} {v/cs:.1%}' for k, v in byk.most_common()))
