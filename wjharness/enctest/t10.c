/* t10: the guarded module-local call (plan2x S1, WASM_FORM_LOCAL_GUARDED). Frames a caller and its co-located
 * callee ("the old copy") with the call between them guarded on a word in linear memory, for three signatures
 * (scalar i32, mixed i64/f64 -> f64, and void with a memory side effect), and writes t10-<n>.wasm + a manifest.
 * t10.mjs then validates and EXECUTES them in node: with the word 0 the caller must reach the old copy through
 * `call <funcidx>`; with the word set it must reach whatever the TABLE holds at the callee's f-slot (the "new
 * body"), and the taken arm must bump the counter word. That is the property R337's stale instances need.
 * Variant 3 (R452, B4 leaf copies) frames the callee as a PRIVATE member (nexport 1 < nmembers 2): the same guarded
 * call must reach the unexported copy with the word clear and the table with it set, and the copy must not be exported. */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>
#include <string.h>

#define GUARD 4096
#define COUNT 4100
#define FSLOT 40

static void
frame (int which, const char *fn, int nexport)
{
	WasmFuncType tA [3], tB [2];
	WasmLocalGroup groups [4];
	WasmBuf fa, fa_out, fb, ea, eb, out;
	WasmAsmMember nm [2];
	WasmRelocFix fix [1];
	FILE *f;
	int j;
	memset (tA, 0, sizeof (tA)); memset (tB, 0, sizeof (tB)); memset (groups, 0, sizeof (groups));
	for (j = 0; j < 4; ++j) { groups [j].type = WASM_I32; groups [j].count = 0; }
	/* the callee's functype, shared by member A's call and member B's own signature */
	if (which == 0) { tA [0].nparams = 1; tA [0].params [0] = WASM_I32; tA [0].ret = WASM_I32; }
	else if (which == 1) { tA [0].nparams = 2; tA [0].params [0] = WASM_I64; tA [0].params [1] = WASM_F64; tA [0].ret = WASM_F64; }
	else { tA [0].nparams = 1; tA [0].params [0] = WASM_I32; tA [0].ret = WASM_VOID; }
	tA [1].nparams = 2; tA [1].params [0] = WASM_I32; tA [1].params [1] = WASM_I32; tA [1].ret = WASM_VOID;
	tA [2] = tA [0];
	tB [0] = tA [0]; tB [1] = tA [1];

	/* A: pass its params straight to B through a CALL hole (tpool 2 = the callee type) */
	wasm_buf_init (&fa); wasm_buf_init_relocs (&fa);
	for (j = 0; j < (int) tA [0].nparams; ++j) wasm_op_local (&fa, WASM_OP_LOCAL_GET, (guint32) j);
	wasm_reloc (&fa, WASM_RELOC_CALL, 2, FSLOT, (gpointer) (gsize) 1);
	memset (fix, 0, sizeof (fix));
	fix [0].form = WASM_FORM_LOCAL_GUARDED; fix [0].idx = 1; fix [0].guard = GUARD; fix [0].guard_count = COUNT;
	wasm_buf_init (&fa_out);
	wasm_body_serialize (&fa, fix, 0, &fa_out);

	/* B, the old copy: i32 -> x + 1 ; (i64, f64) -> f64(x) + y ; i32 -> store x at 2048 */
	wasm_buf_init (&fb);
	if (which == 0) { wasm_op_local (&fb, WASM_OP_LOCAL_GET, 0); wasm_i32_const (&fb, 1); wasm_op (&fb, WASM_OP_I32_ADD); }
	else if (which == 1) { wasm_op_local (&fb, WASM_OP_LOCAL_GET, 0); wasm_u8 (&fb, 0xB9); /* f64.convert_i64_s */
		wasm_op_local (&fb, WASM_OP_LOCAL_GET, 1); wasm_u8 (&fb, 0xA0); /* f64.add */ }
	else { wasm_i32_const (&fb, 2048); wasm_op_local (&fb, WASM_OP_LOCAL_GET, 0); wasm_op (&fb, WASM_OP_I32_STORE); wasm_memarg (&fb, 2, 0); }
	wasm_buf_init (&ea); wasm_op (&ea, WASM_OP_NOP);
	wasm_buf_init (&eb); wasm_op (&eb, WASM_OP_NOP);

	nm [0].locals = groups; nm [0].nlocal_groups = 4; nm [0].f_body = &fa_out; nm [0].e_body = &ea; nm [0].types = tA; nm [0].ntypes = 3;
	nm [1].locals = groups; nm [1].nlocal_groups = 4; nm [1].f_body = &fb; nm [1].e_body = &eb; nm [1].types = tB; nm [1].ntypes = 2;
	wasm_buf_init (&out);
	wasm_module_assemble (nm, 2, (guint32) nexport, TRUE, FALSE, 0, NULL, 0, &out);
	f = fopen (fn, "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
	wasm_buf_free (&out); wasm_buf_free (&fa); wasm_buf_free (&fa_out); wasm_buf_free (&fb); wasm_buf_free (&ea); wasm_buf_free (&eb);
}

int main (void) {
	FILE *man = fopen ("t10-manifest.txt", "w");
	int t;
	for (t = 0; t < 4; ++t) {
		char fn [32];
		int which = t < 3 ? t : 0, nexport = t < 3 ? 2 : 1;
		snprintf (fn, sizeof (fn), "t10-%d.wasm", t);
		frame (which, fn, nexport);
		fprintf (man, "%s %d %d %d %d %d\n", fn, which, GUARD, COUNT, FSLOT, nexport);
	}
	fclose (man);
	printf ("t10: 4 guarded modules framed (one with a private callee)\n");
	return 0;
}
