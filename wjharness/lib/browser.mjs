// Shared Chrome-launch + CDP plumbing for the wj tools.
//
// Factored out of the six near-identical copies that used to live in scratchpad/*.mjs. Two things here
// are load-bearing rather than tidy-up:
//
//   * ONE FRESH BROWSER PER MEASUREMENT. Every extra tab keeps a 16-thread pthread pool alive, and
//     vite's HMR reloads all of them on any frontend edit. Measured that way teavm read 1.004 and IKVM
//     3.975 ms/step; clean they are 0.426 and 2.298 -- so even the RATIO moved, from 4x to 5.4x.
//     Contention is also indistinguishable from a hang: a run declared deadlocked for six minutes
//     finished the instant the other tabs were closed.
//   * The RPC helper resolves the result PAYLOAD and surfaces errors as an `__error` sentinel, and
//     rejects everything still pending when the socket closes. The other convention in the old scripts
//     resolved the whole envelope and dropped errors on the floor, so a failed call became an
//     `undefined` deref several lines later.
//
// Dependency-free: Node's global WebSocket (undici), no npm, no puppeteer.
import { spawn } from "child_process";
import { mkdtempSync, rmSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import "./paths.mjs";   // exports the resolved configuration (CHROME_BIN, ...) into process.env

export function launchChrome({ extraArgs = [], headless = true, gl = true } = {}) {
  const dir = mkdtempSync(join(tmpdir(), "wj-"));
  const args = [
    ...(headless ? ["--headless=new"] : []),
    "--remote-debugging-port=0",          // let the OS pick; we scrape the real one off stderr
    `--user-data-dir=${dir}`,
    "--no-first-run", "--no-default-browser-check",
    "--disable-dev-shm-usage",
    // The dev server is https with a self-signed cert. Without this the renderer parks on the
    // interstitial forever, which looks exactly like a hang.
    "--ignore-certificate-errors",
    /* 2026-10-03: brought in line with mcdrive.mjs launchGame, which kept working while this launcher rotted -- the
     * jbox2d page never got past its first fetch here (a fresh profile with the box's managed extensions loaded and
     * `--use-gl=swiftshader`, which current chromium no longer honours), while the same URL ran in a plain incognito
     * window. Extensions off, real OPFS quota, GPU through ANGLE (MC_SOFTWARE_GL=1 for SwiftShader), no sandbox, no
     * background throttling -- launchGame's comments carry the reason for each. */
    "--no-sandbox", "--unlimited-storage",
    "--disable-extensions", "--disable-component-extensions-with-background-pages",
    "--disable-sync", "--disable-background-networking", "--disable-client-side-phishing-detection",
    "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
    "--disable-backgrounding-occluded-windows",
    "--ignore-gpu-blocklist",
    ...(gl ? (process.env.MC_SOFTWARE_GL === "1"
      ? ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
      : ["--use-gl=angle", `--use-angle=${process.env.MC_ANGLE || "gl"}`]) : []),
    "--js-flags=--stack-trace-limit=64",
    ...extraArgs,
    "about:blank",
  ];
  // WAYLAND_DISPLAY is set in this shell; chromium would otherwise auto-select the Wayland backend and ignore the
  // headless/X selection (mcdrive.mjs launchGame, mcsr/chrome.sh).
  const env = { ...process.env };
  delete env.WAYLAND_DISPLAY;
  delete env.XDG_SESSION_TYPE;
  const proc = spawn(process.env.CHROME_BIN || "/usr/bin/chromium", args, { stdio: ["ignore", "ignore", "pipe"], env });
  let buf = "";
  const wsUrl = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error("timed out waiting for the CDP endpoint")), 30000);
    proc.stderr.on("data", (d) => {
      buf += d.toString();
      const m = buf.match(/ws:\/\/[^\s]+/);
      if (m) { clearTimeout(t); resolve(m[0]); }
    });
    proc.on("exit", (c) => { clearTimeout(t); reject(new Error(`chrome exited early (${c}): ${buf.slice(-400)}`)); });
  });
  const kill = () => {
    try { proc.kill("SIGKILL"); } catch { }
    try { rmSync(dir, { recursive: true, force: true }); } catch { }
  };
  return { wsUrl, kill, proc };
}

export async function connect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = (e) => rej(new Error(`ws: ${e && e.message}`)); });

  let id = 0;
  const pending = new Map();
  const listeners = [];

  ws.addEventListener("message", (ev) => {
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.id && pending.has(m.id)) {
      pending.get(m.id)(m.error ? { __error: m.error } : (m.result || {}));
      pending.delete(m.id);
      return;
    }
    for (const fn of listeners) fn(m);
  });
  // Nothing in the old scripts rejected pendings on close, so a dead session left an await hanging
  // forever and only the outer wall-clock loop saved it.
  ws.addEventListener("close", () => {
    for (const res of pending.values()) res({ __error: { message: "socket closed" } });
    pending.clear();
  });

  const send = (method, params = {}, sessionId) => new Promise((res) => {
    const mid = ++id; pending.set(mid, res);
    ws.send(JSON.stringify({ id: mid, method, params, ...(sessionId ? { sessionId } : {}) }));
  });

  return { send, on: (fn) => listeners.push(fn), close: () => ws.close() };
}

/** Attach to the first page target and return its sessionId. */
export async function attachPage(conn) {
  const { targetInfos } = await conn.send("Target.getTargets");
  const page = (targetInfos || []).find((t) => t.type === "page");
  if (!page) throw new Error("no page target");
  const { sessionId } = await conn.send("Target.attachToTarget", { targetId: page.targetId, flatten: true });
  return sessionId;
}

/**
 * Collect console output from a session. Uses `a.value ?? a.description ?? ''` -- the `?? ''` variant in
 * some of the old scripts silently dropped every non-primitive argument.
 *
 * dotnet's loader proxies worker console.* onto the main thread (teardown_proxy_console in
 * loader/exit.ts), so page-session console is enough to see output from all 16 pthread workers.
 */
export function consoleTap(conn, sessionId, onLine) {
  conn.on((m) => {
    if (m.method !== "Runtime.consoleAPICalled" || m.sessionId !== sessionId) return;
    const text = (m.params.args || []).map((a) => a.value ?? a.description ?? "").join(" ");
    onLine(text);
  });
}
