/* t14: the exnref EH region -- wasm_eh_region_open / wasm_eh_region_catch / wasm_eh_throw_payload, the bytes
 * mini-wasm-emitter.inc frames an EH method's dispatch with, around a stand-in "bb" and the emitter's landing-pad
 * shape (payload -> dispatch -> h < 0 ? throw it on : $blk = h, br $outer). Writes the function's code entry to
 * t14.code; t14.mjs wraps it in a module, validates it and EXECUTES it: a caught x.e re-dispatches through $outer
 * with its payload (twice, so the try_table is re-entered), an unclaimed one leaves carrying the SAME payload, and a
 * foreign (JS) exception passes through uncaught. */
#include "shim.h"
#include "wasm-encoder.h"
#include <stdio.h>

int main (void) {
	enum { P_BLK = 0, L_EXC = 1, L_H = 2 };   /* param 0 is $blk; two i32 locals */
	enum { F_BB = 0, F_DISPATCH = 1 };        /* the module's two imported functions */
	WasmBuf b;
	FILE *f;

	wasm_buf_init (&b);
	wasm_uleb (&b, 1); wasm_uleb (&b, 2); wasm_u8 (&b, WASM_I32);   /* locals: 2 x i32 */
	wasm_eh_region_open (&b);
	wasm_op_local (&b, WASM_OP_LOCAL_GET, P_BLK);
	wasm_op (&b, WASM_OP_CALL); wasm_uleb (&b, F_BB);              /* the bb: may throw x.e or a JS error */
	wasm_op_local (&b, WASM_OP_LOCAL_GET, P_BLK);
	wasm_op (&b, WASM_OP_RETURN);                                  /* the dispatch never falls through */
	wasm_eh_region_catch (&b);
	wasm_op_local (&b, WASM_OP_LOCAL_SET, L_EXC);
	wasm_op_local (&b, WASM_OP_LOCAL_GET, L_EXC);
	wasm_op (&b, WASM_OP_CALL); wasm_uleb (&b, F_DISPATCH);        /* payload -> handler $blk, or -1 */
	wasm_op_local (&b, WASM_OP_LOCAL_TEE, L_H);
	wasm_i32_const (&b, 0);
	wasm_op (&b, WASM_OP_I32_LT_S);
	wasm_op (&b, WASM_OP_IF); wasm_u8 (&b, WASM_VOID);
	wasm_eh_throw_payload (&b, L_EXC);
	wasm_op (&b, WASM_OP_END);
	wasm_op_local (&b, WASM_OP_LOCAL_GET, L_H);
	wasm_op_local (&b, WASM_OP_LOCAL_SET, P_BLK);
	wasm_op (&b, WASM_OP_BR); wasm_uleb (&b, 0);                   /* depth 0 = $outer: re-dispatch */
	wasm_op (&b, WASM_OP_END);                                     /* $outer */
	wasm_op (&b, WASM_OP_UNREACHABLE);
	wasm_op (&b, WASM_OP_END);                                     /* the function */

	f = fopen ("t14.code", "wb");
	fwrite (b.data, 1, b.len, f);
	fclose (f);
	printf ("t14: EH region framed (%u bytes)\n", (unsigned) b.len);
	wasm_buf_free (&b);
	return 0;
}
