/* Byte-identity gate: wasm_module_assemble(n==1) must equal wasm_module_method_and_entry. */
#include "shim.h"
#include "wasm-encoder.h"
#include "legacy.h"

static unsigned rs = 12345;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }

static WasmValtype vt (void) {
	static const WasmValtype t [4] = { WASM_I32, WASM_I64, WASM_F32, WASM_F64 };
	return t [rnd (4)];
}

int main (void) {
	int iter, bad = 0;
	for (iter = 0; iter < 400; ++iter) {
		WasmValtype params [WASM_FUNCTYPE_MAX_PARAMS], ret;
		WasmFuncType extra [24], types [26];
		WasmLocalGroup groups [4];
		WasmFuncImport fimp [8];
		WasmBuf fb, eb, a, b;
		WasmAsmMember m;
		guint32 nparams = rnd (WASM_FUNCTYPE_MAX_PARAMS + 1), nextra = rnd (24), nfimp = rnd (8);
		guint32 i, j;
		gboolean tbl = rnd (2), tag = rnd (2);
		guint32 ehti = nextra ? rnd (nextra) + 2 : 0;

		for (i = 0; i < nparams; ++i) params [i] = vt ();
		ret = rnd (5) ? vt () : WASM_VOID;
		for (i = 0; i < nextra; ++i) {
			extra [i].nparams = rnd (5);
			for (j = 0; j < extra [i].nparams; ++j) extra [i].params [j] = vt ();
			extra [i].ret = rnd (2) ? vt () : WASM_VOID;
		}
		for (i = 0; i < 4; ++i) { groups [i].type = vt (); groups [i].count = rnd (6); }
		for (i = 0; i < nfimp; ++i) { fimp [i].table_index = 40000 + rnd (9999); fimp [i].type_idx = rnd (2 + nextra); }

		wasm_buf_init (&fb); wasm_buf_init (&eb);
		for (i = 0, j = rnd (60); i < j; ++i) { wasm_op (&fb, WASM_OP_NOP); }
		for (i = 0, j = rnd (20); i < j; ++i) { wasm_op (&eb, WASM_OP_NOP); }

		wasm_buf_init (&a);
		wasm_module_method_and_entry (params, nparams, ret, groups, 4, &fb, &eb,
		                              extra, nextra, tbl, tag, ehti, fimp, nfimp, &a);

		/* the same thing, as a 1-member assemble: types[0]=method, types[1]=entry, then the extras */
		types [0].nparams = nparams;
		for (i = 0; i < nparams; ++i) types [0].params [i] = params [i];
		types [0].ret = ret;
		types [1].nparams = 2; types [1].params [0] = WASM_I32; types [1].params [1] = WASM_I32;
		types [1].ret = WASM_VOID;
		for (i = 0; i < nextra; ++i) types [2 + i] = extra [i];
		m.locals = groups; m.nlocal_groups = 4; m.f_body = &fb; m.e_body = &eb;
		m.types = types; m.ntypes = 2 + nextra;

		wasm_buf_init (&b);
		wasm_module_assemble (&m, 1, 1, tbl, tag, ehti, fimp, nfimp, &b);

		if (a.len != b.len || memcmp (a.data, b.data, a.len)) {
			printf ("MISMATCH iter=%d len %u vs %u\n", iter, a.len, b.len);
			for (i = 0; i < (a.len < b.len ? a.len : b.len); ++i)
				if (a.data [i] != b.data [i]) { printf ("  first diff at byte %u: %02x vs %02x\n", i, a.data [i], b.data [i]); break; }
			bad++;
		}
		wasm_buf_free (&a); wasm_buf_free (&b); wasm_buf_free (&fb); wasm_buf_free (&eb);
	}
	printf (bad ? "FAIL: %d mismatches\n" : "PASS: 400/400 byte-identical\n", bad);
	return bad != 0;
}
