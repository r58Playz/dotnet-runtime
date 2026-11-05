// runjs.mjs <file.js> <out.json> [--after-major ADDRS.json] [--ms N] [args-json...]
//   Evaluates (fn)(...args) from file.js on the live harness page and writes the returned JSON string. With
//   --after-major, first waits for mono_gc_stats.major_gc_count to change and sweep_state to read SWEPT (+2 s).
import { readFileSync, writeFileSync } from "fs";
import { cdpConnect } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
const argv = process.argv.slice(2);
const take = (n) => { const i = argv.indexOf(`--${n}`); if (i < 0) return null; const v = argv[i + 1]; argv.splice(i, 2); return v; };
const AM = take("after-major"), MS = Number(take("ms") || 120000);
const [file, out, ...a] = argv;
const SRC = readFileSync(file, "utf8").trim().replace(/;$/, "");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const conn = await cdpConnect({ tries: 5 }); const sid = await attachPage(conn);
const ev = async (expression, ms) => { const r = await Promise.race([conn.send("Runtime.evaluate", { expression, returnByValue: true }, sid), sleep(ms).then(() => ({ timedOut: 1 }))]);
  if (r.timedOut || r.exceptionDetails || r.__error) { console.log("failed", r.timedOut ? "timeout" : (r.exceptionDetails?.exception?.description || JSON.stringify(r.__error))); return null; } return r.result.value; };
if (AM) {
  const A = JSON.parse(readFileSync(AM, "utf8"));
  const peek = `(()=>{const u=new Uint32Array(globalThis.wasm.Module.wasmMemory.buffer);return JSON.stringify([u[${(A.mono_gc_stats + 4) >>> 2}],u[${A.sweep_state >>> 2}]])})()`;
  const m0 = JSON.parse(await ev(peek, 10000))[0];
  for (let k = 0; k < 600; k++) { await sleep(500); const p = await ev(peek, 10000); if (!p) continue; const [m, sw] = JSON.parse(p); if (m !== m0 && sw === 0) { console.log("major", m); break; } }
  await sleep(2000);
}
const v = await ev(`(${SRC})(${a.join(",")})`, MS);
if (v == null) process.exit(1);
writeFileSync(out, v); console.log("ok", out, new Date().toISOString()); process.exit(0);
