// t7 (runtime half): validate + execute every bank t7.c framed. See t7.c for what is checked.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const T = { 0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64', 0x40: 'void' };
const uleb = (v) => { const o = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; o.push(b); } while (v); return o; };
const sleb = (v) => { const o = []; for (;;) { const b = v & 0x7f; v >>= 7; if ((v === 0 && !(b & 0x40)) || (v === -1 && (b & 0x40))) { o.push(b); return o; } o.push(b | 0x80); } };
const str = (s) => [...uleb(s.length), ...Buffer.from(s)];
const sec = (id, body) => [id, ...uleb(body.length), ...body];
// A target of the stub's functype: stores each param at 1024 + 8*j and returns a sentinel.
function target (params, ret) {
  const type = [0x60, ...uleb(params.length), ...params, ...(ret === 0x40 ? [0] : [1, ret])];
  const imp = [1, ...str('m'), ...str('h'), 0x02, 0x03, ...uleb(256), ...uleb(65535)];
  const body = [0];
  params.forEach((p, j) => {
    body.push(0x41, ...sleb(1024 + 8 * j), 0x20, ...uleb(j));
    body.push(...({ 0x7f: [0x36, 2], 0x7e: [0x37, 3], 0x7d: [0x38, 2], 0x7c: [0x39, 3] }[p]), 0);
  });
  if (ret === 0x7f) body.push(0x41, ...sleb(-77));
  if (ret === 0x7e) body.push(0x42, ...sleb(-7777));
  if (ret === 0x7d) body.push(0x43, ...new Uint8Array(new Float32Array([2.5]).buffer));
  if (ret === 0x7c) body.push(0x44, ...new Uint8Array(new Float64Array([-3.25]).buffer));
  body.push(0x0b);
  const code = [1, ...uleb(body.length), ...body];
  const exp = [1, ...str('t'), 0x00, 0];
  return new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, [1, ...type]), ...sec(2, imp), ...sec(3, [1, 0]), ...sec(7, exp), ...sec(10, code)]);
}
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65535, shared: true });
const dv = () => new DataView(mem.buffer);
const SP0 = 1 << 20, SCRATCH = 8192, TARGET = 60;
// MONO_WASM_JIT_SP_ARG banks (t7sp-manifest.txt): the frame base the stub is handed, and where s.p stands on entry --
// equal to the base in mode 1 (that convention keeps them equal), stale and lower in mode 2.
const BASE = SP0 - 8192, STALE = SP0 - 65536;
let fails = 0, runs = 0;
const bad = (m) => { if (fails++ < 20) console.log('t7 FAIL: ' + m); };
const argv = (p, j) => ({ 0x7f: 1000 + j * 37, 0x7e: BigInt(-(2 ** 40) + j), 0x7d: Math.fround(j + 0.5), 0x7c: j * 1.25 - 9 }[p]);
const rd = (p, a) => ({ 0x7f: () => dv().getInt32(a, true), 0x7e: () => dv().getBigInt64(a, true), 0x7d: () => dv().getFloat32(a, true), 0x7c: () => dv().getFloat64(a, true) }[p])();
const wr = (p, a, v) => ({ 0x7f: () => dv().setInt32(a, v, true), 0x7e: () => dv().setBigInt64(a, v, true), 0x7d: () => dv().setFloat32(a, v, true), 0x7c: () => dv().setFloat64(a, v, true) }[p])();
const lines = fs.readFileSync('t7-manifest.txt', 'utf8').trim().split('\n').map((l) => '0 ' + l)
  .concat(fs.readFileSync('t7sp-manifest.txt', 'utf8').trim().split('\n').map((l) => { const [fn, m, ...r] = l.split(' '); return [m, fn, ...r].join(' '); }));
for (const line of lines) {
  const [ms, fn, ks, rets, ...ps] = line.split(' ');
  const spm = +ms, k = +ks, ret = +rets, params = ps.map(Number);
  const na = spm ? params.length - 1 : params.length;   // the callee's arguments: a trailing frame base is not one
  const base = spm ? BASE : SP0, sp0 = spm === 2 ? STALE : base;
  try { execFileSync('wasm-validate', ['--enable-all', fn]); } catch (e) { bad(`${fn}: wasm-validate: ${e.stdout || e.message}`); continue; }
  const table = new WebAssembly.Table({ initial: 64, element: 'anyfunc' });
  const sp = new WebAssembly.Global({ value: 'i32', mutable: true }, sp0);
  const g = (v) => new WebAssembly.Global({ value: 'i32', mutable: false }, v);
  let mode = 0, bindSlot = -1, interpArgs = null, unwound = false, pinOK = true;
  const i32idx = params.slice(0, na).map((p, j) => [p, j]).filter(([p]) => p === 0x7f).map(([, j]) => j);
  const frame = (i32idx.length * 4 + 15) & ~15;
  let curArgs = null;
  const h = {
    1: (slot) => {   // bind: check the pin frame while "the GC could run"
      bindSlot = slot;
      if (i32idx.length) {
        const s = sp.value;
        if (!(s < base) || (s & 15) || (spm && s !== base - frame)) pinOK = false;
        i32idx.forEach((j, kth) => { if (dv().getInt32(s + 4 * kth, true) !== curArgs[j]) pinOK = false; });
      } else if (sp.value !== base) pinOK = false;
      return mode === 0 ? TARGET : 0;
    },
    2: () => SCRATCH,
    3: (slot, buf) => { interpArgs = [slot, buf, params.slice(0, na).map((p, j) => rd(p, buf + 8 * j))];
      if (spm && (sp.value !== base - frame || dv().getInt32(buf + 8 * na, true) !== 0x5a5a5a5a)) pinOK = false;   // native code below the base; the base not spilled
      if (mode === 2) return 1;
      if (ret !== 0x40) wr(ret, buf + 192, { 0x7f: 4242, 0x7e: 99n, 0x7d: 0.75, 0x7c: 6.5 }[ret]);
      return 0; },
    4: () => { unwound = true; throw new Error('unwind'); },
  };
  const s = { p: sp, l: g(0), c: g(0), v: g(0), n: g(0), d: g(0), m: g(0), b: g(0), i: g(0), g: g(0) };
  try {
    const tmod = new WebAssembly.Instance(new WebAssembly.Module(target(params, ret)), { m: { h: mem } });
    table.set(TARGET, tmod.exports.t);
    new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(fn)), { m: { h: mem }, f: { f: table }, s, h });
  } catch (e) { bad(`${fn}: instantiate: ${e.message}`); continue; }
  for (let i = 0; i < k; ++i) {
    const slot = 16 + 2 * i, stub = table.get(slot);
    if (!stub) { bad(`${fn}: slot ${slot} empty`); continue; }
    curArgs = params.map(argv);
    if (spm) curArgs[na] = base;
    for (mode = 0; mode < 3; ++mode) {
      runs++; bindSlot = -1; interpArgs = null; unwound = false; pinOK = true;
      new Uint8Array(mem.buffer, 1024, 256).fill(0);
      new Uint8Array(mem.buffer, SCRATCH, 192).fill(0x5a);
      sp.value = sp0;
      let r, threw = false;
      try { r = stub(...curArgs); } catch (e) { threw = true; }
      if (bindSlot !== slot) bad(`${fn} stub ${i} mode ${mode}: bind got ${bindSlot}`);
      if (!pinOK) bad(`${fn} stub ${i} mode ${mode}: i32 params not pinned below ${spm ? 'the frame base' : 's.p'}`);
      if (mode !== 2 && sp.value !== base) bad(`${fn} stub ${i} mode ${mode}: s.p not restored (${sp.value})`);
      if (mode === 0) {
        if (threw) { bad(`${fn} stub ${i}: tail call threw`); continue; }
        params.forEach((p, j) => { if (rd(p, 1024 + 8 * j) !== curArgs[j]) bad(`${fn} stub ${i}: target arg ${j} ${rd(p, 1024 + 8 * j)} != ${curArgs[j]}`); });
        const want = { 0x7f: -77, 0x7e: -7777n, 0x7d: 2.5, 0x7c: -3.25, 0x40: undefined }[ret];
        if (r !== want) bad(`${fn} stub ${i}: tail-call ret ${r} != ${want}`);
      } else {
        if (!interpArgs || interpArgs[0] !== slot || interpArgs[1] !== SCRATCH) { bad(`${fn} stub ${i}: interp not called right`); continue; }
        interpArgs[2].forEach((v, j) => { if (v !== curArgs[j]) bad(`${fn} stub ${i}: scratch arg ${j} ${v} != ${curArgs[j]}`); });
        if (mode === 1) {
          const want = { 0x7f: 4242, 0x7e: 99n, 0x7d: 0.75, 0x7c: 6.5, 0x40: undefined }[ret];
          if (threw || r !== want) bad(`${fn} stub ${i}: interp ret ${r} != ${want} (threw ${threw})`);
        } else if (!unwound || !threw) bad(`${fn} stub ${i}: throw did not reach continue_unwind`);
      }
      sp.value = sp0;   // a throw leaves s.p to the catching frame's landing pad, as a JIT frame without one does

    }
  }
}
console.log(`t7: ${runs} stub calls, ${fails} failures`);
process.exit(fails ? 1 : 0);
