#!/usr/bin/env python3
"""Per-FUNCTION wire size of every emitted module, and the share under V8's inlining cap.

Why per-function and not per-module: V8's `SmallEnoughToInline` (v8/src/wasm/inlining-tree.h) hard-rejects a
candidate whose `wire_byte_size_` exceeds `wasm_inlining_max_size` (500 in stock Chrome). That size is the
CALLEE FUNCTION's body, not the module — so module bytes, which include the name section, imports and the entry
thunk, systematically overstate it. Earlier rounds estimated the body by subtracting a guessed framing
allowance; this reads the code section instead.

This is the kill-check for the co-location/batching project: co-locating a caller and callee only enables
inlining if the callee's body is under the cap, so the share under 500 B upper-bounds what batching could ever
convert from a dispatch into an inline.

Usage: bodysize.py <dir-of-.wasm> [--cap 500]
"""
import glob
import os
import sys
from collections import defaultdict

CAP = int(sys.argv[sys.argv.index("--cap") + 1]) if "--cap" in sys.argv else 500


def uleb(b, i):
    v = s = 0
    while True:
        byte = b[i]; i += 1
        v |= (byte & 0x7F) << s
        if not (byte & 0x80):
            return v, i
        s += 7


def function_body_sizes(path):
    """Sizes of each function body in the Code section, largest first."""
    b = open(path, "rb").read()
    if len(b) < 8 or b[:4] != b"\0asm":
        return []
    i = 8
    while i < len(b):
        sid = b[i]; i += 1
        size, i = uleb(b, i)
        end = i + size
        if sid == 10:                      # Code section
            n, j = uleb(b, i)
            out = []
            for _ in range(n):
                fsz, j = uleb(b, j)
                out.append(fsz)
                j += fsz
            return sorted(out, reverse=True)
        i = end
    return []


def cat(name):
    if "MHC" in name:
        return "__<>MHC stubs"
    if "DynamicBinder" in name:
        return "__<>DynamicBinder__"
    if "lazyrecompiled" in name:
        return "__<>lazyrecompiled_"
    return "ordinary methods"


files = glob.glob(os.path.join(sys.argv[1], "**", "*.wasm"), recursive=True)
if not files:
    print(f"no .wasm under {sys.argv[1]}", file=sys.stderr)
    sys.exit(1)

buckets = defaultdict(list)
for f in files:
    sizes = function_body_sizes(f)
    if sizes:
        buckets[cat(os.path.basename(f))].append(sizes[0])   # largest = the method body

print(f"cap = {CAP} B (V8 wasm_inlining_max_size)   files = {len(files)}")
print(f"{'category':24} {'n':>7} {'median':>7} {'p90':>7} {'<=cap':>8}")
allb = []
for k, v in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
    v.sort(); allb += v
    med = v[len(v) // 2]
    p90 = v[int(len(v) * 0.9)]
    under = sum(1 for x in v if x <= CAP)
    print(f"{k:24} {len(v):7d} {med:7d} {p90:7d} {100.0*under/len(v):7.1f}%")
allb.sort()
under = sum(1 for x in allb if x <= CAP)
print(f"\nALL: n={len(allb)} median={allb[len(allb)//2]} p90={allb[int(len(allb)*0.9)]} "
      f"<= {CAP}B: {under} ({100.0*under/len(allb):.1f}%)")
print("Reading it: this is the CEILING on what co-location could convert to inlined calls. A low share means")
print("the batching project cannot pay off without first shrinking bodies, whatever the planner does.")
