/* Runtime wasm-JIT diagnostics and bounded observation buffers. Included by mini-wasm.c. */
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

/* Stack-headroom probe state; defined here rather than beside wj_stack_probe because
 * mono_wasm_jit_dump_stats (just below) prints them and comes first in the file. */
static gint32 wj_stack_min_headroom = G_MAXINT32;  /* worst headroom seen, bytes */
static gint32 wj_stack_total_seen = 0;             /* base - end on the thread that hit the worst case */
static gint32 wj_stack_probe_hits = 0;

#define WJC_(i) ((long long) mono_wasm_jit_counters [i])

/* A counter that is paid for on the hot path and then never read is waste, so a new WJC_* entry needs a
 * reader. This assert is the tripwire: appending to the enum breaks the build until you have bumped it,
 * which is the prompt to add the counter to this function and to a mirror.
 *
 * THERE ARE TWO MIRRORS AND THEY ARE NOT EQUALLY CURRENT -- the older wording here claimed every counter
 * appears in both, and that has not been true for a long time. Measured 2026-09-12:
 *   scratchpad/wj/lib/mcdrive.mjs   -- the measurement driver's subset, kept in lockstep, and the one
 *                                      that matters for an A/B. Use scratchpad/wj/wjcsync.py after any
 *                                      MID-ENUM deletion; appending needs no run.
 *   ikvmcraft frontend/src/dotnet/jitbench.ts, `const WJ` -- the in-page bench UI. Names 146 of the 192
 *                                      entries, highest index 155, with a hole at 67..77, so everything
 *                                      added since R245 is absent from it. Its WJ_MAX is DERIVED from
 *                                      the table, so the dump simply stops at 156 rather than lying.
 * Adding to jitbench.ts is optional and app-side; adding to mcdrive.mjs is not optional if the counter
 * is meant to settle a measurement. */
/* Depth at which the admission DFS is counted as running deep. Defined here rather than next to the walk
 * because the stats dump below prints it. */
#define WJ_ADMIT_DEPTH_WARN 64

g_static_assert (WJC_MAX == 260);   /* + replacement give-ups, + the retire/reclaim census */

/* Census counters — see the long note above mono_wasm_jit_liveness.
 *
 * THE GATE IS SPLIT, AND THE SPLIT IS THE POINT. The INSTANTIATION half below is unconditional; only
 * the ENTRY half (wj_census_entered_total, via mono_wasm_jit_census_note_entry) is still behind
 * MONO_WASM_JIT_ENTRYCENSUS, because that one costs a load+test at the interp->JIT boundary and so
 * cannot be on while timing. Instantiation is counted at the two instantiate_*_local choke points,
 * which run tens of thousands of times per run against a call that costs 29.4 us + 15.4 ns/byte
 * (R147) -- an atomic add there is unmeasurable, and it buys the one number this tree could not
 * previously read at all.
 *
 * Why that matters: WJC_ELAPSED_INSTANTIATION, which the `[wasm-jit time]` line prints as
 * `instantiate=`, is added at exactly ONE site (the compiling thread's validating instantiation, at
 * the bottom of mono_wasm_emit_method). Every per-worker instantiation done by admission -- i.e. the
 * entire cost this backend actually pays for being per-worker -- was charged here instead, behind a
 * knob that the measurement discipline says to leave off. So the per-worker instantiation bill had
 * never been measured on a shipped configuration. It is fields 8/10/13 of mono_wasm_jit_liveness,
 * which is documented as an always-on probe and now actually is one.
 *
 * FIRST READING, and it is why this was worth doing. 2026-09-12, wjbuild-p0-instcensus, full Minecraft
 * (worldwait, 1 run, --stats --dumps, boot + world.generate 95.3 s in its STALL mode + a 20 s in-game
 * window), read from the closing dumpWasmJit:
 *     registered      31,374 methods
 *     modules         17,249 distinct  (depcycles.py contracted nodes on the same run's WJ_GRAPH)
 *     instantiations  134,452          = 7.79 per MODULE, 4.29 per registered method
 *     inst_compile     4,908.4 ms      (the number this line used to print alone)
 *     inst_all        16,152.5 ms      = 3.29x that
 * So a module is instantiated on ~7.8 workers, and ~11.2 s per run of instantiation was invisible.
 * Single run, one workload, no spread -- it sizes the pool, it does not settle a comparison.
 *
 * PROCESS-WIDE ATOMICS, not just thread-locals, and that is forced rather than preferred: the worker
 * isolates whose numbers we want are pthreads parked in synchronous wasm, so CDP Runtime.evaluate never
 * returns for them (33 of 34 worker targets read "unreachable"). The same blocking is why the unlanded
 * SHARE_MODULES postMessage relay cannot work without JSPI suspends. Aggregating in C and reading the
 * total from the page target is the only reliable channel. These live in the wasm heap, which pthreads
 * share, so any thread's increment is visible.
 *
 * This used to be split by PATH -- an eager per-thread sweep (sync_thread), a direct-call backstop
 * (instantiate_fslot), and admit. BOTH OF THE FIRST TWO HAD NO CALLERS and have been deleted, so those
 * splits were structurally zero and the split itself was the finding: admit is the only path that ever
 * instantiates on another worker. A first attempt instrumented only sync_thread and read a flat zero at
 * 11,069 registered methods -- read at the time as "nothing is duplicated", when it actually meant the
 * instrumented path was dead. Hooking the two instantiate_*_local choke points is what made the total
 * unmissable, and it remains the right place. */
static gint32 wj_census_inst_total;   /* every successful instantiation, any path, any thread */
static gint32 wj_census_inst_admit;   /* via mono_wasm_jit_admit (per-worker, on demand at dispatch) */
static gint32 wj_census_entered_total;/* distinct (thread, e-slot) pairs actually entered */
/* gint32 MICROSECONDS, summed across every thread, and now accumulating unconditionally rather than only
 * under a knob -- so the overflow bound is worth stating once: 2^31 us is ~35.8 minutes of CUMULATIVE
 * cross-thread instantiation. A full measured run spends ~16 s (see the reading above), so the headroom is
 * ~130x and a session would have to be extraordinary to reach it. Widen this to gint64 before trusting it
 * on a soak; mono_wasm_jit_liveness returns int and would need a millisecond field either way. */
static gint32 wj_census_inst_us_total;/* microseconds spent instantiating, all threads */

/* One successful module instantiation on this thread, `ms` being the WebAssembly.Module+Instance time
 * the EM_ASM measured. Unconditional -- see the gate-split note above. A batched module counts ONCE,
 * not once per member: the WebAssembly.Instance and its export JSFunctions are what a batch amortizes,
 * and counting n here would hide exactly the win batching is supposed to deliver. */
#define WJ_CENSUS_NOTE_INSTANTIATE(ms) do {                                             \
		mono_atomic_inc_i32 (&wj_census_inst_total);                            \
		mono_atomic_add_i32 (&wj_census_inst_us_total, (gint32) ((ms) * 1000.0)); \
	} while (0)

/* GC-SAFE AROUND THE JS CALL. `new WebAssembly.Module` + `new WebAssembly.Instance` is a long synchronous
 * call with no safepoint in it, and until now it ran GC-UNSAFE -- so a worker inside one could not answer a
 * suspend request, and a fleet of them doing it at once could not answer at all. That is not hypothetical:
 * it is `WAITING for 1 threads, got 0 suspended` / `suspend_thread suspend took 20001 ms` and 90 s of
 * silent boot (R162 addendum, recorded at wj_batch_rollback). Under FULL cooperative suspend -- which this
 * build uses, ENABLE_COOP_SUSPEND=1 in the generated config.h, asserted by mono_threads_suspend_init -- a
 * thread in STATE_BLOCKING is treated as ALREADY SUSPENDED at zero rendezvous cost and only blocks on its
 * way back out. Which is exactly the shape wanted for a 29.4 us + 15.4 ns/byte call.
 *
 * The region covers the WHOLE EM_ASM so both wasmTable.set calls land inside one blocking window; the
 * slot-installed marking and the census stay outside, after.
 *
 * SUSPENDABILITY THIS ADMITS, DELIBERATELY. The thread becomes suspendable while wj_desc_state[desc] == 1
 * (visiting) and possibly while holding the wj_compiling CAS. Both were already true -- a compile runs
 * cctors, which interpret, which poll -- and all admission state is __thread, so a suspension here is
 * observationally identical to one at a poll inside the same DFS. wj_compiling is a NON-BLOCKING CAS and
 * every other compiler try-locks, so nothing can wait on it. No caller holds the loader lock across the
 * call either: mono_wasm_emit_method instantiates BEFORE mono_wasm_jit_register takes it, and both
 * mono_wasm_jit_admit and mono_wasm_jit_rebatch are lock-free here. So this strictly removes a reason to
 * block STW and adds no lock-ordering edge.
 *
 * WHY THE STATE TEST RATHER THAN A BARE MONO_ENTER_GC_SAFE. DO_BLOCKING is legal only from STATE_RUNNING
 * and STATE_ASYNC_SUSPEND_REQUESTED (mono-threads-state-machine.c: "Blocking is not nestable"); every
 * other state is mono_fatal_with_history, a hard abort, and one MONO forced abort wedges a whole batch.
 * All four call sites are reached GC-unsafe today and no JS export reaches them, so the else arm should
 * never run -- WJC_JS_BLOCKING_SKIPPED exists to say so out loud rather than to be trusted silently. */
#define WJ_JS_BLOCKING_BEGIN(cookie, sd, entered)                                       \
	MONO_STACKDATA (sd);                                                            \
	gpointer cookie = NULL;                                                         \
	gboolean entered = FALSE;                                                       \
	do {                                                                            \
		MonoThreadInfo *__wj_ti = mono_thread_info_current_unchecked ();        \
		int __wj_st = __wj_ti ? mono_thread_info_current_state (__wj_ti) : -1;  \
		if (G_LIKELY (__wj_st == STATE_RUNNING ||                               \
			      __wj_st == STATE_ASYNC_SUSPEND_REQUESTED)) {              \
			cookie = mono_threads_enter_gc_safe_region_internal (&sd);      \
			entered = TRUE;                                                 \
		} else {                                                                \
			mono_wasm_jit_counters [WJC_JS_BLOCKING_SKIPPED]++;             \
		}                                                                       \
	} while (0)

#define WJ_JS_BLOCKING_END(cookie, sd, entered)                                         \
	do {                                                                            \
		if (G_LIKELY (entered))                                                 \
			mono_threads_exit_gc_safe_region_internal (cookie, &sd);        \
	} while (0)

/* tiering.c's body-swap census. Declared here rather than including interp/tiering.h, which is an
 * interpreter-private header. */
extern gint32 mono_interp_relink_replaced, mono_interp_relink_late;
extern gint32 mono_interp_relink_untouched, mono_interp_relink_rejected;
extern gint32 mono_interp_relink_bail_cleared, mono_interp_relink_refreshed;

EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_stats (void)
{
	/* BUILD MARKER. deploy.sh proves a fresh runtime is in front of the browser by grepping the SERVED
	 * bytes for a string constant, and a refactor that adds no string has nothing to prove itself with --
	 * a static function's name can be inlined away and then never appears in the name section. This line
	 * exists for that, and it is printed rather than merely declared so it cannot be dropped as unused.
	 * Bump the tag whenever a runtime change needs to be provably deployed. */
	printf ("[wasm-jit build] wjbuild-p3-reemit-cov\n");
	printf ("[wasm-jit stats] registered=%lld bailed=%lld invalid=%lld invoked=%lld residual=%lld fastvcall=%lld (counting %s)\n",
		WJC_(WJC_REGISTERED), WJC_(WJC_BAILED), WJC_(WJC_INVALID),
		WJC_(WJC_INVOKE), WJC_(WJC_RESIDUAL), WJC_(WJC_FASTVCALL),
		mono_wasm_jit_stats ? "on" : "OFF — set MONO_WASM_JIT_STATS=1");
	/* TWO INSTANTIATION NUMBERS, AND THEY ANSWER DIFFERENT QUESTIONS -- this line used to print only the
	 * first under the bare label `instantiate=`, which read as the tier's whole instantiation bill and is
	 * not remotely that.
	 *
	 *   inst_compile = WJC_ELAPSED_INSTANTIATION, added at ONE site (mono_wasm_emit_method's validating
	 *                  instantiation on whichever worker won the compile). One per METHOD.
	 *   inst_all     = every instantiation on every worker, including the per-worker ones admission does
	 *                  on demand -- i.e. the cost of the table being per-worker. One per (worker, MODULE).
	 *
	 * inst_all/modules near 1.0 means no per-worker duplication to remove; N means every module is being
	 * instantiated on ~N threads. Compare `modules` against `registered` on the stats line above, not
	 * against attempts: a batched module is one instantiation for n methods, deliberately.
	 *
	 * inst_all is stats-INDEPENDENT (mono_wasm_jit_liveness fields 8/13), so it is the only one of the
	 * two that is also readable in a clean timing run. */
	printf ("[wasm-jit time] gen=%.1fms inst_compile=%.1fms inst_all=%.1fms modules=%d attempts=%lld bytes=%lld\n",
		(double) WJC_(WJC_ELAPSED_GENERATION) / 1000.0, (double) WJC_(WJC_ELAPSED_INSTANTIATION) / 1000.0,
		(double) wj_census_inst_us_total / 1000.0, wj_census_inst_total,
		WJC_(WJC_COMPILE_ATTEMPTS), WJC_(WJC_BYTES_GENERATED));
	printf ("[wasm-jit island] attempt=%lld completed=%lld budget_exhausted=%lld depth_exceeded=%lld blocked_cold=%lld promoted_up=%lld promoted_down=%lld\n",
		WJC_(WJC_ISLAND_ATTEMPT), WJC_(WJC_ISLAND_COMPLETED), WJC_(WJC_ISLAND_BUDGET_EXHAUSTED), WJC_(WJC_ISLAND_DEPTH_EXCEEDED),
		WJC_(WJC_ISLAND_BLOCKED_COLD), WJC_(WJC_PROMOTED_UP), WJC_(WJC_PROMOTED_DOWN));
	/* Event-driven blocker waiting (the island driver's alternative to poll-retrying a cold callee).
	 * woken/parked is the mean number of re-queues each park eventually produced. */
	printf ("[wasm-jit park] parked=%lld waiters_woken=%lld\n",
		WJC_(WJC_PARKED), WJC_(WJC_WAITER_WOKEN));
	printf ("[wasm-jit vcall] ic_hit=%lld ic_miss=%lld vfast_had=%lld vfast_new=%lld vfb_thresh=%lld vfb_perm=%lld\n",
		WJC_(WJC_VIC_HIT), WJC_(WJC_VIC_MISS), WJC_(WJC_VFAST_HAD), WJC_(WJC_VFAST_NEW),
		WJC_(WJC_VFB_THRESH), WJC_(WJC_VFB_PERM));
	/* vfb_thresh split by the target's slot state. cold = still counting (interp is the right answer);
	 * parked = crossed the threshold but its island won't close, i.e. interpreted on EVERY call and the
	 * real interp-residual driver; retry = transient compile-lock contention; notlive = compiled, but
	 * admit_live said no on THIS worker.
	 *
	 * THE FOURTH ARM IS LOAD-BEARING AND USED TO BE MISSING HERE. WJC_VFB_NOTLIVE was added after this
	 * line and only reached the R166 split further down, so this printf advertised a three-way identity
	 * that stopped holding the day the fourth arm appeared -- and notlive is the arm that carried R244's
	 * 14-23M/run regression. Keep all four here, or delete the "sum should equal" claim. */
	printf ("[wasm-jit vfb] cold=%lld parked=%lld retry=%lld notlive=%lld (sum should equal vfb_thresh=%lld)\n",
		WJC_(WJC_VFB_COLD), WJC_(WJC_VFB_PARKED), WJC_(WJC_VFB_RETRY), WJC_(WJC_VFB_NOTLIVE),
		WJC_(WJC_VFB_THRESH));
	printf ("[wasm-jit vperm] aot=%lld eh=%lld ldaddr=%lld lcompare=%lld sig=%lld byref=%lld gshared=%lld rgctx=%lld sync=%lld eh_other=%lld other_opcode=%lld other=%lld\n",
		WJC_(WJC_VPERM_AOT), WJC_(WJC_VPERM_EH), WJC_(WJC_VPERM_LDADDR), WJC_(WJC_VPERM_LCMP),
		WJC_(WJC_VPERM_SIG), WJC_(WJC_VPERM_BYREF), WJC_(WJC_VPERM_GSHARED), WJC_(WJC_VPERM_RGCTX), WJC_(WJC_VPERM_SYNC), WJC_(WJC_VPERM_EHOTHER),
		WJC_(WJC_VPERM_OTHEROP), WJC_(WJC_VPERM_OTHER));
	printf ("[wasm-jit aotroute] aot_routed=%lld interp_routed=%lld vcall_aot_fast=%lld\n",
		WJC_(WJC_AOT_ROUTED), WJC_(WJC_INTERP_ROUTED), WJC_(WJC_VCALL_AOT_FAST));
	/* eslot_residual is the subset of interp_routed that skipped interp_entry entirely: interp_routed counts
	 * the crossing, this counts the ones that took the short way in. */
	printf ("[wasm-jit eslot] eslot_residual=%lld (of interp_routed=%lld) entry_slot_stale=%lld\n",
		WJC_(WJC_ESLOT_RESIDUAL), WJC_(WJC_INTERP_ROUTED), WJC_(WJC_ENTRY_SLOT_STALE));
	/* The three causes wj_admit_dependencies used to collapse into one UNGATED printf whose name walk
	 * trapped (R231). Non-zero is normal -- the refusal is recoverable and the next dispatch retries;
	 * what matters is WHICH cause dominates, which is why they are split rather than summed. */
	printf ("[wasm-jit abimismatch] unregistered=%lld chunk_missing=%lld sig_hash=%lld identity=%lld\n",
		WJC_(WJC_ABI_MISMATCH_UNREG), WJC_(WJC_ABI_MISMATCH_CHUNK), WJC_(WJC_ABI_MISMATCH_SIG),
		WJC_(WJC_ABI_MISMATCH_IDENT));
	/* Separate line because it is NOT an ABI disagreement: every hash and identity check passed and the
	 * slot simply was not installed. Non-zero here is the `function signature mismatch` class. */
	printf ("[wasm-jit admitlive] dep_not_live=%lld (cycle=%lld pending=%lld)\n",
		WJC_(WJC_ADMIT_DEP_NOT_LIVE), WJC_(WJC_ADMIT_DEP_NOT_LIVE_CYCLE), WJC_(WJC_ADMIT_DEP_NOT_LIVE_PENDING));
	/* R245: the rest of the admission refusal routes, which had no counters. `cycle` above plus these
	 * must account for every `return 0` on the path; if dep_not_live no longer equals cycle+pending, or
	 * the admit0 propagation count is wildly larger than the sum of the root causes, a route has been
	 * added without a counter again -- which is exactly how R244's regression stayed invisible. */
	printf ("[wasm-jit admitroutes] dep_admit0=%lld bad_id=%lld no_entry=%lld permfail=%lld state3=%lld blockers_truncated=%lld\n",
		WJC_(WJC_ADMIT_DEP_ADMIT0), WJC_(WJC_ADMIT_BAD_ID), WJC_(WJC_ADMIT_NO_ENTRY),
		WJC_(WJC_ADMIT_PERMFAIL), WJC_(WJC_ADMIT_STATE3), WJC_(WJC_BLOCKERS_TRUNCATED));
	/* R245: the SCC batcher, which had ZERO counters until now -- every outcome was a verbose-only
	 * printf, so its distribution was unobservable in a --stats run.
	 *
	 * READ `condemned` FIRST. It counts members marked permanently bailed by a give_up abort, INCLUDING
	 * members that compiled fine. The island driver fails the one callee and residual-routes the edge;
	 * this driver condemns the whole group. attempt = ok + busy + table + budget + alloc_fail +
	 * seed_perm + too_large + no_progress + iter_cap; if it does not, an exit is uncounted. */
	printf ("[wasm-jit scc] attempt=%lld ok=%lld members=%lld condemned=%lld resv_bypass=%lld colocate_ok=%lld colocate_fail=%lld\n",
		WJC_(WJC_SCC_ATTEMPT), WJC_(WJC_SCC_OK), WJC_(WJC_SCC_MEMBERS), WJC_(WJC_SCC_CONDEMNED),
		WJC_(WJC_SCC_RESV_BYPASS), WJC_(WJC_SCC_COLOCATE_OK), WJC_(WJC_SCC_COLOCATE_FAIL));
	printf ("[wasm-jit sccfail] busy=%lld table=%lld budget=%lld alloc=%lld seed_perm=%lld too_large=%lld no_progress=%lld iter_cap=%lld\n",
		WJC_(WJC_SCC_BUSY), WJC_(WJC_SCC_TABLE), WJC_(WJC_SCC_BUDGET), WJC_(WJC_SCC_ALLOC_FAIL),
		WJC_(WJC_SCC_SEED_PERM), WJC_(WJC_SCC_TOO_LARGE), WJC_(WJC_SCC_NO_PROGRESS), WJC_(WJC_SCC_ITER_CAP));
	/* spared + condemned == every member of every aborted batch. park_no_waiter NON-ZERO IS A PROBLEM:
	 * a spared member with nothing to wait on is parked forever, because PARKED stops the hit counter. */
	printf ("[wasm-jit sccfail2] spared=%lld condemned=%lld park_no_waiter=%lld\n",
		WJC_(WJC_SCC_SPARED), WJC_(WJC_SCC_CONDEMNED), WJC_(WJC_SCC_PARK_NO_WAITER));
	/* The IKVM body-swap edge. relink counters live in tiering.c and had NO reporting surface at all
	 * until R249 -- they were incremented and never read, which is how "generation 1's permanent bail
	 * is inherited by generation 2" survived. bail_cleared counts methods that got a second verdict. */
	printf ("[wasm-jit relink] replaced=%d late=%d untouched=%d rejected=%d bail_cleared=%d refreshed=%d jitted_knob=%d\n",
		mono_interp_relink_replaced, mono_interp_relink_late, mono_interp_relink_untouched,
		mono_interp_relink_rejected, mono_interp_relink_bail_cleared, mono_interp_relink_refreshed,
		mono_wasm_jit_relink_jitted);
	/* Islands started by the promotion drain rather than a threshold crossing. The drain runs on EVERY
	 * safe point with a FRESH budget per queue entry (up to promotion_drain x island_budget force
	 * compiles from one safe point), and none of it reached WJC_ISLAND_ATTEMPT before this. */
	printf ("[wasm-jit islanddrain] attempt=%lld completed=%lld  cycle_break_install=%lld cycle_break_fail=%lld\n",
		WJC_(WJC_ISLAND_DRAIN_ATTEMPT), WJC_(WJC_ISLAND_DRAIN_COMPLETED),
		WJC_(WJC_CYCLE_BREAK_INSTALL), WJC_(WJC_CYCLE_BREAK_FAIL));
	/* js_blocking_skipped MUST BE 0. Non-zero means some caller now reaches instantiation from managed
	 * code already in a coop BLOCKING state, so that instantiation blocked STW exactly as it did before
	 * this change -- correct, merely slow, and the fix is MONO_ENTER_GC_UNSAFE at that caller's P/Invoke
	 * boundary. Printed unconditionally, and counted ungated, because a clean timing run is where it
	 * matters and --stats is off there. */
	printf ("[wasm-jit gcstate] js_blocking_skipped=%lld (must be 0)  jitmm_uninit=%lld (must be 0 -- the interp_code_hash publish race)\n",
		WJC_(WJC_JS_BLOCKING_SKIPPED), WJC_(WJC_JITMM_UNINIT));
	/* drain_threads is the load-bearing one: a rendezvous that reaches only the thread that called it is
	 * not a rendezvous. Expect drains ~= count x drain_threads. */
	/* THE IDENTITY, WITH `capped` AS A TERM -- it was missing and the line printed a sum that did not
	 * hold: drained 930 against parts 208, the 722 difference being exactly the cap refusals, which are
	 * counted after DRAINED is bumped. A dump whose own total contradicts its own body is this file's
	 * documented signature for a broken census, and it was printing one.
	 *
	 * COMPILED must also equal REPUBLISHED + NO_RENDEZVOUS. When it did not -- 2,976 republished against
	 * 200 compiled -- that was not a counting error but a BUFFER OVERRUN in the batch array.
	 *
	 * `rereg` is WJC_FSLOT_REREGISTER, and it is HERE because it is the only way to see that the slot pin
	 * held. A re-emit re-registers the SAME method on the SAME f-slot, so it takes the re-registration
	 * arm rather than FSLOT_COLLISION; if rereg does not track COMPILED, the pin did not stick and the
	 * re-emits landed on FRESH pairs -- which is R170's failure exactly (REEMIT_SLOT_MOVED x20), is
	 * invisible in every other counter, and silently leaks two table entries per method. The counter was
	 * bumped and printed NOWHERE before this, so that check could not be made at all. */
	/* Re-emitted bodies against the run-wide census on the line above: emitted/sites is the coverage
	 * comparison R179 made, and no_fslot is the one R259 says gates everything -- if re-emission works,
	 * that share should be much lower here than run-wide, because the callees have since compiled. */
	{
		long long rs = WJC_(WJC_REEMIT_SITE), re_ = WJC_(WJC_REEMIT_EMITTED);
		printf ("[wasm-jit reemit-devirt] sites=%lld emitted=%lld no_rec=%lld no_fslot=%lld -> coverage %.1f%% (run-wide is on the devirt line)\n",
			rs, re_, WJC_(WJC_REEMIT_NO_REC), WJC_(WJC_REEMIT_NO_FSLOT),
			rs ? 100.0 * (double) re_ / (double) rs : 0.0);
	}
	printf ("[wasm-jit reemit] queued=%lld dedup=%lld mdone=%lld qfull=%lld | drained=%lld = compiled %lld + busy %lld (giveup %lld) + failed %lld + refused %lld + gone %lld + capped %lld | republished=%lld no_rv=%lld sites=%lld rereg=%lld slot_moved=%lld (knob=%d misses=%d batch=%d interval=%dms max=%d)\n",
		WJC_(WJC_REEMIT_QUEUED), WJC_(WJC_REEMIT_DEDUP), WJC_(WJC_REEMIT_METHOD_DONE),
		WJC_(WJC_REEMIT_QFULL), WJC_(WJC_REEMIT_DRAINED),
		WJC_(WJC_REEMIT_COMPILED), WJC_(WJC_REEMIT_BUSY), WJC_(WJC_REEMIT_BUSY_GIVEUP),
		WJC_(WJC_REEMIT_FAILED),
		WJC_(WJC_REEMIT_REFUSED), WJC_(WJC_REEMIT_GONE), WJC_(WJC_REEMIT_CAPPED), WJC_(WJC_REEMIT_REPUBLISHED),
		WJC_(WJC_REEMIT_NO_RENDEZVOUS), WJC_(WJC_REEMIT_SITE), WJC_(WJC_FSLOT_REREGISTER), WJC_(WJC_REEMIT_SLOT_MOVED),
		mono_wasm_jit_reemit, mono_wasm_jit_reemit_misses,
		mono_wasm_jit_reemit_batch, mono_wasm_jit_reemit_interval, mono_wasm_jit_reemit_max);
	/* The admission DFS's own depth. Printed next to re-emission because that is what moves it: see
	 * WJC_ADMIT_DEPTH_MAX. A run that wedges never reaches this print, so read it from the arms that
	 * PASS -- how close they ran to the cliff is the whole question. */
	printf ("[wasm-jit admit-depth] high_water=%lld over_%d=%lld gen_reset_visiting=%lld stale_visiting=%lld batch_swapped=%lld rv_stale2=%lld rv_forced=%lld | single_writer=%d sw_published=%lld sw_install_fail=%lld sw_verify_fail=%lld sw_closure_max=%lld sw_scope=recursive-group | sweep runs=%lld self_dead=%lld dep_bad=%lld | orphaned=%lld admit_orphaned=%lld reused=%lld gen_moved_midwalk=%lld\n",
		WJC_(WJC_ADMIT_DEPTH_MAX), WJ_ADMIT_DEPTH_WARN, WJC_(WJC_ADMIT_DEPTH_OVER),
		WJC_(WJC_ADMIT_GEN_RESET_VISITING), WJC_(WJC_ADMIT_STALE_VISITING),
		WJC_(WJC_ADMIT_BATCH_SWAPPED), WJC_(WJC_RV_DRAIN_STALE2),
		WJC_(WJC_RV_DRAIN_REFRESH_FORCED), mono_wasm_jit_single_writer,
		WJC_(WJC_SW_PUBLISHED), WJC_(WJC_SW_INSTALL_FAIL), WJC_(WJC_SW_VERIFY_FAIL),
		WJC_(WJC_SW_CLOSURE_MAX), WJC_(WJC_SWEEP_RUNS), WJC_(WJC_SWEEP_SELF_DEAD),
		WJC_(WJC_SWEEP_DEP_BAD), WJC_(WJC_FSLOT_ORPHANED), WJC_(WJC_ADMIT_ORPHANED),
		WJC_(WJC_FSLOT_REUSED), WJC_(WJC_ADMIT_GEN_MOVED_MIDWALK));
	/* THE THREE BOUNDS THAT MUST NOT BIND, printed together because each one fails SILENTLY.
	 *   install_budget_out   the closure walk gave up -> admissions refused -> a tier that admits nothing
	 *   retry_full           a drain overflowed its carry list -> it pinned the epoch -> a closure walk
	 *                        on every loop back-edge on that worker
	 *   worker_slots_full    a worker got the always-set action word -> it takes the out-of-line
	 *                        safepoint helper forever
	 * retire_depth is the other end of the same question: reclamation is blocked while any worker is
	 * inside the registry reader bracket, so a large depth with few reclaims means the grace period has
	 * degenerated back into the leak it replaced. */
	printf ("[wasm-jit bounds] closure_max=%lld install_budget_out=%lld | rv_retry ok=%lld maxn=%lld full=%lld"
		" | worker_slots=%lld full=%lld | retired=%lld freed=%lld reclaims=%lld depth_max=%lld\n",
		WJC_(WJC_SW_CLOSURE_MAX), WJC_(WJC_INSTALL_BUDGET_OUT),
		WJC_(WJC_RV_RETRY_OK), WJC_(WJC_RV_RETRY_MAXN), WJC_(WJC_RV_RETRY_FULL),
		WJC_(WJC_WORKER_SLOTS), WJC_(WJC_WORKER_SLOTS_FULL),
		WJC_(WJC_RETIRED), WJC_(WJC_RETIRE_FREED), WJC_(WJC_RETIRE_RECLAIMS), WJC_(WJC_RETIRE_DEPTH_MAX));
	/* The mandatory-replacement chain, end to end. Every step must be readable or "re-emission works" is
	 * an assertion: requested (IKVM body swaps + profile triggers) -> compiled -> republished -> adopted,
	 * with the three ways a mandatory one can be lost broken out. */
	printf ("[wasm-jit replace] reframe ok=%lld fail=%lld | merge_off=%lld merge_split=%lld"
		" | required_giveup=%lld required_dropped=%lld drain_skip=%lld\n",
		WJC_(WJC_REFRAME_OK), WJC_(WJC_REEMIT_REFRAME_FAIL),
		WJC_(WJC_COLOCATE_MERGE_OFF), WJC_(WJC_COLOCATE_MERGE_SPLIT),
		WJC_(WJC_REEMIT_REQUIRED_GIVEUP), WJC_(WJC_REEMIT_REQUIRED_DROPPED),
		WJC_(WJC_REEMIT_BATCH_WAIT));
	printf ("[wasm-jit rendezvous] count=%lld members=%lld drains=%lld drain_threads=%lld slots=%lld refused=%lld log_full=%lld\n",
		WJC_(WJC_RV_COUNT), WJC_(WJC_RV_MEMBERS), WJC_(WJC_RV_DRAIN), WJC_(WJC_RV_DRAIN_THREADS),
		WJC_(WJC_RV_DRAIN_SLOT), WJC_(WJC_RV_DRAIN_REFUSED), WJC_(WJC_RV_LOG_FULL));
	/* THE CO-LOCATOR'S FIELD OF VIEW, and the re-emission headroom, from one census. `visible` is the
	 * share of call sites whose callee the co-locator can even name; `reachable` is the share of the blind
	 * half that a re-emission could convert (the callee exists and was merely not compiled yet), as
	 * against `bailed` (terminal) and `noim` (never executed). Site-counted, not execution-weighted. */
	{
		long long hf = WJC_(WJC_CALLEE_HAS_FSLOT), un = WJC_(WJC_CALLEE_NO_FSLOT_UNTRIED),
			  ba = WJC_(WJC_CALLEE_NO_FSLOT_BAILED), ni = WJC_(WJC_CALLEE_NO_FSLOT_NOIM),
			  tot = hf + un + ba + ni;
		printf ("[wasm-jit callee] sites=%lld has_fslot=%lld untried=%lld bailed=%lld noim=%lld"
			" | colocator sees %.1f%%, re-emission could reach a further %.1f%%\n",
			tot, hf, un, ba, ni,
			tot ? 100.0 * (double) hf / (double) tot : 0.0,
			tot ? 100.0 * (double) un / (double) tot : 0.0);
	}
	printf ("[wasm-jit rendezvous] skipped=%lld (thread never had the module installed, so nothing to replace)\n",
		WJC_(WJC_RV_DRAIN_SKIPPED));
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
	printf ("[wasm-jit callform] local=%lld import=%lld indirect=%lld (per module assembled, not per execution)\n",
		WJC_(WJC_CALL_LOCAL), WJC_(WJC_CALL_IMPORT), WJC_(WJC_CALL_INDIRECT));
	/* Read colocated_members against registered for the co-located fraction of the tier, and the local
	 * count above against indirect for whether the grouping retargeted anything. Both are per module
	 * ASSEMBLED, so a re-framed member is counted again -- which is the point: it is how a re-frame is
	 * shown to have changed a call's form rather than just rebuilt the same module. */
#ifdef HOST_BROWSER
	{ extern int mono_wasm_jit_dump_dep_graph_knob; extern void mono_wasm_jit_dump_dep_graph (void);
	  if (mono_wasm_jit_dump_dep_graph_knob) mono_wasm_jit_dump_dep_graph (); }
#endif
	/* One framing path now. The auto planner (wj_auto_batch_*), per-island batching and whole-program
	 * rebatch are gone: all three were unreachable at BATCH_MODULE=0, all three used the OLD capture
	 * protocol (batch_begin -> N x force_compile -> batch_finish), and that protocol was the only user of
	 * WjBatch/WjBatchMember. Co-location goes through mono_wasm_jit_rebatch over retained relocatable
	 * bodies, which is what both live callers -- wasm_jit_compile_scc and colocate_deps_now -- already did. */
	printf ("[wasm-jit batching] one path: relocatable rebatch; no method imports, so admission never defers\n");
	/* tight_deps_dropped IS THE CAPTURED-EDGE COUNT: it sums (deps before framing - deps after) over every
	 * member, and a dep disappears exactly when co-location turned that call into a module-local one. Read
	 * it against the edges left in WJ_GRAPH for the capture RATE -- 12,966 captured against 26,732
	 * residual = 32.7% on 2026-09-16, which is R195's ~30% plateau arrived at independently. */
	printf ("[wasm-jit colocate] colocated_members=%lld of registered=%lld tight_deps_dropped=%lld rolled_back=%lld scc_refused=%lld\n",
		WJC_(WJC_COLOCATED_MEMBERS), WJC_(WJC_REGISTERED), WJC_(WJC_TIGHT_DEPS_DROPPED),
		WJC_(WJC_COLOCATE_ROLLBACK), WJC_(WJC_COLOCATE_SCC_REFUSED));
	/* WHY the groups that were not formed were not formed. The parts must sum, so print the identity
	 * and its residual rather than leaving the reader to add up eleven numbers: a non-zero `unaccounted`
	 * means a refusal path was added without a counter, which is the failure this split exists to stop.
	 * The dep_* arms are per-CALLEE volumes and deliberately outside the identity. */
	{
		long long try_n = WJC_(WJC_COLOCATE_TRY);
		long long accounted = WJC_(WJC_COLOCATE_SELF_BATCHED) + WJC_(WJC_COLOCATE_SELF_NO_DEPSET)
			+ WJC_(WJC_COLOCATE_SELF_REFUSED) + WJC_(WJC_COLOCATE_SINGLETON)
			+ WJC_(WJC_COLOCATE_SCC_REFUSED) + WJC_(WJC_COLOCATE_REBATCH_FAIL)
			+ WJC_(WJC_COLOCATE_FORMED);
		printf ("[wasm-jit colocate-why] try=%lld = formed %lld + self{batched %lld, no_depset %lld, refused %lld}"
			" + singleton %lld + scc %lld + rebatch_fail %lld  unaccounted=%lld\n",
			try_n, WJC_(WJC_COLOCATE_FORMED), WJC_(WJC_COLOCATE_SELF_BATCHED),
			WJC_(WJC_COLOCATE_SELF_NO_DEPSET), WJC_(WJC_COLOCATE_SELF_REFUSED),
			WJC_(WJC_COLOCATE_SINGLETON), WJC_(WJC_COLOCATE_SCC_REFUSED),
			WJC_(WJC_COLOCATE_REBATCH_FAIL), try_n - accounted);
		printf ("[wasm-jit colocate-dep] per-callee refusals (volumes): unreg=%lld nobody=%lld batched=%lld"
			" refused=%lld byte_cap=%lld member_cap=%lld (COLOCATE_MAX=%d COLOCATE_BYTES=%d)\n",
			WJC_(WJC_COLOCATE_DEP_UNREG), WJC_(WJC_COLOCATE_DEP_NOBODY), WJC_(WJC_COLOCATE_DEP_BATCHED),
			WJC_(WJC_COLOCATE_DEP_REFUSED), WJC_(WJC_COLOCATE_DEP_BYTE_CAP), WJC_(WJC_COLOCATE_MEMBER_CAP),
			mono_wasm_jit_colocate_max, mono_wasm_jit_colocate_bytes);
		printf ("[wasm-jit colocate-merge] groups absorbed=%lld  refused as split=%lld  (read against dep batched=%lld:"
			" the two split what used to be all batched)\n",
			WJC_(WJC_COLOCATE_MERGED), WJC_(WJC_COLOCATE_MERGE_SPLIT), WJC_(WJC_COLOCATE_DEP_BATCHED));
		printf ("[wasm-jit colocate-merge-undo] merges rolled back into their previous group=%lld"
			" (implemented and UNTESTED while this reads 0; a merge rollback restores saved.batch"
			" rather than clearing it, see WJC_COLOCATE_MERGE_ROLLBACK)\n",
			WJC_(WJC_COLOCATE_MERGE_ROLLBACK));
		printf ("[wasm-jit colocate-merge-why] closure refused: sibling preconditions=%lld  caps=%lld"
			"  (+ merged %lld == batched callees reached; precond is fixable in the RULES, caps by a KNOB)\n",
			WJC_(WJC_COLOCATE_MERGE_PRECOND), WJC_(WJC_COLOCATE_MERGE_CAP), WJC_(WJC_COLOCATE_MERGED));
	}
		/* Reach as EXECUTED, from the IC miss path. sibling/(sibling+foreign) is how much of our own
		 * dispatch the grouping half already covers; notours is the AOT wall and is OVER_AOT's problem,
		 * not the planner's. A lower bound -- see WJC_VIC_TGT_*. */
		{
			long long sib = WJC_(WJC_VIC_TGT_SIBLING), fgn = WJC_(WJC_VIC_TGT_FOREIGN),
				  nto = WJC_(WJC_VIC_TGT_NOTOURS), tot = sib + fgn + nto, ours = sib + fgn;
		printf ("[wasm-jit colocate-reach] IC-publish targets: sibling=%lld foreign=%lld notours=%lld"
				" | %.2f%% of ours co-resident, %.2f%% of all\n",
				sib, fgn, nto,
				ours ? 100.0 * (double) sib / (double) ours : 0.0,
				tot ? 100.0 * (double) sib / (double) tot : 0.0);
		}
	/* R166_ADMIT_RETRY_SPLIT. Admission failures by retriability, and the vcall fallback arm that had no
	 * counter until R166. vfb_notlive is the one to watch: it is a virtual call to a method that IS
	 * compiled and simply is not dispatchable on the calling worker, and 109.6M of them in a 120 s window
	 * was the whole of co-location's 1.62x regression. Its three siblings must sum with it to vfb_thresh. */
	printf ("[wasm-jit admit] R166_ADMIT_RETRY_SPLIT fail_perm=%lld fail_retry=%lld | vfb_thresh=%lld = cold %lld + parked %lld + retry %lld + notlive %lld\n",
		WJC_(WJC_ADMIT_FAIL_PERM), WJC_(WJC_ADMIT_FAIL_RETRY), WJC_(WJC_VFB_THRESH),
		WJC_(WJC_VFB_COLD), WJC_(WJC_VFB_PARKED), WJC_(WJC_VFB_RETRY), WJC_(WJC_VFB_NOTLIVE));
	/* R166_ADMIT_LIVE_SPLIT: which half of `admit() && desc_admitted()` refused, and why. */
	printf ("[wasm-jit admit] R166_PREMARK_UNDO admit_live_split admit0=%lld state=%lld gen=%lld e_notlive=%lld f_notlive=%lld\n",
		WJC_(WJC_AL_ADMIT0), WJC_(WJC_AL_STATE), WJC_(WJC_AL_GEN),
		WJC_(WJC_AL_ELIVE), WJC_(WJC_AL_FLIVE));
	/* R167_SHADOW. members = private leaf duplicates framed in; bytes = their wire cost, which is paid
	 * per CALLER module; refused = call relocations that did not qualify. The reach question is
	 * call_local vs call_indirect above, not this count. */
	printf ("[wasm-jit vtabi] vt_byaddr_methods=%lld vret_methods=%lld\n",
		WJC_(WJC_VT_BYADDR_METHODS), WJC_(WJC_VRET_METHODS));
	{
		extern int mono_wasm_jit_stackprobe;
		printf ("[wasm-jit stackprobe] worst C-stack headroom=%d bytes of %d total (%d probes, PROBE=%d)"
			" -- headroom near 0 proves the compile chain overflows; headroom in the hundreds of KB says"
			" the OOB faults are NOT a stack problem and two rounds of array-shaving chased the wrong thing\n",
			wj_stack_min_headroom == G_MAXINT32 ? -1 : wj_stack_min_headroom,
			wj_stack_total_seen, wj_stack_probe_hits, mono_wasm_jit_stackprobe);
	}
	/* Late-f-slot healing sites EMITTED. Each is a helper call, a branch and a dynamic call_indirect on
	 * every dispatch (~19.5M/run, R196), standing in for an f-slot the callee usually acquires shortly
	 * afterwards. MONO_WASM_JIT_HEAL_WAIT used to wake this method for re-emission when that happened,
	 * which deleted the block; that went with re-emission (mechanism fired, 90 of 107 sites woken,
	 * effect nil at a 0.057%% ceiling). The census stays: it sizes the pool a relocatable f-slot hole
	 * would address, which is what R195 argues for instead. */
	printf ("[wasm-jit heal] late-f-slot healing sites emitted=%lld\n", WJC_(WJC_HEAL_SITES));
	printf ("[wasm-jit asm-hygiene] asm_noname=%lld -- MUST be 0, or perf symbolisation silently degrades\n"
		"  to a bare 'wasmjit' for that method and every name-resolving instrument goes blind.\n",
		WJC_(WJC_ASM_NONAME));
	printf ("[wasm-jit transition] residual_healed=%lld fast_delegate=%lld delegate_ic_hit=%lld (fast counters require PROFILE_FAST=1)\n",
		WJC_(WJC_RESIDUAL_HEALED), WJC_(WJC_FAST_DELEGATE), WJC_(WJC_DELEGATE_IC_HIT));
	/* WHY a delegate dispatch missed the direct e-thunk. call_delegate is 97% of call_interp's traffic
	 * and the whole interp-boundary complex is ~5.7-6.1% of the client render thread, but its fast path
	 * is `eslot > 0 && scalar` and which conjunct fails has never been measured. Bumped where the
	 * fallback is TAKEN. norecipe = no usable recipe at all (enters `invoke`); noeslot = recipe in hand
	 * but the target's entry thunk is not admitted on this worker; nonscalar = admitted but the
	 * signature has a by-value vtype or byref, which the e-thunk ABI cannot express. Those three answer
	 * OPPOSITE questions -- noeslot is an admission/ordering problem, nonscalar is an ABI one. */
	printf ("[wasm-jit delegate-slow] norecipe=%lld noeslot=%lld nonscalar=%lld"
		" (vtret=%lld vtarg=%lld byref=%lld)\n"
		"  norecipe+noeslot+nonscalar == the call_interp exits out of call_delegate;"
		" vtret+vtarg+byref == nonscalar.\n"
		"  BYREF is the cheap one: byref params are ALREADY layout-compatible with the e-thunk (one\n"
		"  stack slot each, pointer on both sides). VTARG/VTRET need real ABI work.\n",
		WJC_(WJC_DELEGATE_SLOW_NORECIPE), WJC_(WJC_DELEGATE_SLOW_NOESLOT),
		WJC_(WJC_DELEGATE_SLOW_NONSCALAR), WJC_(WJC_DELEGATE_SLOW_VTRET),
		WJC_(WJC_DELEGATE_SLOW_VTARG), WJC_(WJC_DELEGATE_SLOW_BYREF));
	/* DEVIRT CENSUS. sites = ordinary virtual sites offered to the prediction gate; delegate_sites = the
	 * population it refuses outright. The four refusal reasons pull in OPPOSITE directions on the "raise
	 * the JIT threshold so the interpreter records more" question: no_rec + cold argue for more warmup,
	 * poly argues against it (a second receiver disqualifies a site permanently and irreversibly), so
	 * read them as a pair before changing the threshold. poly_90 is the subset of poly whose front-runner
	 * is already >= 90% of observations -- i.e. what relaxing `margin == total` to a frequency bar would
	 * recover, measured without shipping the relaxation. */
	printf ("[wasm-jit devirt] sites=%lld emitted=%lld | no_rec=%lld cold=%lld poly=%lld (of which >=90%%: %lld) sig=%lld no_fslot=%lld | delegate_sites=%lld fast_devirt=%lld\n",
		WJC_(WJC_DEVIRT_SITE), WJC_(WJC_DEVIRT_EMITTED), WJC_(WJC_DEVIRT_NO_REC), WJC_(WJC_DEVIRT_COLD),
		WJC_(WJC_DEVIRT_POLY), WJC_(WJC_DEVIRT_POLY_90), WJC_(WJC_DEVIRT_SIG), WJC_(WJC_DEVIRT_NO_FSLOT),
		WJC_(WJC_DELEGATE_SITE), WJC_(WJC_FAST_DEVIRT));
	printf ("[wasm-jit devirt-arm2] emitted=%lld | thin=%lld no_alt=%lld no_fslot=%lld sig=%lld self=%lld"
		"  (pct>=%d)  EXECUTED hits=%lld\n"
		"  thin is the break-even REFUSING a site, not a failure: an arm below ~15%% capture loses"
		" (vcall_ways 4->1 was +9.6%% fps at ~1%% capture). Read hits against ic-why alt-receiver.\n",
		WJC_(WJC_DEVIRT_ARM2_EMITTED), WJC_(WJC_DEVIRT_ARM2_THIN), WJC_(WJC_DEVIRT_ARM2_NO_ALT),
		WJC_(WJC_DEVIRT_ARM2_NO_FSLOT), WJC_(WJC_DEVIRT_ARM2_SIG), WJC_(WJC_DEVIRT_ARM2_SELF),
		mono_wasm_jit_devirt_arm2_pct, WJC_(WJC_FAST_DEVIRT2));
	printf ("[wasm-jit delegate-devirt3] armed=%lld thin=%lld refused=%lld (no_fslot=%lld sig=%lld)"
		" | EXECUTED hits=%lld"
		"  (MONO_WASM_JIT_DELEGATE_DEVIRT=%d%%)\n"
		"  Delegates are ~23.5%% of executed dispatch and had NO devirt: prof_predict needs a resolved\n"
		"  target and the delegate recorder passes NULL, and `margin` is 0 by design because the identity\n"
		"  is del->method, not a receiver vtable. Per-identity COUNTS are the only workable signal.\n"
		"  Read hits against fast_delegate -- that is the pool this carves out of.\n",
		WJC_(WJC_DELEGATE_DEVIRT_ARM), WJC_(WJC_DELEGATE_DEVIRT_THIN),
		WJC_(WJC_DELEGATE_DEVIRT_REFUSED), WJC_(WJC_DELEGATE_DEVIRT_NO_FSLOT),
		WJC_(WJC_DELEGATE_DEVIRT_SIG), WJC_(WJC_FAST_DELEGATE_DEVIRT),
		mono_wasm_jit_delegate_devirt);
	printf ("[wasm-jit delegate-devirt] no-record=%lld -- split out of `thin`, which was a CATCH-ALL and\n"
		"  read 7,582 while hiding a reader that could never succeed at a delegate site (it required\n"
		"  id_targets, always NULL there because the identity IS the target). Fourth such bucket found in\n"
		"  one session: a counter reachable two ways is not a diagnosis.\n", WJC_(WJC_DELEGATE_DEVIRT_NOREC));
	printf ("[wasm-jit arm-abi] arm-candidate refusals: unjittable=%lld no_fslot=%lld sig=%lld"
		" | abi invalid=%lld byaddr=%lld shape=%lld"
		" -- `shape` is the only one that would mean overrides of one method lower differently"
		" (generic sharing); the other two are signatures the shared call sequence cannot express.\n",
		WJC_(WJC_ARM_UNJITTABLE), WJC_(WJC_DEVIRT_ARM2_NO_FSLOT), WJC_(WJC_DEVIRT_ARM2_SIG),
		WJC_(WJC_ARM_ABI_INVALID), WJC_(WJC_ARM_ABI_BYADDR), WJC_(WJC_ARM_ABI_SHAPE));
	printf ("[wasm-jit devirt-arm2] poly sites given a FIRST arm=%lld -- these are counted in BOTH `poly`"
		" and `emitted` above, so the census reconciles as sites == emitted + no_rec + cold + poly + sig"
		" + no_fslot - poly_arm1\n", WJC_(WJC_DEVIRT_POLY_ARM1));
	printf ("[wasm-jit ic-why] EXECUTED inline-IC hits split by the devirt outcome of their OWN site"
		" (PROFILE_FAST only; parts MUST sum to fast_vic above):\n"
		"  alt-receiver at a DEVIRTED site = %lld  <- coverage work can NEVER remove these\n"
		"  no_rec=%lld cold=%lld poly=%lld no_fslot=%lld other=%lld  <- sites coverage could reach\n",
		WJC_(WJC_FAST_VIC_PRED), WJC_(WJC_FAST_VIC_NO_REC), WJC_(WJC_FAST_VIC_COLD),
		WJC_(WJC_FAST_VIC_POLY), WJC_(WJC_FAST_VIC_NO_FSLOT), WJC_(WJC_FAST_VIC_OTHER));
	printf ("[wasm-jit devirt-force] forced=%lld capped=%lld (MONO_WASM_JIT_DEVIRT_FORCE=%d min=%d max=%d) -- read against no_fslot above and parked=%lld\n",
		WJC_(WJC_DEVIRT_FORCED), WJC_(WJC_DEVIRT_FORCE_CAPPED),
		mono_wasm_jit_devirt_force, mono_wasm_jit_devirt_force_min, mono_wasm_jit_devirt_force_max,
		WJC_(WJC_PARKED));
	/* LCSE reach. hits/loads_seen is the elimination RATE; evict>0 says the load table is the binding
	 * constraint (raise WJ_LCSE_LOADS) rather than the kill policy. Requires MONO_WASM_JIT_LCSE=1. */
	printf ("[wasm-jit guarded-inline] sites=%lld candidates=%lld admitted=%lld emitted=%lld"
		"  | refused: prof=%lld clauses=%lld size=%lld other=%lld sig=%lld self=%lld late=%lld"
		"  (MONO_WASM_JIT_GUARDED_INLINE=%d SIZE=%d)\n"
		"  ASSERT admitted == emitted + late, AND sites == admitted+prof+self+sig+clauses+size+other.\n"
		"  `candidates` is measured with the knob OFF, so a plain\n"
		"  STATS run sizes the whole addressable population for free -- read it before spending a build.\n"
		"  There is NO inliner in this pipeline for virtual calls:\n"
		"  V8 cannot inline across modules (imports score 0) and mono's own gate refuses every callvirt,\n"
		"  so `sites` is the whole population that pass would reach. READ `clauses` FIRST --\n"
		"  mono_method_check_inlining rejects ANY method with a try/catch and Java is EH-dense, so it may\n"
		"  be the entire story. `prof` is no_rec arriving here: 99.3%% of profile observations land AFTER\n"
		"  the method is JITted, so a first compile usually has no record for the site at all.\n",
		WJC_(WJC_GI_SITE), WJC_(WJC_GI_CANDIDATE), WJC_(WJC_GI_ADMITTED), WJC_(WJC_GI_EMITTED),
		WJC_(WJC_GI_REFUSED_PROF), WJC_(WJC_GI_REFUSED_CLAUSES), WJC_(WJC_GI_REFUSED_SIZE),
		WJC_(WJC_GI_REFUSED_OTHER),
		WJC_(WJC_GI_REFUSED_SIG), WJC_(WJC_GI_REFUSED_SELF), WJC_(WJC_GI_REFUSED_LATE),
		mono_wasm_jit_guarded_inline, mono_wasm_jit_guarded_inline_size);
#ifdef HOST_BROWSER
	/* ALL of them, not a top-N. The tail is 72%% of inlined sites, and a site count is not an execution
	 * count -- a one-site callee can be white hot -- so a truncated list cannot answer the weighting
	 * question it exists for. */
	{ extern void mono_wasm_jit_dump_guarded_inlines (int topn); mono_wasm_jit_dump_guarded_inlines (1024); }
#endif
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

