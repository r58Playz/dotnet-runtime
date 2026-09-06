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
	WJC_VIC_HIT, WJC_VIC_MISS, WJC_VFAST_HAD, WJC_VFAST_NEW, WJC_VFB_THRESH, WJC_VFB_PERM, WJC_VSYNC_WORK,
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
	/* island formation outcomes (Part 3b/5) */
	WJC_ISLAND_ATTEMPT, WJC_ISLAND_COMPLETED, WJC_ISLAND_BUDGET_EXHAUSTED, WJC_ISLAND_DEPTH_EXCEEDED,
	WJC_ISLAND_BLOCKED_PERM, WJC_ISLAND_BLOCKED_COLD, WJC_PROMOTED_UP, WJC_PROMOTED_DOWN,
	/* finer split of the perm-unjittable vcall residual (was lumped into WJC_VPERM_OTHER): which override
	 * shape dominates the steady-state virtual-dispatch boundary cost. SIG=arg/ret type; the rest as named.
	 * AOT = the override is NOT wasm-jitted because it already has native AOT code (slot==-1, bail==0): the
	 * vcall falls back to the AOT residual, NOT an emitter bail — this is the bulk of the perm vcall cost. */
	WJC_VPERM_SIG, WJC_VPERM_BYREF, WJC_VPERM_GSHARED, WJC_VPERM_SYNC, WJC_VPERM_EHOTHER, WJC_VPERM_AOT,
	/* vcalls that took the fast AOT dispatch (MONO_WASM_JIT_VCALL_AOT) instead of the residual */
	WJC_VCALL_AOT_FAST,
	/* event-driven blocker waiting (Part 3 revamp): WJC_PARKED = times a method parked on cold blocker(s)
	 * instead of poll-retrying; WJC_WAITER_WOKEN = total waiters re-queued when a blocker JITted. */
	WJC_PARKED, WJC_WAITER_WOKEN,
	/* below-threshold vcall fallback (WJC_VFB_THRESH) split by the target's wasm_jit_slot state, so we can
	 * tell "cold callee, interp is fine" apart from "hot method whose island won't close" (the real interp-
	 * residual driver): VFB_COLD = slot 0 (still counting), VFB_PARKED = slot -2 (crossed thresh, island
	 * blocked on a cold callee), VFB_RETRY = slot -3 (transient compile-lock contention). Sum == VFB_THRESH. */
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
	/* Admissions deferred because an IMPORTED dependency was still mid-DFS on this worker, so binding it
	 * would have been a LinkError. Not a failure: the method runs interpreted and a later dispatch retries.
	 * Read it against WJC_CALL_IMPORT -- a few per thousand imports is the ordering noise the retry exists
	 * to absorb; a number near the import count means the graph is far more cyclic than assumed. */
	WJC_ADMIT_DEFERRED,
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
	/* MONO_WASM_JIT_DEVIRT_FORCE (default 0). FORCED = a NO_FSLOT site whose predicted target was turned
	 * into an island blocker instead of being dropped, so the caller re-emits once the callee publishes.
	 * CAPPED = the same site declined because the method already carried DEVIRT_FORCE_MAX blockers.
	 *
	 * Read FORCED against the fall in DEVIRT_NO_FSLOT and the rise in DEVIRT_EMITTED for reach, and
	 * against WJC_PARKED / WJC_ISLAND_DEPTH_EXCEEDED / WJC_ISLAND_BUDGET_EXHAUSTED for the cost. The cost
	 * side is the whole reason this is a knob: R153's world-load stall came from methods that could not
	 * clear their blockers and ran interpreted, and this deliberately creates more blockers. */
	WJC_DEVIRT_FORCED, WJC_DEVIRT_FORCE_CAPPED,
	/* MONO_WASM_JIT_COLOCATE_TIGHT_DEPS: dependency entries dropped when a re-framed module's dependency
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
	 * VFB_COLD + VFB_PARKED + VFB_RETRY + VFB_NOTLIVE == VFB_THRESH; check that before trusting a share. */
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
	/* CO-LOCATION REACH AS EXECUTED, split by where the runtime target actually lives. Bumped on the IC
	 * MISS/publish path -- i.e. once per distinct (site, receiver) pair, not per dispatch -- because that
	 * is the only place the resolved target and the calling descriptor are both in hand.
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
	WJC_MAX
};

extern int mono_wasm_jit_stats;                     /* master gate: MONO_WASM_JIT_STATS=1 */
extern gint64 mono_wasm_jit_counters [WJC_MAX];     /* the counters (raw counts / microseconds) */
void mono_wasm_jit_count (int idx);                 /* atomic += 1 */
void mono_wasm_jit_add (int idx, gint64 v);         /* atomic += v (bytes / microseconds) */

/* Per-worker instantiation census: MONO_WASM_JIT_ENTRYCENSUS=1. Separate from the WJC_* counters
 * because those are process-wide and this question is per-thread. See mini-wasm.c. */
extern int mono_wasm_jit_entry_census;
void mono_wasm_jit_census_note_entry (int eslot);

#endif /* __MONO_MINI_WASM_H__ */
