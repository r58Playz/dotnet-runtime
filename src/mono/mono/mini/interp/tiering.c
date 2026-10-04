#include "tiering.h"
/* For WJC_RELINK_BAIL_CLEARED: the body swap drops generation 1's permanent bail and that has to be
 * countable from the harness like every other tier decision. */
#include "../mini-wasm.h"
#include <mono/utils/mono-threads-api.h>
#include <mono/metadata/body-override.h>

/* body_invalidate's flags. 0 = behaviour-preserving (an IKVM generation 2); see body_invalidate. */
#define MONO_BODY_HOOK 1
static int body_invalidate (MonoMethod *target, MonoMethodHeader *new_header, gpointer detour_ftn, MonoMethod *detour_target, int flags);

static mono_mutex_t tiering_mutex;
// FIXME: The add/remove traffic on this table may require dn_simdhash to implement cascade flag cleanup
//  and compaction
static dn_simdhash_ptr_ptr_t *patch_sites_table;
static gboolean enable_tiering;
extern gint32 mono_interp_patch_late_forward;
gint32 mono_interp_tierup_epoch_refused;   /* H1: tier-ups refused because a body swap replaced the IL (healthy, non-zero after swaps) */
gint32 mono_interp_swap_origin_forwarded;  /* R402: swaps that also forwarded the retired tier-up copy's origin (healthy) */
/* Behaviour-altering swaps (MONO_BODY_HOOK) may happen in this process: set by the wasm JIT when liba is linked
 * (mono_wasm_jit_hookable), and by the first hook regardless. Makes the transform register a patch site for an
 * OPTIMIZED target as well (transform.c get_data_item_index_imethod). */
gboolean mono_interp_body_hooks_possible;

void
mono_interp_tiering_init (void)
{
	mono_os_mutex_init_recursive (&tiering_mutex);
	patch_sites_table = dn_simdhash_ptr_ptr_new (0, NULL);
	enable_tiering = TRUE;
}

gboolean
mono_interp_tiering_enabled (void)
{
	return enable_tiering;
}

static InterpMethod*
get_tier_up_imethod (InterpMethod *imethod)
{
	MonoMethod *method = imethod->method;
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (method);
	InterpMethod *new_imethod = (InterpMethod*)m_method_alloc0 (method, sizeof (InterpMethod));

	new_imethod->method = imethod->method;
	new_imethod->param_count = imethod->param_count;
	new_imethod->hasthis = imethod->hasthis;
	new_imethod->vararg = imethod->vararg;
	new_imethod->code_type = imethod->code_type;
	new_imethod->rtype = imethod->rtype;
	new_imethod->param_types = imethod->param_types;
	new_imethod->is_invoke = imethod->is_invoke;
	new_imethod->optimized = TRUE;
	new_imethod->prof_flags = imethod->prof_flags;
	/* Tier-up compiles the SAME IL: the frame migrates between the two by basic-block index. */
	new_imethod->bound_header = imethod->bound_header;
	new_imethod->body_epoch = imethod->body_epoch;

	jit_mm_lock (jit_mm);
	InterpMethod *old_imethod = mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
	/* A BODY SWAP SINCE `imethod` WAS COMPILED. The registered compilation is of different IL (another epoch), and
	 * migrating this frame onto it -- or onto a fresh optimized copy, which the transform would build from the
	 * CURRENT header -- would map its basic-block index into the wrong code (an assert in lookup_patchpoint_data,
	 * or a frame relocated onto a different stack layout). The opcodes test `retired` first; this closes the window
	 * between that test and here. The frame stays interpreted, which is merely slower. */
	if (!old_imethod || imethod->retired || old_imethod->body_epoch != imethod->body_epoch) {
		jit_mm_unlock (jit_mm);
		mono_atomic_inc_i32 (&mono_interp_tierup_epoch_refused);
		return NULL;   /* the newly allocated InterpMethod leaks to the mempool, as below */
	}
	if (old_imethod->optimized) {
		new_imethod = old_imethod; /* leak the newly allocated InterpMethod to the mempool */
	} else {
		/* Runtime wasm-JIT identity belongs to the logical method, not to one interpreter tier. Preserve
		 * the atomically-published descriptor and all policy/diagnostic state when replacing its imethod. */
		new_imethod->wasm_jit_slot = old_imethod->wasm_jit_slot;
		new_imethod->wasm_jit_desc = old_imethod->wasm_jit_desc;
		new_imethod->wasm_jit_fslot = old_imethod->wasm_jit_fslot;
		new_imethod->wasm_jit_hits = old_imethod->wasm_jit_hits;
		new_imethod->wasm_jit_bytes_len = old_imethod->wasm_jit_bytes_len;
		new_imethod->wasm_jit_bytes = old_imethod->wasm_jit_bytes;
		new_imethod->wasm_jit_bail = old_imethod->wasm_jit_bail;
		new_imethod->wasm_jit_invoke_in = old_imethod->wasm_jit_invoke_in;
		new_imethod->wasm_jit_invoke_out = old_imethod->wasm_jit_invoke_out;
		/* Carry the reserved slot pair too. The function-table allocator has no free, so dropping these on
		 * tier-up would leak two entries for every method that reserved and then tiered — silently, and
		 * exactly for the hot methods that tier. */
		new_imethod->wasm_jit_self_resv_eslot = old_imethod->wasm_jit_self_resv_eslot;
		new_imethod->wasm_jit_self_resv_fslot = old_imethod->wasm_jit_self_resv_fslot;
		/* Carry the speculative-devirt profile across tier-up: samples gathered in tier0 describe the
		 * method's call sites, not one tier's bytecode, and the whole point is to still have them when
		 * the wasm JIT compiles later. Losing them here would reset the evidence exactly when a method
		 * gets hot enough to matter. */
#if HOST_BROWSER
		/* R316j (MONO_WASM_JIT_PROF_SHARE): allocate the old generation's block now if it has none, so the generations
		 * SHARE one profile instead of each growing its own after a NULL was copied. */
		{
			extern int mono_wasm_jit_prof_share;
			extern gpointer mono_wasm_jit_prof_ensure (InterpMethod *im);
			new_imethod->wasm_jit_profile = mono_wasm_jit_prof_share ? mono_wasm_jit_prof_ensure (old_imethod)
			                                                         : old_imethod->wasm_jit_profile;
		}
#else
		new_imethod->wasm_jit_profile = old_imethod->wasm_jit_profile;
#endif
		/* R316: tier-2 state is the METHOD's, not one interpreter tier's (same IL). */
		new_imethod->wasm_jit_t2_samples = old_imethod->wasm_jit_t2_samples;
		new_imethod->wasm_jit_tier = old_imethod->wasm_jit_tier;
		new_imethod->wasm_jit_t2_want = old_imethod->wasm_jit_t2_want;
		new_imethod->tier_origin = imethod;   /* see InterpMethod.tier_origin */
		mono_internal_hash_table_remove (&jit_mm->interp_code_hash, method);
		mono_internal_hash_table_insert (&jit_mm->interp_code_hash, method, new_imethod);
	}
	jit_mm_unlock (jit_mm);

	return new_imethod;
}

static void
patch_imethod_site (gpointer data, gpointer user_data)
{
	gpointer *addr = (gpointer*)data;
	// Preserve the tagging, in case the address originates in vtables
	gboolean tagged = INTERP_IMETHOD_IS_TAGGED_1 (*addr);
	*addr = (InterpMethod*)(tagged ? INTERP_IMETHOD_TAG_1 (user_data) : user_data);
}

/* Repoint every site registered under old_imethod at new_imethod, and RE-KEY the list under new_imethod rather than
 * freeing it. Upstream freed it, because an optimized InterpMethod was final; a body swap replaces optimized ones
 * too, and a site that is no longer tracked is a caller that keeps calling the displaced IL forever (the second
 * swap of a method -- a MonoMod Undo after Apply, or a tier-up between two IKVM relinks). Under tiering_mutex. */
static void
patch_interp_data_items (InterpMethod *old_imethod, InterpMethod *new_imethod)
{
	GSList *sites = NULL;
	if (dn_simdhash_ptr_ptr_try_get_value (patch_sites_table, old_imethod, (void **)&sites)) {
		GSList *existing = NULL;
		g_slist_foreach (sites, patch_imethod_site, new_imethod);
		dn_simdhash_ptr_ptr_try_remove (patch_sites_table, old_imethod);
		if (dn_simdhash_ptr_ptr_try_get_value (patch_sites_table, new_imethod, (void **)&existing))
			dn_simdhash_ptr_ptr_try_replace_value (patch_sites_table, new_imethod, g_slist_concat (sites, existing));
		else
			dn_simdhash_ptr_ptr_try_add (patch_sites_table, new_imethod, sites);
	}
}

static InterpMethod*
tier_up_method (InterpMethod *imethod, ThreadContext *context)
{
	g_assert (enable_tiering);
	ERROR_DECL(error);
	// This enables future code to obtain a reference to the optimized imethod
	InterpMethod *new_imethod = get_tier_up_imethod (imethod);
	if (!new_imethod)
		return NULL;   /* refused: a body swap since this compilation (see get_tier_up_imethod) */

	// In theory we can race with other threads compiling the same imethod, but this is not a problem
	if (!new_imethod->transformed)
		mono_interp_transform_method (new_imethod, context, error);
	// Unoptimized method compiled fine, optimized method should also compile without error
	mono_error_assert_ok (error);

	mono_os_mutex_lock (&tiering_mutex);

	if (!imethod->optimized_imethod) {
		/* patch all data items -- to the CURRENT generation: a body swap may have retired new_imethod since
		 * get_tier_up_imethod registered it (the swap's own patch then found no sites under it), and callers must
		 * reach the new IL, not this compilation of the old one. The frames below still migrate to new_imethod. */
		patch_interp_data_items (imethod, interp_imethod_current (new_imethod, TRUE));

		// Other threads executing this imethod will be able to tier the frame up in patchpoints
		imethod->optimized_imethod = new_imethod;
	}
	mono_os_mutex_unlock (&tiering_mutex);

	return new_imethod;
}

/* ---- generational method bodies: replace a method's IL and re-transform it -------------------------
 *
 * IKVM compiles a Java method to IL before the classes it references are necessarily loaded. Under lazy
 * class loading an unresolved reference becomes a __dynamic_* opcode, which lowers to a per-site stub and
 * a cached delegate -- permanently, because the decision is baked into the emitted IL. Recompiling the
 * body once the types ARE loaded removes those stubs, and the residual pool it leaves behind was measured
 * at 11.673% of Minecraft's client render thread (R225/R226).
 *
 * IKVM's own mechanism reaches the recompiled body through a delegate, and R226 measured that indirection
 * costing 3x what removing the stubs saved (+14.2% per frame). Hence this: put the new body IN the method,
 * so there is nothing to pay on the way to it.
 *
 * Why this is now possible, against R19 which shelved it. R19's objection was that mono's publish path
 * memcpy's a whole InterpMethod from a snapshot while live frames read data_items through frame->imethod.
 * That is not what happens: get_tier_up_imethod above ALLOCATES A FRESH InterpMethod and swaps it into
 * interp_code_hash, deliberately leaking the old one so live frames finish on it, and patch_interp_data_items
 * repoints everything that had baked the old pointer. Both are reused here. The IL swap itself is one
 * pointer, because an SRE method stores its header on the wrapper (class-internals.h, MonoMethodWrapper)
 * and mono_method_get_header_internal returns it directly and uncached; the displaced header is not freed,
 * because mono_metadata_free_mh is a no-op unless is_transient and an SRE header's lifetime belongs to its
 * method.
 *
 * Counters rather than silence: a replacement that declines looks exactly like one that never ran, and
 * this file has paid for that confusion before. */
gint32 mono_interp_relink_replaced;
gint32 mono_interp_relink_late;      /* already tiered or already wasm-JITted: refused, not failed */
gint32 mono_interp_relink_untouched; /* never transformed, so the next call picks up the new header */
gint32 mono_interp_relink_rejected;  /* shape/signature mismatch -- an IKVM bug if it ever fires */
gint32 mono_interp_relink_bail_cleared; /* gen-1 had permanently bailed; gen-2 gets a fresh verdict */
gint32 mono_interp_relink_refreshed;    /* gen-1 was live; gen-2 starts untried with a fresh slot pair */
gint32 mono_interp_patch_late_forward;  /* H1: a data item registered after its target was tiered up or swapped, re-pointed to the current one */

static void
patch_imethod_refs (InterpMethod *old_imethod, InterpMethod *new_imethod)
{
	if (!enable_tiering)
		return;
	mono_os_mutex_lock (&tiering_mutex);
	patch_interp_data_items (old_imethod, new_imethod);
	mono_os_mutex_unlock (&tiering_mutex);
}

/* A "late" refusal leaves the method on generation 1 -- its lazy-link stubs -- for the rest of the run, so which
 * methods land here is worth more than the count: the first few are named. Counted as before. */
static void
relink_note_late (MonoMethod *target, const char *why)
{
	int n = mono_atomic_inc_i32 (&mono_interp_relink_late);
	if (n <= 32) {
		char *name = mono_method_get_full_name (target);
		g_print ("[wasm-jit relink] late #%d (%s, past the relink_hook gate): %s\n", n, why, name);
		g_free (name);
	}
}

/* True when the method keeps its IL header on the wrapper, which is the only shape whose body can be
 * swapped: mono_method_get_header_internal returns mw->header directly for these and reads it fresh on
 * every transform. Everything else resolves its header from image metadata, which is immutable. */
static gboolean
has_settable_header (MonoMethod *m)
{
	return m && (m->wrapper_type != MONO_WRAPPER_NONE || m->sre_method);
}

/*
 * Replaces \p target's IL body with \p source's and forces the interpreter to re-transform it.
 * Returns 1 when the body was replaced (or will be, on first transform), 0 when declined, -1 on a
 * shape that must never be swapped.
 *
 * \p source is expected to be a method IKVM compiled for exactly \p target's erased signature, on a
 * scratch type in the same dynamic module so its metadata tokens resolve in the same scope. An instance
 * method's source is static with the declaring type prepended, which is positionally identical -- ldarg.0
 * is `this` either way -- so the arg-slot count is what must agree, not the signature.
 */
static int
replace_method_body_locked (MonoMethod *target, MonoMethod *source)
{
	if (!has_settable_header (target) || !has_settable_header (source)) {
		mono_atomic_inc_i32 (&mono_interp_relink_rejected);
		return -1;
	}

	MonoMethodHeader *new_header = ((MonoMethodWrapper *)source)->header;
	if (!new_header) {
		mono_atomic_inc_i32 (&mono_interp_relink_rejected);
		return -1;
	}

	MonoMethodSignature *ts = mono_method_signature_internal (target);
	MonoMethodSignature *ss = mono_method_signature_internal (source);
	if (!ts || !ss ||
	    (ts->param_count + (ts->hasthis ? 1 : 0)) != (ss->param_count + (ss->hasthis ? 1 : 0))) {
		mono_atomic_inc_i32 (&mono_interp_relink_rejected);
		return -1;
	}

	return body_invalidate (target, new_header, NULL, NULL, 0);
}

/*
 * THE ONE BODY-REPLACEMENT PRIMITIVE (H2): make `new_header` (NULL = the method's own) the body of `target`, and
 * retire every compilation of the previous one so NEW calls reach the new body -- an IKVM generation-2 swap
 * (behaviour-preserving) and a MonoMod-style detour or Undo (MONO_BODY_HOOK, behaviour-altering) alike. Returns 1
 * replaced, 0 declined (PRESERVING only: see mono_wasm_jit_relink_jitted).
 *
 * HOOK differs in two ways: it is never declined, and it sets NoInlining first, under the loader lock, so no compile
 * that starts after this inlines the method (MonoMod's own contract: compilations that already inlined it keep the old
 * body, on every runtime). With the wasm JIT, HOOK still republishes the JIT body the PRESERVING way (compile, then
 * republish) until the routing of plan H5 lands, so a JIT'd method sees the new body only from its republication on.
 */
static int
body_invalidate (MonoMethod *target, MonoMethodHeader *new_header, gpointer detour_ftn, MonoMethod *detour_target, int flags)
{
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (target);
	InterpMethod *old_imethod, *new_imethod, *origin = NULL;
	MonoMethodHeader *prev_header, *installed = NULL;
	/* Set when the method already owns a live descriptor and needs a mandatory same-slot update. */
	gboolean replace_live_generation = FALSE;
	extern int mono_wasm_jit_relink_jitted;

	if (flags & MONO_BODY_HOOK) {
		mono_interp_body_hooks_possible = TRUE;
		mono_loader_lock ();
		target->iflags |= METHOD_IMPL_ATTRIBUTE_NOINLINING;
		mono_loader_unlock ();
	}
	/* The body in force now, as a persistent header the displaced compilation can bind to. Outside the jit_mm lock:
	 * a metadata method's first replacement parses its own header, which can load types. */
	prev_header = mono_body_current_header (target);

	jit_mm_lock (jit_mm);
	old_imethod = (InterpMethod *)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, target);
	if (!old_imethod) {
		/* Not transformed yet. Swap and leave: the first transform reads the header we just installed. */
		mono_body_override_install (target, new_header, detour_ftn, detour_target, NULL);
		jit_mm_unlock (jit_mm);
		mono_atomic_inc_i32 (&mono_interp_relink_untouched);
		return 1;
	}

	/* AN INTERPRETER-TIERED METHOD IS SWAPPED LIKE ANY OTHER (H1). This used to be refused outright -- tier-up
	 * migrates a frame between two compilations by basic-block index, and after a swap the registered one is of
	 * DIFFERENT IL -- and a refusal kept the old body for good (IKVM's "late"; a hook silently not applied). What
	 * makes it safe now: every compilation is bound to its own IL (bound_header, set below on the displaced entry
	 * BEFORE the header pointer moves, so a tier-up transform already in flight still compiles the old IL), tier-up
	 * refuses across epochs (get_tier_up_imethod), and frames already running the displaced code finish on it. */

	/* A live wasm method can be replaced because its logical identity, descriptor and table pair stay
	 * stable: the new body is compiled as another generation and each worker installs it into its OWN
	 * table -- one worker cannot write another's -- at its next JIT safepoint.
	 *
	 * WHAT IS AND IS NOT GUARANTEED, stated here because it is the whole contract. Frames already running
	 * the old body finish on it, which is true of method replacement on every runtime. No NEW entry uses
	 * the old body after the owning worker's next safepoint. That window is bounded by the safepoint
	 * check the emitter puts on every loop back-edge (mini-wasm-publish.inc), and it is the strongest
	 * guarantee obtainable without stopping the world -- which was tried, and whose cost was pauses that
	 * wedged boot three times.
	 *
	 * For an IKVM generation-2 swap the old body is merely SLOWER (same Java method, compiled against the
	 * dynamic-dispatch helpers), so a late adoption is a missed optimisation. For a detour it is a patch
	 * not yet applied. Both are bounded; neither is silent -- see WJC_REEMIT_REQUIRED_GIVEUP. */
	if ((old_imethod->wasm_jit_fslot > 0 || old_imethod->wasm_jit_slot > 0) &&
	    !mono_wasm_jit_relink_jitted && !(flags & MONO_BODY_HOOK)) {
		jit_mm_unlock (jit_mm);
		relink_note_late (target, "wasm-JITted");
		return 0;
	}
	replace_live_generation = (old_imethod->wasm_jit_fslot > 0 || old_imethod->wasm_jit_slot > 0);

	/* Bind the displaced compilation to the IL it was compiled from, BEFORE the header pointer moves, so anything
	 * that still has to transform it -- a tier-up already in flight, an exception in one of its frames -- compiles
	 * its own body (mono_interp_transform_method re-reads bound_header after the header). Its unoptimized origin,
	 * when this entry is a tier-up copy, is already transformed and needs nothing. */
	if (!old_imethod->bound_header)
		old_imethod->bound_header = prev_header;
	mono_memory_barrier ();
	mono_body_override_install (target, new_header, detour_ftn, detour_target, &installed);

	new_imethod = (InterpMethod *)m_method_alloc0 (target, sizeof (InterpMethod));
	new_imethod->method = old_imethod->method;
	new_imethod->param_count = old_imethod->param_count;
	new_imethod->hasthis = old_imethod->hasthis;
	new_imethod->vararg = old_imethod->vararg;
	new_imethod->code_type = old_imethod->code_type;
	new_imethod->rtype = old_imethod->rtype;
	new_imethod->param_types = old_imethod->param_types;
	new_imethod->is_invoke = old_imethod->is_invoke;
	new_imethod->prof_flags = old_imethod->prof_flags;
	/* A new IL generation starts unoptimized: the displaced entry may be a tier-up copy of the OLD IL. */
	new_imethod->optimized = 0;
	new_imethod->bound_header = installed;
	new_imethod->body_epoch = old_imethod->body_epoch + 1;
	/* Function-pointer identity survives the swap: an `ldftn` taken before it and one taken after must compare equal,
	 * and a MonoFtnDesc a caller already holds must now enter the new generation. jit_entry and the unbox entry are
	 * the same pointers' interpreter-entry stubs, keyed by the method. */
	new_imethod->ftndesc = old_imethod->ftndesc;
	new_imethod->ftndesc_unbox = old_imethod->ftndesc_unbox;
	new_imethod->jit_entry = old_imethod->jit_entry;
	new_imethod->llvmonly_unbox_entry = old_imethod->llvmonly_unbox_entry;
	/* transformed stays 0 -- that is the whole point: the next call compiles the NEW header. */
	/* The replacement has happened, so the JIT is free to compile this method again -- from generation 2. */
	new_imethod->relink_pending = 0;

	/* Carry the wasm-JIT identity exactly as get_tier_up_imethod does. The code-bearing members are
	 * provably 0 here (refused above), but the hotness counter, the call profile and the reservation
	 * pair must survive: the function-table allocator has no free, so dropping it leaks two entries per
	 * method, and a lazy pool pair is the f-slot callers already baked. */
	/* A PERMANENT BAIL DOES NOT SURVIVE A BODY SWAP. wasm_jit_slot == -1 is a verdict the emitter
	 * reached about generation 1's IL, and generation 1's IL is precisely what this function is
	 * replacing -- so carrying it forward condemns a body the emitter has never seen, permanently
	 * (interp.c's `if (im->wasm_jit_slot == -1) return -1` is checked on every route into the tier).
	 * The correlation runs the wrong way to leave alone: a gen-1 body bails on constructs that
	 * generation 2 exists to REMOVE, so the methods most likely to be carrying -1 are the ones with
	 * the most to gain from being re-judged.
	 *
	 * Only the permanent verdict is dropped, and only here -- get_tier_up_imethod's copy above is a
	 * compilation of the SAME IL, where the verdict still binds. RETRY is kept too: it describes a
	 * transient condition, not this body's compilability. The hotness counter, the call profile
	 * and the reservation pair below are properties of the METHOD rather than of a compilation and
	 * must survive regardless (see above).
	 *
	 * Safe to reset the pair together: the refusal above guarantees wasm_jit_fslot <= 0 here, and
	 * wasm_jit_desc is only ever written alongside a successful registration (interp.c:2502, :2896,
	 * transform.c:10368), which a -1 method by definition never reached.
	 *
	 * MEASURED ZERO -- KEPT DELIBERATELY, DO NOT RE-DERIVE. WJC_RELINK_BAIL_CLEARED is 0 over a full
	 * boot+worldgen+ingame run (2026-09-12), i.e. this arm never executes. The reasoning that predicted
	 * otherwise -- "gen-1 bails on constructs gen-2 removes, so the -1 population is exactly the one
	 * with the most to gain" -- is sound about which methods WOULD benefit and wrong about when the swap
	 * happens: IKVM's relink hook IS the method's first execution, where the method is either untried
	 * (slot 0) or already live (slot > 0, the replace_live_generation arm above). Reaching -1 first needs a
	 * force-compiled island callee that bailed AND is then swapped, and that intersection is empty here.
	 * The arm stays because the transition it forbids is wrong if it ever does occur, and it costs one
	 * compare; the counter stays because a 0 here is the only thing that distinguishes "cannot happen"
	 * from "silently stopped happening". */
	if (replace_live_generation) {
		/* A new code generation of the same MonoMethod, not a second method. Preserving the pair and the
		 * descriptor is what lets callers that already baked this f-slot reach generation 2 at all, and it
		 * consumes no table entries -- R252's fresh-pair shape leaked two per swap against ~68k spare. */
		/* A HOOK's new generation does not enter the old body's e-slot: its entry thunk calls the OLD f module-locally
		 * (plan H5). The interpreter runs it until the new body publishes its own e-slot. */
		new_imethod->wasm_jit_slot = (flags & MONO_BODY_HOOK) ? 0 : old_imethod->wasm_jit_slot;
		new_imethod->wasm_jit_bail = 0;
		new_imethod->wasm_jit_desc = old_imethod->wasm_jit_desc;
		new_imethod->wasm_jit_fslot = old_imethod->wasm_jit_fslot;
		new_imethod->wasm_jit_bytes = old_imethod->wasm_jit_bytes;
		new_imethod->wasm_jit_bytes_len = old_imethod->wasm_jit_bytes_len;
		new_imethod->wasm_jit_reemit_required = 1;
		mono_atomic_inc_i32 (&mono_interp_relink_refreshed);
		mono_wasm_jit_count (WJC_RELINK_REFRESHED);
	} else if (old_imethod->wasm_jit_slot == -1) {
		/* The ONE path that can falsify an entry in the backend's append-only perm-unjittable pointer
		 * set. Probe it so "the set never goes stale" is measured rather than inherited from R252.
		 *
		 * HOST_BROWSER-GUARDED, and structurally so: the probe lives in mini-wasm.c inside its
		 * `#ifdef HOST_BROWSER` region, while BOTH files are linked into the offline cross-compiler as
		 * well as the runtime -- so an unguarded call leaves mono-aot-cross with an undefined symbol.
		 * csyn.sh CANNOT catch this (it compiles, it does not link) and it is the same failure interp.c
		 * already documents at its own probe site. It cost a full build here. */
#if HOST_BROWSER
		{ extern void mono_wasm_jit_note_perm_cleared (MonoMethod *m); mono_wasm_jit_note_perm_cleared (old_imethod->method); }
#endif
		new_imethod->wasm_jit_slot = 0;
		new_imethod->wasm_jit_bail = 0;
		new_imethod->wasm_jit_desc = old_imethod->wasm_jit_desc;
		new_imethod->wasm_jit_fslot = old_imethod->wasm_jit_fslot;
		mono_atomic_inc_i32 (&mono_interp_relink_bail_cleared);
		mono_wasm_jit_count (WJC_RELINK_BAIL_CLEARED);
	} else {
		new_imethod->wasm_jit_slot = old_imethod->wasm_jit_slot;
		new_imethod->wasm_jit_bail = old_imethod->wasm_jit_bail;
		new_imethod->wasm_jit_desc = old_imethod->wasm_jit_desc;
		new_imethod->wasm_jit_fslot = old_imethod->wasm_jit_fslot;
	}
	new_imethod->wasm_jit_hits = old_imethod->wasm_jit_hits;
	new_imethod->wasm_jit_invoke_in = old_imethod->wasm_jit_invoke_in;
	new_imethod->wasm_jit_invoke_out = old_imethod->wasm_jit_invoke_out;
	new_imethod->wasm_jit_self_resv_eslot = old_imethod->wasm_jit_self_resv_eslot;
	new_imethod->wasm_jit_self_resv_fslot = old_imethod->wasm_jit_self_resv_fslot;
#if HOST_BROWSER
	/* R316j (MONO_WASM_JIT_PROF_SHARE): allocate the old generation's block now if it has none, so the generations
	 * SHARE one profile instead of each growing its own after a NULL was copied. */
	{
		extern int mono_wasm_jit_prof_share;
		extern gpointer mono_wasm_jit_prof_ensure (InterpMethod *im);
		new_imethod->wasm_jit_profile = mono_wasm_jit_prof_share ? mono_wasm_jit_prof_ensure (old_imethod)
		                                                         : old_imethod->wasm_jit_profile;
	}
#else
	new_imethod->wasm_jit_profile = old_imethod->wasm_jit_profile;
#endif
	/* R316: a NEW body (different IL) starts at tier 1 again; its hotness history carries over. */
	new_imethod->wasm_jit_t2_samples = old_imethod->wasm_jit_t2_samples;

	/* Link the displaced body to its replacement for caches the swap cannot re-point (interp_imethod_current;
	 * patch_imethod_refs below only reaches REGISTERED data items). Barrier first: a reader that sees the link
	 * must see a fully initialised new_imethod. */
	mono_memory_barrier ();
	old_imethod->replaced_by = new_imethod;
	/* Retire the displaced body BEFORE publishing the new one. Frames already inside it keep running it
	 * to completion, which is what makes the swap safe -- but they must not tier up out of it. */
	old_imethod->retired = 1;
	/* Its interp->JIT entry fast path skips the scaffolding that would notice the swap. */
	old_imethod->wasm_jit_entry_fast_ok = 0;
	/* THE RETIRED ENTRY MAY BE A TIER-UP COPY WHOSE ORIGIN IS NOT LINKED TO IT YET (tier_up_method still transforming).
	 * The origin's callers -- data items, function pointers, delegates -- then reach neither copy nor replacement:
	 * hookstress measured new calls running the displaced body after Apply/Undo had returned (R402). Forward and
	 * retire it like the copy; its sites are patched below. Whichever of this and tier_up_method's own link runs
	 * first, interp_imethod_current reaches new_imethod (both are under tiering_mutex for the patching). */
	origin = old_imethod->tier_origin;
	if (origin && !origin->replaced_by && origin != new_imethod) {
		origin->replaced_by = new_imethod;
		origin->retired = 1;
		origin->wasm_jit_entry_fast_ok = 0;
		mono_atomic_inc_i32 (&mono_interp_swap_origin_forwarded);
	} else {
		origin = NULL;
	}

	mono_internal_hash_table_remove (&jit_mm->interp_code_hash, target);
	mono_internal_hash_table_insert (&jit_mm->interp_code_hash, target, new_imethod);
	/* Descriptors callers already hold now name the new generation (the barrier above published it whole). Entry
	 * points that read an InterpMethod out of one still forward (interp_imethod_live) -- this saves them the hop. */
	if (new_imethod->ftndesc)
		new_imethod->ftndesc->interp_method = new_imethod;
	if (new_imethod->ftndesc_unbox)
		new_imethod->ftndesc_unbox->interp_method = INTERP_IMETHOD_TAG_UNBOX (new_imethod);
	jit_mm_unlock (jit_mm);

	/* Outside the jit-mm lock, and load-bearing: a caller that was already transformed baked the OLD
	 * InterpMethod* into its data_items, so without this it keeps calling generation 1 forever. */
	patch_imethod_refs (old_imethod, new_imethod);
	if (origin)
		patch_imethod_refs (origin, new_imethod);

	if (replace_live_generation && new_imethod->wasm_jit_desc > 0) {
#ifdef HOST_BROWSER
		extern void mono_wasm_jit_bind_logical (int desc_id, MonoMethod *logical_method);
		mono_wasm_jit_bind_logical (new_imethod->wasm_jit_desc, target);
		if (!(flags & MONO_BODY_HOOK))
			mono_wasm_jit_request_reemit (new_imethod->wasm_jit_desc);
#endif
	}
#ifdef HOST_BROWSER
	/* A hook routes the live generation to its stub on every worker and waits for them (plan H5/H6); it queues the
	 * new body itself, after the route. */
	if (flags & MONO_BODY_HOOK) {
		extern void mono_wasm_jit_hook_publish (int desc_id, MonoMethod *m, InterpMethod *im);
		mono_wasm_jit_hook_publish (replace_live_generation ? new_imethod->wasm_jit_desc : 0, target, new_imethod);
	}
#endif

	/* Announce the first one unconditionally. Two reasons: it is the liveness signal for a feature whose
	 * failure mode is silence (a swap that never fires reads exactly like a swap that is not compiled in),
	 * and it is a STRING LITERAL, which is what scratchpad/mcsr/deploy.sh can actually prove against the
	 * served wasm -- identifiers and comments are not in the bytes it greps. */
	if (mono_atomic_inc_i32 (&mono_interp_relink_replaced) == 1)
		g_print ("[interp relink] in-place body replacement active\n");

	return 1;
}

static void
set_relink_pending (MonoMethod *method, int pending)
{
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (method);
	InterpMethod *imethod;

	jit_mm_lock (jit_mm);
	imethod = (InterpMethod *)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
	if (imethod)
		imethod->relink_pending = pending ? 1 : 0;
	jit_mm_unlock (jit_mm);
}

/*
 * Marks/unmarks a method as awaiting an in-place body replacement, so the wasm JIT leaves it alone in
 * the meantime. Same GC-state requirement as the swap itself: reached by P/Invoke, takes a runtime lock.
 *
 * The caller MUST clear this for every method it marked, on every outcome including refusal -- a method
 * left pending is a method the wasm JIT will never compile, which trades one bounded loss for an
 * unbounded one.
 */
void
mono_interp_mark_relink_pending (MonoMethod *method, int pending)
{
	MONO_ENTER_GC_UNSAFE;
	if (method)
		set_relink_pending (method, pending);
	MONO_EXIT_GC_UNSAFE;
}

/*
 * P/Invoke entry point. Fixes the thread's GC state before doing anything, then defers to the worker.
 *
 * This is not boilerplate. Managed code reaches this through a DllImport, and that transition leaves the
 * thread in GC-SAFE (blocking) state -- the state the runtime expects while managed code is "outside".
 * Everything below then wants runtime locks: jit_mm_lock is mono_mem_manager_lock, which enters a GC-safe
 * region itself, and doing that from an already-blocking thread aborts with
 *
 *     Cannot transition thread ... from STATE_BLOCKING with DO_BLOCKING
 *
 * out of mono_threads_transition_do_blocking. Icalls do not have this problem because the icall
 * convention leaves the thread GC-unsafe; a P/Invoke into runtime internals has to restore that itself.
 * Observed on the render thread inside FlushRelinkBatch.
 */
int
mono_interp_replace_method_body (MonoMethod *target, MonoMethod *source)
{
	int res;
	MONO_ENTER_GC_UNSAFE;
	res = replace_method_body_locked (target, source);
	MONO_EXIT_GC_UNSAFE;
	return res;
}

/* Under tiering_mutex. Optimized InterpMethods are keys too: upstream never registered them because nothing replaced
 * one, and a body swap does (patch_interp_data_items). */
/* ---- the WasmDetour convention's entry points (liba, for MonoMod) ------------------------------------------------
 *
 * MonoMod's WasmDetourFactory installs exactly one shape of IL into a hooked method -- its ILHooks are detours to a
 * generated DynamicMethod too -- so that is the only shape accepted: `ldarg* ; ldc.i4 <ftn> ; [tail.] calli 0xF0F0F0F0
 * ; ret`, the magic token meaning "this method's own signature" (transform.c, method-to-ir.c). It arrives with a tiny
 * or fat header in front; the fat one MonoMod builds names local-signature token 32, which is not a token, and a stub
 * has no locals, so the header is rebuilt here rather than parsed. */
gint32 mono_body_hooks_applied, mono_body_hooks_refused;

static gpointer
body_detour_stub_ftn (const guint8 *code, guint32 len)
{
	guint32 i = 0, v;
	while (i < len) {
		guint8 op = code [i];
		if (op >= 0x02 && op <= 0x05) { i += 1; continue; }                                   /* ldarg.0..3 */
		if (op == 0x0E && i + 1 < len) { i += 2; continue; }                                  /* ldarg.s */
		if (op == 0xFE && i + 3 < len && code [i + 1] == 0x09) { i += 4; continue; }          /* ldarg */
		break;
	}
	if (i + 5 > len || code [i] != 0x20)                                                    /* ldc.i4 */
		return NULL;
	memcpy (&v, code + i + 1, 4);
	i += 5;
	if (i + 2 <= len && code [i] == 0xFE && code [i + 1] == 0x14)                          /* tail. */
		i += 2;
	if (i + 5 > len || code [i] != 0x29)                                                    /* calli */
		return NULL;
	{ guint32 tok; memcpy (&tok, code + i + 1, 4); if (tok != 0xF0F0F0F0u) return NULL; }
	i += 5;
	if (i + 1 != len || code [i] != 0x2A)                                                   /* ret */
		return NULL;
	return (gpointer) (gsize) v;
}

/* The method a function pointer names: a MonoFtnDesc in llvm-only mode, else a (tagged) InterpMethod. Read once, at
 * install time, from a pointer MonoMod just took (GetFunctionPointer), so no compiler interprets the IL constant. */
static MonoMethod *
body_ftn_method (gpointer ftn)
{
	extern gboolean mono_llvm_only;
	if (!ftn)
		return NULL;
	if (mono_llvm_only)
		return ((MonoFtnDesc *) ftn)->method;
	return INTERP_IMETHOD_UNTAG_UNBOX ((InterpMethod *) ftn)->method;
}

/* `blob` = header + IL as MonoMod builds it; `len` 0 = derive the length from the header (liba passes only a pointer).
 * Returns 1 installed, -1 refused (not a detour stub, or a malformed header). */
int
mono_body_set_il (MonoMethod *target, const guint8 *blob, guint32 len, int flags)
{
	const guint8 *code;
	guint32 code_size;
	guint16 max_stack = 8;
	gpointer ftn;
	int res;

	if (!target || !blob)
		return -1;
	if ((blob [0] & 3) == 2) {             /* tiny: size in the high six bits */
		code_size = blob [0] >> 2;
		code = blob + 1;
	} else if ((blob [0] & 3) == 3) {      /* fat: flags (2), max stack (2), code size (4), locals token (4, ignored) */
		memcpy (&max_stack, blob + 2, 2);
		memcpy (&code_size, blob + 4, 4);
		code = blob + 12;
	} else {
		mono_atomic_inc_i32 (&mono_body_hooks_refused);
		return -1;
	}
	if ((len && (guint32) (code - blob) + code_size > len) || code_size > 4096 ||
	    !(ftn = body_detour_stub_ftn (code, code_size))) {
		/* liba's P/Invoke returns void to MonoMod, so a refusal would otherwise read as an applied hook. */
		if (mono_atomic_inc_i32 (&mono_body_hooks_refused) <= 16) {
			char *name = mono_method_get_full_name (target);
			g_print ("[body-override] REFUSED a detour on %s: not a WasmDetour stub (code_size=%u)\n", name, code_size);
			g_free (name);
		}
		return -1;
	}

	MonoMethodHeader *h = (MonoMethodHeader *) g_malloc0 (MONO_SIZEOF_METHOD_HEADER);
	guint8 *c = (guint8 *) g_malloc (code_size);
	memcpy (c, code, code_size);
	h->code = c;
	h->code_size = code_size;
	h->max_stack = max_stack;
	h->is_transient = FALSE;

	MONO_ENTER_GC_UNSAFE;
	res = body_invalidate (target, h, ftn, body_ftn_method (ftn), flags);
	MONO_EXIT_GC_UNSAFE;
	if (res == 1)
		mono_atomic_inc_i32 (&mono_body_hooks_applied);
	return res;
}

/* Undo: the method's own body again. */
int
mono_body_clear (MonoMethod *target, int flags)
{
	int res;
	if (!target)
		return -1;
	MONO_ENTER_GC_UNSAFE;
	res = body_invalidate (target, NULL, NULL, NULL, flags);
	MONO_EXIT_GC_UNSAFE;
	return res;
}

static void
register_imethod_patch_site (InterpMethod *imethod, gpointer *ptr)
{
	GSList *sites = NULL;
	guint8 found = dn_simdhash_ptr_ptr_try_get_value (patch_sites_table, imethod, (void **)&sites);
	sites = g_slist_prepend (sites, ptr);
	if (found)
		dn_simdhash_ptr_ptr_try_replace_value (patch_sites_table, imethod, sites);
	else
		dn_simdhash_ptr_ptr_try_add (patch_sites_table, imethod, sites);
}

static void
register_imethod_data_item (gpointer data, gpointer user_data)
{
	gint32 index = (gint32)(gsize)data;
	InterpMethod **data_items = (InterpMethod**)user_data;

	if (data_items [index]) {
		/* We are under tiering lock: resolve to the CURRENT compilation -- tiered up, or swapped (H1) -- and
		 * register the site under THAT key, so a later tier-up or swap still finds it. The transform that baked
		 * this pointer may have started before either happened. */
		InterpMethod *cur = interp_imethod_current (data_items [index], TRUE);
		if (cur != data_items [index]) {
			data_items [index] = cur;
			mono_atomic_inc_i32 (&mono_interp_patch_late_forward);
		}
		register_imethod_patch_site (cur, (gpointer*)&data_items [index]);
	}
}

void
mono_interp_clear_data_items_patch_sites (gpointer *data_items, int n_data_items)
{
	if (!enable_tiering)
		return;
	// data_items is part of the memory of a dynamic method that is being freed.
	// slots within this memory can be registered as patch sites for other imethods
	// We conservatively assume each slot could be an imethod slot, then look it up
	// in imethod to patch_sites hashtable. If we find it in the hashtable, we remove
	// the slot from the patch site list.
	mono_os_mutex_lock (&tiering_mutex);

	for (int i = 0; i < n_data_items; i++) {
		GSList *sites;
		gpointer *slot = data_items + i;
		gpointer imethod_candidate = *slot;

		if (dn_simdhash_ptr_ptr_try_get_value (patch_sites_table, imethod_candidate, (void **)&sites)) {
			GSList *prev = NULL;

			// Remove slot from sites list
			if (sites->data == slot) {
				// If the slot is found in the first element we will also need to update the hash table since
				// the list head changes
				if (!sites->next) {
					g_slist_free_1 (sites);
					dn_simdhash_ptr_ptr_try_remove (patch_sites_table, imethod_candidate);
				} else {
					prev = sites;
					sites = sites->next;
					g_slist_free_1 (prev);
					dn_simdhash_ptr_ptr_try_replace_value (patch_sites_table, imethod_candidate, sites);
				}
			} else {
				prev = sites;
				sites = sites->next;
				while (sites != NULL) {
					if (sites->data == slot) {
						prev->next = sites->next;
						g_slist_free_1 (sites);
						// duplicates not allowed
						break;
					}
					prev = sites;
					sites = sites->next;
				}
			}
		}
	}
	mono_os_mutex_unlock (&tiering_mutex);
}

void
mono_interp_register_imethod_data_items (gpointer *data_items, GSList *indexes)
{
	if (!enable_tiering)
		return;
	mono_os_mutex_lock (&tiering_mutex);
	g_slist_foreach (indexes, register_imethod_data_item, data_items);
	mono_os_mutex_unlock (&tiering_mutex);
}

// This method should be called within mem manager lock which means
// the contents of **imethod_ptr cannot modify until we register the
// patch site
void
mono_interp_register_imethod_patch_site (gpointer *imethod_ptr)
{
	mono_os_mutex_lock (&tiering_mutex);
	/* Under the tiering lock: resolve to the CURRENT compilation (tiered up, or swapped) and track the site under
	 * it, optimized or not -- a body swap re-points optimized compilations too (H1). The tag is preserved, as
	 * patch_imethod_site does. */
	gboolean tagged = INTERP_IMETHOD_IS_TAGGED_1 (*imethod_ptr);
	InterpMethod *imethod = INTERP_IMETHOD_UNTAG_1 (*imethod_ptr);
	InterpMethod *cur = interp_imethod_current (imethod, TRUE);
	if (cur != imethod)
		*imethod_ptr = tagged ? INTERP_IMETHOD_TAG_1 (cur) : cur;
	register_imethod_patch_site (cur, imethod_ptr);
	mono_os_mutex_unlock (&tiering_mutex);
}

const guint16*
mono_interp_tier_up_frame_enter (InterpFrame *frame, ThreadContext *context)
{
	InterpMethod *optimized_method;
	if (frame->imethod->optimized_imethod)
		optimized_method = frame->imethod->optimized_imethod;
	else
		optimized_method = tier_up_method (frame->imethod, context);
	if (!optimized_method)
		return NULL;   /* refused (body swap): the caller stays in the unoptimized code */
	context->stack_pointer = (guchar*)frame->stack + optimized_method->alloca_size;
	frame->imethod = optimized_method;
	return optimized_method->code;
}

static int
lookup_patchpoint_data (InterpMethod *imethod, int data)
{
       int *position = imethod->patchpoint_data;
       while (*position != G_MAXINT32) {
               if (*position == data)
                       return *(position + 1);
               position += 2;
       }
       return G_MAXINT32;
}

const guint16*
mono_interp_tier_up_frame_patchpoint (InterpFrame *frame, ThreadContext *context, int bb_index)
{
	InterpMethod *unoptimized_method = frame->imethod;
	InterpMethod *optimized_method;
	if (unoptimized_method->optimized_imethod)
		optimized_method = unoptimized_method->optimized_imethod;
	else
		optimized_method = tier_up_method (unoptimized_method, context);
	if (!optimized_method)
		return NULL;   /* refused (body swap): the caller stays in the unoptimized code */
	for (int i = 0; i < unoptimized_method->num_clauses; i++) {
		MonoExceptionClause *clause = &unoptimized_method->clauses [i];
		if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY)
			continue;
		// Patch return addresses used by MINT_CALL_HANDLER + MINT_ENDFINALLY
		guint16 **ip_addr = (guint16**)((char*)frame->stack + unoptimized_method->clause_data_offsets [i]);
		guint16 *ret_ip = *ip_addr;
		// ret_ip could be junk on stack, do a quick check first
		if (ret_ip < unoptimized_method->code)
			continue;
		int native_offset = (int)(ret_ip - unoptimized_method->code);
		int call_handler_index = lookup_patchpoint_data (unoptimized_method, native_offset);
		if (call_handler_index != G_MAXINT32) {
			int offset = lookup_patchpoint_data (optimized_method, call_handler_index);
			g_assert (offset != G_MAXINT32);
			*ip_addr = optimized_method->code + offset;
		}
	}
	context->stack_pointer = (guchar*)frame->stack + optimized_method->alloca_size;
	frame->imethod = optimized_method;
	int offset = lookup_patchpoint_data (optimized_method, bb_index);
	g_assert (offset != G_MAXINT32);
	return optimized_method->code + offset;
}
