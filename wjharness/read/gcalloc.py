#!/usr/bin/env python3
"""gcalloc.py <mcperf capdir> [phase] [--top N] -- in a phase window (default `ingame`), the samples under sgen's
ALLOCATION SLOW PATH (TLAB refill / large-object alloc; every refill hands out sgen's TLAB, so slow-path samples track
allocated volume) and under COLLECTIONS, each attributed to the first managed frame above the allocator: who allocates,
and whose allocation triggered the GC. Task-clock capture; uses bootattr's loader."""
import collections, json, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import bootattr
a = sys.argv[1:]
d = a[0]
pos = [x for x in a[1:] if not x.startswith("--")]
phase = pos[0] if pos else "ingame"
TOP = int(a[a.index("--top") + 1]) if "--top" in a else 30
cap, syms, ph = bootattr.open_taskclock(d)
hz = float(ph["freq"]) if not ph.get("period") else 1e9 / float(ph["period"])
p = next(x for x in ph["phases"] if x["name"] == phase)
w0, w1 = int(p["t0"] * 1e9), int(p["t1"] * 1e9)
ALLOC = re.compile(rb"^JS:(sgen_alloc_obj_nolock|sgen_try_alloc_obj_nolock|sgen_alloc_obj|sgen_alloc_obj_mature|sgen_los_alloc_large_inner|"
                   rb"sgen_alloc_obj_pinned|mono_gc_alloc_obj|mono_gc_alloc_vector|mono_gc_alloc_string|mono_gc_alloc_array|"
                   rb"sgen_fragment_allocator_|sgen_nursery_alloc|sgen_nursery_alloc_range)")
GC = re.compile(rb"^JS:(sgen_perform_collection|collect_nursery|major_do_collection|sgen_ensure_free_space)")
SKIP = re.compile(rb"^(JS:(sgen_|mono_gc_|mono_object_new|mono_array_new|mono_string_new|ves_icall_|aot_wrapper_alloc|"
                  rb"corlib_wrapper_alloc|wrapper_alloc|mono_|interp_|do_icall|wj_|wasm_jit|monoeg_|dl|__)|Builtin:|DSO:|\[unknown\])")
alloc = collections.Counter(); gcby = collections.Counter(); tot = 0; na = ng = 0
by_thread = collections.Counter()
strip = re.compile(rb"-\d+-(liftoff|turbofan)$")
for tid, ip, t, ch in cap.samples:
    if not (w0 <= t <= w1): continue
    tot += 1
    names = [syms.lookup(ip) or b"[unknown]"] + [x for x in (syms.lookup(c) for c in (ch or ())) if x]
    names = [strip.sub(b"", n) for n in names]
    isgc = any(GC.match(n) for n in names)
    isal = any(ALLOC.match(n) for n in names)
    if not (isgc or isal): continue
    # first frame above the deepest allocator/GC frame that is not runtime plumbing
    idx = max(i for i, n in enumerate(names) if ALLOC.match(n) or GC.match(n))
    caller = next((n for n in names[idx + 1:] if not SKIP.match(n)), b"(none)")
    caller = caller[:120].decode("latin1")
    if isgc: gcby[caller] += 1; ng += 1
    else: alloc[caller] += 1; na += 1
    by_thread[(cap.comm.get(tid, "?"), tid, "gc" if isgc else "alloc")] += 1
print(f"{d} {phase}: {(w1 - w0) / 1e9:.1f} s, {tot / hz:.1f} CPU s sampled; allocation slow path {na / hz:.2f} s, collections {ng / hz:.2f} s")
print("\n-- allocation slow path, by first managed caller --")
for k, v in alloc.most_common(TOP): print(f"  {v / hz:6.2f} s {100 * v / max(na, 1):5.1f}%  {k}")
print("\n-- collection time, by the allocation that triggered it --")
for k, v in gcby.most_common(TOP): print(f"  {v / hz:6.2f} s {100 * v / max(ng, 1):5.1f}%  {k}")
print("\n-- by thread --")
for (c, tid, kind), v in sorted(by_thread.items(), key=lambda x: -x[1])[:12]: print(f"  {v / hz:6.2f} s  {kind:5s} {tid} {c}")
