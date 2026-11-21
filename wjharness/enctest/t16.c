/* t16: the image (plan abstract-hatching-stearns G; WASM_RELOC_IMG_GATE / WASM_RELOC_IMG_INDIRECT,
 * wasm_module_assemble_ex's image_n). Three members: 0 dispatches `gate ? call_indirect <(i32)->i32> TBL (x, i) : -1`,
 * 1 is x + 100, 2 is x * 3. Framed as the image (gate 1, table 1 defined, entry 1 + i = member i) into t16.wasm, and
 * member 0 alone with the gate 0 and table 0 (a non-image module: the call is dead code) into t16-off.wasm. t16.mjs
 * validates both and EXECUTES them: the image dispatches through its own table (index 2 -> member 1, 3 -> member 2),
 * index 0 (null) and index 1 (member 0, another type) trap, and the non-image module answers -1 and has neither a table
 * nor an element section. */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>
#include <string.h>

static void
resolve (const WasmBuf *b, WasmRelocFix *fix, int image)
{
	guint32 k;
	for (k = 0; b->relocs && k < b->relocs->n; ++k) {
		memset (&fix [k], 0, sizeof (fix [k]));
		if (b->relocs->r [k].kind == WASM_RELOC_IMG_GATE || b->relocs->r [k].kind == WASM_RELOC_IMG_INDIRECT)
			fix [k].idx = image ? 1 : 0;
	}
}

static void
frame (int image, const char *fn)
{
	WasmFuncType t0 [3], t1 [2];
	WasmLocalGroup groups [4];
	WasmBuf src [3], ser [3], e [3], out;
	WasmAsmMember m [3];
	WasmRelocFix fix [16];
	guint32 tib = 0;
	int i, j, n = image ? 3 : 1;
	FILE *f;
	memset (t0, 0, sizeof (t0)); memset (t1, 0, sizeof (t1)); memset (groups, 0, sizeof (groups)); memset (m, 0, sizeof (m));
	for (j = 0; j < 4; ++j) { groups [j].type = WASM_I32; groups [j].count = 0; }
	/* member 0: (i32 x, i32 i) -> i32, its thunk, and the callees' (i32) -> i32 as an extra type */
	t0 [0].nparams = 2; t0 [0].params [0] = WASM_I32; t0 [0].params [1] = WASM_I32; t0 [0].ret = WASM_I32;
	t0 [1].nparams = 2; t0 [1].params [0] = WASM_I32; t0 [1].params [1] = WASM_I32; t0 [1].ret = WASM_VOID;
	t0 [2].nparams = 1; t0 [2].params [0] = WASM_I32; t0 [2].ret = WASM_I32;
	/* members 1, 2: (i32) -> i32 and the thunk */
	t1 [0].nparams = 1; t1 [0].params [0] = WASM_I32; t1 [0].ret = WASM_I32;
	t1 [1].nparams = 2; t1 [1].params [0] = WASM_I32; t1 [1].params [1] = WASM_I32; t1 [1].ret = WASM_VOID;

	for (i = 0; i < 3; ++i) { wasm_buf_init (&src [i]); wasm_buf_init_relocs (&src [i]); }
	wasm_reloc (&src [0], WASM_RELOC_IMG_GATE, 0, 0, NULL);
	wasm_op (&src [0], WASM_OP_IF); wasm_u8 (&src [0], 0x7f);
	  wasm_op_local (&src [0], WASM_OP_LOCAL_GET, 0);
	  wasm_op_local (&src [0], WASM_OP_LOCAL_GET, 1);
	  wasm_reloc (&src [0], WASM_RELOC_IMG_INDIRECT, 2, 0, NULL);   /* tpool 2: the extra (i32) -> i32 */
	wasm_op (&src [0], WASM_OP_ELSE);
	  wasm_i32_const (&src [0], -1);
	wasm_op (&src [0], WASM_OP_END);
	wasm_op_local (&src [1], WASM_OP_LOCAL_GET, 0); wasm_i32_const (&src [1], 100); wasm_op (&src [1], WASM_OP_I32_ADD);
	wasm_op_local (&src [2], WASM_OP_LOCAL_GET, 0); wasm_i32_const (&src [2], 3); wasm_u8 (&src [2], 0x6c);   /* i32.mul */

	for (i = 0; i < n; ++i) {
		resolve (&src [i], fix, image);
		wasm_buf_init (&ser [i]);
		wasm_body_serialize (&src [i], fix, tib, &ser [i]);
		wasm_buf_init (&e [i]); wasm_op (&e [i], WASM_OP_NOP);
		m [i].locals = groups; m [i].nlocal_groups = 4; m [i].f_body = &ser [i]; m [i].e_body = &e [i];
		m [i].types = i == 0 ? t0 : t1; m [i].ntypes = i == 0 ? 3 : 2;
		tib += m [i].ntypes;
	}
	wasm_buf_init (&out);
	wasm_module_assemble_ex (m, (guint32) n, (guint32) n, TRUE, FALSE, 0, NULL, 0, image ? (guint32) n : 0, &out);
	f = fopen (fn, "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
	wasm_buf_free (&out);
	for (i = 0; i < n; ++i) { wasm_buf_free (&ser [i]); wasm_buf_free (&e [i]); }
	for (i = 0; i < 3; ++i) wasm_buf_free (&src [i]);
}

int main (void) {
	frame (1, "t16.wasm");
	frame (0, "t16-off.wasm");
	printf ("t16: the image framed (3 members, table 1) and member 0 alone (gate 0)\n");
	return 0;
}
