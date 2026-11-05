#!/usr/bin/env node
// faultcensus.mjs [paths...] [--since YYYY-MM-DD] [--by-key] [--selftest] [--json]
//
// Classify every archived run log with lib/verdict.mjs and print what a build's failures ACTUALLY ARE.
//
// WHY A CENSUS RATHER THAN A FAULT RATE
// -------------------------------------
// The measured fault rate is ~19% (4/21, R269). Distinguishing that from a halved ~10% at p<0.05 needs
// on the order of 150 runs per arm, and a run is ~7 minutes -- so an aggregate rate is not a quantity
// this box can move a decision with. PER-CLASS counts are: "loader.c:1826 went 6 -> 0" is a claim a
// handful of runs can support, and it is the claim a fix actually makes.
//
// DEDUPE IS NOT OPTIONAL. One wasm trap emits ~4 matching lines within ~8 ms (WORKER_TRAP_STACK, "worker
// sent an error!", two Uncaught RuntimeError), so a line count overstates events 4x. R230 measured this:
// 104 logs, 105 EVENTS, 103 of them exactly one event -- a trap is one-shot and kills a worker; it does
// not recur. Collapse same-class hits inside a window, exactly as mcdrive does.
import { readFileSync, readdirSync, statSync, existsSync } from "fs";
import { join } from "path";
import { CORPUS, RUNS } from "../lib/paths.mjs";
import { classifyLine, summariseFaults, ALL_CLASSES, SEVERITY_OF, selftest, RULES_FOR_TEST } from "../lib/verdict.mjs";

const argv = process.argv.slice(2);
const has = (f) => argv.includes(`--${f}`);
const str = (f, d = null) => { const i = argv.indexOf(`--${f}`); return i >= 0 ? argv[i + 1] : d; };

if (has("selftest")) process.exit(selftest() ? 0 : 1);

/* --disjoint: run the DISJOINTNESS invariant over the real corpus rather than over the samples.
 *
 * The sample-based check in verdict.mjs can only test lines someone thought to write down, and the two
 * overlaps that actually existed were both found by running against real logs: the abort line quoting
 * the assertion it followed, and `WORKER_TRAP_STACK RangeError: Maximum call stack size exceeded`
 * matching both the trap channel and the stack-overflow cause. A gate that cannot reach the case a
 * change introduces is not evidence about that change. */
const disjointOnly = has("disjoint");

/*
 * LOCAL midnight, not UTC. `new Date("2026-09-21")` is parsed as UTC by spec, while file mtimes are
 * compared against the local clock -- so on a UTC-7 box `--since 2026-09-21` silently began at 17:00 on
 * the 20th and swept in the previous day's runs. That produced a census showing `method-to-ir.c:805`
 * "today" from a run on the PRE-FIX binary, i.e. a filter quietly answering about the wrong population.
 * Exactly the shape this project keeps paying for (`tick-*` vs `tk-*`, the pooled Render thread), so:
 * split the date and build it in local time.
 */
const since = (() => {
  const v = str("since");
  if (!v) return 0;
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(v);
  return m ? new Date(+m[1], +m[2] - 1, +m[3]).getTime() : new Date(v).getTime();
})();
const roots = argv.filter((a) => !a.startsWith("--") && a !== str("since"));
/* default: the archived corpus (state/corpus, the logs verdict.mjs was validated against) plus every run since */
if (!roots.length) roots.push(...[CORPUS(), RUNS()].filter((d) => existsSync(d)));

/* Chromium writes one timestamped line per console message, so a run log is large (~1.7 MB) and there
 * are thousands. Walk once, stream nothing -- these fit in memory individually and the whole corpus is
 * ~6,000 files. */
const logs = [];
const walk = (d, depth = 0) => {
  if (depth > 6) return;
  let ents; try { ents = readdirSync(d, { withFileTypes: true }); } catch { return; }
  for (const e of ents) {
    const p = join(d, e.name);
    if (e.isDirectory()) { if (e.name !== "node_modules" && e.name !== "seed") walk(p, depth + 1); }
    else if (e.isFile() && /\.log$/.test(e.name)) {
      let st; try { st = statSync(p); } catch { continue; }
      if (st.mtimeMs >= since && st.size > 2048) logs.push({ p, mtime: st.mtimeMs, size: st.size });
    }
  }
};
for (const r of roots) { if (existsSync(r)) (statSync(r).isDirectory() ? walk(r) : logs.push({ p: r, mtime: 0, size: 0 })); }
logs.sort((a, b) => a.mtime - b.mtime);

const DEDUPE_MS = 1000;
const perClass = new Map();          // cls -> { events, files:Set, keys:Map }
let filesWithAny = 0, scanned = 0;

for (const { p } of logs) {
  let text; try { text = readFileSync(p, "utf8"); } catch { continue; }
  scanned++;
  const seen = [];                   // {cls, at} of the last event per class, for dedupe
  const local = [];
  let lineNo = 0;
  for (const line of text.split("\n")) {
    lineNo++;
    const c = classifyLine(line);
    if (!c) continue;
    /* Chromium's own timestamp is `[pid:tid:MMDD/HHMMSS.uuuuuu:`; use it when present so the dedupe
     * window is the app's clock and not the reader's. Fall back to the line number, which orders
     * correctly even when it cannot measure. */
    const m = line.match(/:(\d{4})\/(\d{2})(\d{2})(\d{2})\.(\d{6}):/);
    const at = m ? ((+m[2] * 3600 + +m[3] * 60 + +m[4]) * 1000 + +m[5] / 1000) : lineNo * 1000;
    const prev = seen.find((s) => s.cls === c.cls && s.key === c.key);
    if (prev && Math.abs(at - prev.at) < DEDUPE_MS) { prev.n++; continue; }
    seen.push({ cls: c.cls, key: c.key, at, n: 1 });
    local.push({ ...c, at });
  }
  if (!local.length) continue;
  filesWithAny++;
  for (const f of local) {
    if (!perClass.has(f.cls)) perClass.set(f.cls, { events: 0, files: new Set(), keys: new Map() });
    const e = perClass.get(f.cls);
    e.events++; e.files.add(p);
    e.keys.set(f.key, (e.keys.get(f.key) || 0) + 1);
  }
}

if (disjointOnly) {
  const clash = new Map();
  for (const { p } of logs) {
    let t; try { t = readFileSync(p, "utf8"); } catch { continue; }
    for (const line of t.split("\n")) {
      const hits = RULES_FOR_TEST.filter((r) => r.rx.test(line) && !(r.not && r.not.test(line))).map((r) => r.cls);
      if (hits.length > 1) {
        const k = hits.sort().join(" + ");
        if (!clash.has(k)) clash.set(k, { n: 0, sample: line.trim().slice(-140) });
        clash.get(k).n++;
      }
    }
  }
  if (!clash.size) { console.log(`\ndisjointness: OK over ${scanned} logs -- no line matches two classes\n`); process.exit(0); }
  console.log(`\ndisjointness: ${clash.size} OVERLAPPING PAIR(S) over ${scanned} logs`);
  for (const [k, v] of [...clash.entries()].sort((a, b) => b[1].n - a[1].n))
    console.log(`  ${String(v.n).padStart(7)}  ${k}\n           ${v.sample}`);
  console.log("");
  process.exit(1);
}

if (has("json")) {
  const out = {};
  for (const [cls, e] of perClass) out[cls] = { events: e.events, files: e.files.size, keys: Object.fromEntries(e.keys) };
  console.log(JSON.stringify({ scanned, filesWithAny, classes: out }, null, 2));
  process.exit(0);
}

console.log(`\nfaultcensus: ${scanned} logs scanned, ${filesWithAny} carry at least one classified event`
  + (since ? `  (since ${str("since")})` : ""));
console.log(`\n  ${"class".padEnd(20)}${"sev".padEnd(10)}${"events".padStart(8)}${"logs".padStart(7)}   top keys`);
console.log(`  ${"-".repeat(72)}`);

/* Print EVERY class the classifier can emit, including the zeroes. A class that is silently omitted
 * cannot be noticed; a zero row can. Five classifier defects have shipped in this tree and every one of
 * them was invisible because the wrong answer looked like a plausible table. */
for (const cls of ALL_CLASSES) {
  const e = perClass.get(cls);
  const keys = e ? [...e.keys.entries()].sort((a, b) => b[1] - a[1]).slice(0, 3).map(([k, n]) => `${k}(${n})`).join(" ") : "";
  console.log(`  ${cls.padEnd(20)}${(SEVERITY_OF[cls] || "").padEnd(10)}`
    + `${String(e?.events ?? 0).padStart(8)}${String(e?.files.size ?? 0).padStart(7)}   ${keys}`);
}

const zero = ALL_CLASSES.filter((c) => !perClass.has(c));
if (zero.length) {
  console.log(`\n  NOTE: ${zero.length} class(es) found nothing: ${zero.join(", ")}`);
  console.log(`  A zero here is a CLASSIFIER BUG until proven otherwise -- check the pattern against a`);
  console.log(`  real line before concluding the failure does not occur. (It may also simply mean the`);
  console.log(`  corpus predates the build that can produce it; say which, do not assume.)`);
}
console.log("");
