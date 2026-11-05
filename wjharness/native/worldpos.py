#!/usr/bin/env python3
"""worldpos.py <saves dir> <world name> -- 0-based position of <world name> in 1.16.1's Select World list, which is
sorted by level.dat's Data.LastPlayed, newest first. natrun's menu replay has to know it: with a second save
present ("New World.drift-20260925"), whichever world was played last moves to the top, so a fixed key sequence
alternates between the two worlds run to run (g1/hs2 measured the drift world, 2026-09-30)."""
import gzip, os, struct, sys


def nbt_lastplayed(path):
    b = gzip.open(path).read()
    o = [0]
    def rd(n):
        v = b[o[0]:o[0] + n]; o[0] += n; return v
    def name():
        n = struct.unpack('>H', rd(2))[0]; return rd(n).decode('utf8', 'replace')
    def payload(t, want):
        if t == 1: rd(1)
        elif t == 2: rd(2)
        elif t == 3: rd(4)
        elif t == 4: return struct.unpack('>q', rd(8))[0]
        elif t == 5: rd(4)
        elif t == 6: rd(8)
        elif t == 7: n = struct.unpack('>i', rd(4))[0]; rd(n)
        elif t == 8: name()
        elif t == 9:
            et = rd(1)[0]; n = struct.unpack('>i', rd(4))[0]
            for _ in range(n): payload(et, None)
        elif t == 10:
            while True:
                tt = rd(1)[0]
                if tt == 0: break
                nm = name()
                v = payload(tt, want)
                if nm == 'LastPlayed' and tt == 4: return v
                if isinstance(v, int) and tt == 10: return v
        elif t == 11: n = struct.unpack('>i', rd(4))[0]; rd(4 * n)
        elif t == 12: n = struct.unpack('>i', rd(4))[0]; rd(8 * n)
        return None
    t = rd(1)[0]; name()
    return payload(t, 'LastPlayed')


saves, want = sys.argv[1], sys.argv[2]
rows = []
for w in os.listdir(saves):
    p = os.path.join(saves, w, 'level.dat')
    if os.path.exists(p):
        rows.append((nbt_lastplayed(p) or 0, w))
rows.sort(reverse=True)
names = [w for _, w in rows]
print(names.index(want) if want in names else -1)
print(' | '.join(f'{w}={lp}' for lp, w in rows), file=sys.stderr)
