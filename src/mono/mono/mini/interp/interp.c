/**
 * \file
 *
 * interp.c: Interpreter for CIL byte codes
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Miguel de Icaza (miguel@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001, 2002 Ximian, Inc.
 */
#ifndef __USE_ISOC99
#define __USE_ISOC99
#endif
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <math.h>
#include <locale.h>

#include <mono/utils/mono-math.h>
#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-tls-inline.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/utils/mono-memory-model.h>

#ifdef HAVE_ALLOCA_H
#   include <alloca.h>
#else
#   ifdef __CYGWIN__
#      define alloca __builtin_alloca
#   endif
#endif

/* trim excessive headers */
#include <mono/metadata/image.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/cil-coff.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/verify.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/gc-internals.h>
#include <mono/utils/atomic.h>

#include "interp.h"
#include "interp-internals.h"
#include "mintops.h"
#include "interp-intrins.h"
#include "tiering.h"

#ifdef INTERP_ENABLE_SIMD
#include "interp-simd.h"
#endif

#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>
#include <mono/mini/aot-runtime.h>
#include <mono/mini/llvm-runtime.h>
#include <mono/mini/llvmonly-runtime.h>
#include <mono/mini/jit-icalls.h>
#include <mono/mini/ee.h>
#include <mono/mini/trace.h>

#include <mono/metadata/components.h>
#include <mono/metadata/loader-internals.h>

#ifdef TARGET_ARM
#include <mono/mini/mini-arm.h>
#endif
#include <mono/metadata/icall-decl.h>

#include "interp-pgo.h"

#ifdef HOST_BROWSER
#include "jiterpreter.h"
#include <emscripten.h>
#include <emscripten/stack.h>
extern void stackRestore (uintptr_t sp);   /* compiler-rt (stack_ops.S): sets __stack_pointer; JIT frames live on the C stack */

/* Overflow-safe probe bounds for the JIT-boundary diagnostics (mirrors mini-wasm.c wj_probe_ok):
 * the naive `a + 8 > memsz` WRAPS for a near 2^32 (e.g. an object whose first word is int -8), so the
 * probe's own speculative deref becomes the OOB trap that silently kills a JSPI-suspended thread. Also
 * clamps the 65536-page (4GB) case where `pages << 16` overflows 32-bit gsize to 0. */
static inline gsize
wj_memsz (void)
{
	gsize s = (gsize) __builtin_wasm_memory_size (0) << 16;
	return s ? s : (gsize) -1;
}

static inline gboolean
wj_probe_ok (gsize a, gsize memsz)
{
	return !(a & 3) && a >= 1024 && a <= memsz - 8;
}
#endif

/* Arguments that are passed when invoking only a finally/filter clause from the frame */
struct FrameClauseArgs {
	/* Where we start the frame execution from */
	const guint16 *start_with_ip;
	/*
	 * End ip of the exit_clause. We need it so we know whether the resume
	 * state is for this frame (which is called from EH) or for the original
	 * frame further down the stack.
	 */
	const guint16 *end_at_ip;
	/* Frame that is executing this clause */
	InterpFrame *exec_frame;
	gboolean run_until_end;
};

static MONO_NEVER_INLINE void
mono_interp_exec_method (InterpFrame *frame, ThreadContext *context, FrameClauseArgs *clause_args);

/*
 * This code synchronizes with interp_mark_stack () using compiler memory barriers.
 */

static FrameDataFragment*
frame_data_frag_new (int size)
{
	FrameDataFragment *frag = (FrameDataFragment*)g_malloc (size);

	frag->pos = (guint8*)&frag->data;
	frag->end = (guint8*)frag + size;
	frag->next = NULL;
	return frag;
}

static void
frame_data_frag_free (FrameDataFragment *frag)
{
	while (frag) {
		FrameDataFragment *next = frag->next;
		g_free (frag);
		frag = next;
	}
}

static void
frame_data_allocator_init (FrameDataAllocator *stack, int size)
{
	FrameDataFragment *frag;

	frag = frame_data_frag_new (size);
	stack->first = stack->current = frag;
	stack->infos_capacity = 4;
	stack->infos = (FrameDataInfo*)g_malloc (stack->infos_capacity * sizeof (FrameDataInfo));
}

static void
frame_data_allocator_free (FrameDataAllocator *stack)
{
	/* Assert to catch leaks */
	g_assert_checked (stack->current == stack->first && stack->current->pos == (guint8*)&stack->current->data);
	frame_data_frag_free (stack->first);
}

static FrameDataFragment*
frame_data_allocator_add_frag (FrameDataAllocator *stack, int size)
{
	FrameDataFragment *new_frag;

	// FIXME:
	guint frag_size = 4096;
	if (size + sizeof (FrameDataFragment) > frag_size)
		frag_size = size + sizeof (FrameDataFragment);
	new_frag = frame_data_frag_new (frag_size);
	mono_compiler_barrier ();
	stack->current->next = new_frag;
	stack->current = new_frag;
	return new_frag;
}

static gpointer
frame_data_allocator_alloc (FrameDataAllocator *stack, InterpFrame *frame, int size)
{
	FrameDataFragment *current = stack->current;
	gpointer res;

	int infos_len = stack->infos_len;

	if (!infos_len || (infos_len > 0 && stack->infos [infos_len - 1].frame != frame)) {
		/* First allocation by this frame. Save the markers for restore */
		if (infos_len == stack->infos_capacity) {
			stack->infos_capacity = infos_len * 2;
			stack->infos = (FrameDataInfo*)g_realloc (stack->infos, stack->infos_capacity * sizeof (FrameDataInfo));
		}
		stack->infos [infos_len].frame = frame;
		stack->infos [infos_len].frag = current;
		stack->infos [infos_len].pos = current->pos;
		stack->infos_len++;
	}

	if (G_LIKELY (current->pos + size <= current->end)) {
		res = current->pos;
		current->pos += size;
	} else {
		if (current->next && current->next->pos + size <= current->next->end) {
			current = stack->current = current->next;
			current->pos = (guint8*)&current->data;
		} else {
			FrameDataFragment *tmp = current->next;
			/* avoid linking to be freed fragments, so the GC can't trip over it */
			current->next = NULL;
			mono_compiler_barrier ();
			frame_data_frag_free (tmp);

			current = frame_data_allocator_add_frag (stack, size);
		}
		g_assert (current->pos + size <= current->end);
		res = (gpointer)current->pos;
		current->pos += size;
	}
	mono_compiler_barrier ();
	return res;
}

static void
frame_data_allocator_pop (FrameDataAllocator *stack, InterpFrame *frame)
{
	int infos_len = stack->infos_len;

	if (infos_len > 0 && stack->infos [infos_len - 1].frame == frame) {
		infos_len--;
		stack->current = stack->infos [infos_len].frag;
		stack->current->pos = stack->infos [infos_len].pos;
		stack->infos_len = infos_len;
	}
}

/*
 * reinit_frame:
 *
 *   Reinitialize a frame.
 */
static void
reinit_frame (InterpFrame *frame, InterpFrame *parent, InterpMethod *imethod, gpointer retval, gpointer stack)
{
	frame->parent = parent;
	frame->imethod = imethod;
	frame->stack = (stackval*)stack;
	frame->retval = (stackval*)retval;
	frame->state.ip = NULL;
}

#define STACK_ADD_ALIGNED_BYTES(sp,bytes) ((stackval*)((char*)(sp) + (bytes)))
#define STACK_ADD_BYTES(sp,bytes) ((stackval*)((char*)(sp) + ALIGN_TO(bytes, MINT_STACK_SLOT_SIZE)))
#define STACK_SUB_BYTES(sp,bytes) ((stackval*)((char*)(sp) - ALIGN_TO(bytes, MINT_STACK_SLOT_SIZE)))

/*
 * List of classes whose methods will be executed by transitioning to JITted code.
 * Used for testing.
 */
GSList *mono_interp_jit_classes;
/* Optimizations enabled with interpreter */
int mono_interp_opt = INTERP_OPT_DEFAULT;
/* If TRUE, interpreted code will be interrupted at function entry/backward branches */
static gboolean ss_enabled;

static gboolean interp_init_done = FALSE;

#ifdef HOST_WASI
static gboolean debugger_enabled = FALSE;
#endif

static MonoException* do_transform_method (InterpMethod *imethod, InterpFrame *method, ThreadContext *context);

typedef void (*ICallMethod) (InterpFrame *frame);

static MonoNativeTlsKey thread_context_id;

#define DEBUG_INTERP 0
#define COUNT_OPS 0

#if DEBUG_INTERP
int mono_interp_traceopt = 2;
/* If true, then we output the opcodes as we interpret them */
static int global_tracing = 2;

static int debug_indent_level = 0;

static int break_on_method = 0;
static int nested_trace = 0;
static GList *db_methods = NULL;
static char* dump_args (InterpFrame *inv);

static void
output_indent (void)
{
	int h;

	for (h = 0; h < debug_indent_level; h++)
		g_print ("  ");
}

static void
db_match_method (gpointer data, gpointer user_data)
{
	MonoMethod *m = (MonoMethod*)user_data;
	MonoMethodDesc *desc = (MonoMethodDesc*)data;

	if (mono_method_desc_full_match (desc, m))
		break_on_method = 1;
}

static void
debug_enter (InterpFrame *frame, int *tracing)
{
	if (db_methods) {
		g_list_foreach (db_methods, db_match_method, (gpointer)frame->imethod->method);
		if (break_on_method)
			*tracing = nested_trace ? (global_tracing = 2, 3) : 2;
		break_on_method = 0;
	}
	if (*tracing) {
		MonoMethod *method = frame->imethod->method;
		char *mn, *args = dump_args (frame);
		debug_indent_level++;
		output_indent ();
		mn = mono_method_full_name (method, FALSE);
		g_print ("(%p) Entering %s (", mono_thread_internal_current (), mn);
		g_free (mn);
		g_print  ("%s)\n", args);
		g_free (args);
	}
}

#define DEBUG_LEAVE()	\
	if (tracing) {	\
		char *mn, *args;	\
		args = dump_retval (frame);	\
		output_indent ();	\
		mn = mono_method_full_name (frame->imethod->method, FALSE); \
		g_print  ("(%p) Leaving %s", mono_thread_internal_current (),  mn);	\
		g_free (mn); \
		g_print  (" => %s\n", args);	\
		g_free (args);	\
		debug_indent_level--;	\
		if (tracing == 3) global_tracing = 0; \
	}

#else

int mono_interp_traceopt = 0;
#define DEBUG_LEAVE()

#endif

#if defined(__GNUC__) && !defined(TARGET_WASM) && !COUNT_OPS && !DEBUG_INTERP && !ENABLE_CHECKED_BUILD && !PROFILE_INTERP
#define USE_COMPUTED_GOTO 1
#endif

#if USE_COMPUTED_GOTO

#define MINT_IN_DISPATCH(op) goto *in_labels [opcode = (MintOpcode)(op)]
#define MINT_IN_SWITCH(op)   MINT_IN_DISPATCH (op);
#define MINT_IN_BREAK        MINT_IN_DISPATCH (*ip)
#define MINT_IN_CASE(x)      LAB_ ## x:

#else

#define MINT_IN_SWITCH(op) COUNT_OP(op); switch (opcode = (MintOpcode)(op))
#define MINT_IN_CASE(x) case x:
#define MINT_IN_BREAK break

#endif

static void
clear_resume_state (ThreadContext *context)
{
	context->has_resume_state = 0;
	context->handler_frame = NULL;
	context->handler_ei = NULL;
	g_assert (context->exc_gchandle);
	mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = 0;
}

/*
 * If this bit is set, it means the call has thrown the exception, and we
 * reached this point because the EH code in mono_handle_exception ()
 * unwound all the JITted frames below us. mono_interp_set_resume_state ()
 * has set the fields in context to indicate where we have to resume execution.
 */
#define CHECK_RESUME_STATE(context) do { \
		if ((context)->has_resume_state)	\
			goto resume;			\
	} while (0)

static void
set_context (ThreadContext *context)
{
	mono_native_tls_set_value (thread_context_id, context);

	if (!context)
		return;

	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
	g_assertf (jit_tls, "ThreadContext needs initialized JIT TLS");

	/* jit_tls assumes ownership of 'context' */
	jit_tls->interp_context = context;
}

static ThreadContext *
get_context (void)
{
	ThreadContext *context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	if (context == NULL) {
		context = g_new0 (ThreadContext, 1);
		context->stack_start = (guchar*)mono_valloc_aligned (INTERP_STACK_SIZE, MINT_STACK_ALIGNMENT, MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		// A bit for every pointer sized slot in the stack. FIXME don't allocate whole bit array
		if (mono_interp_opt & INTERP_OPT_PRECISE_GC)
			context->no_ref_slots = (guchar*)mono_valloc (NULL, INTERP_STACK_SIZE / (8 * sizeof (gpointer)), MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		context->stack_end = context->stack_start + INTERP_STACK_SIZE - INTERP_REDZONE_SIZE;
		context->stack_real_end = context->stack_start + INTERP_STACK_SIZE;
		/* We reserve a stack slot at the top of the interp stack to make temp objects visible to GC */
		context->stack_pointer = context->stack_start + MINT_STACK_ALIGNMENT;

		frame_data_allocator_init (&context->data_stack, 8192);
		/* Make sure all data is initialized before publishing the context */
		mono_compiler_barrier ();
		set_context (context);
	}
	return context;
}

static void
interp_free_context (gpointer ctx)
{
	ThreadContext *context = (ThreadContext*)ctx;

	ThreadContext *current_context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	/* at thread exit, we can be called from the JIT TLS key destructor with current_context == NULL */
	if (current_context != NULL) {
		/* check that the context we're freeing is the current one before overwriting TLS */
		g_assert (context == current_context);
		set_context (NULL);
	}

	mono_vfree (context->stack_start, INTERP_STACK_SIZE, MONO_MEM_ACCOUNT_INTERP_STACK);
	/* Prevent interp_mark_stack from trying to scan the data_stack, before freeing it */
	context->stack_start = NULL;
	mono_compiler_barrier ();
	frame_data_allocator_free (&context->data_stack);
	g_free (context);
}

static gboolean
need_native_unwind (ThreadContext *context)
{
	return context->has_resume_state && !context->handler_frame;
}

void
mono_interp_error_cleanup (MonoError* error)
{
	mono_error_cleanup (error); /* FIXME: don't swallow the error */
	error_init_reuse (error); // one instruction, so this function is good inline candidate
}

static InterpMethod*
lookup_imethod (MonoMethod *method)
{
	InterpMethod *imethod;
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (method);

	jit_mm_lock (jit_mm);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
	jit_mm_unlock (jit_mm);

	return imethod;
}

InterpMethod*
mono_interp_get_imethod (MonoMethod *method)
{
	InterpMethod *imethod;
	MonoMethodSignature *sig;
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (method);
	int i;

	jit_mm_lock (jit_mm);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
	jit_mm_unlock (jit_mm);
	if (imethod)
		return imethod;

	sig = mono_method_signature_internal (method);

	if (method->dynamic)
		imethod = (InterpMethod*)mono_dyn_method_alloc0 (method, sizeof (InterpMethod));
	else
		imethod = (InterpMethod*)m_method_alloc0 (method, sizeof (InterpMethod));
	imethod->method = method;
	imethod->param_count = sig->param_count;
	imethod->hasthis = sig->hasthis;
	imethod->vararg = sig->call_convention == MONO_CALL_VARARG;
	imethod->code_type = IMETHOD_CODE_UNKNOWN;
	// This flag allows us to optimize out the interp_entry 'is this a delegate invoke' checks
	imethod->is_invoke = (m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) && !strcmp(method->name, "Invoke");
	// always optimize code if tiering is disabled
	// always optimize wrappers
	if (!mono_interp_tiering_enabled () || method->wrapper_type != MONO_WRAPPER_NONE || (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL))
		imethod->optimized = TRUE;

	if (imethod->method->string_ctor)
		imethod->rtype = m_class_get_byval_arg (mono_defaults.string_class);
	else
		imethod->rtype = mini_get_underlying_type (sig->ret);
	if (method->dynamic)
		imethod->param_types = (MonoType**)mono_dyn_method_alloc0 (method, sizeof (MonoType*) * sig->param_count);
	else
		imethod->param_types = (MonoType**)m_method_alloc0 (method, sizeof (MonoType*) * sig->param_count);
	for (i = 0; i < sig->param_count; ++i)
		imethod->param_types [i] = mini_get_underlying_type (sig->params [i]);

	if (!imethod->optimized && mono_interp_pgo_should_tier_method (method))
		imethod->optimized = TRUE;

	jit_mm_lock (jit_mm);
	InterpMethod *old_imethod;
	if (!((old_imethod = mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method)))) {
		mono_internal_hash_table_insert (&jit_mm->interp_code_hash, method, imethod);
	} else {
		imethod = old_imethod; /* leak the newly allocated InterpMethod to the mempool */
	}
	jit_mm_unlock (jit_mm);

	imethod->prof_flags = mono_profiler_get_call_instrumentation_flags (imethod->method);

	return imethod;
}

#if defined (MONO_CROSS_COMPILE) || defined (HOST_WASM)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT;

#elif defined(MONO_ARCH_HAS_NO_PROPER_MONOCTX)
/* some platforms, e.g. appleTV, don't provide us a precise MonoContext
 * (registers are not accurate), thus resuming to the label does not work. */
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT;
#elif defined (_MSC_VER)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX; \
	(ext).interp_exit_label_set = FALSE; \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx); \
	if ((ext).interp_exit_label_set == FALSE) \
		mono_arch_do_ip_adjustment (&(ext).ctx); \
	if ((ext).interp_exit_label_set == TRUE) \
		goto exit_label; \
	(ext).interp_exit_label_set = TRUE;
#elif defined(MONO_ARCH_HAS_MONO_CONTEXT)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX; \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx); \
	MONO_CONTEXT_SET_IP (&(ext).ctx, (&&exit_label)); \
	mono_arch_do_ip_adjustment (&(ext).ctx);
#else
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) g_error ("requires working mono-context");
#endif

/* INTERP_PUSH_LMF_WITH_CTX:
 *
 * same as interp_push_lmf, but retrieving and attaching MonoContext to it.
 * This is needed to resume into the interp when the exception is thrown from
 * native code (see ./mono/tests/install_eh_callback.exe).
 *
 * This must be a macro in order to retrieve the right register values for
 * MonoContext.
 */
#define INTERP_PUSH_LMF_WITH_CTX(frame, ext, exit_label) \
	memset (&(ext), 0, sizeof (MonoLMFExt)); \
	(ext).interp_exit_data = (frame); \
	INTERP_PUSH_LMF_WITH_CTX_BODY ((ext), exit_label); \
	mono_push_lmf (&(ext));

/*
 * interp_push_lmf:
 *
 * Push an LMF frame on the LMF stack
 * to mark the transition to native code.
 * This is needed for the native code to
 * be able to do stack walks.
 */
static void
interp_push_lmf (MonoLMFExt *ext, InterpFrame *frame)
{
	memset (ext, 0, sizeof (MonoLMFExt));
	ext->kind = MONO_LMFEXT_INTERP_EXIT;
	ext->interp_exit_data = frame;

	mono_push_lmf (ext);
}

static void
interp_pop_lmf (MonoLMFExt *ext)
{
	mono_pop_lmf (&ext->lmf);
}

#if HOST_BROWSER
/* Global "a thread is compiling" flag. mono_wasm_force_compile -> mini_method_compile is the ONLY
 * compilation in the interp-only runtime, and it is NOT safe to run concurrently here (the wasm
 * emitter + cctor execution race on shared state -> corrupt module bytes, seen as
 * "WebAssembly.Module(): expected magic word, found <garbage>" CompileErrors when a worker
 * instantiates). The island's burst of cross-worker force-compiles made this frequent (jit82 crash).
 * Serialize with a NON-BLOCKING try-acquire: a thread that loses the race skips compiling and returns
 * "retriable" so the method is re-attempted later — never blocks, so coop-GC suspend can't deadlock. */
static volatile gint32 wj_compiling = 0;

/*
 * Island/transition diagnostics (Part 3). Two bounded, open-addressed, lock-free tables (racy under
 * threads — approximate, which the wasm-jit stats already accept). Gated by mono_wasm_jit_stats.
 *
 *  - wj_entry_edges: the interp->JIT boundary the transition storm crosses. Keyed (caller, callee) at
 *    the MINT_CALL/CALLVIRT invoke sites. `window` is reset by mono_wasm_jit_snapshot so the harness can
 *    report a per-"frame" (per-sampling-window) top-N; `count` is cumulative.
 *  - wj_block_tab: a registry of methods that BLOCKED a caller's island from completing (residual=0 +
 *    callee not jitted). The per-callee count lives on InterpMethod.wasm_jit_block_n; this table just
 *    records which methods to scan for the top-N report (their bail reason is read from wasm_jit_bail).
 *
 * CRITICAL: the dump exports are called from JS on the MAIN thread. mono_method_get_full_name / the
 * jit_mm + loader locks taken by mono_interp_get_imethod can deadlock there against a GC-suspended
 * worker holding the lock (cooperative-GC) — a hard UI freeze. So everything the dump needs (method
 * full names, the caller's InterpMethod*) is resolved + cached HERE at record time, on the coop interp/
 * compile thread where taking those locks is safe. The dumps then only read plain memory: NO Mono API.
 */
#define WJ_EDGE_SLOTS 8192
#define WJ_EDGE_PROBE 64
typedef struct {
	MonoMethod *caller, *callee;          /* hash key */
	guint32 count, window;
	InterpMethod *caller_im;              /* cached: dump reads slot/hits/bail via a plain deref (no lock) */
	char *caller_name, *callee_name;      /* cached full names (g_malloc'd at insertion; never freed) */
} WjEntryEdge;
static WjEntryEdge wj_entry_edges [WJ_EDGE_SLOTS];

static void
wj_edge_bump (InterpMethod *caller_im, InterpMethod *callee_im)
{
	MonoMethod *caller = caller_im->method, *callee = callee_im->method;
	gsize h = (((gsize) caller >> 4) ^ ((gsize) callee >> 4)) & (WJ_EDGE_SLOTS - 1);
	int i;
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		WjEntryEdge *e = &wj_entry_edges [(h + i) & (WJ_EDGE_SLOTS - 1)];
		if (e->callee == callee && e->caller == caller) { e->count++; e->window++; return; }
		if (!e->callee) {
			/* first time we see this edge: cache names + caller imethod now (coop thread, safe to lock) */
			e->caller = caller; e->caller_im = caller_im;
			e->caller_name = mono_method_get_full_name (caller);
			e->callee_name = mono_method_get_full_name (callee);
			e->count = 1; e->window = 1;
			mono_memory_barrier ();
			e->callee = callee;   /* publish the occupied marker last */
			return;
		}
	}
	/* table full / long probe chain: drop the sample (approximate is fine for a bench histogram) */
}

#define WJ_BLOCK_SLOTS 4096
static MonoMethod *wj_block_tab [WJ_BLOCK_SLOTS];
static guint32 wj_block_cnt [WJ_BLOCK_SLOTS];        /* per-slot block count (dump scan reads this — no Mono API) */
static InterpMethod *wj_block_im [WJ_BLOCK_SLOTS];   /* cached: dump reads slot/bail via deref (no jit_mm lock) */
static char *wj_block_name [WJ_BLOCK_SLOTS];         /* cached full name (resolved at record time on a coop thread) */
static void wj_promote_push (MonoMethod *m);

/* Note that `callee` blocked an island compile. The per-method block_n counter is bumped ALWAYS (it's a
 * cheap compile-time event and doubles as a stats-independent "hot at the island boundary" signal for
 * the Lever C cold gate); the top-N report table is only populated when stats are on. Called from
 * wasm_jit_compile_publish on a retriable ("callee not jitted") bail. */
static void
wj_block_note (MonoMethod *callee)
{
	InterpMethod *cim = mono_interp_get_imethod (callee);
	cim->wasm_jit_block_n++;
	{
		extern int mono_wasm_jit_block_force;
		if (mono_wasm_jit_block_force > 0 && cim->wasm_jit_fslot <= 0 && cim->wasm_jit_slot != -1
		    && cim->wasm_jit_block_n == mono_wasm_jit_block_force)
			wj_promote_push (callee);
	}
	if (G_LIKELY (!mono_wasm_jit_stats))
		return;
	{
		gsize h = ((gsize) callee >> 4) & (WJ_BLOCK_SLOTS - 1);
		int i;
		for (i = 0; i < WJ_EDGE_PROBE; ++i) {
			int idx = (h + i) & (WJ_BLOCK_SLOTS - 1);
			MonoMethod **s = &wj_block_tab [idx];
			if (*s == callee) { wj_block_cnt [idx]++; return; }
			if (!*s) {
				/* first time: cache name + imethod now (coop compile thread, safe to lock) */
				wj_block_im [idx] = cim;
				wj_block_name [idx] = mono_method_get_full_name (callee);
				wj_block_cnt [idx] = 1;
				mono_memory_barrier ();
				*s = callee;   /* publish last */
				return;
			}
		}
	}
}

/* Lever A — upward island growth: a bounded best-effort queue of hot interp CALLERS to force-JIT, so
 * their call to an already-JITted callee lowers to a direct f-slot (no transition). Pushed from the
 * invoke sites when a caller's wasm_jit_invoke_out crosses MONO_WASM_JIT_ENTRY_PROMOTE; drained at the
 * wasm_jit_maybe_compile safe point. Racy across threads (drops still acceptable; duplicate enqueues are
 * suppressed so woken/parked methods don't churn the queue). */
#define WJ_PROMOTE_Q 256
static MonoMethod * volatile wj_promote_q [WJ_PROMOTE_Q];
static volatile gint32 wj_promote_head, wj_promote_tail;

static void
wj_promote_push (MonoMethod *m)
{
	gint32 h = wj_promote_head;
	gint32 t = wj_promote_tail;
	gint32 i;
	for (i = h; i != t; ++i)
		if (wj_promote_q [i & (WJ_PROMOTE_Q - 1)] == m)
			return;   /* already queued */
	if (t - wj_promote_head >= WJ_PROMOTE_Q) return;   /* full: drop */
	wj_promote_q [t & (WJ_PROMOTE_Q - 1)] = m;
	wj_promote_tail = t + 1;
}

/* --- Event-driven blocker waiters (residual=0 islands) ---------------------------------------------------
 * Reverse-dependency map: callee MonoMethod* -> the methods PARKED waiting for it to JIT. When a method can't
 * close its island because a DIRECT callee is cold, it registers as a waiter on that callee and parks (slot =
 * WASM_JIT_SLOT_PARKED) instead of busy poll-retrying; when the callee later JITs (or goes permanent),
 * wj_waiter_drain re-queues every waiter via wj_promote_q -> wasm_jit_drain_promotions. Open-addressed by
 * callee-pointer hash; keys are NEVER cleared (methods aren't unloaded on wasm) so there are no tombstones —
 * drain only steals+frees the per-slot array. Mutated under mono_loader_lock (the same lock
 * mono_wasm_jit_register uses), taken only OUTSIDE the wj_compiling compile section, so it never nests the
 * wrong way with the compile path. */
/* wasm_jit_slot states: 0 = untried (counting hits); >0 = JITted (the entry-thunk table slot); -1 = permanent
 * bail; WASM_JIT_SLOT_PARKED = blocked on a cold callee, registered as a waiter and woken by an event (a
 * blocker JITs); WASM_JIT_SLOT_RETRY = transient retry (e.g. another thread was compiling), NOT waiter-
 * parked and still eligible for coarse threshold/promotion retries. */
#define WASM_JIT_SLOT_PARKED (-2)
#define WASM_JIT_SLOT_RETRY  (-3)

static inline gboolean
wj_slot_hot_retry_eligible (gint32 slot)
{
	return slot == 0 || slot == WASM_JIT_SLOT_RETRY;
}

static inline gboolean
wj_slot_retriable (gint32 slot)
{
	return slot == 0 || slot == WASM_JIT_SLOT_PARKED || slot == WASM_JIT_SLOT_RETRY;
}

#define WJ_WAITER_SLOTS 4096
#define WJ_WAITER_MAX   256   /* cap waiters tracked per callee (beyond this it's force-compiled anyway) */
static MonoMethod *wj_waiter_key [WJ_WAITER_SLOTS];
static GPtrArray  *wj_waiter_arr [WJ_WAITER_SLOTS];

/* Park `waiter` on `callee`. If enough islands are already blocked on `callee` (>= MONO_WASM_JIT_BLOCK_PROMOTE
 * distinct waiters), force-compile it now so it drains them. Re-checks callee's slot after inserting to close
 * the register-after-drain race (a concurrent compile may JIT callee between our insert and a prior drain). */
static void
wj_waiter_register (MonoMethod *callee, MonoMethod *waiter)
{
	extern void mono_loader_lock (void);
	extern void mono_loader_unlock (void);
	extern int mono_wasm_jit_block_promote;
	gsize h = ((gsize) callee >> 4) & (WJ_WAITER_SLOTS - 1);
	int i, force_callee = 0, registered = 0;
	mono_loader_lock ();
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		int idx = (h + i) & (WJ_WAITER_SLOTS - 1);
		GPtrArray *a;
		guint j;
		gboolean dup = FALSE;
		if (wj_waiter_key [idx] && wj_waiter_key [idx] != callee)
			continue;                                          /* slot owned by another callee: probe on */
		if (!wj_waiter_key [idx]) wj_waiter_key [idx] = callee; /* claim empty slot (key never cleared) */
		if (!wj_waiter_arr [idx]) wj_waiter_arr [idx] = g_ptr_array_new ();
		a = wj_waiter_arr [idx];
		for (j = 0; j < a->len; ++j) if (a->pdata [j] == waiter) { dup = TRUE; break; }
		if (!dup && (int) a->len < WJ_WAITER_MAX) {
			g_ptr_array_add (a, waiter);
			/* Force-compile the callee exactly ONCE, when its block_promote-th DISTINCT waiter registers
			 * (==, not >=, and only on a real add). Re-pushing on every subsequent registration would flood
			 * the promote queue and, with mutually-blocking callees, spin a compile storm. */
			if (mono_wasm_jit_block_promote > 0 && (int) a->len == mono_wasm_jit_block_promote)
				force_callee = 1;
		}
		registered = 1;
		break;
	}
	mono_loader_unlock ();
	if (mono_interp_get_imethod (callee)->wasm_jit_fslot > 0)
		wj_promote_push (waiter);   /* callee already JITted (race) -> retry the waiter now */
	else if (!registered)
		wj_promote_push (waiter);   /* probe chain full -> can't park; let the waiter re-attempt soon */
	else if (force_callee)
		wj_promote_push (callee);   /* enough islands blocked on it -> force it (its success drains them) */
}

/* `callee` just JITted (or went permanent): re-queue every method parked waiting on it (so it re-attempts and
 * either closes its island now, or — if callee went permanent — discovers that and propagates the perm bail). */
static void
wj_waiter_drain (MonoMethod *callee)
{
	extern void mono_loader_lock (void);
	extern void mono_loader_unlock (void);
	gsize h = ((gsize) callee >> 4) & (WJ_WAITER_SLOTS - 1);
	int i;
	GPtrArray *a = NULL;
	mono_loader_lock ();
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		int idx = (h + i) & (WJ_WAITER_SLOTS - 1);
		if (wj_waiter_key [idx] == callee) { a = wj_waiter_arr [idx]; wj_waiter_arr [idx] = NULL; break; }
		if (!wj_waiter_key [idx]) break;   /* empty slot ends the (tombstone-free) probe chain: not present */
	}
	mono_loader_unlock ();
	if (a) {
		guint j;
		for (j = 0; j < a->len; ++j) wj_promote_push ((MonoMethod *) a->pdata [j]);
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_add (WJC_WAITER_WOKEN, (gint64) a->len);
		g_ptr_array_free (a, TRUE);
	}
}

/* Reset the per-window edge counts (the harness calls this at the START of a bench window; at the END it
 * calls mono_wasm_jit_dump_hot_edges, which prints by `window` = that window's accumulation). */
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_snapshot (void)
{
	int i;
	for (i = 0; i < WJ_EDGE_SLOTS; ++i)
		wj_entry_edges [i].window = 0;
}

/* Top-N interp->JIT entry edges by current-window count, annotated with the CALLER's JIT state so it's
 * obvious whether the caller is promotable-upward (slot 0/parked, jittable) or perm-blocked (slot -1). */
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_hot_edges (int topn)
{
	guint32 lastw = 0xffffffffu; int lasti = WJ_EDGE_SLOTS, shown = 0;
	if (topn <= 0) topn = 40;
	printf ("[wasm-jit hot entry-edges] interp->JIT crossings this window (caller -> callee):\n");
	while (shown < topn) {
		int best = -1, k; guint32 bestw = 0;
		for (k = 0; k < WJ_EDGE_SLOTS; ++k) {
			guint32 w = wj_entry_edges [k].window;
			if (!w || w > lastw || (w == lastw && k >= lasti)) continue;
			if (best < 0 || w > bestw || (w == bestw && k > best)) { best = k; bestw = w; }
		}
		if (best < 0) break;
		{
			WjEntryEdge *e = &wj_entry_edges [best];
			InterpMethod *cim = e->caller_im;   /* cached at record time — plain deref, NO Mono API on the main thread */
			printf ("  %8u (cum %u)  %s -> %s  [caller slot=%d hits=%d bail=%d]\n",
				e->window, e->count, e->caller_name ? e->caller_name : "?", e->callee_name ? e->callee_name : "?",
				cim ? cim->wasm_jit_slot : 0, cim ? cim->wasm_jit_hits : 0, cim ? cim->wasm_jit_bail : 0);
		}
		lastw = bestw; lasti = best; shown++;
	}
	fflush (stdout);
}

/* Top-N island-blocking callees by block count, with WHY each can't JIT (wasm_jit_bail) — the most
 * actionable island signal: "if callee X were jittable, N island attempts would complete." */
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_blockers (int topn)
{
	gint32 lastc = 0x7fffffff; int lasti = -1, shown = 0;
	if (topn <= 0) topn = 40;
	printf ("[wasm-jit island blockers] un-JITted callees that blocked a caller's island:\n");
	while (shown < topn) {
		int best = -1, k; gint32 bestc = 0;
		for (k = 0; k < WJ_BLOCK_SLOTS; ++k) {
			gint32 c = (gint32) wj_block_cnt [k];   /* pure array read — no Mono API / locks in the hot scan */
			if (!c || c > lastc || (c == lastc && k <= lasti)) continue;
			if (best < 0 || c > bestc || (c == bestc && k > best)) { best = k; bestc = c; }
		}
		if (best < 0) break;
		{
			InterpMethod *im = wj_block_im [best];   /* cached — plain deref, NO Mono API on the main thread */
			gint16 bail = im ? im->wasm_jit_bail : 0;
			gint32 slot = im ? im->wasm_jit_slot : 0;
			const char *why;
			switch (bail) {
			case 0:  why = slot == -1 ? "aot-backed" : "not-yet-jitted"; break;
			case -2: why = "EH-clauses"; break;
			case -3: why = "arg/ret-type"; break;
			case -4: why = "other-ir-shape"; break;
			case -5: why = "ldaddr"; break;
			case -6: why = "lcompare"; break;
			case -7: why = "byref"; break;
			case -8: why = "gshared/rgctx"; break;
			case -9: why = "synchronized"; break;
			case -10: why = "eh-other"; break;
			case -11: why = "island-blocked(perm-leaf)"; break;
			default: why = bail > 0 ? "opcode" : "?"; break;
			}
			printf ("  %8d  %-18s (slot=%d bail=%d) %s\n", bestc, why, slot, bail, wj_block_name [best] ? wj_block_name [best] : "?");
		}
		lastc = bestc; lasti = best; shown++;
	}
	fflush (stdout);
}

enum {
	WASM_JIT_COMPILE_PERM = -1,
	WASM_JIT_COMPILE_BLOCKED = 0,
	WASM_JIT_COMPILE_JITTED = 1,
	WASM_JIT_COMPILE_BUSY = 2
};

/* Compile im->method to wasm and, on success, publish the result onto its InterpMethod (so callers
 * see its f-slot). Returns 1 = JITted+published; 0 = retriable bail ("callee not jitted"; the full
 * un-JITted-callee set is in OUT->blockers); 2 = transient retry (another thread was compiling);
 * -1 = permanent bail. OUT (may be NULL) receives the emit result by value — the blocker set rides
 * on it, not thread-locals. */
static int
wasm_jit_compile_publish (InterpMethod *im, MonoWasmJitResult *out)
{
	extern void mono_wasm_force_compile (MonoMethod *m, MonoWasmJitResult *out);
	extern gboolean mono_wasm_jit_name_denied (const char *name);
	MonoWasmJitResult r;
	memset (&r, 0, sizeof (r));
	if (out)
		memset (out, 0, sizeof (*out));
	if (im->method->name && mono_wasm_jit_name_denied (im->method->name))
		return -1;
	/* serialize compilation (see wj_compiling): skip + retry if another thread (or a re-entrant cctor
	 * compile on this thread) holds it. Non-blocking, so it can't deadlock the GC. */
	if (mono_atomic_cas_i32 (&wj_compiling, 1, 0) != 0)
		return WASM_JIT_COMPILE_BUSY;
	/* Re-check the "already JITted" gate NOW that we hold the lock. All callers test im->wasm_jit_fslot>0
	 * before getting here, but that check is stale by the time we win the CAS: a second worker can pass its
	 * (fslot==0) guard while THIS method is being compiled by another thread, then reach its own CAS only
	 * after that thread published fslot + released the lock — the lock serializes compiles but doesn't
	 * un-stale a guard already passed. Without this re-check that worker recompiles the same method, leaking
	 * a fresh global table-slot pair and re-emitting identical bytes (the duplicate WASM_JIT_REGISTERED). */
	if (im->wasm_jit_fslot > 0) {
		mono_atomic_store_i32 (&wj_compiling, 0);
		if (out) {
			out->e_slot = im->wasm_jit_slot;
			out->f_slot = im->wasm_jit_fslot;
			out->bytes = im->wasm_jit_bytes;
			out->bytes_len = im->wasm_jit_bytes_len;
		}
		return WASM_JIT_COMPILE_JITTED;
	}
	mono_wasm_force_compile (im->method, &r);
	mono_atomic_store_i32 (&wj_compiling, 0);
	if (out)
		*out = r;
	if (r.e_slot > 0) {
		im->wasm_jit_fslot = r.f_slot;
		im->wasm_jit_bytes = r.bytes;
		im->wasm_jit_bytes_len = r.bytes_len;
		mono_memory_barrier ();   /* cross-thread: publish bytes/fslot before the >0 slot gate other threads test */
		im->wasm_jit_slot = r.e_slot;
		wj_waiter_drain (im->method);   /* event-driven wake: re-queue any methods parked waiting on this callee */
		return WASM_JIT_COMPILE_JITTED;
	}
	if (r.retriable) {
		/* retriable = blocked by un-JITted callee(s). Record each blocker (block_n is always counted as the
		 * Lever C cold-gate signal; the top-N report table is populated only under stats — see wj_block_note). */
		int i;
		for (i = 0; i < r.nblockers; i++)
			wj_block_note (r.blockers [i]);
		return WASM_JIT_COMPILE_BLOCKED;
	}
	im->wasm_jit_bail = (gint16) r.bail;   /* permanent bail: record why, for the vcall-residual breakdown */
	return WASM_JIT_COMPILE_PERM;
}

/* Lever C cold gate: should this blocking callee be SKIPPED as too cold to pull into the island right now?
 * Pull it (return FALSE) if it's hot by its own interp hit count, or it has BLOCKED >= MONO_WASM_JIT_BLOCK_PROMOTE
 * island attempts (block_n: a hot-path callee reached mostly via JITted callers, whose interp hits don't grow —
 * the hot-ctor blind spot), or the promoted-root relaxation applies. block_n is always counted (wj_block_note),
 * so this works without MONO_WASM_JIT_STATS; block_promote==0 disables the block_n relaxation (hits-only). */
static gboolean
wj_blocker_too_cold (InterpMethod *cim, int depth, gboolean promoted_root)
{
	extern int mono_wasm_jit_thresh, mono_wasm_jit_block_promote, mono_wasm_jit_island_cold_div,
		mono_wasm_jit_promoted_cold_div, mono_wasm_jit_promoted_root_uncold_depth;
	int cold_div = mono_wasm_jit_island_cold_div > 0 ? mono_wasm_jit_island_cold_div : 4;
	int cold_thresh = mono_wasm_jit_thresh / cold_div;
	if (promoted_root) {
		if (depth < mono_wasm_jit_promoted_root_uncold_depth)
			cold_thresh = 0;
		else {
			int promoted_div = mono_wasm_jit_promoted_cold_div > 0 ? mono_wasm_jit_promoted_cold_div : cold_div;
			cold_thresh = mono_wasm_jit_thresh / promoted_div;
		}
	}
	return cim->wasm_jit_hits < cold_thresh
		&& !(mono_wasm_jit_block_promote > 0 && cim->wasm_jit_block_n >= mono_wasm_jit_block_promote);
}

/* Eagerly form a JIT island rooted at m: compile it; if it bails because DIRECT callees aren't JITted
 * (the residual=0 islands policy), recursively compile ALL of those callees (the full set is enumerated
 * by the emitter's pre-scan into res.blockers — see wj_prescan_blockers) and re-emit — so a hot method's
 * whole call-tree JITs in one shot instead of slowly bottom-up over many threshold-crosses + retries, and
 * in ONE emit cycle per layer rather than one re-emit PER blocking callee. Bounded by depth + a shared
 * compile budget so a pathological call graph can't run away. Returns the compile_publish code for m
 * (1/0/2/-1). */
#define WASM_JIT_ISLAND_MAX_DEPTH 10

/* Per-thread DFS stack of methods currently being force-compiled, used ONLY to recognise a call CYCLE (a
 * blocker that is an ancestor) so we can batch-compile the whole strongly-connected set. This is NOT the old
 * give-up "cycle detector" (which marked cyclic SCCs permanently un-JITtable); it feeds wasm_jit_compile_scc,
 * which actually JITs the cycle. */
#define WJ_SCC_MAX 64
static __thread MonoMethod *wj_scc_stack [WJ_SCC_MAX];
static __thread int wj_scc_n;

/* Batch-compile a strongly-connected set of mutually-recursive methods (a call cycle) that can't be closed
 * one-at-a-time under residual=0 — to emit A (which direct-calls B) B needs an f-slot, and vice versa, so
 * neither can be first. Reserve an e/f-slot pair for EVERY member up-front (published on the imethod, where
 * get_callee_fslot finds it), so each member's emit bakes the others' reserved f-slots. Then register all
 * (bytes land in wj_reg) BEFORE publishing any as invocable — the runtime's per-call ensure_fslot backstop
 * instantiates a specific registered slot on demand, so cross-cycle calls resolve regardless of order once
 * every member is registered; the only hard requirement is that no member is invocable (wasm_jit_slot set)
 * until all are registered. `members` points into wj_scc_stack. Returns compile_publish codes (1/0/2/-1). */
static int
wasm_jit_compile_scc (MonoMethod **seed, int n_init, int *budget)
{
	extern void mono_wasm_force_compile (MonoMethod *m, MonoWasmJitResult *out);
	extern int mono_jiterp_allocate_table_entry (int type);
	extern int mono_wasm_jit_verbose;
	MonoMethod *members [WJ_SCC_MAX];
	MonoWasmJitResult results [WJ_SCC_MAX];
	gboolean done [WJ_SCC_MAX];
	int n, i, iter;
	gboolean ok = TRUE, give_up = FALSE;   /* give_up: abort is terminal (perm dep / too big / stuck) vs transient (budget/table -> retry) */

	if (n_init <= 0 || n_init > WJ_SCC_MAX)
		return 0;
	/* Serialize the whole batch: force_compile (unlike compile_publish) does NOT take wj_compiling; a nested
	 * compile on another thread would corrupt the shared resv fields / slot allocation. Non-blocking: retry. */
	if (mono_atomic_cas_i32 (&wj_compiling, 1, 0) != 0)
		return WASM_JIT_COMPILE_BUSY;

	n = n_init;
	for (i = 0; i < n; i++) { members [i] = seed [i]; done [i] = FALSE; memset (&results [i], 0, sizeof (results [i])); }
	/* Phase 0 — reserve a slot pair for every seed member so cross-cycle emits can bake each other's f-slot. */
	for (i = 0; i < n; i++) {
		InterpMethod *im = mono_interp_get_imethod (members [i]);
		if (im->wasm_jit_fslot > 0) { done [i] = TRUE; continue; }
		if (im->wasm_jit_slot == -1) { ok = FALSE; give_up = TRUE; goto out; }   /* seed member permanently un-JITtable -> can't close */
		if (im->wasm_jit_resv_fslot == 0) {
			int e = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			int f = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			if (e <= 0 || f <= 0) { ok = FALSE; goto out; }
			im->wasm_jit_resv_eslot = e; im->wasm_jit_resv_fslot = f;
		}
	}
	/* Phase 1 — grow to a fixpoint. Compile each not-done member INTO its reserved slots (force_compile; the
	 * emitter picks up the reservation via mono_wasm_jit_self_reserved). A member that bails still needs an
	 * un-reserved callee — a not-yet-JITted method the whole cycle transitively depends on: FOLD it into the
	 * batch (reserve + add), then re-compile. Repeat until every member compiles OK (all its callees reserved
	 * or live), or a callee is PERMANENTLY un-JITtable / the closure exceeds the size cap -> abort. The
	 * self-recursion fast path handles pure self-loops; this handles arbitrary multi-method SCCs + their
	 * uncompiled dependency closure (e.g. the $Gson$Types canonicalize <-> *TypeImpl.ctor cluster). */
	for (iter = 0; iter < WJ_SCC_MAX * 2; iter++) {
		gboolean progress = FALSE, all_done = TRUE;
		for (i = 0; i < n; i++) {
			InterpMethod *im;
			int b;
			if (done [i]) continue;
			all_done = FALSE;
			im = mono_interp_get_imethod (members [i]);
			if (im->wasm_jit_fslot > 0) { done [i] = TRUE; progress = TRUE; continue; }
			if (*budget <= 0) { ok = FALSE; goto out; }
			(*budget)--;
			memset (&results [i], 0, sizeof (results [i]));
			mono_wasm_force_compile (members [i], &results [i]);
			if (results [i].e_slot > 0) { done [i] = TRUE; progress = TRUE; continue; }
			for (b = 0; b < results [i].nblockers; b++) {
				MonoMethod *bm = results [i].blockers [b];
				InterpMethod *bim = mono_interp_get_imethod (bm);
				int j, seen = 0;
				if (bim->wasm_jit_fslot > 0 || bim->wasm_jit_resv_fslot > 0) continue;   /* live or already a member */
				if (bim->wasm_jit_slot == -1) { ok = FALSE; give_up = TRUE; goto out; }  /* permanent dependency -> can't close */
				for (j = 0; j < n; j++) if (members [j] == bm) { seen = 1; break; }
				if (seen) continue;
				if (n >= WJ_SCC_MAX) { ok = FALSE; give_up = TRUE; goto out; }           /* closure too large -> abort (perm) */
				{ int e = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
				  int f = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
				  if (e <= 0 || f <= 0) { ok = FALSE; goto out; }
				  bim->wasm_jit_resv_eslot = e; bim->wasm_jit_resv_fslot = f; }
				members [n] = bm; done [n] = FALSE; memset (&results [n], 0, sizeof (results [n])); n++;
				progress = TRUE;
			}
		}
		if (all_done) goto out;         /* ok stays TRUE */
		if (!progress) { ok = FALSE; give_up = TRUE; goto out; }   /* stuck: a member bailed with no growable blocker (perm-ish) */
	}
	ok = FALSE;   /* iteration cap hit without closing */
out:
	if (ok) {
		/* Publish all. Every member is registered now, so once invocable a baked cross-cycle call_indirect
		 * resolves via ensure_fslot. Set fslot before the wasm_jit_slot gate (cross-thread visibility). */
		for (i = 0; i < n; i++) {
			InterpMethod *im = mono_interp_get_imethod (members [i]);
			if (im->wasm_jit_fslot > 0) { im->wasm_jit_resv_eslot = 0; im->wasm_jit_resv_fslot = 0; continue; }
			im->wasm_jit_bytes = results [i].bytes;
			im->wasm_jit_bytes_len = results [i].bytes_len;
			mono_memory_barrier ();
			im->wasm_jit_fslot = results [i].f_slot;
			mono_memory_barrier ();
			im->wasm_jit_slot = results [i].e_slot;
			im->wasm_jit_resv_eslot = 0; im->wasm_jit_resv_fslot = 0;
		}
		mono_atomic_store_i32 (&wj_compiling, 0);
		for (i = 0; i < n; i++)
			wj_waiter_drain (members [i]);
		if (mono_wasm_jit_verbose >= 2) printf ("WASM_JIT_SCC_OK members=%d\n", n);
		return WASM_JIT_COMPILE_JITTED;
	}
	/* Abort. Always clear reservations. If the abort is TERMINAL (give_up: a permanently un-JITtable dependency,
	 * the closure exceeds the size cap, or a member is stuck), mark EVERY member permanent so they stop
	 * re-attempting — no compile storm — and run in the interpreter, matching the stable pre-batch behaviour for
	 * un-closeable cycles. If it is TRANSIENT (budget/table/iteration cap), leave them retriable so a later
	 * attempt with fresh budget can close the cycle. Members already force-compiled into their reserved slots
	 * stay registered-but-unpublished (a bounded one-time orphan; they never become invocable). */
	for (i = 0; i < n; i++) {
		InterpMethod *im = mono_interp_get_imethod (members [i]);
		im->wasm_jit_resv_eslot = 0;
		im->wasm_jit_resv_fslot = 0;
		if (give_up && im->wasm_jit_fslot <= 0) {   /* don't demote one that raced to live elsewhere */
			im->wasm_jit_bail = -11;
			im->wasm_jit_slot = -1;
		}
	}
	mono_atomic_store_i32 (&wj_compiling, 0);
	if (give_up) {
		if (mono_wasm_jit_verbose >= 1) printf ("WASM_JIT_SCC_PERM members=%d (un-closeable cycle -> interp)\n", n);
		return WASM_JIT_COMPILE_PERM;
	}
	if (mono_wasm_jit_verbose >= 2) printf ("WASM_JIT_SCC_RETRY members=%d (transient: budget/table)\n", n);
	return WASM_JIT_COMPILE_BLOCKED;
}

/* Eagerly form a JIT island rooted at m: compile it; recursively compile its un-JITted DIRECT callees (the
 * residual=0 islands policy) and re-emit. Self-recursion is handled in the emitter (self-slot reservation).
 * A multi-method CYCLE (a blocker that is an ancestor on the DFS stack) is handed to wasm_jit_compile_scc,
 * which reserves the whole SCC's slots and compiles+publishes them atomically. Bounded by depth + budget.
 * Returns the compile_publish code for m (1/0/2/-1). */
static int
wasm_jit_force_island (MonoMethod *m, int depth, int *budget, gboolean promoted_root)
{
	extern int mono_wasm_jit_island_depth;   /* Lever C: env-tunable recursion depth (default 10) */
	InterpMethod *im = mono_interp_get_imethod (m);
	int tries, spos, pushed = 0, ret = 0;
	if (im->wasm_jit_fslot > 0) return 1;          /* already JITted */
	if (im->wasm_jit_slot == -1) return -1;        /* permanently bailed */
	if (depth > mono_wasm_jit_island_depth) { if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_DEPTH_EXCEEDED); return 0; }
	/* Push m so a deeper frame can see a cycle back to it (wj_scc_stack). */
	spos = wj_scc_n;
	if (wj_scc_n < WJ_SCC_MAX) { wj_scc_stack [wj_scc_n++] = m; pushed = 1; }
	for (tries = 0; tries <= WASM_JIT_ISLAND_MAX_DEPTH; tries++) {
		MonoWasmJitResult res;
		int r, i, pulled = 0, min_cyc = -1;
		if (*budget <= 0) { if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_BUDGET_EXHAUSTED); ret = 0; goto pop; }
		(*budget)--;
		r = wasm_jit_compile_publish (im, &res);
		if (r == WASM_JIT_COMPILE_BUSY) { ret = r; goto pop; }  /* transient retry: don't park as if a blocker event is pending */
		if (r != WASM_JIT_COMPILE_BLOCKED) { ret = r; goto pop; } /* JITted (1) or permanent (-1) */
		if (res.nblockers == 0) { ret = WASM_JIT_COMPILE_BUSY; goto pop; } /* no blocker recorded -> transient retry */
		/* Pull EVERY hot NON-cyclic blocking callee into the island this pass; defer cyclic (ancestor) blockers
		 * to the SCC batch below; leave cold ones for a later attempt. */
		for (i = 0; i < res.nblockers; i++) {
			MonoMethod *callee = res.blockers [i];
			InterpMethod *cim;
			int _r, j, on_stack = -1;
			if (callee == m) continue;                 /* self-recursion: handled by the emitter's self-slot reservation */
			cim = mono_interp_get_imethod (callee);
			if (cim->wasm_jit_fslot > 0) continue;     /* already JITted (e.g. pulled via an earlier blocker's recursion) */
			if (cim->wasm_jit_slot == -1) {
				/* Callee can NEVER wasm-jit (slot==-1). Under residual=0 our island can't close around it: give up
				 * PERMANENTLY and propagate a transitive-permanent sentinel (bail=-11) so OUR callers stop too. */
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_BLOCKED_PERM);
				im->wasm_jit_bail = -11;
				ret = -1; goto pop;
			}
			for (j = 0; j < wj_scc_n; j++) if (wj_scc_stack [j] == callee) { on_stack = j; break; }
			if (on_stack >= 0) {   /* callee is an ANCESTOR on the DFS path -> m..callee close a call cycle (an SCC) */
				if (min_cyc < 0 || on_stack < min_cyc) min_cyc = on_stack;
				continue;   /* defer: batch-compile the whole SCC once non-cyclic blockers are resolved */
			}
			if (wj_blocker_too_cold (cim, depth, promoted_root)) {
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_BLOCKED_COLD);
				wj_waiter_register (callee, m);   /* park m on this cold callee: re-attempt when it JITs */
				continue;
			}
			_r = wasm_jit_force_island (callee, depth + 1, budget, promoted_root);
			if (_r < 0) { im->wasm_jit_bail = -11; ret = -1; goto pop; }   /* callee permanently un-closable -> so are we */
			if (_r == WASM_JIT_COMPILE_BUSY) { ret = _r; goto pop; }
			if (_r > 0) { pulled++; if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_PROMOTED_DOWN); }
			else wj_waiter_register (callee, m);   /* callee still warming (its own blockers cold) -> wake m when it JITs */
			if (*budget <= 0) break;
		}
		if (pulled > 0)
			continue;   /* re-emit m with the pulled callees now f-slotted */
		if (min_cyc >= 0) {
			/* A blocker is an ancestor -> m is in a call cycle. Batch-compile the SCC seeded by the DFS-stack
			 * segment [min_cyc .. top]; wasm_jit_compile_scc grows it to the full uncompiled closure and either
			 * publishes every member or marks them permanent (un-closeable). Hot non-cyclic blockers were already
			 * pulled above; any cold ones get folded into the batch. */
			ret = wasm_jit_compile_scc (&wj_scc_stack [min_cyc], wj_scc_n - min_cyc, budget);
			goto pop;
		}
		ret = 0;   /* all remaining blockers cold/warming -> stay retriable (parked as a waiter) */
		goto pop;
	}
	ret = 0;
pop:
	if (pushed) wj_scc_n = spos;
	return ret;
}

/* Auto-JIT hotness trigger for a callee (MONO_WASM_JIT_AUTO): count calls; at the threshold compile it
 * to wasm (eagerly forming its call-tree island unless MONO_WASM_JIT_ISLAND=0). Slot states: 0=untried
 * (counting); >0=JITted; -1=permanent bail; WASM_JIT_SLOT_PARKED (-2)=blocked on a cold callee, waiting for
 * an event (a blocker JITs -> wj_waiter_drain) rather than threshold/promotion retry; WASM_JIT_SLOT_RETRY
 * (-3)=transient retry (another thread was compiling), still eligible for coarse retries. Shared by MINT_CALL
 * (direct) and MINT_CALLVIRT_FAST (virtual) dispatch so virtual targets — the bulk of hot IKVM methods — also
 * JIT. */
/* Drain the promotion queue (wj_promote_q) at this safe point: force-JIT a bounded number of queued methods.
 * The queue is fed by Lever A (hot interp CALLERS, MONO_WASM_JIT_ENTRY_PROMOTE), by BLOCK_PROMOTE/block_force
 * (a callee that blocks many islands), and — the event-driven core of the revamp — by wj_waiter_drain (methods
 * woken because a blocker they were parked on just JITted). Runs whenever the queue is non-empty (not gated on
 * ENTRY_PROMOTE, so waiter wakes drain even with Lever A off). Force-compiling a method that's currently being
 * interpreted is safe — the live frame keeps interpreting; the f-slot is used on the next entry. */
static void
wasm_jit_drain_promotions (void)
{
	extern int mono_wasm_jit_auto, mono_wasm_jit_island_budget, mono_wasm_jit_promotion_drain;
	int n;
	if (mono_wasm_jit_auto <= 0 || wj_promote_head == wj_promote_tail)
		return;
	for (n = 0; n < mono_wasm_jit_promotion_drain; ++n) {
		gint32 h = wj_promote_head;
		MonoMethod *pm;
		InterpMethod *pim;
		int budget;
		if (h == wj_promote_tail) break;
		pm = wj_promote_q [h & (WJ_PROMOTE_Q - 1)];
		wj_promote_head = h + 1;
		if (!pm) continue;
		pim = mono_interp_get_imethod (pm);
		if (pim->wasm_jit_fslot > 0 || pim->wasm_jit_slot == -1) continue;   /* already done / hopeless */
		budget = mono_wasm_jit_island_budget;
		{
			int r = wasm_jit_force_island (pm, 0, &budget, TRUE);
			int s = pim->wasm_jit_slot;
			if (r == WASM_JIT_COMPILE_JITTED) {
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_PROMOTED_UP);
			} else if (s > 0) {
				/* Race: another thread published a real e-slot (>0) between our gate check and here. NEVER lower
				 * a live positive slot — the interp invoke path would call_indirect a wrong/placeholder slot and
				 * trap the worker. Leave it intact. */
			} else if (s == -1) {
				/* Another thread marked it permanent while we were attempting it. */
			} else if (r == WASM_JIT_COMPILE_BLOCKED) {
				pim->wasm_jit_slot = WASM_JIT_SLOT_PARKED;   /* parked: woken by wj_waiter_drain when a blocker JITs */
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_PARKED);
			} else if (r == WASM_JIT_COMPILE_BUSY) {
				pim->wasm_jit_slot = WASM_JIT_SLOT_RETRY;    /* transient retry: NOT waiter-parked, no blocker event pending */
			} else {
				pim->wasm_jit_slot = -1;       /* permanent (transitive perm blocker, bail=-11) */
				wj_waiter_drain (pm);          /* wake anyone parked on pm so they discover the permanence */
			}
		}
	}
}

static void
wasm_jit_maybe_compile (InterpMethod *cmethod)
{
	extern int mono_wasm_jit_auto, mono_wasm_jit_thresh, mono_wasm_jit_island, mono_wasm_jit_island_budget;
	wasm_jit_drain_promotions ();   /* Lever A: upward island growth for hot interp callers */
	if (G_UNLIKELY (mono_wasm_jit_auto > 0) && wj_slot_hot_retry_eligible (cmethod->wasm_jit_slot) && ++cmethod->wasm_jit_hits == mono_wasm_jit_thresh) {
		int r;
		/* Don't whole-method-JIT a method that already has AOT code — it runs faster as native AOT, reached
		 * via do_jit_call (the residual / vcall-fallback now routes AOT'd callees there). Mark permanent so
		 * we stop counting + stop wasting compile attempts (+ ldaddr bails) on already-compiled code. */
		extern int mono_wasm_jit_aot_residual;
		if (mono_wasm_jit_aot_residual && mono_interp_jit_call_supported (cmethod->method, mono_method_signature_internal (cmethod->method))) {
			cmethod->wasm_jit_slot = -1;
			return;
		}
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_ATTEMPT);
		if (mono_wasm_jit_island) {
			extern int mono_wasm_jit_hot_root;   /* MONO_WASM_JIT_HOT_ROOT: this method just crossed its own thresh (proven hot) -> build its island as a promoted root so the cold gate is relaxed and its private (blind-spot ~0-hit) callees get pulled in instead of parking it forever */
			int budget = mono_wasm_jit_island_budget;   /* Lever C: env-tunable; max force-compiles per island attempt */
			r = wasm_jit_force_island (cmethod->method, 0, &budget, mono_wasm_jit_hot_root ? TRUE : FALSE);
		} else {
			r = wasm_jit_compile_publish (cmethod, NULL);
		}
		{ extern int mono_wasm_jit_verbose;
		  if (G_UNLIKELY (mono_wasm_jit_verbose >= 2)) {
			/* Island routing outcome. r: -1=PERM 0=BLOCKED(->PARK) 1=JITTED 2=BUSY(->RETRY, NOT parked).
			 * A hot method that keeps printing r=2 here is the retry storm — it never parks, so it
			 * re-emits every ~threshold calls. slot/hits are the pre-branch values (branch updates them below). */
			char *_n = mono_method_get_full_name (cmethod->method);
			printf ("WASM_JIT_ISLAND %s -> r=%d (slot=%d hits=%d)\n", _n, r, cmethod->wasm_jit_slot, cmethod->wasm_jit_hits);
			g_free (_n);
		  } }
		if (r == WASM_JIT_COMPILE_JITTED) {
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_ISLAND_COMPLETED);
			return;   /* JITted + published */
		} else if (r == WASM_JIT_COMPILE_BLOCKED) {
			/* retriable: the island couldn't close because a callee is COLD/WARMING (not permanently
			 * un-jittable — force_island returns -1 + bail=-11 for those). force_island has registered this
			 * method as a WAITER on each cold blocker (wj_waiter_register), so it's re-attempted (via
			 * wj_promote_q) the moment a blocker JITs — event-driven, NO busy -2..-5 poll. PARK it; reset hits
			 * so the coarse fallback re-attempt (only if no wake ever fires) is a full ~thresh calls away. */
			if (wj_slot_retriable (cmethod->wasm_jit_slot)) {   /* race guard: never overwrite a slot another thread published >0 / -1 */
				cmethod->wasm_jit_slot = WASM_JIT_SLOT_PARKED;
				cmethod->wasm_jit_hits = 0;
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_PARKED);
			}
		} else if (r == WASM_JIT_COMPILE_BUSY) {
			/* Another thread held the compile lock. Keep this distinct from a waiter-parked blocker state so
			 * event-driven sleepers don't re-enter threshold/promotion retries. */
			if (wj_slot_retriable (cmethod->wasm_jit_slot)) {
				cmethod->wasm_jit_slot = WASM_JIT_SLOT_RETRY;
				cmethod->wasm_jit_hits = 0;
			}
		} else {
			if (wj_slot_retriable (cmethod->wasm_jit_slot)) {   /* same race: don't de-JIT a method another thread just published */
				cmethod->wasm_jit_slot = -1;   /* permanent: emitter bail, or force_island hit a permanent blocker (bail=-11) */
				wj_waiter_drain (cmethod->method);   /* wake anyone parked on us so they discover the permanence */
			}
		}
	}
}
#endif

static InterpMethod*
get_virtual_method (InterpMethod *imethod, MonoVTable *vtable)
{
	MonoMethod *m = imethod->method;

	if ((m->flags & METHOD_ATTRIBUTE_FINAL) || !(m->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
		if (m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
			return mono_interp_get_imethod (mono_marshal_get_synchronized_wrapper (m));
		else
			return imethod;
	}

	mono_class_setup_vtable (vtable->klass);

	int slot = mono_method_get_vtable_slot (m);
	if (mono_class_is_interface (m->klass)) {
		g_assert (vtable->klass != m->klass);
		/* TODO: interface offset lookup is slow, go through IMT instead */
		gboolean non_exact_match;
		int ioffset = mono_class_interface_offset_with_variance (vtable->klass, m->klass, &non_exact_match);
		g_assert (ioffset >= 0);
		slot += ioffset;
	}

	MonoMethod *virtual_method = m_class_get_vtable (vtable->klass) [slot];
	g_assert (virtual_method);

	if (m->is_inflated && mono_method_get_context (m)->method_inst) {
		MonoGenericContext context = { NULL, NULL };

		if (mono_class_is_ginst (virtual_method->klass))
			context.class_inst = mono_class_get_generic_class (virtual_method->klass)->context.class_inst;
		else if (mono_class_is_gtd (virtual_method->klass))
			context.class_inst = mono_class_get_generic_container (virtual_method->klass)->context.class_inst;
		context.method_inst = mono_method_get_context (m)->method_inst;

		ERROR_DECL (error);
		virtual_method = mono_class_inflate_generic_method_checked (virtual_method, &context, error);
		mono_error_cleanup (error); /* FIXME: don't swallow the error */
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
		virtual_method = mono_marshal_get_native_wrapper (virtual_method, FALSE, FALSE);
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
		virtual_method = mono_marshal_get_synchronized_wrapper (virtual_method);
	}

	InterpMethod *virtual_imethod = mono_interp_get_imethod (virtual_method);
	return virtual_imethod;
}

typedef struct {
	InterpMethod *imethod;
	InterpMethod *target_imethod;
} InterpVTableEntry;

/* memory manager lock must be held */
static GSList*
append_imethod (MonoMemoryManager *memory_manager, GSList *list, InterpMethod *imethod, InterpMethod *target_imethod)
{
	GSList *ret;
	InterpVTableEntry *entry;

	entry = (InterpVTableEntry*) mono_mem_manager_alloc0 (memory_manager, sizeof (InterpVTableEntry));
	entry->imethod = imethod;
	entry->target_imethod = target_imethod;
	ret = mono_mem_manager_alloc0 (memory_manager, sizeof (GSList));
	ret->data = entry;
	ret = g_slist_concat (list, ret);

	mono_interp_register_imethod_patch_site ((gpointer*)&entry->imethod);
	mono_interp_register_imethod_patch_site ((gpointer*)&entry->target_imethod);

	return ret;
}

static InterpMethod*
get_target_imethod (GSList *list, InterpMethod *imethod)
{
	while (list != NULL) {
		InterpVTableEntry *entry = (InterpVTableEntry*) list->data;
		// We don't account for tiering here so this comparison is racy
		// The side effect is that we might end up with duplicates of the same
		// method in the vtable list, but this is extremely uncommon.
		if (entry->imethod == imethod)
			return entry->target_imethod;
		list = list->next;
	}
	return NULL;
}

static inline MonoVTableEEData*
get_vtable_ee_data (MonoVTable *vtable)
{
	MonoVTableEEData *ee_data = (MonoVTableEEData*)vtable->ee_data;

	if (G_UNLIKELY (!ee_data)) {
		ee_data = m_class_alloc0 (vtable->klass, sizeof (MonoVTableEEData));
		mono_memory_barrier ();
		vtable->ee_data = ee_data;
	}
	return ee_data;
}

static gpointer*
get_method_table (MonoVTable *vtable, int offset)
{
	if (offset >= 0)
		return get_vtable_ee_data (vtable)->interp_vtable;
	else
		return (gpointer*)vtable;
}

static gpointer*
alloc_method_table (MonoVTable *vtable, int offset)
{
	gpointer *table;

	if (offset >= 0) {
		table = (gpointer*)m_class_alloc0 (vtable->klass, m_class_get_vtable_size (vtable->klass) * sizeof (gpointer));
		get_vtable_ee_data (vtable)->interp_vtable = table;
	} else {
		table = (gpointer*)vtable;
	}

	return table;
}

static InterpMethod* // Inlining causes additional stack use in caller.
get_virtual_method_fast (InterpMethod *imethod, MonoVTable *vtable, int offset)
{
	gpointer *table;
	MonoMemoryManager *memory_manager = NULL;

	table = get_method_table (vtable, offset);

	if (G_UNLIKELY (!table)) {
		memory_manager = m_class_get_mem_manager (vtable->klass);
		/* Lazily allocate method table */
		mono_mem_manager_lock (memory_manager);
		table = get_method_table (vtable, offset);
		if (!table)
			table = alloc_method_table (vtable, offset);
		mono_mem_manager_unlock (memory_manager);
	}

	if (G_UNLIKELY (!table [offset])) {
		InterpMethod *target_imethod = get_virtual_method (imethod, vtable);
		if (!memory_manager)
			memory_manager = m_class_get_mem_manager (vtable->klass);
		/* Lazily initialize the method table slot */
		mono_mem_manager_lock (memory_manager);
		if (!table [offset]) {
			if (imethod->method->is_inflated || offset < 0) {
				table [offset] = append_imethod (memory_manager, NULL, imethod, target_imethod);
			} else {
				table [offset] = (gpointer) ((gsize)target_imethod | 0x1);
				mono_interp_register_imethod_patch_site (&table [offset]);
			}
		}
		mono_mem_manager_unlock (memory_manager);
	}

	if ((gsize)table [offset] & 0x1) {
		/* Non generic virtual call. Only one method in slot */
		return (InterpMethod*) ((gsize)table [offset] & ~0x1);
	} else {
		/* Virtual generic or interface call. Multiple methods in slot */
		InterpMethod *target_imethod = get_target_imethod ((GSList*)table [offset], imethod);

		if (G_UNLIKELY (!target_imethod)) {
			target_imethod = get_virtual_method (imethod, vtable);
			if (!memory_manager)
				memory_manager = m_class_get_mem_manager (vtable->klass);
			mono_mem_manager_lock (memory_manager);
			if (!get_target_imethod ((GSList*)table [offset], imethod))
				table [offset] = append_imethod (memory_manager, (GSList*)table [offset], imethod, target_imethod);
			mono_mem_manager_unlock (memory_manager);
		}
		return target_imethod;
	}
}

static void
stackval_from_data (MonoType *type, stackval *result, const void *data, gboolean pinvoke)
{
	if (m_type_is_byref (type)) {
		result->data.p = *(gpointer*)data;
		return;
	}
	switch (type->type) {
	case MONO_TYPE_VOID:
		break;;
	case MONO_TYPE_I1:
		result->data.i = *(gint8*)data;
		break;
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		result->data.i = *(guint8*)data;
		break;
	case MONO_TYPE_I2:
		result->data.i = *(gint16*)data;
		break;
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		result->data.i = *(guint16*)data;
		break;
	case MONO_TYPE_I4:
		result->data.i = *(gint32*)data;
		break;
	case MONO_TYPE_U:
	case MONO_TYPE_I:
		result->data.nati = *(mono_i*)data;
		break;
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		result->data.p = *(gpointer*)data;
		break;
	case MONO_TYPE_U4:
		result->data.i = *(guint32*)data;
		break;
	case MONO_TYPE_R4:
		/* memmove handles unaligned case */
		memmove (&result->data.f_r4, data, sizeof (float));
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		memmove (&result->data.l, data, sizeof (gint64));
		break;
	case MONO_TYPE_R8:
		memmove (&result->data.f, data, sizeof (double));
		break;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
		result->data.p = *(gpointer*)data;
		break;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (m_type_data_get_klass_unchecked (type))) {
			stackval_from_data (mono_class_enum_basetype_internal (m_type_data_get_klass_unchecked (type)), result, data, pinvoke);
			break;
		} else {
			int size;
			if (pinvoke)
				size = mono_class_native_size (m_type_data_get_klass_unchecked (type), NULL);
			else
				size = mono_class_value_size (m_type_data_get_klass_unchecked (type), NULL);
			memcpy (result, data, size);
			break;
		}
	case MONO_TYPE_GENERICINST: {
		if (mono_type_generic_inst_is_valuetype (type)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke)
				size = mono_class_native_size (klass, NULL);
			else
				size = mono_class_value_size (klass, NULL);
			memcpy (result, data, size);
			break;
		}
		stackval_from_data (m_class_get_byval_arg (m_type_data_get_generic_class_unchecked (type)->container_class), result, data, pinvoke);
		break;
	}
	default:
		g_error ("got type 0x%02x", type->type);
	}
}

static int
stackval_to_data (MonoType *type, stackval *val, void *data, gboolean pinvoke)
{
	if (m_type_is_byref (type)) {
		gpointer *p = (gpointer*)data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	/* printf ("TODAT0 %p\n", data); */
	switch (type->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1: {
		guint8 *p = (guint8*)data;
		*p = GINT32_TO_UINT8 (val->data.i);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR: {
		guint16 *p = (guint16*)data;
		*p = GINT32_TO_UINT16 (val->data.i);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I: {
		mono_i *p = (mono_i*)data;
		/* In theory the value used by stloc should match the local var type
	 	   but in practice it sometimes doesn't (a int32 gets dup'd and stloc'd into
		   a native int - both by csc and mcs). Not sure what to do about sign extension
		   as it is outside the spec... doing the obvious */
		*p = (mono_i)val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_U: {
		mono_u *p = (mono_u*)data;
		/* see above. */
		*p = (mono_u)val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I4:
	case MONO_TYPE_U4: {
		gint32 *p = (gint32*)data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I8:
	case MONO_TYPE_U8: {
		memmove (data, &val->data.l, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R4: {
		/* memmove handles unaligned case */
		memmove (data, &val->data.f_r4, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R8: {
		memmove (data, &val->data.f, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY: {
		gpointer *p = (gpointer *) data;
		mono_gc_wbarrier_generic_store_internal (p, val->data.o);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR: {
		gpointer *p = (gpointer *) data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (m_type_data_get_klass_unchecked (type))) {
			return stackval_to_data (mono_class_enum_basetype_internal (m_type_data_get_klass_unchecked (type)), val, data, pinvoke);
		} else {
			int size;
			if (pinvoke) {
				size = mono_class_native_size (m_type_data_get_klass_unchecked (type), NULL);
				memcpy (data, val, size);
			} else {
				size = mono_class_value_size (m_type_data_get_klass_unchecked (type), NULL);
				mono_value_copy_internal (data, val, m_type_data_get_klass_unchecked (type));
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
	case MONO_TYPE_GENERICINST: {
		MonoClass *container_class = m_type_data_get_generic_class_unchecked (type)->container_class;

		if (m_class_is_valuetype (container_class) && !m_class_is_enumtype (container_class)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke) {
				size = mono_class_native_size (klass, NULL);
				memcpy (data, val, size);
			} else {
				size = mono_class_value_size (klass, NULL);
				mono_value_copy_internal (data, val, klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_to_data (m_class_get_byval_arg (m_type_data_get_generic_class_unchecked (type)->container_class), val, data, pinvoke);
	}
	default:
		g_error ("got type %x", type->type);
	}
}

static void
stackval_to_data_sign_ext (MonoType *type, stackval *val, void *data, gboolean pinvoke)
{
	switch (type->type) {
		case MONO_TYPE_I1: {
			mono_i *p = (mono_i*)data;
			*p = (mono_i)((gint8)val->data.i);
			break;
		}
		case MONO_TYPE_U1: {
			mono_u *p = (mono_u*)data;
			*p = (mono_u)((guint8)val->data.i);
			break;
		}
		case MONO_TYPE_I2: {
			mono_i *p = (mono_i*)data;
			*p = (mono_i)((gint16)val->data.i);
			break;
		}
		case MONO_TYPE_U2: {
			mono_u *p = (mono_u*)data;
			*p = (mono_u)((guint16)val->data.i);
			break;
		}
#if SIZEOF_VOID_P == 8
		case MONO_TYPE_I4: {
			mono_i *p = (mono_i*)data;
			*p = (mono_i)(val->data.i);
			break;
		}
		case MONO_TYPE_U4: {
			mono_u *p = (mono_u*)data;
			*p = (mono_u)((guint32)val->data.i);
			break;
		}
#endif
		default:
			stackval_to_data (type, val, data, pinvoke);
			break;
	}
}

typedef struct {
	MonoException *ex;
	MonoContext *ctx;
} HandleExceptionCbData;

static void
handle_exception_cb (gpointer arg)
{
	HandleExceptionCbData *cb_data = (HandleExceptionCbData*)arg;

	mono_handle_exception (cb_data->ctx, (MonoObject*)cb_data->ex);
}

/*
 * interp_throw:
 *   Throw an exception from the interpreter.
 */
static MONO_NEVER_INLINE void
interp_throw (ThreadContext *context, MonoException *ex, InterpFrame *frame, const guint16* ip, gboolean rethrow)
{
	ERROR_DECL (error);
	MonoLMFExt ext;

	/*
	 * When explicitly throwing exception we pass the ip of the instruction that throws the exception.
	 * Offset the subtraction from interp_frame_get_ip, so we don't end up in prev instruction.
	 */
	frame->state.ip = ip + 1;

	// This LMF is pop'ed by the EH machinery before resuming
	interp_push_lmf (&ext, frame);

	if (mono_object_isinst_checked ((MonoObject *) ex, mono_defaults.exception_class, error)) {
		MonoException *mono_ex = ex;
		if (!rethrow) {
			mono_ex->stack_trace = NULL;
			mono_ex->trace_ips = NULL;
		}
	}
	mono_error_assert_ok (error);

	MonoContext ctx;
	memset (&ctx, 0, sizeof (MonoContext));
	MONO_CONTEXT_SET_SP (&ctx, frame);

	/*
	 * Call the JIT EH code. The EH code will call back to us using:
	 * - mono_interp_set_resume_state ()/run_finally ()/run_filter ().
	 * Since ctx.ip is 0, this will start unwinding from the LMF frame
	 * pushed above, which points to our frames.
	 */

	mono_handle_exception (&ctx, (MonoObject*)ex);

	if (MONO_CONTEXT_GET_IP (&ctx) != 0) {
		/* We need to unwind into non-interpreter code */
		mono_restore_context (&ctx);
		g_assert_not_reached ();
	}

	g_assert (context->has_resume_state);
}

static MONO_NEVER_INLINE MonoException *
interp_error_convert_to_exception (InterpFrame *frame, MonoError *error, const guint16 *ip)
{
	MonoLMFExt ext;
	MonoException *ex;

	/*
	 * When calling runtime functions we pass the ip of the instruction triggering the runtime call.
	 * Offset the subtraction from interp_frame_get_ip, so we don't end up in prev instruction.
	 */
	frame->state.ip = ip + 1;

	interp_push_lmf (&ext, frame);
	ex = mono_error_convert_to_exception (error);
	interp_pop_lmf (&ext);
	return ex;
}

#define INTERP_BUILD_EXCEPTION_TYPE_FUNC_NAME(prefix_name, type_name) \
prefix_name ## _ ## type_name

#define INTERP_GET_EXCEPTION(exception_type) \
static MONO_NEVER_INLINE MonoException * \
INTERP_BUILD_EXCEPTION_TYPE_FUNC_NAME(interp_get_exception, exception_type) (InterpFrame *frame, const guint16 *ip)\
{ \
	MonoLMFExt ext; \
	MonoException *ex; \
	frame->state.ip = ip + 1; \
	interp_push_lmf (&ext, frame); \
	ex = INTERP_BUILD_EXCEPTION_TYPE_FUNC_NAME(mono_get_exception,exception_type) (); \
	interp_pop_lmf (&ext); \
	return ex; \
}

#define INTERP_GET_EXCEPTION_CHAR_ARG(exception_type) \
static MONO_NEVER_INLINE MonoException * \
INTERP_BUILD_EXCEPTION_TYPE_FUNC_NAME(interp_get_exception, exception_type) (const char *arg, InterpFrame *frame, const guint16 *ip)\
{ \
	MonoLMFExt ext; \
	MonoException *ex; \
	frame->state.ip = ip + 1; \
	interp_push_lmf (&ext, frame); \
	ex = INTERP_BUILD_EXCEPTION_TYPE_FUNC_NAME(mono_get_exception,exception_type) (arg); \
	interp_pop_lmf (&ext); \
	return ex; \
}

INTERP_GET_EXCEPTION(null_reference)
INTERP_GET_EXCEPTION(divide_by_zero)
INTERP_GET_EXCEPTION(overflow)
INTERP_GET_EXCEPTION(invalid_cast)
INTERP_GET_EXCEPTION(index_out_of_range)
INTERP_GET_EXCEPTION(array_type_mismatch)
INTERP_GET_EXCEPTION(arithmetic)
INTERP_GET_EXCEPTION_CHAR_ARG(argument_out_of_range)

// Inlining throw logic into interp_exec_method makes it bigger and could push us up against
//  internal limits in things like WASM compilers
static MONO_NEVER_INLINE void
interp_throw_ex_general (
	MonoException *__ex, ThreadContext *context, InterpFrame *frame, const guint16 *ex_ip, gboolean rethrow
)
{
	HANDLE_FUNCTION_ENTER ();
	MonoExceptionHandle tmp_handle = MONO_HANDLE_NEW (MonoException, __ex);
	interp_throw (context, MONO_HANDLE_RAW(tmp_handle), (frame), (ex_ip), (rethrow));
	HANDLE_FUNCTION_RETURN ();
}

// We conservatively pin exception object here to avoid tweaking the
// numerous call sites of this macro, even though, in a few cases,
// this is not needed.
#define THROW_EX_GENERAL(exception,ex_ip, rethrow)		\
	do {							\
		interp_throw_ex_general (exception, context, frame, ex_ip, rethrow); \
		goto resume;							  \
	} while (0)

#define THROW_EX(exception,ex_ip) THROW_EX_GENERAL ((exception), (ex_ip), FALSE)

#define NULL_CHECK(o) do { \
	if (G_UNLIKELY (!(o))) \
		THROW_EX (interp_get_exception_null_reference (frame, ip), ip); \
	} while (0)

#define EXCEPTION_CHECKPOINT	\
	do {										\
		if (mono_thread_interruption_request_flag && !mono_threads_is_critical_method (frame->imethod->method)) { \
			MonoException *exc = mono_thread_interruption_checkpoint ();	\
			if (exc)							\
				THROW_EX_GENERAL (exc, ip, TRUE);					\
		}									\
	} while (0)

// Reduce duplicate code in mono_interp_exec_method
static MONO_NEVER_INLINE void
do_safepoint (InterpFrame *frame, ThreadContext *context, const guint16 *ip)
{
	MonoLMFExt ext;

	/*
	 * When calling runtime functions we pass the ip of the instruction triggering the runtime call.
	 * Offset the subtraction from interp_frame_get_ip, so we don't end up in prev instruction.
	 */
	frame->state.ip = ip + 1;

	interp_push_lmf (&ext, frame);
	/* Poll safepoint */
	mono_threads_safepoint ();
	interp_pop_lmf (&ext);
}

#define SAFEPOINT \
	do {						\
		if (G_UNLIKELY (mono_polling_required)) \
			do_safepoint (frame, context, ip);	\
	} while (0)

static MonoObject*
ves_array_create (MonoClass *klass, int param_count, stackval *values, MonoError *error)
{
	int rank = m_class_get_rank (klass);
	uintptr_t *lengths = g_newa (uintptr_t, rank * 2);
	intptr_t *lower_bounds = NULL;

	if (param_count > rank && m_class_get_byval_arg (klass)->type == MONO_TYPE_SZARRAY) {
		// Special constructor for jagged arrays
		for (int i = 0; i < param_count; ++i)
			lengths [i] = values [i].data.i;
		return (MonoObject*) mono_array_new_jagged_checked (klass, param_count, lengths, error);
	} else if (2 * rank == param_count) {
		for (int l = 0; l < 2; ++l) {
			int src = l;
			int dst = l * rank;
			for (int r = 0; r < rank; ++r, src += 2, ++dst) {
				lengths [dst] = values [src].data.i;
			}
		}
		/* lower bounds are first. */
		lower_bounds = (intptr_t *) lengths;
		lengths += rank;
	} else {
		/* Only lengths provided. */
		for (int i = 0; i < param_count; ++i) {
			lengths [i] = values [i].data.i;
		}
	}
	return (MonoObject*) mono_array_new_full_checked (klass, lengths, lower_bounds, error);
}

static gint32
ves_array_calculate_index (MonoArray *ao, stackval *sp, gboolean safe)
{
	MonoClass *ac = ((MonoObject *) ao)->vtable->klass;

	guint32 pos = 0;
	if (ao->bounds) {
		for (gint32 i = 0; i < m_class_get_rank (ac); i++) {
			gint32 idx = sp [i].data.i;
			gint32 lower = ao->bounds [i].lower_bound;
			guint32 len = ao->bounds [i].length;
			if (safe && (idx < lower || (guint32)(idx - lower) >= len))
				return -1;
			pos = (pos * len) + (guint32)(idx - lower);
		}
	} else {
		pos = sp [0].data.i;
		if (safe && pos >= ao->max_length)
			return -1;
	}
	return pos;
}

static MonoException*
ves_array_element_address (InterpFrame *frame, MonoClass *required_type, MonoArray *ao, gpointer *ret, stackval *sp, gboolean needs_typecheck)
{
	MonoClass *ac = ((MonoObject *) ao)->vtable->klass;

	g_assert (m_class_get_rank (ac) >= 1);

	gint32 pos = ves_array_calculate_index (ao, sp, TRUE);
	if (pos == -1)
		return mono_get_exception_index_out_of_range ();

	if (needs_typecheck && !mono_class_is_assignable_from_internal (m_class_get_element_class (mono_object_class ((MonoObject *) ao)), required_type))
		return mono_get_exception_array_type_mismatch ();
	gint32 esize = mono_array_element_size (ac);
	*ret = mono_array_addr_with_size_fast (ao, esize, pos);
	return NULL;
}

/* Does not handle `this` argument */
static guint32
compute_arg_offset (MonoMethodSignature *sig, int index)
{
	if (index == 0)
		return 0;

	guint32 offset = 0;
	int size, align;
	MonoType *type;
	for (int i = 0; i < index; i++) {
		type = sig->params [i];
		size = mono_interp_type_size (type, mono_mint_type (type), &align);

		offset = ALIGN_TO (offset, align);
		offset += size;
	}
	type = sig->params [index];
	mono_interp_type_size (type, mono_mint_type (type), &align);

	offset = ALIGN_TO (offset, align);
	return offset;
}

static gpointer
imethod_alloc0 (InterpMethod *imethod, guint size)
{
	if (imethod->method->dynamic)
		return mono_dyn_method_alloc0 (imethod->method, size);
	else
		return m_method_alloc0 (imethod->method, size);
}

static guint32*
initialize_arg_offsets (InterpMethod *imethod, MonoMethodSignature *csig)
{
	if (imethod->arg_offsets)
		return imethod->arg_offsets;

	// For pinvokes, csig represents the real signature with marshalled args. If an explicit
	// marshalled signature was not provided, we use the managed signature of the method.
	MonoMethodSignature *sig = csig;
	if (!sig)
		sig = mono_method_signature_internal (imethod->method);
	int arg_count = sig->hasthis + sig->param_count;
	guint32 *arg_offsets = (guint32*)imethod_alloc0 (imethod, (arg_count + 1) * sizeof (int));
	int index = 0, offset = 0;

	if (sig->hasthis) {
		arg_offsets [index++] = 0;
		offset = MINT_STACK_SLOT_SIZE;
	}

	for (int i = 0; i < sig->param_count; i++) {
		MonoType *type = sig->params [i];
		int size, align;
		size = mono_interp_type_size (type, mono_mint_type (type), &align);

		offset = ALIGN_TO (offset, align);
		arg_offsets [index++] = offset;
		offset += size;
	}
	// This index is not associated with an actual argument, we just store the offset
	// for convenience in order to easily determine the size of the param area used
	arg_offsets [index] = ALIGN_TO (offset, MINT_STACK_SLOT_SIZE);

	mono_memory_write_barrier ();
	/* If this fails, the new one is leaked in the mem manager */
	mono_atomic_cas_ptr ((gpointer*)&imethod->arg_offsets, arg_offsets, NULL);
	return imethod->arg_offsets;
}

static guint32
get_arg_offset_fast (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	guint32 *arg_offsets = imethod->arg_offsets;
	if (arg_offsets)
		return arg_offsets [index];

	arg_offsets = initialize_arg_offsets (imethod, sig);
	g_assert (arg_offsets);
	return arg_offsets [index];
}

static guint32
get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	if (imethod) {
		return get_arg_offset_fast (imethod, sig, index);
	} else {
		g_assert (!sig->hasthis);
		return compute_arg_offset (sig, index);
	}
}

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
static MonoFuncV mono_native_to_interp_trampoline = NULL;
#endif

#ifndef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP

typedef enum {
	PINVOKE_ARG_NONE = 0,
	PINVOKE_ARG_INT = 1,
	PINVOKE_ARG_INT_PAIR = 2,
	PINVOKE_ARG_R8 = 3,
	PINVOKE_ARG_R4 = 4,
	PINVOKE_ARG_VTYPE = 5,
	PINVOKE_ARG_SCALAR_VTYPE = 6,
	// This isn't ifdefed so it's easier to write code that handles it without sprinkling
	//  800 ifdefs in this file
	PINVOKE_ARG_WASM_VALUETYPE_RESULT = 7,
} PInvokeArgType;

typedef struct {
	int ilen, flen;
	MonoType *ret_mono_type;
	PInvokeArgType ret_pinvoke_type;
	PInvokeArgType *arg_types;
} BuildArgsFromSigInfo;

static MonoType *
filter_type_for_args_from_sig (MonoType *type) {
#if defined(HOST_WASM)
	MonoType *etype;
	if (MONO_TYPE_ISSTRUCT (type) && mini_wasm_is_scalar_vtype (type, &etype))
		// FIXME: Does this need to be recursive?
		return etype;
#endif
	return type;
}

static BuildArgsFromSigInfo *
get_build_args_from_sig_info (MonoMemoryManager *mem_manager, MonoMethodSignature *sig)
{
	BuildArgsFromSigInfo *info = mono_mem_manager_alloc0 (mem_manager, sizeof (BuildArgsFromSigInfo));
	int ilen = 0, flen = 0;

	info->arg_types = mono_mem_manager_alloc0 (mem_manager, sizeof (PInvokeArgType) * sig->param_count);

	g_assert (!sig->hasthis);

	for (int i = 0; i < sig->param_count; i++) {
		MonoType *type = filter_type_for_args_from_sig (sig->params [i]);
		guint32 ptype;

retry:
		ptype = m_type_is_byref (type) ? MONO_TYPE_PTR : type->type;
		switch (ptype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
#if SIZEOF_VOID_P == 8
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#endif
			info->arg_types [i] = PINVOKE_ARG_INT;
			ilen++;
			break;
#if SIZEOF_VOID_P == 4
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			info->arg_types [i] = PINVOKE_ARG_INT_PAIR;
			ilen += 2;
			break;
#endif
		case MONO_TYPE_R4:
			info->arg_types [i] = PINVOKE_ARG_R4;
			flen++;
			break;
		case MONO_TYPE_R8:
			info->arg_types [i] = PINVOKE_ARG_R8;
			flen++;
			break;
		case MONO_TYPE_VALUETYPE:
			if (m_class_is_enumtype (m_type_data_get_klass_unchecked (type))) {
				type = mono_class_enum_basetype_internal (m_type_data_get_klass_unchecked (type));
				goto retry;
			}
			info->arg_types [i] = PINVOKE_ARG_VTYPE;

#ifdef HOST_WASM
			{
				MonoType *etype;

				/* Scalar vtypes are passed by value */
				// FIXME: r4/r8
				if (mini_wasm_is_scalar_vtype (sig->params [i], &etype) && etype->type != MONO_TYPE_R4 && etype->type != MONO_TYPE_R8)
					info->arg_types [i] = PINVOKE_ARG_SCALAR_VTYPE;
			}
#endif
			ilen++;
			break;
		case MONO_TYPE_GENERICINST: {
			// FIXME: Should mini_wasm_is_scalar_vtype stuff go in here?
			MonoClass *container_class = m_type_data_get_generic_class_unchecked (type)->container_class;
			type = m_class_get_byval_arg (container_class);
			goto retry;
		}
		default:
			g_error ("build_args_from_sig: not implemented yet (1): 0x%x\n", ptype);
		}
	}

	if (ilen > INTERP_ICALL_TRAMP_IARGS)
		g_error ("build_args_from_sig: TODO, allocate gregs: %d\n", ilen);

	if (flen > INTERP_ICALL_TRAMP_FARGS)
		g_error ("build_args_from_sig: TODO, allocate fregs: %d\n", flen);

	info->ilen = ilen;
	info->flen = flen;

	info->ret_mono_type = filter_type_for_args_from_sig (sig->ret);

	switch (info->ret_mono_type->type) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
			info->ret_pinvoke_type = PINVOKE_ARG_INT;
			break;
#if SIZEOF_VOID_P == 8
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#endif
			info->ret_pinvoke_type = PINVOKE_ARG_INT;
			break;
#if SIZEOF_VOID_P == 4
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			info->ret_pinvoke_type = PINVOKE_ARG_INT;
			break;
#endif
		case MONO_TYPE_VALUETYPE:
		case MONO_TYPE_GENERICINST:
			info->ret_pinvoke_type = PINVOKE_ARG_INT;
#ifdef HOST_WASM
			// This ISSTRUCT check is important, because the type could be an enum
			if (MONO_TYPE_ISSTRUCT (info->ret_mono_type)) {
				// The return type was already filtered previously, so if we get here
				//  we're returning a struct byref instead of as a scalar
				info->ret_pinvoke_type = PINVOKE_ARG_WASM_VALUETYPE_RESULT;
				info->ilen++;
			}
#endif
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			info->ret_pinvoke_type = PINVOKE_ARG_R8;
			break;
		case MONO_TYPE_VOID:
			info->ret_pinvoke_type = PINVOKE_ARG_NONE;
			break;
		default:
			g_error ("build_args_from_sig: ret type not implemented yet: 0x%x\n", info->ret_mono_type->type);
	}

	return info;
}

static void
build_args_from_sig (InterpMethodArguments *margs, MonoMethodSignature *sig, BuildArgsFromSigInfo *info, InterpFrame *frame)
{
#ifdef TARGET_WASM
	margs->sig = sig;
#endif

	margs->ilen = info->ilen;
	margs->flen = info->flen;

	size_t int_i = 0;
	size_t int_f = 0;

	if (info->ret_pinvoke_type == PINVOKE_ARG_WASM_VALUETYPE_RESULT) {
		// Allocate an empty arg0 for the address of the return value
		// info->ilen was already increased earlier
		int_i++;
	}

	if (margs->ilen > 0) {
		if (margs->ilen <= 8)
			margs->iargs = margs->iargs_buf;
		else
			margs->iargs = g_malloc0 (sizeof (gpointer) * margs->ilen);
	}

	if (margs->flen > 0) {
		if (margs->flen <= 8)
			margs->fargs = margs->fargs_buf;
		else
			margs->fargs = g_malloc0 (sizeof (double) * margs->flen);
	}

	for (int i = 0; i < sig->param_count; i++) {
		guint32 offset = get_arg_offset (frame->imethod, sig, i);
		stackval *sp_arg = STACK_ADD_BYTES (frame->stack, offset);

		switch (info->arg_types [i]) {
		case PINVOKE_ARG_INT:
			margs->iargs [int_i] = sp_arg->data.p;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d]: %p (frame @ %d)\n", int_i, margs->iargs [int_i], i);
#endif
			int_i++;
			break;
		case PINVOKE_ARG_R4:
			* (float *) &(margs->fargs [int_f]) = sp_arg->data.f_r4;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->fargs [%d]: %p (%f) (frame @ %d)\n", int_f, margs->fargs [int_f], margs->fargs [int_f], i);
#endif
			int_f ++;
			break;
		case PINVOKE_ARG_R8:
			margs->fargs [int_f] = sp_arg->data.f;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->fargs [%d]: %p (%f) (frame @ %d)\n", int_f, margs->fargs [int_f], margs->fargs [int_f], i);
#endif
			int_f ++;
			break;
		case PINVOKE_ARG_VTYPE:
			margs->iargs [int_i] = sp_arg;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d]: %p (vt) (frame @ %d)\n", int_i, margs->iargs [int_i], i);
#endif
			int_i++;
			break;
		case PINVOKE_ARG_SCALAR_VTYPE:
			margs->iargs [int_i] = *(gpointer*)sp_arg;

#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d]: %p (vt) (frame @ %d)\n", int_i, margs->iargs [int_i], i);
#endif
			int_i++;
			break;
		case PINVOKE_ARG_INT_PAIR: {
			margs->iargs [int_i] = (gpointer)(gssize)sp_arg->data.pair.lo;
			int_i++;
			margs->iargs [int_i] = (gpointer)(gssize)sp_arg->data.pair.hi;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d/%d]: 0x%016" PRIx64 ", hi=0x%08x lo=0x%08x (frame @ %d)\n", int_i - 1, int_i, *((guint64 *) &margs->iargs [int_i - 1]), sp_arg->data.pair.hi, sp_arg->data.pair.lo, i);
#endif
			int_i++;
			break;
		}
		default:
			g_assert_not_reached ();
			break;
		}
	}

	switch (info->ret_pinvoke_type) {
	case PINVOKE_ARG_WASM_VALUETYPE_RESULT:
		// We pass the return value address in arg0 so fill it in, we already
		//  reserved space for it earlier.
		g_assert (frame->retval);
		margs->iargs[0] = (gpointer*)frame->retval;
		// The return type is void so retval should be NULL
		margs->retval = NULL;
		margs->is_float_ret = 0;
		break;
	case PINVOKE_ARG_INT:
		margs->retval = (gpointer*)frame->retval;
		margs->is_float_ret = 0;
		break;
	case PINVOKE_ARG_R8:
		margs->retval = (gpointer*)frame->retval;
		margs->is_float_ret = 1;
		break;
	case PINVOKE_ARG_NONE:
		margs->retval = NULL;
		break;
	default:
		g_assert_not_reached ();
		break;
	}
}
#endif

static void
interp_frame_arg_to_data (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// If index == -1, we finished executing an InterpFrame and the result is at retval.
	if (index == -1)
		stackval_to_data_sign_ext (sig->ret, iframe->retval, data, sig->pinvoke && !sig->marshalling_disabled);
	else if (sig->hasthis && index == 0)
		*(gpointer*)data = iframe->stack->data.p;
	else
		stackval_to_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke && !sig->marshalling_disabled);
}

static void
interp_data_to_frame_arg (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gconstpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// Get result from pinvoke call, put it directly on top of execution stack in the caller frame
	if (index == -1)
		stackval_from_data (sig->ret, iframe->retval, data, sig->pinvoke && !sig->marshalling_disabled);
	else if (sig->hasthis && index == 0)
		iframe->stack->data.p = *(gpointer*)data;
	else
		stackval_from_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke && !sig->marshalling_disabled);
}

static gpointer
interp_frame_arg_to_storage (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	if (index == -1)
		return iframe->retval;
	else
		return STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index));
}

static MonoPIFunc
get_interp_to_native_trampoline (void)
{
	static MonoPIFunc trampoline = NULL;

	if (!trampoline) {
		if (mono_ee_features.use_aot_trampolines) {
			trampoline = (MonoPIFunc) mono_aot_get_trampoline ("interp_to_native_trampoline");
		} else {
			MonoTrampInfo *info;
			trampoline = (MonoPIFunc) mono_arch_get_interp_to_native_trampoline (&info);
			mono_tramp_info_register (info, NULL);
		}
		mono_memory_barrier ();
	}
	return trampoline;
}

static void
interp_to_native_trampoline (gpointer addr, gpointer ccontext)
{
	get_interp_to_native_trampoline () (addr, ccontext);
}

#ifdef HOST_WASM
typedef struct {
	MonoPIFunc entry_func;
	BuildArgsFromSigInfo *call_info;
} WasmPInvokeCacheData;
#endif

/* MONO_NO_OPTIMIZATION is needed due to usage of INTERP_PUSH_LMF_WITH_CTX. */
#ifdef _MSC_VER
#pragma optimize ("", off)
#endif
static MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
ves_pinvoke_method (
	InterpMethod *imethod,
	MonoMethodSignature *sig,
	MonoFuncV addr,
	ThreadContext *context,
	InterpFrame *parent_frame,
	stackval *ret_sp,
	stackval *sp,
	gboolean save_last_error,
	gpointer *cache,
	gboolean *gc_transitions)
{
	InterpFrame frame = {0};
	frame.parent = parent_frame;
	frame.imethod = imethod;
	frame.stack = sp;
	frame.retval = ret_sp;

	MonoLMFExt ext;
	gpointer gc_safe_cookie = NULL;
	gpointer args;

	MONO_REQ_GC_UNSAFE_MODE;

#ifdef HOST_WASM
	/*
	 * Use a per-signature entry function.
	 * Cache it in imethod->data_items.
	 * This is GC safe.
	 */
	MonoPIFunc entry_func = NULL;
	WasmPInvokeCacheData *cache_data = (WasmPInvokeCacheData*)*cache;
	if (!cache_data) {
		cache_data = g_new0 (WasmPInvokeCacheData, 1);
		cache_data->entry_func = (MonoPIFunc)mono_wasm_get_interp_to_native_trampoline (sig);
		cache_data->call_info = get_build_args_from_sig_info (get_default_mem_manager (), sig);
		mono_memory_barrier ();
		*cache = cache_data;
	}
	entry_func = cache_data->entry_func;
#else
	static MonoPIFunc entry_func = NULL;
	if (!entry_func) {
		MONO_ENTER_GC_UNSAFE;
#ifdef MONO_ARCH_HAS_NO_PROPER_MONOCTX
		ERROR_DECL (error);
		entry_func = (MonoPIFunc) mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_to_native_trampoline", (gpointer) mono_interp_to_native_trampoline), error);
		mono_error_assert_ok (error);
#else
		entry_func = get_interp_to_native_trampoline ();
#endif
		mono_memory_barrier ();
		MONO_EXIT_GC_UNSAFE;
	}
#endif

	if (save_last_error) {
		mono_marshal_clear_last_error ();
	}

#ifdef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
	gpointer call_info = *cache;

	if (!call_info) {
		call_info = mono_arch_get_interp_native_call_info (get_default_mem_manager (), sig);
		mono_memory_barrier ();
		*cache = call_info;
	}
	CallContext ccontext;
	mono_arch_set_native_call_context_args (&ccontext, &frame, sig, call_info);
	args = &ccontext;
#else

#ifdef HOST_WASM
	BuildArgsFromSigInfo *call_info = cache_data->call_info;
#else
	BuildArgsFromSigInfo *call_info = NULL;
	g_assert_not_reached ();
#endif

	InterpMethodArguments margs;
	memset (&margs, 0, sizeof (InterpMethodArguments));
	build_args_from_sig (&margs, sig, call_info, &frame);
	args = &margs;
#endif

	INTERP_PUSH_LMF_WITH_CTX (&frame, ext, exit_pinvoke);

	if (*gc_transitions) {
		MONO_STACKDATA (stack_data);
		gc_safe_cookie = mono_threads_enter_gc_safe_region_internal (&stack_data);
		entry_func ((gpointer) addr, args);
		mono_threads_exit_gc_safe_region_internal (gc_safe_cookie, &stack_data);
		*gc_transitions = FALSE;
	} else {
		entry_func ((gpointer) addr, args);
	}

	if (save_last_error)
		mono_marshal_set_last_error ();
	interp_pop_lmf (&ext);

#ifdef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
#ifdef MONO_ARCH_HAVE_SWIFTCALL
	if (mono_method_signature_has_ext_callconv (sig, MONO_EXT_CALLCONV_SWIFTCALL)) {
		int arg_index = -1;
		gpointer data = mono_arch_get_swift_error (&ccontext, sig, &arg_index);

		// Perform an indirect store at arg_index stack location
		if (arg_index >= 0) {
			g_assert (data);
			stackval *result = (stackval*) STACK_ADD_BYTES (frame.stack, get_arg_offset (frame.imethod, sig, arg_index));
			*(gpointer*)result->data.p = *(gpointer*)data;
		}
	}
#endif
	if (!context->has_resume_state) {
		mono_arch_get_native_call_context_ret (&ccontext, &frame, sig, call_info);
	}

	g_free (ccontext.stack);
#else
	// Only the vt address has been returned, we need to copy the entire content on interp stack
	if (!context->has_resume_state && MONO_TYPE_ISSTRUCT (call_info->ret_mono_type)) {
		if (call_info->ret_pinvoke_type != PINVOKE_ARG_WASM_VALUETYPE_RESULT)
			stackval_from_data (call_info->ret_mono_type, frame.retval, (char*)frame.retval->data.p, sig->pinvoke && !sig->marshalling_disabled);
	}

	if (margs.iargs != margs.iargs_buf)
		g_free (margs.iargs);
	if (margs.fargs != margs.fargs_buf)
		g_free (margs.fargs);
#endif
	goto exit_pinvoke; // prevent unused label warning in some configurations

/* If an exception is thrown from native code, execution will continue here */
exit_pinvoke:
	if (*gc_transitions) {
		mono_threads_abort_gc_safe_region_internal (gc_safe_cookie);
		*gc_transitions = FALSE;
	}
	return NULL;
}
#ifdef _MSC_VER
#pragma optimize ("", on)
#endif

/*
 * interp_init_delegate:
 *
 *   Initialize del->interp_method.
 */
static void
interp_init_delegate (MonoDelegate *del, MonoDelegateTrampInfo **out_info, MonoError *error)
{
	MonoMethod *method;

	if (del->interp_method) {
		/* Delegate created by a call to ves_icall_mono_delegate_ctor_interp () */
		del->method = ((InterpMethod *)del->interp_method)->method;
	} else if (del->method_ptr && !del->method) {
		/* Delegate created from methodInfo.MethodHandle.GetFunctionPointer() */
		del->interp_method = (InterpMethod *)del->method_ptr;
		if (mono_llvm_only)
			// FIXME:
			g_assert_not_reached ();
	} else if (del->method) {
		/* Delegate created dynamically */
		del->interp_method = mono_interp_get_imethod (del->method);
	} else {
		/* Created from JITted code */
		g_assert_not_reached ();
	}

	method = ((InterpMethod*)del->interp_method)->method;
	if (del->target &&
			method &&
			method->flags & METHOD_ATTRIBUTE_VIRTUAL &&
			method->flags & METHOD_ATTRIBUTE_ABSTRACT &&
			mono_class_is_abstract (method->klass))
		del->interp_method = get_virtual_method ((InterpMethod*)del->interp_method, del->target->vtable);

	method = ((InterpMethod*)del->interp_method)->method;
	if (method && m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
		const char *name = method->name;
		if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
			/*
			 * When invoking the delegate interp_method is executed directly. If it's an
			 * invoke make sure we replace it with the appropriate delegate invoke wrapper.
			 *
			 * FIXME We should do this later, when we also know the delegate on which the
			 * target method is called.
			 */
			del->interp_method = mono_interp_get_imethod (mono_marshal_get_delegate_invoke (method, NULL));
		}
	}

	if (!((InterpMethod *) del->interp_method)->transformed && method_is_dynamic (method)) {
		/* Return any errors from method compilation */
		mono_interp_transform_method ((InterpMethod *) del->interp_method, get_context (), error);
		return_if_nok (error);
	}

	/*
	 * Compute a MonoDelegateTrampInfo for this delegate if possible and pass it back to
	 * the caller.
	 * Keep a 1 element cache in imethod->del_info. This should be good enough since most methods
	 * are only associated with one delegate type.
	 */
	if (out_info)
		*out_info = NULL;
	if (mono_llvm_only) {
		InterpMethod *imethod = del->interp_method;
		method = imethod->method;
		if (imethod->del_info && imethod->del_info->klass == del->object.vtable->klass) {
			*out_info = imethod->del_info;
		} else if (!imethod->del_info) {
			imethod->del_info = mono_create_delegate_trampoline_info (del->object.vtable->klass, method, FALSE);
			*out_info = imethod->del_info;
		}
	}
}

/* Convert a function pointer for a managed method to an InterpMethod* */
static InterpMethod*
ftnptr_to_imethod (gpointer addr, gboolean *need_unbox)
{
	InterpMethod *imethod;

	if (mono_llvm_only) {
		/* Function pointers are represented by a MonoFtnDesc structure */
		MonoFtnDesc *ftndesc = (MonoFtnDesc*)addr;
		g_assert (ftndesc);
		g_assert (ftndesc->method);

		if (!ftndesc->interp_method) {
			imethod = mono_interp_get_imethod (ftndesc->method);
			mono_memory_barrier ();
			// FIXME Handle unboxing here ?
			ftndesc->interp_method = imethod;
		}
		*need_unbox = INTERP_IMETHOD_IS_TAGGED_UNBOX (ftndesc->interp_method);
		imethod = INTERP_IMETHOD_UNTAG_UNBOX (ftndesc->interp_method);
	} else {
		/* Function pointers are represented by their InterpMethod */
		*need_unbox = INTERP_IMETHOD_IS_TAGGED_UNBOX (addr);
		imethod = INTERP_IMETHOD_UNTAG_UNBOX (addr);
	}
	return imethod;
}

static gpointer
imethod_to_ftnptr (InterpMethod *imethod, gboolean need_unbox)
{
	if (mono_llvm_only) {
		ERROR_DECL (error);
		/* Function pointers are represented by a MonoFtnDesc structure */
		MonoFtnDesc **ftndesc_p;
		if (need_unbox)
			ftndesc_p = &imethod->ftndesc_unbox;
		else
			ftndesc_p = &imethod->ftndesc;
		if (!*ftndesc_p) {
			MonoFtnDesc *ftndesc = mini_llvmonly_load_method_ftndesc (imethod->method, FALSE, need_unbox, error);

#ifdef HOST_WASM
			if (!is_ok (error) || !ftndesc) {
				/*
				 * In llvmonly+aot-only mode we can fail to materialize an interp-in wrapper
				 * for signatures we didn't AOT. Keep a descriptor carrying interp_method so
				 * interpreter-only call paths (calli/delegate metadata) can still resolve
				 * back to InterpMethod without hard-aborting.
				 */
				mono_interp_error_cleanup (error);
				ftndesc = mini_llvmonly_create_ftndesc (imethod->method, NULL, NULL);
			}
#endif

			mono_error_assert_ok (error);
			if (need_unbox)
				ftndesc->interp_method = INTERP_IMETHOD_TAG_UNBOX (imethod);
			else
				ftndesc->interp_method = imethod;
			mono_memory_barrier ();
			*ftndesc_p = ftndesc;
		}
		return *ftndesc_p;
	} else {
		if (need_unbox)
			return INTERP_IMETHOD_TAG_UNBOX (imethod);
		else
			return imethod;
	}
}

static void
interp_delegate_ctor (MonoObjectHandle this_obj, MonoObjectHandle target, gpointer addr, MonoError *error)
{
	gboolean need_unbox;
	/* addr is the result of an LDFTN opcode */
	InterpMethod *imethod = ftnptr_to_imethod (addr, &need_unbox);

	if (!(imethod->method->flags & METHOD_ATTRIBUTE_STATIC)) {
		MonoMethod *invoke = mono_get_delegate_invoke_internal (mono_handle_class (this_obj));
		/* virtual invoke delegates must not have null check */
		if (mono_method_signature_internal (imethod->method)->param_count == mono_method_signature_internal (invoke)->param_count
				&& MONO_HANDLE_IS_NULL (target)) {
			mono_error_set_argument (error, "this", "Delegate to an instance method cannot have null 'this'");
			return;
		}
	}

	g_assert (imethod->method);
	gpointer entry = mini_get_interp_callbacks ()->create_method_pointer (imethod->method, FALSE, error);
	return_if_nok (error);

	MONO_HANDLE_SETVAL (MONO_HANDLE_CAST (MonoDelegate, this_obj), interp_method, gpointer, imethod);

	mono_delegate_ctor (this_obj, target, entry, imethod->method, error);
}

#if DEBUG_INTERP
static void
dump_stackval (GString *str, stackval *s, MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_BOOLEAN:
		g_string_append_printf (str, "[%d] ", s->data.i);
		break;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		g_string_append_printf (str, "[%p] ", s->data.p);
		break;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (m_type_data_get_klass_unchecked (type)))
			g_string_append_printf (str, "[%d] ", s->data.i);
		else
			g_string_append_printf (str, "[vt:%p] ", s->data.p);
		break;
	case MONO_TYPE_R4:
		g_string_append_printf (str, "[%g] ", s->data.f_r4);
		break;
	case MONO_TYPE_R8:
		g_string_append_printf (str, "[%g] ", s->data.f);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	default: {
		GString *res = g_string_new ("");
		mono_type_get_desc (res, type, TRUE);
		g_string_append_printf (str, "[{%s} %" PRId64 "/0x%0" PRIx64 "] ", res->str, (gint64)s->data.l, (guint64)s->data.l);
		g_string_free (res, TRUE);
		break;
	}
	}
}

static char*
dump_retval (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	MonoType *ret = mono_method_signature_internal (inv->imethod->method)->ret;

	if (ret->type != MONO_TYPE_VOID)
		dump_stackval (str, inv->stack, ret);

	return g_string_free (str, FALSE);
}

static char*
dump_args (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	int i;
	MonoMethodSignature *signature = mono_method_signature_internal (inv->imethod->method);

	if (signature->param_count == 0 && !signature->hasthis)
		return g_string_free (str, FALSE);

	if (signature->hasthis) {
		MonoMethod *method = inv->imethod->method;
		dump_stackval (str, inv->stack, m_class_get_byval_arg (method->klass));
	}

	for (i = 0; i < signature->param_count; ++i)
		dump_stackval (str, inv->stack + (!!signature->hasthis) + i, signature->params [i]);

	return g_string_free (str, FALSE);
}
#endif

#define CHECK_ADD_OVERFLOW(a,b) \
	(gint32)(b) >= 0 ? (gint32)(G_MAXINT32) - (gint32)(b) < (gint32)(a) ? -1 : 0	\
	: (gint32)(G_MININT32) - (gint32)(b) > (gint32)(a) ? +1 : 0

#define CHECK_SUB_OVERFLOW(a,b) \
	(gint32)(b) < 0 ? (gint32)(G_MAXINT32) + (gint32)(b) < (gint32)(a) ? -1 : 0	\
	: (gint32)(G_MININT32) + (gint32)(b) > (gint32)(a) ? +1 : 0

#define CHECK_ADD_OVERFLOW_UN(a,b) \
	(guint32)(G_MAXUINT32) - (guint32)(b) < (guint32)(a) ? -1 : 0

#define CHECK_SUB_OVERFLOW_UN(a,b) \
	(guint32)(a) < (guint32)(b) ? -1 : 0

#define CHECK_ADD_OVERFLOW64(a,b) \
	(gint64)(b) >= 0 ? (gint64)(G_MAXINT64) - (gint64)(b) < (gint64)(a) ? -1 : 0	\
	: (gint64)(G_MININT64) - (gint64)(b) > (gint64)(a) ? +1 : 0

#define CHECK_SUB_OVERFLOW64(a,b) \
	(gint64)(b) < 0 ? (gint64)(G_MAXINT64) + (gint64)(b) < (gint64)(a) ? -1 : 0	\
	: (gint64)(G_MININT64) + (gint64)(b) > (gint64)(a) ? +1 : 0

#define CHECK_ADD_OVERFLOW64_UN(a,b) \
	(guint64)(G_MAXUINT64) - (guint64)(b) < (guint64)(a) ? -1 : 0

#define CHECK_SUB_OVERFLOW64_UN(a,b) \
	(guint64)(a) < (guint64)(b) ? -1 : 0

#if SIZEOF_VOID_P == 4
#define CHECK_ADD_OVERFLOW_NAT(a,b) CHECK_ADD_OVERFLOW(a,b)
#define CHECK_ADD_OVERFLOW_NAT_UN(a,b) CHECK_ADD_OVERFLOW_UN(a,b)
#else
#define CHECK_ADD_OVERFLOW_NAT(a,b) CHECK_ADD_OVERFLOW64(a,b)
#define CHECK_ADD_OVERFLOW_NAT_UN(a,b) CHECK_ADD_OVERFLOW64_UN(a,b)
#endif

/* Resolves to TRUE if the operands would overflow */
#define CHECK_MUL_OVERFLOW(a,b) \
	((gint32)(a) == 0) || ((gint32)(b) == 0) ? 0 : \
	(((gint32)(a) > 0) && ((gint32)(b) == -1)) ? FALSE : \
	(((gint32)(a) < 0) && ((gint32)(b) == -1)) ? (a == G_MININT32) : \
	(((gint32)(a) > 0) && ((gint32)(b) > 0)) ? (gint32)(a) > ((G_MAXINT32) / (gint32)(b)) : \
	(((gint32)(a) > 0) && ((gint32)(b) < 0)) ? (gint32)(a) > ((G_MININT32) / (gint32)(b)) : \
	(((gint32)(a) < 0) && ((gint32)(b) > 0)) ? (gint32)(a) < ((G_MININT32) / (gint32)(b)) : \
	(gint32)(a) < ((G_MAXINT32) / (gint32)(b))

#define CHECK_MUL_OVERFLOW_UN(a,b) \
	((guint32)(a) == 0) || ((guint32)(b) == 0) ? 0 : \
	(guint32)(b) > ((G_MAXUINT32) / (guint32)(a))

#define CHECK_MUL_OVERFLOW64(a,b) \
	((gint64)(a) == 0) || ((gint64)(b) == 0) ? 0 : \
	(((gint64)(a) > 0) && ((gint64)(b) == -1)) ? FALSE : \
	(((gint64)(a) < 0) && ((gint64)(b) == -1)) ? (a == G_MININT64) : \
	(((gint64)(a) > 0) && ((gint64)(b) > 0)) ? (gint64)(a) > ((G_MAXINT64) / (gint64)(b)) : \
	(((gint64)(a) > 0) && ((gint64)(b) < 0)) ? (gint64)(a) > ((G_MININT64) / (gint64)(b)) : \
	(((gint64)(a) < 0) && ((gint64)(b) > 0)) ? (gint64)(a) < ((G_MININT64) / (gint64)(b)) : \
	(gint64)(a) < ((G_MAXINT64) / (gint64)(b))

#define CHECK_MUL_OVERFLOW64_UN(a,b) \
	((guint64)(a) == 0) || ((guint64)(b) == 0) ? 0 : \
	(guint64)(b) > ((G_MAXUINT64) / (guint64)(a))

#if SIZEOF_VOID_P == 4
#define CHECK_MUL_OVERFLOW_NAT(a,b) CHECK_MUL_OVERFLOW(a,b)
#define CHECK_MUL_OVERFLOW_NAT_UN(a,b) CHECK_MUL_OVERFLOW_UN(a,b)
#else
#define CHECK_MUL_OVERFLOW_NAT(a,b) CHECK_MUL_OVERFLOW64(a,b)
#define CHECK_MUL_OVERFLOW_NAT_UN(a,b) CHECK_MUL_OVERFLOW64_UN(a,b)
#endif

// Do not inline in case order of frame addresses matters.
static MONO_NEVER_INLINE MonoObject*
interp_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc, MonoError *error)
{
	ThreadContext *context = get_context ();
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	stackval *sp = (stackval*)context->stack_pointer;
	MonoMethod *target_method = method;

	error_init (error);
	if (exc)
		*exc = NULL;

	if (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		target_method = mono_marshal_get_native_wrapper (target_method, FALSE, FALSE);
	MonoMethod *invoke_wrapper = mono_marshal_get_runtime_invoke_full (target_method, FALSE, TRUE);

	//* <code>MonoObject *runtime_invoke (MonoObject *this_obj, void **params, MonoObject **exc, void* method)</code>

	if (sig->hasthis)
		sp [0].data.p = obj;
	else
		sp [0].data.p = NULL;
	sp [1].data.p = params;
	sp [2].data.p = exc;
	sp [3].data.p = target_method;

	InterpMethod *imethod = mono_interp_get_imethod (invoke_wrapper);

	InterpFrame frame = {0};
	frame.imethod = imethod;
	frame.stack = sp;
	frame.retval = sp;

	// The method to execute might not be transformed yet, so we don't know how much stack
	// it uses. We bump the stack_pointer here so any code triggered by method compilation
	// will not attempt to use the space that we used to push the args for this method.
	// The real top of stack for this method will be set in mono_interp_exec_method once the
	// method is transformed.
	context->stack_pointer = (guchar*)(sp + 4);
	g_assert (context->stack_pointer < context->stack_end);

	MONO_ENTER_GC_UNSAFE;
	mono_interp_exec_method (&frame, context, NULL);
	MONO_EXIT_GC_UNSAFE;

	context->stack_pointer = (guchar*)sp;

	if (context->has_resume_state) {
		/*
		 * This can happen on wasm where native frames cannot be skipped during EH.
		 * EH processing will continue when control returns to the interpreter.
		 */
		if (mono_aot_mode == MONO_AOT_MODE_LLVMONLY_INTERP)
			mono_llvm_start_native_unwind ();
		return NULL;
	}
	// The return value is at the bottom of the stack
	return frame.stack->data.o;
}

typedef struct {
	InterpMethod *rmethod;
	gpointer this_arg;
	gpointer res;
	gpointer args [16];
	gpointer *many_args;
} InterpEntryData;

#if HOST_BROWSER
#define WJ_SCRATCH_RET_OFF 192   /* result slot; past the max args (WASM_FUNCTYPE_MAX_PARAMS*8 = 128) */
#define WJ_SCRATCH_SIZE    256

/* TRUE iff this interp_entry invocation is the IMMEDIATE entry made by the wasm-JIT outbound residual
 * (mono_wasm_jit_call_interp): only that caller passes data->res == this thread's scratch result slot,
 * unconditionally (void returns included). Derived from the InterpEntryData instead of a thread flag:
 * the old __thread wj_residual_active stayed TRUE for the whole dynamic extent of the residual callee,
 * so ANY nested interp_entry reached from inside it — a delegate/ftndesc call into an interpreted
 * method, an AOT callee calling back into the interpreter, or a JSPI-interleaved entry on the same
 * worker — was misclassified as the residual entry (observed via the leak diagnostic this replaces):
 * it took the no-write-barrier ref-return store below with a res that is NOT the scratch, and made
 * do_jit_call's jiterp-thunk path skip its LMF for a stock MINT_JIT_CALL (pass-1 then misses handlers
 * across that interp->AOT transition). Deriving from data->res is leak-proof by construction: nested
 * and interleaved entries classify themselves, and no restore is needed on a C++/wasm-EH unwind.
 *
 * The plain (barrier-less) ref-return store this gates is still required for the scratch: data->res is
 * the per-thread scratch buffer, NOT a GC heap slot, so a barrier would register that transient address
 * in sgen's remembered set and a later GC would read whatever overwrote it as an object pointer (heap
 * corruption). The returned object stays live without the barrier — via the interp frame during the
 * call, and via the JIT ref shadow stack immediately after (the JIT copies it there with no
 * intervening GC safepoint). */
gpointer mono_wasm_jit_scratch (void);

static inline gboolean
wj_entry_is_residual (InterpEntryData *data)
{
	return data->res == (gpointer) ((guint8 *) mono_wasm_jit_scratch () + WJ_SCRATCH_RET_OFF);
}
#endif

/* fwd-decl: interp_entry's wasm-JIT residual fast path calls do_jit_call (defined below) to run an AOT'd
 * callee natively instead of interpreting it. wj_residual is TRUE only for the IMMEDIATE wasm-JIT
 * residual call (interp_entry fast path / wasm_jit_aot_call_lean) — it gates the jiterp-thunk LMF skip,
 * which must never apply to the interp's own MINT_JIT_CALL (even one executed inside a residual callee). */
static MONO_NEVER_INLINE void do_jit_call (ThreadContext *context, stackval *ret_sp, stackval *sp, InterpFrame *frame, InterpMethod *rmethod, gboolean wj_residual, MonoError *error);

/* Main function for entering the interpreter from compiled code */
// Do not inline in case order of frame addresses matters.
static MONO_NEVER_INLINE void
interp_entry (InterpEntryData *data)
{
	InterpMethod *rmethod;
	ThreadContext *context;
	stackval *sp;
	int stack_index = 0;
	MonoMethod *method;
	MonoMethodSignature *sig;
	MonoType *type;
	gpointer orig_domain = NULL, attach_cookie;
	int i;

	if ((gsize)data->rmethod & 1) {
		/* Unbox */
		data->this_arg = mono_object_unbox_internal ((MonoObject*)data->this_arg);
		data->rmethod = (InterpMethod*)(gpointer)((gsize)data->rmethod & ~1);
	}
	rmethod = data->rmethod;

	if (rmethod->needs_thread_attach)
		orig_domain = mono_threads_attach_coop (mono_domain_get (), &attach_cookie);

	context = get_context ();
	sp = (stackval*)context->stack_pointer;

	method = rmethod->method;

	if (rmethod->is_invoke) {
		/*
		 * This happens when AOT code for the invoke wrapper is not found.
		 * Have to replace the method with the wrapper here, since the wrapper depends on the delegate.
		 */
		MonoDelegate *del = (MonoDelegate*)data->this_arg;
		// FIXME: This is slow
		method = mono_marshal_get_delegate_invoke (method, del);
		data->rmethod = mono_interp_get_imethod (method);
	}

	sig = mono_method_signature_internal (method);

	// FIXME: Optimize this

	if (sig->hasthis) {
		sp->data.p = data->this_arg;
		stack_index = 1;
	}

	gpointer *params;
	if (data->many_args)
		params = data->many_args;
	else
		params = data->args;
	for (i = 0; i < sig->param_count; ++i) {
		int arg_offset = get_arg_offset_fast (rmethod, NULL, stack_index + i);
		stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, arg_offset);

		if (m_type_is_byref (sig->params [i]))
			sval->data.p = params [i];
		else
			stackval_from_data (sig->params [i], sval, params [i], FALSE);
	}

	InterpFrame frame = {0};
	frame.imethod = data->rmethod;
	frame.stack = sp;
	frame.retval = sp;

	int params_size = get_arg_offset_fast (rmethod, NULL, stack_index + sig->param_count);
	context->stack_pointer = (guchar*)ALIGN_TO ((guchar*)sp + params_size, MINT_STACK_ALIGNMENT);
	g_assert (context->stack_pointer < context->stack_end);

	MONO_ENTER_GC_UNSAFE;
#if HOST_BROWSER
	/* wasm-JIT residual FAST PATH (jit -> AOT): if this call IS the wasm-JITted method's residual entry
	 * (res-keyed, see wj_entry_is_residual) and the callee has AOT code, call it NATIVELY via do_jit_call
	 * instead of interpreting it. interp_entry has already copied the args onto the GC-scanned interp
	 * stack (sp) at get_arg_offset_fast offsets — exactly do_jit_call's expected layout — and set up
	 * `frame`, so this reuses interp_entry's GC-safe setup (no synthesized frame). Without this, a
	 * wasm-JITted method's residual / vcall-fallback to AOT'd library code (fastutil/java/corlib) was
	 * dropped to the interpreter, never touching the AOT body. Eligibility is cached on code_type, like
	 * the interp's MINT_JIT_CALL. */
	extern int mono_wasm_jit_aot_residual;
	gboolean wj_did_jit_call = FALSE;
	/* wasm-JIT e-slot redirect: if the target method has itself been wasm-JITted (a live entry thunk),
	 * run its COMPILED wasm body via the e-thunk instead of interpreting it. This is what makes a JITted
	 * method reached through a NON-MINT_CALL entry actually execute in wasm rather than as its interp copy:
	 * a residual / vcall-fallback from another JITted method (mono_wasm_jit_call_interp,
	 * mono_wasm_jit_vcall_*) and a native/AOT-driven virtual dispatch all funnel here with rmethod = the
	 * resolved target. Before this, such a target ran its interp copy even though it was fully JITted — the
	 * dominant steady-state boundary cost in the bench (the hot entry-edges: IKVM lambda applyAsLong/accept,
	 * Vec3i.equals). interp_entry has already marshalled `this`+scalar args onto the GC-scanned interp stack
	 * at sp (this at 0, each arg at +8 — arg_offsets[k]==k*8 for the scalar-only sigs the JIT emits), which
	 * is EXACTLY the e-thunk's (args_ptr) layout; the e-thunk stores the result back at sp+0, and the shared
	 * return tail below marshals it to data->res identically to the mono_interp_exec_method path. A thrown
	 * callee sets the interp resume-state inside mono_wasm_jit_invoke_caught, which the has_resume_state /
	 * need_native_unwind tail propagates — same model as the AOT residual branch. No extra interp_push_lmf:
	 * the JITted caller that reached this entry already pushed its island LMF (prologue), and the e-thunk
	 * callee pushes its own, so the managed->native boundary is marked without a callee-frame INTERP_EXIT LMF
	 * (whose null ip could mis-match pass-1). Delegate-invoke (is_invoke) rewrites rmethod to the invoke
	 * wrapper, which is not JITted and dispatches its target via its own MINT_CALL — skip it here. */
	{
		extern int mono_wasm_jit_entry_redirect;
		gint32 wj_eslot = rmethod->wasm_jit_slot;
		if (mono_wasm_jit_entry_redirect && G_UNLIKELY (wj_eslot > 0) && !rmethod->is_invoke) {
			extern void mono_wasm_jit_sync_thread (void);
			extern int mono_wasm_jit_slot_live (int slot);
			/* Bring THIS thread's function table up to date, then confirm the slot actually instantiated
			 * here (sync can fail on a worker under memory pressure while the compiling thread succeeded;
			 * call_indirect-ing a mismatched placeholder would trap). If not live, fall through to interpret. */
			mono_wasm_jit_sync_thread ();
			if (G_LIKELY (mono_wasm_jit_slot_live (wj_eslot))) {
				extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
				mono_wasm_jit_invoke_caught (method, wj_eslot, frame.stack, frame.stack);
				wj_did_jit_call = TRUE;
			}
		}
	}
	if (!wj_did_jit_call && G_UNLIKELY (wj_entry_is_residual (data)) && mono_wasm_jit_aot_residual) {
		InterpMethodCodeType ct = rmethod->code_type;
		if (ct == IMETHOD_CODE_UNKNOWN) {
			ct = mono_interp_jit_call_supported (method, sig) ? IMETHOD_CODE_COMPILED : IMETHOD_CODE_INTERP;
			rmethod->code_type = ct;
		}
		if (ct == IMETHOD_CODE_COMPILED) {
			ERROR_DECL (jit_err);
			do_jit_call (context, frame.stack, frame.stack, &frame, rmethod, TRUE, jit_err);
			if (!is_ok (jit_err)) {
				/* AOT method threw a fresh exception (do_jit_call set the error rather than resume-state):
				 * install the interp resume-state so the residual path below returns threw=1 and
				 * the JITted caller unwinds (same model as mono_wasm_jit_raise_corlib). */
				MonoException *ex = mono_error_convert_to_exception (jit_err);
				MonoContext ctx;
				memset (&ctx, 0, sizeof (ctx));
				MONO_CONTEXT_SET_SP (&ctx, &ctx);
				mono_handle_exception (&ctx, (MonoObject*) ex);
			}
			wj_did_jit_call = TRUE;
		}
	}
	if (!wj_did_jit_call)
#endif
		mono_interp_exec_method (&frame, context, NULL);
	MONO_EXIT_GC_UNSAFE;

	context->stack_pointer = (guchar*)sp;

	if (rmethod->needs_thread_attach)
		mono_threads_detach_coop (orig_domain, &attach_cookie);

	/* cppeh: when this interp_entry was driven by a wasm-JITted residual whose callee
	 * threw, the JITted frame HAS a landing pad (its e-thunk boundary catches the C++/wasm-EH unwind), so
	 * propagate via a native unwind — fall through to need_native_unwind below instead of returning
	 * cooperatively. (pass-1 already installed the resume-state / parked any enclosing island handler.) */
	if (need_native_unwind (context)) {
		mono_llvm_start_native_unwind ();
		return;
	}

	if (mono_llvm_only) {
		if (context->has_resume_state) {
			/* The exception will be handled in a frame above us */
			mono_llvm_start_native_unwind ();
			// FIXME: Set dummy return value ?
			return;
		}
	} else {
#ifdef TARGET_WASM
		if (context->has_resume_state) {
			/*
			 * On wasm, native frames between this callback and the handler frame
			 * cannot be skipped during EH. Return to native code and let EH
			 * continue when control reaches the interpreter again.
			 */
			return;
		}
#endif
		g_assert (!context->has_resume_state);
	}

	// The return value is at the bottom of the stack, after the locals space
	type = rmethod->rtype;
	if (type->type != MONO_TYPE_VOID) {
		// interp entry is called either from a interp_in wrapper or a gsharedvt_in_sig wrapper
		// interp_in wrappers always return intptr (they are more aggresively shared) while the
		// gsharedvt_in_sig wrapper returns the actual type. This check follows the logic in
		// interp_create_method_pointer_llvmonly and interp_create_method_pointer so we do the
		// return sign extension only when called from the interp_in wrapper.
#if HOST_BROWSER
		if (wj_entry_is_residual (data) && MONO_TYPE_IS_REFERENCE (type)) {
			/* wasm-JIT residual ref return: plain store, NO write barrier — data->res IS the per-thread
			 * scratch result slot here */
			*(gpointer *) data->res = frame.stack->data.o;
		} else
#endif
		if (!mono_llvm_only || sig->param_count > MAX_INTERP_ENTRY_ARGS)
			stackval_to_data_sign_ext (type, frame.stack, data->res, FALSE);
		else
			stackval_to_data (type, frame.stack, data->res, FALSE);
	}
}

#if HOST_BROWSER
/*
 * mono_wasm_jit_vcall_i4:
 *
 *   Outbound virtual call from a wasm-JITted method (mini-wasm.c) for the shape
 * `int VirtualMethod()` (instance, no params, i4/u4 return — e.g. getBlockState-style hot
 * polymorphic calls). Resolves the override for THIS_OBJ's runtime class and invokes it
 * through the interpreter via interp_entry().
 *
 * This is the interp-only-runtime path: the llvmonly ftndesc dispatch needs an AOT-generated
 * gsharedvt_in_sig native->interp entry wrapper which doesn't exist here, so instead the JIT
 * lowers a virtual call to a direct call_indirect of this helper (resolved by its raw C address).
 * The result is returned by value (res lives on the C stack), so this is reentrant — nested
	 * virtual calls from the callee reuse the C stack frame, no scratch buffer needed.
	 */
static gboolean wasm_jit_prepare_interp_callee (MonoMethod *method, InterpMethod *imethod, MonoError *error);
gint32
mono_wasm_jit_vcall_i4 (MonoObject *this_obj, MonoMethod *base_method)
{
	/* Per-thread inline cache: (vtable, base_method) -> (resolved target imethod, its wasm f-slot).
	 * The full resolver (mono_object_get_virtual_method_internal — GC handles) + imethod hash lookup
	 * run only on a miss; after warmup the hot path is a vtable/base compare + a direct f-slot call.
	 * Sized for the common mono/bi/poly-morphic case; deeper polymorphism just misses more often. */
#define WJ_IC_BITS 4
#define WJ_IC_SIZE (1 << WJ_IC_BITS)
	static __thread MonoVTable *ic_vt [WJ_IC_SIZE];
	static __thread MonoMethod *ic_base [WJ_IC_SIZE];
	static __thread InterpMethod *ic_im [WJ_IC_SIZE];
	static __thread gint32 ic_fslot [WJ_IC_SIZE];

	MonoVTable *vt = this_obj->vtable;
	guint idx = (guint) ((((gsize) vt >> 4) ^ ((gsize) base_method >> 5)) & (WJ_IC_SIZE - 1));
	InterpMethod *imethod;
	gint32 fs;

	if (G_LIKELY (ic_vt [idx] == vt && ic_base [idx] == base_method)) {
		imethod = ic_im [idx];
		fs = ic_fslot [idx];
	} else {
		MonoMethod *target = mono_object_get_virtual_method_internal (this_obj, base_method);
		imethod = mono_interp_get_imethod (target);
		fs = imethod->wasm_jit_fslot;
		ic_vt [idx] = vt; ic_base [idx] = base_method; ic_im [idx] = imethod; ic_fslot [idx] = fs;
	}

	/* Fast path: resolved override is wasm-JITted — call its scalar f directly ((this)->i4 table
	 * index), a ~ns call_indirect with no interpreter re-entry. This is the win: JITted caller →
	 * JITted callee, devirtualized at runtime. (On wasm a function pointer IS a table index.) */
	if (G_LIKELY (fs > 0)) {
		gint32 (*f) (gpointer) = (gint32 (*) (gpointer)) (intptr_t) fs;
		return f (this_obj);
	}

	/* Slow path: un-JITted override — re-enter the interpreter via interp_entry. */
	{
		ERROR_DECL (error);
		InterpEntryData data;
		gint32 res = 0;
		if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (imethod->method, imethod, error))) {
			/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
			 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
			 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
			 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
			 * (vcall_i4) */
			extern void mono_wasm_jit_throw (MonoObject *exc);
			mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
			return 0;
		}
		memset (&data, 0, sizeof (data));
		data.rmethod = imethod;
		data.this_arg = this_obj;
		data.res = &res;
		interp_entry (&data);
		return res;
	}
}

/*
 * mono_wasm_jit_alloc_ic:
 *
 *   Allocate one inline-cache slot (8 bytes in the wasm heap = linear memory: [i32 vtable, i32
 * f-slot], zeroed) for a virtual call site in a wasm-JITted method. The address is baked into the
 * emitted wasm as an i32.const, so the JITted code reads/updates it inline (see mini-wasm.c). One
 * per virtual call site, allocated once at JIT-emit time. Never freed (bounded: one per JITted
 * virtual call site).
 */
gpointer
mono_wasm_jit_alloc_ic (void)
{
	return g_malloc0 (8);
}

/* Per-call-site AOT-vcall inline cache cell (VCALL_AOT_IC): TWO atomic i64 words
 *   +0  ic1 = vtab (low32) | ((table_index<<1 | kind2bit) << 32)
 *   +8  ic2 = vtab (low32) | (rgctx << 32)
 * A given vtable deterministically maps to ONE override -> one (table_index,kind,rgctx). So a hit only
 * requires BOTH words low32 == this->vtable: the payload is then correct regardless of which thread/
 * fill wrote each word (all fills for that vtab store identical values). Fully MT-safe with only atomic
 * i64 loads/stores (no plain-vs-atomic ordering assumption, which the wasm memory model does NOT give).
 * AOT bodies sit at fixed module-shared table indices (thread-invariant) -> no per-thread liveness gate. */
gpointer
mono_wasm_jit_alloc_aot_ic (void)
{
	return g_malloc0 (16);   /* two atomic i64: +0 = vtab | ((ti<<1|kind2)<<32), +8 = vtab | (rgctx<<32) */
}

/*
 * mono_wasm_jit_vcall_ic_resolve:
 *
 *   Generalized inline-cache miss handler for a wasm-JITted virtual call of ANY signature. The
 * JITted code checks IC[0]==this->vtable inline; on a miss it pushes the method's args, then calls
 * here to get the resolved override's f-slot, then call_indirects that f-slot with the method's
 * full functype. So this only RESOLVES + caches (publishing (vtable, f-slot) into the IC, f-slot
 * first then vtable) and returns the f-slot — the JITted code does the actual typed call. Returns
 * 0 if the override isn't wasm-JITted (caller then traps; for the current env-gated bring-up the
 * override is name-targeted so it's JITted during warmup — force-JIT-on-miss is a TODO).
 */
gint32
mono_wasm_jit_vcall_ic_resolve (MonoObject *this_obj, MonoMethod *base_method, gpointer ic)
{
	MonoMethod *target = mono_object_get_virtual_method_internal (this_obj, base_method);
	InterpMethod *imethod = mono_interp_get_imethod (target);
	gint32 fs = imethod->wasm_jit_fslot;
	if (fs > 0) {
		/* Override is wasm-JITted: cache (vtable, f-slot) so the inline hot path dispatches directly. */
		guint32 *icp = (guint32 *) ic;
		icp [1] = (guint32) fs;                           /* f-slot */
		mono_memory_barrier ();
		icp [0] = (guint32) (gsize) this_obj->vtable;     /* publish vtable last */
		return fs;
	}
	return 0;   /* override not wasm-JITted (legacy inline-IC helper; unused — codegen now uses the
	             * resolve-before-spill + mono_wasm_jit_call_interp path in mono_wasm_jit_vcall_resolve). */
}

/*
 * mono_wasm_jit_vcall_ic_miss:
 *
 *   Inline-cache miss handler for a wasm-JITted virtual call (shape (this)->i4). The JITted code
 * checks IC[0]==this->vtable inline; on a miss it calls here. Resolves the override; if it's
 * wasm-JITted, publishes (vtable, f-slot) into the IC (f-slot first, then vtable last as the
 * publish — readers load vtable first) and calls the f-slot directly; otherwise re-enters the
 * interpreter (and does NOT cache, so un-JITted targets always take this path).
 *
 * NOTE: the IC lives in shared linear memory; the i32-pair read/write is benign for the common
 * case but can tear under concurrent cross-type writes from multiple threads. MT-hardening =
 * pack into one i64 and use i64.atomic.load (reader) + atomic store (here).
 */
gint32
mono_wasm_jit_vcall_ic_miss (MonoObject *this_obj, MonoMethod *base_method, gpointer ic)
{
	MonoMethod *target = mono_object_get_virtual_method_internal (this_obj, base_method);
	InterpMethod *imethod = mono_interp_get_imethod (target);
	gint32 fs = imethod->wasm_jit_fslot;

	if (fs > 0) {
		guint32 *icp = (guint32 *) ic;
		icp [1] = (guint32) fs;                                        /* f-slot */
		mono_memory_barrier ();
		icp [0] = (guint32) (gsize) this_obj->vtable;                  /* publish vtable last */
		gint32 (*f) (gpointer) = (gint32 (*) (gpointer)) (intptr_t) fs;
		return f (this_obj);
	}

	{
		ERROR_DECL (error);
		InterpEntryData data;
		gint32 res = 0;
		if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (target, imethod, error))) {
			/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
			 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
			 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
			 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
			 * (vcall_ic_miss) */
			extern void mono_wasm_jit_throw (MonoObject *exc);
			mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
			return 0;
		}
		memset (&data, 0, sizeof (data));
		data.rmethod = imethod;
		data.this_arg = this_obj;
		data.res = &res;
		interp_entry (&data);
		return res;
	}
}

/*
 * Per-thread scratch buffer for the outbound interp residual (a call from a wasm-JITted method to a
 * callee that is NOT itself wasm-JITted). ONE shared per-thread buffer is correct:
 *  - interp_entry() copies all args out of the buffer onto the interp stack at its very start, before
 *    it runs the callee, so a reentrant (recursive) call that reuses the buffer can't corrupt args
 *    that are still needed; and
 *  - the result slot is written then read in strict nested (LIFO) order across reentrancy.
 * Per-thread => MT-safe. The address can't be baked into the emitted module (a __thread address
 * differs per thread), so the JITted code fetches it via mono_wasm_jit_scratch() at each call site. */
static __thread guint8 wj_scratch [WJ_SCRATCH_SIZE];

extern int mono_wasm_jit_stats;

gpointer
mono_wasm_jit_scratch (void)
{
	return wj_scratch;
}

/*
 * wasm_jit_aot_call_lean:
 *
 *   INLINE AOT FASTPATH for the wasm-JIT residual. When the residual callee has AOT code, run it
 * natively via do_jit_call DIRECTLY — skipping interp_entry's InterpEntryData marshalling layer and
 * its is_invoke/unbox/many_args machinery. This is the same path the interpreter itself uses to reach
 * AOT code (MINT_JIT_CALL); the only unavoidable per-call cost is the GC-scanned arg copy plus the
 * minimal frame do_jit_call's LMF needs. Mirrors interp_entry's residual (wj_residual_entry) AOT branch
 * exactly, including the throw->resume-state handoff (so an uncaught throw unwinds to the handler ABOVE
 * the JITted frame) and the no-write-barrier ref-return store into the scratch. Passes wj_residual=TRUE
 * to do_jit_call explicitly (per-call, not a thread flag) to skip the jiterp-thunk LMF.
 *
 * Returns 1 if the callee threw (resume-state set; the JITted caller must `goto resume`), else 0.
 */
static int
wasm_jit_aot_call_lean (InterpMethod *imethod, MonoMethodSignature *sig, guint8 *buf)
{
	ERROR_DECL (error);
	ThreadContext *context;
	stackval *sp;
	int idx = 0, i;
	gpointer orig_domain = NULL, attach_cookie;

	if (imethod->needs_thread_attach)
		orig_domain = mono_threads_attach_coop (mono_domain_get (), &attach_cookie);

	context = get_context ();
	sp = (stackval*)context->stack_pointer;

	/* Copy `this` + args from the JITted scratch onto the GC-scanned interp stack at the interp arg
	 * offsets. The copy is required: a ref arg must be a GC root across the AOT call (which can move it),
	 * and the scratch is not GC-visible. (Same arg convention as the interp_entry residual path.) */
	if (sig->hasthis) {
		sp->data.p = *(gpointer*)(buf + 0);
		idx = 1;
	}
	for (i = 0; i < (int) sig->param_count; ++i) {
		int arg_offset = get_arg_offset_fast (imethod, NULL, idx + i);
		stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, arg_offset);
		guint8 *slot = buf + (idx + i) * 8;
		if (m_type_is_byref (sig->params [i]))
			sval->data.p = *(gpointer*)slot;
		else
			stackval_from_data (sig->params [i], sval, slot, FALSE);
	}

	InterpFrame frame = {0};
	frame.imethod = imethod;
	frame.stack = sp;
	frame.retval = sp;

	int params_size = get_arg_offset_fast (imethod, NULL, idx + sig->param_count);
	context->stack_pointer = (guchar*)ALIGN_TO ((guchar*)sp + params_size, MINT_STACK_ALIGNMENT);
	g_assert (context->stack_pointer < context->stack_end);

	{
		MONO_ENTER_GC_UNSAFE;
		do_jit_call (context, sp, sp, &frame, imethod, TRUE /* the immediate wasm-JIT residual call */, error);
		if (!is_ok (error)) {
			/* AOT method raised a fresh exception (do_jit_call set the error rather than resume-state):
			 * install the interp resume-state so the JITted caller unwinds (mirrors interp_entry). */
			MonoException *ex = mono_error_convert_to_exception (error);
			MonoContext ctx;
			memset (&ctx, 0, sizeof (ctx));
			MONO_CONTEXT_SET_SP (&ctx, &ctx);
			mono_handle_exception (&ctx, (MonoObject*) ex);
		}
		MONO_EXIT_GC_UNSAFE;
	}

	context->stack_pointer = (guchar*)sp;

	if (imethod->needs_thread_attach)
		mono_threads_detach_coop (orig_domain, &attach_cookie);

	if (context->has_resume_state) {
		/* The throw is delivered by a native (C++/wasm-EH) unwind, not cooperatively: a bare JIT->JIT f-slot
		 * caller has no per-call resume-state poll, so returning threw=1 here would let it swallow the
		 * exception (run on with a garbage dummy return + a stale thread resume-state) and SKIP any enclosing
		 * JITted-island catch/finally that pass-1 parked with handler_frame==NULL. Start the native unwind to
		 * the nearest landing pad (this method's wasm catch, an enclosing JITted island, or the interp e-thunk
		 * boundary). pass-1 (mono_handle_exception) already ran above, so this only performs the unwind. */
		mono_llvm_start_native_unwind ();
		g_assert_not_reached ();   /* the C++ throw above does not return */
	}

	/* Write the return value back into the scratch for the JITted caller (mirrors interp_entry exit). */
	if (sig->ret->type != MONO_TYPE_VOID) {
		memset (buf + WJ_SCRATCH_RET_OFF, 0, 8);
		if (MONO_TYPE_IS_REFERENCE (sig->ret))
			*(gpointer*)(buf + WJ_SCRATCH_RET_OFF) = sp->data.o;   /* no write barrier: scratch is not GC-tracked */
		else
			stackval_to_data_sign_ext (sig->ret, sp, buf + WJ_SCRATCH_RET_OFF, FALSE);
	}
	return 0;
}

/* Warm a residual/vcall callee completely BEFORE the JITted caller spills reference args into the
 * GC-invisible scratch buffer: transform the body if needed, parse the signature, and decide code_type
 * (which may run class init / AOT lookup). After this, call_interp can copy refs out of the scratch and
 * enter the callee without a first-use GC window that would stale the spilled raw pointers. */
static gboolean
wasm_jit_prepare_interp_callee (MonoMethod *method, InterpMethod *imethod, MonoError *error)
{
	MonoMethodSignature *sig;
	if (G_UNLIKELY (!imethod->transformed)) {
		mono_interp_transform_method (imethod, get_context (), error);
		if (G_UNLIKELY (!is_ok (error)))
			return FALSE;
	}
	sig = mono_method_signature_internal (method);
	if (imethod->code_type == IMETHOD_CODE_UNKNOWN)
		imethod->code_type = mono_interp_jit_call_supported (method, sig) ? IMETHOD_CODE_COMPILED : IMETHOD_CODE_INTERP;
	return TRUE;
}

/*
 * mono_wasm_jit_pretransform:
 *
 *   Transform a DIRECT-residual callee BEFORE the JITted caller spills the call's arguments into the
 * GC-invisible per-thread scratch buffer. Transforming a cold method runs its class cctor — arbitrary
 * managed code that can allocate (triggering a GC that moves the call's reference args) and can itself
 * re-enter another residual/vcall on this thread (reusing the same scratch). Doing it HERE, while the ref
 * args still live in the GC-scanned ref shadow stack and the scratch is still free, means a GC moves the
	 * args safely and a nested residual can't clobber args that haven't been spilled yet. We also warm the
	 * residual's code_type/signature here because deciding AOT-vs-interp can run class init / AOT lookup.
	 * mono_wasm_jit_call_interp then finds the imethod already prepared and only does the GC-free marshal +
	 * interp_entry/do_jit_call, so the (now-spilled) scratch values stay valid. Mirrors the vcall path's pre-transform in
	 * mono_wasm_jit_vcall_resolve(_fslot) — the discipline the direct residual previously lacked.
 */
void
mono_wasm_jit_pretransform (MonoMethod *method)
{
	InterpMethod *imethod = mono_interp_get_imethod (method);
	ERROR_DECL (error);
	if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (method, imethod, error))) {
		/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
		 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
		 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
		 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
		 * (pretransform) */
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
		return;
	}
}

/*
 * mono_wasm_jit_call_interp:
 *
 *   Outbound DIRECT call from a wasm-JITted method to a callee that has no wasm f-slot (un-JITted, or
 * un-JIT-able because it has EH clauses / an unsupported shape). Instead of bailing the WHOLE caller
 * to the interpreter, the emitter (mini-wasm.c) keeps the caller JITted and lowers just this call to:
 * spill the args into the per-thread scratch buffer (the `this` pointer at slot 0 when instance, then
 * each scalar param value at slot i, 8 bytes per slot), call here, then load the result back from
 * buf+WJ_SCRATCH_RET_OFF. We drive the target through the interpreter via interp_entry().
 *
 * Only scalar (i32/i64/f32/f64; object/pointer = i32 on wasm32) args/returns reach here — the emitter
 * bails calls with vtype/byref args or returns before this, so interp_entry's by-pointer arg
 * convention (data.args[i] points at the value) matches the buffer layout directly.
 *
 * This is the real-Minecraft coverage lever: a hot JITted method almost always calls some cold or
 * un-JIT-able callee, and before this it bailed entirely; now only the cold call pays interp re-entry.
 *
 * RETURNS 1 if the callee threw (the thread resume-state is set and interp_entry did NOT write the
 * result — the JITted caller must abort immediately and let the interp unwind via `goto resume`),
 * else 0. This is the residual's exception path: real code throws (null checks, class init, ...), and
 * without it the JITted caller would read the stale scratch slot as a garbage object -> OOB.
 */
int
mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf)
{
	ERROR_DECL (error);
	if (G_UNLIKELY (!method)) {
		/* A vcall resolve stored a NULL target after raising an exception (null/corrupt receiver): the
		 * interp resume-state is already set, so just signal threw=1 and let the JITted caller bail. This
		 * also guards the mono_interp_get_imethod(NULL) deref below. */
		return 1;
	}
	/* SYNCHRONIZED_INNER wrappers are dummy "Shouldn't be called." throw stubs — the interpreter never
	 * executes them (interp inlines monitor ops during transform and never uses the wrapper pair), so
	 * interp-running one raw throws EEE. Substitute the wrapped method: its transform re-adds the
	 * monitor enter/exit, and Monitor is reentrant so the outer (JITted) wrapper already holding the
	 * lock is fine. Reached when a JITted outer synchronized wrapper residual-calls its not-yet-JITted
	 * inner (runtime-loaded classes: no AOT body to dispatch to). */
	if (G_UNLIKELY (method->wrapper_type == MONO_WRAPPER_OTHER)) {
		WrapperInfo *winfo = mono_marshal_get_wrapper_info (method);
		if (winfo && winfo->subtype == WRAPPER_SUBTYPE_SYNCHRONIZED_INNER) {
			MonoMethod *wrapped = winfo->d.synchronized_inner.method;
			if (wrapped && method->is_inflated) {
				ERROR_DECL (inflate_error);
				wrapped = mono_class_inflate_generic_method_checked (wrapped, mono_method_get_context (method), inflate_error);
				if (!is_ok (inflate_error)) { mono_error_cleanup (inflate_error); wrapped = NULL; }
			}
			if (wrapped)
				method = wrapped;
		}
	}
	InterpMethod *imethod = mono_interp_get_imethod (method);
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	InterpEntryData data;
	int idx = 0, i;

#ifdef HOST_BROWSER
	/* DIAG (type-confusion source): a JITted caller spilled its args into `buf` before this residual call.
	 * Using the callee SIGNATURE (so no 1A over-marking false positive), check every by-value REFERENCE arg
	 * (and `this`): its spilled value must be NULL or a plausible heap pointer. A non-pointer there — e.g. the
	 * -10 (0xfffffff6) that reached a (Biome[]) cast in Villager pathfinding — means the JIT put an int into a
	 * reference slot. Log (rate-limited) the callee + arg index so the SOURCE method is named; non-fatal. */
	{
		gsize _memsz = wj_memsz ();
		int _vidx = sig->hasthis ? 1 : 0;
		if (sig->hasthis) {
			gsize _t = (gsize) *(gpointer *) (buf + 0);
			if (G_UNLIKELY (_t != 0 && (_t < 1024 || _t >= _memsz || (_t & 3)))) {
				static int _zt = 0;
				if (_zt++ < 40) { char *fn = mono_method_get_full_name (method); printf ("WASM_JIT_BADREF_ARG callee=%s this=0x%x — JIT passed a non-pointer as `this` (type-confusion source)\n", fn, (unsigned) _t); fflush (stdout); g_free (fn); }
			}
		}
		for (int _p = 0; _p < (int) sig->param_count; ++_p) {
			if (m_type_is_byref (sig->params [_p]) || !MONO_TYPE_IS_REFERENCE (sig->params [_p]))
				continue;
			gsize _v = (gsize) *(gpointer *) (buf + (_vidx + _p) * 8);
			if (G_UNLIKELY (_v != 0 && (_v < 1024 || _v >= _memsz || (_v & 3)))) {
				static int _z = 0;
				if (_z++ < 40) { char *fn = mono_method_get_full_name (method); printf ("WASM_JIT_BADREF_ARG callee=%s arg#%d value=0x%x — JIT passed a non-pointer as a reference arg (type-confusion source)\n", fn, _p, (unsigned) _v); fflush (stdout); g_free (fn); }
			}
		}
	}
#endif

	if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (method, imethod, error))) {
		/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
		 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
		 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
		 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
		 * (call_interp (threw)) */
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
		return 1;
	}
	/* INLINE AOT FASTPATH: if the callee has AOT code, run it natively via do_jit_call directly,
	 * skipping interp_entry's InterpEntryData marshalling. Covers BOTH the direct-call residual and the
	 * vcall-fallback (which funnels here after resolve). Cached on code_type like the interp MINT_JIT_CALL.
	 * WJC_AOT_ROUTED / WJC_INTERP_ROUTED measure the split (how much the fastpath actually fires). */
	{
		extern int mono_wasm_jit_aot_residual;
		extern MonoMethod *mono_wasm_jit_ring [];
		extern int mono_wasm_jit_ring_count, mono_wasm_jit_ring_frozen;
		InterpMethodCodeType ct = imethod->code_type;
		if (ct == IMETHOD_CODE_UNKNOWN) {
			ct = mono_interp_jit_call_supported (method, sig) ? IMETHOD_CODE_COMPILED : IMETHOD_CODE_INTERP;
			imethod->code_type = ct;
		}
		if (mono_wasm_jit_aot_residual && ct == IMETHOD_CODE_COMPILED) {
			if (G_UNLIKELY (mono_wasm_jit_stats)) {
				mono_wasm_jit_count (WJC_RESIDUAL);
				mono_wasm_jit_count (WJC_AOT_ROUTED);
				if (!mono_wasm_jit_ring_frozen) { mono_wasm_jit_ring [mono_wasm_jit_ring_count & 127] = method; mono_wasm_jit_ring_count++; }
			}
			return wasm_jit_aot_call_lean (imethod, sig, buf);
		}
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_INTERP_ROUTED);
	}
	memset (&data, 0, sizeof (data));
	data.rmethod = imethod;
	if (sig->hasthis) {
		data.this_arg = *(gpointer *) (buf + 0);
		idx = 1;
	}
	for (i = 0; i < (int) sig->param_count; ++i) {
		guint8 *slot = buf + (idx + i) * 8;
		/* interp_entry's arg convention: for a BYREF param, data.args[i] IS the pointer (the byref
		 * value); for a by-value param it's a POINTER TO the value. The emitter spilled the scalar arg
		 * value into the slot, so for a byref the slot holds the pointer itself -> deref it. Getting
		 * this wrong corrupts memory: e.g. Unsafe.Add<byte> -> AddByteOffset(ref byte, IntPtr) would
		 * receive the address of the scratch slot as its `ref` and clobber the buffer. */
		data.args [i] = m_type_is_byref (sig->params [i]) ? *(gpointer *) slot : slot;
	}
	data.res = buf + WJ_SCRATCH_RET_OFF;
	/* Zero the result slot before interp_entry writes it. interp_entry marshals the return via
	 * stackval_to_data[_sign_ext], which writes sub-word returns NARROWLY: bool=1 byte and char=2 bytes
	 * always (neither has a case in stackval_to_data_sign_ext, so both fall through to the narrow
	 * stackval_to_data), plus i1/u1/i2/u2 in llvm_only mode. The JITted caller reads this slot with a
	 * full-width i32.load, so without clearing it first the high bytes carry stale data from a previous
	 * residual -> e.g. a `false` bool reads as a large nonzero value -> a class-name comparison lies ->
	 * wrong type resolution. The emitter also sign/zero-extends per the C# return type; this clear is the
	 * backstop for that. (8 bytes covers every scalar return; byte-wise memset = alignment-safe.) */
	memset (buf + WJ_SCRATCH_RET_OFF, 0, 8);
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		extern MonoMethod *mono_wasm_jit_ring [];
		extern int mono_wasm_jit_ring_count, mono_wasm_jit_ring_frozen;
		mono_wasm_jit_count (WJC_RESIDUAL);
		/* trail of recent residual callees (ring buffer); frozen at a detected failure so a post-crash
		 * dump shows the residuals up to the failure (not the crash-report flood that follows) */
		if (!mono_wasm_jit_ring_frozen) {
			mono_wasm_jit_ring [mono_wasm_jit_ring_count & 127] = method;
			mono_wasm_jit_ring_count++;
		}
	}
	/* interp_entry recognises this as the residual entry by data->res pointing at the scratch result
	 * slot (wj_entry_is_residual) — no thread flag to set/restore, so nothing can leak into nested or
	 * JSPI-interleaved interp entries, and a C++/wasm-EH unwind out of the callee needs no cleanup. */
	interp_entry (&data);
	/* If the callee threw, interp_entry returned with the resume-state set (and without writing the
	 * result). Tell the JITted caller so it aborts instead of dereferencing the stale scratch slot. */
	return get_context ()->has_resume_state ? 1 : 0;
}

/*
 * mono_wasm_jit_vcall_resolve:
 *
 *   Resolve the concrete override of `base_method` for `this_obj`'s runtime type, for a wasm-JITted
 * virtual/interface call. This is the ONLY thing done here — resolution can allocate / run a cctor /
 * populate a vtable slot, which can trigger a GC. The JITted code therefore calls this BEFORE it
 * spills the call's reference arguments into the (GC-invisible) per-thread scratch buffer: until the
 * spill the ref args still live in the GC-scanned ref shadow stack, so a GC here moves them safely.
 * `this_obj` is passed by value as a normal managed-call argument (GC-safe across the resolve). After
 * resolving, the JITted code spills the (now up-to-date) args and invokes via mono_wasm_jit_call_interp
 * — the same proven, MT-safe path the direct residual uses (no inline cache, no cross-thread table
 * slot). Returns the resolved MonoMethod* (a runtime structure, not a GC object, so it survives the
 * subsequent spill + interp entry).
 */
gpointer
mono_wasm_jit_vcall_resolve (MonoObject *this_obj, MonoMethod *base_method)
{
	if (G_UNLIKELY (!this_obj)) {
		/* Defensive backstop; the emitter normally materializes callvirt's null check before reaching here. */
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_get_exception_null_reference ());
		return NULL;
	}
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_objguard;
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		if (G_UNLIKELY (mono_wasm_jit_objguard))
			mono_wasm_jit_check_store ((guint8 *) this_obj, 5);
	}
#endif
	MonoMethod *target = mono_object_get_virtual_method_internal (this_obj, base_method);
	if (G_UNLIKELY (!target)) {
		/* See mono_wasm_jit_vcall_resolve_fslot: a NULL override means a null/corrupt receiver. Raise a
		 * catchable NRE and return NULL; the emitter's mono_wasm_jit_call_interp(NULL) fallback signals
		 * threw=1 so the caller bails, instead of dereferencing NULL below. */
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_get_exception_null_reference ());
		return NULL;
	}
	/* If the resolved override is synchronized, dispatch its SYNCHRONIZED wrapper (Monitor.Enter/Exit):
	 * the raw body has no monitor ops, and mono_wasm_jit_call_interp's mono_interp_get_imethod does NOT
	 * substitute the wrapper (unlike get_virtual_method) -> the body would run without the monitor and a
	 * notify/wait inside throws IllegalMonitorStateException. Do it HERE (before the JITted code spills
	 * the call's ref args into the GC-invisible scratch), so the wrapper-creation + transform below can
	 * allocate/GC while the ref args are still on the GC-scanned shadow stack. */
	if (G_UNLIKELY (target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED))
		target = mono_marshal_get_synchronized_wrapper (target);
	/* Pre-transform the override here too. The JITted code calls this BEFORE spilling the call's
	 * reference args into the GC-invisible scratch buffer; BOTH the resolve above AND fully preparing a
	 * cold method (transform + code_type/signature warmup) can allocate -> GC. Doing them now (while the
	 * ref args still live in the GC-scanned ref shadow stack) lets a GC move them safely.
	 * mono_wasm_jit_call_interp then finds the imethod already prepared and only does the GC-free marshal
	 * + interp_entry, so the (now-spilled) scratch pointers stay valid. (Without this, a first-use GC
	 * inside call_interp — after the spill — would stale the scratch refs, the latent hazard the direct
	 * residual rarely hit.) */
	InterpMethod *imethod = mono_interp_get_imethod (target);
	{
		ERROR_DECL (error);
		if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (target, imethod, error))) {
			/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
			 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
			 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
			 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
			 * (vcall_resolve) */
			extern void mono_wasm_jit_throw (MonoObject *exc);
			mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
			return NULL;
		}
	}
	return target;
}

/*
 * mono_wasm_jit_vcall_resolve_fslot:
 *
 *   Like mono_wasm_jit_vcall_resolve, but for the FAST virtual-dispatch path: resolve the override,
 * stash it at scratch+200 (for the call_interp fallback), and if the override is itself wasm-JITted
 * return its scalar `f` f-slot (after syncing THIS thread's function table so the slot is populated) —
 * so the JITted caller can call_indirect straight into the override's wasm, no interp re-entry. Returns
 * 0 when the override isn't JITted (or is a synchronized wrapper, which never gets an f-slot), so the
 * caller falls back to spill + call_interp (which applies the monitor wrapper). Same GC discipline as
 * vcall_resolve: the resolve+transform happen here, before the JITted code reads/pushes the call args.
 *
 *   INLINE CACHE (ic): a per-call-site 8-byte slot [i32 vtable, i32 InterpMethod*] in shared memory.
 * The virtual resolve (mono_object_get_virtual_method_internal + mono_interp_get_imethod, ~150ns) is a
 * pure function of (this->vtable, base_method); base_method is fixed per site, so we cache vtable ->
 * imethod. On a monomorphic hit we skip the whole resolve and just load the cached imethod — the
 * dominant per-call cost on BOTH the fast and residual paths. We deliberately keep the C call (+ the
 * fslot-check + per-thread sync) rather than a pure-inline wasm IC: the wasm function table is
 * PER-THREAD, so dispatching a cached f-slot inline (no sync) can call_indirect a slot absent on
 * another worker -> trap. Caching the *resolve* here is MT-safe (sync still runs per fast dispatch).
 * The (vtable, imethod) pair is published imethod-first then vtable-last; the i32-pair read can tear
 * under concurrent cross-type writes (benign on the single-threaded render hot path; MT-hardening =
 * i64.atomic — same TODO as the legacy inline-IC helpers).
 */
/* Field offset of InterpMethod.wasm_jit_fslot, for the emitter's inline virtual-IC fast path (which
 * loads imethod->wasm_jit_fslot directly in wasm). mini-wasm.c can't see the InterpMethod layout. */
int
mono_wasm_jit_imethod_fslot_off (void)
{
	return (int) G_STRUCT_OFFSET (InterpMethod, wasm_jit_fslot);
}

int
mono_wasm_jit_vcall_resolve_fslot (MonoObject *this_obj, MonoMethod *base_method, guint8 *scratch, gpointer ic)
{
	guint64 *icp = (guint64 *) ic;
	MonoVTable *vt;
	InterpMethod *imethod;
	MonoMethod *target;
	if (G_UNLIKELY (!this_obj)) {
		*(MonoMethod **) (scratch + 200) = NULL;
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_get_exception_null_reference ());
		return 0;
	}
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_objguard;
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		if (G_UNLIKELY (mono_wasm_jit_objguard))
			mono_wasm_jit_check_store ((guint8 *) this_obj, 5);
	}
#endif
	vt = this_obj->vtable;
	/* Read the (vtable | imethod<<32) pair ATOMICALLY. MC builds chunks on worker threads concurrently
	 * with the render thread, so the same vcall site's IC is written by multiple threads. A non-atomic
	 * i32-pair read can tear (match an old vtable but read a freshly-written imethod for a DIFFERENT
	 * receiver type) -> dispatch to the wrong override (NullPointerException) or a wrong-signature f-slot
	 * call_indirect (traps the worker -> GC can't suspend it). The i64 atomic makes the pair consistent. */
	extern int mono_wasm_jit_vcall_ic, mono_wasm_jit_stats;
	gboolean use_ic = mono_wasm_jit_vcall_ic;
	guint64 cached = use_ic ? (guint64) mono_atomic_load_i64 ((volatile gint64 *) icp) : 0;
	if (use_ic && G_LIKELY ((guint32) cached == (guint32) (gsize) vt)) {
		imethod = (InterpMethod *) (gsize) (guint32) (cached >> 32);   /* IC hit: skip the resolve + get_imethod */
		target = imethod->method;
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VIC_HIT);
	} else {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VIC_MISS);
		target = mono_object_get_virtual_method_internal (this_obj, base_method);
		if (G_UNLIKELY (!target)) {
			/* mono_object_get_virtual_method_internal returns NULL for an unresolvable receiver (object.c:
			 * "res can be null if klass is abstract and doesn't implement method"). The JIT emitter now
			 * null-checks the receiver before this call (matching MINT_CALLVIRT_FAST's NULL_CHECK), so a plain
			 * null receiver raises a catchable NRE and never reaches here; a NULL target now means a corrupt
			 * receiver. Raise a catchable NRE and store a NULL target so the residual's call_interp(NULL)
			 * signals threw=1 and the JITted caller bails — instead of dereferencing the NULL override into
			 * mono_marshal_get_synchronized_wrapper (g_assert(method) abort) or dispatching to garbage. */
			*(MonoMethod **) (scratch + 200) = NULL;
			extern void mono_wasm_jit_throw (MonoObject *exc);
			mono_wasm_jit_throw ((MonoObject *) mono_get_exception_null_reference ());
			return 0;
		}
		/* SIGNATURE-COMPATIBILITY GUARD. The wasm-JIT bakes the call_indirect functype from the CALL SITE's
		 * signature; the override resolved here must be compatible (same param count, this-ness, void-ness of
		 * return). A real override always is. An INCOMPATIBLE result (observed: ChunkHolder.broadcastChanges
		 * (WorldChunk)->void resolving to java.util.LinkedList.pop()->object) means the dispatch produced the
		 * wrong method — and baking that into the (this,args,rgctx) call_indirect is a FATAL wasm
		 * "function signature mismatch" that kills the worker and stalls the GC. Refuse it: name both methods +
		 * the receiver class, then raise a catchable error instead of trapping. Names base_method (otherwise only
		 * recoverable from the emitted wasm), so the next run pinpoints the bad dispatch. Miss-path only (the IC
		 * is never poisoned with an incompatible entry, so hits stay fast + safe). */
		{
			MonoMethodSignature *bs = mono_method_signature_internal (base_method);
			MonoMethodSignature *ts = mono_method_signature_internal (target);
			if (G_UNLIKELY (bs && ts && (bs->param_count != ts->param_count || bs->hasthis != ts->hasthis ||
			    (bs->ret->type == MONO_TYPE_VOID) != (ts->ret->type == MONO_TYPE_VOID)))) {
				char *bn = mono_method_get_full_name (base_method);
				char *tn = mono_method_get_full_name (target);
				printf ("WASM_JIT_VCALL_MISRESOLVE base=%s -> resolved=%s (receiver klass=%s.%s) — incompatible signature (params %d vs %d); raising instead of trapping\n",
					bn, tn, m_class_get_name_space (mono_object_class (this_obj)), m_class_get_name (mono_object_class (this_obj)),
					(int) bs->param_count, (int) ts->param_count);
				fflush (stdout);
				g_free (bn); g_free (tn);
				*(MonoMethod **) (scratch + 200) = NULL;
				extern void mono_wasm_jit_throw (MonoObject *exc);
				mono_wasm_jit_throw ((MonoObject *) mono_get_exception_execution_engine ("wasm-jit virtual mis-resolution (signature mismatch)"));
				return 0;
			}
		}
		if (G_UNLIKELY (target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED))
			target = mono_marshal_get_synchronized_wrapper (target);
		else if (G_UNLIKELY (m_class_is_valuetype (target->klass) && target->wrapper_type == MONO_WRAPPER_NONE)) {
			/* Boxed-valuetype virtual/interface receiver: the resolved override's body expects an UNBOXED `this`
			 * (&data = boxed + sizeof(MonoObject)), but every wasm-JIT vcall path (f-slot fast, vcall_aot, and the
			 * call_interp residual) forwards the RAW boxed receiver loaded from the callsite vreg. The interp's own
			 * dispatch unboxes manually (MINT_CALLVIRT_FAST: `if valuetype -> mono_object_unbox_internal`); we can't
			 * adjust the JIT-loaded `this`, so route through the unbox wrapper instead — it does the +sizeof
			 * (MonoObject) and tail-calls the method, so passing the boxed `this` is correct on ALL three paths
			 * (and the IC caches the wrapper's imethod, so hits stay correct too). Without this, a valuetype
			 * override reads/writes fields against the object header -> garbage / type confusion. */
			target = mono_marshal_get_unbox_wrapper (target);
		}
		imethod = mono_interp_get_imethod (target);
		{
			ERROR_DECL (error);
			if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (target, imethod, error))) {
				/* A cold callee's first transform/signature/code-type warmup failed (cctor threw / unloadable type /
				 * bad IL). Deliver it as a catchable managed exception via the wasm-JIT throw path instead of
				 * mono_error_assert_ok's abort() (which kills the worker). Under CPPEH this C++-unwinds and never
				 * returns; otherwise it installs resume-state and the JITted caller's pending-exception check unwinds.
				 * (vcall_resolve_fslot) */
				extern void mono_wasm_jit_throw (MonoObject *exc);
				mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
				return 0;
			}
		}
		if (use_ic)
			mono_atomic_store_i64 ((volatile gint64 *) icp,
				(gint64) (((guint64) (guint32) (gsize) imethod << 32) | (guint32) (gsize) vt));
	}
	*(MonoMethod **) (scratch + 200) = target;   /* for the call_interp fallback (past RET_OFF(192)+8) */
	/* Force-JIT the hot override so the NEXT vcall to it takes the fast f-slot path instead of falling
	 * back to interp_entry. An override reached ONLY through JITted callers (this path) never accumulates
	 * hits at the interp dispatch sites (MINT_CALL/MINT_CALLVIRT_FAST), so without this it would never
	 * cross the auto-JIT threshold — leaving most virtual calls stuck on the slow residual. Same hit
	 * counter + threshold + eligibility/bail handling as the interp-dispatch trigger (no-op once the
	 * override is JITted (slot>0) or permanently bailed (slot==-1); honours MONO_WASM_JIT_AUTO). */
	gboolean had_fslot = imethod->wasm_jit_fslot > 0;   /* JITted on entry (vfast_had) vs compiled by the call below (vfast_new) */
	if (imethod->wasm_jit_fslot <= 0)
		wasm_jit_maybe_compile (imethod);
	if (imethod->wasm_jit_fslot > 0) {
		extern void mono_wasm_jit_sync_thread (void);
		extern int mono_wasm_jit_slot_live (int slot);
		mono_wasm_jit_sync_thread ();
		/* Only return the f-slot if THIS thread actually instantiated that module. sync_thread can fail to
		 * instantiate on a worker (OOM/CompileError under pressure) while it succeeded on the compiling
		 * thread; the slot then holds a jiterpreter placeholder and call_indirect-ing it traps the worker.
		 * Fall through to the interp residual (call_interp) instead. */
		if (mono_wasm_jit_slot_live (imethod->wasm_jit_fslot)) {
			if (G_UNLIKELY (mono_wasm_jit_stats)) {
				mono_wasm_jit_count (WJC_FASTVCALL);
				mono_wasm_jit_count (had_fslot ? WJC_VFAST_HAD : WJC_VFAST_NEW);
			}
			return imethod->wasm_jit_fslot;
		}
	}
	/* RE-WRITE the resolved target AFTER the (re-entrant) wasm_jit_maybe_compile + mono_wasm_jit_sync_thread
	 * above. Those can drive a NESTED wasm-JIT vcall (the compile/instantiate runs cctors / class setup ->
	 * managed code), and that nested call reuses THIS per-thread scratch buffer, overwriting scratch+200 with
	 * its OWN target. The fallback below (vcall_aot_target reads scratch+200 to pick the AOT body; call_interp
	 * reads it as the residual target) must see THIS call's target, not the nested one — otherwise it bakes the
	 * wrong AOT body into a call_indirect with this call site's functype and fatally signature-mismatches.
	 * (Observed: ChunkHolder.flushUpdates clobbered to java.util.LinkedList.pop.) The early write at the top of
	 * this block is for paths that return before here; the f-slot fast path returns above and never reads
	 * scratch+200, so the re-write only matters for this residual/vcall_aot fallback. */
	*(MonoMethod **) (scratch + 200) = target;
	/* Fallback to interp (call_interp). Split by reason: slot==-1 is permanently un-JITtable (EH/unsupported
	 * opcode/eligibility bail — will NEVER take the fast path); anything else (0 counting, -2..-5 retriable)
	 * is still below the JIT threshold and SHOULD become fast once it crosses (threshold latency). */
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		if (imethod->wasm_jit_slot == -1) {
			mono_wasm_jit_count (WJC_VFB_PERM);
			switch (imethod->wasm_jit_bail) {            /* weighted breakdown of WHY it can't JIT (see MonoWasmJitResult.bail) */
			case 0:  mono_wasm_jit_count (WJC_VPERM_AOT); break;   /* slot==-1 + bail==0: not wasm-jitted because it has native AOT code (not an emitter bail) */
			case -2: mono_wasm_jit_count (WJC_VPERM_EH); break;
			case -3: mono_wasm_jit_count (WJC_VPERM_SIG); break;
			case -5: mono_wasm_jit_count (WJC_VPERM_LDADDR); break;
			case -6: mono_wasm_jit_count (WJC_VPERM_LCMP); break;
			case -7: mono_wasm_jit_count (WJC_VPERM_BYREF); break;
			case -8: mono_wasm_jit_count (WJC_VPERM_GSHARED); break;
			case -9: mono_wasm_jit_count (WJC_VPERM_SYNC); break;
			case -10: mono_wasm_jit_count (WJC_VPERM_EHOTHER); break;
			default:
				if (imethod->wasm_jit_bail > 0) mono_wasm_jit_count (WJC_VPERM_OTHEROP);
				else mono_wasm_jit_count (WJC_VPERM_OTHER);     /* genuinely other IR shape */
				break;
			}
		} else {
			mono_wasm_jit_count (WJC_VFB_THRESH);
			/* split by slot state: distinguishes a genuinely-cold target (slot 0, interp is acceptable) from
			 * a HOT method whose island won't close (slot -2 parked / -3 retry) — the latter interprets every
			 * call and is the real interp-residual driver, fixable only by closing its island, not by residual. */
			switch (imethod->wasm_jit_slot) {
			case 0:                    mono_wasm_jit_count (WJC_VFB_COLD); break;
			case WASM_JIT_SLOT_PARKED: mono_wasm_jit_count (WJC_VFB_PARKED); break;
			case WASM_JIT_SLOT_RETRY:  mono_wasm_jit_count (WJC_VFB_RETRY); break;
			default: break;
			}
		}
	}
	return 0;
}

/* Fast AOT-vcall dispatch (MONO_WASM_JIT_VCALL_AOT). Called by JITted code ONLY after a vcall_resolve_fslot
 * f-slot miss, which left the resolved override MonoMethod* at scratch+200. If that override is AOT-backed,
 * stash its AOT call-target table index at scratch+212 and its rgctx at scratch+216 and return 1 — the
 * JITted caller then call_indirects the AOT body directly (this+args+rgctx native ABI), skipping the
 * residual's double marshalling + do_jit_call frame. Returns 0 if not AOT-backed (caller takes the
 * residual). Same eligibility (no_wrapper / gsharedvt-variable bail + static-rgctx recovery) as the
 * INLINE_AOT direct path, via the shared mono_wasm_jit_aot_call_target.
 *
 * Return code (the override is resolved at runtime, but the call_indirect functype is baked at emit time,
 * so the JITted caller picks one of two variants by this value):
 *   0 = not AOT-backed (take the residual)
 *   1 = AOT-backed, body HAS the trailing extra arg -> call (this,args,rgctx)->ret
 *   2 = AOT-backed, body has NO trailing arg (exempt wrapper kind) -> call (this,args)->ret */
int
mono_wasm_jit_vcall_aot_target (guint8 *scratch)
{
	extern gboolean mono_wasm_jit_aot_call_target (MonoMethod *m, gpointer *out_addr, gpointer *out_rgctx, gboolean *out_has_extra_arg);
	MonoMethod *target = *(MonoMethod **) (scratch + 200);
	gpointer addr = NULL, rgctx = NULL;
	gboolean has_extra_arg = TRUE;
	if (!target)
		return 0;
	gboolean ok = mono_wasm_jit_aot_call_target (target, &addr, &rgctx, &has_extra_arg);
	/* aot_call_target -> init_jit_call_info compiles the callee (+ its gsharedvt-out wrapper), which can run
	 * cctors / class setup -> a NESTED wasm-JIT vcall that reuses THIS per-thread scratch and overwrites
	 * scratch+200 with its own target. We captured `target` in a local before that, but the residual fallback
	 * (taken when ok==FALSE) reads scratch+200 as its call_interp target — so restore it here, else the residual
	 * would invoke the nested call's target with this call site's args. (Same per-thread-scratch re-entrancy bug
	 * that clobbered ChunkHolder.flushUpdates -> LinkedList.pop in resolve_fslot.) */
	*(MonoMethod **) (scratch + 200) = target;
	if (!ok)
		return 0;
	*(gpointer *) (scratch + 212) = addr;
	*(gpointer *) (scratch + 216) = rgctx;
	if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VCALL_AOT_FAST);
	/* DIAGNOSTIC: name the resolved override taking the AOT-fast path, deduped per distinct target (probabilistic
	 * hash dedup, so ~one line per method instead of per call). The JITted caller does the bare call_indirect
	 * with the baked at/at_ne functype IMMEDIATELY after this returns; if that traps "function signature
	 * mismatch", THIS is the last WASM_JIT_VCALL_AOT line printed -> it names the culprit. kind 1 = at
	 * (this,args,rgctx), kind 2 = at_ne (this,args). The attributes pin down WHY the baked signature is wrong
	 * (inflated/gshared, wrapper kind, etc.). Gated on verbose; the dedup keeps it bounded despite ~24M calls. */
	{
		extern int mono_wasm_jit_verbose;
		/* PER-CALL diagnostic -> gated at verbose>=3 (per-call trace tier), NOT >=2: this fires on the
		 * vcall-dispatch fast path (~24M calls), and each printf+fflush is a synchronous console write that
		 * in-loop measurably taints the fps bench (jit30: ~66 lines/frame -> ~40ms/frame, dropped 6.9->5.4).
		 * A verbose<=2 stats/bail run must stay clean. Even at >=3, bound it to ~one line per target with a
		 * 2-WAY seen-set: the old direct-mapped 8192 set let two hash-colliding hot iterators ping-pong-evict
		 * each other (the observed ~9748+~9745 twin counts) -> 10k+ reprints each. */
		if (G_UNLIKELY (mono_wasm_jit_verbose >= 3)) {
			static __thread MonoMethod *wj_vaot_seen [8192][2];   /* per-thread 2-way seen-set (64KB) */
			guint h = (guint) (((gsize) target >> 4) & 8191);
			if (wj_vaot_seen [h][0] != target && wj_vaot_seen [h][1] != target) {
				wj_vaot_seen [h][1] = wj_vaot_seen [h][0];
				wj_vaot_seen [h][0] = target;
				char *fn = mono_method_get_full_name (target);
				printf ("WASM_JIT_VCALL_AOT target=%s kind=%d extra_arg=%d inflated=%d wrapper=%d rgctx=%p\n",
					fn, has_extra_arg ? 1 : 2, has_extra_arg, target->is_inflated, target->wrapper_type, rgctx);
				fflush (stdout);   /* the caller's bare call_indirect may trap immediately after -> make this line visible first */
				g_free (fn);
			}
		}
	}
	return has_extra_arg ? 1 : 2;
}

/* After the throw's FIRST PASS (mono_handle_exception has already run finally clauses + installed the
 * interp/il_state resume-state for the handler), skip the native JITted frames between here and that
 * handler in one shot via a C++/wasm-EH native unwind. The unwind is caught at the nearest landing pad —
 * an in-method wasm catch or the interp e-thunk boundary (mono_wasm_jit_invoke_caught), which relies on
 * the resume-state pass 1 already set. NEVER returns. Called only after pass-1, so it does NOT itself
 * touch thrown_exc/resume-state — pass 1 owns that, exactly like mini_llvmonly_throw_exception. */
static void
wasm_jit_cpp_unwind (MonoObject *exc)
{
	/* Stash the managed exception in jit_tls->thrown_exc so a JITted in-method `catch <x.e>` landing pad
	 * can load it (mini_llvmonly_load_exception) for the type check + OP_GET_EX_OBJ. pass 1 set only the
	 * interp/il_state resume-state (for an OUTER handler), not thrown_exc. */
	MonoJitTlsData *jit_tls = mono_get_jit_tls ();
	if (!jit_tls->thrown_exc)
		jit_tls->thrown_exc = mono_gchandle_new_internal (exc, TRUE);
	mono_llvm_start_native_unwind ();
	g_assert_not_reached ();   /* the C++ throw above does not return */
}

/*
 * mono_wasm_jit_raise_corlib:
 *
 *   Raise a corlib exception from a wasm-JITted method — the cold path of an OP_COND_EXC_* check
 * (overflow / divide-by-zero / array-bounds / null / invalid-cast). exc_id selects the exception (the
 * emitter's name->id map below). Runs pass-1 (mono_handle_exception) to find the handler, then delivers
 * the exception by a C++/wasm-EH native unwind (wasm_jit_cpp_unwind) to the nearest landing pad — an
 * in-method wasm catch or the interp e-thunk boundary. Never returns.
 */
void
mono_wasm_jit_raise_corlib (int exc_id)
{
	MonoException *ex;
	MonoContext ctx;
	switch (exc_id) {
	case 0:  ex = mono_get_exception_overflow (); break;
	case 1:  ex = mono_get_exception_divide_by_zero (); break;
	case 2:  ex = mono_get_exception_index_out_of_range (); break;
	case 3:  ex = mono_get_exception_invalid_cast (); break;
	case 4:  ex = mono_get_exception_null_reference (); break;
	case 5:  ex = mono_get_exception_arithmetic (); break;
	case 6:  ex = mono_get_exception_array_type_mismatch (); break;
	default: ex = mono_get_exception_arithmetic (); break;   /* unreachable: emitter only emits ids 0-6 */
	}
	memset (&ctx, 0, sizeof (ctx));
	MONO_CONTEXT_SET_SP (&ctx, &ctx);
	mono_handle_exception (&ctx, (MonoObject *) ex);   /* pass 1: run finallys + install resume-state for the handler */
	if (MONO_CONTEXT_GET_IP (&ctx) != 0) {
		/* handler is in native code (not expected for a JITted method invoked from interp) */
		mono_restore_context (&ctx);
		g_assert_not_reached ();
	}
	/* pass 1 either installed resume-state (handler in an interp/outer frame) OR stopped at a JITted island
	 * whose OWN wasm landing pad catches the cpp unwind (e.g. a checked-op corlib exception caught by the
	 * SAME JITted method). Either way it is delivered by the native unwind below, which never returns. */
	wasm_jit_cpp_unwind ((MonoObject *) ex);   /* C++-unwind to the nearest landing pad; never returns */
}

/*
 * mono_wasm_jit_throw:
 *
 *   Throw an arbitrary exception object from a wasm-JITted method (OP_THROW: `throw expr`). Same model
 * as mono_wasm_jit_raise_corlib: run pass-1 (mono_handle_exception) to find the handler, then C++-unwind.
 * pass-1 installs interp resume-state for a handler in an interp/outer frame, OR (under EH-in-JIT) stops
 * at a JITted island — including this method's OWN frame, since EH methods are now JITted — and the wasm
 * landing pad runs the catch. `throw` is the #1 leaf-method JIT blocker in IKVM (null/argument-validation
 * throws are everywhere), so supporting it lets throwing leaves JIT and the RESIDUAL=0 islands resolve.
 */
static void
wasm_jit_throw_internal (MonoObject *exc, gboolean rethrow)
{
	ERROR_DECL (error);
	MonoContext ctx;
	if (G_UNLIKELY (!exc))
		exc = (MonoObject *) mono_get_exception_null_reference ();
	/* fresh throw (`throw expr`): clear any captured trace so mono_handle_exception rebuilds it from the
	 * current throw site (mirrors interp_throw). A RETHROW (`throw;`) or a finally re-raise is a CONTINUATION
	 * of an already-in-flight exception, not a new throw site, so PRESERVE the trace pass-1 captured at the
	 * original throw (mirrors interp_rethrow / mini_llvmonly_rethrow_exception, which skip the rebuild). */
	if (!rethrow && mono_object_isinst_checked (exc, mono_defaults.exception_class, error)) {
		MonoException *mex = (MonoException *) exc;
		mex->stack_trace = NULL;
		mex->trace_ips = NULL;
	}
	mono_error_cleanup (error);
	memset (&ctx, 0, sizeof (ctx));
	MONO_CONTEXT_SET_SP (&ctx, &ctx);
	mono_handle_exception (&ctx, exc);   /* pass 1: run finallys + install resume-state for the handler */
	if (MONO_CONTEXT_GET_IP (&ctx) != 0) {
		mono_restore_context (&ctx);
		g_assert_not_reached ();
	}
	/* pass 1 either installed resume-state (handler in an interp/outer frame, reached when the cpp unwind
	 * hits the interp->JIT boundary) OR stopped at a JITted island whose OWN wasm landing pad catches the
	 * cpp unwind (e.g. a `throw` caught by the SAME JITted method). Either way the exception is delivered by
	 * the native unwind below, which never returns. */
	wasm_jit_cpp_unwind (exc);   /* C++-unwind to the nearest landing pad; never returns */
}

void
mono_wasm_jit_throw (MonoObject *exc)
{
	wasm_jit_throw_internal (exc, FALSE);   /* fresh throw: rebuild the stack trace */
}

/* IL `rethrow` (OP_RETHROW) and the finally re-raise (mono_wasm_jit_endfinally_rethrow): re-raise the
 * caught/in-flight exception while PRESERVING its original stack trace (a continuation, not a new throw). */
void
mono_wasm_jit_rethrow (MonoObject *exc)
{
	wasm_jit_throw_internal (exc, TRUE);    /* rethrow / re-raise: keep the captured trace */
}

/* --- interp->JIT entry-thunk boundary (cppeh) -------------------------------------------------------
 * When CPPEH is on, a JITted method that throws (or whose callees throw) C++/wasm-EH-unwinds through all
 * its frames with no per-call resume-state check. The unwind must be CAUGHT here — the interp e-thunk
 * boundary is the JITted region's outermost landing pad — and converted back to an interp exception so
 * the interp's own EH (and any handler in the calling interp frame or above) takes over. Mirrors
 * do_jit_call's LLVMONLY_INTERP path. */
typedef struct { gpointer thunk; gpointer args; gpointer ret; } WasmJitEThunkArgs;

static void
wasm_jit_ethunk_cb (gpointer arg)
{
	WasmJitEThunkArgs *a = (WasmJitEThunkArgs *) arg;
	((void (*)(void *, void *)) a->thunk) (a->args, a->ret);
}

/* Invoke a JITted method's entry thunk (slot) under a C++ catch. On a caught C++/wasm-EH unwind, ENSURE
 * the interp resume-state is installed so the caller's CHECK_RESUME_STATE resumes at the handler (in
 * this interp frame, or re-propagates upward) — mirroring do_jit_call's LLVMONLY_INTERP thrown handling
 * EXACTLY. It must NOT THROW_EX a fresh exception: pass 1 of the throw
 * (mono_handle_exception, run before the native unwind) already installed the resume-state / il_state,
 * and a second handling would double-process it (the resume_state.ex_gchandle assertion). Also restores
 * the C-stack SP — the unwound JITted frames' native EH cleanup is not guaranteed. */
/* Cheap always-on C-stack balance invariant. JITted frames live on the emscripten C stack now: on a
 * CLEAN (non-thrown) return every JIT frame ran its stackRestore, so the SP must be exactly back at
 * the pre-invoke snapshot; a drift means a JIT return path is missing EMIT_REF_LEAVE. On a thrown
 * unwind the native EH landing pads restore the SP, but we resync from the snapshot regardless —
 * belt-and-suspenders that also covers codegen bugs. Warn ONCE (racy global flag is fine for a
 * diagnostic) rather than abort. O(1) per interp->JIT boundary, not per store. */
static void
wj_shadow_balance_warn (const char *what)
{
	static gboolean warned = FALSE;
	if (warned)
		return;
	warned = TRUE;
	g_warning ("wasm-jit: C-stack %s at interp->JIT boundary — a JIT frame push/pop imbalance (was caught by MONO_WASM_JIT_STOREGUARD)", what);
}

/* Names the leaking method on a clean-return imbalance. A CALLEE's leak is corrected by the caller's own
 * stackRestore(entry_sp), so for the drift to reach the interp->JIT boundary the boundary-invoked `method`
 * ITSELF must have a return path missing EMIT_REF_LEAVE — i.e. THIS is the culprit. Rate-limited. */
static void
wj_shadow_balance_warn_m (const char *method, int leak)
{
	static int n = 0;
	if (n >= 40)
		return;
	n++;
	g_warning ("wasm-jit: C-stack imbalance on CLEAN return from %s (leak=%d bytes) — a return path is missing EMIT_REF_LEAVE; resynced at boundary", method, leak);
}

void
mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret)
{
	/* JIT frames live on the emscripten C stack: snapshot the SP so a caught C++ unwind (whose
	 * native landing pads may or may not have run for the torn-through JIT frames) can be resynced
	 * precisely, and a clean return can be balance-checked. */
	uintptr_t c_sp_saved = emscripten_stack_get_current ();
	gboolean thrown = FALSE;
	WasmJitEThunkArgs a;
	a.thunk = (gpointer) (intptr_t) slot;
	a.args = args;
	a.ret = ret;

	/* AOT-style in-method EH (milestone 2c+): the IL_STATE LMFExt that makes a JITted EH method visible to
	 * mono's exception pass-1 is now pushed by the method's OWN prologue (mono_wasm_jit_enter_island),
	 * covering BOTH this interp->JIT boundary AND direct JIT->JIT f-slot calls (which bypass the boundary).
	 * Here we only snapshot the island-stack depth so a C++ unwind that ESCAPES the method (caught below,
	 * skipping the method's own leave_island) resets it — belt-and-suspenders against a leaked push. */
	(void) method;
	extern int mono_wasm_jit_island_sp_save (void);
	extern void mono_wasm_jit_island_sp_restore (int sp);
	int island_sp_saved = mono_wasm_jit_island_sp_save ();

	mono_llvm_catch_exception (wasm_jit_ethunk_cb, &a, &thrown);

	mono_wasm_jit_island_sp_restore (island_sp_saved);
	/* DIAG (loot-NPE bisection): on a catch-taken, non-thrown return, log the return value + the C-stack
	 * balance. bal!=0 => a JIT frame's EMIT_REF_LEAVE didn't restore the SP across the catch. Bounded. */
	{ extern int mono_wasm_jit_verbose; extern __thread int mono_wasm_jit_eh_caught_flag;
	  if (mono_wasm_jit_verbose >= 2 && mono_wasm_jit_eh_caught_flag) { static int _ivc = 0;
		int bal = (int) ((intptr_t) emscripten_stack_get_current () - (intptr_t) c_sp_saved);
		if (_ivc++ < 200) { printf ("INVCAUGHT caught ret=%d thrown=%d spbal=%d\n", ret ? *(gint32 *) ret : -999999, thrown, bal); } }
	  mono_wasm_jit_eh_caught_flag = 0; }
	if (!thrown) {
		/* clean return: every JIT frame ran its stackRestore (and every C callee restored its own frame),
		 * so the SP must be exactly back at the snapshot. If not, `method`'s codegen has a return path
		 * missing EMIT_REF_LEAVE (a callee leak is corrected by the caller's own restore, so the boundary
		 * only sees the boundary method's own leak). Name it, then RESYNC so the drift can't accumulate. */
		uintptr_t now_sp = emscripten_stack_get_current ();
		if (G_UNLIKELY (now_sp != c_sp_saved)) {
			char *fn = mono_method_get_full_name (method);
			wj_shadow_balance_warn_m (fn, (int) ((intptr_t) c_sp_saved - (intptr_t) now_sp));
			g_free (fn);
			stackRestore (c_sp_saved);
		}
		return;
	}
	/* thrown: the native EH landing pads restore the SP as the C++ unwind ran, but the torn-through JIT
	 * frames had no cleanup of their own — resync from the snapshot regardless (idempotent when already
	 * balanced). Above-snapshot means an underflow (a restore ran past its frame). */
	if (G_UNLIKELY (emscripten_stack_get_current () > c_sp_saved))
		wj_shadow_balance_warn ("underflowed on unwind");
	stackRestore (c_sp_saved);
	ThreadContext *context = get_context ();
	MonoJitTlsData *jit_tls = mono_get_jit_tls ();
	if (context->has_resume_state) {
		/* pass 1 already installed the interp resume-state. Drop the now-consumed native exception UNLESS the
		 * handler is a JITted wasm island still ABOVE us — handler_frame==NULL (the interp will
		 * need_native_unwind to it) with no real il_state — because that island's landing-pad dispatch loads
		 * the exception from jit_tls->thrown_exc. Clearing it here as a multi-level INNER invoke_caught (e.g.
		 * nested JITted packet handlers between the throw and the catching method) lost the exception, so the
		 * outer island's dispatch returned -1 and it escaped uncaught. A cooperative interp handler
		 * (handler_frame != NULL) resumes without thrown_exc, and a real AOT il_state frame carries its own
		 * resume_state.ex_gchandle — both can drop it. */
		if (!(context->handler_frame == NULL && jit_tls->resume_state.il_state == NULL))
			mini_llvmonly_clear_exception ();
		return;   /* pass 1 already installed the interp resume-state (handler here or above) */
	}
	if (jit_tls->resume_state.il_state) {
		/* handler is a catch clause in an AOTed frame above us: keep the il_state, just tell the interp
		 * to unwind (need_native_unwind -> re-throw C++ to that frame). */
		context->has_resume_state = TRUE;
		context->handler_frame = NULL;
		mini_llvmonly_clear_exception ();   /* il_state carries its own resume_state.ex_gchandle; drop thrown_exc */
		return;
	}
	/* no handler installed yet (a bare C++ unwind with no pass-1, shouldn't happen for our throws):
	 * convert the pending native exception into resume-state (the boundary catch-all). */
	MonoObject *obj = mini_llvmonly_load_exception ();
	g_assert (obj);
	mini_llvmonly_clear_exception ();
	MonoContext ctx;
	memset (&ctx, 0, sizeof (ctx));
	MONO_CONTEXT_SET_SP (&ctx, &ctx);
	mono_handle_exception (&ctx, obj);
	g_assert (get_context ()->has_resume_state);
}

/* --- AOT-style EH milestone 2b: make the JITted EH frame VISIBLE to mono's exception pass-1 ------------
 * The innermost active JITted-EH invocation's MonoMethodILState (pushed as an IL_STATE LMFExt at the
 * interp->JIT boundary in mono_wasm_jit_invoke_caught). The JITted wasm keeps ->il_offset current (via
 * mono_wasm_jit_set_il_offset, emitted at each bb) so pass-1 (mono_handle_exception) finds THIS method's
 * catch as the nearest handler and STOPS there — running no outer frames' finally clauses — instead of
 * walking past the (otherwise unwinder-invisible) JITted frame. mini-exceptions.c reads this to recognise
 * the wasm-JIT il_state and defer the actual catch to the wasm landing pad (no interp resume). */
__thread MonoMethodILState *mono_wasm_jit_cur_island_il_state = NULL;

/* --- AOT-style in-method EH: per-thread "island" stack ------------------------------------------------
 * A JITted EH method's PROLOGUE calls mono_wasm_jit_enter_island(method) to push an IL_STATE LMFExt that
 * makes the method visible to mono's exception pass-1 (so pass-1 finds ITS catch as the nearest handler,
 * stops there, and runs no OUTER finally clauses); every exit calls mono_wasm_jit_leave_island() to pop.
 * Pushing in the PROLOGUE (not at the interp->JIT boundary) covers direct JIT->JIT f-slot calls too — the
 * fix for the catch-il_state-at-scale ClassCastException. The il_state data[] is zeroed: the GC scans it
 * (in bounds), finds NULL -> no roots from this frame, which is correct because the method's live refs
 * are in the separately-rooted GC ref shadow stack. */
#define WJ_ISLAND_DATA      256   /* max args+locals; the emitter bails EH methods exceeding this */
#define WJ_ISLAND_CHUNK     256   /* island frames per chunk (chunks never move -> LMF pointers stay stable) */
typedef struct {
	MonoLMFExt ext;
	MonoMethodILState *prev;
	int finally_sp;   /* wj_finally_exc_sp at this island's entry; leave/restore trims back to it */
	guint8 st [sizeof (MonoMethodILState) + WJ_ISLAND_DATA * sizeof (gpointer)];
} WjIsland;
/* Chunked, pointer-stable island stack: enter only ever reaches one past the current max, so chunks grow
 * incrementally; existing WjIsland (and the LMFExt linked into the LMF chain) never move on growth. There
 * is no fixed depth cap (the C/wasm stack overflows long before this would), so deep nested JITted-EH
 * recursion can no longer abort() the worker. */
static __thread WjIsland **wj_island_chunks = NULL;
static __thread int wj_island_nchunks = 0;
static __thread int wj_island_sp = 0;

/*
 * Per-thread stack of exceptions captured by an active in-method finally/fault running on the EXCEPTION
 * path. jit_tls->thrown_exc is a SINGLE per-thread gchandle; a nested throw+catch inside a finally body
 * (or a JITted callee the finally invokes) clears it (mono_wasm_jit_eh_dispatch's catch path frees it),
 * which would destroy the exception the finally must re-raise at OP_ENDFINALLY -> a NULL load -> wasm
 * trap -> worker death. So each FINALLY/FAULT dispatch MOVES thrown_exc into this stack (taking ownership
 * of the gchandle) and clears thrown_exc; OP_ENDFINALLY pops it and re-raises. Entries left behind by a
 * finally body that itself threw-and-escaped (never reaching its OP_ENDFINALLY) are reclaimed when the
 * owning island frame is left (trim to the island's recorded finally_sp).
 */
static __thread MonoGCHandle *wj_finally_exc = NULL;   /* gchandles (NULL == empty), same type as jit_tls->thrown_exc */
static __thread int wj_finally_exc_cap = 0;
static __thread int wj_finally_exc_sp = 0;

static WjIsland *
wj_island_at (int i)
{
	int c = i / WJ_ISLAND_CHUNK;
	if (G_UNLIKELY (c >= wj_island_nchunks)) {
		int k, nc = wj_island_nchunks ? wj_island_nchunks * 2 : 4;
		while (c >= nc)
			nc *= 2;
		wj_island_chunks = (WjIsland **) g_realloc (wj_island_chunks, nc * sizeof (WjIsland *));
		for (k = wj_island_nchunks; k < nc; ++k)
			wj_island_chunks [k] = (WjIsland *) g_malloc0 (sizeof (WjIsland) * WJ_ISLAND_CHUNK);
		wj_island_nchunks = nc;
	}
	return &wj_island_chunks [c][i % WJ_ISLAND_CHUNK];
}

/* Move the in-flight exception (gchandle ownership) onto the finally save stack; caller then clears
 * thrown_exc. */
static void
wj_finally_push (MonoGCHandle gchandle)
{
	if (G_UNLIKELY (wj_finally_exc_sp >= wj_finally_exc_cap)) {
		wj_finally_exc_cap = wj_finally_exc_cap ? wj_finally_exc_cap * 2 : 32;
		wj_finally_exc = (MonoGCHandle *) g_realloc (wj_finally_exc, wj_finally_exc_cap * sizeof (MonoGCHandle));
	}
	wj_finally_exc [wj_finally_exc_sp++] = gchandle;
}

/* Pop the exception a finally must re-raise (NULL if the stack is empty). Ownership transfers to the caller. */
static MonoGCHandle
wj_finally_pop (void)
{
	if (wj_finally_exc_sp <= 0)
		return NULL;
	return wj_finally_exc [--wj_finally_exc_sp];
}

/* Free any finally-save entries this island's frame pushed but never popped (a finally body that threw
 * and escaped past its own OP_ENDFINALLY), restoring the save stack to the island's entry baseline. */
static void
wj_finally_trim (int baseline)
{
	while (wj_finally_exc_sp > baseline) {
		MonoGCHandle h = wj_finally_exc [--wj_finally_exc_sp];
		if (h)
			mono_gchandle_free_internal (h);
	}
}

void
mono_wasm_jit_enter_island (MonoMethod *method)
{
	WjIsland *is;
	MonoMethodILState *il;
	is = wj_island_at (wj_island_sp++);
	il = (MonoMethodILState *) is->st;
	memset (is->st, 0, sizeof (is->st));   /* zero method + il_offset + data[] (GC-scanned, kept NULL) */
	il->method = method;
	il->il_offset = -1;
	memset (&is->ext, 0, sizeof (is->ext));
	is->ext.kind = MONO_LMFEXT_IL_STATE;
	is->ext.il_state = il;
	is->finally_sp = wj_finally_exc_sp;
	mono_push_lmf (&is->ext);
	is->prev = mono_wasm_jit_cur_island_il_state;
	mono_wasm_jit_cur_island_il_state = il;
}

void
mono_wasm_jit_leave_island (void)
{
	WjIsland *is;
	if (G_UNLIKELY (wj_island_sp <= 0))
		return;
	is = wj_island_at (--wj_island_sp);
	mono_pop_lmf (&is->ext.lmf);
	mono_wasm_jit_cur_island_il_state = is->prev;
	wj_finally_trim (is->finally_sp);
}

/* boundary safety net: a C++ unwind that escapes a JITted EH method WITHOUT going through its emitted
 * leave_island (shouldn't happen — every escape is the wasm-catch rethrow, which emits leave_island —
 * but defensive) leaks island LMFs; the interp->JIT boundary resets the stack to its entry depth. */
int
mono_wasm_jit_island_sp_save (void)
{
	return wj_island_sp;
}
void
mono_wasm_jit_island_sp_restore (int sp)
{
	while (wj_island_sp > sp) {
		WjIsland *is = wj_island_at (--wj_island_sp);
		mono_pop_lmf (&is->ext.lmf);
		mono_wasm_jit_cur_island_il_state = is->prev;
		wj_finally_trim (is->finally_sp);
	}
}

/* Emitted (call_indirect) at each bb of a JITted EH method: record the current IL offset into the active
 * island's il_state so pass-1's clause walk matches the right try region for a throwing AOT callee. */
void
mono_wasm_jit_set_il_offset (int il_offset)
{
	if (mono_wasm_jit_cur_island_il_state)
		mono_wasm_jit_cur_island_il_state->il_offset = il_offset;
}

/* TRUE if il_state is one of THIS thread's live wasm-JIT EH islands (vs a real AOT il_state emitted by
 * llvmonly codegen, whose data[] points at addressable locals). mono's exception pass-1 uses this to
 * recognise a JITted-EH frame regardless of how the throw started — a native throw helper OR a
 * cooperative interp_throw from a residual interpreted method running inside the JITted method — so it
 * always defers the handler to the method's wasm landing pad (a native unwind) instead of
 * run_clause_with_il_state, which would try to interpret the island's zeroed clause frame. The island
 * stack depth is tiny (JITted-EH nesting), so the linear scan is cheap even on the hot EH path. */
int
mono_wasm_jit_is_island (MonoMethodILState *il_state)
{
	int i, total;
	if (!il_state || !wj_island_chunks)
		return 0;
	/* FIX (finally-codegen wild store): scan ALL allocated island slots, not just [0, wj_island_sp).
	 * An island il_state can still be referenced by a live IL_STATE LMFExt in the LMF chain while sitting
	 * AT/ABOVE the live stack pointer (e.g. the OP_ENDFINALLY re-raise deliberately does NOT pop the
	 * island, and direct JIT->JIT unwinds). The old [0,sp) bound MISSED those, so mono's exception pass-1
	 * (mini-exceptions.c) fell through to interp_run_clause_with_il_state on the island's ZEROED data[] ->
	 * a garbage `this`/params -> a store through the garbage `this` -> wild write to a random in-bounds
	 * heap address (the intermittent "random memory corruption"; here it landed on the jiterpreter tlqueue
	 * GPtrArray). Recognising every island slot by address closes the hole; pass-1 then always defers the
	 * handler to the method's wasm landing pad. (Real AOT il_states live outside wj_island_chunks, so no
	 * false positives.) */
	total = wj_island_nchunks * WJ_ISLAND_CHUNK;
	for (i = 0; i < total; ++i) {
		if ((MonoMethodILState *) wj_island_at (i)->st == il_state) {
			if (G_UNLIKELY (i >= wj_island_sp)) {
				/* Confirmation: this island would have been MISSED by the old [0,sp) check. Bounded log
				 * naming the finally method. If this never fires, the hole was not being hit. */
				static int _hole = 0;
				if (_hole++ < 40) {
					MonoMethod *mm = il_state->method;
					printf ("[island-hole] FIXED: matched popped island slot idx=%d sp=%d method=%s.%s (old [0,sp) check missed it -> interp ran zeroed island -> wild store)\n",
						i, wj_island_sp,
						(mm && mm->klass) ? m_class_get_name (mm->klass) : "?",
						mm ? mm->name : "?");
				}
			}
			return 1;
		}
	}
	return 0;
}

/* DIAG (wasm-jit EH unwind crash): if `lmf` is the address of one of THIS thread's wasm-JIT island LMFExts,
 * return the island's method (read from il_state->method in `st`, at a nonzero struct offset that survives a
 * previous_lmf-only / offset-0 clobber). Lets mono_arch_unwind_frame NAME the method whose island ext got
 * corrupted (bit-2 tag cleared -> read as a plain NULL-method LMF) instead of aborting anonymously, pointing
 * straight at the finally/EH codegen that wild-stored it. Returns NULL if `lmf` is not an island slot. */
MonoMethod *
mono_wasm_jit_island_lmf_method (gpointer lmf)
{
	int i, total;
	if (!wj_island_chunks)
		return NULL;
	total = wj_island_nchunks * WJ_ISLAND_CHUNK;
	for (i = 0; i < total; ++i) {
		WjIsland *is = wj_island_at (i);
		if ((gpointer) &is->ext == lmf) {
			MonoMethodILState *il = (MonoMethodILState *) is->st;
			return il ? il->method : NULL;
		}
	}
	return NULL;
}

/* --- AOT-style EH milestone 2: in-method catch landing-pad dispatch -----------------------------------
 * Set by mono_wasm_jit_eh_dispatch on a catch match; read by the JITted handler's OP_GET_EX_OBJ (the
 * emitter bakes this address + i32.loads it). Per-thread: the in-flight caught exception. */
__thread MonoObject *mono_wasm_jit_caught_exc;
/* DIAG: set to 1 by the dispatch on a catch match; read+cleared by mono_wasm_jit_invoke_caught to
 * correlate a catch-taken invocation with its return value + ref-stack balance (loot-NPE bisection). */
__thread int mono_wasm_jit_eh_caught_flag = 0;

/* Called from a JITted method's `catch <x.e>` landing pad: an exception is unwinding (C++/wasm-EH) and
 * this method's try region(s) caught it. Walk the clauses enclosing the throwing bb (innermost-out); on
 * the first CATCH whose type matches, stash the exception for OP_GET_EX_OBJ, consume the in-flight
 * exception (clear thrown_exc + any resume-state pass-1 picked for an OUTER handler — we handle nearer),
 * and return the handler's dense bbidx. No match -> -1 (the landing pad rethrows to propagate). */
int
mono_wasm_jit_eh_dispatch (WasmEhTable *t, int blk)
{
	ERROR_DECL (error);
	MonoObject *exc = mini_llvmonly_load_exception ();
	int il, i;
	if (!exc)
		return -1;
	il = (blk >= 0 && blk < t->nbbs) ? t->il_offsets [blk] : -1;
	{ extern int mono_wasm_jit_verbose; static int _ehd = 0;
	  if (mono_wasm_jit_verbose >= 3 && _ehd++ < 200) {
		/* DIAG: log the INCOMING resume-state pass-1 installed (if any) BEFORE we clear it. il_state!=0 or
		 * has_resume_state=1 means the throw ran mono_handle_exception pass-1, which WALKED PAST this JITted
		 * frame (it's unwinder-invisible) and found an OUTER handler — running any intervening finally
		 * clauses prematurely. That premature pass-1 processing is the suspected loot-table corruption. */
		ThreadContext *_ctx = get_context (); MonoJitTlsData *_jt = mono_get_jit_tls (); extern gboolean mono_llvm_only;
		printf ("EHDISP %s blk=%d il=%d ncl=%d exc=%s cl0=[%d,%d)f%d | llvmonly=%d INSTATE hrs=%d hf=%p ctxexc=%d il_state=%p rsexc=%d\n", t->name ? t->name : "?", blk, il, t->nclauses, exc ? m_class_get_name (mono_object_class (exc)) : "?", t->nclauses > 0 ? t->clauses[0].try_start : -1, t->nclauses > 0 ? t->clauses[0].try_start + t->clauses[0].try_len : -1, t->nclauses > 0 ? t->clauses[0].flags : -1, mono_llvm_only, _ctx->has_resume_state, (void*)_ctx->handler_frame, _ctx->exc_gchandle != 0, (void*)_jt->resume_state.il_state, _jt->resume_state.ex_gchandle != 0); } }
	if (il < 0)
		return -1;
	/* mirror mono's is_address_protected + clause walk: iterate clauses in table order; the first CATCH
	 * whose try IL-range contains the throwing bb AND whose type matches handles it (nearest-first). */
	for (i = 0; i < t->nclauses; ++i) {
		WasmEhClause *c = &t->clauses [i];
		if (!(il >= c->try_start && il < c->try_start + c->try_len))
			continue;
		if (c->flags == MONO_EXCEPTION_CLAUSE_NONE) {   /* a catch clause */
			if (c->catch_class && mono_object_isinst_checked (exc, (MonoClass *) c->catch_class, error)) {
				ThreadContext *ctx = get_context ();
				MonoJitTlsData *jit_tls = mono_get_jit_tls ();
				mono_wasm_jit_caught_exc = exc;          /* for OP_GET_EX_OBJ in the handler */
				mono_wasm_jit_eh_caught_flag = 1;        /* DIAG: this invocation took the catch path */
				mini_llvmonly_clear_exception ();        /* consumed: drop the in-flight native exception (thrown_exc) */
				/* discard any resume-state pass-1 chose for an OUTER handler — we handle nearer. Cover both
				 * the interp resume-state (interp outer handler) and the il_state resume-state (AOTed outer
				 * handler), so a later mono_handle_exception's `!resume_state.ex_gchandle` assert holds. */
				if (ctx->exc_gchandle) { mono_gchandle_free_internal (ctx->exc_gchandle); ctx->exc_gchandle = 0; }
				ctx->has_resume_state = FALSE;
				ctx->handler_frame = NULL;
				if (jit_tls->resume_state.ex_gchandle) { mono_gchandle_free_internal (jit_tls->resume_state.ex_gchandle); jit_tls->resume_state.ex_gchandle = 0; }
				jit_tls->resume_state.il_state = NULL;
				{ extern int mono_wasm_jit_verbose; static int _ehm = 0; if (mono_wasm_jit_verbose >= 3 && _ehm++ < 200) { printf ("EHDISP   -> MATCH %s cl%d -> bb%d\n", t->name ? t->name : "?", i, c->handler_bbidx); } }
				return c->handler_bbidx;
			}
			mono_error_cleanup (error);
			error_init_reuse (error);
		} else if (c->flags == MONO_EXCEPTION_CLAUSE_FINALLY || c->flags == MONO_EXCEPTION_CLAUSE_FAULT) {
			/* A FINALLY (or FAULT) clause covering the throwing bb. Both run on the exception path identically:
			 * the handler runs in the JITted wasm body (landing pad GOTOs handler_bbidx with finally_ind=-1),
			 * then OP_ENDFINALLY (== endfault) RE-RAISES the exception (mono_wasm_jit_endfinally_rethrow) so it
			 * keeps propagating to an outer catch/finally. (FAULT differs from FINALLY only on the NORMAL path,
			 * where it isn't run — handled in codegen: a fault handler has no OP_CALL_HANDLER.)
			 * We MOVE the in-flight exception onto the finally save stack (taking ownership of the gchandle)
			 * and CLEAR jit_tls->thrown_exc, instead of leaving it in the single shared slot: a nested
			 * throw+catch inside the finally body would otherwise free that shared slot (the catch dispatch
			 * clears thrown_exc), and OP_ENDFINALLY's re-raise would then load NULL and trap the worker. With
			 * the value saved, a nested throw starts from a clean thrown_exc and OP_ENDFINALLY re-raises from
			 * the save stack. We also drop the resume-state pass-1 may have set for an OUTER handler (the
			 * finally runs nearer first; after it re-raises, the advanced il_offset finds the next clause). */
			ThreadContext *ctx = get_context ();
			MonoJitTlsData *jit_tls = mono_get_jit_tls ();
			if (ctx->exc_gchandle) { mono_gchandle_free_internal (ctx->exc_gchandle); ctx->exc_gchandle = 0; }
			ctx->has_resume_state = FALSE;
			ctx->handler_frame = NULL;
			if (jit_tls->resume_state.ex_gchandle) { mono_gchandle_free_internal (jit_tls->resume_state.ex_gchandle); jit_tls->resume_state.ex_gchandle = 0; }
			jit_tls->resume_state.il_state = NULL;
			wj_finally_push (jit_tls->thrown_exc);   /* take ownership of the gchandle for OP_ENDFINALLY's re-raise */
			jit_tls->thrown_exc = 0;                 /* clear so a nested throw inside the finally body can't free it */
			{ extern int mono_wasm_jit_verbose; static int _ehf = 0; if (mono_wasm_jit_verbose >= 3 && _ehf++ < 200) { printf ("EHDISP   -> FINALLY %s cl%d -> bb%d\n", t->name ? t->name : "?", i, c->handler_bbidx); } }
			/* Tag the FINALLY/FAULT dispatch so the landing pad sets finally_ind = -1 ONLY for it (see
			 * WJ_EH_DISPATCH_FINALLY_BIT in mini.h): a CATCH dispatch must NOT clobber a normal-leave
			 * continuation an in-flight OP_CALL_HANDLER stored there. */
			return c->handler_bbidx | WJ_EH_DISPATCH_FINALLY_BIT;
		}
		/* FILTER clauses still bail at the emitter gate; FAULT is handled above (like FINALLY). */
	}
	return -1;   /* no matching handler in this method -> rethrow to propagate */
}

/* OP_ENDFINALLY rethrow path (finally_ind == -1, i.e. the finally ran on the EXCEPTION path, not a normal
 * leave): re-raise the exception this finally was running for so it propagates past this finally to the
 * next enclosing catch/finally (or out of the method). The exception was MOVED onto the finally save stack
 * by the FINALLY dispatch (not left in the shared jit_tls->thrown_exc, which a nested throw+catch in the
 * finally body may have cleared); we pop it here. Re-throwing runs pass-1 again from the finally-handler
 * il_offset (now OUTSIDE this finally's try), so the re-dispatch advances to the outer clause. This MUST
 * NEVER RETURN — the emitter places an unconditional WASM_OP_UNREACHABLE right after the call, so a return
 * would trap the worker instead of unwinding a (catchable) managed exception. */
void
mono_wasm_jit_endfinally_rethrow (void)
{
	extern void mono_wasm_jit_rethrow (MonoObject *exc);
	MonoJitTlsData *jit_tls = mono_get_jit_tls ();
	MonoGCHandle h = wj_finally_pop ();
	MonoObject *exc = h ? mono_gchandle_get_target_internal (h) : NULL;
	if (exc) {
		/* Hand the saved handle back to thrown_exc so the object stays GC-rooted across the re-raise (whose
		 * pass-1 may allocate); the eventual catch / invoke_caught frees it. mono_wasm_jit_rethrow's pass-1
		 * sees thrown_exc already set and reuses it (no double-root). */
		if (jit_tls->thrown_exc && jit_tls->thrown_exc != h)
			mono_gchandle_free_internal (jit_tls->thrown_exc);
		jit_tls->thrown_exc = h;
		mono_wasm_jit_rethrow (exc);   /* CONTINUATION: preserve the original stack trace */
		g_assert_not_reached ();
	}
	if (h)
		mono_gchandle_free_internal (h);
	/* Defensive: the saved exception was lost (an exotic finally-threw-and-escaped mis-attribution, or the
	 * save stack overflowed). Re-raise whatever is still in flight, else synthesize a catchable exception, so
	 * we ALWAYS C++-unwind and never fall into the emitted unreachable -> raw trap -> worker death. */
	if (jit_tls->thrown_exc) {
		exc = mini_llvmonly_load_exception ();
		mini_llvmonly_clear_exception ();
	}
	if (!exc)
		exc = (MonoObject *) mono_get_exception_execution_engine ("wasm-jit: finally re-raise lost the in-flight exception");
	mono_wasm_jit_rethrow (exc);
	g_assert_not_reached ();
}

/* OP_GET_EX_OBJ in a JITted catch handler: return (+ consume) the exception the landing pad's dispatch
 * stashed. mono_wasm_jit_caught_exc is __thread (per-thread address), so the emitter can't bake a fixed
 * address — it call_indirects this getter instead. No GC point between the dispatch stash and this read,
 * and the handler immediately stores the result into the GC-scanned ref shadow stack. */
MonoObject *
mono_wasm_jit_get_caught_exc (void)
{
	MonoObject *e = mono_wasm_jit_caught_exc;
	mono_wasm_jit_caught_exc = NULL;
	return e;
}

/* MONO_WASM_JIT_BBTRACE=<substr>: the emitter inserts a call to this at the start of every bb of a
 * matching method, so we can SEE the exact runtime $blk path (and spot a divergence / wrong re-dispatch /
 * loop). blk>=0 = a bb entry; blk==-1 = the method is returning. Bounded + gated by STATS. Returns blk so
 * the emitter can `drop` it (reusing the (i32,i32)->i32 dispatch functype). */
int
mono_wasm_jit_bbtrace_log (WasmEhTable *t, int blk)
{
	static int _n = 0;
	extern int mono_wasm_jit_verbose;
	if (mono_wasm_jit_verbose >= 2 && _n++ < 600) {
		if (blk == -1)
			printf ("BBTRACE %s RETURN\n", t && t->name ? t->name : "?");
		else
			printf ("BBTRACE %s bb=%d\n", t && t->name ? t->name : "?", blk);
	}
	return blk;
}
#endif

static void
do_icall (MonoMethodSignature *sig, MintICallSig op, stackval *ret_sp, stackval *sp, gpointer ptr, gboolean save_last_error)
{
	if (save_last_error)
		mono_marshal_clear_last_error ();

	switch (op) {
	case MINT_ICALLSIG_V_V: {
		typedef void (*T)(void);
		T func = (T)ptr;
        	func ();
		break;
	}
	case MINT_ICALLSIG_V_P: {
		typedef gpointer (*T)(void);
		T func = (T)ptr;
		ret_sp->data.p = func ();
		break;
	}
	case MINT_ICALLSIG_P_V: {
		typedef void (*T)(gpointer);
		T func = (T)ptr;
		func (sp [0].data.p);
		break;
	}
	case MINT_ICALLSIG_P_P: {
		typedef gpointer (*T)(gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p);
		break;
	}
	case MINT_ICALLSIG_PP_V: {
		typedef void (*T)(gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p);
		break;
	}
	case MINT_ICALLSIG_PP_P: {
		typedef gpointer (*T)(gpointer,gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p, sp [1].data.p);
		break;
	}
	case MINT_ICALLSIG_PPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p);
		break;
	}
	case MINT_ICALLSIG_PPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p, sp [5].data.p);
		break;
	}
	case MINT_ICALLSIG_PPPPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		ret_sp->data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p, sp [5].data.p);
		break;
	}
	default:
		g_assert_not_reached ();
	}

	if (save_last_error)
		mono_marshal_set_last_error ();

	/* convert the native representation to the stackval representation */
	if (sig)
		stackval_from_data (sig->ret, ret_sp, (char*) &ret_sp->data.p, sig->pinvoke && !sig->marshalling_disabled);
}

/* MONO_NO_OPTIMIZATION is needed due to usage of INTERP_PUSH_LMF_WITH_CTX. */
#ifdef _MSC_VER
#pragma optimize ("", off)
#endif
// Do not inline in case order of frame addresses matters, and maybe other reasons.
static MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
do_icall_wrapper (InterpFrame *frame, MonoMethodSignature *sig, MintICallSig op, stackval *ret_sp, stackval *sp, gpointer ptr, gboolean save_last_error, gboolean *gc_transitions)
{
	MonoLMFExt ext;
	gpointer gc_safe_cookie = NULL;

	INTERP_PUSH_LMF_WITH_CTX (frame, ext, exit_icall);

	if (*gc_transitions) {
		MONO_STACKDATA (stack_data);
		gc_safe_cookie = mono_threads_enter_gc_safe_region_internal (&stack_data);
		do_icall (sig, op, ret_sp, sp, ptr, save_last_error);
		mono_threads_exit_gc_safe_region_internal (gc_safe_cookie, &stack_data);
		*gc_transitions = FALSE;
	} else {
		do_icall (sig, op, ret_sp, sp, ptr, save_last_error);
	}

	interp_pop_lmf (&ext);

	goto exit_icall; // prevent unused label warning in some configurations

/* If an exception is thrown from native code, execution will continue here */
exit_icall:
	if (*gc_transitions) {
		mono_threads_abort_gc_safe_region_internal (gc_safe_cookie);
		*gc_transitions = FALSE;
	}

	return NULL;
}
#ifdef _MSC_VER
#pragma optimize ("", on)
#endif

typedef struct {
	int pindex;
	gpointer jit_wrapper;
	gpointer *args;
	gpointer extra_arg;
	MonoFtnDesc ftndesc;
} JitCallCbData;

/* Callback called by mono_llvm_catch_exception () */
static void
jit_call_cb (gpointer arg)
{
	JitCallCbData *cb_data = (JitCallCbData*)arg;
	gpointer jit_wrapper = cb_data->jit_wrapper;
	int pindex = cb_data->pindex;
	gpointer *args = cb_data->args;
	gpointer ftndesc = cb_data->extra_arg;

	switch (pindex) {
	case 0: {
		typedef void (*T)(gpointer);
		T func = (T)jit_wrapper;

		func (ftndesc);
		break;
	}
	case 1: {
		typedef void (*T)(gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], ftndesc);
		break;
	}
	case 2: {
		typedef void (*T)(gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], ftndesc);
		break;
	}
	case 3: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], ftndesc);
		break;
	}
	case 4: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], ftndesc);
		break;
	}
	case 5: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], ftndesc);
		break;
	}
	case 6: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], ftndesc);
		break;
	}
	case 7: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], ftndesc);
		break;
	}
	case 8: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], args [7], ftndesc);
		break;
	}
	case 9: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], args [7], args [8], ftndesc);
		break;
	}
	case 10: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], args [7], args [8], args [9], ftndesc);
		break;
	}
	case 11: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], args [7], args [8], args [9], args [10], ftndesc);
		break;
	}
	case 12: {
		typedef void (*T)(gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T)jit_wrapper;

		func (args [0], args [1], args [2], args [3], args [4], args [5], args [6], args [7], args [8], args [9], args [10], args [11], ftndesc);
		break;
	}
	default:
		g_assert_not_reached ();
		break;
	}
}

enum {
	/* Pass stackval->data.p */
	JIT_ARG_BYVAL,
	/* Pass &stackval->data.p */
	JIT_ARG_BYREF
};

enum {
       JIT_RET_VOID,
       JIT_RET_SCALAR,
       JIT_RET_VTYPE
};

typedef struct _JitCallInfo JitCallInfo;
struct _JitCallInfo {
	gpointer addr;
	gpointer extra_arg;
	gpointer wrapper;
	MonoMethodSignature *sig;
	guint8 *arginfo;
	gint32 res_size;
	int ret_mt;
	gboolean no_wrapper;
#if HOST_BROWSER
	int hit_count;
	WasmJitCallThunk jiterp_thunk;
#endif
};

static MONO_NEVER_INLINE void
init_jit_call_info (InterpMethod *rmethod, MonoError *error)
{
	MonoMethodSignature *sig;
	JitCallInfo *cinfo;

	//printf ("jit_call: %s\n", mono_method_full_name (rmethod->method, 1));

	MonoMethod *method = rmethod->method;

	// FIXME: Memory management
	cinfo = g_new0 (JitCallInfo, 1);

	sig = mono_method_signature_internal (method);
	g_assert (sig);

	gpointer addr = mono_jit_compile_method_jit_only (method, error);
	return_if_nok (error);
	g_assert (addr);

	gboolean need_wrapper = TRUE;
	if (mono_llvm_only) {
		MonoAotMethodFlags flags = mono_aot_get_method_flags (addr);

		if (flags & MONO_AOT_METHOD_FLAG_GSHAREDVT_VARIABLE) {
			/*
			 * The callee already has a gsharedvt signature, we can call it directly
			 * instead of through a gsharedvt out wrapper.
			 */
			need_wrapper = FALSE;
			cinfo->no_wrapper = TRUE;
		}
	}

	gpointer jit_wrapper = NULL;
	if (need_wrapper) {
		MonoMethod *wrapper = mini_get_gsharedvt_out_sig_wrapper (sig);
		jit_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
		mono_error_assert_ok (error);
	}

	if (mono_llvm_only) {
		gboolean caller_gsharedvt = !need_wrapper;
		cinfo->addr = mini_llvmonly_add_method_wrappers (method, addr, caller_gsharedvt, FALSE, &cinfo->extra_arg);
	} else {
		cinfo->addr = addr;
	}

	cinfo->sig = sig;
	cinfo->wrapper = jit_wrapper;

	if (sig->ret->type != MONO_TYPE_VOID) {
		int mt = mono_mint_type (sig->ret);
		if (mt == MINT_TYPE_VT) {
			MonoClass *klass = mono_class_from_mono_type_internal (sig->ret);
			/*
			 * We cache this size here, instead of the instruction stream of the
			 * calling instruction, to save space for common callvirt instructions
			 * that could end up doing a jit call.
			 */
			gint32 size = mono_class_value_size (klass, NULL);
			cinfo->res_size = ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		} else {
			cinfo->res_size = MINT_STACK_SLOT_SIZE;
		}
		cinfo->ret_mt = mt;
	} else {
		cinfo->ret_mt = -1;
	}

	if (sig->param_count) {
		cinfo->arginfo = g_new0 (guint8, sig->param_count);

		for (guint i = 0; i < rmethod->param_count; ++i) {
			MonoType *t = rmethod->param_types [i];
			int mt = mono_mint_type (t);
			if (m_type_is_byref (sig->params [i])) {
				cinfo->arginfo [i] = JIT_ARG_BYVAL;
			} else if (mt == MINT_TYPE_O) {
				cinfo->arginfo [i] = JIT_ARG_BYREF;
			} else {
				/* stackval->data is an union */
				cinfo->arginfo [i] = JIT_ARG_BYREF;
			}
		}
	}

	mono_memory_barrier ();
	rmethod->jit_call_info = cinfo;
}

#if HOST_BROWSER
EMSCRIPTEN_KEEPALIVE void
mono_jiterp_register_jit_call_thunk (void *cinfo, WasmJitCallThunk thunk) {
	// Release-store the thunk pointer so threads that observe non-NULL also see the
	//  fully-published compiled WASM function. Paired with the acquire-load in do_jit_call.
	mono_atomic_store_ptr ((volatile gpointer *)&((JitCallInfo*)cinfo)->jiterp_thunk, (gpointer)thunk);
}
#endif

static MONO_NEVER_INLINE void
do_jit_call (ThreadContext *context, stackval *ret_sp, stackval *sp, InterpFrame *frame, InterpMethod *rmethod, gboolean wj_residual G_GNUC_UNUSED, MonoError *error)
{
	MonoLMFExt ext;
	JitCallInfo *cinfo;
	gboolean thrown = FALSE;

	//printf ("jit_call: %s\n", mono_method_full_name (rmethod->method, 1));

	/*
	 * Call JITted code through a gsharedvt_out wrapper. These wrappers receive every argument
	 * by ref and return a return value using an explicit return value argument.
	 */
	if (G_UNLIKELY (!rmethod->jit_call_info)) {
		init_jit_call_info (rmethod, error);
		mono_error_assert_ok (error);
	}

	cinfo = (JitCallInfo*)rmethod->jit_call_info;

#if JITERPRETER_ENABLE_JIT_CALL_TRAMPOLINES
	/* Skip the per-call LMF for a wasm-JITted method's residual AOT call (gated on the explicit wj_residual
	 * PARAMETER — passed TRUE only by interp_entry's residual fast path and wasm_jit_aot_call_lean — so the
	 * interp's own MINT_JIT_CALL always keeps its LMF, even one executed inside a residual callee; the old
	 * __thread wj_residual_active gate leaked across the callee's whole dynamic extent and dropped the LMF
	 * for unrelated jit calls). The LMF push/pop here only marks the interp->managed transition for the
	 * stack walker; for the wasm-JIT residual it is unneeded — GC roots come from the conservative C-stack
	 * scan + the JIT ref shadow stack (not the LMF), EH pass-1 visibility comes from the island IL_STATE
	 * LMFExt pushed by the JITted method's prologue, and cppeh unwinds natively. (A managed stack trace
	 * taken INSIDE the residual may truncate at this transition — diagnostics quality, not correctness.) */
	// The jiterpreter will compile a unique thunk for each do_jit_call call site if it is hot
	//  enough to justify it. At that point we can invoke the thunk to efficiently do most of
	//  the work that would normally be done by do_jit_call
	if (mono_opt_jiterpreter_jit_call_enabled) {
		// Acquire-load paired with the release-store in mono_jiterp_register_jit_call_thunk so
		//  that observing a non-NULL pointer guarantees the compiled WASM function is visible.
		WasmJitCallThunk thunk = (WasmJitCallThunk)mono_atomic_load_ptr ((volatile gpointer *)&cinfo->jiterp_thunk);
		if (thunk) {
			MonoFtnDesc ftndesc = {0};
			ftndesc.addr = cinfo->addr;
			ftndesc.arg = cinfo->extra_arg;
			if (!wj_residual) interp_push_lmf (&ext, frame);
			if (
				mono_opt_jiterpreter_wasm_eh_enabled ||
				(mono_aot_mode != MONO_AOT_MODE_LLVMONLY_INTERP)
			) {
				// WASM EH is available or we are otherwise in a situation where we know
				//  that the jiterpreter thunk was compiled with exception handling built-in
				//  so we can just invoke it directly and errors will be handled
				thunk (ret_sp, sp, &ftndesc, &thrown);
			} else {
				// Call a special JS function that will invoke the compiled jiterpreter thunk
				//  and trap errors for us to set the thrown flag
				mono_interp_invoke_wasm_jit_call_trampoline (
					thunk, ret_sp, sp, &ftndesc, &thrown
				);
			}
			if (!wj_residual) interp_pop_lmf (&ext);

			// We reuse do_jit_call's epilogue to do things like propagate thrown exceptions
			//  and sign-extend return values instead of inlining that logic into every thunk
			// the dummy implementation sets a special value into thrown to indicate that
			//  we need to go through the slow path because this thread has no thunk yet
			if (G_UNLIKELY (thrown == 999))
				thrown = 0;
			else
				goto epilogue;
		} else {
			int old_count = mono_jiterp_increment_counter (&cinfo->hit_count);
			// If our hit count just reached the threshold, we request that a thunk be jitted
			//  for this specific call site. It will go into a queue and wait until there
			//  are enough jit calls waiting to be compiled into one WASM module
			if (old_count == mono_opt_jiterpreter_jit_call_trampoline_hit_count) {
				mono_interp_jit_wasm_jit_call_trampoline (
					rmethod->method, rmethod, cinfo,
					initialize_arg_offsets(rmethod, mono_method_signature_internal (rmethod->method)),
					mono_aot_mode == MONO_AOT_MODE_LLVMONLY_INTERP
				);
			} else {
				int excess = old_count - mono_opt_jiterpreter_jit_call_queue_flush_threshold;
				// If our hit count just reached the flush threshold, that means that we
				//  previously requested compilation for this call site and it didn't
				//  happen yet. We will request a flush of the entire queue this one
				//  time which will probably result in it being compiled
				if (excess == 0)
					mono_interp_flush_jitcall_queue ();
			}
		}
	}
#endif

	/*
	 * Convert the arguments on the interpreter stack to the format expected by the gsharedvt_out wrapper.
	 */
	gpointer args [32];
	int pindex = 0;
	int stack_index = 0;
	if (rmethod->hasthis) {
		args [pindex ++] = sp [0].data.p;
		stack_index ++;
	}
	/* return address */
	if (cinfo->ret_mt != -1)
		args [pindex ++] = ret_sp;
	for (guint i = 0; i < rmethod->param_count; ++i) {
		stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, get_arg_offset_fast (rmethod, NULL, stack_index + i));
		if (cinfo->arginfo [i] == JIT_ARG_BYVAL)
			args [pindex ++] = sval->data.p;
		else
			/* data is an union, so can use 'p' for all types */
			args [pindex ++] = sval;
	}

	JitCallCbData cb_data;
	cb_data.pindex = pindex;
	cb_data.args = args;
	cb_data.ftndesc.interp_method = NULL;
	cb_data.ftndesc.method = NULL;
	if (cinfo->no_wrapper) {
		cb_data.ftndesc.addr = NULL;
		cb_data.ftndesc.arg = NULL;
		cb_data.jit_wrapper = cinfo->addr;
		cb_data.extra_arg = cinfo->extra_arg;
	} else {
		cb_data.ftndesc.addr = cinfo->addr;
		cb_data.ftndesc.arg = cinfo->extra_arg;
		cb_data.jit_wrapper = cinfo->wrapper;
		cb_data.extra_arg = &cb_data.ftndesc;
	}

	/* The gsharedvt_out slow path KEEPS the per-call LMF even for the wasm-JIT residual: this is the path the
	 * residual actually takes into an AOT'd callee, and mono's exception pass-1 (mono_handle_exception) walks
	 * the LMF chain to find a handler across that interp->AOT transition. Skipping it here makes pass-1 miss a
	 * catch up the stack (observed: GSON ConstructorConstructor's `catch (NoSuchMethodException)` escaping).
	 * Only the jiterpreter-thunk fast path above skips the LMF (validated). */
	interp_push_lmf (&ext, frame);

	if (mono_aot_mode == MONO_AOT_MODE_LLVMONLY_INTERP) {
		/* Catch the exception thrown by the native code using a try-catch */
		mono_llvm_catch_exception (jit_call_cb, &cb_data, &thrown);
	} else {
		jit_call_cb (&cb_data);
	}

	interp_pop_lmf (&ext);

#if JITERPRETER_ENABLE_JIT_CALL_TRAMPOLINES
epilogue:
#endif
	if (thrown) {
		if (context->has_resume_state)
			/*
			 * This happens when interp_entry calls mono_llvm_reraise_exception ().
			 */
			return;
		MonoJitTlsData *jit_tls = mono_get_jit_tls ();
		if (jit_tls->resume_state.il_state) {
			/*
			 * This c++ exception is going to be caught by an AOTed frame above us.
			 * We can't rethrow here, since that will skip the cleanup of the
			 * interpreter stack space etc. So instruct the interpreter to unwind.
			 */
			context->has_resume_state = TRUE;
			context->handler_frame = NULL;
			return;
		}
		MonoObject *obj = mini_llvmonly_load_exception ();
		g_assert (obj);
		mini_llvmonly_clear_exception ();
		mono_error_set_exception_instance (error, (MonoException*)obj);
		return;
	}
	if (cinfo->ret_mt != -1) {
		//  Sign/zero extend if necessary
		switch (cinfo->ret_mt) {
		case MINT_TYPE_I1:
			ret_sp->data.i = *(gint8*)ret_sp;
			break;
		case MINT_TYPE_U1:
			ret_sp->data.i = *(guint8*)ret_sp;
			break;
		case MINT_TYPE_I2:
			ret_sp->data.i = *(gint16*)ret_sp;
			break;
		case MINT_TYPE_U2:
			ret_sp->data.i = *(guint16*)ret_sp;
			break;
		case MINT_TYPE_I4:
		case MINT_TYPE_I8:
		case MINT_TYPE_R4:
		case MINT_TYPE_R8:
		case MINT_TYPE_VT:
		case MINT_TYPE_O:
			/* The result was written to ret_sp */
			break;
		default:
			g_assert_not_reached ();
		}
	}
}

#if HOST_BROWSER
/* TRUE iff the method's raw wasm AOT body carries the trailing "extra" pointer arg (rgctx-or-dummy) that
 * makes it uniformly callable through a funcptr. Direct port of needs_extra_arg() in mini-llvm.c (minus the
 * EmitContext): on wasm, llvm_only AOT gives EVERY method that trailing arg so the caller/callee signatures
 * match for indirect calls — EXCEPT a set of wrapper kinds that are never called indirectly. Those return
 * FALSE: their raw body is (this,args)->ret with NO trailing slot, so the AOT-direct emitter must NOT append
 * the rgctx param/value for them (else the call_indirect declares one param too many -> signature-mismatch
 * trap). Assumes the whole AOT image was built --aot=llvmonly (so the llvm_only/emit_dummy_arg guard in the
 * original is always satisfied here); the MONO_WASM_JIT_VERBOSE>=3 line in the caller surfaces any surprise. */
static gboolean
aot_method_has_extra_arg (MonoMethod *method)
{
	WrapperInfo *info = method->wrapper_type ? mono_marshal_get_wrapper_info (method) : NULL;
	switch (method->wrapper_type) {
	case MONO_WRAPPER_OTHER:
		if (info && (info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG || info->subtype == WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG))
			return FALSE;   /* already have an explicit extra arg */
		break;
	case MONO_WRAPPER_MANAGED_TO_NATIVE:
		if (strstr (method->name, "icall_wrapper"))
			return FALSE;   /* JIT icall wrappers, only called directly */
		break;              /* normal icalls can be virtual -> need the extra arg */
	case MONO_WRAPPER_RUNTIME_INVOKE:
	case MONO_WRAPPER_ALLOC:
	case MONO_WRAPPER_CASTCLASS:
	case MONO_WRAPPER_WRITE_BARRIER:
	case MONO_WRAPPER_NATIVE_TO_MANAGED:
		return FALSE;
	case MONO_WRAPPER_STELEMREF:
		if (info && info->subtype != WRAPPER_SUBTYPE_VIRTUAL_STELEMREF)
			return FALSE;
		break;
	case MONO_WRAPPER_MANAGED_TO_MANAGED:
		if (info && info->subtype == WRAPPER_SUBTYPE_STRING_CTOR)
			return FALSE;
		break;
	default:
		break;
	}
	if (method->string_ctor)
		return FALSE;
	/* called from gsharedvt code via an indirect call that doesn't pass an extra arg */
	if (method->klass == mono_defaults.string_class && (strstr (method->name, "memcpy") || strstr (method->name, "bzero")))
		return FALSE;
	return TRUE;
}

/* Mirror mini_llvmonly_add_method_wrappers() when recovering a static rgctx outside the usual llvm_only
 * wrapper path: these wrapper kinds borrow the wrapped method's generic context rather than their own. */
static MonoMethod *
aot_call_target_rgctx_method (MonoMethod *method)
{
	WrapperInfo *info;

	if (!method || !method->wrapper_type)
		return method;

	info = mono_marshal_get_wrapper_info (method);
	if (!info)
		return method;

	if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_MANAGED && info->subtype == WRAPPER_SUBTYPE_GENERIC_ARRAY_HELPER)
		return info->d.generic_array_helper.method ? info->d.generic_array_helper.method : method;
	if (method->wrapper_type == MONO_WRAPPER_OTHER && info->subtype == WRAPPER_SUBTYPE_SYNCHRONIZED_INNER)
		return info->d.synchronized_inner.method ? info->d.synchronized_inner.method : method;
	return method;
}

/* For the inline-direct-AOT-call emitter (mini-wasm.c): ensure the callee's JitCallInfo, then hand back
 * the two values the emitter bakes as i32 constants — cinfo->addr (the AOT target's table index) and
 * cinfo->extra_arg (the rgctx). The emitter then emits, inline in the JITted method, the same direct
 * same-ABI call the jiterpreter's directJitCalls (enableDirect) thunk does: push (this, native-typed
 * args, rgctx); call_indirect <addr>. cinfo->addr is cross-thread-stable (do_jit_call's generic path
 * calls it from any thread), so baking it is safe — it's the GOT-slot analog. Returns TRUE only for the
 * directly-callable case: jit-call-supported AND !no_wrapper (the gsharedvt_out wrapper exists but is
 * bypassable). no_wrapper (gsharedvt-variable) and unsupported callees return FALSE -> emitter residuals. */
gboolean
mono_wasm_jit_aot_call_target (MonoMethod *method, gpointer *out_addr, gpointer *out_rgctx, gboolean *out_has_extra_arg)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	MonoMethod *rgctx_method;
	if (!mono_interp_jit_call_supported (method, sig))
		return FALSE;
	InterpMethod *imethod = mono_interp_get_imethod (method);
	if (!imethod->jit_call_info) {
		ERROR_DECL (error);
		init_jit_call_info (imethod, error);
		if (!is_ok (error)) { mono_interp_error_cleanup (error); return FALSE; }
	}
	JitCallInfo *cinfo = (JitCallInfo*)imethod->jit_call_info;
	if (!cinfo || cinfo->no_wrapper)
		return FALSE;
	/* The fast paths (inline_aot, vcall_aot) call cinfo->addr DIRECTLY with the baked (this, args [, rgctx])
	 * native ABI. That is only valid if cinfo->addr is a real directly-callable AOT body. For an
	 * INTERP_ENTRY_ONLY method, AOT emitted no native body — cinfo->addr is an interp-entry trampoline with a
	 * uniform (args_ptr, ret_ptr) shape, so call_indirect-ing it with the baked functype is a "function
	 * signature mismatch" trap (observed on the method_20801 vcall path: a plain inflated=0/wrapper=0 target
	 * whose body has no trailing arg). aot_method_has_extra_arg can't see this (it inspects only the managed
	 * method), so detect it here and bail to the residual, which enters such methods correctly via do_jit_call
	 * / the gsharedvt-out wrapper. (GSHAREDVT_VARIABLE is already handled below; this is its sibling case.) */
	if (mono_aot_get_method_flags ((guint8 *) cinfo->addr) & MONO_AOT_METHOD_FLAG_INTERP_ENTRY_ONLY)
		return FALSE;
	*out_addr = cinfo->addr;
	*out_rgctx = cinfo->extra_arg;
	rgctx_method = aot_call_target_rgctx_method (method);
	/* Does the raw AOT body have the trailing extra (rgctx/dummy) arg? The emitter appends the rgctx
	 * param/value only when TRUE; for the exempt wrapper kinds (FALSE) the body is (this,args)->ret with
	 * no trailing slot, so appending one is a signature-mismatch trap. See aot_method_has_extra_arg. */
	if (out_has_extra_arg) {
		*out_has_extra_arg = aot_method_has_extra_arg (method);
		if (!*out_has_extra_arg) {
			extern int mono_wasm_jit_verbose;
			if (mono_wasm_jit_verbose >= 3) {
				char *fn = mono_method_get_full_name (method);
				printf ("WASM_JIT_NOEXTRA %s wrapper_type=%d (AOT-direct call without trailing arg)\n", fn, method->wrapper_type);
				g_free (fn);
			}
		}
	}
	/* Wasm normally runs llvm_only, so init_jit_call_info already populated cinfo->extra_arg through
	 * mini_llvmonly_add_method_wrappers() and the fast paths above are using the exact same rgctx source as the
	 * regular llvmonly direct-call path. Keep the non-llvm_only fallback correct too: if a configuration leaves
	 * extra_arg unset, recover the static rgctx the same way llvmonly would. A gsharedvt-VARIABLE callee still
	 * needs the gsharedvt-in wrapper (its raw body has a variable signature, not callable with the fixed
	 * this+args+rgctx ABI) -> bail so the caller routes it through the residual (do_jit_call applies that
	 * wrapper); otherwise, when the body needs a static rgctx invoke, pass mini_method_get_rgctx() for the same
	 * canonical target method that llvmonly uses. */
	if (!mono_llvm_only && !*out_rgctx) {
		if (mono_aot_get_method_flags ((guint8*)cinfo->addr) & MONO_AOT_METHOD_FLAG_GSHAREDVT_VARIABLE)
			return FALSE;
		if (mono_method_needs_static_rgctx_invoke (rgctx_method, FALSE))
			*out_rgctx = mini_method_get_rgctx (rgctx_method);
	}
	return TRUE;
}

/* Called from JITted code immediately before a direct JIT->JIT f-slot call_indirect, to GUARANTEE the
 * callee's module is instantiated in THIS thread's (per-thread) wasm function table. The wasm table is
 * per-thread for dynamic entries; sync_thread is supposed to populate the whole prefix before a method
 * runs, but a self-compiled/island-run caller (or a sync watermark gap) can leave an earlier-registered
 * callee as the jiterpreter placeholder on this worker (mono_jiterp_placeholder_jit_call,
 * (i32,i32,i32,i32)->void) — call_indirect-ing it is a signature-mismatch trap that silently kills the
 * worker. Unlike the invoke/vcall paths (which check slot_live and fall back to the interp), the direct
 * call had no guard. This instantiates the SPECIFIC callee module via its imethod, regardless of the
 * sync watermark (so it is robust even when sync_thread's fast path would skip it), then returns so the
 * caller can proceed with the now-safe direct call_indirect. No-op (one branch) once the slot is live. */
void
mono_wasm_jit_ensure_fslot (MonoMethod *callee, int fslot)
{
	extern int mono_wasm_jit_slot_live (int slot);
	extern int mono_wasm_jit_instantiate_fslot (int fslot);
	extern int mono_wasm_jit_instantiate_local (int e_slot, int f_slot, const void *bytes, int len, char *errbuf, int errcap, double *out_ms);
	extern int mono_wasm_jit_verbose;
	if (G_LIKELY (mono_wasm_jit_slot_live (fslot)))
		return;
	/* (1) AUTHORITATIVE: instantiate this specific slot from the module registry (wj_reg). Immune to the racy
	 * per-imethod fields and to the sync-break ordering — this is the robust common path. */
	if (mono_wasm_jit_instantiate_fslot (fslot)) {
		static int _ok = 0;
		if (mono_wasm_jit_verbose >= 1 && _ok++ < 80) {
			char *fn = mono_method_get_full_name (callee);
			printf ("WASM_JIT_ENSURE fslot=%d %s (registry-instantiated before direct call)\n", fslot, fn);
			g_free (fn);
		}
		return;
	}
	/* (2) FALLBACK for registry OVERFLOW (WJ_REG_MAX): past the cap, mono_wasm_jit_register drops the entry
	 * but compile_publish still sets im->wasm_jit_{slot,fslot,bytes,len} — so the method is JITted yet absent
	 * from wj_reg, and the registry path above misses it. Instantiate from the imethod fields (the only
	 * source for an overflow method; stable since it was published long before this caller baked fslot). */
	{
		InterpMethod *im = mono_interp_get_imethod (callee);
		if (im && im->wasm_jit_slot > 0) {
			mono_memory_barrier ();   /* ACQUIRE vs compile_publish's release of slot LAST -> bytes/len coherent */
			void *bytes = im->wasm_jit_bytes;
			int len = im->wasm_jit_bytes_len;
			int fs = im->wasm_jit_fslot;
			if (bytes && len > 0 && len < (16 * 1024 * 1024) && fs > 0) {
				char eb [192]; eb [0] = 0; double ms = 0;
				if (mono_wasm_jit_instantiate_local (im->wasm_jit_slot, fs, bytes, len, eb, (int) sizeof (eb), &ms)
						&& mono_wasm_jit_slot_live (fslot)) {
					static int _of = 0;
					if (mono_wasm_jit_verbose >= 1 && _of++ < 80) {
						char *fn = mono_method_get_full_name (callee);
						printf ("WASM_JIT_ENSURE fslot=%d %s (imethod-fallback; not in registry, overflow?)\n", fslot, fn);
						g_free (fn);
					}
					return;
				}
			}
		}
	}
	/* (3) genuinely could not make the slot live (OOM/CompileError on this worker, or torn bytes): the direct
	 * call_indirect that follows WILL trap on the jiterpreter placeholder. Name the method so it is
	 * diagnosable instead of a bare "function signature mismatch". (Accepted: no per-call interp fallback.) */
	{ static int _f = 0;
	  if (_f++ < 80) {
		char *fn = mono_method_get_full_name (callee);
		printf ("WASM_JIT_ENSURE_UNRESOLVED fslot=%d %s : slot not live; direct call will trap\n", fslot, fn);
		g_free (fn);
	  } }
}

#endif

static MONO_NEVER_INLINE void
do_debugger_tramp (void (*tramp) (void), InterpFrame *frame)
{
	MonoLMFExt ext;
	interp_push_lmf (&ext, frame);
	tramp ();
	interp_pop_lmf (&ext);
}

static MONO_NEVER_INLINE MonoException*
do_transform_method (InterpMethod *imethod, InterpFrame *frame, ThreadContext *context)
{
	MonoLMFExt ext;
	/* Don't push lmf if we have no interp data */
	gboolean push_lmf = frame->parent != NULL;
	MonoException *ex = NULL;
	ERROR_DECL (error);

	/* Use the parent frame as the current frame is not complete yet */
	if (push_lmf)
		interp_push_lmf (&ext, frame->parent);

#if DEBUG_INTERP
	if (imethod->method) {
		char* mn = mono_method_full_name (imethod->method, TRUE);
		g_print ("(%p) Transforming %s\n", mono_thread_internal_current (), mn);
		g_free (mn);
	}
#endif

	mono_interp_transform_method (imethod, context, error);
	if (!is_ok (error))
		ex = mono_error_convert_to_exception (error);

	if (push_lmf)
		interp_pop_lmf (&ext);

	return ex;
}

static void
init_arglist (InterpFrame *frame, MonoMethodSignature *sig, stackval *sp, char *arglist)
{
	*(gpointer*)arglist = sig;
	arglist += sizeof (gpointer);

	for (int i = sig->sentinelpos; i < sig->param_count; i++) {
		int align, arg_size, sv_size;
		arg_size = mono_type_stack_size (sig->params [i], &align);
		arglist = (char*)ALIGN_PTR_TO (arglist, align);

		sv_size = stackval_to_data (sig->params [i], sp, arglist, FALSE);
		arglist += arg_size;
		sp = STACK_ADD_BYTES (sp, sv_size);
	}
}

/*
 * These functions are the entry points into the interpreter from compiled code.
 * They are called by the interp_in wrappers. They have the following signature:
 * void (<optional this_arg>, <optional retval pointer>, <arg1>, ..., <argn>, <method ptr>)
 * They pack up their arguments into an InterpEntryData structure and call interp_entry ().
 * It would be possible for the wrappers to pack up the arguments etc, but that would make them bigger, and there are
 * more wrappers then these functions.
 * this/static * ret/void * 16 arguments -> 64 functions.
 */

#if HOST_BROWSER
/*
 * For the jiterpreter, we want to record a hit count for interp_entry wrappers that can
 *  be jitted, but not for ones that can't. As a result we need to put this in its own
 *  macro instead of in INTERP_ENTRY_BASE, so that the generic wrappers don't have to
 *  call it on every invocation.
 * Once this gets called a few hundred times, the wrapper will be jitted so we'll stop
 *  paying the cost of the hit counter and the entry will become faster.
 */
#define INTERP_ENTRY_UPDATE_HIT_COUNT(_method) \
	if (mono_opt_jiterpreter_interp_entry_enabled) \
		mono_interp_record_interp_entry (_method)
#else
#define INTERP_ENTRY_UPDATE_HIT_COUNT(_method)
#endif

#define INTERP_ENTRY_BASE(_method, _this_arg, _res) \
	InterpEntryData data; \
	(data).rmethod = (_method); \
	(data).res = (_res); \
	(data).this_arg = (_this_arg); \
	(data).many_args = NULL;

#define INTERP_ENTRY_BASE_WITH_HIT_COUNT(_method, _this_arg, _res) \
	INTERP_ENTRY_BASE (_method, _this_arg, _res) \
	INTERP_ENTRY_UPDATE_HIT_COUNT (_method);

#define INTERP_ENTRY0(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	interp_entry (&data); \
	}
#define INTERP_ENTRY1(_this_arg, _res, _method) {	  \
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY2(_this_arg, _res, _method) {  \
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY3(_this_arg, _res, _method) { \
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY4(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY5(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY6(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY7(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	(data).args [6] = arg7; \
	interp_entry (&data); \
	}
#define INTERP_ENTRY8(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE_WITH_HIT_COUNT (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	(data).args [6] = arg7; \
	(data).args [7] = arg8; \
	interp_entry (&data); \
	}

#define ARGLIST0 InterpMethod *rmethod
#define ARGLIST1 gpointer arg1, InterpMethod *rmethod
#define ARGLIST2 gpointer arg1, gpointer arg2, InterpMethod *rmethod
#define ARGLIST3 gpointer arg1, gpointer arg2, gpointer arg3, InterpMethod *rmethod
#define ARGLIST4 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, InterpMethod *rmethod
#define ARGLIST5 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, InterpMethod *rmethod
#define ARGLIST6 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, InterpMethod *rmethod
#define ARGLIST7 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, gpointer arg7, InterpMethod *rmethod
#define ARGLIST8 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, gpointer arg7, gpointer arg8, InterpMethod *rmethod

static void interp_entry_static_0 (ARGLIST0) INTERP_ENTRY0 (NULL, NULL, rmethod)
static void interp_entry_static_1 (ARGLIST1) INTERP_ENTRY1 (NULL, NULL, rmethod)
static void interp_entry_static_2 (ARGLIST2) INTERP_ENTRY2 (NULL, NULL, rmethod)
static void interp_entry_static_3 (ARGLIST3) INTERP_ENTRY3 (NULL, NULL, rmethod)
static void interp_entry_static_4 (ARGLIST4) INTERP_ENTRY4 (NULL, NULL, rmethod)
static void interp_entry_static_5 (ARGLIST5) INTERP_ENTRY5 (NULL, NULL, rmethod)
static void interp_entry_static_6 (ARGLIST6) INTERP_ENTRY6 (NULL, NULL, rmethod)
static void interp_entry_static_7 (ARGLIST7) INTERP_ENTRY7 (NULL, NULL, rmethod)
static void interp_entry_static_8 (ARGLIST8) INTERP_ENTRY8 (NULL, NULL, rmethod)
static void interp_entry_static_ret_0 (gpointer res, ARGLIST0) INTERP_ENTRY0 (NULL, res, rmethod)
static void interp_entry_static_ret_1 (gpointer res, ARGLIST1) INTERP_ENTRY1 (NULL, res, rmethod)
static void interp_entry_static_ret_2 (gpointer res, ARGLIST2) INTERP_ENTRY2 (NULL, res, rmethod)
static void interp_entry_static_ret_3 (gpointer res, ARGLIST3) INTERP_ENTRY3 (NULL, res, rmethod)
static void interp_entry_static_ret_4 (gpointer res, ARGLIST4) INTERP_ENTRY4 (NULL, res, rmethod)
static void interp_entry_static_ret_5 (gpointer res, ARGLIST5) INTERP_ENTRY5 (NULL, res, rmethod)
static void interp_entry_static_ret_6 (gpointer res, ARGLIST6) INTERP_ENTRY6 (NULL, res, rmethod)
static void interp_entry_static_ret_7 (gpointer res, ARGLIST7) INTERP_ENTRY7 (NULL, res, rmethod)
static void interp_entry_static_ret_8 (gpointer res, ARGLIST8) INTERP_ENTRY8 (NULL, res, rmethod)
static void interp_entry_instance_0 (gpointer this_arg, ARGLIST0) INTERP_ENTRY0 (this_arg, NULL, rmethod)
static void interp_entry_instance_1 (gpointer this_arg, ARGLIST1) INTERP_ENTRY1 (this_arg, NULL, rmethod)
static void interp_entry_instance_2 (gpointer this_arg, ARGLIST2) INTERP_ENTRY2 (this_arg, NULL, rmethod)
static void interp_entry_instance_3 (gpointer this_arg, ARGLIST3) INTERP_ENTRY3 (this_arg, NULL, rmethod)
static void interp_entry_instance_4 (gpointer this_arg, ARGLIST4) INTERP_ENTRY4 (this_arg, NULL, rmethod)
static void interp_entry_instance_5 (gpointer this_arg, ARGLIST5) INTERP_ENTRY5 (this_arg, NULL, rmethod)
static void interp_entry_instance_6 (gpointer this_arg, ARGLIST6) INTERP_ENTRY6 (this_arg, NULL, rmethod)
static void interp_entry_instance_7 (gpointer this_arg, ARGLIST7) INTERP_ENTRY7 (this_arg, NULL, rmethod)
static void interp_entry_instance_8 (gpointer this_arg, ARGLIST8) INTERP_ENTRY8 (this_arg, NULL, rmethod)
static void interp_entry_instance_ret_0 (gpointer this_arg, gpointer res, ARGLIST0) INTERP_ENTRY0 (this_arg, res, rmethod)
static void interp_entry_instance_ret_1 (gpointer this_arg, gpointer res, ARGLIST1) INTERP_ENTRY1 (this_arg, res, rmethod)
static void interp_entry_instance_ret_2 (gpointer this_arg, gpointer res, ARGLIST2) INTERP_ENTRY2 (this_arg, res, rmethod)
static void interp_entry_instance_ret_3 (gpointer this_arg, gpointer res, ARGLIST3) INTERP_ENTRY3 (this_arg, res, rmethod)
static void interp_entry_instance_ret_4 (gpointer this_arg, gpointer res, ARGLIST4) INTERP_ENTRY4 (this_arg, res, rmethod)
static void interp_entry_instance_ret_5 (gpointer this_arg, gpointer res, ARGLIST5) INTERP_ENTRY5 (this_arg, res, rmethod)
static void interp_entry_instance_ret_6 (gpointer this_arg, gpointer res, ARGLIST6) INTERP_ENTRY6 (this_arg, res, rmethod)
static void interp_entry_instance_ret_7 (gpointer this_arg, gpointer res, ARGLIST7) INTERP_ENTRY7 (this_arg, res, rmethod)
static void interp_entry_instance_ret_8 (gpointer this_arg, gpointer res, ARGLIST8) INTERP_ENTRY8 (this_arg, res, rmethod)

#define INTERP_ENTRY_FUNCLIST(type) (gpointer)interp_entry_ ## type ## _0, (gpointer)interp_entry_ ## type ## _1, (gpointer)interp_entry_ ## type ## _2, (gpointer)interp_entry_ ## type ## _3, (gpointer)interp_entry_ ## type ## _4, (gpointer)interp_entry_ ## type ## _5, (gpointer)interp_entry_ ## type ## _6, (gpointer)interp_entry_ ## type ## _7, (gpointer)interp_entry_ ## type ## _8

static gpointer entry_funcs_static [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (static) };
static gpointer entry_funcs_static_ret [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (static_ret) };
static gpointer entry_funcs_instance [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (instance) };
static gpointer entry_funcs_instance_ret [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (instance_ret) };

/* General version for methods with more than MAX_INTERP_ENTRY_ARGS arguments */
static void
interp_entry_general (gpointer this_arg, gpointer res, gpointer *args, gpointer rmethod)
{
	INTERP_ENTRY_BASE ((InterpMethod*)rmethod, this_arg, res);
	data.many_args = args;
	interp_entry (&data);
}

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE

// Do not inline in case order of frame addresses matters.
static MONO_NEVER_INLINE void
interp_entry_from_trampoline (gpointer ccontext_untyped, gpointer rmethod_untyped)
{
	ThreadContext *context;
	stackval *sp;
	MonoMethod *method;
	MonoMethodSignature *sig;
	CallContext *ccontext = (CallContext*) ccontext_untyped;
	gboolean unbox = INTERP_IMETHOD_IS_TAGGED_UNBOX (rmethod_untyped);
	InterpMethod *rmethod = INTERP_IMETHOD_UNTAG_UNBOX (rmethod_untyped);
	gpointer orig_domain = NULL, attach_cookie;
	int i;

	if (rmethod->needs_thread_attach)
		orig_domain = mono_threads_attach_coop (mono_domain_get (), &attach_cookie);

	context = get_context ();
	sp = (stackval*)context->stack_pointer;

	method = rmethod->method;
	sig = mono_method_signature_internal (method);
	if (method->string_ctor) {
		MonoMethodSignature *newsig = (MonoMethodSignature*)g_alloca (MONO_SIZEOF_METHOD_SIGNATURE + ((sig->param_count + 2) * sizeof (MonoType*)));
		memcpy (newsig, sig, mono_metadata_signature_size (sig));
		newsig->ret = m_class_get_byval_arg (mono_defaults.string_class);
		sig = newsig;
	}

	InterpFrame frame = {0};
	frame.imethod = rmethod;
	frame.stack = sp;
	frame.retval = sp;

	gpointer call_info = mono_arch_get_interp_native_call_info (NULL, sig);

	/* Copy the args saved in the trampoline to the frame stack */
	gpointer retp = mono_arch_get_native_call_context_args (ccontext, &frame, sig, call_info);

	if (unbox) {
		g_assert (sig->hasthis);
		frame.stack->data.p = mono_object_unbox_internal ((MonoObject*)frame.stack->data.p);
	}

#ifdef MONO_ARCH_HAVE_SWIFTCALL
	int swift_error_arg_index = -1;
	gpointer swift_error_data;
	gpointer* swift_error_pointer;
	if (mono_method_signature_has_ext_callconv (sig, MONO_EXT_CALLCONV_SWIFTCALL)) {
		swift_error_data = mono_arch_get_swift_error (ccontext, sig, &swift_error_arg_index);

		int swift_error_offset = frame.imethod->swift_error_offset;
		if (swift_error_offset >= 0) {
			swift_error_pointer = (gpointer*)((guchar*)frame.stack + swift_error_offset);
			*swift_error_pointer = *(gpointer*)swift_error_data;
		}
	}
#endif

	/* Allocate storage for value types */
	stackval *newsp = sp;
	/* FIXME we should reuse computation on imethod for this */
	if (sig->hasthis)
		newsp++;
	for (i = 0; i < sig->param_count; i++) {
		MonoType *type = sig->params [i];
		int size;

		if (type->type == MONO_TYPE_GENERICINST && !MONO_TYPE_IS_REFERENCE (type)) {
			size = mono_class_value_size (mono_class_from_mono_type_internal (type), NULL);
		} else if (type->type == MONO_TYPE_VALUETYPE) {
			if (sig->pinvoke && !sig->marshalling_disabled)
				size = mono_class_native_size (m_type_data_get_klass_unchecked (type), NULL);
			else
				size = mono_class_value_size (m_type_data_get_klass_unchecked (type), NULL);
		} else {
			size = MINT_STACK_SLOT_SIZE;
		}
#ifdef MONO_ARCH_HAVE_SWIFTCALL
		if (swift_error_arg_index >= 0 && swift_error_arg_index == i)
			newsp->data.p = swift_error_pointer;
#endif
		newsp = STACK_ADD_BYTES (newsp, size);
	}
	newsp = (stackval*)ALIGN_TO (newsp, MINT_STACK_ALIGNMENT);
	context->stack_pointer = (guchar*)newsp;
	g_assert (context->stack_pointer < context->stack_end);

	MONO_ENTER_GC_UNSAFE;
	mono_interp_exec_method (&frame, context, NULL);
	MONO_EXIT_GC_UNSAFE;

#ifdef MONO_ARCH_HAVE_SWIFTCALL
	if (swift_error_arg_index >= 0)
		*(gpointer*)swift_error_data = *(gpointer*)swift_error_pointer;
#endif

	context->stack_pointer = (guchar*)sp;
	g_assert (!context->has_resume_state);

	if (rmethod->needs_thread_attach)
		mono_threads_detach_coop (orig_domain, &attach_cookie);

	if (need_native_unwind (context)) {
		mono_llvm_start_native_unwind ();
		return;
	}

	/* Write back the return value */
	/* 'frame' is still valid */
	mono_arch_set_native_call_context_ret (ccontext, &frame, sig, call_info, retp);

	mono_arch_free_interp_native_call_info (call_info);
}

#else

static void
interp_entry_from_trampoline (gpointer ccontext_untyped, gpointer rmethod_untyped)
{
	g_assert_not_reached ();
}

#endif /* MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE */

static void
interp_entry_llvmonly (gpointer res, gpointer *args, gpointer imethod_untyped)
{
	InterpMethod *imethod = (InterpMethod*)imethod_untyped;

	if (imethod->hasthis)
		interp_entry_general (*(gpointer*)(args [0]), res, args + 1, imethod);
	else
		interp_entry_general (NULL, res, args, imethod);
}

static gpointer
interp_get_interp_method (MonoMethod *method)
{
	return mono_interp_get_imethod (method);
}

static MonoJitInfo*
interp_compile_interp_method (MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod = mono_interp_get_imethod (method);

	if (!imethod->transformed) {
		mono_interp_transform_method (imethod, get_context (), error);
		return_val_if_nok (error, NULL);
	}

	return imethod->jinfo;
}

#ifndef MONO_ARCH_HAVE_INTERP_NATIVE_TO_MANAGED
static void
interp_no_native_to_managed (void)
{
	g_error ("interpreter: native-to-managed transition not available on this platform");
}
#endif

static void
no_llvmonly_interp_method_pointer (void)
{
	g_assert_not_reached ();
}

/*
 * interp_create_method_pointer_llvmonly:
 *
 *   Return an ftndesc for entering the interpreter and executing METHOD.
 */
static MonoFtnDesc*
interp_create_method_pointer_llvmonly (MonoMethod *method, gboolean unbox, MonoError *error)
{
	gpointer addr, entry_func = NULL, entry_wrapper;
	MonoMethodSignature *sig;
	MonoMethod *wrapper;
	InterpMethod *imethod;

	imethod = mono_interp_get_imethod (method);

	if (unbox) {
		if (imethod->llvmonly_unbox_entry)
			return (MonoFtnDesc*)imethod->llvmonly_unbox_entry;
	} else {
		if (imethod->jit_entry)
			return (MonoFtnDesc*)imethod->jit_entry;
	}

	sig = mono_method_signature_internal (method);

	/*
	 * The entry functions need access to the method to call, so we have
	 * to use a ftndesc. The caller uses a normal signature, while the
	 * entry functions use a gsharedvt_in signature, so wrap the entry function in
	 * a gsharedvt_in_sig wrapper.
	 * We use a gsharedvt_in_sig wrapper instead of an interp_in wrapper, because they
	 * are mostly the same, and they are already generated. The exception is the
	 * wrappers for methods with more than 8 arguments, those are different.
	 */
	if (sig->param_count > MAX_INTERP_ENTRY_ARGS)
		wrapper = mini_get_interp_in_wrapper (sig);
	else
		wrapper = mini_get_gsharedvt_in_sig_wrapper (sig);

	entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
	if (!is_ok (error)) {
		mono_error_cleanup (error);
		error_init_reuse (error);
		entry_wrapper = NULL;
	}

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer)interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance [sig->param_count];
		else
			entry_func = entry_funcs_instance_ret [sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static [sig->param_count];
		else
			entry_func = entry_funcs_static_ret [sig->param_count];
	}
	g_assert (entry_func);

#if HOST_BROWSER
	// FIXME: We don't support generating wasm trampolines for high arg counts yet
	if (
		(sig->param_count <= MAX_INTERP_ENTRY_ARGS) &&
		mono_opt_jiterpreter_interp_entry_enabled
	) {
		jiterp_preserve_module();

		gpointer wasm_entry_func = mono_interp_jit_wasm_entry_trampoline (
			imethod, method, sig->param_count, (MonoType *)sig->params,
			unbox, sig->hasthis, sig->ret->type != MONO_TYPE_VOID,
			entry_func
		);

		// Compiling a trampoline can fail for various reasons, so in that case we will fall back to the pre-existing ones below
		if (wasm_entry_func)
			entry_func = wasm_entry_func;
	}
#endif

	if (!entry_wrapper) {
		mono_interp_error_cleanup (error);

#ifdef HOST_WASM
		if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
			entry_wrapper = mono_jit_compile_method_jit_only (mini_get_interp_in_wrapper (sig), error);
			if (!is_ok (error)) {
				mono_error_cleanup (error);
				error_init_reuse (error);
				entry_wrapper = NULL;
			}
		}

		if (!entry_wrapper && !mono_method_has_unmanaged_callers_only_attribute (method)) {
			entry_wrapper = mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_entry_from_trampoline", (gpointer) mono_interp_entry_from_trampoline), error);
			if (!is_ok (error)) {
				mono_error_cleanup (error);
				error_init_reuse (error);
				entry_wrapper = NULL;
			}
			if (entry_wrapper)
				entry_func = (gpointer)interp_entry_from_trampoline;
		}

		if (entry_wrapper)
			goto have_entry_wrapper;
#endif

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
		if (!mono_native_to_interp_trampoline) {
			if (mono_aot_only) {
				mono_native_to_interp_trampoline = (MonoFuncV)mono_aot_get_trampoline ("native_to_interp_trampoline");
			} else {
				MonoTrampInfo *info;
				mono_native_to_interp_trampoline = (MonoFuncV)mono_arch_get_native_to_interp_trampoline (&info);
				mono_tramp_info_register (info, NULL);
			}
		}
		entry_wrapper = (gpointer)mono_native_to_interp_trampoline;

		/* Match the normal interp fallback path to preserve thread/LMF semantics. */
		if (sig->pinvoke) {
			entry_func = (gpointer)interp_entry_from_trampoline;
		} else {
			static gpointer cached_func = NULL;
			if (!cached_func) {
				cached_func = mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_entry_from_trampoline", (gpointer) mono_interp_entry_from_trampoline), error);
				if (!is_ok (error)) {
					mono_error_cleanup (error);
					error_init_reuse (error);
					cached_func = (gpointer)interp_entry_from_trampoline;
				} else {
					mono_memory_barrier ();
				}
			}
			entry_func = cached_func;
		}
#else
		char *wrapper_name = mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL);
		char *method_name = mono_method_get_name_full (method, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL);
		mono_error_set_execution_engine (error, "couldn't compile wrapper \"%s\" for \"%s\"", wrapper_name, method_name);
		g_free (wrapper_name);
		g_free (method_name);
		return NULL;
#endif
	}

have_entry_wrapper:

	/* Encode unbox in the lower bit of imethod */
	gpointer entry_arg = imethod;
	if (unbox)
		entry_arg = (gpointer)(((gsize)entry_arg) | 1);

	MonoFtnDesc *entry_ftndesc = mini_llvmonly_create_ftndesc (method, entry_func, entry_arg);

	addr = mini_llvmonly_create_ftndesc (method, entry_wrapper, entry_ftndesc);

	mono_memory_barrier ();
	if (unbox)
		imethod->llvmonly_unbox_entry = addr;
	else
		imethod->jit_entry = addr;

	return (MonoFtnDesc*)addr;
}

/*
 * interp_create_method_pointer:
 *
 * Return a function pointer which can be used to call METHOD using the
 * interpreter. Return NULL for methods which are not supported.
 */
static gpointer
interp_create_method_pointer (MonoMethod *method, gboolean compile, MonoError *error)
{
	gpointer addr, entry_func, entry_wrapper = NULL;
	InterpMethod *imethod = mono_interp_get_imethod (method);

	if (imethod->jit_entry)
		return imethod->jit_entry;

	if (compile && !imethod->transformed) {
		/* Return any errors from method compilation */
		mono_interp_transform_method (imethod, get_context (), error);
		return_val_if_nok (error, NULL);
	}

	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (method->string_ctor) {
		MonoMethodSignature *newsig = (MonoMethodSignature*)g_alloca (MONO_SIZEOF_METHOD_SIGNATURE + ((sig->param_count + 2) * sizeof (MonoType*)));
		memcpy (newsig, sig, mono_metadata_signature_size (sig));
		newsig->ret = m_class_get_byval_arg (mono_defaults.string_class);
		sig = newsig;
	}

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer)interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance [sig->param_count];
		else
			entry_func = entry_funcs_instance_ret [sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static [sig->param_count];
		else
			entry_func = entry_funcs_static_ret [sig->param_count];
	}

#ifndef MONO_ARCH_HAVE_INTERP_NATIVE_TO_MANAGED
#ifdef HOST_WASM
	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (method);
		MonoMethod *orig_method = info->d.native_to_managed.method;

		/*
		 * These are called from native code. Ask the host app for a trampoline.
		 *
		 * Fast path: if the wrapper has an AOT-compiled native body, install it
		 * directly into the slot with the WASM_N2M_AOT_DIRECT_ARG sentinel (the
		 * generated wasm_native_to_interp_* stub branches on this). This skips
		 * the per-call interp_entry + mono_interp_exec_method round-trip for
		 * every [MonoPInvokeCallback]/[UnmanagedCallersOnly] wrapper that landed
		 * in the AOT image. The wrapper IS the wrapper MonoMethod we got from
		 * mono_marshal_get_managed_wrapper above, so it matches the one
		 * add_native_to_managed_wrappers added to the image (both go through
		 * the per-method wrapper cache, which is keyed on the target method).
		 */
		MonoFtnDesc *ftndesc = g_new0 (MonoFtnDesc, 1);
		ERROR_DECL (aot_err);
		gpointer aot_body = mono_aot_get_method (method, aot_err);
		mono_error_cleanup (aot_err);
		if (aot_body) {
			ftndesc->addr = aot_body;
			ftndesc->arg = (gpointer)(intptr_t)-1; /* WASM_N2M_AOT_DIRECT_ARG */
		} else {
			ftndesc->addr = entry_func;
			ftndesc->arg = imethod;
		}

		addr = mono_wasm_get_native_to_interp_trampoline (orig_method, ftndesc);
		if (addr) {
			mono_memory_barrier ();
			imethod->jit_entry = addr;
			return addr;
		}

		/*
		 * The runtime expects a function pointer unique to method and
		 * the native caller expects a function pointer with the
		 * right signature, so fail right away.
		 */
		char *s = mono_method_get_full_name (orig_method);
		char *msg = g_strdup_printf ("No native to managed transition for method '%s', missing [UnmanagedCallersOnly] attribute.", s);
		mono_error_set_platform_not_supported (error, msg);
		g_free (s);
		g_free (msg);
		return NULL;
	}
#endif
	return (gpointer)interp_no_native_to_managed;
#endif

	if (mono_llvm_only) {
		/* The caller should call interp_create_method_pointer_llvmonly */
		//g_assert_not_reached ();
		return (gpointer)no_llvmonly_interp_method_pointer;
	}

#ifndef MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE
	/*
	 * Interp in wrappers get the argument in the rgctx register. If
	 * MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE is defined it means that
	 * on that arch the rgctx register is not scratch, so we use a
	 * separate temp register. We should update the wrappers for this
	 * if we really care about those architectures (arm).
	 */

	MonoMethod *wrapper = NULL;
#ifdef MONO_ARCH_HAVE_SWIFTCALL
	/* Methods with Swift cconv should go to trampoline */
	if (!mono_method_signature_has_ext_callconv (sig, MONO_EXT_CALLCONV_SWIFTCALL))
#endif
	{
		wrapper = mini_get_interp_in_wrapper (sig);
		entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
	}
#endif
	if (!entry_wrapper) {
#ifndef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
		g_assertion_message ("couldn't compile wrapper \"%s\" for \"%s\"",
				mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL),
				mono_method_get_name_full (method,  TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL));
#else
		mono_interp_error_cleanup (error);
		if (!mono_native_to_interp_trampoline) {
			if (mono_aot_only) {
				mono_native_to_interp_trampoline = (MonoFuncV)mono_aot_get_trampoline ("native_to_interp_trampoline");
			} else {
				MonoTrampInfo *info;
				mono_native_to_interp_trampoline = (MonoFuncV)mono_arch_get_native_to_interp_trampoline (&info);
				mono_tramp_info_register (info, NULL);
			}
		}
		entry_wrapper = (gpointer)mono_native_to_interp_trampoline;
		/* We need the lmf wrapper only when being called from mixed mode */
		if (sig->pinvoke)
			entry_func = (gpointer)interp_entry_from_trampoline;
		else {
			static gpointer cached_func = NULL;
			if (!cached_func) {
				cached_func = mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_entry_from_trampoline", (gpointer) mono_interp_entry_from_trampoline), error);
				mono_memory_barrier ();
			}
			entry_func = cached_func;
		}
#endif
	}

	g_assert (entry_func);
	/* This is the argument passed to the interp_in wrapper by the static rgctx trampoline */
	MonoFtnDesc *ftndesc = g_new0 (MonoFtnDesc, 1);
	ftndesc->addr = entry_func;
	ftndesc->arg = imethod;
	mono_error_assert_ok (error);

	/*
	 * The wrapper is called by compiled code, which doesn't pass the extra argument, so we pass it in the
	 * rgctx register using a trampoline.
	 */

	addr = mono_create_ftnptr_arg_trampoline (ftndesc, entry_wrapper);

	mono_memory_barrier ();
	imethod->jit_entry = addr;

	return addr;
}

static void
interp_free_method (MonoMethod *method)
{
	MonoJitMemoryManager *jit_mm = jit_mm_for_method (method);
	MonoDynamicMethod *dmethod = (MonoDynamicMethod*)method;

	jit_mm_lock (jit_mm);

	InterpMethod *imethod = (InterpMethod*)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, method);
	if (imethod) {
#if HOST_BROWSER
		mono_jiterp_free_method_data (method, imethod);
#endif

		mono_interp_clear_data_items_patch_sites (imethod->data_items, imethod->n_data_items);

		mono_internal_hash_table_remove (&jit_mm->interp_code_hash, method);
	}

	jit_mm_unlock (jit_mm);

	if (dmethod->mp) {
		mono_mempool_destroy (dmethod->mp);
		dmethod->mp = NULL;
	}
}

#if COUNT_OPS
static long opcode_counts[MINT_LASTOP];

#define COUNT_OP(op) opcode_counts[op]++
#else
#define COUNT_OP(op)
#endif

#if DEBUG_INTERP
#define DUMP_INSTR() \
	if (tracing > 1) { \
		output_indent (); \
		char *mn = mono_method_full_name (frame->imethod->method, FALSE); \
		g_print ("(%p) %s -> IL_%04x: %-10s\n", mono_thread_internal_current (), mn, (gint32)(ip - frame->imethod->code), mono_interp_opname (*ip)); \
		g_free (mn); \
	}
#else
#define DUMP_INSTR()
#endif

static MONO_NEVER_INLINE MonoException*
do_init_vtable (MonoVTable *vtable, MonoError *error, InterpFrame *frame, const guint16 *ip)
{
	MonoLMFExt ext;
	MonoException *ex = NULL;

	/*
	 * When calling runtime functions we pass the ip of the instruction triggering the runtime call.
	 * Offset the subtraction from interp_frame_get_ip, so we don't end up in prev instruction.
	 */
	frame->state.ip = ip + 1;

	interp_push_lmf (&ext, frame);
	mono_runtime_class_init_full (vtable, error);
	if (!is_ok (error))
		ex = mono_error_convert_to_exception (error);
	interp_pop_lmf (&ext);
	return ex;
}

#define INIT_VTABLE(vtable) do { \
		if (G_UNLIKELY (!(vtable)->initialized)) { \
			MonoException *__init_vtable_ex = do_init_vtable ((vtable), error, frame, ip); \
			if (G_UNLIKELY (__init_vtable_ex)) \
				THROW_EX (__init_vtable_ex, ip); \
		} \
	} while (0);

static MonoObject*
mono_interp_new (MonoClass* klass)
{
	ERROR_DECL (error);
	MonoObject* const object = mono_object_new_checked (klass, error);
	mono_error_cleanup (error); // FIXME: do not swallow the error
	return object;
}

static gboolean
mono_interp_isinst (MonoObject* object, MonoClass* klass)
{
	ERROR_DECL (error);
	gboolean isinst;
#ifdef HOST_BROWSER
	/* DIAG (isinst OOB): an object reaching here from a JITted caller (mono_wasm_jit_call_interp, e.g. the
	 * LandPathNodeMaker pathfinding chain) has been observed with a garbage vtable -> mono_object_class()'s
	 * vtable->klass read OOBs ("memory access out of bounds"), and because this runs on a JSPI-suspended
	 * stack the resulting wasm trap silently kills the worker (the GC then stalls trying to suspend it),
	 * giving a context-free crash. Validate object + vtable + klass via RAW word reads (no struct deref, so
	 * no OOB) BEFORE mono_object_class. On garbage, log (rate-limited, greppable) and return FALSE instead
	 * of dereferencing — the run continues so we can see how often / with what target type it happens. */
	if (G_UNLIKELY (object != NULL)) {
		gsize o = (gsize) object;
		gsize memsz = wj_memsz ();
		gboolean bad = !wj_probe_ok (o, memsz);
		gsize vt = 0, kl = 0;
		if (!bad) { vt = *(gsize *) o;  bad = !vt || !wj_probe_ok (vt, memsz); }     /* object->vtable */
		if (!bad) { kl = *(gsize *) vt; bad = !kl || !wj_probe_ok (kl, memsz); }     /* vtable->klass */
		if (G_UNLIKELY (bad)) {
			static int z = 0;
			if (z++ < 40) {
				printf ("WASM_JIT_ISINST_GARBAGE object=%p vtable=0x%x klass=0x%x target=%s.%s — JIT handed a stale/wrong object to the interp; returning FALSE (not trapping)\n",
					(void *) object, (unsigned) vt, (unsigned) kl,
					klass ? m_class_get_name_space (klass) : "?", klass ? m_class_get_name (klass) : "?");
				fflush (stdout);
			}
			return FALSE;
		}
	}
#endif
	MonoClass *obj_class = mono_object_class (object);
	mono_class_is_assignable_from_checked (klass, obj_class, &isinst, error);
	mono_error_cleanup (error); // FIXME: do not swallow the error
	return isinst;
}

static MONO_NEVER_INLINE InterpMethod*
mono_interp_get_native_func_wrapper (InterpMethod* imethod, MonoMethodSignature* csignature, guchar* code)
{
	/* Pinvoke call is missing the wrapper. See mono_get_native_calli_wrapper */
	MonoMarshalSpec** mspecs = g_newa0 (MonoMarshalSpec*, csignature->param_count + 1);

	MonoMethodPInvoke iinfo;
	memset (&iinfo, 0, sizeof (iinfo));

	MonoMethod *method = imethod->method;
	MonoImage *image = NULL;
	if (imethod->method->dynamic)
		image = ((MonoDynamicMethod*)method)->assembly->image;
	else
		image = m_class_get_image (method->klass);
	MonoMethod* m = mono_marshal_get_native_func_wrapper (image, csignature, &iinfo, mspecs, code);

	for (int i = csignature->param_count; i >= 0; i--)
		if (mspecs [i])
			mono_metadata_free_marshal_spec (mspecs [i]);

	InterpMethod *cmethod = mono_interp_get_imethod (m);

	return cmethod;
}

// Do not inline in case order of frame addresses matters.
static MONO_NEVER_INLINE MonoException*
mono_interp_leave (InterpFrame* parent_frame)
{
	InterpFrame frame = {parent_frame};
	gboolean gc_transitions = FALSE;
	stackval tmp_sp;
	/*
	 * We need for mono_thread_get_undeniable_exception to be able to unwind
	 * to check the abort threshold. For this to work we use frame as a
	 * dummy frame that is stored in the lmf and serves as the transition frame
	 */
	do_icall_wrapper (&frame, NULL, MINT_ICALLSIG_V_P, &tmp_sp, &tmp_sp, (gpointer)mono_thread_get_undeniable_exception, FALSE, &gc_transitions);

	return (MonoException*)tmp_sp.data.p;
}

static gint32
mono_interp_enum_hasflag (stackval *sp1, stackval *sp2, MonoClass* klass)
{
	guint64 a_val = 0, b_val = 0;

	stackval_to_data (m_class_get_byval_arg (klass), sp1, &a_val, FALSE);
	stackval_to_data (m_class_get_byval_arg (klass), sp2, &b_val, FALSE);
	return (a_val & b_val) == b_val;
}

static void
interp_simd_create (gpointer dest, gpointer args, int el_size)
{
	const int num_elements = SIZEOF_V128 / el_size;
	gint8 res_buffer [SIZEOF_V128];
	for (int i = 0; i < num_elements; i++) {
		switch (el_size) {
			case 1: res_buffer [i] = *(gint8*)args; break;
			case 2: ((gint16*)res_buffer) [i] = *(gint16*)args; break;
			case 4: ((gint32*)res_buffer) [i] = *(gint32*)args; break;
			case 8: ((gint64*)res_buffer) [i] = *(gint64*)args; break;
			default:
				g_assert_not_reached ();
		}
		args = (gpointer) ((char*)args + MINT_STACK_SLOT_SIZE);
	}

	memcpy (dest, res_buffer, SIZEOF_V128);
}

// varargs in wasm consumes extra linear stack per call-site.
// These g_warning/g_error wrappers fix that. It is not the
// small wasm stack, but conserving it is still desirable.
static void
g_warning_d (const char *format, int d)
{
	g_warning (format, d);
}

#if !USE_COMPUTED_GOTO
static void
interp_error_xsx (const char *format, int x1, const char *s, int x2)
{
	g_error (format, x1, s, x2);
}
#endif

static MONO_ALWAYS_INLINE gboolean
method_entry (ThreadContext *context, InterpFrame *frame,
#if DEBUG_INTERP
	int *out_tracing,
#endif
	MonoException **out_ex)
{
	gboolean slow = FALSE;

#if DEBUG_INTERP
	debug_enter (frame, out_tracing);
#endif
#if PROFILE_INTERP
	frame->imethod->calls++;
#endif

	*out_ex = NULL;
	if (!G_UNLIKELY (frame->imethod->transformed)) {
		slow = TRUE;
		MonoException *ex = do_transform_method (frame->imethod, frame, context);
		if (ex) {
			*out_ex = ex;
			/*
			 * Initialize the stack base pointer here, in the uncommon branch, so we don't
			 * need to check for it everytime when exitting a frame.
			 */
			frame->stack = (stackval*)context->stack_pointer;
			return slow;
		}
	} else {
		mono_memory_read_barrier ();
	}

	return slow;
}

/* Save the state of the interpreter main loop into FRAME */
#define SAVE_INTERP_STATE(frame) do { \
	frame->state.ip = ip;  \
	} while (0)

/* Load and clear state from FRAME */
#define LOAD_INTERP_STATE(frame) do { \
	ip = frame->state.ip; \
	locals = (unsigned char *)frame->stack; \
	frame->state.ip = NULL; \
	} while (0)

/* Initialize interpreter state for executing FRAME */
#define INIT_INTERP_STATE(frame, _clause_args) do {	 \
	ip = _clause_args ? ((FrameClauseArgs *)_clause_args)->start_with_ip : (frame)->imethod->code; \
	locals = (unsigned char *)(frame)->stack; \
	} while (0)

#if PROFILE_INTERP
static long total_executed_opcodes;
#endif

#define LOCAL_VAR(offset,type) (*(type*)(locals + (offset)))

// The start of the stack has a reserved slot for a GC visible temp object pointer
#ifdef TARGET_WASM
#define SET_TEMP_POINTER(value) (*((volatile MonoObject * volatile *)context->stack_start) = value)
#else
#define SET_TEMP_POINTER(value) (*((MonoObject **)context->stack_start) = value)
#endif

/*
 * Custom C implementations of the min/max operations for float and double.
 * We cannot directly use the C stdlib functions because their semantics do not match
 *  the C# methods in System.Math, but having interpreter opcodes for these operations
 *  improves performance for FP math a lot in some cases.
 */
static float
min_f (float lhs, float rhs)
{
	if (mono_isnan (lhs))
		return lhs;
	else if (mono_isnan (rhs))
		return rhs;
	else if (lhs == rhs)
		return mono_signbit (lhs) ? lhs : rhs;
	else
		return fminf (lhs, rhs);
}

static float
max_f (float lhs, float rhs)
{
	if (mono_isnan (lhs))
		return lhs;
	else if (mono_isnan (rhs))
		return rhs;
	else if (lhs == rhs)
		return mono_signbit (rhs) ? lhs : rhs;
	else
		return fmaxf (lhs, rhs);
}

static double
min_d (double lhs, double rhs)
{
	if (mono_isnan (lhs))
		return lhs;
	else if (mono_isnan (rhs))
		return rhs;
	else if (lhs == rhs)
		return mono_signbit (lhs) ? lhs : rhs;
	else
		return fmin (lhs, rhs);
}

static double
max_d (double lhs, double rhs)
{
	if (mono_isnan (lhs))
		return lhs;
	else if (mono_isnan (rhs))
		return rhs;
	else if (lhs == rhs)
		return mono_signbit (rhs) ? lhs : rhs;
	else
		return fmax (lhs, rhs);
}

// Equivalent of mono_get_addr_compiled_method
static gpointer
interp_ldvirtftn_delegate (gpointer arg, MonoDelegate *del)
{
	MonoMethod *virtual_method = del->method;
	ERROR_DECL(error);

	MonoClass *klass = del->object.vtable->klass;
	MonoMethod *invoke = mono_get_delegate_invoke_internal (klass);
	MonoMethodSignature *invoke_sig = mono_method_signature_internal (invoke);

	MonoClass *arg_class = NULL;
	if (m_type_is_byref (invoke_sig->params [0])) {
		arg_class = mono_class_from_mono_type_internal (invoke_sig->params [0]);
	} else {
		MonoObject *object = (MonoObject*)arg;
		arg_class = object->vtable->klass;
	}

	MonoMethod *res = mono_class_get_virtual_method (arg_class, virtual_method, error);
	mono_error_assert_ok (error);

	gboolean need_unbox = m_class_is_valuetype (res->klass) && !m_class_is_valuetype (virtual_method->klass);

	InterpMethod *imethod = mono_interp_get_imethod (res);
	return imethod_to_ftnptr (imethod, need_unbox);
}

static MONO_NEVER_INLINE void
mono_interp_trace_with_ctx (InterpFrame *frame, void (*trace_cb)(MonoMethod*,MonoJitInfo*,MonoProfilerCallContext*))
{
	MonoProfilerCallContext prof_ctx;
	MonoLMFExt ext;
	memset (&prof_ctx, 0, sizeof (MonoProfilerCallContext));
	prof_ctx.interp_frame = frame;
	prof_ctx.method = frame->imethod->method;
	prof_ctx.return_value = frame->retval;
	interp_push_lmf (&ext, frame);
	trace_cb (frame->imethod->method, frame->imethod->jinfo, &prof_ctx);
	interp_pop_lmf (&ext);
}

static MONO_NEVER_INLINE void
mono_interp_profiler_raise_with_ctx (InterpFrame *frame, void (*prof_cb)(MonoMethod*,MonoProfilerCallContext*))
{
	MonoProfilerCallContext prof_ctx;
	MonoLMFExt ext;
	memset (&prof_ctx, 0, sizeof (MonoProfilerCallContext));
	prof_ctx.interp_frame = frame;
	prof_ctx.method = frame->imethod->method;
	prof_ctx.return_value = frame->retval;
	interp_push_lmf (&ext, frame);
	prof_cb (frame->imethod->method, &prof_ctx);
	interp_pop_lmf (&ext);
}

static MONO_NEVER_INLINE void
mono_interp_profiler_raise (InterpFrame *frame, void (*prof_cb)(MonoMethod*,MonoProfilerCallContext*))
{
	MonoLMFExt ext;
	interp_push_lmf (&ext, frame);
	prof_cb (frame->imethod->method, NULL);
	interp_pop_lmf (&ext);
}

static MONO_NEVER_INLINE void
mono_interp_profiler_raise_tail_call (InterpFrame *frame, MonoMethod *new_method)
{
	MonoLMFExt ext;
	interp_push_lmf (&ext, frame);
	mono_profiler_raise_method_tail_call (frame->imethod->method, new_method);
	interp_pop_lmf (&ext);
}

#define INTERP_PROFILER_RAISE(name_lower, name_upper) \
	if ((flag & TRACING_FLAG) || ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_ ## name_lower) && \
			(frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_ ## name_upper ## _CONTEXT ))) { \
		if (flag & TRACING_FLAG) \
			mono_interp_trace_with_ctx (frame, mono_trace_ ## name_lower ## _method); \
		if (flag & PROFILING_FLAG) { \
			frame->state.ip = ip; \
			mono_interp_profiler_raise_with_ctx (frame, mono_profiler_raise_method_ ## name_lower); \
		} \
	} else if ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_ ## name_lower)) { \
		frame->state.ip = ip; \
		mono_interp_profiler_raise (frame, mono_profiler_raise_method_ ## name_lower); \
	}

#define INTERP_PROFILER_RAISE_SAMPLEPOINT() \
	if ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_samplepoint)) { \
		frame->state.ip = ip; \
		mono_interp_profiler_raise (frame, mono_profiler_raise_method_samplepoint); \
	}

/*
 * If CLAUSE_ARGS is non-null, start executing from it.
 * The ERROR argument is used to avoid declaring an error object for every interp frame, its not used
 * to return error information.
 * FRAME is only valid until the next call to alloc_frame ().
 */
static MONO_NEVER_INLINE void
mono_interp_exec_method (InterpFrame *frame, ThreadContext *context, FrameClauseArgs *clause_args)
{
	InterpMethod *cmethod;
	ERROR_DECL(error);

	/* Interpreter main loop state (InterpState) */
	const guint16 *ip = NULL;
	unsigned char *locals = NULL;
	int call_args_offset;
	int return_offset;
	gboolean gc_transitions = FALSE;

#if DEBUG_INTERP
	int tracing = global_tracing;
#endif
#if USE_COMPUTED_GOTO
	static void * const in_labels[] = {
#define OPDEF(a,b,c,d,e,f) &&LAB_ ## a,
#define IROPDEF(a,b,c,d,e,f)
#include "mintops.def"
	};
#endif

	/*
	 * GC SAFETY:
	 *
	 *  The interpreter executes in gc unsafe (non-preempt) mode. On wasm, we cannot rely on
	 * scanning the stack or any registers. In order to make the code GC safe, every objref
	 * handled by the code needs to be kept alive and pinned in any of the following ways:
	 * - the object needs to be stored on the interpreter stack. In order to make sure the
	 * object actually gets stored on the interp stack and the store is not optimized out,
	 * the store/variable should be volatile.
	 * - if the execution of an opcode requires an object not coming from interp stack to be
	 * kept alive, the tmp_handle below can be used. This handle will keep only one object
	 * pinned by the GC. Ideally, once this object is no longer needed, the handle should be
	 * cleared. If we will need to have more objects pinned simultaneously, additional handles
	 * can be reserved here.
	 */
	MonoException *method_entry_ex;
	if (method_entry (context, frame,
#if DEBUG_INTERP
		&tracing,
#endif
		&method_entry_ex)) {
		if (method_entry_ex)
			THROW_EX (method_entry_ex, NULL);
		EXCEPTION_CHECKPOINT;
		CHECK_RESUME_STATE (context);
	}

	if (!clause_args) {
		context->stack_pointer = (guchar*)frame->stack + frame->imethod->alloca_size;
		g_assert (context->stack_pointer < context->stack_end);
		/* Make sure the stack pointer is bumped before we store any references on the stack */
		mono_compiler_barrier ();
	}

	INIT_INTERP_STATE (frame, clause_args);

	if (clause_args && clause_args->run_until_end)
		/*
		 * Called from run_with_il_state to run the method until the end.
		 * Clear this out so it doesn't confuse the rest of the code.
		 */
		clause_args = NULL;

#ifdef ENABLE_EXPERIMENT_TIERED
	mini_tiered_inc (frame->imethod->method, &frame->imethod->tiered_counter, 0);
#endif
	//g_print ("(%p) Call %s\n", mono_thread_internal_current (), mono_method_get_full_name (frame->imethod->method));

#if defined(ENABLE_HYBRID_SUSPEND) || defined(ENABLE_COOP_SUSPEND)
	mono_threads_safepoint ();
#endif
main_loop:
	/*
	 * using while (ip < end) may result in a 15% performance drop,
	 * but it may be useful for debug
	 */
	while (1) {
#if PROFILE_INTERP
		frame->imethod->opcounts++;
		total_executed_opcodes++;
#endif
		MintOpcode opcode;
		DUMP_INSTR();
		MINT_IN_SWITCH (*ip) {
		MINT_IN_CASE(MINT_INITLOCAL)
		MINT_IN_CASE(MINT_INITLOCALS)
			memset (locals + ip [1], 0, ip [2]);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NIY)
			g_printf ("MONO interpreter: NIY encountered in method %s\n", mono_method_full_name (frame->imethod->method, TRUE));
			g_assert_not_reached ();
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BREAK)
			++ip;
			SAVE_INTERP_STATE (frame);
			do_debugger_tramp (mono_component_debugger ()->user_break, frame);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BREAKPOINT)
			++ip;
			mono_break ();
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_INIT_ARGLIST) {
			const guint16 *call_ip = frame->parent->state.ip - 6;
			g_assert_checked (*call_ip == MINT_CALL_VARARG);
			int params_stack_size = call_ip [5];
			MonoMethodSignature *sig = (MonoMethodSignature*)frame->parent->imethod->data_items [call_ip [4]];

			// we are being overly conservative with the size here, for simplicity
			gpointer arglist = frame_data_allocator_alloc (&context->data_stack, frame, params_stack_size + MINT_STACK_SLOT_SIZE);

			init_arglist (frame, sig, STACK_ADD_BYTES (frame->stack, ip [2]), (char*)arglist);

			// save the arglist for future access with MINT_ARGLIST
			LOCAL_VAR (ip [1], gpointer) = arglist;

			ip += 3;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LDC_I4_0)
			LOCAL_VAR (ip [1], gint32) = 0;
			ip += 2;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I4_1)
			LOCAL_VAR (ip [1], gint32) = 1;
			ip += 2;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I4_S)
			LOCAL_VAR (ip [1], gint32) = (short)ip [2];
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I4)
			LOCAL_VAR (ip [1], gint32) = READ32 (ip + 2);
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I8_0)
			LOCAL_VAR (ip [1], gint64) = 0;
			ip += 2;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I8)
			LOCAL_VAR (ip [1], gint64) = READ64 (ip + 2);
			ip += 6;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_I8_S)
			LOCAL_VAR (ip [1], gint64) = (short)ip [2];
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDC_R4) {
			LOCAL_VAR (ip [1], gint32) = READ32(ip + 2); /* not union usage */
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDC_R8)
			LOCAL_VAR (ip [1], gint64) = READ64 (ip + 2); /* note union usage */
			ip += 6;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_TAILCALL)
		MINT_IN_CASE(MINT_TAILCALL_VIRT)
		MINT_IN_CASE(MINT_JMP) {
			gboolean is_tailcall = *ip != MINT_JMP;
			InterpMethod *new_method;

			if (is_tailcall) {
				guint16 params_offset = ip [1];
				guint16 params_size = ip [3];

				new_method = (InterpMethod*)frame->imethod->data_items [ip [2]];

				if (*ip == MINT_TAILCALL_VIRT) {
					gint16 slot = (gint16)ip [4];
					MonoObject **this_arg_p = (MonoObject **)((guchar*)frame->stack + params_offset);
					MonoObject *this_arg = *this_arg_p;
					new_method = get_virtual_method_fast (new_method, this_arg->vtable, slot);
					if (m_class_is_valuetype (this_arg->vtable->klass) && m_class_is_valuetype (new_method->method->klass)) {
						/* unbox */
						gpointer unboxed = mono_object_unbox_internal (this_arg);
						*this_arg_p = unboxed;
					}

					InterpMethodCodeType code_type = new_method->code_type;

					g_assert (code_type == IMETHOD_CODE_UNKNOWN ||
							code_type == IMETHOD_CODE_INTERP ||
							code_type == IMETHOD_CODE_COMPILED);

					if (G_UNLIKELY (code_type == IMETHOD_CODE_UNKNOWN)) {
						// FIXME push/pop LMF
						MonoMethodSignature *sig = mono_method_signature_internal (new_method->method);
						if (mono_interp_jit_call_supported (new_method->method, sig))
							code_type = IMETHOD_CODE_COMPILED;
						else
							code_type = IMETHOD_CODE_INTERP;
						new_method->code_type = code_type;
					}

					if (code_type == IMETHOD_CODE_COMPILED) {
						error_init_reuse (error);
						do_jit_call (context, frame->retval, (stackval*)((guchar*)frame->stack + params_offset), frame, new_method, FALSE, error);
						if (!is_ok (error)) {
							MonoException *call_ex = interp_error_convert_to_exception (frame, error, ip);
							THROW_EX (call_ex, ip);
						}

						goto exit_frame;
					}
				}

				// Copy the params to their location at the start of the frame
				memmove (frame->stack, (guchar*)frame->stack + params_offset, params_size);
			} else {
				new_method = (InterpMethod*)frame->imethod->data_items [ip [1]];
			}

			if (frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)
				if (MONO_PROFILER_ENABLED (method_tail_call))
					mono_interp_profiler_raise_tail_call (frame, new_method->method);

			if (!new_method->transformed) {
				MonoException *transform_ex = do_transform_method (new_method, frame, context);
				if (transform_ex)
					THROW_EX (transform_ex, ip);
				EXCEPTION_CHECKPOINT;
			}
			/*
			 * It's possible for the caller stack frame to be smaller
			 * than the callee stack frame (at the interp level)
			 */
			context->stack_pointer = (guchar*)frame->stack + new_method->alloca_size;
			if (G_UNLIKELY (context->stack_pointer >= context->stack_end)) {
				context->stack_end = context->stack_real_end;
				THROW_EX (mono_domain_get ()->stack_overflow_ex, ip);
			}

			frame->imethod = new_method;
			ip = frame->imethod->code;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MOV_STACK_UNOPT) {
			int src_offset = ip [1];
			int dst_offset = src_offset + (gint16)ip [2];
			int size = ip [3];

			memmove (locals + dst_offset, locals + src_offset, size);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CALL_DELEGATE) {
			return_offset = ip [1];
			call_args_offset = ip [2];
			MonoDelegate *del = LOCAL_VAR (call_args_offset, MonoDelegate*);
			gboolean is_multicast = del->method == NULL;
			InterpMethod *del_imethod = (InterpMethod*)del->interp_invoke_impl;

			if (!del_imethod) {
				// FIXME push/pop LMF
				if (is_multicast) {
					MonoMethod *invoke = mono_get_delegate_invoke_internal (del->object.vtable->klass);
					del_imethod = mono_interp_get_imethod (mono_marshal_get_delegate_invoke (invoke, del));
					del->interp_invoke_impl = del_imethod;
				} else if (!del->interp_method) {
					// Not created from interpreted code
					g_assert (del->method);
					del_imethod = mono_interp_get_imethod (del->method);
					if (del->target && m_method_is_virtual (del->method))
						del_imethod = get_virtual_method (del_imethod, del->target->vtable);
					del->interp_method = del_imethod;
					del->interp_invoke_impl = del_imethod;
				} else {
					del_imethod = (InterpMethod*)del->interp_method;
					if (del_imethod->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
						del_imethod = mono_interp_get_imethod (mono_marshal_get_native_wrapper (del_imethod->method, FALSE, FALSE));
						del->interp_invoke_impl = del_imethod;
					} else if ((m_method_is_virtual (del_imethod->method) && !m_method_is_static (del_imethod->method)) && !del->target && !m_class_is_valuetype (del_imethod->method->klass)) {
						// 'this' is passed dynamically, we need to recompute the target method
						// with each call
						MonoObject *obj = LOCAL_VAR (call_args_offset + MINT_STACK_SLOT_SIZE, MonoObject*);
						del_imethod = get_virtual_method (del_imethod, obj->vtable);
						if (m_class_is_valuetype (del_imethod->method->klass)) {
							// We are calling into a value type method, `this` needs to be unboxed
							LOCAL_VAR (call_args_offset + MINT_STACK_SLOT_SIZE, gpointer) = mono_object_unbox_internal (obj);
						}
					} else {
						del->interp_invoke_impl = del_imethod;
					}
				}
			}
			if (del_imethod->optimized_imethod) {
				del_imethod = del_imethod->optimized_imethod;
				// don't patch for virtual calls
				if (del->interp_invoke_impl)
					del->interp_invoke_impl = del_imethod;
			}
			cmethod = del_imethod;
			if (!is_multicast) {
				int ref_slot_offset = frame->imethod->ref_slot_offset;
				if (ref_slot_offset >= 0)
					LOCAL_VAR (ref_slot_offset, gpointer) = del;
				int param_count = ip [4];
				if (cmethod->param_count == param_count + 1) {
					// Target method is static but the delegate has a target object. We handle
					// this separately from the case below, because, for these calls, the instance
					// is allowed to be null.
					LOCAL_VAR (call_args_offset, MonoObject*) = del->target;
				} else if (del->target) {
					MonoObject *this_arg = del->target;

					// replace the MonoDelegate* on the stack with 'this' pointer
					if (m_class_is_valuetype (cmethod->method->klass)) {
						gpointer unboxed = mono_object_unbox_internal (this_arg);
						LOCAL_VAR (call_args_offset, gpointer) = unboxed;
					} else {
						LOCAL_VAR (call_args_offset, MonoObject*) = this_arg;
					}
				} else {
					// skip the delegate pointer for static calls
					// FIXME we could avoid memmove
					memmove (locals + call_args_offset, locals + call_args_offset + ip [5], ip [3]);
				}
			}
			ip += 6;

			goto jit_call;
		}
		MINT_IN_CASE(MINT_CALLI) {
			gboolean need_unbox;

			/* In mixed mode, stay in the interpreter for simplicity even if there is an AOT version of the callee */
			cmethod = ftnptr_to_imethod (LOCAL_VAR (ip [2], gpointer), &need_unbox);

			if (cmethod->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
				// FIXME push/pop LMF
				cmethod = mono_interp_get_imethod (mono_marshal_get_native_wrapper (cmethod->method, FALSE, FALSE));
			}

			return_offset = ip [1];
			call_args_offset = ip [3];

			if (need_unbox) {
				MonoObject *this_arg = LOCAL_VAR (call_args_offset, MonoObject*);
				LOCAL_VAR (call_args_offset, gpointer) = mono_object_unbox_internal (this_arg);
			}
			ip += 4;

			goto jit_call;
		}
		MINT_IN_CASE(MINT_CALLI_NAT_FAST) {
			MintICallSig icall_sig = (MintICallSig)ip [4];
			MonoMethodSignature *csignature = (MonoMethodSignature*)frame->imethod->data_items [ip [5]];
			gboolean save_last_error = ip [6];

			stackval *ret = (stackval*)(locals + ip [1]);
			gpointer target_ip = LOCAL_VAR (ip [2], gpointer);
			stackval *args = (stackval*)(locals + ip [3]);
			/* for calls, have ip pointing at the start of next instruction */
			frame->state.ip = ip + 7;

			do_icall_wrapper (frame, csignature, icall_sig, ret, args, target_ip, save_last_error, &gc_transitions);
			EXCEPTION_CHECKPOINT;
			CHECK_RESUME_STATE (context);
			ip += 7;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CALLI_NAT_DYNAMIC) {
			MonoMethodSignature* csignature = (MonoMethodSignature*)frame->imethod->data_items [ip [4]];

			return_offset = ip [1];
			guchar* code = LOCAL_VAR (ip [2], guchar*);
			call_args_offset = ip [3];

			// FIXME push/pop LMF
			cmethod = mono_interp_get_native_func_wrapper (frame->imethod, csignature, code);

			ip += 5;
			goto jit_call;
		}
		MINT_IN_CASE(MINT_CALLI_NAT) {
			MonoMethodSignature *csignature = (MonoMethodSignature*)frame->imethod->data_items [ip [4]];
			InterpMethod *imethod = (InterpMethod*)frame->imethod->data_items [ip [5]];

			guchar *code = LOCAL_VAR (ip [2], guchar*);

			gboolean save_last_error = ip [6];
			gpointer *cache = (gpointer*)&frame->imethod->data_items [ip [7]];
			/* for calls, have ip pointing at the start of next instruction */
			frame->state.ip = ip + 8;
			ves_pinvoke_method (imethod, csignature, (MonoFuncV)code, context, frame, (stackval*)(locals + ip [1]), (stackval*)(locals + ip [3]), save_last_error, cache, &gc_transitions);

			EXCEPTION_CHECKPOINT;
			CHECK_RESUME_STATE (context);

			ip += 8;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CALLVIRT_FAST) {
			MonoObject *this_arg;
			int slot;

			cmethod = (InterpMethod*)frame->imethod->data_items [ip [3]];
			return_offset = ip [1];
			call_args_offset = ip [2];

			this_arg = LOCAL_VAR (call_args_offset, MonoObject*);
			NULL_CHECK (this_arg);

			slot = (gint16)ip [4];
			ip += 5;
			// FIXME push/pop LMF
			cmethod = get_virtual_method_fast (cmethod, this_arg->vtable, slot);
			if (m_class_is_valuetype (cmethod->method->klass)) {
				/* unbox */
				gpointer unboxed = mono_object_unbox_internal (this_arg);
				LOCAL_VAR (call_args_offset, gpointer) = unboxed;
			}

#if HOST_BROWSER
			/* Auto-JIT this resolved VIRTUAL target + invoke its wasm version if compiled, before the
			 * do_jit_call/interp path — so hot virtual methods (the bulk of IKVM's hot calls) get JITted and
			 * run in wasm too, not just direct (MINT_CALL) callees. On a non-wasm-JIT target, fall through to
			 * jit_call (AOT do_jit_call / interp), preserving existing behaviour. */
			wasm_jit_maybe_compile (cmethod);
			if (G_UNLIKELY (cmethod->wasm_jit_slot > 0)) {
				extern void mono_wasm_jit_sync_thread (void);
				extern int mono_wasm_jit_slot_live (int slot);
				mono_wasm_jit_sync_thread ();
				/* Only call the JITted e-thunk if THIS thread actually instantiated it. sync_thread can fail to
				 * instantiate a module on a worker (OOM/CompileError under pressure) while it succeeded on the
				 * compiling thread; the slot then holds a jiterpreter placeholder of a different signature, and
				 * call_indirect-ing it traps + kills the worker. Run in the interpreter (jit_call) instead. */
				if (G_UNLIKELY (!mono_wasm_jit_slot_live (cmethod->wasm_jit_slot)))
					goto jit_call;
				{
					MonoLMFExt ext;
					/* Set the resume IP before the call (see the MINT_CALL path): mono_handle_exception reads
					 * frame->state.ip on THIS frame (reached via the LMF) to match try/catch regions when the
					 * JITted callee's residual throws. A stale ip skips a catch in this method. */
					frame->state.ip = ip;
					interp_push_lmf (&ext, frame);
					{ extern void mono_wasm_jit_invoke_caught (MonoMethod *, gint32, gpointer, gpointer);
					/* catch a C++/wasm-EH unwind escaping the JITted method; it installs the interp resume-state
					 * (pass 1 already did) so CHECK_RESUME_STATE below resumes / re-propagates. */
					mono_wasm_jit_invoke_caught (cmethod->method, (gint32) cmethod->wasm_jit_slot, locals + call_args_offset, locals + return_offset);
					interp_pop_lmf (&ext); }
				}
				{ if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_INVOKE); cmethod->wasm_jit_invoke_in++; wj_edge_bump (frame->imethod, cmethod); }
				  /* Lever A: this interp caller keeps entering jitted islands — queue it for upward JIT. */
				  { extern int mono_wasm_jit_entry_promote; InterpMethod *wjc = frame->imethod;
				    if (G_UNLIKELY (mono_wasm_jit_entry_promote > 0) && wj_slot_hot_retry_eligible (wjc->wasm_jit_slot) && ++wjc->wasm_jit_invoke_out >= mono_wasm_jit_entry_promote) { wjc->wasm_jit_invoke_out = 0; wj_promote_push (wjc->method); } } }
				CHECK_RESUME_STATE (context);
				MINT_IN_BREAK;
			}
#endif
jit_call:
			{
				InterpMethodCodeType code_type = cmethod->code_type;

				g_assert (code_type == IMETHOD_CODE_UNKNOWN ||
						  code_type == IMETHOD_CODE_INTERP ||
						  code_type == IMETHOD_CODE_COMPILED);

				if (G_UNLIKELY (code_type == IMETHOD_CODE_UNKNOWN)) {
					// FIXME push/pop LMF
					MonoMethodSignature *sig = mono_method_signature_internal (cmethod->method);
					if (mono_interp_jit_call_supported (cmethod->method, sig))
						code_type = IMETHOD_CODE_COMPILED;
					else
						code_type = IMETHOD_CODE_INTERP;
					cmethod->code_type = code_type;
				}

				if (code_type == IMETHOD_CODE_INTERP) {

					goto interp_call;

				} else if (code_type == IMETHOD_CODE_COMPILED) {
					frame->state.ip = ip;
					error_init_reuse (error);
					do_jit_call (context, (stackval*)(locals + return_offset), (stackval*)(locals + call_args_offset), frame, cmethod, FALSE, error);
					if (!is_ok (error)) {
						MonoException *call_ex = interp_error_convert_to_exception (frame, error, ip);
						THROW_EX (call_ex, ip);
					}

					CHECK_RESUME_STATE (context);
				}
				MINT_IN_BREAK;
			}
		}
		MINT_IN_CASE(MINT_CALL_VARARG) {
			// Same as MINT_CALL, except at ip [4] we have the index for the csignature,
			// which is required by the called method to set up the arglist.
			cmethod = (InterpMethod*)frame->imethod->data_items [ip [3]];
			return_offset = ip [1];
			call_args_offset = ip [2];
			ip += 6;
			goto jit_call;
		}

		MINT_IN_CASE(MINT_CALL) {
			cmethod = (InterpMethod*)frame->imethod->data_items [ip [3]];
			return_offset = ip [1];
			call_args_offset = ip [2];

#ifdef ENABLE_EXPERIMENT_TIERED
			ip += 5;
#else
			ip += 4;
#endif

#if HOST_BROWSER
			{
				/* If the callee was compiled by the runtime wasm JIT, invoke its entry thunk
				 * e(args_ptr, ret_ptr) via the function-table slot instead of interpreting; the
				 * thunk marshals args from the call-args stackvals and writes the result back. */
					/* hotness trigger: unified with the virtual path — wasm_jit_maybe_compile serializes the
					 * compile (try-lock) + eagerly forms the call-tree island. (Was an inline copy that called
					 * mono_wasm_force_compile directly, bypassing both — the jit83 unserialized-compile hole.) */
					wasm_jit_maybe_compile (cmethod);
								if (G_UNLIKELY (cmethod->wasm_jit_slot > 0)) {
					/* Bring THIS thread's per-thread function table up to date (instantiate any JITted
					 * methods registered since this thread last synced) before invoking, so both this method
					 * and any methods it calls via f-slot call_indirect are present in this thread's table. */
					extern void mono_wasm_jit_sync_thread (void);
					mono_wasm_jit_sync_thread ();
					{ extern int mono_wasm_jit_slot_live (int slot);
					/* Only call the JITted e-thunk if THIS thread actually instantiated it. sync_thread can fail to
					 * instantiate a module on a worker (OOM/CompileError under pressure) while it succeeded on the
					 * compiling thread; the slot then holds a jiterpreter placeholder of a different signature and
					 * call_indirect-ing it traps + kills the worker. Interpret the method instead. */
					if (G_UNLIKELY (!mono_wasm_jit_slot_live (cmethod->wasm_jit_slot)))
						goto interp_call;
					}
					{
						/* Push an LMF across the interp->JITted-wasm call, same as do_jit_call does for compiled
						 * code: it marks the managed->native boundary so GC stack-walking and cooperative/JSPI
						 * suspends triggered inside the JITted method (e.g. its GC safepoint poll) are handled
						 * correctly. Without it a suspend inside JITted code corrupts coop/suspend state. */
						MonoLMFExt ext;
						/* Record our resume IP BEFORE the call, exactly as do_jit_call does (frame->state.ip = ip).
						 * If the JITted callee's residual throws, mono_handle_exception walks via the LMF pushed
						 * below to THIS interp frame and reads frame->state.ip to match the active try/catch regions.
						 * Without this it reads a stale ip (the frame's entry/prologue), the region match fails, and
						 * a catch in THIS method (the JITted method's direct interp caller) is skipped — the
						 * exception escapes past it (the MC RunningOnDifferentThreadException disconnect, and the
						 * EhCatch repro: throw escaped past the inner catch). */
						frame->state.ip = ip;
						interp_push_lmf (&ext, frame);
						{ extern void mono_wasm_jit_invoke_caught (MonoMethod *, gint32, gpointer, gpointer);
						/* catch a C++/wasm-EH unwind escaping the JITted method; it installs the interp resume-state
						 * (pass 1 already did) so CHECK_RESUME_STATE below resumes / re-propagates. */
						mono_wasm_jit_invoke_caught (cmethod->method, (gint32) cmethod->wasm_jit_slot, locals + call_args_offset, locals + return_offset);
						interp_pop_lmf (&ext); }
					}
					{ if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_INVOKE); cmethod->wasm_jit_invoke_in++; wj_edge_bump (frame->imethod, cmethod); }
				  /* Lever A: this interp caller keeps entering jitted islands — queue it for upward JIT. */
				  { extern int mono_wasm_jit_entry_promote; InterpMethod *wjc = frame->imethod;
				    if (G_UNLIKELY (mono_wasm_jit_entry_promote > 0) && wj_slot_hot_retry_eligible (wjc->wasm_jit_slot) && ++wjc->wasm_jit_invoke_out >= mono_wasm_jit_entry_promote) { wjc->wasm_jit_invoke_out = 0; wj_promote_push (wjc->method); } } }
					/* If a residual interp call inside the JITted method threw, interp_entry set the
					 * thread resume-state and the JITted method returned early (a dummy result). Propagate
					 * the exception through the interp's EH, exactly like do_jit_call does after a thrown
					 * call — the LMF pushed above lets mono_handle_exception skip the JITted native frame
					 * to a handler in this frame or above. */
					CHECK_RESUME_STATE (context);
					MINT_IN_BREAK;
				}
			}
#endif

interp_call:
			/*
			 * Make a non-recursive call by loading the new interpreter state based on child frame,
			 * and going back to the main loop.
			 */
			SAVE_INTERP_STATE (frame);

			// Allocate child frame.
			// FIXME: Add stack overflow checks
			{
				InterpFrame *child_frame = frame->next_free;
				if (!child_frame) {
					child_frame = g_newa0 (InterpFrame, 1);
					// Not free currently, but will be when allocation attempted.
					frame->next_free = child_frame;
				}
				reinit_frame (child_frame, frame, cmethod, locals + return_offset, locals + call_args_offset);
				frame = child_frame;
			}
			g_assert_checked (((gsize)frame->stack % MINT_STACK_ALIGNMENT) == 0);

			MonoException *call_ex;
			if (method_entry (context, frame,
#if DEBUG_INTERP
				&tracing,
#endif
				&call_ex)) {
				if (call_ex)
					THROW_EX (call_ex, NULL);
				EXCEPTION_CHECKPOINT;
				CHECK_RESUME_STATE (context);
			}

			context->stack_pointer = (guchar*)frame->stack + cmethod->alloca_size;

			if (G_UNLIKELY (context->stack_pointer >= context->stack_end)) {
				context->stack_end = context->stack_real_end;
				THROW_EX (mono_domain_get ()->stack_overflow_ex, ip);
			}

			/* Make sure the stack pointer is bumped before we store any references on the stack */
			mono_compiler_barrier ();

			INIT_INTERP_STATE (frame, NULL);

			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_JIT_CALL) {
			InterpMethod *rmethod = (InterpMethod*)frame->imethod->data_items [ip [3]];
			error_init_reuse (error);
			/* for calls, have ip pointing at the start of next instruction */
			frame->state.ip = ip + 4;
			do_jit_call (context, (stackval*)(locals + ip [1]), (stackval*)(locals + ip [2]), frame, rmethod, FALSE, error);
			if (!is_ok (error)) {
				MonoException *call_ex = interp_error_convert_to_exception (frame, error, ip);
				THROW_EX (call_ex, ip);
			}

			CHECK_RESUME_STATE (context);
			ip += 4;

			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_JIT_CALL2) {
#ifdef ENABLE_EXPERIMENT_TIERED
			InterpMethod *rmethod = (InterpMethod *) READ64 (ip + 2);

			error_init_reuse (error);

			frame->state.ip = ip + 6;
			do_jit_call (context, (stackval*)(locals + ip [1]), frame, rmethod, error);
			if (!is_ok (error)) {
				MonoException *call_ex = interp_error_convert_to_exception (frame, error);
				THROW_EX (call_ex, ip);
			}

			CHECK_RESUME_STATE (context);

			ip += 6;
#else
			g_error ("MINT_JIT_ICALL2 shouldn't be used");
#endif
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_RET)
			frame->retval [0] = LOCAL_VAR (ip [1], stackval);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_I1)
			frame->retval [0].data.i = (gint8) LOCAL_VAR (ip [1], gint32);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_U1)
			frame->retval [0].data.i = (guint8) LOCAL_VAR (ip [1], gint32);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_I2)
			frame->retval [0].data.i = (gint16) LOCAL_VAR (ip [1], gint32);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_U2)
			frame->retval [0].data.i = (guint16) LOCAL_VAR (ip [1], gint32);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_I4_IMM)
			frame->retval [0].data.i = (gint16)ip [1];
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_I8_IMM)
			frame->retval [0].data.l = (gint16)ip [1];
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_VOID)
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_VT) {
			memmove (frame->retval, locals + ip [1], ip [2]);
			goto exit_frame;
		}
		MINT_IN_CASE(MINT_RET_LOCALLOC)
			frame->retval [0] = LOCAL_VAR (ip [1], stackval);
			frame_data_allocator_pop (&context->data_stack, frame);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_VOID_LOCALLOC)
			frame_data_allocator_pop (&context->data_stack, frame);
			goto exit_frame;
		MINT_IN_CASE(MINT_RET_VT_LOCALLOC) {
			memmove (frame->retval, locals + ip [1], ip [2]);
			frame_data_allocator_pop (&context->data_stack, frame);
			goto exit_frame;
		}

#ifdef ENABLE_EXPERIMENT_TIERED
#define BACK_BRANCH_PROFILE(offset) do { \
		if (offset < 0) \
			mini_tiered_inc (frame->imethod->method, &frame->imethod->tiered_counter, 0); \
	} while (0);
#else
#define BACK_BRANCH_PROFILE(offset)
#endif

		MINT_IN_CASE(MINT_BR_S) {
			short br_offset = (short) *(ip + 1);
			BACK_BRANCH_PROFILE (br_offset);
			ip += br_offset;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BR) {
			gint32 br_offset = (gint32) READ32(ip + 1);
			BACK_BRANCH_PROFILE (br_offset);
			ip += br_offset;
			MINT_IN_BREAK;
		}

#define ZEROP_S(datatype, op) \
	if (LOCAL_VAR (ip [1], datatype) op 0) { \
		gint16 br_offset = (gint16) ip [2]; \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 3;

#define ZEROP(datatype, op) \
	if (LOCAL_VAR (ip [1], datatype) op 0) { \
		gint32 br_offset = (gint32)READ32(ip + 2); \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 4;

		MINT_IN_CASE(MINT_BRFALSE_I4_S)
			ZEROP_S(gint32, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRFALSE_I8_S)
			ZEROP_S(gint64, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRFALSE_I4)
			ZEROP(gint32, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRFALSE_I8)
			ZEROP(gint64, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRTRUE_I4_S)
			ZEROP_S(gint32, !=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRTRUE_I8_S)
			ZEROP_S(gint64, !=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRTRUE_I4)
			ZEROP(gint32, !=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BRTRUE_I8)
			ZEROP(gint64, !=);
			MINT_IN_BREAK;
#define CONDBR_S(cond) \
	if (cond) { \
		gint16 br_offset = (gint16) ip [3]; \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 4;
#define BRELOP_S(datatype, op) \
	CONDBR_S(LOCAL_VAR (ip [1], datatype) op LOCAL_VAR (ip [2], datatype))

#define CONDBR(cond) \
	if (cond) { \
		gint32 br_offset = (gint32) READ32 (ip + 3); \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 5;

#define BRELOP(datatype, op) \
	CONDBR(LOCAL_VAR (ip [1], datatype) op LOCAL_VAR (ip [2], datatype))

		MINT_IN_CASE(MINT_BEQ_I4_S)
			BRELOP_S(gint32, ==)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_I8_S)
			BRELOP_S(gint64, ==)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(!isunordered (f1, f2) && f1 == f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BEQ_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(!mono_isunordered (d1, d2) && d1 == d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BEQ_I4)
			BRELOP(gint32, ==)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_I8)
			BRELOP(gint64, ==)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(!isunordered (f1, f2) && f1 == f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BEQ_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(!mono_isunordered (d1, d2) && d1 == d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_I4_S)
			BRELOP_S(gint32, >=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I8_S)
			BRELOP_S(gint64, >=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(!isunordered (f1, f2) && f1 >= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(!mono_isunordered (d1, d2) && d1 >= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_I4)
			BRELOP(gint32, >=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I8)
			BRELOP(gint64, >=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(!isunordered (f1, f2) && f1 >= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(!mono_isunordered (d1, d2) && d1 >= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_I4_S)
			BRELOP_S(gint32, >)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I8_S)
			BRELOP_S(gint64, >)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(!isunordered (f1, f2) && f1 > f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(!mono_isunordered (d1, d2) && d1 > d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_I4)
			BRELOP(gint32, >)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I8)
			BRELOP(gint64, >)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(!isunordered (f1, f2) && f1 > f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(!mono_isunordered (d1, d2) && d1 > d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_I4_S)
			BRELOP_S(gint32, <)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I8_S)
			BRELOP_S(gint64, <)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(!isunordered (f1, f2) && f1 < f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(!mono_isunordered (d1, d2) && d1 < d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_I4)
			BRELOP(gint32, <)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I8)
			BRELOP(gint64, <)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(!isunordered (f1, f2) && f1 < f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(!mono_isunordered (d1, d2) && d1 < d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_I4_S)
			BRELOP_S(gint32, <=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I8_S)
			BRELOP_S(gint64, <=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(!isunordered (f1, f2) && f1 <= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(!mono_isunordered (d1, d2) && d1 <= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_I4)
			BRELOP(gint32, <=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I8)
			BRELOP(gint64, <=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(!isunordered (f1, f2) && f1 <= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(!mono_isunordered (d1, d2) && d1 <= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BNE_UN_I4_S)
			BRELOP_S(gint32, !=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_I8_S)
			BRELOP_S(gint64, !=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(isunordered (f1, f2) || f1 != f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BNE_UN_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(mono_isunordered (d1, d2) || d1 != d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BNE_UN_I4)
			BRELOP(gint32, !=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_I8)
			BRELOP(gint64, !=)
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(isunordered (f1, f2) || f1 != f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BNE_UN_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(mono_isunordered (d1, d2) || d1 != d2)
			MINT_IN_BREAK;
		}

#define BRELOP_S_CAST(datatype, op) \
	if (LOCAL_VAR (ip [1], datatype) op LOCAL_VAR (ip [2], datatype)) { \
		gint16 br_offset = (gint16) ip [3]; \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 4;

#define BRELOP_CAST(datatype, op) \
	if (LOCAL_VAR (ip [1], datatype) op LOCAL_VAR (ip [2], datatype)) { \
		gint32 br_offset = (gint32)READ32(ip + 3); \
		BACK_BRANCH_PROFILE (br_offset); \
		ip += br_offset; \
	} else \
		ip += 5;

		MINT_IN_CASE(MINT_BGE_UN_I4_S)
			BRELOP_S_CAST(guint32, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I8_S)
			BRELOP_S_CAST(guint64, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(isunordered (f1, f2) || f1 >= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_UN_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(mono_isunordered (d1, d2) || d1 >= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_UN_I4)
			BRELOP_CAST(guint32, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I8)
			BRELOP_CAST(guint64, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(isunordered (f1, f2) || f1 >= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGE_UN_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(mono_isunordered (d1, d2) || d1 >= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_UN_I4_S)
			BRELOP_S_CAST(guint32, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I8_S)
			BRELOP_S_CAST(guint64, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(isunordered (f1, f2) || f1 > f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_UN_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(mono_isunordered (d1, d2) || d1 > d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_UN_I4)
			BRELOP_CAST(guint32, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I8)
			BRELOP_CAST(guint64, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(isunordered (f1, f2) || f1 > f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BGT_UN_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(mono_isunordered (d1, d2) || d1 > d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_UN_I4_S)
			BRELOP_S_CAST(guint32, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I8_S)
			BRELOP_S_CAST(guint64, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(isunordered (f1, f2) || f1 <= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_UN_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(mono_isunordered (d1, d2) || d1 <= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_UN_I4)
			BRELOP_CAST(guint32, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I8)
			BRELOP_CAST(guint64, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(isunordered (f1, f2) || f1 <= f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLE_UN_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(mono_isunordered (d1, d2) || d1 <= d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_UN_I4_S)
			BRELOP_S_CAST(guint32, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I8_S)
			BRELOP_S_CAST(guint64, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_R4_S) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR_S(isunordered (f1, f2) || f1 < f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_UN_R8_S) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR_S(mono_isunordered (d1, d2) || d1 < d2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_UN_I4)
			BRELOP_CAST(guint32, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I8)
			BRELOP_CAST(guint64, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_R4) {
			float f1 = LOCAL_VAR (ip [1], float);
			float f2 = LOCAL_VAR (ip [2], float);
			CONDBR(isunordered (f1, f2) || f1 < f2)
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BLT_UN_R8) {
			double d1 = LOCAL_VAR (ip [1], double);
			double d2 = LOCAL_VAR (ip [2], double);
			CONDBR(mono_isunordered (d1, d2) || d1 < d2)
			MINT_IN_BREAK;
		}

#define ZEROP_SP(datatype, op) \
	if (LOCAL_VAR (ip [1], datatype) op 0) { \
		gint16 br_offset = (gint16) ip [2]; \
		BACK_BRANCH_PROFILE (br_offset); \
		SAFEPOINT; \
		ip += br_offset; \
	} else \
		ip += 3;

MINT_IN_CASE(MINT_BRFALSE_I4_SP) ZEROP_SP(gint32, ==); MINT_IN_BREAK;
MINT_IN_CASE(MINT_BRFALSE_I8_SP) ZEROP_SP(gint64, ==); MINT_IN_BREAK;
MINT_IN_CASE(MINT_BRTRUE_I4_SP) ZEROP_SP(gint32, !=); MINT_IN_BREAK;
MINT_IN_CASE(MINT_BRTRUE_I8_SP) ZEROP_SP(gint64, !=); MINT_IN_BREAK;

#define CONDBR_SP(cond) \
	if (cond) { \
		gint16 br_offset = (gint16) ip [3]; \
		BACK_BRANCH_PROFILE (br_offset); \
		SAFEPOINT; \
		ip += br_offset; \
	} else \
		ip += 4;
#define BRELOP_SP(datatype, op) \
	CONDBR_SP(LOCAL_VAR (ip [1], datatype) op LOCAL_VAR (ip [2], datatype))

		MINT_IN_CASE(MINT_BEQ_I4_SP) BRELOP_SP(gint32, ==); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_I8_SP) BRELOP_SP(gint64, ==); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I4_SP) BRELOP_SP(gint32, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I8_SP) BRELOP_SP(gint64, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I4_SP) BRELOP_SP(gint32, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I8_SP) BRELOP_SP(gint64, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I4_SP) BRELOP_SP(gint32, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I8_SP) BRELOP_SP(gint64, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I4_SP) BRELOP_SP(gint32, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I8_SP) BRELOP_SP(gint64, <=); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_BNE_UN_I4_SP) BRELOP_SP(guint32, !=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_I8_SP) BRELOP_SP(guint64, !=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I4_SP) BRELOP_SP(guint32, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I8_SP) BRELOP_SP(guint64, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I4_SP) BRELOP_SP(guint32, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I8_SP) BRELOP_SP(guint64, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I4_SP) BRELOP_SP(guint32, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I8_SP) BRELOP_SP(guint64, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I4_SP) BRELOP_SP(guint32, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I8_SP) BRELOP_SP(guint64, <); MINT_IN_BREAK;

#define BRELOP_IMM_SP(datatype, op) \
	CONDBR_SP(LOCAL_VAR (ip [1], datatype) op (datatype)(gint16)ip [2])

		MINT_IN_CASE(MINT_BEQ_I4_IMM_SP) BRELOP_IMM_SP(gint32, ==); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BEQ_I8_IMM_SP) BRELOP_IMM_SP(gint64, ==); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I4_IMM_SP) BRELOP_IMM_SP(gint32, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_I8_IMM_SP) BRELOP_IMM_SP(gint64, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I4_IMM_SP) BRELOP_IMM_SP(gint32, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_I8_IMM_SP) BRELOP_IMM_SP(gint64, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I4_IMM_SP) BRELOP_IMM_SP(gint32, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_I8_IMM_SP) BRELOP_IMM_SP(gint64, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I4_IMM_SP) BRELOP_IMM_SP(gint32, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_I8_IMM_SP) BRELOP_IMM_SP(gint64, <=); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_BNE_UN_I4_IMM_SP) BRELOP_IMM_SP(guint32, !=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BNE_UN_I8_IMM_SP) BRELOP_IMM_SP(guint64, !=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I4_IMM_SP) BRELOP_IMM_SP(guint32, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGE_UN_I8_IMM_SP) BRELOP_IMM_SP(guint64, >=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I4_IMM_SP) BRELOP_IMM_SP(guint32, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BGT_UN_I8_IMM_SP) BRELOP_IMM_SP(guint64, >); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I4_IMM_SP) BRELOP_IMM_SP(guint32, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLE_UN_I8_IMM_SP) BRELOP_IMM_SP(guint64, <=); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I4_IMM_SP) BRELOP_IMM_SP(guint32, <); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_BLT_UN_I8_IMM_SP) BRELOP_IMM_SP(guint64, <); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_SWITCH) {
			guint32 val = LOCAL_VAR (ip [1], guint32);
			guint32 n = READ32 (ip + 2);
			ip += 4;
			if (val < n) {
				ip += 2 * val;
				int offset = READ32 (ip);
				ip += offset;
			} else {
				ip += 2 * n;
			}
			MINT_IN_BREAK;
		}
#define LDIND(datatype,casttype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [2], gpointer); \
	NULL_CHECK (ptr); \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (locals + ip [1], ptr, sizeof (datatype)); \
	else \
		LOCAL_VAR (ip [1], datatype) = *(casttype*)ptr; \
	ip += 3; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_LDIND_I1)
			LDIND(int, gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_U1)
			LDIND(int, guint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_I2)
			LDIND(int, gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_U2)
			LDIND(int, guint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_I4) {
			LDIND(int, gint32, FALSE);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDIND_I8)
#ifdef NO_UNALIGNED_ACCESS
			LDIND(gint64, gint64, TRUE);
#else
			LDIND(gint64, gint64, FALSE);
#endif
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_R4)
			LDIND(float, gfloat, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_R8)
#ifdef NO_UNALIGNED_ACCESS
			LDIND(double, gdouble, TRUE);
#else
			LDIND(double, gdouble, FALSE);
#endif
			MINT_IN_BREAK;

#define LDIND_OFFSET(datatype,casttype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [2], gpointer); \
	NULL_CHECK (ptr); \
	ptr = (char*)ptr + LOCAL_VAR (ip [3], mono_i); \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (locals + ip [1], ptr, sizeof (datatype)); \
	else \
		LOCAL_VAR (ip [1], datatype) = *(casttype*)ptr; \
	ip += 4; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_LDIND_OFFSET_I1)
			LDIND_OFFSET(int, gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_U1)
			LDIND_OFFSET(int, guint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_I2)
			LDIND_OFFSET(int, gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_U2)
			LDIND_OFFSET(int, guint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_I4)
			LDIND_OFFSET(int, gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_I8)
#ifdef NO_UNALIGNED_ACCESS
			LDIND_OFFSET(gint64, gint64, TRUE);
#else
			LDIND_OFFSET(gint64, gint64, FALSE);
#endif
			MINT_IN_BREAK;

#define LDIND_OFFSET_ADD_MUL(datatype,casttype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [2], gpointer); \
	NULL_CHECK (ptr); \
	ptr = (char*)ptr + (LOCAL_VAR (ip [3], mono_i) + (gint16)ip [4]) * (gint16)ip [5]; \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (locals + ip [1], ptr, sizeof (datatype)); \
	else \
		LOCAL_VAR (ip [1], datatype) = *(casttype*)ptr; \
	ip += 6; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_I1)
			LDIND_OFFSET_ADD_MUL(gint32, gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_U1)
			LDIND_OFFSET_ADD_MUL(gint32, guint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_I2)
			LDIND_OFFSET_ADD_MUL(gint32, gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_U2)
			LDIND_OFFSET_ADD_MUL(gint32, guint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_I4)
			LDIND_OFFSET_ADD_MUL(gint32, gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_ADD_MUL_IMM_I8)
#ifdef NO_UNALIGNED_ACCESS
			LDIND_OFFSET_ADD_MUL(gint64, gint64, TRUE);
#else
			LDIND_OFFSET_ADD_MUL(gint64, gint64, FALSE);
#endif
			MINT_IN_BREAK;

#define LDIND_OFFSET_IMM(datatype,casttype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [2], gpointer); \
	NULL_CHECK (ptr); \
	ptr = (char*)ptr + (gint16)ip [3]; \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (locals + ip [1], ptr, sizeof (datatype)); \
	else \
		LOCAL_VAR (ip [1], datatype) = *(casttype*)ptr; \
	ip += 4; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_I1)
			LDIND_OFFSET_IMM(int, gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_U1)
			LDIND_OFFSET_IMM(int, guint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_I2)
			LDIND_OFFSET_IMM(int, gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_U2)
			LDIND_OFFSET_IMM(int, guint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_I4)
			LDIND_OFFSET_IMM(int, gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDIND_OFFSET_IMM_I8)
#ifdef NO_UNALIGNED_ACCESS
			LDIND_OFFSET_IMM(gint64, gint64, TRUE);
#else
			LDIND_OFFSET_IMM(gint64, gint64, FALSE);
#endif
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_REF) {
			gpointer ptr = LOCAL_VAR (ip [1], gpointer);
			NULL_CHECK (ptr);
			mono_gc_wbarrier_generic_store_internal (ptr, LOCAL_VAR (ip [2], MonoObject*));
			ip += 3;
			MINT_IN_BREAK;
		}
#define STIND(datatype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [1], gpointer); \
	NULL_CHECK (ptr); \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (ptr, locals + ip [2], sizeof (datatype)); \
	else \
		*(datatype*)ptr = LOCAL_VAR (ip [2], datatype); \
	ip += 3; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_STIND_I1)
			STIND(gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_I2)
			STIND(gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_I4)
			STIND(gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_I8)
#ifdef NO_UNALIGNED_ACCESS
			STIND(gint64, TRUE);
#else
			STIND(gint64, FALSE);
#endif
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_R4)
			STIND(float, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_R8)
#ifdef NO_UNALIGNED_ACCESS
			STIND(double, TRUE);
#else
			STIND(double, FALSE);
#endif
			MINT_IN_BREAK;

#define STIND_OFFSET(datatype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [1], gpointer); \
	NULL_CHECK (ptr); \
	ptr = (char*)ptr + LOCAL_VAR (ip [2], mono_i); \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (ptr, locals + ip [3], sizeof (datatype)); \
	else \
		*(datatype*)ptr = LOCAL_VAR (ip [3], datatype); \
	ip += 4; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_STIND_OFFSET_I1)
			STIND_OFFSET(gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_I2)
			STIND_OFFSET(gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_I4)
			STIND_OFFSET(gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_I8)
#ifdef NO_UNALIGNED_ACCESS
			STIND_OFFSET(gint64, TRUE);
#else
			STIND_OFFSET(gint64, FALSE);
#endif
			MINT_IN_BREAK;

#define STIND_OFFSET_IMM(datatype,unaligned) do { \
	MONO_DISABLE_WARNING(4127) \
	gpointer ptr = LOCAL_VAR (ip [1], gpointer); \
	NULL_CHECK (ptr); \
	ptr = (char*)ptr + (gint16)ip [3]; \
	if (unaligned && ((gsize)ptr % SIZEOF_VOID_P)) \
		memcpy (ptr, locals + ip [2], sizeof (datatype)); \
	else \
		*(datatype*)ptr = LOCAL_VAR (ip [2], datatype); \
	ip += 4; \
	MONO_RESTORE_WARNING \
} while (0)
		MINT_IN_CASE(MINT_STIND_OFFSET_IMM_I1)
			STIND_OFFSET_IMM(gint8, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_IMM_I2)
			STIND_OFFSET_IMM(gint16, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_IMM_I4)
			STIND_OFFSET_IMM(gint32, FALSE);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STIND_OFFSET_IMM_I8)
#ifdef NO_UNALIGNED_ACCESS
			STIND_OFFSET_IMM(gint64, TRUE);
#else
			STIND_OFFSET_IMM(gint64, FALSE);
#endif
			MINT_IN_BREAK;
#define BINOP(datatype, op) \
	LOCAL_VAR (ip [1], datatype) = LOCAL_VAR (ip [2], datatype) op LOCAL_VAR (ip [3], datatype); \
	ip += 4;
		MINT_IN_CASE(MINT_ADD_I4)
			BINOP(gint32, +);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_I8)
			BINOP(gint64, +);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_R4)
			BINOP(float, +);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_R8)
			BINOP(double, +);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD1_I4)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) + 1;
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) + (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_I4_IMM2)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) + (gint32)READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD1_I8)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) + 1;
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_I8_IMM)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) + (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_I8_IMM2)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) + (gint32)READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB_I4)
			BINOP(gint32, -);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB_I8)
			BINOP(gint64, -);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB_R4)
			BINOP(float, -);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB_R8)
			BINOP(double, -);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB1_I4)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) - 1;
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SUB1_I8)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) - 1;
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I4)
			BINOP(gint32, *);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I8)
			BINOP(gint64, *);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) * (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I4_IMM2)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) * (gint32)READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I8_IMM)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) * (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_I8_IMM2)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) * (gint32)READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_MUL_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = (LOCAL_VAR (ip [2], gint32) + (gint16)ip [3]) * (gint16)ip [4];
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ADD_MUL_I8_IMM)
			LOCAL_VAR (ip [1], gint64) = (LOCAL_VAR (ip [2], gint64) + (gint16)ip [3]) * (gint16)ip [4];
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_R4)
			BINOP(float, *);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MUL_R8)
			BINOP(double, *);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_DIV_I4) {
			gint32 i1 = LOCAL_VAR (ip [2], gint32);
			gint32 i2 = LOCAL_VAR (ip [3], gint32);
			if (i2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			if (i2 == (-1) && i1 == G_MININT32)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = i1 / i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_DIV_I8) {
			gint64 l1 = LOCAL_VAR (ip [2], gint64);
			gint64 l2 = LOCAL_VAR (ip [3], gint64);
			if (l2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			if (l2 == (-1) && l1 == G_MININT64)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 / l2;
			ip += 4;
			MINT_IN_BREAK;
			}
		MINT_IN_CASE(MINT_DIV_R4)
			BINOP(float, /);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_DIV_R8)
			BINOP(double, /);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_DIV_UN_I4) {
			guint32 i2 = LOCAL_VAR (ip [3], guint32);
			if (i2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			LOCAL_VAR (ip [1], guint32) = LOCAL_VAR (ip [2], guint32) / i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_DIV_UN_I8) {
			guint64 l2 = LOCAL_VAR (ip [3], guint64);
			if (l2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64) / l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REM_I4) {
			gint32 i1 = LOCAL_VAR (ip [2], gint32);
			gint32 i2 = LOCAL_VAR (ip [3], gint32);
			if (i2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			if (i2 == (-1) && i1 == G_MININT32)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = i1 % i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REM_I8) {
			gint64 l1 = LOCAL_VAR (ip [2], gint64);
			gint64 l2 = LOCAL_VAR (ip [3], gint64);
			if (l2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			if (l2 == (-1) && l1 == G_MININT64)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 % l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REM_R4)
			LOCAL_VAR (ip [1], float) = fmodf (LOCAL_VAR (ip [2], float), LOCAL_VAR (ip [3], float));
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_REM_R8)
			LOCAL_VAR (ip [1], double) = fmod (LOCAL_VAR (ip [2], double), LOCAL_VAR (ip [3], double));
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_REM_UN_I4) {
			guint32 i2 = LOCAL_VAR (ip [3], guint32);
			if (i2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			LOCAL_VAR (ip [1], guint32) = LOCAL_VAR (ip [2], guint32) % i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REM_UN_I8) {
			guint64 l2 = LOCAL_VAR (ip [3], guint64);
			if (l2 == 0)
				THROW_EX (interp_get_exception_divide_by_zero (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64) % l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_AND_I4)
			BINOP(gint32, &);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_AND_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) & (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_AND_I4_IMM2)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) & READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_AND_I8)
			BINOP(gint64, &);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_OR_I4)
			BINOP(gint32, |);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_OR_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) | (gint16)ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_OR_I4_IMM2)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) | READ32 (ip + 3);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_OR_I8)
			BINOP(gint64, |);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_XOR_I4)
			BINOP(gint32, ^);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_XOR_I8)
			BINOP(gint64, ^);
			MINT_IN_BREAK;

#define SHIFTOP(datatype, op) \
	LOCAL_VAR (ip [1], datatype) = LOCAL_VAR (ip [2], datatype) op LOCAL_VAR (ip [3], gint32); \
	ip += 4;

		MINT_IN_CASE(MINT_SHL_I4)
			SHIFTOP(gint32, <<);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHL_I8)
			SHIFTOP(gint64, <<);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_I4)
			SHIFTOP(gint32, >>);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_I8)
			SHIFTOP(gint64, >>);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_UN_I4)
			SHIFTOP(guint32, >>);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_UN_I8)
			SHIFTOP(guint64, >>);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHL_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) << ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHL_I8_IMM)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) << ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_I4_IMM)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) >> ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_I8_IMM)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) >> ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_UN_I4_IMM)
			LOCAL_VAR (ip [1], guint32) = LOCAL_VAR (ip [2], guint32) >> ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHR_UN_I8_IMM)
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64) >> ip [3];
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHL_AND_I4)
			LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], gint32) << (LOCAL_VAR (ip [3], gint32) & 31);
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SHL_AND_I8)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint64) << (LOCAL_VAR (ip [3], gint64) & 63);
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NEG_I4)
			LOCAL_VAR (ip [1], gint32) = - LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NEG_I8)
			LOCAL_VAR (ip [1], gint64) = - LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NEG_R4)
			LOCAL_VAR (ip [1], float) = - LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NEG_R8)
			LOCAL_VAR (ip [1], double) = - LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NOT_I4)
			LOCAL_VAR (ip [1], gint32) = ~ LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_NOT_I8)
			LOCAL_VAR (ip [1], gint64) = ~ LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I1_I4)
			// FIXME read casted var directly and remove redundant conv opcodes
			LOCAL_VAR (ip [1], gint32) = (gint8)LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I1_I8)
			LOCAL_VAR (ip [1], gint32) = (gint8)LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I1_R4)
			LOCAL_VAR (ip [1], gint32) = (gint8) (gint32) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I1_R8)
			/* without gint32 cast, C compiler is allowed to use undefined
			 * behaviour if data.f is bigger than >255. See conv.fpint section
			 * in C standard:
			 * > The conversion truncates; that is, the fractional  part
			 * > is discarded.  The behavior is undefined if the truncated
			 * > value cannot be represented in the destination type.
			 * */
			LOCAL_VAR (ip [1], gint32) = (gint8) (gint32) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U1_I4)
			LOCAL_VAR (ip [1], gint32) = (guint8) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U1_I8)
			LOCAL_VAR (ip [1], gint32) = (guint8) LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U1_R4)
			LOCAL_VAR (ip [1], gint32) = (guint8) (guint32) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U1_R8)
			LOCAL_VAR (ip [1], gint32) = (guint8) (guint32) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I2_I4)
			LOCAL_VAR (ip [1], gint32) = (gint16) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I2_I8)
			LOCAL_VAR (ip [1], gint32) = (gint16) LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I2_R4)
			LOCAL_VAR (ip [1], gint32) = (gint16) (gint32) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I2_R8)
			LOCAL_VAR (ip [1], gint32) = (gint16) (gint32) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U2_I4)
			LOCAL_VAR (ip [1], gint32) = (guint16) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U2_I8)
			LOCAL_VAR (ip [1], gint32) = (guint16) LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U2_R4)
			LOCAL_VAR (ip [1], gint32) = (guint16) (guint32) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U2_R8)
			LOCAL_VAR (ip [1], gint32) = (guint16) (guint32) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I4_R4)
			LOCAL_VAR (ip [1], gint32) = (gint32) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I4_R8)
			LOCAL_VAR (ip [1], gint32) = (gint32) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U4_R4)
#ifdef MONO_ARCH_EMULATE_FCONV_TO_U4
			LOCAL_VAR (ip [1], gint32) = mono_rconv_u4 (LOCAL_VAR (ip [2], float));
#else
			LOCAL_VAR (ip [1], gint32) = (guint32) LOCAL_VAR (ip [2], float);
#endif
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U4_R8)
#ifdef MONO_ARCH_EMULATE_FCONV_TO_U4
			LOCAL_VAR (ip [1], gint32) = mono_fconv_u4 (LOCAL_VAR (ip [2], double));
#else
			LOCAL_VAR (ip [1], gint32) = (guint32) LOCAL_VAR (ip [2], double);
#endif
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I8_I4)
			LOCAL_VAR (ip [1], gint64) = LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I8_U4)
			LOCAL_VAR (ip [1], gint64) = (guint32) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I8_R4)
			LOCAL_VAR (ip [1], gint64) = (gint64) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_I8_R8)
			LOCAL_VAR (ip [1], gint64) = (gint64) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R4_I4)
			LOCAL_VAR (ip [1], float) = (float) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R4_I8)
			LOCAL_VAR (ip [1], float) = (float) LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R4_R8)
			LOCAL_VAR (ip [1], float) = (float) LOCAL_VAR (ip [2], double);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R8_I4)
			LOCAL_VAR (ip [1], double) = (double) LOCAL_VAR (ip [2], gint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R8_I8)
			LOCAL_VAR (ip [1], double) = (double) LOCAL_VAR (ip [2], gint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R8_R4)
			LOCAL_VAR (ip [1], double) = (double) LOCAL_VAR (ip [2], float);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U8_R4)
#ifdef MONO_ARCH_EMULATE_FCONV_TO_U8
			LOCAL_VAR (ip [1], gint64) = mono_rconv_u8 (LOCAL_VAR (ip [2], float));
#else
			LOCAL_VAR (ip [1], gint64) = (guint64) LOCAL_VAR (ip [2], float);
#endif
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_U8_R8)
#ifdef MONO_ARCH_EMULATE_FCONV_TO_U8
			LOCAL_VAR (ip [1], gint64) = mono_fconv_u8 (LOCAL_VAR (ip [2], double));
#else
			LOCAL_VAR (ip [1], gint64) = (guint64) LOCAL_VAR (ip [2], double);
#endif
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CPOBJ) {
			MonoClass* const c = (MonoClass*)frame->imethod->data_items[ip [3]];
			g_assert (m_class_is_valuetype (c));
			/* if this assertion fails, we need to add a write barrier */
			g_assert (!MONO_TYPE_IS_REFERENCE (m_class_get_byval_arg (c)));
			stackval_from_data (m_class_get_byval_arg (c), (stackval*)LOCAL_VAR (ip [1], gpointer), LOCAL_VAR (ip [2], gpointer), FALSE);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CPOBJ_VT) {
			MonoClass* const c = (MonoClass*)frame->imethod->data_items[ip [3]];
			mono_value_copy_internal (LOCAL_VAR (ip [1], gpointer), LOCAL_VAR (ip [2], gpointer), c);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CPOBJ_VT_NOREF) {
			gpointer src_addr = LOCAL_VAR (ip [2], gpointer);
			NULL_CHECK (src_addr);
			memcpy (LOCAL_VAR (ip [1], gpointer), src_addr, ip [3]);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDOBJ_VT) {
			guint16 size = ip [3];
			gpointer srcAddr = LOCAL_VAR (ip [2], gpointer);
			NULL_CHECK (srcAddr);
			memcpy (locals + ip [1], srcAddr, size);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDSTR)
			LOCAL_VAR (ip [1], gpointer) = frame->imethod->data_items [ip [2]];
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSTR_DYNAMIC) {
			MonoString *s = NULL;
			guint32 strtoken = (guint32)(gsize)frame->imethod->data_items [ip [2]];

			MonoMethod *method = frame->imethod->method;
			g_assert (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD);
			s = (MonoString*)mono_method_get_wrapper_data (method, strtoken);
			LOCAL_VAR (ip [1], gpointer) = s;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDSTR_CSTR) {
			MonoString *s = NULL;
			const char* cstr = (const char*)frame->imethod->data_items [ip [2]];

			// FIXME push/pop LMF
			s = mono_string_new_wrapper_internal (cstr);
			LOCAL_VAR (ip [1], gpointer) = s;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_NEWOBJ_ARRAY) {
			MonoClass *newobj_class;
			guint32 token = ip [3];
			guint16 param_count = ip [4];

			newobj_class = (MonoClass*) frame->imethod->data_items [token];

			// FIXME push/pop LMF
			LOCAL_VAR (ip [1], MonoObject*) = ves_array_create (newobj_class, param_count, (stackval*)(locals + ip [2]), error);
			if (!is_ok (error))
				THROW_EX (interp_error_convert_to_exception (frame, error, ip), ip);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_NEWOBJ_STRING) {
			cmethod = (InterpMethod*)frame->imethod->data_items [ip [3]];
			return_offset = ip [1];
			call_args_offset = ip [2];

			// `this` is implicit null. The created string will be returned
			// by the call, even though the call has void return (?!).
			LOCAL_VAR (call_args_offset, gpointer) = NULL;
			ip += 4;
			goto jit_call;
		}
		MINT_IN_CASE(MINT_NEWOBJ_STRING_UNOPT) {
			// Same as MINT_NEWOBJ_STRING but copy params into right place on stack
			cmethod = (InterpMethod*)frame->imethod->data_items [ip [2]];
			return_offset = ip [1];
			call_args_offset = ip [1];
			int aligned_call_args_offset = ALIGN_TO (call_args_offset, MINT_STACK_ALIGNMENT);

			int param_size = ip [3];
                        if (param_size)
                                memmove (locals + aligned_call_args_offset + MINT_STACK_SLOT_SIZE, locals + call_args_offset, param_size);
			call_args_offset = aligned_call_args_offset;
			LOCAL_VAR (call_args_offset, gpointer) = NULL;
			ip += 4;
			goto jit_call;
		}
		MINT_IN_CASE(MINT_NEWOBJ) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [4]];
			/* If the cached vtable was allocated with fewer slots than the
			 * class currently has (TypeBuilder shell vtable, finalized later
			 * to a larger vtable_size), refetch from the class. The
			 * CreateType path nulls klass->runtime_vtable for this case so
			 * the next mono_class_vtable_checked allocates a properly sized
			 * one. Update data_items in place so subsequent dispatches use
			 * the fresh vtable too. */
			{
				int _alloc = mono_vtable_alloc_slots (vtable);
				int _vts_now = m_class_get_vtable_size (vtable->klass);
				if (G_UNLIKELY (_alloc >= 0 && _vts_now > _alloc)) {
					ERROR_DECL (_e);
					MonoVTable *fresh = mono_class_vtable_checked (vtable->klass, _e);
					if (is_ok (_e) && fresh && fresh != vtable) {
						frame->imethod->data_items [ip [4]] = fresh;
						vtable = fresh;
					} else {
						mono_error_cleanup (_e);
					}
				}
			}
			if (!mono_class_is_before_field_init (vtable->klass)) {
				INIT_VTABLE (vtable);
			}
			guint16 imethod_index = ip [3];
			return_offset = ip [1];
			call_args_offset = ip [2];

			// FIXME push/pop LMF
			MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (vtable->klass));
			if (G_UNLIKELY (!o)) {
				mono_error_set_out_of_memory (error, "Could not allocate %i bytes", m_class_get_instance_size (vtable->klass));
				THROW_EX (interp_error_convert_to_exception (frame, error, ip), ip);
			}

			// This is return value
			LOCAL_VAR (return_offset, MonoObject*) = o;
			// Set `this` arg for ctor call
			LOCAL_VAR (call_args_offset, MonoObject*) = o;
			ip += 5;

			cmethod = (InterpMethod*)frame->imethod->data_items [imethod_index];

			goto jit_call;
		}
		MINT_IN_CASE(MINT_NEWOBJ_INLINED) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]];
			{
				int _alloc = mono_vtable_alloc_slots (vtable);
				int _vts_now = m_class_get_vtable_size (vtable->klass);
				if (G_UNLIKELY (_alloc >= 0 && _vts_now > _alloc)) {
					ERROR_DECL (_e);
					MonoVTable *fresh = mono_class_vtable_checked (vtable->klass, _e);
					if (is_ok (_e) && fresh && fresh != vtable) {
						frame->imethod->data_items [ip [2]] = fresh;
						vtable = fresh;
					} else {
						mono_error_cleanup (_e);
					}
				}
			}
			if (!mono_class_is_before_field_init (vtable->klass)) {
				INIT_VTABLE (vtable);
			}

			// FIXME push/pop LMF
			MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (vtable->klass));
			if (G_UNLIKELY (!o)) {
				mono_error_set_out_of_memory (error, "Could not allocate %i bytes", m_class_get_instance_size (vtable->klass));
				THROW_EX (interp_error_convert_to_exception (frame, error, ip), ip);
			}

			// This is return value
			LOCAL_VAR (ip [1], MonoObject*) = o;
			ip += 3;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_NEWOBJ_VT) {
			guint16 imethod_index = ip [3];
			guint16 ret_size = ip [4];
			return_offset = ip [1];
			call_args_offset = ip [2];
			gpointer this_vt = locals + return_offset;

			// clear the valuetype
			memset (this_vt, 0, ret_size);
			// pass the address of the valuetype
			LOCAL_VAR (call_args_offset, gpointer) = this_vt;
			ip += 5;

			cmethod = (InterpMethod*)frame->imethod->data_items [imethod_index];
			goto jit_call;
		}
		MINT_IN_CASE(MINT_NEWOBJ_SLOW) {
			guint32 const token = ip [3];
			return_offset = ip [1];
			call_args_offset = ip [2];

			cmethod = (InterpMethod*)frame->imethod->data_items [token];

			MonoClass * const newobj_class = cmethod->method->klass;

			/*
			 * First arg is the object.
			 * a constructor returns void, but we need to return the object we created
			 */

			g_assert (!m_class_is_valuetype (newobj_class));

			// FIXME push/pop LMF
			MonoVTable *vtable = mono_class_vtable_checked (newobj_class, error);
			if (!is_ok (error) || (!mono_class_is_before_field_init (vtable->klass) && !mono_runtime_class_init_full (vtable, error))) {
				MonoException *exc = interp_error_convert_to_exception (frame, error, ip);
				g_assert (exc);
				THROW_EX (exc, ip);
			}
			error_init_reuse (error);
			MonoObject* o = mono_object_new_checked (newobj_class, error);
			LOCAL_VAR (return_offset, MonoObject*) = o; // return value
			LOCAL_VAR (call_args_offset, MonoObject*) = o; // first parameter

			mono_interp_error_cleanup (error); // FIXME: do not swallow the error
			EXCEPTION_CHECKPOINT;
			ip += 4;
			goto jit_call;
		}

		MINT_IN_CASE(MINT_ROL_I4_IMM) {
			guint32 val = LOCAL_VAR (ip [2], guint32);
			int amount = ip [3];
			LOCAL_VAR (ip [1], guint32) = (val << amount) | (val >> (32 - amount));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ROL_I8_IMM) {
			guint64 val = LOCAL_VAR (ip [2], guint64);
			int amount = ip [3];
			LOCAL_VAR (ip [1], guint64) = (val << amount) | (val >> (64 - amount));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ROR_I4_IMM) {
			guint32 val = LOCAL_VAR (ip [2], guint32);
			int amount = ip [3];
			LOCAL_VAR (ip [1], guint32) = (val >> amount) | (val << (32 - amount));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ROR_I8_IMM) {
			guint64 val = LOCAL_VAR (ip [2], guint64);
			int amount = ip [3];
			LOCAL_VAR (ip [1], guint64) = (val >> amount) | (val << (64 - amount));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CLZ_I4) LOCAL_VAR (ip [1], gint32) = interp_intrins_clz_i4 (LOCAL_VAR (ip [2], guint32)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLZ_I8) LOCAL_VAR (ip [1], gint64) = interp_intrins_clz_i8 (LOCAL_VAR (ip [2], guint64)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CTZ_I4) LOCAL_VAR (ip [1], gint32) = interp_intrins_ctz_i4 (LOCAL_VAR (ip [2], guint32)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CTZ_I8) LOCAL_VAR (ip [1], gint64) = interp_intrins_ctz_i8 (LOCAL_VAR (ip [2], guint64)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_POPCNT_I4) LOCAL_VAR (ip [1], gint32) = interp_intrins_popcount_i4 (LOCAL_VAR (ip [2], guint32)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_POPCNT_I8) LOCAL_VAR (ip [1], gint64) = interp_intrins_popcount_i8 (LOCAL_VAR (ip [2], guint64)); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG2_I4) LOCAL_VAR (ip [1], gint32) = 31 ^ interp_intrins_clz_i4 (LOCAL_VAR (ip [2], guint32) | 1); ip += 3; MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG2_I8) LOCAL_VAR (ip [1], gint32) = 63 ^ interp_intrins_clz_i8 (LOCAL_VAR (ip [2], guint64) | 1); ip += 3; MINT_IN_BREAK;

#ifdef INTERP_ENABLE_SIMD
		MINT_IN_CASE(MINT_SIMD_V128_LDC) {
			memcpy (locals + ip [1], ip + 2, SIZEOF_V128);
			ip += 10;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SIMD_V128_I1_CREATE) {
			interp_simd_create (locals + ip [1], locals + ip [2], 1);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SIMD_V128_I2_CREATE) {
			interp_simd_create (locals + ip [1], locals + ip [2], 2);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SIMD_V128_I4_CREATE) {
			interp_simd_create (locals + ip [1], locals + ip [2], 4);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SIMD_V128_I8_CREATE) {
			interp_simd_create (locals + ip [1], locals + ip [2], 8);
			ip += 3;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_SIMD_INTRINS_P_P)
			interp_simd_p_p_table [ip [3]] (locals + ip [1], locals + ip [2]);
			ip += 4;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SIMD_INTRINS_P_PP)
			interp_simd_p_pp_table [ip [4]] (locals + ip [1], locals + ip [2], locals + ip [3]);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SIMD_INTRINS_P_PPP)
			interp_simd_p_ppp_table [ip [5]] (locals + ip [1], locals + ip [2], locals + ip [3], locals + ip [4]);
			ip += 6;
			MINT_IN_BREAK;
#else
		MINT_IN_CASE(MINT_SIMD_V128_LDC)
		MINT_IN_CASE(MINT_SIMD_V128_I1_CREATE)
		MINT_IN_CASE(MINT_SIMD_V128_I2_CREATE)
		MINT_IN_CASE(MINT_SIMD_V128_I4_CREATE)
		MINT_IN_CASE(MINT_SIMD_V128_I8_CREATE)
		MINT_IN_CASE(MINT_SIMD_INTRINS_P_P)
		MINT_IN_CASE(MINT_SIMD_INTRINS_P_PP)
		MINT_IN_CASE(MINT_SIMD_INTRINS_P_PPP)
			g_assert_not_reached ();
			MINT_IN_BREAK;
#endif

		MINT_IN_CASE(MINT_INTRINS_SPAN_CTOR) {
			gpointer ptr = LOCAL_VAR (ip [2], gpointer);
			int len = LOCAL_VAR (ip [3], gint32);
			if (len < 0)
				THROW_EX (interp_get_exception_argument_out_of_range ("length", frame, ip), ip);
			gpointer span = locals + ip [1];
			*(gpointer*)span = ptr;
			*(gint32*)((gpointer*)span + 1) = len;
			ip += 4;;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_CLEAR_WITH_REFERENCES) {
			gpointer p = LOCAL_VAR (ip [1], gpointer);
			size_t size = LOCAL_VAR (ip [2], mono_u) * sizeof (gpointer);
			mono_gc_bzero_aligned (p, size);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_MARVIN_BLOCK) {
			guint32 *pp0 = (guint32*)(locals + ip [1]);
			guint32 *pp1 = (guint32*)(locals + ip [2]);
			guint32 *dest0 = (guint32*)(locals + ip [3]);
			guint32 *dest1 = (guint32*)(locals + ip [4]);

			interp_intrins_marvin_block (pp0, pp1, dest0, dest1);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_ASCII_CHARS_TO_UPPERCASE) {
			LOCAL_VAR (ip [1], gint32) = interp_intrins_ascii_chars_to_uppercase (LOCAL_VAR (ip [2], guint32));
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_MEMORYMARSHAL_GETARRAYDATAREF) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], gpointer) = (guint8*)o + MONO_STRUCT_OFFSET (MonoArray, vector);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_ORDINAL_IGNORE_CASE_ASCII) {
			LOCAL_VAR (ip [1], gint32) = interp_intrins_ordinal_ignore_case_ascii (LOCAL_VAR (ip [2], guint32), LOCAL_VAR (ip [3], guint32));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_64ORDINAL_IGNORE_CASE_ASCII) {
			LOCAL_VAR (ip [1], gint32) = interp_intrins_64ordinal_ignore_case_ascii (LOCAL_VAR (ip [2], guint64), LOCAL_VAR (ip [3], guint64));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_WIDEN_ASCII_TO_UTF16) {
			LOCAL_VAR (ip [1], mono_u) = interp_intrins_widen_ascii_to_utf16 (LOCAL_VAR (ip [2], guint8*), LOCAL_VAR (ip [3], mono_unichar2*), LOCAL_VAR (ip [4], mono_u));
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_RUNTIMEHELPERS_OBJECT_HAS_COMPONENT_SIZE) {
			MonoObject *obj = LOCAL_VAR (ip [2], MonoObject*);
			LOCAL_VAR (ip [1], gint32) = (obj->vtable->flags & MONO_VT_FLAG_ARRAY_OR_STRING) != 0;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CASTCLASS_INTERFACE)
		MINT_IN_CASE(MINT_ISINST_INTERFACE) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			if (o) {
				MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];
				gboolean isinst;
				if (MONO_VTABLE_IMPLEMENTS_INTERFACE (o->vtable, m_class_get_interface_id (c))) {
					isinst = TRUE;
				} else if (m_class_is_array_special_interface (c)) {
					/* slow path */
					// FIXME push/pop LMF
					isinst = mono_interp_isinst (o, c); // FIXME: do not swallow the error
				} else {
					isinst = FALSE;
				}

				if (!isinst) {
					gboolean const isinst_instr = *ip == MINT_ISINST_INTERFACE;
					if (isinst_instr)
						LOCAL_VAR (ip [1], MonoObject*) = NULL;
					else
						THROW_EX (interp_get_exception_invalid_cast (frame, ip), ip);
				} else {
					LOCAL_VAR (ip [1], MonoObject*) = o;
				}
			} else {
				LOCAL_VAR (ip [1], MonoObject*) = NULL;
			}
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CASTCLASS_COMMON)
		MINT_IN_CASE(MINT_ISINST_COMMON) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			if (o) {
				MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];
				gboolean isinst = mono_class_has_parent_fast (o->vtable->klass, c);

				if (!isinst) {
					gboolean const isinst_instr = *ip == MINT_ISINST_COMMON;
					if (isinst_instr)
						LOCAL_VAR (ip [1], MonoObject*) = NULL;
					else
						THROW_EX (interp_get_exception_invalid_cast (frame, ip), ip);
				} else {
					LOCAL_VAR (ip [1], MonoObject*) = o;
				}
			} else {
				LOCAL_VAR (ip [1], MonoObject*) = NULL;
			}
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CASTCLASS)
		MINT_IN_CASE(MINT_ISINST) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			if (o) {
				MonoClass* const c = (MonoClass*)frame->imethod->data_items [ip [3]];
				// FIXME push/pop LMF
				if (!mono_interp_isinst (o, c)) { // FIXME: do not swallow the error
					gboolean const isinst_instr = *ip == MINT_ISINST;
					if (isinst_instr)
						LOCAL_VAR (ip [1], MonoObject*) = NULL;
					else
						THROW_EX (interp_get_exception_invalid_cast (frame, ip), ip);
				} else {
					LOCAL_VAR (ip [1], MonoObject*) = o;
				}
			} else {
				LOCAL_VAR (ip [1], MonoObject*) = NULL;
			}
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_R_UN_I4)
			LOCAL_VAR (ip [1], double) = (double)LOCAL_VAR (ip [2], guint32);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CONV_R_UN_I8)
			LOCAL_VAR (ip [1], double) = (double)LOCAL_VAR (ip [2], guint64);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_UNBOX) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];

			if (!(m_class_get_rank (o->vtable->klass) == 0 && m_class_get_element_class (o->vtable->klass) == m_class_get_element_class (c)))
				THROW_EX (interp_get_exception_invalid_cast (frame, ip), ip);

			LOCAL_VAR (ip [1], gpointer) = mono_object_unbox_internal (o);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_THROW) {
			MonoException *local_ex = LOCAL_VAR (ip [1], MonoException*);
			if (!local_ex)
				local_ex = interp_get_exception_null_reference (frame, ip);

			THROW_EX (local_ex, ip);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SAFEPOINT)
			SAFEPOINT;
			++ip;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLDA_UNSAFE) {
			LOCAL_VAR (ip [1], gpointer) = (char*)LOCAL_VAR (ip [2], gpointer) + ip [3];
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDFLDA) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], gpointer) = (char *)o + ip [3];
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CKNULL) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], MonoObject*) = o;
			ip += 3;
			MINT_IN_BREAK;
		}

#define LDFLD_UNALIGNED(datatype, fieldtype, unaligned) do { \
	MonoObject *o = LOCAL_VAR (ip [2], MonoObject*); \
	NULL_CHECK (o); \
	if (unaligned) \
		memcpy (locals + ip [1], (char *)o + ip [3], sizeof (fieldtype)); \
	else \
		LOCAL_VAR (ip [1], datatype) = * (fieldtype *)((char *)o + ip [3]) ; \
	ip += 4; \
} while (0)

#define LDFLD(datamem, fieldtype) LDFLD_UNALIGNED(datamem, fieldtype, FALSE)

		MINT_IN_CASE(MINT_LDFLD_I1) LDFLD(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_U1) LDFLD(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_I2) LDFLD(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_U2) LDFLD(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_I4) LDFLD(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_I8) LDFLD(gint64, gint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_R4) LDFLD(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_R8) LDFLD(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_O) LDFLD(gpointer, gpointer); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_I8_UNALIGNED) LDFLD_UNALIGNED(gint64, gint64, TRUE); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDFLD_R8_UNALIGNED) LDFLD_UNALIGNED(double, double, TRUE); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_LDFLD_VT) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			memcpy (locals + ip [1], (char *)o + ip [3], ip [4]);
			ip += 5;
			MINT_IN_BREAK;
		}

#define STFLD_UNALIGNED(datatype, fieldtype, unaligned) do { \
	MonoObject *o = LOCAL_VAR (ip [1], MonoObject*); \
	NULL_CHECK (o); \
	if (unaligned) \
		memcpy ((char *)o + ip [3], locals + ip [2], sizeof (fieldtype)); \
	else \
		* (fieldtype *)((char *)o + ip [3]) = (fieldtype)(LOCAL_VAR (ip [2], datatype)); \
	ip += 4; \
} while (0)

#define STFLD(datamem, fieldtype) STFLD_UNALIGNED(datamem, fieldtype, FALSE)

		MINT_IN_CASE(MINT_STFLD_I1) STFLD(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_U1) STFLD(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_I2) STFLD(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_U2) STFLD(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_I4) STFLD(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_I8) STFLD(gint64, gint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_R4) STFLD(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_R8) STFLD(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_O) {
			MonoObject *o = LOCAL_VAR (ip [1], MonoObject*);
			NULL_CHECK (o);
			mono_gc_wbarrier_set_field_internal (o, (char*)o + ip [3], LOCAL_VAR (ip [2], MonoObject*));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STFLD_I8_UNALIGNED) STFLD_UNALIGNED(gint64, gint64, TRUE); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STFLD_R8_UNALIGNED) STFLD_UNALIGNED(double, double, TRUE); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_STFLD_VT_NOREF) {
			MonoObject *o = LOCAL_VAR (ip [1], MonoObject*);
			NULL_CHECK (o);
			memcpy ((char*)o + ip [3], locals + ip [2], ip [4]);
			ip += 5;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_STFLD_VT) {
			MonoClass *klass = (MonoClass*)frame->imethod->data_items [ip [4]];
			MonoObject *o = LOCAL_VAR (ip [1], MonoObject*);
			NULL_CHECK (o);
			mono_value_copy_internal ((char*)o + ip [3], locals + ip [2], klass);
			ip += 5;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LDSFLDA) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]];
			INIT_VTABLE (vtable);
			LOCAL_VAR (ip [1], gpointer) = frame->imethod->data_items [ip [3]];
			ip += 4;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LDTSFLDA) {
			MonoInternalThread *thread = mono_thread_internal_current ();
			guint32 offset = READ32 (ip + 2);
			LOCAL_VAR (ip [1], gpointer) = ((char*)thread->static_data [offset & 0x3f]) + (offset >> 6);
			ip += 4;
			MINT_IN_BREAK;
		}

/* We init class here to preserve cctor order */
#define LDSFLD(datatype, fieldtype) { \
	MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]]; \
	INIT_VTABLE (vtable); \
	LOCAL_VAR (ip [1], datatype) = * (fieldtype *)(frame->imethod->data_items [ip [3]]) ; \
	ip += 4; \
	}

		MINT_IN_CASE(MINT_LDSFLD_I1) LDSFLD(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_U1) LDSFLD(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_I2) LDSFLD(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_U2) LDSFLD(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_I4) LDSFLD(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_I8) LDSFLD(gint64, gint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_R4) LDSFLD(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_R8) LDSFLD(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDSFLD_O) LDSFLD(gpointer, gpointer); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_LDSFLD_VT) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]];
			INIT_VTABLE (vtable);

			gpointer addr = frame->imethod->data_items [ip [3]];
			guint16 size = ip [4];

			memcpy (locals + ip [1], addr, size);
			ip += 5;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LDSFLD_W) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [READ32 (ip + 2)];
			INIT_VTABLE (vtable);
			gpointer addr = frame->imethod->data_items [READ32 (ip + 4)];
			MonoClass *klass = frame->imethod->data_items [READ32 (ip + 6)];
			stackval_from_data (m_class_get_byval_arg (klass), (stackval*)(locals + ip [1]), addr, FALSE);
			ip += 8;
			MINT_IN_BREAK;
		}

#define STSFLD(datatype, fieldtype) { \
	MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]]; \
	INIT_VTABLE (vtable); \
	* (fieldtype *)(frame->imethod->data_items [ip [3]]) = (fieldtype)(LOCAL_VAR (ip [1], datatype)); \
	ip += 4; \
	}

		MINT_IN_CASE(MINT_STSFLD_I1) STSFLD(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_U1) STSFLD(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_I2) STSFLD(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_U2) STSFLD(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_I4) STSFLD(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_I8) STSFLD(gint64, gint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_R4) STSFLD(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_R8) STSFLD(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STSFLD_O) STSFLD(gpointer, gpointer); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_STSFLD_VT) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [ip [2]];
			INIT_VTABLE (vtable);
			gpointer addr = frame->imethod->data_items [ip [3]];
			memcpy (addr, locals + ip [1], ip [4]);
			ip += 5;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_STSFLD_W) {
			MonoVTable *vtable = (MonoVTable*) frame->imethod->data_items [READ32 (ip + 2)];
			INIT_VTABLE (vtable);
			gpointer addr = frame->imethod->data_items [READ32 (ip + 4)];
			MonoClass *klass = frame->imethod->data_items [READ32 (ip + 6)];
			stackval_to_data (m_class_get_byval_arg (klass), (stackval*)(locals + ip [1]), addr, FALSE);
			ip += 8;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_STOBJ_VT) {
			MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];
			mono_value_copy_internal (LOCAL_VAR (ip [1], gpointer), locals + ip [2], c);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STOBJ_VT_NOREF) {
			memcpy (LOCAL_VAR (ip [1], gpointer), locals + ip [2], ip [3]);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U8_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U8_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I8_U8) {
			guint64 val = LOCAL_VAR (ip [2], guint64);
			if (val > G_MAXINT64)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U8_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (!mono_try_trunc_u64 (val, (guint64*)(locals + ip [1])))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U8_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (!mono_try_trunc_u64 (val, (guint64*)(locals + ip [1])))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I8_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (!mono_try_trunc_i64 (val, (gint64*)(locals + ip [1])))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I8_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (!mono_try_trunc_i64 (val, (gint64*)(locals + ip [1])))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BOX) {
			MonoVTable *vtable = (MonoVTable*)frame->imethod->data_items [ip [3]];

			// FIXME push/pop LMF
			MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (vtable->klass));
			SET_TEMP_POINTER(o);
			stackval_to_data (m_class_get_byval_arg (vtable->klass), (stackval*)(locals + ip [2]), mono_object_get_data (o), FALSE);
			LOCAL_VAR (ip [1], MonoObject*) = o;
			SET_TEMP_POINTER(NULL);

			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BOX_VT) {
			MonoVTable *vtable = (MonoVTable*)frame->imethod->data_items [ip [3]];
			MonoClass *c = vtable->klass;

			if (G_UNLIKELY (m_class_is_byreflike (c))) {
				char *str = g_strdup_printf ("Cannot box IsByRefLike type '%s.%s'", m_class_get_name_space (c), m_class_get_name (c));
				MonoException *ex = mono_exception_from_name_msg (mono_defaults.corlib, "System", "InvalidProgramException", str);
				g_free (str);
				THROW_EX (ex, ip);
			}

			// FIXME push/pop LMF
			MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (c));
			SET_TEMP_POINTER(o);
			mono_value_copy_internal (mono_object_get_data (o), locals + ip [2], c);
			LOCAL_VAR (ip [1], MonoObject*) = o;
			SET_TEMP_POINTER(NULL);

			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BOX_PTR) {
			MonoVTable *vtable = (MonoVTable*)frame->imethod->data_items [ip [3]];
			MonoClass *c = vtable->klass;

			// FIXME push/pop LMF
			MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (c));
			SET_TEMP_POINTER(o);
			mono_value_copy_internal (mono_object_get_data (o), LOCAL_VAR (ip [2], gpointer), c);
			LOCAL_VAR (ip [1], MonoObject*) = o;
			SET_TEMP_POINTER(NULL);

			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_BOX_NULLABLE_PTR) {
			MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];

			// FIXME push/pop LMF
			LOCAL_VAR (ip [1], MonoObject*) = mono_nullable_box (LOCAL_VAR (ip [2], gpointer), c, error);
			mono_interp_error_cleanup (error); /* FIXME: don't swallow the error */
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_NEWARR) {
			// FIXME push/pop LMF
			MonoVTable *vtable = (MonoVTable*)frame->imethod->data_items [ip [3]];
			LOCAL_VAR (ip [1], MonoObject*) = (MonoObject*) mono_array_new_specific_checked (vtable, LOCAL_VAR (ip [2], gint32), error);
			if (!is_ok (error)) {
				THROW_EX (interp_error_convert_to_exception (frame, error, ip), ip);
			}
			ip += 4;
			/*if (profiling_classes) {
				guint count = GPOINTER_TO_UINT (g_hash_table_lookup (profiling_classes, o->vtable->klass));
				count++;
				g_hash_table_insert (profiling_classes, o->vtable->klass, GUINT_TO_POINTER (count));
			}*/

			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_NEWSTR) {
			LOCAL_VAR (ip [1], MonoString*) = mono_string_new_size_checked (LOCAL_VAR (ip [2], gint32), error);
			if (!is_ok (error)) {
				THROW_EX (interp_error_convert_to_exception (frame, error, ip), ip);
			}
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDLEN) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], mono_u) = mono_array_length_internal ((MonoArray *)o);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_GETCHR) {
			MonoString *s = LOCAL_VAR (ip [2], MonoString*);
			NULL_CHECK (s);
			int i32 = LOCAL_VAR (ip [3], int);
			if (i32 < 0 || i32 >= mono_string_length_internal (s))
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = mono_string_chars_internal (s)[i32];
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_GETITEM_SPAN) {
			MonoSpanOfVoid *span = LOCAL_VAR (ip [2], MonoSpanOfVoid*);
			int index = LOCAL_VAR (ip [3], int);
			NULL_CHECK (span);

			gint32 length = span->_length;
			if (index < 0 || index >= length)
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);

			gsize element_size = (gsize)(gint16)ip [4];
			LOCAL_VAR (ip [1], gpointer) = (guint8*)span->_reference + index * element_size;

			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_GETITEM_LOCALSPAN) {
			// Same as getitem span but we know the offset of the span structure on the stack
			MonoSpanOfVoid *span = (MonoSpanOfVoid*)(locals + ip [2]);
			int index = LOCAL_VAR (ip [3], int);

			gint32 length = span->_length;
			if (index < 0 || index >= length)
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);

			gsize element_size = (gsize)(gint16)ip [4];
			LOCAL_VAR (ip [1], gpointer) = (guint8*)span->_reference + index * element_size;

			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STRLEN) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], gint32) = mono_string_length_internal ((MonoString*) o);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ARRAY_RANK) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], gint32) = m_class_get_rank (mono_object_class (o));
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ARRAY_ELEMENT_SIZE) {
			// FIXME push/pop LMF
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], gint32) = mono_array_element_size (mono_object_class (o));
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDELEMA1) {
			/* No bounds, one direction */
			MonoArray *ao = LOCAL_VAR (ip [2], MonoArray*);
			NULL_CHECK (ao);
			guint32 index = LOCAL_VAR (ip [3], guint32);
			if (index >= ao->max_length)
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);
			guint16 size = ip [4];
			LOCAL_VAR (ip [1], gpointer) = mono_array_addr_with_size_fast (ao, size, index);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDELEMA) {
			guint16 rank = ip [3];
			guint16 esize = ip [4];
			stackval *sp = (stackval*)(locals + ip [2]);

			MonoArray *ao = (MonoArray*) sp [0].data.o;
			NULL_CHECK (ao);

			g_assert (ao->bounds);
			guint32 pos = 0;
			for (int i = 0; i < rank; i++) {
				gint32 idx = sp [i + 1].data.i;
				gint32 lower = ao->bounds [i].lower_bound;
				guint32 len = ao->bounds [i].length;
				if (idx < lower || (guint32)(idx - lower) >= len)
					THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);
				pos = (pos * len) + (guint32)(idx - lower);
			}

			LOCAL_VAR (ip [1], gpointer) = mono_array_addr_with_size_fast (ao, esize, pos);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDELEMA_TC) {
			// FIXME push/pop LMF
			stackval *sp = (stackval*)(locals + ip [2]);

			MonoObject *o = (MonoObject*) sp [0].data.o;
			NULL_CHECK (o);

			MonoClass *klass = (MonoClass*)frame->imethod->data_items [ip [3]];
			MonoException *address_ex = ves_array_element_address (frame, klass, (MonoArray *) o, (gpointer*)(locals + ip [1]), sp + 1, TRUE);
			if (address_ex)
				THROW_EX (address_ex, ip);
			ip += 4;
			MINT_IN_BREAK;
		}

#define LDELEM(datatype,elemtype) do { \
	MonoArray *o = LOCAL_VAR (ip [2], MonoArray*); \
	NULL_CHECK (o); \
	guint32 aindex = LOCAL_VAR (ip [3], guint32); \
	if (aindex >= mono_array_length_internal (o)) \
		THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip); \
	LOCAL_VAR (ip [1], datatype) = mono_array_get_fast (o, elemtype, aindex); \
	ip += 4; \
} while (0)
		MINT_IN_CASE(MINT_LDELEM_I1) LDELEM(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_U1) LDELEM(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_I2) LDELEM(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_U2) LDELEM(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_I4) LDELEM(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_U4) LDELEM(gint32, guint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_I8) LDELEM(gint64, guint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_R4) LDELEM(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_R8) LDELEM(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_REF) LDELEM(gpointer, gpointer); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LDELEM_VT) {
			MonoArray *o = LOCAL_VAR (ip [2], MonoArray*);
			NULL_CHECK (o);
			mono_u aindex = LOCAL_VAR (ip [3], gint32);
			if (aindex >= mono_array_length_internal (o))
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);

			guint16 size = ip [4];
			char *src_addr = mono_array_addr_with_size_fast ((MonoArray *) o, size, aindex);
			memcpy (locals + ip [1], src_addr, size);

			ip += 5;
			MINT_IN_BREAK;
		}
#define STELEM_PROLOG(o, aindex) do { \
	o = LOCAL_VAR (ip [1], MonoArray*); \
	NULL_CHECK (o); \
	aindex = LOCAL_VAR (ip [2], gint32); \
	if (aindex >= mono_array_length_internal (o)) \
		THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip); \
} while (0)

#define STELEM(datatype, elemtype) do { \
	MonoArray *o; \
	guint32 aindex; \
	STELEM_PROLOG(o, aindex); \
	mono_array_set_fast (o, elemtype, aindex, LOCAL_VAR (ip [3], datatype)); \
	ip += 4; \
} while (0)
		MINT_IN_CASE(MINT_STELEM_I1) STELEM(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_U1) STELEM(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_I2) STELEM(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_U2) STELEM(gint32, guint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_I4) STELEM(gint32, gint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_I8) STELEM(gint64, gint64); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_R4) STELEM(float, float); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_R8) STELEM(double, double); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_STELEM_REF_UNCHECKED) {
			MonoArray *o;
			guint32 aindex;
			STELEM_PROLOG(o, aindex);
			mono_array_setref_fast ((MonoArray *) o, aindex, LOCAL_VAR (ip [3], MonoObject*));
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STELEM_REF) {
			MonoArray *o;
			guint32 aindex;
			STELEM_PROLOG(o, aindex);
			MonoObject *ref = LOCAL_VAR (ip [3], MonoObject*);

			if (ref) {
				// FIXME push/pop LMF
				gboolean isinst = mono_interp_isinst (ref, m_class_get_element_class (mono_object_class (o)));
				if (!isinst)
					THROW_EX (interp_get_exception_array_type_mismatch (frame, ip), ip);
			}
			mono_array_setref_fast ((MonoArray *) o, aindex, ref);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STELEM_VT) {
			MonoArray *o = LOCAL_VAR (ip [1], MonoArray*);
			NULL_CHECK (o);
			guint32 aindex = LOCAL_VAR (ip [2], guint32);
			if (aindex >= mono_array_length_internal (o))
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);

			guint16 size = ip [5];
			char *dst_addr = mono_array_addr_with_size_fast ((MonoArray *) o, size, aindex);
			MonoClass *klass_vt = (MonoClass*)frame->imethod->data_items [ip [4]];
			mono_value_copy_internal (dst_addr, locals + ip [3], klass_vt);
			ip += 6;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_STELEM_VT_NOREF) {
			MonoArray *o = LOCAL_VAR (ip [1], MonoArray*);
			NULL_CHECK (o);
			guint32 aindex = LOCAL_VAR (ip [2], guint32);
			if (aindex >= mono_array_length_internal (o))
				THROW_EX (interp_get_exception_index_out_of_range (frame, ip), ip);

			guint16 size = ip [5];
			char *dst_addr = mono_array_addr_with_size_fast ((MonoArray *) o, size, aindex);
			memcpy (dst_addr, locals + ip [3], size);
			ip += 6;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I4_U4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I4_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < G_MININT32 || val > G_MAXINT32)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint32) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I4_U8) {
			guint64 val = LOCAL_VAR (ip [2], guint64);
			if (val > G_MAXINT32)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint32) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I4_R4) {
			float val = LOCAL_VAR (ip [2], float);
			double val_r8 = (double)val;
			if (val_r8 > ((double)G_MININT32 - 1) && val_r8 < ((double)G_MAXINT32 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint32) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I4_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > ((double)G_MININT32 - 1) && val < ((double)G_MAXINT32 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint32) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U4_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U4_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0 || val > G_MAXUINT32)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (guint32) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U4_R4) {
			float val = LOCAL_VAR (ip [2], float);
			double val_r8 = val;
			if (val_r8 > -1.0 && val_r8 < ((double)G_MAXUINT32 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint32)val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U4_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > -1.0 && val < ((double)G_MAXUINT32 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint32)val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < G_MININT16 || val > G_MAXINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint16)val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_U4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0 || val > G_MAXINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint16)val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < G_MININT16 || val > G_MAXINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint16) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_U8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0 || val > G_MAXINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint16) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (val > (G_MININT16 - 1) && val < (G_MAXINT16 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint16) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I2_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > (G_MININT16 - 1) && val < (G_MAXINT16 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint16) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U2_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0 || val > G_MAXUINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U2_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0 || val > G_MAXUINT16)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (guint16) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U2_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (val > -1.0f && val < (G_MAXUINT16 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint16) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U2_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > -1.0 && val < (G_MAXUINT16 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint16) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < G_MININT8 || val > G_MAXINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_U4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0 || val > G_MAXINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < G_MININT8 || val > G_MAXINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint8) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_U8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0 || val > G_MAXINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (gint8) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (val > (G_MININT8 - 1) && val < (G_MAXINT8 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint8) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_I1_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > (G_MININT8 - 1) && val < (G_MAXINT8 + 1))
				LOCAL_VAR (ip [1], gint32) = (gint8) val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U1_I4) {
			gint32 val = LOCAL_VAR (ip [2], gint32);
			if (val < 0 || val > G_MAXUINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U1_I8) {
			gint64 val = LOCAL_VAR (ip [2], gint64);
			if (val < 0 || val > G_MAXUINT8)
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = (guint8) val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U1_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (val > -1.0f && val < (G_MAXUINT8 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint8)val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CONV_OVF_U1_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (val > -1.0 && val < (G_MAXUINT8 + 1))
				LOCAL_VAR (ip [1], gint32) = (guint8)val;
			else
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CKFINITE_R4) {
			float val = LOCAL_VAR (ip [2], float);
			if (!mono_isfinite (val))
				THROW_EX (interp_get_exception_arithmetic (frame, ip), ip);
			LOCAL_VAR (ip [1], float) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CKFINITE_R8) {
			double val = LOCAL_VAR (ip [2], double);
			if (!mono_isfinite (val))
				THROW_EX (interp_get_exception_arithmetic (frame, ip), ip);
			LOCAL_VAR (ip [1], double) = val;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MKREFANY) {
			MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];

			gpointer addr = LOCAL_VAR (ip [2], gpointer);
			/* Write the typedref value */
			MonoTypedRef *tref = (MonoTypedRef*)(locals + ip [1]);
			tref->klass = c;
			tref->type = m_class_get_byval_arg (c);
			tref->value = addr;

			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REFANYTYPE) {
			MonoTypedRef *tref = (MonoTypedRef*)(locals + ip [2]);

			LOCAL_VAR (ip [1], gpointer) = tref->type;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_REFANYVAL) {
			MonoTypedRef *tref = (MonoTypedRef*)(locals + ip [2]);

			MonoClass *c = (MonoClass*)frame->imethod->data_items [ip [3]];
			if (c != tref->klass)
				THROW_EX (interp_get_exception_invalid_cast (frame, ip), ip);

			LOCAL_VAR (ip [1], gpointer) = tref->value;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ADD_OVF_I4) {
			gint32 i1 = LOCAL_VAR (ip [2], gint32);
			gint32 i2 = LOCAL_VAR (ip [3], gint32);
			if (CHECK_ADD_OVERFLOW (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = i1 + i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ADD_OVF_I8) {
			gint64 l1 = LOCAL_VAR (ip [2], gint64);
			gint64 l2 = LOCAL_VAR (ip [3], gint64);
			if (CHECK_ADD_OVERFLOW64 (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 + l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ADD_OVF_UN_I4) {
			guint32 i1 = LOCAL_VAR (ip [2], guint32);
			guint32 i2 = LOCAL_VAR (ip [3], guint32);
			if (CHECK_ADD_OVERFLOW_UN (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint32) = i1 + i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ADD_OVF_UN_I8) {
			guint64 l1 = LOCAL_VAR (ip [2], guint64);
			guint64 l2 = LOCAL_VAR (ip [3], guint64);
			if (CHECK_ADD_OVERFLOW64_UN (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = l1 + l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MUL_OVF_I4) {
			gint32 i1 = LOCAL_VAR (ip [2], gint32);
			gint32 i2 = LOCAL_VAR (ip [3], gint32);
			if (CHECK_MUL_OVERFLOW (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = i1 * i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MUL_OVF_I8) {
			gint64 l1 = LOCAL_VAR (ip [2], gint64);
			gint64 l2 = LOCAL_VAR (ip [3], gint64);
			if (CHECK_MUL_OVERFLOW64 (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 * l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MUL_OVF_UN_I4) {
			guint32 i1 = LOCAL_VAR (ip [2], guint32);
			guint32 i2 = LOCAL_VAR (ip [3], guint32);
			if (CHECK_MUL_OVERFLOW_UN (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint32) = i1 * i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MUL_OVF_UN_I8) {
			guint64 l1 = LOCAL_VAR (ip [2], guint64);
			guint64 l2 = LOCAL_VAR (ip [3], guint64);
			if (CHECK_MUL_OVERFLOW64_UN (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint64) = l1 * l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SUB_OVF_I4) {
			gint32 i1 = LOCAL_VAR (ip [2], gint32);
			gint32 i2 = LOCAL_VAR (ip [3], gint32);
			if (CHECK_SUB_OVERFLOW (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint32) = i1 - i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SUB_OVF_I8) {
			gint64 l1 = LOCAL_VAR (ip [2], gint64);
			gint64 l2 = LOCAL_VAR (ip [3], gint64);
			if (CHECK_SUB_OVERFLOW64 (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 - l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SUB_OVF_UN_I4) {
			guint32 i1 = LOCAL_VAR (ip [2], guint32);
			guint32 i2 = LOCAL_VAR (ip [3], guint32);
			if (CHECK_SUB_OVERFLOW_UN (i1, i2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], guint32) = i1 - i2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_SUB_OVF_UN_I8) {
			guint64 l1 = LOCAL_VAR (ip [2], guint64);
			guint64 l2 = LOCAL_VAR (ip [3], guint64);
			if (CHECK_SUB_OVERFLOW64_UN (l1, l2))
				THROW_EX (interp_get_exception_overflow (frame, ip), ip);
			LOCAL_VAR (ip [1], gint64) = l1 - l2;
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ENDFINALLY) {
			guint16 clause_index = *(ip + 1);

			guint16 *ret_ip = *(guint16**)(locals + frame->imethod->clause_data_offsets [clause_index]);
			if (!ret_ip) {
				// this clause was called from EH, return to eh
				g_assert (clause_args && clause_args->exec_frame == frame);
				goto exit_clause;
			}
			ip = ret_ip;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_CALL_HANDLER)
		MINT_IN_CASE(MINT_CALL_HANDLER_S) {
			gboolean short_offset = *ip == MINT_CALL_HANDLER_S;
			const guint16 *ret_ip = short_offset ? (ip + 3) : (ip + 4);
			guint16 clause_index = *(ret_ip - 1);

			*(const guint16**)(locals + frame->imethod->clause_data_offsets [clause_index]) = ret_ip;

			// jump to clause
			ip += short_offset ? (gint16)*(ip + 1) : (gint32)READ32 (ip + 1);
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LEAVE_CHECK)
		MINT_IN_CASE(MINT_LEAVE_S_CHECK) {
			int leave_opcode = *ip;

			if (frame->imethod->method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE) {
				MonoException *abort_exc = mono_interp_leave (frame);
				if (abort_exc)
					THROW_EX (abort_exc, ip);
			}

			gboolean const short_offset = leave_opcode == MINT_LEAVE_S_CHECK;
			ip += short_offset ? (gint16)*(ip + 1) : (gint32)READ32 (ip + 1);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ICALL) {
			stackval *ret = (stackval*)(locals + ip [1]);
			stackval *args = (stackval*)(locals + ip [2]);
			MintICallSig icall_sig = (MintICallSig)ip [3];
			gpointer target_ip = frame->imethod->data_items [ip [4]];

			frame->state.ip = ip + 5;
			do_icall_wrapper (frame, NULL, icall_sig, ret, args, target_ip, FALSE, &gc_transitions);
			EXCEPTION_CHECKPOINT;
			CHECK_RESUME_STATE (context);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDPTR)
			LOCAL_VAR (ip [1], gpointer) = frame->imethod->data_items [ip [2]];
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MONO_NEWOBJ)
			// FIXME push/pop LMF
			LOCAL_VAR (ip [1], MonoObject*) = mono_interp_new ((MonoClass*)frame->imethod->data_items [ip [2]]); // FIXME: do not swallow the error
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MONO_RETOBJ)
			// FIXME push/pop LMF
			stackval_from_data (mono_method_signature_internal (frame->imethod->method)->ret, frame->stack, LOCAL_VAR (ip [1], gpointer),
			     mono_method_signature_internal (frame->imethod->method)->pinvoke && !mono_method_signature_internal (frame->imethod->method)->marshalling_disabled);
			frame_data_allocator_pop (&context->data_stack, frame);
			goto exit_frame;
		MINT_IN_CASE(MINT_MONO_MEMORY_BARRIER) {
			++ip;
			mono_memory_barrier ();
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_U1) {
			guint8 *dest = LOCAL_VAR (ip [2], guint8*);
			guint8 exch = LOCAL_VAR (ip[3], guint8);
			NULL_CHECK(dest);
			LOCAL_VAR(ip[1], guint32) = (guint32)mono_atomic_xchg_u8(dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_I1) {
			gint8 *dest = LOCAL_VAR (ip [2], gint8*);
			gint8 exch = LOCAL_VAR (ip[3], gint8);
			NULL_CHECK(dest);
			LOCAL_VAR(ip[1], gint32) = (gint32)(gint8)mono_atomic_xchg_u8((guint8*)dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_U2) {
			guint16 *dest = LOCAL_VAR (ip [2], guint16*);
			guint16 exch = LOCAL_VAR (ip[3], guint16);
			NULL_CHECK(dest);
			LOCAL_VAR(ip[1], guint32) = (guint32)mono_atomic_xchg_u16(dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_I2) {
			gint16 *dest = LOCAL_VAR (ip [2], gint16*);
			gint16 exch = LOCAL_VAR (ip[3], gint16);
			NULL_CHECK(dest);
			LOCAL_VAR(ip[1], gint32) = (gint32)(gint16)mono_atomic_xchg_u16((guint16*)dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_I4) {
			gint32 *dest = LOCAL_VAR (ip [2], gint32*);
			gint32 exch = LOCAL_VAR (ip[3], gint32);
			NULL_CHECK(dest);
			LOCAL_VAR(ip[1], gint32) = mono_atomic_xchg_i32(dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_EXCHANGE_I8) {
			gboolean flag = FALSE;
			gint64 *dest = LOCAL_VAR (ip [2], gint64*);
			gint64 exch = LOCAL_VAR (ip [3], gint64);
			NULL_CHECK(dest);
#if SIZEOF_VOID_P == 4
			if (G_UNLIKELY (((size_t)dest) & 0x7)) {
				gint64 result;
				mono_interlocked_lock ();
				result = *dest;
				*dest = exch;
				mono_interlocked_unlock ();
				LOCAL_VAR (ip [1], gint64) = result;
				flag = TRUE;
			}
#endif
			if (!flag)
				LOCAL_VAR (ip [1], gint64) = mono_atomic_xchg_i64 (dest, exch);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_U1) {
			guint8 *dest = LOCAL_VAR(ip[2], guint8*);
			guint8 value = LOCAL_VAR(ip[3], guint8);
			guint8 comparand = LOCAL_VAR(ip[4], guint8);
			NULL_CHECK(dest);

			LOCAL_VAR(ip[1], guint32) = (guint32)mono_atomic_cas_u8(dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_I1) {
			gint8 *dest = LOCAL_VAR(ip[2], gint8*);
			gint8 value = LOCAL_VAR(ip[3], gint8);
			gint8 comparand = LOCAL_VAR(ip[4], gint8);
			NULL_CHECK(dest);

			LOCAL_VAR(ip[1], gint32) = (gint32)(gint8)mono_atomic_cas_u8((guint8*)dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_U2) {
			guint16 *dest = LOCAL_VAR(ip[2], guint16*);
			guint16 value = LOCAL_VAR(ip[3], guint16);
			guint16 comparand = LOCAL_VAR(ip[4], guint16);
			NULL_CHECK(dest);

			LOCAL_VAR(ip[1], guint32) = (guint32)mono_atomic_cas_u16(dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_I2) {
			gint16 *dest = LOCAL_VAR(ip[2], gint16*);
			gint16 value = LOCAL_VAR(ip[3], gint16);
			gint16 comparand = LOCAL_VAR(ip[4], gint16);
			NULL_CHECK(dest);

			LOCAL_VAR(ip[1], gint32) = (gint32)(gint16)mono_atomic_cas_u16((guint16*)dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_I4) {
			gint32 *dest = LOCAL_VAR(ip[2], gint32*);
			gint32 value = LOCAL_VAR(ip[3], gint32);
			gint32 comparand = LOCAL_VAR(ip[4], gint32);
			NULL_CHECK(dest);

			LOCAL_VAR(ip[1], gint32) = mono_atomic_cas_i32(dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_CMPXCHG_I8) {
			gboolean flag = FALSE;
			gint64 *dest = LOCAL_VAR(ip[2], gint64*);
			gint64 value = LOCAL_VAR(ip[3], gint64);
			gint64 comparand = LOCAL_VAR(ip[4], gint64);
			NULL_CHECK(dest);

#if SIZEOF_VOID_P == 4
			if (G_UNLIKELY ((size_t)dest & 0x7)) {
				gint64 old;
				mono_interlocked_lock ();
				old = *dest;
				if (old == comparand)
					*dest = value;
				mono_interlocked_unlock ();
				LOCAL_VAR(ip[1], gint64) = old;
				flag = TRUE;
			}
#endif

			if (!flag)
				LOCAL_VAR(ip[1], gint64) = mono_atomic_cas_i64(dest, value, comparand);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_LDDOMAIN)
			LOCAL_VAR (ip [1], gpointer) = mono_domain_get ();
			ip += 2;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MONO_ENABLE_GCTRANS)
			gc_transitions = TRUE;
			ip++;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SDB_INTR_LOC)
			if (G_UNLIKELY (ss_enabled)) {
				typedef void (*T) (void);
				static T ss_tramp;

				if (!ss_tramp) {
					// FIXME push/pop LMF
					void *tramp = mini_get_single_step_trampoline ();
					mono_memory_barrier ();
					ss_tramp = (T)tramp;
				}

				/*
				 * Make this point to the MINT_SDB_SEQ_POINT instruction which follows this since
				 * the address of that instruction is stored as the seq point address. Add also
				 * 1 to offset subtraction from interp_frame_get_ip.
				 */
				frame->state.ip = ip + 2;

				/*
				 * Use the same trampoline as the JIT. This ensures that
				 * the debugger has the context for the last interpreter
				 * native frame.
				 */
				do_debugger_tramp (ss_tramp, frame);

				CHECK_RESUME_STATE (context);
			}
			++ip;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SDB_SEQ_POINT)
			/* Just a placeholder for a breakpoint */
#if HOST_WASI
			if (debugger_enabled)
				mono_component_debugger()->receive_and_process_command_from_debugger_agent ();
#endif
			++ip;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SDB_BREAKPOINT) {
			typedef void (*T) (void);
			static T bp_tramp;
			if (!bp_tramp) {
				// FIXME push/pop LMF
				void *tramp = mini_get_breakpoint_trampoline ();
				mono_memory_barrier ();
				bp_tramp = (T)tramp;
			}

			/* Add 1 to offset subtraction from interp_frame_get_ip */
			frame->state.ip = ip + 1;

			/* Use the same trampoline as the JIT */
			do_debugger_tramp (bp_tramp, frame);

			CHECK_RESUME_STATE (context);

			++ip;
			MINT_IN_BREAK;
		}

#define RELOP(datatype, op) \
	LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], datatype) op LOCAL_VAR (ip [3], datatype); \
	ip += 4;

#define RELOP_FP(datatype, op, noorder) do { \
	datatype a1 = LOCAL_VAR (ip [2], datatype); \
	datatype a2 = LOCAL_VAR (ip [3], datatype); \
	if (mono_isunordered (a1, a2)) \
		LOCAL_VAR (ip [1], gint32) = noorder; \
	else \
		LOCAL_VAR (ip [1], gint32) = a1 op a2; \
	ip += 4; \
} while (0)

		MINT_IN_CASE(MINT_CEQ_I4)
			RELOP(gint32, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEQ0_I4)
			LOCAL_VAR (ip [1], gint32) = (LOCAL_VAR (ip [2], gint32) == 0);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEQ_I8)
			RELOP(gint64, ==);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEQ_R4)
			RELOP_FP(float, ==, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEQ_R8)
			RELOP_FP(double, ==, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CNE_I4)
			RELOP(gint32, !=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CNE_I8)
			RELOP(gint64, !=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CNE_R4)
			RELOP_FP(float, !=, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CNE_R8)
			RELOP_FP(double, !=, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_I4)
			RELOP(gint32, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_I8)
			RELOP(gint64, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_R4)
			RELOP_FP(float, >, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_R8)
			RELOP_FP(double, >, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGE_I4)
			RELOP(gint32, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGE_I8)
			RELOP(gint64, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGE_R4)
			RELOP_FP(float, >=, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGE_R8)
			RELOP_FP(double, >=, 0);
			MINT_IN_BREAK;

#define RELOP_CAST(datatype, op) \
	LOCAL_VAR (ip [1], gint32) = LOCAL_VAR (ip [2], datatype) op LOCAL_VAR (ip [3], datatype); \
	ip += 4;

		MINT_IN_CASE(MINT_CGE_UN_I4)
			RELOP_CAST(guint32, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGE_UN_I8)
			RELOP_CAST(guint64, >=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_UN_I4)
			RELOP_CAST(guint32, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_UN_I8)
			RELOP_CAST(guint64, >);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_UN_R4)
			RELOP_FP(float, >, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CGT_UN_R8)
			RELOP_FP(double, >, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_I4)
			RELOP(gint32, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_I8)
			RELOP(gint64, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_R4)
			RELOP_FP(float, <, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_R8)
			RELOP_FP(double, <, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_UN_I4)
			RELOP_CAST(guint32, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_UN_I8)
			RELOP_CAST(guint64, <);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_UN_R4)
			RELOP_FP(float, <, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLT_UN_R8)
			RELOP_FP(double, <, 1);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_I4)
			RELOP(gint32, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_I8)
			RELOP(gint64, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_UN_I4)
			RELOP_CAST(guint32, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_UN_I8)
			RELOP_CAST(guint64, <=);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_R4)
			RELOP_FP(float, <=, 0);
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CLE_R8)
			RELOP_FP(double, <=, 0);
			MINT_IN_BREAK;

#undef RELOP
#undef RELOP_FP
#undef RELOP_CAST

		MINT_IN_CASE(MINT_LDFTN_ADDR) {
			LOCAL_VAR (ip [1], gpointer) = frame->imethod->data_items [ip [2]];
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDFTN) {
			InterpMethod *m = (InterpMethod*)frame->imethod->data_items [ip [2]];

			// FIXME push/pop LMF
			LOCAL_VAR (ip [1], gpointer) = imethod_to_ftnptr (m, FALSE);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDVIRTFTN) {
			InterpMethod *virtual_method = (InterpMethod*)frame->imethod->data_items [ip [3]];
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);

			// FIXME push/pop LMF
			InterpMethod *res_method = get_virtual_method (virtual_method, o->vtable);
			gboolean need_unbox = m_class_is_valuetype (res_method->method->klass) && !m_class_is_valuetype (virtual_method->method->klass);
			LOCAL_VAR (ip [1], gpointer) = imethod_to_ftnptr (res_method, need_unbox);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDFTN_DYNAMIC) {
			error_init_reuse (error);

			MonoMethod *local_cmethod = LOCAL_VAR (ip [2], MonoMethod*);

			if (local_cmethod->is_generic || mono_class_is_gtd (local_cmethod->klass)) {
				MonoException *ex = mono_exception_from_name_msg (mono_defaults.corlib, "System", "InvalidOperationException", "");
				THROW_EX (ex, ip);
			}

			// FIXME push/pop LMF
			if (G_UNLIKELY (mono_method_has_unmanaged_callers_only_attribute (local_cmethod))) {
				local_cmethod = mono_marshal_get_managed_wrapper  (local_cmethod, NULL, (MonoGCHandle)0, error);
				mono_error_assert_ok (error);
				gpointer addr = mini_get_interp_callbacks ()->create_method_pointer (local_cmethod, TRUE, error);
				LOCAL_VAR (ip [1], gpointer) = addr;
			} else {
				InterpMethod *m = mono_interp_get_imethod (local_cmethod);
				LOCAL_VAR (ip [1], gpointer) = imethod_to_ftnptr (m, FALSE);
			}
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_PROF_ENTER) {
			guint16 flag = ip [1];
			ip += 2;
			INTERP_PROFILER_RAISE(enter, ENTER);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_PROF_SAMPLEPOINT) {
			guint16 flag = ip [1];
			ip += 2;
			INTERP_PROFILER_RAISE_SAMPLEPOINT();
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_PROF_EXIT)
		MINT_IN_CASE(MINT_PROF_EXIT_VOID) {
			gboolean is_void = ip [0] == MINT_PROF_EXIT_VOID;
			guint16 flag = is_void ? ip [1] : ip [2];
			// Set retval
			if (!is_void) {
				int i32 = READ32 (ip + 3);
				if (i32)
					memmove (frame->retval, locals + ip [1], i32);
				else
					frame->retval [0] = LOCAL_VAR (ip [1], stackval);
			}
			INTERP_PROFILER_RAISE(leave, LEAVE);
			frame_data_allocator_pop (&context->data_stack, frame);
			goto exit_frame;
		}
		MINT_IN_CASE(MINT_PROF_COVERAGE_STORE) {
			++ip;
			guint32 *p = (guint32*)GINT_TO_POINTER (READ64 (ip));
			*p = 1;
			ip += 4;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_TIER_ENTER_METHOD) {
			frame->imethod->entry_count++;
			if (frame->imethod->entry_count > INTERP_TIER_ENTRY_LIMIT && !clause_args)
				ip = mono_interp_tier_up_frame_enter (frame, context);
			else
				ip++;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_TIER_PATCHPOINT) {
			frame->imethod->entry_count++;
			if (frame->imethod->entry_count > INTERP_TIER_ENTRY_LIMIT && !clause_args)
				ip = mono_interp_tier_up_frame_patchpoint (frame, context, ip [1]);
			else
				ip += 2;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_LDLOCA_S)
			LOCAL_VAR (ip [1], gpointer) = locals + ip [2];
			// ip[3] reserved for size data for jiterpreter
			ip += 4;
			MINT_IN_BREAK;

#define MOV(argtype1,argtype2) \
	LOCAL_VAR (ip [1], argtype1) = LOCAL_VAR (ip [2], argtype2); \
	ip += 3;
		// When loading from a local, we might need to sign / zero extend to 4 bytes
		// which is our minimum "register" size in interp. They are only needed when
		// the address of the local is taken and we should try to optimize them out
		// because the local can't be propagated.
		MINT_IN_CASE(MINT_MOV_I4_I1) MOV(gint32, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_I4_U1) MOV(gint32, guint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_I4_I2) MOV(gint32, gint16); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_I4_U2) MOV(gint32, guint16); MINT_IN_BREAK;
		// These moves are used to store into the field of a local valuetype
		// No sign extension is needed, we just move bytes from the execution
		// stack, no additional conversion is needed.
		MINT_IN_CASE(MINT_MOV_1) MOV(gint8, gint8); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_2) MOV(gint16, gint16); MINT_IN_BREAK;
		// Normal moves between locals
		MINT_IN_CASE(MINT_MOV_4) MOV(guint32, guint32); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_8) MOV(guint64, guint64); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_MOV_VT) {
			guint16 size = ip [3];
			memmove (locals + ip [1], locals + ip [2], size);
			ip += 4;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_MOV_8_2)
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64);
			LOCAL_VAR (ip [3], guint64) = LOCAL_VAR (ip [4], guint64);
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_8_3)
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64);
			LOCAL_VAR (ip [3], guint64) = LOCAL_VAR (ip [4], guint64);
			LOCAL_VAR (ip [5], guint64) = LOCAL_VAR (ip [6], guint64);
			ip += 7;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MOV_8_4)
			LOCAL_VAR (ip [1], guint64) = LOCAL_VAR (ip [2], guint64);
			LOCAL_VAR (ip [3], guint64) = LOCAL_VAR (ip [4], guint64);
			LOCAL_VAR (ip [5], guint64) = LOCAL_VAR (ip [6], guint64);
			LOCAL_VAR (ip [7], guint64) = LOCAL_VAR (ip [8], guint64);
			ip += 9;
			MINT_IN_BREAK;

		MINT_IN_CASE(MINT_LOCALLOC) {
			int len = LOCAL_VAR (ip [2], gint32);
			gpointer mem;
			if (len > 0) {
				// We align len to 8 so we can safely load all primitive types on all platforms
				mem = frame_data_allocator_alloc (&context->data_stack, frame, ALIGN_TO (len, sizeof (gint64)));

				if (frame->imethod->init_locals)
					memset (mem, 0, len);
			} else {
				mem = NULL;
			}
			LOCAL_VAR (ip [1], gpointer) = mem;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_ENDFILTER)
			/* top of stack is result of filter */
			frame->retval->data.i = LOCAL_VAR (ip [1], gint32);
			goto exit_clause;
		MINT_IN_CASE(MINT_ZEROBLK)
			memset (LOCAL_VAR (ip [1], gpointer), 0, LOCAL_VAR (ip [2], gsize));
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ZEROBLK_IMM)
			memset (LOCAL_VAR (ip [1], gpointer), 0, ip [2]);
			ip += 3;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CPBLK) {
			gpointer dest = LOCAL_VAR (ip [1], gpointer);
			gpointer src = LOCAL_VAR (ip [2], gpointer);
			guint32 size = LOCAL_VAR (ip [3], guint32);
			if (size && (!dest || !src))
				THROW_EX (interp_get_exception_null_reference(frame, ip), ip);
			else
				memcpy (dest, src, size);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INITBLK) {
			gpointer dest = LOCAL_VAR (ip [1], gpointer);
			guint32 size = LOCAL_VAR (ip [3], guint32);
			if (size)
				NULL_CHECK (dest);
			memset (dest, LOCAL_VAR (ip [2], gint32), size);
			ip += 4;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_RETHROW) {
			int exvar_offset = ip [1];
			THROW_EX_GENERAL (*(MonoException**)(frame_locals (frame) + exvar_offset), ip, TRUE);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_MONO_RETHROW) {
			/*
			 * need to clarify what this should actually do:
			 *
			 * Takes an exception from the stack and rethrows it.
			 * This is useful for wrappers that don't want to have to
			 * use CEE_THROW and lose the exception stacktrace.
			 */

			MonoException *exc = LOCAL_VAR (ip [1], MonoException*);
			if (!exc)
				exc = interp_get_exception_null_reference (frame, ip);

			THROW_EX_GENERAL (exc, ip, TRUE);
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LD_DELEGATE_METHOD_PTR) {
			// FIXME push/pop LMF
			MonoDelegate *del = LOCAL_VAR (ip [2], MonoDelegate*);
			if (!del->interp_method) {
				/* Not created from interpreted code */
				g_assert (del->method);
				del->interp_method = mono_interp_get_imethod (del->method);
			} else if (((InterpMethod*)del->interp_method)->optimized_imethod) {
				del->interp_method = ((InterpMethod*)del->interp_method)->optimized_imethod;
			}
			g_assert (del->interp_method);
			LOCAL_VAR (ip [1], gpointer) = imethod_to_ftnptr (del->interp_method, FALSE);
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_LDVIRTFTN_DELEGATE) {
			gpointer arg = LOCAL_VAR (ip [2], gpointer);
			MonoDelegate *del = LOCAL_VAR (ip [3], MonoDelegate*);
			NULL_CHECK (arg);

			LOCAL_VAR (ip [1], gpointer) = interp_ldvirtftn_delegate (arg, del);
			ip += 4;
			MINT_IN_BREAK;
		}

#define MATH_UNOP(mathfunc) \
	LOCAL_VAR (ip [1], double) = mathfunc (LOCAL_VAR (ip [2], double)); \
	ip += 3;

#define MATH_BINOP(mathfunc) \
	LOCAL_VAR (ip [1], double) = mathfunc (LOCAL_VAR (ip [2], double), LOCAL_VAR (ip [3], double)); \
	ip += 4;

		MINT_IN_CASE(MINT_ASIN) MATH_UNOP(asin); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ASINH) MATH_UNOP(asinh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ACOS) MATH_UNOP(acos); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ACOSH) MATH_UNOP(acosh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ATAN) MATH_UNOP(atan); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ATANH) MATH_UNOP(atanh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEILING) MATH_UNOP(ceil); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_COS) MATH_UNOP(cos); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CBRT) MATH_UNOP(cbrt); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_COSH) MATH_UNOP(cosh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_EXP) MATH_UNOP(exp); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_FLOOR) MATH_UNOP(floor); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG) MATH_UNOP(log); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG2) MATH_UNOP(log2); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG10) MATH_UNOP(log10); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SIN) MATH_UNOP(sin); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SQRT) MATH_UNOP(sqrt); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SINH) MATH_UNOP(sinh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_TAN) MATH_UNOP(tan); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_TANH) MATH_UNOP(tanh); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ABS) MATH_UNOP(fabs); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_ATAN2) MATH_BINOP(atan2); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_POW) MATH_BINOP(pow); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MIN) MATH_BINOP(min_d); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MAX) MATH_BINOP(max_d); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_FMA)
			LOCAL_VAR (ip [1], double) = fma (LOCAL_VAR (ip [2], double), LOCAL_VAR (ip [3], double), LOCAL_VAR (ip [4], double));
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SCALEB)
			LOCAL_VAR (ip [1], double) = scalbn (LOCAL_VAR (ip [2], double), LOCAL_VAR (ip [3], gint32));
			ip += 4;
			MINT_IN_BREAK;

#define MATH_UNOPF(mathfunc) \
	LOCAL_VAR (ip [1], float) = mathfunc (LOCAL_VAR (ip [2], float)); \
	ip += 3;

#define MATH_BINOPF(mathfunc) \
	LOCAL_VAR (ip [1], float) = mathfunc (LOCAL_VAR (ip [2], float), LOCAL_VAR (ip [3], float)); \
	ip += 4;
		MINT_IN_CASE(MINT_ASINF) MATH_UNOPF(asinf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ASINHF) MATH_UNOPF(asinhf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ACOSF) MATH_UNOPF(acosf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ACOSHF) MATH_UNOPF(acoshf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ATANF) MATH_UNOPF(atanf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ATANHF) MATH_UNOPF(atanhf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CEILINGF) MATH_UNOPF(ceilf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_COSF) MATH_UNOPF(cosf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_CBRTF) MATH_UNOPF(cbrtf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_COSHF) MATH_UNOPF(coshf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_EXPF) MATH_UNOPF(expf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_FLOORF) MATH_UNOPF(floorf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOGF) MATH_UNOPF(logf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG2F) MATH_UNOPF(log2f); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_LOG10F) MATH_UNOPF(log10f); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SINF) MATH_UNOPF(sinf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SQRTF) MATH_UNOPF(sqrtf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SINHF) MATH_UNOPF(sinhf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_TANF) MATH_UNOPF(tanf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_TANHF) MATH_UNOPF(tanhf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_ABSF) MATH_UNOPF(fabsf); MINT_IN_BREAK;

		MINT_IN_CASE(MINT_ATAN2F) MATH_BINOPF(atan2f); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_POWF) MATH_BINOPF(powf); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MINF) MATH_BINOPF(min_f); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_MAXF) MATH_BINOPF(max_f); MINT_IN_BREAK;
		MINT_IN_CASE(MINT_FMAF)
			LOCAL_VAR (ip [1], float) = fmaf (LOCAL_VAR (ip [2], float), LOCAL_VAR (ip [3], float), LOCAL_VAR (ip [4], float));
			ip += 5;
			MINT_IN_BREAK;
		MINT_IN_CASE(MINT_SCALEBF)
			LOCAL_VAR (ip [1], float) = scalbnf (LOCAL_VAR (ip [2], float), LOCAL_VAR (ip [3], gint32));
			ip += 4;
			MINT_IN_BREAK;

		MINT_IN_CASE(MINT_INTRINS_ENUM_HASFLAG) {
			MonoClass *klass = (MonoClass*)frame->imethod->data_items [ip [4]];
			LOCAL_VAR (ip [1], gint32) = mono_interp_enum_hasflag ((stackval*)(locals + ip [2]), (stackval*)(locals + ip [3]), klass);
			ip += 5;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_GET_HASHCODE) {
			LOCAL_VAR (ip [1], gint32) = mono_object_hash_internal (LOCAL_VAR (ip [2], MonoObject*));
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_TRY_GET_HASHCODE) {
			LOCAL_VAR (ip [1], gint32) = mono_object_try_get_hash_internal (LOCAL_VAR (ip [2], MonoObject*));
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_INTRINS_GET_TYPE) {
			MonoObject *o = LOCAL_VAR (ip [2], MonoObject*);
			NULL_CHECK (o);
			LOCAL_VAR (ip [1], MonoObject*) = (MonoObject*) o->vtable->type;
			ip += 3;
			MINT_IN_BREAK;
		}
		MINT_IN_CASE(MINT_METADATA_UPDATE_LDFLDA) {
			MonoObject *inst = LOCAL_VAR (ip [2], MonoObject*);
			MonoType *field_type = frame->imethod->data_items [ip [3]];
			uint32_t fielddef_token = GPOINTER_TO_UINT32 (frame->imethod->data_items [ip [4]]);
			// FIXME: can we emit a call directly instead of a runtime-invoke?
			gpointer field_addr = mono_metadata_update_added_field_ldflda (inst, field_type, fielddef_token, error);
			/* FIXME: think about pinning the FieldStore and adding a second opcode to
			 * unpin it */
			LOCAL_VAR (ip [1], gpointer) = field_addr;
			mono_interp_error_cleanup (error);
			ip += 5;
			MINT_IN_BREAK;
		}

#ifdef HOST_BROWSER
		MINT_IN_CASE(MINT_TIER_NOP_JITERPRETER) {
			ip += JITERPRETER_OPCODE_SIZE;
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_TIER_PREPARE_JITERPRETER) {
			if (mono_opt_jiterpreter_traces_enabled) {
				/*
				 * prepare_jiterpreter will update the trace's hit count and potentially either JIT it or
				 *  disable this entry point based on whether it fails to JIT. the hit counting is necessary
				 *  because a given method may contain many jiterpreter entry points, but some of them will
				 *  not be actually hit often enough to justify the cost of jitting them. (for example, a
				 *  trace that only runs inside an unlikely branch for throwing exceptions.)
				 * thanks to the heuristic that runs during transform.c's codegen, most (95%+) of these
				 *  entry points will JIT successfully, which will keep the number of NOT_JITTED nops low.
				 * note: threading doesn't work yet, we will need to broadcast jitted traces to all of our
				 *  JS workers in order to register them at the appropriate slots in the function pointer
				 *  table. when growing the function pointer table we will also need to synchronize that.
				 */
				JiterpreterThunk prepare_result = mono_interp_tier_prepare_jiterpreter_fast (frame, ip);
				ptrdiff_t offset;
				switch ((guint32)(void*)prepare_result) {
					case JITERPRETER_TRAINING:
						// jiterpreter still updating hit count before deciding to generate a trace,
						//  so skip this opcode.
						ip += JITERPRETER_OPCODE_SIZE;
						break;
					case JITERPRETER_NOT_JITTED:
						// Patch opcode to disable it because this trace failed to JIT.
						if (!mono_opt_jiterpreter_estimate_heat) {
							if (!mono_jiterp_patch_opcode ((volatile JiterpreterOpcode *)ip, MINT_TIER_PREPARE_JITERPRETER, MINT_TIER_NOP_JITERPRETER))
								g_printf ("Failed to patch opcode at %x into a nop\n", (unsigned int)ip);
						}
						ip += JITERPRETER_OPCODE_SIZE;
						break;
					default:
						/*
						 * trace generated. patch opcode to disable it, then write the function
						 *  pointer, then patch opcode again to turn this trace on.
						 * we do this to ensure that other threads won't see an ENTER_JITERPRETER
						 *  opcode that has no function pointer stored inside of it.
						 * (note that right now threading doesn't work, but it's worth being correct
						 *  here so that implementing thread support will be easier later.)
						 */
						if (!mono_jiterp_patch_opcode ((volatile JiterpreterOpcode *)ip, MINT_TIER_PREPARE_JITERPRETER, MINT_TIER_MONITOR_JITERPRETER))
							g_printf ("Failed to patch opcode at %x into a monitor point\n", (unsigned int)ip);
						// now execute the trace
						// this isn't important for performance, but it makes it easier to use the
						//  jiterpreter early in automated tests where code only runs once
						offset = prepare_result (frame, locals, NULL, ip);
						ip = (guint16*) (((guint8*)ip) + offset);
						break;
				}
			} else {
				ip += JITERPRETER_OPCODE_SIZE;
			}

			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_TIER_MONITOR_JITERPRETER) {
			// The trace is in monitoring mode, where we track how far it actually goes
			//  each time it is executed for a while. After N more hits, we either
			//  turn it into an ENTER or a NOP depending on how well it is working
			ptrdiff_t offset = mono_jiterp_monitor_trace (ip, frame, locals);
			ip = (guint16*) (((guint8*)ip) + offset);
			MINT_IN_BREAK;
		}

		MINT_IN_CASE(MINT_TIER_ENTER_JITERPRETER) {
			// The fn ptr is encoded in a guint16 relative to the index of the first trace fn ptr, so compute the actual ptr
			JiterpreterThunk thunk = (JiterpreterThunk)(void *)(((JiterpreterOpcode *)ip)->relative_fn_ptr + mono_jiterp_first_trace_fn_ptr);
			ptrdiff_t offset = thunk (frame, locals, NULL, ip);
			ip = (guint16*) (((guint8*)ip) + offset);
			MINT_IN_BREAK;
		}
#endif

#if !USE_COMPUTED_GOTO
		default:
			interp_error_xsx ("Unimplemented opcode: %04x %s at 0x%x\n", *ip, mono_interp_opname (*ip), GPTRDIFF_TO_INT (ip - frame->imethod->code));
#endif // USE_COMPUTED_GOTO
		}
	}

	g_assert_not_reached ();

resume:
	g_assert (context->has_resume_state);
	g_assert (frame->imethod);

	if (frame == context->handler_frame) {
		/*
		 * When running finally blocks, we can have the same frame twice on the stack. If we have
		 * clause_args information, we need to check whether resuming should happen inside this
		 * finally block, or in some other part of the method, in which case we need to exit.
		 */
		if (clause_args && frame == clause_args->exec_frame && context->handler_ip >= clause_args->end_at_ip) {
			goto exit_clause;
		} else {
			/* Set the current execution state to the resume state in context */
			ip = context->handler_ip;
			/* spec says stack should be empty at endfinally so it should be at the start too */
			locals = (guchar*)frame->stack;
			g_assert (context->exc_gchandle);

			clear_resume_state (context);
			// goto main_loop instead of MINT_IN_DISPATCH helps the compiler and therefore conserves stack.
			// This is a slow/rare path and conserving stack is preferred over its performance otherwise.
			goto main_loop;
		}
	} else if (clause_args && frame == clause_args->exec_frame) {
		/*
		 * This frame doesn't handle the resume state and it is the first frame invoked from EH.
		 * We can't just return to parent. We must first exit the EH mechanism and start resuming
		 * again from the original frame.
		 */
		goto exit_clause;
	}
	// Because we are resuming in another frame, bypassing a normal ret opcode,
	// we need to make sure to reset the localloc stack
	frame_data_allocator_pop (&context->data_stack, frame);
	// fall through
exit_frame:
	g_assert_checked (frame->imethod);

	if (frame->parent && frame->parent->state.ip) {
		/* Return to the main loop after a non-recursive interpreter call */
		//printf ("R: %s -> %s %p\n", mono_method_get_full_name (frame->imethod->method), mono_method_get_full_name (frame->parent->imethod->method), frame->parent->state.ip);
		g_assert_checked (frame->stack);
		frame = frame->parent;
		/*
		 * FIXME We should be able to avoid dereferencing imethod here, if we will have
		 * a param_area and all calls would inherit the same sp, or if we are full coop.
		 */
		context->stack_pointer = (guchar*)frame->stack + frame->imethod->alloca_size;
		LOAD_INTERP_STATE (frame);

		CHECK_RESUME_STATE (context);

		goto main_loop;
	}
exit_clause:
	if (!clause_args)
		context->stack_pointer = (guchar*)frame->stack;

	DEBUG_LEAVE ();
}

#undef SET_TEMP_POINTER

static void
interp_parse_options (const char *options)
{
	char **args, **ptr;

	if (!options)
		return;

	args = g_strsplit (options, ",", -1);
	for (ptr = args; ptr && *ptr; ptr ++) {
		char *arg = *ptr;

		if (strncmp (arg, "jit=", 4) == 0) {
			mono_interp_jit_classes = g_slist_prepend (mono_interp_jit_classes, arg + 4);
		} else if (strncmp (arg, "interp-only=", strlen ("interp-only=")) == 0) {
			mono_interp_only_classes = g_slist_prepend (mono_interp_only_classes, arg + strlen ("interp-only="));
		} else {
			gboolean invert;
			int opt = 0;

			if (*arg == '-') {
				arg++;
				invert = TRUE;
			} else {
				invert = FALSE;
			}

			if (strncmp (arg, "inline", 6) == 0)
				opt = INTERP_OPT_INLINE;
			else if (strncmp (arg, "cprop", 5) == 0)
				opt = INTERP_OPT_CPROP;
			else if (strncmp (arg, "super", 5) == 0)
				opt = INTERP_OPT_SUPER_INSTRUCTIONS;
			else if (strncmp (arg, "bblocks", 7) == 0)
				opt = INTERP_OPT_BBLOCKS;
			else if (strncmp (arg, "tiering", 7) == 0)
				opt = INTERP_OPT_TIERING;
			else if (strncmp (arg, "simd", 4) == 0)
				opt = INTERP_OPT_SIMD;
#if HOST_BROWSER
			else if (strncmp (arg, "jiterp", 6) == 0)
				opt = INTERP_OPT_JITERPRETER;
#endif
			else if (strncmp (arg, "ssa", 3) == 0)
				opt = INTERP_OPT_SSA;
			else if (strncmp (arg, "precise", 7) == 0)
				opt = INTERP_OPT_PRECISE_GC;
			else if (strncmp (arg, "all", 3) == 0)
				opt = ~INTERP_OPT_NONE;

			if (opt) {
				if (invert)
					mono_interp_opt &= ~opt;
				else
					mono_interp_opt |= opt;
			}
		}
	}
	g_strfreev (args);
}

/*
 * interp_set_resume_state:
 *
 *   Set the state the interpreter will continue to execute from after execution returns to the interpreter.
 * If INTERP_FRAME is NULL, that means the exception is caught in an AOTed frame and the interpreter needs to
 * unwind back to AOT code.
 */
static void
interp_set_resume_state (MonoJitTlsData *jit_tls, MonoObject *ex, MonoJitExceptionInfo *ei, MonoInterpFrameHandle interp_frame, gpointer handler_ip)
{
	ThreadContext *context;

	g_assert (jit_tls);
	context = (ThreadContext*)jit_tls->interp_context;
	g_assert (context);

	context->has_resume_state = TRUE;
	context->handler_frame = (InterpFrame*)interp_frame;
	context->handler_ei = ei;
	if (context->exc_gchandle)
		mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = mono_gchandle_new_internal ((MonoObject*)ex, FALSE);
	/* Ditto */
	if (context->handler_frame) {
		if (ei)
			*(MonoObject**)(frame_locals (context->handler_frame) + ei->exvar_offset) = ex;
	}
	context->handler_ip = (const guint16*)handler_ip;
}

static void
interp_get_resume_state (const MonoJitTlsData *jit_tls, gboolean *has_resume_state, MonoInterpFrameHandle *interp_frame, gpointer *handler_ip)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;

	*has_resume_state = context ? context->has_resume_state : FALSE;
	if (!*has_resume_state)
		return;

	*interp_frame = context->handler_frame;
	*handler_ip = (gpointer)context->handler_ip;
}

/*
 * interp_run_finally:
 *
 *   Run the finally clause identified by CLAUSE_INDEX in the interpreter frame given by
 * frame->interp_frame.
 * Return TRUE if the finally clause threw an exception.
 */
static gboolean
interp_run_finally (StackFrameInfo *frame, int clause_index)
{
	InterpFrame *iframe = (InterpFrame*)frame->interp_frame;
	MonoJitExceptionInfo *ei = &iframe->imethod->jinfo->clauses [clause_index];
	ThreadContext *context = get_context ();
	FrameClauseArgs clause_args;
	const guint16 *state_ip;

	memset (&clause_args, 0, sizeof (FrameClauseArgs));
	clause_args.start_with_ip = (const guint16*)ei->handler_start;
	clause_args.end_at_ip = (const guint16*)ei->data.handler_end;
	clause_args.exec_frame = iframe;

	state_ip = iframe->state.ip;
	iframe->state.ip = NULL;

	InterpFrame* const next_free = iframe->next_free;
	iframe->next_free = NULL;

	// this informs MINT_ENDFINALLY to return to EH
	*(guint16**)(frame_locals (iframe) + iframe->imethod->clause_data_offsets [clause_index]) = NULL;

	mono_interp_exec_method (iframe, context, &clause_args);

	iframe->next_free = next_free;
	iframe->state.ip = state_ip;

	if (need_native_unwind (context)) {
		mono_llvm_start_native_unwind ();
		return TRUE;
	}

	if (context->has_resume_state) {
		return TRUE;
	} else {
		return FALSE;
	}
}

/*
 * interp_run_filter:
 *
 *   Run the filter clause identified by CLAUSE_INDEX in the interpreter frame given by
 * frame->interp_frame.
 */
// Do not inline in case order of frame addresses matters.
static MONO_NEVER_INLINE gboolean
interp_run_filter (StackFrameInfo *frame, MonoException *ex, int clause_index, gpointer handler_ip, gpointer handler_ip_end)
{
	InterpFrame *iframe = (InterpFrame*)frame->interp_frame;
	ThreadContext *context = get_context ();
	stackval retval;
	FrameClauseArgs clause_args;

	/*
	 * Have to run the clause in a new frame which is a copy of IFRAME, since
	 * during debugging, there are two copies of the frame on the stack.
	 */
	InterpFrame child_frame = {0};
	child_frame.parent = iframe;
	child_frame.imethod = iframe->imethod;
	child_frame.stack = (stackval*)context->stack_pointer;
	child_frame.retval = &retval;

	/* Copy the stack frame of the original method */
	memcpy (child_frame.stack, iframe->stack, iframe->imethod->locals_size);
	// Write the exception object in its reserved stack slot
	*((MonoException**)((char*)child_frame.stack + iframe->imethod->clause_data_offsets [clause_index])) = ex;
	context->stack_pointer += iframe->imethod->alloca_size;
	g_assert (context->stack_pointer < context->stack_end);

	memset (&clause_args, 0, sizeof (FrameClauseArgs));
	clause_args.start_with_ip = (const guint16*)handler_ip;
	clause_args.end_at_ip = (const guint16*)handler_ip_end;
	clause_args.exec_frame = &child_frame;

	mono_interp_exec_method (&child_frame, context, &clause_args);

	/* Copy back the updated frame */
	memcpy (iframe->stack, child_frame.stack, iframe->imethod->locals_size);

	context->stack_pointer = (guchar*)child_frame.stack;

	if (need_native_unwind (context)) {
		mono_llvm_start_native_unwind ();
		return TRUE;
	}

	/* ENDFILTER stores the result into child_frame->retval */
	return retval.data.i ? TRUE : FALSE;
}

/* Returns TRUE if there is a pending exception */
static gboolean
interp_run_clause_with_il_state (gpointer il_state_ptr, int clause_index, MonoObject *ex, gboolean *filtered)
{
	MonoMethodILState *il_state = (MonoMethodILState*)il_state_ptr;
	MonoMethodSignature *sig;
	ThreadContext *context = get_context ();
	stackval *sp;
	InterpMethod *imethod;
	FrameClauseArgs clause_args;
	ERROR_DECL (error);

	sig = mono_method_signature_internal (il_state->method);
	g_assert (sig);

	imethod = mono_interp_get_imethod (il_state->method);
	if (!imethod->transformed) {
		// In case method is in process of being tiered up, make sure it is compiled
		mono_interp_transform_method (imethod, context, error);
		mono_error_assert_ok (error);
	}

	sp = (stackval*)context->stack_pointer;

	gpointer ret_addr = NULL;

	int findex = 0;
	if (sig->ret->type != MONO_TYPE_VOID) {
		ret_addr = il_state->data [findex];
		findex ++;
	}
	int first_param_index = 0;
	if (sig->hasthis) {
		if (il_state->data [findex])
			sp->data.p = *(gpointer*)il_state->data [findex];
		first_param_index = 1;
		findex ++;
	}

	for (int i = 0; i < sig->param_count; ++i) {
		if (il_state->data [findex]) {
			int arg_offset = get_arg_offset_fast (imethod, NULL, first_param_index + i);
			stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, arg_offset);

			stackval_from_data (sig->params [i], sval, il_state->data [findex], FALSE);
		}
		findex ++;
	}

	/* Allocate frame */
	InterpFrame frame = {0};
	frame.imethod = imethod;
	frame.stack = sp;
	frame.retval = sp;

	int params_size = get_arg_offset_fast (imethod, NULL, first_param_index + sig->param_count);
	context->stack_pointer = (guchar*)ALIGN_TO ((guchar*)sp + params_size, MINT_STACK_ALIGNMENT);
	context->stack_pointer += imethod->alloca_size;
	g_assert (context->stack_pointer < context->stack_end);

	MonoMethodHeader *header = mono_method_get_header_internal (il_state->method, error);
	mono_error_assert_ok (error);

	/* Init locals */
	if (header->num_locals)
		memset (frame_locals (&frame) + imethod->local_offsets [0], 0, imethod->locals_size);
	/* Copy locals from il_state */
	int locals_start = findex;
	for (int i = 0; i < header->num_locals; ++i) {
		if (il_state->data [locals_start + i])
			stackval_from_data (header->locals [i], (stackval*)(frame_locals (&frame) + imethod->local_offsets [i]), il_state->data [locals_start + i], FALSE);
	}

	memset (&clause_args, 0, sizeof (FrameClauseArgs));
	MonoJitExceptionInfo *ei = &imethod->jinfo->clauses [clause_index];
	MonoExceptionEnum clause_type = ei->flags;
	// For filter clauses, if filtered is set, then we run the filter, otherwise we run the catch handler
	if (clause_type == MONO_EXCEPTION_CLAUSE_FILTER && !filtered)
		clause_type = MONO_EXCEPTION_CLAUSE_NONE;

	if (clause_type == MONO_EXCEPTION_CLAUSE_FILTER)
		clause_args.start_with_ip = (const guint16*)ei->data.filter;
	else
		clause_args.start_with_ip = (const guint16*)ei->handler_start;
	if (clause_type == MONO_EXCEPTION_CLAUSE_NONE || clause_type == MONO_EXCEPTION_CLAUSE_FILTER) {
		/* Run until the end */
		clause_args.end_at_ip = NULL;
		clause_args.run_until_end = TRUE;
	} else {
		clause_args.end_at_ip = (const guint16*)ei->data.handler_end;
	}
	clause_args.exec_frame = &frame;

	if (clause_type == MONO_EXCEPTION_CLAUSE_NONE || clause_type == MONO_EXCEPTION_CLAUSE_FILTER)
		*(MonoObject**)(frame_locals (&frame) + imethod->jinfo->clauses [clause_index].exvar_offset) = ex;
	else
		// this informs MINT_ENDFINALLY to return to EH
		*(guint16**)(frame_locals (&frame) + imethod->clause_data_offsets [clause_index]) = NULL;

	/* Set in mono_handle_exception () */
	context->has_resume_state = FALSE;

	mono_interp_exec_method (&frame, context, &clause_args);

	/* Write back args */
	findex = 0;
	if (sig->ret->type != MONO_TYPE_VOID)
		findex ++;
	if (sig->hasthis) {
		// FIXME: This
		findex ++;
	}
	for (int i = 0; i < sig->param_count; ++i) {
		if (il_state->data [findex]) {
			int arg_offset = get_arg_offset_fast (imethod, NULL, first_param_index + i);
			stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, arg_offset);

			stackval_to_data (sig->params [i], sval, il_state->data [findex], FALSE);
		}
		findex ++;
	}
	/* Write back locals */
	for (int i = 0; i < header->num_locals; ++i) {
		if (il_state->data [locals_start + i])
			stackval_to_data (header->locals [i], (stackval*)(frame_locals (&frame) + imethod->local_offsets [i]), il_state->data [locals_start + i], FALSE);
	}
	mono_metadata_free_mh (header);

	if (clause_type == MONO_EXCEPTION_CLAUSE_NONE && ret_addr) {
		stackval_to_data (sig->ret, frame.retval, ret_addr, FALSE);
	} else if (clause_type == MONO_EXCEPTION_CLAUSE_FILTER) {
		g_assert (filtered);
		*filtered = frame.retval->data.i;
	}

	memset (sp, 0, (guint8*)context->stack_pointer - (guint8*)sp);
	context->stack_pointer = (guchar*)sp;

	if (need_native_unwind (context)) {
		mono_llvm_start_native_unwind ();
		return FALSE;
	}

	return context->has_resume_state;
}

typedef struct {
	InterpFrame *current;
} StackIter;

static gpointer
interp_frame_get_ip (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	/*
	 * For calls, state.ip points to the instruction following the call, so we need to subtract
	 * in order to get inside the call instruction range. Other instructions that set the IP for
	 * the rest of the runtime to see, like throws and sdb breakpoints, will need to account for
	 * this subtraction that we are doing here.
	 */
	return (gpointer)(iframe->state.ip - 1);
}

/*
 * interp_frame_iter_init:
 *
 *   Initialize an iterator for iterating through interpreted frames.
 */
static void
interp_frame_iter_init (MonoInterpStackIter *iter, gpointer interp_exit_data)
{
	StackIter *stack_iter = (StackIter*)iter;

	stack_iter->current = (InterpFrame*)interp_exit_data;
}

/*
 * interp_frame_iter_next:
 *
 *   Fill out FRAME with date for the next interpreter frame.
 */
static gboolean
interp_frame_iter_next (MonoInterpStackIter *iter, StackFrameInfo *frame)
{
	StackIter *stack_iter = (StackIter*)iter;
	InterpFrame *iframe = stack_iter->current;

	memset (frame, 0, sizeof (StackFrameInfo));
	/* pinvoke frames doesn't have imethod set */
	while (iframe && !(iframe->imethod && iframe->imethod->code && iframe->imethod->jinfo))
		iframe = iframe->parent;
	if (!iframe)
		return FALSE;

	MonoMethod *method = iframe->imethod->method;
	frame->interp_frame = iframe;
	frame->method = method;
	frame->actual_method = method;
	if (method && ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) || (method->iflags & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)))) {
		frame->native_offset = -1;
		frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
	} else {
		frame->type = FRAME_TYPE_INTERP;
		/* This is the offset in the interpreter IR. */
		frame->native_offset = GPTRDIFF_TO_INT ((guint8*)interp_frame_get_ip (iframe) - (guint8*)iframe->imethod->code);
		if (method && (!method->wrapper_type || method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD))
			frame->managed = TRUE;
	}
	frame->ji = iframe->imethod->jinfo;
	frame->frame_addr = iframe;

	stack_iter->current = iframe->parent;

	return TRUE;
}

static MonoJitInfo*
interp_find_jit_info (MonoMethod *method)
{
	InterpMethod* imethod;

	imethod = lookup_imethod (method);
	if (imethod)
		return imethod->jinfo;
	else
		return NULL;
}

static void
interp_set_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_SEQ_POINT);
	*code = MINT_SDB_BREAKPOINT;
}

static void
interp_clear_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_BREAKPOINT);
	*code = MINT_SDB_SEQ_POINT;
}

static MonoJitInfo*
interp_frame_get_jit_info (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	return iframe->imethod->jinfo;
}

static gpointer
interp_frame_get_arg (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return (char*)iframe->stack + get_arg_offset_fast (iframe->imethod, NULL, pos + iframe->imethod->hasthis);
}

static gpointer
interp_frame_get_local (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return frame_locals (iframe) + iframe->imethod->local_offsets [pos];
}

static gpointer
interp_frame_get_this (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	g_assert (iframe->imethod->hasthis);
	return iframe->stack;
}

static MonoInterpFrameHandle
interp_frame_get_parent (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	return iframe->parent;
}

static void
interp_start_single_stepping (void)
{
	ss_enabled = TRUE;
}

static void
interp_stop_single_stepping (void)
{
	ss_enabled = FALSE;
}


static void
interp_mark_frame_no_ref_slots (ThreadContext *context, InterpFrame *frame, gpointer *top_limit)
{
	InterpMethod *imethod = frame->imethod;
	gpointer *frame_stack = (gpointer*)frame->stack;
	gpointer *frame_stack_end = (gpointer*)((guchar*)frame->stack + imethod->alloca_size);
	// The way interpreter implements calls is by moving arguments to the param area, at the
	// top of the stack and then proceed with the call. Up to the moment of the call these slots
	// are owned by the calling frame. Once we do the call, the stack pointer of the called
	// frame will point inside the param area of the calling frame.
	//
	// We mark no ref slots from top to bottom and we use the top limit to ignore slots
	// that were already handled in the called frame.
	if (top_limit && top_limit < frame_stack_end)
		frame_stack_end = top_limit;

	for (gpointer *current = frame_stack; current < frame_stack_end; current++) {
		gsize slot_index = current - frame_stack;
		if (!mono_bitset_test_fast (imethod->ref_slots, slot_index)) {
			gsize global_slot_index = current - (gpointer*)context->stack_start;
			gsize table_index = global_slot_index / 8;
			int bit_index = global_slot_index % 8;
			context->no_ref_slots [table_index] |= 1 << bit_index;
		}
	}
}

static void
interp_mark_no_ref_slots (ThreadContext *context, MonoLMF* lmf)
{
	memset (context->no_ref_slots, 0, (context->stack_pointer - context->stack_start) / (8 * sizeof (gpointer)) + 1);
	while (lmf) {
		if ((gsize)lmf->previous_lmf & 2) {
			MonoLMFExt *lmf_ext = (MonoLMFExt*) lmf;
			if (lmf_ext->kind == MONO_LMFEXT_INTERP_EXIT || lmf_ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
				InterpFrame *frame = (InterpFrame*)lmf_ext->interp_exit_data;
				gpointer *top_limit = NULL;
				while (frame) {
					if (frame->imethod) {
						interp_mark_frame_no_ref_slots (context, frame, top_limit);
						top_limit = (gpointer*)frame->stack;
					}
					frame = frame->parent;
				}
			}
		}
		lmf = (MonoLMF*)((gsize)lmf->previous_lmf & ~3);
	}
}

/*
 * interp_mark_stack:
 *
 *   Mark the interpreter stack frames for a thread.
 *
 */
static void
interp_mark_stack (gpointer thread_data, GcScanFunc func, gpointer gc_data, gboolean precise)
{
	MonoThreadInfo *info = (MonoThreadInfo*)thread_data;

	if (!mono_use_interpreter)
		return;
	if (precise)
		return;

	/*
	 * We explicitly mark the frames instead of registering the stack fragments as GC roots, so
	 * we have to process less data and avoid false pinning from data which is above 'pos'.
	 *
	 * The stack frame handling code uses compiler write barriers only, but the calling code
	 * in sgen-mono.c already did a mono_memory_barrier_process_wide () so we can
	 * process these data structures normally.
	 */
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)info->tls [TLS_KEY_JIT_TLS];
	if (!jit_tls)
		return;

	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;
	if (!context || !context->stack_start)
		return;

	if (mono_interp_opt & INTERP_OPT_PRECISE_GC) {
		MonoLMF **lmf_addr = (MonoLMF**)info->tls [TLS_KEY_LMF_ADDR];
		if (lmf_addr)
			interp_mark_no_ref_slots (context, *lmf_addr);
	}

	int slot_index = 0;
	for (gpointer *p = (gpointer*)context->stack_start; p < (gpointer*)context->stack_pointer; p++) {
		if (context->no_ref_slots && (context->no_ref_slots [slot_index / 8] & (1 << (slot_index % 8))))
			;// This slot is marked as no ref, we don't scan it
		else
			func (p, gc_data);
		slot_index++;
	}

	FrameDataFragment *frag;
	for (frag = context->data_stack.first; frag; frag = frag->next) {
		// FIXME: Scan the whole area with 1 call
		for (gpointer *p = (gpointer*)&frag->data; p < (gpointer*)frag->pos; ++p)
			func (p, gc_data);
		if (frag == context->data_stack.current)
			break;
	}
}

#if COUNT_OPS

static int
opcode_count_comparer (const void * pa, const void * pb)
{
	long counta = opcode_counts [*(int*)pa];
	long countb = opcode_counts [*(int*)pb];

	if (counta < countb)
		return 1;
	else if (counta > countb)
		return -1;
	else
		return 0;
}

static void
interp_print_op_count (void)
{
	int ordered_ops [MINT_LASTOP];
	int i;
	long total_ops = 0;

	for (i = 0; i < MINT_LASTOP; i++) {
		ordered_ops [i] = i;
		total_ops += opcode_counts [i];
	}
	qsort (ordered_ops, MINT_LASTOP, sizeof (int), opcode_count_comparer);

	g_print ("total ops %ld\n", total_ops);
	for (i = 0; i < MINT_LASTOP; i++) {
		long count = opcode_counts [ordered_ops [i]];
		g_print ("%s : %ld (%.2lf%%)\n", mono_interp_opname (ordered_ops [i]), count, (double)count / total_ops * 100);
	}
}
#endif

#if PROFILE_INTERP

static InterpMethod **imethods;
static int num_methods;
const int opcount_threshold = 100000;

static void
interp_add_imethod (gpointer method, gpointer user_data)
{
	InterpMethod *imethod = (InterpMethod*) method;
	if (imethod->opcounts > opcount_threshold)
		imethods [num_methods++] = imethod;
}

static int
imethod_opcount_comparer (gconstpointer m1, gconstpointer m2)
{
	long diff = (*(InterpMethod**)m2)->opcounts - (*(InterpMethod**)m1)->opcounts;
	if (diff > 0)
		return 1;
	else if (diff < 0)
		return -1;
	else
		return 0;
}

static void
interp_print_method_counts (void)
{
	MonoJitMemoryManager *jit_mm = get_default_jit_mm ();

	jit_mm_lock (jit_mm);
	imethods = (InterpMethod**) malloc (jit_mm->interp_code_hash.num_entries * sizeof (InterpMethod*));
	mono_internal_hash_table_apply (&jit_mm->interp_code_hash, interp_add_imethod, NULL);
	jit_mm_unlock (jit_mm);

	qsort (imethods, num_methods, sizeof (InterpMethod*), imethod_opcount_comparer);

	printf ("Total executed opcodes %ld\n", total_executed_opcodes);
	long cumulative_executed_opcodes = 0;
	for (int i = 0; i < num_methods; i++) {
		cumulative_executed_opcodes += imethods [i]->opcounts;
		printf ("%d%% Opcounts %ld, calls %ld, Method %s, imethod ptr %p\n", (int)(cumulative_executed_opcodes * 100 / total_executed_opcodes), imethods [i]->opcounts, imethods [i]->calls, mono_method_full_name (imethods [i]->method, TRUE), imethods [i]);
	}
}
#endif

static void
interp_set_optimizations (guint32 opts)
{
	mono_interp_opt = opts;
}

static void
invalidate_transform (gpointer imethod_, gpointer user_data)
{
	InterpMethod *imethod = (InterpMethod *) imethod_;
	imethod->transformed = FALSE;
}

static void
copy_imethod_for_frame (InterpFrame *frame)
{
	InterpMethod *copy = (InterpMethod *) m_method_alloc0 (frame->imethod->method, sizeof (InterpMethod));
	memcpy (copy, frame->imethod, sizeof (InterpMethod));
	copy->next_jit_code_hash = NULL; /* we don't want that in our copy */
	frame->imethod = copy;
	/* Note: The copy will be around until the method is unloaded. Ideally we
	 * would reclaim its memory when the corresponding InterpFrame is popped.
	 */
}

static void
metadata_update_backup_frames (MonoThreadInfo *info, InterpFrame *frame)
{
	while (frame) {
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE, "threadinfo=%p, copy imethod for method=%s", info, mono_method_full_name (frame->imethod->method, 1));
		copy_imethod_for_frame (frame);
		frame = frame->parent;
	}
}

static void
metadata_update_prepare_to_invalidate (void)
{
	/* (1) make a copy of imethod for every interpframe that is on the stack,
	 * so we do not invalidate currently running methods */

	FOREACH_THREAD_EXCLUDE (info, MONO_THREAD_INFO_FLAGS_NO_GC) {
		if (!info || !info->jit_data)
			continue;

		MonoLMF *lmf = info->jit_data->lmf;
		while (lmf) {
			if (((gsize) lmf->previous_lmf) & 2) {
				MonoLMFExt *ext = (MonoLMFExt *) lmf;
				if (ext->kind == MONO_LMFEXT_INTERP_EXIT || ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
					InterpFrame *frame = ext->interp_exit_data;
					metadata_update_backup_frames (info, frame);
				}
			}
			lmf = (MonoLMF *)(((gsize) lmf->previous_lmf) & ~3);
		}
	} FOREACH_THREAD_END

	/* (2) invalidate all the registered imethods */
}

static void
interp_invalidate_transformed (void)
{
	gboolean need_stw_restart = FALSE;
        if (mono_metadata_has_updates ()) {
                mono_stop_world (MONO_THREAD_INFO_FLAGS_NO_GC);
                metadata_update_prepare_to_invalidate ();
                need_stw_restart = TRUE;
        }

	GPtrArray *alcs = mono_alc_get_all ();

	if (alcs) {
		MonoAssemblyLoadContext* alc;
		for (guint i = 0; i < alcs->len; ++i) {
			alc = (MonoAssemblyLoadContext*)g_ptr_array_index (alcs, i);
			MonoJitMemoryManager *jit_mm = (MonoJitMemoryManager*)(alc->memory_manager->runtime_info);

			jit_mm_lock (jit_mm);
			mono_internal_hash_table_apply (&jit_mm->interp_code_hash, invalidate_transform, NULL);
			jit_mm_unlock (jit_mm);
		}

		g_ptr_array_free (alcs, TRUE);
	}

	if (need_stw_restart)
		mono_restart_world (MONO_THREAD_INFO_FLAGS_NO_GC);
}


typedef struct {
	MonoJitInfo **jit_info_array;
	gint size;
	gint next;
} InterpCopyJitInfoFuncUserData;

static void
interp_copy_jit_info_func (gpointer imethod, gpointer user_data)
{
	InterpCopyJitInfoFuncUserData *data = (InterpCopyJitInfoFuncUserData*)user_data;
	if (data->next < data->size)
		data->jit_info_array [data->next++] = ((InterpMethod *)imethod)->jinfo;
}

static void
interp_jit_info_foreach (InterpJitInfoFunc func, gpointer user_data)
{
	GPtrArray *alcs = mono_alc_get_all ();

	if (alcs) {
		MonoAssemblyLoadContext* alc;
		for (guint i = 0; i < alcs->len; ++i) {
			alc = (MonoAssemblyLoadContext*)g_ptr_array_index (alcs, i);
			MonoJitMemoryManager *jit_mm = (MonoJitMemoryManager*)(alc->memory_manager->runtime_info);
			InterpCopyJitInfoFuncUserData copy_jit_info_data;
			// Can't keep memory manager lock while iterating and calling callback since it might take other locks
			// causing poential deadlock situations. Instead, create copy of interpreter imethod jinfo pointers into
			// plain array and use pointers from array when when running callbacks.
			copy_jit_info_data.size = mono_atomic_load_i32 (&(jit_mm->interp_code_hash.num_entries));
			copy_jit_info_data.next = 0;
			copy_jit_info_data.jit_info_array = (MonoJitInfo**) g_new (MonoJitInfo*, copy_jit_info_data.size);
			if (copy_jit_info_data.jit_info_array) {
				jit_mm_lock (jit_mm);
				mono_internal_hash_table_apply (&jit_mm->interp_code_hash, interp_copy_jit_info_func, &copy_jit_info_data);
				jit_mm_unlock (jit_mm);
			}

			if (copy_jit_info_data.jit_info_array) {
				for (int j = 0; j < copy_jit_info_data.next; ++j)
					func (copy_jit_info_data.jit_info_array [j], user_data);
				g_free (copy_jit_info_data.jit_info_array);
			}
		}

		g_ptr_array_free (alcs, TRUE);
	}
}

static gboolean
interp_sufficient_stack (gsize size)
{
	ThreadContext *context = get_context ();

	return (context->stack_pointer + size) < (context->stack_start + INTERP_STACK_SIZE);
}

static void
interp_cleanup (void)
{
#if COUNT_OPS
	interp_print_op_count ();
#endif
#if PROFILE_INTERP
	interp_print_method_counts ();
#endif
}

#undef MONO_EE_CALLBACK
#define MONO_EE_CALLBACK(ret, name, sig) interp_ ## name,

static const MonoEECallbacks mono_interp_callbacks = {
	MONO_EE_CALLBACKS
};

void
mono_ee_interp_init (const char *opts)
{
	g_assert (mono_ee_api_version () == MONO_EE_API_VERSION);
	g_assert (!interp_init_done);
	interp_init_done = TRUE;

	mono_native_tls_alloc (&thread_context_id, NULL);
	set_context (NULL);

	interp_parse_options (opts);

	const char *env_opts = g_getenv ("MONO_INTERPRETER_OPTIONS");
	if (env_opts)
		interp_parse_options (env_opts);
	/* Don't do any optimizations if running under debugger */
	if (mini_get_debug_options ()->mdb_optimizations)
		mono_interp_opt = 0;
	mono_interp_transform_init ();

	if (mono_interp_opt & INTERP_OPT_TIERING)
		mono_interp_tiering_init ();

	mini_install_interp_callbacks (&mono_interp_callbacks);

#ifdef HOST_WASI
	debugger_enabled = mini_get_debug_options ()->mdb_optimizations;
#endif
}

#ifdef HOST_BROWSER
EMSCRIPTEN_KEEPALIVE void
mono_jiterp_stackval_to_data (MonoType *type, stackval *val, void *data)
{
	stackval_to_data (type, val, data, FALSE);
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_stackval_from_data (MonoType *type, stackval *result, const void *data)
{
	stackval_from_data (type, result, data, FALSE);
}

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	return get_arg_offset_fast (imethod, sig, index);
}

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_overflow_check_i4 (gint32 lhs, gint32 rhs, int opcode)
{
	switch (opcode) {
		case MINT_MUL_OVF_I4:
			if (CHECK_MUL_OVERFLOW (lhs, rhs))
				return 1;
		break;
		case MINT_ADD_OVF_I4:
			if (CHECK_ADD_OVERFLOW (lhs, rhs))
				return 1;
		break;
	}

	return 0;
}

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_overflow_check_u4 (guint32 lhs, guint32 rhs, int opcode)
{
	switch (opcode) {
		case MINT_MUL_OVF_UN_I4:
			if (CHECK_MUL_OVERFLOW_UN (lhs, rhs))
				return 1;
		break;
		case MINT_ADD_OVF_UN_I4:
			if (CHECK_ADD_OVERFLOW_UN (lhs, rhs))
				return 1;
		break;
	}

	return 0;
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_ld_delegate_method_ptr (gpointer *destination, MonoDelegate **source)
{
	MonoDelegate *del = *source;
	if (!del->interp_method) {
		/* Not created from interpreted code */
		g_assert (del->method);
		del->interp_method = mono_interp_get_imethod (del->method);
	} else if (((InterpMethod*)del->interp_method)->optimized_imethod) {
		del->interp_method = ((InterpMethod*)del->interp_method)->optimized_imethod;
	}
	g_assert (del->interp_method);
	*destination = imethod_to_ftnptr (del->interp_method, FALSE);
}

MONO_ALWAYS_INLINE void
mono_jiterp_check_pending_unwind (ThreadContext *context)
{
	if (need_native_unwind (context)) {
		// FIXME: Caller needs to check this
		if (mono_opt_llvm_emulate_unwind)
			g_assert_not_reached ();
		mono_llvm_start_native_unwind ();
	}
}

MONO_ALWAYS_INLINE void *
mono_jiterp_get_context (void)
{
	return get_context ();
}

MONO_ALWAYS_INLINE gpointer
mono_jiterp_frame_data_allocator_alloc (FrameDataAllocator *stack, InterpFrame *frame, int size)
{
	return frame_data_allocator_alloc(stack, frame, size);
}

// NOTE: This does not perform a null check and passing a null object or klass is an error!
MONO_ALWAYS_INLINE gboolean
mono_jiterp_isinst (MonoObject* object, MonoClass* klass)
{
	return mono_interp_isinst (object, klass);
}

// after interp_entry_prologue the wrapper will set up all the argument values
//  in the correct place and compute the stack offset, then it passes that in to this
//  function in order to actually enter the interpreter and process the return value
EMSCRIPTEN_KEEPALIVE void
mono_jiterp_interp_entry (void *res)
{
	JiterpEntryDataHeader header;
	MonoType *type;

	// Copy the thread-local header into a local variable. This is necessary for us to be
	//  reentrant-safe because mono_interp_exec_method could end up hitting the trampoline
	//  again on the same thread.
	header = mono_jiterp_get_interp_entry_data ()->header;

	g_assert(header.rmethod);
	g_assert(header.rmethod->method);

	stackval *sp = (stackval*)header.context->stack_pointer;

	InterpFrame frame = {0};
	frame.imethod = header.rmethod;
	frame.stack = sp;
	frame.retval = sp;

	int params_size = get_arg_offset_fast (header.rmethod, NULL, header.params_count);
	// g_printf ("jiterp_interp_entry: rmethod=%d, params_count=%d, params_size=%d\n", header.rmethod, header.params_count, params_size);
	header.context->stack_pointer = (guchar*)ALIGN_TO ((guchar*)sp + params_size, MINT_STACK_ALIGNMENT);

	g_assert (header.context->stack_pointer < header.context->stack_end);

	MONO_ENTER_GC_UNSAFE;
#if HOST_BROWSER
	/* wasm-JIT e-slot redirect for the JITERPRETER's native->interp entry. This is the entry the
	 * jiterpreter compiles for methods called from native/AOT/wasm-JITted code (the "interp_entries" in the
	 * jiterp stats), and it is a SEPARATE path from interp_entry() — so a wasm-JITted target reached this way
	 * ran its INTERP copy despite slot>0/live. That is how every IKVM lambda / functional-interface delegate
	 * is invoked (Java lambda -> .NET delegate -> jiterp interp-entry): Consumer.accept, ToLongFunction.
	 * applyAsLong — the dominant hot entry-edges, invisible to the interp_entry redirect AND every C call
	 * opcode (confirmed: they surfaced only at the universal mono_interp_exec_method probe, as root frames,
	 * live=1). The jiterp entry-prologue already marshalled this+scalar args onto the GC-scanned interp stack
	 * at frame.stack (this at 0, each arg at +8 — get_arg_offset_fast), exactly the e-thunk's args_ptr layout;
	 * the e-thunk writes the result back at frame.stack and the shared tail's mono_jiterp_stackval_to_data
	 * marshals it to `res`, and a thrown callee's resume-state is handled by the has_resume_state tail below —
	 * identical to the mono_interp_exec_method path. Gated by MONO_WASM_JIT_ENTRY_REDIRECT (=0 reverts). */
	{
		extern int mono_wasm_jit_entry_redirect;
		InterpMethod *wj_rm = header.rmethod;
		gboolean wj_dispatched = FALSE;
		if (mono_wasm_jit_entry_redirect && G_UNLIKELY (wj_rm->wasm_jit_slot > 0) && !wj_rm->is_invoke) {
			extern void mono_wasm_jit_sync_thread (void);
			extern int mono_wasm_jit_slot_live (int slot);
			mono_wasm_jit_sync_thread ();
			if (G_LIKELY (mono_wasm_jit_slot_live (wj_rm->wasm_jit_slot))) {
				extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
				mono_wasm_jit_invoke_caught (wj_rm->method, (gint32) wj_rm->wasm_jit_slot, frame.stack, frame.stack);
				wj_dispatched = TRUE;
			}
		}
		if (!wj_dispatched)
			mono_interp_exec_method (&frame, header.context, NULL);
	}
#else
	mono_interp_exec_method (&frame, header.context, NULL);
#endif
	MONO_EXIT_GC_UNSAFE;

	header.context->stack_pointer = (guchar*)sp;

	if (header.rmethod->needs_thread_attach)
		mono_threads_detach_coop (header.orig_domain, &header.attach_cookie);

	mono_jiterp_check_pending_unwind (header.context);

	if (mono_llvm_only) {
		if (header.context->has_resume_state) {
			/* The exception will be handled in a frame above us */
			mono_llvm_start_native_unwind ();
			return;
		}
	} else {
#ifdef TARGET_WASM
		if (header.context->has_resume_state) {
			/* Same as interp_entry (): return so outer interp can resume EH. */
			return;
		}
#endif
		g_assert (!header.context->has_resume_state);
	}

	// The return value is at the bottom of the stack, after the locals space
	type = header.rmethod->rtype;
	if (type->type != MONO_TYPE_VOID)
		mono_jiterp_stackval_to_data (type, frame.stack, res);
}

EMSCRIPTEN_KEEPALIVE volatile size_t *
mono_jiterp_get_polling_required_address ()
{
	return &mono_polling_required;
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_prof_enter (InterpFrame *frame, guint16 *ip)
{
	guint16 flag = ip [1];
	INTERP_PROFILER_RAISE(enter, ENTER);
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_prof_samplepoint (InterpFrame *frame, guint16 *ip)
{
	guint16 flag = ip [1];
	INTERP_PROFILER_RAISE_SAMPLEPOINT();
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_prof_leave (InterpFrame *frame, guint16 *ip)
{
	gboolean is_void = ip [0] == MINT_PROF_EXIT_VOID;
	guint16 flag = is_void ? ip [1] : ip [2];
	INTERP_PROFILER_RAISE(leave, LEAVE);
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_do_safepoint (InterpFrame *frame, guint16 *ip)
{
	do_safepoint (frame, get_context(), ip);
}

EMSCRIPTEN_KEEPALIVE gpointer
mono_jiterp_imethod_to_ftnptr (InterpMethod *imethod)
{
	return imethod_to_ftnptr (imethod, FALSE);
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_enum_hasflag (MonoClass *klass, gint32 *dest, stackval *sp1, stackval *sp2)
{
	*dest = mono_interp_enum_hasflag (sp1, sp2, klass);
}

EMSCRIPTEN_KEEPALIVE gpointer
mono_jiterp_get_simd_intrinsic (int arity, int index)
{
#ifdef INTERP_ENABLE_SIMD
	switch (arity) {
		case 1:
			return interp_simd_p_p_table [index];
		case 2:
			return interp_simd_p_pp_table [index];
		case 3:
			return interp_simd_p_ppp_table [index];
		default:
			g_assert_not_reached();
	}
#else
	g_assert_not_reached();
#endif
}

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_get_simd_opcode (int arity, int index)
{
#ifdef INTERP_ENABLE_SIMD
	switch (arity) {
		case 1:
			return interp_simd_p_p_wasm_opcode_table [index];
		case 2:
			return interp_simd_p_pp_wasm_opcode_table [index];
		case 3:
			return interp_simd_p_ppp_wasm_opcode_table [index];
		default:
			g_assert_not_reached();
	}
#else
	g_assert_not_reached();
#endif
}

#define JITERP_OPINFO_TYPE_NAME 0
#define JITERP_OPINFO_TYPE_LENGTH 1
#define JITERP_OPINFO_TYPE_SREGS 2
#define JITERP_OPINFO_TYPE_DREGS 3
#define JITERP_OPINFO_TYPE_OPARGTYPE 4

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_get_opcode_info (int opcode, int type)
{
	g_assert ((opcode >= 0) && (opcode <= MINT_LASTOP));
	switch (type) {
		case JITERP_OPINFO_TYPE_NAME:
			// We know this conversion is safe because wasm pointers are 32 bits
			return (int)(void*)(mono_interp_opname (opcode));
		case JITERP_OPINFO_TYPE_LENGTH:
			return mono_interp_oplen [opcode];
		case JITERP_OPINFO_TYPE_SREGS:
			return mono_interp_op_sregs [opcode];
		case JITERP_OPINFO_TYPE_DREGS:
			return mono_interp_op_dregs [opcode];
		case JITERP_OPINFO_TYPE_OPARGTYPE:
			return mono_interp_opargtype [opcode];
		default:
			g_assert_not_reached();
	}
}

EMSCRIPTEN_KEEPALIVE int
mono_jiterp_placeholder_trace (void *_frame, void *pLocals, JiterpreterCallInfo *cinfo, const guint16 *ip)
{
	// If this is hit it most likely indicates that a trace is being invoked from a thread
	//  that has not jitted it yet. We want to jit it on this thread and install it at the
	//  correct location in the function pointer table.
	const JiterpreterOpcode *opcode = (const JiterpreterOpcode *)ip;
	if (opcode->relative_fn_ptr) {
		int fn_ptr = opcode->relative_fn_ptr + mono_jiterp_first_trace_fn_ptr;
		InterpFrame *frame = _frame;
		MonoMethod *method = frame->imethod->method;
		const guint16 *start_of_body = frame->imethod->jinfo->code_start;
		int size_of_body = frame->imethod->jinfo->code_size;
		// g_printf ("mono_jiterp_placeholder_trace index=%d fn_ptr=%d ip=%x\n", opcode->trace_index, fn_ptr, ip);
		mono_interp_tier_prepare_jiterpreter (
			frame, method, ip, (gint32)opcode->trace_index,
			start_of_body, size_of_body, frame->imethod->is_verbose,
			fn_ptr
		);
	}
	// advance past the enter/monitor opcode and return to interp
	return mono_interp_oplen [MINT_TIER_ENTER_JITERPRETER] * 2;
}

EMSCRIPTEN_KEEPALIVE void
mono_jiterp_placeholder_jit_call (void *ret_sp, void *sp, void *ftndesc, gboolean *thrown)
{
	// g_print ("mono_jiterp_placeholder_jit_call\n");
	*thrown = 999;
}

EMSCRIPTEN_KEEPALIVE void *
mono_jiterp_get_interp_entry_func (int table)
{
	g_assert (table <= JITERPRETER_TABLE_LAST);

	if (table >= JITERPRETER_TABLE_INTERP_ENTRY_INSTANCE_RET_0)
		return entry_funcs_instance_ret [table - JITERPRETER_TABLE_INTERP_ENTRY_INSTANCE_RET_0];
	else if (table >= JITERPRETER_TABLE_INTERP_ENTRY_INSTANCE_0)
		return entry_funcs_instance [table - JITERPRETER_TABLE_INTERP_ENTRY_INSTANCE_0];
	else if (table >= JITERPRETER_TABLE_INTERP_ENTRY_STATIC_RET_0)
		return entry_funcs_static_ret [table - JITERPRETER_TABLE_INTERP_ENTRY_STATIC_RET_0];
	else if (table >= JITERPRETER_TABLE_INTERP_ENTRY_STATIC_0)
		return entry_funcs_static [table - JITERPRETER_TABLE_INTERP_ENTRY_STATIC_0];
	else
		g_assert_not_reached ();
}

#endif
