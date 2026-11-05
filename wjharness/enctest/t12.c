/* R516: the serializer's call-site table (WasmBuf.sites), which the managed stack walk maps V8 frame positions through.
 * Every call opcode written at a hole must be recorded -- at the offset of that opcode, carrying the tag the reloc was
 * recorded under -- and nothing else. Random bodies over every call form (INDIRECT, IMPORT, LOCAL, the GUARDED pair, the
 * entry thunk's SELF call, dynamic-index call_indirect) mixed with holes that write no call (type immediates). */
#include "shim.h"
#include "wasm-encoder.h"

static unsigned rs = 1516;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }

int main (void) {
	int iter, bad = 0, sites_total = 0;
	for (iter = 0; iter < 500; ++iter) {
		WasmBuf hole, out;
		WasmRelocFix fix [512];
		gint32 want_il [1024];
		guint16 want_inl [1024];
		guint32 nops, i, nh = 0, nwant = 0;

		wasm_buf_init (&hole); wasm_buf_init_relocs (&hole);
		nops = 1 + rnd (120);
		for (i = 0; i < nops && nh < 500; ++i) {
			guint32 tp = rnd (40);
			gint32 il = (gint32) rnd (5000) - 1;
			guint16 inl = (guint16) rnd (6);
			wasm_site_set (&hole, inl, il);
			switch (rnd (9)) {
			case 0: case 1: {   /* method call: any of the four forms */
				int form = (int) rnd (4);
				wasm_reloc (&hole, WASM_RELOC_CALL, tp, 500000 + rnd (90000), (gpointer) (gsize) (iter + 1));
				fix [nh].form = form == 0 ? WASM_FORM_INDIRECT : form == 1 ? WASM_FORM_IMPORT : form == 2 ? WASM_FORM_LOCAL : WASM_FORM_LOCAL_GUARDED;
				fix [nh].idx = rnd (200);
				fix [nh].guard = 4096 + 4 * rnd (100);
				fix [nh].guard_count = rnd (2) ? 8192 : 0;
				nh++;
				want_il [nwant] = il; want_inl [nwant++] = inl;
				if (form == 3) { want_il [nwant] = il; want_inl [nwant++] = inl; }   /* the guarded pair: two calls */
				break;
			}
			case 2:             /* helper */
				wasm_reloc (&hole, WASM_RELOC_HELPER, tp, 30000 + rnd (400000), NULL);
				fix [nh].form = rnd (2) ? WASM_FORM_INDIRECT : WASM_FORM_IMPORT; fix [nh].idx = rnd (30); nh++;
				want_il [nwant] = il; want_inl [nwant++] = inl;
				break;
			case 3:             /* dynamic-index call_indirect */
				wasm_reloc (&hole, WASM_RELOC_INDIRECT, tp, 0, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				want_il [nwant] = il; want_inl [nwant++] = inl;
				break;
			case 4:             /* the entry thunk's own call */
				wasm_reloc (&hole, WASM_RELOC_SELF, tp, 0, NULL);
				fix [nh].form = WASM_FORM_LOCAL; fix [nh].idx = rnd (50); nh++;
				want_il [nwant] = il; want_inl [nwant++] = inl;
				break;
			case 5:             /* a hole that writes no call */
				wasm_op (&hole, WASM_OP_IF);
				wasm_reloc (&hole, rnd (2) ? WASM_RELOC_TYPE_S : WASM_RELOC_TYPE_U, tp, 0, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				break;
			default:
				wasm_i32_const (&hole, (gint32) rnd (100000));
				break;
			}
		}
		wasm_buf_init (&out);
		wasm_body_serialize (&hole, fix, rnd (300), &out);
		{
			guint32 n = out.sites ? out.sites->n : 0, k;
			if (n != nwant) {
				printf ("iter %d: %u sites recorded, %u calls written\n", iter, n, nwant);
				bad++;
			}
			for (k = 0; k < n && k < nwant; ++k) {
				guint32 o = out.sites->off [k];
				if (o >= out.len || (out.data [o] != WASM_OP_CALL && out.data [o] != WASM_OP_CALL_INDIRECT) ||
				    (k && o <= out.sites->off [k - 1]) || out.sites->il [k] != want_il [k] || out.sites->inl [k] != want_inl [k]) {
					printf ("iter %d site %u: off %u byte %02x il %d/%d inl %u/%u\n", iter, k, o, o < out.len ? out.data [o] : 0xff,
						out.sites->il [k], want_il [k], out.sites->inl [k], want_inl [k]);
					bad++;
					break;
				}
			}
			sites_total += (int) n;
		}
		wasm_buf_free (&out); wasm_buf_free (&hole);
	}
	printf (bad ? "FAIL: %d bad bodies\n" : "PASS: %d call sites recorded at their call opcodes with their tags\n", bad ? bad : sites_total);
	return bad != 0;
}
