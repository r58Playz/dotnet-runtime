#!/usr/bin/env python3
"""j2d/addrregion.py -- name the region a sampled DATA ADDRESS lies in (j2d/wledger.py --addr).

Inputs: <rundir>/maps-benchEnd.txt (else -benchStart; mcperf copies the renderer's /proc/<pid>/maps at the window's
edges), <rundir>/phases.json (window.heapMb = wasmMemory.buffer.byteLength), and the served dotnet.native.wasm's
__stack_pointer initialiser (the top of the main thread's stack: below it lie static data and that stack).

Regions:
  wasm:static     linear memory below the __stack_pointer initialiser (mono/emscripten statics, the main stack)
  wasm:sstack     linear memory inside THIS thread's shadow stack: the address range the emitter's own frame constructs
                  (PIN / FRAME / SLOTZERO / EXIT / EH_PUSH / EH_POP / BB_ILOFS stores and loads) touch, widened to the
                  enclosing 1 MiB-aligned window (a pthread's stack is one malloc'd block)
  wasm:heap       the rest of linear memory (SGen objects, malloc'd metadata: vtables, classes, PIC tables, ...)
  v8-sandbox      [anon:v8-sandbox] outside linear memory: the JS heap (FixedArrays, JSArrayBuffers, WasmGlobalObject
                  buffers, the instance object)
  v8              [anon:v8] rw: trusted space (WasmTrustedInstanceData, WasmDispatchTable), code metadata
  v8-code         [anon:v8] rwx/r-x: code space (jump tables, embedded constants)
  v8-ptrtable     [anon:v8-pointer-table]
  native-stack    the anonymous mapping that holds the thread's spill/reload addresses (V8's frames)
  partition_alloc / anon / <file> / unmapped
Identification of linear memory: the rw-p [anon:v8-sandbox] mapping whose size is nearest heapMb (>= 256 MiB). It is
VERIFIED from the samples (calibrate): the share of body heap loads (tag op:*, x86 heap LOAD) that fall inside it is
reported, and a low share means the pick is wrong."""
import bisect, collections, glob, json, os, re, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402


def _leb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7f) << s; s += 7
        if x < 0x80:
            return r, p


def _sleb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7f) << s; s += 7
        if x < 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, p


def stack_pointer_init(path):
    """global[0]'s i32.const initialiser of a wasm module (emscripten's __stack_pointer)."""
    b = open(path, 'rb').read()
    p = 8
    while p < len(b):
        sid = b[p]; p += 1
        n, p = _leb(b, p)
        if sid == 6:
            q = p
            cnt, q = _leb(b, q)
            if cnt:
                q += 2                     # valtype, mut
                if b[q] == 0x41:
                    v, _ = _sleb(b, q + 1)
                    return v & 0xffffffff
            return None
        p += n
    return None


FRAME_TAGS = {'PIN', 'FRAME', 'SLOTZERO', 'EXIT', 'EH_PUSH', 'EH_POP', 'BB_ILOFS', 'REF_LD', 'ADDR_LD'}


class Regions:
    def __init__(self, rundir, served_glob=None):
        if served_glob is None:
            served_glob = os.path.join(paths.need("IKVMCRAFT"), "frontend/public/_framework/dotnet.native.*.wasm")
        mp = next((p for p in (os.path.join(rundir, 'maps-benchEnd.txt'), os.path.join(rundir, 'maps-benchStart.txt'))
                   if os.path.exists(p)), None)
        if not mp:
            raise SystemExit(f'{rundir}: no maps-bench*.txt (mcperf copies them; rerun with the current mcperf.mjs)')
        self.maps_path = mp
        self.vmas = []
        for ln in open(mp):
            f = ln.split(None, 5)
            lo, hi = (int(x, 16) for x in f[0].split('-'))
            self.vmas.append((lo, hi, f[1], f[5].strip() if len(f) > 5 else ''))
        self.vmas.sort()
        self.starts = [v[0] for v in self.vmas]
        heap_mb = None
        try:
            heap_mb = json.load(open(os.path.join(rundir, 'phases.json')))['window'].get('heapMb')
        except Exception:
            pass
        cands = [v for v in self.vmas if v[3] == '[anon:v8-sandbox]' and v[2].startswith('rw') and v[1] - v[0] >= 256 << 20]
        if not cands:
            raise SystemExit(f'{mp}: no rw [anon:v8-sandbox] mapping >= 256 MiB to call linear memory')
        tgt = (heap_mb or 0) * 1048576
        lm = min(cands, key=lambda v: abs((v[1] - v[0]) - tgt)) if heap_mb else max(cands, key=lambda v: v[1] - v[0])
        self.heap_mb = heap_mb
        self.lm_lo, self.lm_hi = lm[0], lm[1]
        # linear memory's base: the mapping may have merged with sandbox pages BELOW it; the base is lm_hi - byteLength
        # when byteLength is known and fits, else the mapping start
        if heap_mb and (lm[1] - lm[0]) >= tgt:
            self.base = lm[1] - int(tgt)
            self.base -= self.base % 65536
        else:
            self.base = lm[0]
        wasm = sorted(glob.glob(served_glob), key=os.path.getmtime)
        self.sp_init = stack_pointer_init(wasm[-1]) if wasm else None
        self.sstack = None
        self.nstack = None
        self._frame_addrs = []
        self._spill_addrs = []
        self.check = collections.Counter()

    def vma(self, ad):
        k = bisect.bisect_right(self.starts, ad) - 1
        if k >= 0 and self.vmas[k][0] <= ad < self.vmas[k][1]:
            return self.vmas[k]
        return None

    def observe(self, tag, x86, ad):
        if tag in FRAME_TAGS and self.base <= ad < self.lm_hi:
            self._frame_addrs.append(ad)
        elif x86.startswith('SPILL/RELOAD'):
            self._spill_addrs.append(ad)
        if tag.startswith('op:') and x86 == 'heap LOAD':
            self.check['body_heap_loads'] += 1
            if self.base <= ad < self.lm_hi:
                self.check['in_linear_memory'] += 1

    def calibrate(self):
        def window(addrs):
            if not addrs:
                return None
            a = sorted(addrs)
            med = a[len(a) // 2]
            lo = med - (med % (1 << 20)); hi = lo + (1 << 20)
            # widen while at least 1% of the observations fall outside
            out = sum(1 for x in a if not (lo <= x < hi))
            while out > len(a) // 100 and hi - lo < (64 << 20):
                lo -= 1 << 20; hi += 1 << 20
                out = sum(1 for x in a if not (lo <= x < hi))
            return (lo, hi, len(a), out)
        self.sstack = window(self._frame_addrs)
        if self._spill_addrs:
            c = collections.Counter(self.vma(x) for x in self._spill_addrs)
            v, n = c.most_common(1)[0]
            self.nstack = (v, n, len(self._spill_addrs))

    def name(self, ad):
        if self.base <= ad < self.lm_hi:
            off = ad - self.base
            if self.sstack and self.sstack[0] <= ad < self.sstack[1]:
                return 'wasm:sstack'
            if self.sp_init is not None and off < self.sp_init:
                return 'wasm:static'
            return 'wasm:heap'
        v = self.vma(ad)
        if v is None:
            return 'unmapped'
        if self.nstack and self.nstack[0] and v == self.nstack[0]:
            return 'native-stack'
        nm = v[3]
        if nm == '[anon:v8-sandbox]':
            return 'v8-sandbox'
        if nm == '[anon:v8]':
            return 'v8-code' if 'x' in v[2] else 'v8'
        if nm == '[anon:v8-pointer-table]':
            return 'v8-ptrtable'
        if nm == '[anon:partition_alloc]':
            return 'partition_alloc'
        if nm in ('', '[heap]') or nm.startswith('[anon:'):
            return 'anon' if nm == '' else nm
        return os.path.basename(nm)

    def describe(self):
        return dict(maps=self.maps_path, heap_mb=self.heap_mb, linear_memory=[hex(self.base), hex(self.lm_hi)],
                    mapping=[hex(self.lm_lo), hex(self.lm_hi)], sp_init=self.sp_init,
                    sstack=[hex(self.sstack[0]), hex(self.sstack[1]), self.sstack[2], self.sstack[3]] if self.sstack else None,
                    nstack=[hex(self.nstack[0][0]), hex(self.nstack[0][1]), self.nstack[1], self.nstack[2]] if self.nstack else None,
                    check=dict(self.check))
