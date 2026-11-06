#!/usr/bin/env python3
"""d8x86.py -- compile captured modules with the version-matched d8 and record, per TurboFan function, its x86 bytes
and the pc -> wasm-offset table.

  d8x86.py <outdir> [--d8 PATH] [--only names.txt] [-j N]

Reads <outdir>/mods/*.wasm (capture.py), writes <outdir>/x86/<sha16>.pkl: {func_index: {'name','code' (bytes),
'insns': [(pc, len)], 'pos': [(pc, wasm_off)]}}. `wasm_off` is relative to the function body start (the local
declarations), which is also wj.origin's base (verified against p45 readLock: every position lands on a
load/store/call opcode).

The d8 must be the browser's exact V8 (read it off the binary: `strings <chromium> | grep -E '^1[0-9][.][0-9]+[.][0-9]+[.][0-9]+$'`;
live code only matches its own version) patched with --wasm-all-source-positions
($WJ_STATE/patches/v8-wasm-all-source-positions.patch, 3 lines), which makes instruction selection record a position
for EVERY instruction instead of only calls and trapping memory accesses. Build: a gclient checkout of
v8.git@refs/tags/<version>, the patch, `gn gen out/rel` with is_debug=false v8_enable_disassembler=true, `autoninja d8`.
The browser updates under you; when it does, wledger's `layout_mismatch` climbs and the d8 needs rebuilding.
--single-threaded keeps print output in compile order. --only restricts to modules defining one of the listed
function names (the sampled set), which is what keeps this minutes instead of an hour."""
import array, os, pickle, re, subprocess, sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import wasmmod

D8 = paths.cfg('D8')   # a release d8 matching the browser's V8 (machine.env); --d8 overrides
# --no-wasm-inlining: with Liftoff off, TurboFan inlines module-local direct calls with NO feedback, on a static budget
# (`set_no_liftoff_inlining_budget(InliningTree::NoLiftoffBudget(..))` under `!v8_flags.liftoff`,
# v8/src/wasm/turboshaft-graph-interface.cc ~380). The browser runs with Liftoff on and never takes that path; it inlines
# only from Liftoff feedback, which d8 cannot have. Without the flag every co-located group's functions came out up to
# 4x larger than the live code and failed the layout gate (46 of 208 on R526's jbox2d run). The functions the browser
# DID inline from feedback still fail the gate and read module-unmatched, which is the correct outcome.
FLAGS = ['--single-threaded', '--print-wasm-code', '--wasm-all-source-positions', '--no-liftoff',
         '--no-wasm-lazy-compilation', '--no-wasm-tier-up', '--no-wasm-inlining']
JS = r'''
for (let i = 0; i < arguments.length; ++i) {
  const f = arguments[i];
  print("@@MODULE " + f);
  try { new WebAssembly.Module(readbuffer(f)); print("@@OK " + f); }
  catch (e) { print("@@ERROR " + f + " " + e); }
}
'''
ins_re = re.compile(r'^0x[0-9a-f]+\s+([0-9a-f]+)\s+([0-9a-f]+)\s')
pos_re = re.compile(r'^\s+([0-9a-f]+)\s+(\d+)(\s+statement)?\s*$')


def parse_stream(lines):
    """Yield one dict per compiled function, streaming (a module's print output can be hundreds of MB):
    {'name','index','compiler','code' (bytes), 'ipc','ilen' (instruction pcs/lengths), 'ppc','ppos' (position
    table)} as compact arrays. Jump-table entries are DATA: 8-byte absolute addresses printed as their VALUE;
    they get placeholder bytes so offsets stay exact and are never compared or counted."""
    fn = None; sect = None
    def done(f):
        f['code'] = bytes(f['code'])
        return f
    for line in lines:
        if line.startswith('name: '):
            if fn is not None and fn['compiler']:
                yield done(fn)
            fn = {'name': line[6:].rstrip('\n'), 'code': bytearray(), 'ipc': array.array('I'), 'ilen': array.array('B'),
                  'ppc': array.array('I'), 'ppos': array.array('I'), 'compiler': None, 'index': None}
            sect = None; continue
        if fn is None:
            continue
        if line.startswith('index: '):
            fn['index'] = int(line[7:]); continue
        if line.startswith('compiler: '):
            fn['compiler'] = line[10:].strip(); continue
        if line.startswith('Instructions (size'):
            sect = 'ins'; continue
        if line.startswith('Source positions:'):
            sect = 'pos'; continue
        if sect == 'ins':
            m = ins_re.match(line)
            if m:
                pc = int(m.group(1), 16)
                if pc > len(fn['code']):
                    fn['code'] += b'\0' * (pc - len(fn['code']))
                if 'jump table entry' in line:
                    fn['code'] += b'\0' * 8
                    continue
                try:
                    hx = bytes.fromhex(m.group(2))
                except ValueError:
                    continue
                fn['ipc'].append(pc); fn['ilen'].append(len(hx))
                fn['code'] += hx
            elif not line.startswith('0x'):
                sect = None
            continue
        if sect == 'pos':
            m = pos_re.match(line)
            if m:
                fn['ppc'].append(int(m.group(1), 16)); fn['ppos'].append(int(m.group(2)))
            elif line.strip() == '' and len(fn['ppc']):
                sect = None
    if fn is not None and fn['compiler']:
        yield done(fn)


def run_batch(args):
    paths, d8, outdir = args
    js = os.path.join(outdir, 'd8drv.js')
    n_ok = n_err = 0
    for path in paths:       # one module per d8: bounded memory, and one bad module cannot take its batch with it
        sha = os.path.basename(path)[:-5]
        p = subprocess.Popen([d8] + FLAGS + [js, '--', path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True, errors='replace', bufsize=1 << 20)
        tf = {}
        for f in parse_stream(p.stdout):
            if f['compiler'] == 'TurboFan' and f['index'] is not None:
                tf[f['index']] = f
        rc = p.wait()
        if rc != 0 or not tf:
            n_err += 1
            with open(os.path.join(outdir, 'x86', sha + '.err'), 'w') as g:
                g.write(f'd8 rc={rc}, {len(tf)} TurboFan functions')
            continue
        with open(os.path.join(outdir, 'x86', sha + '.pkl'), 'wb') as g:
            pickle.dump(tf, g)
        n_ok += 1
    return n_ok, n_err


def main():
    a = sys.argv[1:]
    outdir = a[0]
    d8 = a[a.index('--d8') + 1] if '--d8' in a else D8
    jobs = int(a[a.index('-j') + 1]) if '-j' in a else 3
    only = None
    if '--only' in a:
        # 'entry' (every module's thunk) and 'wasm-function[N]' (unnamed lazy-bank stubs) name nothing unique: keeping
        # them made --only admit 14,063 of 21,886 modules (g1), including the giant ones
        only = set(l.rstrip('\n') for l in open(a[a.index('--only') + 1])
                   if l.strip() and l.strip() != 'entry' and not l.startswith('wasm-function['))
    os.makedirs(os.path.join(outdir, 'x86'), exist_ok=True)
    with open(os.path.join(outdir, 'd8drv.js'), 'w') as f:
        f.write(JS)
    mods = sorted(os.path.join(outdir, 'mods', x) for x in os.listdir(os.path.join(outdir, 'mods')) if x.endswith('.wasm'))
    todo = []
    for p in mods:
        sha = os.path.basename(p)[:-5]
        if os.path.exists(os.path.join(outdir, 'x86', sha + '.pkl')):
            continue
        if only is not None:
            try:
                m = wasmmod.parse(open(p, 'rb').read())
            except Exception:
                continue
            if not any(nm in only for nm in m.names.values()):
                continue
        todo.append(p)
    print(f'{len(mods)} modules, {len(todo)} to compile', flush=True)
    # MEMORY: TurboFan on one huge function can take GBs (a d8 died at the 4 GiB unit cap on g1). Small modules first
    # in parallel; anything over BIG bytes one at a time, smallest first.
    BIG = 200_000
    todo.sort(key=os.path.getsize)
    small = [p for p in todo if os.path.getsize(p) <= BIG]; big = [p for p in todo if os.path.getsize(p) > BIG]
    batches = [small[i:i + 12] for i in range(0, len(small), 12)]
    tok = terr = 0
    with ProcessPoolExecutor(jobs) as ex:
        for k, (ok, err) in enumerate(ex.map(run_batch, [(b, d8, outdir) for b in batches])):
            tok += ok; terr += err
            if k % 50 == 0:
                print(f'  {k}/{len(batches)} batches: ok {tok} err {terr}', flush=True)
    for p in big:
        ok, err = run_batch(([p], d8, outdir)); tok += ok; terr += err
        print(f'  big {os.path.basename(p)} {os.path.getsize(p)} B: ok {ok} err {err}', flush=True)
    print(f'compiled ok {tok}, errors {terr}')


if __name__ == '__main__':
    main()
