#!/usr/bin/env python3
"""jitindex.py -- compact, time-aware index of a V8 jitdump's JIT_CODE_LOAD records (shared by the ledger tools).

1.55 M records as tuples cost ~1 GB of Python objects, which together with the rest of a ledger pass was enough
for the kernel OOM killer on this 15 GB box (2026-09-30, twice). Here: parallel typed arrays sorted by start
address plus interned names -- ~70 MB.

  idx = JitIndex(path)
  r = idx.at(ip, t)      -> record number or -1 (the latest record covering ip that was loaded at or before t)
  idx.name(r), idx.start[r], idx.end[r], idx.code(r) (bytes, read from the dump on demand)
"""
import array, bisect, struct


class JitIndex:
    def __init__(self, path):
        HDR, FIXED = 16, 40
        st = array.array('Q'); en = array.array('Q'); ts = array.array('Q'); fo = array.array('Q'); sz = array.array('I')
        nid = array.array('I'); names = []; nmap = {}
        with open(path, 'rb') as g:
            hdr = g.read(40)
            pos = struct.unpack('<I', hdr[8:12])[0]
            g.seek(pos)
            while True:
                rh = g.read(HDR)
                if len(rh) < HDR:
                    break
                rid, rsize, t = struct.unpack('<IIQ', rh)
                if rsize < HDR:
                    break
                if rid == 0:
                    body = g.read(rsize - HDR)
                    if len(body) < rsize - HDR:
                        break
                    ca, cs = struct.unpack('<QQ', body[16:32])
                    nul = body.find(b'\0', FIXED)
                    if nul >= 0 and HDR + FIXED + (nul - FIXED) + 1 + cs == rsize:
                        nm = body[FIXED:nul].decode('utf8', 'replace')
                        k = nmap.get(nm)
                        if k is None:
                            k = nmap[nm] = len(names); names.append(nm)
                        st.append(ca); en.append(ca + cs); ts.append(t); fo.append(pos + HDR + FIXED + (nul - FIXED) + 1)
                        sz.append(cs); nid.append(k)
                else:
                    g.seek(rsize - HDR, 1)
                pos += rsize
        order = sorted(range(len(st)), key=lambda i: (st[i], ts[i]))
        self.start = array.array('Q', (st[i] for i in order)); self.end = array.array('Q', (en[i] for i in order))
        self.ts = array.array('Q', (ts[i] for i in order)); self.foff = array.array('Q', (fo[i] for i in order))
        self.size = array.array('I', (sz[i] for i in order)); self.nid = array.array('I', (nid[i] for i in order))
        self.names = names
        self.path = path
        self._f = open(path, 'rb')
        self.n = len(order)

    def at(self, ip, t):
        k = bisect.bisect_right(self.start, ip) - 1
        best = -1
        # records sharing a start are time-ordered; a few earlier starts can still cover ip (a reused region)
        j = k
        while j >= 0 and j > k - 12:
            if self.start[j] <= ip < self.end[j] and self.ts[j] <= t and (best < 0 or self.ts[j] > self.ts[best]):
                best = j
            j -= 1
        return best

    def name(self, r):
        return self.names[self.nid[r]]

    def code(self, r):
        self._f.seek(self.foff[r])
        return self._f.read(self.size[r])

    def key(self, r):
        return (self.start[r], self.ts[r])
