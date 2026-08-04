#include <stdio.h>
#include "mini-runtime.h"
#include "mini.h"

/* Diagnostics here use printf, NOT g_printerr, deliberately. Every wasm-JIT diagnostic that has been
 * observed in a captured browser log goes through printf (mini-wasm.c: 43 printf tags, zero g_printerr);
 * the g_printerr tags in this file had never once appeared, which is indistinguishable from "the condition
 * never happens" and cost several wrong conclusions before the channel was suspected rather than the
 * condition. Keep new diagnostics on the channel that is known to reach the log. */

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

/* Is `p` inside this module's linear memory? Needed before dereferencing a suspect pointer: on wasm a load
 * from an address past memory.size traps, so a validity check that dereferences first would itself raise the
 * "memory access out of bounds" it exists to prevent. Reading nonsense from a valid address cannot trap. */
static gboolean
wasm_ptr_in_memory (gconstpointer p)
{
#ifdef __wasm__
	gsize sz = (gsize) __builtin_wasm_memory_size (0) * 65536;
#else
	/* mono-aot-cross links this file and compiles it for the HOST (x64), where the wasm intrinsic does not
	 * exist -- an unguarded use is a hard -Werror=implicit-function-declaration failure in the cross build
	 * even though nothing here ever runs there. Same hazard the wasm-JIT flag definitions in mini-wasm.c
	 * are kept outside HOST_BROWSER for. */
	gsize sz = (gsize) -1;
#endif
	return (gsize) p >= 65536 && (gsize) p < sz;
}

/* Does this LMF ->method plausibly point at a MonoMethod? An LMFExt whose previous_lmf got wild-stored loses
 * its bit-2 tag and is then read as a plain LMF, so ->method is an unrelated overlapping value rather than
 * NULL -- which sails past a NULL check and faults inside mono_compile_method_checked instead. wrapper_type
 * is the discriminator: it is a small enum, a corrupted pointer essentially never lands on a valid one, and
 * it is the exact field whose out-of-range value trips debug-helpers.c's
 * "wrapper_type < MONO_WRAPPER_NUM" assertion further downstream. */
/* Print a method name only if the pointer survives validation, and free what mono_method_get_full_name
 * allocates. The old dump did neither: it called mono_method_get_full_name on unvalidated pointers (which is
 * what trips "wrapper_type < MONO_WRAPPER_NUM") and leaked every name on the grounds that it was aborting
 * anyway -- no longer true now that the unwinder recovers instead of asserting. */
static void wasm_lmf_print_method (const char *label, MonoMethod *m);

static gboolean
wasm_lmf_method_plausible (MonoMethod *m)
{
	if (!m || ((gsize) m & 3) || !wasm_ptr_in_memory (m))
		return FALSE;
	if ((guint) m->wrapper_type >= (guint) MONO_WRAPPER_NUM)
		return FALSE;
	if (!m->klass || ((gsize) m->klass & 3) || !wasm_ptr_in_memory (m->klass))
		return FALSE;
	return TRUE;
}

static void
wasm_lmf_print_method (const char *label, MonoMethod *m)
{
	char *s;
	if (!wasm_lmf_method_plausible (m)) {
		printf ("%s<implausible %p>", label, (void *) m);
		return;
	}
	s = mono_method_get_full_name (m);
	printf ("%s%s", label, s ? s : "<null>");
	g_free (s);
}

/* Re-entrancy guard for the mono_compile_method_checked call below.
 *
 * That compile can itself raise -- e.g. a TypeLoadException for one of IKVM's __<Unloadable> types -- and
 * IKVM routes every managed exception through ExceptionHelper.MapException, which builds a
 * System.Diagnostics.StackTrace, which walks the stack, which lands back here and compiles again. That loop
 * has no bound, so it exhausts the worker's C stack no matter how large it is (verified: a 16 MB
 * DEFAULT_PTHREAD_STACK_SIZE behaves identically to the default) and surfaces as an out-of-bounds access
 * inside jit_compile_method_with_opt_cb rather than as a stack-overflow diagnostic.
 *
 * Naming a frame is optional; not recursing is not. Cleared at the two terminal early-returns below, so a
 * flag left set by a compile that unwound out through wasm EH instead of returning self-heals on the next
 * unwind that reaches the bottom of the chain, rather than silently suppressing frame names forever. */
static __thread gboolean wasm_unwind_compiling;

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

		if (*lmf == jit_tls->first_lmf) {
			wasm_unwind_compiling = FALSE;   /* resync point: bottom of the chain */
			return FALSE;
		}

		/* MonoLMFExt entries are tagged with bit 2 in previous_lmf and are NOT plain LMFs: their ->method
		 * field overlaps other members of the ext struct, so reading it yields an unrelated value. Every
		 * other arch asserts this tag is clear here -- exceptions-amd64.c, -arm64.c and -x86.c all do
		 * g_assert ((((guint64)(*lmf)->previous_lmf) & 2) == 0) -- because mini-exceptions.c consumes ext
		 * frames before delegating to the arch hook. That consumption does not happen for us: this hook
		 * asserts !ji above, i.e. wasm frames never have jit info, so ext frames arrive unconsumed and fall
		 * straight into the plain-LMF path below.
		 *
		 * Untagged, an ext frame's overlapping ->method reaches mono_compile_method_checked. Non-NULL
		 * garbage faults inside the compiler (an out-of-bounds access in jit_compile_method_with_opt_cb);
		 * NULL instead reaches the diagnostic dump below, whose mono_method_get_full_name then trips
		 * "wrapper_type < MONO_WRAPPER_NUM" in debug-helpers.c. Both signatures were observed from this one
		 * path, reached via IKVM's exception mapping -> System.Diagnostics.StackTrace -> mono_get_frame_info
		 * while a wasm-JIT frame was on the stack.
		 *
		 * Skip them. An ext frame is not a managed frame we can name here, so it contributes nothing to a
		 * StackTrace; dropping it shortens a managed trace in rare cases instead of corrupting the heap.
		 * (MONO_LMFEXT_IL_STATE does carry a recoverable ext->il_state->method -- the dump below already
		 * reads it -- so it could be reported as a real frame later if trace fidelity ever matters.) */
		while (*lmf && *lmf != jit_tls->first_lmf && (((gsize) (*lmf)->previous_lmf) & 2))
			*lmf = (MonoLMF *) (((gsize) (*lmf)->previous_lmf) & ~3);
		if (!*lmf || *lmf == jit_tls->first_lmf)
			return FALSE;

		/* The wasm top-LMF marker is an all-zero MonoLMF (MONO_ARCH_INIT_TOP_LMF_ENTRY is empty).
		 * Normally it is recognized by pointer identity with jit_tls->first_lmf above. A cooperative
		 * attach/detach or JSPI TLS handoff can, however, leave an older root allocation at the end of
		 * the active LMF chain while jit_tls->first_lmf names the replacement. It is still a terminal
		 * marker, not a managed frame: it has no predecessor, address, or method. Recognize that exact
		 * structural sentinel so StackTrace stops cleanly instead of trying to compile method == NULL.
		 * Keep the assertion below for every nonterminal NULL-method LMF; linkage or lmf_addr state means
		 * it is a genuinely malformed/corrupted managed frame and must not be hidden. */
		if (G_UNLIKELY (!(*lmf)->previous_lmf && !(*lmf)->lmf_addr && !(*lmf)->method)) {
			*lmf = NULL;
			wasm_unwind_compiling = FALSE;   /* resync point: terminal sentinel */
			return FALSE;
		}

		/* DIAG (wasm-jit EH unwind): a NULL-method PLAIN LMF here means either a save_lmf method's LMF was
		 * pushed without its method, or (more likely under aggressive island JIT) a wasm-JIT EH/finally
		 * codegen wild-store corrupted an LMFExt's previous_lmf (clearing the bit-2 ext tag -> it's read as a
		 * plain LMF with a garbage/NULL method). Dump the whole LMF chain so the NEXT crash names the frames
		 * around the bad entry, instead of just aborting blind. Bounded walk; g_printerr leaks names but we
		 * are aborting anyway. */
		if (G_UNLIKELY (!wasm_lmf_method_plausible ((*lmf)->method))) {
			MonoLMF *l = *lmf;
			int n = 0;
			printf ("WASM_JIT_LMF_NULL_METHOD: LMF chain from %p (first_lmf=%p):\n", (void *) *lmf, (void *) jit_tls->first_lmf);
			while (l && n < 64) {
				gsize prev;
				/* The CHAIN is the corrupted thing, so each link must be checked before it is followed:
				 * `l` came from a previous_lmf that may have been wild-stored, and simply reading
				 * l->previous_lmf through a wild `l` is itself an out-of-bounds access. That is how this
				 * diagnostic turned into the very crash it exists to explain. */
				if (((gsize) l & 3) || !wasm_ptr_in_memory (l)) {
					printf ("  [%d] %p UNREADABLE — chain broken here, stopping walk\n", n, (void *) l);
					break;
				}
				prev = (gsize) l->previous_lmf;
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
					if (ext->kind == MONO_LMFEXT_IL_STATE) {
						printf ("  [%d] %p EXT IL_STATE", n, (void *) l);
						wasm_lmf_print_method (" il_state.method=",
							ext->il_state ? ext->il_state->method : NULL);
						printf ("\n");
					} else {
						printf ("  [%d] %p EXT kind=%d\n", n, (void *) l, ext->kind);
					}
				} else {
					printf ("  [%d] %p PLAIN method=%p", n, (void *) l, (void *) l->method);
					wasm_lmf_print_method (" ", l->method);
					if (island_m)
						wasm_lmf_print_method (" CORRUPTED-ISLAND island.method=", island_m);
					printf ("\n");
				}
				if (l == jit_tls->first_lmf)
					break;
				l = (MonoLMF *) (prev & ~3);
				n++;
			}
			printf ("WASM_JIT_LMF_NULL_METHOD: end (walked %d frames)\n", n);
		}
		/* Do not hand an implausible method to the compiler. Previously this was a bare
		 * g_assert ((*lmf)->method), which catches only the NULL case; a corrupted-but-non-NULL method got
		 * compiled and faulted with "memory access out of bounds" inside jit_compile_method_with_opt_cb.
		 * That fault is reached from IKVM exception mapping -> System.Diagnostics.StackTrace ->
		 * mono_get_frame_info while a wasm-JIT frame is on the stack, so it presents as an unrelated
		 * renderer death far from the real corruption.
		 *
		 * Skip the frame instead: the chain dump above has already named the surrounding frames, so the
		 * corruption is still reported loudly, and a StackTrace loses one entry rather than the process
		 * losing its heap. This is a containment boundary, NOT a repair -- whatever clears the ext tag /
		 * overwrites ->method is still a live bug and the dump is how to find it. */
		if (G_UNLIKELY (!wasm_lmf_method_plausible ((*lmf)->method))) {
			printf ("WASM_JIT_LMF_IMPLAUSIBLE: skipping LMF %p with method=%p (not compiling it)\n",
				(void *) *lmf, (void *) (*lmf)->method);
			*lmf = (MonoLMF *) (((gsize) (*lmf)->previous_lmf) & ~3);
			frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
			frame->ji = NULL;
			return TRUE;
		}
		if (G_UNLIKELY (wasm_unwind_compiling)) {
			printf ("WASM_JIT_UNWIND_REENTRY: already compiling for an unwind on this thread; skipping "
				"frame for %p instead of recursing\n", (void *) (*lmf)->method);
			*lmf = (MonoLMF *) (((gsize) (*lmf)->previous_lmf) & ~3);
			frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
			frame->ji = NULL;
			return TRUE;
		}
		/* Measure, don't infer. This compile is the frame the trap lands in, so record how much C stack is
		 * actually left at the moment we enter it. Raising DEFAULT_PTHREAD_STACK_SIZE to 16 MB changed
		 * nothing, which is consistent with either "the stack is not the problem" or "this code is not
		 * running on the pthread stack we enlarged" -- and those need distinguishing before any further
		 * stack-shaped hypothesis is worth testing. Prints only on a new low-water mark, so the log shows
		 * the approach to exhaustion rather than one line per unwind. */
#if defined (HOST_BROWSER) && defined (__wasm__)
		if (G_UNLIKELY (g_getenv ("MONO_WASM_JIT_STACKPROBE") != NULL)) {
			extern uintptr_t emscripten_stack_get_current (void);
			extern uintptr_t emscripten_stack_get_end (void);
			static __thread gsize lowest;
			uintptr_t cur = emscripten_stack_get_current (), end = emscripten_stack_get_end ();
			gsize freeb = cur > end ? (gsize) (cur - end) : 0;
			if (!lowest || freeb < lowest) {
				lowest = freeb;
				printf ("WASM_JIT_STACKPROBE: unwind-compile entry, free=%zu bytes (sp=%p end=%p)\n",
					(size_t) freeb, (void *) cur, (void *) end);
			}
		}
#endif
		wasm_unwind_compiling = TRUE;
		gpointer addr = mono_compile_method_checked ((*lmf)->method, error);
		wasm_unwind_compiling = FALSE;
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
