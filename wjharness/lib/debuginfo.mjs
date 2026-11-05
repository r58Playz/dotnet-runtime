// Make V8's own frames readable, by wiring Arch's debuginfod into perf's build-id cache.
//
// WHY THIS IS NOT OPTIONAL POLISH
// ------------------------------
// 41% of Minecraft's boot window and a third of world generation execute inside the chromium binary, and
// Arch ships it stripped, so every one of those samples arrives as `[unknown]`. That is not a small gap in
// a corner of the profile: it is the single largest entry, and left anonymous it invites exactly the wrong
// reading. A 44% `[unknown]` line looks like a broken capture, so the natural response is to distrust the
// whole profile — when in fact the samples are perfectly good and simply belong to V8. Once named, the
// first three chromium frames in this capture were `v8::internal::compiler::X64OperandConverter`,
// `InstructionSequence::GetSourcePosition` and `ZoneVector<int>::resize`, i.e. TurboFan compiling wasm.
// That turns "44% unknown" into "boot is dominated by V8 optimizing our code", which is a finding.
//
// WHY perf's OWN debuginfod SUPPORT IS NOT ENOUGH
// ----------------------------------------------
// This perf reports `debuginfod: [ on ]`, the build-id in the capture matches the installed binary, the
// debuginfo is in ~/.cache/debuginfod_client, and `perf script` still printed `[unknown]` for every
// chromium frame with DEBUGINFOD_URLS set. So the fetch has to be done here and the result placed where
// perf looks unconditionally: its build-id cache.
//
// Layout, established by inspecting what `perf buildid-cache --add` produces:
//   ~/.debug/<dso path without leading />/<build-id>/{elf,debug}
//   ~/.debug/.build-id/<first 2 hex>/<remaining 38 hex>  ->  symlink to that directory
// `debug` ALONE is sufficient for symbolisation — verified by moving the 320 MB `elf` aside and re-running
// `perf script`, which still named every frame. So this never downloads the executable, only the debuginfo.
//
// THE BUILD-ID MUST MATCH THE CAPTURE
// ----------------------------------
// debuginfod serves by build-id, and a distro only keeps a window of versions. cap-base was recorded
// against chromium 151.0.7922.75 and Arch already 404s that build-id, so it stays anonymous no matter what
// is installed now; the capture taken minutes later against 151.0.7922.108 symbolises completely. Hence the
// rule: profile with the chromium you can still fetch symbols for, and if a capture matters, fetch them
// before the next upgrade.
import { execFileSync, execSync } from "child_process";
import { existsSync, mkdirSync, copyFileSync, symlinkSync, rmSync, statSync } from "fs";
import { join } from "path";

const HOME = process.env.HOME;
const CACHE = join(HOME, ".debug");
export const DEBUGINFOD_URLS = process.env.DEBUGINFOD_URLS || "https://debuginfod.archlinux.org/";

/** (build-id, dso path) pairs recorded in a capture. */
export function captureBuildIds (capture) {
  try {
    const out = execFileSync("perf", ["buildid-list", "-i", capture], { encoding: "utf8", maxBuffer: 1 << 26 });
    return out.split("\n").map((l) => l.match(/^([0-9a-f]{40})\s+(\S.*)$/)).filter(Boolean)
      .map((m) => ({ id: m[1], dso: m[2].trim() }));
  } catch { return []; }
}

/**
 * Ensure perf can symbolise `dso` at `id`. Returns one of:
 *   "cached"     already present
 *   "fetched"    downloaded and installed
 *   "unavailable" the server has no debuginfo for this build-id (usually: the package was upgraded)
 *   "skipped"    not a DSO worth fetching
 */
export function ensureDebuginfo (id, dso, { log = () => { } } = {}) {
  /* Only real files on disk. /tmp/perf-<pid>.map is V8's own symbol map (already authoritative), and
   * pseudo-DSOs like [vdso] have nothing to fetch. */
  if (!dso.startsWith("/") || /perf-\d+\.map$/.test(dso)) return "skipped";

  const dir = join(CACHE, dso.replace(/^\//, ""), id);
  const debug = join(dir, "debug");
  if (existsSync(debug) && statSync(debug).size > 0) return "cached";

  let src;
  try {
    src = execSync(`debuginfod-find debuginfo ${id} 2>/dev/null`,
      { encoding: "utf8", env: { ...process.env, DEBUGINFOD_URLS }, timeout: 600000 }).trim();
  } catch { return "unavailable"; }
  if (!src || !existsSync(src)) return "unavailable";

  mkdirSync(dir, { recursive: true });
  copyFileSync(src, debug);
  /* The .build-id symlink is how perf finds the cache entry when it has a build-id but no path match. Its
   * target is relative to the .build-id/<xx>/ directory, mirroring what buildid-cache writes. */
  const bidDir = join(CACHE, ".build-id", id.slice(0, 2));
  const link = join(bidDir, id.slice(2));
  mkdirSync(bidDir, { recursive: true });
  try { rmSync(link, { force: true }); symlinkSync(join("../..", dso.replace(/^\//, ""), id), link); } catch { }
  log(`  fetched debuginfo for ${dso} (${(statSync(debug).size / 1048576).toFixed(0)} MB)`);
  return "fetched";
}

/**
 * Do it for every DSO in a capture that is worth symbolising. Chromium is the one that matters; libc and
 * friends are cheap and occasionally useful (musl's a_cas / __pthread_mutex_timedlock show up hot).
 */
export function ensureCaptureSymbols (capture, { log = console.log, only = /chromium|chrome|libc|libstdc|libm/ } = {}) {
  const ids = captureBuildIds(capture).filter((x) => only.test(x.dso));
  if (!ids.length) return;
  const missing = [];
  for (const { id, dso } of ids) {
    const r = ensureDebuginfo(id, dso, { log });
    if (r === "unavailable") missing.push({ id, dso });
  }
  for (const { id, dso } of missing)
    log(`  no debuginfo available for ${dso} (build-id ${id.slice(0, 12)}) — it was probably upgraded since this`
      + `\n    capture; those frames stay anonymous. Re-capture against the installed binary to name them.`);
}
