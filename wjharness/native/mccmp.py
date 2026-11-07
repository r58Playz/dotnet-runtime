#!/usr/bin/env python3
"""mccmp.py -- ours against native HotSpot on the Minecraft SERVER TICK: one event set, one window rule, one reader.

  mccmp.py --native DIR [--native DIR ...] --ours DIR [--ours DIR ...] [--top 40] [--anchor-pct 1.0] [--out FILE]

  --native  mcnative.sh outdirs (perf.data, perf-<pid>.map from jcmd Compiler.perfmap, phases.json, game.log, server.tid)
  --ours    hwrun.sh / mcperf outdirs of the same protocol (Q60): perf.data, perf-<pid>.map, phases.json, game.log
Several runs a side are averaged; every figure is per EXECUTED server tick (window x 20 - logged "ticks behind",
hwcensus's rule), and the spread across runs is printed beside each total.

WHAT IT REPORTS, in the order that answers "what is different":
  1. the hardware table (hwcensus per run): cycles, instructions, IPC, calls, loads, stores, branches;
  2. WHAT KIND of code executes (self, by leaf): Java code against each runtime's machinery;
  3. WHERE IN THE GAME (inclusive, inlining-robust): the tick partitioned by ANCHORS -- Java methods that are real
     frames on BOTH sides at >= --anchor-pct of each side's tick -- each sample charged to the deepest anchor on its
     stack. A partition sums to the total on each side, so its rows add up to the whole gap. Printed as a tree
     (an anchor's parent is the anchor most often above it);
  4. WHICH METHODS: every sample charged to the deepest JAVA frame on its stack ("charged": the method's own code plus
     the runtime code it called -- helpers, stubs, libjvm, mono), ours split into its Java code and its machinery.
     This depends on each JIT's inlining (HotSpot inlines more, so a caller's charged cost holds callees ours calls),
     so read it beside the inclusive column, which does not.

FRAMES ARE NOT SYMMETRIC, and the tables say so where it matters. Native frames are VIRTUAL: j2dagent's pc maps expand
every compiled frame into the methods HotSpot inlined there, so a native inclusive cost is the method's whole cost
however it was compiled. Ours are PHYSICAL: a method mono inlined (small callees; tier 2's guarded inlines) has no
frame, its cost lands in its caller's. So an anchor that reads near parity, or far below its parent's ratio, may be one
mono inlines at its hot sites; read it together with its parent.

NAMES. Ours: `JS:<ret> <pkg.Class>:<method> (<args>)-<n>-<tier>` (V8's perf map). HotSpot: `<ret> <pkg.Class>.<method>(<args>)`
(jcmd Compiler.perfmap). Both join on `pkg.Class.method` (overloads merged, mono .ctor/.cctor = <init>/<clinit>,
IKVM's <default>/instancehelper_ prefixes dropped, lambda classes pooled -- their numbering differs per JVM).
A frame that is IKVM machinery (`__<...>` stubs, IKVM.*, corlib) is never a Java frame.
"""
import collections, json, os, re, subprocess, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401
from perfraw import Capture, Symbols, phase_window  # noqa: E402
import hwcensus  # noqa: E402
import mmap  # noqa: E402

_ns = {"__file__": paths.tool("buckets.py")}
exec(open(paths.tool("buckets.py")).read().split("def census")[0], _ns)
ours_bucket = _ns["classify"]

a = sys.argv[1:]
def opts(k): return [a[i + 1] for i, x in enumerate(a) if x == k]
def opt(k, d=None): return a[a.index(k) + 1] if k in a else d
TOP, APCT = 40, 1.0
# near_call:p2 is the PEBS call event at precise_ip 2: the sample IP is the CALL instruction, so its leaf frame is the
# code that MADE the call (hwcensus.py explains :p1 vs :p2). Few samples (period 400009): read its big rows only.
EVENTS = ("instructions", "cycles", "near_call:p2", "all_stores", "all_loads")
FULL = ("instructions", "cycles")   # the others get sections 2 and 4 only

# ---- names -------------------------------------------------------------------------------------------------------
TIER = re.compile(rb"-\d+-(turbofan|liftoff|builtin)$")
OJAVA = re.compile(r"([A-Za-z_][A-Za-z0-9_.$/]*):(\.c?ctor|[A-Za-z_<][A-Za-z0-9_<>$]*)")
def norm(cls, meth):
    cls = cls.replace("/", "$")
    if "$$Lambda" in cls or "$Lambda$" in cls or re.search(r"\$\$Lambda|/0x", cls):
        return "(lambda adapters)"
    meth = {".ctor": "<init>", ".cctor": "<clinit>"}.get(meth, meth)
    for p in ("<default>", "instancehelper_"):
        if meth.startswith(p): meth = meth[len(p):]
    if cls == "System.Math":
        return "java.lang.Math." + meth[:1].lower() + meth[1:]
    return f"{cls}.{meth}"

_ocache = {}
MODOPT = re.compile(r" ?modopt\([^)]*\)")
JAVA_BUCKETS = ("real Java (app+mods)", "LWJGL bindings", "java.lang.invoke (real MH)", "sun.misc.Unsafe emulation",
                "monitors / locks", "Java type-check machinery")
def java_key(s):
    m = None
    for m in OJAVA.finditer(s.split(" (")[0]):
        pass
    if m and "." in m.group(1) and not m.group(1).startswith(("System.", "IKVM.", "ikvm.", "Mono.")):
        return norm(m.group(1), m.group(2))
    return None
def ours_name(sym):
    """(java key or None, category) for one of our frames. IKVM bakes `modopt(__<Unloadable>N/...)` into signatures
    whose classes were not loaded at link time (R527); it is stripped before anything reads the name."""
    r = _ocache.get(sym)
    if r is None:
        s = MODOPT.sub("", TIER.sub(b"", sym).decode("utf8", "replace"))
        key = None
        if "(wrapper synchronized)" in s:
            key, cat = java_key(s.replace("(wrapper synchronized) ", "")), "Java code (JIT), synchronized wrapper"
        elif "/__<>Anon" in s or "$__<>Anon" in s:
            key, cat = "(lambda adapters)", "Java code (JIT)"     # IKVM's lambda classes; HotSpot's are $$Lambda
        elif "__<overridestub>" in s:
            cat = "IKVM interface/bridge stubs"
        elif s.startswith("JS:(wrapper"):
            cat = "mono wrappers (stelemref, ...)"
        elif s.startswith("JS:interp_entry"):
            cat = "JIT->interpreter entry"
        elif re.match(r"JS:(stackval_|get_arg_offset|do_icall|get_context|interp_|mono_interp)", s):
            cat = "mono interpreter"
        elif re.match(r"JS:(monoeg_|g_)", s):
            cat = "mono runtime C"
        elif "glfw" in s.lower():
            cat = "GLFW (emscripten-glfw)"
        elif re.match(r"JS:(sift|trinkle|qsort|cycle|shl|shr|pntz)$", s):
            cat = "libc / JS glue"
        else:
            cat = ours_bucket(s)
            if s.startswith("JS:") and "__<" not in s and cat in JAVA_BUCKETS:
                key = java_key(s)
            cat = "Java code (JIT)" if key else ("Java-named, unparsed" if cat == "real Java (app+mods)" else cat)
        r = _ocache[sym] = (key, cat)
    return r

NJAVA = re.compile(r"^(?:\S+ )?([A-Za-z_$][\w$.]*(?:/0x[0-9a-f]+)?)\.([\w$<>]+)\(")
_ncache = {}
def native_name(sym):
    r = _ncache.get(sym)
    if r is None:
        s = sym.decode("utf8", "replace")
        key = None
        if s.startswith("DSO:"):
            cat = ("libjvm (runtime, GC, slow paths)" if "libjvm" in s else
                   "libc / libm / vdso clock" if re.search(r"/lib(c|m|pthread)[.-]|\[vdso\]|ld-linux", s) else
                   "other native (LWJGL, ...)")
        elif s == "[unknown]":
            cat = "[unknown]"
        elif s.startswith(("vtable chunks", "itable")) or "vtable stub" in s or "itable stub" in s:
            cat = "JVM dispatch stubs (vtable/itable)"
        elif s == "Interpreter":
            cat = "JVM interpreter"
        else:
            m = NJAVA.match(s)
            if m and "." in m.group(1):
                key = norm(m.group(1), m.group(2)); cat = "Java code (C1/C2)"
            else:
                cat = "JVM stubs, adapters, runtime blobs"
        r = _ncache[sym] = (key, cat)
    return r

# ---- HotSpot's inlining (j2dagent) ----------------------------------------------------------------------------------
class HsInline:
    """j2dagent.txt (,lite): every nmethod's range, load/unload time and pc -> inlining chain. perf's frames are
    PHYSICAL; this expands each compiled frame into the Java methods HotSpot inlined at that pc, innermost first.
    A PcDesc describes the code up to its pc (pc_desc_near), so a pc takes the first PcDesc above it; a RETURN address
    is looked up at ra-1, which lands on the call's own PcDesc (recorded at ra)."""
    def __init__(self, path, t0, t1):
        import array, bisect
        self.bisect = bisect
        names, nms, by_addr, cur = {}, [], {}, None
        with open(path, "rb") as f:
            for line in f:
                c = line[:1]
                if c == b"P":
                    sp = line.index(b" ", 2)
                    cur[5].append(int(line[2:sp], 16)); cur[6].append(line[sp + 1:-1])
                elif c == b"M":
                    _, t, ad, sz, mid = line.split()
                    cur = [int(t), int(ad, 16), int(sz), int(mid), None, array.array("Q"), [], None]
                    nms.append(cur); by_addr.setdefault(cur[1], []).append(cur)
                elif c == b"N":
                    _, i, n = line.rstrip(b"\n").split(b" ", 2); names[int(i)] = n
                elif c == b"U":
                    _, t, ad = line.split(); t = int(t)
                    for r in by_addr.get(int(ad, 16), []):
                        if r[7] is None and r[0] <= t:
                            r[7] = t
                elif c == b"C":
                    cur = cur          # full-mode code bytes: not needed here
        a0, a1 = int(t0 * 1e9), int(t1 * 1e9)
        keep = [r for r in nms if r[0] <= a1 and (r[7] is None or r[7] >= a0)]
        for r in keep:
            if list(r[5]) != sorted(r[5]):
                z = sorted(zip(r[5], r[6])); r[5] = array.array("Q", [x for x, _ in z]); r[6] = [y for _, y in z]
        keep.sort(key=lambda r: r[1])
        self.nm, self.starts, self.names = keep, [r[1] for r in keep], names
        self.keys = {}
        self.n_total, self.n_kept = len(nms), len(keep)

    def key(self, mid):
        k = self.keys.get(mid)
        if k is None:
            n = self.names.get(mid, b"?").decode("utf8", "replace")
            cs, _, rest = n.partition("::")
            cls = cs[1:-1].replace("/", ".") if cs.startswith("L") else cs
            if "+0x" in cls or "/0x" in cls or "$$Lambda" in cls:
                k = "(lambda adapters)"
            else:
                k = norm(cls, rest.split("(")[0])
            self.keys[mid] = k
        return k

    def frames(self, ip, t, leaf):
        """[java key, innermost first] for a compiled pc, or None when ip is in no nmethod alive at t."""
        k = self.bisect.bisect_right(self.starts, ip) - 1
        best = None
        for j in range(k, max(k - 64, -1), -1):
            r = self.nm[j]
            if r[1] <= ip < r[1] + r[2] and r[0] <= t and (r[7] is None or r[7] > t) and (best is None or r[0] > best[0]):
                best = r
        if best is None:
            return None
        pc = ip if leaf else ip - 1
        i = self.bisect.bisect_right(best[5], pc)
        if i >= len(best[5]):
            return [self.key(best[3])]                # past the last PcDesc: stubs at the nmethod's end
        return [self.key(int(x.split(b":")[0])) for x in best[6][i].split(b",")]

# ---- one run ------------------------------------------------------------------------------------------------------
class WinCapture(Capture):
    """perfraw's Capture, keeping only ONE thread's samples inside [t0, t1]: ours' captures span the whole run (boot
    included, every thread), and holding every chain of a 1.7 GB capture costs gigabytes."""
    def __init__(self, path, keep_ids, t0, t1, tid):
        self._flt = (int(t0 * 1e9), int(t1 * 1e9), tid)
        super().__init__(path, True, keep_ids)

    def _parse(self, want_chains):
        import struct, array
        from perfraw import PERF_RECORD_SAMPLE, PERF_RECORD_MMAP, PERF_RECORD_MMAP2
        mm, unp = self.mm, struct.unpack_from
        a0, a1, want_tid = self._flt
        o, end = self.data_off, self.data_off + self.data_size
        while o < end:
            typ, misc, sz = unp("<IHH", mm, o)
            if sz == 0:
                break
            if typ == PERF_RECORD_SAMPLE:
                if unp("<I", mm, o + 28)[0] == want_tid and unp("<Q", mm, o + 8)[0] in self.keep_ids:
                    t = unp("<Q", mm, o + 32)[0]
                    if a0 <= t <= a1:
                        ip = unp("<Q", mm, o + 16)[0]
                        co = self.chain_off
                        nr = unp("<Q", mm, o + co - 8)[0]
                        chain = None
                        if nr < 4096 and co + 8 * nr <= sz:
                            chain = array.array("Q"); chain.frombytes(mm[o + co:o + co + 8 * nr])
                        self.samples.append((want_tid, ip, t, chain))
            elif typ in (PERF_RECORD_MMAP2, PERF_RECORD_MMAP):
                pid, tid = unp("<II", mm, o + 8)
                addr, length, pgoff = unp("<QQQ", mm, o + 16)
                fo = o + 40 + 24 + 8 if typ == PERF_RECORD_MMAP2 else o + 40
                nul = mm.find(b"\0", fo, o + sz)
                self.mmaps.append((pid, addr, addr + length, pgoff, mm[fo:nul].decode("utf8", "replace")))
            o += sz

def server_tid(d, native):
    if native:
        return int(open(os.path.join(d, "server.tid")).read())
    r = subprocess.run([sys.executable, paths.tool("servertid.py"), d], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{d}: servertid: {r.stderr or r.stdout}")
    return int(r.stdout.strip().splitlines()[-1])

def denom(d, t0, t1):
    skipped = 0
    gl = os.path.join(d, "game.log")
    if os.path.exists(gl):
        lines = open(gl, errors="replace").read().splitlines()
        i0 = next((i for i, l in enumerate(lines) if "[mcw] START" in l), None)
        i1 = next((i for i, l in enumerate(lines) if "[mcw] END" in l), None)
        if i0 is not None and i1 is not None and i1 > i0:
            skipped = sum(int(m.group(1)) for l in lines[i0:i1] for m in [re.search(r"or (\d+) ticks behind", l)] if m)
    return (t1 - t0) * 20.0 - skipped, skipped

def hwtable(d, tid):
    r = subprocess.run([sys.executable, paths.tool("hwcensus.py"), d, "--tid", str(tid), "--top", "1"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{d}: hwcensus: {(r.stderr or r.stdout)[-600:]}")
    rows = {}
    for l in r.stdout.splitlines():
        m = re.match(r"(\S+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s*$", l)
        if m:
            rows[m.group(1)] = float(m.group(2))
    return rows

def x_freq(sel): return any(x["freq"] for x in sel)

def load(d, native):
    phases = json.load(open(os.path.join(d, "phases.json")))
    t0, t1 = phase_window(phases, "ingame")
    tid = server_tid(d, native)
    den, skipped = denom(d, t0, t1)
    data = os.path.join(d, "perf.data")
    names = os.listdir(d)
    mp = [os.path.join(d, f) for f in names if f.startswith("perf-") and f.endswith(".map")]
    if native and not mp:
        sys.exit(f"{d}: no perf-<pid>.map (jcmd Compiler.perfmap) -- Java frames would be unnamed")
    jd = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f)) for f in names if f.startswith("jit-") and f.endswith(".dump"))
    f = open(data, "rb"); mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    attrs = hwcensus.attrs_and_ids(mm)
    run = dict(dir=d, native=native, tid=tid, ticks=den, skipped=skipped, span=t1 - t0, hw=hwtable(d, tid), ev={})
    syms = None
    namer = native_name if native else ours_name
    hs = None
    ag = os.path.join(d, "j2dagent.txt")
    if native:
        if not os.path.exists(ag):
            sys.exit(f"{d}: no j2dagent.txt -- without HotSpot's inlining map a native frame hides every method inlined "
                     "into it, and inclusive costs are not comparable (re-run mcnative.sh)")
        hs = HsInline(ag, t0, t1)
    run["inl"] = (hs.n_kept, hs.n_total) if hs else None
    for ev in EVENTS:
        # The big-core attr only: on a hybrid part each event opens on cpu_core AND cpu_atom, at different periods for
        # the raw call events, and the memory events exist on cpu_core alone.
        if ev == EVENTS[0]:
            # How much of the thread ran on E-cores. Every event below is read off the P-core attrs (the raw call and
            # memory events have no single period across core types, or no E-core counterpart), so an arm that was
            # not pinned and spent time on E-cores is UNDER-counted by that share (2026-11-06 j1: 23%).
            both = [x for x in attrs if x["stype"] & 32 and hwcensus.ev_key(x)[1] == ev]
            pid = {i: hwcensus.ev_key(x)[0] for x in both for i in x["ids"]}
            capb = WinCapture(data, set(pid), t0, t1, tid)
            run["atom_share"] = 0.0
            if capb.samples:
                # WinCapture drops the id, so count the P-core ids alone in a second pass
                n_core = len(WinCapture(data, {i for i, k in pid.items() if k == "core"}, t0, t1, tid).samples)
                run["atom_share"] = 1.0 - n_core / len(capb.samples)
        sel = [x for x in attrs if x["stype"] & 32 and hwcensus.ev_key(x) == ("core", ev)]
        ids = {i for x in sel for i in x["ids"]}
        per = {x["period"] for x in sel}
        if len(per) != 1 or x_freq(sel):
            sys.exit(f"{d}: {ev}: core attrs with periods {per}; expected one fixed period")
        w = per.pop() / den / 1e6                     # M events per tick, per sample
        cap = WinCapture(data, ids, t0, t1, tid)
        if syms is None:
            syms = Symbols(mp[0] if mp else None, cap.mmaps, jd[-1][1] if jd else None)
        rows = []
        for tid_, ip, t, chain in cap.samples:
            frames = []
            leaf = True
            for x in (chain if chain is not None else (ip,)):
                if x >= Symbols.CTX_FLOOR:
                    continue
                fr = hs.frames(x, t, leaf) if hs else None
                if fr:
                    frames.extend((k, "Java code (C1/C2)") for k in fr)
                else:
                    nm = syms.lookup(x)
                    if nm is not None:
                        frames.append(namer(nm))
                leaf = False
            if not frames:
                frames = [namer(syms.lookup(ip) or b"[unknown]")]
            rows.append(frames)
        run["ev"][ev] = (w, rows)
    return run

# ---- aggregation --------------------------------------------------------------------------------------------------
def agg(run, ev):
    w, rows = run["ev"][ev]
    tot = len(rows) * w
    cat = collections.Counter(); incl = collections.Counter(); charged = collections.Counter()
    charged_mach = collections.Counter(); selfj = collections.Counter()
    for fr in rows:
        leafkey, leafcat = fr[0]
        cat[leafcat] += w
        seen = set()
        for k, _ in fr:
            if k and k not in seen:
                seen.add(k); incl[k] += w
        deepest = next((k for k, _ in fr if k), None)
        if deepest:
            charged[deepest] += w
            if leafkey is None:
                charged_mach[deepest] += w
            else:
                selfj[leafkey] += w
        else:
            charged["(no Java frame on the stack)"] += w
    return dict(tot=tot, cat=cat, incl=incl, charged=charged, mach=charged_mach, selfj=selfj, rows=rows, w=w)

def mean_counter(cs):
    out = collections.Counter()
    for c in cs:
        for k, v in c.items():
            out[k] += v / len(cs)
    return out

def side(runs, ev):
    ags = [agg(r, ev) for r in runs]
    return dict(tots=[g["tot"] for g in ags], tot=sum(g["tot"] for g in ags) / len(ags),
                **{k: mean_counter([g[k] for g in ags]) for k in ("cat", "incl", "charged", "mach", "selfj")}, ags=ags)

def partition(S, anchors):
    """Each sample charged to its deepest anchor; also the most common anchor above each anchor."""
    part = collections.Counter(); parent = collections.defaultdict(collections.Counter)
    for g in S["ags"]:
        w = g["w"] / len(S["ags"])
        for fr in g["rows"]:
            chain = []
            for k, _ in fr:
                if k in anchors and (not chain or chain[-1] != k):
                    chain.append(k)
            part[chain[0] if chain else "(above every anchor)"] += w
            for i in range(len(chain) - 1):
                if chain[i + 1] != chain[i]:
                    parent[chain[i]][chain[i + 1]] += 1
                    break
    return part, parent

# ---- report -------------------------------------------------------------------------------------------------------
out = []
def p(s=""): out.append(s)
def rng(xs): return f"{min(xs):.2f}-{max(xs):.2f}" if len(xs) > 1 else "n=1"
def ratio(o, n): return f"{o / n:6.2f}x" if n > 1e-9 else "   n/a "

def section3(N, O, ev, gap):
    # "(lambda adapters)" pools every lambda on each side, so it is no one place in the game: never an anchor.
    anchors = {k for k in set(N["incl"]) & set(O["incl"]) if not k.startswith("(")
               and N["incl"][k] >= APCT / 100 * N["tot"] and O["incl"][k] >= APCT / 100 * O["tot"]}
    NP, npar = partition(N, anchors); OP, opar = partition(O, anchors)
    par = {}
    for k in anchors:
        c = npar[k] + opar[k]
        par[k] = c.most_common(1)[0][0] if c else None
    kids = collections.defaultdict(list)
    for k in anchors:
        kids[par[k]].append(k)
    p(f"### 3. Where in the game ({ev}): the tick partitioned by {len(anchors)} anchors (frames on both sides at "
      f">= {APCT}% inclusive)\n")
    p("Each row: the anchor's inclusive cost, then its EXCLUSIVE part (samples whose deepest anchor it is). The exclusive "
      "parts and `(above every anchor)` sum to each side's total, so the `gap` column sums to the whole gap. Native "
      "frames include HotSpot's inlined methods; ours do not include mono's (see the header).\n")
    p("| anchor | native incl | ours incl | x | native excl | ours excl | x | gap M/tick | gap share |")
    p("|---|---|---|---|---|---|---|---|---|")
    def emit(k, depth):
        p(f"| {'&nbsp;&nbsp;' * depth}{k} | {N['incl'][k]:.2f} | {O['incl'][k]:.2f} | {ratio(O['incl'][k], N['incl'][k])} | "
          f"{NP[k]:.2f} | {OP[k]:.2f} | {ratio(OP[k], NP[k])} | {OP[k] - NP[k]:+.2f} | {100 * (OP[k] - NP[k]) / gap:5.1f}% |")
        for c in sorted(kids[k], key=lambda c: -O["incl"][c]):
            emit(c, depth + 1)
    for k in sorted(kids[None], key=lambda c: -O["incl"][c]):
        emit(k, 0)
    k = "(above every anchor)"
    p(f"| {k} | | | | {NP[k]:.2f} | {OP[k]:.2f} | {ratio(OP[k], NP[k])} | {OP[k] - NP[k]:+.2f} | {100 * (OP[k] - NP[k]) / gap:5.1f}% |")
    p()


def section5(N, O, ev, gap, top=30):
    """Out-of-line methods: inclusive cost beyond the tick's own ratio R. excess = ours incl - R x native incl."""
    R = O["tot"] / N["tot"]
    rows, absent = [], []
    for k in set(N["incl"]) | set(O["incl"]):
        if k.startswith("("):
            continue
        ex = O["incl"][k] - R * N["incl"][k]
        if O["incl"][k] >= 0.1 and ex > 0:
            (rows if N["incl"][k] > 0 else absent).append((ex, k))
    rows.sort(reverse=True); absent.sort(reverse=True)
    p(f"### 5. Out of line ({ev}): methods whose inclusive cost exceeds the tick's own {R:.2f}x\n")
    p(f"`excess` = ours incl - {R:.2f} x native incl: what the method costs beyond a uniform slowdown. Excess nests (a "
      "caller inherits its callee's), so read down a chain to the deepest row: that is the cause. Everything not "
      "listed here is the uniform part of the gap.\n")
    p("| method | native incl | ours incl | x | excess M/tick | excess / gap |")
    p("|---|---|---|---|---|---|")
    for ex, k in rows[:top]:
        p(f"| {k} | {N['incl'][k]:.3f} | {O['incl'][k]:.3f} | {ratio(O['incl'][k], N['incl'][k])} | {ex:+.3f} | {100 * ex / gap:5.1f}% |")
    p()
    if absent:
        p("Frames ours has and native never does (no native sample has them, inlined or not). Mostly IKVM's forwarding "
          "stubs for interface default methods -- `ServerWorld.method_8590` calling `WorldView.method_8590`, where "
          "HotSpot's vtable points at the default method itself -- whose inclusive cost is their callee's (listed "
          "under the callee's own name), and intrinsics HotSpot expands inline (`Object.hashCode`), whose cost is real. "
          "Their `ours incl` is NOT excess:\n")
        p("| method | ours incl | ours charged (own code + runtime it called) |")
        p("|---|---|---|")
        for ex, k in absent[:12]:
            p(f"| {k} | {O['incl'][k]:.3f} | {O['charged'][k]:.3f} |")
        p()


def section4(N, O, ev, gap, TOP):
    p(f"### 4. Which methods ({ev}): cost charged to the deepest Java frame, top {TOP} by gap\n")
    p("`ours mach` = the part of ours' charged cost whose leaf is not Java code (helpers, stubs, IKVM, mono, V8). "
      "Native's deepest Java frame is the innermost INLINED method at the pc; ours' is the physical method, holding "
      "whatever mono inlined into it, so a small method HotSpot inlines can read 0 on our side and its caller high.\n")
    p("| method | native charged | ours charged | x | ours mach | native incl | ours incl | x | gap M/tick |")
    p("|---|---|---|---|---|---|---|---|---|")
    allk = set(N["charged"]) | set(O["charged"])
    for k in sorted(allk, key=lambda k: -(O["charged"][k] - N["charged"][k]))[:TOP]:
        p(f"| {k} | {N['charged'][k]:.3f} | {O['charged'][k]:.3f} | {ratio(O['charged'][k], N['charged'][k])} | "
          f"{O['mach'][k]:.3f} | {N['incl'][k]:.3f} | {O['incl'][k]:.3f} | {ratio(O['incl'][k], N['incl'][k])} | "
          f"{O['charged'][k] - N['charged'][k]:+.3f} |")
    tg = sum(max(0.0, O["charged"][k] - N["charged"][k]) for k in allk)
    p(f"\nTop {TOP} rows hold {100 * sum(O['charged'][k] - N['charged'][k] for k in sorted(allk, key=lambda k: -(O['charged'][k] - N['charged'][k]))[:TOP]) / gap:.1f}% "
      f"of the gap; {sum(1 for k in allk if O['charged'][k] - N['charged'][k] > 0.01)} methods carry more than 0.01 M/tick of it.\n")


def main():
  global TOP, APCT
  NATIVE, OURS = opts("--native"), opts("--ours")
  if not NATIVE or not OURS:
      sys.exit(__doc__.split("\n\n")[1])
  TOP = int(opt("--top", "40")); APCT = float(opt("--anchor-pct", "1.0"))
  nat = [load(d, True) for d in NATIVE]
  our = [load(d, False) for d in OURS]
  p(f"# ours vs native HotSpot, Minecraft server tick ({len(our)} ours run(s), {len(nat)} native run(s))\n")
  for r in nat + our:
      rj = {}
      if r["native"] and os.path.exists(os.path.join(r["dir"], "run.json")):
          rj = json.load(open(os.path.join(r["dir"], "run.json")))
      p(f"* {'native' if r['native'] else 'ours  '} {r['dir']}: tid {r['tid']}, window {r['span']:.1f} s, "
        f"{r['ticks']:.0f} executed ticks ({r['skipped']} skipped)" + (f", {rj.get('java_version', '')}" if rj else "")
        + (f", {r['inl'][0]} of {r['inl'][1]} nmethods alive in the window (j2dagent)" if r.get("inl") else "")
      + f", E-core share {100 * r['atom_share']:.1f}%"
      + (" -- WARNING: every figure for this run is P-core only and low by about that share" if r["atom_share"] > 0.02 else ""))
  p()
  p("## 1. Hardware events per server tick (hwcensus, mean of runs)\n")
  p("| event | native M/tick | ours M/tick | ours / native |")
  p("|---|---|---|---|")
  keys = [k for k in ("cycles", "instructions", "near_call:p1", "indirect", "cond", "all_loads", "all_stores")
          if all(k in r["hw"] for r in nat + our)]
  for k in keys:
      n = sum(r["hw"][k] for r in nat) / len(nat); o = sum(r["hw"][k] for r in our) / len(our)
      p(f"| {k} | {n:.3f} ({rng([r['hw'][k] for r in nat])}) | {o:.3f} ({rng([r['hw'][k] for r in our])}) | {ratio(o, n)} |")
  if "cycles" in keys and "instructions" in keys:
      ipc = lambda rs: sum(r["hw"]["instructions"] / r["hw"]["cycles"] for r in rs) / len(rs)
      p(f"| IPC | {ipc(nat):.2f} | {ipc(our):.2f} | |")
  p("\n(near_call:p1 is PEBS-sampled calls; `indirect` is indirect branches incl. calls; see hwcensus.py.)\n")

  for ev in EVENTS:
      N, O = side(nat, ev), side(our, ev)
      ns = sum(len(g["rows"]) for g in N["ags"]); os_ = sum(len(g["rows"]) for g in O["ags"])
      label = {"near_call:p2": "CALLS MADE (near_call:p2, M calls/tick)", "all_stores": "STORES (mem_inst_retired.all_stores, M/tick)",
               "all_loads": "LOADS (mem_inst_retired.all_loads, M/tick)"}.get(ev, ev.upper() + " (M/tick)")
      p(f"## {label}: native {N['tot']:.3f} ({rng(N['tots'])}) vs ours {O['tot']:.3f} ({rng(O['tots'])}) "
        f"= {ratio(O['tot'], N['tot']).strip()}; gap {O['tot'] - N['tot']:.3f}; samples native {ns}, ours {os_}\n")
      p(f"### 2. What kind of code ({ev}, self = leaf frame)\n")
      p("| native | M/tick | share | | ours | M/tick | share |")
      p("|---|---|---|---|---|---|---|")
      nc, oc = N["cat"].most_common(), O["cat"].most_common()
      for i in range(max(len(nc), len(oc))):
          a_ = f"{nc[i][0]} | {nc[i][1]:.2f} | {100 * nc[i][1] / N['tot']:.1f}%" if i < len(nc) else " | | "
          b_ = f"{oc[i][0]} | {oc[i][1]:.2f} | {100 * oc[i][1] / O['tot']:.1f}%" if i < len(oc) else " | | "
          p(f"| {a_} | | {b_} |")
      p()
      gap = O["tot"] - N["tot"]
      if ev in FULL:
          section3(N, O, ev, gap)
      top = TOP if ev in FULL else min(TOP, 25)
      section4(N, O, ev, gap, top)
      section5(N, O, ev, gap)


  txt = "\n".join(out)
  if opt("--out"):
      open(opt("--out"), "w").write(txt + "\n")
  print(txt)


if __name__ == "__main__":
    main()
