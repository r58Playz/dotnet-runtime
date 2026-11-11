// Boot-only PASS/FAIL for one configuration. ~2 minutes instead of ~9.
//
// A full wasmtier/mcperf/mcab run is boot + world generation + a timed window; when the question is only
// "does this configuration still boot correctly", that is 7 wasted minutes per arm. This drives the same
// browser through the same markers and stops the moment boot either reaches the modded main screen or
// throws, so a knob can be bisected in minutes.
//
// It exists because MONO_INLINELIMIT=100 -- the one lever the round-117 analysis named as untested and
// highest-value -- turned out to kill boot inside Fabric's mixin bootstrap. Deciding whether that is the
// wasm JIT miscompiling an inlined body, rather than something else the knob touches, is a sequence of
// cheap boot-only arms, not a sequence of full runs.
//
// Usage:
//   wj bootcheck [--seed NAME] [--knob k=v ...] [--jprop k=v ...] [--timeout 260] [--tag NAME]
//
// --seed (lib/seeds.mjs, default MC_SEED, else 262n) chooses the profile, the version typed into the launcher and the
// title line. Since the 2026-10-21 launcher nothing boots until Play is pressed, so this attaches CDP to press it;
// marker times print relative to Play, and `firstFrame` (the loading overlay) is the first thing a player sees.
import { launchGame, buildUrl, seedProfile, sleep, markersFor, seedFromArgs, cdpConnect, driveLauncher, launcherState, monotonic } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
import { classifyLine, ENV } from "../lib/verdict.mjs";
import { mkdirSync, readFileSync, existsSync, readdirSync, writeFileSync } from "fs";
import { spawn, spawnSync } from "child_process";
import { join, resolve } from "path";
import { defaultOut, HARNESS } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
const KNOBS = (() => {
  const out = {};
  for (let i = 0; i < args.length; i++) {
    if (args[i] !== "--knob") continue;
    const [k, ...rest] = (args[++i] || "").split("=");
    if (k) out[k] = rest.join("=");
  }
  return out;
})();
const TIMEOUT = Number(str("timeout", 260)) * 1000;
const KEEP_GOING = args.includes("--keep-going");
/* --stall-s N (default 120): no APP output (a console line from the page) for N s after Play ends the boot as STALL,
 * naming the last app line. R500's boot hang -- always right after LazyDfuTransform is applied to
 * DataFixerBuilder$Result, ~1 boot in 12-16 -- is total app quiescence, and without this it sat out the whole timeout. */
const STALL_S = Number(str("stall-s", 120));
/* --dump-on-stall: on STALL, ask mono for a thread dump (the app's "Thread dump" button: perform_thread_dump ->
 * mono_threads_request_thread_dump) and snapshot every renderer thread's state/wchan/CPU from /proc before teardown.
 * The dump prints into the log asynchronously, hence the wait. */
const DUMP_ON_STALL = args.includes("--dump-on-stall");
const TAG = str("tag", "bootcheck");
/* --hold-until RX [--hold-s N, default 240]: after a PASS, keep the page alive until a console line matches RX (or N s pass)
 * and print that line -- for a probe the app runs at a fixed time after boot, e.g. IKVM_GC_PROBE_MS's "[IKVM gcprobe]". */
const HOLD_RX = str("hold-until", null) ? new RegExp(str("hold-until", null)) : null;
const HOLD_S = Number(str("hold-s", 240));
/* --census-after-hold ADDRS.json: when the hold line arrives, take memaudit/heapcensus.js (types in the major heap and
 * LOS) into <out>/<tag>-heap.json -- right after IKVM_GC_PROBE_MS's full collection, so it approximates the live set. */
const CENSUS_ADDRS = str("census-after-hold", null);
const ROOT = HARNESS;
const SEED = seedFromArgs(args);
const MARKERS = markersFor(SEED);
/* --out DIR (default runs/<today>-bootcheck, shared by the day's boots): <tag>.log, profile-<tag>/ */
const OUT = defaultOut("bootcheck", str("out", null));

const profile = seedProfile(join(OUT, `profile-${TAG}`), { seed: SEED });
const logPath = join(OUT, `${TAG}.log`);
/* --jprop K=V: a Java system property (index.ts ?jprop.), e.g. fabric.debug.logClassLoad=true for a class-load census */
const JPROPS = args.flatMap((a, i) => (a === "--jprop" ? [args[i + 1]] : []));
const url = (() => { const u = new URL(buildUrl({ knobs: KNOBS, seed: SEED }));
  for (const kv of JPROPS) { const [k, ...v] = kv.split("="); u.searchParams.set(`jprop.${k}`, v.join("=")); }
  return u.toString(); })();
console.log(`${TAG}: seed ${SEED.name} ${JSON.stringify(KNOBS)}`);

/* titleReady (FAILED_AUTH, the MCSR Ranked main screen's offline-auth line) is "boot completed". Anything
 * in FAIL_RX is a real failure. Both are console text, so one tap decides the run -- no polling, and no
 * waiting out a timeout to discover the answer arrived 200 seconds ago. */
const READY = MARKERS.find((m) => m.name === "titleReady").rx;
/* mcdrive's FAIL_RX/OOM_RX are the vetted signatures; the mixin ones below are what this boot failure
 * actually looks like and are not in them.
 *
 * IKVM logs EVERY class it loads, so the log is full of lines like
 *   [IkvmClassLoader] 'net.fabricmc.loader.impl.FormattedException' loaded from URLClassLoader
 * and a failure pattern that mentions an exception TYPE matches the moment that type is loaded -- about
 * thirty seconds in, on a perfectly healthy run. A first cut of this file scored five arms FAIL on exactly
 * that line and produced no information at all. So: never match on a bare type name, and drop class-load
 * lines outright before testing anything. */
/* The class-load guard now lives in verdict.mjs (with a selftest case pinning it), so it is not
 * duplicated here. What IS still local is the MIXIN family: it is a BOOT-ONLY failure shape, and a mixin
 * config error mid-run is not automatically a failed run, so it stays out of the shared taxonomy. */
const MIXIN_FAIL = /Error parsing or using Mixin config|MixinInitialisationError: Error initialising/;
const isFail = (line) => {
  if (SEED.benignRx && SEED.benignRx.test(line)) return null;
  if (MIXIN_FAIL.test(line) && !/\[IkvmClassLoader\]/.test(line)) return { cls: "mixin-init", key: "mixin" };
  const c = classifyLine(line);
  /* env-severity classes are NOT boot failures: a network timeout or a missing GL context says nothing
   * about the build, and counting them as FAIL is what R269 identified as corrupting a stability gate. */
  return c && c.sev !== ENV ? c : null;
};

const game = launchGame({ url, profile, logPath, headless: true });
let verdict = null, detail = "", failClass = null, conn = null;
try {
  conn = await cdpConnect();
  const sid = await attachPage(conn);
  await conn.send("Runtime.enable", {}, sid);
  const nav = Date.now();
  const launch = await driveLauncher(conn, sid, SEED, { log: (m) => console.log(m) });
  const t0 = launch.mode === "launcher" ? Date.now() : nav;
  console.log(`  ${((t0 - nav) / 1000).toFixed(1).padStart(6)}s  page+launcher (marker times below are from Play)`);
  let warned = false;
  let lastApp = Date.now(), lastAppLine = "";
  const seen = new Set();
  let off = 0;
  while (!verdict && Date.now() - t0 < TIMEOUT) {
    await sleep(500);
    if (!warned && !seen.has("javaMain") && /Downloading/i.test(await launcherState(conn, sid) || "")) {
      warned = true; console.log(`  !! the app is DOWNLOADING after Play: seed ${SEED.name} lacks a file this app checks; this boot measures the network`);
    }
    if (!existsSync(logPath)) continue;
    if (!verdict && Date.now() - lastApp > STALL_S * 1000) {
      verdict = "STALL"; detail = `no app output for ${STALL_S} s; last: ${lastAppLine.replace(/^.*CONSOLE:\d+\] /, "").slice(0, 200)}`;
      break;
    }
    const buf = readFileSync(logPath, "utf8");
    if (buf.length <= off) continue;
    const chunk = buf.slice(off); off = buf.length;
    for (const line of chunk.split("\n")) {
      if (!line) continue;
      if (/:CONSOLE:\d+\]/.test(line)) { lastApp = Date.now(); lastAppLine = line; }
      for (const m of MARKERS) {
        if (!seen.has(m.name) && m.rx.test(line)) {
          seen.add(m.name);
          console.log(`  ${((Date.now() - t0) / 1000).toFixed(1).padStart(6)}s  ${m.name}`);
        }
      }
      if (!verdict && READY.test(line)) verdict = failClass ? "PASS-FAULTED" : "PASS";
      /* The FIRST failing line only: later ones are the same exception re-thrown up the chain, and the
       * outermost wrapper ("resource was invalid or could not be read") names the symptom, not the cause. */
      if (!verdict && !failClass) {
        const f = isFail(line);
        /* --keep-going: a TIMING run records the first fault and still waits for the title, unless the fault is
         * terminal (the page is gone and no title can come) */
        if (f) { failClass = `${f.cls}:${f.key}`; detail = line.replace(/^.*CONSOLE:\d+\] /, "").slice(0, 240);
                 if (!KEEP_GOING || f.sev === "terminal") verdict = "FAIL";
                 else console.log(`  ${((Date.now() - t0) / 1000).toFixed(1).padStart(6)}s  !! fault ${failClass} (--keep-going)`); }
      }
    }
  }
  if (!verdict) { verdict = "TIMEOUT"; detail = `no titleReady and no failure within ${TIMEOUT / 1000}s`; }
  /* memory at the title: the page's O(1) counters (loader/Emscripten.c) and the wasm memory length, MB */
  if (verdict.startsWith("PASS")) {
    await sleep(2000);
    const r = await Promise.race([conn.send("Runtime.evaluate", { returnByValue: true, expression: `(() => { const M = globalThis.wasm?.Module; if (!M) return null;
      return JSON.stringify({ wasm: (M.wasmMemory?.buffer ?? globalThis.getDotnetRuntime(0).localHeapViewU8().buffer).byteLength / 1048576, heap: M._ikvm_heap_top_mb?.(), native: M._ikvm_native_used_mb?.(), gc: M._ikvm_gc_heap_mb?.() }); })()` }, sid),
      sleep(8000).then(() => null)]);
    const v = r?.result?.value;
    if (v) { const m = JSON.parse(v); console.log(`  memory at title (MB): wasm ${m.wasm.toFixed(0)} heap ${m.heap?.toFixed(0)} native ${m.native?.toFixed(0)} gc ${m.gc?.toFixed(0)}`); }
    /* the renderer's threads at the title: every emscripten pthread is a DedicatedWorker (an sgen worker pool shows here) */
    let workers = 0;
    for (const d of readdirSync("/proc")) {
      if (!/^\d+$/.test(d)) continue;
      let cl = ""; try { cl = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
      if (!cl.includes(profile) || !cl.includes("--type=renderer")) continue;
      for (const t of readdirSync(`/proc/${d}/task`)) { try { if (readFileSync(`/proc/${d}/task/${t}/comm`, "utf8").startsWith("DedicatedWorker")) workers++; } catch {} }
    }
    console.log(`  DedicatedWorker threads at title: ${workers}`);
    /* --dump-at-title (R516): the runtime's printf dumps (dumpWasmJit -> mono_wasm_jit_dump_stats, which prints the
     * [wasm-jit stackwalk] census among others) right at the title, then a pause for them to reach the log. */
    if (args.includes("--dump-at-title")) {
      await conn.send("Runtime.evaluate", { expression: "globalThis.dumpWasmJit && globalThis.dumpWasmJit()" }, sid).catch(() => {});
      await sleep(4000);
    }
    /* --debug-crash (R516): after the title, hold F3+C for 10.5 s -- Minecraft's "Manually triggered debug crash", i.e. a
     * CrashReport built from a live stack (CrashReportCategory.fillInStackTrace / validateStackTrace index the trace by
     * POSITION). Pair with --hold-until to wait for the report. Document-level KeyboardEvents, as mcdrive's sendKey. */
    if (args.includes("--debug-crash")) {
      const kev = (type, code, key) => conn.send("Runtime.evaluate", { expression: `document.dispatchEvent(new KeyboardEvent(${JSON.stringify(type)},{code:${JSON.stringify(code)},key:${JSON.stringify(key)},repeat:false,bubbles:true,cancelable:true}))` }, sid);
      await kev("keydown", "F3", "F3"); await sleep(200);
      await kev("keydown", "KeyC", "c");
      console.log(`  debug crash: holding F3+C for 10.5 s`);
      await sleep(10500);
      await kev("keyup", "KeyC", "c"); await kev("keyup", "F3", "F3");
    }
    if (HOLD_RX) {
      const h0 = Date.now(); let hit = null;
      while (!hit && Date.now() - h0 < HOLD_S * 1000) {
        await sleep(1000);
        const buf = readFileSync(logPath, "utf8"); const chunk = buf.slice(off); off = buf.length;
        for (const line of chunk.split("\n")) if (HOLD_RX.test(line)) { hit = line; break; }
      }
      console.log(hit ? `  hold: ${hit.replace(/^.*CONSOLE:\d+\] /, "").replace(/, source: .*$/, "").slice(0, 300)}` : `  hold: no line matching ${HOLD_RX} within ${HOLD_S} s`);
      if (hit && CENSUS_ADDRS) {
        const out = join(OUT, `${TAG}-heap.json`);
        const r = spawnSync("node", [join(ROOT, "memaudit/runjs.mjs"), join(ROOT, "memaudit/heapcensus.js"), out, "--ms", "180000", readFileSync(CENSUS_ADDRS, "utf8").trim()], { encoding: "utf8", timeout: 240000 });
        console.log(`  census: ${(r.stdout || "").trim().split("\n").pop() || (r.stderr || "").trim().slice(0, 200)}`);
      }
    }
  }
  if (verdict === "STALL" && DUMP_ON_STALL) {
    const snap = (tag) => {
      const out = [];
      for (const d of readdirSync("/proc")) {
        if (!/^\d+$/.test(d)) continue;
        let cl = ""; try { cl = readFileSync(`/proc/${d}/cmdline`, "utf8"); } catch { continue; }
        if (!cl.includes(profile) || !cl.includes("--type=renderer")) continue;
        for (const t of readdirSync(`/proc/${d}/task`)) {
          try {
            const st = readFileSync(`/proc/${d}/task/${t}/stat`, "utf8"), comm = readFileSync(`/proc/${d}/task/${t}/comm`, "utf8").trim();
            const f = st.slice(st.lastIndexOf(")") + 2).split(" ");
            out.push(`${tag} pid ${d} tid ${t} ${comm.padEnd(16)} state ${f[0]} utime ${f[11]} stime ${f[12]} wchan ${readFileSync(`/proc/${d}/task/${t}/wchan`, "utf8").trim() || "0"}`);
          } catch {}
        }
      }
      return out;
    };
    const a = snap("t0"); await sleep(3000); const b = snap("t3");
    writeFileSync(logPath.replace(/\.log$/, "") + "-stall-threads.txt", a.concat(b).join("\n") + "\n");
    try {
      const r = await Promise.race([conn.send("Runtime.evaluate", { expression: `(globalThis.wasm?.Module?.ccall?.("perform_thread_dump"), "requested")`, returnByValue: true }, sid), sleep(8000).then(() => ({ timedOut: true }))]);
      console.log(`  thread dump: ${r?.timedOut ? "page did not answer in 8 s" : r?.result?.value ?? JSON.stringify(r).slice(0, 120)}; waiting 20 s for it to print`);
    } catch (e) { console.log(`  thread dump request threw: ${e.message}`); }
    await sleep(20000);
    console.log(`  /proc thread snapshot: ${logPath.replace(/\.log$/, "")}-stall-threads.txt`);
  }
  console.log(`\n${TAG}: ${verdict}${failClass ? ` [${failClass}]` : ""}${detail ? "\n  " + detail : ""}`);
} finally {
  try { conn?.close?.(); } catch { }
  try { game.kill(); } catch { }
  await sleep(2000);
}
/* node v26's process.exit() can deadlock in its own platform teardown -- R498 caught one live: main, the last V8Worker
 * and SignalInspector all in __futex_wait, no children, ~1 boot in 15 -- and the caller's timeout then reaps it minutes
 * later. The verdict is already written, so a detached watchdog SIGKILLs this process if exit has not finished in 15 s. */
spawn("sh", ["-c", `sleep 15; kill -9 ${process.pid} 2>/dev/null`], { detached: true, stdio: "ignore" }).unref();
process.exit(verdict === "PASS" ? 0 : 1);
