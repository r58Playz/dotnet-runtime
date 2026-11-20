#!/usr/bin/env python3
"""wasmrw.py -- a small wasm module rewriter for the TeaVM arms (2026-11-19): decode function bodies with their
immediates, inline chosen callees into chosen callers, move a memory-resident word into a wasm global, re-encode.

Used by split-teavm.py to put TeaVM's code in OUR call layout:
  inline(mod, callees, callers)    one level: at each `call f` (f in callees) in a caller, pop the arguments into fresh
                                   locals, zero the callee's own locals, and splice the callee's body inside a block of
                                   its result type (its `return` becomes a `br` to that block). Calls inside an inlined
                                   body stay calls. Locals are allocated once per (caller, callee).
  word_to_global(mod, addr)        `i32.const addr; i32.load` -> `global.get G`, and `i32.const addr; <value>;
                                   i32.store` -> `<value>; global.set G` where <value> is straight-line stack code,
                                   G a new mutable i32 global (module-defined here; wasm-split makes split modules import
                                   it, which is what our `s.p` is). Every other use of the constant is counted and
                                   refused, so the caller can stop.
Decoding covers what TeaVM's linear-memory backend emits (MVP, sign-ext, sat-trunc, bulk memory, legacy and new EH,
reference types) and refuses anything else rather than mis-decode."""
import struct


def uleb(b, o):
    r = s = 0
    while True:
        x = b[o]; o += 1; r |= (x & 0x7f) << s; s += 7
        if x < 0x80:
            return r, o


def sleb(b, o):
    r = s = 0
    while True:
        x = b[o]; o += 1; r |= (x & 0x7f) << s; s += 7
        if x < 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, o


def enc_u(n):
    out = bytearray()
    while True:
        b = n & 0x7f; n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b); return bytes(out)


def enc_s(n):
    out = bytearray()
    while True:
        b = n & 0x7f; n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b); return bytes(out)
        out.append(b | 0x80)


VALTYPES = {0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x70, 0x6f}
ZERO = {0x7f: b'\x41\x00', 0x7e: b'\x42\x00', 0x7d: b'\x43' + b'\x00' * 4, 0x7c: b'\x44' + b'\x00' * 8}


class Module:
    def __init__(self, data):
        assert data[:4] == b'\0asm'
        self.head = data[:8]
        self.sections = []           # [id, bytes]
        o = 8
        while o < len(data):
            sid = data[o]; size, j = uleb(data, o + 1)
            self.sections.append([sid, data[j:j + size]]); o = j + size
        self.types = []              # [(params bytes, results bytes)]
        self.func_type = []          # type index of every function, imports first
        self.nfimports = 0
        self.nglobals = 0
        self.nimported_globals = 0
        self.names = {}
        for sid, b in self.sections:
            if sid == 1:
                n, o = uleb(b, 0)
                for _ in range(n):
                    assert b[o] == 0x60; o += 1
                    np, o = uleb(b, o); ps = b[o:o + np]; o += np
                    nr, o = uleb(b, o); rs = b[o:o + nr]; o += nr
                    assert all(t in VALTYPES for t in ps + rs), 'non-numeric value type'
                    self.types.append((ps, rs))
            elif sid == 2:
                n, o = uleb(b, 0)
                for _ in range(n):
                    ln, o = uleb(b, o); o += ln
                    ln, o = uleb(b, o); o += ln
                    kind = b[o]; o += 1
                    if kind == 0:
                        t, o = uleb(b, o); self.func_type.append(t); self.nfimports += 1
                    elif kind == 1:
                        o += 1; fl, o = uleb(b, o); _, o = uleb(b, o)
                        if fl & 1: _, o = uleb(b, o)
                    elif kind == 2:
                        fl, o = uleb(b, o); _, o = uleb(b, o)
                        if fl & 1: _, o = uleb(b, o)
                    elif kind == 3:
                        o += 2; self.nimported_globals += 1
                    elif kind == 4:
                        o += 1; _, o = uleb(b, o)
            elif sid == 3:
                n, o = uleb(b, 0)
                for _ in range(n):
                    t, o = uleb(b, o); self.func_type.append(t)
            elif sid == 6:
                self.nglobals, _ = uleb(b, 0)
            elif sid == 0:
                ln, o = uleb(b, 0)
                if b[o:o + ln] == b'name':
                    o += ln
                    while o < len(b):
                        sub = b[o]; o += 1; ssz, o = uleb(b, o); end = o + ssz
                        if sub == 1:
                            n, p = uleb(b, o)
                            for _ in range(n):
                                i, p = uleb(b, p); ln2, p = uleb(b, p); self.names[i] = b[p:p + ln2].decode('utf8', 'replace'); p += ln2
                        o = end
        code = [b for sid, b in self.sections if sid == 10][0]
        n, o = uleb(code, 0)
        self.bodies = []
        for _ in range(n):
            sz, o = uleb(code, o); self.bodies.append(code[o:o + sz]); o += sz

    def section(self, sid):
        for s in self.sections:
            if s[0] == sid:
                return s
        return None

    def encode(self):
        code = bytearray(enc_u(len(self.bodies)))
        for b in self.bodies:
            code += enc_u(len(b)) + b
        out = bytearray(self.head)
        for s in self.sections:
            body = bytes(code) if s[0] == 10 else s[1]
            out += bytes([s[0]]) + enc_u(len(body)) + body
        return bytes(out)


def decode(body):
    """-> (local groups [(count, valtype)], instructions [(op, imm, raw)]) where imm is: local/global/func index, i32
    const value, (align, offset) for loads / stores, or None."""
    o = 0
    ng, o = uleb(body, o)
    groups = []
    for _ in range(ng):
        c, o = uleb(body, o); t = body[o]; o += 1
        assert t in VALTYPES, f'local type {t:#x}'
        groups.append((c, t))
    ins = []
    n = len(body)
    while o < n:
        s = o; op = body[o]; o += 1; imm = None
        if 0x28 <= op <= 0x3e:
            al, o = uleb(body, o)
            if al & 0x40: _, o = uleb(body, o)
            off, o = uleb(body, o); imm = (al, off)
        elif op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x10, 0x0c, 0x0d, 0x07, 0x08, 0x09, 0x18, 0x25, 0x26, 0xd2):
            imm, o = uleb(body, o)
        elif op == 0x11:
            _, o = uleb(body, o); _, o = uleb(body, o)
        elif op == 0x41:
            imm, o = sleb(body, o)
        elif op == 0x42:
            _, o = sleb(body, o)
        elif op == 0x43:
            o += 4
        elif op == 0x44:
            o += 8
        elif op in (0x02, 0x03, 0x04, 0x06):
            bt = body[o]
            if bt == 0x40 or bt in VALTYPES:
                o += 1
            else:
                _, o = sleb(body, o)
        elif op == 0x1f:
            bt = body[o]
            if bt == 0x40 or bt in VALTYPES: o += 1
            else: _, o = sleb(body, o)
            k, o = uleb(body, o)
            for _ in range(k):
                kind = body[o]; o += 1
                if kind in (0, 1): _, o = uleb(body, o)
                _, o = uleb(body, o)
        elif op == 0x0e:
            k, o = uleb(body, o)
            for _ in range(k + 1): _, o = uleb(body, o)
        elif op in (0x00, 0x01, 0x05, 0x0b, 0x0f, 0x19, 0x0a, 0x1a, 0x1b, 0xd1) or 0x45 <= op <= 0xc4:
            pass
        elif op == 0x1c:
            k, o = uleb(body, o)
            o += k
        elif op in (0x3f, 0x40):
            o += 1
        elif op == 0xd0:
            o += 1
        elif op == 0xfc:
            sub, o = uleb(body, o)
            if sub <= 7: pass
            elif sub == 8: _, o = uleb(body, o); o += 1
            elif sub == 9: _, o = uleb(body, o)
            elif sub == 10: o += 2
            elif sub == 11: o += 1
            elif sub in (12, 14): _, o = uleb(body, o); _, o = uleb(body, o)
            elif sub in (13, 15, 16, 17): _, o = uleb(body, o)
            else: raise ValueError(f'0xfc {sub}')
            op = (0xfc, sub)
        else:
            raise ValueError(f'opcode {op:#x} at {s}')
        ins.append((op, imm, body[s:o]))
    return groups, ins


def encode_body(groups, ins):
    out = bytearray(enc_u(len(groups)))
    for c, t in groups:
        out += enc_u(c) + bytes([t])
    for _, _, raw in ins:
        out += raw
    return bytes(out)


def nlocals(groups):
    return sum(c for c, _ in groups)


def table0_slots(mod):
    """{function index: [slots]} of table 0's active element segments with constant offsets (TeaVM: one segment)."""
    s = mod.section(9)
    out = {}
    if s is None:
        return out
    b = s[1]
    n, o = uleb(b, 0)
    for _ in range(n):
        fl, o = uleb(b, o)
        assert fl in (0, 2), f'element segment flags {fl}'
        tbl = 0
        if fl == 2:
            tbl, o = uleb(b, o)
        assert b[o] == 0x41; off, o = sleb(b, o + 1); assert b[o] == 0x0b; o += 1
        if fl == 2:
            assert b[o] == 0x00; o += 1
        k, o = uleb(b, o)
        for j in range(k):
            f, o = uleb(b, o)
            if tbl == 0:
                out.setdefault(f, []).append(off + j)
    return out


def install_in_table(data, slots_by_name, table_module, table_field):
    """A split module: add active element segments writing each of its functions named in `slots_by_name` into the
    imported table (table_module, table_field) at those slots -- importing that table at the end of the table index
    space if the module does not already. Returns (new bytes, slots written)."""
    m = Module(data)
    s2 = m.section(2)
    b = s2[1]
    n, o = uleb(b, 0)
    after_count = o
    ntab = 0; tidx = None
    for _ in range(n):
        ln, o = uleb(b, o); mn = b[o:o + ln].decode(); o += ln
        ln, o = uleb(b, o); fn = b[o:o + ln].decode(); o += ln
        kind = b[o]; o += 1
        if kind == 0:
            _, o = uleb(b, o)
        elif kind == 1:
            if mn == table_module and fn == table_field:
                tidx = ntab
            o += 1; fl, o = uleb(b, o); _, o = uleb(b, o)
            if fl & 1: _, o = uleb(b, o)
            ntab += 1
        elif kind == 2:
            fl, o = uleb(b, o); _, o = uleb(b, o)
            if fl & 1: _, o = uleb(b, o)
        elif kind == 3:
            o += 2
        elif kind == 4:
            o += 1; _, o = uleb(b, o)
    segs = []
    for fi, name in m.names.items():
        for slot in slots_by_name.get(name, ()):
            segs.append((slot, fi))
    if not segs:
        return data, 0
    if tidx is None:
        # tables are indexed imports first: appending the import puts it after every imported table, but a DEFINED
        # table would shift; split modules define none
        assert m.section(4) is None, 'split module defines a table'
        tidx = ntab
        imp = enc_u(len(table_module)) + table_module.encode() + enc_u(len(table_field)) + table_field.encode() + b'\x01\x70\x00\x00'
        s2[1] = enc_u(n + 1) + b[after_count:] + imp
    es = m.section(9)
    new = b''.join(b'\x02' + enc_u(tidx) + b'\x41' + enc_s(slot) + b'\x0b\x00' + enc_u(1) + enc_u(fi) for slot, fi in segs)
    if es is None:
        at = next(i for i, s in enumerate(m.sections) if s[0] in (12, 10))
        m.sections.insert(at, [9, enc_u(len(segs)) + new])
    else:
        k, o = uleb(es[1], 0)
        es[1] = enc_u(k + len(segs)) + es[1][o:] + new
    return m.encode(), len(segs)


BLOCK_OPEN = {0x02, 0x03, 0x04, 0x06, 0x1f}


def inline(mod, callees, callers):
    """Inline every call to a function in `callees` (function indices, defined) inside every function in `callers`.
    Returns (functions changed, call sites inlined)."""
    cache = {}
    for f in callees:
        groups, ins = decode(mod.bodies[f - mod.nfimports])
        assert ins and ins[-1][0] == 0x0b
        ps, rs = mod.types[mod.func_type[f]]
        assert len(rs) <= 1, 'multi-value callee'
        cache[f] = (ps, rs, groups, ins)
    changed = sites = 0
    for c in callers:
        bi = c - mod.nfimports
        groups, ins = decode(mod.bodies[bi])
        if not any(op == 0x10 and imm in cache for op, imm, _ in ins):
            continue
        cps, _ = mod.types[mod.func_type[c]]
        base = len(cps) + nlocals(groups)
        alloc = {}                                   # callee -> first new local index
        new_groups = list(groups)
        out = []
        for op, imm, raw in ins:
            if op != 0x10 or imm not in cache:
                out.append((op, imm, raw)); continue
            ps, rs, cgroups, cins = cache[imm]
            if imm not in alloc:
                alloc[imm] = base
                for t in ps:
                    new_groups.append((1, t))
                for cnt, t in cgroups:
                    new_groups.append((cnt, t))
                base += len(ps) + nlocals(cgroups)
            lb = alloc[imm]
            for i in reversed(range(len(ps))):
                out.append((0x21, lb + i, b'\x21' + enc_u(lb + i)))
            li = lb + len(ps)
            for cnt, t in cgroups:
                for _ in range(cnt):
                    z = ZERO[t]
                    out.append((None, None, z)); out.append((0x21, li, b'\x21' + enc_u(li))); li += 1
            out.append((0x02, None, b'\x02' + (bytes(rs) if rs else b'\x40')))
            depth = 0
            for k, (cop, cimm, craw) in enumerate(cins):
                if k == len(cins) - 1:
                    out.append((0x0b, None, b'\x0b')); break
                if cop in (0x20, 0x21, 0x22):
                    out.append((cop, lb + cimm, bytes([cop]) + enc_u(lb + cimm)))
                elif cop == 0x0f:
                    out.append((0x0c, depth, b'\x0c' + enc_u(depth)))
                else:
                    if cop in BLOCK_OPEN:
                        depth += 1
                    elif cop in (0x0b, 0x18):
                        depth -= 1
                    out.append((cop, cimm, craw))
            sites += 1
        mod.bodies[bi] = encode_body(new_groups, out)
        changed += 1
    return changed, sites


# straight-line instructions a stored value may be built from: op -> (pops, pushes)
_PURE = {0x20: (0, 1), 0x22: (1, 1), 0x23: (0, 1), 0x41: (0, 1), 0x28: (1, 1), 0x45: (1, 1)}
for _op in range(0x6a, 0x79):
    _PURE[_op] = (2, 1)


_CONSUME = dict(_PURE)
for _op in range(0x46, 0x50):                        # i32 compares
    _CONSUME[_op] = (2, 1)
_CONSUME[0x21] = (1, 0); _CONSUME[0x1a] = (1, 0)    # local.set, drop
for _op in range(0x28, 0x36):
    _CONSUME[_op] = (1, 1)
for _op in range(0x36, 0x3f):
    _CONSUME[_op] = (2, 0)


def _role(ins, k):
    """How the value pushed by ins[k] is consumed: 'address' (a load's operand, or a store's address under its value),
    'value' (anything else that provably consumes it), or 'unknown' (control flow or an op not modelled first)."""
    depth = 1
    for j in range(k + 1, len(ins)):
        op = ins[j][0]
        if op not in _CONSUME:
            return 'unknown'
        p, q = _CONSUME[op]
        if p > depth - 1:                            # this instruction consumes ins[k]'s value
            if isinstance(op, int) and 0x28 <= op <= 0x35:
                return 'address'
            if isinstance(op, int) and 0x36 <= op <= 0x3e:
                return 'address' if depth == 2 else 'value'
            return 'value'
        depth = depth - p + q
    return 'unknown'


def word_to_global(mod, addr):
    """Move the i32 at linear address `addr` into a new module-defined mutable global. Returns (global index, loads
    rewritten, stores rewritten, refused) -- a non-zero refused count means some access did not match and the module
    must not be used."""
    g = mod.nimported_globals + mod.nglobals
    gs = mod.section(6)
    ent = b'\x7f\x01\x41\x00\x0b'                    # i32, mutable, init i32.const 0
    if gs is None:
        # the global section goes after tag (13) / memory (5) / table (4) / function (3), before export (7)
        order = [1, 2, 3, 4, 5, 13]
        at = max(i for i, s in enumerate(mod.sections) if s[0] in order) + 1
        mod.sections.insert(at, [6, enc_u(1) + ent])
    else:
        n, o = uleb(gs[1], 0)
        gs[1] = enc_u(n + 1) + gs[1][o:] + ent
    mod.nglobals += 1
    gget = b'\x23' + enc_u(g); gset = b'\x24' + enc_u(g)
    loads = stores = refused = 0
    needle = b'\x41' + enc_s(addr)
    for bi, body in enumerate(mod.bodies):
        if needle not in body:
            continue
        groups, ins = decode(body)
        if not any(op == 0x41 and imm == addr for op, imm, _ in ins):
            continue
        # pass 1: loads
        out = []
        k = 0
        while k < len(ins):
            op, imm, raw = ins[k]
            if op == 0x41 and imm == addr and k + 1 < len(ins) and ins[k + 1][0] == 0x28 and ins[k + 1][1][1] == 0:
                out.append((0x23, g, gget)); loads += 1; k += 2; continue
            out.append(ins[k]); k += 1
        ins = out
        # pass 2: stores whose address is the constant
        drop = set(); repl = {}
        for k, (op, imm, raw) in enumerate(ins):
            if not (op == 0x41 and imm == addr):
                continue
            depth = 1; j = k + 1; ok = False
            while j < len(ins):
                jop, jimm, _ = ins[j]
                if jop == 0x36 and depth == 2 and jimm[1] == 0:
                    ok = True; break
                if jop not in _PURE:
                    break
                p, q = _PURE[jop]
                if p > depth - 1:                    # would consume the address constant itself
                    break
                depth = depth - p + q
                j += 1
            if ok:
                drop.add(k); repl[j] = (0x24, g, gset); stores += 1
            elif _role(ins, k) != 'value':
                refused += 1                         # an address use (or an unknown one) that did not match
        ins = [repl.get(k, x) for k, x in enumerate(ins) if k not in drop]
        mod.bodies[bi] = encode_body(groups, ins)
    return g, loads, stores, refused
