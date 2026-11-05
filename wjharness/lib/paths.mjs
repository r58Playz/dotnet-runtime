// Every location and machine fact the harness needs, resolved in ONE place.
//
// Precedence, per key: the process environment, then the machine file (`$WJ_MACHINE_ENV`, default
// ~/.config/wjharness/machine.env), then autodetection, then the default below. Nothing machine-specific is written in
// any tool: a tool that needs a sibling checkout calls need("IKVMCRAFT") and refuses, naming the key and the file, when
// it is not configured. `wj doctor` prints all of it.
//
// Importing this module also EXPORTS the resolved values into process.env, so every child a driver spawns (python,
// bash, perf, chromium) sees the same configuration without resolving it again.
//
//   node lib/paths.mjs --sh     export lines for bash (lib/env.sh evals these)
//   node lib/paths.mjs --json   the same as JSON (lib/paths.py reads these when run outside `wj`)
//
// The data root, $WJ_HOME (default <repo>/scratchpad, which .git/info/exclude keeps out of git):
//   state/  seeds/ refs/ arms/ native/ corpus/ patches/   persistent; reap never touches it
//   runs/   <YYYY-MM-DD>-<tag>/                            every driver's output; disposable by construction
//   notes/  MINECRAFT-FINDINGS.md and the other untracked prose
// One filesystem for all three: seeds are reflink-copied into runs/ for every run.
import { existsSync, readFileSync, mkdirSync } from "fs";
import { resolve, join, dirname, relative, isAbsolute, delimiter } from "path";
import { fileURLToPath } from "url";
import { homedir } from "os";

export const HARNESS = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const REPO = resolve(HARNESS, "..");
const HOME = process.env.HOME || homedir();

/* The keys, with what each is for. `def` is a function so it can depend on earlier keys; undefined = no default
 * (the key must be configured before a tool that needs it runs). */
const KEYS = [
  ["WJ_HOME", "data root (state/, runs/, notes/)", () => join(REPO, "scratchpad")],
  ["IKVMCRAFT", "the ikvmcraft app checkout the harness drives", undefined],
  ["IKVM_SRC", "the IKVM fork checkout (ikvm-wasm-build/tools/ikvm/ikvm)", undefined],
  ["IKVM_BUILD", "the ikvm-wasm-build checkout (its .nuget cache builds IKVM.Runtime)", undefined],
  ["MESA_WINK", "the mesa-wink checkout (GL stack)", undefined],
  ["TEAVM_DIR", "the TeaVM checkout whose samples/benchmark builds the jbox2d jar", undefined],
  ["TEAVM_BASELINE", "served root of the 2026-07-27 TeaVM baseline arm", undefined],
  ["CHROMIUM_SRC", "a chromium checkout (src/, v8 at src/v8), for checking V8 claims", undefined],
  ["PRISM_INSTANCE", "the native Prism instance natrun drives (MCSRRanked-Linux-1.16.1-Basic)", undefined],
  ["NW_TAR", "the OPFS export tar natrun restores the native world from", undefined],
  ["D8", "a d8 binary for ledger/d8x86.py", undefined],
  ["FNA_WASM_BUILD", "the FNA-WASM-Build checkout (build-dotnet.sh)", () => resolve(REPO, "..", "FNA-WASM-Build")],
  ["DOTNET_ZIP", "the runtime pack build-dotnet.sh writes and deploy installs", () => join(REPO, "dotnet-jspi.zip")],
  ["EMSDK", "the canonical emsdk (llvm-dwarfdump, embuilder)", () => cfg("IKVMCRAFT") && join(cfg("IKVMCRAFT"), "statics", "emsdk")],
  ["CHROME_BIN", "the browser every driver launches", () => which("chromium") || which("chromium-browser") || which("google-chrome-stable")],
  ["SERVE_URL", "where the dev server serves the app", () => "https://localhost:5021/"],
  ["DEV_UNIT", "systemd --user unit that runs the dev server", () => "ikvmcraft-dev"],
  ["DEV_CMD", "the dev server command (run in $IKVMCRAFT/frontend)", () => "pnpm dev"],
  ["JAVA_HOME_8", "JDK 8 home (HotSpot reference arms)", () => jdk(8)],
  ["JAVA_HOME_17", "JDK 17 home", () => jdk(17)],
  ["JAVA_HOME_21", "JDK 21 home", () => jdk(21)],
  ["JAVA_HOME_25", "JDK 25 home", () => jdk(25)],
  ["WJ_PCPUS", "CPUs a measured arm is pinned to (the big cores)", () => topo().p],
  ["WJ_ECPUS", "the remaining CPUs (empty on a non-hybrid part)", () => topo().e],
  ["WJ_CENSUS_CPUS", "CPUs a queued census runs on while the next arm boots", () => topo().census],
  ["WJ_BOOT_CPUS", "CPUs a boot A/B pins to: every CPU but the census ones (abboot, natboot)", () => topo().boot],
  ["WJ_PMU_CORE", "perf PMU name of the big cores (cpu_core on hybrid Intel, cpu otherwise)", () => pmu().core],
  ["WJ_PMU_ATOM", "perf PMU name of the small cores (empty when there are none)", () => pmu().atom],
  ["WJ_UARCH", "the event-set family raw perf codes are declared for (see lib/env.sh pmu_require)", () => pmu().uarch],
];

export const machineFile = process.env.WJ_MACHINE_ENV || join(HOME, ".config", "wjharness", "machine.env");

/* KEY=VALUE per line, # comments, optional quotes; ~ and $HOME / ${KEY} expand (KEY from the file so far or the env). */
function readMachineFile (path) {
  const out = {};
  if (!existsSync(path)) return out;
  for (const raw of readFileSync(path, "utf8").split("\n")) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const m = /^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$/.exec(line);
    if (!m) continue;
    let v = m[2].trim().replace(/^(["'])(.*)\1$/, "$2");
    v = v.replace(/^~(?=\/|$)/, HOME).replace(/\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?/g, (_, k) => out[k] ?? process.env[k] ?? "");
    out[m[1]] = v;
  }
  return out;
}
const machine = readMachineFile(machineFile);

const cache = new Map();
/* The resolved value of a key, or undefined. */
export function cfg (key) {
  if (cache.has(key)) return cache.get(key);
  let v = process.env[key];
  if (v === undefined || v === "") v = machine[key];
  if (v === undefined || v === "") {
    const k = KEYS.find(([n]) => n === key);
    v = k && k[2] ? k[2]() : undefined;
  }
  if (v === "") v = undefined;
  cache.set(key, v);
  return v;
}

/* The value of a key a tool cannot run without; throws a message that says how to configure it. */
export function need (key, why = "") {
  const v = cfg(key);
  if (v === undefined) {
    const k = KEYS.find(([n]) => n === key);
    throw new Error(`${key} is not configured${why ? ` (${why})` : ""}: ${k ? k[1] : "harness key"}. `
      + `Set it in ${machineFile} (see ${relative(process.cwd(), join(HARNESS, "machine.env.example"))}) or the environment.`);
  }
  return v;
}

function which (bin) {
  for (const d of (process.env.PATH || "").split(delimiter)) {
    const p = join(d, bin);
    if (d && existsSync(p)) return p;
  }
  return undefined;
}
function jdk (n) {
  for (const p of [`/usr/lib/jvm/java-${n}-openjdk`, `/usr/lib/jvm/java-${n}-openjdk-amd64`, `/usr/lib/jvm/jdk-${n}`])
    if (existsSync(join(p, "bin", "java"))) return p;
  return undefined;
}
function readSys (p) { try { return readFileSync(p, "utf8").trim(); } catch { return undefined; } }

/* "0-3,8,10-11" <-> [0,1,2,3,8,10,11] */
export function parseCpuList (s) {
  const out = [];
  for (const part of (s || "").split(",").filter(Boolean)) {
    const [a, b] = part.split("-").map(Number);
    for (let i = a; i <= (b ?? a); i++) out.push(i);
  }
  return out;
}
export function formatCpuList (list) {
  const xs = [...new Set(list)].sort((a, b) => a - b), runs = [];
  for (const x of xs) {
    const r = runs[runs.length - 1];
    if (r && x === r[1] + 1) r[1] = x; else runs.push([x, x]);
  }
  return runs.map(([a, b]) => (a === b ? `${a}` : `${a}-${b}`)).join(",");
}

/* Hybrid Intel exposes cpu_core / cpu_atom PMUs, each with a cpus list: P-cores (with their SMT siblings) are the
 * pinned set, a census runs on the last four E-cores. On a uniform part the census takes the last quarter and the
 * arms get the rest, so the two never share a CPU. */
let topoCache;
function topo () {
  if (topoCache) return topoCache;
  const core = readSys("/sys/devices/cpu_core/cpus"), atom = readSys("/sys/devices/cpu_atom/cpus");
  if (core && atom) {
    const e = parseCpuList(atom);
    const census = e.slice(-Math.min(4, e.length));
    topoCache = { p: core, e: atom, census: formatCpuList(census),
      boot: formatCpuList(parseCpuList(readSys("/sys/devices/system/cpu/online")).filter((c) => !census.includes(c))) };
  } else {
    const all = parseCpuList(readSys("/sys/devices/system/cpu/online") || "0");
    const k = Math.max(1, Math.floor(all.length / 4));
    topoCache = all.length > 1
      ? { p: formatCpuList(all.slice(0, all.length - k)), e: "", census: formatCpuList(all.slice(-k)), boot: formatCpuList(all.slice(0, all.length - k)) }
      : { p: formatCpuList(all), e: "", census: formatCpuList(all), boot: formatCpuList(all) };
  }
  return topoCache;
}

/* The raw event codes hwrun / hwcensus / natrun / fe use are Golden Cove + Gracemont encodings (Alder Lake and Raptor
 * Lake hybrids). Any other part gets WJ_UARCH=unknown-<vendor>-<family>-<model>, and pmu_require refuses on it. */
let pmuCache;
function pmu () {
  if (pmuCache) return pmuCache;
  const info = readSys("/proc/cpuinfo") || "";
  const field = (k) => (new RegExp(`^${k}\\s*:\\s*(.*)$`, "m").exec(info) || [])[1];
  const vendor = field("vendor_id"), family = Number(field("cpu family")), model = Number(field("model"));
  const hybrid = existsSync("/sys/bus/event_source/devices/cpu_core");
  const ADL_RPL = [0x97, 0x9a, 0xb7, 0xba, 0xbf];
  const uarch = vendor === "GenuineIntel" && family === 6 && ADL_RPL.includes(model) && hybrid
    ? "glc-grt" : `unknown-${vendor}-${family}-${model}`;
  pmuCache = { core: hybrid ? "cpu_core" : "cpu", atom: hybrid ? "cpu_atom" : "", uarch };
  return pmuCache;
}

export const WJ_HOME = () => resolve(need("WJ_HOME"));
export const STATE = () => join(WJ_HOME(), "state");
export const RUNS = () => join(WJ_HOME(), "runs");
export const NOTES = () => join(WJ_HOME(), "notes");
export const SEEDS_DIR = () => join(STATE(), "seeds");
export const ARMS = () => join(STATE(), "arms");
export const REFS = () => join(STATE(), "refs");
export const NATIVE = () => join(STATE(), "native");
export const CORPUS = () => join(STATE(), "corpus");
export const serveUrl = () => cfg("SERVE_URL").replace(/\/?$/, "/");

/* runs/<YYYY-MM-DD>-<tag>[-HHMMSS if taken], created. The default output of every driver. */
const pad = (n) => String(n).padStart(2, "0");
export const today = (d = new Date()) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
export function runDir (tag, { unique = true } = {}) {
  const d = new Date();
  let p = join(RUNS(), `${today(d)}-${tag}`);
  if (unique && existsSync(p)) p += `-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
  mkdirSync(p, { recursive: true });
  return p;
}

/* An output directory must never be inside the harness's code (that is how 450 GB of captures came to sit beside the
 * tools). Returns the resolved path; throws otherwise. */
export function assertOutdir (p) {
  const abs = resolve(p);
  const rel = relative(HARNESS, abs);
  if (rel === "" || (!rel.startsWith("..") && !isAbsolute(rel)))
    throw new Error(`refusing to write output inside the harness code (${abs}); use a directory under ${RUNS()}`);
  return abs;
}

/* A driver's output directory: the one given (checked, created), else runs/<today>-<tool>, SHARED by every invocation
 * of that tool that day -- the old flat mc-out, dated, so a loop of bootchecks (arm.sh, abboot.sh) lands in one place. */
export function defaultOut (toolName, given) {
  const p = given ? assertOutdir(given) : join(RUNS(), `${today()}-${toolName}`);
  mkdirSync(p, { recursive: true });
  return p;
}

/* Path of a harness tool by file name (e.g. "servertid.py"), searched in every code directory. */
export const CODE_DIRS = ["lib", "drive", "build", "read", "fe", "ledger", "tier", "native", "boot", "memaudit", "j2d", "live", "enctest"];
export function tool (name) {
  for (const d of CODE_DIRS) {
    const p = join(HARNESS, d, name);
    if (existsSync(p)) return p;
  }
  throw new Error(`no harness tool named ${name}`);
}

export function resolved () {
  const o = { WJ_HARNESS: HARNESS, WJ_REPO: REPO, WJ_MACHINE_ENV: machineFile };
  for (const [k] of KEYS) { const v = cfg(k); if (v !== undefined) o[k] = v; }
  if (o.WJ_HOME) {
    o.WJ_HOME = resolve(o.WJ_HOME);
    Object.assign(o, { WJ_STATE: join(o.WJ_HOME, "state"), WJ_RUNS: join(o.WJ_HOME, "runs"), WJ_NOTES: join(o.WJ_HOME, "notes") });
  }
  return o;
}
export const KEY_DOCS = KEYS.map(([k, what, def]) => ({ key: k, what, hasDefault: !!def }));

/* Export everything resolved so children inherit it. */
for (const [k, v] of Object.entries(resolved())) if (process.env[k] === undefined || process.env[k] === "") process.env[k] = v;

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const o = resolved();
  if (process.argv.includes("--json")) console.log(JSON.stringify(o));
  else for (const [k, v] of Object.entries(o)) console.log(`export ${k}='${String(v).replace(/'/g, "'\\''")}'`);
}
