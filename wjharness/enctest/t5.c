/*
 * t5: the SAME body framed at two different ti_base values must ask for the SAME functype.
 *
 * WHY THIS EXISTS. R267 spent sixteen addenda on an intermittent `function signature mismatch` and
 * exhausted the callee side of the baked `call_indirect`: the dependency was installed (`instZero` 0 on
 * 3/3), the table occupant was a real function of the right arity, `want_sig == have_sig` every time, and
 * every emitted module decoded cleanly (`undecoded 0` of 528,215 bodies). Addendum 15 named the one term
 * nothing had checked -- the CALLER's expected type:
 *
 *     guint32 ti = ti_base + r->tpool;            wasm_body_serialize
 *
 * recomputed on EVERY reassembly. Re-framing a body -- standalone into a group, group into a different
 * group, a re-emitted body into a fresh module -- is exactly when those are redone, and re-emission does
 * it constantly. t1/t2 diff against frozen framers and t4 covers nexport < nmembers; NONE of them frames
 * one body twice at different ti_base, which is the case re-emission creates. CLAUDE.md's own rule:
 * "a gate that cannot reach the case a change introduces is not evidence about that change."
 *
 * WHAT IS CHECKED, end to end rather than by re-deriving the formula. Each member gets a pool of extras
 * that is UNIQUE TO IT, so a body resolved against the wrong block lands on a structurally different
 * functype and fails here rather than at runtime. Every member is then framed twice -- alone at ti_base 0,
 * and at a random position in a group -- and both modules are decoded from bytes: locate the member's code
 * body, read each call_indirect's type immediate, resolve it through the module's own type section, and
 * compare the resulting functype with the one the member intended.
 *
 * Only call_indirect (WASM_RELOC_INDIRECT, and the default arm's `i32.const idx; call_indirect ti`) is
 * scanned. TYPE_U/TYPE_S ride the identical `ti_base + r->tpool` line in wasm_body_serialize, so the
 * arithmetic is covered by covering one of them; scanning one opcode keeps the decoder unambiguous.
 *
 * THE NEGATIVE CONTROL IS PART OF THE TEST. A gate nobody has seen fail is a gate nobody has tested: the
 * last pass here ran one deliberately mis-computed ti_base and asserts that the checker reports it. A
 * checksum gate that failed CLOSED over 25 identical runs, and two oracles that passed 400/400 across a
 * change they could not reach, are both in this tree's history.
 */
#include "shim.h"
#include "wasm-encoder.h"

static unsigned rs = 20260918;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }

static guint32 rd_uleb (const guint8 *p, guint32 *off) {
	guint32 r = 0, sh = 0, b;
	do { b = p [(*off)++]; r |= (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return r;
}

#define MAXM   8
#define MAXE   6
#define MAXTY  64

typedef struct { WasmValtype params [WASM_FUNCTYPE_MAX_PARAMS]; guint32 nparams; WasmValtype ret; } Ft;

static int ft_eq (const Ft *a, const WasmFuncType *b) {
	guint32 i;
	if (a->nparams != b->nparams || a->ret != b->ret) return 0;
	for (i = 0; i < a->nparams; ++i) if (a->params [i] != b->params [i]) return 0;
	return 1;
}

/* Decode a module's type section into `out`; returns the count. */
static guint32 decode_types (const WasmBuf *m, Ft *out, guint32 cap) {
	guint32 off = 8, n = 0;
	while (off < m->len) {
		guint32 id = m->data [off++], len = rd_uleb (m->data, &off), end = off + len;
		if (id == 1) {
			guint32 cnt = rd_uleb (m->data, &off), i, j;
			for (i = 0; i < cnt && i < cap; ++i) {
				guint32 np, nr;
				g_assert (m->data [off++] == 0x60);
				np = rd_uleb (m->data, &off);
				out [i].nparams = np;
				for (j = 0; j < np; ++j) out [i].params [j] = (WasmValtype) m->data [off++];
				nr = rd_uleb (m->data, &off);
				out [i].ret = nr ? (WasmValtype) m->data [off++] : WASM_VOID;
			}
			n = cnt;
		}
		off = end;
	}
	return n;
}

/*
 * Collect the type immediates of every call_indirect in code-section body `want`. Bodies here contain
 * only `nop` and `i32.const <small>; call_indirect ti 0`, so a 0x11 byte is unambiguous.
 */
static guint32 decode_calls (const WasmBuf *m, guint32 want, guint32 *tis, guint32 cap) {
	guint32 off = 8, n = 0;
	while (off < m->len) {
		guint32 id = m->data [off++], len = rd_uleb (m->data, &off), end = off + len;
		if (id == 10) {
			guint32 cnt = rd_uleb (m->data, &off), b;
			for (b = 0; b < cnt; ++b) {
				guint32 bsz = rd_uleb (m->data, &off), bend = off + bsz, ng, g;
				ng = rd_uleb (m->data, &off);
				for (g = 0; g < ng; ++g) { rd_uleb (m->data, &off); off++; }
				if (b == want) {
					while (off < bend) {
						guint8 op = m->data [off++];
						if (op == 0x41) { while (m->data [off] & 0x80) off++; off++; }       /* i32.const sleb */
						else if (op == 0x11) {                                              /* call_indirect */
							guint32 ti = rd_uleb (m->data, &off);
							rd_uleb (m->data, &off);                                    /* tableidx */
							if (n < cap) tis [n] = ti;
							n++;
						}
					}
				}
				off = bend;
			}
		}
		off = end;
	}
	return n;
}

int main (void) {
	int iter, bad = 0, negctl_caught = 0, checked = 0;

	/* The last iteration is the NEGATIVE CONTROL: one member is relocated against a deliberately wrong
	 * ti_base, and the checker must notice. */
	for (iter = 0; iter < 301; ++iter) {
		int negctl = (iter == 300);
		guint32 n = 2 + rnd (MAXM - 1), i, j, k;
		guint32 nextra [MAXM], tibase [MAXM];
		Ft pool [MAXM][MAXE];
		WasmFuncType types [MAXM][2 + MAXE];
		WasmLocalGroup groups [MAXM][1];
		WasmBuf raw_f [MAXM], raw_e [MAXM], ser_f [MAXM], ser_e [MAXM], grp;
		WasmAsmMember am [MAXM];
		WasmRelocFix fix [MAXE + 1];
		Ft dtypes [MAXTY];
		guint32 tis [MAXE], negctl_member = negctl ? 1 + rnd (n - 1) : (guint32) -1;

		for (i = 0; i < n; ++i) {
			nextra [i] = 1 + rnd (MAXE);
			/* UNIQUE PER MEMBER, deliberately: with a shared pool every block holds the same functypes
			 * and resolving against the wrong one is indistinguishable from resolving against the right
			 * one. The parameter count encodes (member, slot), so a cross-block resolution is visible. */
			for (j = 0; j < nextra [i]; ++j) {
				guint32 np = (i * MAXE + j) % (WASM_FUNCTYPE_MAX_PARAMS - 1) + 1;
				pool [i][j].nparams = np;
				for (k = 0; k < np; ++k) pool [i][j].params [k] = (k + i + j) & 1 ? WASM_I64 : WASM_I32;
				pool [i][j].ret = (i + j) & 1 ? WASM_I32 : WASM_VOID;
			}
			types [i][0].nparams = 1; types [i][0].params [0] = WASM_I32; types [i][0].ret = WASM_VOID;
			types [i][1].nparams = 2; types [i][1].params [0] = WASM_I32;
			types [i][1].params [1] = WASM_I32; types [i][1].ret = WASM_VOID;
			for (j = 0; j < nextra [i]; ++j) {
				types [i][2 + j].nparams = pool [i][j].nparams;
				for (k = 0; k < pool [i][j].nparams; ++k)
					types [i][2 + j].params [k] = pool [i][j].params [k];
				types [i][2 + j].ret = pool [i][j].ret;
			}
			groups [i][0].type = WASM_I32; groups [i][0].count = 0;

			/* The body: one call_indirect per extra, tpool = 2 + j, in pool order. */
			wasm_buf_init (&raw_f [i]); wasm_buf_init_relocs (&raw_f [i]);
			for (j = 0; j < nextra [i]; ++j) {
				wasm_op (&raw_f [i], WASM_OP_NOP);
				wasm_reloc (&raw_f [i], WASM_RELOC_INDIRECT, 2 + j, 0, NULL);
			}
			wasm_buf_init (&raw_e [i]); wasm_buf_init_relocs (&raw_e [i]);
			wasm_op (&raw_e [i], WASM_OP_NOP);
		}

		/* The rule under test, written ONCE here exactly as wj_assemble writes it. */
		for (i = 0; i < n; ++i)
			tibase [i] = i ? tibase [i - 1] + 2 + nextra [i - 1] : 0;
		if (negctl)
			tibase [negctl_member] += 1;   /* an off-by-one block boundary, the R267 shape */

		for (i = 0; i < n; ++i) {
			wasm_buf_init (&ser_f [i]); wasm_buf_init (&ser_e [i]);
			memset (fix, 0, sizeof (fix));
			wasm_body_serialize (&raw_f [i], fix, tibase [i], &ser_f [i]);
			wasm_body_serialize (&raw_e [i], fix, tibase [i], &ser_e [i]);
			am [i].locals = groups [i]; am [i].nlocal_groups = 1;
			am [i].f_body = &ser_f [i]; am [i].e_body = &ser_e [i];
			am [i].types = types [i]; am [i].ntypes = 2 + nextra [i];
		}
		wasm_buf_init (&grp);
		wasm_module_assemble (am, n, n, TRUE, FALSE, 0, NULL, 0, &grp);

		decode_types (&grp, dtypes, MAXTY);
		for (i = 0; i < n; ++i) {
			guint32 got = decode_calls (&grp, i, tis, MAXE);
			if (got != nextra [i]) {
				printf ("  iter%d m%u: %u call_indirect decoded, want %u\n", iter, i, got, nextra [i]);
				bad++; continue;
			}
			for (j = 0; j < nextra [i]; ++j) {
				WasmFuncType resolved;
				guint32 t = tis [j], q;
				resolved.nparams = dtypes [t].nparams; resolved.ret = dtypes [t].ret;
				for (q = 0; q < resolved.nparams; ++q) resolved.params [q] = dtypes [t].params [q];
				checked++;
				if (!ft_eq (&pool [i][j], &resolved)) {
					if (negctl && i == negctl_member) { negctl_caught++; continue; }
					printf ("  iter%d m%u hole%u: type %u resolves to nparams=%u ret=%#x,"
					        " want nparams=%u ret=%#x (ti_base=%u tpool=%u)\n",
					        iter, i, j, t, resolved.nparams, resolved.ret,
					        pool [i][j].nparams, pool [i][j].ret, tibase [i], 2 + j);
					bad++;
				} else if (negctl && i == negctl_member) {
					/* Tolerated: an off-by-one lands on a structurally equal neighbour often enough
					 * that one hole proving nothing is normal. The control is that SOME hole caught it. */
				}
			}
		}

		/* The same body alone at ti_base 0 must ask for the same functypes it asked for in the group.
		 * This is the re-framing case itself: one WjBody, two modules, two different ti_base. */
		if (!negctl) for (i = 0; i < n; ++i) {
			WasmBuf solo_f, solo_e, solo;
			WasmAsmMember one;
			Ft styp [MAXTY];
			guint32 stis [MAXE], got;
			wasm_buf_init (&solo_f); wasm_buf_init (&solo_e); wasm_buf_init (&solo);
			memset (fix, 0, sizeof (fix));
			wasm_body_serialize (&raw_f [i], fix, 0, &solo_f);
			wasm_body_serialize (&raw_e [i], fix, 0, &solo_e);
			one.locals = groups [i]; one.nlocal_groups = 1;
			one.f_body = &solo_f; one.e_body = &solo_e;
			one.types = types [i]; one.ntypes = 2 + nextra [i];
			wasm_module_assemble (&one, 1, 1, TRUE, FALSE, 0, NULL, 0, &solo);
			decode_types (&solo, styp, MAXTY);
			got = decode_calls (&solo, 0, stis, MAXE);
			if (got != nextra [i]) { printf ("  iter%d m%u solo: %u calls, want %u\n", iter, i, got, nextra [i]); bad++; }
			else for (j = 0; j < nextra [i]; ++j) {
				WasmFuncType r2; guint32 t = stis [j], q;
				r2.nparams = styp [t].nparams; r2.ret = styp [t].ret;
				for (q = 0; q < r2.nparams; ++q) r2.params [q] = styp [t].params [q];
				checked++;
				if (!ft_eq (&pool [i][j], &r2)) {
					printf ("  iter%d m%u hole%u SOLO: type %u wrong after re-framing at ti_base 0\n", iter, i, j, t);
					bad++;
				}
			}
			wasm_buf_free (&solo_f); wasm_buf_free (&solo_e); wasm_buf_free (&solo);
		}

		wasm_buf_free (&grp);
		for (i = 0; i < n; ++i) {
			wasm_buf_free (&raw_f [i]); wasm_buf_free (&raw_e [i]);
			wasm_buf_free (&ser_f [i]); wasm_buf_free (&ser_e [i]);
		}
		if (bad > 12) break;
	}
	if (bad) { printf ("FAIL: %d functype resolutions wrong across re-framing\n", bad); return 1; }
	if (!negctl_caught) { printf ("FAIL: the negative control was NOT caught -- this gate proves nothing\n"); return 1; }
	printf ("PASS: %d functype resolutions correct across 300 re-framings; negative control caught (%d holes)\n",
	        checked, negctl_caught);
	return 0;
}
