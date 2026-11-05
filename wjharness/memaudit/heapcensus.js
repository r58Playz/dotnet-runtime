/* Managed-heap census by type, evaluated IN THE PAGE over CDP (R482). Returns a JSON string.
 *
 * Walks sgen's major heap (the allocated_blocks SgenArrayList: 16 KiB blocks, MSBlockInfo header, objects from
 * MS_BLOCK_SKIP = 288 at the block's obj_size stride) and the large-object space (sgen_los_object_array_list ->
 * LOSObject {size, mod_union, data}), resolving each object's vtable -> MonoClass -> namespace.name. The nursery is
 * skipped. A slot whose first word is 0 or points into its own block is a free-list link. Unlocked: sgen and the
 * mutators run during the walk, so a few objects can be misread; `bad` counts vtables that did not resolve.
 *
 * Dead objects are counted until their block is swept; the driver runs this right after a major collection, once
 * sweep_state reads SWEPT (0), so the major-heap totals approximate the live set.
 */
(function heapcensus(ADDR) {
  const t0 = performance.now();
  const M = globalThis.wasm.Module, buf = M.wasmMemory.buffer;
  const u32 = new Uint32Array(buf), u16 = new Uint16Array(buf), u8 = new Uint8Array(buf);
  const R = (a) => u32[a >>> 2], memEnd = buf.byteLength;
  const BS = R(ADDR.ms_block_size) || 16384, SKIP = 288;
  const dec = new TextDecoder();
  const cstr = (a) => { if (!a || a >= memEnd) return null; let e = a; while (e < a + 300 && u8[e]) e++; if (e >= a + 300) return null; if (e === a) return "";   // "" is valid: nested types have an empty namespace
    for (let i = a; i < e; i++) if (u8[i] < 0x20 || u8[i] > 0x7e) return null; return dec.decode(u8.slice(a, e)); };
  const names = new Map();
  const nameOf = (vt) => {
    let n = names.get(vt);
    if (n !== undefined) return n;
    n = null;
    if (vt > 4096 && vt < memEnd - 64) {
      const k = R(vt);
      if (k > 4096 && k < memEnd - 0x40) {
        const nm = cstr(R(k + 0x28)), ns = R(k + 0x2c) ? cstr(R(k + 0x2c)) : "";
        if (nm && ns !== null) { n = ns ? ns + "." + nm : nm; const outer = R(k + 0x20); if (!ns && outer > 4096 && outer < memEnd - 0x40) { const on = cstr(R(outer + 0x28)), ons = cstr(R(outer + 0x2c)); if (on) n = (ons ? ons + "." : "") + on + "/" + nm; } }
      }
    }
    names.set(vt, n);
    return n;
  };
  const listEach = (L, fn) => {   // SgenArrayList: entries[32] @0, capacity @128, slot_hint @132, next_slot @136
    const next = R(L + 136);
    for (let i = 0; i < next; i++) {
      const b = 31 - Math.clz32(i + 32) - 5, off = i - (1 << (b + 5)) + 32;
      const ent = R(L + 4 * b);
      if (!ent) continue;
      const v = R(ent + 4 * off);
      if (v) fn(v);
    }
    return next;
  };
  const types = new Map();   // name -> [n, bytes, losN, losBytes]
  const bump = (nm, sz, los) => { let t = types.get(nm); if (!t) types.set(nm, t = [0, 0, 0, 0]); if (los) { t[2]++; t[3] += sz; } else { t[0]++; t[1] += sz; } };
  const o = { majorCount: R(ADDR.mono_gc_stats + 4), minorCount: R(ADDR.mono_gc_stats), sweepState: R(ADDR.sweep_state),
    blocks: 0, slots: 0, used: 0, usedBytes: 0, freeSlots: 0, bad: 0, badBytes: 0, losN: 0, losBytes: 0, losBad: 0, bySize: {} };
  o.blockEntries = listEach(ADDR.allocated_blocks, (v) => {
    const blk = (v & ~7) >>> 0;   // >>> 0: addresses above 2 GiB must stay unsigned
    if (blk & (BS - 1)) { o.bad++; return; }
    const osz = u16[blk >>> 1];
    if (osz < 8 || osz > BS - SKIP) { o.bad++; return; }
    o.blocks++;
    const cnt = Math.floor((BS - SKIP) / osz);
    for (let i = 0; i < cnt; i++) {
      const a = blk + SKIP + i * osz, w = R(a);
      o.slots++;
      if (w === 0 || (w >= blk && w < blk + BS)) { o.freeSlots++; continue; }
      const nm = nameOf((w & ~7) >>> 0);
      if (!nm) { o.bad++; o.badBytes += osz; continue; }
      o.used++; o.usedBytes += osz; bump(nm, osz, false);
      const sc = o.bySize[osz] || (o.bySize[osz] = [0, 0]); sc[0]++; sc[1] += osz;
    }
  });
  o.losEntries = listEach(ADDR.sgen_los_object_array_list, (v) => {
    const lo = (v & ~1) >>> 0, sz = (R(lo) & ~1) >>> 0, nm = nameOf((R(lo + 8) & ~7) >>> 0);
    if (!nm || sz < 8000 || lo + sz > memEnd) { o.losBad++; return; }
    o.losN++; o.losBytes += sz + 8; bump(nm, sz + 8, true);
  });
  o.types = [...types.entries()].map(([k, v]) => [k, ...v]).sort((a, b) => (b[2] + b[4]) - (a[2] + a[4])).slice(0, 400);
  o.ntypes = types.size;
  o.ms = +(performance.now() - t0).toFixed(1);
  return JSON.stringify(o);
})
