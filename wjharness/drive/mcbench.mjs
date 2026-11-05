// Wall-clock A/B for the REAL workload — full Minecraft — one fresh isolated browser per run.
//
// This is bench.mjs's counterpart. bench.mjs measures the jbox2d fixed-work kernel and prints one number
// (`mean ms/step`) with a checksum that proves two runs computed the same thing. Minecraft has no such
// number, because it is not one workload:
//
//   boot    IKVM class loading + AOT stub resolution + resource reload   (~150s, barely renders)
//   world   chunk generation / lighting on the integrated server         (~170s, no player input)
//   ingame  render + server tick + chunk meshing, steady state           (a fixed 60s window)
//
// A change can halve boot and do nothing in world, or trade one against the other. So this prints all
// three, plus a breakdown of boot and world into sub-phases, plus the in-game frame time — and the
// integrity check that replaces jbox2d's checksum is that the *same script* ran: the marker list is
// fixed, so a run that reached a different set of markers is INVALID rather than fast.
//
// Usage:
//   wj mcbench [--runs 1] [--bench-ms 60000] [--no-walk] [--headed]
//                                  [--knob name=value ...] [--tag NAME] [--stats] [--gclog] [--force]
//
// Examples:
//   wj mcbench --tag base
//   wj mcbench --tag noisland --knob island=0
//   wj mcbench --runs 3 --bench-ms 90000
//
// Results append to $WJ_HOME/state/mc-results.jsonl so an A/B is a jq away and never depends on scrollback.
import { runSession, buildUrl, seedProfile, formatPhases, seedFromArgs } from "../lib/mcdrive.mjs";
import { preflight, sampleEnv } from "../lib/preflight.mjs";
import { describeDeployment, printDeployment } from "../lib/provenance.mjs";
import { mkdirSync, appendFileSync, writeFileSync, readFileSync } from "fs";
import { join, resolve } from "path";
import { defaultOut, STATE } from "../lib/paths.mjs";

const args = process.argv.slice(2);
/* --seed NAME|DIR (lib/seeds.mjs), else MC_SEED, else mcsr */
const SEED = seedFromArgs(args);
const has = (n) => args.includes(`--${n}`);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };

const RUNS = num("runs", 1);
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
const COOLDOWN_MS = num("cooldown-ms", 0);   // Round 99: idle gap between runs, to test/undo thermal drift
const WALK = !has("no-walk");
const TAG = str("tag", "run");
const HEADLESS = !has("headed");

/* Repeatable --knob name=value. Short names are wasm_jit knobs (island, vcall_ways, ...); a name that
 * already carries an env prefix (MONO_*, IKVM_*, DOTNET_*) passes straight through as an env var. Both
 * forms are handled by index.ts's queryOverrides("wj."). */
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
// Counter attribution needs MONO_WASM_JIT_STATS, which adds inline counter bumps to the invoke/residual/
// IC dispatch paths — i.e. it costs time in the paths being measured. Off by default so the phase timings
// above are clean; --stats when the wasm-jit breakdown is what you came for. Do not read timings from a
// --stats run and compare them to a clean one.
if (has("stats")) knobs.stats = 1;

const OUT = defaultOut("mcbench", (() => { const i = process.argv.indexOf("--out"); return i >= 0 ? process.argv[i + 1] : null; })());
const JSONL = join(STATE(), "mc-results.jsonl");

console.log(`[mcbench] ${TAG}: ${RUNS} run(s), ${(BENCH_MS / 1000).toFixed(0)}s in-game window, walk=${WALK}, headless=${HEADLESS}`);
if (Object.keys(knobs).length) console.log(`[mcbench] knobs: ${JSON.stringify(knobs)}`);
preflight({ force: has("force") });

/* WHAT IS ACTUALLY DEPLOYED, stamped into the run. See lib/provenance.mjs: deploy.sh verifies the mono
 * runtime and nothing verifies the managed assemblies, and IKVM's are rebuilt from a repo that routinely
 * carries uncommitted work. A missing marker aborts, because measuring the wrong bytes is worse than not
 * measuring. */
const deployment = describeDeployment({ markers });
const missing = printDeployment(deployment);
if (missing) { console.error(`\nrefusing to measure: ${missing} marker(s) absent from the served bytes. Rebuild and redeploy.`); process.exit(2); }

const url = buildUrl({ knobs, runtimeOptions, gcLog: has("gclog"), seed: SEED });
console.log(`[mcbench] url: ${url}`);

const results = [];
/* CPU CLOCK AND THROTTLE STATE PER RUN.
 *
 * Round 99: three back-to-back plateau runs of the SAME binary came out 24.47, 24.05, 22.22 fps -- a
 * monotone decline, not scatter, with the third 9% below the first. This box is an i7-1360P: a 28 W mobile
 * part whose all-core clock cannot be sustained through three 8-minute full-load runs. If that is what the
 * decline is, then every sequential A/B here is biased, because mcab always runs arm B after arm A within a
 * round -- B would be measured on a hotter, slower machine. Record the evidence per run rather than
 * inferring it: mean clock over all cpus, and the kernel's cumulative throttle counters. */
const cpuState = () => {
  try {
    const mhz = readFileSync("/proc/cpuinfo", "utf8").split("\n").filter((l) => l.startsWith("cpu MHz"))
      .map((l) => parseFloat(l.split(":")[1]));
    const rd = (f) => { try { return parseInt(readFileSync(f, "utf8").trim(), 10); } catch { return null; } };
    return {
      meanMhz: mhz.length ? Math.round(mhz.reduce((a, b) => a + b, 0) / mhz.length) : null,
      coreThrottle: rd("/sys/devices/system/cpu/cpu0/thermal_throttle/core_throttle_count"),
      pkgThrottle: rd("/sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count"),
    };
  } catch { return {}; }
};

for (let i = 1; i <= RUNS; i++) {
  const name = `${TAG}-${i}`;
  const profile = join(OUT, `profile-${name}`);
  const logPath = join(OUT, `${name}.log`);
  console.log(`\n=== run ${i}/${RUNS} (${name}) ===`);
  console.log(`[mcbench] seeding profile (reflink copy of the seed)`);
  seedProfile(profile, { seed: SEED });

  if (COOLDOWN_MS > 0 && i > 1) {
    console.log(`[mcbench] cooldown ${(COOLDOWN_MS / 1000).toFixed(0)}s (idle, so run ${i} does not inherit run ${i - 1}'s heat)`);
    await new Promise((res) => setTimeout(res, COOLDOWN_MS));
  }
  const cpu0 = cpuState();
  /* Sampled per RUN, not once per invocation: preflight's job is to refuse, and the quantities that move
   * between the runs of one matrix (package temp, MemAvailable, loadavg) are exactly the ones that used to
   * go unrecorded. Cheap, silent, never refuses. */
  const envSample = sampleEnv();
  const t0 = Date.now();
  let r;
  try {
    /* --verbose-console adds Chrome's `--v=1`, admitting console.debug (which `--log-level=0` drops).
     * NOTE this is NOT what hides the GC trace — buildUrl sets `?wj.MONO_LOG_LEVEL=error` unless --gclog
     * is passed, so with the default level there is nothing to capture at any Chrome verbosity. Reaching
     * for --v=1 to explain missing GC lines was a wrong diagnosis; use --gclog. */
    r = await runSession({ seed: SEED, url, profile, logPath, headless: HEADLESS, benchMs: BENCH_MS, warmMs: WARM_MS, walk: WALK, dumps: has("dumps"),
                           /* Default ON: the run used to end in SIGKILL, so chunk serialisation and the
                            * OPFS write path were outside every number this project ever took. It happens
                            * AFTER the measurement window, so it cannot affect the in-game figures. */
                           saveQuit: !has("no-save-quit"),
                           extraArgs: has("verbose-console") ? ["--v=1"] : [] });
  } catch (e) {
    console.log(`  !! run threw: ${e && e.message}`);
    continue;
  }
  const wall = (Date.now() - t0) / 1000;
  const cpu1 = cpuState();
  const cpu = { before: cpu0, after: cpu1,
                throttleDelta: (cpu1.coreThrottle != null && cpu0.coreThrottle != null) ? cpu1.coreThrottle - cpu0.coreThrottle : null };
  console.log(`[mcbench] cpu: ${cpu0.meanMhz} -> ${cpu1.meanMhz} MHz mean, core-throttle events during run: ${cpu.throttleDelta}`);

  console.log(`\n[mcbench] ${name}: ${r.ok ? "COMPLETE" : "INCOMPLETE"} in ${wall.toFixed(0)}s`);
  console.log(formatPhases(r));

  /* A fault does not merely annotate the result — it decides whether the result exists. The three kinds
   * mean different things and mcsr/faults.sh explains each: a game fault is about the runtime, a
   * browser-oom means the browser hit an internal limit and the arm tested nothing, and a GLFW error is
   * the environment refusing a GL context (also nothing tested). Report the kind, never just "failed". */
  if (r.faults.length) {
    const kinds = [...new Set(r.faults.map((f) => f.kind))].join(", ");
    console.log(`  !! faults (${kinds}):`);
    for (const f of r.faults.slice(0, 4)) console.log(`     [${f.kind}] ${f.line}`);
  }
  const w = r.window;
  if (w) {
    console.log(`\n  in-game window: ${(w.wallMs / 1000).toFixed(1)}s  ${w.frames} frames  ${w.fps} fps  ${w.msPerFrame} ms/frame  heap ${w.heapMb} MB`);
    /* Announced before anything else in the window is shown, because everything else in it is then
     * meaningless -- and a void window still prints plausible phase timings and a plausible heap size. */
    if (w.void) console.log(`  !! VOID WINDOW: the frame counter never advanced. The render thread was not running`
      + ` (usually the intermittent worker crash at world-join). Every number from this window, including the`
      + ` JIT counter deltas below, describes a game that was not executing. Re-run.`);
    /* Per-second fps, not just the mean. A mean of 20 hides "55 fps for four seconds, then 2", and on
     * this workload that distinction is usually the entire finding. */
    if (w.fpsPerSecond?.length) console.log(`  fps/s: ${w.fpsPerSecond.join(" ")}`);
    const d = w.deltas || {};
    if (d.invoke) {
      const per = (x) => w.frames > 0 ? (x / w.frames).toFixed(1) : "?";
      console.log(`  wasm-jit over window: registered +${d.registered}  invoke +${d.invoke?.toLocaleString()} (${per(d.invoke)}/frame)`
        + `  residual +${d.residual?.toLocaleString()} (${per(d.residual)}/frame)  fastvcall +${d.fastvcall?.toLocaleString()}`);
      /*
       * THE INLINE FAST PATHS ARE THE DENOMINATOR, and without MONO_WASM_JIT_PROFILE_FAST=1 they are not
       * counted at all. vicHit/vicMiss are bumped inside mono_wasm_jit_vcall_resolve_fslot, i.e. only for
       * calls that already fell out of the inline cache; an inline f-slot hit is pure emitted wasm and calls
       * no helper. So vicMiss/(vicHit+vicMiss) is the miss rate AMONG CALLS THAT ALREADY MISSED, which is
       * guaranteed to look bad and says nothing. Measured that way here it read 44.4%.
       *
       * Print the honest denominator when it is available and refuse to print a rate when it is not.
       */
      const fastKnown = d.fastVic != null && (d.fastVic || d.fastAotic || d.fastInlineAot);
      const vcallTotal = fastKnown ? (d.fastVic + d.fastAotic + d.vicHit + d.vicMiss) : null;
      console.log(`    vcall: ic-helper hit/miss ${d.vicHit?.toLocaleString()}/${d.vicMiss?.toLocaleString()}`
        + (vcallTotal
          ? `   inline f-slot hits ${d.fastVic.toLocaleString()}, inline aot-ic hits ${d.fastAotic.toLocaleString()}`
            + `   => TRUE miss rate ${(100 * d.vicMiss / vcallTotal).toFixed(2)}% of ${vcallTotal.toLocaleString()} vcalls`
          : `   (inline fast-path hits NOT COUNTED — add --knob profile_fast=1; without it no miss RATE is`
            + ` computable, only the helper-side split above)`));
      if (fastKnown) console.log(`    inline direct-aot calls ${d.fastInlineAot.toLocaleString()}  vcall-aot-fast ${d.vcallAotFast?.toLocaleString()}`);
      /* DELEGATE SHARE OF THE IC POOL. The devirt prediction path excludes Delegate.Invoke sites twice over
       * -- `!is_delegate_invoke` gates both the predict call and the emitted monomorphic guard -- and the
       * profile record for a delegate site never accumulates a majority margin at all (WjProfSite.margin is
       * left at 0 by design). So delegate traffic can NEVER take the predicted arm, and if it is a large
       * share of fastVic then "prediction coverage is low" and "delegates dominate dispatch" are the same
       * fact. fastDelegate is the delegate fast path proper; delegateIcHit is its inline-recipe subset. */
      if (d.fastDelegate != null && (d.fastDelegate || d.delegateIcHit)) {
        /* fastDelegate is DISJOINT from the vcallTotal pool, not a subset of it: the delegate recipe
         * check sits BEFORE the receiver-vtable IC ways (mini-wasm.c:11261 vs :11414) and branches
         * straight to its own call_indirect on a hit, so a counted delegate dispatch never reaches the
         * shared hit tail. It therefore has to be ADDED to the denominator, not divided into it --
         * dividing by vcallTotal alone reads >100% as soon as delegates outnumber vtable IC hits. */
        const allDispatch = vcallTotal ? vcallTotal + d.fastDelegate : null;
        console.log(`    delegate: direct ${d.fastDelegate.toLocaleString()}  ic-recipe hits ${d.delegateIcHit.toLocaleString()}`
          + (allDispatch ? `  => ${(100 * d.fastDelegate / allDispatch).toFixed(1)}% of ${allDispatch.toLocaleString()} total dispatches`
             + ` (a LOWER bound on the delegate share: a delegate site whose recipe misses falls through to the`
             + ` vtable IC and is counted in the vcall pool instead)` : ""));
      }
      if (d.callLocal != null && (d.callLocal || d.callImport || d.callIndirect)) {
        const ct = d.callLocal + d.callImport + d.callIndirect;
        console.log(`    call forms emitted: local ${d.callLocal.toLocaleString()} / import ${d.callImport.toLocaleString()}`
          + ` / indirect ${d.callIndirect.toLocaleString()}`
          + (ct ? `  (indirect ${(100 * d.callIndirect / ct).toFixed(1)}%)` : "")
          + `  co-located members ${(d.colocatedMembers || 0).toLocaleString()}`);
      }
      /* CO-LOCATION REACH, AS EXECUTED. callLocal above is the call-FORM half only; R183's differential
       * put the GROUPING half at -7.0% of a -12.8% total (55% of the win) and R182 saw callLocal at 4.0%
       * of calls even at 73% reach, so reading reach off callLocal is what made four Minecraft arms
       * conclude co-location was inert. sibling/(sibling+foreign) is the share of OUR OWN dispatch that
       * is co-resident and therefore trackable by V8's CallIndirectIC; notours is the AOT wall.
       * Lower bound -- see the mirror's note on append-only groups. */
      if (d.vicTgtSibling != null && (d.vicTgtSibling || d.vicTgtForeign || d.vicTgtNotours)) {
        const ours = d.vicTgtSibling + d.vicTgtForeign, all = ours + d.vicTgtNotours;
        console.log(`    colocate reach (IC-publish targets): sibling ${d.vicTgtSibling.toLocaleString()}`
          + ` / foreign ${d.vicTgtForeign.toLocaleString()} / not-ours ${d.vicTgtNotours.toLocaleString()}`
          + (ours ? `  => ${(100 * d.vicTgtSibling / ours).toFixed(2)}% of ours co-resident` : "")
          + (all ? `, ${(100 * d.vicTgtSibling / all).toFixed(2)}% of all` : "")
          + `  [LOWER bound: groups form at publish and never grow]`);
      }
      /* WHY groups were not formed. The identity is asserted here rather than eyeballed: a non-zero
       * residual means a refusal path exists with no counter on it, which is the failure this split was
       * added to stop. dep* are per-callee volumes and are outside the identity by construction. */
      if (d.coTry) {
        const acc = (d.coFormed || 0) + (d.coSelfBatched || 0) + (d.coSelfNoDepset || 0)
          + (d.coSelfRefused || 0) + (d.coSingleton || 0) + (d.sccRefused || 0) + (d.coRebatchFail || 0);
        const bad = d.coTry - acc;
        console.log(`    colocate why: try ${d.coTry.toLocaleString()} = formed ${(d.coFormed||0).toLocaleString()}`
          + ` + self{batched ${(d.coSelfBatched||0).toLocaleString()}, no-depset ${(d.coSelfNoDepset||0).toLocaleString()},`
          + ` refused ${(d.coSelfRefused||0).toLocaleString()}}`
          + ` + singleton ${(d.coSingleton||0).toLocaleString()} + scc ${(d.sccRefused||0).toLocaleString()}`
          + ` + rebatch-fail ${(d.coRebatchFail||0).toLocaleString()}`
          + (bad ? `   *** IDENTITY BROKEN: ${bad.toLocaleString()} unaccounted -- an exit has no counter ***` : `   [identity OK]`));
        console.log(`    colocate per-callee refusals (volumes): unreg ${(d.coDepUnreg||0).toLocaleString()}`
          + ` / no-body ${(d.coDepNobody||0).toLocaleString()} / already-batched ${(d.coDepBatched||0).toLocaleString()}`
          + ` / refused ${(d.coDepRefused||0).toLocaleString()} / byte-cap ${(d.coDepByteCap||0).toLocaleString()}`
          + `  member-cap hits ${(d.coMemberCap||0).toLocaleString()}`);
      }
      console.log(`    below-thresh ${d.vfbThresh?.toLocaleString()} (parked ${d.vfbParked?.toLocaleString()})  perm-unjittable ${d.vfbPerm?.toLocaleString()}`
        + `  aot-routed ${d.aotRouted?.toLocaleString()} / interp-routed ${d.interpRouted?.toLocaleString()}`);
      console.log(`    compile: ${d.compileAttempts} attempts, ${d.islandAttempt}->${d.islandCompleted} islands,`
        + ` ${((d.elapsedGen + d.elapsedInst) / 1000).toFixed(1)}ms, ${(d.bytesGenerated / 1024).toFixed(0)}kb`
        + ``);
      /* CALL-PROFILE HEALTH (mono_wasm_jit_prof_stat; not behind --stats). `evicted` is observations
       * dropped because a caller already had WJ_PROF_MAX_SITES=12 distinct (base, kind) callees, and
       * `jitSamples` is the share arriving from the JITted code's IC miss -- i.e. AFTER the only reader
       * has already run, since nothing re-emits a method on a profile change. */
      /* DEVIRT CENSUS (absolute, emit-time). The two halves answer opposite questions: no_rec+cold say
       * the interpreter never got a long enough look at the site, poly says it did and the site is not
       * perfectly monomorphic. Only the first half is helped by raising the JIT threshold; the second is
       * made strictly worse by it, since one non-majority observation disqualifies a site forever. */
      if (w.devirt) {
        const v = w.devirt, pcs = (x) => v.sites ? `${(100 * x / v.sites).toFixed(1)}%` : "?";
        console.log(`    devirt: ${v.sites.toLocaleString()} virtual sites offered, ${v.emitted.toLocaleString()} predicted (${pcs(v.emitted)})`
          + `  |  no-record ${pcs(v.noRec)}  cold<8 ${pcs(v.cold)}  polymorphic ${pcs(v.poly)}`
          + ` (of which >=90%-monomorphic: ${v.poly90.toLocaleString()})  sig-mismatch ${pcs(v.sig)}  no-fslot ${pcs(v.noFslot)}`);
        console.log(`    devirt: ${v.delegateSites.toLocaleString()} Delegate.Invoke sites emitted -- excluded from prediction by construction`
          + (v.fastDevirt ? `;  predicted arm executed ${v.fastDevirt.toLocaleString()} times` : ""));
        /* MONO_WASM_JIT_DEVIRT_FORCE reach AND price, on one line. `forced` should show up as a fall in
         * no-fslot and a rise in predicted; the island/park numbers are what says whether it was paid for
         * with methods stuck in the interpreter (R153). Printed whenever the knob did anything. */
        if (v.forced || v.forceCapped)
          console.log(`    devirt-force: forced ${v.forced.toLocaleString()} blockers, capped ${v.forceCapped.toLocaleString()}`
            + `  |  cost: parked ${(d.parked || 0).toLocaleString()}  island budget-exhausted ${(d.islandBudget || 0).toLocaleString()}`
            + `  depth-exceeded ${(d.islandDepth || 0).toLocaleString()}`);
      }
      if (w.prof) {
        const p = w.prof, pc = (x, n) => n ? `${(100 * x / n).toFixed(1)}%` : "?";
        console.log(`    call profile: ${p.methods.toLocaleString()} callers, ${p.sites.toLocaleString()} sites,`
          + ` ${p.samples.toLocaleString()} samples (post-JIT ${pc(p.jitSamples, p.samples)}),`
          + ` evicted ${p.evicted.toLocaleString()} (${pc(p.evicted, p.samples + p.evicted)} of offered)`);
      }
    } else if (!w.countersAvailable) {
      console.log(`  wasm-jit counters: UNAVAILABLE — Module._mono_wasm_jit_get_counter is not exported by this build.`
        + ` That is a build problem, not a knob: rebuild the runtime. (Both the export and the MONO_WASM_JIT_STATS`
        + ` env var exist in mini-wasm.c, so a missing export means the served wasm is not what you think it is —`
        + ` see wj deploy.)`);
    } else {
      console.log(`  wasm-jit counters: present but all zero over the window — pass --stats (MONO_WASM_JIT_STATS=1).`
        + ` Note a --stats run is measurably slower (world generation went 91s -> 146s), so read COUNTS from it and`
        + ` TIMINGS from a clean run; never compare the two.`);
    }
  } else if (r.benchSummary.length) {
    console.log("\n" + r.benchSummary.join("\n"));
  }

  /* OPFS BLOCKED TIME. Printed rather than left to jq because it is the answer to a question that stood
   * open for a month (R12 counted ~100k blocking round trips and never converted them to seconds) and
   * because `partsSumToWhole` must be read BEFORE the total is quoted -- a non-zero delta means some op
   * kind is doing round trips with no timer and the total is an underestimate. */
  const ob = r.opfs?.blocked;
  if (ob) {
    const pw = ob.partsSumToWhole;
    console.log(`  opfs blocked: ${ob.total.n.toLocaleString()} round trips, ${(ob.total.ms / 1000).toFixed(2)}s blocked`
      + ` (${(100 * ob.total.ms / 1000 / wall).toFixed(1)}% of the ${wall.toFixed(0)}s run)`
      + (pw.nDelta || pw.msDelta ? `   !! PARTS DO NOT SUM: ${pw.nDelta} ops / ${pw.msDelta}ms untimed — total is a FLOOR` : ""));
    for (const [k, v] of Object.entries(ob.total.kinds).sort((a, b) => b[1].ms - a[1].ms))
      console.log(`      ${k.padEnd(8)} ${String(v.n).padStart(8)} ops  ${(v.ms / 1000).toFixed(2)}s`
        + `  ${v.n ? (v.ms * 1000 / v.n).toFixed(1) : "-"}us/op  ${v.mb.toFixed(1)}MB`
        + `  lat[<${ob.latEdgesUs.join("/<")}/+]=${v.lat.join(",")}`);
    for (const [n, v] of Object.entries(ob.byPhase))
      if (v.n) console.log(`      phase ${n.padEnd(16)} ${String(v.n).padStart(8)} ops  ${(v.ms / 1000).toFixed(2)}s`
        + (v.approx ? `  (approx: the poller starts after titleReady, so this includes the sub-second gap)` : ``));
  } else if (r.opfs) {
    console.log(`  opfs blocked: NOT AVAILABLE (only the coarse 8192-read checkpoints) — the poller could not`
      + ` find Module._wasmfs_opfs_stat, so libwasmfs-mt.a predates the instrumentation. Rebuild the emsdk cache.`);
  }

  /* THE SAVE PHASE. Reported with its OPFS write counters beside it, because "the save took N seconds" and
   * "the save issued N writes totalling M ms of blocked time" are different claims and only the second one
   * says anything about the write path. `flush` is the field to watch: it read 0 for the whole project. */
  const savePh = r.phases.find((p) => p.name === "save");
  if (savePh) {
    const w = r.opfs?.blocked?.byPhase?.save;
    console.log(`  save+quit: ${savePh.secs.toFixed(1)}s`
      + (w ? `  ${w.n.toLocaleString()} OPFS ops, ${(w.ms / 1000).toFixed(2)}s blocked`
             + `  [${Object.entries(w.kinds).map(([k, v]) => `${k} ${v.n}`).join(", ")}]` : ``));
  } else if (!has("no-save-quit")) {
    /* "never attempted" and "attempted and failed" need different responses -- the first means the run
     * ended early (a world-load timeout, a fault) and says nothing about the quit path, the second is a
     * Tab count or a genuinely stuck save. Collapsing them printed a quit diagnosis for a run that timed
     * out at world load 480s before the quit could be reached. */
    if (!r.marks.has("quitStart"))
      console.log(`  save+quit: NOT ATTEMPTED — the run ended before the in-game window closed, so the quit`
        + ` path was never reached. Nothing here says anything about the write path.`);
    else
      console.log(`  save+quit: ATTEMPTED AND DID NOT COMPLETE — see the log for which of the two waits timed out.`
        + ` The run's own in-game numbers are unaffected.`);
  }

  /* JIT ADMISSION, PER PHASE. `splitViolations` first: a non-zero value means one of the two identities
   * failed, i.e. a route through the fallback path has no counter and every share below is wrong. That is
   * R166's failure exactly, and it went unseen for a session because nothing checked it. */
  const ad = r.admit;
  if (ad && !ad.allZero) {
    console.log(`  jit admission: ${ad.polls} polls`
      + (ad.splitViolations
          ? `   !! FINAL-SAMPLE IDENTITY VIOLATION — a route through the fallback path has no counter, so`
            + ` every share below is unreliable (this is R166's failure shape)`
          : `  (identities hold in the final sample)`)
      /* Transient mismatches are torn reads, not missing counters: each counter is fetched with its own
       * call, so a poll can land mid-update. Reported, not alarmed on. */
      + (ad.tornReads ? `  [${ad.tornReads} torn reads, benign]` : ""));
    for (const [n, v] of Object.entries(ad.byPhase)) {
      const notLive = v.vfbNotLive ?? 0;
      console.log(`      ${n.padEnd(16)} ` + Object.entries(v).map(([k, x]) => `${k} ${x.toLocaleString()}`).join("  ")
        + (notLive ? `   <- vfbNotLive NON-ZERO: virtual calls fell to the interpreter on some worker` : ""));
    }
  } else if (ad?.allZero) {
    console.log(`  jit admission: all counters zero — pass --stats (MONO_WASM_JIT_STATS=1). Read COUNTS from a`
      + ` --stats run and TIMINGS from a clean one; never the same run.`);
  }

  /* `frameTime` IS THE PRIMARY IN-GAME METRIC AND IT WAS BEING THROWN AWAY HERE.
   *
   * The window computes a full frame-time distribution and this line used to keep only `frames`, `fps`,
   * `msFrame`, `fpsPerSecond` and `heapMb` -- so `frameTime` reached only 186 of 637 records in
   * mc-results.jsonl and every later A/B had to fall back to mean fps. Measured 2026-09-07 over 51 arms /
   * 187 usable runs, within-arm reproducibility:
   *
   *   metric              p90/p10   trimmed CV   between true replicates
   *   frameTime.p50         1.21        4.5%          0.0-2.8%
   *   fpsTail               1.35        5.7%          --
   *   mean fps              1.38        6.6%          9.0%
   *
   * `perf-ipon-a` vs `-b`, same config 11 minutes apart: p50 moved -2.8% while mean fps moved +9.0%. So
   * mean fps is the metric that manufactures effects, and p50ms is the one to rank arms on. It is lifted to
   * the top level as `p50ms` as well as kept nested, because a jq one-liner over this file is how every A/B
   * actually gets read. */
  const fps = w ? { frames: w.frames, fps: w.fps, msFrame: w.msPerFrame, fpsPerSecond: w.fpsPerSecond,
                    heapMb: w.heapMb, frameTime: w.frameTime ?? null, settle: w.settle ?? null, void: w.void ?? null } : null;

  const row = {
    tag: TAG, run: i, url, knobs, ok: r.ok, wall,
    phases: Object.fromEntries(r.phases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
    subphases: Object.fromEntries(r.subphases.map((p) => [p.name, Number(p.secs.toFixed(2))])),
    /* The two things that decide whether a row is comparable to another row at all. `worldGenMode` because
     * world.generate is bimodal (see mcdrive's classifyWorldGen); `env` because thermal state is worth
     * ~11-12% on every CPU-bound phase and was previously recorded only in perf captures. */
    worldGenMode: r.worldGenMode ?? null,
    env: envSample,
    opfs: r.opfs ?? null,
    admit: r.admit ?? null,
    p50ms: w?.frameTime?.p50 ?? null,
    spawnMs: r.spawnMs, fps, deltas: r.window?.deltas ?? null, prof: r.window?.prof ?? null, devirt: r.window?.devirt ?? null,
    /* `faults` STAYS a string[] of kinds: a month of records and `mcab`'s own row-building use that shape,
     * and changing it in place would leave two incompatible schemas in one file. The detail goes in a new
     * field instead -- `kind` alone lost the near 50/50 split between `function signature mismatch` and
     * `memory access out of bounds`, which have different causes, and lost whether an entry was one event
     * or a burst of lines. */
    deployment, verdict: r.verdict, spin: r.spin ?? null,
    faults: Object.values(r.faultSummary ?? {}).map((f) => `${f.cls}:${f.key}`),
    faultEvents: r.faults.map((f) => ({ kind: f.kind, cls: f.cls, key: f.key, sev: f.sev, lines: f.lines ?? 1,
                                        stack: f.stack?.slice(0, 8) ?? [] })),
    markers: [...r.marks.keys()],
    at: new Date().toISOString(),
  };
  appendFileSync(JSONL, JSON.stringify(row) + "\n");
  writeFileSync(join(OUT, `${name}.json`), JSON.stringify(row, null, 2));
  results.push(row);
}

if (!results.length) { console.log("\nno runs produced a result."); process.exit(1); }

/* Median per phase across runs, and the integrity check that stands in for jbox2d's checksum: every run
 * must have reached the same marker set. A run that stopped at a different marker measured a different
 * script, so folding it into a median silently produces a number for a workload nobody ran. */
if (results.length > 1) {
  console.log(`\n=== ${TAG}: median over ${results.length} run(s) ===`);
  const keys = [...new Set(results.flatMap((r) => Object.keys(r.phases)))];
  for (const k of keys) {
    const xs = results.map((r) => r.phases[k]).filter((x) => x != null).sort((a, b) => a - b);
    if (!xs.length) continue;
    const med = xs.length % 2 ? xs[(xs.length - 1) / 2] : (xs[xs.length / 2 - 1] + xs[xs.length / 2]) / 2;
    console.log(`  ${k.padEnd(10)} ${med.toFixed(1).padStart(8)}s   (min ${xs[0].toFixed(1)}, max ${xs[xs.length - 1].toFixed(1)}, n=${xs.length})`);
  }
  const shapes = new Set(results.map((r) => r.markers.join(",")));
  if (shapes.size > 1)
    console.log(`  !! RUNS DIFFER IN SHAPE — ${shapes.size} distinct marker sets. These runs did not execute the same script; the medians above are not comparable.`);
}
console.log(`\nresults appended to ${JSONL}`);
