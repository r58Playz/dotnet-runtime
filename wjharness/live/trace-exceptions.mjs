// Fully headless CDP exception tracer for the wasm-JIT benchmark.
//
// Unlike the older cdptrace.mjs, this owns one fresh isolated Chromium and
// exits when the fixed-work benchmark finishes. It auto-attaches to every pthread worker before the
// worker starts, pauses on exceptions, and prints V8's wasm frames (including dynamic JIT modules).
//
// Usage:
//   wj trace-exceptions https://localhost:5021/ [--timeout 180]
//        [--match 'function signature mismatch|unaligned|out of bounds|unreachable']
//        [--all] [--console 'regex']
import { launchChrome, connect, attachPage, consoleTap } from "../lib/browser.mjs";

const args = process.argv.slice(2);
const url = args[0];
const flag = (name, fallback = null) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const has = (name) => args.includes(`--${name}`);
const usage = "usage: trace-exceptions.mjs <url> [--timeout N] [--match REGEX] [--all] [--console REGEX]";
if (!url || has("help") || url === "-h") {
  console.error(usage);
  process.exit(url ? 0 : 2);
}

const timeoutMs = Number(flag("timeout", "180")) * 1000;
const exceptionPattern = new RegExp(flag(
  "match",
  "RuntimeError|function signature mismatch|unaligned|out of bounds|unreachable|Could not allocate",
), "i");
const consolePattern = flag("console") ? new RegExp(flag("console"), "i") : null;
const traceAll = has("all");
const chrome = launchChrome();

const sessions = new Map();
const scripts = new Map();
const disassemblies = new Map();
const pendingPause = new Map();
let done = false;
let exceptionCount = 0;

const scriptKey = (sid, scriptId) => `${sid}:${scriptId}`;
const short = (sid) => String(sid || "?").slice(0, 8);
const label = (sid) => sessions.get(sid)?.label || "page";
const normalizeName = (name) => String(name || "").replace(/^\$+/, "") || "(anonymous)";
const description = (obj) =>
  String(obj?.description || obj?.className || obj?.value || obj?.text || "(exception)");

function scriptLabel(sid, scriptId) {
  const meta = scripts.get(scriptKey(sid, scriptId));
  if (!meta)
    return `script:${scriptId}`;
  if (meta.scriptLanguage === "WebAssembly") {
    if (!meta.url)
      return "wasm:JIT-module";
    return `wasm:${meta.url.split("/").pop() || meta.url}`;
  }
  return meta.url ? meta.url.split("/").pop() : `script:${scriptId}`;
}

function frameLine(sid, frame) {
  const loc = frame.location || {};
  const meta = scripts.get(scriptKey(sid, loc.scriptId));
  const where = meta?.scriptLanguage === "WebAssembly"
    ? `+0x${Number(loc.columnNumber || 0).toString(16)}`
    : `:${Number(loc.lineNumber || 0) + 1}:${Number(loc.columnNumber || 0) + 1}`;
  return `    at ${normalizeName(frame.functionName)} [${scriptLabel(sid, loc.scriptId)}${where}]`;
}

async function getDisassembly(conn, sid, scriptId) {
  const key = scriptKey(sid, scriptId);
  if (disassemblies.has(key))
    return disassemblies.get(key);
  const promise = (async () => {
    const first = await conn.send("Debugger.disassembleWasmModule", { scriptId }, sid);
    if (first.__error)
      return { error: first.__error.message };
    const lines = [...(first.chunk?.lines || [])];
    const offsets = [...(first.chunk?.bytecodeOffsets || [])];
    let streamId = first.streamId;
    while (streamId) {
      const next = await conn.send("Debugger.nextWasmDisassemblyChunk", { streamId }, sid);
      if (next.__error)
        return { error: next.__error.message };
      const chunk = next.chunk || {};
      if (!(chunk.lines || []).length)
        break;
      lines.push(...chunk.lines);
      offsets.push(...(chunk.bytecodeOffsets || []));
    }
    return { lines, offsets };
  })();
  disassemblies.set(key, promise);
  return promise;
}

async function printWasmWindow(conn, sid, frame) {
  const loc = frame.location || {};
  const meta = scripts.get(scriptKey(sid, loc.scriptId));
  // Runtime/AOT modules are large and already carry useful symbol names. Dynamic JIT batches are
  // compact and anonymous, so nearby instructions are most valuable there.
  if (!meta || meta.scriptLanguage !== "WebAssembly" || /^https?:/i.test(meta.url || ""))
    return;
  const disasm = await getDisassembly(conn, sid, loc.scriptId);
  if (disasm.error) {
    console.error(`    disassembly unavailable: ${disasm.error}`);
    return;
  }
  const ip = Number(loc.columnNumber || 0);
  let at = 0;
  for (let i = 0; i < disasm.offsets.length; i++) {
    if (typeof disasm.offsets[i] !== "number")
      continue;
    if (disasm.offsets[i] > ip)
      break;
    at = i;
  }
  console.error("    JIT wasm around IP:");
  for (let i = Math.max(0, at - 3); i <= Math.min(disasm.lines.length - 1, at + 3); i++) {
    const off = typeof disasm.offsets[i] === "number"
      ? `0x${disasm.offsets[i].toString(16)}`
      : "?";
    console.error(`      ${i === at ? "=>" : "  "} ${off.padStart(9)}  ${disasm.lines[i]}`);
  }
}

function remoteText(value) {
  if (!value)
    return "<missing>";
  if (Object.prototype.hasOwnProperty.call(value, "value"))
    return JSON.stringify(value.value);
  return value.unserializableValue || value.description || value.className || value.type || "?";
}

async function printWasmScopes(conn, sid, frame) {
  for (const scope of frame.scopeChain || []) {
    if (scope.type !== "local" && scope.type !== "wasm-expression-stack")
      continue;
    const objectId = scope.object?.objectId;
    if (!objectId)
      continue;
    const props = await conn.send("Runtime.getProperties", {
      objectId,
      ownProperties: true,
      generatePreview: true,
    }, sid);
    if (props.__error || !(props.result || []).length)
      continue;
    console.error(`    ${scope.type}:`);
    for (const prop of props.result.filter((item) => item?.name !== "__proto__").slice(0, 96))
      console.error(`      ${prop.name} = ${remoteText(prop.value)}`);
  }
}

function shouldTrace(text) {
  return traceAll || exceptionPattern.test(text);
}

async function handlePause(conn, message) {
  const sid = message.sessionId;
  const params = message.params || {};
  try {
    if (params.reason !== "exception" && params.reason !== "promiseRejection")
      return;
    const text = description(params.data);
    if (!shouldTrace(text))
      return;
    exceptionCount++;
    console.error(`\n>>> WASM THROW #${exceptionCount} [${short(sid)} ${label(sid)}] ${text.split("\n")[0]}`);
    const frames = params.callFrames || [];
    for (const frame of frames.slice(0, 24))
      console.error(frameLine(sid, frame));
    const wasmFrame = frames.find((frame) => {
      const loc = frame.location || {};
      return scripts.get(scriptKey(sid, loc.scriptId))?.scriptLanguage === "WebAssembly";
    });
    if (wasmFrame) {
      await printWasmScopes(conn, sid, wasmFrame);
      await printWasmWindow(conn, sid, wasmFrame);
    }
  } finally {
    await conn.send("Debugger.resume", {}, sid);
  }
}

async function setupSession(conn, sid, targetInfo = {}) {
  if (!sid || sessions.has(sid))
    return;
  sessions.set(sid, {
    label: `${targetInfo.type || "page"}:${String(targetInfo.title || targetInfo.url || "").slice(0, 70)}`,
  });
  await conn.send("Runtime.enable", {}, sid);
  await conn.send("Debugger.enable", {}, sid);
  await conn.send("Debugger.setPauseOnExceptions", { state: "all" }, sid);
  // Descendant pthread workers wait until their Debugger domain and exception policy are installed.
  await conn.send("Target.setAutoAttach", {
    autoAttach: true,
    waitForDebuggerOnStart: true,
    flatten: true,
  }, sid);
  await conn.send("Runtime.runIfWaitingForDebugger", {}, sid);
}

try {
  const conn = await connect(await chrome.wsUrl);
  const sid = await attachPage(conn);

  conn.on((message) => {
    if (message.method === "Target.attachedToTarget") {
      void setupSession(conn, message.params.sessionId, message.params.targetInfo).catch((error) => {
        console.error(`session setup failed: ${error}`);
      });
      return;
    }
    if (message.method === "Debugger.scriptParsed" || message.method === "Debugger.scriptFailedToParse") {
      scripts.set(scriptKey(message.sessionId, message.params.scriptId), message.params);
      return;
    }
    if (message.method === "Debugger.paused") {
      const prior = pendingPause.get(message.sessionId) || Promise.resolve();
      const next = prior.then(() => handlePause(conn, message)).catch(async (error) => {
        console.error(`pause handling failed [${short(message.sessionId)}]: ${error}`);
        await conn.send("Debugger.resume", {}, message.sessionId);
      });
      pendingPause.set(message.sessionId, next);
      return;
    }
    if (message.method === "Runtime.exceptionThrown") {
      const details = message.params?.exceptionDetails || {};
      const text = description(details.exception || details);
      if (shouldTrace(text))
        console.error(`\n!!! UNCAUGHT [${short(message.sessionId)} ${label(message.sessionId)}] ${text}`);
    }
  });

  consoleTap(conn, sid, (line) => {
    if (consolePattern && consolePattern.test(line))
      console.log(line);
    if (/\[bench\].*checksum/.test(line))
      done = true;
  });
  await setupSession(conn, sid, { type: "page", url });
  await conn.send("Page.enable", {}, sid);
  await conn.send("Page.navigate", { url }, sid);

  const started = Date.now();
  while (!done && Date.now() - started < timeoutMs)
    await new Promise((resolve) => setTimeout(resolve, 250));
  // Let a worker exception racing the final console message finish disassembly before teardown.
  await new Promise((resolve) => setTimeout(resolve, 1000));
  await Promise.allSettled([...pendingPause.values()]);
  if (!done)
    console.error(`trace timed out after ${timeoutMs / 1000}s`);
  console.log(`trace complete: exceptions=${exceptionCount} benchmark_done=${done}`);
  conn.close();
} finally {
  chrome.kill();
}
