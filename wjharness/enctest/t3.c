/* wasm_body_serialize round trip: a body emitted with holes and serialized with every call resolved to
 * INDIRECT must equal the same body emitted the old way, with the type indices baked at ti_base. */
#include "shim.h"
#include "wasm-encoder.h"

static unsigned rs = 4242;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }

int main (void) {
	int iter, bad = 0, holes_total = 0;
	for (iter = 0; iter < 500; ++iter) {
		WasmBuf hole, baked, out;
		WasmRelocFix fix [512];
		guint32 ti_base = rnd (300), nops, i, k, nh = 0;

		wasm_buf_init (&hole); wasm_buf_init_relocs (&hole);
		wasm_buf_init (&baked);

		nops = 1 + rnd (120);
		for (i = 0; i < nops; ++i) {
			guint32 tp = rnd (40);
			switch (rnd (8)) {
			case 0: case 1:   /* filler that also exercises the tee peephole */
				k = rnd (30);
				wasm_op_local (&hole, WASM_OP_LOCAL_SET, k); wasm_op_local (&baked, WASM_OP_LOCAL_SET, k);
				break;
			case 2:
				k = rnd (30);
				wasm_op_local (&hole, WASM_OP_LOCAL_GET, k); wasm_op_local (&baked, WASM_OP_LOCAL_GET, k);
				break;
			case 3: {         /* helper call, resolved to INDIRECT */
				guint32 addr = 30000 + rnd (400000);
				wasm_reloc (&hole, WASM_RELOC_HELPER, tp, addr, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				wasm_i32_const (&baked, (gint32) addr);
				wasm_op (&baked, WASM_OP_CALL_INDIRECT); wasm_uleb (&baked, ti_base + tp); wasm_uleb (&baked, 0);
				break;
			}
			case 4: {         /* method call, resolved to INDIRECT */
				guint32 fs = 500000 + rnd (90000);
				wasm_reloc (&hole, WASM_RELOC_CALL, tp, fs, (gpointer) (gsize) (iter + 1));
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				wasm_i32_const (&baked, (gint32) fs);
				wasm_op (&baked, WASM_OP_CALL_INDIRECT); wasm_uleb (&baked, ti_base + tp); wasm_uleb (&baked, 0);
				break;
			}
			case 5:           /* dynamic-index call_indirect */
				wasm_reloc (&hole, WASM_RELOC_INDIRECT, tp, 0, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				wasm_op (&baked, WASM_OP_CALL_INDIRECT); wasm_uleb (&baked, ti_base + tp); wasm_uleb (&baked, 0);
				break;
			case 6:           /* multi-value blocktype (SLEB) */
				wasm_op (&hole, WASM_OP_IF);
				wasm_reloc (&hole, WASM_RELOC_TYPE_S, tp, 0, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				wasm_op (&baked, WASM_OP_IF); wasm_sleb (&baked, (gint64) (ti_base + tp));
				break;
			default: {
				gint32 c = (gint32) rnd (100000);
				wasm_i32_const (&hole, c); wasm_i32_const (&baked, c);
				break;
			}
			}
			if (nh >= 500) break;
		}
		holes_total += nh;

		wasm_buf_init (&out);
		wasm_body_serialize (&hole, fix, ti_base, &out);
		if (out.len != baked.len || memcmp (out.data, baked.data, out.len)) {
			printf ("MISMATCH iter=%d len %u vs %u (holes=%u)\n", iter, out.len, baked.len, nh);
			for (i = 0; i < (out.len < baked.len ? out.len : baked.len); ++i)
				if (out.data [i] != baked.data [i]) { printf ("  first diff byte %u: %02x vs %02x\n", i, out.data [i], baked.data [i]); break; }
			bad++;
		}
		wasm_buf_free (&out); wasm_buf_free (&hole); wasm_buf_free (&baked);
	}
	printf (bad ? "FAIL: %d mismatches\n" : "PASS: serializer round-trips (%d holes)\n", bad ? bad : holes_total);
	return bad != 0;
}
