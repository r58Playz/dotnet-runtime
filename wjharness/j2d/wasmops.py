#!/usr/bin/env python3
"""j2d/wasmops.py -- decode a wasm function body into (offset, opcode class) so a V8 source position can be named.

  ops = body_ops(module_bytes, start, end, nfimports, imported_globals)   # start/end from wasmmod (start = locals)
  -> (offsets, classes): sorted body-relative offsets of every instruction and its class name

Offsets are relative to the body start INCLUDING the local declarations -- the same base as V8's source positions
(ledger/wasmmod.py, d8x86.py). Covers MVP + sign-ext + sat-trunc + bulk memory + threads + legacy and new EH + tail
calls + typed function refs; a SIMD (0xfd) or GC (0xfb) opcode stops the decode of that function (classes for the
offsets before it stay valid) and is counted.

The CLASSES are what the j2d comparison reads, chosen so ours and TeaVM sort into the same buckets:
  mem.load / mem.store / mem.atomic / mem.bulk      linear-memory traffic
  call.local / call.import / call.indirect / call.ref / call.tail
  global.get.imp / global.get.mod / global.set.imp / global.set.mod
                                                    imported vs module-defined: V8 lowers an imported MUTABLE global
                                                    through two extra instance arrays (wasm-lowering-reducer.h:1044)
  br / br_if / br_table / if / loop / block / return / trap / eh
  i.arith / f.arith / cmp / conv / const / select / local / ref / other
"""
import struct

_LOAD = set(range(0x28, 0x36))
_STORE = set(range(0x36, 0x3f))


def _uleb(b, o):
    r = sh = 0
    while True:
        x = b[o]; o += 1
        r |= (x & 0x7f) << sh; sh += 7
        if not x & 0x80:
            return r, o


def _sleb_skip(b, o):
    while b[o] & 0x80:
        o += 1
    return o + 1


def _blocktype(b, o):
    x = b[o]
    if x == 0x40 or x in (0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x70, 0x6f, 0x69, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x74, 0x73, 0x72, 0x71):
        return o + 1
    if x in (0x63, 0x64):        # (ref null? ht)
        return _sleb_skip(b, o + 1)
    return _sleb_skip(b, o)      # s33 type index


def _valtype(b, o):
    if b[o] in (0x63, 0x64):
        return _sleb_skip(b, o + 1)
    return o + 1


def _memarg(b, o):
    al, o = _uleb(b, o)
    if al & 0x40:
        _, o = _uleb(b, o)       # memidx
    _, o = _uleb(b, o)           # offset
    return o


def body_ops(mb, start, end, nfimports, imported_globals):
    """imported_globals: number of imported globals (global indices below it are imports)."""
    o = start
    n, o = _uleb(mb, o)
    for _ in range(n):
        _, o = _uleb(mb, o)
        o = _valtype(mb, o)
    offs, cls = [], []
    stopped = None
    while o < end:
        at = o - start
        op = mb[o]; o += 1
        c = 'other'
        try:
            if op in _LOAD:
                o = _memarg(mb, o); c = 'mem.load'
            elif op in _STORE:
                o = _memarg(mb, o); c = 'mem.store'
            elif op == 0x20 or op == 0x21 or op == 0x22:
                _, o = _uleb(mb, o); c = 'local'
            elif op == 0x23 or op == 0x24:
                g, o = _uleb(mb, o)
                c = ('global.get' if op == 0x23 else 'global.set') + ('.imp' if g < imported_globals else '.mod')
            elif op == 0x10:
                f, o = _uleb(mb, o); c = 'call.import' if f < nfimports else 'call.local'
            elif op == 0x11:
                _, o = _uleb(mb, o); _, o = _uleb(mb, o); c = 'call.indirect'
            elif op == 0x12:
                _, o = _uleb(mb, o); c = 'call.tail'
            elif op == 0x13:
                _, o = _uleb(mb, o); _, o = _uleb(mb, o); c = 'call.tail'
            elif op in (0x14, 0x15):
                _, o = _uleb(mb, o); c = 'call.ref' if op == 0x14 else 'call.tail'
            elif op == 0x41 or op == 0x42:
                o = _sleb_skip(mb, o); c = 'const'
            elif op == 0x43:
                o += 4; c = 'const'
            elif op == 0x44:
                o += 8; c = 'const'
            elif op in (0x02, 0x03, 0x04, 0x06):
                o = _blocktype(mb, o); c = {0x02: 'block', 0x03: 'loop', 0x04: 'if', 0x06: 'eh'}[op]
            elif op == 0x1f:     # try_table
                o = _blocktype(mb, o)
                k, o = _uleb(mb, o)
                for _ in range(k):
                    kind = mb[o]; o += 1
                    if kind in (0, 1):
                        _, o = _uleb(mb, o)
                    _, o = _uleb(mb, o)
                c = 'eh'
            elif op in (0x0c, 0x0d):
                _, o = _uleb(mb, o); c = 'br' if op == 0x0c else 'br_if'
            elif op == 0x0e:
                k, o = _uleb(mb, o)
                for _ in range(k + 1):
                    _, o = _uleb(mb, o)
                c = 'br_table'
            elif op in (0x07, 0x08, 0x09, 0x18):
                _, o = _uleb(mb, o); c = 'eh'
            elif op in (0x05, 0x0b, 0x01):
                c = 'block'
            elif op in (0x0a, 0x19):
                c = 'eh'
            elif op == 0x0f:
                c = 'return'
            elif op == 0x00:
                c = 'trap'
            elif op == 0x1a:
                c = 'other'
            elif op == 0x1b:
                c = 'select'
            elif op == 0x1c:
                k, o = _uleb(mb, o)
                for _ in range(k):
                    o = _valtype(mb, o)
                c = 'select'
            elif op in (0x25, 0x26):
                _, o = _uleb(mb, o); c = 'ref'
            elif op in (0x3f, 0x40):
                o += 1; c = 'mem.bulk'
            elif 0x45 <= op <= 0x66:
                c = 'cmp'
            elif 0x67 <= op <= 0x8a or 0xc0 <= op <= 0xc4:
                c = 'i.arith'
            elif 0x8b <= op <= 0xa6:
                c = 'f.arith'
            elif 0xa7 <= op <= 0xbf:
                c = 'conv'
            elif op == 0xd0:
                o = _sleb_skip(mb, o); c = 'ref'
            elif op in (0xd1, 0xd3, 0xd4):
                c = 'ref'
            elif op in (0xd2, 0xd5, 0xd6):
                _, o = _uleb(mb, o); c = 'ref'
            elif op == 0xfc:
                sub, o = _uleb(mb, o)
                if sub <= 7:
                    c = 'conv'
                elif sub == 8:
                    _, o = _uleb(mb, o); o += 1; c = 'mem.bulk'
                elif sub == 9:
                    _, o = _uleb(mb, o); c = 'mem.bulk'
                elif sub == 10:
                    o += 2; c = 'mem.bulk'
                elif sub == 11:
                    o += 1; c = 'mem.bulk'
                elif sub in (12, 14):
                    _, o = _uleb(mb, o); _, o = _uleb(mb, o); c = 'ref'
                elif sub in (13, 15, 16, 17):
                    _, o = _uleb(mb, o); c = 'ref'
                else:
                    stopped = f'0xfc {sub}'; break
            elif op == 0xfe:
                sub, o = _uleb(mb, o)
                if sub == 0x03:
                    o += 1
                else:
                    o = _memarg(mb, o)
                c = 'mem.atomic'
            else:
                stopped = f'{op:#x}'; break
        except IndexError:
            stopped = 'eof'; break
        offs.append(at); cls.append(c)
    return offs, cls, stopped


def imported_global_count(mb):
    """number of imported globals (import kind 3)."""
    o = 8; n_g = 0
    while o < len(mb):
        sid = mb[o]; o += 1
        ln, o = _uleb(mb, o)
        if sid == 2:
            p = o
            n, p = _uleb(mb, p)
            for _ in range(n):
                l1, p = _uleb(mb, p); p += l1
                l2, p = _uleb(mb, p); p += l2
                kind = mb[p]; p += 1
                if kind == 0:
                    _, p = _uleb(mb, p)
                elif kind == 1:
                    p += 1; fl, p = _uleb(mb, p); _, p = _uleb(mb, p)
                    if fl & 1: _, p = _uleb(mb, p)
                elif kind == 2:
                    fl, p = _uleb(mb, p); _, p = _uleb(mb, p)
                    if fl & 1: _, p = _uleb(mb, p)
                elif kind == 3:
                    p = _valtype(mb, p); p += 1; n_g += 1
                elif kind == 4:
                    p += 1; _, p = _uleb(mb, p)
            return n_g
        o += ln
    return n_g


if __name__ == '__main__':
    import sys, collections, os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'ledger'))
    import wasmmod
    mb = open(sys.argv[1], 'rb').read()
    m = wasmmod.parse(mb)
    ng = imported_global_count(mb)
    tot = collections.Counter(); bad = collections.Counter(); exact = 0
    for fi, f in m.funcs.items():
        offs, cls, st = body_ops(mb, f['start'], f['end'], m.nfimports, ng)
        tot.update(cls)
        if st:
            bad[st] += 1
        # a complete decode must end exactly at the body end with an `end`
        elif cls and cls[-1] == 'block' and mb[f['end'] - 1] == 0x0b:
            exact += 1
    print(f'{len(m.funcs)} functions, {exact} decoded exactly to their end, stopped {dict(bad)}; imported globals {ng}')
    for k, v in tot.most_common():
        print(f'  {v:8d}  {k}')
