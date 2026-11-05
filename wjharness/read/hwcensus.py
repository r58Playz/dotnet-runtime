#!/usr/bin/env python3
"""Hardware-event census of ONE thread per server tick (or client frame): instructions, calls, loads,
stores, branches -- and, from PEBS call samples, WHO IS CALLED and FROM WHERE, execution-weighted.

Record with several events, each at its OWN fixed period (a `period=` term overrides mcperf's -F):

  MCPERF_EVENT='instructions/period=2000003/u,cycles/period=2000003/u,br_inst_retired.near_call/up,br_inst_retired.near_call/upp,br_inst_retired.indirect/u,mem_inst_retired.all_loads/u,mem_inst_retired.all_stores/u,br_inst_retired.cond/u' \
    node mcperf.mjs <out> --warm-ms 180000 --bench-ms 120000 --no-walk

WHY TWO near_call EVENTS. For a PEBS event at precise_ip=1 the recorded IP is the NEXT instruction after
the eventing one -- for a call, the CALLEE's first instruction. At precise_ip>=2 the kernel reports the
EVENTING IP -- the call instruction itself, i.e. the CALLER. So `:up` samples name callees and `:upp`
samples name call sites. (A callee-entry sample's frame-pointer chain skips the immediate caller: rbp is
still the caller's, so [rbp+8] is the caller's own return address. That is why the caller census needs
its own event rather than the callchain.)

TRAPS THIS HANDLES
  * perf 7.2 IGNORES a `period=` term on a named JSON event (br_inst_retired.*, mem_inst_retired.*): the
    alias's own SampleAfterValue wins (near_call: 100007 on cpu_core, 200003 on cpu_atom; loads 1000003).
    So those events are given without a period, and the period used here is always the one in each
    attr in the file. Generic events (instructions, cycles) DO honour it and need it -- without one
    they inherit mcperf's -F and land in frequency mode, which this refuses.
  * Hybrid box: an unprefixed event opens on BOTH cpu_core (type 4) and cpu_atom (type 10) where both
    PMUs have it. mem_inst_retired.* exist on cpu_core ONLY, so a thread that spent time on an E-core has
    loads/stores undercounted. Reported: the atom share of instructions, and loads/stores per P-core
    instruction scaled to the whole thread (same code on both core types, so the ratio transfers).
  * Server tick denominator = span*20 - "ticks behind" inside [mcw] START..END (buckets.py's rule).

  usage: hwcensus.py <outdir> [--tid N | --client] [--top N] [--frames]
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
from perfraw import Symbols, phase_window, PERF_RECORD_SAMPLE, PERF_RECORD_MMAP, PERF_RECORD_MMAP2, PERF_RECORD_COMM  # noqa
from hotinsn import kind_of  # noqa
# buckets.py runs main() at import time, so take only its rule table (everything before census()).
_ns = {"__file__": paths.tool("buckets.py")}
exec(open(paths.tool("buckets.py")).read().split("def census")[0], _ns)
bucket_of = _ns["classify"]

S_IP, S_TID, S_TIME, S_CALLCHAIN, S_PERIOD, S_IDENTIFIER, S_CPU = 1, 2, 4, 32, 256, 1 << 16, 128
HELPER = re.compile(r"(mono_wasm_jit_|wj_|wasm_jit_|get_virtual_method_fast|ldvirtfn|do_jit_call|interp_entry"
                    r"|mono_jiterp_|call_interp)")
PMU = {4: "core", 10: "atom", 0: "hw", 1: "sw"}


def attrs_and_ids(mm):
    attr_size = struct.unpack_from("<Q", mm, 16)[0]
    off, size = struct.unpack_from("<QQ", mm, 24)
    out = []
    for i in range(size // attr_size):
        b = off + i * attr_size
        typ, _sz, config, period, stype, rfmt, flags = struct.unpack_from("<IIQQQQQ", mm, b)
        ioff, isz = struct.unpack_from("<QQ", mm, b + attr_size - 16)
        ids = struct.unpack_from(f"<{isz // 8}Q", mm, ioff) if isz else ()
        freq = bool(flags & (1 << 10))
        precise = (flags >> 15) & 3
        out.append(dict(type=typ, config=config, period=period, stype=stype, freq=freq, precise=precise, ids=ids))
    return out


def evnames(path, n):
    r = subprocess.run(["perf", "evlist", "-i", path], capture_output=True, text=True)
    names = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    if len(names) != n:
        sys.exit(f"perf evlist gave {len(names)} names for {n} attrs -- cannot pair them by order")
    return names


RAW = {0x480: "icache_data_stalls", 0x483: "icache_tag_stalls", 0x1c6: "fe_l1i_miss", 0xc0: "instructions", 0x2c4: "near_call", 0xf9c4: "near_call", 0x80c4: "indirect", 0xebc4: "indirect",
       0x81d0: "all_loads", 0x82d0: "all_stores", 0x11c4: "cond", 0x7ec4: "cond",
       # plan typed-spindle 0a: stall-cycle census (P-core encodings, `perf list --details`)
       0x04a3: "stalls_total", 0x0ca3: "stalls_l1d_miss", 0x05a3: "stalls_l2_miss", 0x06a3: "stalls_l3_miss",
       0x1011: "itlb_walk_active", 0x1012: "dtlb_load_walk_active", 0x00c5: "br_misp", 0x80ad: "clear_resteer_cycles",
       0x21a6: "bound_on_loads",
       # machine clears (s4 group 4): count is cmask=1,edge=1 on umask 1
       0x01c3: "machine_clears_count", 0x02c3: "machine_clears_mo", 0x04c3: "machine_clears_smc", 0x0160: "baclears",
       # j2d/s26: cross-core snoops on L3-hit loads (xsnp_fwd = served from another core's private cache: shared-line
       # traffic) and the load miss levels (P-core encodings, `perf list --details`)
       0x01d2: "l3hit_xsnp_miss", 0x02d2: "l3hit_xsnp_no_fwd", 0x04d2: "l3hit_xsnp_fwd", 0x08d2: "l3hit_xsnp_none",
       0x08d1: "l1_miss_loads", 0x10d1: "l2_miss_loads", 0x20d1: "l3_miss_loads"}
HW = {0: "cycles", 1: "instructions", 4: "branches", 5: "branch-misses"}


def ev_key(x):
    """(pmu, key) from the attr's type/config -- never from perf's printed name, whose format varies with
    how the event was spelled (alias, raw event=/umask=, or generic)."""
    t, c = x["type"], x["config"]
    if t == 0:                                   # PERF_TYPE_HARDWARE, hybrid-extended: pmu type in the top 32 bits
        pmu, ev = PMU.get(c >> 32, "core"), HW.get(c & 0xffffffff, f"hw{c & 0xffffffff}")
    elif t in (4, 10):
        pmu, ev = PMU[t], RAW.get(c & 0xffff, f"raw{c:#x}")
    else:
        pmu, ev = PMU.get(t, str(t)), f"t{t}:{c:#x}"
    return pmu, (f"{ev}:p{x['precise']}" if ev == "near_call" else ev)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__.split("usage:")[1])

    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    outdir = a[0]
    data = os.path.join(outdir, "perf.data")
    phases = json.load(open(os.path.join(outdir, "phases.json")))
    if "--window" in a:   # absolute monotonic seconds "t0,t1" instead of the ingame phase (e.g. a spin, R316)
        t0, t1 = (float(x) for x in opt("--window").split(","))
    else:
        t0, t1 = phase_window(phases, "ingame")
    span = t1 - t0

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
    samp = [x for x in at if x["stype"] & S_CALLCHAIN]
    for x in samp:
        if x["freq"]:
            sys.exit(f"{x['name']} is in FREQUENCY mode -- sample counts are not event counts. Give it a period= term.")
        # S_CPU (perf record --sample-cpu, j2d/s25) sits after TIME, past every fixed offset read below: harmless here
        if x["stype"] & ~(S_IP | S_TID | S_TIME | S_CALLCHAIN | S_IDENTIFIER | S_PERIOD | S_CPU):
            sys.exit(f"{x['name']}: unexpected sample_type {x['stype']:#x}")

    # --- one pass: per (tid, attr) counts in window; keep IPs of call samples for the chosen tid later
    data_off, data_size = struct.unpack_from("<QQ", mm, 40)
    unp = struct.unpack_from
    a0, a1 = int(t0 * 1e9), int(t1 * 1e9)
    cnt = collections.Counter()
    live = collections.Counter()              # (tid, 5 s slice) -> samples, any event: the liveness gate below
    ips = collections.defaultdict(list)       # (tid, attr) -> [ip]   (call events only)
    callattrs = {i for i, x in enumerate(at) if x["short"].startswith("near_call")}
    # --self: also keep the IPs of the instructions event, for a per-symbol SELF instruction census
    selfattrs = {i for i, x in enumerate(at) if x["short"].startswith("instructions")} if "--self" in a else set()
    # --sym EVENT: a per-symbol census of ANY event's sample IPs (e.g. a PEBS frontend_retired.l1i_miss, or an icache
    # stall event) -- the attribution --self gives the instructions event. Matched on the event's short key prefix.
    symkey = opt("--sym") if "--sym" in a else None
    symattrs = {i for i, x in enumerate(at) if symkey and x["short"].startswith(symkey)}
    selfattrs |= symattrs
    comm, mmaps = {}, []
    id_tid = {}                               # per-thread (inherited) event instance id -> tid
    # THROTTLE/UNTHROTTLE, attributed by the TID IN THE RECORD'S sample_id trailer. The id field is the PRIMARY id,
    # which perf shares across threads (one instance per (thread, cpu)), so mapping it to "the last tid that
    # sampled with it" -- what this did first -- charged every thread's throttles to whichever came last and read
    # 8,952 on a server thread whose own records were 1,714 (p34 l1). Lost time is exact: THROTTLE -> UNTHROTTLE.
    throttled = collections.Counter()         # (tid, attr) -> THROTTLE records in window
    thr_open = {}                             # (tid, id) -> THROTTLE time
    thr_lost = collections.Counter()          # (tid, attr) -> ns throttled inside the window
    o, end = data_off, data_off + data_size
    while o < end:
        typ, misc, sz = unp("<IHH", mm, o)
        if sz == 0:
            break
        if typ == PERF_RECORD_SAMPLE:
            ident = unp("<Q", mm, o + 8)[0]
            t = unp("<Q", mm, o + 32)[0]
            if a0 <= t <= a1:
                ai = id2a.get(ident)
                if ai is not None:
                    tid = unp("<I", mm, o + 28)[0]
                    id_tid[ident] = tid
                    cnt[(tid, ai)] += 1
                    live[(tid, int((t - a0) / 5e9))] += 1
                    if ai in callattrs or ai in selfattrs:
                        ips[(tid, ai)].append(unp("<Q", mm, o + 16)[0])
        elif typ in (5, 6):                   # PERF_RECORD_(UN)THROTTLE: u64 time, id, stream_id; then sample_id
            tt, tident = unp("<QQ", mm, o + 8)
            ai = id2a.get(tident)
            if ai is not None and sz >= 40:
                ttid = unp("<I", mm, o + 36)[0]   # sample_id trailer: pid, tid (S_TID is always set here)
                if typ == 5:
                    thr_open[(ttid, tident)] = tt
                    if a0 <= tt <= a1:
                        throttled[(ttid, ai)] += 1
                else:
                    st = thr_open.pop((ttid, tident), None)
                    if st is not None and min(tt, a1) > max(st, a0):
                        thr_lost[(ttid, ai)] += min(tt, a1) - max(st, a0)
        elif typ == PERF_RECORD_MMAP2:
            pid, _ = unp("<II", mm, o + 8)
            addr, length, pgoff = unp("<QQQ", mm, o + 16)
            fo = o + 40 + 24 + 8   # both MMAP2 unions are 24 bytes (see perfraw.py)
            nul = mm.find(b"\0", fo, o + sz)
            mmaps.append((pid, addr, addr + length, pgoff, mm[fo:nul].decode("utf8", "replace")))
        elif typ == PERF_RECORD_MMAP:
            pid, _ = unp("<II", mm, o + 8)
            addr, length, pgoff = unp("<QQQ", mm, o + 16)
            nul = mm.find(b"\0", o + 40, o + sz)
            mmaps.append((pid, addr, addr + length, pgoff, mm[o + 40:nul].decode("utf8", "replace")))
        elif typ == PERF_RECORD_COMM:
            tid = unp("<I", mm, o + 12)[0]
            nul = mm.find(b"\0", o + 16, o + sz)
            comm[tid] = mm[o + 16:nul].decode("utf8", "replace")
        o += sz

    if "--tids" in a:   # instruction samples per thread in the window, busiest first
        per = collections.Counter()
        for (t_, ai), n in cnt.items():
            if at[ai]["short"].startswith("instructions"):
                per[t_] += n * at[ai]["period"]
        for t_, n in per.most_common(8):
            print(f"  tid {t_:8d} {comm.get(t_, '?'):20s} {n/1e9:8.2f} G instructions")
        return
    # --- which thread
    if opt("--tid"):
        tid = int(opt("--tid"))
    else:
        r = subprocess.run([sys.executable, paths.tool("servertid.py"), outdir]
                           + (["--client"] if "--client" in a else []), capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(r.stderr or r.stdout)
        tid = int(r.stdout.strip().splitlines()[-1])
    # LIVENESS GATE. Every per-tick/per-frame figure below divides by a NOMINAL denominator (window x 20 Hz minus logged
    # skips), so a thread that STOPS inside the window reads as doing less work per tick: p67 t2's server thread wedged
    # 75 s into a 121 s window and the census reported 56.4 M instr/tick against 91 -- a 38% "improvement" (R339).
    # Refuse unless --allow-stall; a slice under 20% of the thread's median 5 s slice is a stall.
    nsl = int(span // 5)
    row = [live[(tid, k)] for k in range(nsl)]
    med = sorted(row)[len(row) // 2] if row else 0
    low = [k for k, v in enumerate(row) if med and v < 0.2 * med]
    if low:
        msg = (f"THREAD {tid} STALLED inside the window: {len(low)} of {nsl} 5 s slices under 20% of its median "
               f"({med} samples), first at +{low[0]*5}s. Per-tick numbers would be diluted by the idle time.")
        if "--allow-stall" not in a:
            sys.exit(msg + " Pass --allow-stall to print them anyway, or --window the live part.")
        print("!! " + msg + "\n")
    per_frame = "--client" in a or "--frames" in a
    if per_frame:
        denom = float((phases.get("window") or {}).get("frames") or 0)
        unit = "frame"
    else:
        skipped = 0
        gl = os.path.join(outdir, "game.log")
        if os.path.exists(gl):
            lines = open(gl, errors="replace").read().splitlines()
            i0 = next((i for i, l in enumerate(lines) if "[mcw] START" in l), None)
            i1 = next((i for i, l in enumerate(lines) if "[mcw] END" in l), None)
            if i0 is not None and i1 is not None and i1 > i0:
                skipped = sum(int(m.group(1)) for l in lines[i0:i1] for m in [re.search(r"or (\d+) ticks behind", l)] if m)
        denom, unit = span * 20.0 - skipped, "tick"
        print(f"[window {span:.1f}s, {span*20:.0f} nominal ticks, {skipped} skipped -> {denom:.0f} executed]")
    print(f"tid {tid} ({comm.get(tid, '?')}), per {unit}, denominator {denom:.0f}\n")

    # --- event table: counts per unit, core and atom summed where both exist
    ev = collections.defaultdict(lambda: {"core": 0.0, "atom": 0.0, "n": 0})
    has_atom = {x["short"] for x in at if x["pmu"] == "atom"}
    for i, x in enumerate(at):
        if not (x["stype"] & S_CALLCHAIN):
            continue
        n = cnt[(tid, i)]
        ev[x["short"]][x["pmu"] if x["pmu"] in ("core", "atom") else "core"] += n * x["period"]
        ev[x["short"]]["n"] += n
    ins = ev.get("instructions") or next((v for k, v in ev.items() if k.startswith("instructions")), None)
    ins_all = ins["core"] + ins["atom"]
    atom_share = ins["atom"] / ins_all if ins_all else 0
    print(f"{'event':24s} {'M/'+unit:>10s} {'per instr':>10s} {'samples':>9s}  note")
    for k in sorted(ev):
        v = ev[k]
        tot = v["core"] + v["atom"]
        note = ""
        if k not in has_atom and atom_share > 0.998:
            note = "P-core only and the thread ran on E-cores -- NOT MEASURED"
        elif k not in has_atom and atom_share > 0.002:
            # P-core-only event: scale by the thread's P-core instruction ratio
            tot = v["core"] / (1 - atom_share)
            note = f"P-core only, scaled by 1/(1-{atom_share:.3f})"
        print(f"{k:24s} {tot/denom/1e6:10.3f} {tot/ins_all if ins_all else 0:10.4f} {v['n']:9d}  {note}")
    print(f"\natom share of this thread's instructions: {atom_share*100:.2f}%")
    # Hybrid box: the generic `cycles` sums both PMUs, so a run that spent more time on E-cores reads higher cycles for the
    # same work. Print the split (and per-PMU IPC) so a cycles comparison can be read per core type.
    cyc = ev.get("cycles") or next((v for k, v in ev.items() if k.startswith("cycles")), None)
    if cyc:
        ipc = lambda a, b: a / b if b else 0.0
        print(f"per core type: P-core {ins['core']/denom/1e6:.3f} M instr / {cyc['core']/denom/1e6:.3f} M cycles (IPC {ipc(ins['core'], cyc['core']):.3f})"
              f" | E-core {ins['atom']/denom/1e6:.3f} M instr / {cyc['atom']/denom/1e6:.3f} M cycles (IPC {ipc(ins['atom'], cyc['atom']):.3f})")
    thr, lost = collections.Counter(), collections.Counter()
    for (t_, ai), n in throttled.items():
        if t_ == tid:
            thr[at[ai]["name"]] += n
    for (t_, ai), ns in thr_lost.items():
        if t_ == tid:
            lost[at[ai]["name"]] += ns
    if thr:
        # the thread's events stop sampling while throttled, so a count is low by ~lost/window (exact for a thread
        # that runs the whole window, as the server thread does). p33/p34: 0.21-0.48% on instructions.
        print("throttled on this thread (count low by ~ the lost share):")
        for k, n in thr.most_common():
            print(f"     {n:7d}  lost {lost[k]/1e6:7.1f} ms = {100*lost[k]/(span*1e9):5.2f}%  {k}")
    else:
        print("throttle records on this thread: 0")
    callers_attr = [i for i in callattrs if at[i]["precise"] >= 2]
    callee_attr = [i for i in callattrs if at[i]["precise"] == 1]
    calls = sum(cnt[(tid, i)] * at[i]["period"] for i in callee_attr)
    if calls:
        print(f"instructions per call: {ins_all/calls:.1f}")

    # --- callee census (precise=1: IP = callee entry)
    mapp = [os.path.join(outdir, x) for x in os.listdir(outdir) if x.startswith("perf-") and x.endswith(".map")]
    dumps = sorted((os.path.getsize(os.path.join(outdir, x)), os.path.join(outdir, x))
                   for x in os.listdir(outdir) if x.startswith("jit-") and x.endswith(".dump"))
    syms = Symbols(mapp[0] if mapp else None, mmaps, dumps[-1][1] if dumps else None)
    top = int(opt("--top", "40"))

    def census(attr_list, label, entry, what="calls"):
        agg = collections.Counter()
        kinds = collections.Counter()
        bk = collections.Counter()
        sizes = collections.Counter()
        offs = collections.Counter()
        size_of = {}
        for i in attr_list:
            w = at[i]["period"]
            for ip in ips[(tid, i)]:
                nm = syms.lookup(ip) or b"[ctx]"
                s = nm.decode("utf8", "replace")
                agg[s] += w
                k = kind_of(s)
                if k == "aot" and HELPER.search(s):
                    k = "helper"
                kinds[k] += w
                bk[bucket_of("JS:" + s if not s.startswith(("DSO:", "Builtin:", "[")) else s)] += w
                j = bisect.bisect_right(syms.st, ip) - 1
                if j >= 0 and ip < syms.en[j]:
                    size_of[s] = syms.en[j] - syms.st[j]
                    if entry:
                        offs["entry" if ip == syms.st[j] else "mid"] += w
        tot = sum(agg.values())
        if not tot:
            return
        print(f"\n=== {label}: {tot/denom/1e6:.3f} M {what}/{unit} ===")
        if entry:
            print(f"  sample IP at symbol START: {offs['entry']/tot*100:.1f}% (must be ~100% if IP = callee entry)")
            print("  [unknown] = mostly V8 JUMP-TABLE slots: every module-local `call <funcidx>` and builtin stub call targets")
            print("  an anonymous, unlogged jump-table slot (wasm-code-manager.cc:1379-1388, :299) -- direct calls, not machinery (R316)")
        print("  by kind:  " + "  ".join(f"{k} {v/tot*100:.1f}%" for k, v in kinds.most_common()))
        # V8 tier of the wasm code the event landed in (perf-basic-prof names carry -<idx>-liftoff / -turbofan).
        # Liftoff code on a plateau means a function V8 has not tiered up -- rare, and worth naming (--keep-tier).
        tiers = collections.Counter()
        for s, w in agg.items():
            m = re.search(r"-\d+-(turbofan|liftoff)$", s)
            tiers[m.group(1) if m else "other"] += w
        print("  by V8 tier:  " + "  ".join(f"{k} {v/tot*100:.2f}%" for k, v in tiers.most_common()))
        print("  by bucket:")
        for k, v in bk.most_common(14):
            print(f"    {v/tot*100:6.2f}%  {v/denom/1e6:8.3f} M/{unit}  {k}")
        if entry:
            # size classes of the callee's whole x86 body (static), weighted by calls
            for s, w in agg.items():
                z = size_of.get(s)
                c = ("<=128B" if z <= 128 else "<=320B" if z <= 320 else "<=1KB" if z <= 1024 else
                     "<=4KB" if z <= 4096 else ">4KB") if z else "?"
                sizes[c] += w
            print("  by callee x86 size:  " + "  ".join(f"{c} {sizes[c]/tot*100:.1f}%" for c in
                                                       ("<=128B", "<=320B", "<=1KB", "<=4KB", ">4KB", "?")))
        print(f"  top {top}:")
        for s, w in agg.most_common(top):
            z = size_of.get(s)
            s2 = s if "--keep-tier" in a else re.sub(r"-\d+-(turbofan|liftoff)$", "", s)
            print(f"    {w/tot*100:6.2f}%  {w/denom/1e6:7.4f} M/{unit}  {str(z)+'B' if z else '':>7s}  {s2[:150]}")

    census(callee_attr, "CALLEES (who is called)", True)
    census(callers_attr, "CALL SITES (who calls)", False)
    if selfattrs - symattrs:
        census(sorted(selfattrs - symattrs), "SELF INSTRUCTIONS (where they retire)", False, "instructions")
    if symattrs:
        census(sorted(symattrs), f"{symkey} BY SYMBOL (where the event lands)", False, "events")


if __name__ == "__main__":
    main()
