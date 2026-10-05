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
| the backend | `src/mono/mono/mini/mini-wasm.c` + five `#include`d `.inc` files, `wasm-encoder.c` |
| ... `-emitter.inc` | IR -> wasm lowering, the biggest piece |
| ... `-ir.inc` | the relocatable body (`WjBody`), `wj_assemble`, the call forms |
| ... `-batching.inc` | co-location: the planner, `rebatch`, `batch_bind`, rollback |
| ... `-publish.inc` | asynchronous republication: the epoch log, the per-worker drain |
| ... `-diagnostics.inc` | counters and dumps |
| ... `mini-wasm.c` itself | knobs, the registry, admission, the GC shadow frame |
| the assembler (`wj_assemble`) | resolves a body's relocations and frames N members into one module |
| JIT <-> interp boundary | `src/mono/mono/mini/interp/interp.c`, `ee.h`, `transform.c` |
| the app + shipped knob set | `~/Documents/ikvm-wasm/ikvmcraft`, `frontend/src/dotnet/index.ts` |
| IKVM (Java -> CLR) | `~/Documents/ikvm-wasm/ikvm-wasm-build/tools/ikvm/ikvm` — branch `wasm`, and it is CLEAN; the uncommitted IKVM-side work is in **ikvmcraft** (`loader/IkvmWasm.cs`, `loader/Transforms/Bench/`) |
| measurement harness | `scratchpad/wj/` (see **The instruments**) |
| **the running log — read before proposing anything** | `scratchpad/wj/MINECRAFT-FINDINGS.md` |
| chromium/V8 source, for checking claims about V8 | `~/Documents/chromium/src` (v8 at `src/v8`) |
| split DWARF for the shipped wasm | `~/Documents/ikvm-wasm/ikvmcraft/loader/obj/dotnet.native.debug.wasm` |
| **the canonical emsdk** (3.1.56) | `~/Documents/ikvm-wasm/ikvmcraft/statics/emsdk` — frozen from `~/fna-wasm/FNA-WASM-Build` (`emsdk*.patch`, applied by `freeze-emsdk.sh`). The app links `dotnet.native.wasm` against ITS cache (`IkvmWasm.csproj` `WasmCachePath`), so a libc/pthread/wasmfs question is answered there, not in this repo's `src/mono/browser/emsdk`. Patching a system-library source means rebuilding the archives that hold it (`embuilder build --force <lib>`, `EM_FROZEN_CACHE=0`) and redeploying |

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
5. **A failed experiment belongs in `MINECRAFT-FINDINGS.md`, not in the source.** Its result and the
   reason the reasoning was wrong are what stop the next reader re-running it, so they must survive --
   but in the log, cited from code by round (`/* ... see R258 */`). Keep in code only what a reader must
   know to edit the line in front of them: the invariant, the default, and the trap that is not visible
   locally. When you evict a narrative from a comment, paste it into the log in the same commit.
6. **If you cite V8 behaviour, cite the file.** The source is checked out locally; "V8 probably..." is not a
   reason to ship anything.

## Verified V8 facts — do not re-derive, do not guess

Checked against `~/Documents/chromium/src/v8`. The facts below were verified on V8 15.4.77; the tree is now
**15.5.35** (`include/v8-version.h`), so a cited line number may have drifted, and the INSTALLED browser is a third
version (R233 add.4). If something surprising turns up, re-check against that tree rather than trusting this list.

* **Every `WebAssembly.Module` is its own code space, and every worker re-validates it** (R345). `NewNativeModule`
  reserves a fresh RWX region per module (`wasm/wasm-code-manager.cc:2617-2654`), at least 2 x
  `OverheadPerCodeSpace` (a far jump table of ~153 builtins x 16 B) ≈ 8 KB after page rounding, all of it counted
  in VmData. Large RWX VMAs do NOT show pooling: V8 places reservations back to back (a sequential
  `next_code_space_hint_`, `:2335-2336`) and the kernel merges them into one VMA. And `SyncCompile`
  decodes and validates every function body BEFORE the NativeModuleCache lookup (`wasm/wasm-engine.cc:615-640`,
  lookup at `module-compiler.cc:2079`), so a cache hit saves compilation, not validation.

* **`local.get` / `local.set` / `local.tee` emit zero instructions.** `LocalGet` is
  `result->op = ssa_env_[imm.index]` (`wasm/turboshaft-graph-interface.cc:1030-1043`). Local count, reuse and
  copy chains are a **wire-size** question only. What costs is how many values are live across a call.
* **V8 can never inline anything this emitter produces.** Inlining candidates come from a module's own call
  sites, and an imported function has `wire_byte_size_ == 0` so its score is 0 (`wasm/inlining-tree.h:79-85`).
  One method per module ⇒ every cross-method call is a real call, permanently. **mono's own inliner is the
  only inliner in the pipeline.** The one qualification (R351): V8 collects `call_indirect` feedback and
  speculatively inlines SAME-INSTANCE targets (`CallIndirectIC`, `builtins/wasm.tq:821-835`;
  `wasm_inlining_call_indirect` defaults true); cross-instance calls are marked and never inlined. Every f-slot
  call crosses instances today, so this changes nothing yet -- but co-locating an IC's hot target in the same
  module makes that IC call inlinable too, not just the calls converted to `call <funcidx>`. Relevant flags: `wasm_inlining_max_size` **500** wire bytes, budget 5000 TF
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
* **A function may declare at most 50,000 PARAMS PLUS LOCALS.** `kV8MaxWasmFunctionLocals`
  (`wasm/wasm-limits.h:50`); `num_locals_` is seeded from the signature's parameter count
  (`wasm/function-body-decoder-impl.h:1880`), accumulated per local group (`:1938`), and a group that
  crosses the cap is rejected with **`local count too large`** (`:1920-1921`). This is a `CompileError`
  from `WebAssembly.Module()`, i.e. the module never instantiates. The emitter refuses such a method
  before emitting (`WASM_MAX_FUNCTION_LOCALS`, counter `locals_overflow`); 1.16.1's
  `BlockStateFlattening:.cctor` is the known member and emits 1.9 MB. Note this is the ONE place where
  local COUNT is not purely a wire-size question -- see the local-ops fact above for everything else.
* **V8's implicit null checks are WasmGC-only** (`null_checks_for_struct_op`, `wasm-lowering-reducer.h:405-425`).
* **V8 eliminates no linear-memory load, and has no wasm LICM.** Every wasm memory load is built
  `.NotLoadEliminable()` (`wasm/turboshaft-graph-interface.cc:8268-8284`); the wasm optimize phase is
  LateEscapeAnalysis, MachineOptimization, MemoryOptimization, BranchElimination, LateLoadElimination,
  ValueNumbering (`compiler/turboshaft/wasm-optimize-phase.cc:25-28`). So every load mono emits executes, and load
  CSE / LICM can only happen in mono's middle end (R378). Checked on 15.5.35.
* **Classifier warning:** a case-insensitive match for "compile" hits the `-turbofan` SUFFIX on every symbol
  and reports ~88% of the window. Strip `-\d+-(turbofan|liftoff)$` before matching.

## Verified mono-on-wasm facts

* **A deterministic compile failure must never be marked `retriable`.** `retriable` routes to
  `WASM_JIT_COMPILE_BLOCKED` -> **PARKED with `wasm_jit_hits` reset** (`interp.c`), re-attempted a
  threshold later -- and a long-running method re-accrues that threshold on its own back-edges in
  ~1.5 s. So a condition that cannot clear recompiles forever: `BlockStateFlattening:.cctor` ran 15
  full compiles of a 1.9 MB body back to back and ate **22.4 s of boot** with `registered` flat and no
  fault logged (R289). The split that holds: **`CompileError` is deterministic** (re-emission produces
  the same bytes) and is now permanent; **`LinkError` is per-worker** (imports are per-worker) and stays
  retriable. Measured over the whole archived corpus: 20,499 CompileError vs 131 LinkError.

* **`mono_thread_info_yield()` IS A NO-OP.** `mono_threads_platform_yield()` is `{ return TRUE; }`
  (`utils/mono-threads-wasm.c:169-172`). So any `while (CAS...) mono_thread_info_yield ();` in this tree
  is an unbounded, unyielding busy spin, and "spin politely" is not a thing that happens here. Both CAS
  loops in the backend were that shape and are now test-and-test-and-set, bounded and counted
  (`[wasm-jit spin]`). **A shared spinlock is also the only thing that can put two DIFFERENT threads on
  the SAME ~22 bytes of generated code**, which is what R275 measured on the world-load hang — so this is
  the first thing to price against any "two threads at 92% CPU" reading.
* **Nothing that blocks properly is safe at those sites.** `mono_thread_info_sleep(ms>0)` does
  `MONO_ENTER_GC_SAFE`, and *leaving* a GC-safe region is itself one of the rendezvous-drain call sites
  (`mono-threads-coop.c:435`) — so sleeping inside a JIT lock re-enters the drain from under a lock the
  drain can want.

## Architecture invariants — check a design against these before building it

### Bytes are process-wide; instantiation and admission are per-worker

There is no module broadcast. **The emitted BYTES are published once and are identical for every worker**;
each worker then calls `new WebAssembly.Module(bytes)` and `new WebAssembly.Instance(module, imports)`
**itself**, supplying **per-worker imports** (`mini-wasm.c`, the `EM_ASM` at the instantiate sites):

```js
{ m: { h: wasmMemory },      // shared linear memory
  f: { f: wasmTable },       // this worker's function table
  x: { e: wasmExports["__cpp_exception"] },
  s: { p, l, c, v, n, d, m, b, i, g } }   // the per-thread globals, indices 0..9
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
  `s.b` = this worker's scratch base, `s.i` = `&mono_wasm_jit_cur_island_il_state`, `s.g` = this worker's
  SAFEPOINT ACTION WORD (see below). Cost is one load, not the four a mutable import costs. **Adding a
  global is not free**: indices 0..9 are a hard-coded contract with the emitter. The import COUNT is now
  derived from the name table rather than hand-maintained; when it was not, falling out of step produced
  "section was shorter than expected size" and `registered` 0 from boot.

  **`s.g` is how a worker learns anything at all.** It is one i32, per worker, in a process-wide slab (not
  `__thread` -- other threads write it). Non-zero means the emitted safepoint must take its out-of-line
  helper: the GC wants to suspend, or a code publication has not been adopted here yet. Either writer only
  ever SETS; only the owning worker clears, by re-deriving both conditions. Folding the two into one word
  is what keeps the emitted check at ONE LOAD -- testing them separately cost two extra loads, an `i32.ne`
  and an `i32.or` on every loop back-edge in the tier (R268).
* **INSTALLATION is per-worker, which is why admission gates entry** rather than the emitted code testing
  anything.

### A JS worker outlives its pthreads — "per-worker" and "per-thread" are NOT the same thing (R293)

Emscripten reuses workers: when a pthread exits, `returnWorkerToPool` frees its block — struct, TLS **and
stack** — and the next `pthread_create` can hand the SAME worker (same wasm instance, same function table,
same JS realm) to a new pthread with a new TLS block. A run takes up ~60-80 pthreads on a 16-worker pool
(`[wasm-jit bounds] worker_slots`). So the function table, `infoTable`, `Module.__wjSlotFn` and every JIT
instance are **per worker**, while `wj_slot_live`, the PICs, `wj_scratch` and the island state are
**per pthread** — and every JIT instance imports the INSTANTIATING pthread's `__thread` addresses.

The new pthread's own state starts empty, so anything it admits is re-instantiated correctly. What it can
reach without admitting is whatever the worker kept: the jiterpreter's interp-entry trampolines (which bake
`&wj_slot_live`) and guard-free adapters, and through them the old instances — running against FREED TLS.
Measured with the reset off: **158,104 helper calls in one run were handed another pthread's scratch**
(`foreign_scratch`), and R292's recurring OOB is the first `*s.i` store of exactly such code.
`mono_jiterp_wasm_jit_worker_reuse` now runs as each pthread is set up on a worker
(`MONO_WASM_JIT_REUSE_RESET`, `[wasm-jit reuse]`; `foreign_scratch` must read 0): the guarded trampoline
reads the CURRENT pthread's bitmap through a per-worker cell it re-points, guard-free adapters are demoted to
it, e/f slots go back to the placeholder. **Do not "fix" this by discarding trampolines** — the first version
did, and a taken-up thread was then left on the C `interp_entry` boundary (~4 M instr/tick on the server
thread in 2 of 3 runs, R293b), because the only adapter install that works is at flush time. `slow_live`
counts exactly that cost.

**Before storing anything per worker that code will consume, ask what happens when the pthread that
built it has exited.** A `__thread` address baked into a module, a trampoline or a JS-side cache is a
pointer into memory that will be freed while the thing holding it stays callable.

### Admission's contract, and why generated code carries no liveness check

Generated code `call_indirect`s an f-slot with **no liveness check at all**. The entire job of the admission
DFS is to install a method's complete direct-call closure *before* that method may go live. Therefore:

* Proving a dependency is the right method with the right ABI is **not** the same as proving its slot is
  installed on this worker. Both must be checked, and the one that matters at runtime is
  `mono_wasm_jit_slot_live(<the f-slot the caller baked>)`.
* Refusing admission is **recoverable and normal**: return 0, state 0, and the next dispatch retries — but
  **only for a TRANSIENT condition, and the retry rate is not self-limiting.** "Refusals in the hundreds per
  run cost nothing" was this file's claim and **R244 measured 14-23 MILLION per run**
  (`admitDepNotLive`, 99.97% of all refusals), costing 26% of the client thread and 39% of the server tick,
  with `registered` perfectly flat the whole time. A refusal discards the entire DFS walk and the next
  dispatch redoes it, so **routing a PERMANENT condition into the retry path spins forever.** The specific
  trap: `mono_wasm_jit_admit(dep)` has already installed the dep at its CURRENT f-slot by the time
  `mono_wasm_jit_slot_live(ds->slot[i])` tests the slot the CALLER BAKED — if the callee re-registered, that
  slot can never become live. **Before adding a refusal, decide whether its condition can ever clear, and
  count it; `registered` staying flat is not evidence of health.**

### The prefilled placeholder — `table[fslot] != null` is NOT a liveness test

`mono_jiterp_allocate_table_entry` hands out slots from a range the jiterpreter **prefills with a real,
callable function**: `mono_jiterp_placeholder_jit_call`, signature `(i32,i32,i32,i32)->void`, body
`*thrown = 999` (`interp.c`, filled in `jiterpreter-support.ts`). A slot this worker has not instantiated:

* **traps** if you `call_indirect` it with any other signature — this is the `function signature mismatch`
  class; and
* **works** if the expected type happens to be that one common shape — writing 999 through the caller's
  fourth argument as a pointer. Silent heap corruption, no LinkError, no trap, no diagnostic.

The authoritative test is the per-thread bitmap `mono_wasm_jit_slot_live()`. The function table is
per-WORKER for dynamic entries, so a process-wide bitmap cannot answer this; the bitmap is per-PTHREAD,
which is stricter than the table and is only correct because a worker take-up resets the table to match
(R293, above). `Module.__wjSlotFn` is the worker's record of what it installed, not a liveness test.
**Before binding, calling or trusting anything found at an f-slot, ask whether THIS thread put it there.**

**The converse invariant is also load-bearing:** an f-slot only ever holds a JIT `f` from a module this
emitter produced. AOT bodies are reached through their own table indices and their own `at`/`at_ne`
functypes, never through an f-slot. That is what lets a caller bake a functype for an f-slot call with no
runtime kind test, and what makes an ABI change safe to apply to every f-slot functype at once.

### mono's rare paths are our COMMON ones, so the JIT must not assert on metadata it is handed

A type-load failure is exceptional on most workloads. **Under IKVM it is ordinary control flow — an
unloadable type IS a failed load** — so upstream code that treats it as "cannot happen" fires here
routinely. `compute_bb_regions` was the one consumer of `clause_is_dead` that did not guard
(`mini.c:2386`, `:2535`, `:3960` and `mini-llvm.c:13480` all did), so
`method_make_alwaysthrow_typeloadfailure` — which removes every basic block and marks every clause dead —
left the clause table naming blocks that no longer existed, and the JIT aborted the process instead of
compiling a throw stub.

The general rule this makes explicit: **a bad body arrives as a COMPILE REQUEST, not at load time.**
`mono_interp_replace_method_body` swaps a whole header into a live wrapper ~4,500x per run, so the JIT can
be handed an inconsistent one at any moment, and `g_assert` there loses the entire run over one method the
interpreter is executing correctly. Convert to a counted bail on the existing pattern
(`mono_wasm_jit_note_bad_eh_clause`), and put the bounds check FIRST — the original line indexed the array
before the assert could fire, so an out-of-range offset was already an OOB read and the abort was the good
case.

**But check reachability before converting anything.** Four asserts on that list turned out to be in
`get_call_info`, which is `static` with zero callers on this target, and whose only failure value
(`ArgInvalid`) is handled by no consumer — so converting them would have turned an unreachable abort into
reachable silent bad codegen (R282).

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
  The lazy pool's reservation (`mono_wasm_jit_lazy_im`) genuinely needs the creation -- the reservation lives on
  the InterpMethod -- as the island DFS did before it was deleted (R366), where peeking wedged boot.
* **A guard that catches a race must COUNT its catches, and non-zero is then the healthy reading.** A catch
  counter stuck at 0 forever means the guard is dead code, not that the race is impossible.

### An EH method's LMF record lives in its own frame (R353)

With `MONO_WASM_JIT_EH_REC=1` (the default) an EH-bearing method links a `WjEhRec` -- a MonoLMFExt plus its
il_state -- from its OWN C-stack frame into the thread's LMF chain, where the island design used a per-thread
chunk array. So **an exit that skips the unlink leaves the chain pointing into dead stack**, which the next
frame overwrites; the island equivalent pointed into stable memory and only went stale. Every exit therefore
unlinks: EMIT_REF_LEAVE and the landing pad's no-handler rethrow, inline when the record is the head, else through
`mono_wasm_jit_ehrec_unlink`, which unlinks through a stale INNER LMF but leaves a head pass 1 rewound PAST the
record (restoring that resurrects retired frames). The interp->JIT boundary pops records of frames below its saved
C SP. **Before adding an exit path to an EH method, or a way out of one that is not a C++ unwind through its
landing pad, decide how it unlinks the record.** `[wasm-jit ehrec] dispatch_norec` must read 0.

### Lazy tier-1 f-slots: a pool slot is callable before its callee exists (R361-R365)

A direct call to a callee that is not JITted and not AOT-backed does not fail the compile (the island machinery that
used to force-compile such callees is deleted, R366): the emitter RESERVES the callee an e/f pair from a per-functype pool
(`mini-wasm-lazy.inc`), and the f-slot holds, on every worker that instantiated its BANK, a stub of that functype
(`wasm_module_lazy_bank`) that binds the real callee through `mono_wasm_jit_late_fslot` or runs it in the
interpreter. This turns three of the invariants above around, so before touching admission, installation or no-GC
credit:

* **A pool slot's real f is written only by admission, after its dependency walk.** Callers go live against the
  STUB, so the moment a real f lands in the slot every caller on that worker calls it. The compiling worker therefore
  validates without installing (the R322 path), `mono_wasm_jit_instantiate_local`/`_batch_local` instantiate the
  slot's bank BEFORE any real f in it (a later bank instantiation would write the stub back, and admission's cache
  would then hand the stub its own slot to tail-call forever), and bind checks the table holds what this worker
  installed (`WJC_LAZY_REPAIR` counts a violation instead of hanging).
* **Admission never descends into a pool dep**; it ensures the bank. The one exception is a callee registered no-GC
  AT that slot, which is admitted for real so a caller may be credited for it (`wj_lazy_dep_needs_real`).
* **A pool slot is not credited no-GC otherwise** -- bind can compile, instantiate and run a cctor. Because callers
  now compile BEFORE their callees, that credit is mostly unavailable at tier 1 (pin elision 96% -> 85%, R362);
  tier 2 re-emits after the callees exist and gets it back.
* **"Bank instantiated" is per pthread** (a worker take-up re-applies every stub, R293), and a bank bakes no TLS:
  the stub gets its scratch from a helper, never `s.b`.

A callee the pool refuses (AOT-backed, permanently un-JITtable, a wrapper other than synchronized/dynamic-method, a
functype it cannot stub) takes the residual -- nothing brings it up by force any more, and a devirt arm whose target
cannot be reserved is simply dropped. Must read 0: `[wasm-jit lazy]` orphan, bank_fail, repair, notreal, drift, dead.
`bind` is the liveness check, `residual` counts the refusals routed through the interpreter. Against the island
path: registered -39%, instantiations -43%, peak VmData -700 to -900 MiB (first config under the sandboxed 8 GiB in
every soak run), boot not worse, server tick neutral over four batches (R361-R365).

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
  time so the main-thread JS dump never takes a lock. Folding it in would spend the profile's bounded site
  slots (12 per block, up to `MONO_WASM_JIT_PROF_BLOCKS` chained blocks, R311) on a different question and
  change which sites survive eviction — i.e. change codegen.

And one trap already paid for: **a stable inline-cache id is not the same granularity as a profile record.**
The record is keyed by callee base method; an IC belongs to one call site. Reusing the record's id makes two
sites in one method that call the same base share a PIC slot.

### Retained raw `MonoMethod *` are not always dereferenceable — PARTLY GUARDED

The registry and the call profile keep raw `MonoMethod *` for the process lifetime while IKVM generates
dynamic types continuously, so such a pointer is not always dereferenceable by the time the emitter or
admission consults it. **It has three faces, not two**: `memory access out of bounds` inside
`jit_mm_for_method` / `mono_interp_*_imethod`; an assertion in `mono_class_get_flags`
(`class-accessors.c:90`, which arrives *inside* a mono assertion and so is easily misfiled as a managed
cast failure); and a **misaligned `a_cas`** inside `mono_mem_manager_lock` — alignment is a hint for
ordinary wasm loads but is ENFORCED for atomics, so a misaligned CAS means the mutex address was garbage.

**`peek` does not protect against this, and two rounds treated it as though it did.**
`mono_interp_peek_imethod` avoids CREATING an InterpMethod (a different hazard, an uninitialised
`interp_code_hash`); its first statement is still `jit_mm_for_method (method)`, which dereferences the
method and its class four times before any guard can run.

What exists now, and what each thing is worth:

* **`mono_wasm_jit_callee_perm_unjittable` no longer dereferences at all** (R282). It needed one bit, so
  the bit is RECORDED where it becomes true — the five sites that write `wasm_jit_slot = -1` — into an
  append-only pointer set and answered by lookup. **Hashing a pointer never dereferences it**, which is
  the property a range check can never have. Read `[wasm-jit permset]`: `adds` is the liveness check (0
  means the set is never populated and the rest of the line is meaningless), `full` must stay 0, `stale`
  counts the one path that can falsify an entry.
* **`mono_wasm_jit_method_usable`** (`MONO_WASM_JIT_BADMETH`, default 1) converts the DETECTABLE subset
  into a counted refusal at four sites. It does **not** make retention safe — a freed-but-in-range
  pointer still passes — and `WJC_BADMETH_SEEN` is its liveness check.
* **Still open:** `WjRegEntry.body_method`/`.logical_method`, `WjProfSite.id_targets[]`, `WjDepSet.method[]`,
  `wj_sync_inner_canon` and the lazy pool's `WjLazySlot.method` all still retain raw pointers (`wj_block_tab` and
  `wj_waiter_key` went with the islands, R367). `canon_subst` measures `wj_sync_inner_canon` at ~300/run; bind and
  the interpreter leg test `WjLazySlot.method` with `mono_wasm_jit_method_known_dead` first (`[wasm-jit lazy]
  dead=` must read 0). The root fix is to stop retaining, or to purge on `mono_mem_manager_free`; neither is done.
* **An instrumentation gap to fix before quoting those counters:** `WJC_BADMETH_SEEN` is an AGGREGATE
  over all sites, so `registry=0 profile=0` cannot distinguish "ran and caught nothing" from "never ran".
  Those two counters had no caller at all for months and read exactly the same then. A per-site
  denominator is needed before either zero is evidence.

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
it is backend OUTPUT quality — shadow GC frame, dispatch machinery, the `s.p` chain, no
cross-module inlining. **We ARE the middle end**: we emit wasm as the LLVM path does and V8 does register
allocation, so mono's native-minijit row does not bound us.

**The tier does not run at -O0, and the CONSUMER no longer sets this.** The default string is baked into the
runtime at `mini.c:3258-3259` — `"inline,consprop,copyprop,deadce,branch,cfold,loop,alias-analysis,ssa,abcrem"`
— parsed by `wasm_jit_extra_opt()` (`mini.c:3241-3306`) and applied at `mini.c:3514` as a REPLACEMENT of
`cfg->opt`, not a filter. `index.ts`'s `wasm_jit` object is now nearly empty ("the runtime's defaults are the
product's defaults"); grepping ikvmcraft for `consprop` finds nothing. Two things in that string are inert:
**`loop` never runs LICM** — `mono_ssa_loop_invariant_code_motion`'s single call site (`mini.c:4095`) is gated
on `COMPILE_LLVM`, forced FALSE for us — and `alias-analysis` is locals-only (`alias-analysis.c:3`), so it
does nothing for the heap accesses where our load gap against TeaVM lives. The only real mono flag we could
enable and do not is `AGGRESSIVE_INLINING`; `PEEPHOLE`/`SCHED`/`LEAF`/`SSAPRE` are unreachable or have zero
implementation behind them.

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

**MEASURE ON THE SERVER TICK, NOT THE CLIENT FRAME (R244).** Native's client frame is only **2.80 of its
5.17 M/f** of actual Java — the rest is Mesa 0.99, OpenAL 0.48, libm/libc 0.38 — so every "x native" ratio
computed against 5.17 understates the managed gap. Our client thread is likewise diluted by ~35 M instr/frame
of GL stack, chromium and V8 that is out of scope. **Native's server tick is 90.9% real Java** (9.642 of
10.61 M/tick, re-derived R269) and ours carries no GL, no chromium and no `Unsafe` traffic, so it
decomposes cleanly. The rest of native's tick is JVM stubs (itable/vtable/i2c) 0.389, other 0.340,
libjvm/native libs 0.239 -- **the 94% this said before counted the JVM's own stubs and native libs as
Java**, which inflates the codegen term and deflates the machinery one.
**The shipped-build figure has MOVED and the old one is stale.** 2026-09-12's `ship-baseline` read
**172.0 M/tick = 16.2x** (1.91x machinery x 8.45x codegen, real Java 76.35 against native's 9.642; stated
as 2.3x x 7.6x before R269 re-derived the denominator). The path to it, same instrument and workload:
**32.8x** (with the R244 admission regression) -> **18.2x** (fixed) -> **16.5x** (`LAZY_SIG`) ->
**16.2x** (`RELINK_JITTED`).

**Since then IKVM's IN-PLACE RELINK landed and is the largest measured win of the line:**

| build | M instr/tick | note |
|---|---|---|
| relink OFF (`IKVM_LAZY_RECOMPILE=0`) | 188.4 | R277, 2 captures |
| **relink ON (shipped)** | **165.4** | **-12.2%**, non-overlapping both rows, = 15.6x native |
| + the four defect fixes | 161.8 | R279 plateau |
| R280h | 155.2 / 155.5 | both `IKVM_LAZY_CTORS` arms |
| control before the flip, 2026-09-23 | 149.1 / 158.1 | R299 D arms |
| the R299 levers ON by default | 139.7 / 142.1 | R299 Z arms, **-8.3%**, same binary, 0 skipped ticks |
| **current: + R311 method identity & profile delivery** | **115.9 / 115.0** | R311, **-8.9%** raw G vs its OFF arms (128.0 / 125.3), same binary, equal ticks |

The five levers (`GUARDED_INLINE`, `INLINE_LEAF=64`, `LAZY_COLD=1`, `VCALL_MEMO`, `FRAME_ZERO=0`) cut **our
emitted code** (the real-Java bucket) 77.5 -> 64.6 M/tick, reproducing R292's figure for that bucket. The total's
run-to-run spread comes from the JIT-helper and lazy-link buckets (+-3-5 M/tick), so quote the total as -6 to -8%.
They were blocked for months by the recurring OOB, which was worker reuse (R293), not inlining.

The mechanism is far more robust than the timing total: the **IKVM lazy-link dispatch pool HALVED**
(52.38 -> 25.24 M/tick, -51.8%) and the server **completed 2,405 ticks against 2,050 in the same window
(+17.3%)** — the unconfounded comparison when tick counts differ. Four defect fixes took installs from
2,250 with 1,978 failures to **4,500 with 0** (R276/R278). `IKVM_LAZY_INPLACE=1` and
`IKVM_LAZY_RECOMPILE` (default on) are shipped; `IKVM_LAZY_RECOMPILE=0` is the one-knob A/B that takes
the whole feature off the path.

**Quote the number with its date and instrument.** These are cross-binary readings taken on a box whose
same-arm spread on the lazy-link bucket is ~20%, so the ordering is solid and the individual figures are
not to three digits.

**Skipped ticks are the outcome metric to quote alongside it, and they moved further than the ratio did:
519-527 per 120 s window -> 40.** M instr/tick divides by EXECUTED ticks, so it partly hides a server
that is failing to keep up; the skip count does not. Where two arms complete the same tick count, compare
raw G instructions instead -- that is the only fully unconfounded row.

**Re-derive this split on the build you are about to change** -- and pick the server tid EXPLICITLY.
`nativemc/buckets.py` falls back to the `realize_glenv` marker when `--tid` is omitted, which selects the
CLIENT thread; dividing client instructions by TICKS silently reports a plausible number for the wrong
thread. `nativemc/servertid.py` resolves it by which symbols the thread actually ran (both busy threads
are named `DedicatedWorker`, so ranking by CPU does not identify them). Use
`nativemc/buckets.py --ticks` and `nativemc/join.py --thread "Server thread"`. Two gotchas: the server tick is
**not actually 20 Hz for us** (376 of 2,406 ticks skipped in a window; native skips zero), and `join.py` must
select the ONE tid perf counted — three threads are named `Render thread`.

**THE PER-METHOD GAP IS NOT UNIFORM — it spans 2x to 40x** (R244, server-tick join, 390 matched, 7.2x on
matched mass): `TickEntryQueue.setTickAtIndex` **2x**, `selectTicks` 10x, `class_3215.method_12121` 26x,
`class_2945.method_12783` 39x. Near-parity methods EXIST, so "the managed model costs 7x" is a mean over a
wide distribution, not a tax, and the 30-40x tail has a findable cause. Bound every such row: HotSpot inlines,
so `ABSENT` means "the cost is not there", not "it never ran", and rows past ~20 rest on single-digit samples.

**Composition of our client thread** (R240, 2026-09-10 build): IKVM machinery **32.9%**, our JIT helpers +
mono runtime + GL + corlib **30.8%**, real Java 26.8%, chromium/libc 9.4%. On methods present in BOTH profiles
we are **13.5x — not the 15.7x R240 published**, which was inflated ~20% by the pooled-Render-thread bug
(R244). Note this census is task-clock and its build already carried R244's admission regression.

**Four pools nothing had named, client thread, 2026-09-07** (R244): `sun.misc.Unsafe` emulation **11.7 M/f**
(no fast path at all — every `putInt` stackallocs, binary-searches the field table, then reflectively
read-modify-writes the whole field, inside an exception filter); the GL stack **25.0 M/f vs native's 1.12,
~22x**, of which 8.64 is the per-GL-call boundary inside the chromium binary (WebGL validation + GPU command
buffer 41.6%, typed-array views over wasm memory 22.8%, V8<->Blink dispatch 13.5%); monitors **5.0 M/f**; real
`java.lang.invoke` **3.8 M/f = 2.29%**, counting the whole adapter chain (`MethodHandleUtil/DynamicMethodBuilder`,
`DMH.`/`BMH.`, `PairwiseConvert`) — R225's 0.168% counted only symbols literally named `java.lang.invoke.*`
and is a strict subset, not a contradiction.

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
interpreter boundary, no IC dispatch, and 4.2 declared locals per function against our 28.1. **Do NOT cite
that last pair as codegen quality (R244):** ~22 of our 28 are a FIXED scaffolding block
(`mini-wasm.c:8290-8378` — dispatch/IC/delegate/EH/addr slots) declared in every function including a
one-line setter, whose measured floor is 23 locals. The vreg-derived count is ~6, i.e. ~1.4x TeaVM, and
locals are a WIRE-SIZE question only. There is also no register allocator in this path at all —
`mono_local_regalloc`/`mono_linear_scan`/`mono_arch_allocate_vars` are all excluded for `COMPILE_WASM`, so
V8 allocates and the 39.7% `mov` share answers to call density and to the per-def GC shadow-frame mirror
(`mini-wasm.c:4716-4756`: 4 wasm ops + a store on every def of every reference), not to our local assignment.
**Devirt and inlining are MUTUALLY REDUNDANT** — TeaVM's emitted `call_indirect` count is 35 baseline, 33 with
inlining off, 176 with devirt off, **791 with both off** — so never measure one alone and call it "the value
of devirtualization". Three passes mono lacks entirely: `Devirtualization` (no CHA), `ScalarReplacement`/
escape analysis, and `GlobalValueNumbering` (we have only local `lcse`).

**There is no wasm, V8 or memory-model barrier.** TeaVM reaches 1.23x with linear memory on this same V8, and
being in a browser costs a good wasm compiler ~1.02x. Both of those explanations are closed.

## Where the time actually goes (in-game plateau, ~25 fps / 40 ms)

**The server tick's cycle gap IS its instruction gap (R458, stall census on both JVMs, four GP events per group, no
multiplexing).** 68.7 M cycles / 52.7 M instructions per tick against JDK 8's 13.0 / 10.4: 5.30x and 5.04x, IPC 0.767 vs
0.806. **Per instruction our BACK END stalls like HotSpot's** (execution stalls 0.855 vs 0.882, L3-pending 0.508 vs
0.653 -- HotSpot's tick is memory-bound too); **our FRONT END is 1.6-2.5x worse per instruction** (icache data/tag stalls,
iTLB walks, and BACLEARS 21x per tick -- BTB misses; machine clears and SMC are ~0). The front-end tax follows CODE
VOLUME, not placement: C1's byte cut took 6-10% off it (R459), packing the whole hot set into 28 modules took nothing
(R463). And the same body loads cost 2.5x HotSpot's cycles each (R461: 30.5 k L3 misses per tick in field loads alone,
more than HotSpot's whole tick), with allocation only <= 1.8x HotSpot's bytes (R465). So ~3x needs ~40% fewer executed
instructions, or the front-end tax and the load latency attacked directly -- not cheaper instructions.

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

**The call infrastructure, EXECUTED, on the server thread (R290, PEBS, 2026-09-22, 152.1 M instr/tick): 35.7%
of the thread** -- our prologue band 10.6% (the `s.p` read chain, write-back, frame zeroing, first pin),
dynamic-index `call_indirect` 9.4%, constant-index `call_indirect` 5.9%, V8 frame pro/epilogue 5.2%, near-call
spills ~2.4%, import calls 1.9% -- plus 12.2% in the JIT tier's own helpers. `scratchpad/wj/callinfra.py` is the
instrument (`--band-split` splits the prologue, `--edges` dumps callee/caller/form). Split of the prologue band
on the all-levers build (R294): **the `s.p` imported-mutable-global chain alone is 8.3 M/tick = 6.1%**, frame
zeroing 2.95 (plus `memory.fill` for frames over 64 B, outside the band). **Two corrections this made:** R269
sized co-location's pool ~3x too small, and R240 add.5's "prologue: nothing to take" was a static count.

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

**Most IC MISSES were not polymorphism but inheritance (R310), and are now absorbed (R311).** Classified against
the site's front-runner, **~68% of in-game IC misses were another receiver class resolving to the SAME method**
(Entity subclasses calling an inherited Entity method), ~21% a genuinely different method, ~5.5% dropped by the
profile cap. The vtable-keyed 1-way PIC missed on every class change. R311's method-identity check
(`vt->klass->vtable[slot]` against the cached method, class-virtual non-generic sites only) took published
misses 119-128M -> 31-32M per window and the server tick -8.9%. "poly" and "alt-receiver" above are both keyed
on the receiver VTABLE and so overstate true polymorphism; what remains is ~27M different-method misses.

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
| Liftoff / V8 tiering | 59.3% of our self time is `turbofan`, 0.41% `liftoff`; **98.41% of our tier's executed time is already TurboFan-tiered**, so tier-up is not what blocks inlining — callee LOCALITY is. The plateau residue is ~1.8% of server cycles (425 warm functions' Liftoff bodies plus `WasmLiftoffFrameSetup`, which every Liftoff entry calls under `wasm_inlining`), and the `compilationPriority` section that could release it is an EXPERIMENTAL V8 feature the product's Chrome skips (R370) |
| local renaming, coalescing, `local.tee`, copy-chain elimination as *runtime* levers | V8 source proves local ops are free; these are wire-size only |
| helper-import cap | not binding: max 30 declared in any hot module, median 3, cap 192 |
| `IKVM_LAZY_BODIES=0` | 11.2% WORSE; the shipped setting is already optimal |
| `MONO_WASM_JIT_RELINK_JITTED=0` (refusing an IKVM body swap once the wasm JIT compiled generation 1) | **SHIPS 1**, but **the shape has changed and the measurement has not been redone** (R268). R252 measured **-5.2%** server tick (430.7 -> 408.3 G instructions over an identical 2,406 ticks; IKVM `late` 619 -> 37, installs 3,250 -> 3,750, no cost bucket rose) on a design where gen 1 stayed live and gen 2 took a **FRESH e/f pair**. That design is gone: generation 2 now keeps the descriptor and the pair and is republished, because a fresh pair means already-co-located and already-devirted callers never reach generation 2, and because a detour requires same-slot replacement. `relinkRefreshed` is 574/run with only **4 in-game**, so R252's number says nothing about whether the republication machinery pays for itself in the plateau. The INTERPRETER half of the refusal stays unconditional -- tier-up across two different ILs asserts in `lookup_patchpoint_data` |
| carrying a gen-1 PERMANENT bail into gen 2 as a lever | **MEASURES ZERO** (R252). `WJC_RELINK_BAIL_CLEARED` is 0 over a full run: IKVM's relink hook IS the method's first execution, where a method is untried or already live, so the `-1` state is never reached there. The arm is kept and the zero recorded at the site -- without the counter it would read as a shipped fix forever |
| `IKVM_LAZY_SIG=0` (refusing lazy-body candidates whose signature carries an unloadable) | **NOW SHIPS 1** (R251). Admitting them is worth **-9.5% server tick, -52% skipped ticks, +14.4% ticks completed**, both orders, n=2/arm, non-overlapping. Dispatch pool 47.9 -> 36.4 M/tick, type-check pool 11.6 -> 7.3 (generation 2 emits a plain checkcast where generation 1 needed the dynamic chain). **The 2026-09-10 arm that measured this a WASH is superseded, not contradicted**: it ran on the build carrying the R244 admission regression, so its +3.993 admission / +3.321 interpreter loss was mostly the regression. Residual churn on the fixed build is +0.8 M/tick, 8:1 against the win |
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
| `MONO_WASM_JIT_REEMIT` (re-emission with a matured profile) | **The OPTIONAL, profile-driven arm still ships 0. The broker it uses is no longer optional**: an IKVM body swap enqueues a MANDATORY replacement through the same queue regardless of this knob (`wasm_jit_reemit_required`), so "REEMIT=0" no longer means "no re-emission ran". Read `WJC_REEMIT_REQUIRED_*` before concluding anything about a run. **Measured on the shipped config
with `knob=0`: `queued=889`, compiled 591, `republished=591` (R269)** -- so an A/B on this knob compares
"mandatory only" against "mandatory + profile-driven", never "off" against "on". For the optional arm: **mechanism sound, population wrong, and throughput-bound.** Re-emitted bodies reach 54.7% devirt coverage against 28.1% run-wide, but are 2.25% of sites, so run-wide coverage moves ~+0.6 pts. What binds, in order: **drain reach** (56,667 queued vs ~5,800 gated) > **compile-lock contention** (`busy=5,134` vs `done=189`) > co-location conflict (`batched=496`). Note the trigger must not key off `wasm_jit_invoke_in`, which is incremented only under `mono_wasm_jit_stats` — any arm run without `--stats` measures nothing. `MONO_WASM_JIT_COLOCATE_DEPS=0` + re-emission WEDGED (2 of 2) — **but that arm can no longer be run: there is no `MONO_WASM_JIT_COLOCATE_DEPS` getenv and no such variable anywhere in the tree (R245). Co-location is unconditional.** Ships 0 |
| module batching **as it was originally built** | measured negative four times (-26.6%, -35.7%, -13.0%, regression) for two mechanical reasons, and BOTH are now gone: producing a batched body cost a full `mini_method_compile` per member (bodies are now relocatable and re-framing is a memcpy), and the planner planned a plateau ONCE, on a quiescence this workload never reaches. Do not re-run the OLD arms or re-tune `batch_max`/`batch_bytes` (measured non-binding). **Co-location is UNCONDITIONAL** — R245 verified there is no `MONO_WASM_JIT_COLOCATE_DEPS` getenv and no variable behind it, so the `=1`/`=0` arms this file used to describe are not performable. Same for `MONO_WASM_JIT_SCC_COLOCATE`, which several comments still offer as an in-binary A/B. `COLOCATE_MERGE` and `COLOCATE_MAX` are real |
| **raising co-location's STATIC capture** | **R258's CLOSURE IS RETRACTED (R269) -- the measurement stands, the conclusion does not.** R258 read the tick at **-1.2% against a control arm whose own spread was 5.0%**, i.e. it could not have resolved its own lever: sized independently, converting the ~27.4% of dispatch that is a devirt arm still going indirect is worth **~2.5-3.7 M/tick = 1.5-2.2% of the thread**, under half that spread. CLAUDE.md's own rule -- under ~12%, measure the MECHANISM, not the outcome -- was not applied to R258 itself. **And co-location is NOT independent of re-emission**: `WASM_RELOC_CALL` is only emitted when the callee already has an f-slot, so only re-emission creates the holes co-location fills (REEMIT=0 -> 1 measured **+1,077 devirt arms, +1,258 absolute module-local calls**). Never A/B the two separately again. The original R258 text follows, still true as measurement: **CLOSED ON OUTCOME (R258)** `MONO_WASM_JIT_COLOCATE_MERGE=1` moved captured call edges **32.4% -> 44.2%** and `callform local` **+126%** -- and the server tick did not move: OFF mean 167.0 (spread 8.4 = 5.0%), ON mean 165.1, a difference of **-1.2%, one quarter of the control arm's own spread**. The reason is that execution-weighted co-residency only went **4.47% -> 6.53%**, and 2.1 points against a ~5% IC-miss share of dispatch is ~0.1% of dispatch. Ships 0. **Static capture is not the binding constraint; execution weight is.** If this is revisited the target is a profile-weighted global partitioner -- `partreach.py` puts the cap-16 ceiling at 96.4% and a global agglomerative partition at 51.5% of the residual, and an offline seed-and-grow reaches 79% of call edges internal at 16 members, 85.5% execution-weighted |
| **co-location as a route to "most dispatch is a direct call"** | CLOSED ON STRUCTURE (R195). The reachable set is only the devirt predicted arms — both `WASM_RELOC_CALL` sites are gated on the callee already having an f-slot, so a callee un-JITted at emit time has NO hole and only RE-EMISSION can convert it. Arm-local plateaus ~30% because **co-location is a PARTITION and the arm graph is not partitionable**: if two callers hold arms on the same target, only one can have it co-resident. Proof it is the partition and not tuning: surviving refusals are **100% caps, 0 rules**, and doubling `COLOCATE_MAX` bought **+1.5 points**. `max=64`+`bytes=131072` also CRASHES (undiagnosed); `max=32` is clean. The mechanism that bypasses a partition is DUPLICATION — shadow copies |
| **hot-set consolidation (`MONO_WASM_JIT_B4_HOT`, plan P2's ceiling)** | **CLOSED (R463).** The 600 hottest live bodies (98.2% of the plateau's heat, tier 1 and tier 2) framed into 28 modules ~130 s after join, 0 refused: cycles -0.2%, instructions +1.1%, icache/iTLB/BACLEARS per instruction flat (pooled, 13 arms). Neither the partition (R195/R258) nor the PLACEMENT binds: the front-end tax is code volume. The knob stays as the instrument |
| **SCC co-location as a source of reach** | 7 modules / 24 members per boot against a ~24,000-method tier. Cycles are rare on this workload. It was kept as a correctness mechanism until R366 deleted it with the islands: a lazy pool slot lets cycle members bake each other's f-slot before either exists, so nothing needs ordering. Never a performance lever |
| shadow copies — cap sweeps (`WJ_SHADOW_MAX`, `MONO_WASM_JIT_SHADOW_BYTES`) | **CLOSED after ranking, R201/R202.** SELECTION ORDER was the real variable: ranking candidates by **sites/bytes descending** gives 63.7% arm-local with 2.8% FEWER bodies than encounter order at identical caps. After that, raising the caps drove `ShadowCap` to 0 and conversion did **not** move — with ranked selection the candidate SUPPLY is exhausted. **A cap closed as "non-binding" is closed only for the population it was measured on** — an earlier sweep saw 169 shadows where the current stack has 23,202, and its closure had to be retracted. Ships `MONO_WASM_JIT_SHADOW=0`; plateau is ~63% arm-local ≈ ~54% of executed dispatch direct, at **+50% bodies**, and the timing cost of that is still unpriced |
| `shadowNojit` as evidence about the AOT wall | the counter is a TAUTOLOGY: shadow collection walks `WASM_RELOC_CALL`, which only ever names an already-JITted callee, so `nojit` cannot fire. AOT callees emit `WASM_RELOC_AOT` and are never candidates |
| "55% of real call sites target the main module" as a co-location ceiling | RETRACTED (R180) — that is a STATIC SITE COUNT. From 2,598,503 caller->callee edge instances, **93.66% of calls out of our tier land in our own tier** and only 6.24% in AOT code. The AOT wall is not what limits co-location reach |
| **"process-wide modules cannot reach `__thread` state"** | RETRACTED — wrong, and it wrongly closed the monitor inline-CAS lever. See the imported-globals invariant above |
| `IKVM_LAZY_CTORS` (relinking constructors) | **Ships 0, and do not re-run the A/B.** The mechanism is correct and clean -- +208 generation-2 bodies (4,293 -> 4,501), 0 failures, 0 faults over 4 healthy runs, `bad_offsets=0` -- but the server tick is a WASH (155.2 -> 155.5). The lazy-link bucket moved -14.4% in the predicted direction and at the predicted magnitude, and that is **not resolvable on this instrument**: the same-arm spread on that bucket is ~20% (R277's `a0`/`b0` were the same arm and read 27.37 vs 22.43). A ~1.8%-of-thread lever against a ~20% instrument spread is the closure, not the sample size. The durable result of that round is the RUNTIME fix it forced out (`clause_is_dead`) |
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

**`csyn.sh` COMPILES BUT DOES NOT LINK, and that gap has now cost two builds.** `mini-wasm.c`,
`interp.c`, `transform.c` and `tiering.c` are all linked into **`mono-aot-cross`** as well as the runtime,
while most of the JIT lives inside `mini-wasm.c`'s `#ifdef HOST_BROWSER`. So a call added into that region
from any of those files compiles cleanly in both databases and then fails the real build with
`ld.lld: error: undefined symbol`, ~9 minutes in. **Guard the CALL SITE with `#if HOST_BROWSER`** —
`interp.c` already carries a comment explaining this at its own probe site, three lines above the pattern
that was copied without it.

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

* **`mode=stall` is NOT a fault.** `worldGenMode` classifies world-generation DURATION (the documented
  ~20%-incidence bimodality, ~19 s vs ~90 s); a `stall` run routinely completes with `faults=[]` and
  `verdict=ok`. Read `verdict`, which is the one field that says whether a run counts against the build,
  and which excludes `env` / `never-started` / `killed`. This session conflated the two once.
* **`ok` is not a verdict either** — it is a progress flag set the instant `benchEnd` is seen, unaffected
  by faults, so a run can be `ok:true` while trapping.
* **`inplace ok=` is quantised to 250** (it prints at `n % 250 == 0`), so its last value is a FLOOR and a
  change under 250 installs is invisible in it. Use `gen2 clean=`, which is exact. This nearly produced a
  wrong delta twice.
* **When a counter's total exceeds the sum of its attributed parts, the gap is the finding.** Three of
  eight `InPlaceFailed` increments are bulk `Add(..., n)`, so only 336 of 1,978 failures reached the two
  instrumented catch sites — and the amplifier was in the gap.
* **A bucket named for what it EXCLUDES hides what it contains.** "Class has other bridges" was accurate
  and was misread as "no bridge for this site"; it actually meant "a bridge for exactly this site, under a
  different kind". One extra comparison was the whole answer.
* **"Structural, stop here" needs a higher evidence bar than "keep going"**, because only one of them ends
  the investigation. That framing closed a fixable 5% as structural for a day.
* **fps is unusable on this box; composition SHARES are not.** Two control runs of an IDENTICAL config:
  `vcall_resolve_fslot` 4.723% / 4.683% (**0.8%**), `InstanceCheck` 2.642 / 2.658 (0.6%) — against **fps 15.10 /
  18.03 (19.4%)**. Client-thread instruction shares are ~20x more reproducible than frame rate. **Quote
  shares; treat any fps delta under ~20% as nothing.**
* **"N consecutive clean runs" is not a gate unless you state its probability (R269).** At the measured
  **4/21 = 19%** fault rate, P(10 consecutive clean) = **12.1%** and P(20) = **1.5%** -- so a 10-run clean
  streak is a routine outcome of an UNCHANGED build. Distinguishing 19% from a halved ~10% at p<0.05 needs
  on the order of 150 runs per arm. Size a stability gate against the base rate, or call it a smoke test.
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
* **Use protocol Q60 for Minecraft censuses** (`--warm-ms 160000 --bench-ms 60000 --no-walk --no-save-quit`;
  `hwrun.sh`'s defaults since R418). `--warm-ms` DEFAULTS TO 0, so a bare in-game window measures the RAMP.
  Measured on the server tick (R418, `stabslice.py` over R417's eight censuses; times from world join):
  - 0-20 s ~100-114 M instr/tick (catch-up), 60 s ~67, settled ~61-64 from ~100 s, then a ~-3% drift.
  - **An A/B needs the window to start ~200 s after join.** A 160-220 s window lost the GC probe's +4.6%; 100-160 s
    showed nothing.
  - **A 60 s window at 200-260 s measures what the old 120 s one did** (same effect, separated in instructions AND
    cycles; base spread 4.0% / 7.5% vs 3.3% / 7.0%). 30 s windows hold for instructions but cycles spread 10.6%.
  - **The 6,000-tick autosave lands ~300 s after join** (+17 M instr/tick for 10 s). It sat INSIDE the old
    `--warm-ms 180000 --bench-ms 120000` window (~220-340 s) and added ~2% to every reading.
  - The harness spends ~38 s settling before the hold starts, so `--warm-ms 160000` puts the window at ~198-258 s.
  - Re-run `stabslice.py <rundirs> --trace` before trusting Q60 on a new world or mod set.
  - An arm is ~7 min (was ~9.5): boot+menu+world ~135 s is fixed.
  - **Between-run world variance is ~+-3.5% per run** (each run holds its own level for the whole window; R418
    addendum: a b g g b validation could not separate the nursery effect). n=2 per arm resolves only >~6-7%; use
    n=3 per arm (A B B A B A, ~44 min) for 3-6% effects, and the mechanism alone below that.
  - hwrun queues each arm's census to run during the NEXT arm's boot. **Call `hwrun.sh <batchdir> --drain` after
    a batch's last arm**, before reading any `<tag>.census.txt`.
* **The ramp is not JIT warmup — it is work we are too slow to clear.** Q1 vs Q4 per frame: mono JIT compile
  ~0 -> ~0, V8 compile 2.0 -> 0.4. Both compilers are DONE before the window opens. What drains is
  MethodHandle/invokedynamic linking (103.5 -> 47.2 M/frame), chunk/world meshing (67.2 -> 27.7) and IC miss
  (33.4 -> 21.1). Native pays the identical transient and clears it in seconds. **Ramp length is a symptom of
  the 4-5x gap, not a separate warmup problem.**
* **Read `verdict` first, then the three `[wasm-jit ...]` bound lines.** `[wasm-jit bounds]`
  (`install_budget_out`, `rv_retry dropped/try_max`, `worker_slots full`), `[wasm-jit spin]`
  (`pub_reraise_max`, the two lock spin high-waters) and `[wasm-jit permset]` (`adds` is the liveness
  check; `full` must be 0). These are bumped UNGATED, so they are readable with `MONO_WASM_JIT_STATS=0`
  at no measurement cost — and `mono_wasm_jit_liveness(14..17)` exposes the four that matter without
  `--dumps` at all, which is the only way to read a wedged run whose main thread never returns from
  `Runtime.evaluate`.
* **Assert the tier is alive before reading any timing**: `registered` unchanged (~1,845-1,863 during
  world.generate on the current config), `tableExhausted 0`, `faults []`. A wrong functype on an import fails
  *instantiation*, not the call, so the method silently falls back to the interpreter — it looks like a
  performance result. A whole session was once run against a dead JIT tier.
* **Measure the mechanism before the outcome.** A tier dump or a `hotinsn.py` run takes minutes and says
  whether a change did what it was supposed to; an fps A/B takes hours and says only whether the number moved.
* **Measure on mains power.** On battery the platform drops to a power-saving profile: R454's base mapping run fell in a
  dock unplug and came back with a slow boot, a worker stall and an in-game window whose frame counter never moved
  (`ingameVoid`). `lib/preflight.mjs` now refuses on battery and warns off the `performance` power profile.
* **Pin every arm of a comparison the same way (`MC_TASKSET`), and never compare across batches that differ in it.**
  Chromium counts CPUs through `sched_getaffinity` (`base/system/sys_info_posix.cc:157-159`) and sizes its thread pool
  at max(3, CPUs - 1) (`base/task/thread_pool/thread_pool_instance.cc:94`), so the pin changes the renderer's thread
  count (55-57 pinned to 8 CPUs, 63-64 unpinned), its stack reservations and its concurrent compile zones -- i.e. peak
  VmData. R449 compared unpinned arms against a pinned base and first blamed desktop load. Run the base in the SAME batch.
* **Prefer a within-binary knob A/B to a cross-binary comparison.** A cross-binary reading at this spread
  cannot support a 4% claim however tidy the mechanism sounds.
* **Count ABSOLUTE quantities, not shares of a moving denominator.** Every mechanism in the direct-call path
  changes the NUMBER of arms, so "arm-local %" moves with its own denominator and reads as an effect. A
  matched pair on `colocate_deps` (an arm that **can no longer be run** — R245: the knob does not exist):
  OFF looked better at 74.6% vs 58.3% arm-local, but ON produced **13,697
  local arms against 12,041** — 13.7% MORE direct calls. The same trap retired three separate conclusions in
  one session. A larger tier is also usually MORE METHODS COMPILED, not bloat — check the module count before
  reading MB as waste. Tier size varies run to run (28,110 vs 30,893 modules), so do not rank two configs on
  <2 points from single runs.
* **`ic_hit` / `ic_miss` ARE NOT DISJOINT -- do not quote an IC miss rate from them (R269).** Shipped
  config: `ic_hit=106,557,211 ic_miss=212,832,944 vfast_had=213,364,188`. `ic_miss` tracks `vfast_had` to
  0.25% and implies a 66.6% miss rate against the 9.0% measured in-game: two routes are being counted at
  one site.
* **Do not compute a share until every route has a counter, and assert the parts are DISJOINT as well as
  summing to the whole.** This has cost five rounds. Once the IC was called 79% of dispatch because the pool
  had no term for the devirt arm; once delegates were put at 43% because two counters were bumped
  unconditionally at the same two sites and double-counted (the real answer is ~20%). An uncounted route does
  not show up as a gap — **it shows up as everything else looking bigger.**
* **A counter that names an action must be bumped where the action HAPPENS, not where it is decided.** One
  read `DelegateDevirtArm 750` with `FastDelegateDevirt 0` for a whole run: the resolution block ran
  unconditionally while the emitting code sat inside a default-off knob's branch. If the decision and the
  action live in different functions or `if` arms, count both and assert they are equal.
* **A diagnostic behind a default-off knob is not evidence of absence**, and "every error counter is zero" is
  a statement about the counters you have, not about the run.
* **A diagnostic that rides in the module can change what the module IS (R460).** `MONO_WASM_JIT_ORIGIN`'s `wj.origin`
  section was ~half of a giant's module and the tier-2 cap (T2_MAX_BODY) measured the whole module, so every ORIGIN run
  -- capture gates AND ledger mapping runs -- refused tier-2 bodies a normal run admits (LivingEntity.travel, 76 KB of
  function, read "down"). Fixed: the cap subtracts `wasm_last_origin_section_len`. Mapping runs before the fix have a
  more tier-1-heavy population than the runs they explain.
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

### The world-load hang is a SPIN, not a block

Caught live (R275): two threads at **~92% CPU**, both `state=R` with `wchan=0`, concentrated on five
addresses spanning ~22 bytes of `[anon:v8]`; a third worker in `__futex_wait`. `Debugger.pause` could not
interrupt the page or any of 8 workers within 7 s. App-side it is total quiescence — `JIT_ADMIT
registered` frozen, OPFS counts frozen, `faults=[]`, then nothing for 20 minutes.

**Every earlier description of it as "stopped" or "blocked" was wrong about the mechanism**, and the two
need opposite next instruments, so the driver now classifies them: `verdict=spin` (no app progress AND a
thread running with `wchan=0`) versus `verdict=wedge` (no app progress, all threads asleep). On a spin it
captures 12 s of perf automatically. That capture is only readable because `worldwait` now passes
`--perf-basic-prof` by default — R275's own capture had no symbol map and its five addresses stayed
nameless, which is the whole reason the round ended undiagnosed.

**ROOT CAUSE FOUND (R454x): it is `mallinfo()` looping forever on a corrupt heap chunk.** ikvmcraft's frontend polls
`_ikvm_native_used_mb()` every 5 s on the MAIN thread (`frontend/src/dotnet/index.ts`, a UI memory stat), which calls
`mallinfo()` (`loader/Emscripten.c:69`). A spin capture (`j2d/r454b/spin-b-1`, symbolised with `ledger/perfread.py` +
`jitindex`) has 59,422 of 59,436 main-thread cycle samples in `dlmallinfo` for a full minute, ~38 billion loads with
ONE L1 miss, touching exactly two data addresses: dlmalloc's walk advances by each chunk's size field, so a header
whose size reads 0 is walked forever -- holding dlmalloc's global lock, which is why every allocating worker then sits in
`__futex_wait` and the app goes quiet. So the hang is a DETECTOR: the real bug is whatever zeroes a malloc chunk header.
Stopping the poll removes the hang; finding the corruption is separate. The paragraph below predates this.

**With the poll gone the corruption surfaces as MODULE BYTES that change after validation (R462, R464).** A group module
("memory index exceeds number of declared memories") and a lazy bank ("invalid value type 0xb9") failed V8 validation
on bytes another worker had validated -- new fault texts, ~2 of 45 runs on 2026-10-05. `MONO_WASM_JIT_BYTES_CHECK=1`
(block hashes at first instantiation, re-checked at every later one, `WASM_JIT_BYTES_CHANGED` + the 16 bytes before the
buffer; `[wasm-jit bytes] changed` MUST be 0) is the instrument. Its first two versions reported RECYCLED addresses --
never-published buffers (refused registrations, invalid modules, failed groups, batched re-emits' standalone bytes) are
freed after their validating instantiation, and the next buffer at that address read as "changed"; R464's "live lazy
bank handed out twice" was exactly that shape and is NOT established. The third version forgets those six frees and
re-records an address whose length or producer kind changed; only a report on a matching kind and length is evidence.

**Two mechanisms have been fixed that could produce this; NEITHER is confirmed as the cause.**
The rendezvous carry list could not drop a permanent refusal, which re-raises `WJ_ACT_PUB` forever and
puts a full drain on every loop back-edge; and both CAS loops were unyielding busy spins (see the
mono-on-wasm facts). On a healthy run all their counters read ~0, so **the arms are unexercised, not
validated**. The reading that settles it has to come from a HANGING run. If one shows `pub_reraise_max`,
`rv_retry try_max` and both `spin_max` small, neither is the cause — say so and read the capture rather
than defending them. Four explanations for the earlier failures were each proposed before the evidence
was gathered and all four were wrong.

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
| `nativemc/natrun.sh`, `natstat.py` | **The native reference.** Drives native OpenJDK Minecraft on the MATCHED Prism instance (same save, mods sha256-identical) and reports **M instructions/frame** per thread, the same metric as our side. `EXTRA_JAVA=` for ablation arms (`novirt`, `noinl`). Read its README before use: three defects in the old `jvminline.sh` and two Xwayland traps are documented there, and `results/HARNESS-DEFECTS-2026-09-11.md` for the five that were live in these tools until R244 |
| `nativemc/join.py`, `split.py` | Per-method join of an async-profiler collapsed profile against our census, on IKVM-preserved Java names; and the bucket split by whether a method exists on native at all. `--thread "Server thread"` selects the clean instrument. **Both resolve the ONE tid perf counted out of the sibling `perf.txt`** — three threads are named `Render thread` and pooling them deflated every native figure 16.7% (R244) |
| `nativemc/buckets.py` | **Per-BUCKET instructions/frame (or per tick) for one of our threads, against native's WHOLE frame.** The framing that stops a 7%-of-thread pool reading as small. Refuses to run on a task-clock capture. `--show <bucket>` prints a bucket's symbols — **use it before quoting the table**; it found six classifier misses on its own first run |
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
| `stabslice.py` | **Where the server tick stabilises and whether a shorter window measures the same thing** (R418): instructions per tick in slices from world join (`--trace`), and per-window means against the full window (`--wins`). The evidence behind protocol Q60; re-run it on a new world/mod set |
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
| `csyn.sh` | One-file syntax check with the real build's command line, both compilation databases. Sub-second. **Does NOT link** — see the build section |
| `lib/verdict.mjs` | **The fault taxonomy and the run verdict, shared by every driver.** 14 classes each with a STABLE KEY, so batches are comparable: a mono assert is `mono-assert:loader.c:1826`, not an anonymous `fault`. `selftest()` runs real signatures taken from archived logs |
| `faultcensus.mjs` | Per-class fault census over the archived corpus (`--since`, `--json`). **`--disjoint` runs the disjointness invariant over all ~3,500 real logs**, which is what catches the classifier bugs a sample-based selftest cannot: the sample test passed while five were live |
| `depcheck.py` | **The admission-contract gate.** `depcheck.py <graph.log> <dump.wat\|dir>` — every f-slot BAKED into a body against every f-slot a descriptor DECLARES. An undeclared one is unadmittable by construction and becomes a `function signature mismatch` on the first worker that reaches it (R285). **Both inputs must come from ONE run** — f-slot numbers are per-run — and it REFUSES below a 25% ownership ratio rather than reporting a cross-run pairing as clean. `wasmtier.mjs --depgraph` emits both from one run |
| `reap.sh` | Dead-pid `/tmp/perf-*.map`, orphaned profiles, v8 isolate logs. Refuses to run if the seed profile is missing. `--apply` to act; dry-run by default |
| `enctest/run.sh` | Host-side encoder gates in seconds. t1/t2 diff against frozen framers, t3 is the serializer round-trip, **t4 is structural** — it checks the assembler at `nexport < nmembers`, which t1/t2 cannot reach; t5 re-framing, t6 branch hints, **t7 frames lazy stub banks and EXECUTES them in node** (tail-call bind, interpreter leg, throw, the i32 pin frame). Run it before believing anything else about the encoder |
| `killdaemons.sh` | Kills leftover Roslyn/MSBuild build servers using preflight's own match, safely. Exit 0 = safe to measure. Run before every measurement |
| `wjcsync.py` | Re-index the JS counter mirror after `WJC_*` entries change in the C enum. **Use this instead of hand-editing the mirror in `lib/mcdrive.mjs`** |
| `lib/preflight.mjs` | Refuses to measure on a box that is unfit (thermal, CPU contention, build daemons). Obey the refusal |
| `lib/` | `mcdrive.mjs` is the Minecraft driver (launch, seed, phases, counters, memory/partition sampling, knob plumbing); `browser.mjs` is chrome/CDP for the jbox2d-era tools; also `provenance.mjs`, `thermal.mjs`, `wasmnames.mjs`, `debuginfo.mjs` |

**That gap is CLOSED**: a `MONO_WASM: forcing abort` (and every other class `verdict.mjs` marks terminal —
`js-oom`, `em-abort`, `renderer-crash`, `wasm-link`) now ends the run immediately instead of sitting out
the 420/480 s budget, so one abort no longer takes the rest of the batch with it. A wasm TRAP deliberately
still does not abort the run: a worker can die and the run go on to produce a complete window.

## Housekeeping

`scratchpad/` is excluded via `.git/info/exclude`, not `.gitignore` — it holds tens of GB of captures and must
never be added. **It is also not recoverable**: a deleted tool is gone. Chrome profile dumps are ~1 GB each and
jitdumps ~4 GB; keep the newest of each kind and delete the rest. Do not commit `*.orig-backup` files.

**`/tmp` is a 7.7 G tmpfs, i.e. RAM, and `--perf-basic-prof` writes a ~240 MB `/tmp/perf-<pid>.map` per
run.** Nothing used to remove them: 100 stale maps = 4.9 GiB left 2.7 G free, and runs then died at PAGE
LOAD — before any app code — so it read as a browser bug or a flaky deploy, and two arms were nearly
blamed on the change under test (R280f). Now handled at three levels, and all three are needed:
`runSession` drops the maps of every process under its profile at teardown (before `game.kill()`, so the pids still
resolve; the renderer alone used to be dropped, and its small siblings' maps leaked 340 files in a day) unless a spin
capture needs them; `scratchpad/wj/reap.sh` sweeps dead-pid maps, orphaned profiles and v8
isolate logs, and **REFUSES TO RUN if `scratchpad/mcsr/seed` is missing**; and `preflight` gates on
ABSOLUTE free bytes in `/tmp`, not a percentage — R280f happened at 64% full, under the old 70% threshold.

**A diagnostic that is free per run is not free per batch.** This harness has produced that failure twice
from two directions (jitdumps at ~4 GB, symbol maps at ~240 MB), and turning on `--perf-basic-prof` by
default took `/tmp` from 62 MB to 741 MB in three runs before the teardown fix. Anything written per run
into a RAM-backed filesystem needs an owner at teardown, not a periodic sweep — a sweep only helps if
someone remembers, and the failure it prevents does not look like a disk problem.

**`--perf-basic-prof` and `--perf-prof` imply `--log`** (`v8/src/flags/flag-definitions.h:4516-4517`), and V8's
default per-isolate log file then drops an `isolate-<addr>-<pid>-v8.log` per isolate (~64 per run) into the
browser's CWD -- 8,103 had piled up in `scratchpad/wj`, more in the repo root. Every launcher now passes
`--no-logfile-per-isolate --logfile=+` (`lib/mcdrive.mjs` `launchGame`, `j2d/run.mjs`, `perfrun.mjs`): `+` is an
already-unlinked `tmpfile()`, and the perf map / jitdump are separate listeners, unaffected. **A new launcher that
passes either perf flag must pass the pair too.**

**Browser profiles:** ~1 GB per run, reflink-copied from the seed. `reapProfile` drops them on a clean
run and **KEEPS them on a failure**, where they hold the OPFS save and the crash-time state and are the
only copy. 340 of them (333 GB apparent) had accumulated in `mc-out` plus 40 in `/var/tmp`.

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
