// Non-interactive perf capture of ONE full Minecraft run, sliced into the three phases.
//
// perfrun.mjs is this for jbox2d: one homogeneous window, one profile. Minecraft needs the same trick
// applied three times, and the reason is the same one perfrun.mjs documents at length — an unwindowed
// capture is useless. Here it is worse than useless: boot, world generation and steady-state rendering
// are different programs sharing a process, and a profile that averages 150s of class loading with 60s of
// rendering produces a flat list in which nothing is above 2% and no change ever appears to help.
//
// So: ONE capture for the whole run, plus phases.json giving each phase's [start,end] in CLOCK_MONOTONIC,
// which is exactly the clock `perf record -k 1` stamps samples with. mcreport.mjs then slices it with
// `perf report --time`, and pattern.mjs takes the same window via --window.
//
// Usage:
//   wj mcperf <outdir> [--jitdump] [--bench-ms 60000] [--no-walk]
//                                 [--knob name=value ...] [--freq 499] [--force] [--headed]
//   -> writes <outdir>/{perf.data|perf.jitted, phases.json, run.json, game.log}
//   then: wj perfraw <outdir> --phase ingame --by-thread
//
// TWO SYMBOLISATION TIERS, AND WHY THE DEFAULT IS THE CHEAP ONE
// ------------------------------------------------------------
// --perf-basic-prof (default) makes V8 write a flat /tmp/perf-<pid>.map of symbol RANGES. That is enough
// to NAME every frame — managed methods, our JIT'd wasm, the AOT image, the C runtime — which is the whole
// question "where does the time go".
//
// --jitdump switches to --perf-prof, which writes a jitdump the python readers parse directly. Pass
// --inject as well (opt-in, slow) if you specifically need `perf annotate`; see the NO_INJECT block.
// --jitdump switches to --perf-prof, which writes a jitdump that `perf inject --jit` turns into one ELF
// object per function, unlocking `perf annotate` (and therefore pattern.mjs / codesize.mjs). It costs a
// lot: on the ~6s jbox2d bench that explosion was ~300k files and 2.4 GB, and a Minecraft run compiles
// vastly more code than jbox2d does. Use it for a targeted second pass on whichever phase tier 1 says
// matters, not for the survey.
//
// EVENT CHOICE: task-clock, NOT cycles
// ------------------------------------
// This box is a hybrid i7-1360P with separate cpu_core and cpu_atom PMUs, and `-e cycles:u` resolves to
// cpu_core/cycles/u ALONE — every sample from a thread the scheduler put on an E-core is silently missing.
// Minecraft here runs 16+ emscripten pthreads and the scheduler spreads them across both core types, so
// that is not a rounding error, it is a hole in the profile that is invisible in the output. task-clock is
// a software event: it covers every core, needs no PMU, and works at perf_event_paranoid=2 without sudo
// (perf appends :u automatically, which is what we want anyway — kernel frames here are futex/ioctl noise).
// MCPERF_EVENT overrides it when cycle-accurate work is the point.
import { runSession, buildUrl, seedProfile, formatPhases, cdpConnect, monotonic, sleep, seedFromArgs } from "../lib/mcdrive.mjs";
import { preflight, sampleEnv } from "../lib/preflight.mjs";
import { describeDeployment, printDeployment } from "../lib/provenance.mjs";
import { spawn, spawnSync, execSync } from "child_process";
import { mkdirSync, rmSync, existsSync, readdirSync, writeFileSync, readFileSync, statSync } from "fs";
import { join, resolve } from "path";

const args = process.argv.slice(2);
/* --seed NAME|DIR (lib/seeds.mjs), else MC_SEED, else mcsr */
const SEED = seedFromArgs(args);
const has = (n) => args.includes(`--${n}`);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const pos = args.filter((a, i) => !a.startsWith("--") && !(i > 0 && /^--(bench-ms|freq|knob|seed)$/.test(args[i - 1])));
const outdir = resolve(pos[0] || "");
if (!pos[0]) { console.error("usage: mcperf.mjs <outdir> [--jitdump] [--inject] [--bench-ms N] [--no-walk] [--knob k=v] [--freq N]"); process.exit(1); }

const JITDUMP = has("jitdump");
/* INJECT IS OPT-IN (`--inject`). It used to be coupled to --jitdump, and that was backwards in both
 * directions for this repo's tooling.
 *
 * perfraw.py, hotinsn.py and callform-x86.py all read the RAW jitdump (JIT_CODE_LOAD records) directly --
 * perfraw's header even records that inject "produced nothing usable here" -- so the inject buys the
 * analysis nothing. Worse, its success path DELETES the raw dumps, because perf.jitted only references the
 * extracted .so files. So the coupled form spent ~2M files and ~17 GB (measured 2026-08-29: 12 min, 9 GiB,
 * ~1M files before being killed) and then removed the input the analysis actually wants.
 *
 * Measured again 2026-09-01: killed at 8m20s, still running, on a 3.1 GB dump -- while the python readers
 * parse the same dump in seconds. Every shadow/co-location measurement in R166-R170 used the raw readers.
 *
 * Inject remains the right thing for `perf annotate` and inclusive attribution, which is what it was added
 * for -- so it is kept, behind a flag, rather than deleted. `--no-inject` still parses and is a no-op, so
 * older command lines and scripts keep working. */
const NO_INJECT = !has("inject");
const BENCH_MS = num("bench-ms", 60000);
/* WARM-UP HOLD BEFORE THE WINDOW OPENS. Default 120 s, and it used to default to 0.
 *
 * The ramp is ~150-180 s (R97/R100), so a window opened without a hold measures WHERE IN THE RAMP it
 * landed rather than frame cost. Measured 2026-09-07 over 458 usable archived windows: 52.8% were still
 * RISING more than 1.2x from their first third to their last third at the moment they closed, and in 20 of
 * 51 arms the best run picked by mean fps is not the best run picked by fpsTail. Both drivers already
 * carried a comment saying "profile the PLATEAU, not the ~180s ramp" while defaulting to 0, so every A/B
 * that did not pass --warm-ms explicitly was reading ramp position. Comment rule #1, in a new place.
 *
 * 120 s rather than 180 s because R226 measured work-per-frame flat to ~7% after the first quarter of a
 * 120 s window, and each extra second of hold is paid on every arm. Pass --warm-ms 0 deliberately if you
 * want the ramp itself. */
const WARM_MS = num("warm-ms", 120000);
const WALK = !has("no-walk");
const FREQ = String(num("freq", 499));
const EVENT = process.env.MCPERF_EVENT || "task-clock";
const PERIOD = process.env.MCPERF_PERIOD || "";   // fixed sample period (-c N); see the record args below
const HEADLESS = !has("headed");

/* Repeatable --marker <string>: assert a string is present in the SERVED bytes before measuring. Use it
 * whenever the thing under test is an uncommitted change -- especially in IKVM, which deploy.sh does not
 * check at all. e.g. --marker DynamicInstanceOfResolve. */
const markers = [];
for (let i = 0; i < args.length; i++) if (args[i] === "--marker" && args[i + 1]) markers.push(args[++i]);

/* Repeatable --ro <name[=value]>: a mono RUNTIME OPTION rather than a wasm_jit env knob. index.ts's
 * applyRuntimeOptionOverrides turns `?ro.x=v` into `--x=v` and a VALUELESS `?ro.x=` into the bare `--x`,
 * which is how the negative flags are spelled: `--ro no-jiterpreter-traces-enabled` disables the
 * jiterpreter. Needed to test the tiers independently of each other. */
const runtimeOptions = {};
for (let i = 0; i < args.length; i++) {
  if (args[i] !== "--ro") continue;
  const [k, ...rest] = (args[++i] || "").split("=");
  if (k) runtimeOptions[k] = rest.join("=");
}

const knobs = {};
for (let i = 0; i < args.length; i++) {
  if (args[i] !== "--knob") continue;
  const [k, ...rest] = (args[++i] || "").split("=");
  if (k) knobs[k] = rest.join("=");
}

/* perf_event_paranoid: 2 is enough for what this does (per-process, user-space only), so unlike
 * perfrun.mjs there is no sudo requirement. 3+ blocks it outright. */
const par = Number(readFileSync("/proc/sys/kernel/perf_event_paranoid", "utf8").trim());
if (par > 2) { console.error(`kernel.perf_event_paranoid=${par} blocks even per-process user sampling; run: sudo sysctl kernel.perf_event_paranoid=1`); process.exit(1); }

/* The outdir must be on real disk. /tmp here is tmpfs, i.e. RAM: perfrun.mjs's note that 2.4 GB of
 * capture debris on /tmp put this box into reclaim and cost ~9% on the NEXT measurement applies with more
 * force to a Minecraft capture, which is larger. Refuse rather than warn — the damage lands on a later
 * run, where it is untraceable. */
rmSync(outdir, { recursive: true, force: true });
mkdirSync(outdir, { recursive: true });
const mount = execSync(`df --output=target $(readlink -f ${JSON.stringify(outdir)}) | tail -1`, { encoding: "utf8" }).trim();
if (mount === "/tmp" || mount === "/dev/shm") {
  console.error(`outdir is on ${mount}, which is RAM-backed. A capture there competes with the runtime for memory`);
  console.error(`and silently taxes the NEXT measurement. Put it on disk (under $WJ_RUNS).`);
  process.exit(1);
}

console.log(`[mcperf] ${outdir}`);
console.log(`[mcperf] event=${EVENT}@${PERIOD ? `1/${PERIOD} events` : `${FREQ}Hz`}  symbolisation=${JITDUMP ? (NO_INJECT ? "raw jitdump (no inject; perfraw/hotinsn read it directly)" : "jitdump (perf inject --jit; enables annotate)") : "basic-prof (names only)"}`);
/* Say the price BEFORE spending 8 minutes on it. A jitdump capture of this workload measured 2,039,711 files
 * and 17 GB, and the .so files cannot be pruned afterwards (perf.jitted only references them). basic-prof costs
 * ~400 MB for the same run. What jitdump buys is inclusive attribution and annotate; if the question is "how
 * much time is under X", pay it -- basic-prof callchains are ONE FRAME DEEP for JIT'd code, so leaf-only. */
if (JITDUMP && !NO_INJECT) console.log(`[mcperf] !! jitdump+inject: expect ~2M files and ~17 GB in ${outdir}, and it cannot be pruned. Free space first. Pass --no-inject unless you specifically need perf annotate.`);
/* Keep what the preflight found and write it into phases.json below.
 *
 * A refusal is only half the value. On a dev box some foreign load is permanent -- an electron editor at
 * 21% of one core will not go away, and refusing every run is worse than noting it -- so --force is the
 * normal answer, and then the fact has to survive into the artifact. Otherwise the run looks pristine
 * three days later and a 4% difference against another capture has no explanation available. */
const env = preflight({ force: has("force") });

/* WHAT IS ACTUALLY DEPLOYED, stamped into the run. See lib/provenance.mjs: deploy.sh verifies the mono
 * runtime and nothing verifies the managed assemblies, and IKVM's are rebuilt from a repo that routinely
 * carries uncommitted work. A missing marker aborts, because measuring the wrong bytes is worse than not
 * measuring. */
const deployment = describeDeployment({ markers });
const missing = printDeployment(deployment);
if (missing) { console.error(`\nrefusing to measure: ${missing} marker(s) absent from the served bytes. Rebuild and redeploy.`); process.exit(2); }

const profile = join(outdir, "browser-profile");
const logPath = join(outdir, "game.log");
console.log(`[mcperf] seeding profile (reflink copy of the seed)`);
seedProfile(profile, { seed: SEED });

const url = buildUrl({ knobs, runtimeOptions, seed: SEED });
/* --perf-prof-unwinding-info is what makes `-g` call graphs resolve THROUGH generated frames; without it
 * a wasm frame terminates the chain and every caller above it is lost. basic-prof has no equivalent, so
 * tier 1 call graphs are shallower by construction — one more reason it is a survey, not the last word. */
const jsFlags = JITDUMP ? `--perf-prof --perf-prof-unwinding-info --perf-prof-path=${outdir}` : "--perf-basic-prof";

/* ------------------------------------------------------------------------------------------------ *
 * perf, attached to the RENDERER
 * ------------------------------------------------------------------------------------------------ */

/* CDP exposes no target->pid map, so resolve the renderer via SystemInfo.getProcessInfo and wait for a
 * CLEAR winner. Picking the top-CPU chrome process (what a shell one-liner does, and what srvperf.sh
 * does) grabs the GPU or browser process whenever it happens to be busy at that instant — and under
 * SwiftShader the GPU process is genuinely busy, so here that one-liner is not merely unlucky, it is
 * usually wrong. */
async function rendererPid (conn) {
  for (let i = 0; i < 240; i++) {
    const r = await conn.send("SystemInfo.getProcessInfo");
    if (!r.__error) {
      const rs = (r.processInfo || []).filter((p) => p.type === "renderer").sort((a, b) => b.cpuTime - a.cpuTime);
      if (rs.length && rs[0].cpuTime >= 0.25 && (rs.length === 1 || rs[0].cpuTime > 3 * rs[1].cpuTime)) return rs[0].id;
    }
    await sleep(250);
  }
  return null;
}

let rec = null, recErr = "", recStart = null, recPid = null;

/* Fired for every driver marker; acts only on `inworld`. Costs one CDP round trip on a run that is
 * about to sit in a 180 s hold, and turns the silent-mis-attach failure into a restart. */
async function reattachAtInworld (name) {
  /* The data-address census (j2d/addrcensus.py) classifies `perf record -d` addresses against the renderer's
   * mappings at the window's edges: copy /proc/<pid>/maps at benchStart and benchEnd. One small read each. */
  if ((name === "benchStart" || name === "benchEnd") && recPid) {
    try { writeFileSync(join(outdir, `maps-${name}.txt`), readFileSync(`/proc/${recPid}/maps`, "utf8")); }
    catch (e) { console.error(`!! maps snapshot at ${name} failed (${e && e.message})`); }
    windowEdge(name);
    return;
  }
  if (name !== "inworld" || !recPid) return;
  let c = null;
  try {
    c = await cdpConnect();
    const now = await rendererPid(c);
    if (now && now !== recPid) {
      console.log(`[mcperf] renderer moved ${recPid} -> ${now} since attach; restarting perf on the live one`);
      await stopPerf();
      /* stopPerf latches `stopping` and sets recEnd; both must be cleared or the FINAL stop becomes a
       * no-op and the restarted capture is never flushed. startPerf reuses dataPath, so the discarded
       * first capture is overwritten -- which is the intent, it was on the wrong pid. */
      rec = null; recErr = ""; recStart = null; recEnd = null; stopping = false;
      startPerf(now);
    }
  } catch (e) {
    console.error(`!! inworld renderer re-check failed (${e && e.message}); capture may be on the wrong pid`);
  } finally { try { c?.close(); } catch { } }
}
const dataPath = join(outdir, "perf.data");

/* MCPERF_STAT=<events>: COUNTING-mode `perf stat --per-thread` over exactly the in-game window (benchStart ->
 * benchEnd), written to stat.txt. The sampled census undercounts the render thread's instructions 34-45% to
 * throttling (perf_event_max_sample_rate is 3000 on this box); counting mode has no throttle (plan swirling-moore
 * GL0). e.g. MCPERF_STAT=cpu_core/instructions/u,cpu_core/cycles/u
 * MCPERF_THREADS=1: per-thread run / runqueue-wait / sleep over the same window from /proc/<tid>/schedstat (sleep =
 * wall - run - rq, threadwait.mjs's definition) plus a wchan histogram sampled every 200 ms, to threads.json. */
const STAT_EVENTS = process.env.MCPERF_STAT || "";
const STAT_TOP = Number(process.env.MCPERF_STAT_TOP || 0);   // see windowEdge
const STAT_SEP = process.env.MCPERF_STAT_SEP || ",";          // ";" for event names carrying terms (cmask=1,edge)
const THREADS = !!process.env.MCPERF_THREADS;
let statProc = null, thr0 = null, thrT0 = 0, wchanTimer = null;
const wchanHist = new Map();
function taskSnapshot (pid) {
  const out = new Map();
  let tids = [];
  try { tids = readdirSync(`/proc/${pid}/task`); } catch { return out; }
  for (const t of tids) {
    try {
      const [run, rq] = readFileSync(`/proc/${pid}/task/${t}/schedstat`, "utf8").trim().split(/\s+/).map(Number);
      const comm = readFileSync(`/proc/${pid}/task/${t}/comm`, "utf8").trim();
      out.set(t, { run, rq, comm });
    } catch { }
  }
  return out;
}
function windowEdge (name) {
  if (name === "benchStart") {
    if (STAT_EVENTS && !statProc && STAT_TOP > 0) {
      /* MCPERF_STAT_TOP=N: attach to the N threads that ran most over the window's first second (-t), not to the whole
       * process (-p). natrun.sh does the same on the JVM by name: `--per-thread -p` over ~60-90 threads with GP event
       * groups multiplexed into garbage there. Ours has no distinctive name (both busy threads are DedicatedWorker), so
       * nativemc/servertid.py names the server tid afterwards from this run's perf.data. stat_tids.json records them. */
      statProc = "pending";
      const a = taskSnapshot(recPid);
      setTimeout(() => {
        const b = taskSnapshot(recPid);
        const top = [...b].map(([t, s]) => [t, s.run - (a.get(t)?.run ?? s.run), s.comm]).sort((x, y) => y[1] - x[1]).slice(0, STAT_TOP);
        writeFileSync(join(outdir, "stat_tids.json"), JSON.stringify(top.map(([t, r, c]) => ({ tid: Number(t), run_ms: r / 1e6, comm: c }))));
        statProc = spawn("perf", ["stat", "--per-thread", "-x", STAT_SEP, "-e", STAT_EVENTS, "-t", top.map((x) => x[0]).join(","),
                                  "-o", join(outdir, "stat.txt")], { stdio: "ignore" });
        console.log(`[mcperf] perf stat (${STAT_EVENTS}) on tids ${top.map((x) => x[0]).join(",")} for the window`);
      }, 1000);
    } else if (STAT_EVENTS && !statProc) {
      statProc = spawn("perf", ["stat", "--per-thread", "-x", STAT_SEP, "-e", STAT_EVENTS, "-p", String(recPid),
                                "-o", join(outdir, "stat.txt")], { stdio: "ignore" });
      console.log(`[mcperf] perf stat (${STAT_EVENTS}) on pid ${recPid} for the window`);
    }
    if (THREADS && !thr0) {
      thr0 = taskSnapshot(recPid); thrT0 = monotonic();
      wchanTimer = setInterval(() => {
        for (const t of thr0.keys()) {
          try {
            const w = readFileSync(`/proc/${recPid}/task/${t}/wchan`, "utf8").trim() || "0";
            const st = readFileSync(`/proc/${recPid}/task/${t}/stat`, "utf8");
            const state = st.slice(st.lastIndexOf(")") + 2, st.lastIndexOf(")") + 3);
            const k = `${t}\t${state === "R" ? "R(running)" : w}`;
            wchanHist.set(k, (wchanHist.get(k) || 0) + 1);
          } catch { }
        }
      }, 200);
    }
  } else if (name === "benchEnd") {
    if (statProc && statProc !== "pending") { statProc.kill("SIGINT"); }
    if (thr0) {
      clearInterval(wchanTimer);
      const wall = (monotonic() - thrT0) * 1e9;
      const now = taskSnapshot(recPid);
      const rows = [];
      for (const [t, a] of thr0) {
        const b = now.get(t);
        if (!b) continue;
        const run = b.run - a.run, rq = b.rq - a.rq;
        const w = [...wchanHist].filter(([k]) => k.startsWith(`${t}\t`)).map(([k, n]) => [k.split("\t")[1], n]).sort((x, y) => y[1] - x[1]);
        rows.push({ tid: Number(t), comm: b.comm, run_s: run / 1e9, rq_s: rq / 1e9, sleep_s: (wall - run - rq) / 1e9, wchan: w.slice(0, 8) });
      }
      rows.sort((x, y) => y.run_s - x.run_s);
      writeFileSync(join(outdir, "threads.json"), JSON.stringify({ wall_s: wall / 1e9, threads: rows }, null, 1));
      console.log(`[mcperf] threads.json: ${rows.length} threads over ${(wall / 1e9).toFixed(1)} s`);
    }
  }
}

function startPerf (pid) {
  recPid = pid;
  /* -k 1 (CLOCK_MONOTONIC) is MANDATORY, and for two independent reasons. `perf inject --jit` refuses
   * outright without it ("jitted code must be sampled with perf record -k 1") and then fails every MMAP2
   * event, producing a perf.data that reports NOTHING AT ALL — an empty report rather than an error. And
   * it is the clock the phase windows in phases.json are expressed in, so without it `--time` slices
   * against a different timebase and silently returns the wrong samples (or none). */
  /* MCPERF_PERIOD switches from frequency mode (-F) to a FIXED PERIOD (-c): one sample per N events.
   *
   * Why it matters here. R101 argued this from thermal throttling; THAT PREMISE IS RETRACTED (R227
   * addendum 3, re-confirmed 2026-09-10: the busy P-cores hold 3.6-3.7 GHz, because only two threads
   * work and a ~45 W PL1 sustains that). The conclusion survives on its own grounds: fps on this box has
   * an ~12% floor over a >=100 s window, so a 1-2% codegen change is unresolvable in TIME no matter why,
   * while instructions retired for the same work are clock- and scheduler-independent and reproduce to
   * ~0.8%. With a fixed period,
   * sample count in a window is exactly instructions/N, so slicing the in-game window and dividing by the
   * window's frame count yields INSTRUCTIONS PER FRAME -- a clock-independent measure of codegen quality
   * that can validate changes fps cannot. Frequency mode cannot do this: perf varies the period to hit the
   * target rate, so sample count no longer maps to a fixed number of events. */
  /* MCPERF_EXTRA: extra `perf record` args, space-separated (the ledger passes `-b` for LBR stacks). */
  const EXTRA = (process.env.MCPERF_EXTRA || "").split(/\s+/).filter(Boolean);
  const argv = ["record", "-q", "-k", "1", "-g", ...EXTRA,
                ...(PERIOD ? ["-c", PERIOD] : ["-F", FREQ]),
                "-e", EVENT, "-o", dataPath, "-p", String(pid)];
  rec = spawn("perf", argv, { stdio: ["ignore", "pipe", "pipe"] });
  recStart = monotonic();
  rec.stderr.on("data", (d) => { recErr += d.toString(); });
  /* perf prints some fatal errors to STDOUT, not stderr. */
  rec.stdout.on("data", (d) => { recErr += d.toString(); });
  rec.on("exit", (code, sig) => {
    if (!stopping) console.error(`!! perf record exited EARLY (code=${code} sig=${sig}) — the rest of the run is UNPROFILED`);
  });
  /* Announce the mode that is actually in force. This line used to print `@${FREQ}Hz` unconditionally,
   * so a fixed-period run advertised itself as 499 Hz -- the same lie phases.json tells by writing both
   * `freq` and `period`, which is what made perfraw.py --by-thread report 356% of one core. */
  console.log(`[mcperf] recording pid ${pid} (${EVENT}@${PERIOD ? `1/${PERIOD} events` : `${FREQ}Hz`}, -g)`
              + ` from monotonic ${recStart.toFixed(3)}`);
}

let stopping = false, recEnd = null;
async function stopPerf () {
  if (!rec || stopping) return;
  stopping = true;
  recEnd = monotonic();
  rec.kill("SIGINT");
  /* perf needs a moment after SIGINT to write out its buffers. Killing the browser first (which is what
   * runSession's finally does immediately after this hook) while perf is still draining truncates the
   * capture, and a truncated perf.data reports a plausible SUBSET rather than an error. */
  /* 10 s used to be enough. A ~2.8 GB LBR capture with a FRONTEND_RETIRED event took longer, mcperf exited with
   * perf still writing, and the ledger that ran next read the header's stale data_size: no in-game samples
   * (plan2x b1 g3a/b/c, 2026-10-01). Wait for the exit; say so if it never comes. */
  const tw = monotonic();
  for (let i = 0; i < 1200 && rec.exitCode === null && rec.signalCode === null; i++) await sleep(250);
  if (rec.exitCode === null && rec.signalCode === null)
    console.error(`!! perf record still running ${(monotonic() - tw).toFixed(0)}s after SIGINT -- perf.data may be incomplete`);
  console.log(`[mcperf] perf stopped after ${(recEnd - recStart).toFixed(1)}s (drain ${(monotonic() - tw).toFixed(1)}s)`);
}

/* ------------------------------------------------------------------------------------------------ *
 * Run
 * ------------------------------------------------------------------------------------------------ */

// Started WITHOUT await so the pid resolution below races it: runSession spawns the browser synchronously
// before its first await, so the CDP endpoint is on its way up by the time we start polling.
const sessionP = runSession({
  seed: SEED,
  url, profile, logPath, jsFlags, headless: HEADLESS, benchMs: BENCH_MS, warmMs: WARM_MS, walk: WALK, dumps: has("dumps"),
  /* mcperf owns the map: it copies it into the outdir and symbolmap.txt points perf report at /tmp. */
  keepPerfMap: true,
  /* See mcbench: default ON so the capture covers the write path too. The perf recorder is stopped in
   * runSession's `finally` via hooks.onEnd, which runs AFTER the quit, so the save IS inside the capture. */
  saveQuit: !has("no-save-quit"),
  /* The renderer that exists when perf attaches is not always the one that ends up running the game:
   * a cross-origin navigation swaps processes, so the pid resolved at t=0 can be an earlier renderer
   * that then goes idle. Re-check once the world is up -- still before the warm-up hold, so the whole
   * measured window is covered by the restarted capture. */
  hooks: { onEnd: stopPerf, onMarker: reattachAtInworld },
});

let conn = null;
try {
  conn = await cdpConnect();
  const pid = await rendererPid(conn);
  if (!pid) console.error("!! could not resolve the renderer pid — this run is UNPROFILED");
  else startPerf(pid);
} catch (e) {
  console.error(`!! perf attach failed (${e && e.message}) — this run is UNPROFILED`);
} finally {
  try { conn?.close(); } catch { }
}

const r = await sessionP;
await stopPerf();

console.log(`\n[mcperf] run ${r.ok ? "COMPLETE" : "INCOMPLETE"}`);
console.log(formatPhases(r));
if (r.faults.length) {
  const kinds = [...new Set(r.faults.map((f) => f.kind))].join(", ");
  console.log(`  !! faults (${kinds}):`);
  for (const f of r.faults.slice(0, 4)) console.log(`     [${f.kind}] ${f.line}`);
}

/* ------------------------------------------------------------------------------------------------ *
 * Windows + inject
 * ------------------------------------------------------------------------------------------------ */

/* Clamp every window to what was actually RECORDED. A phase that began before perf attached (boot always
 * does, by however long renderer-pid resolution took) or ended after it stopped would otherwise be
 * reported at full length while holding only part of its samples, and the per-phase shares would be
 * quietly wrong in a way nothing in the output hints at. Clamping plus an explicit `covered` fraction
 * makes the gap visible instead. */
const phases = [];
for (const p of [...r.phases, ...r.subphases]) {
  const t0 = recStart != null ? Math.max(p.t0, recStart) : p.t0;
  const t1 = recEnd != null ? Math.min(p.t1, recEnd) : p.t1;
  const covered = p.secs > 0 ? Math.max(0, (t1 - t0) / p.secs) : 0;
  phases.push({
    name: p.name, what: p.what,
    t0: Number(p.t0.toFixed(3)), t1: Number(p.t1.toFixed(3)), secs: Number(p.secs.toFixed(2)),
    window: `${t0.toFixed(3)},${t1.toFixed(3)}`,
    covered: Number(covered.toFixed(3)),
    top: !p.name.includes("."),
  });
}
writeFileSync(join(outdir, "phases.json"), JSON.stringify({
  event: EVENT, freq: Number(FREQ), period: PERIOD || null, jitdump: JITDUMP, url, knobs,
  glRenderer: r.glRenderer, env, envSample: sampleEnv(), forced: has("force"), deployment,
  /* Both are needed to know whether this capture is comparable to another one: world.generate is bimodal
   * (mcdrive's classifyWorldGen) and a stall capture's world phase is a different workload, not a slower
   * one. `opfs` carries the per-phase op counts the app already logs. */
  worldGenMode: r.worldGenMode ?? null, opfs: r.opfs ?? null, admit: r.admit ?? null,
  window: r.window, ingameVoid: r.window ? !!r.window.void : null,
  recStart: recStart != null ? Number(recStart.toFixed(3)) : null,
  recEnd: recEnd != null ? Number(recEnd.toFixed(3)) : null,
  ok: r.ok, spawnMs: r.spawnMs, phases,
}, null, 2));
writeFileSync(join(outdir, "run.json"), JSON.stringify({
  ok: r.ok, url, knobs, spawnMs: r.spawnMs,
  phases: Object.fromEntries(r.phases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
  subphases: Object.fromEntries(r.subphases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
  benchSummary: r.benchSummary, faults: r.faults, verdict: r.verdict, spin: r.spin ?? null,
  faultSummary: r.faultSummary ?? null, markers: [...r.marks.keys()],
}, null, 2));

/* A VOID IN-GAME WINDOW MUST BE SHOUTED ABOUT HERE, not just in mcbench.
 *
 * mcbench prints the window summary and flags a zero-frame window; mcperf did not print it at all, so a
 * profiled capture whose render thread died at world-join produced a complete-looking phases.json, a full
 * perf.data, and an `ingame` window that mcreport would happily bucket into a 26-row subsystem table -- of a
 * game that was executing nothing. That happened on the first capture of the new IKVM build. Boot and world
 * were fine; only the in-game third was worthless, and nothing said so. */
if (r.window && r.window.void) {
  console.log(`\n  !! IN-GAME WINDOW IS VOID: the frame counter never advanced across the whole ${(r.window.wallMs / 1000).toFixed(0)}s window.`);
  console.log(`     The render thread was not running (usually the intermittent worker crash at world-join). The boot and`);
  console.log(`     world phases of this capture are still valid; the 'ingame' phase is NOT — do not report shares from it.`);
} else if (r.window) {
  console.log(`\n  in-game: ${r.window.frames} frames, ${r.window.fps} fps, ${r.window.msPerFrame} ms/frame, heap ${r.window.heapMb} MB`);
}

for (const p of phases.filter((x) => x.top)) {
  if (p.covered < 0.98)
    console.log(`  note: only ${(p.covered * 100).toFixed(0)}% of phase '${p.name}' is inside the capture`
      + (p.name === "boot" ? " (perf attaches after the renderer exists — expected)" : ""));
}

if (!existsSync(dataPath)) {
  console.error(`\n!! no perf.data. perf record's own output:\n${recErr.slice(0, 800) || "(empty)"}`);
  process.exit(1);
}
const mb = (statSync(dataPath).size / 1048576).toFixed(1);
console.log(`\n[mcperf] perf.data ${mb} MB`);

/* Sample count is the cheapest possible check that the capture is real. An empty or near-empty capture
 * looks identical to "this phase does no work" in every downstream report. */
const nsamp = (() => {
  try {
    const s = execSync(`perf report -i ${JSON.stringify(dataPath)} --stdio --header-only 2>/dev/null | grep -oE 'nr_samples = [0-9]+'`, { encoding: "utf8" });
    return Number(s.match(/(\d+)/)?.[1] || 0);
  } catch { return 0; }
})();
if (nsamp) console.log(`[mcperf] ${nsamp.toLocaleString()} samples`);
if (nsamp && nsamp < 1000) console.log(`  !! that is far too few for a ${((recEnd - recStart) || 1).toFixed(0)}s capture — check the event and the pid`);

/*
 * V8's basic-prof symbol map, and why it needs saying out loud.
 *
 * --perf-basic-prof makes the RENDERER write /tmp/perf-<pid>.map, and `perf report` resolves generated
 * frames by looking that exact absolute path up at REPORT time -- not at record time. So the capture in
 * this directory is not self-contained: it depends on a file in a RAM-backed tmpfs that a reboot, a
 * /tmp cleaner, or the next run's pid collision will take away. When it goes, the report does not fail --
 * every generated frame silently degrades to a bare hex address, which reads as "the JIT emitted nothing"
 * rather than as "the symbols are gone". (perfrun.mjs documents the identical trap for the jitdump tier's
 * jitted-*.so files, and it has already happened once there.)
 *
 * So: copy it in, report its size, and record the path the report will actually use.
 */
if (!JITDUMP && recPid) {
  const map = `/tmp/perf-${recPid}.map`;
  if (!existsSync(map)) {
    /* GATE, and it was MISSING here while its lesser sibling below had one. A map that is merely SMALL
     * sets exitCode 3; a map that does not exist AT ALL -- strictly the worse failure, and what a late or
     * wrong perf attach produces -- only printed, so the run exited 0 and read as valid. Observed
     * 2026-09-12: a control capture with `0% of phase 'boot'` and `0% of phase 'world'` inside the window
     * and no map whatsoever still returned success, which is precisely the "reads as a valid low-overhead
     * run" trap R251 added the sibling gate for. Same exit code, same wording. */
    console.error(`!! CAPTURE IS VOID: ${map} does not exist -- V8 wrote no symbol map, so every generated`);
    console.error(`   frame would be a bare address. --no-sandbox and --js-flags=--perf-basic-prof are both`);
    console.error(`   required, and perf must attach to the renderer that ran the game (pid ${recPid} did not).`);
    console.error(`   Do not read this capture.`);
    process.exitCode = 3;
  } else {
    const lines = readFileSync(map, "utf8").split("\n").length - 1;
    const mapMB = statSync(map).size / 1048576;
    console.log(`[mcperf] symbol map ${map}: ${mapMB.toFixed(1)} MB, ${lines.toLocaleString()} symbols`);
    /* GATE, not a note. perf SILENTLY MIS-ATTACHES: if it lands on a renderer that is not the one
     * running the game, the run completes, the phases look right and the capture is worthless -- 0.3 MB
     * and ~3,965 symbols against a healthy ~227 MB and ~2M. That exact pair was printed and ignored once
     * per arm for a whole four-arm matrix (2026-09-12) because this line only REPORTED it. A real
     * in-game capture of this workload cannot produce a map under ~50 MB. */
    if (mapMB < 50) {
      console.error(`!! CAPTURE IS VOID: a real in-game run writes ~200+ MB of symbol map, not ${mapMB.toFixed(1)} MB.`);
      console.error(`   perf attached to pid ${recPid}, which is evidently NOT the renderer that ran the game.`);
      console.error(`   Do not read this capture. See rendererPid()/the inworld re-resolve in this file.`);
      process.exitCode = 3;
    }
    const cp = spawnSync("cp", [map, join(outdir, `perf-${recPid}.map`)]);
    /* runSession keeps this map for us (keepPerfMap) on the understanding that we own its lifetime -- and
     * nothing removed the /tmp original, so every mcperf arm left ~240 MB in a RAM-backed /tmp: 764 of them
     * had preflight refusing arms at 1.9 GB free (R315, R280f's failure again). The readers here all take
     * the copy; only a hand-run `perf report` wants the /tmp path, and symbolmap.txt says how to restore it.
     * Removed only once the copy is known good. */
    const copied = cp.status === 0 && existsSync(join(outdir, `perf-${recPid}.map`)) &&
      statSync(join(outdir, `perf-${recPid}.map`)).size === statSync(map).size;
    if (copied)
      rmSync(map, { force: true });
    writeFileSync(join(outdir, "symbolmap.txt"),
      `${map}\n# copy kept beside this file as perf-${recPid}.map` + (copied ? "; the /tmp original was removed.\n" : ".\n")
      + `# perf report reads the /tmp path above, not the copy -- copy it back there before running perf report.\n`);
  }
}

let final = dataPath;
if (JITDUMP) {
  const dumps = readdirSync(outdir).filter((f) => /^jit-\d+\.dump$/.test(f));
  if (!dumps.length) {
    console.error(`!! no jit-*.dump in ${outdir}; V8 wrote no jitdump despite --perf-prof.`);
    console.error(`   --no-sandbox is required for the renderer to be able to write it at all.`);
  } else {
    const jitted = join(outdir, "perf.jitted");
    if (NO_INJECT) {
      console.log(`[mcperf] --no-inject: ${dumps.length} raw jit-*.dump kept; perfraw.py / hotinsn.py read these`);
      console.log(`[mcperf]   (skipping perf inject --jit: ~2M .so files, and its success path deletes these dumps)`);
    } else {
    const inj = spawnSync("perf", ["inject", "--jit", "-i", dataPath, "-o", jitted],
      { env: { ...process.env, JITDUMPDIR: outdir }, encoding: "utf8" });
    /* A failed inject STILL LEAVES A FILE BEHIND, and reporting off it prints empty sections that read as
     * "no samples" rather than "inject broke". So verify the injected file actually reports something
     * before adopting it, exactly as srvperf.sh learned to. */
    /* --no-inline and a timeout are BOTH load-bearing, learned by wedging the machine.
     *
     * perf resolves inline frames by spawning a PERSISTENT addr2line per DSO, and `perf inject --jit` turns
     * this profile into one DSO per JIT'd function -- 2,039,711 of them, 17 GB, on a single Minecraft capture.
     * So this one verification command forked 1300+ addr2line processes, all idle, and never returned; it had
     * to be killed by hand along with the pile. --no-inline is what stops perf reaching for addr2line at all.
     *
     * The timeout is the backstop: this is a liveness check, so if it cannot answer in 120s the answer is "not
     * usable" regardless of why. Hanging here is strictly worse than reporting unusable, because the caller
     * still has the basic-prof profile to fall back on. */
    const usable = existsSync(jitted) && (() => {
      try {
        return execSync(`timeout 120 perf report --no-inline -i ${JSON.stringify(jitted)} --stdio 2>/dev/null | grep -c '%'`,
          { encoding: "utf8" }).trim() !== "0";
      }
      catch { return false; }
    })();
    if (inj.status === 0 && usable) {
      final = jitted;
      console.log(`[mcperf] jitdump folded in -> ${jitted}`);
      /* perf inject explodes the dump into one jitted-<pid>-<n>.so per function. They CANNOT be deleted:
       * perf.jitted only REFERENCES them, so removing them turns every symbol back into a bare hex
       * address and silently produces an unsymbolised profile that looks like the JIT emitted nothing.
       * Drop only the raw dumps. */
      let freed = 0;
      for (const f of readdirSync(outdir)) if (/^jit-\d+\.dump$/.test(f)) { rmSync(join(outdir, f), { force: true }); freed++; }
      console.log(`[mcperf] dropped ${freed} raw dump(s); jitted-*.so are REQUIRED for symbols — delete the whole outdir when done`);
    } else {
      console.error(`!! perf inject --jit produced nothing usable: ${(inj.stderr || "").slice(0, 300)}`);
      console.error(`   falling back to ${dataPath}; generated frames will be bare addresses`);
    }
    }
  }
}

/* The browser profile is 700 MB of reflinked seed and is of no analytical value once the run is over. */
rmSync(profile, { recursive: true, force: true });
const sz = execSync(`du -sh ${JSON.stringify(outdir)} | cut -f1`, { encoding: "utf8" }).trim();
console.log(`[mcperf] ${outdir} is ${sz}`);
writeFileSync(join(outdir, "capture.txt"), `${final}\n`);
console.log(`\nnext: wj perfraw ${outdir} --phase ingame --by-thread`);
process.exit(r.ok ? 0 : 3);
