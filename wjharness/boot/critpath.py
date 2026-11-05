#!/usr/bin/env python3
"""critpath.py <cap>... -- the client thread's component shares over segments 3-6 (javaMain -> reload start), where it
is the CPU-bound critical path (R487). One column per capture; shares of the client's samples in that window."""
import sys, os, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import bootattr
GROUP = {
    "exec: JIT-emitted Java (our tier)": "A run Java: JIT-emitted code",
    "exec: interpreter": "A run Java: interpreter",
    "exec: AOT JDK (IKVM.Java)": "A run Java: AOT JDK (IKVM.Java)",
    "exec: AOT corlib (no IKVM/Java caller)": "A run Java: AOT JDK (IKVM.Java)",
    "exec: zlib/png": "A run Java: AOT JDK (IKVM.Java)",
    "IKVM: class file parse / define / link (managed)": "B IKVM: parse/define/link",
    "IKVM: Java bytecode -> IL (Compiler/CodeEmitter)": "B IKVM: bytecode -> IL",
    "IKVM/mono: Reflection.Emit + type bake": "B IKVM/mono: Reflection.Emit bake",
    "mono: type/metadata loading": "C mono: type/metadata loading",
    "interp: transform (IL -> interp code)": "C mono: interp transform",
    "AOT: lazy method init / resolution": "C mono: AOT lazy init",
    "exceptions: throw / unwind / stack trace": "C mono: exceptions",
    "GC: sgen collection": "C mono: GC",
    "JIT tier: mono compile (IR passes + emit)": "D JIT tier: compile",
    "JIT tier: V8 compile+instantiate of emitted modules": "D JIT tier: V8 compile+instantiate",
    "JIT<->interp boundary (entry/exit, thunks, imethod lookup)": "D JIT<->interp/AOT boundary",
    "JIT tier: dispatch/admission helpers": "D JIT tier: dispatch/admission",
    "clock: mono_100ns_ticks -> clock_gettime -> performance.now()": "D JIT tier: broker clock read",
    "V8: lazy compile of main-module functions": "E V8: lazy compile (main module)",
    "V8: Liftoff frame setup builtin": "E V8: Liftoff frame setup",
}
cols = []
for d in sys.argv[1:]:
    cap, syms, ph = bootattr.open_taskclock(d)
    hz = float(ph["freq"]) if not ph.get("period") else 1e9 / float(ph["period"])
    mk = bootattr.marks_mono(d, ph)
    a0, a1 = int(mk["javaMain"] * 1e9), int(mk["titleReady"] * 1e9)
    cnt = collections.Counter(s[0] for s in cap.samples if a0 <= s[2] <= a1)
    client = max((t for t in cnt if cap.comm.get(t, "").startswith("DedicatedWorker")), key=lambda t: cnt[t])
    w0, w1 = int(mk["javaMain"] * 1e9), int(mk["reload"] * 1e9)
    c = collections.Counter()
    for tid, ip, t, ch in cap.samples:
        if tid != client or not (w0 <= t <= w1):
            continue
        names = [syms.lookup(ip) or b"?"] + [x for x in (syms.lookup(q) for q in (ch or ())) if x]
        comp = bootattr.component_of(names)
        c[GROUP.get(comp, "F other (" + comp[:30] + ")" if not comp.startswith("leaf") else "F other")] += 1
    cols.append((d, c, sum(c.values()), (w1 - w0) / 1e9, hz))
keys = sorted(set(k for _, c, _, _, _ in cols for k in c))
print(f"{'':<42}" + "".join(f"{os.path.basename(d):>16}" for d, *_ in cols))
print(f"{'window wall s / client CPU s':<42}" + "".join(f"{w:8.1f}/{n / hz:6.1f} " for d, c, n, w, hz in cols))
for k in keys:
    print(f"{k[2:]:<42}" + "".join(f"{100 * c[k] / n:7.1f}% {c[k] / hz:5.1f}s " for d, c, n, w, hz in cols))
