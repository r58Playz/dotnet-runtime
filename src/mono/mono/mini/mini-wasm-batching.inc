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


/* And the dependency set the assembler derives for each member of the same framing. See WjAsmDeps: it is
 * what mono_wasm_jit_batch_bind republishes onto WjRegEntry.deps, because a rebatch has no capture phase
 * and so no WjBatchMember to take them from. */
static __thread WjAsmDeps wj_batch_deps [WJ_BATCH_MAX];


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
	int ok = 0;
	/* A diagnostic still dereferences re->depset and re->batch, and it runs on the MAIN thread while
	 * workers are compiling. Knob-gated is not the same as safe. */
	wj_reg_read_enter ();
	if (i >= 0 && i < wj_reg_n && (re = wj_reg_at (i)) && (re->logical_method || re->body_method)) {
		if (out_method) *out_method = re->logical_method ? re->logical_method : re->body_method;
		if (out_len) *out_len = re->body_len;
		if (out_ndeps) *out_ndeps = re->depset ? re->depset->n : 0;
		if (out_batched) *out_batched = re->batch != NULL;
		ok = 1;
	}
	wj_reg_read_leave ();
	return ok;
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
	MonoMethod *m = NULL;
	wj_reg_read_enter ();
	if (i >= 0 && i < wj_reg_n && (re = wj_reg_at (i)) &&
	    dep >= 0 && re->depset && dep < re->depset->n)
		m = re->depset->method [dep];
	wj_reg_read_leave ();
	return m;
}

/* Everything mono_wasm_jit_batch_bind overwrites on a registry entry, kept so the bind can be undone. */
typedef struct {
	void *bytes; int len;
	WjDepSet *depset;
	guint32 generation;
	/* THE PREVIOUS GROUP, and it is a correctness field, not a diagnostic.
	 *
	 * Before merging was allowed this was always NULL and the rollback could hardcode that. It cannot
	 * once a re-frame may ABSORB an existing group: a member rolled back out of a merged group must go
	 * back to the group it was in, because `saved.bytes` for such a member is that group's SHARED module
	 * -- which exports e<i>/f<i> per member, not e/f. Restoring the bytes while clearing `batch` leaves
	 * the entry claiming a standalone module whose exports do not exist, which mono_wasm_jit_admit turns
	 * into the "function signature mismatch" R165 spent a session on. */
	WjBatchDesc *batch;
} WjRegSaved;

/*
 * UNDO a group bind whose admission failed, so co-location is a TIER-2 OPTIMISATION rather than a
 * commitment: the members go back to the standalone modules they were already running on.
 *
 * This is the single highest-leverage thing in the co-location path, and its absence is what made the
 * feature a 16x regression rather than a null. `re->batch` was assigned in exactly one place and cleared
 * NOWHERE, and `batch_bind` overwrote `re->bytes` with the group module -- so a group that could not be
 * admitted left every member with no module the registry knew about, and admission's `fail:` marked all N
 * `wj_desc_state = 3`, permanently. MEASURED (R160/R161): 2,465 groups and 2,078 distinct methods condemned
 * in one run, 250M interp->JIT entry-thunk crossings per 60 s window against the control's 2.0M (126x), and
 * 812 ms/frame against 50 ms.
 *
 * Nothing here has to be reconstructed: every overwritten field was snapshotted in batch_bind, and the
 * standalone module bytes are the same allocation the entry held before -- batch_bind never freed them
 * (deliberately: another worker may still be walking the previous generation), so restoring the pointer is
 * all that is required.
 *
 * A FRESH generation is published rather than the old one. The group WAS instantiated on this worker before
 * admission was attempted, so this worker's table currently holds the group's exports; they are the members'
 * own bodies and calling them is harmless, but every worker must be made to re-admit against the restored
 * standalone entry. A new generation is what forces that, and the old value would not (workers that already
 * cached it would see no change).
 */
static void
wj_batch_rollback (const int *desc_ids, int n, WjRegSaved *saved, WjBatchDesc *bd)
{
	int i;

	/* One line per rolled-back GROUP at verbose >= 2. Also the string deploy.sh proves this build by: a
	 * change worth deploying should add one, and the previous revision of this path added none, so its
	 * deploy could only be verified by file hash. */
	if (mono_wasm_jit_verbose >= 2)
		printf ("WASM_JIT_COLOCATE_ROLLBACK members=%d (group unwound; members keep standalone modules)\n", n);
	for (i = 0; i < n; i++) {
		WjRegEntry *re = wj_reg_at (desc_ids [i] - 1);
		WjDepSet *failed_depset;
		if (!re)
			continue;
		failed_depset = re->depset;
		/* Back to whatever group the member was in, which is NULL unless this bind was a MERGE. See
		 * WjRegSaved.batch: clearing unconditionally would strand an absorbed member on a shared module
		 * it no longer claims membership of. */
		if (saved [i].batch && G_UNLIKELY (mono_wasm_jit_stats))
			mono_wasm_jit_count (WJC_COLOCATE_MERGE_ROLLBACK);
		re->batch = saved [i].batch;
		/* ONCE ONLY. Without this the method rejoins the candidate pool and the next publish regroups it,
		 * fails the same way and rolls back again -- churn that wedges boot. */
		re->colocate_refused = 1;
		re->bytes = saved [i].bytes;
		re->len = saved [i].len;
		/* Count down, pointers, count up -- same rule as the publish in batch_bind: another worker may be
		 * walking re->deps[0..ndeps) without a lock. */
		mono_memory_barrier ();
		re->depset = saved [i].depset;
		mono_memory_barrier ();
		/* This set belonged only to the generation being rolled back.  Retire it after
		 * restoring the saved pointer; freeing the saved set before rollback made the
		 * restored registry entry point at released storage. */
		if (failed_depset != saved [i].depset)
			wj_retire_payload (failed_depset, WJ_RETIRED_DEPSET);
		/* RESTORE THE ORIGINAL GENERATION -- do not mint a new one.
		 *
		 * A new generation invalidates the entry for EVERY worker, and every one of them then re-admits and
		 * re-instantiates a method whose group only ever existed on this one. `new WebAssembly.Module` is a
		 * long synchronous call with no safepoint in it, so a fleet of workers all doing that at once is a
		 * fleet that cannot answer a GC suspend request -- which is exactly the failure this produced:
		 * `WAITING for 1 threads, got 0 suspended` and `suspend_thread suspend took 20001 ms`, with boot
		 * emitting no output at all. It scales with the number of rollbacks (bootcheck PASSES with rollback
		 * on when the byte cap stops groups forming, TIMES OUT at full volume).
		 *
		 * The group was instantiated HERE and nowhere else, so only this worker needs to redo anything.
		 * Clearing this thread's installed bits makes mono_wasm_jit_admit re-instantiate the restored
		 * standalone module locally; every other worker sees the entry exactly as it was and keeps whatever
		 * valid state it already had. A worker that DID manage to admit the group in the window before this
		 * runs is self-healing: its cached generation no longer matches, so it re-admits. */
		re->generation = saved [i].generation;
		wj_desc_state_ensure (desc_ids [i] + 1);
		wj_desc_state [desc_ids [i]] = 0;
		wj_desc_generation [desc_ids [i]] = saved [i].generation;
		/* DO NOT CLEAR THE INSTALLED BITS -- it is either a no-op or actively harmful, and it opens the
		 * one window in which a slot can go backwards.
		 *
		 * If the group WAS instantiated here, the slot holds that module's export for THIS SAME METHOD:
		 * a valid function with the identical signature, so un-installing it destroys good state for no
		 * gain. If the group was never instantiated -- the usual reason a rollback happens -- the bits
		 * were never set and clearing does nothing.
		 *
		 * What it costs is the same hazard the rendezvous drain had: between the clear and a successful
		 * re-admit the slot is marked uninstalled while every CALLER that already admitted this
		 * descriptor stays published at state 2, and a transient refusal in that window leaves it that
		 * way. Generated code carries no liveness check, so those callers land on
		 * mono_jiterp_placeholder_jit_call. `installed` is monotonic everywhere else in this file, and
		 * the sweep's surviving `inst=0` reports are descriptors that had been installed when their
		 * callers were admitted. State and generation above are still reverted, which is what actually
		 * forces a re-admit of the restored standalone module. */
	}
	(void) bd;
}

/*
 * MONO_WASM_JIT_DUMP_DEP_GRAPH=1: emit the whole registry dependency graph as plain integers, once, so the
 * cycle structure can be computed OFFLINE instead of guessed at.
 *
 * The question this exists to answer: a co-located group's admission defers only when an IMPORT edge points
 * at a descriptor already `visiting` in the current DFS -- a cycle (R160 addendum). Grouping contracts N
 * methods into ONE node of this graph, and contracting nodes in a DAG creates cycles. Whether that is worth
 * fixing by merging each cycle into one module depends entirely on how BIG the cycles are, and nothing in
 * the tree could answer that: the graph accessors above are plain C for in-process planners, with no JS
 * export, so there was no way to look.
 *
 * Emits one line per registry entry:
 *   G <desc> <batch_generation> <e> <f> <ndeps> [d<fslot>...]
 * FIVE numeric fields, not six. This comment used to document an `<nimports>` field and `i<fslot>` edges
 * alongside the `d` ones, with a paragraph on why a cycle through an `i` edge is fatal where a `d` cycle
 * is not. METHOD IMPORTS WERE REMOVED IN R163 and the emitter below has written neither since. The stale
 * shape was not harmless: scratchpad/wj/depcycles.py matched on six groups, so it parsed every archived
 * pre-R163 dump and ZERO dumps taken since, and it reports that as "no WJ_GRAPH block found (was
 * MONO_WASM_JIT_DUMP_DEP_GRAPH=1 and --dumps set?)" -- which reads as "the knob did not fire", not as
 * "the parser is stale". Both are fixed together; keep them together.
 *
 * `batch_generation` is 0 for a standalone entry and otherwise identifies the group -- entries sharing one
 * are premarked `visiting` together, so offline they contract to a single node. Dependencies are f-slots;
 * `f` on each line is what maps them back to a descriptor.
 *
 * MAIN-THREAD SAFE, and it has to be: like every other dump here it is called from JS on the main thread,
 * where taking the loader lock can deadlock against a GC-suspended worker. It reads nothing but plain ints
 * out of the registry -- no MonoMethod deref, no name resolution, no Mono API. A concurrent registration can
 * tear one line; that is acceptable for a census and is why the count is printed rather than implied.
 */
WJ_KEEPALIVE void mono_wasm_jit_dump_dep_graph (void);
WJ_KEEPALIVE void
mono_wasm_jit_dump_dep_graph (void)
{
	int n = wj_reg_n, i, j;

	printf ("WJ_GRAPH_BEGIN n=%d\n", n);
	wj_reg_read_enter ();
	for (i = 0; i < n; ++i) {
		WjRegEntry *re = wj_reg_at (i);
		if (!re)
			continue;
		printf ("G %d %u %d %d %d", i + 1,
			re->batch ? (unsigned) re->batch->generation : 0u,
			re->e, re->f, re->depset ? re->depset->n : 0);
		{ WjDepSet *ds = re->depset;
		  for (j = 0; ds && j < ds->n; ++j)
			printf (" d%d", ds->slot [j]); }
		printf ("\n");
	}
	wj_reg_read_leave ();
	printf ("WJ_GRAPH_END\n");
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
	WjRegSaved *saved = NULL;
	int i;

	if (n <= 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (desc_ids [i] <= 0 || !wj_reg_at (desc_ids [i] - 1))
			return 0;
	}
	/* CO-LOCATION IS AN OPTIMISATION, NOT A COMMITMENT. Snapshot every field this function is about to
	 * overwrite, so a group that cannot be admitted can be UNDONE and its members left on the standalone
	 * modules they were running on a moment ago. See wj_batch_rollback. */
	/* MONO_WASM_JIT_COLOCATE_ROLLBACK shipped 1: restore each absorbed member to its PREVIOUS group when
	 * a merge is undone, rather than clearing it. Not a performance choice -- clearing throws away
	 * grouping that was already validated. */
	if (1)
		saved = g_new0 (WjRegSaved, n);
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
#ifdef HOST_BROWSER
		/* batch_finish has already overwritten these slots on the compiling worker. Any previously
		 * installed guard-free AOT adapter must stop entering them until admission below succeeds. */
		if (re->logical_imethod)
				mono_jiterp_wasm_jit_unpatch_interp_entry (re->logical_imethod);
#endif
		if (saved) {
			saved [i].bytes = re->bytes; saved [i].len = re->len;
			saved [i].depset = re->depset;
			saved [i].generation = re->generation;
			saved [i].batch = re->batch;   /* NULL for a fresh group; the absorbed group on a merge */
		}
		re->batch = bd;
		/* Do NOT publish re->generation here. It is set below, AFTER mono_memory_barrier() and after
		 * re->imports has been republished, and that ordering is the point: a worker that sees the new
		 * generation with the OLD import list admits against imports this generation no longer binds.
		 * Setting it early was tried 2026-08-29 and took WASM_JIT_BATCH_ADMIT_FAIL from 0 to ~1200. */
		/* Point the per-entry bytes at the shared module too, so anything reading re->bytes sees the
		 * real module rather than the discarded standalone one. */
		re->bytes = bytes;
		re->len = len;
		/* Code generation is not topology-invariant: as more callees become JITted, a later batch
		 * re-emission can replace a residual with a new direct f-slot call. Publish the dependency
		 * set captured from this exact body generation. Old arrays intentionally remain allocated:
		 * another worker may still be walking the prior generation lock-free.
		 *
		 * PREFER THE ASSEMBLER'S SET, AND DO IT OUTSIDE THE `bm` GUARD -- exactly like re->imports below,
		 * and for exactly the same reason. `bm` is `wj_batch ? ... : NULL`, and mono_wasm_jit_rebatch has
		 * no capture phase, so on every rebatch `bm` is NULL and this block used to be SKIPPED ENTIRELY.
		 * The entry then kept the standalone generation's dependency list, which still names every callee
		 * co-location had just turned into a module-local `call <funcidx>` -- calls that need no admission
		 * at all, because they are bytes in this very module. Admission therefore demanded a strictly
		 * larger closure than the code could possibly need, on every member of every group.
		 *
		 * MEASURED, one in-world run 2026-08-29 with colocate_deps=1: BATCH_ADMIT_FAIL 1966,
		 * ADMIT_DEFER_GIVEUP 1540, against 3 deferrals total in the control -- and a deferral give-up is
		 * permanent (wj_desc_state = 3), so ~1540 methods lost the JIT tier for the run and world load
		 * slowed until the harness declared it wedged.
		 *
		 * wj_batch_deps is filled by wj_asm_resolve_body for every CALL hole that did NOT resolve to
		 * WASM_FORM_LOCAL, so it is the dependency set of the module actually being installed rather than
		 * of the one each member was compiled as. */
		{
			WjAsmDeps *ad = &wj_batch_deps [i];
			WjDepSet *nds = wj_depset_new (ad->slot, ad->sig, ad->method, ad->n);
			if (G_UNLIKELY (mono_wasm_jit_stats)) {
				int was = re->depset ? re->depset->n : 0;
				mono_wasm_jit_add (WJC_TIGHT_DEPS_DROPPED, (was > ad->n) ? (was - ad->n) : 0);
			}
			/* ONE STORE. The old four-field publish could not be made safe by ordering: a reader's loop
			 * re-reads the count and the pointer every iteration, so it could start on the old array and
			 * finish on the new. Swapping a single pointer to an immutable set means a reader that
			 * snapshots it sees one generation, whole. The superseded set is deliberately not freed. */
			mono_memory_barrier ();
			re->depset = nds;
		}
		/* The new generation imports a different set, and this is true whether or not the members were
		 * re-emitted: co-locating a callee turns its import into a module-local call and removes it.
		 * OUTSIDE the `bm` guard for that reason -- mono_wasm_jit_rebatch frames stored bodies with no
		 * capture at all, and leaving the old import list on the entry would make admission demand
		 * instantiation of callees this generation no longer binds. Republished BEFORE the barrier below
		 * publishes the generation -- same visibility rule as mono_wasm_jit_register. */
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
	if (!mono_wasm_jit_admit (desc_ids [0])) {
		if (saved) {
			wj_batch_rollback (desc_ids, n, saved, bd);
			/* No registry entry can acquire this failed generation after rollback.  An
			 * admission that acquired it before rollback is covered by the registry-reader
			 * grace period, so the large module blob no longer has to leak forever. */
			wj_retire_payload (bytes, WJ_RETIRED_BYTES);
			g_free (saved);
			/* DO NOT FREE bd OR ITS ARRAYS -- USE AFTER FREE (2026-09-09). `re->batch = bd` was published
			 * to every worker above, so a worker already inside mono_wasm_jit_admit is holding
			 * `batch = re->batch` (:2816) and keeps reading batch->n, batch->desc[], batch->e/f and
			 * batch->bytes with no lock -- including at the `fail:` cleanup (:3088). wj_batch_rollback
			 * restores re->batch so no NEW reader can acquire bd, but it cannot recall the readers that
			 * already did. Freeing here produced `RuntimeError: memory access out of bounds` at :3088
			 * off a garbage batch->n, on a worker that entered admit from vcall_resolve_fslot -- 2 traps
			 * in a 5-run batch, 2026-09-09, symbolised against the split DWARF.
			 * The descriptor remains retained for that reason. Its much larger module bytes are retired
			 * through the admission-reader grace period above. Bounded: colocate_refused = 1 makes
			 * rollback once-only per method, and a descriptor is ~48 + 12*n bytes with n <= COLOCATE_MAX. */
			if (G_UNLIKELY (mono_wasm_jit_stats))
				mono_wasm_jit_add (WJC_COLOCATE_ROLLBACK, n);
			/* Also the string deploy.sh proves this build by -- see the rollback printf above. Retention
			 * is what this round changed, so it is the thing worth being able to count in a log. */
			if (mono_wasm_jit_verbose >= 2)
				printf ("WASM_JIT_BATCH_RETAIN members=%d (descriptor retained; module bytes retired after readers)\n", n);
			return 0;   /* failed generation was retired; no registry entry owns it */
		}
		printf ("WASM_JIT_BATCH_ADMIT_FAIL generation=%u members=%d\n", bd->generation, n);
	}
	/* Commit: the saved payloads are no longer reachable from the registry.  Retire
	 * each old module once (all members of an old batch share the same pointer) and
	 * the old immutable dependency sets after admission readers have quiesced. */
	if (saved) {
		/* Registry publication and the compatibility InterpMethod copy are one commit.
		 * Do this before retiring any previous shared blob so tier replacement can never
		 * copy a dangling wasm_jit_bytes pointer. */
		extern void mono_wasm_jit_repoint_imethod_bytes (gpointer, void *, int);
		for (i = 0; i < n; ++i) {
			WjRegEntry *re = wj_reg_at (desc_ids [i] - 1);
			if (re)
				mono_wasm_jit_repoint_imethod_bytes (re->logical_imethod, bytes, len);
		}
		for (i = 0; i < n; ++i) {
			WjRegEntry *re = wj_reg_at (desc_ids [i] - 1);
			int j;
			if (saved [i].depset && (!re || saved [i].depset != re->depset))
				wj_retire_payload (saved [i].depset, WJ_RETIRED_DEPSET);
			/* THE SUPERSEDED GROUP DESCRIPTOR, deduplicated the same way its bytes are: on a MERGE every
			 * absorbed member named the same old WjBatchDesc, so it must be retired once. This used to be
			 * retained on the success path as well as the failure path, and only the failure path has a
			 * reason to be (a concurrent admit may still be walking it; rollback is once-only per method,
			 * so that retention is bounded). On the success path it is unbounded: every re-frame of every
			 * group strands one, and re-framing is what re-emission does constantly. */
			if (saved [i].batch && (!re || saved [i].batch != re->batch)) {
				for (j = 0; j < i; ++j)
					if (saved [j].batch == saved [i].batch)
						break;
				if (j == i)
					wj_retire_payload (saved [i].batch, WJ_RETIRED_BATCH);
			}
			if (!saved [i].bytes || saved [i].bytes == bytes)
				continue;
			for (j = 0; j < i; ++j)
				if (saved [j].bytes == saved [i].bytes)
					break;
			if (j == i)
				wj_retire_payload (saved [i].bytes, WJ_RETIRED_BYTES);
		}
	}
	g_free (saved);
	return 1;
}

/*
 * CO-LOCATION FROM STORED BODIES -- re-frame n ALREADY-REGISTERED methods into one module.
 *
 * This is what the relocatable rewrite was for. Every batching driver in the tree works by re-running
 * mono_wasm_force_compile once per member (interp.c:2222, :2385, :2489, :2694) because a member's bytes
 * used to bake module-dependent indices. That per-member re-compile IS the +36% boot that sank every
 * batching arm ever measured here (-26.6% R61b, -35.7% R96, -13.0% R105, R123); module overhead never was
 * (29.4 us + 15.4 ns/byte, bytecost.mjs -- co-location REDUCES total instantiate cost).
 *
 * Here there is no compile at all: the members' relocatable bodies are on their registry entries, and
 * framing them together is wj_assemble, a memcpy pass over the instruction bytes with every hole re-encoded
 * against the new layout. Intra-set calls come out as `call <funcidx>`: one x86 instruction against
 * call_indirect's ~15, and the only form V8 will ever inline through (R128).
 *
 * Preconditions, all checked: every member is registered, holds a retained body, and owns a live slot pair.
 * The members keep their existing e/f slots -- mono_jiterp_allocate_table_entry is a bump allocator with no
 * free (R140-E17 records re-emit count as a leak axis), so a re-framing must allocate ZERO table entries.
 *
 * Returns 1 on success. On ANY failure nothing is rebound and the members keep the standalone modules they
 * already have, which are valid and installed -- a failed re-frame costs time, never correctness.
 */
int mono_wasm_jit_rebatch (const int *desc_ids, int n, void **out_bytes, int *out_len);
int
mono_wasm_jit_rebatch (const int *desc_ids, int n, void **out_bytes, int *out_len)
{
	WjAsmMember *members;
	WjAsmPolicy pol;
	WasmBuf out;
	int *e_slots, *f_slots;
	void *cached;
	char eb [192];
	double ms = 0;
	int i, ok = 0;

	wj_stack_probe ();
	if (n <= 1 || n > WJ_BATCH_MAX)
		return 0;
	for (i = 0; i < n; ++i) {
		WjRegEntry *re = desc_ids [i] > 0 ? wj_reg_at (desc_ids [i] - 1) : NULL;
		int j;
		if (!re || !re->body || re->e <= 0 || re->f <= 0)
			return 0;
		/*
		 * SPLITTING a batch generation is not supported -- its members share one WebAssembly.Instance and
		 * cannot be instantiated apart, so re-framing one member out of a group would strand the rest.
		 * MERGING splits nothing, and that distinction is what this guard used to conflate.
		 *
		 * The append-only partition it produced was measured (R192) as the ONLY thing refusing callees:
		 * 473 of 473 per-callee refusals were `dre->batch`, with the byte cap at 0 and the member cap
		 * firing once in 389 attempts. So a hot callee shared by five callers co-located with exactly one
		 * of them and the decision was never revised.
		 *
		 * Accept a batched member iff EVERY sibling of its group is also in this request. Then the group
		 * is absorbed whole and nothing is stranded: the union's members keep their own e/f slots, the
		 * new module exports all of them, and instantiate_batch_local installs the lot. It also makes the
		 * rollback sound -- wj_batch_rollback restores each member to saved[].batch, and every member of
		 * an absorbed group is present to be restored together.
		 */
		if (re->batch) {
			extern int mono_wasm_jit_colocate_merge;
			int k, m, complete = 1;
			for (k = 0; k < re->batch->n && complete; ++k) {
				for (m = 0; m < n; ++m)
					if (desc_ids [m] == re->batch->desc [k])
						break;
				if (m == n)
					complete = 0;
			}
			/*
			 * A PURE RE-FRAME IS NOT A MERGE, and conflating the two is what made re-emission structurally
			 * impossible in the shipped configuration.
			 *
			 * The knob test used to come FIRST and refuse any batched member outright when
			 * MONO_WASM_JIT_COLOCATE_MERGE is 0 -- which is what ships. But mono_wasm_jit_refresh_batch
			 * calls this with EXACTLY the members of one existing group, to rebuild that group around a
			 * replaced body. Every descriptor in the request is therefore batched, so the first one hit
			 * the early return and the whole thing failed before the sibling check below could observe
			 * that nothing was being split at all.
			 *
			 * MEASURED, 2026-09-19, worldwait 3 runs --stats --dumps on the shipped config: re-emission
			 * `queued=5020 ... compiled 44 + failed 4730`, with `reframe_fail=4678` accounting for
			 * essentially all of the failures -- i.e. 93% of mandatory IKVM generation-2 replacements
			 * never landed, and the reason was this line rather than anything about the bodies. ~71% of
			 * registered methods are in a group, so nearly every replacement met it.
			 *
			 * The guard's real subject is STRANDING: a group's members share one WebAssembly.Instance and
			 * cannot be instantiated apart, so framing SOME of them leaves the rest pointing at a module
			 * that no longer exports them. Re-framing a complete group strands nothing. Absorbing one
			 * group into a larger request is the MERGE the knob governs, and that is still gated.
			 */
			if (complete && n == re->batch->n) {
				/* The request is exactly this group: rebuild in place. */
			} else if (!complete) {
				/* A sibling is missing: this would be a SPLIT. Refuse the whole request. */
				if (G_UNLIKELY (mono_wasm_jit_stats))
					mono_wasm_jit_count (WJC_COLOCATE_MERGE_SPLIT);
				return 0;
			} else if (!mono_wasm_jit_colocate_merge) {
				/* Complete, but alongside other descriptors: absorbing this group into a bigger one. */
				if (G_UNLIKELY (mono_wasm_jit_stats))
					mono_wasm_jit_count (WJC_COLOCATE_MERGE_OFF);
				return 0;
			}
		}
		/* THIS DESCRIPTOR MUST STILL OWN ITS SLOTS. A method that was re-emitted registers a NEW descriptor
		 * into the SAME reserved e/f pair (mono_wasm_jit_self_reserved keeps the reservation on the imethod
		 * precisely so a re-emit reuses it), and the f-slot -> descriptor map then names the newer one. The
		 * older descriptor is still in the registry, still has a body, and still names those slots -- so
		 * framing it would install ITS exports over the slots the live descriptor owns.
		 *
		 * Do not weaken this to a warning. instantiate_batch_local writes the table unconditionally, and a
		 * slot that ends up holding a function of a different type is a wasm trap at the next call through
		 * it, reported at whatever unlucky frame gets there first. */
		if (wj_desc_for_fslot (re->f) != desc_ids [i]) {
			static int _stale = 0;
			if (_stale++ < 20)
				printf ("WASM_JIT_REBATCH_STALE desc=%d f=%d owned_by=%d (superseded descriptor; refusing to frame)\n",
					desc_ids [i], re->f, wj_desc_for_fslot (re->f));
			return 0;
		}
		/* And no two members may name the same slot, for the same reason: the second export written wins
		 * and the first member's callers keep calling what is now someone else's body. */
		for (j = 0; j < i; ++j) {
			WjRegEntry *pre = wj_reg_at (desc_ids [j] - 1);
			if (pre && (pre->e == re->e || pre->f == re->f || pre->e == re->f || pre->f == re->e))
				return 0;
		}
	}

	/* Every member's WjBody is borrowed by POINTER below -- name, param_types, extra_types, groups, and
	 * the two code buffers -- and held across wj_assemble and a JS WebAssembly.Module compile. A
	 * re-emission of any of those members retires that body. Held until `done:`. */
	wj_reg_read_enter ();
	members = g_new0 (WjAsmMember, n);
	e_slots = g_new0 (int, n);
	f_slots = g_new0 (int, n);
	/* The per-member import lists the assembler fills. Normally owned by the batch_begin/batch_end pair;
	 * there is no capture phase here, so this path owns them and frees them at `done`. */
	for (i = 0; i < n; ++i) {
		wj_asm_deps_free (&wj_batch_deps [i]);
	}
	for (i = 0; i < n; ++i) {
		WjRegEntry *re = wj_reg_at (desc_ids [i] - 1);
		WjBody *b = re->body;
		e_slots [i] = re->e;
		f_slots [i] = re->f;
		members [i].method = b->method;
		members [i].name = b->name;   /* resolved by whoever compiled this member; see WjBody.name */
		members [i].f_slot = re->f;
		members [i].f_body = &b->f_body;
		members [i].e_body = &b->e_body;
		members [i].param_types = b->param_types;
		members [i].nparams = b->nparams;
		members [i].ret_type = b->ret_type;
		members [i].extra_types = b->extra_types;
		members [i].nextra = b->nextra;
		members [i].groups = b->groups;
		members [i].nlocal_groups = 4;
		members [i].uses_calls = b->uses_calls;
		members [i].uses_eh_tag = b->uses_eh_tag;
		members [i].eh_tpool = b->eh_tpool;
	}

	{
	  /* local_calls is the POINT of co-locating -- and also the only thing MONO_WASM_JIT_COLOCATE_LOCAL_CALLS=0
	   * takes away, leaving the module, the slots, the rebind and the admission identical. */
	  /* MONO_WASM_JIT_COLOCATE_LOCAL_CALLS shipped 1. The entire point of framing members into one
	   * module is that calls between them become `call <funcidx>`; the losing arm framed them together
	   * and left the calls indirect, i.e. co-location's cost without its benefit. The policy FIELD
	   * stays -- the lone-method path passes FALSE deliberately, to keep self-recursion indirect. */
	  wj_asm_policy_init (&pol, TRUE);
	  /* CROSS-GROUP EDGES ARE `call_indirect`, and there is no longer a choice about it: method
	   * imports are gone entirely. An import bound at INSTANTIATION had to be ordered, and ordering
	   * is what could not be done -- R161 showed every deferral was an import-edge cycle, and R162
	   * measured what that cost. call_indirect reads the table when it runs, so any order works. */ }
	wasm_buf_init (&out);
	if (!wj_assemble (members, n, n, &pol, &out, NULL, wj_batch_deps) || out.len == 0) {
		wasm_buf_free (&out);
		goto done;
	}
	cached = g_malloc ((gsize) out.len);
	memcpy (cached, out.data, out.len);

	eb [0] = 0;
	if (!mono_wasm_jit_instantiate_batch_local (e_slots, f_slots, n, cached, (int) out.len, eb, (int) sizeof (eb), &ms)) {
		/* The old generation is still in this worker's slots -- instantiate_batch_local installs nothing
		 * on failure -- so declining here really does leave the members exactly as they were. */
		printf ("WASM_JIT_REBATCH_FAIL members=%d bytes=%u : %s\n", n, (unsigned) out.len, eb);
		g_free (cached);
		wasm_buf_free (&out);
		goto done;
	}
	ok = mono_wasm_jit_batch_bind (desc_ids, e_slots, f_slots, n, cached, (int) out.len);
	if (!ok) {
		/* NOT `g_free (cached)` -- the same use-after-free as the batch descriptor. batch_bind
		 * published re->batch = bd with bd->bytes = cached BEFORE it attempted admission, and a
		 * concurrent mono_wasm_jit_admit reads batch->bytes to instantiate. Freeing the module bytes
		 * under it is worse than the descriptor case: it instantiates freed memory rather than just
		 * faulting. The `else` arm below already states this rule for the PREVIOUS generation's bytes;
		 * the failed generation's bytes need it for the same reason. (The g_free (cached) a few lines
		 * up is on the pre-bind path, where re->batch was never published, and stays.) */
	} else {
		/* Hand the shared blob back so the driver can repoint each member's imethod at it. Leaving the
		 * imethods pointing at their discarded standalone modules is not a dangling read -- batch_bind
		 * deliberately does not free the old bytes, because another worker may still be walking the
		 * previous generation. (This used to warn that the registry-overflow fallback in
		 * mono_wasm_jit_ensure_fslot could reinstall the STANDALONE generation over this one; that
		 * function had no callers and is deleted, so the hazard is gone with it.) */
		if (out_bytes) *out_bytes = cached;
		if (out_len) *out_len = (int) out.len;
		if (mono_wasm_jit_verbose >= 2)
			printf ("WASM_JIT_REBATCH members=%d bytes=%u instantiate=%.2fms\n", n, (unsigned) out.len, ms);
	}
	wasm_buf_free (&out);
done:
	/* batch_bind has COPIED whatever it needed into each entry's re->imports, so these are ours to drop
	 * on every path -- including the ones that never reached it. */
	for (i = 0; i < n; ++i) {
		wj_asm_deps_free (&wj_batch_deps [i]);
	}
	g_free (members); g_free (e_slots); g_free (f_slots);
	wj_reg_read_leave ();
	return ok;
}

/*
 * PUT THIS WORKER BACK ON THE GROUP'S MODULE.
 *
 * Emitting a replacement for a batched member ends with a validating
 * mono_wasm_jit_instantiate_local of the STANDALONE module, which writes this worker's table at the
 * member's e/f pair -- over the group's entries. That is fine when the group is then re-framed, because
 * the re-frame reinstalls all of them. When the re-frame FAILS it is not: this worker alone runs
 * generation 2 at that slot while every other worker, and this entry's own depset, still describe
 * generation 1. Generation 2's direct calls may name f-slots nobody admitted here.
 *
 * Re-instantiating the group's existing module restores the coherent state. No admission is needed: this
 * closure was admitted when the group was installed and nothing about it has changed.
 */
static void
wj_reinstall_batch_local (int desc_id)
{
	extern int mono_wasm_jit_instantiate_batch_local (const int *e_slots, const int *f_slots, int n,
	                                                  const void *bytes, int len, char *errbuf, int errcap,
	                                                  double *out_ms);
	WjRegEntry *re;
	WjBatchDesc *bd;
	char eb [192];
	double ms = 0;

	wj_reg_read_enter ();
	re = (desc_id > 0 && desc_id <= wj_reg_n) ? wj_reg_at (desc_id - 1) : NULL;
	bd = re ? re->batch : NULL;
	if (bd && bd->bytes && bd->len > 0) {
		eb [0] = 0;
		if (!mono_wasm_jit_instantiate_batch_local (bd->e, bd->f, bd->n, bd->bytes, bd->len,
		                                            eb, (int) sizeof (eb), &ms))
			printf ("WASM_JIT_REFRAME_UNDO_FAIL desc=%d members=%d : %s\n", desc_id, bd->n, eb);
	}
	wj_reg_read_leave ();
}

/* Re-frame the complete batch currently containing desc_id after one member's retained body changed.
 * Returns 0 for a standalone descriptor, 1 on a successful batch refresh, and -1 for a transient failure.
 * The descriptor list is copied under the registry lock; framing/instantiation stays outside it. */
int
mono_wasm_jit_refresh_batch (int desc_id, void **out_bytes, int *out_len)
{
	WjRegEntry *re;
	int *descs = NULL, n = 0, ok;

	/* re->batch is a retireable pointer and the loader lock does not protect it from reclamation --
	 * different lock, different question. The copy is taken under the reader bracket for that reason;
	 * `descs` is then ours and rebatch re-reads everything it needs from the registry itself. */
	wj_reg_read_enter ();
	mono_loader_lock ();
	re = (desc_id > 0 && desc_id <= wj_reg_n) ? wj_reg_at (desc_id - 1) : NULL;
	if (re && re->batch) {
		n = re->batch->n;
		descs = g_new (int, n);
		memcpy (descs, re->batch->desc, sizeof (int) * (gsize) n);
	}
	mono_loader_unlock ();
	wj_reg_read_leave ();
	if (!descs)
		return 0;
	ok = mono_wasm_jit_rebatch (descs, n, out_bytes, out_len);
	g_free (descs);
	if (ok)
		mono_wasm_jit_counters [WJC_REFRAME_OK]++;
	else
		wj_reinstall_batch_local (desc_id);
	return ok ? 1 : -1;
}

/* Refusal accounting for co-location. Stats-gated like every other WJC_* bump, so a clean timing run
 * pays nothing -- which means these numbers exist only in a `--stats` arm, and "the counter read 0" is
 * a statement about that arm and not about the run. */
#define WJ_CO_COUNT(c) do { if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (c); } while (0)

/*
 * Group a freshly published method with the direct callees it can still be grouped with.
 *
 * This is the trigger that gives co-location reach. It runs once per method, at publish, from information
 * the entry already holds, and it never revises a decision: a callee that already shares a module is
 * skipped, so each method joins at most one group and the partition is append-only. No planner, no
 * quiescence test, no candidate array, no global walk -- the properties that made the previous planner fire
 * once, on a trigger this workload never satisfies, and reach 14% of the tier.
 *
 * Deliberately NOT recursive: only the publishing method's OWN direct callees are considered. Following the
 * callees' callees would grow a group along whatever edge happened to publish first, which is a partition
 * decision that wants a profile behind it.
 *
 * Returns the number of members grouped (0 if nothing was).
 */
/* GLOBAL REPARTITION IS DELETED -- wj_repart_seed_order, wj_repartition_step, wj_repartition_decide,
 * wj_repartition_tick_locked, mono_wasm_jit_repartition_tick, their state, five knobs (REPARTITION,
 * _AUTO, _AGAIN, _MIN_GAIN, _BUDGET) and seven counters.
 *
 * The IDEA was right and stays on the record: the online, incremental co-location below is a greedy
 * first-come partition, and measured on the tier's own reloc call graph a wholesale seed-and-grow
 * partition reaches 83.6% of EXECUTED call weight internal at 16 members and 88.8% at 32 -- weighting
 * edges by execution makes it BETTER, not worse (85.5% / 89.7%), because the hubs that dominate the
 * static picture are cold. Seeding by OUT-DEGREE descending is what makes that work; seeding by
 * ascending descriptor id gives 0% internal, since callees hold lower ids than their callers by
 * construction and every group comes out a singleton.
 *
 * What closed it (R214) is not the partition quality but the DELIVERY: repeatedly re-framing a LIVE
 * tier is unsafe under the current admission model. It produced an out-of-bounds trap and then
 * `function signature mismatch` x4. It also required COLOCATE_DEPS=0 -- rebatch refuses an
 * already-batched member, so incremental co-location running alongside recreates the committed-
 * neighbour problem this existed to avoid -- and that combination wedged 2 of 2 attempts.
 *
 * The lead this becomes is per-CLASS grouping, which gets the partition without the re-framing: 47.7%
 * of executed intra-tier call edges are intra-class (reproduced to 0.25% across two captures), a
 * member cap of 16 keeps 98.8% of that weight, only ~315 classes carry any of it, and class membership
 * is INTRINSIC -- every method belongs to exactly one class, known at emit time, so the partition
 * conflict R195 identified ("a hot callee shared by five callers can co-locate with only one") does not
 * arise. Cap 8 buys 96.8% of the weight for 1.24 MB of cold siblings, 2.2% of the tier. Do that by
 * changing the co-location KEY, not by re-framing what is already live. */

int mono_wasm_jit_colocate_deps_now (int desc_id);
int
mono_wasm_jit_colocate_deps_now (int desc_id)
{
	extern int mono_wasm_jit_colocate_max, mono_wasm_jit_colocate_bytes;
	WjDepSet *rds;
	/* HEAP, not stack, and this is a bug fix rather than tidiness. `descs` only ever holds `cap` entries
	 * (COLOCATE_MAX, 16 by default), but a WJ_BATCH_MAX-sized automatic is 2 KB of C stack in a function
	 * that sits at the bottom of
	 *   maybe_compile -> force_compile -> ... -> compile_publish -> colocate_deps_now -> rebatch -> wj_assemble
	 * Adding 2 KB beside it (a staging array for the merge closure) overflowed that chain and presented
	 * as 8x `RuntimeError: memory access out of bounds` (R194). Enabling HEAL_WAIT (now deleted) length-
	 * ened the SAME chain by the re-emit frames and reproduced it at 12 faults with the array already
	 * removed -- so the remaining 2 KB is itself the margin. Size the allocation by `cap` and the
	 * footprint drops from
	 * 2048 bytes to 64. */
	int *descs = NULL;
	WjRegEntry *re;
	int n = 0, bytes = 0, i, j, cap;

	wj_stack_probe ();
	/* CO-LOCATION IS UNCONDITIONAL. This used to say "MONO_WASM_JIT_COLOCATE_DEPS ships 1"; R245 checked
	 * and there is no such getenv and no such variable anywhere in the tree, so the knob it names cannot
	 * be set and its off-arm cannot be run. Several results in MINECRAFT-FINDINGS rest on A/Bs against
	 * that arm; they were taken when it existed and are not reproducible now.
	 *
	 * The objection to batching was never module overhead -- 29.4 us + 15.4 ns/byte, so co-location
	 * REDUCES total instantiate cost -- it was that producing a batched member cost a full
	 * mono_wasm_force_compile; with relocatable bodies it costs a memcpy. */
	if (desc_id <= 0)
		return 0;
	/* THE BRACKET, and it is not decoration: `rds` below is re->depset, it is dereferenced for the whole
	 * body of this function -- across wj_asm_reaches walking OTHER descriptors' depsets, across a rebatch,
	 * across wj_assemble and a JS WebAssembly.Module compile -- and a concurrent re-registration retires
	 * exactly that pointer. Every `return` from here on must pass through the leave below. */
	wj_reg_read_enter ();
	re = wj_reg_at (desc_id - 1);
	rds = re ? re->depset : NULL;
	/* Structural ineligibility -- no entry, no retained body, no slot pair -- is not a co-location
	 * REFUSAL and is deliberately outside TRY: there is nothing here that a planner could ever group.
	 * TRY starts once we have a framable descriptor, so that the refusal arms below sum to it. */
	if (!re || !re->body || re->e <= 0 || re->f <= 0) {
		wj_reg_read_leave ();
		return 0;
	}
	WJ_CO_COUNT (WJC_COLOCATE_TRY);
	if (re->batch) {
		WJ_CO_COUNT (WJC_COLOCATE_SELF_BATCHED);
		wj_reg_read_leave ();
		return 0;
	}
	if (re->colocate_refused) {
		WJ_CO_COUNT (WJC_COLOCATE_SELF_REFUSED);
		wj_reg_read_leave ();
		return 0;
	}
	if (!rds || rds->n <= 0) {
		WJ_CO_COUNT (WJC_COLOCATE_SELF_NO_DEPSET);
		wj_reg_read_leave ();
		return 0;
	}
	cap = mono_wasm_jit_colocate_max;
	if (cap > WJ_BATCH_MAX)
		cap = WJ_BATCH_MAX;
	if (cap < 2)
		cap = 2;
	descs = g_new0 (int, cap);
	descs [n++] = desc_id;
	bytes += re->len > 0 ? re->len : 0;

	for (i = 0; i < rds->n && n < cap; ++i) {
		int d = wj_desc_for_fslot (rds->slot [i]);
		WjRegEntry *dre = d > 0 ? wj_reg_at (d - 1) : NULL;
		gboolean excluded = FALSE;
		if (!dre || dre->e <= 0 || dre->f <= 0) {
			excluded = TRUE;   /* callee not registered here (AOT / main-module / not yet published) */
			WJ_CO_COUNT (WJC_COLOCATE_DEP_UNREG);
		} else if (!dre->body) {
			excluded = TRUE;   /* registered but its relocatable body was not retained */
			WJ_CO_COUNT (WJC_COLOCATE_DEP_NOBODY);
						} else if (dre->batch) {
							/*
							 * MERGE instead of drop. This was the append-only partition, and R192 measured it as the
							 * ONLY thing refusing callees -- 473 of 473, with the byte cap at 0 and the member cap
							 * firing once in 389 attempts. A hot callee shared by five callers co-located with exactly
							 * one of them, permanently.
							 *
							 * Absorb the callee's ENTIRE group into the request. Whole-group or not at all:
							 * mono_wasm_jit_rebatch refuses a partial absorption because that would be a SPLIT, and
							 * wj_batch_rollback can only restore an absorbed member if every sibling is present to be
							 * restored with it.
							 */
							WjBatchDesc *ob = dre->batch;
							extern int mono_wasm_jit_colocate_merge;
							int k, n_before = n, bytes_before = bytes, ok_all = mono_wasm_jit_colocate_merge;
							/*
							 * APPEND STRAIGHT INTO `descs` AND ROLL BACK `n` ON FAILURE -- do NOT stage the
							 * siblings in a second array. The first version of this used
							 * `int addl [WJ_BATCH_MAX]`, which is 2 KB of C stack on top of the 2 KB `descs`
							 * already here, in the chain force_island -> compile_publish -> colocate_deps_now ->
							 * rebatch -> wj_assemble. That overflowed the wasm C stack and presented as
							 * `RuntimeError: memory access out of bounds` x8 with this function on the stack --
							 * a Minecraft-only failure, because jbox2d's smaller tier never built groups deep
							 * enough to reach it (and it passed there, 567 members, checksum clean).
							 *
							 * Rolling `n` back is exactly equivalent and costs nothing: nothing downstream reads
							 * descs[n..] and the entries above n are simply not part of the request.
							 */
							for (k = 0; ok_all && k < ob->n; ++k) {
								int sd = ob->desc [k];
								WjRegEntry *sre = sd > 0 ? wj_reg_at (sd - 1) : NULL;
								int j2;
								/* Pre-check the cheap preconditions rebatch will re-check anyway: forming a
								 * request rebatch is certain to refuse costs an assemble pass for nothing. */
								if (!sre || !sre->body || sre->e <= 0 || sre->f <= 0 || sre->colocate_refused) {
									ok_all = 0; WJ_CO_COUNT (WJC_COLOCATE_MERGE_PRECOND); break;
								}
								for (j2 = 0; j2 < n; ++j2)
									if (descs [j2] == sd) break;
								if (j2 < n)
									continue;          /* already in the request (incl. ones just appended) */
								if (n >= cap) { ok_all = 0; WJ_CO_COUNT (WJC_COLOCATE_MERGE_CAP); break; }
								descs [n++] = sd;
								bytes += sre->len > 0 ? sre->len : 0;
							}
							if (ok_all && bytes > mono_wasm_jit_colocate_bytes)
								WJ_CO_COUNT (WJC_COLOCATE_MERGE_CAP);
							if (ok_all && bytes <= mono_wasm_jit_colocate_bytes) {
								WJ_CO_COUNT (WJC_COLOCATE_MERGED);
								/* The callee is now in the request as a member of its absorbed group, so no
								 * edge is left behind and the SCC guard below has nothing to protect. */
								continue;
							}
							n = n_before;
							bytes = bytes_before;
							excluded = TRUE;
							WJ_CO_COUNT (WJC_COLOCATE_DEP_BATCHED);
						} else if (dre->colocate_refused) {
			excluded = TRUE;   /* a previous group containing it was rolled back; once-only brake */
			WJ_CO_COUNT (WJC_COLOCATE_DEP_REFUSED);
		} else {
			for (j = 0; j < n; ++j)
				if (descs [j] == d)
					break;
			if (j < n)
				continue;   /* self, or the same callee reached twice: no edge to leave behind */
			if (bytes + (dre->len > 0 ? dre->len : 0) > mono_wasm_jit_colocate_bytes) {
				excluded = TRUE;
				WJ_CO_COUNT (WJC_COLOCATE_DEP_BYTE_CAP);
			}
		}
		if (excluded) {
			/* LEAVING THIS CALLEE OUT MAY STRAND A CYCLE ACROSS TWO MODULES. A cycle kept INSIDE one
			 * module has no edge to order and is fine -- which is the whole reason SCC co-location exists
			 * as a correctness mechanism.
			 *
			 * THE ORIGINAL PREMISE IS STALE (R245). "A cross-module cycle is the one thing admission
			 * cannot order -- an import inside it defers to WJ_ADMIT_DEFER_MAX and then fails permanently
			 * (R161)" was written when a generated module IMPORTED its callees and imports had to be bound,
			 * in order, at instantiation. R163 removed method imports entirely; wj_admit_install_only now
			 * states the consequence outright -- instantiation is SAFE IN ANY ORDER, and only "let A be
			 * CALLED before B is installed" is wrong. So the ordering hazard this guard was built for is
			 * gone.
			 *
			 * The guard is kept anyway, and honestly: the admission DFS still handles a cross-module cycle
			 * badly, because its state-1 cycle break INSTALLS a dependency without publishing its liveness,
			 * so a caller admitted through it finds the slot not live and refuses -- forever (R244). That is
			 * a fixable property of the DFS, not of module boundaries. When it is fixed, re-measure whether
			 * this refusal is still buying anything: WJC_COLOCATE_SCC_REFUSED is the counter to read.
			 *
			 * So if the callee we are about to drop can reach back to us, do not form the group at all.
			 * Refusing is strictly better than forming one that cannot be admitted: the members keep the
			 * standalone modules they already have, at exactly the cost of not co-locating them.
			 *
			 * wj_asm_reaches walks the same WjRegEntry.deps graph admission walks, so the two agree by
			 * construction, and it is conservative (TRUE on any doubt, and on exceeding WJ_REACH_MAX). */
			/* MONO_WASM_JIT_COLOCATE_SCC is CORRECTNESS, not performance: dropping a callee that can
			 * reach back to us strands a cycle ACROSS two modules. Refusing to form the group is strictly
			 * better; the members keep the standalone modules they already have. See the block above for
			 * why the ORIGINAL justification (import binding order, WJ_ADMIT_DEFER_MAX, R161) no longer
			 * applies, and what replaced it. NB: there is no MONO_WASM_JIT_COLOCATE_SCC getenv anywhere --
			 * it is unconditional, so the in-binary A/B this comment implies cannot be run. */
			if (d > 0 && wj_asm_reaches (d, re->f)) {
				if (G_UNLIKELY (mono_wasm_jit_stats))
					mono_wasm_jit_count (WJC_COLOCATE_SCC_REFUSED);
				g_free (descs);
				wj_reg_read_leave ();
				return 0;
			}
			continue;
		}
		descs [n++] = d;
		bytes += dre->len > 0 ? dre->len : 0;
	}
	/* The loop stops at `cap`, so a callee list longer than the cap is a MEMBER_CAP refusal even though
	 * a group did form. Counted once per call, not per dropped callee: what it answers is "is
	 * MONO_WASM_JIT_COLOCATE_MAX binding", and the per-callee volume would confuse that with reach. */
	if (n >= cap && i < rds->n)
		WJ_CO_COUNT (WJC_COLOCATE_MEMBER_CAP);
	/* R211's MULTI-HOP FRONTIER EXPANSION stood here, behind MONO_WASM_JIT_COLOCATE_HOPS, and it is
	 * DELETED as INERT AT EVERY VALUE. It grew the group past the seed's immediate callees by walking
	 * `body->f_body.relocs` breadth-first -- deliberately the relocations and not the depset, because
	 * the tight-dep republish is UNCONDITIONAL and empties a member's depset once its group is framed,
	 * which is why the first version measured 12 members added across a whole run.
	 *
	 * The offline result that motivated it is real and worth keeping: on the tier's own reloc call graph
	 * a seed-and-grow partition with a frontier reaches 79%% of call edges internal at 16 members and
	 * 84%% at 32, and weighting by EXECUTION makes it BETTER, not worse (85.5%% / 89.7%%) -- the hubs
	 * that dominate the static picture are cold, so by execution the graph is densely clustered, which
	 * is the shape a partition wants.
	 *
	 * What the online walk could not do is change the fact that co-location is a PARTITION (R195): a hot
	 * callee shared by five callers joins one group whatever the hop count, so more hops just reach
	 * further into an already-claimed graph. That is why every value measured the same. Per-class
	 * grouping is the version that gets a partition without needing a walk at all. */
	if (n < 2) {
		WJ_CO_COUNT (WJC_COLOCATE_SINGLETON);
		g_free (descs);
		wj_reg_read_leave ();
		return 0;
	}
	if (!mono_wasm_jit_rebatch (descs, n, NULL, NULL)) {
		WJ_CO_COUNT (WJC_COLOCATE_REBATCH_FAIL);
		g_free (descs);
		wj_reg_read_leave ();
		return 0;
	}
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		mono_wasm_jit_add (WJC_COLOCATED_MEMBERS, n);
		mono_wasm_jit_count (WJC_COLOCATE_FORMED);
	}
	g_free (descs);
	wj_reg_read_leave ();
	return n;
}

/* The MonoMethod captured as member i, so the driver can register the right slot pair against it. */

#endif /* HOST_BROWSER */
