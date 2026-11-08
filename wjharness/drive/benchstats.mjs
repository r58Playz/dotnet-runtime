// jbox2d fixed-work bench with KNOBS, and it captures the JIT's own stats lines.
//
// Why this exists beside benchseeded.mjs: that one hardcodes `?bench` with no knob path, so it cannot
// answer "is jbox2d call-dense for US". R181 needs that, because it decides whether jbox2d can serve as
// the A/B instrument for call-form work at all -- TeaVM's jbox2d module has 35 call_indirect in the whole
// program, and if ours is similarly call-sparse then dispatch cost simply is not present in this workload
// and co-location must still be judged on Minecraft.
//
// Knobs travel as ?wj.<name>=<value> (index.ts:288-298), so they append to the bench URL directly.
//
// Usage: node benchstats.mjs [--runs 1] [--knob k=v ...] [--timeout 420]
import { seedProfile, launchGame, driveLauncher } from "../lib/mcdrive.mjs";
import { connect, attachPage, consoleTap } from "../lib/browser.mjs";
import { mkdirSync, readFileSync, existsSync } from "fs";
import { join, resolve } from "path";
import { defaultOut, serveUrl } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const num = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? Number(args[i + 1]) : d; };
const RUNS = num("runs", 1);
const TIMEOUT = num("timeout", 420) * 1000;
const knobs = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] !== "--knob") continue;
  const kv = args[++i] || "";
  if (kv) knobs.push(`wj.${kv.replace("=", "=")}`);
}
const OUT = defaultOut("benchstats", (() => { const i = process.argv.indexOf("--out"); return i >= 0 ? process.argv[i + 1] : null; })());

const MEAN = /\[bench\] mean ([0-9.]+) ms\/step/;
const SUM = /\[bench\] checksum (-?\d+)/;
const url = serveUrl() + "?bench" + (knobs.length ? "&" + knobs.join("&") : "");
console.log(`url: ${url}`);

for (let i = 1; i <= RUNS; i++) {
  const name = `jstats-${i}`;
  const profile = join(OUT, `profile-${name}`);
  const logPath = join(OUT, `${name}.log`);
  seedProfile(profile, { seed: "mcsr" });   // the profile its bench.jar always came from, whatever the Minecraft default is
  console.log(`=== run ${i}/${RUNS} ===`);
  const game = launchGame({ url, profile, logPath, headless: true });
  const t0 = Date.now();
  let mean = null, sum = null;
  /* Since ikvmcraft 246560a (R514) ?bench boots through the launcher like the game: nothing runs until Play. */
  let lconn = null, lsid = null;
  for (let k = 0; k < 30 && !lconn; k++) {   // only the CDP endpoint is retried
    try {
      const ver = await (await fetch(`http://127.0.0.1:${process.env.MC_CDP_PORT || 9333}/json/version`)).json();
      lconn = await connect(ver.webSocketDebuggerUrl);
      lsid = await attachPage(lconn);
    } catch (e) { lconn = null; await new Promise((r) => setTimeout(r, 1000)); }
  }
  if (!lconn) console.log("  !! launcher: no CDP endpoint");
  else {
    await driveLauncher(lconn, lsid, "mcsr", { log: (l) => console.log(l) }).catch((e) => console.log(`  !! launcher: ${e.message}`));
    try { lconn.close(); } catch { }
  }
  while (Date.now() - t0 < TIMEOUT) {
    await new Promise((r) => setTimeout(r, 2000));
    if (!existsSync(logPath)) continue;
    const txt = readFileSync(logPath, "latin1");
    const m = txt.match(MEAN), s = txt.match(SUM);
    if (m) mean = Number(m[1]);
    if (s) sum = s[1];
    if (mean != null && sum != null) break;
  }
  console.log(`  mean=${mean ?? "none"} ms/step  checksum=${sum ?? "none"}`);
  // The counters are NOT dumped at teardown -- `?bench` is the jbox2d driver and never calls it. The app
  // exposes dumpWasmJit() on globalThis (jitbench.ts:603), so trigger it over CDP and tap the console.
  if (mean != null) {
    try {
      const port = process.env.MC_CDP_PORT || 9333;
      const ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
      const conn = await connect(ver.webSocketDebuggerUrl);
      const sid = await attachPage(conn);
      await conn.send("Runtime.enable", {}, sid);
      const got = [];
      consoleTap(conn, sid, (l) => { if (/\[wasm-jit|registered=/.test(l)) got.push(l); });
      await conn.send("Runtime.evaluate", { expression: "globalThis.dumpWasmJit && dumpWasmJit()" }, sid);
      await new Promise((r) => setTimeout(r, 4000));
      if (!got.length) console.log("  !! no [wasm-jit] lines -- is stats=1 set?");
      for (const l of got) console.log("  " + l.trim());
    } catch (e) { console.log(`  !! CDP dump failed: ${e.message}`); }
  }
  try { game.kill?.("SIGKILL"); } catch { }
  console.log(`  log: ${logPath}`);
}
