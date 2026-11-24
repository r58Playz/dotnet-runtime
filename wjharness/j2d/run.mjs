// jbox2d three-way capture: ONE run of one arm under perf, windowed on the bench's own batch lines.
//
//   wj j2d/run <ours|teavm|hotspot> <outdir> [--events A|A4|B|C|D|L] [--timeout 240] [--no-attr]
//   wj j2d/run <ours|tvfull|hs<N>> <outdir> --bench mctick ...   the Minecraft 26.2 server tick (wjharness/bench/mctick):
//           the same arms, windows and event sets over mctick's batches of ticks instead of jbox2d's steps. ours runs
//           ?bench&jar=mctick.jar on a copy of the 262n seed (its OPFS holds 26.2 and the save), HotSpot runs
//           $WJ_STATE/arms/mctick/cp-jvm.txt on the native stage's save, TeaVM serves $WJ_STATE/arms/mctick/teavm-full.
//           --schedule W,S,N is warmup ticks, ticks per batch, batches (default 6000,100,20; window 10,20). ours runs with
//           MONO_WASM_JIT_B4_QUIET=0 unless a --knob sets it: B4 (co-location) frames during the warm-up, the state the
//           game's Q60 window measures; with the quiet gate a run this short never frames a group (2026-11-20: -8% cycles).
//
// Arms (all pinned to the P-core threads 0-7, as natrun and the Minecraft census are -- R369):
//   ours    the ikvmcraft ?bench page (dev server :5021), the shipped runtime + MONO_WASM_JIT_ORIGIN/CAPTURE
//           (custom section + console capture only; enctest t9 shows they change no code). jitdump via --perf-prof.
//   tvfull  TeaVM linear-memory wasm, optimization FULL (R180 arm A); tvsimple: SIMPLE (arm B, passes off). Built by
//           wj bench/jbox2d/build into $WJ_STATE/arms/j2d/teavm-{full,simple} with the pinned TeaVM (bench/teavm-cli),
//           served from a local static server, schedule as ?args=W,S,N. Same chromium, same flags.
//           tvfull60 / tvsimple60 / teavm are the earlier builds (schedule compiled in; teavm = 2026-07-27, 600 batches).
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
import { mkdirSync, rmSync, writeFileSync, createWriteStream, existsSync, mkdtempSync, readdirSync, statSync, readFileSync } from "fs";
import { join, dirname, resolve } from "path";
import { tmpdir } from "os";
import { fileURLToPath } from "url";
import { connect, attachPage, consoleTap } from "../lib/browser.mjs";
import { driveLauncher, seedProfile } from "../lib/mcdrive.mjs";
import { ARMS, need, cfg, serveUrl, assertOutdir, tool, pinPrefix, angleBackend, IS_DARWIN } from "../lib/paths.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const pos = args.filter((a) => !a.startsWith("--"));
const opt = (k, d) => {   // --k V or --k=V
  const i = args.indexOf(`--${k}`);
  if (i >= 0) return args[i + 1];
  const e = args.find((x) => x.startsWith(`--${k}=`));
  return e !== undefined ? e.slice(k.length + 3) : d;
};
const arm = pos[0], outdir = pos[1] && resolve(pos[1]);   // absolute: the workload runs with outdir as its cwd
const BENCH = opt("bench", "jbox2d");   // jbox2d | mctick
const MC = BENCH === "mctick";
if (!["jbox2d", "mctick"].includes(BENCH)) { console.error(`unknown --bench ${BENCH}`); process.exit(1); }
const TEAVM_DIRS = {   // served root of each TeaVM arm (all linear-memory wasm, WasmFixedWorkBenchmarkStarter)
  tvfull: () => join(ARMS(), "j2d", "teavm-full"),       // wj bench/jbox2d/build: optimization FULL, any schedule (?args)
  tvsimple: () => join(ARMS(), "j2d", "teavm-simple"),   // the same at SIMPLE
  // the gradle-sample builds of R180-R527 (TeaVM 9be5eca57 too), batch count compiled in: 60, or 600 for teavm
  tvfull60: () => join(ARMS(), "teavm-full60"),
  tvsimple60: () => join(ARMS(), "teavm-simple60"),
  teavm: () => join(ARMS(), "teavm-baseline"),           // 2026-07-27, 600 batches (stopped at 12)
};
const TEAVM_ARGS = new Set(["tvfull", "tvsimple"]);    // the arms that take the schedule at run time
if (MC) {   // mctick's TeaVM builds (wj bench/mctick/build-teavm [-O 1]); the jbox2d-only arms are refused below
  for (const k of Object.keys(TEAVM_DIRS)) delete TEAVM_DIRS[k];
  TEAVM_DIRS.tvfull = () => join(ARMS(), "mctick", "teavm-full");       // optimization FULL
  TEAVM_DIRS.tvsimple = () => join(ARMS(), "mctick", "teavm-simple");   // SIMPLE: no inlining, devirtualization or ADVANCED passes
  TEAVM_DIRS.tvsplit = () => join(ARMS(), "mctick", "teavm-split");     // FULL, one module per executed function (bench/mctick/split-teavm.py)
}
/* CheerpJ (2026-11-04): cj<V> = CheerpJ's default engine (its JIT emits JavaScript), cjgc<V> = the experimental WasmGC
 * engine (cheerpjInit({wasmGC:true}), JIT emits wasm), on Java V (8/11/17/21/25). j2d/cheerpj/index.html loads the
 * loader from Leaning Technologies' CDN (--cj-loader, default 5.0) and runs j2d/cheerpj/bench.jar -- the same jar as
 * every other arm -- with the schedule as positional args, served cross-origin isolated like the TeaVM arms. */
const CJ = /^cj(gc)?(\d+)$/.exec(arm || "");
/* IKVM on native CoreCLR 10 (--bench mctick only): $WJ_STATE/arms/ikvm-clr10*'s jre/bin/java after one of its env files,
 * the IKVM configurations native/mcnative --ikvm runs (those READMEs): clr = the wasm app's switches (lazy linking; no
 * in-place relink on CoreCLR, so generation 1 stays), clrrelink = the same with the MonoMod relink backend (the closest
 * native analogue of ours), clrclassic = eager linking (upstream IKVM's mode, IKVM's best case). */
const CLR_ENVS = { clr: "ikvm-clr10/env.sh", clrrelink: "ikvm-clr10-relink/env-relink.sh", clrclassic: "ikvm-clr10/env-classic.sh" };
const CLR = MC ? CLR_ENVS[arm] : undefined;
if (!(["ours", ...Object.keys(TEAVM_DIRS)].includes(arm) || /^hs\d+$/.test(arm || "") || CJ || CLR) || !outdir) {
  console.error("usage: run.mjs <ours|tvfull|tvsimple|tvfull60|tvsimple60|teavm|hs<N>|cj<V>|cjgc<V>|clr|clrrelink|clrclassic (mctick)> <outdir> [--events A|A4|B|C|D|L] [--timeout S] [--no-attr] [--no-perf] [--no-jitdump]"
    + " [--stop-after N] [--knob K=V (ours)] [--dumps (ours: dumpWasmJit at batch 1 and 12)] [--perf-extra 'ARGS'] [--js-flags-extra 'F1,F2' (browser arms: appended to V8's --js-flags)] [--tv-dir DIR (a TeaVM arm's served directory, e.g. a split-teavm.py output)]");
  process.exit(1);
}
const SET = opt("events", "A");
const TIMEOUT = Number(opt("timeout", MC ? 1500 : 240)) * 1000;
const ATTR = !args.includes("--no-attr");
const CPUS = process.env.J2D_CPUS || cfg("WJ_PCPUS");   // the big cores (lib/paths.mjs autodetects them)
const PERF = !args.includes("--no-perf");          // timing-only: the workload alone, no perf in the tree
/* --no-jitdump: perf records the event set but V8 writes no jitdump (--perf-prof off). The counts (j2d/counts) need
 * none; the ledger and per-function readers do. A jitdump is ~2.3 GB a mctick run with a 9 MB image, and re-framing
 * churn made one 9.7 GB (2026-11-21): count-only timing batches pass this. */
const JITDUMP = !args.includes("--no-jitdump");
/* SCHEDULE W,S,N (default 600,200,12): warmup steps, steps per batch, batches. The window is batches WA+1..WB
 * (--window-batches WA,WB, default 1,12). R416: in the default schedule 45% of our JIT cycles in the window are
 * TIER-1 code -- the hottest methods (DynamicTree.query/insertLeaf, solveVelocityConstraints) get their tier-2 bodies
 * mid-window -- so a steady-state comparison runs 600,200,60 and reads batches 41..60. HotSpot gets the schedule as
 * the jar's positional args, ours as Java system properties bench.* (?jprop.*, read by IkvmBenchmarkStarter), TeaVM
 * by picking a build with that many batches (its schedule is compiled in). */
const SCHED = (opt("schedule", MC ? "6000,100,20" : "600,200,12")).split(",").map(Number);
const [WA, WB] = (opt("window-batches", MC ? "10,20" : "1,12")).split(",").map(Number);
const STOP_AFTER = Number(opt("stop-after", WB));
const DUMPS = args.includes("--dumps");             // ours: dumpWasmJit() at batch 1 and batch 12, so the LAST dump is the window
const PERF_EXTRA = (opt("perf-extra", "") || "").split(/\s+/).filter(Boolean);   // e.g. "-g" for fp call chains
const dumpQueue = [];  // end the run at this batch (teavm-baseline is a 600-batch build)
/* --xctrace (darwin, browser arms): the macOS stand-in for the perf event sets. At the first batch line, Instruments'
 * "CPU Counters" template attaches to the busiest renderer (mctick: at the first batch line; jbox2d, whose batches are
 * ~0.1 s against xctrace's seconds of start-up: at navigation); it stops after batch WB, and fe/xctrace.py reads every
 * thread's instructions and cycles over the window [batch WA, batch WB] by wall clock. The bench thread is the one
 * with the most cycles. window.json gets them as `xct`. */
const XCPROF_JIT = args.includes("--xcprof-jit");   // + V8's jitdump (code bytes) for fe/xchot.py; a mctick one is GBs
const XCPROF = args.includes("--xcprof") || XCPROF_JIT;   // the same capture with the Time Profiler template + V8's perf map: per-function time (fe/xcprof.py)
const XCT = args.includes("--xctrace") || XCPROF;
if (XCT && process.platform !== "darwin") { console.error("--xctrace is darwin-only (use the perf event sets)"); process.exit(1); }
let xct = null;
/* --xc-mode M: the CPU Counters counting mode (RecountDT's bottleneck.json: bottlenecks [default], processing, delivery,
 * l1d_miss_sampling, discarded_sampling, ...), passed as xctrace --recording-options */
const XC_MODE = opt("xc-mode");
const XC_JIT = args.includes("--xc-jit");   // keep V8's perf map and jitdump with a counters capture too (fe/xcmiss.py)
const batchWall = {};

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
  // A4: A with the L2 / L3 periods 4x coarser (2026-11-21): at A's periods TeaVM's mctick window throttled L2 (~600-1200)
  // and L3 (~250-2000) samples in its miss-dense chunk phase, which voids those counts (j2d/counts.py); quote L2 / L3
  // across arms from A4 runs.
  A4: [
    `${C}/instructions,period=20000003/upp`,
    `${C}/cycles,period=8000009/upp`,
    `${C}/mem_load_retired.l1_miss,period=100003/upp`,
    `${C}/mem_load_retired.l2_miss,period=20011/upp`,
    `${C}/mem_load_retired.l3_miss,period=8009/upp`,
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
  // D: where the cycles stall (mctick, 2026-11-19): B's dTLB and icache events throttled thousands of times a window on
  // ours at B's periods (a lower bound only), so these run 10x coarser; plus completed iTLB walks, mispredicted branches
  // and cycles with an L1D miss outstanding (memory_activity.stalls_l1d_miss).
  D: [
    `${C}/instructions,period=20000003/u`,
    `${C}/cycles,period=8000009/u`,
    `${C}/dtlb_load_misses.walk_active,period=2000003/u`,
    `${C}/itlb_misses.walk_completed,period=20011/u`,
    `${C}/icache_data.stalls,period=4000037/u`,
    `${C}/br_misp_retired.all_branches,period=200003/u`,
    `${C}/memory_activity.stalls_l1d_miss,period=4000037/u`,
  ],
  // L: call edges (mctick split layouts, 2026-11-19): cycles at a coarse period with the branch stack filtered to calls
  // (-j any_call,u below), read by bench/mctick/lbredges.py into the edge profile split-teavm.py co-locates by.
  L: [
    `${C}/cycles,period=2000003/u`,
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
/* --done-rx RX: the run is over when a line matches (a jar that is not the benchmark, run with --jar) */
const DONE_RX = opt("done-rx") ? new RegExp(opt("done-rx")) : null;
let lastProgress = 0;   // Date.now() of the last [bench] / [mctick] line: the stall watchdog's clock
function onLine(text) {
  const t = mono();
  log.write(`${t.toFixed(6)}\t${text}\n`);
  if (DONE_RX && DONE_RX.test(text)) done = true;
  if (/\[bench\]|\[mctick\]/.test(text)) lastProgress = Date.now();
  if (!text.includes("[bench]")) return;
  let m = text.match(/\[bench\] batch (\d+)\/(\d+): ([\d.]+) ms/);
  if (m) {
    batchT[Number(m[1])] = t; batchMs[Number(m[1])] = Number(m[3]); batchWall[Number(m[1])] = Date.now();
    if (XCT && !xct && MC && Number(m[1]) < WA) xct = startXct();   // jbox2d's batches are too short: it starts at navigation
    if (XCT && xct && Number(m[1]) === WB) stopXct();
    // stop at STOP_AFTER when the build runs more batches than that (teavm-baseline: 600; no checksum then)
    if (Number(m[1]) === STOP_AFTER && STOP_AFTER < Number(m[2])) { done = true; stoppedEarly = true; }
    if (DUMPS && (Number(m[1]) === WA || Number(m[1]) === WB)) dumpQueue.push(Number(m[1]));
  }
  m = text.match(/\[bench\] mean ([\d.]+) ms\/(step|tick)/); if (m) mean = Number(m[1]);
  m = text.match(/\[bench\] checksum (-?\d+)/); if (m) { checksum = m[1]; done = true; }
  if (/LOAD FAILED/.test(text)) failed = text;
  if (MC && /\[mctick\] DISCONNECTED/.test(text)) failed = failed || text;
  if (CJ && /^\[cj\] (LOAD FAILED|exit)/.test(text) && checksum === null) failed = failed || text;
}

/* the renderer whose CPU is highest among the browser's descendants (ps; darwin has no /proc) */
function busiestRenderer () {
  const rows = execSync("ps -Ao pid=,ppid=,pcpu=,command=", { encoding: "utf8" }).split("\n").map((l) => l.trim().match(/^(\d+)\s+(\d+)\s+([\d.]+)\s+(.*)$/)).filter(Boolean)
    .map((m) => ({ pid: Number(m[1]), ppid: Number(m[2]), cpu: Number(m[3]), cmd: m[4] }));
  const kids = new Set([perf.pid]);
  for (let grew = true; grew;) { grew = false; for (const r of rows) if (kids.has(r.ppid) && !kids.has(r.pid)) { kids.add(r.pid); grew = true; } }
  const rend = rows.filter((r) => kids.has(r.pid) && /--type=renderer/.test(r.cmd)).sort((a, b) => b.cpu - a.cpu);
  return rend[0]?.pid;
}
function startXct () {
  const pid = busiestRenderer();
  if (!pid) { onLine("[j2d] xctrace: no renderer found"); return { failed: true }; }
  const out = join(outdir, XCPROF ? "profile.trace" : "counters.trace");
  const xopts = [];
  if (XC_MODE && !XCPROF) {
    const f = join(outdir, "xc-options.json");
    /* the whole object `xctrace record --show-recording-options` prints: a partial one fails to load ("missing") */
    writeFileSync(f, JSON.stringify({ "CPU Counters": { allEventsAndFormulas: [], configurationType: { guided: {} }, countingLevel: "EL0",
      pmiEventAliasOrMnemonic: "", pmiThreshold: 1000000, processBucketSize: 10, sampleByTime: !/_sampling$/.test(XC_MODE),
      selectedCountingMode: { analysisMode: "bottleneck", countingMode: XC_MODE }, useDebuggingInformation: false,
      useHighFrequencyForGuidedMode: false, useHighFrequencyForManualMode: false } }));
    xopts.push("--recording-options", f);
  }
  const p = spawn("xcrun", ["xctrace", "record", "--template", XCPROF ? "Time Profiler" : "CPU Counters", ...xopts, "--attach", String(pid), "--output", out, "--no-prompt"],
    { stdio: ["ignore", "pipe", "pipe"], detached: true });
  let tail = ""; p.stdout.on("data", (d) => { tail += d; }); p.stderr.on("data", (d) => { tail += d; });
  const st = { p, pid, out, exited: false, tail: () => tail.slice(-400) };
  p.on("exit", () => { st.exited = true; });
  onLine(`[j2d] xctrace: attached to renderer ${pid}`);
  return st;
}
function stopXct () { if (xct?.p && !xct.stopping) { xct.stopping = true; try { xct.p.kill("SIGINT"); } catch { } } }

// ---------------------------------------------------------------- the workload command
let server = null, cmd, env = { ...process.env }, url = null;
const HS = /^hs\d+$/.test(arm);
if (HS && MC) {
  const cpFile = join(ARMS(), "mctick", "cp-jvm.txt");   // wj bench/mctick/build
  if (!existsSync(cpFile)) { console.error(`no ${cpFile}: run wj bench/mctick/build`); process.exit(1); }
  const javaHome = need(`JAVA_HOME_${arm.slice(2)}`, `the ${arm} arm`);
  const agent = join(ARMS(), "jvmti", "libj2dagent.so");
  const save = join(cfg("WJ_STATE"), "native", opt("stage", "sa262n"), "template", "saves", "New World");
  cmd = [...pinPrefix(CPUS), join(javaHome, "bin", "java"), "-Xmx4g",
    ...(ATTR && SET === "A" ? ["-XX:+UnlockDiagnosticVMOptions", "-XX:+DebugNonSafepoints",
      `-agentpath:${agent}=${join(outdir, "j2dagent.txt")}`] : []),
    `-Dmctick.world=${save}`, `-Dmctick.work=${join(outdir, "work")}`,
    "-cp", readFileSync(cpFile, "utf8").trim(), "wjbench.mctick.Mctick", ...SCHED.map(String)];
} else if (CLR) {
  const cpFile = join(ARMS(), "mctick", "cp-jvm.txt");
  if (!existsSync(cpFile)) { console.error(`no ${cpFile}: run wj bench/mctick/build`); process.exit(1); }
  const envFile = join(ARMS(), CLR), java = join(dirname(envFile), "jre", "bin", "java");
  if (!existsSync(envFile) || !existsSync(java)) { console.error(`no ${envFile} or ${java}: see native/mcnative.sh --ikvm`); process.exit(1); }
  const save = join(cfg("WJ_STATE"), "native", opt("stage", "sa262n"), "template", "saves", "New World");
  const q = (x) => `'${String(x).replace(/'/g, "'\\''")}'`;
  cmd = [...pinPrefix(CPUS), "bash", "-c", `. ${q(envFile)} && exec ${q(java)} -Xmx4g ${q(`-Dmctick.world=${save}`)} `
    + `${q(`-Dmctick.work=${join(outdir, "work")}`)} -cp ${q(readFileSync(cpFile, "utf8").trim())} wjbench.mctick.Mctick `
    + SCHED.join(" ")];
} else if (HS) {
  const jar = join(ARMS(), "j2d", "bench.jar");   // wj bench/jbox2d/build (its classes are byte-identical to ikvmcraft's public/bench.jar)
  if (!existsSync(jar)) { console.error(`no ${jar}: run wj bench/jbox2d/build`); process.exit(1); }
  const javaHome = need(`JAVA_HOME_${arm.slice(2)}`, `the ${arm} arm`);
  const agent = join(ARMS(), "jvmti", "libj2dagent.so");   // built by j2d/jvmti/build.sh
  /* j2d/jvmti/j2dagent: code bytes + inlining-aware pc maps at nmethod load, for j2d/hsledger.py. NOT
   * -XX:+PrintAssembly: that disassembles on the compiler thread and delays C2 -- JDK 21 read 0.50 ms/step under it
   * against 0.36 without, 69% of the window outside compiled code. The agent's cost is not measurable. */
  cmd = [...pinPrefix(CPUS), join(javaHome, "bin", "java"),
    ...(ATTR && SET === "A" ? ["-XX:+UnlockDiagnosticVMOptions", "-XX:+DebugNonSafepoints",
      `-agentpath:${agent}=${join(outdir, "j2dagent.txt")}`] : []),
    "-jar", jar, ...(opt("schedule") ? SCHED.map(String) : [])];
} else {
  if (arm === "ours") {
    // --base: another ikvmcraft dev server, e.g. rvcmp/jdk8's :5022 serving a saved IKVM flavor
    url = `${(opt("base") || serveUrl()).replace(/\/$/, "")}/?bench` + (ATTR ? "&wj.MONO_WASM_JIT_ORIGIN=1&wj.MONO_WASM_JIT_CAPTURE=1" : "")
      + args.flatMap((x, i) => x === "--knob" ? [`&wj.${args[i + 1]}`] : []).join("")   // --knob K=V (ours)
      + (MC && !args.some((x, i) => x === "--knob" && /^MONO_WASM_JIT_B4_QUIET=/.test(args[i + 1] || "")) ? "&wj.MONO_WASM_JIT_B4_QUIET=0" : "")
      + (MC ? `&jar=mctick.jar&jprop.mctick.world=${encodeURIComponent("/libsdl/minecraft/saves/New World")}`
          + `&jprop.mctick.work=${encodeURIComponent("/tmp/mctick")}&jprop.mctick.noexit=true`
          + `&jprop.mctick.warmup=${SCHED[0]}&jprop.mctick.batchTicks=${SCHED[1]}&jprop.mctick.batches=${SCHED[2]}`
        : (opt("schedule") ? `&jprop.bench.warmup=${SCHED[0]}&jprop.bench.batchSteps=${SCHED[1]}&jprop.bench.batches=${SCHED[2]}` : "")
          + (opt("jar") ? `&jar=${encodeURIComponent(opt("jar"))}` : ""));   // --jar FILE: another jar from public/ (ours)
  } else {
    const port = 8000 + Math.floor(Math.random() * 900);
    server = spawn("node", [tool("serve-coep.mjs"), CJ ? `${join(HERE, "cheerpj")}:${join(ARMS(), "cheerpj")}` : (opt("tv-dir") || TEAVM_DIRS[arm]()), String(port)],
      { stdio: "ignore" });
    url = CJ ? `http://localhost:${port}/index.html?v=${CJ[2]}&gc=${CJ[1] ? 1 : 0}&loader=${opt("cj-loader", "5.0")}`
        + (opt("schedule") ? `&args=${SCHED.join(",")}` : "")
      : MC ? `http://localhost:${port}/mctick.html?args=${SCHED.join(",")}`
      : `http://localhost:${port}/teavm-wasm-fixed.html` + (TEAVM_ARGS.has(arm) && opt("schedule") ? `?args=${SCHED.join(",")}` : "");
    await sleep(800);
  }
  /* mctick on ours needs the 262n seed's OPFS (26.2's jars and the save): a reflink copy beside the run, never on /tmp
   * (lib/mcdrive.mjs seedProfile). Everything else gets an empty profile. */
  const prof = MC && arm === "ours" ? seedProfile(join(outdir, "profile"), { seed: "262n" })
    : mkdtempSync(join(tmpdir(), "wj-j2d-"));   // the wj- prefix is how preflight and reap recognise a harness browser
  /* lib/browser.mjs launchChrome's flag set (kept in step with mcdrive.mjs launchGame), plus --perf-prof for the
   * jitdump (code bytes for every compiled wasm function, which the ledger matches against d8). */
  cmd = [...pinPrefix(CPUS), process.env.CHROME_BIN || "/usr/bin/chromium",
    "--headless=new", "--remote-debugging-port=0", `--user-data-dir=${prof}`,
    "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage", "--ignore-certificate-errors",
    "--no-sandbox", "--unlimited-storage", "--disable-extensions", "--disable-component-extensions-with-background-pages",
    "--disable-sync", "--disable-background-networking", "--disable-client-side-phishing-detection",
    "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
    "--ignore-gpu-blocklist", "--use-gl=angle", `--use-angle=${angleBackend()}`,
    `--js-flags=--stack-trace-limit=64,--no-logfile-per-isolate,--logfile=+${XCPROF || XC_JIT ? ",--perf-basic-prof" : ""}${XCPROF_JIT || XC_JIT ? `,--perf-prof,--perf-prof-path=${outdir}` : ""}${opt("js-flags-extra") ? "," + opt("js-flags-extra") : ""}${PERF && JITDUMP && (SET === "A" || SET === "C" || SET === "D" || SET === "L") ? `,--perf-prof,--perf-prof-path=${outdir}` : ""}`,  // jitdump only where the ledger needs it (A, C's call-site census, D's stalls by function); the logfile pair stops --perf-prof's implied --log writing isolate-*.log into the cwd (lib/mcdrive.mjs launchGame)
    "about:blank"];
  delete env.WAYLAND_DISPLAY; delete env.XDG_SESSION_TYPE;
  env.__J2D_PROFILE = prof;
}

const perfArgs = PERF ? ["record", "-q", "-k", "1", ...(SET === "L" ? ["-j", "any_call,u"] : []), ...PERF_EXTRA, "-e", EVENTS.join(","), "-o", join(outdir, "perf.data"), "--", ...cmd] : cmd;
const exe = PERF ? "perf" : perfArgs.shift();
writeFileSync(join(outdir, "cmd.txt"), [exe, ...perfArgs].join(" ") + "\n");
// detached: perf and the workload get their own process group, so the teardown signals exactly them. cwd outdir: the
// native arms' Minecraft writes logs/ into its cwd.
const perf = spawn(exe, perfArgs, { env, cwd: outdir, detached: true, stdio: ["ignore", "pipe", "pipe"] });
let perfErr = "", exited = false;
perf.on("exit", () => { exited = true; });

const t0 = Date.now();
if (HS || CLR) {
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
    /* Workers' uncaught exceptions with their stacks (2026-11-23: three mctick runs died of "worker sent an error! ...
     * memory access out of bounds", which the page's ErrorEvent reports without one). Runtime only, no Debugger. */
    conn.on((m) => {
      if (m.method === "Target.attachedToTarget" && m.params?.targetInfo?.type === "worker") {
        const ws = m.params.sessionId;
        conn.send("Runtime.enable", {}, ws).catch(() => { });
        conn.send("Runtime.runIfWaitingForDebugger", {}, ws).catch(() => { });
      } else if (m.method === "Runtime.exceptionThrown" && m.sessionId && m.sessionId !== sid) {
        const d = m.params?.exceptionDetails;
        onLine(`[worker-exception] ${(d?.exception?.description || d?.text || "").replace(/\n/g, " | ").slice(0, 4000)}`);
      }
    });
    await conn.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sid).catch(() => { });
    await conn.send("Runtime.enable", {}, sid);
    await conn.send("Page.enable", {}, sid);
    await conn.send("Page.navigate", { url }, sid);
    if (XCT && !MC && !xct) { await sleep(1500); xct = startXct(); }   // after the renderer for the page exists
    /* Since ikvmcraft 246560a (R514) ?bench boots through the launcher like the game: nothing runs until Play. The version
     * typed is irrelevant (?bench skips the Minecraft download check and runs bench.jar). */
    if (arm === "ours")
      await driveLauncher(conn, sid, MC ? "262n" : "mcsr", { log: onLine }).catch((e) => { failed = `launcher: ${e.message}`; });
    let dumps = 0;
    while (!done && !failed && Date.now() - t0 < TIMEOUT) {
      /* stall watchdog: once the bench has printed, 120 s without a [bench] / [mctick] line is a dead run (a worker
       * trap leaves the page alive and silent until TIMEOUT) */
      if (lastProgress && Date.now() - lastProgress > 120000) { failed = `stall: no bench progress for ${Math.round((Date.now() - lastProgress) / 1000)} s`; break; }
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

// ---------------------------------------------------------------- the counters capture: stop it before the browser goes
if (xct?.p) {
  stopXct();
  for (let i = 0; i < 1200 && !xct.exited; i++) await sleep(100);   // saving a long capture takes a while
  if (!xct.exited) { try { process.kill(-xct.p.pid, "SIGKILL"); } catch { } }
}
// ---------------------------------------------------------------- teardown: the whole group, then wait for perf
try { process.kill(-perf.pid, "SIGINT"); } catch { }
for (let i = 0; i < 300 && !exited; i++) await sleep(100);
if (!exited) { try { process.kill(-perf.pid, "SIGKILL"); } catch { } await sleep(500); }
if (server) server.kill("SIGKILL");
// a failed mctick run keeps its seed copy: it holds the OPFS state at the failure (lib/mcdrive.mjs reapProfile)
if (env.__J2D_PROFILE && !(MC && (failed || checksum === null))) rmSync(env.__J2D_PROFILE, { recursive: true, force: true });
/* macOS leftovers (2026-11-23: 39 GB of them filled the disk): xctrace's temporary .ktrace, which outlives a SIGINTed
 * recording, and the code-sign clone Chrome makes of itself at launch and removes only on a clean exit. Only what this run
 * created (born after t0), and no clone a live process runs from. */
if (IS_DARWIN) {
  const T = tmpdir(), X = join(dirname(T.replace(/\/$/, "")), "X", "com.google.Chrome.code_sign_clone");
  let ps = ""; try { ps = execSync("ps -axo command=", { encoding: "utf8", maxBuffer: 64 << 20 }); } catch { }
  const mine = (f) => { try { return statSync(f).birthtimeMs >= t0 - 5000; } catch { return false; } };
  try { for (const n of readdirSync(T)) if (/^instruments.*\.ktrace$/.test(n) && mine(join(T, n)) && !/xctrace/.test(ps)) rmSync(join(T, n), { recursive: true, force: true }); } catch { }
  try { for (const n of readdirSync(X)) if (n.startsWith("code_sign_clone.") && mine(join(X, n)) && !ps.includes(n)) rmSync(join(X, n), { recursive: true, force: true }); } catch { }
}
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
  bench: BENCH, arm, set: SET, attr: ATTR, perf: PERF, cpus: CPUS, ok: Boolean(ok), checksum, stopped_early: stoppedEarly, mean_ms_per_step: mean,
  window: ok ? [batchT[WA], batchT[WB]] : null, steps, schedule: SCHED, window_batches: [WA, WB],
  // the bench's own timing of the windowed batches (WA+1..WB)
  window_ms_per_step: ok ? Array.from({ length: WB - WA }, (_, i) => batchMs[WA + 1 + i]).reduce((s, v) => s + v, 0) / steps : null,
  batch_ms: batchMs, failed, perf_stderr_tail: perfErr.slice(-600),
};
if ((XCPROF || XC_JIT) && xct?.pid) {   // V8's map for the renderer, beside the capture; every map this browser wrote leaves /tmp
  const pm = `/tmp/perf-${xct.pid}.map`;
  if (existsSync(pm)) execSync(`cp ${JSON.stringify(pm)} ${JSON.stringify(join(outdir, "perf.map"))}`);
  /* the renderer's jitdump is the one fe/xchot.py reads; the browser's other processes wrote small ones beside it */
  if ((XCPROF_JIT || XC_JIT) && existsSync(join(outdir, `jit-${xct.pid}.dump`))) {
    execSync(`mv ${JSON.stringify(join(outdir, `jit-${xct.pid}.dump`))} ${JSON.stringify(join(outdir, "jit.dump"))}`);
    for (const f of readdirSync(outdir).filter((f) => /^jit-\d+\.dump$/.test(f))) rmSync(join(outdir, f));
  }
  for (const f of readdirSync("/tmp").filter((f) => /^perf-\d+\.map$/.test(f))) {
    const fp = join("/tmp", f); try { if (statSync(fp).mtimeMs >= t0 - 1000) rmSync(fp); } catch { }
  }
}
if (XCPROF && xct?.out && existsSync(xct.out) && existsSync(join(outdir, "perf.map")) && ok) {
  try {
    const txt = execSync(`python3 ${JSON.stringify(join(HERE, "..", "fe", "xcprof.py"))} ${JSON.stringify(xct.out)} ${JSON.stringify(join(outdir, "perf.map"))} --incl --top 80 --from-ms ${batchWall[WA]} --to-ms ${batchWall[WB]}`, { encoding: "utf8", maxBuffer: 1 << 26 });
    writeFileSync(join(outdir, "xcprof.txt"), txt);
    win.xcprof = txt.split("\n").slice(0, 12).join("\n");
  } catch (e) { win.xcprof = `error: ${String(e.message || e).slice(0, 300)}`; }
} else if (!XCPROF && xct?.out && existsSync(xct.out) && ok) {
  try {
    const r = JSON.parse(execSync(`python3 ${JSON.stringify(join(HERE, "..", "fe", "xctrace.py"))} ${JSON.stringify(xct.out)} --json --from-ms ${batchWall[WA]} --to-ms ${batchWall[WB]}`, { encoding: "utf8", maxBuffer: 1 << 26 }));
    /* the bench thread: the most clean pairs (a short-lived worker can sort first; 2026-11-23, Chrome 158) */
    const b = (r.threads || []).reduce((m, t) => (!m || (t.pairs || 0) > (m.pairs || 0) ? t : m), null);
    win.xct = { renderer: xct.pid, ...r, per: b ? { instructions: b.instructions / steps, cycles: b.cycles / steps, ipc: b.ipc, thread: b.thread, covered_s: b.span_s,
      window_s: (batchWall[WB] - batchWall[WA]) / 1000 } : null };
    /* the counting capture is 150-300 MB a run and window.json now holds what it says: drop it unless asked to keep it
     * (--keep-trace, or a sampling mode, whose PCs fe/xcmiss.py reads) -- 2026-11-23, two full disks */
    if (b && !XC_JIT && !args.includes("--keep-trace") && !/_sampling$/.test(XC_MODE || "")) rmSync(xct.out, { recursive: true, force: true });
  } catch (e) { win.xct = { error: String(e.message || e).slice(0, 400), tail: xct.tail() }; }
} else if (xct) win.xct = { error: "no capture", tail: xct.tail?.() };
writeFileSync(join(outdir, "window.json"), JSON.stringify(win, null, 1) + "\n");
const dumps = existsSync(outdir) ? readdirSync(outdir).filter((f) => /^jit-\d+\.dump$/.test(f)) : [];
const sz = existsSync(join(outdir, "perf.data")) ? statSync(join(outdir, "perf.data")).size : 0;
console.log(`${arm} ${SET}: ${ok ? "ok" : "FAILED"} checksum ${checksum} mean ${mean} window ${win.window_ms_per_step?.toFixed(4)} ms/${MC ? "tick" : "step"}`
  + ` (${win.window ? (win.window[1] - win.window[0]).toFixed(2) : "?"} s) perf.data ${(sz / 1e6).toFixed(1)} MB, ${dumps.length} jitdump(s)`
  + (win.xct?.per ? ` | ${(win.xct.per.instructions / 1e6).toFixed(3)} M ins ${(win.xct.per.cycles / 1e6).toFixed(3)} M cyc /${MC ? "tick" : "step"} IPC ${win.xct.per.ipc?.toFixed(2)}` : win.xct?.error ? ` | xctrace: ${win.xct.error.slice(0, 120)}` : "")
  + (failed ? ` FAILED: ${failed}` : ""));
process.exit(ok ? 0 : 1);
