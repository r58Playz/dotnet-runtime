// t14 (runtime half): validate + execute the EH region t14.c framed. See t14.c for what is checked.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
const uleb = (v) => { const o = []; do { let b = v & 0x7f; v >>>= 7; if (v) b |= 0x80; o.push(b); } while (v); return o; };
const str = (s) => [...uleb(s.length), ...Buffer.from(s)];
const sec = (id, body) => [id, ...uleb(body.length), ...body];
const code = [...fs.readFileSync('t14.code')];
const types = [2, 0x60, 1, 0x7f, 0, 0x60, 1, 0x7f, 1, 0x7f];   // t0 (i32)->void: the tag, the bb; t1 (i32)->i32
const imports = [3,
  ...str('x'), ...str('e'), 0x04, 0x00, 0,                       // tag 0 = x.e, as the JIT imports it
  ...str('h'), ...str('b'), 0x00, 0,                             // func 0: the bb
  ...str('h'), ...str('d'), 0x00, 1];                            // func 1: dispatch
const bytes = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, types), ...sec(2, imports), ...sec(3, [1, 1]),
  ...sec(7, [1, ...str('f'), 0x00, 2]), ...sec(10, [1, ...uleb(code.length), ...code])]);
fs.writeFileSync('t14.wasm', bytes);

let fails = 0;
const bad = (m) => { fails++; console.log('t14 FAIL: ' + m); };
try { execFileSync('wasm-validate', ['--enable-all', 't14.wasm'], { stdio: 'pipe' }); }
catch (e) { bad(`wasm-validate: ${(e.stderr || e.stdout || e.message).toString().trim()}`); }
if (!WebAssembly.validate(bytes)) bad('WebAssembly.validate rejects the module');

const tag = new WebAssembly.Tag({ parameters: ['i32'] });
let plan = {}, seen = [];
const bb = (blk) => { const a = plan.bb[blk]; if (a === undefined) return; if (a instanceof Error) throw a; throw new WebAssembly.Exception(tag, [a]); };
const dispatch = (p) => { seen.push(p); return plan.d[p] ?? -1; };
let f;
try { f = new WebAssembly.Instance(new WebAssembly.Module(bytes), { x: { e: tag }, h: { b: bb, d: dispatch } }).exports.f; }
catch (e) { bad(`instantiate: ${e.message}`); }

function run (name, p, check) {
  plan = p; seen = [];
  let r, err;
  try { r = f(0); } catch (e) { err = e; }
  const m = check(r, err);
  if (m) bad(`${name}: ${m}`);
}
if (f) {
  // caught: bb 0 throws x.e(1234) -> dispatch -> 5 -> re-dispatch, bb 5 returns
  run('catch', { bb: { 0: 1234 }, d: { 1234: 5 } }, (r, e) =>
    e ? `threw ${e}` : r !== 5 ? `returned ${r}` : seen.join() !== '1234' ? `dispatch saw ${seen}` : null);
  // caught twice: the try_table is re-entered through $outer
  run('catch twice', { bb: { 0: 11, 3: 22 }, d: { 11: 3, 22: 7 } }, (r, e) =>
    e ? `threw ${e}` : r !== 7 ? `returned ${r}` : seen.join() !== '11,22' ? `dispatch saw ${seen}` : null);
  // unclaimed: leaves carrying the same payload
  run('throw on', { bb: { 0: 777 }, d: {} }, (r, e) =>
    !(e instanceof WebAssembly.Exception) ? `got ${e ?? r}` : !e.is(tag) ? 'wrong tag' :
    e.getArg(tag, 0) !== 777 ? `payload ${e.getArg(tag, 0)}` : seen.join() !== '777' ? `dispatch saw ${seen}` : null);
  // foreign: a JS exception is not caught (dispatch never runs)
  const js = new Error('js');
  run('foreign', { bb: { 0: js }, d: {} }, (r, e) =>
    e !== js ? `got ${e ?? r}` : seen.length ? `dispatch ran for a JS exception (${seen})` : null);
}
console.log(`t14: 4 runs, ${fails} failures`);
process.exit(fails ? 1 : 0);
