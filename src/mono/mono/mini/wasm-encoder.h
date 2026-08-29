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
	WASM_OP_I64_CLZ       = 0x79,
	WASM_OP_I64_CTZ       = 0x7a,
	WASM_OP_I64_POPCNT    = 0x7b,
	WASM_OP_I64_ROTL      = 0x89,
	WASM_OP_I64_ROTR      = 0x8a,

	WASM_OP_F32_ABS       = 0x8b,
	WASM_OP_F32_NEG       = 0x8c,
	WASM_OP_F32_CEIL      = 0x8d,
	WASM_OP_F32_FLOOR     = 0x8e,
	WASM_OP_F32_TRUNC     = 0x8f,
	WASM_OP_F32_NEAREST   = 0x90,
	WASM_OP_F32_SQRT      = 0x91,
	WASM_OP_F32_ADD       = 0x92,
	WASM_OP_F32_SUB       = 0x93,
	WASM_OP_F32_MUL       = 0x94,
	WASM_OP_F32_DIV       = 0x95,
	WASM_OP_F32_MIN       = 0x96,
	WASM_OP_F32_MAX       = 0x97,
	WASM_OP_F32_COPYSIGN  = 0x98,
	WASM_OP_F64_ABS       = 0x99,
	WASM_OP_F64_NEG       = 0x9a,
	WASM_OP_F64_CEIL      = 0x9b,
	WASM_OP_F64_FLOOR     = 0x9c,
	WASM_OP_F64_TRUNC     = 0x9d,
	WASM_OP_F64_NEAREST   = 0x9e,
	WASM_OP_F64_SQRT      = 0x9f,
	WASM_OP_F64_ADD       = 0xa0,
	WASM_OP_F64_SUB       = 0xa1,
	WASM_OP_F64_MUL       = 0xa2,
	WASM_OP_F64_DIV       = 0xa3,
	WASM_OP_F64_MIN       = 0xa4,
	WASM_OP_F64_MAX       = 0xa5,
	WASM_OP_F64_COPYSIGN  = 0xa6,

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

/*
 * Saturating float->int truncation ("non-trapping float-to-int conversions"), 0xFC-prefixed with a
 * ULEB sub-opcode -- emit via wasm_op_sat(), not wasm_op(), since these are two-byte forms.
 *
 * These are the ONLY correct choice here. The plain 0xa8..0xab / 0xae..0xb1 truncations TRAP on
 * out-of-range input or NaN, which would turn a merely-unspecified conversion into a hard abort. The
 * saturating forms clamp to the type's min/max and map NaN to 0, which is exactly Java's (int)float
 * semantics -- and IKVM-translated Java is the workload. It also matches the interpreter, whose
 * MINT_CONV_I4_R4 is a plain C `(gint32) float` cast that clang lowers to trunc_sat on wasm, so a
 * method cannot change behaviour when it tiers from interp to JIT.
 */
typedef enum {
	WASM_SAT_I32_TRUNC_F32_S = 0,
	WASM_SAT_I32_TRUNC_F32_U = 1,
	WASM_SAT_I32_TRUNC_F64_S = 2,
	WASM_SAT_I32_TRUNC_F64_U = 3,
	WASM_SAT_I64_TRUNC_F32_S = 4,
	WASM_SAT_I64_TRUNC_F32_U = 5,
	WASM_SAT_I64_TRUNC_F64_S = 6,
	WASM_SAT_I64_TRUNC_F64_U = 7,
} WasmSatOpcode;

/* growable byte buffer */
typedef struct {
	guint8 *data;
	guint32 len;
	guint32 cap;
	/* Peephole state for `local.set N; local.get N` -> `local.tee N` (wasm_op_local).
	 *
	 * `tee_end` is the buffer length immediately after the last local.set was written, and `tee_off` where
	 * its opcode byte sits. The rewrite fires only when the very next thing emitted is a local.get of the
	 * same index AND nothing else has been appended since — which is exactly the test `tee_end == len`.
	 * That adjacency check is what makes it safe: any intervening instruction, including a block/end that
	 * could make the get reachable without the set, moves `len` and disarms it. */
	guint32 tee_off;
	guint32 tee_end;
	guint32 tee_idx;
	/* Relocations recorded in this buffer, or NULL for a buffer that carries none (every buffer used to
	 * build a SECTION). Only function bodies carry relocs. See WasmRelocs below. */
	struct _WasmRelocs *relocs;
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
void wasm_op_sat    (WasmBuf *b, WasmSatOpcode op);   /* 0xFC-prefixed saturating float->int trunc */
void wasm_op_local  (WasmBuf *b, WasmOpcode op, guint32 local_idx);   /* local.get/set/tee */
void wasm_i32_const (WasmBuf *b, gint32 v);
void wasm_i64_const (WasmBuf *b, gint64 v);
void wasm_f32_const (WasmBuf *b, float v);
void wasm_f64_const (WasmBuf *b, double v);
void wasm_memarg    (WasmBuf *b, guint32 align_log2, guint32 offset);

/*
 * RELOCATIONS -- why a body is not finished when it is emitted.
 *
 * Everything a wasm body encodes inline is either position-independent (there are no byte-offset branches
 * in wasm: `br` takes a label DEPTH, memarg offsets are absolute constants, local indices are function
 * local) or an INDEX into a module-level space. The index spaces are the whole problem: a body's type
 * indices and `call` immediates are only meaningful against the module it was framed into. Bake them and
 * the body can never be re-framed with different neighbours without being compiled again from IL, which is
 * exactly what made module batching cost a full re-JIT per member.
 *
 * So the emitter leaves a HOLE at every such site and records where. A hole is ZERO bytes long: nothing is
 * written, and serialization copies the runs of bytes between holes and encodes each site fresh against the
 * final layout. That is what lets a call change FORM as well as index -- `call_indirect` (~15 x86), an
 * imported `call` (~5) and a module-local `call` (1, and the only one V8 will inline) have different
 * lengths, so patching an immediate in place could not express the choice. (Contrast gex, which pads every
 * relocatable index to a fixed 5-byte LEB and patches in place -- sufficient there because it never changes
 * a call's form, only its index.)
 *
 * Serialization is O(bytes + relocs) either way: a memcpy per run plus a few bytes per site.
 */
typedef enum {
	WASM_RELOC_TYPE_U,    /* a type-index immediate, ULEB-encoded */
	WASM_RELOC_TYPE_S,    /* a type-index immediate, SLEB-encoded (a multi-value block/if blocktype) */
	WASM_RELOC_INDIRECT,  /* `call_indirect <type> 0` with the index already on the stack */
	WASM_RELOC_HELPER,    /* a call to a fixed table index: a C runtime helper in dotnet.native.wasm */
	WASM_RELOC_HELPER_CI, /* a fixed table index the helper-import conversion never covered, so it has
	                       * always been a constant-index call_indirect: the runtime JIT-icall and the
	                       * residual throw continuation */
	WASM_RELOC_AOT,       /* a call to a fixed table index: an AOT method body. All three are encoded
	                       * identically and differ only in which knob gates them at assembly, which
	                       * matters because they are wildly different populations: ~25 distinct helpers,
	                       * hundreds of AOT bodies at ~90k inline-AOT calls per frame, and the
	                       * never-converted remainder, one of whose indices (403815) carries 32.3% of all
	                       * constant-index indirect traffic on its own. */
	WASM_RELOC_CALL,      /* a call to another JITted method, identified by `sym` and its f-slot */
	WASM_RELOC_SELF,      /* a `call` naming the owning member's own method function; always module-local */
} WasmRelocKind;

typedef struct {
	guint32  off;          /* insertion point in the code buffer. The hole occupies NO bytes. */
	guint16  kind;         /* WasmRelocKind */
	guint16  tpool;        /* index into the owning member's type block (0 = its own sig, 1 = its thunk's) */
	guint32  table_index;  /* HELPER: the C function pointer. CALL/SELF: the callee's f-slot. */
	gpointer sym;          /* CALL: callee identity (a MonoMethod*), for co-location and the ABI check */
} WasmReloc;

typedef struct _WasmRelocs {
	WasmReloc *r;
	guint32    n, cap;
} WasmRelocs;

/* How one reloc was resolved. `idx` is the function index for IMPORT/LOCAL and unused for INDIRECT. */
typedef enum { WASM_FORM_INDIRECT, WASM_FORM_IMPORT, WASM_FORM_LOCAL } WasmRelocForm;
typedef struct { guint8 form; guint32 idx; } WasmRelocFix;

/* Attach a reloc list to a body buffer. Must be called before any wasm_reloc on it. */
void wasm_buf_init_relocs (WasmBuf *b);

/*
 * Record a hole at the current end of `b`.
 *
 * Also DISARMS the local.tee peephole, and that is load-bearing rather than tidy: a hole is zero bytes, so
 * without this `tee_end == len` would still hold across it and a `local.set N` / call / `local.get N`
 * sequence would be rewritten into `local.tee N` -- deleting the call's effect on the stack.
 */
void wasm_reloc (WasmBuf *b, WasmRelocKind kind, guint32 tpool, guint32 table_index, gpointer sym);

/*
 * Emit `src` into `out` with every hole filled: `fix[k]` resolves `src->relocs->r[k]`, and a type index
 * `tpool` becomes `ti_base + tpool`. `out` receives instructions only -- no locals declaration, no
 * trailing `end`, exactly like the buffer that goes into a WasmAsmMember body.
 */
void wasm_body_serialize (const WasmBuf *src, const WasmRelocFix *fix, guint32 ti_base, WasmBuf *out);

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
 * One runtime helper the method body calls DIRECTLY, declared as a wasm function import.
 *
 * Without this a call to a C helper is `i32.const <table index>; call_indirect`, because under wasm a C
 * function pointer IS an indirect-function-table index, so the emitter naturally has an index rather than a
 * name. V8 lowers every call_indirect -- even one whose index is a compile-time constant -- to a bounds check,
 * a signature check, table-index arithmetic and `call *`; it does not fold a constant index into a direct
 * call. Measured: a 54-byte-IL MethodHandle stub emits 21 call_indirect, 12 of them to constant helper
 * indices, and the x86 for it contains exactly 21 `call *`.
 *
 * Declaring the helper as an import turns the site into `call <slot>` with none of that preamble. `name` is
 * the table index in decimal, under module name "h"; the instantiation side resolves it with
 * WebAssembly.Table.prototype.get, so no linker export and no fixed helper registry is needed.
 *
 * `type_idx` is an index into this module's type section -- reuse the same callee functype the call_indirect
 * already referenced.
 */
typedef struct {
	guint32 table_index;    /* C function pointer value == indirect-function-table index */
	guint32 type_idx;       /* functype index (T0=method, T1=entry, 2+k = extra_types [k]) */
	/* TRUE if this is a JITted METHOD's f-slot rather than a C helper or an AOT body, in which case the
	 * import is named "m<index>" instead of "<index>". The distinction is load-bearing, not cosmetic.
	 *
	 * A helper's table index is fixed for the life of the process, so binding it at instantiation is
	 * always correct. A method f-slot is NOT: until the callee's module is instantiated ON THIS WORKER
	 * the slot holds the jiterpreter's prefill, mono_jiterp_placeholder_jit_call -- a REAL, CALLABLE
	 * wasm function of type (i32,i32,i32,i32)->void. So an import of an un-instantiated f-slot does not
	 * fail to resolve; it binds successfully whenever the caller's expected type is that same very common
	 * shape, and every call to the callee then lands in the placeholder, whose body is `*thrown = 999` --
	 * i.e. it writes 999 through the caller's FOURTH ARGUMENT as a pointer and returns.
	 *
	 * That is silent heap corruption with no LinkError, no trap and no wrong-type diagnostic, and it is
	 * what the direct-import boot crash was. The "m" prefix is how the instantiation-side resolver tells
	 * the two cases apart so it can refuse the second. (The call_indirect path had the same hazard and
	 * fixed it separately in jit138 with the mono_wasm_jit_slot_live gate; see mini-wasm.c:1106.) */
	guint8  method;
} WasmFuncImport;


/* Name section for a batched module: methods at func nfimports..nfimports+n-1, thunks after them. */
void
wasm_module_append_name_section_multi (WasmBuf *out, const char *module_name, const char *const *names, guint32 n, guint32 nfimports);

/*
 * One member of a module framed by wasm_module_assemble.
 *
 * `f_body`/`e_body` arrive ALREADY RELOCATED: the caller has resolved every type index and every call
 * immediate against this module's final layout. That is the whole point -- a member's bytes are produced
 * once, by one compile, and can then be framed into any module with any other members for the cost of a
 * memcpy, instead of being re-compiled from IL because their indices were baked against the old layout.
 *
 * `types` is this member's type block, in the order it occupies the shared type section:
 *   types[0] = the method's own signature      (its funcidx's type)
 *   types[1] = the entry thunk's signature     ((i32,i32)->void)
 *   types[2..] = callee functypes the bodies reference by call_indirect / multi-value blocktype
 * so `ntypes >= 2` always. Member i's block starts at ti_base_i = sum of ntypes over j<i, and the caller
 * must have relocated against that same running sum.
 */
typedef struct {
	const WasmLocalGroup *locals;
	guint32               nlocal_groups;
	const WasmBuf        *f_body;
	const WasmBuf        *e_body;
	const WasmFuncType   *types;
	guint32               ntypes;
} WasmAsmMember;

/*
 * Frame N members into ONE module -- the only framer there is.
 *
 * Layout:
 *   types:   member blocks back to back, so member i occupies [ti_base_i .. ti_base_i + ntypes_i - 1].
 *   funcs:   the imports first (function imports take indices 0..nfimports-1 whatever the section order),
 *            then the N methods, then the N entry thunks -- so member i's method is nfimports + i.
 *   exports: "f"/"e" at N == 1, "f<i>"/"e<i>" above it, matching the two instantiate paths.
 *
 * Nothing is patched afterwards and nothing is asserted about what the caller baked, because the caller
 * bakes nothing: every index a body needs arrived as a relocation.
 */
void wasm_module_assemble (
	const WasmAsmMember *members, guint32 nmembers,
	gboolean import_table,
	gboolean import_eh_tag, guint32 eh_type_idx,
	const WasmFuncImport *fimports, guint32 nfimports,
	WasmBuf *out);

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
/* `nfimports` is the module's function-import count: the defined method/entry thunk are named at
 * nfimports+0 and nfimports+1, because imported functions occupy the lower indices. Passing 0 when the
 * module actually imports helpers names the imports and leaves the bodies anonymous to perf. */
void wasm_module_append_name_section (WasmBuf *out, const char *module_name, const char *func0_name, guint32 nfimports);

#endif /* __MONO_MINI_WASM_ENCODER_H__ */
