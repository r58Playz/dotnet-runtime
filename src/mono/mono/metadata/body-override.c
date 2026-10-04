/**
 * \file
 * Run-time replacement of a method's IL body: which header a method has now (see body-override.h).
 *
 * Keyed by MonoMethod*, not by (image, token): an inflated instantiation is its own key (a token key would hit every
 * instantiation), and wrapper/SRE methods have no token. The table is consulted by every header reader
 * (mono_method_get_header_internal, mono_method_metadata_has_header, mono_method_get_header_summary) only while some
 * metadata method has an override in force, so the cost with none is one load.
 */
#include <config.h>
#include <mono/metadata/body-override.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/utils/mono-error-internals.h>
#include <mono/utils/mono-os-mutex.h>
#include <mono/utils/mono-lazy-init.h>
#include <mono/utils/atomic.h>

typedef struct {
	MonoMethodHeader *header;     /* metadata method: the override in force, NULL = its own */
	MonoMethodHeader *original;   /* persistent copy of its own header (metadata), or the wrapper's first header */
	guint32 epoch;
	gpointer detour_ftn;
	MonoMethod *detour_target;
} BodyRec;

volatile gint32 mono_body_override_live;

static mono_lazy_init_t body_init = MONO_LAZY_INIT_STATUS_NOT_INITIALIZED;
static mono_mutex_t body_mx;
static GHashTable *body_tab;   /* MonoMethod* -> BodyRec*; records are never freed (compilations bind to their headers) */

static void
body_initialize (void)
{
	mono_os_mutex_init (&body_mx);
	body_tab = g_hash_table_new (NULL, NULL);
}

static gboolean
is_wrapper_header (MonoMethod *m)
{
	return m->wrapper_type != MONO_WRAPPER_NONE || m->sre_method;
}

MonoMethodHeader *
mono_body_override_header (MonoMethod *method)
{
	MonoMethodHeader *h = NULL;
	BodyRec *r;
	if (G_LIKELY (!mono_body_override_live) || !method)
		return NULL;
	mono_os_mutex_lock (&body_mx);
	r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	if (r)
		h = r->header;
	mono_os_mutex_unlock (&body_mx);
	return h;
}

MonoMethodHeader *
mono_body_override_original (MonoMethod *method)
{
	MonoMethodHeader *h = NULL;
	BodyRec *r;
	if (G_LIKELY (!mono_body_override_live) || !method)
		return NULL;
	mono_os_mutex_lock (&body_mx);
	r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	if (r && r->header)
		h = r->original;
	mono_os_mutex_unlock (&body_mx);
	return h;
}

static BodyRec *
body_rec_ensure_locked (MonoMethod *method)
{
	BodyRec *r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	if (!r) {
		r = g_new0 (BodyRec, 1);
		g_hash_table_insert (body_tab, method, r);
	}
	return r;
}

/* The method's own header, persistent. Called with no override in force for `method` (its first install), so the
 * readers below return its real body. Parsed OUTSIDE body_mx: parsing can load types. */
static MonoMethodHeader *
body_own_header (MonoMethod *method)
{
	ERROR_DECL (error);
	MonoMethodHeader *h;
	if (is_wrapper_header (method))
		return ((MonoMethodWrapper *) method)->header;
	h = mono_method_get_header_checked (method, error);
	if (!h) {
		mono_error_cleanup (error);
		return NULL;
	}
	/* Kept for the process lifetime: compilations bind to it, and a free would leave them dangling. */
	h->is_transient = FALSE;
	return h;
}

MonoMethodHeader *
mono_body_current_header (MonoMethod *method)
{
	BodyRec *r;
	MonoMethodHeader *cur = NULL, *own;
	mono_lazy_initialize (&body_init, body_initialize);
	if (is_wrapper_header (method))
		return ((MonoMethodWrapper *) method)->header;
	mono_os_mutex_lock (&body_mx);
	r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	if (r)
		cur = r->header ? r->header : r->original;
	mono_os_mutex_unlock (&body_mx);
	if (cur)
		return cur;
	own = body_own_header (method);
	if (!own)
		return NULL;
	mono_os_mutex_lock (&body_mx);
	r = body_rec_ensure_locked (method);
	if (!r->original)
		r->original = own;    /* a racing first parse leaks one copy; harmless */
	cur = r->header ? r->header : r->original;
	mono_os_mutex_unlock (&body_mx);
	return cur;
}

guint32
mono_body_override_install (MonoMethod *method, MonoMethodHeader *header, gpointer detour_ftn,
                            MonoMethod *detour_target, MonoMethodHeader **installed)
{
	BodyRec *r;
	guint32 epoch;
	MonoMethodHeader *own = NULL;
	mono_lazy_initialize (&body_init, body_initialize);
	if (header)
		header->is_transient = FALSE;
	/* The first install captures the method's own header (metadata: outside the lock, see body_own_header). */
	mono_os_mutex_lock (&body_mx);
	r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	gboolean need_own = !r || !r->original;
	mono_os_mutex_unlock (&body_mx);
	if (need_own)
		own = body_own_header (method);

	mono_os_mutex_lock (&body_mx);
	r = body_rec_ensure_locked (method);
	if (!r->original)
		r->original = own;
	if (is_wrapper_header (method)) {
		MonoMethodHeader *now = header ? header : r->original;
		mono_memory_barrier ();   /* the header's contents before the pointer that publishes it */
		((MonoMethodWrapper *) method)->header = now;
		if (installed)
			*installed = now;
	} else {
		gboolean was = r->header != NULL;
		mono_memory_barrier ();
		r->header = header;
		if (header && !was)
			mono_atomic_inc_i32 (&mono_body_override_live);
		else if (!header && was)
			mono_atomic_dec_i32 (&mono_body_override_live);
		if (installed)
			*installed = header ? header : r->original;
	}
	r->detour_ftn = header ? detour_ftn : NULL;
	r->detour_target = header ? detour_target : NULL;
	epoch = ++r->epoch;
	mono_os_mutex_unlock (&body_mx);
	return epoch;
}

guint32
mono_body_override_info (MonoMethod *method, gpointer *detour_ftn, MonoMethod **detour_target)
{
	BodyRec *r;
	guint32 epoch = 0;
	if (detour_ftn) *detour_ftn = NULL;
	if (detour_target) *detour_target = NULL;
	if (mono_lazy_is_initialized (&body_init) == FALSE || !method)
		return 0;
	mono_os_mutex_lock (&body_mx);
	r = (BodyRec *) g_hash_table_lookup (body_tab, method);
	if (r) {
		epoch = r->epoch;
		if (detour_ftn) *detour_ftn = r->detour_ftn;
		if (detour_target) *detour_target = r->detour_target;
	}
	mono_os_mutex_unlock (&body_mx);
	return epoch;
}
