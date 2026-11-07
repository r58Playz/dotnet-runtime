// Rebuild scratchpad/mcsr/seed — the seeded browser profile every Minecraft measurement depends on.
//
// WHY THIS EXISTS
// ---------------
// mcdrive.mjs's seedProfile() reflink-copies mcsr/seed for every run, and its header explains why: the app
// downloads Minecraft 1.16.1 + Fabric into OPFS when it is not already there, which against a fresh profile
// is a multi-hundred-MB network fetch landing INSIDE the boot phase, with a different completion time every
// run. So boot would be measuring the network.
//
// What no file recorded until now is how the seed is MADE. It is `ikvmcraft-mcsrranked.tar` (the install,
// the New World save, options.txt and the mod configs) extracted into OPFS at the app's origin. That gap is
// how the directory came to be deleted as "crash-harness output": nothing pointed at it and nothing could
// recreate it. Now something can.
//
// HOW, AND WHY NOT THE OBVIOUS WAY
// --------------------------------
// OPFS is origin-scoped, so the extraction has to happen in a page on the dev server's origin. It must NOT
// be the app: main.tsx's mount() checks isMinecraftVersionDownloaded() at load and starts the download when
// it comes back false, which races the extraction that is about to make it true. Navigating to /vite.svg
// gives a document on the right origin that runs no app code at all.
//
// The tar is unpacked by this script rather than by importing the app's own extractTar() out of fs.tsx.
// That module pulls in the UI framework and its top-level css`` helper, which is not obviously safe to
// evaluate in an SVG document -- and depending on app internals is what makes a tool like this rot. A ustar
// reader is ~60 lines and the format has not changed since 1988.
//
// Usage:
//   wj seedbuild [--dest DIR] [--tar URL] [--base URL] [--keep-work]
import { launchGame, cdpConnect, sleep } from "../lib/mcdrive.mjs";
import { attachPage } from "../lib/browser.mjs";
import { existsSync, mkdirSync, rmSync, renameSync, statSync } from "fs";
import { join, resolve, dirname } from "path";
import { SEEDS_DIR, serveUrl } from "../lib/paths.mjs";

const args = process.argv.slice(2);
const str = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 ? args[i + 1] : d; };
const DEST = resolve(str("dest", join(SEEDS_DIR(), "seed")));
const BASE = str("base", serveUrl());
const TAR = str("tar", "/ikvmcraft-mcsrranked.tar");
/* The work profile sits NEXT TO the destination, never in /tmp: /tmp is tmpfs here, so a ~700 MB profile
 * there is charged to RAM and this box has gone into reclaim over less (see preflight.mjs). It also makes
 * the final move a rename within one filesystem instead of a copy. */
const WORK = DEST + ".building";

if (existsSync(DEST)) { console.error(`${DEST} already exists — remove it first if you really mean to rebuild`); process.exit(1); }
rmSync(WORK, { recursive: true, force: true });
mkdirSync(WORK, { recursive: true });
mkdirSync(dirname(DEST), { recursive: true });

/* Runs inside the page. Streams the tar straight into OPFS: at 718 MB, buffering it whole would be charged
 * to the renderer's heap on top of everything else. Only the current entry's header window is retained. */
const EXTRACT = (tarUrl) => `(async () => {
  const S = globalThis.__seed = { files: 0, dirs: 0, bytes: 0, done: false, err: null, cur: "" };
  try {
    const root = await navigator.storage.getDirectory();
    const res = await fetch(${JSON.stringify(tarUrl)});
    if (!res.ok) throw new Error("fetch " + res.status);
    const reader = res.body.getReader();
    let buf = new Uint8Array(0), eof = false;
    const pull = async () => {
      const { value, done } = await reader.read();
      if (done) { eof = true; return; }
      const n = new Uint8Array(buf.length + value.length);
      n.set(buf); n.set(value, buf.length); buf = n;
    };
    const need = async (k) => { while (buf.length < k && !eof) await pull(); return buf.length >= k; };
    const take = (k) => { const out = buf.subarray(0, k); buf = buf.subarray(k); return out; };
    const dec = new TextDecoder();
    const strAt = (h, off, len) => { const s = dec.decode(h.subarray(off, off + len)); const z = s.indexOf("\\0"); return z < 0 ? s : s.slice(0, z); };
    const octAt = (h, off, len) => { const s = strAt(h, off, len).trim(); return s ? parseInt(s, 8) || 0 : 0; };
    const dirCache = new Map([["", root]]);
    const getDir = async (p) => {
      if (dirCache.has(p)) return dirCache.get(p);
      const i = p.lastIndexOf("/");
      const parent = await getDir(i < 0 ? "" : p.slice(0, i));
      const h = await parent.getDirectoryHandle(i < 0 ? p : p.slice(i + 1), { create: true });
      dirCache.set(p, h); return h;
    };
    let longName = null;
    for (;;) {
      if (!(await need(512))) break;
      const h = take(512);
      let allZero = true;
      for (let i = 0; i < 512; i++) if (h[i] !== 0) { allZero = false; break; }
      if (allZero) break;                                  // end-of-archive marker
      const prefix = strAt(h, 345, 155);
      let name = longName ?? ((prefix ? prefix + "/" : "") + strAt(h, 0, 100));
      longName = null;
      const size = octAt(h, 124, 12);
      const type = String.fromCharCode(h[156] || 48);
      const padded = Math.ceil(size / 512) * 512;
      if (type === "L") {                                  // GNU long name: the NAME is this entry's payload
        await need(padded); longName = dec.decode(take(padded).subarray(0, size)).replace(/\\0+$/, ""); continue;
      }
      // pax headers: a per-file "x" header may carry the real path (records "<len> path=<value>\\n"), which is how
      // Python's tarfile writes a name over 100 bytes; skipping it put 12 libraries of seed-262n under their truncated
      // ustar names (2026-11-06). Global "g" headers carry nothing this reader needs.
      if (type === "x" || type === "g") {
        await need(padded); const rec = dec.decode(take(padded).subarray(0, size));
        if (type === "x") { const m = /(?:^|\\n)\\d+ path=([^\\n]*)\\n/.exec(rec); if (m) longName = m[1]; }
        continue;
      }
      name = name.replace(/^\\.?\\//, "").replace(/\\/+$/, "");
      S.cur = name;
      if (type === "5") { if (name) { await getDir(name); S.dirs++; } await need(padded); take(padded); continue; }
      if (type !== "0" && type !== "\\0" && h[156] !== 0) { await need(padded); take(padded); continue; }
      const i = name.lastIndexOf("/");
      const dir = await getDir(i < 0 ? "" : name.slice(0, i));
      const fh = await dir.getFileHandle(i < 0 ? name : name.slice(i + 1), { create: true });
      const w = await fh.createWritable();
      let left = size;
      while (left > 0) {
        if (!(await need(Math.min(left, 1 << 20)))) throw new Error("truncated tar at " + name);
        const chunk = take(Math.min(left, buf.length, 1 << 22));
        await w.write(chunk); left -= chunk.length; S.bytes += chunk.length;
      }
      await w.close();
      const pad = padded - size;
      if (pad) { await need(pad); take(pad); }
      S.files++;
    }
    S.done = true;
  } catch (e) { S.err = String((e && e.stack) || e); S.done = true; }
})()`;

const game = launchGame({ url: BASE.replace(/\/$/, "") + "/vite.svg", profile: WORK, logPath: join(WORK, "chrome.log"), headless: true });
let conn = null, ok = false;
try {
  conn = await cdpConnect();
  const sid = await attachPage(conn);
  /* Wait for the NAVIGATION, not just for the target. attachPage returns as soon as a page target exists,
   * and that target starts on about:blank -- evaluating there installs the extraction state into a context
   * that the pending navigation then throws away, which presents as "page lost the extraction state" a few
   * seconds later. Poll for the real document instead. */
  for (let i = 0; ; i++) {
    const r = await conn.send("Runtime.evaluate", { expression: "location.href + '|' + document.readyState", returnByValue: true }, sid);
    const v = String(r.result?.value ?? "");
    if (v.includes("/vite.svg") && v.endsWith("|complete")) break;
    if (i > 60) throw new Error(`page never reached ${BASE}vite.svg (last: ${v || "about:blank"}) — is the dev server up?`);
    await sleep(500);
  }
  console.log(`extracting ${TAR} into OPFS at ${BASE} ...`);
  /* Fire and poll rather than awaitPromise: a single await on a multi-minute extraction gives no progress
   * and no way to tell "slow" from "wedged", which is the failure mode every harness note here warns about. */
  await conn.send("Runtime.evaluate", { expression: EXTRACT(TAR), awaitPromise: false }, sid);
  let last = 0, quiet = 0;
  for (;;) {
    await sleep(2000);
    const r = await conn.send("Runtime.evaluate", { expression: "JSON.stringify(globalThis.__seed||null)", returnByValue: true }, sid);
    const s = JSON.parse(r.result?.value ?? "null");
    if (!s) { if (++quiet > 15) throw new Error("page lost the extraction state"); continue; }
    quiet = 0;
    process.stdout.write(`\r  ${s.files} files, ${s.dirs} dirs, ${(s.bytes / 1e6).toFixed(0)} MB  ${s.cur.slice(-58).padEnd(58)}`);
    if (s.done) {
      process.stdout.write("\n");
      if (s.err) throw new Error("in-page extraction failed: " + s.err);
      console.log(`extracted ${s.files} files / ${s.dirs} dirs / ${(s.bytes / 1e6).toFixed(0)} MB`);
      break;
    }
    if (s.bytes === last && ++quiet > 60) throw new Error("extraction stalled for 2 minutes");
    last = s.bytes;
  }
  /* Verify the shape the harness actually needs, not just "it wrote something". A seed missing the save
   * boots to the title screen and then sits there, which reads as a hang rather than as a bad seed. */
  const probe = await conn.send("Runtime.evaluate", {
    expression: `(async () => {
      const root = await navigator.storage.getDirectory();
      const out = { top: [], saves: [], mods: 0, options: false };
      for await (const [n, h] of root.entries()) out.top.push(n + (h.kind === "directory" ? "/" : ""));
      try { const mc = await root.getDirectoryHandle("minecraft");
        try { const sv = await mc.getDirectoryHandle("saves"); for await (const [n] of sv.entries()) out.saves.push(n); } catch {}
        try { const md = await mc.getDirectoryHandle("mods"); for await (const _ of md.entries()) out.mods++; } catch {}
        try { await mc.getFileHandle("options.txt"); out.options = true; } catch {}
      } catch {}
      return JSON.stringify(out);
    })()`, awaitPromise: true, returnByValue: true,
  }, sid);
  const v = JSON.parse(probe.result?.value ?? "null");
  console.log(`OPFS root: ${v.top.join(" ")}`);
  console.log(`saves: [${v.saves.join(", ")}]   mods: ${v.mods}   options.txt: ${v.options}`);
  if (!v.saves.length) throw new Error("no world save in OPFS — the seed would boot to the title screen and stop");
  if (!v.options) throw new Error("no minecraft/options.txt in OPFS");
  ok = true;
} finally {
  /* Shut chromium down ORDERLY. launchGame's own kill() is SIGKILL on the process group, which is right for
   * a timed run that has already produced its numbers and wrong here: OPFS writes are flushed on clean
   * shutdown, and a killed renderer yields a seed that looks complete and is subtly truncated. So ask via
   * CDP first, then SIGTERM the group, and only fall back to the SIGKILL as a last resort. */
  try { await conn?.send?.("Browser.close"); } catch { }
  await sleep(3000);
  try { process.kill(-game.proc.pid, 0); process.kill(-game.proc.pid, "SIGTERM"); await sleep(5000); } catch { }
  try { conn?.close?.(); } catch { }
  try { process.kill(-game.proc.pid, 0); game.kill(); } catch { }
  await sleep(1000);
}
if (!ok) { console.error(`\nFAILED — work profile left at ${WORK} for inspection`); process.exit(1); }
/* The build log lives inside the work profile, so it would otherwise be reflink-copied into every future
 * run's profile. Drop it before the profile becomes the seed. */
rmSync(join(WORK, "chrome.log"), { force: true });
renameSync(WORK, DEST);
console.log(`\nseed rebuilt at ${DEST} (${(statSync(DEST).size / 1e6).toFixed(1)} MB dir entry; du it for the real size)`);
console.log(`mcdrive's seedProfile() reflink-copies this per run, so it must stay on the same btrfs filesystem.`);
