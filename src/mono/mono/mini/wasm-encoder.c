/*
 * wasm-encoder.c: minimal WebAssembly byte encoder for the runtime wasm JIT backend.
 * See wasm-encoder.h. Host is little-endian (wasm target + x64 build host), which
 * matches the wasm encoding for raw float/double bytes.
 */

#include <config.h>
#include <glib.h>
#include <string.h>
#include "wasm-encoder.h"

#define WASM_BUF_MIN_CAP 64

void
wasm_buf_init (WasmBuf *b)
{
	b->cap = WASM_BUF_MIN_CAP;
	b->len = 0;
	b->data = (guint8 *) g_malloc (b->cap);
}

void
wasm_buf_free (WasmBuf *b)
{
	g_free (b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static void
wasm_buf_ensure (WasmBuf *b, guint32 extra)
{
	/* 64-bit arithmetic so neither b->len + extra nor the doubling can wrap a guint32 to a too-small
	 * capacity (which would under-allocate -> heap overflow). JIT modules are bounded well under 4GB. */
	guint64 need = (guint64) b->len + extra;
	guint64 cap;
	if (need <= b->cap)
		return;
	cap = b->cap ? b->cap : WASM_BUF_MIN_CAP;
	while (cap < need)
		cap *= 2;
	if (cap > 0xffffffffu)
		g_error ("wasm-encoder buffer exceeds 4GB");   /* unreachable for real JIT modules */
	b->cap = (guint32) cap;
	b->data = (guint8 *) g_realloc (b->data, b->cap);
}

void
wasm_u8 (WasmBuf *b, guint8 v)
{
	wasm_buf_ensure (b, 1);
	b->data [b->len++] = v;
}

void
wasm_bytes (WasmBuf *b, const guint8 *p, guint32 n)
{
	if (!n)
		return;
	wasm_buf_ensure (b, n);
	memcpy (b->data + b->len, p, n);
	b->len += n;
}

void
wasm_uleb (WasmBuf *b, guint64 value)
{
	do {
		guint8 byte = (guint8) (value & 0x7f);
		value >>= 7;
		if (value != 0)
			byte |= 0x80;
		wasm_u8 (b, byte);
	} while (value != 0);
}

void
wasm_sleb (WasmBuf *b, gint64 value)
{
	gboolean more = TRUE;
	while (more) {
		guint8 byte = (guint8) (value & 0x7f);
		value >>= 7; /* arithmetic shift, value is signed */
		if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)))
			more = FALSE;
		else
			byte |= 0x80;
		wasm_u8 (b, byte);
	}
}

void
wasm_name (WasmBuf *b, const char *s)
{
	guint32 n = (guint32) strlen (s);
	wasm_uleb (b, n);
	wasm_bytes (b, (const guint8 *) s, n);
}

void
wasm_f32 (WasmBuf *b, float v)
{
	wasm_bytes (b, (const guint8 *) &v, 4);
}

void
wasm_f64 (WasmBuf *b, double v)
{
	wasm_bytes (b, (const guint8 *) &v, 8);
}

void
wasm_op (WasmBuf *b, WasmOpcode op)
{
	wasm_u8 (b, (guint8) op);
}

/* Saturating float->int truncation: the 0xFC misc prefix plus a ULEB sub-opcode. See WasmSatOpcode
 * for why the saturating forms are the only correct choice (the plain ones trap). */
void
wasm_op_sat (WasmBuf *b, WasmSatOpcode op)
{
	wasm_u8 (b, 0xFC);
	wasm_uleb (b, (guint32) op);
}

void
wasm_op_local (WasmBuf *b, WasmOpcode op, guint32 local_idx)
{
	wasm_u8 (b, (guint8) op);
	wasm_uleb (b, local_idx);
}

void
wasm_i32_const (WasmBuf *b, gint32 v)
{
	wasm_u8 (b, WASM_OP_I32_CONST);
	wasm_sleb (b, v);
}

void
wasm_i64_const (WasmBuf *b, gint64 v)
{
	wasm_u8 (b, WASM_OP_I64_CONST);
	wasm_sleb (b, v);
}

void
wasm_f32_const (WasmBuf *b, float v)
{
	wasm_u8 (b, WASM_OP_F32_CONST);
	wasm_f32 (b, v);
}

void
wasm_f64_const (WasmBuf *b, double v)
{
	wasm_u8 (b, WASM_OP_F64_CONST);
	wasm_f64 (b, v);
}

void
wasm_memarg (WasmBuf *b, guint32 align_log2, guint32 offset)
{
	wasm_uleb (b, align_log2);
	wasm_uleb (b, offset);
}

static void
emit_section (WasmBuf *out, guint8 id, const WasmBuf *sec)
{
	wasm_u8 (out, id);
	wasm_uleb (out, sec->len);
	wasm_bytes (out, sec->data, sec->len);
}

void
wasm_module_single_func (
	const char *export_name,
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *body,
	WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	WasmBuf sec;
	guint32 i;

	wasm_bytes (out, header, 8);

	/* Type section (1): a single function type */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_u8 (&sec, 0x60);
	wasm_uleb (&sec, nparams);
	for (i = 0; i < nparams; ++i)
		wasm_u8 (&sec, (guint8) param_types [i]);
	if (ret_type == WASM_VOID) {
		wasm_uleb (&sec, 0);
	} else {
		wasm_uleb (&sec, 1);
		wasm_u8 (&sec, (guint8) ret_type);
	}
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	/* Function section (3): one function of type 0 */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_uleb (&sec, 0);
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* Export section (7): export the function (func index 0, no func imports) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_name (&sec, export_name);
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, 0);
	emit_section (out, 7, &sec);
	wasm_buf_free (&sec);

	/* Code section (10): one code entry = locals decl + body + end */
	wasm_buf_init (&sec);
	{
		WasmBuf entry;
		guint32 ngroups = 0;
		wasm_buf_init (&entry);

		for (i = 0; i < nlocal_groups; ++i)
			if (locals [i].count > 0)
				ngroups++;
		wasm_uleb (&entry, ngroups);
		for (i = 0; i < nlocal_groups; ++i) {
			if (locals [i].count == 0)
				continue;
			wasm_uleb (&entry, locals [i].count);
			wasm_u8 (&entry, (guint8) locals [i].type);
		}
		wasm_bytes (&entry, body->data, body->len);
		wasm_u8 (&entry, WASM_OP_END);

		wasm_uleb (&sec, 1);          /* one code entry */
		wasm_uleb (&sec, entry.len);  /* entry size */
		wasm_bytes (&sec, entry.data, entry.len);
		wasm_buf_free (&entry);
	}
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
}

static void
emit_code_entry (WasmBuf *sec, const WasmLocalGroup *locals, guint32 nlocal_groups, const WasmBuf *body)
{
	WasmBuf entry;
	guint32 i, ngroups = 0;
	wasm_buf_init (&entry);
	for (i = 0; i < nlocal_groups; ++i)
		if (locals [i].count > 0)
			ngroups++;
	wasm_uleb (&entry, ngroups);
	for (i = 0; i < nlocal_groups; ++i) {
		if (locals [i].count == 0)
			continue;
		wasm_uleb (&entry, locals [i].count);
		wasm_u8 (&entry, (guint8) locals [i].type);
	}
	wasm_bytes (&entry, body->data, body->len);
	wasm_u8 (&entry, WASM_OP_END);
	wasm_uleb (sec, entry.len);
	wasm_bytes (sec, entry.data, entry.len);
	wasm_buf_free (&entry);
}

void
wasm_module_method_and_entry (
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *f_body,
	const WasmBuf *e_body,
	const WasmFuncType *extra_types, guint32 nextra,
	gboolean import_table,
	gboolean import_eh_tag, guint32 eh_type_idx,
	WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	WasmBuf sec;
	guint32 i, j;
	static const WasmLocalGroup no_locals [1] = { { WASM_I32, 0 } };

	wasm_bytes (out, header, 8);

	/* Type section (1): T0 = method (params->ret), T1 = entry thunk ((i32,i32)->void),
	 * T2.. = callee function types referenced by call_indirect in the method body */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 2 + nextra);
	wasm_u8 (&sec, 0x60);
	wasm_uleb (&sec, nparams);
	for (i = 0; i < nparams; ++i)
		wasm_u8 (&sec, (guint8) param_types [i]);
	if (ret_type == WASM_VOID) {
		wasm_uleb (&sec, 0);
	} else {
		wasm_uleb (&sec, 1);
		wasm_u8 (&sec, (guint8) ret_type);
	}
	wasm_u8 (&sec, 0x60);
	wasm_uleb (&sec, 2);
	wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_uleb (&sec, 0);
	for (i = 0; i < nextra; ++i) {
		wasm_u8 (&sec, 0x60);
		wasm_uleb (&sec, extra_types [i].nparams);
		for (j = 0; j < extra_types [i].nparams; ++j)
			wasm_u8 (&sec, (guint8) extra_types [i].params [j]);
		if (extra_types [i].ret == WASM_VOID) {
			wasm_uleb (&sec, 0);
		} else {
			wasm_uleb (&sec, 1);
			wasm_u8 (&sec, (guint8) extra_types [i].ret);
		}
	}
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	/* Import section (2): shared memory m.h (matches the threads runtime heap); — when the method body
	 * uses call_indirect — the indirect function table f.f as table 0; — when it uses try/catch — the
	 * C++ exception tag x.e (matches the runtime's __cpp_exception export, like the jiterpreter); and
	 * three runtime globals: the mutable C stack pointer plus immutable addresses of this instance's
	 * thread-local wasm-JIT slot-liveness pointer/capacity. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, (guint32) (1 + (import_table ? 1 : 0) + (import_eh_tag ? 1 : 0) + 3 /* s.p/l/c globals */));
	wasm_name (&sec, "m");
	wasm_name (&sec, "h");
	wasm_u8 (&sec, 0x02);   /* memory */
	/* limits flag must match the runtime heap's SHARED-ness exactly: a threads build has a shared
	 * memory (0x03 = shared+max), a single-threaded build an unshared one (0x01 = max only) — a
	 * mismatch fails instantiation ("mismatch in shared state of memory"). */
#ifdef DISABLE_THREADS
	wasm_u8 (&sec, 0x01);   /* limits: max, unshared */
#else
	wasm_u8 (&sec, 0x03);   /* limits: shared + max */
#endif
	wasm_uleb (&sec, 256);
	wasm_uleb (&sec, 65535); /* must be >= the runtime heap's max (65535 pages); 32768 fails to match */
	if (import_table) {
		wasm_name (&sec, "f");
		wasm_name (&sec, "f");
		wasm_u8 (&sec, 0x01);                  /* import kind: table */
		wasm_u8 (&sec, (guint8) WASM_FUNCREF); /* element type funcref (0x70) */
		wasm_u8 (&sec, 0x00);                  /* limits: min only (matches any actual table) */
		wasm_uleb (&sec, 0);                   /* min 0 */
	}
	if (import_eh_tag) {
		wasm_name (&sec, "x");
		wasm_name (&sec, "e");
		wasm_u8 (&sec, 0x04);            /* import kind: tag */
		wasm_u8 (&sec, 0x00);            /* tag attribute: exception */
		wasm_uleb (&sec, eh_type_idx);   /* function type index ((i32)->void) */
	}
	{
		/* __stack_pointer as global s.p (kind 0x03), i32 mutable. It is global index 0.
		 * The C-stack frame prologue/epilogue use global.get/set 0 for the SP save/restore. Requires the main
		 * module to export __stack_pointer (-Wl,--export=__stack_pointer, set in browser.proj +
		 * BrowserWasmApp.targets) and the instantiation to pass it as s.p. */
		wasm_name (&sec, "s");
		wasm_name (&sec, "p");
		wasm_u8 (&sec, 0x03);            /* import kind: global */
		wasm_u8 (&sec, (guint8) WASM_I32); /* global type: i32 */
		wasm_u8 (&sec, 0x01);            /* mutability: mutable */
	}
	{
		/* Per-instance TLS addresses, global indices 1 and 2. Dynamic wasm modules are cached process-wide
		 * but instantiated once per worker, so these cannot be baked into the module body. Supplying them
		 * as immutable imports selects the current worker's TLS block at instantiation without paying two
		 * C-boundary calls on every invocation of every virtual-call-containing method. The pointed-to
		 * values remain live: a slot-bitmap realloc updates *s.l and the capacity update is visible at *s.c. */
		wasm_name (&sec, "s");
		wasm_name (&sec, "l");
		wasm_u8 (&sec, 0x03);
		wasm_u8 (&sec, (guint8) WASM_I32);
		wasm_u8 (&sec, 0x00);            /* immutable */
		wasm_name (&sec, "s");
		wasm_name (&sec, "c");
		wasm_u8 (&sec, 0x03);
		wasm_u8 (&sec, (guint8) WASM_I32);
		wasm_u8 (&sec, 0x00);            /* immutable */
	}
	emit_section (out, 2, &sec);
	wasm_buf_free (&sec);

	/* Function section (3): func0 = method (type 0), func1 = entry (type 1) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 2);
	wasm_uleb (&sec, 0);
	wasm_uleb (&sec, 1);
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* Export section (7): the method `f` (func 0, for call_indirect from JITted callers) and
	 * the entry thunk `e` (func 1, for interp entry) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 2);
	wasm_name (&sec, "f");
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, 0);
	wasm_name (&sec, "e");
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, 1);
	emit_section (out, 7, &sec);
	wasm_buf_free (&sec);

	/* Code section (10): method body, then entry-thunk body */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 2);
	emit_code_entry (&sec, locals, nlocal_groups, f_body);
	emit_code_entry (&sec, no_locals, 1, e_body);
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
}

/* Append one functype (0x60 form) to a type section buffer. */
static void
emit_functype (WasmBuf *sec, const WasmValtype *params, guint32 nparams, WasmValtype ret)
{
	guint32 i;
	wasm_u8 (sec, 0x60);
	wasm_uleb (sec, nparams);
	for (i = 0; i < nparams; ++i)
		wasm_u8 (sec, (guint8) params [i]);
	if (ret == WASM_VOID) {
		wasm_uleb (sec, 0);
	} else {
		wasm_uleb (sec, 1);
		wasm_u8 (sec, (guint8) ret);
	}
}

void
wasm_module_methods_and_entries (
	const WasmModuleMember *members, guint32 nmembers,
	gboolean import_table,
	gboolean import_eh_tag, guint32 eh_type_idx,
	WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	static const WasmLocalGroup no_locals [1] = { { WASM_I32, 0 } };
	static const WasmValtype entry_params [2] = { WASM_I32, WASM_I32 };
	WasmBuf sec;
	guint32 i, j, base = 0;

	g_assert (nmembers > 0);

	wasm_bytes (out, header, 8);

	/* Type section (1): member blocks back to back — { method, entry, extras... } each. */
	wasm_buf_init (&sec);
	{
		guint32 ntypes = 0;
		for (i = 0; i < nmembers; ++i)
			ntypes += 2 + members [i].nextra;
		wasm_uleb (&sec, ntypes);
	}
	for (i = 0; i < nmembers; ++i) {
		const WasmModuleMember *m = &members [i];
		/* The body already has this base baked into every call_indirect; if the caller computed it
		 * differently from the layout here, the module would still validate but call through the wrong
		 * signatures. Fail loudly instead. */
		g_assertf (m->ti_base == base, "wasm batch member %u: ti_base %u but its block starts at %u",
		           i, m->ti_base, base);
		emit_functype (&sec, m->param_types, m->nparams, m->ret_type);
		emit_functype (&sec, entry_params, 2, WASM_VOID);
		for (j = 0; j < m->nextra; ++j)
			emit_functype (&sec, m->extra_types [j].params, m->extra_types [j].nparams, m->extra_types [j].ret);
		base += 2 + m->nextra;
	}
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	/* Import section (2): identical to the single-method form — the shared heap, optionally the indirect
	 * function table and the C++ exception tag, __stack_pointer, and the two per-instance TLS addresses. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, (guint32) (1 + (import_table ? 1 : 0) + (import_eh_tag ? 1 : 0) + 3));
	wasm_name (&sec, "m");
	wasm_name (&sec, "h");
	wasm_u8 (&sec, 0x02);
	/* must match the runtime heap's shared-ness exactly or instantiation fails */
#ifdef DISABLE_THREADS
	wasm_u8 (&sec, 0x01);
#else
	wasm_u8 (&sec, 0x03);
#endif
	wasm_uleb (&sec, 256);
	wasm_uleb (&sec, 65535);
	if (import_table) {
		wasm_name (&sec, "f");
		wasm_name (&sec, "f");
		wasm_u8 (&sec, 0x01);
		wasm_u8 (&sec, (guint8) WASM_FUNCREF);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, 0);
	}
	if (import_eh_tag) {
		wasm_name (&sec, "x");
		wasm_name (&sec, "e");
		wasm_u8 (&sec, 0x04);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, eh_type_idx);
	}
	wasm_name (&sec, "s");
	wasm_name (&sec, "p");
	wasm_u8 (&sec, 0x03);
	wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_u8 (&sec, 0x01);
	wasm_name (&sec, "s");
	wasm_name (&sec, "l");
	wasm_u8 (&sec, 0x03);
	wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_u8 (&sec, 0x00);
	wasm_name (&sec, "s");
	wasm_name (&sec, "c");
	wasm_u8 (&sec, 0x03);
	wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_u8 (&sec, 0x00);
	emit_section (out, 2, &sec);
	wasm_buf_free (&sec);

	/* Function section (3): all N methods first, then all N thunks, so member i's method is funcidx i —
	 * knowable from batch membership alone, which is what lets each thunk bake `call i`. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, nmembers * 2);
	for (i = 0; i < nmembers; ++i)
		wasm_uleb (&sec, members [i].ti_base);          /* method type */
	for (i = 0; i < nmembers; ++i)
		wasm_uleb (&sec, members [i].ti_base + 1);      /* entry-thunk type */
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* Export section (7): "f<i>" = method i, "e<i>" = its entry thunk. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, nmembers * 2);
	for (i = 0; i < nmembers; ++i) {
		char nm [24];
		g_snprintf (nm, sizeof (nm), "f%u", i);
		wasm_name (&sec, nm);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, i);
	}
	for (i = 0; i < nmembers; ++i) {
		char nm [24];
		g_snprintf (nm, sizeof (nm), "e%u", i);
		wasm_name (&sec, nm);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, nmembers + i);
	}
	emit_section (out, 7, &sec);
	wasm_buf_free (&sec);

	/* Code section (10): same order as the function section. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, nmembers * 2);
	for (i = 0; i < nmembers; ++i)
		emit_code_entry (&sec, members [i].locals, members [i].nlocal_groups, members [i].f_body);
	for (i = 0; i < nmembers; ++i)
		emit_code_entry (&sec, no_locals, 1, members [i].e_body);
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
}

void
wasm_module_append_name_section (WasmBuf *out, const char *module_name, const char *func0_name)
{
	/* Append a custom "name" section (id 0) so V8 prints real Mono method names in wasm stack
	 * traces (devtools, Error.stack, CDP) instead of anonymous wasm-function[N]. Custom sections
	 * may appear after the code section. Subsection 0 = module name; subsection 1 = function names.
	 * This module defines func0 = the method `f`, func1 = the entry thunk `e`; there are no function
	 * imports (only memory/table/tag), so function indices start at 0. */
	WasmBuf sec, sub;
	const char *m = module_name ? module_name : "wasmjit";
	const char *f0 = func0_name ? func0_name : "method";

	wasm_buf_init (&sec);
	wasm_name (&sec, "name");

	/* subsection 0: module name */
	wasm_buf_init (&sub);
	wasm_name (&sub, m);
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	/* subsection 1: function names (func0 = method, func1 = entry thunk) */
	wasm_buf_init (&sub);
	wasm_uleb (&sub, 2);          /* two name-map entries */
	wasm_uleb (&sub, 0);          /* func index 0 */
	wasm_name (&sub, f0);
	wasm_uleb (&sub, 1);          /* func index 1 */
	wasm_name (&sub, "entry");
	wasm_u8 (&sec, 0x01);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	emit_section (out, 0, &sec);
	wasm_buf_free (&sec);
}

/*
 * Name section for a BATCHED module (wasm_module_methods_and_entries).
 *
 * The single-method variant above hardcodes a two-entry map because that module has exactly func0 =
 * method and func1 = entry thunk. A batch has N methods at indices 0..N-1 followed by N entry thunks at
 * N..2N-1, and nothing was emitting names for it at all -- so every batched module was anonymous and
 * the whole profiling toolkit (cdpperf, cdptrace, cdpprofile, the subsystem split) reported
 * wasm-function[N]. With island batching on by default for Minecraft that would have left the runtime
 * unprofilable exactly where profiling matters most.
 *
 * Name-map entries must be sorted by function index; 0..N-1 then N..2N-1 already is.
 */
void
wasm_module_append_name_section_multi (WasmBuf *out, const char *module_name, const char *const *names, guint32 n)
{
	WasmBuf sec, sub;
	guint32 i;

	if (!n)
		return;

	wasm_buf_init (&sec);
	wasm_name (&sec, "name");

	/* subsection 0: module name */
	wasm_buf_init (&sub);
	wasm_name (&sub, module_name ? module_name : "wasmjit-batch");
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	/* subsection 1: function names, methods then entry thunks */
	wasm_buf_init (&sub);
	wasm_uleb (&sub, n * 2);
	for (i = 0; i < n; ++i) {
		wasm_uleb (&sub, i);
		wasm_name (&sub, (names && names [i]) ? names [i] : "method");
	}
	for (i = 0; i < n; ++i) {
		char eb [512];
		g_snprintf (eb, sizeof (eb), "%s [entry]", (names && names [i]) ? names [i] : "method");
		wasm_uleb (&sub, n + i);
		wasm_name (&sub, eb);
	}
	wasm_u8 (&sec, 0x01);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	emit_section (out, 0, &sec);
	wasm_buf_free (&sec);
}

void
wasm_module_interp_thunk (
	const WasmValtype *param_types, guint32 nparams,
	WasmValtype ret_type,
	const WasmLocalGroup *locals, guint32 nlocal_groups,
	const WasmBuf *body,
	WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	WasmBuf sec;
	guint32 i;

	wasm_bytes (out, header, 8);

	/* Type section (1): T0 = thunk (params->ret), T1 = ()->i32 (mono_wasm_jit_scratch),
	 * T2 = (i32,i32)->i32 (mono_wasm_jit_call_interp). The body's two call_indirects use T1/T2. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 3);
	wasm_u8 (&sec, 0x60);
	wasm_uleb (&sec, nparams);
	for (i = 0; i < nparams; ++i)
		wasm_u8 (&sec, (guint8) param_types [i]);
	if (ret_type == WASM_VOID) {
		wasm_uleb (&sec, 0);
	} else {
		wasm_uleb (&sec, 1);
		wasm_u8 (&sec, (guint8) ret_type);
	}
	wasm_u8 (&sec, 0x60); wasm_uleb (&sec, 0); wasm_uleb (&sec, 1); wasm_u8 (&sec, (guint8) WASM_I32);                                  /* T1 ()->i32 */
	wasm_u8 (&sec, 0x60); wasm_uleb (&sec, 2); wasm_u8 (&sec, (guint8) WASM_I32); wasm_u8 (&sec, (guint8) WASM_I32);
	wasm_uleb (&sec, 1); wasm_u8 (&sec, (guint8) WASM_I32);                                                                            /* T2 (i32,i32)->i32 */
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	/* Import section (2): shared memory m.h + indirect function table f.f (table 0) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 2);
	wasm_name (&sec, "m");
	wasm_name (&sec, "h");
	wasm_u8 (&sec, 0x02);   /* memory */
	/* SHARED-ness must match the runtime heap (see the method-module import section above) */
#ifdef DISABLE_THREADS
	wasm_u8 (&sec, 0x01);   /* limits: max, unshared */
#else
	wasm_u8 (&sec, 0x03);   /* limits: shared + max */
#endif
	wasm_uleb (&sec, 256);
	wasm_uleb (&sec, 65535);
	wasm_name (&sec, "f");
	wasm_name (&sec, "f");
	wasm_u8 (&sec, 0x01);                  /* table */
	wasm_u8 (&sec, (guint8) WASM_FUNCREF);
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, 0);
	emit_section (out, 2, &sec);
	wasm_buf_free (&sec);

	/* Function section (3): one function of type 0 */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_uleb (&sec, 0);
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* Export section (7): the thunk `t` (func index 0; no func imports precede it) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_name (&sec, "t");
	wasm_u8 (&sec, 0x00);
	wasm_uleb (&sec, 0);
	emit_section (out, 7, &sec);
	wasm_buf_free (&sec);

	/* Code section (10): the thunk body */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	emit_code_entry (&sec, locals, nlocal_groups, body);
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
}
