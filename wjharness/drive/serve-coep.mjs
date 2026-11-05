// Minimal static file server with COOP/COEP headers.
//
// Exists for the teavm baseline row: the wasm-gc benchmark ships as build/libs/benchmark.war, and the
// recorded 0.426 ms/step was taken by unzipping it (minus WEB-INF) and serving it this way on 8099.
//
//   cd $TEAVM_DIR/samples/benchmark && unzip -q -o build/libs/benchmark.war -x 'WEB-INF/*' -d /tmp/teavmweb
//   wj serve-coep /tmp/teavmweb 8099
//
// <dir> may be several directories joined with ':' -- a path is served from the first that has it. j2d/run.mjs uses
// that to serve the tracked j2d/cheerpj/index.html over the untracked jars in state/arms/cheerpj.
//
// teavm wasm-gc does not itself need cross-origin isolation (no threads, no SharedArrayBuffer), but
// serving every baseline under the same headers removes one more difference between the rows.
import { createServer } from "http";
import { createReadStream, statSync } from "fs";
import { join, normalize, extname } from "path";

const root = process.argv[2];
const port = Number(process.argv[3] || 8099);
if (!root) { console.error("usage: serve-coep.mjs <dir>[:<dir>...] [port]"); process.exit(1); }
const roots = root.split(":").filter(Boolean);

const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".wasm": "application/wasm", ".json": "application/json", ".css": "text/css",
  ".map": "application/json", ".txt": "text/plain", ".teadbg": "application/octet-stream",
  ".jar": "application/java-archive",
};

createServer((req, res) => {
  // Strip the query and refuse to escape the root.
  const rel = normalize(decodeURIComponent(req.url.split("?")[0])).replace(/^(\.\.[/\\])+/, "");
  let path, size;
  for (const r of roots) {
    let p = join(r, rel);
    try { if (statSync(p).isDirectory()) p = join(p, "index.html"); } catch { }
    try { size = statSync(p).size; path = p; break; } catch { }
  }
  if (path === undefined) {
    res.writeHead(404, { "content-type": "text/plain" });
    res.end(`not found: ${rel}\n`);
    return;
  }

  const hdr = {
    "content-type": TYPES[extname(path)] || "application/octet-stream",
    "cross-origin-opener-policy": "same-origin",
    "cross-origin-embedder-policy": "require-corp",
    "cross-origin-resource-policy": "cross-origin",
    "cache-control": "no-store",
    "accept-ranges": "bytes",
  };
  // Range requests: CheerpJ (j2d/cheerpj) reads jars by byte range and refuses a server without them.
  const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || "");
  if (m && (m[1] || m[2])) {
    let start = m[1] ? Number(m[1]) : Math.max(0, size - Number(m[2]));
    let end = m[1] && m[2] ? Math.min(Number(m[2]), size - 1) : size - 1;
    if (start >= size || start > end) {
      res.writeHead(416, { ...hdr, "content-range": `bytes */${size}` }); res.end(); return;
    }
    res.writeHead(206, { ...hdr, "content-length": end - start + 1, "content-range": `bytes ${start}-${end}/${size}` });
    createReadStream(path, { start, end }).pipe(res);
    return;
  }
  res.writeHead(200, { ...hdr, "content-length": size });
  createReadStream(path).pipe(res);
}).listen(port, () => console.log(`serving ${root} on http://localhost:${port}/ (COOP/COEP)`));
