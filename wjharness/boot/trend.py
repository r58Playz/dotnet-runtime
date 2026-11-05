#!/usr/bin/env python3
"""trend.py -- fine boot segments over the archived corpus, median per day (R487). Reads each log only up to FAILED_AUTH.
navigate = the first timestamped line (the replay convention, see replaylogs.mjs), so segment 1 is approximate."""
import os, re, sys, collections, statistics, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootattr
ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
pend0 = [(n, re.compile(r.encode())) for n, r in bootattr.FINE if n in
         ("preinit", "ikvmReady", "javaMain", "mods", "mixinExtras", "userSet", "reload", "atlasDone", "titleReady")]
rows = []
for dp, dn, fn in os.walk(ROOT):
    if "browser-profile" in dp or "profile-" in dp:
        dn[:] = []
        continue
    for f in fn:
        if not f.endswith(".log"):
            continue
        p = os.path.join(dp, f)
        try:
            if os.path.getsize(p) < 200000:
                continue
            pend = list(pend0); m = {}; first = None
            with open(p, "rb") as fh:
                for line in fh:
                    t = bootattr.TS.match(line)
                    if not t:
                        continue
                    s = int(t[1]) * 3600 + int(t[2]) * 60 + int(t[3]) + int(t[4]) / 1e6
                    if first is None:
                        first = s
                    for i, (n, rx) in enumerate(pend):
                        if rx.search(line):
                            m[n] = s; pend.pop(i); break
                    if "titleReady" in m:
                        break
            if "titleReady" not in m or "javaMain" not in m:
                continue
            m["navigate"] = first
            if any(m[k] < first for k in m) or m["titleReady"] - first > 600:
                continue
            rows.append((time.strftime("%Y-%m-%d", time.localtime(os.path.getmtime(p))), m))
        except OSError:
            pass
SEG = [("page+rt", "navigate", "preinit"), ("ikvm", "preinit", "ikvmReady"), ("fabric", "javaMain", "mods"),
       ("mixin", "mods", "mixinExtras"), ("classload", "mixinExtras", "userSet"), ("clinit", "userSet", "reload"),
       ("reload", "reload", "atlasDone"), ("title", "atlasDone", "titleReady"), ("BOOT", "navigate", "titleReady")]
by = collections.defaultdict(list)
for d, m in rows:
    by[d].append(m)
print(f"{len(rows)} logs reached FAILED_AUTH")
print(f"{'day':<11}{'n':>5}" + "".join(f"{s[0]:>10}" for s in SEG))
for d in sorted(by):
    ms = by[d]
    vals = []
    for s in SEG:
        v = [m[s[2]] - m[s[1]] for m in ms if s[1] in m and s[2] in m]
        vals.append(f"{statistics.median(v):10.1f}" if v else f"{'-':>10}")
    print(f"{d:<11}{len(ms):>5}" + "".join(vals))
