// t16 (runtime half): validate + execute the image modules t16.c framed. See t16.c.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
let fails = 0, checks = 0;
const bad = (m) => { fails++; console.log('t16 FAIL: ' + m); };
for (const fn of ['t16.wasm', 't16-off.wasm']) {
  try { execFileSync('wasm-validate', ['--enable-all', fn], { stdio: 'pipe' }); }
  catch (e) { bad(`${fn}: wasm-validate: ${(e.stderr || e.stdout || e.message).toString().trim()}`); }
}
const ids = (b) => { const r = []; let i = 8; const leb = () => { let v = 0, s = 0, x; do { x = b[i++]; v |= (x & 0x7f) << s; s += 7; } while (x & 0x80); return v; };
  while (i < b.length) { const id = b[i++]; const n = leb(); r.push(id); i += n; } return r; };
const img = fs.readFileSync('t16.wasm'), off = fs.readFileSync('t16-off.wasm');
checks += 4;
if (!ids(img).includes(4)) bad('t16.wasm has no table section');
if (!ids(img).includes(9)) bad('t16.wasm has no element section');
if (ids(off).includes(4)) bad('t16-off.wasm has a table section');
if (ids(off).includes(9)) bad('t16-off.wasm has an element section');
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65535, shared: true });
const g = (v) => new WebAssembly.Global({ value: 'i32', mutable: false }, v);
const imp = () => ({ m: { h: mem }, f: { f: new WebAssembly.Table({ initial: 4, element: 'anyfunc' }) },
  s: { p: new WebAssembly.Global({ value: 'i32', mutable: true }, 1 << 20), l: g(0), c: g(0), v: g(0), n: g(0), d: g(0), m: g(0), b: g(0), i: g(0), g: g(0) } });
const want = (what, got, exp) => { checks++; if (got !== exp) bad(`${what}: got ${got}, want ${exp}`); };
const traps = (what, fn) => { checks++; try { fn(); bad(`${what}: did not trap`); } catch (e) { if (!(e instanceof WebAssembly.RuntimeError)) bad(`${what}: ${e}`); } };
try {
  const a = new WebAssembly.Instance(new WebAssembly.Module(img), imp()).exports;
  want('image f0(5, 2) -> member 1', a.f0(5, 2), 105);
  want('image f0(5, 3) -> member 2', a.f0(5, 3), 15);
  want('image f1(7)', a.f1(7), 107);
  traps('image f0(5, 0): the null entry', () => a.f0(5, 0));
  traps('image f0(5, 1): member 0, another type', () => a.f0(5, 1));
  traps('image f0(5, 4): past the table', () => a.f0(5, 4));
} catch (e) { bad(`image instantiate: ${e.message}`); }
try {
  const b = new WebAssembly.Instance(new WebAssembly.Module(off), imp()).exports;
  want('off f(5, 2): gate 0', b.f(5, 2), -1);
} catch (e) { bad(`off instantiate: ${e.message}`); }
console.log(`t16: ${checks} checks (the image's table and element section, dispatch through table 1, null / type / bound traps; gate 0 elsewhere), ${fails} failures`);
process.exit(fails ? 1 : 0);
