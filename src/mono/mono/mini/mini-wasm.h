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
