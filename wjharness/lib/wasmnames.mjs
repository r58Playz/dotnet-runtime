// Extract the function names our emitter wrote into a module's name section.
//
// Why this exists: pattern.mjs filters profile symbols by /turbofan|liftoff/, which is EVERY wasm
// function V8 compiled -- our JIT'd modules AND dotnet.native.wasm (AOT'd app code + the C runtime).
// Shares computed over that mixture answer "where does time go in wasm", not "what did our emitter
// emit", and the two differ enough to invert conclusions (the sgen write-barrier fence looked like an
// emitter problem until it turned out to be in AOT'd code). Dumping our modules and reading their name
// sections gives an exact membership set, with no reliance on name FORMAT -- the AOT image names methods
// the same way, so only provenance distinguishes them.
//
// Name section layout: a custom section (id 0) whose name is "name", containing subsections;
// subsection 1 is the function-name map (count, then (funcidx, name) pairs).
import { readFileSync, readdirSync } from "fs";
import { join } from "path";

const uleb = (b, p) => {
  let r = 0, s = 0, x;
  do { x = b[p++]; r |= (x & 0x7f) << s; s += 7; } while (x & 0x80);
  return [r >>> 0, p];
};

export function moduleFunctionNames (buf) {
  const out = [];
  if (buf.length < 8 || buf.readUInt32LE(0) !== 0x6d736100) return out;   // "\0asm"
  let p = 8;
  while (p < buf.length) {
    const id = buf[p++];
    let len; [len, p] = uleb(buf, p);
    const end = p + len;
    if (id === 0) {
      let nlen, q = p; [nlen, q] = uleb(buf, q);
      if (buf.toString("latin1", q, q + nlen) === "name") {
        q += nlen;
        while (q < end) {
          const sub = buf[q++];
          let slen; [slen, q] = uleb(buf, q);
          const sEnd = q + slen;
          if (sub === 1) {
            let n, r = q; [n, r] = uleb(buf, r);
            for (let i = 0; i < n && r < sEnd; i++) {
              let idx, nm; [idx, r] = uleb(buf, r); [nm, r] = uleb(buf, r);
              out.push(buf.toString("utf8", r, r + nm));
              r += nm;
            }
          }
          q = sEnd;
        }
      }
    }
    p = end;
  }
  return out;
}

/* Every function name across a dump directory. Returns a Set -- the caller tests profile symbols for
 * membership, so duplicates across batched modules collapse harmlessly. */
export function dumpDirNames (dir) {
  const names = new Set();
  let mods = 0;
  for (const f of readdirSync(dir)) {
    if (!f.endsWith(".wasm")) continue;
    mods++;
    for (const n of moduleFunctionNames(readFileSync(join(dir, f)))) names.add(n);
  }
  return { names, mods };
}
