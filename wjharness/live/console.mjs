// Dump matching console output of one bench run. For MONO_WASM_JIT_DUMP_IR / VERBOSE / STATS output,
// which is the only ground truth for what the emitter decided (bb structure, bails, opcode histograms).
//
// Usage: wj console <url> <lineRegex> [--timeout 180] [--max 4000]
import { launchChrome, connect, attachPage, consoleTap } from "../lib/browser.mjs";

const args = process.argv.slice(2);
const pos = args.filter((a) => !a.startsWith("--"));
const [url, pat] = pos;
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
if (!url || !pat) { console.error("usage: console.mjs <url> <lineRegex> [--timeout N] [--max N]"); process.exit(1); }
const re = new RegExp(pat);
const TIMEOUT = num("timeout", 180) * 1000;
const MAX = num("max", 4000);

const chrome = launchChrome();
const conn = await connect(await chrome.wsUrl);
const sid = await attachPage(conn);

/* Managed code runs on pthread workers, and the JIT's own printf output originates there. dotnet's
 * loader proxies worker console.* onto the main thread, so the page session sees it -- but arm the
 * workers anyway for anything that bypasses the proxy. */
const armed = new Set();
async function arm(s) {
  if (armed.has(s)) return;
  armed.add(s);
  await conn.send("Runtime.enable", {}, s);
  await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s);
  await conn.send("Runtime.runIfWaitingForDebugger", {}, s);
}
let done = false, n = 0;
conn.on((m) => {
  if (m.method === "Target.attachedToTarget") { arm(m.params.sessionId); return; }
  if (m.method !== "Runtime.consoleAPICalled") return;
  const text = (m.params.args || []).map((a) => a.value ?? a.description ?? "").join(" ");
  if (/\[bench\].*checksum/.test(text)) done = true;
  if (re.test(text) && n < MAX) { console.log(text); n++; }
});
await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
await arm(sid);
await conn.send("Page.enable", {}, sid);
await conn.send("Page.navigate", { url }, sid);

const t0 = Date.now();
while (!done && Date.now() - t0 < TIMEOUT) await new Promise((r) => setTimeout(r, 300));
console.error(`\n[${n} matching lines; bench ${done ? "finished" : "timed out"}]`);
chrome.kill();
process.exit(0);
