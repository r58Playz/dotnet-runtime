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
| JIT <-> interp boundary | `src/mono/mono/mini/interp/interp.c`, `ee.h`, `transform.c` |
| the app + shipped knob set | `~/Documents/ikvm-wasm/ikvmcraft`, `frontend/src/dotnet/index.ts` |
| IKVM (Java -> CLR, also uncommitted work) | `~/Documents/ikvm-wasm/ikvm-wasm-build/tools/ikvm/ikvm` |
| measurement harness | `scratchpad/wj/` (see **The instruments**) |
| **the running log — read before proposing anything** | `scratchpad/wj/MINECRAFT-FINDINGS.md` |
| chromium/V8 source, for checking claims about V8 | `~/Documents/chromium/src` (v8 at `src/v8`) |
| split DWARF for the shipped wasm | `~/Documents/ikvm-wasm/ikvmcraft/loader/obj/dotnet.native.debug.wasm` |

`MINECRAFT-FINDINGS.md` is a long numbered log. The alternative to reading the relevant part is re-running an
experiment that already has an answer; several entries exist only because an earlier one was not read.

## Comment rules

The backend is ~28% comments and that is fine — the reasoning is the valuable part. What is not fine is a
comment that misleads.

1. **A knob's default is stated once, at its initialiser, and nowhere else.** Comments saying "DEFAULT OFF"
   on a line whose initialiser is `1` have shipped here, including on the hottest path in the emitter.
2. **Never leave an argument for a value the code does not hold.** Do not leave prose arguing for 2 on a line
   that says 1 because measurement went the other way.
3. **A number without a workload is not a result.** Two workloads are in use: the jbox2d fixed-work kernel
   (homogeneous, low variance, good for within-binary A/B and cross-VM reference rows) and full Minecraft
   (three phases, ~12% floor, thermally throttled box). A percentage from one says nothing about the other.
   Give date, n and spread where possible; if you do not know a number's provenance, say so rather than
   inventing it.
4. **When a result is retracted, fix the comment that carried it.** A retracted fps win sat in the source for
   months after the A/B that killed it.
5. **Keep failed experiments, with their result and their lesson.** The comments recording a measured
   regression and *why the reasoning was wrong* are what stop the next reader re-running them. Do not
   compress them away.
6. **If you cite V8 behaviour, cite the file.** The source is checked out locally; "V8 probably..." is not a
   reason to ship anything.

## Verified V8 facts — do not re-derive, do not guess

Checked against `~/Documents/chromium/src/v8` (V8 15.4.77). If something surprising turns up, re-check
against that tree rather than trusting this list.

* **`local.get` / `local.set` / `local.tee` emit zero instructions.** `LocalGet` is
  `result->op = ssa_env_[imm.index]` (`wasm/turboshaft-graph-interface.cc:1030-1043`). Local count, reuse and
  copy chains are a **wire-size** question only. What costs is how many values are live across a call.
* **V8 can never inline anything this emitter produces.** Inlining candidates come from a module's own call
  sites, and an imported function has `wire_byte_size_ == 0` so its score is 0 (`wasm/inlining-tree.h:79-85`).
  One method per module ⇒ every cross-method call is a real call, permanently. **mono's own inliner is the
  only inliner in the pipeline.** Relevant flags: `wasm_inlining_max_size` **500** wire bytes, budget 5000 TF
  nodes (`flags/flag-definitions.h:2170-2192`).
* **The three call forms, by cost** (verified by reading emitted x86, not inferred):
  * `call <funcidx>` (module-local) — a real direct `call rel32`, and the **only** form V8 can inline through.
  * `call <import>` — **not** a cheap direct call. V8 lowers it through
    `BuildImportedFunctionTargetAndImplicitArg` into `BuildWasmCall(..., kWasmIndirectFunction)`, the same
    path `call_indirect` takes (`wasm/turboshaft-graph-interface.cc:2715-2723`, `-inl.h:64-105`). ~6
    instructions ending in an indirect `call *`. It saves only the table bounds check and canonical-type
    check; **the indirect branch survives.** This is why converting 100% of predicted arms to imports moved
    nothing measurable, and it is the argument for co-location over import conversion.
  * `call_indirect` — table bounds check, canonical-type check, index→code-pointer arithmetic, validity
    check, `call *`: **~15 x86 instructions where a module-local call is 1.** V8 does **not** fold a constant
    `call_indirect` index into a direct call.
* **An imported *mutable* global costs 4 extra loads per access** — two hoisted instance-field loads plus a
  buffer-element load and an offset load (`compiler/turboshaft/wasm-lowering-reducer.h:1079-1110`) — against
  one hoisted load and a constant-offset access for a module-defined one (`:1129-1145`). `s.p`
  (`__stack_pointer`) is the only mutable import, and every framed method reads *and writes* it.
* **V8's implicit null checks are WasmGC-only** (`null_checks_for_struct_op`, `wasm-lowering-reducer.h:405-425`).
* **Classifier warning:** a case-insensitive match for "compile" hits the `-turbofan` SUFFIX on every symbol
  and reports ~88% of the window. Strip `-\d+-(turbofan|liftoff)$` before matching.

## Architecture invariants — check a design against these before building it

### Bytes are process-wide; instantiation and admission are per-worker

There is no module broadcast. **The emitted BYTES are published once and are identical for every worker**;
each worker then calls `new WebAssembly.Module(bytes)` and `new WebAssembly.Instance(module, imports)`
**itself**, supplying **per-worker imports** (`mini-wasm.c`, the `EM_ASM` at the instantiate sites):

```js
{ m: { h: wasmMemory },      // shared linear memory
  f: { f: wasmTable },       // this worker's function table
  x: { e: wasmExports["__cpp_exception"] },
  s: { p, l, c, v, n, d, m, b, i } }   // the per-thread globals, indices 0..8
```

Repeated `new WebAssembly.Module` on identical bytes is cheap because V8 keys its in-process
`NativeModuleCache` on the wire bytes — that is a V8-internal cache, unrelated to Blink's on-disk
`CachedMetadata` code cache.

Three consequences, all load-bearing:

* **Emitted bytes must not depend on anything per-thread.** Deriving a form from TLS makes a module
  unshareable and defeats the cache. An f-slot NUMBER is process-wide, so baking it as a constant is fine.
* **Per-thread data reaches a module through IMPORTED GLOBALS, and this works.** A `global.get` of an
  immutable import resolves per worker while the bytes stay identical — the emitter already does this nine
  times (`wasm-encoder.c`): `s.p` `__stack_pointer` (index 0, the only MUTABLE one), `s.l`/`s.c`
  = `&wj_slot_live`/`_cap`, `s.v`/`s.n` = `&wj_vcall_pic`/`_cap`, `s.d`/`s.m` = `&wj_delegate_pic`/`_cap`,
  `s.b` = this worker's scratch base, `s.i` = `&mono_wasm_jit_cur_island_il_state`. Cost is one load, not the
  four a mutable import costs. **Adding a global is not free**: indices 0..8 are a hard-coded contract with
  the emitter, and letting the import COUNT literal fall out of step produces "section was shorter than
  expected size" and `registered` 0 from boot.
* **INSTALLATION is per-worker, which is why admission gates entry** rather than the emitted code testing
  anything.

### Admission's contract, and why generated code carries no liveness check

Generated code `call_indirect`s an f-slot with **no liveness check at all**. The entire job of the admission
DFS is to install a method's complete direct-call closure *before* that method may go live. Therefore:

* Proving a dependency is the right method with the right ABI is **not** the same as proving its slot is
  installed on this worker. Both must be checked, and the one that matters at runtime is
  `mono_wasm_jit_slot_live(<the f-slot the caller baked>)`.
* Refusing admission is **recoverable and normal**: return 0, state 0, and the next dispatch retries. Refusals
  in the hundreds per run cost nothing measurable as long as `registered` stays flat.

### The prefilled placeholder — `table[fslot] != null` is NOT a liveness test

`mono_jiterp_allocate_table_entry` hands out slots from a range the jiterpreter **prefills with a real,
callable function**: `mono_jiterp_placeholder_jit_call`, signature `(i32,i32,i32,i32)->void`, body
`*thrown = 999` (`interp.c`, filled in `jiterpreter-support.ts`). A slot this worker has not instantiated:

* **traps** if you `call_indirect` it with any other signature — this is the `function signature mismatch`
  class; and
* **works** if the expected type happens to be that one common shape — writing 999 through the caller's
  fourth argument as a pointer. Silent heap corruption, no LinkError, no trap, no diagnostic.

The authoritative test is the per-thread bitmap `mono_wasm_jit_slot_live()` (JS side: `Module.__wjSlotFn`).
Both are per-thread because the function table is per-thread for dynamic entries; a process-wide bitmap
cannot answer this. **Before binding, calling or trusting anything found at an f-slot, ask whether THIS
thread put it there.**

**The converse invariant is also load-bearing:** an f-slot only ever holds a JIT `f` from a module this
emitter produced. AOT bodies are reached through their own table indices and their own `at`/`at_ne`
functypes, never through an f-slot. That is what lets a caller bake a functype for an f-slot call with no
runtime kind test, and what makes an ABI change safe to apply to every f-slot functype at once.

### Resolve and consume in the same breath

Repeated bug shape, found in four separate subsystems: **something is looked up or validated on one thread,
then USED later, after a window in which another thread may have changed it.** The symptom differs every
time, which is why it kept being diagnosed as unrelated problems — name-section symbolisation faulting during
assembly, a ranked shadow list going stale between passes, devirt targets resolved at emit time deadlocking
on a cross-thread classloader call, an f-slot assumed callable because it was non-null.

**The rule:** resolve and consume in the same breath, or re-resolve at the point of use. If a design needs the
two separated — ranking, batching, sorting all do — the early pass may decide **order or priority only**,
never identity: a stale size mis-ranks, which costs nothing; a stale pointer frames the wrong function.

Two corollaries:

* **A metadata operation is never safe to add to a worker-side compile section**, however innocuous it looks.
  `mono_method_get_full_name` walks signatures and can trigger lazy type resolution.
  **`mono_interp_get_imethod` is not a lookup** — on a miss it CREATES an InterpMethod (`m_method_alloc0` plus
  `mono_method_signature_internal`) under the jit-mm lock, and its lookup asserts on an uninitialised
  `interp_code_hash`. Use `mono_interp_peek_imethod` for a predicate. **But the rule is not "never call
  get_imethod here":** it is *a predicate whose ANSWER does not depend on creating one must not create one*.
  Sites whose job is to bring a blocker up (the island DFS) genuinely need the creation, and peeking there
  wedges boot.
* **A guard that catches a race must COUNT its catches, and non-zero is then the healthy reading.** A catch
  counter stuck at 0 forever means the guard is dead code, not that the race is impossible.

### Ownership: publish before you free, and prefer leaking to freeing

Registry entries, depsets and batch descriptors are read **lock-free by other workers**. A pointer published
into the registry may be held by a worker that is mid-walk, and nothing recalls it. So:

* **Old arrays intentionally remain allocated.** Retention is bounded and cheap; a use-after-free here
  presents as an intermittent `memory access out of bounds` far from the free.
* Publish the contents, `mono_memory_barrier()`, *then* publish the pointer/count that makes them reachable.
* On a rollback, restoring the pointer stops NEW readers only. Do not free what you published.

### There is ONE call profile — do not add a second

`InterpMethod.wasm_jit_profile` -> `WjCallProfile` (interp.c) is the single record of what a method's virtual
and delegate call sites dispatch to. It is written by exactly one function, `wj_prof_record`, from three
observation points — the interpreter's virtual dispatch, its delegate dispatch, and the JITted code's IC MISS
(`wj_vcall_pic_publish`) — and read by emit-time devirt prediction, IC sizing, and the batch planner. **~79%
of its observations come from the JIT's IC miss**, i.e. from the half that used to be invisible to the
emitter, so a reader that skips it is skipping most of the data.

Two things nearby are NOT profiles and must not be merged into it:

* **`wj_vcall_pic` / `wj_delegate_pic`** are per-thread DISPATCH STATE, per-thread because f-slot installation
  is. Only the MISS path feeds the profile; keep the hit path a pure TLS load.
* **`wj_entry_edges`** counts interp->JIT TRANSITIONS keyed (caller, callee) and caches method names at record
  time so the main-thread JS dump never takes a lock. Folding it in would spend the profile's 12 bounded site
  slots on a different question and change which sites survive eviction — i.e. change codegen.

And one trap already paid for: **a stable inline-cache id is not the same granularity as a profile record.**
The record is keyed by callee base method; an IC belongs to one call site. Reusing the record's id makes two
sites in one method that call the same base share a PIC slot.

### Retained raw `MonoMethod *` are not always dereferenceable — UNPROTECTED

The registry and the call profile keep raw `MonoMethod *` for the process lifetime while IKVM generates
dynamic types continuously, so such a pointer is not always dereferenceable by the time the emitter or
admission consults it. This presents as `memory access out of bounds` inside `jit_mm_for_method` /
`mono_interp_*_imethod`, or as an assertion in `mono_class_get_flags`. A range check cannot fix it (a freed
pointer still lands in range); the fix is to stop retaining raw method pointers across a window in which IKVM
can free them. **This crash mode is currently unguarded — know that it exists before diagnosing it fresh.**

## How far off native we are, and what closes it

Durable reference points — properties of HotSpot, CoreCLR and TeaVM, not of this emitter, so they do not go
stale. jbox2d fixed-work, every row checksum-gated (`ikvm-bench/BENCHMARKING.md`); browser rows median of 3
in fresh isolated Chrome.

| row | ms/step | factor |
|---|---|---|
| openjdk (HotSpot) | 0.3364 | 1.00x |
| IKVM on CoreCLR 10 | 0.3968 | x1.18 — IKVM's IL translation, off-limits |
| IKVM on mono 10 (x64 minijit) | 0.6110 | x1.54 — mono's compiler vs RyuJIT |
| TeaVM wasm, linear memory (browser) | 0.415 | x1.23 |
| TeaVM wasm-gc (browser) | 0.430 | x1.28 |
| **ours (wasm JIT, browser)** | **1.1317** | **x1.85 over mono native — OUR BACKEND, the codegen target** |
| mono's interpreter, for scale | 3.0485 | the tier is worth 2.69x over interpreting |

We are **3.36x off HotSpot** and **2.40x TeaVM on the same V8**.

**x1.85 is not a pass-configuration gap.** Diffed against mono's `DEFAULT_OPTIMIZATIONS` we ADD `SSA`,
`ABCREM`, `FLOAT32`; the only thing mono native has that we could have is `PEEPHOLE`, which V8 subsumes. So
it is backend OUTPUT quality — shadow GC frame, dispatch machinery, the `s.p` chain, 28.1 locals/function, no
cross-module inlining. **We ARE the middle end**: we emit wasm as the LLVM path does and V8 does register
allocation, so mono's native-minijit row does not bound us.

**The tier does not run at -O0.** The runtime initialiser returns 0 when `MONO_WASM_JIT_OPT` is unset, but
**the consumer overrides it**: `ikvmcraft/frontend/src/dotnet/index.ts` sets
`opt: "inline,consprop,copyprop,deadce,branch,cfold,loop,alias-analysis,ssa,abcrem"`. The knob is spelled
`opt`, so grepping the consumer for `MONO_WASM_JIT_OPT` finds nothing.

**`MONO_OPT_INLINE` does not reach the Java call graph.** `method-to-ir.c:8198` admits a candidate only if the
site is non-virtual, the target is non-virtual, or the target is FINAL — and IKVM emits `callvirt` for every
non-final Java method. Measured effect: bodies +4.4%, calls per method **+1.0%**. "More inlining" and "fewer
calls" are not the same thing on a one-method-per-module tier.

**Minecraft adds machinery worth x1.34** that a jbox2d kernel cannot exercise: the unresolved-member
dispatch pool 13.91%, our dispatch/IC/interp-boundary helpers 6.56%, Java type-check machinery 3.82%, class
loading/mixins 1.29%. The largest is IKVM's, not ours. 3.36 x 1.34 = **~4.5x explained work-rate gap**.
(That first pool was originally labelled "MethodHandle/invokedynamic adapters" and **that label is wrong** —
R225 §4: real `java.lang.invoke` is **0.168%**. It is IKVM's lazy-linking fallback for unloadable types, and
Java has no `invokedynamic` for fields, which the `__field_NNNN` binders in the profile prove.)

**The client-thread gap is ~30x in instructions and 18.6x in CPU — MEASURED, not a floor (R240).** The old
`>=11.1x` was a lower bound only because native was GPU-bound with unknown CPU cost. With `maxFps` capped so
the GPU is unsaturated, a matched native harness (`scratchpad/wj/nativemc/`) gives:

| | ours | native | ratio |
|---|---|---|---|
| client thread, M instr/frame | 165.2 | 5.40 | **~30x** |
| client thread, CPU ms/frame | 36.6 | 1.97 | 18.6x |
| **client thread, instruction RATE** | **4.51 G/s** | **2.86 G/s** | **0.63x** |

**We retire instructions FASTER than native and still need ~30x more of them.** IPC, caches, memory stalls
and "wasm/V8 execute slowly" are excluded as explanations. **Stop quoting `>=11.1x`**; compare the CPU ratio,
not the instruction ratio, against the 4.5x chain above, since that chain is built from time ratios.

**Composition of our client thread, CURRENT build** (R240): IKVM machinery **32.9%**, our JIT helpers + mono
runtime + GL + corlib **30.8%**, real Java 26.8%, chromium/libc 9.4%. On methods present in BOTH profiles we
are 15.7x. `vcall_resolve_fslot` is **5.46 M instr/frame — still 1.0x native's ENTIRE client frame.**

**Deleting ALL machinery on both sides still leaves 11.1x** (165.2 -> 60.0 -> 5.40), so the per-method term
dominates and grows as machinery is harvested. **Always re-derive these shares on the build you are about to
change**: the same buckets read 46.5% / 30.1% / 14.3% on the 2026-09-06 `base-client` census, and using those
stale numbers oversized one lever by 2.8x. Three symbols went to ZERO between those builds (`call_interp`
4.27 -> 0.00 M/f, `interp_entry` 2.06 -> 0.06, `set_il_offset` 1.69 -> 0.00) and `wj_prof_site` fell 18x.

**HotSpot's own optimizations, measured on this workload (R240), cap whole categories:**

| arm | client M instr/frame | vs stock |
|---|---|---|
| stock | 5.40 | 1.00x |
| `novirt` (`-XX:TypeProfileWidth=0 -XX:-UseTypeProfile`) | 7.39 | **1.37x** |
| `noinl` (`-XX:-Inline`) | 12.12 | **2.24x** |

**A HotSpot with NO inliner is still 13.6x cheaper than us.** So the whole inlining family is worth at most
2.24x and devirtualization 1.37x — real, but an order of magnitude short of the gap. R221 §2's 1.205x/1.132x
were fps through a GPU ceiling and are superseded. **Do not lead a plan with inlining or devirt.**

**Our per-method baseline, not our inlining, is the codegen problem.** Paired x86 (hsdis, R240):
`class_2818::method_8320` is 64 B of bytecode -> HotSpot C2 928 B, HotSpot **no-inline 256 B**, ours
**7,936 B with 162 calls, 47 indirect**. Inlining makes HotSpot's code BIGGER (256 -> 928) and faster, so the
8.5x-vs-stock figure understates it: against a no-inline HotSpot we are **31x**.

**TeaVM's OPTIMIZER is only 33% of its advantage.** Every pass off, TeaVM still runs 1.71x of HotSpot.
Single-pass, checksum-gated: devirt +19.3%, inlining +13.0%, ScalarReplacement+RepeatedFieldRead +8.3%,
all-off +33.1%. So call lowering dominates among passes (~25% net) but passes are the minority term. The rest
is architecture: one closed-world module compiled ahead of time, no runtime tiering, no GC shadow frame, no
interpreter boundary, no IC dispatch, and **4.2 declared locals per function against our 28.1**.
**Devirt and inlining are MUTUALLY REDUNDANT** — TeaVM's emitted `call_indirect` count is 35 baseline, 33 with
inlining off, 176 with devirt off, **791 with both off** — so never measure one alone and call it "the value
of devirtualization". Three passes mono lacks entirely: `Devirtualization` (no CHA), `ScalarReplacement`/
escape analysis, and `GlobalValueNumbering` (we have only local `lcse`).

**There is no wasm, V8 or memory-model barrier.** TeaVM reaches 1.23x with linear memory on this same V8, and
being in a browser costs a good wasm compiler ~1.02x. Both of those explanations are closed.

## Where the time actually goes (in-game plateau, ~25 fps / 40 ms)

* **57.03%** of the window is code this emitter generates; 30.83% "AOT image"; ~1.8% V8; 12.14% outside
  JIT-emitted code (chromium, GL emulation, kernel). Measured, `perf-imp`, 126,498 ingame samples.
* **The "AOT image" bucket is NOT one thing and is not immovable.** Split over its 1,710 symbols: AOT-compiled
  MANAGED code **12.63%** of window (the same managed program, AOT'd because the emitter bails or AOT measured
  better — addressable in principle, led by IKVM type-check helpers), **our own JIT tier's runtime helpers
  8.01%**, mono runtime C (GC, metadata, interp) 3.70%, libc/emscripten/GL 6.49%.
* **~7% of the window is the JIT tier's OWN helpers:** `vcall_resolve_fslot` **3.41%** (the IC MISS path, 2nd
  hottest symbol in the window), `admit`+`admit_live` 0.98% (on the dispatch path), `wj_prof_site`+
  `wj_prof_record` 1.10%, `get_virtual_method_fast` 0.82%, `set_il_offset` 0.44%, `call_interp` 0.31%. A devirt
  predicted arm never reaches `vcall_resolve_fslot`, so devirt coverage attacks this pool and the dispatch
  pool at once.
* Only **two threads** do work (server tick 99.6% of a core, client render 87.5%) out of twelve. The server
  tick is a SEPARATE THREAD, so tick work never lands inside a client frame.
* Inside our generated code, by *executed* instruction: **14.85% real computation**, **32.21% register
  pressure**, 24.47% guards/branches, 27.98% heap. **The register-pressure bucket is three things and only one
  is ours**: push/pop frame **10.97%** (V8's own wasm frame — removed only by fewer calls), reg-reg mov 9.77%
  (the allocator's), spill store 6.85% + reload 4.63%. Spills are long-lived values, not scratch:
  **reload:store = 4.22**, and write-once-early slots are 17.4% of slots but 30.1% of reload traffic. Do NOT
  treat "register pressure" as a standalone target; it is a symptom of call density and forced live ranges.
* **~20%** of time is in the first 24 bytes of a function (prologue; AOT code is 12%), and **10-21%** is inside
  a `call_indirect` dispatch preamble. 71% of calls emitted in hot bodies are indirect.
* **AOT is not known to be better codegen than ours.** A `--kind aot` vs `--kind ours` comparison is
  CONFOUNDED — they are different WORKLOADS (AOT runs `instanceof` helpers and classpath; ours runs meshing and
  game logic). Same instrument, same window: real compute is 13.78% of our tier vs 12.52% of AOT's; we are
  worse only on register pressure (30.98% vs 24.48%) and better on guards (26.69% vs 34.50%). The ~87%
  overhead is the managed execution model on wasm, not this emitter. Do not re-assert "AOT is better" without
  measuring the same methods both ways.

**The ceiling on call-form work: converting EVERY call to direct is worth ~13% of frame time** (dispatch is
22.2% of our tier and our tier is ~60% of the window). The PROLOGUE is a separate ~12%-of-frame pool that only
INLINING unlocks. **The bottleneck is the call boundary**; guards, spills and memory traffic are downstream of
it. Judge a proposed change by whether it removes calls, shortens prologues, or shortens live ranges across
calls.

### The executed dispatch split, and the hard ceiling on direct calls

Measured with `profile_fast=1` (costs ~7%; ratios valid, timings void), 90 s in-game window:

| route | executed | of ALL dispatch | can it be DIRECT? |
|---|---|---|---|
| devirt predicted arm | 1,030,605,081 | **39.2%** | **YES** — `WASM_RELOC_CALL` |
| IC (inline hit 25.1% + AOT-IC 4.6% + miss 5.0% + helper 0.8%) | 567,690,313 | 21.6% | no |
| delegate (`fast_delegate`) | 524,858,165 | 20.0% | no |
| inline AOT direct | 505,946,820 | 19.2% | no |

Within the vtable-virtual pool: devirt 64.5% / IC 35.5%. **The ceiling on direct calls is 39.2% and it is
structural**: only `wj_emit_method_call` emits `WASM_RELOC_CALL`, the only relocation the assembler can turn
into `call <funcidx>`; the IC path emits `WASM_RELOC_INDIRECT`, which "can never become a direct call, only
its functype is relocated". Delegates and inline-AOT calls are indirect by construction. Currently **25.0%**
of all dispatch is a real `call rel32`. Even at 100% devirt coverage, delegate (20%) + inline AOT (19%) cap
direct dispatch near **61%**.

Raising that ceiling means raising devirt COVERAGE, not more shadow tuning. **Do not convert an "arm-local %"
into a "% of dispatch"** using the arm's share of the VCALL POOL — that overstates direct dispatch ~2x.

### The devirt census is per-site and unweighted — never plan from it alone

`[wasm-jit devirt]` counts SITES at emit time. A site executed a billion times counts the same as one executed
twice, so the biggest-looking bucket is routinely not the one carrying traffic. Execution-weighted:

| cause | IN-GAME WINDOW | % of SITES | over/under-weight |
|---|---|---|---|
| **alt-receiver at a site that DID devirt** | **32.5%** | n/a | — |
| `no_rec` | 32.2% | 28.1% | x0.87 |
| `poly` | 30.7% | 7.9% | **x6.59** |
| `cold` | 4.6% | 9.0% | x0.60 |
| `no_fslot` | **0.0%** | 3.1% | **x0.11** |

`no_fslot` is 0.0% of hot IC execution — cutting it 69% via threshold knobs is worth nothing on the plateau;
it is a boot/worldgen effect. **Weight a devirt bucket by execution before spending on it.**

**"`no_rec` is cold branches that never execute" is WRONG** — those sites take 34M hits per window. The
reconciliation: **99.3% of profile observations arrive AFTER the method was JITted**, so raising the JIT
threshold adds pre-JIT interp observations (the wrong channel) while the record does exist later from the IC's
own misses. **Re-emission can convert `no_rec`; the threshold structurally cannot.**

Where the hot IC volume is: polymorphic dispatch **63.2%** (alt-receiver 32.5% + poly 30.7%), re-emission
**32.2%**, everything else 4.6%.

**Cost model for a guarded arm, counted off emitted code.** Arm hit = `i32.load; i32.ne; br_if` then
`call <funcidx>` = **~3 x86 + 1**. IC hit = 2 loads + cmp + jne + unpack + `call_indirect` = **~21 x86**. An
extra arm pays its guard on ALL traffic reaching it and saves (ic − direct) on what it captures, so it wins
above **capture > 3/20 ≈ 15%** — and above **~50%** if the target does NOT co-locate, because then the
"direct" call is itself a `call_indirect`. **Co-location is the precondition for another arm, not a bonus.**

## Closed leads — do not re-run these

Round numbers are kept **in this section only**, because tracing a closure back to its experiment is the whole
point of it.

| lead | why it is closed |
|---|---|
| GC as a cost | ~0.3% of wall, confirmed five independent ways |
| **the GC/object model as a performance lever** | ~1.6% of frame, and its alternative is WORSE. Pin stores are 2.55% of emitted instructions, frame-zero 0.63%, and **GC points outnumber ref defs 1.84:1**, so a safepoint model stores per GC point and roughly DOUBLES them (R126, re-derived R180). wasm locals are not addressable or enumerable from outside the module, which is why the shadow frame exists at all — precise stack maps are not available at any price |
| **WasmGC as a redesign target** | CLOSED ON EVIDENCE, not feasibility (R180). TeaVM ships both backends, same source/V8/machine: **wasm-gc 0.430 vs linear memory 0.415 — wasm-gc is SLOWER.** The reachability objection (mono's heap, metadata, GC and the AOT half all share linear-memory objects with JIT'd code) is the second reason, not the first |
| exception handling as a cost | 0.240% of window in self time across 37 EH symbols; ALL interpretation is 0.749%. `mono_llvm_cpp_catch_exception` sits on 92.5% of stacks but is a STRUCTURAL wrapper frame around protected regions, not an exception in flight. Frames meaning an exception is actually in flight are **0.08-0.19% of stacks**. The real EH cost is that `mono_method_check_inlining` refuses ANY method with a clause |
| interpreter in the hot path | `mono_interp_exec_method` occurs 1.12x per stack; 72% of stacks have exactly the one thread-entry frame |
| Liftoff / V8 tiering | 59.3% of our self time is `turbofan`, 0.41% `liftoff`; **98.41% of our tier's executed time is already TurboFan-tiered**, so tier-up is not what blocks inlining — callee LOCALITY is |
| local renaming, coalescing, `local.tee`, copy-chain elimination as *runtime* levers | V8 source proves local ops are free; these are wire-size only |
| helper-import cap | not binding: max 30 declared in any hot module, median 3, cap 192 |
| `IKVM_LAZY_BODIES=0` | 11.2% WORSE; the shipped setting is already optimal |
| `__<>DynamicBinder__` receiver castclass | load-bearing — the adapter's cast *is* the type check; removing it turns a ClassCastException into memory corruption |
| shrinking `__<>MHC` stubs as a ~6% lever | their `call_indirect` density is cold alternative dispatch routes, one of which runs per invoke |
| `MONO_WASM_JIT_INLINE_ILOFS=1` | 9.8% worse on p50 at ±1.1% |
| raising `MONO_INLINELIMIT` above the default 20 | closed on mechanism: bodies +3.5-4.4%, saturating by 60, and calls/method goes UP 1.0%. Costs nothing at boot; buys nothing measurable |
| ikvmc static compilation of Minecraft+Fabric | out of scope on product grounds: runtime version loading and drop-in mods are requirements |
| `MONO_WASM_JIT_COALESCE` | inert by construction — see the local-ops V8 fact |
| `MONO_WASM_JIT_DIRECT_IMPORT` (method imports) | converted 100% of predicted arms (6,892/6,909, R157) and moved nothing (R158-R159, +0.7% at ±8.7%): an import call is still an indirect branch. Method imports have since been REMOVED, which also deleted the whole admission-ordering/deferral problem |
| `MONO_WASM_JIT_THREAD_SP` (threading the frame pointer as a parameter) | **REFUTED, R218.** jbox2d, 6 rounds both orders, checksum-gated, tier fully alive: median **1.2485 -> 1.7375 ms/step, +39.2%**, non-overlapping 6/6. The plan's reasoning — "params are `local.get`, which is free" — conflates two things: `local.get` of an EXISTING local is free; ADDING A PARAMETER is an argument materialisation at every call site plus one more live incoming value in every callee, on a tier already at 32.21% register pressure. **Price a calling-convention change at the CALL SITES, never as a local-op count.** (`s.p` traffic is 4.4-5.3% of window and threading removes only 1 of its 3 ops.) If revisited, the parameter must go TRAILING — a leading param shifts every argument index the prologue pin stores read |
| `MONO_WASM_JIT_STABLE_IC_IDS` | reusing the profile record's id makes two sites in one method that call the same base share a PIC slot — 6,345 emissions per boot. Ships 0 |
| `MONO_WASM_JIT_DELEGATE_OBJ_PIC` (object-keyed delegate cache) | **R193: works and is not worth it.** Miss-path publications 88,209,759 -> **1,996** (44,000x) while the emitted stub shrinks **1,311 -> 1,289 B (-1.7%)** — the `wj_slot_live` probe costs back what the site-id derivation saved, and that probe is unavoidable because the cached f-slot NUMBER is process-wide while its INSTALLATION is per worker. Sized before spending an arm: ~4.8% of delegate dispatches were missing the recipe, so the ceiling is ~0.26% of window. Ships 0 |
| `MONO_WASM_JIT_REEMIT` (re-emission with a matured profile) | **Mechanism sound, population wrong, and now throughput-bound.** Re-emitted bodies reach 54.7% devirt coverage against 28.1% run-wide, but are 2.25% of sites, so run-wide coverage moves ~+0.6 pts. What binds, in order: **drain reach** (56,667 queued vs ~5,800 gated) > **compile-lock contention** (`busy=5,134` vs `done=189`) > co-location conflict (`batched=496`). Note the trigger must not key off `wasm_jit_invoke_in`, which is incremented only under `mono_wasm_jit_stats` — any arm run without `--stats` measures nothing. `MONO_WASM_JIT_COLOCATE_DEPS=0` + re-emission WEDGES (2 of 2). Ships 0 |
| module batching **as it was originally built** | measured negative four times (-26.6%, -35.7%, -13.0%, regression) for two mechanical reasons, and BOTH are now gone: producing a batched body cost a full `mini_method_compile` per member (bodies are now relocatable and re-framing is a memcpy), and the planner planned a plateau ONCE, on a quiescence this workload never reaches. Do not re-run the OLD arms or re-tune `batch_max`/`batch_bytes` (measured non-binding). **Co-location ships ON** (`MONO_WASM_JIT_COLOCATE_DEPS=1`) |
| **co-location as a route to "most dispatch is a direct call"** | CLOSED ON STRUCTURE (R195). The reachable set is only the devirt predicted arms — both `WASM_RELOC_CALL` sites are gated on the callee already having an f-slot, so a callee un-JITted at emit time has NO hole and only RE-EMISSION can convert it. Arm-local plateaus ~30% because **co-location is a PARTITION and the arm graph is not partitionable**: if two callers hold arms on the same target, only one can have it co-resident. Proof it is the partition and not tuning: surviving refusals are **100% caps, 0 rules**, and doubling `COLOCATE_MAX` bought **+1.5 points**. `max=64`+`bytes=131072` also CRASHES (undiagnosed); `max=32` is clean. The mechanism that bypasses a partition is DUPLICATION — shadow copies |
| **SCC co-location as a source of reach** | 7 modules / 24 members per boot against a ~24,000-method tier. Cycles are rare on this workload. Kept as a CORRECTNESS mechanism (an intra-cycle import cannot be ordered), never as a performance lever |
| shadow copies — cap sweeps (`WJ_SHADOW_MAX`, `MONO_WASM_JIT_SHADOW_BYTES`) | **CLOSED after ranking, R201/R202.** SELECTION ORDER was the real variable: ranking candidates by **sites/bytes descending** gives 63.7% arm-local with 2.8% FEWER bodies than encounter order at identical caps. After that, raising the caps drove `ShadowCap` to 0 and conversion did **not** move — with ranked selection the candidate SUPPLY is exhausted. **A cap closed as "non-binding" is closed only for the population it was measured on** — an earlier sweep saw 169 shadows where the current stack has 23,202, and its closure had to be retracted. Ships `MONO_WASM_JIT_SHADOW=0`; plateau is ~63% arm-local ≈ ~54% of executed dispatch direct, at **+50% bodies**, and the timing cost of that is still unpriced |
| `shadowNojit` as evidence about the AOT wall | the counter is a TAUTOLOGY: shadow collection walks `WASM_RELOC_CALL`, which only ever names an already-JITted callee, so `nojit` cannot fire. AOT callees emit `WASM_RELOC_AOT` and are never candidates |
| "55% of real call sites target the main module" as a co-location ceiling | RETRACTED (R180) — that is a STATIC SITE COUNT. From 2,598,503 caller->callee edge instances, **93.66% of calls out of our tier land in our own tier** and only 6.24% in AOT code. The AOT wall is not what limits co-location reach |
| **"process-wide modules cannot reach `__thread` state"** | RETRACTED — wrong, and it wrongly closed the monitor inline-CAS lever. See the imported-globals invariant above |
| monitors / inflated locks as a mutex-traffic problem | every lock IS inflated (`monitor.c:1013`: an object whose identity hash has ever been taken can never use the thin lock again, and Java takes identity hashes constantly), but the recoverable part is NOT mutex traffic — `mono_monitor_try_enter_inflated` already has an uncontended CAS fast path. The ~3.2% is CALL OVERHEAD around one CAS. Synchronized wrappers ARE JITted, so an inline CAS has somewhere to attach. Unbuilt, ceiling ~1.5-2% |

## Building: the runtime and the app are two different builds

The **runtime** (this repo) is built and packed with

```
WASM_ENABLE_JSPI=true ../FNA-WASM-Build/build-dotnet.sh . true ./dotnet-jspi.zip     # ~9 min
```

A plain `./build.sh mono` is NOT that build and its pack has to be hand-patched.

Then **deploy it**, which is a separate step and is not optional:

```
scratchpad/mcsr/deploy.sh '<a string constant your change added>'                    # ~12 min
```

Copying the zip over `statics/dotnet.zip` is not a deploy — the bytes the page loads are
`frontend/public/_framework/dotnet.native.<hash>.wasm`, which only changes when the loader is republished.
`deploy.sh` republishes, applies the two mandatory post-publish patches, and **PROVES the marker is in the
served bytes over HTTP**. A whole measurement matrix once ran on a stale runtime.

**Choosing a marker:** it must be a string constant that lands in the wasm — a `printf` literal works. A
`static` function's NAME does not: it can be inlined away and then never appears in the name section. If a
change adds no string, add a verbose-gated `printf` on the path it changes; that also gives you a way to count
the new behaviour. If a change is a REVERT, prove it both ways — the new marker present *and* the reverted
build's marker absent (`grep -ac` on the served wasm).

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
deterministically (bootcheck 0/3) ~44 s in with an assertion at `loader.c:1826` from `interp_delegate_ctor`,
or, with the JIT tier denied for `Sort`, with `RuntimeError: memory access out of bounds`. **Neither is a real
regression** — both are artifacts of the missing AOT half. This once cost most of a session: the non-AOT build
was mistaken for a broken HEAD and the crash chased through eleven knob bisections. **If a boot fails in a way
that looks like a runtime regression, check the build command before believing it.**

## Measurement discipline

The box is an i7-1360P. It sits at ~95-98 C whenever the game is in-game, and that is the normal
operating point of this cooling profile, **not** evidence that a measurement arm was slowed. **The
"throttles 2106 -> 1403 MHz within a single run" claim is RETRACTED** (R227 addendum 3, re-confirmed
2026-09-10): measured live during an in-game arm, the busy P-cores hold **3.6-3.7 GHz**. The old figure
is an ALL-CORE number and this workload is not all-core — only two threads do work (R184), so Dell's
~45 W BIOS PL1/PL2 spread over 2-3 active cores sustains near-peak clocks. (Do not read the RAPL zones:
they report 256 W on a 28 W part because they are ineffective here.)

**The rule splits by workload**: treat all-core work (builds, deploys) as power-limited and expect
throttling; treat 2-3-core measurement arms as running at ~3.7 GHz. preflight's thermal warning is
therefore uninformative on this box rather than merely unactionable. Ambient desktop load is the
confound that IS real — preflight refusing an arm for that is worth obeying.

* **fps is unusable on this box; composition SHARES are not.** Two control runs of an IDENTICAL config:
  `vcall_resolve_fslot` 4.723% / 4.683% (**0.8%**), `InstanceCheck` 2.642 / 2.658 (0.6%) — against **fps 15.10 /
  18.03 (19.4%)**. Client-thread instruction shares are ~20x more reproducible than frame rate. **Quote
  shares; treat any fps delta under ~20% as nothing.**
* **The fps floor is ~12%, not ~5%** (from 359 fault-free runs / 29 repeated identical configs): median
  same-config fpsTail spread is **11.5% on a >=100 s window** and **22.9% on a 60 s one**. Resolving a 5%
  effect against that needs ~10+ rounds per arm. For anything under ~12%, **measure the MECHANISM**
  (`tiershape.py` resolves 0.23%, `hotinsn.py`, a tier dump) rather than the frame rate.
* **Run BOTH ORDERS.** Rounds 1-3 with arm A first gave non-overlapping ranges favouring A, 3/3 — reversing
  the order reversed the result, and the real rule was that **the arm running SECOND was slower in 6 of 6**.
  A single-order interleave cannot see that. **Never quote non-overlapping ranges from one order.** Use
  `mcab.mjs --cooldown-ms` before every arm; interleaving alone does not cancel the bias. **The 240 s
  cooldown is mostly wasted** now that the thermal premise is retracted (~16 min per four-arm matrix);
  R194's order effect stands as an OBSERVATION but its thermal explanation does not, so keep running
  both orders and stop discounting single-order results as thermally confounded.
* **Use plateau windows** (`--warm-ms 180000 --bench-ms 120000 --no-walk`). `--warm-ms` DEFAULTS TO 0, so a
  bare in-game window measures the RAMP: work per frame falls ~40% inside the first quarter of a 120 s
  "plateau" and is flat to ~7% after. Any historical A/B taken without `--warm-ms` was reading ramp position.
* **The ramp is not JIT warmup — it is work we are too slow to clear.** Q1 vs Q4 per frame: mono JIT compile
  ~0 -> ~0, V8 compile 2.0 -> 0.4. Both compilers are DONE before the window opens. What drains is
  MethodHandle/invokedynamic linking (103.5 -> 47.2 M/frame), chunk/world meshing (67.2 -> 27.7) and IC miss
  (33.4 -> 21.1). Native pays the identical transient and clears it in seconds. **Ramp length is a symptom of
  the 4-5x gap, not a separate warmup problem.**
* **Assert the tier is alive before reading any timing**: `registered` unchanged (~1,845-1,863 during
  world.generate on the current config), `tableExhausted 0`, `faults []`. A wrong functype on an import fails
  *instantiation*, not the call, so the method silently falls back to the interpreter — it looks like a
  performance result. A whole session was once run against a dead JIT tier.
* **Measure the mechanism before the outcome.** A tier dump or a `hotinsn.py` run takes minutes and says
  whether a change did what it was supposed to; an fps A/B takes hours and says only whether the number moved.
* **Prefer a within-binary knob A/B to a cross-binary comparison.** A cross-binary reading at this spread
  cannot support a 4% claim however tidy the mechanism sounds.
* **Count ABSOLUTE quantities, not shares of a moving denominator.** Every mechanism in the direct-call path
  changes the NUMBER of arms, so "arm-local %" moves with its own denominator and reads as an effect. A
  matched pair on `colocate_deps`: OFF looks better at 74.6% vs 58.3% arm-local, but ON produces **13,697
  local arms against 12,041** — 13.7% MORE direct calls. The same trap retired three separate conclusions in
  one session. A larger tier is also usually MORE METHODS COMPILED, not bloat — check the module count before
  reading MB as waste. Tier size varies run to run (28,110 vs 30,893 modules), so do not rank two configs on
  <2 points from single runs.
* **Do not compute a share until every route has a counter, and assert the parts are DISJOINT as well as
  summing to the whole.** This has cost four rounds. Once the IC was called 79% of dispatch because the pool
  had no term for the devirt arm; once delegates were put at 43% because two counters were bumped
  unconditionally at the same two sites and double-counted (the real answer is ~20%). An uncounted route does
  not show up as a gap — **it shows up as everything else looking bigger.**
* **A counter that names an action must be bumped where the action HAPPENS, not where it is decided.** One
  read `DelegateDevirtArm 750` with `FastDelegateDevirt 0` for a whole run: the resolution block ran
  unconditionally while the emitting code sat inside a default-off knob's branch. If the decision and the
  action live in different functions or `if` arms, count both and assert they are equal.
* **A diagnostic behind a default-off knob is not evidence of absence**, and "every error counter is zero" is
  a statement about the counters you have, not about the run.
* **Verify a tool's classifier before trusting its output.** Five classifier bugs of the same shape have
  shipped here: a `scriptId === 0` clause that reported our tier as 2.80% of the window when it is ~60%; a
  category ordering that classified `call *0x18(%rbx)` as a memory access and reported dispatch as 0.0%; a
  dispatch mask that counted import calls as `call_indirect` preambles; a load/store split that read every
  indexed store as a LOAD (`heap STORE` 0.01% where the truth is 7.89%); the `-turbofan` suffix match above.
  **The pattern is identical every time: the classifier was written from a plausible mental model of the
  emitted code and never checked against it.** `hotinsn.py --selftest` exists for this and needs no capture.
* **Test a correctness GATE against known-good data before believing its verdict.** A checksum gate once
  printed MISMATCH over 25 runs whose checksums were identical, because the TSV's last field carried a
  trailing newline. A gate that fails CLOSED is as dangerous as a classifier that fails open.
* **A gate that cannot reach the case a change introduces is not evidence about that change.** Two encoder
  oracles passed 400/400 and 300/300 byte-identical across a change they were structurally incapable of
  testing; `enctest`'s t4 exists because of that.
* **When a diagnostic prints nothing, walk the call chain from the printf OUTWARD to the trigger** before
  concluding anything about the symbol. One "missing export" was in fact a harness flag: the dump is reached
  only via `globalThis.dumpWasmJit()`, which `mcbench.mjs` calls only under `--dumps`.
* **A leftover compiler daemon will silently tax one arm.** The runtime build's `VBCSCompiler` starts under
  the REPO-LOCAL `./.dotnet/dotnet` with its own pipe name, so a system-SDK `dotnet build-server shutdown`
  does not reach it. `preflight` refuses to measure while one is alive, and **that refusal is worth obeying
  rather than passing `--force`**: the daemon exits partway through, taxing the arms UNEQUALLY. Use
  `killdaemons.sh` (exit 0 = safe), which matches preflight's own pattern — a filter checking only
  `VBCSCompiler` misses the `MSBuild.dll` half.
* **`pgrep -f` / `pkill -f` MATCHES THE SEARCHER.** This has cost three incidents, twice killing the invoking
  shell (one took a build with it, leaving corrupt intermediates and a *spurious* compile failure). The
  bracket trick does not help when the pattern sits in a heredoc and so lands in the outer shell's own argv.
  **Find candidates by command line, keep only those whose `comm` is the expected executable, never kill
  self**, and prefer `pgrep` + explicit `kill <pid>` over `pkill`.
* **NEVER EDIT A SOURCE FILE WHILE A BUILD OF IT IS IN FLIGHT.** The build reads each file at a moment you do
  not control, so a file edited during it makes the binary's contents *unknowable* and every number taken on
  it unusable. One arm came back at 1.45x its control and could not be attributed. The edits were behind a
  default-off knob and *very probably* inert — which is exactly the trap. A build is ~9 min and a deploy ~12;
  treat that whole window as read-only and queue the edits.
* **The network stall is defused** — `IKVM_DEFUSE_RANKED_AUTH=1` is the harness default. MCSR Ranked's account
  validation never reaches a terminal socket status under this net bridge, so the modded main screen never
  finishes; that is the "no log output at all for 90s" signature. It is BENCHMARK-ONLY and it CHANGES WHAT THE
  GAME DOES, so it goes through the URL like every other knob and stays visible in each run's recorded command
  line.
* **When testing whether a hung run recovered, filter to lines the APP emits** (`src/dotnet/log.ts`,
  FabricLoader, IkvmClassLoader). Chromium keeps writing `gcm/registration_request` and `gpu_blocklist` errors
  for minutes after the page is dead, and counting those as progress once produced a confident "it recovered"
  that was exactly backwards.

### Long-running work: put it in a transient systemd unit

**Background tasks are killed when their owning agent exits unless it is async.** The harness's cleanup calls
`killShellTasksForAgent`, and having a backgrounded shell is not enough to skip it. The kill acts on the
spawned process handle, so `setsid` + `disown` does **not** protect the work — the real work must live in a
cgroup the handle has no reference to.

```
systemd-run --user --quiet --unit=<name> --working-directory=<dir> -- bash -c '<work>; echo $? > <sentinel>'
```

then poll the sentinel. **Use a transient SERVICE, not `--scope`**: a scope runs in the calling process's
session and dies with the invoking shell exactly as `setsid` does. **And do not rely on a backgrounded watcher
for notification** — a `run_in_background` watcher is killed after ~5 minutes while the unit it watches runs
to completion. Put the WORK in the unit and use a direct sentinel check as the backstop.

**Symptom of getting this wrong:** the run's browser AND its node driver vanish together, the sentinel is never
written, and there is NO stall message. Do NOT diagnose it as an app or JIT fault. Three explanations were
asserted here before the harness source was read and all are wrong: a duration limit, memory pressure, and
"~30 minutes of user inactivity" (a selection effect — killed tasks have a median lifetime of 1.5 min).

### Debugging a wasm trap

* **Raise the stack limit first.** V8 defaults to **10 frames** and every `WORKER_TRAP_STACK` this harness
  captured was truncated at exactly that, which is why traps routinely read as runaway recursion and why
  callers stayed invisible. `--js-flags=--stack-trace-limit=64` is now unconditional in `mcdrive.mjs`'s
  `launchGame`. It must be `--js-flags`, not `Error.stackTraceLimit` from an init script: the flag is
  process-wide and so reaches WORKERS, and workers are where these faults happen.
* **Symbolise; do not read the top frame and guess.** V8 reports MODULE-relative offsets and the split DWARF
  is CODE-SECTION-relative:

  ```
  dwarf_addr = v8_addr - 0x1ac2fc          # the CODE section VMA; confirm with llvm-objdump --headers
  llvm-symbolizer --obj=<...>/loader/obj/dotnet.native.debug.wasm --inlines <dwarf_addr>
  ```

  Confirm the base by looking up the enclosing subprogram's `DW_AT_low_pc` first. `llvm-dwarfdump --lookup`
  on the UNrebased address silently returns nothing, which reads like "no debug info" rather than "wrong
  base". **The symbolizer's inline function NAMES are mis-attributed under LTO — ignore them; the LINE
  NUMBERS come from the line table and are correct.**
* **A truncated stack and a runaway stack look identical** unless you check whether the last frame is a
  plausible root.
* **Date a trap class before reading code.** Grepping archived logs for the class (restricted to stacks
  containing the frame that characterises it) costs one command and has killed a plausible
  "the recent commit did it" theory outright.
* **An intervention against an intermittent fault needs a POSITIVE CONTROL.** At ~33% incidence, four
  consecutive clean runs mean little; run the control arm too.

## The instruments

Everything below is in `scratchpad/wj/` and is **untracked** — treat the tools as valuable and the captures as
disposable. Start with these rather than `perf report`, which takes minutes per query on these captures.

### Drivers — run a real workload

| tool | what it does |
|---|---|
| `worldwait.mjs` | **The current default for fault and counter batches.** `--runs N --stats --bench-ms N --knob K=V`; drives boot -> world -> in-game, prints per-run `faults=[...]`, the admit/tier line, partition and memory sampling |
| `mcab.mjs` | Interleaved A/B over full Minecraft with the variance made explicit instead of assumed away. `--cooldown-ms` |
| `mcbench.mjs` | Wall-clock A/B, one fresh isolated browser per run. `--stats --dumps` to reach `dumpWasmJit()` |
| `mcperf.mjs` | Non-interactive perf capture of ONE run, sliced into the three phases. `--warm-ms` matters |
| `bootcheck.mjs` | Boot-only PASS/FAIL for one configuration, ~2 min instead of ~9 |
| `arm.sh` | One arm of a boot-crash A/B: N sequential bootchecks under one knob set, reported as crashes/valid |
| `bench.mjs`, `benchseeded.mjs`, `benchstats.mjs` | The jbox2d fixed-work kernel — low variance, checksum-gated, the right tool for a within-binary A/B and for cross-VM reference rows. `benchstats` captures the JIT's own stats lines |
| `perfrun.mjs` | Perf capture of one **jbox2d** bench run, producing the `.jitted` file the pattern/size readers take (the kernel-workload counterpart to `mcperf.mjs`) |
| `seedbuild.mjs` | Rebuild `scratchpad/mcsr/seed` (see Housekeeping) |
| `serve-coep.mjs` | Minimal static server with COOP/COEP headers |
| `nativemc/natrun.sh`, `natstat.py` | **The native reference.** Drives native OpenJDK Minecraft on the MATCHED Prism instance (same save, mods sha256-identical) and reports **M instructions/frame** per thread, the same metric as our side. `EXTRA_JAVA=` for ablation arms (`novirt`, `noinl`). Read its README before use: three defects in the old `jvminline.sh` and two Xwayland traps are documented there |
| `nativemc/join.py`, `split.py` | Per-method join of an async-profiler collapsed profile against our census, on IKVM-preserved Java names; and the bucket split by whether a method exists on native at all |
| `jvminline.sh`, `jvmfps.py` | **SUPERSEDED by `nativemc/`** — it reports fps (unusable) and its in-world gate polls `wpstateout.txt`, which state-output 1.2.3 never creates, so it can never pass |

### Capture readers

| tool | what it does |
|---|---|
| `perfraw.py` | **Primary.** Parses `perf.data` directly (0.6 s for 299 MB), resolves against the `--perf-basic-prof` map or the jitdump, and **reads the call chains**. `--phase ingame --by-thread --inclusive`, `--tid N`, `--marker SYM`, `--tsv`. Handles fixed-period captures (`perf record -c N`), whose sample_type omits PERIOD and shifts the callchain 8 bytes |
| `hotinsn.py` | Resolves each sample to the exact x86 instruction. `--bands` (prologue vs body), `--dispatch`, `--kind ours\|aot\|v8`, `--feeder` (splits the `add %r14,reg` decompression pool by what fed it — the `s.p` attribution). **Run `--selftest` before quoting a feeder number** |
| `callform-x86.py` | Disassembles `call *` sites out of a jitdump and groups them by what the preamble CONTAINS. Use this whenever a claim depends on telling `call <import>` from `call_indirect` — only the latter has the table bounds check |
| `codegencensus.py`, `spillsites.py` | What KIND of instruction the hot set spends bytes on; where inside a hot method TurboFan spills |
| `pattern.mjs`, `split.mjs` | Program-wide attribution by EMITTER CONSTRUCT; subsystem split of steady-state time |
| `mcreport.mjs`, `mccause.mjs`, `mcdiff.mjs` | Per-phase attribution (thread / binary / subsystem); inclusive cost of a cause and who is responsible; compare two CAPTURES for changes no knob can toggle |
| `symclass.py`, `lib/symclass.mjs`, `jitsym.py` | Symbol classes from a `perf report` dump; the classifier library; resolving samples against a jitdump without `perf inject` |
| `jitdumpsize.py`, `pairsize.py` | Emitted x86 size per JIT'd function; paired RyuJIT-vs-ours x86 size for the SAME method |
| `gcshare.py`, `windecay.py` | STW GC share of the window; whether fps plateaus or keeps sliding |
| `offcpu.sh`, `offcpu.mjs`, `threadwait.mjs` | Off-CPU attribution during the in-game window; per-thread run / runqueue-wait / sleep accounting and what each thread waited on |
| `stallcatch.sh` | Capture perf until a STALLED world.generate lands |
| `replaylogs.mjs` | Re-derive phases from ARCHIVED logs using the driver's own markers — no re-run needed |
| `memtrace.mjs`, `workerheap.mjs` | Parse a chromium memory-infra trace; per-worker JS heap snapshots |

### Tier dumps and static analysis

| tool | what it does |
|---|---|
| `wasmtier.mjs` | Snapshots the ENTIRE tier (~24-30k modules) as emitted bytecode over one automated run. Passes `dumps: true`, so **one run yields BOTH the call-form reach and the counter census** — pairing a reach number with a census from a different run is the same confound as a cross-binary reading |
| `vcallreach.py` | Static virtual-dispatch census over a tier dump: sites split into Delegate.Invoke vs ordinary virtual, how many carry a devirt arm, and **what call form each arm dispatches with**. Self-validating — every body must decode to exactly its declared end, so **report the "undecoded" count**; a non-zero one means the rest is meaningless. This, not `hotinsn`, is the tool for "did a change alter call forms" |
| `devirtreach.py`, `wjpatterns.py` | Static devirt COVERAGE over a dump; what share of module-local calls could V8 inline |
| `tiershape.py` | **The refactor gate.** Per-method STRUCTURAL identity between two dumps (opcodes, type/function/local indices, branch depths, run-dependent immediates blanked). **Noise floor is ~0.7%, NOT the 0.15% this table used to claim** — measured 2026-09-11 against a same-binary CONTROL: **158 of 22,816** common methods differ between two runs of ONE binary. The old figure (18 of 12,291) is stale because the tier nearly doubled, and more methods means more callees that JIT in one run and not the other. **ALWAYS run a same-binary control pair alongside the A/B**: at 0.15% a verified-inert refactor reads as a 4x failure (R241 measured 0.63% against a 0.69% control). Also pass `--show` large — it prints ~20 diffs and truncates, and the visible ones are not a representative sample |
| `tierid.py` | Per-method BYTE identity, as a SET comparison (a method can be emitted more than once per run, so one name maps to a set of hashes and two runs agree when the sets are equal). Complement to `tiershape.py`, not a replacement — a naive byte-identity gate does not work |
| `tierdiff.py`, `tiergraph.py` | Aggregate shape between two dumps; duplication/fragmentation structure of the tier |
| `bodysize.py`, `inlinable.py`, `bytecost.mjs`, `depcycles.py` | Per-function wire size and the share under V8's 500 B inline cap; how many sites batching made inlinable; whether module SIZE costs anything or only COUNT; co-location admission cycle sizes |
| `memsplit.mjs` | Splits our memory traffic into GC-shadow-frame vs real heap field/array access |
| `compare.mjs` | Side-by-side codegen: our output vs TeaVM wasm-gc for the same Java source (`~/Documents/ikvm-wasm/teavm`) |

### Live telemetry over CDP

| tool | what it does |
|---|---|
| `sigtrap.mjs` | **Catches `function signature mismatch` IN THE ACT and reads the function table at the faulting slot.** The right first instrument for any f-slot/placeholder trap |
| `trace-exceptions.mjs` | Fully headless CDP exception tracer |
| `callform.mjs`, `profstat.mjs` | Call-form census / call-profile telemetry read at titleReady |
| `jitcov.mjs`, `statsdump.mjs` | What registered, what bailed, what is blocking islands; counters before/after the timed batches |
| `patchstats.mjs` | Per-isolate guard-free-adapter install tally (`globalThis.__wj_patch_stats`) |
| `ringprobe.mjs` | Census of OUTBOUND interp-residual callees, deduped on the ring's monotonic index. The ring holds 128 entries against ~58 residuals/step, so **read the SHAPE, not totals**. Requires `MONO_WASM_JIT_STATS=1` |
| `lcsereach.mjs` | LCSE reach across the warmup boundary |
| `console.mjs` | Dump matching console output of one run (for `DUMP_IR` / `VERBOSE` / `STATS` output) |

### Gates and hygiene

| tool | what it does |
|---|---|
| `csyn.sh` | One-file syntax check with the real build's command line, both compilation databases. Sub-second |
| `enctest/run.sh` | Four host-side encoder gates in seconds. t1/t2 diff against frozen framers, t3 is the serializer round-trip, **t4 is structural** — it checks the assembler at `nexport < nmembers`, which t1/t2 cannot reach. Run it before believing anything else about the encoder |
| `killdaemons.sh` | Kills leftover Roslyn/MSBuild build servers using preflight's own match, safely. Exit 0 = safe to measure. Run before every measurement |
| `wjcsync.py` | Re-index the JS counter mirror after `WJC_*` entries change in the C enum. **Use this instead of hand-editing the mirror in `lib/mcdrive.mjs`** |
| `lib/preflight.mjs` | Refuses to measure on a box that is unfit (thermal, CPU contention, build daemons). Obey the refusal |
| `lib/` | `mcdrive.mjs` is the Minecraft driver (launch, seed, phases, counters, memory/partition sampling, knob plumbing); `browser.mjs` is chrome/CDP for the jbox2d-era tools; also `provenance.mjs`, `thermal.mjs`, `wasmnames.mjs`, `debuginfo.mjs` |

**Known harness gap:** one MONO forced abort wedges a whole batch — the browser process is gone but the driver
keeps waiting (observed at 886 s and 1,267 s). Every batch that hits such a fault loses its remaining runs.

## Housekeeping

`scratchpad/` is excluded via `.git/info/exclude`, not `.gitignore` — it holds tens of GB of captures and must
never be added. **It is also not recoverable**: a deleted tool is gone. Chrome profile dumps are ~1 GB each and
jitdumps ~4 GB; keep the newest of each kind and delete the rest. Do not commit `*.orig-backup` files.

`perf inject` is opt-in (`--inject`), not implied by `--jitdump`: every python reader here parses the raw
jitdump directly, inject's success path DELETES those dumps, and it was measured still running at 8m20s on a
3.1 GB dump. Capture size 9.2 GB -> 4.2 GB with it off.

`perf` also **silently mis-attaches**: one capture produced a 0.3 MB / 3,967-symbol map and 133 samples where a
good run has ~206 MB / ~2.0M, and it reads as a valid low-overhead run (its fps was the highest of the night).
**Gate on the symbol-map size.**

**`scratchpad/mcsr/seed` is load-bearing and is NOT crash-harness leftovers.** It is a ~690 MB browser profile
holding Minecraft 1.16.1 + Fabric, the mods, `options.txt` and the "New World" save in OPFS. `mcdrive.mjs`
reflink-copies it for every run; without it the app re-downloads Minecraft *inside the boot phase* and boot
measures the network. It is easy to mistake for junk because it sits in the otherwise-dead `mcsr/` directory —
it was in fact deleted once during a cleanup. Rebuild with `node scratchpad/wj/seedbuild.mjs` (~1 minute; the
dev server must be up).
