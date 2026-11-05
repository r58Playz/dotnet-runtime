// wj doctor -- is this machine configured to run the harness? Prints the resolved configuration, then one line per
// check: ok / warn / MISSING. Exit 1 when something the core drivers need is missing (the app checkout, a browser,
// node/python/perf, a writable data root). Optional pieces (the native reference, d8, TeaVM) only warn: they gate
// single tools, which refuse by name when run.
import { execSync, spawnSync } from "child_process";
import { existsSync, statfsSync, accessSync, constants, readFileSync } from "fs";
import { join } from "path";
import { resolved, KEY_DOCS, machineFile, cfg, HARNESS } from "./paths.mjs";
import { SEEDS } from "./seeds.mjs";

let bad = 0, warned = 0;
const line = (st, what, detail = "") => {
  if (st === "MISSING") bad++; if (st === "warn") warned++;
  console.log(`  ${st.padEnd(7)} ${what}${detail ? `  -- ${detail}` : ""}`);
};
const sh = (cmd) => { try { return execSync(cmd, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim(); } catch { return null; } };
const have = (bin) => spawnSync("sh", ["-c", `command -v ${bin}`], { encoding: "utf8" }).status === 0;

console.log(`machine file: ${machineFile}${existsSync(machineFile) ? "" : "  (absent: defaults and autodetection only)"}\n`);
const r = resolved();
for (const { key, what } of KEY_DOCS) console.log(`  ${key.padEnd(15)} ${r[key] ?? "(unset)"}${r[key] ? "" : `   ${what}`}`);

console.log("\ndata root");
const home = r.WJ_HOME;
try { accessSync(home, constants.W_OK); line("ok", `WJ_HOME ${home} writable`); } catch { line("MISSING", `WJ_HOME ${home}`, "not a writable directory"); }
for (const d of ["state", "runs", "notes"]) {
  const p = join(home, d);
  existsSync(p) ? line("ok", `${d}/`) : line(d === "runs" ? "warn" : "warn", `${d}/ absent`, d === "runs" ? "created on first run" : `mkdir -p ${p}`);
}
const fstype = sh(`stat -f -c %T ${JSON.stringify(home)}`);
if (fstype && !/btrfs|xfs/.test(fstype)) line("warn", `WJ_HOME is on ${fstype}`, "no reflinks: every run copies its 0.5-0.8 GB seed in full");
for (const [name, s] of Object.entries(SEEDS)) {
  const p = join(home, "state", "seeds", s.dir);
  existsSync(p) ? line("ok", `seed ${name} (${s.dir})`) : line(name === "mcsr" ? "MISSING" : "warn", `seed ${name}: ${p}`, "wj seedbuild, or restore it from its OPFS tar");
}

console.log("\nsibling checkouts");
const dirKey = (k, required, probe) => {
  const v = cfg(k);
  if (!v) return line(required ? "MISSING" : "warn", `${k} unset`, `${KEY_DOCS.find((d) => d.key === k).what}; set it in ${machineFile}`);
  if (!existsSync(probe ? join(v, probe) : v)) return line(required ? "MISSING" : "warn", `${k} = ${v}`, `${probe || "path"} not found`);
  line("ok", `${k}`);
};
dirKey("IKVMCRAFT", true, "frontend/src/dotnet/index.ts");
dirKey("IKVM_SRC", false, "src/IKVM.Runtime/JVM.cs");
dirKey("IKVM_BUILD", false, "tools/ikvm");
dirKey("MESA_WINK", false, "wink/build.sh");
dirKey("TEAVM_DIR", false, "samples/benchmark");
dirKey("TEAVM_BASELINE", false, "teavm-wasm-fixed.html");
dirKey("CHROMIUM_SRC", false, "v8/include/v8-version.h");
dirKey("PRISM_INSTANCE", false, "minecraft");
dirKey("NW_TAR", false);
dirKey("D8", false);
dirKey("FNA_WASM_BUILD", false, "build-dotnet.sh");
dirKey("DOTNET_ZIP", false);

console.log("\ntools");
const nodeMajor = Number(process.versions.node.split(".")[0]);
nodeMajor >= 22 ? line("ok", `node ${process.versions.node}`) : line("MISSING", `node ${process.versions.node}`, "needs >= 22 (global WebSocket, import.meta.dirname)");
const py = sh("python3 --version"); py ? line("ok", py) : line("MISSING", "python3");
const chrome = cfg("CHROME_BIN");
chrome && existsSync(chrome) ? line("ok", `browser ${sh(`${chrome} --version`) || chrome}`) : line("MISSING", "CHROME_BIN", "no chromium found on PATH; set CHROME_BIN");
for (const [bin, req, why] of [
  ["perf", true, "every capture"], ["taskset", true, "pinning"], ["systemd-run", true, "wj bg, ledger cgroups"], ["curl", true, "preflight, deploy"],
  ["readelf", false, "perfraw native symbols"], ["objdump", false, "hotinsn"], ["wasm-objdump", false, "wasmtier"], ["wasm2wat", false, "depcheck, wasmtier"],
  ["debuginfod-find", false, "chromium symbols (lib/debuginfo.mjs)"], ["gcc", false, "enctest"], ["dotnet", false, "deploy, ikvm-runtime"],
  ["zip", false, "ikvm-runtime --install"], ["unzip", false, "deploy"], ["ss", false, "ensure-server"],
  ["Xwayland", false, "native reference"], ["xdotool", false, "native reference"], ["mangohud", false, "native reference"],
]) have(bin) ? line("ok", bin) : line(req ? "MISSING" : "warn", bin, why);
for (const v of [8, 25]) cfg(`JAVA_HOME_${v}`) ? line("ok", `JAVA_HOME_${v}`) : line("warn", `JAVA_HOME_${v} unset`, `the hs${v} arms`);

console.log("\nperf and the CPU");
const paranoid = sh("cat /proc/sys/kernel/perf_event_paranoid");
Number(paranoid) <= 1 ? line("ok", `perf_event_paranoid ${paranoid}`) : line("warn", `perf_event_paranoid ${paranoid}`, "user-space PEBS sampling needs <= 1");
const rate = sh("cat /proc/sys/kernel/perf_event_max_sample_rate");
if (rate) line("ok", `perf_event_max_sample_rate ${rate}`, "keep per-event periods under it (a throttled event undercounts)");
r.WJ_UARCH === "glc-grt" ? line("ok", `PMU ${r.WJ_PMU_CORE}${r.WJ_PMU_ATOM ? ` + ${r.WJ_PMU_ATOM}` : ""} (Golden Cove + Gracemont raw codes valid)`)
  : line("warn", `CPU ${r.WJ_UARCH}`, "hwrun / natrun / fe raw-code event sets refuse here (lib/env.sh pmu_require)");
line("ok", `pins: arms ${r.WJ_PCPUS}, census ${r.WJ_CENSUS_CPUS}, boot ${r.WJ_BOOT_CPUS}${r.WJ_ECPUS ? `, E-cores ${r.WJ_ECPUS}` : ""}`);
try {
  const st = statfsSync("/tmp"), free = (st.bfree * st.bsize) / 1e9;
  free >= 2 ? line("ok", `/tmp ${free.toFixed(1)} GB free`) : line("warn", `/tmp ${free.toFixed(1)} GB free`, "wj reap --apply");
} catch { }

console.log("\napp");
const code = sh(`curl -sk -o /dev/null -w '%{http_code}' --max-time 5 ${cfg("SERVE_URL")}`);
code === "200" ? line("ok", `${cfg("SERVE_URL")} serves (unit ${cfg("DEV_UNIT")})`) : line("warn", `${cfg("SERVE_URL")} -> HTTP ${code || "none"}`, "wj ensure-server");
const fw = cfg("IKVMCRAFT") && join(cfg("IKVMCRAFT"), "frontend/public/_framework");
if (fw && existsSync(fw)) {
  const wasm = sh(`ls ${JSON.stringify(fw)} | grep -m1 -o 'dotnet.native.[a-z0-9]*.wasm'`);
  line("ok", `deployed runtime ${wasm || "(none found)"}`);
} else line("warn", "no deployed _framework", "wj deploy <marker>");

console.log(`\n${bad ? `${bad} missing` : "nothing missing"}, ${warned} warning(s). Harness code: ${HARNESS}`);
process.exit(bad ? 1 : 0);
