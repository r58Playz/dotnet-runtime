#!/usr/bin/env python3
"""Which x86 INSTRUCTION was executing? Per-sample attribution inside JIT-emitted code.

WHY THIS, WHEN codegencensus.py AND spillsites.py EXIST
--------------------------------------------------------
Both of those weight a symbol's WHOLE disassembly by that symbol's share of samples. That answers "what does
hot code CONTAIN", which is a static question, and it cannot tell a hot loop from a cold throw stub inside the
same method. On this workload the difference inverts conclusions: MHC stubs carry several alternative dispatch
routes of which exactly one runs per invoke, so their `call_indirect` density looks alarming and costs nothing.

This resolves each sample to (function, byte offset) and classifies the ONE instruction at that offset. It is
therefore a dynamic measurement, and it is what the fps A/Bs cannot do: instructions retired for the same work
do not move with the clock, so it stays valid on a box that thermally throttles 2106 -> 1403 MHz inside a run.

WHAT IT ANSWERS THAT NOTHING ELSE HERE DOES
-------------------------------------------
  --bands       where in a function time goes. Our JIT spends 20.5% of its time in the first 24 BYTES of a
                function -- the prologue -- against 12.4% for AOT code. That is the cost of entering a
                function, and it is the direct readout for any change that removes calls (inlining) or
                shortens the prologue (frame elision, the s.p round-trip).
  --dispatch    how much time is inside an INDIRECT-CALL preamble. Read the warning below before using
                the number: `tight` does NOT separate `call_indirect` from `call <import>`.
                Measured 10.5% (tight) to 20.8% (with the checks) of our tier's time.
  default       the instruction mix actually executed. Our JIT: 18.2% real computation, 32.3% register
                shuffling, 25.7% guards/branches, 23.2% heap loads.

--dispatch's `tight` MASK COUNTS IMPORT CALLS TOO (R158) -- DO NOT USE IT FOR AN IMPORT A/B
--------------------------------------------------------------------------------------------
`tight` marks `shl $0x9 .. call *`. That pair is the WasmCodePointerTable index->address computation
(x16 with a 27-bit mask), and BOTH call forms emit it: V8 lowers an imported DIRECT call through
BuildImportedFunctionTargetAndImplicitArg, which returns a V<Word32> WasmCodePointer, and hands it to
BuildWasmCall(..., kWasmIndirectFunction) -- literally the call_indirect path
(turboshaft-graph-interface.cc:2715-2723, turboshaft-graph-interface-inl.h:64-105, v8 15.4.77).
Verified against emitted x86 with callform-x86.py: 865 of 967 `call *` sites in a sampled capture carry
the shl pair with NO table bounds check, i.e. they are import calls being counted as dispatch preambles.

What actually separates the forms is the part `tight` EXCLUDES: a `call_indirect` additionally does a
table bounds check (`cmpl $<tablesize>; jbe`) and a canonical-type check (`mov $<typeid>; cmp; jne`) and
loads the target at a runtime index. An import call does two constant-offset loads (usually hoisted).
Both then do the same 6-instruction conversion and the same indirect `call *`.

Consequence: `tight` cannot move when a site is converted call_indirect -> import, and R154's
10.44% -> 10.31% reading across a MONO_WASM_JIT_DIRECT_IMPORT A/B was measuring a sequence present in
both arms. Use callform-x86.py's bounds-check split for that question, or count call forms statically
with vcallreach.py.

CLASSIFICATION IS BY MNEMONIC FIRST
------------------------------------
codegencensus.py's CATS list is ordered so that "other memory access (base+index)" matches `call *0x18(%rbx)`
before the indirect-call rule is ever tried, which is how it came to report dispatch as 0.0% of the hot set.
Decide on the mnemonic, then refine on the operand form. Never the other way round.

Requires a jitdump capture (mcperf MCPERF_JITDUMP=1); the code bytes come out of the JIT_CODE_LOAD records,
so no `perf inject --jit` and no per-function .so files are involved. Round 111 concluded that route "did not
pan out"; it matches 256/256 hot symbols on perf-r76/jit-1169200.dump.

Usage:
  hotinsn.py <outdir> [--phase ingame] [--window T0,T1] [--kind ours|aot|v8|all]
             [--bands] [--dispatch] [--feeder] [--top N] [--min-samples N]
  hotinsn.py --selftest          # classifier unit checks, no capture needed
"""
import bisect
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perfraw import Capture, phase_window          # noqa: E402

HDR, FIXED = 16, 40
MONO = re.compile(r"(?<!:):(?!:)")
TIERED = re.compile(r"^JS:(.*)-(\d+|none)-(turbofan|liftoff|none)$")
ARITH = re.compile(r"^(add|sub|imul|mul|idiv|div|shl|shr|sar|rol|ror|and|or|xor|not|neg|inc|dec|movs|movz"
                   r"|cdq|cqo|bt|bs|popcnt|set|cmov)")
SSE = re.compile(r"^(v|movs[sd]$|movap|movup|cvt|ucomis|comis|sqrt|pxor|punpck|pshuf)")


def kind_of(sym):
    """ours | aot | v8 -- by NAMING CONVENTION, never by scriptId.

    MONO_WASM_JIT_NAMES writes mono's own full name (`<ret> <Type>:<method> (<args>)`, or `(wrapper ...)`)
    into every JITted module's name section; AOT methods in dotnet.native.wasm carry underscore-mangled C
    symbols with neither spaces nor a single colon. `::` is C++ in the runtime, not a mono full name.
    Using V8's scriptId instead is what once turned 59.8% into 2.80% -- V8 stamps whatever id it likes.
    """
    m = TIERED.match(sym)
    nm = m.group(1) if m else sym
    if nm.startswith("(wrapper ") or (MONO.search(nm) and " " in nm and "://" not in nm and "::" not in nm):
        return "ours"
    if sym.startswith(("Builtin:", "BytecodeHandler:", "Stub:")):
        return "v8"
    return "aot"


def load_jitdump(path):
    """[(start, end, name, file_offset_of_code, code_size)] sorted by start."""
    ivs = []
    with open(path, "rb") as g:
        hdr = g.read(40)
        en = ">" if hdr[:4] == b"JiTD" else "<"
        if hdr[:4] not in (b"JiTD", b"DTiJ"):
            sys.exit(f"{path}: not a jitdump")
        pos = struct.unpack(en + "I", hdr[8:12])[0]
        g.seek(pos)
        while True:
            rh = g.read(HDR)
            if len(rh) < HDR:
                break
            rid, rsize = struct.unpack(en + "II", rh[:8])
            if rsize < HDR:
                break
            body = g.read(rsize - HDR)
            if len(body) < rsize - HDR:
                break
            if rid == 0 and len(body) >= FIXED:
                ca, cs = struct.unpack(en + "QQ", body[16:32])
                nul = body.find(b"\0", FIXED)
                # The record must be header + fixed + name + NUL + code exactly, or the offsets are wrong and
                # every number below is garbage. This is the invariant a hex-sort bug once violated silently.
                if nul >= 0 and HDR + FIXED + (nul - FIXED) + 1 + cs == rsize:
                    ivs.append((ca, ca + cs, body[FIXED:nul].decode("utf8", "replace"),
                                pos + HDR + FIXED + (nul - FIXED) + 1, cs))
            pos += rsize
    ivs.sort()
    return ivs


def disasm(fh, foff, size):
    """[(offset, mnemonic, operands)] for one JIT_CODE_LOAD blob."""
    fh.seek(foff)
    code = fh.read(size)
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    try:
        os.write(fd, code)
        os.close(fd)
        # objdump SEEKS its input, so a pipe or /dev/stdin yields zero instructions silently.
        p = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "--no-show-raw-insn", tmp],
                           capture_output=True)
    finally:
        os.unlink(tmp)
    out = []
    for line in p.stdout.decode("utf8", "replace").split("\n"):
        m = re.match(r"\s+([0-9a-f]+):\s+(.*)$", line)
        if not m:
            continue
        s = m.group(2).strip()
        if not s or s.startswith("("):
            continue
        parts = s.split(None, 1)
        out.append((int(m.group(1), 16), parts[0], parts[1] if len(parts) > 1 else ""))
    return out


def _dest_operand(ops):
    """Last operand at paren depth 0. AT&T order puts the destination last, but
    `ops.rsplit(",", 1)` splits INSIDE an indexed memory operand: on
    `mov %rax,0x8(%rbx,%rcx,1)` it returns `1)`, which contains no "(" and so read
    as a LOAD. Nearly every store this emitter makes is indexed (linear memory is
    reached as base+index), so stores were being counted as loads -- `heap STORE`
    read 0.01% of our tier when the true figure is 7.89% (R180). Group totals were
    unaffected, since both land in HEAP MEMORY."""
    depth = 0
    start = 0
    for i, ch in enumerate(ops):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            start = i + 1
    return ops[start:]


def classify(mn, ops):
    if mn == "call":
        return "CALL indirect" if ops.startswith("*") else "CALL direct"
    if mn == "jmp":
        return "JMP indirect (tail/dispatch)" if ops.startswith("*") else "jmp (direct)"
    if re.match(r"^j[a-z]{1,3}$", mn):
        return "branch jcc"
    if mn.startswith(("cmp", "test")):
        return "guard cmp/test"
    if mn.startswith("mov") and not SSE.match(mn):
        # rbp/rsp-relative is the machine stack, i.e. TurboFan's own spill slots -- NOT our GC ref frame,
        # which lives in wasm linear memory and is reached through the memory base register.
        if re.search(r"\((%rbp|%rsp)\)", ops) or re.search(r"\(%rbp,|\(%rsp,", ops):
            return "SPILL/RELOAD (rbp/rsp)"
        if "(" in ops:
            return "heap STORE" if "(" in _dest_operand(ops) else "heap LOAD"
        return "reg-reg mov"
    if mn == "lea":
        return "lea (address calc)"
    if mn in ("push", "pop"):
        return "push/pop (frame)"
    if mn in ("ud2", "int3", "hlt"):
        return "trap"
    if mn.startswith("nop") or mn == "xchg":
        return "nop/pad"
    if mn in ("ret", "leave"):
        return "ret/leave"
    if SSE.match(mn):
        return "SIMD/float"
    if ARITH.match(mn):
        return "arith (mem operand)" if "(" in ops else "arith (reg/imm)"
    return "other: " + mn


GROUPS = {
    "REAL COMPUTE": ["arith (reg/imm)", "SIMD/float", "arith (mem operand)"],
    "HEAP MEMORY": ["heap LOAD", "heap STORE", "lea (address calc)"],
    "GUARDS/BRANCHES": ["guard cmp/test", "branch jcc", "jmp (direct)", "trap"],
    "REGISTER PRESSURE / FRAME": ["SPILL/RELOAD (rbp/rsp)", "reg-reg mov", "push/pop (frame)"],
    "CALLS": ["CALL direct", "CALL indirect", "JMP indirect (tail/dispatch)", "ret/leave"],
}


# --- FEEDER SPLIT for the tagged-pointer decompression pool (R190 / Stage 3) --------------------------
#
# `add %r14,<reg>` is a compressed-pointer decompression: %r14 holds the cage base and the register held
# a 32-bit compressed value. R181 measured that pool at 9.66% of our tier / 5.51% of the window; R190
# then split it and attributed 95.3% of it -- 5.25% of the WINDOW -- to `s.p`, on the strength of two
# properties per sample: the sample is in the ENTRY band, and the decompressed value was loaded THROUGH
# %r14 rather than out of an instance field.
#
# That is the signature of V8's imported-MUTABLE-global path, which is
#   ImportedMutableGlobalsBuffers -> element -> decompress -> non-Immutable() offsets load
# (compiler/turboshaft/wasm-lowering-reducer.h:1041-1077). An IMMUTABLE imported global instead lowers to
# LOAD_IMMUTABLE_INSTANCE_FIELD + a TaggedBase().Immutable() constant-offset load (:1096-1104), which is
# not chained through %r14 -- and `s.p` is the only mutable import this emitter declares
# (wasm-encoder.c:344-364).
#
# R190's split was done by hand on a capture that has since been DELETED, so this is the instrument that
# makes it reproducible. It is also the direct readout for Stage 3: threading the frame base should
# collapse the entry-band chained arm specifically, leaving the other arms alone.
#
# CLASSIFIER DISCIPLINE (four bugs of this exact shape are on record -- symclass.mjs's scriptId clause,
# codegencensus.py's category order, hotinsn's own --dispatch mask, and _dest_operand): this one is
# checked against hand-written sequences by --selftest, which is wired into enctest-style use below.
_REG64 = {
    **{f"%r{n}": f"r{n}" for n in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp")},
    **{f"%e{n}": f"r{n}" for n in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp")},
}


def _norm_reg(r):
    """Canonical 64-bit name for a register operand, so a 32-bit load feeding a 64-bit add is matched.

    The decompression idiom is exactly `mov <mem>,%eax ; add %r14,%rax` -- the load writes the 32-bit
    alias and the add reads the 64-bit one, so a matcher that compares operand strings finds no
    definition and reports every sample as `unknown`."""
    r = r.strip()
    if not r.startswith("%"):
        return None
    if r in _REG64:
        return _REG64[r]
    m = re.match(r"^%r(\d+)[dwb]?$", r)
    if m:
        return "r" + m.group(1)
    m = re.match(r"^%(r[a-z]{2})$", r)
    if m:
        return m.group(1)
    return r[1:]


def _src_operand(ops):
    """First operand at paren depth 0 (AT&T source). Mirror of _dest_operand; same paren-depth rule,
    for the same reason -- an indexed memory source contains commas."""
    depth = 0
    for i, ch in enumerate(ops):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            return ops[:i]
    return ops


def is_decompress(mn, ops):
    """`add %r14,<reg>` -- and nothing else. Not `add $0x8,%r14`, not a memory-operand add."""
    return mn == "add" and _src_operand(ops).strip() == "%r14" and "(" not in ops


def feeder_of(ins, j, window=24):
    """What produced the value that ins[j] decompresses.

    Walks back for the most recent definition of the decompressed register. Returns one of:
      'chained (%r14)'  loaded through the cage base -- the imported-MUTABLE-global chain, i.e. `s.p`
      'instance-field'  loaded through some other base register
      'spill reload'    reloaded from TurboFan's own frame
      'chained add'      the register was itself just decompressed
      'other'/'unknown' anything else / no definition within `window`
    """
    tgt = _norm_reg(_dest_operand(ins[j][2]))
    if tgt is None:
        return "unknown"
    for k in range(j - 1, max(-1, j - 1 - window), -1):
        _, mn, ops = ins[k]
        if not ops:
            continue
        dst = _norm_reg(_dest_operand(ops))
        if dst != tgt:
            continue
        if is_decompress(mn, ops):
            return "chained add"
        if mn.startswith("mov") and not SSE.match(mn):
            src = _src_operand(ops)
            if "(" not in src:
                return "other"                      # reg-reg mov or immediate: not a load
            if "(%r14" in src:
                return "chained (%r14)"
            if "(%rbp" in src or "(%rsp" in src:
                return "spill reload"
            return "instance-field"
        return "other"
    return "unknown"


def _feeder_selftest():
    """Hand-written sequences with known answers. A classifier written from a plausible mental model of
    the emitted code and never checked against it is how all four previous classifier bugs happened."""
    def run(seq, want, why):
        ins = [(i * 4, mn, ops) for i, (mn, ops) in enumerate(seq)]
        j = len(ins) - 1
        got = feeder_of(ins, j)
        assert got == want, f"{why}: got {got!r}, want {want!r} for {seq}"

    # the s.p shape: 32-bit load through the cage base, then decompress into the 64-bit alias
    run([("mov", "0x10(%r14),%eax"), ("add", "%r14,%rax")], "chained (%r14)", "imported mutable global")
    # indexed source, which is where a naive rsplit(',') classifier breaks
    run([("mov", "0x8(%r14,%rcx,1),%eax"), ("add", "%r14,%rax")], "chained (%r14)", "indexed cage load")
    # an instance field: some other base register
    run([("mov", "0x1c(%rbx),%eax"), ("add", "%r14,%rax")], "instance-field", "instance field")
    # reloaded from TurboFan's own frame
    run([("mov", "-0x18(%rbp),%eax"), ("add", "%r14,%rax")], "spill reload", "spill reload")
    # an unrelated intervening def of a DIFFERENT register must not be taken as the feeder
    run([("mov", "0x10(%r14),%eax"), ("mov", "0x4(%rbx),%edx"), ("add", "%r14,%rax")],
        "chained (%r14)", "intervening unrelated def")
    # reg-reg mov is not a load
    run([("mov", "%rdx,%rax"), ("add", "%r14,%rax")], "other", "reg-reg mov")
    # no definition in range
    run([("nop", ""), ("add", "%r14,%rax")], "unknown", "no def")
    # r8-r15 with the 'd' suffix
    run([("mov", "0x10(%r14),%r8d"), ("add", "%r14,%r8")], "chained (%r14)", "r8d alias")
    # is_decompress must reject the cases that are not decompressions
    assert not is_decompress("add", "$0x8,%r14"), "adding to r14 is not a decompression"
    assert not is_decompress("add", "%r14,0x8(%rbx)"), "memory-operand add is not a decompression"
    assert is_decompress("add", "%r14,%rax")
    # _dest_operand's own regression (R180): an indexed store must read as a store
    assert "(" in _dest_operand("%rax,0x8(%rbx,%rcx,1)"), "_dest_operand regressed"
    print("feeder selftest: OK (9 sequences + 4 invariants)")


def dispatch_masks(ins):
    """(tight, wide) bitmaps marking instructions inside an INDIRECT-CALL preamble.

    `shl $0x9` / `shr $0x5` is the WasmCodePointerTable index->address computation, and BOTH `call_indirect`
    AND `call <import>` emit it -- see the module docstring. So `tight` is "time in an indirect-call
    preamble of either form", NOT "time in a call_indirect preamble", and it is USELESS as the metric for an
    import-conversion A/B. `wide` adds the ten instructions before, which for a real call_indirect hold the
    table bounds check and the canonical-type check but for an import hold whatever preceded it.
    """
    n = len(ins)
    tight, wide, count = bytearray(n), bytearray(n), 0
    for j, (_, mn, ops) in enumerate(ins):
        if mn != "shl" or "$0x9," not in ops:
            continue
        for k in range(j, min(n, j + 12)):
            if ins[k][1] == "call" and ins[k][2].startswith("*"):
                for q in range(j, k + 1):
                    tight[q] = 1
                for q in range(max(0, j - 10), k + 1):
                    wide[q] = 1
                count += 1
                break
    return tight, wide, count


def main():
    a = sys.argv[1:]
    if "--selftest" in a:
        _feeder_selftest()
        return
    if not a:
        sys.exit(__doc__.strip().split("Usage:")[1])
    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    outdir = a[0]
    dumps = sorted((os.path.getsize(os.path.join(outdir, f)), f)
                   for f in os.listdir(outdir) if f.startswith("jit-") and f.endswith(".dump"))
    if not dumps:
        sys.exit(f"{outdir}: no jit-*.dump (re-capture with MCPERF_JITDUMP=1)")
    jd = os.path.join(outdir, dumps[-1][1])          # the renderer's, i.e. the largest
    cap = Capture(os.path.join(outdir, "perf.data"), want_chains=False)
    import json
    pj = os.path.join(outdir, "phases.json")
    phases = json.load(open(pj)) if os.path.exists(pj) else None
    t0 = t1 = None
    if opt("--phase"):
        t0, t1 = phase_window(phases, opt("--phase"))
    elif opt("--window"):
        t0, t1 = (float(x) for x in opt("--window").split(","))
    sel = cap.window(t0, t1)
    print(f"{jd}: reading code loads...", file=sys.stderr)
    ivs = load_jitdump(jd)
    st = [x[0] for x in ivs]
    want = opt("--kind", "ours")
    minn = int(opt("--min-samples", "1"))

    per = collections.defaultdict(collections.Counter)
    tot = len(sel)
    resolved = 0
    for _, ip, _, _ in sel:
        i = bisect.bisect_right(st, ip) - 1
        if i < 0 or ip >= ivs[i][1]:
            continue
        resolved += 1
        per[i][ip - ivs[i][0]] += 1
    print(f"{tot} samples in window, {100 * resolved / max(tot,1):.2f}% inside JIT-emitted code, "
          f"{len(per)} functions hit")

    sel_fns = [(i, c) for i, c in per.items()
               if (want == "all" or kind_of(ivs[i][2]) == want) and sum(c.values()) >= minn]
    kindtot = sum(sum(c.values()) for _, c in sel_fns)
    if not kindtot:
        sys.exit(f"no samples for --kind {want}")
    print(f"--kind {want}: {kindtot} samples = {100 * kindtot / tot:.2f}% of the window, "
          f"{len(sel_fns)} functions\n")

    fh = open(jd, "rb")
    mix = collections.Counter()
    bands = collections.Counter()
    feeders = collections.Counter()
    decomp_n = 0
    tight_n = wide_n = ndisp = 0
    for i, offc in sorted(sel_fns, key=lambda kv: -sum(kv[1].values())):
        ca, ce, nme, foff, cs = ivs[i]
        ins = disasm(fh, foff, cs)
        if not ins:
            continue
        offs = [x[0] for x in ins]
        idx = {o: q for q, o in enumerate(offs)}
        tight = wide = None
        if "--dispatch" in a:
            tight, wide, d = dispatch_masks(ins)
            ndisp += d
        for off, c in offc.items():
            q = idx.get(off)
            if q is None:                            # sample landed mid-instruction; take the one covering it
                z = bisect.bisect_right(offs, off) - 1
                if z < 0:
                    continue
                q = z
            mix[classify(ins[q][1], ins[q][2])] += c
            if "--feeder" in a and is_decompress(ins[q][1], ins[q][2]):
                # R190's band boundary is 0x60, deliberately WIDER than --bands' 24 B: the prologue's
                # global.get/set of s.p sits behind the V8 frame setup and the stack-limit check.
                feeders[("entry <=0x60" if off <= 0x60 else "body", feeder_of(ins, q))] += c
                decomp_n += c
            if "--bands" in a:
                bands["PROLOGUE (first 24 B)" if off <= 24 else
                      "EPILOGUE (last 16 B)" if cs - off <= 16 else "body"] += c
            if tight is not None:
                tight_n += c if tight[q] else 0
                wide_n += c if wide[q] else 0

    print(f"=== instruction actually executing ({want}) ===")
    for k, v in mix.most_common(int(opt("--top", "22"))):
        print(f"  {100 * v / kindtot:6.2f}% of {want}  ({100 * v / tot:5.2f}% of window)  {k}")
    print("\n=== grouped ===")
    named = set().union(*GROUPS.values())
    for g, ks in GROUPS.items():
        print(f"  {100 * sum(mix[k] for k in ks) / kindtot:6.2f}%   {g}")
    print(f"  {100 * sum(v for k, v in mix.items() if k not in named) / kindtot:6.2f}%   OTHER")

    if "--bands" in a:
        b = sum(bands.values())
        print("\n=== where in the function ===")
        for k, v in bands.most_common():
            print(f"  {100 * v / b:6.2f}%   {k}")

    if "--feeder" in a:
        print(f"\n=== tagged-pointer decompression (`add %r14,reg`) by band and feeder ===")
        print(f"  pool: {decomp_n} samples = {100 * decomp_n / kindtot:.2f}% of {want}, "
              f"{100 * decomp_n / tot:.2f}% of window")
        if decomp_n:
            for (band, fed), v in feeders.most_common():
                print(f"  {100 * v / decomp_n:6.2f}% of pool  ({100 * v / tot:5.2f}% of window)  {band:14s} {fed}")
            sp = sum(v for (band, fed), v in feeders.items()
                     if band.startswith("entry") and fed == "chained (%r14)")
            print(f"\n  entry-band chained-(%r14) arm = {100 * sp / decomp_n:.1f}% of pool, "
                  f"{100 * sp / tot:.2f}% of window   <- the `s.p` attribution (R190 put it at 95.3% / 5.25%)")
            print("  NOTE: `s.p` is the only MUTABLE import (wasm-encoder.c:344-364), so this arm is its\n"
                  "        signature -- but it is an attribution by shape, not a name. An immutable global\n"
                  "        read does not chain through %r14 (wasm-lowering-reducer.h:1096-1104).")

    if "--dispatch" in a:
        print(f"\n=== call_indirect dispatch preamble ({ndisp} sequences) ===")
        print(f"  {100 * tight_n / kindtot:6.2f}% of {want}   tight  (shl$9 .. call*)")
        print(f"  {100 * wide_n / kindtot:6.2f}% of {want}   wide   (+ bounds/type check and target loads)")


if __name__ == "__main__":
    main()
