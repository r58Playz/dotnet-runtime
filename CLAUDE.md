# Working in this fork

This is a fork of `dotnet/runtime` carrying a **runtime wasm JIT for mono** (`src/mono/mono/mini/mini-wasm.c`,
`wasm-encoder.c`). It compiles mono IR to a fresh WebAssembly module per method, at runtime, in the browser. The
workload is Minecraft under IKVM, and the goal is frame rate.

**The performance target is Minecraft 26.2 with no mods, still launched through Fabric Loader (0.19.5)** (2026-11-07):
seed `262n` on our side, stage `sa262n` for native HotSpot, both the same save set to peaceful. Both are the harness
defaults (since 2026-11-08; a run before then without `--seed` measured 1.16.1). Optimize for it and quote it. 26.2
has this one seed: the modded 26.2 packs were retired (`state/retired/2026-11-08`). 1.16.1 + Fabric + Sodium/Lithium
(`--seed mcsr`) remains a correctness and regression workload, but a result on it does not transfer: 26.2's gap to
HotSpot is wider on every counter (**Where we stand**), and its hot code is vanilla's, not Sodium's or Lithium's.

Read this before editing the backend or proposing a performance change. Most of it exists because a previous pass got
it wrong at real cost. Round numbers (`R258`) point into the findings log, which holds the evidence and the history this
file leaves out.

## Where things are

| | |
|---|---|
| the backend | `src/mono/mono/mini/mini-wasm.c` (knobs, registry, admission, GC shadow frame) + its `#include`d `.inc` files: `-emitter` IR -> wasm, `-ir` the relocatable body (`WjBody`) and `wj_assemble`, `-batching` co-location, `-publish` asynchronous republication, `-lazy` lazy f-slot pools, `-vslow`, `-diagnostics`; and `wasm-encoder.c` |
| JIT <-> interp boundary | `src/mono/mono/mini/interp/interp.c`, `ee.h`, `transform.c` |
| this branch | `wasm-jit-11.0.0` (.NET 11.0.0, emscripten 6.0.3, no JSPI), worktree `~/fna-wasm/dotnet-runtime-jit`. Rounds before 2026-11-10 cite `wasm-jspi-jit-10.0.3` (frozen at de5ea57c395); `scratchpad/notes/REBASE-11.0.0-HASHMAP.txt` maps each of its commits to this branch's (`cherry picked from` lines too) |
| the app + shipped knob set | `~/Documents/ikvm-wasm/ikvmcraft-net11` (branch `net11`), `frontend/src/dotnet/index.ts`; the 10.0.3 app is `~/Documents/ikvm-wasm/ikvmcraft` |
| IKVM (Java -> CLR) | `~/Documents/ikvm-wasm/ikvm-wasm-build/tools/ikvm/ikvm`, branch `wasm`; its natives from ikvm-wasm-build `net11-sockets` |
| the GL stack (mesa-wink) | `~/Documents/ikvm-wasm/ikvm-wasm-build/tools/native-deps/mesa-wink`, its own repo (branch `wink`, origin `MercuryWorkshop/mesa-wink`; ikvm-wasm-build ignores it) -- see **The GL stack** |
| measurement harness | `wjharness/` (tracked; `wjharness/wj <tool>`, reference in `wjharness/README.md`); its data in `scratchpad/` -- see **The harness** |
| **the findings log -- read before proposing anything** | `scratchpad/notes/MINECRAFT-FINDINGS.md` (untracked) |
| chromium / V8 source | `~/Documents/chromium/src` (v8 at `src/v8`) |
| split DWARF for the shipped wasm | `~/Documents/ikvm-wasm/ikvmcraft-net11/loader/obj/dotnet.native.debug.wasm` |
| the canonical emsdk (6.0.3) | `~/Documents/ikvm-wasm/ikvmcraft-net11/statics/emsdk` -- see **Building** |
| FNA-WASM-Build (runtime build, emsdk patches) | `~/fna-wasm/FNA-WASM-Build-net11`, branch `net11` |

The findings log is long. The alternative to reading the relevant part is re-running an experiment that already has an
answer.

## Comment rules

The backend is ~28% comments and that is fine. A comment that misleads is not.

1. **A knob's default is stated once, at its initialiser.** "DEFAULT OFF" on a line whose initialiser is `1` has shipped
   here, on the hottest path in the emitter.
2. **Never leave an argument for a value the code does not hold.**
3. **A number without a workload is not a result.** jbox2d (homogeneous, low variance: within-binary A/B and cross-VM
   rows) and Minecraft (three phases, ~12% floor) say nothing about each other. Give date, n and spread; if you do not
   know a number's provenance, say so.
4. **When a result is retracted, fix the comment that carried it.**
5. **A failed experiment belongs in the findings log, not the source**, cited by round (`/* ... see R258 */`). Keep in
   code the invariant, the default and the trap that is not visible locally. When you evict a narrative from a comment,
   paste it into the log in the same change (the log is untracked, so nothing else carries it).
6. **If you cite V8 behaviour, cite the file.** "V8 probably..." is not a reason to ship anything.

## Verified V8 facts

Checked against `~/Documents/chromium/src/v8` (15.4.77, re-checked in part on 15.5.35; the installed browser is a third
version). Line numbers drift: if something surprising turns up, re-check the tree rather than this list.

* **V8 never inlines across modules.** Inlining candidates come from a module's own call sites, and an imported function
  scores 0 (`wasm/inlining-tree.h`), so with one method per module **mono's inliner is the only cross-method inliner in
  the pipeline**. V8 does speculatively inline `call_indirect` targets in the SAME instance (`CallIndirectIC`,
  `builtins/wasm.tq`): co-locating a hot target makes its IC call inlinable too. Caps: `wasm_inlining_max_size` 500 wire
  bytes, budget 5000 nodes (`flags/flag-definitions.h`).
* **The three call forms** (read off emitted x86): `call <funcidx>` (module-local) is one `call rel32` and the only
  inlinable form; `call <import>` is ~6 instructions ending in an indirect `call *` (`BuildImportedFunctionTargetAndImplicitArg`,
  `wasm/turboshaft-graph-interface.cc`), so converting calls to imports buys almost nothing; `call_indirect` is ~15
  (bounds, canonical-type check, index arithmetic, `call *`), and V8 does not fold a constant index into a direct call.
* **`local.get` / `local.set` / `local.tee` emit nothing** (`LocalGet` is an SSA rename). Local count and copy chains are
  wire size only; what costs is how many values are live across a call.
* **An imported MUTABLE global costs 4 extra loads per access**, against one hoisted load for an immutable import or a
  module-defined global (`compiler/turboshaft/wasm-lowering-reducer.h`). `s.p` (`__stack_pointer`) is the only one, and
  every framed method reads and writes it.
* **V8 eliminates no linear-memory load and has no wasm LICM** (every load is `.NotLoadEliminable()`; the passes are in
  `compiler/turboshaft/wasm-optimize-phase.cc`). Load CSE and LICM happen in mono's middle end or nowhere (R378).
* **Every `WebAssembly.Module` is its own code space** (~8 KB of VmData at least), **and every worker re-validates it**:
  a NativeModuleCache hit saves compilation, not validation (`wasm/wasm-engine.cc` `SyncCompile`, R345).
* **A function may declare at most 50,000 params plus locals** (`kV8MaxWasmFunctionLocals`); past it the module fails
  with `local count too large`. The emitter refuses such a method first (`WASM_MAX_FUNCTION_LOCALS`, counter `locals_overflow`).
* **Implicit null checks are WasmGC-only** (`null_checks_for_struct_op`).
* **A call inside a `try` of its own frame has no source position in TurboFan code** (R516); the CallSite reports the
  last positioned instruction before it. An EH method's whole body is a `try`, so the stack walk keys its call sites
  (`WasmSites.kd`) instead of trusting positions.
* Classifier trap: a case-insensitive "compile" matches the `-turbofan` suffix of every symbol. Strip
  `-\d+-(turbofan|liftoff)$` first.

## Verified mono, emscripten and IKVM facts

* **A deterministic compile failure must never be `retriable`.** `retriable` parks the method with its hit count reset,
  and it re-accrues the threshold in ~1.5 s: one 1.9 MB `.cctor` compiled 15 times over and took 22.4 s of boot, with
  nothing logged (R289). `CompileError` is deterministic and permanent; `LinkError` is per-worker (imports are) and
  stays retriable.
* **`mono_thread_info_yield()` is a no-op** here, so `while (CAS) yield();` is an unbounded busy spin. The backend's CAS
  loops are test-and-test-and-set, bounded and counted (`[wasm-jit spin]`). A shared spinlock is the first suspect for
  "two threads at 92% on the same few bytes of code".
* **Nothing that blocks properly is safe under a JIT lock**: `mono_thread_info_sleep(ms>0)` leaves a GC-safe region on
  wake, which is a rendezvous-drain site (`mono-threads-coop.c`), so it can enter the drain under a lock the drain wants.
* **An interpreted `calli` to a native pointer needs a build-time trampoline for its signature** (R497).
  `CalliSignatureCollector` adds every unmanaged calli's cookie to the table WasmAppBuilder generates. A miss is
  `g_error("CANNOT HANDLE INTERP ICALL SIG ...")`, which on a worker reads as `verdict=spin` with no fault (verdict class
  `interp-native-sig`). Check calli shapes before taking an assembly off AOT.
* **A partial AOT profile is worse than none unless it is closed over direct calls** (R498): an AOT'd call to an
  out-of-profile method of the same image goes indirect and may enter the interpreter. No profile ships: the AOT
  whitelist is CoreLib, IKVM.Runtime, IKVM.ByteCode and System.Memory (ikvmcraft `loader/IkvmWasm.csproj`).
* **R479's deadlock fix is upstream in emscripten 6** (21a3a81daf), as are emsdk.patch, .2 and .5; FNA-WASM-Build net11
  carries .3 (a backport of upstream #27937's readwrite-unsafe OPFS handles), .4, .6, .8-.11 rebased, and .12.
* **emscripten 6 picks the wasm EH encoding at COMPILE time** (default legacy) and V8 refuses a module that mixes the two
  (`function-body-decoder-impl.h`, "module uses a mix of legacy and new exception handling instructions"). Every native
  object with wasm EH or wasm setjmp is built `-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0`, and the JIT emits
  `try_table` (exnref).
* **Java's networking is the JDK's own libnet/libnio over WasmFS** (FNA-WASM-Build `emsdk.12.patch`: sockets, epoll,
  eventfd, socketpair, a resolver handing out 240.67.x.y and giving the name back on connect) **and ikvmcraft's
  `loader/wasmfs-wisp-transport.js`** (wisp-js on emsdk.12's network pthread). Wisp has no connect acknowledgement: a
  refused connect succeeds and fails on first I/O. Java TLS is SunJSSE (the M6 note in the findings log).
* **Five upstream .NET 11 browser behaviours this branch changes; keep them through any upstream merge** (findings
  notes of 2026-11-10): runtime options parsed on main before the deputy reserves its jiterpreter tables (f0b54a52a56);
  `getNewWorker` hands out the oldest LOADED worker and holds messages to one still loading, which would drop them
  (806af7938c5); workers set `fetch_like` before the native module copies it (806af7938c5); worker print/printErr marked
  `.proxy` so pthread stdout reaches the page (cc4a3d883e8); System.Native's libc `socket`/`connect`/`send` stubs only
  for CoreCLR (7f3c9a18f69: in mono's static link they replaced libc's for every native).
* **There is no JSPI.** The deputy runs the whole game inside the `Run` JSExport and never returns to its event loop;
  GLFW's poll drains the thread's C proxy queue and its wait is a futex on the SAB input ring. JS work posted to the
  deputy after `Run` is serviced only by that drain (WatchMemoryUsage's callback is); a JSExport call to it would not be.
* **emscripten 6 no longer puts `wasmMemory` on `Module`**: from the page, use `getDotnetRuntime(0).localHeapViewU8()`.
* **dlmalloc's memalign never reuses its own freed block for the same request** (R528): it mallocs request + alignment
  + 12 and returns the slack, so the freed chunk is exactly the request. Every sgen block came through it (LOS objects,
  major arenas, mono_valloc), and the holes ratcheted sbrk to ~1.1 GB over in-use under chunk loading: the 26.2 OOM.
  `mono_valloc_aligned` now serves those alignments by malloc + slack (`MONO_WASM_VALLOC_SLACK`); keep new aligned
  allocations on that path, and read free chunks by exact size (`runs/2026-11-08-memwalk/freesizes.js`).
* **wasmfs's OPFS backend caches negative lookups** (R491, `emsdk.9.patch`). JS that creates OPFS entries directly must
  call `opfsInvalidate()` (ikvmcraft `frontend/src/opfs.ts`), or the game will not see them.
* **IKVM's relink drain must never finish a type or call a Java class loader** (R514). It runs on a ThreadPool thread; a
  `loadClass` there can take a class's loading lock and then Mixin's `synchronized` transformer, which the mixin thread
  holds while it needs the same class -- the 26.2 boot hang. `ByteCodeHelper.InRelinkDrain` makes Finish and loads
  refuse (`[IKVM lazybody] ... drainRefused`). **Before adding work to the drain, check it cannot reach a loader or
  another type's Finish** (R515's RelinkTarget did, and hung 4 of 4 boots). R527's signature loads (`IKVM_LAZY_SIGLOAD`)
  run on the generation-1 Java thread, and never inside a `loadClass` or a class definition.
* **A created TypeBuilder has no member builders** (R531, `MONO_SRE_RELEASE`, sre.c `ves_icall_TypeBuilder_release_builders`):
  unless it is generic or an enum, its MethodDef, FieldDef and pseudo tokens move to the image's `resolved_tokens`, its
  overrides to `released_overrides`, and `methods` / `ctors` / `fields` / `properties` / `events` are null from then on. New
  code that reads those arrays after CreateType, C or managed, must go through `created` or the maps. A dynamic-token lookup
  asks `resolved_tokens` before the token table and again after a miss.
* **Managed stack walks ask V8** (R516, `exceptions-wasm-stackwalk.inc`, `MONO_WASM_STACKWALK_V8`). mono's LMF chain
  sees only interpreter segments and EH records, so JIT and AOT frames without clauses were invisible to StackFrame,
  getCallerClass and exception traces. A walk merges V8's CallSites with the LMF chain. It is opt-in by consumer: keep
  anything that only wants an assembly on the LMF walk (`Type.GetType` walks ~1 M times a boot; as V8 walks that was
  47.6 s). A phantom INTERP_EXIT push must be marked (`interp_lmf_mark_phantom`). In `[wasm-jit stackwalk]` every
  `fallback` and `verify fail` must read 0; `MONO_WASM_STACKWALK_VERIFY=1` checks each walk against the legacy one.

## Architecture invariants -- check a design against these before building it

### Bytes are process-wide; instantiation and admission are per-worker

There is no module broadcast. The emitted bytes are published once and are identical for every worker; each worker
instantiates them itself, with per-worker imports (the `EM_ASM` at the instantiate sites):

```js
{ m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports["__cpp_exception"] }, s: { p, l, c, v, n, d, m, b, i, g } }
```

* **Emitted bytes must not depend on anything per-thread**; that would defeat V8's in-process cache keyed on wire
  bytes. An f-slot NUMBER is process-wide and fine to bake.
* **Per-thread data reaches a module through imported globals**: `global.get` of an immutable import resolves per
  worker for one load. Indices 0..9 are a hard-coded contract with the emitter (`wasm-encoder.c`, before
  `emit_import_section`); `s.p` is the only mutable one. Adding a global is not free.
* **`s.g` is how a worker learns anything**: one i32 per worker in a process-wide slab. Non-zero sends the emitted
  safepoint to its helper (the GC wants to suspend, or a publication is not adopted here yet). Writers only set; only
  the owning worker clears. One word for both conditions keeps the check to one load per back-edge (R268).
* Installation is per-worker, which is why admission gates entry rather than generated code testing anything.

### A JS worker outlives its pthreads (R293)

Emscripten reuses workers: the next `pthread_create` can get the same worker (instance, function table, JS realm) with
a new TLS block and stack. So the function table, `Module.__wjSlotFn` and every JIT instance are per WORKER, while
`wj_slot_live`, the PICs, `wj_scratch` and the island state are per PTHREAD, and every instance imports the
instantiating pthread's `__thread` addresses. `mono_jiterp_wasm_jit_worker_reuse` resets the worker as each pthread is
set up on it (`[wasm-jit reuse]`; `foreign_scratch` must read 0). Do not fix it by discarding trampolines (R293b left
threads on the slow C boundary). **Before storing anything per worker, ask what happens when the pthread that built it
has exited.**

### Admission's contract: generated code carries no liveness check

Generated code `call_indirect`s an f-slot with no check. Admission installs a method's whole direct-call closure before
the method may go live. So:

* Proving a dependency is the right method is not proving its slot is installed on this worker. What matters at runtime
  is `mono_wasm_jit_slot_live(<the f-slot the caller baked>)`. If the callee re-registered, that slot never goes live.
* **A refusal is retried at the next dispatch at full DFS cost, and the rate is not self-limiting**: R244 measured 14-23
  million `admitDepNotLive` per run, 26% of the client thread, with `registered` flat. Before adding a refusal, decide
  whether its condition can ever clear, and count it. `registered` staying flat is not evidence of health.

### `table[fslot] != null` is not a liveness test

Unallocated slots are prefilled with a real function, `mono_jiterp_placeholder_jit_call` `(i32,i32,i32,i32)->void`,
body `*thrown = 999`. Called with another signature it traps (`function signature mismatch`); called with that one it
silently writes 999 through its fourth argument. The authoritative test is the per-pthread bitmap
`mono_wasm_jit_slot_live()`. **Before trusting anything found at an f-slot, ask whether THIS thread put it there.**
The converse is load-bearing too: an f-slot only ever holds a JIT `f` from this emitter (AOT bodies are reached through
their own table indices and functypes), which is what lets a caller bake an f-slot functype with no kind test.

### Lazy tier-1 f-slots: a pool slot is callable before its callee exists (R361-R366)

A direct call to a callee that is not JITted and not AOT-backed reserves an e/f pair from a per-functype pool
(`mini-wasm-lazy.inc`). On every worker that instantiated the slot's BANK, the f-slot holds a stub that binds the real
callee (`mono_wasm_jit_late_fslot`) or runs it in the interpreter.

* **A pool slot's real f is written only by admission, after its dependency walk**: callers go live against the stub,
  so the moment a real f lands, every caller on that worker calls it. The compiling worker validates without
  installing, and a slot's bank is instantiated BEFORE any real f in it (a later bank would write the stub back).
* Admission ensures a pool dep's bank and does not descend into it, except for a callee registered no-GC at that slot
  (`wj_lazy_dep_needs_real`). A pool slot is not otherwise credited no-GC: bind can compile and run a cctor.
* "Bank instantiated" is per pthread, and a bank bakes no TLS (the stub gets scratch from a helper, never `s.b`).
* Must read 0: `[wasm-jit lazy]` orphan, bank_fail, repair, notreal, drift, dead. `residual` counts callees the pool
  refused, which run through the interpreter.

### mono's rare paths are our common ones

Under IKVM a type-load failure is ordinary control flow, and `mono_interp_replace_method_body` swaps whole headers into
live wrappers thousands of times a run, so **a bad body arrives as a compile request.** A `g_assert` there loses the run
over one method the interpreter runs correctly: convert it into a counted bail (`mono_wasm_jit_note_bad_eh_clause`),
bounds check first. **Check reachability before converting**: four such asserts sat in `get_call_info`, which has no
callers here, and converting them would have made an unreachable abort into reachable bad codegen (R282).

### Resolve and consume in the same breath

Found in four subsystems: something looked up or validated on one thread, then used after a window in which another
thread changed it. Resolve and consume together, or re-resolve at use. An early pass (ranking, batching, sorting) may
decide **order, never identity**: a stale size mis-ranks, a stale pointer frames the wrong function.

* **No metadata operation is safe to add to a worker-side compile section.** `mono_method_get_full_name` can trigger
  lazy type resolution, and `mono_interp_get_imethod` CREATES on a miss: use `mono_interp_peek_imethod` for a predicate
  whose answer does not need one.
* **A guard that catches a race must count its catches.** A counter stuck at 0 means the guard is dead code.

### An array indexed by a global id is sized per pthread (R489)

Every WRITE through a descriptor or slot id ensures the array first; ensuring at the walk's entry is not enough.
`wj_admit_install_only` wrote batch generations past the end of such an array, and that was the heap corruption behind
the world-load hang, the changed module bytes and the sgen faults (`[wasm-jit bounds] desc_grow=` counts the growth).

### The scratch belongs to one call until that call has copied it out (R500)

A JIT->interpreter call spills its arguments into the pthread's scratch. Anything that runs managed code between the
spill and the copy (preparing or transforming a cold callee runs IKVM code, whose residuals reuse the scratch) hands the
callee another call's arguments. `wj_call_interp_inner` snapshots first; `WASM_JIT_ARGS_CLOBBERED` counts the catches
(1-2 a world creation is normal).

### An EH method's LMF record lives in its own frame (R353)

An EH method links a `WjEhRec` from its own C-stack frame into the thread's LMF chain (`MONO_WASM_JIT_EH_REC`), so **an
exit that skips the unlink leaves the chain pointing into dead stack.** Every exit unlinks (EMIT_REF_LEAVE, the landing
pad's no-handler rethrow, `mono_wasm_jit_ehrec_unlink`), and the interp->JIT boundary pops records below its saved C SP.
Before adding an exit to an EH method, decide how it unlinks. `[wasm-jit ehrec] dispatch_norec` must read 0.

### Ownership: publish before you free, and prefer leaking

Registry entries, depsets and batch descriptors are read lock-free by other workers, and nothing recalls a published
pointer. Old arrays stay allocated. Publish the contents, `mono_memory_barrier()`, then the pointer or count. A rollback
stops new readers only; do not free what was published. Retired registry bytes are freed only once readers have left
(`MONO_WASM_JIT_RETIRE_FREE`).

### There is ONE call profile

`InterpMethod.wasm_jit_profile` (`WjCallProfile`, interp.c) is the only record of what a method's virtual and delegate
sites dispatch to. Only `wj_prof_record` writes it, fed by the interpreter's virtual and delegate dispatch and by the
JIT's IC miss (~79% of observations). Devirt prediction, IC sizing and the batch planner read it. Not profiles, and not
to be merged into it: `wj_vcall_pic` / `wj_delegate_pic` (per-thread dispatch state; only the miss path feeds the
profile) and `wj_entry_edges` (interp->JIT transitions). A stable IC id is not a profile record's id: reusing the
record's makes two sites calling the same base share a PIC slot.

### Retained raw `MonoMethod *` are not always dereferenceable

The registry and the profile keep raw pointers while IKVM creates and frees dynamic types. It shows three ways: an OOB in
`jit_mm_for_method`, an assert in `mono_class_get_flags`, a misaligned `a_cas` in `mono_mem_manager_lock`.
`mono_interp_peek_imethod` does not protect: it dereferences first.

* Answer a one-bit question by recording the bit where it becomes true, in a set keyed by the pointer (hashing never
  dereferences): `mono_wasm_jit_callee_perm_unjittable`. In `[wasm-jit permset]`, `adds` is the liveness check and
  `full` must be 0.
* `mono_wasm_jit_method_usable` (`MONO_WASM_JIT_BADMETH`) catches the detectable subset only.
* **Test every retained pointer you are about to dereference, not the one it was kept beside** (R479: an entry's
  `logical_imethod` was freed while its method passed). The freed set keys InterpMethods too.
* Still retained: `WjRegEntry.body_method` / `.logical_method`, `WjProfSite.id_targets[]`, `WjDepSet.method[]`,
  `wj_sync_inner_canon`, `WjLazySlot.method` (tested with `mono_wasm_jit_method_known_dead`). The root fix, stop
  retaining or purge on free, is not done.

## Where we stand against other JVMs

### Minecraft 26.2 with no mods (the target) against HotSpot 25

These numbers were taken on wasm-jspi-jit-10.0.3. The .NET 11 / emscripten 6 move was a wash on both workloads
(2026-11-10, interleaved n=3: -0.8% cycles, +1.7% instructions a tick on 262n; jbox2d 0.6019 ms/step on 11 against 0.6013 on 10.0.3).

Server thread per tick, Q60 window (60 s, opening ~183 s after join), both sides pinned to the P-cores, 0 skipped ticks;
native JDK 25.0.5 n=3, ours 2bc83b4fdce with default knobs n=3; mean (range). 2026-11-06, `runs/2026-11-06-262`,
findings note "26.2 with no mods against HotSpot 25":

| M per tick | HotSpot 25 | IKVM on native CoreCLR 10 | ours | ours / HotSpot | ours / CoreCLR |
|---|---|---|---|---|---|
| cycles | 18.58 (18.24-19.18) | 37.00 (36.30-37.38) | 101.06 (99.63-103.25) | 5.44x | 2.73x |
| instructions | 15.09 (14.88-15.25) | 28.31 (27.88-28.90) | 93.77 (92.86-94.35) | 6.21x | 3.31x |
| calls | 0.106 | 0.672 | 0.620 | 5.87x | 0.92x |
| indirect branches | 0.056 | 0.530 | 0.565 | 10.15x | 1.07x |
| loads | 4.63 | 9.23 | 33.74 | 7.29x | 3.65x |
| stores | 1.38 | 3.39 | 19.62 | 14.20x | 5.79x |
| IPC | 0.81 | 0.77 | 0.93 | | |

**IKVM on native CoreCLR is the CLR-hosted reference** (2026-11-08, n=3, `runs/2026-11-08-ikvmclr` k2-k4,
`$WJ_STATE/arms/ikvm-clr10`: ikvmcraft's served IKVM DLLs on Microsoft's CoreCLR 10.0.14, `mcnative --ikvm`;
findings note "IKVM on native CoreCLR"). It is 1.99x HotSpot in cycles and 1.88x in instructions. Run in OUR IKVM
configuration (lazy class loading with in-place relinks, through a MonoMod backend: 46.67 M cycles, 39.81 M
instructions, 2026-11-08, `$WJ_STATE/arms/ikvm-clr10-relink`), our 5.44x splits as **~2.0x IKVM on any CLR x ~1.25x
lazy linking's residue (1.39x in instructions; still there with every relink installed) x ~2.17x wasm, mono and our
JIT (2.36x in instructions)**. 2.17x is therefore the ceiling of backend-only work; the rest needs IKVM's output
changed (findings note "the limit of backend work"). **It makes as many calls and indirect branches as we do**
(6.3x and 9.5x HotSpot's): the call COUNT is IKVM's translation, which RyuJIT with tiered PGO does not inline away
either. What it does not pay is our cost per call: ~42 instructions a call against our ~151, and a fifth of our
stores. Read it with three caveats:
* its IKVM links eagerly (`IKVM_LAZY_CLASS_LOADING=0`, upstream IKVM's mode). With the wasm app's switches it is 58.2 M
  cycles (n=1): IKVM's in-place relink is mono-only, so every candidate keeps generation 1, whose lazy-link machinery
  (dynamic binder, `DelegateShuffleThunk`, dynamic casts) runs on every call. A MonoMod relink backend (generation 1's
  code jumps to generation 2; `$WJ_STATE/arms/ikvm-clr10-relink`, findings note "a MonoMod relink backend") takes
  instructions 63.05 -> 39.81 (n=2 / n=3, same binary) and cycles ~-22%, still +39% instructions over classic;
* workstation GC runs young collections on the server thread (~4.5 M cycles of its 37.0, `gc_heap` card scanning,
  mark, relocate, and `RhpNewFast`), where HotSpot's G1 runs them on its own threads; without them it is ~1.75x;
* two patches let it run at all and change no steady-state code: libcoreclr skips the debugger's metadata copy on every
  type load (R517), and IKVM.Runtime tolerates a re-entrant finish after `CreateType`, which 26.2's `TimeSource`
  triggers on CoreCLR only (the arm's README).

1.16.1 on the same protocol reads 4.38x cycles and 5.16x instructions (`runs/2026-11-06-native`), with 16% of its gap in
the idle wait between ticks; 26.2 has none (`tickServer` holds 99.6% of our thread). Reproduce with
`runs/2026-11-06-262/batch.sh` (interleaved `hwrun` and `mcnative --warm-s <ours' offset>`, on the default seed and
stage), read with `wj native/mccmp` and `wj native/mcinsn`; the purpose cut below is `ledger/refresh.sh` +
`ledger.py --align`.

**The 78.7 M-instruction gap.** Kind of code partitions the whole gap; each other cut covers the part it names.

* **By kind of code**: our JIT'd Java against HotSpot's compiled Java is 78.95 vs 14.43 (5.47x), 82% of the gap. The
  rest is code HotSpot has no counterpart for, 14.8 against 0.66 of stubs and libjvm: the IKVM runtime 4.04, tier
  helpers 3.76, mono C 2.47, IKVM interface/bridge stubs 1.16, mono wrappers 0.96.
* **By purpose (ours only: the call boundary is half of what we execute).** The gap ledger on the current build
  (`runs/2026-11-07-262ledger`, one mapping run, 2026-11-07: 90.74 M instructions and 107.17 M cycles a tick, 3.4%
  unmapped) puts call, frame and dispatch machinery at **47.3 M instructions (52%) and 56.9 M cycles (53%), three times
  HotSpot's whole tick**:
  * calls and dispatch 23.8 M instructions / 31.0 M cycles: METHOD_CALL 7.2, dispatch loads 4.8 (vtable / IMT / guard
    loads, 15.9 M cycles), DYN_CALL 4.4, VCALL_PIC 3.5, DISPATCH 2.3;
  * frame, entry and exit 14.6 / 20.7: FRAME (the `s.p` chain) 7.8, RETURN 2.4, EXIT 1.1, and V8's own prologue and
    entry 2.7 instructions but 8.7 M cycles;
  * EH records 3.0 (EH_PUSH 2.1, BB_ILOFS 0.7), safepoint polls 3.0, GC shadow-frame PIN stores 2.9.

  Method bodies are 29.1 M (32%): branches 9.4 (53% of them guards mono expands around `callvirt`), heap loads 7.1
  (73% `ldfld`; 21.5 M cycles, the largest single cycle sink), explicit null and bounds checks 4.7 (`ldfld` 1.9,
  `callvirt` 1.1; wasm has no implicit null check), arithmetic 2.9, and 1.47 M `mfence`s at Java volatile READS
  (`PalettedContainer.data` 0.90, `AtomicInteger.get` 0.33): the access gets a SEQ barrier, which the emitter lowers
  to `atomic.fence` and V8 to `mfence` (`instruction-selector-x64.cc` `VisitMemoryBarrier`). They cost ~2.4 M cycles
  (2.2%), and HotSpot emits nothing for a volatile read. Non-JIT code is 11.2 M; its largest item is IKVM's
  `getClassFromObject`, 3.2 M (below). On 1.16.1 the same ledger read machinery 40%, bodies 34%, runtime 22%
  (2026-11-05). Tier-1 code is 4.5% of instructions and 12.7% of cycles.
* **By instruction class** (generated Java code, one capture a side): the computation itself is only 2.4x (+4.4 M);
  the gap is around it. Memory is 7.6x (+26.4: loads +19.5, stores +6.8), guards and branches 6.1x (+18.9: cmp/test
  +10.5, jcc +7.9), registers and frame 6.0x (+15.7: spill/reload +7.5, moves +6.4, push/pop +1.9), calls +0.6. Our
  heap loads and stores include the GC shadow frame, the EH record and `s.p` (linear memory); HotSpot's frame is
  `%rsp`-relative and counts as spills. hwrun's and mcnative's instruction event is not PEBS-precise, so a sample
  skids onto a later instruction: in `j1a`, 0 samples land on an `mfence` and 2,610 on the instruction after one.
  Read classes from this cut, not single instructions; use the ledger's precise events for those.
* **By subsystem, entity ticking is the gap.** In instructions, entities are 10.71 vs 69.77 (6.5x) and chunk ticking
  3.74 vs 19.80 (5.3x). In cycles, entities are 8.27 vs 67.24 (8.1x, 72% of the 82.5 M cycle gap) and chunk ticking
  (random ticks, spawning) is 8.43 vs 24.07 (2.9x). Chunk ticking is memory-bound on both sides (native IPC 0.44).
  Entity code runs at a lower IPC than HotSpot's (1.04 vs 1.30), so it costs more than its instruction count. Inside
  it no subsystem dominates: mob AI (`GoalSelector.tick`), movement and collision (`Entity.move`, `BlockCollisions`,
  `checkInsideBlocks`) each read 7-9x in cycles, `checkInsideBlocks` 12.6x.
* **Block-state access is the largest single place**: `LevelChunk.getBlockState` exclusive is 17.32 vs 3.05 M
  instructions (18% of the gap), about the tick's own ratio. Its stores are not: 27% of all ours (5.37 M a tick
  against 0.044), in a lookup whose Java stores nothing. Its `catch Throwable` (a crash report) makes it an EH method,
  which mono will not inline, and which pays the R353 record on every call. EH scaffolding is 2.0 of the 6.4 M a tick
  the ledger charges to its own IL (EH_PUSH 1.58). Its live function is 29 KB of x86 with its tier-2 inlines and
  V8's. This is 1.16.1's DataTracker.get shape, and `MONO_WASM_JIT_EH_INLINE` did not pay there.
* **Out of line with the tick's 6.21x** is the modern-Java machinery the bytecode census predicted (excess in M
  instructions a tick, inclusive and partly nested):
  * `ArrayList.addAll` +1.87, `toArray` +1.48 and `Arrays.copyOf` +1.38, each 16-18x. `copyOf` asks for the array's
    `getClass()`, and IKVM answers that through a ConditionalWeakTable lookup for every array: `getClassFromObject`
    executes 3.2 M a tick, 3.5% of ours.
  * `Holder$Reference.value` +1.39 (115x, a field getter).
  * `LevelHeightAccessor.getMaxY` +1.03 (211x, an interface default method), plus IKVM's forwarding stubs on its
    implementors, frames HotSpot does not have.
  * `TagKey.hashCode` +0.87: an identity hash through mono's C.
  * `EnumMap.containsKey` and `EnumMap.get`, which call `getClass()` again.
  * `Thread.currentThread`, 0.74 as a frame of its own (R317's FAST_TLS lowering does not reach these sites).
  * `LegacyRandomSource.next` at 60x: an AtomicLong CAS through `Unsafe` emulation.

  The top 45 methods hold 72% of the gap; 375 methods carry more than 0.01 M a tick each.

**26.2's bytecode has more and smaller methods** (`native/jarcensus`, against 1.16.1): 2.1x the methods, p90 80 B
against 112, 76% at or under HotSpot's 35-byte always-inline size, more `invokeinterface` / `invokestatic` /
`invokedynamic` per instruction, 1,754 records and 950 default methods. Each default method gets an IKVM forwarding stub
per implementor. So the call boundary and the inliner weigh more on 26.2 than on 1.16.1.

### jbox2d (the kernel)

jbox2d fixed-work, one jar, every run checksum-gated, R440 protocol, Chromium 156, n=4 a row (2026-11-04, R526, R527):

| | ms/step | vs JDK 25 | M cycles / M instructions a step |
|---|---|---|---|
| OpenJDK 25 (HotSpot) | 0.314 | 1.00x | 1.48 / 2.85 |
| TeaVM FULL (AOT to wasm, linear memory) | 0.417 | 1.33x | 1.97 / 5.61 |
| ours, lazy class loading off | 0.495 | 1.58x | |
| **ours, shipped** | **0.60** | **1.91x** | 2.76 / 8.33 |
| CheerpJ 5.0, JS engine, Java 8 | 1.162 | 3.70x | 5.44 / 16.08 |
| CheerpJ 5.0, WasmGC engine, Java 8 | 1.188 | 3.78x | 5.50 / 21.87 |
| CheerpJ 5.0, JS engine, Java 25 (like-for-like: IKVM's class library is Java 25's) | 1.250 | 3.98x | 5.96 / 17.15 |

* **Our best was 0.544 at R440** (2026-10-04, when TeaVM read 0.423 on the same protocol: 1.28x). The shipped row's
  counters were taken in the same relinked state through a knob R527 deleted. For scale, from 2026-09 (12-batch means):
  IKVM on CoreCLR 0.397, IKVM on mono's x64 minijit 0.611, mono's interpreter 3.05.
* **The gap to R440 is erased signatures** (R527): a class not loaded when its user linked is
  `object modopt(__<Unloadable>)` in the baked CLR signature, so generation 2 casts after every use, and
  `TreeNodeStack.push` grows to 143 IL bytes against tier 2's 120-byte guarded-inline cap (`MONO_WASM_JIT_T2_GI_SIZE`).
  R527 has the census and the two levers not taken.
* **CheerpJ** (R526): the JS engine maps Java objects to JS objects and runs its hottest method at 3.6x our cycles;
  outside its generated code, megamorphic keyed-store ICs and a subtype test that walks class metadata cost it most. Its
  WasmGC engine runs this jar only on Java 8 (an illegal cast at startup on 17+). Like ours it emits one module per
  compiled unit, and it puts a `ref.cast` before every field access.
* **WasmGC is not the faster target**: TeaVM's wasm-gc backend is slower than its linear-memory one on the same V8
  (0.430 vs 0.415, R180).

**Quote jbox2d from R440's protocol:** `wj j2d/run ours <dir> --no-perf --no-attr --schedule 600,200,60 --window-batches
40,60`. It pins to `WJ_PCPUS` and reads batches 41-60 of 60, because tier-2 compiles still land in a 12-batch window
(R416). **Before quoting any jbox2d or Minecraft timing, read the `[IKVM lazybody] stuck #N` lines**: a hot method named
there runs IKVM's generation-1 dispatch, and the number measures that, not the tier (R523).

### What the gap is made of

Most of the evidence below was taken on 1.16.1 and is labelled so; re-establish a rule on 26.2 before leaning on its
number.

* **The tick's gap is executed instructions more than their cost**: 26.2 is 6.21x HotSpot 25 in instructions and 5.44x in
  cycles, and 1.16.1 was ~5x JDK 8 in both (R458). On 1.16.1, per instruction our back end stalls like HotSpot's. Our
  front end was 1.6-2.5x worse (icache, iTLB, BTB), and that tax follows CODE VOLUME, not placement: C1's byte cut took
  6-10% off it (R459), while packing the hot set into 28 modules took nothing (R463). The levers are fewer executed
  instructions and smaller hot functions, not cheaper instructions. On 26.2, entity code runs at a lower IPC than
  HotSpot's (1.04 vs 1.30); its front end has not been measured.
* **The kernel no longer explains the game**: jbox2d is ~2x HotSpot while the 26.2 tick is ~6x. Minecraft adds machinery a
  kernel does not exercise: IKVM's unresolved-member dispatch (the lazy-link fallback for unloadable types -- NOT
  `java.lang.invoke`, which is small), our dispatch, IC and boundary helpers, Java type checks, `Unsafe` emulation (no
  fast path), monitors, and on 26.2 IKVM's default-method forwarding stubs and `getClass()` on arrays.
* **The bottleneck is the call boundary.** On 1.16.1, call infrastructure was 35.7% of the server thread executed (R290:
  the prologue band with the `s.p` chain, dynamic and constant `call_indirect`, V8 frames), plus ~12% in our helpers.
  Guards, spills and memory traffic are downstream of call density. Judge a change by whether it removes calls, shortens
  prologues or shortens live ranges across calls (`wj callinfra`).
* **Do not lead a plan with inlining or devirt.** On 1.16.1, HotSpot's whole inlining family is worth 2.24x and its
  devirtualization 1.37x (R240), and a HotSpot with no inliner was still 13.6x cheaper than us. TeaVM with every pass
  off still runs 1.71x HotSpot (passes are a third of its advantage), and its devirt and inlining are mutually redundant
  (`call_indirect` count: 35 baseline, 791 with both off), so never measure one alone.
* **Our per-method baseline is the codegen problem**: one 1.16.1 method of 64 bytecode bytes is 256 B of x86 in a
  no-inline HotSpot and was 7,936 B with 162 calls in ours (R240). The per-method gap spans 2x to 40x (R244);
  near-parity methods exist, so the tail has findable causes.
* **There is no wasm, V8 or memory-model barrier**: TeaVM is 1.33x HotSpot on this V8, in linear memory.
* **We ARE the middle end.** V8 allocates the registers, and mono's register allocators are all excluded for
  `COMPILE_WASM`. The tier's optimization string is baked into the runtime (`wasm_jit_extra_opt`, `mini.c`, replacing
  `cfg->opt`); in it `loop` never runs LICM (that call is gated on `COMPILE_LLVM`) and `alias-analysis` is locals-only.
  `MONO_OPT_INLINE` admits only non-virtual sites or final targets and IKVM emits `callvirt` for every non-final Java
  method, so Java calls inline only through guarded inlining (`MONO_WASM_JIT_GUARDED_INLINE`, tier 2).

### Dispatch: what can become a direct call

* Only `wj_emit_method_call` emits `WASM_RELOC_CALL`, the one relocation the assembler turns into `call <funcidx>`, and
  only for a callee that already has an f-slot. So the reachable set is the devirt predicted arms, and only
  RE-EMISSION creates the holes co-location fills: never A/B the two separately. An IC site (`WASM_RELOC_INDIRECT`)
  stays indirect until devirt covers it; delegates and inline-AOT calls are indirect by construction (~39% of dispatch
  when last split), so even full devirt coverage caps direct dispatch near 61%.
* **A guarded arm pays above ~15% capture**, above ~50% if its target does not co-locate (then the "direct" call is
  itself a `call_indirect`): an arm hit is ~3 x86 + `call`, an IC hit ~21. Co-location is the precondition for an arm.
* **The devirt census counts SITES, not executions.** Weight a bucket by execution before spending on it (`no_fslot` was
  3.1% of sites and 0.0% of hot IC execution). 99.3% of profile observations arrive after the method was JITted, so
  raising the JIT threshold cannot convert `no_rec`; re-emission can.
* Most IC misses were inheritance, not polymorphism (R310). R311's method-identity check (`vt->klass->vtable[slot]`
  against the cached method) took them from 119-128 M to 31-32 M a window, and the server tick 8.9% down.

## Closed leads -- do not re-run these

| lead | why it is closed |
|---|---|
| GC, or the GC/object model, as a lever | ~0.3% of wall. Pin stores and frame zeroing are ~3% of emitted instructions, and GC points outnumber ref defs 1.84:1, so a safepoint model roughly doubles the stores (R126, R180) |
| WasmGC as a redesign target | slower than linear memory in TeaVM (R180) and no faster than JS in CheerpJ (R526); mono's heap, metadata and AOT half share linear-memory objects with JIT code |
| exception handling as a cost | 0.24% self. `mono_llvm_cpp_catch_exception` on 92.5% of stacks is a wrapper frame, not an exception in flight. What does cost is the R353 record every activation of an EH method pays (3.0 M a tick on 26.2, 2026-11-07). |
| inlining methods with clauses (`MONO_WASM_JIT_EH_INLINE`) | three Q60 A/Bs on 1.16.1 (2026-11-06): +3.6% cycles at 39, +1.5% at 35, +3.5% at 423 with the hot callee (DataTracker.getEntry) admitted, calls flat. A clause-free caller that takes an EH callee becomes an EH root and pays the record itself. It pays only if a root can gain clauses without a per-activation record (E4, unbuilt) |
| the interpreter on the hot path | `mono_interp_exec_method` 1.12x per stack; 72% of stacks hold only the thread-entry frame |
| Liftoff / V8 tiering | 98.4% of the tier's time is TurboFan; the residue is ~1.8%, and `compilationPriority` is experimental (R370) |
| local renaming, coalescing, `local.tee`, copy chains (`MONO_WASM_JIT_COALESCE`) | local ops are free in V8; wire size only |
| method imports (`MONO_WASM_JIT_DIRECT_IMPORT`) | 100% of arms converted and nothing moved: an import call is still an indirect branch (R157-R159) |
| the frame pointer as a parameter (`MONO_WASM_JIT_THREAD_SP`) | +39% on jbox2d (R218). Adding a parameter costs at every call site: price a calling-convention change there |
| `MONO_WASM_JIT_INLINE_ILOFS=1` | 9.8% worse: it kept the il_state pointer live across every call (`s.i` re-derives it per store) |
| raising `MONO_INLINELIMIT` | bodies +3.5-4.4%, calls per method +1% |
| `MONO_WASM_JIT_STABLE_IC_IDS` | two sites calling the same base share a PIC slot |
| `MONO_WASM_JIT_DELEGATE_OBJ_PIC` | 44,000x fewer miss publications and a stub only 1.7% smaller: the per-worker liveness probe is unavoidable (R193) |
| profile-driven re-emission (`MONO_WASM_JIT_REEMIT`) | sound and throughput-bound (drain reach, compile-lock contention). An IKVM body swap enqueues MANDATORY re-emission whatever this knob says, so `REEMIT=0` is not "no re-emission": read `WJC_REEMIT_REQUIRED_*` |
| more static co-location capture (`COLOCATE_MERGE`, `COLOCATE_MAX`) | co-location is a partition: two callers with arms on one target cannot both hold it (R195). Capture 32 -> 44% moved execution-weighted co-residency 4.5 -> 6.5% (R258). `max=64` + `bytes=131072` crashes |
| hot-set consolidation (`MONO_WASM_JIT_B4_HOT`) | 98% of the plateau's heat in 28 modules: cycles -0.2%, front-end stalls flat (R463). Kept as an instrument |
| SCC co-location, shadow copies | deleted. Cycles are rare; shadows bought ~63% arm-local at +50% bodies (R201) |
| `IKVM_LAZY_BODIES=0` | 11.2% worse |
| refusing unloadable-signature relink candidates | admitting them (`IKVM_LAZY_SIG=1`, ikvmcraft) is -9.5% server tick (R251) |
| `IKVM_LAZY_CTORS=1` | clean mechanism, a wash on the tick against a ~20% instrument spread (R280) |
| `MONO_WASM_JIT_RELINK_JITTED=0` | R252's -5.2% for `1` was measured on a design since replaced (generation 2 now republishes in place), and not re-measured (R268) |
| removing the `__<>DynamicBinder__` receiver castclass | it IS the type check; without it a ClassCastException becomes memory corruption |
| the B4 drainer's double retire as the heap-corruption cause | refuted (R470); the writer was R489's |
| monitors as mutex traffic | the uncontended CAS fast path exists; the ~3% is call overhead around it. An inline CAS is unbuilt, ceiling ~1.5-2% |
| ikvmc static compilation of Minecraft | out of scope: runtime version loading and drop-in mods are requirements |
| IKVM lazy class loading off (`IKVM_LAZY_CLASS_LOADING=0`) as a speed lever | stays ON: eager linking works only for mod sets that happen to load under IKVM's eager loading, and full mod compatibility is a requirement (2026-11-08). Its cost (x1.25 cycles on CoreCLR even with every relink; R527's erased signatures on jbox2d) is won back inside lazy mode. Measuring it off as an instrument is fine; proposing to ship it is not |

## The GL stack: mesa-wink, GL on the main thread (R477)

Mesa's GL on the `gex_wgpu` Gallium driver over WebGPU. In the mesa-wink checkout, `bash wink/build.sh --variant=mt
--emsdk=$HOME/emsdk --test` builds it and runs its smoke suite. It reaches the app through ikvm-wasm-build's GLFW port
(`tools/native-deps`: `build-glfw.sh --wink-out=...`, `install-glfw.sh --dest=<ikvmcraft>`). GL runs on the browser's main thread
(`?wj.WINK_GL_MAIN`, default 1; `=0`, GL on a worker, needed JSPI and is refused since the .NET 11 move): the render
thread only records (Mesa's glthread), and wink executes on main. At Q60
on 1.16.1 with Minecraft's limiter lifted that is ~170-185 fps, and the render thread (63% JIT-tier Java) is the limiter
(R479). The 26.2 target runs vanilla's renderer (no Sodium). Its census line reads 2.0 waits a frame on main (one FenceSync
and one ClientWaitSync, the round trips below), 0 read live, 0 GPU readbacks and 0.00 MiB of persistent mappings a
submit (`runs/2026-11-06-262/ours/o2a`). Its frame rate and client-thread profile have not been measured.

* **A GL call that returns a value or reads client memory is a round trip through main's event loop**, unless glthread
  answers or copies it. The line every 240 swaps (`wink: the GL thread waited for the main thread N times a frame ...;
  read live M`) should read N ~0 and M 0 in game; answer a new per-frame query in glthread.
* **The canvas must not be transferred**: `wjharness/build/deploy.sh`'s post-publish patch decides it by testing
  `window.WINK_GL_MAIN`, and deploy refuses without that test (the Makefile's copy is not what deploy runs).
* **A GPU readback on main stalls the render thread too.** gex_wgpu keeps CPU shadows only for indirect command buffers
  and buffers up to 64 KiB (`GEX_WGPU_ADOPT_SHADOW`); the census's `GPU readbacks a frame on main` must read 0.
* **A write-mapped persistent buffer is re-sent at every submit** unless it is FLUSH_EXPLICIT and non-coherent (GL leaves
  its unflushed writes undefined, and `glFlushMappedBufferRange` already uploaded the rest); other mappings send a diff.
  26.2's Sodium maps a 32 MB staging buffer that way, and sending it whole was 16 fps against 108 (R509; unconditional
  since mesa-wink 51eb6bdb). Read `gex-wgpu: persistent mappings sent X MiB a submit`. 26.2's per-frame
  `glClientWaitSync` is two round trips through main, unfixed.
* The ~62 fps ceiling exists only in the headless harness (compare GL configurations with `MC_UNCAPPED=1`). Past it, the
  seed's `maxFps:120` holds (`?wj.IKVM_GAME_OPTIONS=maxFps:260` lifts it). Below the limiter, read cycles a frame, not fps.
* Traps: an edit to the `emscripten-glfw` checkout needs `emscripten-glfw.patch` regenerated, or `build-glfw.sh`
  refuses; wink's suite hangs with "no result" ~1 run in 4 on upstream cases, so re-run once; `GEX_*`, `MESA_*` and
  `WINK_*` reach the driver through `?wj.<NAME>=`.

## Building and deploying

The runtime (this repo), ~9 min. A plain `./build.sh mono` is not this build:

```
../FNA-WASM-Build-net11/build-dotnet.sh . true ./dotnet-jit.zip
```

Then **deploy**, ~12 min, which is not optional. The page loads `frontend/public/_framework/dotnet.native.<hash>.wasm`,
which changes only when the loader is republished; deploy republishes, applies the post-publish patches and proves the
marker is in the SERVED bytes (a whole matrix once ran on a stale runtime):

```
wjharness/wj deploy '<a string constant your change added>'
```

A marker must be a string that lands in the wasm (a `printf` literal; not a static function's name, which can be
inlined away). Prove a revert both ways: the new marker present and the reverted build's absent.

* **`wj csyn <file>`** compiles one file with the real command line in under a second, against both compilation
  databases (`mini-wasm.c` builds with and without `HOST_BROWSER`). **It does not link**: `mini-wasm.c`, `interp.c`,
  `transform.c` and `tiering.c` also link into `mono-aot-cross`, so a call into `mini-wasm.c`'s `HOST_BROWSER` region
  needs its CALL SITE under `#if HOST_BROWSER`, or the real build fails at link ~9 minutes in.
* **The product is mixed-AOT: `make build AOT=true`** in ikvmcraft. `make build` alone does not boot (a `loader.c:1826`
  assertion ~44 s in, or an OOB). That is the missing AOT half, not a regression: check the build command before
  believing a boot failure.
* **IKVM.Runtime**: `wj ikvm-runtime --install` builds it the way ikvm-wasm-build does and stages it into ikvmcraft,
  keeping the replaced DLL in `state/arms/ikvm-runtime/`. Prove it is served by the DLL's sha256.
* **The canonical emsdk** is ikvmcraft's `statics/emsdk`: the emscripten .NET 11 provisions (6.0.3, 6ea9c28c38cd, in
  `.dotnet/wasm-tools/emscripten/6.0.2-linux-x64` -- the directory name is stale) with FNA-WASM-Build's `emsdk*.patch`
  applied and its cache rebuilt, by `freeze-emsdk.sh <that dir> <out.zip>`. The app links against ITS cache
  (`IkvmWasm.csproj` `WasmCachePath`), so libc, pthread and wasmfs questions are answered there. Every native is built
  with it too (one emsdk everywhere: the workload refuses an emcc that is not the pack's). Patching a system library
  means `embuilder build --force <lib>` with `EM_FROZEN_CACHE` set EMPTY (.NET's `.emscripten` reads it with
  `bool(os.getenv(..., 'True'))`), then a redeploy. **embuilder clears the whole cache when `cache/sanity.txt` names
  another toolchain path** (R502), and the provisioned sanity.txt names .NET's CI path; the freeze script handles both.
* **The deploy environment for this branch**: `IKVMCRAFT=~/Documents/ikvm-wasm/ikvmcraft-net11`,
  `FNA_WASM_BUILD=~/fna-wasm/FNA-WASM-Build-net11` (or set them in `~/.config/wjharness/machine.env`); the harness
  defaults to `dotnet-jit.zip`.
* **Never edit a source file while a build of it is in flight.** The binary's contents become unknowable, and so does
  every number taken on it. Treat the build and deploy windows as read-only and queue the edits.

## Measurement discipline

**The box** (i7-1360P) sits at 95-98 C in game, and that is normal: 2-3-core arms hold 3.6-3.7 GHz. Builds and deploys
are all-core and do throttle. Measure on mains power (preflight refuses on battery). Ambient desktop load is the confound
that is real, and preflight's refusal for it is worth obeying.

**Reading a run**

* **Read `verdict` first**: it is the one field saying whether a run counts against the build (it excludes `env`,
  `never-started` and `killed`). `mode=stall` classifies world-generation duration, not a fault, and `ok` is only a
  progress flag. Then read the ungated bound lines (`[wasm-jit bounds]`, `[wasm-jit spin]`, `[wasm-jit permset]`), which
  are valid with stats off; `mono_wasm_jit_liveness(14..17)` reads them from a wedged run.
* **Assert the tier is alive before reading any timing**: `registered` sane, `tableExhausted 0`, `faults []`. A wrong
  functype fails instantiation and silently falls back to the interpreter, which looks like a performance result.
* `inplace ok=` is quantised to 250; use `gen2 clean=`. `GC_MAJOR: (LOS overflow)` does not mean the LOS is full (R510).
* When a counter's total exceeds the sum of its attributed parts, the gap is the finding.

**Comparing**

* **fps is unusable on this box; composition shares are not** (an identical config twice: shares within 1%, fps 19%
  apart). The fps floor is ~12% (11.5% on windows of 100 s or more, 23% on 60 s). Under ~12%, **measure the mechanism**
  (`tiershape`, `hotinsn`, a tier dump), not the outcome.
* **Measure on the server tick, not the client frame.** Native's client frame is half Mesa, OpenAL and libc; its server
  tick is ~91% real Java. Name the server tid explicitly (`native/servertid.py`; `buckets.py` without `--tid` picks the
  CLIENT thread). Our ticks skip, so per-tick figures divide by executed ticks: quote skipped ticks too, and compare raw
  totals when tick counts match.
* **Use protocol Q60 for Minecraft, on the default seed `262n`** (`hwrun`'s defaults: `--warm-ms 160000 --bench-ms
  60000 --no-walk --no-save-quit`; the window opens ~183 s after join on 262n and ~198 s on mcsr, before the ~300 s
  autosave). A bare in-game window measures the ramp. On 1.16.1 world variance is ~+-3.5% a run; 262n's n=3 ranges were 3.6% in cycles
  and 1.6% in instructions (2026-11-06). Use n=3 per arm (A B B A B A) for 3-6% effects, and measure the mechanism below
  that. Run `hwrun.sh <batch> --drain` after a batch's last arm, before reading its censuses. For the native side use
  `mcnative` (default stage `sa262n`, default `--warm-s` 183 s), not `natrun`, which plays a Prism instance whose mods
  are not ours.
* **Run both orders**: in one series the arm running second was slower 6 of 6. Never quote non-overlapping ranges from
  a single order.
* **Pin every arm the same way (`MC_TASKSET`)**, and never compare batches that differ in it: chromium sizes its thread
  pool from `sched_getaffinity` (R449).
* **Prefer a within-binary knob A/B.** A cross-binary reading at this spread cannot carry a 4% claim.
* **Count absolute quantities, not shares of a moving denominator.** "Arm-local %" fell while local arms rose 13.7%.
* **Do not compute a share until every route has a counter and the routes are DISJOINT** (`ic_hit` and `ic_miss` are
  not). An uncounted route shows up as everything else looking bigger.
* **"N consecutive clean runs" is not a gate without its probability**: at a 19% fault rate, 10 clean in a row happens
  12% of the time on an unchanged build. Size a stability gate against the base rate, or call it a smoke test. An
  intervention against an intermittent fault needs a positive control.

**Instruments**

* **Bump a counter where the action HAPPENS, not where it is decided.** If they live apart, count both and assert equal.
* A diagnostic behind a default-off knob is not evidence of absence, and **a diagnostic that rides in the module changes
  what the module is** (R460: the `MONO_WASM_JIT_ORIGIN` section pushed bodies over the tier-2 size cap).
* **Verify a classifier against the emitted code before trusting it** (five have shipped wrong; `hotinsn.py --selftest`).
  **Test a gate against known-good data** (a checksum gate failed closed on a trailing newline). **A gate that cannot
  reach the case a change introduces is not evidence about it** (`enctest`'s t4).
* When a diagnostic prints nothing, walk the call chain from the printf outward to its trigger before concluding anything.
* To tell whether a hung run recovered, count only lines the APP emits; chromium keeps logging after the page is dead.

**Hygiene**

* A leftover compiler daemon taxes one arm unequally: `wj killdaemons` (exit 0 = safe). Preflight refuses while one is
  alive; obey it rather than passing `--force`.
* **`pgrep -f` / `pkill -f` match the searcher**: three incidents, two of them killing the invoking shell. Match by
  command line, keep only the expected `comm`, never kill self, and `kill <pid>` explicitly.
* `IKVM_DEFUSE_RANKED_AUTH=1` (passed on `mcsr` only; other seeds get `=0`) stops MCSR Ranked's account check from
  hanging the main screen. It changes what the game does, so it goes through the URL and stays visible in each run's
  command line.
* The native reference reads the Prism instance's own `options.txt`, which a manual session rewrites; `natrun.sh`
  refuses unless `renderDistance` is 12.

### Hangs, heap corruption and traps

* **`verdict=spin`** (no app progress, a thread running with `wchan=0`; perf is captured automatically) and
  **`verdict=wedge`** (no progress, all threads asleep) need opposite next instruments.
* The old world-load hang was `dlmallinfo` walking forever over a zeroed chunk header (R454x): a detector for heap
  corruption, whose writers were R489's and R500's (see the invariants). 26.2's boot hang at mixin apply was R514's lock
  order; `bootcheck --dump-on-stall` catches one.
* Instruments for the next writer: `MONO_WASM_JIT_BYTES_CHECK=1` (`[wasm-jit bytes] changed` must be 0);
  `MONO_GC_HEADER_AUDIT=1` (sgen headers checked at each collection); the free quarantine `MONO_FREE_QUARANTINE_MB=256`
  (removed; `git revert b5779c96628`), whose hit VALUES named R489's writer after four rounds of reading code had not;
  `MONO_GC_STORE_WATCH=1` (`git apply scratchpad/state/patches/r500-instruments.patch`).
* **Traps**: V8 keeps 10 frames by default, so `mcdrive` passes `--js-flags=--stack-trace-limit=64` (a flag, so it
  reaches workers); a truncated stack and a runaway one look alike. Symbolise rather than guess: V8 reports
  module-relative offsets and the split DWARF is code-section-relative, so run `llvm-symbolizer
  --obj=<ikvmcraft>/loader/obj/dotnet.native.debug.wasm --inlines <v8_addr - code_vma>` (`llvm-objdump --headers`;
  confirm against the enclosing subprogram's `DW_AT_low_pc`). An unrebased lookup silently returns nothing. Under LTO the
  inline function NAMES are wrong and the line numbers right. Date a trap class against `state/corpus` before reading
  code.

## The harness: `wjharness/` is code, `scratchpad/` is data

The measurement harness is tracked in `wjharness/` and runs as `wjharness/wj <tool>` (`wj ls`; the tool tables are in
`wjharness/README.md`). Start with its readers, not `perf report`, which takes minutes per query on these captures. Its
data root, `$WJ_HOME` (default `scratchpad/`, excluded via `.git/info/exclude`, never add it), is NOT RECOVERABLE and has
three parts with different rules:

* **`state/`** is persistent; `wj reap` refuses it at any depth. `seeds/` are browser profiles holding Minecraft, the
  mods, `options.txt` and a save in OPFS, reflink-copied for every run. **They are load-bearing**: without one the app
  re-downloads Minecraft during boot and boot measures the network, and each is a user's OPFS export that cannot be
  rebuilt without its tar. `refs/` holds captures later rounds compare against (`j2d-r440-ss`, ...), `arms/` the built
  reference arms (TeaVM, CheerpJ, the jvmti agent, replaced IKVM.Runtime DLLs), `native/` the native reference's staging,
  `corpus/` the archived run logs `lib/verdict.mjs` is validated against, `patches/` instrument patches.
* **`runs/<date>-<tag>/`** holds every driver's output and is disposable by construction. A driver refuses an output
  directory inside `wjharness/`. **A round's launcher lives in its run directory**; a script a second round needs is
  promoted into `wjharness/`, made portable and committed.
* **`notes/`** holds `MINECRAFT-FINDINGS.md`, untracked.

Data a round cites as `j2d/<name>`, `p<NN>` or `r<NNN>` is under `scratchpad/legacy/wj/`, text only: its perf captures,
perf maps, jitdumps, browser profiles and `*.wasm` / `*.pkl` / `*.dll` were deleted on 2026-11-08 (manifest
`notes/LEGACY-DELETION-2026-11-08.txt`), and its logs, reports and launchers remain. The captures kept whole are
`state/refs/j2d-<name>`.

* **No tool names a machine.** Locations come from `wjharness/lib/paths.mjs`: the environment, then
  `~/.config/wjharness/machine.env` (template `wjharness/machine.env.example`), then autodetection. A tool that needs an
  unconfigured checkout refuses and names the key. `wj doctor` reports what is missing. `wj selftest` (syntax, the
  verdict and hotinsn selftests, enctest, and a gate that fails if any tool names this machine's paths) is the check to
  run after editing the harness. CPU sets are `WJ_PCPUS` (an arm's pin, the P-cores here), `WJ_CENSUS_CPUS` and
  `WJ_BOOT_CPUS`; the raw-code perf event sets (`hwrun`, `natrun`, `fe/events.sh`) refuse on a CPU they were not
  written for.
* **Before measuring, run `wj fit`** (dev server up, `killdaemons`, preflight passing).
* **Long work goes under `wj bg <name> -- <cmd>`**: a transient `systemd-run --user` SERVICE (not `--scope`, which dies
  with the calling shell), log and exit code in `$WJ_RUNS/.bg/<name>.{log,rc}`. An agent's background shells are killed
  when it exits, and `setsid` + `disown` does not protect them. If a run's browser AND its node driver vanish together
  with no stall message, that is this, not an app fault. Poll the sentinel; do not rely on a backgrounded watcher.
* **Only `wj reap` deletes** (dry run by default): dead-pid `/tmp/perf-*.map`, old profiles in `runs/`, isolate logs.
  `/tmp` is a RAM-backed tmpfs and `--perf-basic-prof` writes a ~240 MB map per capture; stale ones once left 2.7 G free
  and runs died at page load, reading as a browser bug (R280f). Anything a run writes there needs an owner at teardown.
* **`--perf-basic-prof` and `--perf-prof` imply `--log`** (`v8/src/flags/flag-definitions.h`), which drops an
  `isolate-*-v8.log` per isolate into the browser's cwd. A new launcher that passes either must pass
  `--no-logfile-per-isolate --logfile=+` too.
* **A failed run keeps its browser profile**: it holds the OPFS save and the crash-time state, and is the only copy.
* **A terminal fault ends the run** (`MONO_WASM: forcing abort`, `js-oom`, `em-abort`, `renderer-crash`, `wasm-link`;
  see `verdict.mjs`). A wasm TRAP does not, because a worker can die and the run still produce a complete window.
* **`perf inject` is opt-in** (`mcperf --inject`): the python readers parse the raw jitdump, and inject's success path
  deletes it. **perf can silently mis-attach** (a 0.3 MB map and 133 samples read as a valid low-overhead run): gate on
  the symbol map's size.
* **Whole-process captures on this hybrid CPU use `task-clock`, not `cycles`**: `cycles:u` opens on `cpu_core` only, so
  every sample from a thread on an E-core is silently missing. Per-thread hardware censuses pin to the P-cores instead.
* **Chromium's own frames need debuginfod, by build-id**, and a distro keeps only a window of versions: a capture
  against a chromium whose debuginfo is gone stays `[unknown]` forever (`lib/debuginfo.mjs`, `PERFRAW_DSO_SYMS=1`).
* **`--marker` checks the managed assemblies too** (`lib/provenance.mjs`). Markers match raw bytes, so a method or type
  name works for IKVM.Runtime but an env-var name does not (it is UTF-16 in `#US`); compare the DLL's sha256 instead.
* **World generation varies ~3x on an unchanged build, and in-game fps is comparable only once the server has caught
  up** (`mcdrive` waits and records whether it settled); `mcab` refuses a verdict at `--rounds 1`.
* Do not commit `*.orig-backup` files.
