/**
 * \file
 * Run-time replacement of a method's IL body (the WasmDetour convention: MonoMod-style detours, IKVM generation-2
 * bodies). This half is metadata only -- which header a method has now. Invalidating what was compiled from the
 * previous one is the execution engine's (mini/interp/tiering.c, mono_body_*).
 */
#ifndef __MONO_METADATA_BODY_OVERRIDE_H__
#define __MONO_METADATA_BODY_OVERRIDE_H__

#include <glib.h>
#include <mono/metadata/metadata-internals.h>

/* Number of metadata methods with an override in force: the header readers' fast path is one load of this. */
extern volatile gint32 mono_body_override_live;

/* The override header for a METADATA method, or NULL (none in force, or a wrapper/SRE method, whose override lives on
 * MonoMethodWrapper.header where every reader already looks). Persistent: the caller must not free it. */
MonoMethodHeader *mono_body_override_header (MonoMethod *method);

/* A metadata method's OWN header while an override is in force, else NULL. Persistent. What reflection shows. */
MonoMethodHeader *mono_body_override_original (MonoMethod *method);

/* The header in force for `method` right now, as a PERSISTENT header (never freed, is_transient = 0) that a
 * compilation can bind to: the override, the wrapper's header, or a copy of the method's own header parsed once. NULL
 * on error. Callers serialize installs per method (tiering.c holds the method's jit_mm lock). */
MonoMethodHeader *mono_body_current_header (MonoMethod *method);

/* Install `header` as `method`'s body, or restore its own when `header` is NULL. A wrapper/SRE method's
 * MonoMethodWrapper.header is swapped; a metadata method's override record is set or cleared. `detour_ftn` /
 * `detour_target` record what a WasmDetour stub calls (NULL otherwise). Returns the new epoch; `installed` receives the
 * persistent header now in force. */
guint32 mono_body_override_install (MonoMethod *method, MonoMethodHeader *header, gpointer detour_ftn,
                                    MonoMethod *detour_target, MonoMethodHeader **installed);

/* The epoch of `method`'s body (0 = never replaced) and, when the body in force is a WasmDetour stub, what it calls. */
guint32 mono_body_override_info (MonoMethod *method, gpointer *detour_ftn, MonoMethod **detour_target);

#endif
