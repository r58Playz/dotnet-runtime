#include "mini.h"
#include "mini-runtime.h"
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/loader-internals.h>
#include <mono/metadata/icall-internals.h>
#include <mono/metadata/seq-points-data.h>
#include <mono/mini/aot-runtime.h>
#include <mono/mini/seq-points.h>
#include <mono/utils/mono-threads.h>
#include <mono/metadata/components.h>
#include <mono/metadata/gc-internals.h>
#include <mono/metadata/mono-hash-internals.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/mono-memory-model.h>   /* MONO_MEMORY_BARRIER_* for OP_MEMORY_BARRIER lowering */

#ifdef HOST_BROWSER
#ifndef DISABLE_THREADS
#include <mono/utils/mono-threads-wasm.h>
#endif
#endif

static int mono_wasm_debug_level = 0;
#ifndef DISABLE_JIT

#include "ir-emit.h"
#include "cpu-wasm.h"
#include "wasm-encoder.h"
#include <stdlib.h>
int mono_wasm_jit_get_callee_fslot (MonoMethod *m); /* interp/transform.c */
#ifdef HOST_BROWSER
#include <emscripten.h>
int mono_jiterp_allocate_table_entry (int type); /* interp/jiterpreter.c */
gpointer mono_interp_get_imethod (MonoMethod *method); /* interp/interp.c; kept opaque in this emitter */
gboolean mono_wasm_jit_prof_predict (gpointer caller, MonoMethod *base, MonoVTable **out_vt,
	MonoMethod **out_target, guint32 *out_samples, int *out_why); /* interp.c; lock-free pre-JIT receiver profile */
void mono_jiterp_wasm_jit_patch_interp_entry (void *imethod); /* jiterpreter-interp-entry.ts */
void mono_jiterp_wasm_jit_unpatch_interp_entry (void *imethod); /* jiterpreter-interp-entry.ts */
gint32 *mono_wasm_jit_worker_action_addr (void);
void mono_wasm_jit_safepoint_poll (void);
#define WJ_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define WJ_KEEPALIVE
#endif
/* ============================================================================================
 * HOW TO READ THE COMMENTS BELOW  (see CLAUDE.md at the repo root for the full rules)
 * ============================================================================================
 *
 * This emitter carries a lot of recorded measurement, because most of its shape was decided by A/B
 * rather than by argument. Three conventions make that usable instead of misleading:
 *
 *  1. A KNOB'S DEFAULT IS STATED ONCE, AT ITS INITIALISER, AND NOWHERE ELSE. If a comment somewhere
 *     else tells you what a default is, it is stale by construction -- do not add one. Six comments
 *     here once said "DEFAULT OFF" beside an initialiser holding 1, which is how the inline-AOT path
 *     (~90k calls per frame) came to read as inactive.
 *
 *  2. A NUMBER WITHOUT A WORKLOAD IS NOT A RESULT. Two different workloads were used: the jbox2d
 *     fixed-work kernel (early; homogeneous, one number, low variance) and full Minecraft (current;
 *     three phases, ~5% measurement floor, and a thermally throttled box). A percentage measured on
 *     jbox2d says nothing about Minecraft and vice versa, so every measurement below should name its
 *     workload. Where one does not, treat it as unverified history rather than as a current fact.
 *
 *  3. AN EXPERIMENT THAT FAILED IS WORTH MORE THAN ONE THAT SUCCEEDED, and is kept deliberately --
 *     see MONO_WASM_JIT_INLINE_ILOFS below for the model: hypothesis, measured result, and the
 *     general lesson. Deleting those invites the next reader to re-run them. What must NOT survive is
 *     an argument for a value the code no longer holds, or a number a later round retracted.
 *
 * Two facts about V8 are load-bearing for everything here and are cheap to re-check in
 * ~/Documents/ikvm-wasm/chromium/v8 rather than re-derive:
 *
 *  * `local.get` / `local.set` / `local.tee` emit NO instructions -- they are reads and writes of
 *    Turboshaft's SSA environment (src/wasm/turboshaft-graph-interface.cc, LocalGet/LocalSet/LocalTee).
 *    Local COUNT and local reuse are therefore a wire-size question only. What costs is how many
 *    values are live across a call.
 *  * V8 cannot inline anything this emitter produces. Inlining candidates come from the module's own
 *    call sites, and an imported function has wire_byte_size 0 and so scores 0
 *    (src/wasm/inlining-tree.h). One method per module means every call stays a real call, forever.
 *    That is why mono's own inliner, and the cost of the call sequence itself, are the levers here.
 * ============================================================================================ */

/* The runtime wasm-JIT emit result (slots / bytes / bail / retriable / blockers) is no longer relayed
 * through thread-locals: the emitter writes it onto the per-compile cfg->wasm_jit_result (see
 * MonoWasmJitResult in mini.h) and mono_wasm_force_compile copies it out after the compile returns.
 * Per-compile => re-entrancy-safe by construction (a nested cctor/AOT-init compile has its own cfg),
 * so the old "publish the success gate last behind a barrier" dance is gone. */
/* Automatic hotness trigger (Phase 5): when mono_wasm_jit_auto>0, the interp (MINT_CALL) counts
 * calls to each callee and force-compiles it to wasm once its hit count reaches mono_wasm_jit_thresh,
 * instead of requiring the method to be named in MONO_WASM_JIT_METHOD. -1 = uninitialized. */
/* -1 IS A SENTINEL, NOT A VALUE, AND IT MUST STAY ONE. mono_wasm_jit_auto_init's re-entry guard is
 * `if (mono_wasm_jit_thresh >= 0 && mono_wasm_jit_auto >= 0) return;`, so this variable is what marks
 * the whole knob block as "not yet read". Initialising it to 1 to express "default on" makes that guard
 * true on the FIRST call, so auto_init returns immediately and NO MONO_WASM_JIT_* ENV VAR IS EVER READ
 * -- every knob silently falls back to its static initialiser.
 *
 * That is not a hypothetical: it shipped for one build and voided a measurement. The symptom is nasty
 * because everything still WORKS (the static defaults are the shipped values now, so the tier behaves
 * correctly) -- what breaks is only the ability to OVERRIDE, so a knob A/B runs three identical arms
 * and reports whatever noise it saw. It also defeats the standard deployment check: the env-var STRING
 * is present in the binary, so deploy.sh's marker proof passes, and `[harness] env override: NAME=VAL`
 * still appears in the log because index.ts prints that for any name. The only sound check is a
 * BEHAVIOURAL one -- set MONO_WASM_JIT_STATS=1 and confirm a counter is non-zero.
 *
 * The default-on decision is expressed at the getenv site instead, which is where every other default
 * lives anyway. */
int mono_wasm_jit_auto = -1;
int mono_wasm_jit_badmeth = 1;   /* MONO_WASM_JIT_BADMETH: the dead-retained-MonoMethod guard; see mono_wasm_jit_method_usable */
/* Forward: defined far below, but the registry's two diagnostic name walks need it long before that. */
int mono_wasm_jit_method_usable (MonoMethod *m, int site);
int mono_wasm_jit_thresh = 500;   /* SHIPPED DEFAULT. Tuned on Minecraft 1.16.1 + Fabric + Sodium/Lithium under IKVM, 2026-08/09, plateau protocol. A number without a workload is not a result -- this one has one, and it is not necessarily right for anything else. */
/* auto-JIT hotness threshold. 2000 was the pre-Minecraft value; 500 is what the product runs. R204 cut `no_fslot` 69%% by moving it, and that is worth NOTHING on the plateau -- it is a boot/worldgen effect, because 99.3%% of profile observations arrive AFTER a method is JITted. Right for boot, not a frame-rate lever. */
/* MONO_WASM_JIT_OVER_AOT is deleted. It let the runtime wasm method-JIT compete with an
 * already-available AOT body (the interpreter kept code_type=COMPILED, so a failed emission,
 * publication or per-thread admission fell back to the AOT entry -- which is what made it safer than a
 * whitelist or an aotprofile trim, where an emitter-refused method drops to the INTERPRETER instead).
 *
 * Deleted because it STALLED AT BOOT, i.e. it has never been usable, and a knob that cannot be turned on
 * is not an experiment. The stall was a JIT-created delegate trapping in an AOT delegate-invoke wrapper
 * (perf-overaot/game.log, the same stack R376 hit), fixed in handle_delegate_ctor (R376) -- so the boot stall is
 * no longer a reason. Recording what it was aimed at, because that pool is real and this is now its
 * only trace: the "AOT image" bucket is not one thing, and 12.63%% of the in-game window is AOT-compiled
 * MANAGED code (IKVM_*, System_*, corlib_*) -- the same managed program, AOT'd because the emitter
 * bailed or AOT measured better, so addressable in principle. R173 also retracted "AOT is better
 * codegen": that comparison was confounded (--kind aot and --kind ours are different WORKLOADS), and on
 * the same instrument our tier is 13.78%% real compute against AOT's 12.52%%, worse only on register
 * pressure and better on guards.
 *
 * If that pool is picked up again, the mechanism is small -- gate the "AOT methods are permanently
 * ineligible at their hotness threshold" rule and keep the AOT body as fallback -- and the thing to fix
 * first is the boot stall, not the policy. */
int mono_wasm_jit_arity = 0;      /* MONO_WASM_JIT_ARITY=1: per-call-site receiver-arity histogram for the vcall miss population (N-way IC capture curve). Diagnostic — perturbs timing (like PROFILE_FAST); default off */
/* MONO_WASM_JIT_GUARDED_INLINE: at a virtual call site the profile can predict, emit an INLINED body
 * behind the same vtable guard the emitter's predicted arm already uses, instead of a call. See the
 * long argument at the site in method-to-ir.c.
 *
 * R118's "second bug" -- intermittent `memory access out of bounds` in a JITted __<>MHC stub as inlined
 * bodies grow, ~1 run in 2 -- has the SAME stack as the recurring OOB (R291), which was worker reuse
 * running code against a previous pthread's freed TLS (R293, R293j). Inlining only raised its rate. With
 * the reset in, it has not recurred: 20/20 + 6/6 runs of this knob with the other R292 levers (R299).
 * Worth -2.9% of the server tick alone (R290 add. 2) and part of R299's -8.3% together.
 *
 * Knob-off output must be BYTE-IDENTICAL, not merely close: tiershape.py at 0.00%. Do NOT compare
 * against an absolute noise floor here -- R241 measured the same-binary floor at 0.69% on the current
 * tier (158 of 22,816 methods), 4.6x the 0.15% this comment used to quote from a tier half the size.
 * Run a same-binary CONTROL pair alongside the A/B; an inherited constant is a coin flip, and this is
 * a gate-only change when off, so anything above 0.00% means the disjunct is firing when it should not. */
int mono_wasm_jit_guarded_inline = 1;
/* MONO_WASM_JIT_GUARDED_INLINE_SIZE: IL byte cap for a guarded inline candidate, SEPARATE from
 * INLINE_LENGTH_LIMIT (20) on purpose. R118 closed raising the GLOBAL limit on mechanism -- bodies
 * +3.5-4.4%, saturating by 60, and calls per method going UP 1.0%, because the caller absorbs the
 * callee's own call sites while the callee stays separately JITted -- but that measured the
 * NON-VIRTUAL remainder, which is the only population mono's inliner can reach. This cap governs
 * virtual sites, which that gate refuses outright, so R118's result does not bound it.
 *
 * SWEPT, AND 60 IS THE KNEE. Whole-run census at 20 / 60 / 100:
 *
 *     SIZE   admitted   emitted   size-refused   late-refused
 *      20      4,451     2,844        4,957         1,607
 *      60      6,661     3,464        2,633         3,197
 *     100      8,018     3,466        1,141         4,552
 *
 * EMITTED SATURATES AT ~3,465. Going 60 -> 100 admits 1,357 more candidates and inlines TWO more of
 * them; every other one becomes a late refusal, i.e. a vtable load, a compare and a branch emitted on a
 * hot dispatch path for nothing. So the cap is not what bounds this pass past 60 -- inline_method's own
 * willingness is -- and raising it further only buys waste.
 *
 * This also retires the size test that used to sit at the call site: mono_method_check_inlining applies
 * the global limit ITSELF, so a second cap after it could only ever make the limit smaller, and it
 * measured `size` refusals of exactly 0. The limit is now passed INTO the check.
 *
 * (V8's own wasm inlining cap is 500 WIRE bytes -- a different unit, a different pipeline stage, and
 * unreachable for us anyway while every call is an import.) */
int mono_wasm_jit_guarded_inline_size = 60;
/* MONO_WASM_JIT_ISLAND_NDATA's knob is deleted; the ABI parameter it gated is KEPT and now always
 * applies. It zeroes only the args+locals an EH method can actually address when pushing its il_state
 * island, instead of the full WJ_ISLAND_DATA (256) slots -- ~1040 bytes of memset per EH-method entry.
 * The emitter already computes the count for the `_nd > 256` eligibility bail, so passing it is free,
 * and the safety argument (readers index below ndata and NULL-check; island chunks are not GC roots)
 * is at the memset itself. It was measured at ~0.1 M instr/frame, roughly 0.05%% of the client render
 * thread -- REAL but indistinguishable from a global thermal shift on this box, which is why it kept
 * its knob and never got an answer. Bake the better value in and delete the branch: a lever the
 * instrument cannot resolve is not a lever, and keeping the worse arm alive costs a test in
 * mono_wasm_jit_enter_island forever. */
/* MONO_WASM_JIT_DEVIRT_ARM2=1: emit a SECOND guarded arm for the runner-up receiver.
 *
 * Sized from measurement, not taste (R205). Executed inline-IC hits split by their own site's devirt
 * outcome give, on the in-game window: alt-receiver at a site that DID devirt **32.5%**, no_rec 32.2%,
 * poly 30.7%, cold 4.6%, no_fslot 0.0%. So a third of hot IC traffic is the SECOND object form falling
 * through arm 1's guard, which no amount of devirt COVERAGE can reach -- only another arm can.
 *
 * BREAK-EVEN, from the emitted sequences (R206): an IC hit is ~21 x86 (2 loads, cmp, jne, unpack, then
 * call_indirect at ~15), a guard is ~3 (load, cmp, jne) and a co-located direct call is 1. An extra arm
 * pays its guard on ALL traffic reaching it and saves (ic - direct) on what it captures, so it wins at
 * capture > g/(ic-d) = 3/20 = ~15%. If the target does NOT co-locate the 'direct' call is itself a
 * call_indirect (~15) and the bar jumps to 3/6 = ~50%. **Co-location is therefore the precondition,
 * not a bonus.**
 *
 * The counter-evidence to respect: vcall_ways 4 -> 1 measured **+9.6% fps**, because those ways caught
 * ~1% of traffic -- far below this bar. Consistent with the model, and the reason ARM2_PCT exists
 * rather than emitting arm 2 unconditionally. */
/* MONO_WASM_JIT_PRED_PCT: predict the first arm on a FREQUENCY bar over per-identity counts instead of
 * the Boyer-Moore `margin == total` test. 0 = keep the margin bar.
 *
 * The margin bar demands a site be **100%% monomorphic** -- it rejects on `margin != total` -- while the
 * break-even measured off emitted code says an arm pays above **~15%% capture** when the target
 * co-locates (guard ~3 x86, direct call 1, IC hit ~21). The policy is therefore ~6.7x more conservative
 * than the economics, and it is IRREVERSIBLE: one differing observation disqualifies a site for the life
 * of the process, so a 99/1 site is treated exactly like a 50/50 one. That irreversibility is also why
 * re-emission trades `no_rec` for `poly` instead of for arms (R209).
 *
 * Margin's original justification was cost -- two words, no per-type table, cheap on the interp dispatch
 * path. That is now moot: wj_prof_note_identity already linear-scans ids[] on every observation, so the
 * per-identity counts R206 added cost one increment on a scan that was happening anyway.
 *
 * CLAUDE.md's "relaxing margin == total is DEAD" was measured at a >=90%% bar over SITE COUNTS (477
 * sites, 1.3%%). Both halves are wrong for this decision: R205 showed `poly` is 7.9%% of sites but 52.1%%
 * of cumulative IC EXECUTION (6.6x over-weighted), and >=90%% is six times above break-even.
 *
 * This also makes DELEGATE sites predictable, which margin structurally cannot: `margin` is left 0 for
 * them by design (their identity is del->method, not a receiver vtable), so frequency is the only
 * usable signal there -- and delegates are ~20%% of executed dispatch with no devirt applied at all. */
/* MONO_WASM_JIT_COLOCATE_HOPS is deleted: INERT AT EVERY VALUE (R211). It grew a co-location group
 * beyond the seed's IMMEDIATE callees by walking a frontier, on the theory that one-hop depsets were
 * what capped co-location at ~30%% arm-local. They are not -- R195 established the cap is that
 * co-location is a PARTITION, which no amount of multi-hop growth changes. The frontier walk stays
 * deleted; the partition problem is what per-class grouping addresses. */
/* MONO_WASM_JIT_DELEGATE_DEVIRT: minimum percent of a delegate site's observations the dominant target
 * must hold before it gets a guarded DIRECT arm. 0 = off.
 *
 * Delegates are ~23.5%% of executed dispatch and have never had devirt applied, for two reasons that are
 * both artefacts rather than obstacles: `prof_predict` requires a resolved `target` and the delegate
 * recorder passes NULL (the identity IS the target for a delegate site, per the WJ_SITE_DELEGATE comment),
 * and `margin` is left 0 by design because a majority vote over receiver vtables is meaningless when the
 * identity is del->method. Per-identity COUNTS (R206) are the signal that works here, and the break-even
 * is the same as any other arm: guard ~3 x86 against an IC hit at ~21, so it pays above ~15%% capture when
 * the target co-locates. */
int mono_wasm_jit_delegate_devirt = 15;   /* SHIPPED DEFAULT. Tuned on Minecraft 1.16.1 + Fabric + Sodium/Lithium under IKVM, 2026-08/09, plateau protocol. A number without a workload is not a result -- this one has one, and it is not necessarily right for anything else. */
/* minimum %% of a delegate site's observations its dominant target must hold before it gets a guarded direct arm. MEASURED: -6.6%% of client-thread instructions/frame at n=2 with a flat negative control (__<>MHC stubs -12.8%%, InstanceCheck -22.7%%). thin=35 of 7,556 sites, so delegate sites are overwhelmingly SINGLE-TARGET and the bar is not worth sweeping. */
int mono_wasm_jit_pred_pct = 0;
/* Minimum share of a site's observations the runner-up must hold, in percent. Default is the measured
 * co-located break-even; raise toward 50 if arm-2 targets are not co-locating. */
int mono_wasm_jit_devirt_arm2_pct = 15;
/* MONO_WASM_JIT_STABLE_IC_IDS is deleted. It reused the call profile's record id as the inline-cache
 * site id, and the two are DIFFERENT GRANULARITIES: a profile record is keyed by callee base method
 * (IL offsets are stale after generate_compacted_code), while an IC belongs to one CALL SITE. Two sites
 * in one method calling the same base would share a PIC slot -- at vcall_ways 1, a mutual eviction
 * whenever they see different receivers -- and 6,345 emissions per boot would take an existing id, so
 * it was neither a rounding error nor a refactor. It shipped 0 and is now gone. The trap is worth
 * remembering on its own: A STABLE INLINE-CACHE ID IS NOT THE SAME GRANULARITY AS A PROFILE RECORD. */
/* MONO_WASM_JIT_DIRECT_IMPORT: let a call to another wasm-JITted method be reached through a declared
 * function IMPORT rather than `i32.const <fslot>; call_indirect <ct> 0`.
 *
 * V8 does not fold a constant call_indirect index into a direct call: it emits a table bounds check, a
 * canonical-type check, index->code-pointer arithmetic, a validity check and `call *`, ~15 x86
 * instructions. An imported call lowers via BuildImportedFunctionTargetAndImplicitArg to three loads from
 * WasmDispatchTableForImports plus an indirect `call *`, ~5 (turboshaft-graph-interface.cc:2697-2710,
 * turboshaft-graph-interface-inl.h:62-100). It is NOT a direct `call rel32` and V8 will never inline
 * through it -- only a module-LOCAL callee gets that, which is what co-location is for. This removes two
 * checks and some index arithmetic, nothing more.
 *
 * MEASURED on the product build (mixed-AOT), 2026-08-27, name-paired within-binary tier A/B over 12,073
 * common modules (scratchpad/wj/devirtcheck.py): method-target constant-index call_indirect 52,732 ->
 * 31,794, method-target imports 0 -> 14,409. **39.7% conversion, 20,938 sites.** On the non-AOT build the
 * same knob converted 52.4% / 35,043; both figures fall on the product build because AOT'd corlib and IKVM
 * callees never reach the JIT, so their call sites have no f-slot to import.
 *
 * WHY IT NEEDS AN SCC-CLOSED MODULE, and why that is a correctness condition rather than tuning: an import
 * binds wasmTable.get(fslot) at INSTANTIATION. The ordering that makes that safe -- a callee is admitted in
 * every worker before its caller is -- holds only for a DAG. On a CYCLE the admission DFS has to break
 * somewhere, and whichever member loses instantiates while its partner's slot still holds the guarded
 * interp-entry trampoline: a real function of a different type, hence "imported function does not match the
 * expected type" rather than "is not a function". Round 139 measured that as 14 LinkErrors over 9 slots and
 * a crash before titleReady, 2/2 arms, with the failing imports being adjacent (e,f) pairs -- co-registered
 * members of one island. So the assembler only offers this form when it has been told its member set is
 * SCC-closed (WjAsmPolicy.method_imports); a lone method never claims that.
 *
 * A failed import fails INSTANTIATION, not the call: it shows up as fewer `registered` plus a WJC_INVALID
 * bump and a silent fall back to the interpreter, which looks exactly like a performance result. Assert
 * `registered` is unchanged before reading any timing from this knob. */
/* MONO_WASM_JIT_CI_IMPORTS is deleted. It extended DIRECT_IMPORT's import conversion to the
 * constant-index call_indirect sites (helper calls), and it never had a reason to ship: R158 measured
 * that `call <import>` is NOT a cheap direct call. V8 lowers an imported direct call through
 * BuildImportedFunctionTargetAndImplicitArg into the SAME BuildWasmCall(kWasmIndirectFunction) path
 * call_indirect takes, emitting the WasmCodePointerTable conversion and an indirect branch -- six
 * instructions ending in `call *`. What it saves over call_indirect is the table bounds check, the
 * canonical-type check and the runtime-index loads; THE INDIRECT BRANCH, WHICH IS THE EXPENSIVE PART,
 * SURVIVES. Only a module-local `call <funcidx>` becomes a real `call rel32`, and it is also the only
 * form V8 can inline through. That is why DIRECT_IMPORT converted 100%% of predicted arms (6,892/6,909)
 * and moved nothing measurable, and it is the argument for CO-LOCATION over import conversion. */
/* MONO_WASM_JIT_ESLOT_VERIFY is deleted. Before each interp->JIT entry it read the table slot back from
 * JS and checked it was thunk-shaped (arity 2, not the jiterpreter prefill's 4) -- an EM_ASM per invoke.
 * It was the bring-up instrument for the prefilled-placeholder trap, and that trap now has a real guard:
 * mono_wasm_jit_slot_live, the per-thread bitmap, checked at the entry gate and at every f-slot bake. */
/* SHADOW COPIES ARE DELETED -- five knobs (SHADOW, SHADOW_MAX, SHADOW_BYTES, SHADOW_MODBYTES,
 * SHADOW_NONLEAF), wj_shadow_candidate, wj_collect_shadows, wj_verify_module_exports, the ranking
 * pass, the per-module byte budget and eleven counters.
 *
 * The mechanism WORKED and is well understood. A shadow is a private, unexported DUPLICATE of a
 * callee's body placed in the caller's module: the callee keeps its own standalone module and slots,
 * every caller may hold its own copy, and wj_asm_member_of then resolves the call to `call <funcidx>`
 * -- one `call rel32` instead of a ~15-instruction call_indirect, and the only call form V8 will
 * inline through. It is also the ONLY mechanism that bypasses a partition, which is why it beat
 * co-location by 21 points where doubling COLOCATE_MAX was worth 1.5 (R195/R200: co-location is a
 * partition, so a hot callee shared by five callers co-locates with exactly one of them).
 *
 * It goes on COST against an unmeasurable benefit.
 *
 * THE PRICE, matched pair, one binary, one session (tier-p1ctl vs tier-p1sh):
 *
 *      shipped, no shadows   27,011 modules   56.69 MB   76,528 functions   2.83 funcs/module
 *      shadow=1 nonleaf=1    27,508 modules   73.96 MB  124,031 functions   4.51
 *                              +1.8%           +30.5%      +62.1%
 *
 * Module count FLAT while bytes and functions explode -- which is the check R214 demands, and it says
 * this is DUPLICATION rather than more methods compiled, so the bytes are pure cost. +17.3 MB of wasm
 * for V8 to compile and hold, on a tier already at 3.15x native L1i misses and 4.96x native iTLB
 * misses per million instructions.
 *
 * THE BENEFIT WAS NEVER TIMED, in ~35 rounds. R200 says so outright ("No timing. Every number here is
 * a static call-form census"); R202 repeats it. Every shadow result is vcallreach.py counting call
 * FORMS. The timing measurement has now been taken -- a matched perf arm on the plateau protocol --
 * and it cannot be separated from a monotonic global drift at n=1: every symbol including BOTH
 * negative controls and the client-thread total fell in run order, and the trailing control came back
 * ABOVE the leading one. A mechanism whose effect is below that floor is not worth +30.5% bytes.
 *
 * And the pool it targets is bounded four independent ways: branch mispredicts are 0.59x native's;
 * doubling module-local direct calls moved the prologue band 0.28 points; converting EVERY call to
 * direct is worth ~13% of frame (R171) and only 39.2% of dispatch can ever be direct (R203); and the
 * one shadow-family arm that WAS timed -- co-location's merge -- read parity over 12 runs in both
 * orders (R194).
 *
 * wasm_module_assemble's `nexport` parameter and enctest t4 are KEPT deliberately. `nexport` is two
 * lines in the encoder, t4 is the only gate that reaches `nexport < nmembers`, and per-class module
 * grouping (the strongest remaining architectural lead) wants exactly that shape for cold siblings.
 * Deleting a tested encoder capability to save a parameter, then re-deriving the layout and rewriting
 * the gate, is the churn "a gate that cannot reach the case a change introduces is not evidence"
 * exists to prevent. Nothing in the emitter produces that layout today. */
/* RE-EMISSION IS DELETED -- its four knobs (REEMIT, REEMIT_IC, REEMIT_AFTER, REEMIT_AGE), its
 * `in_reemit` census flag, its ring, drain and pin lived here and in interp.c. See the long note where
 * the ring used to be (interp.c) for the control-bracketed arm that closed it and for what it costs:
 * `no_rec` (13,389 sites, 32.2% of hot IC execution) now has no collector, and guarded CHA is the
 * candidate to become one. */

/* MONO_WASM_JIT_COLOCATE_MERGE: let a re-frame ABSORB an existing group instead of dropping the edge.
 * DEFAULT OFF.
 *
 * The `if (re->batch) return 0;` in mono_wasm_jit_rebatch is about SPLITTING -- a group's members share
 * one WebAssembly.Instance and cannot be instantiated apart, so re-framing one member out of a group
 * strands the rest. Merging splits nothing, and conflating the two is what made the partition
 * append-only: each method joined at most one group, once, and the decision was never revised.
 *
 * R192 measured that as the ONLY constraint that binds. Of 473 per-callee refusals on jbox2d, 473 were
 * `dre->batch`; the byte cap refused 0 and the member cap fired once in 389 attempts, and the AOT wall
 * (`unreg`) did not appear at all. Separately 41% of methods (160/389) publish with an empty depset and
 * never enter the loop -- that is the trigger-timing blocker, which merging does NOT address.
 *
 * The gate this exists to settle: every devirt predicted arm registers its target as a direct dep
 * (wj_result_add_direct_dep, see the arm), so a predicted target IS a co-location candidate today, yet
 * a fresh tier dump shows 8,807 of 10,314 arms still `call_indirect` against 1,507 already local. If
 * the partition is what holds them there, merging moves arms out of that 8,807 and `CallLocal` rises
 * with it. If it does not, the binding constraint is the publish-time trigger and merging is worthless
 * on its own -- two answers pointing at different work, which is why this is measurable alone.
 *
 * OFF by default until that is measured. R166 is the standing warning: a co-location bug left
 * admit_live at 0 for a worker's lifetime and read as a 1.63x regression with every error counter at
 * zero, so the vfb and admit_live identities must be checked before any timing is quoted. */
/* MONO_WASM_JIT_RELINK_JITTED: let an IKVM generation-2 body swap proceed even when generation 1 has
 * already been wasm-JITted. The refusal it lifts costs 736 methods per in-game window, i.e. methods that
 * keep the dynamic-dispatch body forever.
 *
 * IT NOW REPUBLISHES THE EXISTING PAIR, which is the opposite of what R252 shipped and is a deliberate
 * change of direction rather than a regression. R252 gave generation 2 a FRESH e/f pair and left
 * generation 1 live, precisely to avoid re-pointing a live f-slot; the cost is that every caller which
 * baked generation 1's f-slot, and every group that co-located its body, keeps reaching the slow body
 * forever. Same-slot replacement is what a detour (MonoMod/Everest-style) requires and what reaching
 * generation 2 from existing callers requires, so the machinery it needs -- asynchronous publication,
 * the per-worker action word, the adoption drain -- is now the thing being built rather than avoided.
 * mono_wasm_jit_request_reemit (interp.c) is the entry point; mini-wasm-publish.inc is the mechanism.
 *
 * SHIPS 1, on the measurement of the OLD shape. 2026-09-12, server tick, instruction-weighted, BOTH
 * ORDERS, n=2 per arm, on top of IKVM_LAZY_SIG=1: 178.1/179.0 -> 171.2/169.7 M instr/tick,
 * non-overlapping; matched pair (both arms completed all 2,406 nominal ticks) 430.7 -> 408.3 G
 * instructions, -5.2%. IKVM's refusal counter fell 619/619 -> 37/38, installs rose 3,250 -> 3,750.
 * WJC_RELINK_REFRESHED was 574 per run of which only 4 were in-game, the rest during world load -- so
 * that -5.2% says nothing about whether same-slot republication is worth its machinery IN THE PLATEAU,
 * and it must be re-measured on this shape before the two are compared. */
int mono_wasm_jit_relink_jitted = 1;
int mono_wasm_jit_colocate_merge = 0;
int mono_wasm_jit_colocate_max = 16;      /* MONO_WASM_JIT_COLOCATE_MAX: members per group, self included */
/* MONO_WASM_JIT_COLOCATE_BYTES: total member wire bytes. Not a V8 inlining limit -- kMaxInlinedCount (60)
 * bounds inlining INTO one function and must not be reused as a per-module cap -- just a bound on how much
 * one publish re-serialises. */
/* MONO_WASM_JIT_RENDEZVOUS_TEST=N: THE POSITIVE CONTROL for the republication rendezvous, and nothing
 * else. Every N-th successful compile-publish, republish that very descriptor through
 * mono_wasm_jit_rendezvous with its OWN, BYTE-IDENTICAL bytes.
 *
 * Byte-identical is the entire point. The rendezvous is then a provable NO-OP semantically -- every worker
 * re-instantiates the same module into the same slots and ends where it started -- so any fault, wedge or
 * frame-rate change under it is the MECHANISM misbehaving and cannot be a miscompiled replacement body.
 * That separation is what makes this worth a knob: it exercises publication, the epoch, the log, the
 * per-thread drain and the re-admission path at whatever rate is asked for, with the one variable that
 * could confuse the result held fixed.
 *
 * SHIPS 0. A run with it on is a mechanism test and its timings include the requested re-instantiations. */
/* MONO_WASM_JIT_REEMIT: 1 = on, 0 = off. Re-emit a JITted method once ANY ONE of its inline-cache sites
 * has missed MONO_WASM_JIT_REEMIT_MISSES times, republishing onto the SAME e/f pair through the rendezvous
 * so its callers execute the new body.
 *
 * A FLAG, NOT A COUNT. An earlier draft of this comment read "=N ... once N of its sites have missed",
 * which the code never implemented -- the trigger tests `> 0` and fires on the first site to cross the
 * threshold. The threshold itself is MONO_WASM_JIT_REEMIT_MISSES; that is the tuning knob.
 *
 * SHIPS 0 until it is measured. The old re-emission subsystem shipped 0 through seven implementation bugs
 * and was deleted; what is different now is the rendezvous (nothing else could republish a live slot) and
 * the trigger (the IC miss path, which R179 named as the right observation point after measuring that the
 * interp->JIT boundary was the wrong one at vicMiss 1.00x). Neither of those makes it correct by itself. */
int mono_wasm_jit_reemit = 0;
/* Set only while a re-emission's force-compile is running, so the devirt census can tell a re-emitted body
 * apart from a first emission. A plain global is sound because compiles are serialised by the wj_compiling
 * CAS -- exactly one is in flight process-wide. */
int mono_wasm_jit_reemit_inflight = 0;
/* THE DEVIRT CENSUS, SCOPED TO RE-EMITTED BODIES. This is the question re-emission exists to answer and
 * the run-wide census cannot: R179 measured 46.3% devirt coverage on re-emitted bodies against 29.8%
 * run-wide, which is the whole claim -- and then could not turn it into a run-wide gain because its
 * candidates were the wrong population. Without this split, a re-emission arm can only report that it
 * re-emitted N methods, not whether their code came out any better. Ungated like REEMIT_SITE: it must be
 * readable in the same run that produces the compiles. */
#define WJ_REEMIT_SCOPED(c) do { if (G_UNLIKELY (mono_wasm_jit_reemit_inflight)) mono_wasm_jit_counters [c]++; } while (0)
/* Misses at ONE site before it contributes a re-emission trigger. High enough that a site must be genuinely
 * hot rather than merely warm; the site then never triggers again (WjVcallSite.reemit_noted). */
int mono_wasm_jit_reemit_misses = 64;
/* MONO_WASM_JIT_REEMIT_BATCH: descriptors published per record, and MONO_WASM_JIT_REEMIT_INTERVAL_MS
 * the floor between optional profile-driven batches. Batching prevents a burst of matured sites from
 * producing one log record and one instantiation pass per method during class loading. Mandatory semantic
 * replacements bypass the optional rate limit but still use the same queue and publication chokepoint.
 * Clamped to [1, 64] -- 64 is
 * WJ_REEMIT_BATCH_MAX in interp.c and overrunning it would be a stack write past the array. */
int mono_wasm_jit_reemit_batch = 64;       /* 64 / 250 ms: the tier-2 broker's rate, shipped with tier 2 (R348) */
int mono_wasm_jit_reemit_interval = 250;
/* MONO_WASM_JIT_REEMIT_MAX: re-emissions per process, a ceiling the rate limit cannot be talked out of. */
int mono_wasm_jit_reemit_max = 200;
int mono_wasm_jit_rendezvous_test = 0;
int mono_wasm_jit_colocate_bytes = 32768;
/* TIGHT DEPS ARE UNCONDITIONAL. mono_wasm_jit_batch_bind always republishes the dependency set the
 * ASSEMBLER derived for the module a re-framing actually installs, instead of leaving whatever set the
 * members were compiled as. Not doing it is a measured pathology rather than a conservative choice: a
 * rebatch has no capture phase, so the old code skipped the republish and every co-located member went on
 * demanding admission of callees it now reaches with `call <funcidx>` -- BATCH_ADMIT_FAIL 1966 and
 * ADMIT_DEFER_GIVEUP 1540 against 3 in the control, and a give-up is permanent, so ~1540 methods lost the
 * tier for the run.
 *
 * THE KNOB IS GONE BECAUSE IT NEVER EXISTED. `mono_wasm_jit_colocate_tight_deps` was declared, parsed from
 * MONO_WASM_JIT_COLOCATE_TIGHT_DEPS, printed in the colocate stats line -- and NEVER BRANCHED ON. The
 * republish ran unconditionally the whole time, so `=0` changed nothing while the printf reported
 * `TIGHT_DEPS=0`, which is worse than no knob at all: an arm run with it reads as a control and is not one.
 * This is R245's class exactly (MONO_WASM_JIT_COLOCATE_DEPS, MONO_WASM_JIT_SCC_COLOCATE), and it cost a
 * dep-graph dump on 2026-09-16 that was taken specifically to get the UNTRIMMED graph and did not.
 *
 * It matters for reading WJ_GRAPH: because the republish always runs, an intra-group edge is DELETED from
 * the depset when co-location converts it to a local call. So a `d` edge in the dump is one co-location
 * FAILED to capture, and the captured count is WJC_TIGHT_DEPS_DROPPED. Anyone measuring capture off the
 * dump alone gets the sign backwards -- see scratchpad/wj/partreach.py, which now reports both. */
/* MONO_WASM_JIT_DUMP_DEP_GRAPH: one-shot dump of the registry dependency graph for offline cycle analysis
 * (scratchpad/wj/depcycles.py). Defined HERE, with the other knobs, rather than beside the dumper it gates:
 * the dumper lives inside #ifdef HOST_BROWSER but mono_wasm_jit_dump_stats does not, so a definition down
 * there links in the browser build and leaves mono-aot-cross with an undefined symbol. csyn.sh compiles
 * both databases but does not LINK either, so it cannot catch that. */
int mono_wasm_jit_dump_dep_graph_knob = 0;
/* MONO_WASM_JIT_VERIFY_DEPS: probe every dep f-slot for the jiterpreter placeholder just before a
 * descriptor goes live. Diagnostic only; see the block in mono_wasm_jit_admit. */
int mono_wasm_jit_verify_deps = 0;
/* MONO_WASM_JIT_SINGLE_WRITER: route admission through wj_make_callable, the one function permitted to
 * install or publish. Ships 0 until validated; see the block above wj_make_callable. */
int mono_wasm_jit_single_writer = 0;
/* MONO_WASM_JIT_SWEEP: re-verify already-published descriptors every N admissions. 0 = off. */
int mono_wasm_jit_sweep = 0;
/* MONO_WASM_JIT_DEVIRT_IMPORT is GONE, and deliberately not replaced.
 *
 * It used to import the target of a predicted-devirt HIT specifically, as a separate lever from the
 * ordinary direct call, and measured 2.6% of the targeted sites -- 1,732 conversions, ~0.34% of all call
 * sites, an order of magnitude under the ~5% measurement floor. The separation only existed because the two
 * sites emitted different code. They no longer do: a guarded devirt hit leaves the same WASM_RELOC_CALL as
 * any other direct call, so DIRECT_IMPORT covers both and there is nothing left for a second knob to gate. */
/* MONO_WASM_JIT_BATCH_INLINE: select batch members for V8 INLINE ELIGIBILITY rather than for raw call-graph
 * connectivity. Default 1; inert unless MONO_WASM_JIT_BATCH_MODULE is on.
 *
 * Co-location has exactly one remaining justification. Everything else was measured away: the per-module
 * engine tax is ~0.4 ms (about 4-5% of boot+world CPU in total, and under 1% in-game, where registration has
 * finished before the window opens), per-method memory does not improve with batching, and the VMA-count
 * hazard does not exist on this hardware. What is left is letting V8 inline across what are otherwise module
 * boundaries -- one method per module means V8's inlining candidates are always empty.
 *
 * For a call A -> B to actually be inlined, five things must hold, and the planner only controls two:
 *   1. A and B in one module                      <- the planner
 *   2. the call emitted direct, not call_indirect <- follows from 1
 *   3. A itself reaches TurboFan                  <- per-FUNCTION tiering budget, ~37k returns for a ~300 B body
 *   4. B's body <= 500 wire bytes                 <- v8_flags.wasm_inlining_max_size
 *   5. it wins V8's ranking, count/wire_byte_size <- not queryable
 *
 * The old planner tested member count and total bytes, i.e. none of 3, 4 or 5. This encodes 4, via the
 * body_len size predictor (module <= 932 B predicts body <= 500 B at 97.7% accuracy, calibrated over a
 * 23,330-module tier dump where body ~= 0.987 * module - 419).
 *
 * MEASURED AS A REGRESSION, TWICE, AND THEREFORE DEFAULT OFF. Whole-tier dumps, batch_module=1, counting
 * intra-module `call` sites whose callee body is under the cap -- the only number that matters here:
 *
 *   planner                                    batched mods   methods batched   intra-mod calls   inlinable
 *   old (connectivity only)                            184     3,335 (14.0%)            15,150       6,126
 *   v1: either endpoint small, + a 60-member cap       196     ...                      10,950       3,919
 *   v2: directional, callee-only, no cap               201     1,361 ( 5.7%)             8,235       2,344
 *   old with caps raised to the maximum                184     3,183 (13.3%)            15,187       5,847
 *
 * Three things that reading tells you and measuring does not:
 *
 *   - The gate is at the WRONG LEVEL. It prunes EDGES in a spanning merge, and pruning an edge does not
 *     remove an oversized member -- it just stops a merge, so components stay small. Inlinable-call volume
 *     tracks component SIZE, so a better-targeted but smaller batch loses. Eligibility belongs in the
 *     MEMBERSHIP decision, not in the merge.
 *   - batch_max and batch_bytes are NOT BINDING. Raising them to the maximum the code allows moved nothing
 *     (184 -> 184 modules, 15,150 -> 15,187 calls). Component size is set by call-graph topology and by the
 *     once-at-a-plateau planning schedule, not by the caps.
 *   - So selection quality is not the limiter; REACH is. The planner only ever batches ~14% of the tier,
 *     because it plans once at an early plateau and most methods are registered after that. 6,126 inlinable
 *     calls against 332,027 call_indirect is ~1.8% of the call population, and that is the ceiling of the
 *     current architecture regardless of how members are chosen.
 *
 * Kept as a knob rather than deleted because `pair_ok` is the right primitive for a membership-level gate,
 * which is where this idea would have to move to be worth anything. Condition 3 is still not encoded at all:
 * it needs a POST-JIT per-method execution count, and wasm_jit_hits stops at the JIT threshold and is then
 * reset, so it cannot see the ~37k returns that decide tier-up. */

int mono_wasm_jit_lazy_gcp = 1;
/* MONO_WASM_JIT_LAZY_COLD: a method whose only GC points are RAISES and conditional SAFEPOINT POLLS gets a
 * lazy ref frame that is materialised only inside those rare taken arms -- no `s.p` read, no zeroing, no
 * ref-slot mirrors and no exit restore on the hot path. Such a frame can never be live at a ref def, a
 * kill point or an exit: a raise never returns, and a poll RELEASEs the frame before falling through.
 * The existing LAZY_GCP gate cannot reach these methods because every null/bounds check counts as a GC
 * point, and the one sweep that removed its limit (the LAZY_GCP note at the emitter's lazy-frame gate)
 * put an ENSURE in front of every HOT GC point. This counts only the GC points that are neither, and emits
 * no hot-path ENSURE.
 * Target: the getters/field binders that spend ~72% of their time in prologue/epilogue (R290).
 * Level 2 also discounts GC points in blocks that cannot reach the exit -- an IKVM throw path allocates and
 * constructs its exception before raising -- running the frame as an ordinary lazy one inside them
 * (lazy_doomed in the emitter, R294). Level 1 is what R292/R299 timed; level 2 reaches ~100 more methods
 * (R294d) and has never been timed on the tick. */
int mono_wasm_jit_lazy_cold = 1;
/* MONO_WASM_JIT_VCALL_MEMO: the per-thread miss memo in mono_wasm_jit_vcall_resolve_fslot (interp.c), which
 * carries the design and its invariants. resolve_fslot self -41% in R292; timed with the other levers (R299). */
int mono_wasm_jit_vcall_memo = 1;
/* MONO_WASM_JIT_IC_MID: on a way-zero miss in the emitted inline IC, test whether the receiver's class holds
 * the SAME method in the call's vtable slot as the entry's cached target, and reuse the entry's f-slot if so
 * instead of entering the resolver. Class-virtual, non-generic sites only. See R311. */
int mono_wasm_jit_ic_mid = 1;
/* MONO_WASM_JIT_PRED_MID: METHOD-identity guards on the devirt arm and on guarded inlining, and predicting a
 * site whose recorded receivers all resolve to one method. Class-virtual, non-generic sites only. See R311. */
int mono_wasm_jit_pred_mid = 1;
/* MONO_WASM_JIT_PROF_INLINEE: when the caller's profile cannot predict a GI site that came from an inlined
 * callee, ask the callee's own profile -- the interpreter recorded that call under the callee. See R311. */
int mono_wasm_jit_prof_inlinee = 1;
/* MONO_WASM_JIT_PROF_BLOCKS: how many WJ_PROF_MAX_SITES-site profile blocks one caller may chain; 1 is the
 * old fixed cap. See R311. */
int mono_wasm_jit_prof_blocks = 4;
/* MONO_WASM_JIT_FORWARD_RETIRED: caches holding an InterpMethod that an IKVM body swap retired follow its
 * replaced_by link to the current generation (interp_imethod_current). Off, a vcall resolve-cache entry cached
 * on a never-compiled generation 1 reads f-slot 0 forever and crosses into the interpreter on every call (R314:
 * 6.37 M such crossings per run -> 22; -3.6% server instructions/tick, ON/OFF/OFF/ON), and a delegate recipe on
 * a retired imethod runs generation-1 IL. The [wasm-jit retired] counters are the mechanism check. */
int mono_wasm_jit_forward_retired = 1;
/* MONO_WASM_JIT_LEAN_TRY_INVOKE: the interpreter's call hook (WASM_JIT_TRY_INVOKE) calls wasm_jit_maybe_compile only
 * when the callee is still compile-eligible or JIT work is pending, instead of on every interpreted call. */
int mono_wasm_jit_lean_try_invoke = 1;
/* MONO_WASM_JIT_IC_REMAT: IC sites read the PIC pointer/capacity imports at each use instead of four prologue locals
 * live across the body (EMIT_IC_VPIC_PTR in the emitter). */
int mono_wasm_jit_ic_remat = 1;
/* MONO_WASM_JIT_INLINE_CALLS (R315, plan Phase 3): inlinees may keep non-inlined calls and ctor calls, under the
 * per-compile policy below. A DIAGNOSTIC for the whole tier; it is meant to ship only as tier-2 policy (in tier 1
 * every call an inlinee keeps becomes a root direct callee that islands pull in at one hit). */
int mono_wasm_jit_inline_calls = 0;
int mono_wasm_jit_inline_calls_limit = 35;   /* MONO_WASM_JIT_INLINE_CALLS_LIMIT: IL bytes */
int mono_wasm_jit_inline_calls_cost = 120;   /* MONO_WASM_JIT_INLINE_CALLS_COST: replaces inline_method's 60 */
int mono_wasm_jit_inline_calls_depth = 6;    /* MONO_WASM_JIT_INLINE_CALLS_DEPTH: replaces the depth cap of 10 */
/* MONO_WASM_JIT_T2 (R316, plan Phase 4): sampled tier-2 recompiles of the hot set through the re-emission broker.
 * On by default together with the bundle it was measured in -- LEAN_TRY_INVOKE, IC_REMAT, REEMIT_BATCH/INTERVAL,
 * INLINE_COLD_THROW, PROF_ORIGIN, FAST_TLS, LDADDR_REF, T2_STATIC_PRED=2, all set at their initialisers: Minecraft
 * server tick, with branch hints level 2, -8.6% P-core cycles and -23.6% calls against tier 2 off (R348, p70,
 * b s o p p o s b). Needs MONO_WASM_JIT_T2_MAX_BODY (R349): without it tier 2 can hand TurboFan a ~320 KB body. */
int mono_wasm_jit_t2 = 1;
int mono_wasm_jit_t2_sample_ms = 2;      /* MONO_WASM_JIT_T2_SAMPLE_MS: sampling timer period */
int mono_wasm_jit_t2_threshold = 48;     /* MONO_WASM_JIT_T2_THRESHOLD: samples before a method is queued */
int mono_wasm_jit_t2_max = 2000;         /* MONO_WASM_JIT_T2_MAX: tier-2 requests per process */
/* MONO_WASM_JIT_T2_LIMIT / _COST: tier 2's inline IL-size limit and cost cap. 60 / 400 against the old 35 / 200 on
 * the Minecraft server tick (R359/R360, p82 b t t b, one binary, equal ticks): cycles -2.1% (ranges clear), instructions
 * -4.6%, calls -12%; peak VmData +113 MiB, inside the control's own spread. Needs R360's atomics lowering, without which
 * the deeper bodies downgraded 42 roots to tier 1. */
int mono_wasm_jit_t2_limit = 60;
int mono_wasm_jit_t2_cost = 400;
int mono_wasm_jit_t2_depth = 9;          /* MONO_WASM_JIT_T2_DEPTH: inline depth cap at tier 2 */
int mono_wasm_jit_t2_gi_size = 120;      /* MONO_WASM_JIT_T2_GI_SIZE: guarded-inline size cap at tier 2 */
/* MONO_WASM_JIT_PROF_ORIGIN (R316b, plan Phase 3.5): the emitter's devirt / delegate / IC-width reads at a call site
 * that came from an inlinee ask the INLINEE's record first, then the compiled method's. Affects only compiles under
 * the per-compile inline policy (tier 2, INLINE_CALLS); a tier-1 inlinee has no call sites to read for. */
int mono_wasm_jit_prof_origin = 1;
/* MONO_WASM_JIT_FAST_TLS (R317, plan M3): mono_tls_get_thread_extern emitted as two loads through `s.b`. */
int mono_wasm_jit_fast_tls = 1;
/* MONO_WASM_JIT_INLINE_COLD_THROW (R318, plan Phase 3.4): throw-terminated IL segments are not counted toward the inline
 * size limit, under the per-compile policy only (tier 2, INLINE_CALLS). */
int mono_wasm_jit_inline_cold_throw = 1;
/* MONO_WASM_JIT_INLINE_BFI (R319): inline a method of a BeforeFieldInit class whose cctor has not run, without running it.
 * BeforeFieldInit only requires the cctor before a static FIELD access, and such an access inside the inlinee is still
 * guarded (method-to-ir's ldsfld path: a runtime init check, or INLINE_FAILURE "class init"). */
int mono_wasm_jit_inline_bfi = 0;
/* MONO_WASM_JIT_PROF_SHARE (R316j): tier-up and IKVM's body swap SHARE the call profile with the new generation, allocating
 * it at that moment if the method has recorded nothing yet (interp/tiering.c). */
int mono_wasm_jit_prof_share = 0;
/* MONO_WASM_JIT_LDADDR_REF (R321): OP_LDADDR of a ref/byref scalar local, homed in its ref-shadow slot (addrslot -2). */
int mono_wasm_jit_ldaddr_ref = 1;
/* MONO_WASM_JIT_REEMIT_VALIDATE (R322): a re-emit into a slot the compiling thread installed is validated there, not
 * published there (0 = the old install-on-validate, the positive control for the trap it removes). */
int mono_wasm_jit_reemit_validate = 1;
/* MONO_WASM_JIT_T2_STATIC_PRED (R329): a tier-2 GI site with no profile record predicts the callvirt's own method behind
 * the method-identity guard; 2 (R333) also on a COLD verdict (a hot monomorphic IC site records one observation). */
int mono_wasm_jit_t2_static_pred = 2;
/* MONO_WASM_JIT_T2_REARM (R332): how many times a tier-2 request released on BUSY give-up is re-armed (0 = retire it). */
int mono_wasm_jit_t2_rearm = 3;
/* MONO_WASM_JIT_T2_SAMPLE_LOOP: 0 = every poll credits its own method; 1 (R335) = loop polls only, an entry poll
 * defers a pending sample to the next back-edge (R348: +10% server cycles); 2 (R355) = both -- an entry poll credits
 * its method and the next back-edge takes a second sample for its own. Mode 2, p78 b t t b on the Minecraft server
 * tick: tier-2 requests +54%, cycles -3.2% (ranges just clear), instructions -2.2% (overlapping). */
int mono_wasm_jit_t2_sample_loop = 2;
/* MONO_WASM_JIT_T2_COLOCATE (R338): 1 keeps automatic co-location running while tier 2 is on (0 = off under tier 2). */
int mono_wasm_jit_t2_colocate = 0;
/* MONO_WASM_JIT_BRANCH_HINTS (R339): emit a metadata.code.branch_hint section for the branches whose direction the
 * emitter knows (poll arms, throws, threw-checks unlikely; the TLAB fast path likely). 2 (R341) adds every IR branch
 * into an out_of_line block and the vcall IC / devirt-guard miss exits. Layout only: V8 moves the unlikely successor
 * out of line, no instruction is added. Minecraft server tick: 1 vs 0 -2.9% cycles/instruction (R339), 2 vs 1 -7.4%
 * P-core cycles (R341, p69, b t t b); soaked in p70 (15 runs, no fault). 0 = no hint section. */
int mono_wasm_jit_branch_hints = 2;
/* MONO_WASM_JIT_MATH_INTRINS (R346): lower System.Math/MathF Sqrt, Floor, Ceiling to the wasm opcode in the JIT
 * (mono_arch_emit_inst_for_method). 0 leaves them as InternalCall calls, which JIT code reaches only through the
 * interpreter: p71 measured that route at 3.9-4.0% of the Minecraft server tick with the knob off and 1.9-2.2% with
 * it on (b t t b), the Math pinvoke path at 0. */
int mono_wasm_jit_math_intrins = 1;
/* MONO_WASM_JIT_ATOMIC_I8 (J1c, the no-AOT plan): intrinsics.c turns 64-bit Interlocked.Read/Increment/Decrement/Add/
 * Exchange/CompareExchange and Volatile.Read/Write into OP_ATOMIC_*_I8 for a wasm-JIT compile, and the emitter lowers
 * them to i64 wasm atomics. 0 leaves them as calls: the icall, which JIT code reaches through the interpreter unless an
 * AOT'd wrapper exists (R326/R328 priced the AOT'd CAS_long / VolatileRead_long at 0.87 + 0.69 M instr per server tick). */
int mono_wasm_jit_atomic_i8 = 0;
/* MONO_WASM_JIT_TRACE_COMPILE=1 (R376, diagnostic): print "[wasm-jit] compile-begin <ns>.<class>:<method>" as each
 * wasm-JIT compile starts (mono_wasm_force_compile). */
int mono_wasm_jit_trace_compile = 0;
/* MONO_WASM_JIT_T2_MAX_BODY (R349): largest tier-2 module, in bytes, the emitter will hand to V8; a bigger one fails
 * permanently and the method keeps its tier-1 body (or takes the tier-1-policy downgrade). 0 = no cap. */
int mono_wasm_jit_t2_max_body = 98304;
/* MONO_WASM_JIT_EH_REC (R353): an EH method keeps its IL_STATE LMFExt in a record inside its OWN C-stack frame and
 * links/unlinks it inline, instead of calling mono_wasm_jit_enter_island / leave_island (interp.c, WjEhRec). p75, b t t b
 * on the Minecraft server tick: the island helpers 2.98 -> 0.08 M instr/tick, all instructions -3.6%, calls -9.6%;
 * cycles unresolved on that instrument (the helpers were ~1 M cycles/tick). 0 = the island calls. */
int mono_wasm_jit_eh_rec = 1;
/* MONO_WASM_JIT_AOT_BYREF (R356): the emitter's inline direct-AOT call also takes callees with byref PARAMETERS (the
 * ByteCodeHelper volatile/CAS helpers IKVM emits for every Java volatile/atomic access), instead of routing them
 * through the JIT->interp residual. p79 b t t b, server tick: the residual route 1.99 -> 0.58 M instr and 2.18 ->
 * 1.13 M cycles per tick, calls -3.8%, totals -0.7% (inside the spread). 0 = route them through the residual. */
int mono_wasm_jit_aot_byref = 1;
/* MONO_WASM_JIT_AOT_STATIC_UNBOX (R356): the same gate ignores need_unbox_trampoline for a callee without `this`. p80
 * b t t b, server tick: the residual route 0.92/0.69 -> 0.13/0.15 M instr and 1.44/1.20 -> 0.33/0.30 M cycles per tick. */
int mono_wasm_jit_aot_static_unbox = 1;
/* MONO_WASM_JIT_T2_UNIT (Track C, tier-2 units; scratchpad/wj/track-c-brief.md): 2 = supply census only -- classify
 * each tier-2 root's remaining direct callees ([wasm-jit t2u]); nothing about the emitted code changes. */
int mono_wasm_jit_t2_unit = 0;
/* MONO_WASM_JIT_LAZY_NOGC: credit a pool slot no-GC when a no-GC callee is registered at it, admitting that callee for
 * real instead of trusting its stub (0 = never credit a pool slot, the R361 m1 shape). */
int mono_wasm_jit_lazy_nogc = 1;

/* The vtable slot a METHOD-identity guard may test for a call to `base`, -2 for an interface method (its slot
 * is receiver-dependent), -1 otherwise. Plain field reads only: this runs inside the compile section, where a
 * metadata operation must not be added (CLAUDE.md). base->slot is final once the declaring class's vtable
 * exists, and a generic or inflated method is refused because its slot is not the one klass->vtable holds. */
int
mono_wasm_jit_mid_slot (MonoMethod *base)
{
	if (!base || !(base->flags & METHOD_ATTRIBUTE_VIRTUAL) || base->is_inflated || base->is_generic)
		return -1;
	if (mono_class_is_interface (base->klass))
		return -2;
	if (!m_class_get_vtable (base->klass) || base->slot < 0)
		return -1;
	return base->slot;
}
/* MONO_WASM_JIT_INLINE_LEAF: IL-byte limit for inlining a CALL-FREE callee, above mono's 20; 0 = off. See
 * mono_method_check_inlining_limit in method-to-ir.c. 64 is the value R292/R299 timed; it was not swept. */
int mono_wasm_jit_inline_leaf = 64;
/* MONO_WASM_JIT_DEADSET: consult the freed-method set (mono_wasm_jit_note_method_freed) before any site
 * dereferences a RETAINED MonoMethod *. On: it is the fix for the island-DFS asserts (R290). 0 = the A/B. */
int mono_wasm_jit_deadset = 1;
/* MONO_WASM_JIT_RETIRE_FREE: 0 = never free a retired registry payload (leak it). A DIAGNOSTIC, the
 * use-after-free discriminator for the reclamation scheme above wj_reclaim_retired. */
int mono_wasm_jit_retire_free = 1;
/* MONO_WASM_JIT_AOT_ENTRY: 0 = the jiterpreter's interp-entry trampolines never forward AOT callers
 * straight into a JIT f-slot, so every AOT->JIT entry takes the C interp_entry boundary. A DIAGNOSTIC
 * (it was once this switch's A/B baseline, then made unconditional); see mono_jiterp_wasm_jit_entry_ok. */
int mono_wasm_jit_aot_entry = 1;
/* MONO_WASM_JIT_REUSE_RESET: when a JS worker is taken up by a new pthread, demote or revert every table entry
 * that assumed the previous pthread (mono_wasm_jit_worker_reuse). 1 is the fix (R293); 0 only counts -- a
 * DIAGNOSTIC control, not a supported configuration. */
int mono_wasm_jit_reuse_reset = 1;
/* MONO_WASM_JIT_EDGE_SAMPLE: call-edge sampling period in ms, 0 = off (mono_wasm_jit_safepoint_poll_entry). */
int mono_wasm_jit_edge_sample = 0;
/* MONO_WASM_JIT_ENTRY_ADOPT: a worker adopts interp-entry trampolines another worker created (R293c,
 * mono_jiterp_entry_adopt_info). UNSAFE as built: 4 of 5 runs with it on failed (signature-mismatch traps
 * inside a trampoline, foreign_scratch back), 0 of 3 with it off (R293e). Kept for the diagnosis. */
int mono_wasm_jit_entry_adopt = 0;
int mono_wasm_jit_vcall_ways = 1; /* MONO_WASM_JIT_VCALL_WAYS: N-way inline vcall f-slot IC. Clamped [1,8].
 * DEFAULT 1, and that is measured, not conservative: on the plateau instrument (Minecraft, 2026-08, no-walk,
 * 240s cooldown per arm) 4 -> 2 -> 1 improves MONOTONICALLY -- 4 vs 1 is fps 23.7 -> 26.0, msFrame 42.2 -> 38.5 ms,
 * p90 49.7 -> 45.5 ms. Each way adds a guard to EVERY dispatch (~350M per in-game window) to catch misses that
 * are ~1% of that traffic, and MethodHandle/delegate stubs carry the chain unrolled (an `object(object)` MHC
 * adapter measured 1648 B at 4 ways). ic_autosize can only shrink a site the interpreter observed, and
 * runtime-generated stubs never are, so they keep the full width. Costs world generation ~9%. */
int mono_wasm_jit_vcall_aot_ways = 1; /* MONO_WASM_JIT_VCALL_AOT_WAYS: N-way inline AOT-vcall IC. Clamped [1,8].
 * DEFAULT 1. ways>1 is what makes the emitter emit the outlined `vcall_aot_pic_lookup` helper call at every
 * AOT-IC site, and an outlined call on a dispatch path costs more here than the code it saves. Measured vs 4
 * (Minecraft plateau, 2026-08, 2 rounds): p90 -12.8%, mean frame -12.0%, fps +13.6%. Costs boot: classload
 * +5.0%, resources +7.1% -- fewer ways means more misses while receivers are still being discovered. */

/* NB: compiled into BOTH the browser runtime and mono-aot-cross (no longer HOST_BROWSER-gated) so the
 * OFFLINE cross-compiler dump path (mini.c COMPILE_WASM fork) reads MONO_WASM_JIT_VERBOSE/DUMP_IR/STATS +
 * every lever from the env, exactly like the in-browser transform — that's what makes
 * `mono-aot-cross --aot <dll>` with MONO_WASM_JIT_METHOD set a faithful offline emit/bail dumper.
 * Idempotent (guarded on the auto<0 sentinel). The 4 trigger globals above are only USED by the browser
 * interp hotness/island code; defining them for the cross build is harmless (unreferenced there). */

void
mono_wasm_jit_auto_init (void)
{
	const char *e, *t;
	if (mono_wasm_jit_thresh >= 0 && mono_wasm_jit_auto >= 0)
		return;
	/* A DEFAULT IS STATED ONCE, AT ITS INITIALISER (comment rule #1), and this file used to break that
	 * rule structurally: every knob below had its default written TWICE -- once at `int mono_wasm_jit_x
	 * = N;` and again as the literal fallback here. Aligning the initialisers to the shipped values and
	 * dropping the app's overrides therefore changed NOTHING, because these fallbacks kept overwriting
	 * them: a capture taken right afterwards ran at threshold 2000 with an 11,044-method tier instead of
	 * threshold 500 with 29,224. Falling back to the variable's OWN value makes the initialiser the
	 * single source of truth and makes that drift impossible. */
	t = g_getenv ("MONO_WASM_JIT_THRESHOLD");
	{ int tv = (t && *t) ? atoi (t) : mono_wasm_jit_thresh;
	  if (tv > 0) mono_wasm_jit_thresh = tv; }
	{ extern int mono_wasm_jit_stats; const char *s = g_getenv ("MONO_WASM_JIT_STATS"); mono_wasm_jit_stats = (s && *s && *s != '0') ? 1 : 0; }
	/* MONO_WASM_JIT_VERBOSE controls the per-method emit LOG spam, DECOUPLED from stats (counting is cheap,
	 * logging floods): 0=silent (default), 1=+registered/invalid, 2=+bail, 3=+emit-enter AND per-call traces (vcall-aot
	 * dispatch etc. — kept >=3 so a stats/bail run at verbose<=2 is never flooded by per-invocation logs). The 23k-line log
	 * came from these firing whenever stats was on; the aggregated bail histogram replaces them at level 0. */
	{ extern int mono_wasm_jit_verbose; const char *vb = g_getenv ("MONO_WASM_JIT_VERBOSE"); mono_wasm_jit_verbose = (vb && *vb) ? atoi (vb) : 0; }
	/* The dead-retained-MonoMethod guard. DEFAULT ON: it converts a process abort into a counted refusal
	 * and every catch is attributed to its retainer. Gated only so a regression in the guard itself can be
	 * isolated in ONE binary -- the first version rejected live wrappers and dynamic methods and stalled
	 * boot 3/3, and finding that cost a full rebuild because there was no arm to turn it off. */
	{ const char *bm = g_getenv ("MONO_WASM_JIT_BADMETH"); mono_wasm_jit_badmeth = (bm && *bm) ? atoi (bm) : 1; }
	{ extern const char *mono_wasm_jit_watch; const char *w = g_getenv ("MONO_WASM_JIT_WATCH"); mono_wasm_jit_watch = (w && *w) ? g_strdup (w) : NULL; }
	{ extern int mono_wasm_jit_names; const char *nm = g_getenv ("MONO_WASM_JIT_NAMES"); mono_wasm_jit_names = (nm && *nm) ? ((*nm != '0') ? 1 : 0) : mono_wasm_jit_names; }
	{ extern int mono_wasm_jit_inline_zero; const char *iz = g_getenv ("MONO_WASM_JIT_INLINE_ZERO"); mono_wasm_jit_inline_zero = (iz && *iz) ? atoi (iz) : 64; }
	{ extern int mono_wasm_jit_frame_zero; const char *fz = g_getenv ("MONO_WASM_JIT_FRAME_ZERO"); if (fz && *fz) mono_wasm_jit_frame_zero = *fz != '0'; }
	{ extern int mono_wasm_jit_inline_alloc; const char *ia = g_getenv ("MONO_WASM_JIT_INLINE_ALLOC"); if (ia && *ia) mono_wasm_jit_inline_alloc = *ia != '0'; }
	{ const char *ec = g_getenv ("MONO_WASM_JIT_ENTRYCENSUS"); mono_wasm_jit_entry_census = (ec && *ec && *ec != '0') ? 1 : 0; } /* 1 = the ENTRY half of the per-worker census (mono_wasm_jit_liveness fields 6/9); adds a load+test to the interp->JIT boundary, so off while timing. The INSTANTIATION half (fields 5/7/8/10/13) is unconditional and needs no knob. */
	{ extern int mono_wasm_jit_elidediag; const char *ed = g_getenv ("MONO_WASM_JIT_ELIDEDIAG"); mono_wasm_jit_elidediag = (ed && *ed && *ed != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_lmf_publish_diag; const char *lp = g_getenv ("MONO_WASM_JIT_LMF_PUBLISH_DIAG"); mono_wasm_jit_lmf_publish_diag = (lp && *lp && *lp != '0') ? 1 : 0; } /* 1 = mono_set_lmf reports publishing an LMF head whose lmf_addr is 0 (an incomplete push); diagnostic only */ /* 1 = print per-method per-arm ref-slot elision attribution; diagnostic only */
	{ extern int mono_wasm_jit_guard_keep_slotlive; const char *gk = g_getenv ("MONO_WASM_JIT_GUARD_KEEP_SLOTLIVE"); mono_wasm_jit_guard_keep_slotlive = (gk && *gk && *gk != '0') ? 1 : 0; } /* 1 = keep elision on under STOREGUARD/OBJGUARD (partial guard coverage, real configuration) */
	{ extern int mono_wasm_jit_residual_mode; const char *r = g_getenv ("MONO_WASM_JIT_RESIDUAL"); mono_wasm_jit_residual_mode = (r && *r) ? atoi (r) : mono_wasm_jit_residual_mode; }
	{ extern int mono_wasm_jit_arity; const char *ar = g_getenv ("MONO_WASM_JIT_ARITY"); mono_wasm_jit_arity = (ar && *ar && *ar != '0') ? 1 : 0; } /* 1 = record per-call-site receiver-arity histogram (vcall miss population); diagnostic, perturbs timing */
	{ extern int mono_wasm_jit_guarded_inline; const char *gi = g_getenv ("MONO_WASM_JIT_GUARDED_INLINE"); if (gi && *gi) mono_wasm_jit_guarded_inline = *gi != '0'; }
	{ extern int mono_wasm_jit_guarded_inline_size; const char *gs = g_getenv ("MONO_WASM_JIT_GUARDED_INLINE_SIZE"); int v = (gs && *gs) ? atoi (gs) : 60; mono_wasm_jit_guarded_inline_size = (v >= 0 && v <= 4096) ? v : 60; }
	{ extern int mono_wasm_jit_delegate_devirt; const char *dd = g_getenv ("MONO_WASM_JIT_DELEGATE_DEVIRT"); int v = (dd && *dd) ? atoi (dd) : mono_wasm_jit_delegate_devirt; mono_wasm_jit_delegate_devirt = (v >= 0 && v <= 100) ? v : mono_wasm_jit_delegate_devirt; }
	{ extern int mono_wasm_jit_pred_pct; const char *pp = g_getenv ("MONO_WASM_JIT_PRED_PCT"); int v = (pp && *pp) ? atoi (pp) : 0; mono_wasm_jit_pred_pct = (v >= 0 && v <= 100) ? v : 0; }
	{ extern int mono_wasm_jit_devirt_arm2_pct; const char *ap = g_getenv ("MONO_WASM_JIT_DEVIRT_ARM2_PCT"); int v = (ap && *ap) ? atoi (ap) : 15; mono_wasm_jit_devirt_arm2_pct = (v >= 0 && v <= 100) ? v : 15; }
	{ extern int mono_wasm_jit_lazy_gcp; const char *lg = g_getenv ("MONO_WASM_JIT_LAZY_GCP"); mono_wasm_jit_lazy_gcp = (lg && *lg) ? atoi (lg) : 1; } /* GC points a method may have and still defer its ref frame; <=0 = unlimited */
	{ extern int mono_wasm_jit_lazy_cold; const char *lc = g_getenv ("MONO_WASM_JIT_LAZY_COLD"); if (lc && *lc) { int v = atoi (lc); mono_wasm_jit_lazy_cold = (v >= 0 && v <= 2) ? v : 0; } }
	{ extern int mono_wasm_jit_vcall_memo; const char *vm = g_getenv ("MONO_WASM_JIT_VCALL_MEMO"); if (vm && *vm) mono_wasm_jit_vcall_memo = *vm != '0'; }
	{ extern int mono_wasm_jit_ic_mid; const char *v = g_getenv ("MONO_WASM_JIT_IC_MID"); if (v && *v) mono_wasm_jit_ic_mid = *v != '0'; }
	{ extern int mono_wasm_jit_pred_mid; const char *v = g_getenv ("MONO_WASM_JIT_PRED_MID"); if (v && *v) mono_wasm_jit_pred_mid = *v != '0'; }
	{ extern int mono_wasm_jit_prof_inlinee; const char *v = g_getenv ("MONO_WASM_JIT_PROF_INLINEE"); if (v && *v) mono_wasm_jit_prof_inlinee = *v != '0'; }
	{ extern int mono_wasm_jit_prof_blocks; const char *v = g_getenv ("MONO_WASM_JIT_PROF_BLOCKS"); if (v && *v) { int n = atoi (v); mono_wasm_jit_prof_blocks = (n >= 1 && n <= 16) ? n : 1; } }
	{ extern int mono_wasm_jit_forward_retired; const char *v = g_getenv ("MONO_WASM_JIT_FORWARD_RETIRED"); if (v && *v) mono_wasm_jit_forward_retired = *v != '0'; }
	{ extern int mono_wasm_jit_lean_try_invoke; const char *v = g_getenv ("MONO_WASM_JIT_LEAN_TRY_INVOKE"); if (v && *v) mono_wasm_jit_lean_try_invoke = *v != '0'; }
	{ extern int mono_wasm_jit_ic_remat; const char *v = g_getenv ("MONO_WASM_JIT_IC_REMAT"); if (v && *v) mono_wasm_jit_ic_remat = *v != '0'; }
	{ extern int mono_wasm_jit_inline_calls; const char *v = g_getenv ("MONO_WASM_JIT_INLINE_CALLS"); if (v && *v) mono_wasm_jit_inline_calls = *v != '0'; }
	{ extern int mono_wasm_jit_inline_calls_limit; const char *v = g_getenv ("MONO_WASM_JIT_INLINE_CALLS_LIMIT"); if (v && *v) { int n = atoi (v); if (n > 0 && n <= 1000) mono_wasm_jit_inline_calls_limit = n; } }
	{ extern int mono_wasm_jit_inline_calls_cost; const char *v = g_getenv ("MONO_WASM_JIT_INLINE_CALLS_COST"); if (v && *v) { int n = atoi (v); if (n > 0 && n <= 100000) mono_wasm_jit_inline_calls_cost = n; } }
	{ extern int mono_wasm_jit_inline_calls_depth; const char *v = g_getenv ("MONO_WASM_JIT_INLINE_CALLS_DEPTH"); if (v && *v) { int n = atoi (v); if (n > 0 && n <= 32) mono_wasm_jit_inline_calls_depth = n; } }
#define WJ_T2_KNOB(var, name, lo, hi) { extern int var; const char *v = g_getenv (name); if (v && *v) { int n = atoi (v); if (n >= (lo) && n <= (hi)) var = n; } }
	WJ_T2_KNOB (mono_wasm_jit_t2, "MONO_WASM_JIT_T2", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_t2_sample_ms, "MONO_WASM_JIT_T2_SAMPLE_MS", 1, 1000)
	WJ_T2_KNOB (mono_wasm_jit_t2_threshold, "MONO_WASM_JIT_T2_THRESHOLD", 1, 1000000)
	WJ_T2_KNOB (mono_wasm_jit_t2_max, "MONO_WASM_JIT_T2_MAX", 0, 100000)
	WJ_T2_KNOB (mono_wasm_jit_t2_limit, "MONO_WASM_JIT_T2_LIMIT", 1, 1000)
	WJ_T2_KNOB (mono_wasm_jit_t2_cost, "MONO_WASM_JIT_T2_COST", 1, 100000)
	WJ_T2_KNOB (mono_wasm_jit_t2_depth, "MONO_WASM_JIT_T2_DEPTH", 1, 32)
	WJ_T2_KNOB (mono_wasm_jit_t2_gi_size, "MONO_WASM_JIT_T2_GI_SIZE", 0, 4096)
	WJ_T2_KNOB (mono_wasm_jit_prof_origin, "MONO_WASM_JIT_PROF_ORIGIN", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_fast_tls, "MONO_WASM_JIT_FAST_TLS", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_inline_cold_throw, "MONO_WASM_JIT_INLINE_COLD_THROW", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_inline_bfi, "MONO_WASM_JIT_INLINE_BFI", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_prof_share, "MONO_WASM_JIT_PROF_SHARE", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_ldaddr_ref, "MONO_WASM_JIT_LDADDR_REF", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_reemit_validate, "MONO_WASM_JIT_REEMIT_VALIDATE", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_t2_static_pred, "MONO_WASM_JIT_T2_STATIC_PRED", 0, 2)
	WJ_T2_KNOB (mono_wasm_jit_t2_rearm, "MONO_WASM_JIT_T2_REARM", 0, 8)
	WJ_T2_KNOB (mono_wasm_jit_t2_sample_loop, "MONO_WASM_JIT_T2_SAMPLE_LOOP", 0, 2)
	WJ_T2_KNOB (mono_wasm_jit_t2_colocate, "MONO_WASM_JIT_T2_COLOCATE", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_branch_hints, "MONO_WASM_JIT_BRANCH_HINTS", 0, 2)
	WJ_T2_KNOB (mono_wasm_jit_math_intrins, "MONO_WASM_JIT_MATH_INTRINS", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_atomic_i8, "MONO_WASM_JIT_ATOMIC_I8", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_trace_compile, "MONO_WASM_JIT_TRACE_COMPILE", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_t2_max_body, "MONO_WASM_JIT_T2_MAX_BODY", 0, 64 * 1024 * 1024)
	WJ_T2_KNOB (mono_wasm_jit_eh_rec, "MONO_WASM_JIT_EH_REC", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_aot_byref, "MONO_WASM_JIT_AOT_BYREF", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_aot_static_unbox, "MONO_WASM_JIT_AOT_STATIC_UNBOX", 0, 1)
	WJ_T2_KNOB (mono_wasm_jit_t2_unit, "MONO_WASM_JIT_T2_UNIT", 0, 2)
	WJ_T2_KNOB (mono_wasm_jit_lazy_nogc, "MONO_WASM_JIT_LAZY_NOGC", 0, 1)
#if defined(HOST_WASM) && defined(__wasm_atomics__)
	/* mono_wasm_hw_fence lives in utils/atomic.c and exists only in the threaded wasm runtime, not in mono-aot-cross. */
	WJ_T2_KNOB (mono_wasm_hw_fence, "MONO_WASM_HW_FENCE", 0, 1)
#endif
#undef WJ_T2_KNOB
	{ extern int mono_wasm_jit_inline_leaf; const char *il = g_getenv ("MONO_WASM_JIT_INLINE_LEAF"); if (il && *il) { int v = atoi (il); mono_wasm_jit_inline_leaf = (v >= 0 && v <= 256) ? v : 0; } }
	{ extern int mono_wasm_jit_deadset; const char *ds = g_getenv ("MONO_WASM_JIT_DEADSET"); if (ds && *ds) mono_wasm_jit_deadset = *ds != '0'; }
	{ extern int mono_wasm_jit_retire_free; const char *rf = g_getenv ("MONO_WASM_JIT_RETIRE_FREE"); if (rf && *rf) mono_wasm_jit_retire_free = *rf != '0'; }
	{ extern int mono_wasm_jit_aot_entry; const char *ae = g_getenv ("MONO_WASM_JIT_AOT_ENTRY"); if (ae && *ae) mono_wasm_jit_aot_entry = *ae != '0'; }
	{ extern int mono_wasm_jit_reuse_reset; const char *rr = g_getenv ("MONO_WASM_JIT_REUSE_RESET"); if (rr && *rr) mono_wasm_jit_reuse_reset = *rr != '0'; }
	{ extern int mono_wasm_jit_vcall_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline vcall IC; clamp [1,8]; 1 = legacy monomorphic */
	{ extern int mono_wasm_jit_vcall_aot_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_AOT_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_aot_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline AOT-vcall IC; clamp [1,8]; 1 = legacy first-wins */
	/* Default 1, matching the initialiser. It read `: 0` here while the initialiser said 1 and the comment
	 * block above it said "DEFAULT ON, and 0 is MEASURED WORSE" -- and auto_init runs once and overwrites,
	 * so the EFFECTIVE default was 0: the arm R153 measured as reintroducing `function signature mismatch`.
	 * Every co-location measurement taken before this fix was on that arm. Expected to become non-binding
	 * once the tight-dep republish lands: a correct dep list is what made cross-group edges want to be
	 * imports. (It landed, and is unconditional -- see the note at wj_asm_policy_init.) */
	{ extern int mono_wasm_jit_relink_jitted; const char *rj = g_getenv ("MONO_WASM_JIT_RELINK_JITTED"); if (rj && *rj) mono_wasm_jit_relink_jitted = (*rj != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_colocate_max, mono_wasm_jit_colocate_bytes;
	  { extern int mono_wasm_jit_colocate_merge; const char *cg = g_getenv ("MONO_WASM_JIT_COLOCATE_MERGE"); mono_wasm_jit_colocate_merge = (cg && *cg && *cg != '0') ? 1 : 0; }
	  const char *cm = g_getenv ("MONO_WASM_JIT_COLOCATE_MAX"); if (cm && *cm) { int v = atoi (cm); if (v >= 2 && v <= 512) mono_wasm_jit_colocate_max = v; }
	  const char *cb = g_getenv ("MONO_WASM_JIT_COLOCATE_BYTES"); if (cb && *cb) { int v = atoi (cb); if (v > 0) mono_wasm_jit_colocate_bytes = v; } }
	{ extern int mono_wasm_jit_rendezvous_test; const char *rt = g_getenv ("MONO_WASM_JIT_RENDEZVOUS_TEST"); if (rt && *rt) { int v = atoi (rt); if (v >= 0) mono_wasm_jit_rendezvous_test = v; } }
	{ extern int mono_wasm_jit_reemit; const char *re = g_getenv ("MONO_WASM_JIT_REEMIT"); if (re && *re) { int v = atoi (re); if (v >= 0) mono_wasm_jit_reemit = v; } }
	{ extern int mono_wasm_jit_reemit_misses; const char *rm = g_getenv ("MONO_WASM_JIT_REEMIT_MISSES"); if (rm && *rm) { int v = atoi (rm); if (v > 0) mono_wasm_jit_reemit_misses = v; } }
	{ extern int mono_wasm_jit_reemit_batch; const char *rb = g_getenv ("MONO_WASM_JIT_REEMIT_BATCH"); if (rb && *rb) { int v = atoi (rb); if (v >= 1 && v <= 64) mono_wasm_jit_reemit_batch = v; } }
	{ extern int mono_wasm_jit_reemit_interval; const char *ri = g_getenv ("MONO_WASM_JIT_REEMIT_INTERVAL_MS"); if (ri && *ri) { int v = atoi (ri); if (v >= 0) mono_wasm_jit_reemit_interval = v; } }
	{ extern int mono_wasm_jit_reemit_max; const char *rx = g_getenv ("MONO_WASM_JIT_REEMIT_MAX"); if (rx && *rx) { int v = atoi (rx); if (v >= 0) mono_wasm_jit_reemit_max = v; } }
	{ extern int mono_wasm_jit_dump_dep_graph_knob; const char *dg = g_getenv ("MONO_WASM_JIT_DUMP_DEP_GRAPH"); mono_wasm_jit_dump_dep_graph_knob = (dg && *dg) ? (*dg != '0') : 0; }
	{ extern int mono_wasm_jit_verify_deps; const char *vd = g_getenv ("MONO_WASM_JIT_VERIFY_DEPS"); mono_wasm_jit_verify_deps = (vd && *vd) ? (*vd != '0') : 0; }
	{ extern int mono_wasm_jit_single_writer; const char *sw = g_getenv ("MONO_WASM_JIT_SINGLE_WRITER"); mono_wasm_jit_single_writer = (sw && *sw) ? (*sw != '0') : 0; }
	{ extern int mono_wasm_jit_sweep; const char *sp = g_getenv ("MONO_WASM_JIT_SWEEP"); mono_wasm_jit_sweep = (sp && *sp) ? atoi (sp) : 0; }
	{ extern int mono_wasm_jit_raise_nogc; const char *rn = g_getenv ("MONO_WASM_JIT_RAISE_NOGC"); int v = (rn && *rn) ? atoi (rn) : 0; mono_wasm_jit_raise_nogc = (v >= 0 && v <= 2) ? v : 0; } /* 0=off, 1=liveness-only exemption (shipped, UNVERIFIED), 2=POSITIVE CONTROL ONLY (level 1 minus the gen_skipped_raises guard = the codegen measured 4/4 dead). See wj_ins_is_gcpoint -- its verdicts must be read in order. */
	{ extern const char *mono_wasm_jit_dump_ir; mono_wasm_jit_dump_ir = g_getenv ("MONO_WASM_JIT_DUMP_IR"); } /* substring filter; methods whose full name contains it get their clauses+bb regions+opcodes dumped (ground truth for the nested-EH lowering). */
	/* Island heuristic levers (Part 5), all default off. */
	{ extern int mono_wasm_jit_entry_promote; const char *ep = g_getenv ("MONO_WASM_JIT_ENTRY_PROMOTE"); mono_wasm_jit_entry_promote = (ep && *ep) ? atoi (ep) : mono_wasm_jit_entry_promote; }      /* Lever A: 0=off */
	{ extern int mono_wasm_jit_profile_fast; const char *pf = g_getenv ("MONO_WASM_JIT_PROFILE_FAST"); mono_wasm_jit_profile_fast = (pf && *pf && *pf != '0') ? 1 : 0; } /* emit fast-path volume counters, 0=off */
	{ extern int mono_wasm_jit_promotion_drain; const char *pd = g_getenv ("MONO_WASM_JIT_PROMOTION_DRAIN"); mono_wasm_jit_promotion_drain = (pd && *pd && atoi (pd) > 0) ? atoi (pd) : mono_wasm_jit_promotion_drain; }
#ifdef HOST_BROWSER
	/* These three are DEBUG store/GC guards whose globals + runtime-check emission are HOST_BROWSER-only
	 * (they insert per-store checks that only do anything when the JITted code actually RUNS). The offline
	 * cross-compiler dump never executes JITted code, so skip their env here — otherwise auto_init would
	 * reference browser-only globals and fail to link into mono-aot-cross. */
	{ extern int mono_wasm_jit_storeguard; const char *sg = g_getenv ("MONO_WASM_JIT_STOREGUARD"); mono_wasm_jit_storeguard = (sg && *sg && *sg != '0') ? 1 : 0; } /* DEBUG: bounds-check every ref/addr-frame STORE to catch the wild store (traps at the culprit).
 * Emitted in mini-wasm-ir.inc, not in the emitter: check_store kind 0 in wasm_st (the ref
 * shadow-stack store, refbase + slot*4) and kind 1 in wasm_addr_st (addrbase + off). It also
 * disables the lazy GC frame and ref-slot elision, so every ref/addr vreg has a real slot to check.
 *
 * NOT the LOAD side: the ref shadow-stack load in wasm_ld and wasm_addr_ld are unguarded, and
 * OBJGUARD's kind 4 (wasm_guard_memaddr) covers membase loads but not frame-slot loads. R269
 * wasted time asserting this knob emitted nothing at all, from a grep of mini-wasm-emitter.inc
 * alone -- every low-level guard lives in mini-wasm-ir.inc. Grep BOTH before describing a knob.
 * default off */
	{ extern int mono_wasm_jit_objguard; const char *og = g_getenv ("MONO_WASM_JIT_OBJGUARD"); mono_wasm_jit_objguard = (og && *og && *og != '0') ? 1 : 0; } /* DEBUG: before every ref-field store, validate the object BASE is a live heap object (catches missed-ref/stale-base wild stores). default off */
#endif
	{ extern int mono_wasm_jit_missedref; const char *mr = g_getenv ("MONO_WASM_JIT_MISSEDREF"); mono_wasm_jit_missedref = (mr && *mr && *mr != '0') ? 1 : 0; } /* DIAG: names a missed ref. For every method, log any NONREF-classified i32 vreg used as a MEMBASE load/store base or virtual-call receiver (a stale one of these is the wild-deref corruptor), with its defining opcode -> pins which wj_opcode_is_nonref case is wrong. Bounded. default off */
	{ extern int mono_wasm_jit_refverify; const char *rv = g_getenv ("MONO_WASM_JIT_REFVERIFY"); mono_wasm_jit_refverify = (rv && *rv) ? atoi (rv) : 0; } /* 1=log, 2=assert classification-vs-structural-marking violations; default off */
	{ extern int mono_wasm_jit_entry_adopt; const char *ea = g_getenv ("MONO_WASM_JIT_ENTRY_ADOPT"); if (ea && *ea) mono_wasm_jit_entry_adopt = *ea != '0'; }
	{ extern int mono_wasm_jit_edge_sample; const char *es = g_getenv ("MONO_WASM_JIT_EDGE_SAMPLE"); if (es && *es) { int v = atoi (es); mono_wasm_jit_edge_sample = (v >= 0 && v <= 10000) ? v : 0; } }
#ifdef HOST_BROWSER
	{ extern void mono_wasm_jit_edge_start (void); mono_wasm_jit_edge_start (); }
	{ extern void mono_wasm_jit_t2_start (void); mono_wasm_jit_t2_start (); }
#endif
	e = g_getenv ("MONO_WASM_JIT_AUTO");
	mono_memory_barrier ();
	/* DEFAULT 1. This is the single switch that turns the whole JIT tier off, so it keeps its knob where
	 * the other settled booleans lost theirs -- it is the first bisect step for any "is this the wasm
	 * JIT?" question. It used to default OFF, which meant the tier existed only because the app turned
	 * it on. Set LAST, because writing a non-negative value here is what publishes "initialised" to the
	 * re-entry guard at the top; see the sentinel note at the initialiser. */
	mono_wasm_jit_auto = (e && *e) ? ((*e != '0') ? 1 : 0) : 1;
}

/* The forced-compile routing is now JIT_FLAG_WASM_FORCE (per-compile), copied to cfg->wasm_jit_forced
 * in mini.c — the old __thread mono_wasm_jit_force flag is gone (it leaked across nested cctor-driven
 * compiles of unrelated methods; the per-compile flag is scoped correctly). */

#include <string.h>
#include <stdio.h>

/* Bench/measurement counters for the wasm method-JIT, mirroring the jiterpreter's stats infra so the
 * consumer's "Bench 60s" / heat-snapshot harness can A/B the JIT. All counting is gated behind
 * MONO_WASM_JIT_STATS so release builds pay nothing (the hot invoke/residual increments are skipped via
 * a predictable not-taken branch when off). The counters live in a single gint64 array (see the WJC_*
 * enum + mono_wasm_jit_count/_add/_max + the mono_wasm_jit_get_counter export in mini-wasm.h): one
 * source of truth, 64-bit so the per-frame transition counts can't overflow wasm32's 32-bit `long`. */
int mono_wasm_jit_stats = 0;            /* MONO_WASM_JIT_STATS=1 enables counting */
gint64 mono_wasm_jit_counters [WJC_MAX] = { 0 };

void
mono_wasm_jit_count (int idx)
{
	mono_atomic_inc_i64 (&mono_wasm_jit_counters [idx]);
}

void
mono_wasm_jit_add (int idx, gint64 v)
{
	mono_atomic_add_i64 (&mono_wasm_jit_counters [idx], v);
}

/* (mono_wasm_jit_max — a racy high-water setter — lived here. Its only counter was WJC_REF_HWM, the old
 * ref shadow stack's depth; both are gone. Reintroduce it if a future counter is a max rather than a sum.) */

/* Per-method emit LOG verbosity (MONO_WASM_JIT_VERBOSE), independent of the counters: 0 silent, 1
 * registered+invalid, 2 +bail, 3 +emit-enter. Default 0 so a stats run no longer floods stdout. */
int mono_wasm_jit_verbose = 0;
const char *mono_wasm_jit_watch = NULL;
int mono_wasm_jit_names = 1;
/* MONO_WASM_JIT_NAMES KEEPS ITS KNOB and gets default 1. It appends a wasm name section per JITted
 * module, which is what makes every profile symbolised and every trap self-symbolicating -- without it
 * perf reads the whole tier as a bare `wasmjit`. It stays switchable because the symbolisation loop is
 * the source of R199's intermittent `memory access out of bounds` (mono_method_get_full_name walking a
 * signature lazily, on a worker, inside the compile section), so `names=0` is the bisect arm for that
 * fault. The name is now cached at EMIT time, which is the fix; the knob is the fallback. */

/*
 * A header whose EH clause table does not name offsets in its own body. Called from
 * mark_bb_in_region (method-to-ir.c), which used to g_assert here and take the whole process down.
 *
 * The name is printed ONLY under MONO_WASM_JIT_VERBOSE, and that gate is not politeness: this runs on
 * a WORKER inside the compile section, and mono_method_get_full_name walking a signature lazily right
 * there is the source of R199's intermittent `memory access out of bounds` -- the same reason
 * MONO_WASM_JIT_NAMES still has a knob. The COUNTER is always bumped, so a silent run still reports
 * the catch; only the attribution costs the risky call, and only when it is asked for.
 */
void
mono_wasm_jit_note_bad_eh_clause (MonoMethod *method, guint32 off, guint32 code_size)
{
	mono_wasm_jit_count (WJC_BAD_EH_CLAUSE);

	if (mono_wasm_jit_verbose > 0) {
		char *name = method ? mono_method_get_full_name (method) : NULL;
		g_print ("[wasm-jit] WASM_JIT_BAD_EH_CLAUSE: clause offset %u outside/unclaimed in %u-byte body of %s -- declining\n",
			 off, code_size, name ? name : "<unknown>");
		g_free (name);
	}
}
/* The helper-import cap is now WJ_MAX_HELPER_IMPORTS-bounded and fixed at 192.
 *
 * MONO_WASM_JIT_MAX_HIMP is deleted as NON-BINDING, and the way that was established is the useful
 * part. A cross-binary reading once suggested 64 -> 192 was worth +3.9%% fps; the within-binary A/B the
 * knob existed to make possible put every metric inside noise, with the direction if anything favouring
 * the SMALLER cap -- n=3 vs 6 at a 5-9%% spread is exactly the regime that manufactures a 4%% result.
 * Counting imports in the hot set directly agrees: max 30 declared in any hot module, median 3, against
 * a cap of 192. If the emitter ever imports per-callee rather than per-helper the cap could start to
 * bind, and then this becomes a constant to raise rather than a knob to sweep. */
#define WJ_HELPER_IMPORT_CAP 192
/* MONO_WASM_JIT_INLINE_ILOFS IS DELETED, AND ITS LESSON IS THE POINT (comment rule #5).
 *
 * It stored the per-basic-block IL offset INLINE -- three wasm ops against a method-long local holding
 * this activation's MonoMethodILState -- instead of calling mono_wasm_jit_set_il_offset once per basic
 * block of every EH-bearing method. The helper's whole body is a TLS load, a null test and a store, and
 * its 0.39%% self time understates it: 82%% of spilled hot instructions are call-adjacent, and
 * caller-side spill cost is invisible in a callee's self time.
 *
 * RESULT: WORSE, decisively -- p50 41.2 -> 45.2 ms (+9.8%%, spread +-1.1%%), p90 +8.6%%, fps 25.5 -> 22.8
 * (-10.6%%). The reasoning was wrong in an instructive way. Removing the call required keeping the
 * il_state pointer in a METHOD-LONG local, which creates a live value spanning every call in the method
 * -- exactly the call-adjacent live range the spill census was counting. Previously the TLS load
 * happened INSIDE the helper, so nothing was live in the caller across it. On this register-starved
 * target a long-lived value costs more than the call it replaces.
 *
 * THE GENERAL LESSON, which contradicts the naive reading of "82%% of spills are call-adjacent":
 * inlining a helper pays only when it removes CODE (vcall_shared_miss=0, -13.2%%), not when it converts
 * a call into additional live state. Check what a change does to LIVE RANGES before assuming fewer
 * calls is better. MONO_WASM_JIT_ILOFS_GLOBAL is the version that works, and it works precisely because
 * it leaves nothing live: `global.get` the il_state pointer at each store site instead of caching it. */
/* MONO_WASM_JIT_INLINE_ZERO: zero a small GC-scanned frame with inline i64 stores instead of `memory.fill`.
 *
 * Every JITted method with a frame emits, at entry, `memory.fill (refbase, 0, framebytes)`. For the frames
 * that actually occur that is tiny — a hot int->int Minecraft method (class_3508:method_15211, 0.39% of the
 * whole in-game window on its own) fills SIXTEEN BYTES this way on every call. memory.fill is a bulk-memory
 * instruction that V8 lowers through a generic memset path with its own setup and length dispatch; two
 * i64.store instructions do the same work with no call and no branching.
 *
 * framebytes is `(refbytes_al + naddrbytes + 15) & ~15`, i.e. always a multiple of 16 and 16-aligned, so the
 * inline form is exactly framebytes/8 i64 stores with no remainder handling and 8-byte alignment guaranteed.
 * Capped so a large frame still uses the bulk op rather than bloating the body.
 *
 * Note this creates NO new live value — refbase is already live and already the fill's first operand — so the
 * Round 110 failure mode (trading a call for a long live range) does not apply. */
int mono_wasm_jit_inline_zero = 64;   /* max framebytes to zero inline; 0 disables (always memory.fill). */
/* MONO_WASM_JIT_INLINE_ALLOC: known-size `new` bump-allocates from the thread's TLAB in emitted code
 * (OP_WASM_JIT_ALLOC_FAST), calling ves_icall_object_new_specific only when the TLAB is exhausted or the thread
 * is detached. The allocator call it replaces went through the whole C path -- ves_icall_object_new_specific,
 * mono_object_new_specific_checked, sgen_alloc_obj, sgen_try_alloc_obj_nolock, object_new_common_tail -- ~3.2 M
 * instr/tick of self time on the server thread (p22 z1, 2026-09-24) before counting the call. 0 = the A/B arm.
 * The managed allocator wrapper that would have done this is unusable from here: a JIT call resolves it to the
 * interpreter, which cannot run its TLS opcodes (handle_alloc in method-to-ir.c). R305. */
int mono_wasm_jit_inline_alloc = 1;
gint32 mono_wasm_jit_inline_alloc_sites;   /* emit-time count of allocations lowered inline */
/* MONO_WASM_JIT_FRAME_ZERO: 0 = leave a frame's write-through ref region unzeroed (the emitter's eager
 * prologue carries the argument). ~3.5 M instr/tick (R294f, R299); its cost is more conservative stack pins,
 * +20% stack-pinned bytes per minor GC with no measurable heap growth (R300). */
int mono_wasm_jit_frame_zero = 0;
/* MONO_WASM_JIT_DELEGATE_OBJ_PIC is deleted -- the losing arm of a settled two-path choice, with
 * DELEGATE_LOCAL_PIC keeping the field.
 *
 * It cached the (f-slot, shape) recipe on the (delegate class, target method) tramp info hanging off
 * del->invoke_info, so generated code could reach it in two loads from `this` with no site id, no
 * bounds check, no key compare and no ways loop. The KEYING WORKS: miss-path publications went
 * 88,209,759 -> 1,996, a 44,000x reduction. The emitted stub still got BIGGER in the only place it
 * matters: `__<>MHC` 1,311 B -> 1,289 B, i.e. -1.7%%, because the `wj_slot_live` probe costs back what
 * the site-id derivation, bounds check and key compare saved.
 *
 * WHY THE PROBE CANNOT BE REMOVED, which is the durable part: modules are compiled ONCE and broadcast,
 * then INSTANTIATED PER WORKER, so a cached f-slot NUMBER is process-wide while its INSTALLATION is per
 * worker. R63b's per-site PIC could skip the probe only because that array was itself per worker.
 * Object-keyed AND probe-free needs storage that is per-callee AND per-worker -- a different data
 * structure, not a re-argument.
 *
 * And it was sized before an arm was spent on it: ~4.8%% of delegate dispatches were missing the recipe
 * (fast_delegate 418,401,288 vs delegate_ic_hit 398,520,024), so the whole saving is ~0.26%% of window,
 * far below this box's floor. */

/* MONO_WASM_JIT_THREAD_SP IS DELETED, AND ITS REFUTATION IS THE DURABLE PART.
 *
 * It threaded the shadow-frame base as a TRAILING function parameter instead of reading the imported
 * mutable `__stack_pointer` global. The motivating measurement is real and still stands: `s.p` is
 * 4.4-5.3% of the in-game window (R190/R192), 95.3% of the `add %r14,reg` decompression pool is
 * entry-band and chained off a `(%r14)` load -- the signature of the imported-mutable path -- and an
 * imported MUTABLE global costs four extra loads per access against one for a module-defined one
 * (wasm-lowering-reducer.h:1079-1110 vs :1129-1145). `s.p` is the only mutable import, and every framed
 * method both reads and writes it.
 *
 * IT MEASURED +39.2% SLOWER (R218). jbox2d, 6 rounds in BOTH orders, every run checksum
 * -1419038276309998642, `registered=389` / `invalid=0` / residual identical on both arms so the tier
 * was fully alive: median 1.2485 -> 1.7375 ms/step, non-overlapping ranges 6 of 6.
 *
 * THE REASONING ERROR IS WORTH MORE THAN THE KNOB. The plan argued "params are `local.get`, which V8
 * lowers to zero instructions, so the cost is wire size only" -- and that conflates two things.
 * `local.get` of an EXISTING local is free; ADDING A PARAMETER is an argument materialisation at every
 * call site plus one more live incoming value in every callee, on a tier already at 32.21% register
 * pressure and 28.1 locals per function. PRICE A CALLING-CONVENTION CHANGE AT THE CALL SITES, NEVER AS
 * A LOCAL-OP COUNT.
 *
 * AND THREADING ALONE COULD ONLY EVER HAVE WON A THIRD OF THE POOL. A framed method does `global.get`
 * once in the prologue and `global.set` at every exit and every EH landing pad; only the GET is
 * replaced by a parameter. The two SETs are what make native callees allocate BELOW our frame, which
 * is both why the frame sits inside the conservatively scanned range and why nothing clobbers it --
 * the scan itself is NOT bounded by the global (sgen-stw.c:73-91 takes the address of a local in
 * sgen's own frame; mini-gc.c:1136-1139 then pins all of [stack_limit, stack_end)). They go only once
 * something else has put the global below our frames, i.e. a CHUNK reserved once per interp->JIT
 * transition, 409x rarer than a method entry (2.4M transitions vs 974M dispatches per window). So the
 * win was always THE CHUNK, with threading as what makes the chunk legal -- and the chunk has to beat
 * a cost of the order measured above before any of it pays.
 *
 * Two details to keep if it is ever revisited. The parameter must go TRAILING, after the hidden vret
 * pointer: the prologue's ref-arg pin stores read incoming arg `i` as `local.get i`, so a leading param
 * shifts every argument index (CoreCLR can use arg 0 -- morph.cpp:1802-1809 -- because its prologue
 * does not index positionally). And the jiterpreter's interp-entry trampoline forwards STRAIGHT to `f`
 * with its own generated call_indirect and has no way to read a wasm global, so it must either be given
 * an imported global or be refused the direct-forward path -- which is a confound to name, not a free
 * change. */
/* MONO_WASM_JIT_ENTRYCENSUS=1; the census state and reader live in the HOST_BROWSER section below, but
 * this flag must be defined OUT here: mono_wasm_jit_auto_init reads it and mini-wasm.c is linked into
 * mono-aot-cross as well as the runtime, so a browser-only definition is an undefined symbol at the
 * cross-compiler link (same reason mono_wasm_jit_devirt_profile is defined here rather than interp.c). */
int mono_wasm_jit_entry_census = 0;
/* MONO_WASM_JIT_ELIDEDIAG=1: per-method, per-ARM ref-slot elision attribution (WASM_JIT_ELIDE lines).
 * Defined out here for the same mono-aot-cross link reason as the knobs above. */
int mono_wasm_jit_elidediag = 0;

/* MONO_WASM_JIT_LMF_PUBLISH_DIAG=1: make mono_set_lmf (mini-runtime.c) report any attempt to publish an LMF
 * chain head whose lmf_addr is still 0 -- i.e. a push that never wrote the LMF body. Defined here, OUTSIDE
 * HOST_BROWSER, for the same reason the other wasm-JIT flags are: mono-aot-cross links this file. */
int mono_wasm_jit_lmf_publish_diag = 0;
/* MONO_WASM_JIT_GUARD_KEEP_SLOTLIVE=1: do not silently disable SLOTLIVE when STOREGUARD/OBJGUARD is on,
 * so the guards can actually be pointed at an elision-dependent corruption. Costs guard COVERAGE only
 * (OBJGUARD kind 2's ref proxy goes partial); every address-range check stays sound. */
int mono_wasm_jit_guard_keep_slotlive = 0;

/* Aggregated bail-reason histogram (Part 4): the per-method WASM_JIT_BAIL lines (7028 of them in the
 * jit121 capture) buried the signal — 151 ldaddr, 47 EH. This rolls every bail into category buckets +
 * a per-opcode count so the dominant blocker is one summary line. Bumped at `done:` whenever a compile
 * bails (O(1), runs even at verbose 0); dumped by mono_wasm_jit_dump_bail_hist. */
enum { WJB_CALLEE_NOT_JITTED, WJB_RESIDUAL_SHAPE, WJB_EH_CLAUSE, WJB_ARGRET_TYPE, WJB_SYNC_OTHER, WJB_OPCODE, WJB_N };
static guint32 wj_bail_hist [WJB_N];
#define WJ_BAIL_OPMAX 1400
static guint32 wj_bail_op_hist [WJ_BAIL_OPMAX];   /* indexed by mini opcode; counts the >0 fail_op bails */
/* Ring buffer of the last 128 residual callees (MonoMethod*), for post-crash diagnosis: a residual's
 * interp_entry can be what trips a failure, and the last ring entries name it.
 * Populated (gated by MONO_WASM_JIT_STATS) in mono_wasm_jit_call_interp; dumped via the export below. */
MonoMethod *mono_wasm_jit_ring [128];
int mono_wasm_jit_ring_count = 0;
int mono_wasm_jit_ring_frozen = 0; /* set at a detected failure point so the ring stops recording
                                    * before the post-crash crash-report flood overwrites it */

/* Called from sre.c when RuntimeResolve returns null: freeze the residual ring so a post-crash
 * dumpResidualRing() shows the residuals up to the failure. */
void
mono_wasm_jit_freeze_ring (void)
{
	mono_wasm_jit_ring_frozen = 1;
}
/* MONO_WASM_JIT_RESIDUAL selects the un-JITted-target interp residual MODE (no-rebuild kill-switch +
 * a bisection knob to isolate which residual call SHAPE mis-marshals/corrupts, by restricting which
 * shapes take the residual vs bail the whole method to the interpreter):
 *   0 = off  (a JITted method bails to interp when it calls an un-JITted callee; pre-residual behaviour)
 *   1 = full (residual every supported direct call)
 *   2 = only calls with a VOID return   (skips return-value marshalling)
 *   3 = only calls with NO params       (skips param marshalling; `this` still allowed)
 *   4 = only STATIC calls (no `this`)   (skips this marshalling; params/return allowed)
 *   5 = everything EXCEPT calls with params AND a non-void return
 * Check the bench stats (residual count) to confirm a restricted mode still exercised the residual. */
int mono_wasm_jit_residual_mode = 0;   /* SHIPPED DEFAULT. Tuned on Minecraft 1.16.1 + Fabric + Sodium/Lithium under IKVM, 2026-08/09, plateau protocol. A number without a workload is not a result -- this one has one, and it is not necessarily right for anything else. */
/* 0 = do NOT residual-route an un-JITted callee: block on it and grow an island instead, so the
 * caller and callee end up compiled together. 1 (the old default) routed every such call through the
 * interp boundary at ~1,360 instructions of pure marshalling per crossing. RESIDUAL_PERM keeps the
 * one case that must still route: a PERMANENTLY un-JITtable callee, which can never clear. */
/* MONO_WASM_JIT_STACKPROBE: record the worst C-stack headroom seen at the compile chain's probe points.
 * DEFAULT OFF. Diagnostic only -- see wj_stack_probe. Exists because two rounds of shaving stack arrays
 * on a fault-count correlation established nothing, and one run of this settles it. */
int mono_wasm_jit_stackprobe = 0;
/* Defined here (not in the HOST_BROWSER block) because mono_wasm_emit_method references it in BOTH the
 * runtime and the cross-compiler build; the env-init lives in mono_wasm_jit_auto_init (HOST_BROWSER). */
#ifndef HOST_BROWSER
/* Cross-compiler (host != wasm) stub. mono_wasm_emit_method bakes &mono_wasm_jit_check_store into the
 * OBJGUARD/STOREGUARD store paths (the STOREM/STOREMI macros), and that code is compiled in BOTH the
 * runtime and the AOT cross build — but the real body (below, in the HOST_BROWSER block) uses wasm-only
 * builtins (__builtin_wasm_memory_size), so it cannot compile for the x64 host. The cross-compiler never
 * calls mono_wasm_emit_method (that runs only when cfg->compile_wasm, the runtime browser JIT), so a
 * never-executed stub here just satisfies the link without perturbing the browser build. */
void mono_wasm_jit_check_store (guint8 *addr, int kind);
void mono_wasm_jit_check_store (guint8 *addr, int kind) { (void) addr; (void) kind; }
#endif
int mono_wasm_jit_missedref = 0;      /* MONO_WASM_JIT_MISSEDREF: diagnostic — log NONREF-classified vregs used as MEMBASE bases / call receivers + their defining opcode, to name an isref-inference gap. Default off. */
/* MONO_WASM_JIT_RAISE_NOGC: treat raising instructions as non-GC-points in clause-free methods, so
 * SLOTLIVE stops forcing every live ref into the GC frame just because a null check sits between its
 * def and its use. See the argument in wj_ins_is_gcpoint. A LEVEL, not a boolean:
 *   0 — off (default). A raise is an ordinary GC point everywhere.
 *   1 — liveness-only. The exemption applies to THIS method's own def/use generations (gc_gen ->
 *       needs_slot -> sl_elide) and NOT to method_nogc, effective_gcp_count, prior_gcps_are_polls or
 *       the terminal-vcall handoff, all of which publish "no collection can happen across here" to
 *       OTHER code — a claim a raise falsifies, since the raise allocates. Strictly more honest than
 *       level 2, and MEASURED STILL BROKEN (4/4 dead under a 4 MB nursery). That is how we know the
 *       frame-local half of the argument is the unsound one, not the published half.
 *   2 -- LEVEL 1 WITHOUT THE gen_skipped_raises GUARD, i.e. the exact codegen that measured 4/4 dead
 *        boots under a 4 MB nursery. THIS IS A POSITIVE CONTROL AND NOTHING ELSE. It is unsound by
 *        construction, it is never a shipping candidate, and it exists because a clean result from
 *        level 1 is worthless unless the same harness, the same build and the same session can still
 *        produce the failure on demand -- this bug is intermittent and timing-sensitive enough that
 *        "it stopped failing" is otherwise unfalsifiable.
 *
 *        (The OLD level 2 -- "exempt the PUBLISHED consumers too" -- is gone. It was also unsound, but
 *        it is the WRONG control now: the gen_skipped_raises fix may well have repaired it too, and a
 *        positive control that has itself been fixed proves nothing. This one is defined as the absence
 *        of the fix under test, so it cannot silently become clean.)
 *
 * Default OFF (silent-corruption risk class) -- AND THE PRODUCT SHIPS 1. See the ordering note in
 * wj_ins_is_gcpoint: the fix for the failure this knob is documented to have caused landed AFTER that
 * failure was measured, so `1` is unverified rather than known-good until the matrix in
 * scratchpad/mcsr/gcstress.sh is run. */
int mono_wasm_jit_raise_nogc = 0;
int mono_wasm_jit_refverify = 0;      /* MONO_WASM_JIT_REFVERIFY (0/1/2): after the isref fixpoint, cross-check classification against the structural vreg_is_ref/vreg_is_mp marking — 1 logs violations (a marked vreg classified nonref = lost seed = would-be silent corruption), 2 asserts. Debug only, default off. */
const char *mono_wasm_jit_dump_ir = NULL;  /* MONO_WASM_JIT_DUMP_IR=<substr>: dump clauses + bb regions + opcode stream for clause-bearing methods whose full name contains <substr> (EH-lowering ground truth, e.g. "indigo"). */
/* Island heuristic levers (Part 5), all default-OFF so the baseline is unchanged and each can be A/B'd. */
int mono_wasm_jit_entry_promote = 96;   /* SHIPPED DEFAULT. Tuned on Minecraft 1.16.1 + Fabric + Sodium/Lithium under IKVM, 2026-08/09, plateau protocol. A number without a workload is not a result -- this one has one, and it is not necessarily right for anything else. */
/* Lever A: interp->JIT crossings before the CALLER is queued for upward island growth. 0 disabled it entirely. */   /* Lever A: MONO_WASM_JIT_ENTRY_PROMOTE=N — after a hot interp caller invokes JITted callees N times, force-JIT the caller (grow the island UPWARD). 0 = off. */
int mono_wasm_jit_profile_fast = 0;    /* MONO_WASM_JIT_PROFILE_FAST=1 — emit inline volume counters into the fast dispatch paths (INLINE_AOT direct, inline f-slot IC hit, inline AOT-IC hit) which otherwise call no counting helper. Adds hot-path overhead, so OFF by default (only for a dedicated cost-attribution run). Feeds WJC_FAST_*. */
int mono_wasm_jit_promotion_drain = 8; /* MONO_WASM_JIT_PROMOTION_DRAIN — max queued promotions (Lever A callers, block-promote callees, and woken waiters) drained per safe point. */

static gboolean
wj_method_raise_exempt (MonoCompile *cfg)
{
	return mono_wasm_jit_raise_nogc && cfg->header->num_clauses == 0;
}

/* TRUE if `name` is in the comma-separated MONO_WASM_JIT_METHOD list (bring-up targeting). */
gboolean
mono_wasm_jit_name_targeted (const char *name)
{
	const char *t = g_getenv ("MONO_WASM_JIT_METHOD");
	const char *p;
	size_t nl;
	if (!t || !name)
		return FALSE;
	nl = strlen (name);
	for (p = t; *p; ) {
		const char *c = strchr (p, ',');
		size_t seg = c ? (size_t) (c - p) : strlen (p);
		if (seg == nl && !strncmp (p, name, nl))
			return TRUE;
		if (!c)
			break;
		p = c + 1;
	}
	return FALSE;
}

/* Bisection knob: TRUE if `name` (a residual callee's simple name) is in the comma-separated
 * MONO_WASM_JIT_RESIDUAL_SKIP list, so that residual is bailed to the interpreter. Lets us pin which
 * specific residual callee corrupts real code WITHOUT a rebuild — just set the env var and re-run,
 * narrowing the list by halves. Read at emit time (once per JITted method), so it's cheap. */
gboolean
mono_wasm_jit_residual_name_skipped (const char *name)
{
	const char *t = g_getenv ("MONO_WASM_JIT_RESIDUAL_SKIP");
	const char *p;
	size_t nl;
	if (!t || !name)
		return FALSE;
	nl = strlen (name);
	for (p = t; *p; ) {
		const char *c = strchr (p, ',');
		size_t seg = c ? (size_t) (c - p) : strlen (p);
		if (seg == nl && !strncmp (p, name, nl))
			return TRUE;
		if (!c)
			break;
		p = c + 1;
	}
	return FALSE;
}

/* Coverage-stable bisection denylist: TRUE if `name` (a method's simple name) is in the comma-separated
 * MONO_WASM_JIT_NO_METHOD list, so the auto-JIT trigger leaves it in the interpreter. Lets us pin which
 * JITted method computes a wrong value WITHOUT turning auto-JIT off (which perturbs startup) — deny
 * halves of the registered set and re-run.
 *
 * Simple names alone cannot finish a bisection when the suspect set shares one name: this codebase has 187
 * distinct `accept` methods (the visitor pattern, plus every IKVM-generated functional-interface bridge), so
 * "deny accept" is all-or-nothing and the search stops exactly where it needs to get finer. Accept
 * class-qualified segments too -- "Class:name" or "Ns.Class:name" -- which makes the set bisectable down to a
 * single method. Segments without a ':' keep the original simple-name meaning, so existing denylists behave
 * identically. */
gboolean
mono_wasm_jit_method_denied (MonoMethod *m)
{
	const char *t = g_getenv ("MONO_WASM_JIT_NO_METHOD");
	const char *p;
	const char *name, *kn, *ns;
	if (!t || !m || !m->name)
		return FALSE;
	name = m->name;
	kn = m->klass ? m_class_get_name (m->klass) : NULL;
	ns = m->klass ? m_class_get_name_space (m->klass) : NULL;
	for (p = t; *p; ) {
		const char *c = strchr (p, ',');
		size_t seg = c ? (size_t) (c - p) : strlen (p);
		const char *colon = memchr (p, ':', seg);
		if (!colon) {
			/* simple name, original behaviour */
			if (seg == strlen (name) && !strncmp (p, name, seg))
				return TRUE;
		} else {
			size_t clen = (size_t) (colon - p);
			const char *mp = colon + 1;
			size_t mlen = seg - clen - 1;
			int want_argc = -1;
			/* Optional "/N" arity suffix: "Class:accept/3" denies only the 3-parameter overload. Needed
			 * because class+name cannot separate overloads, and ClassReader:accept is TWO methods -- a
			 * 931-byte forwarder and the 18 KB class parser -- which a class-qualified segment denies
			 * together. A '/' here is unambiguous: nested classes put their '/' in the CLASS part, before
			 * the colon. */
			{
				const char *slash = memchr (mp, '/', mlen);
				if (slash) {
					want_argc = atoi (slash + 1);
					mlen = (size_t) (slash - mp);
				}
			}
			if (want_argc >= 0) {
				MonoMethodSignature *sig = mono_method_signature_internal (m);
				if (!sig || sig->param_count != want_argc) { if (!c) break; p = c + 1; continue; }
			}
			if (mlen == strlen (name) && !strncmp (mp, name, mlen)) {
				gboolean hit = FALSE;
				/* match the class part against either "Class" or "Ns.Class" */
				if (kn && clen == strlen (kn) && !strncmp (p, kn, clen))
					hit = TRUE;
				if (!hit && kn && ns && *ns) {
					size_t want = strlen (ns) + 1 + strlen (kn);
					if (clen == want && !strncmp (p, ns, strlen (ns)) &&
					    p [strlen (ns)] == '.' && !strncmp (p + strlen (ns) + 1, kn, strlen (kn)))
						hit = TRUE;
				}
				if (hit) {
					/* Announce it. A qualified segment that matches NOTHING -- one typo in a class name --
					 * denies nothing and the run comes back "clean", which during a bisection reads as
					 * "the culprit is in the other half" and sends the search down the wrong branch. The
					 * tally lets the runner assert that the arm actually removed a method. Capped so a
					 * retrying trigger cannot flood the log. */
					static int shown;
					if (shown < 64) {
						shown++;
						printf ("WASM_JIT_DENY_QUALIFIED %s%s%s:%s\n",
							ns && *ns ? ns : "", ns && *ns ? "." : "", kn ? kn : "?", name);
					}
					return TRUE;
				}
			}
		}
		if (!c)
			break;
		p = c + 1;
	}
	return FALSE;
}

gboolean
mono_wasm_jit_name_denied (const char *name)
{
	const char *t = g_getenv ("MONO_WASM_JIT_NO_METHOD");
	const char *p;
	size_t nl;
	if (!t || !name)
		return FALSE;
	nl = strlen (name);
	for (p = t; *p; ) {
		const char *c = strchr (p, ',');
		size_t seg = c ? (size_t) (c - p) : strlen (p);
		if (seg == nl && !strncmp (p, name, nl))
			return TRUE;
		if (!c)
			break;
		p = c + 1;
	}
	return FALSE;
}

/* REF-SAFETY DIAGNOSTIC: TRUE if `name` is in the comma-separated MONO_WASM_JIT_REFDIAG list. For each
 * such method the emitter dumps every pointer vreg used as a load/store base that the isref pass left
 * UNclassified, with its defining opcode — pinning the ref-producing op the classifier misses (the
 * GC-unsafe local that gets collected/moved across a GC point -> stray store -> heap corruption). */
gboolean
mono_wasm_jit_refdiag_name (const char *name)
{
	const char *t = g_getenv ("MONO_WASM_JIT_REFDIAG");
	const char *p;
	size_t nl;
	if (!t || !name)
		return FALSE;
	nl = strlen (name);
	for (p = t; *p; ) {
		const char *c = strchr (p, ',');
		size_t seg = c ? (size_t) (c - p) : strlen (p);
		if (seg == nl && !strncmp (p, name, nl))
			return TRUE;
		if (!c)
			break;
		p = c + 1;
	}
	return FALSE;
}

/* TRUE if MONO_WASM_JIT_LLVMONLY is set: compile targeted methods with cfg->llvm_only so the
 * front-end emits the llvmonly indirect-dispatch IR (ftndesc-based virtual/interp calls) that the
 * wasm backend can lower, instead of the normal vtable-slot dispatch (which on wasm calls a
 * fixed-signature trampoline → call_indirect signature mismatch). Trade-off: direct calls also go
 * through ftndescs (interp-entry) rather than the Phase-2 direct f-slot — correct, slightly slower.
 * Gated so the direct-call f-slot path remains the default. */
gboolean
mono_wasm_jit_llvmonly_enabled (void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = g_getenv ("MONO_WASM_JIT_LLVMONLY");
		cached = (e && *e && *e != '0') ? 1 : 0;
	}
	return cached;
}

/* Phase 5 (mini-wasm-lazy.inc, included at the end of this file). The first two compile in both builds. */
static gboolean wj_lazy_is_pool_slot (int slot);
static gboolean wj_lazy_dep_needs_real (int slot);
#ifdef HOST_BROWSER
static gboolean wj_lazy_dep_ok (int slot);
static void wj_lazy_ensure_bank_of_slot (int slot);
static gboolean wj_lazy_pool_ft (int slot, WasmFuncType *out);
static const char *wj_lazy_refusal (MonoMethod *m, MonoMethodSignature *csig, gboolean rgctx, const WasmFuncType *ct, int *counter);
static int wj_lazy_reserve (MonoMethod *m, MonoMethodSignature *csig, gboolean rgctx, const WasmFuncType *ct);
static int wj_lazy_arm_fslot (MonoMethod *target, MonoMethod *self, const WasmFuncType *ftd);
#endif

#ifdef HOST_BROWSER
#include "mini-wasm-diagnostics.inc"
/* Per-thread record of which function-table slots THIS thread actually instantiated (instantiate_local
 * returned 1). The wasm function table is per-thread for dynamic entries, and the jiterpreter PREFILLS
 * every JitCall slot with a non-null placeholder (mono_jiterp_placeholder_jit_call, signature
 * (i32,i32,i32,i32)->void). So a slot can be non-null yet NOT hold this method's real export on this
 * thread — e.g. instantiate_local failed here (OOM/CompileError under memory pressure) while it succeeded
 * on the compiling thread. call_indirect-ing such a slot is a signature-mismatch wasm TRAP that kills the
 * worker. The interp invoke paths consult mono_wasm_jit_slot_live() and fall back to the interpreter when
 * the slot isn't live on this thread, instead of trapping. */
static __thread guint8 *wj_slot_live = NULL;
static __thread int wj_slot_live_cap = 0;   /* capacity, in slots */
static __thread guint8 *wj_slot_installed = NULL;
static __thread int wj_slot_installed_cap = 0;

static int wj_slot_is_installed (int slot);   /* defined below; used by the guard in wj_mark_slot_live */

static void
wj_mark_slot_live (int slot)
{
	/* NEVER PUBLISH A SLOT THIS THREAD DID NOT WRITE. Two different questions shared this bitmap:
	 * `wj_slot_installed` is "did THIS thread write this table slot" (set only inside
	 * instantiate_local / instantiate_batch_local, i.e. only after a real write), while liveness is
	 * "is it safe to dispatch". mono_wasm_jit_admit publishes the latter SEPARATELY from any write, so
	 * any admit path that skips instantiation publishes a slot still holding the jiterpreter prefill --
	 * observed as `live=1 probe=2 arity=4` (R150). Gating the sibling loop reduced that but did not
	 * close it, because the invariant was maintained at each publisher rather than at the bitmap.
	 *
	 * Enforce it HERE, not in mono_wasm_jit_slot_live: emitted code reads this bitmap directly through
	 * mono_wasm_jit_slot_live_ptr_addr, so a check inside the C accessor would miss the JITted fast path
	 * entirely. Refusing at the publisher keeps the one bitmap honest for every consumer, C or emitted.
	 * Legitimate callers are unaffected: admit marks re->e/re->f only after instantiate_*_local, which
	 * marks installed for every member it writes. */
	if (!wj_slot_is_installed (slot))
		return;
	if (slot <= 0)
		return;
	if (G_UNLIKELY (slot >= wj_slot_live_cap)) {
		int oldbytes = (wj_slot_live_cap + 7) / 8;
		int ncap = wj_slot_live_cap ? wj_slot_live_cap : 1024;
		int nbytes;
		while (slot >= ncap)
			ncap *= 2;
		nbytes = (ncap + 7) / 8;
		wj_slot_live = (guint8 *) g_realloc (wj_slot_live, nbytes);
		memset (wj_slot_live + oldbytes, 0, nbytes - oldbytes);
		wj_slot_live_cap = ncap;
	}
	wj_slot_live [slot >> 3] |= (guint8) (1u << (slot & 7));
}

/* Clear THIS thread's record that it wrote a table slot, so mono_wasm_jit_admit re-instantiates it here.
 * The counterpart of wj_mark_slot_installed, and the mechanism that lets a rolled-back group be undone on
 * the worker that formed it WITHOUT publishing a new generation and dragging every other worker into a
 * re-admission it has no reason to do. Liveness goes with it: wj_mark_slot_live refuses to publish a slot
 * that is not installed, so clearing installed cannot leave a stale live bit behind. */
static void
wj_clear_slot_installed (int slot)
{
	if (slot <= 0 || slot >= wj_slot_installed_cap)
		return;
	wj_slot_installed [slot >> 3] &= (guint8) ~(1u << (slot & 7));
	if (slot < wj_slot_live_cap)
		wj_slot_live [slot >> 3] &= (guint8) ~(1u << (slot & 7));
}

static void
wj_mark_slot_installed (int slot)
{
	if (slot <= 0)
		return;
	if (G_UNLIKELY (slot >= wj_slot_installed_cap)) {
		int oldbytes = (wj_slot_installed_cap + 7) / 8;
		int ncap = wj_slot_installed_cap ? wj_slot_installed_cap : 1024;
		int nbytes;
		while (slot >= ncap) ncap *= 2;
		nbytes = (ncap + 7) / 8;
		wj_slot_installed = (guint8 *)g_realloc (wj_slot_installed, nbytes);
		memset (wj_slot_installed + oldbytes, 0, nbytes - oldbytes);
		wj_slot_installed_cap = ncap;
	}
	wj_slot_installed [slot >> 3] |= (guint8)(1u << (slot & 7));
}

static int
wj_slot_is_installed (int slot)
{
	return slot > 0 && slot < wj_slot_installed_cap &&
		((wj_slot_installed [slot >> 3] >> (slot & 7)) & 1);
}

/* TRUE iff THIS thread successfully instantiated the module owning `slot` (so call_indirect-ing it is
 * safe). Read by the interp invoke paths + the vcall f-slot resolver in interp.c, and imported by the
 * jiterpreter's interp-entry trampoline (hence exported to JS) to guard its direct-forward fast path. */
WJ_KEEPALIVE int
mono_wasm_jit_slot_live (int slot)
{
	if (slot <= 0 || slot >= wj_slot_live_cap)
		return 0;
	return (wj_slot_live [slot >> 3] >> (slot & 7)) & 1;
}

/* Addresses of the per-thread liveness bitmap pointer + capacity, for the emitter's INLINE liveness check on
 * the vcall f-slot IC hot path (replaces the per-hit mono_wasm_jit_slot_live call_indirect — the profiled
 * ~1.3M/frame boundary). The addresses of these __thread vars are STABLE per thread (fixed TLS offset), so a
 * JITted method fetches them ONCE in its prologue and caches them in locals; each dispatch then re-loads the
 * CURRENT bitmap pointer + cap THROUGH the cached address, so a realloc-on-grow (wj_mark_slot_live) is picked
 * up transparently — the var's value moves, its address doesn't. Per-thread, so no cross-thread race, and no
 * stale-pointer window (we never cache the bitmap pointer itself, only the address of the slot holding it). */
WJ_KEEPALIVE guint8 **
mono_wasm_jit_slot_live_ptr_addr (void)
{
	return &wj_slot_live;
}
WJ_KEEPALIVE int *
mono_wasm_jit_slot_live_cap_addr (void)
{
	return &wj_slot_live_cap;
}

/*
 * A JS WORKER OUTLIVES THE PTHREADS IT HOSTS, and everything this file calls "per-thread" about the function
 * table is really per-WORKER. When a pthread exits, emscripten returns its worker to the pool
 * (returnWorkerToPool) and frees the pthread's block -- struct, TLS and stack -- in
 * __emscripten_thread_free_data; the next pthread_create can hand that same worker, with its wasm instance,
 * its table and its JS realm intact, to a new pthread with a NEW TLS block. A run takes up ~60-80 pthreads
 * on a 16-worker pool (`worker_slots`), so this is routine, not a corner case.
 *
 * Every JIT instance imports the INSTANTIATING pthread's __thread addresses (s.l/c/v/n/d/m/b/i -- the
 * slot-live bitmap, both PICs, the scratch buffer, the current island), and the jiterpreter's guarded
 * interp-entry trampoline bakes &wj_slot_live / &wj_slot_live_cap as constants. After a take-up the new
 * pthread's own state starts empty -- wj_slot_live is __thread -- so anything it ADMITS is re-instantiated
 * correctly. What it can still reach without admitting anything is the old pthread's trampolines and
 * guard-free adapters, installed at interp-entry indices that AOT code calls directly: they run the old
 * instances against FREED TLS, i.e. read and write whatever now occupies it. That is R292's OOB (the first
 * `*s.i` store in a method entered AOT -> trampoline -> JIT, on a ForkJoin worker) and, before the freed
 * block is reused, a silent write into dead memory.
 *
 * Called from JS (jiterpreter-interp-entry.ts) as each pthread is set up on a worker, only when the worker
 * holds such state. Counts it, and returns whether to revert it. The guarded trampoline reads the current
 * pthread's bitmap through a per-worker cell the JS side re-points first, so it stays; guard-free adapters
 * are demoted to it (their premise was the OLD pthread's liveness); e/f slots go back to the placeholder,
 * exactly what the new pthread's empty bitmaps already say they hold. Reverting the trampolines to the
 * generic C entry instead (R293's first version) stranded taken-up threads on the C boundary (R293b).
 */
WJ_KEEPALIVE int
mono_wasm_jit_worker_reuse (int tramps, int adapters, int slots)
{
	/* NO printf HERE: this runs inside emscripten's threadInitTLS, before the new pthread has started, and stdout
	 * may be proxied to the main thread. The counters carry everything the first eight prints did (R293h). */
	mono_wasm_jit_counters [WJC_REUSE_EVENTS]++;
	mono_wasm_jit_counters [WJC_REUSE_TRAMP] += tramps;
	mono_wasm_jit_counters [WJC_REUSE_ADAPTER] += adapters;
	mono_wasm_jit_counters [WJC_REUSE_SLOT] += slots;
	return mono_wasm_jit_reuse_reset;
}

/* Instantiate a cached JITted module into the CURRENT thread's wasm function table. The table is
 * per-thread for dynamically-added entries, so each thread must do this once (lazily, on its first
 * invoke of the method — interp.c MINT_CALL) before call_indirect-ing the slot. Returns 1 on success,
 * 0 on failure (caller then disables the JIT for the method → interpreter fallback). */
int
mono_wasm_jit_instantiate_local (int e_slot, int f_slot, const void *bytes, int len, char *errbuf, int errcap, double *out_ms)
{
	int _ok;
	extern gpointer *mono_wasm_jit_vcall_pic_ptr_addr (void);
	extern gint32 *mono_wasm_jit_vcall_pic_cap_addr (void);
	extern gpointer *mono_wasm_jit_delegate_pic_ptr_addr (void);
	extern gint32 *mono_wasm_jit_delegate_pic_cap_addr (void);
	extern gpointer mono_wasm_jit_scratch (void);
	extern gpointer mono_wasm_jit_cur_island_il_state_addr (void);
	if (f_slot > 0)
		wj_lazy_ensure_bank_of_slot (f_slot);   /* Phase 5: a pool slot's bank goes in before its real f, never after */
	/* $2/$4/$6 below are pointers passed as 32-bit ints; a g_malloc buffer above 2GB (MC's heap grows past
	 * it) arrives NEGATIVE in JS, and HEAPU8.slice(negative,..) reads the wrong region -> garbage module
	 * bytes -> a magic-word CompileError. Re-add 2^32 to recover the real unsigned address. (Use a C-valid
	 * ternary, NOT JS >>>, since clang parses the EM_ASM body tokens and >>> is not a C operator.)
	 * out_ms ($6, an 8-byte-aligned double*) receives the WebAssembly.Module+Instance compile time in ms
	 * (Part 2 instantiation timing) — measured on both the success and CompileError paths; 0/NULL skips it. */
	WJ_JS_BLOCKING_BEGIN (__wj_cookie, __wj_sd, __wj_entered);
	_ok = EM_ASM_INT ({
		var p = $2 < 0 ? $2 + 4294967296 : $2;
		var eb = $4 < 0 ? $4 + 4294967296 : $4;
		var op = $6 < 0 ? $6 + 4294967296 : $6;
		var t0 = performance.now ();
		try {
			var b = HEAPU8.slice (p, p + $3);
			/* Resolver for the direct-call helper imports the emitter declares as ("h", "<table index>").
			 * A C function pointer under wasm IS a table index, so wasmTable.get turns the index the emitter
			 * baked into the callable the import needs — no linker --export and no fixed helper registry.
			 * A Proxy rather than enumerating WebAssembly.Module.imports(): instantiation Gets exactly the
			 * names it needs, and the per-worker cache means each helper is wrapped once, not once per module
			 * (there are ~25k JIT'd modules in a session). Non-numeric keys return undefined so that any
			 * incidental property probe cannot hand back a function. */
			if (!Module.__wjHelperImports) {
				/* NO MEMO. It used to cache wasmTable.get per index, per worker, forever, and that is
				 * what broke direct imports.
				 *
				 * The memo was safe while only HELPERS and AOT bodies were imported: those live at table
				 * indices fixed for the life of the process. A METHOD f-slot is not fixed -- it holds the
				 * jiterpreter's placeholder until the callee's module is instantiated ON THIS WORKER --
				 * so a module that imported the slot earlier cached the placeholder, and the entry never
				 * updated. Every later importer of that slot on that worker then failed instantiation
				 * with "imported function does not match the expected type".
				 *
				 * Invalidating at each write was tried first and is NOT enough: this table has other
				 * writers, in TypeScript, that this file cannot see -- addWasmFunctionPointer and the
				 * interp-entry patch/unpatch pair in jiterpreter-interp-entry.ts. MEASURED: with
				 * invalidation at both instantiate sites, direct_import still crashed 4/4, and turning
				 * the memo off (the since-removed MONO_WASM_JIT_IMPORT_CACHE=0) fixed it. A cache whose
				 * correctness depends on every writer in another module remembering to invalidate it is
				 * not a cache worth having.
				 *
				 * And it buys nothing measurable. The lookup happens once per import, per module, per
				 * worker -- at INSTANTIATION, never on a call path. Whole-session order of magnitude:
				 * ~24k modules x ~3 imports (median; max 30) x ~16 workers, about a million
				 * wasmTable.get calls spread over minutes.
				 *
				 * Two rounds of diagnosis blamed dependency cycles for what this was. See R142. */
				Module.__wjSlotFn = new Map ();
				Module.__wjSlotChanged = 0;
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string") return undefined;
					if (!/^[0-9]+$/.test (k)) return undefined;
					var i = Number (k);
					var f = wasmTable.get (i);
					/* DETECTOR, not a guard. Every slot this worker installed is remembered; if the table
					 * no longer holds what we put there, some OTHER writer has been at it, and an import
					 * bound here would silently be the wrong function. That is the one remaining shape of
					 * the direct-import failure that cannot be seen from C, because the other writers of
					 * this table are in TypeScript. Deterministic per run, unlike the 50%-of-boots crash
					 * it is chasing. */
					if (Module.__wjSlotFn && Module.__wjSlotFn.has (i) && Module.__wjSlotFn.get (i) !== f) {
						Module.__wjSlotChanged = (Module.__wjSlotChanged | 0) + 1;
						if (Module.__wjSlotChanged <= 20)
							console.log ("WASM_JIT_IMPORT_SLOT_CHANGED slot=" + i + " n=" + Module.__wjSlotChanged);
					}
					return f;
				} });
			}
			var inst = new WebAssembly.Instance (new WebAssembly.Module (b), { m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports && wasmExports["__cpp_exception"] }, s: { p: wasmExports && wasmExports["__stack_pointer"], l: $7, c: $8, v: $9, n: $10, d: $11, m: $12, b: $13, i: $14, g: Module._mono_wasm_jit_worker_action_addr () }, h: Module.__wjHelperImports });
			if (op) HEAPF64[op / 8] = performance.now () - t0;
			/* AN F-SLOT ONLY EVER HOLDS A JIT `f` FROM A MODULE THIS EMITTER PRODUCED, and that is a
			 * load-bearing invariant, not an observation: it is what lets a caller bake a functype for
			 * an f-slot call without a runtime kind test (AOT bodies are reached through their own
			 * table indices and their own `at`/`at_ne` types, never through an f-slot), and it is what
			 * makes MONO_WASM_JIT_THREAD_SP's extra parameter safe to add to every f-slot functype at
			 * once. There used to be a second occupant in principle -- wasm_module_interp_thunk framed
			 * a module exporting `t`, a scalar-ABI stand-in that drove an un-JITted callee through the
			 * interpreter, installed by an `e_slot < 0` arm here. It was never wired up: nothing in the
			 * tree ever called the framer, so the arm could not fire and the invariant held by
			 * accident. Both are deleted rather than left as a trap for the next reader to reason
			 * about -- an f-slot whose arity does not match the caller's baked type is the
			 * `function signature mismatch` failure this file has paid for repeatedly. */
			/* R322: slots <= 0 = VALIDATE ONLY -- the module compiled and every import bound, nothing published. */
			if ($0 > 0 && $1 > 0) {
				wasmTable.set ($0, inst.exports.e); /* entry thunk: interp entry */
				wasmTable.set ($1, inst.exports.f); /* scalar method: call_indirect target */
				if (Module.__wjSlotFn) { Module.__wjSlotFn.set ($0, inst.exports.e); Module.__wjSlotFn.set ($1, inst.exports.f); }
			}
			return 1;
		} catch (e) {
			if (op) HEAPF64[op / 8] = performance.now () - t0;
			/* NAME THE EXPORTS THE MODULE ACTUALLY HAS. R269 measured ~20 admissions per run failing here
			 * with `Table.set(): Argument 1 is invalid for table: function-typed object must be null ...`,
			 * which is V8 (wasm-js.cc:2547) saying the VALUE was not a function -- i.e. `inst.exports.e`
			 * was `undefined`, not that the index was out of range. The single-method path looks up "e"/"f";
			 * wasm_module_assemble names them "e<i>"/"f<i>" whenever nexport > 1. So the interesting datum
			 * is which names the module really carries, and guessing at it from the C side is exactly the
			 * classifier mistake this tree keeps paying for. Cheap: only on the failure path. */
			var _ex = "";
			try { _ex = " exports=[" + Object.keys (inst && inst.exports || {}).join (",") + "]"; } catch (e2) {}
			if (eb) stringToUTF8 ("" + e + _ex, eb, $5); /* surface the WebAssembly error to the caller */
			return 0;
		}
	}, e_slot, f_slot, (int) (intptr_t) bytes, len, (int) (intptr_t) errbuf, errcap, (int) (intptr_t) out_ms,
	   (int) (intptr_t) &wj_slot_live, (int) (intptr_t) &wj_slot_live_cap,
	   (int) (intptr_t) mono_wasm_jit_vcall_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_vcall_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_delegate_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_delegate_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_scratch (),
	   (int) (intptr_t) mono_wasm_jit_cur_island_il_state_addr ());
	WJ_JS_BLOCKING_END (__wj_cookie, __wj_sd, __wj_entered);
	if (_ok && e_slot > 0 && f_slot > 0) {   /* R322: a validate-only call installed nothing */
		/* Physical installation is not dispatch admission. A freshly compiled module can have unchecked
		 * direct dependencies that are still placeholders on this thread; only mono_wasm_jit_admit marks
		 * e/f live after recursively admitting the complete closure. */
		wj_mark_slot_installed (e_slot);
		wj_mark_slot_installed (f_slot);
		WJ_CENSUS_NOTE_INSTANTIATE (*out_ms);
	}
	return _ok;
}

/* Instantiate a BATCHED module and install all 2N of its slots at once.
 *
 * The single-method form above looks up exports "e"/"f"; a batched module exports "e<i>"/"f<i>" for each
 * member, so the slot arrays are walked here. One WebAssembly.Instance backs every member — that is the
 * entire point (V8 only inlines within a module), and it also means a member cannot be instantiated on
 * its own: the registry has to route all members of a batch through this one call.
 *
 * e_slots/f_slots are parallel arrays of length n. Returns 1 on success; on failure nothing is installed
 * and errbuf carries the WebAssembly error. */
int mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n, const void *bytes, int len, char *errbuf, int errcap, double *out_ms);
int
mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n, const void *bytes, int len, char *errbuf, int errcap, double *out_ms)
{
	int _ok, i;
	extern gpointer *mono_wasm_jit_vcall_pic_ptr_addr (void);
	extern gint32 *mono_wasm_jit_vcall_pic_cap_addr (void);
	extern gpointer *mono_wasm_jit_delegate_pic_ptr_addr (void);
	extern gint32 *mono_wasm_jit_delegate_pic_cap_addr (void);
	extern gpointer mono_wasm_jit_scratch (void);
	extern gpointer mono_wasm_jit_cur_island_il_state_addr (void);
	/* Same >2GB pointer caveat as mono_wasm_jit_instantiate_local: a g_malloc buffer above 2GB arrives
	 * negative in JS, so re-add 2^32 before slicing.
	 *
	 * clang tokenises this body as C, so it must stay parseable as such: `var x = ...;` at statement
	 * level is tolerated, but a declaration in a for-initialiser (`for (var k = 0; ...)`) is not, and
	 * neither is `>>>`. Hence the hoisted `var` + while loop below. */
	for (i = 0; i < n; ++i)
		if (f_slots [i] > 0)
			wj_lazy_ensure_bank_of_slot (f_slots [i]);   /* Phase 5: as in instantiate_local */
	WJ_JS_BLOCKING_BEGIN (__wj_cookie, __wj_sd, __wj_entered);
	_ok = EM_ASM_INT ({
		var es = $0 < 0 ? $0 + 4294967296 : $0;
		var fs = $1 < 0 ? $1 + 4294967296 : $1;
		var p  = $3 < 0 ? $3 + 4294967296 : $3;
		var eb = $5 < 0 ? $5 + 4294967296 : $5;
		var op = $7 < 0 ? $7 + 4294967296 : $7;
		var t0 = performance.now ();
		try {
			var b = HEAPU8.slice (p, p + $4);
			/* A BATCHED module declares helper imports too -- every module goes through the same framer.
			 * The resolver therefore has to exist HERE as well.
			 *
			 * It used to say `Module.__wjHelperImports || {}` under a comment claiming batched members are
			 * emitted with helper imports off, which stopped being true when the batch framer gained them.
			 * The Proxy is created lazily in mono_wasm_jit_instantiate_local, so on any worker that
			 * instantiated a BATCHED module before it ever instantiated a standalone one, `{}` was supplied
			 * and every `h.<index>` import was missing — a LinkError, i.e. the member silently falls back to
			 * the interpreter and shows up only as a lower `registered` count. That is the exact failure
			 * shape that is indistinguishable from a performance result.
			 *
			 * Same lazy creation, same per-worker cache, so ordering no longer matters. */
			if (!Module.__wjHelperImports) {
				/* NO MEMO. It used to cache wasmTable.get per index, per worker, forever, and that is
				 * what broke direct imports.
				 *
				 * The memo was safe while only HELPERS and AOT bodies were imported: those live at table
				 * indices fixed for the life of the process. A METHOD f-slot is not fixed -- it holds the
				 * jiterpreter's placeholder until the callee's module is instantiated ON THIS WORKER --
				 * so a module that imported the slot earlier cached the placeholder, and the entry never
				 * updated. Every later importer of that slot on that worker then failed instantiation
				 * with "imported function does not match the expected type".
				 *
				 * Invalidating at each write was tried first and is NOT enough: this table has other
				 * writers, in TypeScript, that this file cannot see -- addWasmFunctionPointer and the
				 * interp-entry patch/unpatch pair in jiterpreter-interp-entry.ts. MEASURED: with
				 * invalidation at both instantiate sites, direct_import still crashed 4/4, and turning
				 * the memo off (the since-removed MONO_WASM_JIT_IMPORT_CACHE=0) fixed it. A cache whose
				 * correctness depends on every writer in another module remembering to invalidate it is
				 * not a cache worth having.
				 *
				 * And it buys nothing measurable. The lookup happens once per import, per module, per
				 * worker -- at INSTANTIATION, never on a call path. Whole-session order of magnitude:
				 * ~24k modules x ~3 imports (median; max 30) x ~16 workers, about a million
				 * wasmTable.get calls spread over minutes.
				 *
				 * Two rounds of diagnosis blamed dependency cycles for what this was. See R142. */
				Module.__wjSlotFn = new Map ();
				Module.__wjSlotChanged = 0;
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string") return undefined;
					if (!/^[0-9]+$/.test (k)) return undefined;
					var i = Number (k);
					var f = wasmTable.get (i);
					/* DETECTOR, not a guard. Every slot this worker installed is remembered; if the table
					 * no longer holds what we put there, some OTHER writer has been at it, and an import
					 * bound here would silently be the wrong function. That is the one remaining shape of
					 * the direct-import failure that cannot be seen from C, because the other writers of
					 * this table are in TypeScript. Deterministic per run, unlike the 50%-of-boots crash
					 * it is chasing. */
					if (Module.__wjSlotFn && Module.__wjSlotFn.has (i) && Module.__wjSlotFn.get (i) !== f) {
						Module.__wjSlotChanged = (Module.__wjSlotChanged | 0) + 1;
						if (Module.__wjSlotChanged <= 20)
							console.log ("WASM_JIT_IMPORT_SLOT_CHANGED slot=" + i + " n=" + Module.__wjSlotChanged);
					}
					return f;
				} });
			}
			var inst = new WebAssembly.Instance (new WebAssembly.Module (b), { m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports && wasmExports["__cpp_exception"] }, s: { p: wasmExports && wasmExports["__stack_pointer"], l: $8, c: $9, v: $10, n: $11, d: $12, m: $13, b: $14, i: $15, g: Module._mono_wasm_jit_worker_action_addr () }, h: Module.__wjHelperImports });
			if (op) HEAPF64[op / 8] = performance.now () - t0;
			/* TWO PASSES, and `es / 4` rather than `es >> 2`. Both are the same bug seen twice.
			 *
			 * `>>` is ToInt32 in JS, so a pointer at or above 2^31 -- which g_malloc hands out routinely
			 * once the heap passes 2 GB, and this workload sits at ~2.9 GB in-game -- becomes NEGATIVE,
			 * HEAP32[negative] reads `undefined`, and wasmTable.set(undefined) throws
			 * "Argument 0 must be convertible to a valid number". That is exactly the 341 REBATCH_FAIL +
			 * 38 ADMIT_FAIL measured in one in-world run (coloc-r1.log): they start ~2 minutes in, when
			 * the heap crosses 2 GB, and never stop, while the admission failures start at boot. The
			 * `$0 < 0 ? $0 + 4294967296` normalisation above already exists precisely because pointers
			 * here exceed 2 GB -- and then the shift threw the correction away. Division is exact for a
			 * 4-aligned pointer and, unlike `>>>`, still tokenises as C (see the note above: clang parses
			 * this body as C, so `>>>` and a for-initialiser declaration are both out).
			 *
			 * The single-method path never had this bug because it passes its two slots as SCALARS
			 * ($0/$1) and never indexes HEAP32.
			 *
			 * And the loop is split because the old one installed member-by-member: a throw at member k
			 * left 0..k-1 installed and k..n-1 not, while mono_wasm_jit_rebatch's caller asserts in a
			 * comment that "instantiate_batch_local installs nothing on failure". Resolve every export
			 * and every slot index -- including addressability, since wasmTable.set throws RangeError out
			 * of range -- BEFORE touching the table, so pass 2 cannot fail and the comment is true. */
			var k = 0;
			var ef = null;
			var ff = null;
			var eslot = new Array ($2);
			var fslot = new Array ($2);
			var efn = new Array ($2);
			var ffn = new Array ($2);
			while (k < $2) {
				ef = inst.exports["e" + k];
				ff = inst.exports["f" + k];
				if (!ef || !ff) throw new Error ("batched module missing export e" + k);
				eslot[k] = HEAP32[es / 4 + k];
				fslot[k] = HEAP32[fs / 4 + k];
				if (!(eslot[k] > 0) || !(fslot[k] > 0))
					throw new Error ("batched module bad slot for member " + k + ": e=" + eslot[k] + " f=" + fslot[k]);
				/* Addressability probe. wasmTable.get raises the same RangeError wasmTable.set would, so an
				 * out-of-range index fails HERE, in the pass that installs nothing. Asked of the table
				 * rather than compared against a cached .length because this table is SHARED and another
				 * worker can grow it under us. */
				wasmTable.get (eslot[k]);
				wasmTable.get (fslot[k]);
				efn[k] = ef;
				ffn[k] = ff;
				k = k + 1;
			}
			k = 0;
			while (k < $2) {
				wasmTable.set (eslot[k], efn[k]);
				wasmTable.set (fslot[k], ffn[k]);
				if (Module.__wjSlotFn) { Module.__wjSlotFn.set (eslot[k], efn[k]); Module.__wjSlotFn.set (fslot[k], ffn[k]); }
				k = k + 1;
			}
			return 1;
		} catch (e) {
			if (op) HEAPF64[op / 8] = performance.now () - t0;
			if (eb) stringToUTF8 ("" + e, eb, $6);
			return 0;
		}
	}, (int) (intptr_t) e_slots, (int) (intptr_t) f_slots, n, (int) (intptr_t) bytes, len, (int) (intptr_t) errbuf, errcap, (int) (intptr_t) out_ms,
	   (int) (intptr_t) &wj_slot_live, (int) (intptr_t) &wj_slot_live_cap,
	   (int) (intptr_t) mono_wasm_jit_vcall_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_vcall_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_delegate_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_delegate_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_scratch (),
	   (int) (intptr_t) mono_wasm_jit_cur_island_il_state_addr ());
	WJ_JS_BLOCKING_END (__wj_cookie, __wj_sd, __wj_entered);
	if (_ok) {
		for (i = 0; i < n; i++) {
			wj_mark_slot_installed (e_slots [i]);
			wj_mark_slot_installed (f_slots [i]);
		}
		/* One MODULE, n methods — count it as one instantiation, because the WebAssembly.Instance and
		 * its export JSFunctions (the JSDispatchTable entries the renderer dies allocating) are what a
		 * batch amortizes. Counting n here would hide exactly the win batching is supposed to deliver. */
		WJ_CENSUS_NOTE_INSTANTIATE (*out_ms);
	}
	return _ok;
}

/* Global registry of every JITted method's {slots, cached bytes}. Because the wasm function table is
 * per-thread for dynamic entries, AND JITted methods call each other directly via f-slot call_indirect,
 * every thread that runs any JITted code must have ALL JITted methods instantiated in its own table — not
 * just the ones it can reach. That used to be done by an eager per-thread sweep, mono_wasm_jit_sync_thread;
 * THAT FUNCTION HAD NO CALLERS and is deleted. The job is entirely mono_wasm_jit_admit's: before a worker
 * may enter a method, admission walks its direct-call closure and instantiates each member here. Callees
 * are always registered before callers (the direct-call lowering bails if the callee isn't JITted yet),
 * so walking the closure at admission guarantees a method's f-slot callees are
 * present.
 *
 * CHUNKED + pointer-stable so it never overflows (a big app JITs well past any fixed cap): a FIXED top-level
 * array of chunk pointers (the array itself never moves, so a lock-free reader can't observe a torn base),
 * with chunks g_malloc0'd on demand and never moved/freed. A reader indexing wj_reg_at(i) for i < wj_reg_n
 * always sees a published chunk + a fully written entry: the writer publishes the chunk pointer (barrier)
 * and the entry (barrier) BEFORE bumping wj_reg_n; readers acquire wj_reg_n before dereferencing anything
 * it indexes. (The two readers this used to name, sync_thread and instantiate_fslot, were both dead and
 * are gone; admission is the surviving one.) Appends serialized under the loader lock. */
/* Shared by every member of one batched module (island batching). A batched module exports e<i>/f<i>
 * rather than e/f, and instantiating it installs ALL its members' slots at once — so a member cannot be
 * brought up on its own. Every member's registry entry points here, and whichever member a worker
 * admits first instantiates the whole batch; the others then find their slots already installed (the
 * wj_slot_is_installed guard in mono_wasm_jit_admit) and skip it. Without this a shared blob would
 * build N instances of the same module per worker. */
typedef struct {
	int n;
	int *e;          /* n entry-thunk slots, member order */
	int *f;          /* n method slots */
	int *desc;       /* registry descriptor ids; lets admission mark every sibling generation live */
	void *bytes;     /* the one module; owned here, shared by all members */
	int len;
	guint32 generation;
} WjBatchDesc;

/* An immutable dependency set: the f-slots a body reaches by call_indirect, with the functype hash
 * admission checks against and the callee for diagnostics. Allocated once, never mutated, published by a
 * single pointer store. */
typedef struct {
	int n;
	int *slot;
	guint32 *sig;
	MonoMethod **method;
} WjDepSet;

/* Build one from caller-owned arrays. NULL when there are no deps, so a NULL depset means "no
 * dependencies" and every reader can treat the two identically. */
static WjDepSet *
wj_depset_new (const int *slots, const guint32 *sigs, MonoMethod *const *methods, int n)
{
	WjDepSet *d;
	if (n <= 0)
		return NULL;
	d = g_new0 (WjDepSet, 1);
	d->n = n;
	d->slot = g_new (int, n);
	d->sig = g_new (guint32, n);
	d->method = g_new0 (MonoMethod *, n);
	memcpy (d->slot, slots, sizeof (int) * (gsize) n);
	if (sigs) memcpy (d->sig, sigs, sizeof (guint32) * (gsize) n);
	if (methods) memcpy (d->method, methods, sizeof (MonoMethod *) * (gsize) n);
	return d;
}

/*
 * RECLAIMING SUPERSEDED REGISTRY PAYLOADS.
 *
 * This file's standing rule is "publish before you free, and prefer leaking to freeing": bytes, depsets
 * and batch descriptors are read LOCK-FREE by other workers and nothing recalls a published pointer. That
 * rule is right and is kept -- what is added here is the one thing that makes freeing possible at all,
 * which is knowing when every reader that could be holding the OLD pointer has finished.
 *
 * WHY IT BECAME NECESSARY. Leaking is bounded only while re-publication is rare. Once an IKVM body swap
 * republishes a live descriptor -- ~574 per run, plus re-emission -- each superseded generation strands a
 * module blob, a depset and (on a re-frame) a batch descriptor, and the wasm heap is 4 GiB with the
 * browser already sitting near 2.9 GB.
 *
 * THE SCHEME, and it is the whole of it: a reader brackets the window in which it may hold a registry
 * pointer; a publisher swaps the pointer, then retires the old one; a retired payload is freed once the
 * reader count is observed at zero. That observation is sufficient, and the argument is short -- a reader
 * that entered BEFORE the retire is counted, so zero means all of them have left; a reader that entered
 * AFTER it cannot have acquired the old pointer, because the swap already happened.
 *
 * THREE THINGS THE FIRST VERSION GOT WRONG, all of the same shape -- the bracket did not cover the readers.
 *
 *   1. It wrapped mono_wasm_jit_admit and nothing else, while re->depset is acquired and held across a
 *      full wj_assemble and a JS module compile by mono_wasm_jit_colocate_deps_now, walked for ARBITRARY
 *      other descriptors by wj_asm_reaches, read by mono_wasm_jit_rendezvous's group expansion, and read
 *      by the dep-graph dumps on the main thread. None of those was bracketed, and no retire site is
 *      itself a reader -- so the usual case was "retire, observe zero, free immediately", i.e. no grace
 *      period at all. Every such site is bracketed below; adding a new reader means adding a bracket.
 *   2. It counted PER RECURSION. The admission DFS re-enters through mono_wasm_jit_admit at every
 *      dependency edge, so a contended atomic and two full fences were paid per node of every closure
 *      walk, on twelve workers sharing one line, on the dispatch path. The depth counter is __thread and
 *      only the outermost entry touches the shared word.
 *   3. It reclaimed from the reader's EXIT, unconditionally -- a global spinlock on the dispatch path.
 *      The exit now reads one plain word and does nothing at all in the steady state, because retires
 *      happen during compilation and dispatch is not compiling.
 *
 * WHAT IS STILL TRUE: one worker parked inside the bracket (a JS module compile, a GC-safe region, a JSPI
 * suspension) holds the count non-zero and defers reclamation of everything. That is a burst, not a leak,
 * but it is unbounded in principle -- so the depth and its high-water mark are COUNTED. A guard whose
 * bound is asserted rather than read is not a bound.
 */
typedef enum {
	WJ_RETIRED_BYTES,
	WJ_RETIRED_DEPSET,
	WJ_RETIRED_BODY,
	WJ_RETIRED_BATCH
} WjRetiredKind;

typedef struct _WjRetiredPayload WjRetiredPayload;
struct _WjRetiredPayload {
	gpointer ptr;
	WjRetiredKind kind;
	WjRetiredPayload *next;
};

static volatile gint32 wj_reg_readers;        /* threads that may be holding a retireable pointer */
static volatile gint32 wj_retired_lock;
static volatile gint32 wj_retired_pending;    /* plain read on the reader-exit fast path */
static WjRetiredPayload *wj_retired_payloads;
static gint32 wj_retired_depth;               /* under wj_retired_lock */
static __thread int wj_reg_read_depth;

/*
 * THE YIELD IN THIS LOOP DID NOTHING. mono_thread_info_yield() -> mono_threads_platform_yield(), which on
 * wasm is `{ return TRUE; }` (mono-threads-wasm.c:169-172). So what read as "spin politely" was a bare,
 * unbounded, uncounted CAS hammer on one shared word, from every worker that reaches the reader bracket.
 *
 * THREE CHANGES, and only the first is a performance fix:
 *
 * 1. TEST-AND-TEST-AND-SET. The old loop issued a WRITE (cmpxchg) on every attempt, so N spinners
 *    ping-ponged the cache line N ways and each acquisition was slower the more threads wanted it.
 *    Spinning on a plain LOAD keeps the line shared until it actually looks free. This is the standard
 *    fix and it needs no new primitive, which matters here: the obvious ones are all unsafe. Anything
 *    that blocks properly -- mono_thread_info_sleep(ms>0) -- does MONO_ENTER_GC_SAFE, and *leaving* a
 *    GC-safe region is itself one of the rendezvous-drain call sites (mono-threads-coop.c:435), so
 *    sleeping here would re-enter the drain from inside a lock the drain can want.
 *
 * 2. IT IS COUNTED. See WJC_RETIRE_SPIN_MAX. The count is the entire reason to touch this at all: a
 *    hung run can now say whether this lock is where its cores went, and R275's hang could not.
 *
 * 3. IT IS BOUNDED -- as a DIAGNOSTIC, not as an escape. Past the bound it keeps spinning, because there
 *    is nothing safe to do instead and the critical sections here hold no lock, allocate nothing and
 *    reach no safepoint, so a holder always makes progress. Exceeding the bound therefore means an
 *    invariant is broken rather than that the lock is busy, and the counter is how that becomes visible
 *    instead of presenting as a frozen tier.
 */
#define WJ_RETIRED_SPIN_NOISY 10000

static void
wj_retired_enter (void)
{
	gint64 n = 0;
	for (;;) {
		/* Read first; only attempt the write when it looks free. */
		if (mono_atomic_load_i32 (&wj_retired_lock) == 0 &&
		    mono_atomic_cas_i32 (&wj_retired_lock, 1, 0) == 0)
			break;
		++n;
	}
	if (n) {
		mono_wasm_jit_counters [WJC_RETIRE_SPINS] += n;
		if (n > mono_wasm_jit_counters [WJC_RETIRE_SPIN_MAX])
			mono_wasm_jit_counters [WJC_RETIRE_SPIN_MAX] = n;
	}
}

/*
 * Opportunistic acquisition, for a caller that has something better to do than wait. RECLAMATION IS
 * ALWAYS OPTIONAL: if another thread holds this lock it is either retiring (and will reclaim on its way
 * out) or already reclaiming, so failing here loses nothing but a few bytes held a little longer -- and
 * this tree's rule is to prefer leaking to freeing. Keeping the reader-exit path off the spin entirely is
 * worth far more than the reclaim, because that path runs at dispatch rate on every worker.
 */
static gboolean
wj_retired_tryenter (void)
{
	if (mono_atomic_load_i32 (&wj_retired_lock) == 0 &&
	    mono_atomic_cas_i32 (&wj_retired_lock, 1, 0) == 0)
		return TRUE;
	mono_wasm_jit_counters [WJC_RETIRE_TRYLOCK_MISS]++;
	return FALSE;
}

static void
wj_retired_leave (void)
{
	mono_atomic_store_i32 (&wj_retired_lock, 0);
}

static void
wj_depset_free (WjDepSet *d)
{
	if (!d)
		return;
	g_free (d->slot);
	g_free (d->sig);
	g_free (d->method);
	g_free (d);
}

struct _WjBody;   /* file scope, so the prototype below is not a fresh tag with prototype scope */
static void wj_body_free (struct _WjBody *b);   /* mini-wasm-ir.inc; the typedef is not visible yet */

static void
wj_batchdesc_free (WjBatchDesc *bd)
{
	if (!bd)
		return;
	g_free (bd->e);
	g_free (bd->f);
	g_free (bd->desc);
	/* NOT bd->bytes: the module blob is retired separately and deduplicated across the members that
	 * shared it, so freeing it here would be a double free for a group of n > 1. */
	g_free (bd);
}

static void
wj_reclaim_retired (void)
{
	WjRetiredPayload *p, *next;
	int freed = 0;

	if (!wj_retired_pending)
		return;
	{
		extern int mono_wasm_jit_retire_free;
		if (!mono_wasm_jit_retire_free)
			return;   /* MONO_WASM_JIT_RETIRE_FREE=0: keep every retired payload alive (the UAF discriminator) */
	}
	if (mono_atomic_load_i32 (&wj_reg_readers) != 0)
		return;
	/* TRY, do not spin. This is reached from wj_reg_read_leave, i.e. from the exit of every
	 * mono_wasm_jit_admit, on every worker, at dispatch rate -- the last place that should contain an
	 * unbounded busy wait. A miss just defers the free to the next retire or the next reader exit. */
	if (!wj_retired_tryenter ())
		return;
	if (mono_atomic_load_i32 (&wj_reg_readers) != 0) {
		wj_retired_leave ();
		return;
	}
	p = wj_retired_payloads;
	wj_retired_payloads = NULL;
	wj_retired_depth = 0;
	mono_atomic_store_i32 (&wj_retired_pending, 0);
	wj_retired_leave ();
	while (p) {
		next = p->next;
		switch (p->kind) {
		case WJ_RETIRED_DEPSET: wj_depset_free ((WjDepSet *) p->ptr); break;
		case WJ_RETIRED_BODY:   wj_body_free ((struct _WjBody *) p->ptr); break;
		case WJ_RETIRED_BATCH:  wj_batchdesc_free ((WjBatchDesc *) p->ptr); break;
		default:                g_free (p->ptr); break;
		}
		g_free (p);
		p = next;
		++freed;
	}
	mono_wasm_jit_counters [WJC_RETIRE_FREED] += freed;
	mono_wasm_jit_counters [WJC_RETIRE_RECLAIMS]++;
}

static void
wj_retire_payload (gpointer ptr, WjRetiredKind kind)
{
	WjRetiredPayload *p;
	if (!ptr)
		return;
	p = g_new (WjRetiredPayload, 1);
	p->ptr = ptr;
	p->kind = kind;
	wj_retired_enter ();
	p->next = wj_retired_payloads;
	wj_retired_payloads = p;
	++wj_retired_depth;
	if (wj_retired_depth > (gint32) mono_wasm_jit_counters [WJC_RETIRE_DEPTH_MAX])
		mono_wasm_jit_counters [WJC_RETIRE_DEPTH_MAX] = wj_retired_depth;
	mono_atomic_store_i32 (&wj_retired_pending, 1);
	wj_retired_leave ();
	mono_wasm_jit_counters [WJC_RETIRED]++;
	/* A retire site is never itself inside the bracket when it can help it, so this usually reclaims on
	 * the spot; when it cannot, the next reader-exit or the next retire will. */
	wj_reclaim_retired ();
}

/*
 * THE READER BRACKET. Hold it for as long as a registry pointer -- re->bytes, re->depset, re->body,
 * re->batch, or anything reached through them -- may still be dereferenced. Re-entrant: only the outermost
 * entry touches the shared counter, which matters because the admission DFS nests once per dependency.
 */
static void
wj_reg_read_enter (void)
{
	if (wj_reg_read_depth++ == 0) {
		mono_atomic_inc_i32 (&wj_reg_readers);
		mono_memory_barrier ();   /* announce the reader before acquiring any retireable pointer */
	}
}

static void
wj_reg_read_leave (void)
{
	if (--wj_reg_read_depth == 0) {
		mono_memory_barrier ();
		if (mono_atomic_dec_i32 (&wj_reg_readers) == 0 && wj_retired_pending)
			wj_reclaim_retired ();
	}
}

typedef struct {
	int e, f, len;
	int body_len;       /* original single-method module size; retained across generational rebatches */
	void *bytes;
	MonoMethod *body_method;    /* method whose IR was emitted */
	MonoMethod *logical_method; /* wrapper/method whose InterpMethod publishes this descriptor */
	/* Its InterpMethod, resolved ONCE here rather than looked up during admission.
	 *
	 * mono_wasm_jit_admit used to call mono_interp_get_imethod (logical_method) on whatever worker
	 * happened to admit the descriptor, purely to patch the interp entry. That lookup dereferences the
	 * MonoMethod (jit_mm_for_method, mono_method_signature_internal) and touches the memory manager's
	 * interp_code_hash, and it produced BOTH residual boot faults measured in R151: a worker OOB inside
	 * mono_interp_get_imethod (1 boot in 4), and, once a range guard stopped the wildest pointers,
	 * `mono-internal-hash.c:47 table->table != NULL` (2 boots in 6) -- an interp_code_hash that is not
	 * initialised on that worker.
	 *
	 * Registration already runs on a thread where the method is unambiguously live and its memory manager
	 * is set up, so resolve it there and store the result. Admission then just uses the pointer. */
	gpointer logical_imethod;
	guint32 f_sig_id;
	guint8 no_gc;               /* transitive effect: body reaches no returning GC/safepoint */
	guint8 batch_incompatible;  /* force-compile cannot reproduce this descriptor's captured body */
	/* This method was in a group that had to be ROLLED BACK, so never co-locate it again.
	 *
	 * Deliberately NOT reusing batch_incompatible, which answers a different question (whether a
	 * force-compile can reproduce a captured body) and is read by the auto-planner.
	 *
	 * This flag is what re-supplies the brake that rollback removes. `re->batch` being permanent was the
	 * only thing making the partition append-only: clearing it hands the method back to the pool, and
	 * since a group is formed at every publish, a method whose group failed is immediately eligible to be
	 * pulled into the next one -- which fails the same way, rolls back, and repeats. Each turn costs a
	 * wj_assemble, a real WebAssembly.Module compile for the group, and a standalone re-instantiation.
	 * MEASURED: unbounded enough to wedge boot outright (bootcheck TIMEOUT with rollback on and the SCC
	 * guard off; PASS with rollback off and the guard on). */
	guint8 colocate_refused;
	/* ONE POINTER, SWAPPED ATOMICALLY -- not four independent fields.
	 *
	 * These were `int *deps; guint32 *dep_sig; MonoMethod **dep_method; int ndeps;`, republished
	 * separately by mono_wasm_jit_batch_bind while other workers walk them without a lock. No ordering of
	 * four independent stores fixes that: a reader's `for (i = 0; i < re->ndeps; ++i)` re-reads BOTH the
	 * count and the array pointer on every iteration, so it can begin on the old array and end on the new
	 * one. Publishing count-down / pointers / count-up (the previous attempt) narrows the window and does
	 * not close it.
	 *
	 * A single pointer to an immutable set closes it: a reader snapshots `re->depset` once and everything
	 * it then reads belongs to one generation. Superseded sets are deliberately NOT freed -- another
	 * worker may still be walking one -- which is the same policy the individual arrays already had. */
	WjDepSet *depset;
	/* R320: seqlock over {batch, bytes, len, depset}. ODD while a writer is replacing them (wj_pub_begin/_end);
	 * admission snapshots all of them inside one even, unchanged window (wj_pub_snapshot) so it can never install
	 * one generation's bytes over another's dependency closure. Each field alone was already published safely; the
	 * PAIR was not. */
	volatile gint32 pub_seq;
	/* The f-slots this module BINDS AT INSTANTIATION -- its method imports. NULL/0 when none, which is
	 * the default and the whole tier until MONO_WASM_JIT_DIRECT_IMPORT is on.
	 *
	 * The distinction from `deps` is "should already be admitted" versus "MUST already be admitted". A
	 * call_indirect resolves through the table whenever it runs, so admitting its target late is merely
	 * slow; an import is resolved once, while this module instantiates, and a late target is a LinkError
	 * or -- worse, because nothing reports it -- a binding to whatever else is in the slot.
	 *
	 * Stored as its own list rather than as flags over `deps`, deliberately. Flags require the two lists
	 * to agree, and "every imported slot is also a recorded dependency" is an invariant this code should
	 * ENFORCE rather than assume: wj_result_add_direct_dep dedupes and caps, the import decision is taken
	 * much later in the assembler, and a slot that fell out of one list but not the other would silently
	 * skip the admission requirement below. That is exactly the class of bug this whole path has already
	 * produced twice. */
	guint32 *dep_sig;
	MonoMethod **dep_method; /* callee behind each dep f-slot (diagnostics only) */
	WjBatchDesc *batch;      /* non-NULL iff this method shares a module with others */
	guint32 generation;      /* zero for standalone; bumped whenever the slots are rebound */
	/* The RELOCATABLE body this module was framed from -- instruction bytes plus relocations, before any
	 * module-dependent index was resolved. Keeping it is what makes co-location cheap: re-framing this
	 * method together with others is then wj_assemble over the stored bodies, a memcpy pass, instead of
	 * mono_wasm_force_compile per member. `bytes` above is the FRAMED result and is what workers
	 * instantiate; this is what it was framed from.
	 *
	 * Costs roughly the module minus its framing (median module 780 wire bytes) plus the relocation array,
	 * retained for the life of the process, against a re-compile that costs the whole mono front end. */
	struct _WjBody *body;
	/* THIS DESCRIPTOR NO LONGER OWNS ITS f-SLOT. Set when a re-registration of the same method takes the
	 * slot over (see the FSLOT_REREGISTER arm). The entry stays in the registry -- nothing is ever freed
	 * here, lock-free readers hold pointers into it -- but it must never be admitted again: instantiating
	 * its module would write the OLD body into slots that now belong to the new descriptor.
	 *
	 * Without this, workers that had already admitted the old descriptor keep wj_desc_state == 2 with a
	 * matching generation forever, because the new generation lives on a DIFFERENT registry entry. They
	 * then dispatch into a slot whose current owner they never installed -- the jiterpreter prefill --
	 * and the emitted call_indirect traps. That is R267 addendum 10's root cause. */
	guint8 orphaned;
} WjRegEntry;

/* R320: writers of an entry's {batch, bytes, len, depset} bracket the stores with these (see WjRegEntry.pub_seq). */
static inline void
wj_pub_begin (WjRegEntry *re)
{
	mono_atomic_inc_i32 (&re->pub_seq);
	mono_memory_barrier ();
}

static inline void
wj_pub_end (WjRegEntry *re)
{
	mono_memory_barrier ();
	mono_atomic_inc_i32 (&re->pub_seq);
}

/* R320: one consistent reading of {batch, bytes, len, depset}, or FALSE if a writer was mid-replacement -- a
 * TRANSIENT condition (it clears when the writer's wj_pub_end lands), so the caller refuses with state 0, never 3. */
static gboolean
wj_pub_snapshot (WjRegEntry *re, WjBatchDesc **batch, void **bytes, int *len, WjDepSet **ds)
{
	gint32 s1 = mono_atomic_load_i32 (&re->pub_seq);
	if (s1 & 1)
		return FALSE;
	mono_memory_barrier ();
	*batch = re->batch;
	*bytes = re->bytes;
	*len = re->len;
	*ds = re->depset;
	mono_memory_barrier ();
	return mono_atomic_load_i32 (&re->pub_seq) == s1;
}
#define WJ_REG_CHUNK   8192
#define WJ_REG_NCHUNKS 1024      /* up to 8M JITted methods; the 4KB top-level pointer array never moves */
static WjRegEntry *wj_reg_chunks [WJ_REG_NCHUNKS];
static volatile int wj_reg_n = 0;
static volatile gint32 wj_batch_generation;
#define WJ_SLOT_CHUNK 8192
#define WJ_SLOT_NCHUNKS 1024
static gint32 *wj_fslot_desc_chunks [WJ_SLOT_NCHUNKS]; /* f-slot -> descriptor id (registry index + 1) */
/* serialize registry appends + per-thread sync; the loader lock is global + always inited at startup */
extern void mono_loader_lock (void);
extern void mono_loader_unlock (void);

/* Entry i of the chunked registry (NULL only if its chunk isn't allocated — never for i < wj_reg_n, since
 * the writer allocates+publishes the chunk before bumping the count). */
static inline WjRegEntry *
wj_reg_at (int i)
{
	WjRegEntry *chunk = wj_reg_chunks [i / WJ_REG_CHUNK];
	return chunk ? &chunk [i % WJ_REG_CHUNK] : NULL;
}

/* R358 (MONO_WASM_JIT_T2_UNIT census): the registered wire length of descriptor `desc` (1-based), 0 if none. */
int mono_wasm_jit_desc_body_len (int desc);
int
mono_wasm_jit_desc_body_len (int desc)
{
	int len = 0;
	WjRegEntry *re;
	wj_reg_read_enter ();
	if (desc > 0 && desc <= wj_reg_n && (re = wj_reg_at (desc - 1)))
		len = (int) re->body_len;
	wj_reg_read_leave ();
	return len;
}

int
mono_wasm_jit_register (MonoMethod *method, int e_slot, int f_slot, void *bytes, int len, guint32 f_sig_id, gboolean no_gc, const int *deps, const guint32 *dep_sig, MonoMethod *const *dep_methods, int ndeps)
{
	int desc_id = 0;
	mono_loader_lock ();
	{
		int n = wj_reg_n;
		int ci = n / WJ_REG_CHUNK;
		int fci = f_slot > 0 ? f_slot / WJ_SLOT_CHUNK : -1;
		if (fci >= 0 && fci < WJ_SLOT_NCHUNKS && wj_fslot_desc_chunks [fci] &&
			wj_fslot_desc_chunks [fci][f_slot % WJ_SLOT_CHUNK] != 0) {
			/* THE SLOT IS TAKEN. Refusing is right when it is taken by a DIFFERENT method -- that is the
			 * bug this guard was added for, two descriptors owning one table slot, where the second
			 * export written silently steals the first method's callers.
			 *
			 * It is wrong when the owner is the SAME method, which is exactly and only what a RE-EMIT
			 * produces: the same body recompiled with a matured call profile, deliberately pinned to its
			 * own e/f pair. Reusing the pair is the point -- callers have baked that f-slot, and the new
			 * module exports the same method with the same signature, so replacing what the slot holds is
			 * benign (a call in flight lands on either body and both are correct).
			 *
			 * MEASURED (R174): this guard fired 1,327 times in one run and 4,398 in another, on the
			 * re-emit path alone, and was the last link in the chain that made every re-emit publish
			 * nothing -- registration returned 0, so e_slot stayed 0, so the drain reported NO_PUBLISH.
			 *
			 * Same-method registration therefore updates the existing descriptor below. A distinct method is
			 * still a collision and is refused above; it may not steal this logical method's stable pair. */
			int old_desc = wj_fslot_desc_chunks [fci][f_slot % WJ_SLOT_CHUNK];
			WjRegEntry *old_re = (old_desc > 0 && old_desc <= wj_reg_n) ? wj_reg_at (old_desc - 1) : NULL;
			gboolean same_method = old_re && (old_re->body_method == method || old_re->logical_method == method);
			if (!same_method) {
				printf ("WASM_JIT_FSLOT_COLLISION f=%d old_desc=%d new_method=%s — refusing slot reuse\n",
					f_slot, old_desc, method->name ? method->name : "?");
				mono_loader_unlock ();
				return 0;
			}
			/* A new generation registering onto the f-slot this logical method already owns. */
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_FSLOT_REREGISTER);

			/* REUSE THE DESCRIPTOR. DO NOT MINT A NEW ONE.
			 *
			 * The comment above says replacing what the slot holds is benign, and it is -- but allocating
			 * a FRESH registry entry to do it is not, and that was R267 addendum 10's root cause. The
			 * per-worker admission cache is keyed by DESCRIPTOR ID: `wj_desc_state [d] == 2` with
			 * `wj_desc_generation [d] == re->generation`. Minting a new descriptor leaves every worker
			 * holding a valid-looking admission of the OLD id, whose `re->generation` never moves again
			 * because the new generation lives on a different entry. Those workers dispatch into a slot
			 * whose current owner they never installed -- the jiterpreter prefill -- and trap.
			 *
			 * Updating this entry in place removes the whole problem rather than compensating for it:
			 *   - wj_desc_for_fslot keeps naming this descriptor, so nothing is orphaned;
			 *   - bumping THIS entry's generation is exactly what invalidates every worker's cached
			 *     admission, and the rendezvous already does that proactively;
			 *   - CALLERS NEED NO INVALIDATION AT ALL. They baked the SLOT, and the slot holds a valid
			 *     function of the same signature throughout -- old body or new, both correct. That is why
			 *     a reverse fan-in index is not required here.
			 *
			 * Ownership discipline as everywhere else in this registry: publish the contents, fence, then
			 * publish the word that makes them reachable (the generation). Old bytes/depsets are never
			 * freed -- a worker mid-walk may hold either, and both are self-consistent. */
			if (same_method && old_re && !old_re->batch) {
				void *old_bytes = old_re->bytes;
				WjDepSet *old_depset = old_re->depset;
				WjDepSet *new_depset = wj_depset_new (deps, dep_sig, dep_methods, ndeps);
				wj_pub_begin (old_re);   /* R320: bytes and depset change as ONE unit for admission */
				old_re->bytes = bytes;
				old_re->len = len;
				old_re->body_len = len;
				old_re->f_sig_id = f_sig_id;
				old_re->no_gc = no_gc ? 1 : 0;
				old_re->depset = new_depset;
				wj_pub_end (old_re);
				mono_memory_barrier ();
				old_re->generation = (guint32) mono_atomic_inc_i32 (&wj_batch_generation);
				mono_wasm_jit_counters [WJC_FSLOT_REUSED]++;
				mono_loader_unlock ();
				{
					extern void mono_wasm_jit_repoint_imethod_bytes (gpointer, void *, int);
					mono_wasm_jit_repoint_imethod_bytes (old_re->logical_imethod, bytes, len);
				}
				if (old_bytes != bytes)
					wj_retire_payload (old_bytes, WJ_RETIRED_BYTES);
				if (old_depset != old_re->depset)
					wj_retire_payload (old_depset, WJ_RETIRED_DEPSET);
				return old_desc;
			}
			if (same_method && old_re && old_re->batch) {
				/* A BATCHED MEMBER KEEPS ITS DESCRIPTOR, AND PUBLISHES ALMOST NOTHING HERE.
				 *
				 * The module this entry belongs to is still the GROUP's, and the group still contains the
				 * OLD body: re-framing it is a separate step (mono_wasm_jit_refresh_batch) that runs after
				 * this returns and can fail. So this is the middle of a replacement, and whatever it
				 * writes describes a generation that is not installed anywhere yet.
				 *
				 * THE DEPSET IS THE ONE THAT MATTERS AND IT IS NOT WRITTEN. Admission validates a caller's
				 * closure against it, so publishing generation 2's dependencies over a module that still
				 * contains generation 1 makes admission install the wrong closure while the old body runs
				 * -- a baked call_indirect into an f-slot nothing admitted, i.e. the `function signature
				 * mismatch` class. It would also be pointless on success: mono_wasm_jit_batch_bind
				 * recomputes every member's depset from the ASSEMBLER's own resolution of the module it
				 * installs, which is the only set that describes the code actually running.
				 *
				 * no_gc is forced off rather than updated, because it is a transitive property of the new
				 * body's direct calls and a caller that believes a stale TRUE elides its GC frame. FALSE
				 * is the safe direction and the next full registration restores it.
				 *
				 * body_len and f_sig_id are safe to write: the first is diagnostic, and the second is
				 * derived from the MonoMethod's signature, which no body replacement can change. */
				old_re->body_len = len;
				old_re->f_sig_id = f_sig_id;
				old_re->no_gc = 0;
				mono_memory_barrier ();
				mono_wasm_jit_counters [WJC_FSLOT_REUSED]++;
				mono_loader_unlock ();
				return old_desc;
			}
			/* ORPHAN THE OLD DESCRIPTOR. It keeps its registry entry and its bytes, but it has just lost
			 * the slot, and every worker that admitted it still believes otherwise: wj_desc_state == 2
			 * with a matching wj_desc_generation, because re->generation lives on THIS entry and the new
			 * one is a different entry entirely. Those workers dispatch into a slot whose current owner
			 * they never installed and hit the prefill.
			 *
			 * Bumping the generation is what invalidates the per-worker cache, because the generation
			 * compare is the only signal admit's fast path consults -- and the flag is what stops the
			 * ensuing re-admission from instantiating the old module back over the new owner's slots.
			 * Contents before the pointer/word that publishes them, as everywhere else here. */
			if (old_re && old_desc != (int) (n + 1)) {
				old_re->orphaned = 1;
				mono_memory_barrier ();
				old_re->generation = (guint32) mono_atomic_inc_i32 (&wj_batch_generation);
				mono_wasm_jit_counters [WJC_FSLOT_ORPHANED]++;
			}
		}
		if (ci < WJ_REG_NCHUNKS) {
			WjRegEntry *chunk = wj_reg_chunks [ci];
			if (!chunk) {
				chunk = (WjRegEntry *) g_malloc0 (WJ_REG_CHUNK * sizeof (WjRegEntry));
				mono_memory_barrier ();        /* publish the zeroed chunk before its pointer */
				wj_reg_chunks [ci] = chunk;
			}
			chunk [n % WJ_REG_CHUNK].e = e_slot;
			chunk [n % WJ_REG_CHUNK].f = f_slot;
			chunk [n % WJ_REG_CHUNK].bytes = bytes;
			chunk [n % WJ_REG_CHUNK].len = len;
			chunk [n % WJ_REG_CHUNK].body_len = len;
			chunk [n % WJ_REG_CHUNK].body_method = method;
			chunk [n % WJ_REG_CHUNK].logical_method = method;
			chunk [n % WJ_REG_CHUNK].logical_imethod = method ? mono_interp_get_imethod (method) : NULL;
			chunk [n % WJ_REG_CHUNK].f_sig_id = f_sig_id;
			chunk [n % WJ_REG_CHUNK].no_gc = no_gc ? 1 : 0;
			/* Phase 5: a re-framed batch is installed on the rebatching thread before its admission, which a pool
			 * slot's callers cannot wait for (invariant 1). */
			chunk [n % WJ_REG_CHUNK].colocate_refused = wj_lazy_is_pool_slot (f_slot) ? 1 : 0;
			chunk [n % WJ_REG_CHUNK].depset = wj_depset_new (deps, dep_sig, dep_methods, ndeps);
			/* The imported slots. Written HERE, inside the critical section, and not by a follow-up call
			 * after registration returns -- the barrier and the wj_reg_n bump below are what make this
			 * entry visible to other workers, and an entry seen with no import list is an entry whose
			 * imports admission does not require. It would then allow the cycle-break, instantiate early,
			 * and bind against whatever the table holds: a LinkError if the type differs, and SILENTLY
			 * THE WRONG FUNCTION if it does not. Intermittent by construction, which is the worst kind. */
			if (f_slot > 0 && f_slot / WJ_SLOT_CHUNK < WJ_SLOT_NCHUNKS) {
				int ci2 = f_slot / WJ_SLOT_CHUNK;
				if (!wj_fslot_desc_chunks [ci2])
					wj_fslot_desc_chunks [ci2] = g_new0 (gint32, WJ_SLOT_CHUNK);
				wj_fslot_desc_chunks [ci2][f_slot % WJ_SLOT_CHUNK] = n + 1;
			}
			mono_memory_barrier ();            /* publish the entry before the count */
			wj_reg_n = n + 1;
			desc_id = n + 1;
		} else {
			/* >8M JITted methods (absurd) — the chunk-pointer array is full. The method is JITted
			 * (im->wasm_jit_* set) but NOT in wj_reg, so admission cannot find its bytes and cannot
			 * install it on another worker. The imethod fallback that used to cover this lived in
			 * mono_wasm_jit_ensure_fslot, WHICH HAD NO CALLERS AND IS DELETED -- so past this point a
			 * direct call to such a method traps on a worker that never installed it. Unreachable in
			 * practice at 8M methods; stated rather than implied. Warn once. */
			static int _warned = 0;
			if (!_warned) { _warned = 1; printf ("WASM_JIT_REG_OVERFLOW: >%d JITted methods; not in wj_reg, so admission cannot install it on another worker\n", WJ_REG_NCHUNKS * WJ_REG_CHUNK); }
		}
	}
	mono_loader_unlock ();
	return desc_id;
}

/* Function-table exhaustion, counted separately from the WJC_* stats because those are gated on
 * MONO_WASM_JIT_STATS and this has to be visible in an ordinary run.
 *
 * mono_jiterp_allocate_table_entry (jiterpreter.c) is a bump allocator over a fixed range with NO free:
 * once it is past last_index it returns 0 forever, and mono_wasm_emit_method's `e_slot > 0 && f_slot > 0`
 * guard then just declines to JIT. So the JIT stops silently rather than failing, which presents as "it
 * got slower" and is the worst way for a performance feature to break. Surface it: mono_wasm_jit_liveness(3).
 *
 * Raise the ceiling with --jiterpreter-table-size=N (mono option jiterpreter_table_size, default 6144 in
 * options-def.h; the JIT_CALL table gets that many entries and every JITted method uses two of them).
 * Note jiterpreter_allocate_tables sizes BOTH the trace and JIT_CALL tables from that one option. */
static gint32 wj_table_exhausted;

void mono_wasm_jit_note_table_exhausted (void);
void
mono_wasm_jit_note_table_exhausted (void)
{
	if (mono_atomic_inc_i32 (&wj_table_exhausted) == 1)
		g_printf ("MONO_WASM: wasm JIT function table exhausted - no further methods will be JITted. "
		          "Raise --jiterpreter-table-size.\n");
}

/* Counter-only variant. mono_jiterp_allocate_table_entry knows WHICH table ran dry and which option
 * sizes it, so it prints its own (more specific) line; this exists so that event still shows up in
 * mono_wasm_jit_liveness(3). Previously only the wasm_jit's own slot allocation fed that counter, so
 * an interp-entry table filling up left the probe reading zero while tiering had already stopped. */
void mono_wasm_jit_note_table_exhausted_quiet (void);
void
mono_wasm_jit_note_table_exhausted_quiet (void)
{
	mono_atomic_inc_i32 (&wj_table_exhausted);
}

/* Per-worker instantiation census — MONO_WASM_JIT_ENTRYCENSUS=1, off by default.
 *
 * This census was built when an eager sweep (mono_wasm_jit_sync_thread) was believed to instantiate EVERY
 * registered module on EVERY thread. It had no callers, so it never did, and the census is what would have
 * shown that had its path split been read. What remains is demand-driven admission, and the question the
 * census answers is now how much per-worker DUPLICATION that demand produces. It is not a cosmetic
 * question. Each instantiation is a WebAssembly.Instance whose two
 * exported functions (e and f) are JSFunctions, and V8 charges a JSDispatchTable entry per JSFunction that
 * is reclaimed only on a major GC — so the cost scales as registered_methods x threads. V8 already shares
 * the compiled NativeModule across isolates via its wire-byte cache, which shares the machine code and
 * none of this.
 *
 * "Entered" is counted at the interp->JIT boundary only (mono_wasm_jit_invoke_caught), keyed on e-slot.
 * That deliberately UNDERCOUNTS the demand set: a JITted method reached by a direct call_indirect from
 * another JITted method never crosses that boundary. So entered <= demanded <= instantiated, and the
 * census is sized to answer "is there an order-of-magnitude gap" rather than "which modules exactly".
 * A lazy scheme would still have to serve the JIT->JIT callees, but those already have a demand path
 * (mono_wasm_jit_instantiate_fslot), so they are instantiations that would have happened anyway.
 *
 * Gated because it puts a load+test on the interp->JIT boundary, and anything that costs time inside a
 * measured region is off by default here.
 *
 * The mono_wasm_jit_entry_census flag itself is defined near the other knobs, outside HOST_BROWSER.
 */
#define WJ_CENSUS_SLOTS (1 << 18)   /* e-slots below this are deduplicated; above it each entry counts once */

static __thread int      wj_census_instantiated;   /* modules THIS thread instantiated (any path) */
static __thread int      wj_census_entered;        /* distinct e-slots THIS thread actually entered */
static __thread gint64   wj_census_inst_us;        /* wall-clock THIS thread spent instantiating */
static __thread guint32 *wj_census_seen;           /* dedup bitmap over e-slot numbers */

void
mono_wasm_jit_census_note_entry (int eslot)
{
	guint32 word, bit;
	if (eslot <= 0 || eslot >= WJ_CENSUS_SLOTS) {
		wj_census_entered++;   /* out of bitmap range: count it rather than silently dropping it */
		return;
	}
	if (!wj_census_seen) {
		wj_census_seen = (guint32 *) g_malloc0 ((WJ_CENSUS_SLOTS / 32) * sizeof (guint32));
		if (!wj_census_seen)
			return;
	}
	word = (guint32) eslot >> 5;
	bit = 1u << ((guint32) eslot & 31);
	if (wj_census_seen [word] & bit)
		return;
	wj_census_seen [word] |= bit;
	wj_census_entered++;
	mono_atomic_inc_i32 (&wj_census_entered_total);
}

/* Always-on liveness probe: is the wasm method-JIT actually running, and how much has it compiled?
 *
 * Every WJC_* counter (including WJC_REGISTERED) is gated behind MONO_WASM_JIT_STATS so release
 * builds pay nothing on the hot dispatch paths — which means a CLEAN timing run, the only kind worth
 * timing, reports zero for all of them and cannot distinguish "the JIT is off" from "the JIT is
 * slow". MONO_WASM_JIT_AUTO=0 silently disables the whole tier (it gates wasm_jit_maybe_compile and
 * wasm_jit_drain_promotions) with no symptom other than a bad number, and that cost a real
 * measurement already.
 *
 * So this reads process state that exists regardless of stats: the auto/threshold config and the
 * registry high-water mark. It is O(1), allocation-free and safe to call from JS before every timed
 * run. Field: 0 = auto, 1 = threshold, 2 = registered methods (wj_reg_n), 3 = function-table
 * exhaustion events (see wj_table_exhausted), 4 = JIT_CALL table entries remaining (browser only),
 * 14-17 = the hang probe (see the note at those cases).
 */
EMSCRIPTEN_KEEPALIVE int
mono_wasm_jit_liveness (int field)
{
	switch (field) {
	case 0: return mono_wasm_jit_auto;
	case 1: return mono_wasm_jit_thresh;
	case 2: return wj_reg_n;
	case 3: return wj_table_exhausted;
#ifdef HOST_BROWSER
	/* Entries left in the JIT_CALL table. With field 2 this makes slot leakage directly measurable:
	 * consumed = capacity - remaining, and a healthy run has consumed ~= 2 * registered (every method
	 * takes an e and an f). A large excess means pairs were allocated and never published — which is
	 * what the reservation parking in wasm_jit_compile_scc / mono_wasm_jit_reserve_self exists to stop.
	 * There is no way to infer this from the outside: the allocator has no free and exposes no cursor. */
	case 4: { extern int mono_jiterp_table_remaining (int type); return mono_jiterp_table_remaining (1); }
#endif
	/* Census. THE GATE IS SPLIT: the INSTANTIATION fields (5, 7, 8, 10, 13) are unconditional, and only
	 * the ENTRY fields (6, 9) still need MONO_WASM_JIT_ENTRYCENSUS=1 -- that half costs a load+test at
	 * the interp->JIT boundary, the instantiation half is two atomics at a choke point that already
	 * costs 29.4 us + 15.4 ns/byte. Before the split, field 8/13 read zero on every timed arm, which is
	 * why the per-worker instantiation bill had never been measured. An ENTRY field reading 0 still means
	 * "the knob was off", not "nothing was entered".
	 *
	 * Fields 5-7 are THREAD-LOCAL and only meaningful on a thread you can actually evaluate in; 8-13 are
	 * process-wide atomics and are the ones to trust, because the pthread workers are parked in blocking
	 * wasm and CDP cannot evaluate in them at all.
	 *
	 * The headline is field 8 against field 2: total instantiations vs distinct methods registered. A
	 * ratio near 1.0 means each module is instantiated about once and there is no per-worker duplication
	 * to remove; N means every module is being instantiated on ~N threads, and each of those instances
	 * costs two export JSFunctions and therefore two JSDispatchTable entries. Field 9 (distinct
	 * (thread, method) pairs actually entered) is the lower bound on how many were genuinely needed --
	 * a lower bound, because direct JIT->JIT calls never cross the interp boundary where entry is
	 * counted. Field 10 is the only surviving path split: admit, which is demand-driven already.
	 * FIELDS 11 AND 12 ARE RETIRED, NOT RENUMBERED -- they counted instantiate_fslot and sync_thread,
	 * both of which turned out to have no callers and are gone. The gap is deliberate so a number read
	 * off an archived capture still means what it meant when it was taken. */
	case 5: return wj_census_instantiated;
	case 6: return wj_census_entered;                       /* ENTRYCENSUS-gated */
	case 7: return (int) (wj_census_inst_us / 1000);        /* ms this thread spent instantiating */
	case 8: return wj_census_inst_total;                    /* ALL instantiations, all threads, all paths */
	case 9: return wj_census_entered_total;                 /* ENTRYCENSUS-gated: distinct (thread, e-slot) pairs entered */
	case 10: return wj_census_inst_admit;                   /* of which: via mono_wasm_jit_admit */
	/* 11, 12: retired (instantiate_fslot / sync_thread — both were dead code). */
	case 13: return wj_census_inst_us_total / 1000;         /* ms spent instantiating, all threads */

	/*
	 * THE HANG PROBE, 14-17. Readable with MONO_WASM_JIT_STATS=0 and without --dumps, which is the whole
	 * point: a wedged run's main thread never returns from Runtime.evaluate, so globalThis.dumpWasmJit()
	 * times out and the counters that would explain the hang are exactly the ones that cannot be read.
	 * R269 lost its one opportunity to read a hanging run that way and the round lost its answer.
	 *
	 * These four are plain loads of counters that are bumped UNGATED (the publish/retire family uses
	 * `mono_wasm_jit_counters[X]++` directly, not the stats-gated helper), so a clean timing run carries
	 * them too and they cost nothing to consult.
	 *
	 *   14 pub_reraise_max   a safepoint condition that never clears: the drain then runs at every loop
	 *                        back-edge on that worker, which is what a frozen tier with a busy core is.
	 *   15 rv_retry_dropped  carry-list entries given up on. NON-ZERO IS HEALTHY -- it counts the workers
	 *                        that did not get pinned.
	 *   16 worker_slots_full workers that got the permanently-set action word (past WJ_WORKER_MAX).
	 *   17 retire_spin_max   worst single acquisition of the retired-payload lock.
	 */
	case 14: return (int) mono_wasm_jit_counters [WJC_ACT_PUB_RERAISE_MAX];
	case 15: return (int) mono_wasm_jit_counters [WJC_RV_RETRY_DROPPED];
	case 16: return (int) mono_wasm_jit_counters [WJC_WORKER_SLOTS_FULL];
	case 17: return (int) mono_wasm_jit_counters [WJC_RETIRE_SPIN_MAX];
	default: return -1;
	}
}

void
mono_wasm_jit_bind_logical (int desc_id, MonoMethod *logical_method)
{
	WjRegEntry *re;
	if (desc_id <= 0 || !logical_method)
		return;
	mono_loader_lock ();
	re = desc_id <= wj_reg_n ? wj_reg_at (desc_id - 1) : NULL;
	if (re) {
		/* Rebinding is valid only for the synchronized-inner body substitution or the same method. */
		if (re->logical_method != re->body_method && re->logical_method != logical_method) {
			/* re->logical_method is retained for the process lifetime while IKVM frees dynamic types, and
			 * mono_method_get_full_name WALKS THE SIGNATURE -- which is how a dead pointer here presents
			 * as an OOB inside dlrealloc rather than as anything that names this site (R273). */
			char *oldn = mono_wasm_jit_method_usable (re->logical_method, WJ_BADMETH_SITE_REGISTRY)
				? mono_method_get_full_name (re->logical_method) : g_strdup ("<dead-method>");
			char *newn = mono_method_get_full_name (logical_method);
			printf ("WASM_JIT_LOGICAL_REBIND desc=%d old=%s new=%s\n", desc_id, oldn, newn);
			g_free (oldn); g_free (newn);
		} else {
			re->logical_method = logical_method;
			re->logical_imethod = logical_method ? mono_interp_get_imethod (logical_method) : NULL;
		}
	}
	mono_loader_unlock ();
}

static int
wj_desc_for_fslot (int fslot)
{
	int ci = fslot / WJ_SLOT_CHUNK;
	if (fslot <= 0 || ci < 0 || ci >= WJ_SLOT_NCHUNKS || !wj_fslot_desc_chunks [ci])
		return 0;
	mono_memory_barrier ();
	return wj_fslot_desc_chunks [ci][fslot % WJ_SLOT_CHUNK];
}

/* Per-worker admission state. Generated direct calls contain no liveness checks: a root may enter only
 * after this DFS has installed its complete immutable direct-call closure in the worker's table. */
static __thread guint8 *wj_desc_state;
/* WHICH GENERATION of each descriptor THIS worker admitted, compared against re->generation to detect a
 * re-framed module. NOT a counter -- the block that used to sit here described a deferral count for a
 * mechanism (imported dependencies mid-DFS, WJ_ADMIT_DEFER_MAX) that R163 deleted along with method
 * imports, and it had drifted onto the wrong variable entirely. */
static __thread guint32 *wj_desc_generation;
/* How many times THIS worker has seen a descriptor fail permanently (bad bytes at instantiate). A
 * generation bump resets wj_desc_state to 0 so a RE-FRAMED module gets a fresh chance, which is right --
 * but if the re-framing keeps producing the same broken module, "permanent" degrades into an unbounded
 * retry loop. R167 measured that directly: one descriptor produced 959,405 WASM_JIT_ADMIT_FAIL
 * (Table.set of a non-function) with admitFailPerm at 338,164, and the game ran at 0.02 fps -- a hung
 * frame loop rather than one lost method. Past WJ_PERMFAIL_MAX attempts the verdict stops being
 * revisited on this worker. */
static __thread guint8 *wj_desc_permfail;
#define WJ_PERMFAIL_MAX 3
static __thread int wj_desc_state_cap;

static void
wj_desc_state_ensure (int id)
{
	if (id < wj_desc_state_cap)
		return;
	{
		int old = wj_desc_state_cap, cap = old ? old : 1024;
		while (id >= cap) cap *= 2;
		wj_desc_state = (guint8 *) g_realloc (wj_desc_state, cap);
		wj_desc_generation = (guint32 *) g_realloc (wj_desc_generation, sizeof (guint32) * cap);
		wj_desc_permfail = (guint8 *) g_realloc (wj_desc_permfail, cap);
		memset (wj_desc_state + old, 0, cap - old);
		memset (wj_desc_generation + old, 0, sizeof (guint32) * (cap - old));
		memset (wj_desc_permfail + old, 0, cap - old);
		wj_desc_state_cap = cap;
	}
}

/* R151's boot-crash guard (`wj_method_ptr_ok` + `wj_note_bad_method`, WASM_JIT_BAD_LOGICAL_METHOD)
 * lived here and WAS NEVER SPLICED INTO A CALL PATH -- no caller anywhere in src/, so it printed nothing
 * and refused nothing for its entire life. It was written for the "one boot in four dies in
 * mono_interp_get_imethod on a worker" fault: the registry keeps a raw MonoMethod* per descriptor for
 * the process lifetime while IKVM generates dynamic types continuously, so that pointer is not always
 * dereferenceable by the time admission runs. Deleted rather than wired up, because a range check cannot
 * see a freed pointer that still lands in range and the fault has not recurred; recording it here so the
 * next reader knows that crash mode is UNPROTECTED, not guarded. */

/* Did the batch snapshot go stale between reading `re->batch` and reading `re->bytes`? See the call
 * site: the pair must be consumed consistently, and the reader's natural order is the unsafe one. */
static inline gboolean
wj_batch_raced (WjRegEntry *re, WjBatchDesc *snapshot)
{
	mono_memory_barrier ();
	return snapshot == NULL && re->batch != NULL;
}

int mono_wasm_jit_admit (int desc_id);

/* The slot-live bitmap answers only whether some generation has occupied this table slot. Automatic
 * rebatching deliberately reuses the same e/f slots, so root-entry fast paths must additionally verify
 * that THIS worker admitted the registry entry's current generation. */
/* DIAGNOSTIC 2026-08-29. What does the table ACTUALLY hold at this slot, on this thread?
 *
 * The per-thread bitmaps say the slot is live (WASM_JIT_ETHUNK_NOT_LIVE fired 0 times) yet
 * wasm_jit_ethunk_cb still traps with `function signature mismatch`, so the bitmap and the table
 * disagree. `Module.__wjSlotFn` could not have caught this: it is only consulted inside the IMPORT
 * resolver, so it has never watched an e-slot.
 *
 * Arity is the discriminator. An entry thunk is (i32,i32)->void, length 2. The jiterpreter prefill
 * mono_jiterp_placeholder_jit_call is (i32,i32,i32,i32)->void, length 4.
 *   1 = thunk-shaped   2 = wrong arity (n returned in *out_len)   3 = not what we installed   0 = empty
 */
WJ_KEEPALIVE int
mono_wasm_jit_eslot_probe (int slot, int *out_len)
{
#ifdef HOST_BROWSER
	int len = -1, r;
	r = EM_ASM_INT ({
		/* Same >2GB caveat as the instantiate paths: `>>` is ToInt32, so an out-param above 2 GB would
		 * index HEAP32 negatively and the store would be a silent no-op, reporting arity -1 forever.
		 * Normalise, then divide. */
		var lp = $1 < 0 ? $1 + 4294967296 : $1;
		var f = wasmTable.get ($0);
		if (!f) return 0;
		HEAP32[lp / 4] = (f.length | 0);
		if ((f.length | 0) !== 2) return 2;
		if (Module.__wjSlotFn && Module.__wjSlotFn.has ($0) && Module.__wjSlotFn.get ($0) !== f) return 3;
		return 1;
	}, slot, &len);
	if (out_len) *out_len = len;
	return r;
#else
	(void) slot; if (out_len) *out_len = -1; return 1;
#endif
}

/* The InterpMethod a descriptor was registered against, resolved AT REGISTRATION on a thread where the
 * method was live (see the WjRegEntry note on logical_imethod and R151's two worker boot faults). Handed
 * out so re-emission can carry a plain int through its queue instead of becoming a second holder of a raw
 * InterpMethod* -- the registry already owns that retention risk, and one owner is enough. */
gpointer mono_wasm_jit_desc_logical_imethod (int desc_id);
gpointer
mono_wasm_jit_desc_logical_imethod (int desc_id)
{
	WjRegEntry *re = (desc_id > 0 && desc_id <= wj_reg_n) ? wj_reg_at (desc_id - 1) : NULL;
	return re ? re->logical_imethod : NULL;
}

WJ_KEEPALIVE /* How many methods the JIT tier has registered. Descriptor ids are handed out in
 * registration order, so `registry_count - desc` is also a self-scaling proxy for how much the tier has
 * learned since a given method was compiled. */
int
mono_wasm_jit_registry_count (void)
{
	return wj_reg_n;
}

/* Are these two descriptors members of the SAME framed group, i.e. does a call from one to the other
 * land inside one WebAssembly.Instance?
 *
 * This is the question WJC_CALL_LOCAL cannot answer and the reason it read co-location as inert on
 * Minecraft for four rounds. CALL_LOCAL counts relocations the assembler turned into `call <funcidx>`,
 * which is the call-FORM half; R183's differential measured the other half -- same module, same
 * instance, calls still `call_indirect` -- at -7.0% of -12.8%, i.e. 55% of the win. A dispatch site
 * only gets that half if its RUNTIME target happens to be a sibling, which no emit-time counter can
 * see, because V8's CallIndirectIC gates inlining on `implicitArg == current instance`
 * (builtins/wasm.tq:821-835) and nothing else.
 *
 * Pointer identity on the WjBatchDesc, not adjacency of descriptor ids: consecutive ids look like
 * siblings and are not (see WASM_JIT_DEP_BAD, which makes the same distinction for a different bug). */
/*
 * Result: 1 = sibling (same instance), 0 = ours but a DIFFERENT group, -1 = no descriptor here at all
 * (AOT body, main-module helper, or a callee this worker has never registered).
 *
 * Three-way deliberately. Collapsing 0 and -1 is the mistake WJC_SHADOW_REFUSED made before R167 split
 * it: "ours but in another group" is a reach problem the planner can fix, while "not ours" needs
 * MONO_WASM_JIT_OVER_AOT and no grouping rule can reach it. They lead to opposite decisions.
 *
 * Takes the callee as an F-SLOT rather than a descriptor so the registry lookup stays inside this file;
 * the dispatch paths that need this hold an f-slot and a caller descriptor, never two descriptors.
 */
int mono_wasm_jit_call_is_colocated (int caller_desc, int callee_fslot);
int
mono_wasm_jit_call_is_colocated (int caller_desc, int callee_fslot)
{
	WjRegEntry *ra, *rb;
	int callee_desc;

	if (caller_desc <= 0 || caller_desc > wj_reg_n || callee_fslot <= 0)
		return -1;
	callee_desc = wj_desc_for_fslot (callee_fslot);
	if (callee_desc <= 0 || callee_desc > wj_reg_n)
		return -1;
	if (callee_desc == caller_desc)
		return 1;   /* a self-call is trivially same-instance */
	ra = wj_reg_at (caller_desc - 1);
	rb = wj_reg_at (callee_desc - 1);
	if (!ra || !rb)
		return -1;
	/* Pointer identity on the WjBatchDesc, not adjacency of descriptor ids: consecutive ids look like
	 * siblings and are not (WASM_JIT_DEP_BAD makes the same distinction for a different bug). */
	return (ra->batch && ra->batch == rb->batch) ? 1 : 0;
}

int
mono_wasm_jit_desc_admitted (int desc_id)
{
	WjRegEntry *re;
	if (desc_id <= 0 || desc_id > wj_reg_n || desc_id >= wj_desc_state_cap)
		return 0;
	mono_memory_barrier ();
	re = wj_reg_at (desc_id - 1);
	if (!re)
		return 0;
	return wj_desc_state [desc_id] == 2 &&
		wj_desc_generation [desc_id] == re->generation &&
		mono_wasm_jit_slot_live (re->e) &&
		mono_wasm_jit_slot_live (re->f);
}

/*
 * Admit `desc_id` AND PROVE IT LANDED. Use this, not mono_wasm_jit_admit, before entering an e-slot.
 *
 * mono_wasm_jit_admit returns 1 in two quite different situations: the descriptor is admitted (state 2,
 * exports installed in THIS thread's table), or the DFS found it already `visiting` and broke a cycle --
 * which returns 1 WITHOUT INSTANTIATING ANYTHING. That second case is correct for a call_indirect edge,
 * because the table is consulted when the call runs, and wj_admit_dependencies already knows it: it takes
 * the trouble to re-test `wj_desc_state [imp_id] == 2` after calling admit, for exactly this reason.
 *
 * The interp -> JIT entry paths did not. They call the e-slot directly, and an e-slot that this worker
 * never installed still holds the jiterpreter prefill, mono_jiterp_placeholder_jit_call -- a real function
 * of type (i32,i32,i32,i32)->void. Calling that as the thunk's (i32,i32)->void is a wasm
 * "function signature mismatch" trap that kills the worker, and it is reached whenever anything runs
 * interpreted while an admission DFS is open on this thread (a compile inside admit runs cctors, which
 * interpret, which can dispatch a delegate...).
 *
 * The window was narrow while a descriptor was admitted alone. Co-location widens it by the batch size:
 * mono_wasm_jit_admit premarks EVERY sibling `visiting` before admitting the union of their closures, so a
 * batch of N puts N descriptors in the state that makes admit() lie, for the whole of that DFS.
 *
 * The comment at the MINT_CALLVIRT_FAST site said "then confirm the slot actually instantiated here"; this
 * is the function that finally does it.
 */
int mono_wasm_jit_admit_live (int desc_id);
int
mono_wasm_jit_admit_live (int desc_id)
{
	/* ALREADY-LIVE FAST PATH. Same argument, and the same soundness proof, as the already-admitted fast
	 * path hoisted into mono_wasm_jit_admit: on the plateau this is the answer essentially every time,
	 * and the general path below reaches it via TWO cross-function calls that re-derive the same facts.
	 *
	 * What the general path costs that this does not: mono_wasm_jit_admit's own prologue and bounds
	 * tests, then mono_wasm_jit_desc_admitted repeating the state and generation compares AND issuing a
	 * full mono_memory_barrier -- the very fence mono_wasm_jit_admit's fast path was written to get off
	 * the dispatch path, reintroduced one call later on the same descriptor.
	 *
	 * Sound for the same reason: wj_desc_state / wj_desc_generation / wj_desc_state_cap and the slot
	 * liveness bitmap are all __thread, so every fact below is one THIS thread wrote and no acquire
	 * fence is needed to observe it. re->generation is shared and read unfenced, which is the documented
	 * contract (stale by at most one rebind => a delayed re-admit on a later call, never a wrong answer:
	 * a mismatch falls through to the general path). The conditions mirror mono_wasm_jit_desc_admitted
	 * in its own order -- keep them in step with it and with the WJC_AL_* attribution below. */
	if (G_LIKELY (desc_id > 0 && desc_id <= wj_reg_n && desc_id < wj_desc_state_cap &&
	              wj_desc_state [desc_id] == 2)) {
		WjRegEntry *fre = wj_reg_at (desc_id - 1);
		if (fre && wj_desc_generation [desc_id] == fre->generation &&
		    mono_wasm_jit_slot_live (fre->e) && mono_wasm_jit_slot_live (fre->f))
			return 1;
	}
	if (!mono_wasm_jit_admit (desc_id)) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_AL_ADMIT0);
		return 0;
	}
	if (G_LIKELY (mono_wasm_jit_desc_admitted (desc_id)))
		return 1;
	/* admit() said yes and desc_admitted() said no. Attribute it -- this conjunction is where R166's
	 * 106.7M interpreter fallbacks live, and the two theories tried before this counter existed (a
	 * generation split between trigger and siblings; a stale state 1) were both wrong on inspection.
	 * Mirrors mono_wasm_jit_desc_admitted's conditions in its own order; keep them in step. */
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		WjRegEntry *re = (desc_id > 0 && desc_id <= wj_reg_n) ? wj_reg_at (desc_id - 1) : NULL;
		if (!re || desc_id >= wj_desc_state_cap || wj_desc_state [desc_id] != 2)
			mono_wasm_jit_count (WJC_AL_STATE);
		else if (wj_desc_generation [desc_id] != re->generation)
			mono_wasm_jit_count (WJC_AL_GEN);
		else if (!mono_wasm_jit_slot_live (re->e))
			mono_wasm_jit_count (WJC_AL_ELIVE);
		else
			mono_wasm_jit_count (WJC_AL_FLIVE);
	}
	return 0;
}

/* Validate and admit one descriptor's external direct-call closure. Batch-internal edges encounter
 * siblings pre-marked state=1 by mono_wasm_jit_admit and therefore terminate as ordinary DFS cycles. */
/*
 * Returns 1 admitted, 0 permanently unusable, -1 DEFER: an imported dependency is still mid-DFS.
 *
 * The -1 case is the whole reason this returns an int. mono_wasm_jit_admit returns 1 when it finds a
 * descriptor already `visiting`, which is how it breaks a dependency CYCLE -- correct for a call_indirect
 * edge, because the table is patched after publication and the call resolves whenever it eventually runs.
 * It is NOT correct for an IMPORT edge: the import is bound while this module instantiates, so a target
 * that has not instantiated yet means a LinkError, and the method is lost.
 *
 * The instantiation-ordering problem cannot be settled when the module is EMITTED, which is what the
 * first attempt at this got wrong: the decision is baked at emit time but has to hold at admission time,
 * which happens later and separately on every worker, and by then the dependency graph has grown (a
 * re-emit registers a new descriptor into the same f-slot with a new dependency list, so the callee behind
 * an imported slot can gain an edge back to its importer after the import was baked in).
 *
 * So it is settled HERE instead, where the true state is known: if an imported dependency is not admitted,
 * do not instantiate. Defer, run this method interpreted for now, and let a later dispatch retry -- which
 * usually succeeds, because the retry may enter the DFS from the other end of the cycle and admit the
 * callee first.
 *
 * THE RETRY IS NOT BOUNDED. This used to claim "bounded by WJ_ADMIT_DEFER_MAX"; that macro does not exist
 * anywhere in the tree and the deferral mechanism it named went with method imports in R163. Refusals here
 * are re-walked on EVERY dispatch, forever, which is how R244's 13.6-22.8 MILLION per run happened with
 * `registered` perfectly flat. Every refusal below is counted so that rate is visible.
 */
/* Recursion depth of the admission DFS, per thread because the walk is per thread. See
 * WJC_ADMIT_DEPTH_MAX for why this exists: the mutual recursion below has no depth bound and overflows
 * the wasm stack once re-emission lengthens dependency chains. Instrumented before being fixed, so the
 * explicit-stack rewrite can be sized against a measured distribution rather than a guess. */
static __thread int wj_admit_depth;

static int
wj_admit_dependencies (WjRegEntry *re, WjDepSet *ds, int desc_id, gboolean watch)
{
	int i;
	/* `ds` is the caller's snapshot, taken together with the bytes it will instantiate (R320, wj_pub_snapshot):
	 * ONE snapshot for the whole walk -- see WjRegEntry.depset. */
	for (i = 0; ds && i < ds->n; ++i) {
		/* Phase 5, invariant 1 (mini-wasm-lazy.inc): a pool dep is callable here once its stub bank is, and is never
		 * descended into -- its stub binds the real callee, through that callee's own admission, when first called. */
		if (wj_lazy_is_pool_slot (ds->slot [i])) {
			if (!wj_lazy_dep_ok (ds->slot [i]))
				return 0;
			if (!wj_lazy_dep_needs_real (ds->slot [i]))
				continue;
			/* a registered no-GC callee: a caller may have been credited for it, so it is admitted for real below */
		}
		int dep_id = wj_desc_for_fslot (ds->slot [i]);
		WjRegEntry *dep = dep_id ? wj_reg_at (dep_id - 1) : NULL;
		if (watch)
			printf ("WASM_JIT_ADMIT_DEP parent=%d i=%d f=%d dep=%d state=%d e_live=%d f_live=%d\n",
				desc_id, i, ds->slot [i], dep_id,
				dep_id > 0 && dep_id < wj_desc_state_cap ? wj_desc_state [dep_id] : -1,
				dep ? mono_wasm_jit_slot_live (dep->e) : 0, dep ? mono_wasm_jit_slot_live (dep->f) : 0);
		/* IDENTITY, NOT JUST THE HASH.
		 *
		 * `f_sig_id` is FNV-1a over (nparams, params, ret) -- a 32-BIT value used as an equality test.
		 * With ~30,000 registered methods the birthday probability of at least one collision in a run is
		 * 1 - exp(-30000^2 / 2*2^32) ~= 10%, which is the same order as the observed
		 * `function signature mismatch` trap rate. A collision means the caller's baked expectation and
		 * whatever actually occupies the f-slot hash EQUAL while their functypes DIFFER, so admission
		 * passes and the emitted `call_indirect` traps -- with no counter anywhere, because every guard
		 * agreed.
		 *
		 * The depset already carries the method the caller resolved (`wj_depset_new(slots, sigs,
		 * methods, n)`), and the registry knows what actually registered, so the exact test is available
		 * for one pointer compare. Accept EITHER of logical/body method: they differ legitimately for
		 * wrapper shapes (synchronized inner, etc.) and which one a call site resolved to is not
		 * something to assume here. NULL expectation means the caller did not record one -- older
		 * depsets -- so fall back to the hash alone rather than refusing everything. */
		gboolean ident_bad = ds->method [i] && dep &&
			dep->logical_method != ds->method [i] && dep->body_method != ds->method [i];
		if (ident_bad)
			mono_wasm_jit_counters [WJC_ABI_MISMATCH_IDENT]++;
		if (!dep_id || !dep || dep->f_sig_id != ds->sig [i] || ident_bad) {
			/* Disambiguate what the old print collapsed into "actual=0x0":
			 *  cause=fslot-unregistered  — the baked dep f-slot has NO registry entry at all (a slot that
			 *    was readable at emit time but whose registration never happened / was refused);
			 *  cause=sig-hash-mismatch   — the dep IS registered but its emitted ABI hash differs from what
			 *    the caller derived from the call-site signature (a real WasmCallInfo/self-sig divergence).
			 * dep_now_fslot = the callee's CURRENT published/reserved f-slot (from its imethod): if it is
			 * >0 and != dep_fslot the callee re-registered under a fresh slot after the caller baked the
			 * old one; 0/-1 means it never (re)registered. */
			extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
			extern int mono_wasm_jit_verbose;
			MonoMethod *dm = ds->method [i];
			/* COUNT ALWAYS, NAME ONLY WHEN ASKED -- and this used to be the other way round, which is
			 * what killed roughly 1 A/B run in 6 (R231).
			 *
			 * This refusal is RECOVERABLE and expected: `return 0` just declines to admit and the next
			 * dispatch retries. So the path is taken deliberately and often. The old code answered it
			 * with an UNGATED mono_method_get_full_name on BOTH the caller and the dep, plus a
			 * mono_wasm_jit_get_callee_fslot -- all metadata operations. The name walk goes
			 * mono_method_get_full_name -> mono_signature_get_desc -> mono_type_get_desc and can trigger
			 * LAZY type resolution; admission runs LOCK-FREE, on a WORKER, at every dispatch. That is
			 * exactly R199's fault in a second location, and the captured stack is those frames under
			 * wj_admit_dependencies rather than under wj_assemble, which is why R199's fix (caching
			 * WjBody.name at emit time) did not cover it.
			 *
			 * Splitting the three causes into counters also un-merges what one printf collapsed, so the
			 * question "which cause dominates" is now answerable without turning the fault back on. */
			mono_wasm_jit_counters [!dep_id ? WJC_ABI_MISMATCH_UNREG
					        : (!dep ? WJC_ABI_MISMATCH_CHUNK : WJC_ABI_MISMATCH_SIG)]++;
			if (mono_wasm_jit_verbose) {
				/* Under the loader lock, which is what serialises the lazy resolution these walks can
				 * trigger -- the same discipline the WASM_JIT_LOGICAL_REBIND site above already follows,
				 * and the reason that site has never been seen to fault. */
				char *cn, *dn;
				int now_fslot;
				mono_loader_lock ();
				cn = (re->logical_method && mono_wasm_jit_method_usable (re->logical_method, WJ_BADMETH_SITE_REGISTRY))
					? mono_method_get_full_name (re->logical_method) : NULL;
				dn = dm ? mono_method_get_full_name (dm) : NULL;
				now_fslot = dm ? mono_wasm_jit_get_callee_fslot (dm) : -1;
				printf ("WASM_JIT_ABI_MISMATCH desc=%d dep_fslot=%d expected=0x%x actual=0x%x cause=%s dep_desc=%d dep_now_fslot=%d caller=%s dep=%s\n",
					desc_id, ds->slot [i], ds->sig [i], dep ? dep->f_sig_id : 0,
					!dep_id ? "fslot-unregistered" : (!dep ? "desc-chunk-missing" : "sig-hash-mismatch"),
					dep_id, now_fslot,
					cn ? cn : "?", dn ? dn : "?");
				g_free (cn); g_free (dn);
				mono_loader_unlock ();
			}
			return 0;
		}
		int __wj_dep_r;
		/* THE recursion site. Depth is tracked here rather than inside mono_wasm_jit_admit because that
		 * function has many returns and a hot already-admitted fast path that must not grow; this is the
		 * single edge the DFS descends through. */
		++wj_admit_depth;
		if (wj_admit_depth > (int) mono_wasm_jit_counters [WJC_ADMIT_DEPTH_MAX])
			mono_wasm_jit_counters [WJC_ADMIT_DEPTH_MAX] = wj_admit_depth;
		if (wj_admit_depth == WJ_ADMIT_DEPTH_WARN)
			mono_wasm_jit_counters [WJC_ADMIT_DEPTH_OVER]++;
		__wj_dep_r = mono_wasm_jit_admit (dep_id);
		--wj_admit_depth;
		if (!__wj_dep_r) {
			/* Propagation, not a root cause: the dep counted its OWN refusal reason at its own exit.
			 * Counted anyway because without it this route was invisible, and on HEAD it is ~45% of
			 * alAdmit0 -- the share that made ADMIT_DEP_NOT_LIVE look like the whole story when it was
			 * barely half of it. */
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_DEP_ADMIT0);
			return 0;
		}
		/* ADMISSION'S CONTRACT, ACTUALLY TESTED (R239). Everything above proves the dep is the right
		 * METHOD with the right ABI; nothing proved the f-slot the caller baked is INSTALLED ON THIS
		 * WORKER, which is the whole point of admission -- `generated indirect calls carry no liveness
		 * check` (see the cycle-break in mono_wasm_jit_admit). If it is not installed, the slot holds
		 * mono_jiterp_placeholder_jit_call and the emitted call_indirect traps with `function signature
		 * mismatch` -- which is exactly the surviving trap class, and which every ABI_MISMATCH_* counter
		 * reads 0 for. Test ds->slot [i], not dep->f: the baked slot is what the generated code calls,
		 * so if the callee re-registered under a fresh f-slot this catches the stale one too.
		 * Refusing is the recoverable path the sig-hash arm above already takes -- state 0, and the next
		 * dispatch retries -- so a transient window costs a deferral rather than a dead world. */
		/* INSTALLED, NOT LIVE -- and the difference is the whole bug (R245 Stage 2b, second attempt).
		 *
		 * R239 added this test as `mono_wasm_jit_slot_live`, and that is the wrong predicate. The two
		 * bitmaps answer different questions:
		 *   installed = THIS worker wrote a real function into that table slot, so a call_indirect
		 *               through it lands on real code rather than mono_jiterp_placeholder_jit_call;
		 *   live      = that method's own closure is admitted, so a worker may ENTER it.
		 * What generated code does with `ds->slot [i]` is call_indirect it, unconditionally. So the
		 * property required here is INSTALLATION. Demanding liveness demanded something strictly
		 * stronger and unrelated, and the cycle break -- which installs a closure without publishing any
		 * of it, because publishing is its ancestor's job -- could never satisfy it. Result: 72.5 M
		 * refusals per run that no retry could ever clear (R245).
		 *
		 * live is a SUBSET of installed by construction (wj_mark_slot_live refuses an uninstalled slot),
		 * so this is a strict weakening of a test that was too strong, not a hole.
		 *
		 * MY FIRST ATTEMPT AT THIS WAS UNSOUND AND IS REVERTED. It kept the liveness test and made the
		 * install walk publish liveness for everything it installed. But wj_install_closure SKIPS a dep
		 * whose f-slot is unregistered (wj_desc_for_fslot returns 0) and still returns TRUE, whereas the
		 * walk below REFUSES it (ABI_MISMATCH_UNREG). Granting liveness on the install walk's verdict
		 * therefore published methods that admission would have refused, and they call_indirect'd the
		 * placeholder: `function signature mismatch` in 1 of 2 runs, the exact class this check exists to
		 * prevent. Do not re-derive liveness from a weaker walk. */
		if (!wj_slot_is_installed (ds->slot [i])) {
			/* SPLIT BY CAUSE, because the two cases need opposite fixes and summing them hid that for
			 * a whole build. state 1 means mono_wasm_jit_admit returned 1 through its CYCLE BREAK, which
			 * calls wj_install_closure_root (installs) but never wj_mark_slot_live (publishes) -- so this
			 * liveness test can NEVER pass for that dep, and the refusal recurs on every dispatch for the
			 * life of the process. That is not a transient window and retrying it is not recovery.
			 * Anything else is the ordinary pending case the retry genuinely clears.
			 *
			 * Gated on mono_wasm_jit_stats like every sibling. It used to be a raw counters[]++ while the
			 * WJC_AL_* arms were gated, so with stats off the two sets diverged and no ratio between them
			 * was valid -- which is the form the R244 diagnosis had to work around. */
			if (G_UNLIKELY (mono_wasm_jit_stats)) {
				gboolean cycle = dep_id > 0 && dep_id < wj_desc_state_cap && wj_desc_state [dep_id] == 1;
				mono_wasm_jit_count (WJC_ADMIT_DEP_NOT_LIVE);
				mono_wasm_jit_count (cycle ? WJC_ADMIT_DEP_NOT_LIVE_CYCLE : WJC_ADMIT_DEP_NOT_LIVE_PENDING);
			}
			return 0;
		}
	}
	return 1;
}
/*
 * Install this descriptor's module on THIS worker if it is not already there -- and nothing else. No
 * dependency walk, no liveness, no state change beyond the generation stamp.
 *
 * SAFE IN ANY ORDER, and that is new. Since method imports were removed (R163) a generated module binds
 * nothing but the main module's memory, table, helper/AOT functions and the `s.*` globals -- all of which
 * exist from boot. There is no such thing as an instantiation-time dependency on another JITted method any
 * more, so "instantiate B before A" is never wrong; only "let A be CALLED before B is installed" is.
 *
 * That distinction is what makes the cycle break sound. Admission's DFS returns 1 on a back edge without
 * recursing, which used to mean the descriptor was left UNINSTALLED while its caller went live -- and a
 * generated `call_indirect` carries no liveness check, so the caller reached
 * mono_jiterp_placeholder_jit_call and trapped with `function signature mismatch` (20 per boot, R163).
 * Method imports had been hiding this by turning it into a LinkError at instantiation instead.
 * Installing here restores the invariant the import list used to enforce as a side effect: every f-slot a
 * live method can call_indirect is installed by the time it goes live.
 */
static gboolean
wj_admit_install_only (int desc_id, WjRegEntry *re, WjBatchDesc *snap_batch, void *snap_bytes, int snap_len)
{
	char eb [192];
	double ms = 0;
	guint32 gen;

	/* Never re-instantiate a descriptor that has lost its slot: its module's exports would land on top of
	 * whoever owns those slots now. */
	if (G_UNLIKELY (re->orphaned)) {
		mono_wasm_jit_counters [WJC_ADMIT_ORPHANED]++;
		return FALSE;
	}
	/* ONE READ OF THE GENERATION, used for both the decision and the record.
	 *
	 * `re->generation` and `re->depset` are replaced together when a method is re-emitted. Re-reading the
	 * generation AFTER doing the work certifies this worker against a generation whose DEPSET it never
	 * walked: the walk installs depset A, re-emission swaps in depset B and bumps the generation, and the
	 * publish then writes the NEW generation over an admission performed against the OLD one. The
	 * descriptor is thereafter considered current with a dependency that was never installed here -- which
	 * is what the sweep caught (`dep[3] ... inst=0`, caller at state 2 with a current generation).
	 *
	 * Publishing the SNAPSHOT is both correct and self-healing: if the generation moved meanwhile, this
	 * worker's cache is merely stale and the next dispatch re-admits against the new depset. */
	gen = re->generation;
	if (wj_slot_is_installed (re->e) && wj_slot_is_installed (re->f) &&
	    wj_desc_generation [desc_id] == gen)
		return TRUE;
	eb [0] = 0;
	{
	/* ONE SNAPSHOT. mono_wasm_jit_rebatch publishes a FRESH WjBatchDesc, so re-reading `re->batch`
	 * across these arguments can hand the instantiate one module's bytes with another's slot list --
	 * see WJC_ADMIT_BATCH_SWAPPED. */
	/* R320: the caller's snapshot -- the same reading of the entry whose depset its walk installed. */
	WjBatchDesc *ibatch = snap_batch;
	if (ibatch) {
		extern int mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n, const void *bytes, int len, char *errbuf, int errcap, double *out_ms);
		if (!mono_wasm_jit_instantiate_batch_local (ibatch->e, ibatch->f, ibatch->n,
		                                            ibatch->bytes, ibatch->len, eb, (int) sizeof (eb), &ms)) {
			static int _n = 0;
			if (_n++ < 20)
				printf ("WASM_JIT_CYCLE_INSTALL_FAIL desc=%d (batch n=%d) : %s\n", desc_id, ibatch->n, eb);
			return FALSE;
		}
	} else if (!mono_wasm_jit_instantiate_local (re->e, re->f, snap_bytes, snap_len, eb, (int) sizeof (eb), &ms)) {
		static int _n = 0;
		if (_n++ < 20)
			printf ("WASM_JIT_CYCLE_INSTALL_FAIL desc=%d e=%d f=%d : %s\n", desc_id, re->e, re->f, eb);
		return FALSE;
	}
	}
	/* The snapshot, not a fresh read -- see the note at the top of this function. */
	wj_desc_generation [desc_id] = gen;
	return TRUE;
}

/* Per-thread bookkeeping for the install-closure walk below. Same stamp trick as wj_asm_reaches: a
 * generation counter instead of clearing a 24k array on every call. */
/* Sized for the UNION of a co-located group's closures, not for one method's. wj_install_closure_group
 * seeds the walk from every member of a batch, and the planner has produced 211-member groups, so 512 was
 * exhausted immediately -- the walk then refused admission transiently FOREVER, which presents as a boot
 * that never finishes with `faults=[]` and no trap: the tier simply never admits anything and everything
 * falls back to the interpreter. */
/*
 * A RUNAWAY GUARD, NOT A SIZE LIMIT, and the difference is load-bearing.
 *
 * The stamp already visits each descriptor at most once per pass, so the natural bound on a walk is the
 * size of the registry. A fixed constant here is therefore not a policy about how large a closure may be;
 * it is only a floor under a corrupted graph. Sized from wj_reg_n at each root for that reason.
 *
 * WHY IT MATTERS: R267 addendum 12 measured sw_closure_max sitting AT the previous constant (4096) with
 * re-emission off -- i.e. on the SHIPPED path -- so the walk was truncating real closures and refusing
 * their admissions, which presents as a boot that never finishes with faults=[] and no trap at all.
 * Raising a constant is the wrong answer to that twice over: it had already been raised once from 512 for
 * the same reason, and a cap that is ever REACHED silently changes what the tier admits.
 */
#define WJ_INSTALL_SLACK 256
/* The closure the current install pass touched, in stamp order. See wj_make_callable: enumerating it is
 * what lets publication happen strictly AFTER every install in the closure has completed. Never
 * truncated WITHIN a pass -- dropping an entry does not shrink the walk, it hides an installed
 * descriptor from the publish pass, which is the one thing this list exists to prevent -- and reset at
 * every pass entry (wj_install_closure_root / _group).
 *
 * THE RESET USED TO LIVE ONLY IN wj_make_callable, WHICH SHIPS 0 (R269). That was harmless while
 * wj_clo_list was a fixed WJ_INSTALL_MAX array whose append was guarded by `wj_clo_n < WJ_INSTALL_MAX`;
 * R268 replaced it with an unbounded wj_clo_ensure and removed the guard, which turned a dead reset into
 * an unbounded per-thread leak on the SHIPPED path -- appending one int per distinct descriptor per
 * admission, on every worker, for the life of the process, and never read.
 *
 * It also means WJC_SW_CLOSURE_MAX was a CUMULATIVE APPEND COUNT on the shipped path, not a closure
 * size: that is why it read 65,576 against wj_reg_n = 32,175, and why R267 addendum 12 saw it "sitting
 * AT the cap" of 4,096 almost immediately. Any conclusion drawn from its magnitude before this fix is
 * about how many admissions ran, not how big a closure is. The per-pass bound that actually truncates a
 * walk is wj_inst_budget, and WJC_INSTALL_BUDGET_OUT is the counter that reports it. */
static __thread int     *wj_clo_list;
static __thread int      wj_clo_cap;
static __thread int      wj_clo_n;

static void
wj_clo_ensure (int need)
{
	if (need <= wj_clo_cap)
		return;
	wj_clo_cap = wj_clo_cap ? wj_clo_cap * 2 : 1024;
	if (wj_clo_cap < need)
		wj_clo_cap = need;
	wj_clo_list = (int *) g_realloc (wj_clo_list, sizeof (int) * (gsize) wj_clo_cap);
}
static __thread guint32 *wj_inst_stamp;
static __thread int      wj_inst_cap;
static __thread guint32  wj_inst_gen;
static __thread int      wj_inst_budget;

static void
wj_inst_stamp_ensure (int id)
{
	if (id < wj_inst_cap)
		return;
	{
		int ncap = wj_inst_cap ? wj_inst_cap : 1024;
		while (id >= ncap) ncap *= 2;
		wj_inst_stamp = (guint32 *) g_realloc (wj_inst_stamp, sizeof (guint32) * (gsize) ncap);
		memset (wj_inst_stamp + wj_inst_cap, 0, sizeof (guint32) * (gsize) (ncap - wj_inst_cap));
		wj_inst_cap = ncap;
	}
}

/*
 * Install a descriptor AND EVERYTHING IT CAN REACH, on this worker, without admitting anything.
 *
 * wj_admit_install_only alone was not enough, and the way it failed is worth keeping: installing a module
 * whose own dependencies are still absent does not remove the trap, it moves it one level down. MEASURED --
 * `function signature mismatch` went 20 per boot to 2, and the survivors were a `call_indirect` INSIDE a
 * freshly installed `wasmjit-batch[16]` rather than its entry thunk.
 *
 * The closure is what the invariant actually needs: every f-slot reachable from a method that is about to
 * go live must hold a real function. Order within the closure is irrelevant -- since method imports were
 * removed nothing binds at instantiation -- so this is a plain DFS with a stamp to break cycles, and a
 * cycle member simply gets installed by its own frame.
 *
 * Best effort by construction: a failure to install one member is reported by the caller, and the budget
 * bound means a pathologically large closure stops rather than recursing forever. Both leave the caller to
 * fail admission, which is the safe direction.
 */
static gboolean
wj_install_closure (int desc_id)
{
	WjRegEntry *re;
	gboolean ok = TRUE;
	int i;

	if (desc_id <= 0)
		return FALSE;
	wj_inst_stamp_ensure (desc_id + 1);
	if (wj_inst_stamp [desc_id] == wj_inst_gen)
		return TRUE;                     /* already handled in this pass (or on the stack: a cycle) */
	/* CHARGE THE BUDGET FOR DISTINCT NODES ONLY. The decrement used to sit above the stamp test, so every
	 * REVISIT burned budget too -- and a diamond-shaped dependency graph revisits constantly, so the walk
	 * gave up far below its nominal closure size. The cap means "how big a closure may be", not "how many
	 * edges may be traversed". */
	if (wj_inst_budget-- <= 0) {
		mono_wasm_jit_counters [WJC_INSTALL_BUDGET_OUT]++;
		return FALSE;
	}
	wj_inst_stamp [desc_id] = wj_inst_gen;
	/* Collect the closure as it is stamped, so the publish pass can enumerate exactly what was
	 * installed. Bounded by the same budget that bounds the walk. */
	wj_clo_ensure (wj_clo_n + 1);
	wj_clo_list [wj_clo_n++] = desc_id;
	if (wj_clo_n > (int) mono_wasm_jit_counters [WJC_SW_CLOSURE_MAX])
		mono_wasm_jit_counters [WJC_SW_CLOSURE_MAX] = wj_clo_n;
	re = wj_reg_at (desc_id - 1);
	if (!re)
		return FALSE;
	/* R320: ONE reading of {batch, bytes, len, depset} for this node -- the closure walked below is the closure of
	 * the bytes installed at the end, or the pass refuses (transient: the next pass retries a settled entry). */
	WjBatchDesc *snap_b = NULL;
	void *snap_bytes = NULL;
	int snap_len = 0;
	WjDepSet *snap_ds = NULL;
	if (!wj_pub_snapshot (re, &snap_b, &snap_bytes, &snap_len, &snap_ds)) {
		mono_wasm_jit_counters [WJC_ADMIT_PAYLOAD_TORN]++;
		return FALSE;
	}
	{
		WjDepSet *ds = snap_ds;
		for (i = 0; ds && i < ds->n; ++i) {
			int d;
			if (wj_lazy_is_pool_slot (ds->slot [i]) && !wj_lazy_dep_needs_real (ds->slot [i])) {   /* Phase 5: the stub (invariant 1) */
				if (!wj_lazy_dep_ok (ds->slot [i]))
					ok = FALSE;
				continue;
			}
			d = wj_desc_for_fslot (ds->slot [i]);
			if (d > 0 && !wj_install_closure (d))
				ok = FALSE;
		}
	}
	/* THE BATCH IS THE UNIT, AT EVERY LEVEL -- not just at the root.
	 *
	 * Installing this descriptor instantiates its whole module, which makes ALL n members callable; there
	 * is no way to bring up one member alone. So every member's dependency closure must be installed too,
	 * and that obligation is RECURSIVE: it applies wherever a batch is encountered in the walk, not only
	 * where the walk started. Seeding only from the root's batch left exactly this hole -- a dep in
	 * ANOTHER group was installed, its whole module went callable, and the siblings that were not
	 * themselves dependencies had their own deps skipped.
	 *
	 * Observed as: batch[8] entry -> (f-slot) batch[2].JsonObject:.ctor -> (module-local)
	 * batch[2].LinkedTreeMap:.ctor -> trap. LinkedTreeMap became callable on JsonObject's closure alone.
	 *
	 * Terminates because every member is stamped on entry, so each descriptor is walked once per pass. */
	{
		WjBatchDesc *mb = snap_b;             /* one snapshot (R320) -- see WJC_ADMIT_BATCH_SWAPPED */
		if (mb) {
			int bi;
			for (bi = 0; bi < mb->n; ++bi) {
				int m = mb->desc [bi];
				if (m > 0 && m != desc_id && !wj_install_closure (m))
					ok = FALSE;
			}
		}
	}
	if (!wj_admit_install_only (desc_id, re, snap_b, snap_bytes, snap_len))
		ok = FALSE;
	return ok;
}

/* Did this install pass hold a coop-suspend off? The walk has no safepoint poll and is entered FROM
 * mono_wasm_jit_safepoint_poll (via the rendezvous drain), so the thread has already satisfied its GC
 * check for the pass before the walk starts; a suspend requested during the walk is therefore not
 * serviced until the next back-edge. Counting the condition is far more sensitive than counting the
 * stall it can cause -- the same argument WJC_JITMM_UNINIT is written on. */
static void
wj_walk_note_gc_pending (void)
{
	extern volatile size_t mono_polling_required;
	if (mono_polling_required)
		mono_wasm_jit_counters [WJC_ADMIT_WALK_GC_PENDING]++;
}

static gboolean
wj_install_closure_root (int desc_id, WjRegEntry *re)
{
	(void) re;
	wj_inst_stamp_ensure (desc_id + 1);
	if (++wj_inst_gen == 0) {
		memset (wj_inst_stamp, 0, sizeof (guint32) * (gsize) wj_inst_cap);
		wj_inst_gen = 1;
	}
	wj_inst_budget = wj_reg_n + WJ_INSTALL_SLACK;
	wj_clo_n = 0;
	{
		gboolean r = wj_install_closure (desc_id);
		wj_walk_note_gc_pending ();
		return r;
	}
}

/* Seed the walk from EVERY member of a batch, under ONE stamp generation and ONE budget.
 *
 * Instantiating a batch module makes all n members callable at once -- there is no way to bring up one
 * member alone -- so publishing any of them requires the UNION of all n dependency closures to be
 * installed, not just the triggering member's. The recursive path said so explicitly ("admit the union of
 * all sibling dependency closures before publishing any sibling live") and walked every sibling's deps.
 *
 * Omitting that is what regressed the single writer on a configuration with zero traps: a sibling was
 * published on the strength of the ROOT's closure while its own dependency was never installed, and its
 * call_indirect hit the jiterpreter prefill. The trap stack showed caller and callee in the same batch
 * module (`wasmjit-batch[2]`, unparseSig -> forBasicType), which is that fault exactly. */
static gboolean
wj_install_closure_group (int desc_id, WjRegEntry *re)
{
	WjBatchDesc *ibatch = re->batch;      /* one snapshot -- see WJC_ADMIT_BATCH_SWAPPED */
	gboolean ok = TRUE;
	int i;

	if (!ibatch)
		return wj_install_closure_root (desc_id, re);

	wj_inst_stamp_ensure (desc_id + 1);
	if (++wj_inst_gen == 0) {
		memset (wj_inst_stamp, 0, sizeof (guint32) * (gsize) wj_inst_cap);
		wj_inst_gen = 1;
	}
	wj_inst_budget = wj_reg_n + WJ_INSTALL_SLACK;
	wj_clo_n = 0;
	for (i = 0; i < ibatch->n; ++i) {
		int m = ibatch->desc [i];
		if (m > 0 && !wj_install_closure (m))
			ok = FALSE;
	}
	wj_walk_note_gc_pending ();
	return ok;
}
/* The f-slot signature is (body->param_types) -> body->ret_type, so the expected table arity IS
 * body->nparams. Accessor because WjBody is defined further down, with the assembler that owns it. */
static int wj_body_nparams (struct _WjBody *b);




/* ==================================================================================================
 * THE SINGLE WRITER  (MONO_WASM_JIT_SINGLE_WRITER, ships 0 until validated)
 *
 * R267: installation is not funnelled through the code that enforces the dependency invariant. FOUR
 * paths write this worker's table and only mono_wasm_jit_admit admits the closure first; the cycle break
 * installs WITHOUT the closure and returns 1 meaning "admitted", so "admitted" has two meanings. The
 * invariant "a live f-slot implies this worker installed it AND its whole transitive closure" is upheld
 * by convention across four installers, two liveness markers, a four-state machine, a per-descriptor
 * generation and a per-thread bitmap -- and this file records six leaks of that agreement (R151, R165,
 * R166 x3, and R262/R265/R266 today).
 *
 * THE OBSERVATION THAT MAKES THIS SMALL: INSTALL ORDER IS IRRELEVANT. Installing is writing table slots,
 * and slot writes are independent, so a dependency CYCLE does not obstruct installing both members. Only
 * PUBLICATION needs ordering, and "install the entire closure, then publish it" satisfies the invariant
 * with no topological sort, no SCC condensation and no install-only escape hatch. The cycle break exists
 * only because publication currently happens mid-DFS; remove that and it has no reason to exist.
 *
 * Consequences, each of which deletes a bug class rather than a bug:
 *   - no recursion through mono_wasm_jit_admit, so no VISITING state, so nothing for a generation bump to
 *     clear (R262) and no marker to leak (R265);
 *   - liveness is marked for the WHOLE closure before anything is published, so "live" can never precede
 *     "closure installed" -- R165's `state 2 with neither slot installed` is unrepresentable;
 *   - one batch snapshot per install (wj_admit_install_only already takes exactly one), so R266's tear
 *     has nowhere to happen.
 * ================================================================================================== */

/* Every check wj_admit_dependencies makes EXCEPT the recursive admit: the closure install has already
 * run, so this only confirms that what is installed is the RIGHT thing. Same counters, so the census
 * stays comparable across the knob. */
static gboolean
wj_verify_deps_installed (WjRegEntry *re)
{
	WjDepSet *ds = re->depset;
	int i;
	for (i = 0; ds && i < ds->n; ++i) {
		int dep_id = wj_desc_for_fslot (ds->slot [i]);
		WjRegEntry *dep = dep_id ? wj_reg_at (dep_id - 1) : NULL;
		gboolean ident_bad = ds->method [i] && dep &&
			dep->logical_method != ds->method [i] && dep->body_method != ds->method [i];
		if (ident_bad)
			mono_wasm_jit_counters [WJC_ABI_MISMATCH_IDENT]++;
		if (!dep_id || !dep || dep->f_sig_id != ds->sig [i] || ident_bad) {
			mono_wasm_jit_counters [!dep_id ? WJC_ABI_MISMATCH_UNREG
			                        : (!dep ? WJC_ABI_MISMATCH_CHUNK : WJC_ABI_MISMATCH_SIG)]++;
			return FALSE;
		}
		/* ADMISSION'S ACTUAL CONTRACT (R239): test the slot the CALLER BAKED, not dep->f, because a
		 * callee that re-registered leaves the baked slot stale and generated code calls the baked one. */
		if (!mono_wasm_jit_slot_live (ds->slot [i])) {
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_DEP_NOT_LIVE);
			return FALSE;
		}
	}
	return TRUE;
}

/* TIME-OF-USE VERIFICATION. See WJC_SWEEP_*: publish-time checking is provably insufficient here, so
 * re-check what was already published and find the descriptor that went bad after the fact. */
static __thread int wj_sweep_tick;

/* Does any dependency's SLOT hold a function whose arity differs from that dependency's own parameter
 * count? This is the only check here that looks at the table rather than at the registry's claims about
 * it, and it is the one a baked `call_indirect` actually depends on. */
static gboolean
wj_deps_arity_bad (WjRegEntry *re)
{
	WjDepSet *ds = re->depset;
	int i;
	for (i = 0; ds && i < ds->n; ++i) {
		int dep_id = wj_desc_for_fslot (ds->slot [i]);
		WjRegEntry *dep = dep_id ? wj_reg_at (dep_id - 1) : NULL;
		int len = -1, ok, want;
		if (!dep || !dep->body)
			continue;
		ok = mono_wasm_jit_eslot_probe (ds->slot [i], &len);
		want = wj_body_nparams (dep->body);
		if (ok && want >= 0 && len != want)
			return TRUE;
	}
	return FALSE;
}

static void
wj_sweep_published (void)
{
	int d, cap;
	if (G_LIKELY (!mono_wasm_jit_sweep))
		return;
	if (++wj_sweep_tick < mono_wasm_jit_sweep)
		return;
	wj_sweep_tick = 0;
	/* BOUND THE DIAGNOSTIC. Each sweep is O(registered) and it ran 33,785 times in one run -- about a
	 * billion dependency checks -- which made the instrument a leading cause of the stalls it was hired to
	 * explain. A few hundred sweeps sample the same states; the reports repeat identically anyway. */
	if (mono_wasm_jit_counters [WJC_SWEEP_RUNS] > 300)
		return;
	mono_wasm_jit_counters [WJC_SWEEP_RUNS]++;
	/* COUNTERS THAT SURVIVE A STALL.
	 *
	 * mono_wasm_jit_dump_stats only runs at the end of a healthy run, so every configuration that WEDGES
	 * during boot -- which is every interesting one here -- has yielded no counters at all, four rounds
	 * running. Whether a fix even fired then cannot be read, and a fix whose catch counter is unread is
	 * indistinguishable from a guess that compiled. Printing the few that adjudicate re-emission from
	 * inside the sweep costs one line per sweep and is readable from a stalled run's log. */
	{
		static int _every = 0;
		if ((_every++ % 8) == 0)
			printf ("WASM_JIT_LIVE_COUNTERS4 reused=%lld orphaned=%lld admit_orphaned=%lld gen_moved=%lld sw_pub=%lld sw_vfail=%lld rv_forced=%lld sweep_runs=%lld\n",
				(long long) mono_wasm_jit_counters [WJC_FSLOT_REUSED],
				(long long) mono_wasm_jit_counters [WJC_FSLOT_ORPHANED],
				(long long) mono_wasm_jit_counters [WJC_ADMIT_ORPHANED],
				(long long) mono_wasm_jit_counters [WJC_ADMIT_GEN_MOVED_MIDWALK],
				(long long) mono_wasm_jit_counters [WJC_SW_PUBLISHED],
				(long long) mono_wasm_jit_counters [WJC_SW_VERIFY_FAIL],
				(long long) mono_wasm_jit_counters [WJC_RV_DRAIN_REFRESH_FORCED],
				(long long) mono_wasm_jit_counters [WJC_SWEEP_RUNS]);
	}
	cap = wj_desc_state_cap < wj_reg_n + 1 ? wj_desc_state_cap : wj_reg_n + 1;
	for (d = 1; d < cap; ++d) {
		WjRegEntry *re;
		if (wj_desc_state [d] != 2)
			continue;
		re = wj_reg_at (d - 1);
		if (!re)
			continue;
		/* A stale generation is not a fault: the cache is simply out of date and the next dispatch
		 * re-admits. Only descriptors this thread believes are CURRENT and CALLABLE are interesting. */
		if (wj_desc_generation [d] != re->generation)
			continue;
		if (!mono_wasm_jit_slot_live (re->e) || !mono_wasm_jit_slot_live (re->f)) {
			static int _n1 = 0;
			mono_wasm_jit_counters [WJC_SWEEP_SELF_DEAD]++;
			if (_n1++ < 20)
				printf ("WASM_JIT_SWEEP_SELF_DEAD desc=%d e=%d f=%d e_live=%d f_live=%d e_inst=%d f_inst=%d gen=%u\n",
					d, re->e, re->f, mono_wasm_jit_slot_live (re->e), mono_wasm_jit_slot_live (re->f),
					wj_slot_is_installed (re->e), wj_slot_is_installed (re->f), re->generation);
			continue;
		}
		if (!wj_verify_deps_installed (re)) {
			static int _n2 = 0;
			WjDepSet *ds = re->depset;
			int i;
			mono_wasm_jit_counters [WJC_SWEEP_DEP_BAD]++;
			if (_n2++ < 20) {
				printf ("WASM_JIT_SWEEP_DEP_BAD desc=%d gen=%u batch=%d ndeps=%d\n",
					d, re->generation, re->batch ? re->batch->n : 0, ds ? ds->n : 0);
				for (i = 0; ds && i < ds->n; ++i) {
					int dep_id = wj_desc_for_fslot (ds->slot [i]);
					WjRegEntry *dep = dep_id ? wj_reg_at (dep_id - 1) : NULL;
					if (!dep_id || !dep || dep->f_sig_id != ds->sig [i] ||
					    !mono_wasm_jit_slot_live (ds->slot [i]))
						/* `inst` vs `live` is THE distinction. They are two different bitmaps and live is a
						 * subset of installed: a slot can hold the right function (call works) while simply
						 * not being MARKED live, which is bookkeeping and cannot trap. Only inst=0 means
						 * the table really holds mono_jiterp_placeholder_jit_call. Printing live alone
						 * cannot tell a stale marking from a missing function. */
						{
						/* PROBE THE TABLE ITSELF. Everything else on this line is BOOKKEEPING -- what the
						 * registry says is installed and what signature it records. Nothing has yet
						 * compared the FUNCTION ACTUALLY IN THE SLOT against that claim, and a
						 * `function signature mismatch` on a baked call_indirect means exactly that the
						 * occupant's type differs from the caller's baked expectation. `arity` is the
						 * installed function's real parameter count, `want_ar` the dep's own nparams;
						 * arity 4 with want_ar != 4 is the jiterpreter prefill
						 * (mono_jiterp_placeholder_jit_call). */
						int probe_len = -1;
						int probe_ok = mono_wasm_jit_eslot_probe (ds->slot [i], &probe_len);
						int want_ar = (dep && dep->body) ? wj_body_nparams (dep->body) : -1;
						printf ("   dep[%d] slot=%d desc=%d live=%d inst=%d probe=%d arity=%d want_ar=%d want_sig=%u have_sig=%u dep_gen=%u dep_state=%d%s\n",
							i, ds->slot [i], dep_id, mono_wasm_jit_slot_live (ds->slot [i]),
							wj_slot_is_installed (ds->slot [i]), probe_ok, probe_len, want_ar,
							ds->sig [i], dep ? dep->f_sig_id : 0, dep ? dep->generation : 0,
							(dep_id > 0 && dep_id < wj_desc_state_cap) ? wj_desc_state [dep_id] : -1,
							(probe_len == 4 && want_ar != 4) ? "  <-- JITERPRETER PLACEHOLDER"
							: (probe_ok && want_ar >= 0 && probe_len != want_ar) ? "  <-- WRONG ARITY IN SLOT" : "");
						}
				}
			}
		}
	}
}

static int
wj_make_callable (int desc_id)
{
	WjRegEntry *re;
	int k;

	if (desc_id <= 0 || desc_id > wj_reg_n) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_BAD_ID);
		return 0;
	}
	wj_desc_state_ensure (desc_id + 1);
	mono_memory_barrier ();
	re = wj_reg_at (desc_id - 1);
	if (!re) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_NO_ENTRY);
		return 0;
	}
	/* AN ORPHANED DESCRIPTOR IS NEVER ADMISSIBLE AGAIN -- it no longer owns its f-slot, so instantiating
	 * its module would write the old body over the slot's current owner. This must come BEFORE the
	 * generation arm below: orphaning bumps the generation to invalidate every worker's cache, and that
	 * arm would otherwise reset state 3 -> 0 and walk straight into the re-admission this prevents. No
	 * per-thread state is needed because being slot-less is a process-wide, permanent fact. */
	if (G_UNLIKELY (re->orphaned)) {
		mono_wasm_jit_counters [WJC_ADMIT_ORPHANED]++;
		return 0;
	}
	/* The permfail bound is the one piece of the old state machine that must survive verbatim: without it
	 * a descriptor whose bytes will never instantiate is retried on every dispatch forever (R167:
	 * 959,405 refusals at 0.02 fps). */
	if (desc_id < wj_desc_state_cap && wj_desc_state [desc_id] == 3 &&
	    wj_desc_permfail [desc_id] >= WJ_PERMFAIL_MAX) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_PERMFAIL);
		return 0;
	}
#ifdef HOST_BROWSER
	/* Restore the guarded trampoline before the table underneath it changes; repatched per member below. */
	if (desc_id < wj_desc_state_cap && wj_desc_generation [desc_id] != re->generation && re->logical_imethod)
		mono_jiterp_wasm_jit_unpatch_interp_entry (re->logical_imethod);
#endif

	/* wj_clo_n is reset by wj_install_closure_group / _root, next to the stamp and budget they also
	 * reset -- the pass is the unit, and the reset belongs with the rest of the per-pass state. */
	if (!wj_install_closure_group (desc_id, re)) {
		/* TRANSIENT BY CONSTRUCTION: either the 512 budget ran out or an instantiate failed. State 0, so
		 * the next dispatch retries -- and per R166 the retry is cheap because the instantiate itself is
		 * guarded by wj_slot_is_installed. */
		/* PERMANENT FAILURES MUST TERMINATE. The recursive path split its refusals: bad bytes set
		 * fail_perm, bumped wj_desc_permfail and landed at state 3, everything else at state 0. Losing
		 * that split routes a condition that can never clear into the retry path, which is R167
		 * (959,405 refusals at 0.02 fps) and R244 (14-23M/run). wj_install_closure does not report WHY it
		 * failed, so count consecutive failures per descriptor and promote at the same bound -- same
		 * terminal state, same cap, without plumbing the cause through the walk. */
		if (desc_id < wj_desc_state_cap) {
			if (wj_desc_permfail [desc_id] < 255)
				wj_desc_permfail [desc_id]++;
			wj_desc_state [desc_id] = (wj_desc_permfail [desc_id] >= WJ_PERMFAIL_MAX) ? 3 : 0;
		}
		mono_wasm_jit_counters [WJC_SW_INSTALL_FAIL]++;
		return 0;
	}

	/* PASS 1 -- LIVENESS FOR THE WHOLE CLOSURE, and only now that every install in it has completed.
	 * This ordering is the entire point: a dep's liveness is what pass 2 verifies, so marking must be
	 * global before any verification runs, and publication must follow both. */
	for (k = 0; k < wj_clo_n; ++k) {
		int d = wj_clo_list [k];
		WjRegEntry *dre = (d > 0 && d <= wj_reg_n) ? wj_reg_at (d - 1) : NULL;
		if (!dre)
			continue;
		wj_mark_slot_live (dre->e);
		wj_mark_slot_live (dre->f);
	}
	/* ...and the root's batch siblings, which the batch instantiate above wrote 2N slots for but which
	 * need not appear in the closure (a sibling is only in it if it is also a DEPENDENCY). Without this
	 * they fail pass 2's liveness test and never publish -- R166 measured that omission at 106,724,167
	 * vfbNotLive, because an unpublished sibling falls back to the interpreter on every virtual call. */
	if (re->batch) {
		WjBatchDesc *lbatch = re->batch;          /* one snapshot -- see WJC_ADMIT_BATCH_SWAPPED */
		int bi;
		for (bi = 0; bi < lbatch->n; ++bi) {
			wj_mark_slot_live (lbatch->e [bi]);
			wj_mark_slot_live (lbatch->f [bi]);
		}
	}

	/* PASS 1b -- VERIFY THE WHOLE CLOSURE, because the old path did.
	 *
	 * This is the step whose absence regressed a clean config twice. The recursive admit verified
	 * TRANSITIVELY: admit(R) -> admit(D) -> admit(E), and a sig or identity mismatch anywhere propagated
	 * a refusal all the way to the root, so R never became callable over a broken E. Verifying only the
	 * ROOT's own deps is one level deep, and one level is not enough -- R's dep D looks perfect (installed,
	 * live, right ABI) while D's own dep E is mismatched, so R publishes, R calls D, D call_indirects E
	 * and traps. Measured: re-emission OFF went sig 0 -> 4 with a stall, on a path that had been clean.
	 *
	 * VERIFY ALL, PUBLISH ONE. Those are different questions about different descriptors: installing the
	 * closure is a statement about the table, verifying it is a statement about ABI agreement across the
	 * whole reachable graph, and publishing is a statement about one descriptor's admissibility. The
	 * first cut conflated verify with publish in one direction (verified all, published all) and the
	 * second in the other (verified one, published one). */
	for (k = 0; k < wj_clo_n; ++k) {
		int d = wj_clo_list [k];
		WjRegEntry *dre = (d > 0 && d <= wj_reg_n) ? wj_reg_at (d - 1) : NULL;
		if (!dre)
			continue;
		if (!mono_wasm_jit_slot_live (dre->e) || !mono_wasm_jit_slot_live (dre->f) ||
		    !wj_verify_deps_installed (dre)) {
			/* Transient, and scoped to the ROOT exactly as the recursive refusal was: the next dispatch
			 * re-walks the closure, which R166 notes is cheap because the instantiate is guarded. */
			if (desc_id < wj_desc_state_cap)
				wj_desc_state [desc_id] = 0;
			mono_wasm_jit_counters [WJC_SW_VERIFY_FAIL]++;
			return 0;
		}
	}

	/* PASS 2 -- PUBLISH THE ROOT AND ITS BATCH SIBLINGS ONLY, never the whole closure.
	 *
	 * The first version of this published every closure member, and that REGRESSED a configuration that
	 * had zero traps: old path with re-emission off, sig 0 -> 4 plus a stall. Publishing a dependency is
	 * NOT the same as admitting it. wj_verify_deps_installed below tests liveness that pass 1 has just
	 * granted, so for an in-closure dep it is close to tautological and does not reproduce what the
	 * recursive admit actually checked before declaring a descriptor callable. Installing the closure is
	 * a statement about the TABLE; publishing is a statement about a descriptor's own admissibility, and
	 * only the root (plus the batch siblings that share its module, as the old path did) has had that
	 * established here.
	 *
	 * The ORDERING is what this refactor is for and it is unaffected: every install in the closure
	 * completes before anything is published, so no publication happens mid-walk and the install-only
	 * cycle break has nothing left to do. */
	{
	WjBatchDesc *pbatch = re->batch;              /* one snapshot */
	int pn = pbatch ? pbatch->n : 1, pi;
	for (pi = 0; pi < pn; ++pi) {
		/* Iterate the BATCH's own member list rather than filtering the closure, so a sibling that is
		 * not a dependency of the root is still published -- that is the old path's set exactly. */
		int d = pbatch ? pbatch->desc [pi] : desc_id;
		WjRegEntry *dre = (d > 0 && d <= wj_reg_n) ? wj_reg_at (d - 1) : NULL;
		if (!dre)
			continue;
		wj_desc_state_ensure (d + 1);
		if (d >= wj_desc_state_cap)
			continue;
		/* wj_mark_slot_live refuses an uninstalled slot SILENTLY (R151), so believe the bitmap, never the
		 * fact that we asked. */
		if (!mono_wasm_jit_slot_live (dre->e) || !mono_wasm_jit_slot_live (dre->f)) {
			wj_desc_state [d] = 0;
			continue;
		}
		if (!wj_verify_deps_installed (dre) || wj_deps_arity_bad (dre)) {
			wj_desc_state [d] = 0;
			continue;
		}
		wj_desc_state [d] = 2;
		/* DO NOT RE-READ THE GENERATION HERE. wj_admit_install_only already recorded the generation it
		 * actually installed, and re-reading would certify this worker against a generation whose depset
		 * it never walked -- a method re-emitted mid-walk replaces `depset` and `generation` together.
		 * Leaving the recorded value in place is self-healing: if it is behind, the next dispatch
		 * re-admits against the new depset. */
		if (wj_desc_generation [d] != dre->generation)
			mono_wasm_jit_counters [WJC_ADMIT_GEN_MOVED_MIDWALK]++;
		/* wj_admit_install_only already recorded the generation it installed; leave it. Skipping the write
		 * is correct HERE (unlike the old path above) precisely because the installer sets it. */
		mono_wasm_jit_counters [WJC_SW_PUBLISHED]++;
#ifdef HOST_BROWSER
		if (dre->logical_imethod)
			mono_jiterp_wasm_jit_patch_interp_entry (dre->logical_imethod);
#endif
	}
	}
	return (desc_id < wj_desc_state_cap && wj_desc_state [desc_id] == 2) ? 1 : 0;
}

static int
wj_admit_impl (int desc_id)
{
	WjRegEntry *re;
	WjBatchDesc *batch;
	int i;
	gboolean watch;
	gboolean instantiated_here = FALSE;   /* did THIS call write the table? see the sibling loop below */
	/* Is a failure below PERMANENT (state 3, this worker never retries) or TRANSIENT (state 0, the next
	 * dispatch retries)? Default transient. See the `fail:` label for why the old unconditional 3 was the
	 * single largest cost of co-location. */
	gboolean fail_perm = FALSE;
	guint32 gen_snapshot = 0;   /* re->generation as of BEFORE the dependency walk; see the publish below */
	char eb [192]; double ms = 0;
	if (desc_id <= 0 || desc_id > wj_reg_n) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_BAD_ID);
		return 0;
	}

	/* ALREADY-ADMITTED FAST PATH, hoisted above the ensure/fence/lookup below.
	 *
	 * This is called at every dispatch and the cached case is overwhelmingly the common one, yet reaching
	 * the cache check further down first paid a call (wj_desc_state_ensure), a full memory_barrier and a
	 * registry indirection. mono_wasm_jit_admit is 1125 instructions and 0.92% of the in-game profile;
	 * the fence in particular has no business on a hot dispatch path.
	 *
	 * Sound because wj_desc_state / wj_desc_generation / wj_desc_state_cap are all `__thread`: this reads
	 * only state THIS thread wrote when it admitted the descriptor, so no acquire fence is needed to
	 * observe it. The bounds test against the thread's own cap replaces what ensure() guaranteed, so the
	 * indexing stays in range when this thread has not grown the arrays yet.
	 *
	 * re->generation is shared and read without the fence, so it may be stale by one rebind. That is
	 * already the documented contract below: a generation mismatch invalidates only the local admission
	 * cache, and the previous instance stays valid for invocations already in flight. Being late to notice
	 * a rebind therefore costs nothing but a delayed re-admit on a later call. */
	if (desc_id < wj_desc_state_cap && wj_desc_state [desc_id] == 2) {
		WjRegEntry *fre = wj_reg_at (desc_id - 1);
		if (fre && wj_desc_generation [desc_id] == fre->generation)
			return 1;
	}

	/* THE FORK. Above is the shared fast path (a pure per-thread cache check, unchanged). Below is the
	 * ORIGINAL multi-installer path, kept so the refactor is an in-binary A/B rather than a one-way
	 * rewrite -- CLAUDE.md prefers a within-binary knob comparison to a cross-binary one, and this path
	 * carries every admission on the shipped config. Delete it once the single writer is proven. */
	if (G_UNLIKELY (mono_wasm_jit_sweep))
		wj_sweep_published ();
	if (G_UNLIKELY (mono_wasm_jit_single_writer))
		return wj_make_callable (desc_id);

	wj_desc_state_ensure (desc_id + 1);
	mono_memory_barrier ();
	re = wj_reg_at (desc_id - 1);
	if (!re) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_NO_ENTRY);
		return 0;
	}
	/* AN ORPHANED DESCRIPTOR IS NEVER ADMISSIBLE AGAIN -- it no longer owns its f-slot, so instantiating
	 * its module would write the old body over the slot's current owner. This must come BEFORE the
	 * generation arm below: orphaning bumps the generation to invalidate every worker's cache, and that
	 * arm would otherwise reset state 3 -> 0 and walk straight into the re-admission this prevents. No
	 * per-thread state is needed because being slot-less is a process-wide, permanent fact. */
	if (G_UNLIKELY (re->orphaned)) {
		mono_wasm_jit_counters [WJC_ADMIT_ORPHANED]++;
		return 0;
	}
	gen_snapshot = re->generation;
	watch = mono_wasm_jit_watch && re->logical_method && re->logical_method->name &&
		strstr (re->logical_method->name, mono_wasm_jit_watch);
	if (watch)
		printf ("WASM_JIT_ADMIT_BEGIN desc=%d method=%s state=%d e=%d/live%d f=%d/live%d deps=%d\n",
			desc_id, re->logical_method->name, wj_desc_state [desc_id], re->e,
			mono_wasm_jit_slot_live (re->e), re->f, mono_wasm_jit_slot_live (re->f), re->depset ? re->depset->n : 0);
	/* A standalone descriptor may be rebound into an automatic batch after this worker admitted it.
	 * Generation mismatch invalidates only the local admission cache; the old instance remains valid
	 * for any invocation already in flight while this boundary overwrites the same e/f table slots. */
	if (wj_desc_state [desc_id] == 2 && wj_desc_generation [desc_id] == re->generation) {
		if (watch) printf ("WASM_JIT_ADMIT_CACHED desc=%d\n", desc_id);
		return 1;
	}
	if (wj_desc_generation [desc_id] != re->generation) {
#ifdef HOST_BROWSER
		/* A generated AOT adapter is guard-free after admission. Restore its guarded wrapper before
		 * replacing this worker's table slots with a newer batch generation, then repatch it below
		 * only after the new dependency union has been admitted. */
		if (re->logical_imethod)
				mono_jiterp_wasm_jit_unpatch_interp_entry (re->logical_imethod);
#endif
		/* A NEW generation earns a fresh attempt -- unless this worker has already burned
		 * WJ_PERMFAIL_MAX of them on bytes that would not instantiate. See wj_desc_permfail.
		 *
		 * BUT NEVER CLEAR STATE 1. State 1 is not a cached verdict, it is "THIS thread is inside a DFS
		 * on this descriptor right now", and the cycle break below is the only thing standing between a
		 * cyclic depset and unbounded recursion. A generation change invalidates the admission CACHE
		 * (states 2 and 3); it says nothing about a walk in flight, and clearing the marker under one
		 * defeats the break: on the non-batch path wj_desc_generation is not written until the walk
		 * completes, so the mismatch persists, and every re-entry through a cycle edge cleared the
		 * marker, re-marked visiting, and descended again. That is R262's boot wedge -- a V8 `Maximum
		 * call stack size exceeded` whose stack is this function and wj_admit_dependencies alternating,
		 * on a workload whose measured admission depth with re-emission OFF is 22. Re-emission is the
		 * only thing that bumps a LIVE descriptor's generation while walks are in flight, which is why
		 * nothing else ever reached it. */
		if (wj_desc_state [desc_id] == 1 && wj_admit_depth > 0)
			mono_wasm_jit_counters [WJC_ADMIT_GEN_RESET_VISITING]++;
		else if (wj_desc_state [desc_id] != 3 || wj_desc_permfail [desc_id] < WJ_PERMFAIL_MAX)
			wj_desc_state [desc_id] = 0;
	}
	/* STATE 1 AT DEPTH 0 IS A LEAK, NOT A CYCLE. (This was built believing it was R264 wall 3's cause;
	 * WJC_ADMIT_STALE_VISITING then measured 0 while the trap still fired, so it is NOT that cause. It
	 * is kept as correct hygiene -- a leaked marker would be silent and permanent -- not as a fix.)
	 * The cycle break below is INSTALL-ONLY: it returns 1 without admitting the closure, which is correct
	 * for a real cycle (the in-flight walk above it will finish the job) and catastrophic for a stale
	 * marker (nothing will). A dependency then stays at mono_jiterp_placeholder_jit_call while its caller
	 * is live, and the emitted call_indirect traps with `function signature mismatch`. This thread is not
	 * in a DFS at depth 0, so no cycle through this descriptor is possible; the marker was leaked. */
	if (wj_desc_state [desc_id] == 1 && wj_admit_depth == 0) {
		mono_wasm_jit_counters [WJC_ADMIT_STALE_VISITING]++;
		wj_desc_state [desc_id] = 0;
	}
	if (wj_desc_state [desc_id] == 1) {
		/* Dependency cycle within this admission DFS. Break it -- but INSTALL first, because the caller
		 * about to go live will call_indirect this f-slot and generated indirect calls carry no liveness
		 * check. See wj_admit_install_only. */
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_CYCLE_BREAK_INSTALL);
		if (!wj_install_closure_root (desc_id, re)) {
			/* The ONLY refusal on this path that had no counter, and it is also the only one that
			 * returns while leaving wj_desc_state[desc_id] == 1. Everything else either counts itself
			 * or goes through `fail:`, which clears the visiting state for this descriptor and its
			 * batch siblings. Counted, not fixed, deliberately: whether state 1 is the right thing to
			 * leave behind here is a separate question from being able to see how often it happens.
			 *
			 * MEASURED ZERO ON FIRST READING, and unlike most zeros this one is INFORMATIVE, because its
			 * sibling proves the path runs: 2026-09-12, full Minecraft boot + worldgen + in-game,
			 * cycle_break_install = 2,513 with cycle_break_fail = 0. So the cycle break is common and
			 * wj_install_closure_root has never failed on this workload -- the uncounted route was
			 * harmless, which is a fact nobody could state before. Keep the counter: a zero next to a
			 * non-zero sibling distinguishes "cannot happen here" from "silently stopped happening",
			 * which a missing counter cannot. */
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_CYCLE_BREAK_FAIL);
			return 0;
		}
		return 1;
	}
	if (wj_desc_state [desc_id] == 3) {
		/* Split so the R167 bound is distinguishable from an ordinary permanent refusal. PERMFAIL means
		 * the generation reset above declined to clear state 3 because this worker has already burned
		 * WJ_PERMFAIL_MAX attempts on bytes that would not instantiate -- NON-ZERO HERE IS HEALTHY, it
		 * is the bound doing its job, and its absence is what R167 measured as 959,405 refusals at
		 * 0.02 fps. STATE3 is permanent refusal below the bound. */
		if (G_UNLIKELY (mono_wasm_jit_stats))
			mono_wasm_jit_count (wj_desc_permfail [desc_id] >= WJ_PERMFAIL_MAX
			                     ? WJC_ADMIT_PERMFAIL : WJC_ADMIT_STATE3);
		return 0;
	}
	wj_desc_state [desc_id] = 1;
	/* R320: the bytes and depset this admission uses, read in ONE consistent window. */
	void *snap_bytes = NULL;
	int snap_len = 0;
	WjDepSet *snap_ds = NULL;
	if (!wj_pub_snapshot (re, &batch, &snap_bytes, &snap_len, &snap_ds)) {
		mono_wasm_jit_counters [WJC_ADMIT_PAYLOAD_TORN]++;   /* ungated: a race catch; non-zero is healthy */
		batch = NULL;   /* no sibling was premarked yet: the fail path must not reset any */
		goto fail;
	}
	if (batch) {
		/* Instantiating any member installs every export, but that does NOT make every sibling
		 * dispatchable: each sibling can have different unchecked external call_indirect targets.
		 * Mark the complete generation visiting first (so intra-batch edges are DFS cycles), then
		 * admit the union of all sibling dependency closures before publishing any sibling live.
		 *
		 * The old code walked only the triggering member and then marked all siblings state=2. On a
		 * secondary worker, RealEmitOpCode became live through sibling DoEmit while its
		 * RuntimeILGenerator.Emit dependency was still the table placeholder, producing V8's
		 * "function signature mismatch" trap. */
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			WjRegEntry *sre;
			if (sibling <= 0 || sibling > wj_reg_n || !(sre = wj_reg_at (sibling - 1)) ||
			    sre->batch != batch)
				goto fail;
			wj_desc_state_ensure (sibling + 1);
			/* Same rule as the standalone reset above: a sibling that is VISITING is an ancestor of
			 * the current walk, and clearing its marker defeats the cycle break identically. */
			if (wj_desc_generation [sibling] != batch->generation) {
				if (wj_desc_state [sibling] == 1 && wj_admit_depth > 0)
					mono_wasm_jit_counters [WJC_ADMIT_GEN_RESET_VISITING]++;
				else
					wj_desc_state [sibling] = 0;
			}
			if (wj_desc_state [sibling] == 3)
				goto fail;
			wj_desc_state [sibling] = 1;
			wj_desc_generation [sibling] = batch->generation;
		}
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			WjRegEntry *sre = wj_reg_at (sibling - 1);
			WjBatchDesc *sb = NULL;
			void *sbytes = NULL;
			int slen = 0, r;
			WjDepSet *sds = NULL;
			/* R320: this member's depset must belong to THIS group generation -- the one whose bytes are installed below. */
			if (!wj_pub_snapshot (sre, &sb, &sbytes, &slen, &sds) || sb != batch) {
				mono_wasm_jit_counters [WJC_ADMIT_PAYLOAD_TORN]++;
				goto fail;
			}
			r = wj_admit_dependencies (sre, sds, sibling, watch && sibling == desc_id);
			if (!r)
				goto fail;
		}
	} else {
		int r = wj_admit_dependencies (re, snap_ds, desc_id, watch);
		if (!r)
			goto fail;
	}
	/* The compiling worker already installed this descriptor; other workers instantiate it once here.
	 * A batch member brings up the ENTIRE module (its exports are e<i>/f<i>, so there is no way to
	 * instantiate one member alone) — which also installs its siblings, so they skip this. */
	if (!wj_slot_is_installed (re->e) || !wj_slot_is_installed (re->f) ||
	    wj_desc_generation [desc_id] != re->generation) {
		instantiated_here = TRUE;
		eb [0] = 0;
		/* `batch`, NOT `re->batch`: the snapshot taken before the premark loop. Re-reading the pointer
		 * here is what let a concurrent re-frame pair one module's bytes with another's slot list. */
		if (batch) {
			extern int mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n, const void *bytes, int len, char *errbuf, int errcap, double *out_ms);
			if (!mono_wasm_jit_instantiate_batch_local (batch->e, batch->f, batch->n,
			                                            batch->bytes, batch->len, eb, (int) sizeof (eb), &ms)) {
				printf ("WASM_JIT_ADMIT_FAIL desc=%d (batch n=%d) e=%d f=%d : %s\n", desc_id, batch->n, re->e, re->f, eb);
				fail_perm = TRUE;   /* a LinkError/CompileError on these bytes will not fix itself */
				goto fail;
			}
		} else if (wj_batch_raced (re, batch)) {
			/* THE SNAPSHOT WENT STALE. `batch` was read before `re->bytes`, and batch_bind publishes
			 * `re->batch` then (now) a fence then `re->bytes` -- so a reader in THIS order can still pair
			 * a stale NULL batch with already-batched bytes, and then instantiate a multi-export module
			 * through the single-method path, which looks up "e"/"f" that a batched module does not have.
			 *
			 * Re-reading `re->batch` after the bytes closes it: if the group is visible NOW, the bytes we
			 * were about to use may already be the shared module, so refuse and let the next dispatch
			 * retry against a settled entry. TRANSIENT by construction -- state 0, not fail_perm -- which
			 * is the whole difference from the failure this replaces, where ~23 admissions per run were
			 * marked PERMANENTLY bad on bytes that were merely read at the wrong moment. */
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ADMIT_BATCH_RACED);
			goto fail;
		} else if (!mono_wasm_jit_instantiate_local (re->e, re->f, snap_bytes, snap_len, eb, (int) sizeof (eb), &ms)) {
			printf ("WASM_JIT_ADMIT_FAIL desc=%d e=%d f=%d : %s\n", desc_id, re->e, re->f, eb);
			fail_perm = TRUE;   /* as above: bad bytes, not a transient ordering miss */
			/* A LinkError here should be impossible: admission refuses to instantiate until every
			 * IMPORTED slot is admitted on THIS worker. Dump what it actually saw -- "the slot is not in
			 * the import list", "it is and reported admitted", and "it is not in the dependency list
			 * either" need three different fixes and the message above distinguishes none of them. This
			 * print is what ended two rounds of wrong diagnosis; keep it. */
			{
				int k;
				WjDepSet *fds = re->depset;
				printf ("WASM_JIT_ADMIT_FAIL_DEPS desc=%d ndeps=%d deps[", desc_id, fds ? fds->n : 0);
				for (k = 0; fds && k < fds->n; ++k)
					printf ("%s%d/state%d/live%d", k ? " " : "", fds->slot [k],
						wj_desc_for_fslot (fds->slot [k]) > 0 &&
						wj_desc_for_fslot (fds->slot [k]) < wj_desc_state_cap
							? wj_desc_state [wj_desc_for_fslot (fds->slot [k])] : -1,
						mono_wasm_jit_slot_live (fds->slot [k]));
				printf ("] imports[");
				printf ("]\n");
			}
			goto fail;
		}
		/* Path tag only; the total is counted inside instantiate_*_local. This is THE per-worker path --
		 * already demand-driven at dispatch, so a high count here is not by itself removable waste. */
		wj_census_instantiated++;
		wj_census_inst_us += (gint64) (ms * 1000.0);
		mono_atomic_inc_i32 (&wj_census_inst_admit);
	}
	/* Publish dispatchability only after every unchecked direct dependency is admitted. */
	/* MONO_WASM_JIT_VERIFY_DEPS=1: test the invariant directly, at the only moment it matters -- just
	 * before this descriptor becomes callable.
	 *
	 * Every f-slot in re->deps is a `call_indirect` target of a body about to go live, and generated
	 * indirect calls carry no liveness check, so each must already hold a function of the RIGHT TYPE on
	 * this worker.
	 *
	 * COMPARE AGAINST THE DEP'S OWN ARITY, not against a constant. The first version of this check reused
	 * mono_wasm_jit_eslot_probe's verdict, which is written for E-slots -- always `(i32,i32)->void`, arity
	 * 2 -- and applied it to F-slots, whose arity is the method's own parameter count. It reported 40
	 * "failures" per boot at arity 0 and 1, i.e. perfectly healthy zero- and one-argument methods, and it
	 * reported exactly the same 40 on a PASSING boot. A diagnostic that fires identically whether the run
	 * fails or not is measuring nothing; that it did so was visible in the first run and should have been
	 * the end of it.
	 *
	 * The f signature is (body->param_types[nparams]) -> body->ret_type, so the expected arity is exactly
	 * body->nparams. A mismatch means the slot holds something else: the jiterpreter prefill
	 * (mono_jiterp_placeholder_jit_call, arity 4) or a recycled slot's new occupant. Both are the bug. */
	{
		extern int mono_wasm_jit_verify_deps;
		if (G_UNLIKELY (mono_wasm_jit_verify_deps)) {
			int di;
			WjDepSet *vds = batch ? re->depset : snap_ds;   /* R320: the set this admission walked */
			for (di = 0; vds && di < vds->n; ++di) {
				int dd = wj_desc_for_fslot (vds->slot [di]);
				WjRegEntry *dre = dd > 0 ? wj_reg_at (dd - 1) : NULL;
				int want = dre ? wj_body_nparams (dre->body) : -1;
				int len = -1, pr = mono_wasm_jit_eslot_probe (vds->slot [di], &len);
				if (want < 0 || pr == 0 || len != want) {
					static int _n = 0;
					if (_n++ < 40)
						/* `sib` is THE question: consecutive descriptor ids look like batch siblings but are
						 * not proof. If the bad dep shares this entry's WjBatchDesc then an INTRA-GROUP call
						 * failed to resolve to `call <funcidx>` and stayed an indirect dep -- and the batch
						 * instantiate that installs all 2N sibling slots evidently did not run before this
						 * member published. If it does NOT share it, the callee is a separate group that
						 * mono_wasm_jit_admit returned 1 for without installing. Different bugs, same symptom. */
						printf ("WASM_JIT_DEP_BAD desc=%d dep_fslot=%d dep_desc=%d probe=%d arity=%d want=%d batch=%d sib=%d desc_gen=%u dep_gen=%u dep_state=%d inst_e=%d inst_f=%d dep_e=%d dep_f=%d%s\n",
							desc_id, vds->slot [di], dd, pr, len, want,
							re->batch ? re->batch->n : 0,
							(dre && re->batch && dre->batch == re->batch) ? 1 : 0,
							re->generation, dre ? dre->generation : 0,
							(dd > 0 && dd < wj_desc_state_cap) ? wj_desc_state [dd] : -1,
							dre ? wj_slot_is_installed (dre->e) : -1,
							dre ? wj_slot_is_installed (dre->f) : -1,
							dre ? dre->e : -1, dre ? dre->f : -1,
							(len == 4 && want != 4) ? "  <-- JITERPRETER PLACEHOLDER" : "");
				}
			}
		}
	}
	wj_mark_slot_live (re->e);
	wj_mark_slot_live (re->f);
	/* STATE 2 MUST NOT OUTRUN THE BITMAP.
	 *
	 * wj_mark_slot_live REFUSES to publish a slot this thread never installed (R151's publisher
	 * invariant) -- and it refuses SILENTLY, while `wj_desc_state = 2` was set unconditionally right
	 * after. The two then disagree about the same fact: the state machine says "admitted, safe to
	 * dispatch" and the liveness bitmap says "not live".
	 *
	 * That disagreement is load-bearing, because mono_wasm_jit_admit's fast path trusts the STATE:
	 *     if (wj_desc_state [desc_id] == 2 && generation matches) return 1;
	 * so a dependent's dep walk gets an immediate success for a descriptor whose slots were never
	 * written on this worker, publishes itself, and call_indirects mono_jiterp_placeholder_jit_call.
	 *
	 * MEASURED, with the deps probe (R165):
	 *   WASM_JIT_DEP_BAD desc=8623 dep_desc=8621 arity=4 want=0 sib=0 dep_state=2 inst_e=0 inst_f=0
	 * -- state 2 with neither slot installed, holding the prefill, in a group the dependent is not even
	 * a member of. This is R151's finding one level up: it fixed the bitmap and left the state machine
	 * free to contradict it.
	 *
	 * State 0 rather than 3: not being installed HERE yet is a per-worker, transient condition, and the
	 * next dispatch through this descriptor should retry rather than lose the method for the run. */
	if (!mono_wasm_jit_slot_live (re->e) || !mono_wasm_jit_slot_live (re->f)) {
		static int _nl = 0;
		if (G_UNLIKELY (mono_wasm_jit_verify_deps) && _nl++ < 20)
			printf ("WASM_JIT_PUBLISH_REFUSED desc=%d e=%d f=%d inst_e=%d inst_f=%d (slots not installed here)\n",
				desc_id, re->e, re->f, wj_slot_is_installed (re->e), wj_slot_is_installed (re->f));
		/* goto fail, NOT `return 0` after setting state 0 on desc_id alone. The batch loop above marked
		 * every SIBLING state 1 (visiting); returning here leaves them there for the life of the worker,
		 * and state 1 is the worst of the three states to be stuck in -- mono_wasm_jit_admit's cycle-break
		 * returns 1 for it while mono_wasm_jit_desc_admitted returns 0, so mono_wasm_jit_admit_live is
		 * permanently 0 and every virtual call to the method falls to the interp residual, silently and
		 * with no counter. `fail:` clears the whole generation. */
		goto fail;
	}
	wj_desc_state [desc_id] = 2;
	/* Publish the generation this admission WALKED, and publish it unconditionally.
	 *
	 * The first version of this skipped the write whenever the recorded value differed from
	 * `re->generation` -- which on a FIRST admission is always (recorded 0 vs a live generation), so no
	 * descriptor was ever cached and every dispatch re-admitted from scratch: `gen_moved` measured
	 * 6,140,331 in one run, R244's retry storm introduced wholesale. The snapshot is taken before the
	 * dependency walk, so it still cannot certify this worker against a generation whose depset it never
	 * walked; if the generation moved underneath, the recorded value is simply behind and the next
	 * dispatch re-admits. Counted, not skipped. */
	if (gen_snapshot != re->generation)
		mono_wasm_jit_counters [WJC_ADMIT_GEN_MOVED_MIDWALK]++;
	wj_desc_generation [desc_id] = gen_snapshot;
	/* MUST be the snapshot, not re->batch: the premark loop above validated membership against `batch`,
	 * so publishing from a different descriptor would mark liveness for a set this call never premarked
	 * and never instantiated. Counted, because a swap here is the race itself. */
	if (batch != re->batch)
		mono_wasm_jit_counters [WJC_ADMIT_BATCH_SWAPPED]++;
	if (batch) {
		/* THE PREMARK MUST ALWAYS BE UNDONE, AND `instantiated_here` USED TO GATE THE UNDOING.
		 *
		 * This loop was `if (re->batch && instantiated_here)`. The reasoning was about the LIVENESS
		 * BITMAP and it was right about that: marking siblings live after skipping the instantiate would
		 * publish slots this worker never wrote, so wj_mark_slot_live has to stay guarded. What the guard
		 * also skipped, wrongly, is the STATE assignment -- and the premark loop at the top of this
		 * function has by then set `wj_desc_state [sibling] = 1` for EVERY member of the batch.
		 *
		 * The instantiate above is conditional on `!wj_slot_is_installed (re->e) || ...`, so a worker that
		 * already installed these slots (admitting a different member of the same batch earlier) skips it,
		 * leaves instantiated_here FALSE, and returns with all n siblings still at state 1. State 1 is the
		 * one state that cannot recover on its own: mono_wasm_jit_admit's cycle-break returns 1 for it
		 * while mono_wasm_jit_desc_admitted requires 2, so mono_wasm_jit_admit_live is 0 for those methods
		 * for the life of the worker, and every virtual call to them goes to the interpreter. The old
		 * comment asserted the siblings "were already marked by whichever admission did instantiate";
		 * that holds for the bitmap and is false for the state, which THIS call just clobbered.
		 *
		 * MEASURED (R166): 106,724,167 vfbNotLive in a 120 s window with ADMIT_FAIL_PERM and
		 * ADMIT_FAIL_RETRY both 0 -- i.e. `fail:` never reached, admit() returning 1, desc_admitted()
		 * returning 0. Fixing the four `fail:` leaks first moved that number by 2.6%, because they were
		 * not the path being taken.
		 *
		 * So: always run the loop, and gate only the marking. On the skip path the bitmap is simply read
		 * rather than written, which is exactly the invariant the guard was protecting. */
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			if (sibling <= 0)
				continue;
			wj_desc_state_ensure (sibling + 1);
			/* Same invariant as the self publish above, and this loop had it backwards: it set state 2
			 * BEFORE asking wj_mark_slot_live to publish liveness, so a sibling whose slots this worker
			 * never wrote was announced as admitted regardless of what the bitmap decided. Mark first,
			 * then believe the bitmap. */
			if (instantiated_here) {
				wj_mark_slot_live (batch->e [i]);
				wj_mark_slot_live (batch->f [i]);
			}
			if (!mono_wasm_jit_slot_live (batch->e [i]) || !mono_wasm_jit_slot_live (batch->f [i])) {
				/* Third leak of the same invariant: `continue` used to leave this sibling on the state 1
				 * set by the visiting loop. Put it back to 0 so the next dispatch retries it. */
				wj_desc_state [sibling] = 0;
				continue;
			}
			wj_desc_state [sibling] = 2;
			wj_desc_generation [sibling] = batch->generation;
#ifdef HOST_BROWSER
			/* Patch EVERY sibling, not just the descriptor that triggered this admission.
			 * mono_wasm_jit_batch_bind unpatches all n members before rebinding them (their e/f slots are
			 * reused by the new generation), so repatching only the triggering member strands the other
			 * n-1 on the guarded interp-entry trampoline for the rest of the process. At the batch sizes
			 * this planner produces (211 members on the jbox2d workload) that is the difference between
			 * ~0% and ~12% of steady-state cycles spent in interp_entry. */
			{
				WjRegEntry *sre = (sibling <= wj_reg_n) ? wj_reg_at (sibling - 1) : NULL;
				if (sre && sre->logical_imethod)
						mono_jiterp_wasm_jit_patch_interp_entry (sre->logical_imethod);
			}
#endif
		}
	}
#ifdef HOST_BROWSER
	/* The native/AOT vtable entry initially points at a guarded interp-entry trampoline. Admission is
	 * per worker, just like the function table, so this is the earliest point where THIS worker may
	 * replace it with the generated guard-free pointer-ABI -> scalar-ABI adapter. If the adapter has
	 * not crossed its own compilation threshold yet the TS side records nothing; its later install
	 * checks the already-live f-slot and completes the patch in the opposite ordering.
	 *
	 * Batched descriptors were already covered by the sibling loop above (desc_id is one of them). */
	if (!re->batch && re->logical_imethod)
			mono_jiterp_wasm_jit_patch_interp_entry (re->logical_imethod);
#endif
	if (watch) printf ("WASM_JIT_ADMIT_OK desc=%d e_live=%d f_live=%d\n", desc_id, mono_wasm_jit_slot_live (re->e), mono_wasm_jit_slot_live (re->f));
	return 1;
fail:
	/* STATE 3 IS PERMANENT, AND MOST FAILURES HERE ARE NOT.
	 *
	 * This label was reached by six `goto`s and answered all of them with 3 -- "this worker will never
	 * dispatch to these methods again". Only two deserve it (the instantiate calls: bad bytes stay bad).
	 * The rest are ORDERING misses -- a dependency not yet live on THIS worker, a sibling lookup racing a
	 * rebatch -- which the next dispatch would resolve.
	 *
	 * The scope is what made it expensive. A standalone descriptor condemns one method; a batch condemns
	 * all n members, because they share this label. So one transient dep miss anywhere in a 132-member
	 * group removed the whole group from that worker's JIT tier for the rest of the run.
	 *
	 * MEASURED (R166, 120 s plateau window, colocate_deps=1 vs control): vfbThresh -- a virtual call to a
	 * target with slot > 0 that nonetheless fell back to the interpreter -- 11,860 -> 109,624,991. Its
	 * three sub-counters summed to 1,399, i.e. 99.999% of the fallbacks were the uncounted `default:`
	 * arm, slot > 0 with admit_live == 0. Downstream: residual 8.1x, interpRouted 9.6x, invoke 13.5x,
	 * fastvcall 0.51x, and 85.99 ms/frame against a 53.09 ms control.
	 *
	 * State 0 also costs less when it IS a real failure: the retry re-walks the dep closure, which is
	 * cheap, and the instantiate is guarded by wj_slot_is_installed. */
	if (G_UNLIKELY (mono_wasm_jit_stats))
		mono_wasm_jit_count (fail_perm ? WJC_ADMIT_FAIL_PERM : WJC_ADMIT_FAIL_RETRY);
	if (fail_perm && desc_id > 0 && desc_id < wj_desc_state_cap && wj_desc_permfail [desc_id] < 255)
		wj_desc_permfail [desc_id]++;
	if (batch) {
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			if (sibling <= 0 || sibling >= wj_desc_state_cap)
				continue;
			/* THE GENERATION GUARD LEAKED THE VISITING MARKER. This walk set state 1 on every sibling
			 * above; if a rebatch or a re-emission bumped the generation while the walk was running,
			 * the guard below stopped it being un-set and the sibling stayed VISITING FOREVER. That is
			 * survivable only as long as something else clears it -- which the generation reset used to
			 * do by accident, until R262 correctly stopped clearing in-flight markers. Then it became
			 * permanent, and a permanent state 1 routes every later admit into the install-only cycle
			 * break: admitted without its closure, dependency f-slot still the jiterpreter placeholder,
			 * and a signature-mismatch trap on the first call. That chain is REAL but is NOT what the
			 * observed trap is -- WJC_ADMIT_STALE_VISITING measured 0 while it still fired. Fixed anyway,
			 * because the leak itself is real: always un-mark a VISITING sibling.
			 * Only the PERMANENT verdict stays generation-scoped, because condemning a descriptor whose
			 * bytes someone else has since replaced is not this walk's call to make. */
			if (wj_desc_generation [sibling] == batch->generation)
				wj_desc_state [sibling] = fail_perm ? 3 : 0;
			else if (wj_desc_state [sibling] == 1)
				wj_desc_state [sibling] = 0;
		}
	} else {
		wj_desc_state [desc_id] = fail_perm ? 3 : 0;
	}
	return 0;
}

int
mono_wasm_jit_admit (int desc_id)
{
	int result;
	wj_reg_read_enter ();
	result = wj_admit_impl (desc_id);
	wj_reg_read_leave ();
	return result;
}
#include "mini-wasm-publish.inc"
/*
 * GC-safe object references for JITted methods — C-STACK FRAMES.
 *
 * The JIT keeps vregs in wasm locals (registers) for speed, but wasm locals are NOT GC-scanned. So an
 * object reference held in a wasm local across a GC point (an allocation in a residual callee, a loop
 * safepoint, ...) would be collected or moved out from under the JITted method -> dangling ptr.
 *
 * Fix: each JITted method's reference (and address-taken) vregs live in a real stack frame on the
 * emscripten C stack (__stack_pointer), which sgen already scans CONSERVATIVELY for every thread —
 * the exact mechanism AOT'd LLVM code relies on for refs in C locals. This replaced a custom
 * per-thread "ref shadow stack" arena (mono_gc_register_root + enter/leave + zero-on-pop + balance
 * guards): with real frames, a popped/unwound frame falls below the SP and is simply no longer
 * scanned, C++/wasm-EH landing pads restore the SP like every LLVM-compiled catch does, and
 * JSPI-suspended computations keep their frames inside the scanned [SP, stack-top] region — whatever
 * guarantee AOT frames have, JIT frames inherit by construction.
 *
 * Frame layout (stack grows DOWN; entry_sp is the SP at method entry):
 *   entry_sp                                  <- restored at every exit (global.set of s.p)
 *     ref slots   [refbase + slot*4)          <- refbase = frame base; zeroed in the prologue
 *     addr slots  [addrbase + offset)         <- addrbase = refbase + align8(nrefslots*4)
 *   frame = align16(entry_sp - framebytes)    <- the new __stack_pointer after the prologue
 * SP access from JITted code uses the imported __stack_pointer wasm global (s.p, global 0):
 * global.get/set 0 for the entry-SP capture and frame save/restore. The main module exports
 * __stack_pointer (-Wl,--export=__stack_pointer) and the instantiation passes it as s.p.
 *
 * PIN-PRESSURE / PERF LEVERS on top of the base model (each env-gated, see the flags below; all
 * three mirror or extend what LLVM AOT does with its gc_pin alloca in mini-llvm.c emit_gc_pin):
 *   - MONO_WASM_JIT_REF_WT (write-through): the wasm LOCAL is the ref vreg's value home and the
 *     frame slot is only a def-mirrored pin copy — reads become local.get (AOT parity; AOT keeps
 *     refs in SSA registers and volatile-stores each def into the scanned alloca).
 *   - MONO_WASM_JIT_SLOTLIVE (slot elision): only refs a GC can actually OBSERVE (live across a
 *     GC-capable instruction, or spanning bbs) get a slot at all; the rest stay in plain locals.
 *   - MONO_WASM_JIT_SLOTZERO (dead-slot zeroing): a single-bb slotted ref's slot is zeroed after
 *     its last use, so dead objects stop pinning inside long-lived (JSPI-suspended) frames.
 * The whole scheme rests on two invariants:
 *   (I1) a pinned object never MOVES — so a cached wasm-local copy (REF_WT) can't go stale, and a
 *        JSPI-suspended computation's frozen locals stay valid across a GC. The same invariant AOT
 *        code depends on for refs in registers.
 *   (I2) every slot is CURRENT at every GC point — the slot store is emitted adjacent to each def
 *        with no GC point in between, so a multithreaded STW at an OP_GC_SAFE_POINT poll always
 *        sees live values.
 * Rejected alternatives, for the record: precise/moving roots (incompatible with I1-dependent
 * local caching, JSPI-frozen locals, and wasm operand-stack transients across GC points) and a
 * registered arena root (the pre-frame design this section replaced — loses the by-construction
 * EH/JSPI guarantees of real C-stack frames).
 *
 * QUALIFY THE ARENA REJECTION (R191). It is sound for an arena whose bump pointer is a GLOBAL, which
 * is what the pre-frame design had (enter/leave + zero-on-pop + balance guards). It does NOT follow for
 * an arena whose pointer is THREADED as a call parameter: a parameter is per-frame and per-stack by
 * construction, so unwinding to a caller restores that caller's value with no explicit pop, and JSPI
 * saves it with the frame. CoreCLR's wasm backend threads exactly such a pointer
 * (WellKnownArg::WasmShadowStackPointer, added to every managed call at morph.cpp:1803-1809) and
 * NativeAOT-wasm scans a separate TLS-bounded region conservatively on top of it
 * (nativeaot/Runtime/thread.cpp:419). Threading is not an alternative to a private region — it is what
 * makes one safe. Their ABI also MANDATES our (I1): clr-abi.md "GC References at Call Sites" requires
 * on-frame GC refs be reported pinned so wasm locals need not be updated after a call, and PR #131374
 * fixed an intermittent NullReferenceException from not doing it.
 *
 * What does NOT transfer is their publish: CoreCLR's __stack_pointer accesses sit in main-module C
 * helpers where it is a MODULE-DEFINED global (one hoisted load + constant-offset store) and R2R code
 * never touches it. Ours is an IMPORTED MUTABLE global — 4 loads + 3 decompressions, non-hoistable,
 * measured at 5.25% of the in-game window (R190). Publishing before every GC point would therefore be
 * ~4x MORE traffic than today's 3 ops per framed method.
 */
#ifdef HOST_BROWSER
#include <emscripten/stack.h>
#endif

/*
 * Per-thread "addressable locals" frame stack (linear memory) for OP_LDADDR.
 *
 * A JITted method that takes the address of a SCALAR local — e.g. the `bool& lock_taken` out-arg of a
 * synchronized wrapper's Monitor.Enter, or any `ref local` / `out local` passed to a callee — can't keep
 * that local in a wasm local (wasm locals have no address). Instead the emitter backs each address-taken
 * scalar local with an 8-byte slot in this per-thread frame: the method does base = addr_enter(framebytes)
 * at entry, reads/writes the local at base+offset, passes base+offset as the &local, and addr_leave(base)
 * at every exit (folded into EMIT_REF_LEAVE). The same value funnels through the one memory slot whether it
 * is touched via OP_MOVE (ldloc/stloc) or written through the escaped pointer by a callee, so they stay
 * consistent.
 *
 * The addr slots live in the same C-stack frame as the ref slots (addrbase = refbase + align8(refbytes)).
 * Being on the C stack they ARE now conservatively scanned — same as any C local in AOT'd code; that is
 * harmless for scalars (a value that happens to look like a heap pointer just over-pins) and it is what
 * allows address-taken REF locals to use frame slots too. The frame is zeroed in the prologue (.NET
 * locals are zero-init). */

/* MONO_WASM_JIT_STOREGUARD: 1 = emit a bounds-check call_indirect before every ref-slot / addr-slot
 * store. DEBUG ONLY (a C call per such store); used to catch the wild store that scribbles random in-bounds
 * C-heap (the arenas are g_malloc'd, so an overrun via a drifted base hits neighbours like the marshal cache
 * / jiterp tlqueue). Default off. */
int mono_wasm_jit_storeguard = 0;
/* MONO_WASM_JIT_OBJGUARD: 1 = before every reference-field store, validate the OBJECT BASE is a live heap
 * object (mono_wasm_jit_check_store kind=2), trapping at the culprit JITted method if it's stale/garbage —
 * the missed-ref / dangling-base detector (the heap-store analog of STOREGUARD, which only guards the
 * shadow-stack/addr-frame stores). DEBUG ONLY (a C call per ref store). Default off. */
int mono_wasm_jit_objguard = 0;

/*
 * THE PERMANENTLY-UN-JITTABLE SET -- answering a predicate about a MonoMethod* WITHOUT DEREFERENCING IT.
 *
 * mono_wasm_jit_callee_perm_unjittable is four lines and needs exactly ONE BIT ("has this callee been
 * marked slot == -1"), yet reaching that bit cost four dereferences of a pointer the emitter does not own:
 *
 *     InterpMethod *im = mono_interp_peek_imethod (method);     // -> jit_mm_for_method (method)
 *     return (im && im->wasm_jit_slot == -1) ? 1 : 0;           //    -> m_method_get_mem_manager (m)
 *
 * The emitter consults it about ARBITRARY callees, on a worker, while IKVM generates and frees dynamic
 * types continuously -- so `method` is not always dereferenceable. It has crashed twice at this one site,
 * from two DIFFERENT hazards:
 *
 *   2026-09-10  mono-internal-hash.c:47 `table->table != NULL'   -- an uninitialised interp_code_hash,
 *               fixed by switching get_imethod -> peek_imethod.
 *   2026-09-19  a MISALIGNED a_cas in mono_mem_manager_lock, reached through
 *               peek_imethod -> jit_mm_for_method -> m_method_get_mem_manager on a freed method.
 *               (In wasm, alignment is a hint for ordinary loads but ENFORCED for atomics, so a
 *               misaligned CAS means the mutex address itself was garbage.)
 *
 * PEEK DOES NOT PROTECT AGAINST THE SECOND, and two rounds leaned on "use peek here" as though it settled
 * the site. Peek avoids CREATING an InterpMethod; it still has to find the memory manager first, which is
 * four dereferences of the method and its class before any guard can run. Same four lines, two hazards,
 * one fix applied.
 *
 * THE FIX IS NOT A VALIDITY TEST. CLAUDE.md is right that a range check cannot fix this -- a freed pointer
 * is still in range -- and mono_wasm_jit_method_usable below only converts the DETECTABLE subset into a
 * counted refusal. So do not reach the bit that way at all: maintain the bit in a set keyed by the pointer
 * VALUE, populated where the fact becomes true (the five sites that write wasm_jit_slot = -1), and answer
 * by lookup. HASHING A POINTER NEVER DEREFERENCES IT -- which is the property no validity test can have.
 *
 * Failure modes, stated before building it rather than discovered afterwards:
 *
 *   ADDRESS REUSE (ABA). A freed method whose address is recycled reads as a false POSITIVE. Consequence:
 *   one call edge routes through the interp residual when it need not -- slower by one JIT->interp
 *   transition, still correct. The behaviour being replaced, for the same input, is a process kill.
 *
 *   STALENESS. The answer only ever goes 0 -> 1 in practice: WJC_RELINK_BAIL_CLEARED measures 0 over a
 *   full run (R252), i.e. nothing ever clears a permanent bail. So append-only with no removal is sound
 *   and the read path needs no lock -- and must not have one, because this runs per call site per emit
 *   across compile workers, where a global lock would contend.
 *
 *   FULL. Refuse by answering 0 ("not permanently un-JITtable"), which is the CONSERVATIVE direction: the
 *   island simply tries to pull the callee in, exactly as it does for any un-prepared callee. Counted, so
 *   a table that silently stopped working is visible rather than presenting as a codegen change.
 *
 * Open-addressed, power-of-two, linear probe, never resized. 64 Ki slots = 256 KiB on wasm32 against a
 * measured ~10.3k emit bails per run, so the load factor stays far below the point where probing degrades.
 */
#define WJ_PERM_SET_BITS  16
#define WJ_PERM_SET_SIZE  (1 << WJ_PERM_SET_BITS)
#define WJ_PERM_SET_MASK  (WJ_PERM_SET_SIZE - 1)
static gpointer wj_perm_set [WJ_PERM_SET_SIZE];
static volatile gint32 wj_perm_set_n;

/* Pointers are 4-byte aligned at minimum, so the low bits carry no entropy; mix the high ones down. */
static guint32
wj_perm_hash (gpointer p)
{
	guint32 h = (guint32) (guintptr) p;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	return h & WJ_PERM_SET_MASK;
}

/*
 * Record that `method` is permanently un-JITtable. Called from the sites that write wasm_jit_slot = -1,
 * i.e. where the fact becomes true rather than where it is later consulted -- "resolve and consume in the
 * same breath", applied to a fact instead of a pointer.
 *
 * Lock-free and idempotent. A racing insert of the SAME pointer can write the same slot twice, which is
 * harmless; a racing insert of a DIFFERENT pointer can lose a probe and land one slot later, also
 * harmless. Only a lost insert would matter, and a lost insert degrades to today's behaviour (the callee
 * reads as not-perm), never to a wrong dereference.
 */
void mono_wasm_jit_note_perm_unjittable (MonoMethod *method);
void
mono_wasm_jit_note_perm_unjittable (MonoMethod *method)
{
	guint32 i, h;
	if (!method)
		return;
	if (mono_atomic_load_i32 (&wj_perm_set_n) >= (WJ_PERM_SET_SIZE / 2)) {
		mono_wasm_jit_counters [WJC_PERM_SET_FULL]++;
		return;
	}
	h = wj_perm_hash (method);
	for (i = 0; i < 64; ++i) {
		guint32 k = (h + i) & WJ_PERM_SET_MASK;
		gpointer cur = wj_perm_set [k];
		if (cur == method)
			return;                       /* already recorded */
		if (!cur) {
			if (mono_atomic_cas_ptr (&wj_perm_set [k], method, NULL) == NULL) {
				mono_atomic_inc_i32 (&wj_perm_set_n);
				mono_wasm_jit_counters [WJC_PERM_SET_ADDS]++;
				return;
			}
			--i;                          /* lost the slot to a racer; re-examine it */
		}
	}
	/* 64 probes without a free slot: the table is pathologically clustered rather than full. Same
	 * conservative answer as full, and counted separately so the two are distinguishable. */
	mono_wasm_jit_counters [WJC_PERM_SET_FULL]++;
}

/*
 * Is `method` in the set? POINTER COMPARISON ONLY -- `method` is never dereferenced, so a freed or
 * recycled pointer is answered safely rather than detected.
 */
int mono_wasm_jit_perm_unjittable_known (MonoMethod *method);
int
mono_wasm_jit_perm_unjittable_known (MonoMethod *method)
{
	guint32 i, h;
	if (!method)
		return 0;
	h = wj_perm_hash (method);
	for (i = 0; i < 64; ++i) {
		guint32 k = (h + i) & WJ_PERM_SET_MASK;
		gpointer cur = wj_perm_set [k];
		if (cur == method) {
			mono_wasm_jit_counters [WJC_PERM_SET_HITS]++;
			return 1;
		}
		if (!cur)
			return 0;                     /* a run of occupied slots ended: it was never inserted */
	}
	return 0;
}

/*
 * Did this method's permanence just become STALE? The set is append-only, and the only thing that can
 * falsify an entry is the relink clearing wasm_jit_slot from -1 back to 0 (tiering.c, the
 * WJC_RELINK_BAIL_CLEARED arm). R252 measured that arm at ZERO over a full run -- IKVM's relink hook is
 * the method's first execution, where a method is untried or already live, so -1 is never reached there.
 *
 * Rather than rest the design on someone else's measurement, COUNT IT. Non-zero means the set is handing
 * out stale positives and the cost is real (an edge routed through the interp residual that need not be);
 * zero means the staleness window this design accepts has never once opened. Either way it is a reading,
 * and the alternative -- a removable entry -- would need tombstones and would cost the lock-free read.
 */
/* R315: mini.c's downgrade retry (mono_wasm_force_compile) cannot see the WJC_* enum; count through here. */
void mono_wasm_jit_note_inline_downgrade (gboolean ok);
void
mono_wasm_jit_note_inline_downgrade (gboolean ok)
{
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		mono_wasm_jit_count (WJC_INLINE_DOWNGRADE);
		if (ok)
			mono_wasm_jit_count (WJC_INLINE_DOWNGRADE_OK);
	}
}

void mono_wasm_jit_note_perm_cleared (MonoMethod *method);
void
mono_wasm_jit_note_perm_cleared (MonoMethod *method)
{
	if (method && mono_wasm_jit_perm_unjittable_known (method)) {
		mono_wasm_jit_counters [WJC_PERM_SET_STALE]++;
		/* Undo the HITS bump this probe just caused: it is bookkeeping, not a predicate answer. */
		mono_wasm_jit_counters [WJC_PERM_SET_HITS]--;
	}
}

/*
 * THE FREED-METHOD SET (MONO_WASM_JIT_DEADSET, R290). The backend retains raw MonoMethod * for the process
 * lifetime -- the call profile's id_targets[], wj_sync_inner_canon, the island blocker lists built from
 * them -- while IKVM frees dynamic methods continuously. Two stacks caught on 2026-09-22, both from
 * wasm_jit_force_island -> mono_interp_get_imethod on such a pointer:
 *     mono-internal-hash.c:47  `table->table != NULL'    (garbage jit-mm reached through the method)
 *     loader.c:1826            `mono_metadata_token_table (m->token) == MONO_TABLE_METHOD'  (garbage token)
 * A validity test cannot fix this (a freed pointer is still in range), so -- exactly as the perm set above
 * -- the fact is RECORDED where it becomes true (interp_free_method, before the memory is released) and
 * ANSWERED by pointer value, never by dereference.
 *
 * Unlike the perm set this one must forget: a freed address is soon recycled for a NEW method, and a
 * stale mark would silently exclude it from island forcing and devirt prediction forever. Creating an
 * InterpMethod for a pointer is proof that it names a live method, so mono_interp_get_imethod's creation
 * path clears the mark (a tombstone, so probe chains stay intact and the read path stays lock-free).
 *
 * What it does not close: a method freed BETWEEN a consumer's check and its dereference. That window is
 * the few instructions of one compile section, against the minutes-to-hours a retained pointer lives --
 * and a caught free in that window still reads as today's crash, so the per-site hit counters are the
 * measure of how much of the class this retires. FULL degrades to today's behaviour, counted.
 */
#define WJ_DEAD_SET_BITS  17
#define WJ_DEAD_SET_SIZE  (1 << WJ_DEAD_SET_BITS)
#define WJ_DEAD_SET_MASK  (WJ_DEAD_SET_SIZE - 1)
#define WJ_DEAD_TOMB      ((gpointer) (gsize) 1)
static gpointer wj_dead_set [WJ_DEAD_SET_SIZE];
static volatile gint32 wj_dead_set_n;   /* occupied slots, tombstones included: tombstones still cost probes */

static guint32
wj_dead_hash (gpointer p)
{
	guint32 h = (guint32) (guintptr) p;
	h ^= h >> 16;
	h *= 0x45d9f3bu;
	h ^= h >> 16;
	return h & WJ_DEAD_SET_MASK;
}

void mono_wasm_jit_note_method_freed (MonoMethod *method);
void
mono_wasm_jit_note_method_freed (MonoMethod *method)
{
	guint32 i, h;
	if (!method || !mono_wasm_jit_deadset)
		return;
	if (mono_atomic_load_i32 (&wj_dead_set_n) >= (WJ_DEAD_SET_SIZE / 2)) {
		mono_wasm_jit_counters [WJC_DEADSET_FULL]++;
		return;
	}
	h = wj_dead_hash (method);
	for (i = 0; i < 64; ++i) {
		guint32 k = (h + i) & WJ_DEAD_SET_MASK;
		gpointer cur = wj_dead_set [k];
		if (cur == method)
			return;
		if (!cur || cur == WJ_DEAD_TOMB) {
			if (mono_atomic_cas_ptr (&wj_dead_set [k], method, cur) == cur) {
				if (!cur)
					mono_atomic_inc_i32 (&wj_dead_set_n);
				mono_wasm_jit_counters [WJC_DEADSET_ADDS]++;
				return;
			}
			--i;
		}
	}
	mono_wasm_jit_counters [WJC_DEADSET_FULL]++;
}

/* Called where an InterpMethod is CREATED for `method`: the pointer names a live method again. */
void mono_wasm_jit_note_method_live (MonoMethod *method);
void
mono_wasm_jit_note_method_live (MonoMethod *method)
{
	guint32 i, h;
	if (!method || !mono_atomic_load_i32 (&wj_dead_set_n))
		return;
	h = wj_dead_hash (method);
	for (i = 0; i < 64; ++i) {
		guint32 k = (h + i) & WJ_DEAD_SET_MASK;
		gpointer cur = wj_dead_set [k];
		if (!cur)
			return;
		if (cur == method) {
			if (mono_atomic_cas_ptr (&wj_dead_set [k], WJ_DEAD_TOMB, method) == method)
				mono_wasm_jit_counters [WJC_DEADSET_REVIVED]++;
			return;
		}
	}
}

/* Pointer comparison only: `method` is never dereferenced. `site` is the WJC_DEAD_HIT_* counter to bump. */
/* Exported: the jiterpreter's per-worker infoTable (jiterpreter-interp-entry.ts) asks it before building
 * a trampoline from an entry the freeing worker has already dropped -- that table is per worker, and
 * mono_jiterp_free_method_data_interp_entry runs on the freeing thread only. */
WJ_KEEPALIVE int mono_wasm_jit_method_known_dead (MonoMethod *method, int site);
WJ_KEEPALIVE int
mono_wasm_jit_method_known_dead (MonoMethod *method, int site)
{
	guint32 i, h;
	if (!method || !mono_wasm_jit_deadset || !mono_atomic_load_i32 (&wj_dead_set_n))
		return 0;
	h = wj_dead_hash (method);
	for (i = 0; i < 64; ++i) {
		guint32 k = (h + i) & WJ_DEAD_SET_MASK;
		gpointer cur = wj_dead_set [k];
		if (!cur)
			return 0;
		if (cur == method) {
			if (site >= 0)
				mono_wasm_jit_counters [site]++;
			return 1;
		}
	}
	return 0;
}

/*
 * STACK HEADROOM PROBE (MONO_WASM_JIT_STACKPROBE). Diagnostic; default off.
 *
 * Two rounds have now been spent shaving C-stack arrays on the theory that
 *   maybe_compile -> force_compile -> ... -> compile_publish -> colocate_deps_now -> rebatch -> wj_assemble
 * overflows: R194 removed a 2 KB staging array after 8x `memory access out of bounds`, and R197 moved
 * `descs` to the heap (2048 -> 64 bytes) and took the same fault 12 -> 4. Neither confirmed anything.
 * The theory is not obviously right either -- the wasm C stack is 5 MB on the main thread and 1 MB on
 * workers ((SIZEOF_VOID_P / 4) * 1024 * 1024, mono-threads-wasm.c), which a few KB of locals should not
 * exhaust.
 *
 * So MEASURE the depth instead of inferring it from a fault count. Records the WORST (smallest)
 * headroom seen at each probe point, per thread bounds, so one run answers "is this chain near its
 * limit" or "this is not a stack problem at all". Racy min-tracking is deliberate: a diagnostic that
 * needs a lock changes the thing it measures.
 *
 * `emscripten_stack_get_end()` is the LOW address (the limit) and `_get_base()` the high one -- the
 * stack grows down -- so headroom is current - end and total is base - end.
 */
static inline void
wj_stack_probe (void)
{
	extern int mono_wasm_jit_stackprobe;
	if (G_LIKELY (!mono_wasm_jit_stackprobe))
		return;
	{
		gsize cur = (gsize) emscripten_stack_get_current ();
		gsize end = (gsize) emscripten_stack_get_end ();
		gsize base = (gsize) emscripten_stack_get_base ();
		gint32 head = (cur > end) ? (gint32) (cur - end) : 0;
		wj_stack_probe_hits++;
		if (head < wj_stack_min_headroom) {
			wj_stack_min_headroom = head;
			wj_stack_total_seen = (base > end) ? (gint32) (base - end) : 0;
		}
	}
}

/* Current linear-memory size in bytes, clamped so it is usable in 32-bit arithmetic: at 65536 pages
 * (a fully-grown 4GB memory) `pages << 16` overflows 32-bit gsize to 0. */
static inline gsize
wj_memsz (void)
{
	gsize s = (gsize) __builtin_wasm_memory_size (0) << 16;
	return s ? s : (gsize) -1;
}

/* Overflow-safe "is `a` a plausible aligned pointer we may speculatively READ 8 bytes at?" for the
 * OBJGUARD/MISSEDREF diagnostic probes. The naive form `a + 8 > memsz` WRAPS for a near 2^32 —
 * e.g. probing a heap word that holds a small negative int like -8 (0xFFFFFFF8): a+8 == 0 passes,
 * and the diagnostic's own deref becomes the OOB trap that silently kills a JSPI-suspended thread
 * and stalls the GC (seen live: MISSEDREF ICONST probe trapping inside mono_wasm_emit_method). */
static inline gboolean
wj_probe_ok (gsize a, gsize memsz)
{
	return !(a & 3) && a >= 1024 && a <= memsz - 8;
}

/* Is this pointer PLAUSIBLY a live runtime object? Alignment + heap range, nothing more.
 *
 * R151 wrote exactly this guard (`wj_method_ptr_ok`) for the "one boot in four dies in
 * mono_interp_get_imethod on a worker" fault, never wired it into a call path, and it was later deleted
 * with the reasoning: *"a range check cannot see a freed pointer that still lands in range and the fault
 * has not recurred"*. The first half is still true and is stated again at every call site. **The second
 * half expired on 2026-09-19**, when the fault recurred as a misaligned `a_cas` inside
 * `mono_mem_manager_lock`, reached from `mono_interp_peek_imethod` <- `callee_perm_unjittable` <-
 * `mono_wasm_emit_method`. So the guard is back, wired up this time, and honest about its reach: it
 * converts the DETECTABLE subset of a dangling dereference into a counted refusal, and does nothing
 * whatever about a freed pointer that still looks plausible. The real fix is to stop consulting retained
 * raw MonoMethod* at emit time; see scratchpad/wj/p0/DANGLING-MONOMETHOD.md. */
int mono_wasm_jit_ptr_plausible (gpointer p);
int
mono_wasm_jit_ptr_plausible (gpointer p)
{
#ifdef HOST_BROWSER
	return wj_probe_ok ((gsize) (intptr_t) p, wj_memsz ()) ? 1 : 0;
#else
	return p != NULL;
#endif
}

/*
 * Is this RETAINED MonoMethod* still usable, and if not, WHICH retainer produced it?
 *
 * The fault this catches is `loader.c:1826` -- `mono_metadata_token_table (m->token) == MONO_TABLE_METHOD'
 * -- and the `mono_signature_to_name` -> `g_string_append` -> `dlrealloc` OOB three seconds behind it
 * (R273). Both are a dead method reaching code that formats or resolves it, and by then the stack names
 * mono internals rather than whoever handed the pointer over. `site` is the whole point: the registry,
 * the call profile and the synchronized-wrapper canon table all retain for the process lifetime, so only
 * a per-site count says which one to fix.
 *
 * WHY THIS IS SAFE TO CALL ON A DEAD POINTER, which is the only interesting question here:
 *   - the range probe runs FIRST and short-circuits, so a wild pointer is never dereferenced at all;
 *   - a FREED-but-in-range pointer IS dereferenced, and that is survivable -- the allocation is still
 *     mapped, so `m->token` reads garbage rather than trapping. Reading one word of garbage is exactly
 *     what lets us reject it before mono walks it into a metadata table with that garbage as an index.
 * That asymmetry is why a range check alone was never enough (CLAUDE.md: "a freed pointer still lands in
 * range") and why a range check plus ONE cheap structural test is.
 *
 * It does NOT make a retained pointer safe. Address reuse defeats it: a freed method whose storage now
 * holds a live method passes both tests. This converts the DETECTABLE majority into a counted refusal and
 * localises the retainer; the fix is still to stop retaining (DANGLING-MONOMETHOD.md, option 2).
 */
int mono_wasm_jit_method_usable (MonoMethod *m, int site);
int
mono_wasm_jit_method_usable (MonoMethod *m, int site)
{
	if (!mono_wasm_jit_badmeth)
		return 1;

	mono_wasm_jit_count (WJC_BADMETH_SEEN);

	/* The AUTHORITATIVE answer first, and before anything below dereferences `m`: a method the free hook
	 * recorded is dead however plausible its pointer still looks (R290). */
	if (G_UNLIKELY (mono_wasm_jit_method_known_dead (m, site == WJ_BADMETH_SITE_PROFILE ? WJC_DEAD_HIT_PROF :
	                                                     site == WJ_BADMETH_SITE_CANON ? WJC_DEAD_HIT_CANON : -1)))
		goto dead;
	if (G_UNLIKELY (!mono_wasm_jit_ptr_plausible (m)))
		goto dead;

	/* MIRROR loader.c's OWN PRECONDITIONS, or this rejects live methods.
	 *
	 * `mono_method_signature_checked_slow` reaches the token assert only AFTER returning early for a
	 * cached `signature` and for `is_inflated` (loader.c:1800-1811). A wrapper, a dynamic method or an
	 * inflated generic legitimately carries a token that is not a METHOD token, and testing them all
	 * unconditionally is wrong: it made `peek_imethod` answer NULL for every such method, and IKVM emits
	 * them constantly. Measured: boot stalled 3/3 at ~6 s, and it survived an `IKVM_LAZY_RECOMPILE=0`
	 * arm, which is what proved the fault was here and not in the IKVM rewrite deployed alongside it.
	 *
	 * So the token test applies to exactly the population the assert applies to, and nothing else. */
	if (m->signature || m->is_inflated || m->wrapper_type != MONO_WRAPPER_NONE)
		return 1;
	if (G_UNLIKELY (mono_metadata_token_table (m->token) != MONO_TABLE_METHOD))
		goto dead;
	return 1;

dead:
	switch (site) {
	case WJ_BADMETH_SITE_PEEK:     mono_wasm_jit_count (WJC_BADMETH_PEEK); break;
	case WJ_BADMETH_SITE_REGISTRY: mono_wasm_jit_count (WJC_BADMETH_REGISTRY); break;
	case WJ_BADMETH_SITE_PROFILE:  mono_wasm_jit_count (WJC_BADMETH_PROFILE); break;
	case WJ_BADMETH_SITE_CANON:    mono_wasm_jit_count (WJC_BADMETH_CANON); break;
	default: break;
	}
	if (mono_wasm_jit_verbose)
		printf ("[wasm-jit badmeth] site=%d m=%p token=%x -- refused a dead retained MonoMethod\n",
			site, (void *) m, mono_wasm_jit_ptr_plausible (m) ? m->token : 0);
	return 0;
}

/* Called (when storeguard/objguard is on) right before a ref-shadow-stack (kind 0), addr-frame (kind 1),
 * object/base store (kind 2/3), generic membase access (kind 4), or vcall receiver deref (kind 5), with the computed target address. If the
 * address is outside the expected region — the signature of an enter/leave imbalance on an EH unwind drifting
 * the base past the region, or a corrupted base — trap HERE so the (MONO_WASM_JIT_NAMES-symbolicated) wasm
 * stack trace names the JITted method doing the bad access, instead of the random downstream OOB. */
void
mono_wasm_jit_check_store (guint8 *addr, int kind)
{
	guint8 *lo, *hi;
	if (kind == 4) {
		/* OBJGUARD generic membase load/store address: catch OOB loads and scalar-classified wild store bases
		 * that the ref/byref-specific kind 2/3 checks do not see. */
		if (G_UNLIKELY (addr != NULL)) {
			gsize a = (gsize) addr;
			gsize memsz = wj_memsz ();
			if (G_UNLIKELY (a < 1024 || a >= memsz)) {
				printf ("WASM_JIT_BAD_MEMADDR addr=%p — garbage JIT membase access address; method in the trap below:\n",
					(void *) addr);
				fflush (stdout);
				__builtin_trap ();
			}
		}
		return;
	}
	if (kind == 3) {
		/* OBJGUARD byref base: addr is the base of a store THROUGH a ref/byref (e.g. `*outparam = scalar`).
		 * A byref is not an object header (no vtable to validate), and a byref to a byte/short field is legally
		 * unaligned — so DON'T alignment-check (that would false-trap). Just a loose in-memory range check to
		 * catch a wildly stale byref. (The control-var check above already caught the specific in-range case
		 * where the byref points at the C-stack pointer — that is the real garbage-SP catch.) NULL is a NRE elsewhere. */
		if (G_UNLIKELY (addr != NULL)) {
			gsize a = (gsize) addr;
			gsize memsz = wj_memsz ();
			if (G_UNLIKELY (a < 1024 || a >= memsz)) {
				printf ("WASM_JIT_BAD_BYREF base=%p — garbage byref store base (stale out-param / drift?); storing method in the trap below:\n",
					(void *) addr);
				fflush (stdout);
				__builtin_trap ();
			}
		}
		return;
	}
	if (kind == 2 || kind == 5) {
		/* OBJGUARD: addr is the OBJECT BASE of a reference-field store. A missed reference (a managed pointer
		 * the isref pass left in a GC-invisible wasm local) goes stale/garbage when the GC moves or frees the
		 * object; using it as the store base (or its write-barrier card mark, (base>>9)+cardtable) scribbles
		 * random memory (the tlqueue/marshal-cache OOB). Validate it looks like a live heap object via raw word
		 * reads (no struct layout): obj->vtable and vtable->klass must be plausible in-memory pointers. NULL is
		 * legitimate (null store / NRE handled elsewhere). Trap HERE so the symbolicated wasm trace names the
		 * JITted method with the bad base. */
		if (G_UNLIKELY (addr != NULL)) {
			gsize a = (gsize) addr;
			gsize memsz = wj_memsz ();
			/* The base ADDRESS must itself be a sane aligned in-memory pointer; if not, it is a wild base. */
			if (G_UNLIKELY (!wj_probe_ok (a, memsz))) {
				if (kind == 5)
					printf ("WASM_JIT_BAD_VCALL_THIS obj=%p — receiver out of range / misaligned before vtable load; method in the trap below:\n", (void *) addr);
				else
					printf ("WASM_JIT_BAD_OBJBASE obj=%p — store base out of range / misaligned (wild base); method in the trap below:\n", (void *) addr);
				fflush (stdout);
				__builtin_trap ();
			}
			gsize vt = *(gsize *) addr;   /* obj->vtable */
			/* vtable == 0 is AMBIGUOUS, NOT a confirmed corruption — DON'T trap. A reference value is legitimately
			 * stored THROUGH a byref / interior pointer into a slot that is currently null (an `out` param or a ref
			 * field not yet assigned), so *base reads as 0. kind=2 cannot distinguish that benign shape from a
			 * collected/zeroed object, and hard-trapping on it was the consistent MemoryMappingTree$Entry:.ctor
			 * false positive that masked everything downstream. Rate-limited log only, so a genuinely suspicious
			 * zeroed-object pattern stays visible without killing the thread (a wasm trap on a JSPI-suspended
			 * stack silently kills it and stalls the GC). */
			if (vt == 0) {
				if (kind == 5) {
					printf ("WASM_JIT_BAD_VCALL_THIS obj=%p — receiver has null vtable before virtual dispatch; method in the trap below:\n", (void *) addr);
					fflush (stdout);
					__builtin_trap ();
				}
				static int z = 0;
				if (z++ < 20) { printf ("WASM_JIT_OBJBASE_NULLVT obj=%p — null first word (byref into a null slot, or zeroed object); NOT trapping\n", (void *) addr); fflush (stdout); }
				return;
			}
			/* Non-null vtable: it MUST look like a real vtable (aligned, in range) pointing at a real klass. A
			 * non-null-but-garbage vtable is the unambiguous stale/freed-object signature -> hard trap. */
			gboolean bad = !wj_probe_ok (vt, memsz);
			gsize klass = 0;
			if (!bad) { klass = *(gsize *) vt; bad = !klass || !wj_probe_ok (klass, memsz); } /* vtable->klass */
			if (G_UNLIKELY (bad)) {
				if (kind == 5)
					printf ("WASM_JIT_BAD_VCALL_THIS obj=%p vtable=0x%x klass=0x%x — stale/garbage receiver before virtual dispatch; method in the trap below:\n",
						(void *) addr, (unsigned) vt, (unsigned) klass);
				else
					printf ("WASM_JIT_BAD_OBJBASE obj=%p vtable=0x%x klass=0x%x — stale/garbage object base (non-null junk vtable); storing method in the trap below:\n",
						(void *) addr, (unsigned) vt, (unsigned) klass);
				fflush (stdout);
				__builtin_trap ();   /* deliberate: the symbolicated wasm trace names the culprit method */
			}
		}
		return;
	}
	/* kind 0/1: a JIT frame (ref or addr) slot store. The frame lives on the emscripten C stack, so a
	 * valid target must be within this thread's live stack region: at or above the deepest live SP
	 * (we are called FROM the JITted method, so our own C frame is below its frame) and below the
	 * stack base. Anything else is a wild/clobbered frame base. */
	lo = (guint8 *) emscripten_stack_get_current ();
	hi = (guint8 *) emscripten_stack_get_base ();
	if (G_UNLIKELY (!lo || addr < lo || addr >= hi)) {
		printf ("WASM_JIT_WILD_STORE kind=%d addr=%p stack=[%p,%p) — storing method in the trap below:\n",
			kind, (void *) addr, (void *) lo, (void *) hi);
		fflush (stdout);
		__builtin_trap ();   /* deliberate: the symbolicated wasm trace names the culprit method */
	}
}

/*
 * GC-tracked table for baked managed-object constants (a mini-GOT).
 *
 * ldstr / typeof / constant-folded Object.GetType() lower (non-AOT, in-line blocks) to
 * OP_PCONST(MonoObject*) — a raw, MOVABLE managed pointer the emitter would otherwise bake as a fixed
 * wasm i32.const immediate. The immediate is fixed but the object is not (SGen moves the nursery and
 * compacts the major heap), so after a GC the immediate dangles -> the "random" corruption (NPE, or a
 * null-function trap when the stale pointer reaches a vtable/dispatch). The AOT path already refuses
 * this exact thing (OP_AOTCONST bails: "a movable GC object can't be a wasm immediate"); this is the
 * non-AOT equivalent done correctly.
 *
 * Each distinct baked literal site gets a slot in this fixed, PRECISELY-scanned GC root. Because the
 * descriptor is precise (all-refs), the GC keeps the object alive AND updates the slot when the object
 * moves -> the objects stay movable (no pinning, no fragmentation). The emitter bakes the slot's
 * ADDRESS (native, in linear memory, never moves -> a legitimately constant immediate) and emits an
 * i32.load at the use, so every read yields the object's CURRENT address. The loaded value is routed
 * to the GC ref shadow stack (the isref pass marks the dreg), so it also survives the next GC point
 * while live in the frame.
 *
 * Process-wide (one table; the slot address is the same constant baked into every per-thread module
 * instance, and linear memory is shared). Appends are serialized under the loader lock; after the
 * append the only writer is the GC, and it writes only at a STW collection point (all mutators
 * suspended), so JITted reads are race-free. Slots are never freed/reused — bounded by the number of
 * distinct JITted literal sites, and these literals (interned strings / reflection types) are
 * effectively immortal anyway.
 */
#define WJ_LIT_SLOTS (64 * 1024)
static MonoObject **wj_lit_table = NULL;
static int wj_lit_n = 0;
/* CONTENT-keyed dedup for string literals: the same value collapses to ONE shared slot instead of one
 * per ldstr site per compiled method (which would exhaust the table on a big app). Keyed by string
 * content, which is move-stable — mono_ldstr already interns by content, and MonoGHashTable rebuckets
 * only on resize, NOT when the GC moves a key, so an address key would land in a stale bucket after a
 * move (and could even alias a reused address). The key is GC-tracked (kept alive + address-updated by
 * the GC); the value is the native slot address (MONO_HASH_KEY_GC => values are not GC-scanned).
 * Reflection-type literals (typeof/ldtoken) are rarer and left un-deduped. */
static MonoGHashTable *wj_lit_str_dedup = NULL;

/*
 * Intern a baked managed-object constant: store it in the precise-root table and return the stable
 * native address of its slot (stashed on the OP_PCONST as inst_p1; the emitter bakes it + i32.load).
 * Returns NULL if the table is full (caller bails the method to the interpreter).
 *
 * GC-safe by construction. Called from method-to-ir at the RESOLVE site — `obj` is the value
 * mono_ldstr_checked / mono_type_get_object_checked JUST returned, and there is NO managed allocation
 * between that resolve and this store (the one-time table g_malloc0 + root registration + descriptor
 * build are all native, and the loader lock does not GC) — so `obj` cannot have moved before it is
 * rooted. (Doing this at backend-emit instead would be unsafe: a literal freshly interned during this
 * very compile can be moved by a nursery GC before the backend runs, leaving the IR pointer stale;
 * rooting a stale pointer would make the next collection trace garbage — worse than the stock
 * immediate bake.) From the store onward the precise root tracks the object across every later move
 * (the rest of the compile AND at runtime), so the baked slot address always loads the current ptr.
 */
gpointer
mono_wasm_jit_intern_literal (MonoObject *obj)
{
	gpointer slot;
	gboolean is_str;
	mono_loader_lock ();
	if (G_UNLIKELY (!wj_lit_table)) {
		wj_lit_table = (MonoObject **) g_malloc0 (WJ_LIT_SLOTS * sizeof (MonoObject *));
		mono_gc_register_root ((char *) wj_lit_table, WJ_LIT_SLOTS * sizeof (MonoObject *),
			mono_gc_make_root_descr_all_refs (WJ_LIT_SLOTS), MONO_ROOT_SOURCE_JIT, NULL, "wasm-jit literal table");
		wj_lit_str_dedup = mono_g_hash_table_new_type_internal ((GHashFunc) mono_string_hash_internal,
			(GCompareFunc) mono_string_equal_internal, MONO_HASH_KEY_GC, MONO_ROOT_SOURCE_JIT, NULL, "wasm-jit literal string dedup");
	}
	/* Dedup string literals by content: same value -> the existing shared slot. (lookup/insert + any
	 * resize are all native — no managed allocation — so `obj` cannot move between here and the store
	 * below, keeping the resolve-time freshness guarantee intact.) */
	is_str = obj && mono_object_class (obj) == mono_defaults.string_class;
	if (is_str && (slot = mono_g_hash_table_lookup (wj_lit_str_dedup, obj))) {
		mono_loader_unlock ();
		return slot;
	}
	if (G_UNLIKELY (wj_lit_n >= WJ_LIT_SLOTS)) { mono_loader_unlock (); return NULL; }
	slot = &wj_lit_table [wj_lit_n];
	wj_lit_table [wj_lit_n] = obj;
	wj_lit_n++;
	if (is_str)
		mono_g_hash_table_insert_internal (wj_lit_str_dedup, obj, slot);
	mono_loader_unlock ();
	return slot;
}
#endif

 //FIXME figure out if we need to distingush between i,l,f,d types
typedef enum {
	ArgOnStack,
	ArgValuetypeAddrOnStack,
	ArgGsharedVTOnStack,
	ArgValuetypeAddrInIReg,
	ArgVtypeAsScalar,
	ArgInvalid,
} ArgStorage;

typedef struct {
	ArgStorage storage : 8;
	MonoType *type, *etype;
} ArgInfo;

struct CallInfo {
	int nargs;
	gboolean gsharedvt;

	ArgInfo ret;
	ArgInfo args [1];
};

// WASM ABI: https://github.com/WebAssembly/tool-conventions/blob/main/BasicCABI.md

static ArgStorage
get_storage (MonoType *type, MonoType **etype, gboolean is_return)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_OBJECT:
		return ArgOnStack;

	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		return ArgOnStack;

	case MONO_TYPE_R4:
		return ArgOnStack;

	case MONO_TYPE_R8:
		return ArgOnStack;

	case MONO_TYPE_GENERICINST: {
		if (!mono_type_generic_inst_is_valuetype (type))
			return ArgOnStack;

		if (mini_is_gsharedvt_variable_type (type))
			return ArgGsharedVTOnStack;

		if (mini_wasm_is_scalar_vtype (type, etype))
			return ArgVtypeAsScalar;

		return is_return ? ArgValuetypeAddrInIReg : ArgValuetypeAddrOnStack;
	}
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF: {
		if (mini_wasm_is_scalar_vtype (type, etype))
			return ArgVtypeAsScalar;

		return is_return ? ArgValuetypeAddrInIReg : ArgValuetypeAddrOnStack;
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (mini_is_gsharedvt_type (type));
		return ArgGsharedVTOnStack;
	case MONO_TYPE_VOID:
		g_assert (is_return);
		break;
	default:
		g_error ("Can't handle as return value 0x%x", type->type);
	}
	return ArgInvalid;
}

/*
 * DEAD CODE ON THIS TARGET, and worth saying so because its asserts look alarming.
 *
 * `get_call_info` is static and has ZERO callers: the wasm JIT never builds a mono CallInfo -- it has its
 * own WjCallInfo (mini-wasm-ir.inc) -- and mono_arch_allocate_vars / mono_local_regalloc / linear scan are
 * all excluded for COMPILE_WASM, so nothing reaches it. `get_storage` is likewise called only from here.
 *
 * So the `g_assert (mini_is_gsharedvt_type ...)`, `g_assert (is_return)`, the `g_error ("Can't handle as
 * return value ...")` and `g_assert (sig->call_convention != MONO_CALL_VARARG)` below are NOT reachable
 * aborts, and converting them would be churn presented as a stability fix. They are also not safely
 * convertible: the only failure value in scope, ArgInvalid, is returned in one place and handled by NO
 * consumer, so a bail would become silent bad codegen rather than a refusal.
 */
static CallInfo*
get_call_info (MonoMemPool *mp, MonoMethodSignature *sig)
{
	int n = sig->hasthis + sig->param_count;
	CallInfo *cinfo;

	if (mp)
		cinfo = (CallInfo *)mono_mempool_alloc0 (mp, sizeof (CallInfo) + (sizeof (ArgInfo) * n));
	else
		cinfo = (CallInfo *)g_malloc0 (sizeof (CallInfo) + (sizeof (ArgInfo) * n));

	cinfo->nargs = n;
	cinfo->gsharedvt = mini_is_gsharedvt_variable_signature (sig);

	/* return value */
	cinfo->ret.type = mini_get_underlying_type (sig->ret);
	cinfo->ret.storage = get_storage (cinfo->ret.type, &cinfo->ret.etype, TRUE);

	if (sig->hasthis)
		cinfo->args [0].storage = ArgOnStack;

	// not supported
	g_assert (sig->call_convention != MONO_CALL_VARARG);

	int i;
	for (i = 0; i < sig->param_count; ++i) {
		cinfo->args [i + sig->hasthis].type = mini_get_underlying_type (sig->params [i]);
		cinfo->args [i + sig->hasthis].storage = get_storage (cinfo->args [i + sig->hasthis].type, &cinfo->args [i + sig->hasthis].etype, FALSE);
	}

	return cinfo;
}

gboolean
mono_arch_have_fast_tls (void)
{
	return FALSE;
}

guint32
mono_arch_get_patch_offset (guint8 *code)
{
	g_error ("mono_arch_get_patch_offset");
	return 0;
}
gpointer
mono_arch_ip_from_context (void *sigctx)
{
	g_error ("mono_arch_ip_from_context");
}

gboolean
mono_arch_is_inst_imm (int opcode, int imm_opcode, gint64 imm)
{
	return TRUE;
}

void
mono_arch_lowering_pass (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

gboolean
mono_arch_opcode_supported (int opcode)
{
	switch (opcode) {
	case OP_ATOMIC_ADD_I4:
	case OP_ATOMIC_ADD_I8:
	case OP_ATOMIC_EXCHANGE_U1:
	case OP_ATOMIC_EXCHANGE_U2:
	case OP_ATOMIC_EXCHANGE_I4:
	case OP_ATOMIC_EXCHANGE_I8:
	case OP_ATOMIC_CAS_U1:
	case OP_ATOMIC_CAS_U2:
	case OP_ATOMIC_CAS_I4:
	case OP_ATOMIC_CAS_I8:
	case OP_ATOMIC_LOAD_I1:
	case OP_ATOMIC_LOAD_I2:
	case OP_ATOMIC_LOAD_I4:
	case OP_ATOMIC_LOAD_I8:
	case OP_ATOMIC_LOAD_U1:
	case OP_ATOMIC_LOAD_U2:
	case OP_ATOMIC_LOAD_U4:
	case OP_ATOMIC_LOAD_U8:
	case OP_ATOMIC_LOAD_R4:
	case OP_ATOMIC_LOAD_R8:
	case OP_ATOMIC_STORE_I1:
	case OP_ATOMIC_STORE_I2:
	case OP_ATOMIC_STORE_I4:
	case OP_ATOMIC_STORE_I8:
	case OP_ATOMIC_STORE_U1:
	case OP_ATOMIC_STORE_U2:
	case OP_ATOMIC_STORE_U4:
	case OP_ATOMIC_STORE_U8:
	case OP_ATOMIC_STORE_R4:
	case OP_ATOMIC_STORE_R8:
		return TRUE;
	default:
		return FALSE;
	}
	return FALSE;
}

void
mono_arch_output_basic_block (MonoCompile *cfg, MonoBasicBlock *bb)
{
	g_error ("mono_arch_output_basic_block");
}

#include "mini-wasm-ir.inc"
#include "mini-wasm-batching.inc"
#include "mini-wasm-emitter.inc"
#include "mini-wasm-lazy.inc"

#endif // DISABLE_JIT


const char*
mono_arch_fregname (int reg)
{
	return "freg0";
}

const char*
mono_arch_regname (int reg)
{
	return "r0";
}

int
mono_arch_get_argument_info (MonoMethodSignature *csig, int param_count, MonoJitArgumentInfo *arg_info)
{
	g_error ("mono_arch_get_argument_info");
}

GSList*
mono_arch_get_delegate_invoke_impls (void)
{
	g_error ("mono_arch_get_delegate_invoke_impls");
}

gpointer
mono_arch_get_gsharedvt_call_info (MonoMemoryManager *mem_manager, gpointer addr, MonoMethodSignature *normal_sig, MonoMethodSignature *gsharedvt_sig, gboolean gsharedvt_in, gint32 vcall_offset, gboolean calli)
{
	g_error ("mono_arch_get_gsharedvt_call_info");
	return NULL;
}

gpointer
mono_arch_get_delegate_invoke_impl (MonoMethodSignature *sig, gboolean has_target)
{
	g_error ("mono_arch_get_delegate_invoke_impl");
}

#ifdef HOST_BROWSER

#include <emscripten.h>

//functions exported to be used by JS
G_BEGIN_DECLS

//JS functions imported that we use
#ifdef DISABLE_THREADS
EMSCRIPTEN_KEEPALIVE void mono_wasm_execute_timer (void);
EMSCRIPTEN_KEEPALIVE void mono_background_exec (void);
EMSCRIPTEN_KEEPALIVE void mono_wasm_ds_exec (void);
extern void mono_wasm_schedule_timer (int shortestDueTimeMs);
#else
extern void mono_target_thread_schedule_synchronization_context(MonoNativeThreadId target_thread);
#endif // DISABLE_THREADS
G_END_DECLS

#endif // HOST_BROWSER

gpointer
mono_arch_get_this_arg_from_call (host_mgreg_t *regs, guint8 *code)
{
	g_error ("mono_arch_get_this_arg_from_call");
}

gpointer
mono_arch_get_delegate_virtual_invoke_impl (MonoMethodSignature *sig, MonoMethod *method, int offset, gboolean load_imt_reg)
{
	g_error ("mono_arch_get_delegate_virtual_invoke_impl");
}


void
mono_arch_cpu_init (void)
{
	// printf ("mono_arch_cpu_init\n");
}

void
mono_arch_finish_init (void)
{
	// printf ("mono_arch_finish_init\n");
}

void
mono_arch_init (void)
{
	// printf ("mono_arch_init\n");
}

void
mono_arch_cleanup (void)
{
}

void
mono_arch_register_lowlevel_calls (void)
{
}

void
mono_arch_flush_register_windows (void)
{
}

MonoMethod*
mono_arch_find_imt_method (host_mgreg_t *regs, guint8 *code)
{
	g_error ("mono_arch_find_static_call_vtable");
	return (MonoMethod*) regs [MONO_ARCH_IMT_REG];
}

MonoVTable*
mono_arch_find_static_call_vtable (host_mgreg_t *regs, guint8 *code)
{
	g_error ("mono_arch_find_static_call_vtable");
	return (MonoVTable*) regs [MONO_ARCH_RGCTX_REG];
}

GSList*
mono_arch_get_cie_program (void)
{
	GSList *l = NULL;

	return l;
}

gpointer
mono_arch_build_imt_trampoline (MonoVTable *vtable, MonoIMTCheckItem **imt_entries, int count, gpointer fail_tramp)
{
	g_error ("mono_arch_build_imt_trampoline");
}

guint32
mono_arch_cpu_optimizations (guint32 *exclude_mask)
{
	/* No arch specific passes yet */
	*exclude_mask = 0;
	return 0;
}

host_mgreg_t
mono_arch_context_get_int_reg (MonoContext *ctx, int reg)
{
	g_error ("mono_arch_context_get_int_reg");
	return 0;
}

host_mgreg_t*
mono_arch_context_get_int_reg_address (MonoContext *ctx, int reg)
{
	g_error ("mono_arch_context_get_int_reg_address");
	return 0;
}

#if defined(HOST_BROWSER) || defined(HOST_WASI)

void
mono_runtime_install_handlers (void)
{
}

void
mono_init_native_crash_info (void)
{
	return;
}

#endif

#ifdef HOST_BROWSER

void
mono_runtime_setup_stat_profiler (void)
{
}

gboolean
MONO_SIG_HANDLER_SIGNATURE (mono_chain_signal)
{
	g_error ("mono_chain_signal");

	return FALSE;
}

void
mono_chain_signal_to_default_sigsegv_handler (void)
{
	g_error ("mono_chain_signal_to_default_sigsegv_handler not supported on WASM");
}

gboolean
mono_thread_state_init_from_handle (MonoThreadUnwindState *tctx, MonoThreadInfo *info, void *sigctx)
{
	g_error ("WASM systems don't support mono_thread_state_init_from_handle");
	return FALSE;
}

#ifdef DISABLE_THREADS

// this points to System.Threading.TimerQueue.TimerHandler C# method

static void *timer_handler;

EMSCRIPTEN_KEEPALIVE void
mono_wasm_execute_timer (void)
{
	// callback could be null if timer was never used by the application, but only by prevent_timer_throttling_tick()
	if (timer_handler==NULL) {
		return;
	}

	background_job_cb cb = timer_handler;
	MONO_ENTER_GC_UNSAFE;
	cb ();
	MONO_EXIT_GC_UNSAFE;
}

void
mono_wasm_main_thread_schedule_timer (void *timerHandler, int shortestDueTimeMs)
{
	// NOTE: here the `timerHandler` callback is [UnmanagedCallersOnly] which wraps it with MONO_ENTER_GC_UNSAFE/MONO_EXIT_GC_UNSAFE

	g_assert (timerHandler);
	timer_handler = timerHandler;
    mono_wasm_schedule_timer (shortestDueTimeMs);
}
#endif
#endif

void
mono_arch_register_icall (void)
{
#ifdef HOST_BROWSER
#ifdef DISABLE_THREADS
	mono_add_internal_call_internal ("System.Threading.TimerQueue::MainThreadScheduleTimer", mono_wasm_main_thread_schedule_timer);
	mono_add_internal_call_internal ("System.Threading.ThreadPool::MainThreadScheduleBackgroundJob", mono_main_thread_schedule_background_job);
#else
	mono_add_internal_call_internal ("System.Runtime.InteropServices.JavaScript.JSSynchronizationContext::ScheduleSynchronizationContext", mono_target_thread_schedule_synchronization_context);
#endif /* DISABLE_THREADS */
#endif /* HOST_BROWSER */
}

void
mono_arch_patch_code_new (MonoCompile *cfg, guint8 *code, MonoJumpInfo *ji, gpointer target)
{
	g_error ("mono_arch_patch_code_new");
}

#ifdef HOST_BROWSER

G_BEGIN_DECLS

int inotify_init (void);
int inotify_rm_watch (int fd, int wd);
int inotify_add_watch (int fd, const char *pathname, uint32_t mask);
int sem_timedwait (sem_t *sem, const struct timespec *abs_timeout);

G_END_DECLS

G_BEGIN_DECLS

//llvm builtin's that we should not have used in the first place

#include <sys/types.h>
#include <pwd.h>
#include <uuid/uuid.h>

#ifndef __EMSCRIPTEN_PTHREADS__
int pthread_getschedparam (pthread_t thread, int *policy, struct sched_param *param)
{
	g_error ("pthread_getschedparam");
	return 0;
}
#endif

int
pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
	return 0;
}

int
sigsuspend(const sigset_t *sigmask)
{
	g_error ("sigsuspend");
	return 0;
}

int
inotify_init (void)
{
	g_error ("inotify_init");
}

int
inotify_rm_watch (int fd, int wd)
{
	g_error ("inotify_rm_watch");
	return 0;
}

int
inotify_add_watch (int fd, const char *pathname, uint32_t mask)
{
	g_error ("inotify_add_watch");
	return 0;
}

#ifndef __EMSCRIPTEN_PTHREADS__
int
sem_timedwait (sem_t *sem, const struct timespec *abs_timeout)
{
	g_error ("sem_timedwait");
	return 0;
}
#endif

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
	errno = ENOTSUP;
	return -1;
}

G_END_DECLS

/* Helper for runtime debugging */
void
mono_wasm_print_stack_trace (void)
{
	EM_ASM(
		   var err = new Error();
		   console.log ("Stacktrace: \n");
		   console.log (err.stack);
		   );
}

#endif // HOST_BROWSER

gpointer
mono_arch_load_function (MonoJitICallId jit_icall_id)
{
	return NULL;
}

MONO_API void
mono_wasm_enable_debugging (int log_level)
{
	mono_wasm_debug_level = log_level;
}

MONO_API int
mono_wasm_get_debug_level (void)
{
	return mono_wasm_debug_level;
}

/* Return whenever TYPE represents a vtype with only one scalar member */
gboolean
mini_wasm_is_scalar_vtype (MonoType *type, MonoType **etype)
{
	MonoClass *klass;
	MonoClassField *field;
	gpointer iter;

	if (etype)
		*etype = NULL;

	if (!MONO_TYPE_ISSTRUCT (type))
		return FALSE;
	klass = mono_class_from_mono_type_internal (type);
	mono_class_init_internal (klass);

	int size = mono_class_value_size (klass, NULL);
	if (size == 0 || size > 8)
		return FALSE;

	iter = NULL;
	int nfields = 0;
	field = NULL;
	while ((field = mono_class_get_fields_internal (klass, &iter))) {
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
			continue;
		nfields ++;
		if (nfields > 1)
			return FALSE;
		MonoType *t = mini_get_underlying_type (field->type);
		int align, field_size = mono_type_size (t, &align);
		// inlinearray and fixed both work by having a single field that is bigger than its element type.
		// we also don't want to scalarize a struct that has padding in its metadata, even if it would fit.
		if (field_size != size) {
			return FALSE;
		} else if (MONO_TYPE_ISSTRUCT (t)) {
			if (!mini_wasm_is_scalar_vtype (t, etype))
				return FALSE;
		} else if (!(MONO_TYPE_IS_PRIMITIVE (t) || MONO_TYPE_IS_REFERENCE (t) || MONO_TYPE_IS_POINTER (t))) {
			return FALSE;
		} else {
			if (etype)
				*etype = t;
		}
	}

	// empty struct
	if (nfields == 0 && etype) {
		*etype = m_class_get_byval_arg (mono_defaults.sbyte_class);
	}

	/*
	 * REFUSE, DO NOT ABORT. This used to be `g_assert (!etype || *etype)`, i.e. "a scalar vtype always
	 * yields a scalar type" -- but the JIT reaches this about arbitrary IKVM-generated value types on a
	 * compile worker, and an abort here loses the whole run over one method the interpreter is executing
	 * correctly. Returning FALSE costs that method the scalar-vtype ABI and nothing else.
	 *
	 * It is safe because THE CALLERS ALREADY HANDLE EXACTLY THIS CASE: mini-wasm-ir.inc:1402 is
	 * `if (!mini_wasm_is_scalar_vtype (ut, &etype) || !etype)` -- it declines on a TRUE return with no
	 * etype rather than trusting the assert. So the invariant was already not relied upon by the code it
	 * was protecting, which is the difference between this assert and the ones in get_call_info below
	 * (whose only failure representation, ArgInvalid, no consumer handles -- converting those would turn
	 * an abort into silent bad codegen, so they stay).
	 */
	if (etype && !*etype)
		return FALSE;

	return TRUE;
}
