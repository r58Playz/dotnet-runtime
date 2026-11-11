// Drive the real workload — full Minecraft — to a fixed, repeatable script, and hand back monotonic
// phase boundaries that `perf record -k 1` timestamps can be sliced with.
//
// This is the Minecraft analogue of what `[bench] mean ms/step` gives the jbox2d harness. jbox2d is one
// number over one homogeneous window; Minecraft is three workloads that share nothing:
//
//   boot   ~150s  IKVM class loading + AOT stub resolution + resource reload. Almost no rendering.
//   world  ~170s  chunk generation / lighting / serialisation on the integrated server + chunk workers.
//   ingame  ~60s  steady-state render + server tick + chunk meshing.
//
// Averaging those together is how you get a profile in which nothing is above 2% and no change ever
// looks like it helped. So every consumer here works PER PHASE.
//
// WHY THIS DOES NOT USE THE mcsr HARNESS'S XTEST PATH
// --------------------------------------------------
// scratchpad/mcsr drives the game with XTEST keystrokes into a nested Xwayland display (nest-xwl.sh +
// xin.py), because it needs a REAL GL context: it is hunting crashes and a blank canvas makes the modded
// MCSR Ranked main screen unreadable, so a human has to be able to look at screenshots. That path needs
// three background services alive and a window that is mapped and focused, and verify.sh exists solely
// because "chromium is running" does not imply "input reaches the game".
//
// For MEASUREMENT none of that is needed. emscripten-glfw attaches its key handlers to `document`
// (statics/lib_emscripten_glfw3.js:461) and reads `e.code`, so a synthetic KeyboardEvent dispatched over
// CDP drives the game exactly like a real key -- which is already proven in-tree by jitbench.ts's
// benchWalk. That makes the whole run headless and unattended: no display, no window manager, no XTEST,
// nothing to focus.
//
// WHY PHASE DETECTION READS THE STDERR LOG RATHER THAN THE CDP CONSOLE
// -------------------------------------------------------------------
// mcsr/drive.mjs's header is blunt about it: CDP becomes unreliable once a world is loaded, because
// emscripten pthreads park in blocking wasm and Runtime.evaluate never returns for those targets (33 of
// 34 worker targets came back "unreachable"). --enable-logging=stderr --log-level=0 mirrors every
// console.* into a plain file that survives both CDP death and a wedged renderer, and that is the only
// diagnostic channel this app has that is trustworthy end to end.
//
// So: the log file is the authority for WHEN things happened, and CDP is used only for the two things a
// file cannot do -- injecting keys and reading counters -- both of which happen at points where CDP is
// known-good (menu time) or are allowed to fail (end-of-run counter probe).
//
// HOW LOG TIMESTAMPS BECOME MONOTONIC ONES
// ----------------------------------------
// perf stamps samples with CLOCK_MONOTONIC (`-k 1`, mandatory for jitdump). Chromium stamps log lines
// with local wall time. Converting needs one offset, and the naive way to get it -- "read a line, call
// hrtime" -- is biased: the first read of the log pulls in a whole batch of lines chromium wrote
// milliseconds apart, so anchoring on any single one overestimates its true time by the whole read lag.
//
// The estimator used instead is exact in the limit and never drifts. For every line, the true monotonic
// time is at most the time we read it, so `offset = mono_read - line_secs` is an UPPER bound on the true
// offset -- and the true offset is therefore the MINIMUM of that quantity over all lines seen. Chromium
// emits thousands of lines during a boot, so the running minimum converges within the first second and
// then only ever tightens.
import { spawn, execSync, spawnSync } from "child_process";
import { mkdirSync, rmSync, existsSync, openSync, readSync, writeSync, closeSync, statSync, writeFileSync, appendFileSync, readFileSync, readdirSync } from "fs";
import { join, resolve } from "path";
import { connect, attachPage } from "./browser.mjs";
import { readPartitionStats } from "./partition-alloc.mjs";
import { SEEDS, resolveSeed, seedFromArgs } from "./seeds.mjs";
import { serveUrl } from "./paths.mjs";
export { SEEDS, resolveSeed, seedFromArgs };

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/** hrtime.bigint() is CLOCK_MONOTONIC on Linux — the same clock `perf record -k 1` stamps samples with. */
export const monotonic = () => Number(process.hrtime.bigint()) / 1e9;

/* ------------------------------------------------------------------------------------------------ *
 * Markers
 * ------------------------------------------------------------------------------------------------ */

/*
 * Ordered list of the log lines that delimit real work. Each entry fires ONCE (first match wins), which
 * is what makes them usable as phase boundaries; `Preparing spawn area: N%` and friends repeat hundreds
 * of times and are deliberately not in here.
 *
 * Every one of these was read off a real 6-minute run (mcsr/logs/iter-sym1.log) rather than guessed:
 *
 *   PreInit...                          dotnet.js loaded, runtime created, IkvmWasm.PreInit entered
 *   dotnet initialized                  PreInit returned — IKVM is up, no Java has run yet
 *   Run...                              IkvmWasm.Run(version) entered — Minecraft's main() begins
 *   Setting user:                       Minecraft main() reached its arg parsing (58s of class loading)
 *   StateOutput State: title            the title screen exists
 *   FAILED_AUTH                         MCSR Ranked's auth attempt — the LAST thing boot does. The
 *                                       modded main screen is fully drawn a beat after this, which is
 *                                       why autoworld.sh sleeps before touching the keyboard.
 *   Loaded N recipes                    integrated server datapack load — PROOF Play was activated.
 *                                       Not "Reloading ResourceManager", which the client also logs at
 *                                       boot, and not StateOutput alone, which SpeedRunIGT emits a few
 *                                       seconds late (autoworld.sh burned two retries on that).
 *   Starting integrated minecraft server
 *   StateOutput State: generating       spawn-area generation begins
 *   Time elapsed: N ms                  ...and ends, with the server's own figure for it
 *   StateOutput State: inworld          the client is in the world and ticking
 */
export const MARKERS = [
  { name: "preinit", rx: /PreInit\.\.\./, label: "IkvmWasm.PreInit entered (runtime up)" },
  { name: "ikvmReady", rx: /dotnet initialized/, label: "PreInit returned (IKVM ready)" },
  { name: "javaMain", rx: /"Run\.\.\."/, label: "Minecraft main() entered" },
  { name: "userSet", rx: /Setting user:/, label: "main() past arg parsing" },
  /* The client's first resource reload starts when its loading overlay is first drawn: the first frame the
   * player sees. Every version logs it at boot (1.16.x "Reloading ResourceManager: Default", 26.2 "...: vanilla,
   * <packs>"); first match wins, so a later reload cannot move it. */
  { name: "firstFrame", rx: /Reloading ResourceManager/, label: "first frame (loading overlay, resource reload starts)" },
  { name: "titleState", rx: /StateOutput State: title/, label: "title screen exists" },
  { name: "titleReady", rx: /FAILED_AUTH/, label: "modded main screen up (boot done)" },
  { name: "datapack", rx: /Loaded \d+ recipes|Loaded \d+ advancements/, label: "Play activated (server datapack load)" },
  { name: "serverStart", rx: /Starting integrated minecraft server/, label: "integrated server starting" },
  { name: "generating", rx: /StateOutput State: generating/, label: "spawn-area generation begins" },
  /*
   * THE 83% KNEE, and it is the single most useful boundary in this file.
   *
   * `world.gen` (generating -> spawnDone) is TWO WORKLOADS, because the seed world is deliberately
   * pre-generated to ~83%: below the knee the server READS pre-generated chunks off disk (OPFS), above it
   * it GENERATES new ones (CPU + lighting). R12 identified the split and asked for it; it never landed, so
   * every `world.gen` number since has averaged a disk measurement with a CPU one.
   *
   * Splitting here localises this workload's largest single variance source. `replaylogs.mjs` re-derives
   * these phases from every archived log using the definitions BELOW, so the table is reproducible in ~9 s
   * and cannot drift from the code that produced it. Run 2026-09-07, 1,023 logs, 519 of which reached both
   * the knee and spawnDone, medians per mode:
   *
   *            n     world.gen   load 0->83%   generate 83->99%
   *   fast   417       48.1 s       16.3 s          30.2 s
   *   mid      6       92.2 s       12.6 s          79.8 s
   *   stall   96      111.9 s       18.2 s          90.8 s
   *
   * The entire +60.6 s of the stall mode is in GENERATE; load moves 16.3 -> 18.2 s, i.e. flat. So the OPFS
   * read path is NOT what varies here, which retires the standing "OPFS stalling" hypothesis for this phase
   * (it says nothing about the OPFS cost itself, which is real and separately unmeasured). Progress never
   * regresses -- 0 times in all 1,023 logs -- so it is not a spawn-search or chunk-prep retry either. The
   * 80-89% band alone is 63.2% of the phase (n=57), i.e. the knee is sharp rather than a gradual taper.
   *
   * `(?:...)` deliberately, NOT a capturing group: the pump reads `hit[1]` for `spawnDone`'s milliseconds
   * and a stray capture here would be harmless today but is exactly the kind of coupling that breaks when
   * someone reorders MARKERS.
   */
  { name: "spawn83", rx: /Preparing spawn area: (?:8[3-9]|9\d|100)%/, label: "pre-generated chunks loaded (83% knee)" },
  { name: "spawnDone", rx: /Time elapsed: (\d+) ms/, label: "spawn area prepared" },
  { name: "inworld", rx: /StateOutput State: inworld/, label: "in world, ticking (world load done)" },
  { name: "benchStart", rx: /\[mcw\] START/, label: "in-game measurement window opens" },
  { name: "benchEnd", rx: /\[mcw\] END/, label: "in-game measurement window closes" },
  /*
   * SAVE AND QUIT. Everything below here was outside every measurement this project has ever taken,
   * because `runSession` ended in `game.kill()` -- a SIGKILL -- so chunk serialisation, region-file
   * writes, `level.dat` and `flush` never ran. Evidence, 2026-09-07 over 1,023 archived logs: 503 BEGIN an
   * autosave (`Saving chunks for level`) and exactly 4 ever reach `All chunks are saved`. The four that did
   * took 0.6-2.3 s for all three dimensions, and all four were crash shutdowns rather than clean quits.
   *
   * Every regex here was read off those logs rather than guessed, AND THE ORDER IS NOT WHAT THE NAMES
   * SUGGEST. Verbatim from `mc-out/amp1-1.log`, one of the four runs that ever completed a save:
   *
   *   110714.907  Stopping server                        <- shutdown STARTS here, before any saving
   *   110714.918  Saving worlds
   *   110714.919  Saving chunks for level ... overworld
   *   110717.077  All chunks are saved (New World)        <- 2.16 s later: this is the whole cost
   *   110717.078  Saving chunks for level ... the_nether
   *   110717.080  All chunks are saved (DIM-1)
   *   110717.082  Saving chunks for level ... the_end
   *   110717.084  All chunks are saved (DIM1)
   *   110717.118  All chunks are saved (New World)        <- a second round, during teardown
   *
   * So `Stopping server` is the BEGINNING of shutdown, not the end. Defining `save.write` as
   * savingWorlds -> serverStopped therefore measured a NEGATIVE interval, which `replaylogs.mjs` caught
   * on the first run (-0.01 s) -- exactly the class of error that gate exists for.
   *
   * `All chunks are saved` is emitted once per dimension and then again per dimension during teardown, and
   * markers here are first-match-wins, so `chunksSaved` is the OVERWORLD's completion. That is the right
   * boundary anyway: the overworld took 2.16 s of the 2.27 s total and the other two dimensions ~3 ms
   * each, so first-match captures ~95% of the work and is stable, where "the last of six identical lines"
   * would need a counter and a quiescence rule.
   *
   * ALL FOUR ARE `afterQuit`, AND THAT IS NOT DEFENSIVE -- IT IS REQUIRED. The integrated server saves
   * chunks during WORLD LOAD, well before the measurement window: the first smoke run logged
   * `Saving chunks for level` at 306.5 s, between `spawnDone` (299.2 s) and `inworld` (309.9 s). Markers
   * here are first-match-wins, so without the gate that autosave consumes `savingChunks`, and had it also
   * emitted `All chunks are saved` it would have consumed the save phase's END marker too -- producing a
   * `save.write` interval spanning from world load to a quit minutes later. Arming them only once the
   * driver has asked to quit is what makes the phase mean what its name says.
   */
  { name: "shutdownStart", rx: /Stopping server\b/, label: "integrated server began shutting down", afterQuit: true },
  { name: "savingWorlds", rx: /Saving worlds/, label: "world save started", afterQuit: true },
  { name: "savingChunks", rx: /Saving chunks for level/, label: "first dimension's chunk save started", afterQuit: true },
  /* `after: savingWorlds`, because ESCAPE ITSELF SAVES. The probe caught it: pressing Escape logs
   * "Saving and pausing game..." followed by `Saving chunks for level` for all three dimensions, before
   * the Save-and-Quit button is ever pressed. That pause-save is inside the save phase and is fine, but if
   * it also emitted "All chunks are saved" it would consume this marker and `save.write`
   * (savingWorlds -> chunksSaved) would come out NEGATIVE -- the same inverted-interval bug the log
   * ordering already produced once in this file. Requiring savingWorlds first makes that impossible
   * rather than unlikely. */
  { name: "chunksSaved", rx: /All chunks are saved/, label: "overworld fully written (the bulk of the save)", afterQuit: true, after: "savingWorlds" },
];

/* MARKERS with a seed's own title / join / generation lines (lib/seeds.mjs). The names stay the same, so every
 * phase definition and every reader works across seeds; `titleState`, `spawn83` and the StateOutput lines are
 * MCSR's and simply never fire on another seed. */
export function markersFor (seed) {
  const s = resolveSeed(seed);
  return MARKERS.map((m) =>
    m.name === "titleReady" ? { ...m, rx: s.titleRx } :
    m.name === "inworld" ? { ...m, rx: s.joinRx, label: "in world (client joined)" } :
    m.name === "generating" && s.genRx ? { ...m, rx: s.genRx } : m);
}

/*
 * The three phases every consumer reports on, plus the boot sub-phases. `from`/`to` are marker names.
 *
 * The sub-phases matter more than they look: "boot is 150s" is not actionable, but "58s of it is between
 * Run... and Setting user:, i.e. pure IKVM class loading before a single frame is drawn" points at a
 * specific subsystem. Kept separate from PHASES so a caller can profile the three top-level windows and
 * still get a timing breakdown of boot for free.
 */
export const PHASES = [
  { name: "boot", from: "navigate", to: "titleReady", what: "runtime download+init, IKVM class loading, resource reload" },
  { name: "world", from: "datapack", to: "inworld", what: "chunk generation, lighting, spawn-area prep" },
  { name: "ingame", from: "benchStart", to: "benchEnd", what: "steady-state render + server tick + chunk meshing" },
  /* The write path, which nothing here has ever measured. Bounded by the driver's own quit keystrokes at
   * one end and the server's teardown at the other, so it contains the chunk serialisation and the OPFS
   * writes and nothing else. */
  { name: "save", from: "quitStart", to: "chunksSaved", what: "save and quit: chunk serialisation + OPFS writes" },
];

export const SUBPHASES = [
  /* Since the launcher redesign the runtime starts at Play, not at page load: `play` is the driver's click, so
   * these three are what a player waits through. boot.runtime then contains the launcher and is not comparable
   * with runs before 2026-10-21. */
  { name: "boot.launcher", from: "navigate", to: "play", what: "page load + launcher, until the driver presses Play" },
  { name: "boot.toframe", from: "play", to: "firstFrame", what: "Play -> first frame: runtime, IKVM, Fabric, Mixin, main() class loading, client init" },
  { name: "boot.reload", from: "firstFrame", to: "titleReady", what: "first frame -> title screen: resource reload under the loading overlay" },
  { name: "boot.runtime", from: "navigate", to: "preinit", what: "fetch dotnet.native.wasm, instantiate, create runtime" },
  { name: "boot.ikvminit", from: "preinit", to: "ikvmReady", what: "IkvmWasm.PreInit: assembly load, classpath scan" },
  { name: "boot.classload", from: "javaMain", to: "userSet", what: "Minecraft main() class loading / AOT stub resolution" },
  { name: "boot.resources", from: "userSet", to: "titleState", what: "resource pack reload, texture atlas, model bake" },
  { name: "boot.mainscreen", from: "titleState", to: "titleReady", what: "MCSR Ranked main screen construction" },
  { name: "world.datapack", from: "datapack", to: "generating", what: "datapack load, server construction" },
  /* Kept so a month of recorded `world.gen` figures stays comparable, but it is the SUM of the two below
   * and it is the one number in this harness that must never be averaged raw -- see `worldGenMode`. */
  { name: "world.gen", from: "generating", to: "spawnDone", what: "spawn-area chunk generation + lighting (load+generate)" },
  /* "load" as in "load the pre-generated chunks", NOT "the I/O phase". Measured 2026-09-07 via
   * `replaylogs.mjs` over 876 logs carrying OPFS_STATS: 83% of all counted OPFS reads land in BOOT (jars,
   * classes, assets) and `world.load` does fewer than one 8192-read checkpoint's worth. R12 labelled this
   * segment "LOADING pre-generated chunks off disk (OPFS)" and that framing is too strong -- whatever this
   * segment costs, it is not dominated by counted OPFS reads. */
  { name: "world.load", from: "generating", to: "spawn83", what: "bring up pre-generated chunks (0->83%)" },
  { name: "world.generate", from: "spawn83", to: "spawnDone", what: "GENERATE new chunks + lighting (CPU); carries the bimodal stall" },
  { name: "world.join", from: "spawnDone", to: "inworld", what: "client join, initial chunk upload/mesh" },
  /* Split because the two halves answer different questions: `menu` is how long the client takes to leave
   * the world and reach the shutdown path (GUI + disconnect), `write` is the actual serialisation. Only the
   * second is interesting, and averaging them would hide it behind menu latency the driver itself sets. */
  { name: "save.menu", from: "quitStart", to: "shutdownStart", what: "pause menu -> Save and Quit -> client left the world" },
  { name: "save.write", from: "savingWorlds", to: "chunksSaved", what: "overworld chunk serialisation to OPFS" },
];

/* ------------------------------------------------------------------------------------------------ *
 * World-generation mode
 * ------------------------------------------------------------------------------------------------ */

/*
 * `world.generate` IS BIMODAL, AND AVERAGING IT WITHOUT CLASSIFYING THE MODE IS MEANINGLESS.
 *
 * Measured 2026-09-07 over 312 runs with a matching `mc-out` json (many builds, ~1 month). Histogram of
 * `world.generate` in 10 s buckets -- this is what the thresholds below are derived from:
 *
 *   20-29s  81   30-39s 131   40-49s 28   50-59s 11   60-69s  6      <- fast cluster, 257 runs (82.4%)
 *   70-79s   3                                                       <- the valley, 3 runs (1.0%)
 *   80-89s  23   90-99s  16  100-109s  4  110-119s 5  130+  4        <- stall cluster, 52 runs (16.7%)
 *
 * Incidence 16.7% here, 18.5% when `replaylogs.mjs` applies these thresholds to all 519 usable archived
 * logs, and 17.1% independently in the 41 single-build `perf-<tag>/phases.json` captures. Three different
 * populations agreeing to within 2 points makes it a property of the workload, not of a binary. The stall cluster's INTERNAL
 * spread is 5.9% against the fast cluster's 46% -- it is tighter than the mode it perturbs, which is the
 * signature of a discrete event, not of contention or thermals. The offset is ADDITIVE and stable at
 * ~+57 s (fast 31.7 -> stall 88.6 median on one build; 45.97 -> 103.01 on `world.gen` in the perf corpus),
 * and progress never regresses (0 of 312 runs), so it is not a spawn-search or chunk-prep retry.
 *
 * WHAT IT IS NOT: it is not the OPFS read path. `world.load` is 16.5-29.9 s in BOTH modes (see the 83%
 * knee comment on MARKERS). It is also not any knob -- incidence is 13-37% across 25 different knobs, all
 * consistent with the base rate at those n -- and it leaves no log line behind: diffing the generate
 * segment of a stall run against a fast run of the same tag gives identical line kinds, with only the
 * timer-driven progress count scaling with duration.
 *
 * THRESHOLDS ARE ABSOLUTE SECONDS, and that is a deliberate trade with a known failure mode. Absolute
 * works because the offset is additive and the valley is empty over a month of builds; it will start
 * misclassifying if `world.generate` ever moves by more than ~35 s for a real reason, at which point the
 * histogram above has to be re-derived rather than the constants nudged. Env-overridable so a diagnostic
 * configuration can widen them without an edit. `mcab.mjs` ALSO applies a scale-free within-arm outlier
 * test (>1.5x the arm median), because that one cannot go stale; read the two together.
 */
import { classifyLine, classifyRun, summariseFaults, STACK_FRAME_RX, TERMINAL, ENV } from "./verdict.mjs";

/*
 * APP OUTPUT vs CHROMIUM NOISE.
 *
 * Liveness used to be "any byte in the chromium stderr file", which includes gcm/registration_request
 * and gpu_blocklist errors that chromium keeps emitting for MINUTES after the page is dead. So a wedged
 * renderer inside a chattering browser never tripped the stall detector, and counting that noise as
 * progress once produced a confident "it recovered" that was exactly backwards.
 *
 * The app reaches this file through src/dotnet/log.ts's DOM proxy, i.e. as a chromium CONSOLE line with
 * a localhost source. MEASURED over 60 large logs: 4,000-6,500 app lines against 6-14 noise lines, so
 * the split is unambiguous.
 */
/* IKVM logs every class it loads; a line naming a type is not a line THROWING it. Shared by every
 * type-name matcher in this file -- see the note at bootAbortSeen for what it cost to omit. */
export const CLASSLOAD_NOISE_RX = /\[IkvmClassLoader\]|loaded from URLClassLoader/;

export const APP_LINE_RX = /:CONSOLE:\d+\]|source: https?:\/\/localhost:\d+\//;

export const WORLD_GEN_FAST_MAX = Number(process.env.MC_WGEN_FAST_MAX || 70);
export const WORLD_GEN_STALL_MIN = Number(process.env.MC_WGEN_STALL_MIN || 80);

/* MEMORY-INFRA, DRIVEN OVER CDP AND STREAMED TO A FILE.
 *
 * WHY NOT STARTUP TRACING, which was tried first and does not work here (R233 addendum 2):
 * `--trace-startup=...memory-infra` installs a 10 s periodic detailed trigger, and it fires exactly
 * ONCE, at t=0. Over a 240 s trace every dump carried timestamp 0.0 s, and the renderer running the
 * game never appeared at all -- it is created after browser startup, so it was never registered. The
 * richest dump totalled 75.9 MiB against a renderer measured at 9,532 MiB of VmData.
 *
 * SO TRIGGER THE DUMPS EXPLICITLY. `Tracing.requestMemoryDump` forces one on demand across every
 * process, and -- this is the part that makes it viable -- its RESPONSE is just a dump GUID and a
 * success flag. The payload goes into the trace, not into the CDP reply, so it cannot hit the
 * `RangeError: Invalid string length` that killed cdpmem.mjs. That error was never about CDP: it is
 * node's max string length while materialising the payload, and reading a 919 MB trace off DISK
 * throws the identical error. `transferMode: ReturnAsStream` + chunked `IO.read` keeps every piece
 * small at both ends.
 *
 * BROWSER-LEVEL, so these go WITHOUT a sessionId: Tracing and IO are browser domains, and a
 * page-session send would be refused.
 *
 * This is NOT the native allocation sampler and must not be confused with it. That one captures a
 * stack per sampled allocation and walks into a PROT_NONE guard page, because V8 JIT and wasm frames
 * keep no frame-pointer chain (see cdpmem.mjs). memory-infra's providers only report SIZES and never
 * unwind, so JSPI's stack juggling cannot reach them. */
/* Markers chosen for main-thread AVAILABILITY, not for interest: each is emitted from JS on the main
 * thread, so the event loop is provably alive at that instant. `titleReady` is the first one after the
 * boot-long wasm call ends. */
const SHOT_MARKERS = new Set(["titleReady", "inworld", "benchStart", "benchEnd"]);
const MEMTRACE_MARKERS = new Set(["titleReady", "spawnDone", "inworld", "benchStart", "benchEnd"]);

export async function startMemTrace (conn) {
  const r = await conn.send("Tracing.start", {
    transferMode: "ReturnAsStream",
    streamFormat: "json",
    traceConfig: {
      recordMode: "recordUntilFull",
      /* includedCategories ALONE DOES NOT RESTRICT ANYTHING -- chromium keeps recording its default
       * set alongside it. Measured: a trace asking only for memory-infra came back 1 GB of `toplevel`,
       * `base`, `gpu`, `sequence_manager` and `blink` events, overflowed the ring buffer ("DATA LOSS"),
       * and from then on every Tracing.requestMemoryDump was REFUSED -- so the missing dumps were never
       * about the renderer being busy, they were about the buffer being full of events we did not ask
       * for. `excludedCategories: ["*"]` is what actually makes it memory-infra only. */
      includedCategories: ["disabled-by-default-memory-infra"],
      excludedCategories: ["*"],
    },
  });
  return !r.__error;
}

/* `detailed` is what emits the per-allocator tree; `light` gives process totals only. */
export async function requestMemDump (conn, levelOfDetail = "detailed") {
  const r = await conn.send("Tracing.requestMemoryDump", { deterministic: false, levelOfDetail });
  return r && !r.__error && r.success !== false;
}

export async function endMemTrace (conn, outPath, log = () => {}) {
  /* Subscribe BEFORE ending: tracingComplete can arrive before the `Tracing.end` reply. */
  let resolveDone;
  const done = new Promise((res) => { resolveDone = res; });
  conn.on((m) => { if (m.method === "Tracing.tracingComplete") resolveDone(m.params || {}); });
  await conn.send("Tracing.end");
  const params = await Promise.race([done, new Promise((r) => setTimeout(() => r(null), 120000))]);
  if (!params) { log("  !! memtrace: no tracingComplete within 120s"); return null; }
  if (!params.stream) { log(`  !! memtrace: tracingComplete carried no stream (dataLossOccurred=${params.dataLossOccurred})`); return null; }
  const fd = openSync(outPath, "w");
  let total = 0;
  try {
    for (;;) {
      const r = await conn.send("IO.read", { handle: params.stream, size: 1 << 20 });
      if (!r || r.__error) { log(`  !! memtrace: IO.read failed: ${r?.__error?.message}`); break; }
      if (r.data) {
        const b = r.base64Encoded ? Buffer.from(r.data, "base64") : Buffer.from(r.data, "utf8");
        writeSync(fd, b); total += b.length;
      }
      if (r.eof) break;
    }
  } finally { closeSync(fd); }
  try { await conn.send("IO.close", { handle: params.stream }); } catch { }
  log(`  memtrace: wrote ${(total / 1048576).toFixed(1)} MiB -> ${outPath}${params.dataLossOccurred ? " (DATA LOSS -- buffer overflowed)" : ""}`);
  return outPath;
}

/* V8's JS HEAP, PER ISOLATE -- the ceiling that actually killed a run (R233).
 *
 * `--trace-gc` prints one line per collection per isolate:
 *   [pid:0xISOLATE] NNN ms: Mark-Compact 1500.0 (2048.0) -> 1200.0 (2048.0) MB, ...
 * i.e. used (capacity) before -> used (capacity) after. The interesting number is how close CAPACITY
 * gets to the 4 GB `kDefaultMaxHeapSize`, because that is what `AllocateRawSlowPath` fails against.
 *
 * WHY THIS AND NOT /proc: the JS heap has no anon-name bucket of its own -- it lives inside
 * `[anon:v8-sandbox]` alongside wasm linear memory and every ArrayBuffer, so /proc cannot separate the
 * 4 GB isolate ceiling from the 4 GB wasm ceiling. They are different limits that happen to share a
 * number, and conflating them is how the earlier read of `heapMb` as "73% of the JS heap" went wrong:
 * `heapMb` is wasmMemory.buffer.byteLength, which is the WASM cap, not this one.
 *
 * PER ISOLATE is load-bearing: the OOM was on a DedicatedWorker, not the main thread, and each worker
 * gets its OWN 4 GB budget. A main-thread-only reading says nothing about the one that died. */
/* NOT anchored with ^: these arrive wrapped in chromium's own log prefix, so an anchored pattern
 * matches nothing and reads exactly like "V8 emitted no GC traces". The kind can contain spaces
 * ("Incremental Mark-Compact"), hence [A-Za-z- ]+. Verified against 675 real lines before use. */
const V8GC_RX = /\[(\d+):(0x[0-9a-f]+)\]\s+(\d+) ms: ([A-Za-z-][A-Za-z- ]*?)\s+([\d.]+) \(([\d.]+)\) -> ([\d.]+) \(([\d.]+)\) MB/;

export function summariseV8Gc (lines) {
  const byIsolate = new Map();
  for (const l of lines) {
    const m = V8GC_RX.exec(l);
    if (!m) continue;
    const key = m[2];
    const cur = byIsolate.get(key) || { isolate: key, gcs: 0, peakUsedMb: 0, peakCapMb: 0, kinds: {} };
    cur.gcs++;
    cur.kinds[m[4]] = (cur.kinds[m[4]] || 0) + 1;
    cur.peakUsedMb = Math.max(cur.peakUsedMb, Number(m[5]), Number(m[7]));
    cur.peakCapMb = Math.max(cur.peakCapMb, Number(m[6]), Number(m[8]));
    byIsolate.set(key, cur);
  }
  if (!byIsolate.size) return null;
  const all = [...byIsolate.values()].sort((a, b) => b.peakCapMb - a.peakCapMb);
  const worst = all[0];
  return {
    isolates: all.length,
    totalGcs: all.reduce((n, i) => n + i.gcs, 0),
    /* Summed capacity across isolates is what the PROCESS pays; the max is what decides whether any one
     * isolate hits its own 4 GB wall. Both matter and they fail differently. */
    sumPeakCapMb: +all.reduce((n, i) => n + i.peakCapMb, 0).toFixed(1),
    worstIsolate: { isolate: worst.isolate, gcs: worst.gcs, peakUsedMb: worst.peakUsedMb,
                    peakCapMb: worst.peakCapMb,
                    pctOfIsolateMax: +(100 * worst.peakCapMb / (V8_ISOLATE_HEAP_MAX / 1048576)).toFixed(1),
                    kinds: worst.kinds },
    top: all.slice(0, 5).map((i) => ({ isolate: i.isolate, gcs: i.gcs, peakCapMb: i.peakCapMb })),
  };
}

/* PartitionAlloc root counters come from page metadata, with self-pointer and linked-list
 * validation. This works across Chromium updates without build-specific symbol addresses. */
export { readPartitionStats };

/* RENDERER MEMORY, SAMPLED FROM /proc -- three ceilings, and we are near two of them.
 *
 * WHY NATIVE AND NOT CDP. The renderer has to be alive AND cooperative to answer a CDP memory query,
 * and the one that matters most is asked exactly when it is neither: a memory-infra dump on this
 * workload returns a payload big enough to throw `RangeError: Invalid string length` in the node
 * client, and `measureUserAgentSpecificMemory` barely ever resolves. /proc needs nothing from the
 * page, cannot be starved by a wedged renderer, and keeps working right up to the kill.
 *
 * THE THREE CEILINGS, each verified in source rather than assumed:
 *
 *  1. V8 JS HEAP, 4 GB PER ISOLATE. `Heap::OldGenerationSizeFromPhysicalMemory` is physical/2 on
 *     64-bit, clamped to `kDefaultMaxHeapSize` = 4 GB (v8/src/heap/heap.h:322), and additionally
 *     capped by the pointer-compression cage (`kAllocatorLimitOnMaxOldGenerationSize`, heap.h:329).
 *     THIS IS THE ONE THAT KILLED A RUN: the 2026-09-08 coredump faults in
 *     `Factory::NewWasmDispatchTable` <- `WasmTableObject::Grow` <- `WebAssemblyTableGrow`, i.e. V8
 *     could not allocate the dispatch table while growing our function table, and turned that into
 *     `V8::FatalProcessOutOfMemory`. Note the table itself is small -- 12 B/entry
 *     (`WasmDispatchTable::kEntrySize`, wasm-objects.h:981) and growth is exponential doubling
 *     (wasm-objects.cc:2453), so ~1.2 M entries is ~15 MB and ~45 MB transient while old and new are
 *     both live. It was the STRAW, not the load. It is also NOT visible in /proc as its own bucket,
 *     so treat the v8 buckets below as a proxy and the crash signature as the real alarm.
 *  2. RLIMIT_DATA. `sandbox_linux.cc:476 GetProcessDataSizeLimit` steps on physical RAM:
 *     >32G -> 32 GiB, >16G -> 16 GiB, else 8 GiB. This box is 15.29 GiB, which is NOT > 16 GiB, so it
 *     lands in the 8 GiB bucket -- but the harness already passes
 *     `--enable-features=HigherRendererMemoryLimit`, which doubles it (`limit *= 2`, :505). Read the
 *     real value from /proc rather than recomputing it.
 *  3. WASM32 LINEAR MEMORY, ~4 GB. `MAXIMUM_MEMORY=4294901760` in IkvmWasm.csproj. This is what
 *     `heapMb` (wasmMemory.buffer.byteLength) measures, and it sits around 2.9 GB = ~71% -- so it is
 *     the second ceiling we are close to, and a DIFFERENT one from (1).
 *
 * WHAT `charged` MEANS. Since Linux 4.7 RLIMIT_DATA is enforced against private writable mappings,
 * not just brk, and the kernel's own rule (mm/internal.h is_data_mapping) is
 * `(flags & (VM_WRITE|VM_SHARED|VM_STACK)) == VM_WRITE` -- writable AND private AND not the growsdown
 * stack. `VmData` in /proc/<pid>/status is the kernel's own running total under exactly that rule and
 * is the number it compares to the limit, so VmData is authoritative and the per-bucket split below
 * (which re-derives the rule over /proc/<pid>/maps) is only for ATTRIBUTION. If the two disagree,
 * believe VmData.
 */
const WASM32_MAX_BYTES = 4294901760;          /* MAXIMUM_MEMORY, loader/IkvmWasm.csproj */
const V8_ISOLATE_HEAP_MAX = 4 * 1024 ** 3;    /* kDefaultMaxHeapSize, v8/src/heap/heap.h:322 */
/* What a SANDBOXED renderer would get on this box: GetProcessDataSizeLimit's step function lands 15.29
 * GiB of RAM in the `else 8 GiB` bucket (sandbox_linux.cc:476-500), and the harness's
 * --enable-features=HigherRendererMemoryLimit would double it (:502-506) -- but only if the sandbox ran
 * at all. 8 GiB is the honest bar to hold: it is what the workload gets in a normal browser, and
 * HigherRendererMemoryLimit is a non-default feature that may not be safe to rely on. */
const SANDBOXED_RENDERER_LIMIT = 8 * 1024 ** 3;

/* The renderer is the process under this profile with the most threads -- the browser process also
 * matches the profile path on its own cmdline, and picking it would measure the wrong thing. */
function findRendererPid (profile) {
  let best = null, bestN = 0;
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    let cl = "";
    try { cl = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
    if (!cl.includes(profile) || !cl.includes("--type=renderer")) continue;
    let n = 0;
    try { n = readdirSync(`/proc/${d}/task`).length; } catch { continue; }
    if (n > bestN) { bestN = n; best = Number(d); }
  }
  return best;
}

/* One sample. Cheap by construction: status and limits are small, and maps is a text walk with NO
 * page-table cost (that is smaps, which would add a walk over ~11k mappings per sample). */
function sampleProcMem (pid) {
  const kv = (txt, key) => {
    const m = txt.match(new RegExp(`^${key}:\\s+(\\d+) kB`, "m"));
    return m ? Number(m[1]) * 1024 : null;
  };
  let status, limits, maps;
  try {
    status = readFileSync(`/proc/${pid}/status`, "utf8");
    limits = readFileSync(`/proc/${pid}/limits`, "utf8");
    maps   = readFileSync(`/proc/${pid}/maps`, "utf8");
  } catch { return null; }

  const lim = limits.match(/^Max data size\s+(\S+)\s+(\S+)/m);
  const rlimitData = lim ? (lim[1] === "unlimited" ? Infinity : Number(lim[1])) : null;

  /* Re-derive the kernel's charge rule for ATTRIBUTION only; VmData above is the authority. */
  const buckets = {};
  const v8Pages = { codeRwx: 0, codeRx: 0, dataRw: 0, reservedNoAccess: 0, other: 0, mappings: 0 };
  let chargedFromMaps = 0, rwx = 0, rwxHeap = 0, xCount = 0, xTotal = 0, sandboxMaxVma = 0;
  const xHist = {}, xHistBytes = {};
  for (const line of maps.split("\n")) {
    if (!line) continue;
    const m = /^([0-9a-f]+)-([0-9a-f]+)\s+([rwxps-]{4})\s+[0-9a-f]+\s+\S+\s+\d+(?:\s+(.*))?$/.exec(line);
    if (!m) continue;
    const size = parseInt(m[2], 16) - parseInt(m[1], 16), perms = m[3];
    if (!(size > 0)) continue;
    const path = m[4] || "[anon]";
    if (path === "[anon:v8]") {
      v8Pages.mappings++;
      if (perms[2] === "x" && perms[1] === "w") v8Pages.codeRwx += size;
      else if (perms[2] === "x") v8Pages.codeRx += size;
      else if (perms[1] === "w" && perms[3] === "p") v8Pages.dataRw += size;
      else if (perms.slice(0, 3) === "---") v8Pages.reservedNoAccess += size;
      else v8Pages.other += size;
    }
    /* Executable VMA sizes help identify large reservations and spikes. They do not count V8 code
     * spaces: adjacent per-module reservations with equal permissions merge into one VMA (R345).
     * The unsandboxed harness maps wasm code rwx here. */
    if (perms[2] === "x") {
      if (perms[1] === "w") { rwx += size; if (path === "[heap]") rwxHeap += size; }
      xCount++; xTotal += size;
      const b = size <= 4096 ? "<=4k" : size <= 65536 ? "<=64k" : size <= 1048576 ? "<=1M"
              : size <= 16777216 ? "<=16M" : ">16M";
      xHist[b] = (xHist[b] || 0) + 1;
      xHistBytes[b] = (xHistBytes[b] || 0) + size;
    }
    if (perms[1] !== "w" || perms[3] !== "p") continue;          /* not writable-private -> not charged */
    if (path === "[stack]") continue;                            /* VM_STACK is excluded by the rule */
    chargedFromMaps += size;
    /* The wasm linear memory is ONE contiguous rw VMA inside the sandbox (2.6 GB against <=27 MiB for
     * the next largest in R373's snapshot), so the largest one tracks it per sample without CDP. */
    if (path === "[anon:v8-sandbox]" && size > sandboxMaxVma) sandboxMaxVma = size;
    /* Keep the anon-name tags Chrome/V8 set via PR_SET_VMA_ANON_NAME; lump files by basename. */
    const key = path.startsWith("[anon:") ? path : path.startsWith("[") ? path : path.split("/").pop();
    buckets[key] = (buckets[key] || 0) + size;
  }
  /* Diagnostic-only resident split. smaps walks page tables and is much more expensive than maps;
   * MC_MEM_SMAPS=1 opts into that cost when virtual reservations need a physical-memory check. */
  const smaps = process.env.MC_MEM_SMAPS === "1" ? (() => {
    let src;
    try { src = readFileSync(`/proc/${pid}/smaps`, "utf8"); } catch { return null; }
    const out = {};
    let row = null;
    for (const line of src.split("\n")) {
      if (/^[0-9a-f]+-[0-9a-f]+ /.test(line)) {
        const m = /^([0-9a-f]+)-([0-9a-f]+)\s+([rwxps-]{4})\s+[0-9a-f]+\s+\S+\s+\d+(?:\s+(.*))?$/.exec(line);
        const path = m?.[4] || "[anon]";
        const perms = m?.[3];
        const name = path === "[anon:v8]" ? (perms?.[2] === "x" ? "v8Code" : "v8Data") : path;
        row = ["v8Code", "v8Data", "[anon:v8-sandbox]", "[anon:partition_alloc]", "[anon]", "[anon:v8-pointer-table]", "[heap]"].includes(name)
          ? (out[name] ||= { rss: 0, privateDirty: 0, swap: 0 }) : null;
        continue;
      }
      if (!row) continue;
      const m = /^(Rss|Private_Dirty|Swap):\s+(\d+) kB/.exec(line);
      if (m) row[{ Rss: "rss", Private_Dirty: "privateDirty", Swap: "swap" }[m[1]]] += +m[2] * 1024;
    }
    return out;
  })() : null;
  return {
    t: Date.now(), pid,
    vmData: kv(status, "VmData"), vmRSS: kv(status, "VmRSS"), vmSize: kv(status, "VmSize"),
    vmSwap: kv(status, "VmSwap"), threads: Number((status.match(/^Threads:\s+(\d+)/m) || [])[1] || 0),
    rlimitData, chargedFromMaps, rwx, rwxHeap, buckets, xCount, xTotal, xHist, xHistBytes, sandboxMaxVma,
    /* Isolated: the partition read walks a live process's memory and is the most fragile part of the
     * sample. It must never cost the /proc numbers, which are the authoritative ones. */
    dedicatedWorkers: (() => {
      let n = 0;
      try {
        for (const tid of readdirSync(`/proc/${pid}/task`)) {
          try { if (readFileSync(`/proc/${pid}/task/${tid}/comm`, "utf8").trim() === "DedicatedWorker") n++; }
          catch { /* thread exited between enumeration and read */ }
        }
        return n;
      } catch { return null; }
    })(),
    v8Pages, smaps,
    partitions: (() => { try { return readPartitionStats(pid, maps); } catch (e) { return { __error: e.message }; } })(),
  };
}

/* Peak-oriented: a mean over a run that ends in a crash is not the interesting number. Reports the
 * worst sample, its bucket split, and the headroom against each ceiling so a future run can be warned
 * about before it dies rather than after. */
export function summariseMem (samples, wasmBytes = null) {
  const ok = samples.filter(Boolean);
  if (!ok.length) return null;
  const peak = ok.reduce((a, b) => ((b.vmData || 0) > (a.vmData || 0) ? b : a));
  const top = Object.entries(peak.buckets || {}).sort((a, b) => b[1] - a[1]).slice(0, 8)
    .map(([k, v]) => ({ bucket: k, mb: +(v / 1048576).toFixed(1) }));
  const pct = (a, b) => (a != null && b && Number.isFinite(b) ? +(100 * a / b).toFixed(1) : null);
  const M = (v) => (v == null ? null : +(v / 1048576).toFixed(1));
  return {
    samples: ok.length,
    /* EVERY sample, compact, tagged with the last marker seen: the peak alone cannot say WHEN a bucket
     * grew (boot vs world load vs in-game) or whether it is a ratchet or a transient. */
    series: ok.map((s) => ({ t: s.tRel ?? null, mark: s.mark ?? null, vd: M(s.vmData), rss: M(s.vmRSS),
      swap: M(s.vmSwap), sb: M(s.buckets?.["[anon:v8-sandbox]"]), wasm: M(s.sandboxMaxVma),
      pa: M(s.buckets?.["[anon:partition_alloc]"]), v8x: M(s.v8Pages?.codeRwx), v8rw: M(s.v8Pages?.dataRw),
      heapx: M(s.rwxHeap), anon: M(s.buckets?.["[anon]"]), pt: M(s.buckets?.["[anon:v8-pointer-table]"]),
      thr: s.threads, dw: s.dedicatedWorkers,
      paAlloc: s.partitions?.__all ? M(s.partitions.__all.reduce((n, r) => n + (r.allocated || 0), 0)) : null,
      paVirt: s.partitions?.__all ? M(s.partitions.__allTotalVirtual) : null,
      /* per named root: [virtual (superpages + direct maps), direct maps, allocated] */
      paRoots: s.partitions?.__all ? Object.fromEntries(s.partitions.__all.filter((r) => r.name).map((r) =>
        [r.name, [M((r.superPages || 0) + (r.directMapped || 0)), M(r.directMapped || 0), M(r.allocated || 0)]])) : null })),
    peakVmDataMb: peak.vmData != null ? +(peak.vmData / 1048576).toFixed(1) : null,
    chargedFromMapsMb: +(peak.chargedFromMaps / 1048576).toFixed(1),
    vmDataMinusMapsMb: peak.vmData != null ? +((peak.vmData - peak.chargedFromMaps) / 1048576).toFixed(1) : null,
    peakRssMb: peak.vmRSS != null ? +(peak.vmRSS / 1048576).toFixed(1) : null,
    rwxMb: +(peak.rwx / 1048576).toFixed(1),
    rwxHeapMb: +(peak.rwxHeap / 1048576).toFixed(1),
    /* Executable mapping count, total, and size histogram. Adjacent code spaces can merge into one
     * mapping; this is a VMA histogram, not a module census. */
    execCount: peak.xCount, execTotalMb: +(peak.xTotal / 1048576).toFixed(1),
    execHist: peak.xHist, execHistMb: Object.fromEntries(
      Object.entries(peak.xHistBytes || {}).map(([k, v]) => [k, +(v / 1048576).toFixed(1)])),
    threads: peak.threads,
    dedicatedWorkers: peak.dedicatedWorkers,
    v8PagesMb: Object.fromEntries(Object.entries(peak.v8Pages || {}).map(([k, v]) =>
      [k, k === "mappings" ? v : +(v / 1048576).toFixed(1)])),
    smapsAtVmDataPeakMb: peak.smaps ? Object.fromEntries(Object.entries(peak.smaps).map(([name, st]) =>
      [name, Object.fromEntries(Object.entries(st).map(([k, v]) => [k, +(v / 1048576).toFixed(1)]))])) : null,
    rlimitDataMb: Number.isFinite(peak.rlimitData) ? +(peak.rlimitData / 1048576).toFixed(0) : null,
    pctOfRlimit: pct(peak.vmData, peak.rlimitData),
    /* THE HARNESS RUNS `--no-sandbox`, SO RLIMIT_DATA IS NEVER APPLIED TO OUR RENDERER and the field
     * above reads `unlimited`. That is a property of the harness, not of the workload: chromium lowers
     * RLIMIT_DATA from SandboxLinux::LimitAddressSpace, which is part of sandbox initialisation, so an
     * unsandboxed renderer keeps the inherited (unlimited) value. Measuring only `pctOfRlimit` would
     * therefore report "fine" forever while the same page dies immediately in a normal browser -- which
     * is exactly what happened: a sandboxed Chrome OOMed on this workload while the harness did not.
     * So ALWAYS also report against the limit a REAL renderer would get on this box. */
    rlimitApplied: Number.isFinite(peak.rlimitData),
    sandboxedLimitMb: SANDBOXED_RENDERER_LIMIT / 1048576,
    pctOfSandboxedLimit: pct(peak.vmData, SANDBOXED_RENDERER_LIMIT),
    /* The two ceilings that are NOT RLIMIT_DATA. wasmPct is the one `heapMb` already tracked; the V8
     * isolate heap has no /proc bucket of its own, so it is reported as a constant to compare against
     * rather than as a measurement -- see the header. */
    wasmMb: wasmBytes != null ? +(wasmBytes / 1048576).toFixed(1) : null,
    pctOfWasm32: pct(wasmBytes, WASM32_MAX_BYTES),
    v8IsolateHeapMaxMb: V8_ISOLATE_HEAP_MAX / 1048576,
    topBuckets: top,
    /* Kept for readers of archived symbol-based runs. New versions use partitionsAll: the symbols
     * disappeared from the installed browser, while the self-validating root list still works. */
    partitions: (() => {
      const acc = {};
      for (const smp of ok) {
        for (const [name, st] of Object.entries(smp.partitions || {})) {
          if (name.startsWith("__")) continue;   // __all / __allTotal* are not per-partition entries
          const cur = acc[name] || (acc[name] = { committedMb: 0, maxCommittedMb: 0, maxAllocatedMb: 0, superPagesMb: 0 });
          cur.committedMb = Math.max(cur.committedMb, +(st.committed / 1048576).toFixed(1));
          cur.maxCommittedMb = Math.max(cur.maxCommittedMb, +(st.maxCommitted / 1048576).toFixed(1));
          cur.maxAllocatedMb = Math.max(cur.maxAllocatedMb, +(st.maxAllocated / 1048576).toFixed(1));
          cur.superPagesMb = Math.max(cur.superPagesMb, +(st.superPages / 1048576).toFixed(1));
        }
      }
      return Object.keys(acc).length ? acc : null;
    })(),
    /* Every PartitionRoot read at the SAME /proc sample as peak VmData. Root names are deliberately
     * absent until a symbol or controlled allocation identifies them; address/rank is an identity
     * inside one renderer, not a stable label across runs. */
    partitionsAll: (() => {
      const roots = peak.partitions?.__all;
      if (!roots) return null;
      return {
        count: roots.length,
        nameSource: peak.partitions.__nameSource,
        enumeratedRoots: peak.partitions.__enumeratedRoots,
        rejectedRoots: peak.partitions.__rejectedRoots,
        unreliableCommittedRoots: roots.filter(r => r.counterStatus !== "valid").length,
        sumMaxCommittedMb: +(roots.reduce((n, r) => n + (r.maxCommitted || 0), 0) / 1048576).toFixed(1),
        /* VmData charges writable reservations; committed pages measure physical backing. Keep
         * virtual (superpage + direct-map) and committed comparisons separate. */
        mapChargedMb: +(peak.partitions.__mapCharged / 1048576).toFixed(1),
        sumCommittedMb: roots.every(r => r.counterStatus === "valid") ?
          +(peak.partitions.__allTotalCommitted / 1048576).toFixed(1) : null,
        knownCommittedMb: +(peak.partitions.__allTotalCommitted / 1048576).toFixed(1),
        mapMinusRootCommittedMb: roots.every(r => r.counterStatus === "valid") ?
          +(peak.partitions.__mapMinusRootCommitted / 1048576).toFixed(1) : null,
        sumVirtualMb: +(peak.partitions.__allTotalVirtual / 1048576).toFixed(1),
        mapMinusRootVirtualMb: +(peak.partitions.__mapMinusRootVirtual / 1048576).toFixed(1),
        layout: peak.partitions?.__layout || null,
        roots: roots
          .map((r) => ({ addr: r.addr, name: r.name, counterStatus: r.counterStatus,
                         rawCommittedHex: r.rawCommittedHex, rawMaxCommittedHex: r.rawMaxCommittedHex,
                         maxCommittedMb: r.maxCommitted == null ? null : +(r.maxCommitted / 1048576).toFixed(1),
                         committedMb: r.committed == null ? null : +(r.committed / 1048576).toFixed(1),
                         superPagesMb: +(r.superPages / 1048576).toFixed(1),
                         directMappedMb: +(r.directMapped / 1048576).toFixed(1),
                         allocatedMb: +(r.allocated / 1048576).toFixed(1) }))
          .sort((a, b) => (b.superPagesMb + b.directMappedMb) - (a.superPagesMb + a.directMappedMb)),
      };
    })(),
    partitionError: peak.partitions?.__error || null,
    partitionValidSamples: ok.filter(s => s.partitions?.__all).length,
  };
}

/* Add idle Emscripten pthread workers AFTER the benchmark window, in the SAME renderer. Each worker
 * loads the shared main WasmModule and memory, so this prices a real initialized worker and its V8
 * isolate/table/stack without changing the Minecraft work whose peak was just measured. JIT modules
 * admitted later on that worker are intentionally outside this idle-worker price. */
async function measureIdlePthreadCost (conn, sid, pid, count) {
  const before = sampleProcMem(pid);
  if (!before) return { requested: count, error: 'renderer disappeared before probe' };
  const expression = `(async () => {
    const p = globalThis.wasm?.Module?.PThread;
    if (!p?.allocateUnusedWorker || !p?.loadWasmModuleToWorker) throw new Error('PThread loader unavailable');
    const beforePool = p.unusedWorkers.length;
    for (let i = 0; i < ${count}; i++) {
      p.allocateUnusedWorker();
      await p.loadWasmModuleToWorker(p.unusedWorkers[p.unusedWorkers.length - 1]);
    }
    return { beforePool, afterPool: p.unusedWorkers.length, loaded: ${count} };
  })()`;
  let response;
  let timeout;
  try {
    response = await Promise.race([
      conn.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, sid),
      new Promise((_, reject) => { timeout = setTimeout(() => reject(new Error('worker load exceeded 90 s')), 90000); }),
    ]);
  } catch (e) { return { requested: count, beforeWorkers: before.dedicatedWorkers, error: e.message }; }
  finally { clearTimeout(timeout); }
  if (response?.exceptionDetails || !response?.result?.value?.loaded)
    return { requested: count, beforeWorkers: before.dedicatedWorkers,
             error: response?.exceptionDetails?.exception?.description || 'worker loader returned no count' };
  await sleep(3000);
  const after = sampleProcMem(pid);
  if (!after) return { requested: count, error: 'renderer disappeared after probe' };
  const n = after.dedicatedWorkers - before.dedicatedWorkers;
  const component = (get) => {
    const delta = (get(after) - get(before)) / 1048576;
    return { deltaMb: +delta.toFixed(1), perWorkerMb: n > 0 ? +(delta / n).toFixed(1) : null };
  };
  return {
    requested: count, loaded: response.result.value.loaded,
    beforeWorkers: before.dedicatedWorkers, afterWorkers: after.dedicatedWorkers,
    deltaWorkers: n, beforeThreads: before.threads, afterThreads: after.threads,
    pool: response.result.value,
    vmData: component(s => s.vmData),
    stacksAnon: component(s => s.buckets['[anon]'] || 0),
    partitionAllocMap: component(s => s.buckets['[anon:partition_alloc]'] || 0),
    partitionRootsCommitted: before.partitions?.__all?.every(r => r.counterStatus === 'valid') &&
      after.partitions?.__all?.every(r => r.counterStatus === 'valid') ?
      component(s => s.partitions.__allTotalCommitted) : null,
    v8Sandbox: component(s => s.buckets['[anon:v8-sandbox]'] || 0),
    v8RwxCode: component(s => s.v8Pages.codeRwx),
    v8RwData: component(s => s.v8Pages.dataRw),
    pointerTable: component(s => s.buckets['[anon:v8-pointer-table]'] || 0),
    partitionError: after.partitions?.__error || before.partitions?.__error || null,
  };
}

/*
 * Fold the OPFS_STATS checkpoints into a per-phase I/O picture plus a PACING distribution.
 *
 * Each checkpoint is exactly 8192 reads after the last, so the wall gap between two of them divided by
 * 8192 is microseconds-per-read -- the only latency signal available without new instrumentation. Measured
 * 2026-09-07 across 280 archived runs: floor (median of each run's minimum) 22.8 us, median-of-run 479 us,
 * and EVERY run contains a stretch at ~3.2 ms/read (2721-6886 us). The floor also rises with boot time
 * (20.9 -> 31.2 us across boot tertiles), so it is not purely an artefact of sparse reads.
 *
 * WHERE THE READS ACTUALLY ARE, which is not where R12's framing implies: 83% of them land in BOOT
 * (median over 876 logs; boot.resources 49,152, boot.classload 24,576, boot.mainscreen 8,192), and both
 * `world.*` and `ingame` sit under one checkpoint. So the OPFS read path is a BOOT cost, and any argument
 * that it gates world load or frame rate has to explain that distribution first.
 *
 * READ `usPerRead` AS AN UPPER BOUND, NOT A LATENCY. The gap between checkpoints contains everything else
 * the requesting thread did, so pacing cannot separate "each read got slower" from "the reads were further
 * apart". That distinction needs the round trip timed at the source, which has never been done -- R12
 * counted ~116,000 blocking round trips and never converted them to seconds, leaving the true cost
 * somewhere between ~2.5 s and ~300 s. Do not quote a pacing number as an OPFS cost.
 */
export function summariseOpfs (samples, phaseDefs, blocked = []) {
  if (!samples.length && !blocked.length) return null;
  const last = samples.length ? samples[samples.length - 1] : null;
  const us = [];
  for (let i = 1; i < samples.length; i++) {
    const dr = samples[i].reads - samples[i - 1].reads;
    if (dr > 0) us.push((samples[i].t - samples[i - 1].t) / dr * 1e6);
  }
  us.sort((a, b) => a - b);
  const at = (p) => us.length ? Number(us[Math.min(us.length - 1, Math.floor(us.length * p))].toFixed(1)) : null;
  /* Per-phase deltas, interpolated at the phase boundaries. Coarse by construction -- the checkpoint
   * granularity is 8192 reads -- so a phase shorter than one checkpoint reports 0 rather than a fraction. */
  const byPhase = {};
  for (const d of phaseDefs) {
    const lo = samples.filter((x) => x.t <= d.t0).pop();
    const hi = samples.filter((x) => x.t <= d.t1).pop();
    if (!hi) continue;
    const a = lo ?? { reads: 0, readMB: 0, writes: 0, writeMB: 0, getSize: 0, flush: 0 };
    byPhase[d.name] = { reads: hi.reads - a.reads, readMB: Number((hi.readMB - a.readMB).toFixed(1)),
                        writes: hi.writes - a.writes, getSize: hi.getSize - a.getSize, flush: hi.flush - a.flush };
  }
  /* No `?? samples[0]` fallback here, unlike the blocked-time table: these checkpoints DO fire throughout
   * boot, so a phase with none genuinely did fewer than 8192 reads. Adding a fallback would turn "under
   * one checkpoint" into "the whole run so far", which is the opposite of the truth. */
  return {
    checkpoints: samples.length,
    /* `readsFloor`, not `reads`: see the caveat in the pump. The name is the warning. */
    readsFloor: last?.reads ?? null, readMB: last?.readMB ?? null,
    writes: last?.writes ?? null, writeMB: last?.writeMB ?? null,
    getSize: last?.getSize ?? null, flush: last?.flush ?? null,
    usPerRead: { min: at(0), p50: at(0.5), p90: at(0.9), max: us.length ? Number(us[us.length - 1].toFixed(1)) : null },
    byPhase,
    blocked: summariseBlocked(blocked, phaseDefs),
  };
}

/*
 * Admission counters, per phase. See ADMIT_POLL_JS for what the fields mean and why the window-scoped
 * `deltas` block cannot answer the question these are here for.
 *
 * `allZero` is reported separately from "absent" because the two need opposite responses: absent means the
 * export is missing and the build is wrong, all-zero means MONO_WASM_JIT_STATS was not set. Collapsing
 * them to null is the mistake WINDOW_JS's `countersAvailable` flag exists to avoid, and it is repeated here.
 *
 * `splitViolations` counts polls where either identity failed. NON-ZERO IS A BUG IN THE COUNTERS, not in
 * the run: it means a route through the fallback path has no counter, so any share computed from these is
 * wrong. Zero is the healthy reading, unlike a race-catch counter where zero would mean dead code.
 */
export function summariseAdmit (samples, phaseDefs) {
  if (!samples.length) return null;
  const last = samples[samples.length - 1];
  const keys = Object.keys(last).filter((k) => k !== "t" && !k.endsWith("Ok"));
  const byPhase = {};
  for (const d of phaseDefs) {
    const lo = samples.filter((x) => x.t <= d.t0).pop();
    const hi = samples.filter((x) => x.t <= d.t1).pop();
    if (!hi) continue;
    const o = {};
    let any = false;
    for (const k of keys) {
      const v = (hi[k] ?? 0) - (lo?.[k] ?? 0);
      if (v) { o[k] = v; any = true; }
    }
    if (any) byPhase[d.name] = o;
  }
  return {
    polls: samples.length,
    total: Object.fromEntries(keys.map((k) => [k, last[k]])),
    allZero: keys.every((k) => !last[k]),
    /* TORN READS vs A MISSING COUNTER, and they need opposite responses.
     *
     * The poller reads each counter with its own `_mono_wasm_jit_get_counter` call, so a poll landing
     * mid-update sees an inconsistent set. A real `--stats` run reported 47 violations while its FINAL
     * values satisfied the identity exactly (84,880 + 0 + 219 + 0 == 85,099), i.e. all 47 were tearing.
     * Only a violation in the LAST sample means a route through the fallback path has no counter, which
     * is the R166 failure this check exists for. Transient count is kept but demoted. */
    splitViolations: (last.thresholdSplitOk === false || last.sccSplitOk === false || last.depNotLiveSplitOk === false) ? 1 : 0,
    tornReads: samples.filter((x) => x.thresholdSplitOk === false || x.sccSplitOk === false || x.depNotLiveSplitOk === false).length,
    byPhase,
  };
}

/*
 * The PULL-based blocked-time counters, folded into totals and per-phase deltas.
 *
 * This is the authoritative op count and the only latency measurement: unlike the read checkpoints it is
 * neither quantised to 8192 nor blind to anything happening after the last read. `total` is the whole run
 * as of the last poll; `byPhase` is the delta between the polls bracketing each phase, so it is accurate
 * to the poll interval rather than to 8192 reads.
 *
 * `partsSumToWhole` is not decoration. Four separate rounds in this project were lost to a share computed
 * before every route had a counter, and the fix each time was to assert the identity rather than argue it.
 * `allN`/`allMs` are computed in C over all kinds, so comparing them to the sum of the per-kind rows tests
 * that no op kind is missing a timer -- which is exactly the failure that would make the total an
 * underestimate with no visible symptom.
 */
function summariseBlocked (blocked, phaseDefs) {
  if (!blocked.length) return null;
  const last = blocked[blocked.length - 1];
  const sumN = Object.values(last.kinds).reduce((a, k) => a + k.n, 0);
  const sumMs = Object.values(last.kinds).reduce((a, k) => a + k.ms, 0);
  const byPhase = {};
  for (const d of phaseDefs) {
    const lo = blocked.filter((x) => x.t <= d.t0).pop();
    /*
     * BOOT WOULD OTHERWISE BE MISSING ENTIRELY, and it is where most of the I/O is.
     *
     * The poller is injected after `titleReady`, so its first line lands a fraction of a second AFTER the
     * boot phase closes -- and a plain "last sample at or before t1" therefore finds nothing for boot and
     * skipped it. The first smoke run duly reported world 6,497 ops and ingame 68 against a run total of
     * 112,340, with the other ~105,000 unattributed and no indication that they were missing.
     *
     * The counters are cumulative, so falling back to the FIRST sample gives boot its exact total plus
     * whatever happened in the sub-second gap. Flagged `approx` rather than silently rolled in: an
     * over-attribution that is labelled is usable, an unlabelled one is how the "AOT image is one thing"
     * class of error starts.
     */
    /* `d.top` ONLY. The fallback is right for `boot` and wrong for every sub-phase of boot, and the first
     * real run showed exactly that: boot.runtime, boot.ikvminit, boot.classload, boot.resources and
     * boot.mainscreen each reported the SAME 90,061 ops, because none of them has a poll at or before its
     * end and all five fell back to the same first sample. Five copies of boot's total, printed as if they
     * were five independent measurements.
     *
     * There is no rule that could separate them -- they all begin and end before the poller starts -- so
     * the honest answer is that this instrument cannot attribute WITHIN boot at all. The coarse read
     * checkpoints can (they fire throughout boot), which is why both are kept. */
    const hi = blocked.filter((x) => x.t <= d.t1).pop() ?? (d.top ? blocked[0] : null);
    const approx = !!hi && !blocked.some((x) => x.t <= d.t1);
    if (!hi) continue;
    const a = lo ?? { allN: 0, allMs: 0, kinds: {} };
    const kinds = {};
    for (const [name, v] of Object.entries(hi.kinds)) {
      const p = a.kinds?.[name] ?? { n: 0, ms: 0, mb: 0 };
      if (v.n - p.n > 0) kinds[name] = { n: v.n - p.n, ms: Number((v.ms - p.ms).toFixed(3)), mb: Number((v.mb - p.mb).toFixed(2)) };
    }
    byPhase[d.name] = { n: hi.allN - a.allN, ms: Number((hi.allMs - a.allMs).toFixed(3)), kinds, ...(approx ? { approx: true } : {}) };
  }
  return {
    polls: blocked.length,
    latEdgesUs: last.edges,
    total: { n: last.allN, ms: last.allMs, kinds: last.kinds },
    /* Non-zero mismatch means an op kind is doing round trips with no timer around it, i.e. `total.ms` is
     * an UNDERESTIMATE. Read this before quoting the total.
     *
     * TOLERANCED, because the naive check cried wolf on its first real run: `allMs` and each kind's `ms`
     * are independently rounded to 3 dp in the poller, so summing eight rounded values against one rounded
     * total leaves ~0.001 ms of slop and the report printed "PARTS DO NOT SUM: 0 ops / -0.001ms" on a run
     * where every op kind was in fact timed. A guard that fires on rounding is a guard people learn to
     * ignore. `n` is an integer count and gets no tolerance; `ms` gets 1 ms, which is ~4 orders of
     * magnitude below any real missing-timer signal (the smallest kind here is 10 ms). */
    partsSumToWhole: { nDelta: last.allN - sumN,
                       msDelta: Math.abs(last.allMs - sumMs) < 1 ? 0 : Number((last.allMs - sumMs).toFixed(3)) },
    byPhase,
  };
}

/** "fast" | "stall" | "mid" | null. `mid` is the 1% in the valley and must not be pooled with either. */
export function classifyWorldGen (generateSecs) {
  if (generateSecs == null || !isFinite(generateSecs)) return null;
  if (generateSecs < WORLD_GEN_FAST_MAX) return "fast";
  if (generateSecs >= WORLD_GEN_STALL_MIN) return "stall";
  return "mid";
}

/* Fault signatures, kept byte-identical in intent to build/faults.sh — see that file for why
 * each one is in the set and why "JSPI mailbox export REJECTED" and bare `unwind` are NOT. Duplicated
 * rather than sourced because that file is bash. GLFW error and the OOM set are separated out: both mean
 * the run tested nothing about the runtime (environment / browser-internal limit), so a caller must be
 * able to tell them apart from a real fault before blaming a change. */
export const OOM_RX = /V8 javascript OOM|Fatal JavaScript out of memory|Exceeding maximum wasm|Failed to reserve memory|Check failed: .*[Aa]llocat|PartitionAlloc.*(OOM|out of memory)|Received signal 4|SIGILL/;
export const VOID_RX = /GLFW error/;
export const FAIL_RX = /unexpected GC filler class|Arg_InvalidCastException|Unable to cast object of type|cannot be cast to|Failed to light chunk|Exception ticking world|Lock is no longer valid|Game crashed!|forcing abort|assert_not_reached|Assertion at|MD5 MessageDigest|Unreported exception thrown/;

/*
 * WASM-LEVEL TRAPS, WHICH THIS HARNESS DID NOT DETECT AT ALL UNTIL NOW.
 *
 * Found the hard way on 2026-09-07: a run logged `RuntimeError: function signature mismatch` four times
 * plus one `WORKER_TRAP_STACK`, the world-load worker died, the driver timed out after 480 s waiting for
 * `inworld` -- and mcbench reported `faults: []`. The timeout was then attributed to `--stats` being slow.
 * It was a crash. Two independent gaps produced that:
 *
 *  1. `function signature mismatch` was in NO regex here, despite being the single most cited
 *     JIT-correctness signature in MINECRAFT-FINDINGS (R165's 6/6 crash, R201's x3 with a wedged world
 *     load, R208's x4, R209's prefilled-placeholder diagnosis). The harness has never once matched it.
 *  2. `WORKER_TRAP_STACK` was in no regex either -- and that string exists ONLY because of the local emsdk
 *     patch to worker.js/library_pthread.js, added specifically so worker traps would carry a stack. The
 *     diagnostic was being produced and nothing read it, so every worker trap whose message was not
 *     literally "memory access out of bounds" was invisible.
 *
 * This is CLAUDE.md's own rule landing on the harness itself: "every error counter is zero" is a statement
 * about the counters you have, not about the run.
 *
 * KEPT DISJOINT FROM FAIL_RX ON PURPOSE. `memory access out of bounds` MOVED here from FAIL_RX rather than
 * being added alongside, because a `WORKER_TRAP_STACK RuntimeError: memory access out of bounds` line
 * matches both spellings and would have been counted twice -- one event, two faults, and a share computed
 * from the total then overstates. Assert the parts are disjoint as well as summing to the whole.
 *
 * FAIL_RX is now managed/native-runtime level (mono asserts, managed casts, game logic); TRAP_RX is
 * wasm-execution level (traps, worker deaths).
 */
export const TRAP_RX = /function signature mismatch|memory access out of bounds|WORKER_TRAP_STACK|WORKER_REJECT_STACK|Uncaught RuntimeError|table index is out of bounds|unreachable executed|null function or function signature/;

/*
 * SHUTDOWN SIGNATURES: a fault BEFORE the quit begins, and expected AFTER it.
 *
 * These two lines used to live in FAIL_RX, which was correct as long as the harness only ever ended a run
 * with SIGKILL -- if the world went away, something had gone wrong. It stops being correct the moment the
 * driver quits deliberately: Minecraft 1.16.1's `MinecraftClient.disconnect()` logs "Stopping singleplayer
 * server as player logged out" on a normal Save and Quit, and the server then logs a client disconnect. So
 * a clean save-and-quit would have scored the run as faulted and `mcab`'s `usable()` would have discarded
 * it -- an arm silently reduced to zero usable runs by the very feature that was supposed to exercise more
 * of the pipeline.
 *
 * Kept as a SEPARATE regex rather than removed, because before `quitStart` these are exactly as
 * diagnostic as they always were: two of three runs once had the client time out from its own integrated
 * server mid-settle, which the harness must still catch (see the settle loop -- "the server stopped
 * complaining" and "the server stopped" are the same silence).
 */
/* `i` preserved from the serverDead matcher this replaced -- that one was case-insensitive and the
 * behaviour must not change for the pre-quit case, which is still a real fault detector. */
export const SHUTDOWN_RX = /lost connection:|Stopping singleplayer server|as player logged out|Stopping server\b/i;

/* ------------------------------------------------------------------------------------------------ *
 * Log tail with monotonic conversion
 * ------------------------------------------------------------------------------------------------ */

/** Chromium log prefix: `[pid:tid:MMDD/HHMMSS.uuuuuu:LEVEL:file:line]`.
 * Exported so `replaylogs.mjs` can re-derive phases from an archived log with THIS parser and THESE
 * MARKERS rather than a second copy of both -- the reimplemented-classifier trap that has cost this
 * directory five wrong answers. */
export const TS_RX = /^\[\d+:\d+:(\d{2})(\d{2})\/(\d{2})(\d{2})(\d{2})\.(\d{6}):/;

class LogTail {
  constructor (path) {
    this.path = path;
    this.off = 0;
    this.partial = "";
    /* Running MINIMUM of (mono_read - line_secs). See the header: the true offset can only be below
     * every observation, so the minimum converges to it and never drifts. */
    this.offset = Infinity;
    this.lastSecs = -1;
    this.dayRoll = 0;
  }

  /** Seconds-of-day for a line, monotonically increasing across a midnight rollover. */
  #secs (m) {
    let s = Number(m[3]) * 3600 + Number(m[4]) * 60 + Number(m[5]) + Number(m[6]) / 1e6;
    if (this.lastSecs >= 0 && s + this.dayRoll < this.lastSecs - 60) this.dayRoll += 86400;
    s += this.dayRoll;
    this.lastSecs = Math.max(this.lastSecs, s);
    return s;
  }

  /** Read whatever is new. Calls onLine(text, monoSeconds|null) per complete line. */
  poll (onLine) {
    let size;
    try { size = statSync(this.path).size; } catch { return; }
    if (size <= this.off) return;
    let fd;
    try { fd = openSync(this.path, "r"); } catch { return; }
    try {
      const len = size - this.off;
      const buf = Buffer.allocUnsafe(len);
      const n = readSync(fd, buf, 0, len, this.off);
      this.off += n;
      const now = monotonic();
      const text = this.partial + buf.toString("latin1", 0, n);
      const lines = text.split("\n");
      this.partial = lines.pop() ?? "";
      for (const line of lines) {
        const m = line.match(TS_RX);
        let mono = null;
        if (m) {
          const s = this.#secs(m);
          const o = now - s;
          if (o < this.offset) this.offset = o;
          mono = s + this.offset;
        }
        onLine(line, mono);
      }
    } finally { closeSync(fd); }
  }
}

/* ------------------------------------------------------------------------------------------------ *
 * Launch
 * ------------------------------------------------------------------------------------------------ */


/**
 * A byte-identical copy of the seeded browser profile, which is the ONLY reason a Minecraft run is
 * repeatable at all.
 *
 * The app's mount() downloads Minecraft 1.16.1 + Fabric into OPFS when it is not already there
 * (main.tsx:61). Against a fresh profile that is a multi-hundred-MB network fetch on every single run,
 * with a different completion time each time -- and it lands INSIDE the boot phase, so boot would be
 * measuring the network. mcsr/seed is a profile with the install, the "New World" save, the options and
 * the mod configs already in place; reflink-copying it on btrfs is metadata-only for 698 MB.
 *
 * The copy MUST NOT live on /tmp. /tmp is tmpfs here (7.7 GiB of RAM), so a profile there is charged to
 * memory twice and 2.4 GB of debris already put this box into reclaim and cost ~9% on the next reading.
 */
export function seedProfile (dest, { seed } = {}) {
  /* a registry name (lib/seeds.mjs), a directory, or a resolved definition; default MC_SEED, else 262n (DEFAULT_SEED) */
  seed = resolveSeed(seed).dir;
  if (!existsSync(seed)) throw new Error(`no seed profile at ${seed} — a fresh profile would re-download Minecraft mid-boot`);
  rmSync(dest, { recursive: true, force: true });
  mkdirSync(resolve(dest, ".."), { recursive: true });
  spawnSync("cp", ["-a", "--reflink=auto", seed, dest], { stdio: "inherit" });
  if (!existsSync(join(dest, "Default"))) throw new Error(`seed copy to ${dest} produced no Default/`);
  return dest;
}

/**
 * Drop a run's profile copy -- unless the run failed, in which case it is evidence.
 *
 * WHY THIS IS NOT OPTIONAL. Nothing used to remove these. mc-out held 340 of them at ~1 GB apparent
 * each (333 GB) going back to 2026-08-08, /var/tmp held 40 more from worldwait, and the box reached 84%
 * full. That is not merely untidy: a full disk or a full /tmp kills a run at PAGE LOAD, which reads as a
 * browser bug rather than as housekeeping, and cost two arms on 2026-09-20 (R280f).
 *
 * KEPT ON FAILURE ON PURPOSE. A faulted run's profile holds the OPFS save, the crash-time logs and the
 * mod state, and it is the only copy -- so the one case where deleting is cheap is exactly the case
 * where it is expensive. `keep` is therefore the VERDICT, not a preference.
 *
 * Reflink-copied from the seed, so on btrfs a clean run's profile costs metadata plus whatever it wrote;
 * the apparent size overstates the reclaim by roughly 8x. Delete it anyway -- inodes and `du` confusion
 * both cost more than the bytes suggest.
 */
export function reapProfile (dest, { keep = false, log = null } = {}) {
  if (!dest || !existsSync(dest)) return false;
  if (keep) { log?.(`  (keeping ${dest} -- the run did not end clean, and the profile is the evidence)`); return false; }
  try { rmSync(dest, { recursive: true, force: true }); return true; } catch { return false; }
}

/**
 * Build the URL. Knobs go through the query string because index.ts applies them on top of its in-source
 * defaults (`?wj.NAME=` / `?ro.NAME=` / `?wj.overrides=`), so no source edit and no republish is needed
 * to change a runtime knob between arms.
 *
 * MONO_LOG_LEVEL defaults to `error` here, i.e. GC logging OFF, and that is a measurement decision worth
 * stating: index.ts ships MONO_LOG_LEVEL=debug + MONO_LOG_MASK=gc, which prints a line per MINOR GC
 * through console.log -- and log.ts proxies console into an uncapped DOM list (main.tsx's LogView appends
 * a <div> per line and never trims). GCs are triggered by allocation INSIDE the window being measured, so
 * that cost lands in the measurement, and over a 7-minute run the DOM list grows without bound. Pass
 * gcLog: true when the GC trace is the thing you actually want.
 */
export function buildUrl ({ base = serveUrl(), knobs = {}, runtimeOptions = {}, gcLog = false, seed } = {}) {
  const u = new URL(base);
  /* The defuse below is MCSR Ranked's; on any other seed it would only be a recorded no-op, so leave it off. */
  if (!resolveSeed(seed).defuseRankedAuth && !("IKVM_DEFUSE_RANKED_AUTH" in knobs)) knobs = { ...knobs, IKVM_DEFUSE_RANKED_AUTH: "0" };
  if (!gcLog) u.searchParams.set("wj.MONO_LOG_LEVEL", "error");
  /* DEFUSE THE NETWORK for measurement runs, unless the caller asks otherwise.
   *
   * MCSR Ranked's account validation never reaches a terminal socket status under this net bridge, so the
   * modded main screen never finishes and the run stalls waiting for `inworld` -- the exact
   * "STALLED: no log output at all for 90s" signature. It voided three arms in one session (shadow_bytes
   * =8000, the first ranked-selection arm, and the threshold=2000 profile_fast arm), each ~8 minutes.
   *
   * IKVM_DEFUSE_RANKED_AUTH=1 (ikvmcraft loader/Transforms/Bench/DefuseRankedAuth.cs) skips that
   * validation. It is BENCHMARK-ONLY and it CHANGES WHAT THE GAME DOES, which is why it goes through the
   * URL like every other knob rather than being hidden in the launcher -- it stays visible in each run's
   * recorded command line. Pass IKVM_DEFUSE_RANKED_AUTH=0 to opt out. */
  if (!("IKVM_DEFUSE_RANKED_AUTH" in knobs)) u.searchParams.set("wj.IKVM_DEFUSE_RANKED_AUTH", "1");
  for (const [k, v] of Object.entries(knobs)) u.searchParams.set(`wj.${k}`, String(v));
  for (const [k, v] of Object.entries(runtimeOptions)) u.searchParams.set(`ro.${k}`, String(v));
  /* MC_WISP=<url> -> ?wisp=<url> (src/net.ts wispUrl): send the game's TCP/TLS through another Wisp server
   * instead of the dev server's local one. In the URL, not the launcher, so it is recorded per run. */
  if (process.env.MC_WISP) u.searchParams.set("wisp", process.env.MC_WISP);
  return u.toString();
}

/**
 * Launch the browser on the app, stderr into `logPath`.
 *
 * Flag choices that are load-bearing rather than tidy-up (the rest is copied from mcsr/chrome.sh, whose
 * comments explain them at length):
 *
 *   --enable-logging=stderr --log-level=0   the entire phase-detection channel. Without it there is no
 *                                           trustworthy record of what happened; see the header.
 *   --no-sandbox                            required for the renderer to write V8's perf map / jitdump
 *                                           at all. Harmless here, mandatory when profiling.
 *   --disable-*-backgrounding / occlusion   a throttled renderer looks exactly like the render-loop stall
 *                                           the harness is trying to measure.
 *   --enable-features=HigherRendererMemoryLimit   the AOT build's renderer peaks over 4 GB.
 *   --ignore-certificate-errors             the dev server is https with a self-signed cert; without this
 *                                           the renderer parks on the interstitial, which looks like a hang.
 *   --unlimited-storage                     OPFS holds the Minecraft install and the save.
 *
 * GL: HEADLESS REACHES THE REAL GPU. `--use-gl=angle --use-angle=gl` under --headless=new reports
 * "ANGLE (Intel, Mesa Intel(R) Iris(R) Xe Graphics (RPL-P), OpenGL 4.6)" on this box, verified by reading
 * UNMASKED_RENDERER_WEBGL out of a headless page. That contradicts the assumption the mcsr harness is
 * built on -- mcsr/chrome.sh: "Run this against nest-xwl.sh, not nest.sh: Xwayland exposes dri3 so
 * Chromium reaches the real GPU" -- which is the entire reason the nested-display apparatus exists there.
 * For measurement it is unnecessary: no display, no window manager, no XTEST, and still the real driver.
 *
 * The default matters enormously to one number, so it is worth being exact. Under SwiftShader the in-game
 * window measured 3.67 fps. That is a software rasteriser, not the runtime, and quoting it as the app's
 * frame rate would simply be wrong. Rasterisation happens in the GPU PROCESS either way, so the
 * renderer-side CPU profile stays broadly comparable -- but frame PACING does not, and the render thread's
 * SHARE of the profile moves with it, because a thread blocked on a slow swapBuffers accrues no task-clock.
 *
 * MC_SOFTWARE_GL=1 forces SwiftShader back, for a box with no usable GPU. --ignore-gpu-blocklist stays
 * either way: without it chromium refuses the context outright and emscripten-glfw dies at
 * glfwCreateWindow with GLFW error 65544, killing the game before the title screen (faults.sh scores that
 * as VOID for exactly this reason).
 */
export function launchGame ({ url, profile, logPath, jsFlags = null, extraArgs = [], headless = true, chromeBin = null }) {
  const bin = chromeBin || process.env.CHROME_BIN || "/usr/bin/chromium";
  const cdpPort = process.env.MC_CDP_PORT || 9333;
  for (const d of readdirSync("/proc")) {
    if (!/^\d+$/.test(d)) continue;
    let cmd;
    try { cmd = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
    if (cmd.includes(`--remote-debugging-port=${cdpPort}\0`) && !cmd.includes("--type="))
      throw new Error(`CDP port ${cdpPort} is held by browser pid ${d}; stop that browser before launching a run`);
  }
  const args = [
    ...(headless ? ["--headless=new"] : []),
    `--user-data-dir=${profile}`,
    `--remote-debugging-port=${cdpPort}`,
    "--remote-allow-origins=*",
    "--window-size=1280,800", "--window-position=0,0",
    "--enable-logging=stderr", "--log-level=0",
    /* MC_SANDBOX=1 keeps the sandbox ON, which is the only way to measure against the REAL
     * RLIMIT_DATA -- 8 GiB here, applied by SandboxLinux::LimitAddressSpace, which is skipped entirely
     * for an unsandboxed renderer. Costs the V8 perf map / jitdump (the renderer cannot write them), so
     * it is for memory validation only, never for a profiling run. */
    ...(process.env.MC_SANDBOX === "1" ? [] : ["--no-sandbox"]),
    "--ignore-certificate-errors",
    "--unlimited-storage",
    /* WebGPU: the GL backend is mesa-wink (Mesa's OpenGL on WebGPU, plan swirling-moore GL3), and Linux Chromium
     * exposes navigator.gpu only behind --enable-unsafe-webgpu with Vulkan enabled (the flags mesa-wink's own
     * wink/test/run-browser.mjs passes). --enable-features is ONE switch -- a second occurrence replaces the first --
     * so Vulkan joins HigherRendererMemoryLimit's list. Every arm of a GL comparison must run with these. */
    "--enable-unsafe-webgpu",
    `--enable-features=${[...(process.env.MC_STOCK_LIMIT === "1" ? [] : ["HigherRendererMemoryLimit"]), "Vulkan"].join(",")}`,
    /* MC_UNCAPPED=1: no presentation ceiling. Headless Chromium paces OffscreenCanvas frames at ~60 Hz BeginFrames, and
     * over 36 Q60 arms fps never passed 61.84 while client CPU per frame varied 48-58 M cycles (R475): Minecraft's own
     * vsync-off does not remove it. Uncapped, fps follows client CPU, so a client lever shows as frame rate. Every arm
     * of a comparison must run the same way. */
    ...(process.env.MC_UNCAPPED === "1" ? ["--disable-gpu-vsync", "--disable-frame-rate-limit"] : []),
    "--autoplay-policy=no-user-gesture-required",
    "--disable-background-timer-throttling",
    "--disable-renderer-backgrounding",
    "--disable-backgrounding-occluded-windows",
    "--disable-features=CalculateNativeWinOcclusion",
    "--disable-session-crashed-bubble", "--disable-infobars",
    "--no-first-run", "--no-default-browser-check",
    "--disable-dev-shm-usage",
    // plasma-browser-integration is installed as a managed extension on this box and reached out over
    // D-Bus during a seed run. Keep the measured environment free of it.
    "--disable-extensions", "--disable-component-extensions-with-background-pages",
    "--disable-sync", "--disable-background-networking", "--disable-client-side-phishing-detection",
    "--ignore-gpu-blocklist",
    ...(process.env.MC_SOFTWARE_GL === "1"
      ? ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
      : ["--use-gl=angle", `--use-angle=${process.env.MC_ANGLE || "gl"}`]),
    /* ALWAYS raise the stack trace limit. V8 defaults to 10 frames and EVERY wasm trap this harness
     * has captured (WORKER_TRAP_STACK) was truncated at exactly that -- which is why the
     * ghashtable.c:334 and mono-internal-hash.c:47 aborts showed their mono callee chain but never
     * the caller that reached it, and why root-causing them needed a rebuild instead of a log. Passed
     * as --js-flags because that is process-wide and so reaches WORKERS; Error.stackTraceLimit set
     * from an init script does not, and workers are where all of these faults happen.
     * `--no-logfile-per-isolate --logfile=+`: --perf-basic-prof and --perf-prof both IMPLY --log
     * (v8 flags/flag-definitions.h:4516-4517), and with the default per-isolate log file every isolate
     * wrote isolate-<addr>-<pid>-v8.log into the browser's CWD -- ~64 per run, 8,103 of them had piled up
     * in scratchpad/wj. "+" is tmpfile() (logging/log-file.cc:38), already unlinked, so it dies with the
     * renderer; it only takes effect without the per-isolate prefix (logging/log.cc:2333). Inert when
     * nothing turns --log on. The perf map is a separate listener and is unaffected. */
    `--js-flags=--stack-trace-limit=64 --no-logfile-per-isolate --logfile=+${jsFlags ? " " + jsFlags : ""}`,
    ...extraArgs,
    url,
  ];
  writeFileSync(logPath, "");
  const fd = openSync(logPath, "a");
  // WAYLAND_DISPLAY is set in this shell; chromium would otherwise auto-select the Wayland backend and
  // ignore the headless/X selection entirely (mcsr/chrome.sh hit exactly this).
  const env = { ...process.env };
  delete env.WAYLAND_DISPLAY;
  delete env.XDG_SESSION_TYPE;
  /* MC_TASKSET=<cpu list> pins the whole browser (every child inherits the mask) to those CPUs, e.g. "0-7" = this
   * i7-1360P's P-cores. R362's variance note: the server thread's E-core share swings 12.5-15.2 M cycles/tick between
   * arms and E-cores retire at ~0.64 IPC, so "cycles" mixes a scheduling draw into every census. taskset EXECs the
   * command, so proc.pid is still the browser's. Recorded in the returned command line like every other knob. */
  const pin = process.env.MC_TASKSET;
  const proc = pin
    ? spawn("taskset", ["-c", pin, bin, ...args], { stdio: ["ignore", fd, fd], env, detached: true })
    : spawn(bin, args, { stdio: ["ignore", fd, fd], env, detached: true });
  closeSync(fd);
  const kill = () => {
    try { process.kill(-proc.pid, "SIGKILL"); } catch { }
    try { proc.kill("SIGKILL"); } catch { }
  };
  return { proc, kill, args: pin ? ["taskset", "-c", pin, ...args] : args, bin };
}


/* ------------------------------------------------------------------------------------------------ *
 * SPIN DETECTION
 *
 * R275 caught the world-load hang live and found that every previous description of it was wrong about
 * the mechanism: "It is not blocked. Two threads at ~92% CPU, both state=R, wchan=0 ... it is a busy
 * spin", concentrated on five addresses spanning ~22 bytes of V8-generated code, and `Debugger.pause`
 * could not interrupt it. A wedge and a spin present identically through the log -- no progress,
 * faults=[], the budget runs out -- and they need OPPOSITE next instruments, so the driver must tell
 * them apart rather than reporting "the runtime is wedged" for both.
 *
 * TWO CONDITIONS, CONJOINED, and the conjunction is the whole design. R276 records three detector
 * defects of exactly this shape, the first being "CPU > 80% = spin", which fired on healthy runs because
 * the server tick legitimately pins a core. CPU alone means nothing; CPU *while the app is making no
 * progress* is the signal.
 *
 * /proc needs no privileges and no capture: schedstat's first field is cumulative run_ns, so a delta over
 * a known wall interval is the thread's true on-CPU share, and wchan names what a BLOCKED thread is
 * blocked in. That is the same source threadwait.mjs uses and it is exact, unlike sampling.
 * ------------------------------------------------------------------------------------------------ */
const procRd = (p) => { try { return readFileSync(p, "utf8"); } catch { return null; } };

export function threadSnapshot (pid) {
  const m = new Map();
  let tids; try { tids = readdirSync(`/proc/${pid}/task`); } catch { return m; }
  for (const tid of tids) {
    const ss = procRd(`/proc/${pid}/task/${tid}/schedstat`);
    if (!ss) continue;
    const [run, wait] = ss.trim().split(/\s+/).map(Number);
    m.set(tid, { run, wait,
                 comm: (procRd(`/proc/${pid}/task/${tid}/comm`) || "?").trim(),
                 wchan: (procRd(`/proc/${pid}/task/${tid}/wchan`) || "").trim() });
  }
  return m;
}

/**
 * Is this process SPINNING (running hard, going nowhere) or BLOCKED?
 *
 * Call with a snapshot taken `wallMs` ago. Returns null when the process is gone -- which is itself
 * information the caller needs, because a vanished browser is `killed`, not a wedge.
 */
/**
 * The watchdog's rule, pure so it can be tested (R340). `hist`: Map tid -> CPU fractions, oldest first, one per
 * 5 s. Returns [tid, history, [aliveTid, aliveHistory]] for the first BUSY thread (>= 6 samples, median of the
 * last 12 >= 0.4) whose last two samples are both < 0.05 while another busy thread's last sample is >= 0.3;
 * else null. A whole-process stop never fires (nothing is alive) -- that is waitFor's case, not this one.
 */
export function stallFromHistory (hist) {
  const med = (a) => { const b = [...a].sort((x, y) => x - y); return b.length ? b[b.length >> 1] : 0; };
  const busy = [...hist].filter(([, h]) => h.length >= 6 && med(h.slice(-12)) >= 0.4);
  for (const [tid, h] of busy) {
    const last2 = h.slice(-2);
    if (!(last2.length === 2 && last2.every((f) => f < 0.05))) continue;
    const alive = busy.find(([t2, h2]) => t2 !== tid && h2[h2.length - 1] >= 0.3);
    if (alive) return [tid, h, alive];
  }
  return null;
}

export function spinCheck (pid, before, wallMs, { busyFrac = 0.8 } = {}) {
  if (!pid || !before?.size || wallMs <= 0) return null;
  const after = threadSnapshot(pid);
  if (!after.size) return { gone: true, spinning: false, busy: [] };
  const busy = [];
  let runningTotal = 0;
  for (const [tid, a] of after) {
    const b = before.get(tid); if (!b) continue;
    const frac = (a.run - b.run) / 1e6 / wallMs;      // run_ns -> ms, over wall ms
    runningTotal += frac;
    /* wchan is empty (or 0) for a RUNNABLE thread and names a kernel function for a blocked one. A
     * spinner is hot AND not in the kernel; a thread hot inside io_schedule is doing I/O, not spinning. */
    if (frac >= busyFrac && (!a.wchan || a.wchan === "0"))
      busy.push({ tid, comm: a.comm, frac: Number(frac.toFixed(3)), wchan: a.wchan || null });
  }
  return { gone: false, spinning: busy.length > 0, busy,
           cores: Number(runningTotal.toFixed(2)),
           /* What the blocked threads are waiting in -- names the primitive when it is NOT a spin. */
           wchan: [...after.values()].map((v) => v.wchan).filter((w) => w && w !== "0")
                    .reduce((acc, w) => (acc[w] = (acc[w] || 0) + 1, acc), {}) };
}

/* ------------------------------------------------------------------------------------------------ *
 * The run
 * ------------------------------------------------------------------------------------------------ */

/**
 * Synthetic key, dispatched on `document` — the path emscripten-glfw actually listens on, and the one
 * jitbench.ts's benchWalk already proves drives the game.
 *
 * Deliberately NOT Input.dispatchKeyEvent: CDP's own key path depends on the page having focus and on a
 * live renderer widget, neither of which is guaranteed headless, whereas a document-level dispatch is
 * just a function call in the page. GLFW tracks press/release state and polls it per frame
 * (glfwGetKey), so one keydown "holds" the key until the matching keyup.
 */
/**
 * The launcher (main.tsx, 2026-10-21): type the seed's version and Fabric version, press Play. Returns when Play
 * has been pressed, with what the page reports; throws if the launcher never became usable.
 *
 * The fields are dreamland two-way bindings, which update their state on the `input` event, so setting
 * `.value` alone would leave the app on its default version. The values are read back after the event and a
 * mismatch throws rather than booting the wrong version. A page with no launcher (the app before the
 * redesign started on load) returns { mode: "autostart" } and presses nothing.
 */
export async function driveLauncher (conn, sid, seed, { log = console.log, timeoutMs = 90000 } = {}) {
  const s = resolveSeed(seed);
  const ev = async (expression) => (await conn.send("Runtime.evaluate", { expression, returnByValue: true }, sid))?.result?.value;
  const t0 = Date.now();
  let st = null;
  for (;;) {
    st = await ev(`(() => {
      if (document.readyState !== "complete") return "loading";
      const play = [...document.querySelectorAll("button")].find((b) => b.textContent.trim() === "Play");
      if (play) return play.disabled ? "disabled" : "launcher";
      return document.querySelector("canvas#canvas") ? "autostart" : "none";
    })()`).catch(() => "loading");
    if (st === "launcher" || st === "autostart") break;
    if (Date.now() - t0 > timeoutMs) throw new Error(`launcher never became usable (last state: ${st})`);
    await sleep(300);
  }
  if (st === "autostart") { log(`  launcher: none on this page (app starts on load)`); return { mode: "autostart" }; }
  if (!s.version) throw new Error(`seed ${s.name} names no version to type into the launcher`);
  const r = JSON.parse(await ev(`(() => {
    const field = (label) => {
      const sp = [...document.querySelectorAll("span")].find((x) => x.textContent.trim().startsWith(label));
      return sp && sp.parentElement && sp.parentElement.querySelector("input");
    };
    const set = (inp, v) => { inp.focus(); inp.value = v; inp.dispatchEvent(new Event("input", { bubbles: true })); inp.dispatchEvent(new Event("change", { bubbles: true })); inp.blur(); };
    const ver = field("Minecraft version"), fab = field("Fabric Loader version");
    if (!ver || !fab) return JSON.stringify({ err: "launcher fields not found" });
    set(ver, ${JSON.stringify(s.version)}); set(fab, ${JSON.stringify(s.fabric)});
    return JSON.stringify({ version: ver.value, fabric: fab.value });
  })()`) || "{}");
  if (r.err) throw new Error(r.err);
  if (r.version !== s.version || r.fabric !== s.fabric)
    throw new Error(`launcher kept ${r.version}/${r.fabric}, wanted ${s.version}/${s.fabric}`);
  /* MC_PRE_PLAY_JS=<expression>: evaluated (awaited) in the page just before Play, its value logged. For first-launch
   * work on the run's own profile copy, e.g. removing the game dir's .fabric (Fabric's remapped-jar cache) to measure
   * the remap. Runs before the runtime starts, so wasmfs's OPFS negative cache (R491) is still empty. */
  if (process.env.MC_PRE_PLAY_JS) {
    const pr = await conn.send("Runtime.evaluate", { expression: process.env.MC_PRE_PLAY_JS, awaitPromise: true, returnByValue: true }, sid);
    log(`  launcher: pre-play JS -> ${JSON.stringify(pr?.result?.value ?? pr?.exceptionDetails?.text ?? null).slice(0, 400)}`);
  }
  await ev(`[...document.querySelectorAll("button")].find((b) => b.textContent.trim() === "Play").click()`);
  log(`  launcher: ${r.version} + Fabric ${r.fabric}, Play pressed (seed ${s.name})`);
  return { mode: "launcher", ...r };
}

/* The Runner's overlay text ("Initializing runtime" / "Verifying assets" / "Downloading assets" / "" once the game
 * runs). "Downloading assets" means the seed lacks something this app version checks for, and the boot is about to
 * measure the network. */
export async function launcherState (conn, sid) {
  try {
    const r = await conn.send("Runtime.evaluate", { expression: `(document.querySelector(".overlay.not-ready")?.textContent || "").trim()`, returnByValue: true }, sid);
    return r?.result?.value ?? null;
  } catch { return null; }
}

async function sendKey (conn, sid, code, key, holdMs = 60) {
  const ev = (type) => `document.dispatchEvent(new KeyboardEvent(${JSON.stringify(type)},`
    + `{code:${JSON.stringify(code)},key:${JSON.stringify(key)},repeat:false,bubbles:true,cancelable:true}))`;
  await conn.send("Runtime.evaluate", { expression: ev("keydown") }, sid);
  await sleep(holdMs);
  await conn.send("Runtime.evaluate", { expression: ev("keyup") }, sid);
}

/*
 * Menu path, validated interactively by autoworld.sh screenshotting after every key. Keyboard only:
 * mouse MOTION reaches Minecraft (buttons visibly highlight) but mouse BUTTONS never arrive, so no click
 * ever activates a widget. 1.16.1's GUI keyboard navigation is Tab to move focus, Enter to activate.
 *
 *   Tab,Enter   dismiss the MCSR Ranked "Player authentication data couldn't be loaded" dialog. Its only
 *               button is Cancel and Escape does NOT dismiss it.
 *   Tab,Enter   Singleplayer -> Select World
 *   Tab,Down    focus the world list, select "New World"
 *   Tab,Enter   "Play Selected World"  (Enter while the LIST has focus does nothing in 1.16.1 — the
 *               button itself must be focused)
 */
const MENU_STEPS = [
  ["Tab", "Tab", 700], ["Enter", "Enter", 2500],
  ["Tab", "Tab", 700], ["Enter", "Enter", 3000],
  ["Tab", "Tab", 700], ["ArrowDown", "ArrowDown", 1000],
  ["Tab", "Tab", 700], ["Enter", "Enter", 2000],
];

/*
 * QUIT PATH: pause menu -> "Save and Quit to Title".
 *
 * Same constraints as MENU_STEPS and for the same reasons: mouse MOTION reaches Minecraft but mouse
 * BUTTONS never arrive, so this is keyboard-only, and 1.16.1's GUI navigation is Tab to move focus and
 * Enter to activate.
 *
 * THE TAB COUNT IS THE FRAGILE PART AND IT IS NOT GUESSED FROM THE VANILLA MENU. 1.16.1's pause screen is
 * Back to Game / Advancements / Statistics / (social) / Mods / Options / Open to LAN / Save and Quit to
 * Title -- and this instance runs Fabric with MCSR Ranked and Sodium, which add and remove buttons. So
 * rather than encoding a fixed count, the sequence walks Tab and probes: `MC_QUIT_TABS` is the number of
 * Tab presses before Enter, defaulted to what the interactive screenshot pass established, and overridable
 * without an edit for a different mod set.
 *
 * autoworld.sh's lesson applies directly: a miscounted Tab lands every later press one step out of phase,
 * and the run then sits in the world looking exactly like a hang in the app. That is why the caller treats
 * a missing `savingWorlds` marker as "quit did not take" and reports it, rather than waiting out a budget.
 */
/*
 * 8, MEASURED not guessed. The probe (MC_QUIT_PROBE_DIR) screenshotted every Tab position on this exact
 * mod set: the pause screen is Back to Game / Advancements / Statistics / Discord Server / Report Bugs /
 * Options... / Open to LAN / Save and Quit to Title, focus starts on nothing, and the 8th Tab is the frame
 * where "Save and Quit to Title" gains its focus border. Note four of those eight are NOT vanilla buttons
 * -- Discord Server and Report Bugs are MCSR Ranked's -- which is exactly why the count cannot be read off
 * Minecraft's source and why the probe exists.
 */
/* MC_QUIT_TABS overrides the seed's own count (lib/seeds.mjs `quitTabs`; MCSR's 8 is the one measured below). */
const QUIT_TABS_ENV = process.env.MC_QUIT_TABS ? Number(process.env.MC_QUIT_TABS) : null;
const QUIT_TABS_MAX = Number(process.env.MC_QUIT_TABS_MAX || 12);

/*
 * The in-game measurement window, injected into the page rather than delegated to jitbench.ts's
 * benchMeasure().
 *
 * benchMeasure was the obvious choice and it does not survive being called this way: it opens with
 * dumpJiterpHeat(), which calls Module._dump_jiterp_trace_heat and runtime.INTERNAL.jiterpreter_dump_stats,
 * and from the page those throw emscripten's `unwind` sentinel. The whole async function then rejects one
 * line after printing "[bench] measure START", so the first attempt at this harness logged a window
 * OPENING and then waited out its entire timeout for a window that had already died -- with the game still
 * running at full speed the whole time, which is the most expensive shape a failure can take here.
 *
 * The rule this encodes: a measurement window must not depend on any diagnostic it does not need. Frame
 * count and the WJC_* counters are plain getters over shared memory (index.ts's own fps interval calls
 * _emscripten_glfw3_get_frame_count every second and drive.mjs reads _mono_wasm_jit_liveness from the page
 * in-world), so they are safe; the printf-based dumps are not, and are therefore optional and individually
 * guarded. Every read below is wrapped so that a missing or throwing export degrades that ONE field to
 * null instead of destroying the window.
 *
 * The walk path is deliberately identical to benchWalk's -- sprint forward held throughout, strafe right
 * on 4s phase 1 and left on phase 3 -- so numbers from this harness stay comparable with anything measured
 * through the debug-bar button.
 */
const WINDOW_JS = (durationMs, walk, dumps, settleInfo = {}, warmMs = 0) => `(async () => {
  const settle = ${JSON.stringify(settleInfo)};
  const M = globalThis.wasm && globalThis.wasm.Module;
  const g = (f) => { try { return f(); } catch (e) { return null; } };
  const frames = () => g(() => M._emscripten_glfw3_get_frame_count());
  const wj = (i) => g(() => M._mono_wasm_jit_get_counter(i));
  // WJC_* indices mirrored from frontend/src/dotnet/jitbench.ts's WJ enum. Only the handful that describe
  // the interp<->JIT boundary and compile volume, since a clean (stats-off) run reads 0 for all of them
  // and the point here is to notice WHICH.
  /*
   * WJC_* indices mirrored from frontend/src/dotnet/jitbench.ts's WJ enum.
   *
   * fastVic / fastAotic / fastInlineAot (44-46) ARE THE DENOMINATOR AND ARE EASY TO OMIT. vicHit/vicMiss
   * (8/9) are bumped inside mono_wasm_jit_vcall_resolve_fslot, i.e. only for calls that reached the
   * out-of-line resolver at all. An inline f-slot IC hit is pure emitted wasm and calls no counting helper,
   * so it is invisible unless MONO_WASM_JIT_PROFILE_FAST=1 emits the fast-path volume counters.
   *
   * Reading vicMiss/(vicHit+vicMiss) without them gives the miss rate *among calls that already missed the
   * inline cache* — a number that is guaranteed to look alarming and means almost nothing. It read 44.4%
   * that way here, which is exactly the sort of figure that gets acted on. The true rate needs
   * vicMiss/(fastVic+fastAotic+vicHit+vicMiss), so both knobs have to be on together.
   */
  const COUNTERS = { registered:0, bailed:1, invoke:3, residual:4, fastvcall:5, aotRouted:6, interpRouted:7,
                     vicHit:8, vicMiss:9, vfastHad:10, vfastNew:11, vfbThresh:12, vfbPerm:13,
                     bytesGenerated: 19, elapsedGen: 20, elapsedInst: 21, compileAttempts: 22,
                     islandAttempt: 23, islandCompleted: 24, vcallAotFast: 36, vfbCold: 39, vfbParked: 40, vfbRetry: 41,
                     fastInlineAot: 42, fastVic: 43, fastAotic: 44,
                     /* Delegate.Invoke dispatch, split out of the vcall pool. FastDelegate counts every
                      * direct delegate dispatch; DelegateIcHit is the subset that also consumed the inline
                      * recipe. Both need PROFILE_FAST=1. Added because the devirt prediction path EXCLUDES
                      * delegate sites by construction (mini-wasm.c: !is_delegate_invoke on both the
                      * predict gate and the emitted guard), so "how much of the IC traffic is delegates"
                      * is the first thing to ask about low prediction coverage. */
                     fastDelegate: 50, delegateIcHit: 51,
                     /* Call forms chosen at assembly, per module BUILT. */
                     callLocal: 62, callImport: 63, callIndirect: 64, colocatedMembers: 65,
                     /* MONO_WASM_JIT_COLOCATE_TIGHT_DEPS: dep entries dropped because a re-framed module's
                      * dependency set was recomputed from the assembler instead of inherited. Each one is a
                      * callee that became a module-local 'call funcidx', i.e. the surplus admission
                      * closure the old code demanded. Read beside BATCH_ADMIT_FAIL / ADMIT_DEFER_GIVEUP. */
                     tightDepsDropped: 78,
                     /* co-location safety: members handed back to their standalone modules because the
                      * group could not be admitted, and groups refused because dropping a callee would
                      * have left a cycle spanning two modules. rolledBack should track how often
                      * co-location did not stick; sccRefused should stay small (R161 measured 11 such
                      * cycles in the whole tier). */
                     rolledBack: 79, sccRefused: 80,
                     /* R166. admitFailRetry/admitFailPerm split mono_wasm_jit_admit's failures by whether
                      * this worker will retry (state 0) or has given the method up for the run (state 3).
                      * vfbNotLive is the fourth arm of the vfbThresh split: target compiled, but not
                      * dispatchable on THIS worker. Check vfbCold+vfbParked+vfbRetry+vfbNotLive ==
                      * vfbThresh before quoting any share of the fallback pool. */
                     admitFailPerm: 81, admitFailRetry: 82, vfbNotLive: 83,
                     /* mono_wasm_jit_admit_live's refusal split (R166). alAdmit0 = admit() said no;
                      * alState = admit() returned 1 without state 2 (the cycle-break); alGen/alELive/
                      * alFLive = state 2 but generation or per-worker slot liveness disagreed. These
                      * five must account for the whole of vfbNotLive. */
                     alAdmit0: 84, alState: 85, alGen: 86, alELive: 87, alFLive: 88,
                     /* R167 shadow copies. shadowBytes is the one to watch: a shadow is duplicated per
                      * CALLER module, so read it against bytesGenerated. Reach shows up in callLocal
                      * vs callIndirect, not in shadowMembers. */
                     /* WHY a shadow was refused. nojit = the callee has no relocatable body at all
                      * (AOT / main module) and NO shadow policy can reach it; notleaf = ours, refused by
                      * policy. Those two lead to opposite decisions, which is why one bucket was useless. */
                     /* R170 re-emit with a matured profile. Judge it by the DEVIRT census moving
                      * (no_rec down, emitted up), not by reemitDone, which only counts attempts. */
                     /* The devirt census SCOPED TO RE-EMITTED BODIES. The run-wide devirt numbers are
                      * cumulative over every emission and cannot show what a few dozen re-emissions did;
                      * compare reemitEmitted/reemitSite against the run-wide 28.1% emitted share. */
                     fslotReregister: 89, /* the COST side of MONO_WASM_JIT_DEVIRT_FORCE: it deliberately creates blockers, and
                      * a blocker that never clears is a hot method left in the interpreter -- which is
                      * exactly what R153's world-load stall was. Always read these beside forced. */
                      islandBudget: 25, islandDepth: 26, parked: 37,
                     /* WHY a co-location group was not formed -- one counter per exit of
                      * mono_wasm_jit_colocate_deps_now. ASSERT THE IDENTITY BEFORE QUOTING A SHARE:
                      *   coTry == coFormed + coSelfBatched + coSelfNoDepset + coSelfRefused
                      *            + coSingleton + sccRefused + coRebatchFail
                      * The coDep* arms are per-CALLEE volumes and are deliberately OUTSIDE that sum --
                      * one refused group contributes several. Read coDepBatched first: it is the
                      * append-only partition refusing a callee another caller already claimed, which is
                      * the blocker with the most reach behind it. */
                     coTry: 90, coFormed: 91,
                     coSelfBatched: 92, coSelfNoDepset: 93, coSelfRefused: 94,
                     coDepUnreg: 95, coDepNobody: 96, coDepBatched: 97, coDepRefused: 98,
                     coDepByteCap: 99, coMemberCap: 100, coSingleton: 101, coRebatchFail: 102,
                     /* CO-LOCATION REACH AS EXECUTED, from the IC miss/publish path -- the thing
                      * callLocal is structurally blind to. callLocal counts relocations the assembler
                      * turned into a module-local call funcidx, i.e. the call-FORM half; R183 measured
                      * the GROUPING half at -7.0% of a -12.8% total (55% of the win), and R182 saw
                      * callLocal at 4.0% of calls even at 73% reach. A site gets the grouping half
                      * exactly when its runtime target is a sibling, because V8's CallIndirectIC only
                      * records a precise target while implicitArg == the current instance
                      * (builtins/wasm.tq:821-835).
                      * Read vicTgtSibling/(vicTgtSibling+vicTgtForeign) as the share of OUR OWN
                      * dispatch that is co-resident; vicTgtNotours is the AOT wall and wants over_aot,
                      * not a planner.
                      * TWO BIASES, read them before quoting: (1) groups are append-only and form at
                      * publish, so a target that joins a group after its site's last miss stays
                      * foreign; (2) THIS IS THE MISS PATH, and a virtual target is only ever a group
                      * member if a WASM_RELOC_CALL names it -- which for a dispatch site means a devirt
                      * PREDICTED ARM, and an arm that exists is an arm that hits and never publishes.
                      * So this samples the population that by construction has no direct edge to its
                      * target. MEASURED on jbox2d at 73% member reach: sibling 3 / foreign 2,902,973 /
                      * notours 0. That is a real answer to "does the ordinary-virtual MISS pool get
                      * anything from the current grouping" (no), and NOT an answer to "how much of
                      * R183's grouping half reaches dispatch" -- for that use vcallreach.py's per-arm
                      * call-form split over a tier dump. Do not substitute one for the other. */
                     vicTgtSibling: 103, vicTgtForeign: 104, vicTgtNotours: 105,
                     /* MONO_WASM_JIT_DELEGATE_OBJ_PIC reachability, counted on the miss path. READ
                      * THESE FIRST on an obj_pic arm: dobjNoInfo counts delegates whose recipe had
                      * nowhere to go (del->invoke_info NULL), so if it dominates the emitted arm is
                      * UNREACHABLE rather than ineffective, and those are opposite conclusions that a
                      * timing number cannot tell apart. */
                     /* MONO_WASM_JIT_COLOCATE_MERGE (Stage 2b). coMerged = pre-existing groups ABSORBED
                      * into a new request, i.e. the conversion of what R192 measured as 100% of
                      * per-callee refusals. coMergeSplit = requests rebatch refused because a group was
                      * only PARTIALLY present, which would be a split rather than a merge; it should be
                      * small and it is a correctness guard, not a tuning signal. Read coMerged AGAINST
                      * coDepBatched: the two now split the population that used to be all batched, so
                      * batched falling while merged rises is the mechanism working. */
                     coMerged: 106, coMergeSplit: 107,
                     /* Merges UNDONE. Split from rolledBack because a merge rollback must restore each
                      * absorbed member to its PREVIOUS group (saved.batch) rather than clear it -- for
                      * such a member saved.bytes is that group's shared blob, which exports e<i>/f<i>
                      * and not e/f. jbox2d ran 98 merges with rolledBack=0, so this path is implemented
                      * and UNTESTED; a non-zero value here is its first observation, not routine. */
                     coMergeRollback: 108,
                     /* WHY a merge closure was refused -- the split of the DEP_BATCHED volume that
                      * survives merging, which R194 left at 18,558 on Minecraft and which IS the arm
                      * blocker (an arm's target is a registered direct dep, so a method with arms has a
                      * depset and no_depset cannot be what stops it). precond = a sibling failed the
                      * per-member rules (fixable in the RULES); cap = COLOCATE_MAX/COLOCATE_BYTES
                      * (fixable by a KNOB). precond + cap + merged == batched callees reached. */
                     coMergePrecond: 109, coMergeCap: 110,
                     /* MONO_WASM_JIT_PRETIER: residual-site tiering bumps, i.e. the missing interpreter
                      * call edge for a callee reached only through residual edges, supplied pre-spill in
                      * pretransform instead of per-dispatch in late_fslot. Read against residualHealed. */
                     /* MONO_WASM_JIT_HEAL_WAIT. healSites = late-f-slot healing blocks recorded at emit;
                      * healWoken = those waiters re-queued for re-emission because the callee published.
                      * READ AS A PAIR: sites with no wakes means those callees never JIT (healing was
                      * right); wakes approaching sites means the block is a transient re-emission can
                      * delete. Read healWoken against reemitDone -- a wake that never completes is
                      * R179's failure shape returning. */
                     healSites: 111, /* R200 hygiene pair. shadowSelf closes shadowRefused's parts-sum
                      * (refused == stale+eh+big+self); asmNoname MUST stay 0 -- a member assembled
                      * without a name loses its perf symbol, blinding every name-resolving tool here. */
                     asmNoname: 112,
                     /* R201 shadow selection. candCap = the RANKING INPUT was truncated (the best
                      * candidate may be one we never scored); modCap = a ranked candidate dropped by the
                      * per-module byte budget, distinct from shadowCap (the per-module COUNT cap). */
                     /* R201: caught the resolve-then-use race on a shared WjBody. Non-zero is HEALTHY
                      * (the race being caught); 0 forever would mean the re-validation is dead code.
                      * The fault it prevents is a wasm 'function signature mismatch'.
                      * NOTE: this table is inside a TEMPLATE LITERAL -- no backticks, no dollar-brace. */
                     /* R205: executed inline-IC hits tagged with their own site's devirt outcome.
                      * fastVicPred is the ALTERNATE RECEIVER at a site that DID devirt -- the predicted
                      * arm keeps the IC below it as a fallthrough -- so no coverage work removes it.
                      * These six must sum to fastVic; check that before quoting any of them. */
                     fastVicPred: 113, fastVicNoRec: 114, fastVicCold: 115,
                     fastVicPoly: 116, fastVicNoFslot: 117, fastVicOther: 118,
                     /* R206 second guarded arm. arm2Thin = the break-even REFUSING a site (not a
                      * failure: an arm below ~15% capture loses). fastDevirt2 is the executed hit
                      * count and needs profile_fast; read it against fastVicPred from before the arm. */
                     arm2Emitted: 119, arm2Thin: 120, arm2NoAlt: 121,
                     arm2NoFslot: 122, arm2Sig: 123, arm2Self: 124, fastDevirt2: 125,
                     /* poly sites given a FIRST arm: double-counted in poly AND emitted, so
                      * sites == emitted + no_rec + cold + poly + sig + no_fslot - polyArm1. */
                     polyArm1: 126,
                     /* R207: why wj_arm_abi_ok refused a candidate. armAbiShape is the only one that
                      * would mean overrides of ONE method lower differently (generic sharing). */
                     armAbiInvalid: 127, armAbiByaddr: 128, armAbiShape: 129,
                     /* R207: split out of the old ARM2_SIG catch-all. */
                     armUnjittable: 130,
                     /* R208: re-emissions enqueued from the IC MISS path. NEVER read alone --
                      * always against reemitDone (R179: 16,343 enqueues for 3 re-emits). */
                     /* R211: members added past the seed's own depset by multi-hop growth. */
                     /* R212 global repartition. Read repartMembers against colocatedMembers; a high
                      * repartSingleton means the graph was not yet reachable when it ran. */
                     /* R213: analyses run (0 or 1; rejection is final) and the internal-edge %
                      * the dry run predicted. analysed=1 + groups=0 means measured and refused. */
                     /* R213b: edges the dry run SAW. 0 = graph not reachable yet, not a refusal. */
                     /* R214: passes re-armed as the tier grew -- the coverage lever. */
                     /* R215 delegate devirt. Read fastDelegateDevirt against fastDelegate. */
                     delegateDevirtArm: 131, delegateDevirtThin: 132,
                     delegateDevirtRefused: 133, fastDelegateDevirt: 134,
                     /* R215b: no usable delegate record -- split out of the thin catch-all. */
                     delegateDevirtNorec: 135,
                     /* WHY a delegate dispatch fell off the direct e-thunk into the full interp marshal.
                      * noeslot is an admission/ordering problem, nonscalar an ABI one -- opposite fixes. */
                     delegateSlowNorecipe: 138, delegateSlowNoeslot: 139, delegateSlowNonscalar: 140,
                     /* GUARDED DEVIRTUALIZED INLINING. giCandidate is measured with the knob OFF, so a
                      * plain --stats run sizes the whole addressable population for free. Read
                      * giRefusedClauses next: mono_method_check_inlining rejects ANY method with a
                      * try/catch and Java is EH-dense, so it may be the entire story.
                      * ASSERT giAdmitted == giEmitted + giRefusedLate. */
                     giSite: 141, giCandidate: 142, giAdmitted: 143, giEmitted: 144,
                     giRefusedProf: 145, giRefusedClauses: 146, giRefusedSize: 147,
                     giRefusedSig: 148, giRefusedSelf: 149, giRefusedLate: 150,
                     /* not clauses and not size: NOINLINING, SYNCHRONIZED, gsharedvt, depth, cctor. */
                     giRefusedOther: 151,
                     /* nonscalar split: byref is the layout-compatible one. */
                     delegateSlowVtret: 152, delegateSlowVtarg: 153, delegateSlowByref: 154,
                     /* R310 profile-delivery census (WJC_GI_PROF_* 323..330, WJC_PD_* 331..337). Indices 155..322
                      * are NOT mirrored here; these are appended at their real positions so the window delta works. */
                     giProfNorec: 323, giProfNorecFull: 324, giProfCold: 325, giProfPoly: 326, giProfPolySameTgt: 327,
                     giProfInl: 328, giProfInlRec: 329, giProfInlNoim: 330,
                     pdMiss: 331, pdMissNosite: 332, pdMissFirst: 333, pdMissSameId: 334, pdMissSameTgt: 335,
                     pdMissDiffTgt: 336, pdResolve: 337,
                     /* R314 (L1) retired-InterpMethod census, WJC_RETIRED_* 347..352 + WJC_RESID_LIVE_CALLEE 353
                      * (R311's 338..346 are not mirrored). */
                     retiredIcHit: 347, retiredFastmiss: 348, retiredLateFslot: 349, retiredDelegate: 350,
                     retiredForwarded: 351, retiredFwdLive: 352, residLiveCallee: 353,
                     /* R315 inline-policy census, WJC_INLINE_* 354..360 */
                     inlineAccepted: 354, inlineCallsLifted: 355, inlineRelinkRefused: 356, inlineStackwalkRefused: 357,
                     inlineClauseRecheck: 358, inlineDowngrade: 359, inlineDowngradeOk: 360,
                     /* R316 tier-2 census, WJC_T2_* 361..365 */
                     t2Samples: 361, t2Requested: 362, t2Compiled: 363, t2Failed: 364, t2Refused: 365,
                     /* R316b profile origin, WJC_PROF_ORIGIN_* 366..367 */
                     profOriginUsed: 366, profOriginFallback: 367,
                     /* R317 fast TLS, WJC_FAST_TLS_SITE 368 */
                     fastTlsSite: 368,
                     /* R316c tier-2 v2, WJC_T2_GIVEUP / _GI_EMITTED / _GI_NOPRED 369..371 */
                     t2Giveup: 369, t2GiEmitted: 370, t2GiNopred: 371,
                     /* R318 cold-throw inline sizing, WJC_INLINE_COLD_THROW_ADMIT 372 */
                     inlineColdThrowAdmit: 372,
                     /* R316i tier-2 GI no-prediction split, WJC_T2_GI_NOREC/COLD/POLY 373..375 */
                     t2GiNorec: 373, t2GiCold: 374, t2GiPoly: 375,
                     /* R319 BFI-uninitialized inline admit, WJC_INLINE_BFI_UNINIT 376 */
                     inlineBfiUninit: 376,
                     /* R316j split-profile probe, WJC_T2_GI_NOREC_CANON 377 */
                     t2GiNorecCanon: 377,
                     /* R316k root/inlinee split, WJC_T2_GI_SITE_ROOT .. _INL_POLY 378..382 */
                     t2GiSiteRoot: 378, t2GiInlNoim: 379, t2GiInlNorec: 380, t2GiInlCold: 381, t2GiInlPoly: 382,
                     /* R320 admission payload seqlock catch, WJC_ADMIT_PAYLOAD_TORN 383 */
                     admitPayloadTorn: 383,
                     /* R321 ldaddr of ref/byref locals, WJC_LDADDR_REF_LOCAL 384 */
                     ldaddrRefLocal: 384,
                     /* R322 validate-only re-emits, WJC_REEMIT_VALIDATE_ONLY 385 */
                     reemitValidateOnly: 385,
                     /* R323 lowerings, WJC_OVF_CONV_LOWERED / WJC_ATOMIC_STORE8_LOWERED 386..387 */
                     ovfConvLowered: 386, atomicStore8Lowered: 387,
                     /* R322 probe, WJC_REEMIT_HAZARD 388 (ungated) */
                     reemitHazard: 388,
                     /* R327 tier-2 retry, WJC_T2_RETRY_PARKED / _GIVEUP 389..390 (ungated) */
                     t2RetryParked: 389, t2RetryGiveup: 390,
                     /* R329 tier-2 static GI prediction, WJC_T2_GI_STATIC_PRED / _EMITTED 391..392 */
                     t2GiStaticPred: 391, t2GiStaticEmitted: 392,
                     /* R332 tier-2 re-arm after BUSY give-up, WJC_T2_REARMED 393 (ungated) */
                     t2Rearmed: 393,
                     /* R338 co-location skipped under tier 2, WJC_COLOCATE_T2_SKIP 394 (ungated) */
                     colocateT2Skip: 394, branchHintOol: 395, branchHintIc: 396};
  /* WAS busySelf: 61, busyOther:64 . Those compile-lock counters DO NOT EXIST in this runtime's WJC_*
   * enum -- jitbench.ts removed its own copy of them for exactly this reason and this mirror was not
   * updated. Index 63 is WJC_ESLOT_RESIDUAL and 64 is WJC_CALL_LOCAL, so every "lock busy" figure ever
   * printed from this harness was an unrelated counter under a wrong name. WJC_MAX is 84. */
  const snap = () => { const o = {}; for (const k in COUNTERS) o[k] = wj(COUNTERS[k]); return o; };
  /* Call-profile health, read straight from mono_wasm_jit_prof_stat (EMSCRIPTEN_KEEPALIVE, and
   * deliberately NOT behind MONO_WASM_JIT_STATS). Fields: 0 samples, 1 sites, 2 methods, 3 evicted,
   * 4 post-JIT samples, 5 site ids reused. Absolute values, not deltas: the question is what the
   * profile looks like by the end of the window, not how much it moved inside it. */
  /* DEVIRT CENSUS, absolute rather than a window delta. These are EMIT-time counters: a delta over the
   * window would only describe the methods that happened to compile inside it, which at plateau is a
   * small and unrepresentative tail. Indices from the WJC_* enum (69..80); WJC_MAX is 84. */
  const devirtStat = () => {
    const names = { sites: 66, noRec: 67, cold: 68, poly: 69, poly90: 70, sig: 71, noFslot: 72,
                    emitted: 73, delegateSites: 74, fastDevirt: 75, forced: 76, forceCapped: 77};
    const o = {}; let any = false;
    for (const k in names) { const v = wj(names[k]); o[k] = v; if (v) any = true; }
    return any ? o : null;
  };
  const profStat = () => {
    const f = M && M._mono_wasm_jit_prof_stat;
    if (typeof f !== "function") return null;
    const names = ["samples", "sites", "methods", "evicted", "jitSamples", "idsReused"], o = {};
    for (let i = 0; i < names.length; i++) o[names[i]] = f(i);
    return o;
  };
  /* "the export is missing" and "the counters read zero" need opposite responses -- rebuild versus set
   * MONO_WASM_JIT_STATS=1 -- and a guarded read collapses both to null. Record which it is once. */
  const countersAvailable = typeof (M && M._mono_wasm_jit_get_counter) === "function";
  const key = (type, code, k) => document.dispatchEvent(new KeyboardEvent(type,
    { code, key: k, repeat: false, bubbles: true, cancelable: true }));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  const live = g(() => globalThis.jitLiveness && globalThis.jitLiveness());
  /* HARD PRECONDITION: auto=1 but registered=0 means the wasm-JIT tier compiled NOTHING, and that
   * presents as "slower fps" rather than as an error. It cost a whole session: an emitter change added two
   * imported globals to the module without bumping the import section's entry COUNT, so every module was
   * rejected with "CompileError: section was shorter than expected size", registered sat at 0 from boot,
   * and three interleaved A/Bs were run and reported (delegate PIC, nursery size, JIT threshold) before
   * anyone read a counter. All three were void: both arms of each had no JIT.
   * deploy.sh cannot catch this -- it proves the new code is in the served bytes, not that it still works.
   * So the window refuses to open, loudly, instead of producing a plausible number. */
  if (live && live.auto > 0 && live.registered === 0) {
    const msg = "[mcw] ABORT: wasm-JIT is enabled (auto=" + live.auto + ") but registered=0 -- the tier "
      + "compiled NOTHING. Every number from this run would describe interp+jiterpreter only. "
      + "Check for a module CompileError with ?wj.verbose=1 (the reason is swallowed into WJC_INVALID).";
    console.log(msg);
    return { error: msg };
  }
  /* WARM-UP HOLD before the window opens (warmMs).
   *
   * Round 97/98: the in-game ramp runs for ~180 s, so a 60 s window measured almost pure warmup and every
   * A/B in Rounds 63-96 ranked changes on a warmup metric. Holding here -- rendering and, if walking, moving
   * -- lets the ramp finish so the window that follows measures the PLATEAU, i.e. steady-state codegen.
   * The [mcw] START marker is still emitted at the real window start, so perf slicing stays correct. */
  if (${warmMs} > 0) {
    console.log("[mcw] WARM " + JSON.stringify({ warmMs: ${warmMs}, walk: ${walk} }));
    if (${walk}) { key("keydown","ControlLeft","Control"); key("keydown","KeyW","w"); }
    const wt0 = performance.now();
    let wStrafe = null;
    while (performance.now() - wt0 < ${warmMs}) {
      await sleep(1000);
      if (${walk}) {
        const phase = Math.floor((performance.now() - wt0) / 4000) % 4;
        const want = phase === 1 ? "KeyD" : phase === 3 ? "KeyA" : null;
        if (want !== wStrafe) {
          if (wStrafe) key("keyup", wStrafe, wStrafe === "KeyD" ? "d" : "a");
          if (want) key("keydown", want, want === "KeyD" ? "d" : "a");
          wStrafe = want;
        }
      }
    }
    if (${walk}) { if (wStrafe) key("keyup", wStrafe, wStrafe === "KeyD" ? "d" : "a"); for (const [c, k] of [["KeyW","w"],["ControlLeft","Control"]]) key("keyup", c, k); }
  }
  const f0 = frames(), c0 = snap(), t0 = performance.now();
  console.log("[mcw] START " + JSON.stringify({ durationMs: ${durationMs}, walk: ${walk}, frames: f0, live }));
  ${dumps ? 'g(() => globalThis.dumpWasmJit && globalThis.dumpWasmJit());' : ""}

  const held = [["KeyW","w"],["ControlLeft","Control"],["KeyD","d"],["KeyA","a"]];
  const fps = [];
  const reg = [];          // registered-method count, sampled every 5th second (see the loop below)
  let last = f0, lastT = t0, strafe = null;

  /* FRAME-TIME DISTRIBUTION, not just a mean.
   *
   * Mean fps over this window has been actively misleading: every run RAMPS (6 -> 12.6 fps over 210s), so the
   * mean reports where in the ramp the window happened to sit. Worse, a mean cannot distinguish the two failure
   * shapes that need opposite fixes -- uniformly slow frames (a per-frame serialisation or latency cost) versus
   * mostly-fast frames punctuated by long stalls (GC pauses, blocking I/O). Those look identical at 12 fps.
   *
   * So: poll the frame counter on a short interval and record the GAPS between observed increments. The counter
   * is the emscripten GLFW frame count, i.e. the same source the fps figure uses, so the two cannot disagree.
   * Resolution is the poll interval, which bounds how small a frame time this can resolve -- fine, because the
   * interesting frames here are 50ms+. Sampling in its own loop rather than the 1s loop below keeps the walk
   * keystroke pacing unchanged. */
  const gaps = [];
  let pollLast = f0, pollT = t0, polling = true;
  const poller = (async () => {
    while (polling) {
      await sleep(4);
      const n = performance.now(), f = frames();
      if (f != null && f > pollLast) {
        /* Several frames can land inside one poll interval; spread the elapsed time over them rather than
         * attributing it all to one, which would invent a stall that did not happen. */
        const k = f - pollLast, dt = (n - pollT) / k;
        for (let i = 0; i < k && gaps.length < 200000; i++) gaps.push(dt);
        pollLast = f; pollT = n;
      }
    }
  })();

  const pct = (arr, p) => arr.length ? Number(arr[Math.min(arr.length - 1, Math.floor(arr.length * p))].toFixed(2)) : null;

  try {
    if (${walk}) { key("keydown","ControlLeft","Control"); key("keydown","KeyW","w"); }
    while (performance.now() - t0 < ${durationMs}) {
      await sleep(1000);
      const now = performance.now(), f = frames();
      // Per-second fps, not just an average: a mean of 20 hides a run that renders 55 for four seconds and
      // then 2 for the rest, and on this workload that difference is the entire finding.
      if (f != null) fps.push(Math.round(1000 * (f - last) / (now - lastT)));
      last = f; lastT = now;
      /* JIT REGISTRATION over time, sampled in the clean run.
       *
       * Round 97 found the in-game ramp is ~150s long, so every 60s window ever measured here was almost
       * entirely warmup -- which makes "how long does the tier take to finish compiling" the question that
       * decides whether the ramp is compile-bound or is just the game's own warmup. registered comes from
       * mono_wasm_jit_liveness, which reads process state rather than a WJC_* counter, so unlike every other
       * JIT number it is available WITHOUT MONO_WASM_JIT_STATS -- i.e. in a run whose timings are valid.
       * One wasm call per second cannot move a 38ms frame. */
      if ((fps.length % 5) === 0) reg.push(g(() => globalThis.jitLiveness().registered) ?? -1);
      if (${walk}) {
        const phase = Math.floor((now - t0) / 4000) % 4;
        const want = phase === 1 ? "KeyD" : phase === 3 ? "KeyA" : null;
        if (want !== strafe) {
          if (strafe) key("keyup", strafe, strafe === "KeyD" ? "d" : "a");
          if (want) key("keydown", want, want === "KeyD" ? "d" : "a");
          strafe = want;
        }
      }
    }
  } finally {
    if (${walk}) for (const [c, k] of held) key("keyup", c, k);
    polling = false;
    try { await poller; } catch (e) { }
  }
  const t1 = performance.now(), f1 = frames(), c1 = snap();
  const sorted = gaps.slice().sort((a, b) => a - b);
  /* Share of wall time spent in frames slower than 100ms: the single number that says "stalls" vs "uniformly
   * slow". If p50 is small but this is large, the mean is being set by a few very long frames. */
  const slowMs = gaps.reduce((s, g) => s + (g > 100 ? g : 0), 0);
  const frameTime = {
    n: sorted.length,
    p50: pct(sorted, 0.50), p90: pct(sorted, 0.90), p95: pct(sorted, 0.95), p99: pct(sorted, 0.99),
    min: pct(sorted, 0), max: sorted.length ? Number(sorted[sorted.length - 1].toFixed(2)) : null,
    over100msPctOfWall: gaps.length ? Number((100 * slowMs / (t1 - t0)).toFixed(1)) : null,
  };
  const wall = t1 - t0, nf = (f1 != null && f0 != null) ? f1 - f0 : null;
  const d = {}; for (const k in c1) d[k] = (c1[k] == null || c0[k] == null) ? null : c1[k] - c0[k];
  ${dumps ? 'g(() => globalThis.dumpWasmJit && globalThis.dumpWasmJit());' : ""}
  console.log("[mcw] END " + JSON.stringify({
    wallMs: Math.round(wall), frames: nf,
    fps: nf != null ? Number((nf / (wall / 1000)).toFixed(2)) : null,
    msPerFrame: nf > 0 ? Number((wall / nf).toFixed(2)) : null,
    /* ZERO FRAMES IS A VOID RUN, NOT A SLOW ONE, and it has to say so. A --stats run reported "0 frames /
     * 0 fps" alongside a full set of phase timings and a plausible heap figure; nothing in the output
     * distinguished that from a measurement, and its all-zero JIT counters were then reported as "stats not
     * enabled" when the real cause was that the render thread had died at world-join (the intermittent
     * worker crash) and nothing was executing at all. */
    void: nf === 0 || nf == null || !!settle.serverDead, settle,
    fpsPerSecond: fps, registeredPer5s: reg, frameTime, deltas: d, countersAvailable, prof: profStat(), devirt: devirtStat(),
    heapMb: g(() => Math.round((M.wasmMemory?.buffer ?? globalThis.getDotnetRuntime(0).localHeapViewU8().buffer).byteLength / 1048576)),
    /* THE WASM FUNCTION TABLE IS PER-WORKER, so its length is multiplied by every retained isolate.
     * V8 stores each entry twice: a WasmDispatchTable slot (kEntrySize = 12 B under pointer
     * compression, wasm-objects.h:981) plus a tagged reference in WasmTableObject's
     * entries FixedArray (~4 B), and WasmDispatchTable::Grow DOUBLES capacity (wasm-objects.cc:2450-2456), so
     * the committed cost is up to ~2x the live length. This is also the exact object the 2026-09-08 OOM
     * died allocating (Factory::NewWasmDispatchTable <- WasmTableObject::Grow), so the length is worth
     * carrying in every run rather than reconstructing it after the next crash. */
    /* Per-table reservation usage for the 38 jiterpreter tables (trace, jit_call, then 36 interp-entry
     * shape buckets). Sizes are a pure per-worker reservation, so this is the data that says which of
     * them actually need room -- used can EXCEED capacity, because allocate_table_entry bumps the index
     * before noticing it overflowed, which is what makes it a demand signal rather than a fill level. */
    tableUse: g(() => Array.from({ length: 36}, (_, i) =>
      [M._mono_jiterp_get_table_used(i), M._mono_jiterp_get_table_capacity(i)])),
    tableLen: g(() => (M.wasmTable ?? M.wasmExports?.__indirect_function_table
                       ?? M.asm?.__indirect_function_table ?? globalThis.wasmTable)?.length ?? null),
  }));
})()`;

/*
 * OPFS BLOCKED-TIME POLLER, injected once and left running for the rest of the run.
 *
 * `wasmfs_opfs_stat` (statics/emsdk .../opfs_backend.cpp) accumulates, per op kind, the count, the total
 * NANOSECONDS the calling thread spent blocked in the cross-thread round trip, the bytes moved, and a
 * latency histogram. This is the number R12 never took: it had the op COUNT (~100k round trips) and never
 * converted it to seconds, so the cost was known only to within ~100x.
 *
 * WHY A POLLER IN THE PAGE RATHER THAN CDP READS AT PHASE BOUNDARIES. mcdrive's header is explicit that
 * CDP becomes unreliable once a world is loaded -- emscripten pthreads park in blocking wasm and
 * Runtime.evaluate never returns for those targets. The stderr log is the only channel trustworthy end to
 * end. So this prints into the log on an interval and the pump correlates by timestamp, exactly like every
 * other phase signal here.
 *
 * WHAT IT DOES NOT COVER: it is injected after `titleReady`, because that is where CDP is first attached
 * and attaching earlier would change the boot it is supposed to measure. The counters are CUMULATIVE, so
 * the first line still gives boot's exact totals -- what is lost is the split WITHIN boot. That split
 * still comes from the coarse OPFS_STATS read checkpoints (see summariseOpfs), so the two instruments are
 * complementary rather than redundant: checkpoints give within-boot attribution at 8192-read granularity,
 * this gives exact totals and the only latency distribution available.
 *
 * Degrades to a single "unavailable" line if the export is missing, which is the case worth telling apart
 * from "the counters read zero" -- one needs a rebuild, the other means no I/O happened.
 */
const OPFS_POLL_JS = (everyMs) => `(() => {
  const M = globalThis.wasm && globalThis.wasm.Module;
  const f = M && M._wasmfs_opfs_stat, shape = M && M._wasmfs_opfs_stat_shape;
  if (typeof f !== "function" || typeof shape !== "function") {
    console.log("OPFS_BLOCKED unavailable -- Module._wasmfs_opfs_stat is not exported by this build");
    return "unavailable";
  }
  const KINDS = shape(0), BUCKETS = shape(1);
  const edges = []; for (let i = 0; i < BUCKETS - 1; i++) edges.push(shape(2 + i));
  const NAMES = ["read","write","getSize","setSize","flush","open","close","dir"];
  const dump = () => {
    const o = { edges, kinds: {} };
    for (let k = 0; k < KINDS; k++) {
      const n = f(k, 0);
      if (!n) continue;
      const lat = []; for (let b = 0; b < BUCKETS; b++) lat.push(f(k, 3 + b));
      o.kinds[NAMES[k] || ("k" + k)] = { n, ms: +(f(k, 1) / 1e6).toFixed(3), mb: +(f(k, 2) / 1048576).toFixed(2), lat };
    }
    /* kind -1 is the sum over all kinds, computed in C. Recording it beside the per-kind rows makes
     * "do the parts sum to the whole" checkable rather than asserted -- the failure mode that has cost
     * this project four rounds. */
    o.allN = f(-1, 0); o.allMs = +(f(-1, 1) / 1e6).toFixed(3);
    /* The negative lookup cache (FNA-WASM-Build emsdk.9.patch): misses answered without a round trip (NOT in
     * the dir row above, which counts only lookups that went to the browser), misses recorded, invalidations
     * (JS writes behind wasmfs's back), sets dropped at the per-directory cap. Absent on builds without it. */
    const g = M._wasmfs_opfs_negative_stat;
    if (typeof g === "function") o.neg = { hits: g(0), inserts: g(1), invalidates: g(2), overflows: g(3) };
    console.log("OPFS_BLOCKED " + JSON.stringify(o));
  };
  dump();
  setInterval(dump, ${everyMs});
  return "polling";
})()`;

/*
 * JIT ADMISSION COUNTERS, POLLED PER PHASE -- the other half of the world-generation stall test.
 *
 * WHY THE EXISTING `deltas` BLOCK CANNOT ANSWER THIS. It is snapshotted at the in-game window's start and
 * end, so it describes the window and nothing else. The bimodal +60 s stall is in `world.generate`, which
 * is over before the window opens, so every admission counter this project has ever recorded is blind to
 * the phase where the variance lives.
 *
 * THE HYPOTHESIS THIS TESTS. R166: `mono_wasm_jit_admit`'s cycle-break returns 1 while `desc_admitted`
 * requires 2, so a worker that admits a second member of a batch it already brought up can be left with
 * `admit_live` == 0 for the rest of its life -- and every virtual call on that worker then goes to the
 * interpreter (`vfbNotLive` read 106.7M/120 s when it fired). That is per-thread, all-or-nothing and
 * silent, which is the exact shape of the stall: same work, ~2.7x slower, no log line, knob-independent.
 * If it lands on a chunk-generation worker, generation is interpreted for the whole phase.
 *
 * So: `vfbNotLive` accumulating across `world.generate` in stall runs and staying near zero in fast runs
 * would confirm it outright, and the `al*` split then names which arm of `admit_live` refused.
 *
 * THE IDENTITIES ARE THE POINT, not the individual numbers. R166 cost a session precisely because the
 * fourth arm of the vfbThresh split was a bare `default: break;` -- `vfbThresh` read 9,243x the control
 * while its own sub-counters summed to 1,399, and nothing in the harness could see the discrepancy.
 * Three are asserted per poll: thresholdSplitOk, sccSplitOk and depNotLiveSplitOk (see the dump below).
 * A FOURTH used to be asserted here against `vfbNotLive` and was never checkable -- different
 * denominators -- so it read false forever and nobody noticed. An identity that cannot hold is not a
 * weaker gate than none; it is a worse one, because its failure carries no information.
 *
 * REQUIRES MONO_WASM_JIT_STATS=1 (`--stats`). Without it every counter reads 0 by design, and a --stats
 * run is measurably slower, so read COUNTS from it and TIMINGS from a clean run -- never the same run.
 */
const ADMIT_POLL_JS = (everyMs) => `(() => {
  const M = globalThis.wasm && globalThis.wasm.Module;
  const f = M && M._mono_wasm_jit_get_counter;
  if (typeof f !== "function") {
    console.log("JIT_ADMIT unavailable -- Module._mono_wasm_jit_get_counter is not exported by this build");
    return "unavailable";
  }
  /* Indices mirrored from jitbench.ts's WJ enum, same source as the WINDOW_JS table. Deliberately a SMALL
   * subset: this prints every interval for the whole run, and dumping all ~150 counters would bury the log
   * that phase detection reads. */
  const IDX = { registered: 0, bailed: 1, vfbThresh: 12, vfbPerm: 13,
                vfbCold: 39, vfbParked: 40, vfbRetry: 41, vfbNotLive: 83,
                alAdmit0: 84, alState: 85, alGen: 86, alELive: 87, alFLive: 88,
                /* R230 entry-gate guard. NON-ZERO IS HEALTHY: it is the guard catching R209's torn read
                 * before the call_indirect that would otherwise hit the jiterpreter's prefilled
                 * placeholder and trap. Zero forever means the guard is dead code. */
                entrySlotStale: 155,
                /* R231. The three causes of an admission ABI refusal, split out of the printf whose
                 * ungated mono_method_get_full_name walk was trapping ~1 A/B run in 6. Non-zero is
                 * normal: the refusal is recoverable and the next dispatch retries. */
                abiMismatchUnreg: 156, abiMismatchChunk: 157, abiMismatchSig: 158,
                /* R234. NON-ZERO IS THE POINT: it counts caller/callee pairs whose 32-bit functype hash
                 * matched while the METHOD did not -- i.e. calls that used to reach call_indirect and
                 * trap with a function-signature mismatch. Zero means the collision is not happening in
                 * that run, not that the guard is useless. (No backticks in here: this object is inside
                 * an injected-JS TEMPLATE LITERAL and a backtick ends it.) */
                abiMismatchIdent: 159,
                admitDepNotLive: 160,
                /* R245. admitDepNotLive SPLIT BY CAUSE, which is the whole point of the split: cycle is
                 * the dep admitted through the state-1 cycle break, which installs but never publishes
                 * liveness, so the refusal recurs on EVERY dispatch forever -- that is R244's 13.6-22.8
                 * M/run regression. pending is the ordinary transient case the next dispatch clears.
                 * cycle + pending == admitDepNotLive; if it does not, a third cause exists. */
                admitDepNotLiveCycle: 166, admitDepNotLivePending: 167,
                /* R245. The admission refusal routes that had NO counter at all -- on HEAD they were ~45%
                 * of alAdmit0, which is why admitDepNotLive looked like the whole story when it was barely
                 * half of it. depAdmit0 is propagation (the dep counted its own reason at its own exit);
                 * the rest are mono_wasm_jit_admit's own early exits. permfail NON-ZERO IS HEALTHY: it is
                 * R167's bound stopping an unbounded retry, not a failure. */
                admitDepAdmit0: 161, admitBadId: 162, admitNoEntry: 163,
                admitPermfail: 164, admitState3: 165,
                /* R245. Blocker list overflowed MAX_BLOCKERS (32) while WJ_SCC_MAX is 64, so the batcher
                 * got a closure it cannot close -> !progress -> every member condemned. Non-zero means a
                 * truncation is being converted into a permanent verdict. */
                blockersTruncated: 168,
                /* R245. The SCC batcher had ZERO counters; every outcome was a verbose-only printf, so its
                 * distribution was invisible in a --stats run. READ sccCondemned FIRST: it counts members
                 * marked permanently un-JITtable by a give_up abort, INCLUDING ones that compiled fine --
                 * the island driver fails only the offending callee, this driver condemns the group.
                 * sccAttempt == sccOk + sccBusy + sccTable + sccBudget + sccAllocFail + sccSeedPerm +
                 * sccTooLarge + sccNoProgress + sccIterCap, or an exit is uncounted. */
                sccAttempt: 169, sccOk: 170, sccMembers: 171, sccCondemned: 180,
                sccBusy: 172, sccTable: 173, sccBudget: 174, sccAllocFail: 175,
                sccSeedPerm: 176, sccTooLarge: 177, sccNoProgress: 178, sccIterCap: 179,
                sccResvBypass: 181, sccColocateOk: 182, sccColocateFail: 183,
                /* R245. Islands started by the promotion drain rather than a threshold crossing -- the
                 * drain runs on EVERY safe point with a FRESH budget per queue entry and none of it
                 * reached WJC_ISLAND_ATTEMPT before this. */
                islandDrainAttempt: 184, islandDrainCompleted: 185,
                /* R245 Stage 2. cycleBreakInstall counts DFS back edges taking the cycle break. Common
                 * and healthy; the point is that admitDepNotLive should no longer follow from them, now
                 * that the dependency test asks for INSTALLATION rather than liveness.
                 *
                 * sccSpared = members an aborted SCC batch did NOT condemn (the old policy condemned the
                 * whole group). sccParkNoWaiter NON-ZERO IS A PROBLEM: a spared member parked with nothing
                 * to wake it never retries, because PARKED stops the hit counter. */
                cycleBreakInstall: 186, sccSpared: 187, sccParkNoWaiter: 188,
                /* relinkBailCleared = IKVM swapped in a generation-2 body for a method whose generation 1
                 * had PERMANENTLY bailed, and the -1 was dropped so the emitter judges the new IL. Zero
                 * means either nothing swapped had bailed, or the swap path is not running. */
                relinkBailCleared: 189, relinkRefreshed: 190,
                /* P0. The one admission refusal that had no counter: the cycle break's
                 * wj_install_closure_root failing, which also strands the descriptor at wj_desc_state 1.
                 * Read against cycleBreakInstall. Large or growing is the R244 shape -- each refusal
                 * throws away the whole DFS walk and the next dispatch redoes it. */
                cycleBreakFail: 191,
                /* P1. MUST BE 0. The instantiate path found itself already in a coop BLOCKING state and
                 * skipped the GC-safe bracket around the WebAssembly.Module/Instance construction, so
                 * that instantiation blocked stop-the-world as it did before the bracket existed.
                 * Counted ungated. (NO BACKTICKS IN THIS BLOCK -- it sits inside a template literal that
                 * is evaluated in the page, and a stray backtick terminates the string. That mistake
                 * cost a 3-run batch on 2026-09-12.) */
                jsBlockingSkipped: 192,
                /* P2, the republication rendezvous. Read as an identity, not individually:
                 * rvDrain should be about rvCount x rvDrainThreads -- one drain per thread per
                 * rendezvous. rvDrainThreads is the one that PROVES the mechanism: a rendezvous that
                 * reaches only the thread that called it is not a rendezvous, and a global drain count
                 * cannot tell those apart. rvLogFull non-zero means the feature stopped working part way
                 * through the run. rvRefusedBatched counts co-located members, which cannot be
                 * republished alone because their generation lives on the batch -- expect it LARGE. */
                rvCount: 193, rvMembers: 194, rvDrain: 195, rvDrainThreads: 196,
                rvDrainSlot: 197, rvDrainRefused: 198, rvLogFull: 199, rvRefusedBatched: 200,
                rvDrainSkipped: 201,
                /* P3 SIZING. calleeHasFslot over the four is the CO-LOCATOR'S FIELD OF VIEW: its only
                 * input is re->depset, and a depset entry exists only where the emitter resolved an
                 * f-slot, so everything else is a callee it cannot name -- and therefore cannot group or
                 * force-compile. calleeNoFslotUntried is the re-emission headroom (callee exists, was
                 * merely not compiled yet); Bailed is terminal and Noim never ran. Site-counted, not
                 * execution-weighted -- pair with the colocate-reach triple, which is miss-weighted. */
                calleeHasFslot: 202, calleeNoFslotUntried: 203,
                calleeNoFslotBailed: 204, calleeNoFslotNoim: 205,
                /* MUST BE 0: a memory manager was reached with an uninitialised interp_code_hash, i.e.
                 * the publish-before-initialise race is live. */
                jitmmUninit: 206,
                /* RE-EMISSION. drained must equal compiled+busy+failed+refused+gone, and compiled must
                 * equal republished+noRv. reemitSite is the disagreement detector: compiled > 0 with
                 * reemitSite == 0 means the recompile never reached the emitter (R179 bug #1). */
                reemitQueued: 207, reemitDedup: 208, reemitQfull: 209, reemitDrained: 210,
                reemitBusy: 211, reemitRefused: 212, reemitGone: 213, reemitFailed: 214,
                reemitCompiled: 215, reemitRepublished: 216, reemitNoRv: 217, reemitSite: 218,
                reemitCapped: 219, reemitSlotMoved: 220,
                reemitEmitted: 221, reemitNoRec: 222, reemitNoFslot: 223,
                /* Triggers suppressed because the METHOD was already re-emitted. Expect this to DWARF
                 * reemitQueued once the miss threshold is low: every hot site in an already-re-emitted
                 * body lands here. A zero here with a low threshold means the per-method flag is not
                 * being set, and the population is re-emitting itself in a loop. */
                reemitMethodDone: 224,
                /* Admission DFS recursion depth, high-water and past-64 count. This is the ceiling that
                 * every wedging re-emission arm actually hit (stack overflow in
                 * mono_wasm_jit_admit/wj_admit_dependencies), so it must be read on every arm that
                 * PASSES -- a passing run near the cliff is the warning the wedge cannot give. */
                admitDepthMax: 225, admitDepthOver: 226,
                /* The re-emission boot wedge, caught: the generation reset declining to clear a VISITING
                 * descriptor. NON-ZERO IS HEALTHY -- zero means the guard is dead code. Read next to
                 * admitDepthMax, whose re-emission-OFF control is 22. */
                admitGenResetVisiting: 227,
                /* Candidates abandoned after losing the compile CAS 8 times. Read against reemitBusy. */
                reemitBusyGiveup: 228,
                /* Leaked VISITING markers found at admission depth 0 and cleared. NON-ZERO PROVES THE
                 * LEAK: a stale state 1 routes every admit into the install-only cycle break, so a
                 * dependency stays a table placeholder under a live caller and the emitted call_indirect
                 * traps with a function-signature-mismatch RuntimeError (R264 wall 3). NOTE: this block
                 * is inside a TEMPLATE LITERAL, so a backtick here terminates the string and breaks the
                 * whole driver -- it did. No backticks in this object. */
                admitStaleVisiting: 229,
                /* re->batch changed identity mid-admission, i.e. the group was re-framed under the walk.
                 * NON-ZERO IS HEALTHY and proves the race: admission snapshotted the batch and then
                 * re-read the pointer ~15 times, so a re-frame could pair one module's bytes with
                 * another's slot list and write exports into the wrong methods' slots. (No backticks in
                 * this object -- it is a template literal.) */
                admitBatchSwapped: 230,
                /* Rendezvous drain repairs. rvDrainStale2: the skip arm found state 2 over uninstalled
                 * slots. rvDrainRefreshForced: the re-admit arm had to force state off 2 so admit would
                 * actually re-instantiate instead of taking its state-trusting fast path -- that window
                 * is the R267 trap candidate. NON-ZERO IS HEALTHY for both. (No backticks in this
                 * object -- it is a template literal.) */
                rvDrainStale2: 231, rvDrainRefreshForced: 232,
                /* MONO_WASM_JIT_SINGLE_WRITER. swPublished should track registered when the knob is on
                 * and be 0 when off -- that pair is how you tell the path is actually carrying
                 * admission rather than silently not firing. swInstallFail is transient by construction
                 * (budget or a failed instantiate) and must not grow without bound. */
                swPublished: 233, swInstallFail: 234,
                /* Closure verification refused an admission. The recursive path expressed this as a
                 * refusal propagating up from the offending level; omitting it regressed a clean config
                 * twice. Expect non-zero and BOUNDED -- it is transient by construction. */
                swVerifyFail: 235,
                /* Closure-size high-water for the single writer's install walk. AT the cap means the walk
                 * is truncating and admissions are refused for lack of budget -- which looks like a boot
                 * that never finishes with no fault at all. */
                swClosureMax: 236,
                /* MONO_WASM_JIT_SWEEP time-of-use verification. sweepDepBad non-zero under re-emission and
                 * zero without it means a LEGITIMATE publication is being invalidated after the fact --
                 * which publish-time checking cannot see, and sw_verify_fail=0 proves it is not seeing. */
                sweepRuns: 237, sweepSelfDead: 238, sweepDepBad: 239,
                /* Descriptors that lost their f-slot to a re-registration (fslotOrphaned) and admissions
                 * refused for it (admitOrphaned). admitOrphaned NON-ZERO is the catch: workers really were
                 * holding stale admissions of a slot-less descriptor. */
                fslotOrphaned: 240, admitOrphaned: 241,
                /* Re-registrations that reused the existing descriptor instead of minting a new one.
                 * Should carry essentially all re-emission republications, driving fslotOrphaned to ~0.
                 * REUSED high with ORPHANED ~0 is the fix working. */
                fslotReused: 242,
                /* Admissions that finished against a generation which moved mid-walk (re-emission swapped
                 * depset+generation underneath). Non-zero is the window being closed, not an error. */
                admitGenMovedMidwalk: 243,
                /* R245 Stage 2d. The devirt/delegate arms refusing for want of an f-slot. Added because
                 * the first Stage 2 run showed sccNoProgress 0 -> 2 and the cause could only be INFERRED
                 * (these arms were the one change reaching the emitter during an SCC batch) -- the
                 * counters that would have settled it were not in this subset. A jump here alongside
                 * sccNoProgress confirms the arms are the mechanism; flat means look elsewhere. */
                devirtNoFslot: 72, devirtArm2NoFslot: 122, delegateDevirtNoFslot: 136,
                /* THE HANG PROBE. Polled per phase like everything else here, which matters: these are
                 * the counters that say whether a worker is stuck paying a full rendezvous drain at
                 * every loop back-edge, and a per-phase delta localises it to world.generate or in-game
                 * rather than leaving it as a whole-run total.
                 *
                 * Appended at 271+, i.e. immediately before WJC_MAX, so no existing index moved and
                 * wjcsync.py did not need to run (see its header: append-only changes shift nothing).
                 * Indices were derived by parsing the enum, not counted by hand.
                 *
                 * READING: pubReraiseMax in single digits is health -- a publication is adopted within a
                 * poll or two. Growing without bound is the pathology, and rvRetryDropped /
                 * rvRetryTryMax / workerSlotsFull then say which of the three documented causes it is.
                 * rvRetryDropped NON-ZERO IS HEALTHY: it counts workers that did not get pinned. */
                retireSpins: 271, retireSpinMax: 272, retireTrylockMiss: 273,
                reemitQSpins: 274, reemitQSpinMax: 275,
                rvRetryDropped: 276, rvRetryTryMax: 277, pubReraiseMax: 278,
                /* The retained-MonoMethod* exposure. permSetAdds is the LIVENESS CHECK -- 0 means the set
                 * is never populated and every other value here is uninterpretable. permSetFull must stay
                 * 0 (non-zero = the predicate silently changed answer). canonSubst is the discriminator
                 * for whether wj_sync_inner_canon still needs a purge-on-teardown fix. */
                permSetAdds: 279, permSetHits: 280, permSetFull: 281, permSetStale: 282,
                canonSubst: 283,
                /* Deps only the relocations knew about; 0 = the emit-time recording was already complete. */
                depsFromRelocs: 284,
                /* Deterministic compile failures, refused ONCE instead of retried forever (R289).
                 * localsOverflow: refused before emission for exceeding V8's params+locals cap of 50,000
                 * -- expect a small non-zero (BlockStateFlattening:.cctor). invalidPerm: instantiation
                 * returned a CompileError, so the bytes are invalid and re-emitting reproduces them.
                 * invalidPerm non-zero with localsOverflow zero = a different codegen bug emitting
                 * invalid modules; read the WASM_JIT_INVALID line for the method and V8's message. */
                localsOverflow: 285, invalidPerm: 286};
  const dump = () => {
    const o = {};
    for (const k in IDX) { try { o[k] = f(IDX[k]); } catch (e) { o[k] = null; } }
    /* Computed here rather than in the reader so a violation is visible in the raw log, which is the one
     * channel that survives a wedged renderer. */
    o.thresholdSplitOk = (o.vfbCold + o.vfbParked + o.vfbRetry + o.vfbNotLive) === o.vfbThresh;
    /* The al* identity that used to live here is DELETED, not fixed. It asserted
     *     alAdmit0 + alState + alGen + alELive + alFLive === vfbNotLive
     * and CANNOT hold by construction: the al* arms count refusals across all EIGHT
     * mono_wasm_jit_admit_live call sites, while vfbNotLive counts only the one in the vcall residual.
     * Different denominators -- on HEAD alAdmit0 alone ran 12x vfbNotLive. It read false on every run
     * for as long as it existed and nothing looked, which is worse than not having it: a gate nobody
     * reads is indistinguishable from one that passes. There is no counter for total admit_live
     * failures, so there is no identity to repair it into; adding one is Stage 1 work.
     *
     * The two below CAN hold. sccSplitOk: the batcher has one entry point and nine terminal exits, all
     * counted, so attempts must equal their sum -- false means an exit was added without a counter,
     * which is the failure mode that hid R244's regression. depNotLiveSplitOk: every dep-not-live
     * refusal is either the cycle-break case or the ordinary pending one, so false means a third cause
     * exists that nothing names. */
    o.sccSplitOk = (o.sccOk + o.sccBusy + o.sccTable + o.sccBudget + o.sccAllocFail + o.sccSeedPerm +
                    o.sccTooLarge + o.sccNoProgress + o.sccIterCap) === o.sccAttempt;
    o.depNotLiveSplitOk = (o.admitDepNotLiveCycle + o.admitDepNotLivePending) === o.admitDepNotLive;
    console.log("JIT_ADMIT " + JSON.stringify(o));
  };
  dump();
  setInterval(dump, ${everyMs});
  return "polling";
})()`;

/**
 * Boot the game, enter the world, run a fixed in-game window, and return everything needed to slice a
 * profile by phase.
 *
 * `hooks` lets a profiling caller act at phase boundaries without this function knowing anything about
 * perf: onMarker(name, mono) fires for each first-match marker, onPhase({name,t0,t1}) when a phase closes.
 *
 * @returns {Promise<{ marks, phases, subphases, spawnMs, benchSummary, faults, ok, log }>}
 */
export async function runSession ({
  url,
  profile,
  logPath,
  jsFlags = null,
  extraArgs = [],
  /* Absolute path for a CDP-driven memory-infra trace; null disables. See startMemTrace. */
  memTracePath = null,
  memDumpEveryMs = 15000,
  headless = true,
  benchMs = 60000,
  warmMs = 0,
  walk = true,
  /* The runtime's own printf dumps (dumpWasmJit: bail histogram, island blockers, hot entry edges) at
   * both ends of the window. Off by default because they are the exact calls that throw `unwind` from the
   * page -- guarded here, so the cost of leaving them on is a lost dump, not a lost run -- and because
   * they print thousands of lines through log.ts's DOM proxy right at the window boundary. */
  dumps = false,
  /* Seconds of TOTAL log silence that means the runtime wedged. 90s is comfortably above the longest
   * legitimate gap in a healthy boot or world load (chunk-gen percentage lines, class-loader lines and
   * chromium's own GL messages all keep it well under 30s) and well below the 420/480s budgets. */
  /* How long the integrated server must go without logging tick lag before the in-game window opens, and
   * how long to wait for that. 20s/120s: measured runs go quiet within ~30-60s of `inworld` when world
   * generation behaved, and never within 120s when it did not -- which is exactly the case worth flagging
   * rather than waiting out. */
  /* Env-overridable, because a DIAGNOSTIC configuration is legitimately much slower than a measured one and
   * must not be scored as a hang. MONO_WASM_JIT_OBJGUARD/STOREGUARD emit a C call per ref store, so world
   * generation under them takes far longer than the 50-150s it normally does; without a way to raise the
   * budget the guard arm times out and the experiment reports nothing. */
  settleQuietSecs = Number(process.env.MC_SETTLE_QUIET || 20),
  settleMaxSecs = Number(process.env.MC_SETTLE_MAX || 120),
  stallSeconds = Number(process.env.MC_STALL_SECS || 90),
  /* App-output silence. See the two-clock note where lastAppAt is declared: 150 s is 2.2x the worst
   * app-line gap measured on a healthy run (67.3 s over 60 large logs). */
  appStallSeconds = Number(process.env.MC_APP_STALL_SECS || 150),
  /* mcperf copies the map into its outdir and its symbolmap.txt points `perf report` at the /tmp path,
   * so it owns that file's lifetime; everyone else's is dropped at teardown. */
  keepPerfMap = false,
  bootTimeout = Number(process.env.MC_BOOT_TIMEOUT || 420),
  worldTimeout = Number(process.env.MC_WORLD_TIMEOUT || 480),
  /* Menu-navigation timing scale. autoworld.sh documents why this must exist: the Tab/Enter delays were
   * calibrated against the fast AOT build and are simply wrong on a slower configuration, so the sequence
   * lands out of phase and Play is never activated -- which presents as "the game hung at the title screen"
   * with a perfectly healthy render loop. It cost that harness three retries. Here a `major=marksweep` arm
   * reached the main screen and then never left it, and had to be scored VOID rather than as a GC result.
   * Env-exposed because the slow configuration is always a DIAGNOSTIC one chosen on the command line. */
  navScale = Number(process.env.MC_NAV_SCALE || 1),
  /* 2 s is far more often than the counters need -- they are cumulative -- but a line per 2 s is what
   * lets the pump attribute them to a phase boundary to within a couple of seconds. Cost is one wasm call
   * plus one console line per interval, on the main thread, which cannot move a 40 ms frame. */
  opfsPollMs = Number(process.env.MC_OPFS_POLL_MS || 2000),
  /* 1 s: a trap's four lines land inside ~8 ms, and two genuinely independent faults of the same kind
   * within one second would be indistinguishable anyway. */
  faultDedupeSecs = Number(process.env.MC_FAULT_DEDUPE_SECS || 1),
  /* Save and quit AFTER the measurement window, so the run exercises chunk serialisation and the OPFS
   * write path instead of ending in a SIGKILL. Default ON because the whole point is that the pipeline
   * was never exercised; pass false for a run that must end exactly where the old harness ended it. */
  saveQuit = true,
  /* Which seed (lib/seeds.mjs): the launcher's version, the title/join markers, the menu path, the quit Tab count.
   * Must be the seed the profile was copied from. Default MC_SEED, else 262n (DEFAULT_SEED). */
  seed = undefined,
  /* menuSteps: the key presses that take the main screen into a world; default the seed's. */
  menuSteps = null,
  /* Post-window diagnostic: load this many extra idle Emscripten pthread workers and price them
   * within the same renderer. The ordinary memory peak excludes the probe's samples. */
  workerProbeCount = Number(process.env.MC_WORKER_PROBE || 0),
  /* Generous, and separately: the write budget is the one that might legitimately be hit. The four
   * archived runs that ever completed a save took 0.6-2.3 s, so 120 s is ~50x headroom -- if it trips,
   * something is genuinely wrong with the write path rather than merely slow. */
  saveMenuTimeout = Number(process.env.MC_SAVE_MENU_TIMEOUT || 30),
  /* Set to a directory to PROBE the Tab count instead of quitting; see driveSaveQuit. */
  quitProbeDir = process.env.MC_QUIT_PROBE_DIR || null,
  /* MC_SHOTS_DIR: a PNG of the page at titleReady / inworld / benchStart / benchEnd. The reference set a GL-backend
   * change is compared against (plan swirling-moore GL0/GL4): every run of the seed spawns at the same place, so
   * the same marker shows the same view. One CDP call per marker, off the measured window's hot path. */
  shotsDir = process.env.MC_SHOTS_DIR || null,
  saveWriteTimeout = Number(process.env.MC_SAVE_WRITE_TIMEOUT || 120),
  hooks = {},
  log = console.log,
} = {}) {
  const marks = new Map();          // marker name -> monotonic seconds
  const faults = [];
  const lines = [];                 // last N interesting lines, for diagnostics
  const benchLines = [];

  const S = resolveSeed(seed);
  menuSteps ??= S.menu;
  const quitTabs = QUIT_TABS_ENV ?? S.quitTabs;
  log(`[mc] seed ${S.name}: ${S.version ?? "?"} + Fabric ${S.fabric ?? "?"} -- ${S.what}`);
  const game = launchGame({ url, profile, logPath, jsFlags, extraArgs, headless });
  const onInterrupt = () => { game.kill(); process.exit(130); };
  const onTerminate = () => { game.kill(); process.exit(143); };
  process.once("SIGINT", onInterrupt);
  process.once("SIGTERM", onTerminate);
  marks.set("navigate", monotonic());
  hooks.onMarker?.("navigate", marks.get("navigate"));

  const tail = new LogTail(logPath);
  const pending = new Map(markersFor(S).map((m) => [m.name, m]));
  let spawnMs = null;

  let lastLineAt = monotonic(), stalled = false;
  /*
   * A SECOND CLOCK, on app output only. Not a replacement for the one above -- both are needed, and
   * their thresholds differ because they are measuring different things:
   *
   *   lastLineAt  (any line, 90 s)   a browser that has stopped emitting ANYTHING is gone fast.
   *   lastAppAt   (app lines, 150 s) the app is wedged while chromium keeps talking. This is the case
   *                                  the single clock structurally could not see.
   *
   * 150 s IS MEASURED, NOT GUESSED. The largest app-line gap across 60 large healthy logs is 67.3 s
   * (ww-dlfix run-1; median of the per-run maxima is 27.1 s). At 90 s the headroom over that worst case
   * would be 1.3x, which is how a stall detector starts firing on healthy runs -- the exact defect R276
   * records three of. 150 s gives 2.2x and still fires well inside the 420/480 s budgets.
   */
  let lastAppAt = monotonic();
  /* The text of the last app line (R340 follow-up, p69 f1b): a boot stall whose last app output is the net bridge
   * sending a request (`tcpws` / `tx` from src/net.ts) and never logging a reply is the network stall, not a runtime
   * wedge. f1b's main thread was parked in HttpsURLConnection.getResponseCode -> WebSocketStream.Read under a mod's
   * pre-launch <clinit>, with no timeout; it was recorded `wedge` and counted against the build. */
  let lastAppLine = "";
  let terminalFault = null;      // set by the classifier; makes waitFor give up instead of sitting out the budget
  /* Dedicated counters rather than filtering the `lines` ring buffer, which is capped at 400 entries and
   * would silently trim -- making "has the server complained again?" answer no because the evidence aged out. */
  let tickLagCount = 0, tickLagLast = null;
  /* Evidence that a silent log is a MOD-LOADER abort rather than a wedged runtime. Tracked as flags in
   * the line handler because `lines` is filtered to JIT/bench traffic and would never hold a Fabric
   * exception. See the stall classifier in waitFor(). */
  let bootAbortSeen = null, netTimeoutSeen = null;
  let netFromLastLine = false;   /* netTimeoutSeen came only from the last app line naming the net bridge */
  /* The LINKAGE error behind a mod-loader abort, if any. This exists because classifying every Fabric
   * abort as "environment, discard it" FAILS OPEN in the one direction that matters: a linkage error is
   * precisely what a runtime or IKVM codegen regression produces, and discarding it hides the regression.
   * R270 shipped an ungated IKVM CallerID change and the first two runs aborted with `IllegalAccessError`
   * -- which this classifier told me to discard as environmental. Java's linkage family only; an
   * application exception during mod init is a mod problem, not ours. */
  let bootAbortErr = null;
  const opfsSamples = [];           // OPFS_STATS checkpoints, one per 8192 reads (see the pump)
  const opfsBlocked = [];           // OPFS_BLOCKED poller lines: cumulative blocked time per op kind
  const admitSamples = [];          // JIT_ADMIT poller lines: cumulative admission counters (needs --stats)
  const memSamples = [];            // /proc renderer memory samples (see summariseMem)
  let workerProbe = null;
  let memTimer = null, memPid = null;
  let memTraceOn = false, memDumpTimer = null, memDumps = 0, memTraceSaved = false;
  /* SERVER-DEATH markers. Distinct from FAIL_RX on purpose: these decide whether the in-game window is
   * measuring a WORLD or a disconnect screen, which is a different question from "did the run fault". */
  let serverDead = null;
  /* `serverDead` only arms after `inworld`, because before that a shutdown line means something else --
   * so it could not catch the world-load death mode. This one arms from `serverStart`, and it exists
   * because a run whose integrated server stops between `spawnDone` and `inworld` will never produce
   * `inworld` and used to sit out the full 480 s world budget before reporting a plain timeout, which
   * reads as "world load is slow". Measured: runs that die at `spawnDone` are 20 of 522, and 80% of them
   * carry a wasm trap; the rest are Java-level (one was a NullPointerException out of
   * IKVM.Runtime.ByteCodeHelper.DynamicBinderMemberLookup inside Sodium's chunk builder). Either way the
   * world is gone and waiting cannot help -- the same "slower vs wedged" distinction `stallAfter` exists
   * for. */
  let worldDied = null;
  /* Set the instant the driver decides to quit, BEFORE the first keystroke, so a shutdown line that races
   * the keypress is still classified correctly. See SHUTDOWN_RX. */
  let quitting = false;
  /* The fault whose stack frames are currently arriving, or null. */
  let pendingStack = null;
  const pump = () => tail.poll((line, mono) => {
    if (!line) return;
    const t = mono ?? monotonic();
    lastLineAt = monotonic();
    if (APP_LINE_RX.test(line)) { lastAppAt = monotonic(); lastAppLine = line; }
    for (const [name, m] of pending) {
      /* `afterQuit` markers stay disarmed until the driver asks to quit; see the note on the save markers
       * for the autosave that would otherwise consume them mid-world-load. */
      if (m.afterQuit && !quitting) continue;
      if (m.after && !marks.has(m.after)) continue;
      const hit = line.match(m.rx);
      if (!hit) continue;
      pending.delete(name);
      marks.set(name, t);
      if (name === "spawnDone" && hit[1]) spawnMs = Number(hit[1]);
      log(`  ${(t - marks.get("navigate")).toFixed(1).padStart(7)}s  ${name.padEnd(12)} ${m.label}`);
      hooks.onMarker?.(name, t);
      if (shotsDir && SHOT_MARKERS.has(name) && conn && sid) {
        mkdirSync(shotsDir, { recursive: true });
        conn.send("Page.enable", {}, sid)
          .then(() => conn.send("Page.captureScreenshot", { format: "png" }, sid))
          .then((r) => { if (r && r.data) writeFileSync(join(shotsDir, `${name}.png`), Buffer.from(r.data, "base64")); else log(`  !! shot at ${name}: no data`); })
          .catch((e) => log(`  !! shot at ${name} threw: ${e && e.message}`));
      }
      /* DUMP AT MARKERS, not only on a timer. A periodic dump lands wherever it lands, and on this
       * workload that was twice during the synchronous .NET boot -- when the renderer's main thread is
       * inside one long wasm call and cannot service the dump providers, so it contributed 0 allocators
       * both times while every OTHER process in the same trace returned hundreds. Markers are the
       * moments the main thread is demonstrably back in the event loop (it just logged a line the tap
       * read), which is the only kind of moment memory-infra can actually be served. */
      if (memTraceOn && MEMTRACE_MARKERS.has(name)) {
        requestMemDump(conn).then((ok) => { memDumps++; log(`  memtrace: dump at ${name} -> ${ok ? "ok" : "REFUSED"}`); })
          .catch((e) => log(`  !! memtrace: dump at ${name} threw: ${e && e.message}`));
      }
    }
    // The window's own report. Both lines carry a JSON payload; keep them verbatim for the caller.
    if (/\[mcw\] (START|END)/.test(line)) benchLines.push(line.replace(/^.*?\[mcw\] /, "[mcw] "));
    /* OPFS OP COUNTS, which the app has been emitting into every log for a month with nothing reading them.
     * The instrumentation lives in the patched `library_wasmfs_opfs.js` (FNA-WASM-Build/emsdk.4.patch) and
     * is UNGATED, so these lines are free and always present. Capturing them with timestamps is what turns
     * them into a per-phase I/O metric instead of a curiosity.
     *
     * CAVEAT, and it is load-bearing: the counters print only every 8192 READS, so (a) `reads` is always a
     * multiple of 8192 and is a FLOOR, not a total -- the "reads = 98304" quoted in R12 and in
     * opfs_backend.cpp's own comment is 8192 x 12, i.e. the twelfth checkpoint, and the true count is in
     * [98304, 106496) -- and (b) any write after the last read checkpoint is NEVER printed, which is why
     * `writes` has read 3-13 and `flush` 0 for the whole project. That is not evidence the write path is
     * unused. Phase 2 replaces this with a callable export read at phase boundaries. */
    const opfs = line.match(/OPFS_STATS reads=(\d+) readMB=([\d.]+) writes=(\d+) writeMB=([\d.]+) getSize=(\d+) flush=(\d+)/);
    if (opfs) opfsSamples.push({ t, reads: +opfs[1], readMB: +opfs[2], writes: +opfs[3], writeMB: +opfs[4], getSize: +opfs[5], flush: +opfs[6] });
    /* The pull-based blocked-time poller. Greedy `\{.*\}` for the same reason the [mcw] payload parse is:
     * chromium wraps a console message in quotes and appends `", source: <url>` without escaping, so the
     * JSON sits mid-line, and nothing after the outermost brace contains one. */
    const blk = line.match(/OPFS_BLOCKED (\{.*\})/);
    if (blk) { try { opfsBlocked.push({ t, ...JSON.parse(blk[1]) }); } catch { } }
    const adm = line.match(/JIT_ADMIT (\{.*\})/);
    if (adm) { try { admitSamples.push({ t, ...JSON.parse(adm[1]) }); } catch { } }
    /* Faults are RECORDED, never acted on. mcsr aborts on FAIL_RX because it is bisecting crashes, where
     * continuing past the first fault wastes the run. Here the run IS the product: this smoke test hit
     * "worker sent an error! ... memory access out of bounds" during world load -- a pthread dying, which
     * autoworld.sh's own header notes is "never harmless but not always fatal" (one run survived it,
     * generated the world and went on) -- and the game then reached the world and rendered fine. Aborting
     * there would have thrown away a complete profile over a thread that had already died. Report loudly,
     * keep measuring, and let the timeouts catch a game that is actually gone. */
    /* THE STACK BELONGS TO THE FAULT ABOVE IT. WORKER_TRAP_STACK exists only because of the local emsdk
     * patch to worker.js / library_pthread.js, and its whole purpose is to name the faulting function and
     * byte offset -- yet the payload used to survive only in a 400-line ring that NO DRIVER EVER READ, so
     * every recorded fault kept `line.slice(-220)` and threw the stack away. Attach it where it belongs.
     * Capped at 24 frames: --stack-trace-limit=64 is passed process-wide, and the top frames are the ones
     * that matter (V8's default of 10 is what truncated every previously captured trap). */
    if (pendingStack && STACK_FRAME_RX.test(line)) {
      if (pendingStack.stack.length < 24) pendingStack.stack.push(line.trim().slice(-200));
      return;
    }
    pendingStack = null;

    const c = (S.benignRx && S.benignRx.test(line) ? null : classifyLine(line))
      /* SHUTDOWN_RX cannot live in verdict.mjs: whether it is a fault depends on DRIVER STATE, not on the
       * line. Before the driver asks to quit it means the server died; after, it is the thing being
       * measured. A clean quit emits `Stopping singleplayer server` and `lost connection:`, and treating
       * those as faults once scored every successful quit as a failure. */
      || (!quitting && SHUTDOWN_RX.test(line) ? { cls: "server-died", key: "shutdown", sev: "fault" } : null);
    if (c) {
      /*
       * COUNT EVENTS, NOT LINES. One wasm trap emits FOUR matching lines within ~8ms -- the
       * WORKER_TRAP_STACK header, "worker sent an error!", and two "Uncaught RuntimeError" -- so a
       * per-line count reports `faults: ["wasm-trap","wasm-trap","wasm-trap","wasm-trap"]` for a single
       * trap, which is a 4x overstatement in the results file and in every share computed from it. An
       * offline audit of 1,027 logs put it at 105 events across 104 runs, i.e. almost exactly one each;
       * the line count is ~4x that.
       *
       * Same-kind hits inside `faultDedupeSecs` collapse into the first, and `lines` is bumped instead so
       * nothing is silently dropped -- a burst of 40 is still distinguishable from a burst of 4.
       */
      /* Dedupe on cls+KEY, not on cls. The old code collapsed on `kind` alone, so a `function signature
       * mismatch` and a `memory access out of bounds` inside the same second merged into one event with
       * `lines=2` -- two different bugs, one record, and the distinction is the entire diagnostic value.
       * `sig` existed to recover it after the fact and only ever held a prefix. */
      const prev = faults.find((f) => f.cls === c.cls && f.key === c.key);
      if (prev && t - prev.at < faultDedupeSecs) { prev.lines++; pendingStack = prev; return; }
      const f = { cls: c.cls, key: c.key, sev: c.sev, kind: c.cls, at: t, line: line.slice(-220), lines: 1, stack: [] };
      faults.push(f);
      pendingStack = f;
      /* TERMINAL means waiting cannot help. Recorded here, ACTED ON in waitFor -- see the note there:
       * this is the difference between losing one run and losing the rest of the batch. */
      if (c.sev === TERMINAL && !terminalFault) terminalFault = f;
      if (!prev)
        log(`  ${(t - marks.get("navigate")).toFixed(1).padStart(7)}s  !! ${c.sev} ${c.cls} [${c.key}]: ${line.slice(-140)}`);
    }
    if (/Can't keep up/i.test(line)) { tickLagCount++; tickLagLast = line; }
    /*
     * NEVER MATCH A BARE EXCEPTION TYPE NAME. IKVM logs EVERY class it loads:
     *   [IkvmClassLoader] 'net.fabricmc.loader.impl.FormattedException' loaded from URLClassLoader
     * so `FormattedException` fires when the type is LOADED, on a perfectly healthy run. bootcheck.mjs
     * documented this after a first cut scored five arms FAIL on exactly that line, and verdict.mjs
     * carries the guard -- but THIS matcher predates both and was never routed through it.
     *
     * Measured cost: the 20-run soak's one genuine WEDGE (run 14 -- reached javaMain, then 90 s of
     * silence with every thread in __futex_wait) was reported as "NOT a runtime wedge: the log carries a
     * mod-loader/entrypoint abort ... discard it", quoting that class-load line as the evidence. The
     * verdict field said `wedge` and the narrative said `env`; the narrative was wrong, and it is the
     * half a human reads.
     */
    /*
     * FABRIC'S ERROR GUI IS THE POSITIVE SIGNAL, and it is the one class-load line that IS evidence.
     *
     * When an entrypoint throws, Fabric catches it, puts up FabricGuiEntry/FabricStatusTree and then logs
     * nothing further -- the runtime is healthy and the run must be discarded, not counted. Loading those
     * two classes happens ONLY on that path, unlike an exception TYPE name which IKVM logs whenever the
     * type is loaded. Verified on the 20-run soak: 2 lines in the one aborting run, **0 in every healthy
     * run** (checked runs 1, 5, 9, 15, 20).
     *
     * This replaces an accidental detection. `FormattedException` used to match the line where IKVM
     * LOADS that class, which reached the right verdict by the wrong mechanism -- so tightening the
     * type-name matchers (correct on its own terms) would have silently removed the only detection of
     * this failure mode and started reporting mod-loader aborts as runtime wedges. Both halves were
     * needed; shipping either alone makes the classifier worse.
     */
    if (!bootAbortSeen && /FabricGuiEntry|FabricStatusTree/.test(line))
      bootAbortSeen = "Fabric error GUI shown (entrypoint abort): " + line.slice(-140);
    if (!bootAbortSeen && !CLASSLOAD_NOISE_RX.test(line)
        && /Could not execute entrypoint stage|FormattedException|ExceptionInInitializerError/.test(line))
      bootAbortSeen = line.slice(-200);
    if (!netTimeoutSeen && !CLASSLOAD_NOISE_RX.test(line)
        && /TimeoutException: connect took too long|SocketTimeoutException|UnknownHostException/.test(line))
      netTimeoutSeen = line.slice(-200);
    if (!bootAbortErr && !CLASSLOAD_NOISE_RX.test(line)
        && /java\.lang\.(IllegalAccessError|NoClassDefFoundError|VerifyError|LinkageError|IncompatibleClassChangeError|NoSuchMethodError|NoSuchFieldError|AbstractMethodError|ClassFormatError|ClassCircularityError)/.test(line))
      bootAbortErr = (line.match(/java\.lang\.[A-Za-z]+(?:: ?[^"]{0,120})?/) || [null])[0];
    if (!serverDead && !quitting && marks.has("inworld") && SHUTDOWN_RX.test(line))
      serverDead = line.slice(-160);
    if (!worldDied && !quitting && marks.has("serverStart") && !marks.has("inworld") && SHUTDOWN_RX.test(line))
      worldDied = line.slice(-160);
    /* TRAP_RX included so the wasm stack frames survive in `lines`: the WORKER_TRAP_STACK payload names
     * the faulting function and byte offset, which is the whole reason the emsdk patch prints it. */
    if (/\[bench\]|MONO_WASM_JIT|Can't keep up|WASM EXIT/i.test(line) || TRAP_RX.test(line)) {
      lines.push(line.slice(-220));
      if (lines.length > 400) lines.shift();
    }
  });

  /*
   * `stallAfter`: give up early when the log has gone completely silent, rather than burning the full
   * budget. This is not an optimisation, it is the difference between two diagnoses that need opposite
   * responses -- "this configuration is slower" (log still advancing, wait it out) and "the runtime
   * wedged" (nothing at all, no budget will help).
   *
   * It earned its place immediately: the first profiled run generated the world to 99%, logged
   * `Can't keep up! Running 4615ms or 92 ticks behind`, and then a thread trapped inside
   * aot_wrapper_icall_mono_gc_alloc_vector while JSPI-suspended -- the runtime's own message says a trap
   * on a suspended stack silently kills the thread and "GC will then stall trying to suspend it". Output
   * stopped dead. Without this the harness sat there for another five minutes and then reported a plain
   * timeout, which reads as "world load is slow".
   *
   * Only for the log-chatty waits. It must NOT be used for the in-game window: nothing is expected to
   * print between `[mcw] START` and `[mcw] END`, so silence there is the normal case and a stall check
   * would abort every healthy run.
   */
  /*
   * READ THE COUNTERS BEFORE GIVING UP -- the single most valuable thing a failing run can still do.
   *
   * `--dumps` only fires dumpWasmJit() at the in-game window boundaries, so a run that hangs BEFORE
   * in-game produced NO counters at all. R269 lost its one chance to read a hanging run that way: the
   * only genuine hang since its fixes yielded nothing, and the round lost its answer.
   *
   * BOUNDED RACE, NEVER AWAITED UNBOUNDED. A parked emscripten pthread never returns from
   * Runtime.evaluate, so the timeout arm is not belt-and-braces -- it is the arm that actually fires. A
   * silence-only version would have missed the one case it was written for.
   */
  const dumpCounters = async (why) => {
    if (!conn || !sid) return false;
    try {
      const got = await Promise.race([
        conn.send("Runtime.evaluate", { expression: "globalThis.dumpWasmJit && globalThis.dumpWasmJit()" }, sid)
          .then(() => true),
        new Promise((r) => setTimeout(() => r(false), 8000)),
      ]);
      log(got ? `     (JIT counter dump requested before giving up on ${why}; see [wasm-jit ...] above)`
              : `     (dumpWasmJit() did not return within 8s -- the main thread is parked, which is itself a reading)`);
      return got;
    } catch { return false; }
  };

  /*
   * IS IT SPINNING OR BLOCKED? Asked at every give-up point, because the answer chooses the next
   * instrument and the log cannot distinguish them: both look like "no progress, faults=[]".
   *
   * Costs one /proc read now, a 3 s wait, and one more. That is affordable at a give-up point and it is
   * the difference between R275's 27-minute mystery and a named mechanism -- that round had to be
   * diagnosed by hand, with perf, after the fact.
   */
  let spin = null;
  /*
   * IN-GAME THREAD WATCHDOG (R340). A busy thread that STOPS while the rest of the process keeps going is
   * invisible to everything else here: p67 t2's server thread wedged 76 s into its window on a lost unpark
   * (LinkedBlockingQueue.offer -> ReentrantLock.lock -> park, the consumer idle in take), the client kept
   * rendering, no fault class fired, `verdict` said ok, and the per-tick census read the dead server as a 38%
   * improvement. worldwait runs have no Save and Quit and no census, so there it could never be seen at all.
   *
   * Every 5 s, schedstat run time per thread (exact, no capture). A thread is BUSY once its median over the
   * last 12 samples is >= 40% of a core; it has STALLED when its last two samples are both < 5% while some
   * other busy thread is still >= 30% (the process is alive -- a whole-process wedge is waitFor's job). On a
   * stall: log it with wchan, record a `thread-stall` FAULT (the run counts against the build), and take a
   * 6 s `perf record -g` of that tid -- a blocked thread still wakes (emscripten's cancellable waits re-check
   * every ~100 ms) and V8 keeps frame pointers, so those wake samples carry the full Java stack of the wait.
   * Stopped before Save and Quit, where the server goes idle legitimately.
   */
  let threadStall = null;
  const startThreadWatch = () => {
    const pid = findRendererPid(profile);
    if (!pid) return { stop () { } };
    const hist = new Map();
    let prev = threadSnapshot(pid), prevAt = Date.now(), fired = false;
    const med = (a) => { const b = [...a].sort((x, y) => x - y); return b.length ? b[b.length >> 1] : 0; };
    const timer = setInterval(() => {
      try {
        const now = Date.now(), snap = threadSnapshot(pid), wall = now - prevAt;
        if (!snap.size || wall <= 0) return;
        for (const [tid, a] of snap) {
          /* App threads only (R346). The rule is for a GAME thread that stops while another keeps running; a
           * chromium pool thread (ThreadPoolForeground doing V8 background compiles) is busy through a compile
           * burst and then idles by design, and fired it twice in p70 (m1, s2). The app's pthreads are all
           * DedicatedWorkers -- comm is truncated to 15 chars, which is exactly that name. */
          if (!/^DedicatedWorker/.test(a.comm)) continue;
          const b = prev.get(tid); if (!b) continue;
          const h = hist.get(tid) || []; h.push((a.run - b.run) / 1e6 / wall); if (h.length > 24) h.shift(); hist.set(tid, h);
        }
        prev = snap; prevAt = now;
        /* R377: after a stall fired, keep watching THAT thread. A wedge (the R-case this detector exists for: a server
         * thread parked forever) only ever wakes for its ~100 ms emscripten wait re-checks, ~0.2 ms of CPU per 5 s;
         * a pool worker between bursts runs real work again (M3c's class-loading worker: ~90 ms in one interval at
         * 180 s and again at 270 s, perf-confirmed). >= 1% of a core in any later 5 s interval = recovered. */
        if (fired && threadStall && threadStall.key !== undefined) {
          const h = hist.get(threadStall.key);
          const f = h && h.length ? h[h.length - 1] : 0;
          if (f >= 0.01) threadStall.resumedMax = Math.max(threadStall.resumedMax || 0, f);
        }
        if (fired) return;
        const hit = stallFromHistory(hist);
        for (const [tid, h, alive] of hit ? [hit] : []) {
          const last2 = h.slice(-2);
          fired = true;
          const a = snap.get(tid) || {};
          const t = monotonic();
          const at = marks.get("navigate") != null ? `${(t - marks.get("navigate")).toFixed(1)}s` : "?";
          const msg = `THREAD STALL: tid ${tid} (${a.comm}) was busy (median ${(med(h.slice(-12, -2)) * 100).toFixed(0)}% CPU) and `
            + `has been at ${last2.map((f) => (f * 100).toFixed(1) + "%").join(", ")} for 10 s, wchan=${a.wchan || "-"}, `
            + `while tid ${alive[0]} still runs at ${(alive[1][alive[1].length - 1] * 100).toFixed(0)}%`;
          log(`  ${at.padStart(8)}  !! ${msg}`);
          threadStall = { tid: Number(tid), key: tid, comm: a.comm, wchan: a.wchan || null, at: t, capture: null };
          faults.push({ cls: "thread-stall", key: a.comm || "?", sev: "fault", kind: "thread-stall", at: t,
                        line: msg, lines: 1, stack: [] });
          const out = logPath.replace(/\.log$/, "") + `-stall-${tid}.data`;
          if (existsSync(`/tmp/perf-${pid}.map`)) {
            try {
              const pr = spawn("perf", ["record", "-q", "-F", "499", "-g", "-t", String(tid), "-o", out, "--", "sleep", "6"],
                               { stdio: "ignore" });
              pr.on("exit", (code) => {
                if (code === 0) { threadStall.capture = out; log(`     stall stacks captured -> ${out}  (wj perfraw ${out} --callchains)`); }
                else log(`     (stall capture exited ${code}, non-fatal)`);
              });
            } catch (e) { log(`     (stall capture failed, non-fatal: ${e.message.slice(0, 120)})`); }
          } else log(`     no symbol map at /tmp/perf-${pid}.map -- no capture (it would be unreadable)`);
          break;
        }
      } catch (e) { /* a watchdog must never fail a run */ }
    }, 5000);
    return { stop () {
      clearInterval(timer);
      /* A stall whose thread ran again (see resumedMax above) is a worker between bursts, not a wedge: keep the
       * record, drop the fault, say so. A thread that never resumed keeps its fault. */
      if (threadStall && threadStall.resumedMax && !threadStall.recovered) {
        threadStall.recovered = threadStall.resumedMax;
        const i = faults.findIndex((f) => f.cls === "thread-stall" && f.at === threadStall.at);
        if (i >= 0) faults.splice(i, 1);
        log(`  [thread-stall] RECOVERED: tid ${threadStall.tid} ran again after the stall (peak ${(threadStall.resumedMax * 100).toFixed(1)}%`
          + ` of a core in a 5 s interval) -- a worker between bursts, not a wedge; fault withdrawn`);
      }
    } };
  };
  let threadWatch = null;
  const probeSpin = async (why) => {
    try {
      const pid = findRendererPid(profile);
      if (!pid) { spin = { gone: true, spinning: false, busy: [] }; 
        log(`     the renderer process is GONE -- this is not a wedge; the browser died or was killed.`);
        return spin; }
      const before = threadSnapshot(pid);
      const t0 = Date.now();
      await sleep(3000);
      spin = spinCheck(pid, before, Date.now() - t0);
      if (!spin) return null;
      spin.pid = pid;   /* the renderer's MAIN thread is the tid equal to it */
      if (spin.gone) { log(`     the renderer vanished while probing -- browser death, not a runtime wedge.`); return spin; }
      if (spin.spinning) {
        log(`     SPIN, not a block (${why}): ${spin.busy.length} thread(s) running with wchan=0, ${spin.cores} core(s) busy`);
        for (const b of spin.busy.slice(0, 4)) log(`       tid ${b.tid} ${b.comm} at ${(b.frac * 100).toFixed(0)}% CPU`);
        log(`     A spin is NOT waiting for anything -- a longer budget cannot help, and the next`);
        log(`     instrument is a symbolised profile of those tids, not a log.`);
        /*
         * CAPTURE IT NOW, WHILE IT IS HAPPENING. This is the step R275 could not take: it caught the
         * hang live, profiled it by hand, and got five bare addresses in `[anon:v8]` because the run had
         * no symbol map -- so the round ended knowing there was a ~22-byte loop and not which one.
         *
         * Two preconditions, both checked rather than assumed: perf must be present, and V8 must have
         * written /tmp/perf-<pid>.map (worldwait now passes --perf-basic-prof by default for exactly
         * this). Without the map the capture is worth little, so say so instead of producing one that
         * cannot be read. Non-fatal throughout -- a failed capture must never turn a diagnosable hang
         * into a harness error.
         */
        try {
          const map = `/tmp/perf-${pid}.map`;
          const out = logPath.replace(/\.log$/, "") + "-spin.data";
          if (!existsSync(map)) {
            log(`     NO SYMBOL MAP at ${map} -- a capture would resolve to bare addresses in [anon:v8],`);
            log(`     which is exactly what left R275 unable to name the loop. Re-run without MC_NO_PERF_MAP.`);
          } else {
            log(`     capturing 12s of the spin -> ${out}  (symbols: ${map})`);
            execSync(`perf record -F 999 --call-graph=dwarf -p ${pid} -o ${out} -- sleep 12`,
                     { stdio: "ignore", timeout: 40000 });
            spin.capture = out;
            log(`     captured. Read it with: wj perfraw ${out} --inclusive`);
          }
        } catch (e) { log(`     (spin capture failed, non-fatal: ${e.message.slice(0, 120)})`); }
      } else {
        const w = Object.entries(spin.wchan || {}).sort((a, b) => b[1] - a[1]).slice(0, 3);
        log(`     BLOCKED, not spinning (${why}): ${spin.cores} core(s) busy; waiting in ${w.map(([k, n]) => `${k}x${n}`).join(" ") || "(nothing named)"}`);
      }
    } catch (e) { log(`     (spin probe failed: ${e.message})`); }
    return spin;
  };

  const waitFor = async (name, seconds, what, { stallAfter = null, abortIf = null } = {}) => {
    const t0 = Date.now();
    while (!marks.has(name) && Date.now() - t0 < seconds * 1000) {
      pump();
      /* Checked BEFORE the stall test: a dead world still logs (the shutdown save is chatty), so the
       * silence test would never fire and the run would time out on the full budget instead. */
      if (abortIf) {
        const why = abortIf();
        if (why) {
          log(`  !! ABORTING the wait for ${name} (${what}): ${why}`);
          log(`     A longer budget cannot help -- the thing being waited for can no longer happen.`);
          return false;
        }
      }
      /*
       * A TERMINAL FAULT ENDS THE RUN NOW.
       *
       * Faults used to be "RECORDED, never acted on", and for a wasm trap that is still right -- a worker
       * can die and the run go on to render a complete window, so aborting there throws away a good
       * profile. But `MONO_WASM: forcing abort` is not that: the process is GONE, and the driver then sat
       * out the full 420/480 s budget waiting for a mark that can never arrive. Worse, this harness's
       * known gap was that one forced abort took the REST OF THE BATCH with it (observed at 886 s and
       * 1,267 s) -- the browser is dead, the driver keeps waiting, and every remaining run is lost.
       *
       * Only classes verdict.mjs marks TERMINAL reach here: mono-abort, js-oom, em-abort,
       * renderer-crash, wasm-link. Everything else still just accumulates.
       */
      if (terminalFault) {
        log(`  !! TERMINAL ${terminalFault.cls} [${terminalFault.key}] -- the run is over, not slow.`);
        log(`     ${terminalFault.line.slice(-170)}`);
        if (terminalFault.stack.length) log(`     ${terminalFault.stack.slice(0, 4).join("\n     ")}`);
        log(`     Giving up on ${name} (${what}) immediately; the remaining budget cannot help.`);
        await dumpCounters("a terminal fault");
        return false;
      }
      /* TWO CLOCKS, EITHER FIRES. `lastLineAt` catches a browser that has gone completely silent;
       * `lastAppAt` catches the app wedging while chromium keeps writing gcm/gpu noise -- which the
       * single clock structurally could not see, and which is what "filter to lines the APP emits" in
       * CLAUDE.md is about. */
      const quietAll = monotonic() - lastLineAt, quietApp = monotonic() - lastAppAt;
      if ((stallAfter && quietAll > stallAfter) || quietApp > appStallSeconds) {
        log(quietApp > appStallSeconds && (!stallAfter || quietAll <= stallAfter)
          ? `  !! STALLED: no APP output for ${quietApp.toFixed(0)}s while waiting for ${name} (${what}) -- chromium is still writing, the app is not.`
          : `  !! STALLED: no log output at all for ${stallAfter}s while waiting for ${name} (${what}).`);
        /* DO NOT ASSERT "the runtime is wedged" WITHOUT LOOKING. A mod-loader abort produces the same
         * silence -- Fabric catches the entrypoint exception, puts up FabricGuiEntry/FabricStatusTree,
         * and then logs nothing further -- and the runtime is perfectly healthy, having correctly
         * propagated whatever the mod threw. R269 lost an OBJGUARD arm to this: MCSR Ranked's anti-cheat
         * (AntiCheatManager -> ModVerifier -> FileVerifier -> ClientUtils.getApiResponse) timed out with
         * `cli.System.TimeoutException: connect took too long` during preLaunch, and the run was reported
         * as a wedged runtime. IKVM_DEFUSE_RANKED_AUTH=1 defuses account VALIDATION, not this caller.
         *
         * Environment misreported as a runtime fault is the exact failure Phase 0a exists to stop, so
         * name the abort when the evidence is sitting in the log. */
        if (!marks.has("preinit")) {
          /* THE APP NEVER STARTED. Distinct from a wedge and from a mod-loader abort: IkvmWasm.PreInit
           * never entered, so the runtime never ran and cannot have wedged. Observed in R269's exit gate
           * -- an 8-line log holding nothing but chromium's own GCM/portal warnings, with the dev server
           * up and serving 200 throughout. Page-load or browser-side failure, not a JIT fault, and it
           * must not be counted against a stability gate. */
          log(`     NOT a runtime wedge: the app produced NO output at all (PreInit never entered), so the`);
          log(`     runtime never started. Page-load/browser failure -- discard this run, do not count it.`);
        } else if (bootAbortErr && !netTimeoutSeen) {
          /* A LINKAGE ERROR, NOT AN ENVIRONMENT FAILURE. Fabric aborts the same way either way, so the
           * abort itself says nothing about whose fault it is -- but a linkage error means the bytes we
           * generated do not link, which is ours. COUNT IT. */
          log(`     RUNTIME/CODEGEN FAULT, not environment: Fabric aborted on a LINKAGE error.`);
          log(`     ${bootAbortErr.slice(-170)}`);
          log(`     COUNT THIS RUN AS A FAULT. A linkage error is what a runtime or IKVM codegen`);
          log(`     regression produces; do NOT discard it as a mod-loader abort.`);
          faults.push({ kind: "linkage", at: monotonic(), line: bootAbortErr.slice(-220), lines: 1,
                        sig: (bootAbortErr.match(/java\.lang\.[A-Za-z]+/) || [null])[0] });
        } else if (bootAbortSeen || netTimeoutSeen) {
          log(`     NOT a runtime wedge: the log carries a mod-loader/entrypoint abort${netTimeoutSeen ? " caused by a NETWORK TIMEOUT" : ""}.`);
          log(`     ${(netTimeoutSeen || bootAbortSeen).slice(-170)}`);
          log(`     Treat this run as an ENVIRONMENT failure and discard it; do not count it as a fault.`);
        } else if (/src\/net\.ts|"tcpws |"tx \[object/.test(lastAppLine)) {
          /* Set netTimeoutSeen so the recorded verdict (envAbort) and this narrative come from ONE decision. */
          netTimeoutSeen = "net bridge sent a request and never logged a reply: " + lastAppLine.slice(-140);
          netFromLastLine = true;
          log(`     NOT a runtime wedge: the last app output is the NET BRIDGE sending a request with no reply`);
          log(`     (${lastAppLine.slice(-120)}). A mod's network call is blocked with no timeout -- ENVIRONMENT, discard.`);
        } else {
          log(`     No mod-loader abort in the log, so this looks like a genuine runtime wedge — a longer budget will not help.`);
        }
        if (faults.length) log(`     Last fault seen: ${faults[faults.length - 1].line.slice(-180)}`);
        /* DUMP THE JIT COUNTERS BEFORE GIVING UP. `--dumps` only fires dumpWasmJit() at the in-game
         * window boundaries, so a run that hangs BEFORE in-game -- which is exactly when the counters
         * would say why -- produced nothing at all. R269 lost its one chance to read walk_gc_pending on
         * a hanging run that way.
         *
         * Raced against a short timeout and wrapped: emscripten pthreads park in blocking wasm and
         * Runtime.evaluate never returns for those targets, so this must never be awaited unbounded.
         * Best-effort by design -- a hung main thread simply yields nothing and we carry on. */
        await probeSpin("stall");
        /* The net-bridge reading above is only "what the app said last". If the renderer's MAIN thread is the one
         * spinning, the app is not waiting on the network: R479's boot hang (j2d/w5/z2) printed exactly that
         * narrative while main spun in futex_wait_main_browser_thread -- emscripten's proxying self-deadlock
         * (do_proxy holding the system queue's mutex across a contended malloc). Withdraw it, so the run records
         * as a spin and counts. */
        if (netFromLastLine && spin?.spinning && spin.busy.some((b) => b.tid === spin.pid)) {
          netTimeoutSeen = null;
          log(`     REVISED: the renderer's MAIN thread (tid ${spin.pid}) is spinning -- that is not a network wait, and`);
          log(`     the net-bridge line above is only the last thing the app printed. Counted as a SPIN, not env.`);
        }
        await dumpCounters("the stall");
        stalled = true;
        return false;
      }
      await sleep(200);
    }
    pump();
    if (marks.has(name)) return true;
    log(`  !! timed out after ${((Date.now() - t0) / 1000).toFixed(0)}s waiting for ${name} (${what})`);
    if (faults.length) log(`     ${faults.length} fault line(s) were seen; the game may have died. First: ${faults[0].line.slice(-160)}`);
    /* Same reasoning as the stall path above, and this is the arm that actually fired in R269: a
     * world-load TIMEOUT (log still advancing, so the silence test never tripped) with
     * `admit[world.generate]: (no delta)` -- the tier had stopped registering entirely, and there were
     * no counters to say why because the dump only runs at the in-game window. Bounded race: a parked
     * pthread target never returns from Runtime.evaluate. */
    await probeSpin("timeout");
    await dumpCounters("the timeout");
    return false;
  };

  let conn = null, sid = null, ok = false, glRenderer = null, overlayTimer = null;
  try {
    /* --- boot ---------------------------------------------------------------------------------- */
    /* Memory sampling starts HERE, before boot: it needs only the renderer pid, not CDP, and the boot phase
     * is where most of the renderer's overhead over wasm memory is already allocated (vmdata-ab 2026-09-29). */
    const memTick = () => {
      try {
        if (memPid == null || !existsSync(`/proc/${memPid}`)) memPid = findRendererPid(profile);
        if (memPid == null) return;
        const smp = sampleProcMem(memPid);
        if (!smp) return;
        smp.mark = [...marks.keys()].at(-1) ?? null;
        smp.tRel = marks.has("navigate") ? +(monotonic() - marks.get("navigate")).toFixed(1) : null;
        memSamples.push(smp);
        /* Warn ONCE per threshold crossing rather than every tick: the point is to notice a run walking
         * into a ceiling while it still has time to be believed, not to fill the log. */
        const pctR = smp.rlimitData && Number.isFinite(smp.rlimitData) ? 100 * smp.vmData / smp.rlimitData : 0;
        if (pctR >= 80 && !memSamples.warnedRlimit) {
          memSamples.warnedRlimit = true;
          log(`  !! renderer VmData ${(smp.vmData / 1073741824).toFixed(2)} GiB = ${pctR.toFixed(0)}% of RLIMIT_DATA` +
              ` (${(smp.rlimitData / 1073741824).toFixed(0)} GiB) -- a chromium ceiling, not the wasm one`);
        }
      } catch (e) {
        /* Was a bare `catch {}`, which hid a TypeError in the partition walk that cost every sample in a
         * run. A sample is still not worth failing a run over -- but it IS worth saying once. */
        if (!memSamples.warnedSample) { memSamples.warnedSample = true; log(`  !! mem sampler: ${e && e.message}`); }
      }
    };
    memTimer = setInterval(memTick, Number(process.env.MC_MEM_EVERY_MS) || 12000);
    /* THE LAUNCHER. Since 2026-10-21 nothing boots until Play is pressed, so CDP is needed from the start; `play`
     * is the moment a player's wait begins (boot.toframe / boot.reload are measured from it). A "Downloading
     * assets" overlay means the seed lacks a file this app version checks, and the boot would measure the
     * network: say so, loudly, once. */
    conn = await cdpConnect();
    sid = await attachPage(conn);
    await conn.send("Runtime.enable", {}, sid);
    const launch = await driveLauncher(conn, sid, S, { log });
    marks.set("play", launch.mode === "launcher" ? monotonic() : marks.get("navigate"));
    log(`  ${(marks.get("play") - marks.get("navigate")).toFixed(1).padStart(7)}s  play         ${launch.mode === "launcher" ? "Play pressed" : "(no launcher: app started on load)"}`);
    hooks.onMarker?.("play", marks.get("play"));
    let downloadWarned = false;
    overlayTimer = setInterval(async () => {
      if (marks.has("javaMain")) { clearInterval(overlayTimer); return; }
      const ov = await launcherState(conn, sid);
      if (!downloadWarned && /Downloading/i.test(ov || "")) {
        downloadWarned = true;
        log(`  !! the app is DOWNLOADING assets after Play: seed ${S.name} is missing something this app checks -- this boot measures the network`);
        faults.push({ cls: "seed-incomplete", key: S.name, sev: "env", kind: "seed-incomplete", at: monotonic(), line: `overlay: ${ov}`, lines: 1, stack: [] });
      }
    }, 2000);
    log(`\n[mc] boot: waiting for the modded main screen (<= ${bootTimeout}s)`);
    if (!await waitFor("titleReady", bootTimeout, "modded main screen", { stallAfter: stallSeconds })) return finish();
    hooks.onPhase?.({ name: "boot", t0: marks.get("navigate"), t1: marks.get("titleReady") });

    clearInterval(overlayTimer);

    /* Start the OPFS blocked-time poller before anything else uses CDP: it is the one instrument here that
     * measures a cost no CPU profile can see (a thread waiting on the proxy is off-CPU and accrues no
     * task-clock), so losing it to a later CDP failure would be losing the whole measurement. */
    try {
      const r = await conn.send("Runtime.evaluate", { expression: OPFS_POLL_JS(opfsPollMs), returnByValue: true }, sid);
      const v = r?.result?.value;
      if (v !== "polling") log(`  !! OPFS blocked-time poller: ${v ?? "injection failed"}`);
    } catch (e) { log(`  !! OPFS blocked-time poller threw: ${e && e.message}`); }
    try {
      const r = await conn.send("Runtime.evaluate", { expression: ADMIT_POLL_JS(opfsPollMs), returnByValue: true }, sid);
      const v = r?.result?.value;
      if (v !== "polling") log(`  !! JIT admission poller: ${v ?? "injection failed"}`);
    } catch (e) { log(`  !! JIT admission poller threw: ${e && e.message}`); }

    /* NATIVE memory sampler. Deliberately NOT a CDP poller: see the header on summariseMem for why the
     * renderer cannot be asked this question at the moment it matters. 12 s is enough resolution for a
     * ceiling that takes minutes to approach, and each sample is three small /proc reads with no
     * page-table walk, so it costs nothing measurable against the workload. */
    if (!memTimer) memTimer = setInterval(memTick, Number(process.env.MC_MEM_EVERY_MS) || 12000);

    /* Memory-infra, if asked for. Started here so the trace spans everything after page attach, and
     * dumps are REQUESTED on a timer because the periodic trigger does not fire under startup tracing
     * (R233 addendum 2). Failure is non-fatal: a run without a trace is still a run. */
    if (memTracePath) {
      memTraceOn = await startMemTrace(conn);
      if (!memTraceOn) log("  !! memtrace: Tracing.start refused");
      else {
        log(`  memtrace: recording, detailed dump every ${(memDumpEveryMs / 1000).toFixed(0)}s`);
        memDumpTimer = setInterval(() => {
          requestMemDump(conn).then((ok) => { if (ok) memDumps++; }).catch(() => {});
        }, memDumpEveryMs);
      }
    }

    /* RECORD which GL backend the run actually got, do not assume it from the flags.
     *
     * Same discipline as deploy.sh's marker check, and for the same reason: a flag being passed proves
     * nothing about what the browser did with it. Chromium silently falls back to SwiftShader whenever the
     * real driver is unavailable, and that fallback changes the in-game frame rate by more than an order of
     * magnitude (3.67 fps under SwiftShader here). A run whose fps is quoted without this line is a number
     * nobody can interpret later. */
    try {
      const g = await conn.send("Runtime.evaluate", {
        expression: `(()=>{try{const c=document.createElement('canvas'),x=c.getContext('webgl2');if(!x)return 'NO WEBGL2';`
          + `const d=x.getExtension('WEBGL_debug_renderer_info');return x.getParameter(d?d.UNMASKED_RENDERER_WEBGL:x.RENDERER);}catch(e){return 'probe failed: '+e}})()`,
        returnByValue: true,
      }, sid);
      /* ONE `.result`, not two. lib/browser.mjs's send() already unwraps the CDP envelope and resolves the
       * result PAYLOAD, so `g.result.value` is the string; `g.result.result.value` is what a raw WebSocket
       * client needs and silently yields undefined here. It did exactly that for a whole capture, which
       * recorded `glRenderer: null` and left no evidence in the artifact of which rasteriser produced its
       * frame rate -- the one thing this probe exists to prove. */
      glRenderer = g.result?.value ?? null;
      log(`  GL: ${glRenderer}`);
      if (/SwiftShader|softpipe|llvmpipe/i.test(glRenderer || ""))
        log(`     !! SOFTWARE RASTERISER — in-game fps from this run measures the rasteriser, not the runtime.`);
    } catch { }

    /* The same give-up waitFor performs on a TERMINAL fault, for the loops that are not waitFor (the menu
     * re-drive and the settle wait). Without it a terminal abort in those phases was waited out. */
    const giveUpTerminal = async (name, what) => {
      log(`  !! TERMINAL ${terminalFault.cls} [${terminalFault.key}] -- the run is over, not slow.`);
      log(`     Giving up on ${name} (${what}) immediately; the remaining budget cannot help.`);
      await dumpCounters("a terminal fault");
      return finish();
    };

    /* --- menus -> world ------------------------------------------------------------------------ */
    // Settle first. menu.sh returns as soon as FAILED_AUTH is logged, but the auth dialog is DRAWN a
    // beat later; pressing into that gap makes every later press land one step out of phase and the run
    // then sits on the main screen for its whole world budget looking like a hang in the app.
    await sleepPumping(5000 * navScale, pump);
    log(`[mc] world: driving the menu`);
    for (let attempt = 1; attempt <= 3; attempt++) {
      for (const [step, [code, key, waitMs]] of menuSteps.entries()) {
        await sendKey(conn, sid, code, key);
        await sleepPumping(waitMs * navScale, pump);
        /* MC_MENU_SHOTS=1 (with MC_SHOTS_DIR): a PNG after every menu step, to see where a press landed. */
        if (shotsDir && process.env.MC_MENU_SHOTS === "1") {
          mkdirSync(shotsDir, { recursive: true });
          const r = await conn.send("Page.enable", {}, sid).then(() => conn.send("Page.captureScreenshot", { format: "png" }, sid)).catch(() => null);
          if (r && r.data) writeFileSync(join(shotsDir, `menu-${attempt}-${String(step).padStart(2, "0")}-${key}.png`), Buffer.from(r.data, "base64"));
        }
      }
      // "Did Play activate?" — asked of the SERVER's datapack lines, which cannot appear on the menu.
      const t0 = Date.now();
      while (!marks.has("datapack") && !terminalFault && Date.now() - t0 < 60000) { pump(); await sleep(500); }
      if (terminalFault) return await giveUpTerminal("datapack", "menu navigation");
      if (marks.has("datapack")) { log(`  play activated (attempt ${attempt})`); break; }
      if (attempt === 3) { log("  !! menu navigation failed 3x"); return finish(); }
      log(`  still on the menu after attempt ${attempt} — re-driving`);
    }
    if (!await waitFor("inworld", worldTimeout, "world load",
                       { stallAfter: stallSeconds,
                         abortIf: () => worldDied && `the integrated server stopped before the client joined -- ${worldDied}` }))
      return finish();
    hooks.onPhase?.({ name: "world", t0: marks.get("datapack"), t1: marks.get("inworld") });

    /* --- in game ------------------------------------------------------------------------------- */
    // Let the join settle before opening the window: the first seconds in world are initial chunk
    // upload and meshing, which is world-load work continuing, not steady state.
    /*
     * WAIT FOR THE SERVER TO CATCH UP, do not just sleep.
     *
     * This used to be `sleep(15s)` on the theory that the first seconds in world are initial chunk upload.
     * That is true and insufficient: the integrated server on this box comes out of world generation
     * hundreds of ticks in arrears and keeps logging `Can't keep up! Running <N>ms or <T> ticks behind`
     * well into the window. Both captures compared for the IKVM change opened their window while the server
     * was 387 and 472 ticks behind respectively -- so neither measured steady state, and the fps difference
     * between them (5.67 vs 4.39) tracked how far behind the server happened to be, which in turn tracked a
     * world-generation time that itself varies 1.9x. The phase was measuring run state, not the build.
     *
     * So: wait until the server has stopped complaining for `settleQuietSecs`, capped at `settleMaxSecs`,
     * and RECORD the outcome either way. A window that opened un-settled is still usable for per-subsystem
     * shares -- those are ratios inside the window and do not care what the game was busy with -- but its
     * frame rate is not comparable to another run's, and the payload has to say so rather than leave that
     * to be rediscovered.
     */
    let lastBehindAt = monotonic(), behindTicks = null;
    let nBefore = tickLagCount;
    log(`[mc] ingame: waiting for the server to settle (quiet ${settleQuietSecs}s, cap ${settleMaxSecs}s), then ${warmMs ? `a ${(warmMs / 1000).toFixed(0)}s warm-up hold + ` : ""}a ${(benchMs / 1000).toFixed(0)}s window (walk=${walk})`);
    const settleStart = monotonic();
    threadWatch = startThreadWatch();
    let settled = false;
    while (monotonic() - settleStart < settleMaxSecs) {
      await sleepPumping(2000, pump);
      /* A mono abort also silences the tick-lag complaints, so without this it reads as SETTLED, and the
       * window injection below then never returns on the dead page (R291: a0-1 sat out the 900 s cap). */
      if (terminalFault) break;
      if (tickLagCount > nBefore) {
        nBefore = tickLagCount;
        lastBehindAt = monotonic();
        const m = (tickLagLast || "").match(/Running (\d+)ms or (\d+) ticks behind/);
        if (m) behindTicks = Number(m[2]);
      }
      /*
       * "THE SERVER STOPPED COMPLAINING" AND "THE SERVER STOPPED" ARE THE SAME SILENCE.
       *
       * This is the trap this loop originally fell into, and it manufactured the most convincing wrong
       * numbers of the whole exercise. In two of three runs the client timed out from its own integrated
       * server DURING this wait ("ikvmcraft lost connection: Timed out" -> "Stopping singleplayer server as
       * player logged out"). The tick-lag complaints naturally stopped, the loop declared the server settled
       * after its 20s of quiet, and the window then measured a client sitting on a disconnect screen -- which
       * renders a clean vsync-capped 60 fps because it is not rendering a world at all. Reported as fps that
       * looked like a 5x win over every previous measurement.
       *
       * So silence only counts when the server is still there.
       */
      if (serverDead) {
        log(`  !! SERVER DIED during settle: ${serverDead}`);
        log(`     The client dropped from its own integrated server. A window opened now would measure a`);
        log(`     disconnect screen at the vsync cap, not the game. Marking the window VOID.`);
        break;
      }
      if (monotonic() - lastBehindAt >= settleQuietSecs) { settled = true; break; }
    }
    const settleSecs = monotonic() - settleStart;
    if (settled) log(`  server settled after ${settleSecs.toFixed(0)}s (no tick-lag complaint for ${settleQuietSecs}s)`);
    else log(`  !! server NEVER settled in ${settleSecs.toFixed(0)}s — last lag: ${behindTicks ?? "?"} ticks behind.`
      + `\n     Per-subsystem SHARES from this window remain valid; its FRAME RATE is NOT comparable to another run's.`);

    /* Fire and FORGET. The window resolves after benchMs, and awaiting a 60s+ CDP promise across the point
     * where this app's CDP is least reliable is exactly the dependency mcsr/drive.mjs warns about. The
     * window prints `[mcw] START` and `[mcw] END` to the log, so completion is detected on the channel that
     * survives a wedged renderer. */
    if (terminalFault) return await giveUpTerminal("benchStart", "in-game settle");
    const ev = await Promise.race([
      conn.send("Runtime.evaluate", {
        expression: WINDOW_JS(benchMs, walk, dumps, { settled, settleSecs: Number(settleSecs.toFixed(1)), behindTicks, serverDead }, warmMs),
        awaitPromise: false,
      }, sid),
      new Promise((res) => setTimeout(() => res({ timedOut: true }), 30000)),
    ]);
    if (ev.timedOut) { log("  !! window injection did not return in 30s -- the page is not responding"); return finish(); }
    if (ev.exceptionDetails) log(`  !! window injection threw: ${ev.exceptionDetails.exception?.description?.slice(0, 200)}`);
    /* +warmMs: the START marker is emitted AFTER the warm-up hold, so this wait has to cover the hold or it
     * times out before the window it is waiting for can possibly open. Cost three runs when warmMs landed. */
    if (!await waitFor("benchStart", 30 + Math.ceil(warmMs / 1000), "bench window open")) return finish();
    if (!await waitFor("benchEnd", Math.ceil((benchMs + warmMs) / 1000) + 180, "bench window close")) return finish();
    hooks.onPhase?.({ name: "ingame", t0: marks.get("benchStart"), t1: marks.get("benchEnd") });
    threadWatch?.stop();   // before Save and Quit: a quitting server goes idle legitimately
    // The summary is printed right after the marker; give the tail a moment to see all of it.
    await sleepPumping(3000, pump);
    ok = true;

    /* --- save and quit ------------------------------------------------------------------------- */
    /*
     * `ok` is set BEFORE this block on purpose. The in-game window has already closed and its numbers are
     * already valid, so a quit that fails must not invalidate the run -- it is additional coverage, not a
     * precondition. A failed quit is reported and the `save` phase is simply absent.
     */
    const driveSaveQuit = async () => {
      quitting = true;                 // before the first keystroke; see SHUTDOWN_RX
      marks.set("quitStart", monotonic());
      hooks.onMarker?.("quitStart", marks.get("quitStart"));
      /*
       * PROBE MODE. `quitProbeDir` presses Escape and then Tabs one step at a time, writing a PNG after
       * each, and never presses Enter -- so a human (or a model that can look at images) can read off
       * which Tab index highlights "Save and Quit to Title" for THIS mod set.
       *
       * This exists because the Tab count is the one part of the quit path that cannot be derived from the
       * source: the pause screen's focus order is the order its buttons were added, and Fabric + MCSR
       * Ranked + Sodium add and remove them. autoworld.sh established the world-ENTRY sequence exactly
       * this way and its header records why -- guessing the count lands every later press one step out of
       * phase, and the run then looks like a hang in the app rather than a harness error.
       */
      if (quitProbeDir) {
        mkdirSync(quitProbeDir, { recursive: true });
        await conn.send("Page.enable", {}, sid);
        const shot = async (name) => {
          const r = await conn.send("Page.captureScreenshot", { format: "png" }, sid);
          writeFileSync(join(quitProbeDir, `${name}.png`), Buffer.from(r.data, "base64"));
          log(`  probe: wrote ${name}.png`);
        };
        await shot("00-inworld");
        await sendKey(conn, sid, "Escape", "Escape");
        await sleepPumping(1500 * navScale, pump);
        await shot("01-escape");
        for (let k = 1; k <= QUIT_TABS_MAX; k++) {
          await sendKey(conn, sid, "Tab", "Tab");
          await sleepPumping(400 * navScale, pump);
          await shot(`${String(k + 1).padStart(2, "0")}-tab${k}`);
        }
        log(`  probe: ${QUIT_TABS_MAX} Tab positions captured in ${quitProbeDir}.`
          + ` Find the frame where "Save and Quit to Title" is highlighted and set MC_QUIT_TABS to its tab number.`);
        return;
      }
      log(`[mc] save+quit: Escape, ${quitTabs}x Tab, Enter  (exercises chunk serialisation and the OPFS write path)`);
      try {
        await sendKey(conn, sid, "Escape", "Escape");
        await sleepPumping(1500 * navScale, pump);
        for (let k = 0; k < quitTabs; k++) {
          await sendKey(conn, sid, "Tab", "Tab");
          await sleepPumping(400 * navScale, pump);
        }
        await sendKey(conn, sid, "Enter", "Enter");
      } catch (e) {
        log(`  !! quit keystrokes threw: ${e && e.message} (CDP is unreliable in-world; the run's own numbers stand)`);
        return;
      }
      /* Two waits, because the two failure modes are different and want different messages: the menu not
       * taking (no `savingWorlds`) is a Tab-count problem, while a save that starts and never finishes is
       * the interesting one -- one OPFS proxy worker, `flush` never previously exercised, and `writes` at
       * ~6 per run for the whole project. Cap it and record the phase as incomplete rather than stalling. */
      if (!await waitFor("savingWorlds", saveMenuTimeout, "Save and Quit to take", { stallAfter: null })) {
        if (threadStall && !threadStall.recovered)
          log(`     Save and Quit did not take -- expected: tid ${threadStall.tid} (${threadStall.comm}) STALLED in-game`
            + ` (see THREAD STALL above), and a wedged server cannot save. Not a Tab-count problem.`);
        else
          log(`     the pause menu did not reach Save and Quit. MC_QUIT_TABS=${quitTabs} is probably wrong for`
            + ` this mod set -- re-derive it by screenshotting after each Tab, the way autoworld.sh did -- OR a`
            + ` thread wedged after the window closed (R340: p67 t2 printed this for a wedged server).`);
        return;
      }
      if (!await waitFor("chunksSaved", saveWriteTimeout, "the world to finish saving")) {
        log(`     the save STARTED and did not finish in ${saveWriteTimeout}s. That is a finding, not a harness`
          + ` fault: every write goes through the single OPFS proxy worker and this path has never run.`);
        return;
      }
      log(`  save+quit complete: ${(marks.get("chunksSaved") - marks.get("quitStart")).toFixed(1)}s`
        + ` (write ${(marks.get("chunksSaved") - marks.get("savingWorlds")).toFixed(1)}s)`);
      hooks.onPhase?.({ name: "save", t0: marks.get("quitStart"), t1: marks.get("chunksSaved") });
      /* One more poll interval so the OPFS_BLOCKED line covering the save is in the log before the kill. */
      await sleepPumping(opfsPollMs + 500, pump);
    };
    if (saveQuit && quitTabs == null && !quitProbeDir)
      log(`[mc] save+quit skipped: seed ${S.name} has no measured pause-menu Tab count (probe with MC_QUIT_PROBE_DIR, then MC_QUIT_TABS)`);
    else if (saveQuit) await driveSaveQuit();
    if (workerProbeCount > 0 && workerProbeCount <= 16 && memPid != null) {
      if (memTimer) { clearInterval(memTimer); memTimer = null; }
      workerProbe = await measureIdlePthreadCost(conn, sid, memPid, workerProbeCount);
      log(`  worker probe: ${workerProbe.error ||
        `${workerProbe.deltaWorkers}/${workerProbeCount} loaded workers, VmData ${workerProbe.vmData?.deltaMb} MiB`}`);
    }
    return finish();
  } finally {
    try { if (memTimer) clearInterval(memTimer); } catch { }
    try { if (memDumpTimer) clearInterval(memDumpTimer); } catch { }
    /* BEFORE conn.close() and before game.kill(): the trace is only delivered through the still-open
     * CDP stream, so closing either first loses it -- which is exactly how startup tracing lost two
     * whole runs to the SIGKILL. One last dump so the file always contains a sample at peak. */
    if (memTraceOn && !memTraceSaved) {
      memTraceSaved = true;
      try {
        await requestMemDump(conn);
        memDumps++;
        await endMemTrace(conn, memTracePath, log);
        log(`  memtrace: ${memDumps} detailed dumps requested`);
      } catch (e) { log(`  !! memtrace: save threw: ${e && e.message}`); }
    }
    threadWatch?.stop();
    try { conn?.close(); } catch { }
    /* AWAITED, and the order matters. onEnd is where a profiling caller stops its recorder; killing the
     * browser first truncates whatever perf has not yet written out, and a truncated perf.data reports a
     * plausible SUBSET of the run rather than an error. */
    try { await hooks.onEnd?.(); } catch (e) { log(`  !! onEnd hook threw: ${e && e.message}`); }
    /*
     * DROP THIS RUN'S V8 SYMBOL MAP, unless something actually needs it.
     *
     * --perf-basic-prof writes a ~240 MB /tmp/perf-<pid>.map per run and NOTHING used to remove it. /tmp
     * is a 7.7 G tmpfs, i.e. RAM, so a batch quietly converts itself into memory pressure and then dies
     * at PAGE LOAD -- which reads as a browser bug, not as housekeeping, and cost two arms on 2026-09-20
     * (R280f: 100 maps, 4.9 GiB, 2.7 G free).
     *
     * This became urgent rather than merely tidy when worldwait started passing --perf-basic-prof by
     * default so a caught spin would be symbolisable: three runs took /tmp from 62 MB to 741 MB, which is
     * a ~20-run batch back into exactly the state R280f describes.
     *
     * KEPT when a spin capture was taken -- the capture is unreadable without it -- and kept by mcperf,
     * which copies it into its own outdir and owns the lifetime itself.
     *
     * Every process under this profile, not only the renderer: the flag reaches each V8 the browser starts,
     * and the two or three small siblings' maps (~0.1-0.3 MB each) were left behind by every run -- 340 of
     * them in /tmp by 2026-10-04. Still before game.kill(), so the pids resolve.
     */
    try {
      if (!spin?.capture && !threadStall && !keepPerfMap) {
        for (const d of readdirSync("/proc")) {
          if (!/^\d+$/.test(d)) continue;
          let cl = "";
          try { cl = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
          if (cl.includes(profile)) rmSync(`/tmp/perf-${d}.map`, { force: true });
        }
      }
    } catch { /* never let housekeeping fail a run */ }
    if (overlayTimer) clearInterval(overlayTimer);
    game.kill();
    process.off("SIGINT", onInterrupt);
    process.off("SIGTERM", onTerminate);
  }

  function finish () {
    pump();
    const phases = [], subphases = [];
    for (const p of PHASES) {
      const t0 = marks.get(p.from), t1 = marks.get(p.to);
      if (t0 != null && t1 != null) phases.push({ ...p, t0, t1, secs: t1 - t0 });
    }
    for (const p of SUBPHASES) {
      const t0 = marks.get(p.from), t1 = marks.get(p.to);
      if (t0 != null && t1 != null) subphases.push({ ...p, t0, t1, secs: t1 - t0 });
    }
    /* The window's END payload, parsed. Kept alongside the raw lines rather than replacing them: if the
     * JSON ever fails to parse, the caller can still see what was printed. */
    let window = null;
    for (const l of benchLines) {
      /* Greedy, and deliberately NOT anchored to end-of-line. Chromium wraps a console message in double
       * quotes and appends `", source: <url>`, and it does not escape quotes inside the message -- so the
       * payload sits mid-line surrounded by JSON's own quotes. Greedy `\{.*\}` still terminates on the
       * outermost brace because nothing after it in the line contains one. */
      const m = l.match(/\[mcw\] END (\{.*\})/);
      if (m) { try { window = JSON.parse(m[1]); } catch { } }
    }
    /* The mode is stamped HERE, beside the phase it describes, so no consumer has to know the thresholds
     * or re-derive the split. Null when `world.generate` is missing (a run that never reached the knee). */
    const worldGenMode = classifyWorldGen(subphases.find((p) => p.name === "world.generate")?.secs);
    /* THE URL IS THE RUN'S PROVENANCE, so hand it back rather than leaving each driver to reconstruct
     * it. R269 could not tell from a worldwait artifact whether MONO_WASM_JIT_OBJGUARD had actually been
     * applied, because worldwait records no URL and the knob only appears in a log line when an uncaught
     * error happens to echo it -- so proving a diagnostic knob was live took a grep of the served wasm
     * plus the harness's env-override echo. CLAUDE.md's rule is that a knob must stay visible in each
     * run's recorded command line; this is what makes that possible for every driver at once. */
    /*
     * ONE CANONICAL VERDICT, computed here rather than re-derived by each driver.
     *
     * Six drivers used to each invent their own notion of "did this run count" -- mcab's usable(),
     * mcperf's ingameVoid + exit code 3, bootcheck's PASS/FAIL/TIMEOUT, arm.sh's INVALID/CRASH, and two
     * copies of the void-window rule -- and mc-results.jsonl already carries at least four incompatible
     * row schemas because of it. Worse, `ok` is NOT a verdict: it is a pure progress flag set the instant
     * benchEnd is seen, unaffected by faults, by `stalled`, or by a void window. A run can be ok:true
     * while trapping.
     *
     * `verdict` separates the three outcomes that must NOT be counted against a build -- env,
     * never-started, killed -- from the four that must. That split is R269's finding (three distinct
     * silent-stall causes in one day, only one of them a JIT fault, all three reported as "the runtime is
     * wedged") promoted from log prose into a field a census can group by.
     *
     * `ok` and `stalled` are kept for compatibility with every existing reader; nothing is removed.
     */
    const verdict = classifyRun({
      marks, faults, reachedEnd: ok, stalled, spin,
      browserGone: !!spin?.gone,
      timedOut: !ok && !stalled,
      /* The same two signals the stall classifier prints its "environment failure" narrative from, so the
       * printed reason and the recorded verdict come from ONE decision. */
      envAbort: netTimeoutSeen || bootAbortSeen || null,
    });
    log(`\n[mc] verdict: ${verdict}`
      + (faults.length ? `  (${Object.values(summariseFaults(faults)).map((f) => `${f.cls}:${f.key}x${f.n}`).join(", ")})` : "  (no classified faults)"));
    return { url, marks, phases, subphases, spawnMs, window, glRenderer, benchSummary: benchLines, faults, ok, stalled,
             workerProbe,
             verdict, faultSummary: summariseFaults(faults), spin, threadStall,
             /* `top: true` marks the three (now four) real phases; see summariseBlocked for why only
              * those may fall back to the first poll. */
             worldGenMode, opfs: summariseOpfs(opfsSamples,
                                               [...phases.map((p) => ({ ...p, top: true })), ...subphases],
                                               opfsBlocked),
             admit: summariseAdmit(admitSamples, phases.concat(subphases)),
             v8gc: summariseV8Gc(lines),
             /* benchLines contains raw console strings; window is the parsed END payload. This is
              * end-of-window wasm memory, for comparison with the sampled VmData peak. */
             mem: summariseMem(memSamples, window?.heapMb != null ? window.heapMb * 1048576 : null),
             log: logPath, lines };
  }
}

async function sleepPumping (ms, pump) {
  const end = Date.now() + ms;
  while (Date.now() < end) { pump(); await sleep(Math.min(200, end - Date.now())); }
  pump();
}

/** CDP endpoint on the fixed port. Fixed rather than 0 because stderr is already redirected to the log
 *  file, so there is no stream left to scrape the port off. */
export async function cdpConnect ({ port = process.env.MC_CDP_PORT || 9333, tries = 60 } = {}) {
  for (let i = 0; i < tries; i++) {
    try {
      const j = JSON.parse(execSync(`curl -s http://127.0.0.1:${port}/json/version`, { encoding: "utf8" }));
      if (j.webSocketDebuggerUrl) return await connect(j.webSocketDebuggerUrl);
    } catch { }
    await sleep(500);
  }
  throw new Error(`no CDP endpoint on ${port}`);
}

/** Read the page-side probes that only exist in this app. Best-effort: never let it fail a run. */
export async function probeCounters (conn, sid) {
  try {
    const r = await conn.send("Runtime.evaluate", {
      expression: `JSON.stringify({
        live: (globalThis.jitLiveness && globalThis.jitLiveness()) || null,
        frames: globalThis.wasm?.Module?._emscripten_glfw3_get_frame_count?.() ?? null,
        heapMb: ((globalThis.wasm?.Module?.wasmMemory?.buffer ?? globalThis.getDotnetRuntime?.(0)?.localHeapViewU8?.().buffer)?.byteLength || 0) / 1048576,
      })`,
      returnByValue: true,
    }, sid);
    return JSON.parse(r.result?.value ?? "null");
  } catch { return null; }
}

/** Table of phase timings, the shape both mcbench and mcperf print. */
export function formatPhases (r) {
  const out = [];
  const row = (n, s, w) => `  ${n.padEnd(16)} ${s.toFixed(1).padStart(8)}s  ${w}`;
  out.push("  phase             seconds  what");
  for (const p of r.phases) {
    out.push(row(p.name, p.secs, p.what));
    for (const s of r.subphases.filter((x) => x.name.startsWith(p.name + ".")))
      out.push(`    ${s.name.slice(p.name.length + 1).padEnd(12)} ${s.secs.toFixed(1).padStart(8)}s  ${s.what}`);
  }
  if (r.spawnMs != null) out.push(`  (server's own figure for spawn-area prep: ${(r.spawnMs / 1000).toFixed(1)}s)`);
  return out.join("\n");
}
