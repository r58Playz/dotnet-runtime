// t11 (runtime half): validate + execute every vslow stub t11.c framed. See t11.c for what is checked.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const uleb = (v) => { const o = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; o.push(b); } while (v); return o; };
const sleb = (v) => { const o = []; for (;;) { const b = v & 0x7f; v >>= 7; if ((v === 0 && !(b & 0x40)) || (v === -1 && (b & 0x40))) { o.push(b); return o; } o.push(b | 0x80); } };
const str = (s) => [...uleb(s.length), ...Buffer.from(s)];
const sec = (id, body) => [id, ...uleb(body.length), ...body];
// A callee: stores each param at base + 8*j, the extra rgctx (if any) at base + 8*n, returns a sentinel.
// For an i32 return the sentinel has dirty upper bits (0x12345681) so normalisation is visible.
function target (params, ret, base) {
  const type = [0x60, ...uleb(params.length), ...params, ...(ret === 0x40 ? [0] : [1, ret])];
  const imp = [1, ...str('m'), ...str('h'), 0x02, 0x03, ...uleb(256), ...uleb(65535)];
  const body = [0];
  params.forEach((p, j) => {
    body.push(0x41, ...sleb(base + 8 * j), 0x20, ...uleb(j));
    body.push(...({ 0x7f: [0x36, 2], 0x7e: [0x37, 3], 0x7d: [0x38, 2], 0x7c: [0x39, 3] }[p]), 0);
  });
  if (ret === 0x7f) body.push(0x41, ...sleb(0x12345681));
  if (ret === 0x7e) body.push(0x42, ...sleb(-7777));
  if (ret === 0x7d) body.push(0x43, ...new Uint8Array(new Float32Array([2.5]).buffer));
  if (ret === 0x7c) body.push(0x44, ...new Uint8Array(new Float64Array([-3.25]).buffer));
  body.push(0x0b);
  const code = [1, ...uleb(body.length), ...body];
  const exp = [1, ...str('t'), 0x00, 0];
  return new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, [1, ...type]), ...sec(2, imp), ...sec(3, [1, 0]), ...sec(7, exp), ...sec(10, code)]);
}
const norm = (rn, v) => [v, v & 0xff, (v << 24) >> 24, v & 0xffff, (v << 16) >> 16][rn];
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65535, shared: true });
const dv = () => new DataView(mem.buffer);
const SP0 = 1 << 20, SCRATCH = 8192, THIS = 4096, VT = 0x7700, DESC = 4608, AIC = 4864;
// MONO_WASM_JIT_SP_ARG stubs (t11sp-manifest.txt): the frame base the stub is handed, and where s.p stands on entry --
// equal to the base in mode 1 (that convention keeps them equal), stale and lower in mode 2.
const BASE = SP0 - 8192, STALE = SP0 - 65536;
const T_SITE = 40, T_AT = 41, T_JIT = 42;   // table slots: at_ne / the JIT f (site functype), at (+ rgctx)
const B_SITE = 16384, B_AT = 17408, B_JIT = 18432;
let fails = 0, runs = 0;
const bad = (m) => { if (fails++ < 25) console.log('t11 FAIL: ' + m); };
const argv = (p, j) => j === 0 ? THIS : ({ 0x7f: 1000 + j * 37, 0x7e: BigInt(-(2 ** 40) + j), 0x7d: Math.fround(j + 0.5), 0x7c: j * 1.25 - 9 }[p]);
const rd = (p, a) => ({ 0x7f: () => dv().getInt32(a, true), 0x7e: () => dv().getBigInt64(a, true), 0x7d: () => dv().getFloat32(a, true), 0x7c: () => dv().getFloat64(a, true) }[p])();
const wr = (p, a, v) => ({ 0x7f: () => dv().setInt32(a, v, true), 0x7e: () => dv().setBigInt64(a, v, true), 0x7d: () => dv().setFloat32(a, v, true), 0x7c: () => dv().setFloat64(a, v, true) }[p])();
const ROUTES = ['aic-ne', 'aic-rg', 'resolve', 'aot-ne', 'aot-rg', 'interp', 'throw', 'noaic-resolve'];
const lines = fs.readFileSync('t11-manifest.txt', 'utf8').trim().split('\n').map((l) => '0 ' + l)
  .concat(fs.readFileSync('t11sp-manifest.txt', 'utf8').trim().split('\n').map((l) => { const [fn, m, ...r] = l.split(' '); return [m, fn, ...r].join(' '); }));
for (const line of lines) {
  const [ms, fn, rns, rets, ...ps] = line.split(' ');
  const spm = +ms, rn = +rns, ret = +rets, params = ps.map(Number);
  const site = spm ? params.slice(0, -1) : params;   // the site's arguments: a trailing frame base is not one
  const base = spm ? BASE : SP0, sp0 = spm === 2 ? STALE : base;
  try { execFileSync('wasm-validate', ['--enable-all', fn]); } catch (e) { bad(`${fn}: wasm-validate: ${e.stdout || e.message}`); continue; }
  const table = new WebAssembly.Table({ initial: 64, element: 'anyfunc' });
  const sp = new WebAssembly.Global({ value: 'i32', mutable: true }, sp0);
  const g = (v) => new WebAssembly.Global({ value: 'i32', mutable: false }, v);
  const i32idx = site.map((p, j) => [p, j]).filter(([p]) => p === 0x7f).map(([, j]) => j);
  const frame = (i32idx.length * 4 + 15) & ~15;
  let route = '', curArgs = null, pinOK = true, resolveArgs = null, interpArgs = null, unwound = false, aotArgs = null;
  const pinCheck = () => {
    const s = sp.value;
    if (!(s < base) || (s & 15) || (spm && s !== base - frame)) { pinOK = false; return; }
    i32idx.forEach((j, kth) => { if (dv().getInt32(s + 4 * kth, true) !== curArgs[j]) pinOK = false; });
  };
  const h = {
    1: (th, desc, scr) => { resolveArgs = [th, desc, scr]; pinCheck(); return route === 'resolve' || route === 'noaic-resolve' ? T_JIT : 0; },
    2: () => SCRATCH,
    3: (scr, th, aic) => { aotArgs = [scr, th, aic]; pinCheck();
      if (route === 'aot-ne') { dv().setInt32(scr + 212, T_SITE, true); return 2; }
      if (route === 'aot-rg') { dv().setInt32(scr + 212, T_AT, true); dv().setInt32(scr + 216, 0x5151, true); return 1; }
      dv().setInt32(scr + 200, 0xbeef, true); return 0; },
    4: (tgt, buf) => { interpArgs = [tgt, buf, site.map((p, j) => rd(p, buf + 8 * j))]; pinCheck();
      if (spm && dv().getInt32(buf + 8 * site.length, true) !== 0x5a5a5a5a) pinOK = false;   // the base is not spilled
      if (route === 'throw') return 1;
      if (ret !== 0x40) wr(ret, buf + 192, { 0x7f: 0x7abc0181, 0x7e: 99n, 0x7d: 0.75, 0x7c: 6.5 }[ret]);
      return 0; },
    5: () => { unwound = true; throw new Error('unwind'); },
  };
  const s = { p: sp, l: g(0), c: g(0), v: g(0), n: g(0), d: g(0), m: g(0), b: g(0), i: g(0), g: g(0) };
  let stub;
  try {
    table.set(T_SITE, new WebAssembly.Instance(new WebAssembly.Module(target(site, ret, B_SITE)), { m: { h: mem } }).exports.t);
    table.set(T_AT, new WebAssembly.Instance(new WebAssembly.Module(target([...site, 0x7f], ret, B_AT)), { m: { h: mem } }).exports.t);
    table.set(T_JIT, new WebAssembly.Instance(new WebAssembly.Module(target(params, ret, B_JIT)), { m: { h: mem } }).exports.t);
    new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(fn)), { m: { h: mem }, f: { f: table }, s, h });
    stub = table.get(16);
  } catch (e) { bad(`${fn}: instantiate: ${e.message}`); continue; }
  if (!stub) { bad(`${fn}: slot 16 empty`); continue; }
  // the placeholder-collision rule: the stub's last parameter is an i64 (calling it with a Number must throw)
  try { stub(...params.map(argv), 0); bad(`${fn}: descriptor accepted a Number (not i64)`); } catch (e) { /* expected TypeError */ }
  for (route of ROUTES) {
    runs++;
    curArgs = params.map(argv);
    if (spm) curArgs[site.length] = base;
    pinOK = true; resolveArgs = interpArgs = aotArgs = null; unwound = false;
    new Uint8Array(mem.buffer, B_SITE, 3072).fill(0);
    new Uint8Array(mem.buffer, SCRATCH, 192).fill(0x5a);
    sp.value = sp0;
    dv().setInt32(THIS, VT, true);
    const aic = route === 'noaic-resolve' ? 0 : AIC;
    dv().setInt32(DESC + 4, aic, true);
    // AIC words: an entry for VT only on the inline-AOT-IC routes, else a different vtable (a miss)
    const hit = route === 'aic-ne' || route === 'aic-rg';
    dv().setInt32(AIC + 0, hit ? VT : VT + 8, true); dv().setInt32(AIC + 8, hit ? VT : VT + 8, true);
    dv().setInt32(AIC + 4, route === 'aic-ne' ? (T_SITE << 1) | 1 : (T_AT << 1), true);
    dv().setInt32(AIC + 12, 0x6262, true);
    let r, threw = false;
    try { r = stub(...curArgs, BigInt(DESC)); } catch (e) { threw = true; }
    if (route !== 'throw' && sp.value !== base) bad(`${fn} ${route}: s.p not restored (${sp.value})`);
    if (!pinOK) bad(`${fn} ${route}: i32 params not pinned below ${spm ? 'the frame base' : 's.p'} in a helper`);
    // what a callee stored: a JIT f (B_JIT) gets every param, the frame base last; an AOT body the site's alone (+ rgctx)
    const expectArgs = (at, extra) => {
      const ps = at === B_JIT ? params : site;
      ps.forEach((p, j) => { if (rd(p, at + 8 * j) !== curArgs[j]) bad(`${fn} ${route}: arg ${j} ${rd(p, at + 8 * j)} != ${curArgs[j]}`); });
      if (extra !== undefined && dv().getInt32(at + 8 * ps.length, true) !== extra) bad(`${fn} ${route}: rgctx ${dv().getInt32(at + 8 * ps.length, true)} != ${extra}`);
    };
    const want = (normed) => ({ 0x7f: normed ? norm(rn, 0x12345681) : 0x12345681, 0x7e: -7777n, 0x7d: 2.5, 0x7c: -3.25, 0x40: undefined }[ret]);
    if (threw && route !== 'throw') { bad(`${fn} ${route}: threw`); continue; }
    switch (route) {
    case 'aic-ne': expectArgs(B_SITE); if (r !== want(true)) bad(`${fn} ${route}: ret ${r} != ${want(true)}`); if (resolveArgs) bad(`${fn} ${route}: resolve called on an AOT IC hit`); break;
    case 'aic-rg': expectArgs(B_AT, 0x6262); if (r !== want(true)) bad(`${fn} ${route}: ret ${r}`); break;
    case 'resolve': case 'noaic-resolve':
      if (!resolveArgs || resolveArgs[0] !== THIS || resolveArgs[1] !== DESC || resolveArgs[2] !== SCRATCH) { bad(`${fn} ${route}: resolve args ${resolveArgs}`); break; }
      expectArgs(B_JIT); if (r !== want(false)) bad(`${fn} ${route}: tail-call ret ${r} != ${want(false)} (a JIT f is not normalised)`); break;
    case 'aot-ne': if (!aotArgs || aotArgs[2] !== AIC) bad(`${fn} ${route}: aot args ${aotArgs}`); expectArgs(B_SITE); if (r !== want(true)) bad(`${fn} ${route}: ret ${r}`); break;
    case 'aot-rg': expectArgs(B_AT, 0x5151); if (r !== want(true)) bad(`${fn} ${route}: ret ${r}`); break;
    case 'interp': {
      if (!interpArgs || interpArgs[0] !== 0xbeef || interpArgs[1] !== SCRATCH) { bad(`${fn} ${route}: interp not called right`); break; }
      interpArgs[2].forEach((v, j) => { if (v !== curArgs[j]) bad(`${fn} ${route}: scratch arg ${j} ${v} != ${curArgs[j]}`); });
      const w = { 0x7f: norm(rn, 0x7abc0181), 0x7e: 99n, 0x7d: 0.75, 0x7c: 6.5, 0x40: undefined }[ret];
      if (r !== w) bad(`${fn} ${route}: interp ret ${r} != ${w}`);
      break; }
    case 'throw': if (!unwound || !threw) bad(`${fn} ${route}: throw did not reach continue_unwind`); break;
    }
    sp.value = sp0;
  }
}
console.log(`t11: ${runs} stub calls, ${fails} failures`);
process.exit(fails ? 1 : 0);
