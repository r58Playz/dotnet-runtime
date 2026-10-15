/*
 * gmem.c: memory utility functions
 *
 * Author:
 * 	Gonzalo Paniagua Javier (gonzalo@novell.com)
 *
 * (C) 2006 Novell, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <config.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <eglib-remap.h> // Remove the cast macros and restore the rename macros.
#undef malloc
#undef realloc
#undef free
#undef calloc

#if defined (ENABLE_OVERRIDABLE_ALLOCATORS)

static GMemVTable sGMemVTable = { malloc, realloc, free, calloc };

void
g_mem_set_vtable (GMemVTable* vtable)
{
	sGMemVTable.calloc = vtable->calloc ? vtable->calloc : calloc;
	sGMemVTable.realloc = vtable->realloc ? vtable->realloc : realloc;
	sGMemVTable.malloc = vtable->malloc ? vtable->malloc : malloc;
	sGMemVTable.free = vtable->free ? vtable->free : free;
}

void
g_mem_get_vtable (GMemVTable* vtable)
{
	*vtable = sGMemVTable;
}

#define G_FREE_INTERNAL sGMemVTable.free
#define G_REALLOC_INTERNAL sGMemVTable.realloc
#define G_CALLOC_INTERNAL sGMemVTable.calloc
#define G_MALLOC_INTERNAL sGMemVTable.malloc
#else

void
g_mem_get_vtable (GMemVTable* vtable)
{
	memset (vtable, 0, sizeof (*vtable));
}

void
g_mem_set_vtable (GMemVTable* vtable)
{
}

#define G_FREE_INTERNAL free
#define G_REALLOC_INTERNAL realloc
#define G_CALLOC_INTERNAL calloc
#define G_MALLOC_INTERNAL malloc
#endif

/* MONO_FREE_QUARANTINE_MB (browser only, DIAGNOSTIC, 0 = off): g_free poisons each freed block (0xA5C3E1F7) and
 * holds it back from reuse until that many MB of later frees have pushed it out; a growing g_realloc/g_try_realloc
 * is malloc + copy + a quarantined free of the old block. On the way out every word is checked, and a block written
 * after it was freed prints FREE_QUARANTINE_HIT: size, freed/now seq and ms, the freeing and evicting pthreads, the
 * words that changed, and the block's first 8 words from BEFORE the free (they name what the object was). Then
 * g_mem_quarantine_hit_hook runs -- the wasm JIT points it at its owner report and id counters.
 * Found R489 (a JIT store past the end of a per-thread array) from the VALUES the hits carried. A stale READ of a
 * quarantined block sees poison, so expect asserts on garbage classes or pointers near 0xA5C3E1F7; it costs memory
 * (256 MB held plus the copies). sgen's OS memory is freed through g_free too (mono_vfree, mono-mmap-wasm.c).
 * Enabled by g_mem_quarantine_init (), which the wasm JIT calls once the environment is in place. */
#ifdef HOST_BROWSER
#include <malloc.h>
#include <pthread.h>
#include <emscripten/emscripten.h>
#define GQ_POISON 0xA5C3E1F7u
#define GQ_RING (1 << 20)
#define GQ_PRE 8   /* words of the block kept from BEFORE the poison, so a hit names what the block was */
typedef struct { void *p; size_t size; unsigned seq; unsigned tid; unsigned tms; unsigned pre [GQ_PRE]; } GQEnt;
static double gq_t0;
/* Set by the wasm JIT (mini-wasm.c): after a hit is printed, name the block's recorded previous owners -- pthread stacks
 * and TLS blocks (live or dead) and the JIT's own frees -- as WASM_JIT_BYTES_OWNER lines. */
void (*g_mem_quarantine_hit_hook) (void *p, size_t size);
static GQEnt *gq_ring;
static int gq_on, gq_head, gq_count, gq_reports;
static size_t gq_cap, gq_bytes;
static unsigned gq_seq;
static unsigned long long gq_checked_bytes, gq_hits_blocks;
static pthread_mutex_t gq_lock = PTHREAD_MUTEX_INITIALIZER;

void
g_mem_quarantine_init (void)
{
	const char *v = getenv ("MONO_FREE_QUARANTINE_MB");
	int mb = v && *v ? atoi (v) : 0;
	if (mb <= 0 || gq_on)
		return;
	gq_ring = (GQEnt *) G_CALLOC_INTERNAL (GQ_RING, sizeof (GQEnt));
	if (!gq_ring)
		return;
	gq_cap = (size_t) mb << 20;
	gq_t0 = emscripten_get_now ();
	__atomic_store_n (&gq_on, 1, __ATOMIC_RELEASE);
	printf ("FREE_QUARANTINE on: cap %d MB, ring %d blocks, poison %08x\n", mb, GQ_RING, GQ_POISON);
}

/* Verify a block leaving quarantine; report (outside the lock, by the caller) through buf. Returns TRUE on a hit. */
static gboolean
gq_verify (const GQEnt *e, char *buf, size_t bufsz)
{
	const unsigned *w = (const unsigned *) e->p;
	size_t nw = e->size / 4, i, first = (size_t) -1, last = 0, nbad = 0;
	int n = 0;
	gq_checked_bytes += e->size;
	for (i = 0; i < nw; ++i) {
		if (w [i] != GQ_POISON) {
			if (first == (size_t) -1)
				first = i;
			last = i;
			nbad++;
		}
	}
	if (!nbad)
		return FALSE;
	n = snprintf (buf, bufsz, "FREE_QUARANTINE_HIT ptr=%p size=%zu freed_seq=%u now_seq=%u freed_ms=%u now_ms=%u freer=%x evictor=%x words_changed=%zu span=[%zu,%zu] :",
		e->p, e->size, e->seq, gq_seq, e->tms, (unsigned) (emscripten_get_now () - gq_t0), e->tid, (unsigned) (gsize) pthread_self (),
		nbad, first * 4, last * 4 + 4);
	for (i = first; i <= last && n < (int) bufsz - 200; ++i)
		if (w [i] != GQ_POISON)
			n += snprintf (buf + n, bufsz - n, " +%zu=%08x", i * 4, w [i]);
	n += snprintf (buf + n, bufsz - n, " | before free:");
	for (i = 0; i < GQ_PRE && i < nw && n < (int) bufsz - 12; ++i)
		n += snprintf (buf + n, bufsz - n, " %08x", e->pre [i]);
	return TRUE;
}

static void
gq_free (void *ptr)
{
	size_t size = malloc_usable_size (ptr);
	GQEnt out [8];
	unsigned pre [GQ_PRE];
	int nout = 0, i;
	if (size < 4) {
		G_FREE_INTERNAL (ptr);
		return;
	}
	memset (pre, 0, sizeof (pre));
	memcpy (pre, ptr, size < sizeof (pre) ? size : sizeof (pre));
	for (i = 0; i < (int) (size / 4); ++i)
		((unsigned *) ptr) [i] = GQ_POISON;
	pthread_mutex_lock (&gq_lock);
	while (nout < 8 && gq_count > 0 && (gq_count == GQ_RING || gq_bytes + size > gq_cap)) {
		out [nout++] = gq_ring [gq_head];
		gq_bytes -= gq_ring [gq_head].size;
		gq_head = (gq_head + 1) & (GQ_RING - 1);
		gq_count--;
	}
	if (gq_count < GQ_RING) {
		GQEnt *e = &gq_ring [(gq_head + gq_count) & (GQ_RING - 1)];
		e->p = ptr; e->size = size; e->seq = ++gq_seq;
		e->tid = (unsigned) (gsize) pthread_self ();
		e->tms = (unsigned) (emscripten_get_now () - gq_t0);
		memcpy (e->pre, pre, sizeof (pre));
		gq_count++;
		gq_bytes += size;
		ptr = NULL;
	}
	pthread_mutex_unlock (&gq_lock);
	for (i = 0; i < nout; ++i) {
		char buf [1024];
		if (gq_verify (&out [i], buf, sizeof (buf))) {
			gq_hits_blocks++;
			if (__atomic_add_fetch (&gq_reports, 1, __ATOMIC_RELAXED) <= 200) {
				printf ("%s\n", buf);
				if (g_mem_quarantine_hit_hook)
					g_mem_quarantine_hit_hook (out [i].p, out [i].size);
			}
		}
		G_FREE_INTERNAL (out [i].p);
	}
	if (ptr)
		G_FREE_INTERNAL (ptr);
}
#else
void
g_mem_quarantine_init (void)
{
}
#endif

void
g_free (void *ptr)
{
	if (ptr != NULL) {
#ifdef HOST_BROWSER
		if (G_UNLIKELY (__atomic_load_n (&gq_on, __ATOMIC_ACQUIRE))) {
			gq_free (ptr);
			return;
		}
#endif
		G_FREE_INTERNAL (ptr);
	}
}

gpointer
g_memdup (gconstpointer mem, guint byte_size)
{
	gpointer ptr;

	if (mem == NULL)
		return NULL;

	ptr = g_malloc (byte_size);
	if (ptr != NULL)
		memcpy (ptr, mem, byte_size);

	return ptr;
}

#ifdef HOST_BROWSER
/* With the quarantine on, a growing realloc is malloc + copy + a quarantined free of the old block: realloc frees the
 * old block itself, and the R489 run-2 damage landed in exactly such a block (a vcall PIC's pre-growth array). */
static gpointer
gq_realloc (gpointer obj, gsize size)
{
	size_t old = malloc_usable_size (obj);
	gpointer ptr;
	if (size <= old)
		return obj;
	ptr = G_MALLOC_INTERNAL (size);
	if (ptr) {
		memcpy (ptr, obj, old);
		gq_free (obj);
	}
	return ptr;
}
#endif

gpointer g_realloc (gpointer obj, gsize size)
{
	gpointer ptr;
	if (!size) {
		g_free (obj);
		return 0;
	}
#ifdef HOST_BROWSER
	if (G_UNLIKELY (obj && __atomic_load_n (&gq_on, __ATOMIC_ACQUIRE)))
		ptr = gq_realloc (obj, size);
	else
#endif
	ptr = G_REALLOC_INTERNAL (obj, size);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i bytes", size);
}

gpointer
g_malloc (gsize x)
{
	gpointer ptr;
	if (!x)
		return 0;
	ptr = G_MALLOC_INTERNAL (x);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i bytes", x);
}

gpointer g_calloc (gsize n, gsize x)
{
	gpointer ptr;
	if (!x || !n)
		return 0;
	ptr = G_CALLOC_INTERNAL (n, x);
	if (ptr)
		return ptr;
	g_error ("Could not allocate %i (%i * %i) bytes", x*n, n, x);
}
gpointer g_malloc0 (gsize x)
{
	return g_calloc (1,x);
}

gpointer g_try_malloc (gsize x)
{
	if (x)
		return G_MALLOC_INTERNAL (x);
	return 0;
}


gpointer g_try_realloc (gpointer obj, gsize size)
{
	if (!size) {
		G_FREE_INTERNAL (obj);
		return 0;
	}
#ifdef HOST_BROWSER
	if (G_UNLIKELY (obj && __atomic_load_n (&gq_on, __ATOMIC_ACQUIRE)))
		return gq_realloc (obj, size);
#endif
	return G_REALLOC_INTERNAL (obj, size);
}
