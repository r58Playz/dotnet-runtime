/**
 * \file
 * Collector debugging
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *  Rodrigo Kumpera (kumpera@gmail.com)
 *
 * Copyright 2005-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Copyright 2011 Xamarin, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include <string.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-cardtable.h"
#include "mono/sgen/sgen-protocol.h"
#include "mono/sgen/sgen-memory-governor.h"
#include "mono/sgen/sgen-pinning.h"
#include "mono/sgen/sgen-client.h"

#if _MSC_VER
#pragma warning(disable:4312) // FIXME pointer cast to different size
#endif

#ifndef DISABLE_SGEN_DEBUG_HELPERS


#define LOAD_VTABLE	SGEN_LOAD_VTABLE

#define object_is_forwarded	SGEN_OBJECT_IS_FORWARDED
#define object_is_pinned	SGEN_OBJECT_IS_PINNED
#define safe_object_get_size	sgen_safe_object_get_size

void sgen_describe_ptr (char *ptr);
void sgen_check_object (GCObject *obj);

/*
 * ######################################################################
 * ########  Collector debugging
 * ######################################################################
 */

static const char*descriptor_types [] = {
	"INVALID",
	"run length",
	"bitmap",
	"small pointer-free",
	"complex",
	"vector",
	"complex array",
	"complex pointer-free"
};

static char* describe_nursery_ptr (char *ptr, gboolean need_setup);

static void
describe_pointer (char *ptr, gboolean need_setup)
{
	GCVTable vtable;
	SgenDescriptor desc;
	int type;
	char *start;
	char *forwarded;
	mword size;

 restart:
	if (sgen_ptr_in_nursery (ptr)) {
		start = describe_nursery_ptr (ptr, need_setup);
		if (!start)
			return;
		ptr = start;
		vtable = LOAD_VTABLE ((GCObject*)ptr);
	} else {
		if (sgen_ptr_is_in_los (ptr, &start)) {
			if (ptr == start)
				printf ("Pointer is the start of object %p in LOS space.\n", start);
			else
				printf ("Pointer is at offset 0x%x of object %p in LOS space.\n", (int)(ptr - start), start);
			ptr = start;
			mono_sgen_los_describe_pointer (ptr);
			vtable = LOAD_VTABLE ((GCObject*)ptr);
		} else if (sgen_major_collector.ptr_is_in_non_pinned_space (ptr, &start)) {
			if (ptr == start)
				printf ("Pointer is the start of object %p in oldspace.\n", start);
			else if (start)
				printf ("Pointer is at offset 0x%x of object %p in oldspace.\n", (int)(ptr - start), start);
			else
				printf ("Pointer inside oldspace.\n");
			if (start)
				ptr = start;
			vtable = (GCVTable)sgen_major_collector.describe_pointer (ptr);
		} else if (sgen_major_collector.ptr_is_from_pinned_alloc (ptr)) {
			// FIXME: Handle pointers to the inside of objects
			printf ("Pointer is inside a pinned chunk.\n");
			vtable = LOAD_VTABLE ((GCObject*)ptr);
		} else {
			printf ("Pointer unknown.\n");
			return;
		}
	}

	if (object_is_pinned (ptr))
		printf ("Object is pinned.\n");

	if ((forwarded = (char *)object_is_forwarded (ptr))) {
		printf ("Object is forwarded to %p:\n", forwarded);
		ptr = forwarded;
		goto restart;
	}

	printf ("VTable: %p\n", vtable);
	if (vtable == NULL) {
		printf ("VTable is invalid (empty).\n");
		goto invalid_vtable;
	}
	if (sgen_ptr_in_nursery (vtable)) {
		printf ("VTable is invalid (points inside nursery).\n");
		goto invalid_vtable;
	}
	printf ("Class: %s.%s\n", sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));

	desc = sgen_vtable_get_descriptor (vtable);
	printf ("Descriptor: %lx\n", (long)desc);

	type = desc & DESC_TYPE_MASK;
	printf ("Descriptor type: %d (%s)\n", type, descriptor_types [type]);

	size = sgen_safe_object_get_size ((GCObject*)ptr);
	printf ("Size: %d\n", (int)size);

 invalid_vtable:
	;
	sgen_client_describe_invalid_pointer ((GCObject *) ptr);
}

void
sgen_describe_ptr (char *ptr)
{
	describe_pointer (ptr, TRUE);
}

static gboolean missing_remsets;

/*
 * We let a missing remset slide if the target object is pinned,
 * because the store might have happened but the remset not yet added,
 * but in that case the target must be pinned.  We might theoretically
 * miss some missing remsets this way, but it's very unlikely.
 */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr) && sgen_ptr_in_nursery ((char*)*(ptr))) {	\
			if (!sgen_get_remset ()->find_address ((char*)(ptr)) && !sgen_cement_lookup (*(ptr))) { \
				GCVTable __vt = SGEN_LOAD_VTABLE (obj);	\
				gboolean is_pinned = object_is_pinned (*(ptr));	\
				SGEN_LOG (0, "Oldspace->newspace reference %p at offset %ld in object %p (%s.%s) not found in remsets%s.", *(ptr), (long)((char*)(ptr) - (char*)(obj)), (obj), sgen_client_vtable_get_namespace (__vt), sgen_client_vtable_get_name (__vt), is_pinned ? ", but object is pinned" : ""); \
				sgen_binary_protocol_missing_remset ((obj), __vt, (int) ((char*)(ptr) - (char*)(obj)), *(ptr), (gpointer)LOAD_VTABLE(*(ptr)), is_pinned); \
				if (!is_pinned)				\
					missing_remsets = TRUE;		\
			}						\
		}							\
	} while (0)

/*
 * Check that each object reference which points into the nursery can
 * be found in the remembered sets.
 */
static void
check_consistency_callback (GCObject *obj, size_t size, void *dummy)
{
	char *start = (char*)obj;
	GCVTable vtable = LOAD_VTABLE (obj);
	SgenDescriptor desc = sgen_vtable_get_descriptor (vtable);
	SGEN_LOG (8, "Scanning object %p, vtable: %p (%s)", start, vtable, sgen_client_vtable_get_name (vtable));

#include "sgen-scan-object.h"
}

/*
 * Perform consistency check of the heap.
 *
 * Assumes the world is stopped.
 */
void
sgen_check_remset_consistency (void)
{
	// Need to add more checks

	missing_remsets = FALSE;

	SGEN_LOG (1, "Begin heap consistency check...");

	// Check that oldspace->newspace pointers are registered with the collector
	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)check_consistency_callback, NULL);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_consistency_callback, NULL);

	SGEN_LOG (1, "Heap consistency check done.");

	if (missing_remsets)
		sgen_binary_protocol_flush_buffers (TRUE);
	if (!sgen_binary_protocol_is_enabled ())
		g_assert (!missing_remsets);
}

static gboolean
is_major_or_los_object_marked (GCObject *obj)
{
	if (sgen_safe_object_get_size ((GCObject*)obj) > SGEN_MAX_SMALL_OBJ_SIZE) {
		return sgen_los_object_is_pinned (obj);
	} else {
		return sgen_get_major_collector ()->is_object_live (obj);
	}
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
	if (*(ptr) && !sgen_ptr_in_nursery ((char*)*(ptr)) && !is_major_or_los_object_marked ((GCObject*)*(ptr))) { \
		if (!cards || !sgen_get_remset ()->find_address_with_cards (start, cards, (char*)(ptr))) { \
			GCVTable __vt = SGEN_LOAD_VTABLE (obj);	\
			SGEN_LOG (0, "major->major reference %p at offset %ld in object %p (%s.%s) not found in remsets.", *(ptr), (long)((char*)(ptr) - (char*)(obj)), (obj), sgen_client_vtable_get_namespace (__vt), sgen_client_vtable_get_name (__vt)); \
			sgen_binary_protocol_missing_remset ((obj), __vt, (int) ((char*)(ptr) - (char*)(obj)), *(ptr), (gpointer)LOAD_VTABLE(*(ptr)), object_is_pinned (*(ptr))); \
			missing_remsets = TRUE;				\
		}																\
	}																	\
	} while (0)

static void
check_mod_union_callback (GCObject *obj, size_t size, void *dummy)
{
	char *start = (char*)obj;
	gboolean in_los = (gboolean) (size_t) dummy;
	GCVTable vtable = LOAD_VTABLE (obj);
	SgenDescriptor desc = sgen_vtable_get_descriptor (vtable);
	guint8 *cards;
	SGEN_LOG (8, "Scanning object %p, vtable: %p (%s)", obj, vtable, sgen_client_vtable_get_name (vtable));

	if (!is_major_or_los_object_marked (obj))
		return;

	if (in_los)
		cards = sgen_los_header_for_object (obj)->cardtable_mod_union;
	else
		cards = sgen_get_major_collector ()->get_cardtable_mod_union_for_reference (start);

#include "sgen-scan-object.h"
}

void
sgen_check_mod_union_consistency (void)
{
	missing_remsets = FALSE;

	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)check_mod_union_callback, (void*)FALSE);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_mod_union_callback, (void*)TRUE);

	if (!sgen_binary_protocol_is_enabled ())
		g_assert (!missing_remsets);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		if (*(ptr) && !LOAD_VTABLE (*(ptr)))						\
			g_error ("Could not load vtable for obj %p slot %ld (size %ld)", obj, (long)((char*)ptr - (char*)obj), (long)safe_object_get_size ((GCObject*)obj)); \
	} while (0)

static void
check_major_refs_callback (GCObject *obj, size_t size, void *dummy)
{
	char *start = (char*)obj;
	SgenDescriptor desc = sgen_obj_get_descriptor (obj);

#include "sgen-scan-object.h"
}

void
sgen_check_major_refs (void)
{
	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)check_major_refs_callback, NULL);
	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_major_refs_callback, NULL);
}

/* Check that the reference is valid */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr)) {	\
			g_assert (sgen_client_vtable_get_namespace (SGEN_LOAD_VTABLE_UNCHECKED (*(ptr))));	\
		}	\
	} while (0)

/*
 * sgen_check_object:
 *
 *   Perform consistency check on an object. Currently we only check that the
 * reference fields are valid.
 */
void
sgen_check_object (GCObject *obj)
{
	char *start = (char*)obj;
	SgenDescriptor desc;

	if (!start)
		return;

	desc = sgen_obj_get_descriptor (obj);

#include "sgen-scan-object.h"
}


static GCObject **valid_nursery_objects;
static int valid_nursery_object_count;
static gboolean broken_heap;

static void
setup_mono_sgen_scan_area_with_callback (GCObject *object, size_t size, void *data)
{
	valid_nursery_objects [valid_nursery_object_count++] = object;
}

static void
setup_valid_nursery_objects (void)
{
	if (!valid_nursery_objects)
		valid_nursery_objects = (GCObject **)sgen_alloc_os_memory (sgen_nursery_max_size, (SgenAllocFlags)(SGEN_ALLOC_INTERNAL | SGEN_ALLOC_ACTIVATE), "debugging data", MONO_MEM_ACCOUNT_SGEN_DEBUGGING);
	valid_nursery_object_count = 0;
	sgen_scan_area_with_callback (sgen_nursery_section->data, sgen_nursery_section->end_data, setup_mono_sgen_scan_area_with_callback, NULL, FALSE, FALSE);
}

static gboolean
find_object_in_nursery_dump (char *object)
{
	int first = 0, last = valid_nursery_object_count;
	while (first < last) {
		int middle = first + ((last - first) >> 1);
		if (object == (char*)valid_nursery_objects [middle])
			return TRUE;

		if (object < (char*)valid_nursery_objects [middle])
			last = middle;
		else
			first = middle + 1;
	}
	g_assert (first == last);
	return FALSE;
}

static void
iterate_valid_nursery_objects (IterateObjectCallbackFunc callback, void *data)
{
	int i;
	for (i = 0; i < valid_nursery_object_count; ++i) {
		GCObject *obj = valid_nursery_objects [i];
		callback (obj, safe_object_get_size (obj), data);
	}
}

static char*
describe_nursery_ptr (char *ptr, gboolean need_setup)
{
	int i;

	if (need_setup)
		setup_valid_nursery_objects ();

	for (i = 0; i < valid_nursery_object_count - 1; ++i) {
		if ((char*)valid_nursery_objects [i + 1] > ptr)
			break;
	}

	if (i >= valid_nursery_object_count || (char*)valid_nursery_objects [i] + safe_object_get_size (valid_nursery_objects [i]) < ptr) {
		SGEN_LOG (0, "nursery-ptr (unalloc'd-memory)");
		return NULL;
	} else {
		GCObject *obj = valid_nursery_objects [i];
		if ((char*)obj == ptr)
			SGEN_LOG (0, "nursery-ptr %p", obj);
		else
			SGEN_LOG (0, "nursery-ptr %p (interior-ptr offset %ld)", obj, (long)(ptr - (char*)obj));
		return (char*)obj;
	}
}

/*
 * SCANNED-REFERENCE CHECK (MONO_GC_DEBUG=check-scanned-refs).
 *
 * Names the object that holds a corrupt reference, at the moment the minor collector follows it.
 *
 * WHY NOT REUSE is_valid_object_pointer / bad_pointer_spew DIRECTLY. Those are built for the whole-heap check,
 * which runs BEFORE a collection: the nursery arm goes through find_object_in_nursery_dump, which reads the
 * valid_nursery_objects[] snapshot that setup_valid_nursery_objects() builds at that point. Called from the
 * scan path they would consult a stale or absent snapshot mid-collection, with objects already forwarded, and
 * answer nonsense. The whole-heap check is also O(heap) and on this workload (a ~2.7 GB heap) cannot finish a
 * boot, which is why the failure has never been attributable.
 *
 * So this is deliberately RANGE-ONLY, which is sufficient for the failure being chased: a wasm
 * "memory access out of bounds" means the address left linear memory, so a bounds test catches it, is O(1),
 * needs no snapshot, and is correct at any point in a collection. It reads at most one word, and only after
 * range-checking the address it is about to read -- so the check itself can never trap.
 *
 * Two distinct faults are reported separately, because they mean different things:
 *   REF_OOR   the reference VALUE is not a plausible address -> whoever wrote this field wrote a non-pointer.
 *   REF_VT    the reference is in range but the referent's vtable word is not -> the field points at memory
 *             that is not an object (freed/reused nursery space, or an object whose header was overwritten).
 */
gboolean sgen_check_scanned_refs = FALSE;

static gsize
scanned_ref_memsz (void)
{
#if defined (HOST_BROWSER) && defined (__wasm__)
	/* Same idiom as mini-wasm.c's wj_memsz: pages -> bytes, and never 0 (which would reject everything). */
	gsize s = (gsize) __builtin_wasm_memory_size (0) << 16;
	return s ? s : (gsize) -1;
#else
	return (gsize) -1;
#endif
}

/*
 * Context for a bad slot: the container's array length and the neighbouring words.
 *
 * This is the question the first round of reports could not answer. Every report was identical --
 * value 0xfffffff8 (-8) at offset 5680 in a System.Object[] -- and the two candidate mechanisms are
 * distinguished purely by where 5680 sits and what surrounds it:
 *
 *   index == max_length - 1, neighbours valid  -> a write one PAST the intended range (an overrun storing a
 *                                                 count/sentinel just after the data)
 *   index mid-array, neighbours also non-pointers -> a run of primitives in a reference array (type confusion)
 *   index mid-array, neighbours all valid refs  -> a single targeted bad store
 *
 * max_length lives at MONO_STRUCT_OFFSET(MonoArray, max_length); reading it via the raw offset keeps sgen
 * free of client type dependencies, and it is only read after a range check. Every read here is bounded, so
 * the diagnostic cannot trap even on a wildly corrupt container.
 */
static void
scanned_ref_context (GCObject *container, void **slot, gsize memsz)
{
	gsize base = (gsize) container;
	gsize off = (gsize) ((char *) slot - (char *) container);
	int i;

	/* MonoObject{vtable,sync} + MonoArray{bounds,max_length}: max_length is the 4th pointer-sized word. */
	gsize lenoff = 3 * SIZEOF_VOID_P;
	if (base < memsz && memsz - base > lenoff + SIZEOF_VOID_P) {
		guint32 len = *(guint32 *) (base + lenoff);
		gsize dataoff = 4 * SIZEOF_VOID_P;
		SGEN_LOG (0, "REF_CTX container %p offset %ld candidate max_length=%u (index %ld of %u; %s)",
				container, (long) off, len,
				(long) ((off - dataoff) / SIZEOF_VOID_P), len,
				((off - dataoff) / SIZEOF_VOID_P) == (gsize) (len - 1) ? "LAST ELEMENT" : "mid-array");
	}

	/* Four words either side, so a run of primitives is distinguishable from one bad store.
	 *
	 * Each neighbour's CLASS is reported too, which is what identifies the table: an Object[] is typeless by
	 * declaration, so the only way to learn what it holds is to ask the elements. Guarded in layers -- the
	 * element value, then its vtable word, then the vtable's first word (MonoVTable.klass) -- because naming
	 * a class dereferences vtable->klass->name and a diagnostic that traps is worse than no diagnostic. Only
	 * a neighbour that passes all three is named; anything else prints its raw value alone. */
	for (i = -4; i <= 4; i++) {
		gsize a = base + off + (gsize) (i * (int) SIZEOF_VOID_P);
		gsize v, vtw, klass;
		const char *ns = NULL, *nm = NULL;

		if (a < base || a >= memsz || memsz - a < SIZEOF_VOID_P)
			continue;
		v = (gsize) *(void **) a;
		if (v && !(v & (SIZEOF_VOID_P - 1)) && v < memsz && memsz - v >= SIZEOF_VOID_P) {
			vtw = (gsize) SGEN_POINTER_UNTAG_ALL (*(void **) v);
			if (vtw && !(vtw & (SIZEOF_VOID_P - 1)) && vtw < memsz && memsz - vtw >= SIZEOF_VOID_P) {
				klass = (gsize) *(void **) vtw;
				if (klass && !(klass & (SIZEOF_VOID_P - 1)) && klass < memsz && memsz - klass >= SIZEOF_VOID_P) {
					ns = sgen_client_vtable_get_namespace ((GCVTable) vtw);
					nm = sgen_client_vtable_get_name ((GCVTable) vtw);
				}
			}
		}
		SGEN_LOG (0, "REF_CTX   [%+d] @%p = %p  %s%s%s", i, (void *) a, (void *) v,
				ns ? ns : "(unnamed)", ns && *ns ? "." : "", nm ? nm : "");
	}
}

void
sgen_check_scanned_ref (GCObject *container, void **slot)
{
	gsize memsz = scanned_ref_memsz ();
	gsize ref = (gsize) *slot;
	gsize vtw;
	GCVTable cvt;

	if (!ref)
		return;

	/* A tagged (forwarded/pinned) reference is not what a field holds; only object starts are tagged. */
	if (ref & (SIZEOF_VOID_P - 1) || ref >= memsz || memsz - ref < SIZEOF_VOID_P) {
		cvt = SGEN_LOAD_VTABLE (container);
		SGEN_LOG (0, "REF_OOR bad reference %p at offset %ld in object %p (%s.%s) — the value in this field is "
				"not an address (memsz %p)", (void*) ref, (long) ((char*) slot - (char*) container),
				container, sgen_client_vtable_get_namespace (cvt), sgen_client_vtable_get_name (cvt),
				(void*) memsz);
		broken_heap = TRUE;
		scanned_ref_context (container, slot, memsz);
		return;
	}

	/* Safe: the word at `ref` was just range-checked. Untag before judging it — a referent that is already
	 * forwarded or pinned legitimately carries tag bits in its first word. */
	vtw = (gsize) SGEN_POINTER_UNTAG_ALL (*(void**) ref);
	if (!vtw || (vtw & (SIZEOF_VOID_P - 1)) || vtw >= memsz || memsz - vtw < SIZEOF_VOID_P) {
		cvt = SGEN_LOAD_VTABLE (container);
		SGEN_LOG (0, "REF_VT reference %p at offset %ld in object %p (%s.%s) points at a non-object — its "
				"vtable word is %p (memsz %p)", (void*) ref, (long) ((char*) slot - (char*) container),
				container, sgen_client_vtable_get_namespace (cvt), sgen_client_vtable_get_name (cvt),
				(void*) vtw, (void*) memsz);
		broken_heap = TRUE;
		scanned_ref_context (container, slot, memsz);
	}
}

/*
 * STORE-SIDE guard: judge a reference as it is WRITTEN, instead of when the collector trips over it.
 *
 * check-scanned-refs answered "what is corrupt" -- a growing System.Object[] of
 * System.Reflection.Emit.DynamicMethod, one element replaced by 0xfffffff8 (-8) or by a stale nursery address,
 * mid-array, with valid neighbours. It cannot answer "who wrote it", because by the time a collection runs the
 * writer is many thousands of frames gone.
 *
 * The interpreter is not the writer, and that is established rather than assumed: MINT_STELEM_REF runs
 * mono_interp_isinst on every non-null value, which dereferences the value's vtable, so storing -8 through it
 * would trap inside the type check instead of surviving to the next GC. That leaves the store paths with no
 * type check at all -- Array.Copy on a reference array, a raw store through a byref, and native runtime code
 * calling mono_array_setref_fast -- and every one of those passes through a write barrier. So the barriers are
 * the chokepoint, and this is a guard on all five of them.
 *
 * The check is the same range-only plausibility test the scan-side guard uses, for the same reason: it reads at
 * most one word and only after range-checking it, so the diagnostic cannot itself trap.
 *
 * On a hit it prints the wasm stack, which is the entire point -- that names the writer. Rate-limited, because
 * a corrupt slot rewritten in a loop would otherwise produce a log too large to read; the one-line report is
 * unlimited so the total count stays honest.
 */
#if defined (HOST_BROWSER) && defined (__wasm__)
/* Defined in mini-wasm.c. Logs a JS `new Error().stack`, which is the only stack source here that resolves wasm
 * frame names without a rebuild. Declared by hand rather than via a mini header: sgen deliberately does not
 * depend on the JIT's headers, and one extern is a smaller violation than one include. */
extern void mono_wasm_print_stack_trace (void);
#else
#define mono_wasm_print_stack_trace() do { } while (0)
#endif

gboolean sgen_check_stored_refs = FALSE;
static int stored_ref_stacks = 0;

/* Non-NULL values only; a null store is legal. Reports the referent's vtable word for the caller's message. */
static gboolean
stored_ref_is_bad (gsize v, gsize memsz, gsize *vtw_out)
{
	gsize vtw;

	*vtw_out = 0;
	if (!v)
		return FALSE;
	if ((v & (SIZEOF_VOID_P - 1)) || v >= memsz || memsz - v < SIZEOF_VOID_P)
		return TRUE;
	/* Safe: just range-checked. Untag -- a value copied out of a heap slot mid-collection can be forwarded. */
	vtw = (gsize) SGEN_POINTER_UNTAG_ALL (*(void **) v);
	*vtw_out = vtw;
	return !vtw || (vtw & (SIZEOF_VOID_P - 1)) || vtw >= memsz || memsz - vtw < SIZEOF_VOID_P;
}

/**
 * \param container the object being stored into, or NULL when the barrier only knows the slot address
 * \param slot the destination
 * \param value the reference being stored
 * \param where which barrier caught it, for the report
 */
void
sgen_check_stored_ref (GCObject *container, void **slot, void *value, const char *where)
{
	gsize memsz = scanned_ref_memsz ();
	gsize v = (gsize) value, vtw = 0;

	if (!stored_ref_is_bad (v, memsz, &vtw))
		return;

	if (container) {
		GCVTable cvt = SGEN_LOAD_VTABLE (container);
		SGEN_LOG (0, "STORED_REF %s wrote non-object %p (vtable word %p) at offset %ld into %p (%s.%s), memsz %p",
				where, (void *) v, (void *) vtw, (long) ((char *) slot - (char *) container), container,
				sgen_client_vtable_get_namespace (cvt), sgen_client_vtable_get_name (cvt), (void *) memsz);
	} else {
		SGEN_LOG (0, "STORED_REF %s wrote non-object %p (vtable word %p) into slot %p, memsz %p",
				where, (void *) v, (void *) vtw, slot, (void *) memsz);
	}

	if (++stored_ref_stacks <= 12) {
		if (container)
			scanned_ref_context (container, slot, memsz);
		mono_wasm_print_stack_trace ();
	}
}

static gboolean
is_valid_object_pointer (char *object)
{
	if (sgen_ptr_in_nursery (object))
		return find_object_in_nursery_dump (object);

	if (sgen_los_is_valid_object (object))
		return TRUE;

	if (sgen_major_collector.is_valid_object (object))
		return TRUE;
	return FALSE;
}

static void
bad_pointer_spew (char *obj, char **slot)
{
	char *ptr = *slot;
	GCVTable vtable = LOAD_VTABLE ((GCObject*)obj);

	SGEN_LOG (0, "Invalid object pointer %p at offset %ld in object %p (%s.%s):", ptr,
			(long)((char*)slot - obj),
			obj, sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));
	describe_pointer (ptr, FALSE);
	broken_heap = TRUE;
}

static void
missing_remset_spew (char *obj, char **slot)
{
	char *ptr = *slot;
	GCVTable vtable = LOAD_VTABLE ((GCObject*)obj);

	SGEN_LOG (0, "Oldspace->newspace reference %p at offset %ld in object %p (%s.%s) not found in remsets.",
			ptr, (long)((char*)slot - obj), obj,
			sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));

	broken_heap = TRUE;
}

/*
FIXME Flag missing remsets due to pinning as non fatal
*/
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		if (*(char**)ptr) {					\
			if (!is_valid_object_pointer (*(char**)ptr)) {	\
				bad_pointer_spew ((char*)obj, (char**)ptr); \
			} else if (!sgen_ptr_in_nursery (obj) && sgen_ptr_in_nursery ((char*)*ptr)) { \
				if (!allow_missing_pinned && !SGEN_OBJECT_IS_PINNED (*(ptr)) && !sgen_get_remset ()->find_address ((char*)(ptr)) && !sgen_cement_lookup (*(ptr))) \
					missing_remset_spew ((char*)obj, (char**)ptr); \
			}						\
		}							\
	} while (0)

static void
verify_object_pointers_callback (GCObject *obj, size_t size, void *data)
{
	char *start = (char*)obj;
	gboolean allow_missing_pinned = (gboolean) (size_t) data;
	SgenDescriptor desc = sgen_obj_get_descriptor_safe (obj);

#include "sgen-scan-object.h"
}

/*
FIXME:
-This heap checker is racy regarding inlined write barriers and other JIT tricks that
depend on OP_DUMMY_USE.
*/
void
sgen_check_whole_heap (gboolean allow_missing_pinned)
{
	setup_valid_nursery_objects ();

	broken_heap = FALSE;
	sgen_scan_area_with_callback (sgen_nursery_section->data, sgen_nursery_section->end_data, verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned, FALSE, TRUE);
	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned);
	sgen_los_iterate_objects (verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned);

	g_assert (!broken_heap);
}

static gboolean
ptr_in_heap (char *object)
{
	if (sgen_ptr_in_nursery (object))
		return TRUE;

	if (sgen_los_is_valid_object (object))
		return TRUE;

	if (sgen_major_collector.is_valid_object (object))
		return TRUE;
	return FALSE;
}

/*
 * sgen_check_objref:
 *   Do consistency checks on the object reference OBJ. Assert on failure.
 */
void
sgen_check_objref (char *obj)
{
	g_assert (ptr_in_heap (obj));
}

static void
find_pinning_ref_from_thread (char *obj, size_t size)
{
#ifndef SGEN_WITHOUT_MONO
	char *endobj = obj + size;

	FOREACH_THREAD_EXCLUDE (info, MONO_THREAD_INFO_FLAGS_NO_GC) {
		mword *ctxstart, *ctxcurrent, *ctxend;
		char **start = (char**)info->client_info.stack_start;
		if (info->client_info.skip)
			continue;
		while (start < (char**)info->client_info.info.stack_end) {
			if (*start >= obj && *start < endobj)
				SGEN_LOG (0, "Object %p referenced in thread %p (id %p) at %p, stack: %p-%p", obj, info, (gpointer)(gsize) mono_thread_info_get_tid (info), start, info->client_info.stack_start, info->client_info.info.stack_end);
			start++;
		}

		for (ctxstart = ctxcurrent = (mword*) &info->client_info.ctx, ctxend = (mword*) (&info->client_info.ctx + 1); ctxcurrent < ctxend; ctxcurrent ++) {
			mword w = *ctxcurrent;

			if (w >= (mword)obj && w < (mword)obj + size)
				SGEN_LOG (0, "Object %p referenced in saved reg %d of thread %p (id %p)", obj, (int) (ctxcurrent - ctxstart), info, (gpointer)(gsize) mono_thread_info_get_tid (info));
		}
	} FOREACH_THREAD_END
#endif
}

/*
 * Debugging function: find in the conservative roots where @obj is being pinned.
 */
static G_GNUC_UNUSED void
find_pinning_reference (char *obj, size_t size)
{
	char **start;
	RootRecord *root;
	char *endobj = obj + size;

	SGEN_HASH_TABLE_FOREACH (&sgen_roots_hash [ROOT_TYPE_NORMAL], char **, start, RootRecord *, root) {
		/* if desc is non-null it has precise info */
		if (!root->root_desc) {
			while (start < (char**)root->end_root) {
				if (*start >= obj && *start < endobj) {
					SGEN_LOG (0, "Object %p referenced in pinned roots %p-%p\n", obj, start, root->end_root);
				}
				start++;
			}
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	find_pinning_ref_from_thread (obj, size);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		char* __target = *(char**)ptr;				\
		if (__target) {						\
			if (sgen_ptr_in_nursery (__target)) {		\
				g_assert (!SGEN_OBJECT_IS_FORWARDED (__target)); \
			} else {					\
				mword __size = sgen_safe_object_get_size ((GCObject*)__target); \
				if (__size <= SGEN_MAX_SMALL_OBJ_SIZE)	\
					g_assert (sgen_major_collector.is_object_live ((GCObject*)__target)); \
				else					\
					g_assert (sgen_los_object_is_pinned ((GCObject*)__target)); \
			}						\
		}							\
	} while (0)

static void
check_marked_callback (GCObject *obj, size_t size, void *dummy)
{
	char *start = (char*)obj;
	gboolean flag = (gboolean) (size_t) dummy;
	SgenDescriptor desc;

	if (sgen_ptr_in_nursery (start)) {
		if (flag)
			SGEN_ASSERT (0, SGEN_OBJECT_IS_PINNED (obj), "All objects remaining in the nursery must be pinned");
	} else if (flag) {
		if (!sgen_los_object_is_pinned (obj))
			return;
	} else {
		if (!sgen_major_collector.is_object_live (obj))
			return;
	}

	desc = sgen_obj_get_descriptor_safe (obj);

#include "sgen-scan-object.h"
}

void
sgen_check_heap_marked (gboolean nursery_must_be_pinned)
{
	setup_valid_nursery_objects ();

	iterate_valid_nursery_objects (check_marked_callback, (void*)(size_t)nursery_must_be_pinned);
	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, check_marked_callback, (void*)FALSE);
	sgen_los_iterate_objects (check_marked_callback, (void*)TRUE);
}

static void
check_nursery_objects_untag_callback (char *obj, size_t size, void *data)
{
	g_assert (!SGEN_OBJECT_IS_FORWARDED (obj));
	g_assert (!SGEN_OBJECT_IS_PINNED (obj));
}

void
sgen_check_nursery_objects_untag (void)
{
	sgen_clear_nursery_fragments ();
	sgen_scan_area_with_callback (sgen_nursery_section->data, sgen_nursery_section->end_data,
			(IterateObjectCallbackFunc)check_nursery_objects_untag_callback, NULL, FALSE, TRUE);
}

static void
verify_scan_starts (char *start, char *end)
{
	size_t i;

	for (i = 0; i < sgen_nursery_section->num_scan_start; ++i) {
		char *addr = sgen_nursery_section->scan_starts [i];
		if (addr > start && addr < end)
			SGEN_LOG (0, "NFC-BAD SCAN START [%lu] %p for obj [%p %p]", (unsigned long)i, addr, start, end);
	}
}

void
sgen_debug_verify_nursery (gboolean do_dump_nursery_content)
{
	char *start, *end, *cur, *hole_start;

	if (sgen_nursery_canaries_enabled ())
		SGEN_LOG (0, "Checking nursery canaries...");

	/*This cleans up unused fragments */
	sgen_nursery_allocator_prepare_for_pinning ();

	hole_start = start = cur = sgen_get_nursery_start ();
	end = sgen_get_nursery_end ();

	while (cur < end) {
		size_t ss, size;
		gboolean is_array_fill;

		if (!*(void**)cur) {
			cur += sizeof (void*);
			continue;
		}

		if (object_is_forwarded (cur))
			SGEN_LOG (0, "FORWARDED OBJ %p", cur);
		else if (object_is_pinned (cur))
			SGEN_LOG (0, "PINNED OBJ %p", cur);

		ss = safe_object_get_size ((GCObject*)cur);
		size = SGEN_ALIGN_UP (ss);
		verify_scan_starts (cur, cur + size);
		is_array_fill = sgen_client_object_is_array_fill ((GCObject*)cur);
		if (do_dump_nursery_content) {
			GCVTable vtable = SGEN_LOAD_VTABLE ((GCObject*)cur);
			if (cur > hole_start)
				SGEN_LOG (0, "HOLE [%p %p %d]", hole_start, cur, (int)(cur - hole_start));
			SGEN_LOG (0, "OBJ  [%p %p %d %d %s.%s %d]", cur, cur + size, (int)size, (int)ss,
					sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable),
					is_array_fill);
		}
		if (sgen_nursery_canaries_enabled () && !is_array_fill) {
			CHECK_CANARY_FOR_OBJECT ((GCObject*)cur, TRUE);
			CANARIFY_SIZE (size);
		}
		cur += size;
		hole_start = cur;
	}
}

/*
 * Checks that no objects in the nursery are forwarded or pinned.  This
 * is a precondition to restarting the mutator while doing a
 * concurrent collection.  Note that we don't clear fragments because
 * we depend on that having happened earlier.
 */
void
sgen_debug_check_nursery_is_clean (void)
{
	char *end, *cur;

	cur = sgen_get_nursery_start ();
	end = sgen_get_nursery_end ();

	while (cur < end) {
		size_t size;

		if (!*(void**)cur) {
			cur += sizeof (void*);
			continue;
		}

		g_assert (!object_is_forwarded (cur));
		g_assert (!object_is_pinned (cur));

		size = SGEN_ALIGN_UP (safe_object_get_size ((GCObject*)cur));
		verify_scan_starts (cur, cur + size);

		cur += size;
	}
}

static gboolean scan_object_for_specific_ref_precise = TRUE;

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj) do {					\
		if ((GCObject*)*(ptr) == key) {				\
			GCVTable vtable = SGEN_LOAD_VTABLE (*(ptr));	\
			g_print ("found ref to %p in object %p (%s.%s) at offset %ld\n", \
					key, (obj), sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable), (long)(((char*)(ptr) - (char*)(obj)))); \
		}							\
	} while (0)

static void
scan_object_for_specific_ref (GCObject *obj, GCObject *key)
{
	GCObject *forwarded;

	if ((forwarded = SGEN_OBJECT_IS_FORWARDED (obj)))
		obj = forwarded;

	if (scan_object_for_specific_ref_precise) {
		char *start = (char*)obj;
		SgenDescriptor desc = sgen_obj_get_descriptor_safe (obj);
		#include "sgen-scan-object.h"
	} else {
		mword *words = (mword*)obj;
		size_t size = safe_object_get_size (obj);
		for (gsize i = 0; i < size / sizeof (mword); ++i) {
			if (words [i] == (mword)key) {
				GCVTable vtable = SGEN_LOAD_VTABLE (obj);
				g_print ("found possible ref to %p in object %p (%s.%s) at offset %ld\n",
						key, obj, sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable), (long)(i * sizeof (mword)));
			}
		}
	}
}

static void
scan_object_for_specific_ref_callback (GCObject *obj, size_t size, GCObject *key)
{
	scan_object_for_specific_ref (obj, key);
}

static void
check_root_obj_specific_ref (RootRecord *root, GCObject *key, GCObject *obj)
{
	if (key != obj)
		return;
	g_print ("found ref to %p in root record %p\n", key, root);
}

static GCObject *check_key = NULL;
static RootRecord *check_root = NULL;

static void
check_root_obj_specific_ref_from_marker (GCObject **obj, void *gc_data)
{
	check_root_obj_specific_ref (check_root, check_key, *obj);
}

static void
scan_roots_for_specific_ref (GCObject *key, int root_type)
{
	void **start_root;
	RootRecord *root;
	check_key = key;

	SGEN_HASH_TABLE_FOREACH (&sgen_roots_hash [root_type], void **, start_root, RootRecord *, root) {
		SgenDescriptor desc = root->root_desc;

		check_root = root;

		switch (desc & ROOT_DESC_TYPE_MASK) {
		case ROOT_DESC_BITMAP:
			desc >>= ROOT_DESC_TYPE_SHIFT;
			while (desc) {
				if (desc & 1)
					check_root_obj_specific_ref (root, key, (GCObject *)*start_root);
				desc >>= 1;
				start_root++;
			}
			return;
		case ROOT_DESC_COMPLEX: {
			gsize *bitmap_data = (gsize *)sgen_get_complex_descriptor_bitmap (desc);
			int bwords = (int) ((*bitmap_data) - 1);
			void **start_run = start_root;
			bitmap_data++;
			while (bwords-- > 0) {
				gsize bmap = *bitmap_data++;
				void **objptr = start_run;
				while (bmap) {
					if (bmap & 1)
						check_root_obj_specific_ref (root, key, (GCObject *)*objptr);
					bmap >>= 1;
					++objptr;
				}
				start_run += GC_BITS_PER_WORD;
			}
			break;
		}
		case ROOT_DESC_VECTOR: {
			void **p;

			for (p = start_root; p < (void**)root->end_root; p++) {
				if (*p)
					check_root_obj_specific_ref (root, key, (GCObject *)*p);
			}
			break;
		}
		case ROOT_DESC_USER: {
			SgenUserRootMarkFunc marker = sgen_get_user_descriptor_func (desc);
			marker (start_root, check_root_obj_specific_ref_from_marker, NULL);
			break;
		}
		case ROOT_DESC_RUN_LEN:
			g_assert_not_reached ();
		default:
			g_assert_not_reached ();
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	check_key = NULL;
	check_root = NULL;
}

void
mono_gc_scan_for_specific_ref (GCObject *key, gboolean precise)
{
	void **ptr;
	RootRecord *root;

	scan_object_for_specific_ref_precise = precise;

	sgen_scan_area_with_callback (sgen_nursery_section->data, sgen_nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key, TRUE, FALSE);

	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	scan_roots_for_specific_ref (key, ROOT_TYPE_NORMAL);
	scan_roots_for_specific_ref (key, ROOT_TYPE_WBARRIER);

	SGEN_HASH_TABLE_FOREACH (&sgen_roots_hash [ROOT_TYPE_PINNED], void **, ptr, RootRecord *, root) {
		while (ptr < (void**)root->end_root) {
			check_root_obj_specific_ref (root, (GCObject *)*ptr, key);
			++ptr;
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	if (sgen_is_world_stopped ())
		find_pinning_ref_from_thread ((char*)key, sizeof (GCObject));
}

/* If not null, dump the heap after each collection into this file */
static FILE *heap_dump_file = NULL;

void
sgen_dump_occupied (char *start, char *end, char *section_start)
{
	fprintf (heap_dump_file, "<occupied offset=\"%ld\" size=\"%ld\"/>\n", (long)(start - section_start), (long)(end - start));
}

void
sgen_dump_section (GCMemSection *section, const char *type)
{
	char *start = section->data;
	char *end = section->end_data;
	char *occ_start = NULL;

	fprintf (heap_dump_file, "<section type=\"%s\" size=\"%lu\">\n", type, (unsigned long)(section->end_data - section->data));

	while (start < end) {
		guint size;
		//GCVTable vt;
		//MonoClass *class;

		if (!*(void**)start) {
			if (occ_start) {
				sgen_dump_occupied (occ_start, start, section->data);
				occ_start = NULL;
			}
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		if (!occ_start)
			occ_start = start;

		//vt = SGEN_LOAD_VTABLE (start);
		//class = vt->klass;

		size = SGEN_ALIGN_UP (safe_object_get_size ((GCObject*) start));

		/*
		fprintf (heap_dump_file, "<object offset=\"%d\" class=\"%s.%s\" size=\"%d\"/>\n",
				start - section->data,
				vt->klass->name_space, vt->klass->name,
				size);
		*/

		start += size;
	}
	if (occ_start)
		sgen_dump_occupied (occ_start, start, section->data);

	fprintf (heap_dump_file, "</section>\n");
}

static void
dump_object (GCObject *obj, gboolean dump_location)
{
#ifndef SGEN_WITHOUT_MONO
	static char class_name [1024];

	MonoClass *klass = mono_object_class (obj);
	int i, j;

	/*
	 * Python's XML parser is too stupid to parse angle brackets
	 * in strings, so we just ignore them;
	 */
	i = j = 0;
	while (m_class_get_name (klass) [i] && j < sizeof (class_name) - 1) {
		if (!strchr ("<>\"", m_class_get_name (klass) [i]))
			class_name [j++] = m_class_get_name (klass) [i];
		++i;
	}
	g_assert (j < sizeof (class_name));
	class_name [j] = 0;

	fprintf (heap_dump_file, "<object class=\"%s.%s\" size=\"%ld\"",
			m_class_get_name_space (klass), class_name,
			(long)safe_object_get_size (obj));
	if (dump_location) {
		const char *location;
		if (sgen_ptr_in_nursery (obj))
			location = "nursery";
		else if (safe_object_get_size (obj) <= SGEN_MAX_SMALL_OBJ_SIZE)
			location = "major";
		else
			location = "LOS";
		fprintf (heap_dump_file, " location=\"%s\"", location);
	}
	fprintf (heap_dump_file, "/>\n");
#endif
}

static void
dump_object_callback (GCObject *obj, size_t size, gboolean dump_location)
{
	dump_object (obj, dump_location);
}

void
sgen_debug_enable_heap_dump (const char *filename)
{
	heap_dump_file = fopen (filename, "w");
	if (heap_dump_file) {
		fprintf (heap_dump_file, "<sgen-dump>\n");
		sgen_pin_stats_enable ();
	}
}

void
sgen_debug_dump_heap (const char *type, int num, const char *reason)
{
	SgenPointerQueue *pinned_objects;

	if (!heap_dump_file)
		return;

	fprintf (heap_dump_file, "<collection type=\"%s\" num=\"%d\"", type, num);
	if (reason)
		fprintf (heap_dump_file, " reason=\"%s\"", reason);
	fprintf (heap_dump_file, ">\n");
#ifndef SGEN_WITHOUT_MONO
	fprintf (heap_dump_file, "<other-mem-usage type=\"mempools\" size=\"%ld\"/>\n", mono_mempool_get_bytes_allocated ());
#endif
	sgen_dump_internal_mem_usage (heap_dump_file);
	fprintf (heap_dump_file, "<pinned type=\"stack\" bytes=\"%lu\"/>\n", (unsigned long)sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_STACK));
	/* fprintf (heap_dump_file, "<pinned type=\"static-data\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_STATIC_DATA]); */
	fprintf (heap_dump_file, "<pinned type=\"other\" bytes=\"%lu\"/>\n", (unsigned long)sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_OTHER));

	fprintf (heap_dump_file, "<pinned-objects>\n");
	pinned_objects = sgen_pin_stats_get_object_list ();
	for (gsize i = 0; i < pinned_objects->next_slot; ++i)
		dump_object ((GCObject *)pinned_objects->data [i], TRUE);
	fprintf (heap_dump_file, "</pinned-objects>\n");

	sgen_dump_section (sgen_nursery_section, "nursery");

	sgen_major_collector.dump_heap (heap_dump_file);

	fprintf (heap_dump_file, "<los>\n");
	sgen_los_iterate_objects ((IterateObjectCallbackFunc)dump_object_callback, (void*)FALSE);
	fprintf (heap_dump_file, "</los>\n");

	fprintf (heap_dump_file, "</collection>\n");
}

static GCObject *found_obj;

static void
find_object_for_ptr_callback (GCObject *obj, size_t size, void *user_data)
{
	char *ptr = (char *)user_data;

	if (ptr >= (char*)obj && ptr < (char*)obj + size) {
		g_assert (!found_obj);
		found_obj = obj;
	}
}

/* for use in the debugger */
GCObject*
sgen_find_object_for_ptr (char *ptr)
{
	if (ptr >= sgen_nursery_section->data && ptr < sgen_nursery_section->end_data) {
		found_obj = NULL;
		sgen_scan_area_with_callback (sgen_nursery_section->data, sgen_nursery_section->end_data,
				find_object_for_ptr_callback, ptr, TRUE, FALSE);
		if (found_obj)
			return found_obj;
	}

	found_obj = NULL;
	sgen_los_iterate_objects (find_object_for_ptr_callback, ptr);
	if (found_obj)
		return found_obj;

	/*
	 * Very inefficient, but this is debugging code, supposed to
	 * be called from gdb, so we don't care.
	 */
	found_obj = NULL;
	sgen_major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, find_object_for_ptr_callback, ptr);
	return found_obj;
}

#else

void
sgen_check_heap_marked (gboolean nursery_must_be_pinned)
{
}

void
sgen_check_major_refs (void)
{
}

void
sgen_check_mod_union_consistency (void)
{
}

void
sgen_check_nursery_objects_untag (void)
{
}

void
sgen_check_remset_consistency (void)
{
}

void
sgen_check_whole_heap (gboolean allow_missing_pinned)
{
}

void
sgen_debug_check_nursery_is_clean (void)
{
}

void
sgen_debug_dump_heap (const char *type, int num, const char *reason)
{
}

void
sgen_debug_enable_heap_dump (const char *filename)
{
}

void
sgen_debug_verify_nursery (gboolean do_dump_nursery_content)
{
}

void
sgen_dump_occupied (char *start, char *end, char *section_start)
{
}

#endif /*DISABLE_SGEN_DEBUG_HELPERS */
#endif /*HAVE_SGEN_GC*/
