#include "tiering.h"
#include <mono/utils/mono-threads-api.h>

static mono_mutex_t tiering_mutex;
// FIXME: The add/remove traffic on this table may require dn_simdhash to implement cascade flag cleanup
//  and compaction
static dn_simdhash_ptr_ptr_t *patch_sites_table;
static gboolean enable_tiering;

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

	jit_mm_lock (jit_mm);
	InterpMethod *old_imethod = mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
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
		new_imethod->wasm_jit_block_n = old_imethod->wasm_jit_block_n;
		new_imethod->wasm_jit_invoke_out = old_imethod->wasm_jit_invoke_out;
		new_imethod->wasm_jit_resv_eslot = old_imethod->wasm_jit_resv_eslot;
		new_imethod->wasm_jit_resv_fslot = old_imethod->wasm_jit_resv_fslot;
		/* Carry the parked slot pair too. The function-table allocator has no free, so dropping these on
		 * tier-up would leak two entries for every method that reserved and then tiered — silently, and
		 * exactly for the hot methods that tier. */
		new_imethod->wasm_jit_self_resv_eslot = old_imethod->wasm_jit_self_resv_eslot;
		new_imethod->wasm_jit_self_resv_fslot = old_imethod->wasm_jit_self_resv_fslot;
		/* Carry the speculative-devirt profile across tier-up: samples gathered in tier0 describe the
		 * method's call sites, not one tier's bytecode, and the whole point is to still have them when
		 * the wasm JIT compiles later. Losing them here would reset the evidence exactly when a method
		 * gets hot enough to matter. */
		new_imethod->wasm_jit_profile = old_imethod->wasm_jit_profile;
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

static void
patch_interp_data_items (InterpMethod *old_imethod, InterpMethod *new_imethod)
{
	GSList *sites = NULL;
	if (dn_simdhash_ptr_ptr_try_get_value (patch_sites_table, old_imethod, (void **)&sites)) {
		g_slist_foreach (sites, patch_imethod_site, new_imethod);
		dn_simdhash_ptr_ptr_try_remove (patch_sites_table, old_imethod);
		g_slist_free (sites);
	}
}

static InterpMethod*
tier_up_method (InterpMethod *imethod, ThreadContext *context)
{
	g_assert (enable_tiering);
	ERROR_DECL(error);
	// This enables future code to obtain a reference to the optimized imethod
	InterpMethod *new_imethod = get_tier_up_imethod (imethod);

	// In theory we can race with other threads compiling the same imethod, but this is not a problem
	if (!new_imethod->transformed)
		mono_interp_transform_method (new_imethod, context, error);
	// Unoptimized method compiled fine, optimized method should also compile without error
	mono_error_assert_ok (error);

	mono_os_mutex_lock (&tiering_mutex);

	if (!imethod->optimized_imethod) {
		// patch all data items
		patch_interp_data_items (imethod, new_imethod);

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

static void
patch_imethod_refs (InterpMethod *old_imethod, InterpMethod *new_imethod)
{
	if (!enable_tiering)
		return;
	mono_os_mutex_lock (&tiering_mutex);
	patch_interp_data_items (old_imethod, new_imethod);
	mono_os_mutex_unlock (&tiering_mutex);
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

	MonoJitMemoryManager *jit_mm = jit_mm_for_method (target);
	InterpMethod *old_imethod, *new_imethod;

	jit_mm_lock (jit_mm);
	old_imethod = (InterpMethod *)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, target);
	if (!old_imethod) {
		/* Not transformed yet. Swap and leave: the first transform reads the header we just installed. */
		((MonoMethodWrapper *)target)->header = new_header;
		jit_mm_unlock (jit_mm);
		mono_atomic_inc_i32 (&mono_interp_relink_untouched);
		return 1;
	}

	/* REFUSE anything already promoted, and count it. A tiered imethod is reachable through
	 * optimized_imethod and through patchpoints inside running frames; a wasm-JITted one has emitted
	 * code and an f-slot that other modules may already have baked as a direct call. Neither is
	 * something a body swap may invalidate from here, so the method keeps generation 1 -- which is
	 * exactly today's behaviour for it, hence a refusal and not a regression. */
	if (old_imethod->optimized || old_imethod->optimized_imethod ||
	    old_imethod->wasm_jit_fslot > 0 || old_imethod->wasm_jit_slot > 0) {
		jit_mm_unlock (jit_mm);
		mono_atomic_inc_i32 (&mono_interp_relink_late);
		return 0;
	}

	((MonoMethodWrapper *)target)->header = new_header;

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
	new_imethod->optimized = old_imethod->optimized;
	/* transformed stays 0 -- that is the whole point: the next call compiles the NEW header. */
	/* The replacement has happened, so the JIT is free to compile this method again -- from generation 2. */
	new_imethod->relink_pending = 0;

	/* Carry the wasm-JIT identity exactly as get_tier_up_imethod does. The code-bearing members are
	 * provably 0 here (refused above), but the hotness counter, the call profile and BOTH reservation
	 * pairs must survive: the function-table allocator has no free, so dropping a parked pair leaks two
	 * entries per method, silently, for exactly the methods that were about to get hot. */
	new_imethod->wasm_jit_slot = old_imethod->wasm_jit_slot;
	new_imethod->wasm_jit_desc = old_imethod->wasm_jit_desc;
	new_imethod->wasm_jit_fslot = old_imethod->wasm_jit_fslot;
	new_imethod->wasm_jit_hits = old_imethod->wasm_jit_hits;
	new_imethod->wasm_jit_bail = old_imethod->wasm_jit_bail;
	new_imethod->wasm_jit_invoke_in = old_imethod->wasm_jit_invoke_in;
	new_imethod->wasm_jit_invoke_out = old_imethod->wasm_jit_invoke_out;
	new_imethod->wasm_jit_block_n = old_imethod->wasm_jit_block_n;
	new_imethod->wasm_jit_resv_eslot = old_imethod->wasm_jit_resv_eslot;
	new_imethod->wasm_jit_resv_fslot = old_imethod->wasm_jit_resv_fslot;
	new_imethod->wasm_jit_self_resv_eslot = old_imethod->wasm_jit_self_resv_eslot;
	new_imethod->wasm_jit_self_resv_fslot = old_imethod->wasm_jit_self_resv_fslot;
	new_imethod->wasm_jit_profile = old_imethod->wasm_jit_profile;

	/* Retire the displaced body BEFORE publishing the new one. Frames already inside it keep running it
	 * to completion, which is what makes the swap safe -- but they must not tier up out of it. */
	old_imethod->retired = 1;

	mono_internal_hash_table_remove (&jit_mm->interp_code_hash, target);
	mono_internal_hash_table_insert (&jit_mm->interp_code_hash, target, new_imethod);
	jit_mm_unlock (jit_mm);

	/* Outside the jit-mm lock, and load-bearing: a caller that was already transformed baked the OLD
	 * InterpMethod* into its data_items, so without this it keeps calling generation 1 forever. */
	patch_imethod_refs (old_imethod, new_imethod);

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

static void
register_imethod_patch_site (InterpMethod *imethod, gpointer *ptr)
{
	g_assert (!imethod->optimized);
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
		if (data_items [index]->optimized_imethod) {
			// We are under tiering lock, check if the method has been tiered up already
			data_items [index] = data_items [index]->optimized_imethod;
			return;
		}
		register_imethod_patch_site (data_items [index], (gpointer*)&data_items [index]);
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
	gboolean tagged = INTERP_IMETHOD_IS_TAGGED_1 (*imethod_ptr);
	InterpMethod *imethod = INTERP_IMETHOD_UNTAG_1 (*imethod_ptr);
	if (imethod->optimized) {
		return;
	} else if (imethod->optimized_imethod) {
		*imethod_ptr = tagged ? imethod->optimized_imethod : INTERP_IMETHOD_TAG_1 (imethod->optimized_imethod);
		return;
	}

	mono_os_mutex_lock (&tiering_mutex);
	// We are under tiering lock, check if the method has been tiered up already
	if (imethod->optimized_imethod) {
		*imethod_ptr = tagged ? imethod->optimized_imethod : INTERP_IMETHOD_TAG_1 (imethod->optimized_imethod);
	} else {
		register_imethod_patch_site (imethod, imethod_ptr);
	}
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
