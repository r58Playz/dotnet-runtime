// t10 (runtime half): validate + execute the guarded-call modules t10.c framed. See t10.c.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const uleb = (v) => { const o = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; o.push(b); } while (v); return o; };
const sleb = (v) => { const o = []; for (;;) { const b = v & 0x7f; v >>= 7; if ((v === 0 && !(b & 0x40)) || (v === -1 && (b & 0x40))) { o.push(b); return o; } o.push(b | 0x80); } };
const str = (s) => [...uleb(s.length), ...Buffer.from(s)];
const sec = (id, body) => [id, ...uleb(body.length), ...body];
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65535, shared: true });
const dv = () => new DataView(mem.buffer);
// the "new body" the table holds at the callee's f-slot: same functype, a distinguishable result
function newBody (which) {
  const type = which === 0 ? [0x60, 1, 0x7f, 1, 0x7f] : which === 1 ? [0x60, 2, 0x7e, 0x7c, 1, 0x7c] : [0x60, 1, 0x7f, 0];
  const imp = [1, ...str('m'), ...str('h'), 0x02, 0x03, ...uleb(256), ...uleb(65535)];
  const body = which === 0 ? [0, 0x20, 0, 0x41, ...sleb(100), 0x6a, 0x0b]                       // x + 100
             : which === 1 ? [0, 0x44, ...new Uint8Array(new Float64Array([-1]).buffer), 0x0b]   // -1.0
             : [0, 0x41, ...sleb(2052), 0x20, 0, 0x36, 2, 0, 0x0b];                              // store x at 2052
  const code = [1, ...uleb(body.length), ...body];
  return new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, [1, ...type]), ...sec(2, imp), ...sec(3, [1, 0]),
    ...sec(7, [1, ...str('t'), 0x00, 0]), ...sec(10, code)]);
}
let fails = 0, runs = 0;
const bad = (m) => { if (fails++ < 20) console.log('t10 FAIL: ' + m); };
for (const line of fs.readFileSync('t10-manifest.txt', 'utf8').trim().split('\n')) {
  const [fn, ws, gs, cs, fs_, nx] = line.split(' ');
  const which = +ws, GUARD = +gs, COUNT = +cs, FSLOT = +fs_, NEXPORT = nx === undefined ? 2 : +nx;
  try { execFileSync('wasm-validate', ['--enable-all', fn]); } catch (e) { bad(`${fn}: wasm-validate: ${e.stdout || e.message}`); continue; }
  const table = new WebAssembly.Table({ initial: 64, element: 'anyfunc' });
  const g = (v) => new WebAssembly.Global({ value: 'i32', mutable: false }, v);
  const s = { p: new WebAssembly.Global({ value: 'i32', mutable: true }, 1 << 20), l: g(0), c: g(0), v: g(0), n: g(0), d: g(0), m: g(0), b: g(0), i: g(0), g: g(0) };
  let inst;
  try {
    table.set(FSLOT, new WebAssembly.Instance(new WebAssembly.Module(newBody(which)), { m: { h: mem } }).exports.t);
    inst = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(fn)), { m: { h: mem }, f: { f: table }, s });
  } catch (e) { bad(`${fn}: instantiate: ${e.message}`); continue; }
  const A = NEXPORT === 1 ? inst.exports.f : inst.exports.f0;
  if (NEXPORT === 1) {   // R452: the private copy must not be reachable from outside the module
    const names = Object.keys(inst.exports).sort().join(',');
    if (names !== 'e,f') bad(`${fn}: private member exported (exports ${names})`);
  }
  for (const guard of [0, 1, 0, 1]) {
    runs++;
    dv().setInt32(GUARD, guard, true); const c0 = dv().getInt32(COUNT, true);
    dv().setInt32(2048, 0, true); dv().setInt32(2052, 0, true);
    let r;
    try { r = which === 0 ? A(41) : which === 1 ? A(7n, 0.5) : A(1234); } catch (e) { bad(`${fn} guard=${guard}: threw ${e.message}`); continue; }
    const c1 = dv().getInt32(COUNT, true);
    if (which === 0 && r !== (guard ? 141 : 42)) bad(`${fn} guard=${guard}: got ${r}`);
    if (which === 1 && r !== (guard ? -1 : 7.5)) bad(`${fn} guard=${guard}: got ${r}`);
    if (which === 2 && (dv().getInt32(guard ? 2052 : 2048, true) !== 1234 || dv().getInt32(guard ? 2048 : 2052, true) !== 0)) bad(`${fn} guard=${guard}: side effect in the wrong body`);
    if (c1 - c0 !== guard) bad(`${fn} guard=${guard}: counter moved ${c1 - c0}`);
  }
}
console.log(`t10: ${runs} guarded calls executed, ${fails} failures`);
process.exit(fails ? 1 : 0);
