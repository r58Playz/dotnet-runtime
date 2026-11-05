/* Same gate for N > 1: wasm_module_assemble must equal wasm_module_methods_and_entries. */
#include "shim.h"
#include "wasm-encoder.h"
#include "legacy.h"

static unsigned rs = 777;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static WasmValtype vt (void) { static const WasmValtype t [4] = { WASM_I32, WASM_I64, WASM_F32, WASM_F64 }; return t [rnd (4)]; }

#define MAXM 12
int main (void) {
	int iter, bad = 0;
	for (iter = 0; iter < 300; ++iter) {
		WasmModuleMember om [MAXM];
		WasmAsmMember nm [MAXM];
		WasmValtype params [MAXM][WASM_FUNCTYPE_MAX_PARAMS];
		WasmValtype rets [MAXM];
		WasmFuncType extra [MAXM][16], types [MAXM][18];
		WasmLocalGroup groups [MAXM][4];
		WasmBuf fb [MAXM], eb [MAXM], a, b;
		WasmFuncImport fimp [8];
		guint32 n = 2 + rnd (MAXM - 1), nfimp = rnd (8), base = 0, i, j, k;
		gboolean tbl = rnd (2), tag = rnd (2);
		guint32 ehti = 2;

		for (i = 0; i < nfimp; ++i) { fimp [i].table_index = 40000 + rnd (9999); fimp [i].type_idx = 0; }
		for (i = 0; i < n; ++i) {
			guint32 np = rnd (WASM_FUNCTYPE_MAX_PARAMS + 1), ne = rnd (16);
			for (j = 0; j < np; ++j) params [i][j] = vt ();
			rets [i] = rnd (5) ? vt () : WASM_VOID;
			for (j = 0; j < ne; ++j) {
				extra [i][j].nparams = rnd (5);
				for (k = 0; k < extra [i][j].nparams; ++k) extra [i][j].params [k] = vt ();
				extra [i][j].ret = rnd (2) ? vt () : WASM_VOID;
			}
			for (j = 0; j < 4; ++j) { groups [i][j].type = vt (); groups [i][j].count = rnd (6); }
			wasm_buf_init (&fb [i]); wasm_buf_init (&eb [i]);
			for (j = 0, k = rnd (40); j < k; ++j) wasm_op (&fb [i], WASM_OP_NOP);
			for (j = 0, k = rnd (10); j < k; ++j) wasm_op (&eb [i], WASM_OP_NOP);

			om [i].param_types = params [i]; om [i].nparams = np; om [i].ret_type = rets [i];
			om [i].locals = groups [i]; om [i].nlocal_groups = 4;
			om [i].f_body = &fb [i]; om [i].e_body = &eb [i];
			om [i].extra_types = extra [i]; om [i].nextra = ne; om [i].ti_base = base;
			base += 2 + ne;

			types [i][0].nparams = np;
			for (j = 0; j < np; ++j) types [i][0].params [j] = params [i][j];
			types [i][0].ret = rets [i];
			types [i][1].nparams = 2; types [i][1].params [0] = WASM_I32; types [i][1].params [1] = WASM_I32;
			types [i][1].ret = WASM_VOID;
			for (j = 0; j < ne; ++j) types [i][2 + j] = extra [i][j];
			nm [i].locals = groups [i]; nm [i].nlocal_groups = 4;
			nm [i].f_body = &fb [i]; nm [i].e_body = &eb [i];
			nm [i].types = types [i]; nm [i].ntypes = 2 + ne;
		}

		wasm_buf_init (&a);
		wasm_module_methods_and_entries (om, n, tbl, tag, ehti, fimp, nfimp, &a);
		wasm_buf_init (&b);
		wasm_module_assemble (nm, n, n, tbl, tag, ehti, fimp, nfimp, &b);

		if (a.len != b.len || memcmp (a.data, b.data, a.len)) {
			printf ("MISMATCH iter=%d n=%u len %u vs %u\n", iter, n, a.len, b.len);
			for (i = 0; i < (a.len < b.len ? a.len : b.len); ++i)
				if (a.data [i] != b.data [i]) { printf ("  first diff at byte %u: %02x vs %02x\n", i, a.data [i], b.data [i]); break; }
			bad++;
		}
		wasm_buf_free (&a); wasm_buf_free (&b);
		for (i = 0; i < n; ++i) { wasm_buf_free (&fb [i]); wasm_buf_free (&eb [i]); }
	}
	printf (bad ? "FAIL: %d mismatches\n" : "PASS: 300/300 byte-identical (N>1)\n", bad);
	return bad != 0;
}
