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
#ifdef HOST_BROWSER
#include <emscripten.h>
int mono_jiterp_allocate_table_entry (int type); /* interp/jiterpreter.c */
/* Relay: the emitter sets this (thread-local) to the registered entry-thunk slot on a
 * successful compile; the transform hook (same thread) reads it and stores it on the
 * method's InterpMethod.wasm_jit_slot, so the interp can invoke any JITted method. */
__thread int mono_wasm_jit_last_slot = 0;
__thread int mono_wasm_jit_last_fslot = 0;
/* Relay: set at `done:` to 1 if the compile bailed for a RETRIABLE reason ("callee not jitted" — the
 * callee may become JITted later, so the caller should re-attempt to form a JIT island), else 0. The
 * auto-JIT trigger uses this to choose a retry slot state vs a permanent bail. */
__thread int mono_wasm_jit_last_retriable = 0;
/* Relay: on a "callee not jitted (residual off)" bail under the islands policy, the emitter sets this
 * (thread-local) to the un-JITted DIRECT callee that blocked the compile. The auto-JIT trigger uses it
 * to eagerly force-compile that callee (and, recursively, ITS blocking callees) so a hot method's whole
 * call-tree island forms in one shot, instead of slowly bottom-up over many threshold-crosses + retries
 * (the chunk-mesh getBlockState->index->accessor chains the IL inspection showed). NULL if none. */
__thread MonoMethod *mono_wasm_jit_last_blocking_callee = NULL;
int mono_wasm_jit_island = 1;   /* eager transitive island-JIT; MONO_WASM_JIT_ISLAND=0 = old bottom-up retry only */
/* Relay for the cached module bytes (for per-thread instantiation): the emitter g_malloc-copies the
 * emitted module here on a successful compile; the caller stores it on InterpMethod.wasm_jit_bytes. */
__thread void *mono_wasm_jit_last_bytes = NULL;
__thread int mono_wasm_jit_last_len = 0;
/* Relay: set at `done:` to categorize WHY a compile bailed, for the weighted vcall-residual breakdown
 * (which permanently-un-JITtable overrides dominate the steady-state residual). 0=success/n/a;
 * -2=EH clauses; -3=signature (arg/ret) type; -4=other (synchronized/byref/etc); >0=the unsupported
 * mini opcode number. compile_publish stores it on InterpMethod.wasm_jit_bail when the bail is permanent. */
__thread int mono_wasm_jit_last_bail = 0;
/* Automatic hotness trigger (Phase 5): when mono_wasm_jit_auto>0, the interp (MINT_CALL) counts
 * calls to each callee and force-compiles it to wasm once its hit count reaches mono_wasm_jit_thresh,
 * instead of requiring the method to be named in MONO_WASM_JIT_METHOD. -1 = uninitialized. */
int mono_wasm_jit_auto = -1;
int mono_wasm_jit_thresh = 2000;
int mono_wasm_jit_vcall_ic = 1;   /* virtual-dispatch resolve cache; MONO_WASM_JIT_VCALL_IC=0 disables (always resolve) */

void
mono_wasm_jit_auto_init (void)
{
	const char *e, *t;
	if (mono_wasm_jit_thresh >= 0 && mono_wasm_jit_auto >= 0)
		return;
	t = g_getenv ("MONO_WASM_JIT_THRESHOLD");
	mono_wasm_jit_thresh = (t && *t) ? atoi (t) : 2000;
	if (mono_wasm_jit_thresh <= 0)
		mono_wasm_jit_thresh = 2000;
	{ extern int mono_wasm_jit_stats; const char *s = g_getenv ("MONO_WASM_JIT_STATS"); mono_wasm_jit_stats = (s && *s && *s != '0') ? 1 : 0; }
	/* MONO_WASM_JIT_VERBOSE controls the per-method emit LOG spam, DECOUPLED from stats (counting is cheap,
	 * logging floods): 0=silent (default), 1=+registered/invalid, 2=+bail, 3=+emit-enter. The 23k-line log
	 * came from these firing whenever stats was on; the aggregated bail histogram replaces them at level 0. */
	{ extern int mono_wasm_jit_verbose; const char *vb = g_getenv ("MONO_WASM_JIT_VERBOSE"); mono_wasm_jit_verbose = (vb && *vb) ? atoi (vb) : 0; }
	{ extern int mono_wasm_jit_residual_mode; const char *r = g_getenv ("MONO_WASM_JIT_RESIDUAL"); mono_wasm_jit_residual_mode = (r && *r) ? atoi (r) : 1; }
	{ extern int mono_wasm_jit_virtual; const char *v = g_getenv ("MONO_WASM_JIT_VIRTUAL"); mono_wasm_jit_virtual = (v && *v && *v == '0') ? 0 : 1; } /* 0 = bail virtual calls (revert to whole-method interp); default on */
	{ extern int mono_wasm_jit_vcall_ic; const char *c = g_getenv ("MONO_WASM_JIT_VCALL_IC"); mono_wasm_jit_vcall_ic = (c && *c && *c == '0') ? 0 : 1; } /* 0 = disable the virtual resolve cache (always resolve) */
	{ extern int mono_wasm_jit_cond_exc; const char *ce = g_getenv ("MONO_WASM_JIT_COND_EXC"); mono_wasm_jit_cond_exc = (ce && *ce && *ce == '0') ? 0 : 1; } /* 0 = bail OP_COND_EXC_* methods to interp */
	{ extern int mono_wasm_jit_island; const char *il = g_getenv ("MONO_WASM_JIT_ISLAND"); mono_wasm_jit_island = (il && *il && *il == '0') ? 0 : 1; } /* 0 = no eager island formation (bottom-up retry only) */
	{ extern int mono_wasm_jit_aot_residual; const char *ar = g_getenv ("MONO_WASM_JIT_AOT_RESIDUAL"); mono_wasm_jit_aot_residual = (ar && *ar && *ar == '0') ? 0 : 1; } /* jit->AOT fastpath: residual/vcall-fallback to an AOT'd callee runs it natively via do_jit_call. 0 = old behaviour (interpret it). */
	{ extern int mono_wasm_jit_no_lmf; const char *nl = g_getenv ("MONO_WASM_JIT_NO_LMF"); mono_wasm_jit_no_lmf = (nl && *nl && *nl != '0') ? 1 : 0; } /* PROOF: skip interp_push_lmf in do_jit_call for wasm-JIT residual AOT calls. Tests whether the LMF is needed (conservative GC + local wasm-EH suggest not). 1 = skip, default 0 = keep. */
	{ extern int mono_wasm_jit_inline_aot; const char *ia = g_getenv ("MONO_WASM_JIT_INLINE_AOT"); mono_wasm_jit_inline_aot = (ia && *ia && *ia != '0') ? 1 : 0; } /* emit the inline direct same-ABI AOT call instead of the residual. Build 1 = no wasm-EH (non-throwing callees only). 1 = on, default 0 = off. */
	{ extern int mono_wasm_jit_aotconst; const char *ac = g_getenv ("MONO_WASM_JIT_AOTCONST"); mono_wasm_jit_aotconst = (ac && *ac) ? (*ac != '0') : 1; } /* bake resolved OP_AOTCONST pointers; default ON */
	{ extern int mono_wasm_jit_rgctx; const char *rg = g_getenv ("MONO_WASM_JIT_RGCTX"); mono_wasm_jit_rgctx = (rg && *rg) ? (*rg != '0') : 1; } /* JIT uses_rgctx_reg methods, routing the rgctx call through the interp residual; default ON, =0 reverts to the whole-method bail */
	{ extern int mono_wasm_jit_eh_nocxa; const char *en = g_getenv ("MONO_WASM_JIT_EH_NOCXA"); mono_wasm_jit_eh_nocxa = (en && *en && *en != '0') ? 1 : 0; } /* bisection: skip begin/end_catch in the EH landing pad */
	{ extern int mono_wasm_jit_cppeh; const char *ch = g_getenv ("MONO_WASM_JIT_CPPEH"); mono_wasm_jit_cppeh = (ch && *ch && *ch != '0') ? 1 : 0; } /* AOT-style EH: propagate via C++/wasm-EH native unwinding (throw helpers C++-throw; drop EMIT_PENDING_EXC_CHECK + inline-AOT try/catch; interp->JIT boundary catches C++). 0 = resume-state model (default until validated). */
	{ extern const char *mono_wasm_jit_dump_ir; mono_wasm_jit_dump_ir = g_getenv ("MONO_WASM_JIT_DUMP_IR"); } /* substring filter; methods whose full name contains it get their clauses+bb regions+opcodes dumped (ground truth for the nested-EH lowering). */
	/* Island heuristic levers (Part 5), all default off. */
	{ extern int mono_wasm_jit_entry_promote; const char *ep = g_getenv ("MONO_WASM_JIT_ENTRY_PROMOTE"); mono_wasm_jit_entry_promote = (ep && *ep) ? atoi (ep) : 0; }      /* Lever A: 0=off */
	{ extern int mono_wasm_jit_residual_perm; const char *rp = g_getenv ("MONO_WASM_JIT_RESIDUAL_PERM"); mono_wasm_jit_residual_perm = (rp && *rp && *rp != '0') ? 1 : 0; } /* Lever B: 0=off */
	{ extern int mono_wasm_jit_island_depth; const char *id = g_getenv ("MONO_WASM_JIT_ISLAND_DEPTH"); mono_wasm_jit_island_depth = (id && *id && atoi (id) > 0) ? atoi (id) : 10; }   /* Lever C: default 10 */
	{ extern int mono_wasm_jit_island_budget; const char *ib = g_getenv ("MONO_WASM_JIT_ISLAND_BUDGET"); mono_wasm_jit_island_budget = (ib && *ib && atoi (ib) > 0) ? atoi (ib) : 64; } /* Lever C: default 64 */
	{ extern int mono_wasm_jit_block_promote; const char *bp = g_getenv ("MONO_WASM_JIT_BLOCK_PROMOTE"); mono_wasm_jit_block_promote = (bp && *bp) ? atoi (bp) : 16; } /* Lever C: default 16; 0 disables */
	e = g_getenv ("MONO_WASM_JIT_AUTO");
	mono_memory_barrier ();
	mono_wasm_jit_auto = (e && *e && *e != '0') ? 1 : 0; /* set last: publishes "initialized" */
}
#endif

/* Defined unconditionally for TARGET_WASM (not just HOST_BROWSER): mono_wasm_force_compile + the
 * mini.c COMPILE_WASM trigger reference it, and those compile into the cross-compiler too (where
 * HOST_BROWSER is off). Set around a forced compile so the trigger routes the method to COMPILE_WASM
 * regardless of name targeting. */
__thread gboolean mono_wasm_jit_force = FALSE;

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

/* Racy max (no atomic CAS loop): a high-water mark read for diagnosis only, where an occasional lost
 * update under a race is harmless. */
void
mono_wasm_jit_max (int idx, gint64 v)
{
	if (v > mono_wasm_jit_counters [idx])
		mono_wasm_jit_counters [idx] = v;
}

/* Per-method emit LOG verbosity (MONO_WASM_JIT_VERBOSE), independent of the counters: 0 silent, 1
 * registered+invalid, 2 +bail, 3 +emit-enter. Default 0 so a stats run no longer floods stdout. */
int mono_wasm_jit_verbose = 0;

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
 *   1 = full (residual every supported direct call; default when the env is unset)
 *   2 = only calls with a VOID return   (skips return-value marshalling)
 *   3 = only calls with NO params       (skips param marshalling; `this` still allowed)
 *   4 = only STATIC calls (no `this`)   (skips this marshalling; params/return allowed)
 *   5 = everything EXCEPT calls with params AND a non-void return
 * Check the bench stats (residual count) to confirm a restricted mode still exercised the residual. */
int mono_wasm_jit_residual_mode = 1;
/* MONO_WASM_JIT_VIRTUAL: 0 bails virtual/interface calls (the whole method falls back to the
 * interpreter, the pre-virtual-lift behaviour) — a no-rebuild kill-switch + A/B bisection knob to
 * confirm whether the virtual-dispatch path is what destabilises real (MT) code. Default 1 (on). */
int mono_wasm_jit_virtual = 1;
/* Defined here (not in the HOST_BROWSER block) because mono_wasm_emit_method references it in BOTH the
 * runtime and the cross-compiler build; the env-init lives in mono_wasm_jit_auto_init (HOST_BROWSER). */
int mono_wasm_jit_cond_exc = 1;   /* JIT OP_COND_EXC_* (checked-op throws); MONO_WASM_JIT_COND_EXC=0 bails those methods */
int mono_wasm_jit_aot_residual = 1;   /* jit->AOT fastpath: route a wasm-JITted method's residual/vcall-fallback to an AOT'd callee through do_jit_call (native) instead of interpreting it; MONO_WASM_JIT_AOT_RESIDUAL=0 reverts */
int mono_wasm_jit_no_lmf = 0;         /* PROOF (MONO_WASM_JIT_NO_LMF=1): skip the per-call LMF push/pop in do_jit_call for wasm-JIT residual AOT calls — validates the inline-direct-AOT-call plan (which can't cheaply push an LMF). Conservative GC + local wasm-EH should make it unneeded. */
int mono_wasm_jit_inline_aot = 0;     /* MONO_WASM_JIT_INLINE_AOT=1: emit the inline direct same-ABI AOT call (call_indirect cinfo->addr with this+args+rgctx, no interp_entry/frame/LMF) instead of the residual, for AOT'd callees. Build 1 = no wasm-EH yet (test non-throwing callees). default off. */
int mono_wasm_jit_eh_nocxa = 0;       /* MONO_WASM_JIT_EH_NOCXA=1 (bisection): skip the __cxa_begin_catch/end_catch in the in-method catch landing pad, to test whether the cxa lifecycle (on nested catch + try re-entry) is the world-load corruption. */
int mono_wasm_jit_aotconst = 1;       /* MONO_WASM_JIT_AOTCONST: bake resolved OP_AOTCONST pointers (vtable/class/method/static/image) so newobj/token-constant methods JIT instead of bailing. Default ON (validated on MC jit108; the earlier suspected regression was the missing JSPI build flag, not this). MONO_WASM_JIT_AOTCONST=0 reverts to the bail. */
/* MONO_WASM_JIT_RGCTX: 1 = JIT methods that call a generic-shared callee needing a runtime generic
 * context (cfg->uses_rgctx_reg), routing each such call through the interp residual (which derives the
 * context from the concrete inflated call->method — both interp_entry and do_jit_call-via-gsharedvt_out
 * are rgctx-correct), instead of bailing the WHOLE method at the gate. This is the dominant EH-method
 * blocker on IKVM: a Java try/catch lowers to a catch-block call to the generic ExceptionHelper.MapException<T>,
 * which sets uses_rgctx_reg — so the rgctx bail was killing nearly every render-path EH method (e.g.
 * tesselateBlock) right after the EH gate let it through. The rgctx call sits in the COLD catch block, so
 * the (slower) residual re-entry there is free; the hot try-body JITs to wasm. The direct f-slot path needs
 * no rgctx (our f-slots are dedicated/concrete compiles); INLINE_AOT is skipped for rgctx calls (it would
 * bake a NULL rgctx — cinfo->extra_arg is only populated under mono_llvm_only, which this runtime is not);
 * indirect/virtual rgctx calls still bail (untested shape). Default ON; MONO_WASM_JIT_RGCTX=0 reverts to the
 * old whole-method bail. */
int mono_wasm_jit_rgctx = 1;
int mono_wasm_jit_cppeh = 0;          /* MONO_WASM_JIT_CPPEH=1: AOT-style EH — exceptions propagate via C++/wasm-EH native stack unwinding (mono_llvm_cpp_throw_exception) instead of cooperative resume-state. throw/raise helpers C++-throw; the emitter drops EMIT_PENDING_EXC_CHECK + the inline-AOT try/catch (calls become pure call_indirect = the per-call perf win); the interp->JIT boundary (MINT_CALL e-thunk) wraps the invoke in mono_llvm_catch_exception to convert an escaping C++ exception back to interp resume-state + restore the ref-shadow-stack SP. default 0 = resume-state model. */
const char *mono_wasm_jit_dump_ir = NULL;  /* MONO_WASM_JIT_DUMP_IR=<substr>: dump clauses + bb regions + opcode stream for clause-bearing methods whose full name contains <substr> (EH-lowering ground truth, e.g. "indigo"). */
/* Island heuristic levers (Part 5), all default-OFF so the baseline is unchanged and each can be A/B'd. */
int mono_wasm_jit_entry_promote = 0;   /* Lever A: MONO_WASM_JIT_ENTRY_PROMOTE=N — after a hot interp caller invokes JITted callees N times, force-JIT the caller (grow the island UPWARD). 0 = off. */
int mono_wasm_jit_residual_perm = 0;   /* Lever B: MONO_WASM_JIT_RESIDUAL_PERM=1 — under residual=0, residual-route ONLY a permanently-un-JITtable blocker instead of bailing the whole caller. 0 = off. */
int mono_wasm_jit_island_depth = 10;   /* Lever C: MONO_WASM_JIT_ISLAND_DEPTH — max island DFS depth (was a fixed 10). */
int mono_wasm_jit_island_budget = 64;  /* Lever C: MONO_WASM_JIT_ISLAND_BUDGET — max force-compiles per island attempt (was a fixed 64). */
int mono_wasm_jit_block_promote = 16;  /* Lever C: MONO_WASM_JIT_BLOCK_PROMOTE — pull a cold callee into an island once it has BLOCKED >= N island attempts (block_n), even if its own hit count is low (it's hot via JITted callers). 0 = disable (cold gate is hits-only). The bench showed top blockers ~100, so the old thresh/4 (=500) never fired — 16 catches the hot ctors. */

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
 * halves of the registered set and re-run. */
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

#ifdef HOST_BROWSER
/* Forward a bring-up diagnostic line to the UI-thread console. Worker stdout/stderr (printf,
 * g_printerr) is NOT surfaced to the page, and g_printerr is compiled out in release; managed
 * Console.WriteLine reaches main via a separate path. Mirror the consumer's log forwarder:
 * strdup + async proxy to the main thread, which frees the copy after logging. */
void
mono_wasm_jit_log_main (const char *msg)
{
	/* worker stdout (reaches the DevTools worker console). No main-thread proxy: on real workloads
	 * the JIT emits thousands of lines and the cross-thread MAIN_THREAD_ASYNC_EM_ASM console.log
	 * proxy floods the UI-thread task queue, dominating boot time. */
	printf ("%s\n", msg);
	fflush (stdout);
}

/* Single bench getter (analog of the jiterpreter's mono_jiterp_get_counter): the consumer's
 * heat-snapshot harness reads each counter by its WJC_* index via Module._mono_wasm_jit_get_counter().
 * Returns a double so the harness gets exact values past 2^53 isn't a concern at bench scale (and JS
 * has no int64). Cumulative since boot — the harness snapshots the delta over its window. The
 * ELAPSED_* counters are microseconds; BYTES_GENERATED is bytes; the rest are plain counts. */
EMSCRIPTEN_KEEPALIVE double
mono_wasm_jit_get_counter (int idx)
{
	if (idx < 0 || idx >= WJC_MAX)
		return 0;
	return (double) mono_wasm_jit_counters [idx];
}

#define WJC_(i) ((long long) mono_wasm_jit_counters [i])

EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_stats (void)
{
	printf ("[wasm-jit stats] registered=%lld bailed=%lld invalid=%lld invoked=%lld residual=%lld fastvcall=%lld ref_hwm=%lld/%d (counting %s)\n",
		WJC_(WJC_REGISTERED), WJC_(WJC_BAILED), WJC_(WJC_INVALID),
		WJC_(WJC_INVOKE), WJC_(WJC_RESIDUAL), WJC_(WJC_FASTVCALL), WJC_(WJC_REF_HWM), 64 * 1024,
		mono_wasm_jit_stats ? "on" : "OFF — set MONO_WASM_JIT_STATS=1");
	printf ("[wasm-jit time] gen=%.1fms instantiate=%.1fms attempts=%lld bytes=%lld\n",
		(double) WJC_(WJC_ELAPSED_GENERATION) / 1000.0, (double) WJC_(WJC_ELAPSED_INSTANTIATION) / 1000.0,
		WJC_(WJC_COMPILE_ATTEMPTS), WJC_(WJC_BYTES_GENERATED));
	printf ("[wasm-jit island] attempt=%lld completed=%lld budget_exhausted=%lld depth_exceeded=%lld blocked_perm=%lld blocked_cold=%lld promoted_up=%lld promoted_down=%lld\n",
		WJC_(WJC_ISLAND_ATTEMPT), WJC_(WJC_ISLAND_COMPLETED), WJC_(WJC_ISLAND_BUDGET_EXHAUSTED), WJC_(WJC_ISLAND_DEPTH_EXCEEDED),
		WJC_(WJC_ISLAND_BLOCKED_PERM), WJC_(WJC_ISLAND_BLOCKED_COLD), WJC_(WJC_PROMOTED_UP), WJC_(WJC_PROMOTED_DOWN));
	printf ("[wasm-jit vcall] ic_hit=%lld ic_miss=%lld vfast_had=%lld vfast_new=%lld vfb_thresh=%lld vfb_perm=%lld vsync_work=%lld\n",
		WJC_(WJC_VIC_HIT), WJC_(WJC_VIC_MISS), WJC_(WJC_VFAST_HAD), WJC_(WJC_VFAST_NEW),
		WJC_(WJC_VFB_THRESH), WJC_(WJC_VFB_PERM), WJC_(WJC_VSYNC_WORK));
	printf ("[wasm-jit vperm] aot=%lld eh=%lld ldaddr=%lld lcompare=%lld sig=%lld byref=%lld gshared=%lld sync=%lld eh_other=%lld other_opcode=%lld other=%lld\n",
		WJC_(WJC_VPERM_AOT), WJC_(WJC_VPERM_EH), WJC_(WJC_VPERM_LDADDR), WJC_(WJC_VPERM_LCMP),
		WJC_(WJC_VPERM_SIG), WJC_(WJC_VPERM_BYREF), WJC_(WJC_VPERM_GSHARED), WJC_(WJC_VPERM_SYNC), WJC_(WJC_VPERM_EHOTHER),
		WJC_(WJC_VPERM_OTHEROP), WJC_(WJC_VPERM_OTHER));
	printf ("[wasm-jit aotroute] aot_routed=%lld interp_routed=%lld\n",
		WJC_(WJC_AOT_ROUTED), WJC_(WJC_INTERP_ROUTED));
	fflush (stdout);
	/* the bail histogram (this file) + the island blockers / hot entry-edges (interp.c) */
	{
		extern void mono_wasm_jit_dump_bail_hist (void);
		extern void mono_wasm_jit_dump_blockers (int topn);
		extern void mono_wasm_jit_dump_hot_edges (int topn);
		mono_wasm_jit_dump_bail_hist ();
		mono_wasm_jit_dump_blockers (40);
		mono_wasm_jit_dump_hot_edges (40);
	}
}

/* Dump the residual-callee ring buffer (last <=128 residual calls, oldest first). Call AFTER a crash
 * to name the residual whose interp_entry triggered it (the last entries). Needs MONO_WASM_JIT_STATS=1. */
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_residual_ring (void)
{
	int n = mono_wasm_jit_ring_count;
	int start = n > 128 ? n - 128 : 0;
	int i;
	printf ("[wasm-jit residual ring] last %d residual callees (oldest first):\n", n - start);
	for (i = start; i < n; ++i) {
		char *nm = mono_method_get_full_name (mono_wasm_jit_ring [i & 127]);
		printf ("  [%d] %s\n", i, nm ? nm : "?");
		g_free (nm);
	}
	fflush (stdout);
}

/* Aggregated bail-reason histogram (Part 4): one summary instead of thousands of WASM_JIT_BAIL lines.
 * For the unsupported-opcode bucket, lists the top per-opcode counts so the dominant blocker (e.g.
 * ldaddr) is named. Cumulative since boot. */
static const char *wj_opname (int op);
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_bail_hist (void)
{
	static const char * const wj_bail_cat_name [WJB_N] = {
		"callee-not-jitted", "residual-shape", "eh-clause", "arg/ret-type", "synchronized/other", "unsupported-opcode"
	};
	gint64 total = 0;
	int i;
	for (i = 0; i < WJB_N; ++i) total += wj_bail_hist [i];
	printf ("[wasm-jit bails] total=%lld\n", (long long) total);
	for (i = 0; i < WJB_N; ++i) {
		if (!wj_bail_hist [i]) continue;
		printf ("  %-20s %8u (%.1f%%)\n", wj_bail_cat_name [i], wj_bail_hist [i],
			total ? 100.0 * (double) wj_bail_hist [i] / (double) total : 0.0);
		if (i == WJB_OPCODE) {
			/* name the top opcodes inside the bucket — non-destructive top-8 by repeated lexicographic
			 * max of (count, opcode) strictly below the previous pick (no mutation, dump is re-runnable). */
			guint32 lastv = 0xffffffffu; int lasti = WJ_BAIL_OPMAX; int passes;
			for (passes = 0; passes < 8; ++passes) {
				int best = -1, op; guint32 bestv = 0;
				for (op = 0; op < WJ_BAIL_OPMAX; ++op) {
					guint32 v = wj_bail_op_hist [op];
					if (!v || v > lastv || (v == lastv && op >= lasti)) continue;
					if (best < 0 || v > bestv || (v == bestv && op > best)) { best = op; bestv = v; }
				}
				if (best < 0) break;
				printf ("      %-24s op=%-4d %8u\n", wj_opname (best), best, bestv);
				lastv = bestv; lasti = best;
			}
		}
	}
	fflush (stdout);
}

/* Instantiate a cached JITted module into the CURRENT thread's wasm function table. The table is
 * per-thread for dynamically-added entries, so each thread must do this once (lazily, on its first
 * invoke of the method — interp.c MINT_CALL) before call_indirect-ing the slot. Returns 1 on success,
 * 0 on failure (caller then disables the JIT for the method → interpreter fallback). */
int
mono_wasm_jit_instantiate_local (int e_slot, int f_slot, const void *bytes, int len, char *errbuf, int errcap, double *out_ms)
{
	/* $2/$4/$6 below are pointers passed as 32-bit ints; a g_malloc buffer above 2GB (MC's heap grows past
	 * it) arrives NEGATIVE in JS, and HEAPU8.slice(negative,..) reads the wrong region -> garbage module
	 * bytes -> a magic-word CompileError. Re-add 2^32 to recover the real unsigned address. (Use a C-valid
	 * ternary, NOT JS >>>, since clang parses the EM_ASM body tokens and >>> is not a C operator.)
	 * out_ms ($6, an 8-byte-aligned double*) receives the WebAssembly.Module+Instance compile time in ms
	 * (Part 2 instantiation timing) — measured on both the success and CompileError paths; 0/NULL skips it. */
	return EM_ASM_INT ({
		var p = $2 < 0 ? $2 + 4294967296 : $2;
		var eb = $4 < 0 ? $4 + 4294967296 : $4;
		var op = $6 < 0 ? $6 + 4294967296 : $6;
		var t0 = performance.now ();
		try {
			var b = HEAPU8.slice (p, p + $3);
			var inst = new WebAssembly.Instance (new WebAssembly.Module (b), { m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports && wasmExports["__cpp_exception"] } });
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			if ($0 < 0) {
				wasmTable.set ($1, inst.exports.t); /* interp-entry thunk: scalar -> interpreter */
			} else {
				wasmTable.set ($0, inst.exports.e); /* entry thunk: interp entry */
				wasmTable.set ($1, inst.exports.f); /* scalar method: call_indirect target */
			}
			return 1;
		} catch (e) {
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			if (eb) stringToUTF8 ("" + e, eb, $5); /* surface the WebAssembly error to the caller */
			return 0;
		}
	}, e_slot, f_slot, (int) (intptr_t) bytes, len, (int) (intptr_t) errbuf, errcap, (int) (intptr_t) out_ms);
}

/* Global registry of every JITted method's {slots, cached bytes}. Because the wasm function table is
 * per-thread for dynamic entries, AND JITted methods call each other directly via f-slot
 * call_indirect, every thread that runs any JITted code must have ALL JITted methods instantiated in
 * its own table — not just the ones the interpreter invoked on it. mono_wasm_jit_sync_thread()
 * (called on the interp's JIT-invoke path) brings the calling thread up to date: it instantiates any
 * methods registered since this thread last synced. Callees are always registered before callers
 * (the direct-call lowering bails if the callee isn't JITted yet), so syncing to the current
 * generation before invoking a method guarantees its f-slot callees are present. */
#define WJ_REG_MAX 8192
static struct { int e, f, len; void *bytes; } wj_reg [WJ_REG_MAX];
static volatile int wj_reg_n = 0;
/* serialize registry appends + per-thread sync; the loader lock is global + always inited at startup */
extern void mono_loader_lock (void);
extern void mono_loader_unlock (void);

void
mono_wasm_jit_register (int e_slot, int f_slot, void *bytes, int len)
{
	mono_loader_lock ();
	if (wj_reg_n < WJ_REG_MAX) {
		wj_reg [wj_reg_n].e = e_slot; wj_reg [wj_reg_n].f = f_slot; wj_reg [wj_reg_n].bytes = bytes; wj_reg [wj_reg_n].len = len;
		mono_memory_barrier ();
		wj_reg_n++;
	}
	mono_loader_unlock ();
}

void
mono_wasm_jit_sync_thread (void)
{
	static __thread int synced = 0;
	if (synced >= wj_reg_n) /* fast path: this thread is up to date */
		return;
	if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VSYNC_WORK);   /* slow path: this call instantiates >=1 module */
	mono_loader_lock ();
	while (synced < wj_reg_n) {
		char eb [192]; eb [0] = 0;
		double ms = 0;
		if (!mono_wasm_jit_instantiate_local (wj_reg [synced].e, wj_reg [synced].f, wj_reg [synced].bytes, wj_reg [synced].len, eb, (int) sizeof (eb), &ms)) {
			/* A module that instantiated fine on the COMPILING thread failed here on another thread:
			 * names the corruption (e.g. magic-word/type) + which slot, so we can tell a byte-corruption
			 * (race) apart from a thread-local structural issue. Slot stays a placeholder -> interp. */
			if (mono_wasm_jit_stats) { char b [256]; snprintf (b, sizeof b, "WASM_JIT_SYNC_FAIL e=%d f=%d len=%d : %s", wj_reg [synced].e, wj_reg [synced].f, wj_reg [synced].len, eb); mono_wasm_jit_log_main (b); }
		}
		/* per-thread table-sync instantiation is real compile wall-cost too — fold it into the same timer */
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_add (WJC_ELAPSED_INSTANTIATION, (gint64) (ms * 1000.0));
		synced++;
	}
	mono_loader_unlock ();
}


/*
 * GC-safe object references for JITted methods.
 *
 * The JIT keeps vregs in wasm locals (registers) for speed, but wasm locals are NOT GC-scanned (the
 * GC scans the interp stack precisely via interp_mark_stack, never the native/wasm-locals state). So
 * an object reference held in a wasm local across a GC point (an allocation in a residual callee, a
 * loop safepoint, ...) would be collected or moved out from under the JITted method -> dangling ptr.
 *
 * Fix: each JITted method's REFERENCE vregs (cfg->vreg_is_ref) live in a per-thread shadow stack in
 * linear memory that IS a GC root, instead of in wasm locals. Non-ref vregs (ints/floats/unmanaged
 * pointers) stay in fast wasm locals. The region is registered once per thread as a CONSERVATIVE
 * pinning root (MONO_GC_DESCRIPTOR_NULL): the GC pins whatever object each live slot points at (so it
 * stays valid and isn't moved); leave() zeroes freed slots so popped frames pin nothing.
 *
 * A method does: base = ref_enter(N) at entry (reserve N ref slots), accesses ref vreg i at base[i],
 * ref_leave(base) at every exit. The slot addresses are base-relative (base is per-thread, fetched at
 * runtime — a __thread region address can't be baked into the shared module).
 */
#define WJ_REFSTACK_SLOTS (64 * 1024)   /* 256KB/thread on wasm32; ~13k frames deep — the C stack dies first */
static __thread MonoObject **wj_ref_base = NULL;
static __thread MonoObject **wj_ref_sp   = NULL;
static __thread MonoObject **wj_ref_end  = NULL;

/* Reserve n reference slots for a JITted method's frame; returns the frame base (slot 0). Lazily
 * allocates + GC-registers this thread's shadow stack on first use. */
MonoObject **
mono_wasm_jit_ref_enter (int n)
{
	MonoObject **base;
	if (G_UNLIKELY (!wj_ref_base)) {
		wj_ref_base = (MonoObject **) g_malloc0 (WJ_REFSTACK_SLOTS * sizeof (MonoObject *));
		wj_ref_sp = wj_ref_base;
		wj_ref_end = wj_ref_base + WJ_REFSTACK_SLOTS;
		mono_gc_register_root ((char *) wj_ref_base, WJ_REFSTACK_SLOTS * sizeof (MonoObject *),
			MONO_GC_DESCRIPTOR_NULL, MONO_ROOT_SOURCE_THREAD_STATIC, NULL, "wasm-jit ref shadow stack");
	}
	base = wj_ref_sp;
	if (G_UNLIKELY (base + n > wj_ref_end))
		return base; /* pathological depth (C stack would overflow first); don't bump, accept overlap */
	wj_ref_sp = base + n;
	{
		/* High-water mark of the ref shadow-stack depth (slots), for leak diagnosis: if this climbs
		 * unboundedly toward WJ_REFSTACK_SLOTS over a run, frames are leaking (a wasm-EH unwind skipped
		 * EMIT_REF_LEAVE); if it stays small/bounded, enter/leave is balanced. Racy global max — fine. */
		int d = (int) (wj_ref_sp - wj_ref_base);
		mono_wasm_jit_max (WJC_REF_HWM, d);
	}
	return base;
}

/* Pop a JITted method's ref frame: zero its slots (so the conservative scan pins nothing stale) and
 * restore the stack pointer. */
void
mono_wasm_jit_ref_leave (MonoObject **base)
{
	MonoObject **p = base;
	while (p < wj_ref_sp)
		*p++ = NULL;
	wj_ref_sp = base;
}

/* Current ref-shadow-stack SP (for the AOT-style-EH boundary: save it before invoking a JITted method,
 * and on a caught C++ unwind restore via mono_wasm_jit_ref_leave(saved) — the unwound JITted frames
 * skipped their own ref_leave, so their slots must be zeroed + the SP rewound here). NULL before first
 * use (no JITted frame has run on this thread yet -> nothing to restore). */
MonoObject **
mono_wasm_jit_ref_sp_save (void)
{
	return wj_ref_sp;
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

/*
 * Runtime WebAssembly JIT backend entry point. Sibling of mono_llvm_emit_method /
 * mono_codegen, selected via COMPILE_WASM(cfg) in mini_method_compile. Consumes
 * pre-regalloc vreg IR and emits a self-contained wasm module for the method.
 *
 * Phase 1 (this cut): straight-line int/long/double leaf methods only. Anything
 * with branches/loops/calls/EH or an unsupported opcode/type bails (cfg->native_code
 * left NULL) so the caller falls back to the interpreter. For bring-up it dumps the
 * emitted module as hex so it can be validated offline.
 */

/* 0 = unknown/unsupported (none of the valtypes are 0) */
static WasmValtype
wasm_valtype_of_type (MonoType *t)
{
	t = mini_get_underlying_type (t);
	if (m_type_is_byref (t))
		return WASM_I32;
	switch (t->type) {
	case MONO_TYPE_VOID:
		return WASM_VOID;
	case MONO_TYPE_BOOLEAN: case MONO_TYPE_CHAR:
	case MONO_TYPE_I1: case MONO_TYPE_U1:
	case MONO_TYPE_I2: case MONO_TYPE_U2:
	case MONO_TYPE_I4: case MONO_TYPE_U4:
	case MONO_TYPE_I: case MONO_TYPE_U:
	case MONO_TYPE_PTR: case MONO_TYPE_FNPTR:
	case MONO_TYPE_OBJECT: case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS: case MONO_TYPE_SZARRAY: case MONO_TYPE_ARRAY:
		return WASM_I32;
	case MONO_TYPE_I8: case MONO_TYPE_U8:
		return WASM_I64;
	case MONO_TYPE_R4:
		return WASM_F32;
	case MONO_TYPE_R8:
		return WASM_F64;
	default:
		return 0;
	}
}

/* result valtype produced into ins->dreg by a supported opcode, or 0 */
static WasmValtype
wasm_valtype_of_opcode (int opcode)
{
	switch (opcode) {
	case OP_ICONST: case OP_MOVE: case OP_AOTCONST:   /* AOTCONST bakes a resolved pointer -> i32 on wasm32 */
	case OP_GET_EX_OBJ:   /* caught exception object -> i32 (ref) on wasm32 */
	case OP_IADD: case OP_ISUB: case OP_IMUL:
	case OP_IDIV: case OP_IDIV_UN: case OP_IREM: case OP_IREM_UN:
	case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN:
	case OP_IADD_IMM: case OP_ISUB_IMM: case OP_IMUL_IMM:
	case OP_IAND_IMM: case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_IDIV_IMM: case OP_IDIV_UN_IMM: case OP_IREM_IMM: case OP_IREM_UN_IMM:
	case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADU1_MEMBASE: case OP_LOADI1_MEMBASE: case OP_LOADU2_MEMBASE: case OP_LOADI2_MEMBASE:
	case OP_ICONV_TO_U1: case OP_ICONV_TO_I1: case OP_ICONV_TO_U2: case OP_ICONV_TO_I2:
	case OP_AND_IMM:
	/* native-int (size_t) IMM ops — i32 on wasm32, lowered identically to the OP_I*_IMM variants */
	case OP_ADD_IMM: case OP_SUB_IMM: case OP_MUL_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_INEG: case OP_INOT:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:   /* i64 -> i32 (i32.wrap_i64) */
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:  /* i64 setcc -> i32 0/1 */
	case OP_MOVE_F_TO_I4:  /* f32 bits -> i32 (reinterpret) */
	/* float/r4 compare-to-i32 (setcc): standalone, produce a 0/1 i32 result */
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
		return WASM_I32;
	case OP_ICONV_TO_I8: case OP_ICONV_TO_U8:
	case OP_LOADI8_MEMBASE:
		return WASM_I64;
	case OP_LOADR8_MEMBASE:
		return WASM_F64;
	case OP_LOADR4_MEMBASE:
		return WASM_F32;
	case OP_I8CONST: case OP_LMOVE:
	case OP_LADD: case OP_LSUB: case OP_LMUL:
	case OP_LAND: case OP_LOR: case OP_LXOR:
	case OP_LSHL: case OP_LSHR: case OP_LSHR_UN:
	case OP_LADD_IMM: case OP_LSUB_IMM: case OP_LMUL_IMM:
	case OP_LAND_IMM: case OP_LOR_IMM: case OP_LXOR_IMM:
	case OP_LSHL_IMM: case OP_LSHR_IMM: case OP_LSHR_UN_IMM:
	case OP_LNOT:
	case OP_MOVE_F_TO_I8:  /* f64 bits -> i64 (reinterpret) */
		return WASM_I64;
	case OP_R8CONST: case OP_FMOVE:
	case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
	case OP_ICONV_TO_R8: case OP_RCONV_TO_R8:
	case OP_LCONV_TO_R8:   /* i64 -> f64 */
	case OP_MOVE_I8_TO_F:  /* i64 bits -> f64 (reinterpret) */
		return WASM_F64;
	case OP_R4CONST: case OP_RMOVE:
	case OP_FCONV_TO_R4:
	case OP_ICONV_TO_R4: case OP_RCONV_TO_R4:
	case OP_LCONV_TO_R4:   /* i64 -> f32 */
	case OP_MOVE_I4_TO_F:  /* i32 bits -> f32 (reinterpret) */
	case OP_RADD: case OP_RSUB: case OP_RMUL: case OP_RDIV: case OP_RNEG:
		return WASM_F32;
	default:
		return 0;
	}
}

static int
wasm_valtype_group (WasmValtype t)
{
	switch (t) {
	case WASM_I32: return 0;
	case WASM_I64: return 1;
	case WASM_F32: return 2;
	case WASM_F64: return 3;
	default: return -1;
	}
}

/* Per-compile vreg access context: non-reference vregs live in wasm locals (li[]); reference vregs
 * (refslot[vreg] >= 0) live in the GC-scanned ref shadow stack at refbase + slot*4. refbase/rtmp are
 * wasm i32 locals (the frame base address + a scratch for ref stores). refslot is NULL when the ref
 * shadow stack is disabled (offline dump), so refs fall back to locals. */
typedef struct {
	int *li;
	int nvreg;
	int *refslot;
	int refbase;   /* wasm local: ref-frame base address */
	int rtmp;      /* wasm local: scratch i32 for ref stores */
} WjCtx;

static gboolean
wasm_ld (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->refslot && c->refslot [vreg] >= 0) {
		/* reference vreg: load from the GC-scanned ref shadow stack */
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, (guint32) (c->refslot [vreg] * 4));
		return TRUE;
	}
	if (c->li [vreg] < 0)
		return FALSE;
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->li [vreg]);
	return TRUE;
}

static gboolean
wasm_st (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->refslot && c->refslot [vreg] >= 0) {
		/* reference vreg: store the value (already on the wasm stack) to the ref shadow stack.
		 * i32.store wants [addr, val] but val is on top, so stash it via rtmp then push addr+val. */
		wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->rtmp);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->rtmp);
		wasm_op (b, WASM_OP_I32_STORE); wasm_memarg (b, 2, (guint32) (c->refslot [vreg] * 4));
		return TRUE;
	}
	if (c->li [vreg] < 0)
		return FALSE;
	wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->li [vreg]);
	return TRUE;
}

static gboolean
functype_eq (const WasmFuncType *a, const WasmFuncType *b)
{
	guint32 i;
	if (a->nparams != b->nparams || a->ret != b->ret)
		return FALSE;
	for (i = 0; i < a->nparams; ++i)
		if (a->params [i] != b->params [i])
			return FALSE;
	return TRUE;
}

/* Opcode -> name, for the ref-safety IR dump (MONO_WASM_JIT_REFDIAG) and the bail message. mono_inst_name
 * is compiled out under DISABLE_LOGGING, so re-include mini-ops.h with our own MINI_OP to build a private
 * name table. (Defined unconditionally — also used by the WASM_JIT_BAIL print, incl. the offline dump.) */
static const char *
wj_opname (int op)
{
	switch (op) {
#undef MINI_OP
#undef MINI_OP3
#define MINI_OP(a,b,dest,src1,src2) case a: return b;
#define MINI_OP3(a,b,dest,src1,src2,src3) case a: return b;
#include "mini-ops.h"
#undef MINI_OP
#undef MINI_OP3
	default: return "?";
	}
}

/* wasm float comparison op for width (is_f32 ? f32 : f64) and kind: 0=EQ 1=NE 2=LT 3=GT 4=LE 5=GE.
 * wasm float compares are ORDERED except NE (which is unordered: true on NaN). _UN ordered-relation
 * variants (lt_un etc.) are emitted by the caller as the complementary ordered op + i32.eqz. */
static WasmOpcode
wj_fcmp_op (gboolean is_f32, int kind)
{
	switch (kind) {
	case 0:  return is_f32 ? WASM_OP_F32_EQ : WASM_OP_F64_EQ;
	case 1:  return is_f32 ? WASM_OP_F32_NE : WASM_OP_F64_NE;
	case 2:  return is_f32 ? WASM_OP_F32_LT : WASM_OP_F64_LT;
	case 3:  return is_f32 ? WASM_OP_F32_GT : WASM_OP_F64_GT;
	case 4:  return is_f32 ? WASM_OP_F32_LE : WASM_OP_F64_LE;
	default: return is_f32 ? WASM_OP_F32_GE : WASM_OP_F64_GE;
	}
}

void
mono_wasm_emit_method (MonoCompile *cfg)
{
#ifdef HOST_BROWSER
	if (mono_wasm_jit_verbose >= 3) { char b [160]; snprintf (b, sizeof b, "WASM_JIT_EMIT_ENTER %s opt=0x%x", cfg->method ? cfg->method->name : "?", (unsigned) cfg->opt); mono_wasm_jit_log_main (b); }
#endif
	/* Compile-time accounting (Part 2): wj_gen_t0 = bytecode-GENERATION start (100ns ticks); wj_inst_ms =
	 * the JS WebAssembly.Module/Instance time for THIS compile (written by mono_wasm_jit_instantiate_local).
	 * At `done:` we fold (elapsed - instantiate) into WJC_ELAPSED_GENERATION so the two halves don't overlap. */
	gint64 wj_gen_t0 = G_UNLIKELY (mono_wasm_jit_stats) ? mono_100ns_ticks () : 0;
	double wj_inst_ms = 0;
	MonoMethodSignature *sig = mono_method_signature_internal (cfg->method);
	int nvreg = cfg->next_vreg;
	int nargs = sig->hasthis + sig->param_count;
	WasmValtype *vt = (WasmValtype *) mono_mempool_alloc0 (cfg->mempool, sizeof (WasmValtype) * nvreg);
	int *li = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
	int *refslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* ref vreg -> shadow-stack slot, else -1 */
	WasmValtype *param_types = (WasmValtype *) mono_mempool_alloc0 (cfg->mempool, sizeof (WasmValtype) * (nargs ? nargs : 1));
	WasmValtype ret_vt;
	WasmBuf body, out;
	MonoBasicBlock *bb;
	MonoInst *ins;
	int i, cnt [4] = { 0, 0, 0, 0 }, base [4], run [4] = { 0, 0, 0, 0 };
	WasmLocalGroup groups [4];
	int dispatch_idx = 0, N = 0;
	int scratch_idx G_GNUC_UNUSED = 0; /* i32 local holding the per-thread interp-residual scratch ptr */
	int refbase_idx = 0, rtmp_idx = 0; /* i32 locals: ref-frame base addr + scratch for ref stores */
	int vc_fslot_idx = 0;              /* i32 local: inline virtual-IC fast-path resolved f-slot */
	int vc_ic_idx = 0;                 /* i64 local: inline virtual-IC fast-path IC value (vtable|imethod<<32) */
	int eh_exc_idx = 0, eh_h_idx = 0;  /* i32 locals: in-method EH catch landing pad — saved C++ exc ptr + dispatch result */
	int finally_ind_idx = 0;           /* i32 local: in-method finally indicator (continuation bb idx, or -1 = rethrow) */
	gboolean eh_has_finally = FALSE;   /* TRUE: method has >=1 FINALLY clause (milestone 2c) */
	WasmEhTable *eh_table = NULL;      /* in-method EH clause table (built below, baked into the catch landing pad) */
	gboolean eh_on = FALSE;            /* TRUE: emit the in-method try/catch wrapper for this method */
	int eh_dispatch_ti = -1, eh_endcatch_ti = -1;  /* functype indices: (i32,i32)->i32 dispatch + ()->void end_catch */
	int nrefslots = 0;                 /* number of reference vregs routed to the GC ref shadow stack */
	int enter_ti = -1, leave_ti = -1;  /* functype indices for mono_wasm_jit_ref_enter/leave */
	WjCtx lc;
	int *bbidx;
	WasmFuncType extra_types [32]; /* callee functypes for call_indirect, after T0/T1 */
	int nextra = 0;
	gboolean uses_calls = FALSE;
	gboolean uses_eh_tag = FALSE;   /* inline-AOT-call try/catch -> import the C++ exception tag x.e */
	int eh_type_idx = -1;           /* type index of (i32)->void (the catch handler + the tag) */
	const char *fail = NULL;
	int fail_op = -1;
	char *mname = mono_method_get_full_name (cfg->method);

	wasm_buf_init (&body);

	/* Eligibility gates (critical for auto-JIT robustness on real code like Minecraft, where the
	 * emitter is fed thousands of method shapes): bail to the interpreter for features the wasm
	 * backend doesn't lower yet — exception-handling clauses (no try/catch emission) and
	 * generic-shared methods (rgctx access the non-llvmonly path doesn't handle). Unknown opcodes
	 * still bail individually via the lowering switch's default case. */
#ifndef HOST_BROWSER
	/* EH ground-truth dump (MONO_WASM_JIT_DUMP_IR=<substr>): print clauses + per-bb region + opcode stream
	 * for clause-bearing methods. OFFLINE-CROSS ONLY: mono_inst_name returns a string in the cross but an
	 * int under the runtime's DISABLE_LOGGING, and the dump is only ever driven by the offline MONO_WASM_JIT_DUMP_IR
	 * env — so guard it out of the browser runtime build entirely (was breaking the runtime compile). */
	{ char *_df = g_getenv ("MONO_WASM_JIT_DUMP_IR");   /* read directly: the env-init fn isn't run in the offline cross */
	  if (_df && *_df && cfg->header && cfg->header->num_clauses > 0 && mname && strstr (mname, _df)) {
		guint _ci; MonoBasicBlock *_db; MonoInst *_di;
		g_printerr ("[eh-ir] === %s nclauses=%u ===\n", mname, cfg->header->num_clauses);
		for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			MonoExceptionClause *_c = &cfg->header->clauses [_ci];
			g_printerr ("[eh-ir] clause%u flags=%d(0=catch,2=finally,4=fault) try=[%u,%u) handler=[%u,%u)\n",
				_ci, _c->flags, _c->try_offset, _c->try_offset + _c->try_len, _c->handler_offset, _c->handler_offset + _c->handler_len);
		}
		for (_db = cfg->bb_entry; _db; _db = _db->next_bb) {
			g_printerr ("[eh-ir] bb%d region=0x%x:\n", _db->block_num, _db->region);
			MONO_BB_FOR_EACH_INS (_db, _di) {
				const char *_nm = mono_inst_name (_di->opcode);
				if ((_di->opcode == OP_CALL_HANDLER || _di->opcode == OP_BR) && _di->inst_target_bb)
					g_printerr ("[eh-ir]   %s -> bb%d\n", _nm, _di->inst_target_bb->block_num);
				else if (_di->opcode == OP_AOTCONST)
					g_printerr ("[eh-ir]   %s patch_type=%d p0=%p\n", _nm, (int) (gsize) _di->inst_p1, _di->inst_p0);
				else
					g_printerr ("[eh-ir]   %s\n", _nm);
			}
		}
	  }
	  g_free (_df);
	}
#endif /* !HOST_BROWSER (offline IR dump) */
	if (cfg->header && cfg->header->num_clauses > 0) {
		char *_eh = g_getenv ("MONO_WASM_JIT_EH"); gboolean _ehon = _eh && *_eh && *_eh != '0'; g_free (_eh);
		if (!_ehon) { fail = "has EH clauses"; goto done; }   /* MONO_WASM_JIT_EH=1: attempt the in-method EH lowering */
		/* bisection: MONO_WASM_JIT_EH_ONLY=<substr> emits the in-method wrapper only for methods whose full
		 * name contains <substr> (others bail like EH=0) — to pin down which EH method corrupts world load. */
		{ char *_o = g_getenv ("MONO_WASM_JIT_EH_ONLY"); gboolean _skip = _o && *_o && (!mname || !strstr (mname, _o)); if (_o) g_free (_o); if (_skip) { fail = "eh-only filter"; goto done; } }
		{ extern int mono_wasm_jit_cppeh; guint _ci; gboolean _cppeh = mono_wasm_jit_cppeh;
		  { char *_cv = g_getenv ("MONO_WASM_JIT_CPPEH"); if (_cv) { _cppeh = (*_cv && *_cv != '0'); g_free (_cv); } }   /* env fallback so the offline cross (env-init not run) can validate */
		  if (!_cppeh) { fail = "EH needs cppeh"; goto done; }   /* in-method clauses propagate via C++/wasm-EH */
		  /* increment 2a: catch (NONE). 2c: + finally (FINALLY, incl. Java try-with-resources / try{}finally{}).
		   * filter (1) / fault (4) still bail — handled in a later increment. */
		  for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			  int _f = cfg->header->clauses [_ci].flags;
			  if (_f == MONO_EXCEPTION_CLAUSE_FINALLY) {
				  /* MONO_WASM_JIT_FINALLY=0 bails finally methods (catch-only) — bisects catch-il_state-at-scale
				   * vs the new try/finally codegen without a rebuild once this env check is compiled in. */
				  char *_fv = g_getenv ("MONO_WASM_JIT_FINALLY"); gboolean _foff = _fv && *_fv == '0'; if (_fv) g_free (_fv);
				  if (_foff) { fail = "finally (gated off)"; goto done; }
				  eh_has_finally = TRUE;
			  }
			  else if (_f != MONO_EXCEPTION_CLAUSE_NONE) { fail = "EH clause kind (catch/finally only)"; goto done; }
		  }
		  /* the prologue il_state island has a fixed data[] (WJ_ISLAND_DATA=256 in interp.c) that the GC
		   * scans for args+locals; bail EH methods that exceed it so the GC never reads past the buffer. */
		  { MonoMethodSignature *_s = mono_method_signature_internal (cfg->method);
		    int _nd = (_s && _s->hasthis ? 1 : 0) + (_s ? (int) _s->param_count : 0) + (int) cfg->header->num_locals;
		    if (_nd > 256) { fail = "EH method exceeds il_state data cap (256 args+locals)"; goto done; } }
		  eh_on = TRUE;   /* emit the outer-loop+try wrapper + catch landing pad (table built after bbidx) */
		}
	}
	if (cfg->gshared) { fail = "gshared method"; goto done; }
	/* A call here passes the generic-sharing context in MONO_ARCH_RGCTX_REG (an out-arg register set by
	 * set_rgctx_arg) — e.g. this concrete method calls a gsharedvt/shared callee like
	 * Array.GetGenericValueImpl<T>. The wasm backend does NOT forward that register (neither the direct
	 * f-slot call nor the residual call_interp path carries it), so the callee would run with a garbage
	 * rgctx -> its gsharedvt mrgctx init populates a wrong/empty GOT slot -> a later call_indirect of
	 * that slot traps "null function". Bail the whole method to the interpreter, which forwards the
	 * rgctx correctly. (Shared callees never get an f-slot anyway — they bail just above — so the call
	 * would always take the rgctx-less residual; bailing the caller is the only safe option until we
	 * marshal the rgctx through.)
	 *
	 * MONO_WASM_JIT_RGCTX (default ON) lifts this: instead of bailing the whole method, each rgctx call
	 * is routed per-site to the interp residual (mono_wasm_jit_call_interp), which derives the generic
	 * context from the concrete inflated call->method and is rgctx-correct (interp_entry, and the AOT
	 * fastpath's do_jit_call-via-gsharedvt_out wrapper, both handle it). The INLINE_AOT direct-body path is
	 * skipped for rgctx calls (it would feed a NULL rgctx — cinfo->extra_arg is only set under mono_llvm_only);
	 * indirect/virtual rgctx calls still bail. This unblocks IKVM EH methods whose catch block calls the
	 * generic ExceptionHelper.MapException<T> (the #1 render-path blocker after the EH gate). */
	{ extern int mono_wasm_jit_rgctx;
	  if (!mono_wasm_jit_rgctx && cfg->uses_rgctx_reg) { fail = "uses rgctx reg"; goto done; } }

	for (i = 0; i < nvreg; ++i) {
		li [i] = -1;
		refslot [i] = -1;
	}

	/* argument valtypes -> wasm params */
	for (i = 0; i < nargs; ++i) {
		WasmValtype pt = (i == 0 && sig->hasthis) ? WASM_I32 : wasm_valtype_of_type (sig->params [i - sig->hasthis]);
		if (pt == 0 || pt == WASM_VOID) { fail = "arg type"; goto done; }
		param_types [i] = pt;
		vt [cfg->args [i]->dreg] = pt;
		li [cfg->args [i]->dreg] = i;
	}

	ret_vt = wasm_valtype_of_type (sig->ret);
	if (sig->ret->type != MONO_TYPE_VOID && (ret_vt == 0 || ret_vt == WASM_VOID)) { fail = "ret type"; goto done; }

	/* infer vreg valtypes from defining opcodes */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MONO_BB_FOR_EACH_INS (bb, ins) {
			WasmValtype rt = wasm_valtype_of_opcode (ins->opcode);
			if (!rt && (ins->opcode == OP_CALL || ins->opcode == OP_FCALL || ins->opcode == OP_LCALL || ins->opcode == OP_RCALL
					|| ins->opcode == OP_CALL_REG || ins->opcode == OP_FCALL_REG || ins->opcode == OP_LCALL_REG || ins->opcode == OP_RCALL_REG
					|| ins->opcode == OP_CALL_MEMBASE || ins->opcode == OP_FCALL_MEMBASE || ins->opcode == OP_LCALL_MEMBASE || ins->opcode == OP_RCALL_MEMBASE)) {
				MonoCallInst *c = (MonoCallInst *) ins;
				if (c->signature && c->signature->ret->type != MONO_TYPE_VOID)
					rt = wasm_valtype_of_type (c->signature->ret);
			}
			if (rt && ins->dreg >= 0 && ins->dreg < nvreg && li [ins->dreg] < 0)
				vt [ins->dreg] = rt;
		}
	}

	/* assign locals for non-arg typed vregs, grouped by type */
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0)
			continue;
		g = wasm_valtype_group (vt [i]);
		if (g < 0) continue;
		cnt [g]++;
	}
	/* reserve one extra i32 local at the end of the i32 group for the dispatch index ($blk) */
	dispatch_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* reserve one more i32 local for the interp-residual scratch-buffer pointer (used by the
	 * "callee not jitted" path below to call_indirect mono_wasm_jit_call_interp; dead in methods
	 * with no such call, which is a harmless unused declared local) */
	scratch_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* reserve two more i32 locals: the GC ref shadow-stack frame base, and a scratch for ref stores */
	refbase_idx = nargs + cnt [0];
	cnt [0] += 1;
	rtmp_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* one more i32 local for the inline virtual-IC fast path's resolved f-slot (dead in methods with no
	 * virtual call — a harmless unused declared local) */
	vc_fslot_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* two i32 locals for the in-method EH catch landing pad (dead in methods with no clauses) */
	eh_exc_idx = nargs + cnt [0];
	cnt [0] += 1;
	eh_h_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the in-method finally indicator (dead in non-finally methods): OP_CALL_HANDLER sets
	 * it to the continuation bb index; the catch landing pad sets it to -1; OP_ENDFINALLY branches on it. */
	finally_ind_idx = nargs + cnt [0];
	cnt [0] += 1;
	base [0] = nargs;
	base [1] = base [0] + cnt [0];
	/* dedicated i64 local at the end of the i64 group for the inline virtual-IC value (atomically loaded) */
	vc_ic_idx = base [1] + cnt [1];
	cnt [1] += 1;
	base [2] = base [1] + cnt [1];
	base [3] = base [2] + cnt [2];
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0)
			continue;
		g = wasm_valtype_group (vt [i]);
		if (g < 0) continue;
		li [i] = base [g] + run [g]++;
	}
	groups [0].type = WASM_I32; groups [0].count = cnt [0];
	groups [1].type = WASM_I64; groups [1].count = cnt [1];
	groups [2].type = WASM_F32; groups [2].count = cnt [2];
	groups [3].type = WASM_F64; groups [3].count = cnt [3];

	lc.li = li; lc.nvreg = nvreg; lc.refslot = NULL; lc.refbase = refbase_idx; lc.rtmp = rtmp_idx;
#ifdef HOST_BROWSER
	/* Route managed-reference vregs to the GC-scanned ref shadow stack instead of (GC-invisible) wasm
	 * locals: a reference held in a wasm local across a GC point (a residual interp call, which can
	 * allocate and trigger a moving collection) would be moved/collected out from under the JITted
	 * method -> dangling pointer.
	 *
	 * cfg->vreg_is_ref (mono's per-vreg reference bit) only covers vregs created via mono_compile_create_var
	 * — named locals and arguments — NOT the temporary vregs that hold call results, field loads, etc. Those
	 * temporaries routinely hold a live object across a residual (e.g. `s = Intern(x); i = indexOf(s, c)`),
	 * so we must track them too. Infer ref-ness conservatively from the defining instruction: a call that
	 * returns a reference, any pointer-sized memory load (OP_LOAD_MEMBASE is what the front-end emits for a
	 * reference field; typed int/float loads can't be refs), and moves that propagate one. The shadow stack
	 * is a conservative PINNING root, so over-marking a non-ref i32 is harmless — it just pins whatever the
	 * value happens to point at. (Byrefs/managed pointers are a separate concern; residual byref args/returns
	 * already bail above.) */
	{
		gboolean *isref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		gboolean changed = TRUE;
		int pass;
		for (i = 0; i < nvreg; ++i)
			if (vreg_is_ref (cfg, i))
				isref [i] = TRUE;
		/* Explicitly mark reference-typed ARGUMENTS. mono_compile_create_var marks ref vregs via
		 * vreg_is_ref, but a reference-type class's `this` carries a byref-flagged this_arg, so
		 * mini_type_is_reference() returns FALSE for it and the `this` arg is never marked — leaving it
		 * in a GC-invisible wasm local. A method that forwards a ref arg into a residual then spills a
		 * stale pointer after the residual's GC moves the object. Mark them here so the prologue copies
		 * them into the GC ref shadow stack at entry. `this` of a reference type is a managed reference;
		 * `this` of a valuetype is an interior managed pointer (handled elsewhere), so skip it. */
		for (i = 0; i < nargs; ++i) {
			gboolean arg_is_ref;
			if (i == 0 && sig->hasthis)
				arg_is_ref = !m_class_is_valuetype (cfg->method->klass);
			else {
				MonoType *pt = sig->params [i - sig->hasthis];
				arg_is_ref = !m_type_is_byref (pt) && mini_type_is_reference (pt);
			}
			if (arg_is_ref) {
				int av = cfg->args [i]->dreg;
				if (av >= 0 && av < nvreg)
					isref [av] = TRUE;
			}
		}
		for (pass = 0; changed && pass <= nvreg; ++pass) {
			changed = FALSE;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int d = ins->dreg;
					if (d < 0 || d >= nvreg || isref [d])
						continue;
					switch (ins->opcode) {
					case OP_CALL: case OP_CALL_REG: case OP_CALL_MEMBASE: {
						MonoCallInst *c = (MonoCallInst *) ins;
						if (c->signature && mini_type_is_reference (c->signature->ret)) { isref [d] = TRUE; changed = TRUE; }
						break;
					}
					case OP_LOAD_MEMBASE:
						isref [d] = TRUE; changed = TRUE;
						break;
					case OP_ICONST: /* == OP_PCONST on wasm32: a STACK_OBJ constant is a baked managed-object
					                 * pointer (ldstr/typeof/folded GetType). The emitter loads it from the
					                 * GC-tracked literal table; mark the dreg so the loaded value lands on the
					                 * ref shadow stack and survives the next GC point while live. */
						if (ins->type == STACK_OBJ) { isref [d] = TRUE; changed = TRUE; }
						break;
					case OP_MOVE:
						if (ins->sreg1 >= 0 && ins->sreg1 < nvreg && isref [ins->sreg1]) { isref [d] = TRUE; changed = TRUE; }
						break;
					default:
						break;
					}
				}
			}
		}
		for (i = 0; i < nvreg; ++i)
			if (li [i] >= 0 && isref [i])
				refslot [i] = nrefslots++;
		if (nrefslots > 0)
			lc.refslot = refslot;

		/* REF-SAFETY IR DUMP (MONO_WASM_JIT_REFDIAG=<name,...>): for a watch-listed method, dump the full
		 * post-classification IR — each instruction's opcode name + dreg/sreg with their isref flag
		 * (R=ref shadow-stack slot, -=plain wasm local, .=n/a), and for calls the captured call_info arg
		 * vregs + flags + virt/ret_ref. A managed ref left in a plain local (-) that is live across a GC
		 * point (a call/virtual resolve) is the dangling-pointer corruptor; this shows the exact op that
		 * produced it (and whether a call receiver/arg is being spilled from an unclassified local). */
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name))) {
			MonoBasicBlock *bb2; MonoInst *ins2;
#define WJ_RF(v) (((v) >= 0 && (v) < nvreg) ? (isref [v] ? 'R' : '-') : '.')
			{ char b [160]; snprintf (b, sizeof b, "WASM_JIT_IR === %s nvreg=%d nrefslots=%d nargs=%d ===", cfg->method->name, nvreg, nrefslots, nargs); mono_wasm_jit_log_main (b); }
			for (bb2 = cfg->bb_entry; bb2; bb2 = bb2->next_bb)
				MONO_BB_FOR_EACH_INS (bb2, ins2) {
					char b [256]; int n;
					n = snprintf (b, sizeof b, "WASM_JIT_IR %-22s d=%d%c s1=%d%c s2=%d%c off=%d",
						wj_opname (ins2->opcode), ins2->dreg, WJ_RF (ins2->dreg), ins2->sreg1, WJ_RF (ins2->sreg1),
						ins2->sreg2, WJ_RF (ins2->sreg2), (int) ins2->inst_offset);
					switch (ins2->opcode) {
					case OP_CALL: case OP_CALL_REG: case OP_CALL_MEMBASE:
					case OP_VCALL: case OP_VCALL_REG: case OP_VCALL_MEMBASE:
					case OP_VCALL2: case OP_VCALL2_REG: case OP_VCALL2_MEMBASE:
					case OP_FCALL: case OP_FCALL_REG: case OP_FCALL_MEMBASE:
					case OP_LCALL: case OP_LCALL_REG: case OP_LCALL_MEMBASE:
					case OP_VOIDCALL: case OP_VOIDCALL_REG: case OP_VOIDCALL_MEMBASE: {
						MonoCallInst *c = (MonoCallInst *) ins2;
						if (c->signature && c->call_info) {
							int *ci = (int *) c->call_info;
							int na = (int) c->signature->param_count + (c->signature->hasthis ? 1 : 0);
							int k;
							n += snprintf (b + n, sizeof b - n, " virt=%d ret_ref=%d args[",
								c->method ? !!(c->method->flags & METHOD_ATTRIBUTE_VIRTUAL) : -1,
								mini_type_is_reference (c->signature->ret));
							for (k = 0; k < na && n < (int) sizeof b - 14; ++k)
								n += snprintf (b + n, sizeof b - n, "%s%d%c", k ? "," : "", ci [k], WJ_RF (ci [k]));
							n += snprintf (b + n, sizeof b - n, "]");
						} else if (c->signature) {
							n += snprintf (b + n, sizeof b - n, " (no call_info) ret_ref=%d", mini_type_is_reference (c->signature->ret));
						}
						break;
					}
					default: break;
					}
					mono_wasm_jit_log_main (b);
				}
#undef WJ_RF
		}
	}
#endif

#define BIN(WOP)     do { if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "bin sreg"; goto done; } wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "bin dreg"; goto done; } } while (0)
#define BINI32(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i32_const (&body, (gint32) ins->inst_imm); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
#define BINI64(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i64_const (&body, (gint64) ins->inst_imm); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
/* Long NON-SHIFT IMM ops: the 64-bit immediate lives in ins->inst_l (GET_LONG_IMM on wasm32), NOT
 * inst_imm (which on a 32-bit host can't hold it -> AND/OR masks came out 0). Shifts keep BINI64
 * (their small count is in inst_imm, per mini-llvm's shift_i8 path). */
#define BINI64L(WOP) do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i64_const (&body, (gint64) ins->inst_l); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
/* unary: dreg = WOP(sreg1) — conversions/sign-extends */
#define UN(WOP)      do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "un sreg"; goto done; } wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "un dreg"; goto done; } } while (0)
/* dreg = sreg1 & M (i32) — unsigned narrowing conversions (conv.u1/u2) */
#define MASK(M)      do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "mask sreg"; goto done; } wasm_i32_const (&body, (gint32)(M)); wasm_op (&body, WASM_OP_I32_AND); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "mask dreg"; goto done; } } while (0)
/* i64 shift: value (sreg1) is i64 but the shift count (sreg2) is an i32 vreg in mono IR, while wasm
 * i64.shl/shr_s/shr_u require BOTH operands i64 -> zero-extend an i32 count to i64 first. (Count is
 * 0..63, so the extension sign is irrelevant; i64.shr_u masks mod 64 anyway.) */
#define LSHIFT(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "lsh sreg"; goto done; } if (ins->sreg2 >= 0 && ins->sreg2 < nvreg && vt [ins->sreg2] == WASM_I32) wasm_op (&body, WASM_OP_I64_EXTEND_I32_U); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lsh dreg"; goto done; } } while (0)
/* push i32 (0/1) = (A <cmp> B) for float width IS_F32, comparison KIND (0=EQ..5=GE); NEG wraps in i32.eqz
 * (used to synthesize the unordered ordered-relation variants, e.g. lt_un = !(a>=b)). */
#define FCMP_PUSH(IS_F32, KIND, NEG, A, B) do { \
		if (!wasm_ld (&body, &lc, (A))) { fail = "fcmp a"; goto done; } \
		if (!wasm_ld (&body, &lc, (B))) { fail = "fcmp b"; goto done; } \
		wasm_op (&body, wj_fcmp_op ((IS_F32), (KIND))); \
		if (NEG) wasm_op (&body, WASM_OP_I32_EQZ); \
	} while (0)
/* Pop the GC ref shadow-stack frame (zero its slots + restore SP). Emit before EVERY return so a
 * popped JITted frame leaves nothing for the GC to scan. No-op for methods with no ref vregs. The
 * leave call consumes only refbase + returns void, so it leaves any return value on the stack. */
#ifdef HOST_BROWSER
#define EMIT_REF_LEAVE() do { if (nrefslots > 0) { \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
		wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_ref_leave); \
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) leave_ti); wasm_uleb (&body, 0); \
	} \
	if (eh_on) {   /* pop this EH method's il_state island (pushed by enter_island in the prologue) */ \
		extern void mono_wasm_jit_leave_island (void); \
		wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_leave_island); \
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_endcatch_ti); wasm_uleb (&body, 0); \
	} } while (0)
#else
#define EMIT_REF_LEAVE() do { } while (0)
#endif

/* Abort this method if an exception is unwinding after a DIRECT call into managed/interp code (f-slot
 * call_indirect, virtual-IC dispatch, callreg, throwing icall). The callee may have done a residual
 * interp call that threw: it returned a dummy and set the thread resume-state but couldn't signal
 * "threw" through its typed return. Mirror the residual's own threw-handling so the exception unwinds
 * through every JITted frame, not just the one entered from the interp e-thunk. (No-op offline.) */
#ifdef HOST_BROWSER
#define EMIT_PENDING_EXC_CHECK() do { \
		extern int mono_wasm_jit_cppeh; \
		if (mono_wasm_jit_cppeh) break;   /* AOT-style: a thrown callee C++-unwinds straight through; no per-call check (the perf win) */ \
		extern int mono_wasm_jit_pending_exception (void); \
		WasmFuncType _pt; int _pti = -1, _pk; \
		memset (&_pt, 0, sizeof (_pt)); _pt.ret = WASM_I32; _pt.nparams = 0; \
		for (_pk = 0; _pk < nextra; ++_pk) if (functype_eq (&extra_types [_pk], &_pt)) { _pti = 2 + _pk; break; } \
		if (_pti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = _pt; _pti = 2 + nextra++; } \
		uses_calls = TRUE; \
		wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_pending_exception); \
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) _pti); wasm_uleb (&body, 0); \
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
		switch (ret_vt) { \
		case WASM_I32: wasm_i32_const (&body, 0); break; \
		case WASM_I64: wasm_i64_const (&body, 0); break; \
		case WASM_F32: wasm_f32_const (&body, 0); break; \
		case WASM_F64: wasm_f64_const (&body, 0); break; \
		default: break; \
		} \
		EMIT_REF_LEAVE (); \
		wasm_op (&body, WASM_OP_RETURN); \
		wasm_op (&body, WASM_OP_END); \
	} while (0)
#else
#define EMIT_PENDING_EXC_CHECK() do { } while (0)
#endif

	/* dense bb indexing for the dispatch table */
	bbidx = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
	for (i = 0; i < (int) cfg->max_block_num + 1; ++i)
		bbidx [i] = -1;
	N = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		bbidx [bb->block_num] = N++;

	/* In-method EH: build the per-method clause table the catch landing pad's dispatch helper walks.
	 * Maps each dense bb -> its IL offset (for is-address-protected), and each clause -> {flags, try
	 * IL-range, handler bbidx, catch_class}. g_malloc'd so it outlives this compile (the JITted module
	 * bakes its address); a small per-EH-method leak. */
	if (eh_on) {
		guint _ci; int _d;
		eh_table = (WasmEhTable *) g_malloc0 (sizeof (WasmEhTable));
		eh_table->name = mname ? g_strdup (mname) : NULL;   /* diagnostics */
		eh_table->nbbs = N;
		eh_table->nclauses = (gint32) cfg->header->num_clauses;
		eh_table->il_offsets = (gint32 *) g_malloc0 (sizeof (gint32) * (N > 0 ? N : 1));
		eh_table->clauses = (WasmEhClause *) g_malloc0 (sizeof (WasmEhClause) * (eh_table->nclauses > 0 ? eh_table->nclauses : 1));
		for (_d = 0; _d < N; ++_d) eh_table->il_offsets [_d] = -1;
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			int _bd = bbidx [bb->block_num];
			if (_bd >= 0 && _bd < N)
				eh_table->il_offsets [_bd] = bb->cil_code ? (gint32) (bb->cil_code - cfg->header->code) : -1;
		}
		for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			MonoExceptionClause *_c = &cfg->header->clauses [_ci];
			WasmEhClause *_wc = &eh_table->clauses [_ci];
			MonoBasicBlock *_hb; int _hbb = -1;
			_wc->flags = (gint16) _c->flags;
			_wc->try_start = (gint32) _c->try_offset;
			_wc->try_len = (gint32) _c->try_len;
			_wc->catch_class = (_c->flags == MONO_EXCEPTION_CLAUSE_NONE) ? _c->data.catch_class : NULL;
			for (_hb = cfg->bb_entry; _hb; _hb = _hb->next_bb)
				if (_hb->cil_code && (gint32) (_hb->cil_code - cfg->header->code) == (gint32) _c->handler_offset) { _hbb = bbidx [_hb->block_num]; break; }
			if (_hbb < 0) { fail = "eh handler bb not found"; goto done; }
			_wc->handler_bbidx = _hbb;
		}
	}

	/* prologue: dispatch index ($blk) starts at the entry block (dense index 0) */
	wasm_i32_const (&body, 0);
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);

#ifdef HOST_BROWSER
	/* GC ref-frame prologue: reserve nrefslots slots on the per-thread ref shadow stack and copy any
	 * reference args from their wasm param locals into it, so all live refs are GC-visible. */
	if (nrefslots > 0) {
		WasmFuncType et, lt; int k2;
		memset (&et, 0, sizeof et); et.nparams = 1; et.params [0] = WASM_I32; et.ret = WASM_I32;   /* enter: (i32)->i32 */
		memset (&lt, 0, sizeof lt); lt.nparams = 1; lt.params [0] = WASM_I32; lt.ret = WASM_VOID;   /* leave: (i32)->void */
		for (k2 = 0; k2 < nextra; ++k2) {
			if (enter_ti < 0 && functype_eq (&extra_types [k2], &et)) enter_ti = 2 + k2;
			if (leave_ti < 0 && functype_eq (&extra_types [k2], &lt)) leave_ti = 2 + k2;
		}
		if (enter_ti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = et; enter_ti = 2 + nextra++; }
		if (leave_ti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = lt; leave_ti = 2 + nextra++; }
		uses_calls = TRUE;
		wasm_i32_const (&body, nrefslots);
		wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_ref_enter);
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) enter_ti); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
		for (i = 0; i < nargs; ++i) {
			int av = cfg->args [i]->dreg;
			if (av >= 0 && av < nvreg && refslot [av] >= 0) {
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);   /* addr */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) i);             /* incoming arg param */
				wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [av] * 4));
			}
		}
	}
#endif

	/*
	 * Structured-control dispatch (always correct for arbitrary CFGs):
	 *   loop { block^N { local.get $blk; br_table 0..N-1 } bb0; bb1; ... } unreachable
	 * br_table index i jumps just past block B_i, where bb_i's code lives. A branch
	 * to bb T sets $blk=idx(T) and br's back to the loop to re-dispatch.
	 *
	 * In-method EH (eh_on): wrap the whole dispatch in `loop $outer { try { <dispatch> } catch <x.e> {
	 * <clause-walk landing pad>; br $outer } }`. A C++/wasm-EH unwind out of any bb is caught here; the
	 * landing pad maps the throwing bb ($blk) -> a handler bb (set $blk + br $outer to re-dispatch) or
	 * rethrows. The inner dispatch + all GOTO depths are UNCHANGED — outer-loop/try sit ABOVE the inner
	 * loop, so a GOTO's depth to the inner loop is unaffected. */
	if (eh_on) {
		WasmFuncType _t; int _k;
		/* (i32)->void: the x.e tag type AND mono_jiterp_begin_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 1; _t.params [0] = WASM_I32; _t.ret = WASM_VOID;
		eh_type_idx = -1;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_type_idx = 2 + _k; break; }
		if (eh_type_idx < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_type_idx = 2 + nextra++; }
		/* (i32,i32)->i32: mono_wasm_jit_eh_dispatch (table, blk) -> handler bbidx or -1 */
		memset (&_t, 0, sizeof _t); _t.nparams = 2; _t.params [0] = WASM_I32; _t.params [1] = WASM_I32; _t.ret = WASM_I32;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_dispatch_ti = 2 + _k; break; }
		if (eh_dispatch_ti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_dispatch_ti = 2 + nextra++; }
		/* ()->void: mono_jiterp_end_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_endcatch_ti = 2 + _k; break; }
		if (eh_endcatch_ti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_endcatch_ti = 2 + nextra++; }
		uses_calls = TRUE;
		uses_eh_tag = TRUE;
		/* PROLOGUE: push this EH method's il_state island so mono's exception pass-1 sees it on ANY entry
		 * (interp->JIT boundary OR direct JIT->JIT f-slot call). method ptr baked; (i32)->void = eh_type_idx.
		 * Popped at every exit by EMIT_REF_LEAVE + the rethrow paths. */
		wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
#ifdef HOST_BROWSER
		{ extern void mono_wasm_jit_enter_island (MonoMethod *m); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_enter_island); }
#else
		wasm_i32_const (&body, 0x7ff6);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_type_idx); wasm_uleb (&body, 0);
		wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);   /* $outer: catch re-dispatch loop */
		wasm_op (&body, WASM_OP_TRY); wasm_u8 (&body, 0x40);    /* in-method EH try region (catches x.e) */
	}
	wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);
	for (i = 0; i < N; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dispatch_idx);
	wasm_op (&body, WASM_OP_BR_TABLE);
	wasm_uleb (&body, (guint32) N);
	for (i = 0; i < N; ++i)
		wasm_uleb (&body, (guint32) i);
	wasm_uleb (&body, 0); /* default -> block 0 */

#define GOTO(TBB, DEPTH) do { int _ti = bbidx [(TBB)->block_num]; if (_ti < 0) { fail = "bad branch target"; goto done; } wasm_i32_const (&body, _ti); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx); wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) (DEPTH)); } while (0)

	/* MONO_WASM_JIT_BBTRACE=<substr>: per-bb runtime $blk trace for a matching EH method (debug only). */
	gboolean eh_bbtrace = FALSE;
	{ char *_bbt = g_getenv ("MONO_WASM_JIT_BBTRACE"); if (_bbt) { eh_bbtrace = *_bbt && eh_on && mname && strstr (mname, _bbt); g_free (_bbt); } }
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int loop_depth = N - 1 - i;
		gboolean terminated = FALSE;
		int cmp_a = -1, cmp_b = -1;
		gint32 cmp_imm = 0;
		gboolean cmp_imm_mode = FALSE;
		WasmValtype cmp_float = 0; /* 0 = int compare; WASM_F32/WASM_F64 = float compare (set by OP_(F|R)COMPARE) */
		gboolean cmp_i64 = FALSE;  /* TRUE after OP_LCOMPARE: the fused branch/setcc uses i64 compare ops */
		WasmOpcode cmp_wop = WASM_OP_I32_EQ;

		wasm_op (&body, WASM_OP_END); /* close block B_i; bb_i code follows */

#ifdef HOST_BROWSER
		/* bbtrace: log this bb's dense index at runtime (reuses the (i32,i32)->i32 dispatch functype + drop) */
		if (eh_bbtrace) {
			extern int mono_wasm_jit_bbtrace_log (WasmEhTable *t, int blk);
			wasm_i32_const (&body, (gint32) (intptr_t) eh_table);
			wasm_i32_const (&body, i);
			wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_bbtrace_log);
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_dispatch_ti); wasm_uleb (&body, 0);
			wasm_op (&body, WASM_OP_DROP);
		}
		/* AOT-style in-method EH (milestone 2b): record this bb's IL offset into the active island il_state
		 * (pushed at the interp->JIT boundary) so mono's exception pass-1 matches THIS method's enclosing
		 * try and finds its catch as the NEAREST handler for a throwing AOT callee — stopping the walk here
		 * and running no outer frames' finally clauses. (i32)->void via eh_type_idx. EH methods only. */
		if (eh_on) {
			extern void mono_wasm_jit_set_il_offset (int il_offset);
			wasm_i32_const (&body, eh_table->il_offsets [i]);
			wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_set_il_offset);
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_type_idx); wasm_uleb (&body, 0);
		}
#endif

		MONO_BB_FOR_EACH_INS (bb, ins) {
			if (terminated) continue;   /* skip instrs after a bb terminator (e.g. the OP_BR following OP_CALL_HANDLER) */
			switch (ins->opcode) {
			case OP_NOP: case OP_IL_SEQ_POINT: case OP_SEQ_POINT:
			case OP_DUMMY_USE: case OP_NOT_REACHED: case OP_START_HANDLER:
				break;
			case OP_CALL_HANDLER: {
				/* milestone 2c: "call" a finally subroutine. We don't call — we record the continuation bb
				 * index in finally_ind, then GOTO the finally bb; OP_ENDFINALLY reads finally_ind to branch
				 * back. The leave's OP_BR (whose target IS the continuation) immediately follows and is
				 * skipped via the `terminated` guard above. Bail on a non-OP_BR continuation (a nested-finally
				 * leave emits multiple OP_CALL_HANDLERs in one bb — not supported yet). */
				int _cont; MonoInst *_nx = ins->next;
				while (_nx && (_nx->opcode == OP_NOP || _nx->opcode == OP_IL_SEQ_POINT || _nx->opcode == OP_SEQ_POINT)) _nx = _nx->next;
				if (!_nx || _nx->opcode != OP_BR || !_nx->inst_target_bb) { fail = "finally: complex call_handler continuation"; goto done; }
				_cont = bbidx [_nx->inst_target_bb->block_num];
				if (_cont < 0) { fail = "finally: bad continuation"; goto done; }
				wasm_i32_const (&body, _cont);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) finally_ind_idx);
				GOTO (ins->inst_target_bb, loop_depth);
				terminated = TRUE;
				break;
			}
			case OP_ENDFINALLY: {
				/* milestone 2c: end of a finally handler. finally_ind == -1 => the finally ran on the
				 * EXCEPTION path (the catch landing pad set it) -> re-raise the in-flight exception
				 * (mono_wasm_jit_endfinally_rethrow, ()->void, never returns). Otherwise finally_ind holds the
				 * continuation bb index of the OP_CALL_HANDLER that ran us (normal leave) -> GOTO it. */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) finally_ind_idx);
				wasm_i32_const (&body, -1);
				wasm_op (&body, WASM_OP_I32_EQ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);    /* exception path */
				/* the method escapes via this re-raise -> pop its il_state island first */
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_leave_island (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_leave_island); }
#else
				wasm_i32_const (&body, 0x7ff7);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_endcatch_ti); wasm_uleb (&body, 0);   /* leave_island ()->void */
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_endfinally_rethrow (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_endfinally_rethrow); }
#else
				wasm_i32_const (&body, 0x7ff5);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_endcatch_ti); wasm_uleb (&body, 0);   /* ()->void */
				wasm_op (&body, WASM_OP_UNREACHABLE);   /* the re-raise never returns */
				wasm_op (&body, WASM_OP_END);           /* end if */
				/* normal leave path: GOTO finally_ind (the continuation) */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) finally_ind_idx);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
				wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) loop_depth);
				terminated = TRUE;
				break;
			}
			case OP_GET_EX_OBJ: {
				/* In a JITted catch handler: load the exception the catch landing pad stashed
				 * (mono_wasm_jit_get_caught_exc, ()->i32). dreg is a ref -> wasm_st routes it to the
				 * GC ref shadow stack. */
				WasmFuncType _gt; int _gti = -1, _gk;
				memset (&_gt, 0, sizeof _gt); _gt.nparams = 0; _gt.ret = WASM_I32;
				for (_gk = 0; _gk < nextra; ++_gk) if (functype_eq (&extra_types [_gk], &_gt)) { _gti = 2 + _gk; break; }
				if (_gti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = _gt; _gti = 2 + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				{ extern MonoObject *mono_wasm_jit_get_caught_exc (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_get_caught_exc); }
#else
				wasm_i32_const (&body, 0x7ff4);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) _gti); wasm_uleb (&body, 0);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "get_ex_obj dreg"; goto done; }
				break;
			}
			case OP_MEMORY_BARRIER:
#ifndef DISABLE_THREADS
				/* Match the jiterpreter (MINT_MONO_MEMORY_BARRIER): emit a seq-cst atomic.fence
				 * (0xfe 0x03 0x00). Valid because our module imports shared memory. On a non-threads
				 * build the heap isn't shared and the barrier is a no-op. */
				wasm_u8 (&body, 0xfe);  /* atomic prefix */
				wasm_u8 (&body, 0x03);  /* atomic.fence */
				wasm_u8 (&body, 0x00);  /* memory order: sequentially consistent */
#endif
				break;
			case OP_AOTCONST: {
				/* On wasm (need_got_var=0, compile_aot=0) NEW_AOTCONST lowers to OP_PCONST, so the
				 * GC-safepoint poll-flag address arrives as a normal constant we already handle (and
				 * OP_GC_SAFE_POINT re-bakes it anyway). A genuine OP_AOTCONST here comes from
				 * NEW_AOTCONST_TOKEN (ldstr/ldtoken/type/field/method tokens) and its result vreg IS
				 * consumed, so we must NOT silently skip it (that leaves the dreg undefined). We don't
				 * resolve+bake token constants yet — and MONO_PATCH_INFO_LDSTR resolves to a movable GC
				 * object that can't be a wasm immediate — so bail the method to the interpreter. The
				 * GC_SAFE_POINT_FLAG form (shouldn't reach here on wasm) is dead, so tolerate it. */
				switch ((MonoJumpInfoType) (gsize) ins->inst_p1) {
				case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
					break;   /* dead on wasm; OP_GC_SAFE_POINT re-bakes the flag address */
				/* !compile_aot: NEW_*CONST stored the RESOLVED runtime pointer in inst_p0 (ir-emit.h:
				 * `compile_aot ? klass : vtable`). vtables/classes/methods/static-storage/images are
				 * stable, un-movable, cross-thread -> bake the pointer like a resolved pconst. (Movable
				 * LDSTR objects come via OP_(I|P)CONST + the interned slot, never here.) */
				case MONO_PATCH_INFO_VTABLE: case MONO_PATCH_INFO_CLASS: case MONO_PATCH_INFO_METHOD:
				case MONO_PATCH_INFO_METHODCONST: case MONO_PATCH_INFO_FIELD: case MONO_PATCH_INFO_SFLDA:
				case MONO_PATCH_INFO_IMAGE: case MONO_PATCH_INFO_METHOD_RGCTX:
					/* GATED (MONO_WASM_JIT_AOTCONST=1, default OFF): baking inst_p0 enables newobj/token-constant
					 * methods to JIT, but is unvalidated and suspected of a regression — default to the
					 * known-good bail until isolated. */
					{ extern int mono_wasm_jit_aotconst; if (!mono_wasm_jit_aotconst) { fail = "aotconst (gated off)"; fail_op = ins->opcode; goto done; } }
					wasm_i32_const (&body, (gint32) (intptr_t) ins->inst_p0);
					if (!wasm_st (&body, &lc, ins->dreg)) { fail = "aotconst dreg"; goto done; }
					break;
				default:
					fail = "aotconst type"; fail_op = ins->opcode; goto done;
				}
				break;
			}
			case OP_GC_SAFE_POINT: {
				/* Cooperative-GC safepoint poll: if (mono_polling_required) mono_threads_state_poll().
				 * REQUIRED for multithreaded GC — without it a JITted loop never reaches a safepoint,
				 * so the GC can't suspend the thread and the coop suspend deadlocks (every thread must
				 * poll). Bake the address of the global flag + the poll icall's table index at emit
				 * time; the IR's sreg1 (flag address via AOTCONST) is ignored. */
#ifdef HOST_BROWSER
				{
					extern volatile size_t mono_polling_required;
					extern void mono_threads_state_poll (void);
					WasmFuncType pt; int pti = -1, pk;
					memset (&pt, 0, sizeof (pt)); pt.ret = WASM_VOID; pt.nparams = 0;
					for (pk = 0; pk < nextra; ++pk) if (functype_eq (&extra_types [pk], &pt)) { pti = 2 + pk; break; }
					if (pti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt; pti = 2 + nextra++; }
					uses_calls = TRUE;
					wasm_i32_const (&body, (gint32) (intptr_t) &mono_polling_required);
					wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);              /* if (flag != 0) */
					wasm_i32_const (&body, (gint32) (intptr_t) &mono_threads_state_poll);
					wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) pti); wasm_uleb (&body, 0);
					wasm_op (&body, WASM_OP_END);
				}
#endif
				break;
			}
			case OP_ICONST:
				/* OP_PCONST aliases OP_ICONST on wasm32. A non-null STACK_OBJ constant is a baked managed-
				 * object pointer (ldstr/typeof/folded GetType) — a MOVABLE GC object that must NOT be a fixed
				 * wasm immediate (it dangles after a compacting GC -> NPE / null-function). method-to-ir
				 * already interned it into the GC-tracked literal table at RESOLVE time (a fresh, un-movable-
				 * yet pointer) and stashed the stable slot address in inst_p1; here we bake that address and
				 * i32.load the CURRENT object pointer at runtime (the precise root keeps the slot up to date
				 * across moves). The isref pass marked this dreg, so wasm_st stores the loaded value to the
				 * ref shadow stack -> it also survives the next GC point. inst_p1==0 => not interned (table
				 * full, or a managed-pconst source method-to-ir didn't convert): bail rather than bake a
				 * movable pointer. inst_c0/inst_p0 alias; a null STACK_OBJ and every non-object iconst fall
				 * through to the plain immediate bake. */
				if (ins->type == STACK_OBJ && ins->inst_p0) {
#ifdef HOST_BROWSER
					if (!ins->inst_p1) { fail = "managed pconst not interned"; goto done; }
					wasm_i32_const (&body, (gint32) (intptr_t) ins->inst_p1);
#else
					wasm_i32_const (&body, 0x7ff8); /* offline dump: placeholder slot addr for encoder validation */
#endif
					wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
				} else {
					wasm_i32_const (&body, (gint32) ins->inst_c0);
				}
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "iconst"; goto done; }
				break;
			case OP_I8CONST:
				wasm_i64_const (&body, (gint64) ins->inst_l);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "i8const"; goto done; }
				break;
			case OP_R8CONST:
				wasm_f64_const (&body, *(double *) ins->inst_p0);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "r8const"; goto done; }
				break;
			case OP_MOVE: case OP_LMOVE: case OP_FMOVE: case OP_RMOVE:
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_st (&body, &lc, ins->dreg)) { fail = "move"; goto done; }
				break;
			case OP_IADD: BIN (WASM_OP_I32_ADD); break;
			case OP_ISUB: BIN (WASM_OP_I32_SUB); break;
			case OP_IMUL: BIN (WASM_OP_I32_MUL); break;
			case OP_IDIV: BIN (WASM_OP_I32_DIV_S); break;
			case OP_IDIV_UN: BIN (WASM_OP_I32_DIV_U); break;
			case OP_IREM: BIN (WASM_OP_I32_REM_S); break;
			case OP_IREM_UN: BIN (WASM_OP_I32_REM_U); break;
			case OP_IAND: BIN (WASM_OP_I32_AND); break;
			case OP_IOR: BIN (WASM_OP_I32_OR); break;
			case OP_IXOR: BIN (WASM_OP_I32_XOR); break;
			case OP_ISHL: BIN (WASM_OP_I32_SHL); break;
			case OP_ISHR: BIN (WASM_OP_I32_SHR_S); break;
			case OP_ISHR_UN: BIN (WASM_OP_I32_SHR_U); break;
			case OP_IADD_IMM: BINI32 (WASM_OP_I32_ADD); break;
			case OP_ISUB_IMM: BINI32 (WASM_OP_I32_SUB); break;
			case OP_IMUL_IMM: BINI32 (WASM_OP_I32_MUL); break;
			case OP_IAND_IMM: BINI32 (WASM_OP_I32_AND); break;
			case OP_IOR_IMM: BINI32 (WASM_OP_I32_OR); break;
			case OP_IXOR_IMM: BINI32 (WASM_OP_I32_XOR); break;
			case OP_ISHL_IMM: BINI32 (WASM_OP_I32_SHL); break;
			case OP_ISHR_IMM: BINI32 (WASM_OP_I32_SHR_S); break;
			case OP_ISHR_UN_IMM: BINI32 (WASM_OP_I32_SHR_U); break;
			/* div/rem by a constant (imm != 0, and != -1 for signed -> no INT_MIN/-1 overflow trap) */
			case OP_IDIV_IMM: BINI32 (WASM_OP_I32_DIV_S); break;
			case OP_IDIV_UN_IMM: BINI32 (WASM_OP_I32_DIV_U); break;
			case OP_IREM_IMM: BINI32 (WASM_OP_I32_REM_S); break;
			case OP_IREM_UN_IMM: BINI32 (WASM_OP_I32_REM_U); break;
			/* native-int (size_t) IMM ops — i32 on wasm32, same lowering as the OP_I*_IMM variants */
			case OP_ADD_IMM: BINI32 (WASM_OP_I32_ADD); break;
			case OP_SUB_IMM: BINI32 (WASM_OP_I32_SUB); break;
			case OP_MUL_IMM: BINI32 (WASM_OP_I32_MUL); break;
			case OP_OR_IMM:  BINI32 (WASM_OP_I32_OR); break;
			case OP_XOR_IMM: BINI32 (WASM_OP_I32_XOR); break;
			case OP_SHL_IMM: BINI32 (WASM_OP_I32_SHL); break;
			case OP_SHR_IMM: BINI32 (WASM_OP_I32_SHR_S); break;
			case OP_SHR_UN_IMM: BINI32 (WASM_OP_I32_SHR_U); break;
			case OP_INEG: /* no i32.neg in wasm: 0 - x */
				wasm_i32_const (&body, 0);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ineg sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_SUB);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ineg dreg"; goto done; }
				break;
			case OP_INOT: /* no i32.not in wasm: x ^ -1 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "inot sreg"; goto done; }
				wasm_i32_const (&body, -1);
				wasm_op (&body, WASM_OP_I32_XOR);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "inot dreg"; goto done; }
				break;
			case OP_LNOT: /* no i64.not in wasm: x ^ -1 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "lnot sreg"; goto done; }
				wasm_i64_const (&body, (gint64) -1);
				wasm_op (&body, WASM_OP_I64_XOR);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lnot dreg"; goto done; }
				break;
			case OP_LADD: BIN (WASM_OP_I64_ADD); break;
			case OP_LSUB: BIN (WASM_OP_I64_SUB); break;
			case OP_LMUL: BIN (WASM_OP_I64_MUL); break;
			case OP_LAND: BIN (WASM_OP_I64_AND); break;
			case OP_LOR: BIN (WASM_OP_I64_OR); break;
			case OP_LXOR: BIN (WASM_OP_I64_XOR); break;
			case OP_LSHL: LSHIFT (WASM_OP_I64_SHL); break;
			case OP_LSHR: LSHIFT (WASM_OP_I64_SHR_S); break;
			case OP_LSHR_UN: LSHIFT (WASM_OP_I64_SHR_U); break;
			case OP_LADD_IMM: BINI64L (WASM_OP_I64_ADD); break;
			case OP_LSUB_IMM: BINI64L (WASM_OP_I64_SUB); break;
			case OP_LMUL_IMM: BINI64L (WASM_OP_I64_MUL); break;
			case OP_LAND_IMM: BINI64L (WASM_OP_I64_AND); break;
			case OP_LOR_IMM:  BINI64L (WASM_OP_I64_OR); break;
			case OP_LXOR_IMM: BINI64L (WASM_OP_I64_XOR); break;
			case OP_LSHL_IMM: BINI64 (WASM_OP_I64_SHL); break;
			case OP_LSHR_IMM: BINI64 (WASM_OP_I64_SHR_S); break;
			case OP_LSHR_UN_IMM: BINI64 (WASM_OP_I64_SHR_U); break;
			case OP_FADD: BIN (WASM_OP_F64_ADD); break;
			case OP_FSUB: BIN (WASM_OP_F64_SUB); break;
			case OP_FMUL: BIN (WASM_OP_F64_MUL); break;
			case OP_FDIV: BIN (WASM_OP_F64_DIV); break;
			case OP_FNEG: UN (WASM_OP_F64_NEG); break;
			case OP_RADD: BIN (WASM_OP_F32_ADD); break;   /* native-f32 (r4fp) float arithmetic */
			case OP_RSUB: BIN (WASM_OP_F32_SUB); break;
			case OP_RMUL: BIN (WASM_OP_F32_MUL); break;
			case OP_RDIV: BIN (WASM_OP_F32_DIV); break;
			case OP_RNEG: UN (WASM_OP_F32_NEG); break;
			case OP_ICONV_TO_R4: UN (WASM_OP_F32_CONVERT_I32_S); break;  /* i32 -> f32 */
			case OP_RCONV_TO_R4: /* R4->R4 identity (r4fp f32); demote if the source vreg is f64 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rconv_to_r4 sreg"; goto done; }
				if (ins->sreg1 >= 0 && ins->sreg1 < nvreg && vt [ins->sreg1] == WASM_F64)
					wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "rconv_to_r4 dreg"; goto done; }
				break;
			case OP_ICONV_TO_R8:
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "conv sreg"; goto done; }
				wasm_op (&body, WASM_OP_F64_CONVERT_I32_S);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "conv dreg"; goto done; }
				break;
			case OP_RCONV_TO_R8: UN (WASM_OP_F64_PROMOTE_F32); break;     /* f32 -> f64 */
			case OP_FCONV_TO_R4: UN (WASM_OP_F32_DEMOTE_F64); break;      /* f64 -> f32 */
			case OP_ICONV_TO_I8: UN (WASM_OP_I64_EXTEND_I32_S); break;    /* i32 -> i64 (sign) */
			case OP_ICONV_TO_U8: UN (WASM_OP_I64_EXTEND_I32_U); break;    /* i32 -> i64 (zero) */
			case OP_ICONV_TO_I1: UN (WASM_OP_I32_EXTEND8_S); break;       /* (sbyte) */
			case OP_ICONV_TO_I2: UN (WASM_OP_I32_EXTEND16_S); break;      /* (short) */
			case OP_ICONV_TO_U1: MASK (0xFF); break;                     /* (byte) */
			case OP_ICONV_TO_U2: MASK (0xFFFF); break;                   /* (ushort) */
			case OP_LCONV_TO_I4: case OP_LCONV_TO_U4: UN (WASM_OP_I32_WRAP_I64); break;  /* i64 -> i32 (truncate) */
			case OP_LCONV_TO_R8: UN (WASM_OP_F64_CONVERT_I64_S); break;  /* i64 -> f64 */
			case OP_LCONV_TO_R4: UN (WASM_OP_F32_CONVERT_I64_S); break;  /* i64 -> f32 */
			/* bit-reinterpret (Unsafe.BitCast intrinsic, what IKVM's Float/Double intBitsToFloat etc. lower
			 * to via intrinsics.c): no value change, just reinterpret the bits */
			case OP_MOVE_I4_TO_F: UN (WASM_OP_F32_REINTERPRET_I32); break;  /* i32 bits -> f32 */
			case OP_MOVE_F_TO_I4: UN (WASM_OP_I32_REINTERPRET_F32); break;  /* f32 bits -> i32 */
			case OP_MOVE_I8_TO_F: UN (WASM_OP_F64_REINTERPRET_I64); break;  /* i64 bits -> f64 */
			case OP_MOVE_F_TO_I8: UN (WASM_OP_I64_REINTERPRET_F64); break;  /* f64 bits -> i64 */
			case OP_R4CONST:
				wasm_f32_const (&body, *(float *) ins->inst_p0);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "r4const"; goto done; }
				break;
			case OP_AND_IMM: BINI32 (WASM_OP_I32_AND); break;
			case OP_NOT_NULL: case OP_CHECK_THIS:
				/* Null check. Trap (-> RuntimeError) if sreg1 is null rather than skipping it: a skipped
				 * check would let a following null deref read low linear memory (address 0 is valid in a
				 * wasm linear memory) = silent garbage, not a fault. Methods with EH clauses bail before
				 * here, so we don't need to raise a *catchable* NRE. */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "nullchk sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_EQZ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				wasm_op (&body, WASM_OP_UNREACHABLE);
				wasm_op (&body, WASM_OP_END);
				break;
#define LOADM(WOP, AL) do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "load base"; goto done; } wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "load dreg"; goto done; } } while (0)
			case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE: LOADM (WASM_OP_I32_LOAD, 2); break;
			case OP_LOADU1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_U, 0); break;
			case OP_LOADI1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_S, 0); break;
			case OP_LOADU2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_U, 1); break;
			case OP_LOADI2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_S, 1); break;
			case OP_LOADI8_MEMBASE: LOADM (WASM_OP_I64_LOAD, 3); break;
			case OP_LOADR8_MEMBASE: LOADM (WASM_OP_F64_LOAD, 3); break;
			case OP_LOADR4_MEMBASE: LOADM (WASM_OP_F32_LOAD, 2); break;
#undef LOADM
/* membase store: *(dreg[=inst_destbasereg] + inst_offset) = sreg1. (A reference value still needs its
 * GC write barrier, which mono emits as a separate IR call before the store — so the store is raw.) */
#define STOREM(WOP, AL) do { if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store base"; goto done; } if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "store val"; goto done; } wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
			case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: STOREM (WASM_OP_I32_STORE, 2); break;
			case OP_STOREI1_MEMBASE_REG: STOREM (WASM_OP_I32_STORE8, 0); break;
			case OP_STOREI2_MEMBASE_REG: STOREM (WASM_OP_I32_STORE16, 1); break;
			case OP_STOREI8_MEMBASE_REG: STOREM (WASM_OP_I64_STORE, 3); break;
			case OP_STORER4_MEMBASE_REG: STOREM (WASM_OP_F32_STORE, 2); break;
			case OP_STORER8_MEMBASE_REG: STOREM (WASM_OP_F64_STORE, 3); break;
#undef STOREM
/* membase store of an immediate: *(dreg[=inst_destbasereg] + inst_offset) = inst_imm. dreg aliases
 * inst_destbasereg (the base); inst_imm is the constant. Sub-word stores truncate via i32.store8/16. */
#define STOREMI(WOP, AL) do { if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store-imm base"; goto done; } wasm_i32_const (&body, (gint32) ins->inst_imm); wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
			case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE, 2); break;
			case OP_STOREI1_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE8, 0); break;
			case OP_STOREI2_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE16, 1); break;
#undef STOREMI
			case OP_SETRET:
				/* The return value lives in cfg->ret->dreg; the epilogue below
				 * leaves it on the stack. setret just ensures it holds sreg1. */
				if (cfg->ret && ins->sreg1 != cfg->ret->dreg) {
					if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_st (&body, &lc, cfg->ret->dreg)) { fail = "setret"; goto done; }
				}
				break;
			case OP_ICOMPARE: case OP_COMPARE: /* COMPARE = pointer/native-int compare, i32 on wasm32 */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0;
				break;
			case OP_LCOMPARE: /* i64 compare, consumed by a following OP_LB<cc> branch or OP_LC<cc> setcc */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0; cmp_i64 = TRUE;
				break;
			case OP_LBEQ: case OP_LBNE_UN: case OP_LBLT: case OP_LBLT_UN:
			case OP_LBGT: case OP_LBGT_UN: case OP_LBLE: case OP_LBLE_UN:
			case OP_LBGE: case OP_LBGE_UN:
				/* i64 conditional branch consuming the preceding OP_LCOMPARE (mirrors OP_IB<cc> but i64 ops) */
				if (cmp_a < 0 || !cmp_i64) { fail = "long branch without lcompare"; goto done; }
				switch (ins->opcode) {
				case OP_LBEQ: cmp_wop = WASM_OP_I64_EQ; break;
				case OP_LBNE_UN: cmp_wop = WASM_OP_I64_NE; break;
				case OP_LBLT: cmp_wop = WASM_OP_I64_LT_S; break;
				case OP_LBLT_UN: cmp_wop = WASM_OP_I64_LT_U; break;
				case OP_LBGT: cmp_wop = WASM_OP_I64_GT_S; break;
				case OP_LBGT_UN: cmp_wop = WASM_OP_I64_GT_U; break;
				case OP_LBLE: cmp_wop = WASM_OP_I64_LE_S; break;
				case OP_LBLE_UN: cmp_wop = WASM_OP_I64_LE_U; break;
				case OP_LBGE: cmp_wop = WASM_OP_I64_GE_S; break;
				default: cmp_wop = WASM_OP_I64_GE_U; break; /* OP_LBGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "lcmp a"; goto done; }
				if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lcmp b"; goto done; }
				wasm_op (&body, cmp_wop);   /* i64 compare -> i32 (0/1) */
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				GOTO (ins->inst_true_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_ELSE);
				GOTO (ins->inst_false_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_END);
				terminated = TRUE;
				break;
			case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN: {
				/* i64 setcc: dreg = (cmp_a <cc> cmp_b) ? 1 : 0, consuming the preceding OP_LCOMPARE */
				WasmOpcode w;
				if (cmp_a < 0 || !cmp_i64) { fail = "long setcc without lcompare"; goto done; }
				switch (ins->opcode) {
				case OP_LCEQ:    w = WASM_OP_I64_EQ; break;
				case OP_LCGT:    w = WASM_OP_I64_GT_S; break;
				case OP_LCGT_UN: w = WASM_OP_I64_GT_U; break;
				case OP_LCLT:    w = WASM_OP_I64_LT_S; break;
				default:         w = WASM_OP_I64_LT_U; break; /* OP_LCLT_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "lsetcc a"; goto done; }
				if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lsetcc b"; goto done; }
				wasm_op (&body, w);   /* i64 compare -> i32 (0/1) */
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lsetcc dreg"; goto done; }
				break;
			}
			case OP_ICOMPARE_IMM: case OP_COMPARE_IMM:
				cmp_a = ins->sreg1; cmp_imm = (gint32) ins->inst_imm; cmp_imm_mode = TRUE; cmp_float = 0;
				break;
			case OP_FCOMPARE: /* f64 compare, consumed by a following OP_FB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F64;
				break;
			case OP_RCOMPARE: /* f32 (r4) compare, consumed by a following OP_RB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F32;
				break;
			case OP_IBEQ: case OP_IBNE_UN: case OP_IBLT: case OP_IBLT_UN:
			case OP_IBGT: case OP_IBGT_UN: case OP_IBLE: case OP_IBLE_UN:
			case OP_IBGE: case OP_IBGE_UN:
				if (cmp_a < 0) { fail = "branch without compare"; goto done; }
				switch (ins->opcode) {
				case OP_IBEQ: cmp_wop = WASM_OP_I32_EQ; break;
				case OP_IBNE_UN: cmp_wop = WASM_OP_I32_NE; break;
				case OP_IBLT: cmp_wop = WASM_OP_I32_LT_S; break;
				case OP_IBLT_UN: cmp_wop = WASM_OP_I32_LT_U; break;
				case OP_IBGT: cmp_wop = WASM_OP_I32_GT_S; break;
				case OP_IBGT_UN: cmp_wop = WASM_OP_I32_GT_U; break;
				case OP_IBLE: cmp_wop = WASM_OP_I32_LE_S; break;
				case OP_IBLE_UN: cmp_wop = WASM_OP_I32_LE_U; break;
				case OP_IBGE: cmp_wop = WASM_OP_I32_GE_S; break;
				default: cmp_wop = WASM_OP_I32_GE_U; break; /* OP_IBGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cmp a"; goto done; }
				if (cmp_imm_mode)
					wasm_i32_const (&body, cmp_imm);
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "cmp b"; goto done; }
				wasm_op (&body, cmp_wop);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				GOTO (ins->inst_true_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_ELSE);
				GOTO (ins->inst_false_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_END);
				terminated = TRUE;
				break;
			case OP_COND_EXC_EQ: case OP_COND_EXC_NE_UN: case OP_COND_EXC_LT: case OP_COND_EXC_LT_UN:
			case OP_COND_EXC_GT: case OP_COND_EXC_GT_UN: case OP_COND_EXC_LE: case OP_COND_EXC_LE_UN:
			case OP_COND_EXC_GE: case OP_COND_EXC_GE_UN:
			case OP_COND_EXC_IEQ: case OP_COND_EXC_INE_UN: case OP_COND_EXC_ILT: case OP_COND_EXC_ILT_UN:
			case OP_COND_EXC_IGT: case OP_COND_EXC_IGT_UN: case OP_COND_EXC_ILE: case OP_COND_EXC_ILE_UN:
			case OP_COND_EXC_IGE: case OP_COND_EXC_IGE_UN: {
				/* checked-op throw: if the preceding compare's <cc> holds, raise a corlib exception
				 * (OverflowException / IndexOutOfRange / DivideByZero / ...). Hot path (no throw) = the compare
				 * + a not-taken branch; cold path calls mono_wasm_jit_raise_corlib (sets interp resume-state, no
				 * C++ throw) then bails (dummy ret + ref_leave + return), mirroring EMIT_PENDING_EXC_CHECK.
				 * Reuses the OP_IBcc compare-fusing (cmp_a/cmp_b/cmp_imm). Integer compares only. */
				const char *en = (const char *) ins->inst_p1;
				int exc_id = -1, ck; WasmFuncType rt; int rti = -1;
				{ extern int mono_wasm_jit_cond_exc; if (!mono_wasm_jit_cond_exc) { fail = "cond_exc disabled (env)"; goto done; } }
				if (cmp_a < 0) { fail = "cond_exc without compare"; goto done; }
				if (cmp_float != 0) { fail = "cond_exc float compare"; goto done; }
				switch (ins->opcode) {
				case OP_COND_EXC_EQ: case OP_COND_EXC_IEQ: cmp_wop = WASM_OP_I32_EQ; break;
				case OP_COND_EXC_NE_UN: case OP_COND_EXC_INE_UN: cmp_wop = WASM_OP_I32_NE; break;
				case OP_COND_EXC_LT: case OP_COND_EXC_ILT: cmp_wop = WASM_OP_I32_LT_S; break;
				case OP_COND_EXC_LT_UN: case OP_COND_EXC_ILT_UN: cmp_wop = WASM_OP_I32_LT_U; break;
				case OP_COND_EXC_GT: case OP_COND_EXC_IGT: cmp_wop = WASM_OP_I32_GT_S; break;
				case OP_COND_EXC_GT_UN: case OP_COND_EXC_IGT_UN: cmp_wop = WASM_OP_I32_GT_U; break;
				case OP_COND_EXC_LE: case OP_COND_EXC_ILE: cmp_wop = WASM_OP_I32_LE_S; break;
				case OP_COND_EXC_LE_UN: case OP_COND_EXC_ILE_UN: cmp_wop = WASM_OP_I32_LE_U; break;
				case OP_COND_EXC_GE: case OP_COND_EXC_IGE: cmp_wop = WASM_OP_I32_GE_S; break;
				default: cmp_wop = WASM_OP_I32_GE_U; break; /* GE_UN / IGE_UN */
				}
				if (!en) { fail = "cond_exc no name"; goto done; }
				else if (!strcmp (en, "OverflowException")) exc_id = 0;
				else if (!strcmp (en, "DivideByZeroException")) exc_id = 1;
				else if (!strcmp (en, "IndexOutOfRangeException")) exc_id = 2;
				else if (!strcmp (en, "InvalidCastException")) exc_id = 3;
				else if (!strcmp (en, "NullReferenceException")) exc_id = 4;
				else if (!strcmp (en, "ArithmeticException")) exc_id = 5;
				else if (!strcmp (en, "ArrayTypeMismatchException")) exc_id = 6;
				else { fail = "cond_exc exc name"; goto done; }
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_I32; rt.nparams = 1; rt.ret = WASM_VOID;
				for (ck = 0; ck < nextra; ++ck) if (functype_eq (&extra_types [ck], &rt)) { rti = 2 + ck; break; }
				if (rti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; rti = 2 + nextra++; }
				uses_calls = TRUE;
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cond_exc a"; goto done; }
				if (cmp_imm_mode) wasm_i32_const (&body, cmp_imm);
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "cond_exc b"; goto done; }
				wasm_op (&body, cmp_wop);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				wasm_i32_const (&body, exc_id);
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_raise_corlib (int exc_id); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_raise_corlib); }
#else
				wasm_i32_const (&body, 0x7ff8);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) rti); wasm_uleb (&body, 0);
				{ extern int mono_wasm_jit_cppeh;
				if (mono_wasm_jit_cppeh) {
					/* AOT-style: mono_wasm_jit_raise_corlib C++-throws and never returns; unwind natively. */
					wasm_op (&body, WASM_OP_UNREACHABLE);
				} else {
					switch (ret_vt) {
					case WASM_I32: wasm_i32_const (&body, 0); break;
					case WASM_I64: wasm_i64_const (&body, 0); break;
					case WASM_F32: wasm_f32_const (&body, 0); break;
					case WASM_F64: wasm_f64_const (&body, 0); break;
					default: break;
					}
					EMIT_REF_LEAVE ();
					wasm_op (&body, WASM_OP_RETURN);
				} }
				wasm_op (&body, WASM_OP_END);   /* close the cond `if` (hot path continues after) */
				break;
			}
			case OP_THROW: {
				/* throw <exc=sreg1>: raise via mono_wasm_jit_throw (installs interp resume-state; a JITted
				 * method has no local handler — EH-clause methods bail), then exit the method (dummy ret +
				 * ref_leave + return). The caller's EMIT_PENDING_EXC_CHECK / the interp e-thunk's resume
				 * unwinds to the real handler. Unconditional bb terminator -> mark terminated. #1 leaf
				 * blocker in IKVM, so this is the big bottom-up coverage unlock. */
				WasmFuncType tt; int tti = -1, tk;
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "throw sreg"; goto done; }
				memset (&tt, 0, sizeof (tt)); tt.params [0] = WASM_I32; tt.nparams = 1; tt.ret = WASM_VOID;
				for (tk = 0; tk < nextra; ++tk) if (functype_eq (&extra_types [tk], &tt)) { tti = 2 + tk; break; }
				if (tti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = tt; tti = 2 + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_throw (MonoObject *exc); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_throw); }
#else
				wasm_i32_const (&body, 0x7ff7);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tti); wasm_uleb (&body, 0);
				{ extern int mono_wasm_jit_cppeh;
				if (mono_wasm_jit_cppeh) {
					/* AOT-style: mono_wasm_jit_throw C++-throws (mono_llvm_cpp_throw_exception) and never
					 * returns — the wasm stack unwinds natively to the nearest landing pad (an in-method
					 * catch or the interp e-thunk boundary). No dummy ret / ref_leave (the catch boundary
					 * restores the ref shadow-stack SP). */
					wasm_op (&body, WASM_OP_UNREACHABLE);
				} else {
					switch (ret_vt) {
					case WASM_I32: wasm_i32_const (&body, 0); break;
					case WASM_I64: wasm_i64_const (&body, 0); break;
					case WASM_F32: wasm_f32_const (&body, 0); break;
					case WASM_F64: wasm_f64_const (&body, 0); break;
					default: break;
					}
					EMIT_REF_LEAVE ();
					wasm_op (&body, WASM_OP_RETURN);
				} }
				terminated = TRUE;
				break;
			}
			case OP_FBEQ: case OP_RBEQ: case OP_FBNE_UN: case OP_RBNE_UN:
			case OP_FBLT: case OP_RBLT: case OP_FBLT_UN: case OP_RBLT_UN:
			case OP_FBGT: case OP_RBGT: case OP_FBGT_UN: case OP_RBGT_UN:
			case OP_FBLE: case OP_RBLE: case OP_FBLE_UN: case OP_RBLE_UN:
			case OP_FBGE: case OP_RBGE: case OP_FBGE_UN: case OP_RBGE_UN: {
				/* float conditional branch: consumes the preceding OP_(F|R)COMPARE (width in cmp_float).
				 * wasm has only ordered compares (+ unordered NE), so the *_UN ordered-relation variants
				 * are the complementary ordered op + i32.eqz (e.g. blt_un = !(a>=b), true on NaN). */
				int kind, neg = 0;
				gboolean isf32;
				if (cmp_a < 0 || cmp_float == 0) { fail = "float branch without fcompare"; goto done; }
				isf32 = (cmp_float == WASM_F32);
				switch (ins->opcode) {
				case OP_FBEQ: case OP_RBEQ:       kind = 0; break;
				case OP_FBNE_UN: case OP_RBNE_UN: kind = 1; break;
				case OP_FBLT: case OP_RBLT:       kind = 2; break;
				case OP_FBGT: case OP_RBGT:       kind = 3; break;
				case OP_FBLE: case OP_RBLE:       kind = 4; break;
				case OP_FBGE: case OP_RBGE:       kind = 5; break;
				case OP_FBLT_UN: case OP_RBLT_UN: kind = 5; neg = 1; break; /* !(a>=b) */
				case OP_FBGT_UN: case OP_RBGT_UN: kind = 4; neg = 1; break; /* !(a<=b) */
				case OP_FBLE_UN: case OP_RBLE_UN: kind = 3; neg = 1; break; /* !(a>b) */
				default:                          kind = 2; neg = 1; break; /* (F|R)BGE_UN: !(a<b) */
				}
				FCMP_PUSH (isf32, kind, neg, cmp_a, cmp_b);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				GOTO (ins->inst_true_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_ELSE);
				GOTO (ins->inst_false_bb, loop_depth + 1);
				wasm_op (&body, WASM_OP_END);
				terminated = TRUE;
				break;
			}
			case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN:
			case OP_ICGT: case OP_ICGT_UN: case OP_ICLE: case OP_ICLE_UN:
			case OP_ICGE: case OP_ICGE_UN: {
				/* setcc: dreg = (cmp_a <op> cmp_b) as 0/1, consuming the preceding OP_(I)COMPARE's
				 * operands (these carry no sregs, like the conditional branches above). */
				WasmOpcode w;
				if (cmp_a < 0) { fail = "setcc without compare"; goto done; }
				switch (ins->opcode) {
				case OP_ICEQ:    w = WASM_OP_I32_EQ; break;
				case OP_ICNEQ:   w = WASM_OP_I32_NE; break;
				case OP_ICLT:    w = WASM_OP_I32_LT_S; break;
				case OP_ICLT_UN: w = WASM_OP_I32_LT_U; break;
				case OP_ICGT:    w = WASM_OP_I32_GT_S; break;
				case OP_ICGT_UN: w = WASM_OP_I32_GT_U; break;
				case OP_ICLE:    w = WASM_OP_I32_LE_S; break;
				case OP_ICLE_UN: w = WASM_OP_I32_LE_U; break;
				case OP_ICGE:    w = WASM_OP_I32_GE_S; break;
				default:         w = WASM_OP_I32_GE_U; break; /* OP_ICGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "setcc a"; goto done; }
				if (cmp_imm_mode) wasm_i32_const (&body, cmp_imm);
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "setcc b"; goto done; }
				wasm_op (&body, w);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "setcc dreg"; goto done; }
				break;
			}
			case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
			case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE: {
				/* float compare-to-i32 setcc: standalone (own FREG operands). FC* are f64, RC* are f32
				 * (RC* sort after FC* in the opcode enum). dreg = (sreg1 <cc> sreg2) ? 1 : 0. */
				int kind, neg = 0;
				gboolean isf32 = (ins->opcode >= OP_RCEQ);
				switch (ins->opcode) {
				case OP_FCEQ: case OP_RCEQ:       kind = 0; break;
				case OP_FCNEQ: case OP_RCNEQ:     kind = 1; break; /* unordered ne */
				case OP_FCLT: case OP_RCLT:       kind = 2; break;
				case OP_FCGT: case OP_RCGT:       kind = 3; break;
				case OP_FCLE: case OP_RCLE:       kind = 4; break;
				case OP_FCGE: case OP_RCGE:       kind = 5; break;
				case OP_FCLT_UN: case OP_RCLT_UN: kind = 5; neg = 1; break; /* !(a>=b) */
				default:                          kind = 4; neg = 1; break; /* (F|R)CGT_UN: !(a<=b) */
				}
				FCMP_PUSH (isf32, kind, neg, ins->sreg1, ins->sreg2);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fsetcc dreg"; goto done; }
				break;
			}
			case OP_BR:
				GOTO (ins->inst_target_bb, loop_depth);
				terminated = TRUE;
				break;
			case OP_SWITCH: {
				/* jump table: dispatch sreg1 (the index, already bounds-checked to [0,N) by a preceding
				 * branch) to inst_many_bb[index]. N = GPOINTER_TO_UINT(ins->klass). Emit an if-chain of
				 * (index==i -> GOTO target_i) for i in [0,N-1), then an unconditional GOTO target_{N-1}
				 * (the in-range fall-through). Each per-case GOTO is inside one `if` (loop_depth+1); the
				 * final one is at bb top level (loop_depth), matching OP_BR. */
				guint nsw = GPOINTER_TO_UINT (ins->klass), si;
				if (nsw == 0) { fail = "switch no targets"; goto done; }
				for (si = 0; si + 1 < nsw; ++si) {
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "switch idx"; goto done; }
					wasm_i32_const (&body, (gint32) si);
					wasm_op (&body, WASM_OP_I32_EQ);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					GOTO (ins->inst_many_bb [si], loop_depth + 1);
					wasm_op (&body, WASM_OP_END);
				}
				GOTO (ins->inst_many_bb [nsw - 1], loop_depth);
				terminated = TRUE;
				break;
			}
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: {
				/* Lower a direct managed call to call_indirect through the callee's f-slot.
				 * Callee args are call->args[0..nparams) (this first, if any); the result goes
				 * to ins->dreg. Bails (whole method -> interp) if the callee isn't JITted yet. */
				MonoCallInst *call = (MonoCallInst *) ins;
				MonoMethodSignature *csig = call->signature;
				WasmFuncType ct;
				int call_fslot, type_idx = -1, k, ai;
				/* Synchronized methods keep their Monitor.Enter/Exit in a separate SYNCHRONIZED wrapper;
				 * the residual (mono_wasm_jit_call_interp) invokes the callee via mono_interp_get_imethod,
				 * which — unlike get_virtual_method / the interp transform — does NOT substitute that wrapper,
				 * so the RAW body runs without the monitor -> a notify/wait inside throws
				 * IllegalMonitorStateException and leaves the object's monitor state wrong (the netty
				 * DefaultPromise save/quit hang). The wrapper can't be created GC-safely after the arg spill,
				 * so bail the whole method to the interpreter (which applies the wrapper). Virtual calls to
				 * synchronized methods are handled in mono_wasm_jit_vcall_resolve (pre-spill). */
				if (call->method && (call->method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) { fail = "calls synchronized method (residual skips monitor wrapper)"; goto done; }
				if (!call->method) {
					/* method==NULL: a runtime JIT-icall. On the cold path of an llvmonly
					 * virtual/interp call this is mini_llvmonly_init_vtable_slot, which lazily
					 * resolves the ftndesc vtable slot (the interp warmup doesn't populate the
					 * llvmonly slot, so the first JITted dispatch hits this). Indirect-call the
					 * icall wrapper (call->fptr = a wasm table index) with its signature +
					 * captured args, so the cold path resolves the slot instead of trapping. */
					gint32 ifptr;
#ifdef HOST_BROWSER
					/* Use the RAW icall C function (info->func), a wasm table index callable with
					 * the icall's C signature directly. NOT call->fptr (= mono_icall_get_wrapper,
					 * which on wasm is the fixed-signature mono_wasm_specific_trampoline stub →
					 * call_indirect mismatch). The JITted code passes raw C args (vtable, slot). */
					{
						MonoJitICallInfo *iinfo = call->jit_icall_id ? mono_find_jit_icall_info (call->jit_icall_id) : NULL;
						ifptr = (iinfo && iinfo->func) ? (gint32) (intptr_t) iinfo->func : (gint32) (intptr_t) call->fptr;
					}
#else
					ifptr = 0x7ffe; /* offline dump: placeholder slot for encoder validation */
#endif
					if (!csig) { fail = "icall null sig"; goto done; }
					if (ifptr == 0) { fail = "icall no fptr"; goto done; }
					memset (&ct, 0, sizeof (ct));
					if (csig->hasthis) { if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "icall nargs"; goto done; } ct.params [ct.nparams++] = WASM_I32; }
					for (ai = 0; ai < (int) csig->param_count; ++ai) {
						WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
						if (pv == 0 || pv == WASM_VOID) { fail = "icall arg type"; goto done; }
						if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "icall nargs"; goto done; }
						ct.params [ct.nparams++] = pv;
					}
					if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
					else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) { fail = "icall ret type"; goto done; } }
					for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = 2 + k; break; }
					if (type_idx < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = 2 + nextra++; }
					uses_calls = TRUE;
					if (!call->call_info) { fail = "no captured icall args"; goto done; }
					{
						int *wargs = (int *) call->call_info;
						int nm = csig->param_count + (csig->hasthis ? 1 : 0);
						for (ai = 0; ai < nm; ++ai)
							if (!wasm_ld (&body, &lc, wargs [ai])) { fail = "icall arg ld"; goto done; }
					}
					wasm_i32_const (&body, ifptr);
					wasm_op (&body, WASM_OP_CALL_INDIRECT);
					wasm_uleb (&body, (guint32) type_idx);
					wasm_uleb (&body, 0); /* table 0 (imported f.f) */
					if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "icall dreg"; goto done; }
					EMIT_PENDING_EXC_CHECK ();
					break;
				}
				if (!csig) { fail = "null sig call"; goto done; }
				memset (&ct, 0, sizeof (ct));
				if (csig->hasthis) {
					if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "call nargs"; goto done; }
					ct.params [ct.nparams++] = WASM_I32;
				}
				for (ai = 0; ai < (int) csig->param_count; ++ai) {
					WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
					if (pv == 0 || pv == WASM_VOID) { fail = "call arg type"; goto done; }
					if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "call nargs"; goto done; }
					ct.params [ct.nparams++] = pv;
				}
				if (csig->ret->type == MONO_TYPE_VOID) {
					ct.ret = WASM_VOID;
				} else {
					ct.ret = wasm_valtype_of_type (csig->ret);
					if (ct.ret == 0 || ct.ret == WASM_VOID) { fail = "call ret type"; goto done; }
				}
#ifdef HOST_BROWSER
				{ extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m); call_fslot = mono_wasm_jit_get_callee_fslot (call->method); }
#else
				call_fslot = 0x7fff; /* offline dump: placeholder slot for encoder validation */
#endif
				if (call_fslot > 0) {
					/* Callee is wasm-JITted: direct call_indirect through its f-slot (Phase 2). */
					for (k = 0; k < nextra; ++k)
						if (functype_eq (&extra_types [k], &ct)) { type_idx = 2 + k; break; }
					if (type_idx < 0) {
						if (nextra >= 32) { fail = "too many callee types"; goto done; }
						extra_types [nextra] = ct;
						type_idx = 2 + nextra++;
					}
					uses_calls = TRUE;
					/* arg source vregs captured at method-to-ir time (calls.c), not call->args
					 * (which gets corrupted by later vreg passes) */
					if (!call->call_info) { fail = "no captured call args"; goto done; }
					{
						int *wargs = (int *) call->call_info;
						for (ai = 0; ai < (int) ct.nparams; ++ai)
							if (!wasm_ld (&body, &lc, wargs [ai])) { fail = "call arg ld"; goto done; }
					}
					wasm_i32_const (&body, call_fslot);
					wasm_op (&body, WASM_OP_CALL_INDIRECT);
					wasm_uleb (&body, (guint32) type_idx);
					wasm_uleb (&body, 0); /* table 0 (imported f.f) */
					if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
					EMIT_PENDING_EXC_CHECK ();
				}
#ifdef HOST_BROWSER
				else {
					/* INLINE DIRECT AOT CALL (Build 1, no EH): the callee has no f-slot but IS AOT-compiled.
					 * Call its AOT code DIRECTLY with the native ABI — the jiterpreter directJitCalls/enableDirect
					 * path emitted inline: push (this, args by value, rgctx); call_indirect <cinfo->addr>. No
					 * interp_entry, no frame, no LMF (validated jit102: conservative GC + local resume-state).
					 * Gated by MONO_WASM_JIT_INLINE_AOT (default off). byref args/ret bail to the residual; a
					 * throwing AOT callee would escape uncaught until Build 2 adds wasm-EH — test non-throwing. */
					{
						extern int mono_wasm_jit_inline_aot;
						extern gboolean mono_wasm_jit_aot_call_target (MonoMethod *m, gpointer *addr, gpointer *rgctx);
						gpointer aot_addr = NULL, aot_rgctx = NULL;
						gboolean aot_ok = mono_wasm_jit_inline_aot && !m_type_is_byref (csig->ret);
						for (k = 0; k < (int) csig->param_count && aot_ok; ++k)
							if (m_type_is_byref (csig->params [k])) aot_ok = FALSE;
						if (aot_ok && mono_wasm_jit_aot_call_target (call->method, &aot_addr, &aot_rgctx)) {
							WasmFuncType nt = ct;   /* native functype = (this?, args by value) + i32 rgctx -> ct.ret */
							WasmFuncType eht;       /* (i32)->void: the catch handler (mono_wasm_jit_aot_caught) AND the x.e tag */
							int nti = -1, ehti = -1;
							if (nt.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "aot inline nparams"; goto done; }
							nt.params [nt.nparams++] = WASM_I32;
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { nti = 2 + k; break; }
							if (nti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = nt; nti = 2 + nextra++; }
							uses_calls = TRUE;
							if (!call->call_info) { fail = "no captured call args"; goto done; }
							{ extern int mono_wasm_jit_cppeh;
							if (mono_wasm_jit_cppeh) {
								/* AOT-style EH: bare direct AOT call. A throwing AOT callee C++-unwinds (wasm-EH)
								 * straight through this JITted frame to the nearest landing pad — the interp e-thunk
								 * boundary (mono_llvm_catch_exception) or an in-method catch. No try/catch + no
								 * EMIT_PENDING_EXC_CHECK: the call is pure call_indirect (the per-call perf win). */
								{ int *wargs = (int *) call->call_info; for (ai = 0; ai < (int) ct.nparams; ++ai) if (!wasm_ld (&body, &lc, wargs [ai])) { fail = "call arg ld"; goto done; } }
								wasm_i32_const (&body, (gint32) (intptr_t) aot_rgctx);
								wasm_i32_const (&body, (gint32) (intptr_t) aot_addr);
								wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) nti); wasm_uleb (&body, 0);
								if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
							} else {
								memset (&eht, 0, sizeof (eht)); eht.nparams = 1; eht.params [0] = WASM_I32; eht.ret = WASM_VOID;
								for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &eht)) { ehti = 2 + k; break; }
								if (ehti < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = eht; ehti = 2 + nextra++; }
								uses_eh_tag = TRUE;
								eh_type_idx = ehti;
								/* resume-state model: wrap the AOT call in wasm try/catch <x.e>; a throwing callee is caught
								 * by mono_wasm_jit_aot_caught (-> interp resume-state) + the method bails via EMIT_PENDING_EXC_CHECK. */
								wasm_op (&body, WASM_OP_TRY); wasm_u8 (&body, 0x40);   /* try (void) */
								{ int *wargs = (int *) call->call_info; for (ai = 0; ai < (int) ct.nparams; ++ai) if (!wasm_ld (&body, &lc, wargs [ai])) { fail = "call arg ld"; goto done; } }
								wasm_i32_const (&body, (gint32) (intptr_t) aot_rgctx);   /* rgctx (ftndesc.arg) */
								wasm_i32_const (&body, (gint32) (intptr_t) aot_addr);    /* AOT target table index (cinfo->addr) */
								wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) nti); wasm_uleb (&body, 0);
								if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }   /* store ret inside the try */
								wasm_op (&body, WASM_OP_CATCH); wasm_uleb (&body, 0);   /* catch <tag 0>: pushes the C++ exc ptr (i32) */
								{ extern void mono_wasm_jit_aot_caught (void *exc); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_aot_caught); }
								wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ehti); wasm_uleb (&body, 0);   /* table 0 */
								wasm_op (&body, WASM_OP_END);   /* end try/catch */
								EMIT_PENDING_EXC_CHECK ();
							} }
							{ extern void mono_wasm_jit_log_main (const char *msg); extern int mono_wasm_jit_verbose; static int n_il = 0; if (mono_wasm_jit_verbose >= 3 && n_il++ < 64) { char *cn = mono_method_get_full_name (call->method); char *m2 = g_strdup_printf ("WASM_JIT_INLINE_AOT[%d] %s -> %s", n_il, mname, cn); mono_wasm_jit_log_main (m2); g_free (m2); g_free (cn); } }
							break;
						}
					}
					/* Callee NOT wasm-JITted (no f-slot): route just this call through the interpreter
					 * via mono_wasm_jit_call_interp, keeping the rest of the method JITted — instead of
					 * bailing the whole method (the big real-MC coverage lever: hot JITted methods almost
					 * always call some cold / un-JIT-able callee). Fetch the per-thread scratch buffer,
					 * spill scalar args into it (this at slot 0 if instance, then params at slot i, 8B
					 * each), call the helper, then load the result from buf+WJ_SCRATCH_RET_OFF. */
					extern gpointer mono_wasm_jit_scratch (void);
					extern int mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf); /* ->1 if callee threw */
					WasmFuncType ts, ti;     /* ()->i32 (get scratch); (i32,i32)->i32 (call_interp: threw?) */
					int tsi = -1, tii = -1, bi;
					{
						extern int mono_wasm_jit_residual_mode;
						extern __thread MonoMethod *mono_wasm_jit_last_blocking_callee;
						int rm = mono_wasm_jit_residual_mode;
						/* record the blocking callee so the trigger can eagerly form the island (see the relay decl) */
						/* rgctx calls (MONO_WASM_JIT_RGCTX): if INLINE_AOT couldn't take it (gsharedvt-variable, or
						 * inline-aot off/byref), keep the method JITted by routing this one call through the residual
						 * (mono_wasm_jit_call_interp derives the context from the concrete callee) rather than bailing
						 * the whole method at the RESIDUAL=0 gate. The rgctx call is the cold catch-block edge; the hot
						 * try-body still JITs. */
						if (rm == 0 && !((MonoCallInst*)ins)->rgctx_reg) {
							/* jit->AOT fastpath: an AOT-compiled callee with no f-slot is STILL directly callable
							 * via its native AOT body — so don't bail the whole method. Fall through to the
							 * residual emit, which routes through interp_entry->do_jit_call to the AOT body
							 * (gated by mono_wasm_jit_aot_residual). This breaks the RESIDUAL=0 cascade at the
							 * AOT'd java.* leaves (Math.sqrt, base ctors, java.util.*) that nearly every MC
							 * call-tree bottoms out in (94% of all bails). Interp-only callees with no f-slot
							 * still bail, so the island force-JITs them bottom-up as before. */
							extern int mono_wasm_jit_aot_residual;
							extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
							if (!(mono_wasm_jit_aot_residual && mono_interp_jit_call_supported (call->method, csig))) {
								/* Lever B (MONO_WASM_JIT_RESIDUAL_PERM): if the blocker is PERMANENTLY un-JITtable
								 * (slot==-1: EH/opcode/sig — it will NEVER get an f-slot, so the island can never
								 * close around it), route just this edge through the interp residual instead of
								 * bailing the whole caller. Keeps a hot island JITted around a cold perm-blocker.
								 * A not-yet-jitted callee still bails (the island should pull it in bottom-up). */
								extern int mono_wasm_jit_residual_perm;
								extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
								if (!(mono_wasm_jit_residual_perm && mono_wasm_jit_callee_perm_unjittable (call->method))) {
									mono_wasm_jit_last_blocking_callee = call->method;
									fail = "callee not jitted (residual off)";
									goto done;
								}
								/* else: perm-unjittable blocker + RESIDUAL_PERM -> fall through, emit interp residual */
							}
							/* else: AOT'd callee -> fall through and emit the (AOT-routed) residual call */
						}
						/* bisection gates (see mono_wasm_jit_residual_mode): isolate which marshalling axis
						 * breaks real code by restricting which call shapes the residual handles */
						if (rm == 2 && ct.ret != WASM_VOID) { fail = "callee not jitted (residual: non-void ret)"; goto done; }
						if (rm == 3 && csig->param_count > 0) { fail = "callee not jitted (residual: has params)"; goto done; }
						if (rm == 4 && csig->hasthis) { fail = "callee not jitted (residual: instance)"; goto done; }
						if (rm == 5 && csig->param_count > 0 && ct.ret != WASM_VOID) { fail = "callee not jitted (residual: params+nonvoid)"; goto done; }
					}
					/* No-rebuild bisection: bail residuals whose callee simple-name is listed in
					 * MONO_WASM_JIT_RESIDUAL_SKIP, to pin which specific callee corrupts real code. */
					{ extern gboolean mono_wasm_jit_residual_name_skipped (const char *name);
					  if (call->method->name && mono_wasm_jit_residual_name_skipped (call->method->name)) { fail = "residual skip (env)"; goto done; } }
					if (!call->call_info) { fail = "no captured call args"; goto done; }
					/* The residual routes the call through interp_entry, whose BYREF marshalling is
					 * delicate: byref-of-primitive RETURNS are mis-written by interp_entry's
					 * stackval_to_data_sign_ext (it sign-extends as the element type, not as a pointer),
					 * and byref args need pointer-value handling. Until that's hardened, bail byref
					 * signatures here to the interpreter (the pre-residual behaviour) rather than risk
					 * memory corruption — critical for Unsafe.Add/AddByteOffset etc. (byref-heavy, used
					 * everywhere). The DIRECT f-slot path above handles byref fine (pure wasm pointer
					 * pass, no interp_entry), so a JITted byref callee still upgrades. */
					if (m_type_is_byref (csig->ret)) { fail = "residual byref ret"; goto done; }
					for (bi = 0; bi < (int) csig->param_count; ++bi)
						if (m_type_is_byref (csig->params [bi])) { fail = "residual byref arg"; goto done; }
					if ((int) ct.nparams * 8 > 192 /*WJ_SCRATCH_RET_OFF*/) { fail = "residual nargs"; goto done; }
					/* Don't residual reflection methods: running e.g. MonoMethodInfo:get_parameter_info via
					 * interp_entry while a dynamic type is mid-Finish can re-enter that type's Finish. Bail
					 * these to the interpreter; keep the rest. */
					{ const char *_ns = m_class_get_name_space (call->method->klass);
					  if (_ns && !strncmp (_ns, "System.Reflection", 17)) { fail = "residual reflection"; goto done; } }
					/* Diagnostic: list the params+non-void residual sites. Only reached in modes 1/4 (mode 5
					 * bails this shape above). Lets a =1/=4 run name a suspect caller->callee. */
					if (mono_wasm_jit_stats && csig->param_count > 0 && ct.ret != WASM_VOID) {
						char *cn = mono_method_get_full_name (call->method);
						char b [512]; snprintf (b, sizeof b, "WASM_JIT_RESIDUAL_PNV %s -> %s", mname, cn ? cn : "?");
						mono_wasm_jit_log_main (b); g_free (cn);
					}
					memset (&ts, 0, sizeof (ts)); ts.ret = WASM_I32; ts.nparams = 0;
					memset (&ti, 0, sizeof (ti)); ti.ret = WASM_I32; ti.nparams = 2; ti.params [0] = WASM_I32; ti.params [1] = WASM_I32;
					for (k = 0; k < nextra; ++k) {
						if (tsi < 0 && functype_eq (&extra_types [k], &ts)) tsi = 2 + k;
						if (tii < 0 && functype_eq (&extra_types [k], &ti)) tii = 2 + k;
					}
					if (tsi < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = ts; tsi = 2 + nextra++; }
					if (tii < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = ti; tii = 2 + nextra++; }
					uses_calls = TRUE;
					/* $scratch = mono_wasm_jit_scratch() */
					wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_scratch);
					wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tsi); wasm_uleb (&body, 0);
					wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
					{
						int *wargs = (int *) call->call_info;
						for (ai = 0; ai < (int) ct.nparams; ++ai) {
							WasmOpcode sop; int al;
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wasm_ld (&body, &lc, wargs [ai])) { fail = "residual arg ld"; goto done; }
							switch (ct.params [ai]) {
							case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
							case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
							case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
							case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
							default: fail = "residual arg type"; goto done;
							}
							wasm_op (&body, sop); wasm_memarg (&body, (guint32) al, (guint32) (ai * 8));
						}
					}
					/* mono_wasm_jit_call_interp(method, $scratch) */
					wasm_i32_const (&body, (gint32) (intptr_t) call->method);
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
					wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_interp);
					wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tii); wasm_uleb (&body, 0);
					/* call_interp returns 1 if the callee threw: the result slot is stale, so abort this
					 * method now (return a dummy of its return type). The interp sees the resume-state
					 * after the e-thunk returns and unwinds via `goto resume`. */
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					switch (ret_vt) {
					case WASM_I32: wasm_i32_const (&body, 0); break;
					case WASM_I64: wasm_i64_const (&body, 0); break;
					case WASM_F32: wasm_f32_const (&body, 0); break;
					case WASM_F64: wasm_f64_const (&body, 0); break;
					default: break; /* void: return nothing */
					}
					EMIT_REF_LEAVE ();
					wasm_op (&body, WASM_OP_RETURN);
					wasm_op (&body, WASM_OP_END);
					if (ct.ret != WASM_VOID) {
						WasmOpcode lop; int al;
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						switch (ct.ret) {
						case WASM_I32: lop = WASM_OP_I32_LOAD; al = 2; break;
						case WASM_I64: lop = WASM_OP_I64_LOAD; al = 3; break;
						case WASM_F32: lop = WASM_OP_F32_LOAD; al = 2; break;
						case WASM_F64: lop = WASM_OP_F64_LOAD; al = 3; break;
						default: fail = "residual ret type"; goto done;
						}
						wasm_op (&body, lop); wasm_memarg (&body, (guint32) al, 192 /*WJ_SCRATCH_RET_OFF*/);
						/* Normalize sub-word integer returns to the full i32 the IR consumer expects.
						 * interp_entry writes the result via stackval_to_data, which for a bool/i1/u1
						 * return writes only 1 byte and for i2/u2/char only 2 bytes (interp.c) — so a
						 * full-width i32.load above reads stale upper bytes from the per-thread scratch
						 * buffer. Sign/zero-extend per the C# return type to match exactly what a normal
						 * interp call would leave in the result vreg (idempotent if the write was already
						 * full-width, as in the non-llvm_only sign-ext path). */
						if (ct.ret == WASM_I32) {
							MonoType *rt = mini_get_underlying_type (csig->ret);
							switch (rt->type) {
							case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1:
								wasm_i32_const (&body, 0xff); wasm_op (&body, WASM_OP_I32_AND); break;
							case MONO_TYPE_I1:
								wasm_op (&body, WASM_OP_I32_EXTEND8_S); break;
							case MONO_TYPE_CHAR: case MONO_TYPE_U2:
								wasm_i32_const (&body, 0xffff); wasm_op (&body, WASM_OP_I32_AND); break;
							case MONO_TYPE_I2:
								wasm_op (&body, WASM_OP_I32_EXTEND16_S); break;
							default: break;
							}
						}
						if (!wasm_st (&body, &lc, ins->dreg)) { fail = "residual dreg"; goto done; }
					}
				}
#else
				else { fail = "callee not jitted"; goto done; }
#endif
				break;
			}
			case OP_CALL_REG: case OP_VOIDCALL_REG: case OP_FCALL_REG: case OP_LCALL_REG: case OP_RCALL_REG:
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE: {
				/* Indirect/virtual call: call_indirect through the target in sreg1 (a wasm table
				 * index = ftndesc.addr); rgctx (ftndesc.arg) passed as a trailing i32 param. The
				 * slot holds the receiver's entry, so the dispatch is JITted even if the callee
				 * runs in the interpreter. */
				MonoCallInst *call = (MonoCallInst *) ins;
				MonoMethodSignature *csig = call->signature;
				WasmFuncType ct;
				int type_idx = -1, k, ai, nmeth;
				gboolean is_membase = ins->opcode == OP_CALL_MEMBASE || ins->opcode == OP_VOIDCALL_MEMBASE || ins->opcode == OP_FCALL_MEMBASE || ins->opcode == OP_LCALL_MEMBASE || ins->opcode == OP_RCALL_MEMBASE;
				if (!csig) { fail = "callreg null sig"; goto done; }
				/* rgctx on an indirect/virtual call: the rgctx is a separate outarg the wasm backend doesn't
				 * forward, and neither the inline-IC virtual resolve nor the indirect dispatch carries it. Bail
				 * the whole method (rare; the common static-generic case — IKVM MapException<T> — is a DIRECT
				 * OP_CALL, handled above via INLINE_AOT rgctx passthrough / residual). */
				if (call->rgctx_reg) { fail = "rgctx indirect/virtual call"; goto done; }
				/* Virtual method call (interp-only runtime): the vtable slot holds a fixed-sig
				 * trampoline stub, not a callable entry, so we can't call_indirect it directly. Lower
				 * to a call of mono_wasm_jit_vcall_i4(this, base_method) — a C helper that resolves the
				 * override for the receiver and invokes it via the interpreter (interp_entry). v1
				 * supports the (this)->i4 shape (the common hot polymorphic call, e.g. getBlockState);
				 * other shapes bail to interp. */
					if (is_membase && call->method && (call->method->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
						/* Virtual / interface call under the interp-only runtime, with a fast direct-f-slot path
						 * (gap #3). A C helper resolves the override for the receiver, then:
						 *   - FAST: if the override is itself wasm-JITted, the helper syncs THIS thread's function
						 *     table (so the slot is populated locally — the table is PER-THREAD) and returns its
						 *     scalar `f` f-slot; the JITted caller then call_indirects straight into it (this+args
						 *     -> ret), no interp re-entry. This kills the per-call interp_entry "virtual storm".
						 *   - FALLBACK (un-JITted override, or a synchronized wrapper, which never gets an f-slot):
						 *     marshal (this + args) into the per-thread scratch buffer and run the override through
						 *     the interpreter (interp_entry, which writes the scalar result into scratch + returns 1
						 *     if it threw) — the same proven path the direct residual uses.
						 * The helper carries a per-call-site inline cache (vtable -> InterpMethod*) that skips the
						 * expensive virtual resolve on a monomorphic hit (the dominant per-call cost). We cache the
						 * RESOLVE, not the f-slot, and still per-thread-sync before dispatching the f-slot — so we
						 * never call_indirect a slot absent on the calling thread (the hazard a cached-f-slot inline
						 * IC would hit: the table is PER-THREAD, a slot cached by one worker can be absent in another,
						 * and the i32 pair can tear -> a bad call_indirect that traps and kills the worker). */
						extern gpointer mono_wasm_jit_scratch (void);
						extern int mono_wasm_jit_vcall_resolve_fslot (MonoObject *this_obj, MonoMethod *base_method, guint8 *scratch, gpointer ic);
						extern int mono_wasm_jit_call_interp (MonoMethod *m, guint8 *buf);
						WasmFuncType vts, vtrf, vtd, ftd; int vtsi = -1, vtrfi = -1, vtdi = -1, ftdi = -1, vk, ai, n2, this_vr;
						WasmValtype pp [WASM_FUNCTYPE_MAX_PARAMS], rv; int npp = 0; gpointer vic;
						if (!csig->hasthis) { fail = "vcall not instance"; goto done; }
						if (!call->call_info) { fail = "no captured vcall args"; goto done; }
						this_vr = ((int *) call->call_info) [0];
						{ extern int mono_wasm_jit_virtual; if (!mono_wasm_jit_virtual) { fail = "virtual disabled (env)"; goto done; } }
						/* byref args/ret go through interp_entry's delicate by-pointer marshalling, which the direct
						 * residual also bails (stackval_to_data mis-writes byref-of-primitive returns); bail here too. */
						if (m_type_is_byref (csig->ret)) { fail = "vcall byref ret"; goto done; }
						for (ai = 0; ai < (int) csig->param_count; ++ai)
							if (m_type_is_byref (csig->params [ai])) { fail = "vcall byref arg"; goto done; }
						/* arg valtypes: this + params (scalars only; vtype/unsupported -> bail) */
						pp [npp++] = WASM_I32; /* this */
						for (ai = 0; ai < (int) csig->param_count; ++ai) {
							WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
							if (pv == 0 || pv == WASM_VOID) { fail = "vcall arg type"; goto done; }
							if (npp >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "vcall nargs"; goto done; }
							pp [npp++] = pv;
						}
						if (csig->ret->type == MONO_TYPE_VOID) rv = WASM_VOID;
						else { rv = wasm_valtype_of_type (csig->ret); if (rv == 0 || rv == WASM_VOID) { fail = "vcall ret type"; goto done; } }
						/* functypes: vts ()->i32 (scratch); vtrf (i32,i32,i32,i32)->i32 (resolve_fslot: this,base,scratch,ic);
						 * vtd (i32,i32)->i32 (call_interp fallback); ftd this+params->ret (the override's scalar `f`). */
						memset (&vts, 0, sizeof (vts)); vts.nparams = 0; vts.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vts)) { vtsi = 2 + vk; break; }
						if (vtsi < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = vts; vtsi = 2 + nextra++; }
						memset (&vtrf, 0, sizeof (vtrf)); vtrf.params [0] = WASM_I32; vtrf.params [1] = WASM_I32; vtrf.params [2] = WASM_I32; vtrf.params [3] = WASM_I32; vtrf.nparams = 4; vtrf.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtrf)) { vtrfi = 2 + vk; break; }
						if (vtrfi < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtrf; vtrfi = 2 + nextra++; }
						memset (&vtd, 0, sizeof (vtd)); vtd.params [0] = WASM_I32; vtd.params [1] = WASM_I32; vtd.nparams = 2; vtd.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtd)) { vtdi = 2 + vk; break; }
						if (vtdi < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtd; vtdi = 2 + nextra++; }
						memset (&ftd, 0, sizeof (ftd)); for (vk = 0; vk < npp; ++vk) ftd.params [vk] = pp [vk]; ftd.nparams = (guint32) npp; ftd.ret = rv;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &ftd)) { ftdi = 2 + vk; break; }
						if (ftdi < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = ftd; ftdi = 2 + nextra++; }
						uses_calls = TRUE;
						n2 = csig->param_count + 1; /* this + params */
						/* per-call-site inline cache (8 bytes: [i32 vtable, i32 InterpMethod*]) in shared memory;
						 * resolve_fslot caches the virtual resolve here, skipping it on a monomorphic hit. */
#ifdef HOST_BROWSER
						{ extern gpointer mono_wasm_jit_alloc_ic (void); vic = mono_wasm_jit_alloc_ic (); }
#else
						vic = (gpointer) (intptr_t) 0x7ff0;
#endif
						/* --- INLINE MONOMORPHIC IC FAST PATH (skip the resolve_fslot C helper on a hit) ---
						 * ~97.8% of MC vcalls hit the IC; each otherwise pays a C call (resolve_fslot: atomic load
						 * + checks + sync_thread) before the real call_indirect. Do the hit inline in wasm:
						 *   vtab = *this; ic = atomic_load(&vic);
						 *   if (vtab == (i32)ic) { im = (i32)(ic>>32); fslot = im->wasm_jit_fslot;
						 *     if (fslot != 0 && table[fslot] != null) { call_indirect(fslot); skip slow } }
						 * The table[fslot] != null LIVENESS check is essential + is what makes inline dispatch
						 * MT-safe: the IC is shared but the function table is PER-THREAD, so a slot another thread
						 * cached may be absent on THIS thread — fall back to the C helper (which syncs this thread)
						 * instead of trapping "null function". Atomic i64 load avoids a torn (vtable,im) read. */
						{
#ifdef HOST_BROWSER
							extern int mono_wasm_jit_imethod_fslot_off (void);
							int fslot_off = mono_wasm_jit_imethod_fslot_off ();
#else
							int fslot_off = 0x40; /* placeholder for offline encoder validation (real offset only matters at runtime) */
#endif
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $after (void) */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /*   $do_slow (void) */
							/* vtab = *(this + 0) */
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this ld"; goto done; }
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
							/* ic = i64.atomic.load(&vic); $ic = ic; if ((i32)ic != vtab) -> $do_slow */
							wasm_i32_const (&body, (gint32) (intptr_t) vic);
							wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 0);   /* i64.atomic.load = 0xfe 0x11 (align 3) */
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
							wasm_op (&body, WASM_OP_I32_WRAP_I64);
							wasm_op (&body, WASM_OP_I32_NE);
							wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
							/* im = (i32)(ic >> 32); fslot = im->wasm_jit_fslot; if (fslot == 0) -> $do_slow */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
							wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) fslot_off);
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_I32_EQZ);
							wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
							/* liveness: if (table[fslot] is null on THIS thread) -> $do_slow */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_TABLE_GET); wasm_uleb (&body, 0);
							wasm_op (&body, WASM_OP_REF_IS_NULL);
							wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
							/* FAST: push this+args (fresh from vregs), fslot, call_indirect(ftd); store; skip slow */
							for (ai = 0; ai < n2; ++ai)
								if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "ic fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ftdi); wasm_uleb (&body, 0);
							if (rv != WASM_VOID) { if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ic fast dreg"; goto done; } }
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> $after (skip the C-helper slow path) */
							wasm_op (&body, WASM_OP_END);                            /* end $do_slow */
						}
						/* $scratch = mono_wasm_jit_scratch() */
#ifdef HOST_BROWSER
						wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_scratch);
#else
						wasm_i32_const (&body, 0x7ffb);
#endif
						wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) vtsi); wasm_uleb (&body, 0);
						wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
						/* fslot = mono_wasm_jit_vcall_resolve_fslot(this, base, scratch): resolve the override (synchronized
						 * wrapper substituted), stash the target MonoMethod* at scratch+200 (for the call_interp fallback),
						 * and return the override's scalar `f` f-slot if it's itself wasm-JITted (else 0). Done BEFORE spilling
						 * ref args: resolve+transform can GC, and until the spill the ref args still live on the GC-scanned
						 * shadow stack (so a GC moves them safely; `this` is by value). Stash fslot at scratch+208. */
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* store-addr base for fslot@208 */
						if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall this ld"; goto done; }
#ifdef HOST_BROWSER
						wasm_i32_const (&body, (gint32) (intptr_t) call->method);
#else
						wasm_i32_const (&body, 0x7ffd);
#endif
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* 3rd arg: scratch */
						wasm_i32_const (&body, (gint32) (intptr_t) vic);                           /* 4th arg: inline cache */
#ifdef HOST_BROWSER
						wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_vcall_resolve_fslot);
#else
						wasm_i32_const (&body, 0x7ffa);
#endif
						wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) vtrfi); wasm_uleb (&body, 0); /* -> fslot */
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 208 /* scratch temp: f-slot */);
						/* if (fslot != 0) FAST: call_indirect straight into the override's wasm `f`; else call_interp. */
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);
						wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							/* FAST PATH: push this + args fresh from their vregs (GC-scanned shadow stack; resolve_fslot did
							 * not spill, so no GC-invisible window), then the f-slot table index, call_indirect(ftd). */
							for (ai = 0; ai < n2; ++ai)
								if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "vcall fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);            /* f-slot = table index */
							wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ftdi); wasm_uleb (&body, 0);
							if (rv != WASM_VOID) { if (!wasm_st (&body, &lc, ins->dreg)) { fail = "vcall fast dreg"; goto done; } }
						wasm_op (&body, WASM_OP_ELSE);
							/* FALLBACK: spill this + each arg into scratch[k*8] (this at 0, arg i at (1+i)*8), then re-enter
							 * the interp via call_interp(target, scratch). */
							for (ai = 0; ai < n2; ++ai) {
								WasmOpcode sop; guint32 al2;
								switch (pp [ai]) {
								case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
								case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
								case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
								default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
								}
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "vcall arg ld"; goto done; }
								wasm_op (&body, sop); wasm_memarg (&body, al2, (guint32) (ai * 8));
							}
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 200);            /* target MonoMethod* */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* scratch buffer */
#ifdef HOST_BROWSER
							wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_interp);
#else
							wasm_i32_const (&body, 0x7ffc);
#endif
							wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) vtdi); wasm_uleb (&body, 0);
							wasm_op (&body, WASM_OP_DROP);
							if (rv != WASM_VOID) {
								WasmOpcode lop; guint32 al2;
								switch (rv) {
								case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
								case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
								case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
								default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
								}
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_op (&body, lop); wasm_memarg (&body, al2, 192 /* WJ_SCRATCH_RET_OFF */);
								if (!wasm_st (&body, &lc, ins->dreg)) { fail = "vcall dreg"; goto done; }
							}
						wasm_op (&body, WASM_OP_END);   /* end the fslot if/else (slow path) */
						wasm_op (&body, WASM_OP_END);   /* end $after — the inline-IC fast path branched here, past the slow path */
						EMIT_PENDING_EXC_CHECK ();
						break;
					}
				/* Raw indirect call (callreg / non-virtual membase): the target in sreg1 is a runtime
				 * value (e.g. ftndesc.addr) which, under auto-JIT (non-llvmonly), isn't a reliable wasm
				 * table index — so bail the method to the interpreter. The VIRTUAL subcase above is NOT
				 * gated: it dispatches via the inline IC + interp-entry-thunk fallback, which always
				 * resolves to a callable slot. */
				{
					extern __thread gboolean mono_wasm_jit_force;
					if (mono_wasm_jit_force) { fail = "raw indirect call under auto-JIT"; goto done; }
				}
				/* rgctx (ftndesc.arg) is folded into csig as a trailing param + call->args, so it's
				 * handled like any other arg — no separate rgctx_arg_reg (that field is LLVM-only). */
				memset (&ct, 0, sizeof (ct));
				if (csig->hasthis) { if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "creg nargs"; goto done; } ct.params [ct.nparams++] = WASM_I32; }
				for (ai = 0; ai < (int) csig->param_count; ++ai) {
					WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
					if (pv == 0 || pv == WASM_VOID) { fail = "creg arg type"; goto done; }
					if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "creg nargs"; goto done; }
					ct.params [ct.nparams++] = pv;
				}
				if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
				else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) { fail = "creg ret type"; goto done; } }
				for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = 2 + k; break; }
				if (type_idx < 0) { if (nextra >= 32) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = 2 + nextra++; }
				uses_calls = TRUE;
				nmeth = csig->param_count + (csig->hasthis ? 1 : 0);
				if (!call->call_info) { fail = "no captured callreg args"; goto done; }
				for (ai = 0; ai < nmeth; ++ai)
					if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "creg arg ld"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "creg target ld"; goto done; }
					if (is_membase) { wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) ins->inst_offset); } /* target = *(base + offset) */
				wasm_op (&body, WASM_OP_CALL_INDIRECT);
				wasm_uleb (&body, (guint32) type_idx);
				wasm_uleb (&body, 0);
				if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "creg dreg"; goto done; }
				EMIT_PENDING_EXC_CHECK ();
				break;
			}
			default:
				fail = "unsupported opcode";
				fail_op = ins->opcode;
				goto done;
			}
		}

		if (!terminated) {
			/* fall through to the next block, or return at the method exit */
			if (bb == cfg->bb_exit || !bb->next_bb) {
				if (sig->ret->type != MONO_TYPE_VOID && (!cfg->ret || !wasm_ld (&body, &lc, cfg->ret->dreg))) {
					/* No return value to load here = this exit is unreachable (e.g. a method that ALWAYS
					 * throws — every path took OP_THROW's dummy-return). Emit unreachable to satisfy the
					 * function result type instead of bailing ("ret epilogue"). Reachable non-void exits
					 * load the ret value (wasm_ld above) + fall to the RETURN below. */
					wasm_op (&body, WASM_OP_UNREACHABLE);
				} else {
					EMIT_REF_LEAVE ();
					wasm_op (&body, WASM_OP_RETURN);
				}
			} else {
				GOTO (bb->next_bb, loop_depth);
			}
		}
	}

	wasm_op (&body, WASM_OP_END);          /* close loop */
	wasm_op (&body, WASM_OP_UNREACHABLE);  /* loop never falls through */

	if (eh_on) {
		/* In-method EH landing pad: `catch <x.e>` of the wrapping try. The C++ exc ptr (i32) is pushed.
		 * h = eh_dispatch(table, $blk): the throwing bb's matching catch handler bbidx, or -1. h<0 ->
		 * rethrow (propagate to an outer try / the interp boundary). h>=0 -> claim the C++ exc + set
		 * $blk=h + br $outer to re-dispatch into the handler bb. */
		wasm_op (&body, WASM_OP_CATCH); wasm_uleb (&body, 0);   /* tag 0 = x.e */
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) eh_exc_idx);
#ifdef HOST_BROWSER
		wasm_i32_const (&body, (gint32) (intptr_t) eh_table);
#else
		wasm_i32_const (&body, 0x7ff0);   /* offline: placeholder EH-table addr */
#endif
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dispatch_idx);   /* $blk = the throwing bb */
#ifdef HOST_BROWSER
		{ extern int mono_wasm_jit_eh_dispatch (WasmEhTable *t, int blk); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_eh_dispatch); }
#else
		wasm_i32_const (&body, 0x7ff1);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_dispatch_ti); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) eh_h_idx);
		wasm_i32_const (&body, 0);
		wasm_op (&body, WASM_OP_I32_LT_S);
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);     /* h < 0: no local handler */
		/* the method escapes via rethrow -> pop its il_state island first */
#ifdef HOST_BROWSER
		{ extern void mono_wasm_jit_leave_island (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_leave_island); }
#else
		wasm_i32_const (&body, 0x7ff7);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_endcatch_ti); wasm_uleb (&body, 0);   /* leave_island ()->void */
		wasm_op (&body, WASM_OP_RETHROW); wasm_uleb (&body, 1); /* depth 1 = the enclosing try -> re-propagate */
		wasm_op (&body, WASM_OP_END);                          /* end if */
		/* matched: claim+release the C++ exception (balance the cxa handler count), then dispatch. */
		{ extern int mono_wasm_jit_eh_nocxa;
		if (!mono_wasm_jit_eh_nocxa) {   /* MONO_WASM_JIT_EH_NOCXA=1 skips the cxa claim (bisection) */
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_exc_idx);
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_begin_catch (void *e); wasm_i32_const (&body, (gint32) (intptr_t) mono_jiterp_begin_catch); }
#else
		wasm_i32_const (&body, 0x7ff2);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_type_idx); wasm_uleb (&body, 0);   /* (i32)->void */
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_end_catch (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_jiterp_end_catch); }
#else
		wasm_i32_const (&body, 0x7ff3);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eh_endcatch_ti); wasm_uleb (&body, 0);   /* ()->void */
		} }
		/* milestone 2c: mark the EXCEPTION path for any finally handler we dispatch to — finally_ind = -1 so
		 * its OP_ENDFINALLY re-raises (vs a normal leave, where OP_CALL_HANDLER set finally_ind to a real
		 * continuation bb). Harmless for catch handlers (they never read finally_ind). Finally methods only. */
		if (eh_has_finally) {
			wasm_i32_const (&body, -1);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) finally_ind_idx);
		}
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);   /* $blk = handler bbidx */
		wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);     /* depth 1 = $outer loop -> re-dispatch */
		wasm_op (&body, WASM_OP_END);          /* close try */
		wasm_op (&body, WASM_OP_END);          /* close $outer loop */
		wasm_op (&body, WASM_OP_UNREACHABLE);  /* $outer loop never falls through */
	}

#undef GOTO
#undef BIN
#undef BINI32
#undef BINI64
#undef BINI64L

	/* entry thunk: (args_ptr, ret_ptr)->void — reads each arg from the interp stackval at
	 * args_ptr + i*sizeof(stackval), calls the method (func 0), stores the result at ret_ptr.
	 * Lets the interpreter invoke any signature uniformly via e(args, ret). */
	{
		WasmBuf ethunk;
		gboolean has_ret = (ret_vt != WASM_VOID);
		wasm_buf_init (&ethunk);
		if (has_ret)
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 1); /* ret_ptr (store address) */
		for (i = 0; i < nargs; ++i) {
			WasmOpcode ld; guint32 al;
			switch (param_types [i]) {
			case WASM_I64: ld = WASM_OP_I64_LOAD; al = 3; break;
			case WASM_F32: ld = WASM_OP_F32_LOAD; al = 2; break;
			case WASM_F64: ld = WASM_OP_F64_LOAD; al = 3; break;
			default:       ld = WASM_OP_I32_LOAD; al = 2; break;
			}
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 0); /* args_ptr */
			wasm_op (&ethunk, ld);
			wasm_memarg (&ethunk, al, (guint32) (i * 8)); /* 8 = sizeof(stackval) */
		}
		wasm_op (&ethunk, WASM_OP_CALL);
		wasm_uleb (&ethunk, 0); /* call the method (func index 0) */
		if (has_ret) {
			WasmOpcode st; guint32 al;
			switch (ret_vt) {
			case WASM_I64: st = WASM_OP_I64_STORE; al = 3; break;
			case WASM_F32: st = WASM_OP_F32_STORE; al = 2; break;
			case WASM_F64: st = WASM_OP_F64_STORE; al = 3; break;
			default:       st = WASM_OP_I32_STORE; al = 2; break;
			}
			wasm_op (&ethunk, st);
			wasm_memarg (&ethunk, al, 0); /* *ret_ptr = result */
		}
		wasm_buf_init (&out);
		wasm_module_method_and_entry (param_types, nargs, ret_vt, groups, 4, &body, &ethunk, extra_types, (guint32) nextra, uses_calls, uses_eh_tag, (guint32) (eh_type_idx < 0 ? 0 : eh_type_idx), &out);
		wasm_buf_free (&ethunk);
	}

#ifdef HOST_BROWSER
	if (g_getenv ("MONO_WASM_JIT_DUMP_ONLY") != NULL) {
		/* debug: forward the emitted module as hex to the UI console but DON'T register it, so the
		 * method runs in the interpreter — lets us inspect JIT-path codegen without executing it. */
		GString *hex = g_string_sized_new (out.len * 2 + 1);
		for (i = 0; i < (int) out.len; ++i)
			g_string_append_printf (hex, "%02x", out.data [i]);
		{ char *hb = g_strdup_printf ("WASM_JIT_HEX %s : %u : %s", mname, out.len, hex->str); mono_wasm_jit_log_main (hb); g_free (hb); }
		g_string_free (hex, TRUE);
	} else if (g_getenv ("MONO_WASM_JIT_DUMP_EXIT") == NULL) {
		/* live runtime: allocate the (global) table slots + cache the emitted module bytes. The wasm
		 * function table is PER-THREAD for dynamically-added entries, so instantiation is deferred to
		 * the interp's first invoke on each thread (interp.c MINT_CALL → mono_wasm_jit_instantiate_local),
		 * which instantiates its own WebAssembly.Instance into its own table[e_slot]/[f_slot]. */
		int e_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
		int f_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
		if (e_slot > 0 && f_slot > 0) {
			void *cached = g_malloc ((gsize) out.len);
			char ierr [192]; ierr [0] = 0;
			memcpy (cached, out.data, out.len);
			/* Validate by instantiating once on THIS thread (also populates this thread's table).
			 * If the module is invalid (a codegen bug for some opcode shape), bail to the interpreter
			 * here rather than letting another thread trap on a placeholder slot at invoke time. */
			if (mono_wasm_jit_instantiate_local (e_slot, f_slot, cached, (int) out.len, ierr, (int) sizeof (ierr), &wj_inst_ms)) {
				mono_wasm_jit_last_bytes = cached;
				mono_wasm_jit_last_len = (int) out.len;
				mono_wasm_jit_last_slot = e_slot;
				/* EH methods now push their il_state in the PROLOGUE (mono_wasm_jit_enter_island), so a direct
				 * JIT->JIT f-slot call is visible to pass-1 too — re-expose the f-slot for direct dispatch. */
				mono_wasm_jit_last_fslot = f_slot;
				if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_REGISTERED); mono_wasm_jit_add (WJC_BYTES_GENERATED, (gint64) out.len); }
				{ extern void mono_wasm_jit_register (int e_slot, int f_slot, void *bytes, int len); mono_wasm_jit_register (e_slot, f_slot, cached, (int) out.len); }
				if (mono_wasm_jit_verbose >= 1) { char b [256]; snprintf (b, sizeof b, "WASM_JIT_REGISTERED %s e_slot=%d f_slot=%d len=%u", mname, e_slot, f_slot, (unsigned) out.len); mono_wasm_jit_log_main (b); }
			} else {
				g_free (cached);
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_INVALID);
				if (mono_wasm_jit_verbose >= 1) { char b [320]; snprintf (b, sizeof b, "WASM_JIT_INVALID %s e_slot=%d len=%u : %s", mname, e_slot, (unsigned) out.len, ierr); mono_wasm_jit_log_main (b); }
			}
		}
	} else
#endif
	{
		/* offline: dump module as hex for external validation */
		GString *hex = g_string_sized_new (out.len * 2 + 1);
		for (i = 0; i < (int) out.len; ++i)
			g_string_append_printf (hex, "%02x", out.data [i]);
		printf ("WASM_JIT_MODULE %s : %u bytes : %s\n", mname, out.len, hex->str);
		g_string_free (hex, TRUE);
	}
	wasm_buf_free (&out);

done:
	wasm_buf_free (&body);
	if (G_UNLIKELY (mono_wasm_jit_stats) && wj_gen_t0) {
		/* Compile-time accounting (Part 2): GENERATION = total emit time minus the JS instantiate
		 * (wj_inst_ms, folded into INSTANTIATION here) so the two halves don't overlap; COMPILE_ATTEMPTS
		 * counts every emit (success + bail + invalid). 100ns ticks -> microseconds (/10). */
		gint64 us = (mono_100ns_ticks () - wj_gen_t0) / 10 - (gint64) (wj_inst_ms * 1000.0);
		mono_wasm_jit_add (WJC_ELAPSED_GENERATION, us > 0 ? us : 0);
		mono_wasm_jit_add (WJC_ELAPSED_INSTANTIATION, (gint64) (wj_inst_ms * 1000.0));
		mono_wasm_jit_count (WJC_COMPILE_ATTEMPTS);
	}
#ifdef HOST_BROWSER
	/* Retriable iff the bail was "callee not jitted" (an ordering/island issue that may resolve once the
	 * callee JITs); EH clauses / unsupported opcodes / synchronized / byref are permanent. NULL fail
	 * (success) -> 0. The auto-JIT trigger reads this to pick a retry vs permanent slot state. */
	mono_wasm_jit_last_retriable = (fail && strstr (fail, "callee not jitted")) ? 1 : 0;
	/* Categorize the bail reason for the weighted vcall-residual breakdown (stored on InterpMethod by
	 * compile_publish only when the bail is permanent). Opcode bails carry the opcode number so the bench
	 * can name the dominant blocker (e.g. ldaddr 331); EH/sig/other get sentinel negatives. */
	if (!fail) mono_wasm_jit_last_bail = 0;
	else if (fail_op == OP_LDADDR) mono_wasm_jit_last_bail = -5;         /* ldaddr (needs addressable locals) */
	else if (fail_op == OP_LCOMPARE) mono_wasm_jit_last_bail = -6;       /* lcompare */
	else if (fail_op >= 0) mono_wasm_jit_last_bail = fail_op;            /* some other unsupported opcode (>0) */
	else if (strstr (fail, "EH clauses")) mono_wasm_jit_last_bail = -2;
	else if (strstr (fail, "arg type") || strstr (fail, "ret type")) mono_wasm_jit_last_bail = -3;   /* arg/ret sig type */
	/* Split what used to be the -4 "other" catch-all so the vcall-perm breakdown is actionable (the bench
	 * showed -4 was 99% of the perm vcall residual). Order: most specific class first. */
	else if (strstr (fail, "byref")) mono_wasm_jit_last_bail = -7;                                   /* byref arg/ret */
	else if (strstr (fail, "rgctx") || strstr (fail, "gshared")) mono_wasm_jit_last_bail = -8;        /* generic-shared / rgctx */
	else if (strstr (fail, "synchronized")) mono_wasm_jit_last_bail = -9;                             /* synchronized method/wrapper */
	else if (strstr (fail, "EH") || strstr (fail, "finally") || strstr (fail, "eh ") || strstr (fail, "eh-")) mono_wasm_jit_last_bail = -10; /* other EH reasons (not the -2 clause gate) */
	else mono_wasm_jit_last_bail = -4;                                   /* genuinely other (unsupported IR shape: reg/move/sig/indirect/...) */
#endif
	if (fail) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) {
			int cat;
			mono_wasm_jit_count (WJC_BAILED);
			/* Aggregated bail histogram (Part 4). Check opcode then "residual:" (the rm 2-5 shape bisection)
			 * BEFORE the generic "callee not jitted" (which the shape strings also contain). */
			if (fail_op >= 0) { cat = WJB_OPCODE; if (fail_op < WJ_BAIL_OPMAX) wj_bail_op_hist [fail_op]++; }
			else if (strstr (fail, "residual:")) cat = WJB_RESIDUAL_SHAPE;
			else if (strstr (fail, "callee not jitted")) cat = WJB_CALLEE_NOT_JITTED;
			else if (strstr (fail, "EH clauses")) cat = WJB_EH_CLAUSE;
			else if (strstr (fail, "arg type") || strstr (fail, "ret type")) cat = WJB_ARGRET_TYPE;
			else cat = WJB_SYNC_OTHER;
			wj_bail_hist [cat]++;
		}
#ifdef HOST_BROWSER
		if (mono_wasm_jit_verbose >= 2) { char b [256]; snprintf (b, sizeof b, "WASM_JIT_BAIL %s : %s (op=%d %s)", mname, fail, fail_op, fail_op >= 0 ? wj_opname (fail_op) : "-"); mono_wasm_jit_log_main (b); }
#else
		printf ("WASM_JIT_BAIL %s : %s (op=%d %s)\n", mname, fail, fail_op, fail_op >= 0 ? wj_opname (fail_op) : "-");
		fflush (stdout);
#endif
	}
	g_free (mname);
	if (g_getenv ("MONO_WASM_JIT_DUMP_EXIT") != NULL)
		exit (fail ? 1 : 0);
	/*
	 * v1 (in-runtime validation): mark the compile failed so the method falls back to
	 * the interpreter. We are validating emit+instantiate; installing the compiled code
	 * needs the calling-convention wrapper + a function-table slot (the next step).
	 */
	if (is_ok (cfg->error))
		mono_error_set_execution_engine (cfg->error, "wasm jit v1: validate-only, falling back to interp");
	cfg->exception_type = MONO_EXCEPTION_MONO_ERROR;
}

void
mono_arch_peephole_pass_1 (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

void
mono_arch_peephole_pass_2 (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

guint32
mono_arch_regalloc_cost (MonoCompile *cfg, MonoMethodVar *vmv)
{
	return 0;
}

GList *
mono_arch_get_allocatable_int_vars (MonoCompile *cfg)
{
	g_error ("mono_arch_get_allocatable_int_vars");
}

GList *
mono_arch_get_global_int_regs (MonoCompile *cfg)
{
	g_error ("mono_arch_get_global_int_regs");
}

void
mono_arch_allocate_vars (MonoCompile *cfg)
{
	g_error ("mono_arch_allocate_vars");
}

void
mono_arch_create_vars (MonoCompile *cfg)
{
	MonoMethodSignature *sig;
	CallInfo *cinfo;

	sig = mono_method_signature_internal (cfg->method);

	if (!cfg->arch.cinfo)
		cfg->arch.cinfo = get_call_info (cfg->mempool, sig);
	cinfo = (CallInfo *)cfg->arch.cinfo;

	// if (cinfo->ret.storage == ArgValuetypeInReg)
	// 	cfg->ret_var_is_local = TRUE;

	mini_get_underlying_type (sig->ret);
	if (cinfo->ret.storage == ArgValuetypeAddrInIReg || cinfo->ret.storage == ArgGsharedVTOnStack) {
		cfg->vret_addr = mono_compile_create_var (cfg, mono_get_int_type (), OP_ARG);
		if (G_UNLIKELY (cfg->verbose_level > 1)) {
			printf ("vret_addr = ");
			mono_print_ins (cfg->vret_addr);
		}
	}

	if (cfg->gen_sdb_seq_points)
		g_error ("gen_sdb_seq_points not supported");

	if (cfg->method->save_lmf) {
		cfg->create_lmf_var = TRUE;
		cfg->lmf_ir = TRUE;
	}
}

void
mono_arch_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	g_error ("mono_arch_emit_call");
}

void
mono_arch_emit_epilog (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_epilog");
}

void
mono_arch_emit_exceptions (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_exceptions");
}

MonoInst*
mono_arch_emit_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	return NULL;
}

void
mono_arch_emit_outarg_vt (MonoCompile *cfg, MonoInst *ins, MonoInst *src)
{
	g_error ("mono_arch_emit_outarg_vt");
}

guint8 *
mono_arch_emit_prolog (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_prolog");
}

void
mono_arch_emit_setret (MonoCompile *cfg, MonoMethod *method, MonoInst *val)
{
	MonoType *ret = mini_get_underlying_type (mono_method_signature_internal (method)->ret);

	if (!m_type_is_byref (ret)) {
		if (ret->type == MONO_TYPE_R4) {
			MONO_EMIT_NEW_UNALU (cfg, OP_RMOVE, cfg->ret->dreg, val->dreg);
			return;
		} else if (ret->type == MONO_TYPE_R8) {
			MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, cfg->ret->dreg, val->dreg);
			return;
		} else if (ret->type == MONO_TYPE_I8 || ret->type == MONO_TYPE_U8) {
			MONO_EMIT_NEW_UNALU (cfg, OP_LMOVE, cfg->ret->dreg, val->dreg);
			return;
		}
	}
	MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, cfg->ret->dreg, val->dreg);
}

void
mono_arch_flush_icache (guint8 *code, gint size)
{
}

LLVMCallInfo*
mono_arch_get_llvm_call_info (MonoCompile *cfg, MonoMethodSignature *sig)
{
	int i, n;
	CallInfo *cinfo;
	LLVMCallInfo *linfo;

	cinfo = get_call_info (cfg->mempool, sig);
	n = cinfo->nargs;

	linfo = mono_mempool_alloc0 (cfg->mempool, sizeof (LLVMCallInfo) + (sizeof (LLVMArgInfo) * n));

	if (cinfo->ret.storage == ArgVtypeAsScalar) {
		linfo->ret.storage = LLVMArgWasmVtypeAsScalar;
		linfo->ret.etype = cinfo->ret.etype;
		linfo->ret.esize = mono_class_value_size (mono_class_from_mono_type_internal (cinfo->ret.type), NULL);
	} else if (mini_type_is_vtype (sig->ret)) {
		/* Vtype returned using a hidden argument */
		linfo->ret.storage = LLVMArgVtypeRetAddr;
		// linfo->vret_arg_index = cinfo->vret_arg_index;
	} else {
		if (sig->ret->type != MONO_TYPE_VOID)
			linfo->ret.storage = LLVMArgNormal;
	}

	for (i = 0; i < n; ++i) {
		ArgInfo *ainfo = &cinfo->args[i];

		switch (ainfo->storage) {
		case ArgOnStack:
			linfo->args [i].storage = LLVMArgNormal;
			break;
		case ArgValuetypeAddrOnStack:
			linfo->args [i].storage = LLVMArgVtypeByRef;
			break;
		case ArgGsharedVTOnStack:
			linfo->args [i].storage = LLVMArgGsharedvtVariable;
			break;
		case ArgVtypeAsScalar:
			linfo->args [i].storage = LLVMArgWasmVtypeAsScalar;
			linfo->args [i].type = ainfo->type;
			linfo->args [i].etype = ainfo->etype;
			linfo->args [i].esize = mono_class_value_size (mono_class_from_mono_type_internal (ainfo->type), NULL);
			break;
		case ArgValuetypeAddrInIReg:
			g_error ("this is only valid for sig->ret");
			break;
		}
	}

	return linfo;
}

gboolean
mono_arch_tailcall_supported (MonoCompile *cfg, MonoMethodSignature *caller_sig, MonoMethodSignature *callee_sig, gboolean virtual_)
{
	return FALSE;
}

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

	g_assert (!etype || *etype);

	return TRUE;
}
