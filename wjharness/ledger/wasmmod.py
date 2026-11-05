#!/usr/bin/env python3
"""wasmmod.py -- minimal wasm module reader for the gap ledger.

parse(bytes) -> Module with:
  nfimports            number of imported functions (function index space starts after them)
  funcs[idx]           {'start','end'} module offsets of each DEFINED function's body (start = first byte of the
                       local declarations, i.e. V8's function-body offset base), keyed by FUNCTION INDEX
  names[idx]           name section function names
  origins[idx]         from the "wj.origin" custom section (MONO_WASM_JIT_ORIGIN): {'locals_len', 'meths' (inlinee
                       names, [0] = None for the function itself), 'tr': [(off, tag, meth, il, ilop)]} with `off`
                       relative to the body start (locals-relative, same base as V8's source positions)
origin_at(idx, off) -> (tag, meth, il, ilop) for a body-relative offset (the last transition at or before it).
"""
import bisect, sys


def uleb(b, o):
    r = sh = 0
    while True:
        x = b[o]; o += 1
        r |= (x & 0x7f) << sh; sh += 7
        if not x & 0x80:
            return r, o


def sleb(b, o):
    r = sh = 0
    while True:
        x = b[o]; o += 1
        r |= (x & 0x7f) << sh; sh += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << sh
            return r, o


def name(b, o):
    n, o = uleb(b, o)
    return b[o:o + n].decode('utf8', 'replace'), o + n


class Module:
    def __init__(self, b):
        self.b = b
        self.nfimports = 0
        self.funcs = {}
        self.names = {}
        self.origins = {}
        self._otr = {}
        if b[:4] != b'\0asm':
            raise ValueError('not wasm')
        o = 8
        customs = []
        while o < len(b):
            sid = b[o]; o += 1
            ln, o = uleb(b, o)
            end = o + ln
            if sid == 2:
                self._imports(o)
            elif sid == 10:
                self._code(o)
            elif sid == 0:
                customs.append((o, end))
            o = end
        for (o, end) in customs:
            nm, p = name(b, o)
            if nm == 'name':
                self._names(p, end)
            elif nm == 'wj.origin':
                self._origin(p, end)

    def _imports(self, o):
        b = self.b
        n, o = uleb(b, o)
        for _ in range(n):
            _, o = name(b, o)
            _, o = name(b, o)
            kind = b[o]; o += 1
            if kind == 0:
                _, o = uleb(b, o); self.nfimports += 1
            elif kind == 1:
                o += 1
                fl, o = uleb(b, o); _, o = uleb(b, o)
                if fl & 1: _, o = uleb(b, o)
            elif kind == 2:
                fl, o = uleb(b, o); _, o = uleb(b, o)
                if fl & 1: _, o = uleb(b, o)
            elif kind == 3:
                o += 1; _, o = uleb(b, o)
            elif kind == 4:
                o += 1; _, o = uleb(b, o)
            else:
                raise ValueError(f'import kind {kind}')

    def _code(self, o):
        b = self.b
        n, o = uleb(b, o)
        for i in range(n):
            sz, o = uleb(b, o)
            self.funcs[self.nfimports + i] = {'start': o, 'end': o + sz}
            o += sz

    def _names(self, o, end):
        b = self.b
        while o < end:
            sub = b[o]; o += 1
            ln, o = uleb(b, o)
            e = o + ln
            if sub == 1:
                n, p = uleb(b, o)
                for _ in range(n):
                    idx, p = uleb(b, p)
                    nm, p = name(b, p)
                    self.names[idx] = nm
            o = e

    def _origin(self, o, end):
        b = self.b
        ver, o = uleb(b, o)
        if ver != 1:
            raise ValueError(f'wj.origin version {ver}')
        nf, o = uleb(b, o)
        for _ in range(nf):
            fi, o = uleb(b, o)
            ll, o = uleb(b, o)
            nm, o = uleb(b, o)
            meths = [None]
            for _k in range(1, nm):
                s, o = name(b, o); meths.append(s)
            n, o = uleb(b, o)
            tr = []
            for _k in range(n):
                off, o = uleb(b, o); tag, o = uleb(b, o); me, o = uleb(b, o)
                il, o = sleb(b, o); op, o = uleb(b, o)
                tr.append((off, tag, me, il, op))
            self.origins[fi] = {'locals_len': ll, 'meths': meths, 'tr': tr}
            self._otr[fi] = [t[0] for t in tr]

    def origin_at(self, fi, off):
        tr = self.origins.get(fi)
        if not tr:
            return None
        k = bisect.bisect_right(self._otr[fi], off) - 1
        if k < 0:
            return None
        return tr['tr'][k][1:]


def parse(b):
    return Module(b)


if __name__ == '__main__':
    m = parse(open(sys.argv[1], 'rb').read())
    print('nfimports', m.nfimports)
    for fi, f in sorted(m.funcs.items()):
        o = m.origins.get(fi)
        print(fi, m.names.get(fi), f, 'origins', len(o['tr']) if o else 0, 'meths', len(o['meths']) if o else 0)
