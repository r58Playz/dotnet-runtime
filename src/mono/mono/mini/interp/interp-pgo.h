#ifndef __MONO_MINI_INTERP_PGO_H__
#define __MONO_MINI_INTERP_PGO_H__

gboolean
mono_interp_pgo_should_tier_method (MonoMethod *method);

// Add a method to the active interp_pgo table at runtime (hash computed internally), so it is
// compiled optimized from its first call. For building a table at runtime from a curated list.
void
mono_interp_pgo_add_method (MonoMethod *method);

void
mono_interp_pgo_method_was_tiered (MonoMethod *method);

void
mono_interp_pgo_generate_start (void);

void
mono_interp_pgo_generate_end (void);

#endif // __MONO_MINI_INTERP_PGO_H__
