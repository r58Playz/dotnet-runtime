// Catch `function signature mismatch` IN THE ACT and read the function table at the faulting slot.
//
// R160-R164 chased this by reasoning about admission order and were wrong four times running. The trap
// itself carries the answer: a `call_indirect` pushes its table index onto the wasm expression stack, and
// CDP exposes that stack on the paused frame. So pause on the throw, read the index, and ask the table what
// is actually in that slot -- plus, decisively, whether THIS worker ever installed it.
//
// `Module.__wjSlotFn` is the per-worker Map of slots this worker wrote (mini-wasm.c instantiate paths). If
// the faulting index is absent from it, the caller went live without this worker installing the target,
// which is the whole question. Arity discriminates too: 2 = a real entry thunk, 4 = the jiterpreter's
// mono_jiterp_placeholder_jit_call prefill.
//
// Derived from the older cdptrace.mjs (browser-WS connect, flat auto-attach to pthread workers,
// wasm frame handling). Differences: filters to ONE exception description so pause volume stays near zero,
// and probes the table instead of disassembling.
//
// Run:  wj sigtrap        (leave running, then drive the app on CDP_PORT)
//   env: CDP_PORT (default 9333 -- mcdrive's MC_CDP_PORT), CDP_LOG, SIGTRAP_MATCH (default "signature mismatch")
import { appendFileSync, writeFileSync } from "fs";
import { join } from "path";
import { runDir } from "../lib/paths.mjs";

const PORT  = process.env.CDP_PORT || "9333";
const LOG   = process.env.CDP_LOG  || join(runDir("sigtrap"), "sigtrap.log");
const MATCH = process.env.SIGTRAP_MATCH || "signature mismatch";
const PAUSE_STATE = process.env.SIGTRAP_PAUSE || "all";
writeFileSync(LOG, `=== sigtrap ${new Date().toISOString()} port ${PORT} match=${JSON.stringify(MATCH)} ===\n`);
const log = (s) => { process.stdout.write(s + "\n"); appendFileSync(LOG, s + "\n"); };

async function wsUrl() {
  for (let i = 0; i < 600; i++) {
    try {
      const j = await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json();
      if (j?.webSocketDebuggerUrl) return j.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`no CDP on ${PORT} after 5 min`);
}

// Resolve the runtime the way it is actually reachable from a worker's devtools context:
// getDotnetRuntime(0).Module.wasmExports["__indirect_function_table"]. Bare `wasmTable` is a module-scope
// local of the emscripten glue and does NOT resolve in an evaluated expression, which is why the obvious
// probe returns nothing. Fallbacks kept in order of reliability.
const PROBE = (idxs) => `(() => {
  let M = null;
  try { if (typeof getDotnetRuntime === "function") M = getDotnetRuntime(0) && getDotnetRuntime(0).Module; } catch (e) {}
  if (!M && typeof Module !== "undefined") M = Module;
  let T = null;
  try { T = M && M.wasmExports && M.wasmExports["__indirect_function_table"]; } catch (e) {}
  if (!T) { try { T = M && M.asm && M.asm["__indirect_function_table"]; } catch (e) {} }
  if (!T && typeof wasmTable !== "undefined") T = wasmTable;
  if (!T) return JSON.stringify({ error: "no table; M=" + (!!M) });
  const out = [];
  for (const i of ${JSON.stringify(idxs)}) {
    let arity = -1, name = "", err = null;
    try { const f = T.get(i); if (f) { arity = f.length | 0; name = f.name || ""; } } catch (e) { err = String(e); }
    out.push({ i, arity, name,
      installedHere: !!(M && M.__wjSlotFn && M.__wjSlotFn.has(i)),
      liveHere: (M && M._mono_wasm_jit_slot_live) ? !!M._mono_wasm_jit_slot_live(i) : null,
      err });
  }
  return JSON.stringify({ tableLen: T.length, slots: out });
})()`;

async function onPause(m) {
  const sid = m.sessionId;
  const d = m.params.data || {};
  // A wasm trap arrives as a RemoteObject whose `description` is NOT the message -- measured: a boot with
  // 4 `function signature mismatch` traps produced 12 pauses, every one described only as "Exception".
  // Filtering on that field silently discarded exactly the exceptions being hunted. Resolve the object
  // instead. Affordable because blackboxing the JS glue cut pause volume to ~12 per boot.
  let desc = String(d.description || d.className || "");
  if (d.objectId) {
    const r = await send("Runtime.callFunctionOn", {
      objectId: d.objectId, returnByValue: true,
      functionDeclaration: "function(){ try { return (this && (this.message || this.stack)) || String(this); } catch (e) { return String(this); } }"
    }, sid);
    const v = r && r.result && r.result.value;
    if (typeof v === "string" && v) desc = v;
  }
  if (m.params.reason !== "exception" && m.params.reason !== "promiseRejection") { await send("Debugger.resume", {}, sid); return; }
  pauses++;
  { const k = (m.params.reason || "?") + ":" + desc.split("\n")[0].slice(0, 60); seenDesc.set(k, (seenDesc.get(k) || 0) + 1); }
  if (!desc.includes(MATCH)) {
    if (desc.includes("RuntimeError")) log(`    [near-miss] ${m.params.reason} :: ${desc.split("\n")[0].slice(0, 120)}`);
    skipped++; await send("Debugger.resume", {}, sid); return;
  }

  hits++;
  const frames = m.params.callFrames || [];
  log(`\n>>> HIT #${hits} [${short(sid)} ${label.get(sid) || "?"}] ${desc.split("\n")[0]}   (${skipped} other throws skipped)`);
  skipped = 0;
  const sm = meta.get(sid) || new Map();
  for (const f of frames.slice(0, 10)) {
    const s = sm.get(f.location.scriptId) || {};
    log(`    ${f.functionName || "(anon)"}  ${s.url || s.lang || "?"}  +0x${(f.location.columnNumber || 0).toString(16)}`);
  }
  const wasmFrame = frames.find((f) => (sm.get(f.location.scriptId) || {}).lang === "WebAssembly");
  if (!wasmFrame) { log("    (no wasm frame on the stack)"); await send("Debugger.resume", {}, sid); return; }

  const idxs = await candidateIndices(sid, wasmFrame);
  log(`    candidate table indices from the paused wasm frame: ${idxs.join(", ") || "(none)"}`);
  if (idxs.length) {
    const r = await send("Runtime.evaluate", { expression: PROBE(idxs), returnByValue: true }, sid);
    const v = r?.result?.value;
    if (v) {
      const parsed = JSON.parse(v);
      if (parsed.error) { log(`    probe: ${parsed.error}`); }
      else log(`    table length = ${parsed.tableLen}`);
      for (const s of (parsed.slots || []))
        log(`      slot ${s.i}: arity=${s.arity}${s.arity === 4 ? " <-- JITERPRETER PLACEHOLDER" : s.arity === 2 ? " (entry thunk)" : ""}  installedByThisWorker=${s.installedHere}  live=${s.liveHere}  name=${JSON.stringify(s.name)}${s.err ? "  err=" + s.err : ""}`);
    } else log(`    probe failed: ${JSON.stringify(r).slice(0, 300)}`);
  }
  await send("Debugger.resume", {}, sid);
}

let ws = null, id = 0;
let hits = 0, skipped = 0, pauses = 0;
const seenDesc = new Map();
setInterval(() => {
  if (!pauses) { log(`[tick] pauses=0 sessions=${label.size}`); return; }
  const top = [...seenDesc.entries()].sort((a,b)=>b[1]-a[1]).slice(0,6).map(([d,c])=>`${c}x ${d}`).join(" | ");
  log(`[tick] pauses=${pauses} hits=${hits} sessions=${label.size} :: ${top}`);
}, 15000);
const short = (x) => (x || "").slice(0, 8);
const pending = new Map();
const attached = new Set();
const meta = new Map();
const label = new Map();

function send(method, params = {}, sessionId) {
  return new Promise((res) => {
    if (!ws || ws.readyState !== 1) return res({ __error: "socket down" });
    const mid = ++id; pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
}

function handle(m) {
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.error ? { __error: m.error } : (m.result || {})); pending.delete(m.id); return; }
  if (m.method === "Target.targetCreated" || m.method === "Target.targetInfoChanged") {
    const t = m.params.targetInfo;
    // Browser-level autoAttach does not reliably cover already-open pages, so attach explicitly. Without
    // this the tracer connects, reports "attached", and silently sees nothing -- which is how its first
    // run produced a meaningless all-clear.
    if (t && (t.type === "page" || t.type === "worker") && !attached.has(t.targetId)) {
      attached.add(t.targetId);
      send("Target.attachToTarget", { targetId: t.targetId, flatten: true });
    }
    return;
  }
  if (m.method === "Target.attachedToTarget") {
    const { sessionId, targetInfo } = m.params;
    label.set(sessionId, targetInfo.type);
    meta.set(sessionId, new Map());
    log(`+ session ${short(sessionId)} ${targetInfo.type} ${(targetInfo.url || "").slice(0, 60)}`);
    send("Runtime.enable", {}, sessionId);
    send("Debugger.enable", {}, sessionId);
    // "uncaught", not "all". This app throws constantly on its unwind path (emscripten's `throw "unwind"`),
    // and pausing + resuming on every one across ~100 worker sessions perturbs the workload enough to
    // cause the very hangs we are trying to tell apart from the trap. The trap surfaces as an uncaught
    // RuntimeError / unhandled rejection, so "uncaught" sees it at a fraction of the cost. Fallback if
    // nothing lands: "all" PLUS blackboxing the unwind helpers, the way cdptrace.mjs does.
    // "all", because the trap is CAUGHT and re-reported by the worker's own handler (WORKER_TRAP_STACK),
    // so it is not "uncaught" at the throw site -- measured: a boot with 4 signature mismatches produced 0
    // hits under "uncaught". The cost of "all" is emscripten's constant `throw "unwind"`, so blackbox the
    // JS glue: a wasm trap throws in WASM and still pauses, while the JS unwind helper no longer does.
    // NO file-level blackboxing. Blackboxing dotnet.native/runtime .js suppressed the pause for the very
    // trap being hunted: `function signature mismatch` propagates out through wasm_jit_ethunk_cb, which is
    // CALLED FROM that glue, so a whole-file pattern swallows it. MEASURED: a boot with 4 mismatches gave
    // 36 pauses, all WebAssembly.Exception, and 0 RuntimeError. cdptrace.mjs blackboxes by content RANGE
    // (the `throw "unwind"` site) rather than by file, which is the correct granularity if volume needs
    // cutting again -- set SIGTRAP_BLACKBOX=1 to restore the coarse version for comparison.
    if (process.env.SIGTRAP_BLACKBOX === "1")
      send("Debugger.setBlackboxPatterns", { patterns: ["/dotnet\\.runtime\\..*\\.js$", "/dotnet\\.native\\..*\\.js$"] }, sessionId);
    send("Debugger.setPauseOnExceptions", { state: PAUSE_STATE }, sessionId);
    send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId);
    return;
  }
  if (m.method === "Debugger.scriptParsed") {
    const sm = meta.get(m.sessionId); if (sm) sm.set(m.params.scriptId, { url: m.params.url, lang: m.params.scriptLanguage });
    return;
  }
  if (m.method === "Debugger.paused") {
    void onPause(m).catch(async (e) => { log(`pause handler failed: ${e}`); await send("Debugger.resume", {}, m.sessionId); });
    return;
  }
}

// The harness launches a FRESH browser per run, so the tracer has to outlive the socket it started on.
async function connect() {
  const url = await wsUrl();
  const sock = new WebSocket(url);
  sock.addEventListener("open", () => {
    ws = sock; attached.clear(); meta.clear(); label.clear(); pending.clear();
    send("Target.setDiscoverTargets", { discover: true });
    send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
    log("connected — waiting for targets");
  });
  sock.addEventListener("message", (ev) => { let m; try { m = JSON.parse(ev.data); } catch { return; } handle(m); });
  sock.addEventListener("error", () => {});
  sock.addEventListener("close", () => { log("socket closed — reconnecting"); setTimeout(() => { void connect().catch(() => {}); }, 500); });
}

await connect();
log("sigtrap running — Ctrl-C to stop");
