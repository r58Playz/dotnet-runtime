#!/usr/bin/env python3
"""armcmp.py <batchdir> <tag>... -- one row per hwcensus arm: instr/tick, cycles/tick, cyc/instr, P-core IPC,
L1i miss per 1k instr, icache_data / icache_tag stall share of P-core cycles. A census refused by the liveness gate
(R340) prints its refusal instead of numbers."""
import re, sys, os
d = sys.argv[1]
print(f"{'arm':6s} {'instr':>7s} {'cycles':>7s} {'cyc/ins':>7s} {'P-IPC':>6s} {'L1i/k':>6s} {'icD%':>5s} {'icT%':>5s}  window")
for t in sys.argv[2:]:
    f = os.path.join(d, f"{t}.census.txt")
    if not os.path.exists(f): print(f"{t:6s} (no census)"); continue
    s = open(f).read()
    if "STALLED" in s: print(f"{t:6s} !! {s.strip().splitlines()[-1][:120]}"); continue
    g = lambda k: (lambda m: float(m.group(1)) if m else None)(re.search(rf'^{k}\s+([\d.]+)', s, re.M))
    m = re.search(r'P-core ([\d.]+) M instr / ([\d.]+) M cycles \(IPC ([\d.]+)\)', s)
    w = re.search(r'\[window ([\d.]+)s, (\d+) nominal ticks, (\d+) skipped', s)
    ins, cyc = g('instructions'), g('cycles')
    l1, dd, tt = g('fe_l1i_miss'), g('icache_data_stalls'), g('icache_tag_stalls')
    pc = float(m[2]) if m else None
    fmt = lambda x, f="%.2f": (f % x) if x is not None else "-"
    print(f"{t:6s} {fmt(ins):>7s} {fmt(cyc):>7s} {fmt(cyc/ins if ins and cyc else None, '%.3f'):>7s} {fmt(float(m[3]) if m else None, '%.3f'):>6s} "
          f"{fmt(1000*l1/ins if l1 and ins else None):>6s} {fmt(100*dd/pc if dd and pc else None, '%.1f'):>5s} {fmt(100*tt/pc if tt and pc else None, '%.1f'):>5s}  "
          f"{w[1]+'s '+w[3]+' skip' if w else ''}")
