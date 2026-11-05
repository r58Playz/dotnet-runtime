#!/usr/bin/env python3
"""
Static virtual-dispatch census over a wasmtier dump: for every VIRTUAL call site the emitter laid
down, did it also get a speculative-devirt predicted arm?

R155 established that 79% of *counted* vcalls take the inline f-slot IC hit -- but the denominator it
used (WJC_FAST_VIC + WJC_FAST_AOTIC + WJC_VIC_HIT + WJC_VIC_MISS) has no term for the predicted arm,
because no WJC_FAST_* counter is emitted on it.  Prediction coverage was therefore never measured.
This reads it off the emitted bytes.

Method.  A full opcode walk of each function body (validated: a body must decode to exactly its
declared end, and the pass reports how many did not).  Two markers, both from the vcall lowering in
mini-wasm.c:

  * VIRTUAL CALL SITE = one `call` to the mono_wasm_jit_vcall_resolve_fslot import.  Every site's cold
    path calls it exactly once (the shared/outlined miss path is off in the shipped config,
    vcall_shared_miss=0), and it is identified by TYPE -- (i32,i32,i32,i32)->i32, the `vtrf` functype --
    not by a hardcoded helper number, because helper slot indices move between builds.  That is the bug
    that made devirtcheck.py report "0 conversions" on a dump with 69 new imports.

  * PREDICTED ARM = i32.load align=2 offset=0 ; i32.const <MonoVTable*> ; i32.ne ; br_if 0
    (mini-wasm.c:11019-11024).  Every other i32.ne in the lowering compares against a local, so the
    const-compare form is specific to the prediction; ordinary IR compares can produce the same shape,
    so the constant must also look like a heap pointer.

Usage: python3 vcallreach.py [--blocks] [--limit N] <tierdump-dir> [more dirs...]

  --blocks  run the call_indirect BLOCK CONTAINMENT pass instead of the devirt census -- see
            check_blocks() for what it tests and why it needs nothing outside the module bytes.
"""
import sys, os, glob
from collections import Counter

def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not (x & 0x80): return r, i

def sleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not (x & 0x80):
            if s < 64 and (x & 0x40): r |= -(1 << s)
            return r, i

def sections(b):
    i = 8
    while i < len(b):
        sid = b[i]; i += 1
        n, i = uleb(b, i)
        yield sid, b[i:i+n]
        i += n

def parse_types(b):
    out = []
    for sid, s in sections(b):
        if sid != 1: continue
        n, i = uleb(s, 0)
        for _ in range(n):
            i += 1                                   # 0x60
            np, i = uleb(s, i); ps = tuple(s[i:i+np]); i += np
            nr, i = uleb(s, i); rs = tuple(s[i:i+nr]); i += nr
            out.append((ps, rs))
        break
    return out

VTRF = ((0x7f,)*4, (0x7f,))                          # (i32,i32,i32,i32) -> i32

def parse_fimports(b, types):
    """(count of imported functions, set of funcidx whose type is vtrf)."""
    n_f = 0; vtrf = set()
    for sid, s in sections(b):
        if sid != 2: continue
        n, i = uleb(s, 0)
        for _ in range(n):
            ln, i = uleb(s, i); i += ln
            ln, i = uleb(s, i); i += ln
            k = s[i]; i += 1
            if k == 0:
                ti, i = uleb(s, i)
                if ti < len(types) and types[ti] == VTRF: vtrf.add(n_f)
                n_f += 1
            elif k == 1:
                i += 1; fl = s[i]; i += 1; _, i = uleb(s, i)
                if fl & 1: _, i = uleb(s, i)
            elif k == 2:
                fl = s[i]; i += 1; _, i = uleb(s, i)
                if fl & 1: _, i = uleb(s, i)
            elif k == 3: i += 2
            elif k == 4: i += 1; _, i = uleb(s, i)
        break
    return n_f, vtrf

# --- opcode operand shapes -------------------------------------------------------------------------
NONE = set(range(0x45, 0xC5)) | {0x00,0x01,0x0F,0x1A,0x1B,0x05,0x0B,0xD1,0xD5,0x18,0x19}
BLOCKY  = {0x02,0x03,0x04,0x06}                       # block/loop/if/try -> blocktype
ULEB1   = {0x0C,0x0D,0x10,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x08,0x07,0x09,0xD2,0x3F,0x40}
MEMARG  = set(range(0x28, 0x3F))                      # loads/stores: align, offset
I32C, I64C, F32C, F64C = 0x41, 0x42, 0x43, 0x44

def arm_form(code, i, end, n_fimports):
    """What CALL FORM the predicted arm dispatches with.

    The arm is straight-line between its guard and its call -- wj_emit_one_call_arg only loads locals and
    memory, it never calls -- so the first call opcode after the `br_if 0` IS the dispatch. This is the
    check that decides whether MONO_WASM_JIT_DIRECT_IMPORT actually reaches the predicted arm: `import`
    means it did (`call <import>`, ~5 x86), `indirect` means the arm still pays V8's full call_indirect
    preamble (~15) despite the target being a compile-time constant.
    """
    j = i
    while j < end and j < i + 512:
        op = code[j]; j += 1
        if op == 0x10:
            v, _ = uleb(code, j)
            return 'local' if v >= n_fimports else 'import'
        if op == 0x11:
            return 'indirect'
        # skip immediates of the load/store/const/local ops the argument sequence is made of
        if op in MEMARG: _, j = uleb(code, j); _, j = uleb(code, j)
        elif op in ULEB1: _, j = uleb(code, j)
        elif op == I32C or op == I64C: _, j = sleb(code, j)
        elif op == F32C: j += 4
        elif op == F64C: j += 8
        elif op in BLOCKY:
            bt = code[j]
            if bt == 0x40 or bt in (0x7f,0x7e,0x7d,0x7c,0x70,0x6f): j += 1
            else: _, j = sleb(code, j)
        elif op == 0xFE:
            sub, j = uleb(code, j)
            if sub == 3: j += 1
            else: _, j = uleb(code, j); _, j = uleb(code, j)
    return 'none'

def walk(code, start, end, vtrf_idx, out, n_fimports=0, ci_sink=None):
    """Decode one function body; return True if it lands exactly on `end`."""
    i = start
    prev_load = -1                                    # position of a just-seen i32.load align=2 offset=0
    pend_const = None
    while i < end:
        op = code[i]; here = i; i += 1
        if op in BLOCKY:
            bt = code[i]
            if bt == 0x40 or bt in (0x7f,0x7e,0x7d,0x7c,0x70,0x6f): i += 1
            else: _, i = sleb(code, i)
        elif op == 0x0E:                              # br_table
            n, i = uleb(code, i)
            for _ in range(n + 1): _, i = uleb(code, i)
        elif op in ULEB1:
            v, i = uleb(code, i)
            if op == 0x10:
                out['call'] += 1
                if v in vtrf_idx: out['vcall_sites'] += 1
            elif op == 0x0D and pend_const is not None and here == pend_const[1] + 1:
                pass                                  # handled below via the ne/br_if pair
        elif op == 0x11:                              # call_indirect: typeidx, tableidx
            ti, i = uleb(code, i); _, i = uleb(code, i)
            out['call_indirect'] += 1
            if ci_sink is not None: ci_sink.append(ti)
        elif op in MEMARG:
            a, i = uleb(code, i); o, i = uleb(code, i)
            if op == 0x28 and a == 2 and o == 0: prev_load = here
            # DELEGATE SITE marker: the cold delegate route consume-clears the TLS recipe scratch with
            # i32.store offset=220 (shape) and offset=244 (fslot) before entering managed code
            # (mini-wasm.c:11637-11642). Emitted exactly once per Delegate.Invoke site and by nothing
            # else, so it separates delegate sites from ordinary virtual ones inside the resolve_fslot
            # population -- which matters because the devirt prediction gate excludes delegates outright.
            if op == 0x36 and a == 2 and o == 220: out['delegate_sites'] += 1
        elif op == I32C:
            v, i = sleb(code, i)
            pend_const = (v, here) if prev_load >= 0 else None
        elif op == I64C: _, i = sleb(code, i)
        elif op == F32C: i += 4
        elif op == F64C: i += 8
        elif op == 0xFC:
            sub, i = uleb(code, i)
            if sub in (8,): _, i = uleb(code, i); i += 1
            elif sub in (9,11): _, i = uleb(code, i)
            elif sub in (10,): i += 2
        elif op == 0xFE:                              # ATOMICS -- this is a THREADED build: every
            sub, i = uleb(code, i)                    # i64.atomic.load in the PIC guards and every
            if sub == 3: i += 1                       # atomic rmw.add from PROFILE_FAST lands here.
            else: _, i = uleb(code, i); _, i = uleb(code, i)
        elif op == 0xFD:                              # SIMD (rare: 9 bodies in the tier)
            sub, i = uleb(code, i)
            if sub <= 11: _, i = uleb(code, i); _, i = uleb(code, i)
            elif sub in (12, 13): i += 16
            elif 84 <= sub <= 91: _, i = uleb(code, i); _, i = uleb(code, i); i += 1
            elif 21 <= sub <= 34: i += 1
        elif op in NONE:
            pass
        else:
            return False
        # PREDICTED ARM: i32.load(2,0) ; i32.const K ; i32.ne ; br_if 0
        if op == 0x47 and pend_const is not None and code[i:i+2] == b'\x0d\x00':
            v = pend_const[0] & 0xffffffff
            if v >= 0x10000:
                out['pred'] += 1
                out['arm_' + arm_form(code, i + 2, end, n_fimports)] += 1
        if op not in (0x28, I32C): prev_load = -1; pend_const = None
    return i == end

def scan(path):
    b = open(path, 'rb').read()
    if b[:4] != b'\0asm': return None
    types = parse_types(b)
    n_f, vtrf = parse_fimports(b, types)
    out = Counter()
    bad = 0; nbody = 0
    for sid, s in sections(b):
        if sid != 10: continue
        n, i = uleb(s, 0)
        for _ in range(n):
            sz, i = uleb(s, i)
            end = i + sz
            nl, j = uleb(s, i)                       # locals decl
            for _ in range(nl):
                _, j = uleb(s, j); j += 1
            nbody += 1
            if not walk(s, j, end, vtrf, out, n_f): bad += 1
            i = end
        break
    out['bodies'] = nbody; out['undecoded'] = bad
    return out

def parse_funcsec(b):
    """The function section: one type index per DEFINED function, methods then thunks."""
    for sid, s in sections(b):
        if sid != 3: continue
        n, i = uleb(s, 0)
        out = []
        for _ in range(n):
            v, i = uleb(s, i); out.append(v)
        return out
    return []


def is_ours(b):
    """
    Only modules THIS emitter framed. The dump is every wasm module V8 compiled during the run, which
    includes the jiterpreter's trace and interp-entry modules -- those import `i.<n>` functions and export
    `<name>`/`<name>_direct`, and their function section is not laid out in member blocks at all, so they
    produce pure false positives in the containment pass (16 of the first 300 modules).

    The discriminator is the import set: every module wasm_module_assemble frames declares the nine (now
    ten) `s.*` globals unconditionally, "because the emitter's indices are absolute". `s.p` is present in
    all of them and in nothing else.
    """
    for sid, sec in sections(b):
        if sid != 2: continue
        n, i = uleb(sec, 0)
        for _ in range(n):
            ln, i = uleb(sec, i); mod = sec[i:i+ln]; i += ln
            ln, i = uleb(sec, i); fld = sec[i:i+ln]; i += ln
            k = sec[i]; i += 1
            if k == 0: _, i = uleb(sec, i)
            elif k == 1:
                i += 1; fl = sec[i]; i += 1; _, i = uleb(sec, i)
                if fl & 1: _, i = uleb(sec, i)
            elif k == 2:
                fl = sec[i]; i += 1; _, i = uleb(sec, i)
                if fl & 1: _, i = uleb(sec, i)
            elif k == 3:
                i += 2
                if mod == b's' and fld == b'p': return True
            elif k == 4: i += 1; _, i = uleb(sec, i)
        return False
    return False


def check_blocks(path, ntypes_total_cache=None):
    """
    BLOCK CONTAINMENT: every call_indirect must name a type inside its OWN member's block.

    This is the one term R267 addendum 16 left unverified. Both sides of the baked call_indirect had been
    checked at time of use -- the dependency installed, the occupant a real function of the right arity,
    want_sig == have_sig -- and it still trapped. What was never checked is the index the CALLER asks for.
    The assembler resolves it as `ti = ti_base + r->tpool` (wasm-encoder.c), recomputed on EVERY reassembly,
    and re-emission re-frames bodies constantly. A body relocated against the wrong block lands on some
    other member's functype, which is a `function signature mismatch` with everything else looking clean.

    Nothing outside the module is needed to test it. The FUNCTION SECTION already states where each block
    begins: member i's method has type funcsec[i] and its entry thunk funcsec[n+i], and the layout contract
    (wasm-encoder.c: "member i occupies [ti_base_i .. ti_base_i + ntypes_i - 1]", block = {method, entry,
    extras...}) makes block i exactly [funcsec[i], funcsec[i+1]). So the check is derived from the bytes,
    not from re-running the emitter's arithmetic -- which is the point, since re-deriving the formula would
    only prove the formula equals itself. enctest t5 covers the formula; this covers the tier.

    Returns a Counter, or None if the module is not one of ours.
    """
    b = open(path, 'rb').read()
    if b[:4] != b'\0asm': return None
    if not is_ours(b): return Counter(foreign=1)
    types = parse_types(b)
    funcsec = parse_funcsec(b)
    n_f, vtrf = parse_fimports(b, types)
    c = Counter()
    if not funcsec or len(funcsec) % 2:
        c['odd_funcsec'] = 1; return c
    n = len(funcsec) // 2
    c['modules'] = 1; c['members'] = n

    # Block boundaries, read off the function section.
    base = [funcsec[i] for i in range(n)]
    for i in range(n):
        # The entry thunk is the SECOND entry of its member's block. This is the direct test of
        # `ntypes == 2 + nextra`, the invariant wasm-encoder.c records as having no assert any more.
        if funcsec[n + i] != base[i] + 1:
            c['thunk_not_base_plus_1'] += 1
    for i in range(1, n):
        if base[i] <= base[i - 1] + 1:
            c['block_not_monotonic'] += 1
    end = [base[i + 1] if i + 1 < n else len(types) for i in range(n)]

    for sid, s in sections(b):
        if sid != 10: continue
        cnt, i = uleb(s, 0)
        if cnt != 2 * n: c['code_vs_funcsec'] += 1
        for k in range(cnt):
            sz, i = uleb(s, i)
            fin = i + sz
            nl, j = uleb(s, i)
            for _ in range(nl):
                _, j = uleb(s, j); j += 1
            m = k if k < n else k - n
            sink = []
            ok = walk(s, j, fin, vtrf, c, n_f, ci_sink=sink)
            c['bodies'] += 1
            if not ok:
                c['undecoded'] += 1
                i = fin; continue
            for ti in sink:
                c['ci'] += 1
                if ti >= len(types):
                    c['ci_past_typesec'] += 1
                elif m < n and not (base[m] <= ti < end[m]):
                    c['ci_out_of_block'] += 1
                    if c['ci_out_of_block'] <= 8:
                        print(f"  OUT OF BLOCK {os.path.basename(path)}: member {m}/{n} body {k} "
                              f"asks type {ti}, block is [{base[m]},{end[m]})")
            i = fin
        break
    return c


def blocks_main(roots, limit=0):
    for root in roots:
        files = sorted(glob.glob(os.path.join(root, 'mods', '*.wasm')))
        if limit: files = files[:limit]
        t = Counter()
        for p in files:
            r = check_blocks(p)
            if r: t.update(r)
        print(f"\n=== {root} : call_indirect BLOCK CONTAINMENT ===")
        print(f"  modules {t['modules']:,}   members {t['members']:,}   bodies {t['bodies']:,}"
              f"   (skipped {t['foreign']:,} foreign modules -- jiterpreter traces and interp entries)")
        print(f"  undecoded bodies           : {t['undecoded']:,}  (must be 0, or nothing below means anything)")
        print(f"  call_indirect sites        : {t['ci']:,}")
        print(f"  >> OUT OF THEIR OWN BLOCK  : {t['ci_out_of_block']:,}")
        print(f"  >> past the type section   : {t['ci_past_typesec']:,}")
        print(f"  entry thunk not at base+1  : {t['thunk_not_base_plus_1']:,}  (i.e. ntypes != 2 + nextra)")
        print(f"  blocks not monotonic       : {t['block_not_monotonic']:,}")
        print(f"  code/funcsec count mismatch: {t['code_vs_funcsec']:,}   odd function section: {t['odd_funcsec']:,}")


def main(roots):
    for root in roots:
        files = sorted(glob.glob(os.path.join(root, 'mods', '*.wasm')))
        t = Counter(); nmod = 0; withsite = 0; withpred = 0
        for p in files:
            r = scan(p)
            if not r: continue
            nmod += 1; t.update(r)
            if r['vcall_sites']: withsite += 1
            if r['pred']: withpred += 1
        pc = lambda a, b: f"{100*a/b:.1f}%" if b else "?"
        print(f"\n=== {root} ===")
        print(f"  modules {nmod:,}   bodies {t['bodies']:,}   undecoded bodies {t['undecoded']:,} "
              f"({pc(t['undecoded'], t['bodies'])} -- must be ~0 for the rest to mean anything)")
        dele = t['delegate_sites']; virt = t['vcall_sites'] - dele
        print(f"  dispatch sites emitted     : {t['vcall_sites']:,}  (in {withsite:,} modules)")
        print(f"    of which Delegate.Invoke : {dele:,} ({pc(dele, t['vcall_sites'])}) -- devirt is DISABLED for these")
        print(f"    ordinary virtual         : {virt:,}")
        print(f"  devirt predicted arms      : {t['pred']:,}  (in {withpred:,} modules)")
        print(f"  => PREDICTION COVERAGE     : {pc(t['pred'], virt)} of the ordinary-virtual sites"
              f"  ({pc(t['pred'], t['vcall_sites'])} of all dispatch sites)")
        print(f"  predicted-arm CALL FORM    : import {t['arm_import']:,}  local {t['arm_local']:,}"
              f"  call_indirect {t['arm_indirect']:,}  unresolved {t['arm_none']:,}")
        print(f"  calls {t['call']:,}   call_indirect {t['call_indirect']:,} "
              f"({pc(t['call_indirect'], t['call'] + t['call_indirect'])} of all calls)")

if __name__ == '__main__':
    av = sys.argv[1:]
    lim = 0
    if '--limit' in av:
        k = av.index('--limit'); lim = int(av[k + 1]); del av[k:k + 2]
    if '--blocks' in av:
        av.remove('--blocks')
        blocks_main(av or ['tier-i1'], lim)
    else:
        main(av or ['tier-i1'])
