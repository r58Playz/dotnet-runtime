#ifndef __MONO_MINI_WASM_H__
#define __MONO_MINI_WASM_H__

#include <mono/utils/mono-sigcontext.h>
#include <mono/utils/mono-context.h>

#define MONO_ARCH_CPU_SPEC mono_wasm_desc

#define MONO_MAX_IREGS 1
#define MONO_MAX_FREGS 1
#define MONO_MAX_XREGS 1

#define WASM_REG_0 0

// Does the ABI have a volatile non-parameter register, so tailcall
// can pass context to generics or interfaces?
#define MONO_ARCH_HAVE_VOLATILE_NON_PARAM_REGISTER 0

#define MONO_ARCH_AOT_SUPPORTED 1
#define MONO_ARCH_LLVM_SUPPORTED 1
#define MONO_ARCH_GSHARED_SUPPORTED 1
#define MONO_ARCH_GSHAREDVT_SUPPORTED 1
#define MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES 1
#define MONO_ARCH_NEED_DIV_CHECK 1
#define MONO_ARCH_NO_CODEMAN 1

#define MONO_ARCH_EMULATE_FREM 1
#define MONO_ARCH_EMULATE_FCONV_TO_U8 1
#define MONO_ARCH_EMULATE_FCONV_TO_U4 1
#define MONO_ARCH_NO_EMULATE_LONG_SHIFT_OPS 1
#define MONO_ARCH_NO_EMULATE_LONG_MUL_OPTS 1
#define MONO_ARCH_FLOAT32_SUPPORTED 1

//mini-codegen stubs - this doesn't do anything
#define MONO_ARCH_CALLEE_REGS (1 << 0)
#define MONO_ARCH_CALLEE_FREGS (1 << 1)
#define MONO_ARCH_CALLEE_XREGS (1 << 2)
#define MONO_ARCH_CALLEE_SAVED_FREGS (1 << 3)
#define MONO_ARCH_CALLEE_SAVED_REGS (1 << 4)
#define MONO_ARCH_INST_FIXED_REG(desc) FALSE
#define MONO_ARCH_INST_IS_REGPAIR(desc) FALSE
#define MONO_ARCH_INST_REGPAIR_REG2(desc,hreg1) (-1)
#define MONO_ARCH_INST_SREG2_MASK(ins) 0

struct MonoLMF {
	/*
	 * If the second lowest bit is set to 1, then this is a MonoLMFExt structure, and
	 * the other fields are not valid.
	 */
	gpointer previous_lmf;
	gpointer lmf_addr;

	MonoMethod *method;
};

typedef struct {
	gpointer cinfo;
} MonoCompileArch;

#define MONO_ARCH_INIT_TOP_LMF_ENTRY(lmf) do { } while (0)

#define MONO_CONTEXT_SET_LLVM_EXC_REG(ctx, exc) do { (ctx)->llvm_exc_reg = (gsize)exc; } while (0)

#define MONO_INIT_CONTEXT_FROM_FUNC(ctx,start_func) do {	\
	int ___tmp = 99;	\
	MONO_CONTEXT_SET_IP ((ctx), (start_func));	\
	MONO_CONTEXT_SET_BP ((ctx), (0));	\
	MONO_CONTEXT_SET_SP ((ctx), (&___tmp));	\
} while (0)


#define MONO_ARCH_VTABLE_REG WASM_REG_0
#define MONO_ARCH_IMT_REG WASM_REG_0
#define MONO_ARCH_RGCTX_REG WASM_REG_0

/* must be at a power of 2 and >= 8 */
#define MONO_ARCH_FRAME_ALIGNMENT 16

// Does the ABI have a volatile non-parameter register, so tailcall
// can pass context to generics or interfaces?
#define MONO_ARCH_HAVE_VOLATILE_NON_PARAM_REGISTER 0

#define MONO_ARCH_AOT_SUPPORTED 1
#define MONO_ARCH_LLVM_SUPPORTED 1
#define MONO_ARCH_GSHAREDVT_SUPPORTED 1
#define MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES 1

#define MONO_ARCH_SIMD_INTRINSICS 1

#define MONO_ARCH_INTERPRETER_SUPPORTED 1
#define MONO_ARCH_HAS_REGISTER_ICALL 1
#define MONO_ARCH_HAVE_SDB_TRAMPOLINES 1
#define MONO_ARCH_LLVM_TARGET_LAYOUT "e-m:e-p:32:32-i64:64-n32:64-S128"
#ifdef TARGET_WASI
#define MONO_ARCH_LLVM_TARGET_TRIPLE "wasm32-unknown-wasip2"
#else
#define MONO_ARCH_LLVM_TARGET_TRIPLE "wasm32-unknown-emscripten"
#endif

// sdks/wasm/driver.c is C and uses this
G_EXTERN_C void mono_wasm_enable_debugging (int log_level);
G_EXTERN_C int mono_wasm_get_debug_level (void);

#ifdef HOST_BROWSER

//JS functions imported that we use
#ifdef DISABLE_THREADS
void mono_wasm_execute_timer (void);
void mono_wasm_main_thread_schedule_timer (void *timerHandler, int shortestDueTimeMs);
#endif // DISABLE_THREADS

void mono_wasm_print_stack_trace (void);
#endif // HOST_BROWSER



gboolean
mini_wasm_is_scalar_vtype (MonoType *type, MonoType **etype);

/*
 * wasm full-method JIT statistics (gated by MONO_WASM_JIT_STATS=1).
 *
 * A single counter array replaces the old sprawl of ad-hoc int globals; both mini-wasm.c (the
 * emitter) and interp/interp.c (the interp<->JIT transition glue) bump it via the helpers below.
 * Counters hold RAW values: plain counts, except the WJC_ELAPSED_* timers which hold MICROSECONDS
 * and WJC_BYTES_GENERATED which holds bytes. Storage is gint64 because on wasm32 `long` is 32-bit
 * and the per-frame transition counts (hundreds of thousands/frame) overflow it over a 60s bench.
 *
 * The consumer harness (ikvmcraft frontend/src/dotnet/jitbench.ts, `const WJ`) mirrors this enum BY
 * INDEX and reads each counter via the mono_wasm_jit_get_counter export. KEEP THE ORDER STABLE:
 * append new counters immediately before WJC_MAX only, and update the harness mirror in lockstep.
 * There is no way to detect index drift from JS — mono_wasm_jit_get_counter returns 0 for an
 * out-of-range index, so a stale mirror reads as "that counter never moved" rather than as an error.
 * The `g_static_assert (WJC_MAX == N)` above mono_wasm_jit_dump_stats breaks the build on every
 * append: that is the reminder to update BOTH printers (that function and jitbench.ts).
 */
enum {
	WJC_REGISTERED, WJC_BAILED, WJC_INVALID,
	WJC_INVOKE, WJC_RESIDUAL, WJC_FASTVCALL,
	WJC_AOT_ROUTED, WJC_INTERP_ROUTED,
	WJC_VIC_HIT, WJC_VIC_MISS, WJC_VFAST_HAD, WJC_VFAST_NEW, WJC_VFB_THRESH, WJC_VFB_PERM,
	WJC_VPERM_EH, WJC_VPERM_LDADDR, WJC_VPERM_LCMP, WJC_VPERM_OTHEROP, WJC_VPERM_OTHER,
	/* NB: WJC_REF_HWM sat here and was REMOVED, shifting every counter below it down by one. It held the
	 * high-water depth of the old ref shadow stack (wj_ref_sp - wj_ref_base, against WJ_REFSTACK_SLOTS),
	 * which no longer exists — pins are per-frame C-stack slots now (WJC_REF_SLOTS / WJC_FRAME_BYTES), and
	 * the enter/leave imbalance it was meant to reveal is caught directly, with the method named, by the
	 * C-stack balance checks in interp.c. This is the one exception to append-only: it had no writer, so
	 * nothing could regress, and leaving a permanently-zero slot in a hot-path array is worse than the
	 * one-time cost of renumbering the harness mirror alongside it. */
	/* compile-time accounting (Part 2) */
	WJC_BYTES_GENERATED, WJC_ELAPSED_GENERATION, WJC_ELAPSED_INSTANTIATION, WJC_COMPILE_ATTEMPTS,
	/* Root compiles (ATTEMPT/COMPLETED) and Lever A's upward promotions (PROMOTED_UP). The other four were island
	 * outcomes; nothing writes them since R366 and their slots stay so the by-index mirrors do not shift. */
	WJC_ISLAND_ATTEMPT, WJC_ISLAND_COMPLETED, WJC_ISLAND_BUDGET_EXHAUSTED, WJC_ISLAND_DEPTH_EXCEEDED,
	WJC_ISLAND_BLOCKED_COLD, WJC_PROMOTED_UP, WJC_PROMOTED_DOWN,
	/* finer split of the perm-unjittable vcall residual (was lumped into WJC_VPERM_OTHER): which override
	 * shape dominates the steady-state virtual-dispatch boundary cost. SIG=arg/ret type; the rest as named.
	 * AOT = the override is NOT wasm-jitted because it already has native AOT code (slot==-1, bail==0): the
	 * vcall falls back to the AOT residual, NOT an emitter bail — this is the bulk of the perm vcall cost. */
	WJC_VPERM_SIG, WJC_VPERM_BYREF, WJC_VPERM_GSHARED, WJC_VPERM_SYNC, WJC_VPERM_EHOTHER, WJC_VPERM_AOT,
	/* vcalls that took the fast AOT dispatch (MONO_WASM_JIT_VCALL_AOT) instead of the residual */
	WJC_VCALL_AOT_FAST,
	/* WJC_PARKED = retriable compile results, printed as `[wasm-jit retry] retriable=`; the name predates R367,
	 * when a BLOCKED result parked on its blockers. WAITER_WOKEN is a retired slot (no writer since R367). */
	WJC_PARKED, WJC_WAITER_WOKEN,
	/* below-threshold vcall fallback (WJC_VFB_THRESH) split by the target's wasm_jit_slot state, so we can
	 * tell "cold callee, interp is fine" apart from "hot method whose island won't close" (the real interp-
	 * residual driver): VFB_COLD = slot 0 (still counting), VFB_RETRY = slot -3 (a retriable result or compile-lock
	 * contention). VFB_PARKED (slot -2) is a retired slot: nothing writes -2 since R367. */
	WJC_VFB_COLD, WJC_VFB_PARKED, WJC_VFB_RETRY,
	/* fast-path VOLUME counters, emitted INTO the JITted wasm (gated by MONO_WASM_JIT_PROFILE_FAST, OFF by
	 * default so normal STATS runs are unperturbed). The dispatch fast paths call NO counting helper, so
	 * without these the counted totals (invoked/fastvcall/residual) exclude them and frame cost can't be
	 * attributed. FAST_INLINE_AOT = INLINE_AOT direct AOT call_indirect; FAST_VIC = inline f-slot IC hit
	 * (JIT->JIT); FAST_AOTIC = inline AOT-IC hit (JIT->AOT). */
	WJC_FAST_INLINE_AOT, WJC_FAST_VIC, WJC_FAST_AOTIC,
	/* GC-classification alignment (GCMAPS/taint work): REFBASES_EXTRA counts vregs the REFBASES
	 * dereference-pinning pass flipped to ref that the structural-seed fixpoint had NOT already
	 * classified ref. A long soak at 0 proves REFBASES is formally subsumed by the structural
	 * marking (compute_gc_maps seeds + add/sub taint) and can stay off. Each nonzero hit is a
	 * named counterexample (logged under MONO_WASM_JIT_REFVERIFY). */
	WJC_REFBASES_EXTRA,
	/* rgctx CALLSITE bails (bail -12, split out of WJC_VPERM_GSHARED): the perm callee is a concrete
	 * method whose body makes an indirect/virtual call carrying MONO_ARCH_RGCTX_REG — fixable per-site
	 * (route that one call through the residual), unlike the whole-method gshared gate (-8). */
	WJC_VPERM_RGCTX,
	/* vtype ABI coverage (WS-B B3): methods REGISTERED with >=1 by-addr vtype arg / with a hidden vret —
	 * direct visibility that the new ABI paths are actually being exercised, not silently bailed. */
	WJC_VT_BYADDR_METHODS, WJC_VRET_METHODS,
	/* transition-elision paths added after profile18. RESIDUAL_HEALED counts successful late-fslot
	 * discoveries (and therefore direct JIT->JIT calls from immutable residual sites). FAST_DELEGATE is
	 * emitted only with PROFILE_FAST and counts scalar Delegate.Invoke recipes entered through the target
	 * f-thunk directly, bypassing call_delegate/invoke_caught/e-thunk. DELEGATE_IC_HIT is the subset which
	 * also bypassed vcall_resolve_fslot by consuming the recipe directly in generated wasm. */
	WJC_RESIDUAL_HEALED, WJC_FAST_DELEGATE, WJC_DELEGATE_IC_HIT,
	/* GC pin-pressure accounting (ref write-through / slot elision / dead-slot zeroing work).
	 * Compile-time counts, summed over all compiles: REF_SLOTS = frame ref slots allocated;
	 * REF_WT_VREGS = write-through ref vregs (local is home, slot is the pin mirror);
	 * SLOTS_ELIDED = isref vregs that needed NO slot (no GC point inside their def->use range);
	 * SLOT_ZERO_STORES = dead-slot zero stores emitted at a vreg's last use;
	 * FRAME_BYTES = total C-stack frame bytes across compiled methods (ref + addr slots). */
	WJC_REF_SLOTS, WJC_REF_WT_VREGS, WJC_SLOTS_ELIDED, WJC_SLOT_ZERO_STORES, WJC_FRAME_BYTES,
	/* MONO_WASM_JIT_LCSE reach. LOADS_SEEN is every membase load routed through the LOADM macro while
	 * the pass is on, so HITS/LOADS_SEEN is the true elimination rate -- ADDS/HITS is not, because ADDS
	 * silently under-counts whenever the table is full. EVICT counts adds that displaced an older entry,
	 * i.e. tells you directly whether WJ_LCSE_LOADS is the binding constraint. */
	WJC_LCSE_LOADS_SEEN, WJC_LCSE_ADDS, WJC_LCSE_HITS, WJC_LCSE_EVICT,
	/* residual calls that entered the callee's own JITted e-slot directly, skipping interp_entry */
	WJC_ESLOT_RESIDUAL,
	/* Call-form census, counted at ASSEMBLY: every hole a body left, by the form the assembler chose for
	 * it. Compile-time counts, not execution counts, and the direct mechanism reading for both the import
	 * conversion and co-location -- a change meant to remove call_indirect shows up here in one run, where
	 * the fps A/B that would confirm it costs hours against a ~9% resolution floor.
	 *   LOCAL     `call <funcidx>`  1 x86, and the only form V8 will inline through
	 *   IMPORT    `call <import>`   ~5 x86 (three loads from WasmDispatchTableForImports + `call *`)
	 *   INDIRECT  `i32.const <slot>; call_indirect`  ~15 x86, never inlined
	 * Re-assembling a member counts it again, deliberately: the census is per module built, so comparing
	 * it across generations is how a rebatch is shown to have actually retargeted anything. */
	WJC_CALL_LOCAL, WJC_CALL_IMPORT, WJC_CALL_INDIRECT,
	/* Members re-framed into a shared module by mono_wasm_jit_colocate_deps_now, counted per GROUP FORMED
	 * (a group of 4 adds 4). Read it against WJC_REGISTERED for the fraction of the tier that is co-located,
	 * and against WJC_CALL_LOCAL for whether co-location actually retargeted any call. Those two can move
	 * apart: grouping a caller with a callee it turns out not to call directly costs a re-frame and buys
	 * nothing, and that is exactly the case a greedy first-come partition can produce. */
	WJC_COLOCATED_MEMBERS,
	/* DEVIRT PREDICTION CENSUS, counted at EMIT time, one counter per exit of the speculative-devirt
	 * gate in the vcall lowering. R156 measured coverage at 27.8% of ordinary virtual sites by reading
	 * the emitted bytes (scratchpad/wj/vcallreach.py) -- that says WHAT the coverage is but not WHY the
	 * other 72% missed, and the candidate causes imply OPPOSITE fixes: "site never warmed" argues for a
	 * higher JIT threshold, "site polymorphic" argues that a higher threshold makes it strictly worse
	 * (a second receiver disqualifies a site permanently, so more warmup can only lose sites), and
	 * "target not JITted yet" argues for neither. Splitting them needs one run, not an argument.
	 *   DEVIRT_SITE      ordinary virtual sites reaching the gate
	 *   DEVIRT_NO_REC    the caller has no profile record for this base at all
	 *   DEVIRT_COLD      record exists, fewer than 8 observations
	 *   DEVIRT_POLY      warm, but not perfectly monomorphic (Boyer-Moore margin != total)
	 *   DEVIRT_POLY_90   the subset of POLY that WOULD pass a >=90%-frequency bar (margin/total >= 0.8).
	 *                    `margin` is a MARGIN, not an occurrence count, so the winner's share is
	 *                    (1 + margin/total)/2 and 0.8 is exactly 90%. This sizes the relaxation without
	 *                    shipping it -- the current bar rejects a 99%-monomorphic site and a 50/50 site
	 *                    identically, because once margin < total it can never equal total again.
	 *   DEVIRT_SIG       predicted, but the override's functype does not match the call site's
	 *   DEVIRT_NO_FSLOT  predicted, but the target owns no admitted f-slot yet
	 *   DEVIRT_EMITTED   a predicted arm was actually laid down
	 *   DELEGATE_SITE    Delegate.Invoke sites emitted: the population the gate EXCLUDES outright
	 *                    (`!is_delegate_invoke` guards both the predict call and the emitted arm, and
	 *                    the recorder passes target = NULL for a delegate site so its margin stays 0).
	 * Counted per EMIT ATTEMPT, like WJC_BAILED and unlike WJC_REGISTERED: a method the island driver
	 * re-emits contributes its sites again, and a method that bails after this point still contributes.
	 * Read the ratios, not the absolutes -- the absolutes exceed the distinct-site count in the tier.
	 * FAST_DEVIRT is the matching EXECUTION counter (PROFILE_FAST only). Without it the vcall
	 * denominator has no term for the predicted arm at all, which is why R155's "79% take the IC" was
	 * 79% of a pool that structurally could not contain the thing it was compared against. */
	WJC_DEVIRT_SITE, WJC_DEVIRT_NO_REC, WJC_DEVIRT_COLD, WJC_DEVIRT_POLY, WJC_DEVIRT_POLY_90,
	WJC_DEVIRT_SIG, WJC_DEVIRT_NO_FSLOT, WJC_DEVIRT_EMITTED, WJC_DELEGATE_SITE, WJC_FAST_DEVIRT,
	/* Retired slots: MONO_WASM_JIT_DEVIRT_FORCE went with the islands (R366; a NO_FSLOT arm now takes a lazy pool
	 * slot, WJC_LAZY_ARM). Kept so the by-index mirrors do not shift. */
	WJC_DEVIRT_FORCED, WJC_DEVIRT_FORCE_CAPPED,
	/* THE CAPTURED-EDGE COUNT, and the only place it is recorded. Dependency entries dropped when a re-framed module's dependency
	 * set was recomputed from the assembler instead of inherited from the generation each member was
	 * compiled as. Every entry counted here is a callee that became a module-local `call <funcidx>` and so
	 * needs no admission -- i.e. this is the size of the admission closure the old code was demanding for
	 * nothing. Read it against WASM_JIT_BATCH_ADMIT_FAIL and WASM_JIT_ADMIT_DEFER_GIVEUP, which is what
	 * that surplus closure was costing (1966 and 1540 in one in-world run, against 0 and 3 in the control).
	 * Summed over members and over re-framings, so it is a volume, not a distinct-edge count. */
	WJC_TIGHT_DEPS_DROPPED,
	/* MONO_WASM_JIT_COLOCATE_ROLLBACK: members returned to their standalone modules because the group they
	 * were bound into could not be admitted. Counted per MEMBER, so read it against COLOCATED_MEMBERS for
	 * the fraction of co-location attempts that did not stick. Every one of these used to be a method
	 * permanently denied the JIT tier (wj_desc_state = 3) and therefore interpreted for the rest of the
	 * run -- 2,078 distinct methods in one measured run, and 812 ms/frame against a 50 ms control. */
	WJC_COLOCATE_ROLLBACK,
	/* MONO_WASM_JIT_COLOCATE_SCC: groups NOT formed because dropping a callee would have left a dependency
	 * cycle spanning two modules, which admission cannot order. Refusing costs only the co-location; the
	 * members keep working standalone modules. Expect this to be small -- R161 measured 11 such cycles in
	 * the whole tier. If it is ever large, the partition is cutting through dense regions and wants a real
	 * SCC pass rather than this pairwise guard. */
	WJC_COLOCATE_SCC_REFUSED,
	/* Admission failures split by whether this worker will EVER retry. ADMIT_FAIL_PERM = state 3 (the
	 * module's bytes failed to compile/link here); ADMIT_FAIL_RETRY = state 0 (an ordering miss: a
	 * dependency was not live on this worker yet). Before R166 every failure took the PERM path, which
	 * for a batch condemned all n members at once -- see the `fail:` label in mono_wasm_jit_admit. */
	WJC_ADMIT_FAIL_PERM, WJC_ADMIT_FAIL_RETRY,
	/* The fourth arm of the WJC_VFB_THRESH split, and the one whose ABSENCE hid R166 for a full session:
	 * the target has slot > 0 (it IS JIT-compiled) but mono_wasm_jit_admit_live returned 0, so this worker
	 * cannot dispatch to it and the call goes to the interpreter. Every other slot state had a counter, so
	 * this route showed up not as a gap but as VFB_THRESH being 9,243x larger with no explanation.
	 * VFB_COLD + VFB_RETRY + VFB_NOTLIVE == VFB_THRESH; check that before trusting a share. */
	WJC_VFB_NOTLIVE,
	/* mono_wasm_jit_admit_live's failure routes, one counter each. R166's first fix targeted the four
	 * state-1/state-3 leaks in mono_wasm_jit_admit's `fail:` label and moved VFB_NOTLIVE by 2.6% (109.6M ->
	 * 106.7M) while ADMIT_FAIL_PERM and ADMIT_FAIL_RETRY both stayed at 0 -- i.e. `fail:` is never reached
	 * and the leaks were not the path being taken. admit_live is `admit() && desc_admitted()`, and with no
	 * failure counter firing, admit must be returning 1 while desc_admitted returns 0. These five split
	 * that conjunction into its actual branches instead of a third guess:
	 *   AL_ADMIT0  admit() itself said no (without reaching `fail:`)
	 *   AL_STATE   admit() said yes but the descriptor is not state 2 -- the state-1 cycle-break, which
	 *              returns 1 from mono_wasm_jit_admit while desc_admitted requires 2
	 *   AL_GEN     state 2 but wj_desc_generation != re->generation
	 *   AL_ELIVE / AL_FLIVE  state and generation agree, but the e- or f-slot is not live on THIS worker */
	WJC_AL_ADMIT0, WJC_AL_STATE, WJC_AL_GEN, WJC_AL_ELIVE, WJC_AL_FLIVE,
	/* Registrations that reused an f-slot still owned by an EARLIER descriptor for the SAME method. A
	 * different method hitting that slot is refused loudly (WASM_JIT_FSLOT_COLLISION); this counts only
	 * the legitimate case. Re-emission taking back its own pinned pair was the original producer and is
	 * gone; rebatch re-framing is what reaches it now. */
	WJC_FSLOT_REREGISTER,
	/* WHY a co-location group was not formed, one counter per exit of
	 * mono_wasm_jit_colocate_deps_now. Until these existed the function had six distinct rejection
	 * paths and a single counter on one of them (SCC_REFUSED), so "co-location reaches ~1% of the
	 * Minecraft tier" was an observation with no attribution behind it -- exactly the bare `continue`
	 * that no counter is watching.
	 *
	 * TRY is the denominator: every call that got past the knob and the desc_id check. The identity to
	 * assert before quoting any share of it is
	 *
	 *   COLOCATE_TRY == COLOCATE_SELF + COLOCATE_SINGLETON + COLOCATE_SCC_REFUSED
	 *                   + COLOCATE_REBATCH_FAIL + (groups actually formed)
	 *
	 * where "groups actually formed" is COLOCATED_MEMBERS counted in GROUPS rather than members, which
	 * is why COLOCATE_FORMED exists as its own counter rather than being derived from the member total.
	 *
	 * The COLOCATE_SELF_* arms are the self-entry preconditions (they reject before any callee is
	 * examined); the COLOCATE_DEP_* arms are per-CALLEE and so are volumes, not group counts -- one
	 * refused group can contribute several. DEP_BATCHED is the one to read first: it is the
	 * append-only partition refusing a callee that some other caller already claimed, i.e. the blocker
	 * that 2b lifts. */
	WJC_COLOCATE_TRY, WJC_COLOCATE_FORMED,
	WJC_COLOCATE_SELF_BATCHED, WJC_COLOCATE_SELF_NO_DEPSET, WJC_COLOCATE_SELF_REFUSED,
	WJC_COLOCATE_DEP_UNREG, WJC_COLOCATE_DEP_NOBODY, WJC_COLOCATE_DEP_BATCHED,
	WJC_COLOCATE_DEP_REFUSED, WJC_COLOCATE_DEP_BYTE_CAP, WJC_COLOCATE_MEMBER_CAP,
	WJC_COLOCATE_SINGLETON, WJC_COLOCATE_REBATCH_FAIL,
	/* CO-LOCATION REACH AS EXECUTED, split by where the runtime target actually lives. Bumped in
	 * wj_vcall_pic_publish, because that is the only place the resolved target and the calling descriptor
	 * are both in hand.
	 *
	 * WEIGHTING, CORRECTED: this is ONCE PER IC MISS, not "once per distinct (site, receiver) pair" as
	 * this comment used to claim -- the bump is unconditional inside the publish, before the pair dedup
	 * below it. The magnitudes say so too: ~80-94 M per run, which is miss traffic, not a count of
	 * distinct pairs. Read it as miss-weighted, and note the bias that implies: a polymorphic site misses
	 * repeatedly and is over-weighted relative to a monomorphic one that hits, and IC HITS are not counted
	 * at all. So it is an execution weighting of the MISS population, which is the right denominator for
	 * "would grouping have helped this dispatch" and the wrong one for "what share of all dispatch".
	 *
	 * Why this and not WJC_CALL_LOCAL: CALL_LOCAL counts relocations the assembler turned into
	 * `call <funcidx>`, which is the call-FORM half of co-location. R183's differential measured the
	 * GROUPING half -- same module, same instance, calls still `call_indirect` -- at -7.0% of a -12.8%
	 * total, i.e. 55% of the win, and CALL_LOCAL is structurally blind to it (R182: CALL_LOCAL was 4.0%
	 * of calls at 73% reach). A dispatch site gets the grouping half exactly when its target is a
	 * sibling, because V8's CallIndirectIC records a precise target only while
	 * `implicitArg == current instance` (builtins/wasm.tq:821-835).
	 *
	 *   VIC_TGT_SIBLING   target shares the caller's WjBatchDesc -> V8 can track and inline it
	 *   VIC_TGT_FOREIGN   target is OURS but in another group -> a reach problem a planner can fix
	 *   VIC_TGT_NOTOURS   no descriptor on this worker (AOT / main-module) -> needs OVER_AOT, not grouping
	 *
	 * The three sum to "publishes with a resolved f-slot". Do not collapse FOREIGN and NOTOURS: that is
	 * the mistake WJC_SHADOW_REFUSED made before R167 split it, and they lead to opposite decisions.
	 *
	 * TWO SELECTION BIASES, and the second one nearly made this a tautology of the shadowNojit kind
	 * (R169). Read them before quoting the number.
	 *
	 * (1) Co-location runs at publish and groups are append-only, so a site whose target joins a group
	 *     AFTER its last miss stays FOREIGN forever. Biases DOWN.
	 *
	 * (2) THIS IS THE MISS PATH, and the sites whose targets are co-locatable are exactly the ones that
	 *     do not reach it. A virtual target becomes a member of the caller's group only if it is in
	 *     re->depset, i.e. only if some WASM_RELOC_CALL hole names it -- and for a dispatch site that
	 *     means a DEVIRT PREDICTED ARM. A site with a predicted arm hits it and never publishes. So this
	 *     counter samples the population that by construction has no direct edge to its target.
	 *
	 * What it therefore DOES answer, and nothing else here answers: of the dispatches that fall through
	 * the inline cache, how many find their target already co-resident? MEASURED on jbox2d at 73% member
	 * reach (389 methods, 285 grouped): sibling 3, foreign 2,902,973, notours 0 -- i.e. ~0%. That is a
	 * real result. The ordinary-virtual MISS pool gets nothing from the current grouping, which is the
	 * measurement behind building a co-location reader over the call profile.
	 *
	 * What it does NOT answer is "how much of the grouping half of R183's -7.0% is reaching dispatch".
	 * That question is about the predicted arms and the intra-group direct edges, and the instrument for
	 * it is vcallreach.py's per-arm call-form split over a tier dump -- static, free, and already
	 * written. Do not substitute this counter for it. */
	WJC_VIC_TGT_SIBLING, WJC_VIC_TGT_FOREIGN, WJC_VIC_TGT_NOTOURS,
	/* MERGING existing groups (R193/Stage 2b). MERGED counts pre-existing groups ABSORBED into a new
	 * request -- the conversion of what R192 measured as 100% of per-callee co-location refusals.
	 * MERGE_SPLIT counts requests refused by mono_wasm_jit_rebatch because a member's group was only
	 * PARTIALLY present, which would be a split rather than a merge and is the one thing this must never
	 * do: the members of a group share one WebAssembly.Instance and cannot be instantiated apart, and
	 * wj_batch_rollback can only restore an absorbed member if every sibling is there to restore with it.
	 * Read MERGED against COLOCATE_DEP_BATCHED: the two now split the population that used to be all
	 * DEP_BATCHED, so DEP_BATCHED falling while MERGED rises is the mechanism working. */
	WJC_COLOCATE_MERGED, WJC_COLOCATE_MERGE_SPLIT,
	/* Merges UNDONE, split out of WJC_COLOCATE_ROLLBACK because they are a different and much more
	 * dangerous event. An ordinary rollback returns members to standalone modules they were already
	 * running. A MERGE rollback must return each absorbed member to the GROUP it came from -- restoring
	 * `saved[].batch` rather than clearing it -- because for such a member `saved[].bytes` is that
	 * group's shared blob, which exports e<i>/f<i> per member and not e/f. Clearing instead of restoring
	 * leaves the entry claiming a standalone module whose exports do not exist, which is the
	 * "function signature mismatch" R165 spent a session on.
	 *
	 * jbox2d exercised 98 merges with rolled_back=0, so this path is IMPLEMENTED AND UNTESTED. Treat a
	 * non-zero count here as the first observation of it, not as routine. */
	WJC_COLOCATE_MERGE_ROLLBACK,
	/* WHY a merge closure was refused, splitting what otherwise all lands in COLOCATE_DEP_BATCHED.
	 * R194 left 18,558 of these on Minecraft WITH merging on, and they are the arm blocker: an arm's
	 * target is registered as a direct dep (wj_result_add_direct_dep), so a method that HAS arms
	 * necessarily has a depset -- which means `no_depset` cannot be what stops its arms co-locating, and
	 * the surviving DEP_BATCHED volume is. Without this split there is no way to tell a closure refused
	 * by a sibling's preconditions (fixable in the rules) from one refused by a cap (fixable by a knob),
	 * and those lead to opposite work.
	 *
	 *   MERGE_PRECOND  a sibling of the callee's group failed the per-member preconditions -- not
	 *                  registered here, no retained body, no slot pair, or previously colocate_refused
	 *   MERGE_CAP      the closure would exceed COLOCATE_MAX members or COLOCATE_BYTES wire bytes
	 *
	 * MERGE_PRECOND + MERGE_CAP + MERGED == the times a batched callee was reached with merging on. */
	WJC_COLOCATE_MERGE_PRECOND, WJC_COLOCATE_MERGE_CAP,
	/* Late-f-slot healing sites recorded at emit: sites paying a helper call, a branch and a dynamic
	 * call_indirect on EVERY dispatch (~19.5M/run, R196) to re-discover an f-slot the callee usually
	 * acquires shortly afterwards. MONO_WASM_JIT_HEAL_WAIT paired this with HEAL_WOKEN and woke the
	 * method for re-emission, which deleted the block; both went with re-emission (the mechanism fired,
	 * 90 of 107 sites woken, and the effect was nil at a 0.057%% ceiling). The census stays because it
	 * sizes the pool a RELOCATABLE f-slot hole would address, which is what R195 argues for instead. */
	WJC_HEAL_SITES,
	/* R200: the two uncounted routes that kept a parts-sum identity from closing mechanically.
	 * SHADOW_SELF is the `callee == self` early return in wj_shadow_candidate -- self-recursion, which must
	 * never be shadowed, and which was silently absorbed into SHADOW_REFUSED's residual (191 of 25,242).
	 * ASM_NONAME is a member reaching wj_assemble with name == NULL: harmless to execution but it degrades
	 * that method's perf symbol to a bare `wasmjit`, i.e. it silently blinds every instrument in
	 * scratchpad/wj that resolves by name. Must stay 0. */
	WJC_ASM_NONAME,
	/* R205: EXECUTION-WEIGHTED devirt diagnosis. The [wasm-jit devirt] census counts SITES at emit time,
	 * unweighted, so it cannot say whether the IC's executed volume comes from sites that failed devirt or
	 * from the SECOND RECEIVER at sites that succeeded -- the predicted arm keeps the IC below it as a
	 * fallthrough (see the `Adaptive slim` comment at the guard), so both feed WJC_FAST_VIC.
	 * These tag each emitted IC with the devirt outcome of ITS OWN site, so the runtime volume splits by
	 * cause. PRED = the site HAS a predicted arm and this is the alternate receiver, which no amount of
	 * coverage work can remove; the rest are sites coverage could in principle reach.
	 * Require PROFILE_FAST, like every other FAST_* counter. */
	WJC_FAST_VIC_PRED, WJC_FAST_VIC_NO_REC, WJC_FAST_VIC_COLD,
	WJC_FAST_VIC_POLY, WJC_FAST_VIC_NO_FSLOT, WJC_FAST_VIC_OTHER,
	/* R206 SECOND GUARDED ARM (MONO_WASM_JIT_DEVIRT_ARM2). EMITTED/THIN/NO_ALT/NO_FSLOT/SIG/SELF are
	 * emit-time and partition every site that reached the arm-2 gate; FAST_DEVIRT2 is the EXECUTED hit
	 * count and needs PROFILE_FAST. Read FAST_DEVIRT2 against FAST_VIC_PRED from the run BEFORE the arm
	 * existed: that is the traffic arm 2 was built to capture, so the two should trade off one for one.
	 * THIN is not a failure -- it is the break-even refusing a site where an arm would LOSE (vcall_ways
	 * 4->1 was +9.6% fps precisely because those ways caught ~1%). */
	WJC_DEVIRT_ARM2_EMITTED, WJC_DEVIRT_ARM2_THIN, WJC_DEVIRT_ARM2_NO_ALT,
	WJC_DEVIRT_ARM2_NO_FSLOT, WJC_DEVIRT_ARM2_SIG, WJC_DEVIRT_ARM2_SELF, WJC_FAST_DEVIRT2,
	/* R206b: a POLY site that received a FIRST arm. Such a site is counted in BOTH devirt `poly` (bumped
	 * when the prediction was refused) and `emitted` (bumped when the arm is laid down), so the devirt
	 * census only reconciles as: sites == emitted + no_rec + cold + poly + sig + no_fslot - poly_arm1.
	 * Without this the parts-sum silently gains a double-count the moment ARM2 is enabled. */
	WJC_DEVIRT_POLY_ARM1,
	/* R207: WHY wj_arm_abi_ok refused. ARM2_SIG was one bucket and measured 1,725 -- about a third of
	 * candidates -- which is suspicious, since overrides of ONE virtual method should lower to identical
	 * wasm functypes. Split so the cause is readable instead of guessed at: INVALID = the signature does
	 * not lower at all (mono_wasm_get_call_info refused it: too many params, an unsupported type);
	 * BYADDR = it lowers but returns its value through a hidden pointer, which the shared call sequence
	 * cannot express; SHAPE = it lowers cleanly and simply differs from the call site's functype, which
	 * is the only one that would point at generic sharing. */
	WJC_ARM_ABI_INVALID, WJC_ARM_ABI_BYADDR, WJC_ARM_ABI_SHAPE,
	/* R207b: the remaining causes the old ARM2_SIG catch-all could be reached by. Bumped in the
	 * FAILURE branch only, by re-testing the cheap predicates -- the success path is byte-identical to
	 * the build this was measured clean on. (A refactor that moved these tests into a helper measured
	 * three consecutive failures -- two OOB and one wedge -- so the success path is left alone.) */
	WJC_ARM_UNJITTABLE,
	/* R215 delegate devirt. ARM = a delegate site given a guarded direct arm; THIN = its dominant
	 * target held less than DELEGATE_DEVIRT%% of observations; REFUSED = no f-slot or a divergent
	 * functype. FAST_DELEGATE_DEVIRT is the EXECUTED hit count and needs PROFILE_FAST -- read it against
	 * FAST_DELEGATE, which is the whole delegate pool it is carving out of. */
	WJC_DELEGATE_DEVIRT_ARM, WJC_DELEGATE_DEVIRT_THIN, WJC_DELEGATE_DEVIRT_REFUSED,
	WJC_FAST_DELEGATE_DEVIRT,
	/* R215b: no usable delegate record at all -- split out of THIN, which was a catch-all covering both
	 * "below the bar" and "the reader could not work here". That conflation hid a reader that could
	 * NEVER succeed (it required id_targets, always NULL at a delegate site) behind a plausible 7,582. */
	WJC_DELEGATE_DEVIRT_NOREC,
	/* Split of DELEGATE_DEVIRT_REFUSED, which merged two refusals that lead to OPPOSITE decisions and so
	 * could not be planned from: NO_FSLOT is a pure ORDERING artifact (the profile was right, the target
	 * simply had not compiled yet) and is what DEVIRT_FORCE's blocker exists to fix; SIG is a lowered
	 * signature the shared call sequence cannot express, which is permanent. Measured 4,360 merged.
	 * REFUSED is kept and still counts their sum, so old readings stay comparable. */
	WJC_DELEGATE_DEVIRT_NO_FSLOT, WJC_DELEGATE_DEVIRT_SIG,
	/* WHY a delegate dispatch fell off the direct e-thunk and into the full interp marshal. 97% of
	 * mono_wasm_jit_call_interp's cost arrives from mono_wasm_jit_call_delegate, and the whole
	 * interp-boundary complex (call_interp plus everything it re-derives) is ~5.7-6.1% of the client
	 * render thread -- but the fast path is gated on `eslot > 0 && scalar` and NOBODY KNOWS WHICH
	 * CONJUNCT FAILS. These three are bumped at the point the fallback is TAKEN, not where it is decided
	 * (R215: DelegateDevirtArm read 750 with FastDelegateDevirt 0 because the deciding block and the
	 * emitting block were different populations). Assert: NORECIPE + NOESLOT + NONSCALAR == the
	 * call_interp exits out of call_delegate. NORECIPE is the `invoke` fallthrough (no usable recipe at
	 * all); the other two are the `target` fallthrough with a recipe in hand. */
	WJC_DELEGATE_SLOW_NORECIPE, WJC_DELEGATE_SLOW_NOESLOT, WJC_DELEGATE_SLOW_NONSCALAR,
	/* GUARDED DEVIRTUALIZED INLINING (method-to-ir.c, MONO_WASM_JIT_GUARDED_INLINE). There is no
	 * inliner in this pipeline for virtual calls at all -- V8 cannot inline across modules and mono's
	 * own gate refuses every callvirt -- so this is the population that pass would reach.
	 *
	 * ASSERT: SITE == ADMITTED + PROF + SELF + SIG + CLAUSES + SIZE + OTHER.
	 *
	 * SITE      a virtual site offered to the gate (the denominator; everything below is out of this)
	 * CANDIDATE passed every PURE check with the knob OFF -- i.e. the size of the population before any
	 *           metadata is touched. This is the number to read first, from a plain STATS run, because
	 *           it costs no build: SITE minus PROF/SELF/SIG is what the pass could ever reach.
	 * ADMITTED  passed every check, so the guard and the hot arm were emitted
	 * EMITTED   inline_method actually produced a body
	 * REFUSED_LATE  inline_method refused AFTER the guard was emitted, so the hot arm is a bare branch
	 *
	 * ASSERT: ADMITTED == EMITTED + REFUSED_LATE. Anything else means "decided" and "happened" are
	 * different populations, which is the accounting failure R215 cost a round to.
	 *
	 * READ REFUSED_CLAUSES FIRST. mono_method_check_inlining rejects ANY method carrying a try/catch,
	 * and Java is EH-dense, so that bucket may be the whole story -- checking it costs nothing and
	 * comes before sweeping the size limit or blaming the profile. REFUSED_PROF is the `no_rec`
	 * population arriving here: 99.3% of profile observations land after the method is JITted, so a
	 * first compile usually has no record for a site at all. */
	WJC_GI_SITE, WJC_GI_CANDIDATE, WJC_GI_ADMITTED, WJC_GI_EMITTED,
	WJC_GI_REFUSED_PROF, WJC_GI_REFUSED_CLAUSES, WJC_GI_REFUSED_SIZE,
	WJC_GI_REFUSED_SIG, WJC_GI_REFUSED_SELF, WJC_GI_REFUSED_LATE,
	/* Everything mono_method_check_inlining refuses that is NOT clauses and NOT size -- NOINLINING,
	 * SYNCHRONIZED, gsharedvt, inline_depth > 10, or a cctor needing a generic context. It exists so the
	 * other two cannot silently absorb a cause nobody named; a counter reachable two ways is not a
	 * diagnosis, and this tree has paid for that four times. */
	WJC_GI_REFUSED_OTHER,
	/* Split of WJC_DELEGATE_SLOW_NONSCALAR, which merged three causes needing OPPOSITE fixes. VTRET is a
	 * value-type return (the 8-byte scratch result slot cannot hold it); VTARG is a by-value value-type
	 * param (the scratch holds the copy's ADDRESS where the e-thunk wants the struct INLINE, and an
	 * inline struct shifts every later offset off the flat 8-byte grid); BYREF is a byref param and
	 * nothing worse -- which is already LAYOUT-COMPATIBLE, since a non-VT type takes exactly one
	 * MINT_STACK_SLOT_SIZE slot and both sides hold the pointer. Disjoint, and they sum to NONSCALAR. */
	WJC_DELEGATE_SLOW_VTRET, WJC_DELEGATE_SLOW_VTARG, WJC_DELEGATE_SLOW_BYREF,
	/* R230: interp->JIT entries REFUSED because the snapshotted e-slot was not live on THIS worker,
	 * after the descriptor check had already passed. This is R209's torn read at the entry gate, and
	 * NON-ZERO IS THE HEALTHY READING -- it means the guard caught a race that would otherwise have
	 * call_indirect'd the jiterpreter's prefilled placeholder (mono_jiterp_placeholder_jit_call,
	 * (i32,i32,i32,i32)->void) through a (i32,i32)->void functype and trapped with
	 * `function signature mismatch` in wasm_jit_ethunk_cb, killing the worker.
	 *
	 * R209 fixed exactly this at ONE of the six callers of mono_wasm_jit_invoke_caught (the MINT_CALL
	 * gate) and the pattern was never propagated; an audit found four of six missing it, three of which
	 * also read wasm_jit_slot TWICE. A trap census over 1,027 archived logs put wasm_jit_ethunk_cb in
	 * 59 of 105 trap stacks with mono_jiterp_placeholder_jit_call the top frame in 4 -- i.e. the
	 * placeholder is demonstrably being reached. Counted per caller site is not worth six counters; one
	 * is enough to say whether the guard is live at all. Zero forever would mean it is dead code. */
	WJC_ENTRY_SLOT_STALE,
	/* Split of the WASM_JIT_ABI_MISMATCH diagnostic in wj_admit_dependencies, which used to be an
	 * UNGATED printf that called mono_method_get_full_name TWICE on the admission path -- a metadata
	 * walk, lock-free, on a worker, at every dispatch. That is R199's fault in a second location and
	 * it is what actually killed A/B runs (stack: mono_type_get_desc <- mono_signature_get_desc <-
	 * mono_method_get_full_name <- wj_admit_dependencies). The refusal is RECOVERABLE and expected, so
	 * the path is taken deliberately and the names were rolled every time. Counted here instead;
	 * printing now requires MONO_WASM_JIT_VERBOSE and takes the loader lock. */
	WJC_ABI_MISMATCH_UNREG, WJC_ABI_MISMATCH_CHUNK, WJC_ABI_MISMATCH_SIG,
	/* The dep at this f-slot is not the METHOD the caller baked, caught by identity after the 32-bit
	 * functype hash said it matched. Non-zero means the hash was letting a mismatched pair through --
	 * i.e. this counts trap-shaped bugs that used to reach `call_indirect`. See wj_admit_dependencies. */
	WJC_ABI_MISMATCH_IDENT,
	/* mono_wasm_jit_admit returned 1 for a dependency and the f-slot the CALLER BAKED is still not
	 * installed on this worker. Admission exists to make exactly this impossible -- generated
	 * call_indirect carries no liveness check -- so non-zero means a caller was about to go live over
	 * a slot holding mono_jiterp_placeholder_jit_call, which traps as `function signature mismatch`.
	 * This is the first counter on that route; every ABI_MISMATCH_* guard reads 0 when it happens. */
	WJC_ADMIT_DEP_NOT_LIVE,
	/* --- R245: every refusal route counted ------------------------------------------------------
	 *
	 * WHY THIS BLOCK EXISTS. R244 measured `admitDepNotLive` at 13.6-22.8 MILLION per run against a
	 * documented safe rate of "hundreds", with `registered` perfectly flat -- 26% of the client thread
	 * and 39% of the server tick, invisible to every gate the harness has. Diagnosing it was only
	 * possible because ONE of the refusal routes happened to have a counter. On HEAD that route is just
	 * ~55% of `alAdmit0`; the rest went through exits with no counter at all, which is the failure shape
	 * this file keeps warning about: an uncounted route does not show up as a gap, it shows up as
	 * everything else looking smaller. So: every `return 0` on the admission path gets a counter, and
	 * every exit of the SCC driver gets one, before anything about them is changed.
	 */
	/* The recursive admit of a dependency failed (wj_admit_dependencies' `if (!mono_wasm_jit_admit
	 * (dep_id)) return 0;`). The dep's own refusal reason is counted at ITS exit, so this is the
	 * propagation count, not a root cause -- read it as walk depth, not as a fault. */
	WJC_ADMIT_DEP_ADMIT0,
	/* mono_wasm_jit_admit's own early refusals, which had no counters at all. BAD_ID = desc_id out of
	 * range; NO_ENTRY = no registry entry; PERMFAIL = state 3 with permfail >= WJ_PERMFAIL_MAX, i.e. the
	 * R167 bound doing its job (non-zero here is HEALTHY, it is the alternative to a 959,405-refusal
	 * hang). STATE3 = state 3 below the bound, still refusing. */
	WJC_ADMIT_BAD_ID, WJC_ADMIT_NO_ENTRY, WJC_ADMIT_PERMFAIL, WJC_ADMIT_STATE3,
	/* WJC_ADMIT_DEP_NOT_LIVE split by CAUSE, which is the whole diagnostic value. CYCLE = the dep was
	 * admitted through mono_wasm_jit_admit's state-1 cycle break, which INSTALLS but never calls
	 * wj_mark_slot_live -- so the liveness test below it can never pass and the refusal recurs on every
	 * dispatch for the life of the process. That is R244's regression, and it is what this split exists
	 * to make visible. PENDING = the ordinary transient case, a dep not yet installed here, which the
	 * next dispatch genuinely can clear. If CYCLE dominates, the fix is structural, not a retry bound. */
	WJC_ADMIT_DEP_NOT_LIVE_CYCLE, WJC_ADMIT_DEP_NOT_LIVE_PENDING,
	/* Retired slots, BLOCKERS_TRUNCATED through SCC_COLOCATE_FAIL: the island blocker list and the SCC batcher
	 * went with the islands (R366/R367). Kept so the by-index mirrors do not shift. */
	WJC_BLOCKERS_TRUNCATED,
	WJC_SCC_ATTEMPT, WJC_SCC_OK, WJC_SCC_MEMBERS,
	WJC_SCC_BUSY, WJC_SCC_TABLE, WJC_SCC_BUDGET, WJC_SCC_ALLOC_FAIL,
	WJC_SCC_SEED_PERM, WJC_SCC_TOO_LARGE, WJC_SCC_NO_PROGRESS, WJC_SCC_ITER_CAP,
	WJC_SCC_CONDEMNED,
	WJC_SCC_RESV_BYPASS,
	WJC_SCC_COLOCATE_OK, WJC_SCC_COLOCATE_FAIL,
	/* Islands driven off the promotion queue rather than a threshold crossing. WJC_ISLAND_ATTEMPT /
	 * _COMPLETED are bumped only from wasm_jit_maybe_compile, so every island the drain started was
	 * invisible -- and the drain runs on every safe point with a FRESH budget per queue entry. */
	WJC_ISLAND_DRAIN_ATTEMPT, WJC_ISLAND_DRAIN_COMPLETED,
	/* R245 Stage 2. How often mono_wasm_jit_admit's DFS hit a back edge and took the cycle break, which
	 * installs the descriptor's closure and returns 1 WITHOUT publishing liveness -- publishing is the
	 * ancestor frame's job. Non-zero is normal and expected; it is how a dependency cycle terminates.
	 *
	 * It is counted because this path was invisible while it was the single largest cost in the tier: the
	 * dependency test that follows it demanded LIVENESS, which the cycle break cannot provide by design,
	 * so every cycle refused forever (72.5 M/run). That test now asks for INSTALLATION, which is what
	 * generated code actually needs and what this path does provide. Read against ADMIT_DEP_NOT_LIVE:
	 * cycle breaks should be common, and refusals should no longer follow from them. */
	WJC_CYCLE_BREAK_INSTALL,
	/* Retired slots (the SCC batcher, R366). */
	WJC_SCC_SPARED, WJC_SCC_PARK_NO_WAITER,
	/* IKVM replaced this method's IL and generation 1 had permanently bailed; the -1 was dropped so the
	 * emitter judges the NEW body (tiering.c). Non-zero is the healthy reading -- zero means either that
	 * no swapped method had bailed, or that the swap path never ran at all. */
	WJC_RELINK_BAIL_CLEARED,
	/* MONO_WASM_JIT_RELINK_JITTED only: generation 1 was already live, so generation 2 started untried
	 * with a fresh e/f pair rather than inheriting gen-1's. Zero with the knob on means the swap path
	 * never reaches an already-JITted method, which would make the knob inert. */
	WJC_RELINK_REFRESHED,
	/* The cycle-break arm of mono_wasm_jit_admit refused: wj_install_closure_root could not install the
	 * whole closure, so admit returns 0 AND leaves the descriptor at wj_desc_state == 1 -- the state this
	 * file calls the worst of the three to be stuck in, because nothing clears it but a generation bump.
	 * It was the one route on the admission path with no counter at all, which is precisely the shape
	 * that hid R244's 14-23M/run regression. Read it against CYCLE_BREAK_INSTALL: fail/(install+fail) is
	 * how often breaking a cycle fails outright. Non-zero is not automatically a bug -- a closure whose
	 * dep is genuinely unregistered yet is transient -- but a LARGE or GROWING value is, because each one
	 * discards the entire DFS walk and the next dispatch redoes it. */
	WJC_CYCLE_BREAK_FAIL,
	/* The instantiate path found itself already in a coop BLOCKING state and so did NOT enter a GC-safe
	 * region around `new WebAssembly.Module`. EXPECTED ZERO -- every caller is reached from the
	 * interpreter or the compile path, both GC-unsafe, and no JS export reaches them (checked). It is
	 * counted rather than asserted because the alternative is `mono_fatal_with_history ("Cannot
	 * transition thread ... from STATE_BLOCKING with DO_BLOCKING")`, i.e. a hard abort, and one MONO
	 * forced abort wedges a whole measurement batch. NON-ZERO means a new caller reaches instantiation
	 * from managed code through a P/Invoke and needs MONO_ENTER_GC_UNSAFE at its boundary the way
	 * mono_interp_replace_method_body does; the run is still correct, it merely blocks STW again.
	 * BUMPED UNGATED (like WJC_ABI_MISMATCH_*), because a guard whose counter needs --stats is not
	 * evidence of anything in a clean run, and this path should never execute at all. */
	WJC_JS_BLOCKING_SKIPPED,
	/* THE REPUBLICATION RENDEZVOUS (mini-wasm.c). Read these as an identity, not individually:
	 *   RV_COUNT      rendezvous performed; RV_MEMBERS descriptors republished across them.
	 *   RV_DRAIN      drains that did work -- ONE PER THREAD PER RENDEZVOUS in the healthy case, so
	 *                 RV_DRAIN should be about RV_COUNT x RV_DRAIN_THREADS. Much less means threads are
	 *                 not reaching a re-entry hook; much more means the epoch is being bumped by
	 *                 something other than a rendezvous.
	 *   RV_DRAIN_THREADS  DISTINCT threads that have ever drained. THIS IS THE ONE THAT PROVES THE
	 *                 MECHANISM: a rendezvous that reaches only the initiator is not a rendezvous, and a
	 *                 global drain count cannot tell the two apart.
	 *   RV_DRAIN_SLOT / _REFUSED  per-descriptor re-admissions that succeeded / were refused. REFUSED is
	 *                 not automatically a bug (admission refuses transiently and the next dispatch
	 *                 retries) but it means that worker is running on the OLD body until it does.
	 *   RV_LOG_FULL   rendezvous refused because the log is exhausted. Non-zero means the feature stopped
	 *                 working part way through a run, silently, which is the worst way for it to fail. */
	WJC_RV_COUNT, WJC_RV_MEMBERS, WJC_RV_DRAIN, WJC_RV_DRAIN_THREADS,
	WJC_RV_DRAIN_SLOT, WJC_RV_DRAIN_REFUSED, WJC_RV_LOG_FULL,
	/* RETIRED, NOT RENUMBERED -- the slot stays so archived captures keep meaning what they meant.
	 *
	 * It counted descriptors DROPPED from a rendezvous for being co-located, back when a batched member
	 * could not be republished alone. That refusal is gone: the rendezvous now expands a batched member
	 * to its whole GROUP, bumps the batch generation as well as every member's, and logs one
	 * representative. Nothing bumps this any more, so it is no longer printed -- a permanently-zero
	 * field on a live line reads as "this never happens" rather than "this check is gone". */
	WJC_RV_REFUSED_BATCHED,
	/* Drain entries where THIS thread never had the module installed, so there was nothing stale in its
	 * table and re-admitting would have been pure work -- and not even pure: a refusal on such a thread
	 * leaves the cached generation mismatched forever, because admit writes it only on success, so every
	 * later dispatch re-attempts. EXPECT THIS TO DOMINATE drain_slot + drain_refused: most workers never
	 * touch most methods. A LOW value alongside a high refused count is the regression to watch for. */
	WJC_RV_DRAIN_SKIPPED,
	/* CALLEE-RESOLUTION CENSUS (transform.c, wj_note_callee_resolution). Counted once per CALL SITE at
	 * emit time, at the one lookup every direct/devirt/delegate gate goes through.
	 *
	 * IT ANSWERS TWO QUESTIONS AT ONCE, and they are the two that decide whether re-emission is worth
	 * building:
	 *
	 * 1. CAN THE CO-LOCATOR SEE THE SITE? Its whole input is re->depset, and a depset entry exists only
	 *    where WASM_RELOC_CALL was emitted, i.e. only where this lookup returned > 0. HAS_FSLOT over the
	 *    total is therefore literally the co-locator's field of view. Everything else emits AOT or an
	 *    indirect form, and those relocations carry no `sym` -- so the callee has no NAME any later pass
	 *    could use. The co-locator cannot group, and cannot force-compile, what it cannot name.
	 *
	 * 2. HOW MUCH OF THE BLIND HALF IS REACHABLE? UNTRIED is the population a re-emission would convert:
	 *    the callee exists and simply had not been compiled when the caller was. BAILED never will be
	 *    (wasm_jit_slot == -1 is terminal). NOIM has never run at all. Keeping the three apart is the
	 *    point -- a single "no f-slot" counter conflates a ceiling with a floor.
	 *
	 * SITE-COUNTED, NOT EXECUTION-WEIGHTED, and this file's own rule applies: weight a bucket by
	 * execution before spending on it. Pair it with the colocate-reach triple, which is miss-weighted. */
	WJC_CALLEE_HAS_FSLOT, WJC_CALLEE_NO_FSLOT_UNTRIED,
	WJC_CALLEE_NO_FSLOT_BAILED, WJC_CALLEE_NO_FSLOT_NOIM,
	/* mono_interp_peek_imethod found a memory manager whose interp_code_hash was not initialised yet --
	 * i.e. it landed in the publish-before-initialise window that aborts the process when an unguarded
	 * mono_interp_get_imethod wins the same race (`mono-internal-hash.c:47`). Bumped UNGATED: it is the
	 * positive control for that fix, and a race detector behind --stats is not evidence in a clean run.
	 * MUST BE 0. Non-zero means the window is live regardless of whether anything crashed this run. */
	WJC_JITMM_UNINIT,
	/* RE-EMISSION (interp.c). Read as an identity, and the identity is the point: R179's seven bugs were
	 * every one of them caught by two counters DISAGREEING about the same event, never by a number looking
	 * wrong -- `REEMIT_DONE=31` while `REEMIT_SITE=0`, `done=4` while `queued=35,617,130`.
	 *
	 *   QUEUED    triggers accepted.  DEDUP  triggers dropped because the method was already pending.
	 *   QFULL     dropped on a full queue -- a missed optimisation, not an error, but a LARGE value means
	 *             the drain is not keeping up and the trigger is mis-tuned.
	 *   DRAINED   popped.  Must equal COMPILED + BUSY + FAILED + REFUSED + GONE.
	 *   BUSY      lost the compile CAS; RE-QUEUED, not consumed (R179 bug #2 was treating it as a verdict).
	 *   REFUSED   interpreter-tiered/retired, or the slot pin declined.
	 *   GONE      the descriptor no longer resolves to a live JITted imethod.
	 *   COMPILED  new bytes produced.
	 *   REPUBLISHED  ... and every worker forced to re-instantiate them. THIS IS THE ONE THAT MATTERS:
	 *             COMPILED without REPUBLISHED is a re-emission nobody executes, which is precisely what a
	 *             re-emission subsystem without a rendezvous silently is.
	 *   SITE      devirt sites emitted DURING a re-emission. Zero here with COMPILED non-zero means the
	 *             recompile is not reaching the emitter -- R179 bug #1's signature. */
	WJC_REEMIT_QUEUED, WJC_REEMIT_DEDUP, WJC_REEMIT_QFULL, WJC_REEMIT_DRAINED,
	WJC_REEMIT_BUSY, WJC_REEMIT_REFUSED, WJC_REEMIT_GONE, WJC_REEMIT_FAILED,
	WJC_REEMIT_COMPILED, WJC_REEMIT_REPUBLISHED, WJC_REEMIT_NO_RENDEZVOUS, WJC_REEMIT_SITE,
	/* Drains refused because MONO_WASM_JIT_REEMIT_MAX was reached. Non-zero means the run wanted more
	 * re-emission than the ceiling allows -- informative, not an error. */
	WJC_REEMIT_CAPPED,
	/* A re-emit came back on a DIFFERENT f-slot than it went in on, i.e. the slot pin did not hold and it
	 * landed on a fresh pair. MUST BE 0: that is R170's failure (REEMIT_SLOT_MOVED x20), it leaks two
	 * table entries per occurrence, and it republishes a slot nobody calls -- all invisible in every other
	 * counter. WJC_FSLOT_REREGISTER cannot answer this; rebatch bumps it too. */
	WJC_REEMIT_SLOT_MOVED,
	/* The devirt census restricted to RE-EMITTED bodies (WJ_REEMIT_SCOPED). emitted/SITE is the coverage
	 * of re-emitted code, to be read against the run-wide devirt line; NO_FSLOT is the bucket R259 showed
	 * gates both `poly` and coverage, and is the one re-emission is supposed to drain. */
	WJC_REEMIT_EMITTED, WJC_REEMIT_NO_REC, WJC_REEMIT_NO_FSLOT,
	/* Triggers suppressed because this METHOD has already been re-emitted once.
	 *
	 * The one-shot that shipped first was per SITE (WjVcallSite.reemit_noted), which does NOT bound a
	 * method: re-emitting it builds a new body whose IC sites are FRESH, with misses = 0 and
	 * reemit_noted = 0, so the same method re-triggers itself. At MISSES=64 that loop is slow enough to
	 * hide behind the MAX cap; at a low threshold -- which is what reaching most of the population needs,
	 * because a monomorphic site misses ONCE and then hits forever -- it is unbounded. This counter is
	 * how that is seen rather than assumed: it must be LARGE next to WJC_REEMIT_QUEUED, since every hot
	 * site in an already-re-emitted method lands here. Zero means the flag is not being set. */
	WJC_REEMIT_METHOD_DONE,
	/* ADMISSION DFS RECURSION DEPTH. mono_wasm_jit_admit and wj_admit_dependencies are mutually
	 * recursive with no depth parameter anywhere, and the walk descends one level per dependency EDGE.
	 *
	 * This is the actual ceiling on re-emission, and it took a captured stack to see it: every re-emission
	 * arm that wedged at boot died with `RangeError: Maximum call stack size exceeded` whose stack is 64
	 * frames of this pair alternating, and every arm that passed has ZERO such faults (8 runs, perfect
	 * discrimination, 2026-09-17). The mechanism is not incidental -- re-emission works by turning
	 * INDIRECT calls into DIRECT ones, a direct call is exactly what puts a callee in the caller's
	 * depset, so a re-emission that succeeds is one that LENGTHENS admission chains. The feature's
	 * success and this overflow are the same event.
	 *
	 * DEPTH_MAX is a high-water mark, not a count: it is written only when it grows. Read it against a
	 * re-emission-off control -- the claim is that it grows with re-emissions, and a flat high-water
	 * would refute the whole diagnosis. DEPTH_OVER counts walks past WJ_ADMIT_DEPTH_WARN, i.e. how close
	 * to the cliff a passing run runs. */
	WJC_ADMIT_DEPTH_MAX, WJC_ADMIT_DEPTH_OVER,
	/* The generation reset declined to clear a VISITING (state 1) descriptor -- i.e. this is the race
	 * that turned re-emission into a boot wedge, caught.
	 *
	 * R262: the reset ran BEFORE the state==1 cycle-break test and cleared it, and on the non-batch path
	 * wj_desc_generation is not written before the dependency walk, so the mismatch persists for the
	 * whole walk: every re-entry through a cycle edge cleared the marker, re-marked visiting and
	 * descended again. Unbounded recursion, and a V8 `Maximum call stack size exceeded` at the bottom of
	 * it. Only a rendezvous bumps a live descriptor's generation while walks are in flight, which is why
	 * only re-emission ever hit it.
	 *
	 * Per CLAUDE.md, NON-ZERO IS THE HEALTHY READING: a zero here means the guard is dead code, not that
	 * the race is impossible -- and the measured control (re-emission off) reaches admission depth 22,
	 * so anything approaching the stack limit is this bug and not a deep graph. */
	WJC_ADMIT_GEN_RESET_VISITING,
	/* Re-emissions abandoned after WJ_REEMIT_BUSY_MAX losses of the compile CAS. Read it against
	 * WJC_REEMIT_BUSY: the pair says whether contention is costing candidates (giveup large) or merely
	 * costing retries (busy large, giveup small). The budget exists because an unbounded BUSY re-enqueue
	 * keeps the drain alive for the whole run, which is what pushed re-emission into the in-game window
	 * and produced an 18.8M admission-refusal storm. */
	WJC_REEMIT_BUSY_GIVEUP,
	/* A state-1 (VISITING) marker found with wj_admit_depth == 0, i.e. LEAKED by a walk that is no longer
	 * running, and cleared. NON-ZERO IS THE HEALTHY READING, and non-zero also proves the leak is real.
	 *
	 * State 1 only means anything while this thread is inside the DFS. A leaked marker is worse than
	 * useless: every later admit takes the state==1 arm, which is the INSTALL-ONLY cycle break, so it
	 * returns 1 ("admitted") without ever admitting the closure -- and generated code call_indirects a
	 * dependency f-slot with no liveness check, so the slot still holds
	 * mono_jiterp_placeholder_jit_call and the call traps with `function signature mismatch`.
	 *
	 * MEASURED 0 (2026-09-17) on the configuration that DOES trap, with the trap still firing. So this
	 * was built as R264 wall 3's cause and is NOT: no marker is being leaked. The guard is kept because
	 * a leak here would be silent and permanent, and because the `fail:` arm below genuinely can produce
	 * one -- but per this file's rule the zero is recorded rather than the guard being read as a fix.
	 * Do not cite this counter as an explanation of the signature-mismatch trap.
	 *
	 * The known leak is the `fail:` label, whose batch arm clears a sibling only when
	 * wj_desc_generation[sibling] == batch->generation -- so a rebatch landing mid-walk leaves every
	 * sibling of a FAILED walk marked forever. Fixed there too; this counter is the backstop for any
	 * leak path not yet found, which is why it is depth-based rather than specific to that one. */
	WJC_ADMIT_STALE_VISITING,
	/* `re->batch` changed identity WHILE this admission was running, i.e. the group was re-framed under
	 * the walk. NON-ZERO IS THE HEALTHY READING and is also the proof the race is real.
	 *
	 * mono_wasm_jit_admit snapshots `batch = re->batch` and then used to re-read `re->batch` about
	 * fifteen more times, including five reads inside ONE instantiate call
	 * (`re->batch->e, ->f, ->n, ->bytes, ->len`). mono_wasm_jit_rebatch publishes a FRESH WjBatchDesc, so
	 * each descriptor is internally consistent but the POINTER moves -- and a re-frame landing between
	 * those reads pairs one module's bytes with another's slot list, writing exports into slots that
	 * belong to different methods. The sibling publish loop had the same split: it validated membership
	 * against the snapshot and then marked liveness from `re->batch`, so it could announce slots live
	 * that this call never instantiated. Either route ends at a call_indirect to a slot holding the
	 * wrong arity -- `function signature mismatch`, intermittently, and more often the more rebatching
	 * happens. CLAUDE.md names this exactly: "a stale pointer frames the wrong function." */
	WJC_ADMIT_BATCH_SWAPPED,
	/* The rendezvous drain found a descriptor at STATE 2 ("admitted, dispatchable") whose e/f slots this
	 * worker has NOT installed, and downgraded it. NON-ZERO IS THE HEALTHY READING and is also the proof
	 * this path is reachable.
	 *
	 * The drain's skip arm records the new generation so the descriptor stops taking admit's
	 * generation-mismatch arm on every dispatch, and its comment asserted "State is left untouched:
	 * 0 means untried". Nothing guarantees state is 0. If it is 2, then state 2 + the CURRENT generation
	 * is exactly what admit's fast path believes -- and that fast path trusts the STATE, not the liveness
	 * bitmap -- so it returns 1 for a descriptor whose slots were never written here, the caller goes
	 * live, and its call_indirect lands on mono_jiterp_placeholder_jit_call. That is R165's finding
	 * verbatim ("state 2 with neither slot installed, holding the prefill"), recreated by code added in
	 * Phase 2 rather than by the path R165 fixed.
	 *
	 * Downgrade 2 -> 0 only. State 3 is left alone so the permfail bound survives; clearing it would
	 * route a permanent condition back into the retry path, which is R244. */
	WJC_RV_DRAIN_STALE2,
	/* The drain cleared a descriptor's installed bits and had to force its state off 2 so that
	 * mono_wasm_jit_admit would actually re-instantiate instead of taking its fast path.
	 *
	 * THE BUG THIS FIXES, and it is the one the R267 bisection pointed at. The drain clears the installed
	 * bits and then calls admit to rebuild -- but admit's fast path is
	 *     if (state == 2 && wj_desc_generation [desc] == re->generation) return 1;
	 * which trusts the STATE and never looks at the bitmap. The rendezvous bumps re->generation under the
	 * stop, so normally this thread's cached generation mismatches and the fast path is skipped. But
	 * after the world restarts, a thread can LAZILY ADMIT the descriptor -- writing the new generation and
	 * installing the slots -- before it reaches a coop re-entry point that runs the drain. The drain then
	 * clears the bits, calls admit, gets an immediate fast-path success, and THE SLOTS ARE NEVER
	 * REINSTALLED. The next call_indirect through that f-slot hits mono_jiterp_placeholder_jit_call:
	 * `function signature mismatch`, intermittently, scaling with rendezvous count.
	 *
	 * NON-ZERO IS THE HEALTHY READING -- it counts the window being closed, not an error. */
	WJC_RV_DRAIN_REFRESH_FORCED,
	/* MONO_WASM_JIT_SINGLE_WRITER: descriptors published by the single writer, and closure installs it
	 * refused. Read PUBLISHED against WJC_REGISTERED to confirm the path is actually carrying admission,
	 * and INSTALL_FAIL against WJC_ADMIT_FAIL_RETRY -- a refusal here is transient by construction
	 * (budget exhaustion or a failed instantiate), so it must not grow without bound. */
	WJC_SW_PUBLISHED, WJC_SW_INSTALL_FAIL,
	/* Closure verification refused the admission: some member of the installed closure has a dependency
	 * whose ABI/identity does not agree, or whose baked slot is not live here. The recursive admit
	 * expressed this as a refusal propagating up from the offending level; the single writer has to check
	 * it explicitly, and NOT doing so is what regressed a clean config twice (see wj_make_callable). */
	WJC_SW_VERIFY_FAIL,
	/* High-water closure size seen by the install walk, PER PASS -- and only per pass since R269 fixed
	 * the reset. `wj_clo_n` was reset only in wj_make_callable (MONO_WASM_JIT_SINGLE_WRITER, ships 0),
	 * so on the shipped path this was a CUMULATIVE APPEND COUNT: it read 65,576 against a registry of
	 * 32,175, and it reached any constant almost immediately. R267 addendum 12's "sitting AT the cap of
	 * 4096, i.e. real closures are being truncated" was therefore read off a counter that could not
	 * measure a closure size. The truncation question is answered by WJC_INSTALL_BUDGET_OUT alone,
	 * which must stay 0. */
	WJC_SW_CLOSURE_MAX,
	/* MONO_WASM_JIT_SWEEP: TIME-OF-USE verification. Re-checks descriptors this thread has ALREADY
	 * published (state 2, generation current) and reports any whose own slots died or whose dependency
	 * set no longer verifies.
	 *
	 * Why this exists: the single writer verifies the full closure at PUBLISH time and
	 * `sw_verify_fail` is 0 on every run -- yet re-emission still traps 4/4. So the trap is not an
	 * illegitimate publication; something invalidates a LEGITIMATE one afterwards, and no amount of
	 * publish-time checking can see that. Re-emission is the only thing that replaces a live descriptor's
	 * BYTES and its DEPSET, and a new body has different direct calls, i.e. a different required closure
	 * than the one verified when its callers were admitted.
	 *
	 * DEP_BAD non-zero under re-emission and zero without it confirms that reading. SELF_DEAD separates
	 * "my own e/f slot stopped being live" from "a dependency did". */
	WJC_SWEEP_RUNS, WJC_SWEEP_SELF_DEAD, WJC_SWEEP_DEP_BAD,
	/* Descriptors that lost their f-slot to a re-registration of the same method, and admissions refused
	 * because the descriptor is one of them. ORPHANED tracks re-emission's rate; ADMIT_ORPHANED is the
	 * catch -- non-zero means workers really were holding stale admissions of a slot-less descriptor,
	 * which is the trap in R267 addendum 10. */
	WJC_FSLOT_ORPHANED, WJC_ADMIT_ORPHANED,
	/* Re-registrations that UPDATED the existing descriptor in place instead of minting a new one. This
	 * should carry essentially all of re-emission's republications, and WJC_FSLOT_ORPHANED should fall to
	 * ~0 as a result -- an orphan only remains possible for a batched descriptor, which this path
	 * declines. Read the pair together: REUSED high with ORPHANED ~0 is the fix working. */
	WJC_FSLOT_REUSED,
	/* An admission finished against a generation that had MOVED since its dependency walk began -- i.e. the
	 * method was re-emitted mid-walk and `depset`/`generation` were replaced under it. Publishing the fresh
	 * generation there would mark this worker current with a dependency it never installed; the walk's own
	 * recorded generation is published instead, so the worker is simply stale and re-admits. NON-ZERO IS
	 * EXPECTED under re-emission and is the window being closed, not an error. */
	WJC_ADMIT_GEN_MOVED_MIDWALK,
	/* A mandatory (semantic) replacement that the broker gave up on, and the transient publication
	 * failures that lead there.
	 *
	 * REFRAME_FAIL: the body compiled and registered, but re-framing its co-location GROUP failed, so the
	 * group still carries the OLD body. The replacement is retried on the same bounded budget as a lost
	 * compile CAS; when that runs out REQUIRED_GIVEUP fires, `wasm_jit_reemit_required` is cleared and the
	 * old body stays live. Unbounded retry here is what leaked a module blob per attempt and recompiled
	 * the same method at every compile safepoint until the wasm heap was exhausted before Minecraft
	 * finished initialising -- the exact failure the BLOCKED arm's comment in interp.c was written to
	 * prevent, on the one path it did not cover.
	 *
	 * REQUIRED_DROPPED: a mandatory replacement refused for a condition that CANNOT clear (no f-slot, the
	 * interpreter tiered the method, the emitter bailed permanently). The flag is cleared so the method
	 * stops being rerouted into the broker forever by wj_waiter_wake.
	 *
	 * BOTH MUST BE READ, AND A NON-ZERO GIVEUP IS A CORRECTNESS STATEMENT, NOT A MISSED OPTIMISATION, THE
	 * DAY A TRUE DETOUR USES THIS PATH: for an IKVM generation-2 swap the old body is merely slower, but a
	 * MonoMod-style detour that never lands is a silently un-applied patch. Whoever wires that up must
	 * turn these into a reported failure rather than a counter. */
	WJC_REEMIT_REFRAME_FAIL, WJC_REEMIT_REQUIRED_GIVEUP, WJC_REEMIT_REQUIRED_DROPPED,
	/* Superseded registry payloads (module bytes, depsets, relocatable bodies, batch descriptors) retired
	 * behind the reader grace period, and what became of them. See the long note above wj_retire_payload.
	 *
	 * RETIRED vs FREED is the leak reading: they should track. DEPTH_MAX is the one that matters, because
	 * reclamation is blocked while ANY worker is inside the bracket and one parked inside a JS module
	 * compile or a JSPI suspension defers everything -- a burst by design, unbounded in principle, so the
	 * high-water mark is read rather than asserted. RECLAIMS is the liveness signal: 0 with RETIRED large
	 * means the bracket is never observed empty and the scheme has degenerated into the leak it replaced. */
	WJC_RETIRED, WJC_RETIRE_FREED, WJC_RETIRE_RECLAIMS, WJC_RETIRE_DEPTH_MAX,
	/* Re-admissions a worker owes after a drain refused one, and how the carry list behaved. RETRY_OK is
	 * the healthy reading and should track DRAIN_REFUSED; RETRY_MAXN is the high-water occupancy; a
	 * NON-ZERO RETRY_FULL means the list overflowed and that drain fell back to pinning the epoch, which
	 * puts a closure walk on every loop back-edge until it clears -- if it is ever non-zero, raise
	 * WJ_RV_RETRY_MAX rather than ignoring it. */
	WJC_RV_RETRY_OK, WJC_RV_RETRY_FULL, WJC_RV_RETRY_MAXN,
	/* Workers that have claimed a safepoint action word (high-water), and claims that found the slab
	 * full. SLOTS says how close WJ_WORKER_MAX is to binding; a non-zero SLOTS_FULL means some worker is
	 * permanently taking the out-of-line safepoint helper -- correct, but slow, and invisible otherwise. */
	WJC_WORKER_SLOTS, WJC_WORKER_SLOTS_FULL,
	/* The install walk's runaway guard fired. MUST BE ZERO. It is sized from the registry size and the
	 * stamp visits each descriptor once, so reaching it means the dependency graph is not what the walk
	 * assumes -- not that a closure was legitimately large. */
	WJC_INSTALL_BUDGET_OUT,
	/* A rebatch request that was complete for its group but carried OTHER descriptors too -- a real merge,
	 * refused because MONO_WASM_JIT_COLOCATE_MERGE ships 0. Split out from WJC_COLOCATE_MERGE_SPLIT
	 * because the two used to be one early return that also swallowed the PURE RE-FRAME case, which is
	 * neither a merge nor a split and is what re-emission needs (see mono_wasm_jit_rebatch). */
	WJC_COLOCATE_MERGE_OFF,
	/* Group re-frames that SUCCEEDED, the denominator WJC_REEMIT_REFRAME_FAIL never had. Before the pure
	 * re-frame was separated from a merge this was structurally 0 in the shipped configuration and the
	 * failure count alone could not say so. */
	WJC_REFRAME_OK,
	/* Drain calls that returned immediately because the publication batch was full and the flush was
	 * still rate-limited. This is the WAIT, and it must be cheap: the entry stays queued and nothing is
	 * dequeued. Read it against WJC_REEMIT_DRAINED -- when the same condition was handled by popping and
	 * re-pushing instead, DRAINED read 2.8 MILLION against 847 real outcomes. */
	WJC_REEMIT_BATCH_WAIT,
	/* A closure install walk finished with a GC suspend ALREADY REQUESTED -- i.e. the walk was holding a
	 * coop-suspend off. The walk has no safepoint poll, is reached FROM mono_wasm_jit_safepoint_poll via
	 * the rendezvous drain (so the thread has just satisfied its GC check for that pass), and since R268
	 * it may visit up to wj_reg_n descriptors while instantiating modules. R269 observed a void in-game
	 * window whose two busy threads were `mono_threads_wait_pending_operations` at 99.98% and a spin in
	 * admit/admit_dependencies/rendezvous_drain, which is what that would look like. NON-ZERO means the
	 * mechanism is real; 0 across a faulting run exonerates it. */
	WJC_ADMIT_WALK_GC_PENDING,
	/* A wasm-JIT PUBLISH found no InterpMethod for the method it was about to publish into, so the
	 * publication was skipped instead of dereferencing NULL. Two distinct conditions used to reach the
	 * same two lines unguarded (interp.c, the compile_publish and SCC publish paths): an uninitialised
	 * interp_code_hash aborts the process at `mono-internal-hash.c:47`, and a present-but-empty lookup
	 * dereferences NULL and reports `memory access out of bounds` -- which are precisely the two fault
	 * signatures R269 kept hitting. Skipping is recoverable: the publish reports BUSY, so the method
	 * keeps its accrued hotness and re-attempts a stride later rather than being lost.
	 *
	 * NOT DISJOINT FROM WJC_JITMM_UNINIT -- this is the TOTAL and that one is the uninitialised-hash
	 * SUBSET of it, so `publish_no_imethod - jitmm_uninit` is the plain-miss (tiering replaced the
	 * InterpMethod) count. Stated explicitly because this tree has four recorded instances of a share
	 * computed over counters nobody had checked for overlap. NON-ZERO is expected to be rare; a large
	 * value means the publish path is routinely racing tiering.c and deserves its own round. */
	WJC_PUBLISH_NO_IMETHOD,
	/* A retained `MonoMethod *` failed the plausibility probe (misaligned or outside the heap) at
	 * mono_interp_peek_imethod, so the lookup was refused instead of dereferencing it. Split by which
	 * pointer failed: the METHOD itself, or the MonoJitMemoryManager derived from it -- the 2026-09-19
	 * crash was the SECOND (the four derefs in jit_mm_for_method all succeeded and produced garbage,
	 * which mono_mem_manager_lock then CASed).
	 *
	 * NON-ZERO IS THE HEALTHY READING once the class is live: it means a dangling dereference was caught
	 * rather than executed. A permanent 0 means either the class is quiet or the probe is dead code --
	 * and R151's version WAS dead code for its entire life, never spliced into a call path. */
	WJC_DANGLING_METHOD, WJC_DANGLING_JITMM,
	/* Admission read `re->batch` as NULL and then `re->bytes` as the SHARED module -- a stale snapshot,
	 * refused as TRANSIENT instead of instantiating a multi-export module through the single-method path.
	 * Before this existed the same event was ~23 PERMANENT admission failures per run. */
	WJC_ADMIT_BATCH_RACED,
	/* A retained raw MonoMethod* that is no longer usable, CAUGHT AT THE POINT OF USE and attributed to
	 * the retainer that produced it. Split by site because "it happened" is not root cause: the registry,
	 * the call profile and the synchronized-wrapper canon table all retain for the process lifetime, and
	 * only a per-site count says which one hands out the dead pointer. See R273 / DANGLING-MONOMETHOD.md.
	 * NON-ZERO IS THE HEALTHY READING once the fault exists -- each catch is a `loader.c:1826` assert or an
	 * OOB in mono_signature_to_name that did NOT happen. All zero means either the fault is absent or the
	 * guards are not on the path, and `badmeth_seen` distinguishes those. */
	WJC_BADMETH_SEEN,        /* validations performed -- a zero here means the guards never ran */
	WJC_BADMETH_PEEK,        /* mono_interp_peek_imethod */
	WJC_BADMETH_REGISTRY,    /* WjRegEntry body/logical method */
	WJC_BADMETH_PROFILE,     /* WjProfSite id_targets[] */
	WJC_BADMETH_CANON,       /* wj_sync_inner_canon value */

	/* A header whose EH clause table does not describe its own body. Reached only through
	 * mono_interp_replace_method_body's whole-header swap (IKVM generation 2), which is why it is a
	 * wasm-JIT counter and not a mono one. Non-zero means the guard in mark_bb_in_region caught a
	 * malformed body that USED TO ABORT THE PROCESS -- so non-zero is the healthy reading only in the
	 * sense that the run survived; the body itself is still a bug in whoever emitted it. */
	WJC_BAD_EH_CLAUSE,

	/* SPIN CENSUS. mono_thread_info_yield() is a NO-OP on wasm -- mono_threads_platform_yield() is
	 * `{ return TRUE; }` (mono-threads-wasm.c:169-172) -- so every CAS retry loop in this backend was an
	 * unbounded, unyielding, UNCOUNTED busy spin. Two threads hammering one lock word is also the only
	 * thing that puts two DIFFERENT threads on the SAME ~22 bytes of generated code, which is exactly
	 * what R275 measured on the world-load hang (5 addresses, 0x6b3f-0x6b55, both threads, wchan=0,
	 * uninterruptible by Debugger.pause).
	 *
	 * THESE COUNTERS EXIST TO SETTLE THAT, and the reading that matters comes from a HANGING run:
	 *   spin_max large   -> contention on this lock is real and worth attacking.
	 *   spin_max small   -> the spin is NOT the hang, and the next instrument is a symbolised profile.
	 * A high-water rather than a total, because a total cannot distinguish "briefly contended a million
	 * times" from "one thread stuck". Both are recorded: _SPINS is the total, _SPIN_MAX the worst single
	 * acquisition. */
	WJC_RETIRE_SPINS, WJC_RETIRE_SPIN_MAX, WJC_RETIRE_TRYLOCK_MISS,
	WJC_REEMIT_Q_SPINS, WJC_REEMIT_Q_SPIN_MAX,

	/* THE CARRY LIST'S DROP CENSUS -- see wj_rv_retry in mini-wasm-publish.inc.
	 *
	 * An entry used to leave the list ONLY on admit success, while mono_wasm_jit_admit has refusals that
	 * are permanent by construction. One such entry keeps wj_rv_retry_n > 0 forever, which re-raises
	 * WJ_ACT_PUB at every safepoint, which makes EVERY LOOP BACK-EDGE in every JITted method on that
	 * worker run a full drain. `registered` then stays flat while the thread burns a core -- the exact
	 * signature R269/R275 recorded and could not explain.
	 *
	 * RETRY_DROPPED is the count of entries given up on after WJ_RV_RETRY_ATTEMPTS. Non-zero is the
	 * HEALTHY reading: it is the number of workers that did NOT get stuck. A counter stuck at 0 means
	 * either the condition never arises or the drop path is dead code -- and RETRY_ATTEMPT_MAX
	 * distinguishes those, because it rises as soon as anything retries at all. */
	WJC_RV_RETRY_DROPPED, WJC_RV_RETRY_ATTEMPT_MAX,

	/* HOW OFTEN A WORKER LEAVES THE SAFEPOINT HELPER WITH WORK STILL OWED, consecutively.
	 *
	 * `s.g` is cleared by RE-DERIVING its two conditions, so a condition that is permanently true makes
	 * the emitted `global.get 9; i32.load; if` fire on every loop back-edge forever. All three known ways
	 * that happens are documented at their sites (a stuck carry list, the `pin` path leaving the epoch
	 * behind, and the overflow word), and none of them was observable from outside: the tier simply looks
	 * frozen while a core is busy.
	 *
	 * A HIGH-WATER OF CONSECUTIVE RE-RAISES IS THE DIRECT TEST, and it is cheap because it is per-worker
	 * and unsynchronised. In health it stays in single digits -- a publication is adopted within a poll
	 * or two. In the pathology it grows without bound. Read it beside rv_retry/worker_slots to say WHICH
	 * of the three it is. */
	WJC_ACT_PUB_RERAISE_MAX,

	/* THE PERMANENTLY-UN-JITTABLE POINTER SET (mono_wasm_jit_note_perm_unjittable).
	 *
	 * `adds` is the LIVENESS CHECK, and reading the rest without it is meaningless: a zero `adds` means
	 * the set is never populated -- i.e. the insert is not at the site where the fact becomes true --
	 * and then `hits` reading 0 says nothing at all. That is the likely implementation error for this
	 * shape, so it is the first thing to check.
	 *
	 * `hits` is how many predicate answers came from POINTER COMPARISON instead of from four
	 * dereferences of a possibly-freed MonoMethod*. It is not a fault count: most hits are perfectly
	 * live methods. It is the size of the exposure that no longer exists.
	 *
	 * `full` must stay 0. Non-zero means the table stopped answering and the predicate silently fell
	 * back to "not permanently un-JITtable" -- a codegen difference, not a crash, which is exactly the
	 * kind of thing that reads as a mysterious regression. */
	WJC_PERM_SET_ADDS, WJC_PERM_SET_HITS, WJC_PERM_SET_FULL, WJC_PERM_SET_STALE,

	/* Emit-time predicate calls whose callee was SUBSTITUTED by wj_canonical_callee, i.e. answered about
	 * a pointer out of the process-lifetime wj_sync_inner_canon table rather than about the IR's own
	 * call->method. DANGLING-MONOMETHOD.md ranks that table as the largest retention window in the
	 * backend and names this the discriminating measurement: it says whether fixing the PREDICATE leaves
	 * the real retainer in place, and it is far cheaper than the purge-on-teardown fix it would justify. */
	WJC_CANON_SUBSTITUTED,

	/* Dependencies that ONLY the relocations knew about -- added to a standalone method's depset by the
	 * union in the framing block, on top of what the emit-time recorder had. The emit-time set is
	 * recorded by hand at the sites that call wj_result_add_direct_dep, so it is only as complete as that
	 * list; the relocs are what the bytes are built from. ZERO means the hand-recording was already
	 * complete and the union is inert. NON-ZERO is the size of the hole R285 caught one instance of: a
	 * baked f-slot no descriptor declared, hence never installed, hence a placeholder trap. */
	WJC_DEPS_FROM_RELOCS,
	/* Methods refused before emission because params + declared locals would exceed V8's
	 * kV8MaxWasmFunctionLocals (v8/src/wasm/wasm-limits.h:50). Non-zero is expected and small: 1.16.1's
	 * BlockStateFlattening:.cctor is the known member. See WJC_INVALID_PERM for why this is checked
	 * HERE and not left to instantiation. */
	WJC_LOCALS_OVERFLOW,
	/* Instantiation failures refused PERMANENTLY because the error was a CompileError, i.e. the bytes
	 * are invalid and re-emitting produces the same bytes. The rest of the WJC_INVALID population stays
	 * retriable (a per-worker OOM is transient and re-emitting on an unloaded worker succeeds).
	 * This split exists because routing a deterministic failure into the retry path is unbounded: one
	 * method recompiled every dispatch cost 22.4 s of boot. See R289. */
	WJC_INVALID_PERM,
	/* MONO_WASM_JIT_LAZY_COLD (R290). LAZY_COLD = methods compiled with a cold-only lazy ref frame (every GC
	 * point a raise or a conditional poll); LAZY_COLD_NEW = those the LAZY_GCP gate alone would have left
	 * EAGER, i.e. the knob's actual reach. Zero with the knob on means the gate never fires. */
	WJC_LAZY_COLD,
	WJC_LAZY_COLD_NEW,
	/* MONO_WASM_JIT_VCALL_MEMO (R290): per-thread (site, vtable) -> f-slot memo on the vcall MISS path.
	 * HIT = resolves served by it; FILL = entries written after a successful admission; STALE = keyed hits
	 * that failed re-validation (tier-up, re-slot, or not admissible) and fell through. */
	WJC_VMEMO_HIT,
	WJC_VMEMO_FILL,
	WJC_VMEMO_STALE,
	/* MONO_WASM_JIT_INLINE_LEAF (R290): callees the size gate admitted ONLY because the leaf limit applied.
	 * Counted at the gate, so it is an upper bound on inlines -- inline_method can still refuse. */
	WJC_INLINE_LEAF_ADMIT,
	/* The freed-method set (MONO_WASM_JIT_DEADSET, R290). ADDS = methods recorded at interp_free_method
	 * (0 => the hook never fires and every HIT below is meaningless); FULL must stay 0; REVIVED = marks
	 * cleared because an InterpMethod was created at a recycled address. HIT_* = retained pointers a
	 * consumer was about to dereference that were known freed -- each one a crash that did not happen. */
	WJC_DEADSET_ADDS,
	WJC_DEADSET_FULL,
	WJC_DEADSET_REVIVED,
	WJC_DEAD_HIT_ISLAND,
	WJC_DEAD_HIT_PROF,
	WJC_DEAD_HIT_CANON,
	/* Fixed-index relocs (HELPER / AOT / HELPER_CI) whose index turned out to be a JIT slot (R291): DEP = an
	 * f-slot, now declared as a dependency; ESLOT = an e-slot, which cannot be. Either non-zero names the
	 * R285 population; WASM_JIT_FIXED_JIT_SLOT (verbose) names the reloc kind. */
	WJC_FIXED_JIT_DEP,
	WJC_FIXED_JIT_ESLOT,
	/* R292, ungated, at every JITted-EH-method entry: STACK_LOW = entries with < 32 KB of linear-memory
	 * stack left; ISLAND_TLS_BAD = entries whose island thread-locals were already implausible. Either
	 * non-zero before the recurring island-store OOB names its cause. */
	WJC_STACK_LOW,
	WJC_ISLAND_TLS_BAD,
	/* R293, ungated: a JS worker taken up by a NEW pthread while still holding code built under the previous
	 * one (mono_wasm_jit_worker_reuse). EVENTS = such take-ups; TRAMP / ADAPTER = interp-entry trampolines /
	 * guard-free adapters found installed; SLOT = e/f slots found installed. All four are the hazard's
	 * population, counted whether or not MONO_WASM_JIT_REUSE_RESET then reverts it. */
	WJC_REUSE_EVENTS,
	WJC_REUSE_TRAMP,
	WJC_REUSE_ADAPTER,
	WJC_REUSE_SLOT,
	/* ...and the positive control: helpers handed a scratch buffer that belongs to another pthread, i.e. code
	 * instantiated under a previous pthread executing on this one (wj_note_foreign_scratch). */
	WJC_REUSE_FOREIGN_SCRATCH,
	/* LAZY_COLD=2 (R294): cold frames that needed the doomed-block discount, i.e. had GC points only on
	 * throw-only paths besides their raises and polls. The reach of level 2 over level 1. */
	WJC_LAZY_COLD_DOOMED,
	/* MONO_WASM_JIT_FRAME_ZERO=0 (R294): ref-region bytes the eager prologue no longer zeroes, summed per
	 * method emitted. 0 with the knob off = the arm never ran. */
	WJC_FRAME_ZERO_SKIPPED,
	/* R293b, ungated: native->managed entries through the generic C interp_entry for a method whose JIT
	 * f-slot is live on the entering thread (a trampoline could have forwarded it). */
	WJC_ENTRY_SLOW_LIVE,
	/* LAZY_COLD=2 reach, split (R294): methods with GC points only reachable on throw paths (DOOMED_SEEN), and
	 * of those the ones with no HOT GC point left (DOOMED_HOT0). HOT0 - WJC_LAZY_COLD_DOOMED = refused by
	 * another gate term (addr slots, a non-write-through ref, EH). */
	WJC_LAZY_DOOMED_SEEN,
	WJC_LAZY_DOOMED_HOT0,
	/* R293c, ungated: interp-entry trampolines adopted by a worker that did not create them, and refusals
	 * (no recorded index / an unsupported shape). */
	WJC_ENTRY_ADOPT,
	WJC_ENTRY_ADOPT_FAIL,
	/* MONO_WASM_JIT_EDGE_SAMPLE (R295): timer ticks, samples taken, samples whose callee AND caller resolved,
	 * requests dropped after 64 loop polls with no entry poll, and the two table-full refusals. */
	WJC_EDGE_TICKS,
	WJC_EDGE_SAMPLES,
	WJC_EDGE_MATCHED,
	WJC_EDGE_DROPPED,
	WJC_EDGE_NAME_FULL,
	WJC_EDGE_TABLE_FULL,
	/* Calls into mono_jiterp_placeholder_jit_call from anything but do_jit_call: an e/f slot this thread never
	 * installed was called, and 999 was written through an arbitrary pointer. Must read 0. */
	WJC_PLACEHOLDER_STRAY,
	/* Edge samples dropped because the entry poll came more than 2 ms after the tick (a blocked worker). */
	WJC_EDGE_LATE,
	/* R310 profile-delivery census, all stats-gated. The GI gate's `prof` refusal split by WHY
	 * (NOREC + COLD + POLY + torn == WJC_GI_REFUSED_PROF), with two sub-splits: a no_rec whose caller's
	 * profile is already full (WJ_PROF_MAX_SITES), and a poly site whose recorded receivers all resolve to
	 * ONE method (a method-identity guard would take it). INL: the refused site came from an inlined
	 * callee, whose calls the interpreter recorded under the CALLEE'S InterpMethod, not the caller's;
	 * INL_REC = the callee's own record would have predicted, INL_NOIM = the callee has no InterpMethod. */
	WJC_GI_PROF_NOREC,
	WJC_GI_PROF_NOREC_FULL,
	WJC_GI_PROF_COLD,
	WJC_GI_PROF_POLY,
	WJC_GI_PROF_POLY_SAMETGT,
	WJC_GI_PROF_INL,
	WJC_GI_PROF_INL_REC,
	WJC_GI_PROF_INL_NOIM,
	/* The same question weighted by EXECUTION, at the IC miss publish (wj_vcall_pic_publish). MISS ==
	 * NOSITE + FIRST + SAME_ID + SAME_TGT + DIFF_TGT. NOSITE: the caller's profile was full, so the miss was
	 * dropped. SAME_TGT: a receiver other than the site's front-runner that resolves to the SAME method.
	 * RESOLVE counts every non-delegate entry to vcall_resolve_fslot, published or not (the denominator). */
	WJC_PD_MISS,
	WJC_PD_MISS_NOSITE,
	WJC_PD_MISS_FIRST,
	WJC_PD_MISS_SAME_ID,
	WJC_PD_MISS_SAME_TGT,
	WJC_PD_MISS_DIFF_TGT,
	WJC_PD_RESOLVE,
	/* R311 mechanism census, stats-gated. MID_IC_SITES: emitted inline ICs given the method-identity fallback;
	 * MID_IC_SKIP_IFACE / _OTHER: IC sites refused it (interface slot / generic or no slot yet). MID_ARM /
	 * MID_GI: devirt arms / GI guards emitted with a method-identity check. MID_PRED_GI / _ARM: predictions
	 * that exist only because every recorded receiver resolves to one method; MID_PRED_INL: GI predictions
	 * taken from the inlined callee's own record. PROF_BLOCK_GROW: profile blocks chained, at run time. */
	WJC_MID_IC_SITES,
	WJC_MID_IC_SKIP_IFACE,
	WJC_MID_IC_SKIP_OTHER,
	WJC_MID_ARM,
	WJC_MID_GI,
	WJC_MID_PRED_GI,
	WJC_MID_PRED_ARM,
	WJC_MID_PRED_INL,
	WJC_PROF_BLOCK_GROW,
	/* R314 (L1) census, stats-gated: a cache handed back an InterpMethod that an IKVM body swap retired.
	 * RETIRED_IC_HIT: the vcall resolve cache (resolve_fslot's IC words); RETIRED_FASTMISS: get_virtual_method_fast;
	 * RETIRED_LATE_FSLOT: a heal site's baked late_im; RETIRED_DELEGATE: a delegate recipe. Each is counted
	 * RETIRED_FORWARDED: forwards taken (all of them, since forwarding is unconditional); RETIRED_FWD_LIVE: of those, the call returned a live
	 * f-slot. RESID_LIVE_CALLEE: a call_interp crossing whose canonical callee is JIT-live on this thread --
	 * the residual pool neither retired-cache route explains. */
	WJC_RETIRED_IC_HIT,
	WJC_RETIRED_FASTMISS,
	WJC_RETIRED_LATE_FSLOT,
	WJC_RETIRED_DELEGATE,
	WJC_RETIRED_FORWARDED,
	WJC_RETIRED_FWD_LIVE,
	WJC_RESID_LIVE_CALLEE,
	/* R315 (plan Phase 3) inline-policy census, stats-gated. ACCEPTED: inlines accepted in wasm compiles (at
	 * inline_method's accepted return, every policy). CALLS_LIFTED: call/ctor-call gates the policy let through
	 * inside inlinees (a decision; ACCEPTED moving is the action). RELINK_/STACKWALK_REFUSED: the two refusals kept.
	 * CLAUSE_RECHECK: inlinee header had clauses at inline time. DOWNGRADE(_OK): permanent bails recompiled at the
	 * ordinary policy (and how many of those then succeeded). */
	WJC_INLINE_ACCEPTED,
	WJC_INLINE_CALLS_LIFTED,
	WJC_INLINE_RELINK_REFUSED,
	WJC_INLINE_STACKWALK_REFUSED,
	WJC_INLINE_CLAUSE_RECHECK,
	WJC_INLINE_DOWNGRADE,
	WJC_INLINE_DOWNGRADE_OK,
	/* R316 tier 2, stats-gated: SAMPLES taken in tier-1 bodies; REQUESTED queued; COMPILED / FAILED the broker's
	 * outcome for a tier-2 request (FAILED includes a downgraded compile that then bailed too); REFUSED dropped at
	 * the broker's canonical-imethod gate. */
	WJC_T2_SAMPLES,
	WJC_T2_REQUESTED,
	WJC_T2_COMPILED,
	WJC_T2_FAILED,
	WJC_T2_REFUSED,
	WJC_PROF_ORIGIN_USED,      /* R316b: a profile read at an inlined site answered from the inlinee's record */
	WJC_PROF_ORIGIN_FALLBACK,  /* ... the site had an origin whose record has no entry for it: the root's was read */
	WJC_FAST_TLS_SITE,         /* R317: mono_tls_get_thread_extern sites emitted as loads (MONO_WASM_JIT_FAST_TLS) */
	WJC_T2_GIVEUP,             /* R316c: a tier-2 request released on the broker's BUSY budget */
	WJC_T2_GI_EMITTED,         /* R316c: guarded inlines emitted by tier-2 compiles */
	WJC_T2_GI_NOPRED,          /* R316c: guarded-inline sites in tier-2 compiles refused for want of a prediction */
	WJC_INLINE_COLD_THROW_ADMIT, /* R318: callees under the size limit only once their cold throw segments are discounted */
	WJC_T2_GI_NOREC,           /* R316i: tier-2 GI no-prediction, split: the caller record has no entry for the site */
	WJC_T2_GI_COLD,            /* ... an entry with fewer than 8 observations */
	WJC_T2_GI_POLY,            /* ... warm but polymorphic (incl. torn reads) */
	WJC_INLINE_BFI_UNINIT,     /* R319: inlinees admitted although their BeforeFieldInit class is uninitialized */
	WJC_T2_GI_NOREC_CANON,     /* R316j: tier-2 GI "no record" whose canonical generation does hold a record */
	WJC_T2_GI_SITE_ROOT,       /* R316k: tier-2 GI no-prediction at a site of the compiled method itself */
	WJC_T2_GI_INL_NOIM,        /* ... at an inlinee's site, the inlinee having no InterpMethod */
	WJC_T2_GI_INL_NOREC,       /* ... at an inlinee's site, the inlinee's record has no entry */
	WJC_T2_GI_INL_COLD,        /* ... ... an entry with fewer than 8 observations */
	WJC_T2_GI_INL_POLY,        /* ... ... warm but polymorphic (or torn) */
	WJC_ADMIT_PAYLOAD_TORN,    /* R320: admission read {batch,bytes,len,depset} mid-replacement and refused (transient) */
	WJC_LDADDR_REF_LOCAL,      /* R321: address-taken ref/byref scalar locals homed in their ref-shadow slot */
	WJC_REEMIT_VALIDATE_ONLY,  /* R322: re-emits into a slot the compiling thread had installed: validated, not published there */
	WJC_OVF_CONV_LOWERED,      /* R323: checked i64 -> i32 conversions emitted (were "unsupported opcode") */
	WJC_ATOMIC_STORE8_LOWERED, /* R323b: 1-byte OP_ATOMIC_STOREs emitted as i32.atomic.store8 (were "unsupported opcode") */
	WJC_REEMIT_HAZARD,         /* R322 probe: re-emits into a slot installed here whose body bakes a dep NOT installed here (ungated) */
	WJC_T2_RETRY_PARKED,       /* retired slot: R327's BLOCKED tier-2 retry went with the islands (R367) */
	WJC_T2_RETRY_GIVEUP,       /* retired slot, as above */
	WJC_T2_GI_STATIC_PRED,     /* R329: tier-2 GI sites with no record predicted from the callvirt's own method (decision) */
	WJC_T2_GI_STATIC_EMITTED,  /* R329: ... of which the guarded inline was emitted (action) */
	WJC_T2_REARMED,            /* R332: tier-2 requests re-armed after a BUSY give-up (ungated) */
	WJC_COLOCATE_T2_SKIP,      /* R338: automatic co-location attempts skipped because tier 2 is on (ungated) */
	WJC_BRANCH_HINT_OOL,       /* R341: IR branches hinted from bb->out_of_line, per emission */
	WJC_BRANCH_HINT_IC,        /* R341: vcall IC / devirt-guard miss exits hinted, per emission */
	WJC_MATH_INTRINS_LOWERED,  /* R346: System.Math/MathF Sqrt/Floor/Ceiling call sites lowered to a wasm opcode */
	WJC_T2_BODY_CAPPED,        /* R349: tier-2 bodies refused for exceeding MONO_WASM_JIT_T2_MAX_BODY (ungated) */
	WJC_EHREC_EMITTED,         /* R353: EH methods emitted with an inline frame record (MONO_WASM_JIT_EH_REC) */
	WJC_EHREC_TRIM,            /* R353: record exits that found the finally-save depth moved and called the trim */
	WJC_EHREC_BOUNDARY_POP,    /* R353: dead-frame records the interp->JIT boundary popped off the LMF head */
	WJC_EHREC_DISPATCH_NOREC,  /* R353: landing-pad dispatches whose cur_island was not the method's record; must be 0 */
	WJC_EHREC_UNLINK_INNER,    /* R353: record exits that unlinked through a stale inner LMF left at the head */
	WJC_EHREC_UNLINK_OUTER,    /* R353: record exits that found the head rewound past them (pass 1) and left it */
	WJC_T2_LOOP_OWED,          /* R355: entry-poll samples that owed the next back-edge a second one (ungated) */
	WJC_T2_LOOP_CREDIT,        /* R355: owed samples a back-edge took */
	WJC_T2_LOOP_LATE,          /* R355: owed samples a back-edge reached more than 2 ms after the tick (dropped) */
	/* R358: the tier-2 unit SUPPLY census (MONO_WASM_JIT_T2_UNIT=2). Per root: its distinct direct (RELOC_CALL) callees,
	 * each in exactly one bucket; CALLEES == SELF+WRAPPER+NOTJIT+UNSTABLE+EH+BIG+ELIGIBLE (+OVERFLOW, uncounted). */
	WJC_T2U_ROOTS,
	WJC_T2U_SITES,
	WJC_T2U_CALLEES,
	WJC_T2U_SELF,
	WJC_T2U_WRAPPER,
	WJC_T2U_NOTJIT,
	WJC_T2U_UNSTABLE,
	WJC_T2U_EH,
	WJC_T2U_BIG,
	WJC_T2U_ELIGIBLE,
	WJC_T2U_ELIG_HOT,          /* eligible callees with >= 8 tier-2 samples of their own */
	WJC_T2U_ELIG_SITES,
	WJC_T2U_ELIG_BYTES,
	WJC_T2U_OVERFLOW,          /* distinct callees past the 256 a root is scanned for */
	WJC_ATOMIC_LOWERED,        /* R360: 16/32-bit atomic loads/stores, CAS, exchange and add emitted inline */
	/* Phase 5, MONO_WASM_JIT_LAZY_T1 (mini-wasm-lazy.inc). Ungated. Must read 0: ORPHAN, BANK_FAIL, REPAIR. */
	WJC_LAZY_SITES,            /* direct call sites emitted through a pool f-slot */
	WJC_LAZY_RESERVE,          /* pool pairs handed out (one per callee) */
	WJC_LAZY_REUSE,            /* ... a callee that already had one */
	WJC_LAZY_REFUSE_SHAPE,     /* refused: rgctx, wrapper/synchronized, icall/runtime/pinvoke/abstract, reflection, unusable */
	WJC_LAZY_REFUSE_AOT,       /* refused: AOT-backed (keeps its inline direct AOT call) */
	WJC_LAZY_REFUSE_PERM,      /* refused: permanently un-JITtable */
	WJC_LAZY_REFUSE_SIG,       /* refused: call-site functype is not the callee's own (or not resolved yet) */
	WJC_LAZY_REFUSE_RESV,      /* refused: the callee holds a non-pool reservation (SCC batch, self-recursion) */
	WJC_LAZY_REFUSE_FULL,      /* refused: table, pool or bank space exhausted */
	WJC_LAZY_BANKS,            /* stub banks framed */
	WJC_LAZY_POOL_SLOTS,       /* table entries they took (2 per pair, reserved or not) */
	WJC_LAZY_BANK_INST,        /* bank instantiations (per pthread) */
	WJC_LAZY_BANK_FAIL,        /* ... that V8 refused */
	WJC_LAZY_DEP_STUB,         /* admission deps satisfied by a stub */
	WJC_LAZY_VALIDATE_ONLY,    /* compile-time installs of a pool pair left to admission (invariant 1) */
	WJC_LAZY_SIG_DRIFT,        /* a pool-reserved method compiled to another functype: fresh pair instead */
	WJC_LAZY_NOGC_REFUSED,     /* no-GC credit refused because the callee is a pool slot */
	WJC_LAZY_BIND,             /* stub binds */
	WJC_LAZY_BIND_REAL,        /* ... to the callee's own slot */
	WJC_LAZY_BIND_ALIAS,       /* ... to the callee's live f under another pair */
	WJC_LAZY_BIND_OTHER_SIG,   /* ... callee live under another functype: interpreter */
	WJC_LAZY_BIND_NOTREAL,     /* ... late_fslot's slot did not hold a function this worker installed */
	WJC_LAZY_REPAIR,           /* table slots found overwritten and restored (the ordering rule broken) */
	WJC_LAZY_ORPHAN,           /* a stub with no owner */
	WJC_LAZY_INTERP,           /* interpreter legs */
	WJC_LAZY_INTERP_THREW,     /* ... that threw */
	WJC_LAZY_REFUSE_WRAPPER,   /* refused: a wrapper (synchronized ones only below LAZY_T1=2) -- split out of SHAPE */
	WJC_LAZY_ARM,              /* devirt/delegate arms given a pool slot for their predicted target (LAZY_T1=3) */
	WJC_LAZY_DEAD,             /* a stub whose owner was freed or changed functype (aborts; must be 0) */
	WJC_LAZY_REFUSE_REFLECT,   /* refused: System.Reflection (split out of SHAPE) */
	WJC_LAZY_REFUSE_UNUSABLE,  /* refused: mono_wasm_jit_method_usable said no (split out of SHAPE) */
	WJC_LAZY_RESIDUAL,         /* LAZY_T1=4: refused callees routed through the residual instead of blocking */
	/* R371 census (ungated, changes nothing emitted): the wire bytes emitted per basic block and the call sites in it, by
	 * the kind of block -- OTHER, a catch/filter/fault HANDLER, or a block that THROWS -- so the order +0/+1/+2 is fixed. */
	WJC_COLD_BYTES_OTHER, WJC_COLD_BYTES_HANDLER, WJC_COLD_BYTES_THROW,
	WJC_COLD_CALLS_OTHER, WJC_COLD_CALLS_HANDLER, WJC_COLD_CALLS_THROW,
	WJC_COLD_T2_BYTES_COLD,    /* ... handler + throw bytes of tier-2 compiles */
	WJC_COLD_T2_BYTES_ALL,     /* ... all block bytes of tier-2 compiles */
	WJC_ATOMIC_I8_LOWERED,     /* J1c: 64-bit atomic loads/stores, CAS, exchange and add emitted inline (MONO_WASM_JIT_ATOMIC_I8) */
	WJC_DEVIRT_VALUETYPE,      /* R376: a devirt arm / guarded inline refused because its profile target is a VALUETYPE method */
	WJC_AGGR_INLINE_CAPPED,    /* R376: an [AggressiveInlining] exemption refused because the compile passed MONO_WASM_JIT_AGGR_INLINE_BLOCKS */
	WJC_VCALL2_BYADDR,         /* J1e: an OP_VCALL2_MEMBASE site lowered with a hidden-vret result (MONO_WASM_JIT_VCALL_VRET) */
	WJC_VCALL2_SCALAR,         /* J1e: an OP_VCALL2_MEMBASE site lowered with a single-field (scalar) value-type result */
	WJC_ICALL_AOT_SITE,        /* J1a: a direct InternalCall site emitted as an inline-AOT call of its native wrapper (MONO_WASM_JIT_ICALL_AOT) */
	WJC_LDADDR_ARG,            /* R377: an address-taken scalar argument homed in an addr-frame slot (MONO_WASM_JIT_LDADDR_ARG) */
	WJC_F4_VT_NONREF,          /* plan2x F4: vtable vregs exempted from the ref frame (MONO_WASM_JIT_F4 & 1) */
	WJC_F4_VT_REUSE,           /* ... IC / devirt / AOT-IC sites that reused the IR's vtable vreg (& 2) */
	WJC_F4_IC_MEMO,            /* ... IC method-identity fallbacks emitted with the vtable memo (& 4) */
	WJC_F4_ARM_MEMO,           /* ... devirt-arm MID guards emitted with the process-wide memo (& 8) */
	WJC_F9_MCACHE_SITES,       /* plan2x F9: IC sites emitted with the megamorphic-cache probe */
	WJC_F9_MCACHE_FILLS,       /* ... cache entries written by the PIC publish (stats-gated) */
	WJC_S1_GUARDED,            /* plan2x S1: guarded module-local calls framed (ungated) */
	WJC_S1_SUPERSEDED,         /* ... group generations whose guard word was set (ungated) */
	WJC_S1_GUARD_TAKEN,        /* ... guarded calls that took the table route, counted by the emitted code */
	WJC_S1_REEMIT_TRY,         /* ... co-location attempts after a re-emit publish (ungated) */
	WJC_T2C_SITES,             /* plan2x S0: tier-1 entry counters emitted */
	WJC_T2C_HITS,              /* ... counters that reached MONO_WASM_JIT_T2_COUNT (ungated) */
	WJC_T2C_REQUESTED,         /* ... of those, tier-2 requests made (ungated) */
	WJC_T2C_CAPPED,            /* ... refused by MONO_WASM_JIT_T2_MAX (ungated) */
	WJC_S2_SITES,              /* plan2x S2: tier-1 call-site counters emitted */
	WJC_S2_ENTRIES,            /* ... tier-1 entry counters emitted */
	WJC_S2_FULL,               /* ... records refused: the table is full (must stay 0) */
	WJC_S2_UNKNOWN,            /* ... tier-2 size gate: no trusted count for the site (static policy) */
	WJC_S2_COLD,               /* ... the site never executed: refused */
	WJC_S2_WARM,               /* ... counted, below the hot bar (static policy) */
	WJC_S2_HOT,                /* ... at or above the hot bar */
	WJC_S2_RAISED,             /* ... hot, and the limit raised for it */
	WJC_S2_NOPROF,             /* ... hot and over the static limit, but the callee has no tier-1 profile and makes calls */
	WJC_S2_BUDGET_OUT,         /* ... hot and over the static limit, but the root's budget is spent */
	WJC_S2_INLINED,            /* ... a raised site that inline_method accepted (the action) */
	WJC_S2_CTX_MATCH,          /* ... inline_method found the gate's verdict for its own site */
	WJC_S2_CTX_MISS,           /* ... inline_method entered with no matching verdict (unknown context) */
	WJC_S3_GI_PRED,            /* plan2x S3: a no-prediction GI site whose base is not overridden: CHA target */
	WJC_S3_GI_OVERRIDDEN,      /* ... a no-prediction GI site whose base is a CHA candidate but overridden */
	WJC_S3_GI_EMITTED,         /* ... a CHA-guarded inline emitted (the action) */
	WJC_S3_ARM_PRED,           /* ... an emitter devirt site with no prediction, base not overridden: CHA target */
	WJC_S3_ARM_OVERRIDDEN,     /* ... ... base a CHA candidate but overridden */
	WJC_S3_ARM_EMITTED,        /* ... a CHA-guarded direct-call arm emitted (the action) */
	WJC_S3_VERIFY_SITES,       /* ... CHA arms carrying the verify check (S3 & 4) */
	WJC_S3_GI_STATIC,          /* ... an R329 static GI prediction whose identity guard the CHA word replaced */
	WJC_S4_SEEN,               /* plan2x S4: loads examined by the applying pass */
	WJC_S4_REWRITTEN,          /* ... of those, rewritten into a move (the action) */
	WJC_T2_STRICT_TRY,         /* plan2x S0b: strict tier-2 retries after a T2_MAX_BODY failure (ungated) */
	WJC_T2_STRICT_OK,          /* ... of those, a tier-2 body (ungated) */
	WJC_S2_GI_HOT_SITES,       /* plan2x S2 & 4: tier-1 guarded-inline hot-arm counters emitted */
	WJC_S2_GI_COLD,            /* ... tier-2 GI gate: the site never executed (hot arm + fallback): refused */
	WJC_S2_GI_RAISED,          /* ... tier-2 GI gate: hot, and the GI size limit raised for it */
	WJC_S2_GI_UNKNOWN,         /* ... tier-2 GI gate: no trusted count (the GI size limit stands) */
	WJC_ADOPT_SKIP,            /* W1 C0: an install the old generation test would have skipped (R399's class: module not here); ungated */
	WJC_ADOPT_REINST,          /* W1 C0: an instantiation the old test would have repeated (same module already here); ungated */
	/* H3 (R401): a compile whose method's body was swapped after the compile took its epoch snapshot is stale -- it may
	 * have read the displaced IL -- and is refused at each point it could reach callers. Healthy, non-zero after swaps
	 * that race a compile; ungated. */
	WJC_EPOCH_REFUSED_LOCAL,     /* before this worker installs it (instantiate_local) */
	WJC_EPOCH_REFUSED_REGISTER,  /* at registration, under the loader lock: the registry keeps the previous bytes */
	WJC_EPOCH_REFUSED_PUBLISH,   /* at publication onto the InterpMethod, under the jit_mm lock */
	WJC_EPOCH_REFUSED_RV,        /* at republication: a newer swap's own re-emission will publish; its request is kept */
	WJC_PRESERVE_NOGC_REFUSED,   /* a generation that lost no-GC, over callers compiled crediting it: kept the old one */
	WJC_REEMIT_FOLLOWED,         /* the broker followed a superseded InterpMethod to the current one, request carried */
	WJC_REEMIT_MANDATORY_BUSY,   /* a body swap's re-emission lost the compile lock and was re-queued with no give-up */
	WJC_STALE_CERTIFIED,         /* W1 C0 tripwire, MUST READ 0: admitted while its slots held another module */
	/* plan H4-H6 (R403): hooks with the JIT on. Healthy: routed, restub, bind_interp, guarded, waits, skipped, proxy.
	 * MUST READ 0: route_fail, unguarded (in hookable mode), ack_timeout. */
	WJC_HOOK_CREDIT_REFUSED,     /* a no-GC credit not given because the app is hookable */
	WJC_HOOK_ROUTED,             /* hooks whose method had a live JIT generation, routed to its stub */
	WJC_HOOK_ROUTE_FAIL,         /* MUST READ 0: a live generation that could not be routed (keeps its old body) */
	WJC_HOOK_RESTUB,             /* per-worker stub installs at a hooked f-slot */
	WJC_HOOK_BIND_INTERP,        /* calls through a hooked f-slot's stub, run in the interpreter */
	WJC_HOOK_GROUP_GUARDED,      /* hooks whose method was co-located: the group's guard word set */
	WJC_HOOK_GROUP_UNGUARDED,    /* MUST READ 0 when hookable: co-located without a guard -- local calls stay stale */
	WJC_HOOK_WAITS,              /* Apply/Undo waits for every thread's acknowledgement */
	WJC_HOOK_WAIT_SKIPPED,       /* hooks with no live JIT generation: interpreter state is shared, nothing to wait for */
	WJC_HOOK_ACK_PROXY,          /* threads counted acknowledged because they were not running managed code */
	WJC_HOOK_WAIT_US_MAX,        /* high-water of one wait, microseconds */
	WJC_HOOK_ACK_TIMEOUT,        /* MUST READ 0: a wait that gave up (MONO_WASM_JIT_HOOK_ACK_TIMEOUT_MS) */
	WJC_T2_COLD_PRED_GI,         /* R405: tier-2 GI sites predicted from a one-receiver COLD record (decision) */
	WJC_T2_COLD_PRED_GI_INL,     /* R405: ... of which from the inlinee's record */
	WJC_T2_COLD_PRED_ARM,        /* R405: tier-2 devirt arms predicted from a one-receiver COLD record (decision) */
	WJC_PROF_ANC_FOUND,          /* R407: tier-2 GI sites with no record of their own whose enclosing inlinee had one */
	WJC_PROF_ANC_GI,             /* R407: ... predicted from it (decision) */
	WJC_PROF_ANC_ARM,            /* R407: tier-2 virtual calls re-keyed to an enclosing inlinee's record for the arm */
	WJC_T2_LOOP_SITES,           /* R409: tier-1 loop polls emitted with the back-edge counter */
	WJC_UNDEF_LOCAL,             /* R410: local variables read but never written in a body, typed from their variable */
	WJC_T2_GI2_CANDIDATE,        /* R411: tier-2 GI sites whose record names a second target */
	WJC_T2_GI2_EMITTED,          /* R411: ... inlined behind a second guard (the action) */
	WJC_T2_GI2_LATE,             /* R411: ... refused by inline_method after the guard was emitted */
	WJC_T2_GI2_POLY_ARM1,        /* R411: tier-2 POLYMORPHIC GI sites predicted their top arm (decision) */
	WJC_T2_GI2_RETRY,            /* R411: tier-2 bodies over T2_MAX_BODY with bimorphic arms, recompiled without them */
	WJC_T2_GI2_RETRY_OK,         /* ... and that fit */
	WJC_B4_PLANS,                /* plan B4 (R443): hot-set partitions computed by the re-emit drainer */
	WJC_B4_FRAMED,               /* ... groups framed (mono_wasm_jit_rebatch succeeded; the action) */
	WJC_B4_REFUSED,              /* ... groups refused (rebatch failed, or over the member / byte bound) */
	WJC_B4_MEMBERS,              /* ... members in the framed groups (merged siblings included) */
	WJC_B4_PUBLISHED,            /* ... framed groups queued for the next rendezvous */
	WJC_MAX
};

/* Retainer identities for mono_wasm_jit_method_usable(). Kept beside the WJC_BADMETH_* counters they
 * select so a new site cannot be added without a counter to attribute it to. */
enum {
	WJ_BADMETH_SITE_PEEK = 0,
	WJ_BADMETH_SITE_REGISTRY,
	WJ_BADMETH_SITE_PROFILE,
	WJ_BADMETH_SITE_CANON,
};

extern int mono_wasm_jit_stats;                     /* master gate: MONO_WASM_JIT_STATS=1 */
extern gint64 mono_wasm_jit_counters [WJC_MAX];     /* the counters (raw counts / microseconds) */
void mono_wasm_jit_count (int idx);                 /* atomic += 1 */
void mono_wasm_jit_add (int idx, gint64 v);         /* atomic += v (bytes / microseconds) */

/* Per-worker instantiation census: MONO_WASM_JIT_ENTRYCENSUS=1. Separate from the WJC_* counters
 * because those are process-wide and this question is per-thread. See mini-wasm.c. */
extern int mono_wasm_jit_entry_census;
void mono_wasm_jit_census_note_entry (int eslot);

/* Offset, past the per-thread residual scratch (`s.b`, interp.c's wj_scratch), of the word holding the address of
 * this thread's mono_wasm_sgen_tls_info -- read by OP_WASM_JIT_ALLOC_FAST (R305). interp.c asserts it equals
 * WJ_SCRATCH_SIZE. */
#define WJ_SCRATCH_TLAB_SLOT 256
/* The next word: the address of this thread's mono_wasm_tls_thread (threads.c), read by the MONO_WASM_JIT_FAST_TLS
 * form of mono_tls_get_thread_extern (R317). */
#define WJ_SCRATCH_THREAD_SLOT (WJ_SCRATCH_TLAB_SLOT + 8)
/* R353 (MONO_WASM_JIT_EH_REC): the address of this thread's LMF-address mirror (mini-runtime.c), and of its
 * finally-save depth (interp.c). */
#define WJ_SCRATCH_LMFADDR_SLOT (WJ_SCRATCH_THREAD_SLOT + 8)
#define WJ_SCRATCH_FINSP_SLOT (WJ_SCRATCH_LMFADDR_SLOT + 8)

/* Byte offsets of interp.c's WjEhRec (R353), for the emitter, which cannot see MonoLMFExt from here. */
typedef struct {
	int size, previous_lmf, lmf_addr, lmf_method, kind, il_state, magic, prev, finally_sp, il, il_method, il_offset, body;
	guint32 magic_key;
	int kind_il_state;
} WjEhRecLayout;
const WjEhRecLayout *mono_wasm_jit_ehrec_layout (void);

#endif /* __MONO_MINI_WASM_H__ */
