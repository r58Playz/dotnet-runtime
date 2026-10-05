/*
 * COMPACT VIRTUAL CALL SITES (plan typed-spindle C1, MONO_WASM_JIT_VSITE_COMPACT; the module encoder is
 * wasm-encoder-vslow.inc). Included by mini-wasm.c before the emitter, which is its only client.
 *
 * A compact site keeps its devirt arms and way 0 of the worker PIC inline and hands every way-0 miss to one shared
 * slow-path function per (functype, return normalisation): the stub. Everything a site used to emit after way 0 --
 * method identity, the F9 cache, ways 1..N, the inline AOT IC and the per-site cold miss -- lives in the stub and in
 * mono_wasm_jit_vslow_resolve (interp.c), driven by a per-site descriptor. R455 sized what this takes out of the
 * emitted code: virtual sites are 450-600 B each and 48-54% of every byte the tier emits.
 *
 * THE INVARIANTS:
 *  1. A stub is written into its own table slot by its module's active element segment, so it exists on a pthread
 *     exactly when that pthread instantiated it. Callers reach it through a helper call by table index -- resolved as
 *     an import at instantiation or as a constant call_indirect -- so EVERY stub is instantiated on a pthread before
 *     that pthread instantiates any JIT module (wj_vslow_ensure_all, called first by both instantiate paths). Stubs
 *     are published in order and a pthread tracks how many it has, so the check is one compare once caught up.
 *  2. The stub's descriptor parameter is an i64, so its type can never equal the jiterpreter placeholder's
 *     (i32,i32,i32,i32)->void: an uninstalled slot traps instead of writing 999 through an argument.
 *  3. A stub bakes no TLS (scratch comes from a helper, never s.b), like a lazy bank: a pthread that takes over a
 *     worker starts with done = 0 and re-instantiates every stub, which rewrites the same slots harmlessly (R293).
 *  4. Writers (stub creation, descriptor allocation) run only inside the compile section; a stub is validated and
 *     instantiated on the creating pthread before it is published, so a site is never baked against a stub that V8
 *     would refuse -- a deterministic CompileError here would otherwise make every caller's instantiation fail on
 *     every worker, forever (CLAUDE.md, the R289 rule).
 *  5. Stubs and descriptors are never freed (CLAUDE.md, "prefer leaking to freeing").
 */
#ifdef HOST_BROWSER

#define WJ_VSLOW_MAX   4096
#define WJ_VSLOW_HASH  8192   /* open addressing over (functype hash, retnorm); power of two */

typedef struct {
	WasmFuncType ft;
	int retnorm;
	int slot;
	guint8 *bytes;
	int len;
} WjVslowStub;

/* The per-site descriptor the stub's i64 parameter points at; read by the stub (aic) and by
 * mono_wasm_jit_vslow_resolve (everything). Layout is a contract with interp.c's copy of this struct. */
typedef struct {
	gpointer vic;        /* +0  the site's shared vcall IC (mono_wasm_jit_alloc_ic) */
	gpointer aic;        /* +4  its AOT IC (mono_wasm_jit_alloc_aot_ic), or NULL */
	MonoMethod *base;    /* +8  the callvirt's base method */
	gint32 site;         /* +12 the worker PIC's site id */
	gint32 ways;         /* +16 the global PIC width the site's region was laid out at */
	gint32 mid_slot;     /* +20 the class vtable slot for the method-identity check, or -1 */
	gint32 pad [2];
} WjVslowDesc;

static WjVslowStub *volatile wj_vslow_stubs [WJ_VSLOW_MAX];
static volatile int wj_vslow_n;
static gint16 wj_vslow_hash [WJ_VSLOW_HASH];   /* stub index + 1 */
static __thread int wj_vslow_done;             /* stubs [0, done) are instantiated on THIS pthread */

WJ_KEEPALIVE int mono_wasm_jit_vslow_resolve (MonoObject *this_obj, gpointer desc, guint8 *scratch);

/* Instantiate stub i on this pthread. Runs GC-safe across the JS work like every instantiation here (R162). */
static gboolean
wj_vslow_instantiate (WjVslowStub *s)
{
	int ok;
	{ extern int mono_wasm_jit_bytes_check;
	  if (G_UNLIKELY (mono_wasm_jit_bytes_check))
		wj_bytes_check (s->bytes, s->len, "v"); }   /* R462 */
	WJ_JS_BLOCKING_BEGIN (__wj_cookie, __wj_sd, __wj_entered);
	ok = EM_ASM_INT ({
		var p = $0 < 0 ? $0 + 4294967296 : $0;
		try {
			/* The same helper resolver instantiate_local and the lazy banks build, created the same way if a stub is
			 * this worker's first module: a later instantiate_local must find __wjSlotFn, or the worker-reuse reset
			 * misses its slots. */
			if (!Module.__wjHelperImports) {
				Module.__wjSlotFn = new Map ();
				Module.__wjSlotChanged = 0;
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string" || !/^[0-9]+$/.test (k)) return undefined;
					return wasmTable.get (Number (k));
				} });
			}
			new WebAssembly.Instance (new WebAssembly.Module (HEAPU8.slice (p, p + $1)), { m: { h: wasmMemory }, f: { f: wasmTable },
				s: { p: wasmExports["__stack_pointer"], l: 0, c: 0, v: 0, n: 0, d: 0, m: 0, b: 0, i: 0, g: 0 }, h: Module.__wjHelperImports });
			/* R459 diagnostic: what this worker put at the stub's slot, for the LinkError explainer in instantiate_local */
			(Module.__wjVslowFn || (Module.__wjVslowFn = new Map ())).set ($2, wasmTable.get ($2));
			return 1;
		} catch (e) {
			if ((Module.__wjVslowErr = (Module.__wjVslowErr | 0) + 1) <= 8)
				console.log ("WASM_JIT_VSLOW_FAIL " + e);
			return 0;
		}
	}, (int) (intptr_t) s->bytes, s->len, s->slot);
	WJ_JS_BLOCKING_END (__wj_cookie, __wj_sd, __wj_entered);
	if (ok)
		mono_wasm_jit_counters [WJC_VSLOW_INST]++;
	else
		mono_wasm_jit_counters [WJC_VSLOW_FAIL]++;
	return ok != 0;
}

/* Invariant 1: every published stub instantiated on this pthread. A stub that fails here (per-worker: an OOM) is
 * retried on the next call rather than skipped, so `done` never counts a stub this pthread does not have. */
static void
wj_vslow_ensure_all (void)
{
	int n = wj_vslow_n;
	if (G_LIKELY (wj_vslow_done >= n))
		return;
	mono_memory_barrier ();
	while (wj_vslow_done < n) {
		WjVslowStub *s = wj_vslow_stubs [wj_vslow_done];
		if (!s || !wj_vslow_instantiate (s))
			return;
		wj_vslow_done++;
	}
}

/* The table slot of the stub for (ft, retnorm), creating, validating and publishing it on first use; 0 = no stub
 * (the caller emits the full inline form). Compile section only (invariant 4). */
static int
wj_vslow_slot (const WasmFuncType *ft, int retnorm)
{
	extern int mono_jiterp_allocate_table_entries (int type, int n);
	extern gpointer mono_wasm_jit_scratch (void);
	extern int mono_wasm_jit_vcall_aot_target (guint8 *scratch, MonoObject *this_obj, gpointer aic);
	extern int mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf);
	extern void mono_wasm_jit_continue_unwind (void);
	guint32 h = (wj_functype_hash (ft) * 31u + (guint32) retnorm), i, k;
	guint32 helpers [5];
	WjVslowStub *s;
	WasmBuf out;
	int slot, id;
	if (ft->nparams < 1 || ft->nparams + 1 > WASM_FUNCTYPE_MAX_PARAMS)
		return 0;
	/* The module about to name this stub is validated by instantiating it on THIS pthread (the emit-time check), so
	 * this pthread must hold every published stub, including ones other compiling workers created. */
	wj_vslow_ensure_all ();
	for (k = 0; k < WJ_VSLOW_HASH; ++k) {
		i = (h + k) & (WJ_VSLOW_HASH - 1);
		if (!wj_vslow_hash [i])
			break;
		s = wj_vslow_stubs [wj_vslow_hash [i] - 1];
		if (s->retnorm == retnorm && functype_eq (&s->ft, ft))
			return s->slot;
	}
	if (k == WJ_VSLOW_HASH || wj_vslow_n >= WJ_VSLOW_MAX) {
		mono_wasm_jit_counters [WJC_VSLOW_REFUSED]++;
		return 0;
	}
	slot = mono_jiterp_allocate_table_entries (1 /* JITERPRETER_TABLE_JIT_CALL */, 1);
	if (slot <= 0) {
		mono_wasm_jit_counters [WJC_VSLOW_REFUSED]++;
		return 0;
	}
	helpers [0] = (guint32) (intptr_t) mono_wasm_jit_vslow_resolve;
	helpers [1] = (guint32) (intptr_t) mono_wasm_jit_scratch;
	helpers [2] = (guint32) (intptr_t) mono_wasm_jit_vcall_aot_target;
	helpers [3] = (guint32) (intptr_t) mono_wasm_jit_call_interp;
	helpers [4] = (guint32) (intptr_t) mono_wasm_jit_continue_unwind;
	wasm_buf_init (&out);
	wasm_module_vslow (ft, retnorm, slot, helpers, &out);
	{ extern int mono_wasm_jit_capture;
	  if (G_UNLIKELY (mono_wasm_jit_capture))
		mono_wasm_jit_capture_module ("v", "wasmjit-vslow", out.data, out.len); }
	s = g_new0 (WjVslowStub, 1);
	s->ft = *ft;
	s->retnorm = retnorm;
	s->slot = slot;
	s->len = (int) out.len;
	s->bytes = (guint8 *) g_malloc (out.len);
	memcpy (s->bytes, out.data, out.len);
	wasm_buf_free (&out);
	/* Invariant 4: validate and install here before anything can name it. Catch this pthread up first, so `done`
	 * stays a prefix count. A stub V8 refuses is a codegen defect: refuse the site, count it, never publish. */
	wj_vslow_ensure_all ();
	if (wj_vslow_done != wj_vslow_n || !wj_vslow_instantiate (s)) {
		mono_wasm_jit_counters [WJC_VSLOW_REFUSED]++;
		return 0;   /* the slot and the bytes leak (invariant 5) */
	}
	id = wj_vslow_n;
	wj_vslow_stubs [id] = s;
	mono_memory_barrier ();
	wj_vslow_hash [i] = (gint16) (id + 1);
	wj_vslow_n = id + 1;
	wj_vslow_done = id + 1;
	mono_wasm_jit_counters [WJC_VSLOW_STUBS]++;
	return slot;
}

/* R459 diagnostic: after an instantiation failed on a helper import ("h" "<index>"), print what C believes the index
 * is -- a stub (which, and whether this pthread counts it as instantiated), a descriptor's f- or e-slot -- next to the
 * JS side's view, which instantiate_local appends to the error. Failure path only. */
static void
wj_vslow_explain (const char *err)
{
	const char *p = err ? strstr (err, "\"h\" \"") : NULL;
	int idx, k, n = wj_vslow_n, kind = -1, jd;
	if (!p)
		return;
	idx = atoi (p + 5);
	for (k = 0; k < n; ++k)
		if (wj_vslow_stubs [k] && wj_vslow_stubs [k]->slot == idx)
			break;
	jd = wj_fixed_index_jit_desc (idx, &kind);
	printf ("WASM_JIT_VSLOW_EXPLAIN idx=%d stub=%d of %d done=%d desc_for_fslot=%d fixed_jit_desc=%d kind=%d\n",
		idx, k < n ? k : -1, n, wj_vslow_done, wj_desc_for_fslot (idx), jd, kind);
}

/* R459: an instantiation failed with a LinkError on a helper import that is a STUB's slot -- this pthread's table did not
 * hold the stub there although invariant 1 says it must (C1's first outcome batch lost an arm to exactly that, cause not
 * yet known). Print both sides' view, re-install the stub here, and let the caller retry once. Counted, never silent:
 * repair MUST read 0. Returns TRUE iff a stub was re-installed. Failure path only. */
static gboolean
wj_vslow_repair (const char *err)
{
	const char *p = err ? strstr (err, "\"h\" \"") : NULL;
	int idx, k, n = wj_vslow_n;
	if (!p)
		return FALSE;
	idx = atoi (p + 5);
	for (k = 0; k < n; ++k)
		if (wj_vslow_stubs [k] && wj_vslow_stubs [k]->slot == idx)
			break;
	if (k == n)
		return FALSE;
	wj_vslow_explain (err);
	/* from the import name on: the head carries "LinkError", which the harness's verdict classes as a TERMINAL fault
	 * (lib/verdict.mjs wasm-link) -- the run this repair keeps alive must not be ended by its own report */
	if ((mono_wasm_jit_counters [WJC_VSLOW_REPAIR]++) < 20)
		printf ("WASM_JIT_VSLOW_REPAIR stub=%d slot=%d : %s\n", k, idx, p);
	return wj_vslow_instantiate (wj_vslow_stubs [k]);
}

/* A site's descriptor (invariant 5: never freed). Compile section only. */
static WjVslowDesc *
wj_vslow_desc (gpointer vic, gpointer aic, MonoMethod *base, int site, int ways, int mid_slot)
{
	WjVslowDesc *d = g_new0 (WjVslowDesc, 1);
	d->vic = vic;
	d->aic = aic;
	d->base = base;
	d->site = site;
	d->ways = ways;
	d->mid_slot = mid_slot;
	return d;
}

#endif /* HOST_BROWSER */
