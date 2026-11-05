// vcmp.mjs --seed NAME|DIR [--ver V] [--fabric F] --out DIR [--keys K:ms,...] [--warm-s N] [--window-s N] [--probe] [--no-perf]
//          [--knob K=V]... [--jprop K=V]... [--rm-mod GLOB]... [--opt key:value]... [--headful]
//
// One in-game window of ONE Minecraft version, for comparing versions/mod sets on the same served build (R508).
// The MCSR-era drivers (mcperf/runSession) key their phases off StateOutput and 1.16.1's menu; this one boots any
// seed whose OPFS holds `versions/<V>-fabric-0.19.5`, picks the version with main.tsx's ?mcver=, and drives the
// title -> Select World -> "New World" path by keyboard (--keys, one screenshot after each key so a miscounted Tab is
// visible). After "joined the game" + --warm-s it records, over --window-s:
//   perf.data      perf record -k 1 -g task-clock @499 Hz on the renderer (perfraw.py reads it with phases.json)
//   stat.txt       perf stat --per-thread cycles:u,instructions:u (counting mode: no sample throttling)
//   threads.json   per-thread run / runqueue / sleep from /proc schedstat + a wchan histogram
//   summary.json   frames over the window (emscripten-glfw's counter), fps, the url, the knobs
// --rm-mod GLOB deletes matching jars from minecraft/mods in the profile's OPFS before Play (a mod-set arm);
// --opt key:value goes to IKVM_GAME_OPTIONS (options.txt lines rewritten at launch).
// --gclog keeps index.ts's GC log (MONO_LOG_LEVEL=debug, MASK=gc); --sample-s S evaluates, every S s from Play on, the
// page's memory bar and (--sample-js FILE, called with --sample-arg's JSON file contents) a census function, into
// samples.jsonl; --perf-from-play starts the perf record at Play instead of at the window; --end-js FILE runs once after
// the window into end.json. (R510)
// Run it in a transient systemd service (CLAUDE.md, "Long-running work"); it writes <out>/done when finished.
import { seedProfile, reapProfile, buildUrl, launchGame, cdpConnect, sleep, monotonic, TRAP_RX, resolveSeed, driveLauncher } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
import { preflight } from "../lib/preflight.mjs";
import { spawn } from "child_process";
import { appendFileSync, copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "fs";
import { join, resolve } from "path";
import { runDir, assertOutdir } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
const multi = (n) => args.flatMap((a, i) => (a === `--${n}` ? [args[i + 1]] : []));
/* --seed is a registry name (lib/seeds.mjs) or a profile directory; --ver / --fabric override what it types into the launcher */
const SEEDDEF = (() => { const d = resolveSeed(str("seed")); return { ...d, version: str("ver", d.version || "1.16.5"), fabric: str("fabric", d.fabric || "0.19.5") }; })();
const VER = SEEDDEF.version;
const SEED = SEEDDEF.dir;
const OUT = str("out") ? assertOutdir(str("out")) : runDir("vcmp");
const WARM_S = Number(str("warm-s", 90));
const WINDOW_S = Number(str("window-s", 60));
const PROBE = args.includes("--probe");
const PERF = !args.includes("--no-perf") && !PROBE;
/* --keys K:ms,... ; default the seed's own menu path (lib/seeds.mjs) */
const KEYS = str("keys", null) != null
  ? str("keys").split(",").filter(Boolean).map((s) => { const [k, ms] = s.split(":"); return [k, Number(ms || 1500)]; })
  : (SEEDDEF.menu || []).map(([k, , ms]) => [k, Math.max(ms, 1200)]);
const TITLE_TIMEOUT_S = Number(str("title-timeout-s", 420));
const JOIN_TIMEOUT_S = Number(str("join-timeout-s", 300));
const knobs = { IKVM_DEFUSE_RANKED_AUTH: "0" };
for (const kv of multi("knob")) { const [k, ...v] = kv.split("="); knobs[k] = v.join("="); }
const opts = multi("opt");
if (opts.length) knobs.IKVM_GAME_OPTIONS = opts.join(",");
const jprops = { "sodium.checks.issue2561": "false" };
for (const kv of multi("jprop")) { const [k, ...v] = kv.split("="); jprops[k] = v.join("="); }
const RM_MODS = multi("rm-mod");
const GCLOG = args.includes("--gclog");
const SAMPLE_S = Number(str("sample-s", 0));
const SAMPLE_JS = str("sample-js", null), SAMPLE_ARG = str("sample-arg", null), END_JS = multi("end-js");
const PERF_FROM_PLAY = args.includes("--perf-from-play");
const WINDOWS = str("windows", null);
const jsCall = (file) => file ? `(${readFileSync(file, "utf8").trim().replace(/^\/\*[\s\S]*?\*\/\s*/, "").replace(/;$/, "")})(${SAMPLE_ARG ? readFileSync(SAMPLE_ARG, "utf8").trim() : ""})` : null;

mkdirSync(join(OUT, "shots"), { recursive: true });
const LOG = join(OUT, "chrome.log");
const T0 = Date.now();
const say = (s) => { const line = `[${((Date.now() - T0) / 1000).toFixed(1)}s] ${s}`; console.log(line); appendFileSync(join(OUT, "driver.log"), line + "\n"); };
const FATAL_RX = /Assertion at|assert_not_reached|forcing abort|Game crashed!|worker sent an error!/;
const OOM_RX = /Garbage collector could not allocate|Cannot enlarge memory|OutOfMemoryError|Out of memory/;
const fatal = (t) => FATAL_RX.test(t) || TRAP_RX.test(t);

if (args.includes("--force")) preflight({ log: say, force: true }); else preflight({ log: say });
const PROFILE = join(OUT, "profile");
seedProfile(PROFILE, { seed: SEED });
const u = new URL(buildUrl({ knobs, gcLog: GCLOG, seed: SEEDDEF }));
for (const [k, v] of Object.entries(jprops)) u.searchParams.set(`jprop.${k}`, v);
const url = u.toString();
say(`ver ${VER} seed ${SEED}\nurl ${url}`);
const game = launchGame({ url, profile: PROFILE, logPath: LOG, headless: !args.includes("--headful"), jsFlags: PERF ? "--perf-basic-prof" : null });

let conn, sid, verdict = "timeout";
const summary = { ver: VER, seed: SEED, url, knobs, jprops, rmMods: RM_MODS, uncapped: process.env.MC_UNCAPPED === "1", taskset: process.env.MC_TASKSET || null };
const bounded = (p, ms, fallback = undefined) => Promise.race([p, sleep(ms).then(() => fallback)]);
const pids = new Set();
const profilePids = () => {
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    try { if (readFileSync(`/proc/${d}/cmdline`, "utf8").includes(PROFILE)) pids.add(Number(d)); } catch {}
  }
};
const rendererPid = () => {
  let best = null, bestN = 0;
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    let cl = ""; try { cl = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
    if (!cl.includes(PROFILE) || !cl.includes("--type=renderer")) continue;
    let n = 0; try { n = readdirSync(`/proc/${d}/task`).length; } catch { continue; }
    if (n > bestN) { bestN = n; best = Number(d); }
  }
  return best;
};
const tasks = (pid) => {
  const m = new Map();
  let tids = []; try { tids = readdirSync(`/proc/${pid}/task`); } catch { return m; }
  for (const t of tids) {
    try {
      const [run, rq] = readFileSync(`/proc/${pid}/task/${t}/schedstat`, "utf8").trim().split(/\s+/).map(Number);
      m.set(t, { run, rq, comm: readFileSync(`/proc/${pid}/task/${t}/comm`, "utf8").trim() });
    } catch {}
  }
  return m;
};

try {
  conn = await cdpConnect();
  sid = await attachPage(conn);
  await conn.send("Page.enable", {}, sid);
  await conn.send("Runtime.enable", {}, sid);
  const evalPage = async (expression, awaitPromise = false) =>
    (await conn.send("Runtime.evaluate", { expression, awaitPromise, returnByValue: true }, sid)).result?.value;
  const shot = async (name) => {
    const r = await bounded(conn.send("Page.captureScreenshot", { format: "png" }, sid), 10_000, {});
    if (r?.data) writeFileSync(join(OUT, "shots", `${name}.png`), Buffer.from(r.data, "base64"));
  };
  const key = async (code) => {
    const k = /^(Tab|Enter|Escape|F\d+|Arrow\w+)$/.test(code) ? code : code.replace(/^(Key|Digit)/, "").toLowerCase();
    const ev = (type) => `document.dispatchEvent(new KeyboardEvent(${JSON.stringify(type)},`
      + `{code:${JSON.stringify(code)},key:${JSON.stringify(k)},repeat:false,bubbles:true,cancelable:true}))`;
    await evalPage(ev("keydown")); await sleep(80); await evalPage(ev("keyup"));
  };
  const logText = () => { try { return readFileSync(LOG, "utf8"); } catch { return ""; } };
  const waitLog = async (rx, ms, what) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const t = logText();
      if (rx.test(t)) return true;
      if (fatal(t)) { verdict = "fault"; say(`fault while waiting for ${what}: ${(t.match(FATAL_RX) || t.match(TRAP_RX))[0]}`); return false; }
      if (OOM_RX.test(t)) { verdict = "oom"; say(`OOM while waiting for ${what}`); return false; }
      await sleep(1000);
    }
    say(`timed out waiting for ${what}`); return false;
  };
  const frames = async () => Number(await bounded(evalPage("globalThis.wasm?.Module?._emscripten_glfw3_get_frame_count?.() ?? -1"), 15_000, -1));

  for (let i = 0; ; i++) {
    const v = String(await evalPage("location.href + '|' + document.readyState") ?? "");
    if (v.startsWith("https://") && v.endsWith("|complete")) break;
    if (i > 120) throw new Error("page never loaded: " + v);
    await sleep(500);
  }
  if (RM_MODS.length) {
    const rx = RM_MODS.map((g) => "^" + g.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".") + "$");
    const r = await evalPage(`(async () => { const rx = ${JSON.stringify(rx)}.map((s) => new RegExp(s, "i"));
      const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle("minecraft")).getDirectoryHandle("mods");
      const gone = []; const names = []; for await (const [n, h] of d.entries()) if (h.kind === "file") names.push(n);
      for (const n of names) if (rx.some((r) => r.test(n))) { await d.removeEntry(n); gone.push(n); }
      return gone; })()`, true);
    say(`removed ${r?.length ?? 0} mods: ${(r || []).join(" ")}`);
    summary.removed = r;
  }
  await driveLauncher(conn, sid, SEEDDEF, { log: say });
  const tPlay = Date.now();
  say("Play pressed");
  if (PERF && PERF_FROM_PLAY) {
    let rp = null; for (let i = 0; i < 60 && !(rp = rendererPid()); i++) await sleep(500);
    summary.rendererPid = rp;
    globalThis.__rec = spawn("perf", ["record", "-q", "-k", "1", "-g", "-F", "499", "-e", "task-clock", "-o", join(OUT, "perf.data"), "-p", String(rp)], { stdio: "ignore" });
    summary.perfFrom = monotonic();
    say(`perf from Play on renderer ${rp}`);
  }
  let sampling = true;
  const SAMPLE_EXPR = jsCall(SAMPLE_JS);
  const sampler = SAMPLE_S > 0 ? (async () => {
    while (sampling) {
      const t = (Date.now() - tPlay) / 1000;
      const bar = await bounded(evalPage(`((document.querySelector(".debug") || document.querySelector(".header .stats"))?.innerText || "").replace(/\\s+/g, " ")`), 5000, null);
      let js = null;
      if (SAMPLE_EXPR) { const r = await bounded(evalPage(SAMPLE_EXPR), 20000, null); try { js = r ? JSON.parse(r) : null; } catch { js = { raw: String(r).slice(0, 200) }; } }
      appendFileSync(join(OUT, "samples.jsonl"), JSON.stringify({ t, mono: monotonic(), bar, js }) + "\n");
      await sleep(SAMPLE_S * 1000);
    }
  })() : null;
  summary.tPlay = tPlay;
  if (!await waitLog(SEEDDEF.titleRx, TITLE_TIMEOUT_S * 1000, "the title screen")) throw new Error("no title screen");
  summary.titleS = (Date.now() - tPlay) / 1000;
  say(`title screen up (${summary.titleS.toFixed(1)} s after Play)`);
  await sleep(6000);
  await shot("00-title");
  let i = 0;
  for (const [k, ms] of KEYS) { i++; await key(k); await sleep(ms); await shot(`${String(i).padStart(2, "0")}-${k}`); say(`key ${k}`); }
  if (PROBE) { verdict = "probe"; throw new Error("probe: done"); }
  if (!await waitLog(SEEDDEF.joinRx, JOIN_TIMEOUT_S * 1000, "joined the game")) throw new Error("never joined");
  const tJoin = Date.now();
  summary.joinS = (tJoin - tPlay) / 1000;
  say(`joined (${summary.joinS.toFixed(1)} s after Play); warming ${WARM_S} s`);
  await sleep(WARM_S * 1000);
  await shot("10-prewindow");
  if (fatal(logText())) { verdict = "fault"; throw new Error("fault before the window"); }

  const rpid = rendererPid();
  profilePids();
  summary.rendererPid = rpid;
  let rec = null, stat = null;
  const m0 = monotonic();
  if (PERF && PERF_FROM_PLAY) rec = globalThis.__rec;
  else if (PERF) {
    rec = spawn("perf", ["record", "-q", "-k", "1", "-g", "-F", "499", "-e", "task-clock", "-o", join(OUT, "perf.data"), "-p", String(rpid)], { stdio: "ignore" });
  }
  if (PERF) {
    stat = spawn("perf", ["stat", "--per-thread", "-x", ",", "-e", "cycles:u,instructions:u", "-p", String(rpid), "-o", join(OUT, "stat.txt")], { stdio: "ignore" });
  }
  /* One window: frames, per-thread run/rq/sleep and a wchan histogram over SECS, plus the log lines written meanwhile. */
  const measure = async (name, secs) => {
    const th0 = tasks(rpid), w0 = monotonic();
    const f0 = await frames();
    const logLen0 = logText().length;
    const wchan = new Map();
    const wt = setInterval(() => {
      for (const t of th0.keys()) {
        try {
          const w = readFileSync(`/proc/${rpid}/task/${t}/wchan`, "utf8").trim() || "0";
          const st = readFileSync(`/proc/${rpid}/task/${t}/stat`, "utf8");
          const s = st.slice(st.lastIndexOf(")") + 2, st.lastIndexOf(")") + 3);
          const k = `${t}\t${s === "R" ? "R" : w}`;
          wchan.set(k, (wchan.get(k) || 0) + 1);
        } catch {}
      }
    }, 200);
    say(`window ${name}: ${secs} s (renderer ${rpid}, frames ${f0})`);
    await sleep(secs * 1000);
    const f1 = await frames();
    const w1 = monotonic(), th1 = tasks(rpid);
    clearInterval(wt);
    const wall = (w1 - w0) * 1e9, rows = [];
    for (const [t, a] of th0) {
      const b = th1.get(t); if (!b) continue;
      const run = b.run - a.run, rq = b.rq - a.rq;
      if (run < 1e8) continue;
      const wc = [...wchan].filter(([k]) => k.startsWith(`${t}\t`)).map(([k, n]) => [k.split("\t")[1], n]).sort((x, y) => y[1] - x[1]).slice(0, 6);
      rows.push({ tid: Number(t), comm: b.comm, run_s: run / 1e9, rq_s: rq / 1e9, sleep_s: (wall - run - rq) / 1e9, wchan: wc });
    }
    rows.sort((x, y) => y.run_s - x.run_s);
    const sfx = name === "ingame" ? "" : `-${name}`;
    writeFileSync(join(OUT, `threads${sfx}.json`), JSON.stringify({ wall_s: wall / 1e9, threads: rows }, null, 1));
    writeFileSync(join(OUT, `window${sfx}.log`), logText().slice(logLen0).split("\n").filter((l) => /wink|gex-wgpu|Can't keep up|CONSOLE/.test(l)).join("\n"));
    const w = { name, t0: w0, t1: w1, s: w1 - w0, frames: f1 - f0, fps: (f1 - f0) / (w1 - w0), f0, f1 };
    say(`window ${name} done: ${w.frames} frames in ${w.s.toFixed(1)} s = ${w.fps.toFixed(2)} fps`);
    return w;
  };
  // --windows "name=KEY,KEY:secs/..." (/ separates windows: batch.sh evals its arm string) : keys pressed before each window (then a 4 s settle and a screenshot)
  const WINS = WINDOWS ? WINDOWS.split("/").filter(Boolean).map((x) => { const [nk, secs] = x.split(":"); const [name, ks] = nk.split("=");
    return { name, keys: (ks || "").split(",").filter(Boolean), secs: Number(secs || WINDOW_S) }; }) : [{ name: "ingame", keys: [], secs: WINDOW_S }];
  summary.windows = [];
  for (const W of WINS) {
    for (const k of W.keys) { await key(k); await sleep(600); }
    if (W.keys.length) { await sleep(4000); await shot(`w-${W.name}`); }
    summary.windows.push(await measure(W.name, W.secs));
    if (fatal(logText())) break;
  }
  if (stat) stat.kill("SIGINT");
  if (rec) { rec.kill("SIGINT"); await new Promise((r) => { rec.on("exit", r); setTimeout(r, 120_000); }); }
  if (stat) await new Promise((r) => { stat.on("exit", r); setTimeout(r, 10_000); });
  const m1 = monotonic();
  await shot("11-postwindow");
  sampling = false;
  for (const f of END_JS) {
    const r = await bounded(evalPage(jsCall(f), true), 240000, null);   // awaitPromise: memaudit/waitmajor.js returns one
    const name = END_JS.length === 1 ? "end.json" : `end-${f.split("/").pop().replace(/\.js$/, "")}.json`;
    writeFileSync(join(OUT, name), r || "null"); say(`end census ${f}: ${r ? r.length : 0} bytes`);
  }
  const last = summary.windows[summary.windows.length - 1];
  summary.window = summary.windows[0];
  writeFileSync(join(OUT, "phases.json"), JSON.stringify({ event: "task-clock", freq: 499, period: null, url, knobs,
    phases: [...summary.windows.map((w) => ({ name: w.name, what: "window", t0: w.t0, t1: w.t1, secs: w.s, window: `${w.t0},${w.t1}`, covered: 1, top: true })),
             ...(summary.perfFrom ? [{ name: "run", what: "Play -> last window end", t0: summary.perfFrom, t1: last.t1, secs: last.t1 - summary.perfFrom, window: `${summary.perfFrom},${last.t1}`, covered: 1, top: true }] : [])] }, null, 1));
  say(`windows done: ${summary.windows.map((w) => `${w.name} ${w.fps.toFixed(2)} fps`).join(", ")}; perf ${(m1 - m0).toFixed(1)} s`);
  verdict = fatal(logText()) ? "fault" : "ok";
} catch (e) {
  if (verdict === "timeout") verdict = "driver-error";
  say("stop: " + (e?.message || e));
} finally {
  try { profilePids(); } catch {}
  try { await bounded(conn?.send?.("Browser.close"), 5000); } catch {}
  await sleep(3000);
  try { game.kill(); } catch {}
  // perf maps: keep the renderer's next to the capture (perfraw.py looks there), drop the rest (R280f)
  for (const p of pids) {
    const m = `/tmp/perf-${p}.map`;
    if (!existsSync(m)) continue;
    if (p === summary.rendererPid && PERF) { try { copyFileSync(m, join(OUT, `perf-${p}.map`)); } catch (e) { say(`map copy failed: ${e.message}`); } }
    try { rmSync(m); } catch {}
  }
  // a sibling process whose cmdline lacks the profile path also writes a map: drop dead pids' maps written during this run
  for (const f of readdirSync("/tmp")) {
    const m = f.match(/^perf-(\d+)\.map$/); if (!m) continue;
    try { if (statSync(`/tmp/${f}`).mtimeMs >= T0 && !existsSync(`/proc/${m[1]}`)) rmSync(`/tmp/${f}`); } catch {}
  }
  reapProfile(PROFILE, { keep: verdict !== "ok" && verdict !== "probe", log: say });
  summary.verdict = verdict;
  summary.secs = (Date.now() - T0) / 1000;
  writeFileSync(join(OUT, "summary.json"), JSON.stringify(summary, null, 2));
  say(`verdict ${verdict}; out ${OUT}`);
  writeFileSync(join(OUT, "done"), verdict + "\n");
  process.exit(0);
}
