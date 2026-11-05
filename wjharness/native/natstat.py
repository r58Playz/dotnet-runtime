#!/usr/bin/env python3
"""Native Minecraft: M instructions/frame per thread, the counterpart of our 165.2 M/frame.

Frames come from the MangoHud CSV, which at log_interval=0 logs ONE ROW PER FRAME (verified: 338 rows
over 6.334 s in the 2026-08-25 capture). Rows are sliced to the bench window by
  wall = (log-start epoch parsed from the filename) + elapsed_ns/1e9
exactly as jvmfps.py does, so the two agree by construction.

The client render thread is the fps-setting thread and is compared per FRAME. The server tick thread is
FIXED-RATE at 20 Hz, so per-frame is meaningless for it -- it is reported per TICK instead.
"""
import sys, os, glob, datetime, re

d = sys.argv[1]
t1 = float(open(os.path.join(d, "t1")).read().strip())
t2 = float(open(os.path.join(d, "t2")).read().strip())
win = t2 - t1

frames = 0
for f in glob.glob(os.path.join(d, "*.csv")):
    if f.endswith("_summary.csv"):
        continue
    base = os.path.basename(f)[:-4]
    try:
        stamp = datetime.datetime.strptime(base[-19:], "%Y-%m-%d_%H-%M-%S").timestamp()
    except ValueError:
        continue
    for line in open(f, errors="replace"):
        p = line.split(",")
        if len(p) < 3:
            continue
        try:
            fps, el = float(p[0]), float(p[-1])
        except ValueError:
            continue
        if t1 <= stamp + el / 1e9 <= t2:
            frames += 1

names = {}
tf = os.path.join(d, "threads.txt")
if os.path.exists(tf):
    for line in open(tf):
        a = line.split(None, 1)
        if len(a) == 2:
            names[a[0]] = a[1].strip()

rows = []
for line in open(os.path.join(d, "perf.txt"), errors="replace"):
    # Three separate ways this one regex has failed, all of them silently -- it printed a clean-looking
    # table every time:
    #   1. comm CONTAINS SPACES ("Render thread-1234"), so \S+? matched zero rows. The .+? is load-bearing.
    #   2. the event is spelled cpu_core/instructions/u on this hybrid box (natrun.sh asks for the
    #      cpu_core PMU by name to avoid the atom/core multiplexing of a bare -e instructions), so an
    #      alternation of bare "instructions" matched nothing and every capture read TOTAL 0.00.
    #   3. the task-clock rows carry a UNIT TOKEN ("7,052.67 msec task-clock:u") that the instruction
    #      rows do not, so the event group never reached "task-clock" and the cpu% column was 0.0 for
    #      every thread in every capture ever taken. R240's "1.97 CPU ms/frame" was computed by hand.
    # Gate any change to this line against the stored captures in results/ -- their answers are known.
    m = re.match(r"\s*(.+?)-(\d+)\s+([\d,\.]+)\s+(?:msec\s+)?(cpu_core/instructions/u|instructions|task-clock)", line)
    if m:
        rows.append((m.group(1), int(m.group(2)), float(m.group(3).replace(",", "")), m.group(4)))

ins = {}
clk = {}
for comm, tid, val, ev in rows:
    if ev.endswith("instructions/u") or ev == "instructions":
        ins[(comm, tid)] = val
    else:
        clk[(comm, tid)] = val

ticks = win * 20.0
print(f"window {win:.1f}s   frames {frames}   fps {frames/win:.2f}   ticks(20Hz) {ticks:.0f}")
print(f"{'thread':<28}{'Ginstr':>10}{'cpu%':>8}{'M/frame':>10}{'M/tick':>9}")
tot = 0.0
for (comm, tid), v in sorted(ins.items(), key=lambda kv: -kv[1]):
    if v < 1e8:
        continue
    tot += v
    ms = clk.get((comm, tid), 0.0)
    nm = names.get(str(tid), comm)
    mpf = v / frames / 1e6 if frames else float("nan")
    mpt = v / ticks / 1e6
    print(f"{nm[:27]:<28}{v/1e9:>10.2f}{100*ms/1000/win:>8.1f}{mpf:>10.2f}{mpt:>9.2f}")
print(f"{'TOTAL (>0.1G threads)':<28}{tot/1e9:>10.2f}{'':>8}{tot/frames/1e6 if frames else 0:>10.2f}")
