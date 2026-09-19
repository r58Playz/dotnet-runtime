/* Asynchronous code-generation publication. Included by mini-wasm.c so the registry and per-worker
 * admission state remain private to one translation unit while publication evolves independently. */

/* ================================================================================================== *
 * ASYNCHRONOUS REPUBLICATION
 *
 * The problem this exists for, stated once. A JITted method's bytes are published process-wide, but each
 * worker instantiates its own WebAssembly.Instance and writes its own function table -- ONE WORKER CANNOT
 * WRITE ANOTHER'S -- and generated code call_indirects an f-slot with NO liveness check, by design. So a
 * live f-slot is repointed by its owning worker. Publication appends an immutable generation record and
 * each worker adopts pending records at a JIT safepoint before returning to managed code. Workers need not
 * synchronize with one another: an old frame may finish against a coherent old generation while another
 * worker has already installed the replacement.
 *
 * WHAT IS REUSED RATHER THAN BUILT. Two things already exist and are proven:
 *   - the GENERATION mechanism: wj_desc_generation[desc] != re->generation already makes a worker drop its
 *     admission cache and re-instantiate (mono_wasm_jit_admit). Bumping re->generation is the whole
 *     publication step.
 *   - mono_wasm_jit_admit itself does the re-instantiation, in dependency order, with every refusal
 *     counted. The drain below calls it rather than reimplementing a second, less-tested installer.
 * The additional guarantee is the emitted epoch check: a worker cannot pass a JIT safepoint with a
 * publication it has not adopted.
 *
 * WHY AN EPOCH AND A LOG, not a single "current batch". A thread drains at a JIT safepoint or managed
 * re-entry. A thread in GC-SAFE can sit out SEVERAL publications
 * -- it is not executing JIT code meanwhile, so its stale entries are harmless until it returns -- and
 * then must catch up on ALL of them. A single slot would lose the earlier ones. The log is append-only and
 * never freed, per this file's ownership rule: it is read lock-free by every worker and nothing recalls it.
 * ================================================================================================== */

/* One rendezvous: the descriptors republished, and the epoch that publication established. */
typedef struct {
	gint32  epoch;
	gint32  n;
	gint32 *desc;     /* owned, never freed */
} WjRendezvous;

#define WJ_RV_LOG_MAX 4096          /* rendezvous per process; see wj_rv_log_full */
static WjRendezvous  wj_rv_log [WJ_RV_LOG_MAX];
static volatile gint32 wj_global_epoch;      /* published LAST, after the log entry's contents */
static gint32 wj_rv_log_full;                /* 1 once the log is exhausted: refuse further rendezvous */

/* This thread's position in the log, and the re-entrancy guard. Both __thread: the drain runs on the
 * thread that owns the table it is fixing, and nothing else reads them. */
static __thread gint32 wj_local_epoch;
static __thread int    wj_draining;
/* Has THIS thread ever drained? Turns the global drain count into a participation count, which is the
 * number the positive control actually needs -- "some thread drained 500 times" and "every thread drained"
 * are different claims and only the second one proves the rendezvous reached the whole fleet. */
static __thread int    wj_drained_ever;

WJ_KEEPALIVE gint32 *
mono_wasm_jit_worker_epoch_addr (void)
{
	return &wj_local_epoch;
}

/*
 * Catch this thread up to the global epoch. Called from the three coop re-entry points, i.e. from every
 * place a thread can resume executing managed code after having been stopped or blocked.
 *
 * FAST PATH IS ONE GLOBAL LOAD AND ONE COMPARE. wj_global_epoch is 0 until the first rendezvous, so a
 * process that never republishes pays exactly that, at sites that are already out-of-line calls doing CAS
 * work.
 *
 * RE-ENTRANCY IS REAL AND IS GUARDED. The drain calls mono_wasm_jit_admit, which instantiates, which now
 * enters a GC-SAFE region (the Phase 1 bracket) -- and leaving that region is ITSELF one of the three
 * call sites. Without wj_draining this recurses until the stack dies. The guard makes the nested call a
 * no-op; the outer drain is still walking the log and will finish the work.
 */
void mono_wasm_jit_rendezvous_drain (void);
void
mono_wasm_jit_rendezvous_drain (void)
{
	gint32 target, i, completed;
	gboolean retry = FALSE;

	target = wj_global_epoch;
	if (G_LIKELY (wj_local_epoch == target))
		return;
	if (wj_draining)
		return;
	wj_draining = 1;
	completed = wj_local_epoch;

	/* Walk every rendezvous this thread has not yet seen, oldest first. Oldest first matters: a
	 * descriptor republished twice must end on the LATER bytes, and admit resolves against the registry's
	 * CURRENT generation, so replaying in order costs at most one redundant instantiation and replaying
	 * out of order could leave the earlier one installed. */
	for (i = wj_local_epoch; i < target && i < WJ_RV_LOG_MAX; ++i) {
		WjRendezvous *rv = &wj_rv_log [i];
		gint32 k;
		if (!rv->desc)
			continue;
		for (k = 0; k < rv->n; ++k) {
			int desc_id = rv->desc [k];
			if (desc_id <= 0)        /* dropped at publish time (no entry, or co-located) */
				continue;
			/* CLEAR THE INSTALLED BITS AND NOTHING ELSE, then let admit do the work.
			 *
			 * Clearing the bits is what forces a real re-instantiation rather than a no-op:
			 * wj_mark_slot_live refuses an uninstalled slot, so admit's liveness publication depends on
			 * the instantiate actually running.
			 *
			 * DO NOT ALSO POKE wj_desc_state / wj_desc_generation HERE. The first version did, setting
			 * generation to (guint32)-1 to "force the mismatch arm", and that was a RETRY STORM: admit
			 * only writes wj_desc_generation on SUCCESS, so a descriptor whose re-admission failed kept
			 * -1 forever, and -1 never equals re->generation -- which made admit's "a NEW generation
			 * earns a fresh attempt" arm fire on EVERY dispatch, defeating the state-3 permanent refusal
			 * and its permfail bound outright. Measured: admitDepAdmit0 332,376,959 and admitState3
			 * 33,249,377 in one world.generate against 2 and 0 with the knob off, and the run wedged.
			 * This is CLAUDE.md's "routing a PERMANENT condition into the retry path spins forever",
			 * arrived at from the other direction.
			 *
			 * None of it was needed. Publication already bumped re->generation before exposing the epoch, so this
			 * thread's cached generation mismatches by construction; admit's own generation arm resets
			 * the state, keeps the permfail bound, and writes the generation back when it succeeds. */
			WjRegEntry *re = (desc_id > 0 && desc_id <= wj_reg_n) ? wj_reg_at (desc_id - 1) : NULL;
			if (!re)
				continue;
			/* ONLY THREADS THAT ACTUALLY HAD IT INSTALLED RE-ADMIT. A thread that never instantiated this
			 * module has nothing stale in its table -- there is no old body for it to keep calling -- so
			 * re-admitting would be pure work, and worse than pure work:
			 *
			 * admit walks the whole direct-call closure and can refuse for entirely ordinary reasons on a
			 * thread that has never needed this method (a dependency not registered here yet). A REFUSAL
			 * LEAVES THE CACHED GENERATION MISMATCHED, because admit writes wj_desc_generation only on
			 * success -- so every later dispatch takes the generation arm again and re-attempts, forever.
			 * That is R244's shape exactly: a condition that cannot clear, routed into the retry path.
			 * Measured at 42% of re-admissions refused (275 of 654 in one run) and admitState3 going from
			 * 0 with the knob off to 13.5M with it on, at matched world.generate duration.
			 *
			 * Threads that DO have it installed are the entire population that matters: they are the only
			 * ones holding a stale table entry, which is the whole thing a rendezvous exists to replace. */
			if (!wj_slot_is_installed (re->e) && !wj_slot_is_installed (re->f)) {
				/* Record the generation as seen so this descriptor stops taking the mismatch arm on
				 * every dispatch. State is left untouched: 0 means untried, and the ordinary lazy
				 * admission path will install it if this thread ever does need it. */
				wj_desc_state_ensure (desc_id);
				if (desc_id < wj_desc_state_cap) {
					/* STATE MUST NOT CLAIM CALLABLE WHILE THE BITMAP SAYS OTHERWISE. The line below
					 * records the new generation; combined with state 2 that is precisely what admit's
					 * fast path accepts -- and it trusts the STATE, not the bitmap -- so it would return
					 * 1 for a descriptor whose slots this worker never wrote, and the caller's
					 * call_indirect would hit the jiterpreter prefill. R165 recorded exactly that
					 * ("state 2 with neither slot installed, holding the prefill"); this is the same
					 * failure reachable through the drain. Downgrade 2 -> 0 = "untried at this
					 * generation", which is what the skip arm actually means. Leave 3 alone: clearing a
					 * permanent refusal would route it back into the retry path (R244). */
					if (wj_desc_state [desc_id] == 2) {
						wj_desc_state [desc_id] = 0;
						mono_wasm_jit_counters [WJC_RV_DRAIN_STALE2]++;
					}
					wj_desc_generation [desc_id] = re->generation;
				}
				mono_wasm_jit_counters [WJC_RV_DRAIN_SKIPPED]++;
				continue;
			}
			/* DO NOT CLEAR THE INSTALLED BITS.
			 *
			 * Clearing them and then re-admitting opens a window in which the slot is marked uninstalled
			 * while every caller that already admitted this descriptor is still published: if the
			 * re-admit below refuses for any ordinary transient reason, the slot STAYS uninstalled, and
			 * those callers' call_indirect hits mono_jiterp_placeholder_jit_call. Observed directly --
			 * `desc=1634 gen=427` published with `dep[3] -> desc 1748 inst=0`, a dependency that had been
			 * installed when 1634 was admitted and was un-installed afterwards. `installed` is otherwise
			 * monotonic, so this was the only thing that could have done it.
			 *
			 * And the clear is not needed: the rendezvous has already bumped re->generation, and
			 * wj_admit_install_only re-instantiates whenever the recorded generation differs, regardless
			 * of the installed bits. It bought nothing and cost the window. */
			/* THE STATE MUST NOT OUTLIVE THE BITS WE JUST CLEARED. admit's fast path returns 1 on
			 * `state == 2 && generation matches` WITHOUT consulting the liveness bitmap, so if this
			 * thread lazily admitted the descriptor after the rendezvous bumped re->generation but before
			 * reaching this drain, the call below short-circuits and the slots we just cleared are never
			 * reinstalled -- leaving the jiterpreter prefill under a live caller. See
			 * WJC_RV_DRAIN_REFRESH_FORCED. State 3 is left alone so the permfail bound survives. */
			if (desc_id < wj_desc_state_cap && wj_desc_state [desc_id] == 2) {
				wj_desc_state [desc_id] = 0;
				mono_wasm_jit_counters [WJC_RV_DRAIN_REFRESH_FORCED]++;
			}
			if (mono_wasm_jit_admit (desc_id))
				mono_wasm_jit_counters [WJC_RV_DRAIN_SLOT]++;
			else {
				mono_wasm_jit_counters [WJC_RV_DRAIN_REFUSED]++;
				retry = TRUE;
				break;
			}
		}
		if (retry)
			break;
		completed = i + 1;
	}
	/* Never acknowledge a generation whose slots were not installed. The generated epoch check will take
	 * this slow path again at the next safepoint; acknowledging here would make an old table entry look
	 * current and turn a transient installation failure into stale-code execution. */
	wj_local_epoch = retry ? completed : target;
	wj_draining = 0;

	mono_wasm_jit_counters [WJC_RV_DRAIN]++;
	if (!wj_drained_ever) {
		wj_drained_ever = 1;
		mono_wasm_jit_counters [WJC_RV_DRAIN_THREADS]++;
	}
}

/* One emitted call handles both reasons a JIT safepoint can be taken. Publication does not request a GC
 * suspension, so generation adoption must not be hidden behind mono_threads_state_poll's slow path. */
void
mono_wasm_jit_safepoint_poll (void)
{
	extern volatile size_t mono_polling_required;
	extern void mono_threads_state_poll (void);
	if (mono_polling_required)
		mono_threads_state_poll ();
	mono_wasm_jit_rendezvous_drain ();
}

/*
 * Republish `n` descriptors at their EXISTING e/f slots. Every worker adopts the record independently at
 * its next JIT safepoint or managed re-entry. Returns 1 on success, 0 if publication was refused.
 *
 * The caller is responsible for having already written the new bytes into re->bytes/re->len (or for
 * republishing the same bytes, which is what the positive control does). This function owns only the
 * ordering: contents, fence, then the epoch that makes them reachable -- the same discipline
 * mono_wasm_jit_register and mono_wasm_jit_batch_bind use, for the same reason.
 */
int mono_wasm_jit_rendezvous (const int *descs, int n);
int
mono_wasm_jit_rendezvous (const int *descs, int n)
{
	gint32 epoch;
	gint32 *copy;
	int i, nexp = 0, published = 0;

	if (n <= 0 || !descs)
		return 0;
	if (wj_rv_log_full) {
		mono_wasm_jit_counters [WJC_RV_LOG_FULL]++;
		return 0;
	}

	/* Cheap pre-check; the authoritative read is under the publication lock below. */
	if (wj_global_epoch >= WJ_RV_LOG_MAX) {
		wj_rv_log_full = 1;
		mono_wasm_jit_counters [WJC_RV_LOG_FULL]++;
		return 0;
	}

	/* Do not recursively publish while this worker is adopting a generation. */
	if (wj_draining)
		return 0;

	/* Expand complete groups before taking the short publication lock:
	 *
	 * 1. A CO-LOCATED MEMBER CANNOT BE REPUBLISHED ALONE -- admission compares a batched member against
	 *    its BATCH's generation, not its entry's -- so the old code refused it. Since ~71% of registered
	 *    methods end up in a group, re-emission met that immediately: `RV count=0` with every attempt
	 *    refused (2026-09-17). The fix is the one R256 named: republish the whole GROUP. Every member is
	 *    re-instantiated together, which is what instantiate_batch_local does anyway -- one module backs
	 *    them all.
	 * 2. Expansion and the emptiness test do not mutate shared state and do not belong under the lock.
	 *
	 * Reading re->batch unsynchronised here is sound for what it decides: a descriptor that gets batched
	 * between this walk and the lock is simply published as a singleton, which the generation bump below
	 * then corrects on the next re-frame. Getting it wrong costs an extra republish, not correctness. */
	{
		int cap = 0;
		for (i = 0; i < n; ++i) {
			WjRegEntry *re = (descs [i] > 0 && descs [i] <= wj_reg_n) ? wj_reg_at (descs [i] - 1) : NULL;
			cap += (re && re->batch) ? re->batch->n : 1;
		}
		if (cap <= 0)
			return 0;
		copy = (gint32 *) g_malloc ((gsize) cap * sizeof (gint32));
		if (!copy)
			return 0;
		nexp = 0;
		for (i = 0; i < n; ++i) {
			WjRegEntry *re = (descs [i] > 0 && descs [i] <= wj_reg_n) ? wj_reg_at (descs [i] - 1) : NULL;
			int j, m;
			if (!re)
				continue;
			/* ONE REPRESENTATIVE PER GROUP IN THE LOG, not every member.
			 *
			 * The drain re-instantiates each logged descriptor on each worker that had it installed --
			 * and instantiate_batch_local installs ALL 2N slots of a group in ONE call. So admitting one
			 * member re-instantiates the whole module and installs every sibling's pair; the other N-1
			 * admits are redundant by construction, and each one is a closure walk, multiplied by every
			 * worker. MEASURED: expanding every member wedged boot at ~3 rendezvous while a single
			 * rendezvous of 3 members was fine, and raising the batch size (fewer, larger rendezvous)
			 * did NOT help -- which is what says the cost is per-MEMBER, not per-rendezvous.
			 *
			 * The GENERATION bump below still covers every member; it is only the log -- the thing the
			 * drain walks -- that collapses to one entry per group. A sibling whose own cached generation
			 * is now stale simply re-admits lazily on its next dispatch, by which time the batch
			 * instantiate has already installed its slots, so that admit takes the cheap path. */
			if (re->batch) {
				m = re->batch->desc [0];            /* representative; siblings ride its instantiate */
				if (m <= 0 || m > wj_reg_n)
					continue;
			} else {
				m = descs [i];
			}
			for (j = 0; j < nexp; ++j)              /* two inputs can share a group */
				if (copy [j] == m) break;
			if (j == nexp && nexp < cap)
				copy [nexp++] = m;
		}
		if (nexp <= 0) {
			g_free (copy);
			return 0;
		}
		n = nexp;
	}

	/* Serialize generation assignment and log publication. Readers remain lock-free: the epoch is stored
	 * only after the complete log record and descriptor generations are visible. Reading it before this
	 * short lock would let two concurrent publishers
	 * see the same value, write the SAME log slot -- the second overwriting the first -- and both set the
	 * epoch to it, which silently LOSES a republication. Losing one means a worker keeps calling a body it
	 * was told to stop calling, i.e. precisely the class this mechanism exists to prevent. */
	mono_loader_lock ();
	epoch = wj_global_epoch;
	if (epoch >= WJ_RV_LOG_MAX) {
		/* REFUSE rather than wrap, for the same reason. Bounded retention is the cheap side of that trade:
		 * the log is a few thousand small arrays, never freed, because every worker reads them lock-free
		 * and nothing recalls them. */
		wj_rv_log_full = 1;
		mono_wasm_jit_counters [WJC_RV_LOG_FULL]++;
		mono_loader_unlock ();
		g_free (copy);
		return 0;
	}

	/* Bump each descriptor's generation. This is the existing signal every worker's admission path already
	 * tests (mono_wasm_jit_admit's generation arm); the drain only makes the re-admission PROMPT and
	 * mandatory rather than lazy.
	 *
	 * Co-located members were expanded to one representative per complete group above. Their generation
	 * lives on the batch, so publication bumps both the shared batch generation and every member entry. */
	for (i = 0; i < n; ++i) {
		WjRegEntry *re = (copy [i] > 0 && copy [i] <= wj_reg_n) ? wj_reg_at (copy [i] - 1) : NULL;
		if (!re) {
			copy [i] = 0;
			continue;
		}
		/* A group's generation lives on the BATCH, and admission's sibling premark tests it there, so
		 * bump both: the batch (what siblings are compared against) and the entry (what this descriptor's
		 * own fast path compares against). Idempotent across members of the same group. */
		/* Bump the BATCH's generation (what admission's sibling premark compares against) and EVERY
		 * member's entry generation (what each descriptor's own fast path compares against). The log
		 * carries one representative, but the generations have to cover the whole group or a sibling
		 * would keep serving its cached verdict. */
		if (re->batch) {
			int k;
			re->batch->generation = (guint32) (epoch + 1);
			for (k = 0; k < re->batch->n; ++k) {
				int m = re->batch->desc [k];
				WjRegEntry *mre = (m > 0 && m <= wj_reg_n) ? wj_reg_at (m - 1) : NULL;
				if (mre)
					mre->generation = (guint32) (epoch + 1);
			}
		} else {
			re->generation = (guint32) (epoch + 1);
		}
		++published;
	}
	if (!published) {
		/* Nothing to republish after the batched members were dropped. Leave the epoch
		 * alone -- bumping it would send every worker through a drain that has no work in it. */
		mono_loader_unlock ();
		g_free (copy);
		return 0;
	}

	wj_rv_log [epoch].n = n;
	wj_rv_log [epoch].epoch = epoch + 1;
	mono_memory_barrier ();              /* contents before the pointer that makes them reachable */
	wj_rv_log [epoch].desc = copy;
	mono_memory_barrier ();              /* ... and the pointer before the epoch that indexes it */
	wj_global_epoch = epoch + 1;
	mono_loader_unlock ();

	/* The initiator owns a table too, and it is not going through a resume hook. */
	mono_wasm_jit_rendezvous_drain ();

	mono_wasm_jit_counters [WJC_RV_COUNT]++;
	mono_wasm_jit_add (WJC_RV_MEMBERS, n);
	return 1;
}
