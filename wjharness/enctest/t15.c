/* t15: module-defined globals (WASM_RELOC_GLOBAL, WasmAsmMember.nglobals; MONO_WASM_JIT_T1_GLOBALS). Frames three
 * members -- two globals, none, one -- whose bodies reach their globals only through GLOBAL holes, resolved the way
 * wj_asm_resolve_body does (WASM_IMPORTED_GLOBALS + the member's base + its own number), into t15.wasm; and member 1
 * alone into t15-none.wasm. t15.mjs validates both and EXECUTES t15.wasm: each member's globals are its own, they
 * persist across calls, and a second instance starts from 0. t15-none.wasm must carry no global section (a module
 * without globals is byte-identical to one framed before they existed). */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>
#include <string.h>

static void
gop (WasmBuf *b, guint8 op, guint32 g)
{
	wasm_op (b, op);
	wasm_reloc (b, WASM_RELOC_GLOBAL, 0, g, NULL);
}

static void
resolve (const WasmBuf *b, WasmRelocFix *fix, guint32 gbase)
{
	guint32 k;
	for (k = 0; b->relocs && k < b->relocs->n; ++k) {
		memset (&fix [k], 0, sizeof (fix [k]));
		if (b->relocs->r [k].kind == WASM_RELOC_GLOBAL)
			fix [k].idx = WASM_IMPORTED_GLOBALS + gbase + b->relocs->r [k].table_index;
	}
}

int main (void) {
	WasmFuncType t [2];
	WasmLocalGroup groups [4];
	WasmBuf src [3], ser [3], e [3], out;
	WasmAsmMember m [3];
	WasmRelocFix fix [16];
	const guint32 ng [3] = { 2, 0, 1 };
	guint32 gbase = 0;
	FILE *f;
	int i, j;
	memset (t, 0, sizeof (t)); memset (groups, 0, sizeof (groups)); memset (m, 0, sizeof (m));
	for (j = 0; j < 4; ++j) { groups [j].type = WASM_I32; groups [j].count = 0; }
	t [0].nparams = 1; t [0].params [0] = WASM_I32; t [0].ret = WASM_I32;
	t [1].nparams = 2; t [1].params [0] = WASM_I32; t [1].params [1] = WASM_I32; t [1].ret = WASM_VOID;

	for (i = 0; i < 3; ++i) { wasm_buf_init (&src [i]); wasm_buf_init_relocs (&src [i]); }
	/* 0: g0 += x; g1 += 1; return g0 * 1000 + g1 */
	gop (&src [0], WASM_OP_GLOBAL_GET, 0); wasm_op_local (&src [0], WASM_OP_LOCAL_GET, 0); wasm_op (&src [0], WASM_OP_I32_ADD);
	gop (&src [0], WASM_OP_GLOBAL_SET, 0);
	gop (&src [0], WASM_OP_GLOBAL_GET, 1); wasm_i32_const (&src [0], 1); wasm_op (&src [0], WASM_OP_I32_ADD);
	gop (&src [0], WASM_OP_GLOBAL_SET, 1);
	gop (&src [0], WASM_OP_GLOBAL_GET, 0); wasm_i32_const (&src [0], 1000); wasm_u8 (&src [0], 0x6c); /* i32.mul */
	gop (&src [0], WASM_OP_GLOBAL_GET, 1); wasm_op (&src [0], WASM_OP_I32_ADD);
	/* 1: no globals: x * 2 */
	wasm_op_local (&src [1], WASM_OP_LOCAL_GET, 0); wasm_i32_const (&src [1], 2); wasm_u8 (&src [1], 0x6c);
	/* 2: g0 += x; return g0 */
	gop (&src [2], WASM_OP_GLOBAL_GET, 0); wasm_op_local (&src [2], WASM_OP_LOCAL_GET, 0); wasm_op (&src [2], WASM_OP_I32_ADD);
	gop (&src [2], WASM_OP_GLOBAL_SET, 0);
	gop (&src [2], WASM_OP_GLOBAL_GET, 0);

	for (i = 0; i < 3; ++i) {
		resolve (&src [i], fix, gbase);
		wasm_buf_init (&ser [i]);
		wasm_body_serialize (&src [i], fix, 0, &ser [i]);
		gbase += ng [i];
		wasm_buf_init (&e [i]); wasm_op (&e [i], WASM_OP_NOP);
		m [i].locals = groups; m [i].nlocal_groups = 4; m [i].f_body = &ser [i]; m [i].e_body = &e [i];
		m [i].types = t; m [i].ntypes = 2; m [i].nglobals = ng [i];
	}
	wasm_buf_init (&out);
	wasm_module_assemble (m, 3, 3, FALSE, FALSE, 0, NULL, 0, &out);
	f = fopen ("t15.wasm", "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
	wasm_buf_free (&out);
	wasm_buf_init (&out);
	wasm_module_assemble (&m [1], 1, 1, FALSE, FALSE, 0, NULL, 0, &out);
	f = fopen ("t15-none.wasm", "wb"); fwrite (out.data, 1, out.len, f); fclose (f);
	wasm_buf_free (&out);
	for (i = 0; i < 3; ++i) { wasm_buf_free (&src [i]); wasm_buf_free (&ser [i]); wasm_buf_free (&e [i]); }
	printf ("t15: module-defined globals framed (3 members: 2, 0, 1 globals)\n");
	return 0;
}
