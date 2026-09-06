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
#include <mono/metadata/object-internals.h>   /* MONO_IMT_SIZE + mono_method_get_imt_slot for the vcall fast-miss slot (mono_wasm_jit_vcall_resolve_fslot) */
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
 * Island/transition diagnostics (Part 3). Bounded, open-addressed lock-free tables. Slot ownership
 * and counters are atomic; publication markers are written last so main-thread dumps never observe
 * partially initialized names. Edge tracking remains part of MONO_WASM_JIT_STATS because the named
 * crossings are used to drive upward island growth and eliminate transition-heavy interpreter callers.
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
	volatile gint32 state;                  /* 0 empty, 1 initializing, 2 published */
	MonoMethod *caller, *callee;          /* hash key */
	guint32 count, window;
	InterpMethod *caller_im;              /* cached: dump reads slot/hits/bail via a plain deref (no lock) */
	char *caller_name, *callee_name;      /* cached full names (g_malloc'd at insertion; never freed) */
} WjEntryEdge;
static WjEntryEdge wj_entry_edges [WJ_EDGE_SLOTS];
static volatile gint32 wj_entry_edge_topology;

static void
wj_edge_bump (InterpMethod *caller_im, InterpMethod *callee_im)
{
	MonoMethod *caller = caller_im->method, *callee = callee_im->method;
	gsize h = (((gsize) caller >> 4) ^ ((gsize) callee >> 4)) & (WJ_EDGE_SLOTS - 1);
	int i;
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		WjEntryEdge *e = &wj_entry_edges [(h + i) & (WJ_EDGE_SLOTS - 1)];
		if (e->state == 2 && e->callee == callee && e->caller == caller) { mono_atomic_inc_i32 ((volatile gint32 *)&e->count); mono_atomic_inc_i32 ((volatile gint32 *)&e->window); return; }
		if (e->state == 0 && mono_atomic_cas_i32 (&e->state, 1, 0) == 0) {
			/* first time we see this edge: cache names + caller imethod now (coop thread, safe to lock) */
			e->caller = caller; e->callee = callee; e->caller_im = caller_im;
			e->caller_name = mono_method_get_full_name (caller);
			e->callee_name = mono_method_get_full_name (callee);
			e->count = 1; e->window = 1;
			mono_memory_barrier ();
			mono_atomic_xchg_i32 (&e->state, 2);   /* publish occupied last */
			mono_atomic_inc_i32 (&wj_entry_edge_topology);
			return;
		}
		if (e->state == 1)
			return;   /* an initialization race may drop one sample, but can never duplicate/corrupt a slot */
	}
	/* table full / long probe chain: drop the sample (approximate is fine for a bench histogram) */
}

#define WJ_BLOCK_SLOTS 4096
static MonoMethod *wj_block_tab [WJ_BLOCK_SLOTS];
static volatile gint32 wj_block_state [WJ_BLOCK_SLOTS]; /* 0 empty, 1 initializing, 2 published */
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
	gint32 block_n = mono_atomic_inc_i32 (&cim->wasm_jit_block_n);
	{
		extern int mono_wasm_jit_block_force;
		if (mono_wasm_jit_block_force > 0 && cim->wasm_jit_fslot <= 0 && cim->wasm_jit_slot != -1
		    && block_n == mono_wasm_jit_block_force)
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
			if (wj_block_state [idx] == 2 && *s == callee) { mono_atomic_inc_i32 ((volatile gint32 *)&wj_block_cnt [idx]); return; }
			if (wj_block_state [idx] == 0 && mono_atomic_cas_i32 (&wj_block_state [idx], 1, 0) == 0) {
				/* first time: cache name + imethod now (coop compile thread, safe to lock) */
				wj_block_im [idx] = cim;
				wj_block_name [idx] = mono_method_get_full_name (callee);
				wj_block_cnt [idx] = 1;
				mono_memory_barrier ();
				*s = callee;
				mono_atomic_xchg_i32 (&wj_block_state [idx], 2);   /* publish last */
				return;
			}
			if (wj_block_state [idx] == 1)
				return;
		}
	}
}

/* wj_vperm_tab: execution-WEIGHTED registry of permanently-unjittable vcall-residual callees. The
 * WJC_VPERM_* counters give the per-REASON weights; this table gives the per-METHOD weights behind
 * them, so "gshared=800K" resolves to the handful of hot methods (and their exact emitter gate,
 * wasm_jit_fail) that carry it. Bumped per residual execution from the vcall resolve path (a coop
 * thread — names cached at record time per the main-thread rule above); the dump only reads memory. */
#define WJ_VPERM_SLOTS 2048
static InterpMethod *wj_vperm_im [WJ_VPERM_SLOTS];
static gint64 wj_vperm_w [WJ_VPERM_SLOTS];
static char *wj_vperm_name [WJ_VPERM_SLOTS];
static volatile gint32 wj_vperm_state [WJ_VPERM_SLOTS]; /* 0 empty, 1 initializing, 2 published */

static void
wj_vperm_note (InterpMethod *im)
{
	gsize h = ((gsize) im >> 4) & (WJ_VPERM_SLOTS - 1);
	int i;
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		int idx = (h + i) & (WJ_VPERM_SLOTS - 1);
		if (wj_vperm_state [idx] == 2 && wj_vperm_im [idx] == im) { mono_atomic_inc_i64 (&wj_vperm_w [idx]); return; }
		if (wj_vperm_state [idx] == 0 && mono_atomic_cas_i32 (&wj_vperm_state [idx], 1, 0) == 0) {
			char *name = mono_method_get_full_name (im->method);
			wj_vperm_w [idx] = 1;
			wj_vperm_name [idx] = name;
			wj_vperm_im [idx] = im;
			mono_atomic_xchg_i32 (&wj_vperm_state [idx], 2);   /* publish occupied last */
			return;
		}
		if (wj_vperm_state [idx] == 1)
			return;
	}
	/* table full / long probe chain: drop the sample (approximate is fine for a bench histogram) */
}

/* wj_iroute_tab: the same weighted shape as wj_vperm_tab, for the INTERP-ROUTED population — residual
 * calls whose callee has neither AOT code nor a wasm-JIT f-slot, so they pay full interp_entry marshalling
 * and then run in the interpreter. The aggregate is stark (38.5M interp-routed vs 2.6M aot-routed in one
 * session, and 7.1M vs 359K inside a single in-game window) but a single number cannot be acted on: it does
 * not say whether it is a few hot methods or a long tail, nor WHY each callee is un-JITted. This resolves
 * it to named methods with their bail reason, which is what makes the interp-routing item actionable at
 * all. Bumped only under MONO_WASM_JIT_STATS, so it costs nothing in a timing run. */
/* RETRY-STATE ATTRIBUTION. `vfb_thresh` is 90% `retry` (slot -3, the compile-lock back-off): measured
 * 9,410,099 of 10,476,605 in one window. That is an EVENT count, and an event count cannot distinguish
 * A FEW METHODS LIVELOCKING from MANY CYCLING NORMALLY -- which need opposite fixes, and which is the
 * same "no denominator" hole that produced three wrong re-emission explanations before
 * the re-emission drain's own distinct-method denominator settled it. Same shape as wj_iroute_* below:
 * hash by InterpMethod, cache the name
 * at record time so the main-thread dump takes no lock. */
#define WJ_RETRY_SLOTS 1024
static InterpMethod *wj_retry_im [WJ_RETRY_SLOTS];
static gint64 wj_retry_w [WJ_RETRY_SLOTS];
static char *wj_retry_name [WJ_RETRY_SLOTS];
static volatile gint32 wj_retry_state [WJ_RETRY_SLOTS];

#define WJ_IROUTE_SLOTS 2048
static InterpMethod *wj_iroute_im [WJ_IROUTE_SLOTS];
static gint64 wj_iroute_w [WJ_IROUTE_SLOTS];
static char *wj_iroute_name [WJ_IROUTE_SLOTS];
static volatile gint32 wj_iroute_state [WJ_IROUTE_SLOTS];

static void
wj_retry_note (InterpMethod *im)
{
	gsize h = ((gsize) im >> 4) & (WJ_RETRY_SLOTS - 1);
	int i;
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		int idx = (h + i) & (WJ_RETRY_SLOTS - 1);
		if (wj_retry_state [idx] == 2 && wj_retry_im [idx] == im) { mono_atomic_inc_i64 (&wj_retry_w [idx]); return; }
		if (wj_retry_state [idx] == 0 && mono_atomic_cas_i32 (&wj_retry_state [idx], 1, 0) == 0) {
			char *name = mono_method_get_full_name (im->method);
			wj_retry_w [idx] = 1;
			wj_retry_name [idx] = name;
			wj_retry_im [idx] = im;
			mono_atomic_xchg_i32 (&wj_retry_state [idx], 2);
			return;
		}
		if (wj_retry_state [idx] == 1)
			return;
	}
}

static void
wj_iroute_note (InterpMethod *im)
{
	gsize h = ((gsize) im >> 4) & (WJ_IROUTE_SLOTS - 1);
	int i;
	for (i = 0; i < WJ_EDGE_PROBE; ++i) {
		int idx = (h + i) & (WJ_IROUTE_SLOTS - 1);
		if (wj_iroute_state [idx] == 2 && wj_iroute_im [idx] == im) { mono_atomic_inc_i64 (&wj_iroute_w [idx]); return; }
		if (wj_iroute_state [idx] == 0 && mono_atomic_cas_i32 (&wj_iroute_state [idx], 1, 0) == 0) {
			char *name = mono_method_get_full_name (im->method);
			wj_iroute_w [idx] = 1;
			wj_iroute_name [idx] = name;
			wj_iroute_im [idx] = im;
			mono_atomic_xchg_i32 (&wj_iroute_state [idx], 2);
			return;
		}
		if (wj_iroute_state [idx] == 1)
			return;
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

/* --- RE-EMISSION IS DELETED. Its ring, pin, drain, four knobs and thirteen counters lived here. ------
 *
 * The premise was sound and is still true: a method is emitted ONCE, at its hotness threshold, and the
 * devirt gate reads whatever the call profile held AT THAT MOMENT -- while 78.8% of profile observations
 * arrive later, through the JIT's own IC miss path. So codegen sees the coldest profile the method will
 * ever have, and re-emitting with a matured one demonstrably recovers `no_rec` sites (R170b: no_rec 31.2%
 * -> 0.0% on re-emitted bodies; R209: coverage +19.4 pts with DEVIRT_ARM2 as the consumer).
 *
 * It was deleted on MEASUREMENT plus a structural ceiling, not on "no effect seen", which alone would be
 * weak:
 *
 *  - CONTROL-BRACKETED ARM (ctl -> reemit_ic=0 -> ctl, nothing else on the box between arms). The arm
 *    carries a ~+7% global offset, so every number is normalised against a negative control. NOT ONE
 *    TARGET RISES with re-emission off: vcall_resolve_fslot 0.7843 / **0.7183** / 0.7285,
 *    get_virtual_method_fast 0.1030 / **0.0959** / 0.0865, wj_prof_record_at 0.1357 / **0.1274** /
 *    0.1113, with realize_glenv (2nd negative control) 0.3163 / 0.3048 / 0.3028 confirming the
 *    normalisation works. The point estimate on the miss path is marginally BETTER without it, which is
 *    backwards from the mechanism.
 *  - REACH CEILING ~1,100 of ~28,000 methods (4%), measured by adding the distinct-method denominator
 *    that four successive wrong throughput stories lacked. A 10x lower trigger bought +51%, not 10x.
 *    ~1,008 arms against ~13,000.
 *  - It is a large concurrent subsystem with a documented wedge history: R174's five arms and R208/R209
 *    both trace to the torn read at the interp->JIT entry gate (admit_live on the descriptor, then a
 *    separately-read slot, which re-emission republishes -> a thread enters a slot it never instantiated
 *    and hits the jiterpreter prefill -> `function signature mismatch`). That gate's snapshot+slot_live
 *    fix is KEPT: it is correct independently of re-emission.
 *  - DEVIRT_ARM2, which supplied most of the shipped gain in the stack they were measured in, is
 *    independent of it and stays.
 *
 * What goes with it, and why each was independently worthless:
 *   REEMIT       the original invoke_in trigger. Structurally INERT in any shipped config: it read
 *                cmethod->wasm_jit_invoke_in, which is incremented only inside the mono_wasm_jit_stats
 *                guard one line below it and is documented "stats only".
 *   REEMIT_AFTER / REEMIT_AGE   drain gates. AGE measured a red herring: +9.5% `done` for 4.6x churn.
 *   PRETIER      the residual-site tiering bump. Inert: -2.2% residual_healed.
 *   HEAL_WAIT    late-f-slot healing waiters. The MECHANISM fired (90 of 107 sites woken) and the effect
 *                was nil, ceiling 0.057%. Note the healing SITES themselves are NOT deleted -- R195
 *                establishes that residual healing is also the tiering edge for residual-only callees,
 *                so mono_wasm_jit_late_fslot stays; only the re-emission-based waiter goes.
 *
 * WHAT THIS COSTS, stated plainly so it is not rediscovered: `no_rec` is 13,389 sites, 30.5% of all
 * sites and 32.2% of hot IC execution, and re-emission was its ONLY proposed collector -- raising the
 * JIT threshold structurally cannot reach it, because 99.3% of profile observations arrive after the
 * method is JITted. So no_rec now has no collector at all. The candidate to become one is GUARDED CHA
 * as a prediction source: the devirt arm is already guarded, so the guard IS the invalidation and CHA
 * being wrong costs a fallthrough rather than correctness. Size it before building it (count no_rec
 * sites with exactly one loaded implementor) rather than reviving this. */

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

/*
 * THE CALL PROFILE. One record, one writer, every devirt and call-edge consumer reading it.
 *
 * There used to be two systems answering the same question with the same key. The interpreter's
 * `WjVProf` recorded (caller, base) -> (receiver, target) from MINT_CALLVIRT_FAST, and stopped the moment
 * the caller was JITted. The JIT's own per-thread PIC then recorded the same thing for the same sites,
 * from the emitted code's IC-miss path, and was enumerated separately (`mono_wasm_jit_vcall_profile_*`)
 * by the batch planner, which reconciled the two with hand-picked multipliers because they were not
 * commensurable. Neither could see the other's observations, and the *better* data -- post-JIT,
 * target-resolved, still updating -- never reached the emitter at all.
 *
 * They are one record now. What that buys, concretely:
 *
 *   - EVERY observation point writes here: the interpreter's virtual dispatch and delegate dispatch, AND
 *     the JITted code's IC miss (wj_vcall_pic_publish). So a re-emission sees what the compiled code
 *     learned, which is the only way emit-time devirtualisation can improve on its first guess.
 *   - The `caller->wasm_jit_slot != 0` recording gate is GONE. It existed only because the record was
 *     pre-JIT-only; once shared there is no reason to stop, and WJ_PROF_SATURATE still bounds the cost.
 *   - `site_id` IS STABLE. It is allocated once, on the record, and every emission of that call site
 *     reuses it -- so a re-emit inherits its predecessor's worker-local PIC entries instead of starting
 *     empty, and stops burning ids out of WJ_VCALL_SITE_MAX. wj_auto_batch_poll recorded the old
 *     behaviour as a defect in its own comments: "a failed batch re-emit allocates fresh (initially
 *     empty) sites, making retries increasingly expensive". Under a continuous re-batcher that is fatal
 *     rather than untidy.
 *
 * WHAT DELIBERATELY STAYS SEPARATE, so nobody "finishes the job" by mistake:
 *
 *   - `wj_vcall_pic` / `wj_delegate_pic` stay `__thread` and stay the thing the emitted code loads
 *     inline. They are DISPATCH STATE, not a profile, and they must be per-thread because f-slot
 *     installation is per-thread (this is exactly why R132's process-wide wj_slot_is_installed guard
 *     could not work against a per-thread table). Only the MISS path feeds this record, so the hit path
 *     is still a pure TLS load with no added instruction -- load-bearing, since R85/R88 measured that
 *     merely HAVING a call at a dispatch site forces caller-saved spills, and R114's vcall_ways 4->1
 *     (+9.6% fps tail) was the biggest single win of that session by removing dispatch work.
 *   - `wj_entry_edges` stays. It looks like a third profile and is not: it counts interp->JIT
 *     TRANSITIONS keyed (caller, callee), caches full method names at record time so the main-thread JS
 *     dump can read it without taking a lock, and carries per-window counts for the top-N report. Folding
 *     it in would consume this record's bounded site slots with observations that answer a different
 *     question, and would change which virtual sites survive eviction -- i.e. change emitted code. It
 *     belongs with the planner rewrite, not here.
 *
 * Keyed by (callee base method, kind) per caller rather than by IL offset; see the wasm_jit_profile
 * comment in interp-internals.h for why offsets and per-site indices were both rejected. A merged entry
 * costs prediction accuracy, never correctness -- the emitted guard re-checks the vtable every call.
 */
#define WJ_PROF_MAX_SITES 12    /* per caller; linear scan, so keep it small. Hot methods have few distinct virtual callees. */
#define WJ_PROF_SATURATE  64    /* stop recording a site once the winner has this many samples: the answer is not going to change */
#define WJ_PROF_WAYS      8     /* distinct identities tracked per site; matches the [1,8] clamp on MONO_WASM_JIT_VCALL_WAYS */

/* What a site dispatches ON, which is what its identity token and its IC's ways discriminate on. */
typedef enum {
	WJ_SITE_VIRTUAL  = 0,   /* callvirt: identity is the receiver MonoVTable* */
	WJ_SITE_DELEGATE = 1,   /* Delegate.Invoke: identity is del->method, the delegate's target */
} WjSiteKind;

typedef struct {
	MonoMethod *base;      /* key part 1: callee base method, or the delegate class's Invoke */
	guint8 kind;           /* key part 2: WjSiteKind. A class can have a virtual site and a delegate
	                        * site on the same base method, and they discriminate on different things. */
	guint8 nids;           /* distinct identities recorded (0 = none) */
	guint8 ids_overflow;   /* an identity was seen that did not fit in ids[] */
	guint8 reserved;
	/* STABLE inline-cache identity for this site, or 0 = not yet emitted. Allocated once by
	 * mono_wasm_jit_prof_site_id at the first emission and reused by every later one, which is what
	 * lets a re-emitted site keep the worker-local PIC entries the previous generation warmed.
	 *
	 * GATED behind MONO_WASM_JIT_STABLE_IC_IDS, DEFAULT OFF, because the two things are not the same
	 * granularity and pretending they are is a behaviour change, not a refactor. This record is keyed by
	 * (base, kind) -- deliberately, see interp-internals.h -- so TWO call sites in one method that call
	 * the same base method share it, and would therefore share one PIC slot where they used to get one
	 * each. At the shipped vcall_ways of 1 that is a real trade with an unmeasured sign: two sites with
	 * the same dominant receiver share a warm entry and win, two with different receivers evict each
	 * other every call and lose. MEASURED on one boot: 6,345 emissions would take an existing id.
	 * Land the unification inert; A/B this separately. */
	guint32 site_id;
	/* Current front-runner identity: a MonoVTable* for a virtual site, a MonoMethod* for a delegate one.
	 * Only pointer identity is ever compared, so the two coexist without the reader knowing which it is. */
	gpointer identity;
	/* The override `identity` actually dispatched to, captured here rather than re-derived at emit time.
	 * That is not an optimization, it is a deadlock fix: resolving it during emission via
	 * mono_class_get_virtual_method can trigger mono_class_setup_vtable -> class init -> managed code
	 * -> IKVM's classloader -> a SYNCHRONOUS cross-thread JS call, all while the emitting thread holds
	 * the wasm-JIT compile lock. The target thread then blocks on that lock and both wedge; a CPU trace
	 * of the hang showed every thread parked in emscripten_futex_wait / pthread_cond_wait with
	 * mono_threads_wasm_sync_run_in_target_thread_vii live. The observer has already done this
	 * resolution safely, so just remember the answer. */
	MonoMethod *target;
	/* Boyer-Moore majority-vote MARGIN for `identity`, not an occurrence count: a matching observation
	 * increments it, a differing one decrements it, and hitting zero lets the next observation take
	 * over as front-runner. That fits in two words with no per-type table, which is what lets this sit
	 * on the interp dispatch path.
	 *
	 * Consequence for readers: margin/total is a margin ratio, NOT the frequency of `identity`. A
	 * perfectly monomorphic site gives margin == total (100%); a 50/50 site gives ~0% even though the
	 * winner is seen half the time. So thresholding on it is strictly more conservative than
	 * thresholding on frequency would be -- the direction we want, since a wrong guess costs a failed
	 * guard. A DELEGATE site leaves it at 0 by design: its emitted IC is keyed by delegate.method plus
	 * the target vtable, not by a receiver vtable, so there is no (identity, target) pair whose majority
	 * would mean anything. mono_wasm_jit_prof_predict requires both and correctly ignores those sites,
	 * while mono_wasm_jit_prof_arity reads only the identity set and works for either kind. */
	guint32 margin;
	guint32 total;         /* observations at this site */
	/* Distinct identities seen, saturating at WJ_PROF_WAYS.
	 *
	 * The margin above answers "is this site monomorphic". It cannot answer "how many receivers does it
	 * see", because a majority counter tracks exactly ONE candidate -- a 3-type site and a 7-type site
	 * both just show a small margin. Sizing an inline cache needs the count: the emitter otherwise lays
	 * down MONO_WASM_JIT_VCALL_WAYS guard chains at every virtual call site whether or not the receiver
	 * is ever polymorphic, and each dead way is a compare, a branch and a call_indirect in the body.
	 *
	 * A saturating set is sufficient because the useful range is 1..WJ_PROF_WAYS, and it costs at most
	 * WJ_PROF_WAYS pointer compares on a path that already linear-scans the site array and stops
	 * entirely once the site saturates. `ids_overflow` records "more identities than we can size for",
	 * which must be distinguished from the saturated count so a very polymorphic site gets the full
	 * width rather than being read as exactly WJ_PROF_WAYS. */
	gpointer ids [WJ_PROF_WAYS];
	/* R206 -- what a SECOND guarded arm needs, and neither of these is derivable from the fields above.
	 *
	 * `target` is the front-runner's resolved callee ONLY, and `margin` is a Boyer-Moore margin that
	 * tracks exactly one candidate, so a 2-way scheme has no way to name the runner-up or to size it.
	 * Both must come from the OBSERVER: resolving a target at emit time via
	 * mono_class_get_virtual_method deadlocks (see the comment on `target` above -- class init ->
	 * IKVM classloader -> synchronous cross-thread JS call while holding the compile lock).
	 *
	 * id_counts is a true per-identity occurrence count (saturating in guint16), NOT a margin, so the
	 * emitter can apply the break-even test directly: an extra guarded arm costs its guard on all
	 * traffic reaching it and saves (ic - direct) on what it captures, so it pays at roughly
	 * count/total > 14% when the target co-locates and > 38% when it does not.
	 *
	 * Cost: 80 B per site, 960 B per profiled caller. It extends THE one profile record rather than
	 * adding a second, per CLAUDE.md. */
	MonoMethod *id_targets [WJ_PROF_WAYS];
	guint16     id_counts [WJ_PROF_WAYS];
} WjProfSite;

typedef struct {
	guint32 n;
	WjProfSite sites [WJ_PROF_MAX_SITES];
} WjCallProfile;

/* MONO_WASM_JIT_DEVIRT_PROFILE=1: collect the profile above. Default off until the consumer lands.
 * DEFINED IN mini-wasm.c, not here: mono_wasm_jit_auto_init reads it, and mini-wasm.c is linked into
 * BOTH the runtime and the offline cross-compiler (mono-aot-cross) while interp.c is only in the
 * runtime. Defining it here breaks the mono-aot-cross link with an undefined symbol — the same reason
 * mono_wasm_jit_residual_mode lives in mini-wasm.c. */
extern int mono_wasm_jit_devirt_profile;

/* Collection telemetry, deliberately NOT behind MONO_WASM_JIT_STATS: the whole point is to check that
 * the profile is being populated during a normal (stats-off) run, which is the only kind worth timing.
 * Racy adds; these are for "is it working / how confident is it", not accounting. */
static gint32 wj_prof_samples;    /* observations recorded */
static gint32 wj_prof_sites;      /* distinct (caller, base, kind) triples seen */
static gint32 wj_prof_methods;    /* callers that allocated a profile */
static gint32 wj_prof_evicted;    /* observations dropped because a caller hit WJ_PROF_MAX_SITES */
static gint32 wj_prof_jit_samples;/* of which came from the JITTED code's IC miss rather than the interp */
static gint32 wj_prof_ids_reused; /* site_id handed back to a re-emission instead of freshly allocated */

/* Field: 0 = samples, 1 = sites, 2 = methods, 3 = evicted, 4 = post-JIT samples, 5 = site ids reused. */
EMSCRIPTEN_KEEPALIVE int
mono_wasm_jit_prof_stat (int field)
{
	switch (field) {
	case 0: return wj_prof_samples;
	case 1: return wj_prof_sites;
	case 2: return wj_prof_methods;
	case 3: return wj_prof_evicted;
	case 4: return wj_prof_jit_samples;
	case 5: return wj_prof_ids_reused;
	default: return -1;
	}
}

/* Add `identity` to the site's distinct-identity set. Saturating: past WJ_PROF_WAYS it only sets the
 * overflow flag, so the cost stays bounded no matter how polymorphic the site is. */
static void
wj_prof_note_identity (WjProfSite *s, gpointer identity, MonoMethod *target)
{
	guint32 k;

	for (k = 0; k < s->nids; ++k)
		if (s->ids [k] == identity) {
			/* Saturating: a count that wrapped would invert the ranking, which is worse than one that
			 * stops growing -- once an identity is this dominant its rank cannot change. */
			if (s->id_counts [k] < G_MAXUINT16)
				s->id_counts [k]++;
			/* A sizing-only observation arrives with target == NULL; do not clobber a resolved one. */
			if (target && !s->id_targets [k])
				s->id_targets [k] = target;
			return;
		}
	if (s->nids >= WJ_PROF_WAYS) {
		s->ids_overflow = 1;
		return;
	}
	s->id_targets [s->nids] = target;
	s->id_counts [s->nids] = 1;
	s->ids [s->nids++] = identity;
}

/* This caller's profile block, allocating it on first use. NULL only if the allocation failed. */
static WjCallProfile *
wj_prof_block (InterpMethod *caller)
{
	WjCallProfile *p = (WjCallProfile *) caller->wasm_jit_profile;

	if (!p) {
		p = (WjCallProfile *) m_method_alloc0 (caller->method, sizeof (WjCallProfile));
		if (!p)
			return NULL;
		/* Benign race: a concurrent recorder may install its own and we leak this one into the
		 * method's mempool (freed with the method). Publish with a CAS so readers never see a torn
		 * pointer, and re-read the winner. */
		if (mono_atomic_cas_ptr (&caller->wasm_jit_profile, p, NULL) != NULL)
			p = (WjCallProfile *) caller->wasm_jit_profile;
		else
			wj_prof_methods++;
	}

	return p;
}

/* Find this caller's record for (base, kind), appending one if there is room. NULL when the caller has
 * no profile block or has run out of site slots (counted as an eviction).
 *
 * This used to report via an `added` out-parameter whether it appended. Nothing needs it: the only
 * consumer was wj_prof_record's first-observation test, and `added` implies total == 0 there, so the
 * `total` test alone is equivalent. See wj_prof_record_at. */
static WjProfSite *
wj_prof_site (InterpMethod *caller, MonoMethod *base, WjSiteKind kind, gboolean create)
{
	WjCallProfile *p;
	guint32 i, n;

	if (!caller || !base)
		return NULL;
	p = create ? wj_prof_block (caller) : (WjCallProfile *) caller->wasm_jit_profile;
	if (!p)
		return NULL;
	mono_memory_barrier ();
	n = p->n;
	if (n > WJ_PROF_MAX_SITES)
		n = WJ_PROF_MAX_SITES;
	for (i = 0; i < n; ++i)
		if (p->sites [i].base == base && p->sites [i].kind == (guint8) kind)
			return &p->sites [i];
	if (!create)
		return NULL;
	if (p->n >= WJ_PROF_MAX_SITES) {
		wj_prof_evicted++;                /* full: ignore further callees rather than thrash */
		return NULL;
	}
	i = p->n;
	wj_prof_sites++;
	p->sites [i].base = base;
	p->sites [i].kind = (guint8) kind;
	/* Publish HERE rather than leaving it to the caller. Both callers append -- the recorder and the
	 * site-id allocator -- and an entry that is reachable by (base, kind) but not counted by `n` would be
	 * silently re-created by the other one, losing whichever field the first had written. Publishing a
	 * site whose payload is still zero is safe because every reader validates: prof_predict rejects a NULL
	 * identity and prof_arity rejects nids == 0. */
	mono_memory_barrier ();
	p->n = i + 1;
	return &p->sites [i];
}

/*
 * Record one observation. THE one writer.
 *
 * `identity` is the receiver vtable for a virtual site and the delegate's target for a delegate one;
 * `target` is the resolved override and may be NULL for a delegate site, which has nothing to predict.
 *
 * Called from the interpreter's virtual and delegate dispatch AND from the JITted code's IC-miss publish,
 * so it must stay cheap: a linear scan over <=12 pointers, no allocation after the first call, no locking.
 *
 * Racy by design on threaded builds. Concurrent recorders can interleave and lose a sample or briefly
 * disagree about the front-runner; the result is only ever a worse *prediction*, and the emitted code
 * guards on the identity regardless. Taking a lock here would cost more than the profile is worth.
 */
static void
wj_prof_record_at (WjProfSite *s, WjSiteKind kind, gpointer identity, MonoMethod *target,
                   gboolean from_jit)
{
	/* `total == 0` is FIRST OBSERVATION. It used to be spelled `!added && s->total != 0`, and the
	 * `added` conjunct was redundant: wj_prof_site appends into a zeroed block, so a site it reports as
	 * added always has total == 0 and the two tests can never disagree. Dropping it is what lets the
	 * lookup be hoisted out of this function and memoised by the caller.
	 *
	 * The test is on `total` rather than on "did this call create the record" because the site-id
	 * allocator also creates records, so a slot can exist with no observation in it. Taking the update
	 * path there would adopt the front-runner WITHOUT crediting it (the eviction branch deliberately
	 * leaves the margin at zero, so that a site which has ever been polymorphic can never again claim
	 * margin == total), and the site could then never be predicted however monomorphic it turned out to
	 * be. Keyed on `total`, the invariant is the same one the pre-unification recorder had. */
	if (s->total != 0) {
		/* A delegate site never accumulates a majority margin (see WjProfSite.margin), so it must
		 * saturate on `total` instead or it would record forever.
		 *
		 * FIRST, deliberately: at the plateau nearly every observation lands on a site that decided long
		 * ago, and with the record memoised at the call site this early-out is the whole cost of the
		 * profile -- two loads and a compare, no barrier and no scan. */
		guint32 seen = (kind == WJ_SITE_DELEGATE || !target) ? s->total : s->margin;
		if (seen >= WJ_PROF_SATURATE)
			return;                       /* decided; stop paying for this site */
		wj_prof_samples++;
		if (from_jit)
			wj_prof_jit_samples++;
		s->total++;
		wj_prof_note_identity (s, identity, target);
		if (!target)
			return;                       /* sizing-only observation; leave the majority alone */
		if (s->identity == identity) {
			s->margin++;
		} else if (s->margin == 0) {
			/* front-runner was evicted below; adopt this one, target and identity together */
			s->identity = identity;
			s->target = target;
		} else {
			s->margin--;                  /* majority vote: a competing type erodes the incumbent */
		}
		return;
	}
	wj_prof_samples++;
	if (from_jit)
		wj_prof_jit_samples++;
	s->identity = target ? identity : NULL;
	s->target = target;
	s->margin = target ? 1 : 0;
	s->total = 1;
	s->ids [0] = identity;
	s->nids = 1;
	s->ids_overflow = 0;
}

/* Resolve this caller's record for (base, kind) and record into it. The observation points that hold a
 * durable per-site structure (the JITted code's IC miss) should memoise the record and call
 * wj_prof_record_at directly instead; this form re-scans the caller's site table every call. */
static void
wj_prof_record (InterpMethod *caller, MonoMethod *base, WjSiteKind kind, gpointer identity,
                MonoMethod *target, gboolean from_jit)
{
	WjProfSite *s;

	if (!caller || !base || !identity)
		return;
	s = wj_prof_site (caller, base, kind, TRUE);
	if (!s)
		return;
	wj_prof_record_at (s, kind, identity, target, from_jit);
}

/*
 * Record a delegate invocation for sizing that site's delegate IC.
 *
 * The identity counted is del->method -- the delegate's target -- because that is what the emitted IC's
 * ways discriminate on. Multicast delegates (del->method == NULL) are filtered by the caller: the emitted
 * IC deliberately misses on them, so counting them would inflate the arity and buy back nothing.
 */
static void
wj_prof_record_delegate (InterpMethod *caller, MonoDelegate *del)
{
	MonoMethod *invoke;

	if (!caller || !del || !del->method)
		return;
	/* Keyed by the delegate class's Invoke method, which is exactly what the emitter has in hand at the
	 * call site (call->method) when it decides the IC width. */
	invoke = mono_get_delegate_invoke_internal (del->object.vtable->klass);
	if (!invoke)
		return;
	wj_prof_record (caller, invoke, WJ_SITE_DELEGATE, del->method, NULL, FALSE);
}

/* A fresh inline-cache site id, one per emitted site.
 *
 * MONO_WASM_JIT_STABLE_IC_IDS used to make this reuse the CALL PROFILE's record id instead, so a
 * re-emitted method kept its predecessor's warmed worker-local PIC entries. It is deleted, and the
 * reason is a granularity trap worth keeping: a profile record is keyed by callee BASE METHOD (IL
 * offsets are stale after generate_compacted_code), while an IC belongs to one CALL SITE. Two sites in
 * one method calling the same base would share a PIC slot -- at the shipped vcall_ways of 1, a mutual
 * eviction whenever they see different receivers -- and 6,345 emissions per boot would have taken an
 * existing id, so it was neither a rounding error nor a refactor. A STABLE INLINE-CACHE ID IS NOT THE
 * SAME GRANULARITY AS A PROFILE RECORD. */
guint32
mono_wasm_jit_prof_site_id (gpointer caller_ptr, MonoMethod *base, int kind, volatile gint32 *counter)
{
	(void) caller_ptr; (void) base; (void) kind;
	return (guint32) mono_atomic_inc_i32 (counter) - 1;
}

/*
 * R206: the RUNNER-UP receiver at a site, for a second guarded arm.
 *
 * Why this is a separate entry point and not a flag on mono_wasm_jit_prof_predict: that function answers
 * "is this site perfectly monomorphic", which is a one-way, deliberately conservative question keyed on
 * the Boyer-Moore margin. This one answers "what else does this site see, and how often", which the
 * margin structurally cannot express -- it tracks exactly ONE candidate. The two use different fields
 * (`identity`/`margin` vs `ids[]`/`id_counts[]`) and must not be merged.
 *
 * `skip` is the receiver an arm was already emitted for (may be NULL when the site was refused
 * outright, e.g. WJ_PRED_POLY -- then this returns the site's most frequent receiver, which is the
 * "give a polymorphic site one arm" case).
 *
 * Returns the highest-count identity != skip that has a RESOLVED target, and its share of total
 * observations in *out_pct. The caller applies the break-even test; this function does not embed a
 * policy. Same lock-free double-read discipline as prof_predict: the recorder never takes a lock, so a
 * replacement in flight must not pair one receiver's vtable with another receiver's target.
 */
gboolean
mono_wasm_jit_prof_predict_alt (gpointer caller_ptr, MonoMethod *base, MonoVTable *skip,
	MonoVTable **out_vt, MonoMethod **out_target, guint32 *out_pct)
{
	InterpMethod *caller = (InterpMethod *) caller_ptr;
	WjProfSite *s;
	guint32 k, nids1, nids2, total1, total2, best_c = 0;
	gpointer best_id = NULL;
	MonoMethod *best_t = NULL;

	if (out_pct)
		*out_pct = 0;
	if (!caller || !base || !out_vt || !out_target)
		return FALSE;
	s = wj_prof_site (caller, base, WJ_SITE_VIRTUAL, FALSE);
	if (!s)
		return FALSE;

	nids1 = s->nids; total1 = s->total;
	mono_memory_barrier ();
	for (k = 0; k < nids1 && k < WJ_PROF_WAYS; ++k) {
		gpointer id = s->ids [k];
		MonoMethod *t = s->id_targets [k];
		guint32 c = s->id_counts [k];
		if (!id || !t || id == (gpointer) skip)
			continue;
		/* A sizing-only observation leaves id_targets NULL, so an identity can be present with no
		 * target; those are skipped above rather than guessed at. */
		if (c > best_c) { best_c = c; best_id = id; best_t = t; }
	}
	mono_memory_barrier ();
	nids2 = s->nids; total2 = s->total;
	/* nids only grows and total only grows; a change means the set moved under us. Refusing is free --
	 * the site keeps its IC and the next emission sees a settled record. */
	if (nids1 != nids2 || total1 != total2 || !best_id || !best_t || !total1)
		return FALSE;

	*out_vt = (MonoVTable *) best_id;
	*out_target = best_t;
	if (out_pct)
		*out_pct = (guint32) ((guint64) best_c * 100 / total1);
	return TRUE;
}

/*
 * R215b: the dominant target at a DELEGATE site.
 *
 * Delegates need their own reader, and the reason is structural rather than cosmetic. For a delegate site
 * the IDENTITY IS THE TARGET -- `wj_prof_record` is called with `del->method` as the identity and NULL as
 * the target, because the `target` field means "the resolved override" and a delegate has no vtable
 * receiver to resolve. So mono_wasm_jit_prof_predict_alt, which requires id_targets[k] to be non-NULL,
 * can never succeed at a delegate site: MEASURED, 7,582 delegate sites and 0 armed.
 *
 * `margin` is likewise 0 by design here (a majority vote over receivers is meaningless when the identity
 * is a method), so frequency over id_counts is the only workable signal -- which is what this reads.
 */
gboolean
mono_wasm_jit_prof_predict_delegate (gpointer caller_ptr, MonoMethod *base,
	MonoMethod **out_target, guint32 *out_pct)
{
	InterpMethod *caller = (InterpMethod *) caller_ptr;
	WjProfSite *s;
	guint32 k, nids1, total1, nids2, total2, best_c = 0;
	gpointer best_id = NULL;

	if (out_pct)
		*out_pct = 0;
	if (!caller || !base || !out_target)
		return FALSE;
	s = wj_prof_site (caller, base, WJ_SITE_DELEGATE, FALSE);
	if (!s)
		return FALSE;
	nids1 = s->nids; total1 = s->total;
	mono_memory_barrier ();
	for (k = 0; k < nids1 && k < WJ_PROF_WAYS; ++k) {
		gpointer id = s->ids [k];
		guint32 c = s->id_counts [k];
		if (!id)
			continue;
		if (c > best_c) { best_c = c; best_id = id; }
	}
	mono_memory_barrier ();
	nids2 = s->nids; total2 = s->total;
	if (nids1 != nids2 || total1 != total2 || !best_id || !total1)
		return FALSE;
	*out_target = (MonoMethod *) best_id;      /* identity IS the target here */
	if (out_pct)
		*out_pct = (guint32) ((guint64) best_c * 100 / total1);
	return TRUE;
}

/*
 * Why a prediction was refused, for the emitter's WJC_DEVIRT_* census. Mirrored as plain ints rather
 * than a shared enum because interp.c and mini-wasm.c have no private header in common (the same reason
 * mono_wasm_jit_devirt_profile is DEFINED in mini-wasm.c). Keep in step with the WJC_DEVIRT_* block in
 * mini-wasm.h and with wj_devirt_census in mini-wasm.c.
 */
#define WJ_PRED_OK       0
#define WJ_PRED_NO_REC   1   /* no record for this (caller, base, VIRTUAL) */
#define WJ_PRED_COLD     2   /* record exists, fewer than 8 observations */
#define WJ_PRED_POLY     3   /* warm, margin != total, and below the 90%-frequency bar too */
#define WJ_PRED_POLY_90  4   /* warm, margin != total, but the front-runner is >= 90% of observations */
#define WJ_PRED_TORN     5   /* lock-free read disagreed with itself; retry would succeed */

/*
 * Return a prediction only for a sufficiently warmed, perfectly monomorphic site.
 *
 * `margin` is a Boyer-Moore margin rather than an occurrence counter, so equality with total is the
 * one useful exact statement it can make: every observation had the same receiver.  The generated
 * code still guards on that vtable, making a stale/racy prediction a performance miss rather than a
 * correctness assumption.  Read the tuple twice because the recorder is deliberately lock-free; a
 * replacement in progress must not pair one receiver's vtable with another receiver's target.
 */
gboolean
mono_wasm_jit_prof_predict (gpointer caller_ptr, MonoMethod *base, MonoVTable **out_vt,
	MonoMethod **out_target, guint32 *out_samples, int *out_why)
{
	InterpMethod *caller = (InterpMethod *) caller_ptr;
	WjProfSite *s;
	gpointer id1, id2;
	MonoMethod *target1, *target2;
	guint32 hits1, hits2, total1, total2;

	if (out_why)
		*out_why = WJ_PRED_NO_REC;
	if (!caller || !base || !out_vt || !out_target)
		return FALSE;
	s = wj_prof_site (caller, base, WJ_SITE_VIRTUAL, FALSE);
	if (!s)
		return FALSE;
	id1 = s->identity; target1 = s->target; hits1 = s->margin; total1 = s->total;
	mono_memory_barrier ();
	id2 = s->identity; target2 = s->target; hits2 = s->margin; total2 = s->total;
	if (id1 != id2 || target1 != target2 || hits1 != hits2 || total1 != total2) {
		if (out_why) *out_why = WJ_PRED_TORN;
		return FALSE;
	}
	if (total1 < 8) {
		if (out_why) *out_why = WJ_PRED_COLD;
		return FALSE;
	}
	{
		/* R210: FREQUENCY BAR over per-identity counts, replacing `margin == total` when enabled.
		 *
		 * Selects the most frequent identity and accepts it if its share of observations clears
		 * MONO_WASM_JIT_PRED_PCT. Subsumes the margin path: a perfectly monomorphic site has 100% share
		 * and passes trivially, while a 90/10 site -- which the margin bar rejects for the life of the
		 * process, identically to a 50/50 one -- now predicts, and its 10% falls through the guard to the
		 * IC exactly as a prediction miss always has. Guard ~3 x86 against ~17 saved on a hit, so the
		 * economics justify a far lower bar than 100%.
		 *
		 * Reads ids[]/id_counts[]/id_targets[] under the same lock-free double-read discipline as the
		 * margin path below: nids and total are re-read after the scan and a disagreement refuses. */
		extern int mono_wasm_jit_pred_pct;
		if (mono_wasm_jit_pred_pct > 0) {
			guint32 k, nids1 = s->nids, best_c = 0, nids2, total2b;
			gpointer best_id = NULL;
			MonoMethod *best_t = NULL;
			for (k = 0; k < nids1 && k < WJ_PROF_WAYS; ++k) {
				gpointer id = s->ids [k];
				MonoMethod *t = s->id_targets [k];
				guint32 c = s->id_counts [k];
				/* An identity seen only through a sizing-only observation has no resolved target and
				 * cannot be called; skip rather than guess. */
				if (!id || !t)
					continue;
				if (c > best_c) { best_c = c; best_id = id; best_t = t; }
			}
			mono_memory_barrier ();
			nids2 = s->nids; total2b = s->total;
			if (nids1 != nids2 || total1 != total2b) {
				if (out_why) *out_why = WJ_PRED_TORN;
				return FALSE;
			}
			if (!best_id || !best_t || !total1 ||
			    (guint64) best_c * 100 / total1 < (guint32) mono_wasm_jit_pred_pct) {
				if (out_why) *out_why = WJ_PRED_POLY;
				return FALSE;
			}
			*out_vt = (MonoVTable *) best_id;
			*out_target = best_t;
			if (out_samples) *out_samples = total1;
			if (out_why) *out_why = WJ_PRED_OK;
			return TRUE;
		}
	}
	if (!id1 || !target1) {
		if (out_why) *out_why = WJ_PRED_COLD;
		return FALSE;
	}
	if (hits1 != total1) {
		/* Not perfectly monomorphic. Record separately whether it WOULD pass a frequency bar: `margin`
		 * is a Boyer-Moore margin (wins - losses) against `total` (wins + losses), so the front-runner's
		 * share is (1 + margin/total)/2 and margin/total >= 0.8 is exactly ">= 90% of observations".
		 * The distinction matters because the current bar treats a 99%-monomorphic site and a 50/50 site
		 * identically -- and it is one-way: the moment margin < total it can never equal total again,
		 * since each further observation adds 1 to total and at most 1 to margin. */
		if (out_why)
			*out_why = (hits1 * 5 >= total1 * 4) ? WJ_PRED_POLY_90 : WJ_PRED_POLY;
		return FALSE;
	}
	if (out_why)
		*out_why = WJ_PRED_OK;
	*out_vt = (MonoVTable *) id1;
	*out_target = target1;
	if (out_samples)
		*out_samples = total1;
	return TRUE;
}

/*
 * How many distinct identities this caller saw at `base`, for sizing that site's inline cache.
 *
 * 0 means "no usable observation" — no profile, site not found, too few samples, or the set overflowed —
 * and the caller must fall back to the configured width rather than guess narrow. Returning the saturated
 * count on overflow would be exactly wrong: a site with 20 receivers would be sized for 8 and read as
 * fully covered.
 *
 * Same double-read discipline as mono_wasm_jit_prof_predict: the recorder is lock-free, so a set being
 * appended to concurrently must not be read as a smaller count paired with a larger one. Unlike the
 * prediction path a stale answer here is harmless in both directions — too few ways costs IC misses, too
 * many costs dead guards — but a torn read could produce a nonsense width, so it is still rejected.
 *
 * Searches BOTH kinds: the emitter asks this for a virtual site and for a Delegate.Invoke site, and only
 * one of the two can exist for a given base method at a given call site.
 */
guint32
mono_wasm_jit_prof_arity (gpointer caller_ptr, MonoMethod *base)
{
	InterpMethod *caller = (InterpMethod *) caller_ptr;
	int k;

	if (!caller || !base)
		return 0;
	for (k = 0; k < 2; ++k) {
		WjProfSite *s = wj_prof_site (caller, base, (WjSiteKind) k, FALSE);
		guint32 n1, n2, total1, total2, ovf1, ovf2;
		if (!s)
			continue;
		n1 = s->nids; total1 = s->total; ovf1 = s->ids_overflow;
		mono_memory_barrier ();
		n2 = s->nids; total2 = s->total; ovf2 = s->ids_overflow;
		if (n1 != n2 || total1 != total2 || ovf1 != ovf2)
			return 0;
		/* Same warm-up bar as the prediction path: below it, "we only ever saw one type" is as likely to
		 * mean "we barely ran" as it is to mean monomorphic, and narrowing on that would turn a cold
		 * polymorphic site into a permanent stream of IC misses. */
		if (ovf1 || n1 == 0 || total1 < 8)
			return 0;
		return n1;
	}
	return 0;
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
		for (j = 0; j < a->len; ++j) {
			MonoMethod *wm = (MonoMethod *) a->pdata [j];
			wj_promote_push (wm);
		}
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
		mono_atomic_xchg_i32 ((volatile gint32 *)&wj_entry_edges [i].window, 0);
}

/* Vtable/IMT slot for a virtual/interface base method — mirrors transform.c:get_virt_method_slot
 * (static there). Stable per call site; the get_virtual_method_fast cache key used by the vcall
 * fast-miss path (mono_wasm_jit_vcall_resolve[_fslot]). */
static int
wj_virt_method_slot (MonoMethod *m)
{
	return mono_class_is_interface (m->klass)
		? (-2 * MONO_IMT_SIZE + mono_method_get_imt_slot (m))
		: mono_method_get_vtable_slot (m);
}

/* --- vcall receiver-arity diagnostic (MONO_WASM_JIT_ARITY=1) -------------------------------------
 * Answers the polymorphic-IC question directly: of the calls that reach the vcall resolve helper (i.e.
 * MISS the site's monomorphic inline IC), what fraction would an N-way IC capture? Per call site we keep
 * a shadow LRU of the last WJ_ARITY_WAYS receiver vtables (after the N-way IC, fast-miss metadata, and
 * optional delegate recipe; allocated only when the flag is on) and, on each helper call, record the
 * LRU depth at which the receiver is found (depth d =>
 * an (d+1)-way LRU IC would hit) or "miss" (beyond N distinct => megamorphic). Cumulative depth<=k gives
 * the k-way capture rate. Counters are plain (not atomic) and the per-site shadow races across MC worker
 * threads — fine for a distributional diagnostic (like MONO_WASM_JIT_PROFILE_FAST, it perturbs timing, so
 * run it in a dedicated arity run, not a perf run). */
#define WJ_ARITY_WAYS 8
extern int mono_wasm_jit_arity;
static long long wj_arity_hit [WJ_ARITY_WAYS];   /* receiver found at LRU depth d (0=MRU) */
static long long wj_arity_miss;                   /* receiver not among the last WJ_ARITY_WAYS distinct vtables */

/* Optional per-call-site single-cast delegate recipes, allocated only for Delegate.Invoke sites.
 * Each way has its own seqlock: even = stable, odd = writer active, zero = empty. The cache is shared
 * by wasm worker threads, so readers validate the sequence around the plain payload loads and can never
 * combine fields from two delegate targets. InterpMethod is cached rather than a raw e-slot so a target
 * which JITs later is observed and admitted into each thread's function table before use. */
typedef struct {
	volatile gint32 seq;
	MonoMethod * volatile source;
	MonoVTable * volatile receiver_vt;
	MonoMethod * volatile target;
	InterpMethod * volatile imethod;
	volatile gint32 shape;
	volatile gint32 slots;
	volatile gint32 scalar;
} WjDelegateIC;

/*
 * The generated dispatch PIC is deliberately worker-local.  Dynamic wasm table entries are local to
 * a worker, so caching an InterpMethod process-wide forced every hit to load its fslot and consult a
 * second TLS liveness bitmap.  A local entry can cache the final admitted fslot directly:
 *
 *   +0  atomic-looking hot pair [i32 receiver vtable | i32 admitted fslot]
 *   +8  resolved MonoMethod* (cold profile consumer only)
 *   +12 hit count (plain: only this worker writes it)
 *
 * The 16-byte stride keeps the hot pair naturally aligned.  The i64 pair is loaded with an ordinary
 * wasm load: although the heap is shared, a TLS block has one writer/reader worker.  `target` and
 * `hits` are the target-frequency feed for dynamic batching and are intentionally outside the pair,
 * so they do not enlarge the dispatch key.
 */
typedef struct {
	guint64 key_fslot;
	MonoMethod *target;
	guint32 hits;
} WjLocalVcallPicEntry;

typedef struct {
	/* STABLE across re-emissions of this call site -- it comes from the site's profile record, not from
	 * a fresh counter bump, so the worker-local PIC entries a previous generation warmed are still this
	 * site's. See WjProfSite.site_id. */
	guint32 site_id;
	/* This site's profile record, memoised on first use by the IC-miss observation point.
	 *
	 * Occupies the explicit padding word that used to sit here (and still keeps the resolver IC which
	 * follows this header naturally 8-byte aligned: on wasm32 the pointers below are 4 bytes each, so
	 * without a fourth word the header was 12 bytes and every i64.atomic access in vcall_resolve_fslot
	 * trapped as unaligned). Zero bytes added.
	 *
	 * WHY MEMOISING IS SAFE HERE, given "resolve and consume in the same breath". That rule is about
	 * things whose IDENTITY can change under you -- a WjBody another thread retires, a name that lazily
	 * resolves metadata. Neither applies: (caller_im, base, WJ_SITE_VIRTUAL) is FIXED for a call site,
	 * and the record it names never moves. The WjCallProfile is allocated once from caller->method's
	 * mempool and published by a CAS whose loser re-reads the winner, so it is never replaced; sites live
	 * in a fixed WJ_PROF_MAX_SITES array, appended in place, never reordered and never recycled (a full
	 * table refuses new sites, it does not evict old ones). So the address is stable for the life of the
	 * InterpMethod -- which this struct already depends on outliving it, via caller_im.
	 *
	 * WHY IT IS WORTH A FIELD: this is the IC MISS path, 78.8% of all profile observations (R148), and
	 * wj_prof_record's first act is a memory barrier plus a linear scan of up to WJ_PROF_MAX_SITES
	 * (base, kind) pairs -- paid on every miss, including the overwhelmingly common one at a site that
	 * saturated long ago and will return without recording anything. Caching the record lets the
	 * saturation test come first and reduces the steady-state cost to a load and a compare.
	 *
	 * Benign under races: concurrent first-misses resolve the same (base, kind) and store the same
	 * pointer, and a NULL result (no profile block, or the caller's site table is full) is simply left
	 * uncached and retried. */
	WjProfSite *prof;
	/* The caller's InterpMethod rather than its MonoMethod, so the IC-miss path can feed the call
	 * profile without a locking mono_interp_get_imethod. Resolved once, at emit time, where taking the
	 * jit_mm lock is safe. */
	InterpMethod *caller_im;
	MonoMethod *base;
} WjVcallSite;

g_static_assert ((sizeof (WjVcallSite) & 7) == 0);

static volatile gint32 wj_vcall_site_count;
#define WJ_VCALL_SITE_MAX 65536
static __thread WjLocalVcallPicEntry *wj_vcall_pic;
static __thread gint32 wj_vcall_pic_cap; /* sites, not entries */

static inline WjVcallSite *
wj_vcall_site (gpointer ic)
{
	return (WjVcallSite *) ((guint8 *) ic - sizeof (WjVcallSite));
}

static WjLocalVcallPicEntry *
wj_vcall_pic_for_site (gpointer ic, gboolean grow)
{
	extern int mono_wasm_jit_vcall_ways;
	WjVcallSite *site = wj_vcall_site (ic);
	guint32 need = site->site_id + 1;
	if (G_UNLIKELY (need > (guint32) wj_vcall_pic_cap)) {
		gint32 oldcap, ncap;
		gsize oldbytes, nbytes;
		if (!grow)
			return NULL;
		oldcap = wj_vcall_pic_cap;
		ncap = oldcap ? oldcap : 256;
		while (need > (guint32) ncap)
			ncap *= 2;
		oldbytes = (gsize) oldcap * mono_wasm_jit_vcall_ways * sizeof (WjLocalVcallPicEntry);
		nbytes = (gsize) ncap * mono_wasm_jit_vcall_ways * sizeof (WjLocalVcallPicEntry);
		wj_vcall_pic = (WjLocalVcallPicEntry *) g_realloc (wj_vcall_pic, nbytes);
		memset ((guint8 *) wj_vcall_pic + oldbytes, 0, nbytes - oldbytes);
		/* Publish the pointer before the capacity. Generated wasm reads cap first and only dereferences
		 * the pointer when the site fits. Both are same-thread accesses; this ordering also makes the
		 * invariant explicit for a future shared profile reader. */
		mono_memory_barrier ();
		wj_vcall_pic_cap = ncap;
	}
	{
		/* Bounds invariant the generated code relies on but cannot check. If this ever fires, the inline
		 * load at base + site_id*stride would have read past the array -- catching it here names the site
		 * instead of leaving an opaque wasm trap. */
		gsize off = (gsize) site->site_id * mono_wasm_jit_vcall_ways;
		gsize lim = (gsize) wj_vcall_pic_cap * mono_wasm_jit_vcall_ways;
		if (G_UNLIKELY (!wj_vcall_pic || off + mono_wasm_jit_vcall_ways > lim))
			return NULL;
	}
	return wj_vcall_pic + (gsize) site->site_id * mono_wasm_jit_vcall_ways;
}

static void
wj_vcall_pic_publish (gpointer ic, MonoVTable *vt, MonoMethod *target, gint32 fslot)
{
	extern int mono_wasm_jit_vcall_ways;
	WjLocalVcallPicEntry *p = wj_vcall_pic_for_site (ic, TRUE);
	/* THE MISS PATH IS AN OBSERVATION POINT. This is the half of the call profile the interpreter can
	 * never supply: post-JIT, per-site, target-resolved, and still updating while the compiled code runs.
	 * It is on the MISS path only, so the PIC hit path is unchanged -- a pure TLS load with no added
	 * instruction, which is the property R114 was protecting when vcall_ways 4->1 won +9.6% on the tail. */
	{
		WjVcallSite *site = wj_vcall_site (ic);
		if (G_UNLIKELY (mono_wasm_jit_devirt_profile) && vt && site->caller_im && site->base) {
			/* Memoised: (caller_im, base, WJ_SITE_VIRTUAL) is fixed for this site, so the record it
			 * names is too. See WjVcallSite.prof for why that pointer is safe to hold, and for what the
			 * lookup costs on a path that takes 78.8% of all observations. */
			WjProfSite *ps = site->prof;
			if (G_UNLIKELY (!ps)) {
				ps = wj_prof_site (site->caller_im, site->base, WJ_SITE_VIRTUAL, TRUE);
				if (ps)
					site->prof = ps;
			}
			if (ps)
				wj_prof_record_at (ps, WJ_SITE_VIRTUAL, vt, target, TRUE);
		}
		/* R208's RE-EMISSION TRIGGER stood here: enqueue the caller once its own emitted code had missed
		 * at this site enough times. It was the right observation point -- a miss HERE means the profile
		 * can now predict something it could not at emit time, whereas the old invoke_in trigger counted
		 * interp->JIT BOUNDARY crossings and measured vicMiss 1.00x. Deleted with the rest of
		 * re-emission; see the block where the ring used to be. */
		/* CO-LOCATION REACH, measured where it actually happens. Same reasoning as the profile record
		 * above: this is the miss path, so the PIC hit path stays a pure TLS load. See WJC_VIC_TGT_*
		 * for why WJC_CALL_LOCAL cannot answer this and for the lower-bound caveat. */
		if (G_UNLIKELY (mono_wasm_jit_stats) && site->caller_im) {
			extern int mono_wasm_jit_call_is_colocated (int caller_desc, int callee_fslot);
			switch (mono_wasm_jit_call_is_colocated (site->caller_im->wasm_jit_desc, fslot)) {
			case 1:  mono_wasm_jit_count (WJC_VIC_TGT_SIBLING); break;
			case 0:  mono_wasm_jit_count (WJC_VIC_TGT_FOREIGN); break;
			default: mono_wasm_jit_count (WJC_VIC_TGT_NOTOURS); break;
			}
		}
	}
	guint64 pair = ((guint64) (guint32) fslot << 32) | (guint32) (gsize) vt;
	int k, victim = 0;
	guint32 least = 0xffffffffu;

	/* Publishing is a pure optimisation, so skipping a failed bounds invariant is always safe. */
	if (G_UNLIKELY (!p))
		return;

	/* Refresh an existing receiver in place. Otherwise prefer an empty way, then replace the least
	 * frequent target. This is steadier than cross-thread LRU and preserves V8's monomorphic feedback
	 * at one-target sites. */
	for (k = 0; k < mono_wasm_jit_vcall_ways; ++k) {
		if ((guint32) p [k].key_fslot == (guint32) (gsize) vt) {
			p [k].target = target;
			p [k].key_fslot = pair;
			if (p [k].hits != 0xffffffffu) p [k].hits++;
			return;
		}
		if (!p [k].key_fslot) { victim = k; least = 0; break; }
		if (p [k].hits < least) { least = p [k].hits; victim = k; }
	}
	p [victim].target = target;
	p [victim].hits = 1;
	mono_memory_barrier ();
	p [victim].key_fslot = pair; /* publish the guard+payload last */
}

/* Imported as immutable globals by each dynamic method instance.  They are the addresses of the TLS
 * pointer/cap variables, not snapshots of their values, so realloc-on-growth is visible immediately. */
gpointer *
mono_wasm_jit_vcall_pic_ptr_addr (void)
{
	return (gpointer *) &wj_vcall_pic;
}

gint32 *
mono_wasm_jit_vcall_pic_cap_addr (void)
{
	return &wj_vcall_pic_cap;
}

guint32
mono_wasm_jit_vcall_pic_site_id (gpointer ic)
{
	return wj_vcall_site (ic)->site_id;
}

/* mono_wasm_jit_vcall_profile_count / _entry are GONE, and with them wj_vcall_sites[]: they existed so
 * the batch planner could walk this worker's PIC as a second, separately-scaled profile. The miss path
 * now writes the one call profile directly (see wj_vcall_pic_publish), so the planner reads a single
 * source and the x8-vs-x4 reconciliation multipliers are gone with it. The PIC itself stays exactly as
 * it was: worker-local dispatch state, never a profile. */

int
mono_wasm_jit_vcall_pic_stride (void)
{
	return (int) sizeof (WjLocalVcallPicEntry);
}

static WjDelegateIC *
wj_delegate_ic (gpointer ic)
{
	extern int mono_wasm_jit_vcall_ways;
	return (WjDelegateIC *) ((guint8 *) ic + 8 * (mono_wasm_jit_vcall_ways + 1));
}

static int
wj_delegate_ic_size (void)
{
	extern int mono_wasm_jit_vcall_ways;
	return mono_wasm_jit_vcall_ways * (int) sizeof (WjDelegateIC);
}

/* Emitter-time layout queries for the generated-wasm recipe fast path. WjDelegateIC stays private
 * to the interpreter and mini-wasm.c does not duplicate its C layout. None of these are hot calls. */
gpointer
mono_wasm_jit_delegate_ic_base (gpointer ic)
{
	return wj_delegate_ic (ic);
}

int
mono_wasm_jit_delegate_ic_stride (void)
{
	return (int) sizeof (WjDelegateIC);
}

int
mono_wasm_jit_delegate_ic_field_off (int field)
{
	switch (field) {
	case 0: return (int) G_STRUCT_OFFSET (WjDelegateIC, seq);
	case 1: return (int) G_STRUCT_OFFSET (WjDelegateIC, source);
	case 2: return (int) G_STRUCT_OFFSET (WjDelegateIC, receiver_vt);
	case 3: return (int) G_STRUCT_OFFSET (WjDelegateIC, imethod);
	case 4: return (int) G_STRUCT_OFFSET (WjDelegateIC, shape);
	case 5: return (int) G_STRUCT_OFFSET (WjDelegateIC, scalar);
	default: g_assert_not_reached (); return 0;
	}
}

/* --- worker-local delegate recipe PIC ------------------------------------------------------------
 *
 * The delegate twin of WjLocalVcallPicEntry, added for exactly the reason recorded above that one: a
 * process-wide recipe cannot cache a function-table slot, because dynamic table entries are per worker.
 * WjDelegateIC therefore caches an InterpMethod, and every generated hit had to pay for that choice --
 * load imethod, load imethod->fslot, test it, then probe the TLS liveness bitmap (cap compare, pointer
 * load, byte load, shift, mask) -- plus a seqlock bracket of two atomic loads because the shared entry
 * has concurrent writers.
 *
 * A worker-local entry removes all of it. It caches the ALREADY ADMITTED fslot, so a hit is a compare
 * and a shift; and with one writer and one reader on the same thread there is nothing for a seqlock to
 * protect. Measured against the counters, this is the largest dispatch class in the profile
 * (fast_delegate == delegate_ic_hit == 1.01e9, ~36% of all dispatches), and the guard sequence goes from
 * ~79 wasm ops / 9 loads (3 atomic) to ~49 / 6 (0 atomic).
 *
 * The two safety properties it leans on are the same ones the vcall PIC already relies on, both checked:
 * wj_slot_live bits are only ever SET (nothing clears them), and a published PIC entry is never
 * invalidated. So "fslot was admitted on this thread once" stays true forever, which is what lets the
 * fslot be cached instead of re-derived.
 *
 *   +0  [i32 source MonoMethod* | i32 admitted fslot]   guard and payload in ONE i64 load
 *   +8  required del->target->vtable, or 0 for a static/nonvirtual recipe
 *   +12 WJ_DELEGATE_* shape
 *
 * An empty entry is all zero, and a real MonoMethod* is never 0, so the source compare rejects empty
 * ways without a separate occupancy test. Publication is gated on `scalar && fslot > 0`, which is what
 * lets the generated code drop the scalar test and the fslot != 0 test as well.
 */
typedef struct {
	guint64 key_fslot;
	guint32 receiver_vt;
	guint32 shape;
} WjLocalDelegatePicEntry;

g_static_assert (sizeof (WjLocalDelegatePicEntry) == 16);

extern int mono_wasm_jit_delegate_local_pic;   /* MONO_WASM_JIT_DELEGATE_LOCAL_PIC, defined in mini-wasm.c */
extern int mono_wasm_jit_eslot_residual;        /* MONO_WASM_JIT_ESLOT_RESIDUAL, defined in mini-wasm.c */
extern int mono_wasm_jit_admit (int desc_id);   /* declared locally at several call sites; needed here too */

static __thread WjLocalDelegatePicEntry *wj_delegate_pic;
static __thread gint32 wj_delegate_pic_cap;   /* sites, not entries */

static WjLocalDelegatePicEntry *
wj_delegate_pic_for_site (gpointer ic, gboolean grow)
{
	extern int mono_wasm_jit_vcall_ways;
	WjVcallSite *site = wj_vcall_site (ic);
	guint32 need = site->site_id + 1;
	if (G_UNLIKELY (need > (guint32) wj_delegate_pic_cap)) {
		gint32 oldcap, ncap;
		gsize oldbytes, nbytes;
		if (!grow)
			return NULL;
		oldcap = wj_delegate_pic_cap;
		ncap = oldcap ? oldcap : 256;
		while (need > (guint32) ncap)
			ncap *= 2;
		oldbytes = (gsize) oldcap * mono_wasm_jit_vcall_ways * sizeof (WjLocalDelegatePicEntry);
		nbytes = (gsize) ncap * mono_wasm_jit_vcall_ways * sizeof (WjLocalDelegatePicEntry);
		wj_delegate_pic = (WjLocalDelegatePicEntry *) g_realloc (wj_delegate_pic, nbytes);
		memset ((guint8 *) wj_delegate_pic + oldbytes, 0, nbytes - oldbytes);
		/* Pointer before capacity, same as the vcall PIC: generated code reads cap first and only
		 * dereferences the pointer for a site the cap admits. */
		mono_memory_barrier ();
		wj_delegate_pic_cap = ncap;
	}
	{
		gsize off = (gsize) site->site_id * mono_wasm_jit_vcall_ways;
		gsize lim = (gsize) wj_delegate_pic_cap * mono_wasm_jit_vcall_ways;
		if (G_UNLIKELY (!wj_delegate_pic || off + mono_wasm_jit_vcall_ways > lim))
			return NULL;
	}
	return wj_delegate_pic + (gsize) site->site_id * mono_wasm_jit_vcall_ways;
}

/* Publish one recipe for THIS worker. Called only from the miss helper, so it runs at most once per
 * (site, way, thread) per distinct key -- a generated hit never reaches here. */
static void
wj_delegate_pic_publish (gpointer ic, MonoMethod *source, MonoVTable *receiver_vt, gint32 fslot, gint32 shape)
{
	extern int mono_wasm_jit_vcall_ways;
	WjLocalDelegatePicEntry *p;
	guint64 pair;
	int k, victim = -1;

	if (fslot <= 0 || !source)
		return;   /* nothing cacheable: a hit must imply an admitted scalar target */
	p = wj_delegate_pic_for_site (ic, TRUE);
	if (G_UNLIKELY (!p))
		return;   /* publishing is a pure optimisation */

	pair = ((guint64) (guint32) fslot << 32) | (guint32) (gsize) source;
	/* Refresh this key in place, else take an empty way, else hash to a victim. Same policy as
	 * wj_delegate_cache_write, so the two caches evict alike and a shared-cache hit that re-publishes
	 * here lands in the same way it did before. */
	for (k = 0; k < mono_wasm_jit_vcall_ways; ++k) {
		if ((guint32) p [k].key_fslot == (guint32) (gsize) source &&
		    p [k].receiver_vt == (guint32) (gsize) receiver_vt) {
			victim = k;
			break;
		}
		if (victim < 0 && !p [k].key_fslot)
			victim = k;
	}
	if (victim < 0) {
		gsize hash = ((gsize) source >> 4) ^ ((gsize) receiver_vt >> 5);
		victim = (int) (hash % (guint) mono_wasm_jit_vcall_ways);
	}
	p [victim].receiver_vt = (guint32) (gsize) receiver_vt;
	p [victim].shape = (guint32) shape;
	mono_memory_barrier ();
	p [victim].key_fslot = pair;   /* guard+payload last */
}

/* Addresses (not snapshots) of the TLS pointer/cap, imported by each generated module exactly like the
 * vcall PIC's, so a realloc-on-growth is picked up with no re-emission. */
gpointer *
mono_wasm_jit_delegate_pic_ptr_addr (void)
{
	return (gpointer *) &wj_delegate_pic;
}

gint32 *
mono_wasm_jit_delegate_pic_cap_addr (void)
{
	return &wj_delegate_pic_cap;
}

int
mono_wasm_jit_delegate_pic_stride (void)
{
	return (int) sizeof (WjLocalDelegatePicEntry);
}

int
mono_wasm_jit_delegate_field_off (int field)
{
	switch (field) {
	case 0: return (int) G_STRUCT_OFFSET (MonoDelegate, target);
	case 1: return (int) G_STRUCT_OFFSET (MonoDelegate, method);
	case 2: return (int) G_STRUCT_OFFSET (MonoMulticastDelegate, delegates);
	/* R192/Stage 1, the object-keyed delegate recipe. 3 is the hop from the delegate to its
	 * (delegate class, target method) tramp info; 4 is the packed (fslot << 3) | shape inside it.
	 * Queried rather than hardcoded for the same reason 0-2 are: mini-wasm.c's #else offline
	 * placeholders are already stale (delegate_list_off there is 64 against a real 60), so a
	 * hardcoded offset in the emitter is a silent wrong-field load rather than a build error. */
	case 3: return (int) G_STRUCT_OFFSET (MonoDelegate, invoke_info);
	case 4: return (int) G_STRUCT_OFFSET (MonoDelegateTrampInfo, wasm_jit_recipe);
	default: g_assert_not_reached (); return 0;
	}
}

static void
wj_arity_record (gpointer ic, MonoVTable *vt, gboolean delegate_site)
{
	extern int mono_wasm_jit_vcall_ways;
	guint8 *p = (guint8 *) ic + 8 * (mono_wasm_jit_vcall_ways + 1);
	if (delegate_site)
		p += wj_delegate_ic_size ();
	guint32 *sh = (guint32 *) p;   /* after N IC entries + fast-miss meta + optional delegate recipe */
	guint32 v = (guint32) (gsize) vt;                 /* a real vtable pointer is never 0, so 0 slots never false-match */
	int d = -1, i;
	for (i = 0; i < WJ_ARITY_WAYS; ++i) if (sh [i] == v) { d = i; break; }
	if (d < 0) { wj_arity_miss++; for (i = WJ_ARITY_WAYS - 1; i > 0; --i) sh [i] = sh [i - 1]; sh [0] = v; }
	else       { wj_arity_hit [d]++; for (i = d; i > 0; --i) sh [i] = sh [i - 1]; sh [0] = v; }
}

static void
wj_dump_arity (void)
{
	long long tot = wj_arity_miss, cum = 0; int i;
	for (i = 0; i < WJ_ARITY_WAYS; ++i) tot += wj_arity_hit [i];
	if (!tot) return;
	printf ("[wasm-jit vcall arity] of calls reaching the resolve helper (miss population), N-way LRU IC capture:\n");
	for (i = 0; i < WJ_ARITY_WAYS; ++i) {
		cum += wj_arity_hit [i];
		printf ("  <=%d-way: %5.1f%%  (depth %d: %lld)\n", i + 1, 100.0 * (double) cum / (double) tot, i, wj_arity_hit [i]);
	}
	printf ("  megamorphic (>%d-way): %5.1f%%  (%lld);  sampled %lld helper calls\n",
		WJ_ARITY_WAYS, 100.0 * (double) wj_arity_miss / (double) tot, wj_arity_miss, tot);
	fflush (stdout);
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
			guint32 w;
			if (wj_entry_edges [k].state != 2) continue;
			w = wj_entry_edges [k].window;
			if (!w || w > lastw || (w == lastw && k >= lasti)) continue;
			if (best < 0 || w > bestw || (w == bestw && k > best)) { best = k; bestw = w; }
		}
		if (best < 0) break;
		{
			WjEntryEdge *e = &wj_entry_edges [best];
			InterpMethod *cim = e->caller_im;   /* cached at record time — plain deref, NO Mono API on the main thread */
			printf ("  %8u (cum %u)  %s -> %s  [caller slot=%d hits=%d bail=%d%s%s]\n",
				e->window, e->count, e->caller_name ? e->caller_name : "?", e->callee_name ? e->callee_name : "?",
				cim ? cim->wasm_jit_slot : 0, cim ? cim->wasm_jit_hits : 0, cim ? cim->wasm_jit_bail : 0,
				cim && cim->wasm_jit_fail ? " gate=" : "", cim && cim->wasm_jit_fail ? cim->wasm_jit_fail : "");
		}
		lastw = bestw; lasti = best; shown++;
	}
	if (mono_wasm_jit_arity) wj_dump_arity ();   /* receiver-arity capture curve (MONO_WASM_JIT_ARITY=1); prints at each benchMeasure window-end */
	fflush (stdout);
}

/* Bail category -> short reason word for the dump lines. slot disambiguates bail==0 (aot-backed vs
 * simply not yet attempted). Keep in sync with the categorization at mini-wasm.c `done:`. */
static const char *
wj_bail_word (gint16 bail, gint32 slot)
{
	switch (bail) {
	/* slot > 0 means the method IS compiled and published — reporting that as "not-yet-jitted" made the
	 * whole iroute table read as an admission failure when the top entry (2.76M calls) had slot=550959,
	 * i.e. was already JITted and merely reached through interp_entry rather than called directly. */
	case 0:  return slot == -1 ? "aot-backed" : (slot > 0 ? "jitted" : "not-yet-jitted");
	case -2: return "EH-clauses";
	case -3: return "arg/ret-type";
	case -4: return "other-ir-shape";
	case -5: return "ldaddr";
	case -6: return "lcompare";
	case -7: return "byref";
	case -8: return "gshared-method";
	case -9: return "synchronized";
	case -10: return "eh-other";
	case -11: return "island-blocked(perm-leaf)";
	case -12: return "rgctx-callsite";
	default: return bail > 0 ? "opcode" : "?";
	}
}

/* Top-N island-blocking callees by block count, with WHY each can't JIT (wasm_jit_bail) — the most
 * actionable island signal: "if callee X were jittable, N island attempts would complete."
 * Followed by the execution-WEIGHTED top-N of permanently-unjittable vcall-residual callees
 * (wj_vperm_tab): which named methods carry the [wasm-jit vperm] per-reason weights, with the
 * emitter's exact gate string. */
EMSCRIPTEN_KEEPALIVE void
mono_wasm_jit_dump_blockers (int topn)
{
	gint32 lastc = 0x7fffffff; int lasti = -1, shown = 0;
	if (topn <= 0) topn = 40;
	printf ("[wasm-jit island blockers] un-JITted callees that blocked a caller's island:\n");
	while (shown < topn) {
		int best = -1, k; gint32 bestc = 0;
		for (k = 0; k < WJ_BLOCK_SLOTS; ++k) {
			gint32 c;
			if (wj_block_state [k] != 2) continue;
			c = (gint32) wj_block_cnt [k];   /* pure array read — no Mono API / locks in the hot scan */
			if (!c || c > lastc || (c == lastc && k <= lasti)) continue;
			if (best < 0 || c > bestc || (c == bestc && k > best)) { best = k; bestc = c; }
		}
		if (best < 0) break;
		{
			InterpMethod *im = wj_block_im [best];   /* cached — plain deref, NO Mono API on the main thread */
			gint16 bail = im ? im->wasm_jit_bail : 0;
			gint32 slot = im ? im->wasm_jit_slot : 0;
			printf ("  %8d  %-18s (slot=%d bail=%d) %s\n", bestc, wj_bail_word (bail, slot), slot, bail, wj_block_name [best] ? wj_block_name [best] : "?");
		}
		lastc = bestc; lasti = best; shown++;
	}
	/* weighted perm-vcall top-N: `weight | reason | bail | exact emitter gate | method` */
	printf ("[wasm-jit vperm top] perm-unjittable vcall-residual callees by executed residual count:\n");
	{
		gint64 lastw = G_MAXINT64; int lastk = -1;
		shown = 0;
		while (shown < topn) {
			int best = -1, k; gint64 bestw = 0;
			for (k = 0; k < WJ_VPERM_SLOTS; ++k) {
				gint64 w;
				if (wj_vperm_state [k] != 2) continue;
				w = wj_vperm_w [k];   /* pure array read — no Mono API / locks on the main thread */
				if (!w || !wj_vperm_im [k] || w > lastw || (w == lastw && k <= lastk)) continue;
				if (best < 0 || w > bestw || (w == bestw && k > best)) { best = k; bestw = w; }
			}
			if (best < 0) break;
			{
				InterpMethod *im = wj_vperm_im [best];   /* cached — plain deref */
				gint16 bail = im->wasm_jit_bail;
				const char *gate = im->wasm_jit_fail;    /* static literal set by the emitter (may be NULL, e.g. aot-backed) */
				printf ("  %10lld  %-18s (bail=%d%s%s) %s\n", (long long) bestw, wj_bail_word (bail, im->wasm_jit_slot), bail,
					gate ? " gate=" : "", gate ? gate : "", wj_vperm_name [best] ? wj_vperm_name [best] : "?");
			}
			lastw = bestw; lastk = best; shown++;
		}
	}
	/* interp-routed top-N: `executed count | bail reason | exact emitter gate | method`. A callee here has
	 * no AOT code AND no admitted f-slot, so every one of these calls paid interp_entry marshalling and then
	 * ran interpreted — orders of magnitude more than a JIT-to-JIT dispatch. Read the bail column first: a
	 * `not-yet-jitted` population is a THRESHOLD/admission problem, while named permanent gates are emitter
	 * work, and the two need opposite responses. */
	/* RETRY-state top-N. `vfb_thresh` is ~90% slot -3, and the event count alone cannot say whether that
	 * is a handful of methods livelocking on the compile CAS or a healthy population cycling through a
	 * 64-call back-off. Read the DISTINCT row count against the total: few rows with huge counts = a
	 * livelock worth fixing; many rows with small counts = normal churn and the pool is structural. */
	printf ("[wasm-jit retry top] slot=-3 (compile-BUSY back-off) callees by executed fallback count:\n");
	{
		gint64 lastw = G_MAXINT64; int shown2 = 0, distinct = 0, k2; gint64 sumw = 0;
		for (k2 = 0; k2 < WJ_RETRY_SLOTS; ++k2)
			if (wj_retry_state [k2] == 2) { distinct++; sumw += wj_retry_w [k2]; }
		while (shown2 < topn) {
			int best = -1, k; gint64 bestw = 0;
			for (k = 0; k < WJ_RETRY_SLOTS; ++k) {
				gint64 w;
				if (wj_retry_state [k] != 2) continue;
				w = wj_retry_w [k];
				if (w > bestw && w < lastw) { bestw = w; best = k; }
			}
			if (best < 0) break;
			printf ("  %10lld  slot=%d  %s\n", (long long) bestw, wj_retry_im [best]->wasm_jit_slot,
				wj_retry_name [best] ? wj_retry_name [best] : "?");
			lastw = bestw; shown2++;
		}
		printf ("  -- %d distinct methods, %lld total fallbacks (table holds %d)\n",
			distinct, (long long) sumw, WJ_RETRY_SLOTS);
	}
	printf ("[wasm-jit iroute top] interp-routed residual callees by executed count:\n");
	{
		gint64 lastw = G_MAXINT64; int lastk = -1;
		shown = 0;
		while (shown < topn) {
			int best = -1, k; gint64 bestw = 0;
			for (k = 0; k < WJ_IROUTE_SLOTS; ++k) {
				gint64 w;
				if (wj_iroute_state [k] != 2) continue;
				w = wj_iroute_w [k];
				if (!w || !wj_iroute_im [k] || w > lastw || (w == lastw && k <= lastk)) continue;
				if (best < 0 || w > bestw || (w == bestw && k > best)) { best = k; bestw = w; }
			}
			if (best < 0) break;
			{
				InterpMethod *im = wj_iroute_im [best];
				gint16 bail = im->wasm_jit_bail;
				const char *gate = im->wasm_jit_fail;
				/* fslot as well as slot: the two answer different questions and only together say whether
				 * this crossing COULD have been a direct call. slot>0 means the method is compiled;
				 * fslot>0 means it has a scalar f-thunk the caller could call_indirect straight into. A row
				 * with slot>0 and fslot==0 can only be entered through the e-slot, so its boundary cost is
				 * irreducible. A row with BOTH >0 is a site where self-healing simply was not emitted, and
				 * extending it there removes the crossing entirely. */
				printf ("  %10lld  %-18s (slot=%d fslot=%d bail=%d%s%s) %s\n", (long long) bestw,
					wj_bail_word (bail, im->wasm_jit_slot), im->wasm_jit_slot, im->wasm_jit_fslot, bail,
					gate ? " gate=" : "", gate ? gate : "", wj_iroute_name [best] ? wj_iroute_name [best] : "?");
			}
			lastw = bestw; lastk = best; shown++;
		}
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
	extern gboolean mono_wasm_jit_method_denied (MonoMethod *m);
	extern int mono_wasm_jit_colocate_deps_now (int desc_id);
	MonoWasmJitResult r;
	memset (&r, 0, sizeof (r));
	if (out)
		memset (out, 0, sizeof (*out));
	if (mono_wasm_jit_method_denied (im->method))
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
	/* Nothing gets past this gate any more: re-emission was the one caller that needed to, and it is
	 * gone. Without the gate a second worker recompiles the same method and leaks a fresh table-slot
	 * pair, re-emitting identical bytes (the duplicate WASM_JIT_REGISTERED). */
	if (im->wasm_jit_fslot > 0) {
		mono_atomic_store_i32 (&wj_compiling, 0);
		if (out) {
			out->desc_id = im->wasm_jit_desc;
			out->e_slot = im->wasm_jit_slot;
			out->f_slot = im->wasm_jit_fslot;
			out->bytes = im->wasm_jit_bytes;
			out->bytes_len = im->wasm_jit_bytes_len;
		}
		return WASM_JIT_COMPILE_JITTED;
	}
	/* MONO_WASM_JIT_COMPILE_TRACE: name the method around the compile, so a fault INSIDE the compiler can
	 * be attributed. The captured trap is an out-of-bounds access in jit_compile_method_with_opt_cb --
	 * mini's own frontend -- reached only from a JITted cold-miss edge (which is why MONO_WASM_JIT_DUMP_ONLY
	 * never reproduces it: nothing is registered there, so no JITted code runs to trigger the admission that
	 * compiles the target). Printed as a PAIR: compiles are serialized by the wj_compiling CAS above, so a
	 * COMPILING with no matching COMPILED is unambiguously the method that died. */
	{
		static int trace = -1;
		if (G_UNLIKELY (trace < 0))
			trace = g_getenv ("MONO_WASM_JIT_COMPILE_TRACE") != NULL;
		if (trace) {
			char *tn = mono_method_get_full_name (im->method);
			printf ("WASM_JIT_COMPILING %s\n", tn);
			g_free (tn);
			mono_wasm_force_compile (im->method, &r);
			tn = mono_method_get_full_name (im->method);
			printf ("WASM_JIT_COMPILED %s e=%d f=%d\n", tn, r.e_slot, r.f_slot);
			g_free (tn);
		} else {
			mono_wasm_force_compile (im->method, &r);
		}
	}
	mono_atomic_store_i32 (&wj_compiling, 0);
	if (out)
		*out = r;
	if (r.e_slot > 0) {
		extern void mono_wasm_jit_bind_logical (int desc_id, MonoMethod *logical_method);
		MonoMethod *logical_method = im->method;
		MonoJitMemoryManager *jit_mm = jit_mm_for_method (logical_method);
		mono_wasm_jit_bind_logical (r.desc_id, logical_method);
		/* Serialize against tiering.c replacing the InterpMethod. Whichever operation wins the jit-mm lock,
		 * the descriptor is either published to the replacement or copied by tier-up before replacement. */
		jit_mm_lock (jit_mm);
		im = (InterpMethod *)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, logical_method);
		im->wasm_jit_fslot = r.f_slot;
		im->wasm_jit_bytes = r.bytes;
		im->wasm_jit_bytes_len = r.bytes_len;
		im->wasm_jit_desc = r.desc_id;
		mono_memory_barrier ();   /* publish the immutable descriptor and compatibility fields before the slot gate */
		im->wasm_jit_slot = r.e_slot;
		jit_mm_unlock (jit_mm);
		/* MONO_WASM_JIT_HEAL_WAIT registered this method as a waiter on each of r.heal_callees[], so the
		 * callee's own publish would wake it for RE-EMISSION and the new body would replace the
		 * late-f-slot healing block with a plain direct call. Deleted with re-emission: the mechanism
		 * fired (90 of 107 sites woken) and its effect was nil, ceiling 0.057%. The healing SITES stay --
		 * R195: residual healing is also the tiering edge, so removing the guard removes re-emission's
		 * input rather than just a branch, and here there is no re-emission left to feed anyway. */
		wj_waiter_drain (logical_method);   /* event-driven wake: re-queue any methods parked waiting on this callee */
		/* CO-LOCATION, after publication and never before it. The method is already invocable at this
		 * point and stays invocable whatever happens next: a re-frame that fails leaves every member on
		 * the standalone module it already has. That ordering is the whole difference from every batching
		 * arm measured here, all of which DEFERRED publication until the group was built and paid +36% on
		 * boot for it. */
		mono_wasm_jit_colocate_deps_now (r.desc_id);
		return WASM_JIT_COMPILE_JITTED;
	}
	if (r.retriable) {
		/* retriable = blocked by un-JITted callee(s). Record each blocker (block_n is always counted as the
		 * Lever C cold-gate signal; the top-N report table is populated only under stats — see wj_block_note). */
		int i;
		for (i = 0; i < r.nblockers; i++)
			wj_block_note (r.blockers [i]);
		/* The bail histogram counts callee-not-jitted once per DISTINCT method (the emitter skips the
		 * bucket): the island driver re-emits a blocked method every iteration, so per-attempt counting
		 * measured island convergence, not terminal outcomes (profile4: 3066/7040 were re-attempts). */
		if (G_UNLIKELY (mono_wasm_jit_stats) && mono_atomic_cas_u8 (&im->wasm_jit_blocked_noted, 1, 0) == 0) {
			extern void mono_wasm_jit_bail_hist_note_blocked (void);
			mono_wasm_jit_bail_hist_note_blocked ();
		}
		return WASM_JIT_COMPILE_BLOCKED;
	}
	im->wasm_jit_bail = (gint16) r.bail;   /* permanent bail: record why, for the vcall-residual breakdown */
	im->wasm_jit_fail = r.fail_reason;     /* exact gate (static literal), for the weighted vperm top-N dump */
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
/* Retire a batch reservation without losing the slots.
 *
 * wasm_jit_resv_* has to be cleared the moment the batch ends, because mono_wasm_jit_get_callee_fslot hands
 * it out to OTHER methods so cycle members can bake each other's f-slot: a reservation that outlived its
 * batch would let an unrelated caller bake a slot nothing ever instantiates into.
 *
 * But mono_jiterp_allocate_table_entry is a bump allocator with no free, so simply zeroing the fields loses
 * the pair permanently and the next attempt allocates a new one — 2 entries per member per aborted batch,
 * scaling with batch size, which is exactly the axis module batching increases.
 *
 * So move the pair to wasm_jit_self_resv_*, which is private to the method (get_callee_fslot does not read
 * it) and is picked back up by phase 0 / the self-recursion emit. Same slots, no visibility hazard. */
static void
wj_park_reservation (InterpMethod *im)
{
	if (im->wasm_jit_resv_fslot > 0 && im->wasm_jit_self_resv_fslot <= 0) {
		im->wasm_jit_self_resv_eslot = im->wasm_jit_resv_eslot;
		im->wasm_jit_self_resv_fslot = im->wasm_jit_resv_fslot;
	}
	im->wasm_jit_resv_eslot = 0;
	im->wasm_jit_resv_fslot = 0;
}

static int
wasm_jit_compile_scc (MonoMethod **seed, int n_init, int *budget)
{
	extern void mono_wasm_force_compile (MonoMethod *m, MonoWasmJitResult *out);
	extern int mono_jiterp_allocate_table_entry (int type);
	extern int mono_wasm_jit_verbose;
	extern int mono_wasm_jit_residual_perm;
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
	/* All-or-nothing capacity gate. Reserving until the table runs dry leaves the batch half-reserved and
	 * burns every pair it did take (the allocator has no free), so check the whole seed up front and abort
	 * transiently instead — the members stay retriable and a later attempt can close. */
	{
		extern int mono_jiterp_table_remaining (int type);
		extern void mono_wasm_jit_note_table_exhausted (void);
		int need = 0;
		for (i = 0; i < n; i++) {
			InterpMethod *im = mono_interp_get_imethod (members [i]);
			if (im->wasm_jit_fslot <= 0 && im->wasm_jit_resv_fslot == 0 && im->wasm_jit_self_resv_fslot <= 0)
				need += 2;
		}
		if (need > mono_jiterp_table_remaining (1 /* JITERPRETER_TABLE_JIT_CALL */)) {
			mono_wasm_jit_note_table_exhausted ();
			ok = FALSE; goto out;
		}
	}
	/* Phase 0 — reserve a slot pair for every seed member so cross-cycle emits can bake each other's f-slot. */
	for (i = 0; i < n; i++) {
		InterpMethod *im = mono_interp_get_imethod (members [i]);
		if (im->wasm_jit_fslot > 0) { done [i] = TRUE; continue; }
		if (im->wasm_jit_slot == -1) { ok = FALSE; give_up = TRUE; goto out; }   /* seed member permanently un-JITtable -> can't close */
		if (im->wasm_jit_resv_fslot == 0) {
			int e, f;
			if (im->wasm_jit_self_resv_fslot > 0) {
				/* Reclaim a pair parked by an earlier aborted batch (or by a self-recursion emit) instead of
				 * allocating a new one — see the parking comment on the abort path below. */
				e = im->wasm_jit_self_resv_eslot; f = im->wasm_jit_self_resv_fslot;
			} else {
				e = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
				f = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
				if (e <= 0 || f <= 0) {
					extern void mono_wasm_jit_note_table_exhausted (void);
					mono_wasm_jit_note_table_exhausted ();
					ok = FALSE; goto out;
				}
			}
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
			if (results [i].e_slot > 0) {
				/* Identity check: a member with a reservation MUST have registered into it — fellow members
				 * have already baked that reserved f-slot into their modules. A bypass here (a compile that
				 * registered under a fresh pair, e.g. a method-substitution losing the reservation key) is
				 * the admit "fslot-unregistered" failure in the making; surface it at the source. */
				if (im->wasm_jit_resv_fslot > 0 && results [i].f_slot != im->wasm_jit_resv_fslot) {
					char *mn = mono_method_get_full_name (members [i]);
					printf ("WASM_JIT_RESV_BYPASS %s reserved f=%d registered f=%d\n", mn, im->wasm_jit_resv_fslot, results [i].f_slot);
					g_free (mn);
				}
				done [i] = TRUE; progress = TRUE; continue;
			}
			for (b = 0; b < results [i].nblockers; b++) {
				MonoMethod *bm = results [i].blockers [b];
				InterpMethod *bim = mono_interp_get_imethod (bm);
				int j, seen = 0;
				if (bim->wasm_jit_fslot > 0 || bim->wasm_jit_resv_fslot > 0) continue;   /* live or already a member */
				if (bim->wasm_jit_slot == -1) {
					/* The next emit can residual-route this permanent leaf.  Count that as
					 * progress so the member is retried instead of poisoning the SCC. */
					if (mono_wasm_jit_residual_perm) { progress = TRUE; continue; }
					ok = FALSE; give_up = TRUE; goto out;
				}
				for (j = 0; j < n; j++) if (members [j] == bm) { seen = 1; break; }
				if (seen) continue;
				if (n >= WJ_SCC_MAX) { ok = FALSE; give_up = TRUE; goto out; }           /* closure too large -> abort (perm) */
				{ int e, f;
				  if (bim->wasm_jit_self_resv_fslot > 0) {   /* reclaim a parked pair (see the abort path) */
					e = bim->wasm_jit_self_resv_eslot; f = bim->wasm_jit_self_resv_fslot;
				  } else {
					e = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
					f = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
					if (e <= 0 || f <= 0) {
						extern void mono_wasm_jit_note_table_exhausted (void);
						mono_wasm_jit_note_table_exhausted ();
						ok = FALSE; goto out;
					}
				  }
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
		/* --- SCC CO-LOCATION, ALWAYS ON ---------------------------------------------------------
		 *
		 * Every member has compiled into its own module and registered but is not yet invocable. Re-frame
		 * them all into ONE module now and repoint the registry at it. V8 cannot inline across a module
		 * boundary under any circumstances (an imported function has wire_byte_size_ == 0, so its
		 * InliningTree score is 0 -- inlining-tree.h:79-85), so co-location is what turns each intra-cycle
		 * call from a ~15-instruction call_indirect dispatch into a single `call rel32` that V8 may also
		 * inline through.
		 *
		 * This used to be gated behind MONO_WASM_JIT_BATCH_MODULE >= 2 and worked by running
		 * mono_wasm_force_compile once per member -- a whole mini_method_compile each -- because a member's
		 * emitted bytes baked module-dependent indices. That per-member re-compile is the +36% boot present
		 * in every batching arm ever measured here, and it is why all four of them lost. It is gone: the
		 * members' relocatable bodies are on their registry entries and mono_wasm_jit_rebatch frames them
		 * with a memcpy pass. There is no capture phase either, so the whole
		 * batch_begin/force_compile/divergence-check/batch_finish/batch_end protocol -- and the
		 * "batch_count() == k+1 && batch_member_method(k) == expected" check that guarded it -- goes with it.
		 *
		 * On any failure the members keep the standalone modules they already have; they are valid and
		 * installed, so a failed re-frame costs time, never correctness.
		 *
		 * MONO_WASM_JIT_SCC_COLOCATE=0 turns it off IN THE SAME BINARY, which is the only kind of A/B this
		 * workload's ~5% noise floor supports for a change this size. */
		extern int mono_wasm_jit_scc_colocate;
		if (mono_wasm_jit_scc_colocate) {
			extern int mono_wasm_jit_rebatch (const int *desc_ids, int n, void **out_bytes, int *out_len);
			int descs [WJ_SCC_MAX], idx [WJ_SCC_MAX];
			int bn = 0, k;

			/* Only members THIS call compiled: one that was already live (phase 0 marked it done) has no
			 * result and belongs to whatever module it came from. */
			for (i = 0; i < n; i++) {
				if (results [i].e_slot <= 0 || results [i].desc_id <= 0)
					continue;
				idx [bn] = i;
				descs [bn] = results [i].desc_id;
				bn++;
			}
			if (bn > 1) {
				void *bbytes = NULL;
				int blen = 0;
				if (mono_wasm_jit_rebatch (descs, bn, &bbytes, &blen)) {
					/* Publish the shared blob rather than the discarded standalone modules. */
					for (k = 0; k < bn; k++) {
						results [idx [k]].bytes = bbytes;
						results [idx [k]].bytes_len = blen;
					}
					if (mono_wasm_jit_verbose >= 1)
						printf ("WASM_JIT_SCC_COLOCATED members=%d bytes=%d\n", bn, blen);
				} else if (mono_wasm_jit_verbose >= 1) {
					printf ("WASM_JIT_SCC_COLOCATE_FAIL members=%d (keeping standalone modules)\n", bn);
				}
			}
		}
		/* Publish all. Every member is registered now, so once invocable a baked cross-cycle call_indirect
		 * resolves via ensure_fslot. Set fslot before the wasm_jit_slot gate (cross-thread visibility). */
		for (i = 0; i < n; i++) {
			InterpMethod *im;
			MonoJitMemoryManager *jit_mm = jit_mm_for_method (members [i]);
			extern void mono_wasm_jit_bind_logical (int desc_id, MonoMethod *logical_method);
			mono_wasm_jit_bind_logical (results [i].desc_id, members [i]);
			jit_mm_lock (jit_mm);
			im = (InterpMethod *)mono_internal_hash_table_lookup (&jit_mm->interp_code_hash, members [i]);
			if (im->wasm_jit_fslot > 0) {
				/* Raced to live elsewhere, so this batch's reservation went unused. PARK it rather than drop
				 * it (see the abort path): the allocator cannot take a slot back. */
				wj_park_reservation (im);
				jit_mm_unlock (jit_mm); continue;
			}
			im->wasm_jit_bytes = results [i].bytes;
			im->wasm_jit_bytes_len = results [i].bytes_len;
			mono_memory_barrier ();
			im->wasm_jit_fslot = results [i].f_slot;
			im->wasm_jit_desc = results [i].desc_id;
			mono_memory_barrier ();
			im->wasm_jit_slot = results [i].e_slot;
			/* Published INTO the reserved pair, so the slots are now owned by wasm_jit_fslot/_slot. Drop both
			 * the batch reservation and any parked pair so neither is handed out as spare capacity. */
			im->wasm_jit_resv_eslot = 0; im->wasm_jit_resv_fslot = 0;
			im->wasm_jit_self_resv_eslot = 0; im->wasm_jit_self_resv_fslot = 0;
			jit_mm_unlock (jit_mm);
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
		wj_park_reservation (im);
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
	extern int mono_wasm_jit_residual_perm;
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
				/* The emitter supports a residual call to a permanent leaf.  Re-emit m
				 * now that the callee's terminal state is visible instead of making the
				 * caller permanently un-JITtable as well. */
				if (mono_wasm_jit_residual_perm) { pulled++; continue; }
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
			if (_r < 0) {
				/* compile_publish records the bail reason but the outer hotness driver
				 * normally publishes slot=-1.  This is a recursive island attempt, so
				 * publish the terminal state here before asking the parent emitter to
				 * recognize the callee as residual-eligible. */
				if (wj_slot_retriable (cim->wasm_jit_slot)) {
					cim->wasm_jit_slot = -1;
					wj_waiter_drain (callee);
				}
				/* Retry this method so RESIDUAL_PERM can route only that edge through interp. */
				if (mono_wasm_jit_residual_perm) { pulled++; continue; }
				im->wasm_jit_bail = -11; ret = -1; goto pop;
			}
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
	/* Hotness gate. The bump MUST be atomic: wasm_jit_hits lives on the SHARED InterpMethod and is bumped
	 * from every worker (render/server/pool) in the auto-walk. A plain `++` loses updates AND lets one
	 * thread's write skip the exact threshold value — which under the old (non-atomic) `== thresh` test
	 * stranded a genuinely-hot method below the island trigger PERMANENTLY (kept running interpreted and
	 * re-counting: the dominant "below-threshold retry" tail; the bench proved the race live — ISLAND prints
	 * showed hits=501/96/66 at trigger time). The atomic inc is the actual fix: every value is now produced
	 * exactly once, so exactly ONE thread ever observes `== thresh` — no skip (no stranding) AND no herd. A
	 * `>=` test here is wrong: it fires on EVERY eligible caller at/above the threshold, so all concurrent
	 * callers of a freshly-hot method pile into force_island (one compiles, the rest early-out) and a RETRY
	 * method re-attempts every call — jit75 showed that as 82k island attempts/window for ~500 real compiles.
	 * `== thresh` (atomic) keeps the exactly-once trigger; the BUSY back-off re-runs the counter up to thresh
	 * again, so it still re-fires exactly once per cycle. */
	if (G_UNLIKELY (mono_wasm_jit_auto > 0) && wj_slot_hot_retry_eligible (cmethod->wasm_jit_slot) && mono_atomic_inc_i32 (&cmethod->wasm_jit_hits) == mono_wasm_jit_thresh) {
		int r;
		/* Don't whole-method-JIT a method that already has AOT code. MONO_WASM_JIT_OVER_AOT gated this,
		 * letting the same hotness/island machinery compile it again with the runtime wasm emitter and
		 * keeping code_type IMETHOD_CODE_COMPILED so any emit/admission failure still reached the AOT
		 * body -- which is what made it safer than trimming the AOT set, where an emitter-refused method
		 * drops to the INTERPRETER instead. Deleted because it STALLED AT BOOT, i.e. it was never usable.
		 * The pool it aimed at is real (12.63% of the in-game window is AOT-compiled MANAGED code) and is
		 * described where the knob was, in mini-wasm.c. */
		if (mono_interp_jit_call_supported (cmethod->method, mono_method_signature_internal (cmethod->method))) {
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
				mono_atomic_store_i32 (&cmethod->wasm_jit_hits, 0);   /* atomic: paired with the atomic inc above (PARKED is woken via wj_promote_q, not the counter) */
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_PARKED);
			}
		} else if (r == WASM_JIT_COMPILE_BUSY) {
			/* Another thread held the compile lock. Keep this distinct from a waiter-parked blocker state so
			 * event-driven sleepers don't re-enter threshold/promotion retries. Do NOT zero the counter: a
			 * BUSY is transient (the lock holder is compiling this or a sibling right now), so discarding a
			 * full threshold's worth of accrued hotness would force the method back through the interpreter
			 * for another ~thresh calls for no reason — the "erases progress under contention" half of the
			 * below-threshold-retry tail. Back off by a small fixed stride instead: the method re-attempts a
			 * handful of calls later (bounded — the `>=` gate + this stride mean no per-call storm) while
			 * keeping nearly all its progress. */
			if (wj_slot_retriable (cmethod->wasm_jit_slot)) {
				cmethod->wasm_jit_slot = WASM_JIT_SLOT_RETRY;
				mono_atomic_store_i32 (&cmethod->wasm_jit_hits, mono_wasm_jit_thresh > 64 ? mono_wasm_jit_thresh - 64 : 0);
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
	/* A byref is a POINTER, never the pointee's element type. The cases below switch on type->type,
	 * which for a byref carries the POINTEE's type, so `ref sbyte/byte/short/ushort` would write the
	 * sign/zero-extended LOW BYTE(S) OF THE POINTER (and `ref int`/`ref uint` likewise on a 64-bit
	 * host, where the I4/U4 cases below are compiled in). Delegate to stackval_to_data, whose byref
	 * case stores the full gpointer — and does so WITHOUT a write barrier, which the wasm-JIT residual
	 * scratch destination requires. Both siblings already guard byref first (stackval_to_data,
	 * stackval_from_data); this one did not.
	 *
	 * Callers that reach here with a raw byref-preserving type: wasm_jit_aot_call_lean (sig->ret) and
	 * interp_frame_arg_to_data (sig->ret, index == -1). interp_entry's tail only escaped because
	 * rmethod->rtype has already been normalized to MONO_TYPE_I by mini_get_underlying_type. */
	if (m_type_is_byref (type)) {
		stackval_to_data (type, val, data, pinvoke);
		return;
	}
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

/* Layout mirrored by mono_wasm_jit_sig_arg_offsets below (wasm-JIT e-thunk bakes these offsets) —
 * keep the two in sync. */
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

/*
 * mono_wasm_jit_sig_arg_offsets:
 *
 *   Interp-stack offset of each argument of SIG (this + params), laid out exactly like
 * initialize_arg_offsets above — MUST stay in sync with it. The wasm-JIT entry thunk bakes these
 * offsets as its arg-load immediates: for all-scalar signatures every arg is one MINT_STACK_SLOT_SIZE
 * slot so the offsets are exactly i*8 (byte-identical modules to the old hardcoded stride), but a
 * value-type arg occupies its ALIGN_TO'd size inline and shifts everything after it.
 *   Returns sig->hasthis + sig->param_count (the number of offsets written), or -1 if that exceeds
 * MAX. Callable at emit time (no InterpMethod needed).
 */
int
mono_wasm_jit_sig_arg_offsets (MonoMethodSignature *sig, guint32 *offs, int max)
{
	int index = 0, offset = 0;

	if (sig->hasthis + sig->param_count > max)
		return -1;
	if (sig->hasthis) {
		offs [index++] = 0;
		offset = MINT_STACK_SLOT_SIZE;
	}
	for (int i = 0; i < sig->param_count; i++) {
		MonoType *type = sig->params [i];
		int size, align;
		size = mono_interp_type_size (type, mono_mint_type (type), &align);

		offset = ALIGN_TO (offset, align);
		offs [index++] = offset;
		offset += size;
	}
	return index;
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
#ifdef HOST_BROWSER
		/* PUBLISH the trampoline info on the delegate itself, so the wasm method-JIT's emitted dispatch
		 * can reach the recipe cached on it (MonoDelegateTrampInfo.wasm_jit_recipe) in one load from
		 * `this` instead of deriving a site id and probing a per-site array.
		 *
		 * This is `invoke_info`'s DOCUMENTED purpose -- object-internals.h annotates the field
		 * `/* MonoDelegateTrampInfo *\/`, the full-JIT path stores exactly this pointer into it
		 * (method-to-ir.c:3710) and mono_delegate_trampoline reads it back as exactly this type
		 * (mini-trampolines.c:1019). Nothing on wasm wrote it before, so that reader saw NULL and fell
		 * back to its `arg`; giving it the real info can only make it more correct.
		 *
		 * Only when the info's key actually matches this delegate: the 1-element del_info cache above
		 * leaves *out_info NULL when imethod is shared by a SECOND delegate class, and publishing a
		 * mismatched info would hand generated code a recipe computed for a different Invoke
		 * signature. NULL simply means "no fast recipe", which the emitted guard already handles. */
		if (out_info && *out_info)
			del->invoke_info = *out_info;
#endif
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
		if (mono_aot_mode == MONO_AOT_MODE_LLVMONLY_INTERP) {
			mono_llvm_start_native_unwind ();
		}
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
	gpointer wj_vret; /* wasm-JIT residual hidden-vret destination (caller's return buffer). Captured by
	                   * mono_wasm_jit_call_interp ONCE at entry from scratch+WJ_SCRATCH_VRET_OFF — the
	                   * scratch slot itself is clobbered by any NESTED residual inside the callee, so the
	                   * writeback must use this LIFO-safe copy. Only meaningful when wj_entry_is_residual. */
} InterpEntryData;

#if HOST_BROWSER
#define WJ_SCRATCH_ARG_SLOTS 16
#define WJ_SCRATCH_RET_OFF 192   /* result slot; past the max args (WASM_FUNCTYPE_MAX_PARAMS*8 = 128) */
#define WJ_SCRATCH_VRET_OFF 232  /* hidden-vret: the CALLER's return-buffer address; the VT result is
                                  * memcpy'd through it (never via the 8-byte ret slot at 192, which a
                                  * large struct would overrun into the 200..231 control fields) */
#define WJ_SCRATCH_SIZE    256
/* scratch layout past the ret slot (offsets baked into JITted modules in mini-wasm.c — keep in sync):
 *   200 resolved vcall target MonoMethod* (vcall_resolve_fslot -> call_interp fallback)
 *   204 direct-delegate target MonoMethod*
 *   208 vcall resolved f-slot temp;  212 AOT call-target table index;  216 AOT rgctx
 *   220 direct-delegate argument-rewrite shape
 *   224 CALLING JITted method MonoMethod* (WASM_JIT_BADREF_ARG caller attribution)
 *   228 direct-delegate target e-slot (0 when residual marshalling is required)
 *   232 hidden-vret destination pointer (WJ_SCRATCH_VRET_OFF)
 *   236 direct-delegate target logical slot count;  240 target uses the scalar e-thunk ABI
 *   244 direct-delegate target f-slot; 248 closed/bound delegate target object
 *   252 direct-delegate target InterpMethod* -- C-SIDE ONLY, no emitted code reads or writes it.
 *       prepare_delegate_call has already resolved this (mono_interp_get_imethod, or the delegate IC's
 *       forwarded entry); handing it to call_interp saves that hash lookup on the delegate path, which
 *       is 97% of call_interp's traffic and where the pretransform memo structurally cannot hit.
 *       mono_interp_get_imethod measures 0.480%/0.513% of the client render thread with 88.8% of it
 *       under call_interp. Published last with the rest of the recipe and consume-cleared with it. */

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
#if HOST_BROWSER
/*
 * BRACKETING PROBE (MONO_WASM_JIT_LMF_PUBLISH_DIAG=1).
 *
 * exceptions-wasm.c's unwinder keeps finding the LMF chain head pointing at C-stack memory whose body was
 * never written (previous_lmf == 0, lmf_addr == 0, ->method = residue that differs every build). None of the
 * publishers account for it: mono_set_lmf instrumentation catches only the legitimate all-zero heap
 * first_lmf, mono_push_lmf tags bit 2 and this entry is untagged, mini-wasm.c refuses save_lmf methods
 * outright, and two attempts at initialising it in emit_push_lmf left the observed fields untouched.
 *
 * wasm has no watchpoints, so instead of asking "who writes it" ask "between which two points does the head
 * become bad". Sampling at labelled, nested choke points and reporting only the first good->bad TRANSITION
 * localises the writer to one region: if the head is sound entering AOT'd code and bad on the way out, the
 * writer is generated code; if it goes bad across the JIT boundary, it is the JITted body; and so on.
 *
 * first_lmf must be excluded -- it is legitimately all-zero (g_new0 plus an empty
 * MONO_ARCH_INIT_TOP_LMF_ENTRY), which is exactly the shape being hunted.
 */
void
mono_wasm_jit_lmf_bracket (const char *where)
{
	extern int mono_wasm_jit_lmf_publish_diag;
	MonoJitTlsData *jit_tls;
	MonoLMF *l;
	gboolean bad;
	static __thread gboolean was_bad;

	if (G_LIKELY (!mono_wasm_jit_lmf_publish_diag))
		return;
	l = mono_get_lmf ();
	jit_tls = mono_get_jit_tls ();
	bad = l && !((gsize) l & 3) && (gsize) l >= 65536 && (!jit_tls || l != jit_tls->first_lmf) &&
		!(((gsize) l->previous_lmf) & 2) && !l->lmf_addr;
	if (bad && !was_bad) {
		static int n;
		if (n++ < 20)
			printf ("WASM_JIT_LMF_BRACKET: head became bad at %s: lmf=%p prev=%p addr=%p method=%p\n",
				where, (void *) l, (void *) l->previous_lmf, (void *) l->lmf_addr, (void *) l->method);
	}
	was_bad = bad;
}
/* REVERTED (Round 83d): hoisting the flag test to the call site was cost-monotone in principle — the probe
 * already early-outs, so only the CALL and its spill band were being removed — but the build carrying it is
 * the only one in ~150 logs to produce `RuntimeError: unreachable` (4 occurrences, one a WORKER_TRAP_STACK,
 * during world load). Cause not established: the three earlier runs on that build never reached world load
 * (memory starvation), so "new bug" could not be separated from "first opportunity to hit an old
 * intermittent". A micro-optimisation whose predicted effect was "low single digits, likely unmeasurable" is
 * not worth carrying that risk, so the plain call is restored. Re-attempt only with a bisect that reaches
 * world load on both arms. */
#define WJ_LMF_BRACKET(where) mono_wasm_jit_lmf_bracket (where)
#else
#define WJ_LMF_BRACKET(where) do { } while (0)
#endif

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

		/* The entry ABI is asymmetric (produced by mini_get_interp_in_wrapper: `ldarg` for a byref
		 * param, `ldarg_addr` otherwise): for a BYREF param args[i] IS the pointer, for a by-value
		 * param it is a POINTER TO the value. This branch is one half of a two-sided contract — the
		 * wasm-JIT residual's half is wj_arg_slot_holds_pointer. Note stackval_from_data is itself
		 * byref-guarded, so dropping this branch would DOUBLE-DEREF rather than fail loudly. */
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
	gboolean wj_did_jit_call = FALSE;
	/* NOTE: this one flag gates two different things -- the hotness/compile trigger AND the protective
	 * entry LMF. Splitting them so delegate-invoke entries (is_invoke, which have no island LMF either)
	 * also got the LMF was TRIED and did not reduce the dangling-LMF-head events at all, so it was
	 * reverted rather than left in: it adds a push/pop to every delegate-invoke entry for no measured
	 * benefit. The leak is a SKIPPED POP elsewhere, not a missing push here -- see the
	 * WASM_JIT_LMF_IMBALANCE resync in mono_wasm_jit_invoke_caught. */
	gboolean wj_external_entry = !wj_entry_is_residual (data) && !rmethod->is_invoke;
	MonoLMFExt wj_entry_ext;
	/* Native/AOT callers enter an interpreted target here without executing a MINT_CALL or
	 * MINT_CALLVIRT_FAST in an interpreter caller. Advance the same full-method wasm-JIT hotness
	 * counter used by those opcodes, otherwise an override reached only from AOT (for example an
	 * Object.equals implementation called by an AOT collection helper) can tier to optimized interp
	 * code and accumulate arbitrarily-hot jiterpreter traces without ever attempting the full-method
	 * JIT. Do not count the immediate wasm-JIT residual entry: its JITted caller already resolved and
	 * counted the callee, and counting it again would make residual-heavy call sites promote at twice
	 * the configured rate. Delegate Invoke is replaced with a generated wrapper above; that wrapper
	 * dispatches its actual target through MINT_CALL, so leave it to the normal call-site trigger. */
	WJ_LMF_BRACKET ("ie-enter");
	if (G_UNLIKELY (wj_external_entry)) {
		/* Unlike a wasm-JIT residual caller, a native/AOT caller has no wasm island LMF protecting
		 * this transition. Compilation can safepoint, and a successfully compiled non-EH method has
		 * no IL_STATE island of its own, so keep the marshalled interp frame visible across both the
		 * compile and the possible immediate redirect. This is the same boundary MINT_CALL uses before
		 * mono_wasm_jit_invoke_caught; without it the unwinder reaches the AOT entry's incomplete plain
		 * save-LMF (method == NULL) and exceptions-wasm aborts. */
		interp_push_lmf (&wj_entry_ext, &frame);
		wasm_jit_maybe_compile (frame.imethod);
	}
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
	 * need_native_unwind tail propagates — same model as the AOT residual branch. A wasm-JIT residual caller
	 * already has an island LMF; a native/AOT entry uses wj_entry_ext above. Delegate-invoke (is_invoke)
	 * rewrites rmethod to the invoke wrapper, which dispatches its target via MINT_CALL — skip it here. */
	{
		gint32 wj_eslot = rmethod->wasm_jit_slot;
		if (G_UNLIKELY (wj_eslot > 0) && !rmethod->is_invoke) {
			extern int mono_wasm_jit_admit_live (int desc_id);
			/* Bring THIS thread's function table up to date, then confirm the slot actually instantiated
			 * here (sync can fail on a worker under memory pressure while the compiling thread succeeded;
			 * call_indirect-ing a mismatched placeholder would trap). If not live, fall through to interpret.
			 * _live, not plain admit: admit() also returns 1 when it BREAKS A CYCLE without instantiating. */
			if (G_LIKELY (mono_wasm_jit_admit_live (rmethod->wasm_jit_desc))) {
				extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
				mono_wasm_jit_invoke_caught (method, wj_eslot, frame.stack, frame.stack);
				wj_did_jit_call = TRUE;
			}
		}
	}
	WJ_LMF_BRACKET ("ie-body-done");
	if (G_UNLIKELY (wj_external_entry))
		interp_pop_lmf (&wj_entry_ext);
	WJ_LMF_BRACKET ("ie-popped");
	if (!wj_did_jit_call && G_UNLIKELY (wj_entry_is_residual (data))) {
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
		} else if (wj_entry_is_residual (data) && mono_mint_type (type) == MINT_TYPE_VT) {
			extern gboolean mono_wasm_jit_ret_is_byaddr (MonoType *t);
			/* wasm-JIT residual hidden-vret: memcpy the VT result through the caller's buffer pointer
			 * (data->wj_vret, captured at call_interp entry — the scratch+232 slot itself may have been
			 * clobbered by a nested residual inside the callee) — never via stackval_to_data into the
			 * 8-byte ret slot (a large struct would overrun the 200..231 scratch control fields).
			 * Destination is the caller's conservatively-scanned C-stack frame, not heap: plain
			 * memcpy, no barrier. */
			if (mono_wasm_jit_ret_is_byaddr (type)) {
				memcpy (data->wj_vret, frame.stack, mono_class_value_size (mono_class_from_mono_type_internal (type), NULL));
			} else {
				/* ArgVtypeAsScalar: return the inline struct bytes through the ordinary 8-byte
				 * scalar result slot; the JIT reloads its single field using the wasm ABI type. */
				int wj_vt_size = mono_class_value_size (mono_class_from_mono_type_internal (type), NULL);
				/* Only scalar vtypes (<=8 bytes, one field — the classification twin of the emitter's
				 * wj_scalar_vtype_valtype) may take this branch; anything larger is byaddr above. A
				 * bigger copy would overrun the ret slot into the 200..231 scratch control fields. */
				g_assert (wj_vt_size <= 8);
				memset (data->res, 0, 8);
				memcpy (data->res, frame.stack, wj_vt_size);
			}
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
 *   Allocate one inline-cache cell in the wasm heap (linear memory, zeroed; g_malloc0 is >=8-byte
 * aligned, which the i64 atomics below require) for a virtual call site in a wasm-JITted method:
 *     +0  N-way resolve IC entries: [i32 vtable | i32 InterpMethod* override]. Read inline by the
 *         emitted wasm (i64.atomic.load) AND by mono_wasm_jit_vcall_resolve_fslot; the +0 address is
 *         baked into the emitted wasm as an i32.const so the JITted code reads/updates it inline.
 *     +8*N fast-miss metadata: [(guint32) vtable/imt slot | (u32)(InterpMethod* base) << 32], lazily
 *         filled by resolve_fslot on the FIRST miss (both are a pure function of base_method, so a racy
 *         concurrent first-miss publish stores identical values) and used to drive get_virtual_method_fast
 *         (the interp's cached per-(vtable,slot) resolve) on every subsequent miss. Only the C helper
 *         reads it.
 *     optional N-way single-cast delegate dispatch recipes, present only at Delegate.Invoke sites.
 *     optional receiver-arity diagnostic shadow, present only when that diagnostic is enabled.
 * One per virtual call site, allocated once at JIT-emit time. Never freed (bounded: one per JITted
 * virtual call site).
 */
gpointer
mono_wasm_jit_alloc_ic (int delegate_site, MonoMethod *caller, MonoMethod *base)
{
	/* Layout: [ways x i64 IC entries][i64 fast-miss meta][optional delegate recipe][optional arity shadow]. `ways` and the arity
	 * flag are fixed at startup, so alloc-time and run-time (resolve_fslot / wj_arity_record) agree on the
	 * offsets. The arity shadow is allocated only when MONO_WASM_JIT_ARITY=1. */
	extern int mono_wasm_jit_arity, mono_wasm_jit_vcall_ways;
	int sz = 8 * (mono_wasm_jit_vcall_ways + 1);   /* N resolve entries + one fast-miss meta i64 */
	WjVcallSite *site;
	if (delegate_site) sz += wj_delegate_ic_size ();
	if (mono_wasm_jit_arity) sz += WJ_ARITY_WAYS * (int) sizeof (guint32);
	site = (WjVcallSite *) g_malloc0 (sizeof (WjVcallSite) + sz);
	site->caller_im = caller ? mono_interp_get_imethod (caller) : NULL;
	site->base = base;
	/* STABLE id, taken from this site's profile record rather than bumped fresh. A re-emission of the
	 * same (caller, base) site therefore lands on the SAME worker-local PIC entries, instead of starting
	 * empty and burning another id out of WJ_VCALL_SITE_MAX -- which wj_auto_batch_poll recorded as a
	 * defect in its own comments and which a continuous re-batcher would turn from untidy into fatal. */
	site->site_id = mono_wasm_jit_prof_site_id (site->caller_im, base,
		delegate_site ? WJ_SITE_DELEGATE : WJ_SITE_VIRTUAL, &wj_vcall_site_count);
	return (guint8 *) site + sizeof (WjVcallSite);
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
	extern int mono_wasm_jit_vcall_aot_ways;
	return g_malloc0 (16 * mono_wasm_jit_vcall_aot_ways);   /* N entries, each two atomic i64: +16k = vtab|((ti<<1|kind2)<<32), +16k+8 = vtab|(rgctx<<32) */
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

/*
 * Cold virtual misses need a marshalling frame whose reference slots remain valid while resolution
 * allocates or triggers a moving GC. Keeping 256 bytes in every generated caller's C-stack frame made
 * the steady-state hit path pay a larger prologue/epilogue and inflated deep virtual-call stacks.
 *
 * Allocate these frames lazily per worker instead. Each frame is permanently registered as a
 * conservative pinning root, cleared before it is returned to the pool, and selected by a depth
 * counter so resolver/cctor re-entry cannot overwrite an outer miss. We intentionally retain the
 * allocation/root for the worker lifetime: unregistering a TLS address during worker teardown races
 * the collector, while the maximum storage is only 256 bytes per observed nesting level.
 */
static __thread guint8 **wj_vcall_miss_frames;
static __thread gint32 wj_vcall_miss_frame_cap;
static __thread gint32 wj_vcall_miss_frame_depth;

extern int mono_wasm_jit_stats;

/*
 * TRUE iff the wasm-JIT caller spilled a POINTER (not the value) into this arg's scratch slot, so the
 * residual marshal must DEREF the slot rather than pass its address:
 *   - a BYREF param: the emitter spills the byref value, which IS the pointer.
 *   - a BY-ADDR VTYPE param (mono_wasm_jit_arg_is_byaddr, the emitter's classification twin): the
 *     emitter spills the ADDRESS of the caller's call-site copy in its GC-scanned C-stack frame.
 * Everything else is spilled by value, and interp_entry's arg convention wants a pointer TO the value.
 *
 * This is the C-side half of a two-sided contract; the other half is the byref branch in interp_entry's
 * arg loop (`if (m_type_is_byref (sig->params [i])) sval->data.p = params [i];`). Keep them in agreement:
 * stackval_from_data is ITSELF byref-guarded, so a desync produces a silent DOUBLE DEREF rather than a
 * crash — e.g. Unsafe.Add<byte> -> AddByteOffset(ref byte, IntPtr) would receive the address of the
 * scratch slot as its `ref` and clobber the buffer. Hence one shared predicate instead of two copies.
 */
static inline gboolean
wj_arg_slot_holds_pointer (MonoType *t)
{
	extern gboolean mono_wasm_jit_arg_is_byaddr (MonoType *t);
	return m_type_is_byref (t) || mono_wasm_jit_arg_is_byaddr (t);
}

/* Signature-shape cache: see the long note on InterpMethod.wasm_jit_ptrarg_mask.
 *
 * DERIVED FROM wj_arg_slot_holds_pointer and mono_mint_type, never a re-statement of them, because a
 * desync between the emitter's spill convention and this classification is a silent double deref rather
 * than a crash. Computed on the first crossing; every later crossing reads two fields. */
#define WJ_ARGSHAPE_COMPUTED 1u   /* this InterpMethod's shape has been derived */
#define WJ_ARGSHAPE_SCALAR   2u   /* scalar return AND no byref/VT param: the e-slot residual's gate */
#define WJ_ARGSHAPE_WIDE     4u   /* >32 params: ptrarg_mask cannot address them, use the predicate */

static guint8
wj_argshape_compute (InterpMethod *imethod, MonoMethodSignature *sig)
{
	guint8 shape = WJ_ARGSHAPE_COMPUTED;
	guint32 mask = 0;
	int i, np = (int) sig->param_count;
	gboolean scalar = mono_mint_type (sig->ret) != MINT_TYPE_VT;
	if (np > 32)
		shape |= WJ_ARGSHAPE_WIDE;
	for (i = 0; i < np; ++i) {
		MonoType *t = sig->params [i];
		if (scalar && (m_type_is_byref (t) || mono_mint_type (t) == MINT_TYPE_VT))
			scalar = FALSE;
		if (i < 32 && wj_arg_slot_holds_pointer (t))
			mask |= 1u << i;
	}
	if (scalar)
		shape |= WJ_ARGSHAPE_SCALAR;
	imethod->wasm_jit_ptrarg_mask = mask;
	/* Publish the mask BEFORE the COMPUTED bit: a concurrent reader that sees COMPUTED must see the
	 * mask that goes with it. Racing computations are otherwise benign -- the shape is a pure function
	 * of the signature, so two threads derive the identical answer. */
	mono_memory_barrier ();
	imethod->wasm_jit_argshape = shape;
	return shape;
}

static inline guint8
wj_argshape (InterpMethod *imethod, MonoMethodSignature *sig)
{
	guint8 shape = imethod->wasm_jit_argshape;
	if (G_UNLIKELY (!(shape & WJ_ARGSHAPE_COMPUTED)))
		shape = wj_argshape_compute (imethod, sig);
	return shape;
}

/* TRUE iff arg `i`'s scratch slot holds a POINTER rather than the value, from the cache. Falls back to
 * the predicate itself for the >32-param tail, which the mask cannot address. */
static inline gboolean
wj_argshape_ptr (guint8 shape, guint32 mask, MonoMethodSignature *sig, int i)
{
	if (G_UNLIKELY ((shape & WJ_ARGSHAPE_WIDE) && i >= 32))
		return wj_arg_slot_holds_pointer (sig->params [i]);
	return (mask & (1u << i)) != 0;
}

gpointer
mono_wasm_jit_scratch (void)
{
	return wj_scratch;
}

gpointer
mono_wasm_jit_vcall_miss_frame_acquire (void)
{
	gint32 depth = wj_vcall_miss_frame_depth;
	if (G_UNLIKELY (depth == wj_vcall_miss_frame_cap)) {
		gint32 oldcap = wj_vcall_miss_frame_cap;
		gint32 ncap = oldcap ? oldcap * 2 : 4;
		wj_vcall_miss_frames = (guint8 **) g_realloc (wj_vcall_miss_frames,
			(gsize) ncap * sizeof (guint8 *));
		memset (wj_vcall_miss_frames + oldcap, 0, (gsize) (ncap - oldcap) * sizeof (guint8 *));
		wj_vcall_miss_frame_cap = ncap;
	}
	if (G_UNLIKELY (!wj_vcall_miss_frames [depth])) {
		guint8 *frame = (guint8 *) g_malloc0 (WJ_SCRATCH_SIZE);
		/* A NULL descriptor is SGen's conservative/pinning-root form: object-looking words in this
		 * mixed scalar/reference frame pin their targets instead of being rewritten as typed refs. */
		gboolean ok = mono_gc_register_root ((char *) frame, WJ_SCRATCH_SIZE,
			MONO_GC_DESCRIPTOR_NULL, MONO_ROOT_SOURCE_JIT, frame, "wasm-jit vcall miss frame");
		g_assert (ok);
		wj_vcall_miss_frames [depth] = frame;
	}
	wj_vcall_miss_frame_depth = depth + 1;
	return wj_vcall_miss_frames [depth];
}

void
mono_wasm_jit_vcall_miss_frame_release (gpointer frame)
{
	gint32 depth = wj_vcall_miss_frame_depth;
	g_assert (depth > 0 && wj_vcall_miss_frames [depth - 1] == frame);
	/* Drop all conservative roots before making this level reusable. */
	memset (frame, 0, WJ_SCRATCH_SIZE);
	wj_vcall_miss_frame_depth = depth - 1;
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
	guint8 shape = wj_argshape (imethod, sig);
	guint32 ptrmask = imethod->wasm_jit_ptrarg_mask;
	for (i = 0; i < (int) sig->param_count; ++i) {
		int arg_offset = get_arg_offset_fast (imethod, NULL, idx + i);
		stackval *sval = STACK_ADD_ALIGNED_BYTES (sp, arg_offset);
		guint8 *slot = buf + (idx + i) * 8;
		if (m_type_is_byref (sig->params [i]))
			sval->data.p = *(gpointer*)slot;
		else if (wj_argshape_ptr (shape, ptrmask, sig, i))
			/* by-addr vtype: the slot holds the ADDRESS of the caller's call-site copy (GC-scanned
			 * C-stack frame); copy the VALUE onto the interp stack from there */
			stackval_from_data (sig->params [i], sval, *(gpointer*)slot, FALSE);
		else
			stackval_from_data (sig->params [i], sval, slot, FALSE);
	}

	InterpFrame frame = {0};
	frame.imethod = imethod;
	frame.stack = sp;
	frame.retval = sp;

	/* hidden-vret destination: capture BEFORE the callee runs — a nested residual inside the AOT
	 * callee reuses this per-thread scratch and clobbers +232 (LIFO discipline, like the args). */
	gpointer wj_vret_dst = *(gpointer *) (buf + WJ_SCRATCH_VRET_OFF);

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
		if (mono_mint_type (sig->ret) == MINT_TYPE_VT) {
			extern gboolean mono_wasm_jit_ret_is_byaddr (MonoType *t);
			/* hidden vret: memcpy the VT result through the caller's buffer pointer (captured above,
			 * before the callee could clobber +232 via a nested residual) — never via stackval_to_data
			 * into the 8-byte ret slot (a large struct would overrun the 200..231 control fields).
			 * Plain memcpy: the destination is the caller's C-stack frame (conservatively scanned). */
			if (mono_wasm_jit_ret_is_byaddr (sig->ret)) {
				memcpy (wj_vret_dst, sp, mono_class_value_size (mono_class_from_mono_type_internal (sig->ret), NULL));
			} else {
				memset (buf + WJ_SCRATCH_RET_OFF, 0, 8);
				memcpy (buf + WJ_SCRATCH_RET_OFF, sp, mono_class_value_size (mono_class_from_mono_type_internal (sig->ret), NULL));
			}
		} else {
			memset (buf + WJ_SCRATCH_RET_OFF, 0, 8);
			if (MONO_TYPE_IS_REFERENCE (sig->ret))
				*(gpointer*)(buf + WJ_SCRATCH_RET_OFF) = sp->data.o;   /* no write barrier: scratch is not GC-tracked */
			else
				stackval_to_data_sign_ext (sig->ret, sp, buf + WJ_SCRATCH_RET_OFF, FALSE);
		}
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

/* An immutable caller can outlive the point where its residual callee acquires an f-slot. Re-read the
 * baked imethod, follow interpreter tiering, and admit the module into this thread's function table.
 * The generated caller uses a nonzero result for a direct JIT->JIT call before any scratch spill. */
int
mono_wasm_jit_late_fslot (InterpMethod *imethod)
{
	if (!imethod)
		return 0;
	while (imethod->optimized_imethod)
		imethod = imethod->optimized_imethod;
	/* Direct residual edges used to be invisible to auto tiering: unlike an interpreter MINT_CALL,
	 * mono_wasm_jit_call_interp does not run wasm_jit_maybe_compile because its caller already resolved
	 * the callee. Consequently a method reached only through an immutable residual edge could never
	 * acquire the f-slot this helper was waiting for (profile19: residual_healed=0). Count this execution
	 * as the missing interpreter call edge. This runs before the generated caller spills arguments into
	 * GC-invisible scratch, so island compilation and any resulting GC remain safe. */
	if (wj_slot_hot_retry_eligible (imethod->wasm_jit_slot))
		wasm_jit_maybe_compile (imethod);
	while (imethod->optimized_imethod)
		imethod = imethod->optimized_imethod;
	if (imethod->wasm_jit_fslot > 0) {
		/* _live: admit() alone also succeeds on a cycle break, which instantiates nothing. Handing back an
		 * f-slot this thread never installed hands back the jiterpreter placeholder. */
		extern int mono_wasm_jit_admit_live (int desc_id);
		if (mono_wasm_jit_admit_live (imethod->wasm_jit_desc)) {
			if (G_UNLIKELY (mono_wasm_jit_stats))
				mono_wasm_jit_count (WJC_RESIDUAL_HEALED);
			return imethod->wasm_jit_fslot;
		}
	}
	return 0;
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
/* The direct residual emitter calls pretransform immediately before call_interp.  Keep the
 * resolved imethod in TLS so call_interp does not repeat interp_code_hash lookup on every
 * crossing (tens of millions during Minecraft startup).  A nested residual can run while
 * prepare_interp_callee executes, but this function publishes its own pair only after that
 * work returns, restoring the outer call's value.  Vcall targets and substituted wrappers
 * simply miss the identity check and use the normal lookup. */
static __thread MonoMethod *wasm_jit_pretransformed_method;
static __thread InterpMethod *wasm_jit_pretransformed_imethod;

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
	/* MONO_WASM_JIT_PRETIER bumped tiering for RESIDUAL callees here, on the theory that a residual edge
	 * is invisible to auto-tiering (call_interp does not run wasm_jit_maybe_compile, so a method reached
	 * only through residual edges never crosses the threshold -- the `profile19: residual_healed=0`
	 * failure). The placement was right: before the caller spills its refs into the GC-invisible scratch,
	 * which is why the bump could not live in call_interp at all. The EFFECT was -2.2% residual_healed,
	 * i.e. nothing. Deleted with re-emission. */
	wasm_jit_pretransformed_imethod = imethod;
	mono_memory_barrier ();
	wasm_jit_pretransformed_method = method;
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
static int wj_call_interp_inner (MonoMethod *method, guint8 *buf, InterpMethod *known_imethod);

int
mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf)
{
	return wj_call_interp_inner (method, buf, NULL);
}

/* `known_imethod`, when non-NULL, is an InterpMethod the CALLER has already resolved for exactly this
 * `method` and is handing over inside one crossing -- not a cache. Only the delegate path passes it (see
 * scratch+252). It is dropped the moment the SYNCHRONIZED_INNER substitution below changes `method`,
 * because then it describes a different method; that is the same hazard the pretransform memo's identity
 * check exists for. */
static int
wj_call_interp_inner (MonoMethod *method, guint8 *buf, InterpMethod *known_imethod)
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
			if (wrapped) {
				method = wrapped;
				known_imethod = NULL;   /* it described the wrapper, not the wrapped method */
			}
		}
	}
	/* The direct residual's immediately preceding pretransform already paid this hash lookup.
	 * Reuse it when the target identity still matches.  Synchronized-inner substitution changes
	 * `method`, and vcall fallback has no pretransform handoff, so both retain the old safe path. */
	InterpMethod *imethod = known_imethod ? known_imethod
		: method == wasm_jit_pretransformed_method
		? wasm_jit_pretransformed_imethod
		: mono_interp_get_imethod (method);
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	InterpEntryData data;
	int idx = 0, i;
	guint8 shape;
	guint32 ptrmask;

#ifdef HOST_BROWSER
	/* DIAG (type-confusion source): a JITted caller spilled its args into `buf` before this residual call.
	 * Using the callee SIGNATURE (so no 1A over-marking false positive), check every by-value REFERENCE arg
	 * (and `this`): its spilled value must be NULL or a plausible heap pointer. A non-pointer there — e.g. the
	 * -10 (0xfffffff6) that reached a (Biome[]) cast in Villager pathfinding — means the JIT put an int into a
	 * reference slot. Log (rate-limited) the callee + arg index so the SOURCE method is named; non-fatal.
	 *
	 * GATED ON OBJGUARD, and it shipped UNGATED (`#ifdef HOST_BROWSER` only) for the whole type-confusion
	 * hunt. Measured cost of that: MONO_TYPE_IS_REFERENCE is `mono_type_is_reference(t)`, a real CALL
	 * (metadata.h), and it read 0.197%/0.287% of the client render thread with 98.6% of it arriving under
	 * mono_wasm_jit_call_interp -- i.e. the diagnostic was directly visible in the in-game census, on a
	 * path taken ~8,500 times per frame. OBJGUARD rather than STATS because this is the interp-boundary
	 * half of exactly what OBJGUARD does on the emitted-store side ("validate the base is a live heap
	 * object, DEBUG ONLY"), and because a STATS run is a counter census that must not also change codegen
	 * -- the caller-attribution store this block reads (mini-wasm.c, scratch+224) is emitted under the
	 * same knob, so knob-off removes the store from every residual and delegate site as well. */
	extern int mono_wasm_jit_objguard;
	if (G_UNLIKELY (mono_wasm_jit_objguard)) {
		gsize _memsz = wj_memsz ();
		int _vidx = sig->hasthis ? 1 : 0;
		/* Caller attribution: every JITted residual/vcall-fallback site bakes ITS OWN MonoMethod* at
		 * scratch+224 immediately before calling here (mini-wasm.c), so on a bad ref we can name the
		 * type-confusion SOURCE. Plausibility-checked like the args (a stale/garbage word prints "?"). */
		MonoMethod *_caller = NULL;
		{ gsize _cp = (gsize) *(gpointer *) (buf + 224);
		  if (_cp >= 1024 && _cp < _memsz && !(_cp & 3)) _caller = (MonoMethod *) _cp; }
		if (sig->hasthis) {
			gsize _t = (gsize) *(gpointer *) (buf + 0);
			if (G_UNLIKELY (_t != 0 && (_t < 1024 || _t >= _memsz || (_t & 3)))) {
				static int _zt = 0;
				if (_zt++ < 40) { char *fn = mono_method_get_full_name (method); char *cn = _caller ? mono_method_get_full_name (_caller) : NULL; printf ("WASM_JIT_BADREF_ARG callee=%s this=0x%x caller=%s — JIT passed a non-pointer as `this` (type-confusion source)\n", fn, (unsigned) _t, cn ? cn : "?"); fflush (stdout); g_free (fn); g_free (cn); }
			}
		}
		for (int _p = 0; _p < (int) sig->param_count; ++_p) {
			gboolean _byref = m_type_is_byref (sig->params [_p]);
			gsize _v;
			if (!_byref && !MONO_TYPE_IS_REFERENCE (sig->params [_p]))
				continue;
			_v = (gsize) *(gpointer *) (buf + (_vidx + _p) * 8);
			/* BYREF arg: the slot holds an INTERIOR pointer, so it is not an object header and a byref to
			 * a byte/short field is legitimately UNALIGNED — range-check only, no `& 3` (the same shape as
			 * OBJGUARD kind 3). This arm used to `continue` past byref entirely, which left a stale
			 * interior pointer (the scratch is not a GC root) as the one spill class with NO diagnostic
			 * coverage at all — the class we most need named now that byref calls reach the residual. */
			if (_byref) {
				if (G_UNLIKELY (_v != 0 && (_v < 1024 || _v >= _memsz))) {
					static int _zb = 0;
					if (_zb++ < 40) { char *fn = mono_method_get_full_name (method); char *cn = _caller ? mono_method_get_full_name (_caller) : NULL; printf ("WASM_JIT_BADREF_ARG callee=%s arg#%d value=0x%x caller=%s — JIT passed an out-of-range BYREF arg (stale interior pointer / type-confusion source)\n", fn, _p, (unsigned) _v, cn ? cn : "?"); fflush (stdout); g_free (fn); g_free (cn); }
				}
				continue;
			}
			if (G_UNLIKELY (_v != 0 && (_v < 1024 || _v >= _memsz || (_v & 3)))) {
				static int _z = 0;
				if (_z++ < 40) { char *fn = mono_method_get_full_name (method); char *cn = _caller ? mono_method_get_full_name (_caller) : NULL; printf ("WASM_JIT_BADREF_ARG callee=%s arg#%d value=0x%x caller=%s — JIT passed a non-pointer as a reference arg (type-confusion source)\n", fn, _p, (unsigned) _v, cn ? cn : "?"); fflush (stdout); g_free (fn); g_free (cn); }
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
	/* Per-method signature shape, derived once and read on every later crossing. Both loops below were
	 * pure functions of `sig` recomputed ~8,500 times a frame; see InterpMethod.wasm_jit_ptrarg_mask.
	 *
	 * Derived from `imethod`, i.e. AFTER the SYNCHRONIZED_INNER substitution above, so it describes the
	 * same method `sig` does. Derived AFTER wasm_jit_prepare_interp_callee for a second reason: the
	 * classification runs mini_get_underlying_type / mono_class_from_mono_type_internal over the params,
	 * and that is where the two loops it replaces already sat. Hoisting it above the warmup would reach
	 * an unloadable param type BEFORE the gate that turns that into a catchable managed exception. */
	shape = wj_argshape (imethod, sig);
	ptrmask = imethod->wasm_jit_ptrarg_mask;
	/* INLINE AOT FASTPATH: if the callee has AOT code, run it natively via do_jit_call directly,
	 * skipping interp_entry's InterpEntryData marshalling. Covers BOTH the direct-call residual and the
	 * vcall-fallback (which funnels here after resolve). Cached on code_type like the interp MINT_JIT_CALL.
	 * WJC_AOT_ROUTED / WJC_INTERP_ROUTED measure the split (how much the fastpath actually fires). */
	{
		extern MonoMethod *mono_wasm_jit_ring [];
		extern int mono_wasm_jit_ring_count, mono_wasm_jit_ring_frozen;
		InterpMethodCodeType ct = imethod->code_type;
		if (ct == IMETHOD_CODE_UNKNOWN) {
			ct = mono_interp_jit_call_supported (method, sig) ? IMETHOD_CODE_COMPILED : IMETHOD_CODE_INTERP;
			imethod->code_type = ct;
		}
		if (ct == IMETHOD_CODE_COMPILED) {
			if (G_UNLIKELY (mono_wasm_jit_stats)) {
				mono_wasm_jit_count (WJC_RESIDUAL);
				mono_wasm_jit_count (WJC_AOT_ROUTED);
				if (!mono_wasm_jit_ring_frozen) { mono_wasm_jit_ring [mono_wasm_jit_ring_count & 127] = method; mono_wasm_jit_ring_count++; }
			}
			return wasm_jit_aot_call_lean (imethod, sig, buf);
		}
		if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_INTERP_ROUTED); wj_iroute_note (imethod); }
		/* DIRECT E-SLOT ENTRY: the callee has no AOT code but DOES have its own JITted wasm, and
		 * interp_entry would discover that and run it anyway (the e-slot fast path further down this file).
		 * Everything in between — this function's arg marshalling into InterpEntryData, interp_entry's
		 * InterpFrame setup, its LMF push/pop and its copy of the args onto the interp stack — is scaffolding
		 * for an interpreter run that will not happen.
		 *
		 * Measured cost of that scaffolding, in-game: interp_entry 3.838% of samples (671 instructions, 31.7%
		 * of them register spills) and this function 2.588% (1047 instructions, 34.3% spills), with 82% of all
		 * spills in the hot set landing within +-4 instructions of a call — i.e. the boundary is expensive
		 * precisely because it is a chain of C calls each spilling its live values.
		 *
		 * The scratch layout is ALREADY what the e-thunk wants: `this` at +0 and each scalar arg at +8, which
		 * is the same `(args_ptr)` contract interp_entry's own e-slot path documents. The delegate path
		 * already enters an e-slot from this very buffer (see mono_wasm_jit_call_delegate), so this is that
		 * same move applied to the ordinary residual.
		 *
		 * Gated on a SCALAR signature for the same reason the delegate recipe is: a by-value vtype or byref
		 * param means the slot holds an address rather than the value, and the e-thunk expects values. Gated
		 * on mono_wasm_jit_admit_live so the slot is known instantiated on THIS worker — call_indirect-ing a
		 * placeholder is a signature-mismatch trap that kills the thread, not a recoverable miss. Plain
		 * admit() is NOT that guarantee: it also returns 1 when it breaks a dependency cycle without
		 * instantiating anything. */
		if (mono_wasm_jit_eslot_residual && imethod->wasm_jit_slot > 0 && !imethod->is_invoke) {
			extern int mono_wasm_jit_admit_live (int desc_id);
			gboolean scalar = (shape & WJ_ARGSHAPE_SCALAR) != 0;
			if (scalar && mono_wasm_jit_admit_live (imethod->wasm_jit_desc)) {
				extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
				if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_RESIDUAL); mono_wasm_jit_count (WJC_ESLOT_RESIDUAL); }
				/* Clear the 8-byte result slot for the same reason the interp_entry path does: a sub-word
				 * return writes narrowly while the JITted caller reads a full-width i32, so stale high bytes
				 * from a previous residual would turn a `false` bool into a large nonzero value. */
				memset (buf + WJ_SCRATCH_RET_OFF, 0, 8);
				mono_wasm_jit_invoke_caught (method, imethod->wasm_jit_slot, buf, buf + WJ_SCRATCH_RET_OFF);
				/* THE RETURN VALUE IS INVERTED FROM WHAT IT LOOKS LIKE: this function returns 1 for THREW
				 * and 0 for success (see its header comment). Returning 1 here told every JITted caller that
				 * the callee had thrown, on every bypassed call — which aborted the caller into an interp
				 * unwind and killed boot with an InvocationTargetException during class loading. Report what
				 * actually happened, exactly as the normal tail does. */
				return get_context ()->has_resume_state ? 1 : 0;
			}
		}
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
		 * receive the address of the scratch slot as its `ref` and clobber the buffer.
		 * A BY-ADDR VTYPE param (mono_wasm_jit_arg_is_byaddr — the emitter's classification twin) also
		 * spilled an ADDRESS: the caller's call-site copy in its GC-scanned C-stack frame. Deref it too —
		 * interp_entry's stackval_from_data wants a pointer to the VALUE, which that copy is.
		 * Both cases are wj_arg_slot_holds_pointer, shared with wasm_jit_aot_call_lean -- read here out
		 * of the per-method shape cache rather than re-derived per crossing. */
		data.args [i] = wj_argshape_ptr (shape, ptrmask, sig, i) ? *(gpointer *) slot : slot;
	}
	data.res = buf + WJ_SCRATCH_RET_OFF;
	/* hidden-vret destination: capture ONCE, now — a nested residual inside the callee reuses this
	 * per-thread scratch and clobbers +232 before interp_entry's writeback runs (args are LIFO-safe
	 * because interp_entry copies them out at its start; this pointer is consumed at the END). */
	data.wj_vret = *(gpointer *) (buf + WJ_SCRATCH_VRET_OFF);
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
	/* FAST MISS PATH (mirror of mono_wasm_jit_vcall_resolve_fslot): resolve via the interp's cached
	 * get_virtual_method_fast instead of the uncached mono_object_get_virtual_method_internal (the profiled
	 * #1 game-thread cost). This is the lower-volume residual sibling with NO per-call-site IC cell, so the
	 * base imethod + vtable/imt slot are computed fresh: the base-method mono_interp_get_imethod is the SAME
	 * kind of locked lookup this path already did for the target below, and get_virtual_method_fast itself is
	 * O(1) after the first (vtable,slot) touch (the table is shared with MINT_CALLVIRT_FAST). It applies
	 * generic inflation + the synchronized/native wrappers (so the SYNCHRONIZED branch below is a no-op on
	 * its result) but — like the original of this path — does NOT apply the boxed-valuetype unbox wrapper.
	 * It asserts a non-NULL override: the emitter materializes callvirt's null check before reaching here, so
	 * a NULL override means a corrupt receiver (same contract MINT_CALLVIRT_FAST relies on). The resolve can
	 * still allocate/GC, and — as before — it runs HERE, before the JITted caller spills the call's ref args
	 * into the GC-invisible scratch, so the ref args are still on the GC-scanned shadow stack. */
	InterpMethod *base_im = mono_interp_get_imethod (base_method);
	InterpMethod *imethod = get_virtual_method_fast (base_im, this_obj->vtable, wj_virt_method_slot (base_method));
	MonoMethod *target = imethod->method;
	/* Synchronized override -> its SYNCHRONIZED wrapper (Monitor.Enter/Exit): the raw body has no monitor
	 * ops and mono_wasm_jit_call_interp's mono_interp_get_imethod does NOT substitute the wrapper -> the body
	 * would run without the monitor and a notify/wait throws IllegalMonitorStateException. get_virtual_method
	 * already applied it (so this is normally a no-op; kept as belt-and-braces). Re-derive imethod only when
	 * it fires — get_virtual_method_fast already returned the override's imethod on the common path, so we
	 * skip an extra locked mono_interp_get_imethod. */
	if (G_UNLIKELY (target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
		target = mono_marshal_get_synchronized_wrapper (target);
		imethod = mono_interp_get_imethod (target);
	}
	/* Pre-transform the override here too. The JITted code calls this BEFORE spilling the call's reference
	 * args into the GC-invisible scratch buffer; fully preparing a cold method (transform + code_type/
	 * signature warmup) can allocate -> GC. Doing it now (while the ref args still live in the GC-scanned ref
	 * shadow stack) lets a GC move them safely. mono_wasm_jit_call_interp then finds the imethod already
	 * prepared and only does the GC-free marshal + interp_entry, so the (now-spilled) scratch pointers stay
	 * valid. */
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
/* Field offset of MonoMethodILState.il_offset, for the emitter's inline per-bb IL-offset store (mini-wasm.c
 * cannot see the struct layout). Pairs with mono_wasm_jit_enter_island's return value. */
int
mono_wasm_jit_il_state_offset_off (void)
{
	return (int) G_STRUCT_OFFSET (MonoMethodILState, il_offset);
}

/* Field offset of InterpMethod.wasm_jit_fslot, for the emitter's inline virtual-IC fast path (which
 * loads imethod->wasm_jit_fslot directly in wasm). mini-wasm.c can't see the InterpMethod layout. */
int
mono_wasm_jit_imethod_fslot_off (void)
{
	return (int) G_STRUCT_OFFSET (InterpMethod, wasm_jit_fslot);
}

/* Cold ways of the compact JIT->AOT PIC. Return the address of a complete two-word entry so the
 * generated caller can use one common kind/argument/call tail for both way zero and cold-way hits. */
gpointer
mono_wasm_jit_vcall_aot_pic_lookup (MonoVTable *vt, gpointer aic)
{
	extern int mono_wasm_jit_vcall_aot_ways;
	guint64 *p = (guint64 *) aic;
	int k;

	if (!vt || !p || mono_wasm_jit_vcall_aot_ways <= 1)
		return NULL;
	for (k = 1; k < mono_wasm_jit_vcall_aot_ways; ++k) {
		guint64 a = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (p + 2 * k));
		guint64 b;
		if ((guint32) a != (guint32) (gsize) vt)
			continue;
		b = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (p + 2 * k + 1));
		if ((guint32) b == (guint32) (gsize) vt)
			return p + 2 * k;
	}
	return NULL;
}

static void
wj_vcall_aot_ic_fill (gpointer aic, MonoVTable *vt, gpointer addr, gpointer rgctx, int kind)
{
	extern int mono_wasm_jit_vcall_aot_ways;
	guint64 *p = (guint64 *) aic;
	int k, victim = -1;
	guint32 v = (guint32) (gsize) vt;
	guint32 ti_kind = ((guint32) (gsize) addr << 1) | (kind == 2 ? 1u : 0u);

	if (!p || !vt || !addr)
		return;
	for (k = 0; k < mono_wasm_jit_vcall_aot_ways; ++k) {
		guint64 a = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (p + 2 * k));
		guint64 b = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (p + 2 * k + 1));
		if ((guint32) a == v && (guint32) b == v)
			return;
		if (victim < 0 && (guint32) a == 0 && (guint32) b == 0)
			victim = k;
	}
	if (victim < 0)
		victim = (int) ((((gsize) vt >> 4) ^ ((gsize) vt >> 11)) % (guint) mono_wasm_jit_vcall_aot_ways);

	/* Publish payload word first, discriminator word last. Readers require both vtable tags to match,
	 * so any interleaved writer produces only a benign miss, never a mixed target/rgctx dispatch. */
	mono_atomic_store_i64 ((volatile gint64 *) (p + 2 * victim + 1),
		(gint64) (((guint64) (guint32) (gsize) rgctx << 32) | v));
	mono_atomic_store_i64 ((volatile gint64 *) (p + 2 * victim),
		(gint64) (((guint64) ti_kind << 32) | v));
}

enum {
	WJ_DELEGATE_NONE,
	WJ_DELEGATE_CLOSED_INSTANCE,
	WJ_DELEGATE_BOUND_STATIC,
	WJ_DELEGATE_OPEN_STATIC,
	WJ_DELEGATE_OPEN_INSTANCE
};

static gboolean
wj_delegate_cache_read_way (WjDelegateIC *cache, MonoMethod *source, MonoVTable *receiver_vt,
	MonoMethod **target, InterpMethod **imethod, int *shape, int *slots, gboolean *scalar)
{
	gint32 before = mono_atomic_load_i32 (&cache->seq);
	MonoMethod *cached_source, *cached_target;
	MonoVTable *cached_receiver;
	InterpMethod *cached_imethod;
	gint32 cached_shape, cached_slots, cached_scalar;

	if (!before || (before & 1))
		return FALSE;
	mono_memory_barrier ();
	cached_source = cache->source;
	cached_receiver = cache->receiver_vt;
	cached_target = cache->target;
	cached_imethod = cache->imethod;
	cached_shape = cache->shape;
	cached_slots = cache->slots;
	cached_scalar = cache->scalar;
	mono_memory_barrier ();
	if (before != mono_atomic_load_i32 (&cache->seq) || cached_source != source ||
	    cached_receiver != receiver_vt || !cached_target || !cached_imethod)
		return FALSE;
	*target = cached_target;
	*imethod = cached_imethod;
	*shape = cached_shape;
	*slots = cached_slots;
	*scalar = cached_scalar != 0;
	return TRUE;
}

static void
wj_delegate_cache_write_way (WjDelegateIC *cache, MonoMethod *source, MonoVTable *receiver_vt,
	MonoMethod *target, InterpMethod *imethod, int shape, int slots, gboolean scalar)
{
	gint32 before = mono_atomic_load_i32 (&cache->seq);
	if (before & 1)
		return;
	if (mono_atomic_cas_i32 (&cache->seq, before + 1, before) != before)
		return;
	cache->source = source;
	cache->receiver_vt = receiver_vt;
	cache->target = target;
	cache->imethod = imethod;
	cache->shape = shape;
	cache->slots = slots;
	cache->scalar = scalar ? 1 : 0;
	mono_memory_barrier ();
	mono_atomic_xchg_i32 (&cache->seq, before + 2);
}

static gboolean
wj_delegate_cache_read (WjDelegateIC *cache, MonoMethod *source, MonoVTable *receiver_vt,
	MonoMethod **target, InterpMethod **imethod, int *shape, int *slots, gboolean *scalar)
{
	extern int mono_wasm_jit_vcall_ways;
	for (int i = 0; i < mono_wasm_jit_vcall_ways; ++i) {
		if (wj_delegate_cache_read_way (&cache [i], source, receiver_vt,
			target, imethod, shape, slots, scalar))
			return TRUE;
	}
	return FALSE;
}

static void
wj_delegate_cache_write (WjDelegateIC *cache, MonoMethod *source, MonoVTable *receiver_vt,
	MonoMethod *target, InterpMethod *imethod, int shape, int slots, gboolean scalar)
{
	extern int mono_wasm_jit_vcall_ways;
	int victim = -1;

	/* Preserve an existing way for this key, otherwise fill an empty way before evicting. Concurrent
	 * duplicate fills are harmless: both recipes are pure functions of the same immutable key. */
	for (int i = 0; i < mono_wasm_jit_vcall_ways; ++i) {
		gint32 seq = mono_atomic_load_i32 (&cache [i].seq);
		if (!(seq & 1) && seq && cache [i].source == source && cache [i].receiver_vt == receiver_vt) {
			victim = i;
			break;
		}
		if (victim < 0 && !seq)
			victim = i;
	}
	if (victim < 0) {
		gsize hash = ((gsize) source >> 4) ^ ((gsize) receiver_vt >> 5);
		victim = (int) (hash % (guint) mono_wasm_jit_vcall_ways);
	}
	wj_delegate_cache_write_way (&cache [victim], source, receiver_vt,
		target, imethod, shape, slots, scalar);
}

/* Resolve a single-cast Delegate.Invoke to the delegate's real target while all call arguments are
 * still live in the JIT caller's GC-scanned shadow frame. The generated invoke wrapper is deliberately
 * bypassed: IKVM's hot MH<> delegates frequently select runtime-only invoke_bound wrappers, so asking
 * AOT for the wrapper merely enters the interpreter and the wrapper then crosses another boundary to
 * the dynamic target. Store a compact dispatch recipe in scratch for mono_wasm_jit_call_delegate(),
 * which runs only after the caller has spilled the arguments.
 *
 * Open virtual/value-type delegates still require the first invocation argument to resolve/unbox the
 * target. That argument is not available at this pre-spill point, so leave those uncommon shapes on the
 * proven generated-wrapper path. Multicast likewise needs the wrapper's invocation-list loop. */
static gboolean
wasm_jit_prepare_delegate_call (MonoDelegate *del, MonoMethod *invoke, guint8 *scratch, gpointer ic)
{
	MonoMethod *source, *target;
	MonoMethodSignature *isig, *tsig;
	InterpMethod *imethod;
	MonoVTable *receiver_vt = NULL;
	WjDelegateIC *cache = wj_delegate_ic (ic);
	int shape = WJ_DELEGATE_NONE;
	int slots = 0;
	int eslot = 0, fslot = 0;
	gboolean scalar = FALSE;
	ERROR_DECL (error);

	/* A multicast delegate can still retain a non-NULL `method` (normally the last target), so that
	 * field alone cannot distinguish it from a single-cast delegate. Only the generated Invoke wrapper
	 * owns the invocation-list iteration and last-result semantics. */
	if (!del || !del->method || ((MonoMulticastDelegate *) del)->delegates)
		return FALSE; /* multicast or not initialized */

	source = del->method;
	if (!m_method_is_static (source) && m_method_is_virtual (source) && del->target)
		receiver_vt = del->target->vtable;
	if (wj_delegate_cache_read (cache, source, receiver_vt, &target, &imethod, &shape, &slots, &scalar)) {
		/* Tiering replaces, rather than mutates, an InterpMethod. Follow the forwarding link so a recipe
		 * populated before tier-up does not permanently retain the unoptimized method. */
		InterpMethod *cached_imethod = imethod;
		while (imethod->optimized_imethod)
			imethod = imethod->optimized_imethod;
		if (imethod != cached_imethod)
			wj_delegate_cache_write (cache, source, receiver_vt, target, imethod, shape, slots, scalar);
		/* A recipe can be cached on the target's first invocation, before it crosses the auto-JIT threshold.
		 * Keep accumulating the same hotness/retry state as the uncached path; otherwise that first recipe
		 * permanently strands the target in the interpreter (profile13: registered -6%, interp-routed +40%).
		 * This can re-enter while compiling, so publish the scratch recipe only after it returns. */
		if (imethod->wasm_jit_slot <= 0)
			wasm_jit_maybe_compile (imethod);
		/* Function tables are per-thread. The cached InterpMethod identity is process-wide, but its e-slot
		 * must still be admitted on this worker before call_delegate can enter it. */
		if (imethod->wasm_jit_slot > 0) {
			extern int mono_wasm_jit_admit_live (int desc_id);
			if (mono_wasm_jit_admit_live (imethod->wasm_jit_desc)) {
				eslot = imethod->wasm_jit_slot;
				fslot = imethod->wasm_jit_fslot;
			}
		}
		/* Reaching this helper at all means the worker-local PIC missed, so re-publish there too: the
		 * shared cache can hit on a thread that has never dispatched this site (or whose way was
		 * evicted). Only an admitted scalar fslot is cacheable, which is what lets the generated hit
		 * skip both the scalar test and the liveness probe. */
		if (mono_wasm_jit_delegate_local_pic && scalar)
			wj_delegate_pic_publish (ic, source, receiver_vt, fslot, shape);
		*(MonoMethod **) (scratch + 200) = invoke;
		*(MonoMethod **) (scratch + 204) = target;
		*(gint32 *) (scratch + 228) = eslot;
		*(gint32 *) (scratch + 236) = slots;
		*(gint32 *) (scratch + 240) = scalar ? 1 : 0;
		*(gint32 *) (scratch + 244) = scalar ? fslot : 0;
		*(MonoObject **) (scratch + 248) = del->target;
		*(InterpMethod **) (scratch + 252) = imethod;
		*(gint32 *) (scratch + 220) = shape; /* publish scratch recipe last */
		return TRUE;
	}

	target = source;
	isig = mono_method_signature_internal (invoke);
	tsig = mono_method_signature_internal (target);
	if (!isig || !tsig)
		return FALSE;

	if (m_method_is_static (target)) {
		if (tsig->param_count == isig->param_count + 1) {
			/* Bound static: del->target holds the pre-bound FIRST argument. The generated invoke_bound
			 * wrapper UNBOX_ANYs it when the target's first parameter is a value type (emit_delegate_
			 * invoke_internal_ilgen); this direct recipe passes the raw del->target object with no unbox,
			 * so it is only correct for a REFERENCE-typed first parameter (same MONO_TYPE_IS_REFERENCE
			 * test the wrapper keys its unbox on). A value-type binding would pass the box's pointer
			 * bits as the value (scalar) or copy from the box HEADER (by-addr) — leave those on the
			 * proven wrapper path. */
			if (MONO_TYPE_IS_REFERENCE (tsig->params [0]))
				shape = WJ_DELEGATE_BOUND_STATIC;
		} else if (!del->target && tsig->param_count == isig->param_count)
			shape = WJ_DELEGATE_OPEN_STATIC;
	} else if (del->target) {
		/* llvmonly delegate initialization normally resolves this already. Repeat defensively for
		 * delegates initialized through an interpreter-only path, before args become GC-invisible. */
		if (m_method_is_virtual (target)) {
			target = mono_object_get_virtual_method_internal (del->target, target);
			if (!target)
				return FALSE;
			tsig = mono_method_signature_internal (target);
		}
		shape = WJ_DELEGATE_CLOSED_INSTANCE;
	} else if (!m_method_is_virtual (target) && !m_class_is_valuetype (target->klass) &&
	           isig->param_count == tsig->param_count + 1) {
		shape = WJ_DELEGATE_OPEN_INSTANCE;
	}

	if (shape == WJ_DELEGATE_NONE ||
	    ((isig->ret->type == MONO_TYPE_VOID) != (tsig->ret->type == MONO_TYPE_VOID)))
		return FALSE;

	if (target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		target = mono_marshal_get_synchronized_wrapper (target);
	else if (shape == WJ_DELEGATE_CLOSED_INSTANCE && m_class_is_valuetype (target->klass) &&
	         target->wrapper_type == MONO_WRAPPER_NONE)
		/* Keep the boxed delegate target in scratch[0]; the wrapper performs the unbox safely. */
		target = mono_marshal_get_unbox_wrapper (target);

	imethod = mono_interp_get_imethod (target);
	if (G_UNLIKELY (!wasm_jit_prepare_interp_callee (target, imethod, error))) {
		extern void mono_wasm_jit_throw (MonoObject *exc);
		mono_wasm_jit_throw ((MonoObject *) mono_error_convert_to_exception (error));
		return FALSE;
	}
	if (imethod->wasm_jit_slot <= 0)
		wasm_jit_maybe_compile (imethod);
	if (imethod->wasm_jit_slot > 0) {
		extern int mono_wasm_jit_admit_live (int desc_id);
		if (mono_wasm_jit_admit_live (imethod->wasm_jit_desc)) {
			eslot = imethod->wasm_jit_slot;
			fslot = imethod->wasm_jit_fslot;
		}
	}
	tsig = mono_method_signature_internal (target);
	slots = tsig->param_count + (tsig->hasthis ? 1 : 0);
	scalar = mono_mint_type (tsig->ret) != MINT_TYPE_VT;
	for (int i = 0; scalar && i < (int) tsig->param_count; ++i)
		scalar = !m_type_is_byref (tsig->params [i]) && mono_mint_type (tsig->params [i]) != MINT_TYPE_VT;
	wj_delegate_cache_write (cache, source, receiver_vt, target, imethod, shape, slots, scalar);
	if (mono_wasm_jit_delegate_local_pic && scalar)
		wj_delegate_pic_publish (ic, source, receiver_vt, fslot, shape);
	/* R187's OBJECT-REACHABLE RECIPE published here: the same (fslot, shape) the per-site PIC above
	 * writes, cached once on the (delegate class, target method) tramp info that interp_init_delegate
	 * hangs on del->invoke_info, so generated code could reach it in two loads from `this` with no site
	 * id, no capacity check, no key compare and no ways loop. Deleted with MONO_WASM_JIT_DELEGATE_OBJ_PIC
	 * -- see the note at that knob for why the keying worked 44,000x and the emitted stub still got
	 * bigger, and for why the wj_slot_live probe it could not drop is a structural consequence of
	 * "modules are compiled once and broadcast, but INSTANTIATED PER WORKER". MonoDelegateTrampInfo
	 * .wasm_jit_recipe and its offset accessor stay: they cost nothing and are the field an object-keyed
	 * design would use again, if one is ever built on per-callee AND per-worker storage. */

	/* Publish only after every potentially re-entrant operation above. A nested wasm-JIT vcall reuses
	 * this TLS scratch buffer; writing the complete outer recipe last prevents nested state leakage. */
	*(MonoMethod **) (scratch + 200) = invoke;
	*(MonoMethod **) (scratch + 204) = target;
	*(gint32 *) (scratch + 228) = eslot;
	*(gint32 *) (scratch + 236) = slots;
	*(gint32 *) (scratch + 240) = scalar ? 1 : 0;
	*(gint32 *) (scratch + 244) = scalar ? fslot : 0;
	*(MonoObject **) (scratch + 248) = del->target;
	*(InterpMethod **) (scratch + 252) = imethod;
	*(gint32 *) (scratch + 220) = shape;
	return TRUE;
}

/* Post-spill half of direct single-cast delegate dispatch. Its ABI intentionally matches
 * mono_wasm_jit_call_interp(method, scratch), allowing the emitter to select either helper without
 * duplicating its result/exception handling. `invoke` is retained only for that common ABI. */
int
mono_wasm_jit_call_delegate (MonoMethod *invoke, guint8 *scratch)
{
	MonoDelegate *del = *(MonoDelegate **) (scratch + 0);
	MonoMethod *target = *(MonoMethod **) (scratch + 204);
	int shape = *(gint32 *) (scratch + 220);
	int eslot = *(gint32 *) (scratch + 228);
	int slots = *(gint32 *) (scratch + 236);
	gboolean scalar = *(gint32 *) (scratch + 240) != 0;
	InterpMethod *timethod = *(InterpMethod **) (scratch + 252);

	/* CONSUME-CLEAR the recipe now that its fields are captured in locals. A recipe is only ever
	 * live between prepare_delegate_call's publish and this read (pure wasm spill code in between);
	 * leaving it set would let it leak across the per-thread scratch into a LATER residual on this
	 * thread: re-entrant work inside resolve_fslot / vcall_aot_target (compiles running cctors) can
	 * drive a nested delegate vcall, and without this clear the OUTER site's residual epilogue would
	 * read the nested call's stale shape at +220 and dispatch the wrong target with its own args —
	 * the same nested-clobber class as the scratch+200 re-store in resolve_fslot / vcall_aot_target. */
	*(gint32 *) (scratch + 220) = WJ_DELEGATE_NONE;
	*(InterpMethod **) (scratch + 252) = NULL;

	if (!del || !target || shape <= WJ_DELEGATE_NONE || shape > WJ_DELEGATE_OPEN_INSTANCE ||
	    slots < 0 || slots > WJ_SCRATCH_ARG_SLOTS)
		/* NOTE the callee here is `invoke`, not `target`: the recipe was rejected, so the resolved
		 * InterpMethod does not describe the method being entered and must not be handed over. */
		{
			if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_DELEGATE_SLOW_NORECIPE);
			return mono_wasm_jit_call_interp (invoke, scratch);
		}

	if (shape == WJ_DELEGATE_CLOSED_INSTANCE || shape == WJ_DELEGATE_BOUND_STATIC) {
		/* Both shapes preserve the original slot count: delegate `this` becomes the closed/bound
		 * target and every explicit Invoke argument remains at its existing +8 slot. */
		*(MonoObject **) (scratch + 0) = del->target;
	} else {
		/* Open static: drop delegate `this`. Open instance: the first Invoke argument becomes method
		 * `this`. In both cases the real target consumes exactly one fewer scratch slot. */
		memmove (scratch, scratch + 8, slots * 8);
	}

	/* Direct e-thunk entry is safe only for scalar signatures: its args pointer uses the interpreter's
	 * inline stackval layout, whereas a wasm-JIT residual stores by-address value types as pointers to
	 * caller copies. Let call_interp marshal those complex signatures, after which interp_entry can still
	 * redirect the real target to this same e-slot. */
	{
		/* The recipe's eslot was live when it was published, and the liveness bitmap is never cleared, so
		 * this is belt-and-braces rather than a second guard -- but it is one load, it is local to the
		 * call rather than to whoever wrote the recipe, and it is the check a future writer cannot forget.
		 * A slot that is not live here holds the jiterpreter prefill, whose type is not the thunk's. */
		extern int mono_wasm_jit_slot_live (int slot);
		if (eslot > 0 && !mono_wasm_jit_slot_live (eslot))
			eslot = 0;
	}
	if (eslot > 0 && scalar) {
		extern void mono_wasm_jit_invoke_caught (MonoMethod *, gint32, gpointer, gpointer);
		memset (scratch + WJ_SCRATCH_RET_OFF, 0, 8);
		mono_wasm_jit_invoke_caught (target, eslot, scratch, scratch + WJ_SCRATCH_RET_OFF);
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_INVOKE);
		return get_context ()->has_resume_state ? 1 : 0;
	}

	/* Which conjunct of the `eslot > 0 && scalar` gate above failed. Bumped HERE, where the fallback is
	 * actually taken, so "decided" and "happened" cannot be different populations (R215). */
	if (G_UNLIKELY (mono_wasm_jit_stats))
		mono_wasm_jit_count (eslot > 0 ? WJC_DELEGATE_SLOW_NONSCALAR : WJC_DELEGATE_SLOW_NOESLOT);
	/* Hand over the InterpMethod prepare_delegate_call already resolved for `target`. This is a handover
	 * inside one crossing, NOT a cache: it is published with the recipe, consume-cleared above, and the
	 * only code between the two is the caller's wasm spill sequence -- the same window scratch+204's
	 * `target` (which this path already dereferences) lives in. If that window ever widens, this slot
	 * moves with the rest of the recipe rather than being treated as durable state. */
	return wj_call_interp_inner (target, scratch, timethod);
}

int
mono_wasm_jit_vcall_resolve_fslot (MonoObject *this_obj, MonoMethod *base_method, guint8 *scratch, gpointer ic)
{
	guint64 *icp = (guint64 *) ic;
	MonoVTable *vt;
	InterpMethod *imethod;
	MonoMethod *target;
	/* Parent-class pointer check FIRST, name strcmp second. Both operands are pure, so the order is free to
	 * choose, and it is not free to get wrong: this runs on every trip through the vcall MISS path -- ~9% of
	 * ~350M in-game dispatches (mini-wasm.c's IC-sizing note) -- and virtually none of those sites are
	 * delegate invokes. Two loads and a pointer compare reject them; the strcmp then never runs. */
	gboolean delegate_site = m_class_get_parent (base_method->klass) == mono_defaults.multicastdelegate_class &&
		!strcmp (base_method->name, "Invoke");
	/* Clear the direct-delegate recipe before any early return or re-entrant work. */
	*(gint32 *) (scratch + 220) = WJ_DELEGATE_NONE;
	*(MonoMethod **) (scratch + 204) = NULL;
	*(gint32 *) (scratch + 228) = 0;
	*(gint32 *) (scratch + 236) = 0;
	*(gint32 *) (scratch + 240) = 0;
	*(gint32 *) (scratch + 244) = 0;
	*(MonoObject **) (scratch + 248) = NULL;
	*(InterpMethod **) (scratch + 252) = NULL;
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
	if (G_UNLIKELY (mono_wasm_jit_arity)) wj_arity_record (ic, vt, delegate_site);   /* receiver-diversity histogram (MONO_WASM_JIT_ARITY=1) */
	/* Read the (vtable | imethod<<32) pair ATOMICALLY. MC builds chunks on worker threads concurrently
	 * with the render thread, so the same vcall site's IC is written by multiple threads. A non-atomic
	 * i32-pair read can tear (match an old vtable but read a freshly-written imethod for a DIFFERENT
	 * receiver type) -> dispatch to the wrong override (NullPointerException) or a wrong-signature f-slot
	 * call_indirect (traps the worker -> GC can't suspend it). The i64 atomic makes the pair consistent. */
	extern int mono_wasm_jit_stats, mono_wasm_jit_vcall_ways;
	gboolean use_ic = TRUE;   /* virtual-dispatch resolve cache (always on) */
	int ic_ways = mono_wasm_jit_vcall_ways;
	gboolean ic_hit = FALSE;
	int ic_way = -1;
	if (use_ic) {
		/* N-way scan: first vtable match wins, in the SAME order the emitted inline IC checks the ways, so
		 * the C helper and the inline fast path stay in agreement. */
		int k;
		for (k = 0; k < ic_ways; ++k) {
			guint64 c = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (icp + k));
			if (G_LIKELY ((guint32) c == (guint32) (gsize) vt)) {
				imethod = (InterpMethod *) (gsize) (guint32) (c >> 32);   /* IC hit: skip the resolve + get_imethod */
				target = imethod->method;
				ic_hit = TRUE;
				ic_way = k;
				break;
			}
		}
	}
	if (ic_hit) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VIC_HIT);
		/* FOLLOW TIERING BEFORE READING THE F-SLOT. Tiering REPLACES an InterpMethod rather than mutating
		 * it (get_tier_up_imethod swaps the interp_code_hash entry and links old->optimized_imethod), and
		 * wasm_jit_compile_publish deliberately re-looks-up the CANONICAL imethod under the jit-mm lock
		 * before writing wasm_jit_fslot. So an IC entry cached before tier-up permanently addresses an
		 * object that will never receive an f-slot: the fslot check below reads 0 forever, every call falls
		 * back to interp_entry, and wasm_jit_maybe_compile can't help because the canonical method is
		 * already compiled. That is a PERMANENT residual on the hottest monomorphic sites — measured as
		 * 144,464 of 144,464 steady-state residuals in the jbox2d bench, ~99% of them one method
		 * (PolygonShape:computeAABB), with ic_hit tracking the residual count exactly.
		 * Re-cache the forwarded imethod so subsequent hits skip the walk. The delegate IC already does
		 * exactly this (wasm_jit_prepare_delegate_call); the vcall IC was missing it. */
		InterpMethod *cached_imethod = imethod;
		while (imethod->optimized_imethod)
			imethod = imethod->optimized_imethod;
		if (G_UNLIKELY (imethod != cached_imethod)) {
			target = imethod->method;
			mono_atomic_store_i64 ((volatile gint64 *) (icp + ic_way),
				(gint64) (((guint64) (guint32) (gsize) imethod << 32) | (guint32) (gsize) vt));
		}
	} else {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_VIC_MISS);
		/* FAST MISS PATH: resolve the override via the interp's per-(vtable,slot) cache
		 * (get_virtual_method_fast) instead of the uncached mono_object_get_virtual_method_internal — the
		 * profiled #1 game-thread cost (~150ns/call: a GC HANDLE_FUNCTION frame + mono_class_setup_vtable +
		 * mono_class_interface_offset_with_variance, run on EVERY call). get_virtual_method_fast indexes
		 * vtable->ee_data->interp_vtable[slot] — O(1) after the first (vtable,slot) touch — and fills it via
		 * get_virtual_method on a genuine miss. Crucially it is the SAME table MINT_CALLVIRT_FAST populates,
		 * so a POLYMORPHIC site whose receiver vtable keeps missing THIS site's monomorphic JIT IC (icp[0])
		 * still resolves in O(1) here — that is the whole point of the fast miss path.
		 *
		 * base_imethod + the vtable/imt slot are a pure function of base_method (fixed per call site), so we
		 * compute them ONCE on the first miss and cache them in the IC's 2nd i64 (icp[1] = base_imethod<<32 |
		 * (guint32)slot); a concurrent first-miss writer stores identical values, so the racy publish is
		 * benign. The slot formula mirrors transform.c:get_virt_method_slot (static there).
		 *
		 * get_virtual_method(_fast) already applies generic inflation + the synchronized/native wrappers (so
		 * the SYNCHRONIZED branch below is a no-op on its result), but NOT the boxed-valuetype unbox wrapper —
		 * the valuetype branch below still handles that. It asserts a non-NULL override, the same contract
		 * MINT_CALLVIRT_FAST relies on: the emitter's inline receiver null-check already converted a null
		 * `this` into a catchable NRE upstream (see the null-`this` guard at the top of this function), so
		 * only a corrupt receiver could reach a NULL override here, exactly as in the interpreter. */
		{
			guint64 meta = (guint64) mono_atomic_load_i64 ((volatile gint64 *) (icp + ic_ways));   /* fast-miss meta sits after the N IC entries */
			InterpMethod *base_im = (InterpMethod *) (gsize) (guint32) (meta >> 32);
			int gvm_slot;
			if (G_UNLIKELY (!base_im)) {
				base_im = mono_interp_get_imethod (base_method);
				gvm_slot = wj_virt_method_slot (base_method);
				mono_atomic_store_i64 ((volatile gint64 *) (icp + ic_ways),
					(gint64) (((guint64) (guint32) (gsize) base_im << 32) | (guint32) (gint32) gvm_slot));
			} else {
				gvm_slot = (gint32) (guint32) meta;
			}
			imethod = get_virtual_method_fast (base_im, vt, gvm_slot);
			/* Same tiering hazard as the IC-hit path above: interp_vtable[slot] is filled once and is not
			 * re-pointed when the override tiers up, so resolve the forwarding link BEFORE this imethod is
			 * cached into the IC — otherwise the entry we are about to publish is born stale. */
			while (imethod->optimized_imethod)
				imethod = imethod->optimized_imethod;
			target = imethod->method;
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
		/* `imethod` already holds get_virtual_method_fast's resolved override (the common no-wrapper case),
		 * so re-derive it ONLY when a wrapper below changes `target` — this skips a redundant, LOCKED
		 * mono_interp_get_imethod hash lookup (jit_mm_lock) on the hot miss path (~110k/frame). The
		 * SYNCHRONIZED branch is a no-op on a get_virtual_method result (which already applied the sync
		 * wrapper — the wrapper is not itself flagged SYNCHRONIZED); it stays as belt-and-braces. */
		if (G_UNLIKELY (target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
			target = mono_marshal_get_synchronized_wrapper (target);
			imethod = mono_interp_get_imethod (target);
		} else if (G_UNLIKELY (m_class_is_valuetype (target->klass) && target->wrapper_type == MONO_WRAPPER_NONE)) {
			/* Boxed-valuetype virtual/interface receiver: the resolved override's body expects an UNBOXED `this`
			 * (&data = boxed + sizeof(MonoObject)), but every wasm-JIT vcall path (f-slot fast, vcall_aot, and the
			 * call_interp residual) forwards the RAW boxed receiver loaded from the callsite vreg. The interp's own
			 * dispatch unboxes manually (MINT_CALLVIRT_FAST: `if valuetype -> mono_object_unbox_internal`); we can't
			 * adjust the JIT-loaded `this`, so route through the unbox wrapper instead — it does the +sizeof
			 * (MonoObject) and tail-calls the method, so passing the boxed `this` is correct on ALL three paths
			 * (and the IC caches the wrapper's imethod, so hits stay correct too). Without this, a valuetype
			 * override reads/writes fields against the object header -> garbage / type confusion. */
			target = mono_marshal_get_unbox_wrapper (target);
			imethod = mono_interp_get_imethod (target);
		}
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
		if (use_ic) {
			/* LRU-insert (vt -> imethod) as the MRU way, shifting the rest down (evict the oldest). Every
			 * entry is written as ONE atomic i64, so a concurrent inline reader always sees a whole, valid
			 * (vtable,imethod) pair in each way — reordering affects only hit rate, never correctness (the
			 * per-way vtable check + fslot/slot_live gate still guard every dispatch). This is what turns a
			 * 2-type site's misses into inline hits: both vtables end up cached in the 2 ways and neither
			 * evicts the other. */
			int k;
			for (k = ic_ways - 1; k > 0; --k)
				mono_atomic_store_i64 ((volatile gint64 *) (icp + k),
					mono_atomic_load_i64 ((volatile gint64 *) (icp + k - 1)));
			mono_atomic_store_i64 ((volatile gint64 *) icp,
				(gint64) (((guint64) (guint32) (gsize) imethod << 32) | (guint32) (gsize) vt));
		}
	}
	*(MonoMethod **) (scratch + 200) = target;   /* for the call_interp fallback (past RET_OFF(192)+8) */
	if (delegate_site && !strcmp (target->name, "Invoke") &&
	    m_class_get_parent (target->klass) == mono_defaults.multicastdelegate_class) {
		if (wasm_jit_prepare_delegate_call ((MonoDelegate *) this_obj, target, scratch, ic))
			/* The post-spill delegate helper will rewrite the ABI and invoke the real target. Returning an
			 * ordinary f-slot here would use Invoke's unmodified argument list and is therefore invalid. */
			return 0;
		/* Preparation failure can install a managed resume-state on non-CPPEH builds. Do not continue
		 * resolving/compiling Invoke in that state; make the shared residual epilogue abort immediately. */
		if (G_UNLIKELY (get_context ()->has_resume_state)) {
			*(MonoMethod **) (scratch + 200) = NULL;
			return 0;
		}
	}
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
		extern int mono_wasm_jit_admit_live (int desc_id);
		/* Only return the f-slot if THIS thread actually instantiated that module. sync_thread can fail to
		 * instantiate on a worker (OOM/CompileError under pressure) while it succeeded on the compiling
		 * thread; the slot then holds a jiterpreter placeholder and call_indirect-ing it traps the worker.
		 * Fall through to the interp residual (call_interp) instead.
		 *
		 * _live rather than plain admit, which is what this comment always MEANT: admit() also returns 1
		 * when the DFS finds the descriptor already `visiting` and breaks a cycle -- without instantiating
		 * anything. Worse here than at an e-slot, because wj_vcall_pic_publish below CACHES the result in
		 * the per-thread PIC, and because a placeholder whose type happens to match is not a trap at all:
		 * mono_jiterp_placeholder_jit_call's body writes 999 through its fourth argument and returns. */
		if (mono_wasm_jit_admit_live (imethod->wasm_jit_desc)) {
			/* Publish only after this worker admitted the target. Therefore a generated PIC hit can use
			 * the cached fslot without a second liveness test or an InterpMethod load. */
			wj_vcall_pic_publish (ic, vt, target, imethod->wasm_jit_fslot);
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
	/* Same nested-clobber discipline for the delegate recipe: this path did NOT publish one (a
	 * successful prepare returned earlier), so the residual epilogue's +220 select must see NONE.
	 * call_delegate consume-clears every recipe it reads, making this a defensive backstop against a
	 * nested delegate vcall (inside the re-entrant compile above) leaking its shape into this site. */
	*(gint32 *) (scratch + 220) = WJ_DELEGATE_NONE;
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
			case -12: mono_wasm_jit_count (WJC_VPERM_RGCTX); break; /* rgctx callsite (split out of gshared: per-site fixable) */
			default:
				if (imethod->wasm_jit_bail > 0) mono_wasm_jit_count (WJC_VPERM_OTHEROP);
				else mono_wasm_jit_count (WJC_VPERM_OTHER);     /* genuinely other IR shape */
				break;
			}
			wj_vperm_note (imethod);   /* per-METHOD weight behind the per-reason counters above */
		} else {
			mono_wasm_jit_count (WJC_VFB_THRESH);
			/* split by slot state: distinguishes a genuinely-cold target (slot 0, interp is acceptable) from
			 * a HOT method whose island won't close (slot -2 parked / -3 retry) — the latter interprets every
			 * call and is the real interp-residual driver, fixable only by closing its island, not by residual. */
			switch (imethod->wasm_jit_slot) {
			case 0:                    mono_wasm_jit_count (WJC_VFB_COLD); break;
			case WASM_JIT_SLOT_PARKED: mono_wasm_jit_count (WJC_VFB_PARKED); break;
			case WASM_JIT_SLOT_RETRY:  mono_wasm_jit_count (WJC_VFB_RETRY); wj_retry_note (imethod); break;
			/* slot > 0: the target IS compiled, so reaching the residual means mono_wasm_jit_admit_live
			 * said no on THIS worker -- its module is not instantiated here, or its descriptor is stuck
			 * mid-DFS (state 1, where admit returns 1 and desc_admitted returns 0). This arm used to be
			 * `default: break;`, and an uncounted route does not read as a gap: R166's 109.6M of these
			 * showed up only as VFB_THRESH being 9,243x the control with its three sub-counters summing
			 * to 1,399. Keep it counted even when it is zero. */
			default: mono_wasm_jit_count (WJC_VFB_NOTLIVE); break;
			}
		}
	}
	return 0;
}

/*
 * Cold virtual-call miss stub shared by every scalar signature.
 *
 * Generated callers retain only their necessarily signature-specific stores/return load. Resolution,
 * first-call JIT entry, delegate selection, AOT-PIC population and interpreter fallback live here once.
 * `frame` is a lazily acquired 256-byte worker-local buffer registered as a conservative pinning root
 * (not the re-entrant TLS residual scratch), so reference arguments remain valid while resolution can
 * allocate or trigger a moving GC.
 */
int
mono_wasm_jit_vcall_shared_miss (MonoObject *this_obj, MonoMethod *base_method, guint8 *frame,
	gpointer ic, gpointer aic)
{
	int fslot = mono_wasm_jit_vcall_resolve_fslot (this_obj, base_method, frame, ic);
	MonoMethod *target = *(MonoMethod **) (frame + 200);
	int delegate_shape = *(gint32 *) (frame + 220);

	if (G_UNLIKELY (!target))
		return 1; /* resolver already raised/installed the managed exception */

	if (fslot > 0) {
		/*
		 * `frame` has the compact outbound-residual layout (one 8-byte slot per wasm argument).
		 * Do not pass it directly to the e-thunk: that thunk consumes Mono's interpreter argument
		 * area, whose offsets are computed from mono_interp_type_size and need not match the compact
		 * layout for every scalar/generic signature.  This happened to work for the AOT-heavy
		 * workload, but no-AOT exposed wrong virtual returns while loading jvmdg's Java API.
		 *
		 * Marshal this one cold invocation through call_interp.  interp_entry copies the compact
		 * slots into its real argument area and then redirects an already-JITted target through its
		 * e-thunk.  resolve_fslot has already published the typed PIC, so every subsequent call still
		 * takes the direct f-slot path; this adds no steady-state dispatch overhead.
		 */
		return mono_wasm_jit_call_interp (target, frame);
	}

	if (delegate_shape != WJ_DELEGATE_NONE)
		return mono_wasm_jit_call_delegate (target, frame);

	/* Populate the existing AOT PIC on the first miss, but execute this single cold call through the
	 * generic interpreter bridge. The next call hits the caller's compact AOT guard and uses its typed
	 * direct body. C cannot portably invoke an arbitrary wasm signature itself. */
	if (aic) {
		extern int mono_wasm_jit_vcall_aot_target (guint8 *, MonoObject *, gpointer);
		(void) mono_wasm_jit_vcall_aot_target (frame, this_obj, aic);
	}
	return mono_wasm_jit_call_interp (target, frame);
}

/* Fast AOT-vcall dispatch (MONO_WASM_JIT_VCALL_AOT). Called by JITted code ONLY after a vcall_resolve_fslot
 * f-slot miss, which left the resolved override MonoMethod* at scratch+200. If that override is AOT-backed,
 * stash its AOT call-target table index at scratch+212 and its rgctx at scratch+216 and return 1 — the
 * JITted caller then call_indirects the AOT body directly (this+args+rgctx native ABI), skipping the
 * residual's double marshalling + do_jit_call frame. Returns 0 if not AOT-backed (caller takes the
 * residual). Same eligibility (no_wrapper / gsharedvt-variable bail + static-rgctx recovery) as the
 * INLINE_AOT direct path, via the shared mono_wasm_jit_aot_call_target.
 *
 * Delegate Invoke needs one extra resolution step here. The Invoke MonoMethod has no ordinary body; the
 * wrapper which implements it depends on the actual delegate instance (normal/open-virtual/bound-static).
 * `this_obj` is passed separately because this helper runs before the caller spills its arguments to scratch.
 *
 * Return code (the override is resolved at runtime, but the call_indirect functype is baked at emit time,
 * so the JITted caller picks one of two variants by this value):
 *   0 = not AOT-backed (take the residual)
 *   1 = AOT-backed, body HAS the trailing extra arg -> call (this,args,rgctx)->ret
 *   2 = AOT-backed, body has NO trailing arg (exempt wrapper kind) -> call (this,args)->ret */
int
mono_wasm_jit_vcall_aot_target (guint8 *scratch, MonoObject *this_obj, gpointer aic)
{
	extern gboolean mono_wasm_jit_aot_call_target (MonoMethod *m, gpointer *out_addr, gpointer *out_rgctx, gboolean *out_has_extra_arg);
	MonoMethod *target = *(MonoMethod **) (scratch + 200);
	MonoMethod *call_target = target;
	gpointer addr = NULL, rgctx = NULL;
	gboolean has_extra_arg = TRUE;
	gboolean ok;
	if (!target)
		return 0;
	/* A prepared single-cast delegate is dispatched after the argument spill. Do not repeat the failed
	 * generated-wrapper AOT lookup which profile10 showed on every MH<> invocation. */
	if (*(gint32 *) (scratch + 220) != WJ_DELEGATE_NONE)
		return 0;
	/* interp_entry performs this same substitution after crossing into the interpreter. Do it before the AOT
	 * lookup instead, so a fully-AOTted delegate wrapper can be called with the original Invoke signature and
	 * the JIT->interp transition is avoided. Keep `target` unchanged at scratch+200: if the selected shape was
	 * not precompiled, call_interp must receive the original Invoke and repeat the instance-aware selection. */
	if (this_obj && !strcmp (target->name, "Invoke") &&
	    m_class_get_parent (target->klass) == mono_defaults.multicastdelegate_class)
		call_target = mono_marshal_get_delegate_invoke (target, (MonoDelegate *) this_obj);
	ok = mono_wasm_jit_aot_call_target (call_target, &addr, &rgctx, &has_extra_arg);
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
	wj_vcall_aot_ic_fill (aic, this_obj ? this_obj->vtable : NULL, addr, rgctx, has_extra_arg ? 1 : 2);
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
			guint h = (guint) (((gsize) call_target >> 4) & 8191);
			if (wj_vaot_seen [h][0] != call_target && wj_vaot_seen [h][1] != call_target) {
				wj_vaot_seen [h][1] = wj_vaot_seen [h][0];
				wj_vaot_seen [h][0] = call_target;
				char *fn = mono_method_get_full_name (call_target);
				printf ("WASM_JIT_VCALL_AOT target=%s kind=%d extra_arg=%d inflated=%d wrapper=%d rgctx=%p\n",
					fn, has_extra_arg ? 1 : 2, has_extra_arg, call_target->is_inflated, call_target->wrapper_type, rgctx);
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
	/* Install THIS exception, not merely "one if none is recorded".
	 *
	 * The old `if (!thrown_exc)` guard exists for RE-throws: endfinally_rethrow sets thrown_exc = h and
	 * then rethrows the same object, and pass 1 is meant to reuse that handle rather than double-root it.
	 * But the guard cannot tell a re-throw of the SAME object from a NEW throw arriving while a stale
	 * handle lingers -- and a stale handle is reachable, because only a JIT landing pad clears
	 * thrown_exc. An exception that unwinds out of JIT code and is caught by an INTERPRETED or AOT frame
	 * leaves it set forever. The next throw from JIT code then keeps the OLD exception, so every landing
	 * pad loads and TYPE-MATCHES the wrong object.
	 *
	 * That is what the field evidence shows: an exception reported as OffThreadException unwinding out of
	 * LocalChannel.doBeginRead's `inboundBuffer.isEmpty()` (IL 22, confirmed genuine by the bb-header
	 * execution trail), a call that cannot raise it, on netty's read cycle where channelRead0 -- the only
	 * thing that throws OffThreadException -- does not even appear on the stack.
	 *
	 * Comparing the target object keeps the re-throw optimisation exact while replacing a stale handle. */
	if (jit_tls->thrown_exc) {
		MonoObject *cur = mono_gchandle_get_target_internal (jit_tls->thrown_exc);
		if (cur != exc) {
			/* Retarget in place: unlike free+new this cannot expose raw EXC across a handle allocation,
			 * and it preserves the established pinned-handle reuse discipline used by the other writers. */
			mono_gchandle_set_target (jit_tls->thrown_exc, exc);
		}
	} else {
		jit_tls->thrown_exc = mono_gchandle_new_internal (exc, TRUE);
	}
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

/* WHERE THE CORRUPTED UNWIND LMF COMES FROM (kept as a note; the scaffolding that found it is gone).
 *
 * A throwaway LMF-balance check here, plus a walk of the entries between the exit head and the entry head,
 * separated two populations at this boundary:
 *
 *  - the thread's own outer island chain (IL_STATE exts in the per-thread island array, contiguous:
 *    runtime_invoke -> ExecutionContext.RunInternal -> Thread/StartHelper.RunWorker -> Thread.threadProc ->
 *    threadProc2 -> ForkJoinWorkerThread.run -> ForkJoinTask.doExec -> CompletableFuture/AsyncSupply.run).
 *    These appear "above" the saved head only because pass 1 rewound the chain PAST it. Not a leak.
 *
 *  - one PLAIN entry at a C-STACK address whose previous_lmf is 0 and whose lmf_addr is 0, with ->method as
 *    uninitialised residue. previous_lmf == 0 rules out a push (mono_push_lmf writes old|2), and
 *    lmf_addr == 0 rules out a completed AOT save-LMF. So the chain head was published as a RAW STACK
 *    ADDRESS with the body never written -- the "AOT entry's incomplete plain save-LMF" this file already
 *    mentions above. It is that entry, not any interp/wasm-JIT push, that reaches
 *    mono_arch_unwind_frame and used to be handed to mono_compile_method_checked.
 *
 * Fixing it belongs in whatever publishes that save-LMF (AOT/LLVM prologue ordering), not here.
 * exceptions-wasm.c validates before compiling, which is what stops the crash meanwhile.
 */

void
mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret)
{
#ifdef HOST_WASM
	/* DIAGNOSTIC 2026-08-29 (co-location crash). Every caller is supposed to have proved this slot live on
	 * THIS worker -- via admit_live()/desc_admitted(), or a bare slot_live() at the recipe site. If that
	 * were true this branch is dead. It is not: MONO_WASM_JIT_COLOCATE_DEPS=1 traps in wasm_jit_ethunk_cb
	 * with `function signature mismatch`, which is what calling the jiterpreter prefill
	 * (i32,i32,i32,i32)->void as the thunk's (i32,i32)->void looks like.
	 *
	 * Log and STILL call: the trap follows immediately, but the line lands first and names the method, so
	 * the unguarded path is identified instead of inferred. Reading has already excluded the mapping
	 * (f{i}/e{i} export order), a crossed e/f, IMPORT_SLOT_CHANGED (0 in the repro), REBATCH_STALE (0), and
	 * both bitmaps being process-wide (both are __thread). */
	/* MONO_WASM_JIT_ESLOT_VERIFY's per-invoke probe stood here: read the table slot back from JS and
	 * check it is thunk-shaped (arity 2, not the jiterpreter prefill's 4), an EM_ASM per interp->JIT
	 * entry. It was the bring-up instrument for the prefilled-placeholder trap, and that trap now has a
	 * real guard -- mono_wasm_jit_slot_live, the per-thread bitmap, checked at this gate and at every
	 * f-slot bake. Deleted with the knob. */
#endif
	/* JIT frames live on the emscripten C stack: snapshot the SP so a caught C++ unwind (whose
	 * native landing pads may or may not have run for the torn-through JIT frames) can be resynced
	 * precisely, and a clean return can be balance-checked. */
	uintptr_t c_sp_saved = emscripten_stack_get_current ();
	/* NOTE: do NOT try to resync the LMF chain head here the way c_sp_saved resyncs the SP. It was tried,
	 * and it is wrong: pass 1 legitimately rewinds the chain PAST this boundary's entry head (measured --
	 * the head at exit was the thread's outer island chain: runtime_invoke -> ExecutionContext.RunInternal
	 * -> Thread.threadProc -> ForkJoinWorkerThread.run -> ...), so restoring the saved head resurrects
	 * frames the unwinder deliberately retired. That is the same hazard mono_wasm_jit_leave_island's
	 * conditional pop guards against, and the boot regressed from RENDERING to TIMEOUT with it in. */
	gboolean thrown = FALSE;
	WasmJitEThunkArgs a;

	/* Census hook (MONO_WASM_JIT_ENTRYCENSUS=1): this is the interp->JIT boundary, so it is the one
	 * choke point where "this worker actually entered that module" can be observed without touching
	 * generated code. Undercounts by design — direct JIT->JIT f-slot calls bypass it. See mini-wasm.c.
	 * Both symbols live in mini-wasm.c for the mono-aot-cross link reason noted above wj_prof_stat. */
	{
		extern int mono_wasm_jit_entry_census;
		extern void mono_wasm_jit_census_note_entry (int eslot);
		if (G_UNLIKELY (mono_wasm_jit_entry_census))
			mono_wasm_jit_census_note_entry (slot);
	}

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

	WJ_LMF_BRACKET ("wj-enter");
	mono_llvm_catch_exception (wasm_jit_ethunk_cb, &a, &thrown);
	WJ_LMF_BRACKET ("wj-exit");

	mono_wasm_jit_island_sp_restore (island_sp_saved);
	WJ_LMF_BRACKET ("wj-island-restored");
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

/* Address of the per-thread current-island il_state slot, for the emitter's INLINE per-bb IL-offset
 * store (imported global s.i). Same shape and same reason as mono_wasm_jit_slot_live_ptr_addr: the
 * ADDRESS of a __thread variable is fixed for the thread's life, so a module compiled once and
 * broadcast process-wide can still reach per-thread state -- the import is resolved per worker at
 * INSTANTIATION, which is what makes this legal where baking a constant would not be. */
MonoMethodILState **
mono_wasm_jit_cur_island_il_state_addr (void)
{
	return &mono_wasm_jit_cur_island_il_state;
}

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

/*
 * mono_wasm_jit_continue_unwind:
 *
 *   Called from JITted code when a residual/vcall/delegate callee reported threw==1. Its ONLY job is to
 * resume the native unwind that is already armed.
 *
 * The emitted code used to treat "abort" as a plain wasm RETURN, which loses the exceptional control-flow
 * edge: the boundary (mono_wasm_jit_invoke_caught) has caught the C++ unwind and deliberately KEPT the
 * exception for "a JITted island still ABOVE us", so returning normally means this frame's own landing pad
 * and every JIT frame above it are skipped. The exception then sits in context->has_resume_state until an
 * unrelated later interp return trips need_native_unwind and fires it at a frame that cannot handle it.
 *
 * Deliberately does NOT call mono_wasm_jit_rethrow/mono_wasm_jit_throw/mono_handle_exception: pass 1 has
 * already run and installed the resume state. Re-running it could process finally clauses twice, replace
 * the resume state, or trip the existing resume-state gchandle assertions. This must only perform the
 * unwind, exactly as wasm_jit_aot_call_lean already does for the JIT->JIT f-slot path.
 */
void
mono_wasm_jit_continue_unwind (void)
{
	if (G_UNLIKELY (mono_opt_llvm_emulate_unwind)) {
		/* Emulated unwind sets a flag and RETURNS instead of throwing; the wasm JIT's in-method EH is
		 * built on real wasm/C++ EH and has no flag-polling in its emitted code, so there is no correct
		 * behaviour here. Fail loudly rather than falling through to `unreachable` (an opaque worker
		 * trap) and rather than restoring the old dummy-return, which is the bug this replaces. */
		g_error ("wasm-jit: in-method EH requires real wasm EH; emulate-unwind is unsupported");
	}
	mono_llvm_start_native_unwind ();
	g_assert_not_reached ();   /* the C++ throw above does not return */
}


/* Returns the MonoMethodILState this activation pushed, so a JITted prologue can keep it in a wasm local and
 * store its il_offset inline at each basic block instead of calling mono_wasm_jit_set_il_offset per bb. */
gpointer
mono_wasm_jit_enter_island (MonoMethod *method, int ndata)
{
	WjIsland *is;
	MonoMethodILState *il;
	gsize zbytes;
	is = wj_island_at (wj_island_sp++);
	il = (MonoMethodILState *) is->st;
	/* ZERO ONLY WHAT THIS METHOD CAN USE. `st` is sized for WJ_ISLAND_DATA (256) args+locals, so the old
	 * unconditional `sizeof (is->st)` memset cleared ~1040 bytes on EVERY EH-method entry -- ~260 i32
	 * stores for a method that typically has ~10 args+locals, on an EH-dense Java workload. Measured
	 * 0.83-0.92 M instr/frame in enter_island on the client render thread.
	 *
	 * Sound because nothing reads past `ndata`: every consumer indexes `il_state->data [findex]` at a
	 * SIGNATURE-DERIVED index (interp.c's il_state entry path) and NULL-checks each read, so entries
	 * beyond the method's own args+locals are unreachable. And they are not GC roots -- island chunks are
	 * plain g_malloc0 (`wj_island_at`), never mono_gc_register_root'ed, and no GC path handles
	 * MONO_LMFEXT_IL_STATE -- so stale bytes past `ndata` cannot be scanned either. (Frames ARE recycled
	 * via wj_island_sp, so a shallow method can inherit a deeper one's tail; that is exactly the case the
	 * two properties above make harmless.)
	 *
	 * The header zeroing is retained but is itself redundant: `method` and `il_offset` are both assigned
	 * immediately below. Kept so `ext`/`data` alignment padding stays deterministic.
	 *
	 * MONO_WASM_JIT_ISLAND_NDATA gated this and is gone. It was worth ~0.1 M instr/frame, roughly 0.05%
	 * of the client render thread -- real, and below what this box can resolve, so the A/B the knob
	 * existed for could never have been answered. The ABI parameter stays and the better arm is now
	 * unconditional; the range test below still covers an out-of-range count. */
	if (ndata < 0 || ndata > WJ_ISLAND_DATA)
		zbytes = sizeof (is->st);
	else
		zbytes = sizeof (MonoMethodILState) + (gsize) ndata * sizeof (gpointer);
	memset (is->st, 0, zbytes);
	il->method = method;
	il->il_offset = -1;
	memset (&is->ext, 0, sizeof (is->ext));
	is->ext.kind = MONO_LMFEXT_IL_STATE;
	is->ext.il_state = il;
	is->finally_sp = wj_finally_exc_sp;
	mono_push_lmf (&is->ext);
	is->prev = mono_wasm_jit_cur_island_il_state;
	mono_wasm_jit_cur_island_il_state = il;
	return il;
}

void
mono_wasm_jit_leave_island (void)
{
	WjIsland *is;
	if (G_UNLIKELY (wj_island_sp <= 0))
		return;
	is = wj_island_at (--wj_island_sp);
	/* Exception pass 1 can already have rewound the TLS LMF past this island to
	 * an outer AOT/interpreter handler.  In that case restoring the predecessor
	 * captured on island entry resurrects frames which the unwinder deliberately
	 * retired.  Only unlink the island when it is still the current TLS top.
	 *
	 * Splicing it out wherever it sits was tried instead (to close a slot-recycling hazard) and made no
	 * difference to the corruption it was aimed at -- the real fault was the landing pad never restoring
	 * the head, see wj_eh_restore_lmf -- so this is back to the simpler conditional form. */
	if (mono_get_lmf () == &is->ext.lmf)
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
		if (mono_get_lmf () == &is->ext.lmf)
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

/* Find the innermost active wasm-JIT EH invocation of METHOD. The landing pad normally belongs to the
 * current island, but matching by method makes that ownership explicit and remains correct while an
 * inner EH method is unwinding through its own landing pad. Searching from the top also selects the
 * right activation for recursion. */
static int
wj_island_il_offset_for_method (MonoMethod *method)
{
	int i;

	if (!method || !wj_island_chunks)
		return -1;
	for (i = wj_island_sp - 1; i >= 0; --i) {
		WjIsland *is = wj_island_at (i);
		MonoMethodILState *il = (MonoMethodILState *) is->st;
		if (il->method == method)
			return il->il_offset;
	}
	return -1;
}

/* Innermost live island frame for METHOD, or NULL. Same search as wj_island_il_offset_for_method (top-down,
 * so recursion selects the right activation); returned so the catch path can restore the LMF head. */
static WjIsland *
wj_island_for_method (MonoMethod *method)
{
	int i;

	if (!method || !wj_island_chunks)
		return NULL;
	for (i = wj_island_sp - 1; i >= 0; --i) {
		WjIsland *is = wj_island_at (i);
		MonoMethodILState *il = (MonoMethodILState *) is->st;
		if (il->method == method)
			return is;
	}
	return NULL;
}

/*
 * Restore the LMF chain head to the island of the JITted EH method that is about to resume in its landing pad.
 *
 * mono's own two-pass EH does this: on finding a handler it calls mono_set_lmf (lmf) for the handler's frame
 * (mini-exceptions.c). The wasm JIT instead unwinds natively (cppeh) into its own landing pad and never touched
 * the chain, so after a catch the head still pointed at the innermost LMF of a frame the native unwind had
 * already destroyed -- a plain LMF on abandoned C stack whose body reads all-zero once the stack is reused.
 * That is exactly the head the bracketing probe kept reporting (0x1472xxxx, prev=0, lmf_addr=0, not first_lmf),
 * and it is why three fixes aimed at do_jit_call's pop changed nothing: the pop faithfully restores what it
 * captured; the head was already bad before the push.
 *
 * This is NOT the boundary resync that interp.c:6855 records as tried-and-reverted. That one restored a head
 * captured BEFORE the call, after the call had finished, where pass 1 may legitimately have rewound past it.
 * Here the method is RESUMING: its island is live, its callers are intact, and the island ext is precisely the
 * chain state that method should run under -- the same thing mono installs for a handler frame.
 */
static void
wj_eh_restore_lmf (MonoMethod *method)
{
	WjIsland *is = wj_island_for_method (method);

	if (!is)
		return;
	if (mono_get_lmf () == &is->ext.lmf)
		return;

#if HOST_BROWSER
	{
		extern int mono_wasm_jit_lmf_publish_diag;
		static int n;
		if (G_UNLIKELY (mono_wasm_jit_lmf_publish_diag) && n++ < 12)
			printf ("WASM_JIT_EH_LMF_RESTORE: head=%p -> island ext=%p n=%d\n",
				(void *) mono_get_lmf (), (void *) &is->ext.lmf, n);
	}
#endif
	mono_set_lmf (&is->ext.lmf);
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
 * Set by mono_wasm_jit_eh_dispatch on a catch match; read by the JITted handler's OP_GET_EX_OBJ.
 * Keep a strong handle, not a raw MonoObject*: dispatch consumes jit_tls->thrown_exc before it returns
 * to wasm, and the landing pad calls C++ catch helpers + set_il_offset before OP_GET_EX_OBJ runs. Those
 * calls are not intended to be GC points, but correctness must not depend on that implementation detail.
 * The handle remains live until the wasm handler has stored the object into its GC-scanned ref slot. */
static __thread MonoGCHandle mono_wasm_jit_caught_exc;

static void
wj_caught_clear (void)
{
	if (mono_wasm_jit_caught_exc)
		mono_gchandle_free_internal (mono_wasm_jit_caught_exc);
	mono_wasm_jit_caught_exc = 0;
}
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
	MonoJitTlsData *jit_tls = mono_get_jit_tls ();
	MonoObject *exc;
	int blk_il, island_il, il, i;
	/* mini_llvmonly_load_exception dereferences thrown_exc while updating trace_ips. A wasm/C++
	 * exception without Mono's managed payload is not ours to dispatch; let the pad rethrow it. */
	if (!jit_tls || !jit_tls->thrown_exc)
		return -1;
	exc = mini_llvmonly_load_exception ();
	if (!exc)
		return -1;
	/* Mono's exception pass 1 already consumes the active island's IL offset. Use that same authoritative
	 * position for the landing-pad clause walk, retaining the bb mapping only as a defensive fallback. */
	blk_il = (blk >= 0 && blk < t->nbbs) ? t->il_offsets [blk] : -1;
	island_il = wj_island_il_offset_for_method (t->method);
	il = island_il >= 0 ? island_il : blk_il;
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
				/* Move ownership of the strong in-flight handle to the caught slot. This avoids both a
				 * raw-object GC hole and a second handle allocation on every caught exception. */
				wj_caught_clear ();
				mono_wasm_jit_caught_exc = jit_tls->thrown_exc;
				jit_tls->thrown_exc = 0;
				if (jit_tls->thrown_non_exc) {
					mono_gchandle_free_internal (jit_tls->thrown_non_exc);
					jit_tls->thrown_non_exc = 0;
				}
				mono_memory_barrier ();
				mono_wasm_jit_eh_caught_flag = 1;        /* DIAG: this invocation took the catch path */
				/* discard any resume-state pass-1 chose for an OUTER handler — we handle nearer. Cover both
				 * the interp resume-state (interp outer handler) and the il_state resume-state (AOTed outer
				 * handler), so a later mono_handle_exception's `!resume_state.ex_gchandle` assert holds. */
				if (ctx->exc_gchandle) { mono_gchandle_free_internal (ctx->exc_gchandle); ctx->exc_gchandle = 0; }
				ctx->has_resume_state = FALSE;
				ctx->handler_frame = NULL;
				if (jit_tls->resume_state.ex_gchandle) { mono_gchandle_free_internal (jit_tls->resume_state.ex_gchandle); jit_tls->resume_state.ex_gchandle = 0; }
				jit_tls->resume_state.il_state = NULL;
				wj_eh_restore_lmf (t->method);
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
			/* Tag the FINALLY/FAULT dispatch so the landing pad sets finally_ind = -1 ONLY for it (see
			 * WJ_EH_DISPATCH_FINALLY_BIT in mini.h): a CATCH dispatch must NOT clobber a normal-leave
			 * continuation an in-flight OP_CALL_HANDLER stored there. */
			wj_eh_restore_lmf (t->method);
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

/* OP_GET_EX_OBJ in a JITted catch handler: return the exception the landing pad's dispatch stashed.
 * The emitter stores it into the GC-scanned ref shadow stack and only then calls the release helper, so
 * the object stays rooted across the complete raw-pointer handoff. */
MonoObject *
mono_wasm_jit_get_caught_exc (void)
{
	return mono_wasm_jit_caught_exc ? mono_gchandle_get_target_internal (mono_wasm_jit_caught_exc) : NULL;
}

void
mono_wasm_jit_release_caught_exc (void)
{
	wj_caught_clear ();
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
			WJ_LMF_BRACKET ("aot-enter");
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
			WJ_LMF_BRACKET ("aot-exit");
			if (!wj_residual) interp_pop_lmf (&ext);
			WJ_LMF_BRACKET ("aot-popped");
			/* Two failed fixes (island splice, pop guard) both left "head became bad at aot-popped" at 18/20,
			 * so stop guessing which of the two remaining stories is true and record the discriminator.
			 * wj_residual==TRUE means the push/pop above were SKIPPED entirely, so a bad head here was left by
			 * the JITTED THUNK (its island push/pop is unbalanced); wj_residual==FALSE means a pop did run and
			 * the predecessor it installed is itself dangling. Also print what ext captured, and whether the
			 * head lands in island-chunk heap or a C-stack frame -- the corrupt previous_lmf seen at the
			 * consumer (0x2481d3d2) was in the island-chunk range and unaligned. */
			{
				extern int mono_wasm_jit_lmf_publish_diag;
				static int n;
				MonoLMF *h = mono_get_lmf ();
				MonoJitTlsData *jt = mono_get_jit_tls ();
				/* MUST use the bracket's exact predicate. The first cut of this check omitted the
				 * alignment, >=64K and != first_lmf exclusions, so it fired on jit_tls->first_lmf --
				 * legitimately all-zero (g_new0 + empty MONO_ARCH_INIT_TOP_LMF_ENTRY) and explicitly
				 * called out in the bracket comment as "exactly the shape being hunted". Those 12 lines
				 * were the chain BOTTOM, not the 18 bad heads the bracket reports, and reading them as
				 * the same events would have pointed the fix at the wrong statement. */
				gboolean h_bad = h && !((gsize) h & 3) && (gsize) h >= 65536 &&
					(!jt || h != jt->first_lmf) &&
					!(((gsize) h->previous_lmf) & 2) && !h->lmf_addr;
				if (G_UNLIKELY (mono_wasm_jit_lmf_publish_diag) && n < 12 && h_bad) {
					n++;
					printf ("WASM_JIT_AOTPOP_STATE: residual=%d head=%p head_prev=%p ext=%p ext_prev=%p aligned=%d\n",
						wj_residual ? 1 : 0, (void *) h, (void *) h->previous_lmf,
						(void *) &ext.lmf, (void *) ext.lmf.previous_lmf,
						(((gsize) h) & 3) == 0);
				}
			}

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

/*
 * Clause index whose IL try range covers il_offset; -1 for none; -2 for "no cached ranges, parse the header".
 *
 * The -2 case matters: silently answering -1 would turn a missing snapshot into "this frame has no handler",
 * which swallows exceptions rather than merely being slow. The caller must fall back.
 */
static int
interp_find_il_clause_for_offset (MonoMethod *method, int il_offset)
{
	InterpMethod *imethod = mono_interp_get_imethod (method);

	/* Transforming here would be a surprising side effect of a lookup, so defer to the caller instead. */
	if (!imethod->transformed)
		return -2;
	if (!imethod->num_clauses)
		return -1;
	if (!imethod->il_try_ranges)
		return -2;

	for (int i = 0; i < imethod->num_clauses; ++i) {
		guint32 off = imethod->il_try_ranges [i * 2];
		guint32 len = imethod->il_try_ranges [i * 2 + 1];

		if (GINT_TO_UINT32 (il_offset) >= off && GINT_TO_UINT32 (il_offset) < off + len)
			return i;
	}
	return -1;
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
	 * of dereferencing — the run continues so we can see how often / with what target type it happens.
	 *
	 * GATED ON OBJGUARD (it shipped ungated). Three raw word reads + two wj_probe_ok range checks on every
	 * mono_interp_isinst is not free on a Java workload, and this is the same class of check OBJGUARD
	 * already owns. Knob-off restores the plain mono_object_class deref, i.e. the upstream behaviour. */
	extern int mono_wasm_jit_objguard;
	if (G_UNLIKELY (mono_wasm_jit_objguard && object != NULL)) {
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
		/* `arg` is the ADDRESS of the first invocation argument, not its value: the open-delegate invoke
		 * wrapper pushes it with mono_mb_emit_ldarg_addr (marshal-lightweight.c), which is why the native
		 * twin of this helper (mono_ldvirtftn_delegate in marshal.c) also derefs. Reading `arg` itself as
		 * the object instead walked one level short — the receiver was decoded as a vtable and its vtable
		 * as a MonoClass, so the resolve below entered mono_class_setup_vtable on a non-class and died in
		 * mono_class_get_flags / mono_class_check_vtable_constraints.
		 *
		 * Unreachable in a pure interpreter run (open delegates dispatch via MINT_CALL_DELEGATE and never
		 * execute this wrapper's IL) and unreachable under AOT (which runs the native twin), so only the
		 * wasm-JIT residual — the one path that drives the generated invoke wrapper THROUGH the interp —
		 * ever reaches MINT_LDVIRTFTN_DELEGATE. */
		MonoObject *object = *(MonoObject**)arg;
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

#if HOST_BROWSER
/*
 * WASM_JIT_TRY_INVOKE: the interp->wasm-JIT dispatch block, shared by every interp call opcode that
 * has resolved a concrete callee (MINT_CALL, MINT_CALLVIRT_FAST, MINT_CALL_DELEGATE, MINT_CALLI).
 * Expects cmethod / return_offset / call_args_offset set and ip already advanced past the
 * instruction (frame->state.ip = ip is the resume ip mono_handle_exception reads on THIS frame —
 * via the LMF pushed here — to match try/catch regions when the JITted callee's residual throws;
 * a stale ip skips a catch in this method).
 *
 * Feeds the hotness/island trigger (wasm_jit_maybe_compile serializes the compile via try-lock and
 * eagerly forms the call-tree island), then, if the callee has a JITted e-thunk, invokes it via
 * mono_wasm_jit_invoke_caught (which catches a C++/wasm-EH unwind escaping the JITted method; pass 1
 * already installed the interp resume-state, so CHECK_RESUME_STATE resumes / re-propagates) and
 * breaks out of the opcode. The LMF push marks the managed->native boundary so GC stack-walking and
 * cooperative/JSPI suspends inside the JITted method are handled correctly.
 *
 * Falls through when the callee has no published slot (stays interp-resident). Jumps to
 * FALLBACK_LABEL when the slot is published but THIS thread failed to instantiate the module
 * (sync_thread can fail on a worker under memory pressure while it succeeded on the compiling
 * thread; the per-thread slot then holds a jiterpreter placeholder of a different signature and
 * call_indirect-ing it traps + kills the worker — mono_wasm_jit_admit gates that).
 *
 * Plain braces, NOT do{}while(0): MINT_IN_BREAK is `break` under switch dispatch and must break
 * the opcode case, not a wrapper loop.
 */
#define WASM_JIT_TRY_INVOKE(fallback_label) \
	{ \
		wasm_jit_maybe_compile (cmethod); \
		gint32 wj_eslot = cmethod->wasm_jit_slot; \
		if (G_UNLIKELY (wj_eslot > 0)) { \
			extern int mono_wasm_jit_admit_live (int desc_id); \
			extern int mono_wasm_jit_slot_live (int slot); \
			extern void mono_wasm_jit_invoke_caught (MonoMethod *, gint32, gpointer, gpointer); \
			extern int mono_wasm_jit_entry_promote; \
			if (G_UNLIKELY (!mono_wasm_jit_admit_live (cmethod->wasm_jit_desc))) \
				goto fallback_label; \
			/* R209: SNAPSHOT THE SLOT, THEN VERIFY THAT SLOT -- not just the descriptor. \
			 * \
			 * This gate used to read wasm_jit_slot twice and wasm_jit_desc once, three separate reads of \
			 * fields RE-EMISSION rewrites concurrently. A thread could pass admit_live for the OLD \
			 * descriptor it had admitted and then call a NEWLY published slot it never instantiated -- \
			 * whose table entry is still mono_jiterp_placeholder_jit_call, (i32,i32,i32,i32)->void, \
			 * called here as (i32,i32)->void. That is a `function signature mismatch` trap in \
			 * wasm_jit_ethunk_cb, which is what wedged every REEMIT arm at volume (R208) and, \
			 * unexplained, the five arms of R174. \
			 * \
			 * The interp_entry path at the e-thunk call already carries this exact check and calls it \
			 * belt-and-braces because the liveness bitmap is never cleared. That reasoning holds only \
			 * while a method's slot never CHANGES; re-emission is exactly the case where it does. \
			 * Costs one load on the interp->JIT boundary (~2.4M/window), not on dispatch (~974M). */ \
			if (G_UNLIKELY (!mono_wasm_jit_slot_live (wj_eslot))) \
				goto fallback_label; \
			{ \
				MonoLMFExt wj_ext; \
				frame->state.ip = ip; \
				interp_push_lmf (&wj_ext, frame); \
				mono_wasm_jit_invoke_caught (cmethod->method, wj_eslot, locals + call_args_offset, locals + return_offset); \
				interp_pop_lmf (&wj_ext); \
			} \
			{ if (G_UNLIKELY (mono_wasm_jit_stats)) { mono_wasm_jit_count (WJC_INVOKE); mono_atomic_inc_i32 (&cmethod->wasm_jit_invoke_in); \
			    /* wj_entry_edges was ALSO gated on BATCH_MODULE==1 for the auto planner, which is gone. \
			     * Stats alone now, which is the only consumer left (mono_wasm_jit_dump_hot_edges). */ \
			    wj_edge_bump (frame->imethod, cmethod); } } \
			/* Lever A: this interp caller keeps entering jitted islands — queue it for upward JIT. */ \
			{ InterpMethod *wjc = frame->imethod; \
			  if (G_UNLIKELY (mono_wasm_jit_entry_promote > 0) && wj_slot_hot_retry_eligible (wjc->wasm_jit_slot) && ++wjc->wasm_jit_invoke_out >= mono_wasm_jit_entry_promote) { wjc->wasm_jit_invoke_out = 0; wj_promote_push (wjc->method); } } \
			/* If a residual interp call inside the JITted method threw, interp_entry set the thread \
			 * resume-state and the JITted method returned early (a dummy result). Propagate through \
			 * the interp's EH — the LMF above lets mono_handle_exception skip the JITted native frame. */ \
			CHECK_RESUME_STATE (context); \
			MINT_IN_BREAK; \
		} \
	}
#endif

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

#ifdef HOST_BROWSER
			/* Delegate-IC sizing data. The wasm JIT lays down MONO_WASM_JIT_VCALL_WAYS unrolled guard
			 * chains at every Delegate.Invoke site, and until now had no way to know that a site only
			 * ever sees one target: the emitted IC is empty on the first compile, and wj_prof_record
			 * runs only from MINT_CALLVIRT_FAST, which never sees delegates. The interpreter runs this
			 * site before the JIT compiles the caller, and has the delegate in hand right here. */
			if (G_UNLIKELY (mono_wasm_jit_devirt_profile) && !is_multicast)
				wj_prof_record_delegate (frame->imethod, del);
#endif

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

#if HOST_BROWSER
			/* Delegate targets never reached the wasm-JIT dispatch: this opcode resolves the target and
			 * jumps straight to jit_call (do_jit_call/interp), so a target with a published e-thunk ran
			 * its interp copy forever (profile4: the hottest DynamicBinder/MemberName wrappers sat at
			 * slot>0 hits=0 doing millions of interp-routed calls). The args area at call_args_offset is
			 * already massaged into the target's own layout above, exactly what the e-thunk expects.
			 * Multicast resolves to the delegate-invoke wrapper — intentionally left interpreted, same as
			 * the jiterp entry's is_invoke skip: it dispatches the real targets via MINT_CALL_DELEGATE. */
			if (!cmethod->is_invoke)
				WASM_JIT_TRY_INVOKE (jit_call);
#endif
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

#if HOST_BROWSER
			/* Same dispatch gap as MINT_CALL_DELEGATE: a calli target with a published e-thunk ran its
			 * interp copy (jit_call has no wasm-JIT check). Args at call_args_offset are in the callee's
			 * layout (this unboxed above when needed). */
			if (!cmethod->is_invoke)
				WASM_JIT_TRY_INVOKE (jit_call);
#endif
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
#if HOST_BROWSER
			/* Speculative-devirt profile: capture the base method BEFORE the resolve overwrites cmethod,
			 * then record base + receiver + the resolved override once we have it. Recording the
			 * override matters — re-deriving it at emit time deadlocks (see WjProfSite.target). */
			MonoMethod *wj_prof_base = G_UNLIKELY (mono_wasm_jit_devirt_profile) ? cmethod->method : NULL;
#endif
			// FIXME push/pop LMF
			cmethod = get_virtual_method_fast (cmethod, this_arg->vtable, slot);
#if HOST_BROWSER
			if (G_UNLIKELY (wj_prof_base != NULL))
				wj_prof_record (frame->imethod, wj_prof_base, WJ_SITE_VIRTUAL, this_arg->vtable,
					cmethod->method, FALSE);
#endif
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
			WASM_JIT_TRY_INVOKE (jit_call);
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
			/* If the callee was compiled by the runtime wasm JIT, invoke its entry thunk
			 * e(args_ptr, ret_ptr) via the function-table slot instead of interpreting; the
			 * thunk marshals args from the call-args stackvals and writes the result back. */
			WASM_JIT_TRY_INVOKE (interp_call);
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
#if HOST_BROWSER
			/* MONO_WASM_JIT_OVER_AOT redirected MINT_JIT_CALL through the ordinary hotness/dispatch
			 * block here, because transform.c replaces a statically-bound call with MINT_JIT_CALL when an
			 * AOT body exists and that opcode bypassed wasm_jit_maybe_compile -- so merely relaxing the
			 * AOT eligibility gate could only ever tier virtual or external-entry AOT methods. Deleted
			 * with the knob (it stalled at boot). If that pool is picked up again, this redirect is half
			 * the mechanism and the gate in wasm_jit_maybe_compile is the other half. */
#endif
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
#if HOST_BROWSER
jit_call_aot:
			error_init_reuse (error);
			/* ip was advanced above; cmethod/offsets retain the original call operands. */
			frame->state.ip = ip;
			do_jit_call (context, (stackval*)(locals + return_offset), (stackval*)(locals + call_args_offset), frame, rmethod, FALSE, error);
			if (!is_ok (error)) {
				MonoException *call_ex = interp_error_convert_to_exception (frame, error, ip);
				THROW_EX (call_ex, ip);
			}
			CHECK_RESUME_STATE (context);
			MINT_IN_BREAK;
#endif
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

#if HOST_BROWSER
	/*
	 * MONO_WASM_JIT_AOT_ENTRY fast path.
	 *
	 * By the time we get here the jiterpreter trampoline has ALREADY marshalled this call's arguments
	 * into the interp stack at `sp` -- which is exactly the layout the wasm-JIT entry thunk reads. When
	 * the target is JITted and admitted, everything the slow path below does around the actual call is
	 * scaffolding for an interpreter run that will not happen: zeroing an InterpFrame, computing
	 * get_arg_offset_fast, pushing and popping an LMF, re-asking wasm_jit_maybe_compile, and re-running
	 * the admission DFS. `perf annotate` shows this function's cost is FLAT across ~74 instructions with
	 * no hotspot, which is what "the whole preamble is the overhead" looks like -- so the only way to
	 * remove it is not to execute it.
	 *
	 * Gated on wasm_jit_entry_fast_ok, which the slow path sets only after admission has succeeded once,
	 * plus a cheap current-generation admission check. Automatic batching can later replace the same
	 * table slot with a new generation, so the historical fast-ok bit alone is intentionally insufficient.
	 *
	 * Deliberately kept: the stack-pointer bump (a JITted body can re-enter the interpreter through a
	 * residual, which would otherwise scribble on these arguments), the GC-mode transition, and the whole
	 * tail below (thread detach, pending unwind, resume state, return marshalling).
	 */
	{
		extern int mono_wasm_jit_aot_entry;
		extern int mono_wasm_jit_desc_admitted (int desc_id);
		InterpMethod *fm = header.rmethod;
		/* wasm_jit_entry_fast_ok is shared historical state, while descriptor admission and the function
		 * table are per-thread and per-generation. An unsynced or freshly-rebatched worker takes the slow
		 * path once, admits the current dependency union, and then becomes fast again. */
		if (G_UNLIKELY (mono_wasm_jit_aot_entry) && fm->wasm_jit_entry_fast_ok && !fm->is_invoke &&
		    fm->wasm_jit_slot > 0 && mono_wasm_jit_desc_admitted (fm->wasm_jit_desc)) {
			extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
			MonoType *ftype;
			int fparams_size = get_arg_offset_fast (fm, NULL, header.params_count);

			header.context->stack_pointer = (guchar*)ALIGN_TO ((guchar*)sp + fparams_size, MINT_STACK_ALIGNMENT);
			g_assert (header.context->stack_pointer < header.context->stack_end);

			MONO_ENTER_GC_UNSAFE;
			mono_wasm_jit_invoke_caught (fm->method, (gint32) fm->wasm_jit_slot, sp, sp);
			MONO_EXIT_GC_UNSAFE;

			header.context->stack_pointer = (guchar*)sp;

			if (fm->needs_thread_attach)
				mono_threads_detach_coop (header.orig_domain, &header.attach_cookie);
			mono_jiterp_check_pending_unwind (header.context);

			if (mono_llvm_only) {
				if (header.context->has_resume_state) {
					mono_llvm_start_native_unwind ();
					return;
				}
			} else if (header.context->has_resume_state) {
				return;
			}

			ftype = fm->rtype;
			if (ftype->type != MONO_TYPE_VOID)
				mono_jiterp_stackval_to_data (ftype, sp, res);
			return;
		}
	}
#endif

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
	 * identical to the mono_interp_exec_method path. */
	{
		InterpMethod *wj_rm = header.rmethod;
		gboolean wj_dispatched = FALSE;
		MonoLMFExt wj_entry_ext;
		/* This entry is the jiterpreter thunk used by native/AOT callers. Like interp_entry above, it
		 * bypasses every interpreter call opcode, so it must feed the full-method wasm-JIT hotness gate
		 * itself. Compile before testing the slot so the threshold-crossing invocation can immediately
		 * redirect to the newly-published e-thunk. Delegate Invoke wrappers dispatch their real target
		 * through MINT_CALL and are intentionally left to that trigger. */
		if (!wj_rm->is_invoke) {
			/* This is an external managed-to-interp entry, not an interp MINT_CALL and not the explicit
			 * wasm-JIT residual helper. Keep the prepared interp frame on the LMF chain while compilation
			 * safepoints and while a compiled non-EH body runs. */
			interp_push_lmf (&wj_entry_ext, &frame);
			wasm_jit_maybe_compile (wj_rm);
		}
		if (G_UNLIKELY (wj_rm->wasm_jit_slot > 0) && !wj_rm->is_invoke) {
			/* _live, not plain admit. This site is the worst place to accept admit()'s cycle-break "yes":
			 * it does not merely enter the e-slot once, it sets wasm_jit_entry_fast_ok and repoints the
			 * MonoFtnDesc every AOT caller holds at a guard-free adapter for that slot -- permanently. */
			extern int mono_wasm_jit_admit_live (int desc_id);
			if (G_LIKELY (mono_wasm_jit_admit_live (wj_rm->wasm_jit_desc))) {
				extern void mono_wasm_jit_invoke_caught (MonoMethod *method, gint32 slot, gpointer args, gpointer ret);
				/* MONO_WASM_JIT_AOT_ENTRY: everything this function did to get here — the header copy,
				 * InterpFrame setup, get_arg_offset_fast, the GC-unsafe transition, the LMF push above,
				 * and the stackval marshalling the jiterp trampoline did on the way in — is overhead
				 * wrapped around a body we have already compiled. Repoint the MonoFtnDesc that AOT
				 * callers hold at a direct adapter so subsequent calls never reach this path at all.
				 *
				 * Done HERE, lazily, rather than at publish time or in
				 * interp_create_method_pointer_llvmonly: that function usually runs long before the
				 * method is hot enough to be JITted, and it caches its ftndesc on jit_entry forever, so
				 * building the adapter there would miss precisely the hot methods worth redirecting.
				 * Mutating desc->addr redirects every holder at once (delegates keep the desc pointer,
				 * not a copy); it is a single aligned word, so a concurrent reader sees the old or the
				 * new target and both are valid. The desc's `arg` stays as it was and the adapter drops
				 * it, which is exactly the trailing ftndesc argument the AOT ABI passes. */
				/* DIAGNOSTIC (verbose>=2): sample which methods actually cross here and whether they even
				 * have a ftndesc to repoint. Sampled rather than once-per-method so the output is weighted
				 * by call FREQUENCY -- the first N methods to cross are startup noise, and what matters is
				 * which ones cross millions of times. */
				{
					extern int mono_wasm_jit_verbose;
					static gint32 wj_probe_n = 0;
					if (G_UNLIKELY (mono_wasm_jit_verbose >= 2)) {
					gint32 pn = mono_atomic_inc_i32 (&wj_probe_n);
						if ((pn % 5000) == 0 && pn <= 200000)
							printf ("WASM_JIT_AOT_PROBE #%d %s jit_entry=%d unbox_entry=%d ftndesc=%d fslot=%d\n",
								(int) pn, wj_rm->method ? wj_rm->method->name : "?",
								wj_rm->jit_entry ? 1 : 0, wj_rm->llvmonly_unbox_entry ? 1 : 0,
								wj_rm->ftndesc ? 1 : 0, (int) wj_rm->wasm_jit_fslot);
					}
				}
				/* Admission succeeded for the current generation, so later entries may take the fast
				 * path above. Its generation check sends them back here after a future rebatch. */
				wj_rm->wasm_jit_entry_fast_ok = 1;
				mono_wasm_jit_invoke_caught (wj_rm->method, (gint32) wj_rm->wasm_jit_slot, frame.stack, frame.stack);
				wj_dispatched = TRUE;
			}
		}
		if (!wj_rm->is_invoke)
			interp_pop_lmf (&wj_entry_ext);
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
