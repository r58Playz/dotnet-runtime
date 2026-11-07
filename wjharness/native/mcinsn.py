#!/usr/bin/env python3
"""mcinsn.py -- WHICH x86 INSTRUCTIONS the server tick executes, ours against native HotSpot, per tick.

  mcinsn.py --native DIR --ours DIR [--event instructions|cycles] [--top N]

mccmp.py says where in the game and in which methods the gap is; this says what the extra instructions ARE. Each
sampled server-thread instruction inside generated code is disassembled and classified with hotinsn.py's classifier
(mnemonic first, then operand form), the same function on both sides, so a class means the same x86 on both.

  --native  an mcnative.sh outdir run with --agent-full: j2dagent.txt then carries every nmethod's CODE BYTES
  --ours    an hwrun.sh/mcperf outdir captured with --jitdump: jit-<pid>.dump carries every wasm function's code

What a class means differs by side in ONE place, which the report names: "heap STORE/LOAD" on our side includes the
GC shadow frame and the wasm stack (`s.p`), which live in linear memory, while HotSpot's frame is %rsp-relative and
reads as SPILL/RELOAD. Everything outside generated code (libjvm, chromium, libc) is one row per side.
"""
import bisect, collections, json, os, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401
import hwcensus  # noqa: E402
import mmap  # noqa: E402
from perfraw import phase_window  # noqa: E402
import hotinsn  # noqa: E402
import mccmp  # noqa: E402

def objdump(code):
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    try:
        os.write(fd, code); os.close(fd)
        r = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "--no-show-raw-insn", tmp], capture_output=True)
    finally:
        os.unlink(tmp)
    out = []
    for line in r.stdout.decode("utf8", "replace").split("\n"):
        m = re.match(r"\s+([0-9a-f]+):\s+(.*)$", line)
        if not m:
            continue
        s = m.group(2).strip()
        if not s or s.startswith("("):
            continue
        parts = s.split(None, 1)
        out.append((int(m.group(1), 16), parts[0], parts[1] if len(parts) > 1 else ""))
    return out


def samples(d, native):
    """(per-sample weight in M/tick, [(ip, t)]) for the server thread inside the window, EV only."""
    phases = json.load(open(os.path.join(d, "phases.json")))
    t0, t1 = phase_window(phases, "ingame")
    tid = mccmp.server_tid(d, native)
    den, _ = mccmp.denom(d, t0, t1)
    data = os.path.join(d, "perf.data")
    f = open(data, "rb"); mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    # BOTH core types: an unpinned arm's server thread can spend a fifth of its time on E-cores (2026-11-06 j1: 23%),
    # and which core ran an instruction does not change what the instruction is. Generic events share one period.
    sel = [x for x in hwcensus.attrs_and_ids(mm) if x["stype"] & 32 and hwcensus.ev_key(x)[1] == EV]
    per = {x["period"] for x in sel}
    if len(per) != 1:
        sys.exit(f"{d}: {EV}: periods {per} across core types; use instructions or cycles")
    cap = mccmp.WinCapture(data, {i for x in sel for i in x["ids"]}, t0, t1, tid)
    return per.pop() / den / 1e6, [(ip, t) for _, ip, t, _ in cap.samples], t0, t1, cap


class Blobs:
    """Code ranges with their bytes: (start, end, name, kind, t_load, t_unload, bytes-getter)."""
    def __init__(self):
        self.rows = []

    def done(self):
        self.rows.sort(key=lambda r: r[0])
        self.st = [r[0] for r in self.rows]
        self.dis = {}

    def at(self, ip, t):
        k = bisect.bisect_right(self.st, ip) - 1
        best = None
        for j in range(k, max(k - 64, -1), -1):
            r = self.rows[j]
            if r[0] <= ip < r[1] and r[4] <= t and (r[5] is None or r[5] > t) and (best is None or r[4] > best[4]):
                best = r
        return best

    def insn(self, r, ip):
        key = (r[0], r[4])
        ins = self.dis.get(key)
        if ins is None:
            ins = self.dis[key] = objdump(r[6]())
        offs = [x[0] for x in ins]
        q = bisect.bisect_right(offs, ip - r[0]) - 1
        return ins[q] if q >= 0 else None


def native_blobs(d, t0, t1):
    path = os.path.join(d, "j2dagent.txt")
    b = Blobs(); names = {}; by_addr = {}; cur = None; nfull = 0
    with open(path, "rb") as f:
        for line in f:
            c = line[:1]
            if c == b"M":
                _, t, ad, sz, mid = line.split()
                cur = [int(ad, 16), int(ad, 16) + int(sz), int(mid), "java", int(t), None, None]
                b.rows.append(cur); by_addr.setdefault(cur[0], []).append(cur)
            elif c == b"C":
                code = bytes.fromhex(line[2:].strip().decode())
                cur[6] = (lambda code=code: code); nfull += 1
            elif c == b"U":
                _, t, ad = line.split(); t = int(t)
                for r in by_addr.get(int(ad, 16), []):
                    if r[5] is None and r[4] <= t:
                        r[5] = t
            elif c == b"N":
                _, i, n = line.rstrip(b"\n").split(b" ", 2); names[int(i)] = n.decode("utf8", "replace")
    if not nfull:
        sys.exit(f"{path}: no code bytes (C records) -- run mcnative.sh with --agent-full")
    a0, a1 = int(t0 * 1e9), int(t1 * 1e9)
    b.rows = [tuple(r[:2]) + (names.get(r[2], "?"), "java", r[4], r[5], r[6]) for r in b.rows
              if r[6] and r[4] <= a1 and (r[5] is None or r[5] >= a0)]
    b.done()
    return b


def ours_blobs(d):
    jds = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f)) for f in os.listdir(d)
                 if f.startswith("jit-") and f.endswith(".dump"))
    if not jds:
        sys.exit(f"{d}: no jit-*.dump -- capture ours with --jitdump (MCPERF_JITDUMP=1)")
    jd = jds[-1][1]
    fh = open(jd, "rb")
    b = Blobs()
    for ca, ce, name, foff, cs in hotinsn.load_jitdump(jd):
        k = hotinsn.kind_of(name)
        def get(foff=foff, cs=cs):
            fh.seek(foff); return fh.read(cs)
        b.rows.append((ca, ce, name, k, 0, None, get))
    b.done()
    return b


def mix(d, native):
    w, ss, t0, t1, cap = samples(d, native)
    b = native_blobs(d, t0, t1) if native else ours_blobs(d)
    out = collections.Counter(); where = collections.Counter()
    for ip, t in ss:
        r = b.at(ip, t if native else 0)
        if r is None:
            where["outside generated code (libjvm / chromium / libc / stubs)"] += w
            continue
        kind = "compiled Java (C1/C2)" if native else {"ours": "our JIT (Java code + IKVM stubs)", "aot": "main module (mono runtime C, our JIT helpers, AOT corlib/IKVM.Runtime)",
                                                         "v8": "V8 builtins / wrappers"}.get(r[3], r[3])
        where[kind] += w
        x = b.insn(r, ip)
        cls = hotinsn.classify(x[1], x[2]) if x else "(undecoded)"
        out[(kind, cls)] += w
    return out, where, len(ss) * w



def main():
    global NAT, OUR, EV, TOP
    a = sys.argv[1:]
    def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
    NAT, OUR = opt("--native"), opt("--ours")
    if not NAT or not OUR:
        sys.exit(__doc__.split("\n\n")[1])
    EV = opt("--event", "instructions")
    TOP = int(opt("--top", "12"))
    nm, nw, ntot = mix(NAT, True)
    om, ow, otot = mix(OUR, False)
    print(f"# Instruction mix of the server tick ({EV}), native {NAT} vs ours {OUR}\n")
    print(f"total: native {ntot:.2f}, ours {otot:.2f} M/tick\n")
    print("| where the sample landed | native M/tick | ours M/tick |\n|---|---|---|")
    for k in sorted(set(nw) | set(ow), key=lambda k: -(ow[k] + nw[k])):
        print(f"| {k} | {nw[k]:.2f} | {ow[k]:.2f} |")
    JIT = "our JIT (Java code + IKVM stubs)"
    print(f"\n## By instruction class: native compiled Java vs our JIT code (M/tick; groups from hotinsn.GROUPS)\n")
    print("| group / class | native | ours | x | gap |\n|---|---|---|---|---|")
    nj = collections.Counter({c: v for (k, c), v in nm.items()})
    oj = collections.Counter({c: v for (k, c), v in om.items() if k == JIT})
    seen = set()
    for g, cls in list(hotinsn.GROUPS.items()) + [("OTHER", sorted((set(nj) | set(oj)) - {c for v in hotinsn.GROUPS.values() for c in v}))]:
        gn, go = sum(nj[c] for c in cls), sum(oj[c] for c in cls)
        print(f"| **{g}** | **{gn:.2f}** | **{go:.2f}** | {go / gn if gn else 0:.2f}x | **{go - gn:+.2f}** |")
        for c in sorted(cls, key=lambda c: -(oj[c] - nj[c]))[:TOP]:
            if nj[c] or oj[c]:
                print(f"| &nbsp;&nbsp;{c} | {nj[c]:.2f} | {oj[c]:.2f} | {oj[c] / nj[c] if nj[c] else 0:.2f}x | {oj[c] - nj[c]:+.2f} |")
    tn, to = sum(nj.values()), sum(oj.values())
    print(f"| **total** | **{tn:.2f}** | **{to:.2f}** | {to / tn if tn else 0:.2f}x | **{to - tn:+.2f}** |")
    print("\nOn our side, heap STORE/LOAD includes the GC shadow frame and the wasm stack (linear memory); HotSpot's frame is "
          "%rsp-relative and lands in SPILL/RELOAD.")


EV = "instructions"
TOP = 12
if __name__ == "__main__":
    main()
