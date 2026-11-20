# bench/ -- the benchmarks' sources and the pinned TeaVM

Everything the cross-runtime benchmarks run is built from here; nothing comes from a TeaVM checkout's samples any more
(until 2026-11-18 the jbox2d sources and `ikvm-bench` lived in `$TEAVM_DIR/samples/benchmark`). Built artifacts go to
`$WJ_STATE/arms/`; `j2d/run` drives every arm.

```
bench/
  teavm-cli.sh        builds the pinned TeaVM into $WJ_STATE/arms/teavm-cli/ (see "TeaVM" below)
  teavm.sh            runs it: wj bench/teavm -t wasm -O 3 -p <cp> ... <main class>
  teavm-patch-edit.sh edit the newest TeaVM patch in teavm-cli.sh's worktree (begin / save)
  teavm-patches/      0001.. patches to TeaVM 9be5eca57, applied in order (each file says why and what codegen it touches)
  teavm-classlib/     JDK classes TeaVM's classlib lacks, compiled into its classlib module (single-threaded
                      java.util.concurrent, FileChannel, HexFormat, MessageDigest, ...; wjteavm.Background)
  jbox2d/             the jbox2d fixed-work kernel: src/ (Scene + drivers), web/, build.sh
  mctick/             Minecraft 26.2's server tick: src/ (all arms), src-jvm/, src-launch/ (ours), src-teavm/,
                      teavm-patch/ (build-time bytecode patches), web/, build.sh, build-teavm.sh
```

## jbox2d

`wj bench/jbox2d/build [--install]` writes `$WJ_STATE/arms/j2d/`: `bench.jar` (ours, HotSpot, CheerpJ; class files 52,
byte-identical to the jar the R180-R527 rounds ran, sha256 eabfcc8c... in ikvmcraft's `public/`) and `teavm-full/`,
`teavm-simple/` (TeaVM linear-memory wasm at optimization FULL / SIMPLE, the R180 arms A / B, schedule from
`?args=W,S,N`). `--install` copies the jar to ikvmcraft and the CheerpJ arm. Run: `wj j2d/run tvfull|tvsimple|hs25|ours
<outdir> --schedule 600,200,60 --window-batches 40,60 --no-perf` (the R440 protocol). Checksum after 12,600 steps:
`-8730375734842757503` on every arm.

## mctick

The server half of Minecraft 26.2 as a fixed-work benchmark: load the 262n save (peaceful), join the save's player
through vanilla's own spawn path (`PrepareSpawnTask`) on a connection that drops packets (`NullConnection`: it answers
keep-alives and chunk-batch acks as a client does), run ticks back to back, time batches. View and simulation distance
are the seed's (12, 12). A batch's time is the whole server thread between ticks (client packets, the tick, main-thread
and chunk tasks); `tick-only` is `tickServer` alone. The checksum (game time, chunk count, entities by type) is not
stable across runs: vanilla seeds its level RNGs from `System.nanoTime`. Gate a run on the end state instead: on 262n,
`overworld 2601 chunks`, `chunk packets 637`, ~250-265 entities, and `chunk origins: 841 full chunks: 841 from the
save, 0 regenerated` with every ring's entities loaded (2026-11-19, all three arms).

Every arm does the same work only because of two rules in `BenchServer` / `Mctick.Ticker`; keep them:

* **every queued main-thread task is due** (`BenchServer.shouldRun`). A real server runs its queue in the time left
  after a tick; this loop has none and never advances `nextTickTimeNanos`, so MinecraftServer held each task until it
  was 3 ticks old. ServerLevel runs entity deserialization on the server's queue, one chunk a run, and HotSpot (like
  ours, whose IO results also come from other threads) had loaded the entities of 197 of 841 full chunks after 600
  ticks, ~100 entities instead of ~250. On TeaVM every executor is the server thread, so `server.execute` ran inline
  and it loaded everything: the arms were measuring different worlds.
* **between ticks, drain until quiet** (TeaVM: background queue and server tasks alternate until the queue is empty,
  at most 64 rounds; `ticks whose between-tick drain hit its cap` should read 0 after loading). One pass per tick left
  every background -> main-thread hop to the next tick, and chunk loading took ~3x the ticks.

`wj bench/mctick/build [--install] [--no-teavm]` writes `$WJ_STATE/arms/mctick/`: `cp-jvm.txt` (HotSpot / CoreCLR:
client.jar + the vanilla libraries from the native stage `sa262n`), `mctick.jar` (ours: `?bench&jar=mctick.jar` on a copy
of the 262n seed, whose OPFS holds 26.2 and the save; `Launch` builds the class path there) and `teavm-full/`
(FULL); `wj bench/mctick/build-teavm -O 1` adds `teavm-simple/` (SIMPLE, the `tvsimple` arm), and
`python3 bench/mctick/split-teavm.py <a tvfull set-A, -D or -L run> [--inline-runtime] [--sp-global] [--mode M]` puts
TeaVM's code in OUR call layout and co-locates it (run any output with `wj j2d/run tvsplit --tv-dir DIR`): every Java
function the run executed in a module of its own (`--mode function`) or grouped (`class`, `edges`, `classedges`, `hot`,
`one`; `--cap` bytes), TeaVM's allocStack / type checks inlined into them (`--inline-runtime`), its stack pointer an
imported mutable global like our `s.p` (`--sp-global`). `wasmrw.py` is the rewriter it uses (decode, inline, move a
memory word into a global, install split functions in table 0); `lbredges.py` reads a set-L run (`--events L`: the
branch stack filtered to calls) into the edge and heat profiles `edges` / `classedges` / `hot` need.
`--js-flags-extra=--no-wasm-inlining` on `tvfull` turns V8's inliner off.
Run: `wj j2d/run ours|hs25|tvfull|tvsimple|tvsplit|clrrelink|clrclassic|clr <outdir> --bench mctick [--schedule W,S,N]` (default
2000,100,20, window 10,20). The `clr*` arms are IKVM on native CoreCLR 10 with the same `cp-jvm.txt` (`$WJ_STATE/arms/
ikvm-clr10*`, see their READMEs): `clrrelink` is our IKVM configuration with a MonoMod relink backend, `clrclassic`
eager linking, `clr` our switches with generation 1 kept; they boot in ~3.5 min (join), so budget the timeout.

### The TeaVM arm

Same `src/` as the JVM arms, plus:

* **one thread, no coroutines.** TeaVM's linear-memory backend runs threads as coroutines and coroutine-transforms every
  method that can reach a suspending one. Minecraft reached them through `BlockableEventLoop.waitForTasks`'s
  `Thread.yield`, `MinecraftServer.halt`'s `Thread.join` and a debug server's `Thread.sleep`: 94,471 methods would have
  been transformed. So executors post to one queue (`wjteavm.Background`, run between ticks by `Platform.drainBackground`
  where the JVM arms' pool threads run the same tasks concurrently), every blocking wait runs queued work until its
  condition holds, and `teavm-patch/PatchClasses.java` rewrites every `Thread.yield/sleep/join` and `Object.wait` in
  client.jar and the libraries into calls to that queue. TeaVM's `-Dwj.asyncReport` (patch 0004) must then report only
  `Object.monitorEnterWait`, which every single-threaded TeaVM program with `synchronized` has.
* **files and resources** come from `mctick-data.zip` (client.jar's `data/`, the lang files, `version.json`, the save),
  which the page hands to the module and `Platform.init` unpacks into TeaVM's in-memory file system; class-path
  resources are served from `/res/` (patch 0003).
* **shadows** (`src-teavm/`, ahead of client.jar on TeaVM's class path): a console slf4j logger (no log4j), netty's
  `PlatformDependent` / `RefCnt` / `TypeParameterMatcher` / `VarHandleByteBufferAccess` / `ByteBufUtil` allocator
  (netty's own sources, Apache 2.0, with the Unsafe / VarHandle / generic-reflection paths TeaVM cannot link replaced),
  OSHI's `SystemInfo`, an inert `jar` file system provider. Minecraft's own classes are changed only by bytecode patches
  at build time (`PatchClasses`, which documents each), so no Mojang code is in this repository.
* **heap**: `--max-heap 1600` MB. TeaVM's memory layout is int arithmetic and must fit in 2 GiB with the GC's tables.

## TeaVM

`teavm-cli.sh` pins **9be5eca57** (2026-02-24): TeaVM removed its linear-memory wasm backend in the next commit,
d83fab4ae ("Remove legacy Wasm and WASI backends"), so master and every release from 0.14.0 emit only wasm-gc, JavaScript
and C, and this pin is the newest linear-memory TeaVM there is. The build runs in a worktree of `$TEAVM_DIR` under
`$WJ_RUNS/.tmp/teavm-cli-src`, applies `teavm-patches/*.patch`, copies `teavm-classlib/` into the classlib module,
and repairs three defects of the shaded CLI jar at the pin (see the script). `BUILD.txt` beside the jar lists the
patches and hashes. The patches, in short:

| patch | what | codegen |
|---|---|---|
| 0001 | inverted null check in generic-reflection dependency linking (every program reaching generic reflection crashed the compiler) | none |
| 0002 | upstream's points-to memory fixes (55692a84a 9089fe100 00c0c9b6d bbc601ccb 7c35d97ec): mctick's analysis did not finish in 12 GB before | reachability / devirtualization may differ |
| 0003 | JDK methods TeaVM's classlib lacks; class-path resources from the VFS; UUID as the JDK's two longs | only where reached |
| 0004 | `-Dwj.asyncReport`: list reachable suspending methods with a caller chain | none |
| 0005 | reflection on the low-level target answers "none" instead of linking unimplemented natives; `Array.get/set` | only those methods |
| 0006 | vtable default-method resolution (still wrong upstream), `ConcurrentMap.computeIfAbsent`, upstream 7883fc417, `DecimalFormat` prefix-only negative pattern, no GC reference type for `WeakReference` on this target | where the old behaviour was wrong |
| 0007 | the low-level GC traces object monitors (header word) and re-encodes them when a Monitor moves (upstream fixed only the C target, f30b56fea) | the GC's own code |
| 0008 | `ServiceLoader` allocated its result with the service class instead of its array class: heap corruption | that runtime method |
| 0009 | low-level classlib: `getEnumConstants` returns an `E[]`, `RandomAccessFile` creates and no longer truncates (region files read back empty and every chunk regenerated, silently), `Path.of(URI)`, `Files.find`, default `Path` iterator / equals / resolve, `getSuperclass` | those methods |
| 0010 | a class initializer initializes its nearest initializing superclass first (JLS 12.4.2): "Duplicate id value for 0!" | one call per class initializer |
| 0011 | FULL: the inliner's devirtualization did not count the SPECIAL calls it created, so a method could lose its program while called; a declared method without a program is now `unreachable` (was an empty body: invalid wasm for `BlockStateBase.isAir`) and the build prints `wj: methods declared without a program: N` (1 on mctick, `OptionalLong.isPresent`, unreferenced) | methods wrongly dropped are emitted |
| 0012 | `ClassInitElimination` took an interface call as proof the interface was initialized; at FULL an inlined `Hopper.getSuckAabb()` then read `SUCK_AABB` before `Hopper.<clinit>` (NPE on the first hopper tick) | interface init checks after an interface call are kept |

After changing any of them: `wj bench/teavm-cli --force`, then rebuild the arms (`wj bench/jbox2d/build`,
`wj bench/mctick/build`).
