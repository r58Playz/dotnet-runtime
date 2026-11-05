// Wait for a THERMAL/CLOCK CONDITION, not for a fixed number of seconds.
//
// mcab's cooldown was `setTimeout(240s)`. That waits for time, which is not the thing that matters: this
// box's package swings 400 MHz .. 4.7 GHz (observed mean 2.2 GHz at 64 C), so a fixed sleep starts each
// arm at whatever frequency the governor happens to be holding. Measured consequence, 2026-08-29
// direct_import A/B: the `noimp` arm read 21.57, 20.77 and then 14.22 fps -- a 35% self-spread that no
// number of rounds can average away, and far outside R149's 11.5% median.
//
// So gate on the package temperature instead, and report what the gate actually saw. Cool boxes stop
// waiting early (the old 240 s was pure cost when already cold); hot boxes wait longer than 240 s, which
// is exactly when the old fixed wait was too short.
import { readFileSync, existsSync, readdirSync } from "fs";

const ZONES = (() => {
  try {
    return readdirSync("/sys/class/thermal")
      .filter((d) => d.startsWith("thermal_zone"))
      .map((d) => `/sys/class/thermal/${d}/`)
      .filter((z) => { try { return readFileSync(z + "type", "utf8").trim() === "x86_pkg_temp"; } catch { return false; } });
  } catch { return []; }
})();

export function pkgTempC () {
  for (const z of ZONES) { try { return parseInt(readFileSync(z + "temp", "utf8"), 10) / 1000; } catch {} }
  return null;   // no package sensor: callers fall back to a fixed wait
}

/**
 * Frequency of the BUSIEST cores, MHz -- {max, topMean} over the `n` fastest right now.
 *
 * NOT the mean over all CPUs. This box is 8 P-cores (400-5000 MHz) + 8 E-cores (400-3700), and the
 * workload saturates about two threads (server tick + client render, R117), so twelve idle cores parked
 * at their 400 MHz floor drag an all-core mean to a number that describes nothing. Observed 2026-08-29:
 * all-core mean 1596 MHz while CPU4 was at 4200 and CPU6 at 4014 -- the machine was boosting fine on the
 * cores that mattered and the mean called it throttled. `n = 2` matches the thread count that is actually
 * busy; `max` is the ceiling the package is currently allowing.
 */
export function busyFreqMhz (n = 2) {
  const v = allFreqsMhz ();
  if (!v.length) return { max: null, topMean: null };
  v.sort((a, b) => b - a);
  const top = v.slice(0, Math.min(n, v.length));
  return { max: v[0], topMean: top.reduce((a, b) => a + b, 0) / top.length };
}

function allFreqsMhz () {
  try {
    const base = "/sys/devices/system/cpu";
    const cpus = readdirSync(base).filter((d) => /^cpu\d+$/.test(d));
    const v = [];
    for (const c of cpus) {
      const f = `${base}/${c}/cpufreq/scaling_cur_freq`;
      if (existsSync(f)) { const k = parseInt(readFileSync(f, "utf8"), 10); if (k > 0) v.push(k / 1000); }
    }
    return v;
  } catch { return []; }
}

/**
 * Hold until the package is at or below `targetC`, then return what the gate observed.
 * `maxMs` bounds the wait; `minMs` still applies so a run never starts on the previous arm's tail.
 */
export async function coolTo ({ targetC = 80, minMs = 15000, maxMs = 180000, log = console.log } = {}) {
  /* WHY 80 AND NOT 55. This is an i7-1360P (Raptor Lake): Tjmax is ~100 C and this machine reports PL1/PL2
   * at 256 W, i.e. effectively unlimited, so temperature is the binding constraint and it only binds near
   * the top. Measured on this box: 100 C with the busiest cores still at 4462 MHz. And the workload drives
   * the package to ~95 C within its own warm-up no matter what it started at -- so pre-cooling to 55 C
   * buys a different starting point on a ramp that ends in the same place, at the cost of minutes per arm.
   * What is worth preventing is only arm B starting while still saturated from arm A. 80 C does that. */
  const t0 = Date.now();
  const start = pkgTempC();
  if (start === null) {                       // no sensor -- degrade to the old behaviour, and say so
    log(`[thermal] no x86_pkg_temp sensor; falling back to a fixed ${(maxMs / 1000) | 0}s wait`);
    await new Promise((r) => setTimeout(r, maxMs));
    return { start: null, end: null, waitedMs: Date.now() - t0, reachedTarget: false };
  }
  for (;;) {
    const el = Date.now() - t0, t = pkgTempC();
    if (el >= maxMs) { log(`[thermal] gave up at ${t.toFixed(0)}C after ${(el / 1000) | 0}s (target ${targetC}C)`); break; }
    if (t <= targetC && el >= minMs) break;
    await new Promise((r) => setTimeout(r, 2000));
  }
  const end = pkgTempC(), f = busyFreqMhz();
  log(`[thermal] ${start.toFixed(0)}C -> ${end.toFixed(0)}C in ${((Date.now() - t0) / 1000) | 0}s`
    + `, busiest cores ${f.max ? f.max.toFixed(0) : "?"} / ${f.topMean ? f.topMean.toFixed(0) : "?"} MHz`
    + ` (target <= ${targetC}C)`);
  return { start, end, waitedMs: Date.now() - t0, reachedTarget: end <= targetC, maxMhz: f.max, topMeanMhz: f.topMean };
}
