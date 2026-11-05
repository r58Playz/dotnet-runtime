/* t6: branch hints (R339). A body emitted with relocation HOLES and hints on `if` / `br_if` opcodes, serialized with
 * every kind of hole resolved to a random form (the forms differ in length), must carry each hint onto the SAME
 * opcode byte in the output -- including a hint recorded at exactly a hole's offset, whose inserted bytes come first.
 * Then, framed by wasm_module_assemble, the "metadata.code.branch_hint" section must precede the code section and
 * each (funcidx, offset) must name that opcode relative to the function's local declarations. t1-t5 never record a
 * hint, so they cannot see any of this. Carries a negative control: an off-by-one offset must be detected. */
#include "shim.h"
#include "wasm-encoder.h"
#include <string.h>

static unsigned rs = 777;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static guint32 rd_uleb (const guint8 *p, guint32 *off) {
	guint32 r = 0, sh = 0; guint8 b;
	do { b = p [(*off)++]; r |= (guint32) (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return r;
}

/* Build one body with holes and hints; returns the number of hints and their opcodes in `ops`. */
static guint32 build (WasmBuf *b, WasmRelocFix *fix, guint32 *nfix, guint8 *ops, guint8 *vals) {
	guint32 i, nops = 1 + rnd (80), nh = 0;
	wasm_buf_init (b); wasm_buf_init_relocs (b);
	*nfix = 0;
	for (i = 0; i < nops; ++i) {
		guint32 tp = rnd (6);
		switch (rnd (9)) {
		case 0: {   /* helper hole, random form */
			wasm_reloc (b, WASM_RELOC_HELPER, tp, 30000 + rnd (40000), NULL);
			fix [*nfix].form = rnd (2) ? WASM_FORM_IMPORT : WASM_FORM_INDIRECT; fix [*nfix].idx = rnd (20); (*nfix)++;
			break;
		}
		case 1: {   /* method call hole: indirect, import or module-local */
			unsigned f = rnd (3);
			wasm_reloc (b, WASM_RELOC_CALL, tp, 500000 + rnd (9000), (gpointer) (gsize) 1);
			fix [*nfix].form = f == 0 ? WASM_FORM_INDIRECT : f == 1 ? WASM_FORM_IMPORT : WASM_FORM_LOCAL;
			fix [*nfix].idx = rnd (300); (*nfix)++;
			break;
		}
		case 2:     /* dynamic call_indirect hole */
			wasm_reloc (b, WASM_RELOC_INDIRECT, tp, 0, NULL);
			fix [*nfix].form = WASM_FORM_INDIRECT; fix [*nfix].idx = 0; (*nfix)++;
			break;
		case 3: case 4: {   /* a hinted if / br_if */
			guint8 v = (guint8) rnd (2), op = rnd (2) ? WASM_OP_IF : WASM_OP_BR_IF;
			wasm_hint (b, v);
			wasm_op (b, op);
			if (op == WASM_OP_IF) wasm_u8 (b, 0x40); else wasm_uleb (b, rnd (5));
			ops [nh] = op; vals [nh] = v; nh++;
			break;
		}
		case 5: {   /* a hole at exactly the hint's offset: reloc, then hint, then the opcode */
			guint8 v = (guint8) rnd (2);
			wasm_reloc (b, WASM_RELOC_CALL, tp, 500000 + rnd (9000), (gpointer) (gsize) 1);
			fix [*nfix].form = rnd (2) ? WASM_FORM_INDIRECT : WASM_FORM_LOCAL; fix [*nfix].idx = rnd (300); (*nfix)++;
			wasm_hint (b, v);
			wasm_op (b, WASM_OP_BR_IF); wasm_uleb (b, 0);
			ops [nh] = WASM_OP_BR_IF; vals [nh] = v; nh++;
			break;
		}
		default:
			wasm_i32_const (b, (gint32) rnd (100000));
			wasm_op (b, WASM_OP_DROP);
			break;
		}
	}
	return nh;
}

int main (void) {
	int iter, bad = 0, ctl_caught = 0, hints_total = 0, mod_checked = 0;
	for (iter = 0; iter < 300; ++iter) {
		WasmBuf src, out, eb, mod;
		WasmRelocFix fix [256];
		guint8 ops [256], vals [256];
		guint32 nfix, nh, j;
		WasmLocalGroup groups [4];
		WasmFuncType types [2];
		WasmAsmMember m;
		WasmFuncImport fimp [4];
		guint32 nfimp = rnd (4), off = 8, hint_func = 0, hint_n = 0, code_seen = 0, hint_sec_seen = 0;
		guint32 hint_offs [256]; guint8 hint_vals [256];

		nh = build (&src, fix, &nfix, ops, vals);
		wasm_buf_init (&out);
		wasm_body_serialize (&src, fix, 3, &out);
		/* serialized hints: same count, same values, each on its opcode */
		if ((out.hints ? out.hints->n : 0) != nh) { printf ("iter %d: %u hints in, %u out\n", iter, nh, out.hints ? out.hints->n : 0); bad++; goto next; }
		for (j = 0; j < nh; ++j) {
			guint32 o = out.hints->off [j];
			if (o >= out.len || out.data [o] != ops [j] || out.hints->val [j] != vals [j]) {
				printf ("iter %d hint %u: off %u byte %02x want %02x\n", iter, j, o, o < out.len ? out.data [o] : 0, ops [j]); bad++; goto next;
			}
			/* negative control: the byte one past is never the same opcode kind here often enough to be useful, so
			 * instead check that a deliberately shifted offset would NOT satisfy the test for at least one hint */
			if (o + 1 < out.len && out.data [o + 1] != ops [j])
				ctl_caught = 1;
		}
		hints_total += (int) nh;

		/* frame it and read the section back */
		for (j = 0; j < 4; ++j) { groups [j].type = WASM_I32; groups [j].count = rnd (70); }
		types [0].nparams = 0; types [0].ret = WASM_VOID;
		types [1].nparams = 2; types [1].params [0] = WASM_I32; types [1].params [1] = WASM_I32; types [1].ret = WASM_VOID;
		for (j = 0; j < nfimp; ++j) { fimp [j].table_index = 40000 + j; fimp [j].type_idx = 0; fimp [j].method = 0; }
		wasm_buf_init (&eb); wasm_op (&eb, WASM_OP_NOP);
		m.locals = groups; m.nlocal_groups = 4; m.f_body = &out; m.e_body = &eb; m.types = types; m.ntypes = 2;
		wasm_buf_init (&mod);
		wasm_module_assemble (&m, 1, 1, FALSE, FALSE, 0, fimp, nfimp, &mod);
		while (off < mod.len) {
			guint32 id = mod.data [off++], len = rd_uleb (mod.data, &off), end = off + len;
			if (id == 0) {
				guint32 p = off, nl = rd_uleb (mod.data, &p);
				if (nl == 25 && !memcmp (mod.data + p, "metadata.code.branch_hint", 25)) {
					guint32 nf, k;
					p += nl;
					if (code_seen) { printf ("iter %d: hint section after code\n", iter); bad++; }
					hint_sec_seen = 1;
					nf = rd_uleb (mod.data, &p);
					for (k = 0; k < nf; ++k) {
						guint32 fi = rd_uleb (mod.data, &p), cnt = rd_uleb (mod.data, &p), q;
						hint_func = fi; hint_n = cnt;
						for (q = 0; q < cnt; ++q) {
							hint_offs [q] = rd_uleb (mod.data, &p);
							if (rd_uleb (mod.data, &p) != 1) { printf ("iter %d: data size != 1\n", iter); bad++; }
							hint_vals [q] = mod.data [p++];
						}
					}
				}
			} else if (id == 10) {
				guint32 p = off, nbodies = rd_uleb (mod.data, &p), bsize, bstart, q;
				code_seen = 1;
				(void) nbodies;
				bsize = rd_uleb (mod.data, &p);          /* function 0 of the defined ones = the method */
				bstart = p;                              /* V8's locals_offset_: first byte of the local decls */
				(void) bsize;
				if (nh && hint_sec_seen) {
					if (hint_func != nfimp || hint_n != nh) { printf ("iter %d: func %u n %u, want %u %u\n", iter, hint_func, hint_n, nfimp, nh); bad++; }
					for (q = 0; q < hint_n && q < nh; ++q) {
						if (mod.data [bstart + hint_offs [q]] != ops [q] || hint_vals [q] != vals [q]) {
							printf ("iter %d: module hint %u at +%u is %02x want %02x\n", iter, q, hint_offs [q], mod.data [bstart + hint_offs [q]], ops [q]); bad++; break;
						}
					}
					mod_checked++;
				}
			}
			off = end;
		}
		if (nh && !hint_sec_seen) { printf ("iter %d: no hint section\n", iter); bad++; }
		wasm_buf_free (&mod); wasm_buf_free (&eb);
	next:
		wasm_buf_free (&src); wasm_buf_free (&out);
	}
	if (!ctl_caught) { printf ("FAIL: negative control never distinguished a shifted offset\n"); bad++; }
	if (bad) { printf ("FAIL: %d branch-hint mismatches\n", bad); return 1; }
	printf ("PASS: %d hints carried through serialization; %d modules' hint sections verified against the code section\n", hints_total, mod_checked);
	return 0;
}
