// worldwait.mjs [--runs N] [--stats] [--dumps] [--bench-ms N] [--knob K=V] [--ro K=V]
//   — answer R67c's open question for the WORLD-GENERATION STALL, and (with --stats --dumps) serve as
//     CLAUDE.md's default driver for counter and fault batches.
//
// THE QUESTION, open since Round 67c and never answered on the slow mode:
//   "is the server thread RUN-bound (schedstat run_ns ~ wall) or SLEEP-bound? Run-bound confirms this
//    correction; sleep-bound would revive the blocking story."
//
// R13 answered it ONCE and on the wrong mode. Its schedstat table (DedicatedWorker 99.0% run / 0.8% sleep)
// was taken in a plateau window of a run that was not stalled -- it reports 6.8% of wall in frames over
// 100ms, i.e. the healthy mode. The stalled mode has never been sampled, so the branch that would "revive
// the blocking story" has simply never been looked at.
//
// `world.generate` (the 83%->99% segment) is where the bimodality lives: 30.3s fast vs 91.2s stall, a
// discrete ~+60s, 18.8% of 522 archived runs, knob-independent, and SILENT -- a corpus-wide line-kind
// differential over 418 fast against 98 stall runs finds no signature, and the two candidates it does
// surface ("Unable to get gpu adapter", "DEPRECATED_ENDPOINT") are emitted at a constant ~0.15 per 10s
// regardless of segment length, i.e. background noise that a longer window merely collects more of.
//
// WHY /proc AND NOT perf: threadwait.mjs's header records that this perf build cannot parse its own
// sched:sched_switch data ("broken or missing trace data") even on a fresh one-sample capture with tracefs
// readable. /proc/<tid>/{schedstat,wchan,comm} needs no privileges and answers the same question.
//
//   run     = on CPU
//   rq_wait = wanted the CPU and did not get it  -> CPU oversubscription (a scheduling problem)
//   sleep   = wall - run - rq_wait, i.e. waiting for something else -> blocking (a design problem)
//
// Conflating the last two would point at the wrong fix, which is why they stay separate.
import { runSession, buildUrl, seedProfile, reapProfile, monotonic, seedFromArgs } from "../lib/mcdrive.mjs";
import { preflight } from "../lib/preflight.mjs";
import { readFileSync, readdirSync, writeFileSync, mkdirSync, renameSync, openSync, readSync, closeSync } from "fs";
import { readPartitionStats } from "../lib/mcdrive.mjs";
import { join } from "path";
import { defaultOut, RUNS as RUNS_DIR } from "../lib/paths.mjs";

const args = process.argv.slice(2);
/* --seed NAME|DIR (lib/seeds.mjs), else MC_SEED, else 262n (DEFAULT_SEED) */
const SEED = seedFromArgs(args);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const RUNS = num("runs", 8);
const WORKER_PROBE = num("worker-probe", 0);
const MAPS_SNAPSHOT = args.includes("--maps-snapshot");
/* --vmledger (R486): at benchStart and benchEnd, the renderer's status, FULL smaps (VmFlags included, so the
 * kernel's own data_vm rule can be re-applied per VMA) and status again, plus every thread's comm and
 * /proc/<tid>/syscall (its user SP while blocked in a syscall, which is what ties an anonymous stack VMA to
 * the thread that owns it). Read synchronously, so the three files describe one instant to within the read. */
const VMLEDGER = args.includes("--vmledger");
/* --vmpeek (R486): with --vmledger, also read OWNER headers out of the renderer's memory at the same markers.
 * PartitionAlloc: every 2 MiB super page that overlaps a readable [anon:partition_alloc] VMA, 32 bytes at
 * super page + 4 KiB, where PartitionSuperPageExtentEntry {root, next, n_consecutive, n_nonempty} lives; plus the
 * root reader's counters. V8: every 256 KiB-aligned address inside a writable [anon:v8-sandbox] VMA under 1 GiB
 * (i.e. not the wasm memory) or an [anon:v8] rw-p VMA, 32 bytes, where a MemoryChunk header would sit. Only this
 * process may read /proc/<pid>/mem (Yama ptrace_scope=1 wants an ancestor), which is why it lives in the driver. */
const VMPEEK = args.includes("--vmpeek");
function vmPeek (dir, tag, pid) {
  const t0 = Date.now();
  const maps = readFileSync(`/proc/${pid}/maps`, "utf8");
  let pa;
  try { pa = readPartitionStats(pid, maps); } catch (e) { pa = { __error: e.message }; }
  writeFileSync(join(dir, `${tag}-pa.json`), JSON.stringify(pa));
  const SP = 1n << 21n, VP = 1n << 17n;
  const sp = new Set(), vp = new Set();
  for (const line of maps.split("\n")) {
    const m = /^([0-9a-f]+)-([0-9a-f]+) (\S{4})/.exec(line);
    if (!m) continue;
    const s = BigInt("0x" + m[1]), e = BigInt("0x" + m[2]);
    /* PA's metadata is OUT of the pools on this build: one 4 KiB rw island per super page / direct map in a 16 GiB
     * partition_alloc region, PartitionSuperPageExtentEntry {root, next, n_consecutive, n_nonempty} at its start
     * (R486). Read every 4 KiB rw PA VMA at offset 0; the BRP pool's sp+8K ref-count pages come along and are told
     * apart offline by address. */
    if (line.endsWith("[anon:partition_alloc]") && m[3] === "rw-p" && e - s === 4096n) sp.add(s);
    const v8rw = line.endsWith("[anon:v8]") && m[3] === "rw-p";
    const sbrw = line.endsWith("[anon:v8-sandbox]") && m[3] === "rw-p" && e - s < (1n << 30n);
    if (v8rw || sbrw) for (let b = ((s + VP - 1n) / VP) * VP; b < e; b += VP) vp.add(b);
  }
  const fd = openSync(`/proc/${pid}/mem`, "r");
  const buf = Buffer.alloc(32);
  const peek = (set, off) => [...set].sort((x, y) => (x < y ? -1 : 1)).map((b) => {
    let n = -1; try { n = readSync(fd, buf, 0, 32, b + off); } catch {}
    return `${b.toString(16)}\t${n === 32 ? buf.toString("hex") : "unreadable"}`;
  }).join("\n") + "\n";
  writeFileSync(join(dir, `${tag}-pa-islands.tsv`), peek(sp, 0n));
  writeFileSync(join(dir, `${tag}-v8-chunks.tsv`), peek(vp, 0n));
  /* V8's page metadata table (R486): IsolateGroup::default_isolate_group_ is a static pointer in chromium's .bss
   * (0x135813a8 in build 9edff249..., from the debuginfod .symtab); the group holds metadata_pointer_table_, 32768
   * entries of {BasePage* metadata_, Isolate* isolate_} (isolate-group.h:75-104, :343 at V8 15.3.76). Dump 640 KiB of
   * the group; the table's offset is found offline by matching validated chunk headers' indices. */
  try {
    const img = /^([0-9a-f]+)-[0-9a-f]+ r..p 00000000 \S+ \d+\s+(\/\S*\/chromium)$/m.exec(maps);
    if (img) {
      const gp = Buffer.alloc(8);
      readSync(fd, gp, 0, 8, BigInt("0x" + img[1]) + 0x135813a8n);
      const group = gp.readBigUInt64LE(0);
      const g = Buffer.alloc(640 * 1024);
      const n = readSync(fd, g, 0, g.length, group);
      writeFileSync(join(dir, `${tag}-isolate-group.bin`), g.subarray(0, n));
      writeFileSync(join(dir, `${tag}-isolate-group.txt`), `image ${img[1]} group 0x${group.toString(16)} read ${n}\n`);
    }
  } catch (e) { console.log(`  [worldwait] vmpeek isolate group: ${e.message}`); }
  console.log(`  [worldwait] vmpeek ${tag}: ${sp.size} PA islands, ${vp.size} v8 chunk slots, ${Date.now() - t0} ms`);
  closeSync(fd);
}
function vmLedgerSnap (dir, tag, pid) {
  const rd = (p) => { try { return readFileSync(p, "utf8"); } catch (e) { return `!! ${e.message}\n`; } };
  const t0 = Date.now();
  writeFileSync(join(dir, `${tag}-status-pre.txt`), rd(`/proc/${pid}/status`));
  writeFileSync(join(dir, `${tag}-smaps.txt`), rd(`/proc/${pid}/smaps`));
  writeFileSync(join(dir, `${tag}-status-post.txt`), rd(`/proc/${pid}/status`));
  const th = [];
  let tids = [];
  try { tids = readdirSync(`/proc/${pid}/task`); } catch {}
  for (const t of tids) th.push(`${t}\t${rd(`/proc/${pid}/task/${t}/comm`).trim()}\t${rd(`/proc/${pid}/task/${t}/syscall`).trim()}`);
  writeFileSync(join(dir, `${tag}-threads.tsv`), th.join("\n") + "\n");
  console.log(`  [worldwait] vmledger ${tag}: ${tids.length} threads, ${Date.now() - t0} ms`);
}
if (!Number.isInteger(WORKER_PROBE) || WORKER_PROBE < 0 || WORKER_PROBE > 16) {
  console.error("worldwait: --worker-probe must be an integer from 0 to 16");
  process.exit(2);
}
if (args.includes("--smaps")) process.env.MC_MEM_SMAPS = "1";
/* --stats turns on MONO_WASM_JIT_STATS so the JIT_ADMIT poller reads non-zero. It also SLOWS the run
 * (CLAUDE.md: world generation 91s -> 146s), so `world.generate` shifts in BOTH modes and the absolute
 * thresholds in classifyWorldGen no longer split it. Classify a --stats batch from its own observed
 * distribution instead of trusting `mode`; the raw `generate` is recorded either way. */
const STATS = args.includes("--stats");
/* --dumps calls globalThis.dumpWasmJit() at both ends of the in-game window.
 *
 * THIS FILE IS CLAUDE.md's DOCUMENTED DEFAULT DRIVER FOR COUNTER AND FAULT BATCHES, AND IT COULD NOT
 * REACH THE STRUCTURAL DUMPS AT ALL. `[wasm-jit build]`, `[wasm-jit relink]`, the co-location identity,
 * the admit split and WJ_GRAPH all print from mono_wasm_jit_dump_stats, which is reached ONLY via
 * globalThis.dumpWasmJit() -- and runSession's `dumps` option defaults false, so a `--stats` run here
 * produced none of them and the log looked clean rather than empty. That cost two runs on 2026-09-12
 * (R253) before the cause was the harness rather than the knob.
 *
 * Separate from --stats on purpose. --stats gates the COUNTERS (and slows world generation ~60%);
 * --dumps gates PRINTING them. MONO_WASM_JIT_DUMP_DEP_GRAPH=1 needs both: the knob to build the graph
 * and this to emit it. The printf dumps are the calls that can throw `unwind` from the page, so
 * runSession guards each one -- the cost of leaving this on is a lost dump, not a lost run. */
const DUMPS = args.includes("--dumps");
/* --gclog turns on MONO_LOG_LEVEL=debug + MONO_LOG_MASK=gc.
 *
 * THIS IS WHY THE STALL LOOKED "SILENT". Every measurement so far ran at the harness default
 * MONO_LOG_LEVEL=error, so no GC line is emitted at all -- the corpus-wide line-kind differential over
 * 418 fast vs 98 stall runs could not have seen a GC even if one caused the whole stall. Worldgen is the
 * most allocation-heavy phase in the run (chunk sections, palettes, heightmaps), and the stall is
 * confined to the 83-88% band, i.e. the first ~22 chunks of NEW generation.
 *
 * COST, per mcdrive's buildUrl note: a line per MINOR GC through console.log into an uncapped DOM list,
 * so absolute timings inflate and the mode thresholds shift. The FAST/STALL contrast and the GC COUNTS
 * are still valid -- classify from the observed distribution, not from the absolute cutoffs. */
const GCLOG = args.includes("--gclog");
/* --v8gc: V8's OWN --trace-gc, which is a DIFFERENT instrument from --gclog (that one is mono's GC via
 * MONO_LOG_MASK). This is the only way to see the 4 GB per-isolate JS-heap ceiling that R233's coredump
 * died against, because /proc lumps the JS heap into [anon:v8-sandbox] with wasm linear memory. */
const V8GC = args.includes("--v8gc");
/* --memtrace: chromium's OWN memory-infra, captured to a FILE.
 *
 * This is the only instrument that breaks down [anon:partition_alloc] (2.25 GiB and the largest
 * unattributed bucket in R233): PartitionAlloc, Blink, V8 heap spaces and skia all register
 * MemoryAllocatorDump providers. wasm CODE space does not, which is why the /proc sampler exists
 * alongside this rather than instead of it.
 *
 * TO A FILE, NOT OVER CDP: the CDP route throws `RangeError: Invalid string length` in the node client
 * on this workload -- a limit on stringifying the payload in JS, not on the data.
 *
 * AND WITHOUT heap_profiler_options / --enable-heap-profiling. That is a DIFFERENT subsystem: the native
 * allocation sampler captures a stack per sampled allocation, and V8 JIT + wasm frames keep no
 * frame-pointer chain to unwind, so it walks into a PROT_NONE guard page (SEGV_ACCERR). That is the
 * crash to remember; memory-infra's dump providers only report SIZES and never unwind, so they are not
 * exposed to it -- and JSPI, which moves stacks around, only makes the unwinding path worse, not the
 * dumps. Keep them separate. */
const MEMTRACE = args.includes("--memtrace");
/* --ab-codecache: INTERLEAVED arms for Blink's wasm DISK code cache (?codecache=0 makes the app hand the
 * runtime a JS-constructed Response, which has no CachedMetadataHandler, so Blink never serialises the
 * compiled module into BufferPartition). Interleaved because this is a memory claim on a box whose
 * baseline drifts; odd runs keep the cache (current behaviour). */
const AB_CODECACHE = args.includes("--ab-codecache");
/* --codecache 0|1 forces one arm for a whole batch. */
const CODECACHE = num("codecache", NaN);
/* --ab-uamem: INTERLEAVED arms for the `uamem` knob, alternating every run.
 *
 * Interleaved and not two batches, because Round 230 addendum 7 measured stall INCIDENCE clustering by
 * batch on one unchanged binary -- 0%, 31%, 25%, 0% -- so a sequential before/after here would have
 * "shown" a 31-point effect from nothing at all. This is R194's ordering rule applied to a rate rather
 * than a timing. */
const AB_UAMEM = args.includes("--ab-uamem");
/* --ab-gcpace: INTERLEAVED arms for the GC PACEMAKER that replaces measureUserAgentSpecificMemory.
 *
 * `gc()` only exists under --expose-gc, which is passed unconditionally below so both arms run the same
 * binary and the same flags -- only the page-side interval differs. Odd runs get the pacemaker ON.
 * Interleaved for the same reason --ab-uamem is: stall incidence clusters by batch (0/31/25/0 percent on
 * one unchanged binary), so sequential arms can manufacture a 30-point effect from nothing. */
const AB_GCPACE = args.includes("--ab-gcpace");
/* --ab-parkfix: INTERLEAVED arms for IKVM_PARK_FIX (drop LockSupport.parkNanos from the idle wait).
 *
 * This one has TWO predictions and the weaker-looking one is the better test. Stall incidence should fall
 * (the 1.17 ms miss penalty is what makes the pipeline bistable), but that needs ~20 runs/arm to resolve
 * against a ~19% base rate. The FAST-mode `world.generate` should ALSO shorten, because a fast run still
 * burns 34,736 parks / 37.3 s of overshoot -- and fast runs are ~80% of all runs and reproduce to ~5%,
 * so a few runs per arm settle it. Measure the fast mode, not the incidence. */
const AB_PARKFIX = args.includes("--ab-parkfix");
/* --ab-parknanos: INTERLEAVED arms for the SHIPPED park fix (libikvm/park.c, memory.atomic.wait32 with a
 * nanosecond timeout, replacing Object.wait's 1ms floor). IKVM_PARK_NANOS=0 forces the old Monitor path,
 * so both arms are ONE binary -- which matters more here than anywhere: stall incidence clusters by batch
 * (0/31/25/0 percent across four batches of an UNCHANGED binary), so sequential arms cannot resolve it.
 * Odd runs get the OLD behaviour, matching --ab-uamem's convention. */
const AB_PARKNANOS = args.includes("--ab-parknanos");
/* Repeatable --knob NAME=VALUE, same spelling as mcbench/mcperf. index.ts forwards any MONO_|IKVM_|DOTNET_
 * prefixed knob straight through as an env var, so this is how a Java-side transformer gets switched on. */
const KNOBS = {};
/* Repeatable --ro NAME=VALUE -> ?ro.NAME=VALUE, a mono RUNTIME OPTION (options-def.h), which is a
 * different namespace from --knob (env vars). jiterpreter-aot-table-size lives here. */
const ROPTS = {};
for (let i = 0; i < args.length; i++) {
  if (args[i] === "--knob") { const [k, ...v] = String(args[i + 1] ?? "").split("="); if (k) KNOBS[k] = v.join("="); }
  if (args[i] === "--ro") { const [k, ...v] = String(args[i + 1] ?? "").split("="); if (k) ROPTS[k] = v.join("="); }
}
/*
 * OUTPUT DIRECTORY. `--out <dir>` because a FIXED one silently destroys the previous batch: this was
 * hard-coded, a second batch reused `run-1.log`..`run-N.log`, and it overwrote the one log in 20 that
 * held a real fault -- the `BootstrapMethodError` chain that turned out to be the whole finding of that
 * round. The evidence survived only because it had already been transcribed into MINECRAFT-FINDINGS.md.
 *
 * Worse, the flag was PASSED and IGNORED: this file parses `--knob`/`--ro` and drops anything it does not
 * recognise, so `--out soak2-out` looked accepted and did nothing. A harness that silently ignores an
 * argument is how an arm measures a configuration nobody asked for -- the same failure mode index.ts
 * records for per-knob query params.
 */
const outIdx = args.indexOf("--out");
/* --out DIR (default runs/<today>-worldwait; an earlier batch's run-N files there move to prev-<stamp>/) */
const OUT = defaultOut("worldwait", outIdx >= 0 && args[outIdx + 1] ? args[outIdx + 1] : null);

/* REFUSE unknown flags rather than ignoring them. */
{
  const KNOWN = new Set(["--runs", "--stats", "--dumps", "--force", "--out", "--knob", "--ro", "--bench-ms", "--seed",
                         "--warm-ms", "--no-walk", "--walk", "--headless", "--no-headless", "--uamem",
                         "--gcpace", "--parkfix", "--parknanos", "--codecache", "--memtrace", "--seconds",
                         "--gclog", "--v8gc", "--worker-probe", "--smaps", "--maps-snapshot", "--vmledger", "--vmpeek"]);
  const takesValue = new Set(["--runs", "--out", "--knob", "--ro", "--bench-ms", "--warm-ms", "--seconds", "--seed",
                              "--gcpace", "--parknanos", "--codecache", "--worker-probe"]);
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (!a.startsWith("--")) continue;
    if (!KNOWN.has(a)) {
      console.error(`worldwait: unknown flag ${a}. Refusing rather than ignoring it -- an ignored flag`);
      console.error(`           means the arm ran a configuration you did not ask for.`);
      process.exit(2);
    }
    if (takesValue.has(a)) i++;
  }
}

/* Never clobber a previous batch's logs in the same directory. */
{
  const stale = readdirSync(OUT).filter((f) => /^run-\d+\.(log|json)$/.test(f));
  if (stale.length) {
    const keep = join(OUT, `prev-${new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19)}`);
    mkdirSync(keep, { recursive: true });
    for (const f of stale) renameSync(join(OUT, f), join(keep, f));
    console.log(`worldwait: moved ${stale.length} file(s) from a previous batch into ${keep}`);
  }
}

const rd = (p) => { try { return readFileSync(p, "utf8"); } catch { return null; } };

/*
 * THE GAME'S RENDERER, picked by THREAD COUNT, not by being the first `--type=renderer` in /proc order.
 *
 * The first version matched `--type=renderer` + the profile path and took whichever came first. It
 * attached to a renderer with SEVEN threads, all 100% sleeping, comms `chromium` and `ThreadPoolForeg` --
 * a blank/secondary renderer -- and duly reported `total run-seconds 0.0` for a phase in which the game
 * was saturating two cores. A measurement that reads zero is easy to mistake for "the threads were idle",
 * which is the exact wrong conclusion here.
 *
 * Emscripten's pthreads are Web Workers, so the renderer hosting the game has ~30+ threads while every
 * other chromium process has a handful. Taking the max is unambiguous, needs no knowledge of chromium's
 * process naming, and is self-checking: `assertGameThreads` refuses a process with no DedicatedWorker
 * rather than silently sampling the wrong one.
 */
function findRenderer (profile) {
  let best = null, bestN = 0;
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    const cl = rd(`/proc/${d}/cmdline`);
    if (!cl || !cl.includes(profile)) continue;
    let n = 0;
    try { n = readdirSync(`/proc/${d}/task`).length; } catch { continue; }
    if (n > bestN) { bestN = n; best = Number(d); }
  }
  return best;
}

/** Comm histogram, printed once. A wrong process is then visible in the output instead of inferred from
 *  a suspicious zero. Returns the number of worker threads found. */
function assertGameThreads (pid, log) {
  const c = new Map();
  let tids = [];
  try { tids = readdirSync(`/proc/${pid}/task`); } catch { }
  for (const t of tids) {
    const comm = (rd(`/proc/${pid}/task/${t}/comm`) || "?").trim();
    c.set(comm, (c.get(comm) || 0) + 1);
  }
  const top = [...c.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6);
  const workers = (c.get("DedicatedWorker") || 0);
  log(`  [worldwait] pid ${pid}: ${tids.length} threads, ${workers} DedicatedWorker`
    + `  [${top.map(([k, n]) => `${k}x${n}`).join(" ")}]`);
  if (!workers) log(`  !! no DedicatedWorker threads — this is probably NOT the game's renderer; sample is suspect`);
  return workers;
}

function snapshot (pid) {
  const m = new Map();
  let tids;
  try { tids = readdirSync(`/proc/${pid}/task`); } catch { return m; }
  for (const tid of tids) {
    const ss = rd(`/proc/${pid}/task/${tid}/schedstat`);
    if (!ss) continue;
    /* THE THIRD FIELD IS TIMESLICES -- how many times the kernel put this thread on a CPU -- and it was
     * being parsed and thrown away. It is the direct test for a PARK LOOP.
     *
     * Minecraft's server thread idles via BlockableEventLoop.waitForTasks():
     *     Thread.yield(); LockSupport.parkNanos("waiting for tasks", 100000L);   // 100 us
     * On a JVM that is a cheap timed park. Under emscripten every park/unpark is an Atomics.wait /
     * Atomics.notify, and V8 routes ALL of them through ONE GLOBAL mutex -- verified in
     * $CHROMIUM_SRC/v8/src/execution/futex-emulation.cc: `DEFINE_LAZY_LEAKY_OBJECT_GETTER
     * (FutexWaitList, GetWaitList)` with the comment "returns the lazily initialized global wait list",
     * taken on the wait path (:370), the wake path (:150), and held by the condvar itself (:531).
     *
     * A thread parking every 100 us shows ~10,000 timeslices/second. On-CPU profiling CANNOT see any of
     * this (a parked thread accrues no samples, and the futex symbols are ~0.5% of the window), which is
     * why five instruments in a row came back empty. Context-switch RATE is the one cheap observable that
     * a park loop cannot hide from. */
    const [run, wait, slices] = ss.trim().split(/\s+/).map(Number);
    m.set(tid, { run, wait, slices, comm: (rd(`/proc/${pid}/task/${tid}/comm`) || "?").trim(),
                 wchan: (rd(`/proc/${pid}/task/${tid}/wchan`) || "?").trim() });
  }
  return m;
}

const results = [];
preflight({ force: args.includes("--force") });

for (let i = 1; i <= RUNS; i++) {
  const profile = seedProfile(join(RUNS_DIR(), ".profiles", `worldwait-profile-${i}`), { seed: SEED });
  const logPath = join(OUT, `run-${i}.log`);
  /* `uamem` is a plain page param, not a `wj.`/`ro.` knob, so it is appended rather than passed through
   * buildUrl's prefix machinery. Odd runs get the OLD behaviour (measurement on) so the control and the
   * fix alternate. */
  const uamem = AB_UAMEM ? (i % 2 === 1) : false;
  const parkfix = AB_PARKFIX ? (i % 2 === 1) : null;
  const gcpace = AB_GCPACE ? (i % 2 === 1 ? 5000 : 0) : null;
  const parknanos = AB_PARKNANOS ? (i % 2 === 1 ? "0" : "1") : null;
  const codecache = AB_CODECACHE ? (i % 2 === 1 ? 1 : 0) : (Number.isNaN(CODECACHE) ? null : CODECACHE);
  /* CDP-DRIVEN, not --trace-startup. The command-line route fires its periodic trigger exactly once at
   * t=0 and never registers the renderer that runs the game, so every dump it produced was boot-time
   * noise from the wrong processes (R233 addendum 2). runSession now starts Tracing over CDP and
   * REQUESTS dumps on a timer instead. */
  const traceFile = join(OUT, `run-${i}-memtrace.json`);
  const url = buildUrl({ seed: SEED, knobs: { ...KNOBS, ...(STATS ? { stats: 1 } : {}),
                                  ...(parkfix !== null ? { IKVM_PARK_FIX: parkfix ? "1" : "0" } : {}),
                                  ...(parknanos !== null ? { IKVM_PARK_NANOS: parknanos } : {}) },
                         runtimeOptions: ROPTS,
                         gcLog: GCLOG })
    + (uamem ? "&uamem=1" : "")
    + (gcpace !== null ? `&gcpace=${gcpace}` : "")
    + (codecache !== null ? `&codecache=${codecache}` : "");
  let pid = null, t0 = null, first = null, wchanHits = new Map(), samples = 0;
  let timer = null, acctAtSpawnDone = null;

  const startSampling = () => {
    pid = findRenderer(profile);
    if (!pid) { console.log(`  !! run ${i}: renderer pid not found; skipping the sample`); return; }
    assertGameThreads(pid, console.log);
    first = snapshot(pid); t0 = monotonic();
    /* 200ms, matching threadwait.mjs. wchan is SAMPLED rather than integrated, so its histogram is
     * "share of samples found blocked here" -- an approximation of blocked-time share, which is enough to
     * name the primitive (futex vs io_schedule vs poll) and that is the question. */
    timer = setInterval(() => {
      samples++;
      const s = snapshot(pid);
      for (const [tid, v] of s) {
        if (!v.wchan || v.wchan === "0" || v.wchan === "?") continue;
        const k = `${v.comm}|${v.wchan}`;
        wchanHits.set(k, (wchanHits.get(k) || 0) + 1);
      }
    }, 200);
  };
  const stopSampling = () => {
    if (timer) { clearInterval(timer); timer = null; }
    if (!pid || !first) return null;
    const wall = monotonic() - t0, last = snapshot(pid), rows = [];
    for (const [tid, b] of last) {
      /* Threads that appear DURING the phase are kept, not dropped. The first version iterated `first`
       * and skipped anything absent from it, which silently loses exactly the pool that matters here --
       * Minecraft's chunk-generation/lighting ForkJoinPool workers are created on demand, and a trap
       * audit found `ForkJoinTask_doExec` in 14 of 105 trap stacks, so those threads are live in this
       * phase. Their counters start from thread creation rather than from t0, so `run` can slightly
       * exceed the sampled window; flagged `late` so it is visible rather than quietly inflating. */
      const a = first.get(tid);
      const run = (b.run - (a?.run ?? 0)) / 1e9, rqw = (b.wait - (a?.wait ?? 0)) / 1e9;
      const sleep = Math.max(0, wall - run - rqw);
      const slices = (b.slices ?? 0) - (a?.slices ?? 0);
      rows.push({ tid, comm: b.comm, run, rqw, sleep, late: !a, slices, ctxPerSec: slices / wall,
                  /* Mean on-CPU burst. A park loop shows a tiny one (microseconds); real compute shows
                   * milliseconds. This separates "blocked waiting for work" from "doing work slowly". */
                  usPerSlice: slices ? (run * 1e6) / slices : null,
                  runPct: 100 * run / wall, rqwPct: 100 * rqw / wall, sleepPct: 100 * sleep / wall });
    }
    rows.sort((x, y) => y.run - x.run);
    return { wall, samples, rows, wchan: [...wchanHits.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12) };
  };

  console.log(`\n=== worldwait run ${i}/${RUNS} ===`);
  let r;
  try {
    r = await runSession({
      seed: SEED,
      /* --bench-ms lengthens the in-game window. Only reason to: --memtrace's trace file is auto-saved
       * when its duration expires and LOST if the browser is SIGKILLed first, so the run has to outlast
       * the trace. Run length otherwise varies with the server-settle wait (20-120 s), which makes a
       * fixed trace duration a coin flip. */
      url, profile, logPath, benchMs: num("bench-ms", 5000), warmMs: num("warm-ms", 0), walk: false, saveQuit: false,   // --warm-ms was accepted and ignored before 2026-10-01
      workerProbeCount: WORKER_PROBE,
      dumps: DUMPS,
      /* --expose-gc on BOTH arms: the page decides whether to call gc(), so the browser configuration is
       * held constant and the only difference is the interval. */
      // WJ_JS_FLAGS passes raw V8 flags through. Added for R262's cross-check: the admission DFS
      // overflow is a V8 call-stack RangeError, so --stack-size=N is an INDEPENDENT test of the depth
      // diagnosis -- if a wedging arm passes with a bigger V8 stack, depth is the mechanism, and that
      // conclusion does not rest on the WJC_ADMIT_DEPTH_* counters being right. Diagnostic only: the
      // shipped app cannot set V8 flags, so this is never a fix.
      /*
       * --perf-basic-prof BY DEFAULT ON THIS DRIVER, and only on this one.
       *
       * R275 caught the world-load hang live -- two threads at ~92% CPU spinning on five addresses
       * spanning ~22 bytes -- and could not name the loop, because the addresses were in `[anon:v8]`
       * with no symbol map. The capture was useless for the one question it existed to answer, and the
       * round ended with "reproduce under mcperf.mjs and those five addresses become a function name".
       *
       * The map costs ~240 MB in /tmp and some V8 bookkeeping, which is why mcperf makes it a choice.
       * Here it is free of consequence: worldwait is the FAULT/COUNTER driver -- its output is verdicts
       * and counters, never a timing -- so the overhead cannot corrupt anything it reports, and the one
       * thing it exists to catch is precisely the thing that needs symbols. reap.sh clears the maps, and
       * preflight now refuses on a full /tmp, so the accumulation that made this expensive is handled.
       *
       * MC_NO_PERF_MAP=1 opts out for a run that must be as close to the product as possible.
       */
      jsFlags: [AB_GCPACE ? "--expose-gc" : null, V8GC ? "--trace-gc" : null,
                process.env.MC_NO_PERF_MAP ? null : "--perf-basic-prof",
                process.env.WJ_JS_FLAGS || null].filter(Boolean).join(" ") || null,
      memTracePath: MEMTRACE ? traceFile : null,
      memDumpEveryMs: num("memtrace-every", 15) * 1000,
      hooks: {
        onMarker: (name) => {
          /* Sampling is bounded by the MARKERS, not by when runSession returns.
           *
           * The first version started at spawn83 and stopped only after runSession returned, which is
           * fine for a healthy run and useless for a sick one: run 1 of the first batch had its world die
           * at spawnDone, sat out the full 480 s `inworld` budget, and so reported a 33 s generate segment
           * averaged over a ~500 s window. Every percentage in that sample was diluted ~15x. Close the
           * window where the phase closes. */
          if (name === "spawn83") { console.log("  [worldwait] generate segment opened — sampling threads"); startSampling(); }
          if (name === "spawnDone") { acctAtSpawnDone = stopSampling(); console.log("  [worldwait] generate segment closed — sampling stopped"); }
          if ((name === "benchStart" || name === "benchEnd") && VMLEDGER && pid) {
            try { vmLedgerSnap(OUT, `run-${i}-${name}`, pid); if (VMPEEK) vmPeek(OUT, `run-${i}-${name}`, pid); }
            catch (e) { console.log(`  [worldwait] vmledger ${name} failed: ${e.message}`); }
          }
          if (name === "benchEnd" && MAPS_SNAPSHOT && pid) {
            try { writeFileSync(join(OUT, `run-${i}-maps.txt`), readFileSync(`/proc/${pid}/maps`)); }
            catch (e) { console.log(`  [worldwait] maps snapshot failed: ${e.message}`); }
          }
        },
      },
    });
  } catch (e) { console.log(`  !! run ${i} threw: ${e && e.message}`); continue; }
  /* Prefer the marker-bounded accounting; fall back only if spawnDone never fired (a dead world), where
   * the numbers describe the death rather than the phase and are labelled as such. */
  const acct = acctAtSpawnDone ?? stopSampling();
  const acctBounded = !!acctAtSpawnDone;

  const gen = r.subphases.find((p) => p.name === "world.generate")?.secs ?? null;
  const load = r.subphases.find((p) => p.name === "world.load")?.secs ?? null;
  const row = { run: i, ok: r.ok, mode: r.worldGenMode, generate: gen, load, acctBounded, stats: STATS, uamem, gcpace, parkfix,
                /* THE VERDICT IS THE FIELD TO GROUP BY. `ok` is a progress flag -- true the instant
                 * benchEnd is seen, regardless of faults -- and `mode` is world-generation DURATION, not
                 * a failure, which this project has already conflated with the world-load hang once.
                 * `verdict` is the one field that says whether the run counts against the build. */
                verdict: r.verdict, spin: r.spin ?? null,
                /* Per CLASS and KEY, not just the kind. `["fault","fault"]` could not distinguish a
                 * dangling MonoMethod (loader.c:1826) from a malformed EH clause table
                 * (method-to-ir.c:805) -- both of which occur in this corpus -- so no batch could ever
                 * show that a fix had moved one of them. */
                faults: Object.values(r.faultSummary ?? {}).map((f) => `${f.cls}:${f.key}`),
                faultDetail: r.faultSummary ?? null, acct,
                /* The whole point of --stats: admission counters scoped to world.generate. `vfbParked`
                 * and `vfbNotLive` are the two that would show workers losing the JIT tier mid-phase,
                 * which is the leading candidate for a parallelism collapse that costs +40% CPU. */
                admit: r.admit ?? null, opfs: r.opfs ?? null, v8gc: r.v8gc ?? null,
                /* Renderer memory, sampled from /proc every 12 s (see summariseMem in mcdrive). Carried
                 * per-run so it can be correlated with the JIT counters in `admit` -- module count and
                 * tier size are the suspected drivers of the chromium-side ceilings. */
                mem: r.mem ?? null, workerProbe: r.workerProbe ?? null,
                tableLen: r.window?.tableLen ?? null,
                /* Tier SIZE for the same run (R347): the page's stats-independent jitLiveness() snapshot, i.e.
                 * registered methods and ALL instantiations, which the memory rows need as their denominator
                 * (`admit.total.registered` is stats-gated and reads 0 on a non-stats run). The END bench line
                 * spans several log lines, so it is read back from the run's own log, last snapshot wins. */
                live: (() => {
                  try {
                    const m = [...readFileSync(r.log, "utf8").matchAll(/\{"auto":[^}]*\}/g)].at(-1);
                    return m ? JSON.parse(m[0]) : null;
                  } catch { return null; }
                })(),
                /* Provenance: the exact URL, so the knob set of a stored run is readable without
                 * re-deriving it from the batch script (see runSession's note). */
                url: r.url ?? null };
  results.push(row);
  writeFileSync(join(OUT, `run-${i}.json`), JSON.stringify(row, null, 2));
  /* Drop the ~1 GB profile copy unless the run failed, in which case it is the only evidence there is.
   * Nothing used to do this: 40 of these accumulated in /var/tmp (38 GB) alongside 340 in mc-out, and a
   * full disk or a full /tmp kills a run at PAGE LOAD -- which reads as a browser bug, not as
   * housekeeping, and cost two arms that were nearly blamed on the change under test. */
  reapProfile(profile, { keep: row.verdict !== "ok", log: (m) => console.log(m) });

  console.log(`  ${AB_UAMEM ? `[uamem=${uamem ? 1 : 0}] ` : ""}${AB_GCPACE ? `[gcpace=${gcpace}] ` : ""}${AB_PARKFIX ? `[parkfix=${parkfix ? 1 : 0}] ` : ""}${AB_PARKNANOS ? `[parknanos=${parknanos}] ` : ""}${codecache !== null ? `[codecache=${codecache}] ` : ""}verdict=${r.verdict}  mode=${r.worldGenMode}  world.generate=${gen}s  load=${load}s  faults=${JSON.stringify(row.faults)}`);
  const ag = r.admit?.byPhase?.["world.generate"];
  if (r.admit && !r.admit.allZero)
    console.log(`  admit[world.generate]: ${ag ? Object.entries(ag).map(([k, v]) => `${k} ${v.toLocaleString()}`).join("  ") : "(no delta)"}`
      + (r.admit.splitViolations ? `   !! FINAL-SAMPLE IDENTITY VIOLATION — a fallback route has no counter` : "")
      + (r.admit.tornReads ? `   (${r.admit.tornReads} torn reads, benign: counters are read non-atomically)` : ""));
  else if (r.admit?.allZero) console.log(`  admit: all zero — pass --stats`);
  /* PER-WORKER INSTANTIATION, parsed out of the runtime's own `[wasm-jit time]` dump rather than a
   * counter poll, because that line is where the two numbers sit side by side and the whole point is the
   * gap between them:
   *   inst_compile -- the compiling thread's ONE validating instantiation per method. This is what the
   *                   line used to print, alone, under the label `instantiate=`.
   *   inst_all     -- every instantiation on every worker. The cost of the function table being
   *                   per-worker, which nothing in the tree could read before the census gate was split.
   * per-module needs the DISTINCT module count, which only depcycles.py knows (co-location puts up to 16
   * methods in one module), so this prints per-REGISTERED-method and says so. Run
   *   python3 depcycles.py worldwait-out/run-N.log
   * for the real divisor -- it reports contracted nodes, i.e. modules. */
  { let logText = ""; try { logText = readFileSync(logPath, "utf8"); } catch (e) { }
    /* LAST match, not first: dumpWasmJit runs at BOTH ends of the window, and the closing one is the
     * only one that has seen the whole run. */
    const m = [...logText.matchAll(/\[wasm-jit time\] gen=([\d.]+)ms inst_compile=([\d.]+)ms inst_all=([\d.]+)ms modules=(\d+)/g)].pop();
    if (m) {
      const [, gen, ic, ia, mods] = m;
      const reg = Number((logText.match(/\[wasm-jit stats\] registered=(\d+)/g) || []).pop()?.match(/(\d+)/)?.[1] ?? 0);
      console.log(`  instantiate: compile-thread ${ic}ms, ALL workers ${ia}ms (x${(Number(ia) / Math.max(1, Number(ic))).toFixed(2)})`
        + `  modules=${Number(mods).toLocaleString()}`
        + (reg ? ` = ${(Number(mods) / reg).toFixed(2)} per registered method` : "")
        + `   [gen ${gen}ms]`);
    } }
  if (r.v8gc) console.log(`  v8gc: ${r.v8gc.isolates} isolates, ${r.v8gc.totalGcs} GCs, summed peak cap ${r.v8gc.sumPeakCapMb} MiB` +
    `\n       worst isolate ${r.v8gc.worstIsolate.isolate}: cap ${r.v8gc.worstIsolate.peakCapMb} MiB = ${r.v8gc.worstIsolate.pctOfIsolateMax}% of the 4 GB per-isolate max, used ${r.v8gc.worstIsolate.peakUsedMb} MiB`);
  { const tableLen = row.tableLen;
    if (tableLen) {
      /* 12 B dispatch slot + ~4 B entries reference per entry. Capacity can exceed length, and
       * renderer thread count includes non-worker threads, so report only the per-worker floor. */
      const perWorkerMb = tableLen * 16 / 1048576;
      const workers = r.mem?.dedicatedWorkers;
      console.log(`  wasmTable: ${tableLen} entries, at least ${perWorkerMb.toFixed(1)} MiB/worker` +
        (workers != null ? `, >= ${(perWorkerMb * workers).toFixed(0)} MiB across ${workers} DedicatedWorkers` : ""));
    } }
  if (r.mem?.partitionsAll) {
    const pa = r.mem.partitionsAll;
    console.log(`  partitionAlloc: ${pa.count} roots at VmData peak, map ${pa.mapChargedMb} MiB,` +
      ` roots virtual ${pa.sumVirtualMb} MiB (gap ${pa.mapMinusRootVirtualMb}),` +
      ` committed (valid roots) ${pa.knownCommittedMb} MiB` +
      `${pa.unreliableCommittedRoots ? `, ${pa.unreliableCommittedRoots} invalid committed counter(s)` : ""}` +
      ` (${r.mem.partitionValidSamples}/${r.mem.samples} samples read)`);
    for (const [i, t] of pa.roots.slice(0, 8).entries())
      console.log(`       ${t.name || `root ${i + 1}`}: committed ${t.committedMb ?? "?"} MiB, allocated ${t.allocatedMb},` +
        ` direct ${t.directMappedMb}, super ${t.superPagesMb}${t.counterStatus === "valid" ? "" : ` [${t.counterStatus}]`}  ${t.addr}`);
  } else if (r.mem?.partitionError) {
    console.log(`  partitionAlloc: unavailable at VmData peak (${r.mem.partitionError})`);
  }
  if (row.live) console.log(`  tier: registered ${row.live.registered}  instantiations ${row.live.instantiations}`
    + ` (${row.live.instPerModule}/method)  instantiate ${row.live.instantiateMs} ms`);
  if (r.mem) console.log(`  mem: peak VmData ${r.mem.peakVmDataMb} MiB` +
    `  = ${r.mem.pctOfSandboxedLimit}% of the ${(r.mem.sandboxedLimitMb / 1024).toFixed(0)} GiB a SANDBOXED renderer gets` + `${r.mem.rlimitApplied ? ` (this run: ${r.mem.rlimitDataMb} MiB)` : " (this run: unlimited, --no-sandbox)"}` +
    `  rss ${r.mem.peakRssMb}  rwx ${r.mem.rwxMb}  wasm ${r.mem.wasmMb ?? "?"} MiB` +
    `${r.mem.pctOfWasm32 != null ? ` (${r.mem.pctOfWasm32}% of wasm32 cap)` : ""}` +
    `  threads ${r.mem.threads}, DedicatedWorkers ${r.mem.dedicatedWorkers ?? "?"}` +
    `\n       top: ${(r.mem.topBuckets || []).map((b) => `${b.bucket}=${b.mb}`).join(" ")}` +
    `\n       v8 pages: rwx code ${r.mem.v8PagesMb?.codeRwx ?? "?"} MiB, rw data ${r.mem.v8PagesMb?.dataRw ?? "?"} MiB` +
    `${r.mem.rwxHeapMb ? `, separate RWX [heap] ${r.mem.rwxHeapMb} MiB` : ""}` +
    `${r.mem.smapsAtVmDataPeakMb ? `\n       rss by mapping: ${Object.entries(r.mem.smapsAtVmDataPeakMb).map(([k,v]) => `${k}=${v.rss}`).join(" ")}` : ""}` +
    `\n       exec: ${r.mem.execCount} mappings, ${r.mem.execTotalMb} MiB  hist(n): ${JSON.stringify(r.mem.execHist)}  hist(MiB): ${JSON.stringify(r.mem.execHistMb)}`);
  if (r.workerProbe) {
    const p = r.workerProbe;
    console.log(`  worker probe: ${p.error || `${p.deltaWorkers}/${p.requested} new loaded workers; VmData ${p.vmData?.deltaMb} MiB`}`);
    if (!p.error) console.log(`       MiB/worker: VmData ${p.vmData.perWorkerMb}, stacks ${p.stacksAnon.perWorkerMb},` +
      ` PA map ${p.partitionAllocMap.perWorkerMb}, V8 sandbox ${p.v8Sandbox.perWorkerMb},` +
      ` V8 rwx ${p.v8RwxCode.perWorkerMb}, V8 rw data ${p.v8RwData.perWorkerMb}`);
  }
  if (acct) {
    console.log(`  sampled ${acct.wall.toFixed(1)}s over ${acct.samples} polls, ${acct.rows.length} threads`
      + (acctBounded ? `  (bounded by spawn83..spawnDone)` : `  !! NOT bounded by spawnDone — the world died; these numbers describe the death, not the phase`));
    console.log(`    ${"tid".padStart(8)} ${"comm".padEnd(16)} ${"run%".padStart(7)} ${"rqwait%".padStart(8)} ${"sleep%".padStart(7)} ${"ctx/s".padStart(8)} ${"us/slice".padStart(9)}`);
    for (const t of acct.rows.slice(0, 6))
      console.log(`    ${t.tid.padStart(8)} ${t.comm.padEnd(16)} ${t.runPct.toFixed(1).padStart(7)} ${t.rqwPct.toFixed(1).padStart(8)} ${t.sleepPct.toFixed(1).padStart(7)} ${t.ctxPerSec.toFixed(0).padStart(8)} ${(t.usPerSlice ?? 0).toFixed(1).padStart(9)}${t.late ? "  (mid-phase)" : ""}`);
    const busy = acct.rows.filter((t) => t.runPct > 20);
    const totCtx = acct.rows.reduce((a, t) => a + t.slices, 0);
    console.log(`    threads over 20% run: ${busy.length}   total run-seconds: ${acct.rows.reduce((a, t) => a + t.run, 0).toFixed(1)}`
      + `  (= ${(acct.rows.reduce((a, t) => a + t.run, 0) / acct.wall).toFixed(2)} cores)`
      + `   TOTAL ctx-switches/s: ${(totCtx / acct.wall).toFixed(0)}`);
    console.log(`    top wchan: ${acct.wchan.slice(0, 4).map(([k, n]) => `${k}=${n}`).join("  ")}`);
  }
}

/* ---- summary, split by mode: this is the whole point ---- */
console.log(`\n${"=".repeat(90)}`);
const byMode = (m) => results.filter((r) => r.mode === m && r.acct);
for (const m of ["fast", "mid", "stall"]) {
  const g = byMode(m).filter((r) => r.acctBounded); if (!g.length) continue;
  console.log(`\n${m.toUpperCase()}  n=${g.length}  world.generate median ${g.map((r) => r.generate).sort((a, b) => a - b)[g.length >> 1]}s`);
  for (const r of g) {
    const busy = r.acct.rows.filter((t) => t.runPct > 20);
    const cores = r.acct.rows.reduce((a, t) => a + t.run, 0) / r.acct.wall;
    const ctx = r.acct.rows.reduce((a, t) => a + t.slices, 0) / r.acct.wall;
    /* Workers only: chromium's own threads have their own cadence and would dilute the signal. */
    const dw = r.acct.rows.filter((t) => t.comm === "DedicatedWorker");
    const dwCtx = dw.reduce((a, t) => a + t.slices, 0) / r.acct.wall;
    const busyDw = dw.filter((t) => t.runPct > 5);
    const usSlice = busyDw.length
      ? busyDw.reduce((a, t) => a + (t.usPerSlice ?? 0), 0) / busyDw.length : 0;
    console.log(`  run ${r.run}: generate ${String(r.generate).padStart(6)}s  cores ${cores.toFixed(2)}`
      + `  ctx/s ${ctx.toFixed(0).padStart(6)}  workerCtx/s ${dwCtx.toFixed(0).padStart(6)}`
      + `  mean us/slice on busy workers ${usSlice.toFixed(0).padStart(5)}`);
  }
}
console.log(`\nRUN-BOUND vs SLEEP-BOUND is the reading: if the stall's busy threads are still ~99% run, the`);
console.log(`stall is CPU work and R67c stands. If they drop into sleep, the blocking story is back and`);
console.log(`wchan names the primitive. Cores-busy separates a third case: fewer threads doing the work.`);
writeFileSync(join(OUT, "summary.json"), JSON.stringify(results, null, 2));
