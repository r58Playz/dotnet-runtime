#!/usr/bin/env python3
"""segs.py <log>... -- fine boot segments per log, plus per-arm medians when logs are named r487-<arm><n>.log (R487)."""
import sys, os, re, collections, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootattr
pend0 = [(n, re.compile(r.encode())) for n, r in bootattr.FINE]
SEG = [("page+rt", "navigate", "preinit"), ("ikvm", "preinit", "ikvmReady"), ("fabric", "javaMain", "mods"),
       ("mixin", "mods", "mixinExtras"), ("classld", "mixinExtras", "userSet"), ("clinit", "userSet", "reload"),
       ("reload", "reload", "atlasDone"), ("title", "atlasDone", "titleReady"), ("BOOT", "navigate", "titleReady")]
arms = collections.defaultdict(list)
print(f"{'log':<22}" + "".join(f"{s[0]:>9}" for s in SEG))
for p in sys.argv[1:]:
    pend = list(pend0); m = {}; first = None
    with open(p, "rb") as fh:
        for line in fh:
            t = bootattr.TS.match(line)
            if not t:
                continue
            s = int(t[1]) * 3600 + int(t[2]) * 60 + int(t[3]) + int(t[4]) / 1e6
            first = first if first is not None else s
            for i, (n, rx) in enumerate(pend):
                if rx.search(line):
                    m[n] = s; pend.pop(i); break
            if "titleReady" in m:
                break
    m["navigate"] = first
    v = [m[b] - m[a] if a in m and b in m else float("nan") for _, a, b in SEG]
    name = os.path.basename(p).replace(".log", "")
    print(f"{name:<22}" + "".join(f"{x:9.1f}" for x in v))
    a = re.match(r"r\d+-([a-z]+)\d+$", name)
    if a:
        arms[a.group(1)].append(v)
if arms:
    print("\nper-arm median [min-max]:")
    for k, rows in sorted(arms.items()):
        print(f"{k + ' n=' + str(len(rows)):<22}" + "".join(f"{statistics.median(c):9.1f}" for c in zip(*rows)))
        print(f"{'':<22}" + "".join(f"{min(c):5.1f}-{max(c):<4.1f}"[:9].rjust(9) for c in zip(*rows)))
