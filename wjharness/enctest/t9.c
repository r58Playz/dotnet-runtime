/* t9: origin map (MONO_WASM_JIT_ORIGIN, the gap ledger). A body emitted with relocation HOLES and origin
 * transitions -- including a push immediately before a hole and a pop immediately after it, i.e. two transitions
 * at the SAME source offset on opposite sides of a zero-width hole -- is serialized with random call forms. The
 * oracle is the SAME body emitted with every hole's bytes baked inline (emit_call_site's encoding, replicated
 * here): its transition list needs no translation, so the serialized list must equal it exactly. Then the framed
 * module with origins must equal the module without them plus one trailing "wj.origin" section, whose offsets
 * (locals-relative) must name the same bytes. Negative control: a hole-side swap of one transition must be
 * detected. */
#include "shim.h"
#include "wasm-encoder.h"
#include <string.h>

static unsigned rs = 9091;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static guint32 rd_uleb (const guint8 *p, guint32 *off) {
	guint32 r = 0, sh = 0; guint8 b;
	do { b = p [(*off)++]; r |= (guint32) (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return r;
}
static gint32 rd_sleb (const guint8 *p, guint32 *off) {
	gint32 r = 0; guint32 sh = 0; guint8 b;
	do { b = p [(*off)++]; r |= (gint32) (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	if (sh < 32 && (b & 0x40)) r |= -(1 << sh);
	return r;
}

#define TI_BASE 3

/* Emit into `h` (with holes) and `k` (baked) in lockstep. */
static void build (WasmBuf *h, WasmBuf *k, WasmRelocFix *fix, guint32 *nfix) {
	guint32 i, nops = 1 + rnd (90);
	wasm_buf_init (h); wasm_buf_init_relocs (h); wasm_origins_init (h);
	wasm_buf_init (k); wasm_origins_init (k);
	*nfix = 0;
	for (i = 0; i < nops; ++i) {
		guint32 tp = rnd (6);
		switch (rnd (8)) {
		case 0: case 1: {   /* a call hole wrapped in push/pop, exactly like wj_emit_*_call */
			guint16 ph = wasm_origin_push (h, 7), pk = wasm_origin_push (k, 7);
			unsigned f = rnd (3);
			guint32 tidx = 500000 + rnd (9000), idx = rnd (300);
			wasm_reloc (h, WASM_RELOC_CALL, tp, tidx, (gpointer) (gsize) 1);
			fix [*nfix].form = f == 0 ? WASM_FORM_INDIRECT : f == 1 ? WASM_FORM_IMPORT : WASM_FORM_LOCAL;
			fix [*nfix].idx = idx; (*nfix)++;
			if (f == 0) { wasm_i32_const (k, (gint32) tidx); wasm_op (k, WASM_OP_CALL_INDIRECT); wasm_uleb (k, TI_BASE + tp); wasm_uleb (k, 0); }
			else { wasm_op (k, WASM_OP_CALL); wasm_uleb (k, idx); }
			wasm_origin_pop (h, ph); wasm_origin_pop (k, pk);
			break;
		}
		case 2: {   /* a dynamic call_indirect hole, wrapped */
			guint16 ph = wasm_origin_push (h, 8), pk = wasm_origin_push (k, 8);
			wasm_reloc (h, WASM_RELOC_INDIRECT, tp, 0, NULL);
			fix [*nfix].form = WASM_FORM_INDIRECT; fix [*nfix].idx = 0; (*nfix)++;
			wasm_op (k, WASM_OP_CALL_INDIRECT); wasm_uleb (k, TI_BASE + tp); wasm_uleb (k, 0);
			wasm_origin_pop (h, ph); wasm_origin_pop (k, pk);
			break;
		}
		case 3: {   /* an un-wrapped hole: its bytes belong to whatever tag is current */
			wasm_reloc (h, WASM_RELOC_INDIRECT, tp, 0, NULL);
			fix [*nfix].form = WASM_FORM_INDIRECT; fix [*nfix].idx = 0; (*nfix)++;
			wasm_op (k, WASM_OP_CALL_INDIRECT); wasm_uleb (k, TI_BASE + tp); wasm_uleb (k, 0);
			break;
		}
		case 4: {   /* an instruction boundary: new opcode tag, method, IL offset */
			guint16 t = (guint16) (WASM_ORIGIN_OP_BASE + rnd (900)), m = (guint16) rnd (4);
			gint32 il = (gint32) rnd (5000) - 1;
			guint16 op = (guint16) rnd (256);
			wasm_origin_set (h, t, m, il, op); wasm_origin_set (k, t, m, il, op);
			break;
		}
		case 5: {   /* a nested construct with ordinary bytes inside */
			guint16 t = (guint16) (1 + rnd (30));
			guint16 ph = wasm_origin_push (h, t), pk = wasm_origin_push (k, t);
			gint32 c = (gint32) rnd (100000);
			wasm_i32_const (h, c); wasm_op (h, WASM_OP_DROP);
			wasm_i32_const (k, c); wasm_op (k, WASM_OP_DROP);
			wasm_origin_pop (h, ph); wasm_origin_pop (k, pk);
			break;
		}
		default: {
			gint32 c = (gint32) rnd (100000);
			wasm_i32_const (h, c); wasm_op (h, WASM_OP_DROP);
			wasm_i32_const (k, c); wasm_op (k, WASM_OP_DROP);
			break;
		}
		}
	}
}

static int same_list (const WasmOrigins *a, const WasmOrigins *b) {
	guint32 j;
	if (!a || !b || a->n != b->n) return 0;
	for (j = 0; j < a->n; ++j) {
		const WasmOrigin *x = &a->v [j], *y = &b->v [j];
		if (x->off != y->off || x->tag != y->tag || x->meth != y->meth || x->il != y->il || x->ilop != y->ilop)
			return 0;
	}
	return 1;
}

int main (void) {
	int iter, bad = 0, ctl_caught = 0, ctl_tried = 0, trans_total = 0, mod_checked = 0;
	for (iter = 0; iter < 400; ++iter) {
		WasmBuf h, k, out, eb, mod1, mod2;
		WasmRelocFix fix [256];
		guint32 nfix, j;
		WasmLocalGroup groups [4];
		WasmFuncType types [2];
		WasmAsmMember m;
		WasmOrigins *saved;
		guint32 nfimp = rnd (4);

		build (&h, &k, fix, &nfix);
		wasm_buf_init (&out);
		wasm_body_serialize (&h, fix, TI_BASE, &out);
		if (out.len != k.len || memcmp (out.data, k.data, k.len)) { printf ("iter %d: serialized bytes != baked bytes (oracle broken)\n", iter); bad++; goto next; }
		if (!same_list (out.origins, k.origins)) {
			printf ("iter %d: origin lists differ (%u vs %u)\n", iter, out.origins ? out.origins->n : 0, k.origins->n);
			for (j = 0; out.origins && j < out.origins->n && j < k.origins->n; ++j)
				if (out.origins->v [j].off != k.origins->v [j].off || out.origins->v [j].tag != k.origins->v [j].tag) {
					printf ("  first diff at %u: got off %u tag %u, want off %u tag %u\n", j, out.origins->v [j].off, out.origins->v [j].tag, k.origins->v [j].off, k.origins->v [j].tag);
					break;
				}
			bad++; goto next;
		}
		trans_total += (int) k.origins->n;
		/* negative control: move one transition that sits right after a wrapped hole to the hole's start; the
		 * comparison must notice (only meaningful where the hole has nonzero output length, i.e. always). */
		for (j = 1; j < out.origins->n; ++j) {
			if (out.origins->v [j].tag != 7 && out.origins->v [j - 1].tag == 7 && out.origins->v [j].off > out.origins->v [j - 1].off) {
				guint32 keep = out.origins->v [j].off;
				ctl_tried = 1;
				out.origins->v [j].off = out.origins->v [j - 1].off;
				if (!same_list (out.origins, k.origins)) ctl_caught = 1;
				out.origins->v [j].off = keep;
				break;
			}
		}

		/* frame with and without the origins: identical except the trailing custom section */
		for (j = 0; j < 4; ++j) { groups [j].type = WASM_I32; groups [j].count = rnd (70); }
		types [0].nparams = 0; types [0].ret = WASM_VOID;
		types [1].nparams = 2; types [1].params [0] = WASM_I32; types [1].params [1] = WASM_I32; types [1].ret = WASM_VOID;
		{
			WasmFuncImport fimp [4];
			for (j = 0; j < nfimp; ++j) { fimp [j].table_index = 40000 + j; fimp [j].type_idx = 0; fimp [j].method = 0; }
			wasm_buf_init (&eb); wasm_op (&eb, WASM_OP_NOP);
			m.locals = groups; m.nlocal_groups = 4; m.f_body = &out; m.e_body = &eb; m.types = types; m.ntypes = 2;
			wasm_buf_init (&mod1); wasm_buf_init (&mod2);
			wasm_module_assemble (&m, 1, 1, FALSE, FALSE, 0, fimp, nfimp, &mod1);
			saved = out.origins; out.origins = NULL;
			wasm_module_assemble (&m, 1, 1, FALSE, FALSE, 0, fimp, nfimp, &mod2);
			out.origins = saved;
		}
		if (k.origins->n == 0) {   /* no transitions -> no section: the two modules must be identical */
			if (mod1.len != mod2.len || memcmp (mod1.data, mod2.data, mod2.len)) { printf ("iter %d: empty origins changed the module\n", iter); bad++; }
			goto fr;
		}
		if (mod1.len <= mod2.len || memcmp (mod1.data, mod2.data, mod2.len)) { printf ("iter %d: module without origins is not a prefix\n", iter); bad++; goto fr; }
		{
			/* locate function 0's body start in mod2's code section, then read the trailing section */
			guint32 off = 8, bstart = 0;
			while (off < mod2.len) {
				guint32 id = mod2.data [off++], len = rd_uleb (mod2.data, &off), end = off + len;
				if (id == 10) { guint32 p = off; rd_uleb (mod2.data, &p); rd_uleb (mod2.data, &p); bstart = p; }
				off = end;
			}
			off = mod2.len;
			if (mod1.data [off++] != 0) { printf ("iter %d: trailing section not custom\n", iter); bad++; goto fr; }
			{
				guint32 len = rd_uleb (mod1.data, &off), end = off + len, nl = rd_uleb (mod1.data, &off);
				guint32 ver, nf, fi, ll, nm, n, q, locals_len;
				if (nl != 9 || memcmp (mod1.data + off, "wj.origin", 9)) { printf ("iter %d: wrong section name\n", iter); bad++; goto fr; }
				off += nl;
				ver = rd_uleb (mod1.data, &off); nf = rd_uleb (mod1.data, &off);
				fi = rd_uleb (mod1.data, &off); ll = rd_uleb (mod1.data, &off); nm = rd_uleb (mod1.data, &off);
				(void) nm;
				n = rd_uleb (mod1.data, &off);
				/* locals_len must equal the local-decl bytes at bstart, i.e. the instructions start there */
				{ guint32 p = bstart, ng = rd_uleb (mod2.data, &p), g; for (g = 0; g < ng; ++g) { rd_uleb (mod2.data, &p); p++; } locals_len = p - bstart; }
				if (ver != 1 || nf != 1 || fi != nfimp || ll != locals_len || n != k.origins->n) {
					printf ("iter %d: header ver %u nf %u fi %u(want %u) ll %u(want %u) n %u(want %u)\n", iter, ver, nf, fi, nfimp, ll, locals_len, n, k.origins->n); bad++; goto fr;
				}
				for (q = 0; q < n; ++q) {
					guint32 o = rd_uleb (mod1.data, &off), t = rd_uleb (mod1.data, &off), me = rd_uleb (mod1.data, &off);
					gint32 il = rd_sleb (mod1.data, &off);
					guint32 op = rd_uleb (mod1.data, &off);
					const WasmOrigin *w = &k.origins->v [q];
					if (o != ll + w->off || t != w->tag || me != w->meth || il != w->il || op != w->ilop) {
						printf ("iter %d: section entry %u: off %u tag %u il %d, want %u %u %d\n", iter, q, o, t, il, ll + w->off, w->tag, w->il); bad++; break;
					}
					/* the offset names a real instruction byte in mod2 (or the body end) */
					if (bstart + o > mod2.len) { printf ("iter %d: offset past module\n", iter); bad++; break; }
				}
				if (off != end) { printf ("iter %d: section length mismatch\n", iter); bad++; }
				mod_checked++;
			}
		}
	fr:
		wasm_buf_free (&mod1); wasm_buf_free (&mod2); wasm_buf_free (&eb);
	next:
		wasm_buf_free (&h); wasm_buf_free (&k); wasm_buf_free (&out);
	}
	if (ctl_tried && !ctl_caught) { printf ("FAIL: negative control (hole-side swap) went undetected\n"); bad++; }
	if (!ctl_tried) { printf ("FAIL: negative control never ran\n"); bad++; }
	if (bad) { printf ("FAIL: %d origin-map mismatches\n", bad); return 1; }
	printf ("PASS: %d origin transitions carried through serialization exactly; %d modules' wj.origin sections verified and code sections unchanged\n", trans_total, mod_checked);
	return 0;
}
