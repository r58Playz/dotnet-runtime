// memcensus.mjs --log <run.log> --out <dir> [--at titleReady,inworld,benchStart,benchEnd] [--every S]
//               [--wasm dotnet.native.<hash>.wasm --addrs <addrs.py output>]   (both are per BUILD)
//   Attaches to a RUNNING harness browser (worldwait/mcperf) over CDP, waits for the run's log markers, and at each
//   one evaluates census.js in the page: a linear-memory census from the runtime's own counters plus a bounded
//   dlmalloc chunk walk. Writes <out>/census-<marker>.json. Read-only; costs ~1-3 s of main thread per census.
//
// Addresses are from loader/obj/dotnet.native.debug.wasm for the build named in EXPECT_WASM; the script refuses to
// read a page that loaded a different dotnet.native.*.wasm (a stale address table reads garbage, not an error).
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { cdpConnect } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
const LOG = opt("log"), OUT = opt("out", "memcensus-out");
const AT = opt("at", "titleReady,inworld,benchStart,benchEnd").split(",");
const EVERY = Number(opt("every", 0));
// R500 (wgen262): periodic censuses start at --every-from (default benchStart) and end after --every-for seconds
const EVERY_FROM = opt("every-from", "benchStart"), EVERY_FOR = Number(opt("every-for", 0));
const EXPECT_WASM = opt("wasm", "dotnet.native.syq4t2o2ya.wasm");
if (!LOG) { console.error("--log required"); process.exit(2); }
mkdirSync(OUT, { recursive: true });

const ADDR = {
  sbrk_val: 0x1b71c68, allocation_count: 0x1f70430, total_allocation_count: 0x1f7046c, mempool_total: 0x1f92570,
  sgen_los_memory_usage: 0x1f785f4, sgen_los_memory_usage_total: 0x1f785f8, los_num_sections: 0x1f78694,
  ms_block_size: 0x1f78698, num_major_sections: 0x1f78798, num_empty_blocks: 0x1f787a0, total_alloc: 0x1f78818,
  total_alloc_max: 0x1f7881c, sgen_nursery_size: 0x1f7882c, mono_default_stacksize: 0x1f92890, mono_gm: 0x1f92c0c,
  em_gm: 0x22078e4, em_mparams: 0x22078cc, main_pthread: 0x22035a0,
};
if (opt("addrs")) Object.assign(ADDR, JSON.parse(readFileSync(opt("addrs"), "utf8")));
const CENSUS = readFileSync(join(dirname(fileURLToPath(import.meta.url)), "census.js"), "utf8");
const EXPR = `(${CENSUS.trim().replace(/;$/, "")})(${JSON.stringify(ADDR)})`;

const MARK = { titleReady: /FAILED_AUTH/, inworld: /StateOutput State: inworld/, benchStart: /\[mcw\] START/, benchEnd: /\[mcw\] END/,
  // wgen262.mjs (26.2 world creation): RRLS's title marker, the integrated server's spawn-area line
  title262: /Overlay is removed!/, spawn262: /Time elapsed: \d+ ms/ };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

async function evalPage(conn, sid, expression, ms = 90000) {
  const r = await Promise.race([
    conn.send("Runtime.evaluate", { expression, returnByValue: true }, sid),
    sleep(ms).then(() => ({ timedOut: true })),
  ]);
  if (r.timedOut) return { err: "timeout" };
  if (r.__error) return { err: JSON.stringify(r.__error) };
  if (r.exceptionDetails) return { err: r.exceptionDetails.exception?.description || "exception" };
  return { value: r.result?.value };
}

let conn = null, sid = null;
async function ensureConn() {
  if (conn && sid) return true;
  try { conn = await cdpConnect({ tries: 600 }); sid = await attachPage(conn); log("attached", sid); return true; }
  catch (e) { log("attach failed:", e.message); conn = sid = null; return false; }
}

async function census(tag) {
  if (!await ensureConn()) return;
  const w = await evalPage(conn, sid, `JSON.stringify(performance.getEntriesByType('resource').map(e => e.name).filter(n => /dotnet\\.native\\.[a-z0-9]+\\.wasm/.test(n)))`, 15000);
  const names = w.value ? JSON.parse(w.value) : [];
  if (!names.some((n) => n.endsWith(EXPECT_WASM))) { log(`REFUSING ${tag}: page loaded ${JSON.stringify(names)} not ${EXPECT_WASM}`); return; }
  const t = Date.now();
  const r = await evalPage(conn, sid, EXPR);
  if (r.err) { log(`${tag}: ${r.err}`); writeFileSync(join(OUT, `census-${tag}.err`), r.err); return; }
  const o = JSON.parse(r.value); o.tag = tag; o.wallMs = Date.now() - t; o.at = new Date().toISOString();
  writeFileSync(join(OUT, `census-${tag}.json`), JSON.stringify(o));
  const MB = (x) => (x / 1048576).toFixed(1);
  log(`${tag}: mem ${MB(o.memBytes)} sbrk ${MB(o.sbrk)} dl.footprint ${MB(o.dl.footprint)} walk ${o.walk.ms}ms chunks ${o.walk.chunks}`
    + ` insane ${o.walk.insane} FREE ${MB(o.walk.cls.FREE?.[1] || 0)} sgen ${MB(o.acct.slice(3, 11).reduce((a, b) => a + b, 0))} mempool ${MB(o.mempool)}`
    + ` threads ${o.threads.n}/${MB(o.threads.mapBytes)} magic ${o.dl.magic === o.dl.mparams_magic ? "ok" : "MISMATCH"}`);
}

/* wait for markers in the log, in order */
const seen = new Set();
let lastEvery = 0, sawStart = false, startAt = 0;
for (;;) {
  let text = "";
  try { text = existsSync(LOG) ? readFileSync(LOG, "utf8") : ""; } catch { }
  for (const m of AT) {
    if (seen.has(m) || !MARK[m]?.test(text)) continue;
    seen.add(m); log("marker", m);
    await census(m);
    if (m === EVERY_FROM) { sawStart = true; startAt = Date.now(); }
  }
  const everyOver = EVERY_FOR ? Date.now() - startAt > EVERY_FOR * 1000 : seen.has("benchEnd");
  if (EVERY && sawStart && !everyOver && Date.now() - lastEvery > EVERY * 1000) {
    if (lastEvery) await census(`t${Math.round((Date.now() - startAt) / 1000)}`);
    lastEvery = Date.now();
  }
  if (AT.every((m) => seen.has(m)) && (!EVERY || !sawStart || everyOver)) break;
  await sleep(1000);
}
log("done");
process.exit(0);
