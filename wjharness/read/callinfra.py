#!/usr/bin/env python3
"""How much of ONE thread's executed instructions is CALL INFRASTRUCTURE? From an instruction-weighted
jitdump capture:

    MCPERF_EVENT=instructions MCPERF_PERIOD=2000000 node mcperf.mjs <out> --jitdump \
        --warm-ms 180000 --bench-ms 120000 --no-walk

WHY THIS EXISTS, next to hotinsn.py
-----------------------------------
hotinsn.py has no thread filter (so the server tick and the client frame are pooled), and its --dispatch
mask is a fixed 10-instruction window behind `shl $0x9`, which cannot tell `call_indirect` from `call
<import>` (its own docstring says so). This reads the SAME code bytes but, at every `call *`, takes the
DATA-FLOW backward slice of the registers the call consumes (target table base, code-pointer index, the
implicit instance argument in %rsi) plus the checks that guard them. That separates the forms by what the
slice CONTAINS, as V8 15.4 emits them (verified against perf-mg0/jit-752929.dump, 2026-09-22):

  call_indirect : dispatch-table load, BOUNDS check (`cmp %idx,0x7(%tbl); jbe`), `imul $0xc` (12-byte
                  WasmDispatchTable entry), canonical-SIG check (`mov $id; cmp; jne`), code-pointer and
                  implicit-arg loads, then the common tail. ~17 instructions.
  call <import> : imported-function-target loads off the instance, then the common tail. ~12.
  common tail   : `mov table,%r10; shl $9; shr $5; cmpq $sighash,8(%r10,%idx); je; int3; call *`
                  (the WasmCodePointerTable conversion + sandbox signature-hash check; BOTH forms).
  module-local  : `call rel32`. One instruction. (V8 routes it through the module jump table.)

A constant-index call_indirect (our devirt arms, HELPER_CI) may have its bounds check folded; it still
carries the sig check and the table load, so `jne`/`imul` classify it. VERIFY with --show before quoting.

WHAT IT REPORTS (per thread, instruction-weighted, % of the thread and M instr/tick if --ticks)
  * our tier: V8 frame prologue (push rbp .. stack-check jcc), entry band (our s.p/frame/pin/IC-preload
    prologue, bounded at 0x60 or the first call), dispatch slices by form, the call instructions,
    epilogues, and call-adjacent spill/reload (an ESTIMATE: +-12 instructions of a call).
  * AOT image (dotnet.native.wasm): the same split for its own functions, plus its named JIT-tier helpers.
  * V8 builtins.
  * WHO ENTERS OUR TIER, by call form: every sample in an `ours` function's frame prologue (after `mov
    %rsp,%rbp`, so the frame-pointer chain names the DIRECT caller) is attributed to the call site that
    entered it. Prologue length is fixed per V8, so prologue samples are proportional to CALL COUNT. This is
    the execution-weighted answer to "what fraction of calls into our tier are module-local / constant
    call_indirect (co-location-reachable) / dynamic call_indirect (IC, not reachable) / from AOT or JS".

Usage:
  callinfra.py <outdir> --tid N [--phase ingame] [--ticks N] [--show FORM] [--max-funcs N]
  callinfra.py --selftest-static <jitdump> [--n N]   # classify call sites in N functions, print examples
"""
import bisect
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perfraw import Capture, phase_window            # noqa: E402
from hotinsn import load_jitdump, disasm, kind_of, classify, _norm_reg, _dest_operand  # noqa: E402

REGRE = re.compile(r"%[a-z0-9]+")
JCC = re.compile(r"^j(?!mp)[a-z]{1,3}$")
RMW = re.compile(r"^(add|sub|or|and|xor|shl|shr|sar|imul|adc|sbb|rol|ror|inc|dec|neg|not)")
NOWRITE = ("cmp", "test", "push", "call", "ret", "jmp", "int3", "nop", "ud2", "bt")


def regs_in(s):
    out = set()
    for r in REGRE.findall(s):
        n = _norm_reg(r)
        if n and n not in ("rip",):
            out.add(n)
    return out


def split_ops(ops):
    """Top-level operands (commas inside parens are not separators)."""
    out, depth, cur = [], 0, ""
    for ch in ops:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return [o.strip() for o in out]


def rw(mn, ops):
    """(written_reg_or_None, read_regs) for one AT&T instruction."""
    parts = split_ops(ops)
    if not parts:
        return None, set()
    dst = parts[-1]
    reads = set()
    for p in parts[:-1]:
        reads |= regs_in(p)
    written = None
    if "(" in dst:                       # memory destination: its address regs are READ
        reads |= regs_in(dst)
    elif mn.startswith(NOWRITE) or JCC.match(mn):
        reads |= regs_in(dst)
    else:
        w = regs_in(dst)
        written = next(iter(w)) if len(w) == 1 else None
        if RMW.match(mn) and len(parts) > 1 and not (mn.startswith("xor") and parts[0] == parts[-1]):
            reads |= w
        if len(parts) == 1 and RMW.match(mn):   # shl %r, sar $1 forms
            reads |= w
    return written, reads


def dispatch_slice(ins, j, window=34):
    """Indices forming the dispatch slice of the `call *` at ins[j], and its form."""
    _, mn, ops = ins[j]
    # %rbp/%rsp (frame), %r13 (V8 root register), %r14 (cage base) are never SET UP by a call preamble;
    # keeping them "needed" makes the walk run past the preamble's start and swallow unrelated guards.
    FIXED = {"rbp", "rsp", "r13", "r14", "rip"}
    need = (regs_in(ops) | {"rsi"}) - FIXED
    inc = set([j])
    cmps = []
    defined = set()
    for k in range(j - 1, max(-1, j - 1 - window), -1):
        _, m, o = ins[k]
        if m == "call" or m.startswith("ret") or m == "jmp":
            break
        w, r = rw(m, o)
        if m.startswith(("cmp", "test")):
            if r & need or any(x in need for x in regs_in(o)):
                inc.add(k)
                cmps.append(k)
            continue
        if w and w in need:
            inc.add(k)
            need.discard(w)
            need |= r - FIXED
            defined.add(w)
            if RMW.match(m) and not (m.startswith("xor")):
                need.add(w)
        if not need:
            break
    # Checks that guard the slice without feeding the call: a load off a slice register (the dispatch-table
    # entry's canonical sig id) compared, possibly against a `mov $imm`, then a jcc. call_indirect's sig check.
    lo = min(inc)
    for k in range(lo, j):
        if k in inc:
            continue
        _, m, o = ins[k]
        if not m.startswith("mov") or "(" not in o:
            continue
        w, r = rw(m, o)
        if not w or not (r & defined):
            continue
        for c in range(k + 1, min(j, k + 4)):
            if ins[c][1].startswith("cmp") and w in regs_in(ins[c][2]):
                inc.add(k)
                cmps.append(c)
                inc.add(c)
                for z in range(k + 1, c):
                    if ins[z][1] == "mov" and ins[z][2].startswith("$") and (regs_in(ins[z][2]) & regs_in(ins[c][2])):
                        inc.add(z)
                break
    # the jcc / int3 guarding each included compare
    for c in cmps:
        for q in range(c + 1, min(j, c + 4)):
            if JCC.match(ins[q][1]) or ins[q][1] == "int3":
                inc.add(q)
            if JCC.match(ins[q][1]):
                break
    body = [ins[q] for q in sorted(inc)]
    has_imul = any(m == "imul" and "$0xc" in o for _, m, o in body)
    has_bound = any(JCC.match(m) and m in ("jbe", "ja", "jae", "jb", "jnb", "jnae") for _, m, o in body)
    has_ne = any(m == "jne" for _, m, o in body)
    if "(%r10," in ops or re.match(r"^\*\(%r\w+,%r\w+,1\)$", ops):
        form = "call_indirect" if (has_imul or has_bound or has_ne) else "import"
    else:
        form = "call *reg (C/builtin)"
    # INDEX COMPUTATION vs V8's fixed preamble. The bounds check `cmp %idx,0x7(%tbl); jbe` is where V8's
    # call_indirect lowering starts consuming the wasm i32 index; whatever in the slice produced %idx
    # before it is OUR emitted code (the IC's PIC probe, a devirt arm's constant). A folded constant
    # index (`cmpl $imm,0x7(%tbl)`) has none.
    index = set()
    if form == "call_indirect":
        bc = None
        for q in sorted(inc):
            if ins[q][1].startswith("cmp") and q + 1 < len(ins) and ins[q + 1][1] in ("jbe", "ja", "jae", "jb"):
                bc = q
                break
        if bc is not None:
            parts = split_ops(ins[bc][2])
            idx = regs_in(parts[0]) if parts and "(" not in parts[0] else set()
            want = set(idx) - FIXED
            for q in sorted((x for x in inc if x < bc), reverse=True):
                w, r = rw(ins[q][1], ins[q][2])
                if w and w in want:
                    index.add(q)
                    want.discard(w)
                    want |= r - FIXED
                    if RMW.match(ins[q][1]):
                        want.add(w)
    # CONSTANT vs DYNAMIC index. A constant `i32.const K; call_indirect` (a devirt arm, a direct call to a
    # non-co-resident JIT callee, a HELPER_CI icall) has its bounds check against an IMMEDIATE, or folded
    # into a large displacement, and no index computation; an IC hit / delegate loads the index at run time.
    # Only the constant kind can become a module-local `call` by co-location.
    if form == "call_indirect":
        dyn = bool(index)
        if not dyn:
            for q in sorted(inc):
                if ins[q][1].startswith("cmp") and q + 1 < len(ins) and ins[q + 1][1] in ("jbe", "ja", "jae", "jb"):
                    parts = split_ops(ins[q][2])
                    dyn = bool(parts) and parts[0].startswith("%")
                    break
        form = "call_indirect(" + ("dynamic idx" if dyn else "const idx") + ")"
    return inc, form, index


def analyse_function(ins):
    """Per-instruction role for one function. Returns (roles list, site-form-by-return-offset)."""
    n = len(ins)
    role = ["body"] * n
    # V8 frame prologue: push rbp ... the first stack-limit compare's jcc, within the first 24 instructions
    pro_end = -1
    for q in range(min(n, 24)):
        _, m, o = ins[q]
        if m.startswith("cmp") and "%rsp" in o:
            for z in range(q + 1, min(n, q + 3)):
                if JCC.match(ins[z][1]):
                    pro_end = z
                    break
            break
    for q in range(pro_end + 1):
        role[q] = "v8 prologue"
    sites = {}
    first_call = n
    for j, (off, m, o) in enumerate(ins):
        if m == "call":
            first_call = min(first_call, j)
            if o.startswith("*"):
                inc, form, index = dispatch_slice(ins, j)
                for q in inc:
                    if role[q] == "body":
                        role[q] = ("dispatch-index (our IC/arm code):" if q in index else "dispatch:") + form
                role[j] = "call-insn:" + form
            else:
                form = "direct rel32"
                role[j] = "call-insn:" + form
            ret_off = ins[j + 1][0] if j + 1 < n else None
            if ret_off is not None:
                sites[ret_off] = form
    # EAGER-FRAME PROLOGUE, recognised by its signature rather than by position. The emitter's prologue is
    # `global.get s.p; i32.sub framebytes; i32.and -16; global.set s.p; zero; pin` and V8 keeps the
    # `and $0xfffffff0` on the new SP. A band bounded only by position (the first version of this tool:
    # "0x60 bytes or the first call") swallowed the BODY of lazy/frameless leaf functions -- Vec3i.getX
    # has no hot-path s.p access at all and read as 63% "prologue" (R290 correction).
    and_q = None
    for q in range(pro_end + 1, min(n, pro_end + 1 + 40)):
        _, m, o = ins[q]
        if m == "call" or JCC.match(m):
            break
        if m == "and" and o.startswith("$0xfffffff0,"):
            and_q = q
            break
    if and_q is not None:
        end_q = and_q
        for q in range(and_q + 1, min(n, and_q + 40)):
            _, m, o = ins[q]
            if m == "call" or JCC.match(m):
                break
            if (m.startswith("mov") and "(" in _dest_operand(o) and not re.search(r"\((%rbp|%rsp)", _dest_operand(o))):
                end_q = q          # s.p store-back, a zeroing store or a pin store into the new frame
        for q in range(pro_end + 1, end_q + 1):
            if role[q] == "body":
                role[q] = "entry band (s.p/frame/pins)"
    # epilogues: mov %rbp,%rsp ; pop %rbp ; ret   (and leave / ret)
    for j, (off, m, o) in enumerate(ins):
        if m.startswith("ret"):
            role[j] = "epilogue"
            for q in range(j - 1, max(-1, j - 3), -1):
                mm, oo = ins[q][1], ins[q][2]
                if (mm == "pop" and "%rbp" in oo) or (mm == "mov" and oo == "%rbp,%rsp") or mm == "leave":
                    role[q] = "epilogue"
                else:
                    break
    # call-adjacent spill/reload: within 12 instructions before (spill) / after (reload) any call
    calls = [j for j, (_, m, _) in enumerate(ins) if m == "call"]
    for j in calls:
        for q in range(max(0, j - 12), min(n, j + 13)):
            if role[q] == "body" and classify(ins[q][1], ins[q][2]) == "SPILL/RELOAD (rbp/rsp)":
                role[q] = "near-call spill/reload"
    return role, sites, pro_end


def band_sub(ins, q):
    """Which part of the eager-frame prologue instruction q is (--band-split). The band is
    [pro_end+1 .. last store after `and $0xfffffff0`]; see the EAGER-FRAME comment above."""
    and_q = None
    for k in range(max(0, q - 60), min(len(ins), q + 60)):
        if ins[k][1] == "and" and ins[k][2].startswith("$0xfffffff0,"):
            if k <= q or and_q is None:
                and_q = k
            if k > q:
                break
    m, o = ins[q][1], ins[q][2]
    if and_q is None:
        return "?no and"
    if q < and_q:
        return "A s.p read chain + frame arith (before and)"
    if q == and_q:
        return "B and $-16"
    newsp = _dest_operand(ins[and_q][2])
    dst = _dest_operand(o) if "," in o else ""
    src = o.split(",")[0] if "," in o else ""
    if m.startswith("mov") and "(" in dst:
        if src == newsp:
            return "C s.p write-back (store of new SP)"
        if src.startswith("$0x0") or src == "$0x0":
            return "D frame zeroing (imm 0 store)"
        if src.startswith("%xmm") or src.startswith("%ymm"):
            return "D frame zeroing (vector store)"
        return "E pin / other store into frame"
    if m in ("xor", "xorl", "pxor", "vpxor", "xorps", "vxorps"):
        return "D frame zeroing (zero a reg)"
    return "F other (lea/mov reg/loads after and)"


def mov_rbp_index(ins):
    for q in range(min(len(ins), 6)):
        if ins[q][1] == "mov" and ins[q][2] == "%rsp,%rbp":
            return q
    return 1


HELPER = re.compile(r"(mono_wasm_jit_|wj_|wasm_jit_|get_virtual_method_fast|ldvirtfn|do_jit_call|interp_entry"
                    r"|mono_jiterp_|call_interp)")


def selftest_static(jd, nfun):
    ivs = load_jitdump(jd)
    forms = collections.Counter()
    ex = collections.defaultdict(list)
    with open(jd, "rb") as fh:
        k = 0
        for (s, e, name, foff, cs) in ivs:
            if kind_of(name) != "ours" or "turbofan" not in name:
                continue
            k += 1
            if k > nfun:
                break
            ins = disasm(fh, foff, cs)
            for j, (off, m, o) in enumerate(ins):
                if m == "call" and o.startswith("*"):
                    inc, form, _ = dispatch_slice(ins, j)
                    forms[form] += 1
                    if len(ex[form]) < 3:
                        ex[form].append((name[:90], [f"{'*' if q in inc else ' '} {ins[q][1]} {ins[q][2]}"
                                                     for q in range(max(0, j - 22), j + 1)], len(inc)))
    print(f"{k - 1} functions: {dict(forms)}")
    for f, lst in ex.items():
        for nm, lines, ln in lst:
            print(f"\n== {f}  slice={ln} instrs  in {nm}")
            for L in lines:
                print("    " + L)


def main():
    a = sys.argv[1:]
    if a and a[0] == "--selftest-static":
        selftest_static(a[1], int(a[a.index("--n") + 1]) if "--n" in a else 40)
        return
    if not a:
        sys.exit(__doc__.strip().split("Usage:")[1])

    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    outdir = a[0]
    tid = int(opt("--tid"))
    ticks = float(opt("--ticks", "0"))
    dumps = sorted((os.path.getsize(os.path.join(outdir, f)), f)
                   for f in os.listdir(outdir) if f.startswith("jit-") and f.endswith(".dump"))
    jd = os.path.join(outdir, dumps[-1][1])
    cap = Capture(os.path.join(outdir, "perf.data"), want_chains=True)
    phases = json.load(open(os.path.join(outdir, "phases.json")))
    t0, t1 = phase_window(phases, opt("--phase", "ingame"))
    period = float(phases.get("period") or 2000000) if isinstance(phases, dict) else 2e6
    sel = [s for s in cap.window(t0, t1) if s[0] == tid]
    print(f"{len(sel):,} samples on tid {tid} in [{t0:.1f},{t1:.1f}]", file=sys.stderr)
    ivs = load_jitdump(jd)
    st = [x[0] for x in ivs]
    CTX = 0xFFFFFFFFFFFFF001

    def fn_of(ip):
        i = bisect.bisect_right(st, ip) - 1
        return i if (i >= 0 and ip < ivs[i][1]) else None

    per = collections.defaultdict(collections.Counter)
    chains_for = collections.defaultdict(list)       # fn index -> [(off, chain)] for prologue attribution
    unres = 0
    for (_, ip, _, ch) in sel:
        i = fn_of(ip)
        if i is None:
            unres += 1
            continue
        off = ip - ivs[i][0]
        per[i][off] += 1
        if ch is not None and off < 0x40:
            chains_for[i].append((off, ch))
    tot = len(sel)

    fh = open(jd, "rb")
    cache = {}

    def analysed(i):
        if i not in cache:
            ca, ce, nm, foff, cs = ivs[i]
            ins = disasm(fh, foff, cs)
            role, sites, pro_end = analyse_function(ins) if ins else ([], {}, -1)
            cache[i] = (ins, role, sites, pro_end)
        return cache[i]

    buckets = collections.Counter()      # (kind, role) -> samples
    helper = collections.Counter()
    v8 = collections.Counter()
    entry = collections.Counter()        # (callee kind, caller kind, form) -> prologue samples
    edges = collections.Counter()        # (callee name, caller name, form) -> prologue samples (--edges)
    bandsplit = "--band-split" in sys.argv
    band = collections.Counter()         # --band-split: entry-band part -> samples
    bandins = collections.Counter()      # --band-split: normalised entry-band instruction -> samples
    bandfn = collections.Counter()       # --band-split: function -> entry-band samples
    bandoff = collections.Counter()      # --band-split: (function, insn index) -> samples
    maxf = int(opt("--max-funcs", "1000000"))
    order = sorted(per.items(), key=lambda kv: -sum(kv[1].values()))[:maxf]
    for i, offc in order:
        nm = ivs[i][2]
        kind = kind_of(nm)
        if kind == "v8":
            v8[nm.split("-")[0][:70]] += sum(offc.values())
            continue
        ins, role, sites, pro_end = analysed(i)
        if not ins:
            continue
        offs = [x[0] for x in ins]
        base = "helper" if (kind == "aot" and HELPER.search(nm)) else ("thunk" if " [entry]" in nm else kind)
        if base == "helper":
            helper[re.sub(r"-\d+-(turbofan|liftoff)$", "", nm)[3:70]] += sum(offc.values())
        for off, c in offc.items():
            q = bisect.bisect_right(offs, off) - 1
            if q < 0:
                continue
            r = role[q]
            if r == "body":
                r = "body:" + classify(ins[q][1], ins[q][2])
            buckets[(base, r)] += c
            if bandsplit and base == "ours" and r.startswith("entry band"):
                band[band_sub(ins, q)] += c
                bandfn[i] += c
                bandoff[(i, q)] += c
                bandins[re.sub(r"0x[0-9a-f]+", "N", f"{ins[q][1]} {ins[q][2]}")] += c
        # who entered this function: prologue samples after `mov %rsp,%rbp`
        if kind in ("ours", "aot"):
            mr = mov_rbp_index(ins)
            lo = offs[mr + 1] if mr + 1 < len(offs) else 4
            hi = offs[pro_end] if pro_end >= 0 else 0x20
            for off, ch in chains_for.get(i, ()):
                if not (lo <= off <= hi):
                    continue
                addrs = [x for x in ch if x < CTX]
                if len(addrs) < 2:
                    entry[(base, "no-chain", "-")] += 1
                    continue
                ret = addrs[1]
                ci = fn_of(ret)
                if ci is None:
                    entry[(base, "native/unknown", "-")] += 1
                    continue
                cnm = ivs[ci][2]
                ck = kind_of(cnm)
                if ck == "v8":
                    entry[(base, "v8:" + cnm.split("-")[0][3:40], "-")] += 1
                    continue
                cins, _, csites, _ = analysed(ci)
                form = csites.get(ret - ivs[ci][0], "?no call before ret")
                cbase = "helper" if (ck == "aot" and HELPER.search(cnm)) else ("thunk" if " [entry]" in cnm else ck)
                entry[(base, cbase, form)] += 1
                edges[(nm, cnm, form)] += 1

    def row(label, n):
        s = f"  {100 * n / tot:6.2f}%"
        if ticks:
            s += f"  {n * 2e6 / ticks / 1e6:7.2f} M/tick"
        print(f"{s}   {label}")

    print(f"\n{tot:,} samples, tid {tid}; {100 * unres / tot:.2f}% outside JIT/wasm code (native DSOs)")
    for base in ("ours", "thunk", "aot", "helper"):
        bt = sum(v for (b, r), v in buckets.items() if b == base)
        print(f"\n=== {base}: {100 * bt / tot:.2f}% of thread ===")
        callish = 0
        for (b, r), v in sorted(buckets.items(), key=lambda kv: -kv[1]):
            if b != base or r.startswith("body:"):
                continue
            row(r, v)
            callish += v
        print(f"  {'-' * 40}")
        row("CALL INFRA subtotal (above rows)", callish)
        rest = collections.Counter()
        for (b, r), v in buckets.items():
            if b == base and r.startswith("body:"):
                rest[r[5:]] += v
        for r, v in rest.most_common(8):
            row("body: " + r, v)
    print("\n=== JIT-tier helpers in the AOT image (self) ===")
    for k, v in helper.most_common(14):
        row(k, v)
    print("\n=== V8 builtins (self) ===")
    for k, v in v8.most_common(10):
        row(k, v)
    et = sum(entry.values())
    print(f"\n=== WHO ENTERS WHOM, by call form ({et:,} prologue samples; proportional to call count) ===")
    for (callee, caller, form), v in sorted(entry.items(), key=lambda kv: -kv[1])[:30]:
        print(f"  {100 * v / et:6.2f}%   into {callee:6s} from {caller:24s} via {form}")
    if bandsplit:
        bt = sum(band.values())
        print(f"\n=== ENTRY BAND split ({bt:,} samples) ===")
        for k, v in sorted(band.items()):
            row(k, v)
        print("  -- top instructions")
        for k, v in bandins.most_common(25):
            row(k[:70], v)
        for fi, fv in bandfn.most_common(2):
            fins, frole, _, fpe = cache[fi]
            print(f"\n  -- prologue of {ivs[fi][2][:90]}  ({fv} band samples)")
            for q in range(0, min(len(fins), 60)):
                if frole[q] in ("body",) and q > fpe + 45:
                    break
                print(f"   {bandoff.get((fi, q), 0):5d}  {frole[q][:14]:14s} {fins[q][1]} {fins[q][2]}")
    ef = opt("--edges")
    if ef:
        # one row per (callee, caller, form): prologue samples, i.e. proportional to that edge's call count
        with open(ef, "w") as f:
            f.write("samples\tcallee\tcaller\tform\n")
            for (ce, cr, fm), v in edges.most_common():
                f.write(f"{v}\t{ce}\t{cr}\t{fm}\n")
        print(f"\nwrote {len(edges):,} edges to {ef}")
    show = opt("--show")
    if show:
        k = 0
        for i, (ins, role, sites, pe) in cache.items():
            for j, (off, m, o) in enumerate(ins):
                if role[j] == "call-insn:" + show:
                    print(f"\n-- {ivs[i][2][:100]}")
                    for q in range(max(0, j - 20), j + 1):
                        print(f"   {'*' if role[q].startswith('dispatch') else ' '} {ins[q][1]} {ins[q][2]}")
                    k += 1
                    break
            if k >= 4:
                break


if __name__ == "__main__":
    main()
