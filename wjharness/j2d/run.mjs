// jbox2d three-way capture: ONE run of one arm under perf, windowed on the bench's own batch lines.
//
//   wj j2d/run <ours|teavm|hotspot> <outdir> [--events A|B] [--timeout 240] [--no-attr]
//
// Arms (all pinned to the P-core threads 0-7, as natrun and the Minecraft census are -- R369):
//   ours    the ikvmcraft ?bench page (dev server :5021), the shipped runtime + MONO_WASM_JIT_ORIGIN/CAPTURE
//           (custom section + console capture only; enctest t9 shows they change no code). jitdump via --perf-prof.
//   teavm   $TEAVM_BASELINE/teavm-wasm-fixed.html (the 2026-07-27 legacy linear-memory
//           build, R180 arm A), served from a local static server. Same chromium, same flags.
//   hs8     java-8-openjdk (the log's reference JVM, and natrun's) on the same jar, with j2d/jvmti/j2dagent (set A)
//           writing every nmethod's code and pc -> (method, bci) map for j2d/hsledger.py.
//   hs21    java-21-openjdk, same. On 2026-10-03 JDK 8u504 runs this jar at ~0.51 ms/step against ~0.36 for JDK 21,
//           with 8.8 M assists.sse_avx_mix per run against 4 -- the same JDK 8 read 0.33 in early September, so the
//           box changed (see the round). --no-attr drops the agent / the wasm knobs (timing check).
//
// The perf process WRAPS the workload (perf record ... -- <cmd>), so every process and thread is followed from its
// first instruction: no renderer-pid race (perfrun.mjs's), and the jitdump mmap is seen.
//
// WINDOW: from the arrival of "[bench] batch 1/12" to the arrival of "[bench] batch 12/12" = batches 2..12 =
// 2200 steps of steady state. Times are CLOCK_MONOTONIC (process.hrtime), the clock `perf record -k 1` stamps.
// Writes <outdir>/{perf.data, jit-*.dump (wasm arms), game.log, window.json}.
import { spawn, execSync } from "child_process";
import { mkdirSync, rmSync, writeFileSync, createWriteStream, existsSync, mkdtempSync, readdirSync, statSync } from "fs";
import { join, dirname } from "path";
import { tmpdir } from "os";
import { fileURLToPath } from "url";
import { connect, attachPage, consoleTap } from "../lib/browser.mjs";
import { driveLauncher } from "../lib/mcdrive.mjs";
import { ARMS, need, cfg, serveUrl, assertOutdir, tool } from "../lib/paths.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const pos = args.filter((a) => !a.startsWith("--"));
const opt = (k, d) => { const i = args.indexOf(`--${k}`); return i >= 0 ? args[i + 1] : d; };
const [arm, outdir] = pos;
const TEAVM_DIRS = {   // served root of each TeaVM arm (all legacy linear-memory wasm, WasmFixedWorkBenchmarkStarter)
  teavm: () => need("TEAVM_BASELINE", "the teavm arm"),   // 2026-07-27, 600 batches (stopped at 12)
  tvfull: () => join(ARMS(), "teavm-full"),       // current source, 12 batches, wasm optimization AGGRESSIVE (TeaVM FULL), R180 arm A
  tvsimple: () => join(ARMS(), "teavm-simple"),   // the same with -Pteavm.wasm.optimization=NONE (TeaVM SIMPLE: passes off), R180 arm B
  tvfull60: () => join(ARMS(), "teavm-full60"),   // tvfull / tvsimple rebuilt with BATCHES = 60 (the steady-state schedule, below)
  tvsimple60: () => join(ARMS(), "teavm-simple60"),
};
/* CheerpJ (2026-11-04): cj<V> = CheerpJ's default engine (its JIT emits JavaScript), cjgc<V> = the experimental WasmGC
 * engine (cheerpjInit({wasmGC:true}), JIT emits wasm), on Java V (8/11/17/21/25). j2d/cheerpj/index.html loads the
 * loader from Leaning Technologies' CDN (--cj-loader, default 5.0) and runs j2d/cheerpj/bench.jar -- the same jar as
 * every other arm -- with the schedule as positional args, served cross-origin isolated like the TeaVM arms. */
const CJ = /^cj(gc)?(\d+)$/.exec(arm || "");
if (!(["ours", ...Object.keys(TEAVM_DIRS)].includes(arm) || /^hs\d+$/.test(arm || "") || CJ) || !outdir) {
  console.error("usage: run.mjs <ours|teavm|tvfull|tvsimple|hs8|hs21|cj<V>|cjgc<V>> <outdir> [--events A|B] [--timeout S] [--no-attr] [--no-perf]"
    + " [--stop-after N] [--knob K=V (ours)] [--dumps (ours: dumpWasmJit at batch 1 and 12)] [--perf-extra 'ARGS']");
  process.exit(1);
}
const SET = opt("events", "A");
const TIMEOUT = Number(opt("timeout", 240)) * 1000;
const ATTR = !args.includes("--no-attr");
const CPUS = process.env.J2D_CPUS || cfg("WJ_PCPUS");   // the big cores (lib/paths.mjs autodetects them)
const PERF = !args.includes("--no-perf");          // timing-only: the workload alone, no perf in the tree
/* SCHEDULE W,S,N (default 600,200,12): warmup steps, steps per batch, batches. The window is batches WA+1..WB
 * (--window-batches WA,WB, default 1,12). R416: in the default schedule 45% of our JIT cycles in the window are
 * TIER-1 code -- the hottest methods (DynamicTree.query/insertLeaf, solveVelocityConstraints) get their tier-2 bodies
 * mid-window -- so a steady-state comparison runs 600,200,60 and reads batches 41..60. HotSpot gets the schedule as
 * the jar's positional args, ours as Java system properties bench.* (?jprop.*, read by IkvmBenchmarkStarter), TeaVM
 * by picking a build with that many batches (its schedule is compiled in). */
const SCHED = (opt("schedule", "600,200,12")).split(",").map(Number);
const [WA, WB] = (opt("window-batches", "1,12")).split(",").map(Number);
const STOP_AFTER = Number(opt("stop-after", WB));
const DUMPS = args.includes("--dumps");             // ours: dumpWasmJit() at batch 1 and batch 12, so the LAST dump is the window
const PERF_EXTRA = (opt("perf-extra", "") || "").split(/\s+/).filter(Boolean);   // e.g. "-g" for fp call chains
const dumpQueue = [];  // end the run at this batch (teavm-baseline is a 600-batch build)

/* EVENTS. Five general-purpose counters plus the two fixed ones is what this P-core schedules without
 * multiplexing (measured 2026-10-03: a sixth GP event drops every event to 50-88% coverage, which would make
 * samples x period undercount). THROTTLING is per TICK: CONFIG_HZ=1000 and max_sample_rate 3000 give each event at
 * most 3 samples per millisecond, and these arms retire ~12 G instr/s (IPC ~3) and ~4 G loads/s, so periods sized
 * for a per-SECOND budget (2,000,003 / 5,000,011 for instructions) still throttled hundreds of times inside a 2 s
 * window and undercounted instructions by up to 2x. Every event here is held to <= ~1 sample/ms on the fastest
 * arm; j2d/counts.py reports in-window throttles of the bench thread per event and they must read 0.
 * perf 7.2 honours `period=` inside cpu_core/.../. */
const C = cfg("WJ_PMU_CORE");   // these are Golden Cove event names (lib/paths.mjs: cpu_core on hybrid Intel)
const EVENTS = {
  A: [
    `${C}/instructions,period=20000003/upp`,         // PEBS, eventing IP
    `${C}/cycles,period=8000009/upp`,                // PEBS cycles: a stall lands on the instruction holding retirement
    `${C}/mem_load_retired.l1_miss,period=100003/upp`,
    `${C}/mem_load_retired.l2_miss,period=5003/upp`,
    `${C}/mem_load_retired.l3_miss,period=2003/upp`,
    `${C}/br_inst_retired.near_call,period=200003/u`,
    `${C}/mem_inst_retired.all_loads,period=8000009/u`,
  ],
  B: [
    `${C}/instructions,period=20000003/u`,
    `${C}/cycles,period=8000009/u`,
    `${C}/mem_inst_retired.all_stores,period=4000037/u`,
    `${C}/dtlb_load_misses.walk_active,period=200003/u`,
    `${C}/dtlb_load_misses.walk_completed,period=2003/u`,
    `${C}/br_inst_retired.indirect,period=200003/u`,
    `${C}/icache_data.stalls,period=400009/u`,
  ],
  // C: call sites, PEBS (R432). The eventing IP of a retired near call is the CALL itself, so samples land on the
  // caller's call instruction: wledger.py's calls_per_step column, per caller and call form. ~30 k calls/step at
  // ~0.6 ms/step is ~50 M calls/s, so 20011 keeps it near 2.5 samples/ms (the throttle is 3/ms).
  C: [
    `${C}/instructions,period=20000003/upp`,
    `${C}/cycles,period=8000009/upp`,
    `${C}/br_inst_retired.near_call,period=20011/upp`,
  ],
}[SET];
if (!EVENTS) { console.error(`unknown event set ${SET}`); process.exit(1); }

assertOutdir(outdir);   // it is deleted and re-created below
rmSync(outdir, { recursive: true, force: true });
mkdirSync(outdir, { recursive: true });
const log = createWriteStream(join(outdir, "game.log"));
const mono = () => Number(process.hrtime.bigint()) / 1e9;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const batchT = {}, batchMs = {};
let mean = null, checksum = null, done = false, failed = null, stoppedEarly = false;
function onLine(text) {
  const t = mono();
  log.write(`${t.toFixed(6)}\t${text}\n`);
  if (!text.includes("[bench]")) return;
  let m = text.match(/\[bench\] batch (\d+)\/(\d+): ([\d.]+) ms/);
  if (m) {
    batchT[Number(m[1])] = t; batchMs[Number(m[1])] = Number(m[3]);
    // stop at STOP_AFTER when the build runs more batches than that (teavm-baseline: 600; no checksum then)
    if (Number(m[1]) === STOP_AFTER && STOP_AFTER < Number(m[2])) { done = true; stoppedEarly = true; }
    if (DUMPS && (Number(m[1]) === WA || Number(m[1]) === WB)) dumpQueue.push(Number(m[1]));
  }
  m = text.match(/\[bench\] mean ([\d.]+) ms\/step/); if (m) mean = Number(m[1]);
  m = text.match(/\[bench\] checksum (-?\d+)/); if (m) { checksum = m[1]; done = true; }
  if (/LOAD FAILED/.test(text)) failed = text;
  if (CJ && /^\[cj\] (LOAD FAILED|exit)/.test(text) && checksum === null) failed = failed || text;
}

// ---------------------------------------------------------------- the workload command
let server = null, cmd, env = { ...process.env }, url = null;
const HS = /^hs\d+$/.test(arm);
if (HS) {
  const jar = join(need("TEAVM_DIR", "the HotSpot arms run its ikvm-bench jar"), "samples/benchmark/ikvm-bench/target/ikvm-bench-1.0-SNAPSHOT-jar-with-dependencies.jar");
  const javaHome = need(`JAVA_HOME_${arm.slice(2)}`, `the ${arm} arm`);
  const agent = join(ARMS(), "jvmti", "libj2dagent.so");   // built by j2d/jvmti/build.sh
  /* j2d/jvmti/j2dagent: code bytes + inlining-aware pc maps at nmethod load, for j2d/hsledger.py. NOT
   * -XX:+PrintAssembly: that disassembles on the compiler thread and delays C2 -- JDK 21 read 0.50 ms/step under it
   * against 0.36 without, 69% of the window outside compiled code. The agent's cost is not measurable. */
  cmd = ["taskset", "-c", CPUS, join(javaHome, "bin", "java"),
    ...(ATTR && SET === "A" ? ["-XX:+UnlockDiagnosticVMOptions", "-XX:+DebugNonSafepoints",
      `-agentpath:${agent}=${join(outdir, "j2dagent.txt")}`] : []),
    "-jar", jar, ...(opt("schedule") ? SCHED.map(String) : [])];
} else {
  if (arm === "ours") {
    // --base: another ikvmcraft dev server, e.g. rvcmp/jdk8's :5022 serving a saved IKVM flavor
    url = `${(opt("base") || serveUrl()).replace(/\/$/, "")}/?bench` + (ATTR ? "&wj.MONO_WASM_JIT_ORIGIN=1&wj.MONO_WASM_JIT_CAPTURE=1" : "")
      + args.flatMap((x, i) => x === "--knob" ? [`&wj.${args[i + 1]}`] : []).join("")   // --knob K=V (ours)
      + (opt("schedule") ? `&jprop.bench.warmup=${SCHED[0]}&jprop.bench.batchSteps=${SCHED[1]}&jprop.bench.batches=${SCHED[2]}` : "");
  } else {
    const port = 8000 + Math.floor(Math.random() * 900);
    server = spawn("node", [tool("serve-coep.mjs"), CJ ? `${join(HERE, "cheerpj")}:${join(ARMS(), "cheerpj")}` : TEAVM_DIRS[arm](), String(port)],
      { stdio: "ignore" });
    url = CJ ? `http://localhost:${port}/index.html?v=${CJ[2]}&gc=${CJ[1] ? 1 : 0}&loader=${opt("cj-loader", "5.0")}`
        + (opt("schedule") ? `&args=${SCHED.join(",")}` : "")
      : `http://localhost:${port}/teavm-wasm-fixed.html`;
    await sleep(800);
  }
  const prof = mkdtempSync(join(tmpdir(), "wj-j2d-"));   // the wj- prefix is how preflight and reap recognise a harness browser
  /* lib/browser.mjs launchChrome's flag set (kept in step with mcdrive.mjs launchGame), plus --perf-prof for the
   * jitdump (code bytes for every compiled wasm function, which the ledger matches against d8). */
  cmd = ["taskset", "-c", CPUS, process.env.CHROME_BIN || "/usr/bin/chromium",
    "--headless=new", "--remote-debugging-port=0", `--user-data-dir=${prof}`,
    "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage", "--ignore-certificate-errors",
    "--no-sandbox", "--unlimited-storage", "--disable-extensions", "--disable-component-extensions-with-background-pages",
    "--disable-sync", "--disable-background-networking", "--disable-client-side-phishing-detection",
    "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
    "--ignore-gpu-blocklist", "--use-gl=angle", `--use-angle=${process.env.MC_ANGLE || "gl"}`,
    `--js-flags=--stack-trace-limit=64,--no-logfile-per-isolate,--logfile=+${PERF && (SET === "A" || SET === "C") ? `,--perf-prof,--perf-prof-path=${outdir}` : ""}`,  // jitdump only where the ledger needs it (A, and C's call-site census); the logfile pair stops --perf-prof's implied --log writing isolate-*.log into the cwd (lib/mcdrive.mjs launchGame)
    "about:blank"];
  delete env.WAYLAND_DISPLAY; delete env.XDG_SESSION_TYPE;
  env.__J2D_PROFILE = prof;
}

const perfArgs = PERF ? ["record", "-q", "-k", "1", ...PERF_EXTRA, "-e", EVENTS.join(","), "-o", join(outdir, "perf.data"), "--", ...cmd] : cmd;
const exe = PERF ? "perf" : perfArgs.shift();
writeFileSync(join(outdir, "cmd.txt"), [exe, ...perfArgs].join(" ") + "\n");
// detached: perf and the workload get their own process group, so the teardown signals exactly them.
const perf = spawn(exe, perfArgs, { env, detached: true, stdio: ["ignore", "pipe", "pipe"] });
let perfErr = "", exited = false;
perf.on("exit", () => { exited = true; });

const t0 = Date.now();
if (HS) {
  let buf = "";
  const feed = (d) => { buf += d.toString(); let k; while ((k = buf.indexOf("\n")) >= 0) { onLine(buf.slice(0, k)); buf = buf.slice(k + 1); } };
  perf.stdout.on("data", feed);
  perf.stderr.on("data", (d) => { perfErr += d.toString(); feed(d); });
  while (!done && !exited && Date.now() - t0 < TIMEOUT) await sleep(100);
} else {
  perf.stdout.on("data", (d) => { perfErr += d.toString(); });
  const wsUrl = await new Promise((resolve, reject) => {
    const tm = setTimeout(() => reject(new Error("no CDP endpoint")), 30000);
    perf.stderr.on("data", (d) => {
      perfErr += d.toString();
      const m = perfErr.match(/ws:\/\/[^\s]+/);
      if (m) { clearTimeout(tm); resolve(m[0]); }
    });
  }).catch((e) => { failed = e.message; return null; });
  if (wsUrl) {
    const conn = await connect(wsUrl);
    const sid = await attachPage(conn);
    consoleTap(conn, sid, onLine);
    await conn.send("Runtime.enable", {}, sid);
    await conn.send("Page.enable", {}, sid);
    await conn.send("Page.navigate", { url }, sid);
    /* Since ikvmcraft 246560a (R514) ?bench boots through the launcher like the game: nothing runs until Play. The version
     * typed is irrelevant (?bench skips the Minecraft download check and runs bench.jar). */
    if (arm === "ours")
      await driveLauncher(conn, sid, "mcsr", { log: onLine }).catch((e) => { failed = `launcher: ${e.message}`; });
    let dumps = 0;
    while (!done && !failed && Date.now() - t0 < TIMEOUT) {
      while (dumpQueue.length) {   // dump from here, not the console callback: the RPC needs the loop
        const b = dumpQueue.shift();
        const r = await conn.send("Runtime.evaluate", { expression: "globalThis.dumpWasmJit ? (globalThis.dumpWasmJit(), 1) : 0", returnByValue: true }, sid);
        onLine(`[j2d] dumpWasmJit at batch ${b}: ${r.result?.value ?? JSON.stringify(r.__error)}`); dumps++;
      }
      await sleep(50);
    }
    while (dumpQueue.length) {
      const b = dumpQueue.shift();
      const r = await conn.send("Runtime.evaluate", { expression: "globalThis.dumpWasmJit ? (globalThis.dumpWasmJit(), 1) : 0", returnByValue: true }, sid);
      onLine(`[j2d] dumpWasmJit at batch ${b}: ${r.result?.value ?? JSON.stringify(r.__error)}`); dumps++;
    }
    await sleep(DUMPS ? 3000 : 500);   // the dump's console lines arrive asynchronously
    try { conn.close(); } catch { }
  }
}

// ---------------------------------------------------------------- teardown: the whole group, then wait for perf
try { process.kill(-perf.pid, "SIGINT"); } catch { }
for (let i = 0; i < 300 && !exited; i++) await sleep(100);
if (!exited) { try { process.kill(-perf.pid, "SIGKILL"); } catch { } await sleep(500); }
if (server) server.kill("SIGKILL");
if (env.__J2D_PROFILE) rmSync(env.__J2D_PROFILE, { recursive: true, force: true });
log.end();

/* CheerpJ flushes System.out only when the Java thread yields -- in this bench, at exit -- so every batch line arrives
 * at once and its arrival time says nothing. Rebuild the boundaries backwards from the checksum line (printed right after
 * the last batch) with the bench's own batch durations: batchT[i] = end of batch i. The error is the checksum + flush +
 * CDP latency (~ms against a multi-second window), and it shifts the window EARLIER, into batch WA's steady state. */
if (CJ && checksum !== null && batchT[WB]) {
  const tEnd = batchT[WB];
  for (let i = WB, t = tEnd; i >= 1 && batchMs[i] !== undefined; t -= batchMs[i] / 1000, i--) batchT[i] = t, batchT[i - 1] = t - batchMs[i] / 1000;
}
const steps = (WB - WA) * SCHED[1];
const ok = done && batchT[WA] && batchT[WB] && (checksum !== null || stoppedEarly);
const win = {
  arm, set: SET, attr: ATTR, perf: PERF, cpus: CPUS, ok: Boolean(ok), checksum, stopped_early: stoppedEarly, mean_ms_per_step: mean,
  window: ok ? [batchT[WA], batchT[WB]] : null, steps, schedule: SCHED, window_batches: [WA, WB],
  // the bench's own timing of the windowed batches (WA+1..WB)
  window_ms_per_step: ok ? Array.from({ length: WB - WA }, (_, i) => batchMs[WA + 1 + i]).reduce((s, v) => s + v, 0) / steps : null,
  batch_ms: batchMs, failed, perf_stderr_tail: perfErr.slice(-600),
};
writeFileSync(join(outdir, "window.json"), JSON.stringify(win, null, 1) + "\n");
const dumps = existsSync(outdir) ? readdirSync(outdir).filter((f) => /^jit-\d+\.dump$/.test(f)) : [];
const sz = existsSync(join(outdir, "perf.data")) ? statSync(join(outdir, "perf.data")).size : 0;
console.log(`${arm} ${SET}: ${ok ? "ok" : "FAILED"} checksum ${checksum} mean ${mean} window ${win.window_ms_per_step?.toFixed(4)} ms/step`
  + ` (${win.window ? (win.window[1] - win.window[0]).toFixed(2) : "?"} s) perf.data ${(sz / 1e6).toFixed(1)} MB, ${dumps.length} jitdump(s)`
  + (failed ? ` FAILED: ${failed}` : ""));
process.exit(ok ? 0 : 1);
