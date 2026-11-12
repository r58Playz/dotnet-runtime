/* t7: lazy stub banks (Phase 5, MONO_WASM_JIT_LAZY_T1). Frames wasm_module_lazy_bank for a spread of functypes and
 * writes each to t7-<n>.wasm; t7.mjs then validates and EXECUTES them in node: bind returning a slot must tail-call
 * that slot with the stub's own arguments, bind returning 0 must spill the arguments at scratch + 8*j, call interp
 * with (slot, scratch) and return what it left at scratch + 192; a throw must reach continue_unwind. The stub's
 * i32 params must be visible in its C-stack frame (below s.p) while bind runs -- that is the GC pin. */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>
#include <string.h>

int main (void) {
	static const WasmValtype vt [4] = { WASM_I32, WASM_I64, WASM_F32, WASM_F64 };
	unsigned rs = 12345, t;
	FILE *man = fopen ("t7-manifest.txt", "w");
	for (t = 0; t < 40; ++t) {
		WasmFuncType ft;
		WasmBuf out;
		int slots [8];
		guint32 helpers [4] = { 1, 2, 3, 4 };   /* table indices of bind, scratch, interp, unwind in t7.mjs */
		guint32 i, k = 1 + t % 8;
		char fn [64];
		FILE *f;
		memset (&ft, 0, sizeof (ft));
		ft.nparams = t == 0 ? 0 : t == 1 ? WASM_FUNCTYPE_MAX_PARAMS : (rs = rs * 1103515245u + 12345u, (rs >> 16) % 9);
		for (i = 0; i < ft.nparams; ++i) { rs = rs * 1103515245u + 12345u; ft.params [i] = vt [(rs >> 16) % 4]; }
		rs = rs * 1103515245u + 12345u;
		ft.ret = ((rs >> 16) % 5) == 4 ? WASM_VOID : vt [(rs >> 16) % 4];
		for (i = 0; i < k; ++i) slots [i] = 16 + 2 * i;   /* f-slots 16, 18, ... in t7.mjs's table */
		wasm_buf_init (&out);
		wasm_module_lazy_bank (&ft, slots, k, helpers, &out);
		snprintf (fn, sizeof (fn), "t7-%u.wasm", t);
		f = fopen (fn, "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
		fprintf (man, "%s %u %u", fn, k, (unsigned) ft.ret);
		for (i = 0; i < ft.nparams; ++i) fprintf (man, " %u", (unsigned) ft.params [i]);
		fprintf (man, "\n");
		wasm_buf_free (&out);
	}
	fclose (man);
	printf ("t7: 40 banks framed\n");
	/* MONO_WASM_JIT_SP_ARG: the same spread with a trailing i32 frame base in the functype, framed in modes 1 and 2.
	 * t7.mjs runs them with s.p stale (mode 2) and checks the pin frame sits below the BASE, the base reaches the
	 * tail-called target as its last argument and is not spilled for the interpreter, and s.p is the base afterwards. */
	man = fopen ("t7sp-manifest.txt", "w");
	rs = 54321;
	for (t = 0; t < 40; ++t) {
		WasmFuncType ft;
		WasmBuf out;
		int slots [8];
		guint32 helpers [4] = { 1, 2, 3, 4 };
		guint32 i, k = 1 + t % 8;
		char fn [64];
		FILE *f;
		memset (&ft, 0, sizeof (ft));
		ft.nparams = t < 2 ? 0 : t < 4 ? WASM_FUNCTYPE_MAX_PARAMS - 1 : (rs = rs * 1103515245u + 12345u, (rs >> 16) % 9);
		for (i = 0; i < ft.nparams; ++i) { rs = rs * 1103515245u + 12345u; ft.params [i] = t >= 4 && t < 8 ? vt [1 + (rs >> 16) % 3] : vt [(rs >> 16) % 4]; }
		ft.params [ft.nparams++] = WASM_I32;   /* the frame base */
		rs = rs * 1103515245u + 12345u;
		ft.ret = ((rs >> 16) % 5) == 4 ? WASM_VOID : vt [(rs >> 16) % 4];
		for (i = 0; i < k; ++i) slots [i] = 16 + 2 * i;
		wasm_sp_arg = 1 + (int) (t & 1);
		wasm_buf_init (&out);
		wasm_module_lazy_bank (&ft, slots, k, helpers, &out);
		snprintf (fn, sizeof (fn), "t7sp-%u.wasm", t);
		f = fopen (fn, "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
		fprintf (man, "%s %d %u %u", fn, wasm_sp_arg, k, (unsigned) ft.ret);
		for (i = 0; i < ft.nparams; ++i) fprintf (man, " %u", (unsigned) ft.params [i]);
		fprintf (man, "\n");
		wasm_buf_free (&out);
	}
	wasm_sp_arg = 0;
	fclose (man);
	printf ("t7: 40 banks framed with a frame-base parameter (MONO_WASM_JIT_SP_ARG 1 and 2)\n");
	return 0;
}
