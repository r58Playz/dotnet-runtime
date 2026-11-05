// wgen262.mjs [--timeout-s N] [--after-spawn-s N] [--title-tabs N] [--knob K=V]... [--jprop K=V]... [--headful] [--seed DIR]
//   -- reproduce a fault in 26.2's INITIAL world generation, with the faulting worker's full stack.
//
// The MCSR-era drivers (worldwait/mcbench/runSession) cannot reach this: their markers are StateOutput's and their
// menu path is 1.16.1's. This one boots mcsr/seed-262 (seedbuild.mjs from an OPFS tar of the 26.2 + Fabric pack),
// deletes its "New World" so Singleplayer opens Create World directly, presses the app's Play button, and drives
// Minecraft by keyboard: Tab x N + Enter (Singleplayer), then Enter (Create New World). A screenshot follows every
// key, so a miscounted Tab is visible instead of looking like a hang.
//
// WHY CDP ON EVERY WORKER: a worker that aborts surfaces in the stderr log as one line ("worker sent an error!"),
// and dotnet proxies worker console.* to the page, where the stack is the page's. The faulting frames only exist
// in the worker's own isolate, so every worker is auto-attached and its Runtime.exceptionThrown and error-level
// console events are written with their call frames to cdp-events.jsonl. Sends to worker sessions are never
// awaited: a pthread busy in wasm does not answer (CLAUDE.md).
//
// Run it in a transient systemd service (CLAUDE.md, "Long-running work"); it writes <out>/done when finished.
import { seedProfile, reapProfile, buildUrl, launchGame, cdpConnect, sleep, TRAP_RX, resolveSeed, driveLauncher } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
import { preflight } from "../lib/preflight.mjs";
import { appendFileSync, mkdirSync, readFileSync, writeFileSync } from "fs";
import { join, resolve } from "path";
import { runDir, assertOutdir } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
/* a registry name (lib/seeds.mjs) or a 26.2 profile directory */
const SEEDDEF = (() => { const d = resolveSeed(str("seed", "262b")); return { ...d, version: d.version || "26.2", fabric: d.fabric || "0.19.5" }; })();
const SEED = SEEDDEF.dir;
const TIMEOUT_S = Number(str("timeout-s", 1200));
const TITLE_TABS = Number(str("title-tabs", 1));
const AFTER_SPAWN_S = Number(str("after-spawn-s", 90));
/* R507: --title-only S ends the run S seconds after the title screen (verdict title-ok, or fault): a boot test for the
 * intermittent cast fault, which strikes during bootstrap or the title's resource reload. */
const TITLE_ONLY_S = args.includes("--title-only") ? Number(str("title-only", 15)) : -1;
// a normal boot reaches the title in 115-160 s; the free quarantine's boot hang (R500) never does
const TITLE_TIMEOUT_S = Number(str("title-timeout-s", 600));
const HEADFUL = args.includes("--headful");
// only what ends a run: FAIL_RX also matches cast warnings a 149-mod boot can log harmlessly
const FATAL_RX = /Assertion at|assert_not_reached|forcing abort|Game crashed!|worker sent an error!/;
/* wasm32's 4 GB ends a run as SGen failing to map a block, not as a trap: batch2/r1 sat at fps 0 with wasm 4096 MB for
 * two minutes and was called clean. Its own verdict, so an OOM is not counted as a corruption fault or as clean. */
const OOM_RX = /Garbage collector could not allocate|Cannot enlarge memory|OutOfMemoryError|Out of memory/;
const fatal = (t) => FATAL_RX.test(t) || TRAP_RX.test(t);
const knobs = {};
args.forEach((a, i) => { if (a === "--knob") { const [k, ...v] = args[i + 1].split("="); knobs[k] = v.join("="); } });
/* Java system properties, as index.ts's ?jprop.<name>=<value>. Sodium 0.9 refuses this LWJGL (3.4.2-snapshot, it wants
 * 3.4.1) and exits before the title screen unless its issue-2561 check is off -- the property the app's users pass. */
const jprops = { "sodium.checks.issue2561": "false" };
args.forEach((a, i) => { if (a === "--jprop") { const [k, ...v] = args[i + 1].split("="); jprops[k] = v.join("="); } });

const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const OUT = str("out", null) ? assertOutdir(str("out")) : runDir(`wgen262-${stamp}`);
mkdirSync(join(OUT, "shots"), { recursive: true });
const LOG = join(OUT, "chrome.log");
const EVENTS = join(OUT, "cdp-events.jsonl");
const say = (s) => { const line = `[${((Date.now() - T0) / 1000).toFixed(1)}s] ${s}`; console.log(line); appendFileSync(join(OUT, "driver.log"), line + "\n"); };
const T0 = Date.now();

/* --force: a correctness run, not a measurement -- another process's CPU only slows it. preflight's refusals become
 * warnings then, all but memory, which this keeps itself: a world creation peaks near 4 GB of wasm. */
if (args.includes("--force")) {
  const avail = Number((readFileSync("/proc/meminfo", "utf8").match(/MemAvailable:\s+(\d+)/) || [])[1] || 0) / 1048576;
  if (avail < 5) { say(`refusing to measure: ${avail.toFixed(1)} GB available, under 5 GB even with --force`); process.exit(2); }
  preflight({ log: say, force: true });
} else preflight({ log: say });
const PROFILE = join(OUT, "profile");
seedProfile(PROFILE, { seed: SEED });
const u = new URL(buildUrl({ knobs, seed: SEEDDEF }));
for (const [k, v] of Object.entries(jprops)) u.searchParams.set(`jprop.${k}`, v);
const url = u.toString();
say(`seed ${SEED}\nurl ${url}`);
const game = launchGame({ url, profile: PROFILE, logPath: LOG, headless: !HEADFUL });

let conn, sid, faulted = false, verdict = "timeout";
const sessions = new Map();          // sessionId -> { type, url }
const record = (o) => appendFileSync(EVENTS, JSON.stringify(o) + "\n");
/* A fault can leave the page's main thread frozen, and a CDP request to a frozen target never answers -- so every
 * request that can reach the page after a fault is bounded; an unbounded one held a batch's teardown for 15 minutes. */
const bounded = (p, ms, fallback = undefined) => Promise.race([p, sleep(ms).then(() => fallback)]);
const frames = (st) => (st?.callFrames || []).map((f) => `${f.functionName || "<anon>"} ${f.url}:${f.lineNumber}:${f.columnNumber}`);

try {
  conn = await cdpConnect();
  sid = await attachPage(conn);
  sessions.set(sid, { type: "page", url });

  conn.on((m) => {
    if (m.method === "Target.attachedToTarget") {
      const s = m.params.sessionId, ti = m.params.targetInfo;
      sessions.set(s, { type: ti.type, url: ti.url });
      // fire and forget: never await a worker session
      conn.send("Runtime.enable", {}, s);
      conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s);
      return;
    }
    const who = sessions.get(m.sessionId) || { type: "?", url: "" };
    if (m.method === "Runtime.exceptionThrown") {
      const d = m.params.exceptionDetails || {};
      record({ t: Date.now() - T0, kind: "exception", target: who.type, text: d.text, description: d.exception?.description, frames: frames(d.stackTrace) });
      if (who.type !== "page" || fatal(d.exception?.description || "")) faulted = true;
    } else if (m.method === "Runtime.consoleAPICalled" && who.type !== "page") {
      const text = (m.params.args || []).map((a) => a.value ?? a.description ?? "").join(" ");
      if (m.params.type === "error" || m.params.type === "assert" || fatal(text))
        record({ t: Date.now() - T0, kind: "console", target: who.type, type: m.params.type, text, frames: frames(m.params.stackTrace) });
    }
  });
  await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sid);
  await conn.send("Page.enable", {}, sid);
  await conn.send("Runtime.enable", {}, sid);

  const evalPage = async (expression, awaitPromise = false) =>
    (await conn.send("Runtime.evaluate", { expression, awaitPromise, returnByValue: true }, sid)).result?.value;
  const shot = async (name) => {
    const r = await bounded(conn.send("Page.captureScreenshot", { format: "png" }, sid), 10_000, {});
    if (r.data) writeFileSync(join(OUT, "shots", `${name}.png`), Buffer.from(r.data, "base64"));
  };
  const key = async (code, k, holdMs = 80) => {
    const ev = (type) => `document.dispatchEvent(new KeyboardEvent(${JSON.stringify(type)},`
      + `{code:${JSON.stringify(code)},key:${JSON.stringify(k)},repeat:false,bubbles:true,cancelable:true}))`;
    await evalPage(ev("keydown")); await sleep(holdMs); await evalPage(ev("keyup"));
  };
  const logText = () => { try { return readFileSync(LOG, "utf8"); } catch { return ""; } };
  const waitLog = async (rx, ms, what) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const t = logText();
      if (rx.test(t)) return true;
      if (faulted || fatal(t)) { faulted = true; say(`fault while waiting for ${what}`); return false; }
      await sleep(1000);
    }
    say(`timed out waiting for ${what}`); return false;
  };

  for (let i = 0; ; i++) {
    const v = String(await evalPage("location.href + '|' + document.readyState") ?? "");
    if (v.startsWith("https://") && v.endsWith("|complete")) break;
    if (i > 120) throw new Error("page never loaded: " + v);
    await sleep(500);
  }

  // "New World" out of the seed's OPFS, so Singleplayer opens Create World (the user's path to the fault)
  const del = await evalPage(`(async () => { try {
      const sv = await (await (await navigator.storage.getDirectory()).getDirectoryHandle("minecraft")).getDirectoryHandle("saves");
      await sv.removeEntry("New World", { recursive: true }); const left = []; for await (const [n] of sv.entries()) left.push(n);
      return "deleted; saves now [" + left.join(", ") + "]"; } catch (e) { return "delete failed: " + e; } })()`, true);
  say(`world: ${del}`);

  await driveLauncher(conn, sid, SEEDDEF, { log: say });

  /* ModernFix's start line only exists with its measure_time feature, which Fabulously Optimized turns off; RRLS logs
   * "Overlay is removed!" when the loading overlay leaves and the title screen is showing. */
  if (!await waitLog(/Game took [\d.]+ seconds to start|Overlay is removed!/, TITLE_TIMEOUT_S * 1000, "the title screen")) throw new Error("no title screen");
  say("title screen up");
  if (TITLE_ONLY_S >= 0) {
    const until = Date.now() + TITLE_ONLY_S * 1000;
    while (Date.now() < until && !faulted) await sleep(1000);
    if (!faulted) verdict = "title-ok";
    throw new Error("title-only: done");
  }
  await sleep(4000);
  await shot("00-title");
  for (let i = 1; i <= TITLE_TABS; i++) { await key("Tab", "Tab"); await sleep(700); await shot(`01-tab${i}`); }
  await key("Enter", "Enter"); say("Enter on the focused title button (Singleplayer)");
  await sleep(5000); await shot("02-after-singleplayer");
  await key("Enter", "Enter"); say("Enter on Create World");
  await sleep(5000); await shot("03-after-create");

  const end = T0 + TIMEOUT_S * 1000;
  let spawnAt = 0, lastSample = 0;
  while (Date.now() < end) {
    // the page's debug bar: fps and the gc / native / wasm memory counters, which the log never carries
    if (Date.now() - lastSample > 30_000) {
      lastSample = Date.now();
      const bar = await bounded(evalPage(`((document.querySelector(".debug") || document.querySelector(".header .stats"))?.innerText || "").replace(/\\s+/g, " ")`), 5000, "(page did not answer)");
      say(`bar: ${bar}`);
    }
    const t = logText();
    if (faulted || fatal(t)) { faulted = true; verdict = "fault"; say("FAULT seen; collecting 20 s more"); await sleep(20000); break; }
    if (OOM_RX.test(t)) { verdict = "oom"; say(`OOM: ${t.match(OOM_RX)[0]}; collecting 5 s more`); await sleep(5000); break; }
    if (!spawnAt && /Time elapsed: \d+ ms/.test(t)) { spawnAt = Date.now(); say(`spawn area prepared; watching ${AFTER_SPAWN_S} s more`); }
    if (spawnAt && Date.now() - spawnAt > AFTER_SPAWN_S * 1000) { verdict = "clean"; break; }
    await sleep(2000);
  }
  try { await shot("99-end"); } catch {}
} catch (e) {
  verdict = verdict === "timeout" ? "driver-error" : verdict;
  say("driver error: " + (e?.stack || e));
} finally {
  try { await bounded(conn?.send?.("Browser.close"), 5000); } catch {}
  await sleep(3000);
  try { process.kill(-game.proc.pid, 0); game.kill(); } catch {}
  const keep = verdict !== "clean" && verdict !== "title-ok";
  reapProfile(PROFILE, { keep, log: say });
  // the instruments' lines, counted: a quarantine hit or a changed module block does not end a run, so it is easy to miss
  let counts = {};
  try {
    const t = readFileSync(LOG, "utf8");
    for (const tag of ["SGEN_BAD_DESC", "SGEN_HEADER_AUDIT", "FREE_QUARANTINE_HIT", "WASM_JIT_BYTES_CHANGED", "WASM_JIT_BYTES_OWNER"])
      counts[tag] = (t.match(new RegExp(`\\[MONO\\] ${tag}|^[^\\n]*CONSOLE[^\\n]*"${tag}`, "gm")) || []).length;
  } catch {}
  writeFileSync(join(OUT, "summary.json"), JSON.stringify({ verdict, faulted, counts, seed: SEED, url, knobs, jprops, titleTabs: TITLE_TABS, secs: (Date.now() - T0) / 1000 }, null, 2));
  say(`verdict ${verdict}; out ${OUT}`);
  writeFileSync(join(OUT, "done"), verdict + "\n");
  process.exit(0);
}
