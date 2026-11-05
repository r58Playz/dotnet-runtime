// Snapshot the ENTIRE wasm-JIT tier as emitted bytecode, over one automated boot -> world -> in-game run.
//
// Round 56 compared RyuJIT's x86 against V8's x86 and found MethodHandle call-site stubs cost 42.7x
// (5376 B / ~1160 instructions vs 126 B / 26). What it could NOT answer is why an IL-54 stub expands to 21
// indirect call sites, because that question is about the wasm our emitter produced, not the x86 V8 made
// of it. The x86 is downstream of the thing under suspicion.
//
// WHY CDP RATHER THAN THE RUNTIME'S OWN KNOB
//
// MONO_WASM_JIT_HEX_METHOD=<substr> already prints live module bytes as hex, but it is a *filter*: you must
// name the method up front, it only covers methods matching the substring, and reaching it means editing the
// frontend's knob string and redeploying. CDP's Debugger.scriptParsed fires for every WebAssembly module the
// process ever compiles, so one run yields the whole tier -- every JIT'd method, no filter chosen in advance,
// no rebuild -- which is what makes population-level statements (medians, per-category shapes) possible at
// all instead of anecdotes about whichever method we guessed.
//
// TWO THINGS THIS RUN IS NOT GOOD FOR
//
//   1. Enabling the Debugger domain makes V8 compile wasm for debugging (Liftoff, no tier-up), so NOTHING
//      about native code size or speed may be quoted from this run. The wasm BYTECODE is upstream of that
//      choice and is unaffected -- it is the emitter's output, byte for byte. Round 56's x86 numbers stand on
//      their own capture; this one only explains them.
//   2. Timings. The collector shares the machine and pulls megabytes over the WS while the game runs.
//
// DEDUPE KEY. Each of the ~16 pthread workers parses its own copy of every JIT'd module (the wasm function
// table for dynamically-added entries is per-thread), so one session yields ~191k scriptParsed events for
// ~12k distinct modules. Hashing after the fetch would pay for all 191k.
//
// The obvious pre-fetch key, scriptParsed's `hash`, is a TRAP: for wasm V8 reports
// e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 -- the SHA-256 of the empty string -- for
// every module. Deduping on it collapsed a whole run to 1 saved module out of 191,380 while cheerfully
// reporting 191,175 "duplicates". wasmprobe.mjs measured which fields actually vary; the answer is `url`:
//
//   wasm://wasm/void System.Text.Encodings.Web.AllowedBmpCodePointsBitmap:AllowChar (char)-73935516
//
// i.e. the mono method full name plus an 8-hex content hash. That identifies the module AND names the method
// without parsing the name section. scriptParsed also carries `length`, so module SIZES are available for the
// full population without fetching anything.
//
// URL-backed modules (dotnet.native.wasm and friends) are skipped by design: they are the AOT image, not JIT
// output, and they are enormous. wasmnames.mjs's header records why that distinction matters -- shares
// computed over the mixture of "our modules" and "the AOT image" have inverted a conclusion before.
//
// Usage:
//   wj wasmtier [--outdir DIR] [--benchms MS] [--wat N] [--max N] [--headed]
//                                    [--knob k=v ...]
//
// --knob is the point of this tool for A/B work: it dumps the tier a given configuration ACTUALLY produces,
// which is a mechanism reading rather than an outcome. A change meant to remove calls or shrink bodies shows
// up here in minutes; the fps A/B that would confirm it costs hours and a ~5% resolution floor. If the tier
// does not move, do not spend the hours. Knob names are passed through as `wj.<name>` exactly like every
// other harness entry point, so MONO_*/IKVM_*/DOTNET_* env knobs work too (index.ts routes those).
import { runSession, buildUrl, seedProfile, cdpConnect, monotonic } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
import { preflight } from "../lib/preflight.mjs";
import { moduleFunctionNames } from "../lib/wasmnames.mjs";
import { createHash } from "crypto";
import { execFileSync } from "child_process";
import { mkdirSync, writeFileSync, readFileSync } from "fs";
import { join, resolve } from "path";
import { runDir, assertOutdir, serveUrl } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };

const OUTDIR = str("outdir") ? assertOutdir(str("outdir")) : runDir("wasmtier");
const MODDIR = join(OUTDIR, "mods");
const BENCHMS = num("benchms", 60000);
const WAT_PER_CAT = num("wat", 3);
const MAX_MODULES = num("max", 40000);
const DEPGRAPH = process.argv.includes("--depgraph");
const HEADLESS = !args.includes("--headed");
const KNOBS = (() => {
  const out = {};
  for (let i = 0; i < args.length; i++) {
    if (args[i] !== "--knob") continue;
    const [k, ...rest] = (args[++i] || "").split("=");
    if (k) out[k] = rest.join("=");
  }
  return out;
})();
/* Cap on concurrent getScriptSource calls. The game is the priority: the collector must not become the
 * reason a phase misses its timeout, and an unbounded fan-out over ~10k modules would do exactly that. */
const INFLIGHT = 8;
/* wasm-objdump is one process per module, so opcode stats run on a bounded sample per category rather than
 * the full set. The count actually sampled is reported -- a silent cap reads as full coverage.
 *
 * The sample is taken by STRIDE across the fetch order, not as the first N. Fetch order is boot order, so a
 * leading slice is a sample of whatever the loader happened to compile first, and it is not representative:
 * it reported "MHC call_indirect unchanged at 13" for a change that in fact moved 53% of MHC stubs from 13 to
 * 7, because the narrowed stubs were in-game ones that sort later. Stride keeps it deterministic. */
const OPCODE_SAMPLE = num("sample", 80);
const strideSample = (arr, n) => {
  if (arr.length <= n) return arr;
  const step = arr.length / n, out = [];
  for (let i = 0; i < n; i++) out.push(arr[Math.floor(i * step)]);
  return out;
};

preflight({ force: args.includes("--force") });
mkdirSync(MODDIR, { recursive: true });

/* ---------- collector ---------- */

const seenUrl = new Map();        // module url -> {length, url} for the FULL distinct population
const saved = new Map();          // sha256 -> {sha, size, names, path}
const stats = { wasmScripts: 0, urlBacked: 0, dupSkipped: 0, fetchFailed: 0, noBytecode: 0, oversize: 0 };
const sessions = new Set();
let queue = [], running = 0, collecting = true;

function saveModule(buf, session, url) {
  const sha = createHash("sha256").update(buf).digest("hex").slice(0, 16);
  if (saved.has(sha)) return;
  const path = join(MODDIR, `${sha}.wasm`);
  writeFileSync(path, buf);
  saved.set(sha, { sha, size: buf.length, names: moduleFunctionNames(buf), path, session, url });
}

/** Method name out of `wasm://wasm/<mono full name>-<8hex>`; null for unnamed modules (jiterpreter traces). */
function methodFromUrl(url) {
  const m = /^wasm:\/\/wasm\/(.+)-[0-9a-f]{8}$/.exec(url || "");
  return m && /[:.]/.test(m[1]) ? m[1] : null;
}

async function pump(conn) {
  while (running < INFLIGHT && queue.length) {
    const job = queue.shift();
    running++;
    (async () => {
      const r = await conn.send("Debugger.getScriptSource", { scriptId: job.scriptId }, job.session);
      if (r.__error) { stats.fetchFailed++; return; }
      if (typeof r.bytecode !== "string" || !r.bytecode) { stats.noBytecode++; return; }
      saveModule(Buffer.from(r.bytecode, "base64"), job.session, job.url);
    })().catch(() => { stats.fetchFailed++; }).finally(() => { running--; void pump(conn); });
  }
}

async function startCollector() {
  const conn = await cdpConnect({});
  const attachSession = async (sid) => {
    if (sessions.has(sid)) return;
    sessions.add(sid);
    // Auto-attach reaches the pthread workers, which is where most JIT'd modules are instantiated: the
    // wasm function table for dynamically-added entries is PER-THREAD, so a page-only attach would see a
    // fraction of the tier and every median computed from it would describe the main thread alone.
    await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sid);
    await conn.send("Debugger.enable", {}, sid);
  };

  conn.on((m) => {
    if (m.method === "Target.attachedToTarget") { void attachSession(m.params.sessionId); return; }
    if (m.method === "Target.targetCreated" || m.method === "Target.targetInfoChanged") {
      const ti = m.params.targetInfo;
      if (ti && ti.type === "page" && (ti.url || "").startsWith(new URL(serveUrl()).origin))
        void conn.send("Target.attachToTarget", { targetId: ti.targetId, flatten: true });
      return;
    }
    if (m.method !== "Debugger.scriptParsed") return;
    const p = m.params;
    if (p.scriptLanguage !== "WebAssembly") return;
    stats.wasmScripts++;
    if (/^https?:/.test(p.url || "")) { stats.urlBacked++; return; }
    if (p.length && p.length > 4 << 20) { stats.oversize++; return; }
    // Key on url, NOT on p.hash -- see the header: p.hash is the empty-string SHA-256 for every wasm module.
    const key = /^wasm:\/\//.test(p.url || "") ? p.url : `${m.sessionId}:${p.scriptId}`;
    if (seenUrl.has(key)) { stats.dupSkipped++; return; }
    seenUrl.set(key, { url: p.url || "", length: p.length ?? 0 });
    if (!collecting || saved.size + running + queue.length >= MAX_MODULES) return;
    queue.push({ scriptId: p.scriptId, session: m.sessionId, url: p.url || "" });
    void pump(conn);
  });

  await conn.send("Target.setDiscoverTargets", { discover: true });
  const { targetInfos } = await conn.send("Target.getTargets");
  for (const ti of targetInfos || [])
    if (ti.type === "page" && (ti.url || "").startsWith(new URL(serveUrl()).origin))
      await conn.send("Target.attachToTarget", { targetId: ti.targetId, flatten: true });
  return conn;
}

/* ---------- analysis ---------- */

const uleb = (b, p) => { let r = 0, s = 0, x; do { x = b[p++]; r |= (x & 0x7f) << s; s += 7; } while (x & 0x80); return [r >>> 0, p]; };

/** Number of function bodies, read from the code section (id 10) header. */
function functionCount(buf) {
  if (buf.length < 8) return 0;
  let p = 8;
  while (p < buf.length) {
    const id = buf[p++];
    let len; [len, p] = uleb(buf, p);
    if (id === 10) { const [n] = uleb(buf, p); return n; }
    p += len;
  }
  return 0;
}

/** Categorise from any name text (module url, or name-section names joined). */
function category(text) {
  const all = String(text || "");
  if (all.includes("__<>MHC")) return "__<>MHC stubs";
  if (all.includes("__<>DynamicBinder__")) return "__<>DynamicBinder__";
  if (!all.trim()) return "unnamed (no name section)";
  return "ordinary methods";
}

function opcodeCounts(path) {
  try {
    const d = execFileSync("wasm-objdump", ["-d", path], { encoding: "utf8", maxBuffer: 1 << 28 });
    const count = (rx) => (d.match(rx) || []).length;
    return {
      call_indirect: count(/\bcall_indirect\b/g),
      call: count(/\bcall\b(?!_)/g),
      loads: count(/\b(i32|i64|f32|f64)\.load\b/g),
      stores: count(/\b(i32|i64|f32|f64)\.store\b/g),
    };
  } catch { return null; }
}

const median = (v) => { if (!v.length) return 0; const s = [...v].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };

/* ---------- run ---------- */

const profile = join(OUTDIR, "profile");
const logPath = join(OUTDIR, "game.log");
seedProfile(profile);

let conn = null;
startCollector().then((c) => { conn = c; console.log("[wasmtier] collector attached"); })
  .catch((e) => console.log(`[wasmtier] collector FAILED to attach: ${e.message}`));

const t0 = monotonic();
const r = await runSession({
  url: buildUrl({ knobs: KNOBS }),
  profile,
  logPath,
  headless: HEADLESS,
  benchMs: BENCHMS,
  /* Ask for the printf dumps too. A tier dump answers "what call FORM did each site get" and the
   * counters answer "why did the rest not convert"; before R201 those were two separate ~7-minute runs
   * of the same config, and pairing a reach number with a census taken on a DIFFERENT run is exactly
   * the confound CLAUDE.md warns about for cross-binary readings. Costs nothing here: the dumps are
   * printfs into game.log, which this tool already captures, and the tier walk is CDP-side. */
  dumps: true,
  hooks: {
    onMarker: async (name) => {
      if (name === "title" || name === "inworld")
        console.log(`[wasmtier] ${name}: ${saved.size} unique modules, ${stats.wasmScripts} wasm scripts seen`);
      /*
       * DUMP AT BOTH inworld AND benchEnd, and let depcheck union the rows.
       *
       * A single snapshot at inworld is a MEASUREMENT BUG, not a conservative choice: the tier walk
       * collects modules for the whole run, so every method registered after the snapshot has no graph
       * entry and its callees necessarily read as "declared by nobody". Measured on the first sizing:
       * 23,578 descriptors in the graph against 32,888 modules fetched -- a ~9,000-module blind spot,
       * against 8,284 reported violations. That is the confound accounting for most of the signal.
       *
       * Two snapshots cost a few seconds and make a slot count as declared if ANY snapshot names it, so
       * a dependency that simply had not been registered yet stops looking like a missing one.
       */
      if (!(DEPGRAPH && (name === "inworld" || name === "benchEnd"))) return;
    /*
     * DUMP THE DEPENDENCY GRAPH AT `inworld`, IN THE SAME RUN AS THE TIER, for depcheck.py.
     *
     * depcheck compares the f-slots BAKED into each module against the f-slots each descriptor
     * DECLARES; an undeclared one is unadmittable by construction, so the first worker to reach it finds
     * the prefilled placeholder and traps with `function signature mismatch` (R285). F-slot numbers are
     * PER-RUN, so the graph and the dump have to come from one run or the comparison is of two unrelated
     * numberings -- depcheck refuses below a 60% ownership ratio rather than reporting that as clean.
     *
     * AT `inworld`, NOT AT onEnd. The first version dumped from the onEnd hook and produced
     * WJ_GRAPH_BEGIN/END with ZERO rows: by then the run has quit the game and the runtime is torn down,
     * so the registry reads empty -- the same log shows `JIT_ADMIT registered:0` beside it. The tier is
     * fully populated and the runtime is alive here.
     *
     * The registry size is checked FIRST and a zero refuses loudly. Two silent failures already cost a
     * 7-minute run each: Runtime.evaluate RESOLVES on a thrown expression (it does not reject, so a
     * try/catch reports success for an expression that did nothing), and an empty dump looks exactly
     * like a clean one downstream.
     */
      try {
        const psid = await attachPage(conn);
        const ev = async (expr, byValue) => {
          const r = await conn.send("Runtime.evaluate",
            { expression: expr, returnByValue: !!byValue }, psid);
          if (r?.exceptionDetails)
            throw new Error(`${expr.slice(0, 48)}... threw: ${r.exceptionDetails.text || ""} `
              + `${r.exceptionDetails.exception?.description || ""}`);
          return r?.result?.value;
        };
        if (await ev("typeof getDotnetRuntime(0).Module._mono_wasm_jit_dump_dep_graph", true) !== "function")
          throw new Error("export _mono_wasm_jit_dump_dep_graph not reachable on the page");
        /* liveness field 2 is wj_reg_n -- the registry high-water. Zero means the graph would be empty. */
        const nreg = await ev("getDotnetRuntime(0).Module._mono_wasm_jit_liveness(2)", true);
        if (!nreg)
          throw new Error(`registry is empty (wj_reg_n=${nreg}) -- a dump here would be BEGIN/END with no rows`);
        await ev("getDotnetRuntime(0).Module._mono_wasm_jit_dump_dep_graph()");
        await new Promise((r) => setTimeout(r, 4000));   /* let ~N printf lines reach the log FILE */
        console.log(`[wasmtier] dep graph dumped at ${name}: ${nreg} descriptors (WJ_GRAPH_BEGIN..END)`);
      } catch (e) { console.log(`[wasmtier] !! dep-graph dump FAILED (non-fatal): ${e.message}`); }
    },

  },
});
collecting = false;
// Let in-flight fetches land rather than truncating the tail of the tier.
for (let i = 0; i < 100 && (running > 0 || queue.length); i++) await new Promise((s) => setTimeout(s, 200));
try { conn?.close(); } catch { }

console.log(`\n================ wasm JIT tier snapshot ================`);
console.log(`  run: ok=${r.ok} wall=${(monotonic() - t0).toFixed(1)}s  faults=${r.faults?.length ?? 0}`);
console.log(`  wasm scripts parsed=${stats.wasmScripts}  url-backed(AOT, skipped)=${stats.urlBacked}` +
  `  dup-skipped=${stats.dupSkipped}  oversize=${stats.oversize}`);
console.log(`  distinct modules seen=${seenUrl.size}  fetched=${saved.size}  fetch-failed=${stats.fetchFailed}  no-bytecode=${stats.noBytecode}`);
if (saved.size + stats.fetchFailed < seenUrl.size)
  console.log(`  note: ${seenUrl.size - saved.size - stats.fetchFailed} distinct modules were seen but not fetched` +
    ` (--max ${MAX_MODULES}${collecting ? "" : " / run ended"}); SIZE stats below still cover all of them, opcode stats do not`);

// Size stats come from scriptParsed's `length` and therefore cover every distinct module, fetched or not.
const popCats = new Map();
for (const { url, length } of seenUrl.values()) {
  const c = category(methodFromUrl(url) || "");
  if (!popCats.has(c)) popCats.set(c, []);
  popCats.get(c).push(length);
}

const rows = [...saved.values()];
for (const m of rows) m.funcs = functionCount(readFileSync(m.path));
const fetchedCats = new Map();
for (const m of rows) {
  const c = category(methodFromUrl(m.url) || m.names.join(" "));
  if (!fetchedCats.has(c)) fetchedCats.set(c, []);
  fetchedCats.get(c).push(m);
}

console.log(`\n  per category -- module bytes are EMITTED WASM, i.e. our emitter's own output:`);
for (const [c, lens] of [...popCats.entries()].sort((a, b) => b[1].length - a[1].length)) {
  const v = fetchedCats.get(c) || [];
  const picked = strideSample(v, OPCODE_SAMPLE);
  const sample = picked.map((m) => opcodeCounts(m.path)).filter(Boolean);
  const s = (k) => median(sample.map((x) => x[k]));
  console.log(`    ${c.padEnd(28)} n=${String(lens.length).padEnd(6)} median wasm=${String(median(lens)).padEnd(7)}` +
    ` p90=${String(lens.sort((a, b) => a - b)[Math.floor(lens.length * 0.9)] ?? 0).padEnd(7)}` +
    (sample.length
      ? ` | fetched ${v.length}, opcodes sampled ${sample.length}: funcs=${median(picked.map((m) => m.funcs))}` +
        ` call_indirect=${s("call_indirect")} call=${s("call")} loads=${s("loads")} stores=${s("stores")}`
      : ` | fetched ${v.length}, no opcode sample`));
}
const cats = fetchedCats;

// A readable .wat per category, for the actual question: what are those indirect call sites?
let watted = 0;
for (const [c, v] of cats) {
  for (const m of v.slice(0, WAT_PER_CAT)) {
    const out = join(OUTDIR, `${c.replace(/[^a-z0-9]+/gi, "_")}-${m.sha}.wat`);
    try { execFileSync("wasm2wat", ["--enable-all", "-o", out, m.path]); watted++; } catch { }
  }
}
console.log(`\n  wrote ${watted} .wat files + ${saved.size} .wasm under ${OUTDIR}`);
writeFileSync(join(OUTDIR, "index.json"), JSON.stringify({
  /* Record WHICH configuration produced this tier. Without it two dumps are indistinguishable, and the
   * whole point of --knob is comparing them. (mcsr/deploy.sh exists because a measurement matrix once ran
   * on a stale runtime; the same class of mistake is cheaper to make here.) */
  stats, benchMs: BENCHMS, ok: r.ok, knobs: KNOBS,
  modules: rows.map((m) => ({ sha: m.sha, size: m.size, funcs: m.funcs, url: m.url, names: m.names.slice(0, 4) })),
 distinctSeen: [...seenUrl.values()],
}, null, 1));
