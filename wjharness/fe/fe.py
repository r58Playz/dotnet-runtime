#!/usr/bin/env python3
"""fe.py <batchdir> <tag>... -- the TMA front-end breakdown (Golden Cove formulas, perf's cpu_core metric expressions) of the
server and client threads, one column per arm. Ours (o*): <tag>/stat.txt (perf stat -x ';', MCPERF_STAT_TOP), server / client
tid from native/servertid.py on the same run. HotSpot (h*): <tag>/perf.txt (natrun, human format), by thread name.

Level 1-2 are shares of issue SLOTS; the level-3 fetch-latency terms are shares of CYCLES (Intel's definitions -- they overlap
and are approximate, so they need not sum to fetch latency). MITE / DSB / LSD use thread cycles for core cycles (with the
sibling hyperthread busy they read low). Per-instruction rates are per 1,000 instructions."""
import os, re, subprocess, sys

d = sys.argv[1]
tags = sys.argv[2:]
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
servertid = paths.tool("servertid.py")
NUM = re.compile(r"^[\d,.]+$")


def ours(tag):
    rows = {}
    for l in open(os.path.join(d, tag, "stat.txt"), errors="replace"):
        f = l.rstrip("\n").split(";")
        if len(f) < 4 or not f[0] or f[0].startswith("#"):
            continue
        m = re.match(r"^(.*)-(\d+)$", f[0].strip())
        if not m or not NUM.match(f[1].strip()):
            continue
        rows.setdefault(m.group(2), {})[f[3].strip()] = float(f[1].replace(",", ""))
    def tid(*a):
        r = subprocess.run([sys.executable, servertid, os.path.join(d, tag), *a], capture_output=True, text=True)
        t = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
        return t if t.isdigit() else None
    s, c = tid(), tid("--client")
    return rows.get(s or "", {}), rows.get(c or "", {}), s, c


def nat(tag):
    rows = {}
    for l in open(os.path.join(d, tag, "perf.txt"), errors="replace"):
        m = re.match(r"^\s*(.+?)-(\d+)\s+([\d,.]+)\s+(\S+)", l)
        if not m:
            continue
        rows.setdefault(m.group(1), {})[m.group(4)] = float(m.group(3).replace(",", ""))
    return rows.get("Server thread", {}), rows.get("Render thread", {}), "Server thread", "Render thread"


def g(e, name):
    for k, v in e.items():
        if k.split("/")[1 if "/" in k else 0].lower() == name.lower():
            return v
    return None


def metrics(e):
    if not e:
        return {}
    G = lambda n: g(e, n)
    sl = sum(G(x) or 0 for x in ("topdown-retiring", "topdown-bad-spec", "topdown-fe-bound", "topdown-be-bound"))
    slots, clk, ins = G("slots"), G("cycles"), G("instructions")
    drop = (G("INT_MISC.UOP_DROPPING") or 0) / slots if slots else 0
    o = {}
    if sl and slots:
        o["L1 frontend bound %"] = 100 * (G("topdown-fe-bound") / sl - drop)
        o["L1 bad speculation %"] = 100 * G("topdown-bad-spec") / sl
        o["L1 backend bound %"] = 100 * G("topdown-be-bound") / sl
        o["L1 retiring %"] = 100 * G("topdown-retiring") / sl
        o["L2  fetch latency %"] = 100 * (G("topdown-fetch-lat") / sl - drop)
        o["L2  fetch bandwidth %"] = o["L1 frontend bound %"] - o["L2  fetch latency %"]
        o["L2  memory bound %"] = 100 * G("topdown-mem-bound") / sl
        o["L2  branch mispredicts %"] = 100 * G("topdown-br-mispredict") / sl
    if clk:
        c = lambda n: 100 * G(n) / clk if G(n) is not None else float("nan")
        unk = c("INT_MISC.UNKNOWN_BRANCH_CYCLES")
        o["L3   icache misses (cyc%)"] = c("ICACHE_DATA.STALLS")
        o["L3   itlb misses (cyc%)"] = c("ICACHE_TAG.STALLS")
        o["L3   branch resteers (cyc%)"] = c("INT_MISC.CLEAR_RESTEER_CYCLES") + (unk if unk == unk else 0)
        bs = G("topdown-bad-spec")
        ratio = (G("topdown-br-mispredict") / bs) if bs else 0
        o["L4    mispredict resteers"] = ratio * c("INT_MISC.CLEAR_RESTEER_CYCLES")
        o["L4    clears resteers"] = (1 - ratio) * c("INT_MISC.CLEAR_RESTEER_CYCLES")
        o["L4    unknown branches (BTB)"] = unk
        ms = G("UOPS_RETIRED.MS,cmask=1,edge")
        rs, ia = G("UOPS_RETIRED.SLOTS"), G("UOPS_ISSUED.ANY")
        if ms is not None and rs and ia:
            o["L3   ms switches (cyc%)"] = 100 * 3 * ms / (rs / ia) / clk
        o["L3   lcp (cyc%)"] = c("DECODE.LCP")
        o["L3   dsb switches (cyc%)"] = c("DSB2MITE_SWITCHES.PENALTY_CYCLES")
        if G("IDQ.MITE_CYCLES_ANY") is not None:
            o["L3   mite bw (cyc%)"] = 100 * (G("IDQ.MITE_CYCLES_ANY") - G("IDQ.MITE_CYCLES_OK")) / clk / 2
            o["L3   dsb bw (cyc%)"] = 100 * (G("IDQ.DSB_CYCLES_ANY") - G("IDQ.DSB_CYCLES_OK")) / clk / 2
        tot = sum(G(n) or 0 for n in ("IDQ.DSB_UOPS", "IDQ.MITE_UOPS", "IDQ.MS_UOPS", "LSD.UOPS"))
        if tot:
            o["DSB coverage %"] = 100 * G("IDQ.DSB_UOPS") / tot
            o["MITE (legacy decode) share %"] = 100 * G("IDQ.MITE_UOPS") / tot
    if ins and clk:
        o["IPC"] = ins / clk
        k = lambda n: 1000 * G(n) / ins if G(n) is not None else float("nan")
        o["L1i miss / k instr"] = k("FRONTEND_RETIRED.L1I_MISS")
        o["iTLB walk cyc / k instr"] = k("ITLB_MISSES.WALK_ACTIVE")
        o["iTLB->STLB hits / k instr"] = k("ITLB_MISSES.STLB_HIT")
        o["BTB miss (baclears) / k instr"] = k("baclears.any")
        o["taken branches / k instr"] = k("BR_INST_RETIRED.NEAR_TAKEN")
        o["branches / k instr"] = k("BR_INST_RETIRED.ALL_BRANCHES")
        o["mispredicts / k instr"] = k("BR_MISP_RETIRED.ALL_BRANCHES")
        o["M instructions (window)"] = ins / 1e6
    return o


cols = {}
for t in tags:
    try:
        s, c, sid, cid = (ours if t.startswith("o") else nat)(t)
    except FileNotFoundError as ex:
        print(f"{t}: {ex}")
        continue
    cols[t] = (metrics(s), metrics(c), sid, cid)
for role, ix in (("SERVER thread", 0), ("CLIENT / render thread", 1)):
    print(f"\n== {role} " + "  ".join(f"{t}={cols[t][2 + ix]}" for t in cols))
    keys = []
    for t in cols:
        for k in cols[t][ix]:
            if k not in keys:
                keys.append(k)
    print(f"{'':34s}" + "".join(f"{t:>10s}" for t in cols))
    for k in keys:
        print(f"{k:34s}" + "".join(f"{cols[t][ix].get(k, float('nan')):10.2f}" for t in cols))
