/**
 * \file
 * Object allocation routines + managed allocators
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

/*
 * ######################################################################
 * ########  Object allocation
 * ######################################################################
 * This section of code deals with allocating memory for objects.
 * There are several ways:
 * *) allocate large objects
 * *) allocate normal objects
 * *) fast lock-free allocation
 * *) allocation of pinned objects
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include <string.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-protocol.h"
#include "mono/sgen/sgen-memory-governor.h"
#include "mono/sgen/sgen-client.h"
#include "mono/utils/mono-memory-model.h"
#include "mono/utils/mono-tls-inline.h"

#define ALIGN_UP		SGEN_ALIGN_UP
#define ALLOC_ALIGN		SGEN_ALLOC_ALIGN
#define MAX_SMALL_OBJ_SIZE	SGEN_MAX_SMALL_OBJ_SIZE

#ifdef HEAVY_STATISTICS
static guint64 stat_objects_alloced = 0;
static guint64 stat_bytes_alloced = 0;
static guint64 stat_bytes_alloced_los = 0;
#endif

/* The total number of bytes allocated so far in program execution by all attached threads.
 * This is not constantly synchronized, but only updated on each GC. */
static guint64 bytes_allocated_attached = 0;

/* Total bytes allocated so far in program execution by detached threads */
static guint64 bytes_allocated_detached = 0;

/*
 * Allocation is done from a Thread Local Allocation Buffer (TLAB). TLABs are allocated
 * from nursery fragments.
 * tlab_next is the pointer to the space inside the TLAB where the next object will
 * be allocated.
 * tlab_temp_end is the pointer to the end of the temporary space reserved for
 * the allocation: it allows us to set the scan starts at reasonable intervals.
 * tlab_real_end points to the end of the TLAB.
 */

#define TLAB_START	(__thread_info__->tlab_start)
#define TLAB_NEXT	(__thread_info__->tlab_next)
#define TLAB_TEMP_END	(__thread_info__->tlab_temp_end)
#define TLAB_REAL_END	(__thread_info__->tlab_real_end)

#ifdef HOST_BROWSER
/* The `*p == NULL` assert in sgen_try_alloc_obj_nolock has fired three times here with nothing to read (2026-11-10,
 * 2026-11-14). sgen_wasm_tlab_born records, per thread, the nursery-collection count at which its current TLAB was
 * made; mono_gc_wasm_dirty_alloc_report (sgen-mono.c) prints what the allocation found before the assert aborts. */
static __thread int sgen_wasm_tlab_born_gc = -1;
static __thread char *sgen_wasm_tlab_born_start;
#define SGEN_WASM_TLAB_BORN(start) do { sgen_wasm_tlab_born_gc = sgen_gc_collection_count (GENERATION_NURSERY); sgen_wasm_tlab_born_start = (char *) (start); } while (0)
extern void mono_gc_wasm_alloc_state_report (int raw_state);
#define SGEN_WASM_STATE_BLOCKING 6   /* mono-threads.h STATE_BLOCKING; the states after it are its suspended forms */
extern void mono_gc_wasm_dirty_alloc_report (void **p, size_t size, int path, char *tlab_start, char *tlab_next,
	char *tlab_temp_end, char *tlab_real_end, int born_gc, char *born_start);

/* MONO_WASM_TLAB_VERIFY=1 (a diagnostic; off unless the variable is set to something other than 0): the asserts below test ONE word, the first of
 * an object the C path hands out, so a stray store into TLAB memory is seen only when an object happens to start on it
 * and the JIT's inline bump (OP_WASM_JIT_ALLOC_FAST) never tests anything. With this on, every object the C path hands
 * out is checked over its whole extent (check 11), and the unallocated rest of the thread's TLAB is scanned each time an
 * allocation crosses a scan-start granule or takes the locked path (12, 13) -- which every thread does at least once
 * per SGEN_SCAN_START_SIZE bytes, inline allocations included. The first non-zero word is reported
 * (mono_gc_wasm_dirty_alloc_report) and the run aborted there. */
static int sgen_wasm_tlab_verify = -1;
static void
sgen_wasm_verify_zero (char *from, char *to, int path, char *tlab_start, char *tlab_next, char *tlab_temp_end, char *tlab_real_end)
{
	char *q;
	for (q = from; q + sizeof (gpointer) <= to; q += sizeof (gpointer))
		if (G_UNLIKELY (*(gsize *) q)) {
			mono_gc_wasm_dirty_alloc_report ((void **) q, (size_t) (to - from), path, tlab_start, tlab_next, tlab_temp_end,
				tlab_real_end, sgen_wasm_tlab_born_gc, sgen_wasm_tlab_born_start);
			g_error ("MONO_WASM_TLAB_VERIFY: a non-zero word at %p in nursery memory that was never handed out (check %d)", q, path);
		}
}
/* MONO_WASM_TLAB_VERIFY=2 is the scan's positive control (tests only): the 20,000th object the C path hands out in the
 * process gets a marker word stored 64 bytes past its end, inside the unallocated rest of its TLAB -- a stray store by
 * construction -- and the rest of the TLAB is scanned on the spot (check 14): the run must end there with that word in the report. */
static gint32 sgen_wasm_tlab_verify_n;
#define SGEN_WASM_VERIFY_INIT() do { \
		if (sgen_wasm_tlab_verify < 0) { \
			char *sgen_wasm_v = g_getenv ("MONO_WASM_TLAB_VERIFY"); \
			sgen_wasm_tlab_verify = (sgen_wasm_v && *sgen_wasm_v && *sgen_wasm_v != '0') ? (*sgen_wasm_v == '2' ? 2 : 1) : 0; \
			g_free (sgen_wasm_v); \
		} \
	} while (0)
#define SGEN_WASM_VERIFY(from, to, path) do { \
		if (G_UNLIKELY (sgen_wasm_tlab_verify)) { \
			SGEN_WASM_VERIFY_INIT (); \
			if (sgen_wasm_tlab_verify && (from) && (char *) (from) < (char *) (to)) \
				sgen_wasm_verify_zero ((char *) (from), (char *) (to), (path), TLAB_START, TLAB_NEXT, TLAB_TEMP_END, TLAB_REAL_END); \
		} \
	} while (0)
#define SGEN_WASM_VERIFY_SELFTEST(p, size) do { \
		if (G_UNLIKELY (sgen_wasm_tlab_verify == 2) && mono_atomic_inc_i32 (&sgen_wasm_tlab_verify_n) == 20000 && \
		    TLAB_NEXT && (char *) (p) + (size) + 64 + sizeof (gpointer) <= TLAB_REAL_END && (char *) (p) + (size) >= TLAB_NEXT) { \
			*(gsize *) ((char *) (p) + (size) + 64) = 0x5e1f7e57; \
			printf ("[sgen dirty-alloc] MONO_WASM_TLAB_VERIFY=2: stored the marker 5e1f7e57 at %p, 64 bytes past a new object, in the rest of this thread's TLAB\n", (void *) ((char *) (p) + (size) + 64)); \
			/* scanned at once: the JIT's inline bump could hand the marked word out as a field before the next check */ \
			sgen_wasm_verify_zero ((char *) (p) + (size), TLAB_REAL_END, 14, TLAB_START, TLAB_NEXT, TLAB_TEMP_END, TLAB_REAL_END); \
		} \
	} while (0)
#define SGEN_WASM_DIRTY(p, size, path) do { \
		if (G_UNLIKELY (*(p) != NULL)) \
			mono_gc_wasm_dirty_alloc_report ((p), (size), (path), TLAB_START, TLAB_NEXT, TLAB_TEMP_END, TLAB_REAL_END, \
				sgen_wasm_tlab_born_gc, sgen_wasm_tlab_born_start); \
	} while (0)
#else
#define SGEN_WASM_TLAB_BORN(start) do { } while (0)
#define SGEN_WASM_VERIFY(from, to, path) do { } while (0)
#define SGEN_WASM_DIRTY(p, size, path) do { } while (0)
#define SGEN_WASM_VERIFY_SELFTEST(p, size) do { } while (0)
#endif

static void
increment_thread_allocation_counter (size_t byte_size)
{
	mono_thread_info_current ()->total_bytes_allocated += byte_size;
}

static GCObject*
alloc_degraded (GCVTable vtable, size_t size, gboolean for_mature)
{
	GCObject *p;
	increment_thread_allocation_counter (size);

	if (!for_mature) {
		sgen_client_degraded_allocation ();
		SGEN_ATOMIC_ADD_P (sgen_degraded_mode, size);
		sgen_ensure_free_space (size, GENERATION_OLD);
	} else {
		gboolean forced;
		if (sgen_need_major_collection (size, &forced))
			sgen_perform_collection (size, GENERATION_OLD, "mature allocation failure", !for_mature || forced, TRUE);
	}


	p = sgen_major_collector.alloc_degraded (vtable, size);

	if (!for_mature)
		sgen_binary_protocol_alloc_degraded (p, vtable, size, sgen_client_get_provenance ());

	return p;
}

static void
zero_tlab_if_necessary (void *p, size_t size)
{
	if (sgen_nursery_clear_policy == CLEAR_AT_TLAB_CREATION || sgen_nursery_clear_policy == CLEAR_AT_TLAB_CREATION_DEBUG) {
		memset (p, 0, size);
	} else {
		/*
		 * This function is called for all allocations in
		 * TLABs.  TLABs originate from fragments, which are
		 * initialized to be faux arrays.  The remainder of
		 * the fragments are zeroed out at initialization for
		 * CLEAR_AT_GC, so here we just need to make sure that
		 * the array header is zeroed.  Since we don't know
		 * whether we're called for the start of a fragment or
		 * for somewhere in between, we zero in any case, just
		 * to make sure.
		 */
		sgen_client_zero_array_fill_header (p, size);
	}
}

/*
 * Provide a variant that takes just the vtable for small fixed-size objects.
 * The aligned size is already computed and stored in vt->gc_descr.
 * Note: every SGEN_SCAN_START_SIZE or so we are given the chance to do some special
 * processing. We can keep track of where objects start, for example,
 * so when we scan the thread stacks for pinned objects, we can start
 * a search for the pinned object in SGEN_SCAN_START_SIZE chunks.
 */
GCObject*
sgen_alloc_obj_nolock (GCVTable vtable, size_t size)
{
	/* FIXME: handle OOM */
	void **p;
	char *new_next;
	size_t real_size = size;
	TLAB_ACCESS_INIT;

	CANARIFY_SIZE(size);

	HEAVY_STAT (++stat_objects_alloced);
	if (real_size <= SGEN_MAX_SMALL_OBJ_SIZE)
		HEAVY_STAT (stat_bytes_alloced += size);
	else
		HEAVY_STAT (stat_bytes_alloced_los += size);

	size = ALIGN_UP (size);

	SGEN_ASSERT (6, sgen_vtable_get_descriptor (vtable), "VTable without descriptor");

	if (G_UNLIKELY (sgen_has_per_allocation_action)) {
		static int alloc_count;
		int current_alloc = mono_atomic_inc_i32 (&alloc_count);

		if (sgen_collect_before_allocs) {
			if (((current_alloc % sgen_collect_before_allocs) == 0) && sgen_nursery_section) {
				sgen_perform_collection (0, GENERATION_NURSERY, "collect-before-alloc-triggered", TRUE, TRUE);
				if (!sgen_degraded_mode && sgen_can_alloc_size (size) && real_size <= SGEN_MAX_SMALL_OBJ_SIZE) {
					// FIXME:
					g_assert_not_reached ();
				}
			}
		} else if (sgen_verify_before_allocs) {
			if ((current_alloc % sgen_verify_before_allocs) == 0)
				sgen_check_whole_heap_stw ();
		}
	}

	/*
	 * We must already have the lock here instead of after the
	 * fast path because we might be interrupted in the fast path
	 * (after confirming that new_next < TLAB_TEMP_END) by the GC,
	 * and we'll end up allocating an object in a fragment which
	 * no longer belongs to us.
	 *
	 * The managed allocator does not do this, but it's treated
	 * specially by the world-stopping code.
	 */

	if (real_size > SGEN_MAX_SMALL_OBJ_SIZE) {
		p = (void **)sgen_los_alloc_large_inner (vtable, ALIGN_UP (real_size));
		if (p) {
			increment_thread_allocation_counter (size);
		}
	} else {
		/* tlab_next and tlab_temp_end are TLS vars so accessing them might be expensive */

		SGEN_WASM_VERIFY (TLAB_NEXT, TLAB_REAL_END, 13);
		p = (void**)TLAB_NEXT;
		/* FIXME: handle overflow */
		new_next = (char*)p + size;
		TLAB_NEXT = new_next;

		if (G_LIKELY (new_next < TLAB_TEMP_END)) {
			/* Fast path */

			CANARIFY_ALLOC(p,real_size);
			SGEN_LOG (6, "Allocated object %p, vtable: %p (%s), size: %" G_GSIZE_FORMAT "d", p, vtable, sgen_client_vtable_get_name (vtable), size);
			sgen_binary_protocol_alloc (p , vtable, size, sgen_client_get_provenance ());
			SGEN_WASM_DIRTY (p, size, 5);
			g_assert (*p == NULL);
			mono_atomic_store_seq (p, vtable);

			return (GCObject*)p;
		}

		/* Slow path */

		/* there are two cases: the object is too big or we run out of space in the TLAB */
		/* we also reach here when the thread does its first allocation after a minor
		 * collection, since the tlab_ variables are initialized to NULL.
		 * there can be another case (from ORP), if we cooperate with the runtime a bit:
		 * objects that need finalizers can have the high bit set in their size
		 * so the above check fails and we can readily add the object to the queue.
		 * This avoids taking again the GC lock when registering, but this is moot when
		 * doing thread-local allocation, so it may not be a good idea.
		 */
		if (TLAB_NEXT >= TLAB_REAL_END) {
			int available_in_tlab;
			/*
			 * Run out of space in the TLAB. When this happens, some amount of space
			 * remains in the TLAB, but not enough to satisfy the current allocation
			 * request. We keep the TLAB for future allocations if the remaining
			 * space is above a treshold, and satisfy the allocation directly
			 * from the nursery. Otherwise, we attempt to get a new TLAB from the
			 * nursery and allocate into it.
			 */
			TLAB_NEXT -= size;
			/* when running in degraded mode, we continue allocing that way
			 * for a while, to decrease the number of useless nursery collections.
			 */
			if (sgen_degraded_mode && sgen_degraded_mode < sgen_nursery_size)
				return alloc_degraded (vtable, size, FALSE);

			available_in_tlab = (int)(TLAB_REAL_END - TLAB_NEXT);//We'll never have tlabs > 2Gb
			if (size > sgen_tlab_size || available_in_tlab > SGEN_MAX_NURSERY_WASTE) {
				/* Allocate directly from the nursery */
				p = (void **)sgen_nursery_alloc (size);
				if (!p) {
					/*
					 * We couldn't allocate from the nursery, so we try
					 * collecting.  Even after the collection, we might
					 * still not have enough memory to allocate the
					 * object.  The reason will most likely be that we've
					 * run out of memory, but there is the theoretical
					 * possibility that other threads might have consumed
					 * the freed up memory ahead of us.
					 *
					 * What we do in this case is allocate degraded, i.e.,
					 * from the major heap.
					 *
					 * Ideally we'd like to detect the case of other
					 * threads allocating ahead of us and loop (if we
					 * always loop we will loop endlessly in the case of
					 * OOM).
					 */
					sgen_ensure_free_space (real_size, GENERATION_NURSERY);
					if (!sgen_degraded_mode)
						p = (void **)sgen_nursery_alloc (size);

					if (p)
						increment_thread_allocation_counter (size);

				}
				if (!p)
					return alloc_degraded (vtable, size, TRUE);

				zero_tlab_if_necessary (p, size);
			} else {
				size_t alloc_size = 0;
				if (TLAB_START)
					SGEN_LOG (3, "Retire TLAB: %p-%p [%ld]", TLAB_START, TLAB_REAL_END, (long)(TLAB_REAL_END - TLAB_NEXT - size));
				sgen_nursery_retire_region (p, available_in_tlab);

				p = (void **)sgen_nursery_alloc_range (sgen_tlab_size, size, &alloc_size);
				if (!p) {
					/* See comment above in similar case. */
					sgen_ensure_free_space (sgen_tlab_size, GENERATION_NURSERY);
					if (!sgen_degraded_mode)
						p = (void **)sgen_nursery_alloc_range (sgen_tlab_size, size, &alloc_size);
				}
				if (!p)
					return alloc_degraded (vtable, size, TRUE);

				increment_thread_allocation_counter (TLAB_NEXT - TLAB_START);

				/* Allocate a new TLAB from the current nursery fragment */
				TLAB_START = (char*)p;
				TLAB_NEXT = TLAB_START;
				TLAB_REAL_END = TLAB_START + alloc_size;
				TLAB_TEMP_END = TLAB_START + MIN (SGEN_SCAN_START_SIZE, alloc_size);
				SGEN_WASM_TLAB_BORN (TLAB_START);

				zero_tlab_if_necessary (TLAB_START, alloc_size);

				/* Allocate from the TLAB */
				p = (void **)TLAB_NEXT;
				TLAB_NEXT += size;
				sgen_set_nursery_scan_start ((char*)p);
			}
		} else {
			/* Reached tlab_temp_end */

			/* record the scan start so we can find pinned objects more easily */
			sgen_set_nursery_scan_start ((char*)p);
			/* we just bump tlab_temp_end as well */
			TLAB_TEMP_END = MIN (TLAB_REAL_END, TLAB_NEXT + SGEN_SCAN_START_SIZE);
			SGEN_LOG (5, "Expanding local alloc: %p-%p", TLAB_NEXT, TLAB_TEMP_END);
		}
		CANARIFY_ALLOC(p,real_size);
	}

	if (G_LIKELY (p)) {
		SGEN_LOG (6, "Allocated object %p, vtable: %p (%s), size: %" G_GSIZE_FORMAT "d", p, vtable, sgen_client_vtable_get_name (vtable), size);
		sgen_binary_protocol_alloc (p, vtable, size, sgen_client_get_provenance ());
		mono_atomic_store_seq (p, vtable);
	}

	return (GCObject*)p;
}

GCObject*
sgen_try_alloc_obj_nolock (GCVTable vtable, size_t size)
{
	void **p;
	char *new_next;
	size_t real_size = size;
	int wasm_path G_GNUC_UNUSED = 0;   /* HOST_BROWSER: 1 over-TLAB-size, 2 inside the TLAB, 3 beside it, 4 a new TLAB */
	TLAB_ACCESS_INIT;
#ifdef HOST_BROWSER
	/* A thread the suspend machinery counts as stopped (BLOCKING and beyond: it entered a GC-safe region and has not
	 * left it) must not be here: a collection does not wait for it, so its TLAB can be cleared and its nursery moved
	 * under this very allocation. Counted and named (mono_gc_wasm_alloc_state_report), never fatal. */
	if (G_UNLIKELY (__thread_info__->client_info.info.thread_state.state >= SGEN_WASM_STATE_BLOCKING))
		mono_gc_wasm_alloc_state_report (__thread_info__->client_info.info.thread_state.raw);
#endif

	CANARIFY_SIZE(size);

	size = ALIGN_UP (size);
	SGEN_ASSERT (9, real_size >= SGEN_CLIENT_MINIMUM_OBJECT_SIZE, "Object too small");

	SGEN_ASSERT (6, sgen_vtable_get_descriptor (vtable), "VTable without descriptor");

	if (real_size > SGEN_MAX_SMALL_OBJ_SIZE)
		return NULL;

	if (G_UNLIKELY (size > sgen_tlab_size)) {
		/* Allocate directly from the nursery */

		p = (void **)sgen_nursery_alloc (size);
		if (!p)
			return NULL;

		increment_thread_allocation_counter (size);
		sgen_set_nursery_scan_start ((char*)p);

		/*FIXME we should use weak memory ops here. Should help specially on x86. */
		zero_tlab_if_necessary (p, size);
		wasm_path = 1;
	} else {
		int available_in_tlab;
		char *real_end;
		/* tlab_next and tlab_temp_end are TLS vars so accessing them might be expensive */

		p = (void**)TLAB_NEXT;
		/* FIXME: handle overflow */
		new_next = (char*)p + size;

		real_end = TLAB_REAL_END;
		available_in_tlab = (int)(real_end - (char*)p);//We'll never have tlabs > 2Gb

		if (G_LIKELY (new_next < real_end)) {
			TLAB_NEXT = new_next;
			wasm_path = 2;

			/* Second case, we overflowed temp end */
			if (G_UNLIKELY (new_next >= TLAB_TEMP_END)) {
				SGEN_WASM_VERIFY (p, TLAB_REAL_END, 12);
				sgen_set_nursery_scan_start (new_next);
				/* we just bump tlab_temp_end as well */
				TLAB_TEMP_END = MIN (TLAB_REAL_END, TLAB_NEXT + SGEN_SCAN_START_SIZE);
				SGEN_LOG (5, "Expanding local alloc: %p-%p", TLAB_NEXT, TLAB_TEMP_END);
			}
		} else if (available_in_tlab > SGEN_MAX_NURSERY_WASTE) {
			/* Allocate directly from the nursery */
			p = (void **)sgen_nursery_alloc (size);
			if (!p)
				return NULL;

			increment_thread_allocation_counter (size);
			zero_tlab_if_necessary (p, size);
			wasm_path = 3;
		} else {
			size_t alloc_size = 0;

			sgen_nursery_retire_region (p, available_in_tlab);
			new_next = (char *)sgen_nursery_alloc_range (sgen_tlab_size, size, &alloc_size);
			p = (void**)new_next;
			if (!p)
				return NULL;

			increment_thread_allocation_counter (TLAB_NEXT - TLAB_START);

			TLAB_START = (char*)new_next;
			TLAB_NEXT = new_next + size;
			TLAB_REAL_END = new_next + alloc_size;
			TLAB_TEMP_END = new_next + MIN (SGEN_SCAN_START_SIZE, alloc_size);
			sgen_set_nursery_scan_start ((char*)p);
			SGEN_WASM_TLAB_BORN (new_next);

			zero_tlab_if_necessary (new_next, alloc_size);
			wasm_path = 4;
		}
	}

	HEAVY_STAT (++stat_objects_alloced);
	HEAVY_STAT (stat_bytes_alloced += size);

	CANARIFY_ALLOC(p,real_size);
	SGEN_LOG (6, "Allocated object %p, vtable: %p (%s), size: %" G_GSIZE_FORMAT "d", p, vtable, sgen_client_vtable_get_name (vtable), size);
	sgen_binary_protocol_alloc (p, vtable, size, sgen_client_get_provenance ());
	SGEN_WASM_VERIFY (p, (char *) p + size, 11);
	SGEN_WASM_DIRTY (p, size, wasm_path);
	SGEN_WASM_VERIFY_SELFTEST (p, size);
	g_assert (*p == NULL); /* FIXME disable this in non debug builds */

	mono_atomic_store_seq (p, vtable);

	return (GCObject*)p;
}

GCObject*
sgen_alloc_obj (GCVTable vtable, size_t size)
{
	GCObject *res;
	TLAB_ACCESS_INIT;

	if (!SGEN_CAN_ALIGN_UP (size))
		return NULL;

	if (G_UNLIKELY (sgen_has_per_allocation_action)) {
		static int alloc_count;
		int current_alloc = mono_atomic_inc_i32 (&alloc_count);

		if (sgen_verify_before_allocs) {
			if ((current_alloc % sgen_verify_before_allocs) == 0) {
				LOCK_GC;
				sgen_check_whole_heap_stw ();
				UNLOCK_GC;
			}
		}
		if (sgen_collect_before_allocs) {
			if (((current_alloc % sgen_collect_before_allocs) == 0) && sgen_nursery_section) {
				LOCK_GC;
				sgen_perform_collection (0, GENERATION_NURSERY, "collect-before-alloc-triggered", TRUE, TRUE);
				UNLOCK_GC;
			}
		}
	}

	ENTER_CRITICAL_REGION;
	res = sgen_try_alloc_obj_nolock (vtable, size);
	if (res) {
		EXIT_CRITICAL_REGION;
		return res;
	}
	EXIT_CRITICAL_REGION;

	LOCK_GC;
	res = sgen_alloc_obj_nolock (vtable, size);
	UNLOCK_GC;

	return res;
}

/*
 * To be used for interned strings and possibly MonoThread, reflection handles.
 * We may want to explicitly free these objects.
 */
GCObject*
sgen_alloc_obj_pinned (GCVTable vtable, size_t size)
{
	GCObject *p;


	if (!SGEN_CAN_ALIGN_UP (size))
		return NULL;
	size = ALIGN_UP (size);

	LOCK_GC;

	if (size > SGEN_MAX_SMALL_OBJ_SIZE) {
		/* large objects are always pinned anyway */
		p = (GCObject *)sgen_los_alloc_large_inner (vtable, size);
	} else {
		SGEN_ASSERT (9, sgen_client_vtable_is_inited (vtable), "class %s:%s is not initialized", sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));
		p = sgen_major_collector.alloc_small_pinned_obj (vtable, size, SGEN_VTABLE_HAS_REFERENCES (vtable));
	}
	if (G_LIKELY (p)) {
		SGEN_LOG (6, "Allocated pinned object %p, vtable: %p (%s), size: %" G_GSIZE_FORMAT "d", p, vtable, sgen_client_vtable_get_name (vtable), size);
		increment_thread_allocation_counter (size);
		sgen_binary_protocol_alloc_pinned (p, vtable, size, sgen_client_get_provenance ());
	}
	UNLOCK_GC;
	return p;
}

/*
 * Used to allocate thread objects during attach. Doesn't trigger collections since
 * the thread is not yet attached.
 */
GCObject*
sgen_alloc_obj_mature (GCVTable vtable, size_t size)
{
	GCObject *res;

	if (!SGEN_CAN_ALIGN_UP (size))
		return NULL;
	size = ALIGN_UP (size);

	LOCK_GC;
	res = sgen_major_collector.alloc_degraded (vtable, size);
	UNLOCK_GC;

	if (res) {
		increment_thread_allocation_counter (size);
	}

	return res;
}

/*
 * Clear the thread local TLAB variables for all threads.
 */
void
sgen_clear_tlabs (void)
{
	guint64 total_bytes_allocated_globally = 0;

	FOREACH_THREAD_ALL (info) {
		/* A new TLAB will be allocated when the thread does its first allocation */
		info->total_bytes_allocated += info->tlab_next - info->tlab_start;
		total_bytes_allocated_globally += info->total_bytes_allocated;
		info->tlab_start = NULL;
		info->tlab_next = NULL;
		info->tlab_temp_end = NULL;
		info->tlab_real_end = NULL;
	} FOREACH_THREAD_END

	sgen_set_bytes_allocated_attached (total_bytes_allocated_globally);
}

void sgen_update_allocation_count (void)
{
	guint64 total_bytes_allocated_globally = 0;

	FOREACH_THREAD_ALL (info) {
		total_bytes_allocated_globally += info->tlab_next - info->tlab_start;
		total_bytes_allocated_globally += info->total_bytes_allocated;
	} FOREACH_THREAD_END

	sgen_set_bytes_allocated_attached (total_bytes_allocated_globally);
}

void
sgen_set_bytes_allocated_attached (guint64 bytes)
{
	bytes_allocated_attached = bytes;
}

void
sgen_increment_bytes_allocated_detached (guint64 bytes)
{
	bytes_allocated_detached += bytes;
}

guint64
sgen_get_total_allocated_bytes (MonoBoolean precise)
{
	if (precise) {
		LOCK_GC;
		sgen_stop_world (0, FALSE);

		sgen_update_allocation_count ();

		sgen_restart_world (0, FALSE);
		UNLOCK_GC;
	}

	return bytes_allocated_attached + bytes_allocated_detached;
}


void
sgen_init_allocator (void)
{
#ifdef HEAVY_STATISTICS
	mono_counters_register ("# objects allocated", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_objects_alloced);
	mono_counters_register ("bytes allocated", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_bytes_alloced);
	mono_counters_register ("bytes allocated in LOS", MONO_COUNTER_GC | MONO_COUNTER_ULONG, &stat_bytes_alloced_los);
#endif
}

#endif /*HAVE_SGEN_GC*/
