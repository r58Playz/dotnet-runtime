// Read PartitionAlloc's per-root counters from a Chromium renderer that this process launched.
// /proc/<pid>/mem requires an ancestor relationship under Yama ptrace_scope=1. No browser symbols
// are needed to enumerate roots: inverted_self_ validates each object, and next_root/prev_root
// enumerate the rest (partition_root.h). Names use checked addresses from one installed build.
import { closeSync, openSync, readFileSync, readSync } from 'node:fs';
import { execFileSync } from 'node:child_process';

const COUNTERS = 0x15d0n; // Six adjacent size_t atomics; validate before use.
const MASK64 = 0xffffffffffffffffn;
const PAGE = 4096n;
const MIB = 1048576n;
const GIB = 1073741824n;
const cache = new Map();
const symbolBuildCache = new Map();
const CHROMIUM_153_BUILD_ID = '9edff2492605d22e9fdebd338b9713367b83286b';

function read (fd, addr, n) {
  if (addr < 0n) return null;
  const buf = Buffer.allocUnsafe(n);
  try {
    return readSync(fd, buf, { position: addr, offset: 0, length: n }) === n ? buf : null;
  } catch { return null; }
}

function counters (fd, root) {
  const buf = read(fd, root + COUNTERS, 48);
  if (!buf) return null;
  const v = Array.from({ length: 6 }, (_, i) => buf.readBigUInt64LE(i * 8));
  const [c, mc, sp, dm, a, ma] = v;
  // A direct-map-only root has no superpages. Keep its virtual counters. In the
  // Minecraft renderer the malloc root's first two words read in the UINT64_MAX
  // range while the remaining four are plausible: the offset is right (DumpStats
  // reads 0x15d0..0x15f8 in this build) and the counter itself has wrapped
  // negative (R486). Preserve the virtual sizes but mark its committed
  // accounting unusable instead of hiding the root.
  const virtual = sp + dm;
  if ((!sp && !dm) || sp % (2n * MIB) !== 0n || sp > 16n * GIB || dm > 16n * GIB ||
      a > virtual + 2n * MIB || ma < a || ma > 16n * GIB) return null;
  const committedValid = c <= virtual + 2n * MIB && mc >= c && mc <= 16n * GIB && ma <= mc;
  return {
    committed: committedValid ? Number(c) : null,
    maxCommitted: committedValid ? Number(mc) : null,
    superPages: Number(sp), directMapped: Number(dm),
    allocated: Number(a), maxAllocated: Number(ma),
    counterStatus: committedValid ? 'valid' : 'committed-invalid',
    ...(committedValid ? {} : { rawCommittedHex: '0x' + c.toString(16),
      rawMaxCommittedHex: '0x' + mc.toString(16) }),
  };
}

function isRoot (fd, root, selfOff) {
  const b = read(fd, root + selfOff, 8);
  // The list also contains roots with no allocations. Requiring nonzero size counters here
  // would terminate traversal at an empty root and hide every root after it.
  return !!b && b.readBigUInt64LE(0) === ((~root) & MASK64);
}

function paMaps (maps) {
  const ranges = [];
  let charged = 0;
  for (const line of maps.split('\n')) {
    if (!line.includes('[anon:partition_alloc]')) continue;
    const m = /^([0-9a-f]+)-([0-9a-f]+) (....)/.exec(line);
    if (!m) continue;
    const start = BigInt('0x' + m[1]), end = BigInt('0x' + m[2]);
    if (m[3][1] === 'w' && m[3][3] === 'p') charged += Number(end - start);
    if (m[3] === 'rw-p' && end - start <= 16384n) ranges.push({ start, end });
  }
  return { ranges, charged };
}

function findRootNear (fd, p) {
  const page = p & ~(PAGE - 1n);
  for (let at = page - 0x10000n; at <= page + 0x10000n; at += PAGE) {
    const b = read(fd, at, Number(PAGE));
    if (!b) continue;
    for (let i = 0; i < b.length; i += 8) {
      const root = (~b.readBigUInt64LE(i)) & MASK64;
      const selfOff = at + BigInt(i) - root;
      if (root % 64n !== 0n || root > p || p - root > 0x10000n ||
          selfOff < 0x1000n || selfOff > 0x10000n) continue;
      if (counters(fd, root)) return { root, selfOff };
    }
  }
  return null;
}

function findSeeds (fd, ranges) {
  const visitedPages = new Set();
  const seeds = new Map();
  for (const { start, end } of ranges.slice(0, 512)) {
    const b = read(fd, start, Number(end - start));
    if (!b) continue;
    for (let i = 0; i + 8 <= b.length; i += 8) {
      const p = b.readBigUInt64LE(i);
      if (p < 0x100000000n || p > 0x800000000000n || p % 8n) continue;
      const page = p & ~(PAGE - 1n);
      if (visitedPages.has(page)) continue;
      visitedPages.add(page);
      const root = findRootNear(fd, p);
      if (root) seeds.set(root.root, root);
      if (visitedPages.size >= 256 || seeds.size >= 32) return [...seeds.values()];
    }
  }
  return [...seeds.values()];
}

function findStaticSeeds (fd, maps) {
  const rows = maps.split('\n').map(line => {
    const m = /^([0-9a-f]+)-([0-9a-f]+)\s+([rwxps-]{4})\s+[0-9a-f]+\s+\S+\s+\d+(?:\s+(.*))?$/.exec(line);
    return m ? { start: BigInt('0x' + m[1]), end: BigInt('0x' + m[2]),
      perms: m[3], path: m[4] || '' } : null;
  }).filter(Boolean);
  const lastImage = rows.findLastIndex(r => r.path.endsWith('/chromium'));
  if (lastImage < 0) return [];
  const base = rows[lastImage].end;
  const seeds = new Map();
  let end = base;
  // Chromium's static PartitionRoots live in the anonymous BSS extension immediately
  // after its mapped data. Read at most 16 MiB; the following [heap] can be 512 MiB.
  for (const r of rows.slice(lastImage + 1)) {
    if (r.start !== end || r.perms !== 'rw-p' || r.path || r.end - base > 16n * MIB) break;
    for (let at = r.start; at < r.end; at += PAGE) {
      const b = read(fd, at, Number(at + PAGE > r.end ? r.end - at : PAGE));
      if (!b) continue;
      for (let i = 0; i + 8 <= b.length; i += 8) {
        const root = (~b.readBigUInt64LE(i)) & MASK64;
        const off = at + BigInt(i) - root;
        if (root >= base && root < r.end && root % 64n === 0n &&
            off >= 0x1000n && off <= 0x10000n && isRoot(fd, root, off))
          seeds.set(root, { root, selfOff: off });
      }
    }
    end = r.end;
  }
  return [...seeds.values()];
}

function linkedRoots (fd, seed, off) {
  const seen = new Set([seed.root]);
  for (const direction of [off, off - 8n]) {
    let root = seed.root;
    for (let i = 0; i < 256; i++) {
      const b = read(fd, root + direction, 8);
      if (!b) break;
      const next = b.readBigUInt64LE(0);
      if (!next || seen.has(next) || !isRoot(fd, next, seed.selfOff)) break;
      seen.add(next);
      root = next;
    }
  }
  return [...seen];
}

function discover (fd, maps) {
  const staticSeeds = findStaticSeeds(fd, maps);
  const seeds = staticSeeds.length ? staticSeeds : findSeeds(fd, paMaps(maps).ranges);
  if (!seeds.length) return null;
  const groups = [];
  const covered = new Set();
  for (const seed of seeds) {
    if (covered.has(seed.root)) continue;
    let best = [seed.root], bestOff = null;
    // A renderer can have roots outside the first root's list. Validate each discovered
    // component and retain the union; a first-seed-only walk missed GiBs in Minecraft.
    for (let off = seed.selfOff + 8n; off < seed.selfOff + 0x1000n; off += 8n) {
      const roots = linkedRoots(fd, seed, off);
      if (roots.length > best.length) { best = roots; bestOff = off; }
    }
    groups.push({ seed: seed.root, selfOff: seed.selfOff, nextOff: bestOff });
    for (const root of best) covered.add(root);
  }
  return groups;
}

function nameRoots (fd, maps, roots) {
  const image = maps.split('\n').map(line =>
    /^([0-9a-f]+)-[0-9a-f]+\s+[rwxps-]{4}\s+00000000\s+\S+\s+\d+\s+(\/\S*\/chromium)$/.exec(line)
  ).find(Boolean);
  if (!image) return null;
  const path = image[2];
  if (!symbolBuildCache.has(path)) {
    let ok = false;
    try { ok = execFileSync('readelf', ['-n', path], { encoding: 'utf8', timeout: 2000 })
      .includes(`Build ID: ${CHROMIUM_153_BUILD_ID}`); } catch { }
    symbolBuildCache.set(path, ok);
  }
  if (!symbolBuildCache.get(path)) return null;
  const base = BigInt('0x' + image[1]);
  const known = new Set(roots.map(r => r.addr));
  const names = {};
  const pointer = (off) => read(fd, base + off, 8)?.readBigUInt64LE(0);
  for (const [name, off] of [['Blink BufferPartition', 0x13765660n],
                              ['Blink ArrayBufferPartition', 0x13765658n]]) {
    const p = pointer(off);
    if (p && known.has('0x' + p.toString(16))) names['0x' + p.toString(16)] = name;
  }
  for (const [name, off] of [['malloc-shim configured main', 0x135ccac0n],
                              ['malloc-shim original', 0x135d03c0n]]) {
    const addr = '0x' + (base + off).toString(16);
    if (known.has(addr)) names[addr] = name;
  }
  return names;
}

export function readPartitionStats (pid, mapsText = null) {
  let maps;
  try { maps = mapsText ?? readFileSync(`/proc/${pid}/maps`, 'utf8'); }
  catch (e) { return { __error: `maps: ${e.code || e.message}` }; }
  const { charged } = paMaps(maps);
  let fd;
  try { fd = openSync(`/proc/${pid}/mem`, 'r'); }
  catch (e) { return { __error: `mem: ${e.code || e.message}`, __mapCharged: charged }; }
  try {
    let layout = cache.get(pid);
    if (!layout || !layout.every(g => isRoot(fd, g.seed, g.selfOff))) {
      layout = discover(fd, maps);
      if (layout) { layout.scannedCharged = charged; cache.set(pid, layout); }
    }
    if (!layout) return { __error: 'no validated PartitionRoot', __mapCharged: charged };
    let pointers = [];
    const collect = (groups) => {
      pointers = [...new Set(groups.flatMap(g => g.nextOff === null ? [g.seed] :
        linkedRoots(fd, { root: g.seed, selfOff: g.selfOff }, g.nextOff)))];
      return pointers.map(root => {
        const c = counters(fd, root);
        return c ? { addr: '0x' + root.toString(16), ...c } : null;
      }).filter(Boolean);
    };
    let all = collect(layout);
    let virtual = all.reduce((n, r) => n + r.superPages + r.directMapped, 0);
    const priorScan = layout.scannedCharged || 0;
    if (charged - virtual > Math.max(64 * Number(MIB), charged * 0.2) &&
        charged > priorScan + 128 * Number(MIB)) {
      const refreshed = discover(fd, maps);
      if (refreshed) {
        refreshed.scannedCharged = charged;
        layout = refreshed;
        cache.set(pid, layout);
        all = collect(layout);
        virtual = all.reduce((n, r) => n + r.superPages + r.directMapped, 0);
      }
    }
    const committed = all.reduce((n, r) => n + (r.committed || 0), 0);
    const names = nameRoots(fd, maps, all);
    if (names) for (const root of all) root.name = names[root.addr] || null;
    return {
      __all: all,
      __allTotalCommitted: committed,
      __allTotalMaxCommitted: all.reduce((n, r) => n + (r.maxCommitted || 0), 0),
      __mapCharged: charged,
      __mapMinusRootCommitted: charged - committed,
      __allTotalVirtual: virtual,
      __mapMinusRootVirtual: charged - virtual,
      __enumeratedRoots: pointers.length,
      __rejectedRoots: pointers.filter(root => !counters(fd, root)).map(root => {
        const b = read(fd, root + COUNTERS, 48);
        return { addr: '0x' + root.toString(16), raw: b ? Array.from({ length: 6 }, (_, i) => Number(b.readBigUInt64LE(i * 8))) : null };
      }),
      __layout: layout.map(g => ({ selfOffset: '0x' + g.selfOff.toString(16),
        nextOffset: g.nextOff === null ? null : '0x' + g.nextOff.toString(16) })),
      __nameSource: names ? `Chromium build ${CHROMIUM_153_BUILD_ID}` : null,
    };
  } finally { closeSync(fd); }
}
