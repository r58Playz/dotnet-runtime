#!/usr/bin/env python3
"""Read a perf.data DIRECTLY, without invoking perf. Symbols, self time, and CALL CHAINS.

WHY THIS EXISTS
---------------
Every consumer here (mcreport.mjs, symclass.py, jitsym.py) shells out to `perf report` or `perf script`.
On the in-game captures that costs minutes per query -- `perf report` on scratchpad/perf-plateau/perf.data
(299 MB, 1.97M map symbols) takes long enough that questions stop being asked. Parsing the file format
directly takes **0.58 s** for the same capture, and 2.8 s more to resolve all 124,784 in-window call chains.
The difference is the difference between "run one report and reason about it" and "ask twenty questions".

AND IT UNLOCKS THE CALL CHAINS, WHICH WERE NEVER USED
-----------------------------------------------------
mcperf records with `-g` (mcperf.mjs:180), so PERF_SAMPLE_CALLCHAIN has been in every capture taken here.
No consumer read it: mcreport/symclass work off `perf report` self-time rows, and mccause.mjs reconstructs
"inclusive cost" from name patterns rather than from stacks. With the real chains, one pass answers questions
that cost several rounds each to guess at -- e.g. `mono_interp_exec_method` occurs a mean of 1.12 times per
stack and 72% of stacks contain exactly one (the thread-entry frame), which settles "is the interpreter in
the hot path" as a measurement instead of an inference.

FORMAT NOTES, i.e. the parts that are easy to get silently wrong
----------------------------------------------------------------
* Header: "PERFILE2", then u64 size, u64 attr_size, then (offset,size) pairs for attrs / data / event_types.
  Records live in [data_off, data_off+data_size) as {u32 type, u16 misc, u16 size}.
* The SAMPLE layout is not fixed -- it is determined by attr.sample_type, in the canonical field order from
  perf_event.h. These captures use 0x10127 = IDENTIFIER|IP|TID|TIME|CALLCHAIN|PERIOD, which lays out as
      id@+8  ip@+16  pid@+24 tid@+28  time@+32  period@+40  nr@+48  chain@+56
  IDENTIFIER being present is what puts an extra u64 BEFORE ip; reading ip at +8 gives garbage that still
  resolves often enough to look plausible, which is the trap. SAMPLE_TYPE is verified against the attrs
  rather than assumed, and an unexpected value is a hard error.
* Callchains carry PERF_CONTEXT_* sentinels (0xffff_ffff_ffff_fe00 and friends) between kernel/user regions.
  They are not addresses; they are dropped.
* MMAP2 records with PERF_RECORD_MISC_MMAP_BUILD_ID (misc & 0x4000) replace maj/min/ino/ino_gen with a
  build-id, which moves the filename. Both shapes are handled; getting it wrong silently mislabels DSOs.

SYMBOLISATION
-------------
Two capture styles, both handled, because mcperf picks between them (mcperf.mjs:137):

  * --perf-basic-prof writes /tmp/perf-<pid>.map, "<hex start> <hex size> <name>", copied next to the
    capture. It is append-only, so the SAME address appears repeatedly as code is re-registered; entries
    are deduped keeping the last write.
  * MCPERF_JITDUMP=1 writes jit-<pid>.dump instead and NO map. Every JIT_CODE_LOAD record already carries
    code_addr, code_size and the name, so the same address->name table is built straight from it -- no
    `perf inject --jit`, whose ELF-writing step is the fragile part and which produced nothing usable here.
    The record layout is validated (header + fixed + name + NUL + code must account for total_size exactly)
    rather than trusted; a mismatch means the offsets are wrong and every number downstream is garbage.

Native frames fall back to the DSO name from the mmap records (name them properly with lib/debuginfo.mjs
first if you care which chromium function it was).

Usage:
  perfraw.py <dir-or-perf.data> [--window T0,T1] [--phase ingame] [--top N] [--tsv out.tsv]
             [--by-thread] [--inclusive] [--callchains] [--tid N | --marker SYM] [--event NAME]
  --event keeps ONE event's samples of a multi-event capture (hwcensus short names: cycles, instructions, ...).
  --marker/--tid restrict every number below to ONE thread; use it for anything about frame rate.
  <dir> is an mcperf outdir: perf.data + phases.json + the perf-<pid>.map copy are found inside it.
  --phase reads the window out of phases.json so the monotonic timestamps never have to be typed by hand.
"""
import array
import bisect
import collections
import json
import mmap
import os
import struct
import sys

PERF_RECORD_MMAP, PERF_RECORD_COMM, PERF_RECORD_SAMPLE, PERF_RECORD_MMAP2 = 1, 3, 9, 10
# sample_type bits, perf_event.h
S_IP, S_TID, S_TIME, S_ADDR, S_READ, S_CALLCHAIN, S_ID = 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6
S_CPU, S_PERIOD, S_STREAM_ID, S_IDENTIFIER = 1 << 7, 1 << 8, 1 << 9, 1 << 16
EXPECTED = S_IDENTIFIER | S_IP | S_TID | S_TIME | S_CALLCHAIN | S_PERIOD   # 0x10127, what mcperf produces


def _attrs(mm):
    """[(sample_type, read_format)] from the file header's attr table."""
    attr_size = struct.unpack_from("<Q", mm, 16)[0]
    off, size = struct.unpack_from("<QQ", mm, 24)
    out = []
    for i in range(size // attr_size):
        base = off + i * attr_size
        out.append((struct.unpack_from("<Q", mm, base + 24)[0], struct.unpack_from("<Q", mm, base + 32)[0]))
    return out


class Capture:
    """One perf.data, parsed once. `samples` is (tid, ip, time, chain-or-None) in file order."""

    def __init__(self, path, want_chains=True, keep_ids=None):
        self.path = path
        self.keep_ids = keep_ids
        f = open(path, "rb")
        self.mm = mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        if mm[:8] != b"PERFILE2":
            sys.exit(f"{path}: not a PERFILE2 perf.data")
        chained = set()
        for st, _ in _attrs(mm):
            # A dummy tracking event (config=9) legitimately lacks CALLCHAIN; only the sampling attr matters.
            # S_CPU (`perf record --sample-cpu`, j2d/s25) sits between TIME and PERIOD and moves the chain by 8 more.
            if st & S_CALLCHAIN and (st & ~S_CPU) not in (EXPECTED, EXPECTED & ~S_PERIOD):
                sys.exit(f"{path}: unexpected sample_type {st:#x}; the fixed field offsets below do not apply")
            if st & S_CALLCHAIN:
                chained.add(st)
        # The offset must come from the SAMPLING attrs, never from whichever attr came last: perf appends a dummy
        # tracking event (0x10107 -- PERIOD, no CALLCHAIN), so a multi-event fixed-period capture (all 0x10027)
        # read chain_off 56 from the dummy, failed the nr bound on every record and returned NO chains at all
        # (p67, 2026-09-27). Mixed PERIOD/no-PERIOD sampling attrs would need a per-id offset; refuse instead.
        if len(chained) > 1:
            sys.exit(f"{path}: sampling attrs disagree on PERIOD ({', '.join(hex(s) for s in chained)}); per-id offsets not implemented")
        st = next(iter(chained), EXPECTED)
        # PERIOD IS OPTIONAL, AND ITS ABSENCE MOVES THE CALLCHAIN. `perf record -c N` (the fixed-period
        # mode MCPERF_PERIOD selects, and the one the client-thread instruction census runs in) records a
        # constant period and omits the field, giving sample_type 0x10027 instead of 0x10127. Everything
        # before PERIOD in perf_event.h's canonical field order -- identifier, ip, tid, time -- keeps its
        # offset; nr and the chain shift back by 8. This used to be a hard exit, which is why the primary
        # metric's own captures could not be read by the fast reader at all.
        self.chain_off = 48 + (8 if st & S_PERIOD else 0) + (8 if st & S_CPU else 0)
        self.data_off, self.data_size = struct.unpack_from("<QQ", mm, 40)
        self.comm, self.mmaps, self.samples = {}, [], []
        self._parse(want_chains)

    def _parse(self, want_chains):
        mm, unp = self.mm, struct.unpack_from
        o, end = self.data_off, self.data_off + self.data_size
        while o < end:
            typ, misc, sz = unp("<IHH", mm, o)
            if sz == 0:
                break
            if typ == PERF_RECORD_SAMPLE:
                if self.keep_ids is not None and unp("<Q", mm, o + 8)[0] not in self.keep_ids:
                    o += sz
                    continue
                ip = unp("<Q", mm, o + 16)[0]
                tid = unp("<I", mm, o + 28)[0]
                t = unp("<Q", mm, o + 32)[0]
                chain = None
                if want_chains:
                    co = self.chain_off
                    nr = unp("<Q", mm, o + co - 8)[0]
                    # Trust the record's own size over nr: a bad nr here would read past the record.
                    if nr < 4096 and co + 8 * nr <= sz:
                        chain = array.array("Q")
                        chain.frombytes(mm[o + co:o + co + 8 * nr])
                self.samples.append((tid, ip, t, chain))
            elif typ == PERF_RECORD_MMAP2:
                pid, tid = unp("<II", mm, o + 8)
                addr, length, pgoff = unp("<QQQ", mm, o + 16)
                # Both unions are 24 bytes: maj,min,ino,ino_gen (4+4+8+8) or build_id_size,res[3],build_id[20]
                # (misc bit 14 = MMAP_BUILD_ID). Then u32 prot, u32 flags. This used 32 for the non-build-id case,
                # which named every //anon mapping with 8 bytes of garbage (e.g. 'DSO:$') -- found 2026-09-25.
                fo = o + 40 + 24 + 8
                nul = mm.find(b"\0", fo, o + sz)
                self.mmaps.append((pid, addr, addr + length, pgoff, mm[fo:nul].decode("utf8", "replace")))
            elif typ == PERF_RECORD_MMAP:
                pid, tid = unp("<II", mm, o + 8)
                addr, length, pgoff = unp("<QQQ", mm, o + 16)
                nul = mm.find(b"\0", o + 40, o + sz)
                self.mmaps.append((pid, addr, addr + length, pgoff, mm[o + 40:nul].decode("utf8", "replace")))
            elif typ == PERF_RECORD_COMM:
                tid = unp("<I", mm, o + 12)[0]
                nul = mm.find(b"\0", o + 16, o + sz)
                self.comm[tid] = mm[o + 16:nul].decode("utf8", "replace")
            o += sz

    def window(self, t0=None, t1=None):
        """Samples inside [t0, t1] SECONDS of the monotonic clock perf recorded with -k 1."""
        if t0 is None:
            return self.samples
        a, b = int(t0 * 1e9), int(t1 * 1e9)
        return [s for s in self.samples if a <= s[2] <= b]


def jitdump_ranges(path):
    """[(start, end, name)] from a V8 jitdump's JIT_CODE_LOAD records."""
    HDR, FIXED = 16, 40
    rows = []
    with open(path, "rb") as g:
        hdr = g.read(40)
        if hdr[:4] not in (b"JiTD", b"DTiJ"):
            return rows
        en = ">" if hdr[:4] == b"JiTD" else "<"
        g.seek(struct.unpack(en + "I", hdr[8:12])[0])
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
                if nul >= 0 and HDR + FIXED + (nul - FIXED) + 1 + cs == rsize:
                    rows.append((ca, ca + cs, body[FIXED:nul]))
    return rows


class Symbols:
    """Address -> name, from a V8 perf map OR jitdump, plus the capture's DSO mmaps."""

    # Context sentinels are >= PERF_CONTEXT_MAX (-4095); real user addresses never reach here.
    CTX_FLOOR = 0xFFFFFFFFFFFFF001

    def __init__(self, mappath, mmaps, jitdump=None):
        rows = []
        if mappath and os.path.exists(mappath):
            with open(mappath, "rb") as f:
                for line in f:
                    try:
                        a, b, rest = line.split(b" ", 2)
                        s, ln = int(a, 16), int(b, 16)
                    except ValueError:
                        continue
                    rows.append((s, s + ln, rest[:-1]))
        elif jitdump:
            rows = jitdump_ranges(jitdump)
        rows.sort(key=lambda r: r[0])
        ded = []
        for r in rows:                       # append-only map: same start re-registered, keep the last
            if ded and ded[-1][0] == r[0]:
                ded[-1] = r
            else:
                ded.append(r)
        self.st = [r[0] for r in ded]
        self.en = [r[1] for r in ded]
        self.nm = [r[2] for r in ded]
        self.dso = sorted((m[1], m[2], m[4]) for m in mmaps if not m[4].startswith("//anon"))
        self.dst = [d[0] for d in self.dso]
        # PERFRAW_DSO_SYMS=1: name native samples too, as DSO:<path>!<function>, so a reader matching on the path
        # keeps working. Chromium ships stripped; its .symtab comes from debuginfod (Arch's server has every
        # build), resolved by build-id. Off by default: the first parse of a 184 MB debug file takes ~30 s.
        self.dsosyms = os.environ.get("PERFRAW_DSO_SYMS") == "1"
        self._pgoff = {(m[1], m[2]): m[3] for m in mmaps}
        self._cache = {}

    def lookup(self, ip):
        """b'<name>' for a JIT symbol, b'DSO:<path>' for native, b'[unknown]', or None for a context marker."""
        r = self._cache.get(ip)
        if r is not None:
            return r
        if ip >= self.CTX_FLOOR:
            self._cache[ip] = None
            return None
        i = bisect.bisect_right(self.st, ip) - 1
        if i >= 0 and ip < self.en[i]:
            r = self.nm[i]
        else:
            j = bisect.bisect_right(self.dst, ip) - 1
            if j >= 0 and ip < self.dso[j][1]:
                r = b"DSO:" + self.dso[j][2].encode()
                if self.dsosyms:
                    st, en, path = self.dso[j]
                    fn = dso_symbol(path, ip - st + self._pgoff.get((st, en), 0))
                    if fn:
                        r += b"!" + fn
            else:
                r = b"[unknown]"
        self._cache[ip] = r
        return r


_DSO_TABLES = {}


def dso_symbol(path, file_offset):
    """Function name at a FILE OFFSET of a native binary, from its .symtab -- its own, or the debug file debuginfod
    holds for its build-id -- else its .dynsym. None when the binary cannot be read or the offset is in no function.
    Tables are parsed once with nm and cached under ~/.cache/perfraw-syms by build-id."""
    t = _DSO_TABLES.get(path)
    if t is None:
        t = _DSO_TABLES[path] = _dso_table(path) or False
    if not t:
        return None
    loads, starts, ends, names = t
    for off, va, sz in loads:
        if off <= file_offset < off + sz:
            vaddr = file_offset - off + va
            break
    else:
        return None
    i = bisect.bisect_right(starts, vaddr) - 1
    if i < 0 or vaddr >= ends[i]:
        return None
    return names[i]


def _dso_table(path):
    import pickle, subprocess
    if not os.path.exists(path):
        return None
    try:
        notes = subprocess.run(["readelf", "-n", path], capture_output=True, text=True).stdout
        bid = next((ln.split()[-1] for ln in notes.splitlines() if "Build ID" in ln), None)
        cache = os.path.expanduser(f"~/.cache/perfraw-syms/{bid}.pkl") if bid else None
        if cache and os.path.exists(cache):
            return pickle.load(open(cache, "rb"))
        loads = []
        for ln in subprocess.run(["readelf", "-lW", path], capture_output=True, text=True).stdout.splitlines():
            f = ln.split()
            if f and f[0] == "LOAD":
                loads.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
        src = path
        if bid:
            dbg = os.path.expanduser(f"~/.cache/debuginfod_client/{bid}/debuginfo")
            if not os.path.exists(dbg):
                subprocess.run(["debuginfod-find", "debuginfo", bid], capture_output=True)
            if os.path.exists(dbg):
                src = dbg
        out = subprocess.run(["nm", "-C", "-S", "--defined-only", src], capture_output=True, text=True).stdout
        if not out.strip():
            out = subprocess.run(["nm", "-C", "-S", "-D", "--defined-only", src], capture_output=True, text=True).stdout
        rows = []
        for ln in out.splitlines():
            f = ln.split(" ", 3)
            if len(f) == 4 and f[2] in "tTwW" and len(f[2]) == 1:
                a, sz = int(f[0], 16), int(f[1], 16)
                if sz:
                    rows.append((a, a + sz, f[3].encode()[:160]))
        rows.sort()
        t = (loads, [r[0] for r in rows], [r[1] for r in rows], [r[2] for r in rows])
        if cache:
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            pickle.dump(t, open(cache, "wb"))
        return t
    except Exception:
        return None


def event_ids(data, want):
    """Sample ids of the attr(s) whose hwcensus short name is `want` (e.g. cycles, instructions), so a
    multi-event capture (hwrun's twelve events) can be read as ONE event's profile. Without this every sample
    of every event counts once, and the 'self %' column is a mixture weighted by each event's sample rate."""
    import hwcensus
    f = open(data, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    keep = set()
    for x in hwcensus.attrs_and_ids(mm):
        if x["stype"] & S_CALLCHAIN and hwcensus.ev_key(x)[1] == want:
            keep.update(x["ids"])
    if not keep:
        sys.exit(f"{data}: no sampling attr named {want!r}")
    return keep


def open_capture(target, want_chains=True, event=None):
    """(Capture, Symbols, phases-json-or-None) from an mcperf outdir or a bare perf.data path."""
    d = target if os.path.isdir(target) else (os.path.dirname(target) or ".")
    data = os.path.join(d, "perf.data") if os.path.isdir(target) else target
    names = os.listdir(d)
    maps = [os.path.join(d, f) for f in names if f.startswith("perf-") and f.endswith(".map")]
    # Several jit-*.dump exist per capture (one per process); the renderer's is by far the largest.
    dumps = sorted((os.path.getsize(os.path.join(d, f)), os.path.join(d, f))
                   for f in names if f.startswith("jit-") and f.endswith(".dump"))
    pj = os.path.join(d, "phases.json")
    phases = json.load(open(pj)) if os.path.exists(pj) else None
    cap = Capture(data, want_chains, event_ids(data, event) if event else None)
    return cap, Symbols(maps[0] if maps else None, cap.mmaps, dumps[-1][1] if dumps else None), phases


def phase_window(phases, name, allow_void=False):
    """Window for a named phase, REFUSING a void in-game window.

    mcperf sets `ingameVoid` when the frame counter never advanced across the whole window -- the
    render thread died, or the runtime livelocked. Such a capture still holds samples, still reports
    `ok: true` with an empty `faults` list, and still yields a plausible-looking table: R269 read
    171 -> 1,064 M instr/tick off one, because its two busy threads were a compile worker spinning in
    the admission DFS and a coop-suspend waiter, not the game. The driver already printed
    "IN-GAME WINDOW IS VOID"; nothing downstream read it. Fail CLOSED here rather than in each reader,
    because every tool funnels through this function.
    """
    if name == "ingame" and (phases or {}).get("ingameVoid") and not allow_void:
        sys.exit("refusing: phases.json says ingameVoid -- the frame counter never advanced across this\n"
                 "window, so the 'ingame' phase is not the game. Any per-tick or per-frame number taken\n"
                 "from it is meaningless. Pass --allow-void if you are deliberately studying the stall.")
    for p in (phases or {}).get("phases", []):
        if p.get("name") == name:
            return float(p["t0"]), float(p["t1"])
    sys.exit(f"no phase {name!r} in phases.json")


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__.strip().split("Usage:")[1])
    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    target = a[0]
    cap, syms, phases = open_capture(target, want_chains=("--inclusive" in a or "--callchains" in a
                                                          or "--marker" in a), event=opt("--event"))
    t0 = t1 = None
    if opt("--phase"):
        t0, t1 = phase_window(phases, opt("--phase"), "--allow-void" in a)
    elif opt("--window"):
        t0, t1 = (float(x) for x in opt("--window").split(","))
    sel = cap.window(t0, t1)
    tot = len(sel)
    if not tot:
        sys.exit("no samples in the window")
    span = (t1 - t0) if t0 is not None else 0
    print(f"{os.path.basename(cap.path)}: {len(cap.samples)} samples total, {tot} in window"
          + (f" [{t0:.3f},{t1:.3f}] = {span:.2f}s" if t0 else ""))

    # RESTRICT TO ONE THREAD. Frame rate is set by the client render thread alone (R184), so a
    # window-wide share over-credits any lever whose work lands in the server tick -- 38.8% of
    # instructions and 0% of frame rate. --marker names that thread the way R184 named it, by a symbol
    # only it runs, because tids differ every run and hard-coding one silently measures nothing after
    # the next capture.
    if opt("--marker"):
        want = opt("--marker").encode()
        hits = collections.Counter()
        for tid, ip, t, chain in sel:
            for x in (chain if chain is not None else (ip,)):
                n = syms.lookup(x)
                if n is not None and want in n:
                    hits[tid] += 1
                    break
        if not hits:
            sys.exit(f"--marker {opt('--marker')}: no thread in the window runs it")
        pick = hits.most_common(1)[0][0]
        print(f"\n--marker {opt('--marker')} -> tid {pick} ({hits[pick]} marked samples; "
              f"{len(hits)} threads matched)")
        sel = [s2 for s2 in sel if s2[0] == pick]
    elif opt("--tid"):
        pick = int(opt("--tid"))
        sel = [s2 for s2 in sel if s2[0] == pick]
    if opt("--marker") or opt("--tid"):
        if not sel:
            sys.exit("no samples for that thread in the window")
        tot = len(sel)
        # Under MCPERF_PERIOD every sample is exactly N retired instructions, so this is a real rate and
        # not a share -- the clock-independent primary metric, comparable across binaries.
        per = int((phases or {}).get("period") or 0)
        extra = (f", {tot * per / 1e9:.1f} G instr, {tot * per / 1e9 / span:.2f} G/s" if per and span else "")
        print(f"thread window: {tot} samples{extra}")

    if "--by-thread" in a:
        # WHICH DENOMINATOR IS VALID DEPENDS ON THE CAPTURE MODE, and mcperf writes `freq` into
        # phases.json even when it recorded with a FIXED PERIOD, so reading `freq` unconditionally is a
        # trap. Under -c N a sample is N retired INSTRUCTIONS, not a tick of wall clock: dividing the
        # count by 499 Hz reported "356.1% of one core" for a single thread and "TOTAL 7.11 cores busy"
        # against the 2.11 R117/R184 measured. A thread cannot exceed 100% of a core; that is the tell.
        #   fixed period  -> samples are instruction-proportional; report the instruction RATE, which is
        #                    the quantity native is compared on anyway (ours 4.51 vs native 2.86 G/s).
        #   frequency (-F)-> samples are time-proportional; % of one core is meaningful.
        per = int((phases or {}).get("period") or 0)
        hz = 0.0 if per else float((phases or {}).get("freq") or 0)
        th = collections.Counter(s[0] for s in sel)
        print("\n=== samples by thread ===")
        for tid, c in th.most_common(20):
            if per and span:
                busy = f"  {c * per / 1e9 / span:5.2f} G instr/s"
            elif hz and span:
                busy = f"  {100 * c / (hz * span):5.1f}% of one core"
            else:
                busy = ""
            print(f"  {100 * c / tot:6.2f}%  {c:8d}  tid={tid:<8} {cap.comm.get(tid, '?'):<18}{busy}")
        if per and span:
            print(f"  TOTAL {tot * per / 1e9 / span:.2f} G instr/s (fixed period {per}; NOT a core count)")
        elif hz and span:
            print(f"  TOTAL {tot / (hz * span):.2f} cores busy")

    counts = collections.Counter()
    if "--inclusive" in a:
        for tid, ip, t, chain in sel:
            seen = set()
            for x in (chain if chain is not None else (ip,)):
                n = syms.lookup(x)
                if n is not None and n not in seen:
                    seen.add(n)
                    counts[n] += 1
        label = "INCLUSIVE (share of stacks touching the symbol)"
    else:
        for tid, ip, t, chain in sel:
            n = syms.lookup(ip)
            if n is not None:
                counts[n] += 1
        label = "SELF"

    if opt("--tsv"):
        with open(opt("--tsv"), "w") as w:
            for n, c in counts.most_common():
                w.write(f"{100 * c / tot:.4f}\t{n.decode('utf8', 'replace')}\n")
        print(f"\nwrote {opt('--tsv')} ({len(counts)} symbols)")
    print(f"\n=== top {opt('--top', '40')} by {label} ===")
    for n, c in counts.most_common(int(opt("--top", "40"))):
        print(f"  {100 * c / tot:6.3f}%  {n.decode('utf8', 'replace')[:130]}")


if __name__ == "__main__":
    main()
