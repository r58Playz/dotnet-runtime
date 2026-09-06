#ifndef __MONO_MINI_INTERPRETER_INTERNALS_H__
#define __MONO_MINI_INTERPRETER_INTERNALS_H__

#include <setjmp.h>
#include <glib.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/object.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-internals.h>
#include "interp.h"

#define MINT_TYPE_I1 0
#define MINT_TYPE_U1 1
#define MINT_TYPE_I2 2
#define MINT_TYPE_U2 3
#define MINT_TYPE_I4 4
#define MINT_TYPE_I8 5
#define MINT_TYPE_R4 6
#define MINT_TYPE_R8 7
#define MINT_TYPE_O  8
#define MINT_TYPE_VT 9
#define MINT_TYPE_VOID 10

#define INLINED_METHOD_FLAG 0xffff
#define TRACING_FLAG 0x1
#define PROFILING_FLAG 0x2

#define MINT_STACK_SLOT_SIZE (sizeof (stackval))
// This alignment provides us with straight forward support for Vector128
#define MINT_STACK_ALIGNMENT (2 * MINT_STACK_SLOT_SIZE)
#define MINT_SIMD_ALIGNMENT (MINT_STACK_ALIGNMENT)
#define SIZEOF_V128 16

#define INTERP_STACK_SIZE (1024*1024)
#define INTERP_REDZONE_SIZE (8*1024)

enum {
	VAL_I32     = 0,
	VAL_DOUBLE  = 1,
	VAL_I64     = 2,
	VAL_VALUET  = 3,
	VAL_POINTER = 4,
	VAL_NATI    = 0 + VAL_POINTER,
	VAL_MP      = 1 + VAL_POINTER,
	VAL_TP      = 2 + VAL_POINTER,
	VAL_OBJ     = 3 + VAL_POINTER
};

#if SIZEOF_VOID_P == 4
typedef guint32 mono_u;
typedef gint32  mono_i;
#define MINT_TYPE_I MINT_TYPE_I4
#elif SIZEOF_VOID_P == 8
typedef guint64 mono_u;
typedef gint64  mono_i;
#define MINT_TYPE_I MINT_TYPE_I8
#endif

#ifdef TARGET_WASM
#define INTERP_NO_STACK_SCAN 1
#endif

/*
 * Value types are represented on the eval stack as pointers to the
 * actual storage. A value type cannot be larger than 16 MB.
 */
typedef struct {
	union {
		gint32 i;
		gint64 l;
		struct {
			gint32 lo;
			gint32 hi;
		} pair;
		float f_r4;
		double f;
#ifdef INTERP_NO_STACK_SCAN
		/* Ensure objref is always flushed to interp stack */
		MonoObject * volatile o;
#else
		MonoObject *o;
#endif
		/* native size integer and pointer types */
		gpointer p;
		mono_u nati;
		gpointer vt;
	} data;
} stackval;

typedef struct InterpFrame InterpFrame;

typedef void (*MonoFuncV) (void);
typedef void (*MonoPIFunc) (void *callme, void *margs);


typedef enum {
	IMETHOD_CODE_INTERP,
	IMETHOD_CODE_COMPILED,
	IMETHOD_CODE_UNKNOWN
} InterpMethodCodeType;

#define PROFILE_INTERP 0

#if __GNUC__
#define INTERP_ENABLE_SIMD
#endif

#define INTERP_IMETHOD_TAG_1(im) ((gpointer)((mono_u)(im) | 1))
#define INTERP_IMETHOD_IS_TAGGED_1(im) ((mono_u)(im) & 1)
#define INTERP_IMETHOD_UNTAG_1(im) ((InterpMethod*)((mono_u)(im) & ~1))

#define INTERP_IMETHOD_TAG_UNBOX(im) INTERP_IMETHOD_TAG_1(im)
#define INTERP_IMETHOD_IS_TAGGED_UNBOX(im) INTERP_IMETHOD_IS_TAGGED_1(im)
#define INTERP_IMETHOD_UNTAG_UNBOX(im) INTERP_IMETHOD_UNTAG_1(im)

/*
 * Structure representing a method transformed for the interpreter
 */
typedef struct InterpMethod InterpMethod;
struct InterpMethod {
	/* NOTE: These first two elements (method and
	   next_jit_code_hash) must be in the same order and at the
	   same offset as in MonoJitInfo, because of the jit_code_hash
	   internal hash table in MonoDomain. */
	MonoMethod *method;
	InterpMethod *next_jit_code_hash;

	// Sort pointers ahead of integers to minimize padding for alignment.

	unsigned short *code;
	MonoPIFunc func;
	MonoExceptionClause *clauses; // num_clauses
	/* 2 * num_clauses entries: (try_offset, try_len) in IL offsets, or NULL when num_clauses == 0.
	 *
	 * clauses[] above CANNOT serve this purpose: transform.c rewrites its offsets in place to interp bytecode
	 * offsets via get_native_offset, so after transformation the IL ranges are gone. Exception handling needs
	 * the IL ranges (it matches against MonoMethodILState.il_offset), and its only other source is
	 * mono_method_get_header_checked -- which mallocs. Captured here before the rewrite, from the same mempool
	 * as the rest of InterpMethod, so it lives and dies with the method and adds no lifetime hazard. */
	guint32 *il_try_ranges;
	void **data_items;
	guint32 *local_offsets;
	guint32 *arg_offsets;
	guint32 *clause_data_offsets;
	gpointer jit_call_info;
	gpointer jit_entry;
	gpointer llvmonly_unbox_entry;
	MonoType *rtype;
	MonoType **param_types;
	MonoJitInfo *jinfo;
	MonoFtnDesc *ftndesc;
	MonoFtnDesc *ftndesc_unbox;
	MonoDelegateTrampInfo *del_info;

	/* locals_size is equal to the offset of the param_area */
	guint32 locals_size;
	guint32 alloca_size;
	int n_data_items;
	int num_clauses; // clauses
	int transformed; // boolean
	gint32 wasm_jit_slot; // runtime wasm JIT: function-table slot of the entry thunk e (0 = untried, -2 = waiter-parked, -3 = transient retry, -1 = permanent bail, >0 = JITted)
	gint32 wasm_jit_desc; // immutable centralized descriptor id; 0 until release-published with wasm_jit_slot
	gint32 wasm_jit_fslot; // runtime wasm JIT: slot of the scalar method fn f, for call_indirect from JITted callers (0 = none)
	gint32 wasm_jit_hits;  // runtime wasm JIT: call-count toward the auto-JIT hotness threshold (MONO_WASM_JIT_AUTO)
	gint32 wasm_jit_bytes_len; // runtime wasm JIT: length of the cached module bytes (for per-thread instantiation)
	gpointer wasm_jit_bytes;   // runtime wasm JIT: cached emitted module bytes; each thread instantiates its own WebAssembly.Instance from these into its own function table on first invoke (the table is per-thread for dynamically-added entries)
	gint16 wasm_jit_bail;  // runtime wasm JIT: why this method permanently failed to JIT (set when slot==-1), for the weighted vcall-residual breakdown. 0=n/a; -2=EH clauses; -3=sig(arg/ret) type; -4=other; -8=gshared method; -11=island perm-leaf poison; -12=rgctx callsite; >0=the unsupported mini opcode number
	guint8 wasm_jit_blocked_noted; // runtime wasm JIT stats: this method was already counted (once) into the callee-not-jitted bail-histogram bucket — the island driver re-emits blocked methods every iteration, so publish-time distinct-method counting keeps the histogram terminal, not per-attempt
	guint8 wasm_jit_entry_fast_ok; // runtime wasm JIT (MONO_WASM_JIT_AOT_ENTRY): this method has been admitted at least once, so later entries may try the fast path that skips the InterpFrame/LMF/maybe_compile/admit scaffolding. The fast path still verifies per-thread admission of the descriptor's current generation because automatic rebatching reuses its e/f slots.
	const char *wasm_jit_fail; // runtime wasm JIT: the emitter's exact fail string (static literal) behind wasm_jit_bail, for the weighted vperm top-N dump (names the specific gate, e.g. "ldaddr of vtype with refs" vs just "ldaddr")
	gint32 wasm_jit_invoke_in; // runtime wasm JIT diag (Part 3c): times entered interp->JIT (this method was the JITted callee at a MINT_CALL/CALLVIRT). stats only
	/* R208's per-method IC-MISS count lived here (wasm_jit_ic_misses): the re-emission trigger that
	 * selected methods hot INSIDE compiled code rather than at the interp->JIT boundary, which is what
	 * made it the right selector and R179's `invoke_in` the wrong one. Gone with re-emission. */
	gint32 wasm_jit_block_n;   // runtime wasm JIT (Part 3a / Lever C): times this (un-JITted) method BLOCKED a caller's island. Always counted (cheap, compile-time): also a stats-independent "hot at the island boundary" signal for the cold gate.
	gint32 wasm_jit_invoke_out; // runtime wasm JIT (Lever A): times THIS (interp) method invoked a JITted callee. Drives MONO_WASM_JIT_ENTRY_PROMOTE upward island growth.
	gint32 wasm_jit_resv_eslot; // runtime wasm JIT (multi-method cycle batch): reserved-but-unpublished entry-thunk slot while this method's SCC is being batch-compiled (0 = none)
	gint32 wasm_jit_resv_fslot; // runtime wasm JIT (multi-method cycle batch): reserved-but-unpublished fn slot; get_callee_fslot returns it so cycle members bake each other's f-slot before any member is published/invocable. Because OTHER methods can bake it, it must be cleared the moment the batch ends (wj_park_reservation) — otherwise a caller could bake a slot nothing instantiates into.
	/* runtime wasm JIT (self-recursion): this method's OWN e/f pair, reserved at the first emit that needs to
	 * bake a self-call and then held ACROSS re-emits. Deliberately separate from wasm_jit_resv_* above, which
	 * is a live-batch reservation that get_callee_fslot publishes to other methods: the batch always clears it
	 * on both the success and abort paths, so a slot other methods can bake is guaranteed to be instantiated.
	 * A self reservation has to outlive a FAILED emit (that is the whole point — see below), so it must stay
	 * invisible to get_callee_fslot or an unrelated caller could bake an f-slot nothing ever installs into.
	 * Only mono_wasm_jit_self_reserved reads these, and only for the method being emitted.
	 *
	 * Why hold it across re-emits: mono_jiterp_allocate_table_entry is a bump allocator with NO free, and
	 * these used to be locals of mono_wasm_emit_method — so every failed emit orphaned two table entries and
	 * the next attempt allocated a fresh pair. The drivers re-emit hard (wasm_jit_force_island up to 10 passes,
	 * wasm_jit_compile_scc phase 1 up to WJ_SCC_MAX*2), so the leak scaled with re-emission count rather than
	 * method count — which is exactly the axis module batching increases. */
	gint32 wasm_jit_self_resv_eslot;
	gint32 wasm_jit_self_resv_fslot;
	/* Three re-emission state bytes lived here -- reemit_state (0 never considered / 1 queued / 3 in
	 * flight / 2 done-never-again), heal_pending (a HEAL_WAIT wake, which had to BYPASS the age gate
	 * because it is an EVENT rather than a staleness heuristic) and reemit_seen (the distinct-method
	 * denominator every per-attempt reemit counter lacked). All gone with re-emission; the two guint8s
	 * they shared padding with are unaffected. */
	/* SIGNATURE-SHAPE CACHE for the residual/delegate interp boundary. Both per-crossing loops in
	 * mono_wasm_jit_call_interp are pure functions of MonoMethodSignature*, which is fixed for the
	 * callee, and they were being re-derived on EVERY crossing -- ~8,500 times per frame.
	 *
	 * Measured cost of re-deriving them, client render thread, in-game plateau, two independent captures
	 * (b5ctl / b2ctl), each symbol with its immediate caller resolved from the perf call chains:
	 *
	 *     mini_is_gsharedvt_type            0.813% / 0.700%   (99.6% under mini_is_gsharedvt_variable_type)
	 *     wj_byaddr_vtype                   0.772% / 0.858%   (99.4% under mono_wasm_jit_arg_is_byaddr)
	 *     mono_mint_type                    0.721% / 0.691%   (97.2% under mono_wasm_jit_call_interp)
	 *     mini_type_get_underlying_type     0.538% / 0.570%   (98.9% under wj_byaddr_vtype)
	 *     mono_wasm_jit_arg_is_byaddr       0.272% / 0.318%   (92.1% under mono_wasm_jit_call_interp)
	 *     mini_is_gsharedvt_variable_type   0.059%
	 *                                       ------------------
	 *                                       3.175% / 3.137%   of the fps-setting thread
	 *
	 * `ptrarg_mask` bit i = wj_arg_slot_holds_pointer (sig->params[i]); `argshape` carries COMPUTED,
	 * SCALAR (the e-slot residual's eligibility test) and WIDE (>32 params -- fall back to the loop, so
	 * nothing regresses on a wide signature). Both are DERIVED FROM the predicate rather than being a
	 * second copy of it: the header on wj_arg_slot_holds_pointer is explicit that a desync there is a
	 * silent DOUBLE DEREF, not a crash.
	 *
	 * Keyed on the InterpMethod, which is resolved AFTER call_interp's SYNCHRONIZED_INNER substitution,
	 * so the cache and the `sig` it describes are the same method by construction -- attaching it to the
	 * incoming MonoMethod would have described the wrapper's signature instead.
	 *
	 * Safe against the flags it depends on: VTYPE_BYADDR and VRET are documented "read ONCE
	 * (process-lifetime): f_sig_id fingerprints must not split mid-process", so the classification cannot
	 * change under the cache. Concurrent computation is benign (pure function, every thread computes the
	 * same answer); the store order below publishes the mask before the COMPUTED bit.
	 *
	 * The emitter's own uses of the predicate (mini-wasm.c) are untouched -- there it runs once per
	 * method at emit time and costs nothing. Only the per-crossing C-side consumers read this. */
	guint32 wasm_jit_ptrarg_mask;
	guint8 wasm_jit_argshape;
	/* THE CALL PROFILE (MONO_WASM_JIT_DEVIRT_PROFILE): WjCallProfile*, lazily allocated. See the long
	 * comment above WjProfSite in interp.c.
	 *
	 * Records what this method's virtual and delegate call sites dispatch to. Written by the interpreter
	 * WHILE THIS METHOD RUNS INTERPRETED, so the wasm JIT has a target to speculate on at its FIRST
	 * compile — its own vcall IC only fills in after the method is already JITted, which is too late to
	 * shape its code — AND, once it is JITted, by that IC's miss path, so a re-emission can see what the
	 * compiled code learned. It also carries the STABLE per-site inline-cache id, which is what lets a
	 * re-emitted site keep the worker-local PIC entries the previous generation warmed.
	 *
	 * Keyed by CALLEE BASE METHOD, not by call site. Interp code offsets are not usable as a key:
	 * generate_compacted_code lays out a fresh buffer after interp_optimize_code, so any offset taken
	 * at emit time is stale, and threading a per-site index through would mean a 6th operand on
	 * MINT_CALLVIRT_FAST (and matching jiterpreter opcode-table changes). The base method is already in
	 * hand at the dispatch point and needs no encoding change. Two sites in one method calling the same
	 * base method therefore share an entry; that only costs precision, never correctness, because the
	 * emitted code guards on the vtable and falls back on a miss. */
	gpointer wasm_jit_profile;
	unsigned int param_count;
	unsigned int hasthis; // boolean
	MonoProfilerCallInstrumentationFlags prof_flags;
	InterpMethodCodeType code_type;
	int ref_slot_offset; // GC visible pointer slot
	int swift_error_offset; // swift error struct
	MonoBitSet *ref_slots;
#ifdef ENABLE_EXPERIMENT_TIERED
	MiniTieredCounter tiered_counter;
#endif
	gint32 entry_count;
	InterpMethod *optimized_imethod;
	// This data is used to resolve native offsets from unoptimized method to native offsets
	// in the optimized method. We rely on keys identifying a certain logical execution point
	// to be equal between unoptimized and optimized method. In unoptimized method we map from
	// native_offset to a key and in optimized_method we map from key to a native offset.
	//
	// The logical execution points that are being tracked are some basic block starts (in this
	// case we don't need any tracking in the unoptimized method, just the mapping from bbindex
	// to its native offset) and call handler returns. Call handler returns store the return ip
	// on the stack so once we tier up the method we need to update these to IPs in the optimized
	// method. The key for a call handler is its index, in appearance order in the IL, multiplied
	// by -1. (So we don't collide with basic block indexes)
	//
	// Since we have both positive and negative keys in this array, we use G_MAXINTRE as terminator.
	int *patchpoint_data;
	unsigned int init_locals : 1;
	unsigned int vararg : 1;
	unsigned int optimized : 1;
	unsigned int needs_thread_attach : 1;
	// If set, this method is MulticastDelegate.Invoke
	unsigned int is_invoke : 1;
	unsigned int is_verbose : 1;
#if HOST_BROWSER
	unsigned int contains_traces : 1;
	MonoBitSet *address_taken_bits;
#endif
#if PROFILE_INTERP
	long calls;
	long opcounts;
#endif
};

/* Used for localloc memory allocation */
typedef struct _FrameDataFragment FrameDataFragment;
struct _FrameDataFragment {
	guint8 *pos, *end;
	struct _FrameDataFragment *next;
#if SIZEOF_VOID_P == 4
	/* Align data field to MINT_VT_ALIGNMENT */
	gint32 pad;
#endif
	double data [MONO_ZERO_LEN_ARRAY];
};

typedef struct {
	InterpFrame *frame;
	/*
	 * frag and pos hold the current allocation position when the stored frame
	 * starts allocating memory. This is used for restoring the localloc stack
	 * when frame returns.
	 */
	FrameDataFragment *frag;
	guint8 *pos;
} FrameDataInfo;

typedef struct {
	FrameDataFragment *first, *current;
	FrameDataInfo *infos;
	int infos_len, infos_capacity;
	/* For GC sync */
	int inited;
} FrameDataAllocator;


/* Arguments that are passed when invoking only a finally/filter clause from the frame */
typedef struct FrameClauseArgs FrameClauseArgs;

/* State of the interpreter main loop */
typedef struct {
	const unsigned short  *ip;
} InterpState;

struct InterpFrame {
	InterpFrame *parent; /* parent */
	InterpMethod  *imethod; /* parent */
	stackval       *retval; /* parent */
	stackval       *stack;
	InterpFrame    *next_free;
	/* State saved before calls */
	/* This is valid if state.ip != NULL */
	InterpState state;
};

#define frame_locals(frame) ((guchar*)(frame)->stack)

typedef struct {
	/* Lets interpreter know it has to resume execution after EH */
	gboolean has_resume_state;
	/* Frame to resume execution at */
	/* Can be NULL if the exception is caught in an AOTed frame */
	InterpFrame *handler_frame;
	/* IP to resume execution at */
	const guint16 *handler_ip;
	/* Clause that we are resuming to */
	MonoJitExceptionInfo *handler_ei;
	/* Exception that is being thrown. Set with rest of resume state */
	MonoGCHandle exc_gchandle;
	/* This is a contiguous space allocated for interp execution stack */
	guchar *stack_start;
	/* End of the stack space excluding the redzone used to handle stack overflows */
	guchar *stack_end;
	guchar *stack_real_end;
	/*
	 * This stack pointer is the highest stack memory that can be used by the current frame. This does not
	 * change throughout the execution of a frame and it is essentially the upper limit of the execution
	 * stack pointer. It is needed when re-entering interp, to know from which address we can start using
	 * stack, and also needed for the GC to be able to scan the stack.
	 */
	guchar *stack_pointer;
	/* Used for allocation of localloc regions */
	FrameDataAllocator data_stack;
	/* If bit n is set, it means that the n-th stack slot (pointer sized) from stack_start doesn't contain any refs */
	guint8 *no_ref_slots;
} ThreadContext;

typedef struct {
	gint64 transform_time;
	gint64 methods_transformed;
	gint64 optimize_time;
	gint64 ssa_compute_time;
	gint64 ssa_compute_dominance_time;
	gint64 ssa_compute_global_vars_time;
	gint64 ssa_compute_pruned_liveness_time;
	gint64 ssa_rename_vars_time;
	gint64 optimize_bblocks_time;
	gint64 cprop_time;
	gint64 super_instructions_time;
	gint32 emitted_instructions;
	gint32 inlined_methods;
	gint32 inline_failures;
} MonoInterpStats;

extern MonoInterpStats mono_interp_stats;

extern int mono_interp_traceopt;
extern int mono_interp_opt;
extern GSList *mono_interp_jit_classes;

void
mono_interp_transform_method (InterpMethod *imethod, ThreadContext *context, MonoError *error);

void
mono_interp_transform_init (void);

InterpMethod *
mono_interp_get_imethod (MonoMethod *method);

/* out_why (optional) receives a WJ_PRED_* code saying why a refusal happened, for the emitter's
 * WJC_DEVIRT_* census; see the #define block above the definition in interp.c. */
gboolean
mono_wasm_jit_prof_predict (gpointer caller, MonoMethod *base, MonoVTable **out_vt,
	MonoMethod **out_target, guint32 *out_samples, int *out_why);

void
mono_interp_print_code (InterpMethod *imethod);

gboolean
mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);

void
mono_interp_error_cleanup (MonoError *error);

int
mono_mint_type (MonoType *type);

int
mono_interp_type_size (MonoType *type, int mt, int *align_p);

#if HOST_BROWSER

gboolean
mono_jiterp_isinst (MonoObject* object, MonoClass* klass);

void
mono_jiterp_check_pending_unwind (ThreadContext *context);

void *
mono_jiterp_get_context (void);

int
mono_jiterp_overflow_check_i4 (gint32 lhs, gint32 rhs, int opcode);

int
mono_jiterp_overflow_check_u4 (guint32 lhs, guint32 rhs, int opcode);

void
mono_jiterp_ld_delegate_method_ptr (gpointer *destination, MonoDelegate **source);

void
mono_jiterp_stackval_to_data (MonoType *type, stackval *val, void *data);

void
mono_jiterp_stackval_from_data (MonoType *type, stackval *result, const void *data);

int
mono_jiterp_get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index);

gpointer
mono_jiterp_frame_data_allocator_alloc (FrameDataAllocator *stack, InterpFrame *frame, int size);

gpointer
mono_jiterp_get_simd_intrinsic (int arity, int index);

int
mono_jiterp_get_simd_opcode (int arity, int index);

int
mono_jiterp_get_opcode_info (int opcode, int type);

#endif

#endif /* __MONO_MINI_INTERPRETER_INTERNALS_H__ */
