// j2d/cjcapture.mjs -- save the code CheerpJ GENERATES while it runs jbox2d: every eval'd / Function()-built script (the
// default engine's JIT output is JavaScript) and every WebAssembly module (the WasmGC engine's JIT output), from the page
// and any worker, through CDP's Debugger domain.
//
//   wj cjcapture <cj<V>|cjgc<V>> <outdir> [--schedule W,S,N]
//
// NOT A TIMING RUN: an enabled debugger can move V8's wasm to debug (Liftoff) code, and nothing here is pinned. Timing
// and counters come from j2d/run.mjs; this only answers "what code did it emit". Writes <outdir>/js/<n>.js,
// <outdir>/wasm/<n>.wasm and index.tsv (n, kind, session, bytes, url, first line).
import { spawn } from "child_process";
import { mkdirSync, rmSync, writeFileSync, appendFileSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { ARMS, tool } from "../lib/paths.mjs";
import { launchChrome, connect, attachPage } from "../lib/browser.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(`--${k}`); return i >= 0 ? args[i + 1] : d; };
const [arm, outdir] = args.filter((a, i) => !a.startsWith("--") && !(i > 0 && args[i - 1].startsWith("--")));
const CJ = /^cj(gc)?(\d+)$/.exec(arm || "");
if (!CJ || !outdir) { console.error("usage: cjcapture.mjs <cj<V>|cjgc<V>> <outdir> [--schedule W,S,N]"); process.exit(1); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

rmSync(outdir, { recursive: true, force: true });
mkdirSync(join(outdir, "js"), { recursive: true }); mkdirSync(join(outdir, "wasm"), { recursive: true });
const port = 8000 + Math.floor(Math.random() * 900);
const server = spawn("node", [tool("serve-coep.mjs"), `${join(HERE, "cheerpj")}:${join(ARMS(), "cheerpj")}`, String(port)], { stdio: "ignore" });
await sleep(800);
const url = `http://localhost:${port}/index.html?v=${CJ[2]}&gc=${CJ[1] ? 1 : 0}&args=${opt("schedule", "600,200,12")}`;

const chrome = launchChrome({});
const conn = await connect(await chrome.wsUrl);
const page = await attachPage(conn);
const scripts = [];   // {sid, scriptId, url, lang, len}
let done = false, checksum = null;
const log = [];
conn.on(async (m) => {
  if (m.method === "Debugger.scriptParsed") {
    const p = m.params;
    // the loader's own files have URLs; generated code has none (eval / new Function) or is wasm://
    if (p.scriptLanguage === "WebAssembly" || !p.url || p.url.startsWith("wasm://"))
      scripts.push({ sid: m.sessionId, scriptId: p.scriptId, url: p.url, lang: p.scriptLanguage || "JavaScript", len: p.length });
  } else if (m.method === "Target.attachedToTarget") {   // workers
    const s = m.params.sessionId;
    await conn.send("Debugger.enable", { maxScriptsCacheSize: 1e9 }, s);
    await conn.send("Runtime.enable", {}, s);
    await conn.send("Runtime.runIfWaitingForDebugger", {}, s);
  } else if (m.method === "Runtime.consoleAPICalled") {
    const text = (m.params.args || []).map((a) => a.value ?? a.description ?? "").join(" ");
    log.push(text);
    const c = text.match(/\[bench\] checksum (-?\d+)/); if (c) { checksum = c[1]; done = true; }
    if (/^\[cj\] (LOAD FAILED|rejection)/.test(text)) done = true;
  }
});
await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, page);
await conn.send("Debugger.enable", { maxScriptsCacheSize: 1e9 }, page);
await conn.send("Runtime.enable", {}, page);
await conn.send("Page.enable", {}, page);
/* V8's sampling profiler, page thread only (CheerpJ runs Java there): self samples by (function, scriptId) say WHICH
 * generated function is hot, and scriptId joins it to the saved source -- the perf ledger's J_<hex> names are heap
 * addresses that change run to run on the JS engine. Started at the first batch line (after warmup + JIT). */
await conn.send("Profiler.enable", {}, page);
await conn.send("Profiler.setSamplingInterval", { interval: 200 }, page);
await conn.send("Profiler.start", {}, page);
await conn.send("Page.navigate", { url }, page);
const t0 = Date.now();
while (!done && Date.now() - t0 < 400000) await sleep(200);

const prof = (await conn.send("Profiler.stop", {}, page)).profile;
const self = new Map();   // nodeId -> samples
for (const id of prof?.samples || []) self.set(id, (self.get(id) || 0) + 1);
const byFn = new Map();
let total = 0;
for (const nd of prof?.nodes || []) {
  const k = `${nd.callFrame.functionName}\t${nd.callFrame.scriptId}\t${nd.callFrame.url}`;
  const v = self.get(nd.id) || 0; total += v;
  byFn.set(k, (byFn.get(k) || 0) + v);
}
const sid2n = new Map(scripts.map((s, i) => [s.scriptId, i]));   // n below is the index in `scripts`
writeFileSync(join(outdir, "profile.tsv"), [...byFn].sort((a, b) => b[1] - a[1]).slice(0, 300)
  .map(([k, v]) => { const [fn, sid, u] = k.split("\t"); return `${(100 * v / total).toFixed(2)}%\t${fn}\t${sid2n.has(sid) ? sid2n.get(sid) : "-"}\t${u}`; }).join("\n") + "\n");
let saved = 0;
for (const [n, s] of scripts.entries()) {   // file number = index in `scripts`, as profile.tsv's script column
  const r = await conn.send("Debugger.getScriptSource", { scriptId: s.scriptId }, s.sid);
  if (r.__error) continue;
  saved++;
  if (s.lang === "WebAssembly") {
    const b = Buffer.from(r.bytecode || "", "base64");
    writeFileSync(join(outdir, "wasm", `${n}.wasm`), b);
    appendFileSync(join(outdir, "index.tsv"), `${n}\twasm\t${s.sid === page ? "page" : s.sid}\t${b.length}\t${s.url}\t\n`);
  } else {
    const src = r.scriptSource || "";
    writeFileSync(join(outdir, "js", `${n}.js`), src);
    appendFileSync(join(outdir, "index.tsv"), `${n}\tjs\t${s.sid === page ? "page" : s.sid}\t${src.length}\t${s.url}\t${src.split("\n")[0].slice(0, 160).replace(/\t/g, " ")}\n`);
  }
}
writeFileSync(join(outdir, "console.txt"), log.join("\n") + "\n");
console.log(`${arm}: checksum ${checksum} scripts ${scripts.length} saved ${saved} (js ${scripts.filter((s) => s.lang !== "WebAssembly").length}, wasm ${scripts.filter((s) => s.lang === "WebAssembly").length})`);
try { conn.close(); } catch { }
chrome.kill(); server.kill("SIGKILL");
process.exit(checksum ? 0 : 1);
