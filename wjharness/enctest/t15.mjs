// t15 (runtime half): validate + execute the module-defined-globals modules t15.c framed. See t15.c.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
let fails = 0;
const bad = (m) => { fails++; console.log('t15 FAIL: ' + m); };
for (const fn of ['t15.wasm', 't15-none.wasm']) {
  try { execFileSync('wasm-validate', ['--enable-all', fn], { stdio: 'pipe' }); }
  catch (e) { bad(`${fn}: wasm-validate: ${(e.stderr || e.stdout || e.message).toString().trim()}`); }
}
// top-level section ids
const ids = (b) => { const r = []; let i = 8; const leb = () => { let v = 0, s = 0, x; do { x = b[i++]; v |= (x & 0x7f) << s; s += 7; } while (x & 0x80); return v; };
  while (i < b.length) { const id = b[i++]; const n = leb(); r.push(id); i += n; } return r; };
const full = fs.readFileSync('t15.wasm'), none = fs.readFileSync('t15-none.wasm');
if (!ids(full).includes(6)) bad('t15.wasm has no global section');
if (ids(none).includes(6)) bad('t15-none.wasm has a global section');
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65535, shared: true });
const g = (v) => new WebAssembly.Global({ value: 'i32', mutable: false }, v);
const imp = () => ({ m: { h: mem }, s: { p: new WebAssembly.Global({ value: 'i32', mutable: true }, 1 << 20), l: g(0), c: g(0), v: g(0), n: g(0), d: g(0), m: g(0), b: g(0), i: g(0), g: g(0) } });
let a, b2;
try {
  const mod = new WebAssembly.Module(full);
  a = new WebAssembly.Instance(mod, imp()).exports;
  b2 = new WebAssembly.Instance(mod, imp()).exports;
} catch (e) { bad(`instantiate: ${e.message}`); }
if (a) {
  const want = (what, got, exp) => { if (got !== exp) bad(`${what}: got ${got}, want ${exp}`); };
  want('f0(5)', a.f0(5), 5001);
  want('f2(3)', a.f2(3), 3);
  want('f0(7)', a.f0(7), 12002);
  want('f1(21)', a.f1(21), 42);
  want('f2(4)', a.f2(4), 7);
  want('f0(0)', a.f0(0), 12003);
  want('second instance f0(5)', b2.f0(5), 5001);
  want('second instance f2(1)', b2.f2(1), 1);
  want('first instance unchanged f2(0)', a.f2(0), 7);
}
console.log(`t15: 11 checks (per-member globals, persistent, per instance; no section without globals), ${fails} failures`);
process.exit(fails ? 1 : 0);
