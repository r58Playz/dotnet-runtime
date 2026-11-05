#!/usr/bin/env python3
"""Per-tick / per-frame hardware counts from a natrun.sh perf.txt taken with PERF_EVENTS (the native half of
hwcensus.py). The server tick is 20 Hz with zero skipped ticks on native (checked in game.log below).

  usage: natcensus.py <natrun-outdir>
"""
import os, re, sys, collections
d = sys.argv[1]
txt = open(os.path.join(d, "perf.txt"), errors="replace").read()
bench = int(open(os.path.join(d, "t2")).read()) - int(open(os.path.join(d, "t1")).read())
rows = collections.defaultdict(dict)
for line in txt.splitlines():
    # natstat.py's note: comm has spaces ("Server thread-1234"), so the name match must be .+?
    m = re.match(r"\s*(.+?)-(\d+)\s+([\d,\.]+)\s+(?:msec\s+)?(\S+)", line)
    if not m:
        continue
    name, tid, val, ev = m.group(1), m.group(2), float(m.group(3).replace(",", "")), m.group(4)
    ev = re.sub(r"^cpu_core/|/u$", "", ev)
    # the last dotted component alone collides (icache_data.stalls / icache_tag.stalls): keep the class prefix
    # for stall-type events, and the old short names for everything else (existing censuses print the same)
    ev = ev if re.search(r"stalls|walk_active|resteer|bound_on|br_misp|machine_clears|baclears", ev) else ev.split(".")[-1]
    rows[name][ev] = val
behind = 0
gl = os.path.join(d, "game.log")
if os.path.exists(gl):
    behind = sum(int(x) for x in re.findall(r"or (\d+) ticks behind", open(gl, errors="replace").read()))
print(f"window {bench}s; 'ticks behind' warnings in the whole game.log: {behind}")
for name, evs in rows.items():
    ins = evs.get("instructions")
    if not ins:
        continue
    denom = bench * 20.0 if name.startswith("Server") else None
    unit = "tick" if denom else "s"
    denom = denom or float(bench)
    print(f"\n{name}: per {unit} (denominator {denom:.0f})")
    print(f"  {'event':16s} {'M/'+unit:>10s} {'per instr':>10s}")
    for ev, v in sorted(evs.items()):
        if ev in ("task-clock", "clock"):
            print(f"  {'task-clock ms':16s} {v/denom:10.3f}")
            continue
        print(f"  {ev:16s} {v/denom/1e6:10.3f} {v/ins:10.4f}")
    if evs.get("near_call"):
        print(f"  instructions per call: {ins/evs['near_call']:.1f}")
