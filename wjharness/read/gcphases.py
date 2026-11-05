#!/usr/bin/env python3
"""gcphases.py <log> -- per-minor sgen phase times from MONO_GC_DEBUG=2 (SGEN_LOG level 2: "Finding pinned pointers",
"Minor scan copy/clear remsets", "Old generation scan" (the card-table remembered set), "Minor scan roots"), each group
closed by its GC_MINOR line; the rest of the pause (gray-stack drain = copying, fragment rebuild) is total minus the
four. Phases split by the same markers as gctime.py. Debug printing itself costs time inside the pause, so read the
SHARES, not the absolute ms."""
import re, sys, statistics
TS = re.compile(r"^\[\d+:\d+:\d{4}/(\d\d)(\d\d)(\d\d)\.(\d+):")
PAT = {"pin": re.compile(r"Finding pinned pointers: \S+ in (\d+) usecs"), "remset_cc": re.compile(r"Minor scan copy/clear remsets: (\d+) usecs"),
       "oldgen": re.compile(r"Old generation scan: (\d+) usecs"), "roots": re.compile(r"Minor scan roots: (\d+) usecs")}
MIN = re.compile(r"\[t[0-9a-f]+\]\[MONO:Mono\] GC_MINOR: \([^)]*\) time ([\d.]+)ms, stw ([\d.]+)ms promoted (\d+)K")
MARKS = [("firstFrame", r"Reloading ResourceManager"), ("title", r"Overlay is removed!|Game took|FAILED_AUTH"), ("join", r"joined the game|StateOutput State: inworld")]
marks, cur, rows = {}, {}, []
seen = set()
for line in open(sys.argv[1], errors="replace"):
    m = TS.match(line)
    if not m: continue
    t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1e6
    for n, rx in MARKS:
        if n not in marks and re.search(rx, line): marks[n] = t
    # SGEN_LOG lines print once; skip the duplicate "[MONO]" copy of anything
    for k, rx in PAT.items():
        g = rx.search(line)
        if g: cur[k] = int(g[1]) / 1000
    g = MIN.search(line)
    if g:
        rows.append((t, float(g[1]), cur)); cur = {}
def ph(t):
    if "firstFrame" in marks and t < marks["firstFrame"]: return "boot->frame"
    if "title" in marks and t < marks["title"]: return "frame->title"
    if "join" in marks and t < marks["join"]: return "title->join"
    return "in game"
for name in ("boot->frame", "frame->title", "title->join", "in game"):
    sel = [r for r in rows if ph(r[0]) == name]
    if not sel: continue
    tot = sum(r[1] for r in sel)
    parts = {k: sum(r[2].get(k, 0) for r in sel) for k in PAT}
    rest = tot - sum(parts.values())
    print(f"{name}: {len(sel)} minors, {tot / 1000:.2f} s, mean {tot / len(sel):.1f} ms;  " +
          "  ".join(f"{k} {100 * v / tot:4.1f}%" for k, v in parts.items()) + f"  rest(copy+fragments) {100 * rest / tot:4.1f}%")
    og = sorted(r[2].get("oldgen", 0) for r in sel)
    print(f"   old-gen (card) scan per minor: p50 {og[len(og) // 2]:.1f} ms, p90 {og[int(len(og) * .9)]:.1f}, max {og[-1]:.1f}")
