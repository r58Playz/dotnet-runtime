/*
 * The two module framers wasm_module_assemble replaced, frozen here as the ORACLE for the byte-identity
 * gate. They are not built into the runtime any more -- see enctest/run.sh.
 *
 * Keeping them is the whole reason the relocatable-body refactor is checkable in seconds instead of in a
 * 35-minute build plus a browser run: t.c and t2.c assert that wasm_module_assemble reproduces these two
 * byte for byte, at N == 1 and at N > 1, over randomised member shapes. Do not "fix" anything here. The
 * point is that it is the old code.
 */
#ifndef WJ_ENCTEST_LEGACY_H
#define WJ_ENCTEST_LEGACY_H
#include "shim.h"
#include "wasm-encoder.h"

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
 *
 * `fimports`/`nfimports` declare direct-call helper imports (see WasmFuncImport). Function imports take
 * function indices 0..nfimports-1 in the order given, so the module's own two functions shift to
 * nfimports (the method `f`) and nfimports+1 (the entry thunk `e`) — any `call` immediate in `f_body`/`e_body`
 * naming a DEFINED function must already account for that offset, since those immediates are ULEB-encoded
 * inline and cannot be shifted after the fact. Helper `call` immediates need no such adjustment: slot i is
 * function index i regardless of how many slots end up being used, which is what makes it safe to assign
 * them in first-use order while the body is still being emitted.
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
	const WasmFuncImport *fimports, guint32 nfimports,
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
	const WasmFuncImport *fimports, guint32 nfimports,
	WasmBuf *out);

/* A u32 ULEB padded to exactly 5 bytes, and an in-place rewrite of one. Lets a value be baked into a body
 * before it is known -- specifically a batched entry thunk's `call <funcidx>`, which is offset by the batch's
 * final function-import count. */
void wasm_uleb5 (WasmBuf *b, guint32 value);
void wasm_uleb5_patch (WasmBuf *b, guint32 off, guint32 value);

#endif
