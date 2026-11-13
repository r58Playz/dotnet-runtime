# wjharness — the wasm JIT's measurement harness

Drives the real workloads (Minecraft under IKVM in Chromium, the jbox2d kernel, and the TeaVM / HotSpot / CheerpJ
reference arms) and reads what they produce. Code only: nothing here is output, and nothing here names a machine.

## Layout

```
wjharness/                tracked; `wj ls` lists every tool
  wj                      entry point
  lib/                    paths (the one resolver), mcdrive (the Minecraft driver), browser, seeds, verdict, preflight, ...
  drive/                  run a workload: worldwait bootcheck mcperf mcab mcbench hwrun abboot arm fit seedbuild nbtedit wgen262 vcmp
  j2d/                    jbox2d: run (the timing tool), counts, the wasm / HotSpot / CheerpJ ledgers, cheerpj/, jvmti/
  read/                   capture readers: perfraw hotinsn hwcensus callinfra memclass stabslice armcmp gc* faultcensus
  fe/                     front-end (i-cache / iTLB / BTB) readers and event set (R525)
  ledger/                 the per-function gap ledger (ours vs HotSpot): capture d8x86 ledger hsledger gap refresh
  tier/                   tier dumps and static gates: wasmtier tiershape tierid vcallreach bodysize depcheck
  native/                 the native Minecraft reference: mcstage, mcnative, mccmp, mcinsn (standalone); natrun, buckets, join, servertid, ...
  boot/                   boot attribution: bootattr stages critpath segs trend natboot
  memaudit/               heap censuses evaluated in the page
  live/                   CDP telemetry: sigtrap trace-exceptions console
  build/                  deploy ikvm-runtime csyn killdaemons reap wjcsync
  enctest/                host-side encoder gates (seconds)

$WJ_HOME                  default <repo>/scratchpad (git-excluded); one CoW filesystem
  state/                  persistent, never reaped
    seeds/                browser profiles with Minecraft + mods + a save in OPFS (lib/seeds.mjs) -- NOT regenerable without the OPFS tar
    refs/                 captures later rounds compare against (j2d-r440-ss, ...)
    arms/                 built reference arms (teavm-*, cap-teavm*, cheerpj jars, jvmti agent, replaced IKVM.Runtime DLLs)
    native/               the native reference's staging (stage/, config/, results/, asm/ = hsdis)
    corpus/               archived run logs: what lib/verdict.mjs's classes are validated against
    patches/              instrument patches kept for re-application (r500-instruments.patch, ...)
    mc-results.jsonl      mcbench / mcab rows
  runs/<date>-<tag>/      EVERY driver's output, and the round's own launcher; disposable by construction
  notes/                  MINECRAFT-FINDINGS.md (the running log) and other untracked prose
```

The rules that keep it that way:

* **A driver writes only under `$WJ_RUNS`.** Default outputs are `runs/<today>-<tool>`; `--out` / an outdir argument is
  checked, and one inside `wjharness/` is refused (`assertOutdir`, `wj_outdir`).
* **A round's launcher lives in its run directory** (`runs/2026-11-04-port-control/run.sh`) and goes with it. A script a
  second round needs is promoted into `wjharness/`, made portable, and committed.
* **Long work runs under `wj bg <name> -- <cmd>`**, a transient systemd service: a background shell is killed with the
  agent that started it, and `setsid` / `--scope` do not help (CLAUDE.md). Status: `wj bg <name>`.
* Only `wj reap` deletes, and only from `runs/` and `/tmp` (dead-pid perf maps, old profiles, isolate logs).

## Configuration

`lib/paths.mjs` resolves every location: environment, then `~/.config/wjharness/machine.env` (template:
`machine.env.example`), then autodetection (CPU sets from the hybrid PMUs, JDKs under `/usr/lib/jvm`, chromium on PATH),
then defaults. Shell tools get it from `lib/env.sh`, Python from `lib/paths.py` (which also puts every code directory on
`sys.path`). A tool that needs an unconfigured checkout refuses and names the key.

```sh
wj doctor        # what resolved, what is missing (exit 1 if a core driver cannot run)
wj selftest      # syntax of every tool, verdict + hotinsn selftests, enctest, the no-machine-paths gate, corpus
wj paths         # the resolved values
```

On a new machine: clone, copy `machine.env.example`, set `IKVMCRAFT` (the only key the drivers need), restore the seeds
into `$WJ_HOME/state/seeds/` (or `wj seedbuild --tar` from an OPFS export), `wj doctor`.

Raw-code perf event sets (hwrun's census, natrun, fe/events.sh) are Golden Cove + Gracemont encodings; on any other CPU
`pmu_require` refuses rather than miscounting. CPU sets: `WJ_PCPUS` (arms; the P-cores here), `WJ_CENSUS_CPUS` (a queued
census, the last E-cores), `WJ_BOOT_CPUS` (everything but the census CPUs).

## Quick start

```sh
wj fit                                                        # wait until preflight passes
wj j2d/run ours $WJ_RUNS/x/d1 --no-perf --no-attr --schedule 600,200,60 --window-batches 40,60   # jbox2d, R440 protocol
wj worldwait --runs 3 --stats --bench-ms 60000               # Minecraft faults + counters (default seed 262n, the target)
wj hwrun $WJ_RUNS/2026-11-05-x a1 --knob K=V                  # one Q60 hardware-census arm; ... then:
wj hwrun $WJ_RUNS/2026-11-05-x --drain
wj mcnative --out $WJ_RUNS/2026-11-05-x/n1                    # the HotSpot 25 reference for the same workload (sa262n)
wj bootcheck --dump-on-stall                                  # boot only, ~2 min
wj deploy '<a string your change added>'                      # after build-dotnet.sh; proves the marker is SERVED
```

## Tools

### Drivers (`drive/`, `j2d/run`)

| tool | what it does |
|---|---|
| `worldwait` | **The default for fault and counter batches.** `--runs N --stats --bench-ms N --knob K=V`; boot -> world -> in-game, per-run `faults=[...]`, the admit/tier line, partition and memory sampling |
| `hwrun` | One arm of a hardware census (protocol Q60: `--warm-ms 160000 --bench-ms 60000 --no-save-quit`), its census queued to run during the next arm's boot on `$WJ_CENSUS_CPUS`. `--drain` after the last arm |
| `mcperf` | Perf capture of ONE run, sliced into phases (`phases.json`); `--jitdump`, `--marker` refuses unless a string is in the served bytes |
| `mcab` / `mcbench` | Interleaved A/B over full Minecraft (`--cooldown-ms`) / wall-clock timing; rows into `state/mc-results.jsonl` |
| `bootcheck` | Boot-only PASS/FAIL, ~2 min. Marker times from Play; `--keep-going`, `--stall-s`, `--dump-on-stall`, `--jprop`, memory at the title |
| `abboot` | Interleaved (ABBA) bootcheck arms -> `boots.tsv` (first frame, title, heap at title) |
| `arm` | One arm of a boot-crash A/B: N bootchecks under one knob set, crashes/valid with the crash text |
| `fit` | Block until ensure-server, killdaemons and preflight all pass |
| `j2d/run` | **The jbox2d timing tool**: `ours`, TeaVM (`teavm tvfull tvsimple tvfull60 tvsimple60`), HotSpot (`hs8 hs21 hs25`), CheerpJ (`cj<V> cjgc<V>`), pinned to `$WJ_PCPUS`. `--schedule` + `--window-batches` for the steady-state protocol, `--no-perf` for timing, `--events A|B|C` for the ledger sets |
| `benchstats` | jbox2d counters (`[wasm-jit ...]` stat lines). Its `mean` is the 12-batch mean and it pins only with `MC_TASKSET`: counters, not timing |
| `seedbuild` | Build a seed from an OPFS export: `--dest $WJ_STATE/seeds/seed-X --tar /X.tar` (tar copied into the dev server's `frontend/public`, deleted after), then register it in `lib/seeds.mjs` |
| `nbtedit` | `<level.dat in> <out>`: an NBT round trip that sets a 26.2 save's difficulty to peaceful and changes nothing else (seeds 262p, 262n) |
| `wgen262`, `vcmp` | 26.2 world generation driver; version-compare driver with `--seed`, screenshots per key (`--probe`) |
| `ensure-server` | Bring `$DEV_UNIT` up and wait for a real 200 (race-free against the dying process holding the port) |
| `native/mcstage` | `[--tar T --version V --name N]` Builds a **standalone** native stage, `$WJ_STATE/native/<name>/` (default `sa262n` = 26.2 no mods from `state/seeds/ikvmcraft-262n.tar`; `sa` = 1.16.1 MCSR with `--tar $NW_TAR --version 1.16.1-fabric-0.19.3 --name sa`): the game dir, mods, config, options and save from the tar ours plays, the classpath (sha1-checked) and LWJGL natives from `$PRISM_HOME`, read-only. Nothing under Prism is written |
| `native/mcnative` | **The native reference, standalone**: HotSpot (`--jdk 25` default), or IKVM on native CoreCLR (`--ikvm $WJ_STATE/arms/ikvm-clr10[/env-classic.sh]`: the arm's README; CoreCLR writes its own perf map, no agent), on a fresh copy of the stage (`--stage NAME`, default `sa262n`; `--warm-s` defaults to the stage's Q60 offset, 183 s, or 198 s on `sa`; joins by menu replay for MCSR, `--quickPlaySingleplayer` otherwise), menu replayed on a private Xwayland, captured the way `hwrun` captures ours (Q60 window, hwrun's event set, `-k 1 -g`, `phases.json`, `[mcw]` markers), with `jcmd Compiler.perfmap` and `j2dagent` (`,lite`; `--agent-full` adds code bytes) for HotSpot's inlining. P-cores pinned |
| `native/mccmp` | **Ours vs native on the server tick**: hardware table, kind of code, the tick partitioned by anchors (where in the game), methods charged and inclusive, and methods out of line with the tick's own ratio. Several runs a side. Native frames are expanded through HotSpot's inlining map; ours are physical (see its header) |
| `native/jarcensus` | How Minecraft's bytecode changed across versions (`--prism 1.16.1,26.2,...`): method sizes against the inlining caps, EH methods, interface calls, casts, invokedynamic by bootstrap. Static only |
| `native/mcinsn` | The executed x86 instruction mix of the server tick, both sides through `hotinsn`'s classifier. Needs `mcnative --agent-full` and an ours capture taken with `--jitdump` |
| `native/natrun` | The older native reference: plays the Prism instance IN PLACE (`$PRISM_INSTANCE`: its options, mods and saves), JDK 8, M instructions/frame per thread. Its mod set is not ours (2026-11-06: the instance holds mcsrranked 6.0.4 beside 5.8.1); prefer `mcnative` |

### Seeds

`--seed NAME` on any driver, or `MC_SEED`. **The default is `262n`, the performance target** (26.2 on Fabric Loader, no
mods, peaceful; see CLAUDE.md). It became the default on 2026-11-08: a run before then without `--seed` measured `mcsr`.
The modded packs `262u`, `262b` and `262p` and the registry-race diagnostic `262d` were retired that day to
`state/retired/2026-11-08/` (with the native stages `sa262d` and `fo262`); `262u` (Fabulously Optimized) came back on
2026-11-11 as a correctness workload. To use another, move it back into `state/seeds/` and restore its `lib/seeds.mjs`
entry from git. Profiles under `state/seeds/`:

| seed | profile | version | what |
|---|---|---|---|
| **`262n`** (default, target) | `seed-262n` | 26.2 + Fabric 0.19.5 | **no mods**; the user's 2026-10-21 "New World" save set to **peaceful** (it spawns at night), vanilla resource pack, vsync off, RD 12, OpenGL backend; built from `state/seeds/ikvmcraft-262n.tar`, and so is the native stage `sa262n` |
| `262u` | `seed-262u` | 26.2 + Fabric 0.19.5 | **correctness, not performance**: Fabulously Optimized's 149 mods (Sodium, Iris, Lithium, ModernFix, EMF/ETF, ...), RD 2, survival; the user's 2026-10-19 export. The widest IKVM / relink / GC surface the harness has |
| `262c` | `seed-262c` | 26.2 + Fabric 0.19.5 | no mods, creative; a datapack teleports along +X: continuous chunk loading (memory census) |
| `mcsr` | `seed` | 1.16.1 + Fabric 0.19.3 | MCSR Ranked, "New World" pre-generated to the 83% knee: the 1.16.1 regression workload |
| `165` | `seed-165` | 1.16.5 + 0.19.5 | 10 mods incl. Iris + BSL, RD 12 |

Each entry in `lib/seeds.mjs` carries the title / join lines, the menu key path into the save, the save+quit Tab count,
and `benignRx` (lines that seed logs on every healthy boot). `bootcheck`'s `boot.toframe` and `boot.reload` phases are
measured from the launcher's Play.

### Capture readers (`read/`, `fe/`, `ledger/`, `native/`, `boot/`)

| tool | what it does |
|---|---|
| `perfraw` | **Primary.** Parses `perf.data` directly (0.6 s for 299 MB), resolves against the perf map or jitdump, reads call chains. `--phase ingame --by-thread --inclusive`, `--tid`, `--marker`, `--tsv`. `PERFRAW_DSO_SYMS=1` names native samples through debuginfod |
| `hotinsn` | Each sample to its exact x86 instruction: `--bands`, `--dispatch`, `--kind ours|aot|v8`, `--feeder`. **`--selftest` before quoting a feeder number** |
| `hwcensus` | The hardware census of one hwrun arm (server tick; `--client` for the render thread) |
| `callinfra` | The call infrastructure, executed: prologue band, dispatch forms (`--band-split`, `--edges`) |
| `memclass` | Where one thread's loads and stores come from (PEBS precise) |
| `stabslice` | Where the server tick stabilises; whether a shorter window measures the same thing (`--trace`, `--wins`) |
| `armcmp` | Side-by-side per-arm table from a batch's census files |
| `gcshare`, `gctime`, `gcphases`, `gcalloc` | STW GC share of a window; GC pauses per phase (`MONO_LOG_LEVEL=debug`); per sgen phase (`MONO_GC_DEBUG=2`); allocation by site |
| `faultcensus` | Per-class fault census over `state/corpus` + `runs/` (`--since`, `--json`); **`--disjoint`** runs the disjointness invariant over every real log |
| `fe/*` | Front-end misses by region (`fesites`), the door into native code (`fedoor`, `--tick-only` / `--part`), native functions (`fenative`), tick work vs tasks vs the idle wait (`fepart`; the split is `feclass.py`, corrected 2026-11-05), top-down (`fe`) |
| `sitewhy`, `inltree` | The call-site census by inline verdict (`--tier`, `--no-phantoms` drops FAST_TLS's non-calls); mono's verbose inline trees (`MONO_VERBOSE_METHOD=<root>;...`) with costs of refused subtrees |
| `ledger/refresh` | The gap ledger: one mapping run -> ours ledger, optional HotSpot ledger, joined report (needs `D8`). `ledger.py --align` (on in refresh) recovers functions whose live layout V8's feedback inlining changed: d8's instructions are aligned to the live ones and the inserted callee code is tagged `v8:inlined-callee` |
| `j2d/prep`, `j2d/wledger`, `j2d/hsledger`, `j2d/counts`, `j2d/report`, `cjledger`, `cjcapture`, `cjprobe` | jbox2d ledgers and counts per run; CheerpJ codegen capture and GC-contract probe |
| `native/buckets`, `join`, `split`, `servertid`, `natstat`, `natcensus` | Per-bucket instructions against native's whole frame; per-method join on Java names; the server tid **by the symbols it ran** (both busy threads are `DedicatedWorker`). `--tid`/`--thread` explicitly, always |
| `boot/bootattr`, `stages`, `critpath`, `segs`, `trend`, `natboot` | Boot attribution by anchor, stages, the client thread's critical path, HotSpot's boot reference |

### Tier dumps and gates (`tier/`, `build/`, `enctest/`, `live/`)

| tool | what it does |
|---|---|
| `wasmtier` | Snapshot the whole tier as emitted bytecode over one run (`--depgraph` for depcheck) |
| `tiershape` | **The refactor gate**: per-method structural identity between two dumps. Noise floor ~0.7%: always run a same-binary control pair |
| `tierid`, `vcallreach`, `bodysize` | Byte identity as a set comparison; static virtual-dispatch census (report its `undecoded` count); wire size per function |
| `depcheck` | The admission contract: every f-slot baked into a body against every one a descriptor declares. Both inputs from ONE run |
| `enctest/run.sh` | Host-side encoder gates in seconds (framers, serializer, re-framing, stub banks executed in node). Run before believing anything about the encoder |
| `deploy` | Republish ikvmcraft against `$DOTNET_ZIP`, apply the post-publish patches, restart `$DEV_UNIT`, prove the marker in the SERVED bytes |
| `ikvm-runtime` | Rebuild IKVM.Runtime (R516's recipe); `--java` also rebuilds IKVM.Java with the new ikvmc (needed for map.xml / import-time intrinsics); `--install` stages into ikvmcraft (dir + zip), keeping the replaced DLLs in `state/arms/ikvm-runtime/` and `state/arms/ikvm-java/` |
| `csyn` | One-file syntax check with the real command line, both compilation databases. Does NOT link |
| `killdaemons` | Kill leftover Roslyn/MSBuild servers by preflight's own match, safely. Exit 0 = safe to measure |
| `wjcsync` | Re-index the JS counter mirrors (jitbench.ts, lib/mcdrive.mjs) after `WJC_*` changes in the C enum |
| `sigtrap` | Catch `function signature mismatch` in the act and read the table at the faulting slot |
| `trace-exceptions`, `console` | Headless CDP exception tracer; matching console lines of one run |

## Reference arms: how each is built

* **TeaVM** (`state/arms/teavm-{full,simple}{,60}`): `$TEAVM_DIR/samples/benchmark` built with the R180 settings
  (FULL = wasm optimization AGGRESSIVE, SIMPLE = `-Pteavm.wasm.optimization=NONE`; `60` = BATCHES 60). Served by
  `drive/serve-coep.mjs`. `cap-*` are their module captures for `j2d/prep`.
* **HotSpot**: `$JAVA_HOME_<N>` running `$TEAVM_DIR/samples/benchmark/ikvm-bench/target/ikvm-bench-*-jar-with-dependencies.jar`,
  with `state/arms/jvmti/libj2dagent.so` for the ledger: `bash j2d/jvmti/build.sh`.
* **CheerpJ**: `j2d/cheerpj/index.html` (tracked) over `state/arms/cheerpj/{bench.jar,gcprobe.jar}`; the loader comes from
  Leaning Technologies' CDN.
* **Native Minecraft, standalone** (`mcnative`): `bash native/mcstage.sh` builds `state/native/sa262n/` from
  `state/seeds/ikvmcraft-262n.tar` and `$PRISM_HOME` (`sa`, 1.16.1, from `$NW_TAR`); the JVMTI agent is
  `state/arms/jvmti/libj2dagent.so` (`bash j2d/jvmti/build.sh`, shared with the jbox2d ledger). JDKs are `$JAVA_HOME_<N>`.
* **Native Minecraft, in place** (`natrun`, older): the Prism instance (`$PRISM_INSTANCE`), `state/native/stage`
  (`cp.single` = Prism's `-cp` minus NewLaunch.jar, `natives/` = the lwjgl natives). Its async-profiler is gone from
  `state/native/`, so it now runs without the per-method census.
