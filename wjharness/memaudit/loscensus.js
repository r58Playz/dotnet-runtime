/* LOS allocation census, evaluated IN THE PAGE over CDP, repeatedly (R510). Returns a JSON string.
 *
 * sgen frees a large object only in a major collection's LOS sweep, so between two majors the LOS object list only
 * grows: diffing successive snapshots of sgen_los_object_array_list gives every large allocation since the previous
 * snapshot (missing only an object both allocated and swept between two snapshots that straddle a major). State lives in
 * globalThis.__losState across calls. Each new object is keyed by its class name plus, for a byte[], what its first bytes
 * are (a class file, a PNG, a zip entry ...) and, for a reference array, the class of its first non-null element.
 * Layouts (wasm32): LOSObject {size, mod_union} then the object; MonoArray {vtable, sync, bounds, max_length @12, data @16}.
 * Unlocked, like heapcensus.js: a few objects can be misread mid-allocation; `bad` counts them.
 */
(function loscensus(ADDR) {
  const t0 = performance.now();
  const M = globalThis.wasm.Module, buf = (M.wasmMemory?.buffer ?? globalThis.getDotnetRuntime(0).localHeapViewU8().buffer);
  const u32 = new Uint32Array(buf), u8 = new Uint8Array(buf);
  const R = (a) => u32[a >>> 2], memEnd = buf.byteLength;
  const dec = new TextDecoder();
  const S = globalThis.__losState || (globalThis.__losState = { prev: new Map(), names: new Map(), snaps: 0 });
  const cstr = (a) => { if (!a || a >= memEnd) return null; let e = a; while (e < a + 300 && u8[e]) e++; if (e >= a + 300) return null; if (e === a) return "";
    for (let i = a; i < e; i++) if (u8[i] < 0x20 || u8[i] > 0x7e) return null; return dec.decode(u8.slice(a, e)); };
  const nameOf = (vt) => {
    let n = S.names.get(vt);
    if (n !== undefined) return n;
    n = null;
    if (vt > 4096 && vt < memEnd - 64) {
      const k = R(vt);
      if (k > 4096 && k < memEnd - 0x40) {
        const nm = cstr(R(k + 0x28)), ns = R(k + 0x2c) ? cstr(R(k + 0x2c)) : "";
        if (nm && ns !== null) { n = ns ? ns + "." + nm : nm; const outer = R(k + 0x20); if (!ns && outer > 4096 && outer < memEnd - 0x40) { const on = cstr(R(outer + 0x28)), ons = cstr(R(outer + 0x2c)); if (on) n = (ons ? ons + "." : "") + on + "/" + nm; } }
      }
    }
    S.names.set(vt, n);
    return n;
  };
  const listEach = (L, fn) => {
    const next = R(L + 136);
    for (let i = 0; i < next; i++) {
      const b = 31 - Math.clz32(i + 32) - 5, off = i - (1 << (b + 5)) + 32;
      const ent = R(L + 4 * b);
      if (!ent) continue;
      const v = R(ent + 4 * off);
      if (v) fn(v);
    }
  };
  const hex = (a, n) => { let s = ""; for (let i = 0; i < n; i++) s += u8[a + i].toString(16).padStart(2, "0"); return s; };
  const magic = (d) => {
    const h = hex(d, 4);
    if (h === "cafebabe") return "classfile";
    if (h === "89504e47") return "png";
    if (h === "504b0304") return "zip-local";
    if (h === "504b0102") return "zip-central";
    if (h.startsWith("1f8b")) return "gzip";
    if (h.startsWith("78")) return "zlib?";
    let z = true; for (let i = 0; i < 64; i++) if (u8[d + i]) { z = false; break; }
    if (z) return "zeros64";
    let txt = true; for (let i = 0; i < 32; i++) { const c = u8[d + i]; if (c < 9 || (c > 13 && c < 32) || c > 126) { txt = false; break; } }
    return txt ? "text" : "bin:" + h;
  };
  const cur = new Map();
  let bad = 0, losN = 0, losBytes = 0;
  listEach(ADDR.sgen_los_object_array_list, (v) => {
    const lo = (v & ~1) >>> 0, sz = (R(lo) & ~1) >>> 0, vt = (R(lo + 8) & ~7) >>> 0;
    if (sz < 8000 || lo + sz > memEnd) { bad++; return; }
    losN++; losBytes += sz;
    cur.set(lo, sz + ":" + vt);
  });
  const add = new Map(), gone = new Map(), bySize = {};
  let nNew = 0, bNew = 0, nGone = 0, bGone = 0;
  const bump = (m, k, sz) => { let t = m.get(k); if (!t) m.set(k, t = [0, 0]); t[0]++; t[1] += sz; };
  const first = S.snaps === 0;
  for (const [lo, key] of cur) {
    if (S.prev.get(lo) === key) continue;
    const [sz, vt] = key.split(":").map(Number), obj = lo + 8;
    let nm = nameOf(vt) || "?", tag = "";
    if (nm === "?") bad++;
    else if (nm === "System.Byte[]" || nm === "System.SByte[]") tag = magic(obj + 16);
    else if (nm.endsWith("[]") && !/^System\.(Int|UInt|Single|Double|Char|Boolean|Int64|UInt64|Int16|UInt16)/.test(nm)) {
      const n = R(obj + 12), lim = Math.min(n, 64);
      for (let i = 0; i < lim; i++) { const e = R(obj + 16 + 4 * i); if (e) { tag = "elem " + (nameOf((R(e) & ~7) >>> 0) || "?"); break; } }
      if (!tag) tag = "elem null*" + lim;
    }
    nNew++; bNew += sz;
    bump(add, nm + (tag ? " | " + tag : ""), sz);
    const b = sz < 16384 ? "8-16K" : sz < 65536 ? "16-64K" : sz < 262144 ? "64-256K" : sz < 1048576 ? "256K-1M" : sz < 4194304 ? "1-4M" : ">=4M";
    const e = bySize[b] || (bySize[b] = [0, 0]); e[0]++; e[1] += sz;
  }
  for (const [lo, key] of S.prev) {
    if (cur.get(lo) === key) continue;
    const [sz, vt] = key.split(":").map(Number);
    nGone++; bGone += sz;
    bump(gone, nameOf(vt) || "?", sz);
  }
  S.prev = cur; S.snaps++;
  const top = (m, n) => [...m.entries()].sort((a, b) => b[1][1] - a[1][1]).slice(0, n).map(([k, v]) => [k, v[0], v[1]]);
  return JSON.stringify({
    t: Date.now(), first, ms: +(performance.now() - t0).toFixed(1), majors: R(ADDR.mono_gc_stats + 4), minors: R(ADDR.mono_gc_stats),
    losN, losBytes, losUsage: R(ADDR.sgen_los_memory_usage), losUsageTotal: R(ADDR.sgen_los_memory_usage_total),
    losSections: R(ADDR.los_num_sections), majorSections: R(ADDR.num_major_sections), sbrk: R(ADDR.sbrk_val), wasm: memEnd,
    nNew, bNew, nGone, bGone, bad, bySize, add: top(add, 60), gone: top(gone, 15) });
})
