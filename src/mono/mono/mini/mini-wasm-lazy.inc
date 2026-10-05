/*
 * PHASE 5: LAZY TIER-1 F-SLOTS (R361-R366; scratchpad/wj/phase5-brief.md).
 *
 * A direct call to a callee that is not JITted yet used to be a compile failure ("callee not jitted (residual off)"),
 * answered by the island machinery force-compiling the callee's whole closure first -- ~17k methods a run compiled
 * that way, most at one hit (R347/R348), and deleted in R366. Now the call site RESERVES the callee an e/f
 * pair from a per-functype POOL and bakes the f-slot as an ordinary direct call. Until the callee is compiled and
 * admitted on a worker, that worker's table holds a STUB at the f-slot (wasm_module_lazy_bank, wasm-encoder.c):
 *
 *   bind (slot)   = mono_wasm_jit_late_fslot on the owner -- counts the call toward the callee's own threshold,
 *                   compiles it when that is reached, and admits it here if it is registered. A live f-slot comes
 *                   back and the stub tail-calls it; admission has already overwritten the stub, so later calls
 *                   from this worker never see it again.
 *   otherwise     the callee runs in the interpreter through mono_wasm_jit_call_interp, exactly the marshal the
 *                   residual route uses, and the stub stays.
 *
 * THE INVARIANTS, each one the reason a line below exists:
 *  1. A pool f-slot holds, on every worker that can reach it, either its stub or the callee's REAL f whose own
 *     closure is admitted there. So a caller's admission needs no recursion into a pool dep, only its bank
 *     (wj_lazy_ensure_bank), and the REAL f is written into a pool slot by admission alone, after its dependency
 *     walk: the compiling worker validates without installing (the R322 path), and alias writes follow a
 *     successful admission of the aliased slot.
 *  2. Stubs are per pthread in the sense that matters: a bank's stubs bake no TLS (scratch comes from a helper,
 *     never s.b), and "bank instantiated here" is __thread, so a pthread that takes over a worker re-instantiates
 *     every bank it needs and the active element segments put every stub back (R293).
 *  3. A pool slot is never credited no-GC (wj_fslot_is_nogc): the stub's bind can compile, instantiate and run a
 *     cctor, so a call through a pool slot is a GC point on any worker that has not bound it yet.
 *  4. Writers (reservation, pool refill, bank publication) run only inside the compile section, which wj_compiling
 *     serialises; readers are lock-free and see contents before the pointer or count that publishes them.
 */

/* A lazy pool slot? Answered for the non-browser build too, because wj_fslot_is_nogc compiles in both. */
#ifndef HOST_BROWSER
static gboolean wj_lazy_is_pool_slot (int slot) { (void) slot; return FALSE; }
static gboolean wj_lazy_dep_needs_real (int slot) { (void) slot; return FALSE; }
#else

#define WJ_LAZY_MAX_POOLS   2048
#define WJ_LAZY_POOL_HASH   4096          /* open addressing over wj_functype_hash, power of two */
#define WJ_LAZY_MAX_BANKS   16384
/* Pairs per bank: the unused tail of a functype's last bank is wasted table space, and the JIT_CALL table is shared
 * with everything the tier compiles (p82: 56,913 of 131,072 entries used without this). Small banks bound the waste
 * at WJ_LAZY_K_MAX pairs per functype; the price is more, smaller bank modules per worker. */
#define WJ_LAZY_K_MIN       4
#define WJ_LAZY_K_MAX       16

typedef struct {
	WasmFuncType ft;
	int k;
	int e0;              /* pair i = (e0 + 2i, e0 + 2i + 1): e immediately before f, as every slot-pair reader assumes */
	guint8 *bytes;       /* the bank module; immutable, never freed */
	int len;
	int hook_f;          /* plan H5: a one-slot bank framed over a HOOKED method's own f-slot (not a pool), 0 = a pool bank */
	int prev_bank;       /* H5: index + 1 of the pool bank this hook bank displaced at hook_f, ensured first (invariant 1) */
} WjLazyBank;

typedef struct {
	WasmFuncType ft;
	int bank;            /* the bank pairs are handed out of, -1 = none yet */
	int next;            /* next unused pair in it */
	int k_next;          /* size of the next refill: doubles, so a hot functype does not fragment into small banks */
} WjLazyPool;

typedef struct {
	MonoMethod *method;
	gpointer imethod;    /* InterpMethod, opaque here as everywhere in this file */
	gint32 bank;         /* bank index + 1; 0 = not a pool slot */
} WjLazySlot;

static WjLazyPool wj_lazy_pools [WJ_LAZY_MAX_POOLS];
static int wj_lazy_npools;
static gint16 wj_lazy_pool_hash [WJ_LAZY_POOL_HASH];   /* pool index + 1 */
static WjLazyBank *volatile wj_lazy_banks [WJ_LAZY_MAX_BANKS];
static volatile int wj_lazy_nbanks;
/* f-slot -> owner, chunked like wj_fslot_desc_chunks. A chunk is published zeroed before its pointer. */
static WjLazySlot *volatile wj_lazy_slot_chunks [WJ_SLOT_NCHUNKS];
/* Banks THIS pthread instantiated, one bit each (invariant 2). */
static __thread guint32 wj_lazy_bank_done [WJ_LAZY_MAX_BANKS / 32];

static WjLazySlot *
wj_lazy_slot (int slot)
{
	WjLazySlot *c;
	if (slot <= 0 || slot / WJ_SLOT_CHUNK >= WJ_SLOT_NCHUNKS)
		return NULL;
	c = wj_lazy_slot_chunks [slot / WJ_SLOT_CHUNK];
	if (!c)
		return NULL;
	mono_memory_barrier ();
	c = &c [slot % WJ_SLOT_CHUNK];
	return c->bank > 0 ? c : NULL;
}

static gboolean
wj_lazy_is_pool_slot (int slot)
{
	return wj_lazy_slot (slot) != NULL;
}

/* Instantiate a bank on THIS pthread if it has not: its active segments write every stub into its slots.
 * Runs GC-safe across the JS work like every instantiation here (R162). FALSE only if V8 refused the module. */
static gboolean
wj_lazy_ensure_bank (int bank)
{
	WjLazyBank *b;
	int ok;
	if (bank < 0 || bank >= wj_lazy_nbanks)
		return FALSE;
	if (wj_lazy_bank_done [bank / 32] & (1u << (bank % 32)))
		return TRUE;
	mono_memory_barrier ();
	b = wj_lazy_banks [bank];
	if (!b)
		return FALSE;
	/* H5: the pool bank a hook bank displaced still covers this slot; instantiated after the real f, its segment would
	 * write its old stub back (invariant 1), so it goes in first, here, like any slot's bank. */
	if (b->prev_bank > 0 && !wj_lazy_ensure_bank (b->prev_bank - 1))
		return FALSE;
	{ extern int mono_wasm_jit_bytes_check;
	  if (G_UNLIKELY (mono_wasm_jit_bytes_check))
		wj_bytes_check (b->bytes, b->len, "l"); }   /* R462 */
	WJ_JS_BLOCKING_BEGIN (__wj_cookie, __wj_sd, __wj_entered);
	ok = EM_ASM_INT ({
		var p = $0 < 0 ? $0 + 4294967296 : $0;
		try {
			/* The same resolver instantiate_local builds, created the same way if a bank is this worker's first
			 * module: a later instantiate_local must find __wjSlotFn, or the worker-reuse reset misses its slots. */
			if (!Module.__wjHelperImports) {
				Module.__wjSlotFn = new Map ();
				Module.__wjSlotChanged = 0;
				Module.__wjHelperImports = new Proxy ({}, { get: function (t, k) {
					if (typeof k !== "string" || !/^[0-9]+$/.test (k)) return undefined;
					return wasmTable.get (Number (k));
				} });
			}
			/* A hook bank's segment overwrites a slot that may hold a real function with ALIASES (bind's table copies):
			 * re-point them here, since every later reader of the slot sees only the stub. */
			var prev = $2 > 0 ? wasmTable.get ($2) : null;
			new WebAssembly.Instance (new WebAssembly.Module (HEAPU8.slice (p, p + $1)), { m: { h: wasmMemory }, f: { f: wasmTable },
				s: { p: wasmExports["__stack_pointer"], l: 0, c: 0, v: 0, n: 0, d: 0, m: 0, b: 0, i: 0, g: 0 }, h: Module.__wjHelperImports });
			/* H5: a hook bank's stub, kept per worker so a later hook re-installs it with a table write rather than
			 * another instance (wj_hook_restub_local). The stub bakes no TLS, so it outlives the pthread. */
			if ($2 > 0) {
				var stub = wasmTable.get ($2);
				(Module.__wjStubFn || (Module.__wjStubFn = new Map ())).set ($2, stub);
				if (prev && prev !== stub && Module.__wjAliasRepoint)
					Module.__wjAliasRepoint ($2, prev, stub);
			}
			return 1;
		} catch (e) {
			if ((Module.__wjLazyBankErr = (Module.__wjLazyBankErr | 0) + 1) <= 8)
				console.log ("WASM_JIT_LAZY_BANK_FAIL " + e);
			return 0;
		}
	}, (int) (intptr_t) b->bytes, b->len, b->hook_f);
	WJ_JS_BLOCKING_END (__wj_cookie, __wj_sd, __wj_entered);
	if (!ok) {
		mono_wasm_jit_counters [WJC_LAZY_BANK_FAIL]++;
		return FALSE;
	}
	wj_lazy_bank_done [bank / 32] |= 1u << (bank % 32);
	mono_wasm_jit_counters [WJC_LAZY_BANK_INST]++;
	return TRUE;
}

/* Every path that writes a REAL function into a table slot calls this first (instantiate_local/_batch_local): if the
 * slot is a pool slot, its bank must already be instantiated on this pthread, or a later instantiation of the bank
 * would write the stub back over the real f -- and with admission's cache saying "live", bind would hand the stub its
 * own slot to tail-call, forever. */
static void
wj_lazy_ensure_bank_of_slot (int slot)
{
	WjLazySlot *ls = wj_lazy_slot (slot);
	if (ls)
		wj_lazy_ensure_bank (ls->bank - 1);
}

/* Is table[slot] the real function this worker installed there? Repairs it if something else overwrote it.
 * __wjSlotFn is the worker's record of every real install (and of every alias, below). The ordering rule above
 * makes a repair unreachable; the check is what turns a violation of it into a count instead of a hang. */
static gboolean
wj_lazy_table_real (int slot)
{
	int r = EM_ASM_INT ({
		var m = Module.__wjSlotFn;
		var fn = m && m.get ($0);
		if (!fn) return 0;
		if (wasmTable.get ($0) !== fn) { wasmTable.set ($0, fn); return 2; }
		return 1;
	}, slot);
	if (r == 2)
		mono_wasm_jit_counters [WJC_LAZY_REPAIR]++;
	return r != 0;
}

/* The functype a pool f-slot's stub was framed for. */
static gboolean
wj_lazy_pool_ft (int slot, WasmFuncType *out)
{
	WjLazySlot *ls = wj_lazy_slot (slot);
	if (!ls)
		return FALSE;
	*out = wj_lazy_banks [ls->bank - 1]->ft;
	return TRUE;
}

/* Invariant 3's exception: a callee registered no-GC AT this pool slot is admitted for real by its callers' admission
 * (not left to its stub), so a caller may be credited for it. Registered-at-this-slot, not merely registered: a callee
 * that landed under another pair is reached through the stub's alias, which a credit cannot wait for. */
static gboolean
wj_lazy_dep_needs_real (int slot)
{
	extern int mono_wasm_jit_lazy_nogc;
	int d;
	WjRegEntry *re;
	if (!mono_wasm_jit_lazy_nogc || !wj_lazy_is_pool_slot (slot))
		return FALSE;
	d = wj_desc_for_fslot (slot);
	re = d > 0 ? wj_reg_at (d - 1) : NULL;
	return re && re->f == slot && re->no_gc;
}

/* Admission's answer for a pool dep (invariant 1): callable here once its bank is. */
static gboolean
wj_lazy_dep_ok (int slot)
{
	WjLazySlot *ls = wj_lazy_slot (slot);
	if (!ls)
		return FALSE;
	mono_wasm_jit_counters [WJC_LAZY_DEP_STUB]++;
	return wj_lazy_ensure_bank (ls->bank - 1);
}

WJ_KEEPALIVE int mono_wasm_jit_lazy_bind (int slot);
WJ_KEEPALIVE int mono_wasm_jit_lazy_interp (int slot, guint8 *buf);

/* A pool for `ft`, created on first use. */
static WjLazyPool *
wj_lazy_pool_for (const WasmFuncType *ft)
{
	guint32 h = wj_functype_hash (ft), i, k;
	for (k = 0; k < WJ_LAZY_POOL_HASH; ++k) {
		i = (h + k) & (WJ_LAZY_POOL_HASH - 1);
		if (!wj_lazy_pool_hash [i]) {
			WjLazyPool *p;
			if (wj_lazy_npools >= WJ_LAZY_MAX_POOLS)
				return NULL;
			p = &wj_lazy_pools [wj_lazy_npools];
			p->ft = *ft;
			p->bank = -1;
			p->next = 0;
			p->k_next = WJ_LAZY_K_MIN;
			wj_lazy_pool_hash [i] = (gint16) ++wj_lazy_npools;
			return p;
		}
		if (functype_eq (&wj_lazy_pools [wj_lazy_pool_hash [i] - 1].ft, ft))
			return &wj_lazy_pools [wj_lazy_pool_hash [i] - 1];
	}
	return NULL;
}

/* A fresh bank of p->k_next pairs for pool p: one contiguous block of table entries, the module framed over its
 * f-slots, published before anything can name a slot in it. */
static gboolean
wj_lazy_refill (WjLazyPool *p)
{
	extern int mono_jiterp_allocate_table_entries (int type, int n);
	extern gpointer mono_wasm_jit_scratch (void);
	extern void mono_wasm_jit_continue_unwind (void);
	guint32 helpers [4];
	int *fs, i, k = p->k_next, e0, id;
	WjLazyBank *b;
	WasmBuf out;
	if (wj_lazy_nbanks >= WJ_LAZY_MAX_BANKS)
		return FALSE;
	e0 = mono_jiterp_allocate_table_entries (1 /* JITERPRETER_TABLE_JIT_CALL */, 2 * k);
	if (e0 <= 0) {
		mono_wasm_jit_counters [WJC_LAZY_REFUSE_FULL]++;
		return FALSE;
	}
	helpers [0] = (guint32) (intptr_t) mono_wasm_jit_lazy_bind;
	helpers [1] = (guint32) (intptr_t) mono_wasm_jit_scratch;
	helpers [2] = (guint32) (intptr_t) mono_wasm_jit_lazy_interp;
	helpers [3] = (guint32) (intptr_t) mono_wasm_jit_continue_unwind;
	fs = g_new (int, k);
	for (i = 0; i < k; ++i)
		fs [i] = e0 + 2 * i + 1;
	wasm_buf_init (&out);
	wasm_module_lazy_bank (&p->ft, fs, (guint32) k, helpers, &out);
	g_free (fs);
	{ extern int mono_wasm_jit_capture;
	  if (G_UNLIKELY (mono_wasm_jit_capture))
		mono_wasm_jit_capture_module ("l", "wasmjit-lazy-bank", out.data, out.len); }
	b = g_new0 (WjLazyBank, 1);
	b->ft = p->ft;
	b->k = k;
	b->e0 = e0;
	b->len = (int) out.len;
	b->bytes = (guint8 *) g_malloc (out.len);
	memcpy (b->bytes, out.data, out.len);
	wasm_buf_free (&out);
	id = wj_lazy_nbanks;
	wj_lazy_banks [id] = b;
	mono_memory_barrier ();
	wj_lazy_nbanks = id + 1;
	p->bank = id;
	p->next = 0;
	if (p->k_next < WJ_LAZY_K_MAX)
		p->k_next *= 2;
	mono_wasm_jit_counters [WJC_LAZY_BANKS]++;
	mono_wasm_jit_counters [WJC_LAZY_POOL_SLOTS] += 2 * k;
	return TRUE;
}

/* Why this call site's callee cannot get a pool slot, or NULL if it can. A pure predicate -- no reservation, no
 * InterpMethod created, no signature resolved (m->signature must already be there). `counter` names the refusal. */
static const char *
wj_lazy_refusal (MonoMethod *m, MonoMethodSignature *csig, gboolean rgctx, const WasmFuncType *ct, int *counter)
{
	extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
	extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
	const char *ns;
	WasmCallInfo cci;
	*counter = WJC_LAZY_REFUSE_SHAPE;
	if (!m || !csig || rgctx)
		return "rgctx";
	/* The OUTER synchronized wrapper compiles as itself and registers under its own InterpMethod, so it can take a
	 * pool pair like any method. So does a DYNAMIC METHOD (IKVM's DynamicBinder and MemberName adapters, R364): the
	 * owner map retains its MonoMethod exactly as the registry already does, and bind/interp check the freed-method
	 * set before using it. Every other wrapper takes the residual -- the synchronized INNER one registers under a
	 * substituted body (mono_wasm_force_compile). */
	*counter = WJC_LAZY_REFUSE_WRAPPER;
	if (m->wrapper_type != MONO_WRAPPER_NONE && m->wrapper_type != MONO_WRAPPER_SYNCHRONIZED &&
	    m->wrapper_type != MONO_WRAPPER_DYNAMIC_METHOD)
		return "wrapper";
	*counter = WJC_LAZY_REFUSE_SHAPE;
	if ((m->iflags & (METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED | METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)) ||
	    (m->flags & (METHOD_ATTRIBUTE_PINVOKE_IMPL | METHOD_ATTRIBUTE_ABSTRACT)))
		return "shape";
	*counter = WJC_LAZY_REFUSE_REFLECT;
	ns = m_class_get_name_space (m->klass);
	if (ns && !strncmp (ns, "System.Reflection", 17))
		return "reflection";   /* the residual's own rule: running one via interp_entry mid-Finish re-enters it */
	*counter = WJC_LAZY_REFUSE_UNUSABLE;
	if (!mono_wasm_jit_method_usable (m, WJ_BADMETH_SITE_PEEK))
		return "unusable";
	*counter = WJC_LAZY_REFUSE_AOT;
	if (mono_interp_jit_call_supported (m, csig))
		return "aot";          /* keeps its inline direct AOT call */
	*counter = WJC_LAZY_REFUSE_PERM;
	if (mono_wasm_jit_callee_perm_unjittable (m))
		return "perm";
	/* EXACT functype, not a hash (R234): the callee compiles against its own signature, and a stub, unlike an
	 * admission refusal, cannot decline a caller whose baked type differs -- it would trap. */
	*counter = WJC_LAZY_REFUSE_SIG;
	if (!m->signature)
		return "sig unresolved";
	mono_wasm_get_call_info (m->signature, &cci);
	if (!cci.valid || !functype_eq (&cci.ftype, ct))
		return "sig";
	return NULL;
}

/* The f-slot a devirt or delegate ARM calls its predicted target through (R363): the target's own if it has one, else
 * a pool slot; 0 drops the arm. The arm's caller checks the target's ABI against the site's functype; the refusal
 * checks it again, exactly, because a stub cannot decline a call the way the old admission refusal did. */
static int
wj_lazy_arm_fslot (MonoMethod *target, MonoMethod *self, const WasmFuncType *ftd)
{
	int f = target ? mono_wasm_jit_get_callee_fslot (target) : 0;
	if (f > 0 || !target || target == self || !target->signature)
		return f;
	f = wj_lazy_reserve (target, target->signature, FALSE, ftd);
	if (f > 0)
		mono_wasm_jit_counters [WJC_LAZY_ARM]++;
	return f;
}

/* The call site's get-or-reserve. Returns the callee's pool f-slot, or 0 (counted) to take the old path. */
static int
wj_lazy_reserve (MonoMethod *m, MonoMethodSignature *csig, gboolean rgctx, const WasmFuncType *ct)
{
	extern gpointer mono_wasm_jit_lazy_im (MonoMethod *m, int *fslot, int *self_resv_f);
	extern void mono_wasm_jit_lazy_im_reserve (gpointer im, int e, int f);
	gpointer im;
	WjLazyPool *p;
	WjLazySlot *c;
	int counter, e, f, ci, im_f = 0, im_self = 0;
	if (wj_lazy_refusal (m, csig, rgctx, ct, &counter)) {
		mono_wasm_jit_counters [counter]++;
		return 0;
	}
	/* Creates the InterpMethod on a miss, which is the one creation this path genuinely needs (CLAUDE.md: the
	 * reservation lives on it). The emitter's heal block made the same call for the same callees. */
	im = mono_wasm_jit_lazy_im (m, &im_f, &im_self);
	if (!im)
		return 0;
	if (im_f > 0)
		return im_f;                 /* registered since get_callee_fslot looked */
	if (im_self > 0) {
		if (wj_lazy_is_pool_slot (im_self)) {
			mono_wasm_jit_counters [WJC_LAZY_REUSE]++;
			return im_self;
		}
		mono_wasm_jit_counters [WJC_LAZY_REFUSE_RESV]++;   /* a self-recursion pair: no stub behind it */
		return 0;
	}
	p = wj_lazy_pool_for (ct);
	if (!p) {
		mono_wasm_jit_counters [WJC_LAZY_REFUSE_FULL]++;
		return 0;
	}
	if ((p->bank < 0 || p->next >= wj_lazy_banks [p->bank]->k) && !wj_lazy_refill (p))
		return 0;
	e = wj_lazy_banks [p->bank]->e0 + 2 * p->next;
	f = e + 1;
	ci = f / WJ_SLOT_CHUNK;
	if (ci >= WJ_SLOT_NCHUNKS) {
		mono_wasm_jit_counters [WJC_LAZY_REFUSE_FULL]++;
		return 0;
	}
	p->next++;
	if (!wj_lazy_slot_chunks [ci]) {
		WjLazySlot *nc = g_new0 (WjLazySlot, WJ_SLOT_CHUNK);
		mono_memory_barrier ();
		wj_lazy_slot_chunks [ci] = nc;
	}
	c = &wj_lazy_slot_chunks [ci][f % WJ_SLOT_CHUNK];
	c->method = m;
	c->imethod = im;
	mono_memory_barrier ();
	c->bank = p->bank + 1;
	mono_wasm_jit_lazy_im_reserve (im, e, f);
	mono_wasm_jit_counters [WJC_LAZY_RESERVE]++;
	return f;
}

/*
 * PLAN H5: route a hooked method's f-slot to its stub. Called inside the compile section (invariant 4) by the hook
 * path (mono_wasm_jit_hook_route). The first hook of a descriptor frames a one-slot bank over its OWN f-slot -- not a
 * pool pair: callers baked this slot -- and every later hook reuses it. The slot's owner record then makes `f` a pool
 * slot for good: callers' admission ensures the bank instead of recursing into the descriptor (invariant 1 holds:
 * the slot holds the stub or the real f, which admission writes after its walk), and it is never credited no-GC.
 */
static gboolean
wj_lazy_hook_bank (WjRegEntry *re, MonoMethod *m, gpointer imethod, const WasmFuncType *ft)
{
	extern gpointer mono_wasm_jit_scratch (void);
	extern void mono_wasm_jit_continue_unwind (void);
	guint32 helpers [4];
	int f = re->f, ci, id;
	WjLazySlot *c;
	if (f <= 0 || (ci = f / WJ_SLOT_CHUNK) >= WJ_SLOT_NCHUNKS)
		return FALSE;
	if (!re->hook_bank) {
		WjLazyBank *b;
		WasmBuf out;
		if (wj_lazy_nbanks >= WJ_LAZY_MAX_BANKS)
			return FALSE;
		helpers [0] = (guint32) (intptr_t) mono_wasm_jit_lazy_bind;
		helpers [1] = (guint32) (intptr_t) mono_wasm_jit_scratch;
		helpers [2] = (guint32) (intptr_t) mono_wasm_jit_lazy_interp;
		helpers [3] = (guint32) (intptr_t) mono_wasm_jit_continue_unwind;
		wasm_buf_init (&out);
		wasm_module_lazy_bank (ft, &f, 1, helpers, &out);
		b = g_new0 (WjLazyBank, 1);
		b->ft = *ft;
		b->k = 1;
		b->e0 = re->e;
		b->hook_f = f;
		{   /* a pool slot already: its bank keeps covering it (see wj_lazy_ensure_bank) */
			WjLazySlot *old = wj_lazy_slot (f);
			b->prev_bank = old ? old->bank : 0;
		}
		b->len = (int) out.len;
		b->bytes = (guint8 *) g_malloc (out.len);
		memcpy (b->bytes, out.data, out.len);
		wasm_buf_free (&out);
		id = wj_lazy_nbanks;
		wj_lazy_banks [id] = b;
		mono_memory_barrier ();
		wj_lazy_nbanks = id + 1;
		re->hook_bank = id + 1;
		mono_wasm_jit_counters [WJC_LAZY_BANKS]++;
	}
	if (!wj_lazy_slot_chunks [ci]) {
		WjLazySlot *nc = g_new0 (WjLazySlot, WJ_SLOT_CHUNK);
		mono_memory_barrier ();
		wj_lazy_slot_chunks [ci] = nc;
	}
	c = &wj_lazy_slot_chunks [ci][f % WJ_SLOT_CHUNK];
	c->method = m;
	c->imethod = imethod;
	mono_memory_barrier ();
	c->bank = re->hook_bank;
	return TRUE;
}

/* H5: route `desc_id` (a live JIT generation of `m`, whose current InterpMethod is `imethod`) to its hook stub. Inside
 * the compile section (interp.c mono_wasm_jit_hook_publish). 1 = routed, every worker adopts at its next drain;
 * 0 = not a routable generation; -1 = could not (MUST READ 0: the old body stays in the tables). */
int mono_wasm_jit_hook_route_locked (int desc_id, MonoMethod *m, gpointer imethod);
int
mono_wasm_jit_hook_route_locked (int desc_id, MonoMethod *m, gpointer imethod)
{
	WjRegEntry *re;
	MonoMethodSignature *sig;
	WasmCallInfo cci;
	if (desc_id <= 0 || desc_id > wj_reg_n || !(re = wj_reg_at (desc_id - 1)) || re->orphaned || re->f <= 0)
		return 0;
	/* The stub must be of exactly the functype callers baked for this f-slot. */
	sig = m ? mono_method_signature_internal (m) : NULL;
	if (!sig)
		goto fail;
	mono_wasm_get_call_info (sig, &cci);
	if (!cci.valid || wj_functype_hash (&cci.ftype) != re->f_sig_id)
		goto fail;
	if (!wj_lazy_hook_bank (re, m, imethod, &cci.ftype))
		goto fail;
	mono_loader_lock ();
	re->hook_payload = wj_payload_new ();
	re->body_current = 0;
	mono_memory_barrier ();
	re->hooked = 1;
	/* B4 leaf copies (R452): every group carrying a private copy of this body calls it module-locally too */
	{ extern void mono_wasm_jit_b4_copy_invalidate (int fslot);
	  mono_wasm_jit_b4_copy_invalidate (re->f); }
	/* A co-located method is also called module-locally by its group; the group's guard word sends those calls through
	 * the table, i.e. to the stub (plan H4: hookable apps build guarded groups, S1 bit 1). */
	if (re->batch) {
		if (re->batch->guard) {
			mono_atomic_store_i32 (re->batch->guard, 1);
			mono_wasm_jit_counters [WJC_HOOK_GROUP_GUARDED]++;
		} else {
			mono_wasm_jit_counters [WJC_HOOK_GROUP_UNGUARDED]++;
		}
	}
	mono_memory_barrier ();
	re->generation = (guint32) mono_atomic_inc_i32 (&wj_batch_generation);
	mono_loader_unlock ();
	mono_wasm_jit_counters [WJC_HOOK_ROUTED]++;
	return 1;
fail:
	mono_wasm_jit_counters [WJC_HOOK_ROUTE_FAIL]++;
	return -1;
}

/* Put the hook's stub into THIS worker's table at the hooked f-slot, retire the e-slot here, re-point aliases. */
static gboolean
wj_hook_restub_local (WjRegEntry *re)
{
	int b = re->hook_bank - 1, ok;
	guint32 hp = re->hook_payload;
	if (b < 0 || !hp)
		return FALSE;
	if (wj_slot_payload_at (re->f) == hp && !wj_slot_is_installed (re->e))
		return TRUE;
	if (!wj_lazy_ensure_bank (b))
		return FALSE;
	ok = EM_ASM_INT ({
		var s = Module.__wjStubFn && Module.__wjStubFn.get ($0);
		if (!s)
			return 0;
		var old = wasmTable.get ($0);
		if (old !== s) {
			wasmTable.set ($0, s);
			if (Module.__wjAliasRepoint)
				Module.__wjAliasRepoint ($0, old, s);
		}
		if (Module.__wjSlotFn)
			Module.__wjSlotFn.set ($0, s);
		return 1;
	}, re->f);
	if (!ok)
		return FALSE;
	wj_mark_slot_installed (re->f);
	wj_slot_payload_set (re->f, hp);
	wj_mark_slot_live (re->f);
	/* The entry thunk calls the OLD f module-locally: not live here, so no interpreter entry takes it. */
	wj_clear_slot_installed (re->e);
	mono_wasm_jit_counters [WJC_HOOK_RESTUB]++;
	return TRUE;
}

/*
 * The stub's bind (wasm_module_lazy_bank). Returns the f-slot to tail-call through, or 0 after preparing the
 * callee for the interpreter leg. Everything here may collect: the stub has pinned its i32 params.
 */
WJ_KEEPALIVE int
mono_wasm_jit_lazy_bind (int slot)
{
	extern int mono_wasm_jit_late_fslot (gpointer imethod);
	extern void mono_wasm_jit_pretransform (MonoMethod *method);
	WjLazySlot *ls = wj_lazy_slot (slot);
	int f;
	mono_wasm_jit_counters [WJC_LAZY_BIND]++;
	if (G_UNLIKELY (!ls || !ls->imethod)) {
		mono_wasm_jit_counters [WJC_LAZY_ORPHAN]++;
		return 0;
	}
	/* A freed owner (a collected dynamic method) cannot be bound or interpreted, and a caller reaching its slot at
	 * all means something kept it callable past its death: fail loudly rather than run another method's code. */
	if (G_UNLIKELY (mono_wasm_jit_method_known_dead (ls->method, -1))) {
		mono_wasm_jit_counters [WJC_LAZY_DEAD]++;
		g_error ("wasm-jit lazy: f-slot %d's owner was freed", slot);
	}
	/* H5: a hooked method's slot. Admission would hand back this very slot (it holds the stub), so run the hook's IL in
	 * the interpreter until the registration of the new body clears the flag. */
	{
		int hd = wj_desc_for_fslot (slot);
		WjRegEntry *hre = hd > 0 ? wj_reg_at (hd - 1) : NULL;
		if (hre && hre->f == slot && hre->hooked) {
			mono_wasm_jit_counters [WJC_HOOK_BIND_INTERP]++;
			mono_wasm_jit_pretransform (ls->method);
			return 0;
		}
	}
	/* This pthread may be running another pthread's stub (a worker take-up, R293): own the bank before admission
	 * below writes a real f into its slot. */
	wj_lazy_ensure_bank (ls->bank - 1);
	f = mono_wasm_jit_late_fslot (ls->imethod);
	if (f > 0 && !wj_lazy_table_real (f)) {
		mono_wasm_jit_counters [WJC_LAZY_BIND_NOTREAL]++;
		f = 0;
	}
	if (f > 0) {
		int d;
		WjRegEntry *re;
		if (f == slot) {
			mono_wasm_jit_counters [WJC_LAZY_BIND_REAL]++;
			return slot;
		}
		/* Registered under another pair (an SCC batch's, or a drifted signature's). Its f is live here, so if it
		 * is the same method under the same functype, point this slot at it too: admission did the walk for it. */
		d = wj_desc_for_fslot (f);
		re = d > 0 ? wj_reg_at (d - 1) : NULL;
		if (re && (re->logical_method == ls->method || re->body_method == ls->method) &&
		    re->f_sig_id == wj_functype_hash (&wj_lazy_banks [ls->bank - 1]->ft)) {
			/* Recorded per real f-slot, so a republication there re-points the alias too (H3): it is a COPY of the
			 * function object, and before this a worker kept calling the displaced body through it for good. Only an
			 * alias still holding the displaced function moves; one a take-up reset or bind re-pointed elsewhere is
			 * dropped from the list instead. Per worker, like the table. */
			EM_ASM ({
				var fn = wasmTable.get ($1);
				wasmTable.set ($0, fn);
				if (Module.__wjSlotFn) Module.__wjSlotFn.set ($0, fn);   /* so a worker take-up resets it (R293) */
				if (!Module.__wjAliasOf) {
					Module.__wjAliasOf = new Map ();
					Module.__wjAliasRepoints = 0;
					Module.__wjAliasRepoint = function (f, oldfn, newfn) {
						var al = Module.__wjAliasOf.get (f);
						if (!al || oldfn === newfn) return;
						var i = 0;
						while (i < al.length) {
							if (oldfn && wasmTable.get (al[i]) === oldfn) {
								wasmTable.set (al[i], newfn);
								if (Module.__wjSlotFn) Module.__wjSlotFn.set (al[i], newfn);
								Module.__wjAliasRepoints++;
								i++;
							} else {
								al[i] = al[al.length - 1];
								al.pop ();
							}
						}
					};
				}
				var al = Module.__wjAliasOf.get ($1);
				if (!al) Module.__wjAliasOf.set ($1, al = []);
				if (al.indexOf ($0) < 0) al.push ($0);
			}, slot, f);
			wj_slot_payload_set (slot, wj_slot_payload_at (f));   /* W1 C0: the alias holds f's module */
			mono_wasm_jit_counters [WJC_LAZY_BIND_ALIAS]++;
			return f;
		}
		mono_wasm_jit_counters [WJC_LAZY_BIND_OTHER_SIG]++;
	}
	/* The residual's discipline: transform (and run any cctor) BEFORE the arguments go into GC-invisible scratch. */
	mono_wasm_jit_pretransform (ls->method);
	return 0;
}

/*
 * The stub's interpreter leg. The stub stored its wasm params at buf + 8*j, which is the residual's layout for
 * the call arguments (cci.nargs of them); what it cannot know from a functype is the managed shape, so the two
 * places that differ are fixed here from the signature: a hidden vret pointer (the LAST wasm param) moves to
 * WJ_SCRATCH_VRET_OFF, and a sub-word return is widened the way the residual's emitted code widens it.
 */
WJ_KEEPALIVE int
mono_wasm_jit_lazy_interp (int slot, guint8 *buf)
{
	extern int mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf);
	WjLazySlot *ls = wj_lazy_slot (slot);
	MonoMethodSignature *sig;
	WasmCallInfo cci;
	int threw;
	mono_wasm_jit_counters [WJC_LAZY_INTERP]++;
	if (G_UNLIKELY (!ls || !ls->method))
		g_error ("wasm-jit lazy: interp leg for f-slot %d has no owner", slot);
	if (G_UNLIKELY (mono_wasm_jit_method_known_dead (ls->method, -1))) {
		mono_wasm_jit_counters [WJC_LAZY_DEAD]++;
		g_error ("wasm-jit lazy: f-slot %d's owner was freed", slot);
	}
	sig = mono_method_signature_internal (ls->method);
	mono_wasm_get_call_info (sig, &cci);
	/* The scratch holds the arguments laid out for the bank's functype; marshalling them by another is corruption.
	 * Only a method reallocated at a freed owner's address could differ here. */
	if (G_UNLIKELY (!cci.valid || !functype_eq (&cci.ftype, &wj_lazy_banks [ls->bank - 1]->ft))) {
		mono_wasm_jit_counters [WJC_LAZY_DEAD]++;
		g_error ("wasm-jit lazy: f-slot %d's owner no longer has the stub's functype", slot);
	}
	if (cci.vret_byaddr)
		*(gint32 *) (buf + 232) = *(gint32 *) (buf + 8 * cci.nargs);   /* WJ_SCRATCH_VRET_OFF */
	threw = mono_wasm_jit_call_interp (ls->method, buf);
	if (threw) {
		mono_wasm_jit_counters [WJC_LAZY_INTERP_THREW]++;
		return threw;
	}
	if (cci.ftype.ret == WASM_I32 && !m_type_is_byref (sig->ret)) {
		gint32 *r = (gint32 *) (buf + 192);                              /* WJ_SCRATCH_RET_OFF */
		switch (mini_get_underlying_type (sig->ret)->type) {
		case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: *r &= 0xff; break;
		case MONO_TYPE_I1: *r = (gint8) *r; break;
		case MONO_TYPE_CHAR: case MONO_TYPE_U2: *r &= 0xffff; break;
		case MONO_TYPE_I2: *r = (gint16) *r; break;
		default: break;
		}
	}
	return 0;
}

#endif /* HOST_BROWSER */
