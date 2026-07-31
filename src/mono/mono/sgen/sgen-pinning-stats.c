/**
 * \file
 * Copyright 2001-2003 Ximian, Inc
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#ifndef DISABLE_SGEN_DEBUG_HELPERS

#include <string.h>

#include "mono/sgen/sgen-gc.h"
#include "mono/sgen/sgen-pinning.h"
#include "mono/sgen/sgen-hash-table.h"
#include "mono/sgen/sgen-client.h"

typedef struct _PinStatAddress PinStatAddress;
struct _PinStatAddress {
	char *addr;
	int pin_types;
	PinStatAddress *left;
	PinStatAddress *right;
};

typedef struct {
	size_t num_pins [PIN_TYPE_MAX];
} PinnedClassEntry;

typedef struct {
	gulong num_remsets;
} GlobalRemsetClassEntry;

static gboolean do_pin_stats = FALSE;

static PinStatAddress *pin_stat_addresses = NULL;
static size_t pinned_byte_counts [PIN_TYPE_MAX];

static size_t pinned_bytes_in_generation [GENERATION_MAX];
static int pinned_objects_in_generation [GENERATION_MAX];

static SgenPointerQueue pinned_objects = SGEN_POINTER_QUEUE_INIT (INTERNAL_MEM_STATISTICS);

/*
 * Keyed on the vtable pointer, not on a "namespace.name" string. These tables are updated
 * once per pinned object with the world stopped, and building a name there would call
 * g_strdup_printf -> libc malloc, whose lock a suspended mutator may already hold. Names
 * are resolved in sgen_pin_stats_flush_report(), after the world restarts.
 */
static SgenHashTable pinned_class_hash_table = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_STATISTICS, INTERNAL_MEM_STAT_PINNED_CLASS, sizeof (PinnedClassEntry), g_direct_hash, g_direct_equal);
static SgenHashTable global_remset_class_hash_table = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_STATISTICS, INTERNAL_MEM_STAT_REMSET_CLASS, sizeof (GlobalRemsetClassEntry), g_direct_hash, g_direct_equal);

/*
 * Pinning histogram for the collection that just finished. sgen_pin_stats_report() runs
 * inside the pause and may only touch this fixed buffer -- no allocation, no stdio, since
 * both take locks a suspended thread can hold. sgen_pin_stats_flush_report() then formats
 * it once the mutators are live again, mirroring how SgenLogEntry defers GC_MINOR/GC_MAJOR.
 */
#define PIN_SNAPSHOT_MAX_CLASSES 1024

typedef struct {
	GCVTable vtable;
	size_t num_pins [PIN_TYPE_MAX];
} PinSnapshotEntry;

typedef struct {
	GCVTable vtable;
	gulong num_remsets;
} RemsetSnapshotEntry;

static PinSnapshotEntry pin_snapshot [PIN_SNAPSHOT_MAX_CLASSES];
static RemsetSnapshotEntry remset_snapshot [PIN_SNAPSHOT_MAX_CLASSES];
static int pin_snapshot_count, pin_snapshot_dropped;
static int remset_snapshot_count, remset_snapshot_dropped;
static size_t pin_snapshot_byte_counts [PIN_TYPE_MAX];
static int pin_snapshot_objects [GENERATION_MAX];
static gboolean pin_snapshot_pending;

void
sgen_pin_stats_enable (void)
{
	do_pin_stats = TRUE;
}

static void
pin_stats_tree_free (PinStatAddress *node)
{
	if (!node)
		return;
	pin_stats_tree_free (node->left);
	pin_stats_tree_free (node->right);
	sgen_free_internal_dynamic (node, sizeof (PinStatAddress), INTERNAL_MEM_STATISTICS);
}

void
sgen_pin_stats_reset (void)
{
	int i;
	pin_stats_tree_free (pin_stat_addresses);
	pin_stat_addresses = NULL;
	for (i = 0; i < PIN_TYPE_MAX; ++i)
		pinned_byte_counts [i] = 0;
	for (i = 0; i < GENERATION_MAX; ++i) {
		pinned_bytes_in_generation [i] = 0;
		pinned_objects_in_generation [i] = 0;
	}
	sgen_pointer_queue_clear (&pinned_objects);
	sgen_hash_table_clean (&pinned_class_hash_table);
	sgen_hash_table_clean (&global_remset_class_hash_table);
}

void
sgen_pin_stats_register_address (char *addr, int pin_type)
{
	PinStatAddress **node_ptr = &pin_stat_addresses;
	PinStatAddress *node;
	int pin_type_bit = 1 << pin_type;

	if (!do_pin_stats)
		return;
	while (*node_ptr) {
		node = *node_ptr;
		if (addr == node->addr) {
			node->pin_types |= pin_type_bit;
			return;
		}
		if (addr < node->addr)
			node_ptr = &node->left;
		else
			node_ptr = &node->right;
	}

	node = (PinStatAddress *)sgen_alloc_internal_dynamic (sizeof (PinStatAddress), INTERNAL_MEM_STATISTICS, TRUE);
	node->addr = addr;
	node->pin_types = pin_type_bit;
	node->left = node->right = NULL;

	*node_ptr = node;
}

static void
pin_stats_count_object_from_tree (GCObject *object, size_t size, PinStatAddress *node, int *pin_types)
{
	char *obj = (char*)object;
	if (!node)
		return;
	if (node->addr >= obj && node->addr < obj + size) {
		int i;
		for (i = 0; i < PIN_TYPE_MAX; ++i) {
			int pin_bit = 1 << i;
			if (!(*pin_types & pin_bit) && (node->pin_types & pin_bit)) {
				pinned_byte_counts [i] += size;
				*pin_types |= pin_bit;
			}
		}
	}
	if (obj < node->addr)
		pin_stats_count_object_from_tree (object, size, node->left, pin_types);
	if (obj + size - 1 > node->addr)
		pin_stats_count_object_from_tree (object, size, node->right, pin_types);
}

static gpointer
lookup_vtable_entry (SgenHashTable *hash_table, GCVTable vtable, gpointer empty_entry)
{
	gpointer entry = sgen_hash_table_lookup (hash_table, vtable);

	if (!entry) {
		sgen_hash_table_replace (hash_table, vtable, empty_entry, NULL);
		entry = sgen_hash_table_lookup (hash_table, vtable);
	}

	return entry;
}

static void
register_vtable (GCVTable vtable, int pin_types)
{
	PinnedClassEntry empty_entry;
	PinnedClassEntry *entry;
	int i;

	memset (&empty_entry, 0, sizeof (PinnedClassEntry));
	entry = (PinnedClassEntry *)lookup_vtable_entry (&pinned_class_hash_table, vtable, &empty_entry);

	for (i = 0; i < PIN_TYPE_MAX; ++i) {
		if (pin_types & (1 << i))
			++entry->num_pins [i];
	}
}

void
sgen_pin_stats_register_object (GCObject *obj, int generation)
{
	int pin_types = 0;
	size_t size = 0;

	/* Also needed by GC_PIN_STATS, which otherwise reports zero when print-pinning
	 * is used without binary-protocol. */
	if (sgen_binary_protocol_is_enabled () || do_pin_stats) {
		size = sgen_safe_object_get_size (obj);
		pinned_bytes_in_generation [generation] += size;
		++pinned_objects_in_generation [generation];
	}

	if (!do_pin_stats)
		return;

	if (!size)
		size = sgen_safe_object_get_size (obj);

	pin_stats_count_object_from_tree (obj, size, pin_stat_addresses, &pin_types);
	sgen_pointer_queue_add (&pinned_objects, obj);

	if (pin_types)
		register_vtable (SGEN_LOAD_VTABLE (obj), pin_types);
}

void
sgen_pin_stats_register_global_remset (GCObject *obj)
{
	GlobalRemsetClassEntry empty_entry;
	GlobalRemsetClassEntry *entry;

	if (!do_pin_stats)
		return;

	memset (&empty_entry, 0, sizeof (GlobalRemsetClassEntry));
	entry = (GlobalRemsetClassEntry *)lookup_vtable_entry (&global_remset_class_hash_table, SGEN_LOAD_VTABLE (obj), &empty_entry);

	++entry->num_remsets;
}

/* Runs with the world stopped: copy only, never allocate or log. */
void
sgen_pin_stats_report (void)
{
	GCVTable vtable;
	PinnedClassEntry *pinned_entry;
	GlobalRemsetClassEntry *remset_entry;
	int i;

	sgen_binary_protocol_pin_stats (pinned_objects_in_generation [GENERATION_NURSERY], pinned_bytes_in_generation [GENERATION_NURSERY],
			pinned_objects_in_generation [GENERATION_OLD], pinned_bytes_in_generation [GENERATION_OLD]);

	if (!do_pin_stats)
		return;

	pin_snapshot_count = pin_snapshot_dropped = 0;
	SGEN_HASH_TABLE_FOREACH (&pinned_class_hash_table, GCVTable, vtable, PinnedClassEntry *, pinned_entry) {
		if (pin_snapshot_count < PIN_SNAPSHOT_MAX_CLASSES) {
			PinSnapshotEntry *dst = &pin_snapshot [pin_snapshot_count++];
			dst->vtable = vtable;
			for (i = 0; i < PIN_TYPE_MAX; ++i)
				dst->num_pins [i] = pinned_entry->num_pins [i];
		} else {
			++pin_snapshot_dropped;
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	remset_snapshot_count = remset_snapshot_dropped = 0;
	SGEN_HASH_TABLE_FOREACH (&global_remset_class_hash_table, GCVTable, vtable, GlobalRemsetClassEntry *, remset_entry) {
		if (remset_snapshot_count < PIN_SNAPSHOT_MAX_CLASSES) {
			RemsetSnapshotEntry *dst = &remset_snapshot [remset_snapshot_count++];
			dst->vtable = vtable;
			dst->num_remsets = remset_entry->num_remsets;
		} else {
			++remset_snapshot_dropped;
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	for (i = 0; i < PIN_TYPE_MAX; ++i)
		pin_snapshot_byte_counts [i] = pinned_byte_counts [i];
	for (i = 0; i < GENERATION_MAX; ++i)
		pin_snapshot_objects [i] = pinned_objects_in_generation [i];

	pin_snapshot_pending = TRUE;
}

static size_t
pin_snapshot_total (PinSnapshotEntry *e)
{
	size_t total = 0;
	int i;
	for (i = 0; i < PIN_TYPE_MAX; ++i)
		total += e->num_pins [i];
	return total;
}

/*
 * Runs after sgen_client_restart_world(), so resolving class names and calling mono_trace
 * (both of which allocate) is safe here. Emitted under the `gc` trace mask so it lands in
 * the same stream as GC_MINOR/GC_MAJOR.
 */
void
sgen_pin_stats_flush_report (void)
{
	int i, j;

	if (!pin_snapshot_pending)
		return;
	pin_snapshot_pending = FALSE;

	/* Selection sort in place, worst pinner first -- bounded by PIN_SNAPSHOT_MAX_CLASSES. */
	for (i = 0; i < pin_snapshot_count; ++i) {
		int best = i;
		for (j = i + 1; j < pin_snapshot_count; ++j) {
			if (pin_snapshot_total (&pin_snapshot [j]) > pin_snapshot_total (&pin_snapshot [best]))
				best = j;
		}
		if (best != i) {
			PinSnapshotEntry tmp = pin_snapshot [i];
			pin_snapshot [i] = pin_snapshot [best];
			pin_snapshot [best] = tmp;
		}
	}

	mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_GC,
			"GC_PIN_STATS: %d classes%s, %d objects pinned in nursery, %d in major; bytes pinned stack: %ld static: %ld other: %ld",
			pin_snapshot_count, pin_snapshot_dropped ? " (truncated)" : "",
			pin_snapshot_objects [GENERATION_NURSERY], pin_snapshot_objects [GENERATION_OLD],
			(long)pin_snapshot_byte_counts [PIN_TYPE_STACK],
			(long)pin_snapshot_byte_counts [PIN_TYPE_STATIC_DATA],
			(long)pin_snapshot_byte_counts [PIN_TYPE_OTHER]);

	for (i = 0; i < pin_snapshot_count; ++i) {
		PinSnapshotEntry *e = &pin_snapshot [i];
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_GC, "GC_PIN_CLASS: %6ld  stack:%-6ld static:%-6ld other:%-6ld  %s.%s",
				(long)pin_snapshot_total (e),
				(long)e->num_pins [PIN_TYPE_STACK],
				(long)e->num_pins [PIN_TYPE_STATIC_DATA],
				(long)e->num_pins [PIN_TYPE_OTHER],
				sgen_client_vtable_get_namespace (e->vtable),
				sgen_client_vtable_get_name (e->vtable));
	}

	for (i = 0; i < remset_snapshot_count; ++i) {
		RemsetSnapshotEntry *e = &remset_snapshot [i];
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_GC, "GC_REMSET_CLASS: %6ld  %s.%s",
				(long)e->num_remsets,
				sgen_client_vtable_get_namespace (e->vtable),
				sgen_client_vtable_get_name (e->vtable));
	}
}

size_t
sgen_pin_stats_get_pinned_byte_count (int pin_type)
{
	return pinned_byte_counts [pin_type];
}

SgenPointerQueue*
sgen_pin_stats_get_object_list (void)
{
	return &pinned_objects;
}

#endif

#endif /* HAVE_SGEN_GC */
