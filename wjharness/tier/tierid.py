#!/usr/bin/env python3
"""Per-method BYTE IDENTITY between two whole-tier dumps.

tierdiff.py answers "did the population's aggregate shape move" -- calls per method, bytes per method. This
answers a different and much sharper question, and it is the gate for any change that is supposed to be a
pure refactor: for every method BOTH runs JITted, are the module bytes the same?

It works because wasmtier.mjs already records a content hash per module (`sha`), and V8's own module URL
carries the mono full name. So identity is a set comparison, not 24,000 byte diffs.

WHY A SET AND NOT A VALUE. A method can be emitted more than once in a run -- a re-emit after its callees
JIT, an island re-compile -- so one name maps to a SET of distinct module hashes. Two runs agree on a method
when those sets are equal. A name whose sets merely overlap is reported separately from one that is
disjoint: overlap usually means the two runs re-emitted it a different number of times, which is a
scheduling difference and not a codegen one; disjoint means the bytes really changed.

POPULATION ASYMMETRY IS EXPECTED AND IS NOT A FAILURE. Compilation is hotness-triggered, so timing noise
alone changes which methods reach the JIT at all. Only the intersection carries information; the
only-in-A / only-in-B counts are printed so a wildly lopsided run is visible, not so they can be compared.

Usage:  tierid.py <dirA> <dirB> [--show N]
"""
import json, sys, re, os
from collections import defaultdict

def load(d):
    with open(os.path.join(d, "index.json")) as f:
        idx = json.load(f)
    byname = defaultdict(set)
    for m in idx["modules"]:
        url = m.get("url") or ""
        # wasm://wasm/<mono full name>-<8 hex>  -- V8 appends its own content hash, which is exactly what
        # we must strip: it varies with the bytes, so leaving it on makes every method look unique.
        name = re.sub(r"^wasm://wasm/", "", url)
        name = re.sub(r"-[0-9a-f]{1,8}$", "", name)
        if name:
            byname[name].add(m["sha"])
    return byname, idx

def main():
    argv, args, show = sys.argv[1:], [], 20
    i = 0
    while i < len(argv):
        if argv[i] == "--show":
            show = int(argv[i + 1]); i += 2
        else:
            args.append(argv[i]); i += 1
    if len(args) != 2:
        print(__doc__); sys.exit(2)
    A, ia = load(args[0])
    B, ib = load(args[1])
    common = set(A) & set(B)
    same = [n for n in common if A[n] == B[n]]
    overlap = [n for n in common if A[n] != B[n] and (A[n] & B[n])]
    diff = [n for n in common if not (A[n] & B[n])]
    onlyA = set(A) - set(B)
    onlyB = set(B) - set(A)

    print(f"A {args[0]}: {len(ia['modules'])} modules, {len(A)} distinct methods   ok={ia.get('ok')}")
    print(f"B {args[1]}: {len(ib['modules'])} modules, {len(B)} distinct methods   ok={ib.get('ok')}")
    print(f"common methods         {len(common)}")
    print(f"  identical hash sets  {len(same)}  ({100.0*len(same)/len(common):.2f}%)" if common else "  (no common methods)")
    print(f"  overlapping          {len(overlap)}   (same bytes present in both, different re-emit counts)")
    print(f"  DIFFERENT bytes      {len(diff)}   <-- the number that must be 0 for a pure refactor")
    print(f"only in A {len(onlyA)}   only in B {len(onlyB)}   (hotness-triggered; not a failure by itself)")
    for n in sorted(diff)[:show]:
        print(f"  DIFF {n}\n       A={sorted(A[n])} B={sorted(B[n])}")
    if len(diff) > show:
        print(f"  ... {len(diff)-show} more")
    sys.exit(1 if diff else 0)

main()
