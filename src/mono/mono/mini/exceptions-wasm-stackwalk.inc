/*
 * exceptions-wasm-stackwalk.inc -- the managed stack walk, asked of V8 (HOST_BROWSER only; #included by exceptions-wasm.c).
 *
 * wasm has no native unwinding, so mono's walker (unwinder_unwind_frame) sees only what is linked into the LMF chain:
 * interpreter segments, and the IL_STATE records of methods WITH clauses (llvm-only deopt, the wasm JIT's WjEhRec).
 * Every JIT or AOT frame without clauses was invisible -- and every consumer (CoreLib's StackFrame/StackTrace, IKVM's
 * getCallerClass/StackWalker/exception traces) assumes it sees every frame. V8 does see every wasm frame, including the
 * ones it inlined itself and across JSPI stack switches, so a walk asks it once (one EM_JS call) and merges the answer
 * with the LMF chain, which still owns the interpreter's frames: those are data, and appear to V8 only as
 * mono_interp_exec_method activations. Nothing on any hot path changes; a walk costs ~90-130 us in a 26.2 boot (~60-90
 * V8 frames each, `[wasm-jit stackwalk]` us/walks at the title, 2026-10-24), ~2% of boot (R516).
 *
 * THE MERGE, top of stack first:
 *   - a JIT frame (an instance this worker registered at instantiation, keyed by the module's payload id) is emitted
 *     with its member's method; an AOT frame (a main-module function whose index maps to an AOT method's table index)
 *     with its jinfo's;
 *   - each emitted frame claims the LMF item it owns, if that is next: its own EH record (JIT: matched by the body
 *     InterpMethod, not the method, because of recursion; AOT: by method), and its own phantom (interp.c,
 *     interp_lmf_mark_phantom: an INTERP_EXIT standing in for a callee that is not interpreted);
 *   - a mono_interp_exec_method activation splices the next interpreter segment. Items still unclaimed before it are
 *     dropped and counted: a phantom whose callee never ran (it was being compiled), a record whose frame is gone.
 * The ONLY hard invariant is that segments pair with exec activations in order. Anything that breaks it -- an exec
 * activation with no segment, a segment left over -- returns -1 and the caller takes the legacy walk, counted by
 * reason. Frame identity always comes from V8; the LMF supplies only interpreter frames and block IL offsets.
 */

#include <emscripten.h>
#include "wasm-encoder.h"
#include <mono/utils/mono-time.h>
#include <mono/utils/mono-tls-inline.h>

/* MONO_WASM_STACKWALK_V8: 1 = current-thread walks ask V8 (this file); 0 = the LMF-only walk every consumer had before.
 * MONO_WASM_STACKWALK_VERIFY: 1 = after each merged walk, run the legacy one too and count whether every frame it found
 * is in the merged list, in order (mini-exceptions.c). Diagnostic: the legacy walk compiles inside the walk. */
int mono_wasm_stackwalk_v8 = 1;
int mono_wasm_stackwalk_verify = 0;
/* MONO_WASM_STACKWALK_VERBOSE: >= 1 prints the first mismatches with both frame lists and the first distinct JIT frames
 * that found no call site (WASM_STACKWALK_SITE_MISS); >= 2 every walk. */
static int wjsw_verbose = 0;
static volatile gint32 wjsw_knobs_read;

static void
wjsw_read_knobs (void)
{
	const char *v;
	if (wjsw_knobs_read)
		return;
	if ((v = g_getenv ("MONO_WASM_STACKWALK_V8")) && *v)
		mono_wasm_stackwalk_v8 = atoi (v) ? 1 : 0;
	if ((v = g_getenv ("MONO_WASM_STACKWALK_VERIFY")) && *v)
		mono_wasm_stackwalk_verify = atoi (v) ? 1 : 0;
	if ((v = g_getenv ("MONO_WASM_STACKWALK_VERBOSE")) && *v)
		wjsw_verbose = atoi (v);
	mono_memory_barrier ();
	wjsw_knobs_read = 1;
}

int
mono_wasm_stackwalk_verify_on (void)
{
	wjsw_read_knobs ();
	return mono_wasm_stackwalk_verify;
}

/* --- census: bumped ungated, printed as [wasm-jit stackwalk] ------------------------------------------------------- */

enum {
	WJSW_C_WALKS, WJSW_C_FRAMEINFO, WJSW_C_TRACE, WJSW_C_WALKFULL, WJSW_C_EXC,   /* consumers: MONO_WASM_SW_C_* */
	WJSW_C_CAPTURES, WJSW_C_RECAPTURE, WJSW_C_V8_FRAMES, WJSW_C_OUT_FRAMES, WJSW_C_JIT, WJSW_C_AOT, WJSW_C_INTERP,
	WJSW_C_EXEC, WJSW_C_PHANTOM_OK, WJSW_C_PHANTOM_DROP, WJSW_C_REC_OK, WJSW_C_REC_DROP, WJSW_C_PLAIN_DROP,
	WJSW_C_IFR_DUP, WJSW_C_CLAUSE_DUP, WJSW_C_THUNK, WJSW_C_US,
	/* fallbacks, by reason -- every one is a walk that got the legacy answer */
	WJSW_C_F_OFF, WJSW_C_F_DEPTH, WJSW_C_F_CAPTURE, WJSW_C_F_NOREC, WJSW_C_F_NOSEG, WJSW_C_F_EXTRASEG, WJSW_C_F_BADLMF,
	WJSW_C_F_AOTBUSY, WJSW_C_F_SIZE,
	WJSW_C_AOT_NOTED, WJSW_C_AOT_MAPPED, WJSW_C_AOT_AMBIG, WJSW_C_AOT_BUILDS, WJSW_C_AOT_US,
	WJSW_C_MODS, WJSW_C_MODS_BAD, WJSW_C_VERIFY_OK, WJSW_C_VERIFY_FAIL,
	WJSW_C_SITES, WJSW_C_SITE_KEYED, WJSW_C_SITE_HIT, WJSW_C_SITE_KEY, WJSW_C_SITE_MISS, WJSW_C_INLINED,
	WJSW_C_N
};
static gint64 wjsw_c [WJSW_C_N];
g_static_assert (WJSW_C_FRAMEINFO == MONO_WASM_SW_C_FRAMEINFO && WJSW_C_TRACE == MONO_WASM_SW_C_TRACE &&
                 WJSW_C_WALKFULL == MONO_WASM_SW_C_WALKFULL && WJSW_C_EXC == MONO_WASM_SW_C_EXC);
#define WJSW_COUNT(k) (wjsw_c [(k)]++)

void
mono_wasm_stackwalk_verify_result (int ok)
{
	WJSW_COUNT (ok ? WJSW_C_VERIFY_OK : WJSW_C_VERIFY_FAIL);
}

void
mono_wasm_stackwalk_print_stats (void)
{
	gint64 *c = wjsw_c;
	/* `walks` is the liveness check (0 = the V8 path never ran: knob off, or nothing walked); the f_* fields are walks
	 * that FELL BACK to the legacy answer, by reason, and are what to read first; verify_fail MUST read 0 under VERIFY. */
	printf ("[wasm-jit stackwalk] v8=%d walks=%lld (frameinfo=%lld trace=%lld walkfull=%lld exc=%lld) captures=%lld recapture=%lld"
		" v8_frames=%lld out=%lld (jit=%lld aot=%lld interp=%lld) exec=%lld us=%lld"
		" | phantom ok=%lld drop=%lld rec ok=%lld drop=%lld plain_drop=%lld ifr_dup=%lld clause_dup=%lld thunk=%lld"
		" | fallback off=%lld depth=%lld capture=%lld norec=%lld noseg=%lld extraseg=%lld badlmf=%lld aotbusy=%lld size=%lld"
		" | aot noted=%lld mapped=%lld ambig=%lld builds=%lld us=%lld | modules=%lld bad=%lld | verify ok=%lld fail=%lld"
		" | jit sites=%lld keyed=%lld hit=%lld key=%lld miss=%lld inlined_frames=%lld\n",
		mono_wasm_stackwalk_v8, c [WJSW_C_WALKS], c [WJSW_C_FRAMEINFO], c [WJSW_C_TRACE], c [WJSW_C_WALKFULL], c [WJSW_C_EXC],
		c [WJSW_C_CAPTURES], c [WJSW_C_RECAPTURE], c [WJSW_C_V8_FRAMES], c [WJSW_C_OUT_FRAMES], c [WJSW_C_JIT], c [WJSW_C_AOT],
		c [WJSW_C_INTERP], c [WJSW_C_EXEC], c [WJSW_C_US],
		c [WJSW_C_PHANTOM_OK], c [WJSW_C_PHANTOM_DROP], c [WJSW_C_REC_OK], c [WJSW_C_REC_DROP], c [WJSW_C_PLAIN_DROP],
		c [WJSW_C_IFR_DUP], c [WJSW_C_CLAUSE_DUP], c [WJSW_C_THUNK],
		c [WJSW_C_F_OFF], c [WJSW_C_F_DEPTH], c [WJSW_C_F_CAPTURE], c [WJSW_C_F_NOREC], c [WJSW_C_F_NOSEG],
		c [WJSW_C_F_EXTRASEG], c [WJSW_C_F_BADLMF], c [WJSW_C_F_AOTBUSY], c [WJSW_C_F_SIZE],
		c [WJSW_C_AOT_NOTED], c [WJSW_C_AOT_MAPPED], c [WJSW_C_AOT_AMBIG], c [WJSW_C_AOT_BUILDS], c [WJSW_C_AOT_US],
		c [WJSW_C_MODS], c [WJSW_C_MODS_BAD], c [WJSW_C_VERIFY_OK], c [WJSW_C_VERIFY_FAIL],
		c [WJSW_C_SITES], c [WJSW_C_SITE_KEYED], c [WJSW_C_SITE_HIT], c [WJSW_C_SITE_KEY], c [WJSW_C_SITE_MISS],
		c [WJSW_C_INLINED]);
}

/* --- JIT module records, keyed by payload id -----------------------------------------------------------------------
 *
 * A payload id names one set of module bytes, process-wide and never reused (mini-wasm.c wj_payload_new); every
 * instantiation of those bytes, on any worker, is passed it, and registers its WebAssembly.Instance -> payload in the
 * worker's WeakMap (Module.__wjsw.inst). So a V8 frame names its record exactly, including a generation-1 body still
 * running after its f-slot was republished -- the f-slot could not tell them apart. Records are never freed: the
 * frames that name them can be on any stack, and they are ~16 bytes a member. */

typedef struct {
	MonoWasmSwInl inl;
	MonoJitInfo *volatile ji;
} WjswInl;

typedef struct {
	guint32 code_off;        /* the member's f body: V8's code.offset(), the start of its locals declaration */
	guint32 body_start;      /* code_off + its locals declaration: where the serialized body (the call-site offsets) starts */
	MonoMethod *method;
	gpointer body_imethod;   /* the InterpMethod it was compiled against (superseded test, EH record match), or NULL */
	MonoJitInfo *volatile ji;   /* synthetic, method only, made on first use: consumers want a jinfo per frame */
	/* Call sites (mini-wasm-ir.inc wj_assemble -> wasm_body_serialize): body offset of each call opcode, increasing,
	 * with the (IL offset, inline index) it was emitted for, and its key (wasm-encoder.h WasmSites.kd), or NULL. */
	guint32 nsites;
	guint32 *site_off;
	gint32 *site_il;
	guint16 *site_inl;
	guint16 *site_kd;
	int ninl;
	WjswInl *inl;            /* [1..ninl] */
} WjswMember;

typedef struct {
	int n;
	WjswMember m [MONO_ZERO_LEN_ARRAY];
} WjswModRec;

#define WJSW_REC_CHUNK_BITS 12
#define WJSW_REC_CHUNKS 4096
static WjswModRec **volatile wjsw_rec_chunks [WJSW_REC_CHUNKS];

static WjswModRec *
wjsw_rec_for_payload (guint32 p)
{
	guint32 c = p >> WJSW_REC_CHUNK_BITS;
	WjswModRec **chunk;
	if (!p || c >= WJSW_REC_CHUNKS)
		return NULL;
	chunk = wjsw_rec_chunks [c];
	return chunk ? chunk [p & ((1u << WJSW_REC_CHUNK_BITS) - 1)] : NULL;
}

static int
wjsw_leb (const guint8 *b, int len, int p, guint32 *out)
{
	guint32 v = 0;
	int shift = 0;
	while (p < len && shift < 35) {
		guint8 x = b [p++];
		v |= (guint32) (x & 0x7f) << shift;
		if (!(x & 0x80)) {
			*out = v;
			return p;
		}
		shift += 7;
	}
	return -1;
}

/* The code offsets (V8's code.offset(): just past each body's size LEB) of the first `max` defined functions. */
static int
wjsw_code_offsets (const guint8 *b, int len, guint32 *out, int max)
{
	int p = 8;
	if (!b || len < 8 || b [0] != 0 || b [1] != 'a' || b [2] != 's' || b [3] != 'm')
		return -1;
	while (p < len) {
		guint8 id = b [p++];
		guint32 sz, cnt, i;
		int end, k = 0;
		if ((p = wjsw_leb (b, len, p, &sz)) < 0 || (gint64) p + sz > len)
			return -1;
		end = p + (int) sz;
		if (id != 10) {
			p = end;
			continue;
		}
		if ((p = wjsw_leb (b, end, p, &cnt)) < 0)
			return -1;
		for (i = 0; i < cnt && k < max; ++i) {
			guint32 bsz;
			if ((p = wjsw_leb (b, end, p, &bsz)) < 0 || (gint64) p + bsz > end)
				return -1;
			out [k++] = (guint32) p;
			p += (int) bsz;
		}
		return k;
	}
	return -1;
}

/* The length of the locals declaration at `p` (a function body's start). */
static int
wjsw_locals_len (const guint8 *b, int len, int p)
{
	guint32 groups, i, cnt;
	int q = wjsw_leb (b, len, p, &groups);
	for (i = 0; q >= 0 && i < groups; ++i) {
		q = wjsw_leb (b, len, q, &cnt);
		if (q < 0 || q >= len)
			return -1;
		q++;   /* the value type: one byte for every type this emitter declares */
	}
	return q < 0 ? -1 : q - p;
}

/* Called where a JIT module's payload id is minted (mini-wasm-emitter.inc, mini-wasm-batching.inc), with the members
 * in assembly order -- their f bodies are the module's first `n` defined functions. `sites` / `inl_tabs` / `ninl`
 * (each may be NULL, as may any entry) are the members' call sites and inlinees; both are copied. */
void
mono_wasm_stackwalk_module_note (guint32 payload, int n, MonoMethod *const *methods, gpointer const *body_imethods,
                                 const struct _WasmSites *const *sites, const void *const *inl_tabs, const int *ninl,
                                 const guint8 *bytes, int len)
{
	guint32 c = payload >> WJSW_REC_CHUNK_BITS, *offs;
	WjswModRec *rec, **chunk;
	int i, k;
	if (!payload || n <= 0 || c >= WJSW_REC_CHUNKS)
		return;
	offs = g_new (guint32, n);
	k = wjsw_code_offsets (bytes, len, offs, n);
	if (k != n) {
		WJSW_COUNT (WJSW_C_MODS_BAD);
		g_free (offs);
		return;
	}
	rec = (WjswModRec *) g_malloc0 (sizeof (WjswModRec) + sizeof (WjswMember) * (gsize) n);
	rec->n = n;
	for (i = 0; i < n; ++i) {
		WjswMember *mem = &rec->m [i];
		const WasmSites *st = sites ? sites [i] : NULL;
		const MonoWasmSwInl *tab = inl_tabs ? (const MonoWasmSwInl *) inl_tabs [i] : NULL;
		int ll = wjsw_locals_len (bytes, len, (int) offs [i]);
		mem->code_off = offs [i];
		mem->body_start = ll >= 0 ? offs [i] + (guint32) ll : 0;
		mem->method = methods [i];
		mem->body_imethod = body_imethods ? body_imethods [i] : NULL;
		if (st && st->n && ll >= 0) {
			mem->nsites = st->n;
			mem->site_off = (guint32 *) g_memdup (st->off, (guint) (sizeof (guint32) * st->n));
			mem->site_il = (gint32 *) g_memdup (st->il, (guint) (sizeof (gint32) * st->n));
			mem->site_inl = (guint16 *) g_memdup (st->inl, (guint) (sizeof (guint16) * st->n));
			wjsw_c [WJSW_C_SITES] += st->n;
			if (st->kd) {
				guint32 j, nk = 0;
				for (j = 0; j < st->n; ++j)
					nk += st->kd [j] != 0;
				if (nk) {
					mem->site_kd = (guint16 *) g_memdup (st->kd, (guint) (sizeof (guint16) * st->n));
					wjsw_c [WJSW_C_SITE_KEYED] += nk;
				}
			}
		}
		if (tab && ninl && ninl [i] > 0) {
			int k;
			mem->ninl = ninl [i];
			mem->inl = g_new0 (WjswInl, ninl [i] + 1);
			for (k = 1; k <= ninl [i]; ++k)
				mem->inl [k].inl = tab [k];
		}
	}
	g_free (offs);
	chunk = wjsw_rec_chunks [c];
	if (!chunk) {
		WjswModRec **fresh = g_new0 (WjswModRec *, 1u << WJSW_REC_CHUNK_BITS);
		if (mono_atomic_cas_ptr ((gpointer volatile *) &wjsw_rec_chunks [c], fresh, NULL) != NULL)
			g_free (fresh);
		chunk = wjsw_rec_chunks [c];
	}
	mono_memory_barrier ();   /* the record's contents before the pointer that makes it reachable */
	chunk [payload & ((1u << WJSW_REC_CHUNK_BITS) - 1)] = rec;
	WJSW_COUNT (WJSW_C_MODS);
}

static WjswMember *
wjsw_member_at (WjswModRec *rec, guint32 code_off)
{
	int lo = 0, hi = rec->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (rec->m [mid].code_off == code_off)
			return &rec->m [mid];
		if (rec->m [mid].code_off < code_off)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;   /* an entry thunk: not a managed frame */
}

/* The call site at body offset `rel` (exact: V8 reports a call frame at its call opcode). FALSE = none recorded. */
static gboolean
wjsw_member_site (WjswMember *m, guint32 rel, gint32 *il, guint16 *inl, gboolean *by_key)
{
	int lo = 0, hi = (int) m->nsites - 1;
	*by_key = FALSE;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (m->site_off [mid] == rel) {
			*il = m->site_il [mid];
			*inl = m->site_inl [mid];
			return TRUE;
		}
		if (m->site_off [mid] < rel)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	/* Not a call: V8 positions no call inside a `try` of its own frame, and reports the last positioned
	 * instruction before it instead. A key lies between the previous site and its own, so the only site whose key
	 * `rel` can be is the first one after it. */
	if (m->site_kd && lo < (int) m->nsites && m->site_kd [lo] && m->site_off [lo] - m->site_kd [lo] == rel) {
		*il = m->site_il [lo];
		*inl = m->site_inl [lo];
		*by_key = TRUE;
		return TRUE;
	}
	return FALSE;
}

static MonoJitInfo *
wjsw_synth_ji (MonoJitInfo *volatile *slot, MonoMethod *method, gpointer code)
{
	MonoJitInfo *ji = *slot;
	if (!ji) {
		ji = (MonoJitInfo *) g_malloc0 (mono_jit_info_size ((MonoJitInfoFlags) 0, 0, 0));
		mono_jit_info_init (ji, method, (guint8 *) code, 1, (MonoJitInfoFlags) 0, 0, 0);
		if (mono_atomic_cas_ptr ((gpointer volatile *) slot, ji, NULL) != NULL) {
			g_free (ji);
			ji = *slot;
		}
	}
	return ji;
}

static MonoJitInfo *
wjsw_member_ji (WjswMember *m)
{
	/* code_start is the member record: unique, and never a real code address, so nothing maps an ip into it */
	return wjsw_synth_ji (&m->ji, m->method, m);
}

/* --- AOT: main-module function index -> table index --------------------------------------------------------------
 *
 * V8 names a main-module frame by function index; mono finds an AOT method by its function-table index (that is what
 * a function pointer is on wasm: aot-runtime.c's methods[] / mini_jit_info_table_find). The only map between the two
 * is the table itself: an exported function's .name is its function index. So each loaded AOT image's table indices are
 * noted here (register_methods_in_jinfo) and mapped in ONE bulk JS call the first time a walk needs them -- ~120 k
 * entries, once a process. binaryen can merge identical bodies, so one function index may serve two methods: such
 * an entry is AMBIGUOUS and resolves to nothing (counted), rather than to the wrong method. */

typedef struct {
	int n;
	gint32 *fidx;   /* sorted */
	gint32 *ti;     /* -1 = ambiguous */
} WjswAotMap;

static WjswAotMap *volatile wjsw_aot_map;
static gint32 *wjsw_aot_pend;
static int wjsw_aot_npend, wjsw_aot_cpend;
static volatile gint32 wjsw_aot_pend_lock, wjsw_aot_build_lock;

void
mono_wasm_stackwalk_note_aot_methods (gpointer const *table_indices, int n)
{
	int i;
	if (n <= 0)
		return;
	/* Tiny critical section, taken a few dozen times a run (once per AOT image): test-and-test-and-set, bounded. */
	for (i = 0; i < 100000000; ++i)
		if (!wjsw_aot_pend_lock && mono_atomic_cas_i32 (&wjsw_aot_pend_lock, 1, 0) == 0)
			break;
	if (wjsw_aot_npend + n > wjsw_aot_cpend) {
		wjsw_aot_cpend = MAX (wjsw_aot_cpend * 2, wjsw_aot_npend + n);
		wjsw_aot_pend = (gint32 *) g_realloc (wjsw_aot_pend, sizeof (gint32) * (gsize) wjsw_aot_cpend);
	}
	for (i = 0; i < n; ++i)
		wjsw_aot_pend [wjsw_aot_npend++] = GPOINTER_TO_INT (table_indices [i]);
	wjsw_c [WJSW_C_AOT_NOTED] += n;
	mono_atomic_store_i32 (&wjsw_aot_pend_lock, 0);
}

EM_JS (void, wjsw_js_funcidx, (int tis, int n, int out), {
	var a = (tis < 0 ? tis + 4294967296 : tis) / 4;
	var o = (out < 0 ? out + 4294967296 : out) / 4;
	var i, f, fi;
	for (i = 0; i < n; i++) {
		fi = -1;
		try {
			f = wasmTable.get (HEAP32 [a + i]);
			if (f)
				fi = Number (f.name);
		} catch (e) {
			fi = -1;
		}
		HEAP32 [o + i] = (fi >= 0) ? fi : -1;
	}
});

typedef struct { gint32 f, t; } WjswPair;

static int
wjsw_pair_cmp (const void *a, const void *b)
{
	gint32 x = ((const WjswPair *) a)->f, y = ((const WjswPair *) b)->f;
	return x < y ? -1 : x > y ? 1 : 0;
}

/* Map whatever AOT images were noted since the last call. FALSE = some are pending and another thread is mapping
 * them right now: the caller must not walk with an incomplete map (its AOT frames would vanish), so it falls back. */
static gboolean
wjsw_aot_ensure (void)
{
	gint32 *tis, *fis;
	WjswPair *pairs;
	WjswAotMap *old, *nm;
	int n, i, j, k, np;
	gint64 t0;
	if (!wjsw_aot_npend)
		return TRUE;
	if (wjsw_aot_build_lock || mono_atomic_cas_i32 (&wjsw_aot_build_lock, 1, 0) != 0)
		return FALSE;
	for (i = 0; i < 100000000; ++i)
		if (!wjsw_aot_pend_lock && mono_atomic_cas_i32 (&wjsw_aot_pend_lock, 1, 0) == 0)
			break;
	tis = wjsw_aot_pend;
	n = wjsw_aot_npend;
	wjsw_aot_pend = NULL;
	wjsw_aot_npend = wjsw_aot_cpend = 0;
	mono_atomic_store_i32 (&wjsw_aot_pend_lock, 0);
	if (!n) {
		mono_atomic_store_i32 (&wjsw_aot_build_lock, 0);
		return TRUE;
	}
	t0 = mono_100ns_ticks ();
	fis = g_new (gint32, n);
	wjsw_js_funcidx ((int) (gsize) tis, n, (int) (gsize) fis);
	old = wjsw_aot_map;
	pairs = g_new (WjswPair, (gsize) n + (old ? old->n : 0));
	np = 0;
	if (old)
		for (i = 0; i < old->n; ++i) {
			pairs [np].f = old->fidx [i];
			pairs [np++].t = old->ti [i];
		}
	for (i = 0; i < n; ++i)
		if (fis [i] >= 0) {
			pairs [np].f = fis [i];
			pairs [np++].t = tis [i];
		}
	qsort (pairs, (size_t) np, sizeof (WjswPair), wjsw_pair_cmp);
	nm = g_new0 (WjswAotMap, 1);
	nm->fidx = g_new (gint32, MAX (np, 1));
	nm->ti = g_new (gint32, MAX (np, 1));
	for (i = 0, k = 0; i < np; i = j) {
		gint32 t = pairs [i].t;
		for (j = i + 1; j < np && pairs [j].f == pairs [i].f; ++j)
			if (pairs [j].t != t)
				t = -1;
		if (t == -1 && pairs [i].t != -1)
			WJSW_COUNT (WJSW_C_AOT_AMBIG);
		nm->fidx [k] = pairs [i].f;
		nm->ti [k++] = t;
	}
	nm->n = k;
	g_free (pairs);
	g_free (fis);
	g_free (tis);
	mono_memory_barrier ();
	wjsw_aot_map = nm;   /* the old map is leaked: a concurrent walk may be reading it */
	wjsw_c [WJSW_C_AOT_MAPPED] = k;
	WJSW_COUNT (WJSW_C_AOT_BUILDS);
	wjsw_c [WJSW_C_AOT_US] += (mono_100ns_ticks () - t0) / 10;
	mono_atomic_store_i32 (&wjsw_aot_build_lock, 0);
	return TRUE;
}

/* The AOT jinfo of main-module function `fidx`, or NULL for a native (C) function or an ambiguous index. */
static MonoJitInfo *
wjsw_aot_ji (gint32 fidx)
{
	WjswAotMap *m = wjsw_aot_map;
	int lo = 0, hi;
	MonoJitInfo *ji;
	if (!m)
		return NULL;
	hi = m->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (m->fidx [mid] == fidx) {
			if (m->ti [mid] < 0)
				return NULL;
			ji = mini_jit_info_table_find (GINT_TO_POINTER (m->ti [mid]));
			return (ji && !ji->is_trampoline && !ji->async) ? ji : NULL;
		}
		if (m->fidx [mid] < fidx)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* --- the capture ------------------------------------------------------------------------------------------------- *
 *
 * One CallSite per V8 frame, ~0.25 us each (scratchpad/wj/stackwalk/bench.mjs). For a wasm frame getThis() is its
 * WebAssembly.Instance, getEnclosingColumnNumber() the function's code offset, getPosition() the call's module offset
 * (checked on V8 14.6, stackwalk/probe.mjs). Per frame written: [kind, a, code offset, position], kind 1 = JIT
 * (a = payload id), 2 = main module (a = function index, or a negative anchor: -2 mono_interp_exec_method, -3
 * mini_llvmonly_resume_exception_il_state, -4 interp_run_clause_with_il_state). Everything else -- JS, jiterpreter
 * modules, lazy stub banks -- is skipped. The main module is classified once per function per worker. Anchors are
 * NEVER_INLINE or address-taken in the served wasm; a plain static may be inlined away and is not one.
 * Returns the frames written, -1 if the capture failed, -2 if `cap` was too small. *total = V8's frame count, which
 * equals `limit` when the capture was truncated. */
EM_JS (int, wjsw_js_capture, (int buf, int cap, int limit, int total), {
	var st = Module.__wjsw;
	if (!st)
		st = Module.__wjsw = { inst: new WeakMap (), main: null, cls: new Map () };
	var sp = Error.prepareStackTrace, sl = Error.stackTraceLimit, cs = null, h = {};
	try {
		Error.stackTraceLimit = limit;
		Error.prepareStackTrace = function (e, s) { return s; };
		Error.captureStackTrace (h);
		cs = h.stack;
	} catch (e) {
		cs = null;
	} finally {
		Error.prepareStackTrace = sp;
		Error.stackTraceLimit = sl;
	}
	if (!cs || typeof cs.length !== "number")
		return -1;
	var p = (buf < 0 ? buf + 4294967296 : buf) / 4;
	HEAP32 [(total < 0 ? total + 4294967296 : total) / 4] = cs.length;
	var n = 0, i, c, t, pl, encl, k, s, x, nm;
	for (i = 0; i < cs.length; i++) {
		c = cs [i];
		try {
			t = c.getThis ();
		} catch (e) {
			t = undefined;
		}
		if (!t || typeof t !== "object")
			continue;
		pl = st.inst.get (t);
		if (pl === undefined) {
			if (st.main === null) {
				s = c.getFileName ();
				if (typeof s !== "string" || s.indexOf ("wasm://wasm/dotnet.native.wasm") !== 0)
					continue;
				st.main = t;
			} else if (t !== st.main) {
				continue;
			}
		}
		encl = c.getEnclosingColumnNumber ();
		k = 0;
		if (pl === undefined) {
			k = st.cls.get (encl);
			if (k === undefined) {
				nm = c.getFunctionName () || "";
				if (nm === "mono_interp_exec_method")
					k = -2;
				else if (nm === "mini_llvmonly_resume_exception_il_state")
					k = -3;
				else if (nm === "interp_run_clause_with_il_state")
					k = -4;
				else {
					s = String (c.toString ());
					x = s.lastIndexOf ("wasm-function[");
					k = x >= 0 ? parseInt (s.substring (x + 14), 10) : -1;
					if (!(k >= 0))
						k = -1;
				}
				st.cls.set (encl, k);
			}
			if (k === -1)
				continue;
		}
		if (n >= cap)
			return -2;
		HEAP32 [p + 4 * n] = pl === undefined ? 2 : 1;
		HEAP32 [p + 4 * n + 1] = pl === undefined ? k : pl;
		HEAP32 [p + 4 * n + 2] = encl;
		HEAP32 [p + 4 * n + 3] = c.getPosition ();
		n++;
	}
	return n;
});

#define WJSW_A_EXEC (-2)
#define WJSW_A_RESUME (-3)
#define WJSW_A_CLAUSE (-4)

/* --- the LMF side --------------------------------------------------------------------------------------------- */

enum { WJSW_L_SEG = 1, WJSW_L_ILSTATE, WJSW_L_PHANTOM, WJSW_L_PLAIN };

typedef struct {
	int kind;
	int first, count;            /* SEG: frames [first, first + count) of the scratch's interpreter frames */
	MonoMethod *method;          /* ILSTATE / PHANTOM / PLAIN */
	MonoMethodILState *il;       /* ILSTATE */
	gpointer ehbody;             /* ILSTATE: the JIT record's body InterpMethod; NULL for an AOT il_state */
} WjswItem;

typedef struct {
	gint32 *v8;            int v8_cap;     /* 4 ints a frame */
	WjswItem *items;       int items_cap;
	StackFrameInfo *ifr;   int ifr_cap;
	guint8 *ifr_dup;       int ifr_dup_cap;
	MonoWasmSwFrame *out;  int out_cap;
	gpointer *hset;        int hset_cap;
} WjswScratch;

#define WJSW_MAX_DEPTH 4
#define WJSW_MAX_FRAMES 8192
static __thread int wjsw_depth;
static __thread WjswScratch *wjsw_scratch [WJSW_MAX_DEPTH];

gboolean mono_interp_imethod_superseded (gpointer imethod);   /* interp.c */
gpointer mono_interp_ehrec_body (MonoMethodILState *il);     /* interp.c */
gpointer mono_interp_frame_imethod (gpointer interp_frame);  /* interp.c */
int mono_interp_frame_il_offset (gpointer interp_frame);     /* interp.c */

#define WJSW_GROW(arr, cap, need, type) do { \
	if ((need) > (cap)) { \
		int _nc = MAX ((cap) * 2, MAX ((need), 64)); \
		(arr) = (type *) g_realloc ((arr), sizeof (type) * (gsize) _nc); \
		(cap) = _nc; \
	} } while (0)

/* The LMF chain, top first, as items; interpreter segments expanded through the interpreter's own iterator. Reads
 * only -- unlike the legacy unwinder, which compiles inside the walk (compile_interp_method for an IL_STATE, a full
 * compile for a plain LMF). Returns the item count, or -1 on a chain it does not understand. */
static int
wjsw_read_lmf (WjswScratch *s, int *out_nifr)
{
	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
	MonoLMF *lmf = mono_get_lmf ();
	int ni = 0, nf = 0, guard = 0;
	while (lmf && (!jit_tls || lmf != jit_tls->first_lmf)) {
		gsize prev = (gsize) lmf->previous_lmf;
		WjswItem *it;
		if (++guard > WJSW_MAX_FRAMES)
			return -1;
		WJSW_GROW (s->items, s->items_cap, ni + 1, WjswItem);
		it = &s->items [ni];
		memset (it, 0, sizeof (*it));
		if (prev & 2) {
			MonoLMFExt *ext = (MonoLMFExt *) lmf;
			if (ext->kind == MONO_LMFEXT_INTERP_EXIT || ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
				if (ext->lmf.method) {
					it->kind = WJSW_L_PHANTOM;
					it->method = ext->lmf.method;
				} else {
					it->kind = WJSW_L_SEG;
					it->first = nf;
					if (ext->interp_exit_data) {
						MonoInterpStackIter iter;
						StackFrameInfo f;
						mini_get_interp_callbacks ()->frame_iter_init (&iter, ext->interp_exit_data);
						while (mini_get_interp_callbacks ()->frame_iter_next (&iter, &f)) {
							if (nf >= WJSW_MAX_FRAMES)
								return -1;
							WJSW_GROW (s->ifr, s->ifr_cap, nf + 1, StackFrameInfo);
							s->ifr [nf++] = f;
						}
					}
					it->count = nf - it->first;
				}
				ni++;
			} else if (ext->kind == MONO_LMFEXT_IL_STATE) {
				if (!ext->il_state)
					return -1;
				it->kind = WJSW_L_ILSTATE;
				it->il = ext->il_state;
				it->method = ext->il_state->method;
				it->ehbody = mono_interp_ehrec_body (ext->il_state);
				ni++;
			} else if (ext->kind != MONO_LMFEXT_DEBUGGER_INVOKE && ext->kind != MONO_LMFEXT_JIT_ENTRY) {
				return -1;
			}
		} else {
			/* A plain LMF (the needs_LMF path). The all-zero one is the chain's terminal sentinel (mono_arch_unwind_frame). */
			if (!lmf->previous_lmf && !lmf->lmf_addr && !lmf->method)
				break;
			if (!wasm_lmf_method_plausible (lmf->method))
				return -1;
			it->kind = WJSW_L_PLAIN;
			it->method = lmf->method;
			ni++;
		}
		lmf = (MonoLMF *) (prev & ~(gsize) 3);
	}
	*out_nifr = nf;
	return ni;
}

/* Interpreter frames reached twice: a finally re-enters its own frame (interp_run_finally) and a filter parents a
 * copy on it (interp_run_filter), so a NESTED activation's segment carries the frames of the one below it again. Each
 * frame is kept only in the LOWEST segment that has it, i.e. where it physically is. */
static void
wjsw_mark_dups (WjswScratch *s, int nitems, int nifr)
{
	int cap = 64, i, j, mask;
	while (cap < nifr * 2)
		cap *= 2;
	WJSW_GROW (s->hset, s->hset_cap, cap, gpointer);
	memset (s->hset, 0, sizeof (gpointer) * (gsize) cap);
	WJSW_GROW (s->ifr_dup, s->ifr_dup_cap, nifr, guint8);
	mask = cap - 1;
	for (i = nitems - 1; i >= 0; --i) {
		WjswItem *it = &s->items [i];
		if (it->kind != WJSW_L_SEG)
			continue;
		for (j = it->first; j < it->first + it->count; ++j) {
			gpointer f = s->ifr [j].interp_frame;
			guint32 h = ((guint32) (gsize) f >> 3) & (guint32) mask;
			s->ifr_dup [j] = 0;
			while (s->hset [h] && s->hset [h] != f)
				h = (h + 1) & (guint32) mask;
			if (s->hset [h] == f)
				s->ifr_dup [j] = 1;
		}
		for (j = it->first; j < it->first + it->count; ++j) {
			gpointer f = s->ifr [j].interp_frame;
			guint32 h = ((guint32) (gsize) f >> 3) & (guint32) mask;
			while (s->hset [h] && s->hset [h] != f)
				h = (h + 1) & (guint32) mask;
			s->hset [h] = f;
		}
	}
}

static MonoWasmSwFrame *
wjsw_emit (WjswScratch *s, int *n)
{
	MonoWasmSwFrame *f;
	WJSW_GROW (s->out, s->out_cap, *n + 1, MonoWasmSwFrame);
	f = &s->out [(*n)++];
	memset (f, 0, sizeof (*f));
	return f;
}

static void
wjsw_fill_managed (MonoWasmSwFrame *f, int src, MonoJitInfo *ji, MonoMethod *m)
{
	f->src = src;
	f->sfi.type = FRAME_TYPE_MANAGED;
	f->sfi.ji = ji;
	f->sfi.method = f->sfi.actual_method = m;
	f->sfi.managed = m && (!m->wrapper_type || m->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD);
	f->sfi.native_offset = -1;
	f->sfi.il_offset = -1;
}

/* The same frame's method, possibly under two generic identities: AOT code runs a SHARED instantiation
 * (MapException<T_REF>), and its EH record or phantom names the instance (MapException<java.lang.Exception>). */
gboolean
mono_wasm_stackwalk_same_method (MonoMethod *a, MonoMethod *b)
{
	if (a == b)
		return TRUE;
	if (!a || !b || !a->is_inflated || !b->is_inflated)
		return FALSE;
	return mono_method_get_declaring_generic_method (a) == mono_method_get_declaring_generic_method (b);
}

/* An emitted JIT/AOT frame claims the LMF items it owns, if they are next: its EH record, then its phantom. Phantoms of
 * callees that never ran may sit in front of them; once a later item is claimed those are known dead and dropped. */
static int
wjsw_claim (WjswScratch *s, int li, int nitems, MonoMethod *m, gpointer body, gboolean jit, int *il_offset, MonoMethod **precise)
{
	int j = li;
	while (j < nitems && s->items [j].kind == WJSW_L_PHANTOM && !mono_wasm_stackwalk_same_method (s->items [j].method, m))
		j++;
	if (j < nitems) {
		WjswItem *it = &s->items [j];
		gboolean match = FALSE;
		if (it->kind == WJSW_L_ILSTATE)
			match = jit ? (it->ehbody && it->ehbody == body) : (!it->ehbody && mono_wasm_stackwalk_same_method (it->method, m));
		else if (it->kind == WJSW_L_PLAIN)
			match = mono_wasm_stackwalk_same_method (it->method, m);
		if (match) {
			if (it->kind == WJSW_L_ILSTATE) {
				*il_offset = it->il->il_offset;
				WJSW_COUNT (WJSW_C_REC_OK);
			}
			if (it->method && it->method != m)
				*precise = it->method;   /* the record names the instantiation the shared code is running */
			wjsw_c [WJSW_C_PHANTOM_DROP] += j - li;
			li = j = j + 1;
			while (j < nitems && s->items [j].kind == WJSW_L_PHANTOM && !mono_wasm_stackwalk_same_method (s->items [j].method, m))
				j++;
		}
		if (j < nitems && s->items [j].kind == WJSW_L_PHANTOM && mono_wasm_stackwalk_same_method (s->items [j].method, m)) {
			if (s->items [j].method != m && !*precise)
				*precise = s->items [j].method;
			wjsw_c [WJSW_C_PHANTOM_DROP] += j - li;
			WJSW_COUNT (WJSW_C_PHANTOM_OK);
			li = j + 1;
		}
	}
	return li;
}

static void
wjsw_dump (const char *why, WjswScratch *s, int nv, int nitems)
{
	int i;
	printf ("WASM_STACKWALK %s v8=%d items=%d:", why, nv, nitems);
	for (i = 0; i < nv && i < 200; ++i) {
		gint32 *v = s->v8 + 4 * i;
		if (v [0] == 1)
			printf (" J%d@%d", v [1], v [2]);
		else if (v [1] < 0)
			printf (" %s", v [1] == WJSW_A_EXEC ? "EXEC" : v [1] == WJSW_A_RESUME ? "RESUME" : "CLAUSE");
		else if (wjsw_aot_ji (v [1]))
			printf (" A%d", v [1]);
		else
			printf (" n%d", v [1]);   /* a native function: its name is wasm-function[N] in the served wasm's name section */
	}
	printf (" |");
	for (i = 0; i < nitems && i < 200; ++i) {
		WjswItem *it = &s->items [i];
		printf (" %s", it->kind == WJSW_L_SEG ? "SEG" : it->kind == WJSW_L_ILSTATE ? (it->ehbody ? "JREC" : "AREC") :
			it->kind == WJSW_L_PHANTOM ? "PH" : "PLAIN");
		if (it->kind == WJSW_L_SEG)
			printf ("(%d)", it->count);
	}
	printf ("\n");
}

/* Merge V8's frames (top first) with the LMF items. Returns the frame count, or -1 with *reason set. */
static int
wjsw_merge (WjswScratch *s, int nv, int nitems, gboolean truncated, int *reason)
{
	int li = 0, n = 0, i, j;
	MonoMethod *seg_root = NULL, *clause_dup = NULL;
	for (i = 0; i < nv; ++i) {
		gint32 *v = s->v8 + 4 * i;
		if (v [0] == 1) {
			WjswModRec *rec = wjsw_rec_for_payload ((guint32) v [1]);
			WjswMember *mem;
			MonoWasmSwFrame *f;
			int il = -1;
			if (!rec) {
				*reason = WJSW_C_F_NOREC;
				return -1;
			}
			if (!(mem = wjsw_member_at (rec, (guint32) v [2]))) {
				WJSW_COUNT (WJSW_C_THUNK);
				continue;
			}
			MonoMethod *precise = NULL;
			gint32 site_il = -1;
			guint16 site_inl = 0;
			gboolean by_key;
			gboolean superseded = mono_interp_imethod_superseded (mem->body_imethod);
			clause_dup = NULL;
			li = wjsw_claim (s, li, nitems, mem->method, mem->body_imethod, TRUE, &il, &precise);
			/* The call site: its IL offset, and the inlinees it is inside of, innermost first, each a frame of its own
			 * (HotSpot reports inlined frames; a caller-sensitive query is relative to them). */
			if (mem->body_start && (guint32) v [3] >= mem->body_start &&
			    wjsw_member_site (mem, (guint32) v [3] - mem->body_start, &site_il, &site_inl, &by_key)) {
				WJSW_COUNT (by_key ? WJSW_C_SITE_KEY : WJSW_C_SITE_HIT);
				while (site_inl && site_inl <= mem->ninl) {
					WjswInl *ie = &mem->inl [site_inl];
					f = wjsw_emit (s, &n);
					wjsw_fill_managed (f, MONO_WASM_SW_SRC_JIT, wjsw_synth_ji (&ie->ji, ie->inl.method, ie), ie->inl.method);
					f->sfi.il_offset = superseded ? -1 : site_il;
					WJSW_COUNT (WJSW_C_INLINED);
					site_il = ie->inl.call_il;
					site_inl = ie->inl.parent;
				}
				if (site_il >= 0)
					il = site_il;   /* the call's own offset: finer than the EH record's block start */
			} else if (mem->nsites) {
				WJSW_COUNT (WJSW_C_SITE_MISS);
				if (G_UNLIKELY (wjsw_verbose >= 1)) {
					/* where V8's position fell, against the sites around it */
					static int miss_printed;
					static guint64 miss_seen [24];
					guint64 mk = ((guint64) (guint32) v [1] << 32) | (guint32) v [3];
					int ms;
					for (ms = 0; ms < miss_printed && miss_seen [ms] != mk; ++ms)
						;
					if (ms == miss_printed && miss_printed < 24) {
						miss_seen [miss_printed++] = mk;
						guint32 rel = (guint32) v [3] - mem->body_start, lo = 0, hi = 0, hkd = 0, k;
						char *nm = mono_method_get_full_name (mem->method);
						for (k = 0; k < mem->nsites; ++k) {
							if (mem->site_off [k] <= rel)
								lo = mem->site_off [k];
							else {
								hi = mem->site_off [k];
								hkd = mem->site_kd ? mem->site_kd [k] : 0;
								break;
							}
						}
						printf ("WASM_STACKWALK_SITE_MISS %s payload=%d code_off=%u body_start=%u pos=%d rel=%u sites=%u prev=%u next=%u next_key=%d\n",
							nm ? nm : "?", v [1], mem->code_off, mem->body_start, v [3], rel, mem->nsites, lo, hi,
							hkd ? (int) (hi - hkd) : -1);
						g_free (nm);
					}
				}
			}
			f = wjsw_emit (s, &n);
			wjsw_fill_managed (f, MONO_WASM_SW_SRC_JIT, wjsw_member_ji (mem), mem->method);
			if (precise)
				f->sfi.actual_method = precise;
			f->sfi.il_offset = superseded ? -1 : il;
			WJSW_COUNT (WJSW_C_JIT);
		} else if (v [1] == WJSW_A_EXEC) {
			WjswItem *it;
			WJSW_COUNT (WJSW_C_EXEC);
			while (li < nitems && s->items [li].kind != WJSW_L_SEG) {
				int k = s->items [li].kind;
				WJSW_COUNT (k == WJSW_L_PHANTOM ? WJSW_C_PHANTOM_DROP : k == WJSW_L_PLAIN ? WJSW_C_PLAIN_DROP : WJSW_C_REC_DROP);
				li++;
			}
			if (li >= nitems) {
				*reason = WJSW_C_F_NOSEG;
				return -1;
			}
			it = &s->items [li++];
			seg_root = NULL;
			for (j = it->first; j < it->first + it->count; ++j) {
				MonoWasmSwFrame *f;
				seg_root = s->ifr [j].method;
				if (s->ifr_dup [j]) {
					WJSW_COUNT (WJSW_C_IFR_DUP);
					continue;
				}
				f = wjsw_emit (s, &n);
				f->src = MONO_WASM_SW_SRC_INTERP;
				f->sfi = s->ifr [j];
				f->sfi.il_offset = f->sfi.type == FRAME_TYPE_INTERP ? mono_interp_frame_il_offset (f->sfi.interp_frame) : -1;
				WJSW_COUNT (WJSW_C_INTERP);
			}
			clause_dup = NULL;
		} else if (v [1] == WJSW_A_RESUME) {
			/* exec_method -> interp_run_clause_with_il_state -> mini_llvmonly_resume_exception_il_state -> M: an AOT
			 * deopt method's handler running in the interpreter. M's own wasm frame (its landing pad) is the same
			 * activation as the clause frame just spliced: drop it. */
			clause_dup = seg_root;
		} else if (v [1] == WJSW_A_CLAUSE) {
			/* nothing: the resume anchor below it decides */
		} else {
			MonoJitInfo *ji = wjsw_aot_ji (v [1]);
			MonoMethod *m;
			MonoWasmSwFrame *f;
			int il = -1;
			if (!ji)
				continue;   /* a C function */
			m = jinfo_get_method (ji);
			if (clause_dup && m == clause_dup) {
				clause_dup = NULL;
				WJSW_COUNT (WJSW_C_CLAUSE_DUP);
				continue;
			}
			MonoMethod *precise = NULL;
			clause_dup = NULL;
			li = wjsw_claim (s, li, nitems, m, NULL, FALSE, &il, &precise);
			f = wjsw_emit (s, &n);
			wjsw_fill_managed (f, MONO_WASM_SW_SRC_AOT, ji, m);
			if (precise)
				f->sfi.actual_method = precise;
			f->sfi.il_offset = il;
			WJSW_COUNT (WJSW_C_AOT);
		}
	}
	for (; li < nitems; ++li) {
		int k = s->items [li].kind;
		if (k == WJSW_L_SEG) {
			if (truncated)
				break;   /* the capture stopped short of this segment's activation: the result is a prefix */
			*reason = WJSW_C_F_EXTRASEG;
			return -1;
		}
		WJSW_COUNT (k == WJSW_L_PHANTOM ? WJSW_C_PHANTOM_DROP : k == WJSW_L_PLAIN ? WJSW_C_PLAIN_DROP : WJSW_C_REC_DROP);
	}
	return n;
}

static MONO_NEVER_INLINE int
wjsw_capture (WjswScratch *s, int limit, gboolean *truncated)
{
	gint32 total = 0;
	int cap = MIN (limit, WJSW_MAX_FRAMES), r;
	for (;;) {
		WJSW_GROW (s->v8, s->v8_cap, cap * 4, gint32);
		r = wjsw_js_capture ((int) (gsize) s->v8, s->v8_cap / 4, limit, (int) (gsize) &total);
		WJSW_COUNT (WJSW_C_CAPTURES);
		if (r != -2)
			break;
		if (cap >= WJSW_MAX_FRAMES)
			return -1;
		cap = MIN (cap * 2, WJSW_MAX_FRAMES);
	}
	*truncated = total >= limit;
	return r;
}

/*
 * Walk the current thread's managed stack, top first. Returns the frame count with *out pointing at them -- valid
 * until the matching mono_wasm_stackwalk_end () -- or -1, and then the caller takes its legacy walk (and must NOT call
 * _end). `want` > 0 is how many MANAGED frames the caller needs (a prefix is enough, the capture can stop early); 0 = all.
 * consumer: WJSW_C_FRAMEINFO / _TRACE / _WALKFULL / _EXC, counted.
 */
int
mono_wasm_stackwalk_begin (int consumer, int want, MonoWasmSwFrame **out)
{
	WjswScratch *s;
	int nitems = 0, nifr = 0, nv = 0, n, limit, reason = 0;
	gboolean truncated = FALSE;
	gint64 t0;

	wjsw_read_knobs ();
	if (!mono_wasm_stackwalk_v8) {
		WJSW_COUNT (WJSW_C_F_OFF);
		return -1;
	}
	if (wjsw_depth >= WJSW_MAX_DEPTH) {
		WJSW_COUNT (WJSW_C_F_DEPTH);
		return -1;
	}
	if (!wjsw_aot_ensure ()) {
		WJSW_COUNT (WJSW_C_F_AOTBUSY);
		return -1;
	}
	t0 = mono_100ns_ticks ();
	if (!(s = wjsw_scratch [wjsw_depth]))
		s = wjsw_scratch [wjsw_depth] = g_new0 (WjswScratch, 1);
	wjsw_depth++;
	WJSW_COUNT (WJSW_C_WALKS);
	WJSW_COUNT (consumer);

	nitems = wjsw_read_lmf (s, &nifr);
	if (nitems < 0) {
		reason = WJSW_C_F_BADLMF;
		goto fail;
	}
	wjsw_mark_dups (s, nitems, nifr);
	/* A prefix request starts small: the top of the stack is ~10-30 native frames (this walker, the icall, the EM_JS
	 * wrapper) before the first managed one. A full one takes everything up to pass 1's cap. */
	limit = want > 0 ? 64 + want * 4 : 4096;
	for (;;) {
		nv = wjsw_capture (s, limit, &truncated);
		if (nv < 0) {
			reason = WJSW_C_F_CAPTURE;
			goto fail;
		}
		wjsw_c [WJSW_C_V8_FRAMES] += nv;
		n = wjsw_merge (s, nv, nitems, truncated, &reason);
		if (n < 0)
			goto fail;
		if (!truncated || want <= 0 || n >= want + 8 || limit >= 4096)
			break;
		limit = MIN (limit * 4, 4096);
		WJSW_COUNT (WJSW_C_RECAPTURE);
	}
	if (truncated && want <= 0) {
		reason = WJSW_C_F_SIZE;   /* deeper than the cap: say so rather than hand back a silent prefix */
		goto fail;
	}
	wjsw_c [WJSW_C_OUT_FRAMES] += n;
	wjsw_c [WJSW_C_US] += (mono_100ns_ticks () - t0) / 10;
	if (G_UNLIKELY (wjsw_verbose >= 2))
		wjsw_dump ("ok", s, nv, nitems);
	{
		gint64 w = wjsw_c [WJSW_C_WALKS];
		if (w == 1000 || w == 10000 || w == 100000 || w == 1000000)
			mono_wasm_stackwalk_print_stats ();
	}
	*out = s->out;
	return n;
fail:
	WJSW_COUNT (reason);
	if (G_UNLIKELY (wjsw_verbose >= 1)) {
		static int dumped;
		if (dumped++ < 16)
			wjsw_dump (reason == WJSW_C_F_NOSEG ? "noseg" : reason == WJSW_C_F_EXTRASEG ? "extraseg" :
				reason == WJSW_C_F_NOREC ? "norec" : reason == WJSW_C_F_BADLMF ? "badlmf" : "other", s, nv, nitems);
	}
	wjsw_depth--;
	return -1;
}

void
mono_wasm_stackwalk_end (void)
{
	if (wjsw_depth > 0)
		wjsw_depth--;
}
