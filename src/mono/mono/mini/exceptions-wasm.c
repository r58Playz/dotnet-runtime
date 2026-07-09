#include "mini-runtime.h"
#include "mini.h"

static void
wasm_restore_context (void)
{
	g_error ("wasm_restore_context");
}

static void
wasm_call_filter (void)
{
	g_error ("wasm_call_filter");
}

static void
wasm_throw_exception (void)
{
	g_error ("wasm_throw_exception");
}

static void
wasm_rethrow_exception (void)
{
	g_error ("wasm_rethrow_exception");
}

static void
wasm_rethrow_preserve_exception (void)
{
	g_error ("wasm_rethrow_preserve_exception");
}

static void
wasm_throw_corlib_exception (void)
{
	g_error ("wasm_throw_corlib_exception");
}

gboolean
mono_arch_unwind_frame (MonoJitTlsData *jit_tls,
						MonoJitInfo *ji, MonoContext *ctx,
						MonoContext *new_ctx, MonoLMF **lmf,
						host_mgreg_t **save_locations,
						StackFrameInfo *frame)
{
	memset (frame, 0, sizeof (StackFrameInfo));
	frame->ji = ji;

	*new_ctx = *ctx;

	g_assert (!ji);

	/*
	 * Can't unwind native frames on WASM, so we only process the ones
	 * which push an LMF frame. See the needs_LMF code in
	 * method-to-ir.c.
	 */
	if (*lmf) {
		ERROR_DECL (error);

		if (*lmf == jit_tls->first_lmf)
			return FALSE;

		/* DIAG (wasm-jit EH unwind): a NULL-method PLAIN LMF here means either a save_lmf method's LMF was
		 * pushed without its method, or (more likely under aggressive island JIT) a wasm-JIT EH/finally
		 * codegen wild-store corrupted an LMFExt's previous_lmf (clearing the bit-2 ext tag -> it's read as a
		 * plain LMF with a garbage/NULL method). Dump the whole LMF chain so the NEXT crash names the frames
		 * around the bad entry, instead of just aborting blind. Bounded walk; g_printerr leaks names but we
		 * are aborting anyway. */
		if (G_UNLIKELY (!(*lmf)->method)) {
			MonoLMF *l = *lmf;
			int n = 0;
			g_printerr ("WASM_JIT_LMF_NULL_METHOD: LMF chain from %p (first_lmf=%p):\n", (void *) *lmf, (void *) jit_tls->first_lmf);
			while (l && n < 64) {
				gsize prev = (gsize) l->previous_lmf;
				/* A wasm-JIT island ext whose previous_lmf was wild-stored to ~0 loses its bit-2 tag and reads
				 * as a plain NULL-method LMF; recover the island's method (survives the offset-0 clobber) to
				 * name the corrupting method's finally/EH codegen. Browser-only (the island machinery is
				 * HOST_BROWSER; mono-aot-cross links this file but never runs islands). */
				MonoMethod *island_m = NULL;
#ifdef HOST_BROWSER
				{ extern MonoMethod *mono_wasm_jit_island_lmf_method (gpointer lmf); island_m = mono_wasm_jit_island_lmf_method (l); }
#endif
				if (prev & 2) {
					MonoLMFExt *ext = (MonoLMFExt *) l;
					if (ext->kind == MONO_LMFEXT_IL_STATE)
						g_printerr ("  [%d] %p EXT IL_STATE il_state.method=%s\n", n, (void *) l,
							ext->il_state && ext->il_state->method ? mono_method_get_full_name (ext->il_state->method) : "NULL");
					else
						g_printerr ("  [%d] %p EXT kind=%d\n", n, (void *) l, ext->kind);
				} else {
					g_printerr ("  [%d] %p PLAIN method=%s%s%s\n", n, (void *) l,
						l->method ? mono_method_get_full_name (l->method) : "NULL",
						island_m ? " CORRUPTED-ISLAND island.method=" : "",
						island_m ? mono_method_get_full_name (island_m) : "");
				}
				if (l == jit_tls->first_lmf)
					break;
				l = (MonoLMF *) (prev & ~3);
				n++;
			}
			g_printerr ("WASM_JIT_LMF_NULL_METHOD: end (walked %d frames)\n", n);
		}
		/* This will compute the original method address */
		g_assert ((*lmf)->method);
		gpointer addr = mono_compile_method_checked ((*lmf)->method, error);
		mono_error_assert_ok (error);

		ji = mini_jit_info_table_find (addr);
		g_assert (ji);

		frame->type = FRAME_TYPE_MANAGED;
		frame->ji = ji;
		frame->actual_method = (*lmf)->method;

		*lmf = (MonoLMF *)(((guint64)(*lmf)->previous_lmf) & ~3);
		return TRUE;
	}

	return FALSE;
}

gpointer
mono_arch_get_call_filter (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("call_filter", (guint8*)wasm_call_filter, 1, NULL, NULL);
	return (gpointer)wasm_call_filter;
}

gpointer
mono_arch_get_restore_context (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("restore_context", (guint8*)wasm_restore_context, 1, NULL, NULL);
	return (gpointer)wasm_restore_context;
}
gpointer
mono_arch_get_throw_corlib_exception (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("throw_corlib_exception", (guint8*)wasm_throw_corlib_exception, 1, NULL, NULL);
	return (gpointer)wasm_throw_corlib_exception;
}

gpointer
mono_arch_get_rethrow_exception (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("rethrow_exception", (guint8*)wasm_rethrow_exception, 1, NULL, NULL);
	return (gpointer)wasm_rethrow_exception;
}

gpointer
mono_arch_get_rethrow_preserve_exception (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("rethrow_preserve_exception", (guint8*)wasm_rethrow_preserve_exception, 1, NULL, NULL);
	return (gpointer)wasm_rethrow_exception;
}

gpointer
mono_arch_get_throw_exception (MonoTrampInfo **info, gboolean aot)
{
	if (info)
		*info = mono_tramp_info_create ("throw_exception", (guint8*)wasm_throw_exception, 1, NULL, NULL);
	return (gpointer)wasm_throw_exception;
}

void
mono_arch_undo_ip_adjustment (MonoContext *ctx)
{
}

gboolean
mono_arch_handle_exception (void *sigctx, gpointer obj)
{
	g_error ("mono_arch_handle_exception");
}
