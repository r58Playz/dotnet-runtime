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
  **BUT THREADING IS NOT FREE, AND THAT ASSUMPTION IS NOW REFUTED (R218).** Built as
  `MONO_WASM_JIT_THREAD_SP` and measured on jbox2d, 6 rounds in BOTH orders, all checksum
  `-1419038276309998642`, `registered=389` / `invalid=0` / `residual` identical on both arms so the tier
  is fully alive: median **1.2485 -> 1.7375 ms/step, +39.2%, non-overlapping ranges 6/6**. The plan's
  reasoning was *"params are `local.get`, which V8 lowers to zero instructions, so the cost is wire size
  only"* -- and that conflates two different things. `local.get` of an EXISTING local is free; ADDING A
  PARAMETER is an argument materialisation at every call site plus one more live incoming value in every
  callee, on a tier already at 32.21% register pressure and 28.1 locals/function. **Price a calling-
  convention change at the CALL SITES, never as a local-op count.** The `s.p` pool it competes with is
  4.4-5.3% of window TOTAL, and threading alone removes only 1 of its 3 ops, so the chunk (which removes
  the other 2) has to beat a cost of this order before any of it pays. Ships **0**. Do not re-derive the
  409x ratio as if it were the whole argument: the ratio is real and the cost term was simply missing.
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

* **PROCESS-WIDE IN THE BYTES, PER-WORKER IN ADMISSION.** Modules are compiled ONCE and broadcast
  (`mono_wasm_jit_broadcast_module`), then instantiated per worker, so V8's code cache only applies while
  the emitted bytes depend on nothing per-thread. An f-slot NUMBER is process-wide and only its
  INSTALLATION is per-worker, which is why admission gates entry rather than the emitted code testing
  anything -- baking a constant f-slot is cache-safe, deriving a form from TLS would make every module
  unshareable. Same asymmetry capped R193's object-keyed delegate recipe (it had to keep the
  `wj_slot_live` probe for exactly this reason), so check any new dispatch design against this rule
  before building it.
* **Residual healing is also the TIERING EDGE, so it cannot just be deleted (R195).**
  `mono_wasm_jit_late_fslot` (`interp.c:5468`) calls `wasm_jit_maybe_compile` because
  `mono_wasm_jit_call_interp` does not, so a callee reached only through an immutable residual edge would
  never acquire an f-slot at all (`profile19: residual_healed=0` is that failure). Removing the guard
  removes re-emission's INPUT, not just a branch. Split the two jobs instead: keep the tiering bump off
  the per-dispatch path, and make the call site a `WASM_RELOC_CALL` hole so a re-frame bakes a constant.

* **`wj_assemble`'s NAME SECTION can fault, and it is the source of the intermittent
  `memory access out of bounds` (R199).** The symbolisation loop (`mini-wasm.c:6360-6363`, gated on
  `pol->names`, and `MONO_WASM_JIT_NAMES` ships **1**) calls `mono_method_get_full_name` per member,
  which walks the signature via `mono_signature_get_desc` -> `mono_type_get_desc` and can trigger LAZY
  signature/type resolution -- on a WORKER thread, concurrently with class setup. The captured trace is
  exactly those frames under `wj_assemble`. Incidence ~33%, so it looks like a random crash and any
  intervention needs a POSITIVE CONTROL: a `names=0` arm read clean and so did the `names=1` control,
  four consecutive runs after two faulting ones. Co-location merging surfaces it without causing it
  (the loop resolves N names per assembly and merging raises N). **The fix is constrained**: a
  signature-free label removes the faulting walk, but `hotinsn.py`'s `kind_of()` calls a symbol OURS
  only if it has BOTH a colon and a space, so a bare `Type:method` label silently reclassifies the whole
  tier as `aot` -- the same failure shape as `symclass.mjs` reading our tier at 2.80% instead of ~60%.
  Keep the `<x> <Type>:<method> (<args>)` shape and check `symclass.mjs` too.

## RESOLVE AND CONSUME IN THE SAME BREATH — four subsystems, one shape

Four separate bugs here have had the identical structure: **something is looked up or validated on one
thread, then USED later, after a window in which another thread may have changed it.** The symptom differs
every time, which is why it kept being diagnosed as four unrelated problems.

| where | what was resolved early | how it failed |
|---|---|---|
| `wj_assemble` name section (R199) | `mono_method_get_full_name` per member, during assembly | lazy signature/type resolution on a worker inside the compile section -> intermittent `memory access out of bounds`, ~33% incidence. Fixed by caching `WjBody.name` at EMIT time |
| `wj_collect_shadows` ranking (R201) | 64 shared `WjBody *` collected in pass 1, framed in pass 2 | the owning thread retires/replaces a body in between -> **`function signature mismatch` x3** and a wedged world load. Fixed by re-fetching at the point of use (`WJC_SHADOW_REVALIDATE` counts the catches) |
| devirt target resolution | `mono_class_get_virtual_method` at emit time | class init -> managed code -> IKVM classloader -> synchronous cross-thread JS call while holding the compile lock: **every thread parked**. Fixed by having the OBSERVER pass `target` in (`interp.c:1008-1015`) |
| the f-slot / prefilled placeholder | "the slot is non-null, so it is callable" | a slot THIS worker never installed holds `mono_jiterp_placeholder_jit_call`; wrong signature traps, right signature silently corrupts the heap |

**The rule:** resolve and consume in the same breath, or re-resolve at the point of use. If a design needs
the two separated — ranking, batching, sorting all do — then the early pass may decide **order or priority
only**, never identity: a stale size just mis-ranks, which costs nothing, while a stale pointer frames the
wrong function. And a metadata operation (`mono_method_get_full_name`, `mono_interp_get_imethod`,
`mono_class_get_virtual_method`) is never safe to add to a worker-side compile section, however innocuous
the call looks — each of those lazily resolves something and two of them take a lock.

Corollary for counters: a guard that catches a race should COUNT its catches, and non-zero is then the
healthy reading. `WJC_SHADOW_REVALIDATE` at 0 forever would mean the guard is dead code, not that the
race is impossible.

## The prefilled placeholder — the trap that has now bitten twice

`mono_jiterp_allocate_table_entry` hands out slots from a range the jiterpreter **prefills with a real,
callable wasm function**: `mono_jiterp_placeholder_jit_call`, whose signature is `(i32,i32,i32,i32)->void`
and whose entire body is `*thrown = 999` (`interp.c:15719`, filled at `jiterpreter-support.ts:2198`).

So **`table[fslot] != null` is not a liveness test**, and neither is "the import resolved". A slot this
worker has not instantiated holds a function that:

* traps if you `call_indirect` it with any other signature — which is loud, and is what jit138 hit; and
* **works** if the expected type is that one very common shape — writing 999 through the caller's fourth
  argument as a pointer and returning. Silent heap corruption, no LinkError, no trap, no diagnostic.

**And the converse invariant, which is load-bearing (R216):** an f-slot only ever holds a JIT `f` from a
module this emitter produced. Nothing else is ever installed there — AOT bodies are reached through their
own table indices and their own `at`/`at_ne` functypes, never through an f-slot. That is what lets a caller
bake a functype for an f-slot call with no runtime kind test, and it is what makes an ABI change like
`MONO_WASM_JIT_THREAD_SP`'s trailing parameter safe to apply to every f-slot functype at once. There was
one *potential* second occupant: `wasm_module_interp_thunk` framed a module exporting `t`, a scalar-ABI
stand-in that drove an un-JITted callee through the interpreter, installed by an `e_slot < 0` arm in
`mono_wasm_jit_instantiate_local`. It was never wired up — nothing in the tree ever called the framer, so
the arm could not fire and the invariant held by accident. Both are now DELETED rather than left for the
next reader to reason around.

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

## THE DEVIRT CENSUS IS PER-SITE AND UNWEIGHTED — never plan from it alone (R205)

`[wasm-jit devirt]` counts SITES at emit time. A site executed a billion times counts the same as one
executed twice, so the bucket that looks biggest is routinely not the one carrying the traffic. The
execution-weighted split (each emitted IC tagged with the devirt outcome of its OWN site; parts sum to
`fast_vic` to 0.0009%):

| cause | IN-GAME WINDOW | cumulative | % of SITES | over/under-weight |
|---|---|---|---|---|
| **alt-receiver at a site that DID devirt** | **32.5%** | 17.7% | n/a | — |
| `no_rec` | 32.2% | 24.5% | 28.1% | x0.87 |
| `poly` | 30.7% | **52.1%** | 7.9% | **x6.59** |
| `cold` | 4.6% | 5.4% | 9.0% | x0.60 |
| `no_fslot` | **0.0%** | 0.3% | 3.1% | **x0.11** |

**`no_fslot` is 0.0% of hot IC execution** (2,052 hits in a 90 s window). R204 cut it 69% via
`MONO_WASM_JIT_THRESHOLD` 500->2000 and `DEVIRT_FORCE_MIN` 64->8, and that is worth **nothing on the
plateau** — it is a boot/worldgen effect. Both knobs may still be right for boot; neither is a frame-rate
lever. Do not repeat this: weight a devirt bucket by execution before spending on it.

**"`no_rec` is cold branches that never execute" is WRONG** — those sites take 34M hits per window. The
reconciliation is the standing measurement that **99.3% of profile observations arrive AFTER the method was
JITted**: raising the JIT threshold adds PRE-JIT interp observations, which is the wrong channel entirely,
while the record does exist later from the IC's own misses. So **re-emission can convert `no_rec` and the
threshold structurally cannot**, which is also why R170b saw `no_rec` 31.2% -> 0.0% on re-emitted bodies.

**Where the hot IC volume is:** polymorphic dispatch **63.2%** (alt-receiver 32.5% + poly 30.7%),
re-emission **32.2%**, everything else 4.6%.

**RE-EMISSION AND THE SECOND ARM COMPOSE, AND NEITHER PAYS ALONE (R209).** `MONO_WASM_JIT_REEMIT_IC`
(default 0) triggers re-emission from the IC MISS path -- the selector R179 named and never built, since
its own trigger counted interp->JIT BOUNDARY crossings and was stats-gated. Within-run, comparing
re-emitted bodies against the same run's census: **arm2 OFF, coverage 28.2% vs 39.0% run-wide (-10.8
pts); arm2 ON, 62.9% vs 43.5% (+19.4 pts)**. Re-emission matures the profile so `no_rec` falls, but
`margin == total` is IRREVERSIBLE, so the extra observation converts `no_rec` into `poly` -- a pure loss
unless something consumes polymorphic sites. Arm 2 is that consumer (`poly_arm1` 2,051 -> 2,187,
`arm2_emitted` 1,284 -> 1,378). This is why every earlier re-emission arm read flat even when it worked.
**The wedge that blocked it was a torn read at the interp->JIT entry gate**, which checked `admit_live`
on the DESCRIPTOR and then called a separately-read `wasm_jit_slot`; re-emission republishes both, so a
thread could enter a slot it never instantiated and hit the prefilled placeholder -- `function signature
mismatch` in `wasm_jit_ethunk_cb`. Snapshot the slot and verify it with `mono_wasm_jit_slot_live`. That
is also the mechanism behind R174's five wedged arms.

**A SECOND GUARDED ARM AT POLY SITES SHIPS BEHIND `MONO_WASM_JIT_DEVIRT_ARM2` (default 0), and it works
(R206).** `margin == total` is irreversible, so a site that ever saw a second receiver is refused an arm
for the life of the process -- a 90/10 site identically to a 50/50 one. Giving such sites up to two arms,
chosen by the per-identity counts added to `WjProfSite`, measures **+18.7% direct-call sites** (local arms
per 1k modules 461.2 -> 547.4, n=2/n=3, **non-overlapping ranges**), `fast_devirt2` 9.1M in-window hits,
and poly's share of IC hits **29.0% -> 12.1%**. `ARM2_PCT` (default 15) refuses sites below the break-even
-- 546 of them -- which is what keeps this from repeating the `vcall_ways` regression. Not yet timed.

**Cost model for a guarded arm, counted off emitted code (R206).** Arm hit = `i32.load; i32.ne; br_if` then
`call <funcidx>` = **~3 x86 + 1**. IC hit = 2 loads + cmp + jne + unpack + `call_indirect` = **~21 x86**.
An extra arm pays its guard on ALL traffic reaching it and saves (ic - direct) on what it captures, so it
wins above **capture > g/(ic-d) = 3/20 ≈ 15%** — and above **~50%** if the target does NOT co-locate,
because then the "direct" call is itself a `call_indirect`. **Co-location is the precondition for another
arm, not a bonus.** This model is consistent with the one hard datapoint: `vcall_ways` 4 -> 1 was
**+9.6% fps** with those ways capturing ~1%, far below the bar.

## The EXECUTED dispatch split, and the hard ceiling on direct calls (R203)

Measured with `profile_fast=1` (costs ~7%; ratios valid, timings void), 90 s in-game window, deltas between
the window's two counter snapshots. Config: ranked shadows + `colocate_merge` + `devirt_force`.

| route | executed | of ALL dispatch | can it be DIRECT? |
|---|---|---|---|
| devirt predicted arm | 1,030,605,081 | **39.2%** | **YES** — `WASM_RELOC_CALL` |
| IC (inline hit 25.1% + AOT-IC 4.6% + miss 5.0% + helper 0.8%) | 567,690,313 | 21.6% | no |
| delegate (`fast_delegate`) | 524,858,165 | 20.0% | no |
| inline AOT direct | 505,946,820 | 19.2% | no |
| | **2,629,100,379** | | |

**Within the vtable-virtual pool: devirt 64.5% / IC 35.5%, a 1.82:1 ratio.** Note `vcall_resolve_fslot` —
the IC MISS path and the 2nd hottest symbol in the window — is only **5.0%** of that pool; the inline IC
HIT is 25.1%, i.e. five times larger.

**THE CEILING ON DIRECT CALLS IS 39.2%, AND IT IS STRUCTURAL.** Only `wj_emit_method_call` emits
`WASM_RELOC_CALL`, which is the only relocation the assembler can turn into `call <funcidx>`; the IC path
emits `WASM_RELOC_INDIRECT`, whose own comment reads *"can never become a direct call; only its functype is
relocated"*. Delegates and inline-AOT calls are likewise indirect by construction. So **however good
co-location and shadowing get, they cannot take direct dispatch past the devirt arm's share of execution.**
Currently 25.0% of all dispatch is a real `call rel32` (39.2% x 63.7% arm-local).

**Raising that ceiling means raising devirt COVERAGE, not more shadow tuning** — coverage moves volume out
of the IC pool and into the arm pool. Coverage is 34.6% of ordinary-virtual sites and the census names the
blocker: `no_rec` **13,389 (30.5% of sites)** against `no_fslot` 6,115, `poly` 3,739, `cold` 3,531. `no_rec`
is precisely what re-emission with a matured profile fixed (31.4% -> 9.8%, R170b/R179), which is the one
mechanism that attacks the ceiling rather than the fill. Even at 100% coverage, delegate (20%) + inline AOT
(19%) cap direct dispatch near **61%**.

**Do not convert an "arm-local %" into a "% of dispatch" using the 74-85% figure.** That number is the arm's
share of the VCALL POOL, not of all dispatch; using it as the latter overstates direct dispatch by ~2x (it
produced a "~54% of dispatch is direct" claim when the measured answer is 25.0%).

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
| shadow copies, NON-LEAF (`MONO_WASM_JIT_SHADOW=1 MONO_WASM_JIT_SHADOW_NONLEAF=1`) | **WORKS, R168.** 101 shadows, `callLocal` 97 -> 204 (2.10x, share 3.49% -> 7.25%), `callIndirect` 0.92x, wire cost 6.43% of `bytesGenerated`, and BOTH fallbacks plus every admission counter at 0 — the dep merge (shadow callees joined the host's direct-dep list, admitted by R166's closure walk) held with zero failures. **Plateau A/B (4 arms, R149 protocol, all fault-free): PARITY** — control 42.95/41.79 (mean 42.37), shadows 42.24/41.02 (mean 41.63), 1.7% at a 2.8% arm spread, which resolves nothing at n=2. Mechanism reproduced (callLocal 2.2x and 4.5x) and every safety counter stayed 0. That parity is what the arithmetic predicts: ~400 converted sites against a ~500k-site tier is <0.1%. Shadows removed the reach CEILING, and **R200 pointed them at the tier**: with `colocate_merge=1 devirt_force=1 shadow_nonleaf=1 shadow_bytes=2000` there are **23,202 shadow members** (not 101) and the devirt predicted arm goes **14.6% -> 62.2% arm-local**, i.e. ~11% -> ~53% of EXECUTED dispatch on a real `call rel32`. **The reason shadows beat co-location by 21 points where a cap increase is worth 1.5 is that co-location is a PARTITION** — each method joins at most one group, so a hot callee shared by five callers co-locates with exactly one of them however large the caps get; duplication is the only mechanism that bypasses a partition. What is still unknown is TIMING (every R200 number is a static call-form census) and the price of **+50% bodies**. **`MONO_WASM_JIT_SHADOW_BYTES` was closed as a lever by R169 and that closure is RETRACTED (R200).** R169 swept 500/2000/8000, got 169/180/456 shadows for a callLocal share of 6.75%/8.20%/8.10%, and concluded 16x the cap buys no extra conversion. On the CURRENT stack (`colocate_merge` + `devirt_force` + `shadow_nonleaf`) 500 -> 2000 moves devirt arm-local **51.2% -> 62.2%**, the largest single step in R200's table. R169 was not measured badly — it measured **169 shadows** where this stack has **23,202**, a 137x larger population in which `ShadowBig` is **96% of all refusals**. **A knob closed as "non-binding" is closed only for the population it was measured on**; re-test a cap whenever the population it gates grows by an order of magnitude. R169's mechanism sentence (paid per BYTE, pays back per SITE) survives as the COST model: bodies grew 75,609 -> 168,005 across R200's table (+50% over the no-shadow arm), which is real wire size at ~15.4 ns/byte of V8 compile time and is still unpriced.  **R201/R202: SELECTION ORDER was the real variable, and the caps are now closed.** `wj_collect_shadows` took callees in relocation-ENCOUNTER order, so a 16-slot cap went to whichever callees a body mentioned first; ranking by **sites/bytes descending** (a shadow costs its body once per module and repays once per SITE) gives **63.7% arm-local with 2.8% FEWER bodies** than encounter order at identical caps -- the only change so far that improved conversion and duplication together. After that, raising `WJ_SHADOW_MAX` 16->48 and `SHADOW_BYTES` to 4000 drove `ShadowCap` to **0** and `ShadowBig` to 592 and conversion did **NOT** move (62.8%, members flat at ~33k): with ranked selection and slack caps the candidate SUPPLY is exhausted, so cap sweeps are closed. **Shadows plateau at ~63% arm-local = ~54% of executed dispatch direct** (from ~11% shipped, 4.9x; `CallLocal` 17,186 -> 90,157). What blocks the rest is NOT a cap: at r202, recorded refusals 1,535 + `ShadowModCap` 727 = 2,262 against **6,793** surviving indirect arms, leaving **4,531 arms indirect with no recorded refusal of any kind** -- an UNCOUNTED ROUTE. It is not R195's no-f-slot ceiling (devirt's `no_fslot` counts sites that never got an arm; these arms exist in the dump). Put a counter at the point where a predicted arm chooses its call form before theorising or sweeping anything else. Also note tier size varies run to run (28,110 vs 30,893 modules), so do not rank two configs on <2 points from single runs. Ships default 0. |
| shadow copies as built (leaf-only) | R167: **4 accepted, 608 refused**, and the split says `notleaf` 449 / `nojit` **0** — every refused callee already has a relocatable body, so the R129 main-module ceiling is NOT what blocks them; the leaf rule this round wrote is. Relaxing it is the next lever (R166's dep-closure admission is what would absorb a non-leaf shadow's callees). Also NOT working yet: `shadow=1` hangs on an `inst.exports.e` undefined at instantiate, 959k retries; the encoder layout is exonerated by `enctest` t4. Ships `MONO_WASM_JIT_SHADOW=0`. |
| re-emission with a matured profile (`MONO_WASM_JIT_REEMIT`) | **AND IT IS INERT AT `stats: 0` (R192)**: the trigger reads `cmethod->wasm_jit_invoke_in` (`interp.c:9609-9611`) but that field is incremented only inside the `mono_wasm_jit_stats` guard one line later, is documented "stats only" (`interp-internals.h:172`), and has no other writer but a copy in `tiering.c:55`. So any reemit arm run without `--stats` measured nothing, and R179's named successor must not key off a stats-gated field. **CLOSED, R179. Hypothesis right, population wrong.** On the one arm that landed a real sample (16 methods): devirt coverage **46.3% vs 29.8%** run-wide, `no_rec` **9.8% vs 31.4%** — recompiling with a matured profile does recover the no-record sites. But `vicMiss` measured **1.00x**, predicted in advance: the trigger (`invoke_in >= 5`) selects methods crossing the interp→JIT BOUNDARY, not methods hot INSIDE JIT code, so `vcall_resolve_fslot` (3.41% of window) is untouched. Seven implementation bugs, each revealed by fixing the last, ending in trigger-vs-age-gate churn (**17,343 enqueues for 3 re-emits**). Do not patch it an eighth time — the next version selects candidates from the CALL PROFILE's hot sites, which is a different mechanism. Ships 0. Superseded note (R170b): On re-emitted bodies devirt `no_rec` is **0.0%** vs 31.2% run-wide, and emitted 37.5% vs 29.8% — re-emitting with a matured profile eliminates the biggest devirt blocker outright (n=24 sites, repeat it). But `SLOT_MOVED` x20 / `reemitDone` 0: every re-emit lands on a different e/f pair, leaking two table slots each, and two boot stalls followed. **That diagnosis is REFUTED (see the comment now at `mono_wasm_jit_self_reserved`, `transform.c`): the pin WORKS.** `REEMIT_PIN_MISS` fires because re-emitting A runs cctors and island compiles for unrelated methods B, C, D, each of which asks for ITS OWN slots while A's pin is set — refusing them is the point, the observed pairs are plainly unrelated (`pinned=Utf8JsonReader:get_ValueIsEscaped, asked_for=AllowedBmpCodePointsBitmap:_GetIndexAndOffset`), and `reemitDone == reemitRereg` confirms A keeps its own pair. So re-emission's machinery is SOUND and R179's closure rests on POPULATION alone. Do not re-derive the pin. Ships 0. Earlier note (superseded): R170 first The devirt census is CUMULATIVE over every emission in the run, so 28 re-emissions cannot move it — a null result that is an instrument limit, not evidence. Needs devirt counters scoped to re-emitted bodies. Also `REEMIT_SLOT_MOVED` x20+: the pin via `wasm_jit_self_resv_*` does not survive `wasm_jit_compile_publish` re-looking-up the InterpMethod under the jit-mm lock (`interp.c:2060`), so a fresh slot pair is allocated and the old one leaks. Ships 0. |
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

* **THE BOX IS AT ~95 C WHENEVER THE GAME IS IN-GAME, PERMANENTLY.** It is an i7-1360P, a 28 W mobile
  part, and it throttles 2106 -> 1403 MHz. So "let it cool first" is NOT an available remedy and
  `preflight`'s thermal warning is not actionable -- the only defence against ordering bias is to run
  BOTH ORDERS. R194 is the demonstration: rounds 1-3 with arm A first gave non-overlapping ranges
  favouring A, 3/3, which is the standard this project has used since R40; reversing the order in rounds
  4-6 reversed the result, and the real rule was that **the arm running SECOND was slower in 6 of 6
  rounds**. A single-order interleave cannot see that. Never quote non-overlapping ranges from one
  order.
* **COUNT ABSOLUTE LOCAL ARMS, NOT arm-local% (R214).** Every mechanism in the direct-call path changes
  the NUMBER of arms, so the fraction moves with its own denominator and reads as an effect. Matched
  pair on `colocate_deps`: OFF looks better at 74.6% vs 58.3% arm-local, but ON produces **13,697 local
  arms against 12,041** -- 13.7% MORE direct calls -- because it also produces 23,490 arms against
  16,141. Same trap retired three separate conclusions in one session: a "1,046 net-negative arms"
  claim, a "repartition cut duplication 50.4%->39.6%" claim, and "incremental co-location is
  net-harmful". A larger tier is also usually MORE METHODS COMPILED, not bloat -- check the module
  count before reading MB as waste.
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
* **THE NETWORK STALL IS DEFUSED -- `IKVM_DEFUSE_RANKED_AUTH=1`, now the harness DEFAULT (R205).**
  MCSR Ranked's account validation never reaches a terminal socket status under this net bridge, so the
  modded main screen never finishes and the run stalls waiting for `inworld`. That is the
  "STALLED: no log output at all for 90s" signature, and it voided THREE arms in one session (~8 min
  each). The switch lives on the C# side -- `ikvmcraft/loader/Transforms/Bench/DefuseRankedAuth.cs`,
  read at `loader/IkvmWasm.cs:172` -- and reaches the runtime because `index.ts` passes any
  `MONO_|IKVM_|DOTNET_`-prefixed knob straight through as an env var. `lib/mcdrive.mjs`'s `buildUrl` now
  sets it unless the caller passes `IKVM_DEFUSE_RANKED_AUTH=0`. It is BENCHMARK-ONLY and it CHANGES WHAT
  THE GAME DOES, so it goes through the URL like every other knob and stays visible in each run's
  recorded command line rather than being hidden in the launcher.
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
  later and put delegates at 37-42%. With the arm counted the arm is ~74-85%. **And R187's own rebuttal is HALF RETRACTED (R193): the object-keyed delegate cache does NOT shorten the hit path.** R187 answered the in-source objection at `WjLocalDelegatePicEntry` with "every cost in that sentence is a cost of the cache being SHARED, not of it being object-keyed". True for the seqlock and the atomics; FALSE for the `wj_slot_live` probe. The probe exists because the cached f-slot NUMBER is process-wide while its INSTALLATION is per worker, and R63b's per-site PIC could only skip it because that array was itself per worker. Built and measured: `MONO_WASM_JIT_DELEGATE_OBJ_PIC` moves miss-path publications 88,209,759 -> **1,996** (the per-callee keying works, 44,000x) while the emitted `__<>MHC` stub shrinks **1,311 B -> 1,289 B, i.e. -1.7%** — the probe costs back what the site-id derivation, bounds check and key compare saved. Object-keyed and probe-free need storage that is per-callee AND per-worker; that is a different data structure, not a re-argument. Sized before spending an arm: ~4.8% of delegate dispatches were missing the recipe (`fast_delegate` 418,401,288 vs `delegate_ic_hit` 398,520,024), so the saving is ~0.26% of window — below the ~12% floor, so do NOT spend an fps matrix on it. Ships 0. **R187's "delegates are 43.0% of dispatch" is itself RETRACTED (R203): it DOUBLE-COUNTS.** `WJC_DELEGATE_IC_HIT` is documented at its own declaration as *"the subset which also bypassed vcall_resolve_fslot"* of `WJC_FAST_DELEGATE`, and the two are bumped back-to-back **unconditionally at the same two sites** (`mini-wasm.c:12273-12274`, `:12423-12424`), so `fast_delegate + delegate_ic_hit` counts the same dispatches twice. Delegates are **~20% of all dispatch**, not 43% — measured R203, and the in-source note at `WjLocalDelegatePicEntry` saying ~36% is also too high. This is the FOURTH round lost to a share computed before its routes were checked, and the first where the bad share was written into this file: **assert the parts are DISJOINT as well as summing to the whole.** An
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
* **BACKGROUND TASKS ARE KILLED WHEN THEIR OWNING AGENT EXITS UNLESS IT IS ASYNC — run measurements in
  their own cgroup (R207).** Read from the harness source (`~/Documents/puter/nodejs/claude-code/dist/agent/`
  and `extract-agent/agent.extracted/`, v2.1.250 vs installed 2.1.259 — same shape in both), not guessed.
  `runAgent`'s cleanup ends with `{name:"shellTasks", keepaliveGated:true, run:()=>killShellTasksForAgent(agentId)}`,
  which kills every running `local_bash` with that `agentId` (log line: *"killing orphaned shell task <id>
  (agent <id> exiting)"*), and stages are skipped only when

      keepalive = isAsync && completedNormally && !aborted && (otherTasks || hasBackgroundedShell)

  **`isAsync` is a required conjunct**: having a backgrounded shell is NOT enough. If the owning agent is
  not async, the gated stages run and the task dies. The kill is `task.shellCommand.kill()` on the handle,
  which is why **`setsid` + `disown` does NOT protect the work** (still a descendant of the spawned pid at
  kill time) while a transient unit does — the spawned process is just the `systemd-run` client, and the
  real work lives in a cgroup the handle has no reference to.
  **THE FIX:** `systemd-run --user --quiet --unit=<name> -- bash -c '<work>; echo $? > <sentinel>'`
  then poll the sentinel.
  **AND DO NOT RELY ON A BACKGROUNDED WATCHER FOR NOTIFICATION (2026-09-06).** A `run_in_background`
  bash watcher (`until [ -f <sentinel> ]; do sleep N; done`) is killed after roughly five minutes --
  observed twice in one session on ~6-minute measurement runs -- while the systemd unit it was watching
  kept running to completion both times. So the watcher is a convenience, never the mechanism: put the
  WORK in the unit, and use a recurring `/loop` tick (or a direct sentinel check) as the actual backstop.
  A killed watcher looks exactly like a killed job if you only read the notification. Verified: a run launched this way survived many turn boundaries that had killed
  every previous attempt.
  **USE A TRANSIENT SERVICE, NOT `--scope`, AND THIS NOTE USED TO SAY `--scope` (2026-09-05).** A scope
  runs the work in the CALLING process's session and keeps it a child of the invoking shell, so it dies
  with that shell exactly like `setsid`+`disown` does — only the cgroup differs, and the cgroup is not
  what the harness kills. `systemd-run` without `--scope` starts a transient *service*: systemd forks it
  from PID 1, so it has no ancestor the harness holds a handle to. The mono inline-limit ladder in the
  2026-09-05 session was truncated at 27 of 40 rows this way, and the failure looks identical to the one
  above (output stops mid-run, no error, sentinel never written) — which is why it was misread as the app
  crashing. Drop `--scope` from any invocation copied out of this file before that date.
  **Symptom:** the run's browser AND its node driver vanish together, the sentinel is never written, and
  there is NO stall message — nothing failed, it was killed. Do NOT diagnose it as an app or JIT fault.
  **Three wrong explanations were asserted here before the source was read**, and all are retracted: a
  duration limit (a 45-min chain had already survived); memory pressure / kernel OOM (the `dmesg` was
  `warn_alloc` for an unrelated `upowerd` order-4 allocation — a WARNING, no `Killed process` line); and
  "~30 minutes of user inactivity" (a SELECTION EFFECT — during idle stretches the tasks launched happened
  to be short watchers, and killed tasks in fact have a median lifetime of 1.5 min, some under 30 s, which
  no idle threshold can explain). When a harness behaviour is in question, READ THE HARNESS.
* **NEVER EDIT A SOURCE FILE WHILE A BUILD OF IT IS IN FLIGHT (R216).** The build reads each file at a
  moment you do not control, so a file edited during it makes the resulting binary's contents *unknowable*
  and every number taken on that binary unusable. R216 lost a delegate-devirt arm this way: the run came
  back `ok=false wall=666.8s` against a 458.8s control -- a 1.45x regression that could not be attributed,
  because `mini-wasm.c.o` is stamped mid-way through the edits. The edits were all behind a default-off
  knob and were *very probably* inert, which is exactly the trap: "probably inert" is not provenance. A
  build is ~9 min and a deploy ~12 min; treat that whole window as read-only and queue the edits.
* **A counter that names an action must be bumped where the action HAPPENS, not where it is decided
  (R215).** Delegate devirt read `DelegateDevirtArm 750` with `FastDelegateDevirt 0` for a whole run: the
  resolution block that chose a target ran unconditionally, while the code that emitted the arm sat inside
  `if (obj_pic)` -- a knob that ships **0**. So "armed" and "emitted" were different populations and every
  counter read exactly as designed. This is the same family as "do not compute a share until every route
  has a counter", with a new shape: the counter was in the right place and the CODE was in a dead branch.
  If the decision and the action live in different functions or different `if` arms, count both and assert
  they are equal.
* **A leftover compiler daemon will silently tax one arm.** The runtime build's `VBCSCompiler` starts under
  the REPO-LOCAL `./.dotnet/dotnet` with its own pipe name, so a `dotnet build-server shutdown` issued by the
  system SDK does not reach it; one was caught at 101% CPU for 11:50. `preflight` refuses to measure while one
  is alive, and that refusal is worth obeying rather than passing `--force`: the daemon exits partway through,
  so it taxes the arms UNEQUALLY. **R200 paid this: a `shadow_bytes=8000` arm was forced past the warning,
  the daemon exited mid-run, the run stalled (`ok=false`) and the arm was void.** Use
  `scratchpad/wj/killdaemons.sh` (exit 0 = safe), which matches preflight's OWN pattern — a filter that
  checks only `VBCSCompiler` misses the `MSBuild.dll` half, which is what let that arm through.
* **A `pgrep -f` / `pkill -f` pattern MATCHES THE SEARCHER, and this has now cost three incidents in one
  session.** Twice as `pkill -f <pat>` killing the invoking shell (exit 144 — one of them took a build with
  it, leaving corrupt intermediates and a *spurious* compile failure that cost a whole cycle), and once
  where the pattern sat inside a heredoc and so became part of the outer shell's own argv, so the "safe"
  bracket trick did not help: `[V]BCS` only fails to match the literal text `[V]BCS`, not a shell whose
  argv contains the plain word `VBCSCompiler`. **Find candidates by command line, then keep only those whose
  `comm` is the expected executable, and never kill self.** That is what `killdaemons.sh` does; prefer
  `pgrep` + explicit `kill <pid>` over `pkill` everywhere else.

## 2026-09-06 session: five things that change how to measure and what to attack

**FPS IS UNUSABLE ON THIS BOX AND COMPOSITION SHARES ARE NOT.** Two control runs of an IDENTICAL config,
same binary, same chromium: `vcall_resolve_fslot` 4.723% / 4.683% (**0.8%**), `InstanceCheck` 2.642 /
2.658 (0.6%), `admit*` 1.343 / 1.365 (1.6%) -- against **fps 15.10 / 18.03 (19.4%)**. Client-thread
instruction SHARES are ~20x more reproducible than frame rate. R149's 11.5% floor is optimistic at
120 s. Quote shares; treat any fps delta under ~20% as nothing. Read them with
`perfraw.py <dir> --phase ingame --marker realize_glenv --tsv out.tsv`.

**AND EVERY BARE IN-GAME WINDOW MEASURES THE RAMP: `--warm-ms` DEFAULTS TO 0** in both `mcperf.mjs`
(line 74, under a comment that says "profile the PLATEAU, not the ~180s ramp") and `mcbench.mjs`.
Work per frame falls ~40% inside the FIRST QUARTER of a 120 s "plateau" window (730 -> 410 M/frame) and
is flat to ~7% after. The reported mean is therefore set by how much transient it contains, which
depends on boot+worldgen duration -- 68-96 s across one evening. Any historical fps A/B taken without
`--warm-ms` was reading ramp position.

**THE RAMP IS NOT JIT WARMUP -- IT IS WORK WE ARE TOO SLOW TO CLEAR.** Measured Q1 vs Q4, per frame:
mono JIT compile **~0 -> ~0**, V8 compile 2.0 -> 0.4, V8 Liftoff share 0.60% -> 0.19%. Both compilers
are DONE before the window opens. What actually drains: **MethodHandle/invokedynamic linking 103.5 ->
47.2 M/frame**, chunk/world (Sodium mesh, chunk gen, lighting) 67.2 -> 27.7, IC miss 33.4 -> 21.1.
Native pays the identical transient -- lazy `invokedynamic` linking and post-join meshing are Java
semantics -- and clears it in seconds because each unit costs ~4-5x less. **Ramp length is a symptom of
the 4-5x gap, not a separate warmup problem.** (Classifier warning: a case-insensitive regex for
"compile" matches the `-turbofan` SUFFIX on every symbol and reports 88% of the window. Strip
`-\d+-(turbofan|liftoff)$` before matching -- fifth classifier bug of this shape.)

**DELEGATE DEVIRT WORKS, AND R215's "INERT" NOTE IS SUPERSEDED.** R215 was right that emission sat in a
dead `if (obj_pic)` branch (`mini-wasm.c:13347`); there is now a SECOND site at `:13544` inside
`for (way = 0; !obj_pic; ...)` which IS live in the shipped config, plus `prof_predict_delegate` (the
alt-reader required `id_targets[]`, always NULL at a delegate site -- 7,582 sites, 0 armed).
`MONO_WASM_JIT_DELEGATE_DEVIRT=15` measured **−6.6% of client-thread instructions/frame, n=2, with a
flat negative control**: `__<>MHC` stubs 12.84 -> 11.20 M/frame (−12.8%), `InstanceCheck` 5.97 -> 4.62
(−22.7%), chromium DSO flat. `thin=35` of 7,556 sites, so delegate sites are overwhelmingly
SINGLE-TARGET and the capture bar is not worth sweeping.
**STRUCTURALLY: the MethodHandle pool and the TYPE-CHECK pool are NOT independent.** A delegate arm
bypasses the `__<>DynamicBinder__` adapter and R109 established that adapter's castclass IS the type
check, so type checks are DOWNSTREAM of delegate dispatch. Do not budget them as separate wins.

**RE-EMISSION IS THROUGHPUT-BOUND, NOT POPULATION-BOUND -- a DIFFERENT closure from R179's.** R179
closed it because the trigger selected interp->JIT boundary crossings; R208's IC-miss trigger fixed
that. What binds now, in order, and the order is not what it looks like: **drain reach** (56,667 queued
vs ~5,800 ever gated) > **compile-lock contention** (`busy=5,134` against `done=189`) > **co-location**
(`batched=496`). Re-emitted bodies do reach 54.7% devirt coverage against 28.1% run-wide, but they are
2.25% of sites, so run-wide coverage moves ~+0.6 pts. Fixing the co-location conflict alone -- the
obvious-looking fix -- moves the THIRD-largest limit.
**`MONO_WASM_JIT_COLOCATE_DEPS=0` + re-emission WEDGES**: 2 of 2 attempts (void window, then hung at
world load). Do not re-run it to test the co-location conflict -- the split counter answers it for free.

**RETRACTED 2026-09-06: "process-wide modules cannot reach `__thread` state".** I closed the monitor
inline-CAS lever on this and generalised it into an architectural rule. **It is wrong.** Modules are
compiled once but INSTANTIATED PER WORKER, so an IMPORTED GLOBAL is resolved per worker and a
`global.get` yields per-thread data with the bytes still identical process-wide. The emitter ALREADY
does this seven times (`wasm-encoder.c:352-358`): `s.l`/`s.c` = `&wj_slot_live`/`_cap`, `s.v`/`s.n` =
`&wj_vcall_pic`/`_cap`, `s.d`/`s.m` = `&wj_delegate_pic`/`_cap`, and `s.b` = this worker's scratch base
-- whose own comment states the exact win I called impossible: "replacing a `mono_wasm_jit_scratch()`
helper CALL on every residual and every vcall cold miss".
The real constraint is only that a per-thread address cannot be baked as a CONSTANT in the shared bytes.
Importing it is fine and is the established pattern. Cost is one `global.get` of an immutable import
(~1 load), not the 4 extra loads a MUTABLE import costs -- only `s.p` is mutable.
**Adding a ninth global is not free**: the index space is a hard-coded 0..7 contract with the emitter,
and the import COUNT literal in `emit_import_section` falling out of step once produced "section was
shorter than expected size", `registered` 0 from boot, and cost a whole session.

**Monitors: every lock is INFLATED, and the cause is `monitor.c:1013`** -- an object whose IDENTITY HASH
has ever been taken can never use the thin lock again, because mono's flat lock stores the owner IN the
lock word and cannot coexist with a stored hash. Java takes identity hashes constantly, so most objects
inflate permanently. HotSpot does not pay this: its stack lock DISPLACES the header. But the recoverable
part is NOT mutex traffic -- `mono_monitor_try_enter_inflated` already has an uncontended CAS fast path
(`monitor.c:820-833`), so the ~3.2% is CALL OVERHEAD around one CAS. Synchronized wrappers ARE JITted
(they do not set `save_lmf`; only the native/icall/pinvoke wrappers do), so an inline CAS has somewhere
to attach. Unbuilt, unsized, ceiling ~1.5-2%.

**Two more merged counters, same disease as the vfbThresh/delegate-share family.** Both are now split.
`WJC_DELEGATE_DEVIRT_REFUSED` merged an ORDERING artifact with a PERMANENT one: measured
**no_fslot=3387 / sig=966**, so 78% is fixable and the addressable population is 5.5x what is armed.
`WJC_REEMIT_REFUSED`'s first site (`interp.c`, the pre-compile gate) merged three causes AND is a
DIFFERENT site from the `WASM_JIT_REEMIT_NO_PUBLISH` printf -- which is why that printf logged 0 while
the counter read 466. Measured **batched=496, no_eslot=0, no_fslot=0**. A plausible guess that
`old_f <= 0` dominated was wrong by exactly 496 to 0.

**`perfraw.py` could not read the primary metric at all, and now can.** Fixed-period captures
(`MCPERF_PERIOD`, i.e. `perf record -c N`) carry sample_type `0x10027` -- no PERIOD field -- and the
reader hard-exited; PERIOD's absence shifts `nr`/callchain back 8 bytes. Also added `--tid N` /
`--marker SYM` to restrict every number to one thread. **And `perf` silently mis-attaches**: one capture
produced a 0.3 MB / 3,967-symbol map and 133 samples where a good run has ~206 MB / ~2.0M, and it reads
as a valid low-overhead run (its fps was the highest of the night). Gate on the symbol-map size.

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
* `killdaemons.sh` — kills leftover Roslyn/MSBuild build servers using preflight's own match, safely
  (see the two hazards in its header). Run it before every measurement; exit 0 means safe to measure.
* **`wasmtier.mjs` now passes `dumps: true`**, so one tier run yields BOTH the call-form reach and the
  counter census. Before R201 those were two ~7-minute runs of the same config, and pairing a reach
  number with a census from a different run is the same confound as a cross-binary reading.

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
