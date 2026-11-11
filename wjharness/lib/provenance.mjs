// Record WHICH BYTES a measurement was taken against — including IKVM's, which nothing else checks.
//
// mcsr/deploy.sh exists because a whole measurement matrix once ran on a stale mono runtime, and it solves
// that by grepping the served `dotnet.native.<hash>.wasm` for string markers the change introduced. It says
// nothing at all about the MANAGED assemblies, and IKVM is where a large share of this workload's cost lives:
// `IKVM.Runtime.<hash>.dll` is rebuilt from a separate repo
// (ikvm-wasm-build/tools/ikvm/ikvm) that routinely carries uncommitted work.
//
// That gap is not theoretical. This session's first findings write-up listed "Java type checks go through
// reflection, 4.6% of frame time" as an open problem, while the IKVM checkout had an uncommitted diff adding
// a per-call-site cache for exactly those checks. Confirming whether the measured build contained it took a
// hand-rolled `grep -a DynamicInstanceOfResolve` over the served DLL. It did contain it, and it was working —
// but "the finding is already fixed and you measured the fix" and "the finding is real" are opposite
// conclusions, and no artefact in the capture could distinguish them.
//
// So every capture now stamps: the served runtime wasm and managed assemblies (name, size, mtime), and
// whether a caller-supplied list of marker strings is present in them. Markers are matched as raw bytes, so
// for a managed assembly a method or type NAME works (metadata strings survive), whereas for
// dotnet.runtime.<hash>.js only string literals do — identifiers there are minified away, which cost
// deploy.sh two round trips to learn.
import { readFileSync, readdirSync, statSync, existsSync } from "fs";
import { join } from "path";

import { need } from "./paths.mjs";
const APP = need("IKVMCRAFT", "provenance reads the deployed _framework");
const FW = join(APP, "frontend", "public", "_framework");

/** Files worth naming in a capture: the runtime, and the managed assemblies most likely to be stale. */
const OF_INTEREST = [/^dotnet\.native\.[^.]+\.wasm$/, /^dotnet\.runtime\.[^.]+\.js$/,
  /^IKVM\.Runtime\.[^.]+\.(dll|wasm)$/, /^IKVM\.Java\.[^.]+\.(dll|wasm)$/, /^IkvmWasm\.[^.]+\.(dll|wasm)$/,   // .wasm: webcil
  /* dotnet.native.<hash>.js and the worker module carry everything emscripten's JS libraries contribute at
   * LINK time -- library_pthread.js, the GL bindings, WASMFS. A change to the emsdk under
   * ikvmcraft/statics/emsdk lands ONLY here, in neither the wasm nor dotnet.runtime.js, so deploy.sh's marker
   * check cannot see it and would report a correctly-deployed emsdk patch as MISSING. */
  /^dotnet\.native\.[^.]+\.js$/, /^dotnet\.native\.worker\.[^.]+\.m?js$/];

/* deploy.sh ends with ikvmcraft's split-resources.py (R521): a resource over 20 MiB -- the runtime wasm, IKVM.Java,
 * the big ikvmc DLLs -- is served as <name>0..<name>N-1 and listed with its whole size in split-resources.json, and
 * <name> itself is gone. Reading only whole files made every wasm marker read MISSING against a correct deploy
 * (2026-11-07: mcperf refused `MONO_GC_FIN_PIN, default 1`, present in the reassembled bytes; deploy.sh checks
 * before it splits, so it never saw this). A reassembly whose size disagrees with the manifest is an error, not a
 * file: a marker search over a torn resource proves nothing. */
function splitManifest () {
  const p = join(FW, "split-resources.json");
  if (!existsSync(p)) return {};
  try { return JSON.parse(readFileSync(p, "utf8")); } catch { return {}; }
}
function partNames (f, s) { return Array.from({ length: s.parts }, (_, i) => f + i); }
function readServed (f, split) {
  const s = split[f];
  if (!s) return readFileSync(join(FW, f));
  const buf = Buffer.concat(partNames(f, s).map((n) => readFileSync(join(FW, n))));
  if (buf.length !== s.size) throw new Error(`${f}: ${buf.length} bytes in ${s.parts} parts, manifest says ${s.size}`);
  return buf;
}

/**
 * @param {{ markers?: string[] }} opts  marker strings to look for, e.g. ["DynamicInstanceOfResolve"]
 * @returns {{ files: object[], markers: object, framework: string }|{error: string}}
 */
export function describeDeployment ({ markers = [] } = {}) {
  if (!existsSync(FW)) return { error: `no served _framework at ${FW}` };
  const split = splitManifest();
  const want = (f) => OF_INTEREST.some((rx) => rx.test(f));
  const names = readdirSync(FW).filter(want);
  for (const f of Object.keys(split)) if (want(f) && !names.includes(f)) names.push(f);
  const files = names.map((f) => {
    const s = split[f];
    const st = statSync(join(FW, s ? f + "0" : f));
    /* LOCAL time, deliberately. Every other timestamp a user compares this against -- chromium's log lines,
     * `ls -la` on the capture -- is local, and a UTC string here reads as a deploy seven hours in the future,
     * which is exactly the kind of confusion that makes someone doubt correct provenance. */
    const t = new Date(st.mtimeMs - st.mtimeMs % 1000);
    const local = new Date(t.getTime() - t.getTimezoneOffset() * 60000).toISOString().replace("T", " ").slice(0, 19);
    if (!s) return { name: f, mb: Number((st.size / 1048576).toFixed(2)), mtime: local };
    const onDisk = partNames(f, s).reduce((n, p) => n + (existsSync(join(FW, p)) ? statSync(join(FW, p)).size : 0), 0);
    return { name: f, mb: Number((s.size / 1048576).toFixed(2)), mtime: local, parts: s.parts,
      ...(onDisk !== s.size ? { error: `${onDisk} bytes in parts, manifest says ${s.size}` } : {}) };
  }).sort((a, b) => b.mb - a.mb);

  /* Marker search over the whole set rather than a named file: a change can land in the wasm, in the runtime
   * JS, or in a managed DLL, and requiring the caller to know which is how a correct deployment gets reported
   * as MISSING. Report WHERE each marker was found instead. */
  /* Managed assemblies store the two kinds of string in DIFFERENT encodings, and searching for only one of them
   * reports a correctly-deployed change as MISSING -- the exact failure this module exists to prevent.
   *
   *   #Strings heap (type/member/param NAMES)      UTF-8   -> a plain byte search finds it
   *   #US heap (string LITERALS in the IL)         UTF-16  -> a plain byte search does NOT
   *
   * That bit: verifying a fresh IKVM build for the env-var knobs `IKVM_BINDER_SHARE_THUNKS` etc. Every one read
   * as absent from a dll that in fact contained all three, because they are literals passed to
   * GetEnvironmentVariable. Method names in the same dll matched fine, which made the result look credible.
   * Reading as latin1 makes byte n map to char n, so the UTF-16LE form is just the ASCII with NULs interleaved. */
  const utf16 = (s) => s.split("").map((c) => c + "\u0000").join("");
  const found = {};
  for (const m of markers) {
    const hits = [];
    for (const f of names) {
      try {
        const buf = readServed(f, split).toString("latin1");
        if (buf.includes(m)) hits.push(f.replace(/\.[^.]+\./, "."));
        else if (buf.includes(utf16(m))) hits.push(f.replace(/\.[^.]+\./, ".") + " (utf16)");
      } catch { }
    }
    found[m] = hits;
  }
  return { framework: FW, files, markers: found };
}

/** One-line-per-fact summary, for a run's stdout. Returns the number of markers that were NOT found. */
export function printDeployment (dep, log = console.log) {
  if (dep.error) { log(`  !! deployment unknown: ${dep.error}`); return 0; }
  for (const f of dep.files) {
    log(`  served  ${f.name.padEnd(34)} ${String(f.mb).padStart(7)} MB  ${f.mtime.replace("T", " ").slice(0, 19)}` +
      (f.parts ? `  (${f.parts} parts)` : ""));
    if (f.error) log(`  !! ${f.name}: torn split resource -- ${f.error}`);
  }
  let missing = 0;
  for (const [m, where] of Object.entries(dep.markers)) {
    if (where.length) log(`  marker OK      ${m}  (in ${where.join(", ")})`);
    else { log(`  marker MISSING ${m}  — the code you think you are measuring is NOT in the served bytes`); missing++; }
  }
  return missing;
}
