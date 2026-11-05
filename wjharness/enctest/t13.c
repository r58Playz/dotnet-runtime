/* R516: call-site KEYS (WasmSites.kd). V8 positions no call inside a `try` of its own frame and reports the last
 * positioned instruction before it, so the stack walk also matches a site by the last memory access before its call --
 * recorded only when its straight-line run (control ops and calls end one) has a memory access after the call too, or
 * an `unreachable` right behind it. This re-implements that rule over the SOURCE events and checks every recorded key
 * decodes to exactly the memory access the rule picks (each carries a unique memarg offset), and that no other site has
 * one. The GUARDED pair's key is its guard load, on the first of its two calls. */
#include "shim.h"
#include "wasm-encoder.h"

static unsigned rs = 1613;
static unsigned rnd (unsigned n) { rs = rs * 1103515245u + 12345u; return (rs >> 16) % n; }

static guint32 rd_uleb (const guint8 *p, guint32 *i)
{
	guint32 v = 0, sh = 0;
	guint8 b;
	do { b = p [(*i)++]; v |= (guint32) (b & 0x7f) << sh; sh += 7; } while (b & 0x80);
	return v;
}

int main (void) {
	int iter, bad = 0, keyed = 0, sites_total = 0, controls_open = 0;
	for (iter = 0; iter < 2000; ++iter) {
		WasmBuf hole, out;
		WasmRelocFix fix [512];
		gint64 want [1024];          /* per site: the memarg offset its key must decode to, or -1 */
		int site_key_ev [1024];      /* per site: the memarg id the rule saw before its call, or -1 */
		int site_guarded [1024];     /* 1: the first call of a guarded pair (its key is the guard) */
		guint32 site_guard [1024];
		gboolean closed [1024];
		guint32 nops, i, nh = 0, nsite = 0, mem_id = 0;
		int last_mem = -1, pending = -1;

		wasm_buf_init (&hole); wasm_buf_init_relocs (&hole);
		nops = 1 + rnd (160);
		for (i = 0; i < nops && nh < 500 && nsite < 1000; ++i) {
			guint32 tp = rnd (40);
			switch (rnd (12)) {
			case 0: case 1: case 2: {   /* a memory access with a unique offset */
				guint32 off = 20000 + mem_id;
				if (rnd (4) == 0) {
					wasm_op (&hole, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&hole, 0x17); wasm_memarg (&hole, 2, off);
				} else {
					wasm_op (&hole, rnd (2) ? WASM_OP_I32_LOAD : WASM_OP_I32_STORE); wasm_memarg (&hole, 2, off);
				}
				if (pending >= 0) { closed [pending] = TRUE; pending = -1; }
				last_mem = (int) off;
				mem_id++;
				break;
			}
			case 3: {                   /* control flow ends a run */
				static const WasmOpcode ctl [] = { WASM_OP_BLOCK, WASM_OP_LOOP, WASM_OP_ELSE, WASM_OP_END, WASM_OP_BR_IF, WASM_OP_BR };
				WasmOpcode c = ctl [rnd (6)];
				wasm_op (&hole, c);
				if (c == WASM_OP_BLOCK || c == WASM_OP_LOOP)
					wasm_u8 (&hole, 0x40);
				else if (c == WASM_OP_BR_IF || c == WASM_OP_BR)
					wasm_uleb (&hole, rnd (4));
				if (pending >= 0) controls_open++;
				pending = -1; last_mem = -1;
				break;
			}
			case 4:                     /* unreachable: a positioned trap -- bounds the call before it, ends the run */
				wasm_op (&hole, WASM_OP_UNREACHABLE);
				if (pending >= 0) { closed [pending] = TRUE; pending = -1; }
				last_mem = -1;
				break;
			case 5:                     /* a type hole behind an `if`: control */
				wasm_op (&hole, WASM_OP_IF);
				wasm_reloc (&hole, rnd (2) ? WASM_RELOC_TYPE_S : WASM_RELOC_TYPE_U, tp, 0, NULL);
				fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0; nh++;
				pending = -1; last_mem = -1;
				break;
			case 6: case 7:             /* nothing positioned */
				wasm_i32_const (&hole, (gint32) rnd (100000));
				if (rnd (2))
					wasm_op_local (&hole, WASM_OP_LOCAL_SET, rnd (9));
				break;
			default: {                  /* a call, any form */
				int kind = (int) rnd (5), form = (int) rnd (4);
				if (kind == 0) {
					wasm_reloc (&hole, WASM_RELOC_HELPER, tp, 30000 + rnd (400000), NULL);
					fix [nh].form = rnd (2) ? WASM_FORM_INDIRECT : WASM_FORM_IMPORT; fix [nh].idx = rnd (30);
					form = 0;
				} else if (kind == 1) {
					wasm_reloc (&hole, WASM_RELOC_INDIRECT, tp, 0, NULL);
					fix [nh].form = WASM_FORM_INDIRECT; fix [nh].idx = 0;
					form = 0;
				} else {
					wasm_reloc (&hole, WASM_RELOC_CALL, tp, 500000 + rnd (90000), (gpointer) (gsize) (iter + 1));
					fix [nh].form = form == 0 ? WASM_FORM_INDIRECT : form == 1 ? WASM_FORM_IMPORT : form == 2 ? WASM_FORM_LOCAL : WASM_FORM_LOCAL_GUARDED;
					fix [nh].idx = rnd (200);
					fix [nh].guard = 40000 + 4 * nh;   /* unique: identifies the guard load */
					fix [nh].guard_count = rnd (2) ? 8192 : 0;
				}
				/* the rule: this call ends the previous one's run (left open if nothing closed it) */
				pending = -1;
				site_key_ev [nsite] = last_mem;
				site_guarded [nsite] = form == 3;
				site_guard [nsite] = fix [nh].guard;
				closed [nsite] = FALSE;
				pending = (int) nsite;
				nsite++;
				if (form == 3) {         /* the pair's second call: never keyed */
					site_key_ev [nsite] = -1; site_guarded [nsite] = 0; closed [nsite] = FALSE;
					nsite++;
				}
				nh++;
				last_mem = -1;
				break;
			}
			}
		}
		for (i = 0; i < nsite; ++i)
			want [i] = !closed [i] ? -1 : site_guarded [i] ? (gint64) site_guard [i] : (gint64) site_key_ev [i];

		wasm_buf_init (&out);
		wasm_body_serialize (&hole, fix, rnd (300), &out);
		{
			guint32 n = out.sites ? out.sites->n : 0, k;
			if (n != nsite) {
				printf ("iter %d: %u sites recorded, %u expected\n", iter, n, nsite);
				bad++;
			}
			for (k = 0; k < n && k < nsite; ++k) {
				guint32 kd = out.sites->kd [k], o = out.sites->off [k];
				gint64 got = -1;
				if (kd) {
					guint32 p = o - kd;
					guint8 op = out.data [p++];
					if (op == WASM_OP_ATOMIC_PREFIX)
						p++;
					else if (op < 0x28 || op > 0x3e) {
						printf ("iter %d site %u: key at %u is opcode %02x, not a memory access\n", iter, k, o - kd, op);
						bad++;
						break;
					}
					rd_uleb (out.data, &p);
					got = rd_uleb (out.data, &p);
					keyed++;
				}
				if (got != want [k]) {
					printf ("iter %d site %u: key decodes to memarg offset %lld, the rule picks %lld (guarded %d closed %d)\n",
						iter, k, (long long) got, (long long) want [k], site_guarded [k], closed [k]);
					bad++;
					break;
				}
			}
			sites_total += (int) n;
		}
		wasm_buf_free (&out); wasm_buf_free (&hole);
	}
	printf (bad ? "FAIL: %d bad bodies\n" : "PASS: %d of %d call sites keyed, every key the access the rule picks (%d runs left open by control)\n",
		bad ? bad : keyed, sites_total, controls_open);
	return bad != 0;
}
