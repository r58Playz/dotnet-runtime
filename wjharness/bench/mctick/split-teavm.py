#!/usr/bin/env python3
"""split-teavm.py <tvrun> [options] -- the TeaVM arm in OUR call layout, and co-location variants of it (2026-11-19).

Our JIT compiles one wasm module per method: calls between methods go call_indirect through a shared table, calls into
the runtime are function imports, and the stack pointer `s.p` is an imported MUTABLE global (V8: 4 extra loads per
access). This puts TeaVM's code in that layout:
  * every Java function a TeaVM run executed goes into a module of its own (--mode function) or into a group
    (co-location: calls inside a group are module-local `call`s, which V8 can inline; across groups they stay
    call_indirect / imports): --mode class (the methods of one class), edges (greedy merge along the hottest call
    edges), classedges (class groups, then merged along the hottest edges between groups), hot (the hottest functions
    packed together, the rest alone), hotone / hotclassedges (hot's packing of the hottest, the cold rest packed into
    modules in placement order / grouped by class and merged along the edges between cold groups only: the cold
    tail's own grouping, priced against hot with the hot groups identical, 2026-11-22), one (no split at all: TeaVM's
    single module through the same rewrites);
    --cap BYTES bounds a group's wasm bytes (default 1 MiB);
  * --inline-runtime: TeaVM's WasmRuntime.allocStack and supertypeof_* (its shadow-frame allocation and type checks)
    are inlined into those functions first (wasmrw.inline; our JIT emits both inline), so they do not become import
    calls; binaryen's whole-module inliner needs > 10 GB for this;
  * --sp-global: TeaVM's shadow-stack pointer (the word at linear address 3444) becomes a mutable global, defined in
    the primary and imported by every split module, as our `s.p` is (wasmrw.word_to_global; refuses unless every
    access matched);
  * what stays in the primary: TeaVM's runtime, class-init and type-check helpers, never-executed code.

  --sp-value F,..  under --sp-global, functions whose unmatched uses of the address constant are plain values, read
                   by hand (M4 build: meth_nmudf_BlockStateData_bootstrapD passes 3488 as a call argument)
  <tvrun>          a j2d/run set-A, -D or -L run of the FULL arm: its V8 jitdump lists every function V8 compiled; V8
                   compiles wasm lazily, so that is the executed set
  --from DIR       the arm to rewrite (default $WJ_STATE/arms/mctick/teavm-full)
  --out DIR        the result (default $WJ_STATE/arms/mctick/teavm-split; any directory: serve it with
                   `wj j2d/run tvsplit --tv-dir DIR`)
  --edges TSV      call edges for edges / classedges / hot: `caller<TAB>callee<TAB>weight` (lbredges.py)
  --hot TSV        function heat for hot: `name<TAB>weight` (lbredges.py); --hot-share F (default 0.98) of it is packed

The page instantiates the primary, then every split module with the primary's exports, then calls TeaVM's init
(exported as __wj_init: the start section is removed so init cannot call a function not yet loaded) and main."""
import collections, glob, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'lib')); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
sys.path.insert(0, HERE)
from jitindex import JitIndex  # noqa: E402
import wasmrw  # noqa: E402

a = sys.argv[1:]
if not a or a[0].startswith('-'):
    sys.exit(__doc__)
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
state = subprocess.run(['node', '-e', 'import("' + os.path.join(HERE, '..', '..', 'lib', 'paths.mjs') + '").then(m => console.log(m.cfg("WJ_STATE")))'],
                       capture_output=True, text=True).stdout.strip()
SRC = opt('--from', os.path.join(state, 'arms', 'mctick', 'teavm-full'))
OUT = opt('--out', os.path.join(state, 'arms', 'mctick', 'teavm-split'))
MODE = opt('--mode', 'function')
CAP = int(opt('--cap', str(1 << 20)))
RUN = a[0]
FEATURES = ['--mvp-features', '--enable-exception-handling', '--enable-reference-types', '--enable-bulk-memory',
            '--enable-sign-ext', '--enable-nontrapping-float-to-int', '--enable-mutable-globals']   # what V8 runs: not --all-features (binaryen 130 then emits `exact` imports)
KEEP = re.compile(r'^(meth_otr_|meth_otbw|teavm_|initclass_|supertypeof_|start$|_start$)')
SP_ADDR = None      # TeaVM's WasmRuntime stack-pointer word: read off allocStack below (3444 in the x86 box's build)
assert MODE in ('function', 'class', 'edges', 'classedges', 'hot', 'hotone', 'hotclassedges', 'one'), MODE

# the executed set
jd = max(glob.glob(os.path.join(RUN, 'jit-*.dump')), key=os.path.getsize)
idx = JitIndex(jd)
executed = set()
for r in range(idx.n):
    m = re.match(r'JS:(.*)-\d+-(turbofan|liftoff)$', idx.name(r))
    if m:
        executed.add(m.group(1))
mod = wasmrw.Module(open(os.path.join(SRC, 'wasm', 'mctick.wasm'), 'rb').read())
byname = {n: i for i, n in mod.names.items()}
split = sorted(n for n in executed if n.startswith('meth_') and not KEEP.match(n) and n in byname)
log = [f'split-teavm.py {" ".join(a)}', f'from {SRC}', f'executed set: {jd} ({len(executed)} functions, {len(split)} Java functions to place)']

# start section -> export __wj_init
st = mod.section(8)
sidx, _ = wasmrw.uleb(st[1], 0)
mod.sections.remove(st)
ex = mod.section(7)
n, o = wasmrw.uleb(ex[1], 0)
ex[1] = wasmrw.enc_u(n + 1) + ex[1][o:] + wasmrw.enc_u(9) + b'__wj_init' + b'\x00' + wasmrw.enc_u(sidx)

alloc = byname['meth_otbw_WasmRuntime_allocStack']
# the stack-pointer word, read off allocStack itself: `i32.const ADDR; i32.load` first
_, ai = wasmrw.decode(mod.bodies[alloc - mod.nfimports])
assert ai[0][0] == 0x41 and ai[1][0] == 0x28, 'allocStack no longer starts with the SP load'
SP_ADDR = ai[0][1]
log.append(f'stack-pointer word: {SP_ADDR}')
if '--inline-runtime' in a:
    callees = {alloc} | {i for n_, i in byname.items() if n_.startswith('supertypeof_') and i >= mod.nfimports}
    ch, sites = wasmrw.inline(mod, callees, [byname[n_] for n_ in split])
    log.append(f'inline-runtime: allocStack + {len(callees) - 1} supertypeof_* inlined at {sites} call sites in {ch} functions')
if '--sp-global' in a:
    vf = set(opt('--sp-value', '').split(',')) - {''}   # functions whose other uses of the address constant were read as values
    g, nl, ns, refused = wasmrw.word_to_global(mod, SP_ADDR, vf)
    log.append(f'sp-global: word {SP_ADDR} -> global {g}: {nl} loads, {ns} stores rewritten, {refused} refused'
               + (f' (uses read as values in: {",".join(sorted(vf))})' if vf else ''))
    assert refused == 0, 'an access to the stack-pointer word did not match: refusing'

if os.path.exists(OUT):
    shutil.rmtree(OUT)
os.makedirs(os.path.join(OUT, 'wasm'))
rewritten = os.path.join(OUT, 'wasm', 'rewritten.wasm')
open(rewritten, 'wb').write(mod.encode())
chk = subprocess.run(['node', '-e', f'new WebAssembly.Module(require("fs").readFileSync("{rewritten}"))'], capture_output=True, text=True)
assert chk.returncode == 0, 'the rewritten module does not validate: ' + chk.stderr[-400:]

# placement
size = {n_: len(mod.bodies[byname[n_] - mod.nfimports]) for n_ in split}
def cls_of(n_):
    t = n_.split('_')[2:]          # drop meth, package abbreviation
    k = max((i for i, x in enumerate(t) if x[:1].isupper()), default=0)
    return '_'.join(t[:k + 1])
def read_tsv(p, cols):
    out = []
    for line in open(p):
        f = line.rstrip('\n').split('\t')
        if len(f) >= cols:
            out.append(f)
    return out
groups = []
if MODE == 'function':
    groups = [[n_] for n_ in split]
elif MODE == 'class' or MODE == 'classedges':
    by = collections.defaultdict(list)
    for n_ in split:
        by[cls_of(n_)].append(n_)
    for k, ms in sorted(by.items()):
        cur, cb = [], 0
        for n_ in sorted(ms, key=lambda x: -size[x]):
            if cur and cb + size[n_] > CAP:
                groups.append(cur); cur, cb = [], 0
            cur.append(n_); cb += size[n_]
        groups.append(cur)
elif MODE == 'edges':
    groups = [[n_] for n_ in split]
elif MODE in ('hot', 'hotone', 'hotclassedges'):
    heat = collections.Counter()
    for f in read_tsv(opt('--hot'), 2):
        heat[f[0][len('trampoline_'):] if f[0].startswith('trampoline_') else f[0]] += float(f[1])
    tot = sum(heat.get(n_, 0) for n_ in split); share = float(opt('--hot-share', '0.98'))
    hot, acc = [], 0.0
    for n_ in sorted(split, key=lambda x: -heat.get(x, 0)):
        if acc >= share * tot or heat.get(n_, 0) <= 0:
            break
        hot.append(n_); acc += heat.get(n_, 0)
    cur, cb = [], 0
    for n_ in hot:
        if cur and cb + size[n_] > CAP:
            groups.append(cur); cur, cb = [], 0
        cur.append(n_); cb += size[n_]
    if cur:
        groups.append(cur)
    hs = set(hot)
    nhot_groups = len(groups)
    cold = [n_ for n_ in split if n_ not in hs]
    if MODE == 'hot':
        groups += [[n_] for n_ in cold]
    elif MODE == 'hotone':
        cur, cb = [], 0
        for n_ in cold:
            if cur and cb + size[n_] > CAP:
                groups.append(cur); cur, cb = [], 0
            cur.append(n_); cb += size[n_]
        if cur:
            groups.append(cur)
    else:
        by = collections.defaultdict(list)
        for n_ in cold:
            by[cls_of(n_)].append(n_)
        for k, ms in sorted(by.items()):
            cur, cb = [], 0
            for n_ in sorted(ms, key=lambda x: -size[x]):
                if cur and cb + size[n_] > CAP:
                    groups.append(cur); cur, cb = [], 0
                cur.append(n_); cb += size[n_]
            groups.append(cur)
    log.append(f'{MODE}: {len(hot)} functions hold {100 * acc / max(tot, 1):.1f}% of the profiled heat, packed in {nhot_groups} modules; '
               f'{len(cold)} cold functions in {len(groups) - nhot_groups} modules')
if MODE in ('edges', 'classedges', 'hotclassedges'):
    # greedy union along the heaviest edges, capped (hotclassedges: between cold groups only; the hot groups are hot's)
    frozen = set(range(nhot_groups)) if MODE == 'hotclassedges' else set()
    gid = {}
    for i, g_ in enumerate(groups):
        for n_ in g_:
            gid[n_] = i
    members = {i: list(g_) for i, g_ in enumerate(groups)}
    gbytes = {i: sum(size[n_] for n_ in g_) for i, g_ in enumerate(groups)}
    w = collections.Counter()
    for f in read_tsv(opt('--edges'), 3):
        x_, y_ = (n_[len('trampoline_'):] if n_.startswith('trampoline_') else n_ for n_ in f[:2])   # a profiled split layout's vtable trampolines
        if x_ in gid and y_ in gid and x_ != y_:
            w[(x_, y_)] += float(f[2])
    merges = 0
    for (x, y), wt in sorted(w.items(), key=lambda kv: -kv[1]):
        gx, gy = gid[x], gid[y]
        if gx == gy or gx in frozen or gy in frozen or gbytes[gx] + gbytes[gy] > CAP:
            continue
        if len(members[gx]) < len(members[gy]):
            gx, gy = gy, gx
        for n_ in members[gy]:
            gid[n_] = gx
        members[gx] += members.pop(gy); gbytes[gx] += gbytes.pop(gy); merges += 1
    groups = list(members.values())
    log.append(f'{MODE}: {len(w)} profiled edges between placed functions, {merges} merges under a {CAP} B cap')

if MODE == 'one':
    os.replace(rewritten, os.path.join(OUT, 'wasm', 'mctick.wasm'))
    index = []
    open(os.path.join(OUT, 'wasm', 'secondaries.bin'), 'wb').close()
    log.append('one: no split (the rewritten single module)')
else:
    sec = os.path.join(OUT, 'wasm', 'sec'); os.makedirs(sec)
    with open(os.path.join(OUT, 'wasm', 'manifest.txt'), 'w') as f:
        for k, g_ in enumerate(groups):
            f.write(f'm{k}\n' + ''.join(n_ + '\n' for n_ in g_) + '\n')
    subprocess.run(['wasm-split', '--multi-split', rewritten, '--manifest', os.path.join(OUT, 'wasm', 'manifest.txt'),
                    '--out-prefix', sec + '/', '-o', os.path.join(OUT, 'wasm', 'mctick.wasm'), '--no-placeholders',
                    '--import-namespace', 'primary', '-g', *FEATURES], check=True)
    os.unlink(rewritten)
    # wasm-split leaves a trampoline in every vtable (table 0) slot of a split function (`call_indirect` through its own
    # table), so a virtual call would make two calls; ours makes one. Each split module writes its functions into
    # table 0 at their original slots instead (active segments: secondaries are instantiated after the primary).
    pslots = wasmrw.table0_slots(mod)
    placed = set(split)
    slots_by_name = {mod.names[f]: v for f, v in pslots.items() if mod.names.get(f) in placed}
    index, off, installed = [], 0, 0
    with open(os.path.join(OUT, 'wasm', 'secondaries.bin'), 'wb') as b:
        for k in range(len(groups)):
            p = os.path.join(sec, f'm{k}.wasm')
            blob, nslot = wasmrw.install_in_table(open(p, 'rb').read(), slots_by_name, 'primary', 'table')
            installed += nslot
            b.write(blob); index.append([off, len(blob)]); off += len(blob)
            os.unlink(p)
    log.append(f'vtable slots installed by split modules: {installed} (of {sum(len(v) for v in slots_by_name.values())} for placed functions)')
    os.rmdir(sec)
    sizes = sorted(len(g_) for g_ in groups)
    log.append(f'{len(groups)} split modules (functions per module: median {sizes[len(sizes) // 2]}, max {sizes[-1]}; '
               f'{sum(1 for s in sizes if s == 1)} singletons), {off} B')
json.dump(index, open(os.path.join(OUT, 'wasm', 'secondaries.json'), 'w'))

# loader: TeaVM's runtime JS + loadSplit; the page calls it instead of load
rt = open(os.path.join(SRC, 'wasm', 'mctick.wasm-runtime.js')).read()
anchor = '    return { JavaError, load, loader };'
assert anchor in rt, 'TeaVM runtime JS changed: no export line'
rt = rt.replace(anchor, '''    // wj (split-teavm.py): the primary, then one instance per split module, then TeaVM's init, then main
    async function loadSplit(path, options) {
        if (!options) {
            options = {};
        }
        const importObj = {};
        const controller = defaults(importObj);
        if (typeof options.installImports !== "undefined") {
            options.installImports(importObj, controller);
        }
        const base = path.replace(/[^/]*$/, "");
        const [obj, bin, index] = await Promise.all([
            WebAssembly.instantiateStreaming(fetch(path), importObj),
            fetch(base + "secondaries.bin").then(r => r.arrayBuffer()).then(b => new Uint8Array(b)),
            fetch(base + "secondaries.json").then(r => r.json())]);
        const t0 = performance.now();
        const imports = { primary: obj.instance.exports };
        for (const [off, len] of index) {
            new WebAssembly.Instance(new WebAssembly.Module(bin.subarray(off, off + len)), imports);
        }
        console.log("[mctick] page: " + index.length + " split modules instantiated in " + Math.round(performance.now() - t0) + " ms");
        controller.instance = obj.instance;
        obj.instance.exports.__wj_init();
        let teavm = createTeaVM(obj.instance);
        teavm.main = createMain(teavm, controller);
        return teavm;
    }

    return { JavaError, load, loader, loadSplit };''')
open(os.path.join(OUT, 'wasm', 'mctick.wasm-runtime.js'), 'w').write(rt)
page = open(os.path.join(SRC, 'mctick.html')).read()
assert 'TeaVM.wasm.load("wasm/mctick.wasm"' in page
page = page.replace('TeaVM.wasm.load("wasm/mctick.wasm"', 'TeaVM.wasm.loadSplit("wasm/mctick.wasm"').replace(
    '<title>mctick, TeaVM linear-memory wasm</title>', f'<title>mctick, TeaVM linear-memory wasm, {MODE} placement</title>')
open(os.path.join(OUT, 'mctick.html'), 'w').write(page)
os.link(os.path.join(SRC, 'mctick-data.zip'), os.path.join(OUT, 'mctick-data.zip'))
log.append(f'primary {os.path.getsize(os.path.join(OUT, "wasm", "mctick.wasm"))} B')
open(os.path.join(OUT, 'BUILD.txt'), 'w').write('\n'.join(log) + '\n')
print('\n'.join(log))
