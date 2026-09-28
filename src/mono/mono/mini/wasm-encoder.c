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
	/* Disarm the local.tee peephole. Buffers are stack-allocated at several call sites and are NOT zeroed,
	 * so leaving these as garbage could make a local.get rewrite a byte that is not a local.set. */
	b->tee_off = 0;
	b->tee_end = 0;
	b->tee_idx = 0;
	b->relocs = NULL;
	b->hints = NULL;
}

void
wasm_hint (WasmBuf *b, guint8 likely)
{
	WasmHints *h = b->hints;
	if (!h)
		h = b->hints = g_new0 (WasmHints, 1);
	/* Offsets must be strictly increasing in the section; an emitter that rolled the buffer back and re-emitted
	 * could otherwise record a smaller one. Drop any hint at or past the current end first. */
	while (h->n && h->off [h->n - 1] >= b->len)
		h->n--;
	if (h->n == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 16;
		h->off = (guint32 *) g_realloc (h->off, sizeof (guint32) * (gsize) h->cap);
		h->val = (guint8 *) g_realloc (h->val, (gsize) h->cap);
	}
	h->off [h->n] = b->len;
	h->val [h->n] = likely ? 1 : 0;
	h->n++;
}

void
wasm_buf_init_relocs (WasmBuf *b)
{
	b->relocs = g_new0 (WasmRelocs, 1);
}

void
wasm_buf_free (WasmBuf *b)
{
	g_free (b->data);
	b->data = NULL;
	b->len = b->cap = 0;
	if (b->relocs) {
		g_free (b->relocs->r);
		g_free (b->relocs);
		b->relocs = NULL;
	}
	if (b->hints) {
		g_free (b->hints->off);
		g_free (b->hints->val);
		g_free (b->hints);
		b->hints = NULL;
	}
}

void
wasm_reloc (WasmBuf *b, WasmRelocKind kind, guint32 tpool, guint32 table_index, gpointer sym)
{
	WasmRelocs *rl = b->relocs;
	WasmReloc *e;

	g_assert (rl);   /* a body that emits holes must have had wasm_buf_init_relocs called on it */
	if (rl->n == rl->cap) {
		rl->cap = rl->cap ? rl->cap * 2 : 16;
		rl->r = (WasmReloc *) g_realloc (rl->r, sizeof (WasmReloc) * (gsize) rl->cap);
	}
	e = &rl->r [rl->n++];
	e->off = b->len;
	e->kind = (guint16) kind;
	e->tpool = (guint16) tpool;
	e->table_index = table_index;
	e->sym = sym;
	/* See the header: a zero-length hole would otherwise leave the local.tee peephole armed ACROSS it. */
	b->tee_end = 0;
}

/* One resolved call site. `ti` is the module type index; `fix` says which of the three forms to use. */
static void
emit_call_site (WasmBuf *out, const WasmReloc *r, const WasmRelocFix *fix, guint32 ti)
{
	switch (fix->form) {
	case WASM_FORM_IMPORT:
	case WASM_FORM_LOCAL:
		/* Identical encoding; they differ only in which half of the function index space `idx` names,
		 * and therefore in what V8 makes of them (three loads and an indirect call, versus one
		 * `call rel32` that is also the only form V8 will inline through). */
		wasm_op (out, WASM_OP_CALL);
		wasm_uleb (out, fix->idx);
		break;
	default:
		wasm_i32_const (out, (gint32) r->table_index);
		wasm_op (out, WASM_OP_CALL_INDIRECT);
		wasm_uleb (out, ti);
		wasm_uleb (out, 0);   /* table 0 (the imported f.f) */
		break;
	}
}

static void
wasm_hint_at (WasmHints *h, guint32 off, guint8 val)
{
	if (h->n && h->off [h->n - 1] >= off)
		return;   /* keep strictly increasing */
	if (h->n == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 16;
		h->off = (guint32 *) g_realloc (h->off, sizeof (guint32) * (gsize) h->cap);
		h->val = (guint8 *) g_realloc (h->val, (gsize) h->cap);
	}
	h->off [h->n] = off;
	h->val [h->n] = val;
	h->n++;
}

void
wasm_body_serialize (const WasmBuf *src, const WasmRelocFix *fix, guint32 ti_base, WasmBuf *out)
{
	const WasmRelocs *rl = src->relocs;
	const WasmHints *hs = src->hints;
	guint32 k, prev = 0, n = rl ? rl->n : 0, hi = 0, nh = hs ? hs->n : 0, base = out->len;
	/* R339: a hint at source offset o lands at (out offset of the span holding o) + (o - span start). A hole at
	 * exactly o is inserted BEFORE the byte at o, so such a hint belongs to the span that starts at the hole. */
#define WASM_FLUSH_HINTS(LIMIT) do { \
		guint32 _span_out = out->len; \
		while (hi < nh && hs->off [hi] < (LIMIT)) { \
			if (hs->off [hi] >= prev) { \
				if (!out->hints) out->hints = g_new0 (WasmHints, 1); \
				wasm_hint_at (out->hints, _span_out - base + (hs->off [hi] - prev), hs->val [hi]); \
			} \
			hi++; \
		} \
	} while (0)

	for (k = 0; k < n; ++k) {
		const WasmReloc *r = &rl->r [k];
		guint32 ti = ti_base + r->tpool;
		g_assert (r->off >= prev && r->off <= src->len);   /* relocs are sorted and in range */
		WASM_FLUSH_HINTS (r->off);
		wasm_bytes (out, src->data + prev, r->off - prev);
		prev = r->off;
		switch ((WasmRelocKind) r->kind) {
		case WASM_RELOC_TYPE_U:
			wasm_uleb (out, ti);
			break;
		case WASM_RELOC_TYPE_S:
			wasm_sleb (out, (gint64) ti);
			break;
		case WASM_RELOC_INDIRECT:
			wasm_op (out, WASM_OP_CALL_INDIRECT);
			wasm_uleb (out, ti);
			wasm_uleb (out, 0);
			break;
		case WASM_RELOC_SELF:
			/* The entry thunk calling its own method. Always module-local: they are framed together. */
			g_assert (fix [k].form == WASM_FORM_LOCAL);
			wasm_op (out, WASM_OP_CALL);
			wasm_uleb (out, fix [k].idx);
			break;
		default:
			emit_call_site (out, r, &fix [k], ti);
			break;
		}
	}
	WASM_FLUSH_HINTS (src->len);
#undef WASM_FLUSH_HINTS
	wasm_bytes (out, src->data + prev, src->len - prev);
	/* Serializing must never disturb the peephole state of a buffer that is still being appended to. */
	out->tee_end = 0;
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
	guint32 start;

	/* `local.set N` followed immediately by `local.get N` is `local.tee N`: set pops, get pushes the same
	 * value back, tee does both. Identical semantics, 2 bytes instead of 4. The emitter produces this pair
	 * constantly because every IR temp is materialised into its own local and read straight back — a hot
	 * method's body is roughly half `local.set K; local.get K`. Rewriting in place here (the single choke
	 * point for local ops) shrinks bodies with no change to what executes.
	 *
	 * Only fires when nothing was appended between the two, so an intervening instruction — crucially a
	 * block/loop/end, which could otherwise make the get reachable without the set — disarms it. */
	if (op == WASM_OP_LOCAL_GET && b->tee_end == b->len && b->tee_end != 0 && b->tee_idx == local_idx) {
		b->data [b->tee_off] = (guint8) WASM_OP_LOCAL_TEE;
		b->tee_end = 0;
		return;
	}

	start = b->len;
	wasm_u8 (b, (guint8) op);
	wasm_uleb (b, local_idx);
	if (op == WASM_OP_LOCAL_SET) {
		b->tee_off = start;
		b->tee_idx = local_idx;
		b->tee_end = b->len;
	} else {
		b->tee_end = 0;
	}
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

/*
 * The import section, which is byte-identical in every module this backend frames. Factored out of the
 * two legacy framers so the hand-maintained import COUNT literal below exists exactly once: when it fell
 * out of step with the emits that follow it, every module was rejected with "section was shorter than
 * expected size", `registered` sat at 0 from boot, and it cost a whole measurement session to find.
 * Count the emits below before editing the literal.
 *
 * The global index space is a fixed contract with the emitter, which hard-codes `global.get/set 0..9`:
 *   0 s.p  __stack_pointer, i32 MUTABLE (the only mutable import; every framed method reads and writes it)
 *   1 s.l  &wj_slot_live          2 s.c  &wj_slot_live_cap
 *   3 s.v  &wj_vcall_pic          4 s.n  &wj_vcall_pic_cap
 *   5 s.d  &wj_delegate_pic       6 s.m  &wj_delegate_pic_cap
 *   7 s.b  the VALUE of this worker's residual scratch base (wj_scratch is a __thread ARRAY, so its
 *          address is constant for the thread's life and can be consumed directly, replacing a
 *          mono_wasm_jit_scratch() helper CALL on every residual and every vcall cold miss)
 *   8 s.i  &mono_wasm_jit_cur_island_il_state (a __thread POINTER, so its ADDRESS is constant for the
 *          thread's life). Same trick as s.b one line up, applied to the per-bb IL-offset store in
 *          eh_on methods: `global.get 8; i32.load; i32.const off; i32.store` replaces a
 *          mono_wasm_jit_set_il_offset helper CALL per basic block.
 *          NOT the same as MONO_WASM_JIT_INLINE_ILOFS, which measured 9.8% WORSE: that variant kept the
 *          il_state pointer in a wasm LOCAL, live across every call in the body. This re-derives it from
 *          the global at each store, so nothing is live between them.
 *   9 s.g  address of this worker's SAFEPOINT ACTION WORD (mini-wasm-publish.inc). Non-zero means the
 *          emitted safepoint must take its out-of-line helper: the GC wants to suspend, or a code
 *          publication has not been adopted here yet. One word rather than two conditions so the check
 *          stays a single load on every loop back-edge; the helper re-derives and clears it.
 * Every module declares all TEN whether or not it uses them, because the emitter's indices are absolute.
 *
 * Function imports come LAST in this section but FIRST in the function index space -- wasm gives imported
 * functions indices 0..nfimports-1 regardless of section order -- which is why every defined-function
 * index elsewhere is offset by nfimports.
 */
static void
emit_import_section (WasmBuf *out, gboolean import_table, gboolean import_eh_tag, guint32 eh_type_idx,
                     const WasmFuncImport *fimports, guint32 nfimports)
{
	static const char *const gnames [] = { "p", "l", "c", "v", "n", "d", "m", "b", "i", "g" };
	WasmBuf sec;
	guint32 i;

	wasm_buf_init (&sec);
	/* DERIVED from gnames, not a hand-maintained literal: when the two fell out of step every module was
	 * rejected with "section was shorter than expected size", `registered` sat at 0 from boot, and it cost
	 * a whole measurement session to find. Adding a global now needs one edit, not two. */
	wasm_uleb (&sec, (guint32) (1 /* m.h */ + (import_table ? 1 : 0) + (import_eh_tag ? 1 : 0) + (sizeof (gnames) / sizeof (gnames [0])) + nfimports));

	/* Shared heap m.h. The limits flag must match the runtime heap's SHARED-ness exactly: a threads build
	 * has a shared memory (0x03 = shared+max), a single-threaded build an unshared one (0x01 = max only);
	 * a mismatch fails instantiation with "mismatch in shared state of memory". */
	wasm_name (&sec, "m");
	wasm_name (&sec, "h");
	wasm_u8 (&sec, 0x02);
#ifdef DISABLE_THREADS
	wasm_u8 (&sec, 0x01);
#else
	wasm_u8 (&sec, 0x03);
#endif
	wasm_uleb (&sec, 256);
	wasm_uleb (&sec, 65535);   /* must be >= the runtime heap's max (65535 pages); 32768 fails to match */

	if (import_table) {
		wasm_name (&sec, "f");
		wasm_name (&sec, "f");
		wasm_u8 (&sec, 0x01);                  /* kind: table */
		wasm_u8 (&sec, (guint8) WASM_FUNCREF);
		wasm_u8 (&sec, 0x00);                  /* limits: min only (matches any actual table) */
		wasm_uleb (&sec, 0);
	}
	if (import_eh_tag) {
		wasm_name (&sec, "x");
		wasm_name (&sec, "e");
		wasm_u8 (&sec, 0x04);                  /* kind: tag */
		wasm_u8 (&sec, 0x00);                  /* attribute: exception */
		wasm_uleb (&sec, eh_type_idx);         /* functype (i32)->void */
	}
	for (i = 0; i < (sizeof (gnames) / sizeof (gnames [0])); ++i) {
		wasm_name (&sec, "s");
		wasm_name (&sec, gnames [i]);
		wasm_u8 (&sec, 0x03);                  /* kind: global */
		wasm_u8 (&sec, (guint8) WASM_I32);
		wasm_u8 (&sec, i == 0 ? 0x01 : 0x00);  /* only s.p is mutable */
	}
	/* Named by table index in decimal; the page's import resolver is a Proxy over wasmTable.get, so any
	 * table index resolves and no linker export or fixed helper registry is needed.
	 *
	 * A METHOD f-slot is named "m<index>" instead of "<index>". See WasmFuncImport.method: the resolver
	 * must refuse an f-slot this worker has not instantiated, because that slot holds a CALLABLE
	 * placeholder rather than nothing, and binding it is silent corruption. A helper index needs no such
	 * check -- it is a C function fixed for the life of the process -- and paying one on every helper
	 * import would put a lookup in front of the whole shipped default path for nothing. */
	for (i = 0; i < nfimports; ++i) {
		char idxname [16];
		g_snprintf (idxname, sizeof (idxname), fimports [i].method ? "m%u" : "%u", (unsigned) fimports [i].table_index);
		wasm_name (&sec, "h");
		wasm_name (&sec, idxname);
		wasm_u8 (&sec, 0x00);                  /* kind: function */
		wasm_uleb (&sec, fimports [i].type_idx);
	}
	emit_section (out, 2, &sec);
	wasm_buf_free (&sec);
}

/*
 * Frame N members into one module. N == 1 produces byte-for-byte what wasm_module_method_and_entry
 * produced, which is the gate the relocatable-body refactor is verified against.
 *
 * Unlike the framers it replaces, this one bakes NOTHING into the bodies: `f_body`/`e_body` arrive already
 * relocated by the caller, which is what makes re-framing a member set a byte copy rather than a re-compile.
 * The type-index base a member's body was relocated against is `ti_base` as computed here -- the running
 * sum of `ntypes` -- and the caller must have used the same value. There is no assert for it any more
 * because the caller now derives it from this same rule rather than remembering it from an earlier emit.
 *
 * Layout:
 *   types:   member blocks back to back; member i occupies [ti_base_i .. ti_base_i + ntypes_i - 1] and its
 *            block is { method_i, entry_i, extras_i... }, so types[0] and types[1] are load-bearing.
 *   funcs:   0..nfimports-1 imported, then the N methods, then the N entry thunks. Methods precede thunks
 *            so member i's method is funcidx nfimports + i.
 *   exports: first `nexport` members only; nexport == 1 -> "f"/"e" (the single-method instantiate path),
 *            nexport > 1 -> "f<i>"/"e<i>" (the batch one). Members past nexport are shadows: callable
 *            in-module, exported nowhere.
 */
void
wasm_module_assemble (const WasmAsmMember *members, guint32 nmembers, guint32 nexport,
                      gboolean import_table, gboolean import_eh_tag, guint32 eh_type_idx,
                      const WasmFuncImport *fimports, guint32 nfimports,
                      WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	static const WasmLocalGroup no_locals [1] = { { WASM_I32, 0 } };
	WasmBuf sec;
	guint32 i, j, base;

	g_assert (nmembers > 0);
	g_assert (nexport > 0 && nexport <= nmembers);

	wasm_bytes (out, header, 8);

	/* Type section (1) */
	wasm_buf_init (&sec);
	{
		guint32 ntypes = 0;
		for (i = 0; i < nmembers; ++i) {
			g_assert (members [i].ntypes >= 2);   /* method + entry are mandatory */
			ntypes += members [i].ntypes;
		}
		wasm_uleb (&sec, ntypes);
	}
	for (i = 0; i < nmembers; ++i)
		for (j = 0; j < members [i].ntypes; ++j)
			emit_functype (&sec, members [i].types [j].params, members [i].types [j].nparams,
			               members [i].types [j].ret);
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	emit_import_section (out, import_table, import_eh_tag, eh_type_idx, fimports, nfimports);

	/* Function section (3): all N methods, then all N thunks. */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, nmembers * 2);
	for (i = 0, base = 0; i < nmembers; base += members [i].ntypes, ++i)
		wasm_uleb (&sec, base);          /* method type = the member block's first entry */
	for (i = 0, base = 0; i < nmembers; base += members [i].ntypes, ++i)
		wasm_uleb (&sec, base + 1);      /* entry-thunk type = its second */
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* Export section (7) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, nexport * 2);
	for (i = 0; i < nexport; ++i) {
		char nm [24];
		if (nexport == 1) { nm [0] = 'f'; nm [1] = 0; }
		else g_snprintf (nm, sizeof (nm), "f%u", i);
		wasm_name (&sec, nm);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, nfimports + i);
	}
	for (i = 0; i < nexport; ++i) {
		char nm [24];
		if (nexport == 1) { nm [0] = 'e'; nm [1] = 0; }
		else g_snprintf (nm, sizeof (nm), "e%u", i);
		wasm_name (&sec, nm);
		wasm_u8 (&sec, 0x00);
		wasm_uleb (&sec, nfimports + nmembers + i);
	}
	emit_section (out, 7, &sec);
	wasm_buf_free (&sec);

	/* R339: branch hints (custom section "metadata.code.branch_hint"), before the code section. Offsets are relative
	 * to the start of each function's LOCAL DECLARATIONS (V8: pc_offset - locals_offset_), so add that length. */
	{
		guint32 nf = 0;
		for (i = 0; i < nmembers; ++i)
			if (members [i].f_body->hints && members [i].f_body->hints->n)
				nf++;
		if (nf) {
			WasmBuf hsec;
			wasm_buf_init (&hsec);
			wasm_name (&hsec, "metadata.code.branch_hint");
			wasm_uleb (&hsec, nf);
			for (i = 0; i < nmembers; ++i) {
				const WasmHints *h = members [i].f_body->hints;
				WasmBuf ld;
				guint32 j, g, ngroups = 0;
				if (!h || !h->n)
					continue;
				wasm_buf_init (&ld);
				for (g = 0; g < members [i].nlocal_groups; ++g)
					if (members [i].locals [g].count > 0)
						ngroups++;
				wasm_uleb (&ld, ngroups);
				for (g = 0; g < members [i].nlocal_groups; ++g) {
					if (members [i].locals [g].count == 0)
						continue;
					wasm_uleb (&ld, members [i].locals [g].count);
					wasm_u8 (&ld, (guint8) members [i].locals [g].type);
				}
				wasm_uleb (&hsec, nfimports + i);   /* methods precede the thunks */
				wasm_uleb (&hsec, h->n);
				for (j = 0; j < h->n; ++j) {
					wasm_uleb (&hsec, ld.len + h->off [j]);
					wasm_uleb (&hsec, 1);
					wasm_u8 (&hsec, h->val [j]);
				}
				wasm_buf_free (&ld);
			}
			emit_section (out, 0, &hsec);
			wasm_buf_free (&hsec);
		}
	}

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

/*
 * A LAZY STUB BANK (MONO_WASM_JIT_LAZY_T1, mini-wasm-lazy.inc): k functions of the one functype `ft`, stub i
 * written into table slot f_slots [i] by an ACTIVE element segment -- so instantiating the bank on a worker is
 * exactly what makes those f-slots callable there, and nothing else ever has to install a stub.
 *
 * The four function imports are runtime helpers named by table index, like every JIT module's:
 *   [0] bind   (i32 slot)->i32     the f-slot to tail-call through, or 0 = run the callee in the interpreter
 *   [1] scratch ()->i32            THIS pthread's scratch (never s.b: a bank outlives the pthread that made it, R293)
 *   [2] interp (i32 slot, i32 buf)->i32   1 if the callee threw
 *   [3] continue_unwind ()->void   never returns
 * Of the ten s.* globals only s.p (0) is used; all ten are declared because emit_import_section is the one
 * definition of that section. Stub body, n params, locals old/sp/r at n, n+1, n+2:
 *   frame: every i32 param stored in a C-stack frame, i.e. under sgen's conservative stack scan, because bind
 *          can compile, instantiate, take GC-safe transitions and run a cctor -- all of which can collect
 *   r = bind (N); if r { pop; return_call_indirect ft (params..., r) }
 *   r = scratch (); params -> r + 8*j; if interp (N, r) { continue_unwind; unreachable }
 *   [ret = load r + 192]; pop
 */
void
wasm_module_lazy_bank (const WasmFuncType *ft, const int *f_slots, guint32 k, const guint32 *helper_idx, WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	WasmFuncImport fim [4];
	WasmValtype p1 [1] = { WASM_I32 }, p2 [2] = { WASM_I32, WASM_I32 };
	WasmLocalGroup lg [1] = { { WASM_I32, 3 } };
	WasmBuf sec;
	guint32 i, j, n = ft->nparams, ni32 = 0, frame;
	guint32 l_old = n, l_sp = n + 1, l_r = n + 2;

	for (j = 0; j < n; ++j)
		if (ft->params [j] == WASM_I32)
			ni32++;
	frame = (ni32 * 4 + 15) & ~15u;

	wasm_bytes (out, header, 8);
	/* types: 0 = the stub, 1..4 = the helpers in import order */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 5);
	emit_functype (&sec, ft->params, ft->nparams, ft->ret);
	emit_functype (&sec, p1, 1, WASM_I32);
	emit_functype (&sec, NULL, 0, WASM_I32);
	emit_functype (&sec, p2, 2, WASM_I32);
	emit_functype (&sec, NULL, 0, WASM_VOID);
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	for (i = 0; i < 4; ++i) {
		fim [i].table_index = helper_idx [i];
		fim [i].type_idx = 1 + i;
		fim [i].method = 0;
	}
	emit_import_section (out, TRUE, FALSE, 0, fim, 4);

	wasm_buf_init (&sec);
	wasm_uleb (&sec, k);
	for (i = 0; i < k; ++i)
		wasm_uleb (&sec, 0);
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* element: one active segment per stub, table 0 (the imported f.f), offset = its f-slot */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, k);
	for (i = 0; i < k; ++i) {
		wasm_uleb (&sec, 0);                     /* flags 0: active, table 0, funcidx vector */
		wasm_op (&sec, WASM_OP_I32_CONST); wasm_sleb (&sec, f_slots [i]); wasm_op (&sec, WASM_OP_END);
		wasm_uleb (&sec, 1);
		wasm_uleb (&sec, 4 + i);                 /* after the four imported helpers */
	}
	emit_section (out, 9, &sec);
	wasm_buf_free (&sec);

	wasm_buf_init (&sec);
	wasm_uleb (&sec, k);
	for (i = 0; i < k; ++i) {
		WasmBuf body;
		guint32 kth = 0;
		wasm_buf_init (&body);
		if (frame) {
			wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0);
			wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_old);
			wasm_i32_const (&body, (gint32) frame);
			wasm_op (&body, WASM_OP_I32_SUB);
			wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_sp);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
			for (j = 0; j < n; ++j) {
				if (ft->params [j] != WASM_I32)
					continue;
				wasm_op_local (&body, WASM_OP_LOCAL_GET, l_sp);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, j);
				wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 4 * kth++);
			}
		}
		wasm_i32_const (&body, f_slots [i]);
		wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_r);
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		if (frame) {
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_old);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
		}
		for (j = 0; j < n; ++j)
			wasm_op_local (&body, WASM_OP_LOCAL_GET, j);
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
		wasm_u8 (&body, 0x13);                  /* return_call_indirect */
		wasm_uleb (&body, 0);                    /* type 0 */
		wasm_uleb (&body, 0);                    /* table 0 */
		wasm_op (&body, WASM_OP_END);
		wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 1);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, l_r);
		for (j = 0; j < n; ++j) {
			WasmOpcode sop;
			guint32 al;
			switch (ft->params [j]) {
			case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
			case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
			case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
			default:       sop = WASM_OP_I32_STORE; al = 2; break;
			}
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, j);
			wasm_op (&body, sop); wasm_memarg (&body, al, 8 * j);
		}
		wasm_i32_const (&body, f_slots [i]);
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
		wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 2);
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 3);
		wasm_op (&body, WASM_OP_UNREACHABLE);
		wasm_op (&body, WASM_OP_END);
		if (ft->ret != WASM_VOID) {
			WasmOpcode lop;
			guint32 al;
			switch (ft->ret) {
			case WASM_I64: lop = WASM_OP_I64_LOAD; al = 3; break;
			case WASM_F32: lop = WASM_OP_F32_LOAD; al = 2; break;
			case WASM_F64: lop = WASM_OP_F64_LOAD; al = 3; break;
			default:       lop = WASM_OP_I32_LOAD; al = 2; break;
			}
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
			wasm_op (&body, lop); wasm_memarg (&body, al, 192);   /* WJ_SCRATCH_RET_OFF */
		}
		if (frame) {
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_old);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
		}
		emit_code_entry (&sec, lg, 1, &body);
		wasm_buf_free (&body);
	}
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
}

void
wasm_module_append_name_section (WasmBuf *out, const char *module_name, const char *func0_name, guint32 nfimports)
{
	/* Append a custom "name" section (id 0) so V8 prints real Mono method names in wasm stack
	 * traces (devtools, Error.stack, CDP) instead of anonymous wasm-function[N]. Custom sections
	 * may appear after the code section. Subsection 0 = module name; subsection 1 = function names.
	 *
	 * The method `f` and the entry thunk `e` are the module's DEFINED functions, so their indices sit
	 * above every imported function -- `nfimports` of them, from the direct-call helper imports. Naming
	 * indices 0 and 1 unconditionally (which this did while function imports did not exist) labels the
	 * IMPORTS instead and leaves the real bodies anonymous.
	 *
	 * That is not a cosmetic bug. perf resolves JIT'd wasm frames through these names, so getting it wrong
	 * silently moved ~45% of in-game samples into 4,250 `wasm-function[N]` symbols and made whole method
	 * families (`__<>MHC*` among them) read as 0% of the profile -- an apparent optimisation win that was
	 * only a lost name. CDP is unaffected because it reports the module name from subsection 0, which is
	 * why the tier snapshots still showed correct method names and the two tools disagreed. */
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

	/* subsection 1: function names. Indices are nfimports + {0,1}: the defined method and entry thunk sit
	 * above the imported helpers. Name-map entries must be sorted by index, which they are. */
	wasm_buf_init (&sub);
	wasm_uleb (&sub, 2);                   /* two name-map entries */
	wasm_uleb (&sub, nfimports + 0);       /* the method `f` */
	wasm_name (&sub, f0);
	wasm_uleb (&sub, nfimports + 1);       /* the entry thunk `e` */
	wasm_name (&sub, "entry");
	wasm_u8 (&sec, 0x01);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	emit_section (out, 0, &sec);
	wasm_buf_free (&sec);
}

/*
 * Name section for a module with more than one member.
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
wasm_module_append_name_section_multi (WasmBuf *out, const char *module_name, const char *const *names, guint32 n, guint32 nfimports)
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
	/* nfimports offset: names index the FUNCTION index space, where imports occupy 0..nfimports-1. Without
	 * this every batched name would be attached to the wrong function once a batch declares imports, which
	 * silently mislabels profiles rather than failing. */
	for (i = 0; i < n; ++i) {
		wasm_uleb (&sub, nfimports + i);
		wasm_name (&sub, (names && names [i]) ? names [i] : "method");
	}
	for (i = 0; i < n; ++i) {
		char eb [512];
		g_snprintf (eb, sizeof (eb), "%s [entry]", (names && names [i]) ? names [i] : "method");
		wasm_uleb (&sub, nfimports + n + i);
		wasm_name (&sub, eb);
	}
	wasm_u8 (&sec, 0x01);
	wasm_uleb (&sec, sub.len);
	wasm_bytes (&sec, sub.data, sub.len);
	wasm_buf_free (&sub);

	emit_section (out, 0, &sec);
	wasm_buf_free (&sec);
}
