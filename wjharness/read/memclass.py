#!/usr/bin/env python3
"""Where do one thread's LOADS and STORES come from? Classifies every PEBS-precise memory sample by the
instruction it lands on (jitdump disassembly), the function kind (ours / aot / helper) and the role
callinfra.py assigns that instruction (V8 prologue, entry band, dispatch, epilogue, near-call spill, body).

Record (cpu_core only -- mem_inst_retired does not exist on cpu_atom):

  MCPERF_EVENT='cpu_core/instructions,period=4000037/upp,cpu_core/event=0xd0,umask=0x82,period=200003/upp,cpu_core/event=0xd0,umask=0x81,period=400009/upp' \
    node mcperf.mjs <out> --jitdump --warm-ms 180000 --bench-ms 120000 --no-walk

MEMORY OPERAND CLASSES (x86 as V8 15.4 emits wasm, checked against the p30/p32 hot paths):
  push/call       implicit stack store: `push %rbp` (V8 frame), the return address of a `call`
  pop/ret         implicit stack load
  stack [rbp/rsp] V8's own frame: spill slots, reloads, frame header
  linear          `disp(%base,%index,1)` -- a wasm linear-memory access (heap object, vtable, shadow frame,
                  IL-state cell, imported-mutable-global cell: s.p is reached this way too)
  instance        one-register `disp(%reg)`: V8 instance / trusted data / table / code-pointer-table access
  frame-base      a linear store whose base or index register provably holds THIS function's shadow-frame
                  base (the value `and $0xfffffff0` produced in the prologue), tracked through V8 spill
                  slots in address order. Pins, zeroing, lazy-frame writes. A LOWER BOUND: control flow is
                  ignored and anything the tracker loses falls back to `linear`.
  imm             a linear store of an immediate (IL-offset / $blk cells, zeroing, constant field inits)

  usage: memclass.py <outdir> [--tid N] [--top N]
"""
import bisect
import collections
import json
import mmap
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
from perfraw import phase_window, PERF_RECORD_SAMPLE  # noqa
from hotinsn import load_jitdump, disasm, kind_of, _dest_operand  # noqa
from callinfra import analyse_function, band_sub, HELPER  # noqa
from hwcensus import attrs_and_ids, ev_key, evnames  # noqa

MEM = re.compile(r"(-?0x[0-9a-f]+)?\((%[a-z0-9]+)?(?:,(%[a-z0-9]+),(\d))?\)")


def operands(ops):
    """Split AT&T operands at paren depth 0."""
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


def reg64(r):
    r = r.lstrip("%")
    m = re.match(r"r(\d+)[dwb]?$", r)
    if m:
        return "r" + m.group(1)
    base = {"eax": "rax", "ax": "rax", "al": "rax", "ebx": "rbx", "bx": "rbx", "bl": "rbx", "ecx": "rcx", "cx": "rcx",
            "cl": "rcx", "edx": "rdx", "dx": "rdx", "dl": "rdx", "esi": "rsi", "si": "rsi", "sil": "rsi", "edi": "rdi",
            "di": "rdi", "dil": "rdi", "ebp": "rbp", "esp": "rsp"}
    return base.get(r, r)


NOACCESS = ("lea", "nop", "prefetch")
NOWRITE_M = ("cmp", "test", "bt", "ucomis", "comis")
PUREMOV = re.compile(r"^(mov|movz|movs|movabs|vmov|cvt|vcvt|movq|movd|movl|movb|movw)")


def mem_operand(m, ops, want_store):
    """The memory operand this instruction STORES to (want_store) or LOADS from, else None."""
    if m.startswith(NOACCESS):
        return None
    ol = operands(ops)
    memops = [o for o in ol if "(" in o]
    if not memops:
        return None
    dst = ol[-1]
    dst_is_mem = "(" in dst
    if want_store:
        return dst if (dst_is_mem and not m.startswith(NOWRITE_M)) else None
    # a load: any memory SOURCE; or a memory destination that is also read (RMW, cmp/test)
    srcs = [o for o in ol[:-1] if "(" in o]
    if srcs:
        return srcs[0]
    if dst_is_mem and not PUREMOV.match(m):
        return dst
    if m.startswith(("call", "jmp")) and dst_is_mem:
        return dst
    return None


def mem_class(m, ops, want_store):
    """Class of the memory access this instruction makes in the requested direction, or None."""
    if want_store:
        if m.startswith("push"):
            return "push"
        if m.startswith("call"):
            return "call (ret addr)"
    else:
        if m.startswith("pop"):
            return "pop"
        if m.startswith("ret"):
            return "ret"
    mo = mem_operand(m, ops, want_store)
    if mo is None:
        return None
    mm = MEM.search(mo)
    if not mm:
        return "other"
    disp, base, index, scale = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
    if base in ("%rbp", "%rsp") and not index:
        return "stack [rbp/rsp]"
    if "%r14" in (base, index):
        return "V8 cage (tagged)"
    if base == "%r13" and not index:
        return "V8 roots/isolate"
    if index and scale == "1":
        return "linear"
    if index:
        return "indexed scale>1"
    if base == "%rip":
        return "rip-relative"
    if disp and int(disp, 16) >= 0x10000:
        return "linear const-addr"
    return "instance/other 1-reg"


class FrameBase:
    """Registers / rbp slots holding the shadow-frame base, walked in address order (approximate)."""

    def __init__(self, ins):
        self.at = {}                     # instruction index -> frozenset of 64-bit regs holding the frame base
        regs, slots = set(), set()
        for q, (_, m, o) in enumerate(ins):
            self.at[q] = frozenset(regs)
            ops = operands(o)
            if m == "and" and o.startswith("$0xfffffff0,"):
                regs = {reg64(ops[-1])}
                slots = set()
                continue
            if not ops:
                continue
            dst = ops[-1]
            if m.startswith("mov") and len(ops) == 2:
                src = ops[0]
                if "(" in dst:                               # store
                    ms = MEM.search(dst)
                    if src.startswith("%") and reg64(src) in regs and ms and ms.group(2) == "%rbp" and not ms.group(3):
                        slots.add(ms.group(1))
                    elif ms and ms.group(2) == "%rbp" and not ms.group(3):
                        slots.discard(ms.group(1))
                    continue
                if dst.startswith("%"):
                    d = reg64(dst)
                    ms = MEM.search(src)
                    if src.startswith("%") and reg64(src) in regs:
                        regs.add(d)
                    elif ms and ms.group(2) == "%rbp" and not ms.group(3) and ms.group(1) in slots:
                        regs.add(d)
                    else:
                        regs.discard(d)
                    continue
            if m.startswith("call"):
                regs = set()                                 # V8 wasm: no callee-saved registers
                continue
            if m in ("add", "sub", "addl", "subl", "addq", "subq") and ops[0].startswith("$") and dst.startswith("%"):
                continue                                     # frame base +- constant is still frame-relative
            if dst.startswith("%") and not m.startswith(("cmp", "test", "push")):
                regs.discard(reg64(dst))

    def holds(self, q, m, ops_str, want_store):
        mo = mem_operand(m, ops_str, want_store)
        if mo is None:
            return False
        mm = MEM.search(mo)
        if not mm:
            return False
        s = self.at.get(q, frozenset())
        return any(r and reg64(r) in s for r in (mm.group(2), mm.group(3)))


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__.split("usage:")[1])

    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    outdir = a[0]
    top = int(opt("--top", "25"))
    data = os.path.join(outdir, "perf.data")
    phases = json.load(open(os.path.join(outdir, "phases.json")))
    t0, t1 = phase_window(phases, "ingame")
    span = t1 - t0
    if opt("--tid"):
        tid = int(opt("--tid"))
    else:
        r = subprocess.run([sys.executable, paths.tool("servertid.py"), outdir],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(r.stderr or r.stdout)
        tid = int(r.stdout.strip().splitlines()[-1])
    ticks = span * 20.0
    gl = os.path.join(outdir, "game.log")
    if os.path.exists(gl):
        lines = open(gl, errors="replace").read().splitlines()
        i0 = next((i for i, l in enumerate(lines) if "[mcw] START" in l), None)
        i1 = next((i for i, l in enumerate(lines) if "[mcw] END" in l), None)
        if i0 is not None and i1 is not None and i1 > i0:
            ticks -= sum(int(m.group(1)) for l in lines[i0:i1] for m in [re.search(r"or (\d+) ticks behind", l)] if m)

    f = open(data, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    at = attrs_and_ids(mm)
    names = evnames(data, len(at))
    id2a = {}
    for i, x in enumerate(at):
        x["name"] = names[i]
        x["pmu"], x["short"] = ev_key(x)
        for j in x["ids"]:
            id2a[j] = i
    want = {i: x["short"] for i, x in enumerate(at) if x["short"] in ("all_stores", "all_loads", "instructions")}
    ips = collections.defaultdict(list)
    data_off, data_size = struct.unpack_from("<QQ", mm, 40)
    unp = struct.unpack_from
    a0, a1 = int(t0 * 1e9), int(t1 * 1e9)
    o, end = data_off, data_off + data_size
    while o < end:
        typ, misc, sz = unp("<IHH", mm, o)
        if sz == 0:
            break
        if typ == PERF_RECORD_SAMPLE:
            t = unp("<Q", mm, o + 32)[0]
            if a0 <= t <= a1 and unp("<I", mm, o + 28)[0] == tid:
                ai = id2a.get(unp("<Q", mm, o + 8)[0])
                if ai in want:
                    ips[ai].append(unp("<Q", mm, o + 16)[0])
        o += sz

    dumps = sorted((os.path.getsize(os.path.join(outdir, x)), os.path.join(outdir, x))
                   for x in os.listdir(outdir) if x.startswith("jit-") and x.endswith(".dump"))
    if not dumps:
        sys.exit("no jitdump -- record with --jitdump")
    ivs = load_jitdump(dumps[-1][1])
    st = [x[0] for x in ivs]
    fh = open(dumps[-1][1], "rb")
    cache = {}

    def fn_of(ip):
        i = bisect.bisect_right(st, ip) - 1
        return i if (i >= 0 and ip < ivs[i][1]) else None

    def analysed(i):
        if i not in cache:
            ins = disasm(fh, ivs[i][3], ivs[i][4])
            role, _, _ = analyse_function(ins) if ins else ([], {}, -1)
            cache[i] = (ins, [x[0] for x in ins], role, FrameBase(ins) if ins else None)
        return cache[i]

    print(f"tid {tid}, window {span:.1f}s, {ticks:.0f} ticks\n")
    for ai in sorted(want, key=lambda k: want[k]):
        if want[ai] == "instructions":
            continue
        is_store = want[ai] == "all_stores"
        period = at[ai]["period"]
        tot = len(ips[ai])
        agg = collections.Counter()      # (kind, role-group, class) -> samples
        by_cls = collections.Counter()
        by_fn = collections.Counter()
        exact = hit = unres = 0
        for ip in ips[ai]:
            i = fn_of(ip)
            if i is None:
                unres += 1
                agg[("unresolved", "", "")] += 1
                continue
            nm = ivs[i][2]
            kind = kind_of(nm)
            if kind == "aot" and HELPER.search(nm):
                kind = "helper"
            ins, offs, role, fb = analysed(i)
            if not ins:
                continue
            off = ip - ivs[i][0]
            q = bisect.bisect_right(offs, off) - 1
            if q < 0:
                continue
            if offs[q] == off:
                exact += 1
            _, m, ops = ins[q]
            cls = mem_class(m, ops, is_store)
            if cls is None:
                # PEBS eventing IP should be the memory instruction itself; tolerate a one-instruction skid
                if q > 0:
                    m2, ops2 = ins[q - 1][1], ins[q - 1][2]
                    c2 = mem_class(m2, ops2, is_store)
                    if c2:
                        q, m, ops, cls = q - 1, m2, ops2, c2 + " (skid-1)"
                if cls is None:
                    cls = f"no {'store' if is_store else 'load'} operand ({m})"
            else:
                hit += 1
            if cls.startswith("linear"):
                if fb and fb.holds(q, m, ops, is_store):
                    cls = "frame-base (pins/zero/frame)"
                elif is_store and ops.startswith("$"):
                    cls = "linear imm"
            r = role[q] if q < len(role) else "body"
            if r.startswith("entry band"):
                r = "entry band: " + band_sub(ins, q)[:1]
            rg = r.split(":")[0] if not r.startswith("entry band") else r
            agg[(kind, rg, cls)] += 1
            by_cls[(kind, cls)] += 1
            by_fn[re.sub(r"-\d+-(turbofan|liftoff)$", "", nm)[:110]] += 1
        what = "STORES" if is_store else "LOADS"
        scale = period / ticks / 1e6
        print(f"=== {what}: {tot} samples x {period} = {tot*scale:.2f} M/tick;"
              f" eventing IP exactly on an instruction {exact/max(1,tot)*100:.1f}%, on a matching memory op {hit/max(1,tot)*100:.1f}%,"
              f" unresolved {unres/max(1,tot)*100:.1f}%")
        print("  by kind x class:")
        for (k, c), n in sorted(by_cls.items(), key=lambda kv: -kv[1])[:24]:
            print(f"    {n/tot*100:6.2f}%  {n*scale:7.3f} M/tick  {k:8s} {c}")
        print("  by kind x role x class (top):")
        for (k, rg, c), n in agg.most_common(top):
            print(f"    {n/tot*100:6.2f}%  {n*scale:7.3f} M/tick  {k:8s} {rg:28s} {c}")
        print("  top functions:")
        for nm, n in by_fn.most_common(12):
            print(f"    {n/tot*100:6.2f}%  {n*scale:7.3f} M/tick  {nm}")
        print()


if __name__ == "__main__":
    main()
