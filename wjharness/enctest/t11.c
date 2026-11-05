/* t11: compact virtual call sites' slow-path stub (plan typed-spindle C1, wasm_module_vslow). Frames the stub for a
 * spread of functypes -- param 0 is always the i32 receiver -- and every return normalisation, writes each to
 * t11-<n>.wasm; t11.mjs validates and EXECUTES them in node through every route: the inline AOT IC (both kinds),
 * resolve -> tail call (with the i32 params pinned below s.p while resolve runs), the AOT target resolver (both kinds),
 * the interpreter residual with a sub-word return normalised, a throw reaching continue_unwind, and a site with no AOT
 * IC. The stub's type must end in an i64 (the placeholder-collision rule), which t11.mjs also checks. */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>
#include <string.h>

int main (void) {
	static const WasmValtype vt [4] = { WASM_I32, WASM_I64, WASM_F32, WASM_F64 };
	unsigned rs = 777, t;
	FILE *man = fopen ("t11-manifest.txt", "w");
	for (t = 0; t < 48; ++t) {
		WasmFuncType ft;
		WasmBuf out;
		guint32 helpers [5] = { 1, 2, 3, 4, 5 };   /* resolve, scratch, aot, interp, unwind in t11.mjs's table */
		guint32 i;
		int retnorm;
		char fn [64];
		FILE *f;
		memset (&ft, 0, sizeof (ft));
		ft.nparams = t == 0 ? 1 : t == 1 ? WASM_FUNCTYPE_MAX_PARAMS - 1 : (rs = rs * 1103515245u + 12345u, 1 + (rs >> 16) % 8);
		ft.params [0] = WASM_I32;
		for (i = 1; i < ft.nparams; ++i) { rs = rs * 1103515245u + 12345u; ft.params [i] = vt [(rs >> 16) % 4]; }
		rs = rs * 1103515245u + 12345u;
		ft.ret = ((rs >> 16) % 5) == 4 ? WASM_VOID : vt [(rs >> 16) % 4];
		retnorm = ft.ret == WASM_I32 ? (int) (t % 5) : 0;
		wasm_buf_init (&out);
		wasm_module_vslow (&ft, retnorm, 16, helpers, &out);   /* the stub's own slot is 16 */
		snprintf (fn, sizeof (fn), "t11-%u.wasm", t);
		f = fopen (fn, "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
		fprintf (man, "%s %d %u", fn, retnorm, (unsigned) ft.ret);
		for (i = 0; i < ft.nparams; ++i) fprintf (man, " %u", (unsigned) ft.params [i]);
		fprintf (man, "\n");
		wasm_buf_free (&out);
	}
	fclose (man);
	printf ("t11: 48 stubs framed\n");
	return 0;
}
