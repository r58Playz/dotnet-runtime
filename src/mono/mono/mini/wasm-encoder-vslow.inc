/*
 * A VIRTUAL SLOW-PATH MODULE (plan typed-spindle C1, MONO_WASM_JIT_VSITE_COMPACT; the registry is mini-wasm-vslow.inc).
 * Included by wasm-encoder.c, so it shares that file's static section helpers.
 *
 * WHY. A virtual call site used to emit its whole dispatch inline: way 0 of the worker PIC, the method-identity check,
 * the F9 cache, ways 1..N, the inline AOT IC with two typed call paths, and the per-site cold miss with its own
 * resolve / AOT / interpreter routes, reloading the arguments for each -- 450-600 B per site, 48-54% of every byte the
 * tier emits, and what pushes the tick's giants past T2_MAX_BODY (R455). A compact site keeps only what runs on the
 * common path inline (its devirt arms and way 0) and hands every way-0 miss to ONE function per (functype, return
 * normalisation), shared process-wide and written into its own table slot here.
 *
 * Type (site params..., i64 desc) -> site ret. The descriptor travels as an i64 ON PURPOSE: a slot this worker has
 * not instantiated still holds the jiterpreter's prefilled placeholder, (i32,i32,i32,i32)->void, whose body writes 999
 * through its fourth argument; with an i32 descriptor a site of type (i32,i32,i32)->void would match that signature
 * and corrupt the heap silently instead of trapping (CLAUDE.md, "The prefilled placeholder"). An i64 parameter can
 * never match it. The registry still instantiates every stub on a pthread before any module that names one.
 *
 * Helpers, imported by table index exactly like a lazy bank's:
 *   [0] resolve  (i32 this, i32 desc, i32 scratch)->i32  the worker PIC's ways and method identity, then
 *                                                          mono_wasm_jit_vcall_resolve_fslot: an admitted f-slot, or 0
 *                                                          with the resolved target at scratch+200
 *   [1] scratch  ()->i32                                 THIS pthread's scratch (never s.b: the stub outlives a pthread)
 *   [2] aot      (i32 scratch, i32 this, i32 aic)->i32   mono_wasm_jit_vcall_aot_target: 0 = not AOT-backed, 1 = AOT
 *                                                          body + rgctx (scratch+212 / +216), 2 = AOT body, no extra arg
 *   [3] interp   (i32 target, i32 buf)->i32              mono_wasm_jit_call_interp: 1 if the callee threw
 *   [4] continue_unwind ()->void                         never returns
 * Descriptor (WjVslowDesc): +0 the site's vcall IC, +4 its AOT IC (0 = none), +8 the base method, the rest for resolve.
 *
 * Body, n site params:
 *   AOT IC way 0 (no frame): if aic && both words' low halves == this->vtable: call the AOT body (kind bit in the low
 *     bit of word 0's high half, rgctx in word 1's), normalise a sub-word return, return.
 *   frame: every i32 param stored in a C-stack frame for the helper calls -- resolve, aot and interp can compile,
 *     instantiate, run a cctor and collect, so the arguments must be under sgen's conservative stack scan. The frame
 *     is popped before every final call, as in the lazy bank.
 *   f = resolve (this, desc, scratch); if f: pop; return_call_indirect site (params..., f)   (a JIT f is normalised)
 *   k = aot (scratch, this, aic); if k: pop; call the AOT body by k; normalise; return
 *   residual: params -> scratch + 8*j; if interp (target, scratch) { continue_unwind; unreachable }
 *     [ret = load scratch + 192; normalise]; pop
 * retnorm: 0 none, 1 u8 (bool/byte), 2 s8, 3 u16 (char/ushort), 4 s16 -- applied to AOT and interpreter returns only,
 * as the inline cold path did (wasm_emit_subword_ret_norm and the residual's switch).
 */
static void
vslow_norm (WasmBuf *b, int retnorm)
{
	switch (retnorm) {
	case 1: wasm_i32_const (b, 0xff); wasm_op (b, WASM_OP_I32_AND); break;
	case 2: wasm_op (b, WASM_OP_I32_EXTEND8_S); break;
	case 3: wasm_i32_const (b, 0xffff); wasm_op (b, WASM_OP_I32_AND); break;
	case 4: wasm_op (b, WASM_OP_I32_EXTEND16_S); break;
	default: break;
	}
}

static void
vslow_blocktype (WasmBuf *b, WasmValtype ret)
{
	wasm_u8 (b, ret == WASM_VOID ? 0x40 : (guint8) ret);
}

void
wasm_module_vslow (const WasmFuncType *ft, int retnorm, int f_slot, const guint32 *helper_idx, WasmBuf *out)
{
	static const guint8 header [8] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	WasmFuncImport fim [5];
	WasmValtype stub_p [WASM_FUNCTYPE_MAX_PARAMS + 1], at_p [WASM_FUNCTYPE_MAX_PARAMS + 1];
	WasmValtype p2 [2] = { WASM_I32, WASM_I32 }, p3 [3] = { WASM_I32, WASM_I32, WASM_I32 };
	WasmLocalGroup lg [2] = { { WASM_I32, 9 }, { WASM_I64, 2 } };
	WasmBuf sec, body;
	guint32 i, j, n = ft->nparams, ni32 = 0, frame, kth;
	/* locals after the n site params and the i64 descriptor (param n) */
	guint32 l_desc = n + 1, l_vt = n + 2, l_aic = n + 3, l_ti = n + 4, l_rg = n + 5, l_r = n + 6, l_fs = n + 7;
	guint32 l_old = n + 8, l_sp = n + 9, l_w0 = n + 10, l_w1 = n + 11;

	g_assert (n >= 1 && n + 1 <= WASM_FUNCTYPE_MAX_PARAMS);   /* `this` is param 0; the caller checked the room */
	for (j = 0; j < n; ++j) {
		stub_p [j] = ft->params [j];
		at_p [j] = ft->params [j];
		if (ft->params [j] == WASM_I32)
			ni32++;
	}
	stub_p [n] = WASM_I64;
	at_p [n] = WASM_I32;
	frame = (ni32 * 4 + 15) & ~15u;

	wasm_bytes (out, header, 8);
	/* types: 0 stub, 1 the site's own (f-slot call, AOT body without the extra arg), 2 AOT body with rgctx,
	 * 3 (i32,i32,i32)->i32 resolve/aot, 4 ()->i32 scratch, 5 (i32,i32)->i32 interp, 6 ()->void continue_unwind */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 7);
	emit_functype (&sec, stub_p, n + 1, ft->ret);
	emit_functype (&sec, ft->params, n, ft->ret);
	emit_functype (&sec, at_p, n + 1, ft->ret);
	emit_functype (&sec, p3, 3, WASM_I32);
	emit_functype (&sec, NULL, 0, WASM_I32);
	emit_functype (&sec, p2, 2, WASM_I32);
	emit_functype (&sec, NULL, 0, WASM_VOID);
	emit_section (out, 1, &sec);
	wasm_buf_free (&sec);

	{
		static const guint32 htype [5] = { 3, 4, 3, 5, 6 };
		for (i = 0; i < 5; ++i) {
			fim [i].table_index = helper_idx [i];
			fim [i].type_idx = htype [i];
			fim [i].method = 0;
		}
	}
	emit_import_section (out, TRUE, FALSE, 0, fim, 5);

	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_uleb (&sec, 0);
	emit_section (out, 3, &sec);
	wasm_buf_free (&sec);

	/* element: one active segment, table 0 (the imported f.f), offset = the stub's slot, function 5 (after 5 imports) */
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	wasm_uleb (&sec, 0);
	wasm_op (&sec, WASM_OP_I32_CONST); wasm_sleb (&sec, f_slot); wasm_op (&sec, WASM_OP_END);
	wasm_uleb (&sec, 1);
	wasm_uleb (&sec, 5);
	emit_section (out, 9, &sec);
	wasm_buf_free (&sec);

#define VS_PARAMS() do { for (j = 0; j < n; ++j) wasm_op_local (&body, WASM_OP_LOCAL_GET, j); } while (0)
#define VS_POP() do { if (frame) { wasm_op_local (&body, WASM_OP_LOCAL_GET, l_old); \
		wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); } } while (0)
	wasm_buf_init (&body);
	wasm_op_local (&body, WASM_OP_LOCAL_GET, n);
	wasm_op (&body, WASM_OP_I32_WRAP_I64);
	wasm_op_local (&body, WASM_OP_LOCAL_SET, l_desc);

	/* AOT IC way 0 */
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_desc);
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 4);
	wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_aic);
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		wasm_op_local (&body, WASM_OP_LOCAL_GET, 0);
		wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, l_vt);
		wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $no_aot */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_aic);
			wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 0);   /* i64.atomic.load */
			wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_w0);
			wasm_op (&body, WASM_OP_I32_WRAP_I64);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_vt);
			wasm_op (&body, WASM_OP_I32_NE);
			wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_aic);
			wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x11); wasm_memarg (&body, 3, 8);
			wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_w1);
			wasm_op (&body, WASM_OP_I32_WRAP_I64);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_vt);
			wasm_op (&body, WASM_OP_I32_NE);
			wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_w0);
			wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, l_ti);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_w1);
			wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, l_rg);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_ti);
			wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);
			wasm_op (&body, WASM_OP_IF); vslow_blocktype (&body, ft->ret);
				VS_PARAMS ();
				wasm_op_local (&body, WASM_OP_LOCAL_GET, l_ti);
				wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, 1); wasm_uleb (&body, 0);
			wasm_op (&body, WASM_OP_ELSE);
				VS_PARAMS ();
				wasm_op_local (&body, WASM_OP_LOCAL_GET, l_rg);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, l_ti);
				wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);
				wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, 2); wasm_uleb (&body, 0);
			wasm_op (&body, WASM_OP_END);
			if (ft->ret == WASM_I32)
				vslow_norm (&body, retnorm);
			wasm_op (&body, WASM_OP_RETURN);
		wasm_op (&body, WASM_OP_END);   /* $no_aot */
	wasm_op (&body, WASM_OP_END);

	/* frame for the helpers */
	if (frame) {
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_old);
		wasm_i32_const (&body, (gint32) frame);
		wasm_op (&body, WASM_OP_I32_SUB);
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_sp);
		wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
		kth = 0;
		for (j = 0; j < n; ++j) {
			if (ft->params [j] != WASM_I32)
				continue;
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_sp);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, j);
			wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 4 * kth++);
		}
	}
	wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 1);          /* scratch () */
	wasm_op_local (&body, WASM_OP_LOCAL_SET, l_r);

	/* JIT f-slot */
	wasm_op_local (&body, WASM_OP_LOCAL_GET, 0);
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_desc);
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
	wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 0);          /* resolve (this, desc, scratch) */
	wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_fs);
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		VS_POP ();
		VS_PARAMS ();
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_fs);
		wasm_u8 (&body, 0x13);                                      /* return_call_indirect */
		wasm_uleb (&body, 1);
		wasm_uleb (&body, 0);
	wasm_op (&body, WASM_OP_END);

	/* AOT-backed override */
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
	wasm_op_local (&body, WASM_OP_LOCAL_GET, 0);
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_aic);
	wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 2);          /* aot (scratch, this, aic) */
	wasm_op_local (&body, WASM_OP_LOCAL_TEE, l_ti);
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		VS_POP ();
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_ti);
		wasm_i32_const (&body, 2);
		wasm_op (&body, WASM_OP_I32_EQ);
		wasm_op (&body, WASM_OP_IF); vslow_blocktype (&body, ft->ret);
			VS_PARAMS ();
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
			wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, 1); wasm_uleb (&body, 0);
		wasm_op (&body, WASM_OP_ELSE);
			VS_PARAMS ();
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
			wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 216);   /* rgctx */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
			wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);
			wasm_op (&body, WASM_OP_CALL_INDIRECT); wasm_uleb (&body, 2); wasm_uleb (&body, 0);
		wasm_op (&body, WASM_OP_END);
		if (ft->ret == WASM_I32)
			vslow_norm (&body, retnorm);
		wasm_op (&body, WASM_OP_RETURN);
	wasm_op (&body, WASM_OP_END);

	/* residual: the interpreter, through the arguments spilled to scratch */
	for (j = 0; j < n; ++j) {
		WasmOpcode sop;
		guint32 al;
		switch (ft->params [j]) {
		case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
		case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
		case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
		default:       sop = WASM_OP_I32_STORE; al = 2; break;
		}
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
		wasm_op_local (&body, WASM_OP_LOCAL_GET, j);
		wasm_op (&body, sop); wasm_memarg (&body, al, 8 * j);
	}
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 200);   /* resolved target MonoMethod* */
	wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
	wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 3);          /* interp (target, scratch) */
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
		wasm_op (&body, WASM_OP_CALL); wasm_uleb (&body, 4);      /* continue_unwind () */
		wasm_op (&body, WASM_OP_UNREACHABLE);
	wasm_op (&body, WASM_OP_END);
	if (ft->ret != WASM_VOID) {
		WasmOpcode lop;
		guint32 al;
		switch (ft->ret) {
		case WASM_I64: lop = WASM_OP_I64_LOAD; al = 3; break;
		case WASM_F32: lop = WASM_OP_F32_LOAD; al = 2; break;
		case WASM_F64: lop = WASM_OP_F64_LOAD; al = 3; break;
		default:       lop = WASM_OP_I32_LOAD; al = 2; break;
		}
		wasm_op_local (&body, WASM_OP_LOCAL_GET, l_r);
		wasm_op (&body, lop); wasm_memarg (&body, al, 192);           /* WJ_SCRATCH_RET_OFF */
		if (ft->ret == WASM_I32)
			vslow_norm (&body, retnorm);
	}
	VS_POP ();
#undef VS_PARAMS
#undef VS_POP

	(void) l_rg;
	wasm_buf_init (&sec);
	wasm_uleb (&sec, 1);
	emit_code_entry (&sec, lg, 2, &body);
	emit_section (out, 10, &sec);
	wasm_buf_free (&sec);
	wasm_buf_free (&body);
	emit_stub_names (out, "wjvslow", 5, 1, "wj_vslow_stub");
}
