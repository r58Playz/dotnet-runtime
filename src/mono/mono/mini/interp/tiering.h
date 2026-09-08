#ifndef __MONO_MINI_INTERP_TIERING_H__
#define __MONO_MINI_INTERP_TIERING_H__

#include "interp-internals.h"

#define INTERP_TIER_ENTRY_LIMIT 1000

void
mono_interp_tiering_init (void);

gboolean
mono_interp_tiering_enabled (void);

void
mono_interp_register_imethod_data_items (gpointer *data_items, GSList *indexes);

void
mono_interp_clear_data_items_patch_sites (gpointer *data_items, int n_data_items);

void
mono_interp_register_imethod_patch_site (gpointer *imethod_ptr);

/* Generational method bodies: swap in IL compiled after the once-unloadable types resolved, and force a
 * re-transform. Returns 1 replaced, 0 declined (already tiered or wasm-JITted), -1 bad shape.
 * Counters are read by the IKVM-side census; see the block comment in tiering.c. */
int
mono_interp_replace_method_body (MonoMethod *target, MonoMethod *source);

/* Marks a method as awaiting an in-place body replacement so the wasm JIT skips it; pending=0 clears. */
void
mono_interp_mark_relink_pending (MonoMethod *method, int pending);

extern gint32 mono_interp_relink_replaced;
extern gint32 mono_interp_relink_late;
extern gint32 mono_interp_relink_untouched;
extern gint32 mono_interp_relink_rejected;

const guint16*
mono_interp_tier_up_frame_enter (InterpFrame *frame, ThreadContext *context);

const guint16*
mono_interp_tier_up_frame_patchpoint (InterpFrame *frame, ThreadContext *context, int bb_index);

#endif /* __MONO_MINI_INTERP_TIERING_H__ */
