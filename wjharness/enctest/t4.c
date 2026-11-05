/*
 * t4: the SHADOW layout -- wasm_module_assemble with nexport < nmembers.
 *
 * WHY THIS EXISTS. t1 and t2 both call the assembler with nexport == nmembers, because that is all the
 * encoder could do before shadows. So when R167 added the shadow path (a lone method framed together with
 * private duplicate callees, exported nowhere) the gates passed 400/400 and 300/300 byte-identical and
 * still said nothing about the one case the change introduced. The live run then failed with
 *   TypeError: WebAssembly.Table.set(): function-typed object must be null or a WebAssembly function
 * ~959,405 times -- an export that did not resolve to a function.
 *
 * This is a STRUCTURAL check, not an oracle comparison: there is no legacy framer that can produce this
 * layout, so nothing to diff against. It decodes the module the assembler emits and asserts the contract
 * the JS instantiate path depends on:
 *   - exactly nexport*2 exports, named "f"/"e" at nexport == 1 and "f<i>"/"e<i>" above it;
 *   - "f<i>" -> funcidx nfimports + i          (methods come first)
 *   - "e<i>" -> funcidx nfimports + nmembers + i  (ALL methods precede ANY thunk -- the shadow members
 *     sit between method 0 and thunk 0, which is exactly what the index arithmetic must account for);
 *   - the function and code sections both describe 2*nmembers functions, shadows included, since a shadow
 *     is a defined function that simply has no export naming it.
 */
#include "shim.h"
#include "wasm-encoder.h"

static unsigned rs = 20260831;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }
static WasmValtype vt (void) { static const WasmValtype t [4] = { WASM_I32, WASM_I64, WASM_F32, WASM_F64 }; return t [rnd (4)]; }

static guint32 rd_uleb (const guint8 *p, guint32 *off) {
	guint32 r = 0, sh = 0, b;
	do { b = p [(*off)++]; r |= (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return r;
}

#define MAXM 10
int main (void) {
	int iter, bad = 0;
	for (iter = 0; iter < 200; ++iter) {
		WasmAsmMember nm [MAXM];
		WasmValtype params [MAXM][WASM_FUNCTYPE_MAX_PARAMS];
		WasmFuncType extra [MAXM][8], types [MAXM][10];
		WasmLocalGroup groups [MAXM][4];
		WasmBuf fb [MAXM], eb [MAXM], out;
		WasmFuncImport fimp [8];
		guint32 n = 2 + rnd (MAXM - 1), nfimp = rnd (8), i, j;
		guint32 nexport = 1 + rnd (n);          /* the whole point: often < n */
		guint32 off = 8, nfunc_sec = 0, ncode_sec = 0, nexp_seen = 0, seen_f = 0, seen_e = 0;

		for (i = 0; i < nfimp; ++i) { fimp [i].table_index = 40000 + rnd (9999); fimp [i].type_idx = 0; }
		for (i = 0; i < n; ++i) {
			guint32 np = rnd (5), ne = rnd (8);
			for (j = 0; j < np; ++j) params [i][j] = vt ();
			types [i][0].nparams = np;
			for (j = 0; j < np; ++j) types [i][0].params [j] = params [i][j];
			types [i][0].ret = rnd (5) ? vt () : WASM_VOID;
			types [i][1].nparams = 2; types [i][1].params [0] = WASM_I32;
			types [i][1].params [1] = WASM_I32; types [i][1].ret = WASM_VOID;
			for (j = 0; j < ne; ++j) { extra [i][j].nparams = 0; extra [i][j].ret = WASM_VOID; types [i][2 + j] = extra [i][j]; }
			for (j = 0; j < 4; ++j) { groups [i][j].type = WASM_I32; groups [i][j].count = rnd (3); }
			wasm_buf_init (&fb [i]); wasm_buf_init (&eb [i]);
			wasm_op (&fb [i], WASM_OP_NOP); wasm_op (&eb [i], WASM_OP_NOP);
			nm [i].locals = groups [i]; nm [i].nlocal_groups = 4;
			nm [i].f_body = &fb [i]; nm [i].e_body = &eb [i];
			nm [i].types = types [i]; nm [i].ntypes = 2 + ne;
		}
		wasm_buf_init (&out);
		wasm_module_assemble (nm, n, nexport, FALSE, FALSE, 0, fimp, nfimp, &out);

		/* Walk the sections. */
		while (off < out.len) {
			guint32 id = out.data [off++], len = rd_uleb (out.data, &off), end;
			end = off + len;
			if (id == 3) nfunc_sec = rd_uleb (out.data, &off);
			else if (id == 10) ncode_sec = rd_uleb (out.data, &off);
			else if (id == 7) {
				guint32 cnt = rd_uleb (out.data, &off), k;
				nexp_seen = cnt;
				for (k = 0; k < cnt; ++k) {
					guint32 nlen = rd_uleb (out.data, &off), idx, want;
					char nmbuf [32];
					guint32 c; int isf;
					for (c = 0; c < nlen && c < 31; ++c) nmbuf [c] = (char) out.data [off + c];
					nmbuf [nlen < 31 ? nlen : 31] = 0;
					off += nlen; off++;            /* skip kind byte */
					idx = rd_uleb (out.data, &off);
					isf = nmbuf [0] == 'f';
					{
						guint32 mi = (nlen == 1) ? 0 : (guint32) atoi (nmbuf + 1);
						if (mi >= nexport) { printf ("  iter%d: export %s names member %u >= nexport %u\n", iter, nmbuf, mi, nexport); bad++; }
						want = isf ? nfimp + mi : nfimp + n + mi;
						if (idx != want) { printf ("  iter%d: export %s -> funcidx %u, want %u (n=%u nexport=%u nfimp=%u)\n", iter, nmbuf, idx, want, n, nexport, nfimp); bad++; }
						if (nexport == 1 && nlen != 1) { printf ("  iter%d: nexport==1 but export name is '%s', want 'f'/'e'\n", iter, nmbuf); bad++; }
						if (nexport > 1 && nlen == 1) { printf ("  iter%d: nexport==%u but export name is bare '%s'\n", iter, nexport, nmbuf); bad++; }
					}
					if (isf) seen_f++; else seen_e++;
				}
			}
			off = end;
		}
		if (nexp_seen != nexport * 2) { printf ("  iter%d: %u exports, want %u\n", iter, nexp_seen, nexport * 2); bad++; }
		if (seen_f != nexport || seen_e != nexport) { printf ("  iter%d: %u f-exports / %u e-exports, want %u each\n", iter, seen_f, seen_e, nexport); bad++; }
		if (nfunc_sec != n * 2) { printf ("  iter%d: function section declares %u, want %u (shadows are defined functions)\n", iter, nfunc_sec, n * 2); bad++; }
		if (ncode_sec != n * 2) { printf ("  iter%d: code section has %u bodies, want %u\n", iter, ncode_sec, n * 2); bad++; }

		wasm_buf_free (&out);
		for (i = 0; i < n; ++i) { wasm_buf_free (&fb [i]); wasm_buf_free (&eb [i]); }
		if (bad > 12) break;
	}
	if (bad) { printf ("FAIL: %d structural violations at nexport < nmembers\n", bad); return 1; }
	printf ("PASS: 200/200 shadow layouts well-formed (nexport < nmembers)\n");
	return 0;
}
