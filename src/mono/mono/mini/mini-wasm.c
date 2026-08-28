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
#ifdef HOST_BROWSER
#include <emscripten.h>
int mono_jiterp_allocate_table_entry (int type); /* interp/jiterpreter.c */
gpointer mono_interp_get_imethod (MonoMethod *method); /* interp/interp.c; kept opaque in this emitter */
gboolean mono_wasm_jit_vprof_predict (gpointer caller, MonoMethod *base, MonoVTable **out_vt,
	MonoMethod **out_target, guint32 *out_samples); /* interp.c; lock-free pre-JIT receiver profile */
void mono_jiterp_wasm_jit_patch_interp_entry (void *imethod); /* jiterpreter-interp-entry.ts */
void mono_jiterp_wasm_jit_unpatch_interp_entry (void *imethod); /* jiterpreter-interp-entry.ts */
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
int mono_wasm_jit_island = 1;   /* eager transitive island-JIT; MONO_WASM_JIT_ISLAND=0 = old bottom-up retry only */
/* Automatic hotness trigger (Phase 5): when mono_wasm_jit_auto>0, the interp (MINT_CALL) counts
 * calls to each callee and force-compiles it to wasm once its hit count reaches mono_wasm_jit_thresh,
 * instead of requiring the method to be named in MONO_WASM_JIT_METHOD. -1 = uninitialized. */
int mono_wasm_jit_auto = -1;
int mono_wasm_jit_thresh = 2000;
/* MONO_WASM_JIT_OVER_AOT=1: let the runtime wasm method-JIT compete with an already-available AOT
 * body. The interpreter still retains code_type=COMPILED and therefore falls back to the original AOT
 * entry if emission, publication, or per-thread admission fails. Default off: AOT methods are marked
 * permanently ineligible at their hotness threshold, preserving the normal AOT-first policy. */
int mono_wasm_jit_over_aot = 0;
int mono_wasm_jit_arity = 0;      /* MONO_WASM_JIT_ARITY=1: per-call-site receiver-arity histogram for the vcall miss population (N-way IC capture curve). Diagnostic — perturbs timing (like PROFILE_FAST); default off */
/* MONO_WASM_JIT_DEVIRT_PROFILE=1: record the receiver vtable seen at interp virtual call sites.
 * Collection lives in interp.c (wj_vprof_*), observable via mono_wasm_jit_vprof_stat. Defined HERE,
 * not in interp.c, because mono_wasm_jit_auto_init below references it and this file is linked into
 * both the runtime and the offline cross-compiler (mono-aot-cross), which has no interpreter — same
 * reason as mono_wasm_jit_residual_mode. Costs ~0.7% when on. Default off.
 *
 * The first broad consumer added a guarded direct-call diamond in front of the complete PIC/AOT
 * lowering at every profiled callvirt. That duplicated both paths and measured as a 1.5x regression.
 * The adaptive slim consumer below is materially different: for a perfectly-monomorphic site whose
 * target already has an admitted f-slot, it emits only one checked target plus the signature-shared
 * cold miss. There is no duplicated PIC/AOT diamond. Terminal forwarding calls additionally retain
 * bottom-up island blocking so their predicted target can be published before re-emission.
 *
 * The guard is still necessary for correctness if feedback becomes stale. In batch mode its stable
 * call_indirect also gives V8 same-module target feedback; outside batch mode it still replaces a much
 * larger worker-PIC hit path. The profile remains batching's observed virtual call graph too. */
int mono_wasm_jit_devirt_profile = 0;
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
/* MONO_WASM_JIT_CI_IMPORTS: convert the two constant-index call_indirect sites the Round 121 helper-import
 * work did not cover -- the runtime JIT-icall and the residual throw continuation -- into declared imports.
 *
 * Same mechanism and the same soundness argument as MONO_WASM_JIT_HELPER_IMPORTS: the target is a C
 * function in dotnet.native.wasm at a table index fixed for the process, so an import binds once at
 * instantiation and stays correct. Default 0 = the form these two sites have always had, which is what lets
 * the relocatable-emitter refactor be checked as structurally inert.
 *
 * Worth measuring on its own: R129's whole-tier census put constant-index call_indirect to a helper at
 * 76,281 sites over 18 distinct indices, of which index 403815 alone carried 32.3% of the traffic. An
 * import is ~5 x86 against call_indirect's ~15. */
int mono_wasm_jit_ci_imports = 0;
int mono_wasm_jit_direct_import = 0;
/* MONO_WASM_JIT_DEVIRT_IMPORT is GONE, and deliberately not replaced.
 *
 * It used to import the target of a predicted-devirt HIT specifically, as a separate lever from the
 * ordinary direct call, and measured 2.6% of the targeted sites -- 1,732 conversions, ~0.34% of all call
 * sites, an order of magnitude under the ~5% measurement floor. The separation only existed because the two
 * sites emitted different code. They no longer do: a guarded devirt hit leaves the same WASM_RELOC_CALL as
 * any other direct call, so DIRECT_IMPORT covers both and there is nothing left for a second knob to gate. */
/* MONO_WASM_JIT_BATCH_MODULE=1: emit an SCC batch's members into ONE WebAssembly.Module instead of one
 * module each. V8 never inlines across a module boundary but inlines freely within one, so co-location
 * alone takes an accessor-sized call from ~1.5ns to the ~0.25ns no-call floor (scratchpad/callbench.mjs).
 * Mode 1 is the automatic graph batcher; modes 2/3 remain diagnostic controls. */
int mono_wasm_jit_batch_module = 0;
/* MONO_WASM_JIT_BATCH_ALL_AT: with BATCH_MODULE=3, the registered-method count at which the whole
 * program is folded into a single module (the co-location upper-bound experiment). */
int mono_wasm_jit_batch_all_at = 250;
/* BATCH_MODULE=1 is the shipping automatic batcher.  It waits for the discovered method/edge graph
 * to settle, then greedily co-locates its hot connected components.  These are planner bounds rather
 * than a trigger count: registration/edge stability decides WHEN to plan. */
int mono_wasm_jit_batch_settle = 128;
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
/* MONO_WASM_JIT_LAZY_GCP: how many effective GC points a method may have and still defer its GC ref frame
 * until the first one is reached. 1 = the historical behaviour and the MEASURED BEST; <= 0 = no limit.
 * See the gate for the A/B: relaxing it costs 19.3% p50 and triples world generation. */
int mono_wasm_jit_lazy_gcp = 1;
int mono_wasm_jit_batch_inline = 0;
int mono_wasm_jit_batch_min = 16;
int mono_wasm_jit_batch_max = 384;
int mono_wasm_jit_batch_bytes = 786432;
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
int mono_wasm_jit_vcall_shared_miss_enabled = 1; /* MONO_WASM_JIT_VCALL_SHARED_MISS: one signature-neutral cold miss stub using a lazy GC-pinned worker frame */
int mono_wasm_jit_vcall_slim = 1; /* MONO_WASM_JIT_VCALL_SLIM: replace a perfectly-monomorphic profiled site's whole PIC/AOT diamond with one guarded admitted f-slot call + the shared cold miss. */
int mono_wasm_jit_structured_cfg = 1; /* MONO_WASM_JIT_STRUCTURED_CFG: elide the dispatch br_table for verified forward CFGs and single-entry natural loops; irregular/nested shapes retain the universal dispatcher. */

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
	{ extern const char *mono_wasm_jit_watch; const char *w = g_getenv ("MONO_WASM_JIT_WATCH"); mono_wasm_jit_watch = (w && *w) ? g_strdup (w) : NULL; }
	{ extern int mono_wasm_jit_names; const char *nm = g_getenv ("MONO_WASM_JIT_NAMES"); mono_wasm_jit_names = (nm && *nm && *nm != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_helper_imports; const char *hi = g_getenv ("MONO_WASM_JIT_HELPER_IMPORTS"); mono_wasm_jit_helper_imports = (hi && *hi) ? ((*hi != '0') ? 1 : 0) : 1; }
	{ extern int mono_wasm_jit_inline_zero; const char *iz = g_getenv ("MONO_WASM_JIT_INLINE_ZERO"); mono_wasm_jit_inline_zero = (iz && *iz) ? atoi (iz) : 64; }
	{ extern int mono_wasm_jit_inline_ilofs; const char *io = g_getenv ("MONO_WASM_JIT_INLINE_ILOFS"); mono_wasm_jit_inline_ilofs = (io && *io) ? ((*io != '0') ? 1 : 0) : 0; }
	{ extern int mono_wasm_jit_max_himp; const char *mh = g_getenv ("MONO_WASM_JIT_MAX_HIMP"); int v = (mh && *mh) ? atoi (mh) : 192; mono_wasm_jit_max_himp = (v > 0 && v <= 512 /* == WJ_MAX_HELPER_IMPORTS, defined further down; keep in step */) ? v : 192; }
	{ extern int mono_wasm_jit_aot_imports; const char *ai2 = g_getenv ("MONO_WASM_JIT_AOT_IMPORTS"); mono_wasm_jit_aot_imports = (ai2 && *ai2) ? ((*ai2 != '0') ? 1 : 0) : 1; } /* inline-AOT direct call as a function import rather than a constant-index call_indirect */
	{ extern int mono_wasm_jit_ic_autosize; const char *ia = g_getenv ("MONO_WASM_JIT_IC_AUTOSIZE"); mono_wasm_jit_ic_autosize = (ia && *ia) ? ((*ia != '0') ? 1 : 0) : 1; }
	{ extern int mono_wasm_jit_eslot_residual; const char *er = g_getenv ("MONO_WASM_JIT_ESLOT_RESIDUAL"); mono_wasm_jit_eslot_residual = (er && *er) ? ((*er != '0') ? 1 : 0) : 1; }
	{ extern int mono_wasm_jit_delegate_local_pic; const char *dp = g_getenv ("MONO_WASM_JIT_DELEGATE_LOCAL_PIC"); mono_wasm_jit_delegate_local_pic = (dp && *dp) ? ((*dp != '0') ? 1 : 0) : 1; } /* 1 = worker-local delegate recipe PIC (no seqlock, no liveness probe, cached admitted fslot); 0 = the shared WjDelegateIC path. Both are emitted, so this is the A/B arm selector for the largest dispatch class. */
	{ const char *ec = g_getenv ("MONO_WASM_JIT_ENTRYCENSUS"); mono_wasm_jit_entry_census = (ec && *ec && *ec != '0') ? 1 : 0; } /* 1 = per-worker instantiated-vs-entered census; adds a load+test to the interp->JIT boundary, so off while timing */
	{ extern int mono_wasm_jit_elidediag; const char *ed = g_getenv ("MONO_WASM_JIT_ELIDEDIAG"); mono_wasm_jit_elidediag = (ed && *ed && *ed != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_lmf_publish_diag; const char *lp = g_getenv ("MONO_WASM_JIT_LMF_PUBLISH_DIAG"); mono_wasm_jit_lmf_publish_diag = (lp && *lp && *lp != '0') ? 1 : 0; } /* 1 = mono_set_lmf reports publishing an LMF head whose lmf_addr is 0 (an incomplete push); diagnostic only */ /* 1 = print per-method per-arm ref-slot elision attribution; diagnostic only */
	{ extern int mono_wasm_jit_guard_keep_slotlive; const char *gk = g_getenv ("MONO_WASM_JIT_GUARD_KEEP_SLOTLIVE"); mono_wasm_jit_guard_keep_slotlive = (gk && *gk && *gk != '0') ? 1 : 0; } /* 1 = keep elision on under STOREGUARD/OBJGUARD (partial guard coverage, real configuration) */
	{ extern int mono_wasm_jit_residual_mode; const char *r = g_getenv ("MONO_WASM_JIT_RESIDUAL"); mono_wasm_jit_residual_mode = (r && *r) ? atoi (r) : 1; }
	{ extern int mono_wasm_jit_arity; const char *ar = g_getenv ("MONO_WASM_JIT_ARITY"); mono_wasm_jit_arity = (ar && *ar && *ar != '0') ? 1 : 0; } /* 1 = record per-call-site receiver-arity histogram (vcall miss population); diagnostic, perturbs timing */
	{ extern int mono_wasm_jit_devirt_profile; const char *dp = g_getenv ("MONO_WASM_JIT_DEVIRT_PROFILE"); mono_wasm_jit_devirt_profile = (dp && *dp && *dp != '0') ? 1 : 0; }
	{ extern int mono_wasm_jit_batch_module; const char *bm = g_getenv ("MONO_WASM_JIT_BATCH_MODULE"); mono_wasm_jit_batch_module = (bm && *bm) ? atoi (bm) : 0; }   /* 1 = batch islands; 2 = single-member batches (bisect); 3 = whole program in one module (upper bound) */
	{ extern int mono_wasm_jit_batch_all_at; const char *ba = g_getenv ("MONO_WASM_JIT_BATCH_ALL_AT"); mono_wasm_jit_batch_all_at = (ba && *ba) ? atoi (ba) : 250; }
	{ extern int mono_wasm_jit_batch_settle; const char *bs = g_getenv ("MONO_WASM_JIT_BATCH_SETTLE"); mono_wasm_jit_batch_settle = (bs && *bs && atoi (bs) > 0) ? atoi (bs) : 128; }
	{ extern int mono_wasm_jit_batch_min; const char *bn = g_getenv ("MONO_WASM_JIT_BATCH_MIN"); mono_wasm_jit_batch_min = (bn && *bn && atoi (bn) > 1) ? atoi (bn) : 16; }
	{ extern int mono_wasm_jit_lazy_gcp; const char *lg = g_getenv ("MONO_WASM_JIT_LAZY_GCP"); mono_wasm_jit_lazy_gcp = (lg && *lg) ? atoi (lg) : 1; } /* GC points a method may have and still defer its ref frame; <=0 = unlimited */
	{ extern int mono_wasm_jit_batch_inline; const char *bi = g_getenv ("MONO_WASM_JIT_BATCH_INLINE"); mono_wasm_jit_batch_inline = (bi && *bi) ? (*bi != '0') : 0; } /* MEASURED WORSE -- see the note at the definition. Default OFF; =1 only to re-run the A/B. */
	{ extern int mono_wasm_jit_batch_max; const char *bx = g_getenv ("MONO_WASM_JIT_BATCH_MAX"); int n = (bx && *bx) ? atoi (bx) : 384; mono_wasm_jit_batch_max = n < 2 ? 2 : (n > 512 ? 512 : n); }
	{ extern int mono_wasm_jit_batch_bytes; const char *bb = g_getenv ("MONO_WASM_JIT_BATCH_BYTES"); mono_wasm_jit_batch_bytes = (bb && *bb && atoi (bb) > 0) ? atoi (bb) : 786432; }
	{ extern int mono_wasm_jit_vcall_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline vcall IC; clamp [1,8]; 1 = legacy monomorphic */
	{ extern int mono_wasm_jit_vcall_aot_ways; const char *w = g_getenv ("MONO_WASM_JIT_VCALL_AOT_WAYS"); int n = (w && *w) ? atoi (w) : 1; mono_wasm_jit_vcall_aot_ways = n < 1 ? 1 : (n > 8 ? 8 : n); } /* N-way inline AOT-vcall IC; clamp [1,8]; 1 = legacy first-wins */
	{ extern int mono_wasm_jit_vcall_shared_miss_enabled; const char *sm = g_getenv ("MONO_WASM_JIT_VCALL_SHARED_MISS"); mono_wasm_jit_vcall_shared_miss_enabled = (sm && *sm) ? (*sm != '0') : 1; }
	{ extern int mono_wasm_jit_vcall_slim; const char *sl = g_getenv ("MONO_WASM_JIT_VCALL_SLIM"); mono_wasm_jit_vcall_slim = (sl && *sl) ? (*sl != '0') : 1; }
	{ extern int mono_wasm_jit_ci_imports; const char *ic = g_getenv ("MONO_WASM_JIT_CI_IMPORTS"); mono_wasm_jit_ci_imports = (ic && *ic) ? (*ic != '0') : 0; } /* the two never-converted constant-index call sites via declared imports */
	{ extern int mono_wasm_jit_direct_import; const char *dd = g_getenv ("MONO_WASM_JIT_DIRECT_IMPORT"); mono_wasm_jit_direct_import = (dd && *dd) ? (*dd != '0') : 0; } /* a JITted callee via a declared import instead of constant-index call_indirect; needs an SCC-closed module */
	{ extern int mono_wasm_jit_structured_cfg; const char *sc = g_getenv ("MONO_WASM_JIT_STRUCTURED_CFG"); mono_wasm_jit_structured_cfg = (sc && *sc) ? (*sc != '0') : 1; }
	{ extern int mono_wasm_jit_over_aot; const char *oa = g_getenv ("MONO_WASM_JIT_OVER_AOT"); mono_wasm_jit_over_aot = (oa && *oa && *oa != '0') ? 1 : 0; } /* experimental second compiler tier for hot AOT bodies; safe fallback remains the AOT entry */
	{ extern int mono_wasm_jit_island; const char *il = g_getenv ("MONO_WASM_JIT_ISLAND"); mono_wasm_jit_island = (il && *il && *il == '0') ? 0 : 1; } /* 0 = no eager island formation (bottom-up retry only) */
	{ extern int mono_wasm_jit_inline_aot; const char *ia = g_getenv ("MONO_WASM_JIT_INLINE_AOT"); mono_wasm_jit_inline_aot = (ia && *ia) ? (*ia != '0') : 1; } /* emit the inline direct same-ABI AOT call instead of the residual. Build 1 = no wasm-EH (non-throwing callees only). 1 = on, default 0 = off. */
	{ extern int mono_wasm_jit_ldaddr_vtype; const char *lv = g_getenv ("MONO_WASM_JIT_LDADDR_VTYPE"); mono_wasm_jit_ldaddr_vtype = (lv && *lv) ? (*lv != '0') : 1; } /* OP_LDADDR of NON-SCALAR ref-free local via a full-size addr-frame slot. Exonerated re: corruption; kept gated for repro parity. */
	{ extern int mono_wasm_jit_vtype_scalar_ref; const char *vr = g_getenv ("MONO_WASM_JIT_VTYPE_SCALAR_REF"); mono_wasm_jit_vtype_scalar_ref = (vr && *vr) ? (*vr != '0') : 1; } /* ref-etype scalar-vtype arg via a GC-scanned ref-shadow slot; GC-critical. */
	{ extern int mono_wasm_jit_vtype_scalar; const char *vs = g_getenv ("MONO_WASM_JIT_VTYPE_SCALAR"); mono_wasm_jit_vtype_scalar = (vs && *vs) ? (*vs != '0') : 1; } /* pass a BYVAL ref-free scalar-vtype call arg as its single-field etype scalar (LLVMArgWasmVtypeAsScalar ABI). Needs LDADDR_VTYPE. */
	{ extern int mono_wasm_jit_vtype_byaddr; const char *vb = g_getenv ("MONO_WASM_JIT_VTYPE_BYADDR"); mono_wasm_jit_vtype_byaddr = (vb && *vb) ? (*vb != '0') : 1; } /* multi-field/large vtype args as i32 pointer to a caller-owned C-stack copy (ArgValuetypeAddrOnStack). Default ON. Read ONCE (process-lifetime): f_sig_id fingerprints must not split mid-process. */
	{ extern int mono_wasm_jit_vret; const char *vr2 = g_getenv ("MONO_WASM_JIT_VRET"); mono_wasm_jit_vret = (vr2 && *vr2) ? (*vr2 != '0') : 1; } /* vtype returns via hidden vret pointer (trailing i32 param internally). Default ON. Same process-lifetime rule. */
	{ extern int mono_wasm_jit_byref; const char *br = g_getenv ("MONO_WASM_JIT_BYREF"); mono_wasm_jit_byref = (br && *br) ? (*br != '0') : 1; } /* lower calls whose callee sig has byref args/ret through the residual + vcall fallback instead of bailing the caller (-7). Default ON; =0 reverts to the pre-hardening bails for A/B. */
	{ extern int mono_wasm_jit_ldaddr_vtype_ref; const char *lr = g_getenv ("MONO_WASM_JIT_LDADDR_VTYPE_REF"); mono_wasm_jit_ldaddr_vtype_ref = (lr && *lr) ? (*lr != '0') : 1; } /* ref-bearing non-scalar vtype locals in the (conservatively scanned) addr frame. Default ON. */
	{ extern int mono_wasm_jit_ref_wt; const char *wt = g_getenv ("MONO_WASM_JIT_REF_WT"); mono_wasm_jit_ref_wt = (wt && *wt) ? (*wt != '0') : 0; } /* write-through ref vregs: wasm local is the value home, the frame slot is a def-mirrored pin (the LLVM-AOT gc_pin model); reads stop touching memory. Default OFF until soak. */
	{ extern int mono_wasm_jit_slotlive; const char *sl = g_getenv ("MONO_WASM_JIT_SLOTLIVE"); mono_wasm_jit_slotlive = (sl && *sl) ? (*sl != '0') : 0; } /* GC-point liveness slot elision: an isref vreg whose whole def->use range crosses no GC point keeps NO frame slot (stays a fast wasm local the GC never needs to see). Cuts pin pressure + frame size. Default OFF until soak. */
	{ extern int mono_wasm_jit_slotzero; const char *sz = g_getenv ("MONO_WASM_JIT_SLOTZERO"); mono_wasm_jit_slotzero = (sz && *sz) ? (*sz != '0') : 0; } /* dead-slot zeroing: null a single-bb ref slot at its last use so dead objects stop pinning (long-lived JSPI frames otherwise pin them until frame pop). Requires REF_WT+SLOTLIVE. Default OFF until soak. */
	{ extern int mono_wasm_jit_nce; const char *nc = g_getenv ("MONO_WASM_JIT_NCE"); int n = (nc && *nc) ? atoi (nc) : 1; mono_wasm_jit_nce = n < 0 ? 0 : (n > 2 ? 2 : n); } /* 0=off; 1=safe bb-local NCE; 2=experimental dominator propagation. */
	{ extern int mono_wasm_jit_lcse; const char *lc = g_getenv ("MONO_WASM_JIT_LCSE"); mono_wasm_jit_lcse = (lc && *lc && *lc != '0') ? 1 : 0; } /* extended-bb redundant heap-load elimination. Default OFF pending an A/B; reach measured 0.69% (54/7830 loads) on jbox2d — correct but nearly inert, see WjLcse. */
	{ extern int mono_wasm_jit_coalesce; const char *cs = g_getenv ("MONO_WASM_JIT_COALESCE"); mono_wasm_jit_coalesce = (cs && *cs && *cs != '0') ? 1 : 0; } /* share one wasm local between vregs with disjoint live ranges. Default OFF until A/B'd. */
	{ extern int mono_wasm_jit_aot_entry; const char *ae = g_getenv ("MONO_WASM_JIT_AOT_ENTRY"); mono_wasm_jit_aot_entry = (ae && *ae && *ae != '0') ? 1 : 0; } /* fast path in the jiterpreter native->interp entry for already-JITted methods. */
	{ extern int mono_wasm_jit_nodispatch; const char *nd = g_getenv ("MONO_WASM_JIT_NODISPATCH"); mono_wasm_jit_nodispatch = (nd && *nd && *nd != '0') ? 1 : 0; } /* elide dispatch scaffolding for single-bb methods. Default OFF, unvalidated. */
	{ extern int mono_wasm_jit_raise_nogc; const char *rn = g_getenv ("MONO_WASM_JIT_RAISE_NOGC"); mono_wasm_jit_raise_nogc = (rn && *rn) ? atoi (rn) : 0; } /* 0=off, 1=liveness-only exemption, 2=also publish nogc/gcp-count (measured unsound). See wj_ins_is_gcpoint. */
	{ extern int mono_wasm_jit_marshal_wrappers; const char *mw = g_getenv ("MONO_WASM_JIT_MARSHAL_WRAPPERS"); mono_wasm_jit_marshal_wrappers = (mw && *mw && *mw != '0') ? 1 : 0; } /* JIT managed<->native marshalling wrappers; default 0 = bail them to the interp (fix for the get_method_attributes wild store), =1 reverts (buggy) for A/B */
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
	{ extern int mono_wasm_jit_vcall_aot; const char *va = g_getenv ("MONO_WASM_JIT_VCALL_AOT"); mono_wasm_jit_vcall_aot = (va && *va) ? (*va != '0') : 1; } /* fast AOT-vcall dispatch: 0=off (residual) */
	{ extern int mono_wasm_jit_vcall_aot_ic; const char *vc = g_getenv ("MONO_WASM_JIT_VCALL_AOT_IC"); mono_wasm_jit_vcall_aot_ic = (vc && *vc) ? (*vc != '0') : 1; } /* per-call-site AOT-vcall IC; needs VCALL_INLINE_IC+VCALL_AOT. */
#ifdef HOST_BROWSER
	/* These three are DEBUG store/GC guards whose globals + runtime-check emission are HOST_BROWSER-only
	 * (they insert per-store checks that only do anything when the JITted code actually RUNS). The offline
	 * cross-compiler dump never executes JITted code, so skip their env here — otherwise auto_init would
	 * reference browser-only globals and fail to link into mono-aot-cross. */
	{ extern int mono_wasm_jit_storeguard; const char *sg = g_getenv ("MONO_WASM_JIT_STOREGUARD"); mono_wasm_jit_storeguard = (sg && *sg && *sg != '0') ? 1 : 0; } /* DEBUG: bounds-check every ref/addr-frame store to catch the wild store (traps at the culprit). default off */
	{ extern int mono_wasm_jit_objguard; const char *og = g_getenv ("MONO_WASM_JIT_OBJGUARD"); mono_wasm_jit_objguard = (og && *og && *og != '0') ? 1 : 0; } /* DEBUG: before every ref-field store, validate the object BASE is a live heap object (catches missed-ref/stale-base wild stores). default off */
#endif
	{ extern int mono_wasm_jit_missedref; const char *mr = g_getenv ("MONO_WASM_JIT_MISSEDREF"); mono_wasm_jit_missedref = (mr && *mr && *mr != '0') ? 1 : 0; } /* DIAG: names a missed ref. For every method, log any NONREF-classified i32 vreg used as a MEMBASE load/store base or virtual-call receiver (a stale one of these is the wild-deref corruptor), with its defining opcode -> pins which wj_opcode_is_nonref case is wrong. Bounded. default off */
	{ extern int mono_wasm_jit_refverify; const char *rv = g_getenv ("MONO_WASM_JIT_REFVERIFY"); mono_wasm_jit_refverify = (rv && *rv) ? atoi (rv) : 0; } /* 1=log, 2=assert classification-vs-structural-marking violations; default off */
	{ extern int mono_wasm_jit_vcall_inline_ic; const char *vi = g_getenv ("MONO_WASM_JIT_VCALL_INLINE_IC"); mono_wasm_jit_vcall_inline_ic = (vi && *vi) ? (*vi != '0') : 1;
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

/* (mono_wasm_jit_max — a racy high-water setter — lived here. Its only counter was WJC_REF_HWM, the old
 * ref shadow stack's depth; both are gone. Reintroduce it if a future counter is a max rather than a sum.) */

/* Per-method emit LOG verbosity (MONO_WASM_JIT_VERBOSE), independent of the counters: 0 silent, 1
 * registered+invalid, 2 +bail, 3 +emit-enter. Default 0 so a stats run no longer floods stdout. */
int mono_wasm_jit_verbose = 0;
const char *mono_wasm_jit_watch = NULL;
int mono_wasm_jit_names = 0;   /* MONO_WASM_JIT_NAMES=1 emits a wasm name section per JITted module so traps self-symbolicate */
/* MONO_WASM_JIT_HELPER_IMPORTS=0 keeps runtime-helper calls as `i32.const <table index>; call_indirect`.
 * Default 1 = declare each called helper as a wasm function import and emit a direct `call`, which drops the
 * bounds check, signature check and table-index arithmetic V8 emits for every call_indirect (it does not fold
 * a constant index into a direct call — a 54-byte-IL MethodHandle stub had 21 call_indirect in its wasm and
 * exactly 21 `call *` in its x86). Kept as a knob because it changes the module's import list and function
 * index space, which is exactly the kind of change worth being able to switch off in one run. */
int mono_wasm_jit_helper_imports = 1;
/* MONO_WASM_JIT_AOT_IMPORTS: give the INLINE-AOT direct call the same treatment HELPER_IMPORTS gives runtime
 * helpers — declare the callee's table index as a wasm function import and emit a plain `call`, instead of
 * `i32.const <index>; call_indirect <ti> 0`.
 *
 * Why it was left out: the helper-import path was built for the ~25 fixed runtime helpers, and the inline-AOT
 * site (the `aot_addr` call below) was written before it existed. But it is the SAME shape — a constant table
 * index and a functype the emitter already computed — and the reason HELPER_IMPORTS exists applies verbatim:
 * V8 does not fold a constant call_indirect index into a direct call, so every one of these pays a bounds
 * check, a signature check and index arithmetic. The site is hot: WJC_FAST_INLINE_AOT read 943,008,975 over a
 * single session (~90k inline-AOT calls per frame).
 *
 * Expected size is honest and small — a few instructions on ~90k calls/frame is order 1-2% of a 38 ms frame,
 * which is BELOW this harness's run-to-run spread. It is shipped on mechanism (V8 provably emits the checks;
 * see the HELPER_IMPORTS comment's x86 verification) rather than on a measured delta, and kept as a knob so
 * it can be bisected out if it ever correlates with a fault.
 *
 * Failure mode differs from call_indirect and is worth knowing: a wrong functype traps at CALL time today,
 * but as an import it fails INSTANTIATION, so the whole method silently falls back to the interpreter and
 * shows up as fewer `registered` methods + a WJC_INVALID bump rather than as a crash. */
int mono_wasm_jit_aot_imports = 1;
/* MONO_WASM_JIT_MAX_HIMP: how many distinct (table index, functype) helper/AOT-callee imports one module may
 * declare before further direct calls fall back to call_indirect. Was a compile-time 32, then 64, then 192, because
 * AOT_IMPORTS made the array far busier than the ~25 fixed runtime helpers it was originally sized for.
 *
 * THE CAP IS NOT BINDING and raising it buys nothing. A cross-binary reading once suggested 64 -> 192 was
 * worth +3.9% fps; the within-binary A/B that this knob exists to make possible put every metric inside noise
 * with the direction if anything favouring the SMALLER cap, so that +3.9% was noise (n=3 vs 6 at 5-9% spread
 * is exactly the regime that manufactures a 4% difference). Counting imports in the hot set directly agrees:
 * max 30 declared in any hot module, median 3, against a cap of 192. Kept as a knob because the cap could
 * start binding if the emitter ever imports per-callee rather than per-helper. Bounded by
 * WJ_MAX_HELPER_IMPORTS, which is only the array size. */
int mono_wasm_jit_max_himp = 192;
/* MONO_WASM_JIT_INLINE_ILOFS: store the per-basic-block IL offset INLINE (3 wasm ops against a method-long
 * local holding this activation's MonoMethodILState) instead of calling mono_wasm_jit_set_il_offset.
 *
 * That helper's whole body is a TLS load, a null test and a store, yet it was emitted as an out-of-line call
 * ONCE PER BASIC BLOCK of every EH-bearing method. Its 0.39% self time understates it: the profile's spill
 * census puts 82% of spilled hot instructions adjacent to a call, and caller-side spill cost is invisible in a
 * callee's self time — a call at every bb boundary forces the call-clobber discipline all over the method.
 * mono_wasm_jit_enter_island now returns the il_state so the prologue can cache it.
 *
 * RESULT: WORSE, and decisively — p50 41.2 -> 45.2 ms (+9.8%, spread ±1.1%), p90 +8.6%, fps 25.5 -> 22.8
 * (-10.6%). The reasoning above was wrong in an instructive way. Removing the call required keeping the
 * il_state pointer in a METHOD-LONG local, which creates a live value spanning every call in the method —
 * exactly the call-adjacent live range that the spill census was counting. Previously the TLS load happened
 * INSIDE the helper, so nothing was live in the caller across the call. On this register-starved target a
 * long-lived value costs more than the call it replaced.
 *
 * The general lesson, which contradicts the naive reading of "82% of spills are call-adjacent": inlining a
 * helper only pays when it removes CODE (vcall_shared_miss=0, -13.2%), not when it converts a call into
 * additional live state. Check what a change does to live ranges before assuming fewer calls is better. */
int mono_wasm_jit_inline_ilofs = 0;   /* MEASURED WORSE — see above. Default OFF; =1 only to re-run the A/B. */
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
/* MONO_WASM_JIT_IC_AUTOSIZE=0 emits MONO_WASM_JIT_VCALL_WAYS ways at every virtual call site. Default 1 =
 * size each site from the interpreter's receiver observations (mono_wasm_jit_vprof_arity), so a site only
 * ever seen monomorphic loses its ways 1..N-1 cold-scan loop entirely. Requires the devirt profile to be on
 * (it is, in the shipping knob set) — with no observations the configured width is used unchanged. */
int mono_wasm_jit_ic_autosize = 1;
/* MONO_WASM_JIT_DELEGATE_LOCAL_PIC: dispatch Delegate.Invoke through the WORKER-LOCAL recipe PIC
 * (WjLocalDelegatePicEntry) instead of the process-wide WjDelegateIC. The shared entry cannot cache a
 * function-table slot — dynamic table entries are per worker — so it caches an InterpMethod, and every
 * hit paid for that: load imethod, load imethod->fslot, test it, then probe the TLS liveness bitmap,
 * inside a seqlock bracket of two atomic loads. Worker-local caches the admitted fslot directly and has
 * a single writer, so all of that disappears (~79 -> ~49 wasm ops, 9 -> 6 loads, 3 -> 0 atomics on the
 * largest dispatch class in the profile). Kept as a knob because it is the only honest way to measure
 * it: fps deltas on this box need interleaved arms of the SAME binary. */
int mono_wasm_jit_delegate_local_pic = 1;
/* MONO_WASM_JIT_ESLOT_RESIDUAL: a residual whose callee has no AOT code but IS wasm-JIT compiled enters the
 * callee's e-slot straight from the residual scratch, instead of marshalling into InterpEntryData and letting
 * interp_entry rediscover the same e-slot. Measured target: interp_entry 3.838% + call_interp 2.588% of
 * in-game samples, both ~1/3 register-spill traffic around their own internal calls. */
int mono_wasm_jit_eslot_residual = 1;
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
 *   1 = full (residual every supported direct call; default when the env is unset)
 *   2 = only calls with a VOID return   (skips return-value marshalling)
 *   3 = only calls with NO params       (skips param marshalling; `this` still allowed)
 *   4 = only STATIC calls (no `this`)   (skips this marshalling; params/return allowed)
 *   5 = everything EXCEPT calls with params AND a non-void return
 * Check the bench stats (residual count) to confirm a restricted mode still exercised the residual. */
int mono_wasm_jit_residual_mode = 1;
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
int mono_wasm_jit_inline_aot = 1;     /* MONO_WASM_JIT_INLINE_AOT=1: emit the inline direct same-ABI AOT call (call_indirect cinfo->addr with this+args+rgctx, no interp_entry/frame/LMF) instead of the residual, for AOT'd callees. Build 1 = no wasm-EH yet (test non-throwing callees). Default ON, and hot: ~90k inline-AOT calls per frame. */
/* rgctx handling: methods that call a generic-shared callee needing a runtime generic context
 * (cfg->uses_rgctx_reg) route each such call through the interp residual (which derives the
 * context from the concrete inflated call->method — both interp_entry and do_jit_call-via-gsharedvt_out
 * are rgctx-correct), instead of bailing the WHOLE method at the gate. This is the dominant EH-method
 * blocker on IKVM: a Java try/catch lowers to a catch-block call to the generic ExceptionHelper.MapException<T>,
 * which sets uses_rgctx_reg — so the old rgctx bail killed nearly every render-path EH method (e.g.
 * tesselateBlock) right after the EH gate let it through. The rgctx call sits in the COLD catch block, so
 * the (slower) residual re-entry there is free; the hot try-body JITs to wasm. The direct f-slot path needs
 * no rgctx (our f-slots are dedicated/concrete compiles); INLINE_AOT is skipped for rgctx calls because the
 * call needs the CALLSITE runtime generic context, while the AOT fast path only knows how to bake the CALLEE
 * extra arg/rgctx (cinfo->extra_arg in llvm_only, or the matching fallback recovery below);
 * indirect/virtual rgctx calls still bail (untested shape). */
int mono_wasm_jit_marshal_wrappers = 0; /* MONO_WASM_JIT_MARSHAL_WRAPPERS: JIT the managed<->native marshalling wrappers (managed-to-native icall/pinvoke, native-to-managed, runtime-invoke). Default 0 = bail them to the interpreter. Their marshalling IR (LMF save/restore, the native fptr baked as an iconst, handle/byref marshal stores, coop-GC transitions) produces a ref store through a garbage/stale object base that the isref classifier + raw membase-store lowering mishandle -> wild store -> intermittent heap/metadata corruption (confirmed live: System.Reflection.MonoMethodInfo:get_method_attributes -> OBJGUARD kind 2 -> AIOOBE / mono_metadata_token_table assert). =1 reverts (buggy) for A/B. The synchronized (SYNCHRONIZED/OTHER) wrapper path is unaffected. */
int mono_wasm_jit_ldaddr_vtype = 1;   /* MONO_WASM_JIT_LDADDR_VTYPE: extend OP_LDADDR to NON-SCALAR ref-free valuetype locals via a full-size addr-frame slot. Default ON (exonerated: jit17 corrupted with it OFF; kept gated for binary/repro parity). */
int mono_wasm_jit_vtype_scalar_ref = 1; /* MONO_WASM_JIT_VTYPE_SCALAR_REF: extend VTYPE_SCALAR to a scalar-vtype whose SINGLE field is a managed REFERENCE (e.g. RuntimeTypeHandle{RuntimeType}). Backed by a GC-SCANNED ref-shadow-stack slot (not the un-scanned addr frame): OP_LDADDR yields refbase+slot*4 so the field store/load track the ref as a conservative pinning root, and the store's inline card-barrier marks a HARMLESS card (wasm32 has no overlapping cards — the 8MB table covers the whole 32-bit space, so a non-heap mark is in-bounds and never scanned). GC-CRITICAL: validate in-browser with STOREGUARD/OBJGUARD. */
int mono_wasm_jit_vtype_scalar = 1;   /* MONO_WASM_JIT_VTYPE_SCALAR: pass a BYVAL scalar-vtype call arg (mini_wasm_is_scalar_vtype: struct <=8 bytes, one field) as its single-field SCALAR — the ABI the AOT callee was compiled with (LLVMArgWasmVtypeAsScalar). The vtype value is addr-frame-backed (LDADDR_VTYPE), so we load its field (offset 0) from the addr-frame slot and pass that. Ref-free etype only; the ref-etype variant is gated separately (VTYPE_SCALAR_REF, GC-scanned ref-shadow slot). Requires LDADDR_VTYPE. */
int mono_wasm_jit_vtype_byaddr = 1;   /* MONO_WASM_JIT_VTYPE_BYADDR: multi-field/large value-type args as an i32 pointer to a caller-owned copy (native ArgValuetypeAddrOnStack). The copy lives in the caller's C-stack frame — conservatively GC-scanned, so ref-bearing structs (IKVM MHA`8) pin their referents exactly like AOT'd structs in C locals. Classification is process-lifetime-constant (read once here) so f_sig_id fingerprints can't split across an in-process flag flip. */
int mono_wasm_jit_vret = 1;           /* MONO_WASM_JIT_VRET: value-type returns via a hidden vret pointer — internally a TRAILING i32 param (the native AOT ABI puts vret FIRST; the inline-AOT call path reorders). Same process-lifetime rule as VTYPE_BYADDR. */
int mono_wasm_jit_byref = 1;          /* MONO_WASM_JIT_BYREF: lower a call whose CALLEE SIGNATURE has byref (ref/out/Span) args or a byref return through the interp residual and the vcall fallback, instead of bailing the WHOLE caller (bail -7, permanent). Previously gated "until byref marshalling is hardened"; it now is:
                                       *   ARGS were already correct — the residual spills the byref VALUE (the pointer) and the C side derefs that slot (wj_arg_slot_holds_pointer, shared by mono_wasm_jit_call_interp + wasm_jit_aot_call_lean), matching interp_entry's asymmetric arg convention (byref = the pointer, by-value = a pointer TO the value).
                                       *   RETURNS needed one fix: stackval_to_data_sign_ext switched on type->type without a byref guard, so `ref sbyte/byte/short/ushort` got the sign/zero-extended LOW BYTE(S) OF THE POINTER (live via wasm_jit_aot_call_lean, which passes the raw sig->ret). Fixed unconditionally in interp.c.
                                       * The emitter needed nothing: byref is WJ_ARG_SCALAR/i32 in mono_wasm_get_call_info, and every sub-word ret normalizer keys off mini_get_underlying_type, which erases byref to MONO_TYPE_I. GC: byref args/returns are already pinned (see the arg_is_ref / def_nonref seeds below) and the interp stack is scanned conservatively, so an interior pointer pins its target.
                                       * =0 restores the four pre-hardening bails for A/B bisection. Declared unconditionally (NOT under HOST_BROWSER): the vcall gate compiles in the cross build too. */
int mono_wasm_jit_ldaddr_vtype_ref = 1; /* MONO_WASM_JIT_LDADDR_VTYPE_REF: allow REF-BEARING non-scalar vtype locals in the addr frame (full-size slot). The addr slots moved into the conservatively-scanned C-stack frame (see the frame doc above wasm_ld) — embedded refs over-pin, same guarantee AOT structs-in-C-locals rely on; the old "GC-unsafe frame" bail predates that move. */
int mono_wasm_jit_missedref = 0;      /* MONO_WASM_JIT_MISSEDREF: diagnostic — log NONREF-classified vregs used as MEMBASE bases / call receivers + their defining opcode, to name an isref-inference gap. Default off. */
int mono_wasm_jit_ref_wt = 0;         /* MONO_WASM_JIT_REF_WT: write-through ref vregs — the wasm LOCAL is the value home (fast reads), and every def ALSO stores to the frame slot so the conservative scan pins the referent (exactly LLVM AOT's gc_pin volatile-store model, mini-llvm.c emit_gc_pin). Sound because a pinned object never moves, so the cached local can't go stale — the same invariant AOT locals and JSPI-frozen locals rely on. Slot-HOMED exceptions: addrslot==-2 sentinels (their slot address escapes via OP_LDADDR, callees write through it). Default OFF until soak; flip to 1 after the test matrix passes. */
int mono_wasm_jit_aot_entry = 0;      /* MONO_WASM_JIT_AOT_ENTRY: fast path in mono_jiterp_interp_entry (interp.c) for a method that is
                                       * already JITted and admitted. The jiterpreter trampoline has by then already marshalled the args into
                                       * the interp stack in exactly the layout the entry thunk reads, so the InterpFrame zeroing, LMF push/pop,
                                       * maybe_compile and admission DFS around the call are all scaffolding for an interpreter run that will not
                                       * happen. perf annotate shows that function is FLAT across ~74 instructions with no hotspot, i.e. the whole
                                       * preamble IS the cost, so it can only be removed by not entering it. Worth ~3.4% on jbox2d; the symbol
                                       * drops 5.99% -> 4.19% of steady-state time. Gated per-thread on mono_wasm_jit_slot_live. */
int mono_wasm_jit_coalesce = 0;       /* MONO_WASM_JIT_COALESCE: share one wasm local between vregs whose live ranges are disjoint,
                                       * computed from a real backward liveness dataflow (mention ranges are unsound across a back edge).
                                       * li[] is otherwise one local per vreg with NO reuse: AABB:combine declares 58 where teavm needs 5.
                                       * Caveat worth keeping in mind before attributing any win to this: TurboFan converts wasm locals to
                                       * SSA, so its register pressure follows live-range OVERLAP, which renaming does not change. Default OFF. */
int mono_wasm_jit_lcse = 0;           /* MONO_WASM_JIT_LCSE: extended-basic-block redundant heap-load elimination.
                                       * mono has NO general CSE/GVN (optflags-def.h: SSAPRE is marked obsolete, ALIAS_ANALYSIS
                                       * is locals-only), so every reload javac emitted survives into the wasm. Measured on
                                       * AABB:combine, identical Java source: we emit 39 heap loads where teavm emits 14, and
                                       * TurboFan does NOT clean them up -- its compiled output has 67 memory loads, so all 39
                                       * are real. The dominant shape is javac's `a < b ? a : b` (jbox2d's MathUtils.min/max),
                                       * which reloads BOTH operands in BOTH arms after the compare already loaded them.
                                       * Measure reach with MONO_WASM_JIT_STATS=1 and read [wasm-jit lcse]:
                                       * hits/loads_seen, NOT adds/hits. Default OFF until A/B'd. */
int mono_wasm_jit_slotlive = 0;       /* MONO_WASM_JIT_SLOTLIVE: GC-point liveness slot elision — an isref vreg gets a frame slot ONLY if a GC can actually observe it there: it is live across a GC-capable instruction (wj_ins_is_gcpoint) or spans basic blocks. A ref defined and fully consumed between two GC points is invisible to the collector (cooperative suspend: this thread only scans at safepoints/calls), so it can stay in an unscanned wasm local. Main pin-pressure lever: most deref-backstop bases and immediately-consumed call results lose their slots. Disabled when STOREGUARD/OBJGUARD are on (they key ref-ness off refslot, so elision would change guard semantics). Default OFF until soak. */
int mono_wasm_jit_slotzero = 0;       /* MONO_WASM_JIT_SLOTZERO: dead-slot zeroing — zero a SINGLE-BB slotted ref vreg's frame slot at its last use (only when a GC point follows in the bb), so the dead object stops pinning. Critical for long-lived frames (a JSPI-suspended main loop otherwise pins its stale refs for the app lifetime). Single-bb scope makes death provable without dataflow (a vreg live into any EH handler is multi-bb by definition). Requires REF_WT (reads come from the local, so the slot can be zeroed BEFORE the killing instruction — stack-neutral, no terminator special cases) and SLOTLIVE (which computes the last-use walk). Default OFF until soak. */
/* MONO_WASM_JIT_NCE: null-check elimination. cfg->explicit_null_checks is forced on for this
 * backend (mini.c, at the compile_wasm fork) because wasm linear-memory address 0 is a perfectly valid
 * address and cannot fault, so EVERY dereference carries its own COMPARE_IMM+COND_EXC pair. Nothing in
 * the mini pipeline removes them: abcremoval's null-check rule only CONSUMES facts produced by
 * OP_NOT_NULL, and ir-emit.h emits OP_NOT_NULL under COMPILE_LLVM only. Measured on the jbox2d bench,
 * with the consumer's full opt set on: 40 of 40 call_indirect in AABB:combine are null-check throw
 * helpers, and 7781 of 16417 (47%) program-wide.
 *
 * Mode 1 is deliberately the cheap one -- a per-bb bitmap of vregs already proven non-null,
 * killed at any redefinition -- because that is what the dominant pattern needs: a chain of
 * dereferences off one receiver (a.lowerBound.x, a.lowerBound.y, ...) re-tests the same vreg within a
 * single basic block. It also removes the emitter's OWN vcall receiver check, which duplicates the
 * IR-level check method_to_ir already emitted for the same receiver.
 *
 * Soundness rests on seeing every write to a tracked vreg. Every value-producing store in the emitter
 * goes through wasm_st(ins->dreg) (plus the one cfg->ret->dreg in OP_SETRET, killed explicitly), and
 * address-taken vregs -- whose home is memory a callee can write through -- are never tracked at all.
 * Mode 2 additionally propagates never-written facts down Mono's dominator tree, restricted to methods
 * with no EH clauses. The dominator relation does not model implicit exception transfers, so a fact
 * established on a null check's normal continuation can be false inside a catch/finally entered from
 * before that check ran; a clause-free method has no such transfer, which makes the propagation sound
 * without any dataflow reasoning (same argument, same property, as the RAISE_NOGC gate below).
 *
 * Mode 1 remains the default. Mode 2's measured value on the jbox2d workload is ~0, from an interleaved
 * A/B of mode 1 against mode 2 on this branch: 0.721/0.744 vs 0.733/0.738 ms/step, i.e. inside run-to-run
 * spread and slightly worse if anything, checksum identical throughout. Note this contradicts the reach
 * argument above (AABB:combine's eight repeated parameter checks are real, and mode 2 does remove them) --
 * removing those null checks simply does not move wall clock here, because the bodies are not
 * null-check-bound once the entry tier is working. The pass is kept because the clause-free gate makes it
 * correct and it may pay off on other EH-free numeric code, but do not enable it without re-measuring.
 * =0 is the kill switch. */
int mono_wasm_jit_nce = 1;
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
 *   2 — everything, i.e. the original behaviour. MEASURED UNSOUND; see wj_ins_is_gcpoint.
 * Default OFF (silent-corruption risk class). */
int mono_wasm_jit_raise_nogc = 0;
/* MONO_WASM_JIT_NODISPATCH: skip the loop/block/br_table scaffolding for a single-bb, edge-free,
 * clause-free method (see skip_dispatch). Shape-wise it is strictly less code, but the first two
 * measurements both came out 9-15% SLOWER than the 1.905 control -- taken on a machine with a 3-5
 * loadavg from a concurrent build, so unattributable rather than disproven. Default OFF until it can be
 * A/B'd on a quiet machine; it is a flag precisely so that does not require another build. */
int mono_wasm_jit_nodispatch = 0;
int mono_wasm_jit_refverify = 0;      /* MONO_WASM_JIT_REFVERIFY (0/1/2): after the isref fixpoint, cross-check classification against the structural vreg_is_ref/vreg_is_mp marking — 1 logs violations (a marked vreg classified nonref = lost seed = would-be silent corruption), 2 asserts. Debug only, default off. */
const char *mono_wasm_jit_dump_ir = NULL;  /* MONO_WASM_JIT_DUMP_IR=<substr>: dump clauses + bb regions + opcode stream for clause-bearing methods whose full name contains <substr> (EH-lowering ground truth, e.g. "indigo"). */
/* Island heuristic levers (Part 5), all default-OFF so the baseline is unchanged and each can be A/B'd. */
int mono_wasm_jit_entry_promote = 0;   /* Lever A: MONO_WASM_JIT_ENTRY_PROMOTE=N — after a hot interp caller invokes JITted callees N times, force-JIT the caller (grow the island UPWARD). 0 = off. */
int mono_wasm_jit_residual_perm = 0;   /* Lever B: MONO_WASM_JIT_RESIDUAL_PERM=1 — under residual=0, residual-route ONLY a permanently-un-JITtable blocker instead of bailing the whole caller. 0 = off. */
int mono_wasm_jit_residual_cold = 0;   /* Lever B': MONO_WASM_JIT_RESIDUAL_COLD=1 — under residual=0, residual-route a blocker the island cold-gate would refuse to pull in (still counting hits, below thresh/cold_div, not block-promoted): a cold branch (IKVM __<GetInstance> lambda factory, one-shot init, error path) reached rarely from a hot caller. Lets the hot method JIT while paying ~1 transition per cold call, NOT a per-iteration storm; hot/parked callees still bail so the island force-JITs them. 0 = off. NOTE: jit34 showed this misclassifies hot-via-JITted-caller callees as cold -> 2M residuals/frame -> 1.5fps. Keep OFF until residuals self-heal to the callee's f-slot. */
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
int mono_wasm_jit_vcall_aot = 1;
int mono_wasm_jit_vcall_aot_ic = 1;   /* MONO_WASM_JIT_VCALL_AOT_IC=1: per-call-site inline cache for AOT-backed virtual targets — skip scratch()+resolve_fslot()+aot_target() (3 C calls/vcall) on a monomorphic hit, call_indirect the cached AOT body directly. Needs VCALL_INLINE_IC + VCALL_AOT. Hottest-path + MT — validate in-browser after any change. */
/* MONO_WASM_JIT_VCALL_INLINE_IC: the inline monomorphic vcall IC fast-path (call_indirect the cached
 * f-slot in wasm, skipping the scratch() + resolve_fslot C helpers on a hit — the profiled #1 game-thread
 * cost, vcall_resolve_fslot ~17%). DEFAULT OFF; =1 enables. NOW MT-SAFE on threaded builds: the original
 * "table[fslot] != null" liveness check was wrong (the per-thread table grows with a NON-null jiterpreter
 * placeholder, mono_jiterp_placeholder_jit_call (i32,i32,i32,i32)->void, so it passed for un-instantiated
 * slots -> call_indirect signature-mismatch trap, jit138). Fixed: the inline path now gates on the
 * authoritative per-thread bitmap via one cheap mono_wasm_jit_slot_live() call (wasm exposes no funcref
 * equality / funcref->i32 to compare the slot against the placeholder inline). Still one C boundary per hit
 * vs two + resolve for the helper; a full pure-wasm gate would need __tls_base imported to read the bitmap. */
int mono_wasm_jit_vcall_inline_ic = 1;

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

/* Every counter in the WJC_* enum must appear BOTH here and in the harness mirror (ikvmcraft
 * frontend/src/dotnet/jitbench.ts, `const WJ`), otherwise a counter is paid for on the hot path and
 * then never read. This assert is the tripwire: appending to the enum breaks the build until you have
 * bumped it, which is the prompt to add the new counter to this function and to jitbench.ts. */
g_static_assert (WJC_MAX == 68);

EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_stats (void)
{
	printf ("[wasm-jit stats] registered=%lld bailed=%lld invalid=%lld invoked=%lld residual=%lld fastvcall=%lld (counting %s)\n",
		WJC_(WJC_REGISTERED), WJC_(WJC_BAILED), WJC_(WJC_INVALID),
		WJC_(WJC_INVOKE), WJC_(WJC_RESIDUAL), WJC_(WJC_FASTVCALL),
		mono_wasm_jit_stats ? "on" : "OFF — set MONO_WASM_JIT_STATS=1");
	printf ("[wasm-jit time] gen=%.1fms instantiate=%.1fms attempts=%lld bytes=%lld\n",
		(double) WJC_(WJC_ELAPSED_GENERATION) / 1000.0, (double) WJC_(WJC_ELAPSED_INSTANTIATION) / 1000.0,
		WJC_(WJC_COMPILE_ATTEMPTS), WJC_(WJC_BYTES_GENERATED));
	printf ("[wasm-jit island] attempt=%lld completed=%lld budget_exhausted=%lld depth_exceeded=%lld blocked_perm=%lld blocked_cold=%lld promoted_up=%lld promoted_down=%lld\n",
		WJC_(WJC_ISLAND_ATTEMPT), WJC_(WJC_ISLAND_COMPLETED), WJC_(WJC_ISLAND_BUDGET_EXHAUSTED), WJC_(WJC_ISLAND_DEPTH_EXCEEDED),
		WJC_(WJC_ISLAND_BLOCKED_PERM), WJC_(WJC_ISLAND_BLOCKED_COLD), WJC_(WJC_PROMOTED_UP), WJC_(WJC_PROMOTED_DOWN));
	/* Event-driven blocker waiting (the island driver's alternative to poll-retrying a cold callee).
	 * woken/parked is the mean number of re-queues each park eventually produced. */
	printf ("[wasm-jit park] parked=%lld waiters_woken=%lld\n",
		WJC_(WJC_PARKED), WJC_(WJC_WAITER_WOKEN));
	printf ("[wasm-jit vcall] ic_hit=%lld ic_miss=%lld vfast_had=%lld vfast_new=%lld vfb_thresh=%lld vfb_perm=%lld vsync_work=%lld\n",
		WJC_(WJC_VIC_HIT), WJC_(WJC_VIC_MISS), WJC_(WJC_VFAST_HAD), WJC_(WJC_VFAST_NEW),
		WJC_(WJC_VFB_THRESH), WJC_(WJC_VFB_PERM), WJC_(WJC_VSYNC_WORK));
	/* vfb_thresh split by the target's slot state. cold = still counting (interp is the right answer);
	 * parked = crossed the threshold but its island won't close, i.e. interpreted on EVERY call and the
	 * real interp-residual driver; retry = transient compile-lock contention. Sum == vfb_thresh. */
	printf ("[wasm-jit vfb] cold=%lld parked=%lld retry=%lld (sum should equal vfb_thresh=%lld)\n",
		WJC_(WJC_VFB_COLD), WJC_(WJC_VFB_PARKED), WJC_(WJC_VFB_RETRY), WJC_(WJC_VFB_THRESH));
	printf ("[wasm-jit vperm] aot=%lld eh=%lld ldaddr=%lld lcompare=%lld sig=%lld byref=%lld gshared=%lld rgctx=%lld sync=%lld eh_other=%lld other_opcode=%lld other=%lld\n",
		WJC_(WJC_VPERM_AOT), WJC_(WJC_VPERM_EH), WJC_(WJC_VPERM_LDADDR), WJC_(WJC_VPERM_LCMP),
		WJC_(WJC_VPERM_SIG), WJC_(WJC_VPERM_BYREF), WJC_(WJC_VPERM_GSHARED), WJC_(WJC_VPERM_RGCTX), WJC_(WJC_VPERM_SYNC), WJC_(WJC_VPERM_EHOTHER),
		WJC_(WJC_VPERM_OTHEROP), WJC_(WJC_VPERM_OTHER));
	printf ("[wasm-jit aotroute] aot_routed=%lld interp_routed=%lld vcall_aot_fast=%lld\n",
		WJC_(WJC_AOT_ROUTED), WJC_(WJC_INTERP_ROUTED), WJC_(WJC_VCALL_AOT_FAST));
	/* eslot_residual is the subset of interp_routed that skipped interp_entry entirely: interp_routed counts
	 * the crossing, this counts the ones that took the short way in. */
	printf ("[wasm-jit eslot] eslot_residual=%lld (of interp_routed=%lld)\n",
		WJC_(WJC_ESLOT_RESIDUAL), WJC_(WJC_INTERP_ROUTED));
	/* Fast-path VOLUME. These dispatches are pure emitted wasm and call no counting helper, so without
	 * MONO_WASM_JIT_PROFILE_FAST=1 they are invisible and the counted totals above (invoked / fastvcall /
	 * residual) understate real dispatch volume — frame cost then can't be attributed. The counters are
	 * emitted INTO the JITted code, so they cost something: profile in a dedicated run, not a timing one. */
	printf ("[wasm-jit fast] inline_aot=%lld fslot_ic_hit=%lld aot_ic_hit=%lld%s\n",
		WJC_(WJC_FAST_INLINE_AOT), WJC_(WJC_FAST_VIC), WJC_(WJC_FAST_AOTIC),
		mono_wasm_jit_profile_fast ? "" : " (not profiled — set MONO_WASM_JIT_PROFILE_FAST=1)");
	printf ("[wasm-jit gcref] refbases_extra=%lld (0 across a soak with REFBASES=1 => REFBASES subsumed by structural seeds)\n",
		WJC_(WJC_REFBASES_EXTRA));
	printf ("[wasm-jit gcpin] ref_slots=%lld wt_vregs=%lld slots_elided=%lld slot_zero_stores=%lld frame_bytes=%lld\n",
		WJC_(WJC_REF_SLOTS), WJC_(WJC_REF_WT_VREGS), WJC_(WJC_SLOTS_ELIDED), WJC_(WJC_SLOT_ZERO_STORES), WJC_(WJC_FRAME_BYTES));
	printf ("[wasm-jit callform] local=%lld import=%lld indirect=%lld (per module assembled, not per execution)  admit_deferred=%lld\n",
		WJC_(WJC_CALL_LOCAL), WJC_(WJC_CALL_IMPORT), WJC_(WJC_CALL_INDIRECT), WJC_(WJC_ADMIT_DEFERRED));
	printf ("[wasm-jit vtabi] vt_byaddr_methods=%lld vret_methods=%lld\n",
		WJC_(WJC_VT_BYADDR_METHODS), WJC_(WJC_VRET_METHODS));
	printf ("[wasm-jit transition] residual_healed=%lld fast_delegate=%lld delegate_ic_hit=%lld (fast counters require PROFILE_FAST=1)\n",
		WJC_(WJC_RESIDUAL_HEALED), WJC_(WJC_FAST_DELEGATE), WJC_(WJC_DELEGATE_IC_HIT));
	/* LCSE reach. hits/loads_seen is the elimination RATE; evict>0 says the load table is the binding
	 * constraint (raise WJ_LCSE_LOADS) rather than the kill policy. Requires MONO_WASM_JIT_LCSE=1. */
	printf ("[wasm-jit lcse] loads_seen=%lld adds=%lld hits=%lld evict=%lld\n",
		WJC_(WJC_LCSE_LOADS_SEEN), WJC_(WJC_LCSE_ADDS), WJC_(WJC_LCSE_HITS), WJC_(WJC_LCSE_EVICT));
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
 * ldaddr) is named. Cumulative since boot.
 * NB: the callee-not-jitted bucket counts DISTINCT methods (noted once at compile_publish via
 * mono_wasm_jit_bail_hist_note_blocked), not per-island-re-attempt — so `total` here is smaller than
 * the per-attempt `bailed=` counter in [wasm-jit stats]. */
static const char *wj_opname (int op);

/* Count a method into the callee-not-jitted histogram bucket. Called from wasm_jit_compile_publish
 * (interp.c) on the FIRST blocked publish of each method only — the emitter itself skips this bucket
 * because the island driver re-emits blocked methods every iteration (per-attempt counting measured
 * island convergence, not outcomes). */
void
mono_wasm_jit_bail_hist_note_blocked (void)
{
	wj_bail_hist [WJB_CALLEE_NOT_JITTED]++;
}
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
static __thread guint8 *wj_slot_installed = NULL;
static __thread int wj_slot_installed_cap = 0;

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

/* Census counters (MONO_WASM_JIT_ENTRYCENSUS=1) — see the long note above mono_wasm_jit_liveness.
 *
 * PROCESS-WIDE ATOMICS, not just thread-locals, and that is forced rather than preferred: the worker
 * isolates whose numbers we want are pthreads parked in synchronous wasm, so CDP Runtime.evaluate never
 * returns for them (33 of 34 worker targets read "unreachable"). The same blocking is why the unlanded
 * SHARE_MODULES postMessage relay cannot work without JSPI suspends. Aggregating in C and reading the
 * total from the page target is the only reliable channel. These live in the wasm heap, which pthreads
 * share, so any thread's increment is visible.
 *
 * Split by PATH because the paths mean different things: the eager per-thread sweep (sync_thread) is
 * waste a lazy scheme would remove, whereas admit and the fslot backstop are demand-driven and would
 * still happen. A first attempt instrumented only sync_thread and read a flat zero at 11,069 registered
 * methods -- not because nothing was duplicated, but because admit is the path that actually instantiates
 * on other workers ("The compiling worker already installed this descriptor; other workers instantiate it
 * once here"). Hooking the two instantiate_*_local choke points instead makes the total unmissable. */
static gint32 wj_census_inst_total;   /* every successful instantiation, any path, any thread */
static gint32 wj_census_inst_sync;    /* via mono_wasm_jit_sync_thread (eager sweep) */
static gint32 wj_census_inst_admit;   /* via mono_wasm_jit_admit (per-worker, on demand at dispatch) */
static gint32 wj_census_inst_fslot;   /* via mono_wasm_jit_instantiate_fslot (direct-call backstop) */
static gint32 wj_census_entered_total;/* distinct (thread, e-slot) pairs actually entered */
static gint32 wj_census_inst_us_total;/* microseconds spent instantiating, all threads */

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
			/* Resolver for the direct-call helper imports the emitter declares as ("h", "<table index>").
			 * A C function pointer under wasm IS a table index, so wasmTable.get turns the index the emitter
			 * baked into the callable the import needs — no linker --export and no fixed helper registry.
			 * A Proxy rather than enumerating WebAssembly.Module.imports(): instantiation Gets exactly the
			 * names it needs, and the per-worker cache means each helper is wrapped once, not once per module
			 * (there are ~25k JIT'd modules in a session). Non-numeric keys return undefined so that any
			 * incidental property probe cannot hand back a function. */
			if (!Module.__wjHelperImports) {
				/* THE CACHE IS A MEMO OF wasmTable.get AND MUST BE INVALIDATED WHEN THE TABLE CHANGES.
				 *
				 * It was safe while only HELPERS were imported: those are C functions in
				 * dotnet.native.wasm, at table indices fixed for the life of the process, so wrapping
				 * each once instead of once per module (~25k of them) is pure win.
				 *
				 * A METHOD f-slot is not fixed. It holds a placeholder until the callee's module is
				 * instantiated ON THIS WORKER, and only then becomes the callee's `f`. Any module that
				 * imported that slot earlier cached the placeholder -- and the entry then never updates,
				 * so every later importer of that slot on that worker gets a function of the wrong type
				 * and fails INSTANTIATION with "imported function does not match the expected type".
				 *
				 * That is the whole of the direct-import failure, and it was diagnosed twice as something
				 * else. Round 132 read the residual 40 faults as a cross-worker admission race; Round 139
				 * read them as unsound imports across dependency CYCLES and concluded the fix was
				 * co-locating SCCs. The tell was in both write-ups and read as evidence for a cycle: ONE
				 * callee failing for FOUR separate importers. That is one poisoned cache entry, not four
				 * bad call sites. Instrumenting the failing admission settled it -- every dependency was
				 * admitted (state 2) and live on that worker, so the table was right and the reader was
				 * stale.
				 *
				 * So: keep the memo, and delete the entry wherever a slot is written below. */
				Module.__wjHelperCache = {};
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string" || !/^[0-9]+$/.test (k)) return undefined;
					var c = Module.__wjHelperCache;
					var f = c[k];
					if (f === undefined) { f = wasmTable.get (Number (k)); c[k] = f; }
					return f;
				} });
			}
			var inst = new WebAssembly.Instance (new WebAssembly.Module (b), { m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports && wasmExports["__cpp_exception"] }, s: { p: wasmExports && wasmExports["__stack_pointer"], l: $7, c: $8, v: $9, n: $10, d: $11, m: $12, b: $13 }, h: Module.__wjHelperImports });
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			if ($0 < 0) {
				wasmTable.set ($1, inst.exports.t); /* interp-entry thunk: scalar -> interpreter */
			} else {
				wasmTable.set ($0, inst.exports.e); /* entry thunk: interp entry */
				wasmTable.set ($1, inst.exports.f); /* scalar method: call_indirect target */
			}
			/* The import resolver memoises wasmTable.get; these two entries just changed. */
			if (Module.__wjHelperCache) { delete Module.__wjHelperCache[$0]; delete Module.__wjHelperCache[$1]; }
			return 1;
		} catch (e) {
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			if (eb) stringToUTF8 ("" + e, eb, $5); /* surface the WebAssembly error to the caller */
			return 0;
		}
	}, e_slot, f_slot, (int) (intptr_t) bytes, len, (int) (intptr_t) errbuf, errcap, (int) (intptr_t) out_ms,
	   (int) (intptr_t) &wj_slot_live, (int) (intptr_t) &wj_slot_live_cap,
	   (int) (intptr_t) mono_wasm_jit_vcall_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_vcall_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_delegate_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_delegate_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_scratch ());
	if (_ok) {
		/* Physical installation is not dispatch admission. A freshly compiled module can have unchecked
		 * direct dependencies that are still placeholders on this thread; only mono_wasm_jit_admit marks
		 * e/f live after recursively admitting the complete closure. */
		wj_mark_slot_installed (e_slot);
		wj_mark_slot_installed (f_slot);
		if (G_UNLIKELY (mono_wasm_jit_entry_census)) {
			mono_atomic_inc_i32 (&wj_census_inst_total);
			mono_atomic_add_i32 (&wj_census_inst_us_total, (gint32) (*out_ms * 1000.0));
		}
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
	/* Same >2GB pointer caveat as mono_wasm_jit_instantiate_local: a g_malloc buffer above 2GB arrives
	 * negative in JS, so re-add 2^32 before slicing.
	 *
	 * clang tokenises this body as C, so it must stay parseable as such: `var x = ...;` at statement
	 * level is tolerated, but a declaration in a for-initialiser (`for (var k = 0; ...)`) is not, and
	 * neither is `>>>`. Hence the hoisted `var` + while loop below. */
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
				/* THE CACHE IS A MEMO OF wasmTable.get AND MUST BE INVALIDATED WHEN THE TABLE CHANGES.
				 *
				 * It was safe while only HELPERS were imported: those are C functions in
				 * dotnet.native.wasm, at table indices fixed for the life of the process, so wrapping
				 * each once instead of once per module (~25k of them) is pure win.
				 *
				 * A METHOD f-slot is not fixed. It holds a placeholder until the callee's module is
				 * instantiated ON THIS WORKER, and only then becomes the callee's `f`. Any module that
				 * imported that slot earlier cached the placeholder -- and the entry then never updates,
				 * so every later importer of that slot on that worker gets a function of the wrong type
				 * and fails INSTANTIATION with "imported function does not match the expected type".
				 *
				 * That is the whole of the direct-import failure, and it was diagnosed twice as something
				 * else. Round 132 read the residual 40 faults as a cross-worker admission race; Round 139
				 * read them as unsound imports across dependency CYCLES and concluded the fix was
				 * co-locating SCCs. The tell was in both write-ups and read as evidence for a cycle: ONE
				 * callee failing for FOUR separate importers. That is one poisoned cache entry, not four
				 * bad call sites. Instrumenting the failing admission settled it -- every dependency was
				 * admitted (state 2) and live on that worker, so the table was right and the reader was
				 * stale.
				 *
				 * So: keep the memo, and delete the entry wherever a slot is written below. */
				Module.__wjHelperCache = {};
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string" || !/^[0-9]+$/.test (k)) return undefined;
					var c = Module.__wjHelperCache;
					var f = c[k];
					if (f === undefined) { f = wasmTable.get (Number (k)); c[k] = f; }
					return f;
				} });
			}
			var inst = new WebAssembly.Instance (new WebAssembly.Module (b), { m: { h: wasmMemory }, f: { f: wasmTable }, x: { e: wasmExports && wasmExports["__cpp_exception"] }, s: { p: wasmExports && wasmExports["__stack_pointer"], l: $8, c: $9, v: $10, n: $11, d: $12, m: $13, b: $14 }, h: Module.__wjHelperImports });
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			var k = 0;
			var ef = null;
			var ff = null;
			while (k < $2) {
				ef = inst.exports["e" + k];
				ff = inst.exports["f" + k];
				if (!ef || !ff) throw new Error ("batched module missing export e" + k);
				wasmTable.set (HEAP32[(es >> 2) + k], ef);
				wasmTable.set (HEAP32[(fs >> 2) + k], ff);
				/* Same invalidation as the single-method form: the resolver memoises wasmTable.get. */
				if (Module.__wjHelperCache) {
					delete Module.__wjHelperCache[HEAP32[(es >> 2) + k]];
					delete Module.__wjHelperCache[HEAP32[(fs >> 2) + k]];
				}
				k = k + 1;
			}
			return 1;
		} catch (e) {
			if (op) HEAPF64[op >> 3] = performance.now () - t0;
			if (eb) stringToUTF8 ("" + e, eb, $6);
			return 0;
		}
	}, (int) (intptr_t) e_slots, (int) (intptr_t) f_slots, n, (int) (intptr_t) bytes, len, (int) (intptr_t) errbuf, errcap, (int) (intptr_t) out_ms,
	   (int) (intptr_t) &wj_slot_live, (int) (intptr_t) &wj_slot_live_cap,
	   (int) (intptr_t) mono_wasm_jit_vcall_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_vcall_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_delegate_pic_ptr_addr (), (int) (intptr_t) mono_wasm_jit_delegate_pic_cap_addr (),
	   (int) (intptr_t) mono_wasm_jit_scratch ());
	if (_ok) {
		for (i = 0; i < n; i++) {
			wj_mark_slot_installed (e_slots [i]);
			wj_mark_slot_installed (f_slots [i]);
		}
		/* One MODULE, n methods — count it as one instantiation, because the WebAssembly.Instance and
		 * its export JSFunctions (the JSDispatchTable entries the renderer dies allocating) are what a
		 * batch amortizes. Counting n here would hide exactly the win batching is supposed to deliver. */
		if (G_UNLIKELY (mono_wasm_jit_entry_census)) {
			mono_atomic_inc_i32 (&wj_census_inst_total);
			mono_atomic_add_i32 (&wj_census_inst_us_total, (gint32) (*out_ms * 1000.0));
		}
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

typedef struct {
	int e, f, len;
	int body_len;       /* original single-method module size; retained across generational rebatches */
	void *bytes;
	MonoMethod *body_method;    /* method whose IR was emitted */
	MonoMethod *logical_method; /* wrapper/method whose InterpMethod publishes this descriptor */
	guint32 f_sig_id;
	guint8 no_gc;               /* transitive effect: body reaches no returning GC/safepoint */
	guint8 batch_incompatible;  /* force-compile cannot reproduce this descriptor's captured body */
	int ndeps;
	int *deps; /* immutable f-slot dependency list */
	/* Which of those deps this module reaches by IMPORT rather than by call_indirect, and therefore binds
	 * at INSTANTIATION rather than at call time. NULL when none, which is the default and the whole tier
	 * until MONO_WASM_JIT_DIRECT_IMPORT is on.
	 *
	 * The distinction is the difference between "should already be admitted" and "MUST already be
	 * admitted". A call_indirect resolves through the table whenever it runs, so admitting its target
	 * late is merely slow; an import is resolved once, while this module is being instantiated, and
	 * admitting its target late is a LinkError that loses the whole method. */
	guint8 *dep_strict;
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
} WjRegEntry;
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

int
mono_wasm_jit_register (MonoMethod *method, int e_slot, int f_slot, void *bytes, int len, guint32 f_sig_id, gboolean no_gc, const int *deps, const guint32 *dep_sig, MonoMethod *const *dep_methods, int ndeps, const int *strict_slots, int nstrict)
{
	int desc_id = 0;
	mono_loader_lock ();
	{
		int n = wj_reg_n;
		int ci = n / WJ_REG_CHUNK;
		int fci = f_slot > 0 ? f_slot / WJ_SLOT_CHUNK : -1;
		if (fci >= 0 && fci < WJ_SLOT_NCHUNKS && wj_fslot_desc_chunks [fci] &&
			wj_fslot_desc_chunks [fci][f_slot % WJ_SLOT_CHUNK] != 0) {
			printf ("WASM_JIT_FSLOT_COLLISION f=%d old_desc=%d new_method=%s — refusing slot reuse\n",
				f_slot, wj_fslot_desc_chunks [fci][f_slot % WJ_SLOT_CHUNK], method->name ? method->name : "?");
			mono_loader_unlock ();
			return 0;
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
			chunk [n % WJ_REG_CHUNK].f_sig_id = f_sig_id;
			chunk [n % WJ_REG_CHUNK].no_gc = no_gc ? 1 : 0;
			chunk [n % WJ_REG_CHUNK].ndeps = ndeps;
			if (ndeps > 0) {
				chunk [n % WJ_REG_CHUNK].deps = g_new (int, ndeps);
				memcpy (chunk [n % WJ_REG_CHUNK].deps, deps, sizeof (int) * ndeps);
				chunk [n % WJ_REG_CHUNK].dep_sig = g_new (guint32, ndeps);
				memcpy (chunk [n % WJ_REG_CHUNK].dep_sig, dep_sig, sizeof (guint32) * ndeps);
				chunk [n % WJ_REG_CHUNK].dep_method = g_new0 (MonoMethod *, ndeps);
				if (dep_methods)
					memcpy (chunk [n % WJ_REG_CHUNK].dep_method, dep_methods, sizeof (MonoMethod *) * ndeps);
				/* Which deps this module BINDS AT INSTANTIATION. Written HERE, inside the critical section,
				 * and not by a follow-up call after registration returns -- because the barrier and the
				 * wj_reg_n bump below are what make this entry visible to other workers, and an entry seen
				 * with dep_strict still NULL is an entry whose imports admission treats as loose. It would
				 * then allow the cycle-break, instantiate early, and bind the import against whatever the
				 * table holds -- a LinkError if the placeholder's type differs, and SILENTLY THE WRONG
				 * FUNCTION if it does not. Intermittent by construction, which is the worst kind. */
				if (nstrict > 0 && strict_slots) {
					int si, sk;
					chunk [n % WJ_REG_CHUNK].dep_strict = g_new0 (guint8, ndeps);
					for (si = 0; si < ndeps; ++si)
						for (sk = 0; sk < nstrict; ++sk)
							if (deps [si] == strict_slots [sk]) {
								chunk [n % WJ_REG_CHUNK].dep_strict [si] = 1;
								break;
							}
				}
			}
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
			 * (im->wasm_jit_* set) but NOT in wj_reg, so sync_thread can't pre-populate it;
			 * mono_wasm_jit_ensure_fslot's imethod fallback keeps it correct per direct call. Warn once. */
			static int _warned = 0;
			if (!_warned) { _warned = 1; printf ("WASM_JIT_REG_OVERFLOW: >%d JITted methods; sync_thread incomplete, relying on ensure_fslot imethod fallback\n", WJ_REG_NCHUNKS * WJ_REG_CHUNK); }
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
 * mono_wasm_jit_sync_thread instantiates EVERY registered module on EVERY thread, eagerly. Whether that
 * is waste depends on a number nothing currently reports: how many of those modules the worker goes on to
 * actually enter. It is not a cosmetic question. Each instantiation is a WebAssembly.Instance whose two
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
 * exhaustion events (see wj_table_exhausted), 4 = JIT_CALL table entries remaining (browser only).
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
	/* Census (MONO_WASM_JIT_ENTRYCENSUS=1). Fields 5-7 are THREAD-LOCAL and only meaningful on a thread
	 * you can actually evaluate in; 8-12 are process-wide atomics and are the ones to trust, because the
	 * pthread workers are parked in blocking wasm and CDP cannot evaluate in them at all.
	 *
	 * The headline is field 8 against field 2: total instantiations vs distinct methods registered. A
	 * ratio near 1.0 means each module is instantiated about once and there is no per-worker duplication
	 * to remove; N means every module is being instantiated on ~N threads, and each of those instances
	 * costs two export JSFunctions and therefore two JSDispatchTable entries. Field 9 (distinct
	 * (thread, method) pairs actually entered) is the lower bound on how many were genuinely needed --
	 * a lower bound, because direct JIT->JIT calls never cross the interp boundary where entry is
	 * counted. Fields 10-12 split the total by path, which is what says whether laziness would help:
	 * sync_thread is the eager sweep, admit and fslot are already demand-driven. */
	case 5: return wj_census_instantiated;
	case 6: return wj_census_entered;
	case 7: return (int) (wj_census_inst_us / 1000);        /* ms this thread spent instantiating */
	case 8: return wj_census_inst_total;                    /* ALL instantiations, all threads, all paths */
	case 9: return wj_census_entered_total;                 /* distinct (thread, e-slot) pairs entered */
	case 10: return wj_census_inst_admit;                   /* of which: via mono_wasm_jit_admit */
	case 11: return wj_census_inst_fslot;                   /* of which: via instantiate_fslot backstop */
	case 12: return wj_census_inst_sync;                    /* of which: via the eager sync_thread sweep */
	case 13: return wj_census_inst_us_total / 1000;         /* ms spent instantiating, all threads */
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
			char *oldn = mono_method_get_full_name (re->logical_method);
			char *newn = mono_method_get_full_name (logical_method);
			printf ("WASM_JIT_LOGICAL_REBIND desc=%d old=%s new=%s\n", desc_id, oldn, newn);
			g_free (oldn); g_free (newn);
		} else {
			re->logical_method = logical_method;
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

/* How many times one worker will defer a descriptor whose imported dependency was mid-DFS before giving up
 * and treating it as permanently unadmittable (which is the behaviour there was before imports existed:
 * the method runs interpreted). Deferrals are cheap -- the walk stops at the first unmet import -- but they
 * are paid per dispatch, so an unbreakable cycle must not retry forever. */
#define WJ_ADMIT_DEFER_MAX 16

/* Per-worker admission state. Generated direct calls contain no liveness checks: a root may enter only
 * after this DFS has installed its complete immutable direct-call closure in the worker's table. */
static __thread guint8 *wj_desc_state;
/* How many times this worker has DEFERRED admitting a descriptor because one of its imported
 * dependencies was mid-DFS (see WJ_ADMIT_DEFER_MAX). Per worker, like every other admission state. */
static __thread guint8 *wj_desc_defer;
static __thread guint32 *wj_desc_generation;
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
		wj_desc_defer = (guint8 *) g_realloc (wj_desc_defer, cap);
		wj_desc_generation = (guint32 *) g_realloc (wj_desc_generation, sizeof (guint32) * cap);
		memset (wj_desc_state + old, 0, cap - old);
		memset (wj_desc_defer + old, 0, cap - old);
		memset (wj_desc_generation + old, 0, sizeof (guint32) * (cap - old));
		wj_desc_state_cap = cap;
	}
}

int mono_wasm_jit_admit (int desc_id);

/* The slot-live bitmap answers only whether some generation has occupied this table slot. Automatic
 * rebatching deliberately reuses the same e/f slots, so root-entry fast paths must additionally verify
 * that THIS worker admitted the registry entry's current generation. */
WJ_KEEPALIVE int
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
 * callee first. Bounded by WJ_ADMIT_DEFER_MAX so a genuine unbreakable cycle stops paying for the walk.
 */
static int
wj_admit_dependencies (WjRegEntry *re, int desc_id, gboolean watch)
{
	int i;
	for (i = 0; i < re->ndeps; ++i) {
		int dep_id = wj_desc_for_fslot (re->deps [i]);
		WjRegEntry *dep = dep_id ? wj_reg_at (dep_id - 1) : NULL;
		if (watch)
			printf ("WASM_JIT_ADMIT_DEP parent=%d i=%d f=%d dep=%d state=%d e_live=%d f_live=%d\n",
				desc_id, i, re->deps [i], dep_id,
				dep_id > 0 && dep_id < wj_desc_state_cap ? wj_desc_state [dep_id] : -1,
				dep ? mono_wasm_jit_slot_live (dep->e) : 0, dep ? mono_wasm_jit_slot_live (dep->f) : 0);
		if (!dep_id || !dep || dep->f_sig_id != re->dep_sig [i]) {
			/* Disambiguate what the old print collapsed into "actual=0x0":
			 *  cause=fslot-unregistered  — the baked dep f-slot has NO registry entry at all (a slot that
			 *    was readable at emit time but whose registration never happened / was refused);
			 *  cause=sig-hash-mismatch   — the dep IS registered but its emitted ABI hash differs from what
			 *    the caller derived from the call-site signature (a real WasmCallInfo/self-sig divergence).
			 * dep_now_fslot = the callee's CURRENT published/reserved f-slot (from its imethod): if it is
			 * >0 and != dep_fslot the callee re-registered under a fresh slot after the caller baked the
			 * old one; 0/-1 means it never (re)registered. */
			extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
			MonoMethod *dm = re->dep_method ? re->dep_method [i] : NULL;
			char *cn = re->logical_method ? mono_method_get_full_name (re->logical_method) : NULL;
			char *dn = dm ? mono_method_get_full_name (dm) : NULL;
			printf ("WASM_JIT_ABI_MISMATCH desc=%d dep_fslot=%d expected=0x%x actual=0x%x cause=%s dep_desc=%d dep_now_fslot=%d caller=%s dep=%s\n",
				desc_id, re->deps [i], re->dep_sig [i], dep ? dep->f_sig_id : 0,
				!dep_id ? "fslot-unregistered" : (!dep ? "desc-chunk-missing" : "sig-hash-mismatch"),
				dep_id, dm ? mono_wasm_jit_get_callee_fslot (dm) : -1,
				cn ? cn : "?", dn ? dn : "?");
			g_free (cn); g_free (dn);
			return 0;
		}
		if (!mono_wasm_jit_admit (dep_id))
			return 0;
		/* mono_wasm_jit_admit returned 1; for a STRICT dep that is not enough -- it also returns 1 when
		 * it broke a cycle without instantiating. Only state 2 means the callee's exports are in this
		 * worker's table, which is what an import needs. */
		if (re->dep_strict && re->dep_strict [i] &&
		    !(dep_id < wj_desc_state_cap && wj_desc_state [dep_id] == 2)) {
			if (watch)
				printf ("WASM_JIT_ADMIT_DEFER desc=%d dep=%d f=%d (imported, not admitted yet)\n",
					desc_id, dep_id, re->deps [i]);
			return -1;
		}
	}
	return 1;
}

int
mono_wasm_jit_admit (int desc_id)
{
	WjRegEntry *re;
	WjBatchDesc *batch;
	int i;
	gboolean watch;
	char eb [192]; double ms = 0;
	if (desc_id <= 0 || desc_id > wj_reg_n)
		return 0;

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

	wj_desc_state_ensure (desc_id + 1);
	mono_memory_barrier ();
	re = wj_reg_at (desc_id - 1);
	if (!re)
		return 0;
	watch = mono_wasm_jit_watch && re->logical_method && re->logical_method->name &&
		strstr (re->logical_method->name, mono_wasm_jit_watch);
	if (watch)
		printf ("WASM_JIT_ADMIT_BEGIN desc=%d method=%s state=%d e=%d/live%d f=%d/live%d deps=%d\n",
			desc_id, re->logical_method->name, wj_desc_state [desc_id], re->e,
			mono_wasm_jit_slot_live (re->e), re->f, mono_wasm_jit_slot_live (re->f), re->ndeps);
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
		if (re->logical_method)
			mono_jiterp_wasm_jit_unpatch_interp_entry (mono_interp_get_imethod (re->logical_method));
#endif
		wj_desc_state [desc_id] = 0;
	}
	if (wj_desc_state [desc_id] == 1)
		return 1; /* dependency cycle within this admission DFS */
	if (wj_desc_state [desc_id] == 3)
		return 0;
	wj_desc_state [desc_id] = 1;
	batch = re->batch;
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
			if (wj_desc_generation [sibling] != batch->generation)
				wj_desc_state [sibling] = 0;
			if (wj_desc_state [sibling] == 3)
				goto fail;
			wj_desc_state [sibling] = 1;
			wj_desc_generation [sibling] = batch->generation;
		}
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			WjRegEntry *sre = wj_reg_at (sibling - 1);
			int r = wj_admit_dependencies (sre, sibling, watch && sibling == desc_id);
			if (r < 0)
				goto defer;
			if (!r)
				goto fail;
		}
	} else {
		int r = wj_admit_dependencies (re, desc_id, watch);
		if (r < 0)
			goto defer;
		if (!r)
			goto fail;
	}
	/* The compiling worker already installed this descriptor; other workers instantiate it once here.
	 * A batch member brings up the ENTIRE module (its exports are e<i>/f<i>, so there is no way to
	 * instantiate one member alone) — which also installs its siblings, so they skip this. */
	if (!wj_slot_is_installed (re->e) || !wj_slot_is_installed (re->f) ||
	    wj_desc_generation [desc_id] != re->generation) {
		eb [0] = 0;
		if (re->batch) {
			extern int mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n, const void *bytes, int len, char *errbuf, int errcap, double *out_ms);
			if (!mono_wasm_jit_instantiate_batch_local (re->batch->e, re->batch->f, re->batch->n,
			                                            re->batch->bytes, re->batch->len, eb, (int) sizeof (eb), &ms)) {
				printf ("WASM_JIT_ADMIT_FAIL desc=%d (batch n=%d) e=%d f=%d : %s\n", desc_id, re->batch->n, re->e, re->f, eb);
				goto fail;
			}
		} else if (!mono_wasm_jit_instantiate_local (re->e, re->f, re->bytes, re->len, eb, (int) sizeof (eb), &ms)) {
			printf ("WASM_JIT_ADMIT_FAIL desc=%d e=%d f=%d : %s\n", desc_id, re->e, re->f, eb);
			/* A LinkError here should be impossible for an imported dependency: the strict-dep check
			 * above refuses to instantiate until every one of them is admitted on THIS worker. Dump what
			 * it actually saw, because the difference between "the dep was not marked strict", "it was
			 * marked and reported admitted", and "the slot is not in the dependency list at all" needs
			 * three different fixes and the message above distinguishes none of them. */
			{
				int k;
				printf ("WASM_JIT_ADMIT_FAIL_DEPS desc=%d ndeps=%d strict=%s [", desc_id, re->ndeps,
					re->dep_strict ? "yes" : "NONE");
				for (k = 0; k < re->ndeps; ++k)
					printf ("%s%d%s/state%d/live%d", k ? " " : "", re->deps [k],
						(re->dep_strict && re->dep_strict [k]) ? "S" : "",
						wj_desc_for_fslot (re->deps [k]) > 0 &&
						wj_desc_for_fslot (re->deps [k]) < wj_desc_state_cap
							? wj_desc_state [wj_desc_for_fslot (re->deps [k])] : -1,
						mono_wasm_jit_slot_live (re->deps [k]));
				printf ("]\n");
			}
			goto fail;
		}
		/* Path tag only; the total is counted inside instantiate_*_local. This is THE per-worker path --
		 * already demand-driven at dispatch, so a high count here is not by itself removable waste. */
		if (G_UNLIKELY (mono_wasm_jit_entry_census)) {
			wj_census_instantiated++;
			wj_census_inst_us += (gint64) (ms * 1000.0);
			mono_atomic_inc_i32 (&wj_census_inst_admit);
		}
	}
	/* Publish dispatchability only after every unchecked direct dependency is admitted. */
	wj_mark_slot_live (re->e);
	wj_mark_slot_live (re->f);
	wj_desc_state [desc_id] = 2;
	wj_desc_generation [desc_id] = re->generation;
	if (re->batch) {
		/* One instantiation installed every export. Mark the siblings now so later dependency/root
		 * admission does not instantiate the same module once per member on this worker. */
		for (i = 0; i < re->batch->n; ++i) {
			int sibling = re->batch->desc [i];
			if (sibling <= 0)
				continue;
			wj_desc_state_ensure (sibling + 1);
			wj_desc_state [sibling] = 2;
			wj_desc_generation [sibling] = re->batch->generation;
			wj_mark_slot_live (re->batch->e [i]);
			wj_mark_slot_live (re->batch->f [i]);
#ifdef HOST_BROWSER
			/* Patch EVERY sibling, not just the descriptor that triggered this admission.
			 * mono_wasm_jit_batch_bind unpatches all n members before rebinding them (their e/f slots are
			 * reused by the new generation), so repatching only the triggering member strands the other
			 * n-1 on the guarded interp-entry trampoline for the rest of the process. At the batch sizes
			 * this planner produces (211 members on the jbox2d workload) that is the difference between
			 * ~0% and ~12% of steady-state cycles spent in interp_entry. */
			{
				WjRegEntry *sre = (sibling <= wj_reg_n) ? wj_reg_at (sibling - 1) : NULL;
				if (sre && sre->logical_method)
					mono_jiterp_wasm_jit_patch_interp_entry (mono_interp_get_imethod (sre->logical_method));
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
	if (!re->batch && re->logical_method)
		mono_jiterp_wasm_jit_patch_interp_entry (mono_interp_get_imethod (re->logical_method));
#endif
	if (watch) printf ("WASM_JIT_ADMIT_OK desc=%d e_live=%d f_live=%d\n", desc_id, mono_wasm_jit_slot_live (re->e), mono_wasm_jit_slot_live (re->f));
	wj_desc_defer [desc_id] = 0;   /* it got in; forget any earlier deferrals */
	return 1;
defer:
	/* NOT a failure: an imported dependency is mid-DFS on this worker, so instantiating now would bind an
	 * import against a slot that still holds the guarded interp-entry trampoline. Roll back to UNVISITED
	 * so the next dispatch tries again -- from a different entry point, which is usually the other end of
	 * the cycle and admits the callee first. Until then this method runs interpreted, which is exactly
	 * where it would have ended up anyway, minus the LinkError.
	 *
	 * Bounded, because a genuinely unbreakable cycle would otherwise pay for the DFS at every dispatch
	 * forever. After WJ_ADMIT_DEFER_MAX attempts it becomes an ordinary permanent failure, i.e. the
	 * pre-import behaviour. */
	if (batch) {
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			if (sibling > 0 && sibling < wj_desc_state_cap &&
			    wj_desc_generation [sibling] == batch->generation && wj_desc_state [sibling] == 1)
				wj_desc_state [sibling] = 0;
		}
	}
	if (G_UNLIKELY (mono_wasm_jit_stats))
		mono_wasm_jit_count (WJC_ADMIT_DEFERRED);
	if (wj_desc_defer [desc_id] < WJ_ADMIT_DEFER_MAX) {
		wj_desc_defer [desc_id]++;
		wj_desc_state [desc_id] = 0;
		return 0;
	}
	printf ("WASM_JIT_ADMIT_DEFER_GIVEUP desc=%d e=%d f=%d after %d attempts (imported dependency never admitted)\n",
		desc_id, re->e, re->f, WJ_ADMIT_DEFER_MAX);
	/* fall through to the permanent path */
fail:
	if (batch) {
		for (i = 0; i < batch->n; ++i) {
			int sibling = batch->desc [i];
			if (sibling > 0 && sibling < wj_desc_state_cap &&
			    wj_desc_generation [sibling] == batch->generation)
				wj_desc_state [sibling] = 3;
		}
	} else {
		wj_desc_state [desc_id] = 3;
	}
	return 0;
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
		if (re->batch
		    ? !mono_wasm_jit_instantiate_batch_local (re->batch->e, re->batch->f, re->batch->n,
		                                               re->batch->bytes, re->batch->len,
		                                               eb, (int) sizeof (eb), &ms)
		    : !mono_wasm_jit_instantiate_local (re->e, re->f, re->bytes, re->len,
		                                        eb, (int) sizeof (eb), &ms)) {
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
		/* Per-path attribution: this is the EAGER sweep, the one a lazy scheme would remove. The totals
		 * themselves are counted at the instantiate_*_local choke points, so this only tags the path. */
		if (G_UNLIKELY (mono_wasm_jit_entry_census)) {
			wj_census_instantiated++;
			wj_census_inst_us += (gint64) (ms * 1000.0);
			mono_atomic_inc_i32 (&wj_census_inst_sync);
		}
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
				if (mono_wasm_jit_slot_live (fslot)) {
					if (G_UNLIKELY (mono_wasm_jit_entry_census)) {
						wj_census_instantiated++;
						wj_census_inst_us += (gint64) (ms * 1000.0);
						mono_atomic_inc_i32 (&wj_census_inst_fslot);
					}
					return 1;
				}
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
	/* MONO_TYPE_GENERICINST covers both closed generic reference types and generic value types.
	 * The switch below intentionally leaves value types to WasmCallInfo's scalar/by-address ABI,
	 * but reference instantiations (MH<object>, IEnumerable<T>, Comparer<T>, ...) are ordinary
	 * wasm32 managed references. Omitting this check caused virtually every hot IKVM __<>MHC caller
	 * to bail "call ret type" when a helper returned an IKVM.Runtime.MH<...> delegate. */
	if (MONO_TYPE_IS_REFERENCE (t))
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
	/* float -> int (saturating trunc; IREG dest per mini-ops.h). _TO_I is native int = i32 on wasm32. */
	case OP_RCONV_TO_I4: case OP_FCONV_TO_I4: case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
	case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
	case OP_RCONV_TO_I1: case OP_FCONV_TO_I1: case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
	case OP_RCONV_TO_I2: case OP_FCONV_TO_I2: case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
	case OP_LZCNT32: case OP_POPCNT32:
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
	case OP_RCONV_TO_I8: case OP_FCONV_TO_I8: case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:  /* float -> i64 */
	case OP_LZCNT64: case OP_POPCNT64:
		return WASM_I64;
	case OP_R8CONST: case OP_FMOVE:
	case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
	case OP_ICONV_TO_R8: case OP_RCONV_TO_R8:
	case OP_LCONV_TO_R8:   /* i64 -> f64 */
	case OP_MOVE_I8_TO_F:  /* i64 bits -> f64 (reinterpret) */
	/* double math intrinsics (the F-suffixed / R-prefixed forms below are the f32 ones) */
	case OP_ABS: case OP_SQRT: case OP_CEIL: case OP_FLOOR: case OP_TRUNC: case OP_ROUND:
	case OP_FMIN: case OP_FMAX: case OP_FCOPYSIGN:
		return WASM_F64;
	case OP_R4CONST: case OP_RMOVE:
	case OP_FCONV_TO_R4:
	case OP_ICONV_TO_R4: case OP_RCONV_TO_R4:
	case OP_LCONV_TO_R4:   /* i64 -> f32 */
	case OP_MOVE_I4_TO_F:  /* i32 bits -> f32 (reinterpret) */
	case OP_RADD: case OP_RSUB: case OP_RMUL: case OP_RDIV: case OP_RNEG:
	/* float32 math intrinsics */
	case OP_ABSF: case OP_SQRTF: case OP_CEILF: case OP_FLOORF: case OP_TRUNCF:
	case OP_RMIN: case OP_RMAX: case OP_RCOPYSIGN:
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

/* TRUE unless this instruction PROVABLY cannot trigger a GC while it executes (MONO_WASM_JIT_SLOTLIVE).
 * DEFAULT-TRUE: anything not on the allow-list below is assumed to be able to reach a safepoint —
 * an under-inclusive "pure" list only costs slot-elision coverage, an over-inclusive one is silent
 * corruption. GC points include every call family (direct, residual, f-slot, icall, vcall IC — the
 * callee can allocate and trigger a moving collection), OP_GC_SAFE_POINT (the cooperative STW poll),
 * OP_THROW/OP_RETHROW and OP_COND_EXC_* (raising enters the runtime, which allocates the exception),
 * integer div/rem (inline DivideByZero/Overflow raise), OP_CALL_HANDLER/OP_ENDFINALLY, and atomics.
 * The allow-list is the emitter's inline-lowered pure population: const/move/arith/logic/shift,
 * conversions (a wasm trunc can TRAP, but a trap is not a GC point), compares + branches, the typed
 * MEMBASE loads/stores (wasm linear memory doesn't fault-signal; mono's null checks are separate
 * explicit COND_EXC instructions), OP_LDADDR (pure address arithmetic), OP_SETRET, and the emitter's
 * known no-op set (seq points, GC liveness annotations, START_HANDLER). */
/* corlib exception id for an OP_COND_EXC_* name, as mono_wasm_jit_raise_corlib expects it; -1 if we
 * don't lower that one. Was an inline if-chain at the COND_EXC site; factored out so the shared-throw
 * prescan classifies exactly the same set the emitter does. */
static int
wj_exc_id_for_name (const char *en)
{
	if (!en) return -1;
	if (!strcmp (en, "OverflowException")) return 0;
	if (!strcmp (en, "DivideByZeroException")) return 1;
	if (!strcmp (en, "IndexOutOfRangeException")) return 2;
	if (!strcmp (en, "InvalidCastException")) return 3;
	if (!strcmp (en, "NullReferenceException")) return 4;
	if (!strcmp (en, "ArithmeticException")) return 5;
	if (!strcmp (en, "ArrayTypeMismatchException")) return 6;
	return -1;
}

#define WJ_EXC_IDS 7

/* The instructions MONO_WASM_JIT_RAISE_NOGC exempts: everything whose only way to reach the collector is
 * by raising, i.e. by allocating an exception and never returning to this frame. Factored out of
 * wj_ins_is_gcpoint so the two classes of consumer can disagree about them -- see the level-1 argument
 * there. OP_CHECK_THIS and OP_[L]DIV/REM are deliberately absent: those are handled elsewhere or raise
 * through paths this set does not describe. */
static gboolean
wj_ins_is_raise (MonoInst *ins)
{
	switch (ins->opcode) {
	case OP_THROW: case OP_RETHROW:
	case OP_COND_EXC_EQ: case OP_COND_EXC_NE_UN: case OP_COND_EXC_LT: case OP_COND_EXC_LT_UN:
	case OP_COND_EXC_GT: case OP_COND_EXC_GT_UN: case OP_COND_EXC_LE: case OP_COND_EXC_LE_UN:
	case OP_COND_EXC_GE: case OP_COND_EXC_GE_UN:
	case OP_COND_EXC_IEQ: case OP_COND_EXC_INE_UN: case OP_COND_EXC_ILT: case OP_COND_EXC_ILT_UN:
	case OP_COND_EXC_IGT: case OP_COND_EXC_IGT_UN: case OP_COND_EXC_ILE: case OP_COND_EXC_ILE_UN:
	case OP_COND_EXC_IGE: case OP_COND_EXC_IGE_UN:
	case OP_COND_EXC_OV: case OP_COND_EXC_NO: case OP_COND_EXC_C: case OP_COND_EXC_NC:
	case OP_COND_EXC_IOV: case OP_COND_EXC_INO: case OP_COND_EXC_IC: case OP_COND_EXC_INC:
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
wj_ins_is_gcpoint (MonoInst *ins, gboolean raise_exempt)
{
	/* MONO_WASM_JIT_RAISE_NOGC. A raise is not a GC point in the sense SLOTLIVE cares about.
	 *
	 * SLOTLIVE's question is "can the collector observe this vreg HERE", which only matters for values
	 * that are still needed afterwards. A raising instruction has exactly two outcomes: it falls through
	 * without allocating anything (OP_COND_EXC_* on the common path), or it raises and NEVER RETURNS to
	 * this frame. Nothing in this method reads a vreg after the raise, so nothing has to survive it.
	 *
	 * The catch is a handler in THIS method: with a clause, the catch landing pad resumes into a handler
	 * bb that does read those vregs, so the raise really is a point the collector must see them at.
	 * Hence the clause_free gate -- keyed off cfg->header->num_clauses rather than the emitter's eh_on,
	 * because num_clauses is the property the argument actually rests on.
	 *
	 * Objects reachable only from this frame's unscanned wasm locals can therefore be collected while
	 * the exception is being allocated. That is fine: the frame is being torn down and will never
	 * dereference them again. Anything still reachable from a scanned root (a field, an array, an outer
	 * frame) is unaffected.
	 *
	 * This is the one change in this series where a mistake is silent heap corruption rather than a
	 * crash, so it is default OFF until soaked with REFVERIFY=2 + OBJGUARD=1 + STOREGUARD=1. */
	/* MEASURED UNSOUND: this exemption loses references. Keep MONO_WASM_JIT_RAISE_NOGC=0 (the default).
	 *
	 * With the nursery shrunk to 4m so that collections are frequent enough to expose it, booting
	 * ikvmcraft / MCSR Ranked 1.16.1 gives:
	 *     default (RAISE_NOGC=1)      4/4 boots dead, "unexpected GC filler class" in ~25s
	 *     MONO_WASM_JIT_RAISE_NOGC=0  0/4
	 *     MONO_WASM_JIT_SLOTLIVE=0    0/4      (RAISE_NOGC has no effect except through SLOTLIVE)
	 *     MONO_WASM_JIT_AUTO=0        0/4
	 *     MONO_WASM_JIT_SLOTZERO=0    3/4      negative control: a different elision knob does NOT help
	 * MONO_CLASS_GC_FILLER is what sgen refills reclaimed nursery memory with (class-init.c:4227), so
	 * the receiver really had been collected out from under a live reference.
	 *
	 * The blame is the OP_COND_EXC_* arm, not OP_THROW. Dropping only OP_THROW/OP_RETHROW from this
	 * switch was built and measured and did NOT help (3/4 still dead), which stands to reason: explicit
	 * throws are rare, whereas the implicit null/bounds/overflow checks are everywhere, so they are what
	 * actually drives effective_gcp_count down -- and that count gates lazy ref-frame materialization
	 * (`effective_gcp_count == 1` => lazy_ref_frame) as well as method_nogc. A raise on the taken path
	 * allocates the exception, which can collect before the lazily materialized frame exists, and then
	 * nothing in that frame is scannable.
	 *
	 * Why this workload and not the jbox2d kernel the series was tuned against: it throws constantly.
	 * IKVM's class-loading probes alone raise ~8.8k NPEs per boot inside
	 * sun.misc.URLClassPath.Loader.findResource, each caught by that method's own catch(Exception).
	 *
	 * THE FIX (level 1, plus the taken-arm materialization below). Two independent holes were closed:
	 *
	 *   a. The "never returns to this frame" argument is about THIS FRAME. It says nothing about
	 *      method_nogc / effective_gcp_count / prior_gcps_are_polls / terminal_vcall_handoff, which all
	 *      publish "no collection happens across here" to OTHER code -- the caller that elides a root
	 *      across a direct call, the handoff that gives its roots to a callee. A raise allocates, so a
	 *      raising instruction is a collection point for every one of those consumers. Level 1 keeps the
	 *      exemption only for gc_gen/needs_slot, the one consumer whose question really is frame-local.
	 *   b. Even frame-locally, the exemption drives effective_gcp_count to 1 and so turns on
	 *      lazy_ref_frame; a raise reached BEFORE the one real GC point then allocates with no frame
	 *      materialized, and the slots that do exist are nowhere the collector can see. Every raise site
	 *      now emits ENSURE_REF_FRAME inside its taken arm (WJ_THROW_BR's shared blocks and LDIV_RAISE),
	 *      exactly as the conditional safepoint poll already does. The hot not-taken path is unchanged.
	 *
	 * Level 2 is the original all-consumers behaviour, retained only so the two can be A/B'd; it must be
	 * judged on failure rate under GC stress -- not on a clean run with OBJGUARD/STOREGUARD, because
	 * those disable SLOTLIVE (mini-wasm.c:5218) and therefore cannot exercise it at all.
	 *
	 * BOTH LEVELS MEASURED STILL BROKEN (4/4 dead each, 4 MB nursery, against a positive control that
	 * reproduces on demand). So neither hole above was the cause, and the frame-local exemption -- not
	 * bumping gc_gen at a raise -- is the unsound part on its own.
	 *
	 * MECHANISM (measured): the exemption removes an accidental pin.
	 * WJ_SL_USE lets a forwarding virtual call's argument skip the used-at-a-GC-point clause (`_fwd`,
	 * wj_ins_is_pinned_vcall_forward) because the callee is supposed to take ownership of the root.
	 * Normally the span clause still fires -- a raise counts as a GC point, bumps gc_gen, and the
	 * argument gets a slot anyway. Exempt the raise and BOTH clauses go quiet: the argument has no slot
	 * anywhere, and the handoff guarantee is all that is left. It does not hold: every failure lands in
	 * mini_llvmonly_init_vtable_slot -> resolve_vcall, the PIC MISS path, at the resolve step, on a
	 * receiver that is already a nursery filler -- before the miss frame has pinned anything.
	 * Disabling the classification (VCALL_INLINE_IC=0 or VCALL_SHARED_MISS=0) makes level 2 clean.
	 * Fixed in WJ_SL_USE by honouring `_fwd` only while no raise has been exempted in the current
	 * generation; see gen_skipped_raises.
	 *
	 * The issue is a shared rule rather than a single bad method; see scratchpad/mcsr/FINDINGS.md
	 * Finding 18 for the measurement record. */
	if (raise_exempt && wj_ins_is_raise (ins))
		return FALSE;

	switch (ins->opcode) {
	/* constants + moves (incl. STACK_OBJ ICONST: a pure literal-table i32.load) */
	case OP_ICONST: case OP_I8CONST: case OP_R4CONST: case OP_R8CONST:
	case OP_MOVE: case OP_LMOVE: case OP_FMOVE: case OP_RMOVE:
	case OP_MOVE_F_TO_I4: case OP_MOVE_I4_TO_F: case OP_MOVE_F_TO_I8: case OP_MOVE_I8_TO_F:
	/* integer arithmetic/logic/shift — div/rem deliberately ABSENT (they raise) */
	case OP_IADD: case OP_ISUB: case OP_IMUL: case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN: case OP_INEG: case OP_INOT:
	case OP_IADD_IMM: case OP_ISUB_IMM: case OP_IMUL_IMM: case OP_IAND_IMM: case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_ADD_IMM: case OP_SUB_IMM: case OP_MUL_IMM: case OP_AND_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_LADD: case OP_LSUB: case OP_LMUL: case OP_LAND: case OP_LOR: case OP_LXOR:
	case OP_LSHL: case OP_LSHR: case OP_LSHR_UN: case OP_LNEG: case OP_LNOT:
	case OP_LADD_IMM: case OP_LSUB_IMM: case OP_LMUL_IMM: case OP_LAND_IMM: case OP_LOR_IMM: case OP_LXOR_IMM:
	case OP_LSHL_IMM: case OP_LSHR_IMM: case OP_LSHR_UN_IMM:
	/* float arithmetic (float div doesn't raise) */
	case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
	case OP_RADD: case OP_RSUB: case OP_RMUL: case OP_RDIV: case OP_RNEG:
	/* conversions */
	case OP_ICONV_TO_I1: case OP_ICONV_TO_U1: case OP_ICONV_TO_I2: case OP_ICONV_TO_U2:
	case OP_ICONV_TO_R4: case OP_ICONV_TO_R8:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4: case OP_LCONV_TO_R4: case OP_LCONV_TO_R8:
	case OP_FCONV_TO_R4: case OP_RCONV_TO_R4: case OP_RCONV_TO_R8:
	/* float -> int (saturating trunc): a number, never a managed pointer */
	case OP_RCONV_TO_I4: case OP_FCONV_TO_I4: case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
	case OP_RCONV_TO_I8: case OP_FCONV_TO_I8: case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:
	case OP_RCONV_TO_I1: case OP_FCONV_TO_I1: case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
	case OP_RCONV_TO_I2: case OP_FCONV_TO_I2: case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
	case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
	/* float math + bit intrinsics: likewise pure numbers */
	case OP_ABS: case OP_SQRT: case OP_CEIL: case OP_FLOOR: case OP_TRUNC: case OP_ROUND:
	case OP_ABSF: case OP_SQRTF: case OP_CEILF: case OP_FLOORF: case OP_TRUNCF:
	case OP_FMIN: case OP_FMAX: case OP_RMIN: case OP_RMAX: case OP_FCOPYSIGN: case OP_RCOPYSIGN:
	case OP_LZCNT32: case OP_LZCNT64: case OP_POPCNT32: case OP_POPCNT64:
	case OP_SEXT_I4: case OP_ZEXT_I4:
	/* compares + setcc + branches */
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
	case OP_COMPARE: case OP_ICOMPARE: case OP_LCOMPARE: case OP_FCOMPARE: case OP_RCOMPARE:
	case OP_COMPARE_IMM: case OP_ICOMPARE_IMM: case OP_LCOMPARE_IMM:
	case OP_BR:
	case OP_IBEQ: case OP_IBNE_UN: case OP_IBLT: case OP_IBLT_UN: case OP_IBGT: case OP_IBGT_UN:
	case OP_IBLE: case OP_IBLE_UN: case OP_IBGE: case OP_IBGE_UN:
	case OP_LBEQ: case OP_LBNE_UN: case OP_LBLT: case OP_LBLT_UN: case OP_LBGT: case OP_LBGT_UN:
	case OP_LBLE: case OP_LBLE_UN: case OP_LBGE: case OP_LBGE_UN:
	case OP_FBEQ: case OP_FBNE_UN: case OP_FBLT: case OP_FBLT_UN: case OP_FBGT: case OP_FBGT_UN:
	case OP_FBLE: case OP_FBLE_UN: case OP_FBGE: case OP_FBGE_UN:
	case OP_RBEQ: case OP_RBNE_UN: case OP_RBLT: case OP_RBLT_UN: case OP_RBGT: case OP_RBGT_UN:
	case OP_RBLE: case OP_RBLE_UN: case OP_RBGE: case OP_RBGE_UN:
	/* typed memory accesses (null checks are separate explicit COND_EXC instructions) */
	case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
	case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
	case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
	case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
	case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
	case OP_LDADDR:
	case OP_SETRET:
	/* the emitter's no-op set */
	case OP_NOP: case OP_IL_SEQ_POINT: case OP_SEQ_POINT:
	/* OP_NOT_NULL emits nothing (a fact marker, see its emit case), so it can neither reach a GC nor
	 * write memory. It must be listed: this classifier is an inverted whitelist ending in
	 * `default: return TRUE`, and MONO_EMIT_NULL_CHECK now emits an OP_NOT_NULL at every null check.
	 * Left to the default it would make every null check a GC point, forcing a frame slot for every ref
	 * live across one and undoing most of what MONO_WASM_JIT_SLOTLIVE elides. Note OP_CHECK_THIS is
	 * deliberately NOT here -- it really can raise. */
	case OP_NOT_NULL:
	case OP_DUMMY_USE: case OP_NOT_REACHED: case OP_START_HANDLER:
	case OP_GC_LIVENESS_DEF: case OP_GC_LIVENESS_USE:
	case OP_GC_PARAM_SLOT_LIVENESS_DEF: case OP_GC_SPILL_SLOT_LIVENESS_DEF:
		return FALSE;
	default:
		return TRUE;
	}
}

/* Bump a WJC counter only when MONO_WASM_JIT_STATS is on. The counters are atomics, and these sit in
 * the per-instruction emit path where an unconditional atomic is measurable on compile time. */
static void
wj_count (int idx)
{
	if (G_UNLIKELY (mono_wasm_jit_stats))
		mono_wasm_jit_count (idx);
}

/*
 * MONO_WASM_JIT_LCSE — redundant heap-load elimination, scoped to an extended basic block.
 *
 * Two kinds of fact are tracked:
 *   LOAD  : the value of *(base + off), read with a given wasm load opcode, lives in vreg `vreg`.
 *   ALIAS : `from` holds the same value as `to`, so a lookup keyed on `from` can use `to` instead.
 *
 * ALIAS is what makes CHAINED reloads work, and it is the whole reason this is not just a bb-local
 * peephole. For `a.lowerBound.x` reloaded in a ternary arm, eliding the outer load leaves the arm's
 * `.x` load keyed on the elided load's dreg rather than on the cached vreg; without canonicalising
 * through ALIAS the second load misses and only half the redundancy goes away.
 *
 * Deliberately small and linearly scanned: the win is a handful of hot locations per extended block,
 * not a big table, and fixed arrays cost no allocation per bb.
 */
/*
 * LOADs and ALIASes live in SEPARATE arrays, and both evict the OLDEST entry when full rather than
 * refusing the newest. Sharing one 16-entry table let a dense run of scalar moves (mono's call-arg
 * setup and decompose emit them freely) fill it with alias entries before a load could get in, and a
 * full table then dropped adds silently.
 *
 * MEASURED: that was NOT the reason reach was low. With the split in place, WJC_LCSE_EVICT is 0 on the
 * jbox2d bench -- the table is never full -- and reach is 54 hits / 7830 loads = 0.69%. 4633 loads DO
 * get cached, so the entries are being created and then dying before anything matches them. The two
 * structural reasons, both inherent to the EBB scope rather than to any table size:
 *   - the whole table is cleared at every call (wj_ins_is_gcpoint's inverted whitelist is default-TRUE),
 *     and these hot loops are call-dense;
 *   - a merge block has in_count > 1 and therefore starts empty, which is exactly where the reload in
 *     the motivating `a < b ? a : b` shape sits.
 * Fixing reach means a real GVN over the dominator tree with per-field memory dependence (kill only on
 * stores/calls that can write THAT field), not a bigger table. Until then this pass is correct and
 * nearly inert, and the 39-vs-14 heap-load gap against teavm stands unaddressed.
 */
#define WJ_LCSE_LOADS   16
#define WJ_LCSE_ALIASES 16
typedef struct {
	int wop;       /* the EMITTED wasm opcode + alignment, NOT the mono opcode: OP_LOAD_MEMBASE,     */
	int align;     /* OP_LOADI4_MEMBASE and OP_LOADU4_MEMBASE are three keys for one i32.load, so    */
	               /* keying on the mono opcode makes a ref field and an int field at the same       */
	               /* address miss each other. Width and signedness stay distinct because they are   */
	               /* distinct wasm opcodes.                                                          */
	int base;      /* base vreg (already canonical) */
	gint32 off;    /* byte offset */
	int vreg;      /* the vreg that holds the loaded value */
} WjLcseLoad;
typedef struct {
	int from;      /* reads of this vreg may use ... */
	int to;        /* ... this one instead */
} WjLcseAlias;
typedef struct {
	WjLcseLoad ld [WJ_LCSE_LOADS];
	WjLcseAlias al [WJ_LCSE_ALIASES];
	int nld, nal;
} WjLcse;

static int
wj_lcse_canon (WjLcse *t, int v)
{
	int guard;
	if (!t || v < 0)
		return v;
	/* Chains are built by appending and are short; the bound is a hard stop against a cycle, not an
	 * expected depth. */
	for (guard = 0; guard < WJ_LCSE_ALIASES; ++guard) {
		int i, next = -1;
		for (i = 0; i < t->nal; ++i)
			if (t->al [i].from == v) { next = t->al [i].to; break; }
		if (next < 0 || next == v)
			break;
		v = next;
	}
	return v;
}

/*
 * Drop every fact invalidated by a write to V's storage. Called on every def BEFORE the defining
 * instruction is lowered, so the instruction's own sources still resolve against the old state.
 *
 * Invalidation is by STORAGE, not by vreg number. Under MONO_WASM_JIT_COALESCE several vregs share one
 * wasm local, so defining any of them overwrites the cached value of all of them -- and the coalescing
 * liveness cannot have accounted for that, because LCSE's read of a cached vreg is created during
 * EMISSION, after the IR-derived liveness that chose the sharing has already run. Comparing li[] closes
 * that gap exactly, and cannot over-kill: each valtype group owns a disjoint range of local indices, so
 * two vregs share a local only if they really do share storage.
 */
static void
wj_lcse_kill (WjLcse *t, const int *li, int nvreg, int v)
{
	int i, k, lv;
	if (!t || v < 0)
		return;
	lv = (li && v < nvreg) ? li [v] : -1;
#define WJ_LCSE_DEAD(x) ((x) == v || (lv >= 0 && (x) >= 0 && (x) < nvreg && li [(x)] == lv))
	for (i = 0, k = 0; i < t->nld; ++i)
		if (!WJ_LCSE_DEAD (t->ld [i].base) && !WJ_LCSE_DEAD (t->ld [i].vreg))
			t->ld [k++] = t->ld [i];
	t->nld = k;
	for (i = 0, k = 0; i < t->nal; ++i)
		if (!WJ_LCSE_DEAD (t->al [i].from) && !WJ_LCSE_DEAD (t->al [i].to))
			t->al [k++] = t->al [i];
	t->nal = k;
#undef WJ_LCSE_DEAD
}

static void
wj_lcse_add_load (WjLcse *t, int wop, int align, int base, gint32 off, int vreg)
{
	/* base == vreg would be self-referential: `t = load(t, off)` redefines its own base, and caching
	 * (t,off) -> t would then hand out the loaded value as if it were the address. */
	if (!t || base < 0 || vreg < 0 || base == vreg)
		return;
	if (t->nld >= WJ_LCSE_LOADS) {
		memmove (&t->ld [0], &t->ld [1], sizeof (t->ld [0]) * (WJ_LCSE_LOADS - 1));
		t->nld = WJ_LCSE_LOADS - 1;
	}
	t->ld [t->nld].wop = wop;
	t->ld [t->nld].align = align;
	t->ld [t->nld].base = base;
	t->ld [t->nld].off = off;
	t->ld [t->nld].vreg = vreg;
	t->nld++;
}

static void
wj_lcse_add_alias (WjLcse *t, int from, int to)
{
	if (!t || from < 0 || to < 0 || from == to)
		return;
	if (t->nal >= WJ_LCSE_ALIASES) {
		memmove (&t->al [0], &t->al [1], sizeof (t->al [0]) * (WJ_LCSE_ALIASES - 1));
		t->nal = WJ_LCSE_ALIASES - 1;
	}
	t->al [t->nal].from = from;
	t->al [t->nal].to = to;
	t->nal++;
}

static int
wj_lcse_find (WjLcse *t, int wop, int align, int base, gint32 off)
{
	int i;
	if (!t)
		return -1;
	for (i = 0; i < t->nld; ++i)
		if (t->ld [i].wop == wop && t->ld [i].align == align && t->ld [i].base == base && t->ld [i].off == off)
			return t->ld [i].vreg;
	return -1;
}

/* Could this instruction change memory that a cached load already read? */
static gboolean
wj_ins_clobbers_mem (MonoInst *ins)
{
	/* Stores write it; OP_LDADDR hands its address to code we cannot see. */
	if (MONO_IS_STORE_MEMBASE (ins) || ins->opcode == OP_LDADDR)
		return TRUE;
	/* A raise never returns, so nothing later in this method observes anything it did. Decided here
	 * rather than by asking wj_ins_is_gcpoint with clause_free=TRUE, because that answer is gated on
	 * MONO_WASM_JIT_RAISE_NOGC — which is about GC visibility, not memory effects — and LCSE's
	 * effectiveness should not depend on an unrelated flag. Null checks are the commonest instruction
	 * between a load and its reload, so getting this wrong costs the entire optimisation. */
	if (MONO_IS_COND_EXC (ins) || ins->opcode == OP_THROW || ins->opcode == OP_RETHROW)
		return FALSE;
	/* Otherwise reuse the GC-point classifier's inverted whitelist: its FALSE set is exactly the
	 * constants/moves/arithmetic/conversion/compare/branch/load family. Anything it does not recognise —
	 * every call included — clears the table, i.e. an unknown opcode costs optimisation, never
	 * correctness. */
	return wj_ins_is_gcpoint (ins, FALSE);
}

/* Upper bound on distinct direct-call helper imports per module. The emitter has ~25 distinct helper
 * symbols; 32 leaves headroom without making the per-compile context meaningfully bigger. */
/* Was 32, sized for the emitter's ~25 distinct runtime helpers. AOT_IMPORTS makes inline-AOT CALLEES intern
 * into the same array, and a callee set is per-method and unbounded, so 32 would have let a call-heavy method
 * spend every slot on callees and push the real helpers back onto call_indirect (interning is first-use order,
 * so it is the LATE user that loses). Raised, but deliberately not huge: each declared import costs ~10 bytes
 * of module and the median body is ~500 B, so an unbounded cap trades a per-call win for module bloat across
 * ~29k modules. Overflow still degrades to call_indirect rather than failing the compile. */
/* Sized for the batch case, where ALL members share one array: a batch of 8 members each interning ~10
 * distinct (table_index, type_idx) pairs needs far more than one method does on its own. Note the pairs do not
 * collapse across members even for the same helper, because a member's type_idx is relative to its own type
 * block -- correct, but duplicative. Overflow still degrades to call_indirect rather than failing the compile. */
/* Storage cap. The EFFECTIVE cap is the runtime knob below, so the two can be A/B'd in one binary — which
 * matters because this number turned out to be load-bearing rather than a formality: Round 99 (AOT_IMPORTS)
 * started interning inline-AOT CALLEES into the same array, so the old 64 became a binding limit and any method
 * past it silently reverted every further direct call to `i32.const <idx>; call_indirect`. */
#define WJ_MAX_HELPER_IMPORTS 512

/* Per-compile vreg access context: non-reference vregs live in wasm locals (li[]); reference vregs
 * (refslot[vreg] >= 0) live in the GC-scanned ref shadow stack at refbase + slot*4. refbase/rtmp are
 * wasm i32 locals (the frame base address + a scratch for ref stores). refslot is NULL when the ref
 * shadow stack is disabled (offline dump), so refs fall back to locals. */
typedef struct {
	int *li;
	int nvreg;
	int *refslot;
	/* MONO_WASM_JIT_REF_WT: per-vreg write-through flag. When set, the wasm local li[vreg] is the
	 * value HOME (reads are a plain local.get) and the frame slot is only a def-mirrored pin copy
	 * (every wasm_st also stores to the slot). NULL / unset = slot-homed (pre-WT behaviour, and the
	 * addrslot==-2 sentinels whose slot address escapes to callees). */
	guint8 *ref_wt;
	int refbase;   /* wasm local: ref-frame base address */
	int rtmp;      /* wasm local: scratch i32 for ref stores */
	int lazy_frame;       /* frame is absent until the first effective returning GC point */
	int frame_active;     /* wasm i32 local: lazy frame has been materialized */
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
	/* The helper-import array used to live here, filled during emission. It does not any more: a call to a
	 * fixed table index leaves a relocation and the assembler assigns the slots (wj_asm_intern_import),
	 * because a member cannot know its own module's final import count while it is still being written. */
} WjCtx;

/* --- the four call forms the emitter can leave behind --------------------------------------------
 *
 * None of these writes a call. Each records a zero-length HOLE (see wasm-encoder.h) saying what is being
 * called and with what signature; the form -- `call_indirect` through the table, `call` to an import, or
 * `call` to a module-local function -- is chosen when the member is framed, and re-chosen every time it is
 * re-framed. That is what makes retargeting free, and it is why nothing here consults a knob.
 *
 * `type_idx` is an index into THIS member's type block (0 = the method's own signature, 1 = its entry
 * thunk's, 2.. = the callee types interned during emission), not a module type index. */

/* A runtime helper, or any other target at a fixed function-table index: a C function in
 * dotnet.native.wasm, or an AOT method body. Under wasm a C function pointer IS an indirect-function-table
 * index, which is why an address is all this needs. Args are already on the stack. */
static void
wj_emit_helper_call (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_HELPER, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* Another wasm-JITted method, named by identity as well as by f-slot: the identity is what lets the
 * assembler notice the callee is co-located (making this a one-instruction `call <funcidx>` that V8 can
 * inline through) and what it checks the callee's registered ABI against before daring to import it. */
static void
wj_emit_method_call (WasmBuf *b, MonoMethod *callee, int fslot, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_CALL, type_idx, (guint32) fslot, callee);
}

/* A call whose target index is computed at run time -- a virtual dispatch, an IC hit, a calli. This one
 * can never become a direct call; only its functype is relocated. */
static void
wj_emit_dynamic_call (WasmBuf *b, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_INDIRECT, type_idx, 0, NULL);
}

/* A fixed table index that the helper-import conversion never covered.
 *
 * Mechanically identical to a helper -- a C function in dotnet.native.wasm, at a table index fixed for the
 * process -- and split out for one reason: Round 121 converted a NAMED SET of helper sites to imports after
 * measuring them (leave_island, threads_state_poll, raise_corlib, check_store), and these two were not in
 * it. They are the runtime JIT-icall and the residual throw continuation, and they have always been
 * `i32.const <fptr>; call_indirect`.
 *
 * Folding them in with the helpers converted them silently, which the tier-shape gate caught: 14,583 of
 * 22,547 common methods changed structure in a refactor that was supposed to change none. Converting them
 * is very likely a win -- an import is ~5 x86 against ~15, and R129 measured index 403815 alone at 32.3% of
 * all constant-index indirect traffic -- but it is a change with an outcome, so it waits behind its own
 * knob and its own measurement rather than riding along inside a refactor. */
static void
wj_emit_helper_call_ci (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_HELPER_CI, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* An AOT method body. Identical mechanically to a helper -- a fixed function-table index with a known
 * functype -- and separate only so MONO_WASM_JIT_AOT_IMPORTS and MONO_WASM_JIT_HELPER_IMPORTS remain two
 * knobs at assembly time rather than one. */
static void
wj_emit_aot_call (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_AOT, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* A multi-value block type, which is a type index too and moves for exactly the same reason. */
static void
wj_emit_blocktype (WasmBuf *b, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_TYPE_S, type_idx, 0, NULL);
}

/* A scalar register-to-register copy. These carry no type information of their own — see the
 * valtype unification pass in mono_wasm_emit_method. OP_VMOVE/OP_XMOVE are NOT moves for this
 * purpose: they copy value types, which have no wasm valtype at all. */
static gboolean
wj_ins_is_move (int opcode)
{
	return opcode == OP_MOVE || opcode == OP_LMOVE || opcode == OP_FMOVE || opcode == OP_RMOVE;
}

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
		wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
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
	/* A BYREF return is a POINTER and must NOT be masked/extended as its pointee type: `ref sbyte` would
	 * get i32.extend8_s applied to the pointer. This is load-bearing and currently INVISIBLE at the call
	 * site — the ct.ret == WASM_I32 guards around every caller DO fire for a byref; what saves us is that
	 * mini_get_underlying_type erases byref to MONO_TYPE_I (mini_type_get_underlying_type's first line),
	 * so the switch falls to `default`. Made explicit here so a future "simplification" to
	 * `ret->type` cannot silently corrupt byref-of-primitive returns. */
	if (m_type_is_byref (ret))
		return;
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
		if (c->ref_wt && c->ref_wt [vreg]) {
			/* write-through ref: the wasm local is the value home; the slot copy (stored at every
			 * def) pins the referent so this cached value can't be moved out from under us. */
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->li [vreg]);
			return TRUE;
		}
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
		wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
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
		 * i32.store wants [addr, val] but val is on top, so stash it via a local then push addr+val.
		 * Write-through mode stashes into the vreg's OWN local (which becomes the value home);
		 * slot-homed mode uses the shared rtmp scratch. Either way the slot is written at every def
		 * — adjacent to the def, with no GC point in between — so the pin copy is always current. */
		int stash = (c->ref_wt && c->ref_wt [vreg]) ? c->li [vreg] : c->rtmp;
		wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) stash);
		if (c->lazy_frame) {
			/* Before materialization the wasm local is the sole value home and there is no valid
			 * refbase to mirror into. Once active, retain ordinary write-through semantics. */
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->frame_active);
			wasm_op (b, WASM_OP_IF); wasm_u8 (b, 0x40);
		}
#ifdef HOST_BROWSER
		if (G_UNLIKELY (c->storeguard)) {
			/* check_store(refbase + slot*4, 0) before the store (STOREGUARD; arenas+helper are HOST_BROWSER) */
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
			wasm_i32_const (b, (gint32) (c->refslot [vreg] * 4));
			wasm_op (b, WASM_OP_I32_ADD);
			wasm_i32_const (b, 0);
			wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
		}
#endif
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) stash);
		wasm_op (b, WASM_OP_I32_STORE); wasm_memarg (b, 2, (guint32) (c->refslot [vreg] * 4));
		if (c->lazy_frame)
			wasm_op (b, WASM_OP_END);
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

static guint32
wj_functype_hash (const WasmFuncType *t)
{
	guint32 h = 2166136261u, i;
	h = (h ^ t->nparams) * 16777619u;
	for (i = 0; i < t->nparams; ++i)
		h = (h ^ (guint32)t->params [i]) * 16777619u;
	return (h ^ (guint32)t->ret) * 16777619u;
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

/* Published method effect used by the caller's GC-liveness pass. Direct callees are island-built and
 * registered before their callers, so looking through the immutable f-slot descriptor gives us a
 * naturally transitive bottom-up no-GC classification without a separate whole-program pass. */
static gboolean
wj_fslot_is_nogc (int fslot)
{
	int ci, desc_id;
	WjRegEntry *re;
	if (fslot <= 0)
		return FALSE;
	ci = fslot / WJ_SLOT_CHUNK;
	if (ci < 0 || ci >= WJ_SLOT_NCHUNKS || !wj_fslot_desc_chunks [ci])
		return FALSE;
	mono_memory_barrier ();
	desc_id = wj_fslot_desc_chunks [ci][fslot % WJ_SLOT_CHUNK];
	if (desc_id <= 0 || desc_id > wj_reg_n)
		return FALSE;
	re = wj_reg_at (desc_id - 1);
	return re && re->f == fslot && re->no_gc;
}

/* Return the admitted direct managed callee's f-slot, or zero for every shape whose dispatch can enter
 * the runtime (rgctx, icall, virtual/residual, unresolved, or recursion). Keep the synchronized-wrapper
 * normalization identical to the actual OP_CALL lowering. */
static int
wj_direct_admitted_fslot (MonoCompile *cfg, MonoInst *ins)
{
	MonoCallInst *call;
	MonoMethod *m;
	extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
	extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
	switch (ins->opcode) {
	case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
		break;
	default:
		return 0;
	}
	call = (MonoCallInst *) ins;
	m = call->method;
	if (!m || call->rgctx_reg || m == cfg->method)
		return 0;
	if (m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		m = mono_marshal_get_synchronized_wrapper (m);
	m = wj_canonical_callee (m);
	if (!m || m == cfg->method)
		return 0;
	return mono_wasm_jit_get_callee_fslot (m);
}

/* GC-point classifier after resolving direct method effects. Calls outside this narrow admitted-direct
 * set remain conservative. A no-GC callee may itself call earlier no-GC callees, making this transitive. */
static gboolean
wj_ins_is_effective_gcpoint (MonoCompile *cfg, MonoInst *ins)
{
	if (!wj_ins_is_gcpoint (ins, wj_method_raise_exempt (cfg)))
		return FALSE;
	return !wj_fslot_is_nogc (wj_direct_admitted_fslot (cfg, ins));
}

/* The same classification as seen by the consumers that PUBLISH a no-collection claim outside this
 * frame -- method_nogc (read by every caller's direct-call admission), effective_gcp_count (gates the
 * lazy frame), prior_gcps_are_polls and the terminal-vcall handoff. At RAISE_NOGC level 1 a raise counts
 * here even though it does not count for this method's own def/use generations, because the raise really
 * does allocate; only the "nothing in THIS frame reads a vreg afterwards" half of the argument survives.
 * Identical to wj_ins_is_effective_gcpoint at levels 0 and 2. */
static gboolean
wj_ins_is_published_gcpoint (MonoCompile *cfg, MonoInst *ins)
{
	if (mono_wasm_jit_raise_nogc == 1 && wj_ins_is_raise (ins))
		return TRUE;   /* no raise opcode is a call, so the direct-callee resolution cannot apply */
	/* level 2 with the shard knob: a method the shard excluded gets the honest answer from below. */
	return wj_ins_is_effective_gcpoint (cfg, ins);
}

/*
 * A virtual forwarding call can take ownership of its reference roots at the call boundary:
 *
 *  - a compact-PIC hit has no managed safepoint between loading the caller's wasm locals and entering
 *    the already-admitted target;
 *  - a miss first acquires a worker-local conservative root frame (native allocation/root-table work,
 *    no managed allocation), copies every argument into it, and only then resolves or invokes code
 *    which may collect.
 *
 * Keep this tied to the exact machinery providing that guarantee. The legacy residual scratch is not
 * a GC root and must continue to make the caller retain its slots.
 */
static gboolean
wj_ins_is_pinned_vcall_forward (MonoInst *ins)
{
	MonoCallInst *call;
	extern int mono_wasm_jit_vcall_inline_ic;
	extern int mono_wasm_jit_vcall_shared_miss_enabled;

	switch (ins->opcode) {
	case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
	case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
		break;
	default:
		return FALSE;
	}
	call = (MonoCallInst *) ins;
	return mono_wasm_jit_vcall_inline_ic && mono_wasm_jit_vcall_shared_miss_enabled &&
		call->method && (call->method->flags & METHOD_ATTRIBUTE_VIRTUAL) &&
		call->signature && call->signature->hasthis && call->call_info;
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

static void
wj_result_add_direct_dep (MonoWasmJitResult *res, int fslot, guint32 sig_id, MonoMethod *callee)
{
	int i;
	if (fslot <= 0)
		return;
	for (i = 0; i < res->ndirect_deps; ++i)
		if (res->direct_deps [i] == fslot) {
			if (res->direct_dep_sig [i] != sig_id)
				res->direct_deps_truncated = 1; /* same slot observed with two ABIs: reject emission */
			return;
		}
	if (res->ndirect_deps < MONO_WASM_JIT_MAX_DIRECT_DEPS) {
		res->direct_deps [res->ndirect_deps] = fslot;
		res->direct_dep_method [res->ndirect_deps] = callee;
		res->direct_dep_sig [res->ndirect_deps++] = sig_id;
	}
	else
		res->direct_deps_truncated = 1;
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
	extern int mono_wasm_jit_residual_perm, mono_wasm_jit_residual_cold;
	extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
	MonoBasicBlock *bb;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			MonoCallInst *call;
			MonoMethod *call_method;
			MonoMethodSignature *csig;
			switch (ins->opcode) {
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
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
			if (call_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
				call_method = mono_marshal_get_synchronized_wrapper (call_method);
			call_method = wj_canonical_callee (call_method);   /* stabilize the per-compile synchronized-inner wrapper */
			if (call_method == cfg->method)                                                        /* self-recursion: baked via self-slot reservation, not a blocker */
				continue;
			if (mono_wasm_jit_get_callee_fslot (call_method) > 0)                                   /* already JITted */
				continue;
			if (mono_interp_jit_call_supported (call_method, csig))    /* AOT-routed residual */
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

/* Scalar-vtype call ABI (MONO_WASM_JIT_VTYPE_SCALAR / _REF). A struct mini_wasm_is_scalar_vtype accepts
 * (<=8 bytes, exactly one field) is passed in the wasm ABI BY ITS SINGLE FIELD's scalar, not by address
 * (get_storage -> ArgVtypeAsScalar -> LLVMArgWasmVtypeAsScalar). wj_scalar_vtype_valtype returns that
 * scalar's wasm valtype (sets *out, returns TRUE), else FALSE. A ref-free struct is gated on
 * VTYPE_SCALAR (its ByVal value lives in an addr-frame slot); a ref-etype one (single object field,
 * e.g. RuntimeTypeHandle{RuntimeType}) on VTYPE_SCALAR_REF — its single ref lives in a GC-scanned
 * ref-shadow slot (ldaddr pre-pass sentinel addrslot=-2) so it is tracked as a pinning root.
 * Cross-available (used by the emit below in both builds). */
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

/*
 * WasmCallInfo: the single declarative wasm ABI descriptor for a MonoMethodSignature, shared by every
 * call-emit path (method self-sig, JIT->JIT, inline-AOT, interp residual, vcall ICs). It replaces the
 * ~8 sites that each re-derived a WasmFuncType from the signature by hand (the reviewer's "several bugs
 * come from paths independently reconstructing the same ABI"). mono_wasm_get_call_info builds the
 * CANONICAL callee functype -- [this i32] + params -> ret, with NO trailing rgctx/extra arg (that is a
 * per-call-site concern, layered on top where needed) -- using exactly the rules the direct-call path
 * used: this -> i32, each param via wasm_valtype_of_type with a ref-free/ref scalar-vtype fallback
 * (LLVMArgWasmVtypeAsScalar), ret via wasm_valtype_of_type. Unrepresentable shapes leave valid=FALSE
 * with a fail_reason the consumer bails on (preserving today's bail accounting). Hidden vret
 * (ArgValuetypeAddrInIReg) sets vret_byaddr and, until the vret lowering lands, still bails.
 */
typedef enum {
	WJ_ARG_SCALAR,          /* one wasm scalar (incl. managed byref/ptr/obj as i32, i8 as i64, ...) */
	WJ_ARG_VTYPE_SCALAR,    /* ArgVtypeAsScalar: struct passed as its single-field etype scalar */
	WJ_ARG_VTYPE_BYADDR,    /* ArgValuetypeAddrOnStack: i32 pointer to a caller-owned copy (MONO_WASM_JIT_VTYPE_BYADDR) */
	WJ_ARG_GSHAREDVT,       /* ArgGsharedVTOnStack: unsupported here */
	WJ_ARG_INVALID,
} WjArgKind;

typedef struct {
	WjArgKind kind;
	WasmValtype wtype;      /* wasm valtype (this -> WASM_I32; WASM_VOID only for a void ret) */
	MonoType *type;         /* the managed arg/ret type (NULL for the synthetic this) */
	MonoType *etype;        /* scalar-vtype single-field etype, else NULL */
	gint32 vsize, valign;   /* WJ_ARG_VTYPE_BYADDR (and byaddr ret): mono_class_value_size of the struct, else 0 */
} WjArgInfo;

typedef struct {
	guint8 valid;           /* FALSE -> the consumer bails with fail_reason */
	const char *fail_reason;
	gint16 fail_arg;         /* invalid ABI: -1 return, >=0 explicit parameter, -2 non-type failure */
	guint8 hasthis;
	guint8 vret_byaddr;     /* ret is a by-address vtype (hidden vret). Valid under MONO_WASM_JIT_VRET: the
	                         * internal convention appends a TRAILING i32 vret-pointer param to ftype and the
	                         * wasm ret becomes void (managed-arg -> wasm-param index mapping stays identity;
	                         * the native AOT ABI is vret-FIRST — the inline-AOT call path reorders). With the
	                         * flag off it implies !valid, as before. */
	int nargs;              /* hasthis + param_count (NOT counting the vret param; ftype.nparams does) */
	WjArgInfo ret;
	WjArgInfo args [WASM_FUNCTYPE_MAX_PARAMS];
	WasmFuncType ftype;     /* canonical callee functype: [this i32] + params [+ vret i32] -> ret */
	guint32 f_sig_id;       /* wj_functype_hash (&ftype); the persisted per-method ABI fingerprint */
} WasmCallInfo;

/*
 * TRUE iff T is a plain by-address-able value type — the ArgValuetypeAddrOnStack /
 * ArgValuetypeAddrInIReg class of get_storage above, and ONLY that class: scalar-vtypes are
 * excluded so WJ_ARG_VTYPE_BYADDR <=> the native AOT ABI also passes a pointer (the inline-AOT
 * path then never has an internal-vs-native kind mismatch). SIMD is excluded (16-byte interp
 * alignment + v128 ABI, neither wired up); gsharedvt-variable is not a concrete layout; the size
 * cap matches the ldaddr-vtype addr-frame cap.
 */
static gboolean
wj_byaddr_vtype (MonoType *t, gint32 *vsize, gint32 *valign)
{
	MonoType *ut;
	MonoClass *k;
	guint32 al = 0;
	if (m_type_is_byref (t))
		return FALSE;
	ut = mini_get_underlying_type (t);
	if (!MONO_TYPE_ISSTRUCT (ut))
		return FALSE;
	if (mini_is_gsharedvt_variable_type (ut))
		return FALSE;
	{
		MonoType *setype = NULL;
		if (mini_wasm_is_scalar_vtype (ut, &setype))
			return FALSE;
	}
	k = mono_class_from_mono_type_internal (ut);
	if (!k || m_class_is_simd_type (k))
		return FALSE;
	*vsize = (gint32) mono_class_value_size (k, &al);
	*valign = (gint32) al;
	if (*vsize <= 0 || *vsize > 4096)
		return FALSE;
	return TRUE;
}

/* C-side twins of the by-addr classification, for the residual marshal (interp.c): call_interp /
 * aot_call_lean must deref EXACTLY the scratch slots the emitter spilled as copy ADDRESSES (a by-addr
 * vtype arg) and write VT returns through the caller pointer at WJ_SCRATCH_VRET_OFF — nothing else.
 * Includes the flag gates so classification is identical on both sides of the boundary. */
gboolean
mono_wasm_jit_arg_is_byaddr (MonoType *t)
{
	extern int mono_wasm_jit_vtype_byaddr;
	gint32 sz, al;
	return mono_wasm_jit_vtype_byaddr && wj_byaddr_vtype (t, &sz, &al);
}

gboolean
mono_wasm_jit_ret_is_byaddr (MonoType *t)
{
	extern int mono_wasm_jit_vret;
	gint32 sz, al;
	return mono_wasm_jit_vret && wj_byaddr_vtype (t, &sz, &al);
}

static void
mono_wasm_get_call_info (MonoMethodSignature *sig, WasmCallInfo *ci)
{
	extern int mono_wasm_jit_vtype_byaddr, mono_wasm_jit_vret;
	int i;
	memset (ci, 0, sizeof (*ci));
	ci->fail_arg = -2;
	ci->valid = TRUE;
	ci->hasthis = sig->hasthis ? 1 : 0;
	ci->nargs = sig->hasthis + sig->param_count;
	if (ci->nargs > WASM_FUNCTYPE_MAX_PARAMS) { ci->valid = FALSE; ci->fail_reason = "call nargs"; return; }

	if (sig->hasthis) {
		ci->args [0].kind = WJ_ARG_SCALAR;
		ci->args [0].wtype = WASM_I32;
		ci->ftype.params [ci->ftype.nparams++] = WASM_I32;
	}
	for (i = 0; i < (int) sig->param_count; ++i) {
		WjArgInfo *a = &ci->args [i + sig->hasthis];
		WasmValtype pv = wasm_valtype_of_type (sig->params [i]);
		a->type = sig->params [i];
		if (pv == 0 || pv == WASM_VOID) {
			/* BYVAL scalar-vtype arg: declare the param as its single-field etype scalar (the AOT
			 * callee's LLVMArgWasmVtypeAsScalar ABI). Multi-field/large vtype: i32 pointer to a
			 * caller-owned copy (ArgValuetypeAddrOnStack). Anything else is unrepresentable -> bail. */
			WasmValtype sv;
			if (wj_scalar_vtype_valtype (sig->params [i], &sv)) {
				a->kind = WJ_ARG_VTYPE_SCALAR;
				a->etype = sig->params [i];
				pv = sv;
			} else if (mono_wasm_jit_vtype_byaddr && wj_byaddr_vtype (sig->params [i], &a->vsize, &a->valign)) {
				a->kind = WJ_ARG_VTYPE_BYADDR;
				pv = WASM_I32;
			} else {
				ci->valid = FALSE;
				ci->fail_arg = (gint16) i;
				ci->fail_reason = mini_is_gsharedvt_variable_type (mini_get_underlying_type (sig->params [i]))
					? "call arg gsharedvt" : "call arg type";
				return;
			}
		} else {
			a->kind = WJ_ARG_SCALAR;
		}
		a->wtype = pv;
		ci->ftype.params [ci->ftype.nparams++] = pv;
	}

	ci->ret.type = sig->ret;
	if (sig->ret->type == MONO_TYPE_VOID) {
		ci->ret.kind = WJ_ARG_SCALAR;
		ci->ret.wtype = WASM_VOID;
		ci->ftype.ret = WASM_VOID;
	} else {
		WasmValtype rv = wasm_valtype_of_type (sig->ret);
		if (rv == 0 || rv == WASM_VOID) {
			WasmValtype sv;
			if (wj_scalar_vtype_valtype (sig->ret, &sv)) {
				/* ArgVtypeAsScalar: return the struct's single field directly, matching LLVM's
				 * native wasm ABI. */
				ci->ret.kind = WJ_ARG_VTYPE_SCALAR;
				ci->ret.etype = sig->ret;
				ci->ret.wtype = sv;
				ci->ftype.ret = sv;
			} else {
				/* Multi-field vtype return via a trailing hidden by-address pointer. */
				ci->vret_byaddr = 1;
				if (!(mono_wasm_jit_vret && wj_byaddr_vtype (sig->ret, &ci->ret.vsize, &ci->ret.valign))) {
					ci->valid = FALSE;
					ci->fail_arg = -1;
					ci->fail_reason = mini_is_gsharedvt_variable_type (mini_get_underlying_type (sig->ret))
						? "call ret gsharedvt" : "call ret type";
					return;
				}
				if (ci->ftype.nparams + 1 > WASM_FUNCTYPE_MAX_PARAMS) { ci->valid = FALSE; ci->fail_reason = "call nargs"; return; }
				ci->ret.kind = WJ_ARG_VTYPE_BYADDR;
				ci->ret.wtype = WASM_VOID;
				ci->ftype.params [ci->ftype.nparams++] = WASM_I32;
				ci->ftype.ret = WASM_VOID;
			}
		} else {
			ci->ret.kind = WJ_ARG_SCALAR;
			ci->ret.wtype = rv;
			ci->ftype.ret = rv;
		}
	}
	ci->f_sig_id = wj_functype_hash (&ci->ftype);
}

/*
 * Describe a JITted method's `f` signature for the jiterpreter's native->interp entry trampoline, which
 * forwards to it directly with call_indirect.
 *
 * The trampoline is handed each argument as a POINTER to the value (the gsharedvt-in convention), while
 * `f` takes values, so per argument it needs both how to LOAD it and what wasm type results. Both come
 * from mono_wasm_get_call_info here rather than being re-derived in TypeScript: it is the same
 * classification the emitter used to build `f`, and wasm checks call_indirect signatures exactly, so any
 * divergence would be a trap at the first call rather than a bail. (jiterpreter's own
 * mono_jiterp_type_get_raw_value_size cannot serve: it reports 4 for both i32 and r4, which is why the
 * existing marshaller has "FIXME: 4 and 8-byte floats" and routes them to a helper call.)
 *
 * kinds[i]  : WJ_ENTRY_LD_* below — the load to perform on argument i's pointer.
 * vtypes[i] : the wasm valtype of argument i; vtypes[nargs] is the RETURN valtype (WASM_VOID if none).
 *
 * Returns the argument count (including `this`), or -1 if this method is not eligible.
 */
#define WJ_ENTRY_LD_I32      0
#define WJ_ENTRY_LD_I64      1
#define WJ_ENTRY_LD_F32      2
#define WJ_ENTRY_LD_F64      3
#define WJ_ENTRY_LD_I8_S     4
#define WJ_ENTRY_LD_U8       5
#define WJ_ENTRY_LD_I16_S    6
#define WJ_ENTRY_LD_U16      7
#define WJ_ENTRY_LD_ASIS     8   /* the local already holds the value (this-reference, byref, pointer) */

#ifdef HOST_BROWSER
EMSCRIPTEN_KEEPALIVE int
mono_wasm_jit_entry_sig (MonoMethod *method, guint8 *kinds, guint8 *vtypes, int max)
{
	WasmCallInfo ci;
	MonoMethodSignature *sig;
	int i, nargs, nparams;

	if (!method || !kinds || !vtypes)
		return -1;
	sig = mono_method_signature_internal (method);
	if (!sig)
		return -1;
	nparams = (int) sig->param_count;
	nargs = nparams + (sig->hasthis ? 1 : 0);
	if (nargs + 1 > max)
		return -1;

	mono_wasm_get_call_info (sig, &ci);
	if (!ci.valid || ci.vret_byaddr || ci.nargs != nargs)
		return -1;

	for (i = 0; i < nargs; ++i) {
		MonoType *t;
		if (ci.args [i].kind != WJ_ARG_SCALAR)
			return -1;
		vtypes [i] = (guint8) ci.args [i].wtype;

		if (sig->hasthis && i == 0) {
			kinds [i] = WJ_ENTRY_LD_ASIS;   /* the trampoline's this_arg local is the object pointer */
			continue;
		}
		t = sig->params [i - (sig->hasthis ? 1 : 0)];
		if (m_type_is_byref (t)) {
			kinds [i] = WJ_ENTRY_LD_ASIS;   /* byref: the pointer IS the value */
			continue;
		}
		switch (mini_get_underlying_type (t)->type) {
		case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: kinds [i] = WJ_ENTRY_LD_U8; break;
		case MONO_TYPE_I1:                         kinds [i] = WJ_ENTRY_LD_I8_S; break;
		case MONO_TYPE_CHAR: case MONO_TYPE_U2:    kinds [i] = WJ_ENTRY_LD_U16; break;
		case MONO_TYPE_I2:                         kinds [i] = WJ_ENTRY_LD_I16_S; break;
		case MONO_TYPE_I4: case MONO_TYPE_U4:      kinds [i] = WJ_ENTRY_LD_I32; break;
		case MONO_TYPE_I8: case MONO_TYPE_U8:      kinds [i] = WJ_ENTRY_LD_I64; break;
		case MONO_TYPE_R4:                         kinds [i] = WJ_ENTRY_LD_F32; break;
		case MONO_TYPE_R8:                         kinds [i] = WJ_ENTRY_LD_F64; break;
		case MONO_TYPE_I: case MONO_TYPE_U:
		case MONO_TYPE_PTR: case MONO_TYPE_FNPTR:
		case MONO_TYPE_OBJECT: case MONO_TYPE_STRING: case MONO_TYPE_CLASS:
		case MONO_TYPE_SZARRAY: case MONO_TYPE_ARRAY:
			kinds [i] = WJ_ENTRY_LD_I32; break;
		default:
			return -1;   /* value types, generic instances, anything unclassified */
		}
		/* The load must produce exactly the type the callee declares, or the call_indirect traps. */
		if ((kinds [i] == WJ_ENTRY_LD_I64 && vtypes [i] != WASM_I64) ||
		    (kinds [i] == WJ_ENTRY_LD_F32 && vtypes [i] != WASM_F32) ||
		    (kinds [i] == WJ_ENTRY_LD_F64 && vtypes [i] != WASM_F64) ||
		    (kinds [i] <= WJ_ENTRY_LD_I32 && kinds [i] != WJ_ENTRY_LD_I64 && vtypes [i] != WASM_I32) ||
		    (kinds [i] >= WJ_ENTRY_LD_I8_S && kinds [i] <= WJ_ENTRY_LD_U16 && vtypes [i] != WASM_I32))
			return -1;
	}
	vtypes [nargs] = (guint8) ci.ret.wtype;   /* WASM_VOID for void */
	return nargs;
}
#endif


/*
 * MONO_WASM_JIT_AOT_ENTRY — hand AOT/native callers a direct call into our JITted body.
 *
 * interp_create_method_pointer_llvmonly hands AOT code a ftndesc whose target eventually reaches
 * mono_jiterp_interp_entry. That path ALREADY redirects to us (interp.c: if wasm_jit_slot > 0 it calls
 * mono_wasm_jit_invoke_caught -> the e-thunk -> our body), so for a method we have compiled every step
 * around the call is pure boundary overhead: InterpFrame setup, get_arg_offset_fast, stack-pointer
 * juggling, GC-unsafe transition, LMF push/pop, and stackval marshalling in and out. Measured at 7.6% of
 * steady-state time (mono_jiterp_interp_entry 5.99 + _prologue 1.16 + stackval_from_data 0.46).
 *
 * This builds a one-function adapter module forwarding (args..., extra) to the method's f-slot, and
 * returns its table slot. Callers install it as the ftndesc target.
 *
 * Restricted on purpose (see the gate below): the adapter's parameter list has to agree EXACTLY with how
 * mono lowered the same signature for AOT, or the call_indirect signature check traps.
 */


/* Verbose-only detail for signature bails. Keep this out of the aggregate counters: it deliberately
 * performs name/layout queries and is intended for short MONO_WASM_JIT_VERBOSE=2 diagnosis runs.
 * `arg` is -1 for the return, >=0 for an explicit parameter, and -2 when only the overall arity is
 * known. Printing both the raw wasm classification and each feature gate makes a "call ret type"
 * actionable without another instrumentation build. */
static void
wj_print_bail_sig (const char *site, MonoMethod *callee, MonoMethodSignature *sig, int arg)
{
	extern int mono_wasm_jit_vtype_scalar, mono_wasm_jit_vtype_scalar_ref;
	extern int mono_wasm_jit_vtype_byaddr, mono_wasm_jit_vret, mono_wasm_jit_byref;
	char *cn = callee ? mono_method_get_full_name (callee) : NULL;

	if (!sig) {
		printf (" [sig site=%s callee=%s signature=null]", site ? site : "?", cn ? cn : "<indirect>");
		g_free (cn);
		return;
	}
	if (arg < -1 || arg >= (int) sig->param_count) {
		printf (" [sig site=%s callee=%s part=arity hasthis=%d params=%d]", site ? site : "?",
			cn ? cn : "<indirect>", sig->hasthis ? 1 : 0, (int) sig->param_count);
		g_free (cn);
		return;
	}

	{
		MonoType *type = arg < 0 ? sig->ret : sig->params [arg];
		MonoType *ut = mini_get_underlying_type (type);
		MonoType *scalar_etype = NULL;
		MonoClass *klass = MONO_TYPE_ISSTRUCT (ut) ? mono_class_from_mono_type_internal (ut) : NULL;
		char *tn = mono_type_full_name (type);
		char *sen = NULL;
		gint32 byaddr_size = 0, byaddr_align = 0;
		guint32 layout_align = 0;
		int layout_size = klass ? mono_class_value_size (klass, &layout_align) : -1;
		gboolean scalar_candidate = mini_wasm_is_scalar_vtype (ut, &scalar_etype);
		gboolean byaddr_candidate = wj_byaddr_vtype (type, &byaddr_size, &byaddr_align);
		WasmValtype raw = wasm_valtype_of_type (type);
		WasmValtype scalar_wasm = scalar_etype ? wasm_valtype_of_type (scalar_etype) : 0;
		if (scalar_etype)
			sen = mono_type_full_name (scalar_etype);

		printf (" [sig site=%s callee=%s part=%s index=%d type=%s mono=%d underlying=%d raw_wasm=%d"
			" struct=%d gsharedvt=%d simd=%d refs=%d scalar_candidate=%d scalar_type=%s scalar_wasm=%d byaddr_candidate=%d"
			" size=%d align=%u byaddr_size=%d byaddr_align=%d"
			" flags(vtype_scalar=%d scalar_ref=%d byaddr=%d vret=%d byref=%d)]",
			site ? site : "?", cn ? cn : "<indirect>", arg < 0 ? "ret" : "arg", arg,
			tn ? tn : "?", (int) type->type, (int) ut->type, (int) raw,
			klass ? 1 : 0, mini_is_gsharedvt_variable_type (ut) ? 1 : 0,
			klass && m_class_is_simd_type (klass) ? 1 : 0,
			klass && (m_class_has_references (klass) || m_class_has_ref_fields (klass)) ? 1 : 0,
			scalar_candidate ? 1 : 0, sen ? sen : "-", (int) scalar_wasm, byaddr_candidate ? 1 : 0,
			layout_size, (unsigned) layout_align, (int) byaddr_size, (int) byaddr_align,
			mono_wasm_jit_vtype_scalar, mono_wasm_jit_vtype_scalar_ref,
			mono_wasm_jit_vtype_byaddr, mono_wasm_jit_vret, mono_wasm_jit_byref);
		g_free (sen);
		g_free (tn);
	}
	g_free (cn);
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

/* Store an ArgVtypeAsScalar call result (currently on the wasm stack) through the return-temp
 * address captured at WjCallArgs position n. MINI represents every vtype call result as an addressable temp and
 * emits OP_VCALL2 with no scalar dreg, even when the native wasm ABI returns the single field directly. */
static gboolean
wj_store_scalar_vtype_result (WasmBuf *body, WjCtx *c, WasmValtype vt, int addr_vreg)
{
	int g = wasm_valtype_group (vt);
	int tmp;
	WasmOpcode sop;
	guint32 al;
	if (g < 0)
		return FALSE;
	/* i32 always has the ref-store scratch; wider groups have addr-frame scratch because the
	 * addressable vtype result temp itself requires an addr frame. */
	tmp = vt == WASM_I32 ? c->rtmp : c->addr_tmp [g];
	if (tmp < 0)
		return FALSE;
	switch (vt) {
	case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
	case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
	case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
	case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
	default: return FALSE;
	}
	wasm_op_local (body, WASM_OP_LOCAL_SET, (guint32) tmp);
	if (!wasm_ld (body, c, addr_vreg))
		return FALSE;
	wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) tmp);
	wasm_op (body, sop); wasm_memarg (body, al, 0);
	return TRUE;
}

/*
 * WjCallArgs — the wasm JIT's per-call-site argument descriptor, hung off the (otherwise unused for
 * wasm) call->call_info. Built by mono_wasm_emit_call at method-to-ir time.
 *
 * The emitter reads call arguments POSITIONALLY rather than from the call instruction's sregs: an
 * instruction has MONO_MAX_SRC_REGS of those and a managed signature has arbitrarily many arguments.
 * What each position stores is the DEFINING INSTRUCTION, and the vreg is read back out of it through
 * wj_arg_vreg at the moment of use.
 *
 * That indirection is the whole point of the struct. mono_wasm_emit_call runs long before codegen,
 * and the optimization pipeline in between rewrites vreg NUMBERS -- SSA renaming above all. A cached
 * number silently becomes some other value's vreg, which is a wrong ARGUMENT rather than a bail.
 * A MonoInst* is mempool-allocated and stable for the whole compile, and every pass that renumbers a
 * vreg does it by rewriting exactly the dreg field this reads, so the answer is always current and
 * nothing has to be resynchronized after the pipeline. (capvreg is a narrow, individually justified
 * fallback for one instruction-retiring transform -- see wj_arg_vreg -- not a second general record.)
 */
typedef struct {
	int nargs;             /* managed args including `this`; carrier[]/capvreg[] have nargs+1 entries */
	MonoInst **carrier;    /* [0..nargs-1] arg source; [nargs] the hidden-vret address, or NULL */
	int *capvreg;          /* the vreg each carrier defined AT CAPTURE TIME — see wj_arg_vreg */
	gint32 *copyoff;       /* [0..nargs-1] by-addr copy-region byte offset in the addr frame, -1 = none.
	                        * Filled by the emit-time pre-pass. Not a vreg, so it needs no indirection. */
} WjCallArgs;

/*
 * Current vreg of managed argument AI (AI == nargs is the hidden-vret address), or -1 if there is none.
 * Callers turn -1 into a clean bail rather than emitting an access to local -1.
 *
 * Normally this is just carrier->dreg, read live so that any pass which renumbers the vreg is followed
 * automatically. The capture-time number is the fallback for exactly one situation: mono_local_cprop's
 * reverse copy propagation rewrites `B <- FOO; A <- B` into `A <- FOO` by pointing FOO's dreg at A and
 * MONO_DELETE_INS-ing the carrier -- which NULLIFY_INSes it, leaving dreg == -1. The argument still
 * lands in A, because A is precisely the dreg the producer was retargeted to; only the instruction that
 * used to name it is gone. So falling back to the captured number is not a guess, it is the same value
 * by construction.
 *
 * Nothing else can retire a carrier: mono_local_deadce marks out_ireg_args vregs used, and both
 * mono_ssa_deadce and ssa_cprop's folding only touch defs of cfg->varinfo entries, which a carrier dreg
 * (fresh, singly-defined, non-variable) is not. If a future pass does retire one for another reason,
 * the fallback stops being sound -- so it is deliberately narrow rather than a general "trust the
 * snapshot" rule.
 */
static int
wj_arg_vreg (MonoCallInst *call, int ai)
{
	WjCallArgs *w = (WjCallArgs *) call->call_info;
	MonoInst *c;

	if (!w || ai < 0 || ai > w->nargs)
		return -1;
	c = w->carrier [ai];
	if (!c)
		return -1;
	return c->dreg >= 0 ? c->dreg : w->capvreg [ai];
}

/* Emit one managed-call arg (index ai over the managed args, incl. `this` at 0 when hasthis).
 * A BYVAL ref-free scalar-vtype param (MONO_WASM_JIT_VTYPE_SCALAR) is loaded as its single-field etype
 * scalar from the ByVal value's addr-frame slot. A by-addr vtype param (WJ_ARG_VTYPE_BYADDR) is
 * memory.copied from its source — the vtype's addr-frame slot, or the caller's own incoming by-addr
 * pointer param (sentinel -3) — into this call site's dedicated copy region, and the REGION's address
 * is the arg: the callee owns that copy (byval mutation safe), and re-emitting the copy inline before
 * every call makes loops re-copy per iteration. Everything else is a normal wasm_ld of the arg vreg.
 * Shared by the direct-JIT, inline-AOT and interp-residual arg loops so all three agree on the ABI
 * (the residual spills the pushed value — scalar or copy address — into its scratch slot; the interp
 * side derefs by-addr slots per mono_wasm_jit_arg_is_byaddr). */
static gboolean
wj_emit_one_call_arg (WasmBuf *body, WjCtx *c, const WasmCallInfo *ci, MonoMethodSignature *csig, MonoCallInst *call, int ai)
{
	int pidx = ai - (csig->hasthis ? 1 : 0);
	if (ci && ai >= 0 && ai < ci->nargs && ci->args [ai].kind == WJ_ARG_VTYPE_BYADDR) {
		WjCallArgs *w = (WjCallArgs *) call->call_info;
		int srcvreg = wj_arg_vreg (call, ai);
		gint32 copyoff = (w && ai < w->nargs) ? w->copyoff [ai] : -1;
		if (copyoff < 0 || srcvreg < 0 || srcvreg >= c->nvreg || !c->addrslot)
			return FALSE;
		/* dest: this call site's copy region */
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (body, copyoff);
		wasm_op (body, WASM_OP_I32_ADD);
		/* src: the vtype value's address */
		if (c->addrslot [srcvreg] >= 0) {
			wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
			if (c->addrslot [srcvreg]) { wasm_i32_const (body, c->addrslot [srcvreg]); wasm_op (body, WASM_OP_I32_ADD); }
		} else if (c->addrslot [srcvreg] == -3) {
			if (!wasm_ld (body, c, srcvreg))   /* our own incoming by-addr param holds the address */
				return FALSE;
		} else {
			return FALSE;   /* "vtype arg no addr slot": source vreg lost its addressable backing */
		}
		wasm_i32_const (body, ci->args [ai].vsize);
		wasm_u8 (body, 0xFC); wasm_uleb (body, 10); wasm_u8 (body, 0x00); wasm_u8 (body, 0x00);   /* memory.copy mem0<-mem0 (bulk memory) */
		/* the arg value = the copy's address */
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (body, copyoff);
		wasm_op (body, WASM_OP_I32_ADD);
		return TRUE;
	}
	if (pidx >= 0 && pidx < (int) csig->param_count) {
		WasmValtype sv;
		if (wj_scalar_vtype_valtype (csig->params [pidx], &sv))
			return wj_emit_scalar_vtype_arg (body, c, csig->params [pidx], wj_arg_vreg (call, ai));
	}
	return wasm_ld (body, c, wj_arg_vreg (call, ai));
}

/* IC probe load: i64.atomic.load = 0xfe 0x11 (align 3). Shared by the inline vcall and AOT-vcall cache
 * probes, which read a (vtab-tag, payload) pair packed into one i64 and rely on the load being atomic —
 * a torn read could match the tag while carrying another receiver type's payload. */
static void
wj_emit_ic_load64 (WasmBuf *b, guint32 off)
{
	wasm_op (b, WASM_OP_ATOMIC_PREFIX); wasm_u8 (b, 0x11); wasm_memarg (b, 3, off);
}

/* Fast-path volume profiling (MONO_WASM_JIT_PROFILE_FAST): emit an inline atomic increment of
 * mono_wasm_jit_counters[idx] into the JITted body. &counters[idx] is a link-time-constant address in the
 * runtime's linear memory (the emitter runs in the same process, same memory — just like the embedded C
 * function-pointer constants). i64.atomic.rmw.add (0xfe 0x26, align 3) keeps the count correct when worker
 * threads share the site; the returned old value is dropped. No-op unless profile_fast is set at emit time,
 * so normal STATS runs pay nothing. Only called from HOST_BROWSER fast-path sites; G_GNUC_UNUSED for the
 * cross-compiler build (which never reaches those sites). */
static __thread gboolean wj_batch_profile_suppressed;

static G_GNUC_UNUSED void
wj_emit_fast_count (WasmBuf *body, int idx)
{
	extern int mono_wasm_jit_profile_fast;
	extern gint64 mono_wasm_jit_counters [];
	if (!mono_wasm_jit_profile_fast || wj_batch_profile_suppressed)
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

/* --- assembly: relocated member bodies -> one WebAssembly module ---------------------------------
 *
 * This is where every call in every body gets its FORM, and that is the entire point of the relocation
 * machinery: the decision is made here, from the module's own membership, and remade from scratch every
 * time the same bodies are framed again with different neighbours. Producing a batched body used to mean
 * running mini_method_compile once per member; it now means a memcpy pass.
 *
 * Cost, from scratchpad/wj/bytecost.mjs: 29.4 us fixed per module + 15.4 ns per wire byte. Co-location
 * therefore REDUCES total instantiate cost -- every batching regression ever measured here (-26.6% R61b,
 * -35.7% R96, -13.0% R105) was deferral, members running interpreted while their batch formed, not module
 * overhead.
 *
 * The three forms, and what V8 makes of each (R128, read out of turboshaft-graph-interface.cc:2697-2727):
 *   i32.const <fslot>; call_indirect   table bounds check, canonical-type check, index->code-pointer
 *                                      arithmetic, validity check, `call *`     ~15 x86, never inlined
 *   call <import>                      three loads from WasmDispatchTableForImports + `call *`
 *                                                                                ~5 x86, never inlined
 *   call <local funcidx>               `call rel32`                              1 x86, and the ONLY
 *                                                                                form V8 will inline
 */
/*
 * Everything one method contributes to a module, with nothing about the module baked in. This is the unit
 * that gets stored (WjRegEntry.body) and re-framed.
 */
typedef struct _WjBody {
	MonoMethod    *method;
	WasmValtype    param_types [WASM_FUNCTYPE_MAX_PARAMS];
	guint32        nparams;
	WasmValtype    ret_type;
	WasmLocalGroup groups [4];
	WasmBuf        f_body;        /* instruction bytes + relocations; owned */
	WasmBuf        e_body;
	WasmFuncType  *extra_types;   /* owned, nextra entries */
	guint32        nextra;
	guint8         uses_calls;
	guint8         uses_eh_tag;
	int            eh_tpool;
} WjBody;

static void
wj_body_free (WjBody *b)
{
	if (!b)
		return;
	wasm_buf_free (&b->f_body);
	wasm_buf_free (&b->e_body);
	g_free (b->extra_types);
	g_free (b);
}

/* Take ownership of the two body buffers (the caller's are left zeroed) and copy the small side tables. */
static WjBody *
wj_body_take (MonoMethod *method, WasmBuf *f, WasmBuf *e,
              const WasmValtype *params, guint32 nparams, WasmValtype ret,
              const WasmLocalGroup *groups, const WasmFuncType *extras, guint32 nextra,
              gboolean uses_calls, gboolean uses_eh_tag, int eh_tpool)
{
	WjBody *b = g_new0 (WjBody, 1);
	g_assert (nparams <= WASM_FUNCTYPE_MAX_PARAMS);
	b->method = method;
	memcpy (b->param_types, params, sizeof (WasmValtype) * nparams);
	b->nparams = nparams;
	b->ret_type = ret;
	memcpy (b->groups, groups, sizeof (WasmLocalGroup) * 4);
	b->f_body = *f; memset (f, 0, sizeof (*f));
	b->e_body = *e; memset (e, 0, sizeof (*e));
	if (nextra) {
		b->extra_types = (WasmFuncType *) g_malloc (sizeof (WasmFuncType) * (gsize) nextra);
		memcpy (b->extra_types, extras, sizeof (WasmFuncType) * (gsize) nextra);
	}
	b->nextra = nextra;
	b->uses_calls = uses_calls ? 1 : 0;
	b->uses_eh_tag = uses_eh_tag ? 1 : 0;
	b->eh_tpool = eh_tpool;
	return b;
}

typedef struct {
	MonoMethod           *method;         /* for the name section and for co-location matching */
	int                   f_slot;         /* this member's own f-slot: the target of the cycle test */
	WasmBuf              *f_body;         /* code with holes; not modified */
	WasmBuf              *e_body;
	/* The member's type block is (its own signature, its thunk's signature, its callee types). The first
	 * two are given as their parts rather than as WasmFuncTypes because that is how the emitter already
	 * has them; wj_assemble materialises the contiguous block. */
	const WasmValtype    *param_types;
	guint32               nparams;
	WasmValtype           ret_type;
	const WasmFuncType   *extra_types;
	guint32               nextra;
	const WasmLocalGroup *groups;
	guint32               nlocal_groups;
	guint8                uses_calls;
	guint8                uses_eh_tag;
	int                   eh_tpool;       /* pool index of the tag's (i32)->void type, or -1 */
} WjAsmMember;

typedef struct {
	guint8 helper_imports;   /* MONO_WASM_JIT_HELPER_IMPORTS: a C helper may become an import */
	guint8 aot_imports;      /* MONO_WASM_JIT_AOT_IMPORTS: an AOT body may become an import */
	guint8 ci_imports;       /* MONO_WASM_JIT_CI_IMPORTS: the never-converted constant-index sites too */
	/* MONO_WASM_JIT_DIRECT_IMPORT: a JITted callee may become an import. Soundness is decided PER EDGE by
	 * wj_asm_method_importable, not by this flag -- see wj_asm_reaches for the cycle argument. */
	guint8 method_imports;
	guint8 local_calls;      /* resolve a co-located callee to `call <funcidx>` */
	guint8 names;            /* MONO_WASM_JIT_NAMES: append the name section */
	int    max_fimports;     /* mono_wasm_jit_max_himp */
} WjAsmPolicy;

/* Per-member census of what the forms came out as; feeds the WJC_ counters and the tier dumps. */
typedef struct { int nlocal, nimport, nindirect; } WjAsmStats;

/* The f-slots one member reached by IMPORT, i.e. the deps it binds at INSTANTIATION rather than at call
 * time. The registry needs them (WjRegEntry.dep_strict) so admission can refuse to instantiate before they
 * are installed instead of discovering it as a LinkError. Caller-provided, assembler-filled, caller-freed. */
typedef struct { int *slot; int n, cap; } WjAsmImports;

/* Where wj_assemble reports which deps a lone method binds at instantiation, so the registry can mark them
 * strict (WjRegEntry.dep_strict). Thread-local for the same reason wj_batch is: one compile at a time. */
static __thread WjAsmImports wj_self_imports;

static void
wj_asm_imports_add (WjAsmImports *ai, int fslot)
{
	if (!ai)
		return;
	if (ai->n == ai->cap) {
		ai->cap = ai->cap ? ai->cap * 2 : 8;
		ai->slot = (int *) g_realloc (ai->slot, sizeof (int) * (gsize) ai->cap);
	}
	ai->slot [ai->n++] = fslot;
}

/* Intern one function import. Slot i IS function index i -- function imports are the only kind-0x00
 * imports -- which is what lets a body name a slot long before the final count is known.
 *
 * The dedup key is (table index, MODULE type index), not (table index, functype): two members calling the
 * same helper with the same signature still get two imports, because each member's type block holds its
 * own copy of that functype and a wasm import names one specific type index. Duplicative but correct, and
 * removing it means deduping the type section, which is a separate change with its own byte-identity cost. */
static int
wj_asm_intern_import (WasmFuncImport *himp, int *nhimp, int cap, guint32 table_index, guint32 ti)
{
	int i;
	for (i = 0; i < *nhimp; ++i)
		if (himp [i].table_index == table_index && himp [i].type_idx == ti)
			return i;
	if (*nhimp >= cap || *nhimp >= WJ_MAX_HELPER_IMPORTS)
		return -1;      /* cap reached: the site stays a call_indirect, exactly as before */
	himp [*nhimp].table_index = table_index;
	himp [*nhimp].type_idx = ti;
	return (*nhimp)++;
}

/* --- a BEST-EFFORT filter on import edges, not the guarantee ---------------------------------------
 *
 * READ THIS FIRST: what makes imports safe is the admission-time check in wj_admit_dependencies, not this.
 * The first version of this work put the whole guarantee here and it does not work, because the question
 * "will the callee be instantiated before me" cannot be answered when the module is EMITTED. The answer has
 * to hold at ADMISSION, which happens later and separately on every worker, and by then the graph has
 * grown: a re-emit registers a new descriptor into the same f-slot with a new dependency list, so the
 * callee behind an imported slot can gain an edge back to its importer after the import was already baked
 * in. Measured: with only this check in place, direct_import still produced LinkErrors with the Round 139
 * fingerprint intact -- one callee failing for four separate importers.
 *
 * It is kept because it is cheap and it is RIGHT about the cycles that are already visible, which is most
 * of them (SCC members are co-registered, so they see each other), and every edge it refuses is a method
 * that does not have to be deferred and retried at run time. It is a filter. Do not add to it in the
 * belief that enough of it would make admission's check unnecessary.
 *
 * An import binds wasmTable.get(fslot) at INSTANTIATION, so the callee's module has to be instantiated on
 * this worker before the caller's is. mono_wasm_jit_admit guarantees that by admitting a descriptor's
 * direct-dependency closure first -- but it is a DFS, and on a CYCLE it must stop somewhere: whichever
 * member it reaches while an ancestor is still `visiting` returns without instantiating, so the ancestor
 * instantiates FIRST. If that ancestor imported it, the import resolves against a slot still holding the
 * guarded interp-entry trampoline, a real function of a different type -- which is exactly why Round 139
 * saw "imported function does not match the expected type" rather than "is not a function", 14 LinkErrors
 * over 9 slots and a crash before titleReady in 2/2 arms.
 *
 * The rule that fixes it, per EDGE rather than per module:
 *
 *     A -> B may be an import IFF B cannot reach A.
 *
 * Why that is sufficient, against the admission DFS as written: when the DFS is about to instantiate u it
 * has already called mono_wasm_jit_admit on every dep of u. For an IMPORT dep v that call either returned
 * with v instantiated, or returned early on `wj_desc_state [v] == 1` -- v is an ancestor still on the DFS
 * stack, which means a path v -> ... -> u exists, which means v reaches u, which the rule forbids. So the
 * early return cannot happen on an import edge and v is always installed before u needs it. An edge inside
 * a cycle simply stays a call_indirect, which is immune because the table is patched after publication
 * instead of bound at instantiation.
 *
 * Note that the DFS can be ENTERED at either end of a cycle, so "which one loses the race" is not fixed --
 * which is why the test has to be on mutual reachability and not on anything about ordering.
 *
 * Note what this does NOT need: co-location. R139 concluded the fix was "emit an entire cycle into a single
 * module", which is true but needs the whole batching machinery; refusing the cycle edges is sound on its
 * own and costs only those edges.
 *
 * The graph walked here is the same one admission walks -- WjRegEntry.deps, f-slots -- so the two agree by
 * construction. A dep whose callee is not registered cannot be imported anyway (the ABI check below
 * rejects it), so an unresolvable f-slot ends the walk rather than widening it.
 *
 * The TARGET is identified by f-slot rather than by descriptor, because at the moment a lone method is
 * framed it HAS an f-slot (reserved before emission, which is what lets its callees bake it) but is not
 * registered yet and so has no descriptor at all. Matching on the slot is also what the admission walk
 * itself matches on.
 */
#define WJ_REACH_MAX 512   /* descriptors visited before giving up; giving up means REACHABLE, i.e. no import */

#ifdef HOST_BROWSER

static __thread guint32 *wj_reach_stamp;   /* per registry index; == wj_reach_gen means "seen this walk" */
static __thread int     *wj_reach_stack;
static __thread int      wj_reach_cap;
static __thread guint32  wj_reach_gen;

/* Can `from_desc` reach the method occupying `target_fslot`? Conservative: TRUE on any doubt. */
static gboolean
wj_asm_reaches (int from_desc, int target_fslot)
{
	int sp = 0, visited = 0, n = wj_reg_n;

	if (from_desc <= 0)
		return TRUE;      /* the callee is not registered; nothing to walk, so assume the worst */
	if (target_fslot <= 0) {
		/* The caller has no f-slot yet: it is a first emit whose pair is allocated after framing. A slot
		 * that does not exist cannot appear in any registry entry's dependency list, so nothing can reach
		 * it and it cannot be in a cycle. (A method that DOES already own or have reserved a slot -- a
		 * re-emit, a self-recursion reservation, an SCC batch reservation, or a pair parked by an aborted
		 * batch -- passes that slot in, precisely because those can have in-edges.) */
		return FALSE;
	}
	if (n > wj_reach_cap) {
		int ncap = wj_reach_cap ? wj_reach_cap : 1024;
		while (ncap < n) ncap *= 2;
		wj_reach_stamp = (guint32 *) g_realloc (wj_reach_stamp, sizeof (guint32) * (gsize) ncap);
		memset (wj_reach_stamp + wj_reach_cap, 0, sizeof (guint32) * (gsize) (ncap - wj_reach_cap));
		wj_reach_stack = (int *) g_realloc (wj_reach_stack, sizeof (int) * (gsize) ncap);
		wj_reach_cap = ncap;
	}
	/* Stamped rather than cleared: clearing a 24k-entry array per call site would cost more than the walk. */
	if (++wj_reach_gen == 0) {
		memset (wj_reach_stamp, 0, sizeof (guint32) * (gsize) wj_reach_cap);
		wj_reach_gen = 1;
	}
	wj_reach_stack [sp++] = from_desc;
	wj_reach_stamp [from_desc - 1] = wj_reach_gen;
	while (sp > 0) {
		WjRegEntry *re = wj_reg_at (wj_reach_stack [--sp] - 1);
		int i;
		if (!re)
			continue;
		if (++visited > WJ_REACH_MAX)
			return TRUE;   /* too big to settle: assume the worst and leave the edge indirect */
		for (i = 0; i < re->ndeps; ++i) {
			int d;
			if (re->deps [i] == target_fslot)
				return TRUE;
			d = wj_desc_for_fslot (re->deps [i]);
			if (d <= 0 || d > wj_reg_n)
				continue;
			if (wj_reach_stamp [d - 1] == wj_reach_gen)
				continue;
			wj_reach_stamp [d - 1] = wj_reach_gen;
			if (sp < wj_reach_cap)
				wj_reach_stack [sp++] = d;
			else
				return TRUE;
		}
	}
	return FALSE;
}
#else
/* The offline cross-compiler has no registry to walk and imports nothing anyway. */
static gboolean
wj_asm_reaches (int from_desc, int target_fslot)
{
	(void) from_desc; (void) target_fslot;
	return TRUE;
}
#endif

/*
 * May this JITted callee be reached through an import?
 *
 * Two things have to hold, and only the second is about the knob.
 *
 * 1. The callee's OWN emitted ABI must equal the signature this call site is using. `types[tpool]` comes
 *    from the CALLSITE signature; the function actually sitting in the f-slot was emitted from the
 *    callee's own. They disagree for vret-by-address, scalar-vtype and bridge/generic shapes.
 *    call_indirect tolerates that because it type-checks at CALL time against whatever the slot holds by
 *    then; an import is type-checked at INSTANTIATION and a mismatch is a LinkError that loses the whole
 *    method -- measured 2026-08-26 without this check at 41 LinkErrors and `registered` 13,309 -> 10,287,
 *    a 22.7% drop that would have presented as an unexplained fps regression.
 *
 *    The check reads the callee's REGISTERED f_sig_id rather than re-deriving its signature. That is not
 *    just cheaper: mono_method_signature_internal g_asserts on the method token (loader.c:1826), and
 *    calling it for every direct callee rather than a handful of profile-predicted ones is what made a
 *    rare survivable worker assertion look like a deterministic boot failure.
 *
 *    AND THE COMPARISON MUST BE STRUCTURAL, not on the f_sig_id hash. wj_functype_hash is FNV-32 over
 *    (nparams, params, ret), and enumerating every functype this backend can produce -- up to 8 params
 *    over {i32,i64,f32,f64} with a void-or-scalar return, 436,905 of them -- gives **8 COLLIDING PAIRS**.
 *    A collision here is not a missed optimisation, it is a LinkError: the import is declared with the
 *    caller's type, the slot holds a function of the other type, and instantiation refuses the whole
 *    module. MEASURED: with the hash comparison, 13 admissions failed per boot with
 *    "imported function does not match the expected type" while the instrumentation showed every
 *    dependency admitted (state 2) and live on that worker -- i.e. nothing to do with ordering, which is
 *    what the first two attempts at this were chasing.
 *
 *    The exact type comes from the callee's retained relocatable body, whose (param_types, nparams,
 *    ret_type) IS its `f` signature -- the same triple the encoder writes as its type[0]. No descriptor,
 *    no body, or a rebound batch generation that dropped it: refuse, because an unverifiable ABI is
 *    exactly the case that costs the whole method.
 *
 *    (The pre-existing dep_sig check in wj_admit_dependencies has the same collision exposure and is
 *    left alone: there a false match produces a call-time signature trap rather than a lost module, and
 *    making it exact would mean storing a functype per dependency -- 128 per method, ~230 MB tier-wide.)
 *
 * 2. A slot with no descriptor is a parked reservation -- a jiterpreter placeholder, not a function of
 *    this type -- so it can never be imported.
 *
 * 3. The callee must not be able to reach the CALLER, or the two are in a cycle and binding at
 *    instantiation cannot be ordered. See wj_asm_reaches; this is the Round 139 crash.
 *
 * 4. And it must not BE the caller. Self-recursion's f-slot is reserved before this module exists, so at
 *    its own instantiation wasmTable.get(self) is either empty -- a LinkError losing the whole method to
 *    the interpreter -- or still the guarded interp-entry trampoline, which would leave the recursion
 *    permanently interpreted. (The right fix for self-recursion is a module-local `call`, which is what
 *    WjAsmPolicy.local_calls does when it is on; it is not an import.)
 */
static gboolean
wj_asm_method_importable (const WasmReloc *r, const WasmFuncType *sig, int self_fslot)
{
#ifdef HOST_BROWSER
	int desc = wj_desc_for_fslot ((int) r->table_index);
	WjRegEntry *re = desc > 0 ? wj_reg_at (desc - 1) : NULL;

	WasmFuncType callee;

	if ((int) r->table_index == self_fslot)
		return FALSE;
	if (!re || !re->f_sig_id || !re->body)
		return FALSE;
	if (re->f_sig_id != wj_functype_hash (sig))
		return FALSE;   /* cheap pre-filter; a match here proves nothing on its own (see above) */
	memset (&callee, 0, sizeof (callee));
	g_assert (re->body->nparams <= WASM_FUNCTYPE_MAX_PARAMS);
	memcpy (callee.params, re->body->param_types, sizeof (WasmValtype) * re->body->nparams);
	callee.nparams = re->body->nparams;
	callee.ret = re->body->ret_type;
	if (!functype_eq (&callee, sig))
		return FALSE;
	return !wj_asm_reaches (desc, self_fslot);
#else
	/* The offline cross-compiler has no registry, no function table and no instantiation, so nothing is
	 * importable and the dump stays a self-contained module. */
	(void) r; (void) sig; (void) self_fslot;
	return FALSE;
#endif
}

/* Which member, if any, holds `callee`? -1 when it is not co-located. Linear because a module holds tens
 * of members, not thousands, and this runs once per call site at framing time. */
static int
wj_asm_member_of (const WjAsmMember *m, int n, MonoMethod *callee)
{
	int i;
	if (!callee)
		return -1;
	for (i = 0; i < n; ++i)
		if (m [i].method == callee)
			return i;
	return -1;
}

/* Resolve one body's holes. Called twice per member (method body, then entry thunk) in exactly the order
 * the emitter produced them, because import slots are assigned on first use and that order is what makes a
 * single-member module byte-identical to what the pre-relocation emitter emitted. */
static void
wj_asm_resolve_body (const WasmBuf *b, WasmRelocFix *fix, guint32 ti_base, int self_index,
                     const WjAsmMember *mem, int n, const WasmFuncType *self_types,
                     const WjAsmPolicy *pol, WasmFuncImport *himp, int *nhimp, WjAsmStats *st,
                     WjAsmImports *ai)
{
	const WasmRelocs *rl = b->relocs;
	guint32 k;

	for (k = 0; rl && k < rl->n; ++k) {
		const WasmReloc *r = &rl->r [k];
		guint32 ti = ti_base + r->tpool;
		int slot = -1, member;

		fix [k].form = WASM_FORM_INDIRECT;
		fix [k].idx = 0;
		switch ((WasmRelocKind) r->kind) {
		case WASM_RELOC_TYPE_U:
		case WASM_RELOC_TYPE_S:
		case WASM_RELOC_INDIRECT:
			continue;   /* a type index only; there is no form to choose */
		case WASM_RELOC_SELF:
			/* The entry thunk calling its own method. They are framed together by construction, so this
			 * is always module-local. The member index is turned into a function index below.
			 *
			 * NOT counted in the census: there is exactly one per member no matter what, so counting it
			 * would add a constant ~24k to the LOCAL column and drown the thing the census exists to
			 * show, which is what the CALL SITES IN METHOD BODIES resolved to. */
			fix [k].form = WASM_FORM_LOCAL;
			fix [k].idx = (guint32) self_index;
			continue;
		case WASM_RELOC_HELPER:
			if (pol->helper_imports)
				slot = wj_asm_intern_import (himp, nhimp, pol->max_fimports, r->table_index, ti);
			break;
		case WASM_RELOC_HELPER_CI:
			if (pol->ci_imports)
				slot = wj_asm_intern_import (himp, nhimp, pol->max_fimports, r->table_index, ti);
			break;
		case WASM_RELOC_AOT:
			if (pol->aot_imports)
				slot = wj_asm_intern_import (himp, nhimp, pol->max_fimports, r->table_index, ti);
			break;
		case WASM_RELOC_CALL:
			member = pol->local_calls ? wj_asm_member_of (mem, n, (MonoMethod *) r->sym) : -1;
			if (member >= 0) {
				fix [k].form = WASM_FORM_LOCAL;
				fix [k].idx = (guint32) member;
				st->nlocal++;
				continue;
			}
			if (pol->method_imports &&
			    wj_asm_method_importable (r, &self_types [r->tpool], mem [self_index].f_slot))
				slot = wj_asm_intern_import (himp, nhimp, pol->max_fimports, r->table_index, ti);
			break;
		}
		if (slot >= 0) {
			fix [k].form = WASM_FORM_IMPORT;
			fix [k].idx = (guint32) slot;
			st->nimport++;
			if (r->kind == WASM_RELOC_CALL)
				wj_asm_imports_add (ai, (int) r->table_index);
		} else {
			st->nindirect++;
		}
	}
}

/*
 * The knob set, read once per assembly.
 *
 * `local_calls` says a co-located callee may become `call <funcidx>`; it is off for a lone method so that
 * a self-recursive call keeps the shape it has always had, and on for a real batch, which is where
 * co-location is the entire point. Whether a JITted callee may be IMPORTED is not a policy question at all
 * -- it is decided per edge by wj_asm_method_importable, because soundness depends on that specific
 * caller/callee pair and not on the module.
 *
 * Under the offline cross-compiler there is no function table and no instantiation, so nothing may become
 * an import: the dump has to be a self-contained module the encoder can be validated against.
 */
static void
wj_asm_policy_init (WjAsmPolicy *pol, gboolean local_calls)
{
	extern int mono_wasm_jit_helper_imports, mono_wasm_jit_aot_imports, mono_wasm_jit_direct_import;
	extern int mono_wasm_jit_ci_imports, mono_wasm_jit_max_himp, mono_wasm_jit_names;

	memset (pol, 0, sizeof (*pol));
	pol->max_fimports = mono_wasm_jit_max_himp;
	pol->names = mono_wasm_jit_names ? 1 : 0;
	pol->local_calls = local_calls ? 1 : 0;
#ifdef HOST_BROWSER
	pol->helper_imports = mono_wasm_jit_helper_imports ? 1 : 0;
	pol->aot_imports = mono_wasm_jit_aot_imports ? 1 : 0;
	pol->ci_imports = mono_wasm_jit_ci_imports ? 1 : 0;
	pol->method_imports = mono_wasm_jit_direct_import ? 1 : 0;
#endif
}

/*
 * Frame `n` members into one module.
 *
 * `out` receives the module bytes. Returns FALSE only on an internal inconsistency; running out of import
 * slots is not a failure, it just leaves those sites as call_indirect.
 */
static gboolean
wj_assemble (const WjAsmMember *mem, int n, const WjAsmPolicy *pol, WasmBuf *out, WjAsmStats *out_st,
             WjAsmImports *out_imp)
{
	static const WasmValtype entry_params [2] = { WASM_I32, WASM_I32 };
	WasmFuncImport *himp;
	WasmAsmMember *am;
	WasmFuncType **types;
	WasmBuf *bodies;
	WasmRelocFix **fixes;
	WjAsmStats st;
	guint32 *ti_base;
	gboolean import_table = FALSE, import_eh_tag = FALSE;
	guint32 eh_type_idx = 0;
	int nhimp = 0, i;

	if (n <= 0)
		return FALSE;
	memset (&st, 0, sizeof (st));
	himp = g_new0 (WasmFuncImport, WJ_MAX_HELPER_IMPORTS);
	ti_base = g_new0 (guint32, n);
	fixes = g_new0 (WasmRelocFix *, 2 * n);
	bodies = g_new0 (WasmBuf, 2 * n);
	types = g_new0 (WasmFuncType *, n);
	am = g_new0 (WasmAsmMember, n);

	for (i = 0; i < n; ++i) {
		guint32 j, nt = 2 + mem [i].nextra;
		g_assert (mem [i].nparams <= WASM_FUNCTYPE_MAX_PARAMS);
		ti_base [i] = i ? ti_base [i - 1] + 2 + mem [i - 1].nextra : 0;
		types [i] = g_new0 (WasmFuncType, nt);
		types [i][0].nparams = mem [i].nparams;
		for (j = 0; j < mem [i].nparams; ++j)
			types [i][0].params [j] = mem [i].param_types [j];
		types [i][0].ret = mem [i].ret_type;
		types [i][1].nparams = 2;
		types [i][1].params [0] = entry_params [0];
		types [i][1].params [1] = entry_params [1];
		types [i][1].ret = WASM_VOID;
		for (j = 0; j < mem [i].nextra; ++j)
			types [i][2 + j] = mem [i].extra_types [j];
		if (mem [i].uses_calls)
			import_table = TRUE;
		if (mem [i].uses_eh_tag && !import_eh_tag) {
			import_eh_tag = TRUE;
			/* The tag's functype is a module type index like any other, so it moves with the member's
			 * block. First EH-using member wins; every other one's (i32)->void is structurally identical.
			 * A member that wants the tag without having interned the type is not expected, but falling
			 * back to its block's first entry keeps the module well-formed instead of aborting a compile
			 * -- which is what the pre-relocation framer did with its `eh_type_idx < 0 ? 0` guard. */
			eh_type_idx = ti_base [i] + (mem [i].eh_tpool >= 0 ? (guint32) mem [i].eh_tpool : 0);
		}
	}

	/* Pass 1 -- decide every form, assigning import slots in first-use order. LOCAL results hold a MEMBER
	 * index at this point, because the function index they need is nfimports + member and nfimports is not
	 * final until the last hole has been looked at. */
	for (i = 0; i < n; ++i) {
		const WasmRelocs *fr = mem [i].f_body->relocs, *er = mem [i].e_body->relocs;
		fixes [2 * i] = g_new0 (WasmRelocFix, fr && fr->n ? fr->n : 1);
		fixes [2 * i + 1] = g_new0 (WasmRelocFix, er && er->n ? er->n : 1);
		wj_asm_resolve_body (mem [i].f_body, fixes [2 * i], ti_base [i], i, mem, n, types [i], pol, himp, &nhimp, &st,
		                     out_imp ? &out_imp [i] : NULL);
		wj_asm_resolve_body (mem [i].e_body, fixes [2 * i + 1], ti_base [i], i, mem, n, types [i], pol, himp, &nhimp, &st,
		                     out_imp ? &out_imp [i] : NULL);
	}

	/* Pass 2 -- imports are counted, so a member index becomes a function index, and the bodies can be
	 * written out. Defined functions start at nhimp and the methods precede the thunks. */
	for (i = 0; i < 2 * n; ++i) {
		const WasmBuf *src = (i & 1) ? mem [i / 2].e_body : mem [i / 2].f_body;
		const WasmRelocs *rl = src->relocs;
		guint32 k;
		for (k = 0; rl && k < rl->n; ++k)
			if (fixes [i][k].form == WASM_FORM_LOCAL)
				fixes [i][k].idx += (guint32) nhimp;
		wasm_buf_init (&bodies [i]);
		wasm_body_serialize (src, fixes [i], ti_base [i / 2], &bodies [i]);
	}

	for (i = 0; i < n; ++i) {
		am [i].locals = mem [i].groups;
		am [i].nlocal_groups = mem [i].nlocal_groups;
		am [i].f_body = &bodies [2 * i];
		am [i].e_body = &bodies [2 * i + 1];
		am [i].types = types [i];
		am [i].ntypes = 2 + mem [i].nextra;
	}
	wasm_module_assemble (am, (guint32) n, import_table, import_eh_tag, eh_type_idx,
	                      himp, (guint32) nhimp, out);
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		mono_wasm_jit_add (WJC_CALL_LOCAL, st.nlocal);
		mono_wasm_jit_add (WJC_CALL_IMPORT, st.nimport);
		mono_wasm_jit_add (WJC_CALL_INDIRECT, st.nindirect);
	}

	/* Symbolise. Without this every member is wasm-function[N] to V8, so perf attributes all JITted time to
	 * one anonymous bucket -- which once moved ~45% of in-game samples into 4,250 unnamed symbols and made
	 * whole method families read as 0% of the profile. */
	if (pol->names) {
		char **names = g_new0 (char *, n);
		char modname [256];
		for (i = 0; i < n; ++i)
			names [i] = mem [i].method ? mono_method_get_full_name (mem [i].method) : NULL;
		if (n == 1)
			wasm_module_append_name_section (out, names [0] ? names [0] : "wasmjit",
			                                 names [0] ? names [0] : "method", (guint32) nhimp);
		else {
			g_snprintf (modname, sizeof (modname), "wasmjit-batch[%d]", n);
			wasm_module_append_name_section_multi (out, modname, (const char *const *) names,
			                                       (guint32) n, (guint32) nhimp);
		}
		for (i = 0; i < n; ++i)
			g_free (names [i]);
		g_free (names);
	}

	for (i = 0; i < 2 * n; ++i) {
		wasm_buf_free (&bodies [i]);
		g_free (fixes [i]);
	}
	for (i = 0; i < n; ++i)
		g_free (types [i]);
	g_free (types); g_free (bodies); g_free (fixes); g_free (am); g_free (ti_base); g_free (himp);
	if (out_st)
		*out_st = st;
	return TRUE;
}

#ifdef HOST_BROWSER
/* --- island module batching -----------------------------------------------------------------------
 *
 * Several methods emitted into ONE WebAssembly.Module instead of one module each. This is the single
 * highest-value codegen change measured on this workload: V8 will not inline across a module boundary
 * under any circumstances, but within one module it inlines through `call` AND speculatively through
 * `call_indirect` — so co-location alone takes an accessor-sized call from ~1.5 ns to the ~0.25 ns
 * no-call floor, with no change to how any call site is emitted. (scratchpad/callbench.mjs; flat out to
 * 500 functions per module per scratchpad/scalerun.mjs.)
 *
 * The emitter stays almost entirely unaware. Two things have to be batch-relative while a body is being
 * generated, because both are ULEB-encoded inline in the instruction stream and so cannot be fixed up
 * afterwards:
 *   - type indices, handled by ti_base (see mono_wasm_emit_method);
 *   - the entry thunk's `call <method>`, which is a constant 0 standalone but must be the member index
 *     in a batch. Methods occupy funcidx 0..N-1 precisely so this is knowable from membership alone.
 *
 * When a batch is active mono_wasm_emit_method captures its output here instead of framing a module,
 * instantiating and registering; the driver frames all members at once afterwards.
 */
#define WJ_BATCH_MAX 512   /* a whole-program batch is the upper-bound experiment; islands are far smaller */

typedef struct {
	MonoMethod *method;
	WasmValtype param_types [WASM_FUNCTYPE_MAX_PARAMS];
	guint32 nparams;
	WasmValtype ret_type;
	WasmLocalGroup groups [4];
	WasmBuf f_body;                 /* ownership moves here; NOT freed by the emitter */
	WasmBuf e_body;
	WasmFuncType *extra_types;      /* g_malloc'd, nextra entries; inline storage would be MBs at 512 members */
	guint32 nextra;
	int *deps;                      /* direct f-slot closure captured by THIS batch re-emission */
	guint32 *dep_sig;
	MonoMethod **dep_method;
	int ndeps;
	/* Per-member module requirements, unioned by the assembler. Previously these were tracked on the batch
	 * as it filled, because the framing call needed them before any member could be written out; they are
	 * per-member facts and now live where they belong. */
	guint8 uses_calls;
	guint8 uses_eh_tag;
	int eh_tpool;                   /* pool index of the tag's (i32)->void functype, or -1 */
	gboolean captured;
} WjBatchMember;

typedef struct {
	int n;                          /* members captured so far */
	int cur;                        /* index of the member being emitted (its funcidx, and its thunk's callee) */
	gboolean in_member;             /* a member emit is in progress; a NESTED emit must not join */
	/* The type base, the shared helper-import array and the union flags all used to live here, because a
	 * member's body baked type indices and `call` immediates against the batch as it filled -- so the batch
	 * had to know its own running totals while it was still being built. Nothing is baked any more: the
	 * assembler walks the members' relocations once, assigns import slots in first-use order and computes
	 * each member's type base from the same rule the encoder lays the section out with. A batch is now just
	 * a list of bodies. */
	WjBatchMember m [WJ_BATCH_MAX];
} WjBatch;

/* Non-NULL only between mono_wasm_jit_batch_begin/end, on the compiling thread. The whole batch runs
 * under wj_compiling, so a single thread-local is enough. */
static __thread WjBatch *wj_batch;

/* Where wj_assemble reports which deps a BATCH member binds at instantiation; see wj_self_imports. */
static __thread WjAsmImports wj_batch_imports [WJ_BATCH_MAX];

int mono_wasm_jit_batch_begin (void);
void mono_wasm_jit_batch_end (void);
int mono_wasm_jit_batch_count (void);

/* Start capturing. Returns 0 if a batch is already open (nesting is not supported — a re-entrant
 * compile must fall back to the standalone path rather than corrupt the open batch). */
int
mono_wasm_jit_batch_begin (void)
{
	if (wj_batch)
		return 0;
	wj_batch = g_new0 (WjBatch, 1);
	memset (wj_batch_imports, 0, sizeof (wj_batch_imports));
	wj_batch->cur = -1;
	wj_batch_profile_suppressed = TRUE;
	return 1;
}

/* Predeclaring the membership used to matter: a member's body baked `call <memberidx>` for a fellow
 * member, so a FORWARD callee had to be known before anything was emitted. Nothing is baked now, and
 * co-location is decided at assembly from the members actually handed to it -- by which time every one has
 * been captured -- so this is just batch_begin with an arity check. Kept as a distinct entry point because
 * the planner calls it and the check is worth having. */
int
mono_wasm_jit_batch_begin_members (MonoMethod *const *members, int n)
{
	if (!members || n <= 0 || n > WJ_BATCH_MAX)
		return 0;
	return mono_wasm_jit_batch_begin ();
}

void
mono_wasm_jit_batch_end (void)
{
	int i;
	if (!wj_batch)
		return;
	for (i = 0; i < wj_batch->n; i++) {
		g_free (wj_batch_imports [i].slot);
		memset (&wj_batch_imports [i], 0, sizeof (wj_batch_imports [i]));
		wasm_buf_free (&wj_batch->m [i].f_body);
		wasm_buf_free (&wj_batch->m [i].e_body);
		g_free (wj_batch->m [i].extra_types);
		g_free (wj_batch->m [i].deps);
		g_free (wj_batch->m [i].dep_sig);
		g_free (wj_batch->m [i].dep_method);
	}
	g_free (wj_batch);
	wj_batch = NULL;
	wj_batch_profile_suppressed = FALSE;
}

int
mono_wasm_jit_batch_count (void)
{
	return wj_batch ? wj_batch->n : 0;
}

/* Frame every captured member into ONE module, instantiate it, and hand the bytes back.
 *
 * `e_slots`/`f_slots` are the caller's pre-reserved pairs, in member order — the batch driver reserves
 * them up front (that is what lets members bake each other's f-slots), so they are already correct here.
 * `out_bytes` receives a g_malloc'd copy of the module the caller must keep alive: every thread
 * re-instantiates from it, and all members share the one blob.
 *
 * Returns the member count on success, 0 on failure (errbuf carries why). Members are not registered
 * here — the driver does that, because registration also publishes slots and must be ordered against
 * the rest of the batch.
 */
int mono_wasm_jit_batch_finish (const int *e_slots, const int *f_slots, void **out_bytes, int *out_len,
                                char *errbuf, int errcap);
int
mono_wasm_jit_batch_finish (const int *e_slots, const int *f_slots, void **out_bytes, int *out_len,
                            char *errbuf, int errcap)
{
	WjAsmMember *members;
	WasmBuf out;
	void *cached;
	double ms = 0;
	int i, n;

	if (!wj_batch || wj_batch->n <= 0)
		return 0;
	n = wj_batch->n;
	/* A batch re-emission can discover direct targets that were not available when a member's
	 * standalone registry entry was produced. Every baked f-slot must already have an authoritative
	 * descriptor; a parked reservation is still a jiterpreter placeholder and would turn into a
	 * call_indirect signature trap. Reject before batch_finish overwrites any table slots. */
	for (i = 0; i < n; i++) {
		WjBatchMember *bm = &wj_batch->m [i];
		int d;
		for (d = 0; d < bm->ndeps; ++d) {
			if (!wj_desc_for_fslot (bm->deps [d])) {
				if (errbuf && errcap)
					g_snprintf (errbuf, errcap, "member %d has unregistered dependency fslot %d",
						i, bm->deps [d]);
				return 0;
			}
		}
	}
	members = g_new0 (WjAsmMember, n);
	for (i = 0; i < n; i++) {
		WjBatchMember *bm = &wj_batch->m [i];
		if (!bm->captured) {
			if (errbuf && errcap) g_snprintf (errbuf, errcap, "batch member %d was never captured", i);
			g_free (members);
			return 0;
		}
		members [i].method = bm->method;
		members [i].f_slot = f_slots [i];
		members [i].f_body = &bm->f_body;
		members [i].e_body = &bm->e_body;
		members [i].param_types = bm->param_types;
		members [i].nparams = bm->nparams;
		members [i].ret_type = bm->ret_type;
		members [i].extra_types = bm->extra_types;
		members [i].nextra = bm->nextra;
		members [i].groups = bm->groups;
		members [i].nlocal_groups = 4;
		members [i].uses_calls = bm->uses_calls;
		members [i].uses_eh_tag = bm->uses_eh_tag;
		members [i].eh_tpool = bm->eh_tpool;
	}

	/* Everything that used to happen between here and the framing call -- patching every entry thunk's
	 * `call <method>` and every intra-batch callee index now that the import count was finally known -- is
	 * gone. Those were 5-byte placeholders precisely because the values were not knowable at emit time;
	 * they are relocations now, and the assembler resolves them from the membership it is handed.
	 *
	 * `local_calls` is on: co-locating members so their calls to each other become `call <funcidx>` is the
	 * entire reason a batch exists. */
	{
		WjAsmPolicy pol;
		wj_asm_policy_init (&pol, TRUE);
		wasm_buf_init (&out);
		wj_assemble (members, n, &pol, &out, NULL, wj_batch_imports);
	}
	g_free (members);
	cached = g_malloc ((gsize) out.len);
	memcpy (cached, out.data, out.len);
	*out_len = (int) out.len;
	wasm_buf_free (&out);

	if (!mono_wasm_jit_instantiate_batch_local (e_slots, f_slots, n, cached, *out_len, errbuf, errcap, &ms)) {
		g_free (cached);
		*out_bytes = NULL;
		*out_len = 0;
		return 0;
	}
	*out_bytes = cached;
	if (mono_wasm_jit_verbose >= 2)
		printf ("WASM_JIT_BATCH_MODULE members=%d bytes=%d instantiate=%.2fms\n", n, *out_len, ms);
	return n;
}

/* Enumerate the JIT registry, so a caller can rebuild ALL registered methods as one module (the
 * whole-program co-location experiment). Returns the entry count; mono_wasm_jit_reg_entry fills in the
 * method and its slots for index i, or returns 0 if the entry is unusable (not registered or missing
 * its logical method). Existing batch members remain enumerable for generational merging. */
int mono_wasm_jit_reg_count (void);
int
mono_wasm_jit_reg_count (void)
{
	return wj_reg_n;
}

int mono_wasm_jit_reg_entry (int i, MonoMethod **out_method, int *out_e, int *out_f, int *out_desc);
int
mono_wasm_jit_reg_entry (int i, MonoMethod **out_method, int *out_e, int *out_f, int *out_desc)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n)
		return 0;
	re = wj_reg_at (i);
	if (!re || re->e <= 0 || re->f <= 0)
		return 0;
	/* Automatic rebatching may deliberately move a complete old batch into a larger generation.
	 * The planner treats old batches as indivisible, so none of their siblings can be orphaned. */
	/* logical_method is only filled in by mono_wasm_jit_bind_logical, which the SCC publish path calls;
	 * ordinary force_island registrations leave it NULL, so fall back to the method whose IR was emitted. */
	if (!re->logical_method && !re->body_method)
		return 0;
	*out_method = re->logical_method ? re->logical_method : re->body_method;
	*out_e = re->e;
	*out_f = re->f;
	*out_desc = i + 1;
	return 1;
}

/* Read-only graph view used by the automatic batch planner.  Unlike reg_entry this also exposes
 * dependency topology and reports already-batched entries rather than silently dropping them. */
int
mono_wasm_jit_reg_graph_entry (int i, MonoMethod **out_method, int *out_len, int *out_ndeps,
	gboolean *out_batched)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n || !(re = wj_reg_at (i)))
		return 0;
	if (!re->logical_method && !re->body_method)
		return 0;
	if (out_method) *out_method = re->logical_method ? re->logical_method : re->body_method;
	if (out_len) *out_len = re->body_len;
	if (out_ndeps) *out_ndeps = re->ndeps;
	if (out_batched) *out_batched = re->batch != NULL;
	return 1;
}

/* The method identity whose IR was actually emitted for a registry entry.  This can differ from the
 * logical InterpMethod owner (notably synchronized/intrinsic wrappers), and batch capture/predeclared
 * direct-call membership must use this identity or a correct re-emit looks like divergence. */
MonoMethod *
mono_wasm_jit_reg_body_method (int i)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n || !(re = wj_reg_at (i)))
		return NULL;
	return re->body_method;
}

/* Some runtime intrinsics have a useful standalone descriptor but force-compiling their logical
 * method does not reproduce a normal captured body.  They must remain external f-slot calls when
 * automatic batches are re-emitted.  If such a member is ever discovered while growing an existing
 * batch, quarantine that complete old generation: excluding only one sibling would let a replacement
 * module orphan the other members of the still-live old module. */
int
mono_wasm_jit_reg_mark_batch_incompatible (MonoMethod *method)
{
	WjBatchDesc *batch = NULL;
	int i, marked = 0;

	if (!method)
		return 0;
	mono_loader_lock ();
	for (i = 0; i < wj_reg_n; ++i) {
		WjRegEntry *re = wj_reg_at (i);
		if (re && (re->logical_method == method || re->body_method == method)) {
			batch = re->batch;
			break;
		}
	}
	for (i = 0; i < wj_reg_n; ++i) {
		WjRegEntry *re = wj_reg_at (i);
		if (!re || re->batch_incompatible)
			continue;
		if ((batch && re->batch == batch) ||
		    (!batch && (re->logical_method == method || re->body_method == method))) {
			re->batch_incompatible = 1;
			marked++;
		}
	}
	mono_loader_unlock ();
	return marked;
}

int
mono_wasm_jit_reg_batch_incompatible (int i)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n || !(re = wj_reg_at (i)))
		return 1;
	return re->batch_incompatible != 0;
}

guint32
mono_wasm_jit_reg_batch_generation (int i)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n || !(re = wj_reg_at (i)))
		return 0;
	return re->batch ? re->generation : 0;
}

MonoMethod *
mono_wasm_jit_reg_graph_dep (int i, int dep)
{
	WjRegEntry *re;
	if (i < 0 || i >= wj_reg_n || !(re = wj_reg_at (i)) ||
	    dep < 0 || dep >= re->ndeps || !re->dep_method)
		return NULL;
	return re->dep_method [dep];
}

/* Attach one shared WjBatchDesc to every member's registry entry, so whichever member a worker admits
 * first instantiates the whole module and the rest skip it. Takes ownership of `bytes`.
 *
 * `desc_ids` are the descriptor ids returned by mono_wasm_jit_register for the members, in the same
 * order as e_slots/f_slots. Returns 0 (and frees nothing) if any id is bad — the caller then still owns
 * `bytes`. */
int mono_wasm_jit_batch_bind (const int *desc_ids, const int *e_slots, const int *f_slots, int n,
                              void *bytes, int len);
int
mono_wasm_jit_batch_bind (const int *desc_ids, const int *e_slots, const int *f_slots, int n,
                          void *bytes, int len)
{
	WjBatchDesc *bd;
	int i;

	if (n <= 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (desc_ids [i] <= 0 || !wj_reg_at (desc_ids [i] - 1))
			return 0;
	}
	bd = g_new0 (WjBatchDesc, 1);
	bd->n = n;
	bd->e = (int *) g_malloc (sizeof (int) * n);
	bd->f = (int *) g_malloc (sizeof (int) * n);
	bd->desc = (int *) g_malloc (sizeof (int) * n);
	memcpy (bd->e, e_slots, sizeof (int) * n);
	memcpy (bd->f, f_slots, sizeof (int) * n);
	memcpy (bd->desc, desc_ids, sizeof (int) * n);
	bd->bytes = bytes;
	bd->len = len;
	bd->generation = (guint32) mono_atomic_inc_i32 (&wj_batch_generation);
	for (i = 0; i < n; i++) {
		WjRegEntry *re = wj_reg_at (desc_ids [i] - 1);
		WjBatchMember *bm = wj_batch && i < wj_batch->n ? &wj_batch->m [i] : NULL;
		int *new_deps = NULL;
		guint32 *new_dep_sig = NULL;
		MonoMethod **new_dep_method = NULL;
#ifdef HOST_BROWSER
		/* batch_finish has already overwritten these slots on the compiling worker. Any previously
		 * installed guard-free AOT adapter must stop entering them until admission below succeeds. */
		if (re->logical_method)
			mono_jiterp_wasm_jit_unpatch_interp_entry (mono_interp_get_imethod (re->logical_method));
#endif
		re->batch = bd;
		/* Point the per-entry bytes at the shared module too, so anything reading re->bytes sees the
		 * real module rather than the discarded standalone one. */
		re->bytes = bytes;
		re->len = len;
		/* Code generation is not topology-invariant: as more callees become JITted, a later batch
		 * re-emission can replace a residual with a new direct f-slot call. Publish the dependency
		 * set captured from this exact body generation. Old arrays intentionally remain allocated:
		 * another worker may still be walking the prior generation lock-free. */
		if (bm) {
			if (bm->ndeps > 0) {
				new_deps = g_new (int, bm->ndeps);
				new_dep_sig = g_new (guint32, bm->ndeps);
				new_dep_method = g_new0 (MonoMethod *, bm->ndeps);
				memcpy (new_deps, bm->deps, sizeof (int) * bm->ndeps);
				memcpy (new_dep_sig, bm->dep_sig, sizeof (guint32) * bm->ndeps);
				memcpy (new_dep_method, bm->dep_method, sizeof (MonoMethod *) * bm->ndeps);
			}
			re->deps = new_deps;
			re->dep_sig = new_dep_sig;
			re->dep_method = new_dep_method;
			re->ndeps = bm->ndeps;
			/* The new generation's imports are a different set, and a stale dep_strict would be indexed
			 * against the old dependency array. Re-derive from what this assembly reported, BEFORE the
			 * barrier below publishes the generation -- same visibility rule as mono_wasm_jit_register. */
			re->dep_strict = NULL;
			if (bm && new_deps && wj_batch_imports [i].n > 0) {
				int si, sk;
				re->dep_strict = g_new0 (guint8, bm->ndeps);
				for (si = 0; si < bm->ndeps; ++si)
					for (sk = 0; sk < wj_batch_imports [i].n; ++sk)
						if (new_deps [si] == wj_batch_imports [i].slot [sk]) {
							re->dep_strict [si] = 1;
							break;
						}
			}
		}
		mono_memory_barrier ();
		re->generation = bd->generation;
		/* batch_finish physically installed this generation on the compiling worker, but installation
		 * is not admission. Invalidate the old standalone/batch admission cache before walking the
		 * complete new batch's external dependency union below. */
		wj_desc_state_ensure (desc_ids [i] + 1);
		wj_desc_state [desc_ids [i]] = 0;
		wj_desc_generation [desc_ids [i]] = bd->generation;
	}
	/* Admit from one member: mono_wasm_jit_admit premarks every sibling, admits the union of all their
	 * external closures, and only then republishes every e/f slot live. Keep the registry binding even
	 * if admission fails; callers will remain on guarded interpreter paths and the diagnostic identifies
	 * the invalid dependency rather than freeing bytes still owned by the registry. */
	if (!mono_wasm_jit_admit (desc_ids [0]))
		printf ("WASM_JIT_BATCH_ADMIT_FAIL generation=%u members=%d\n", bd->generation, n);
	return 1;
}

/* The MonoMethod captured as member i, so the driver can register the right slot pair against it. */
MonoMethod *mono_wasm_jit_batch_member_method (int i);
MonoMethod *
mono_wasm_jit_batch_member_method (int i)
{
	if (!wj_batch || i < 0 || i >= wj_batch->n)
		return NULL;
	return wj_batch->m [i].method;
}

#endif /* HOST_BROWSER */

/*
 * Conservative structured-CFG recognizer.
 *
 * The universal lowering below handles every graph with a dispatch loop + br_table.  Most hot Java
 * methods need much less: either every edge is forward, or all backward edges return to one natural
 * loop header.  The latter is exactly the shape of a while/for loop after MINI's block ordering.
 *
 * Do not attempt a general relooper here.  Accept only graphs whose dense layout proves the lexical
 * structure directly:
 *   - no edge from outside the loop enters its interior (the header is the sole entry);
 *   - every backward edge in the loop targets that header;
 *   - prefix and suffix CFGs are acyclic.
 * Anything nested, discontiguous or irreducible keeps the old dispatcher unchanged.
 */
enum {
	WJ_CFG_DISPATCH = 0,
	WJ_CFG_FORWARD = 1,
	WJ_CFG_SINGLE_LOOP = 2
};

static int
wj_structured_cfg_kind (MonoCompile *cfg, int *bbidx, int n, int *out_h, int *out_l)
{
	MonoBasicBlock *bb;
	int i, h = -1, l = -1;

	if (!mono_wasm_jit_structured_cfg || !cfg || !cfg->header ||
	    cfg->header->num_clauses != 0 || n <= 0)
		return WJ_CFG_DISPATCH;

	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int k;
		if (bb->flags & BB_EXCEPTION_HANDLER)
			return WJ_CFG_DISPATCH;
		for (k = 0; k < bb->out_count; ++k) {
			MonoBasicBlock *tb = bb->out_bb [k];
			int t;
			if (!tb || tb->block_num < 0 || tb->block_num > (int) cfg->max_block_num)
				return WJ_CFG_DISPATCH;
			t = bbidx [tb->block_num];
			if (t < 0)
				return WJ_CFG_DISPATCH;
			if (t <= i) {
				if (h < 0)
					h = t;
				else if (h != t)
					return WJ_CFG_DISPATCH; /* nested/multiple loop headers */
				if (i > l)
					l = i;
			}
		}
	}
	if (h < 0) {
		*out_h = *out_l = -1;
		return WJ_CFG_FORWARD;
	}
	if (h > l || l >= n)
		return WJ_CFG_DISPATCH;

	/* Prove the loop is a contiguous, single-entry region and that no secondary backedge exists. */
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int k;
		for (k = 0; k < bb->out_count; ++k) {
			int t = bbidx [bb->out_bb [k]->block_num];
			if (i < h) {
				if (t <= i || (t > h && t <= l))
					return WJ_CFG_DISPATCH;
			} else if (i <= l) {
				if (t <= i && t != h)
					return WJ_CFG_DISPATCH;
				if (t < h)
					return WJ_CFG_DISPATCH;
			} else if (t <= i) {
				return WJ_CFG_DISPATCH;
			}
			if ((i < h || i > l) && t > h && t <= l)
				return WJ_CFG_DISPATCH;
		}
	}

	*out_h = h;
	*out_l = l;
	return WJ_CFG_SINGLE_LOOP;
}

/* Label depth for an edge in one of the verified structured layouts. */
static int
wj_structured_branch_depth (int kind, int h, int l, int n, int from, int to)
{
	int s = l + 1;
	if (kind == WJ_CFG_FORWARD)
		return to > from ? to - from - 1 : -1;
	if (kind != WJ_CFG_SINGLE_LOOP)
		return -1;
	if (from < h) {
		if (to > from && to < h)
			return to - from - 1;
		if (to == h)
			return h - from - 1; /* dedicated loop-entry block */
		if (to >= s && to < n)
			return (h - from) + (to - s);
		return -1;
	}
	if (from <= l) {
		if (to == h)
			return l - from; /* remaining body blocks, then the loop label */
		if (to > from && to <= l)
			return to - from - 1;
		if (to >= s && to < n)
			return (l - from) + 1 + (to - s); /* body labels + loop + suffix label */
		return -1;
	}
	return to > from && to < n ? to - from - 1 : -1;
}

static gboolean
wj_structured_target_has_label (int kind, int h, int l, int from, int to)
{
	if (kind == WJ_CFG_FORWARD)
		return to > from;
	if (kind != WJ_CFG_SINGLE_LOOP)
		return FALSE;
	if (from < h)
		return (to > from && to <= h) || to >= l + 1;
	if (from <= l)
		return to == h || (to > from && to <= l) || to >= l + 1;
	return to > from;
}

/* Number of lexical control labels between bb code and the shared throw blocks. */
static int
wj_structured_outer_depth (int kind, int h, int l, int n, int from)
{
	if (kind == WJ_CFG_FORWARD)
		return n - 1 - from;
	if (kind == WJ_CFG_SINGLE_LOOP) {
		int s = l + 1;
		if (from < h)
			return (h - from) + (n - s);
		if (from <= l)
			return (l - from) + 1 + (n - s);
		return n - 1 - from;
	}
	return -1;
}



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
	WasmValtype *param_types = (WasmValtype *) mono_mempool_alloc0 (cfg->mempool, sizeof (WasmValtype) * (nargs + 1)); /* +1: trailing hidden-vret param */
	WasmValtype ret_vt;
	WasmCallInfo self_ci;              /* the method's OWN callee ABI (self-sig); also the registered f_sig_id, so callers' baked dep_sig and our registration can never disagree */
	int nwparams;                      /* wasm param count of func f = nargs + (hidden vret ? 1 : 0). ALL local-index math is based on this, not nargs: wasm locals are numbered after the params */
	gboolean self_has_byaddr = FALSE;  /* stats: method takes >=1 by-addr vtype arg */
	WasmBuf body, out;
	WjBody *wj_stored_body = NULL;   /* the relocatable body, handed to the registry on successful registration */
	MonoBasicBlock *bb;
	MonoInst *ins;
	int i, cnt [4] = { 0, 0, 0, 0 }, base [4], run [4] = { 0, 0, 0, 0 };
	WasmLocalGroup groups [4];
	int dispatch_idx = 0, N = 0;
	int scratch_idx G_GNUC_UNUSED = 0; /* i32 local holding the per-thread interp-residual scratch ptr */
	int refbase_idx = 0, rtmp_idx = 0; /* i32 locals: ref-frame base addr + scratch for ref stores */
	int spentry_idx = 0;               /* i32 local: C-stack SP at entry (exit/landing-pad restore target) */
	int framebytes = 0, refbytes_al = 0; /* C-stack frame size (16-aligned) / 8-aligned ref-slot bytes */
	gboolean method_nogc = FALSE;        /* transitive direct-call effect published in WjRegEntry */
	int effective_gcp_count = 0;         /* static returning GC points after direct no-GC resolution */
	gboolean lazy_ref_frame = FALSE;     /* defer a pure ref frame until the first effective GC point */
	MonoInst *terminal_vcall_ins = NULL; /* last GC point in an acyclic forwarding wrapper; owns root handoff + guarded specialization */
	int terminal_vcall_ord = -1;         /* layout ordinal used to prove every elided reference dies at/before that handoff */
	gboolean terminal_vcall_handoff = FALSE; /* all pre-call references die at the terminal vcall; poll-prefix frames may stay cold */
	gboolean terminal_vcall_poll_prefix = FALSE; /* every earlier effective GC point is a conditional OP_GC_SAFE_POINT poll */
	int vc_fslot_idx = 0;              /* i32 local: inline virtual-IC fast-path resolved f-slot */
	int vc_aotkind_idx = 0;            /* i32 local: VCALL_AOT dispatch kind from vcall_aot_target (0=residual,1=+rgctx,2=no-extra) */
	int aic_vtab_idx = 0, aic_ti_idx = 0, aic_rgctx_idx = 0; /* i32 locals: AOT-vcall IC — this->vtable, ti<<1|kind2, rgctx */
	int dic_base_idx = 0;              /* i32 local: delegate-IC entry base for the way under test (wire-byte hoist) */
	int slotlive_ptr_idx = 0, slotlive_cap_idx = 0; /* i32 locals: cached &wj_slot_live / &wj_slot_live_cap for the INLINE f-slot-IC liveness check (dead in methods with no vcall) */
	/* Only the POINTER addresses need locals: the pointer is re-loaded through its address on every dispatch
	 * (so a realloc is picked up), while each capacity is loaded once in the prologue straight into its value
	 * local — the cap address is dead immediately and never earned a slot. */
	int vpic_ptr_idx = 0;              /* i32 local: address of this worker's TLS vcall PIC pointer */
	int dpic_ptr_idx = 0;              /* i32 local: same, for this worker's TLS delegate recipe PIC */
	/* i32 locals holding the VALUE of each TLS PIC capacity, loaded once in the prologue. Every dispatch
	 * used to re-load it through the imported address just to bounds-check its own site id. Hoisting the
	 * LOAD (not the comparison, which needs the site id and so cannot precede body emission) is safe in the
	 * only direction that matters: capacity is monotonically non-decreasing (wj_*_pic_for_site only ever
	 * grows it), so a stale cached value is an UNDER-estimate, and an under-estimate sends the dispatch to
	 * the slow path — a missed IC hit, never a read past the array. */
	int vpic_capval_idx = 0, dpic_capval_idx = 0;
	gboolean has_vcall = FALSE;        /* TRUE: method has >=1 OP_*CALL_MEMBASE (a vcall-IC site) -> emit the prologue slotlive fetch */
	int vc_ic_idx = 0;                 /* i64 local: inline virtual-IC fast-path IC value (vtable|imethod<<32) */
	int eh_exc_idx = 0, eh_h_idx = 0;  /* i32 locals: in-method EH catch landing pad — saved C++ exc ptr + dispatch result */
	int ilstate_idx = -1;              /* i32 local: this activation's MonoMethodILState, for the inline per-bb IL-offset store */
	int ilofs_ti = -1;                 /* functype index for (i32)->i32, i.e. enter_island returning the il_state */
	int finally_ind_idx = 0;           /* i32 local: in-method finally indicator (continuation bb idx, or -1 = rethrow) */
	gboolean eh_has_finally = FALSE;   /* TRUE: method has >=1 FINALLY clause (milestone 2c) */
	WasmEhTable *eh_table = NULL;      /* in-method EH clause table (built below, baked into the catch landing pad) */
	gboolean eh_on = FALSE;            /* TRUE: emit the in-method try/catch wrapper for this method */
	int eh_dispatch_ti = -1, eh_endcatch_ti = -1;  /* functype indices: (i32,i32)->i32 dispatch + ()->void end_catch */
	int resid_void_ti = -1;            /* ()->void: mono_wasm_jit_continue_unwind. Registered UNCONDITIONALLY --
	                                    * the residual/vcall/delegate threw==1 sites occur in non-EH methods too,
	                                    * where eh_endcatch_ti is never set up. */
	int nrefslots = 0;                 /* number of reference vregs routed to the GC ref shadow stack */
	/* MONO_WASM_JIT_SLOTZERO: dead-slot kill chains, built by the SLOTLIVE walk (classifier block) and
	 * consumed by the emit loop below. Both iterate the identical bb/ins sequence, so a plain running
	 * instruction ordinal links them: sl_kill_head[ord] = first vreg whose slot dies right BEFORE the
	 * ord-th instruction (its last use), sl_kill_next[vreg] = next vreg dying at the same ordinal. */
	int *sl_kill_head = NULL;          /* [nins] ordinal -> first dying vreg, or -1 */
	/* [nvreg] MONO_WASM_JIT_LCSE: vreg holds a reference with NO pin slot (SLOTLIVE elided it). Such a
	 * value must never be cached or reused by LCSE — see WJ_LCSE_UNPINNED at the LOADM macro. NULL when
	 * slot elision is not in play, in which case every isref vreg is pinned and the question is moot. */
	guint8 *lcse_nopin = NULL;
	int *sl_kill_next = NULL;          /* [nvreg] chain */
	int addrbase_idx = 0;              /* i32 local: addressable-locals frame base address (OP_LDADDR) */
	int addr_tmp_idx [4] = { 0, 0, 0, 0 }; /* per-type scratch locals for addr-frame stores (i32/i64/f32/f64) */
	int naddrbytes = 0;                /* total bytes of addressable-locals frame (8 per address-taken local) */
	WjCtx lc;
	int *bbidx;
	int structured_cfg_kind = WJ_CFG_DISPATCH;
	int structured_loop_h = -1, structured_loop_l = -1;
	WasmFuncType extra_types [WJ_EXTRA_TYPES_MAX]; /* callee functypes for call_indirect, after T0/T1 */
	int nextra = 0;
	/* Where this method's extra callee types start WITHIN ITS OWN type block: T0 = the method, T1 = the
	 * entry thunk, T2.. = the extras. So it is the constant 2, always, and every type index the body
	 * carries is block-relative.
	 *
	 * It used to be the block's base in the whole module's type section, adjusted per batch member --
	 * because type indices were ULEB-encoded inline at each call_indirect and could not be relocated
	 * afterwards, so the base had to be right at emit time. That is exactly what made a batch cost a full
	 * re-JIT per member. Type indices are now RELOCATIONS (WASM_RELOC_TYPE_*), resolved to
	 * ti_base + tpool when the member is framed, so the emitter no longer needs to know anything about
	 * which module it will end up in. Kept as a named constant rather than folded into the ~30 interning
	 * sites so that what the 2 means stays written down. */
	const int ti_base = 2;
#ifdef HOST_BROWSER
	/* Batching: this method's block starts after every already-captured member's block. Known now
	 * because members are emitted one at a time, in order. */
	int wj_batch_slot = -1;
	/* A member emit can re-enter this function (cctor / AOT-target init compiled on demand). Such a
	 * nested compile must NOT claim a member slot: it would take the same index as its parent, compute
	 * ti_base from the same running total, and capture first — leaving the parent's already-baked type
	 * indices pointing into the wrong block. Nested compiles take the standalone path instead. */
	if (wj_batch && !wj_batch->in_member && wj_batch->n < WJ_BATCH_MAX) {
		wj_batch_slot = wj_batch->n;
		wj_batch->cur = wj_batch_slot;
		wj_batch->in_member = TRUE;
	}
#endif
	gboolean uses_calls = FALSE;
	gboolean uses_eh_tag = FALSE;   /* in-method EH landing pad -> import the C++ exception tag x.e */
	int eh_type_idx = -1;           /* type index of (i32)->void (the catch handler + the tag) */
	const char *fail = NULL;
	int fail_op = -1;
	/* Populated only for a signature-classification bail and consumed by the VERBOSE=2 log at done.
	 * Metadata is stable for the compile lifetime; no strings/layout work is paid on successful emits. */
	const char *fail_sig_site = NULL;
	MonoMethod *fail_sig_callee = NULL;
	MonoMethodSignature *fail_sig = NULL;
	int fail_sig_arg = -2;
	char *mname = mono_method_get_full_name (cfg->method);
#ifdef HOST_BROWSER
	/* Self-recursive calls reserve a fresh immutable slot pair. Failed reservations are intentionally
	 * orphaned: recycling is incompatible with baked f-slot identities and append-only descriptors. */
	int wj_self_e_slot = 0, wj_self_f_slot = 0;
#endif

	wasm_buf_init (&body);
	wasm_buf_init_relocs (&body);   /* every call site and type index in it is a hole; see wasm-encoder.h */

	/* Eligibility gates (critical for auto-JIT robustness on real code like Minecraft, where the
	 * emitter is fed thousands of method shapes): bail to the interpreter for features the wasm
	 * backend doesn't lower yet — unsupported EH/control-flow shapes and generic-shared methods
	 * (rgctx access the non-llvmonly path doesn't handle). Unknown opcodes still bail individually
	 * via the lowering switch's default case. */
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
		{ guint _ci;   /* in-method EH clauses always propagate via C++/wasm-EH (cppeh is the only model) */
		  /* Supported native wasm-EH clauses: catch (NONE), finally (FINALLY, including Java
		   * try-with-resources), and fault (FAULT). Filters still bail to the interpreter. */
		  for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			  int _f = cfg->header->clauses [_ci].flags;
			  if (_f == MONO_EXCEPTION_CLAUSE_FINALLY) {
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
	/* save_lmf methods (synchronized bodies, some wrappers) lower emit_push_lmf/emit_pop_lmf IR:
	 * the LMF struct lives behind a VARLOADA'd local (a fixed 8-byte addr-frame slot here — smaller
	 * than MonoLMF) and its address is PUBLISHED into the thread's LMF chain; an exceptional unwind
	 * that skips the pop leaves the chain pointing into a popped frame -> the next LMF-chain walk
	 * (GC STW, sampling) reads garbage as metadata. Historically this path never ran (the TLS
	 * getter asserted at compile); now that it compiles, bail it to the interpreter cleanly. */
	if (cfg->method->save_lmf) { fail = "save_lmf (LMF push/pop) not supported"; goto done; }
	/* Marshalling wrappers (managed-to-native icall/pinvoke, native-to-managed, runtime-invoke) carry LMF
	 * save/restore, the native fptr baked as an iconst, handle/byref marshal stores and coop-GC transition
	 * IR — shapes the isref classifier + raw membase-store lowering mishandle, producing a ref store through
	 * a garbage/stale object base that scribbles the heap/metadata (confirmed live:
	 * System.Reflection.MonoMethodInfo:get_method_attributes, a hot IKVM-reflection icall wrapper -> OBJGUARD
	 * kind 2 wild store -> intermittent ArrayIndexOutOfBounds / mono_metadata_token_table assert). These
	 * wrappers are ~a native call, so the JIT upside is near zero; bail them to the interpreter until their
	 * codegen is verified. The synchronized wrapper (MONO_WRAPPER_SYNCHRONIZED) and its inner
	 * (MONO_WRAPPER_OTHER) are a separate, supported path and are NOT bailed here.
	 * MONO_WASM_JIT_MARSHAL_WRAPPERS=1 reverts (buggy) for A/B. */
	{ extern int mono_wasm_jit_marshal_wrappers;
	  if (!mono_wasm_jit_marshal_wrappers) {
		  switch (cfg->method->wrapper_type) {
		  case MONO_WRAPPER_MANAGED_TO_NATIVE:
		  case MONO_WRAPPER_NATIVE_TO_MANAGED:
		  case MONO_WRAPPER_RUNTIME_INVOKE:
			  fail = "marshalling wrapper (unsupported IR shape)"; goto done;
		  default:
			  break;
		  }
	  } }
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
	 * The rgctx lift: instead of bailing the whole method, each rgctx call
	 * is routed per-site to the interp residual (mono_wasm_jit_call_interp), which derives the generic
	 * context from the concrete inflated call->method and is rgctx-correct (interp_entry, and the AOT
	 * fastpath's do_jit_call-via-gsharedvt_out wrapper, both handle it). The INLINE_AOT direct-body path is
	 * skipped for rgctx calls because it can only bake the callee's extra arg/rgctx, not the separate CALLSITE
	 * runtime generic context carried in MONO_ARCH_RGCTX_REG;
	 * indirect/virtual rgctx calls still bail. This unblocks IKVM EH methods whose catch block calls the
	 * generic ExceptionHelper.MapException<T> (the #1 render-path blocker after the EH gate). */

	for (i = 0; i < nvreg; ++i) {
		li [i] = -1;
		refslot [i] = -1;
		addrslot [i] = -1;
	}

	/* argument valtypes -> wasm params, from the one shared ABI descriptor. Using the same
	 * mono_wasm_get_call_info for the self-sig and for call sites is what keeps the registered
	 * f_sig_id and callers' baked dep_sig structurally identical (they hash the same ftype).
	 * A scalar-vtype arrives as its single field's wasm scalar, while MINI's body addresses the
	 * argument as a struct. Reconstruct it at entry: ref-bearing scalar structs use a GC-scanned
	 * ref-shadow slot (-2); ref-free structs use an address-frame slot populated by the prologue. */
	mono_wasm_get_call_info (sig, &self_ci);
	if (!self_ci.valid) {
		fail = self_ci.fail_reason;
		fail_sig_site = "self"; fail_sig_callee = cfg->method; fail_sig = sig; fail_sig_arg = self_ci.fail_arg;
		goto done;
	}
	for (i = 0; i < nargs; ++i) {
		if (self_ci.args [i].kind == WJ_ARG_VTYPE_BYADDR) {
			/* by-addr vtype arg: the wasm param IS the address of the caller-owned copy. All body
			 * access is decomposed OP_LDADDR (+ MEMBASE loads/stores / a wbarrier-copy icall) on the
			 * arg var — the pre-pass marks it with the -3 sentinel and OP_LDADDR just re-pushes the
			 * param. Mutation through it writes the caller's per-call copy: correct byval semantics
			 * (LLVMArgVtypeByRef precedent). The pointer itself is non-heap (caller C-stack frame /
			 * interp args area), so the vreg is never seeded ref and stays a fast wasm param. */
			param_types [i] = WASM_I32;
			vt [cfg->args [i]->dreg] = WASM_I32;
			li [cfg->args [i]->dreg] = i;
			addrslot [cfg->args [i]->dreg] = -3;
			self_has_byaddr = TRUE;
			continue;
		}
		if (self_ci.args [i].kind == WJ_ARG_VTYPE_SCALAR) {
			int av = cfg->args [i]->dreg;
			MonoClass *ak = mono_class_from_mono_type_internal (self_ci.args [i].type);
			if (av < 0 || av >= nvreg || !ak) { fail = "scalar-vtype arg var"; goto done; }
			param_types [i] = self_ci.args [i].wtype;
			vt [av] = self_ci.args [i].wtype;
			/* The prologue reads wasm param i directly. Body accesses use reconstructed storage. */
			li [av] = -1;
			if (m_class_has_references (ak) || m_class_has_ref_fields (ak)) {
				if (self_ci.args [i].wtype != WASM_I32) { fail = "scalar-vtype ref width"; goto done; }
				addrslot [av] = -2;
			} else {
				naddrbytes = (naddrbytes + 7) & ~7;
				addrslot [av] = naddrbytes;
				naddrbytes += 8;
			}
			continue;
		}
		if (self_ci.args [i].kind != WJ_ARG_SCALAR) { fail = "arg type"; goto done; }
		param_types [i] = self_ci.args [i].wtype;
		vt [cfg->args [i]->dreg] = self_ci.args [i].wtype;
		li [cfg->args [i]->dreg] = i;
	}
	nwparams = nargs;
	if (self_ci.vret_byaddr) {
		/* hidden vret: a TRAILING i32 param holding the caller's return-buffer address, bound to
		 * cfg->vret_addr (created by mono_arch_create_vars when ret storage is ArgValuetypeAddrInIReg;
		 * method_to_ir already lowered every `ret` into a store through it — after decompose that is
		 * MEMBASE stores / a mono_gc_wbarrier_range_copy icall taking this pointer). The wasm return
		 * type is void. Like by-addr args the pointer is non-heap and stays a fast local; the prologue
		 * ref-arg copy loop only walks cfg->args, so it never touches this param. */
		int vrd = cfg->vret_addr ? cfg->vret_addr->dreg : -1;
		if (vrd < 0 || vrd >= nvreg) { fail = "vret no var"; goto done; }
		param_types [nargs] = WASM_I32;
		vt [vrd] = WASM_I32;
		li [vrd] = nargs;
		nwparams = nargs + 1;
	}
	ret_vt = self_ci.ret.wtype;   /* WASM_VOID for void and hidden-vret returns, else the scalar ret valtype */

	/*
	 * Infer vreg valtypes from defining opcodes.
	 *
	 * MOVES ARE DELIBERATELY EXCLUDED and handled by a unification pass further down (search
	 * "unify move valtypes"). A move is the one shape whose operand type comes from its operands
	 * rather than from itself, and mono does not always spell it correctly -- leaving SSA turns a
	 * phi into a move whose opcode comes from the phi's, and mono_ssa_compute has no STACK_R4 case,
	 * so an f32 phi can reach codegen as an integer OP_MOVE. Taking the opcode at its word there
	 * declares an f32 vreg to be i32, and the result is a module that only fails at INSTANTIATE
	 * (WJC_INVALID -- and under module batching that loses every sibling method too), which is the
	 * one failure mode this emitter must never have. Deriving the type from the operands instead
	 * makes the answer independent of how the move happens to be spelled.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MONO_BB_FOR_EACH_INS (bb, ins) {
			WasmValtype rt;
			if (wj_ins_is_move (ins->opcode))
				continue;
			rt = wasm_valtype_of_opcode (ins->opcode);
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
	 * OP_LDADDR yields addrbase+offset to hand to the callee. Supported: non-ref scalar locals (8-byte
	 * slot); value-type locals under MONO_WASM_JIT_LDADDR_VTYPE (full-size slot), including REF-BEARING
	 * ones under MONO_WASM_JIT_LDADDR_VTYPE_REF — the addr slots live in the C-stack frame which sgen
	 * scans conservatively (see the frame doc above), so embedded refs pin their referents like any AOT
	 * struct in a C local. Still bailed: ref/byref SCALAR locals (a lone ref slot wants the ref-shadow
	 * region, only the scalar-ref vtype sentinel -2 does that today) and address-taken ARGUMENTS (still
	 * in a wasm param). Anything unsupported bails the whole method (the prior behaviour for any
	 * OP_LDADDR). There is no knob for the pass itself -- it always runs; MONO_WASM_JIT_LDADDR_VTYPE and
	 * MONO_WASM_JIT_LDADDR_VTYPE_REF gate only which SHAPES it accepts.
	 * NB: NOT HOST_BROWSER-gated. This is a pure compile-time classification pass (it assigns addrslot
	 * offsets / decides which ldaddr shapes are supported); the addr-frame runtime helpers it feeds
	 * (the C-stack frame prologue, baked by the emit below) is HOST_BROWSER-gated separately. Un-gating it
	 * lets the offline cross-compiler dump reach the REAL ldaddr gate (e.g. "ldaddr of vtype with refs")
	 * instead of the spurious "ldaddr unsupported var" the emit hits when no slot was ever assigned. */
	{
	{
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
				if (addrslot [vv] == -3)
					continue;   /* by-addr vtype ARG: the wasm param already holds the address; OP_LDADDR re-pushes it */
				if (li [vv] >= 0) { fail = "ldaddr of arg local"; fail_op = OP_LDADDR; goto done; }   /* scalar arg: in a wasm param, no address */
				if (m_type_is_byref (var->inst_vtype) || mini_type_is_reference (var->inst_vtype)) { fail = "ldaddr of ref/byref local"; fail_op = OP_LDADDR; goto done; }
				lvt = wasm_valtype_of_type (var->inst_vtype);
				if (lvt == 0 || lvt == WASM_VOID || wasm_valtype_group (lvt) < 0) {
					/* Non-scalar (valuetype) address-taken local. DEFAULT: bail. MONO_WASM_JIT_LDADDR_VTYPE=1
					 * backs it with a full-size addr-frame slot (field access via OP_LDADDR + MEMBASE);
					 * vt[vv] stays 0 so a scalar/bulk value access bails. */
					extern int mono_wasm_jit_ldaddr_vtype;
					MonoClass *vk;
					int vsize;
					if (!mono_wasm_jit_ldaddr_vtype) { fail = "ldaddr of non-scalar local"; fail_op = OP_LDADDR; goto done; }
					vk = mono_class_from_mono_type_internal (var->inst_vtype);
					if (!vk) { fail = "ldaddr vtype no class"; fail_op = OP_LDADDR; goto done; }
					vsize = mono_class_value_size (vk, NULL);
					if (m_class_has_references (vk) || m_class_has_ref_fields (vk)) {
						/* Ref-bearing vtype. A SCALAR one (single managed-ref field, e.g.
						 * RuntimeTypeHandle{RuntimeType}) keeps the ref-shadow slot (sentinel addrslot=-2;
						 * refslot assigned after the isref pass): OP_LDADDR yields refbase+slot*4, the field
						 * store/load track it as a pinning root, and the store card-barrier marks a harmless
						 * card (wasm32: no overlapping cards). A MULTI-FIELD one gets a normal full-size
						 * addr-frame slot under MONO_WASM_JIT_LDADDR_VTYPE_REF: the addr frame lives in the
						 * C-stack frame, which sgen scans CONSERVATIVELY (frame doc above wasm_ld) — embedded
						 * refs over-pin their referents, the exact guarantee AOT'd structs in C locals rely
						 * on. The prologue zero-fill keeps the GC from ever scanning garbage in the slot.
						 * (The old unconditional "GC-unsafe frame" bail predated the addr frame's move onto
						 * the C stack, when it really was an unscanned g_malloc arena.) */
						extern int mono_wasm_jit_vtype_scalar_ref;
						extern int mono_wasm_jit_ldaddr_vtype_ref;
						MonoType *setype = NULL;
						if (mono_wasm_jit_vtype_scalar_ref && vsize <= 8 && mini_wasm_is_scalar_vtype (var->inst_vtype, &setype) && setype && mini_type_is_reference (setype)) {
							addrslot [vv] = -2;
							continue;
						}
						if (!mono_wasm_jit_ldaddr_vtype_ref) { fail = "ldaddr of vtype with refs"; fail_op = OP_LDADDR; goto done; }
						/* else: fall through to the full-size slot assignment below */
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

	/* By-addr call-arg copy regions: every call site passing a vtype BY ADDRESS gets its own
	 * ALIGN8(vsize) region in the addr frame per such arg. The arg emit (wj_emit_one_call_arg)
	 * memory.copies the value in and passes the region's address, so a callee that mutates its byval
	 * copy can never touch the caller's live value, and a call in a loop re-copies each iteration by
	 * construction (the copy is emitted inline immediately before every call — an invariant future
	 * opt passes must not hoist). Residual calls use the same copy (uniform path; the interp copies
	 * it once more onto its GC-scanned stack before the callee runs). Regions live INSIDE the
	 * [refbase, entry_sp) frame: conservatively scanned (embedded refs pin their referents) and
	 * restored-past on every exit/landing pad — never below SP. Offsets land in WjCallArgs::copyoff
	 * (see mono_wasm_emit_call). */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MONO_BB_FOR_EACH_INS (bb, ins) {
			MonoCallInst *call;
			MonoMethodSignature *csig;
			WasmCallInfo cci;
			WjCallArgs *w;
			int ai;
			switch (ins->opcode) {
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
			case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
			case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
				break;
			default:
				continue;
			}
			call = (MonoCallInst *) ins;
			csig = call->signature;
			if (!call->method || !csig || !call->call_info)
				continue;   /* icalls take scalars only; a missing capture bails at the emit */
			mono_wasm_get_call_info (csig, &cci);
			if (!cci.valid)
				continue;   /* the emit bails with cci.fail_reason */
			w = (WjCallArgs *) call->call_info;
			for (ai = 0; ai < cci.nargs && ai < w->nargs; ++ai) {
				if (cci.args [ai].kind != WJ_ARG_VTYPE_BYADDR)
					continue;
				naddrbytes = (naddrbytes + 7) & ~7;
				w->copyoff [ai] = naddrbytes;
				naddrbytes += (cci.args [ai].vsize + 7) & ~7;
			}
		}
	}

	/*
	 * The return vreg's valtype is the wasm function type's, full stop. The epilogue loads
	 * cfg->ret->dreg and the module declares the function returns ret_vt, so anything else is
	 * unrepresentable. Seeding it here rather than inferring it matters because mono_arch_emit_setret
	 * writes cfg->ret->dreg with a MOVE, and moves no longer declare a type (see below) -- this is the
	 * authoritative source that replaces the one the move used to provide. Skipped for a hidden-vret
	 * method, whose wasm return is void and whose cfg->ret is a value type in the addr frame.
	 */
	if (ret_vt != WASM_VOID && cfg->ret && cfg->ret->dreg >= 0 && cfg->ret->dreg < nvreg &&
	    li [cfg->ret->dreg] < 0 && addrslot [cfg->ret->dreg] < 0) {
		if (vt [cfg->ret->dreg] && vt [cfg->ret->dreg] != ret_vt) { fail = "ret valtype conflict"; goto done; }
		vt [cfg->ret->dreg] = ret_vt;
	}

	/*
	 * Unify move valtypes.
	 *
	 * Every other valtype seed is final by now (params above, defining opcodes, the OP_LDADDR
	 * pre-pass), so this is the point at which a move can be resolved from its operands. A move
	 * asserts only that its two vregs hold the SAME wasm type, so propagate a known type across it
	 * in whichever direction is known, to a fixpoint (a chain of moves needs more than one sweep;
	 * the loop terminates because every iteration that continues has typed at least one more vreg).
	 *
	 * Both sides typed and DIFFERENT is a genuine disagreement, not a spelling problem: emitting it
	 * would produce `local.get <f32>; local.set <i32>`, which validates nowhere. Bail the method.
	 *
	 * A move whose operands are both untyped simply stays untyped; the vreg then gets no wasm local
	 * and the first wasm_ld/wasm_st of it bails cleanly -- which is what would have happened anyway,
	 * since an untyped source cannot be loaded.
	 */
	{
		gboolean changed = TRUE;
		while (changed) {
			changed = FALSE;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int s, d;
					if (!wj_ins_is_move (ins->opcode))
						continue;
					s = ins->sreg1; d = ins->dreg;
					if (s < 0 || s >= nvreg || d < 0 || d >= nvreg)
						continue;
					if (vt [s] && vt [d]) {
						if (vt [s] != vt [d]) { fail = "move valtype conflict"; fail_op = ins->opcode; goto done; }
						continue;
					}
					/* An ARG vreg's type comes from the wasm param signature and is authoritative;
					 * it is already set, so this only ever fills in the other side. */
					if (vt [s] && !vt [d]) { vt [d] = vt [s]; changed = TRUE; }
					else if (vt [d] && !vt [s]) { vt [s] = vt [d]; changed = TRUE; }
				}
			}
		}
	}

	/*
	 * MONO_WASM_JIT_COALESCE — let several vregs share one wasm local when their live ranges are disjoint.
	 *
	 * li[] is otherwise strictly one local per vreg with no reuse at all: AABB:combine declares 58 where
	 * teavm's optimiser needs 5 for the identical Java source.
	 *
	 * Liveness here is a real backward dataflow, NOT first-mention/last-mention. The cheap version is
	 * unsound across a back edge:
	 *
	 *     bb0: t1 = 5
	 *     bb1: use t1 ; t2 = 7 ; use t2 ; br bb1      <- loop header
	 *
	 * t1's mentions all precede t2's, so a mention-range allocator shares their local -- and on the
	 * second iteration `use t1` reads t2's value. Propagating live_in/live_out keeps t1 live across the
	 * whole loop, which is what makes the overlap visible.
	 *
	 * Uses are enumerated exactly as the SLOTLIVE walk does, and for the same reason: a missed use
	 * shortens a range and silently shares a still-live local. That means the generic source registers,
	 * plus a store-membase `dreg` (which is an ADDRESS, not a result), plus the positional call-arg vregs
	 * hanging off call_info. cfg->ret->dreg is additionally pinned live to the end of the method because
	 * the epilogue loads it OUTSIDE the instruction stream, where no walk of the IR can see it.
	 *
	 * Each valtype group holds exactly one wasm type (wasm_valtype_group), so sharing a slot within a
	 * group can never produce a type mismatch.
	 */
	int *lslot = NULL;
	/*
	 * Clause-free methods only. The liveness below walks bb->out_bb, and mono does NOT thread exception
	 * edges through it -- a throw reaches its handler via the EH machinery, not a CFG successor. So a vreg
	 * whose only live path into a handler is that implicit edge looks dead here, and coalescing it would
	 * miscompile the handler. jbox2d has no EH at all, so the bench cannot expose this; Minecraft is full
	 * of it. Widening this needs the intervals of everything live in a try region extended across the
	 * whole region plus its handlers.
	 */
	if (mono_wasm_jit_coalesce && nvreg > 0 && cfg->header->num_clauses == 0) {
		int words = (nvreg + 31) / 32;
		int nbb2 = 0, ordn = 0, bbn;
		MonoBasicBlock *bbl; MonoInst *insl;

		for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
			nbb2++;
		/* Bound the dataflow: 4 bitsets of nbb2*words words. Past this a method keeps the old
		 * one-local-per-vreg assignment rather than paying unbounded compile time/memory. */
		if (nbb2 > 0 && (gsize) nbb2 * words <= 65536) {
			int *bb_lo = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nbb2);
			int *bb_hi = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nbb2);
			int *vlo = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
			int *vhi = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
			guint32 *bs_use = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_def = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_in  = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_out = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			int iter, changed = 1;

#define WJ_BS(BASE, BB) ((BASE) + (gsize) (BB) * words)
#define WJ_BS_GET(BASE, BB, V) ((WJ_BS (BASE, BB) [(V) >> 5] >> ((V) & 31)) & 1u)
#define WJ_BS_SET(BASE, BB, V) do { WJ_BS (BASE, BB) [(V) >> 5] |= 1u << ((V) & 31); } while (0)

			for (i = 0; i < nvreg; ++i) { vlo [i] = -1; vhi [i] = -1; }

			/* per-bb upward-exposed uses + defs, and the ordinal range of each bb */
			bbn = 0;
			for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
				bb_lo [bbn] = ordn;
				MONO_BB_FOR_EACH_INS (bbl, insl) {
					int srcs [MONO_MAX_SRC_REGS];
					int nsrc = mono_inst_get_src_registers (insl, srcs);
					gboolean dreg_is_base = FALSE;
					int u, d;
					switch (insl->opcode) {
					case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
					case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG:
					case OP_STORER8_MEMBASE_REG:
					case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM:
					case OP_STOREI2_MEMBASE_IMM:
						dreg_is_base = TRUE; break;
					default: break;
					}
#define WJ_CO_USE(V) do { int _v = (V); \
	if (_v >= 0 && _v < nvreg) { \
		if (!WJ_BS_GET (bs_def, bbn, _v)) WJ_BS_SET (bs_use, bbn, _v); \
		if (vlo [_v] < 0 || ordn < vlo [_v]) vlo [_v] = ordn; \
		if (ordn > vhi [_v]) vhi [_v] = ordn; \
	} } while (0)
					for (u = 0; u < nsrc; ++u)
						WJ_CO_USE (srcs [u]);
					if (dreg_is_base)
						WJ_CO_USE (insl->dreg);
					if (MONO_IS_CALL (insl)) {
						MonoCallInst *cl = (MonoCallInst *) insl;
						WjCallArgs *cw = (WjCallArgs *) cl->call_info;
						if (cw) {
							for (u = 0; u <= cw->nargs; ++u)
								WJ_CO_USE (wj_arg_vreg (cl, u));
						}
					}
#undef WJ_CO_USE
					d = dreg_is_base ? -1 : insl->dreg;
					if (d >= 0 && d < nvreg) {
						WJ_BS_SET (bs_def, bbn, d);
						if (vlo [d] < 0 || ordn < vlo [d]) vlo [d] = ordn;
						if (ordn > vhi [d]) vhi [d] = ordn;
					}
					ordn++;
				}
				bb_hi [bbn] = ordn - 1;   /* < bb_lo for an empty bb; handled below */
			}

			/* live_out[b] = U live_in[succ];  live_in[b] = use[b] U (live_out[b] - def[b])
			 *
			 * Block index lookups are precomputed. Walking cfg->bb_entry to find a successor's index
			 * inside the fixpoint would make this O(nbb^2) per iteration, which is real money on the
			 * 7000-instruction methods here. */
			MonoBasicBlock **bbarr = (MonoBasicBlock **) mono_mempool_alloc (cfg->mempool, sizeof (MonoBasicBlock *) * nbb2);
			int *bbnum2idx = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
			for (i = 0; i <= (int) cfg->max_block_num; ++i)
				bbnum2idx [i] = -1;
			bbn = 0;
			for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
				bbarr [bbn] = bbl;
				if (bbl->block_num >= 0 && bbl->block_num <= (int) cfg->max_block_num)
					bbnum2idx [bbl->block_num] = bbn;
			}
			for (iter = 0; changed && iter < nbb2 + 4; ++iter) {
				changed = 0;
				/* reverse layout order converges fast for a mostly-forward CFG */
				for (bbn = nbb2 - 1; bbn >= 0; --bbn) {
					MonoBasicBlock *b = bbarr [bbn];
					int k, w;
					for (k = 0; k < b->out_count; ++k) {
						MonoBasicBlock *o = b->out_bb [k];
						int oi = (o && o->block_num >= 0 && o->block_num <= (int) cfg->max_block_num)
							? bbnum2idx [o->block_num] : -1;
						if (oi < 0) continue;
						for (w = 0; w < words; ++w) {
							guint32 nv = WJ_BS (bs_out, bbn) [w] | WJ_BS (bs_in, oi) [w];
							if (nv != WJ_BS (bs_out, bbn) [w]) { WJ_BS (bs_out, bbn) [w] = nv; changed = 1; }
						}
					}
					for (w = 0; w < words; ++w) {
						guint32 nv = WJ_BS (bs_use, bbn) [w] | (WJ_BS (bs_out, bbn) [w] & ~WJ_BS (bs_def, bbn) [w]);
						if (nv != WJ_BS (bs_in, bbn) [w]) { WJ_BS (bs_in, bbn) [w] = nv; changed = 1; }
					}
				}
			}

			/* A vreg live at a block boundary is live across that whole block, back edges included. */
			for (bbn = 0; bbn < nbb2; ++bbn) {
				int hi = bb_hi [bbn] < bb_lo [bbn] ? bb_lo [bbn] : bb_hi [bbn];
				for (i = 0; i < nvreg; ++i) {
					if (!WJ_BS_GET (bs_in, bbn, i) && !WJ_BS_GET (bs_out, bbn, i))
						continue;
					if (vlo [i] < 0 || bb_lo [bbn] < vlo [i]) vlo [i] = bb_lo [bbn];
					if (hi > vhi [i]) vhi [i] = hi;
				}
			}

			/* The epilogue reads cfg->ret->dreg after the last instruction; no IR walk can see that. */
			if (cfg->ret && cfg->ret->dreg >= 0 && cfg->ret->dreg < nvreg) {
				int rv = cfg->ret->dreg;
				if (vlo [rv] < 0) vlo [rv] = 0;
				vhi [rv] = ordn;
			}
			/* Never-mentioned vregs collapse onto [0,0]: nothing can read them, since every emitter read
			 * goes through a source reg, a call_info slot, or cfg->ret, all enumerated above. */
			for (i = 0; i < nvreg; ++i)
				if (vlo [i] < 0) { vlo [i] = 0; vhi [i] = 0; }

			/* Greedy allocation in increasing start order (counting sort on vlo keeps it deterministic).
			 * A slot is reusable once its current occupant's range has ENDED strictly before this one
			 * starts. */
			{
				int nord = ordn + 2;
				int *cntb = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nord);
				int *order = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *slot_end [4] = { NULL, NULL, NULL, NULL };
				int nslot [4] = { 0, 0, 0, 0 };
				int pos = 0, gi;

				lslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				for (i = 0; i < nvreg; ++i) lslot [i] = -1;
				for (gi = 0; gi < 4; ++gi)
					slot_end [gi] = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);

				for (i = 0; i < nvreg; ++i) cntb [vlo [i] < nord ? vlo [i] : nord - 1]++;
				for (i = 1; i < nord; ++i) cntb [i] += cntb [i - 1];
				for (i = nvreg - 1; i >= 0; --i) {
					int key = vlo [i] < nord ? vlo [i] : nord - 1;
					order [--cntb [key]] = i;
				}

				for (pos = 0; pos < nvreg; ++pos) {
					int v = order [pos], g, s, chosen = -1;
					if (li [v] >= 0 || vt [v] == 0 || addrslot [v] >= 0)
						continue;
					g = wasm_valtype_group (vt [v]);
					if (g < 0) continue;
					/* Scanning is capped: past 256 slots the search cost stops being worth the reuse. */
					for (s = 0; s < nslot [g] && s < 256; ++s)
						if (slot_end [g] [s] < vlo [v]) { chosen = s; break; }
					if (chosen < 0) chosen = nslot [g]++;
					slot_end [g] [chosen] = vhi [v];
					lslot [v] = chosen;
				}
			}
#undef WJ_BS
#undef WJ_BS_GET
#undef WJ_BS_SET
		}
	}

	/* assign locals for non-arg typed vregs, grouped by type (skip address-taken locals: they live in the
	 * addr frame, not a wasm local) */
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0 || addrslot [i] >= 0)
			continue;
		g = wasm_valtype_group (vt [i]);
		if (g < 0) continue;
		/* Coalesced: the group needs as many locals as its highest slot index, not one per vreg. Both
		 * this counting pass and the assignment pass below key off the same lslot[], so they cannot
		 * disagree about the group size. */
		if (lslot && lslot [i] >= 0) {
			if (lslot [i] + 1 > cnt [g]) cnt [g] = lslot [i] + 1;
		} else {
			cnt [g]++;
		}
	}
	/* reserve one extra i32 local at the end of the i32 group for the dispatch index ($blk) */
	dispatch_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* reserve one more i32 local for the interp-residual scratch-buffer pointer (used by the
	 * "callee not jitted" path below to call_indirect mono_wasm_jit_call_interp; dead in methods
	 * with no such call, which is a harmless unused declared local) */
	scratch_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* reserve two more i32 locals: the GC ref-slot frame base, and a scratch for ref stores */
	refbase_idx = nwparams + cnt [0];
	cnt [0] += 1;
	rtmp_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one more i32 local: the C-stack SP captured at entry (every exit stackRestores to it) */
	spentry_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one more i32 local for the inline virtual-IC fast path's resolved f-slot (dead in methods with no
	 * virtual call — a harmless unused declared local) */
	vc_fslot_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the VCALL_AOT dispatch kind (0/1/2 from mono_wasm_jit_vcall_aot_target); dead in
	 * methods with no virtual call — a harmless unused declared local */
	vc_aotkind_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* i32 local: this->vtable, live across the AOT-vcall IC key+vtab_check compares (VCALL_AOT_IC) */
	aic_vtab_idx = nwparams + cnt [0];
	cnt [0] += 1;
	aic_ti_idx = nwparams + cnt [0];
	cnt [0] += 1;
	aic_rgctx_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* i32 local: base address of the delegate-IC entry for the way being checked.
	 *
	 * Purely a WIRE-BYTE optimisation, and wire bytes are load-bearing: V8 rejects a callee for inlining
	 * outright when its body exceeds --wasm-inlining-max-size (500 in stock Chrome, see
	 * v8/src/wasm/inlining-tree.h SmallEnoughToInline). Each way reads seven fields off the same entry, and
	 * re-materialising the address per field costs 6 bytes a time (42 B); setting it once and reading a local
	 * costs 8 + 7*2 = 22 B, so -20 B per way and -80 B on a site still at four ways. The largest MHC cohort
	 * (2020 one-arg stubs) sits ~64 B over the cap, so bytes at this scale decide inlinability. */
	dic_base_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* two i32 locals caching &wj_slot_live / &wj_slot_live_cap for the inline f-slot-IC liveness check
	 * (dead in methods with no vcall — harmless unused declared locals) */
	slotlive_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	slotlive_cap_idx = nwparams + cnt [0];
	cnt [0] += 1;
	vpic_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* the same pair for the worker-local delegate recipe PIC (dead unless the method has a
	 * Delegate.Invoke site, which is the common case — an unused declared local costs nothing) */
	dpic_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* the hoisted capacity VALUES (see the note at their declaration) */
	vpic_capval_idx = nwparams + cnt [0];
	cnt [0] += 1;
	dpic_capval_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* two i32 locals for the in-method EH catch landing pad (dead in methods with no clauses) */
	eh_exc_idx = nwparams + cnt [0];
	cnt [0] += 1;
	eh_h_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* Only when the method actually has EH: a non-EH method never calls enter_island, so reserving this would
	 * cost every JITted method a local for nothing. */
	{
		extern int mono_wasm_jit_inline_ilofs;
		if (eh_on && mono_wasm_jit_inline_ilofs) { ilstate_idx = nwparams + cnt [0]; cnt [0] += 1; }
	}
	/* one i32 local for the in-method finally indicator (dead in non-finally methods): OP_CALL_HANDLER sets
	 * it to the continuation bb index; the catch landing pad sets it to -1; OP_ENDFINALLY branches on it. */
	finally_ind_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the addressable-locals frame base (OP_LDADDR; dead in methods with no address-taken
	 * local), plus the i32 scratch for addr-frame stores (only when there ARE such locals). */
	addrbase_idx = nwparams + cnt [0];
	cnt [0] += 1;
	if (naddrbytes > 0) { addr_tmp_idx [0] = nwparams + cnt [0]; cnt [0] += 1; }   /* i32 store scratch */
	base [0] = nwparams;
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
		li [i] = base [g] + ((lslot && lslot [i] >= 0) ? lslot [i] : run [g]++);
	}
	groups [0].type = WASM_I32; groups [0].count = cnt [0];
	groups [1].type = WASM_I64; groups [1].count = cnt [1];
	groups [2].type = WASM_F32; groups [2].count = cnt [2];
	groups [3].type = WASM_F64; groups [3].count = cnt [3];

	lc.li = li; lc.nvreg = nvreg; lc.refslot = NULL; lc.ref_wt = NULL; lc.refbase = refbase_idx; lc.rtmp = rtmp_idx;
	/* In lazy methods spentry is zero until materialization, then becomes the nonzero entry SP.
	 * Reuse it as the active marker instead of reserving one extra i32 local in every JIT method. */
	lc.lazy_frame = 0; lc.frame_active = spentry_idx;
	lc.vt = vt; lc.addrbase = addrbase_idx;
	{ /* addrslot must be visible to the OP_LDADDR emit when there are ONLY sentinel entries (refvt -2 /
	   * by-addr arg -3, no addr-frame bytes) — otherwise naddrbytes==0 would NULL it and those ldaddrs
	   * misfire as "unsupported var". */
		gboolean _have_sentinel = FALSE; int _i; for (_i = 0; _i < nvreg; ++_i) if (addrslot [_i] == -2 || addrslot [_i] == -3) { _have_sentinel = TRUE; break; }
		lc.addrslot = (naddrbytes > 0 || _have_sentinel) ? addrslot : NULL; }
	lc.addr_ldkind = (naddrbytes > 0) ? addr_ldkind : NULL;
	lc.addr_tmp [0] = addr_tmp_idx [0]; lc.addr_tmp [1] = addr_tmp_idx [1];
	lc.addr_tmp [2] = addr_tmp_idx [2]; lc.addr_tmp [3] = addr_tmp_idx [3];
	/* MONO_WASM_JIT_STOREGUARD (debug): pre-register the (i32,i32)->void functype for the per-store
	 * mono_wasm_jit_check_store call_indirect so wasm_st/wasm_addr_st can reference it by index. Done here
	 * (before body emission, nextra==0) so check_ti is stable; force uses_calls so the table is imported. */
	lc.storeguard = 0; lc.objguard = 0; lc.check_ti = -1;
	/* Helper imports are no longer interned during emission at all. A call to a fixed table index leaves a
	 * relocation naming the address and the signature; slots are assigned by the assembler, in first-use
	 * order, once the whole membership is known. So there is nothing to seed here, nothing to copy back
	 * afterwards, and no way for a member's `call <slot>` to name an import its module does not declare.
	 *
	 * That last failure mode is why batching once forced helper imports OFF for every batched member --
	 * which silently reverted every helper call, and after AOT_IMPORTS every inline-AOT callee call, to
	 * constant-index call_indirect, and made every batching measurement taken before Round 105
	 * "co-location MINUS all function imports". It cannot recur: there is no longer a count for a member
	 * to be out of step with. */
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_storeguard, mono_wasm_jit_objguard;
		if ((mono_wasm_jit_storeguard || mono_wasm_jit_objguard) && nextra < WJ_EXTRA_TYPES_MAX) {
			WasmFuncType cst; memset (&cst, 0, sizeof (cst));
			cst.params [0] = WASM_I32; cst.params [1] = WASM_I32; cst.nparams = 2; cst.ret = WASM_VOID;
			extra_types [nextra] = cst; lc.check_ti = ti_base + nextra++;
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
	 * whatever the value points at) while a MISSED ref is silent corruption — so we err toward marking. The
	 * proven-scalar arithmetic/compare/const/typed-load population stays in fast wasm locals; only
	 * genuinely-maybe-ref vregs go to memory.
	 *
	 * Implemented as a greatest fixed point on nonref[] (a vreg is provably-non-ref only if ALL definitions
	 * are scalar): start optimistic (all i32 candidates nonref), then knock a candidate down to ref the moment
	 * any defining instruction can produce a pointer; isref = the i32 vregs we could not prove scalar. */
	{
		gboolean *isref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		gboolean *nonref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		guint8 *sl_elide = NULL;   /* MONO_WASM_JIT_SLOTLIVE: isref vregs whose slot is elided (see the pass below); isref[] itself stays intact for the diagnostics (MISSEDREF, REFDIAG) */
		gboolean changed = TRUE;
		int pass;
		/* candidates: pointer-sized vregs (i64/f32/f64 can never hold a ref -> never shadow-stacked) */
		for (i = 0; i < nvreg; ++i)
			nonref [i] = (vt [i] == WASM_I32);
		/* vregs mono already knows are object refs OR managed pointers are definitely refs.
		 * With cfg->compute_gc_maps set for COMPILE_WASM, MINI's own
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
			/* PRODUCTION FIX: the prove-non-ref fixpoint trusts mono's
			 * ins->type to spot refs, but that misses heap pointers in shapes it can't see (a movable object
			 * baked as a non-STACK_OBJ iconst, an interior-pointer add mis-typed as scalar, ...), leaving a live
			 * ref in a plain wasm local that dangles after a compacting GC — the confirmed intermittent
			 * corruption source. Close it cheaply: any vreg used as a DEREFERENCE — a MEMBASE load/store
			 * base or a virtual-call receiver — is a live pointer at that instruction, so pin it on the
			 * (conservative) ref shadow stack. Over-marking an unmanaged/addr-frame base is harmless (it pins
			 * nothing); a missed heap base is silent corruption. Only dereferenced pointers move to the shadow
			 * stack — not every i32 temporary. */
			{
			  {
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
		/* GC-POINT LIVENESS SLOT ELISION (MONO_WASM_JIT_SLOTLIVE): a frame slot exists so the
		 * conservative stack scan can see (and pin) the ref — but the scan only ever runs while this
		 * thread sits at a GC point (a call that can allocate, the OP_GC_SAFE_POINT STW poll, a raise).
		 * An isref vreg whose every def->use interval crosses NO GC point is therefore invisible to
		 * the collector for its whole life and can stay in a plain (unscanned) wasm local: no slot,
		 * no prologue copy, no pin. Rules, all conservative toward keeping the slot:
		 *   - seen (def or use) in more than one bb -> slot. Covers loops and EH: a mid-bb raise
		 *     enters a HANDLER bb, so a vreg live into any handler is multi-bb by definition and a
		 *     single-bb vreg can never be observed there. No dataflow needed.
		 *   - used AT a GC-point instruction -> slot. A call arg must stay pinned by the CALLER for
		 *     the callee's whole execution: the residual path stages args in an unscanned scratch
		 *     buffer, so the caller's slot can be the only thing keeping the arg alive.
		 *   - used at a later "gc generation" than its def (a GC-point ins executed in between) -> slot.
		 * Call args are read positionally from WjCallArgs (call->call_info), NOT sregs — they
		 * must be enumerated or an arg-only vreg would look dead and lose its slot. Def-position gen
		 * is recorded AFTER the gc bump so a call RESULT consumed before the next GC point can elide.
		 * The dreg of a MEMBASE store is its base — a USE, not a def (mirroring the REFBASES pass).
		 * Disabled under STOREGUARD/OBJGUARD so refslot stays a complete ref proxy for the guards. */
		{
			extern int mono_wasm_jit_slotlive;
			gboolean slotlive_on = mono_wasm_jit_slotlive != 0;
#ifdef HOST_BROWSER
			{
				extern int mono_wasm_jit_storeguard, mono_wasm_jit_objguard, mono_wasm_jit_guard_keep_slotlive;
				/* MONO_WASM_JIT_GUARD_KEEP_SLOTLIVE=1 keeps elision ON under the guards.
				 *
				 * The unconditional override below makes the guards unable to validate the one feature most
				 * likely to need it: enabling either guard silently turns SLOTLIVE off, so a corruption that
				 * only reproduces with elision on cannot be caught by the very tool built to catch it. That
				 * gap is already on record (a.txt: "OBJGUARD/STOREGUARD cannot validate SLOTLIVE because
				 * they disable it"), and it cost this investigation a full round.
				 *
				 * The override protects COVERAGE, not correctness. refslot is used as a complete "is a ref"
				 * proxy by exactly one check -- OBJGUARD kind 2 (is the stored VALUE a ref) -- and elision
				 * makes that proxy partial. Every other check is an address-range test that does not consult
				 * ref-ness at all: STOREGUARD kinds 0/1 run inside the slot store path, and OBJGUARD kinds
				 * 3/4/5 (byref base, generic membase, vcall receiver) bounds-check a computed address. Those
				 * stay sound with elision on; they simply see fewer stores. Partial coverage of the real
				 * configuration beats complete coverage of a configuration that does not reproduce. */
				if ((mono_wasm_jit_storeguard || mono_wasm_jit_objguard) && !mono_wasm_jit_guard_keep_slotlive)
					slotlive_on = FALSE;
			}
#endif
			if (slotlive_on) {
				guint8 *needs_slot = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				guint8 *sl_multi = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);      /* events span >1 bb */
				guint8 *sl_def_first = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);  /* first event was a DEF (no loop-carried use-before-def) */
				int *ev_bb = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* bb of first event, -1 = unseen */
				int *def_gen = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nvreg);
				int *last_use_ord = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *last_def_ord = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *bb_last_gcp, *sl_bbidx; int nbbs = 0, nins = 0;
				MonoBasicBlock *bbl; MonoInst *insl;
				MonoInst *last_gcp_ins = NULL;
				int bbn, nelide = 0, ord = 0, last_gcp_ord = -1;
				gboolean sl_has_backedge = FALSE, prior_gcps_are_polls = TRUE;
				/* Raises the exemption hid anywhere in this method. The terminal-vcall handoff below is
				 * only sound while this is zero -- see there. */
				int method_skipped_raises = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
					nbbs++;
				bb_last_gcp = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (nbbs ? nbbs : 1));
				sl_bbidx = (int *) mono_mempool_alloc (cfg->mempool,
					sizeof (int) * ((int) cfg->max_block_num + 1));
				for (i = 0; i < nbbs; ++i)
					bb_last_gcp [i] = -1;
				for (i = 0; i <= (int) cfg->max_block_num; ++i)
					sl_bbidx [i] = -1;
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
					sl_bbidx [bbl->block_num] = bbn++;
				/* A layout-backward CFG edge can execute a textual "last use" again after the call.
				 * Terminal handoff therefore requires a DAG. This deliberately treats self-edges as
				 * backedges and declines the optimization rather than attempting loop liveness. */
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
					int oi;
					for (oi = 0; oi < bbl->out_count; ++oi) {
						MonoBasicBlock *out = bbl->out_bb [oi];
						int outi = (out && out->block_num >= 0 &&
							out->block_num <= (int) cfg->max_block_num) ?
							sl_bbidx [out->block_num] : -1;
						if (outi >= 0 && outi <= bbn)
							sl_has_backedge = TRUE;
					}
				}
				for (i = 0; i < nvreg; ++i) {
					ev_bb [i] = -1;
					last_use_ord [i] = -1;
					last_def_ord [i] = -1;
				}
				/* args are defined by the prologue: entry bb (walk ordinal 0), generation 0 */
				for (i = 0; i < nargs; ++i) {
					int av = cfg->args [i]->dreg;
					if (av >= 0 && av < nvreg) { ev_bb [av] = 0; def_gen [av] = 0; sl_def_first [av] = 1; }
				}
				/* EH landing pads and their runtime dispatch are deliberately excluded from the
				 * frame-free class. Otherwise, every instruction must be locally pure or a direct
				 * call to an already-published no-GC method. */
				method_nogc = cfg->header->num_clauses == 0;
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
					int gc_gen = 0;
					/* Raises exempted since the current generation began. A forwarded call argument may
					 * only skip the used-at-a-GC-point clause below while this is zero -- see there. */
					int gen_skipped_raises = 0;
					MONO_BB_FOR_EACH_INS (bbl, insl) {
						int srcs [MONO_MAX_SRC_REGS];
						int nsrc = mono_inst_get_src_registers (insl, srcs);
						/* Two views, identical unless RAISE_NOGC==1 (see wj_ins_is_published_gcpoint):
						 * `gcp` drives this method's own def/use generations, `gcp_pub` drives every
						 * claim published to other code. */
						gboolean gcp = wj_ins_is_effective_gcpoint (cfg, insl);
						gboolean gcp_pub = wj_ins_is_published_gcpoint (cfg, insl);
						gboolean pinned_vforward = gcp && wj_ins_is_pinned_vcall_forward (insl);
						gboolean forward_call = gcp &&
							(wj_direct_admitted_fslot (cfg, insl) > 0 || pinned_vforward);
						MonoCallInst *sl_call = MONO_IS_CALL (insl) ? (MonoCallInst *) insl : NULL;
						WjCallArgs *sl_cw = sl_call ? (WjCallArgs *) sl_call->call_info : NULL;
						gboolean dreg_is_base = FALSE;
						int u, d;
						if (gcp_pub) {
							method_nogc = FALSE;
							effective_gcp_count++;
							if (last_gcp_ins && last_gcp_ins->opcode != OP_GC_SAFE_POINT)
								prior_gcps_are_polls = FALSE;
							last_gcp_ins = insl;
							last_gcp_ord = ord;
						}
						switch (insl->opcode) {
						case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
						case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
						case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
							dreg_is_base = TRUE; break;
						default: break;
						}
#define WJ_SL_USE(v,FWD) do { int _u = (v); gboolean _fwd = (FWD); \
	if (_u >= 0 && _u < nvreg && isref [_u]) { \
		if (ev_bb [_u] == -1) { ev_bb [_u] = bbn; needs_slot [_u] = TRUE; } /* use before any def (loop-carried) */ \
		else if (ev_bb [_u] != bbn) { sl_multi [_u] = 1; needs_slot [_u] = TRUE; } \
		else if (def_gen [_u] != gc_gen) needs_slot [_u] = TRUE; \
		/* `_fwd` lets a forwarding call's argument skip this clause because the callee is supposed to
		 * take ownership of the root (wj_ins_is_pinned_vcall_forward). That leaves the argument with NO
		 * slot at all -- which was survivable only by accident: without the raise exemption a raise in
		 * the span bumps gc_gen, the clause above fires, and the argument gets pinned anyway. With the
		 * exemption both clauses go quiet and the handoff guarantee is the only thing left, and the
		 * measured crash (always mini_llvmonly_init_vtable_slot -> resolve_vcall on a filler-class
		 * receiver) says it does not hold on the PIC miss path. So honour `_fwd` only when no raise was
		 * skipped in this generation. */ \
		else if (gcp && (!_fwd || gen_skipped_raises > 0)) needs_slot [_u] = TRUE; \
		last_use_ord [_u] = ord; \
	} } while (0)
						for (u = 0; u < nsrc; ++u) {
							gboolean is_forward_arg = FALSE;
							int au;
							if (forward_call && sl_cw)
								for (au = 0; au <= sl_cw->nargs; ++au)
									if (wj_arg_vreg (sl_call, au) == srcs [u]) {
										is_forward_arg = TRUE;
										break;
									}
							WJ_SL_USE (srcs [u], is_forward_arg);
						}
						if (dreg_is_base)
							WJ_SL_USE (insl->dreg, FALSE);
						/* positional call-arg uses (WjCallArgs): [0..n-1] args, [n] the vret addr */
						if (sl_call) {
							if (sl_cw) {
								for (u = 0; u <= sl_cw->nargs; ++u)
									WJ_SL_USE (wj_arg_vreg (sl_call, u), forward_call);
							}
						}
#undef WJ_SL_USE
						if (gcp) {
							gc_gen++;
							gen_skipped_raises = 0;   /* a real GC point starts a new generation */
							bb_last_gcp [bbn] = ord;
						} else if (wj_ins_is_raise (insl)) {
							gen_skipped_raises++;     /* exempted raise: the accidental pinning is gone */
							method_skipped_raises++;
						}
						/* def AFTER the bump: a call result belongs to the post-call generation */
						d = dreg_is_base ? -1 : insl->dreg;
						if (d >= 0 && d < nvreg && isref [d]) {
							last_def_ord [d] = ord;
							if (ev_bb [d] == -1) { ev_bb [d] = bbn; sl_def_first [d] = 1; def_gen [d] = gc_gen; }
							else if (ev_bb [d] != bbn) { sl_multi [d] = 1; needs_slot [d] = TRUE; }
							else def_gen [d] = gc_gen;
						}
						ord++;
					}
				}
				nins = ord;
				/* With a terminal pinned virtual call and no way to loop back, every reference whose
				 * textual final use is at/before this call is dead in the caller while the callee runs
				 * or owns a root in the callee/miss frame. Earlier effective points are allowed only
				 * when they are conditional polls; those materialize and release the caller frame
				 * entirely inside their taken arms. */
				/* MEASURED: this handoff is the RAISE_NOGC corruption path. Its third arm in sl_elide
				 * below drops a reference's frame slot WITHOUT consulting needs_slot at all, so nothing
				 * in the liveness walk can restrain it -- and its enabling condition,
				 * effective_gcp_count == 1, is exactly what the raise exemption manufactures (a method
				 * with one real call and any number of null checks now looks single-GC-point). The
				 * references it drops are then unpinned for the whole method, and the claim that they
				 * "own a root in the callee/miss frame" is the same promise the PIC miss path breaks:
				 * every failure lands in mini_llvmonly_init_vtable_slot -> resolve_vcall on a receiver
				 * already reclaimed. Disabling wj_ins_is_pinned_vcall_forward outright (SHARED_MISS=0)
				 * is clean 0/4 against a 4/4 control; gating only the `_fwd` skip in WJ_SL_USE changed
				 * nothing (3/4 vs 2/4), which is what pins it to THIS consumer. So: no handoff in a
				 * method where the exemption hid a raise. */
				if (cfg->header->num_clauses == 0 && last_gcp_ins && method_skipped_raises == 0 &&
				    wj_ins_is_pinned_vcall_forward (last_gcp_ins) && !sl_has_backedge) {
					gboolean all_pre_refs_die = TRUE;
					terminal_vcall_ins = last_gcp_ins;
					terminal_vcall_ord = last_gcp_ord;
					terminal_vcall_poll_prefix = prior_gcps_are_polls;
					/* A reference defined by the terminal call itself is not live while that call
					 * executes. Every reference defined earlier must have its final use at/before
					 * the call before the whole frame can be handed off. */
					for (i = 0; i < nvreg; ++i)
						if (li [i] >= 0 && isref [i] && last_def_ord [i] < terminal_vcall_ord &&
						    last_use_ord [i] > terminal_vcall_ord) {
							all_pre_refs_die = FALSE;
							break;
						}
					terminal_vcall_handoff = all_pre_refs_die;
				}
				sl_elide = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				/* Per-ARM elision attribution (MONO_WASM_JIT_ELIDEDIAG=1).
				 *
				 * The three arms below mean different things and have different soundness arguments, but
				 * nelide lumps them together, so a corruption reproducer cannot tell which one dropped the
				 * slot. arm3 (terminal-vcall handoff) is the one already fixed via method_skipped_raises;
				 * arm1 (method_nogc) overrides the liveness walk wholesale, including cross-bb and
				 * loop-carried refs; arm2 is the walk's own verdict. Counting them separately is what
				 * distinguishes "the walk is wrong" from "an arm bypassed the walk" -- the same role
				 * span_gcp=0 played for the handoff arm. Attribution follows the || short-circuit order. */
				int nref_total = 0, nel_nogc = 0, nel_needs = 0, nel_handoff = 0;
				for (i = 0; i < nvreg; ++i)
					if (li [i] >= 0 && isref [i])
						nref_total++;
				for (i = 0; i < nvreg; ++i)
					if (li [i] >= 0 && isref [i] &&
					    (method_nogc || !needs_slot [i] ||
					     (effective_gcp_count == 1 && terminal_vcall_handoff &&
					      last_use_ord [i] <= terminal_vcall_ord))) {
						if (method_nogc) nel_nogc++;
						else if (!needs_slot [i]) nel_needs++;
						else nel_handoff++;
						/* A reference used at an admitted direct GC-capable call only may hand its
						 * root to the callee when that call is its final use. There is no safepoint
						 * between the caller's local.get and the callee's eager/lazy root setup.
						 * Any later use is in a new gc_gen and set needs_slot above. For a whole
						 * transitively no-GC method, even cross-bb/loop-carried refs are invisible
						 * to the collector, so the method effect safely overrides sl_multi too. */
						sl_elide [i] = 1;
						nelide++;
					}
				if (nelide > 0 && mono_wasm_jit_stats)
					mono_wasm_jit_add (WJC_SLOTS_ELIDED, nelide);
				/* Ground truth for "which arm dropped the slot", printed per method so an inline-on vs
				 * inline-off pair can be diffed directly. clauses/gcp/skipped are the enabling conditions:
				 * method_nogc requires num_clauses==0, and arm3 additionally requires gcp==1. */
				if (G_UNLIKELY (mono_wasm_jit_elidediag)) {
					/* INDEPENDENT CROSS-CHECK of the generation bookkeeping, by ORDINAL rather than by
					 * gc_gen. The arm-2 soundness claim is "an elided ref's def->use range crosses no GC
					 * point", but the walk enforces that through def_gen/gc_gen, and gc_gen RESETS per
					 * basic block while def_gen[] persists. Re-deriving the same property from raw
					 * instruction ordinals cannot share a bug with the generation counters, so a nonzero
					 * span_bad here is proof the walk elided a ref that a collector can observe -- the
					 * same role span_gcp=0 played when it exonerated the walk for the handoff arm.
					 * Second walk mirrors the first's bb/ins iteration exactly, so ordinals line up. */
					int span_bad = 0, span_max = 0, ngcp = 0, ncap = 0, o2 = 0, k;
					int *gords;
					MonoBasicBlock *b2;
					MonoInst *i2;
					for (b2 = cfg->bb_entry; b2; b2 = b2->next_bb)
						MONO_BB_FOR_EACH_INS (b2, i2)
							ncap++;
					gords = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (ncap + 1));
					for (b2 = cfg->bb_entry; b2; b2 = b2->next_bb)
						MONO_BB_FOR_EACH_INS (b2, i2) {
							if (wj_ins_is_published_gcpoint (cfg, i2))
								gords [ngcp++] = o2;
							o2++;
						}
					for (i = 0; i < nvreg; ++i)
						if (li [i] >= 0 && isref [i] && sl_elide [i] && !method_nogc && !needs_slot [i] &&
						    last_def_ord [i] >= 0 && last_use_ord [i] > last_def_ord [i]) {
							int c = 0;
							for (k = 0; k < ngcp; ++k)
								if (gords [k] > last_def_ord [i] && gords [k] < last_use_ord [i])
									c++;
							if (c > 0) {
								span_bad++;
								if (c > span_max)
									span_max = c;
							}
						}
					char *_en = mono_method_get_full_name (cfg->method);
					printf ("WASM_JIT_ELIDE %s nogc=%d clauses=%d gcp=%d skipped=%d handoff=%d refs=%d elided=%d arm1_nogc=%d arm2_needs=%d arm3_handoff=%d span_bad=%d span_max=%d\n",
					        _en, method_nogc ? 1 : 0, (int) cfg->header->num_clauses, effective_gcp_count,
					        method_skipped_raises, terminal_vcall_handoff ? 1 : 0,
					        nref_total, nelide, nel_nogc, nel_needs, nel_handoff, span_bad, span_max);
					g_free (_en);
				}
				/* Hand the elision set to LCSE (function-scope; isref/sl_elide are local to this block).
				 * An elided ref is only safe because its IR def->use range crosses no GC point, and an
				 * LCSE reuse read would extend that range. */
				lcse_nopin = sl_elide;
				/* DEAD-SLOT ZEROING (MONO_WASM_JIT_SLOTZERO, Phase 3a): a slotted single-bb vreg is
				 * provably dead after its last use — zero its slot there so the dead object stops
				 * pinning (a long-lived/JSPI-suspended frame otherwise pins it until frame pop).
				 * Emission happens at the ordinal AFTER the last use (a last use as a call arg must
				 * stay pinned across that whole call), which is always still inside the bb because a
				 * kill is only worth emitting when a GC point FOLLOWS the last use in the same bb
				 * (bb_last_gcp > last_use_ord) — without one, the slot is never observed again anyway.
				 * Candidates must be def-first (a loop-carried use-before-def vreg is live around the
				 * back edge in its wasm local, and under REF_WT only the slot copy pins it — zeroing
				 * would dangle the local) and single-bb (multi-bb death needs real liveness — Phase 3b).
				 * Requires REF_WT: reads come from the local, so the zero store is position-safe. */
				{
					extern int mono_wasm_jit_slotzero, mono_wasm_jit_ref_wt;
					if (mono_wasm_jit_slotzero && mono_wasm_jit_ref_wt && nins > 0) {
						int nzero = 0;
						sl_kill_next = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
						sl_kill_head = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (nins + 1));
						for (i = 0; i < nins + 1; ++i)
							sl_kill_head [i] = -1;
						for (i = 0; i < nvreg; ++i) {
							int ko;
							sl_kill_next [i] = -1;
							if (!(li [i] >= 0 && isref [i] && needs_slot [i] && !sl_elide [i]))
								continue;   /* only slotted vregs have anything to zero */
							if (sl_multi [i] || !sl_def_first [i] || last_use_ord [i] < 0)
								continue;
							if (ev_bb [i] < 0 || ev_bb [i] >= nbbs || bb_last_gcp [ev_bb [i]] <= last_use_ord [i])
								continue;   /* no GC point after the last use -> never observed again */
							ko = last_use_ord [i] + 1;
							sl_kill_next [i] = sl_kill_head [ko];
							sl_kill_head [ko] = i;
							nzero++;
						}
						if (nzero == 0)
							sl_kill_head = NULL;   /* nothing to do: skip the per-ins check in the emit loop */
						else if (mono_wasm_jit_stats)
							mono_wasm_jit_add (WJC_SLOT_ZERO_STORES, nzero);
					}
				}
			}
		}
		for (i = 0; i < nvreg; ++i)
			if (li [i] >= 0 && isref [i] && !(sl_elide && sl_elide [i]))
				refslot [i] = nrefslots++;
		/* ref-etype scalar-vtype locals (addrslot==-2 from the ldaddr pre-pass): each gets a GC-scanned
		 * ref-shadow slot so its single ref field is a tracked/pinning root (VTYPE_SCALAR_REF). */
		for (i = 0; i < nvreg; ++i)
			if (addrslot [i] == -2) {
				refslot [i] = nrefslots++;
			}
		if (nrefslots > 0)
			lc.refslot = refslot;
		/* Write-through (MONO_WASM_JIT_REF_WT): mark the slotted ref vregs whose wasm local becomes
		 * the value home, with the slot demoted to a def-mirrored pin copy (wasm_ld/wasm_st switch on
		 * this). Excluded: addrslot==-2 sentinels — their slot ADDRESS escapes to callees via OP_LDADDR
		 * (VTYPE_SCALAR_REF), so a callee write through that pointer must stay visible to reads, i.e.
		 * the slot must remain the home. All other slot accesses funnel through wasm_ld/wasm_st (plus
		 * the prologue arg copy, which already leaves the param local as the arg's home). */
		{
			extern int mono_wasm_jit_ref_wt;
			if (mono_wasm_jit_ref_wt && nrefslots > 0) {
				guint8 *ref_wt = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				int nwt = 0;
				for (i = 0; i < nvreg; ++i)
					if (refslot [i] >= 0 && li [i] >= 0 && addrslot [i] != -2) {
						ref_wt [i] = 1;
						nwt++;
					}
				if (nwt > 0)
					lc.ref_wt = ref_wt;
				if (mono_wasm_jit_stats)
					mono_wasm_jit_add (WJC_REF_WT_VREGS, nwt);
			}
		}
		/* C-stack frame size: ref slots first (4-byte), then the 8-aligned addr slots; whole frame
		 * 16-aligned per the emscripten SP ABI. */
		refbytes_al = (nrefslots * 4 + 7) & ~7;
		framebytes = (refbytes_al + naddrbytes + 15) & ~15;
		/* Lazy materialization is restricted to the representation where every root has a wasm-local
		 * value home. Address frames and EH need a valid frame from entry; slot-homed ref-vtype
		 * sentinels likewise cannot be reconstructed. Debug store guards intentionally keep the old
		 * eager path. */
		/* The GC-point count is a HEURISTIC here, not a correctness requirement, and it was pinned at 1.
		 * ENSURE_REF_FRAME is idempotent -- it tests whether the frame has already been materialised -- and
		 * the main emit loop already emits it before EVERY effective GC point (see the
		 * wj_ins_is_effective_gcpoint site), so a method with N of them is handled correctly today. The
		 * only question a larger limit raises is whether it PAYS: lazy costs a 3-op check at each GC point
		 * and saves the whole ~15-op frame prologue on any path that reaches none.
		 *
		 * Pinning it at 1 confined the mechanism to almost nothing. Measured on a whole-tier dump weighted
		 * by an in-game profile: 94.3% of our tier's execution time is in EAGER-frame methods, 2.4% lazy,
		 * 3.3% frameless -- while the prologue is 21.4% of that tier's time. MONO_WASM_JIT_LAZY_GCP makes
		 * the limit sweepable; <= 0 means no limit.
		 *
		 * SWEPT, AND 1 IS RIGHT. Removing the limit does exactly what it should to the emitted code -- eager
		 * frames fall from 94.3% of our tier's execution to 9.1%, lazy rises 2.4% -> 87.5% -- and is a large
		 * REGRESSION anyway. Interleaved A/B, plateau instrument, 240s cooldown per arm, 2 rounds:
		 *
		 *     p50        45.8 -> 54.6 ms   +19.3%   (spread +/-1.7%, far outside noise)
		 *     fps        21.6 -> 16.7      -22.7%
		 *     msFrame    46.3 -> 59.8 ms   +29.3%
		 *     world.gen  42.7 -> 134.0 s  +214.1%
		 *     boot.classload             +9.1%
		 *
		 * The price is code: median module 800 -> 1064 B (+33%), p90 2925 -> 5652 (+93%), because ENSURE
		 * lands at every GC point. Note WHERE it hurt most -- world generation, the compile-heavy phase,
		 * tripled. Compile cost alone cannot explain that (29.4 us fixed + 15.4 ns/wire-byte measured
		 * separately, so +264 B across 24k modules is ~0.1 s, not 91 s), so the bulk is more likely TurboFan
		 * coping badly with ~20 extra ops and a branch at every GC point, on a target already shown to be
		 * register-starved. Not chased further.
		 *
		 * An intermediate value was NOT swept: at 2 or 3 only methods with exactly that many GC points become
		 * lazy -- a small population -- and the gradient from 1 to unlimited is steeply negative. */
		if (framebytes > 0 &&
		    ((mono_wasm_jit_lazy_gcp <= 0 || effective_gcp_count <= mono_wasm_jit_lazy_gcp) ||
		     (terminal_vcall_handoff && terminal_vcall_poll_prefix)) &&
		    naddrbytes == 0 && !eh_on && lc.ref_wt &&
		    !lc.storeguard && !lc.objguard) {
			gboolean all_wt = TRUE;
			for (i = 0; i < nvreg; ++i)
				if (refslot [i] >= 0 && (!lc.ref_wt [i] || li [i] < 0)) {
					all_wt = FALSE;
					break;
				}
			if (all_wt) {
				lazy_ref_frame = TRUE;
				lc.lazy_frame = 1;
			}
		}
		if (mono_wasm_jit_stats) {
			mono_wasm_jit_add (WJC_REF_SLOTS, nrefslots);
			mono_wasm_jit_add (WJC_FRAME_BYTES, framebytes);
		}

		/* MISSED-REF FINDER (MONO_WASM_JIT_MISSEDREF=1): the corruptor class is a ref left in a plain
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
		 * (R=ref shadow-stack slot, r=isref but slot ELIDED by SLOTLIVE, -=plain wasm local, .=n/a),
		 * and for calls the captured call_info arg
		 * vregs + flags + virt/ret_ref. A managed ref left in a plain local (-) that is live across a GC
		 * point (a call/virtual resolve) is the dangling-pointer corruptor; this shows the exact op that
		 * produced it (and whether a call receiver/arg is being spilled from an unclassified local). */
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name))) {
			MonoBasicBlock *bb2; MonoInst *ins2;
#define WJ_RF(v) (((v) >= 0 && (v) < nvreg) ? (isref [v] ? ((sl_elide && sl_elide [v]) ? 'r' : 'R') : '-') : '.')
			{ printf ("WASM_JIT_IR === %s nvreg=%d nrefslots=%d nargs=%d lcseflag=%d ===\n", cfg->method->name, nvreg, nrefslots, nargs, mono_wasm_jit_lcse); }
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
							int na = ((WjCallArgs *) c->call_info)->nargs;
							int k;
							n += snprintf (b + n, sizeof b - n, " virt=%d ret_ref=%d args[",
								c->method ? !!(c->method->flags & METHOD_ATTRIBUTE_VIRTUAL) : -1,
								mini_type_is_reference (c->signature->ret));
							for (k = 0; k < na && n < (int) sizeof b - 14; ++k) {
								int av = wj_arg_vreg (c, k);
								n += snprintf (b + n, sizeof b - n, "%s%d%c", k ? "," : "", av, WJ_RF (av));
							}
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
 * long opcodes). cppeh: the raise C++-throws and never returns (unreachable). `ldiv_rti` (the (i32)->void
 * functype index) must be in scope. Both uses sit inside an `if` arm, so the ENSURE_REF_FRAME here costs
 * the hot path nothing; see the shared throw blocks for why a raise needs a materialized frame. */
#ifdef HOST_BROWSER
#define LDIV_RAISE_FPTR ((gpointer) (intptr_t) mono_wasm_jit_raise_corlib)
#else
#define LDIV_RAISE_FPTR ((gpointer) (intptr_t) 0x7ff8)   /* offline dump: placeholder table index */
#endif
#ifdef HOST_BROWSER
/* Direct import rather than `i32.const <addr>; call_indirect` -- same reason as EMIT_REF_LEAVE: a constant
 * index is not folded into a direct call by V8. raise_corlib is a C function in dotnet.native.wasm. */
#define LDIV_RAISE(EXC_ID) do { \
		RAISE_ENSURE_REF_FRAME ();   /* raise_corlib allocates: roots must be scannable. Taken arm only. */ \
		extern void mono_wasm_jit_raise_corlib (int exc_id); \
		wasm_i32_const (&body, (EXC_ID)); \
		wj_emit_helper_call (&body, LDIV_RAISE_FPTR, (guint32) ldiv_rti); \
		wasm_op (&body, WASM_OP_UNREACHABLE);   /* mono_wasm_jit_raise_corlib C++-throws and never returns */ \
	} while (0)
#else
#define LDIV_RAISE(EXC_ID) do { \
		RAISE_ENSURE_REF_FRAME (); \
		wasm_i32_const (&body, (EXC_ID)); \
		wj_emit_helper_call (&body, LDIV_RAISE_FPTR, (guint32) ldiv_rti); \
		wasm_op (&body, WASM_OP_UNREACHABLE); \
	} while (0)
#endif
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
 * global.set consumes only entry_sp, so it leaves any return value on the stack. No-op for
 * frame-less methods. */
/* A residual/vcall/delegate callee reported threw==1: CONTINUE THE NATIVE UNWIND, never return.
 *
 * The boundary (mono_wasm_jit_invoke_caught) has already caught the C++ unwind and, for a handler in a
 * JITted island above us, deliberately keeps the exception armed in the interp resume state. Returning
 * normally here loses the exceptional edge: this frame's own landing pad is skipped, its JIT caller runs
 * on unaware, and the exception surfaces later at an unrelated frame (see FINDINGS Finding 15).
 *
 * Deliberately NO EMIT_REF_LEAVE() before the call. If this method has a matching handler, its landing
 * pad needs the frame and island still live; the pad restores __stack_pointer to its own refbase itself
 * (REF-SP-1). If it has no match, its pad does leave_island + rethrow, a non-EH frame is simply torn
 * through, and the next outer pad -- or the interp boundary's saved SP -- does the restore. Popping the
 * roots here would strip them before this method's own catch/finally gets control.
 *
 * No dummy return value either: `unreachable` makes the rest of the block polymorphic, so the void
 * blocktype validates without one. */
#define EMIT_RESIDUAL_THROW_CONTINUATION() do { \
		extern void mono_wasm_jit_continue_unwind (void); \
		g_assert (resid_void_ti >= 0); \
		wj_emit_helper_call_ci (&body, WJ_CONTINUE_UNWIND_ADDR, (guint32) resid_void_ti); \
		wasm_op (&body, WASM_OP_UNREACHABLE); \
	} while (0)

#ifdef HOST_BROWSER
#define WJ_CONTINUE_UNWIND_ADDR ((gpointer) (intptr_t) mono_wasm_jit_continue_unwind)
#else
#define WJ_CONTINUE_UNWIND_ADDR ((gpointer) (intptr_t) 0x7ff8)   /* offline dump: placeholder table index */
#endif

#ifdef HOST_BROWSER
#define EMIT_REF_LEAVE() do { if (framebytes > 0) { \
		if (lazy_ref_frame) { \
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
		} \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
		wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
		if (lazy_ref_frame) wasm_op (&body, WASM_OP_END); \
	} \
	if (eh_on) {   /* pop this EH method's il_state island (pushed by enter_island in the prologue) */ \
		extern void mono_wasm_jit_leave_island (void); \
		/* Direct import, not `i32.const <addr>; call_indirect`: V8 does not fold a constant index into a \
		 * direct call, so the indirect form pays a table bounds check, a canonical-type check, index->code \
		 * arithmetic and a validity check -- about fifteen x86 instructions where a direct `call` is one. \
		 * This macro runs before EVERY return of an EH-bearing method, so it is one of the densest constant \
		 * targets in the whole tier. Safe to import because the address is a C function in \
		 * dotnet.native.wasm: fixed for the process, never repointed. (Contrast the method f-slot in the \
		 * ordinary direct-call path, which IS repointed on rebind and therefore must stay indirect.) */ \
		wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_leave_island, (guint32) eh_endcatch_ti); \
	} } while (0)
#else
#define EMIT_REF_LEAVE() do { } while (0)
#endif

/* Materialize a lazy pure-ref frame immediately before the first instruction that can return after a
 * GC. The current SP is still this method's entry SP: everything executed before here is inline-pure or
 * a transitively no-GC/frame-free direct callee. Copy every write-through local only after reserving and
 * zeroing the frame. The captured nonzero entry SP is also the active marker; later definitions use it
 * to mirror conditionally in wasm_st. */
#ifdef HOST_BROWSER
#define ENSURE_REF_FRAME() do { if (lazy_ref_frame) { int _rf_i; \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_I32_EQZ); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_i32_const (&body, framebytes); wasm_op (&body, WASM_OP_I32_SUB); \
	wasm_i32_const (&body, -16); wasm_op (&body, WASM_OP_I32_AND); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_i32_const (&body, 0); wasm_i32_const (&body, framebytes); \
	wasm_u8 (&body, 0xFC); wasm_uleb (&body, 11); wasm_u8 (&body, 0); \
	for (_rf_i = 0; _rf_i < nvreg; ++_rf_i) if (refslot [_rf_i] >= 0) { \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) li [_rf_i]); \
		wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [_rf_i] * 4)); \
	} \
	wasm_op (&body, WASM_OP_END); \
} } while (0)
#else
#define ENSURE_REF_FRAME() do { } while (0)
#endif

/* A conditional safepoint poll needs roots only in its taken arm. After the poll returns, the wasm
 * locals contain the same pinned object addresses and no GC can observe them again until the next
 * materialization/handoff. Pop the lazy frame and clear its active marker so a later poll can rebuild
 * it. This is what keeps the overwhelmingly-common poll-not-requested path frame-free. */
#ifdef HOST_BROWSER
#define RELEASE_LAZY_REF_FRAME() do { if (lazy_ref_frame) { \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
	wasm_i32_const (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_END); \
} } while (0)
#else
#define RELEASE_LAZY_REF_FRAME() do { } while (0)
#endif

/* Materialize before a call that C++-throws, so exception allocation cannot run while this method's
 * roots live only in wasm locals. */
#define RAISE_ENSURE_REF_FRAME() ENSURE_REF_FRAME ()


	/* MONO_WASM_JIT_NCE: per-bb "already proven non-null" vreg bitmap (see the flag's comment).
	 * Reset at the top of every bb, so nothing crosses a control-flow edge and no dominance
	 * information is needed. */
	guint8 *nn = NULL;
	if (mono_wasm_jit_nce && nvreg > 0)
		nn = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
	/* Trackable = a real vreg that lives in a wasm local. Address-taken vregs (addrslot != -1, which
	 * includes the -2 ref-vtype sentinels) are homed in the addressable-locals frame and their address
	 * escapes to callees, so a callee store could change them behind our back: never track those. */
#define NN_OK(VR)   ((nn) && (VR) >= 0 && (VR) < nvreg && !(lc.addrslot && lc.addrslot [(VR)] != -1))
#define NN_GET(VR)  (NN_OK (VR) && nn [(VR)])
#define NN_SET(VR)  do { if (NN_OK (VR)) nn [(VR)] = 1; } while (0)
#define NN_KILL(VR) do { if ((nn) && (VR) >= 0 && (VR) < nvreg) nn [(VR)] = 0; } while (0)

	/* dense bb indexing for the dispatch table */
	bbidx = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
	for (i = 0; i < (int) cfg->max_block_num + 1; ++i)
		bbidx [i] = -1;
	N = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		bbidx [bb->block_num] = N++;
	structured_cfg_kind = wj_structured_cfg_kind (cfg, bbidx, N,
		&structured_loop_h, &structured_loop_l);

	/* MONO_WASM_JIT_LCSE state.
	 *
	 * The table flows from a bb to a successor only when that successor has EXACTLY ONE predecessor —
	 * i.e. along the single edge of an extended basic block. That is what makes it sound with no path
	 * analysis at all: with one predecessor there is no other way in, so nothing unaccounted-for can run
	 * between the two blocks. Dominance alone would NOT be enough (idom(bb) can reach bb through
	 * intervening blocks that store), which is why this does not reuse the NCE level-2 walk.
	 *
	 * That single-predecessor shape is exactly the ternary arm this targets, so the restriction costs
	 * nothing on the measured case.
	 *
	 * Disabled under STOREGUARD/OBJGUARD: those wrap each access in a check_store call, and eliding a
	 * load would elide its guard, changing what the guard run verifies. */
	WjLcse *lcse = NULL;
	WjLcse **lcse_exit = NULL;
	int lcse_adds = 0, lcse_hits = 0, lcse_inherits = 0;
	{
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name)))
			printf ("WASM_JIT_LCSE %s: flag=%d nvreg=%d sg=%d og=%d\n",
				cfg->method->name, mono_wasm_jit_lcse, nvreg, lc.storeguard, lc.objguard);
	}
	if (mono_wasm_jit_lcse && nvreg > 0 && !lc.storeguard && !lc.objguard) {
		guint8 *needed = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) (N > 0 ? N : 1));
		MonoBasicBlock *b3;
		int nneed = 0;

		lcse = (WjLcse *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse));
		/* Only blocks that some LATER block inherits from need their exit state kept; for the diamond
		 * that is just the compare block. Saving one table per bb unconditionally would cost sizeof(WjLcse)
		 * times the block count, which is real for the 7000-instruction methods here. */
		for (b3 = cfg->bb_entry; b3; b3 = b3->next_bb) {
			int ci = bbidx [b3->block_num], pi;
			if (ci < 0 || b3->in_count != 1 || !b3->in_bb [0])
				continue;
			if (b3->flags & BB_EXCEPTION_HANDLER)
				continue;   /* reached by an EH edge, not by falling out of in_bb[0] */
			pi = (b3->in_bb [0]->block_num <= (int) cfg->max_block_num) ? bbidx [b3->in_bb [0]->block_num] : -1;
			if (pi < 0 || pi >= ci)
				continue;   /* back edge: the predecessor has not been emitted yet */
			if (!needed [pi]) { needed [pi] = 1; nneed++; }
		}
		if (nneed > 0) {
			lcse_exit = (WjLcse **) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse *) * (gsize) (N > 0 ? N : 1));
			for (i = 0; i < N; ++i)
				if (needed [i])
					lcse_exit [i] = (WjLcse *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse));
		}
		{
			extern gboolean mono_wasm_jit_refdiag_name (const char *);
			if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name)))
				printf ("WASM_JIT_LCSE %s: on, N=%d nvreg=%d single-pred-sources=%d\n",
					cfg->method->name, N, nvreg, nneed);
		}
	}

	/* NCE level 2: cross-bb facts, propagated down the DOMINATOR tree.
	 *
	 * The bb-local set alone leaves the commonest shape untouched. Measured on AABB:combine: its two
	 * AABB parameters are null-checked EIGHT TIMES EACH, spread over 15 basic blocks (the method is a
	 * chain of min/max branches), so a set that resets at every block boundary removes none of them.
	 * ContactSolver:solveVelocityConstraints, which is one huge block, halved from bb-local alone.
	 *
	 * A fact only survives an edge if the vreg cannot have changed in between, so this propagates ONLY
	 * vregs with no definition anywhere in the method -- parameters and `this`. "Exactly one defining
	 * instruction" is NOT enough: a def inside a loop re-executes, so a check in a dominator outside
	 * the loop says nothing about the second iteration. Never-written is the rule that needs no
	 * reasoning about back edges, and it is what the measured case wants anyway.
	 *
	 * Recognising the check here does not reuse the emitter's compare fusing; it matches the IR pair
	 * directly (OP_COMPARE_IMM imm=0 followed by OP_COND_EXC_EQ "NullReferenceException"), which is the
	 * shape MONO_EMIT_NULL_CHECK expands to and the same rule abcremoval.c uses. Requiring adjacency
	 * (modulo nops) can only miss facts, never invent them. */
	guint8 **nn_in = NULL;
	/* Clause-free only. The dominator relation does not model implicit exception transfers, so a
	 * non-null fact established on a null check's normal continuation can be false inside a catch or
	 * finally reached from BEFORE that check ran. A method with no EH clauses has no such transfer to
	 * be wrong about, which makes the propagation sound without any dataflow reasoning -- the same
	 * argument, keyed off the same property, as the RAISE_NOGC gate above. */
	if (mono_wasm_jit_nce >= 2 && nn && cfg->bb_entry && (cfg->comp_done & MONO_COMP_IDOM) &&
	    cfg->header->num_clauses == 0) {
		guint8 *nn_written = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
		MonoBasicBlock **stk, *b2;
		MonoInst *i2;
		int sp = 0, di, cap = (N > 0 ? N : 1) + 1;

		for (b2 = cfg->bb_entry; b2; b2 = b2->next_bb)
			MONO_BB_FOR_EACH_INS (b2, i2)
				if (i2->dreg >= 0 && i2->dreg < nvreg) nn_written [i2->dreg] = 1;
		if (cfg->ret && cfg->ret->dreg >= 0 && cfg->ret->dreg < nvreg) nn_written [cfg->ret->dreg] = 1;

		nn_in = (guint8 **) mono_mempool_alloc0 (cfg->mempool, sizeof (guint8 *) * (gsize) cap);
		for (di = 0; di < N; ++di)
			nn_in [di] = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);

		/* pre-order walk: a bb's entry facts are its immediate dominator's exit facts, so every parent
		 * is processed before its children. Each bb has exactly one idom, hence appears once as a child. */
		stk = (MonoBasicBlock **) mono_mempool_alloc (cfg->mempool, sizeof (MonoBasicBlock *) * (gsize) cap);
		stk [sp++] = cfg->bb_entry;
		while (sp > 0) {
			MonoBasicBlock *d = stk [--sp];
			int dix = bbidx [d->block_num];
			guint8 *out;
			GSList *ch;
			if (dix < 0 || dix >= N) continue;
			out = (guint8 *) mono_mempool_alloc (cfg->mempool, (gsize) nvreg);
			memcpy (out, nn_in [dix], (gsize) nvreg);
			MONO_BB_FOR_EACH_INS (d, i2) {
				int cv = -1;
				if (i2->opcode == OP_NOT_NULL || i2->opcode == OP_CHECK_THIS) {
					cv = i2->sreg1;
				} else if ((i2->opcode == OP_COMPARE_IMM || i2->opcode == OP_ICOMPARE_IMM) && i2->inst_imm == 0) {
					MonoInst *nx = i2->next;
					while (nx && (nx->opcode == OP_NOP || nx->opcode == OP_IL_SEQ_POINT || nx->opcode == OP_SEQ_POINT))
						nx = nx->next;
					if (nx && (nx->opcode == OP_COND_EXC_EQ || nx->opcode == OP_COND_EXC_IEQ) &&
					    nx->inst_p1 && !strcmp ((const char *) nx->inst_p1, "NullReferenceException"))
						cv = i2->sreg1;
				}
				if (cv >= 0 && cv < nvreg && !nn_written [cv] && !(lc.addrslot && lc.addrslot [cv] != -1))
					out [cv] = 1;
			}
			for (ch = d->dominated; ch; ch = ch->next) {
				MonoBasicBlock *c = (MonoBasicBlock *) ch->data;
				int cix = (c->block_num <= (int) cfg->max_block_num) ? bbidx [c->block_num] : -1;
				if (cix < 0 || cix >= N) continue;
				memcpy (nn_in [cix], out, (gsize) nvreg);
				if (sp < cap) stk [sp++] = c;
			}
		}
	}

	/* In-method EH: build the per-method clause table the catch landing pad's dispatch helper walks.
	 * Maps each dense bb -> its IL offset (for is-address-protected), and each clause -> {flags, try
	 * IL-range, handler bbidx, catch_class}. g_malloc'd so it outlives this compile (the JITted module
	 * bakes its address); a small per-EH-method leak. */
	if (eh_on) {
		guint _ci; int _d;
		eh_table = (WasmEhTable *) g_malloc0 (sizeof (WasmEhTable));
		eh_table->method = cfg->method;
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
				/* bb->cil_code points into the IL of whichever method the block came FROM. For an INLINED
				 * block that is the INLINEE's buffer, so `bb->cil_code - cfg->header->code` is pointer
				 * arithmetic across two unrelated allocations and yields an arbitrary offset -- which is then
				 * what eh_dispatch tests against THIS method's try ranges at runtime. It is wrong in both
				 * directions: an inlined block genuinely inside a try can be scored outside it, so a catch
				 * that should fire does not and the exception escapes; or one outside can be scored inside,
				 * and the landing pad over-catches.
				 *
				 * mono keeps the caller-space offset in bb->real_offset: inline_method seeds the inlined
				 * blocks with the CALL SITE's offset (method-to-ir.c, sbblock/ebblock->real_offset =
				 * real_offset, called with cfg->real_offset), and mono's own mark_bb_in_region compares
				 * real_offset -- not cil_code arithmetic -- against clause ranges. It is also the
				 * semantically right answer here: inlined code is logically at the call site, so it should
				 * inherit exactly the call site's protection.
				 *
				 * Found via MONO_INLINELIMIT: the default of 20 IL bytes inlines too little to expose this,
				 * but at 30 enough inlined blocks land inside EH methods that a gson catch stopped firing and
				 * Fabric's mixin bootstrap died during boot. `auto=0` (whole JIT tier off) at the same limit
				 * boots fine, which is what pins it here rather than in the inliner. */
				gint32 _off = -1;
				if (bb->cil_code) {
					if (bb->cil_code >= cfg->header->code &&
					    bb->cil_code < cfg->header->code + cfg->header->code_size)
						_off = (gint32) (bb->cil_code - cfg->header->code);
					else
						_off = (gint32) bb->real_offset;   /* inlined: cil_code belongs to the inlinee */
				}
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
				/* Range-guarded for the same reason as the il_offsets loop above: only a block whose
				 * cil_code lies inside THIS method's IL can be the clause's handler entry. Without the test,
				 * an inlined block's cross-buffer difference can equal handler_offset by coincidence and --
				 * because this loop breaks on the first match -- send every catch to the wrong code. Rarer
				 * than the offset bug, worse when it happens. */
				if (_hb->cil_code &&
				    _hb->cil_code >= cfg->header->code &&
				    _hb->cil_code < cfg->header->code + cfg->header->code_size &&
				    (gint32) (_hb->cil_code - cfg->header->code) == (gint32) _c->handler_offset) { _hbb = bbidx [_hb->block_num]; break; }
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

	/* Prologue: $blk is needed only by the universal dispatcher/EH landing pad. Structured non-EH
	 * layouts enter their first lexical block directly and never read it. */
	if (structured_cfg_kind == WJ_CFG_DISPATCH &&
	    !(mono_wasm_jit_nodispatch && N == 1 && !eh_on && cfg->bb_entry && cfg->bb_entry->out_count == 0)) {
		wasm_i32_const (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
	}

#ifdef HOST_BROWSER
	/* C-STACK FRAME PROLOGUE. Reserve this method's ref+addr slots as a real frame on the emscripten
	 * C stack (see the layout doc at the top of this file): capture entry_sp, drop the SP by the
	 * 16-aligned frame size, zero the frame (GC must not scan garbage; .NET locals are zero-init),
	 * copy reference args into their slots, and derive addrbase. Every exit stackRestores entry_sp
	 * (EMIT_REF_LEAVE); the EH landing pad stackRestores refbase (this frame stays live, unwound
	 * callee frames fall below the SP and stop being scanned — no zeroing, no balance bookkeeping).
	 * An EH method with an EMPTY frame still captures entry_sp (refbase = entry_sp) so its landing
	 * pad can pop the frames of callees the C++ unwind tore through. */
	if ((framebytes > 0 && !lazy_ref_frame) || eh_on) {
		uses_calls = TRUE;
		/* entry_sp = __stack_pointer (imported wasm global 0) */
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx);
		if (framebytes > 0 && !lazy_ref_frame) {
			/* refbase (frame base) = (entry_sp - framebytes) & ~15; __stack_pointer = refbase */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_i32_const (&body, framebytes);
			wasm_op (&body, WASM_OP_I32_SUB);
			wasm_i32_const (&body, -16);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
			/* memory.fill (refbase, 0, framebytes): the frame is above the SP now, so the GC scans it —
			 * it must hold no garbage/stale pointers; .NET local zero-init falls out of the same fill */
			{
				extern int mono_wasm_jit_inline_zero;
				if (framebytes <= mono_wasm_jit_inline_zero) {
					/* framebytes is 16-aligned and a multiple of 16, so this is exactly framebytes/8 aligned
					 * i64 stores — no remainder, no alignment fixup. */
					int zo;
					for (zo = 0; zo < framebytes; zo += 8) {
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
						wasm_i64_const (&body, 0);
						wasm_op (&body, WASM_OP_I64_STORE);
						wasm_memarg (&body, 3, (guint32) zo);
					}
				} else {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					wasm_i32_const (&body, 0);
					wasm_i32_const (&body, framebytes);
					wasm_u8 (&body, 0xFC); wasm_uleb (&body, 11); wasm_u8 (&body, 0);   /* memory.fill mem 0 (bulk memory) */
				}
			}
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
			/* Reconstruct ref-free scalar-vtype parameters in their address-frame slots. Ref-bearing
			 * scalar structs were copied to ref slots by the loop above. */
			for (i = 0; i < nargs; ++i) {
				int av = cfg->args [i]->dreg;
				if (self_ci.args [i].kind == WJ_ARG_VTYPE_SCALAR && av >= 0 && av < nvreg && addrslot [av] >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) i);
					if (!wasm_addr_st (&body, &lc, av)) { fail = "scalar-vtype arg store"; goto done; }
				}
			}
		} else {
			/* eh_on with an empty frame: refbase = entry_sp is the landing pad's restore target */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
		}
	}
	if (lazy_ref_frame)
		uses_calls = TRUE; /* imports the mutable __stack_pointer global used by ENSURE_REF_FRAME */
	/* INLINE f-slot-IC liveness prologue: cache this dynamic instance's imported addresses of the
	 * wj_slot_live bitmap pointer + capacity. The imports are bound to the current worker's TLS block when
	 * the cached module is instantiated, so two global.get operations replace the former two C-boundary
	 * address-helper calls per invocation. Each IC hit still loads THROUGH these stable addresses, making
	 * a later bitmap realloc/capacity growth visible with no stale-pointer window. */
	if (mono_wasm_jit_vcall_inline_ic && has_vcall) {
		extern int mono_wasm_jit_delegate_local_pic;
		/* METHOD-LONG LIVE RANGES ARE NOT FREE -- the LOCALS are.
		 *
		 * Be precise about which, because the two suggest opposite work. A wasm local is free: V8's
		 * `LocalGet` is `result->op = ssa_env_[imm.index]` and `LocalSet` is `ssa_env_[imm.index] = value.op`
		 * (v8/src/wasm/turboshaft-graph-interface.cc:1030-1043), so local ops emit NO instructions and
		 * renaming or reusing local slots cannot change anything at runtime (which is why MONO_WASM_JIT_COALESCE
		 * measured inert). What costs is the DATAFLOW: a value that is live across other calls has to survive
		 * them, and the census of V8's x86 output attributes 24.2% of emitted instructions to TurboFan's own
		 * register spills (rbp/rsp traffic).
		 *
		 * So the thing to minimise is not the local count but how many values this prologue keeps alive for the
		 * whole body, each competing with the method's real values. That set is now the minimum:
		 *   - s.l / s.c (slot-live bitmap) are read ONLY by the shared WjDelegateIC arm, so they are not
		 *     fetched at all when the worker-local delegate PIC is on (the default) — 2 locals reclaimed;
		 *   - the cap ADDRESSES are consumed immediately to load their values, so they never need a local at
		 *     all: global.get; i32.load; local.set value — 2 more reclaimed.
		 * That is 4 fewer method-long locals in every method containing a virtual call. */
		if (!mono_wasm_jit_delegate_local_pic) {
			wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 1); /* imported s.l = &wj_slot_live */
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) slotlive_ptr_idx);
			wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 2); /* imported s.c = &wj_slot_live_cap */
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) slotlive_cap_idx);
		}
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 3); /* imported s.v = &wj_vcall_pic */
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vpic_ptr_idx);
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 4); /* imported s.n = &wj_vcall_pic_cap */
		wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vpic_capval_idx);
		{
			/* The delegate PIC pair, on the same terms. Fetched unconditionally with the rest rather than
			 * gated on "has a delegate site": the gate would need a second prescan, and two global.get in a
			 * prologue that already does four are not worth another pass over the IR. */
			if (mono_wasm_jit_delegate_local_pic) {
				wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 5); /* imported s.d = &wj_delegate_pic */
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dpic_ptr_idx);
				wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 6); /* imported s.m = &wj_delegate_pic_cap */
				wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dpic_capval_idx);
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
	{
		/* ()->void for mono_wasm_jit_continue_unwind. Registered for every method, not just eh_on ones:
		 * a non-EH JIT frame must ALSO continue the unwind rather than return, otherwise it is skipped
		 * and the exception is delivered late at an unrelated frame. functype_eq dedups, so an eh_on
		 * method shares this entry with eh_endcatch_ti. */
		WasmFuncType _t; int _k;
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { resid_void_ti = ti_base + _k; break; }
		if (resid_void_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; resid_void_ti = ti_base + nextra++; }
	}
	if (eh_on) {
		WasmFuncType _t; int _k;
		/* (i32)->void: the x.e tag type AND mono_jiterp_begin_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 1; _t.params [0] = WASM_I32; _t.ret = WASM_VOID;
		eh_type_idx = -1;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_type_idx = ti_base + _k; break; }
		if (eh_type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_type_idx = ti_base + nextra++; }
		/* (i32,i32)->i32: mono_wasm_jit_eh_dispatch (table, blk) -> handler bbidx (a finally/fault
		 * handler's bbidx is tagged with WJ_EH_DISPATCH_FINALLY_BIT; the landing pad strips it) or -1 */
		memset (&_t, 0, sizeof _t); _t.nparams = 2; _t.params [0] = WASM_I32; _t.params [1] = WASM_I32; _t.ret = WASM_I32;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_dispatch_ti = ti_base + _k; break; }
		if (eh_dispatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_dispatch_ti = ti_base + nextra++; }
		/* ()->void: mono_jiterp_end_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_endcatch_ti = ti_base + _k; break; }
		if (eh_endcatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_endcatch_ti = ti_base + nextra++; }
		uses_calls = TRUE;
		uses_eh_tag = TRUE;
		/* PROLOGUE: push this EH method's il_state island so mono's exception pass-1 sees it on ANY entry
		 * (interp->JIT boundary OR direct JIT->JIT f-slot call). method ptr baked; (i32)->void = eh_type_idx.
		 * Popped at every exit by EMIT_REF_LEAVE + the rethrow paths. */
		wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
#ifdef HOST_BROWSER
		{
			extern gpointer mono_wasm_jit_enter_island (MonoMethod *m);
			extern int mono_wasm_jit_inline_ilofs;
			/* enter_island now RETURNS this activation's MonoMethodILState, so its wasm signature is
			 * (i32)->i32 and the call must be typed that way on BOTH paths — typing it (i32)->void because
			 * the result happens to be unused is a signature mismatch, i.e. an instant call_indirect trap or
			 * a failed import. Register the type through the same extras table the call sites use. */
			WasmFuncType nt;
			int k;
			memset (&nt, 0, sizeof (nt));
			nt.params [0] = WASM_I32; nt.nparams = 1; nt.ret = WASM_I32;
			for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { ilofs_ti = ti_base + k; break; }
			if (ilofs_ti < 0) {
				if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
				extra_types [nextra] = nt; ilofs_ti = ti_base + nextra++;
			}
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_enter_island, (guint32) ilofs_ti);
			if (ilstate_idx >= 0 && mono_wasm_jit_inline_ilofs)
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) ilstate_idx);   /* cache for the per-bb store */
			else
				wasm_op (&body, WASM_OP_DROP);
		}
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff6, (guint32) eh_type_idx);
#endif
		wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);   /* $outer: catch re-dispatch loop */
		wasm_op (&body, WASM_OP_TRY); wasm_u8 (&body, 0x40);    /* in-method EH try region (catches x.e) */
	}
	/*
	 * SHARED THROW BLOCKS.
	 *
	 * Every raise used to be inlined at its check site: `if (cond) { call_indirect <raise>; unreachable }`.
	 * AABB:combine had 23 of those and TurboFan turned the method into 880 instructions with 118 `call`s,
	 * where RyuJIT needs ~40 and zero. The cost is not the calls themselves -- they are on cold paths --
	 * it is that each `call_indirect` is a CALL SITE to the register allocator (everything live across it
	 * must be assumed clobbered) and each one carries V8's table-index + signature-check sequence, which
	 * is where the 23 matching or/shl/shr triples in the disassembly come from.
	 *
	 * So emit ONE raise per exception id, past the end of the body, and branch to it:
	 *     block $throw_id ... br_if $throw_id ... end ; <raise id> ; unreachable
	 * A check becomes three instructions and no call.
	 *
	 * Placement: inside the EH try when eh_on, or a raise from the shared block would no longer be
	 * catchable by this method's own landing pad. That also makes the depth arithmetic uniform -- from
	 * inside bb_i the labels are B_{i+1}..B_{N-1} (N-1-i of them), the dispatch loop, then throw[0..K-1],
	 * so throw[t] sits at depth N - i + t regardless of whether the EH scaffolding is present.
	 */
	/*
	 * A single-basic-block method has nowhere to dispatch to, yet it still paid the full
	 * `loop { block { local.get $blk; br_table } }` scaffolding: a $blk init in the prologue, an
	 * indirect jump at entry, and a trailing `unreachable`. That is most of why
	 * TreeNodeStack:getCount() -- literally `return count;` -- compiled to 103 instructions and 523
	 * bytes. Skip the scaffolding entirely when there is exactly one bb, it has no outgoing edges (so
	 * no branch can target it and no back edge exists), and there is no EH wrapper whose landing pad
	 * would re-dispatch through $blk.
	 */
	gboolean legacy_skip_dispatch = mono_wasm_jit_nodispatch && (N == 1 && !eh_on &&
		cfg->bb_entry && cfg->bb_entry->out_count == 0);
	gboolean structured_cfg = structured_cfg_kind != WJ_CFG_DISPATCH;
	gboolean skip_dispatch = structured_cfg || legacy_skip_dispatch;

	int wj_throw_slot [WJ_EXC_IDS];
	int nthrow = 0;
	for (i = 0; i < WJ_EXC_IDS; ++i) wj_throw_slot [i] = -1;
	{
		MonoBasicBlock *tb;
		MonoInst *ti;
		gboolean want [WJ_EXC_IDS];
		memset (want, 0, sizeof (want));
		for (tb = cfg->bb_entry; tb; tb = tb->next_bb) {
			MONO_BB_FOR_EACH_INS (tb, ti) {
				switch (ti->opcode) {
				/* NB: OP_NOT_NULL is deliberately absent -- it is a fact marker that emits nothing, so it
				 * never branches to the NRE block and must not be what keeps that block alive. */
				case OP_CHECK_THIS:
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
				case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
					want [4] = TRUE;   /* NullReferenceException: explicit checks + vcall receiver check */
					break;
				default:
					if (MONO_IS_COND_EXC (ti)) {
						int id = wj_exc_id_for_name ((const char *) ti->inst_p1);
						if (id >= 0) want [id] = TRUE;
					}
					break;
				}
			}
		}
		for (i = 0; i < WJ_EXC_IDS; ++i)
			if (want [i]) wj_throw_slot [i] = nthrow++;
	}
	/* Outermost first, so the last one emitted (throw[0]) is innermost and depth N-i+t holds. */
	for (i = nthrow - 1; i >= 0; --i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }

	if (!skip_dispatch) {
		wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);
		for (i = 0; i < N; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dispatch_idx);
		wasm_op (&body, WASM_OP_BR_TABLE);
		wasm_uleb (&body, (guint32) N);
		for (i = 0; i < N; ++i)
			wasm_uleb (&body, (guint32) i);
		wasm_uleb (&body, 0); /* default -> block 0 */
	} else if (structured_cfg_kind == WJ_CFG_FORWARD) {
		/* One lexical label per bb. Closing the first label enters bb0; the remaining labels are
		 * precisely the direct targets of forward edges. A one-bb leaf needs no label at all. */
		if (N > 1)
			for (i = 0; i < N; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
	} else if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP) {
		int suffix_n = N - (structured_loop_l + 1);
		/* Suffix labels surround the whole prefix+loop so an early exit can jump over both. Prefix
		 * labels are innermost and close one-by-one until execution reaches the loop entry. */
		for (i = 0; i < suffix_n; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
		/* One extra prefix block labels the loop entry itself, so a branch can skip the remaining
		 * prefix without requiring a dispatcher. No prefix means entry falls straight into the loop. */
		if (structured_loop_h > 0)
			for (i = 0; i < structured_loop_h + 1; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
	}

/*
 * Edge lowering. EXTRA is the number of wasm blocks opened INSIDE this bb's code at the branch point
 * (0 at bb top level, 1 inside an `if`/`else`) -- not an absolute depth as it used to be.
 *
 * The old form sent EVERY edge, including plain fallthrough, back through the dispatch loop:
 * `i32.const j; local.set $blk; br <loop>`, re-running `local.get $blk; br_table`. TurboFan lowers that
 * br_table to `lea <table>(%rip),%r10; jmp *(%r10,%rN,8)` -- an INDIRECT jump with as many live targets
 * as the method has basic blocks. Measured on the jbox2d bench, those two instructions are 23.8% of
 * AABB:combine, 17.3% of DynamicTree:insertLeaf, 11.8% of DynamicTree:query, ~7% of ContactSolver and
 * of DynamicTree:balance. It is not the branch itself that costs -- it is that an indirect jump cycling
 * through ~15 targets defeats the indirect predictor, and every miss is a front-end refill. RyuJIT
 * compiles the same control flow to short direct jbe/jmp pairs and never builds a table.
 *
 * The nesting arithmetic makes the fix exact rather than heuristic. Inside bb_i the still-open labels
 * are, innermost first, the block whose `end` precedes bb_{i+1}, then bb_{i+2}'s, ... and finally the
 * dispatch loop at depth N-1-i. So:
 *   j == i+1  -> fall out of the bb; the `end` at the top of the next iteration IS the target
 *   j >  i    -> br (j - i - 1), a direct forward branch, no $blk write, no re-dispatch
 *   j <= i    -> unchanged: set $blk and br to the loop, the only case that needs the br_table
 * With a sane bb order that leaves the dispatch running once per call (the entry) plus real back edges.
 */
#define GOTO(TBB, EXTRA) do { \
		int _ti = bbidx [(TBB)->block_num]; \
		if (_ti < 0) { fail = "bad branch target"; goto done; } \
		if (structured_cfg) { \
			int _d = wj_structured_branch_depth (structured_cfg_kind, structured_loop_h, \
				structured_loop_l, N, i, _ti); \
			if (_d < 0) { fail = "structured cfg edge escaped verifier"; goto done; } \
			/* A direct adjacent forward edge is lexical fallthrough. A self/back edge at depth zero \
			 * still needs a br to the loop label. */ \
			if (_d + (EXTRA) != 0 || _ti <= i) { wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) (_d + (EXTRA))); } \
		} else if (legacy_skip_dispatch) { \
			fail = "branch in a single-bb method"; goto done; \
		} else if (_ti > i) { \
			int _d = _ti - i - 1 + (EXTRA); \
			/* depth 0 at bb top level is the fallthrough edge: emitting nothing gets there. */ \
			if (_d != 0) { wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) _d); } \
		} else { \
			wasm_i32_const (&body, _ti); \
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx); \
			wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) (N - 1 - i + (EXTRA))); \
		} \
	} while (0)

/* Branch to the shared raise site for EXC_ID, consuming a "should throw" i32 already on the stack.
 * EXTRA is the same in-bb nesting count GOTO takes. WJ_HAS_THROW guards the (never expected) case where
 * the prescan did not reserve a block for this id, so the caller can fall back to the inline form. */
#define WJ_HAS_THROW(EXC_ID) ((EXC_ID) >= 0 && (EXC_ID) < WJ_EXC_IDS && wj_throw_slot [(EXC_ID)] >= 0)
#define WJ_THROW_BR(EXC_ID, EXTRA) do { \
		int _od = structured_cfg ? wj_structured_outer_depth (structured_cfg_kind, structured_loop_h, \
			structured_loop_l, N, i) : ((N - 1 - i) + (skip_dispatch ? 0 : 1)); \
		if (_od < 0) { fail = "structured throw depth"; goto done; } \
		wasm_op (&body, WASM_OP_BR_IF); \
		wasm_uleb (&body, (guint32) (_od + wj_throw_slot [(EXC_ID)] + (EXTRA))); \
	} while (0)

/* TRUE when the target has a lexical label at this point, allowing a direct br_if. The verified
 * prefix -> loop-header edge is deliberately fallthrough-only because the loop label is not open yet. */
#define GOTO_HAS_LABEL(TBB) (structured_cfg ? \
	wj_structured_target_has_label (structured_cfg_kind, structured_loop_h, structured_loop_l, i, \
		bbidx [(TBB)->block_num]) : (bbidx [(TBB)->block_num] > i))

/*
 * Two-way branch, with the comparison result already on the wasm stack.
 *
 * When the true target is forward this becomes `br_if <depth>` followed by the false edge -- so the
 * common shape (both targets forward, false target adjacent) collapses to a single conditional branch
 * and a fallthrough, which is what RyuJIT emits for the same IR. The if/else form is kept only for a
 * backward true target, where $blk has to be written before branching and there is nowhere to put that
 * inside a br_if.
 */
#define COND_BRANCH() do { \
		if (GOTO_HAS_LABEL (ins->inst_true_bb)) { \
			int _ct = bbidx [ins->inst_true_bb->block_num]; \
			int _cd = structured_cfg ? wj_structured_branch_depth (structured_cfg_kind, structured_loop_h, \
				structured_loop_l, N, i, _ct) : (_ct - i - 1); \
			if (_cd < 0) { fail = "structured conditional edge escaped verifier"; goto done; } \
			wasm_op (&body, WASM_OP_BR_IF); \
			wasm_uleb (&body, (guint32) _cd); \
			GOTO (ins->inst_false_bb, 0); \
		} else { \
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
			GOTO (ins->inst_true_bb, 1); \
			wasm_op (&body, WASM_OP_ELSE); \
			GOTO (ins->inst_false_bb, 1); \
			wasm_op (&body, WASM_OP_END); \
		} \
	} while (0)

#ifdef HOST_BROWSER
	/* Islands (residual=0): enumerate the full un-JITted-callee blocker set up front so the auto-JIT trigger
	 * forms the whole island in ONE emit cycle (see wj_prescan_blockers). No-op under residual!=0. */
	{ extern int mono_wasm_jit_residual_mode; if (mono_wasm_jit_residual_mode == 0) wj_prescan_blockers (cfg); }
#endif
	i = 0;
	int sl_ord = 0;   /* running ins ordinal, mirroring the SLOTLIVE walk exactly (same bb/ins iteration) */
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

		/* Seed this bb with what its dominators already proved (never-written vregs only); everything
		 * else starts unknown and is rebuilt bb-locally below. */
		if (nn) {
			if (nn_in && i < N && nn_in [i]) memcpy (nn, nn_in [i], (gsize) nvreg);
			else memset (nn, 0, (gsize) nvreg);
		}

		/* LCSE: inherit the load table only across a single-predecessor edge from an already-emitted
		 * block (see the allocation comment). Anything else starts empty. */
		if (lcse) {
			lcse->nld = lcse->nal = 0;
			if (lcse_exit && bb->in_count == 1 && bb->in_bb [0] && !(bb->flags & BB_EXCEPTION_HANDLER)) {
				int pi = (bb->in_bb [0]->block_num <= (int) cfg->max_block_num) ? bbidx [bb->in_bb [0]->block_num] : -1;
				if (pi >= 0 && pi < i && lcse_exit [pi]) {
					*lcse = *lcse_exit [pi];
					if (lcse->nld) lcse_inherits++;
				}
			}
		}

		if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && i == structured_loop_h) {
			int body_n = structured_loop_l - structured_loop_h + 1;
			if (structured_loop_h > 0)
				wasm_op (&body, WASM_OP_END); /* branch target for the loop entry */
			wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40); /* natural-loop header */
			for (int _bi = 0; _bi < body_n; ++_bi) {
				wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);
			}
		}
		if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && i == structured_loop_l + 1)
			wasm_op (&body, WASM_OP_END); /* close natural loop before suffix */
		if (!skip_dispatch)
			wasm_op (&body, WASM_OP_END); /* close dispatcher block B_i; bb_i code follows */
		else if (structured_cfg_kind == WJ_CFG_FORWARD) {
			if (N > 1)
				wasm_op (&body, WASM_OP_END); /* close lexical forward label for bb_i */
		} else if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP) {
			/* Prefix, loop body and suffix each opened exactly one label per member. */
			wasm_op (&body, WASM_OP_END);
		}

#ifdef HOST_BROWSER
		/* AOT-style in-method EH (milestone 2b): record this bb's IL offset into the active island il_state
		 * (pushed at the interp->JIT boundary) so mono's exception pass-1 matches THIS method's enclosing
		 * try and finds its catch as the NEAREST handler for a throwing AOT callee — stopping the walk here
		 * and running no outer frames' finally clauses. (i32)->void via eh_type_idx. EH methods only. */
		if (eh_on) {
			extern void mono_wasm_jit_set_il_offset (int il_offset);
			/* $blk must still identify the CURRENT bb: the catch landing pad passes it to
			 * mono_wasm_jit_eh_dispatch to find the handler for the bb that threw. Direct forward
			 * branches no longer write it on every edge, so write it once per bb instead -- still far
			 * cheaper than the per-edge br_table re-dispatch it replaces. */
			wasm_i32_const (&body, i);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
			{
				extern int mono_wasm_jit_inline_ilofs;
				extern int mono_wasm_jit_il_state_offset_off (void);
				if (ilstate_idx >= 0 && mono_wasm_jit_inline_ilofs) {
					/* il_state->il_offset = <offset>, three ops, no call. The pointer is non-NULL for the whole
					 * body: the prologue above pushed the island and took its return value, and leave_island
					 * only runs on the way out. */
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) ilstate_idx);
					wasm_i32_const (&body, eh_table->il_offsets [i]);
					wasm_op (&body, WASM_OP_I32_STORE);
					wasm_memarg (&body, 2, (guint32) mono_wasm_jit_il_state_offset_off ());
				} else {
					wasm_i32_const (&body, eh_table->il_offsets [i]);
					wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_set_il_offset, eh_type_idx);
				}
			}
		}
#endif

		MONO_BB_FOR_EACH_INS (bb, ins) {
			int my_ord = sl_ord++;   /* bump for EVERY ins (incl. terminated-skipped) to stay aligned with the SLOTLIVE walk */
			if (terminated) continue;   /* skip instrs after a bb terminator (e.g. the OP_BR following OP_CALL_HANDLER) */

			/* NCE kill point. Every value-producing store in this emitter goes through
			 * wasm_st(ins->dreg), so killing dreg here covers every write to a tracked vreg (the lone
			 * exception, OP_SETRET's cfg->ret->dreg, kills itself in its own case). Done BEFORE the
			 * instruction is lowered, since the null-check sites only ever consult SOURCE vregs.
			 * An OP_MOVE carries the fact across, which is what keeps a receiver proven after the
			 * copies mini's call-arg setup inserts -- captured before the kill so dreg == sreg1 is safe. */
			if (G_UNLIKELY (nn != NULL)) {
				gboolean nn_move = (ins->opcode == OP_MOVE) && NN_GET (ins->sreg1);
				NN_KILL (ins->dreg);
				if (nn_move) NN_SET (ins->dreg);
			}
			/* LCSE kill point, same placement and for the same reason as NCE's: the instruction's own
			 * sources must still resolve against the pre-def state. A copy additionally carries the
			 * equivalence forward, which is what lets a load keyed on the copy hit the original's entry. */
			if (G_UNLIKELY (lcse != NULL)) {
				/* SCALAR moves only. OP_VMOVE/OP_XMOVE copy a value type through memory rather than
				 * moving a wasm local, so treating one as a value equivalence would key later lookups
				 * off the wrong storage. */
				int mv_src = wj_ins_is_move (ins->opcode) ? wj_lcse_canon (lcse, ins->sreg1) : -1;
				if (wj_ins_clobbers_mem (ins))
					lcse->nld = lcse->nal = 0;
				else
					wj_lcse_kill (lcse, li, nvreg, ins->dreg);
				if (mv_src >= 0 && mv_src < nvreg && ins->dreg >= 0 && ins->dreg < nvreg &&
				    mv_src != ins->dreg && lc.vt && lc.vt [mv_src] == lc.vt [ins->dreg] &&
				    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [mv_src] != -1)))
					wj_lcse_add_alias (lcse, ins->dreg, mv_src);
			}
			/* SLOTZERO kill point: the vregs chained at this ordinal had their last use at the PREVIOUS
			 * instruction — zero their (now dead) frame slots so the objects stop pinning. Emitted before
			 * this instruction's own code; stack-neutral, and under REF_WT (required) every read comes
			 * from the wasm local, so a zeroed slot is never read. */
			if (G_UNLIKELY (sl_kill_head != NULL)) {
				int kv = sl_kill_head [my_ord];
				if (lazy_ref_frame && kv >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				}
				while (kv >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					wasm_i32_const (&body, 0);
					wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [kv] * 4));
					/* The pin is gone, so the value must not be reused past this point: LCSE's reuse
					 * read is created here at emission and the liveness that decided this slot was dead
					 * never saw it. Killing the entry keeps the two in agreement. */
					if (G_UNLIKELY (lcse != NULL))
						wj_lcse_kill (lcse, li, nvreg, kv);
					kv = sl_kill_next [kv];
				}
				if (lazy_ref_frame && sl_kill_head [my_ord] >= 0)
					wasm_op (&body, WASM_OP_END);
			}
#ifdef HOST_BROWSER
			if (wj_ins_is_effective_gcpoint (cfg, ins) &&
			    !(lazy_ref_frame && ins->opcode == OP_GC_SAFE_POINT) &&
			    !(lazy_ref_frame && terminal_vcall_handoff && ins == terminal_vcall_ins))
				ENSURE_REF_FRAME ();
#endif
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
				GOTO (ins->inst_target_bb, 0);
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
				{ extern void mono_wasm_jit_endfinally_rethrow (void); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_endfinally_rethrow, eh_endcatch_ti); }
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff5, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
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
				 * GC ref shadow stack. Release the dispatch-owned strong handle only AFTER that store,
				 * closing the raw-pointer GC window between native dispatch and handler entry. */
				WasmFuncType _gt; int _gti = -1, _gk;
				memset (&_gt, 0, sizeof _gt); _gt.nparams = 0; _gt.ret = WASM_I32;
				for (_gk = 0; _gk < nextra; ++_gk) if (functype_eq (&extra_types [_gk], &_gt)) { _gti = ti_base + _gk; break; }
				if (_gti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _gt; _gti = ti_base + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_get_caught_exc, _gti);
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff4, (guint32) _gti);
#endif
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "get_ex_obj dreg"; goto done; }
#ifdef HOST_BROWSER
				wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_release_caught_exc, eh_endcatch_ti);
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff6, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
				break;
			}
			case OP_MEMORY_BARRIER:
#ifndef DISABLE_THREADS
				/* Only a SEQ barrier gets a fence, as mini-amd64.c does.
				 *
				 * This used to fence unconditionally ("match the jiterpreter"), but
				 * MINT_MONO_MEMORY_BARRIER is the lowering of the SEQ case only; copying it for ACQ/REL
				 * was the mistake. V8 lowers atomic.fence to a real `mfence` on x64, and it cost 4.24%
				 * of total profiled time on jbox2d -- the largest single unclassified instruction in the
				 * profile, ahead of `ret`, and 54% of the stelemref wrapper by itself. EVERY implicit
				 * barrier the front end emits is ACQ or REL (reference stores under !weak_memory_model:
				 * memory-access.c:522, method-to-ir.c:2373 and friends); SEQ comes only from explicit
				 * Interlocked / Thread.MemoryBarrier / Volatile intrinsics.
				 *
				 * Why dropping ACQ/REL is sound, in order of how load-bearing it is:
				 *
				 * 1. This emitter has no OP_ATOMIC_LOAD / OP_ATOMIC_STORE cases at all -- the only wasm
				 *    atomics it emits are for its own inline caches. So a method containing a managed
				 *    volatile or Interlocked access bails and runs in the interpreter or AOT'd code.
				 *    Every OP_MEMORY_BARRIER we actually emit therefore orders ORDINARY, non-atomic
				 *    accesses. Per the wasm relaxed-memory model that is a data race with or without
				 *    the fence: atomic.fence does not make an ordinary store a synchronising one, so it
				 *    never conferred the ordering it looked like it did.
				 * 2. Where synchronisation does go through wasm atomics, all wasm atomics are currently
				 *    seq-cst, so the ordering is carried by the atomic access itself -- same-thread
				 *    accesses are happens-before ordered and an atomic read-from establishes cross-thread
				 *    happens-before. A separate ACQ/REL fence cannot strengthen that. Binaryen encodes
				 *    exactly this: visitAtomicFence lowers atomic.fence to nothing while only seq-cst
				 *    atomics exist.
				 * 3. Corroboration, not justification: the 210 MB AOT'd dotnet.native.wasm contains ZERO
				 *    atomic.fence. mini-llvm.c does pass the kind to mono_llvm_build_fence and LLVM does
				 *    emit a fence for every ordering -- wasm-opt then strips them all, for reason (2).
				 *
				 * SEQ keeps a real fence deliberately: .NET permits low-level patterns that expect
				 * Thread.MemoryBarrier to be a physical full barrier, and those sites are rare enough
				 * that the mfence costs nothing measurable. */
				if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ) {
					wasm_u8 (&body, 0xfe);  /* atomic prefix */
					wasm_u8 (&body, 0x03);  /* atomic.fence */
					wasm_u8 (&body, 0x00);  /* memory order: sequentially consistent */
				}
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
					/* Baking inst_p0 enables newobj/token-constant methods to JIT. Only provably-stable,
					 * un-movable, cross-thread pointers reach here (vtable/class/method/static-field-addr/
					 * image/method-rgctx); movable GC objects (ldstr/typeof) go through the precise-root
					 * literal table instead, so a GC can't dangle these immediates. */
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
					for (pk = 0; pk < nextra; ++pk) if (functype_eq (&extra_types [pk], &pt)) { pti = ti_base + pk; break; }
					if (pti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt; pti = ti_base + nextra++; }
					uses_calls = TRUE;
					wasm_i32_const (&body, (gint32) (intptr_t) &mono_polling_required);
					wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);              /* if (flag != 0) */
					/* Materialize roots inside the rare taken arm, not before the flag load. */
					ENSURE_REF_FRAME ();
					/* Direct import: a C function address in dotnet.native.wasm, fixed for the process. */
					wj_emit_helper_call (&body, (gpointer) &mono_threads_state_poll, (guint32) pti);
					RELEASE_LAZY_REF_FRAME ();
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
				if (vv >= 0 && lc.addrslot && lc.addrslot [vv] == -3) {
					/* by-addr vtype ARG: the incoming i32 param already holds the address of the
					 * caller-owned copy — re-push it (wasm_ld takes the li/param path: addrslot<0,
					 * never a refslot since the pointer is non-heap). */
					if (!wasm_ld (&body, &lc, vv) || !wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr byaddr arg"; fail_op = OP_LDADDR; goto done; }
					break;
				}
				if (vv >= 0 && lc.addrslot && lc.addrslot [vv] == -2) {
					/* ref-etype scalar vtype: address is its GC-scanned ref-shadow slot (refbase + slot*4). */
					if (!lc.refslot || lc.refslot [vv] < 0) { fail = "ldaddr refvt no slot"; fail_op = OP_LDADDR; goto done; }
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					if (lc.refslot [vv] != 0) { wasm_i32_const (&body, lc.refslot [vv] * 4); wasm_op (&body, WASM_OP_I32_ADD); }
					if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
					NN_SET (ins->dreg);   /* a frame-slot address is never null */
					break;
				}
				if (vv < 0 || !lc.addrslot || lc.addrslot [vv] < 0) { fail = "ldaddr unsupported var"; fail_op = OP_LDADDR; goto done; }
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) addrbase_idx);
				if (lc.addrslot [vv] != 0) { wasm_i32_const (&body, lc.addrslot [vv]); wasm_op (&body, WASM_OP_I32_ADD); }
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
				NN_SET (ins->dreg);   /* a frame-slot address is never null */
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
				gboolean lsigned = (ins->opcode == OP_LDIV || ins->opcode == OP_LREM);
				WasmOpcode dop = ins->opcode == OP_LDIV ? WASM_OP_I64_DIV_S
					: ins->opcode == OP_LDIV_UN ? WASM_OP_I64_DIV_U
					: ins->opcode == OP_LREM ? WASM_OP_I64_REM_S : WASM_OP_I64_REM_U;
				WasmFuncType rt; int ldiv_rti = -1, rk;
				/* (i32)->void functype for mono_wasm_jit_raise_corlib */
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_I32; rt.nparams = 1; rt.ret = WASM_VOID;
				for (rk = 0; rk < nextra; ++rk) if (functype_eq (&extra_types [rk], &rt)) { ldiv_rti = ti_base + rk; break; }
				if (ldiv_rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; ldiv_rti = ti_base + nextra++; }
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

			/*
			 * FLOAT -> INT. Previously ENTIRELY unsupported: the emitter had every int->float and
			 * float->float conversion but no float->int at all, so any `(int)someFloat` bailed the whole
			 * method. That shape is pervasive in jbox2d (MathUtils.floor/round) and it was the root bail
			 * that cascaded into ~29 lost methods once the inline limit was raised past 20.
			 *
			 * Always the SATURATING (0xFC-prefixed) forms. The plain truncations TRAP on NaN or
			 * out-of-range, which would turn an ECMA-"unspecified" conversion into a hard abort. Saturating
			 * clamps to min/max with NaN->0 -- precisely Java's (int)float rule, and the workload is
			 * IKVM-translated Java. It also matches the interpreter, whose MINT_CONV_I4_R4 is a plain C
			 * `(gint32) float` cast that clang lowers to trunc_sat on wasm, so a method cannot change
			 * behaviour when it tiers from interp to JIT.
			 *
			 * Source precision comes from the SOURCE VREG's valtype, not the opcode's R/F prefix: with
			 * MONO_OPT_FLOAT32 off an R4 value lives in an f64 vreg, and assuming f32 would emit a
			 * type-invalid module -- the same reason OP_RCONV_TO_R4 above checks vt[] before demoting.
			 */
			#define WJ_SRC_IS_F32 (!(ins->sreg1 >= 0 && ins->sreg1 < nvreg && vt [ins->sreg1] == WASM_F64))
			#define SAT_CONV(SAT32, SAT64, NARROW) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "fconv sreg"; goto done; } \
				wasm_op_sat (&body, _f32 ? (SAT32) : (SAT64)); \
				NARROW; \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fconv dreg"; goto done; } \
			} while (0)
			#define SAT_NONE   do { } while (0)
			#define SAT_EXT8   wasm_op (&body, WASM_OP_I32_EXTEND8_S)
			#define SAT_EXT16  wasm_op (&body, WASM_OP_I32_EXTEND16_S)
			#define SAT_MASK8  do { wasm_i32_const (&body, 0xFF); wasm_op (&body, WASM_OP_I32_AND); } while (0)
			#define SAT_MASK16 do { wasm_i32_const (&body, 0xFFFF); wasm_op (&body, WASM_OP_I32_AND); } while (0)
			/* -> i32 (native int is i32 on wasm32, so _TO_I folds in here) */
			case OP_RCONV_TO_I4: case OP_FCONV_TO_I4:
			case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_NONE); break;
			case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_U, WASM_SAT_I32_TRUNC_F64_U, SAT_NONE); break;
			/* -> i64 */
			case OP_RCONV_TO_I8: case OP_FCONV_TO_I8:
				SAT_CONV (WASM_SAT_I64_TRUNC_F32_S, WASM_SAT_I64_TRUNC_F64_S, SAT_NONE); break;
			case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:
				SAT_CONV (WASM_SAT_I64_TRUNC_F32_U, WASM_SAT_I64_TRUNC_F64_U, SAT_NONE); break;
			/* -> sub-word: truncate to i32 SIGNED then narrow, mirroring mono's native lowering
			 * (cvttsd2si + movsx/movzx) rather than converting straight to the narrow type. */
			case OP_RCONV_TO_I1: case OP_FCONV_TO_I1:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_EXT8); break;
			case OP_RCONV_TO_I2: case OP_FCONV_TO_I2:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_EXT16); break;
			case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_MASK8); break;
			case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_MASK16); break;
			/* NB: the OP_*CONV_TO_OVF_* family is deliberately NOT here. Those must raise
			 * OverflowException on out-of-range, which needs a range test plus a raise -- saturating
			 * them would silently return a wrong value instead of throwing. They keep bailing. */

			/*
			 * Float math intrinsics. Each is ONE wasm instruction and every one was previously an
			 * unsupported-opcode bail. wasm's semantics match the managed ones: min/max propagate NaN and
			 * order -0 below +0, and f*.nearest is round-half-to-EVEN, which is Math.Round(double)'s
			 * default. Precision from the source vreg, as above.
			 */
			#define FUN1(W32, W64) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "fmath sreg"; goto done; } \
				wasm_op (&body, _f32 ? (W32) : (W64)); \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fmath dreg"; goto done; } \
			} while (0)
			#define FBIN(W32, W64) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "fmath sreg"; goto done; } \
				wasm_op (&body, _f32 ? (W32) : (W64)); \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fmath dreg"; goto done; } \
			} while (0)
			case OP_ABS: case OP_ABSF:     FUN1 (WASM_OP_F32_ABS,     WASM_OP_F64_ABS);     break;
			case OP_SQRT: case OP_SQRTF:   FUN1 (WASM_OP_F32_SQRT,    WASM_OP_F64_SQRT);    break;
			case OP_CEIL: case OP_CEILF:   FUN1 (WASM_OP_F32_CEIL,    WASM_OP_F64_CEIL);    break;
			case OP_FLOOR: case OP_FLOORF: FUN1 (WASM_OP_F32_FLOOR,   WASM_OP_F64_FLOOR);   break;
			case OP_TRUNC: case OP_TRUNCF: FUN1 (WASM_OP_F32_TRUNC,   WASM_OP_F64_TRUNC);   break;
			case OP_ROUND:                 FUN1 (WASM_OP_F32_NEAREST, WASM_OP_F64_NEAREST); break;
			case OP_FMIN: case OP_RMIN:    FBIN (WASM_OP_F32_MIN, WASM_OP_F64_MIN); break;
			case OP_FMAX: case OP_RMAX:    FBIN (WASM_OP_F32_MAX, WASM_OP_F64_MAX); break;
			case OP_FCOPYSIGN: case OP_RCOPYSIGN:
				FBIN (WASM_OP_F32_COPYSIGN, WASM_OP_F64_COPYSIGN); break;

			/* Integer bit intrinsics (BitOperations.LeadingZeroCount / PopCount). The 64-bit forms are
			 * LREG->LREG in mono and i64.clz/i64.popcnt yield i64, so no wrap is needed. */
			case OP_LZCNT32:  UN (WASM_OP_I32_CLZ);    break;
			case OP_LZCNT64:  UN (WASM_OP_I64_CLZ);    break;
			case OP_POPCNT32: UN (WASM_OP_I32_POPCNT); break;
			case OP_POPCNT64: UN (WASM_OP_I64_POPCNT); break;
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
			case OP_NOT_NULL:
				/* Metadata only, NOT a check. The front end emits this to ASSERT that sreg1 is already
				 * known non-null -- right after an allocation, or right after the check it just emitted.
				 * mini-ops.h declares it (NONE, IREG, NONE), so it has no result, and it is a no-op in
				 * every other backend: amd64/x86/arm/arm64/ppc/s390x/riscv all `break`, and mini-llvm.c
				 * groups it with OP_NOP/OP_LIVERANGE_START.
				 *
				 * Sharing OP_CHECK_THIS's case below inverted its meaning -- it made the one opcode that
				 * certifies a check is unnecessary emit a load+eqz+branch. That was live cost, not a
				 * hypothetical: two sites emit OP_NOT_NULL UNGATED by COMPILE_LLVM, after handle_alloc
				 * for newobj (method-to-ir.c:9649) and after ldlen (method-to-ir.c:10867), so every
				 * `new` and every `arraylength` in JIT'd code carried a null check on a value the front
				 * end had just certified non-null. jbox2d is dense in both.
				 *
				 * Record the fact so NCE can drop LATER real checks on the same vreg, and emit nothing. */
				NN_SET (ins->sreg1);
				break;
			case OP_CHECK_THIS: {
				/* Null check: if sreg1 is null, raise a CATCHABLE NullReferenceException, mirroring OP_COND_EXC
				 * — NOT a raw wasm trap. address 0 is valid in wasm linear memory, so a skipped check silently
				 * corrupts low memory; and EH methods now compile, so a null this/deref inside
				 * a try must reach managed EH/finally rather than `unreachable`. Hot path = ld + eqz + not-taken
				 * branch; cold path raises NRE (exc_id 4) then C++-unwinds (cppeh) or bails to interp resume-state. */
				if (NN_GET (ins->sreg1)) break;   /* NCE: already proven non-null earlier in this bb */
				NN_SET (ins->sreg1);              /* past this point it cannot be null */
				if (!WJ_HAS_THROW (4)) { fail = "no shared throw block for NRE"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "nullchk sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_EQZ);
				WJ_THROW_BR (4, 0);   /* one shared raise site per method, not a call per check */
				break;
			}
/* membase load: dreg = *(sreg1 + inst_offset).
 *
 * Under LCSE, if this exact (base, offset, opcode) was already loaded earlier in the extended block and
 * nothing has clobbered memory since, replace the memory load with a LOCAL COPY of the vreg that already
 * holds the value. The i32.load/f32.load is gone, which is the point; TurboFan then SSA-renames the copy
 * away for free.
 *
 * It is tempting to emit NOTHING and just record dreg as an alias of the cached vreg, resolving every
 * later read through the table. That MISCOMPILES, and the failure is not subtle once seen:
 *
 *     t7  = load(a,12)     -> entry (a,12) -> t7
 *     t15 = load(a,12)     -> hit; emit nothing, alias t15 -> t7
 *     t7  = <redefined>    -> the alias must die (t7 no longer holds the value)
 *     use t15              -> reads t15's own local, WHICH WAS NEVER WRITTEN -> zero
 *
 * jbox2d's min/max ternaries reuse vregs constantly, so this produced garbage floats, NaNs, and a
 * non-terminating solver loop rather than a crash. Emitting the copy makes dreg a real def with its
 * normal wasm_st behaviour -- which also keeps the ref frame honest, since a slotted ref dreg still
 * writes its pin slot exactly where SLOTLIVE/SLOTZERO's liveness walk expects it.
 *
 * The ALIAS entry is still recorded, but purely to canonicalise TABLE KEYS: it lets a chained load
 * (`a.lowerBound.x`, whose base is this load's dreg) find the original base's entry. If it dies, the
 * only cost is a missed elision. */
/* An UNPINNED REFERENCE is never cached or reused. lcse_nopin[v] marks an isref vreg whose frame slot
 * MONO_WASM_JIT_SLOTLIVE elided because its IR def->use range crossed no GC point. LCSE's reuse read is
 * created at emission and is invisible to that analysis, so it can extend the range past a GC point --
 * and the two passes disagree about which instructions those are (LCSE deliberately treats a raise as
 * non-clobbering, since null checks are the commonest thing between a load and its reload, while the
 * frame model counts a raise as a GC point unless RAISE_NOGC is set on a clause-free method). Refusing
 * to cache unpinned refs removes the disagreement outright and costs almost nothing: the loads this
 * pass exists for are the float and int field reads in jbox2d's inner loops, none of which are refs. */
#define WJ_LCSE_UNPINNED(v)  (lcse_nopin && (v) >= 0 && (v) < nvreg && lcse_nopin [(v)])
#define LOADM(WOP, AL) do { \
		int _cb = G_UNLIKELY (lcse != NULL) ? wj_lcse_canon (lcse, ins->sreg1) : -1; \
		int _hit = (_cb >= 0) ? wj_lcse_find (lcse, (WOP), (AL), _cb, (gint32) ins->inst_offset) : -1; \
		if (G_UNLIKELY (lcse != NULL)) wj_count (WJC_LCSE_LOADS_SEEN); \
		if (_hit >= 0 && _hit < nvreg && ins->dreg >= 0 && ins->dreg < nvreg && _hit != ins->dreg && \
		    !WJ_LCSE_UNPINNED (_hit) && \
		    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [_hit] != -1))) { \
			if (!wasm_ld (&body, &lc, _hit)) { fail = "lcse src"; goto done; } \
			if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lcse dreg"; goto done; } \
			wj_lcse_add_alias (lcse, ins->dreg, _hit); \
			lcse_hits++; wj_count (WJC_LCSE_HITS); \
			break; \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "load addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "load base"; goto done; } \
		wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); \
		if (!wasm_st (&body, &lc, ins->dreg)) { fail = "load dreg"; goto done; } \
		if (_cb >= 0 && ins->dreg >= 0 && ins->dreg < nvreg && !WJ_LCSE_UNPINNED (ins->dreg) && \
		    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [_cb] != -1))) { \
			if (lcse->nld >= WJ_LCSE_LOADS) wj_count (WJC_LCSE_EVICT); \
			wj_lcse_add_load (lcse, (WOP), (AL), _cb, (gint32) ins->inst_offset, ins->dreg); \
			lcse_adds++; wj_count (WJC_LCSE_ADDS); \
		} \
	} while (0)
			case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE: LOADM (WASM_OP_I32_LOAD, 2); break;
			case OP_LOADU1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_U, 0); break;
			case OP_LOADI1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_S, 0); break;
			case OP_LOADU2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_U, 1); break;
			case OP_LOADI2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_S, 1); break;
			case OP_LOADI8_MEMBASE: LOADM (WASM_OP_I64_LOAD, 3); break;
			case OP_LOADR8_MEMBASE: LOADM (WASM_OP_F64_LOAD, 3); break;
			case OP_LOADR4_MEMBASE: LOADM (WASM_OP_F32_LOAD, 2); break;
#undef LOADM
#undef WJ_LCSE_UNPINNED
/* membase store: *(dreg[=inst_destbasereg] + inst_offset) = sreg1. (A reference value still needs its
 * GC write barrier, which mono emits as a separate IR call before the store — so the store is raw.) */
#define STOREM(WOP, AL) do { \
		gboolean _og_valref = lc.refslot && ins->sreg1 >= 0 && ins->sreg1 < nvreg && lc.refslot [ins->sreg1] >= 0; \
		gboolean _og_baseref = lc.refslot && ins->dreg >= 0 && ins->dreg < nvreg && lc.refslot [ins->dreg] >= 0; \
		if (G_UNLIKELY (lc.objguard) && (_og_valref || _og_baseref)) { \
			/* OBJGUARD. Two store shapes can scribble random memory via a bad base: \
			 * (a) ref VALUE into a genuine OBJECT base (dreg is vreg_is_ref and NOT an interior mp): the base is \
			 *     a live heap object whose write barrier card-marks (base>>9)+cardtable; its offset-0 word IS a \
			 *     vtable, so validate it looks like a live heap object (kind 2). \
			 * (b) BYREF / interior-pointer base (dreg is vreg_is_mp, a byref arg, an ldflda/ldaddr temp, or any \
			 *     vreg not proven to be an object): its offset-0 word is a struct FIELD, not a vtable. A ref value \
			 *     is routinely stored THROUGH such a base into a value type — e.g. a struct-returning method writing \
			 *     ref fields into its hidden vret buffer (ReadOnlySequence<T>.SliceImpl storing SequencePosition. \
			 *     _object into *retbuf), or `*outparam = x`. kind 2 would false-trap on a stale/small first word \
			 *     (seen live: obj first word 0x1e0), so just range-check the base (kind 3). \
			 * The kind is chosen by the BASE's object-ness, NOT by whether the VALUE is a ref: refslot/isref merge \
			 * object refs and byrefs into one bucket, so consult vreg_is_ref/vreg_is_mp to separate them here. */ \
			gboolean _og_baseobj = ins->dreg >= 0 && ins->dreg < nvreg && vreg_is_ref (cfg, ins->dreg) && !vreg_is_mp (cfg, ins->dreg); \
			int _ogkind = (_og_valref && _og_baseobj) ? 2 : 3; \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard base"; goto done; } \
			wasm_i32_const (&body, _ogkind); \
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, (guint32) lc.check_ti); \
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
			 * byref can land on the C-stack pointer. Range/control-var check the base (kind 3) before the store. */ \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard imm base"; goto done; } \
			wasm_i32_const (&body, 3); \
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, (guint32) lc.check_ti); \
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
					/* The one write in this emitter that is not ins->dreg, so the generic NCE and LCSE
					 * kills at the top of the loop do not cover it. Both have to be told by hand. */
					NN_KILL (cfg->ret->dreg);
					if (G_UNLIKELY (lcse != NULL))
						wj_lcse_kill (lcse, li, nvreg, cfg->ret->dreg);
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
				COND_BRANCH ();
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
				COND_BRANCH ();
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
				int exc_id = -1;
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
				exc_id = wj_exc_id_for_name (en);
				if (exc_id < 0) { fail = en ? "cond_exc exc name" : "cond_exc no name"; goto done; }
				/* NCE. Only the explicit-null-check SHAPE qualifies: an NRE raised when the fused compare
				 * is `<vreg> == 0` against a 32-bit immediate zero. That is exactly what
				 * MONO_EMIT_NULL_CHECK expands to (ir-emit.h: OP_COMPARE_IMM imm=0 + OP_COND_EXC_EQ).
				 * Every other cond_exc -- bounds checks, overflow, divide-by-zero -- is left alone, and so
				 * is an NRE raised by any other comparison, because "this vreg is not null" says nothing
				 * about those conditions. */
				if (exc_id == 4 && cmp_imm_mode && !cmp_i64 && cmp_imm == 0 &&
				    (ins->opcode == OP_COND_EXC_EQ || ins->opcode == OP_COND_EXC_IEQ)) {
					if (NN_GET (cmp_a)) break;
					NN_SET (cmp_a);
				}
				if (!WJ_HAS_THROW (exc_id)) { fail = "no shared throw block for exc"; goto done; }
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cond_exc a"; goto done; }
				if (cmp_imm_mode) { if (cmp_i64) wasm_i64_const (&body, cmp_imm64); else wasm_i32_const (&body, cmp_imm); }
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "cond_exc b"; goto done; }
				wasm_op (&body, cmp_wop);
				/* The compare result IS the "should throw" condition, so it feeds br_if directly: no `if`
				 * block and no per-site call. The hot path is the not-taken branch. */
				WJ_THROW_BR (exc_id, 0);
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
				/* mono_wasm_jit_throw captures a stack trace, i.e. it allocates. The thrown object
				 * itself lives in a wasm local until this point, so a lazy frame has to exist before
				 * the call -- same reason as the shared raise blocks below. (RETHROW implies eh_on,
				 * where lazy frames are off and this expands to nothing.) */
				RAISE_ENSURE_REF_FRAME ();
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "throw sreg"; goto done; }
				memset (&tt, 0, sizeof (tt)); tt.params [0] = WASM_I32; tt.nparams = 1; tt.ret = WASM_VOID;
				for (tk = 0; tk < nextra; ++tk) if (functype_eq (&extra_types [tk], &tt)) { tti = ti_base + tk; break; }
				if (tti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tt; tti = ti_base + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				/* OP_RETHROW preserves the original stack trace (mono_wasm_jit_rethrow); OP_THROW rebuilds it. */
				if (ins->opcode == OP_RETHROW) {
					extern void mono_wasm_jit_rethrow (MonoObject *exc); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_rethrow, tti);
				} else {
					extern void mono_wasm_jit_throw (MonoObject *exc); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_throw, tti);
				}
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff7, (guint32) tti);
#endif
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
				COND_BRANCH ();
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
				GOTO (ins->inst_target_bb, 0);
				terminated = TRUE;
				break;
			case OP_SWITCH: {
				/* jump table: dispatch sreg1 (the index, already bounds-checked to [0,N) by a preceding
				 * branch) to inst_many_bb[index]. N = GPOINTER_TO_UINT(ins->klass). Emit an if-chain of
				 * (index==i -> GOTO target_i) for i in [0,N-1), then an unconditional GOTO target_{N-1}
				 * (the in-range fall-through). Each per-case GOTO is inside one `if` (nesting 1); the
				 * final one is at bb top level (nesting 0), matching OP_BR. */
				guint nsw = GPOINTER_TO_UINT (ins->klass), si;
				if (nsw == 0) { fail = "switch no targets"; goto done; }
				for (si = 0; si + 1 < nsw; ++si) {
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "switch idx"; goto done; }
					wasm_i32_const (&body, (gint32) si);
					wasm_op (&body, WASM_OP_I32_EQ);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					GOTO (ins->inst_many_bb [si], 1);
					wasm_op (&body, WASM_OP_END);
				}
				GOTO (ins->inst_many_bb [nsw - 1], 0);
				terminated = TRUE;
				break;
			}
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2: {
				/* Lower a direct managed call to call_indirect through the callee's f-slot.
				 * Callee args are call->args[0..nparams) (this first, if any); the result goes
				 * to ins->dreg — except OP_VCALL2 (vtype return): dreg is -1 and the callee writes
				 * the result through the trailing hidden-vret pointer (WjCallArgs position n, the decomposed
				 * OUTARG_VTRETADDR address). Bails (whole method -> interp) if the callee isn't
				 * JITted yet. */
				MonoCallInst *call = (MonoCallInst *) ins;
				MonoMethodSignature *csig = call->signature;
				WasmFuncType ct;
				WasmCallInfo cci;
				int call_fslot, type_idx = -1, k, ai;
#ifdef HOST_BROWSER
				gboolean late_fslot_block = FALSE;
#endif
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
					extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
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
						if (pv == 0 || pv == WASM_VOID) {
							fail = "icall arg type";
							fail_sig_site = "icall"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = ai;
							goto done;
						}
						if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "icall nargs"; goto done; }
						ct.params [ct.nparams++] = pv;
					}
					if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
					else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) {
						fail = "icall ret type";
						fail_sig_site = "icall"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = -1;
						goto done;
					} }
					for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
					if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = ti_base + nextra++; }
					uses_calls = TRUE;
					if (!call->call_info) { fail = "no captured icall args"; goto done; }
					{
						int nm = csig->param_count + (csig->hasthis ? 1 : 0);
						for (ai = 0; ai < nm; ++ai)
							if (!wasm_ld (&body, &lc, wj_arg_vreg (call, ai))) { fail = "icall arg ld"; goto done; }
					}
					wj_emit_helper_call_ci (&body, (gpointer) (intptr_t) ifptr, (guint32) type_idx);
					if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "icall dreg"; goto done; }
					break;
				}
				if (!csig) { fail = "null sig call"; goto done; }
				/* Canonical callee functype + per-arg ABI from the one shared descriptor (this + args
				 * [+ trailing vret i32] -> ret). By-addr args and hidden vret are fully lowered here
				 * (Stage 2.3); ct.nparams includes the vret param, cci.nargs does not. */
				mono_wasm_get_call_info (csig, &cci);
				if (!cci.valid) {
					fail = cci.fail_reason;
					fail_sig_site = "call"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = cci.fail_arg;
					goto done;
				}
				ct = cci.ftype;
#ifdef HOST_BROWSER
				if (call_method == cfg->method) {
					extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
					/* SCC batch: the orchestrator reserved our slot on the imethod -> get_callee_fslot returns
					 * it. Otherwise standalone self-recursion needs a slot to bake before the method exists. */
					call_fslot = mono_wasm_jit_get_callee_fslot (call_method);
					if (call_fslot <= 0) {
						if (!wj_self_f_slot) {
							extern int mono_wasm_jit_reserve_self (MonoMethod *m, int *e_out, int *f_out);
							/* Reserve ON THE IMETHOD (wasm_jit_self_resv_*), not into these locals. These are
							 * locals of this function, so when an emit failed the pair was orphaned and the
							 * next attempt allocated a fresh one — and the allocator has no free. Since the
							 * drivers re-emit hard (force_island up to 10 passes, compile_scc phase 1 up to
							 * WJ_SCC_MAX*2), that leak scaled with re-emission count, which is precisely the
							 * axis island/module batching increases. Reserving on the imethod makes a re-emit
							 * reuse the same pair. */
							if (!mono_wasm_jit_reserve_self (cfg->method, &wj_self_e_slot, &wj_self_f_slot)) {
								fail = "jit function table exhausted"; goto done;
							}
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
#ifdef HOST_BROWSER
					wj_result_add_direct_dep (&cfg->wasm_jit_result, call_fslot, wj_functype_hash (&ct), call_method);
					if (cfg->wasm_jit_result.direct_deps_truncated) { fail = "too many direct dependencies"; goto done; }
#endif
					/* Intern the callee functype unconditionally. It used to be skipped when the callee
					 * was a fellow batch member, because that site was hard-coded to `call <memberidx>`
					 * and a direct call names no type -- but the form is no longer decided here, so a
					 * type index has to exist for the call_indirect the assembler may still choose. */
					for (k = 0; k < nextra; ++k)
						if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
					if (type_idx < 0) {
						if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
						extra_types [nextra] = ct;
						type_idx = ti_base + nextra++;
					}
					uses_calls = TRUE;
					/* arg source vregs captured at method-to-ir time (calls.c), not call->args
					 * (which gets corrupted by later vreg passes) */
					if (!call->call_info) { fail = "no captured call args"; goto done; }
					{
						for (ai = 0; ai < cci.nargs; ++ai)
							if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "call arg ld"; goto done; }
						/* hidden vret: the trailing param is the ret-buffer address (the decomposed
						 * OUTARG_VTRETADDR vreg — an addr-frame address the callee writes through) */
						if (cci.vret_byaddr && !wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "call vret ld"; goto done; }
					}
					/* One hole, three possible outcomes, none of them decided here: a module-local
					 * `call <funcidx>` if the callee turns out to be co-located, an imported `call` if it
					 * is not but is safe to bind at instantiation, and `i32.const <fslot>; call_indirect`
					 * otherwise. Which one is chosen -- and re-chosen on every re-framing -- is the
					 * assembler's decision, because it is the only thing that knows the module. */
					wj_emit_method_call (&body, call_method, call_fslot, (guint32) type_idx);
					if (ct.ret != WASM_VOID) {
						if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
							if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "call scalar-vtype ret"; goto done; }
						} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
					}
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
						/* Hidden vret also bails to the residual: the native AOT ABI puts the vret pointer
						 * FIRST (LLVMArgVtypeRetAddr, vret_arg_index==0) while our internal convention is
						 * vret-LAST — reordering here isn't wired up, and the residual is vret-correct.
						 * By-addr ARGS are fine inline: the native ABI passes the same copy address
						 * positionally (ArgValuetypeAddrOnStack). */
						gboolean aot_ok = mono_wasm_jit_inline_aot && !m_type_is_byref (csig->ret)
							&& !call->rgctx_reg && !call->need_unbox_trampoline && !cci.vret_byaddr;
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
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { nti = ti_base + k; break; }
							if (nti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = nt; nti = ti_base + nextra++; }
							uses_calls = TRUE;
							if (!call->call_info) { fail = "no captured call args"; goto done; }
							{
								/* AOT-style EH (cppeh, the only model now): bare direct AOT call. A throwing AOT
								 * callee C++-unwinds (wasm-EH) straight through this JITted frame to the nearest
								 * landing pad — the interp e-thunk boundary (mono_llvm_catch_exception) or an
								 * in-method catch. The call is pure call_indirect (the per-call perf win); no
								 * try/catch, no per-call pending-exception check. */
								wj_emit_fast_count (&body, WJC_FAST_INLINE_AOT);   /* profile: INLINE_AOT direct dispatch */
								{ for (ai = 0; ai < cci.nargs; ++ai) if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "call arg ld"; goto done; } }
								if (aot_has_extra) wasm_i32_const (&body, (gint32) (intptr_t) aot_rgctx);   /* trailing rgctx/dummy — only if the body has it */
								/* A constant table index with a known functype, exactly like a runtime helper -- an AOT
								 * body is just another fixed occupant of the function table. Whether it becomes an
								 * import or stays a call_indirect is MONO_WASM_JIT_AOT_IMPORTS, applied at assembly.
								 * This is the highest-volume import source there is: ~90k inline-AOT calls per frame. */
								wj_emit_aot_call (&body, (gpointer) (intptr_t) aot_addr, (guint32) nti);
								if (ct.ret != WASM_VOID) {
								if (ct.ret == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret);   /* raw AOT body: dirty upper bits */
									if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
										if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "aot scalar-vtype ret"; goto done; }
									} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
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
						/* rgctx calls: if INLINE_AOT couldn't take it (gsharedvt-variable, or
						 * inline-aot off/byref), keep the method JITted by routing this one call through the residual
						 * (mono_wasm_jit_call_interp derives the context from the concrete callee) rather than bailing
						 * the whole method at the RESIDUAL=0 gate. The rgctx call is the cold catch-block edge; the hot
						 * try-body still JITs. */
						if (rm == 0 && !((MonoCallInst*)ins)->rgctx_reg) {
							/* jit->AOT fastpath: an AOT-compiled callee with no f-slot is STILL directly callable
							 * via its native AOT body — so don't bail the whole method. Fall through to the
							 * residual emit, which routes through interp_entry->do_jit_call to the AOT body.
							 * This breaks the RESIDUAL=0 cascade at the
							 * AOT'd java.* leaves (Math.sqrt, base ctors, java.util.*) that nearly every MC
							 * call-tree bottoms out in (94% of all bails). Interp-only callees with no f-slot
							 * still bail, so the island force-JITs them bottom-up as before. */
							extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
							if (!mono_interp_jit_call_supported (call_method, csig)) {
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
					/* BYREF args/ret in the CALLEE signature (MONO_WASM_JIT_BYREF, default on). The residual
					 * routes the call through interp_entry, and both halves of that marshalling are now
					 * correct, so we no longer bail the whole caller (which was permanent, bail -7, and was
					 * the single largest perm-unjittable source in the IKVM soak: 4.27M of 4.28M vperm):
					 *   ARGS: the spill stores the byref VALUE (the pointer) into the scratch slot, and the
					 *     C side derefs exactly those slots (wj_arg_slot_holds_pointer in interp.c, shared by
					 *     mono_wasm_jit_call_interp and wasm_jit_aot_call_lean) to match interp_entry's
					 *     asymmetric convention (byref = the pointer; by-value = a pointer TO the value).
					 *     This was already implemented before the gate — the gate was pure conservatism.
					 *   RET: stackval_to_data_sign_ext now guards byref (interp.c). It previously switched on
					 *     type->type, so `ref sbyte/byte/short/ushort` got the sign/zero-extended LOW BYTE(S)
					 *     OF THE POINTER — reachable via wasm_jit_aot_call_lean, which passes the raw
					 *     sig->ret. (interp_entry's own tail was always safe: rmethod->rtype is normalized
					 *     to MONO_TYPE_I.) That was the real bug this gate was protecting against, and it is
					 *     what makes Unsafe.Add/AddByteOffset safe here.
					 * Nothing is needed on the emit side: byref is WJ_ARG_SCALAR/i32 in cci/ct, and the
					 * sub-word ret normalization below keys off mini_get_underlying_type, which erases byref
					 * to MONO_TYPE_I -> no mask/extend over a pointer. The DIRECT f-slot path above always
					 * handled byref (pure wasm pointer pass, no interp_entry).
					 * =0 restores the bails; the fail strings must keep the substring "byref" and must NOT
					 * contain "arg type"/"ret type" (tested first) so the -7 classification still holds. */
					if (!mono_wasm_jit_byref) {
						if (m_type_is_byref (csig->ret)) {
							fail = "residual byref ret";
							fail_sig_site = "residual"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = -1;
							goto done;
						}
						for (bi = 0; bi < (int) csig->param_count; ++bi)
							if (m_type_is_byref (csig->params [bi])) {
								fail = "residual byref arg";
								fail_sig_site = "residual"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = bi;
								goto done;
							}
					}
					if (cci.nargs * 8 > 192 /*WJ_SCRATCH_RET_OFF*/) { fail = "residual nargs"; goto done; }
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
					/* SELF-HEALING DIRECT RESIDUAL. This caller was emitted before call_method acquired
					 * an f-slot. Bake its stable InterpMethod identity and, on each residual execution,
					 * ask the tiny late-fslot helper to follow tiering + admit the module on this thread.
					 * A live result calls the ordinary f-thunk directly, before pretransform/scratch/
					 * interp_entry. A zero result falls through to the unchanged residual below.
					 *
					 * Exclude native-AOT callees (their slot can never heal and the inline-AOT/residual
					 * AOT routes above are authoritative) and permanent wasm-JIT bails. */
					{
						extern gpointer mono_wasm_jit_get_callee_imethod (MonoMethod *method);
						extern int mono_wasm_jit_late_fslot (gpointer imethod);
						extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
						extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
						gpointer late_im = mono_wasm_jit_get_callee_imethod (call_method);
						gboolean can_heal = late_im && !mono_wasm_jit_callee_perm_unjittable (call_method)
							&& !mono_interp_jit_call_supported (call_method, csig);
						if (can_heal) {
							WasmFuncType ht; int hti = -1;
							for (k = 0; k < nextra; ++k)
								if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
							if (type_idx < 0) {
								if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
								extra_types [nextra] = ct; type_idx = ti_base + nextra++;
							}
							memset (&ht, 0, sizeof (ht)); ht.params [0] = WASM_I32; ht.nparams = 1; ht.ret = WASM_I32;
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ht)) { hti = ti_base + k; break; }
							if (hti < 0) {
								if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
								extra_types [nextra] = ht; hti = ti_base + nextra++;
							}
							uses_calls = TRUE;
							late_fslot_block = TRUE;
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $after_residual */
							wasm_i32_const (&body, (gint32) (intptr_t) late_im);
							wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_late_fslot, hti);
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							{
								for (ai = 0; ai < cci.nargs; ++ai)
									if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "late call arg ld"; goto done; }
								if (cci.vret_byaddr && !wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "late call vret ld"; goto done; }
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wj_emit_dynamic_call (&body, (guint32) type_idx);
								if (ct.ret != WASM_VOID) {
									if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
										if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "late scalar-vtype ret"; goto done; }
									} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "late call dreg"; goto done; }
								}
							}
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> $after_residual */
							wasm_op (&body, WASM_OP_END);
						}
					}
					memset (&ts, 0, sizeof (ts)); ts.ret = WASM_I32; ts.nparams = 0;
					memset (&ti, 0, sizeof (ti)); ti.ret = WASM_I32; ti.nparams = 2; ti.params [0] = WASM_I32; ti.params [1] = WASM_I32;
					for (k = 0; k < nextra; ++k) {
						if (tsi < 0 && functype_eq (&extra_types [k], &ts)) tsi = ti_base + k;
						if (tii < 0 && functype_eq (&extra_types [k], &ti)) tii = ti_base + k;
					}
					if (tsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ts; tsi = ti_base + nextra++; }
					if (tii < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ti; tii = ti_base + nextra++; }
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
						for (tpk = 0; tpk < nextra; ++tpk) if (functype_eq (&extra_types [tpk], &tp)) { tpi = ti_base + tpk; break; }
						if (tpi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tp; tpi = ti_base + nextra++; }
						wasm_i32_const (&body, (gint32) (intptr_t) call_method);
						wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_pretransform, tpi);
					}
						/* $scratch = imported s.b. `wj_scratch` is a __thread ARRAY, so its address is a per-thread
						 * CONSTANT — no load and, more importantly, no helper CALL, which Rounds 88/90 established
						 * is the expensive part of a dispatch path in this backend. */
					wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);
					wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
					{
						for (ai = 0; ai < cci.nargs; ++ai) {
							WasmOpcode sop; int al;
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "residual arg ld"; goto done; }
							switch (ct.params [ai]) {
							case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
							case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
							case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
							case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
							default: fail = "residual arg type"; goto done;
							}
							wasm_op (&body, sop); wasm_memarg (&body, (guint32) al, (guint32) (ai * 8));
						}
						/* hidden vret: bake the caller's return-buffer address at scratch+232. The C side
						 * memcpys the VT result through it — NEVER via the 8-byte ret slot at 192, which a
						 * large struct would overrun into the 200..231 control fields. (A by-addr vtype arg's
						 * scratch slot likewise holds the copy's ADDRESS; call_interp/aot_call_lean deref
						 * those per mono_wasm_jit_arg_is_byaddr.) */
						if (cci.vret_byaddr) {
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "residual vret ld"; goto done; }
							wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 232 /* WJ_SCRATCH_VRET_OFF */);
						}
					}
					/* DIAG (WASM_JIT_BADREF_ARG caller attribution): bake THIS (calling) method at scratch+224 so
					 * call_interp's bad-ref check can name the type-confusion SOURCE, not just the callee. Written
					 * immediately before the call (after the spills) so a nested residual can't leave a stale value. */
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
					wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
					wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 224 /* WJ_SCRATCH_CALLER_OFF */);
					/* mono_wasm_jit_call_interp(method, $scratch): bake call_method (the synchronized wrapper for a
					 * synchronized callee) so the interp runs it WITH the monitor; plain callees pass through. */
					wasm_i32_const (&body, (gint32) (intptr_t) call_method);
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
					wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_call_interp, tii);
					/* call_interp returns 1 if the callee threw: the result slot is stale AND an exception
					 * is armed. Continue the native unwind (never a normal return -- that skipped this
					 * frame's own landing pad and delivered the exception late; FINDINGS Finding 15). */
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					/* threw: continue the native unwind; do NOT return and do NOT pop roots first. */
					EMIT_RESIDUAL_THROW_CONTINUATION ();
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
						/* NB byref: a byref ret IS WASM_I32, so this guard fires — it is
						 * mini_get_underlying_type erasing byref to MONO_TYPE_I that keeps the switch a
						 * no-op over the pointer. Skip explicitly rather than rely on that (see
						 * wasm_emit_subword_ret_norm). */
						if (ct.ret == WASM_I32 && !m_type_is_byref (csig->ret)) {
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
						if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
							if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "residual scalar-vtype ret"; goto done; }
						} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "residual dreg"; goto done; }
					}
					if (late_fslot_block)
						wasm_op (&body, WASM_OP_END); /* $after_residual */
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
				/* Do not reject an rgctx-bearing virtual call here. The virtual path resolves the
				 * concrete override first: its JIT f-slot uses our canonical concrete ABI, its AOT
				 * route obtains the resolved ftndesc rgctx from vcall_aot_target, and its interpreter
				 * fallback derives context from that concrete MonoMethod. The callsite rgctx is thus
				 * neither part of nor needed by any of those three dispatches. This gate used to keep
				 * otherwise-concrete Minecraft virtual callers permanently interpreted (~0.9M weighted
				 * vperm calls in profile8). Raw indirect calls are handled by their existing auto-JIT
				 * safety gate below; in the explicitly targeted case their rgctx outarg is already folded
				 * into csig/call_info, as documented there. */
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
						extern gpointer mono_wasm_jit_vcall_aot_pic_lookup (MonoVTable *vt, gpointer aic);
							extern int mono_wasm_jit_call_interp (MonoMethod *m, guint8 *buf);
							extern int mono_wasm_jit_call_delegate (MonoMethod *m, guint8 *buf);
						WasmFuncType vts, vtrf, vtd, ftd, dftd; WasmCallInfo vci; int vtsi = -1, vtrfi = -1, vtdi = -1, ftdi = -1, dftdi = -1, vk, ai, n2, this_vr; int aic_ati = -1, aic_ati_ne = -1;
						WasmValtype pp [WASM_FUNCTYPE_MAX_PARAMS], rv; int npp = 0; gpointer vic;
						MonoVTable *pred_vt = NULL;
						MonoMethod *pred_target = NULL;
						int pred_fslot = 0;
						gboolean slim_pred = FALSE;
						gboolean is_delegate_invoke = !strcmp (call->method->name, "Invoke") &&
							m_class_get_parent (call->method->klass) == mono_defaults.multicastdelegate_class;
						if (!csig->hasthis) { fail = "vcall not instance"; goto done; }
						if (!call->call_info) { fail = "no captured vcall args"; goto done; }
						this_vr = wj_arg_vreg (call, 0);
						/* BYREF args/ret (MONO_WASM_JIT_BYREF, default on) — see the long rationale at the direct
						 * residual gate: the arg deref and the byref-guarded stackval_to_data_sign_ext make
						 * interp_entry's by-pointer marshalling correct, so we no longer bail.
						 * Relaxing THIS gate is the larger half of the win. It sits at the very TOP of the vcall
						 * lowering — before mono_wasm_get_call_info below, the inline f-slot IC, the devirt/slim
						 * PIC, the AOT IC, the direct-f-slot fast path AND the residual fallback — so a byref
						 * virtual site used to lose EVERY fast path, not just the interp fallback, even though
						 * all the wasm-only ones pass a byref as a plain i32. (mono_wasm_get_call_info classifies
						 * a byref as WJ_ARG_SCALAR/i32 with valid=TRUE, so it clears the arg-kind filter below.) */
						if (!mono_wasm_jit_byref) {
							if (m_type_is_byref (csig->ret)) {
								fail = "vcall byref ret";
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
								goto done;
							}
							for (ai = 0; ai < (int) csig->param_count; ++ai)
								if (m_type_is_byref (csig->params [ai])) {
									fail = "vcall byref arg";
									fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = ai;
									goto done;
								}
						}
						/* Arg/ret valtypes from the shared ABI descriptor (this + params -> ret). A
						 * scalar-vtype arg is scalar at the wasm boundary too; load its single field in
						 * every fast/AOT path and spill that same scalar into the residual scratch. */
						{
							mono_wasm_get_call_info (csig, &vci);
							if (!vci.valid) {
								fail = vci.vret_byaddr ? "vcall ret type" : vci.fail_reason;
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = vci.fail_arg;
								goto done;
							}
							/* hidden vret is now valid under MONO_WASM_JIT_VRET, but the vcall-IC lowering for it
							 * is deferred (Stage 2.4) — explicit bail since valid no longer implies a scalar ret */
							if (vci.vret_byaddr) {
								fail = "vcall ret type";
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
								goto done;
							}
							for (ai = 0; ai < vci.nargs; ++ai)
								if (vci.args [ai].kind != WJ_ARG_SCALAR && vci.args [ai].kind != WJ_ARG_VTYPE_SCALAR &&
								    vci.args [ai].kind != WJ_ARG_VTYPE_BYADDR) { fail = "vcall arg type"; goto done; }
							for (ai = 0; ai < (int) vci.ftype.nparams; ++ai) pp [ai] = vci.ftype.params [ai];
							npp = (int) vci.ftype.nparams;
							rv = vci.ftype.ret;
						}
						/* functypes: vts ()->i32 (scratch); vtrf (i32,i32,i32,i32)->i32 (resolve_fslot: this,base,scratch,ic);
						 * vtd (i32,i32)->i32 (call_interp fallback); ftd this+params->ret (the override's scalar `f`). */
						memset (&vts, 0, sizeof (vts)); vts.nparams = 0; vts.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vts)) { vtsi = ti_base + vk; break; }
						if (vtsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vts; vtsi = ti_base + nextra++; }
						memset (&vtrf, 0, sizeof (vtrf)); vtrf.params [0] = WASM_I32; vtrf.params [1] = WASM_I32; vtrf.params [2] = WASM_I32; vtrf.params [3] = WASM_I32; vtrf.nparams = 4; vtrf.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtrf)) { vtrfi = ti_base + vk; break; }
						if (vtrfi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtrf; vtrfi = ti_base + nextra++; }
						memset (&vtd, 0, sizeof (vtd)); vtd.params [0] = WASM_I32; vtd.params [1] = WASM_I32; vtd.nparams = 2; vtd.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtd)) { vtdi = ti_base + vk; break; }
						if (vtdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtd; vtdi = ti_base + nextra++; }
						memset (&ftd, 0, sizeof (ftd)); for (vk = 0; vk < npp; ++vk) ftd.params [vk] = pp [vk]; ftd.nparams = (guint32) npp; ftd.ret = rv;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &ftd)) { ftdi = ti_base + vk; break; }
						if (ftdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ftd; ftdi = ti_base + nextra++; }
						/* Adaptive speculative devirtualization. A perfectly-monomorphic interpreter profile
						 * can slim ANY ordinary virtual site whose target already owns an admitted f-slot.
						 * Terminal forwarding calls retain the stronger bottom-up behavior: if their target
						 * has not compiled yet, make it an island blocker and re-emit after publication.
						 * Ordinary compute methods never grow their island merely for speculation; a target
						 * not already present simply leaves that site on the compact PIC.
						 *
						 * Native-AOT and permanently un-JITtable targets retain the ordinary PIC/AOT miss
						 * routes. Registering a successful direct dependency makes the predicted target
						 * available in every worker before this caller is admitted there. */
#ifdef HOST_BROWSER
						if (!is_delegate_invoke && mono_wasm_jit_devirt_profile &&
						    (mono_wasm_jit_vcall_slim ||
						     (terminal_vcall_handoff && terminal_vcall_ins == ins))) {
							if (mono_wasm_jit_vprof_predict (mono_interp_get_imethod (cfg->method),
							    call->method, &pred_vt, &pred_target, NULL)) {
								extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
								extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
								extern gboolean mono_interp_jit_call_supported (MonoMethod *method,
									MonoMethodSignature *sig);
								MonoMethodSignature *pred_sig = mono_method_signature_internal (pred_target);
								WasmCallInfo pred_ci;
								mono_wasm_get_call_info (pred_sig, &pred_ci);
								if (!pred_ci.valid || pred_ci.vret_byaddr ||
								    !functype_eq (&pred_ci.ftype, &ftd)) {
									pred_vt = NULL;
									pred_target = NULL;
									pred_fslot = 0;
								} else {
									pred_fslot = mono_wasm_jit_get_callee_fslot (pred_target);
									if (pred_fslot <= 0) {
										if (terminal_vcall_handoff && terminal_vcall_ins == ins &&
										    pred_target != cfg->method &&
										    !mono_wasm_jit_callee_perm_unjittable (pred_target) &&
										    !mono_interp_jit_call_supported (pred_target, pred_sig)) {
											wj_result_add_blocker (&cfg->wasm_jit_result, pred_target);
											fail = "callee not jitted (terminal devirt)";
											goto done;
										}
										pred_vt = NULL;
										pred_target = NULL;
										pred_fslot = 0;
									} else {
										wj_result_add_direct_dep (&cfg->wasm_jit_result, pred_fslot,
											wj_functype_hash (&ftd), pred_target);
										if (cfg->wasm_jit_result.direct_deps_truncated) {
											fail = "too many direct dependencies";
											goto done;
										}
										slim_pred = mono_wasm_jit_vcall_slim &&
											mono_wasm_jit_vcall_inline_ic &&
											mono_wasm_jit_vcall_shared_miss_enabled;
									}
								}
							}
						}
#endif
						/* Open-static/open-instance delegate targets consume the Invoke arguments after
						 * dropping the delegate receiver. Their canonical f-thunk type is therefore ftd
						 * without parameter zero. Closed-instance/bound-static retain the full ftd. */
						if (is_delegate_invoke) {
							if (npp < 1) { fail = "delegate no receiver"; goto done; }
							memset (&dftd, 0, sizeof (dftd));
							for (vk = 1; vk < npp; ++vk) dftd.params [vk - 1] = pp [vk];
							dftd.nparams = (guint32) (npp - 1); dftd.ret = rv;
							for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &dftd)) { dftdi = ti_base + vk; break; }
							if (dftdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = dftd; dftdi = ti_base + nextra++; }
						}
						uses_calls = TRUE;
						n2 = csig->param_count + 1; /* this + params */
						/* per-call-site AOT-vcall IC cell (VCALL_AOT_IC): 20B, see mono_wasm_jit_alloc_aot_ic */
						gpointer aic = NULL;
						{ extern int mono_wasm_jit_vcall_aot_ic, mono_wasm_jit_vcall_inline_ic, mono_wasm_jit_vcall_aot;
						  /* Delegate wrapper selection depends on the instance's target shape, not just its vtable.
						   * A vtable-keyed AOT IC could therefore reuse a normal wrapper for an open-virtual or bound
						   * delegate of the same delegate type. Leave those sites on the instance-aware helper path. */
						  if (!slim_pred && !is_delegate_invoke && mono_wasm_jit_vcall_aot_ic && mono_wasm_jit_vcall_inline_ic && mono_wasm_jit_vcall_aot) {
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
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { aic_ati = ti_base + vk; break; }
								if (aic_ati < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at; aic_ati = ti_base + nextra++; }
								memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { aic_ati_ne = ti_base + vk; break; }
								if (aic_ati_ne < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at_ne; aic_ati_ne = ti_base + nextra++; }
							}
							if (aic_ati < 0 || aic_ati_ne < 0) aic = NULL;   /* functype table full -> skip AOT-IC (safe) */
						}
						/* Per-call-site N-way virtual IC plus miss metadata in shared memory. Delegate.Invoke
						 * sites append a single-cast dispatch recipe used by the instance-aware helper. */
#ifdef HOST_BROWSER
						{ extern gpointer mono_wasm_jit_alloc_ic (int delegate_site, MonoMethod *caller, MonoMethod *base);
						  vic = mono_wasm_jit_alloc_ic (is_delegate_invoke, cfg->method, call->method); }
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
						/* NCE: method_to_ir already emitted an IR-level check for this same receiver on
						 * nearly every callvirt, so this block is usually a pure duplicate of a test
						 * that ran a few instructions ago. Skip it when the receiver is already proven;
						 * otherwise emit it and record the proof for the rest of the bb. */
						if (NN_GET (this_vr)) goto vcall_nullchk_done;
						NN_SET (this_vr);
						{
							if (!WJ_HAS_THROW (4)) { fail = "no shared throw block for NRE"; goto done; }
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall nullchk this"; goto done; }
							wasm_op (&body, WASM_OP_I32_EQZ);
							WJ_THROW_BR (4, 0);
						}
vcall_nullchk_done:
						/* --- WORKER-LOCAL INLINE VCALL PIC ---
						 * A stable site id selects this worker's compact {receiver-vtable, admitted-fslot}
						 * entries through imported TLS pointer/capacity addresses. Because a miss publishes only
						 * after admitting the target into this worker's function table, a hit needs neither the old
						 * shared InterpMethod load nor a slot-liveness bitmap test. Way zero remains straight-line;
						 * the remaining ways use one compact loop and all hits share one typed call tail. */
						if (mono_wasm_jit_vcall_inline_ic) {
#ifdef HOST_BROWSER
							extern int mono_wasm_jit_imethod_fslot_off (void);
							int fslot_off = mono_wasm_jit_imethod_fslot_off ();
							extern guint32 mono_wasm_jit_vcall_pic_site_id (gpointer ic);
							extern int mono_wasm_jit_vcall_pic_stride (void);
							guint32 vpic_site = mono_wasm_jit_vcall_pic_site_id (vic);
							int vpic_stride = mono_wasm_jit_vcall_pic_stride ();
							extern gpointer mono_wasm_jit_delegate_ic_base (gpointer ic);
							extern int mono_wasm_jit_delegate_ic_stride (void);
							extern int mono_wasm_jit_delegate_ic_field_off (int field);
							extern int mono_wasm_jit_delegate_field_off (int field);
							gpointer delegate_ic = is_delegate_invoke ? mono_wasm_jit_delegate_ic_base (vic) : NULL;
							int delegate_ic_stride = mono_wasm_jit_delegate_ic_stride ();
							int dic_seq_off = mono_wasm_jit_delegate_ic_field_off (0);
							int dic_source_off = mono_wasm_jit_delegate_ic_field_off (1);
							int dic_receiver_off = mono_wasm_jit_delegate_ic_field_off (2);
							int dic_imethod_off = mono_wasm_jit_delegate_ic_field_off (3);
							int dic_shape_off = mono_wasm_jit_delegate_ic_field_off (4);
							int dic_scalar_off = mono_wasm_jit_delegate_ic_field_off (5);
							int delegate_target_off = mono_wasm_jit_delegate_field_off (0);
							int delegate_method_off = mono_wasm_jit_delegate_field_off (1);
							int delegate_list_off = mono_wasm_jit_delegate_field_off (2);
							extern int mono_wasm_jit_delegate_pic_stride (void);
							int dpic_stride = mono_wasm_jit_delegate_pic_stride ();
#else
							int fslot_off = 0x40; /* placeholder for offline encoder validation (real offset only matters at runtime) */
							guint32 vpic_site = 0;
							int vpic_stride = 16;
							gpointer delegate_ic = (gpointer) (intptr_t) 0x7000;
							int delegate_ic_stride = 32, dic_seq_off = 0, dic_source_off = 4;
							int dic_receiver_off = 8, dic_imethod_off = 16, dic_shape_off = 20, dic_scalar_off = 28;
							int delegate_target_off = 16, delegate_method_off = 20, delegate_list_off = 64;
							int dpic_stride = 16;
#endif
							/* Worker-local delegate PIC field offsets, mirroring WjLocalDelegatePicEntry. The way
							 * offset folds into the load immediate, so a way costs no base arithmetic at all. */
							extern int mono_wasm_jit_delegate_local_pic;
							int dpic_local = mono_wasm_jit_delegate_local_pic;
							int dpic_rvt_off = 8, dpic_shape_off = 12;
							int way;
							/* Inline-cache width for THIS site, from the interpreter's receiver observations, in
							 * place of the global MONO_WASM_JIT_VCALL_WAYS everywhere. A site the interpreter only
							 * ever saw one receiver at does not need the ways 1..N-1 cold-scan loop, and that loop
							 * is not free: it is a block, a loop, bounds arithmetic and a call_indirect emitted
							 * into every virtual call site in every method, whether or not the receiver is ever
							 * polymorphic.
							 *
							 * Narrowing is safe in the only direction that matters: the emitted code re-checks the
							 * cached vtable on every call, so an under-sized site costs IC misses, never a wrong
							 * target. mono_wasm_jit_vprof_arity returns 0 unless the site is warm (>= 8 samples)
							 * and its receiver set did not overflow, so "no data" and "too many types" both keep
							 * the configured width.
							 *
							 * The PIC array layout is deliberately NOT narrowed: its stride stays at the global
							 * width so the addressing baked into emitted code and the runtime's fill path continue
							 * to agree. A narrow site simply leaves its upper slots unused.
							 *
							 * Delegate.Invoke sites are sized from the same number, fed by a separate recorder in
							 * MINT_CALL_DELEGATE (wj_vprof_record_delegate) that counts distinct delegate TARGETS
							 * rather than receiver vtables, because that is what the delegate IC's ways
							 * discriminate on. That is the population that matters for MethodHandle stubs: a
							 * MethodHandle invoke is a delegate invoke, so those stubs carry the unrolled
							 * VCALL_WAYS-deep chain below and it dominates their emitted size. */
							int ic_ways = mono_wasm_jit_vcall_ways;
#ifdef HOST_BROWSER
							if (mono_wasm_jit_ic_autosize && ic_ways > 1) {
								extern guint32 mono_wasm_jit_vprof_arity (gpointer caller, MonoMethod *base);
								guint32 ar = mono_wasm_jit_vprof_arity (mono_interp_get_imethod (cfg->method), call->method);
								/* arity + 1, not arity. The profile only observes this site while the caller is
								 * still INTERPRETED, so it cannot see receivers that first appear after the
								 * method is compiled -- and the counters say that is exactly what happens: the
								 * vcall IC miss rate is 9.0% inside the in-game window against 2.83% run
								 * cumulative, i.e. misses concentrate after compilation. One spare way absorbs
								 * the common "one new type shows up later" case; a miss costs a trip through
								 * mono_wasm_jit_vcall_resolve_fslot, which is 1781 instructions and the single
								 * hottest symbol in the profile at 2.36%, so the spare guard is far cheaper
								 * than the miss it prevents. */
								/* Exactly the observed arity, NOT arity+1. The spare-way idea was tried and
								 * MEASURED as a loss: it improved the true miss rate 1.178% -> ~0.95% but cost
								 * ~34% of in-game fps (13.4 -> 8.9, tight 1.8% spread over two clean runs),
								 * because an extra way adds a guard to EVERY dispatch -- ~350M of them per
								 * in-game window -- to avoid misses that are only ~1% of that traffic. Paying a
								 * ubiquitous cost for a rare saving is the wrong direction, however cheap the
								 * guard looks in isolation. */
								if (ar > 0 && (int) ar < ic_ways)
									ic_ways = (int) ar;
							}
#endif
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $after (void) */
							/* Strict monomorphic guard. A hit bypasses the worker PIC completely. The prediction
							 * is deliberately speculative: interpreter warmup can see one receiver before the
							 * steady-state workload introduces another. Adaptive slim therefore retains one
							 * worker-local alternate way below. It costs one bounds check + one i64 guard on a
							 * prediction miss, but turns the second and later calls of the common alternate into
							 * direct f-slot dispatch instead of repeatedly entering the shared resolver. */
							if (pred_fslot > 0 && pred_vt && pred_target && !is_delegate_invoke) {
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $pred_miss */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "devirt this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_i32_const (&body, (gint32) (intptr_t) pred_vt);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "devirt arg ld"; goto done; }
									/* Same single hole as an ordinary direct call. The prediction is already
									 * guarded (this->vtable == pred_vt), so if the assembler can co-locate
									 * pred_target this becomes a `call <funcidx>` that V8 may inline -- a VIRTUAL
									 * call inlined behind a guard, which is the thing mono itself cannot do
									 * (method-to-ir.c:8199 admits a candidate only when the site or the target is
									 * non-virtual, or the target is final). */
									wj_emit_method_call (&body, pred_target, pred_fslot, (guint32) ftdi);
									if (rv != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "devirt dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> outer $after */
								wasm_op (&body, WASM_OP_END); /* $pred_miss */
							}
							if (slim_pred) {
								/* One compact worker-local alternate PIC entry. The first prediction miss reaches
								 * vcall_resolve_fslot, which publishes its admitted {vtable,fslot} into way zero.
								 * Thereafter this guard handles that receiver without the miss frame, Mono lookup,
								 * admission walk, or interpreter bridge. More polymorphic sites still retain the
								 * shared cold path; keeping only one alternate is what preserves slim code size. */
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $slim_pic_miss */
									/* site < *TLS-cap, otherwise the worker has not allocated this PIC yet */
									wasm_i32_const (&body, (gint32) vpic_site);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_capval_idx);
									wasm_op (&body, WASM_OP_I32_GE_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* entry = *TLS-ptr + site * ways * stride */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_ptr_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride));
									wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "slim pic this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
									wasm_i64_const (&body, 32);
									wasm_op (&body, WASM_OP_I64_SHR_U);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* Preserve exact alternate-target frequency when profiling is enabled. */
									{
										extern int mono_wasm_jit_profile_fast;
										if (mono_wasm_jit_profile_fast && !wj_batch_profile_suppressed) {
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 12);
											wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
											wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 12);
										}
									}
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "slim pic arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wj_emit_dynamic_call (&body, (guint32) ftdi);
									if (rv != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "slim pic dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> outer $after */
								wasm_op (&body, WASM_OP_END); /* $slim_pic_miss */
								goto vcall_cold_miss_emit;
							}
							/* OBJGUARD (once, hoisted above the N ways): validate the receiver before any raw
							 * this->vtable load. Otherwise a type-confused scalar/garbage receiver traps as a bare
							 * wasm OOB before the C resolver can print anything. */
#ifdef HOST_BROWSER
							if (G_UNLIKELY (lc.objguard && lc.check_ti >= 0)) {
								extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this guard ld"; goto done; }
								wasm_i32_const (&body, 5);
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, lc.check_ti);
							}
#endif
							/* Delegate.Invoke has a second IC beside the ordinary receiver-vtable IC. Its miss path
							 * (resolve_fslot -> prepare_delegate_call) publishes a seqlock-protected recipe keyed by
							 * delegate.method and, for virtual targets, target->vtable. Consume that recipe here before
							 * entering either C resolver. On a hit the current closed target is read from the delegate
							 * instance (never cached), and the per-thread f-slot bitmap provides the same admission gate
							 * as the ordinary inline vcall IC. Multicast delegates explicitly miss: a recipe learned from
							 * a single-cast instance at this polymorphic callsite must never bypass their invocation list. */
							if (is_delegate_invoke) {
								/* ic_ways, not the global width: a site the interpreter only ever saw one target
								 * at needs one chain, and each dead chain here is a seqlock read, a compare, a
								 * branch and two call_indirect in every body containing the site. */
								/* dpic_local: read the WORKER-LOCAL recipe PIC instead of the shared WjDelegateIC.
								 * The local entry caches the ALREADY ADMITTED fslot next to its guard key in one
								 * i64, which removes, per dispatch: the two-atomic seqlock bracket, the scalar
								 * test, the imethod -> imethod->fslot double load, and the whole TLS liveness
								 * bitmap probe. Nothing here is a weaker check -- each removed test is instead an
								 * invariant of publication (wj_delegate_pic_publish), and the entry is private to
								 * this worker so there is no writer to race. */
								if (dpic_local) {
									/* Emitted ONCE for all ways: the multicast test depends only on `this`, and
									 * the base/bounds arithmetic only on the site. Way selection then folds into
									 * the load immediates, so an extra way costs no address arithmetic at all. */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_ic_done */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate pic list ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_list_off);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_i32_const (&body, (gint32) vpic_site);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dpic_capval_idx);
									wasm_op (&body, WASM_OP_I32_GE_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Array BASE only — the site displacement folds into each way's load immediate
									 * below, exactly as for the fslot IC, so the `i32.const; i32.add` disappears from
									 * every delegate dispatch (~1015M per window). */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dpic_ptr_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dic_base_idx);
								}
								for (way = 0; way < ic_ways; ++way) {
								    if (dpic_local) {
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_way_fail */
									/* delegate.method == entry.source, and the admitted fslot arrives in the SAME
									 * i64 as the key it is guarded by — one load for guard and payload both. */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate pic source ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_method_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride));
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* An empty way is all zero and a real MonoMethod* is never 0, so the compare
									 * above is also the occupancy test — no separate seq/valid field. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride + dpic_rvt_off));
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_rgctx_idx);
								    } else {
									gint32 dic_addr = (gint32) (intptr_t) delegate_ic + way * delegate_ic_stride;
									/* Materialise the entry base ONCE per way; the seven field reads below use the
									 * local. Saves 20 wire bytes a way, which is what decides whether the enclosing
									 * stub fits V8's 500-byte inlining cap. */
									wasm_i32_const (&body, dic_addr);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_way_fail */
									/* seq must be stable, nonzero, and even. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x10); wasm_memarg (&body, 2, (guint32) dic_seq_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Multicast list must be null. */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_list_off);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* delegate.method == recipe.source */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic source ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_method_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_source_off);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* A zero receiver key denotes nonvirtual/static recipes. Otherwise validate the
									 * current closed target's vtable without introducing another control depth. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_receiver_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_rgctx_idx);
								    }
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic target ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_rgctx_idx);
											wasm_op (&body, WASM_OP_I32_EQ);
										wasm_op (&body, WASM_OP_ELSE);
											wasm_i32_const (&body, 0);
										wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_ELSE);
										wasm_i32_const (&body, 1);
									wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								    if (dpic_local) {
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride + dpic_shape_off));
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									/* fslot = high half of the pair already loaded and validated above. It is
									 * nonzero, scalar-shaped and live on THIS worker by construction, not by
									 * inspection: wj_delegate_pic_publish stores only an admitted scalar fslot,
									 * the array is per-worker, and slot-live bits are never cleared. That is what
									 * retires the scalar test, the fslot != 0 test and the bitmap probe. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
									wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								    } else {
									/* Only scalar recipes have a canonical direct f-thunk ABI. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_scalar_off);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_shape_off);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_imethod_off);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) fslot_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Re-read seq after all payload fields. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x10); wasm_memarg (&body, 2, (guint32) dic_seq_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Per-thread f-slot liveness/admission gate. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) slotlive_cap_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op (&body, WASM_OP_I32_LT_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) slotlive_ptr_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHR_U);
										wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op (&body, WASM_OP_I32_LOAD8_U); wasm_memarg (&body, 0, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wasm_i32_const (&body, 7); wasm_op (&body, WASM_OP_I32_AND);
										wasm_op (&body, WASM_OP_I32_SHR_U);
										wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);
									wasm_op (&body, WASM_OP_ELSE);
										wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								    }
									wj_emit_fast_count (&body, WJC_DELEGATE_IC_HIT);
									wj_emit_fast_count (&body, WJC_FAST_DELEGATE);
									/* Closed-instance/bound-static replace Delegate this with the current target;
									 * open-static/open-instance simply drop Delegate this. Both arms return the same
									 * scalar value, so a typed if carries it to the common destination store. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
									wasm_op (&body, WASM_OP_I32_LE_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, rv == WASM_VOID ? 0x40 : (guint8) rv);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic closed this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate ic closed arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) ftdi);
									wasm_op (&body, WASM_OP_ELSE);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate ic open arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) dftdi);
									wasm_op (&body, WASM_OP_END);
									if (rv != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "delegate ic dreg"; goto done; }
									/* One level deeper in the local-PIC arm: the ways sit inside the shared
									 * $delegate_ic_done block that carries the hoisted multicast/bounds tests. */
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, dpic_local ? 2 : 1); /* -> outer $after */
									wasm_op (&body, WASM_OP_END); /* $delegate_way_fail */
								}
								if (dpic_local)
									wasm_op (&body, WASM_OP_END); /* $delegate_ic_done */
							}
								/* Worker-local compact N-way {vtable,fslot} PIC. The stable site id indexes the
								 * current worker's TLS array selected by imported s.v/s.n. The miss helper publishes
								 * only an admitted local fslot, eliminating the InterpMethod load and liveness bitmap
								 * test from a hit. Each 16-byte entry keeps the hot pair at +0 and cold
								 * {target,count} batching feedback at +8/+12. Way zero stays straight-line for V8's
								 * monomorphic feedback; cold ways share one loop and all ways share one typed tail. */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $slow (void) */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $tail (void) */
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this ld"; goto done; }
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
								/* Bounds-check before dereferencing the TLS PIC pointer. */
								wasm_i32_const (&body, (gint32) vpic_site);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_capval_idx);
								wasm_op (&body, WASM_OP_I32_GE_U);
								wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1); /* miss -> $slow */
								/* Keep the ARRAY BASE in the local and fold `site * ways * stride` into way zero's
								 * load immediate, instead of materialising a cursor. A wasm load already carries a
								 * 32-bit offset, so the `i32.const K; i32.add` that built the cursor is pure waste on
								 * every HIT — and a hit is the overwhelming case. The cold ways still need a real
								 * moving cursor, so it is computed inside $way_fail below, i.e. only when way zero
								 * misses. Net: 2 fewer ops per dispatch on ~822M fslot-IC dispatches per window. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_ptr_idx);
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								/* Only the monomorphic/MRU way is emitted straight-line. */
							for (way = 0; way < 1; ++way) {
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /*   $way_fail (void) */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
								/* ic = i64.load(base + site*ways*stride); if ((i32)ic != vtab) -> $way_fail */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
								wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride));
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
							wasm_op (&body, WASM_OP_I32_WRAP_I64);
							wasm_op (&body, WASM_OP_I32_NE);
							wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								/* fslot = high32. A matching real vtable implies a nonzero admitted fslot. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
								wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* Exact target-frequency writes are opt-in: normal dispatch must not dirty a
									 * cache line on every hit. PROFILE_FAST already denotes a dedicated sampling
									 * run, so the batcher gets both aggregate and per-target volume from one flag. */
									{
										extern int mono_wasm_jit_profile_fast;
										if (mono_wasm_jit_profile_fast && !wj_batch_profile_suppressed) {
										/* aic_ti holds the array BASE now, so the hit counter's offset carries the
										 * site displacement as well (entry+12 == base + site_off + 12). */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + 12));
										wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + 12));
									}
								}
							/* HIT: the resolved f-slot is in $vc_fslot; jump to the shared call tail below. */
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> exit $tail */
							wasm_op (&body, WASM_OP_END);                            /* end $way_fail */
							}   /* for each way */
							if (ic_ways > 1) {
									/* Way zero missed: scan ways 1..N with one constant-size wasm loop. */
									wasm_i32_const (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* Way zero missed: NOW materialise the moving cursor the cold loop needs, at
									 * base + site_off + stride (way 1). This arithmetic used to run on every
									 * dispatch to build a cursor way zero did not need. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + vpic_stride));
									wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $cold_done */
								wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);  /* $cold_loop */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $cold_next */
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op (&body, WASM_OP_I32_NE);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
											wasm_i64_const (&body, 32);
											wasm_op (&body, WASM_OP_I64_SHR_U);
											wasm_op (&body, WASM_OP_I32_WRAP_I64);
											wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
											{
												extern int mono_wasm_jit_profile_fast;
												if (mono_wasm_jit_profile_fast && !wj_batch_profile_suppressed) {
													wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
													wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
													wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 12);
													wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
													wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 12);
												}
											}
											wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 2); /* hit -> $cold_done */
									wasm_op (&body, WASM_OP_END); /* $cold_next */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_i32_const (&body, vpic_stride); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_ptr_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
										/* End of the ways this site actually checks. The site's REGION is still
										 * global-width (stride unchanged), so this is base + ic_ways entries, not
										 * the start of the next site — those two coincide only when the site was
										 * not narrowed. */
										wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + ic_ways * vpic_stride));
										wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op (&body, WASM_OP_I32_LT_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0); /* -> $cold_loop */
									wasm_i32_const (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_END); /* $cold_loop */
								wasm_op (&body, WASM_OP_END); /* $cold_done */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0); /* hit -> exit $tail */
							}
							/* every way missed -> exit $slow, skipping the shared tail */
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);
							wasm_op (&body, WASM_OP_END);                            /* end $tail */
							/* SHARED HIT TAIL, emitted once for the whole chain: push this+args (fresh from
							 * vregs -- nothing is carried across the block boundary, so both blocks stay void),
							 * then call_indirect the f-slot the winning guard resolved. */
							wj_emit_fast_count (&body, WJC_FAST_VIC);   /* profile: inline f-slot IC hit (JIT->JIT) */
							for (ai = 0; ai < n2; ++ai)
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "ic fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
							wj_emit_dynamic_call (&body, (guint32) ftdi);
							if (rv != WASM_VOID) { if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ic fast dreg"; goto done; } }
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> $after (skip the C-helper slow path) */
							wasm_op (&body, WASM_OP_END);                            /* end $slow */
							/* falls through to the AOT-IC / resolve_fslot slow path, at its ORIGINAL nesting */
							/* --- INLINE AOT-VCALL IC (VCALL_AOT_IC), MT-safe: two atomic i64 words, each vtab-tagged. A hit needs
							 * BOTH words' low32 == this->vtable; then ti/kind (ic1.hi) and rgctx (ic2.hi) are correct for this
							 * vtable regardless of interleaved fills (a vtable maps to ONE target -> identical values). Only
							 * atomic i64 loads (no plain-vs-atomic ordering, which wasm's memory model does not provide). */
							if (aic) {
								/* Compact N-way AOT PIC. Way zero stays inline; ways 1..N are scanned by one
								 * signature-independent helper which returns an entry address. Both routes then
								 * use this single tag validation, payload extraction, argument load and typed
								 * call tail. Cache capacity therefore no longer multiplies generated code. */
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "aot ic this ld"; goto done; }
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_selected */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_way0_fail */
										wasm_i32_const (&body, (gint32) (intptr_t) aic); wj_emit_ic_load64 (&body, 0);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_i32_const (&body, (gint32) (intptr_t) aic); wj_emit_ic_load64 (&body, 8);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_i32_const (&body, (gint32) (intptr_t) aic);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx); /* selected entry ptr */
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* skip cold scan */
									wasm_op (&body, WASM_OP_END);
									if (mono_wasm_jit_vcall_aot_ways > 1) {
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_i32_const (&body, (gint32) (intptr_t) aic);
#ifdef HOST_BROWSER
										wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_aot_pic_lookup, vtdi);
#else
										wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7fe2, (guint32) vtdi);
#endif
									} else {
										wasm_i32_const (&body, 0);
									}
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_END);
								/* Revalidate both tags while loading the payload. A concurrent eviction between
								 * selection and this load becomes a miss rather than mixed target data. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx); wj_emit_ic_load64 (&body, 0); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx); wj_emit_ic_load64 (&body, 8); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_rgctx_idx);
								wj_emit_fast_count (&body, WJC_FAST_AOTIC);   /* profile: inline AOT-IC hit (JIT->AOT) */
								for (ai = 0; ai < n2; ++ai) if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "aot ic arg ld"; goto done; }
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);   /* kind2bit selector */
								wasm_op (&body, WASM_OP_IF); wj_emit_blocktype (&body, (guint32) aic_ati_ne);   /* typed block in: (this,args) out: rv */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);   /* ti */
									wj_emit_dynamic_call (&body, (guint32) aic_ati_ne);
								wasm_op (&body, WASM_OP_ELSE);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_rgctx_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);
									wj_emit_dynamic_call (&body, (guint32) aic_ati);
								wasm_op (&body, WASM_OP_END);   /* end kind if/else */
								if (rv != WASM_VOID) { if (rv == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "aot ic dreg"; goto done; } }
								wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* hit -> $after */
									wasm_op (&body, WASM_OP_END);   /* selected entry invalid/absent -> resolve */
								}
							}
vcall_cold_miss_emit:
							{
							extern int mono_wasm_jit_vcall_shared_miss_enabled;
							if (!mono_wasm_jit_vcall_shared_miss_enabled) {
							/* Legacy per-caller cold miss lowering, retained for an env-var A/B. */
							/* $scratch = imported s.b (see the direct-residual site above). HOT now that
							 * vcall_shared_miss=0 makes per-site cold-miss lowering the active path. */
#ifdef HOST_BROWSER
						wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);
#else
						wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ffb, (guint32) vtsi);
#endif
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
						wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_resolve_fslot, vtrfi);
#else
						wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ffa, (guint32) vtrfi); /* -> fslot */
#endif
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 208 /* scratch temp: f-slot */);
						/* if (fslot != 0) FAST: call_indirect straight into the override's wasm `f`; else call_interp. */
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);
						wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							/* FAST PATH: push this + args fresh from their vregs (GC-scanned shadow stack; resolve_fslot did
							 * not spill, so no GC-invisible window), then the f-slot table index, call_indirect(ftd). */
							for (ai = 0; ai < n2; ++ai)
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);            /* f-slot = table index */
							wj_emit_dynamic_call (&body, (guint32) ftdi);
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
									extern int mono_wasm_jit_vcall_aot_target (guint8 *scratch, MonoObject *this_obj, gpointer aic);
									WasmFuncType at, at_ne, aott; int ati = -1, ati_ne = -1, aotti = -1;
									if (n2 + 1 > WASM_FUNCTYPE_MAX_PARAMS) { fail = "vcall aot nparams"; goto done; }
									/* aott (i32,i32,i32)->i32 = the resolve helper (scratch, receiver, AOT IC), returning
									 * 0=residual / 1=AOT+rgctx / 2=AOT,no-extra-arg.
									 * at (this,params,i32 rgctx)->rv = AOT body WITH the trailing extra (rgctx/dummy) arg; at_ne
									 * (this,params)->rv = AOT body of an exempt wrapper kind WITHOUT it. The override is resolved at
									 * RUNTIME but the call_indirect functype is baked here, so emit BOTH variants and pick by the
									 * helper's return code (kind==2 -> at_ne). Keeps every AOT-backed vcall fast (no residual). */
									memset (&aott, 0, sizeof (aott)); aott.params [0] = WASM_I32; aott.params [1] = WASM_I32; aott.params [2] = WASM_I32; aott.nparams = 3; aott.ret = WASM_I32;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &aott)) { aotti = ti_base + vk; break; }
									if (aotti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = aott; aotti = ti_base + nextra++; }
									memset (&at, 0, sizeof (at)); for (vk = 0; vk < n2; ++vk) at.params [vk] = pp [vk]; at.params [n2] = WASM_I32; at.nparams = (guint32) (n2 + 1); at.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { ati = ti_base + vk; break; }
									if (ati < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at; ati = ti_base + nextra++; }
									memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { ati_ne = ti_base + vk; break; }
									if (ati_ne < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at_ne; ati_ne = ti_base + nextra++; }
									uses_calls = TRUE;
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $no_aot */
									/* Resolve and fill the compact AOT IC in C. This removes the former N-way
									 * unrolled empty-slot search and atomic publication sequence from every site. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall aot this ld"; goto done; }
									wasm_i32_const (&body, (gint32) (intptr_t) aic);
#ifdef HOST_BROWSER
									wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_aot_target, aotti);
#else
									wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff9, (guint32) aotti);
#endif
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_aotkind_idx);   /* stash kind, keep on stack */
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* kind==0 -> $no_aot -> residual */
									/* cppeh: bare AOT call — a throwing callee C++-unwinds natively to the nearest landing
									 * pad (an in-method catch or the interp e-thunk boundary). No try/catch wrapper. */
									/* Collapsed kind branch (mirror of the inline AOT-IC): args identical in both arms, so load
									 * them ONCE and pick the variant with a TYPED if-block (params = ati_ne = (this,args)->rv,
									 * guaranteed valid — bailed above otherwise). Only the rgctx push + functype differ. */
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall aot arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2);
									wasm_op (&body, WASM_OP_I32_EQ);
									wasm_op (&body, WASM_OP_IF); wj_emit_blocktype (&body, (guint32) ati_ne);   /* typed block in: (this,args) out: rv */
										/* no-extra-arg variant: AOT addr@212; call_indirect (this,args)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wj_emit_dynamic_call (&body, (guint32) ati_ne);
									wasm_op (&body, WASM_OP_ELSE);
										/* with-rgctx variant: rgctx@216, AOT addr@212; call_indirect (this,args,rgctx)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 216);   /* rgctx (ftndesc.arg) */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wj_emit_dynamic_call (&body, (guint32) ati);
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
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall arg ld"; goto done; }
								wasm_op (&body, sop); wasm_memarg (&body, al2, (guint32) (ai * 8));
							}
							/* INLINE DELEGATE RECIPE HIT. resolve_fslot/prepare_delegate_call published a
							 * thread-admitted target f-slot at +244 only for scalar-compatible recipes.
							 * Rewrite the Invoke ABI in linear memory, load the rewritten arguments, and
							 * enter the target f-thunk directly. This stays inside the caller's wasm/EH
							 * island and skips call_delegate -> invoke_caught -> e-thunk. A zero f-slot
							 * (cache miss, multicast, complex ABI, admission failure) takes the proven C
							 * helper below unchanged. */
							if (is_delegate_invoke) {
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_done */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 244);
								wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
									/* Capture shape, then consume-clear the recipe before entering managed
									 * code: a nested delegate call reuses this TLS scratch. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 220);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 220);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 244);
									wj_emit_fast_count (&body, WJC_FAST_DELEGATE);

									/* shape <= BOUND_STATIC: replace delegate `this` with the closed/bound
									 * target and retain the full signature. Otherwise drop delegate `this`
									 * with overlap-safe memory.copy and use ftd-with-param0-removed. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
									wasm_op (&body, WASM_OP_I32_LE_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 248);
										wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0);
										if (rv != WASM_VOID)
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										for (ai = 0; ai < n2; ++ai) {
											WasmOpcode lop; guint32 al2;
											switch (pp [ai]) {
											case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
											case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
											case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
											default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
											}
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
											wasm_op (&body, lop); wasm_memarg (&body, al2, (guint32) (ai * 8));
										}
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) ftdi);
										if (rv != WASM_VOID) {
											WasmOpcode sop; guint32 al2;
											switch (rv) {
											case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
											case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
											case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
											default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
											}
											wasm_op (&body, sop); wasm_memarg (&body, al2, 192);
										}
									wasm_op (&body, WASM_OP_ELSE);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_i32_const (&body, 8); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 236);
										wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHL);
										wasm_u8 (&body, 0xFC); wasm_uleb (&body, 10); wasm_u8 (&body, 0); wasm_u8 (&body, 0); /* memory.copy 0 0 */
										if (rv != WASM_VOID)
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										for (ai = 0; ai < n2 - 1; ++ai) {
											WasmOpcode lop; guint32 al2;
											switch (pp [ai + 1]) {
											case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
											case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
											case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
											default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
											}
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
											wasm_op (&body, lop); wasm_memarg (&body, al2, (guint32) (ai * 8));
										}
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) dftdi);
										if (rv != WASM_VOID) {
											WasmOpcode sop; guint32 al2;
											switch (rv) {
											case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
											case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
											case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
											default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
											}
											wasm_op (&body, sop); wasm_memarg (&body, al2, 192);
										}
									wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> $delegate_done */
								wasm_op (&body, WASM_OP_END);
							}
							/* DIAG (WASM_JIT_BADREF_ARG caller attribution): bake THIS (calling) method at scratch+224
							 * (mirrors the direct residual site; 224 is past ret(192)/target(200)/fslot(208)/aot(212,216)). */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
#ifdef HOST_BROWSER
							wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
#else
							wasm_i32_const (&body, 0x7ff8);
#endif
							wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 224 /* WJ_SCRATCH_CALLER_OFF */);
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 200);            /* target MonoMethod* */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* scratch buffer */
							/* Select the instance-aware post-spill delegate helper when resolve_fslot published
							 * a direct-target recipe at +220; otherwise retain the ordinary call_interp path. */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 220);
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
#ifdef HOST_BROWSER
								wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_delegate);
							wasm_op (&body, WASM_OP_ELSE);
								wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_interp);
#else
								wasm_i32_const (&body, 0x7ff7);
							wasm_op (&body, WASM_OP_ELSE);
								wasm_i32_const (&body, 0x7ffc);
#endif
							wasm_op (&body, WASM_OP_END);
							wj_emit_dynamic_call (&body, (guint32) vtdi);
							/* call_interp returns 1 if the vcall fallback threw. Continue the native unwind
							 * instead of reading the stale scratch result; mirrors the direct residual path. */
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
								/* threw: continue the native unwind; do NOT return and do NOT pop roots first. */
								EMIT_RESIDUAL_THROW_CONTINUATION ();
							wasm_op (&body, WASM_OP_END);
							if (is_delegate_invoke)
								wasm_op (&body, WASM_OP_END); /* $delegate_done */
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
								/* NB byref: see wasm_emit_subword_ret_norm — a byref ret is WASM_I32 and must
								 * not be masked/extended as its pointee type. */
								if (rv == WASM_I32 && !m_type_is_byref (csig->ret)) {
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
							} else {
								/* Signature-neutral shared miss ABI:
								 *   (this, base_method, frame, site_ic, aot_ic) -> threw
								 * Only the unavoidable typed stores/load remain in this caller. */
								WasmFuncType smt;
								WasmFuncType rlt;
								int smti = -1, rlti = -1;
								extern int mono_wasm_jit_vcall_shared_miss (MonoObject *, MonoMethod *, guint8 *, gpointer, gpointer);
								extern gpointer mono_wasm_jit_vcall_miss_frame_acquire (void);
								extern void mono_wasm_jit_vcall_miss_frame_release (gpointer);
								memset (&smt, 0, sizeof (smt));
								for (vk = 0; vk < 5; ++vk) smt.params [vk] = WASM_I32;
								smt.nparams = 5; smt.ret = WASM_I32;
								for (vk = 0; vk < nextra; ++vk)
									if (functype_eq (&extra_types [vk], &smt)) { smti = ti_base + vk; break; }
								if (smti < 0) {
									if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
									extra_types [nextra] = smt; smti = ti_base + nextra++;
								}
								memset (&rlt, 0, sizeof (rlt));
								rlt.params [0] = WASM_I32; rlt.nparams = 1; rlt.ret = WASM_VOID;
								for (vk = 0; vk < nextra; ++vk)
									if (functype_eq (&extra_types [vk], &rlt)) { rlti = ti_base + vk; break; }
								if (rlti < 0) {
									if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
									extra_types [nextra] = rlt; rlti = ti_base + nextra++;
								}
								/* The cold helper can C++/wasm-EH unwind during resolution or target entry.
								 * Import x.e even for an otherwise EH-free caller so a tiny cleanup catch can
								 * release the pinned frame and immediately rethrow the original exception. */
								uses_eh_tag = TRUE;
								if (eh_type_idx < 0)
									eh_type_idx = rlti; /* both are (i32)->void */
								uses_calls = TRUE;
								/* Acquire a GC-pinned, nesting-safe worker-local frame only on this cold edge.
								 * Unlike adding 256 bytes to naddrbytes, this has zero prologue/stack cost on hits. */
#ifdef HOST_BROWSER
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_miss_frame_acquire, vtsi);
#else
								wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff5, (guint32) vtsi);
#endif
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
								for (ai = 0; ai < n2; ++ai) {
									WasmOpcode sop; guint32 al2;
									switch (pp [ai]) {
									case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
									case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
									case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
									default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
									}
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "shared vcall arg ld"; goto done; }
									wasm_op (&body, sop); wasm_memarg (&body, al2, (guint32) (ai * 8));
								}
								/* Preserve caller attribution used by the residual diagnostics. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
#ifdef HOST_BROWSER
								wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
#else
								wasm_i32_const (&body, 0x7ff8);
#endif
								wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 224);
								wasm_op (&body, WASM_OP_TRY); wasm_u8 (&body, 0x40);
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "shared vcall this ld"; goto done; }
#ifdef HOST_BROWSER
								wasm_i32_const (&body, (gint32) (intptr_t) call->method);
#else
								wasm_i32_const (&body, 0x7ffd);
#endif
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_i32_const (&body, (gint32) (intptr_t) vic);
								wasm_i32_const (&body, (gint32) (intptr_t) aic);
#ifdef HOST_BROWSER
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_shared_miss, smti);
#else
								wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff6, (guint32) smti);
#endif
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx); /* threw */
								wasm_op (&body, WASM_OP_CATCH); wasm_uleb (&body, 0); /* x.e; exception ptr on stack */
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) eh_exc_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
#ifdef HOST_BROWSER
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_miss_frame_release, rlti);
#else
								wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff4, (guint32) rlti);
#endif
								wasm_op (&body, WASM_OP_RETHROW); wasm_uleb (&body, 0);
								wasm_op (&body, WASM_OP_END);
								/* Transfer a successful return into its GC-tracked destination before clearing
								 * the conservative frame root. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
								wasm_op (&body, WASM_OP_I32_EQZ);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
									if (rv != WASM_VOID) {
										WasmOpcode lop; guint32 al2;
										switch (rv) {
										case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
										case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
										case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
										default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
										}
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, lop); wasm_memarg (&body, al2, 192);
										if (rv == WASM_I32)
											wasm_emit_subword_ret_norm (&body, csig->ret);
										if (!wasm_st (&body, &lc, ins->dreg)) { fail = "shared vcall dreg"; goto done; }
									}
								wasm_op (&body, WASM_OP_END);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
#ifdef HOST_BROWSER
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_miss_frame_release, rlti);
#else
								wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff4, (guint32) rlti);
#endif
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
									/* threw: continue the native unwind; do NOT return and do NOT pop roots first. */
									EMIT_RESIDUAL_THROW_CONTINUATION ();
								wasm_op (&body, WASM_OP_END);
							}
							}
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
					if (pv == 0 || pv == WASM_VOID) {
						fail = "creg arg type";
						fail_sig_site = "callreg"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = ai;
						goto done;
					}
					if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "creg nargs"; goto done; }
					ct.params [ct.nparams++] = pv;
				}
				if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
				else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) {
					fail = "creg ret type";
					fail_sig_site = "callreg"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
					goto done;
				} }
				for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
				if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = ti_base + nextra++; }
				uses_calls = TRUE;
				nmeth = csig->param_count + (csig->hasthis ? 1 : 0);
				if (!call->call_info) { fail = "no captured callreg args"; goto done; }
				for (ai = 0; ai < nmeth; ++ai)
					if (!wasm_ld (&body, &lc, wj_arg_vreg (call, ai))) { fail = "creg arg ld"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "creg target ld"; goto done; }
					if (is_membase) { wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) ins->inst_offset); } /* target = *(base + offset) */
				wj_emit_dynamic_call (&body, (guint32) type_idx);
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
				/* Key on the WASM return type (ret_vt), NOT the managed one: a hidden-vret method has a
				 * non-void MANAGED return but a VOID wasm result — its value was already stored through
				 * the trailing vret pointer, so its exit is a plain void return. (Keying on the managed
				 * type made every vret method's exit emit `unreachable` -> RuntimeError on first return,
				 * e.g. JsonDocument StackRowStack:Pop.) */
				gboolean ret_loaded = ret_vt == WASM_VOID;
				if (!ret_loaded && cfg->ret) {
					ret_loaded = self_ci.ret.kind == WJ_ARG_VTYPE_SCALAR
						? wj_emit_scalar_vtype_arg (&body, &lc, sig->ret, cfg->ret->dreg)
						: wasm_ld (&body, &lc, cfg->ret->dreg);
				}
				if (!ret_loaded) {
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
				GOTO (bb->next_bb, 0);
			}
		}

		/* LCSE: hand this block's exit state to whichever single-predecessor successor inherits it. */
		if (lcse && lcse_exit && i < N && lcse_exit [i])
			*lcse_exit [i] = *lcse;
	}

	if (G_UNLIKELY (lcse != NULL)) {
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name))
			printf ("WASM_JIT_LCSE %s: adds=%d hits=%d inherited-nonempty-bbs=%d\n",
				cfg->method->name, lcse_adds, lcse_hits, lcse_inherits);
	}
	/* The table describes nothing outside the block loop; drop it before the shared raise sites and
	 * the epilogue so a stale entry cannot be consulted. */
	lcse = NULL;

	if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && structured_loop_l + 1 == N)
		wasm_op (&body, WASM_OP_END);          /* loop reaches the method's final bb */
	if (!skip_dispatch) {
		wasm_op (&body, WASM_OP_END);          /* close loop */
		wasm_op (&body, WASM_OP_UNREACHABLE);  /* loop never falls through */
	}

	/* Shared raise sites, innermost first: close throw[t], then emit its one call to
	 * mono_wasm_jit_raise_corlib. Only reachable via `br_if $throw_t`, since the dispatch loop above
	 * never falls through and every raise ends in `unreachable`.
	 *
	 * Each block materializes the lazy ref frame first. mono_wasm_jit_raise_corlib ALLOCATES (the
	 * exception object, its stack trace), so a collection can happen here, and with lazy_ref_frame the
	 * method's roots may still be nowhere but unscannable wasm locals -- exactly the MONO_WASM_JIT_RAISE_NOGC
	 * corruption documented at wj_ins_is_gcpoint. This is the same shape as the conditional safepoint
	 * poll: materialize inside the rare taken arm, leaving the hot not-taken path frame-free. No
	 * RELEASE_LAZY_REF_FRAME follows, because the raise never returns; the C-stack SP is resynced from
	 * the boundary snapshot in mono_wasm_jit_invoke_caught's thrown path. */
	if (nthrow > 0) {
		WasmFuncType st; int sti = -1, sk, eid;
		memset (&st, 0, sizeof (st)); st.params [0] = WASM_I32; st.nparams = 1; st.ret = WASM_VOID;
		for (sk = 0; sk < nextra; ++sk) if (functype_eq (&extra_types [sk], &st)) { sti = ti_base + sk; break; }
		if (sti < 0) {
			if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
			extra_types [nextra] = st; sti = ti_base + nextra++;
		}
		uses_calls = TRUE;
		for (i = 0; i < nthrow; ++i) {
			eid = -1;
			for (sk = 0; sk < WJ_EXC_IDS; ++sk) if (wj_throw_slot [sk] == i) { eid = sk; break; }
			if (eid < 0) { fail = "throw slot without exc id"; goto done; }
			wasm_op (&body, WASM_OP_END);   /* close throw[i]; br_if $throw_i lands here */
			RAISE_ENSURE_REF_FRAME ();
			wasm_i32_const (&body, eid);
#ifdef HOST_BROWSER
			{ extern void mono_wasm_jit_raise_corlib (int exc_id); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_raise_corlib, sti); }
#else
			wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff8, (guint32) sti);
#endif
			wasm_op (&body, WASM_OP_UNREACHABLE);   /* raise_corlib C++-throws; never returns */
		}
	}

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
		{ extern int mono_wasm_jit_eh_dispatch (WasmEhTable *t, int blk); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_eh_dispatch, eh_dispatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff1, (guint32) eh_dispatch_ti);
#endif
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) eh_h_idx);
		wasm_i32_const (&body, 0);
		wasm_op (&body, WASM_OP_I32_LT_S);
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);     /* h < 0: no local handler */
		/* the method escapes via rethrow -> pop its il_state island first */
#ifdef HOST_BROWSER
		{ extern void mono_wasm_jit_leave_island (void); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_leave_island, eh_endcatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff7, (guint32) eh_endcatch_ti);   /* leave_island ()->void */
#endif
		wasm_op (&body, WASM_OP_RETHROW); wasm_uleb (&body, 1); /* depth 1 = the enclosing try -> re-propagate */
		wasm_op (&body, WASM_OP_END);                          /* end if */
		/* matched: claim+release the C++ exception (balance the cxa handler count), then dispatch. */
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_exc_idx);
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_begin_catch (void *e); wj_emit_helper_call (&body, (gpointer) mono_jiterp_begin_catch, eh_type_idx); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff2, (guint32) eh_type_idx);   /* (i32)->void */
#endif
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_end_catch (void); wj_emit_helper_call (&body, (gpointer) mono_jiterp_end_catch, eh_endcatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff3, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
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
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
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
#undef WJ_HAS_THROW
#undef WJ_THROW_BR
#undef GOTO_IS_FWD
#undef COND_BRANCH
#undef NN_OK
#undef NN_GET
#undef NN_SET
#undef NN_KILL
#undef BIN
#undef BINI32
#undef BINI64
#undef BINI64L

	/* entry thunk: (args_ptr, ret_ptr)->void — reads each arg from the interp stack at
	 * args_ptr + its interp arg offset (mono_wasm_jit_sig_arg_offsets: one 8-byte stackval per
	 * scalar arg, so all-scalar sigs load at exactly i*8), calls the method (func 0), stores the
	 * result at ret_ptr. Lets the interpreter invoke any signature uniformly via e(args, ret). */
	{
		WasmBuf ethunk;
		gboolean has_ret = (ret_vt != WASM_VOID);
		guint32 aoffs [WASM_FUNCTYPE_MAX_PARAMS];
		extern int mono_wasm_jit_sig_arg_offsets (MonoMethodSignature *asig, guint32 *offs, int max);
		if (mono_wasm_jit_sig_arg_offsets (sig, aoffs, WASM_FUNCTYPE_MAX_PARAMS) != nargs) { fail = "ethunk arg offsets"; goto done; }
		wasm_buf_init (&ethunk);
		wasm_buf_init_relocs (&ethunk);
		if (has_ret)
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 1); /* ret_ptr (store address) */
		for (i = 0; i < nargs; ++i) {
			WasmOpcode ld; guint32 al;
			if (self_ci.args [i].kind == WJ_ARG_VTYPE_BYADDR) {
				/* pass the ADDRESS of the struct stored inline in the interp args area (args_ptr + off).
				 * The area is GC-scanned (interp stack) and callee-owned after the call, so the callee
				 * reading or mutating through the pointer is byval-correct with no copy. */
				wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 0); /* args_ptr */
				if (aoffs [i]) { wasm_i32_const (&ethunk, (gint32) aoffs [i]); wasm_op (&ethunk, WASM_OP_I32_ADD); }
				continue;
			}
			switch (param_types [i]) {
			case WASM_I64: ld = WASM_OP_I64_LOAD; al = 3; break;
			case WASM_F32: ld = WASM_OP_F32_LOAD; al = 2; break;
			case WASM_F64: ld = WASM_OP_F64_LOAD; al = 3; break;
			default:       ld = WASM_OP_I32_LOAD; al = 2; break;
			}
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 0); /* args_ptr */
			wasm_op (&ethunk, ld);
			wasm_memarg (&ethunk, al, aoffs [i]);
		}
		if (self_ci.vret_byaddr)
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 1); /* trailing vret param = ret_ptr: the interp stores VT returns inline at the retval slot, exactly where the callee writes */
		/* The thunk calls its own method. That function index is (this module's import count + this
		 * member's index) and NEITHER is known here -- a later member can still intern an import that
		 * shifts every defined function up. It used to be a 5-byte padded ULEB plus a recorded offset for
		 * batch_finish to patch; it is now simply a hole the assembler fills, which is the same trick with
		 * none of the bookkeeping. `tpool` 1 is the thunk's own signature, unused for a SELF reloc but
		 * kept well-formed. */
		wasm_reloc (&ethunk, WASM_RELOC_SELF, 1, 0, NULL);
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
#ifdef HOST_BROWSER
		if (wj_batch_slot >= 0) {
			/* Batching: hand the pieces to the batch instead of framing a module. The driver frames all
			 * members together once every one has been emitted, so nothing is instantiated or registered
			 * here — `out` stays empty and the caller sees no e_slot, which the driver expects. */
			WjBatchMember *bm = &wj_batch->m [wj_batch_slot];
			bm->method = cfg->method;
			g_assert ((guint32) nwparams <= WASM_FUNCTYPE_MAX_PARAMS);
			memcpy (bm->param_types, param_types, sizeof (WasmValtype) * nwparams);
			bm->nparams = (guint32) nwparams;
			bm->ret_type = ret_vt;
			memcpy (bm->groups, groups, sizeof (WasmLocalGroup) * 4);
			g_assert (nextra <= WJ_EXTRA_TYPES_MAX);
			bm->extra_types = nextra ? (WasmFuncType *) g_malloc (sizeof (WasmFuncType) * nextra) : NULL;
			if (nextra)
				memcpy (bm->extra_types, extra_types, sizeof (WasmFuncType) * nextra);
			bm->nextra = (guint32) nextra;
			bm->ndeps = cfg->wasm_jit_result.ndirect_deps;
			if (bm->ndeps > 0) {
				bm->deps = g_new (int, bm->ndeps);
				bm->dep_sig = g_new (guint32, bm->ndeps);
				bm->dep_method = g_new0 (MonoMethod *, bm->ndeps);
				memcpy (bm->deps, cfg->wasm_jit_result.direct_deps, sizeof (int) * bm->ndeps);
				memcpy (bm->dep_sig, cfg->wasm_jit_result.direct_dep_sig, sizeof (guint32) * bm->ndeps);
				memcpy (bm->dep_method, cfg->wasm_jit_result.direct_dep_method, sizeof (MonoMethod *) * bm->ndeps);
			}
			/* Take ownership of both buffers -- their relocation lists included -- because they must
			 * outlive this frame. They are the member now: nothing else about this compile is kept. */
			bm->f_body = body;
			bm->e_body = ethunk;
			memset (&body, 0, sizeof (body));
			memset (&ethunk, 0, sizeof (ethunk));
			bm->uses_calls = uses_calls ? 1 : 0;
			bm->uses_eh_tag = uses_eh_tag ? 1 : 0;
			bm->eh_tpool = eh_type_idx;
			bm->captured = TRUE;
			wj_batch->in_member = FALSE;
			wj_batch->n++;
			wj_batch->cur = -1;
			wasm_buf_init (&out);
			goto done;
		}
#endif
		{
			/* A lone method is a one-member module, framed by the same assembler a batch uses. Import
			 * slots are assigned as the holes are walked -- method body first, then entry thunk, in
			 * emission order -- which is exactly the order the pre-relocation emitter assigned them in,
			 * so the bytes are unchanged. */
			WjAsmMember am;
			WjAsmPolicy pol;
			memset (&am, 0, sizeof (am));
			memset (&wj_self_imports, 0, sizeof (wj_self_imports));
			am.method = cfg->method;
			/* The target of the cycle test. Zero when this method has neither a live slot nor a
			 * reservation, which means its pair is allocated below, after framing -- see wj_asm_reaches
			 * for why a slot that does not exist yet is safe rather than unknown.
			 *
			 * HOST_BROWSER only, and not merely because there is nothing to import offline: both of these
			 * live in the interpreter (transform.c), which mono-aot-cross does not link. csyn.sh compiles
			 * this file for both targets and still cannot see that -- it is a LINK error, and it is the
			 * one class of breakage a per-file check cannot catch. */
#ifdef HOST_BROWSER
			{ extern int mono_wasm_jit_self_reserved (MonoMethod *m, int *e_out, int *f_out);
			  extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
			  int se = 0, sf = 0;
			  if (!mono_wasm_jit_self_reserved (cfg->method, &se, &sf))
				sf = mono_wasm_jit_get_callee_fslot (cfg->method);
			  am.f_slot = sf > 0 ? sf : 0; }
#endif
			am.f_body = &body;
			am.e_body = &ethunk;
			am.param_types = param_types;
			am.nparams = (guint32) nwparams;
			am.ret_type = ret_vt;
			am.extra_types = extra_types;
			am.nextra = (guint32) nextra;
			am.groups = groups;
			am.nlocal_groups = 4;
			am.uses_calls = uses_calls ? 1 : 0;
			am.uses_eh_tag = uses_eh_tag ? 1 : 0;
			am.eh_tpool = eh_type_idx;
			wj_asm_policy_init (&pol, FALSE /* keep self-recursion indirect, as it has always been */);
			wasm_buf_init (&out);
			wj_assemble (&am, 1, &pol, &out, NULL, &wj_self_imports);
			/* Keep the relocatable body. Framing consumed nothing -- the buffers still hold instruction
			 * bytes and relocations, unresolved -- so this method can later be re-framed alongside others
			 * without being compiled again. Attached to the registry on success below; freed at `done`
			 * if registration never happens. */
			wj_stored_body = wj_body_take (cfg->method, &body, &ethunk, param_types, (guint32) nwparams,
			                               ret_vt, groups, extra_types, (guint32) nextra,
			                               uses_calls, uses_eh_tag, eh_type_idx);
		}
		wasm_buf_free (&ethunk);
	}

#ifdef HOST_BROWSER
	{
		/* Live single-method hex dump, selected by full-name substring (MONO_WASM_JIT_HEX_METHOD).
		 *
		 * DUMP_ONLY cannot answer "what does the failing run actually execute". It suppresses registration
		 * for EVERY method, so no callee owns an f-slot and the emitter takes different direct-call and
		 * devirt decisions -- the bytes it prints are a different module than the live one. Filtering by
		 * name leaves the rest of the process on its normal path, so the dumped module IS the one that
		 * runs. */
		const char *want = g_getenv ("MONO_WASM_JIT_HEX_METHOD");
		if (want && *want && mname && strstr (mname, want)) {
			GString *hex = g_string_sized_new (out.len * 2 + 1);
			for (i = 0; i < (int) out.len; ++i)
				g_string_append_printf (hex, "%02x", out.data [i]);
			printf ("WASM_JIT_LIVEHEX %s : %u : %s\n", mname, out.len, hex->str);
			g_string_free (hex, TRUE);
		}
	}
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
		 * reserved these and publishes us); (2) this method's own self-recursion reservation, which
		 * mono_wasm_jit_self_reserved also returns (it now lives on the imethod, so it survives a failed emit
		 * and a re-emit reuses it); (3) fresh allocation. Instantiate INTO the reserved pair so baked
		 * self/cross-cycle calls resolve. */
		int e_slot = 0, f_slot = 0;
		{ extern int mono_wasm_jit_self_reserved (MonoMethod *m, int *e_out, int *f_out);
		  if (!mono_wasm_jit_self_reserved (cfg->method, &e_slot, &f_slot)) {
			e_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			f_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			/* Exhaustion is terminal for the JIT and otherwise invisible — the guard below just declines to
			 * register and the tier quietly stops. Count it so mono_wasm_jit_liveness(3) shows it. */
			if (e_slot <= 0 || f_slot <= 0) {
				extern void mono_wasm_jit_note_table_exhausted (void);
				mono_wasm_jit_note_table_exhausted ();
			}
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
				if (G_UNLIKELY (mono_wasm_jit_stats)) {
					mono_wasm_jit_count (WJC_REGISTERED); mono_wasm_jit_add (WJC_BYTES_GENERATED, (gint64) out.len);
					if (self_has_byaddr) mono_wasm_jit_count (WJC_VT_BYADDR_METHODS);
					if (self_ci.vret_byaddr) mono_wasm_jit_count (WJC_VRET_METHODS);
				}
				{ extern int mono_wasm_jit_register (MonoMethod *, int, int, void *, int, guint32, gboolean, const int *, const guint32 *, MonoMethod *const *, int, const int *, int);
				  /* the fingerprint is the self-sig WasmCallInfo's — the same descriptor call sites hash
				   * into dep_sig, so caller/callee can never disagree structurally */
				  cfg->wasm_jit_result.f_sig_id = self_ci.f_sig_id;
				  cfg->wasm_jit_result.desc_id = mono_wasm_jit_register (cfg->method, e_slot, f_slot, cached, (int) out.len,
					cfg->wasm_jit_result.f_sig_id, method_nogc, cfg->wasm_jit_result.direct_deps,
					cfg->wasm_jit_result.direct_dep_sig, cfg->wasm_jit_result.direct_dep_method, cfg->wasm_jit_result.ndirect_deps,
					wj_self_imports.slot, wj_self_imports.n); }
				if (cfg->wasm_jit_result.desc_id <= 0) {
					g_free (cached);
					cached = NULL;
				} else {
					{ WjRegEntry *sre = wj_reg_at (cfg->wasm_jit_result.desc_id - 1);
					  if (sre) { sre->body = wj_stored_body; wj_stored_body = NULL; } }
					if (mono_wasm_jit_verbose >= 1) {
						printf ("WASM_JIT_REGISTERED %s desc=%d e_slot=%d f_slot=%d len=%u deps=%d [", mname, cfg->wasm_jit_result.desc_id, e_slot, f_slot, (unsigned) out.len, cfg->wasm_jit_result.ndirect_deps);
						for (i = 0; i < cfg->wasm_jit_result.ndirect_deps; ++i)
							printf ("%s%d/0x%x", i ? "," : "", cfg->wasm_jit_result.direct_deps [i], cfg->wasm_jit_result.direct_dep_sig [i]);
						printf ("]\n");
					}
				}
				/* Write the success result onto cfg (read back by mono_wasm_force_compile after the compile
				 * returns, on this same thread). e_slot is the >0 success gate the readers test; no cross-thread
				 * barrier needed here — the result is consumed on this thread via cfg, and the cross-thread
				 * publish to InterpMethod.wasm_jit_slot keeps its own barrier in wasm_jit_compile_publish. */
				if (cached) {
					cfg->wasm_jit_result.bytes = cached;
					cfg->wasm_jit_result.bytes_len = (int) out.len;
					cfg->wasm_jit_result.f_slot = f_slot;
					cfg->wasm_jit_result.e_slot = e_slot;
				}
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
	g_free (wj_self_imports.slot);
	memset (&wj_self_imports, 0, sizeof (wj_self_imports));
	wj_body_free (wj_stored_body);   /* NULL once the registry has taken it */
	wj_stored_body = NULL;
	wasm_buf_free (&body);
#ifdef HOST_BROWSER
	/* This frame claimed a member slot but did not reach the capture (it bailed): release the claim so
	 * the next member can join. Guarded on wj_batch_slot so a nested standalone emit cannot clear the
	 * claim its parent still holds. */
	if (wj_batch_slot >= 0 && wj_batch)
		wj_batch->in_member = FALSE;
#endif
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
	/* -12 vs -8: an rgctx CALLSITE bail (this concrete method makes an indirect/virtual call that carries
	 * MONO_ARCH_RGCTX_REG — fixable per-site by routing that one call through the residual) is a different
	 * workstream from the whole-method gshared gate (the method ITSELF is generic-shared). The old lumped -8
	 * hid which one carries the ~800K weighted vperm fallbacks. */
	else if (strstr (fail, "rgctx")) cfg->wasm_jit_result.bail = -12;                                  /* rgctx callsite (indirect/virtual call carrying the callsite rgctx) */
	else if (strstr (fail, "gshared")) cfg->wasm_jit_result.bail = -8;                                  /* generic-shared method (whole-method gate) */
	else if (strstr (fail, "synchronized")) cfg->wasm_jit_result.bail = -9;                             /* synchronized method/wrapper */
	else if (strstr (fail, "EH") || strstr (fail, "finally") || strstr (fail, "eh ") || strstr (fail, "eh-")) cfg->wasm_jit_result.bail = -10; /* other EH reasons (not the -2 clause gate) */
	else cfg->wasm_jit_result.bail = -4;                                   /* genuinely other (unsupported IR shape: reg/move/sig/indirect/...) */
	cfg->wasm_jit_result.fail_reason = fail;   /* static literal (or NULL) — names the exact gate in the weighted vperm dump */
	/* No on-fail clear needed: the success fields (e_slot/f_slot/bytes) are only written on the
	 * instantiate-success path above, and this cfg is private to this compile — a nested re-entrant
	 * compile has its own cfg and cannot have set them here (the old per-thread-relay clobber is gone). */
#endif
	if (fail) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) {
			int cat = -1;
			mono_wasm_jit_count (WJC_BAILED);
			/* Aggregated bail histogram (Part 4). Check opcode then "residual:" (the rm 2-5 shape bisection)
			 * BEFORE the generic "callee not jitted" (which the shape strings also contain). */
			if (fail_op >= 0) { cat = WJB_OPCODE; if (fail_op < WJ_BAIL_OPMAX) wj_bail_op_hist [fail_op]++; }
			else if (strstr (fail, "residual:")) cat = WJB_RESIDUAL_SHAPE;
			/* "callee not jitted" is NOT counted here: it is retriable and the island driver re-emits the
			 * same method every iteration until its callees JIT, so a per-attempt count mostly measures
			 * island convergence (profile4: 3066 of 7040 "bails" were these re-attempts). It is counted
			 * once per DISTINCT method at wasm_jit_compile_publish (mono_wasm_jit_bail_hist_note_blocked),
			 * so the histogram reflects terminal outcomes. */
			else if (strstr (fail, "callee not jitted")) cat = -1;
			else if (strstr (fail, "EH clauses")) cat = WJB_EH_CLAUSE;
			else if (strstr (fail, "arg type") || strstr (fail, "ret type")) cat = WJB_ARGRET_TYPE;
			else cat = WJB_SYNC_OTHER;
			if (cat >= 0)
				wj_bail_hist [cat]++;
		}
		if (mono_wasm_jit_verbose >= 2) {
			/* Name the FIRST recorded blocker + the blocker count. A retriable "callee not jitted" bail with
			 * nblk=0 is the smoking gun for the retry storm: compile_publish can't block_note anything, so
			 * force_island returns BUSY -> SLOT_RETRY -> re-attempt every threshold (never parks). */
			int _nblk = cfg->wasm_jit_result.nblockers;
			char *_cn = _nblk > 0 ? mono_method_get_full_name (cfg->wasm_jit_result.blockers [0]) : NULL;
			printf ("WASM_JIT_BAIL %s : %s (op=%d %s) [nblk=%d%s%s]", mname, fail, fail_op,
				fail_op >= 0 ? wj_opname (fail_op) : "-", _nblk,
				_cn ? " blocker=" : "", _cn ? _cn : "");
			if (fail_sig_site)
				wj_print_bail_sig (fail_sig_site, fail_sig_callee, fail_sig, fail_sig_arg);
			printf ("\n");
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
 * mono_arch_emit_call (an unimplemented stub here). It builds the WjCallArgs descriptor the emitter
 * reads its arguments from; see the comment on WjCallArgs above for why that descriptor holds
 * INSTRUCTIONS rather than vreg numbers.
 *
 * Scalar/ref/byref args get a real OP_*MOVE carrier into a fresh vreg, added to cfg->cbb and
 * registered in call->out_ireg_args -- LLVM's mechanism, and it is what makes these immune to the
 * pipeline:
 *   - SSA renaming only renumbers VARIABLES (get_vreg_to_inst != NULL) and multiply-defined local
 *     vregs; a carrier dreg is a fresh, singly-defined, non-variable vreg, so it is neither.
 *   - mono_ssa_deadce only nullifies defs of cfg->varinfo entries, so it cannot reach a carrier --
 *     while the carrier's sreg1 IS a recorded use, which is what keeps the SOURCE alive.
 *   - mono_local_deadce (local-propagation.c) and alias analysis (kill_call_arg_alias) walk
 *     out_ireg_args explicitly.
 * A ref arg's carrier dreg is classified ref by the fixpoint's OP_MOVE taint, costing one shadow
 * slot per ref arg per call site -- the price of rooting the argument across the call.
 *
 * Vtype args get NO carrier of their own: OP_VMOVE would need a fresh vtype VARIABLE, which is
 * exactly the renameable thing we are trying to avoid, and OP_LLVM_OUTARG_VT has no non-LLVM
 * decompose case. Instead the source variable is marked MONO_INST_INDIRECT, which takes it out of
 * SSA entirely (create_new_vars and mono_ssa_remove's coalescer both skip VOLATILE|INDIRECT vars),
 * so recording the source instruction is enough. That is also honest about the ABI: WJ_ARG_VTYPE_BYADDR
 * passes these BY ADDRESS out of an addr-frame slot, so their address is genuinely taken. Java (and
 * therefore IKVM) has no byval structs, so the lost optimization costs nothing on the target workload.
 */
void
mono_wasm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoMethodSignature *sig = call->signature;
	int n = sig->param_count + sig->hasthis, i;
	WjCallArgs *w = (WjCallArgs *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjCallArgs));

	w->nargs = n;
	w->carrier = (MonoInst **) mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst *) * (n + 1));
	w->capvreg = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (n + 1));
	w->copyoff = (gint32 *) mono_mempool_alloc (cfg->mempool, sizeof (gint32) * (n > 0 ? n : 1));
	for (i = 0; i <= n; ++i)
		w->capvreg [i] = -1;
	for (i = 0; i < n; ++i)
		w->copyoff [i] = -1;

	for (i = 0; i < n; ++i) {
		MonoInst *in = call->args [i];
		MonoInst *ins;
		MonoType *t = (sig->hasthis && i == 0) ? mono_get_int_type () : sig->params [i - sig->hasthis];
		guint32 opcode = mono_type_to_regmove (cfg, t);

		if (opcode == OP_VMOVE || opcode == OP_XMOVE) {
			/* vtype: no carrier — pin the source variable out of SSA instead (see above) */
			MonoInst *var = get_vreg_to_inst (cfg, in->dreg);
			if (var)
				var->flags |= MONO_INST_INDIRECT;
			w->carrier [i] = in;
			w->capvreg [i] = in->dreg;
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
		/* always the ireg list (hreg 0), even for f/l moves — the list is only a liveness hint for
		 * mono_local_deadce/alias analysis here, not a register assignment */
		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, 0, FALSE);
		w->carrier [i] = ins;
		w->capvreg [i] = ins->dreg;
	}
	/* vtype return: capture the hidden-vret ADDRESS. calls.c created call->vret_var as a real
	 * OP_OUTARG_VTRETADDR instruction in cfg->cbb (decompose later rewrites it in place into an
	 * OP_LDADDR of the ret temp, keeping the same MonoInst and dreg), so it is a carrier like any
	 * other. The emitter pushes it as the trailing vret param on OP_VCALL2 sites. */
	if (call->vret_var && mini_type_is_vtype (mini_get_underlying_type (sig->ret))) {
		w->carrier [n] = call->vret_var;
		w->capvreg [n] = call->vret_var->dreg;
		mono_call_inst_add_outarg_reg (cfg, call, call->vret_var->dreg, 0, FALSE);
	}
	call->call_info = (CallInfo *) w;
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
