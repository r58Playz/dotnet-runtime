# Working in this fork

This is a fork of `dotnet/runtime` carrying a **runtime wasm JIT for mono** (`src/mono/mono/mini/mini-wasm.c`,
`wasm-encoder.c`). It compiles mono IR to a fresh WebAssembly module per method at runtime, in the browser.
Its target workload is Minecraft 1.16.1 + Fabric + Sodium/Lithium running under IKVM, and the goal is frame
rate.

Read this before editing the wasm JIT backend or before proposing a performance change. Most of what follows
exists because a previous pass got it wrong at real cost.

## Where things are

| | |
|---|---|
| the emitter | `src/mono/mono/mini/mini-wasm.c` (~11k lines), `wasm-encoder.c` |
| the assembler (`wj_assemble`) | resolves a body's relocations and frames N members into one module |
| JIT <-> interp boundary | `src/mono/mono/mini/interp/interp.c`, `ee.h` |
| the app + shipped knob set | `~/Documents/ikvm-wasm/ikvmcraft`, `frontend/src/dotnet/index.ts` |
| IKVM (Java -> CLR, also uncommitted work) | `~/Documents/ikvm-wasm/ikvm-wasm-build/tools/ikvm/ikvm` |
| measurement harness | `scratchpad/wj/` |
| **the running log — read before proposing anything** | `scratchpad/wj/MINECRAFT-FINDINGS.md` |
| chromium/V8 source, for checking claims about V8 | `~/Documents/ikvm-wasm/chromium` |

`MINECRAFT-FINDINGS.md` is ~117 numbered rounds. It is long, but the alternative to reading the relevant part
is re-running an experiment that already has an answer. Several rounds exist only because an earlier round's
conclusion was not read.

## Comment rules

The backend is ~28% comments and that is fine — the reasoning is the valuable part. What is not fine is a
comment that misleads, and the audit that produced these rules found eight of those.

1. **A knob's default is stated once, at its initialiser, and nowhere else.** Six comments said "DEFAULT OFF"
   on a line whose initialiser was `1`. One of them was `inline_aot`, the path taking ~90k calls per frame.
2. **Never leave an argument for a value the code does not hold.** `vcall_ways = 1` carried a trailing
   sentence arguing why 2 was better, after measurement had shown 4 > 2 > 1 monotonically.
3. **A number without a workload is not a result.** Two workloads have been used: the jbox2d fixed-work kernel
   (early, homogeneous, low variance) and full Minecraft (current, three phases, ~5% floor, thermally
   throttled box). A percentage from one says nothing about the other. Where possible also give date, n and
   spread; if you do not know the provenance of a number you are moving, say so rather than inventing it.
4. **When a result is retracted, fix the comment that carried it.** The `MAX_HIMP` block asserted a +3.9% fps
   win for months after the within-binary A/B retracted it as noise.
5. **Keep failed experiments, with their result and their lesson.** `MONO_WASM_JIT_INLINE_ILOFS` is the model:
   hypothesis, the measured 9.8% regression, and *why* the reasoning was wrong. Those comments are what stop
   the next reader re-running them. Do not compress them away.
6. **If you cite V8 behaviour, cite the file.** The source is checked out locally; "V8 probably..." is not a
   reason to ship anything.

## Verified V8 facts — do not re-derive, do not guess

Checked against `~/Documents/ikvm-wasm/chromium/v8` (tree 150.0.7854.0; the installed chromium may be a little
newer, so re-check if something surprising turns up).

* **`local.get` / `local.set` / `local.tee` emit zero instructions.** `LocalGet` is
  `result->op = ssa_env_[imm.index]`, `LocalSet` is `ssa_env_[imm.index] = value.op`
  (`src/wasm/turboshaft-graph-interface.cc:1030-1043`). Local count, local reuse and copy chains are a
  **wire-size** question only. What costs is how many values are live across a call. This is why
  `MONO_WASM_JIT_COALESCE` measured inert and why the `local.tee` peephole is kept for size, not speed.
* **V8 can never inline anything this emitter produces.** `InliningTree` candidates come from the module's own
  call sites, and an imported function has `wire_byte_size_ == 0` so `score()` is 0
  (`src/wasm/inlining-tree.h:79-85`). One method per module ⇒ every call is a real call, permanently. So
  mono's own inliner is the only inliner in the pipeline.
* Flags: `wasm_inlining` true, `wasm_inlining_call_indirect` true, `wasm_inlining_max_size` **500** wire bytes,
  budget 5000 TF nodes (`src/flags/flag-definitions.h:2170-2192`).
* **V8 does not fold a constant `call_indirect` index into a direct call.** It emits a table bounds check, a
  canonical-type check, index→code-pointer arithmetic, a validity check and `call *`: **~15 x86 instructions
  where a direct `call` is 1.** Verified by reading the emitted x86, not inferred.
* **An imported *mutable* global costs 4 extra loads per access** — two hoisted instance-field loads plus a
  buffer-element load and an offset load (`src/compiler/turboshaft/wasm-lowering-reducer.h:1079-1110`) —
  against one hoisted load and a constant-offset access for a module-defined one (`:1129-1145`). `s.p`
  (`__stack_pointer`) is the only mutable import, and every framed method reads *and writes* it.

## Where the time actually goes (in-game plateau, ~25 fps / 40 ms)

* **~60%** of the window is code this emitter generates; ~28% AOT image; ~10% native; ~1% V8 builtins.
* Only **two threads** do work (server tick 99.6% of a core, client render 87.5%) out of twelve. Frame time is
  CPU-bound on those two.
* Inside our generated code, by *executed* instruction: **~18% real computation**, ~32% register pressure
  (spill/reload, reg-reg moves, push/pop), ~26% guards and branches, ~23% heap loads.
* **~20%** of that time is in the first 24 bytes of a function (prologue; AOT code is 12%), and
  **10-21%** is inside a `call_indirect` dispatch preamble. 71% of calls emitted in hot bodies are indirect.

**The bottleneck is the call boundary.** The guards, spills and memory traffic are downstream of it. Judge a
proposed change by whether it removes calls, shortens prologues, or shortens live ranges across calls.

Note what that does NOT imply. Mono's inliner is the only inliner in the pipeline, but raising its limit does
not remove calls here — the caller absorbs the callee's body *including the callee's own call sites*, while the
callee stays separately JITted because it is independently hot. Measured: +4.4% body opcodes, +1.0% calls per
method. "More inlining" and "fewer calls" are not the same thing on a one-method-per-module tier.

## Closed leads — do not re-run these

| lead | why it is closed |
|---|---|
| GC | ~0.3% of wall, confirmed five independent ways |
| interpreter in the hot path | `mono_interp_exec_method` occurs 1.12x per stack; 72% of stacks have exactly the one thread-entry frame |
| Liftoff / V8 tiering | 59.3% of our self time is `turbofan`, 0.41% `liftoff` |
| local renaming, coalescing, `local.tee`, copy-chain elimination as *runtime* levers | V8 source proves local ops are free |
| helper-import cap | not binding: max 30 declared in any hot module, median 3, cap 192 |
| `IKVM_LAZY_BODIES=0` | 11.2% WORSE; the shipped setting is already optimal |
| `__<>DynamicBinder__` receiver castclass | load-bearing — the adapter's cast *is* the type check; removing it turns a ClassCastException into memory corruption |
| shrinking `__<>MHC` stubs as a ~6% lever | their `call_indirect` density is cold alternative dispatch routes, one of which runs per invoke |
| `MONO_WASM_JIT_INLINE_ILOFS=1` | 9.8% worse on p50 at ±1.1% |
| raising `MONO_INLINELIMIT` above the default 20 | closed on mechanism: bodies +3.5-4.4%, saturating by 60, and calls/method goes UP 1.0% (the caller absorbs the callee's own calls while the callee stays separately JITted). Costs nothing at boot; buys nothing measurable |
| ikvmc static compilation of Minecraft+Fabric | out of scope on product grounds: runtime version loading and drop-in mods are requirements |
| **module batching, as it was BUILT** | measured negative four times (-26.6% R61b, -35.7% R96, -13.0% R105, regression R123), always for the same two mechanical reasons, and BOTH are now being removed rather than re-tuned. (1) Producing a batched body cost one full `mini_method_compile` per member, because the body baked module-dependent indices; R141 made bodies relocatable, so re-framing is a memcpy. (2) The planner plans a plateau ONCE, on a globally-quiescent-graph trigger this workload never satisfies — reach 14% of the tier / 1.8% of calls, and `batch_max`/`batch_bytes` measured non-binding. Do not re-run the OLD arms or re-tune the OLD knobs; the numbers above are what they are worth. The redesign is planned (relocatable emission -> SCC-closed assembly + direct-import -> one call profile -> a weight-driven planner behind `MONO_WASM_JIT_BATCH_MODULE`, still default 0). Note the ceiling before spending on it: 55.0% of real call sites target the main module and can never be co-located (R129), only 22.8% of execution-weighted callee mass is under V8's 500-byte inline cap (R121b), and a callee is only inlined if the CALLER itself crosses V8's ~37,000-return tiering budget (R120). |

## Building: the runtime and the app are two different builds

The **runtime** (this repo) is built and packed with

```
WASM_ENABLE_JSPI=true ../FNA-WASM-Build/build-dotnet.sh . true ./dotnet-jspi.zip     # ~9 min
```

which wraps `./build.sh -os browser -s mono+libs /p:RunAOTCompilation=true /p:WasmEnableThreads=true
/p:WasmEnableJSPI=true -c Release`. A plain `./build.sh mono` is NOT that build and its pack has to be
hand-patched; Round 5's whole result was taken on one.

Then **deploy it**, which is a separate step and is not optional:

```
scratchpad/mcsr/deploy.sh '<a string constant your change added>'                    # ~12 min
```

Copying the zip over `statics/dotnet.zip` is not a deploy — the bytes the page loads are
`frontend/public/_framework/dotnet.native.<hash>.wasm`, which only changes when the loader is republished.
`deploy.sh` republishes, applies the two mandatory post-publish patches, and PROVES the marker is in the
served bytes over HTTP. A whole measurement matrix once ran on a stale runtime.

Before either, `scratchpad/wj/csyn.sh <file>` compiles one source file with the real command line in under a
second. It checks both compilation databases, which matters: `mini-wasm.c` builds twice, with and without
`HOST_BROWSER`.

## Build the product configuration: `make build AOT=true`

**The shipped build is MIXED-AOT** — corlib and IKVM are AOT-compiled, the rest is JIT/interp. In
`~/Documents/ikvm-wasm/ikvmcraft` that is:

```
make build AOT=true        # NOT `make build`
```

`make build` alone produces a non-AOT build that **is not the product and does not boot**. It fails
deterministically (bootcheck 0/3) ~44 s in with

```
Assertion at mono/metadata/loader.c:1826,
  condition `mono_metadata_token_table (m->token) == MONO_TABLE_METHOD' not met
```

from `interp_delegate_ctor` constructing a `Comparison`1` (`comparer.Compare` in
`ArraySortHelper`1.Sort`), and with the JIT tier denied for `Sort` it fails instead on
`RuntimeError: memory access out of bounds`. **Neither is a real regression.** Both are artifacts of the
missing AOT half.

This cost most of a session (Rounds 133-135): the non-AOT build was mistaken for a broken HEAD, `git stash` +
a clean-HEAD rebuild "confirmed" it, and the crash was then chased through eleven knob bisections, three
instrumented builds and a full diagnosis of a bug that does not exist in the product. If a boot fails in a
way that looks like a runtime regression, **check the build command before believing it.**

## Measurement discipline

The box is an i7-1360P — a 28 W mobile part that throttles 2106 → 1403 MHz *within a single run*. That fact
invalidated a session's worth of A/B ordering before it was noticed.

* **The floor is ~5%.** Anything smaller needs replication across independent pairs, not more confidence.
* **240 s cooldown before every arm** (`mcab.mjs --cooldown-ms`). Interleaving alone does not cancel the bias,
  because arm B always follows arm A within a round.
* **Use plateau windows** (`--warm-ms 180000 --bench-ms 120000 --no-walk`), never a bare 60 s window — a 60 s
  in-game window is ~100% warmup and measures ramp position, not frame cost.
* **Prefer a within-binary knob A/B to a cross-binary comparison.** A cross-binary reading at this spread
  cannot support a 4% claim however tidy the mechanism sounds; that exact mistake produced the retracted
  `MAX_HIMP` result, and the knob test that overturned it cost 30 minutes.
* **Assert `registered` is unchanged before reading any timing** (also `tableExhausted 0`, `faults []`). A
  wrong functype on an import fails *instantiation*, not the call, so the method silently falls back to the
  interpreter — it looks like a performance result. A whole session was once run against a dead JIT tier.
* **Measure the mechanism before the outcome.** A `wasmtier.mjs` dump or a `hotinsn.py` run takes minutes and
  says whether a change did what it was supposed to; an fps A/B takes hours and says only whether the number
  moved. If the mechanism did not move, do not spend the hours.
* **Verify a tool's classifier before trusting its output.** `lib/symclass.mjs` had a `scriptId === 0` clause
  that reported our tier as 2.80% of the window when it is ~60%, and a whole round's conclusions were built on
  it. `codegencensus.py` had a category ordering that classified `call *0x18(%rbx)` as a memory access and
  reported dispatch as 0.0%. Both looked like answers.

## The instruments

In `scratchpad/wj/`. Start with these rather than `perf report`, which takes minutes per query on these
captures:

* `perfraw.py` — parses `perf.data` directly (0.6 s for 299 MB), resolves against either the
  `--perf-basic-prof` map or the jitdump, and **reads the call chains**, which every capture has always had
  and nothing used until round 117. `--phase ingame --by-thread --inclusive`.
* `hotinsn.py` — resolves each sample to the exact x86 instruction it hit. `--bands` (prologue vs body),
  `--dispatch` (time inside a `call_indirect` preamble), `--kind ours|aot|v8`. This is the only tool here that
  can tell a hot loop from a cold stub inside the same method.
* `wasmtier.mjs` — snapshots the entire JIT tier (~24k modules) as emitted bytecode over one run. Reading the
  emitted wasm is what found the last two real wins.
* `mcab.mjs` / `mcperf.mjs` / `mcbench.mjs` — the interleaved A/B, the perf capture, and the phase-sliced run.
* `tiershape.py` — per-method STRUCTURAL identity between two tier dumps: opcodes, type indices, function
  indices, local indices, branch depths, with the run-dependent immediates blanked. The gate for "is this
  change a pure refactor". **Its noise floor is measured: 0.15%** (18 of 12,291 methods differ between two
  runs of the SAME binary, because a callee that JITs in one run and not the other changes its caller).
  Do NOT write a byte-identity gate instead — R141 explains why one cannot work.
* `enctest/run.sh` — three host-side encoder gates in seconds, against frozen copies of the framers the
  relocatable rewrite deleted. Run it before believing anything else about the encoder.
* `csyn.sh` — one-file syntax check with the real build's command line, both compilation databases.

## Housekeeping

`scratchpad/` is excluded via `.git/info/exclude`, not `.gitignore` — it holds tens of GB of captures and must
never be added. Chrome profile dumps are ~1 GB each and jitdumps ~4 GB; keep the newest of each kind and
delete the rest. Do not commit `*.orig-backup` files.

**`scratchpad/mcsr/seed` is load-bearing and is NOT crash-harness leftovers.** It is a ~690 MB browser
profile holding Minecraft 1.16.1 + Fabric, the mods, `options.txt` and the "New World" save in OPFS.
`lib/mcdrive.mjs`'s `seedProfile()` reflink-copies it for every run; without it the app re-downloads
Minecraft *inside the boot phase* and boot measures the network. It is easy to mistake for junk because it
sits in the otherwise-dead `mcsr/` directory — it was in fact deleted once during a cleanup.

It can be rebuilt: `node scratchpad/wj/seedbuild.mjs` extracts
`ikvmcraft/frontend/public/ikvmcraft-mcsrranked.tar` into OPFS from a page on the dev server's origin
(`/vite.svg`, deliberately not the app — `main.tsx`'s `mount()` starts the download at load and would race
the extraction). Takes ~1 minute; the dev server must be up.

Note that everything in `scratchpad/wj/` — the whole harness and every instrument — is untracked for the
same reason the captures are. Treat the tools as valuable and the data as disposable.
