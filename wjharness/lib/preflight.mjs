// "Is this machine fit to measure on?" — shared by bench.mjs (jbox2d) and mcbench/mcperf (Minecraft).
//
// Lifted verbatim out of bench.mjs so the Minecraft harness cannot drift from the jbox2d one. Every
// check below individually produced a measurement error LARGER than most of the effects these harnesses
// are used to measure, and each cost real time to diagnose after the fact:
//
//   - A leftover Roslyn VBCSCompiler from a build sat at 104% CPU for 12 minutes and made three
//     consecutive readings 9-15% slow, including a control that should have reproduced a known number.
//     /proc/loadavg's 1-minute figure looked fine the whole time.
//   - /tmp is tmpfs, i.e. RAM. 2.4 GB of cdpperf debris left there put the machine into reclaim
//     (kswapd running) and cost ~9% with no code change at all.
//   - Sustained benching takes this package 62 -> 83 C and drifts ~6% across a sequential sweep.
//
// So refuse to measure into a dirty environment rather than produce a number that has to be re-litigated
// later. `force` downgrades the refusal to a warning.
//
// MINECRAFT ADDITIONS (a full run is ~7 minutes, so a bad environment costs far more here than it does
// on a 30-second jbox2d bench):
//   - Other vite dev servers. This box routinely has three or four node/vite processes from unrelated
//     projects alive; they are idle most of the time so the pcpu check misses them, but an HMR rebuild
//     mid-run lands squarely inside a phase window.
//   - Free memory. The AOT runtime is a 188 MB wasm and the renderer peaks over 4 GB. Starting a run
//     with under ~5 GB available means the measurement races systemd-oomd, and an oomd kill is
//     indistinguishable from the renderer crash the mcsr harness exists to study (see faults.sh).
//   - Leftover chromium from a previous run. Each one holds a 16-thread pthread pool and a GL context;
//     browser.mjs documents that extra tabs moved even the jbox2d teavm/IKVM *ratio* from 4x to 5.4x.
import { execSync } from "child_process";
import { readFileSync, statfsSync } from "fs";
import { serveUrl, cfg } from "./paths.mjs";

/**
 * Instantaneous per-process CPU, as a map comm -> %CPU (100 = one core).
 *
 * NOT `ps -eo pcpu`, which averages over the process's ENTIRE LIFETIME. For anything running since boot
 * -- kwin, kswapd, plasmashell -- that average is dominated by load that has already finished, and in a
 * measuring session it is actively self-defeating: every AOT build and every one of the ~20 browser
 * instances this harness starts raises those averages, so the gate becomes MORE likely to refuse the
 * longer you measure. Observed 2026-08-27: the gate refused three A/B attempts citing
 * `kwin_wayland 5.9%` and `kswapd 4.0%` while the true instantaneous figures were 2.5-3.0% and 0.0%.
 *
 * `top -b -n 2` throws away the first (lifetime) sample and computes a real delta for the second, which
 * is what we parse. Falls back to `ps` if top is unavailable -- with the old semantics, but a stale
 * average is better than no check at all.
 */
function cpuByComm (intervalSec = 0.5) {
  try {
    const out = execSync(`top -b -n 2 -d ${intervalSec} -w 512`, { encoding: "utf8" });
    const tables = out.split(/^\s*PID\s+USER/m);
    const last = tables[tables.length - 1];          // second iteration = true delta
    const map = new Map();
    for (const line of last.split("\n")) {
      const f = line.trim().split(/\s+/);
      if (f.length < 12) continue;
      const pct = parseFloat(f[8]), comm = f[11];
      if (!Number.isFinite(pct) || !comm) continue;
      map.set(comm, Math.max(map.get(comm) ?? 0, pct));
    }
    if (map.size) return map;
  } catch { }
  const map = new Map();
  try {
    const ps = execSync("ps -eo pcpu,comm --sort=-pcpu | head -20", { encoding: "utf8" });
    for (const line of ps.split("\n").slice(1)) {
      const m = line.trim().match(/^([\d.]+)\s+(\S+)/);
      if (m) map.set(m[2], Math.max(map.get(m[2]) ?? 0, parseFloat(m[1])));
    }
  } catch { }
  return map;
}

/**
 * @param {{ force?: boolean, minFreeGb?: number, devServer?: boolean, log?: (s: string) => void }} opts
 *   devServer: false for a harness that serves its own pages (mbench), which needs no ikvmcraft dev server.
 * @returns {{ problems: string[], warnings: string[] }} — also exits(2) unless `force`.
 */
/*
 * A CHEAP, UNGATED SNAPSHOT OF THE VOLATILE MACHINE STATE, for recording alongside each run.
 *
 * `preflight()` runs ONCE per invocation and its job is to REFUSE; these are the three quantities that
 * move between the runs of a single matrix, and until now none of them was written down anywhere a
 * later analysis could reach. `perf-<tag>/phases.json` records preflight's `warnings` and
 * `mc-results.jsonl` recorded nothing at all -- so the fact that thermal state is worth ~11-12% on every
 * CPU-bound phase (2026-09-07: HOT n=10 vs COOL n=31, boot +11.6%, boot.classload +11.4%,
 * world.gen +17.6%) was unrecoverable from the results file, and day-to-day `boot` drift of 25-41% on
 * byte-identical configs had no covariate to explain it.
 *
 * Deliberately silent and deliberately never refusing: a sampler that can exit is a sampler nobody calls
 * from inside a loop. Every field degrades to null rather than throwing -- a missing thermal zone must not
 * cost a run.
 */
export function sampleEnv () {
  const out = { pkgTempC: null, memAvailGb: null, load1: null, at: new Date().toISOString() };
  try {
    for (const z of execSync("ls -d /sys/class/thermal/thermal_zone*/", { encoding: "utf8" }).trim().split("\n")) {
      if (readFileSync(z + "type", "utf8").trim() !== "x86_pkg_temp") continue;
      out.pkgTempC = Number((parseInt(readFileSync(z + "temp", "utf8"), 10) / 1000).toFixed(1));
    }
  } catch { }
  try {
    const kb = Number(readFileSync("/proc/meminfo", "utf8").match(/^MemAvailable:\s+(\d+) kB$/m)?.[1] || 0);
    if (kb) out.memAvailGb = Number((kb / 1024 / 1024).toFixed(2));
  } catch { }
  try { out.load1 = Number(readFileSync("/proc/loadavg", "utf8").split(" ")[0]); } catch { }
  return out;
}

export function preflight ({ force = false, minFreeGb = 5, devServer = true, log = console.log } = {}) {
  const problems = [], warnings = [];

  /* IS ANYTHING ACTUALLY SERVING THE APP?
   *
   * A dead dev server is indistinguishable from a runtime hang through the harness's watchdog: the browser
   * loads nothing, the app logs nothing, and mcbench reports "STALLED: no log output for 90s" exactly as it
   * does for a real fault. One arm on 2026-09-01 produced a 915-byte log containing only chromium's own
   * gcm errors, and was nearly diagnosed as a JIT stall.
   *
   * deploy.sh does NOT cover this -- it proves markers in the file ON DISK, so it reports `marker OK` while
   * nothing is listening. It even warned ("nothing restarted: no active unit and nothing listening on
   * 5021") and the run went ahead anyway. One HTTP request costs nothing and removes the whole class. */
  if (devServer) try {
    const code = execSync(`curl -sk -o /dev/null -w '%{http_code}' ${serveUrl()} 2>/dev/null || echo 000`,
      { encoding: "utf8" }).trim();
    if (code !== "200")
      problems.push(`the app is not being served (${serveUrl()} -> HTTP ${code})`
        + ` -- start the dev server: wj ensure-server (unit ${cfg("DEV_UNIT")})`);
  } catch {
    problems.push(`could not probe ${serveUrl()} -- is the dev server up?`);
  }


  // Anything but the browser eating CPU. The 1-minute loadavg is not sufficient -- check processes.
  try {
    /* THRESHOLD IS PER-CORE, AND THIS BOX HAS 16 OF THEM for a workload that saturates 2 (R117: server
     * tick + client render, everything else under 5%). A process at 5% of one core is not competing with
     * anything -- there are fourteen idle cores for the scheduler to put it on. The old 5% gate came from
     * the jbox2d harness and it refuses on desktop noise: kwin compositing, an ACPI IRQ thread. What
     * actually costs a measurement is something taking a WHOLE core away, like the 104% Roslyn server this
     * file's header describes. Flag at half a core. */
    for (const [cmd, pct] of cpuByComm()) {
      if (pct < 50) continue;
      // us, or the browser under test. `top` truncates COMMAND, so match on a prefix.
      if (/^(chrom|chrome|node|claude|bench|top$|ps$)/.test(cmd)) continue;
      problems.push(`${cmd} is using ${pct.toFixed(1)}% CPU (instantaneous)`);
    }
  } catch { }

  // Build servers specifically: they linger after a build and are easy to miss.
  //
  // The bracket is load-bearing: `pgrep -f 'VBCS'` MATCHES ITS OWN `sh -c` COMMAND LINE, so it always
  // reported at least one build server and the preflight refused every clean machine. '[V]BCS' cannot
  // match the literal string '[V]BCS' in the invoking command line, so only real processes count.
  try {
    const bs = execSync("pgrep -c -f '[V]BCSCompiler|[M]SBuild.dll' 2>/dev/null || true", { encoding: "utf8" }).trim();
    if (bs && bs !== "0") problems.push(`${bs} Roslyn/MSBuild server(s) alive -- run: dotnet build-server shutdown`);
  } catch { }

  // Long-lived JVMs: gradle daemons from a teavm rebuild sat at 5% for an hour. `ps` above catches them
  // only while they are busy, and a daemon that is idle NOW may wake mid-measurement.
  try {
    const j = execSync("ps -eo etimes,comm | awk '$2==\"java\"{print $1}'", { encoding: "utf8" }).trim();
    const old = j.split("\n").filter((x) => Number(x) > 120).length;
    if (old) warnings.push(`${old} long-lived java process(es) (gradle daemon? run: gradlew --stop)`);
  } catch { }

  /* tmpfs pressure. /tmp being RAM makes a full /tmp a memory problem, not a disk one.
   *
   * THE PERCENTAGE IS THE WRONG UNIT, and this gate proved it by passing. R280f: two consecutive arms
   * died with "no log output at all for 90s while waiting for titleReady ... PreInit never entered",
   * i.e. at PAGE LOAD, before any app code -- so it read as a browser bug or a flaky deploy, and both
   * failures were nearly attributed to the constructor change under test. The cause was 100 stale
   * `/tmp/perf-*.map` at 230-270 MB each = 4.9 GiB of a 7.7 G tmpfs. That is 64% full: UNDER this
   * gate's 70% threshold, so it said nothing.
   *
   * What a run actually needs is absolute: mcdrive reflink-copies the ~697 MB seed profile, chromium
   * wants its own scratch, and a --perf-basic-prof capture adds a fresh ~240 MB map. Gate on free
   * BYTES against that, and keep the percentage as a warning for the case where the disk is small. */
  try {
    const st = statfsSync("/tmp");
    const freeGb = (st.bfree * st.bsize) / 1e9;
    const usedPct = 100 * (1 - st.bfree / st.blocks);
    const maps = execSync("ls /tmp/perf-*.map 2>/dev/null | wc -l", { encoding: "utf8" }).trim();
    const hint = Number(maps) > 0
      ? ` -- ${maps} stale perf symbol map(s) are there; run \`wj reap --apply\``
      : "";
    if (freeGb < 2)
      problems.push(`/tmp (tmpfs, RAM-backed) has only ${freeGb.toFixed(1)} GB free`
        + ` -- a run needs ~1 GB for the seed profile copy alone and will die at PAGE LOAD without it${hint}`);
    else if (usedPct > 70)
      warnings.push(`/tmp is ${usedPct.toFixed(0)}% full (${freeGb.toFixed(1)} GB free)${hint}`);
  } catch { }

  /* Capture space. A perf capture is ~1.3 GB of perf.data plus a ~240 MB map; a jitdump run is far more.
   * Running out mid-capture produces a truncated perf.data that still parses, which is worse than a
   * refusal. Warning rather than a refusal because a counter-only batch needs almost nothing. */
  try {
    const st = statfsSync(process.cwd());
    const freeGb = (st.bavail * st.bsize) / 1e9;
    if (freeGb < 10)
      warnings.push(`only ${freeGb.toFixed(0)} GB free on the capture filesystem`
        + ` -- a perf capture is ~1.5 GB; run \`wj reap\` to reclaim`);
  } catch { }

  // Reclaim activity means the benchmark's nursery is fighting for pages.
  try {
    let ksw = 0;
    for (const [cmd, pct] of cpuByComm()) if (/^kswapd/.test(cmd)) ksw = Math.max(ksw, pct);
    if (ksw > 1) warnings.push(`kswapd at ${ksw.toFixed(1)}% -- memory reclaim is active`);
  } catch { }

  // Mains power. On battery the platform drops to a power-saving profile and the run stalls in ways no other check
  // here sees: R454's base mapping run fell inside a 19:52-20:07 dock unplug (journal) and came back with a slow boot,
  // a worker stall and an in-game window whose frame counter never moved (ingameVoid). Refuse rather than warn.
  try {
    const ps = "/sys/class/power_supply/";
    const mains = execSync(`ls ${ps}`, { encoding: "utf8" }).trim().split("\n")
      .filter((d) => { try { return readFileSync(ps + d + "/type", "utf8").trim() === "Mains"; } catch { return false; } });
    if (mains.length && !mains.some((d) => { try { return readFileSync(ps + d + "/online", "utf8").trim() === "1"; } catch { return false; } }))
      problems.push(`running on battery (${mains.join(", ")} offline) -- plug in; a power-saving profile skews every arm`);
    try {
      const prof = execSync("powerprofilesctl get 2>/dev/null", { encoding: "utf8" }).trim();
      if (prof && prof !== "performance") warnings.push(`power profile is '${prof}', not 'performance'`);
    } catch { }
  } catch { }

  // Thermal: informational, since interleaving is the real defence.
  try {
    for (const z of execSync("ls -d /sys/class/thermal/thermal_zone*/", { encoding: "utf8" }).trim().split("\n")) {
      if (readFileSync(z + "type", "utf8").trim() !== "x86_pkg_temp") continue;
      const c = parseInt(readFileSync(z + "temp", "utf8"), 10) / 1000;
      if (c > 75) warnings.push(`package is ${c.toFixed(0)}C -- expect downclocking; interleave A/B or let it cool`);
    }
  } catch { }

  /* --- Minecraft-run additions ------------------------------------------------------------------ */

  // MemAvailable, not MemFree: the renderer's growth comes out of reclaimable cache as happily as out of
  // free pages, and "free" alone reads alarmingly low on a box with a warm page cache.
  try {
    const mi = readFileSync("/proc/meminfo", "utf8");
    const availKb = Number(mi.match(/^MemAvailable:\s+(\d+) kB$/m)?.[1] || 0);
    const availGb = availKb / 1024 / 1024;
    if (availGb && availGb < minFreeGb)
      problems.push(`only ${availGb.toFixed(1)} GB available (want >= ${minFreeGb}) -- the run will race systemd-oomd,`
        + ` and an oomd kill looks exactly like the renderer crash under study`);
  } catch { }

  // Leftover browsers from a previous run or from mcsr. Each holds a 16-thread pool; contention is
  // indistinguishable from a hang (browser.mjs: a run declared deadlocked for six minutes finished the
  // instant the other tabs were closed).
  try {
    /* HARNESS browsers only: every harness profile lives under $WJ_HOME/runs/ (lib/paths.mjs) or is a short-lived
     * /tmp/wj-* (lib/browser.mjs). The [r]/[w] brackets keep the pattern from matching the shell that runs pgrep.
     *
     * HARNESS browsers only. This used to match any `--user-data-dir`, which also catches every Electron
     * app the developer happens to be running -- obsidian, vesktop, a YouTube PWA -- and those are not
     * ours to kill. Ten of them blocked a run in 2026-09 with ZERO harness browsers alive, and the advice
     * it printed (`pkill -f 'chrome.*user-data-dir'`) would have killed the developer's editor and chat.
     * CLAUDE.md already records a sweep that SIGKILLed vesktop and obsidian; do not let preflight advise
     * a repeat of it.
     *
     * A harness browser is identifiable by its profile path, and that is also the only kind the caller can
     * actually do anything about. */
    const n = Number(execSync("pgrep -c -f 'user-data-dir=([^ ]*/[r]uns/|/tmp/[w]j-)' 2>/dev/null || echo 0", { encoding: "utf8" }).trim());
    if (n > 0) problems.push(`${n} harness browser(s) from a previous run still alive`
      /* DO NOT put a paste-able `pkill -f` here. `pgrep -f`/`pkill -f` MATCH THE SEARCHER: the pattern
       * sits in the invoking shell's own argv, so a pasted `pkill -f 'user-data-dir=..runs/'`
       * can kill the shell that ran it. That has cost three incidents in this tree, one of which took a
       * build with it. killdaemons.sh has the safe shape (match, filter by `ps -o comm=`, never self). */
      + ` -- kill them: \`wj reap\` lists them`);
  } catch { }

  /* The developer's own Electron/Chrome apps. Not ours to kill, but they DO tax a measurement -- an app at
   * 62% of a core invalidated an arm here -- so surface them as a warning the reader can weigh. */
  try {
    const n = Number(execSync("pgrep -c -f -- '--user-data-dir' 2>/dev/null || echo 0", { encoding: "utf8" }).trim());
    const mine = Number(execSync("pgrep -c -f 'user-data-dir=([^ ]*/[r]uns/|/tmp/[w]j-)' 2>/dev/null || echo 0", { encoding: "utf8" }).trim());
    if (n - mine > 0) warnings.push(`${n - mine} browser/Electron process(es) of your own are running`
      + ` -- they compete for CPU on this 28 W part; results will be noisier than a quiet box`);
  } catch { }

  // Unrelated dev servers. Idle now, rebuilding mid-window later.
  try {
    const out = execSync("pgrep -af '[v]ite' 2>/dev/null || true", { encoding: "utf8" }).trim();
    const others = out ? out.split("\n").filter((l) => !/ikvmcraft/.test(l)) : [];
    if (others.length) warnings.push(`${others.length} vite dev server(s) unrelated to ikvmcraft are running`
      + ` -- an HMR rebuild during a phase window lands inside the measurement`);
  } catch { }

  for (const w of warnings) log(`  warn: ${w}`);
  if (!problems.length) return { problems, warnings };
  for (const p of problems) log(`  !! ${p}`);
  if (force) { log("  (--force given, measuring anyway)"); return { problems, warnings }; }
  log("\nrefusing to measure. fix the above, or pass --force.");
  process.exit(2);
}
