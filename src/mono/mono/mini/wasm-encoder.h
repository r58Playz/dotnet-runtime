/*
 * wasm-encoder.h: minimal WebAssembly byte encoder for the runtime wasm JIT backend.
 *
 * The native JIT pipeline is C and Mono IR lives in C, so mono_wasm_emit_method
 * (mini-wasm.c) emits wasm bytes directly via this encoder instead of the
 * TypeScript jiterpreter WasmBuilder. This is the C analog of the
 * BlobBuilder/WasmBuilder in src/mono/browser/runtime/jiterpreter-support.ts.
 *
 * Scope: emit one self-contained module exporting a single function. Imports
 * (memory, the indirect function table, the EH tag) and call_indirect are added
 * in later phases; the first leaf-method cut needs none of them.
 */

#ifndef __MONO_MINI_WASM_ENCODER_H__
#define __MONO_MINI_WASM_ENCODER_H__

#include <glib.h>

/* wasm value types (numtype/reftype encodings) */
typedef enum {
	WASM_VOID    = 0x40,
	WASM_I32     = 0x7f,
	WASM_I64     = 0x7e,
	WASM_F32     = 0x7d,
	WASM_F64     = 0x7c,
	WASM_V128    = 0x7b,
	WASM_FUNCREF = 0x70,
} WasmValtype;

/* wasm opcodes (only those the backend emits; extended per phase) */
typedef enum {
	WASM_OP_UNREACHABLE   = 0x00,
	WASM_OP_NOP           = 0x01,
	WASM_OP_BLOCK         = 0x02,
	WASM_OP_LOOP          = 0x03,
	WASM_OP_IF            = 0x04,
	WASM_OP_ELSE          = 0x05,
	/* legacy exception-handling proposal (what mono + the jiterpreter use) */
	WASM_OP_TRY           = 0x06,
	WASM_OP_CATCH         = 0x07,
	WASM_OP_THROW         = 0x08,
	WASM_OP_RETHROW       = 0x09,
	WASM_OP_CATCH_ALL     = 0x19,
	WASM_OP_END           = 0x0b,
	WASM_OP_BR            = 0x0c,
	WASM_OP_BR_IF         = 0x0d,
	WASM_OP_BR_TABLE      = 0x0e,
	WASM_OP_RETURN        = 0x0f,
	WASM_OP_CALL          = 0x10,
	WASM_OP_CALL_INDIRECT = 0x11,
	WASM_OP_DROP          = 0x1a,
	WASM_OP_SELECT        = 0x1b,

	WASM_OP_REF_IS_NULL   = 0xd1,   /* funcref -> i32 (1 if null) — reference-types */
	WASM_OP_TABLE_GET     = 0x25,   /* [i32 idx] -> funcref; followed by a tableidx uleb — reference-types */
	WASM_OP_ATOMIC_PREFIX = 0xfe,   /* threads/atomics prefix; i64.atomic.load = 0xfe 0x11 <memarg align=3> (0x01 is wait32!) */

	WASM_OP_LOCAL_GET     = 0x20,
	WASM_OP_LOCAL_SET     = 0x21,
	WASM_OP_LOCAL_TEE     = 0x22,
	WASM_OP_GLOBAL_GET    = 0x23,
	WASM_OP_GLOBAL_SET    = 0x24,

	WASM_OP_I32_LOAD      = 0x28,
	WASM_OP_I64_LOAD      = 0x29,
	WASM_OP_F32_LOAD      = 0x2a,
	WASM_OP_F64_LOAD      = 0x2b,
	WASM_OP_I32_LOAD8_S   = 0x2c,
	WASM_OP_I32_LOAD8_U   = 0x2d,
	WASM_OP_I32_LOAD16_S  = 0x2e,
	WASM_OP_I32_LOAD16_U  = 0x2f,
	WASM_OP_I64_LOAD8_S   = 0x30,
	WASM_OP_I64_LOAD8_U   = 0x31,
	WASM_OP_I64_LOAD16_S  = 0x32,
	WASM_OP_I64_LOAD16_U  = 0x33,
	WASM_OP_I64_LOAD32_S  = 0x34,
	WASM_OP_I64_LOAD32_U  = 0x35,
	WASM_OP_I32_STORE     = 0x36,
	WASM_OP_I64_STORE     = 0x37,
	WASM_OP_F32_STORE     = 0x38,
	WASM_OP_F64_STORE     = 0x39,
	WASM_OP_I32_STORE8    = 0x3a,
	WASM_OP_I32_STORE16   = 0x3b,
	WASM_OP_I64_STORE8    = 0x3c,
	WASM_OP_I64_STORE16   = 0x3d,
	WASM_OP_I64_STORE32   = 0x3e,

	WASM_OP_I32_CONST     = 0x41,
	WASM_OP_I64_CONST     = 0x42,
	WASM_OP_F32_CONST     = 0x43,
	WASM_OP_F64_CONST     = 0x44,

	WASM_OP_I32_EQZ       = 0x45,
	WASM_OP_I32_EQ        = 0x46,
	WASM_OP_I32_NE        = 0x47,
	WASM_OP_I32_LT_S      = 0x48,
	WASM_OP_I32_LT_U      = 0x49,
	WASM_OP_I32_GT_S      = 0x4a,
	WASM_OP_I32_GT_U      = 0x4b,
	WASM_OP_I32_LE_S      = 0x4c,
	WASM_OP_I32_LE_U      = 0x4d,
	WASM_OP_I32_GE_S      = 0x4e,
	WASM_OP_I32_GE_U      = 0x4f,
	WASM_OP_I64_EQZ       = 0x50,
	WASM_OP_I64_EQ        = 0x51,
	WASM_OP_I64_NE        = 0x52,
	WASM_OP_I64_LT_S      = 0x53,
	WASM_OP_I64_LT_U      = 0x54,
	WASM_OP_I64_GT_S      = 0x55,
	WASM_OP_I64_GT_U      = 0x56,
	WASM_OP_I64_LE_S      = 0x57,
	WASM_OP_I64_LE_U      = 0x58,
	WASM_OP_I64_GE_S      = 0x59,
	WASM_OP_I64_GE_U      = 0x5a,
	WASM_OP_F32_EQ        = 0x5b,
	WASM_OP_F32_NE        = 0x5c,
	WASM_OP_F32_LT        = 0x5d,
	WASM_OP_F32_GT        = 0x5e,
	WASM_OP_F32_LE        = 0x5f,
	WASM_OP_F32_GE        = 0x60,
	WASM_OP_F64_EQ        = 0x61,
	WASM_OP_F64_NE        = 0x62,
	WASM_OP_F64_LT        = 0x63,
	WASM_OP_F64_GT        = 0x64,
	WASM_OP_F64_LE        = 0x65,
	WASM_OP_F64_GE        = 0x66,

	WASM_OP_I32_CLZ       = 0x67,
	WASM_OP_I32_CTZ       = 0x68,
	WASM_OP_I32_POPCNT    = 0x69,
	WASM_OP_I32_ADD       = 0x6a,
	WASM_OP_I32_SUB       = 0x6b,
	WASM_OP_I32_MUL       = 0x6c,
	WASM_OP_I32_DIV_S     = 0x6d,
	WASM_OP_I32_DIV_U     = 0x6e,
	WASM_OP_I32_REM_S     = 0x6f,
	WASM_OP_I32_REM_U     = 0x70,
	WASM_OP_I32_AND       = 0x71,
	WASM_OP_I32_OR        = 0x72,
	WASM_OP_I32_XOR       = 0x73,
	WASM_OP_I32_SHL       = 0x74,
	WASM_OP_I32_SHR_S     = 0x75,
	WASM_OP_I32_SHR_U     = 0x76,
	WASM_OP_I32_ROTL      = 0x77,
	WASM_OP_I32_ROTR      = 0x78,

	WASM_OP_I64_ADD       = 0x7c,
	WASM_OP_I64_SUB       = 0x7d,
	WASM_OP_I64_MUL       = 0x7e,
	WASM_OP_I64_DIV_S     = 0x7f,
	WASM_OP_I64_DIV_U     = 0x80,
	WASM_OP_I64_REM_S     = 0x81,
	WASM_OP_I64_REM_U     = 0x82,
	WASM_OP_I64_AND       = 0x83,
	WASM_OP_I64_OR        = 0x84,
	WASM_OP_I64_XOR       = 0x85,
	WASM_OP_I64_SHL       = 0x86,
	WASM_OP_I64_SHR_S     = 0x87,
	WASM_OP_I64_SHR_U     = 0x88,
	WASM_OP_I64_ROTL      = 0x89,
	WASM_OP_I64_ROTR      = 0x8a,

	WASM_OP_F32_NEG       = 0x8c,
	WASM_OP_F32_ADD       = 0x92,
	WASM_OP_F32_SUB       = 0x93,
	WASM_OP_F32_MUL       = 0x94,
	WASM_OP_F32_DIV       = 0x95,
	WASM_OP_F64_ADD       = 0xa0,
	WASM_OP_F64_SUB       = 0xa1,
	WASM_OP_F64_MUL       = 0xa2,
	WASM_OP_F64_DIV       = 0xa3,
	WASM_OP_F64_NEG       = 0x9a,

	WASM_OP_I32_WRAP_I64       = 0xa7,
	WASM_OP_I64_EXTEND_I32_S   = 0xac,
	WASM_OP_I64_EXTEND_I32_U   = 0xad,
	WASM_OP_F32_CONVERT_I32_S  = 0xb2,
	WASM_OP_F32_CONVERT_I32_U  = 0xb3,
	WASM_OP_F32_CONVERT_I64_S  = 0xb4,
	WASM_OP_F64_CONVERT_I32_S  = 0xb7,
	WASM_OP_F64_CONVERT_I32_U  = 0xb8,
	WASM_OP_F64_CONVERT_I64_S  = 0xb9,
	WASM_OP_I32_REINTERPRET_F32 = 0xbc,
	WASM_OP_I64_REINTERPRET_F64 = 0xbd,
	WASM_OP_F32_REINTERPRET_I32 = 0xbe,
	WASM_OP_F64_REINTERPRET_I64 = 0xbf,
	WASM_OP_I32_TRUNC_F64_S    = 0xaa,
	WASM_OP_F32_DEMOTE_F64     = 0xb6,
	WASM_OP_F64_PROMOTE_F32    = 0xbb,

	WASM_OP_I32_EXTEND8_S      = 0xc0,
	WASM_OP_I32_EXTEND16_S     = 0xc1,
} WasmOpcode;

/* growable byte buffer */
typedef struct {
	guint8 *data;
	guint32 len;
	guint32 cap;
} WasmBuf;

void wasm_buf_init  (WasmBuf *b);
void wasm_buf_free  (WasmBuf *b);
void wasm_u8        (WasmBuf *b, guint8 v);
void wasm_bytes     (WasmBuf *b, const guint8 *p, guint32 n);
void wasm_uleb      (WasmBuf *b, guint64 v);
void wasm_sleb      (WasmBuf *b, gint64 v);
void wasm_name      (WasmBuf *b, const char *s);
void wasm_f32       (WasmBuf *b, float v);
void wasm_f64       (WasmBuf *b, double v);

/* convenience instruction emitters (append into a function body buffer) */
void wasm_op        (WasmBuf *b, WasmOpcode op);
void wasm_op_local  (WasmBuf *b, WasmOpcode op, guint32 local_idx);   /* local.get/set/tee */
void wasm_i32_const (WasmBuf *b, gint32 v);
void wasm_i64_const (WasmBuf *b, gint64 v);
void wasm_f32_const (WasmBuf *b, float v);
void wasm_f64_const (WasmBuf *b, double v);
void wasm_memarg    (WasmBuf *b, guint32 align_log2, guint32 offset);

/* A local-declaration group (count locals of a given type). */
typedef struct {
	WasmValtype type;
	guint32     count;
} WasmLocalGroup;

/* A function type (for callee signatures referenced by call_indirect). */
#define WASM_FUNCTYPE_MAX_PARAMS 16
typedef struct {
	WasmValtype params [WASM_FUNCTYPE_MAX_PARAMS];
	guint32     nparams;
	WasmValtype ret;        /* WASM_VOID for no return */
} WasmFuncType;

/*
 * Frame a complete module that imports nothing and exports a single function
 * `export_name` with the given signature. `body` holds the function's
 * instruction bytes WITHOUT the locals declaration and WITHOUT the trailing
 * 0x0b end (both are appended here). Result module bytes are appended to `out`.
 */
void wasm_module_single_func (
	const char *export_name,
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *body,
	WasmBuf *out);

/*
 * Frame a module with TWO functions and a shared-memory import (`m`.`h`):
 *   func 0 = the method `f` (signature param_types→ret_type, with `locals`/`f_body`);
 *   func 1 = an entry thunk `e` (signature (i32 args_ptr, i32 ret_ptr)→void, `e_body`,
 *            no locals) which is the only export ("e").
 * The thunk reads the method's args from interp stackvals in linear memory and writes the
 * result back, so the interpreter can invoke any signature uniformly via `e(args, ret)`.
 *
 * `extra_types`/`nextra` add callee function types after T0=method, T1=entry (so callee type k
 * is type index 2+k) — referenced by call_indirect in `f_body`. If `import_table` is TRUE the
 * module also imports the indirect function table as `f`.`f` (table 0) for those call_indirects.
 * If `import_eh_tag` is TRUE the module imports the C++ exception tag as `x`.`e` (kind 0x04) with
 * function type index `eh_type_idx` ((i32)->void) — for `catch <x.e>` in `f_body` (inline-AOT-call EH).
 */
void wasm_module_method_and_entry (
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *f_body,
	const WasmBuf *e_body,
	const WasmFuncType *extra_types, guint32 nextra,
	gboolean import_table,
	gboolean import_eh_tag, guint32 eh_type_idx,
	WasmBuf *out);

/*
 * One member of a batched module. Mirrors the arguments wasm_module_method_and_entry takes for a single
 * method, plus `ti_base` — where this member's block starts in the shared type section.
 *
 * `ti_base` is not advisory. Type indices are ULEB-encoded inline in the body at every call_indirect, so
 * they cannot be relocated after emission; the emitter has to have used this exact base while generating
 * `f_body`. wasm_module_methods_and_entries lays the type section out to match and asserts agreement.
 */
typedef struct {
	const WasmValtype *param_types;
	guint32 nparams;
	WasmValtype ret_type;
	const WasmLocalGroup *locals;
	guint32 nlocal_groups;
	const WasmBuf *f_body;
	const WasmBuf *e_body;
	const WasmFuncType *extra_types;
	guint32 nextra;
	guint32 ti_base;
} WasmModuleMember;

/*
 * Frame ONE module holding N methods and their N entry thunks — the island-batching form of
 * wasm_module_method_and_entry. Layout:
 *
 *   types:     member blocks concatenated; member i occupies [ti_base_i .. ti_base_i + 1 + nextra_i],
 *              namely { method_i, entry_i, extras_i... }, so ti_base_i = sum over j<i of (2 + nextra_j).
 *   funcs:     0..N-1   = the methods (exported "f0".."f{N-1}")
 *              N..2N-1  = the entry thunks (exported "e0".."e{N-1}")
 *   imports:   as the single-method form — memory m.h, table f.f if any member calls indirectly,
 *              tag x.e if any member has an in-method landing pad, global s.p.
 *
 * The point of co-locating is that V8 will not inline across a WebAssembly.Module boundary but inlines
 * freely — through `call` AND speculatively through `call_indirect` — within one. Measured at ~3.5-6x
 * per call on accessor-sized callees, and flat out to 500 functions per module (scratchpad/scalerun.mjs).
 *
 * Each thunk calls its own method with `call i`, so the emitter must have written member i's index there
 * rather than the single-method form's constant 0.
 */
void wasm_module_methods_and_entries (
	const WasmModuleMember *members, guint32 nmembers,
	gboolean import_table,
	gboolean import_eh_tag, guint32 eh_type_idx,
	WasmBuf *out);

/* Name section for a batched module: methods at func 0..n-1, entry thunks at n..2n-1. */
void
wasm_module_append_name_section_multi (WasmBuf *out, const char *module_name, const char *const *names, guint32 n);

/*
 * Frame a module exporting a single function `t` (the interp-entry thunk) with signature
 * param_types->ret_type, importing shared memory (`m`.`h`) and the indirect function table (`f`.`f`).
 * Beyond the thunk's own type T0, two callee function types are predefined for the body's
 * call_indirects: T1 = ()->i32 (mono_wasm_jit_scratch) and T2 = (i32,i32)->i32
 * (mono_wasm_jit_call_interp). The body marshals the scalar args into the per-thread scratch buffer,
 * drives the target through the interpreter, and loads back the result — so the thunk is a drop-in
 * call_indirect target with the same scalar ABI as a JITted method's `f`, for un-JITted callees.
 */
void wasm_module_interp_thunk (
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *body,
	WasmBuf *out);

/*
 * Append a custom "name" section (id 0) to an already-framed module so V8 prints real method
 * names in wasm stack traces instead of anonymous wasm-function[N]. Names func0 = the method
 * (`func0_name`) and func1 = "entry"; sets the module name to `module_name`. Cheap; only emit
 * when symbolication is wanted (gated by MONO_WASM_JIT_NAMES in the emitter).
 */
void wasm_module_append_name_section (WasmBuf *out, const char *module_name, const char *func0_name);

#endif /* __MONO_MINI_WASM_ENCODER_H__ */
