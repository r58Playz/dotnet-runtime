#!/usr/bin/env python3
"""j2d/cjledger.py -- a code-kind ledger for ANY browser arm of j2d/run.mjs (written for CheerpJ, 2026-11-04): every
sample of the bench thread inside the window, per step, by what V8 code it landed in, read off the jitdump names alone.
wledger.py is the deep ledger for wasm arms (module capture, d8 layout gate, wasm-op classes); this one needs no capture
and so also reads a JavaScript-emitting runtime.

  cjledger.py <rundir> [--top N] [--json]

Kinds, from V8's jitdump record names (log.cc CodeEventLogger: "JS:" + marker + debug name + " " + script:line:col for JS,
"JS:" + name + "-" + index + "-" + tier for wasm; markers code-kind.cc CodeKindToMarker):
  js-turbofan (*, o* = OSR)  js-maglev (+, o+)  js-sparkplug (^)  js-ignition (~)
  wasm-turbofan  wasm-liftoff
  v8-<tag> (Builtin:, Stub:, RegExp:, BytecodeHandler:, ...: a record with another tag)
  native (no jitdump record covers the ip: V8 embedded builtins not logged, chromium, the kernel is excluded by /u)
Per function: cycles and instructions per step, IPC. Needs set A or C (instructions + cycles)."""
import collections, glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import perfread
from jitindex import JitIndex
from counts import run_counts, key_of

WASM = re.compile(r'^JS:(.*)-(\d+|<anonymous>)-(turbofan|liftoff)$')
JS = re.compile(r"^JS:(o\*|o\+|\*'|\+'|\*|\+|\^|~)(.*)$")
JSKIND = {'*': 'js-turbofan', "*'": 'js-turbofan', 'o*': 'js-turbofan', '+': 'js-maglev', "+'": 'js-maglev',
          'o+': 'js-maglev', '^': 'js-sparkplug', '~': 'js-ignition'}


def classify(name):
    if name is None:
        return 'native', '(native)'
    m = WASM.match(name)
    if m:
        return f'wasm-{m.group(3)}', m.group(1)
    m = JS.match(name)
    if m:
        return JSKIND[m.group(1)], m.group(2)
    tag = name.split(':', 1)[0] if ':' in name else 'other'
    return f'v8-{tag}', name


def ledger(rundir):
    c = run_counts(rundir)
    if not c['ok']:
        sys.exit(f'{rundir}: run not ok')
    tid, steps = c['tid'], c['steps']
    t0, t1 = json.load(open(os.path.join(rundir, 'window.json')))['window']
    dumps = glob.glob(os.path.join(rundir, 'jit-*.dump'))
    idx = JitIndex(max(dumps, key=os.path.getsize)) if dumps else None
    data = os.path.join(rundir, 'perf.data')
    at = perfread.attrs(data)
    keys = [key_of(x['name']) for x in at]
    kind = collections.defaultdict(collections.Counter)   # kind -> event -> count
    fn = collections.defaultdict(collections.Counter)     # (kind, name) -> event -> count
    cache = {}
    for ai, ip, t, stid, chain, lbr in perfread.samples(data, tid=tid, t0=t0, t1=t1, want_chain=False, want_lbr=False):
        ev = keys[ai]
        if ev not in ('instructions', 'cycles', 'loads', 'near_call', 'l1_miss'):
            continue
        r = idx.at(ip, t) if idx else -1
        if r not in cache:
            cache[r] = classify(idx.name(r) if r >= 0 else None)
        k, nm = cache[r]
        w = at[ai]['period'] / steps
        kind[k][ev] += w
        fn[(k, nm)][ev] += w
    return c, kind, fn


def main():
    a = sys.argv[1:]
    top = int(a[a.index('--top') + 1]) if '--top' in a else 25
    for d in [x for i, x in enumerate(a) if not x.startswith('--') and not (i and a[i - 1] == '--top')]:
        c, kind, fn = ledger(d)
        ps = c['per_step']
        print(f"## {os.path.basename(d)} ({c['arm']}): {c['ms_per_step']:.4f} ms/step, {ps['cycles']/1e6:.3f} M cycles, "
              f"{ps['instructions']/1e6:.3f} M instr per step, thread share {c['thread_share']:.0%}, throttled {c['throttles'] or 0}")
        tot = sum(v['cycles'] for v in kind.values())
        print(f"{'kind':16s} {'Mcyc/step':>10s} {'Minstr/step':>12s} {'IPC':>5s} {'share':>6s}")
        for k, v in sorted(kind.items(), key=lambda kv: -kv[1]['cycles']):
            print(f"{k:16s} {v['cycles']/1e6:10.3f} {v['instructions']/1e6:12.3f} "
                  f"{(v['instructions'] / v['cycles'] if v['cycles'] else 0):5.2f} {v['cycles'] / tot:6.1%}")
        print(f"\ntop {top} functions by cycles:")
        print(f"{'Mcyc/step':>10s} {'Minstr/step':>12s} {'IPC':>5s} {'share':>6s} {'kind':14s} name")
        for (k, nm), v in sorted(fn.items(), key=lambda kv: -kv[1]['cycles'])[:top]:
            print(f"{v['cycles']/1e6:10.3f} {v['instructions']/1e6:12.3f} "
                  f"{(v['instructions'] / v['cycles'] if v['cycles'] else 0):5.2f} {v['cycles'] / tot:6.1%} {k:14s} {nm[:110]}")
        if '--json' in a:
            json.dump(dict(counts=c, kinds={k: dict(v) for k, v in kind.items()},
                           fns=[dict(kind=k, name=nm, **v) for (k, nm), v in fn.items()]),
                      open(os.path.join(d, 'cjledger.json'), 'w'), indent=1)
        print()


if __name__ == '__main__':
    main()
