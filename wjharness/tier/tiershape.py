#!/usr/bin/env python3
"""Per-method STRUCTURAL identity between two whole-tier dumps.

Raw byte identity across two runs is IMPOSSIBLE and it took a failed gate to see why. A module bakes, as
plain constants, several things that are allocation-order dependent and therefore differ between two runs of
the same binary:
  * inline-cache cell addresses      (i32.const 46768656 vs 46768512)
  * MonoVTable pointers in guards    (i32.const 856568520 vs 800518856)
  * PIC site ids and displacements   (i32.const 19853, i64.load offset=317648)
and, across two BUILDS, the helper function-table indices themselves move, because they are indices into a
different dotnet.native.wasm ("h" "403805" vs "h" "403806").

None of that is codegen. What a refactor must not change is the STRUCTURE: which opcodes, in which order,
with which type indices, which function indices, which local indices and which branch depths. This decodes
the code section and fingerprints exactly that, blanking only the immediates listed above.

What it therefore CATCHES: a call that changed form (call_indirect <-> call), a type index that moved, an
import that appeared or vanished, a body that grew or shrank an instruction.
What it deliberately does NOT catch: a constant whose VALUE changed but not its width. That is the price of
comparing two runs at all, and those constants are addresses and profile ids, not code.

Usage:  tiershape.py <dirA> <dirB> [--show N] [--dump NAME]
"""
import json, os, re, sys, hashlib
from collections import defaultdict

# opcodes whose immediates are run-dependent and are blanked; everything else keeps its immediates.
def read_uleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not (x & 0x80): return r, i

def read_sleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not (x & 0x80):
            if s < 64 and (x & 0x40): r |= -(1 << s)
            return r, i

def skip_name(b, i):
    n, i = read_uleb(b, i)
    return b[i:i+n], i + n

# (immediate kinds) u=uleb, s=sleb, b=blockty, m=memarg, f4/f8=float, br=br_table, x=none
IMM = {}
def setimm(lo, hi, kind):
    for o in range(lo, hi + 1): IMM[o] = kind
setimm(0x00, 0x01, "")                      # unreachable, nop
for o in (0x02, 0x03, 0x04): IMM[o] = "b"   # block, loop, if
for o in (0x05, 0x0b, 0x0f): IMM[o] = ""    # else, end, return
for o in (0x0c, 0x0d): IMM[o] = "u"         # br, br_if
IMM[0x0e] = "br"                            # br_table
IMM[0x10] = "u"                             # call
IMM[0x11] = "uu"                            # call_indirect
IMM[0x06] = "b"; IMM[0x07] = "u"            # legacy EH try, catch (dumps from before the exnref port)
IMM[0x08] = "u"; IMM[0x09] = "u"            # throw, legacy rethrow
IMM[0x18] = "u"; IMM[0x19] = ""             # legacy delegate, catch_all
IMM[0x0a] = ""                              # throw_ref
IMM[0x1f] = "tt"                            # try_table: blocktype, then a vec of catch clauses
for o in (0x1a, 0x1b): IMM[o] = ""          # drop, select
IMM[0x1c] = "vt"                            # select t
for o in range(0x20, 0x25): IMM[o] = "u"    # local/global get/set/tee
for o in range(0x25, 0x27): IMM[o] = "u"    # table.get/set
for o in range(0x28, 0x3f): IMM[o] = "m"    # loads/stores
for o in (0x3f, 0x40): IMM[o] = "u"         # memory.size/grow
IMM[0x41] = "S"                             # i32.const   (BLANKED)
IMM[0x42] = "S"                             # i64.const   (BLANKED)
IMM[0x43] = "f4"; IMM[0x44] = "f8"          # f32/f64.const (BLANKED)
for o in range(0x45, 0xc5): IMM.setdefault(o, "")
for o in range(0xc0, 0xc5): IMM[o] = ""     # sign extensions

def fingerprint_code(b, i, end, out):
    while i < end:
        op = b[i]; i += 1
        if op == 0xfc:
            sub, i = read_uleb(b, i)
            out.append(f"fc{sub}")
            if sub in (8, 9, 11, 12, 13, 14, 15, 16, 17):   # bulk memory / table ops carry indices
                if sub in (8, 12, 13, 14): _, i = read_uleb(b, i)
                _, i = read_uleb(b, i)
            continue
        if op == 0xfe:                                       # threads: memarg after the sub-opcode
            sub, i = read_uleb(b, i)
            out.append(f"fe{sub}")
            if sub == 0x03: _, i = read_uleb(b, i)           # atomic.fence
            else:
                _, i = read_uleb(b, i); _, i = read_uleb(b, i)   # align, offset (offset BLANKED)
            continue
        if op == 0xfd:
            sub, i = read_uleb(b, i)
            out.append(f"fd{sub}")
            continue
        kind = IMM.get(op)
        if kind is None:
            out.append(f"?{op:02x}"); return i
        out.append(f"{op:02x}")
        for k in kind:
            if k == "u":
                v, i = read_uleb(b, i); out.append(str(v))
            elif k == "S":
                _, i = read_sleb(b, i); out.append("K")       # blanked: addresses, ids, vtables
            elif k == "b":
                if b[i] == 0x40: out.append("void"); i += 1
                elif b[i] in (0x7f, 0x7e, 0x7d, 0x7c, 0x70, 0x6f): out.append(f"vt{b[i]}"); i += 1
                else:
                    v, i = read_sleb(b, i); out.append(f"ty{v}")   # multi-value: a TYPE INDEX, kept
            elif k == "m":
                a, i = read_uleb(b, i); _, i = read_uleb(b, i); out.append(f"a{a}o")  # offset blanked
            elif k == "f4": i += 4; out.append("K")
            elif k == "f8": i += 8; out.append("K")
            elif k == "br":
                n, i = read_uleb(b, i); out.append(str(n))
                for _ in range(n + 1):
                    v, i = read_uleb(b, i); out.append(str(v))
            elif k == "vt":
                n, i = read_uleb(b, i); i += n; out.append(f"vt{n}")
            elif k == "tt":
                if b[i] == 0x40: out.append("void"); i += 1
                elif b[i] in (0x7f, 0x7e, 0x7d, 0x7c, 0x70, 0x6f): out.append(f"vt{b[i]}"); i += 1
                else:
                    v, i = read_sleb(b, i); out.append(f"ty{v}")
                n, i = read_uleb(b, i); out.append(f"c{n}")
                for _ in range(n):
                    kind = b[i]; i += 1; out.append(f"k{kind}")
                    if kind in (0x00, 0x01):                  # catch / catch_ref carry a tag index
                        v, i = read_uleb(b, i); out.append(str(v))
                    v, i = read_uleb(b, i); out.append(str(v))  # label
    return i

def shape(path):
    b = open(path, "rb").read()
    i = 8
    parts = []
    while i < len(b):
        sid = b[i]; i += 1
        slen, i = read_uleb(b, i)
        end = i + slen
        if sid == 1:                                   # types: kept in full, they are structural
            parts.append("T:" + b[i:end].hex())
        elif sid == 2:                                 # imports: keep kinds and type indices, blank NAMES
            n, j = read_uleb(b, i)
            desc = [f"I{n}"]
            for _ in range(n):
                _, j = skip_name(b, j)
                _, j = skip_name(b, j)                 # the helper's decimal table index moves per build
                k = b[j]; j += 1
                desc.append(f"k{k}")
                if k == 0x00: v, j = read_uleb(b, j); desc.append(f"t{v}")
                elif k == 0x01: j += 1; fl = b[j]; j += 1; _, j = read_uleb(b, j); desc += ["tbl"]
                elif k == 0x02:
                    fl = b[j]; j += 1
                    _, j = read_uleb(b, j)
                    if fl & 1: _, j = read_uleb(b, j)
                    desc.append("mem")
                elif k == 0x03: j += 2; desc.append("glob")
                elif k == 0x04: j += 1; v, j = read_uleb(b, j); desc.append(f"tag{v}")
            parts.append(":".join(desc))
        elif sid == 3 or sid == 7:                     # functions, exports: structural
            parts.append(f"S{sid}:" + b[i:end].hex())
        elif sid == 10:                                # code
            n, j = read_uleb(b, i)
            out = [f"C{n}"]
            for _ in range(n):
                blen, j = read_uleb(b, j)
                bend = j + blen
                ng, j = read_uleb(b, j)
                out.append(f"L{ng}")
                for _ in range(ng):
                    c, j = read_uleb(b, j); out.append(f"{c}x{b[j]}"); j += 1
                fingerprint_code(b, j, bend, out)
                j = bend
            parts.append(" ".join(out))
        elif sid == 0:
            pass                                       # custom (name section): not structure
        else:
            parts.append(f"S{sid}:{slen}")
        i = end
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:16], parts

def load(d):
    idx = json.load(open(os.path.join(d, "index.json")))
    byname = defaultdict(set)
    where = {}
    for m in idx["modules"]:
        name = re.sub(r"-[0-9a-f]{1,8}$", "", re.sub(r"^wasm://wasm/", "", m.get("url") or ""))
        if not name: continue
        h, _ = shape(os.path.join(d, "mods", m["sha"] + ".wasm"))
        byname[name].add(h)
        where.setdefault((name, h), m["sha"])
    return byname, where, idx

def main():
    argv, args, show, dump = sys.argv[1:], [], 20, None
    i = 0
    while i < len(argv):
        if argv[i] == "--show": show = int(argv[i+1]); i += 2
        elif argv[i] == "--dump": dump = argv[i+1]; i += 2
        else: args.append(argv[i]); i += 1
    if len(args) != 2: print(__doc__); sys.exit(2)
    A, wa, ia = load(args[0]); B, wb, ib = load(args[1])
    common = set(A) & set(B)
    same    = [n for n in common if A[n] == B[n]]
    overlap = [n for n in common if A[n] != B[n] and (A[n] & B[n])]
    diff    = [n for n in common if not (A[n] & B[n])]
    print(f"A {args[0]}: {len(ia['modules'])} modules, {len(A)} distinct methods")
    print(f"B {args[1]}: {len(ib['modules'])} modules, {len(B)} distinct methods")
    print(f"common methods            {len(common)}")
    print(f"  identical structure     {len(same)}  ({100.0*len(same)/max(1,len(common)):.2f}%)")
    print(f"  overlapping             {len(overlap)}")
    print(f"  DIFFERENT structure     {len(diff)}   <-- must be 0 for a pure refactor")
    print(f"only in A {len(set(A)-set(B))}   only in B {len(set(B)-set(A))}")
    for n in sorted(diff)[:show]:
        print(f"  DIFF {n}")
        print(f"       A mods/{wa[(n, sorted(A[n])[0])]}.wasm   B mods/{wb[(n, sorted(B[n])[0])]}.wasm")
    if len(diff) > show: print(f"  ... {len(diff)-show} more")
    sys.exit(1 if diff else 0)

main()
