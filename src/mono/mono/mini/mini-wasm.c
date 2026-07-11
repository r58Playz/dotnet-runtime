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
#endif
/* The runtime wasm-JIT emit result (slots / bytes / bail / retriable / blockers) is no longer relayed
 * through thread-locals: the emitter writes it onto the per-compile cfg->wasm_jit_result (see
 * MonoWasmJitResult in mini.h) and mono_wasm_force_compile copies it out after the compile returns.
 * Per-compile => re-entrancy-safe by construction (a nested cctor/AOT-init compile has its own cfg),
 * so the old "publish the success gate last behind a barrier" dance is gone. */
int mono_wasm_jit_island = 1;   /* eager transitive island-JIT; MONO_WASM_JIT_ISLAND=0 = old bottom-up retry only */
/* Automatic hotness trigger (Phase 5): when mono_wasm_jit_auto>0, the interp (MINT_CALL) counts
 * calls to each callee and force-compiles it to wasm once its hit count reaches mono_wasm_jit_thresh,
 * instead of requiring the method to be named in MONO_WASM_JIT_METHOD. -1 = uninitialized. */
int mono_wasm_jit_auto = -1;
int mono_wasm_jit_thresh = 2000;
int mono_wasm_jit_vcall_ic = 1;   /* virtual-dispatch resolve cache; MONO_WASM_JIT_VCALL_IC=0 disables (always resolve) */
int mono_wasm_jit_arity = 0;      /* MONO_WASM_JIT_ARITY=1: per-call-site receiver-arity histogram for the vcall miss population (N-way IC capture curve). Diagnostic — perturbs timing (like PROFILE_FAST); default off */
int mono_wasm_jit_vcall_ways = 1; /* MONO_WASM_JIT_VCALL_WAYS: N-way inline vcall f-slot IC (1 = monomorphic/legacy). Clamped [1,8]. 2 captures the ~63% of the miss population that are 2-type sites (arity depth-1) which a 1-way IC gets 0% of. */
int mono_wasm_jit_vcall_aot_ways = 1; /* MONO_WASM_JIT_VCALL_AOT_WAYS: N-way inline AOT-vcall IC (1 = monomorphic first-wins/legacy). Clamped [1,8]. 2 captures the AOT-backed 2-type sites (arity depth-0 once VCALL_WAYS>=2): the loser vtable of a 2-way AOT site is stuck reaching the resolve helper behind the 1-entry cache. */

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
	t = g_getenv ("MONO_WASM_JIT_THRESHOLD");
	mono_wasm_jit_thresh = (t && *t) ? atoi (t) : 2000;
	if (mono_wasm_jit_thresh <= 0)
		mono_wasm_jit_thresh = 2000;
	{ extern int mono_wasm_jit_stats; const char *s = g_getenv ("MONO_WASM_JIT_STATS"); mono_wasm_jit_stats = (s && *s && *s != '0') ? 1 : 0; }
	/* MONO_WASM_JIT_VERBOSE controls the per-method emit LOG spam, DECOUPLED from stats (counting is cheap,
	 * logging floods): 0=silent (default), 1=+registered/invalid, 2=+bail, 3=+emit-enter AND per-call traces (vcall-aot
	 * dispatch etc. — kept >=3 so a stats/bail run at verbose<=2 is never flooded by per-invocation logs). The 23k-line log
	 * came from these firing whenever stats was on; the aggregated bail histogram replaces them at level 0. */
	{ extern int mono_wasm_jit_verbose; const char *vb = g_getenv ("MONO_WASM_JIT_VERBOSE"); mono_wasm_jit_verbose = (vb && *vb) ? atoi (vb) : 0; }
	{ extern int mono_wasm_jit_names; const char *nm = g_getenv ("MONO_WASM_JIT_NAMES"); mono_wasm_jit_names = (nm && *nm && *nm != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_residual_mode; const char *r = g_getenv ("MONO_WASM_JIT_RESIDUAL"); mono_wasm_jit_residual_mode = (r && *r) ? atoi (r) : 1; }
	{ extern int mono_wasm_jit_virtual; const char *v = g_getenv ("MONO_WASM_JIT_VIRTUAL"); mono_wasm_jit_virtual = (v && *v && *v == '0') ? 0 : 1; } /* 0 = bail virtual calls (revert to whole-method interp); default on */
	{ extern int mono_wasm_jit_vcall_ic; const char *c = g_getenv ("MONO_WASM_JIT_VCALL_IC"); mono_wasm_jit_vcall_ic = (c && *c && *c == '0') ? 0 : 1; } /* 0 = disable the virtual resolve cache (always resolve) */
	{ extern int mono_wasm_jit_arity; const char *ar = g_getenv ("MONO_WASM_JIT_ARITY"); mono_wasm_jit_arity = (ar && *ar && *ar != '0') ? 1 : 0; } /* 1 = record per-call-site receiver-arity histogram (vcall miss population); diagnostic, perturbs timing */
	{ extern int mono_wasm_jit_vcall_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline vcall IC; clamp [1,8]; 1 = legacy monomorphic */
	{ extern int mono_wasm_jit_vcall_aot_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_AOT_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_aot_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline AOT-vcall IC; clamp [1,8]; 1 = legacy first-wins */
	{ extern int mono_wasm_jit_cond_exc; const char *ce = g_getenv ("MONO_WASM_JIT_COND_EXC"); mono_wasm_jit_cond_exc = (ce && *ce && *ce == '0') ? 0 : 1; } /* 0 = bail OP_COND_EXC_* methods to interp */
	{ extern int mono_wasm_jit_island; const char *il = g_getenv ("MONO_WASM_JIT_ISLAND"); mono_wasm_jit_island = (il && *il && *il == '0') ? 0 : 1; } /* 0 = no eager island formation (bottom-up retry only) */
	{ extern int mono_wasm_jit_aot_residual; const char *ar = g_getenv ("MONO_WASM_JIT_AOT_RESIDUAL"); mono_wasm_jit_aot_residual = (ar && *ar && *ar == '0') ? 0 : 1; } /* jit->AOT fastpath: residual/vcall-fallback to an AOT'd callee runs it natively via do_jit_call. 0 = old behaviour (interpret it). */
	{ extern int mono_wasm_jit_inline_aot; const char *ia = g_getenv ("MONO_WASM_JIT_INLINE_AOT"); mono_wasm_jit_inline_aot = (ia && *ia && *ia != '0') ? 1 : 0; } /* emit the inline direct same-ABI AOT call instead of the residual. Build 1 = no wasm-EH (non-throwing callees only). 1 = on, default 0 = off. */
	{ extern int mono_wasm_jit_aotconst; const char *ac = g_getenv ("MONO_WASM_JIT_AOTCONST"); mono_wasm_jit_aotconst = (ac && *ac) ? (*ac != '0') : 1; } /* bake resolved OP_AOTCONST pointers; default ON */
	{ extern int mono_wasm_jit_rgctx; const char *rg = g_getenv ("MONO_WASM_JIT_RGCTX"); mono_wasm_jit_rgctx = (rg && *rg) ? (*rg != '0') : 1; } /* JIT uses_rgctx_reg methods, routing the rgctx call through the interp residual; default ON, =0 reverts to the whole-method bail */
	{ extern int mono_wasm_jit_ldaddr; const char *ld = g_getenv ("MONO_WASM_JIT_LDADDR"); mono_wasm_jit_ldaddr = (ld && *ld) ? (*ld != '0') : 1; } /* OP_LDADDR: back address-taken scalar locals with a per-thread linear-memory frame (unblocks synchronized wrappers' Monitor.Enter bool& + ref/out-local calls); default ON, =0 reverts to the ldaddr bail */
	{ extern int mono_wasm_jit_ldaddr_vtype; const char *lv = g_getenv ("MONO_WASM_JIT_LDADDR_VTYPE"); mono_wasm_jit_ldaddr_vtype = (lv && *lv && *lv != '0') ? 1 : 0; } /* OP_LDADDR of NON-SCALAR ref-free local via a full-size addr-frame slot. DEFAULT OFF (exonerated re: corruption but kept for repro parity). */
	{ extern int mono_wasm_jit_vtype_scalar_ref; const char *vr = g_getenv ("MONO_WASM_JIT_VTYPE_SCALAR_REF"); mono_wasm_jit_vtype_scalar_ref = (vr && *vr && *vr != '0') ? 1 : 0; } /* ref-etype scalar-vtype arg via a GC-scanned ref-shadow slot; GC-critical, default OFF */
	{ extern int mono_wasm_jit_vtype_scalar; const char *vs = g_getenv ("MONO_WASM_JIT_VTYPE_SCALAR"); mono_wasm_jit_vtype_scalar = (vs && *vs && *vs != '0') ? 1 : 0; } /* pass a BYVAL ref-free scalar-vtype call arg as its single-field etype scalar (LLVMArgWasmVtypeAsScalar ABI). Default OFF; needs LDADDR_VTYPE. */
	{ extern int mono_wasm_jit_longdiv; const char *lv = g_getenv ("MONO_WASM_JIT_LONGDIV"); mono_wasm_jit_longdiv = (lv && *lv) ? (*lv != '0') : 1; } /* JIT i64 div/rem (OP_LDIV family) with inline div-by-zero/overflow checks; default ON, =0 reverts to the bail (bisection) */
	{ extern int mono_wasm_jit_sync; const char *sv = g_getenv ("MONO_WASM_JIT_SYNC"); mono_wasm_jit_sync = (sv && *sv) ? (*sv != '0') : 1; } /* direct call to a synchronized method: substitute the SYNCHRONIZED wrapper (f-slot/residual); default ON, =0 reverts to bailing the whole caller (bisection) */
	{ extern int mono_wasm_jit_eh_nocxa; const char *en = g_getenv ("MONO_WASM_JIT_EH_NOCXA"); mono_wasm_jit_eh_nocxa = (en && *en && *en != '0') ? 1 : 0; } /* bisection: skip begin/end_catch in the EH landing pad */
	{ extern const char *mono_wasm_jit_dump_ir; mono_wasm_jit_dump_ir = g_getenv ("MONO_WASM_JIT_DUMP_IR"); } /* substring filter; methods whose full name contains it get their clauses+bb regions+opcodes dumped (ground truth for the nested-EH lowering). */
	/* Island heuristic levers (Part 5), all default off. */
	{ extern int mono_wasm_jit_entry_promote; const char *ep = g_getenv ("MONO_WASM_JIT_ENTRY_PROMOTE"); mono_wasm_jit_entry_promote = (ep && *ep) ? atoi (ep) : 0; }      /* Lever A: 0=off */
	{ extern int mono_wasm_jit_residual_perm; const char *rp = g_getenv ("MONO_WASM_JIT_RESIDUAL_PERM"); mono_wasm_jit_residual_perm = (rp && *rp && *rp != '0') ? 1 : 0; } /* Lever B: 0=off */
	{ extern int mono_wasm_jit_residual_cold; const char *rc = g_getenv ("MONO_WASM_JIT_RESIDUAL_COLD"); mono_wasm_jit_residual_cold = (rc && *rc && *rc != '0') ? 1 : 0; } /* Lever B': cold-leaf residual, 0=off */
	{ extern int mono_wasm_jit_profile_fast; const char *pf = g_getenv ("MONO_WASM_JIT_PROFILE_FAST"); mono_wasm_jit_profile_fast = (pf && *pf && *pf != '0') ? 1 : 0; } /* emit fast-path volume counters, 0=off */
	{ extern int mono_wasm_jit_island_depth; const char *id = g_getenv ("MONO_WASM_JIT_ISLAND_DEPTH"); mono_wasm_jit_island_depth = (id && *id && atoi (id) > 0) ? atoi (id) : 10; }   /* Lever C: default 10 */
	{ extern int mono_wasm_jit_island_budget; const char *ib = g_getenv ("MONO_WASM_JIT_ISLAND_BUDGET"); mono_wasm_jit_island_budget = (ib && *ib && atoi (ib) > 0) ? atoi (ib) : 64; } /* Lever C: default 64 */
	{ extern int mono_wasm_jit_block_promote; const char *bp = g_getenv ("MONO_WASM_JIT_BLOCK_PROMOTE"); mono_wasm_jit_block_promote = (bp && *bp) ? atoi (bp) : 16; } /* Lever C: default 16; 0 disables */
	{ extern int mono_wasm_jit_promotion_drain; const char *pd = g_getenv ("MONO_WASM_JIT_PROMOTION_DRAIN"); mono_wasm_jit_promotion_drain = (pd && *pd && atoi (pd) > 0) ? atoi (pd) : 8; }
	{ extern int mono_wasm_jit_island_cold_div; const char *cd = g_getenv ("MONO_WASM_JIT_ISLAND_COLD_DIV"); mono_wasm_jit_island_cold_div = (cd && *cd && atoi (cd) > 0) ? atoi (cd) : 4; }
	{ extern int mono_wasm_jit_promoted_cold_div; const char *pc = g_getenv ("MONO_WASM_JIT_PROMOTED_COLD_DIV"); mono_wasm_jit_promoted_cold_div = (pc && *pc && atoi (pc) > 0) ? atoi (pc) : 16; }
	{ extern int mono_wasm_jit_promoted_root_uncold_depth; const char *pu = g_getenv ("MONO_WASM_JIT_PROMOTED_ROOT_UNCOLD_DEPTH"); mono_wasm_jit_promoted_root_uncold_depth = (pu && *pu && atoi (pu) >= 0) ? atoi (pu) : 1; }
	{ extern int mono_wasm_jit_block_force; const char *bf = g_getenv ("MONO_WASM_JIT_BLOCK_FORCE"); mono_wasm_jit_block_force = (bf && *bf) ? atoi (bf) : 4; }
	{ extern int mono_wasm_jit_hot_root; const char *hr = g_getenv ("MONO_WASM_JIT_HOT_ROOT"); mono_wasm_jit_hot_root = (hr && *hr && *hr != '0') ? 1 : 0; } /* own-threshold island = promoted root, 0=off */
	{ extern int mono_wasm_jit_vcall_aot; const char *va = g_getenv ("MONO_WASM_JIT_VCALL_AOT"); mono_wasm_jit_vcall_aot = (va && *va && *va != '0') ? 1 : 0; } /* fast AOT-vcall dispatch: 0=off (residual) */
	{ extern int mono_wasm_jit_vcall_aot_ic; const char *vc = g_getenv ("MONO_WASM_JIT_VCALL_AOT_IC"); mono_wasm_jit_vcall_aot_ic = (vc && *vc && *vc != '0') ? 1 : 0; } /* per-call-site AOT-vcall IC; needs VCALL_INLINE_IC+VCALL_AOT; default off */
	{ extern int mono_wasm_jit_entry_redirect; const char *er = g_getenv ("MONO_WASM_JIT_ENTRY_REDIRECT"); mono_wasm_jit_entry_redirect = (er && *er) ? (*er != '0') : 1; } /* interp_entry redirects a JITted target to its own e-thunk (self-heal: a residual / vcall-fallback / native-vtable entry runs the wasm body, not the interp copy); default ON, =0 reverts to interpreting the target */
#ifdef HOST_BROWSER
	/* These three are DEBUG store/GC guards whose globals + runtime-check emission are HOST_BROWSER-only
	 * (they insert per-store checks that only do anything when the JITted code actually RUNS). The offline
	 * cross-compiler dump never executes JITted code, so skip their env here — otherwise auto_init would
	 * reference browser-only globals and fail to link into mono-aot-cross. */
	{ extern int mono_wasm_jit_storeguard; const char *sg = g_getenv ("MONO_WASM_JIT_STOREGUARD"); mono_wasm_jit_storeguard = (sg && *sg && *sg != '0') ? 1 : 0; } /* DEBUG: bounds-check every ref/addr-frame store to catch the wild store (traps at the culprit). default off */
	{ extern int mono_wasm_jit_objguard; const char *og = g_getenv ("MONO_WASM_JIT_OBJGUARD"); mono_wasm_jit_objguard = (og && *og && *og != '0') ? 1 : 0; } /* DEBUG: before every ref-field store, validate the object BASE is a live heap object (catches missed-ref/stale-base wild stores). default off */
	{ extern int mono_wasm_jit_pinall; const char *pn = g_getenv ("MONO_WASM_JIT_PINALL"); mono_wasm_jit_pinall = (pn && *pn && *pn != '0') ? 1 : 0; } /* DEBUG TEST: route EVERY i32 vreg to the GC-pinning ref shadow stack. If the random corruption stops, a ref vreg was being missed by the isref inference. SLOW. default off */
#endif
	{ extern int mono_wasm_jit_missedref; const char *mr = g_getenv ("MONO_WASM_JIT_MISSEDREF"); mono_wasm_jit_missedref = (mr && *mr && *mr != '0') ? 1 : 0; } /* DIAG: PINALL confirmed a missed ref; this names it. For every method, log any NONREF-classified i32 vreg used as a MEMBASE load/store base or virtual-call receiver (a stale one of these is the wild-deref corruptor), with its defining opcode -> pins which wj_opcode_is_nonref case is wrong. Bounded. default off */
	{ extern int mono_wasm_jit_refbases; const char *rb = g_getenv ("MONO_WASM_JIT_REFBASES"); mono_wasm_jit_refbases = (rb && *rb) ? (*rb != '0') : 1; } /* FIX (default ON): pin every vreg used as a MEMBASE load/store base or virtual-call receiver on the ref shadow stack — a dereferenced pointer must stay valid across GC. Closes the missed-ref corruption the prove-non-ref pass leaves (object baked as non-STACK_OBJ iconst, interior-ptr add, ...) at far less cost than PINALL. =0 reverts (buggy) for A/B. */
	{ extern int mono_wasm_jit_gcmaps; const char *gm = g_getenv ("MONO_WASM_JIT_GCMAPS"); mono_wasm_jit_gcmaps = (gm && *gm) ? (*gm != '0') : 1; } /* structural ref/mp seeds from MINI's compute_gc_maps marking; default ON, =0 for A/B against opcode-inference-only */
	{ extern int mono_wasm_jit_refverify; const char *rv = g_getenv ("MONO_WASM_JIT_REFVERIFY"); mono_wasm_jit_refverify = (rv && *rv) ? atoi (rv) : 0; } /* 1=log, 2=assert classification-vs-structural-marking violations; default off */
	{ extern int mono_wasm_jit_outarg; const char *oa = g_getenv ("MONO_WASM_JIT_OUTARG"); mono_wasm_jit_outarg = (oa && *oa && *oa != '0') ? 1 : 0; } /* LLVM-style call-arg capture (moves + out_ireg_args); prerequisite for enabling mini local opts. default off (legacy snapshot) */
	{ extern int mono_wasm_jit_vcall_inline_ic; const char *vi = g_getenv ("MONO_WASM_JIT_VCALL_INLINE_IC"); mono_wasm_jit_vcall_inline_ic = (vi && *vi && *vi != '0') ? 1 : 0;
	} /* inline vcall IC fast path: 0=off (default), 1=on. Now MT-SAFE on threaded builds — the buggy
	   * ref.is_null liveness (placeholder sig mismatch, jit138) is replaced by a per-thread slot_live gate. */
	e = g_getenv ("MONO_WASM_JIT_AUTO");
	mono_memory_barrier ();
	mono_wasm_jit_auto = (e && *e && *e != '0') ? 1 : 0; /* set last: publishes "initialized" */
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
int mono_wasm_jit_names = 0;   /* MONO_WASM_JIT_NAMES=1 emits a wasm name section per JITted module so traps self-symbolicate */

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
 * no rgctx (our f-slots are dedicated/concrete compiles); INLINE_AOT is skipped for rgctx calls because the
 * call needs the CALLSITE runtime generic context, while the AOT fast path only knows how to bake the CALLEE
 * extra arg/rgctx (cinfo->extra_arg in llvm_only, or the matching fallback recovery below);
 * indirect/virtual rgctx calls still bail (untested shape). Default ON; MONO_WASM_JIT_RGCTX=0 reverts to the
 * old whole-method bail. */
int mono_wasm_jit_rgctx = 1;
int mono_wasm_jit_longdiv = 1;        /* MONO_WASM_JIT_LONGDIV: JIT i64 div/rem (OP_LDIV family). =0 bails those methods (bisection). */
int mono_wasm_jit_sync = 1;           /* MONO_WASM_JIT_SYNC: substitute the SYNCHRONIZED wrapper for a direct call to a synchronized method. =0 reverts to bailing the whole caller (bisection). */
int mono_wasm_jit_ldaddr = 1;         /* MONO_WASM_JIT_LDADDR: emit OP_LDADDR by backing address-taken SCALAR locals with a per-thread linear-memory frame (their address can't be a wasm local). Unblocks synchronized wrappers (Monitor.Enter's bool& lock_taken) and ref/out-local call sites (the #1 ldaddr bail). Default ON; =0 reverts to bailing the whole method on OP_LDADDR. */
int mono_wasm_jit_ldaddr_vtype = 0;   /* MONO_WASM_JIT_LDADDR_VTYPE: extend OP_LDADDR to NON-SCALAR ref-free valuetype locals via a full-size addr-frame slot. DEFAULT OFF (exonerated: jit17 corrupted with it off; kept gated for binary/repro parity). */
int mono_wasm_jit_vtype_scalar_ref = 0; /* MONO_WASM_JIT_VTYPE_SCALAR_REF: extend VTYPE_SCALAR to a scalar-vtype whose SINGLE field is a managed REFERENCE (e.g. RuntimeTypeHandle{RuntimeType}). Backed by a GC-SCANNED ref-shadow-stack slot (not the un-scanned addr frame): OP_LDADDR yields refbase+slot*4 so the field store/load track the ref as a conservative pinning root, and the store's inline card-barrier marks a HARMLESS card (wasm32 has no overlapping cards — the 8MB table covers the whole 32-bit space, so a non-heap mark is in-bounds and never scanned). GC-CRITICAL: validate in-browser with STOREGUARD/OBJGUARD. Default OFF. */
int mono_wasm_jit_vtype_scalar = 0;   /* MONO_WASM_JIT_VTYPE_SCALAR: pass a BYVAL scalar-vtype call arg (mini_wasm_is_scalar_vtype: struct <=8 bytes, one field) as its single-field SCALAR — the ABI the AOT callee was compiled with (LLVMArgWasmVtypeAsScalar). The vtype value is addr-frame-backed (LDADDR_VTYPE), so we load its field (offset 0) from the addr-frame slot and pass that. REF-FREE etype only: a ref-etype scalar-vtype (e.g. RuntimeTypeHandle{RuntimeType}) can't live in the un-scanned addr frame — it bails at "ldaddr of vtype with refs" before here and needs the GC-scanned/ref-shadow-stack path (not yet implemented). Default OFF; requires LDADDR_VTYPE. */
int mono_wasm_jit_missedref = 0;      /* MONO_WASM_JIT_MISSEDREF: diagnostic — log NONREF-classified vregs used as MEMBASE bases / call receivers + their defining opcode, to name the isref-inference gap PINALL papers over. Default off. */
int mono_wasm_jit_refbases = 1;       /* MONO_WASM_JIT_REFBASES: pin every dereferenced pointer (MEMBASE base / call receiver) on the ref shadow stack, closing the missed-ref corruption the prove-non-ref pass leaves. Default ON; =0 reverts for A/B. */
int mono_wasm_jit_gcmaps = 1;         /* MONO_WASM_JIT_GCMAPS: set cfg->compute_gc_maps for COMPILE_WASM so MINI's own ref/managed-pointer marking (mini.c create_var_for_vreg, ir-emit.h alloc_ireg_ref/_mp) seeds the isref classification structurally — the same type facts LLVM/native GC maps use, replacing the old wasm-only ad-hoc marking. Default ON; =0 reverts to opcode-inference-only seeds for A/B. */
int mono_wasm_jit_refverify = 0;      /* MONO_WASM_JIT_REFVERIFY (0/1/2): after the isref fixpoint, cross-check classification against the structural vreg_is_ref/vreg_is_mp marking — 1 logs violations (a marked vreg classified nonref = lost seed = would-be silent corruption), 2 asserts. Debug only, default off. */
int mono_wasm_jit_outarg = 0;         /* MONO_WASM_JIT_OUTARG: capture call args LLVM-style in mono_wasm_emit_call — a real per-arg OP_*MOVE into a fresh vreg registered in call->out_ireg_args (so DEADCE/alias treat it as used), the mechanism that lets mini opt passes run without corrupting the captured arg vregs. 0 (default) = legacy raw-dreg snapshot, byte-identical modules; MUST stay 0 while cfg->opt is hard-reset, and the opt whitelist must not enable CONSPROP/COPYPROP/DEADCE until this is 1. */
const char *mono_wasm_jit_dump_ir = NULL;  /* MONO_WASM_JIT_DUMP_IR=<substr>: dump clauses + bb regions + opcode stream for clause-bearing methods whose full name contains <substr> (EH-lowering ground truth, e.g. "indigo"). */
/* Island heuristic levers (Part 5), all default-OFF so the baseline is unchanged and each can be A/B'd. */
int mono_wasm_jit_entry_promote = 0;   /* Lever A: MONO_WASM_JIT_ENTRY_PROMOTE=N — after a hot interp caller invokes JITted callees N times, force-JIT the caller (grow the island UPWARD). 0 = off. */
int mono_wasm_jit_residual_perm = 0;   /* Lever B: MONO_WASM_JIT_RESIDUAL_PERM=1 — under residual=0, residual-route ONLY a permanently-un-JITtable blocker instead of bailing the whole caller. 0 = off. */
int mono_wasm_jit_residual_cold = 0;   /* Lever B': MONO_WASM_JIT_RESIDUAL_COLD=1 — under residual=0, residual-route a blocker the island cold-gate would refuse to pull in (still counting hits, below thresh/cold_div, not block-promoted): a cold branch (IKVM __<GetInstance> lambda factory, one-shot init, error path) reached rarely from a hot caller. Lets the hot method JIT while paying ~1 transition per cold call, NOT a per-iteration storm; hot/parked callees still bail so the island force-JITs them. 0 = off. NOTE: jit34 showed this misclassifies hot-via-JITted-caller callees as cold -> 2M residuals/frame -> 1.5fps. Keep OFF until residuals self-heal to the callee's f-slot. */
int mono_wasm_jit_entry_redirect = 1;  /* MONO_WASM_JIT_ENTRY_REDIRECT — interp_entry redirects a target that is itself wasm-JITted to its own e-thunk (run the compiled wasm body) instead of interpreting it. This is the "residual self-heals to the callee's f-slot" fix the RESIDUAL_COLD note above was waiting on: a residual / vcall-fallback / native-vtable dispatch to an already-JITted method previously ran its interp copy (the dominant steady-state boundary cost — hot entry-edges applyAsLong/accept/Vec3i.equals). Default ON; =0 reverts to interpreting the target (bisection). */
int mono_wasm_jit_profile_fast = 0;    /* MONO_WASM_JIT_PROFILE_FAST=1 — emit inline volume counters into the fast dispatch paths (INLINE_AOT direct, inline f-slot IC hit, inline AOT-IC hit) which otherwise call no counting helper. Adds hot-path overhead, so OFF by default (only for a dedicated cost-attribution run). Feeds WJC_FAST_*. */
int mono_wasm_jit_island_depth = 10;   /* Lever C: MONO_WASM_JIT_ISLAND_DEPTH — max island DFS depth (was a fixed 10). */
int mono_wasm_jit_island_budget = 64;  /* Lever C: MONO_WASM_JIT_ISLAND_BUDGET — max force-compiles per island attempt (was a fixed 64). */
int mono_wasm_jit_block_promote = 16;  /* Lever C: MONO_WASM_JIT_BLOCK_PROMOTE — pull a cold callee into an island once it has BLOCKED >= N island attempts (block_n), even if its own hit count is low (it's hot via JITted callers). 0 = disable (cold gate is hits-only). The bench showed top blockers ~100, so the old thresh/4 (=500) never fired — 16 catches the hot ctors. */
int mono_wasm_jit_promotion_drain = 8; /* MONO_WASM_JIT_PROMOTION_DRAIN — max queued promotions (Lever A callers, block-promote callees, and woken waiters) drained per safe point. */
int mono_wasm_jit_island_cold_div = 4; /* MONO_WASM_JIT_ISLAND_COLD_DIV — normal cold gate divisor for eager island callees; thresh/div is the minimum retained hit count. */
int mono_wasm_jit_promoted_cold_div = 16; /* MONO_WASM_JIT_PROMOTED_COLD_DIV — looser cold gate divisor when force-JITing an upward-promoted caller. */
int mono_wasm_jit_promoted_root_uncold_depth = 1; /* MONO_WASM_JIT_PROMOTED_ROOT_UNCOLD_DEPTH — for promoted roots, skip the cold gate entirely through this DFS depth. */
int mono_wasm_jit_block_force = 4; /* MONO_WASM_JIT_BLOCK_FORCE — queue a blocking callee for direct promotion once it has blocked this many island attempts. 0 disables. */
int mono_wasm_jit_hot_root = 0; /* MONO_WASM_JIT_HOT_ROOT=1 — a method crossing its OWN auto-JIT threshold is proven hot (>=thresh calls), so build its island as a PROMOTED ROOT (relax the cold gate through PROMOTED_ROOT_UNCOLD_DEPTH) instead of the strict cold gate. Pulls the hot method's private callees (reached only via it -> ~0 interp hits -> the blind spot that makes it PARK forever) into the island so it actually JITs. Depth-limited + budget-bounded so it doesn't drag in the whole cold subtree. jit35: 94.6% of below-threshold vcall fallbacks were PARKED hot methods. Default OFF (A/B). */
/* MONO_WASM_JIT_VCALL_AOT=1: when a JITted method's virtual call resolves to an AOT-backed override (the
 * vcall_resolve_fslot f-slot miss — the dominant steady-state residual, ~98% "aot-backed" in the bench),
 * call_indirect the override's AOT body DIRECTLY (this+args+rgctx, same native ABI the inline-AOT direct
 * call uses) instead of routing through the residual (mono_wasm_jit_call_interp -> wasm_jit_aot_call_lean
 * -> do_jit_call: double arg-marshalling + an LMF frame). Default 0; mirrors INLINE_AOT's EH handling
 * (resume-state try/catch, or bare under CPPEH). Off-by-default so the validated residual path is unchanged. */
int mono_wasm_jit_vcall_aot = 0;
int mono_wasm_jit_vcall_aot_ic = 0;   /* MONO_WASM_JIT_VCALL_AOT_IC=1: per-call-site inline cache for AOT-backed virtual targets — skip scratch()+resolve_fslot()+aot_target() (3 C calls/vcall) on a monomorphic hit, call_indirect the cached AOT body directly. Needs VCALL_INLINE_IC + VCALL_AOT. Default OFF; hottest-path + MT — validate in-browser. */
/* MONO_WASM_JIT_VCALL_INLINE_IC: the inline monomorphic vcall IC fast-path (call_indirect the cached
 * f-slot in wasm, skipping the scratch() + resolve_fslot C helpers on a hit — the profiled #1 game-thread
 * cost, vcall_resolve_fslot ~17%). DEFAULT OFF; =1 enables. NOW MT-SAFE on threaded builds: the original
 * "table[fslot] != null" liveness check was wrong (the per-thread table grows with a NON-null jiterpreter
 * placeholder, mono_jiterp_placeholder_jit_call (i32,i32,i32,i32)->void, so it passed for un-instantiated
 * slots -> call_indirect signature-mismatch trap, jit138). Fixed: the inline path now gates on the
 * authoritative per-thread bitmap via one cheap mono_wasm_jit_slot_live() call (wasm exposes no funcref
 * equality / funcref->i32 to compare the slot against the placeholder inline). Still one C boundary per hit
 * vs two + resolve for the helper; a full pure-wasm gate would need __tls_base imported to read the bitmap. */
int mono_wasm_jit_vcall_inline_ic = 0;

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
	printf ("[wasm-jit aotroute] aot_routed=%lld interp_routed=%lld vcall_aot_fast=%lld\n",
		WJC_(WJC_AOT_ROUTED), WJC_(WJC_INTERP_ROUTED), WJC_(WJC_VCALL_AOT_FAST));
	printf ("[wasm-jit gcref] refbases_extra=%lld (0 across a soak with REFBASES=1 => REFBASES subsumed by structural seeds)\n",
		WJC_(WJC_REFBASES_EXTRA));
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

static void
wj_mark_slot_live (int slot)
{
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

/* TRUE iff THIS thread successfully instantiated the module owning `slot` (so call_indirect-ing it is
 * safe). Read by the interp invoke paths + the vcall f-slot resolver in interp.c. */
int
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
guint8 **
mono_wasm_jit_slot_live_ptr_addr (void)
{
	return &wj_slot_live;
}
int *
mono_wasm_jit_slot_live_cap_addr (void)
{
	return &wj_slot_live_cap;
}

/* Instantiate a cached JITted module into the CURRENT thread's wasm function table. The table is
 * per-thread for dynamically-added entries, so each thread must do this once (lazily, on its first
 * invoke of the method — interp.c MINT_CALL) before call_indirect-ing the slot. Returns 1 on success,
 * 0 on failure (caller then disables the JIT for the method → interpreter fallback). */
int
mono_wasm_jit_instantiate_local (int e_slot, int f_slot, const void *bytes, int len, char *errbuf, int errcap, double *out_ms)
{
	int _ok;
	/* $2/$4/$6 below are pointers passed as 32-bit ints; a g_malloc buffer above 2GB (MC's heap grows past
	 * it) arrives NEGATIVE in JS, and HEAPU8.slice(negative,..) reads the wrong region -> garbage module
	 * bytes -> a magic-word CompileError. Re-add 2^32 to recover the real unsigned address. (Use a C-valid
	 * ternary, NOT JS >>>, since clang parses the EM_ASM body tokens and >>> is not a C operator.)
	 * out_ms ($6, an 8-byte-aligned double*) receives the WebAssembly.Module+Instance compile time in ms
	 * (Part 2 instantiation timing) — measured on both the success and CompileError paths; 0/NULL skips it. */
	_ok = EM_ASM_INT ({
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
	if (_ok) {
		/* record which slots this thread now has the REAL export in, so the invoke paths can tell a live
		 * slot from a jiterpreter placeholder and fall back to interp instead of trapping (see wj_slot_live). */
		wj_mark_slot_live (e_slot);
		wj_mark_slot_live (f_slot);
	}
	return _ok;
}

/* Global registry of every JITted method's {slots, cached bytes}. Because the wasm function table is
 * per-thread for dynamic entries, AND JITted methods call each other directly via f-slot call_indirect,
 * every thread that runs any JITted code must have ALL JITted methods instantiated in its own table — not
 * just the ones the interpreter invoked on it. mono_wasm_jit_sync_thread() (called on the interp's
 * JIT-invoke path) brings the calling thread up to date: it instantiates any methods registered since this
 * thread last synced. Callees are always registered before callers (the direct-call lowering bails if the
 * callee isn't JITted yet), so syncing to the current generation guarantees a method's f-slot callees are
 * present.
 *
 * CHUNKED + pointer-stable so it never overflows (a big app JITs well past any fixed cap): a FIXED top-level
 * array of chunk pointers (the array itself never moves, so a lock-free reader can't observe a torn base),
 * with chunks g_malloc0'd on demand and never moved/freed. A reader indexing wj_reg_at(i) for i < wj_reg_n
 * always sees a published chunk + a fully written entry: the writer publishes the chunk pointer (barrier)
 * and the entry (barrier) BEFORE bumping wj_reg_n; readers acquire wj_reg_n (sync_thread under the loader
 * lock; mono_wasm_jit_instantiate_fslot via a barrier after snapshotting wj_reg_n). Appends serialized
 * under the loader lock. */
typedef struct { int e, f, len; void *bytes; } WjRegEntry;
#define WJ_REG_CHUNK   8192
#define WJ_REG_NCHUNKS 1024      /* up to 8M JITted methods; the 4KB top-level pointer array never moves */
static WjRegEntry *wj_reg_chunks [WJ_REG_NCHUNKS];
static volatile int wj_reg_n = 0;
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

void
mono_wasm_jit_register (int e_slot, int f_slot, void *bytes, int len)
{
	mono_loader_lock ();
	{
		int n = wj_reg_n;
		int ci = n / WJ_REG_CHUNK;
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
			mono_memory_barrier ();            /* publish the entry before the count */
			wj_reg_n = n + 1;
		} else {
			/* >8M JITted methods (absurd) — the chunk-pointer array is full. The method is JITted
			 * (im->wasm_jit_* set) but NOT in wj_reg, so sync_thread can't pre-populate it;
			 * mono_wasm_jit_ensure_fslot's imethod fallback keeps it correct per direct call. Warn once. */
			static int _warned = 0;
			if (!_warned) { _warned = 1; printf ("WASM_JIT_REG_OVERFLOW: >%d JITted methods; sync_thread incomplete, relying on ensure_fslot imethod fallback\n", WJ_REG_NCHUNKS * WJ_REG_CHUNK); }
		}
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
		WjRegEntry *re = wj_reg_at (synced);   /* under the loader lock + synced < wj_reg_n -> chunk is published */
		/* Skip a slot this thread already instantiated. The COMPILING thread instantiates its own module
		 * directly in mono_wasm_emit_method (before registering it), so without this it would redundantly
		 * re-instantiate its own just-compiled modules over live table slots on its next sync. */
		if (mono_wasm_jit_slot_live (re->e)) { synced++; continue; }
		if (!mono_wasm_jit_instantiate_local (re->e, re->f, re->bytes, re->len, eb, (int) sizeof (eb), &ms)) {
			/* A module that instantiated fine on the COMPILING thread failed here on another thread:
			 * names the corruption (e.g. magic-word/type) + which slot, so we can tell a byte-corruption
			 * (race) apart from a thread-local structural issue. Slot stays a placeholder -> interp.
			 * Do NOT advance past the failure: later JITted modules can directly call earlier f-slots, so
			 * marking later slots live on this thread while an earlier dependency is still a placeholder
			 * can turn a managed throw into an uncaught call_indirect signature trap. */
			/* UNCONDITIONAL (rate-limited): a sync break is rare but is THE cause of the per-thread placeholder
			 * call_indirect traps (a self-compiled method whose slot is live runs while an earlier dependency,
			 * held back past this break, is still a jiterpreter placeholder). It was previously stats-gated, so
			 * the break was invisible without MONO_WASM_JIT_STATS. Always surface it + the JS error string. */
			{ static int _sf = 0; if (_sf++ < 60) printf ("WASM_JIT_SYNC_FAIL idx=%d/%d e=%d f=%d len=%d : %s\n", synced, wj_reg_n, re->e, re->f, re->len, eb); }
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_add (WJC_ELAPSED_INSTANTIATION, (gint64) (ms * 1000.0));
			break;
		}
		/* per-thread table-sync instantiation is real compile wall-cost too — fold it into the same timer */
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_add (WJC_ELAPSED_INSTANTIATION, (gint64) (ms * 1000.0));
		synced++;
	}
	mono_loader_unlock ();
}

/* Backstop for the direct f-slot call path (mono_wasm_jit_ensure_fslot): make `fslot` live in THIS thread's
 * wasm table using the AUTHORITATIVE module registry (wj_reg) rather than the racy per-imethod fields. The
 * compiling thread registers {e,f,bytes,len} (barrier-published, see mono_wasm_jit_register) before any
 * caller can bake fslot into a direct call_indirect, so the registry always has the callee's bytes even
 * when (a) this worker momentarily reads im->wasm_jit_slot<=0 / im->wasm_jit_bytes==NULL (publish race), or
 * (b) sync_thread broke before reaching this slot (leaving it a jiterpreter placeholder). Instantiating one
 * SPECIFIC slot is safe: it does NOT advance the sync watermark or mark later slots live (the sync-break
 * invariant). A transient instantiate failure (shared-memory byte race) is retried. Returns 1 if the slot
 * is live on return, else 0 (genuine failure — OOM/CompileError on this worker, or not in the registry —
 * the direct call_indirect will then trap; mono_wasm_jit_ensure_fslot surfaces which method). */
int
mono_wasm_jit_instantiate_fslot (int fslot)
{
	int i, attempt;
	if (fslot <= 0)
		return 0;
	if (G_LIKELY (mono_wasm_jit_slot_live (fslot)))
		return 1;
	{
		int n = wj_reg_n;          /* snapshot the count... */
		mono_memory_barrier ();    /* ...then ACQUIRE: entries + chunk pointers published before n are visible */
		for (i = 0; i < n; ++i) {
			WjRegEntry *re = wj_reg_at (i);
			if (!re || re->f != fslot)
				continue;
			for (attempt = 0; attempt < 3 && !mono_wasm_jit_slot_live (fslot); ++attempt) {
				char eb [192]; eb [0] = 0; double ms = 0;
				void *bytes = re->bytes; int len = re->len;
				if (!bytes || len <= 0 || len >= (16 * 1024 * 1024))
					break;   /* registry entry not fully published / bogus length */
				mono_wasm_jit_instantiate_local (re->e, re->f, bytes, len, eb, (int) sizeof (eb), &ms);
				if (mono_wasm_jit_slot_live (fslot))
					return 1;
				mono_memory_barrier ();   /* before the next attempt: re-read shared bytes coherently */
			}
			break;   /* fslot is unique in the registry */
		}
	}
	return mono_wasm_jit_slot_live (fslot);
}


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
 *   entry_sp                                  <- restored at every exit (stackRestore)
 *     ref slots   [refbase + slot*4)          <- refbase = frame base; zeroed in the prologue
 *     addr slots  [addrbase + offset)         <- addrbase = refbase + align8(nrefslots*4)
 *   frame = align16(entry_sp - framebytes)    <- the new __stack_pointer after the prologue
 * SP access from JITted code uses the same baked-C-function call_indirect mechanism as every other
 * runtime helper: emscripten_stack_get_current () -> i32 and stackRestore (i32) -> void (the
 * compiler-rt primitives the main module already exports as stackSave/stackRestore).
 */
#ifdef HOST_BROWSER
#include <emscripten/stack.h>
extern void stackRestore (uintptr_t sp);   /* compiler-rt (stack_ops.S): sets __stack_pointer */
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
/* MONO_WASM_JIT_PINALL: 1 = mark EVERY i32 vreg as a reference so it is routed to the GC-scanned (pinning)
 * ref shadow stack instead of a GC-invisible wasm local. Over-marking a non-ref i32 is harmless (it just
 * conservatively pins whatever the value points at). DEBUG TEST to confirm/refute a missed-ref coverage gap
 * as the corruption source. Very slow (every int op hits memory). Default off. */
int mono_wasm_jit_pinall = 0;
/* MONO_WASM_JIT_OBJGUARD: 1 = before every reference-field store, validate the OBJECT BASE is a live heap
 * object (mono_wasm_jit_check_store kind=2), trapping at the culprit JITted method if it's stale/garbage —
 * the missed-ref / dangling-base detector (the heap-store analog of STOREGUARD, which only guards the
 * shadow-stack/addr-frame stores). DEBUG ONLY (a C call per ref store). Default off. */
int mono_wasm_jit_objguard = 0;

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
		 * where the byref points at wj_ref_sp — that is the real garbage-SP catch.) NULL is a NRE elsewhere. */
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
	case OP_LDADDR:       /* address of an address-taken local -> i32 (frame base + slot offset) on wasm32 */
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
	case OP_LCONV_TO_I: case OP_LCONV_TO_U:     /* i64 -> native int/uint = i32 on wasm32 (wrap) */
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
	case OP_LDIV: case OP_LDIV_UN: case OP_LREM: case OP_LREM_UN:
	case OP_LAND: case OP_LOR: case OP_LXOR:
	case OP_LSHL: case OP_LSHR: case OP_LSHR_UN:
	case OP_LADD_IMM: case OP_LSUB_IMM: case OP_LMUL_IMM:
	case OP_LDIV_IMM: case OP_LDIV_UN_IMM: case OP_LREM_IMM: case OP_LREM_UN_IMM:
	case OP_LAND_IMM: case OP_LOR_IMM: case OP_LXOR_IMM:
	case OP_LSHL_IMM: case OP_LSHR_IMM: case OP_LSHR_UN_IMM:
	case OP_LNOT: case OP_LNEG:
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

/* TRUE iff this instruction's result PROVABLY cannot be a managed pointer (so the dreg may stay in a fast
 * wasm local instead of the GC-scanned ref shadow stack). This is an allow-list of value-producing opcodes
 * whose result is, by ECMA-335 semantics, an integer/float scalar: integer arithmetic/logic/shift/compare,
 * width/sign conversions, float-bit reinterprets, integer constants, and the typed-width integer loads (a
 * reference field always lowers to a bare OP_LOAD_MEMBASE, never a typed load). A STACK_OBJ/STACK_MP result
 * is a managed pointer regardless of opcode, so it is rejected up front. Anything NOT proven here is treated
 * as a possible ref (conservative-by-default): over-marking only over-pins on the conservative root, while a
 * missed ref is a silent dangling-pointer bug. OP_MOVE and call results are classified by the caller (they
 * need the source vreg / the call signature, not just the opcode). */
static gboolean
wj_opcode_is_nonref (MonoInst *ins)
{
	if (ins->type == STACK_OBJ || ins->type == STACK_MP)
		return FALSE;
	switch (ins->opcode) {
	/* NOTE: integer add/sub and and-mask (incl. their _IMM forms) are deliberately NOT here:
	 * on wasm32 OP_PADD/OP_PSUB/OP_PAND_IMM alias OP_IADD/OP_ISUB/OP_IAND_IMM, so "add" is how
	 * every interior pointer (ldelema/ldflda/Unsafe.Add) is formed — and the front end doesn't
	 * always type those STACK_MP (EMIT_NEW_BIALU_IMM leaves type 0; Unsafe.* uses STACK_PTR).
	 * The fixpoint loop taint-propagates them instead: add/sub/mask of a proven-scalar source
	 * stays scalar, of a possible-ref source is a possible ref. */
	case OP_ICONST: case OP_I8CONST:
	case OP_IMUL:
	case OP_IDIV: case OP_IDIV_UN: case OP_IREM: case OP_IREM_UN:
	case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN:
	case OP_INEG: case OP_INOT:
	case OP_IMUL_IMM:
	case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_IDIV_IMM: case OP_IDIV_UN_IMM: case OP_IREM_IMM: case OP_IREM_UN_IMM:
	case OP_MUL_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_ICONV_TO_U1: case OP_ICONV_TO_I1: case OP_ICONV_TO_U2: case OP_ICONV_TO_I2:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:
	case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADU1_MEMBASE: case OP_LOADI1_MEMBASE: case OP_LOADU2_MEMBASE: case OP_LOADI2_MEMBASE:
	case OP_MOVE_F_TO_I4:
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
		return TRUE;
	default:
		return FALSE;
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
	/* OP_LDADDR support: address-taken SCALAR locals live in the per-thread addressable-locals frame
	 * (linear memory) instead of a wasm local, so their address can be passed to callees. addrslot[vreg]
	 * is the byte offset into that frame (>=0 if the vreg is such a local, else -1); addrbase is the wasm
	 * i32 local holding the frame base; vt[] gives each vreg's wasm valtype (for the load/store width);
	 * addr_tmp[g] is a per-type-group scratch local used to reorder [value]->[addr,value] for the store. */
	int *addrslot;
	/* For an address-taken SUB-WORD local, the declared managed width+signedness so wasm_addr_ld reads it
	 * back correctly after a NARROW callee write (1=load8_s, 2=load8_u, 3=load16_s, 4=load16_u; 0=full). A
	 * callee given &local writes only the managed type's size; the slot's upper bytes keep their stale value
	 * (addr_enter zero, or the JIT's last full store), so a plain i32.load would mis-read a signed sub-word
	 * (e.g. a -1 short reads as 0x0000FFFF). A width-correct narrow load reads only the live low bytes. */
	signed char *addr_ldkind;
	int addrbase;
	WasmValtype *vt;
	int addr_tmp [4];
	/* MONO_WASM_JIT_STOREGUARD debug assert: when storeguard != 0, every ref-shadow-stack / addr-frame
	 * store is preceded by a call_indirect to mono_wasm_jit_check_store(addr, kind) (functype index
	 * check_ti = (i32,i32)->void) that traps if the target is outside this thread's arena. */
	int storeguard;
	int objguard;     /* MONO_WASM_JIT_OBJGUARD: validate the object base of ref-field stores (check_store kind=2) */
	int check_ti;
} WjCtx;

/* Emit a memory load/store of an addressable local (addrslot[vreg] >= 0), at addrbase + offset, using the
 * vreg's wasm valtype for the width. Returns FALSE on an unsupported width. */
static gboolean
wasm_addr_ld (WasmBuf *b, WjCtx *c, int vreg)
{
	guint32 off = (guint32) c->addrslot [vreg];
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	if (c->vt [vreg] == WASM_I32 && c->addr_ldkind && c->addr_ldkind [vreg]) {
		/* sub-word local: read back only the live low byte(s) with the declared signedness, so a narrow
		 * callee write (which left the upper bytes stale) is interpreted correctly. */
		switch (c->addr_ldkind [vreg]) {
		case 1: wasm_op (b, WASM_OP_I32_LOAD8_S);  wasm_memarg (b, 0, off); return TRUE;
		case 2: wasm_op (b, WASM_OP_I32_LOAD8_U);  wasm_memarg (b, 0, off); return TRUE;
		case 3: wasm_op (b, WASM_OP_I32_LOAD16_S); wasm_memarg (b, 1, off); return TRUE;
		case 4: wasm_op (b, WASM_OP_I32_LOAD16_U); wasm_memarg (b, 1, off); return TRUE;
		default: break;
		}
	}
	switch (c->vt [vreg]) {
	case WASM_I32: wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, off); return TRUE;
	case WASM_I64: wasm_op (b, WASM_OP_I64_LOAD); wasm_memarg (b, 3, off); return TRUE;
	case WASM_F32: wasm_op (b, WASM_OP_F32_LOAD); wasm_memarg (b, 2, off); return TRUE;
	case WASM_F64: wasm_op (b, WASM_OP_F64_LOAD); wasm_memarg (b, 3, off); return TRUE;
	default: return FALSE;
	}
}

static gboolean
wasm_addr_st (WasmBuf *b, WjCtx *c, int vreg)
{
	guint32 off = (guint32) c->addrslot [vreg];
	int g = wasm_valtype_group (c->vt [vreg]);
	WasmOpcode sop; int al;
	if (g < 0) return FALSE;
	/* value is on the wasm stack; i32.store wants [addr, value], so stash value -> per-type scratch,
	 * push addr (addrbase), re-push value, then store. */
	switch (c->vt [vreg]) {
	case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
	case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
	case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
	case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
	default: return FALSE;
	}
	wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->addr_tmp [g]);
#ifdef HOST_BROWSER
	if (G_UNLIKELY (c->storeguard)) {
		/* check_store(addrbase + off, 1) before the store (STOREGUARD; arenas+helper are HOST_BROWSER) */
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (b, (gint32) off);
		wasm_op (b, WASM_OP_I32_ADD);
		wasm_i32_const (b, 1);
		wasm_i32_const (b, (gint32) (intptr_t) mono_wasm_jit_check_store);
		wasm_op (b, WASM_OP_CALL_INDIRECT); wasm_uleb (b, (guint32) c->check_ti); wasm_uleb (b, 0);
	}
#endif
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addr_tmp [g]);
	wasm_op (b, sop); wasm_memarg (b, (guint32) al, off);
	return TRUE;
}

/* Normalize a sub-word integer return value (already on the wasm stack) to the full i32 the IR consumer
 * expects. A raw llvmonly AOT body returns bool/i1/u1/i2/u2 with UNDEFINED upper bits (LLVM legalizes `ret
 * i8/i16` with no zeroext/signext), so a JITted caller that consumes the dreg in an i32 compare/branch/
 * index would read garbage. Mirrors the residual interp_entry normalization. */
static void
wasm_emit_subword_ret_norm (WasmBuf *b, MonoType *ret)
{
	switch (mini_get_underlying_type (ret)->type) {
	case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: wasm_i32_const (b, 0xff); wasm_op (b, WASM_OP_I32_AND); break;
	case MONO_TYPE_I1: wasm_op (b, WASM_OP_I32_EXTEND8_S); break;
	case MONO_TYPE_CHAR: case MONO_TYPE_U2: wasm_i32_const (b, 0xffff); wasm_op (b, WASM_OP_I32_AND); break;
	case MONO_TYPE_I2: wasm_op (b, WASM_OP_I32_EXTEND16_S); break;
	default: break;
	}
}

static gboolean
wasm_ld (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->addrslot && c->addrslot [vreg] >= 0)   /* address-taken local: read from the addr frame */
		return wasm_addr_ld (b, c, vreg);
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
wasm_guard_memaddr (WasmBuf *b, WjCtx *c, int base_vreg, gint32 offset)
{
#ifdef HOST_BROWSER
	if (G_UNLIKELY (c->objguard && c->check_ti >= 0)) {
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		if (!wasm_ld (b, c, base_vreg))
			return FALSE;
		if (offset) {
			wasm_i32_const (b, offset);
			wasm_op (b, WASM_OP_I32_ADD);
		}
		wasm_i32_const (b, 4);   /* generic membase access address */
		wasm_i32_const (b, (gint32) (intptr_t) mono_wasm_jit_check_store);
		wasm_op (b, WASM_OP_CALL_INDIRECT); wasm_uleb (b, (guint32) c->check_ti); wasm_uleb (b, 0);
	}
#endif
	return TRUE;
}

static gboolean
wasm_st (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->addrslot && c->addrslot [vreg] >= 0)   /* address-taken local: write to the addr frame */
		return wasm_addr_st (b, c, vreg);
	if (c->refslot && c->refslot [vreg] >= 0) {
		/* reference vreg: store the value (already on the wasm stack) to the ref shadow stack.
		 * i32.store wants [addr, val] but val is on top, so stash it via rtmp then push addr+val. */
		wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->rtmp);
#ifdef HOST_BROWSER
		if (G_UNLIKELY (c->storeguard)) {
			/* check_store(refbase + slot*4, 0) before the store (STOREGUARD; arenas+helper are HOST_BROWSER) */
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
			wasm_i32_const (b, (gint32) (c->refslot [vreg] * 4));
			wasm_op (b, WASM_OP_I32_ADD);
			wasm_i32_const (b, 0);
			wasm_i32_const (b, (gint32) (intptr_t) mono_wasm_jit_check_store);
			wasm_op (b, WASM_OP_CALL_INDIRECT); wasm_uleb (b, (guint32) c->check_ti); wasm_uleb (b, 0);
		}
#endif
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

#ifdef HOST_BROWSER
/* The synchronized-INNER wrapper (the dummy a MONO_WRAPPER_SYNCHRONIZED wrapper calls to reach the real
 * body — see method-to-ir.c) is created FRESH on every IR build (mono_marshal_get_synchronized_inner_wrapper
 * is uncached), so its MonoMethod — and thus its InterpMethod and wasm f-slot — changes on every compile.
 * Under the f-slot model a synchronized wrapper could then NEVER observe its inner as JITted: it re-bails
 * "callee not jitted" and the island re-compiles a fresh inner each pass (a table-slot leak + render
 * hitches). Canonicalize it: map the wrapped method -> the FIRST inner-wrapper instance we see and always
 * use THAT one for the f-slot lookup / island blocker / compile, so the slot is stable. Every inner wrapper
 * for a given method is the same stub (resolved to the same body), so substituting the canonical one is
 * sound. Other callees pass through unchanged. */
static GHashTable *wj_sync_inner_canon;   /* wrapped MonoMethod* -> canonical synchronized-inner-wrapper MonoMethod* */
static MonoMethod *
wj_canonical_callee (MonoMethod *m)
{
	WrapperInfo *info;
	MonoMethod *wrapped, *canon;
	if (!m || m->wrapper_type != MONO_WRAPPER_OTHER)
		return m;
	info = mono_marshal_get_wrapper_info (m);
	if (!info || info->subtype != WRAPPER_SUBTYPE_SYNCHRONIZED_INNER)
		return m;
	wrapped = info->d.synchronized_inner.method;
	if (!wrapped)
		return m;
	mono_loader_lock ();
	if (!wj_sync_inner_canon)
		wj_sync_inner_canon = g_hash_table_new (g_direct_hash, g_direct_equal);
	canon = (MonoMethod *) g_hash_table_lookup (wj_sync_inner_canon, wrapped);
	if (!canon) { canon = m; g_hash_table_insert (wj_sync_inner_canon, wrapped, m); }
	mono_loader_unlock ();
	return canon;
}

/* Append a blocking callee to the cfg result, deduped + capped. Shared by the residual=0 pre-scan
 * (enumerates the full set) and the emit bail site (records the first blocker it hits) so the two can't
 * disagree on the head of the list. */
static void
wj_result_add_blocker (MonoWasmJitResult *res, MonoMethod *m)
{
	int i;
	for (i = 0; i < res->nblockers; ++i)
		if (res->blockers [i] == m)
			return;
	if (res->nblockers < MONO_WASM_JIT_MAX_BLOCKERS)
		res->blockers [res->nblockers++] = m;
	else
		res->blockers_truncated = 1;
}

/* Pre-scan (residual=0 / islands only): enumerate ALL direct un-JITted callees that would HARD-BLOCK this
 * method's compile, into cfg->wasm_jit_result.blockers — so wasm_jit_force_island can pull them all in one
 * emit cycle instead of re-emitting the method once per blocker (the emit itself still bails at the FIRST
 * blocker; this just hands the island builder the full set up front). The predicate MUST mirror the bail
 * site in the OP_CALL lowering below: a direct OP_CALL-family call, method!=NULL, callee has no f-slot, not
 * an rgctx call, not AOT-residual-eligible, not (residual_perm && perm-unjittable). INLINE_AOT (default
 * off) is not modelled — when on it can only REMOVE a blocker (inlines an AOT callee), so the worst case is
 * force_island wasting a little budget on a callee that would actually inline; never a correctness issue. */
static void
wj_prescan_blockers (MonoCompile *cfg)
{
	extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
	extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
	extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
	extern int mono_wasm_jit_callee_too_cold (MonoMethod *m);
	extern int mono_wasm_jit_aot_residual, mono_wasm_jit_residual_perm, mono_wasm_jit_residual_cold, mono_wasm_jit_sync;
	extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
	MonoBasicBlock *bb;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			MonoCallInst *call;
			MonoMethod *call_method;
			MonoMethodSignature *csig;
			switch (ins->opcode) {
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL:
				break;
			default:
				continue;
			}
			call = (MonoCallInst *) ins;
			call_method = call->method;
			csig = call->signature;
			if (!call_method || !csig)        /* method==NULL = JIT-icall (never a managed blocker); no sig handled by emit */
				continue;
			if (call->rgctx_reg)              /* rgctx call -> routed through residual, never a hard blocker */
				continue;
			if (call_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
				if (!mono_wasm_jit_sync)      /* sync disabled -> emit bails permanently (not retriable); skip */
					continue;
				call_method = mono_marshal_get_synchronized_wrapper (call_method);
			}
			call_method = wj_canonical_callee (call_method);   /* stabilize the per-compile synchronized-inner wrapper */
			if (call_method == cfg->method)                                                        /* self-recursion: baked via self-slot reservation, not a blocker */
				continue;
			if (mono_wasm_jit_get_callee_fslot (call_method) > 0)                                   /* already JITted */
				continue;
			if (mono_wasm_jit_aot_residual && mono_interp_jit_call_supported (call_method, csig))    /* AOT-routed residual */
				continue;
			if (mono_wasm_jit_residual_perm && mono_wasm_jit_callee_perm_unjittable (call_method))   /* perm-unjittable -> residual */
				continue;
			if (mono_wasm_jit_residual_cold && mono_wasm_jit_callee_too_cold (call_method))          /* cold leaf -> residual, not a hard blocker */
				continue;
			wj_result_add_blocker (&cfg->wasm_jit_result, call_method);
		}
	}
}
#endif

/* Scalar-vtype call ABI (MONO_WASM_JIT_VTYPE_SCALAR). A struct mini_wasm_is_scalar_vtype accepts (<=8
 * bytes, exactly one field) is passed in the wasm ABI BY ITS SINGLE FIELD's scalar, not by address
 * (get_storage -> ArgVtypeAsScalar -> LLVMArgWasmVtypeAsScalar). wj_scalar_vtype_valtype returns that
 * scalar's wasm valtype for a REF-FREE such struct (sets *out, returns TRUE), else FALSE. A ref-etype
 * scalar vtype (single object field, e.g. RuntimeTypeHandle{RuntimeType}) returns FALSE: its scalar is
 * a managed ref that would have to be GC-tracked, which the un-scanned addr frame the ByVal value lives
 * in cannot provide — those still bail earlier ("ldaddr of vtype with refs") and want the ref-shadow
 * path (not yet implemented). Cross-available (used by the emit below in both builds). */
static gboolean
wj_scalar_vtype_valtype (MonoType *t, WasmValtype *out)
{
	MonoType *ut, *etype = NULL;
	MonoClass *k;
	WasmValtype ev;
	if (!t || m_type_is_byref (t))
		return FALSE;
	ut = mini_get_underlying_type (t);
	if (ut->type != MONO_TYPE_VALUETYPE && ut->type != MONO_TYPE_GENERICINST)
		return FALSE;
	if (mini_is_gsharedvt_variable_type (ut))
		return FALSE;
	if (!mini_wasm_is_scalar_vtype (ut, &etype) || !etype)
		return FALSE;
	k = mono_class_from_mono_type_internal (ut);
	if (!k)
		return FALSE;
	{
		extern int mono_wasm_jit_vtype_scalar, mono_wasm_jit_vtype_scalar_ref;
		if (m_class_has_references (k) || m_class_has_ref_fields (k)) {
			if (!(mono_wasm_jit_vtype_scalar_ref && mini_type_is_reference (etype)))
				return FALSE;   /* ref-etype: only under VTYPE_SCALAR_REF (GC-scanned slot) */
		} else if (!mono_wasm_jit_vtype_scalar) {
			return FALSE;   /* ref-free scalar vtype gated on VTYPE_SCALAR */
		}
	}
	ev = wasm_valtype_of_type (etype);
	if (ev == 0 || ev == WASM_VOID)
		return FALSE;
	*out = ev;
	return TRUE;
}

/* Emit a BYVAL ref-free scalar-vtype call arg onto the wasm stack. The ByVal value is addr-frame-backed
 * (LDADDR_VTYPE gave the source vtype temp a slot, and LOWER-VTYPE-OPTS lowered its vzero/field-store/
 * vmove to plain stores into that slot), so load the single field (offset 0) as the etype scalar — what
 * the AOT callee's LLVMArgWasmVtypeAsScalar param expects. Returns FALSE (the caller then bails the whole
 * method — safe) if the arg isn't a ref-free scalar vtype or the ByVal value has no addr-frame slot. */
static gboolean
wj_emit_scalar_vtype_arg (WasmBuf *body, WjCtx *c, MonoType *pt, int argvreg)
{
	WasmValtype ev;
	guint32 off;
	if (!wj_scalar_vtype_valtype (pt, &ev))
		return FALSE;
	if (!c->addrslot || argvreg < 0 || argvreg >= c->nvreg)
		return FALSE;
	if (c->addrslot [argvreg] == -2) {
		/* ref-etype scalar vtype: the single ref lives in a GC-scanned ref-shadow slot; load it (i32). */
		if (ev != WASM_I32 || !c->refslot || c->refslot [argvreg] < 0)
			return FALSE;
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		if (c->refslot [argvreg] != 0) { wasm_i32_const (body, c->refslot [argvreg] * 4); wasm_op (body, WASM_OP_I32_ADD); }
		wasm_op (body, WASM_OP_I32_LOAD); wasm_memarg (body, 2, 0);
		return TRUE;
	}
	if (c->addrslot [argvreg] < 0)
		return FALSE;
	off = (guint32) c->addrslot [argvreg];
	wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	switch (ev) {
	case WASM_I32: wasm_op (body, WASM_OP_I32_LOAD); wasm_memarg (body, 2, off); return TRUE;
	case WASM_I64: wasm_op (body, WASM_OP_I64_LOAD); wasm_memarg (body, 3, off); return TRUE;
	case WASM_F32: wasm_op (body, WASM_OP_F32_LOAD); wasm_memarg (body, 2, off); return TRUE;
	case WASM_F64: wasm_op (body, WASM_OP_F64_LOAD); wasm_memarg (body, 3, off); return TRUE;
	default: return FALSE;
	}
}

/* Emit one managed-call arg (index ai over the wasm functype params, incl. `this` at 0 when hasthis).
 * A BYVAL ref-free scalar-vtype param (MONO_WASM_JIT_VTYPE_SCALAR) is loaded as its single-field etype
 * scalar from the ByVal value's addr-frame slot; everything else is a normal wasm_ld of the arg vreg.
 * Shared by the direct-JIT, inline-AOT and interp-residual arg loops so all three agree on the ABI
 * (the residual then spills that scalar into the scratch slot, and interp_entry copies it back as the
 * vtype value — the field IS the whole vtype, so the bytes match). */
static gboolean
wj_emit_one_call_arg (WasmBuf *body, WjCtx *c, MonoMethodSignature *csig, int *wargs, int ai)
{
	int pidx = ai - (csig->hasthis ? 1 : 0);
	if (pidx >= 0 && pidx < (int) csig->param_count) {
		WasmValtype sv;
		if (wj_scalar_vtype_valtype (csig->params [pidx], &sv))
			return wj_emit_scalar_vtype_arg (body, c, csig->params [pidx], wargs [ai]);
	}
	return wasm_ld (body, c, wargs [ai]);
}

/* Fast-path volume profiling (MONO_WASM_JIT_PROFILE_FAST): emit an inline atomic increment of
 * mono_wasm_jit_counters[idx] into the JITted body. &counters[idx] is a link-time-constant address in the
 * runtime's linear memory (the emitter runs in the same process, same memory — just like the embedded C
 * function-pointer constants). i64.atomic.rmw.add (0xfe 0x26, align 3) keeps the count correct when worker
 * threads share the site; the returned old value is dropped. No-op unless profile_fast is set at emit time,
 * so normal STATS runs pay nothing. Only called from HOST_BROWSER fast-path sites; G_GNUC_UNUSED for the
 * cross-compiler build (which never reaches those sites). */
static G_GNUC_UNUSED void
wj_emit_fast_count (WasmBuf *body, int idx)
{
	extern int mono_wasm_jit_profile_fast;
	extern gint64 mono_wasm_jit_counters [];
	if (!mono_wasm_jit_profile_fast)
		return;
	wasm_i32_const (body, (gint32) (intptr_t) &mono_wasm_jit_counters [idx]);
	wasm_i64_const (body, 1);
	wasm_op (body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (body, 0x1f); wasm_memarg (body, 3, 0);   /* i64.atomic.rmw.add (0xfe 0x1f; 0x26 is rmw.SUB!) */
	wasm_op (body, WASM_OP_DROP);
}

/* Max distinct call_indirect callee functypes per JITted method (T0/T1 are the 2 base types; these are
 * the extras). Raised 32->128: big hot methods (class_1309:method_6091 LivingEntity, class_922:method_4054
 * entity render) have >32 distinct call signatures and were bailing "too many callee types". wasm allows
 * arbitrarily many functypes, so this is just the on-stack array size (~128*sizeof(WasmFuncType)). */
#define WJ_EXTRA_TYPES_MAX 128

void
mono_wasm_emit_method (MonoCompile *cfg)
{
#ifdef HOST_BROWSER
	if (mono_wasm_jit_verbose >= 2) { printf ("WASM_JIT_EMIT_ENTER %s opt=0x%x\n", cfg->method ? cfg->method->name : "?", (unsigned) cfg->opt); }
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
	int *addrslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* address-taken local vreg -> byte offset in the addr frame, else -1 */
	signed char *addr_ldkind = (signed char *) mono_mempool_alloc0 (cfg->mempool, sizeof (signed char) * nvreg); /* sub-word read-back kind (0=full), see WjCtx.addr_ldkind */
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
	int spentry_idx = 0;               /* i32 local: C-stack SP at entry (exit/landing-pad restore target) */
	int framebytes = 0, refbytes_al = 0; /* C-stack frame size (16-aligned) / 8-aligned ref-slot bytes */
	int vc_fslot_idx = 0;              /* i32 local: inline virtual-IC fast-path resolved f-slot */
	int vc_aotkind_idx = 0;            /* i32 local: VCALL_AOT dispatch kind from vcall_aot_target (0=residual,1=+rgctx,2=no-extra) */
	int aic_vtab_idx = 0, aic_ti_idx = 0, aic_rgctx_idx = 0; /* i32 locals: AOT-vcall IC — this->vtable, ti<<1|kind2, rgctx */
	int slotlive_ptr_idx = 0, slotlive_cap_idx = 0; /* i32 locals: cached &wj_slot_live / &wj_slot_live_cap for the INLINE f-slot-IC liveness check (dead in methods with no vcall) */
	gboolean has_vcall = FALSE;        /* TRUE: method has >=1 OP_*CALL_MEMBASE (a vcall-IC site) -> emit the prologue slotlive fetch */
	int vc_ic_idx = 0;                 /* i64 local: inline virtual-IC fast-path IC value (vtable|imethod<<32) */
	int eh_exc_idx = 0, eh_h_idx = 0;  /* i32 locals: in-method EH catch landing pad — saved C++ exc ptr + dispatch result */
	int finally_ind_idx = 0;           /* i32 local: in-method finally indicator (continuation bb idx, or -1 = rethrow) */
	gboolean eh_has_finally = FALSE;   /* TRUE: method has >=1 FINALLY clause (milestone 2c) */
	WasmEhTable *eh_table = NULL;      /* in-method EH clause table (built below, baked into the catch landing pad) */
	gboolean eh_on = FALSE;            /* TRUE: emit the in-method try/catch wrapper for this method */
	int eh_dispatch_ti = -1, eh_endcatch_ti = -1;  /* functype indices: (i32,i32)->i32 dispatch + ()->void end_catch */
	int nrefslots = 0;                 /* number of reference vregs routed to the GC ref shadow stack */
	int enter_ti = -1, leave_ti = -1;  /* functype indices: emscripten_stack_get_current ()->i32 / stackRestore (i32)->void */
	int addrbase_idx = 0;              /* i32 local: addressable-locals frame base address (OP_LDADDR) */
	int addr_tmp_idx [4] = { 0, 0, 0, 0 }; /* per-type scratch locals for addr-frame stores (i32/i64/f32/f64) */
	int naddrbytes = 0;                /* total bytes of addressable-locals frame (8 per address-taken local) */
	WjCtx lc;
	int *bbidx;
	WasmFuncType extra_types [WJ_EXTRA_TYPES_MAX]; /* callee functypes for call_indirect, after T0/T1 */
	int nextra = 0;
	gboolean uses_calls = FALSE;
	gboolean uses_eh_tag = FALSE;   /* in-method EH landing pad -> import the C++ exception tag x.e */
	int eh_type_idx = -1;           /* type index of (i32)->void (the catch handler + the tag) */
	const char *fail = NULL;
	int fail_op = -1;
	char *mname = mono_method_get_full_name (cfg->method);
#ifdef HOST_BROWSER
	/* Self-recursive DIRECT calls under residual=0: a method calling itself can't bake its own f-slot
	 * because the slot doesn't exist until this compile finishes. Reserve our OWN e/f-slot pair lazily on
	 * the first self-call (reusing a pair recycled from a prior bailed self-emit if one is pending — the
	 * jiterp table is append-only, so we can't free) and bake it; the success path instantiates INTO these.
	 * 0 = none reserved. wj_recycle_* is a per-thread (__thread) static so it needs no lock and can never
	 * alias a slot across threads even on the unserialized name-targeted compile path. */
	static __thread int wj_recycle_e_slot = 0, wj_recycle_f_slot = 0;
	int wj_self_e_slot = 0, wj_self_f_slot = 0;
#endif

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
		{ guint _ci;   /* in-method EH clauses always propagate via C++/wasm-EH (cppeh is the only model) */
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
			  else if (_f == MONO_EXCEPTION_CLAUSE_FAULT) {
				  /* fault: like finally but runs ONLY on the exception path (never on normal leave), then
				   * re-raises. IKVM emits these (compiler.cs BeginFaultBlock) for catch-all/synchronized
				   * cleanup in Mixin-transformed bytecode (Lithium/Starlight/indigo) whose shape its
				   * finally-pattern-matcher didn't recognize. The handler ends with OP_ENDFINALLY (== endfault),
				   * which re-raises on the exception path, and has no OP_CALL_HANDLER, so faithful IL lowering
				   * never runs it on the normal path. Reuse the finally_ind machinery (flag like finally);
				   * mono_wasm_jit_eh_dispatch treats FAULT identically to FINALLY. */
				  eh_has_finally = TRUE;
			  }
			  else if (_f != MONO_EXCEPTION_CLAUSE_NONE) { fail = "EH clause kind (filter not supported)"; goto done; }
		  }
		  /* Bail a method with a finally/fault clause whose TRY region is nested inside ANOTHER finally/fault
		   * clause's HANDLER region — i.e. a try/finally inside a finally body, e.g. `finally { using (x) {...} }`
		   * or `finally { lock (o) {...} }`. There is only ONE finally_ind continuation local: while the outer
		   * finally runs, the inner leave's OP_CALL_HANDLER overwrites finally_ind, so the outer OP_ENDFINALLY
		   * then branches to the inner continuation (silent wrong control flow) — or, on the exception path, the
		   * clobbered finally_ind makes the outer endfinally skip its re-raise (exception swallowed/escapes).
		   * Until each finally gets its own continuation slot, run such methods in the interpreter. */
		  { guint _a, _b;
		    for (_a = 0; _a < cfg->header->num_clauses && !fail; ++_a) {
			    MonoExceptionClause *_ca = &cfg->header->clauses [_a];
			    if (_ca->flags != MONO_EXCEPTION_CLAUSE_FINALLY && _ca->flags != MONO_EXCEPTION_CLAUSE_FAULT) continue;
			    for (_b = 0; _b < cfg->header->num_clauses; ++_b) {
				    MonoExceptionClause *_cb = &cfg->header->clauses [_b];
				    if (_b == _a) continue;
				    if (_cb->flags != MONO_EXCEPTION_CLAUSE_FINALLY && _cb->flags != MONO_EXCEPTION_CLAUSE_FAULT) continue;
				    if (_cb->try_offset >= _ca->handler_offset && _cb->try_offset < _ca->handler_offset + _ca->handler_len) {
					    fail = "nested finally inside finally handler (single finally_ind)"; goto done; }
			    }
		    } }
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
	 * skipped for rgctx calls because it can only bake the callee's extra arg/rgctx, not the separate CALLSITE
	 * runtime generic context carried in MONO_ARCH_RGCTX_REG;
	 * indirect/virtual rgctx calls still bail. This unblocks IKVM EH methods whose catch block calls the
	 * generic ExceptionHelper.MapException<T> (the #1 render-path blocker after the EH gate). */
	{ extern int mono_wasm_jit_rgctx;
	  if (!mono_wasm_jit_rgctx && cfg->uses_rgctx_reg) { fail = "uses rgctx reg"; goto done; } }

	for (i = 0; i < nvreg; ++i) {
		li [i] = -1;
		refslot [i] = -1;
		addrslot [i] = -1;
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
			if (rt && ins->dreg >= 0 && ins->dreg < nvreg && li [ins->dreg] < 0) {
				/* Two defs with different wasm valtypes (possible once opt passes coalesce
				 * vregs): last-writer-wins would emit a type-invalid module that only fails
				 * at instantiate (WJC_INVALID, whole-module loss). Bail cleanly instead. */
				if (vt [ins->dreg] && vt [ins->dreg] != rt) { fail = "vreg valtype conflict"; fail_op = ins->opcode; goto done; }
				vt [ins->dreg] = rt;
			}
		}
	}

	/* OP_LDADDR pre-pass: find every address-taken local and route it to the addressable-locals frame.
	 * A var whose address is taken (ldloca/ldarga -> OP_LDADDR, inst_p0 = the var) can't live in a wasm
	 * local — wasm locals have no address — so back it with an 8-byte slot in the per-thread addr frame:
	 * all of its OP_MOVE loads/stores route through that memory (wasm_ld/wasm_st via addrslot), and
	 * OP_LDADDR yields addrbase+offset to hand to the callee. Only NON-REFERENCE SCALAR LOCALS are
	 * supported: a ref/byref slot would need GC tracking the frame doesn't provide, a vtype has no scalar
	 * valtype, and an address-taken ARGUMENT (still in a wasm param) is bailed too. Anything unsupported
	 * bails the whole method (the prior behaviour for any OP_LDADDR). MONO_WASM_JIT_LDADDR=0 disables the
	 * pass so OP_LDADDR bails as before.
	 * NB: NOT HOST_BROWSER-gated. This is a pure compile-time classification pass (it assigns addrslot
	 * offsets / decides which ldaddr shapes are supported); the addr-frame runtime helpers it feeds
	 * (the C-stack frame prologue, baked by the emit below) is HOST_BROWSER-gated separately. Un-gating it
	 * lets the offline cross-compiler dump reach the REAL ldaddr gate (e.g. "ldaddr of vtype with refs")
	 * instead of the spurious "ldaddr unsupported var" the emit hits when no slot was ever assigned. */
	{ extern int mono_wasm_jit_ldaddr;
	if (mono_wasm_jit_ldaddr) {
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			MONO_BB_FOR_EACH_INS (bb, ins) {
				MonoInst *var;
				int vv;
				WasmValtype lvt;
				if (ins->opcode != OP_LDADDR)
					continue;
				var = (MonoInst *) ins->inst_p0;
				if (!var || !var->inst_vtype) { fail = "ldaddr no var"; fail_op = OP_LDADDR; goto done; }
				vv = var->dreg;
				if (vv < 0 || vv >= nvreg) { fail = "ldaddr var vreg"; fail_op = OP_LDADDR; goto done; }
				if (addrslot [vv] >= 0)
					continue;   /* already assigned (same local addressed at multiple sites) */
				if (li [vv] >= 0) { fail = "ldaddr of arg local"; fail_op = OP_LDADDR; goto done; }   /* arg: in a wasm param, no address */
				if (m_type_is_byref (var->inst_vtype) || mini_type_is_reference (var->inst_vtype)) { fail = "ldaddr of ref/byref local"; fail_op = OP_LDADDR; goto done; }
				lvt = wasm_valtype_of_type (var->inst_vtype);
				if (lvt == 0 || lvt == WASM_VOID || wasm_valtype_group (lvt) < 0) {
					/* Non-scalar (valuetype) address-taken local. DEFAULT: bail. MONO_WASM_JIT_LDADDR_VTYPE=1
					 * backs it with a full-size addr-frame slot (field access via OP_LDADDR + MEMBASE). Gate:
					 * bail if the vtype holds managed refs (addr frame is not GC-scanned); vt[vv] stays 0 so a
					 * scalar/bulk value access bails. Exonerated re: the intermittent corruption (kept gated). */
					extern int mono_wasm_jit_ldaddr_vtype;
					MonoClass *vk;
					int vsize;
					if (!mono_wasm_jit_ldaddr_vtype) { fail = "ldaddr of non-scalar local"; fail_op = OP_LDADDR; goto done; }
					vk = mono_class_from_mono_type_internal (var->inst_vtype);
					if (!vk) { fail = "ldaddr vtype no class"; fail_op = OP_LDADDR; goto done; }
					vsize = mono_class_value_size (vk, NULL);
					if (m_class_has_references (vk) || m_class_has_ref_fields (vk)) {
						/* Ref-bearing vtype: addr frame is not GC-scanned. EXCEPTION (VTYPE_SCALAR_REF): a SCALAR vtype
						 * whose single field is a managed ref (e.g. RuntimeTypeHandle{RuntimeType}) is byte-for-byte one
						 * ref -> back it with a GC-SCANNED ref-shadow slot (sentinel addrslot=-2; refslot assigned after
						 * the isref pass). OP_LDADDR yields refbase+slot*4; the field store/load track it as a pinning
						 * root and the store card-barrier marks a harmless card (wasm32: no overlapping cards). */
						extern int mono_wasm_jit_vtype_scalar_ref;
						MonoType *setype = NULL;
						if (mono_wasm_jit_vtype_scalar_ref && vsize <= 8 && mini_wasm_is_scalar_vtype (var->inst_vtype, &setype) && setype && mini_type_is_reference (setype)) {
							addrslot [vv] = -2;
							continue;
						}
						fail = "ldaddr of vtype with refs (GC-unsafe frame)"; fail_op = OP_LDADDR; goto done;
					}
					if (vsize <= 0 || vsize > 4096) { fail = "ldaddr vtype size"; fail_op = OP_LDADDR; goto done; }
					naddrbytes = (naddrbytes + 7) & ~7;
					addrslot [vv] = naddrbytes;
					naddrbytes += (vsize + 7) & ~7;
					continue;
				}
				addrslot [vv] = naddrbytes;
				naddrbytes += 8;   /* 8 bytes/slot: 8-aligned, covers i64/f64; zero-init by the prologue frame fill */
				vt [vv] = lvt;     /* the local is only written via the escaped pointer in some shapes -> set its valtype explicitly */
				/* record sub-word width+signedness so wasm_addr_ld reads it back with a width-correct narrow
				 * load (a narrow callee write through &local leaves the upper bytes stale). */
				switch (mini_get_underlying_type (var->inst_vtype)->type) {
				case MONO_TYPE_I1:                       addr_ldkind [vv] = 1; break;   /* load8_s  (sbyte) */
				case MONO_TYPE_U1: case MONO_TYPE_BOOLEAN: addr_ldkind [vv] = 2; break; /* load8_u  (byte/bool) */
				case MONO_TYPE_I2:                       addr_ldkind [vv] = 3; break;   /* load16_s (short) */
				case MONO_TYPE_U2: case MONO_TYPE_CHAR:  addr_ldkind [vv] = 4; break;   /* load16_u (ushort/char) */
				default: break;
				}
			}
		}
	} }

	/* assign locals for non-arg typed vregs, grouped by type (skip address-taken locals: they live in the
	 * addr frame, not a wasm local) */
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0 || addrslot [i] >= 0)
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
	/* reserve two more i32 locals: the GC ref-slot frame base, and a scratch for ref stores */
	refbase_idx = nargs + cnt [0];
	cnt [0] += 1;
	rtmp_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* one more i32 local: the C-stack SP captured at entry (every exit stackRestores to it) */
	spentry_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* one more i32 local for the inline virtual-IC fast path's resolved f-slot (dead in methods with no
	 * virtual call — a harmless unused declared local) */
	vc_fslot_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the VCALL_AOT dispatch kind (0/1/2 from mono_wasm_jit_vcall_aot_target); dead in
	 * methods with no virtual call — a harmless unused declared local */
	vc_aotkind_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* i32 local: this->vtable, live across the AOT-vcall IC key+vtab_check compares (VCALL_AOT_IC) */
	aic_vtab_idx = nargs + cnt [0];
	cnt [0] += 1;
	aic_ti_idx = nargs + cnt [0];
	cnt [0] += 1;
	aic_rgctx_idx = nargs + cnt [0];
	cnt [0] += 1;
	/* two i32 locals caching &wj_slot_live / &wj_slot_live_cap for the inline f-slot-IC liveness check
	 * (dead in methods with no vcall — harmless unused declared locals) */
	slotlive_ptr_idx = nargs + cnt [0];
	cnt [0] += 1;
	slotlive_cap_idx = nargs + cnt [0];
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
	/* one i32 local for the addressable-locals frame base (OP_LDADDR; dead in methods with no address-taken
	 * local), plus the i32 scratch for addr-frame stores (only when there ARE such locals). */
	addrbase_idx = nargs + cnt [0];
	cnt [0] += 1;
	if (naddrbytes > 0) { addr_tmp_idx [0] = nargs + cnt [0]; cnt [0] += 1; }   /* i32 store scratch */
	base [0] = nargs;
	base [1] = base [0] + cnt [0];
	/* dedicated i64 local at the end of the i64 group for the inline virtual-IC value (atomically loaded) */
	vc_ic_idx = base [1] + cnt [1];
	cnt [1] += 1;
	if (naddrbytes > 0) { addr_tmp_idx [1] = base [1] + cnt [1]; cnt [1] += 1; }   /* i64 store scratch */
	base [2] = base [1] + cnt [1];
	if (naddrbytes > 0) { addr_tmp_idx [2] = base [2] + cnt [2]; cnt [2] += 1; }   /* f32 store scratch */
	base [3] = base [2] + cnt [2];
	if (naddrbytes > 0) { addr_tmp_idx [3] = base [3] + cnt [3]; cnt [3] += 1; }   /* f64 store scratch */
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0 || addrslot [i] >= 0)   /* addr-taken locals live in the frame, not a wasm local */
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
	lc.vt = vt; lc.addrbase = addrbase_idx;
	{ /* addrslot must be visible to the OP_LDADDR emit when there are ONLY refvt locals (sentinel -2,
	   * no addr-frame bytes) — otherwise naddrbytes==0 would NULL it and the refvt ldaddr misfires. */
		gboolean _have_refvt = FALSE; int _i; for (_i = 0; _i < nvreg; ++_i) if (addrslot [_i] == -2) { _have_refvt = TRUE; break; }
		lc.addrslot = (naddrbytes > 0 || _have_refvt) ? addrslot : NULL; }
	lc.addr_ldkind = (naddrbytes > 0) ? addr_ldkind : NULL;
	lc.addr_tmp [0] = addr_tmp_idx [0]; lc.addr_tmp [1] = addr_tmp_idx [1];
	lc.addr_tmp [2] = addr_tmp_idx [2]; lc.addr_tmp [3] = addr_tmp_idx [3];
	/* MONO_WASM_JIT_STOREGUARD (debug): pre-register the (i32,i32)->void functype for the per-store
	 * mono_wasm_jit_check_store call_indirect so wasm_st/wasm_addr_st can reference it by index. Done here
	 * (before body emission, nextra==0) so check_ti is stable; force uses_calls so the table is imported. */
	lc.storeguard = 0; lc.objguard = 0; lc.check_ti = -1;
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_storeguard, mono_wasm_jit_objguard;
		if ((mono_wasm_jit_storeguard || mono_wasm_jit_objguard) && nextra < WJ_EXTRA_TYPES_MAX) {
			WasmFuncType cst; memset (&cst, 0, sizeof (cst));
			cst.params [0] = WASM_I32; cst.params [1] = WASM_I32; cst.nparams = 2; cst.ret = WASM_VOID;
			extra_types [nextra] = cst; lc.check_ti = 2 + nextra++;
			lc.storeguard = mono_wasm_jit_storeguard ? 1 : 0;
			lc.objguard = mono_wasm_jit_objguard ? 1 : 0;
			uses_calls = TRUE;
		}
	}
#endif
#ifdef HOST_BROWSER
	/* Route every vreg that MIGHT hold a managed pointer to the GC-scanned ref shadow stack instead of a
	 * (GC-invisible) wasm local: a reference held in a wasm local across a GC point (a residual interp call,
	 * which can allocate and trigger a moving collection; a loop safepoint) would be moved/collected out
	 * from under the JITted method -> dangling pointer.
	 *
	 * Policy: CONSERVATIVE-BY-DEFAULT, mirroring the interpreter's own interp_mark_stack ("scan a slot unless
	 * it is provably non-ref"). Every pointer-sized (i32 on wasm32) vreg is assumed to be a ref and is cleared
	 * only when we can PROVE it scalar: cfg->vreg_is_ref / ref+byref args are pinned outright, and a temporary
	 * stays a fast local only if EVERY definition is an integer/float op (wj_opcode_is_nonref), an OP_MOVE from
	 * an already-proven-scalar source, or a call returning a scalar. cfg->vreg_is_ref alone is insufficient —
	 * it covers only mono_compile_create_var vregs (named locals/args), not the temporaries that hold call
	 * results, field loads, etc. across a residual (e.g. `s = Intern(x); i = indexOf(s, c)`). Because the
	 * shadow stack is a conservative PINNING root, over-marking a non-ref i32 is harmless (it just pins
	 * whatever the value points at) while a MISSED ref is silent corruption — so we err toward marking. This
	 * is the production form of MONO_WASM_JIT_PINALL: the proven-scalar arithmetic/compare/const/typed-load
	 * population stays in fast wasm locals; only genuinely-maybe-ref vregs go to memory.
	 *
	 * Implemented as a greatest fixed point on nonref[] (a vreg is provably-non-ref only if ALL definitions
	 * are scalar): start optimistic (all i32 candidates nonref), then knock a candidate down to ref the moment
	 * any defining instruction can produce a pointer; isref = the i32 vregs we could not prove scalar. */
	{
		gboolean *isref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		gboolean *nonref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		gboolean changed = TRUE;
		int pass;
		/* candidates: pointer-sized vregs (i64/f32/f64 can never hold a ref -> never shadow-stacked) */
		for (i = 0; i < nvreg; ++i)
			nonref [i] = (vt [i] == WASM_I32);
		/* vregs mono already knows are object refs OR managed pointers are definitely refs.
		 * With MONO_WASM_JIT_GCMAPS (default on) setting cfg->compute_gc_maps, MINI's own
		 * marking populates both arrays structurally from the type system: create_var_for_vreg
		 * marks ref/byref vars, alloc_ireg_ref/_mp mark ref and interior-pointer TEMPS
		 * (ldelema, ldflda, the stfld write-barrier address, Unsafe.Add/AddByteOffset).
		 * These are the same facts the LLVM/native GC paths consume — the fixpoint below is
		 * just the closure of them over MOVE chains, pointer arithmetic and call returns. */
		for (i = 0; i < nvreg; ++i)
			if (vreg_is_ref (cfg, i) || vreg_is_mp (cfg, i))
				nonref [i] = FALSE;
		/* Explicitly mark reference-typed AND byref ARGUMENTS. mono_compile_create_var marks object-ref
		 * vregs via vreg_is_ref, but it never covers managed POINTERS: (a) a reference-type class's `this`
		 * carries a byref-flagged this_arg, so mini_type_is_reference() returns FALSE for it; (b) byref
		 * params (ref/out, Span<T>) and a valuetype's interior-pointer `this` are managed pointers too. All
		 * of these are otherwise left in a GC-invisible wasm local, so a method that holds one across a GC
		 * point (a residual, an f-slot call, a loop safepoint) dangles when the target object moves. Mark
		 * them here so the prologue copies them into the GC-scanned (conservatively pinning) ref shadow
		 * stack at entry: an object ref keeps the object alive+pinned; a byref pins whatever heap object it
		 * points into (a byref into native/stack memory pins nothing — harmless). */
		for (i = 0; i < nargs; ++i) {
			gboolean arg_is_ref;
			if (i == 0 && sig->hasthis)
				arg_is_ref = TRUE;   /* ref-type this = object ref; valuetype this = interior byref — both need GC tracking */
			else {
				MonoType *pt = sig->params [i - sig->hasthis];
				arg_is_ref = m_type_is_byref (pt) || mini_type_is_reference (pt);   /* byref (ref/out/Span) OR object ref */
			}
			if (arg_is_ref) {
				int av = cfg->args [i]->dreg;
				if (av >= 0 && av < nvreg)
					nonref [av] = FALSE;
			}
			/* scalar args keep their nonref candidate: no defining instruction in the body clears them */
		}
		/* knock candidates down to ref: a vreg is provably non-ref only if EVERY definition is scalar */
		for (pass = 0; changed && pass <= nvreg; ++pass) {
			changed = FALSE;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int d = ins->dreg;
					gboolean def_nonref;
					if (d < 0 || d >= nvreg || !nonref [d])
						continue;   /* not a candidate, or already proven a ref */
					if (ins->opcode == OP_MOVE)
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1]);
					else if ((ins->opcode == OP_IADD || ins->opcode == OP_ISUB)
							&& ins->type != STACK_OBJ && ins->type != STACK_MP)
						/* pointer arithmetic (OP_PADD/OP_PSUB alias these on wasm32): the result of
						 * add/sub is an interior pointer whenever either source might be a ref/mp —
						 * taint-propagate like OP_MOVE instead of trusting a scalar allow-list.
						 * Backstop for interior pointers that carry neither a STACK_MP type nor a
						 * structural mark. Scalar+scalar stays scalar, so ordinary integer math
						 * (and the write-barrier card-mark address computation) costs nothing.
						 * An explicitly OBJ/MP-typed add falls through to wj_opcode_is_nonref,
						 * whose type front-check classifies it ref unconditionally. */
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1])
							&& (ins->sreg2 >= 0 && ins->sreg2 < nvreg && nonref [ins->sreg2]);
					else if ((ins->opcode == OP_IADD_IMM || ins->opcode == OP_ISUB_IMM
							|| ins->opcode == OP_ADD_IMM || ins->opcode == OP_SUB_IMM
							|| ins->opcode == OP_IAND_IMM || ins->opcode == OP_AND_IMM)
							&& ins->type != STACK_OBJ && ins->type != STACK_MP)
						/* ptr+imm (field offset) / ptr&~mask (alignment): pointer-preserving unary shapes */
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1]);
					else if (ins->opcode == OP_CALL || ins->opcode == OP_CALL_REG || ins->opcode == OP_CALL_MEMBASE) {
						/* a call result is a ref unless its return type is a scalar (int/float/native-int/ptr);
						 * byref/object returns must be pinned. OP_VCALL/FCALL/LCALL dregs aren't i32 -> not candidates. */
						MonoCallInst *c = (MonoCallInst *) ins;
						def_nonref = c->signature && !m_type_is_byref (c->signature->ret) && !mini_type_is_reference (c->signature->ret);
					} else
						def_nonref = wj_opcode_is_nonref (ins);
					if (!def_nonref) { nonref [d] = FALSE; changed = TRUE; }
				}
			}
		}
		/* isref = the live pointer-sized vregs we could not prove scalar (these go on the ref shadow stack) */
		for (i = 0; i < nvreg; ++i)
			isref [i] = (vt [i] == WASM_I32) && !nonref [i];
		{
			/* DECISIVE TEST (MONO_WASM_JIT_PINALL): pin every i32 vreg by treating it as a reference, so it
			 * lands on the GC-scanned (conservatively pinning) ref shadow stack instead of a GC-invisible wasm
			 * local. Over-marking a non-ref i32 is harmless. If the random heap corruption STOPS with this on,
			 * a reference vreg was being MISSED by the inference above (left unpinned -> moved by the copying
			 * nursery -> stale -> wild store); if it persists, the corruptor is a wild *base*, not a stale ref. */
			extern int mono_wasm_jit_pinall;
			if (G_UNLIKELY (mono_wasm_jit_pinall)) {
				for (i = 0; i < nvreg; ++i)
					if (vt [i] == WASM_I32)
						isref [i] = TRUE;
			}
			/* PRODUCTION FIX (MONO_WASM_JIT_REFBASES, default on): the prove-non-ref fixpoint trusts mono's
			 * ins->type to spot refs, but that misses heap pointers in shapes it can't see (a movable object
			 * baked as a non-STACK_OBJ iconst, an interior-pointer add mis-typed as scalar, ...), leaving a live
			 * ref in a plain wasm local that dangles after a compacting GC — the intermittent corruption PINALL
			 * confirmed. Close it WITHOUT PINALL's cost: any vreg used as a DEREFERENCE — a MEMBASE load/store
			 * base or a virtual-call receiver — is a live pointer at that instruction, so pin it on the
			 * (conservative) ref shadow stack. Over-marking an unmanaged/addr-frame base is harmless (it pins
			 * nothing); a missed heap base is silent corruption. Only dereferenced pointers move to the shadow
			 * stack — not every i32 temporary. */
			{ extern int mono_wasm_jit_refbases;
			  if (mono_wasm_jit_refbases) {
				MonoBasicBlock *bbf; MonoInst *insf;
				int *dop = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nvreg);
				for (i = 0; i < nvreg; ++i) dop [i] = -1;
				for (bbf = cfg->bb_entry; bbf; bbf = bbf->next_bb)
					MONO_BB_FOR_EACH_INS (bbf, insf)
						if (insf->dreg >= 0 && insf->dreg < nvreg) dop [insf->dreg] = insf->opcode;
				for (bbf = cfg->bb_entry; bbf; bbf = bbf->next_bb) {
					MONO_BB_FOR_EACH_INS (bbf, insf) {
						int b = -1;
						switch (insf->opcode) {
						case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
						case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
						case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
							b = insf->sreg1; break;
						case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
						case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
						case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
							b = insf->dreg; break;
						case OP_CALL_MEMBASE: case OP_VCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_VOIDCALL_MEMBASE:
							b = insf->sreg1; break;
						default: break;
						}
						/* Skip bases proven to be an addr-frame pointer (OP_LDADDR result = addrbase+slot, linear
						 * memory, never a heap ref) — pinning those is pure waste. Everything else that gets
						 * dereferenced could be a heap pointer -> pin it. */
						if (b >= 0 && b < nvreg && vt [b] == WASM_I32 && dop [b] != OP_LDADDR) {
							/* Subsumption audit: with the structural seeds (GCMAPS) + add/sub taint, the
							 * fixpoint should already classify every dereferenced heap pointer — a flip
							 * here is a counterexample. Counted always (cheap), named under REFVERIFY.
							 * A long soak with REFBASES=1 and refbases_extra==0 proves REFBASES is
							 * subsumed and can stay off in production. */
							if (!isref [b]) {
								extern int mono_wasm_jit_refverify;
								if (mono_wasm_jit_stats)
									mono_wasm_jit_count (WJC_REFBASES_EXTRA);
								if (G_UNLIKELY (mono_wasm_jit_refverify)) {
									char *mn = mono_method_get_full_name (cfg->method);
									/* wj_opname, NOT mono_inst_name: under DISABLE_LOGGING (browser Release)
									 * mono_inst_name returns the opcode as an int — %s would wild-read it. */
									printf ("WASM_JIT_REFBASES_EXTRA: %s vreg R%d (def op %s, deref op %s) classified nonref by fixpoint\n",
										mn ? mn : "?", b, dop [b] >= 0 ? wj_opname (dop [b]) : "?", wj_opname (insf->opcode));
									g_free (mn);
								}
								isref [b] = TRUE;
							}
						}
					}
				}
			  } }
			/* REFVERIFY cross-check (debug): the classification must be a superset of the structural
			 * marking — a vreg mono marked ref/mp that the fixpoint left as a plain wasm local is a
			 * lost seed, i.e. exactly the missed-ref shape that corrupts silently. Also flag marked
			 * vregs whose inferred wasm valtype is not pointer-sized (a type-confusion anomaly). */
			{ extern int mono_wasm_jit_refverify;
			  if (G_UNLIKELY (mono_wasm_jit_refverify)) {
				for (i = 0; i < nvreg; ++i) {
					gboolean marked = vreg_is_ref (cfg, i) || vreg_is_mp (cfg, i);
					if (!marked)
						continue;
					if (vt [i] == WASM_I32 && !isref [i]) {
						char *mn = mono_method_get_full_name (cfg->method);
						printf ("WASM_JIT_REFVERIFY: %s vreg R%d marked %s but classified NONREF (lost seed)\n",
							mn ? mn : "?", i, vreg_is_ref (cfg, i) ? "ref" : "mp");
						g_free (mn);
						if (mono_wasm_jit_refverify >= 2)
							g_assert_not_reached ();
					} else if (vt [i] != 0 && vt [i] != WASM_I32) {
						char *mn = mono_method_get_full_name (cfg->method);
						printf ("WASM_JIT_REFVERIFY: %s vreg R%d marked %s but valtype %d (not pointer-sized)\n",
							mn ? mn : "?", i, vreg_is_ref (cfg, i) ? "ref" : "mp", (int) vt [i]);
						g_free (mn);
					}
				}
			  } }
		}
		for (i = 0; i < nvreg; ++i)
			if (li [i] >= 0 && isref [i])
				refslot [i] = nrefslots++;
		/* ref-etype scalar-vtype locals (addrslot==-2 from the ldaddr pre-pass): each gets a GC-scanned
		 * ref-shadow slot so its single ref field is a tracked/pinning root (VTYPE_SCALAR_REF). */
		for (i = 0; i < nvreg; ++i)
			if (addrslot [i] == -2) {
				refslot [i] = nrefslots++;
			}
		if (nrefslots > 0)
			lc.refslot = refslot;
		/* C-stack frame size: ref slots first (4-byte), then the 8-aligned addr slots; whole frame
		 * 16-aligned per the emscripten SP ABI. */
		refbytes_al = (nrefslots * 4 + 7) & ~7;
		framebytes = (refbytes_al + naddrbytes + 15) & ~15;

		/* MISSED-REF FINDER (MONO_WASM_JIT_MISSEDREF=1): PINALL proved the corruptor is a ref left in a plain
		 * wasm local (isref=FALSE) that goes stale after a GC. The dangerous uses are dereferences: a MEMBASE
		 * load/store base, or a virtual-call receiver. Log each such NONREF base + the opcode that DEFINED it,
		 * so the wrong wj_opcode_is_nonref case is nameable. OP_LDADDR bases are addr-frame (non-heap) -> skip.
		 * Bounded across the whole run. */
		{ extern int mono_wasm_jit_missedref;
		  if (G_UNLIKELY (mono_wasm_jit_missedref)) {
			MonoInst **defins = (MonoInst **) mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst *) * nvreg);
			MonoBasicBlock *bbr; MonoInst *insr;
			gsize memsz = 0;
#ifdef HOST_BROWSER
			memsz = wj_memsz ();
#endif
			for (bbr = cfg->bb_entry; bbr; bbr = bbr->next_bb)
				MONO_BB_FOR_EACH_INS (bbr, insr)
					if (insr->dreg >= 0 && insr->dreg < nvreg) defins [insr->dreg] = insr;
			for (bbr = cfg->bb_entry; bbr; bbr = bbr->next_bb) {
				MONO_BB_FOR_EACH_INS (bbr, insr) {
					int base = -1; const char *knd = NULL;
					MonoInst *d; int dop;
					switch (insr->opcode) {
					case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
					case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
					case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
						base = insr->sreg1; knd = "load"; break;
					case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
					case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
					case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
						base = insr->dreg; knd = "store"; break;
					case OP_CALL_MEMBASE: case OP_VCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_VOIDCALL_MEMBASE:
						base = insr->sreg1; knd = "callrecv"; break;
					default: break;
					}
					if (base < 0 || base >= nvreg || vt [base] != WASM_I32 || isref [base])
						continue;
					d = defins [base];
					dop = d ? d->opcode : -1;
					if (dop == OP_LDADDR)
						continue;   /* addr-frame pointer (non-heap) */
					if (dop == OP_ICONST) {
						/* A CONSTANT base is safe ONLY if it's a fixed address (vtable/class/static blob — never
						 * GC-moved). If the constant is a MOVABLE heap object baked directly (should have gone
						 * through the PCONST precise root), the GC moves it -> the baked constant goes stale ->
						 * wild deref = the corruptor. Flag it by probing whether the constant looks like a live
						 * object (val -> vtable -> klass are all plausible in-memory pointers). */
#ifdef HOST_BROWSER
						/* wj_probe_ok, NOT open-coded bounds math: `v + 8 <= memsz` wraps for v near 2^32
						 * (a probed word holding e.g. int -8) and the probe's own deref becomes an OOB trap. */
						gsize v = (gsize) d->inst_c0;
						if (wj_probe_ok (v, memsz)) {
							gsize vtab = *(gsize *) v;
							if (wj_probe_ok (vtab, memsz)) {
								gsize kl = *(gsize *) vtab;
								if (wj_probe_ok (kl, memsz)) {
									static int _mo = 0;
									if (_mo++ < 200)
										printf ("WASM_JIT_MISSED_REF_ICONST_OBJ %s : %s base=%d val=0x%x vtable=0x%x (MOVABLE object baked as iconst -> stale after GC)\n",
											mname, knd, base, (unsigned) v, (unsigned) vtab);
								}
							}
						}
#endif
						continue;   /* fixed-address iconst -> not a missed ref */
					}
					{ static int _mn = 0;
					  if (_mn++ < 400)
						printf ("WASM_JIT_MISSED_REF_BASE %s : %s base=%d def=%s\n", mname, knd, base, dop >= 0 ? wj_opname (dop) : "(arg/undef)"); }
				}
			}
		  } }

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
			{ printf ("WASM_JIT_IR === %s nvreg=%d nrefslots=%d nargs=%d ===\n", cfg->method->name, nvreg, nrefslots, nargs); }
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
					printf ("%s\n", b);
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
/* Raise a corlib exception (by id; see OP_COND_EXC's exc_id map) from inside an `if`, then leave the method
 * — used by the inline OP_LDIV/OP_LREM div-by-zero/overflow guards (wasm32's decompose omits them for the
 * long opcodes). cppeh: the raise C++-throws and never returns (unreachable). resume-state: dummy ret +
 * ref/addr-frame leave + return, mirroring OP_COND_EXC. `ldiv_rti` (the (i32)->void functype index) must be
 * in scope. */
#ifdef HOST_BROWSER
#define LDIV_RAISE_FPTR() do { extern void mono_wasm_jit_raise_corlib (int exc_id); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_raise_corlib); } while (0)
#else
#define LDIV_RAISE_FPTR() wasm_i32_const (&body, 0x7ff8)
#endif
#define LDIV_RAISE(EXC_ID) do { \
		wasm_i32_const (&body, (EXC_ID)); \
		LDIV_RAISE_FPTR (); \
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ldiv_rti); wasm_uleb (&body, 0); \
		wasm_op (&body, WASM_OP_UNREACHABLE);   /* mono_wasm_jit_raise_corlib C++-throws and never returns */ \
	} while (0)
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
/* Pop this method's C-stack frame (restore __stack_pointer to the entry SP). Emit before EVERY
 * return: a popped frame falls below the SP and stops being GC-scanned — no zeroing needed. The
 * stackRestore call consumes only entry_sp + returns void, so it leaves any return value on the
 * stack. No-op for frame-less methods. */
#ifdef HOST_BROWSER
#define EMIT_REF_LEAVE() do { if (framebytes > 0) { \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
		wasm_i32_const (&body, (gint32) (intptr_t) stackRestore); \
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
		{
			MonoBasicBlock *_prev = NULL;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				int _bd = bbidx [bb->block_num];
				gint32 _off = bb->cil_code ? (gint32) (bb->cil_code - cfg->header->code) : -1;
				/* A decompose-synthesized bb carries NO cil_code (il_offset -1). The important case is the
				 * InvalidCastException COND_EXC block that mono_decompose_typechecks splits out of a
				 * castclass/isinst INSIDE a try: with il_offset -1, eh_dispatch's `if (il < 0) return -1`
				 * can't find the enclosing clause, so a catchable exception ESCAPES the in-method landing pad
				 * (and pass-1's island walk mis-targets). Inherit the previous bb's IL offset when both bbs
				 * are in the SAME EH region (mono_replace_ins gives the synthesized block its origin's region),
				 * so the inherited offset really does fall inside the same clause; never inherit across a
				 * region boundary (that could over-catch). */
				if (_off < 0 && _prev && _prev->region == bb->region) {
					int _pd = bbidx [_prev->block_num];
					if (_pd >= 0 && _pd < N)
						_off = eh_table->il_offsets [_pd];
				}
				if (_bd >= 0 && _bd < N)
					eh_table->il_offsets [_bd] = _off;
				_prev = bb;
			}
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

	/* prescan: does this method have any OP_*CALL_MEMBASE (a virtual/interface call = an inline-IC site)?
	 * If so we fetch &wj_slot_live / &wj_slot_live_cap once in the prologue (below) so each IC hit can do the
	 * liveness check INLINE instead of a per-hit mono_wasm_jit_slot_live call_indirect. Leaf methods with no
	 * vcall skip the fetch entirely (no wasted prologue C calls). */
	{
		MonoBasicBlock *_bb;
		for (_bb = cfg->bb_entry; _bb && !has_vcall; _bb = _bb->next_bb) {
			MonoInst *_ins;
			MONO_BB_FOR_EACH_INS (_bb, _ins) {
				switch (_ins->opcode) {
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
				case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
					has_vcall = TRUE; break;
				default: break;
				}
				if (has_vcall) break;
			}
		}
	}

	/* prologue: dispatch index ($blk) starts at the entry block (dense index 0) */
	wasm_i32_const (&body, 0);
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);

#ifdef HOST_BROWSER
	/* C-STACK FRAME PROLOGUE. Reserve this method's ref+addr slots as a real frame on the emscripten
	 * C stack (see the layout doc at the top of this file): capture entry_sp, drop the SP by the
	 * 16-aligned frame size, zero the frame (GC must not scan garbage; .NET locals are zero-init),
	 * copy reference args into their slots, and derive addrbase. Every exit stackRestores entry_sp
	 * (EMIT_REF_LEAVE); the EH landing pad stackRestores refbase (this frame stays live, unwound
	 * callee frames fall below the SP and stop being scanned — no zeroing, no balance bookkeeping).
	 * An EH method with an EMPTY frame still captures entry_sp (refbase = entry_sp) so its landing
	 * pad can pop the frames of callees the C++ unwind tore through. */
	if (framebytes > 0 || eh_on) {
		WasmFuncType gt, lt; int k2;
		memset (&gt, 0, sizeof gt); gt.nparams = 0; gt.ret = WASM_I32;                              /* emscripten_stack_get_current: ()->i32 */
		memset (&lt, 0, sizeof lt); lt.nparams = 1; lt.params [0] = WASM_I32; lt.ret = WASM_VOID;   /* stackRestore: (i32)->void */
		for (k2 = 0; k2 < nextra; ++k2) {
			if (enter_ti < 0 && functype_eq (&extra_types [k2], &gt)) enter_ti = 2 + k2;
			if (leave_ti < 0 && functype_eq (&extra_types [k2], &lt)) leave_ti = 2 + k2;
		}
		if (enter_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = gt; enter_ti = 2 + nextra++; }
		if (leave_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = lt; leave_ti = 2 + nextra++; }
		uses_calls = TRUE;
		/* entry_sp = emscripten_stack_get_current () */
		wasm_i32_const (&body, (gint32) (intptr_t) emscripten_stack_get_current);
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) enter_ti); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx);
		if (framebytes > 0) {
			/* refbase (frame base) = (entry_sp - framebytes) & ~15; __stack_pointer = refbase */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_i32_const (&body, framebytes);
			wasm_op (&body, WASM_OP_I32_SUB);
			wasm_i32_const (&body, -16);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_i32_const (&body, (gint32) (intptr_t) stackRestore);
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) leave_ti); wasm_uleb (&body, 0);
			/* memory.fill (refbase, 0, framebytes): the frame is above the SP now, so the GC scans it —
			 * it must hold no garbage/stale pointers; .NET local zero-init falls out of the same fill */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_i32_const (&body, 0);
			wasm_i32_const (&body, framebytes);
			wasm_u8 (&body, 0xFC); wasm_uleb (&body, 11); wasm_u8 (&body, 0);   /* memory.fill mem 0 (bulk memory) */
			for (i = 0; i < nargs; ++i) {
				int av = cfg->args [i]->dreg;
				if (av >= 0 && av < nvreg && refslot [av] >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);   /* addr */
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) i);             /* incoming arg param */
					wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [av] * 4));
				}
			}
			if (naddrbytes > 0) {
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
				wasm_i32_const (&body, refbytes_al);
				wasm_op (&body, WASM_OP_I32_ADD);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) addrbase_idx);
			}
		} else {
			/* eh_on with an empty frame: refbase = entry_sp is the landing pad's restore target */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
		}
	}
	/* INLINE f-slot-IC liveness prologue: fetch the STABLE per-thread addresses of the wj_slot_live bitmap
	 * pointer + capacity ONCE (2 tiny ()->i32 calls per invocation), caching them in locals. Each vcall IC hit
	 * then does the liveness check inline (load the current bitmap ptr/cap through these addresses + a bit
	 * test) instead of a per-hit mono_wasm_jit_slot_live call_indirect (the profiled ~1.3M/frame boundary).
	 * Only when the method has a vcall site AND the inline IC is on. */
	if (mono_wasm_jit_vcall_inline_ic && has_vcall) {
		WasmFuncType pt; int pti = -1, k3;
		memset (&pt, 0, sizeof pt); pt.nparams = 0; pt.ret = WASM_I32;   /* ()->i32 */
		for (k3 = 0; k3 < nextra; ++k3) if (functype_eq (&extra_types [k3], &pt)) { pti = 2 + k3; break; }
		if (pti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt; pti = 2 + nextra++; }
		uses_calls = TRUE;
#ifdef HOST_BROWSER
		{ extern guint8 **mono_wasm_jit_slot_live_ptr_addr (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_slot_live_ptr_addr); }
#else
		wasm_i32_const (&body, 0x7ff4);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) pti); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) slotlive_ptr_idx);
#ifdef HOST_BROWSER
		{ extern int *mono_wasm_jit_slot_live_cap_addr (void); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_slot_live_cap_addr); }
#else
		wasm_i32_const (&body, 0x7ff5);
#endif
		wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) pti); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) slotlive_cap_idx);
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
		if (eh_type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_type_idx = 2 + nextra++; }
		/* (i32,i32)->i32: mono_wasm_jit_eh_dispatch (table, blk) -> handler bbidx (a finally/fault
		 * handler's bbidx is tagged with WJ_EH_DISPATCH_FINALLY_BIT; the landing pad strips it) or -1 */
		memset (&_t, 0, sizeof _t); _t.nparams = 2; _t.params [0] = WASM_I32; _t.params [1] = WASM_I32; _t.ret = WASM_I32;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_dispatch_ti = 2 + _k; break; }
		if (eh_dispatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_dispatch_ti = 2 + nextra++; }
		/* ()->void: mono_jiterp_end_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_endcatch_ti = 2 + _k; break; }
		if (eh_endcatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_endcatch_ti = 2 + nextra++; }
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
#ifdef HOST_BROWSER
	/* Islands (residual=0): enumerate the full un-JITted-callee blocker set up front so the auto-JIT trigger
	 * forms the whole island in ONE emit cycle (see wj_prescan_blockers). No-op under residual!=0. */
	{ extern int mono_wasm_jit_residual_mode; if (mono_wasm_jit_residual_mode == 0) wj_prescan_blockers (cfg); }
#endif
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int loop_depth = N - 1 - i;
		gboolean terminated = FALSE;
		int cmp_a = -1, cmp_b = -1;
		gint32 cmp_imm = 0;
		gint64 cmp_imm64 = 0;      /* the immediate for an i64 compare-imm (OP_LCOMPARE_IMM); cmp_imm holds the i32 one */
		gboolean cmp_imm_mode = FALSE;
		WasmValtype cmp_float = 0; /* 0 = int compare; WASM_F32/WASM_F64 = float compare (set by OP_(F|R)COMPARE) */
		gboolean cmp_i64 = FALSE;  /* TRUE after OP_LCOMPARE(_IMM): the fused branch/setcc/cond_exc uses i64 compare ops */
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
			/* GC liveness annotations emitted under cfg->compute_gc_maps (set for COMPILE_WASM to get
			 * MINI's structural ref/mp vreg marking). They only feed the precise-GC-map builder, which
			 * is compiled out (mini-gc.c #if 0) — the wasm backend's ref classification reads the
			 * vreg_is_ref/vreg_is_mp arrays directly, so these are pure no-ops here. */
			case OP_GC_LIVENESS_DEF: case OP_GC_LIVENESS_USE:
			case OP_GC_PARAM_SLOT_LIVENESS_DEF: case OP_GC_SPILL_SLOT_LIVENESS_DEF:
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
				/* Re-raise the in-flight exception WITHOUT popping this method's il_state island here. The
				 * re-raise (mono_wasm_jit_endfinally_rethrow -> mono_wasm_jit_rethrow) runs a fresh exception
				 * pass-1 that MUST still see this island, so it (a) stops at THIS method for an enclosing
				 * catch/finally (advancing il_offset past the just-run finally) instead of walking past the
				 * unwinder-invisible JITted frame and running OUTER frames' finally clauses prematurely, and
				 * (b) is re-caught by this method's OWN wasm landing pad. The island is popped EXACTLY ONCE on
				 * the genuine exit: the catch landing pad's h<0 rethrow (leave_island + RETHROW) or the
				 * normal-return EMIT_REF_LEAVE. Popping it here too would double-pop — removing the ENCLOSING
				 * JITted EH caller's island (jit-order-dependent thread-kill / EH corruption). */
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
				if (_gti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _gt; _gti = 2 + nextra++; }
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
					/* GATED (MONO_WASM_JIT_AOTCONST, default ON): baking inst_p0 enables newobj/token-constant
					 * methods to JIT. Only provably-stable, un-movable, cross-thread pointers reach here
					 * (vtable/class/method/static-field-addr/image/method-rgctx); movable GC objects (ldstr/typeof)
					 * go through the precise-root literal table instead, so a GC can't dangle these immediates.
					 * MONO_WASM_JIT_AOTCONST=0 reverts to bailing the method to the interpreter. */
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
					if (pti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt; pti = 2 + nextra++; }
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
			case OP_LDADDR: {
				/* &local: push the local's addressable-frame slot address (addrbase + offset), store to dreg.
				 * The addr pre-pass assigned addrslot for every supported address-taken local; if it didn't
				 * (ref/byref/vtype/arg, or LDADDR disabled), bail the whole method as before. */
				MonoInst *var = (MonoInst *) ins->inst_p0;
				int vv = (var && var->dreg >= 0 && var->dreg < nvreg) ? var->dreg : -1;
				if (vv >= 0 && lc.addrslot && lc.addrslot [vv] == -2) {
					/* ref-etype scalar vtype: address is its GC-scanned ref-shadow slot (refbase + slot*4). */
					if (!lc.refslot || lc.refslot [vv] < 0) { fail = "ldaddr refvt no slot"; fail_op = OP_LDADDR; goto done; }
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					if (lc.refslot [vv] != 0) { wasm_i32_const (&body, lc.refslot [vv] * 4); wasm_op (&body, WASM_OP_I32_ADD); }
					if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
					break;
				}
				if (vv < 0 || !lc.addrslot || lc.addrslot [vv] < 0) { fail = "ldaddr unsupported var"; fail_op = OP_LDADDR; goto done; }
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) addrbase_idx);
				if (lc.addrslot [vv] != 0) { wasm_i32_const (&body, lc.addrslot [vv]); wasm_op (&body, WASM_OP_I32_ADD); }
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
				break;
			}
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
			case OP_LNEG: /* no i64.neg in wasm: 0 - x */
				wasm_i64_const (&body, (gint64) 0);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "lneg sreg"; goto done; }
				wasm_op (&body, WASM_OP_I64_SUB);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lneg dreg"; goto done; }
				break;
			case OP_LADD: BIN (WASM_OP_I64_ADD); break;
			case OP_LSUB: BIN (WASM_OP_I64_SUB); break;
			case OP_LMUL: BIN (WASM_OP_I64_MUL); break;
			/* i64 div/rem. wasm i64.div_s/rem_s TRAP on divide-by-zero and (signed) INT64_MIN/-1 overflow.
			 * Unlike the i32 OP_IDIV family — whose divide-by-zero/overflow OP_COND_EXC checks mono's
			 * decompose emits ahead of the op — the LONG div decompose block is #if TARGET_SIZEOF_VOID_P==8,
			 * so on wasm32 (ptr=4) it is SKIPPED and OP_LDIV reaches us unchecked. Emit the checks inline here
			 * (mirroring decompose.c) so a long divide raises DivideByZeroException / OverflowException
			 * instead of trapping. IMM variants below stay unchecked: decompose only forms them for a known
			 * non-zero (and, signed, non -1) constant divisor. */
			case OP_LDIV: case OP_LDIV_UN: case OP_LREM: case OP_LREM_UN: {
				{ extern int mono_wasm_jit_longdiv; if (!mono_wasm_jit_longdiv) { fail = "long div (disabled)"; fail_op = ins->opcode; goto done; } }
				gboolean lsigned = (ins->opcode == OP_LDIV || ins->opcode == OP_LREM);
				WasmOpcode dop = ins->opcode == OP_LDIV ? WASM_OP_I64_DIV_S
					: ins->opcode == OP_LDIV_UN ? WASM_OP_I64_DIV_U
					: ins->opcode == OP_LREM ? WASM_OP_I64_REM_S : WASM_OP_I64_REM_U;
				WasmFuncType rt; int ldiv_rti = -1, rk;
				{ extern int mono_wasm_jit_cond_exc; if (!mono_wasm_jit_cond_exc) { fail = "ldiv cond_exc disabled (env)"; goto done; } }
				/* (i32)->void functype for mono_wasm_jit_raise_corlib */
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_I32; rt.nparams = 1; rt.ret = WASM_VOID;
				for (rk = 0; rk < nextra; ++rk) if (functype_eq (&extra_types [rk], &rt)) { ldiv_rti = 2 + rk; break; }
				if (ldiv_rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; ldiv_rti = 2 + nextra++; }
				uses_calls = TRUE;
				/* divide-by-zero: if (sreg2 == 0) raise DivideByZeroException (exc_id 1) */
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv b"; goto done; }
				wasm_op (&body, WASM_OP_I64_EQZ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				LDIV_RAISE (1);
				wasm_op (&body, WASM_OP_END);
				if (lsigned) {
					/* overflow: if (sreg1 == INT64_MIN && sreg2 == -1) raise OverflowException (exc_id 0) */
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ldiv a"; goto done; }
					wasm_i64_const (&body, (gint64) G_MININT64);
					wasm_op (&body, WASM_OP_I64_EQ);
					if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv b2"; goto done; }
					wasm_i64_const (&body, (gint64) -1);
					wasm_op (&body, WASM_OP_I64_EQ);
					wasm_op (&body, WASM_OP_I32_AND);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					LDIV_RAISE (0);
					wasm_op (&body, WASM_OP_END);
				}
				/* the checked division */
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv sreg"; goto done; }
				wasm_op (&body, dop);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldiv dreg"; goto done; }
				break;
			}
			case OP_LAND: BIN (WASM_OP_I64_AND); break;
			case OP_LOR: BIN (WASM_OP_I64_OR); break;
			case OP_LXOR: BIN (WASM_OP_I64_XOR); break;
			case OP_LSHL: LSHIFT (WASM_OP_I64_SHL); break;
			case OP_LSHR: LSHIFT (WASM_OP_I64_SHR_S); break;
			case OP_LSHR_UN: LSHIFT (WASM_OP_I64_SHR_U); break;
			case OP_LADD_IMM: BINI64L (WASM_OP_I64_ADD); break;
			case OP_LSUB_IMM: BINI64L (WASM_OP_I64_SUB); break;
			case OP_LMUL_IMM: BINI64L (WASM_OP_I64_MUL); break;
			/* div/rem by a constant long. cprop's strength-reduction normally eliminates a 0 or (signed) -1
			 * constant divisor before the backend, but guard anyway: wasm i64.div_s/rem_s TRAP on a /0 and on
			 * INT64_MIN / -1 — which would crash the worker instead of raising DivideByZero/Overflow. Bail
			 * those (rare) shapes to the interpreter, which raises the correct managed exception. */
			case OP_LDIV_IMM: case OP_LDIV_UN_IMM: case OP_LREM_IMM: case OP_LREM_UN_IMM: {
				gboolean lsigned = (ins->opcode == OP_LDIV_IMM || ins->opcode == OP_LREM_IMM);
				WasmOpcode dop = ins->opcode == OP_LDIV_IMM ? WASM_OP_I64_DIV_S
					: ins->opcode == OP_LDIV_UN_IMM ? WASM_OP_I64_DIV_U
					: ins->opcode == OP_LREM_IMM ? WASM_OP_I64_REM_S : WASM_OP_I64_REM_U;
				if (ins->inst_l == 0 || (lsigned && ins->inst_l == -1)) { fail = "ldiv_imm 0/-1 divisor"; fail_op = ins->opcode; goto done; }
				BINI64L (dop);
				break;
			}
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
			case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:
			case OP_LCONV_TO_I: case OP_LCONV_TO_U: UN (WASM_OP_I32_WRAP_I64); break;  /* i64 -> i32/native-int (truncate) */
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
			case OP_NOT_NULL: case OP_CHECK_THIS: {
				/* Null check: if sreg1 is null, raise a CATCHABLE NullReferenceException, mirroring OP_COND_EXC
				 * — NOT a raw wasm trap. address 0 is valid in wasm linear memory, so a skipped check silently
				 * corrupts low memory; and EH methods now compile (MONO_WASM_JIT_EH), so a null this/deref inside
				 * a try must reach managed EH/finally rather than `unreachable`. Hot path = ld + eqz + not-taken
				 * branch; cold path raises NRE (exc_id 4) then C++-unwinds (cppeh) or bails to interp resume-state. */
				WasmFuncType rt; int rti = -1, ck;
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "nullchk sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_EQZ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_I32; rt.nparams = 1; rt.ret = WASM_VOID;
				for (ck = 0; ck < nextra; ++ck) if (functype_eq (&extra_types [ck], &rt)) { rti = 2 + ck; break; }
				if (rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; rti = 2 + nextra++; }
				uses_calls = TRUE;
				wasm_i32_const (&body, 4);   /* NullReferenceException (see OP_COND_EXC's exc_id map) */
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_raise_corlib (int exc_id); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_raise_corlib); }
#else
				wasm_i32_const (&body, 0x7ff8);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) rti); wasm_uleb (&body, 0);
				wasm_op (&body, WASM_OP_UNREACHABLE);   /* mono_wasm_jit_raise_corlib C++-throws; never returns */
				wasm_op (&body, WASM_OP_END);
				break;
			}
#define LOADM(WOP, AL) do { if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "load addr guard"; goto done; } if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "load base"; goto done; } wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "load dreg"; goto done; } } while (0)
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
#define STOREM(WOP, AL) do { \
		gboolean _og_valref = lc.refslot && ins->sreg1 >= 0 && ins->sreg1 < nvreg && lc.refslot [ins->sreg1] >= 0; \
		gboolean _og_baseref = lc.refslot && ins->dreg >= 0 && ins->dreg < nvreg && lc.refslot [ins->dreg] >= 0; \
		if (G_UNLIKELY (lc.objguard) && (_og_valref || _og_baseref)) { \
			/* OBJGUARD. Two store shapes can scribble random memory via a bad base: \
			 * (a) ref VALUE (sreg1 is a ref) -> base (dreg) is a managed object whose write barrier card-marks \
			 *     (base>>9)+cardtable; validate it is a live heap object (kind 2). \
			 * (b) ref/byref BASE (dreg is a ref/byref) storing a scalar, e.g. `*outparam = x` -> a stale/wild \
			 *     byref base can land on wj_ref_sp itself (the garbage-SP corruption); range/control-var check \
			 *     it (kind 3). Trap at the culprit either way. Prefer kind 2 when the value is a ref. */ \
			int _ogkind = _og_valref ? 2 : 3; \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard base"; goto done; } \
			wasm_i32_const (&body, _ogkind); \
			wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_check_store); \
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) lc.check_ti); wasm_uleb (&body, 0); \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->dreg, (gint32) ins->inst_offset)) { fail = "store addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store base"; goto done; } if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "store val"; goto done; } wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
			case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: STOREM (WASM_OP_I32_STORE, 2); break;
			case OP_STOREI1_MEMBASE_REG: STOREM (WASM_OP_I32_STORE8, 0); break;
			case OP_STOREI2_MEMBASE_REG: STOREM (WASM_OP_I32_STORE16, 1); break;
			case OP_STOREI8_MEMBASE_REG: STOREM (WASM_OP_I64_STORE, 3); break;
			case OP_STORER4_MEMBASE_REG: STOREM (WASM_OP_F32_STORE, 2); break;
			case OP_STORER8_MEMBASE_REG: STOREM (WASM_OP_F64_STORE, 3); break;
#undef STOREM
/* membase store of an immediate: *(dreg[=inst_destbasereg] + inst_offset) = inst_imm. dreg aliases
 * inst_destbasereg (the base); inst_imm is the constant. Sub-word stores truncate via i32.store8/16. */
#define STOREMI(WOP, AL) do { \
		if (G_UNLIKELY (lc.objguard) && lc.refslot && ins->dreg >= 0 && ins->dreg < nvreg && lc.refslot [ins->dreg] >= 0) { \
			/* OBJGUARD: storing an immediate THROUGH a ref/byref base (e.g. `*outparam = 0`); a stale/wild \
			 * byref can land on wj_ref_sp. Range/control-var check the base (kind 3) before the store. */ \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard imm base"; goto done; } \
			wasm_i32_const (&body, 3); \
			wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_check_store); \
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) lc.check_ti); wasm_uleb (&body, 0); \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->dreg, (gint32) ins->inst_offset)) { fail = "store-imm addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store-imm base"; goto done; } wasm_i32_const (&body, (gint32) ins->inst_imm); wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
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
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0; cmp_i64 = FALSE;
				break;
			case OP_LCOMPARE: /* i64 compare, consumed by a following OP_LB<cc> branch / OP_LC<cc> setcc / OP_COND_EXC */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0; cmp_i64 = TRUE;
				break;
			case OP_LCOMPARE_IMM: /* i64 compare vs immediate (a long compare folded against a constant by cprop);
			                       * consumed by a following OP_LB<cc> / OP_LC<cc> / OP_COND_EXC. The 64-bit immediate
			                       * lives in inst_l (data.i8const), NOT inst_imm: on wasm32 cprop stores it via
			                       * ins->inst_l (local-propagation.c), and inst_imm (= data.op[1].const_val) overlaps
			                       * only the HIGH 32 bits of i8const — reading it yields the wrong value (0 for any
			                       * immediate that fits in 32 bits). */
				cmp_a = ins->sreg1; cmp_imm64 = (gint64) ins->inst_l; cmp_imm_mode = TRUE; cmp_float = 0; cmp_i64 = TRUE;
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
				if (cmp_imm_mode) wasm_i64_const (&body, cmp_imm64);   /* OP_LCOMPARE_IMM: imm in inst_l, no cmp_b */
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lcmp b"; goto done; }
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
				if (cmp_imm_mode) wasm_i64_const (&body, cmp_imm64);   /* OP_LCOMPARE_IMM: imm in inst_l, no cmp_b */
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lsetcc b"; goto done; }
				wasm_op (&body, w);   /* i64 compare -> i32 (0/1) */
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lsetcc dreg"; goto done; }
				break;
			}
			case OP_ICOMPARE_IMM: case OP_COMPARE_IMM:
				cmp_a = ins->sreg1; cmp_imm = (gint32) ins->inst_imm; cmp_imm_mode = TRUE; cmp_float = 0; cmp_i64 = FALSE;
				break;
			case OP_FCOMPARE: /* f64 compare, consumed by a following OP_FB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F64; cmp_i64 = FALSE;
				break;
			case OP_RCOMPARE: /* f32 (r4) compare, consumed by a following OP_RB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F32; cmp_i64 = FALSE;
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
				 * + a not-taken branch; cold path calls mono_wasm_jit_raise_corlib, which C++-throws (cppeh) and
				 * never returns (WASM_OP_UNREACHABLE). Reuses the OP_IBcc compare-fusing (cmp_a/cmp_b/cmp_imm).
				 * Integer compares only. */
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
				if (cmp_i64) {
					/* i64 operands (e.g. the OP_LDIV div-by-zero check OP_LCOMPARE_IMM(sreg2,0) -> COND_EXC_IEQ):
					 * use the i64 compare ops; the result is still i32 (0/1) and feeds the same IF below. */
					switch (ins->opcode) {
					case OP_COND_EXC_EQ: case OP_COND_EXC_IEQ: cmp_wop = WASM_OP_I64_EQ; break;
					case OP_COND_EXC_NE_UN: case OP_COND_EXC_INE_UN: cmp_wop = WASM_OP_I64_NE; break;
					case OP_COND_EXC_LT: case OP_COND_EXC_ILT: cmp_wop = WASM_OP_I64_LT_S; break;
					case OP_COND_EXC_LT_UN: case OP_COND_EXC_ILT_UN: cmp_wop = WASM_OP_I64_LT_U; break;
					case OP_COND_EXC_GT: case OP_COND_EXC_IGT: cmp_wop = WASM_OP_I64_GT_S; break;
					case OP_COND_EXC_GT_UN: case OP_COND_EXC_IGT_UN: cmp_wop = WASM_OP_I64_GT_U; break;
					case OP_COND_EXC_LE: case OP_COND_EXC_ILE: cmp_wop = WASM_OP_I64_LE_S; break;
					case OP_COND_EXC_LE_UN: case OP_COND_EXC_ILE_UN: cmp_wop = WASM_OP_I64_LE_U; break;
					case OP_COND_EXC_GE: case OP_COND_EXC_IGE: cmp_wop = WASM_OP_I64_GE_S; break;
					default: cmp_wop = WASM_OP_I64_GE_U; break; /* GE_UN / IGE_UN */
					}
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
				if (rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; rti = 2 + nextra++; }
				uses_calls = TRUE;
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cond_exc a"; goto done; }
				if (cmp_imm_mode) { if (cmp_i64) wasm_i64_const (&body, cmp_imm64); else wasm_i32_const (&body, cmp_imm); }
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
				/* mono_wasm_jit_raise_corlib C++-throws and never returns; unwind natively. */
				wasm_op (&body, WASM_OP_UNREACHABLE);
				wasm_op (&body, WASM_OP_END);   /* close the cond `if` (hot path continues after) */
				break;
			}
			case OP_RETHROW:   /* rethrow: method-to-ir sets sreg1 to the enclosing catch's exvar (the caught
			                    * exception), so re-raise it like throw but via mono_wasm_jit_rethrow, which
			                    * PRESERVES the captured stack trace (a continuation, not a new throw site).
			                    * Only reached under cppeh EH — the EH gate bails clause-bearing methods otherwise. */
			case OP_THROW: {
				/* throw <exc=sreg1>: raise via mono_wasm_jit_throw, which C++-throws (cppeh) and never returns;
				 * the wasm stack unwinds natively to the nearest landing pad (an in-method catch, or the interp
				 * e-thunk boundary). Unconditional bb terminator -> mark terminated. */
				WasmFuncType tt; int tti = -1, tk;
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "throw sreg"; goto done; }
				memset (&tt, 0, sizeof (tt)); tt.params [0] = WASM_I32; tt.nparams = 1; tt.ret = WASM_VOID;
				for (tk = 0; tk < nextra; ++tk) if (functype_eq (&extra_types [tk], &tt)) { tti = 2 + tk; break; }
				if (tti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tt; tti = 2 + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				/* OP_RETHROW preserves the original stack trace (mono_wasm_jit_rethrow); OP_THROW rebuilds it. */
				if (ins->opcode == OP_RETHROW) {
					extern void mono_wasm_jit_rethrow (MonoObject *exc); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_rethrow);
				} else {
					extern void mono_wasm_jit_throw (MonoObject *exc); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_throw);
				}
#else
				wasm_i32_const (&body, 0x7ff7);
#endif
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tti); wasm_uleb (&body, 0);
				/* mono_wasm_jit_throw/rethrow C++-throws (mono_llvm_cpp_throw_exception) and never returns —
				 * the wasm stack unwinds natively to the nearest landing pad (an in-method catch or the interp
				 * e-thunk boundary, which restores the ref shadow-stack SP). */
				wasm_op (&body, WASM_OP_UNREACHABLE);
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
				/* A synchronized callee keeps its Monitor.Enter/Exit in a separate SYNCHRONIZED wrapper; the raw
				 * method body (what mono_interp_get_imethod returns) has no monitor ops, so dispatching it
				 * directly would run unlocked -> a notify/wait throws IllegalMonitorStateException + leaves the
				 * monitor state wrong (the netty DefaultPromise save/quit hang). Substitute the wrapper HERE, at
				 * compile time (GC-safe; the prior bail's "can't create GC-safely" caveat was about the EMITTED
				 * runtime code after the arg spill, not the compiler). call_method is the effective callee for
				 * BOTH the direct f-slot path (the wrapper f-slots once its own body — incl. Monitor.Enter's
				 * bool& via OP_LDADDR — is jittable) AND the interp residual (which then runs the wrapper, with
				 * the monitor). The wrapper has the same signature, so the captured args still line up. Virtual
				 * calls are handled the same way in mono_wasm_jit_vcall_resolve(_fslot) (pre-spill). */
				MonoMethod *call_method = call->method;
				if (call_method && (call_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
					extern int mono_wasm_jit_sync;
					extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
					if (!mono_wasm_jit_sync) { fail = "calls synchronized method (sync disabled)"; goto done; }   /* bisection: revert to bailing the whole caller */
					call_method = mono_marshal_get_synchronized_wrapper (call_method);
				}
#ifdef HOST_BROWSER
				/* Canonicalize the (uncached, per-compile-fresh) synchronized-inner wrapper to a stable
				 * instance so its f-slot is found across re-emits — see wj_canonical_callee. MUST match the
				 * pre-scan, which canonicalizes identically, so the recorded blocker == this f-slot key. */
				call_method = wj_canonical_callee (call_method);
#endif
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
					if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = 2 + nextra++; }
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
					if (pv == 0 || pv == WASM_VOID) {
						/* BYVAL scalar-vtype arg (MONO_WASM_JIT_VTYPE_SCALAR): declare the param as its single-field
						 * etype scalar, matching the AOT callee's LLVMArgWasmVtypeAsScalar ABI; the arg-emit loops
						 * below load that scalar from the ByVal value's addr-frame slot. Ref-free only
						 * (wj_scalar_vtype_valtype); anything else keeps the original bail. */
						WasmValtype sv;
						if (!wj_scalar_vtype_valtype (csig->params [ai], &sv)) { fail = "call arg type"; goto done; }
						pv = sv;
					}
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
				if (call_method == cfg->method) {
					extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
					/* SCC batch: the orchestrator reserved our slot on the imethod -> get_callee_fslot returns
					 * it. Otherwise this is standalone self-recursion: reserve our own e/f-slot pair here
					 * (reusing a recycled pair from a prior bailed self-emit) and bake it; the success path
					 * instantiates INTO these. Table exhausted (alloc -> 0) -> fall through to the not-jitted bail. */
					call_fslot = mono_wasm_jit_get_callee_fslot (call_method);
					if (call_fslot <= 0) {
						if (!wj_self_f_slot) {
							if (wj_recycle_f_slot) { wj_self_e_slot = wj_recycle_e_slot; wj_self_f_slot = wj_recycle_f_slot; wj_recycle_e_slot = 0; wj_recycle_f_slot = 0; }
							else { wj_self_e_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */); wj_self_f_slot = mono_jiterp_allocate_table_entry (1); }
						}
						call_fslot = wj_self_f_slot;
					}
				} else {
					extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
					call_fslot = mono_wasm_jit_get_callee_fslot (call_method);
				}
#else
				call_fslot = 0x7fff; /* offline dump: placeholder slot for encoder validation */
#endif
				if (call_fslot > 0) {
					/* Callee is wasm-JITted: direct call_indirect through its f-slot (Phase 2). */
					for (k = 0; k < nextra; ++k)
						if (functype_eq (&extra_types [k], &ct)) { type_idx = 2 + k; break; }
					if (type_idx < 0) {
						if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
						extra_types [nextra] = ct;
						type_idx = 2 + nextra++;
					}
					uses_calls = TRUE;
					/* arg source vregs captured at method-to-ir time (calls.c), not call->args
					 * (which gets corrupted by later vreg passes) */
					if (!call->call_info) { fail = "no captured call args"; goto done; }
#ifdef HOST_BROWSER
					/* SYNC-ON-CALL: ensure the callee's module is instantiated in THIS thread's per-thread
					 * function table BEFORE the direct call_indirect. The slot may still hold the jiterpreter
					 * placeholder on a worker that ran this (self-compiled/island-run) caller without fully
					 * syncing the callee -> call_indirect of a placeholder is a signature-mismatch trap. The
					 * helper no-ops (one branch) once the slot is live; on a miss it lazily instantiates the
					 * SPECIFIC callee module (robust vs the sync watermark) and returns, keeping the fast
					 * direct path (no interp residual). emitted: ensure_fslot(call_method, call_fslot). */
					{
						extern void mono_wasm_jit_ensure_fslot (MonoMethod *callee, int fslot);
						WasmFuncType et; int eti = -1;
						memset (&et, 0, sizeof (et)); et.params [0] = WASM_I32; et.params [1] = WASM_I32; et.nparams = 2; et.ret = WASM_VOID;
						for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &et)) { eti = 2 + k; break; }
						if (eti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = et; eti = 2 + nextra++; }
						wasm_i32_const (&body, (gint32) (intptr_t) call_method);
						wasm_i32_const (&body, call_fslot);
						wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_ensure_fslot);
						wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) eti); wasm_uleb (&body, 0);
					}
#endif
					{
						int *wargs = (int *) call->call_info;
						for (ai = 0; ai < (int) ct.nparams; ++ai)
							if (!wj_emit_one_call_arg (&body, &lc, csig, wargs, ai)) { fail = "call arg ld"; goto done; }
					}
					wasm_i32_const (&body, call_fslot);
					wasm_op (&body, WASM_OP_CALL_INDIRECT);
					wasm_uleb (&body, (guint32) type_idx);
					wasm_uleb (&body, 0); /* table 0 (imported f.f) */
					if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
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
						extern gboolean mono_wasm_jit_aot_call_target (MonoMethod *m, gpointer *addr, gpointer *rgctx, gboolean *has_extra_arg);
						gpointer aot_addr = NULL, aot_rgctx = NULL;
						gboolean aot_has_extra = TRUE;   /* does the raw AOT body carry the trailing rgctx/dummy arg? */
						/* Gate: byref args/ret bail (interp_entry handles those). ALSO bail two cases this inline
						 * path cannot marshal, to the residual (which is correct for both):
						 *  - call->rgctx_reg: a generic-shared callee needs the CALLSITE runtime generic context, but
						 *    aot_call_target only returns the callee-side extra arg/rgctx used by the llvm_only direct
						 *    call ABI. Reusing that value here would feed the callee the wrong context (bad GOT lookups /
						 *    null-fn trap). The comment at the RGCTX flag promised this skip; enforce it here (the vcall
						 *    path already bails rgctx at the membase site). - call->need_unbox_trampoline: a boxed-valuetype receiver (object/interface target)
						 *    needs `this` unboxed (+sizeof(MonoObject)); aot_call_target hands back the RAW body with no
						 *    unbox tagging, so the inline path would pass the boxed header pointer -> field reads off by
						 *    the header. The residual/interp preserves unbox semantics. */
						gboolean aot_ok = mono_wasm_jit_inline_aot && !m_type_is_byref (csig->ret)
							&& !call->rgctx_reg && !call->need_unbox_trampoline;
						for (k = 0; k < (int) csig->param_count && aot_ok; ++k)
							if (m_type_is_byref (csig->params [k])) aot_ok = FALSE;
						if (aot_ok && mono_wasm_jit_aot_call_target (call_method, &aot_addr, &aot_rgctx, &aot_has_extra)) {
							WasmFuncType nt = ct;   /* native functype = (this?, args by value) [+ i32 rgctx if aot_has_extra] -> ct.ret */
							int nti = -1;
							/* Append the trailing extra (rgctx/dummy) arg ONLY when the body actually has it. Exempt
							 * wrapper kinds (alloc/castclass/icall/etc.) have a bare (this,args)->ret body — appending
							 * would declare one param too many -> call_indirect signature-mismatch trap. */
							if (aot_has_extra) {
								if (nt.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "aot inline nparams"; goto done; }
								nt.params [nt.nparams++] = WASM_I32;
							}
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { nti = 2 + k; break; }
							if (nti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = nt; nti = 2 + nextra++; }
							uses_calls = TRUE;
							if (!call->call_info) { fail = "no captured call args"; goto done; }
							{
								/* AOT-style EH (cppeh, the only model now): bare direct AOT call. A throwing AOT
								 * callee C++-unwinds (wasm-EH) straight through this JITted frame to the nearest
								 * landing pad — the interp e-thunk boundary (mono_llvm_catch_exception) or an
								 * in-method catch. The call is pure call_indirect (the per-call perf win); no
								 * try/catch, no per-call pending-exception check. */
								wj_emit_fast_count (&body, WJC_FAST_INLINE_AOT);   /* profile: INLINE_AOT direct dispatch */
								{ int *wargs = (int *) call->call_info; for (ai = 0; ai < (int) ct.nparams; ++ai) if (!wj_emit_one_call_arg (&body, &lc, csig, wargs, ai)) { fail = "call arg ld"; goto done; } }
								if (aot_has_extra) wasm_i32_const (&body, (gint32) (intptr_t) aot_rgctx);   /* trailing rgctx/dummy — only if the body has it */
								wasm_i32_const (&body, (gint32) (intptr_t) aot_addr);
								wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) nti); wasm_uleb (&body, 0);
								if (ct.ret != WASM_VOID) {
									if (ct.ret == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret);   /* raw AOT body: dirty upper bits */
									if (!wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
								}
							}
							{ extern int mono_wasm_jit_verbose; static int n_il = 0; if (mono_wasm_jit_verbose >= 2 && n_il++ < 128) { char *cn = mono_method_get_full_name (call->method); printf ("WASM_JIT_INLINE_AOT[%d] %s -> %s\n", n_il, mname, cn); g_free (cn); } }
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
						int rm = mono_wasm_jit_residual_mode;
						/* record the blocking callee on the cfg result so the trigger can eagerly form the island */
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
							if (!(mono_wasm_jit_aot_residual && mono_interp_jit_call_supported (call_method, csig))) {
								/* Lever B (MONO_WASM_JIT_RESIDUAL_PERM): if the blocker is PERMANENTLY un-JITtable
								 * (slot==-1: EH/opcode/sig — it will NEVER get an f-slot, so the island can never
								 * close around it), route just this edge through the interp residual instead of
								 * bailing the whole caller. Keeps a hot island JITted around a cold perm-blocker.
								 * A not-yet-jitted callee still bails (the island should pull it in bottom-up). */
								extern int mono_wasm_jit_residual_perm;
								extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
								if (!(mono_wasm_jit_residual_perm && mono_wasm_jit_callee_perm_unjittable (call_method))) {
									/* Lever B' (MONO_WASM_JIT_RESIDUAL_COLD): the blocker is genuinely COLD — the
									 * island cold-gate would refuse to pull it in (still counting hits, below
									 * thresh/cold_div, not block-promoted). A cold branch (IKVM __<GetInstance>
									 * lambda factory, one-shot init, error path) reached rarely from this hot caller.
									 * Route just this edge through the interp residual so the hot method JITs, paying
									 * ~1 transition per cold call rather than bailing the whole (hot) method. Hot/parked
									 * callees are NOT cold -> they still bail here and the island force-JITs them, so no
									 * per-iteration residual storm on the hot path (the user's transition-cost constraint). */
									extern int mono_wasm_jit_residual_cold;
									extern int mono_wasm_jit_callee_too_cold (MonoMethod *m);
									if (!(mono_wasm_jit_residual_cold && mono_wasm_jit_callee_too_cold (call_method))) {
										/* The residual=0 pre-scan (wj_prescan_blockers) already enumerated the full
										 * blocker set; record this one too (deduped) so the list head is correct even
										 * if the pre-scan predicate ever drifts from this site. */
										wj_result_add_blocker (&cfg->wasm_jit_result, call_method);
										fail = "callee not jitted (residual off)";
										goto done;
									}
									{ extern int mono_wasm_jit_verbose; static int n_cl = 0; if (mono_wasm_jit_verbose >= 2 && n_cl++ < 128) { char *cn = mono_method_get_full_name (call_method); printf ("WASM_JIT_RESIDUAL_COLD[%d] %s -> %s\n", n_cl, mname, cn); g_free (cn); } }
									/* else: cold-leaf blocker + RESIDUAL_COLD -> fall through, emit interp residual */
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
					if (mono_wasm_jit_verbose >= 3 && csig->param_count > 0 && ct.ret != WASM_VOID) {
						char *cn = mono_method_get_full_name (call->method);
						printf ("WASM_JIT_RESIDUAL_PNV %s -> %s\n", mname, cn ? cn : "?");
						g_free (cn);
					}
					memset (&ts, 0, sizeof (ts)); ts.ret = WASM_I32; ts.nparams = 0;
					memset (&ti, 0, sizeof (ti)); ti.ret = WASM_I32; ti.nparams = 2; ti.params [0] = WASM_I32; ti.params [1] = WASM_I32;
					for (k = 0; k < nextra; ++k) {
						if (tsi < 0 && functype_eq (&extra_types [k], &ts)) tsi = 2 + k;
						if (tii < 0 && functype_eq (&extra_types [k], &ti)) tii = 2 + k;
					}
					if (tsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ts; tsi = 2 + nextra++; }
					if (tii < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ti; tii = 2 + nextra++; }
					uses_calls = TRUE;
					/* scratch-reentrancy: pre-transform the callee BEFORE spilling its args into the
					 * GC-invisible scratch (mirrors the vcall path's mono_wasm_jit_vcall_resolve). The
					 * transform can run the callee's class cctor — arbitrary managed code that allocates
					 * (-> GC, which would move ref args already spilled as raw pointers into the non-GC-scanned
					 * scratch) and can itself re-enter another residual on this thread (clobbering the still-
					 * unread spilled args). Doing it now — while the ref args still live in the GC-scanned ref
					 * shadow stack and the scratch is free — closes both windows; call_interp then finds the
					 * imethod already transformed and only does the GC-free marshal. (i32)->void. */
					{
						extern void mono_wasm_jit_pretransform (MonoMethod *method);
						WasmFuncType tp; int tpi = -1, tpk;
						memset (&tp, 0, sizeof (tp)); tp.params [0] = WASM_I32; tp.nparams = 1; tp.ret = WASM_VOID;
						for (tpk = 0; tpk < nextra; ++tpk) if (functype_eq (&extra_types [tpk], &tp)) { tpi = 2 + tpk; break; }
						if (tpi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tp; tpi = 2 + nextra++; }
						wasm_i32_const (&body, (gint32) (intptr_t) call_method);
						wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_pretransform);
						wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tpi); wasm_uleb (&body, 0);
					}
					/* $scratch = mono_wasm_jit_scratch() */
					wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_scratch);
					wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) tsi); wasm_uleb (&body, 0);
					wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
					{
						int *wargs = (int *) call->call_info;
						for (ai = 0; ai < (int) ct.nparams; ++ai) {
							WasmOpcode sop; int al;
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wj_emit_one_call_arg (&body, &lc, csig, wargs, ai)) { fail = "residual arg ld"; goto done; }
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
					/* mono_wasm_jit_call_interp(method, $scratch): bake call_method (the synchronized wrapper for a
					 * synchronized callee) so the interp runs it WITH the monitor; plain callees pass through. */
					wasm_i32_const (&body, (gint32) (intptr_t) call_method);
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
						WasmFuncType vts, vtrf, vtd, ftd; int vtsi = -1, vtrfi = -1, vtdi = -1, ftdi = -1, vk, ai, n2, this_vr; int aic_ati = -1, aic_ati_ne = -1;
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
						if (vtsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vts; vtsi = 2 + nextra++; }
						memset (&vtrf, 0, sizeof (vtrf)); vtrf.params [0] = WASM_I32; vtrf.params [1] = WASM_I32; vtrf.params [2] = WASM_I32; vtrf.params [3] = WASM_I32; vtrf.nparams = 4; vtrf.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtrf)) { vtrfi = 2 + vk; break; }
						if (vtrfi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtrf; vtrfi = 2 + nextra++; }
						memset (&vtd, 0, sizeof (vtd)); vtd.params [0] = WASM_I32; vtd.params [1] = WASM_I32; vtd.nparams = 2; vtd.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtd)) { vtdi = 2 + vk; break; }
						if (vtdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtd; vtdi = 2 + nextra++; }
						memset (&ftd, 0, sizeof (ftd)); for (vk = 0; vk < npp; ++vk) ftd.params [vk] = pp [vk]; ftd.nparams = (guint32) npp; ftd.ret = rv;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &ftd)) { ftdi = 2 + vk; break; }
						if (ftdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ftd; ftdi = 2 + nextra++; }
						uses_calls = TRUE;
						n2 = csig->param_count + 1; /* this + params */
						/* per-call-site AOT-vcall IC cell (VCALL_AOT_IC): 20B, see mono_wasm_jit_alloc_aot_ic */
						gpointer aic = NULL;
						{ extern int mono_wasm_jit_vcall_aot_ic, mono_wasm_jit_vcall_inline_ic, mono_wasm_jit_vcall_aot;
						  if (mono_wasm_jit_vcall_aot_ic && mono_wasm_jit_vcall_inline_ic && mono_wasm_jit_vcall_aot) {
#ifdef HOST_BROWSER
							extern gpointer mono_wasm_jit_alloc_aot_ic (void); aic = mono_wasm_jit_alloc_aot_ic ();
#else
							aic = (gpointer) (intptr_t) 0x7fe0;
#endif
						  } }
						if (aic) {
							/* register AOT-body functypes up front for the inline AOT-IC: at (this,args,rgctx)->rv, at_ne (this,args)->rv */
							WasmFuncType at, at_ne;
							if (n2 + 1 <= WASM_FUNCTYPE_MAX_PARAMS) {
								memset (&at, 0, sizeof (at)); for (vk = 0; vk < n2; ++vk) at.params [vk] = pp [vk]; at.params [n2] = WASM_I32; at.nparams = (guint32) (n2 + 1); at.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { aic_ati = 2 + vk; break; }
								if (aic_ati < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at; aic_ati = 2 + nextra++; }
								memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { aic_ati_ne = 2 + vk; break; }
								if (aic_ati_ne < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at_ne; aic_ati_ne = 2 + nextra++; }
							}
							if (aic_ati < 0 || aic_ati_ne < 0) aic = NULL;   /* functype table full -> skip AOT-IC (safe) */
						}
						/* per-call-site inline cache (8 bytes: [i32 vtable, i32 InterpMethod*]) in shared memory;
						 * resolve_fslot caches the virtual resolve here, skipping it on a monomorphic hit. */
#ifdef HOST_BROWSER
						{ extern gpointer mono_wasm_jit_alloc_ic (void); vic = mono_wasm_jit_alloc_ic (); }
#else
						vic = (gpointer) (intptr_t) 0x7ff0;
#endif
						/* RECEIVER NULL CHECK — materialize the implicit null check a callvirt relies on. On real
						 * hardware the vtable load from a null `this` faults; wasm linear-memory address 0 is a
						 * valid address, so the load silently reads garbage. The interp does this explicitly too
						 * (MINT_CALLVIRT_FAST: NULL_CHECK(this_arg) before resolving). Without it a null receiver
						 * flows into mono_wasm_jit_vcall_resolve_fslot, where mono_object_get_virtual_method_internal
						 * returns NULL and the NULL override is dereferenced -> mono_marshal_get_synchronized_wrapper(NULL)
						 * aborts (marshal.c g_assert(method)). On null, raise a catchable NullReferenceException
						 * (exc_id 4) and bail, exactly like OP_COND_EXC. */
						{
							WasmFuncType nrt; int nrti = -1, nck;
							memset (&nrt, 0, sizeof (nrt)); nrt.params [0] = WASM_I32; nrt.nparams = 1; nrt.ret = WASM_VOID;
							for (nck = 0; nck < nextra; ++nck) if (functype_eq (&extra_types [nck], &nrt)) { nrti = 2 + nck; break; }
							if (nrti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = nrt; nrti = 2 + nextra++; }
							uses_calls = TRUE;
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall nullchk this"; goto done; }
							wasm_op (&body, WASM_OP_I32_EQZ);
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							wasm_i32_const (&body, 4);   /* NullReferenceException */
#ifdef HOST_BROWSER
							{ extern void mono_wasm_jit_raise_corlib (int exc_id); wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_raise_corlib); }
#else
							wasm_i32_const (&body, 0x7ff8);
#endif
							wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) nrti); wasm_uleb (&body, 0);
							/* mono_wasm_jit_raise_corlib C++-throws and never returns; unwind natively. */
							wasm_op (&body, WASM_OP_UNREACHABLE);
							wasm_op (&body, WASM_OP_END);
						}
						/* --- INLINE MONOMORPHIC IC FAST PATH (skip the resolve_fslot C helper on a hit) ---
						 * ~97.8% of MC vcalls hit the IC; each otherwise pays TWO C calls (scratch() + resolve_fslot:
						 * atomic load + resolve/checks + sync_thread) before the real call_indirect — the profiled
						 * #1 game-thread cost (vcall_resolve_fslot ~17%). Do the hit inline in wasm:
						 *   vtab = *this; ic = atomic_load(&vic);
						 *   if (vtab == (i32)ic) { im = (i32)(ic>>32); fslot = im->wasm_jit_fslot;
						 *     if (fslot != 0 && mono_wasm_jit_slot_live(fslot)) { call_indirect(fslot); skip slow } }
						 * The liveness check is essential + is what makes inline dispatch MT-safe: the IC is shared
						 * but the function table is PER-THREAD, so a slot another thread cached may be absent on THIS
						 * thread. The ORIGINAL inline check (table[fslot] != null via ref.is_null) was WRONG — the
						 * per-thread table grows with a NON-null jiterpreter placeholder, so it passed for un-instantiated
						 * slots -> signature-mismatch trap (jit138). Fixed below: gate on the authoritative per-thread
						 * bitmap via one cheap mono_wasm_jit_slot_live() call (wasm has no funcref equality to compare the
						 * slot against the placeholder inline). Env-gated (MONO_WASM_JIT_VCALL_INLINE_IC=1) for A/B. */
						if (mono_wasm_jit_vcall_inline_ic) {
#ifdef HOST_BROWSER
							extern int mono_wasm_jit_imethod_fslot_off (void);
							int fslot_off = mono_wasm_jit_imethod_fslot_off ();
#else
							int fslot_off = 0x40; /* placeholder for offline encoder validation (real offset only matters at runtime) */
#endif
							int way;
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $after (void) */
							/* OBJGUARD (once, hoisted above the N ways): validate the receiver before any raw
							 * this->vtable load. Otherwise a type-confused scalar/garbage receiver traps as a bare
							 * wasm OOB before the C resolver can print anything. */
#ifdef HOST_BROWSER
							if (G_UNLIKELY (lc.objguard && lc.check_ti >= 0)) {
								extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this guard ld"; goto done; }
								wasm_i32_const (&body, 5);
								wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_check_store);
								wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) lc.check_ti); wasm_uleb (&body, 0);
							}
#endif
							/* N-WAY inline IC (MONO_WASM_JIT_VCALL_WAYS, default 1 = monomorphic). One BLOCK per cached
							 * (vtable -> f-slot) entry: a guard failure br 0's to the next way, a hit br 1's to $after.
							 * A 2-way IC captures the ~63% of miss traffic that are 2-type sites (arity depth-1) which a
							 * 1-way IC gets 0% of (it thrashes). After all ways: the AOT-IC + resolve_fslot slow path. */
							for (way = 0; way < mono_wasm_jit_vcall_ways; ++way) {
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /*   $way_fail (void) */
							/* vtab = *(this + 0) */
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this ld"; goto done; }
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
							/* ic = i64.atomic.load(&vic[way]); $ic = ic; if ((i32)ic != vtab) -> $way_fail */
							wasm_i32_const (&body, (gint32) (intptr_t) vic + 8 * way);
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
							/* liveness: if (!slot_live(fslot)) -> $do_slow. The function table is PER-THREAD, so a slot
							 * can hold the jiterpreter placeholder (NON-null, (i32,i32,i32,i32)->void) on a thread that
							 * hasn't instantiated the callee -> call_indirect would signature-mismatch trap (jit138). The
							 * authoritative per-thread signal is the wj_slot_live bitmap. INLINE it (no per-hit C call —
							 * the profiled ~1.3M/frame boundary): read the CURRENT bitmap ptr + cap through the prologue-
							 * cached &wj_slot_live / &wj_slot_live_cap (stable per-thread addresses; a realloc-on-grow moves
							 * the values, not their addresses, so this stays correct with zero stale-pointer window):
							 *   live = (fslot < cap) ? ((bitmap[fslot>>3] >> (fslot&7)) & 1) : 0;   if (!live) -> $do_slow
							 * The cap gate short-circuits the bitmap load, so cap==0 / bitmap==NULL (never instantiated on
							 * this thread) safely takes the slow path with no OOB/NULL read. fslot!=0 is already ensured by
							 * the EQZ check above. Falls back to the C call only if the prologue fetch didn't run (should
							 * not happen: has_vcall gates both), guarded by slotlive_ptr_idx being a valid local. */
							{
								/* fslot < cap ? */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) slotlive_cap_idx);
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);        /* cap = *(&wj_slot_live_cap) */
								wasm_op (&body, WASM_OP_I32_LT_U);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);    /* if (fslot<cap) { bit } else { 0 } */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) slotlive_ptr_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);    /* bitmap = *(&wj_slot_live) */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHR_U);
									wasm_op (&body, WASM_OP_I32_ADD);                                 /* &bitmap[fslot>>3] */
									wasm_op (&body, WASM_OP_I32_LOAD8_U); wasm_memarg (&body, 0, 0);  /* byte */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_i32_const (&body, 7); wasm_op (&body, WASM_OP_I32_AND);
									wasm_op (&body, WASM_OP_I32_SHR_U);                               /* byte >> (fslot&7) */
									wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);      /* bit */
								wasm_op (&body, WASM_OP_ELSE);
									wasm_i32_const (&body, 0);
								wasm_op (&body, WASM_OP_END);
								wasm_op (&body, WASM_OP_I32_EQZ);
								wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* !live -> $do_slow */
							}
							wj_emit_fast_count (&body, WJC_FAST_VIC);   /* profile: inline f-slot IC hit (JIT->JIT) */
							/* FAST: push this+args (fresh from vregs), fslot, call_indirect(ftd); store; skip slow */
							for (ai = 0; ai < n2; ++ai)
								if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "ic fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ftdi); wasm_uleb (&body, 0);
							if (rv != WASM_VOID) { if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ic fast dreg"; goto done; } }
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> $after (skip the C-helper slow path) */
							wasm_op (&body, WASM_OP_END);                            /* end $way_fail */
							}   /* for each way -> fall through to the AOT-IC / resolve_fslot slow path */
							/* --- INLINE AOT-VCALL IC (VCALL_AOT_IC), MT-safe: two atomic i64 words, each vtab-tagged. A hit needs
							 * BOTH words' low32 == this->vtable; then ti/kind (ic1.hi) and rgctx (ic2.hi) are correct for this
							 * vtable regardless of interleaved fills (a vtable maps to ONE target -> identical values). Only
							 * atomic i64 loads (no plain-vs-atomic ordering, which wasm's memory model does not provide). */
							if (aic) {
								/* N-WAY AOT-IC (MONO_WASM_JIT_VCALL_AOT_WAYS): one BLOCK per cached AOT entry (two vtab-tagged
								 * i64 at aic+16*way). A hit needs BOTH words' low32 == vtab (already tear-safe: a torn 2-word
								 * entry fails the both-match check -> miss, never a wrong dispatch, so N-way LRU/fill is MT-safe
								 * for free). 2 ways capture the AOT 2-type sites whose loser vtable was stuck reaching the helper
								 * behind the 1-entry first-wins cache. */
								int aic_way;
								for (aic_way = 0; aic_way < mono_wasm_jit_vcall_aot_ways; ++aic_way) {
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_way ($after now depth 1) */
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "aot ic this ld"; goto done; }
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);   /* v = *this */
								wasm_i32_const (&body, (gint32) (intptr_t) aic + 16 * aic_way); wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 0); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);   /* a = ic1[way] (reuse vc_ic_idx i64) */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* a.vtab != v -> $aot_way (next) */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);   /* ti_kind = a>>32 */
								wasm_i32_const (&body, (gint32) (intptr_t) aic + 16 * aic_way); wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 8); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);   /* b = ic2[way] */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* b.vtab != v -> $aot_way (next) */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_rgctx_idx);   /* rgctx = b>>32 */
								wj_emit_fast_count (&body, WJC_FAST_AOTIC);   /* profile: inline AOT-IC hit (JIT->AOT) */
								/* Collapsed kind branch: the args are identical in both arms — only the trailing rgctx push
								 * and the call_indirect functype differ — so load args ONCE here and let a TYPED if-block
								 * (params = aic_ati_ne = (this,args)->rv, guaranteed valid: aic==NULL otherwise) carry them into
								 * whichever arm runs. Halves the emitted arg-load sequence per way — the aic-only code bloat
								 * that made widening the AOT-IC a net loss (vs the lean f-slot vic). Needs multi-value blocks. */
								for (ai = 0; ai < n2; ++ai) if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "aot ic arg ld"; goto done; }
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);   /* kind2bit selector */
								wasm_op (&body, WASM_OP_IF); wasm_sleb (&body, (gint64) aic_ati_ne);   /* typed block in: (this,args) out: rv */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);   /* ti */
									wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) aic_ati_ne); wasm_uleb (&body, 0);
								wasm_op (&body, WASM_OP_ELSE);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_rgctx_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);
									wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) aic_ati); wasm_uleb (&body, 0);
								wasm_op (&body, WASM_OP_END);   /* end kind if/else */
								if (rv != WASM_VOID) { if (rv == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "aot ic dreg"; goto done; } }
								wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* hit -> $after */
								wasm_op (&body, WASM_OP_END);   /* end $aot_way */
								}   /* for each AOT way -> fall through to resolve_fslot slow path */
							}
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
							/* Fast AOT-vcall (MONO_WASM_JIT_VCALL_AOT, gated, default off): if the resolved override
							 * (target@200, stashed by resolve_fslot) is AOT-backed, call its AOT body DIRECTLY with the
							 * native (this,args,rgctx) ABI — skipping the residual's double marshalling + do_jit_call
							 * frame. Wrapped in a $no_aot block: if the helper says "not AOT" we br to $no_aot and fall
							 * into the (shared, unchanged) residual below; if AOT we call + br past the residual. */
							{
								extern int mono_wasm_jit_vcall_aot;
								if (mono_wasm_jit_vcall_aot) {
									extern int mono_wasm_jit_vcall_aot_target (guint8 *scratch);
									WasmFuncType at, at_ne, aott; int ati = -1, ati_ne = -1, aotti = -1;
									if (n2 + 1 > WASM_FUNCTYPE_MAX_PARAMS) { fail = "vcall aot nparams"; goto done; }
									/* aott (i32)->i32 = the resolve helper, now returning 0=residual / 1=AOT+rgctx / 2=AOT,no-extra-arg.
									 * at (this,params,i32 rgctx)->rv = AOT body WITH the trailing extra (rgctx/dummy) arg; at_ne
									 * (this,params)->rv = AOT body of an exempt wrapper kind WITHOUT it. The override is resolved at
									 * RUNTIME but the call_indirect functype is baked here, so emit BOTH variants and pick by the
									 * helper's return code (kind==2 -> at_ne). Keeps every AOT-backed vcall fast (no residual). */
									memset (&aott, 0, sizeof (aott)); aott.params [0] = WASM_I32; aott.nparams = 1; aott.ret = WASM_I32;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &aott)) { aotti = 2 + vk; break; }
									if (aotti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = aott; aotti = 2 + nextra++; }
									memset (&at, 0, sizeof (at)); for (vk = 0; vk < n2; ++vk) at.params [vk] = pp [vk]; at.params [n2] = WASM_I32; at.nparams = (guint32) (n2 + 1); at.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { ati = 2 + vk; break; }
									if (ati < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at; ati = 2 + nextra++; }
									memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { ati_ne = 2 + vk; break; }
									if (ati_ne < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at_ne; ati_ne = 2 + nextra++; }
									uses_calls = TRUE;
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $no_aot */
									/* kind = mono_wasm_jit_vcall_aot_target(scratch); if (kind==0) br $no_aot (-> residual) */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
#ifdef HOST_BROWSER
									wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_vcall_aot_target);
#else
									wasm_i32_const (&body, 0x7ff9);
#endif
									wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) aotti); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_aotkind_idx);   /* stash kind, keep on stack */
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* kind==0 -> $no_aot -> residual */
									if (aic) {   /* FILL the FIRST EMPTY of the N AOT-IC entries (each two i64: ic1=vtab|((ti<<1|kind2)<<32), ic2=vtab|(rgctx<<32)) */
										/* first-empty-win, no eviction: on a MISS the receiver vtab is (was) in no entry, so filling any
										 * empty slot with it can't dup. A 2-type AOT site fills slot0 with the winner, then slot1 with the
										 * loser -> both hit thereafter (fixes the 1-entry first-wins thrash). br out after the first fill so
										 * one miss fills exactly one slot (else the winner would fill every empty slot -> no room for the loser). */
										int aic_way;
										wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_filled */
										for (aic_way = 0; aic_way < mono_wasm_jit_vcall_aot_ways; ++aic_way) {
										wasm_i32_const (&body, (gint32) (intptr_t) aic + 16 * aic_way); wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 0); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op (&body, WASM_OP_I32_EQZ);
										wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);   /* if (ic1[way].key == 0) */
										wasm_i32_const (&body, (gint32) (intptr_t) aic + 16 * aic_way);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "aot ic fill v1"; goto done; } wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); wasm_op (&body, WASM_OP_I64_EXTEND_I32_U);   /* vtab low32 */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHL);   /* ti<<1 */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx); wasm_i32_const (&body, 2); wasm_op (&body, WASM_OP_I32_EQ); wasm_op (&body, WASM_OP_I32_OR);   /* | kind2bit */
										wasm_op (&body, WASM_OP_I64_EXTEND_I32_U); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHL); wasm_op (&body, WASM_OP_I64_OR);
										wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x18); wasm_memarg (&body, 3, 0);   /* i64.atomic.store ic1[way] */
										wasm_i32_const (&body, (gint32) (intptr_t) aic + 16 * aic_way);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "aot ic fill v2"; goto done; } wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); wasm_op (&body, WASM_OP_I64_EXTEND_I32_U);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 216); wasm_op (&body, WASM_OP_I64_EXTEND_I32_U); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHL); wasm_op (&body, WASM_OP_I64_OR);
										wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x18); wasm_memarg (&body, 3, 8);   /* i64.atomic.store ic2[way] */
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* filled one slot -> exit $aot_filled (skip remaining ways) */
										wasm_op (&body, WASM_OP_END);   /* end if-empty(way) */
										}   /* for each AOT way */
										wasm_op (&body, WASM_OP_END);   /* end $aot_filled */
									}
									/* cppeh: bare AOT call — a throwing callee C++-unwinds natively to the nearest landing
									 * pad (an in-method catch or the interp e-thunk boundary). No try/catch wrapper. */
									/* Collapsed kind branch (mirror of the inline AOT-IC): args identical in both arms, so load
									 * them ONCE and pick the variant with a TYPED if-block (params = ati_ne = (this,args)->rv,
									 * guaranteed valid — bailed above otherwise). Only the rgctx push + functype differ. */
									for (ai = 0; ai < n2; ++ai)
										if (!wasm_ld (&body, &lc, ((int *) call->call_info) [ai])) { fail = "vcall aot arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2);
									wasm_op (&body, WASM_OP_I32_EQ);
									wasm_op (&body, WASM_OP_IF); wasm_sleb (&body, (gint64) ati_ne);   /* typed block in: (this,args) out: rv */
										/* no-extra-arg variant: AOT addr@212; call_indirect (this,args)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ati_ne); wasm_uleb (&body, 0);
									wasm_op (&body, WASM_OP_ELSE);
										/* with-rgctx variant: rgctx@216, AOT addr@212; call_indirect (this,args,rgctx)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 216);   /* rgctx (ftndesc.arg) */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) ati); wasm_uleb (&body, 0);
									wasm_op (&body, WASM_OP_END);   /* end variant if/else */
									if (rv != WASM_VOID) {
										if (rv == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret);   /* raw AOT body: dirty upper bits */
										if (!wasm_st (&body, &lc, ins->dreg)) { fail = "vcall aot dreg"; goto done; }
									}
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* AOT done -> exit fslot-IF, skip residual */
									wasm_op (&body, WASM_OP_END);   /* end $no_aot */
								}
							}
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
							/* call_interp returns 1 if the vcall fallback threw. Abort immediately instead of
							 * reading the stale scratch result; mirrors the direct residual path exactly. */
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
								switch (ret_vt) {
								case WASM_I32: wasm_i32_const (&body, 0); break;
								case WASM_I64: wasm_i64_const (&body, 0); break;
								case WASM_F32: wasm_f32_const (&body, 0); break;
								case WASM_F64: wasm_f64_const (&body, 0); break;
								default: break;
								}
								EMIT_REF_LEAVE ();
								wasm_op (&body, WASM_OP_RETURN);
							wasm_op (&body, WASM_OP_END);
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
								/* Normalize sub-word integer returns exactly like the direct residual (interp_entry
								 * writes bool/i1/u1 as 1 byte and char/i2/u2 as 2 bytes; the full-width i32.load above
								 * reads stale high bytes from the scratch). Without this a vcall fallback returning a
								 * narrow signed/bool value gives the JITted caller wrong high bits/sign -> wrong managed
								 * behaviour (a plausible jit137-style NPE). */
								if (rv == WASM_I32) {
									MonoType *rt = mini_get_underlying_type (csig->ret);
									switch (rt->type) {
									case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: wasm_i32_const (&body, 0xff); wasm_op (&body, WASM_OP_I32_AND); break;
									case MONO_TYPE_I1: wasm_op (&body, WASM_OP_I32_EXTEND8_S); break;
									case MONO_TYPE_CHAR: case MONO_TYPE_U2: wasm_i32_const (&body, 0xffff); wasm_op (&body, WASM_OP_I32_AND); break;
									case MONO_TYPE_I2: wasm_op (&body, WASM_OP_I32_EXTEND16_S); break;
									default: break;
									}
								}
								if (!wasm_st (&body, &lc, ins->dreg)) { fail = "vcall dreg"; goto done; }
							}
						wasm_op (&body, WASM_OP_END);   /* end the fslot if/else (slow path) */
						if (mono_wasm_jit_vcall_inline_ic)
							wasm_op (&body, WASM_OP_END);   /* end $after (only emitted when the inline-IC fast path is on) */
						break;
					}
				/* Raw indirect call (callreg / non-virtual membase): the target in sreg1 is a runtime
				 * value (e.g. ftndesc.addr) which, under auto-JIT (non-llvmonly), isn't a reliable wasm
				 * table index — so bail the method to the interpreter. The VIRTUAL subcase above is NOT
				 * gated: it dispatches via the inline IC + interp-entry-thunk fallback, which always
				 * resolves to a callable slot. */
				{
					if (cfg->wasm_jit_forced) { fail = "raw indirect call under auto-JIT"; goto done; }
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
				if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = 2 + nextra++; }
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
		/* REF-SP-1: restore __stack_pointer to THIS method's frame base before re-dispatching into the
		 * handler. A C++/wasm-EH unwind into this catch skipped the EMIT_REF_LEAVE of every JITted frame
		 * it tore through (nested non-EH callees, and EH callees that escaped via their own h<0 rethrow),
		 * potentially leaving the SP below this frame. stackRestore(refbase) pops exactly those callee
		 * frames — they fall below the SP and stop being GC-scanned (no zeroing needed) — while this
		 * method's OWN frame [refbase, entry_sp) stays live for the handler. Idempotent across repeated
		 * catches (e.g. a finally that re-raises and is re-caught). Unconditional: an eh_on method always
		 * captures refbase in the prologue (== entry_sp for an empty frame). leave_ti is registered for
		 * eh_on. */
#ifdef HOST_BROWSER
		{
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_i32_const (&body, (gint32) (intptr_t) stackRestore);
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, (guint32) leave_ti); wasm_uleb (&body, 0);
		}
#endif
		/* milestone 2c: mark the EXCEPTION path (finally_ind = -1, so OP_ENDFINALLY re-raises) ONLY when
		 * the matched handler IS a finally/fault — eh_dispatch tags those with WJ_EH_DISPATCH_FINALLY_BIT.
		 * The old unconditional set clobbered the OP_CALL_HANDLER continuation whenever a CATCH matched
		 * while a NORMAL-path finally body was executing (`finally { try { close(); } catch { } }`, the
		 * common Java/IKVM cleanup shape): after the catch swallowed, that finally's OP_ENDFINALLY read
		 * -1 and took the re-raise path — popping an UNRELATED outer finally's saved exception (cross-
		 * frame exception theft + gchandle double-free) or synthesizing a spurious EEE on a normal leave.
		 * After setting the marker, strip the tag so $blk receives the plain handler bbidx. Finally
		 * methods only (a catch-only method's dispatch never tags, and its pad has no finally_ind use). */
		if (eh_has_finally) {
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
			wasm_i32_const (&body, WJ_EH_DISPATCH_FINALLY_BIT);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);    /* tagged: finally/fault handler */
			wasm_i32_const (&body, -1);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) finally_ind_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
			wasm_i32_const (&body, ~WJ_EH_DISPATCH_FINALLY_BIT);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) eh_h_idx);
			wasm_op (&body, WASM_OP_END);                          /* end if (untagged catch: finally_ind untouched) */
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
		if (mono_wasm_jit_names)
			wasm_module_append_name_section (&out, mname, mname);
		wasm_buf_free (&ethunk);
	}

#ifdef HOST_BROWSER
	if (g_getenv ("MONO_WASM_JIT_DUMP_ONLY") != NULL) {
		/* debug: forward the emitted module as hex to the UI console but DON'T register it, so the
		 * method runs in the interpreter — lets us inspect JIT-path codegen without executing it. */
		GString *hex = g_string_sized_new (out.len * 2 + 1);
		for (i = 0; i < (int) out.len; ++i)
			g_string_append_printf (hex, "%02x", out.data [i]);
		{ printf ("WASM_JIT_HEX %s : %u : %s\n", mname, out.len, hex->str); }
		g_string_free (hex, TRUE);
	} else if (g_getenv ("MONO_WASM_JIT_DUMP_EXIT") == NULL) {
		/* live runtime: allocate the (global) table slots + cache the emitted module bytes. The wasm
		 * function table is PER-THREAD for dynamically-added entries, so instantiation is deferred to
		 * the interp's first invoke on each thread (interp.c MINT_CALL → mono_wasm_jit_instantiate_local),
		 * which instantiates its own WebAssembly.Instance into its own table[e_slot]/[f_slot]. */
		/* Slot selection: (1) an SCC batch reservation on our imethod (multi-method cycle — the orchestrator
		 * reserved these and publishes us); (2) a standalone self-recursion reservation made in this emit;
		 * (3) fresh allocation. Instantiate INTO the reserved pair so baked self/cross-cycle calls resolve. */
		int e_slot = 0, f_slot = 0;
		{ extern int mono_wasm_jit_self_reserved (MonoMethod *m, int *e_out, int *f_out);
		  if (!mono_wasm_jit_self_reserved (cfg->method, &e_slot, &f_slot)) {
			e_slot = wj_self_f_slot ? wj_self_e_slot : mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			f_slot = wj_self_f_slot ? wj_self_f_slot : mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
		  } }
		if (e_slot > 0 && f_slot > 0) {
			void *cached = g_malloc ((gsize) out.len);
			char ierr [192]; ierr [0] = 0;
			memcpy (cached, out.data, out.len);
			/* Validate by instantiating once on THIS thread (also populates this thread's table).
			 * If the module is invalid (a codegen bug for some opcode shape), bail to the interpreter
			 * here rather than letting another thread trap on a placeholder slot at invoke time. */
			if (mono_wasm_jit_instantiate_local (e_slot, f_slot, cached, (int) out.len, ierr, (int) sizeof (ierr), &wj_inst_ms)) {
				/* The compile result is written onto cfg->wasm_jit_result (below). It's per-compile, so a
				 * re-entrant nested compile (cctors / AOT-target init) has its OWN cfg and can't clobber it —
				 * no per-thread-relay ordering dance needed. We still write the e_slot gate AFTER register +
				 * sync_thread so it's only set once this method's callees are guaranteed live on this thread. */
				if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_REGISTERED); mono_wasm_jit_add (WJC_BYTES_GENERATED, (gint64) out.len); }
				{ extern void mono_wasm_jit_register (int e_slot, int f_slot, void *bytes, int len); mono_wasm_jit_register (e_slot, f_slot, cached, (int) out.len); }
				if (mono_wasm_jit_verbose >= 1) { printf ("WASM_JIT_REGISTERED %s e_slot=%d f_slot=%d len=%u\n", mname, e_slot, f_slot, (unsigned) out.len); }
				/* The self-instantiate above populated ONLY this method's slots on this thread. But an
				 * island/eager compile can run this freshly-compiled method on THIS SAME thread without going
				 * back through the interp invoke path (interp.c MINT_CALL), which is where a thread otherwise
				 * picks up the per-thread function-table sync. This method's baked direct JIT->JIT f-slot calls
				 * (which have no slot_live guard) then target callee slots this thread may never have
				 * instantiated -> the slot still holds the jiterpreter placeholder (mono_jiterp_placeholder_jit_call,
				 * (i32,i32,i32,i32)->void) -> a call_indirect signature-mismatch trap. Bring this thread fully
				 * current now: every callee is registered before its caller, so syncing the whole prefix here
				 * guarantees all of this method's direct f-slot callees are live before it can run. No-op fast
				 * path when already current. */
				mono_wasm_jit_sync_thread ();
				/* Write the success result onto cfg (read back by mono_wasm_force_compile after the compile
				 * returns, on this same thread). e_slot is the >0 success gate the readers test; no cross-thread
				 * barrier needed here — the result is consumed on this thread via cfg, and the cross-thread
				 * publish to InterpMethod.wasm_jit_slot keeps its own barrier in wasm_jit_compile_publish. */
				cfg->wasm_jit_result.bytes = cached;
				cfg->wasm_jit_result.bytes_len = (int) out.len;
				cfg->wasm_jit_result.f_slot = f_slot;
				cfg->wasm_jit_result.e_slot = e_slot;
			} else {
				g_free (cached);
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_INVALID);
				if (mono_wasm_jit_verbose >= 1) { printf ("WASM_JIT_INVALID %s e_slot=%d len=%u : %s\n", mname, e_slot, (unsigned) out.len, ierr); }
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
	cfg->wasm_jit_result.retriable = (fail && strstr (fail, "callee not jitted")) ? 1 : 0;
	/* Categorize the bail reason for the weighted vcall-residual breakdown (stored on InterpMethod by
	 * compile_publish only when the bail is permanent). Opcode bails carry the opcode number so the bench
	 * can name the dominant blocker (e.g. ldaddr 331); EH/sig/other get sentinel negatives. */
	if (!fail) cfg->wasm_jit_result.bail = 0;
	else if (fail_op == OP_LDADDR) cfg->wasm_jit_result.bail = -5;         /* ldaddr (needs addressable locals) */
	else if (fail_op == OP_LCOMPARE) cfg->wasm_jit_result.bail = -6;       /* lcompare */
	else if (fail_op >= 0) cfg->wasm_jit_result.bail = fail_op;            /* some other unsupported opcode (>0) */
	else if (strstr (fail, "EH clauses")) cfg->wasm_jit_result.bail = -2;
	else if (strstr (fail, "arg type") || strstr (fail, "ret type")) cfg->wasm_jit_result.bail = -3;   /* arg/ret sig type */
	/* Split what used to be the -4 "other" catch-all so the vcall-perm breakdown is actionable (the bench
	 * showed -4 was 99% of the perm vcall residual). Order: most specific class first. */
	else if (strstr (fail, "byref")) cfg->wasm_jit_result.bail = -7;                                   /* byref arg/ret */
	else if (strstr (fail, "rgctx") || strstr (fail, "gshared")) cfg->wasm_jit_result.bail = -8;        /* generic-shared / rgctx */
	else if (strstr (fail, "synchronized")) cfg->wasm_jit_result.bail = -9;                             /* synchronized method/wrapper */
	else if (strstr (fail, "EH") || strstr (fail, "finally") || strstr (fail, "eh ") || strstr (fail, "eh-")) cfg->wasm_jit_result.bail = -10; /* other EH reasons (not the -2 clause gate) */
	else cfg->wasm_jit_result.bail = -4;                                   /* genuinely other (unsupported IR shape: reg/move/sig/indirect/...) */
	/* No on-fail clear needed: the success fields (e_slot/f_slot/bytes) are only written on the
	 * instantiate-success path above, and this cfg is private to this compile — a nested re-entrant
	 * compile has its own cfg and cannot have set them here (the old per-thread-relay clobber is gone). */
	/* Reserved a self-slot pair but didn't instantiate into it (bail / invalid module) -> hand it to the
	 * recycle so the next self-recursive emit reuses it instead of leaking the append-only table entry. */
	if (wj_self_f_slot && cfg->wasm_jit_result.f_slot != wj_self_f_slot && !wj_recycle_f_slot) {
		wj_recycle_e_slot = wj_self_e_slot;
		wj_recycle_f_slot = wj_self_f_slot;
	}
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
		if (mono_wasm_jit_verbose >= 2) {
			/* Name the FIRST recorded blocker + the blocker count. A retriable "callee not jitted" bail with
			 * nblk=0 is the smoking gun for the retry storm: compile_publish can't block_note anything, so
			 * force_island returns BUSY -> SLOT_RETRY -> re-attempt every threshold (never parks). */
			int _nblk = cfg->wasm_jit_result.nblockers;
			char *_cn = _nblk > 0 ? mono_method_get_full_name (cfg->wasm_jit_result.blockers [0]) : NULL;
			printf ("WASM_JIT_BAIL %s : %s (op=%d %s) [nblk=%d%s%s]\n", mname, fail, fail_op,
				fail_op >= 0 ? wj_opname (fail_op) : "-", _nblk,
				_cn ? " blocker=" : "", _cn ? _cn : "");
			g_free (_cn);
		}
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

/*
 * mono_wasm_emit_call:
 *
 *   COMPILE_WASM analog of mono_llvm_emit_call, invoked from mono_emit_call_args in place of
 * mono_arch_emit_call. The emitter reads call args positionally from a plain int array stored on
 * the (otherwise unused for wasm) call->call_info.
 *
 * Legacy mode (MONO_WASM_JIT_OUTARG=0, default): snapshot call->args[i]->dreg raw. Nothing in the
 * IR uses those vregs, so any opt pass that runs later (copyprop/deadce/...) corrupts them — this
 * is why mini_method_compile hard-resets cfg->opt for wasm.
 *
 * Structural mode (OUTARG=1): clone LLVM's mechanism — emit a real OP_*MOVE per scalar/ref/byref
 * arg into a fresh vreg, add it to cfg->cbb, and register the dreg in call->out_ireg_args via
 * mono_call_inst_add_outarg_reg. The moves are ordinary instructions (copyprop/deadce/SSA see the
 * dependency), mono_local_deadce explicitly marks out_ireg_args vregs used, and alias analysis
 * special-cases them (kill_call_arg_alias) — so the captured vregs survive the opt pipeline.
 * The side array then holds the MOVE dregs, and the emitter is unchanged. A ref arg's move dreg is
 * classified ref by the fixpoint's OP_MOVE taint (extra shadow slot per ref arg per call site —
 * the cost of rooting the arg at the call).
 *
 * Vtype args (the VTYPE_SCALAR machinery) get NO move (OP_VMOVE would need vtype plumbing and
 * OP_LLVM_OUTARG_VT has no non-LLVM decompose case): the original dreg is recorded in the side
 * array AND registered in out_ireg_args so DEADCE keeps its def alive. Worst case under opts is a
 * clean "call arg ld" bail, never corruption.
 */
void
mono_wasm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	extern int mono_wasm_jit_outarg;
	MonoMethodSignature *sig = call->signature;
	int n = sig->param_count + sig->hasthis, i;
	int *wargs = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (n > 0 ? n : 1));

	if (!mono_wasm_jit_outarg) {
		for (i = 0; i < n; ++i)
			wargs [i] = call->args [i]->dreg;
		call->call_info = (CallInfo *) wargs;
		return;
	}

	for (i = 0; i < n; ++i) {
		MonoInst *in = call->args [i];
		MonoInst *ins;
		MonoType *t = (sig->hasthis && i == 0) ? mono_get_int_type () : sig->params [i - sig->hasthis];
		guint32 opcode = mono_type_to_regmove (cfg, t);

		if (opcode == OP_VMOVE || opcode == OP_XMOVE) {
			/* vtype: no move — record + register the original dreg (keeps the def live) */
			wargs [i] = in->dreg;
			mono_call_inst_add_outarg_reg (cfg, call, in->dreg, 0, FALSE);
			continue;
		}
		MONO_INST_NEW (cfg, ins, opcode);
		if (opcode == OP_FMOVE || opcode == OP_RMOVE)
			ins->dreg = mono_alloc_freg (cfg);
		else if (opcode == OP_LMOVE)
			ins->dreg = mono_alloc_lreg (cfg);
		else
			ins->dreg = mono_alloc_ireg (cfg);
		ins->sreg1 = in->dreg;
		MONO_ADD_INS (cfg->cbb, ins);
		/* always the ireg list (hreg 0), even for f/l moves — positional decode, exactly like
		 * mono_llvm_emit_call; deadce walks both lists anyway */
		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, 0, FALSE);
		wargs [i] = ins->dreg;
	}
	call->call_info = (CallInfo *) wargs;
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
