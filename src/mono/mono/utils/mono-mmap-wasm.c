
#include <config.h>

#ifdef HOST_WASM

#include <sys/types.h>
#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "mono-mmap.h"
#include "mono-mmap-internals.h"
#include "mono-proclib.h"
#include <mono/utils/mono-threads.h>
#include <mono/utils/atomic.h>
#include <mono/utils/options.h>

#define BEGIN_CRITICAL_SECTION do { \
	MonoThreadInfo *__info = mono_thread_info_current_unchecked (); \
	if (__info) __info->inside_critical_region = TRUE;	\

#define END_CRITICAL_SECTION \
	if (__info) __info->inside_critical_region = FALSE;	\
} while (0)	\

int
mono_pagesize (void)
{
	return 16384;
}

int
mono_valloc_granule (void)
{
	return mono_pagesize ();
}

static int
prot_from_flags (int flags)
{
#if HOST_WASI
	// The mmap in wasi-sdk rejects PROT_NONE, but otherwise disregards the flags
	// We just need to pass an acceptable value
	return PROT_READ;
#endif

	int prot = PROT_NONE;
	/* translate the protection bits */
	if (flags & MONO_MMAP_READ)
		prot |= PROT_READ;
	if (flags & MONO_MMAP_WRITE)
		prot |= PROT_WRITE;
	if (flags & MONO_MMAP_EXEC)
		prot |= PROT_EXEC;

	return prot;
}

/**
 * mono_setmmapjit:
 * \param flag indicating whether to enable or disable the use of MAP_JIT in mmap
 *
 * Call this method to enable or disable the use of MAP_JIT to create the pages
 * for the JIT to use.   This is only needed for scenarios where Mono is bundled
 * as an App in MacOS
 */
void
mono_setmmapjit (int flag)
{
	/* Ignored on HOST_WASM */
}


void*
mono_valloc (void *addr, size_t length, int flags, MonoMemAccountType type)
{
	g_assert (addr == NULL);
	return mono_valloc_aligned (length, mono_pagesize (), flags, type);
}

/*
 * Aligned blocks that the same request can reuse (memwalk, 2026-11-08; findings log "the dlmalloc memalign ratchet").
 *
 * dlmalloc's memalign mallocs request + alignment + 12 and gives the leading and trailing slack back, so the block it
 * returns frees into a chunk of exactly the request -- which the same memalign can never use again, and the slack it
 * gave back is soon held by small allocations, so the hole cannot coalesce either. Every sgen block goes through here:
 * each LOS object (512-aligned, one allocation each: wasm has no LOS sections), each major-heap arena (32 blocks aligned
 * to the block, 64 KiB on wasm since R528) and every mono_valloc (16 KiB pagesize). Under continuous chunk loading each major freed thousands of large
 * objects into holes no large object could reuse (19,352 free chunks of exactly 16,904 B = a freed int[4096]), and the
 * sbrk break climbed ~35 MB/min with in-use memory flat, up to the 4 GiB wall.
 *
 * With MONO_WASM_VALLOC_SLACK on, an alignment in (16, VALLOC_SLACK_MAX_ALIGN] on a block of at least
 * VALLOC_SLACK_MIN_RATIO x the alignment is served by malloc (size + alignment), aligned inside and kept whole, so it
 * frees into exactly what the next same-size request asks for. The slack (alignment bytes, split before and after
 * the block) is dead space: no other allocation shares the block's cards. mono_vfree is passed no alignment and a
 * header could not be told from dlmalloc's own, so the aligned ->
 * raw mapping lives in a side table (every slack block, so the slack counters are exact); an address not in it
 * (posix_memalign) is freed as is.
 * Larger alignments (the nursery: its own size) and small blocks (16 KiB internal-allocator blocks) keep posix_memalign.
 * Trap: VALLOC_SLACK_MAX_ALIGN must stay >= sgen's major block size, or arenas fall back to posix_memalign and ratchet.
 * The accounts keep counting the requested size; the slack is mono_wasm_valloc_slack_bytes.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#define VALLOC_SLACK_MAX_ALIGN (64 * 1024)
#define VALLOC_SLACK_MIN_RATIO 4
typedef struct { uintptr_t key, raw, extra; } VallocSlot;   /* key 0: empty, 1: deleted */
static VallocSlot *valloc_tab;
static size_t valloc_cap, valloc_used, valloc_dead;
static pthread_mutex_t valloc_lock = PTHREAD_MUTEX_INITIALIZER;
static int valloc_slack = -1;   /* MONO_WASM_VALLOC_SLACK, read once; default 1 */
size_t mono_wasm_valloc_slack_blocks, mono_wasm_valloc_slack_bytes, mono_wasm_valloc_slack_allocs, mono_wasm_valloc_slack_tablefail;

static inline size_t
valloc_hash (uintptr_t k, size_t cap)
{
	return ((k >> 3) * 2654435761u) & (cap - 1);
}

/* Under valloc_lock. Rehashes into a table of the size the live entries need; the old table is freed (nothing reads it
 * outside the lock). */
static gboolean
valloc_tab_grow (void)
{
	size_t ncap = valloc_cap ? valloc_cap : 1024;
	while ((valloc_used + 1) * 2 > ncap)
		ncap *= 2;
	VallocSlot *nt = (VallocSlot *)calloc (ncap, sizeof (VallocSlot));
	if (!nt)
		return FALSE;
	for (size_t i = 0; i < valloc_cap; i++) {
		if (valloc_tab [i].key <= 1)
			continue;
		size_t j = valloc_hash (valloc_tab [i].key, ncap);
		while (nt [j].key)
			j = (j + 1) & (ncap - 1);
		nt [j] = valloc_tab [i];
	}
	free (valloc_tab);
	valloc_tab = nt;
	valloc_cap = ncap;
	valloc_dead = 0;
	return TRUE;
}

static gboolean
valloc_tab_insert (void *addr, void *raw, size_t extra)
{
	gboolean ok = TRUE;
	pthread_mutex_lock (&valloc_lock);
	if ((valloc_used + valloc_dead + 1) * 10 > valloc_cap * 6)
		ok = valloc_tab_grow ();
	if (ok) {
		size_t j = valloc_hash ((uintptr_t)addr, valloc_cap);
		while (valloc_tab [j].key > 1)
			j = (j + 1) & (valloc_cap - 1);
		if (valloc_tab [j].key == 1)
			valloc_dead--;
		valloc_tab [j].key = (uintptr_t)addr;
		valloc_tab [j].raw = (uintptr_t)raw;
		valloc_tab [j].extra = extra;
		valloc_used++;
		mono_wasm_valloc_slack_allocs++;
		mono_wasm_valloc_slack_blocks++;
		mono_wasm_valloc_slack_bytes += extra;
	} else {
		mono_wasm_valloc_slack_tablefail++;
	}
	pthread_mutex_unlock (&valloc_lock);
	return ok;
}

/* The raw pointer recorded for ADDR, removing it; NULL when ADDR was not served by the slack path. */
static void *
valloc_tab_remove (void *addr)
{
	void *raw = NULL;
	pthread_mutex_lock (&valloc_lock);
	if (valloc_cap) {
		size_t j = valloc_hash ((uintptr_t)addr, valloc_cap);
		while (valloc_tab [j].key) {
			if (valloc_tab [j].key == (uintptr_t)addr) {
				raw = (void *)valloc_tab [j].raw;
				mono_wasm_valloc_slack_blocks--;
				mono_wasm_valloc_slack_bytes -= valloc_tab [j].extra;
				valloc_tab [j].key = 1;
				valloc_used--;
				valloc_dead++;
				break;
			}
			j = (j + 1) & (valloc_cap - 1);
		}
	}
	pthread_mutex_unlock (&valloc_lock);
	return raw;
}

void*
mono_valloc_aligned (size_t size, size_t alignment, int flags, MonoMemAccountType type)
{
#ifdef DISABLE_THREADS
	void *old_sbrk = NULL;
	if ((flags & MONO_MMAP_NOZERO) == 0) {
		old_sbrk = sbrk (0);
	}
#endif

	void *res = NULL;
	if (valloc_slack < 0) {
		const char *e = getenv ("MONO_WASM_VALLOC_SLACK");
		valloc_slack = e ? atoi (e) : 1;
		fprintf (stderr, "[wasm-valloc] slack=%d (aligned blocks served by malloc + alignment up to %d B alignment)\n", valloc_slack, VALLOC_SLACK_MAX_ALIGN);
	}
	if (valloc_slack && alignment > 16 && alignment <= VALLOC_SLACK_MAX_ALIGN && size / VALLOC_SLACK_MIN_RATIO >= alignment
	    && size <= SIZE_MAX - alignment) {
		void *raw = malloc (size + alignment);
		if (!raw)
			return NULL;
		res = (void *)(((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1));
		if (!valloc_tab_insert (res, raw, alignment)) {
			free (raw);
			return NULL;
		}
	} else if (posix_memalign (&res, alignment, size))
		return NULL;

#ifdef DISABLE_THREADS
	if ((flags & MONO_MMAP_NOZERO) == 0 && old_sbrk > res) {
		// this means that we got an old block, not the new block from sbrk
		memset (res, 0, size);
	}
#else
	if ((flags & MONO_MMAP_NOZERO) == 0) {
		memset (res, 0, size);
	}
#endif

	mono_account_mem (type, (ssize_t)size);

	return res;
}

int
mono_vfree (void *addr, size_t length, MonoMemAccountType type)
{
	// NOTE: this doesn't implement partial freeing like munmap does
	// (sgen's major heap frees only whole arenas on wasm, R502)
	void *raw = valloc_cap ? valloc_tab_remove (addr) : NULL;
	g_free (raw ? raw : addr);

	mono_account_mem (type, -(ssize_t)length);

	return 0;
}

void*
mono_file_map (size_t length, int flags, int fd, guint64 offset, void **ret_handle)
{
	void *ptr;
	int mflags = 0;
	int prot = prot_from_flags (flags);
	/* translate the flags */
	if (flags & MONO_MMAP_PRIVATE)
		mflags |= MAP_PRIVATE;
	if (flags & MONO_MMAP_SHARED)
		mflags |= MAP_SHARED;
	if (flags & MONO_MMAP_FIXED)
		mflags |= MAP_FIXED;

	if (length == 0)
		/* emscripten throws an exception on 0 length */
		return NULL;

	BEGIN_CRITICAL_SECTION;
	ptr = mmap (0, length, prot, mflags, fd, offset);
	END_CRITICAL_SECTION;
	if (ptr == MAP_FAILED)
		return NULL;
	*ret_handle = (void*)length;
	return ptr;
}

void*
mono_file_map_error (size_t length, int flags, int fd, guint64 offset, void **ret_handle,
	const char *filepath, char **error_message)
{
	return mono_file_map (length, flags, fd, offset, ret_handle);
}

int
mono_file_unmap (void *addr, void *handle)
{
	int res;

	BEGIN_CRITICAL_SECTION;
	res = munmap (addr, (size_t)handle);
	END_CRITICAL_SECTION;

	return res;
}

int
mono_mprotect (void *addr, size_t length, int flags)
{
	if (flags & MONO_MMAP_DISCARD) {
		memset (addr, 0, length);
	}
	return 0;
}

#endif
