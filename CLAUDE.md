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
| chromium/V8 source, for checking claims about V8 | `~/Documents/chromium/src` (v8 at `src/v8`) |

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

Checked against `~/Documents/chromium/src/v8` (V8 15.4.77; the older `~/Documents/ikvm-wasm/chromium`
checkout referenced by earlier rounds is GONE). Facts below were re-verified against 15.4.77 in R158 unless
they carry an older tree; if something surprising turns up, re-check rather than trusting a stale round.

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
  where a module-local `call <funcidx>` is 1.** Verified by reading the emitted x86, not inferred.
* **`call <import>` is NOT a cheap direct call — it is an indirect call minus two checks (R158).** V8 lowers
  an imported direct call through `BuildImportedFunctionTargetAndImplicitArg`, which returns a `V<Word32>`
  *WasmCodePointer*, and passes it to `BuildWasmCall(..., kWasmIndirectFunction)` — the same path
  `call_indirect` takes (`wasm/turboshaft-graph-interface.cc:2715-2723`,
  `wasm/turboshaft-graph-interface-inl.h:64-105`, v8 15.4.77 at `~/Documents/chromium/src/v8`). So an import
  call still emits the WasmCodePointerTable conversion and the indirect branch:
  `movabs <table>; shl $0x9; shr $0x5; add; cmpq $1,0x8(); jne; call *()` — 6 instructions, ending in an
  indirect `call *`. What it saves over `call_indirect` is only the table bounds check, the canonical-type
  check and the runtime-index loads. **The indirect branch, which is the expensive part, survives.**
  Only `call <funcidx>` (module-local, `RelocatableConstant(WASM_CALL)`) becomes a real direct `call rel32`,
  and it is also the only form V8 can inline through. This is why `MONO_WASM_JIT_DIRECT_IMPORT` converted
  100% of predicted arms (6,892/6,909, R157) and moved nothing measurable — and it is the argument for
  co-location over import conversion.
* **`hotinsn.py --dispatch`'s `tight` mask counts import calls as dispatch preambles**, because it keys on
  the `shl $0x9` that both forms emit. It cannot measure an import-conversion A/B. Use
  `callform-x86.py` (bounds-check split, from emitted x86) or `vcallreach.py` (static call forms).
* **`s.p` IS 4.4-5.3% OF THE IN-GAME WINDOW (R190, range per R192)** — measured, not estimated: 95.3% of the
  `add %r14,reg` decompression pool is entry-band and chained off a `(%r14)` load, the signature of
  the imported-mutable path, and `s.p` is the only mutable import. Do NOT answer this with a private
  GC-scanned arena: that is the design `mini-wasm.c:3020-3062` replaced, and it loses three
  by-construction guarantees (unwound frames fall below SP; wasm-EH landing pads restore SP; JSPI
  keeps frames in the scanned region). THREADING a frame pointer as a call parameter keeps all three
  and cuts `s.p` traffic ~409x (974M dispatches vs 2.4M interp->JIT transitions per window).
  **THE WIN IS THE CHUNK, NOT THE THREADING, and threading is what makes the chunk legal (R192).** The
  conservative scan is NOT bounded by `__stack_pointer`: `sgen-stw.c:73-91` takes the address of a local
  in sgen's own frame, `:118-124` takes `wasm_sp` for suspended threads, and `mini-gc.c:1136-1139` then
  pins all of `[stack_limit, stack_end)`. What the per-method `global.set` buys is that native callees
  allocate BELOW our frame -- which is both why the frame is scanned and why nothing clobbers it. So
  threading alone removes only the `global.get` (1 of 3 ops); the two `global.set`s go only if something
  has already put the global below our frames, i.e. a chunk reserved once per interp->JIT transition.
  CoreCLR (`/home/r58playz/fna-wasm/runtime`) threads it as arg 0 on every managed call
  (`morph.cpp:1802-1809`, `WellKnownArg::WasmShadowStackPointer`) with EMPTY epilogs, and publishes only
  inside main-module C helpers (`helpers.cpp:717-728`, `fcall.h:346-360`) -- and shipped a bug when one
  transition skipped the publish: *"the native callee allocates its shadow frame from the stale global
  ... and overlaps/clobbers our address-taken locals"* (`codegenwasm.cpp:3202-3216`). Our AOT half is
  main-module code that reads the global itself and cannot take a threaded argument, so their shape
  would need an inline publish in our wasm before every AOT call. Their ABI also MANDATES our (I1)
  ("These GC references will be reported as pinned to the GC", `clr-abi.md:876-878`, enforced at
  `gcencode.cpp:4190-4198`). Put the parameter TRAILING, not at arg 0: our prologue pin stores read
  incoming arg `i` as `local.get i`, so a leading param shifts every argument index.
* **An imported *mutable* global costs 4 extra loads per access** — two hoisted instance-field loads plus a
  buffer-element load and an offset load (`src/compiler/turboshaft/wasm-lowering-reducer.h:1079-1110`) —
  against one hoisted load and a constant-offset access for a module-defined one (`:1129-1145`). `s.p`
  (`__stack_pointer`) is the only mutable import, and every framed method reads *and writes* it.

## The prefilled placeholder — the trap that has now bitten twice

`mono_jiterp_allocate_table_entry` hands out slots from a range the jiterpreter **prefills with a real,
callable wasm function**: `mono_jiterp_placeholder_jit_call`, whose signature is `(i32,i32,i32,i32)->void`
and whose entire body is `*thrown = 999` (`interp.c:15719`, filled at `jiterpreter-support.ts:2198`).

So **`table[fslot] != null` is not a liveness test**, and neither is "the import resolved". A slot this
worker has not instantiated holds a function that:

* traps if you `call_indirect` it with any other signature — which is loud, and is what jit138 hit; and
* **works** if the expected type is that one very common shape — writing 999 through the caller's fourth
  argument as a pointer and returning. Silent heap corruption, no LinkError, no trap, no diagnostic.

The authoritative test is the per-thread bitmap, `mono_wasm_jit_slot_live()` (or, on the JS side,
`Module.__wjSlotFn`, the per-worker record of what this worker installed). Both are per-thread because the
function table is per-thread for dynamic entries — a process-wide bitmap cannot answer this, which is why
`wj_slot_is_installed` could not be used for it (R132).

This has now produced two separate bugs a session apart: the inline vcall IC (jit138, fixed with the
`slot_live` gate) and `MONO_WASM_JIT_DIRECT_IMPORT` (R146, fixed by naming method imports `h."m<index>"` so
the resolver can refuse a slot this worker never installed). **Before binding, calling or trusting anything
found at an f-slot, ask whether THIS thread put it there.**

## There is ONE call profile — do not add a second

`InterpMethod.wasm_jit_profile` -> `WjCallProfile` (interp.c) is the single record of what a method's
virtual and delegate call sites dispatch to. It is written by exactly one function, `wj_prof_record`, from
three observation points — the interpreter's virtual dispatch, its delegate dispatch, and the JITted code's
IC MISS (`wj_vcall_pic_publish`) — and read by emit-time devirt prediction, IC sizing, and the batch
planner. **78.8% of its observations come from the JIT's IC miss** (R148), i.e. from the half that used to
be invisible to the emitter, so a reader that skips it is skipping most of the data.

Two things nearby are NOT profiles and must not be merged into it:

* **`wj_vcall_pic` / `wj_delegate_pic`** are per-thread DISPATCH STATE, and per-thread because f-slot
  installation is per-thread. Only the MISS path feeds the profile; keep the hit path a pure TLS load.
* **`wj_entry_edges`** counts interp->JIT TRANSITIONS keyed (caller, callee) and caches method names at
  record time so the main-thread JS dump never takes a lock. Folding it in would spend the profile's 12
  bounded site slots on a different question and change which sites survive eviction — i.e. change codegen.

And one trap, already paid for: **a stable inline-cache id is not the same granularity as a profile
record.** The record is keyed by callee base method (IL offsets are stale after `generate_compacted_code`);
an IC belongs to one call site. Reusing the record's id makes two sites in one method that call the same
base share a PIC slot — 6,345 emissions per boot. That is `MONO_WASM_JIT_STABLE_IC_IDS`, default 0.

## How far off native we actually are, and what closes it (R180 — read before proposing a redesign)

Every row below is a DURABLE reference point: a property of HotSpot, CoreCLR or TeaVM, not of this
emitter, so it does not go stale the way our own rows do. jbox2d fixed-work, `RUNS=7`, all checksum
`-1419038276309998642` (`ikvm-bench/BENCHMARKING.md`); browser rows median of 3 in fresh isolated Chrome.

| row | ms/step | vs HotSpot |
|---|---|---|
| HotSpot JDK 8 (native) | 0.3390 | 1.00x |
| IKVM on CoreCLR 10 (native) | 0.3895 | **1.15x** — IKVM's IL-translation tax, not ours to fix |
| IKVM on mono 10 (native x64 minijit) | 0.6277 | 1.84x |
| **TeaVM legacy wasm, LINEAR MEMORY (browser)** | **0.415** | **1.27x** |
| TeaVM wasm-gc (browser) | 0.430 | 1.31x |
| mono's native interpreter, for scale | 3.3599 | 9.33x |

### OUR position, measured on identical work (R181) — quote THIS, not the fps ratio

`bench-all.sh RUNS=5` + `benchseeded.mjs --runs 3`, same jar, same box, every row checksum
`-1419038276309998642`, mean-of-medians:

| row | ms/step | step factor |
|---|---|---|
| openjdk (HotSpot) | 0.3364 | 1.00x |
| IKVM on CoreCLR 10 | 0.3968 | x1.18 — IKVM's IL translation, off-limits |
| IKVM on mono 10 (x64 minijit) | 0.6110 | x1.54 — mono's compiler vs RyuJIT |
| **ours (wasm JIT, browser)** | **1.1317** | **x1.85 — OUR BACKEND. This is the codegen target.** |
| | | **= 3.36x off HotSpot** |

mono's interpreter is 3.0485, so the tier is worth **2.69x over interpreting**; we are **2.40x** TeaVM
on the same V8. **x1.85 is not a pass-configuration gap**: diffed against mono's
`DEFAULT_OPTIMIZATIONS` (`driver.c:115-132`) we ADD `SSA`, `ABCREM`, `FLOAT32`, and the only thing mono
native has that we could have is `PEEPHOLE` (V8's own peepholes subsume it); the other five are in
`WASM_JIT_OPT_DENY` for structural reasons. So x1.85 is backend OUTPUT quality — shadow GC frame,
dispatch machinery, the `s.p` chain, 28.1 locals/function, no cross-module inlining.

**Minecraft adds its own machinery worth x1.34**: 25.58% of the in-game window is families a jbox2d
kernel cannot exercise — MethodHandle/invokedynamic adapters **13.91%**, our dispatch/IC/interp-boundary
helpers **6.56%**, Java type-check machinery 3.82%, class loading/mixins 1.29%. Note the largest is
IKVM's, not ours, and co-location cannot touch it. 3.36 x 1.34 = **~4.5x explained work-rate gap**.

### The Minecraft gap: >=11.1x as CLIENT-THREAD CPU PER FRAME (R181 + R184)

Native Minecraft is **320 fps / 3.1 ms** (2026-09-02, same instance/mods/world, vsync off, ~1389x840)
against our 25.57 fps. But fps is set by ONE thread, so compare that thread:

| | client-thread CPU per frame |
|---|---|
| ours | 88.9% of a core over 120.5 s / 3,081 frames = **34.8 ms** |
| native | 3.125 ms WALL, and GPU-bound, so CPU/frame **< 3.125 ms** |
| | **>= 11.1x** (a lower bound) |

**The one real confound is that native is GPU-BOUND** (GPU 91% @ 1350 MHz, CPU 18%), so 320 fps is a
floor on its CPU capability. Shrinking the window does not fix it — less GPU work per frame just raises
fps and the GPU stays saturated. *Native is GPU-limited; we are CPU-limited.* A JVM-side A/B through fps
is therefore impossible here: cap `maxFps` and read CPU% instead.

**Two other confounds were asserted and are RETRACTED (R184).** The server tick is a SEPARATE THREAD —
per-thread symbol census: tid 99.1% runs `LithiumServerTickScheduler` (server), tid 88.9% runs
`realize_glenv`/`fpe_GetCache` (client render) — so tick work never lands inside a client frame and the
"13x more tick work per frame" term is void. And core-normalising the fps ratio is the wrong operation,
because fps is set by one thread rather than by whole-process throughput.

Also retracted: an earlier pass derived a "x3.72 workload term" as 12.5/3.36 and the four factors
multiplied to exactly 12.50 — an accounting identity, since the last term was DEFINED as the residual.
Sizing the pool it named gave x1.34, leaving x2.77 unexplained. **A residual is not a finding**, and
piling up plausible confounds is its own failure mode: three were offered, one was measured.

**There is no wasm, V8 or memory-model barrier.** TeaVM reaches 1.27x with linear memory on this same V8.
Being in a browser costs a good wasm compiler ~1.02x (0.415 / 0.406). So neither "wasm is slow" nor "the
memory model is the floor" explains anything, and both are closed.

**But TeaVM's OPTIMIZER is only 33% of its advantage (R180, ablated).** Every pass off — no devirt, no
inlining, no scalar replacement, no LICM, no repeated-field-read — and TeaVM still runs **1.71x of
HotSpot**. Single-pass costs, 5 interleaved rounds, all checksum-gated: **devirt +19.3%, inlining +13.0%,
ScalarReplacement+RepeatedFieldRead +8.3%, all-off +33.1%.** So call lowering DOES dominate among the
passes (~25% net, ~3x the memory passes) — but passes are the minority term. The rest is architecture:
one closed-world module compiled ahead of time, no runtime tiering, no GC shadow frame, no interpreter
boundary, no IC dispatch machinery, and **4.2 declared locals per function against our 28.1**.
Price any "add pass X" proposal against that 33%, and any redesign against the architecture line.

**Devirt and inlining are MUTUALLY REDUNDANT** — do not measure either alone and call it "the value of
devirtualization". TeaVM's emitted `call_indirect` count: 35 baseline, 33 with inlining off, 176 with
devirt off, **791 with both off**. Each pass covers for the other.

**Three passes mono does not have at all**, checked against `optflags-def.h`: `Devirtualization` (no CHA,
R127), `ScalarReplacement`/`VariableEscapeAnalyzer` (no escape-analysis flag exists), and
`GlobalValueNumbering` (no GVN flag; `SSAPRE` is marked "(obsolete)"; we have only local `lcse`). GVN runs
at every TeaVM level including SIMPLE, so it is inside that 1.71x and cannot be ablated — prime suspect
for any large unexplained residual.

**We ARE the middle end.** The wasm JIT takes mono IR and emits wasm as the LLVM path does; V8 does the
register allocation and peepholes. So mono's native-minijit row (1.84x) measures mono's x86 backend and
does NOT bound us. And the tier does not run at -O0: the runtime initialiser `wasm_jit_extra_opt()`
returns 0 when `MONO_WASM_JIT_OPT` is unset, but **the consumer overrides it** —
`ikvmcraft/frontend/src/dotnet/index.ts` sets
`opt: "inline,consprop,copyprop,deadce,branch,cfold,loop,alias-analysis,ssa,abcrem"`, mapped at
`index.ts:298`. SSA, propagation, DEADCE, ABCREM, alias analysis and the inliner all ship ON. The knob is
spelled `opt`, so grepping the consumer for `MONO_WASM_JIT_OPT` finds nothing — comment rule #1 in a new
place, and it cost two wrong conclusions in one session.

**`MONO_OPT_INLINE` being on does not reach the Java call graph.** `method-to-ir.c:8198` admits a
candidate only if the site is non-virtual, the target is non-virtual, or the target is FINAL, and IKVM
emits `callvirt` for every non-final Java method. That is why R118 measured bodies +4.4% and **calls per
method +1.0%**: the inliner reached only the non-virtual remainder and absorbed its callees' call sites.

## Where the time actually goes (in-game plateau, ~25 fps / 40 ms)

* **57.03%** of the window is code this emitter generates; 30.83% "AOT image"; ~1.8% V8; 12.14% outside
  JIT-emitted code (chromium, GL emulation, kernel). Measured, `perf-imp`, 126,498 ingame samples.
* **The "AOT image" bucket is NOT one thing, and treating it as immovable is an error** (R180). Split by
  symbol convention over its 1,710 symbols: **AOT-compiled MANAGED code 12.63%** of window (`IKVM_*`,
  `System_*`, `corlib_*` — the same managed program, AOT'd because the emitter bails or AOT measured
  better, so addressable in principle, led by IKVM type-check helpers `InstanceCheckIsInstanceOf` 1.56% +
  `InstanceCheckOf..IsInstanceOf` 0.86%); **our own JIT tier's runtime helpers 8.01%**; mono runtime C
  (GC, metadata, interp) 3.70%; libc/emscripten/GL 6.49%. A "delete the whole tier" bound that ignores
  the managed half of that bucket under-reports — and a fixed-work bound cannot model a change that
  REMOVES calls at all, which is why the ~1.17x floor arithmetic in the section above is the one to trust
  for "how far could this go".
* **~7% of the window is the JIT tier's OWN helpers (R173, shipped config):** `vcall_resolve_fslot` **3.41%**
  (the IC MISS path, 2nd hottest symbol in the window), `admit`+`admit_live` 0.98% (on the dispatch path),
  `wj_prof_site`+`wj_prof_record` 1.10%, `get_virtual_method_fast` 0.82%, `set_il_offset` 0.44%,
  `call_interp` 0.31%. That is ~40x what shadow copies save. A devirt predicted arm never reaches
  `vcall_resolve_fslot`, so devirt coverage attacks this pool and the dispatch pool at once.
* **R172's "AOT is not better codegen" is RETRACTED (R173): the comparison was confounded.** `--kind aot` and
  `--kind ours` are different WORKLOADS — AOT runs `instanceof` helpers and classpath (2.36% of window on
  InstanceCheck* alone), ours runs meshing and game logic. Nothing is known about relative codegen quality;
  the only sound test is the same methods both ways, i.e. `MONO_WASM_JIT_OVER_AOT` as an experiment.
  Superseded note (R172): Same instrument, same window: REAL COMPUTE is **13.78% of
  our tier vs 12.52% of AOT's**; we are worse only on REGISTER PRESSURE (30.98% vs 24.48%) and better on
  GUARDS (26.69% vs 34.50%). The ~87% overhead is the managed execution model on wasm, not this emitter.
  **This kills R63's argument for leaving `MONO_WASM_JIT_OVER_AOT` off** ("would divert dispatches into our
  less optimised JIT") — the codegen trade is roughly neutral, and AOT additionally runs catch blocks in the
  interpreter. Do not re-assert "AOT is better" without measuring it: it cost one `hotinsn.py --kind aot`.
* Only **two threads** do work (server tick 99.6% of a core, client render 87.5%) out of twelve. Frame time is
  CPU-bound on those two.
* Inside our generated code, by *executed* instruction (re-measured R180/R189 with the corrected
  classifier): **14.85% real computation**, **32.21% register pressure**, 24.47% guards/branches,
  27.98% heap. **The register-pressure bucket is three things and only one is ours** (R189):
  push/pop frame **10.97%** (V8's own wasm frame — same pool as the prologue, removed only by fewer
  calls), reg-reg mov 9.77% (the allocator's), spill store 6.85% + reload 4.63%. Spills are
  long-lived values, not scratch: **reload:store = 4.22**, and **WRITE-ONCE-EARLY slots are 17.4% of
  slots but 30.1% of all reload traffic at 12.1 reloads each** — those are the emitter's method-long
  prologue caches, worth ~0.79% of window directly. Do NOT treat "register pressure" as a
  standalone target; it is a symptom of call density and forced live ranges.
* **~20%** of that time is in the first 24 bytes of a function (prologue; AOT code is 12%), and
  **10-21%** is inside a `call_indirect` dispatch preamble. 71% of calls emitted in hot bodies are indirect.

**The ceiling on call-form work, measured (R171): converting EVERY call to direct is worth ~13% of frame
time** — dispatch is 22.2% of our tier and our tier is ~60% of the window. Shadows removed 1.3% of dispatch
sequences, i.e. 0.173% of the frame, which is why every fps arm read parity. The PROLOGUE is a separate
~12%-of-frame pool that only INLINING unlocks. (R171 concluded "V8 does not inline shadows" from the
tier-wide prologue share rising 19.25% -> 19.87%; **that inference is RETRACTED, R180** — ~200 shadow
call sites against a ~500k-site tier cannot move a tier-wide aggregate in either direction, so the
reading was noise, not a negative result. Whether V8 inlines a shadow remains untested.) What the
prologue pool does imply either way: a shadow that is NOT inlined still has its own prologue, so
shadowing converts `call_indirect -> callee prologue` into `call rel32 -> shadow prologue` — the branch
goes and the frame setup stays. Price any call-form proposal against those two ceilings before building it.

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
| **the GC/object model as a performance lever** | ~1.6% of frame, and its alternative is WORSE. Independently re-derived (R180): pin stores **2.55%** of emitted instructions, frame-zero **0.63%**, and **GC points outnumber ref defs 1.84:1**, so a safepoint model stores per GC point and roughly DOUBLES them (R126's conclusion, reproduced). wasm locals are not addressable or enumerable from outside the module, which is why the shadow frame exists at all — precise stack maps are not available at any price. |
| **WasmGC as a redesign target** | CLOSED ON EVIDENCE, not on feasibility (R180). TeaVM ships both backends and they were measured on the same source, same V8, same machine: **wasm-gc 0.430 vs linear memory 0.415 — wasm-gc is SLOWER**. So "typed `struct.get` must beat linear memory" is refuted, and the reachability objection (mono's heap, metadata, GC and the AOT half all share linear-memory objects with JIT'd code) is the second reason, not the first. V8's implicit null checks ARE WasmGC-only (`null_checks_for_struct_op`, `REDUCE(ArrayLength)`, `wasm-lowering-reducer.h:405-425,1124-1147`) — that is real, and still not worth the rewrite. |
| exception handling as a cost | 0.240% of window in self time across 37 EH symbols; ALL interpretation is 0.749%. `mono_llvm_cpp_catch_exception` sits on 92.5% of stacks but is a STRUCTURAL wrapper frame around protected regions (72.18% of stacks carry exactly one — same false-positive shape as `mono_interp_exec_method`'s 92%), and is 99.87% of EH frame occurrences. Frames meaning an exception is actually in flight total ~100-241 occurrences across 126,498 stacks = **0.08-0.19% of stacks**, so interpreted catch bodies essentially never run. The real EH cost is that `mono_method_check_inlining` refuses ANY method with a clause. |
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
| **module batching, as it was BUILT** | measured negative four times (-26.6% R61b, -35.7% R96, -13.0% R105, regression R123) for two mechanical reasons, and BOTH are now gone. (1) Producing a batched body cost one full `mini_method_compile` per member; R141 made bodies relocatable and R147's `mono_wasm_jit_rebatch` re-frames from the retained bodies, so it is a memcpy. (2) The planner planned a plateau ONCE, on a quiescence this workload never reaches — reach 14% of the tier. Do not re-run the OLD arms or re-tune the OLD knobs (`batch_max`/`batch_bytes` measured non-binding). **FIXED AND ON BY DEFAULT SINCE R166** — `MONO_WASM_JIT_COLOCATE_DEPS` ships **1**. The 6/6 `function signature mismatch` crash was R165 (`wj_desc_state = 2` outran the liveness bitmap; `re->deps` read torn). The 1.63x regression that remained *with every error counter at zero* was R166: `mono_wasm_jit_admit` premarks every batch sibling state 1, and the loop restoring them to 2 was gated on `instantiated_here`, which is false exactly when a worker admits a second member of a batch it already brought up. State 1 is unrecoverable — `admit`'s cycle-break returns 1 while `desc_admitted` requires 2 — so `admit_live` was 0 for that worker's lifetime and every virtual call went to the interpreter (`vfbNotLive` 106.7M/120 s, now **0**). Measured after: clean controls 51.89/52.70/53.09 vs clean co-located 50.23/48.67 — **parity**, 0 faults in 4 arms. Ceiling to know before spending on it: **reach, not correctness** — 163 sites became `call <funcidx>` in the best R166 arm, ~0.03% of the tier, which is why parity was called the honest ceiling — **but that site count is superseded (R192): a fresh tier dump with `colocate_deps=1` shipping shows `predicted-arm CALL FORM: local 1,507 / call_indirect 8,807`, i.e. co-location already converts 14.6% of devirt predicted arms, and the arm carries 74-85% of executed dispatch. Weight arm conversions by EXECUTION, not by their share of a ~500k-site tier, which is the denominator that produced the "parity is the ceiling" reading** and shadow copies (leaf-only, gate on `WjAsmMember.uses_calls == 0`) is the next lever. **TWO of the three ceilings quoted here are RETRACTED (R180).** (a) R129's "55.0% of real call sites target the main module" is a STATIC SITE COUNT and does not describe execution: from 2,598,503 caller->callee edge instances in our tier's call chains, **93.66% of calls out of our tier land in our own tier** and only 6.24% in AOT code, so the AOT wall is not what limits co-location reach. (`MONO_WASM_JIT_OVER_AOT=1` remains the untested knob for the AOT-managed 12.63%-of-window pool, and it keeps the AOT body as fallback so emitter-refused methods do not fall to the interpreter the way a whitelist/`aotprofile` trim would.) (b) R120's "a callee is only inlined if the CALLER crosses V8's ~37,000-return tiering budget" does not follow: **98.41% of our tier's executed time is already in TurboFan-tiered code** (1.56% Liftoff), so the callers HAVE crossed it. The ~37,000 arithmetic itself is sound (`wasm_tiering_budget` 13,000,000 spent at ~`code_size + 40 + 20` per invocation, `liftoff-compiler.cc:718-719,1186-1192`) — tier-up is simply not what blocks inlining; callee LOCALITY is. Only R121b's cap survives: 22.8% of execution-weighted callee mass is under V8's 500-byte inline cap (my static re-measure: 8.3% of body BYTE MASS, median body 469 B against the 500 B cap). |
| shadow copies, NON-LEAF (`MONO_WASM_JIT_SHADOW=1 MONO_WASM_JIT_SHADOW_NONLEAF=1`) | **WORKS, R168.** 101 shadows, `callLocal` 97 -> 204 (2.10x, share 3.49% -> 7.25%), `callIndirect` 0.92x, wire cost 6.43% of `bytesGenerated`, and BOTH fallbacks plus every admission counter at 0 — the dep merge (shadow callees joined the host's direct-dep list, admitted by R166's closure walk) held with zero failures. **Plateau A/B (4 arms, R149 protocol, all fault-free): PARITY** — control 42.95/41.79 (mean 42.37), shadows 42.24/41.02 (mean 41.63), 1.7% at a 2.8% arm spread, which resolves nothing at n=2. Mechanism reproduced (callLocal 2.2x and 4.5x) and every safety counter stayed 0. That parity is what the arithmetic predicts: ~400 converted sites against a ~500k-site tier is <0.1%. Shadows removed the reach CEILING; they are not yet pointed at enough of the tier to pay. **`MONO_WASM_JIT_SHADOW_BYTES` is CLOSED as a lever (R169):** sweeping 500/2000/8000 gives 169/180/456 shadows and 5.75%/26.31%/31.91% wire cost for a callLocal share of 6.75%/8.20%/8.10% — 16x the cap buys no extra conversion, because a shadow is paid for per BYTE and pays back per SITE. 500 is the best point by 4x. Ships default 0. |
| shadow copies as built (leaf-only) | R167: **4 accepted, 608 refused**, and the split says `notleaf` 449 / `nojit` **0** — every refused callee already has a relocatable body, so the R129 main-module ceiling is NOT what blocks them; the leaf rule this round wrote is. Relaxing it is the next lever (R166's dep-closure admission is what would absorb a non-leaf shadow's callees). Also NOT working yet: `shadow=1` hangs on an `inst.exports.e` undefined at instantiate, 959k retries; the encoder layout is exonerated by `enctest` t4. Ships `MONO_WASM_JIT_SHADOW=0`. |
| re-emission with a matured profile (`MONO_WASM_JIT_REEMIT`) | **AND IT IS INERT AT `stats: 0` (R192)**: the trigger reads `cmethod->wasm_jit_invoke_in` (`interp.c:9609-9611`) but that field is incremented only inside the `mono_wasm_jit_stats` guard one line later, is documented "stats only" (`interp-internals.h:172`), and has no other writer but a copy in `tiering.c:55`. So any reemit arm run without `--stats` measured nothing, and R179's named successor must not key off a stats-gated field. **CLOSED, R179. Hypothesis right, population wrong.** On the one arm that landed a real sample (16 methods): devirt coverage **46.3% vs 29.8%** run-wide, `no_rec` **9.8% vs 31.4%** — recompiling with a matured profile does recover the no-record sites. But `vicMiss` measured **1.00x**, predicted in advance: the trigger (`invoke_in >= 5`) selects methods crossing the interp→JIT BOUNDARY, not methods hot INSIDE JIT code, so `vcall_resolve_fslot` (3.41% of window) is untouched. Seven implementation bugs, each revealed by fixing the last, ending in trigger-vs-age-gate churn (**17,343 enqueues for 3 re-emits**). Do not patch it an eighth time — the next version selects candidates from the CALL PROFILE's hot sites, which is a different mechanism. Ships 0. Superseded note (R170b): On re-emitted bodies devirt `no_rec` is **0.0%** vs 31.2% run-wide, and emitted 37.5% vs 29.8% — re-emitting with a matured profile eliminates the biggest devirt blocker outright (n=24 sites, repeat it). But `SLOT_MOVED` x20 / `reemitDone` 0: every re-emit lands on a different e/f pair, leaking two table slots each, and two boot stalls followed. The `MonoMethod`-keyed pin in `mono_wasm_jit_self_reserved` still does not hold — suspect `mono_wasm_force_compile`'s wrapper substitution making `cfg->method != pm`, NOT yet verified. Ships 0. Earlier note (superseded): R170 first The devirt census is CUMULATIVE over every emission in the run, so 28 re-emissions cannot move it — a null result that is an instrument limit, not evidence. Needs devirt counters scoped to re-emitted bodies. Also `REEMIT_SLOT_MOVED` x20+: the pin via `wasm_jit_self_resv_*` does not survive `wasm_jit_compile_publish` re-looking-up the InterpMethod under the jit-mm lock (`interp.c:2060`), so a fresh slot pair is allocated and the old one leaks. Ships 0. |
| `shadowNojit` as evidence about the AOT wall | R169: **the counter is a tautology.** `wj_collect_shadows` walks `WASM_RELOC_CALL`, which only `wj_emit_method_call` emits, for an already-JITted callee whose f-slot it takes as a parameter. AOT callees emit `WASM_RELOC_AOT` and are never shadow candidates, so `nojit` cannot fire. The AOT wall shows up instead as the **64.9% `call <import>`** share of emitted calls. |
| **co-location as a route to "most dispatch is a direct call"** | CLOSED ON STRUCTURE (R195). The reachable set is only the devirt predicted arms -- both `WASM_RELOC_CALL` sites are gated on the callee already having an f-slot (`mini-wasm.c:11000`, `:11796`), so a callee un-JITted at emit time has NO hole and only RE-EMISSION can convert it (which also falsifies "a later re-grouping trigger": walking `f_body.relocs` later finds exactly the depset population). Measured arm-local: shipped **14.6%** -> merge **26.1%** -> +devirt_force **28.6%** -> +max=32 **30.1%**, i.e. ~25% of executed dispatch direct against a ~74-85% ceiling. It plateaus because **co-location is a PARTITION and the arm graph is not partitionable**: if two callers hold arms on the same target, only one can have it co-resident unless all three join one group, so a merging request grows multiplicatively and any cap is exhausted at once. Proof it is the partition and not tuning: the surviving refusals are **100% caps, 0 rules** (`precond=0 caps=19,929`) and yet doubling `COLOCATE_MAX` bought **+1.5 points**. `max=64`+`bytes=131072` also CRASHES (4x OOB, never reaches in-world; `max=32` is clean) and that crash is undiagnosed. The mechanism that bypasses a partition is DUPLICATION -- shadow copies, `MONO_WASM_JIT_SHADOW` -- now with a narrow target (12,444 indirect arms) instead of the whole tier. |
| **SCC co-location as a source of reach** | 7 modules / 24 members per boot against a ~24,000-method tier (R147). Cycles are rare on this workload. It is kept as a CORRECTNESS mechanism (an intra-cycle import cannot be ordered), never as a performance lever. |

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

* **The floor is ~12%, not ~5%** (R149, measured from 359 fault-free runs / 29 repeated identical configs in
  `mc-results.jsonl`): median same-config fpsTail spread is **11.5% on a >=100 s window** and **22.9% on a
  60 s one**; only 3 of 29 groups came in at or under 10%. The old "~5%" was an assumption, and it was
  load-bearing. Resolving a 5% effect against an 11.5% spread needs ~10+ rounds per arm, so for anything
  under ~12% measure the MECHANISM (`tiershape.py` resolves 0.23%, `hotinsn.py`, a tier dump) rather than the
  frame rate — and always report the arm's own spread beside the delta.
* **240 s cooldown before every arm** (`mcab.mjs --cooldown-ms`). Interleaving alone does not cancel the bias,
  because arm B always follows arm A within a round.
* **Use plateau windows** (`--warm-ms 180000 --bench-ms 120000 --no-walk`), never a bare 60 s window — a 60 s
  in-game window is ~100% warmup and measures ramp position, not frame cost.
* **Prefer a within-binary knob A/B to a cross-binary comparison.** A cross-binary reading at this spread
  cannot support a 4% claim however tidy the mechanism sounds; that exact mistake produced the retracted
  `MAX_HIMP` result, and the knob test that overturned it cost 30 minutes.
* **The 2026-09-01 boot stalls are a NETWORK hang, and they are real hangs.** Every captured one ends on the
  same three app lines — `Update Status: AUTHENTICATING`, `tcpws [object URL]`, then net.ts `closing → closed
  → "really closed undefined"` — and the app emits NOTHING afterwards. **When testing whether a hung run
  recovered, filter to lines the APP emits** (`src/dotnet/log.ts`, FabricLoader, IkvmClassLoader): chromium
  keeps writing `gcm/registration_request` and `gpu_blocklist` errors for minutes after the page is dead, and
  counting those as progress produced a confident "it recovered" that was exactly backwards (R177, retracted).
  A perf capture showed a worker RUNNING (62% of samples, 35% in V8's wasm compiler) — a spinning worker
  beside a hung network wait, not a deadlocked runtime. Eleven runs were discarded and three causal theories
  built on which arms "stalled" (`reemit=5` 5/9 vs control 4/9) before anyone looked at the app's own lines.
* **Assert `registered` is unchanged before reading any timing** (also `tableExhausted 0`, `faults []`). A
  wrong functype on an import fails *instantiation*, not the call, so the method silently falls back to the
  interpreter — it looks like a performance result. A whole session was once run against a dead JIT tier.
* **Measure the mechanism before the outcome.** A `wasmtier.mjs` dump or a `hotinsn.py` run takes minutes and
  says whether a change did what it was supposed to; an fps A/B takes hours and says only whether the number
  moved. If the mechanism did not move, do not spend the hours.
* **Verify a tool's classifier before trusting its output.** `lib/symclass.mjs` had a `scriptId === 0` clause
  that reported our tier as 2.80% of the window when it is ~60%, and a whole round's conclusions were built on
  it. `codegencensus.py` had a category ordering that classified `call *0x18(%rbx)` as a memory access and
  reported dispatch as 0.0%. `hotinsn.py --dispatch` marked `shl $0x9 .. call *` as a `call_indirect`
  preamble when BOTH call forms emit it, so it read ~0 across an import-conversion A/B that had in fact
  converted 100% of the sites (R158). All three looked like answers. The pattern is identical every time: the
  classifier was written from a plausible mental model of the emitted code and never checked against it.
* **Do not compute a share until every route has a counter.** R155 divided by a vcall pool with no term for
  the devirt predicted arm and concluded the IC was 79% of dispatch; R156 repeated the same error one round
  later and put delegates at 37-42%. With the arm counted the arm is ~74-85%. **And R187's own rebuttal is HALF RETRACTED (R193): the object-keyed delegate cache does NOT shorten the hit path.** R187 answered the in-source objection at `WjLocalDelegatePicEntry` with "every cost in that sentence is a cost of the cache being SHARED, not of it being object-keyed". True for the seqlock and the atomics; FALSE for the `wj_slot_live` probe. The probe exists because the cached f-slot NUMBER is process-wide while its INSTALLATION is per worker, and R63b's per-site PIC could only skip it because that array was itself per worker. Built and measured: `MONO_WASM_JIT_DELEGATE_OBJ_PIC` moves miss-path publications 88,209,759 -> **1,996** (the per-callee keying works, 44,000x) while the emitted `__<>MHC` stub shrinks **1,311 B -> 1,289 B, i.e. -1.7%** — the probe costs back what the site-id derivation, bounds check and key compare saved. Object-keyed and probe-free need storage that is per-callee AND per-worker; that is a different data structure, not a re-argument. Sized before spending an arm: ~4.8% of delegate dispatches were missing the recipe (`fast_delegate` 418,401,288 vs `delegate_ic_hit` 398,520,024), so the saving is ~0.26% of window — below the ~12% floor, so do NOT spend an fps matrix on it. Ships 0. **The "delegates are ~9%" that used to close this sentence is RETRACTED (R187): measured with `profile_fast=1`, delegate direct 418,401,288 + ic-recipe 398,520,024 = **43.0% of 974,158,019 dispatches**, and that is a LOWER bound because a recipe miss falls through to the vtable IC and is counted in the vcall pool. The in-source note at `WjLocalDelegatePicEntry` said ~36% and was the better number.** An
  uncounted route does not show up as a gap — it shows up as everything else looking bigger. **R166 is the
  third instance and cost a session**: `vfbThresh` read 9,243x the control on a co-location A/B while its
  own three sub-counters summed to 1,399, because the arm for "target IS compiled but is not live on THIS
  worker" was `default: break;`. So make it mechanical — **assert the parts sum to the whole before quoting
  either.** `vfbCold + vfbParked + vfbRetry + vfbNotLive == vfbThresh` is now checkable, and two of those
  four were missing from the harness mirror entirely, which is why the identity could not be tested.
* **Test a correctness GATE against known-good data before believing its verdict.** R180's first stats pass printed "CHECKSUM GATE: MISMATCH" over 25 runs whose checksums were in fact all identical: the TSV's last field carried a trailing newline, so `split('\t')` yielded `"-1419...642\n"` and every comparison failed. Trusting it would have discarded five valid ablation arms as broken. A gate that can fail CLOSED is as dangerous as a classifier that fails open.
* **When a diagnostic prints nothing, walk the call chain from the printf OUTWARD to the trigger before
  concluding anything about the symbol (R192).** R188 concluded `mono_wasm_jit_dump_hot_edges` was
  missing from the JS export table and filed an `EXPORTED_FUNCTIONS` fix; it is in fact in the wasm
  export section AND assigned as `Module["_..."]` by the same idiom as the demonstrably-working
  `_mono_wasm_jit_get_counter`. The real cause was a harness flag: `wj_dump_arity` is reached only via
  `mono_wasm_jit_dump_stats` (which chains to it unguarded, `mini-wasm.c:1255-1263`), and only
  `globalThis.dumpWasmJit()` calls that from the page — unconditionally in `benchstats.mjs:66`, but
  **only under `--dumps` in `mcbench.mjs`**. So `--knob arity=1` needs `mcbench --stats --dumps`.
* **A diagnostic behind a default-off knob is not evidence of absence.** R166 grepped a default-knob run for
  `WASM_JIT_PUBLISH_REFUSED`, got 0, and concluded it had not happened; the printf is gated on
  `MONO_WASM_JIT_VERIFY_DEPS`, default 0, so it returns 0 either way. More generally: "every error counter
  is zero" is a statement about the counters you have, not about the run. Two of R166's four bugs were a
  bare `continue` and a `return 0`, which no counter anywhere was watching.
* **A leftover compiler daemon will silently tax one arm.** The runtime build's `VBCSCompiler` starts under
  the REPO-LOCAL `./.dotnet/dotnet` with its own pipe name, so a `dotnet build-server shutdown` issued by the
  system SDK does not reach it; one was caught at 101% CPU for 11:50. `preflight` refuses to measure while one
  is alive, and that refusal is worth obeying rather than passing `--force`: the daemon exits partway through,
  so it taxes the arms UNEQUALLY.

## The instruments

In `scratchpad/wj/`. Start with these rather than `perf report`, which takes minutes per query on these
captures:

* `perfraw.py` — parses `perf.data` directly (0.6 s for 299 MB), resolves against either the
  `--perf-basic-prof` map or the jitdump, and **reads the call chains**, which every capture has always had
  and nothing used until round 117. `--phase ingame --by-thread --inclusive`.
* `hotinsn.py` — resolves each sample to the exact x86 instruction it hit. **Its load/store split was WRONG until R180** and is now fixed: `classify()` decided direction with `"(" in ops.rsplit(",", 1)[-1]`, which splits INSIDE an indexed memory operand — on `mov %rax,0x8(%rbx,%rcx,1)` it returned `1)` and read the store as a LOAD. Nearly every store this emitter makes is indexed, so `heap STORE` reported **0.01%** where the truth is **7.89%**, folded into `heap LOAD`'s 27.59% (true 19.71%). GROUP totals were unaffected — both land in HEAP MEMORY — so R171/R173's grouped figures survive unchanged. Fixed via `_dest_operand()` (splits at paren depth 0) with a self-test; fourth classifier bug of exactly this shape. `--bands` (prologue vs body),
  `--dispatch` (time inside a `call_indirect` preamble), `--kind ours|aot|v8`, and **`--feeder`** (R192):
  splits the `add %r14,reg` decompression pool by band and by what fed it, which is the `s.p`
  attribution and the direct readout for any change to the frame convention. `--selftest` runs its
  classifier against 9 hand-written sequences and 4 invariants and needs no capture — run it before
  quoting a feeder number, because this is the fifth classifier in this directory and four of the
  previous four were wrong. This is the only tool here that
  can tell a hot loop from a cold stub inside the same method.
* `wasmtier.mjs` — snapshots the entire JIT tier (~24k modules) as emitted bytecode over one run. Reading the
  emitted wasm is what found the last two real wins.
* `vcallreach.py` — full opcode walk of a `wasmtier` dump: dispatch sites split into Delegate.Invoke vs
  ordinary virtual, how many carry a devirt predicted arm, and **what call form each predicted arm
  dispatches with**. Self-validating: every body must decode to exactly its declared end (report it — a
  non-zero "undecoded" count means the rest is meaningless), and it reproduces the known direct_import
  conversion as a control. This is the right tool for "did a change alter call forms", not `hotinsn`.
* `callform-x86.py` — disassembles `call *` sites out of a jitdump's JIT_CODE_LOAD bytes and groups them by
  what the preamble CONTAINS. Written to check `hotinsn --dispatch`'s classifier against emitted code rather
  than trust it, which is how R158 found that classifier counting import calls. Use it whenever a claim
  depends on telling `call <import>` from `call_indirect`: only the latter has the table bounds check.
* `mcab.mjs` / `mcperf.mjs` / `mcbench.mjs` — the interleaved A/B, the perf capture, and the phase-sliced run.
  **`perf inject` is opt-in (`--inject`), not implied by `--jitdump`** (R171): every python reader here parses
  the raw jitdump directly, inject's success path DELETES those dumps, and it was measured still running at
  8m20s on a 3.1 GB dump. Capture size 9.2 GB -> 4.2 GB with it off. Prefer perf captures to fps arms for
  mechanism questions — 60 s and thermally insensitive, against ~12 min for a plateau fps arm.
* `tiershape.py` — per-method STRUCTURAL identity between two tier dumps: opcodes, type indices, function
  indices, local indices, branch depths, with the run-dependent immediates blanked. The gate for "is this
  change a pure refactor". **Its noise floor is measured: 0.15%** (18 of 12,291 methods differ between two
  runs of the SAME binary, because a callee that JITs in one run and not the other changes its caller).
  Do NOT write a byte-identity gate instead — R141 explains why one cannot work.
* `enctest/run.sh` — four host-side encoder gates in seconds. t1/t2 diff against frozen copies of the framers
  the relocatable rewrite deleted; t3 is the serializer round-trip; **t4 is structural, not an oracle diff**:
  it checks `wasm_module_assemble` at `nexport < nmembers` (the shadow layout), which t1 and t2 cannot reach
  because both call the assembler with `nexport == nmembers`. R167 added it after those two passed 400/400
  and 300/300 byte-identical across a change they were structurally incapable of testing. **A gate that
  cannot reach the case a change introduces is not evidence about that change.** Run it before believing
  anything else about the encoder.
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
