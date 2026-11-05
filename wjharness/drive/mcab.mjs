// Interleaved A/B over full Minecraft, with the variance made explicit instead of assumed away.
//
// mcbench.mjs runs one arm N times. That is the wrong shape for a comparison on this box, and bench.mjs's
// preflight already says why: sustained measuring takes this package 62 -> 83 C and drifts ~6% across a
// sequential sweep. A Minecraft run is ~6 MINUTES, so "3 runs of A then 3 runs of B" spans forty minutes
// of monotonically rising die temperature and hands the second arm a systematically slower machine. Every
// number in the comparison then moves in the same direction, which is indistinguishable from a real
// regression -- and the arms differ by exactly the amount you were hoping to detect.
//
// So arms are interleaved (A B A B ...), and the report states the SPREAD of each arm next to the
// difference between them. That pairing is the whole point: a 4% difference between two arms whose own
// runs spread 9% is not a result, and printing it without the spread invites it to be treated as one.
//
// Usage:
//   wj mcab --a base --b noisland --b-knob island=0 [--rounds 3] [--bench-ms 60000]
//   wj mcab --a base --a-knob threshold=500 --b hot --b-knob threshold=100
//
// Reads/writes the same $WJ_HOME/state/mc-results.jsonl as mcbench.mjs, so results accumulate across
// sessions and a later run can be compared against an older arm without re-running it.
import { runSession, buildUrl, seedProfile, formatPhases, seedFromArgs } from "../lib/mcdrive.mjs";
import { preflight } from "../lib/preflight.mjs";
import { mkdirSync, appendFileSync, readFileSync, existsSync } from "fs";
import { join, resolve } from "path";
import { defaultOut, STATE } from "../lib/paths.mjs";
import { coolTo, pkgTempC, busyFreqMhz } from "../lib/thermal.mjs";

const args = process.argv.slice(2);
/* --seed NAME|DIR (lib/seeds.mjs), else MC_SEED, else mcsr */
const SEED = seedFromArgs(args);
const has = (n) => args.includes(`--${n}`);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
const knobsFor = (prefix) => {
  const out = {};
  for (let i = 0; i < args.length; i++) {
    if (args[i] !== `--${prefix}-knob`) continue;
    const [k, ...rest] = (args[++i] || "").split("=");
    if (k) out[k] = rest.join("=");
  }
  return out;
};

const A = str("a", null), B = str("b", null);
if (!A || !B) { console.error("usage: mcab.mjs --a <tagA> --b <tagB> [--a-knob k=v] [--b-knob k=v] [--rounds N] [--bench-ms N] [--no-walk] [--reuse]"); process.exit(1); }
const ROUNDS = num("rounds", 3);
const BENCH_MS = num("bench-ms", 60000);
const WARM_MS = num("warm-ms", 0);
const COOLDOWN_MS = num("cooldown-ms", 0);   // now an UPPER BOUND on the thermal wait, not a fixed sleep  // Round 102: interleaving alone does NOT cancel thermal drift; Round 99: hold before the window so it measures the PLATEAU
const COOL_TARGET_C = num("cool-target-c", 55);   // gate each arm on package temp, not elapsed time
let thermalBefore = null;   // what the thermal gate observed for the arm about to run
const WALK = !has("no-walk");
const arms = [{ tag: A, knobs: knobsFor("a") }, { tag: B, knobs: knobsFor("b") }];

const OUT = defaultOut("mcab", (() => { const i = process.argv.indexOf("--out"); return i >= 0 ? process.argv[i + 1] : null; })());
const JSONL = join(STATE(), "mc-results.jsonl");

/* --reuse folds in whatever is already on disk for these tags. Useful when one arm is a long-lived
 * baseline; dangerous if the runtime has been republished since, because nothing in the row records WHICH
 * dotnet.native.wasm produced it -- so it is opt-in and warns. (deploy.sh exists because a whole
 * measurement matrix once ran on a stale runtime.) */
const existing = [];
if (has("reuse") && existsSync(JSONL)) {
  for (const l of readFileSync(JSONL, "utf8").split("\n").filter(Boolean)) {
    try { const r = JSON.parse(l); if (r.tag === A || r.tag === B) existing.push(r); } catch { }
  }
  if (existing.length) console.log(`--reuse: folding in ${existing.length} existing row(s). These may predate the current runtime — nothing in a row records which dotnet.native.wasm produced it.`);
}

console.log(`[mcab] ${A} vs ${B}, ${ROUNDS} interleaved round(s) => ${ROUNDS * 2} runs, ~${(ROUNDS * 2 * 6.5).toFixed(0)} min`);
for (const a of arms) console.log(`  ${a.tag.padEnd(12)} ${Object.keys(a.knobs).length ? JSON.stringify(a.knobs) : "(in-source defaults)"}`);
if (JSON.stringify(arms[0].knobs) === JSON.stringify(arms[1].knobs))
  console.log(`  note: both arms have identical knobs — this measures the harness's own run-to-run spread, which is a genuinely useful thing to know before trusting any A/B.`);
preflight({ force: has("force") });

const rows = [...existing];
for (let round = 1; round <= ROUNDS; round++) {
  for (const arm of arms) {
    const name = `${arm.tag}-r${round}`;
    const profile = join(OUT, `profile-${name}`);
    const logPath = join(OUT, `${name}.log`);
    const url = buildUrl({ knobs: arm.knobs, seed: SEED });
    console.log(`\n=== round ${round}/${ROUNDS}  arm ${arm.tag} ===`);
    /* COOLDOWN BEFORE EVERY ARM, not just between rounds.
     *
     * Round 100 measured this box (i7-1360P, 28 W) throttling 2106 -> 1403 MHz WITHIN a single run, with
     * 8.5k-13k core-throttle events per run, and three identical back-to-back runs drifting 24.47 -> 24.05
     * -> 22.22 fps. Interleaving arms does not fix that: this loop always runs arm B after arm A inside a
     * round, so B is systematically measured on a hotter machine and a small real win looks like a small
     * loss. An idle gap before EACH arm is what removes the bias (it took spread 9.4% -> 5.7% in mcbench). */
    /* THERMAL GATE, not a fixed sleep. The comment above is right that an idle gap removes the bias, but
     * a duration is the wrong control variable: this package swings 400 MHz .. 4.7 GHz, so a fixed wait
     * starts each arm at whatever clock the governor is holding. Measured 2026-08-29 (direct_import A/B,
     * 240 s cooldowns): the `noimp` arm read 21.57, 20.77, then 14.22 fps -- 35% self-spread, which no
     * number of rounds averages away and which is far outside R149's 11.5% median. Waiting for a
     * TEMPERATURE means every arm starts from the same thermal state; it also stops sooner than 240 s on a
     * cold box and longer on a hot one, which is exactly when the fixed wait was wrong in each direction. */
    if (COOLDOWN_MS > 0 && !(round === 1 && arm === arms[0])) {
      thermalBefore = await coolTo ({ targetC: COOL_TARGET_C, minMs: 20000, maxMs: COOLDOWN_MS * 2,
                                      log: (m) => console.log(`[mcab] ${m}`) });
    } else {
      thermalBefore = { start: pkgTempC (), end: pkgTempC (), waitedMs: 0, freq: busyFreqMhz () };
    }
    seedProfile(profile, { seed: SEED });
    let r;
    try {
      /* saveQuit defaults ON in runSession; --no-save-quit reverts to the old SIGKILL ending. It runs
       * after the window closes, so it cannot move an in-game metric -- but it does add ~10-30s per run to
       * an A/B that already costs ~12 min per arm, which is the only reason an opt-out exists. */
      r = await runSession({ seed: SEED, url, profile, logPath, benchMs: BENCH_MS, warmMs: WARM_MS, walk: WALK,
                             saveQuit: !has("no-save-quit") });
      if (r && thermalBefore) r.thermal = { ...thermalBefore, endC: pkgTempC (), endFreq: busyFreqMhz () };
    } catch (e) { console.log(`  !! threw: ${e && e.message}`); continue; }
    console.log(formatPhases(r));
    if (r.window) console.log(`  ingame: ${r.window.fps} fps  ${r.window.msPerFrame} ms/frame  heap ${r.window.heapMb} MB`);
    if (r.faults.length) console.log(`  !! ${r.faults.length} fault line(s), kinds: ${[...new Set(r.faults.map((f) => f.kind))].join(", ")}`);
    const row = {
      tag: arm.tag, run: round, url, knobs: arm.knobs, ok: r.ok,
      phases: Object.fromEntries(r.phases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
      subphases: Object.fromEntries(r.subphases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
      spawnMs: r.spawnMs,
      /* fpsPerSecond is recorded, not just the window mean, because THE WINDOW MEAN IS NOT A USABLE
       * STATISTIC HERE. The in-game window is a JIT-warmup RAMP, so the mean measures where in the ramp
       * the window happened to land. Measured case: two runs of the SAME arm gave 5.83 and 21.26 fps —
       * a 3.6x "difference" that was entirely shape. The 21.26 run entered the window already warm
       * (15 fps at second 3), peaked at 28 and DECAYED to 7; the other rose monotonically 2 -> 9. See
       * fpsTail below for the statistic that survives this. */
      fps: r.window ? { frames: r.window.frames, fps: r.window.fps, msFrame: r.window.msPerFrame,
                        heapMb: r.window.heapMb, fpsPerSecond: r.window.fpsPerSecond ?? null,
                        frameTime: r.window.frameTime ?? null } : null,
      worldGenMode: r.worldGenMode ?? null, opfs: r.opfs ?? null,
      faultEvents: r.faults.map((f) => ({ kind: f.kind, cls: f.cls, key: f.key, sev: f.sev, lines: f.lines ?? 1 })),
      verdict: r.verdict, spin: r.spin ?? null,
      faults: Object.values(r.faultSummary ?? {}).map((f) => `${f.cls}:${f.key}`),
      markers: [...r.marks.keys()], at: new Date().toISOString(),
    };
    appendFileSync(JSONL, JSON.stringify(row) + "\n");
    rows.push(row);
  }
}

/* ------------------------------------------------------------------------------------------------ *
 * Report
 * ------------------------------------------------------------------------------------------------ */

const med = (xs) => {
  const s = [...xs].sort((a, b) => a - b);
  if (!s.length) return null;
  return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
};
/* Median of the LAST tailSecs one-second fps buckets. This is the primary in-game metric: it samples the
 * converged end of the warmup ramp instead of averaging across it, and a median rather than a mean so one
 * GC-stalled second cannot carry the number. On the four dpicoff/dpicon runs that motivated it, the tail
 * statistic cut the within-arm spread from 107%/265% (window mean) to 15% for one arm — i.e. it is the
 * difference between an A/B that can resolve a 10-20% effect and one that cannot. */
const TAIL_SECS = num("tail-secs", 10);
const tailFps = (r) => {
  const ps = r.fps?.fpsPerSecond;
  if (!Array.isArray(ps) || !ps.length) return null;
  return med(ps.slice(-TAIL_SECS));
};

/* A run that ENDS well below its own peak did not measure a steady state, whatever its mean says: it was
 * fast on entry and then degraded, and folding it into a median with converged runs averages two different
 * scenarios. Judged by tail/max, NOT by where the peak index falls: `indexOf(max)` labels a run that
 * saturates early and then holds its ceiling as "decaying" (observed on a 5,6,17...26,24,26,25 run whose
 * tail was its maximum), which is exactly backwards. */
const rampShape = (r) => {
  const ps = r.fps?.fpsPerSecond;
  if (!Array.isArray(ps) || ps.length < 6) return "?";
  const mx = Math.max(...ps), tail = med(ps.slice(-TAIL_SECS));
  if (!mx) return "?";
  return tail / mx >= 0.7 ? "converged" : "decaying";
};

/* A run that FAULTED is not a slow run, it is a different run, and folding it into a median silently
 * corrupts the arm. Observed: a worker took `WORKER_TRAP_STACK RuntimeError: memory access out of bounds`
 * mid-window, its fps curve ended `[... 0,0,0,0]`, and because `ok` was still true the crashed run's p50
 * became the arm's median and inverted the comparison. That trap is a KNOWN intermittent on this project
 * (17 logs across 11 unrelated tags, all predating these changes), so it will keep happening and must be
 * excluded rather than hoped away. Excluded loudly below, never silently dropped. */
/* Prefer the canonical verdict; fall back to the old derivation for rows written before it existed,
 * because mc-results.jsonl is append-only and already carries several schema generations. */
const usable = (r) => (r.verdict ? r.verdict === "ok" : (r.ok && (!r.faults || r.faults.length === 0)))
  /* ...and not VOID. A window that produced no frames at all is not a slow run, it is a non-run: observed
   * `curB-r2` reporting fps 0 with fpsPerSecond [0,0,0...0,0,0] and no fault line, which then became part of
   * its arm's median and produced a "9.5% WORSE" verdict between two IDENTICAL configurations. The harness
   * already computes a `void` flag for exactly this; honour it, and treat zero frames as void regardless. */
  && (r.fps?.frames ?? 0) > 0 && (r.fps?.fps ?? 0) > 0;
/* METRICS THAT MUST NOT BE AVERAGED ACROSS WORLD-GENERATION MODES.
 *
 * `world.generate` is bimodal -- ~17-18% of runs pay a near-constant +60 s (see mcdrive's
 * classifyWorldGen for the histogram and the three independent incidence measurements). Folding one stall
 * run into a 3-run median moves that arm's `world` by ~20 s, which is larger than any effect this harness
 * has ever tried to measure, and it does so at random. So for these keys the median is taken over
 * FAST-MODE runs only, and the dropped count is printed rather than swallowed.
 *
 * `world.load` is deliberately NOT in this set: it measures 16.3 s fast vs 18.2 s stall, i.e. it is flat
 * across the modes, and that flatness is exactly the evidence that the stall is not in the OPFS read path.
 * Filtering it would hide the control. */
const MODE_SENSITIVE = new Set(["world", "world.gen", "world.generate"]);

const armRuns = (tag) => rows.filter((r) => r.tag === tag && usable(r));
const modeCensus = (tag) => {
  const c = { fast: 0, mid: 0, stall: 0, unknown: 0 };
  for (const r of armRuns(tag)) c[r.worldGenMode ?? "unknown"]++;
  return c;
};
const pick = (tag, key) => {
  let rs = armRuns(tag);
  if (MODE_SENSITIVE.has(key)) {
    const fast = rs.filter((r) => r.worldGenMode === "fast");
    /* Fall back to the unfiltered set rather than reporting nothing -- an arm with no fast run still needs
     * a number, it just needs to be labelled MIXED in the report. */
    if (fast.length >= 2) rs = fast;
  }
  return rs
    .map((r) => key === "fps" ? r.fps?.fps : key === "fpsTail" ? tailFps(r)
         : key === "p50ms" ? r.fps?.frameTime?.p50 : key === "p90ms" ? r.fps?.frameTime?.p90
         : key === "msFrame" ? r.fps?.msFrame : (r.phases[key] ?? r.subphases[key]))
    .filter((x) => x != null && Number.isFinite(x));
};

/* p50ms FIRST, because it is the only in-game number here that survives the window's regime structure.
 * A --walk window contains three regimes: JIT warmup, a plateau, and (once the player leaves pre-generated
 * terrain) a sustained chunk-generation collapse. Measured on one 60s window: plateau 16-23 fps for seconds
 * 11-38, then 2-3 fps to the end. The three candidate metrics score that window at:
 *     mean fps 11.15  |  fpsTail (last 10s) 2.5  |  p50 frame time 57.18 ms = 17.5 fps
 * p50 frame time is right because it is weighted per FRAME: a 20s collapse emits ~50 frames while a 28s
 * plateau emits ~500, so the plateau dominates the median automatically. The mean is weighted per SECOND and
 * the tail is weighted entirely on the worst regime -- fpsTail was my own "fix" for the mean and it is worse,
 * measuring the collapse consistently instead of the ramp inconsistently.
 * fpsTail and fps are kept below it only so the difference stays visible in the output. */
const METRICS = [
  ["boot", "s", "lower"], ["boot.classload", "s", "lower"], ["boot.resources", "s", "lower"], ["boot.mainscreen", "s", "lower"],
  ["world", "s", "lower"], ["world.gen", "s", "lower"],
  /* The 83%-knee split. `world.load` is the OPFS read segment and `world.generate` is new-chunk CPU; they
   * behave completely differently and the combined `world.gen` above averages them. */
  ["world.load", "s", "lower"], ["world.generate", "s", "lower"], ["world.join", "s", "lower"],
  ["p50ms", "ms", "lower"], ["p90ms", "ms", "lower"], ["fpsTail", "fps", "higher"], ["fps", "fps", "higher"], ["msFrame", "ms", "lower"],
  /* The write path. `save.write` is the serialisation itself; `save` includes the driver's own menu
   * keystroke latency and is therefore partly a harness constant, not a property of the build. */
  ["save", "s", "lower"], ["save.write", "s", "lower"],
];

console.log(`\n${"=".repeat(96)}`);
console.log(`${A} vs ${B}`);
console.log("=".repeat(96));
const okA = rows.filter((r) => r.tag === A && usable(r)).length, okB = rows.filter((r) => r.tag === B && usable(r)).length;
console.log(`usable runs (ok AND fault-free): ${A} ${okA}, ${B} ${okB}`);
for (const r of rows.filter((x) => x.ok && !usable(x)))
  console.log(`  !! EXCLUDED ${r.tag}-r${r.run}: `
    + (r.faults && r.faults.length ? `${r.faults.length} fault line(s) [${[...new Set(r.faults)].join(", ")}]`
                                   : `VOID window (${r.fps?.frames ?? 0} frames, ${r.fps?.fps ?? 0} fps)`)
    + ` — not a slow run, a different scenario.`);

/* Shape check, the stand-in for jbox2d's checksum. Two runs that reached different marker sets executed
 * different scripts, and folding them into one median produces a number for a workload nobody ran. */
const shapes = new Map();
for (const r of rows.filter((x) => x.ok)) shapes.set(r.tag, (shapes.get(r.tag) || new Set()).add(r.markers.join(",")));
for (const [tag, s] of shapes)
  if (s.size > 1) console.log(`  !! arm ${tag} produced ${s.size} distinct marker sets — its runs are not the same script and its median is not meaningful.`);

/* WORLD-GENERATION MODE CENSUS, printed before the table so a mixed arm is visible while reading it.
 * An arm that mixes fast and stall runs has a `world`/`world.gen`/`world.generate` median describing a
 * workload nobody ran -- the same class of error as the marker-set check above, and ~17-18% likely per
 * run. The MODE_SENSITIVE keys below are taken over fast-mode runs only when at least two exist. */
for (const tag of [A, B]) {
  const c = modeCensus(tag);
  const n = c.fast + c.mid + c.stall + c.unknown;
  if (!n) continue;
  const mixed = [c.fast, c.mid, c.stall].filter((x) => x > 0).length > 1;
  console.log(`  world-gen mode ${tag.padEnd(12)} fast ${c.fast}  mid ${c.mid}  stall ${c.stall}`
    + (c.unknown ? `  unknown ${c.unknown}` : "")
    + (mixed ? `   !! MIXED — world.* medians below use fast-mode runs only (>=2), else read them as MIXED`
             : c.stall === n && n ? `   !! ALL STALL — world.* medians describe the stall mode, not the workload` : ""));
}

console.log(`\n  ${"metric".padEnd(18)} ${A.slice(0, 10).padStart(10)} ${B.slice(0, 10).padStart(10)}  ${"delta".padStart(9)}  ${"spread".padStart(13)}  verdict`);
for (const [key, unit, better] of METRICS) {
  const xa = pick(A, key), xb = pick(B, key);
  if (!xa.length || !xb.length) continue;
  const ma = med(xa), mb = med(xb);
  const pct = ma === 0 ? 0 : 100 * (mb - ma) / ma;
  const spread = (xs) => { const m = med(xs); return m ? 100 * (Math.max(...xs) - Math.min(...xs)) / m : 0; };
  const worst = Math.max(spread(xa), spread(xb));
  /* The verdict is deliberately conservative and deliberately says WHY. A difference smaller than the
   * within-arm spread is not a small effect, it is an effect this many runs cannot see -- and the honest
   * answer is "run more rounds", not a hedged number that will be quoted without its caveat.
   *
   * THE n=1 CASE MUST BE SPECIAL-CASED, and getting this wrong is worse than having no tool. With one run
   * per arm the observed spread is 0 by construction, so `|pct| < worst` is never true and EVERY difference
   * gets a confident verdict. A single round of this A/B duly reported "ways8 WORSE by 64.2%" on world
   * generation -- a phase whose own runs have since been observed spanning 75.8s to 145.9s with no change at
   * all. The number was noise wearing a verdict's clothes. */
  const improved = better === "higher" ? pct > 0 : pct < 0;
  const n = Math.min(xa.length, xb.length);
  const verdict = n < 2
    ? `NO VERDICT POSSIBLE — one run per arm cannot separate this from run-to-run variance (--rounds 2+)`
    : Math.abs(pct) < worst ? `inside noise (spread ${worst.toFixed(1)}%) — needs more rounds`
    : improved ? `${B} BETTER by ${Math.abs(pct).toFixed(1)}%` : `${B} WORSE by ${Math.abs(pct).toFixed(1)}%`;
  /* Show the n a mode-sensitive metric actually used, since it can differ from the arm's usable count. */
  const nTag = MODE_SENSITIVE.has(key) ? ` [n ${xa.length}/${xb.length}]` : "";
  console.log(`  ${key.padEnd(18)} ${ma.toFixed(1).padStart(10)} ${mb.toFixed(1).padStart(10)}  ${(pct >= 0 ? "+" : "") + pct.toFixed(1) + "%"}`.padEnd(63)
    + `  ${(`±${worst.toFixed(1)}%`).padStart(13)}  ${verdict}${nTag}`);
}
console.log(`\n  "spread" is the wider of the two arms' own (max-min)/median over ${ROUNDS} round(s).`);

/* Per-run ramp detail. Printed always, because the aggregate rows above cannot show that two runs of one
 * arm measured different SCENARIOS, and that is the failure mode this harness is most prone to. */
console.log(`\n  per-run in-game ramp (last ${TAIL_SECS}s median is the fpsTail metric):`);
for (const r of rows.filter((x) => x.ok)) {
  const t = tailFps(r), ps = r.fps?.fpsPerSecond;
  console.log(`    ${(r.tag + "-r" + r.run).padEnd(16)} mean ${String(r.fps?.fps ?? "?").padStart(6)}  tail ${String(t ?? "?").padStart(5)}`
    + `  ${rampShape(r).padEnd(8)} ${ps ? `[${ps.slice(0, 3).join(",")} ... ${ps.slice(-4).join(",")}]` : "(no per-second data — older row)"}`);
}
if (rows.some((r) => r.ok && rampShape(r) === "decaying"))
  console.log(`  !! at least one run PEAKED before its final 40% — it entered the window warm and degraded, which is`
    + `\n     a different scenario from a rising ramp. Its mean is not comparable to a rising run's mean.`);
if (ROUNDS < 2)
  console.log(`  !! ${ROUNDS} round means spread is 0 BY CONSTRUCTION and no delta above is a result. This workload's`
    + `\n     own variance has been measured at 75.8-145.9s for world.gen and 85.8-101.5s for boot with NO change`
    + `\n     at all, so use --rounds 3 for anything you intend to act on.`);
console.log(`  rows in ${JSONL}`);
