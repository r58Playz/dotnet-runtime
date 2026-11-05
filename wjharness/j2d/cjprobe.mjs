// j2d/cjprobe.mjs -- run j2d/cheerpj/gcprobe.jar (gcprobe-src/GcProbe.java: the GC / object-model contract) under CheerpJ.
//   wj cjprobe <cj<V>|cjgc<V>>          prints the probe's "[t]" lines, page errors, and the exit
import { spawn } from "child_process";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { ARMS, tool } from "../lib/paths.mjs";
import { launchChrome, connect, attachPage, consoleTap } from "../lib/browser.mjs";
const HERE = dirname(fileURLToPath(import.meta.url));
const CJ = /^cj(gc)?(\d+)$/.exec(process.argv[2] || "");
if (!CJ) { console.error("usage: cjprobe.mjs <cj<V>|cjgc<V>>"); process.exit(1); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const port = 8000 + Math.floor(Math.random() * 900);
const server = spawn("node", [tool("serve-coep.mjs"), `${join(HERE, "cheerpj")}:${join(ARMS(), "cheerpj")}`, String(port)], { stdio: "ignore" });
await sleep(800);
const chrome = launchChrome({});
const conn = await connect(await chrome.wsUrl);
const sid = await attachPage(conn);
let end = false;
consoleTap(conn, sid, (t) => {
  if (process.env.CJPROBE_ALL || /^\[t\]|^\[cj\] (error|rejection|LOAD FAILED|exit)/.test(t)) console.log(t);
  if (/^\[t\] done|^\[cj\] (rejection|LOAD FAILED|exit)/.test(t)) end = true;
});
await conn.send("Runtime.enable", {}, sid);
await conn.send("Page.navigate", { url: `http://localhost:${port}/index.html?v=${CJ[2]}&gc=${CJ[1] ? 1 : 0}&jar=gcprobe.jar` }, sid);
const t0 = Date.now();
while (!end && Date.now() - t0 < 240000) await sleep(200);
if (!end) console.log("[probe] TIMEOUT after 240 s");
try { conn.close(); } catch { }
chrome.kill(); server.kill("SIGKILL");
