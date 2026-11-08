#!/usr/bin/env python3
"""nbtedit.py <level.dat in> <out> -- an NBT round trip that sets Data/difficulty_settings/difficulty to "peaceful" and
changes nothing else (26.2 layout). Made seed-262p from the 2026-10-21 tar, and seed 262n's save through
runs/2026-11-06-262/mktar.py (lib/seeds.mjs). Moved here from scratchpad/legacy/wj/ on 2026-11-08."""
import gzip, struct, sys
class R:
    def __init__(s, b): s.b, s.p = b, 0
    def take(s, n): v = s.b[s.p:s.p+n]; s.p += n; return v
def rd(r, t):
    if t == 1: return struct.unpack('>b', r.take(1))[0]
    if t == 2: return struct.unpack('>h', r.take(2))[0]
    if t == 3: return struct.unpack('>i', r.take(4))[0]
    if t == 4: return struct.unpack('>q', r.take(8))[0]
    if t == 5: return struct.unpack('>f', r.take(4))[0]
    if t == 6: return struct.unpack('>d', r.take(8))[0]
    if t == 7: n = struct.unpack('>i', r.take(4))[0]; return r.take(n)
    if t == 8: n = struct.unpack('>H', r.take(2))[0]; return r.take(n).decode('utf8')
    if t == 9:
        et = r.take(1)[0]; n = struct.unpack('>i', r.take(4))[0]
        return ('list', et, [rd(r, et) for _ in range(n)])
    if t == 10:
        out = []
        while True:
            tt = r.take(1)[0]
            if tt == 0: return out
            n = struct.unpack('>H', r.take(2))[0]; nm = r.take(n).decode('utf8')
            out.append([tt, nm, rd(r, tt)])
    if t == 11: n = struct.unpack('>i', r.take(4))[0]; return ('ia', list(struct.unpack(f'>{n}i', r.take(4*n))))
    if t == 12: n = struct.unpack('>i', r.take(4))[0]; return ('la', list(struct.unpack(f'>{n}q', r.take(8*n))))
    raise ValueError(t)
def wr(t, v):
    if t == 1: return struct.pack('>b', v)
    if t == 2: return struct.pack('>h', v)
    if t == 3: return struct.pack('>i', v)
    if t == 4: return struct.pack('>q', v)
    if t == 5: return struct.pack('>f', v)
    if t == 6: return struct.pack('>d', v)
    if t == 7: return struct.pack('>i', len(v)) + v
    if t == 8: b = v.encode('utf8'); return struct.pack('>H', len(b)) + b
    if t == 9: _, et, items = v; return bytes([et]) + struct.pack('>i', len(items)) + b''.join(wr(et, x) for x in items)
    if t == 10:
        o = b''
        for tt, nm, vv in v: b = nm.encode('utf8'); o += bytes([tt]) + struct.pack('>H', len(b)) + b + wr(tt, vv)
        return o + b'\0'
    if t == 11: return struct.pack('>i', len(v[1])) + struct.pack(f'>{len(v[1])}i', *v[1])
    if t == 12: return struct.pack('>i', len(v[1])) + struct.pack(f'>{len(v[1])}q', *v[1])
def load(path):
    r = R(gzip.open(path).read()); t = r.take(1)[0]; n = struct.unpack('>H', r.take(2))[0]; nm = r.take(n).decode()
    return t, nm, rd(r, t)
def save(path, root):
    t, nm, v = root; b = nm.encode()
    with gzip.open(path, 'wb') as f: f.write(bytes([t]) + struct.pack('>H', len(b)) + b + wr(t, v))
def find(comp, path):
    for e in comp:
        if e[1] == path[0]: return e if len(path) == 1 else find(e[2], path[1:])
    return None
if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    root = load(src)
    # round-trip check first
    import io
    t, nm, v = root
    e = find(v, ['Data', 'difficulty_settings', 'difficulty'])
    print('difficulty was', e[2]); e[2] = 'peaceful'
    for k in (['Data', 'DayTime'], ['Data', 'Time']):
        x = find(v, k); print(k, x and x[2])
    save(dst, root)
    print('difficulty now', find(load(dst)[2], ['Data', 'difficulty_settings', 'difficulty'])[2])
