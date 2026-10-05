/*
 * Conservative structured-CFG recognizer.
 *
 * The universal lowering below handles every graph with a dispatch loop + br_table.  Most hot Java
 * methods need much less: either every edge is forward, or all backward edges return to one natural
 * loop header.  The latter is exactly the shape of a while/for loop after MINI's block ordering.
 *
 * Do not attempt a general relooper here.  Accept only graphs whose dense layout proves the lexical
 * structure directly:
 *   - no edge from outside the loop enters its interior (the header is the sole entry);
 *   - every backward edge in the loop targets that header;
 *   - prefix and suffix CFGs are acyclic.
 * Anything nested, discontiguous or irreducible keeps the old dispatcher unchanged.
 */
enum {
	WJ_CFG_DISPATCH = 0,
	WJ_CFG_FORWARD = 1,
	WJ_CFG_SINGLE_LOOP = 2,
	WJ_CFG_NESTED = 3      /* plan A5 (MONO_WASM_JIT_CFG_NEST): any 'S' CFG, see wj_nest_plan */
};

/* Plan A5's scope schedule for a WJ_CFG_NESTED method. Positions are BOUNDARIES: boundary k is just before bb k's
 * code, boundary n the end of the last bb. A scope spans [start, end): a `block` is labelled at its end (a forward
 * branch to bb `end` lands there), a `loop` at its start (a back edge to its header re-enters it). */
typedef struct {
	int m;
	int *start, *end;
	char *loop;
	int *blk, *hdr;                     /* per bb: the block ending at it, the loop it heads; -1 */
	int *stk, *stk_off, *stk_len;       /* per bb: the scopes open inside its code, outermost first */
	int *open, *open_off, *open_len;    /* per boundary 0..n: scopes opened there, outermost first */
	int *nclose;                        /* per boundary 0..n: scopes closed there (innermost first) */
} WjNest;

static int
wj_structured_cfg_kind (MonoCompile *cfg, int *bbidx, int n, int *out_h, int *out_l)
{
	MonoBasicBlock *bb;
	int i, h = -1, l = -1;

	/* MONO_WASM_JIT_STRUCTURED_CFG shipped 1: emit real wasm block/loop structure wherever the CFG
	 * allows it, instead of the `loop { block { local.get $blk; br_table } }` dispatch scaffolding on
	 * every method. It also subsumes the old NODISPATCH single-bb special case. Settled. */
	if (!cfg || !cfg->header ||
	    cfg->header->num_clauses != 0 || n <= 0)
		return WJ_CFG_DISPATCH;

	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int k;
		if (bb->flags & BB_EXCEPTION_HANDLER)
			return WJ_CFG_DISPATCH;
		for (k = 0; k < bb->out_count; ++k) {
			MonoBasicBlock *tb = bb->out_bb [k];
			int t;
			if (!tb || tb->block_num < 0 || tb->block_num > (int) cfg->max_block_num)
				return WJ_CFG_DISPATCH;
			t = bbidx [tb->block_num];
			if (t < 0)
				return WJ_CFG_DISPATCH;
			if (t <= i) {
				if (h < 0)
					h = t;
				else if (h != t)
					return WJ_CFG_DISPATCH; /* nested/multiple loop headers */
				if (i > l)
					l = i;
			}
		}
	}
	if (h < 0) {
		*out_h = *out_l = -1;
		return WJ_CFG_FORWARD;
	}
	if (h > l || l >= n)
		return WJ_CFG_DISPATCH;

	/* Prove the loop is a contiguous, single-entry region and that no secondary backedge exists. */
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int k;
		for (k = 0; k < bb->out_count; ++k) {
			int t = bbidx [bb->out_bb [k]->block_num];
			if (i < h) {
				if (t <= i || (t > h && t <= l))
					return WJ_CFG_DISPATCH;
			} else if (i <= l) {
				if (t <= i && t != h)
					return WJ_CFG_DISPATCH;
				if (t < h)
					return WJ_CFG_DISPATCH;
			} else if (t <= i) {
				return WJ_CFG_DISPATCH;
			}
			if ((i < h || i > l) && t > h && t <= l)
				return WJ_CFG_DISPATCH;
		}
	}

	*out_h = h;
	*out_l = l;
	return WJ_CFG_SINGLE_LOOP;
}

/* Plan A5 census (MONO_WASM_JIT_CFGC=1, R420): could a CFG the dispatcher takes be stackified in mono's OWN block
 * order -- one wasm `loop` per back-edge target, one `block` per forward target? Layout-only, no reordering, so
 * SLOTLIVE ordinals, LCSE's single-predecessor flow and lazy_doomed's last-in-layout rule would be untouched.
 *   'E'  the method has clauses (EH keeps the dispatcher: the landing pad re-dispatches on $blk)
 *   'S'  stackifiable: every back edge i -> j (j <= i) makes [j, i] a loop interval; intervals that cross are widened
 *        until they nest (a wasm `loop` may enclose blocks that never branch back -- they just run once), and then
 *        no edge enters a loop's interior from outside it
 *   'I'  some edge enters a loop interval below its header in this order: it would need a new block order
 * *nloops / *depth: loop count and deepest nesting after widening. lo/hi, when non-NULL, receive each header's
 * interval (hi [j] = -1 when j heads no loop). */
static char
wj_cfg_stackify_class (MonoCompile *cfg, int *bbidx, int n, int *nloops, int *depth, int *lo_out, int *hi_out)
{
	MonoBasicBlock *bb;
	int i, k, changed;
	int *hi = hi_out ? hi_out : (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	*nloops = *depth = 0;
	if (!cfg->header || cfg->header->num_clauses != 0)
		return 'E';
	for (i = 0; i < n; ++i)
		hi [i] = -1;
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		if (bb->flags & BB_EXCEPTION_HANDLER)
			return 'E';
		for (k = 0; k < bb->out_count; ++k) {
			MonoBasicBlock *tb = bb->out_bb [k];
			int t;
			if (!tb || tb->block_num < 0 || tb->block_num > (int) cfg->max_block_num || (t = bbidx [tb->block_num]) < 0)
				return 'I';
			if (t <= i && i > hi [t])
				hi [t] = i;
		}
	}
	/* widen crossing intervals until the family nests: h1 < h2 <= hi[h1] < hi[h2] -> hi[h1] = hi[h2] */
	do {
		changed = 0;
		for (i = 0; i < n; ++i) {
			if (hi [i] < 0)
				continue;
			for (k = i + 1; k <= hi [i]; ++k)
				if (hi [k] > hi [i]) {
					hi [i] = hi [k];
					changed = 1;
				}
		}
	} while (changed);
	/* single entry: an edge s -> t with t inside (h, hi[h]] must come from inside [h, hi[h]] */
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		for (k = 0; k < bb->out_count; ++k) {
			int t = bbidx [bb->out_bb [k]->block_num], h;
			for (h = 0; h < t; ++h)
				if (hi [h] >= t && (i < h || i > hi [h]))
					return 'I';
		}
	}
	for (i = 0; i < n; ++i) {
		int d = 0, h;
		if (hi [i] < 0)
			continue;
		(*nloops)++;
		for (h = 0; h <= i; ++h)
			if (hi [h] >= i)
				d++;
		if (d > *depth)
			*depth = d;
	}
	if (lo_out)
		for (i = 0; i < n; ++i)
			lo_out [i] = hi [i] >= 0 ? i : -1;
	return 'S';
}

/* Plan A5: build the scope schedule for an 'S' CFG in mono's block order. One `block` per bb t >= 1, opened at the
 * earliest bb that branches forward to t (or t-1, for the fallthrough), and one `loop` per back-edge header over
 * wj_cfg_stackify_class's widened interval. Block starts are then widened until no two scopes cross; a crossing
 * that widening cannot fix (a block ending inside a loop it starts before) and any stack-discipline violation in
 * the simulated schedule return FALSE, and the method keeps the dispatcher -- the schedule is proved before a byte
 * is emitted. */
static gboolean
wj_nest_plan (MonoCompile *cfg, int *bbidx, int n, WjNest *ns)
{
	MonoBasicBlock *bb;
	int i, k, s, t, m, nl = 0, dp = 0, changed, sp;
	int *hi, *order;
	if (n < 2)
		return FALSE;
	hi = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	if (wj_cfg_stackify_class (cfg, bbidx, n, &nl, &dp, NULL, hi) != 'S')
		return FALSE;
	ns->m = m = 0;
	ns->start = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + nl));
	ns->end = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + nl));
	ns->loop = (char *) mono_mempool_alloc0 (cfg->mempool, n + nl);
	ns->blk = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	ns->hdr = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	for (i = 0; i < n; ++i)
		ns->blk [i] = ns->hdr [i] = -1;
	for (t = 1; t < n; ++t) {
		ns->blk [t] = m;
		ns->start [m] = t - 1;
		ns->end [m] = t;
		m++;
	}
	for (i = 0; i < n; ++i)
		if (hi [i] >= 0) {
			ns->hdr [i] = m;
			ns->start [m] = i;
			ns->end [m] = hi [i] + 1;
			ns->loop [m] = 1;
			m++;
		}
	ns->m = m;
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i)
		for (k = 0; k < bb->out_count; ++k) {
			t = bbidx [bb->out_bb [k]->block_num];
			if (t > i && ns->start [ns->blk [t]] > i)
				ns->start [ns->blk [t]] = i;
		}
	/* a scope that starts before block s and ends strictly inside it: s must start where it starts */
	do {
		changed = 0;
		for (s = 0; s < m; ++s) {
			if (ns->loop [s])
				continue;
			for (k = 0; k < m; ++k)
				if (k != s && ns->start [k] < ns->start [s] && ns->start [s] < ns->end [k] && ns->end [k] < ns->end [s]) {
					ns->start [s] = ns->start [k];
					changed = 1;
				}
		}
	} while (changed);
	for (s = 0; s < m; ++s)
		for (k = 0; k < m; ++k)
			if (ns->start [s] < ns->start [k] && ns->start [k] < ns->end [s] && ns->end [s] < ns->end [k])
				return FALSE;
	/* opens per boundary, outermost first: later end first; at an equal end the block is outer */
	order = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * m);
	ns->open = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * m);
	ns->open_off = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	ns->open_len = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	ns->nclose = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	ns->stk_off = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	ns->stk_len = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	{
		int pos = 0;
		for (k = 0; k <= n; ++k) {
			int c = 0, a, b;
			ns->open_off [k] = pos;
			for (s = 0; s < m; ++s)
				if (ns->start [s] == k)
					order [c++] = s;
			for (a = 1; a < c; ++a)   /* insertion sort: end descending, block before loop */
				for (b = a; b > 0; --b) {
					int x = order [b - 1], y = order [b];
					if (ns->end [x] > ns->end [y] || (ns->end [x] == ns->end [y] && !ns->loop [x]))
						break;
					order [b - 1] = y;
					order [b] = x;
				}
			for (a = 0; a < c; ++a)
				ns->open [pos++] = order [a];
			ns->open_len [k] = c;
		}
	}
	/* simulate: pop what ends here (it must be on top), push what starts here; snapshot each bb's stack */
	{
		int *stack = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * m);
		int total = 0, pos = 0;
		sp = 0;
		for (k = 0; k <= n; ++k) {
			int c = 0;
			while (sp > 0 && ns->end [stack [sp - 1]] == k) {
				sp--;
				c++;
			}
			for (s = 0; s < sp; ++s)
				if (ns->end [stack [s]] <= k)
					return FALSE;   /* a scope ending here is buried under one that does not */
			ns->nclose [k] = c;
			for (s = 0; s < ns->open_len [k]; ++s)
				stack [sp++] = ns->open [ns->open_off [k] + s];
			if (k < n)
				total += sp;
		}
		if (sp != 0)
			return FALSE;
		ns->stk = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (total + 1));
		sp = 0;
		for (k = 0; k < n; ++k) {
			sp -= ns->nclose [k];
			for (s = 0; s < ns->open_len [k]; ++s)
				stack [sp++] = ns->open [ns->open_off [k] + s];
			ns->stk_off [k] = pos;
			ns->stk_len [k] = sp;
			for (s = 0; s < sp; ++s)
				ns->stk [pos++] = stack [s];
		}
	}
	return TRUE;
}

/* Plan A5: the br depth from inside bb `from` (at its top level) to bb `to`: the block ending at `to` for a forward
 * edge, the loop headed by `to` for a back edge. -1 when that scope is not open there (the planner rules it out). */
static int
wj_nest_depth (const WjNest *ns, int from, int to)
{
	int sid = to > from ? ns->blk [to] : ns->hdr [to], p;
	if (sid < 0)
		return -1;
	for (p = ns->stk_len [from] - 1; p >= 0; --p)
		if (ns->stk [ns->stk_off [from] + p] == sid)
			return ns->stk_len [from] - 1 - p;
	return -1;
}

/* Plan A5 v2 (MONO_WASM_JIT_CFG_NEST=2, R424): a loop-aware block order. R421's census found the dispatcher's CFGs are
 * javac's rotated loops -- `goto test; body: ...; test: if (c) goto body` -- entered at the test, which mono lays out
 * AFTER the body, so no in-order stackifier reaches them (59-61% of JIT cycles). This reorders the blocks so every
 * natural loop is contiguous with its header first, keeping mono's order wherever the CFG allows: Kahn's sort per loop
 * region keyed on the original position, each child loop one node (keyed on its earliest block) expanded in place;
 * bb_exit last. A port of scratchpad/wj/enctest/cfgsort.py, which fuzzes it (9,000 javac-shaped CFGs: every rotated
 * one becomes stackifiable). Only called on CFGs the in-order stackifier rejects ('I'), at the top of
 * mono_wasm_emit_method before any pass walks the layout; the caller re-checks the result and restores the original
 * order unless it is now 'S'. Refuses (FALSE, layout untouched): a clause, an unreachable block, an irreducible edge,
 * a block that leaves by an implicit fallthrough without exactly one successor, more than WJ_CFG_SORT_MAX blocks. */
#define WJ_CFG_SORT_MAX 4096
typedef struct {
	int n, nl;
	int *succ_off, *succ;        /* RPO successors */
	int *loop_hdr;               /* loops by increasing size: header (RPO) */
	guint8 *mem;                 /* nl x n membership, in the loop_hdr order */
	int *loop_ix;                /* header (RPO) -> its index in loop_hdr, -1 */
	int *inner, *parent;         /* block -> innermost loop header; header -> enclosing header; -1 */
	int *key;                    /* RPO block -> original position; for a header used as an item: its loop's earliest */
	int *lkey;
	int *out, nout;
	int *indeg, *stamp, stampv, ne_max;
	MonoMemPool *mp;
} WjSort;

static int
wj_sort_item (WjSort *s, int x, int r)
{
	int c = s->inner [x], prev = -1;
	if (c == r)
		return x;
	if (c < 0)
		return r == -1 ? x : -2;
	while (c >= 0 && c != r) {
		prev = c;
		c = s->parent [c];
	}
	return c == r ? prev : -2;
}

static gboolean
wj_sort_in_region (WjSort *s, int x, int r)
{
	return r < 0 || s->mem [(gsize) s->loop_ix [r] * s->n + x];
}

static gboolean
wj_sort_place (WjSort *s, int r)
{
	int x, k, ni = 0, ne = 0, nready = 0, done = 0, stamp = ++s->stampv;
	/* per-level scratch from the compile's mempool: a recursion level per loop-nesting depth, and alloca of n-sized
	 * arrays per level could run a worker's stack out on a method near WJ_CFG_SORT_MAX */
	int *items = (int *) mono_mempool_alloc (s->mp, sizeof (int) * (s->n + 1));
	int *ea = (int *) mono_mempool_alloc (s->mp, sizeof (int) * (s->ne_max + 1));
	int *eb = (int *) mono_mempool_alloc (s->mp, sizeof (int) * (s->ne_max + 1));
	int *ready = (int *) mono_mempool_alloc (s->mp, sizeof (int) * (s->n + 1));
	int *indeg = s->indeg;   /* indexed by item; this level's items are disjoint from any child level's except the
	                          * child's header, which this level no longer needs once the child is placed */
	for (x = 0; x < s->n; ++x) {
		int it;
		if (!wj_sort_in_region (s, x, r))
			continue;
		it = wj_sort_item (s, x, r);
		if (it < 0 || s->stamp [it] == stamp)
			continue;
		s->stamp [it] = stamp;
		items [ni++] = it;
		indeg [it] = 0;
	}
	for (x = 0; x < s->n; ++x) {
		int a, j;
		if (!wj_sort_in_region (s, x, r))
			continue;
		a = wj_sort_item (s, x, r);
		for (j = s->succ_off [x]; j < s->succ_off [x + 1]; ++j) {
			int y = s->succ [j], b;
			if (y == r || !wj_sort_in_region (s, y, r))
				continue;
			b = wj_sort_item (s, y, r);
			if (b < 0 || b == a)
				continue;
			ea [ne] = a;
			eb [ne++] = b;
			indeg [b]++;
		}
	}
	for (k = 0; k < ni; ++k)
		if (indeg [items [k]] == 0)
			ready [nready++] = items [k];
	while (nready > 0) {
		int bi = 0, i, e;
		for (i = 1; i < nready; ++i) {
			int a = ready [i], c = ready [bi];
			int ka = (s->loop_ix [a] >= 0 && a != r) ? s->lkey [a] : s->key [a];
			int kc = (s->loop_ix [c] >= 0 && c != r) ? s->lkey [c] : s->key [c];
			if (ka < kc)
				bi = i;
		}
		x = ready [bi];
		ready [bi] = ready [--nready];
		done++;
		if (s->loop_ix [x] >= 0 && x != r) {
			if (!wj_sort_place (s, x))
				return FALSE;
		} else {
			s->out [s->nout++] = x;
		}
		for (e = 0; e < ne; ++e)
			if (ea [e] == x && --indeg [eb [e]] == 0)
				ready [nready++] = eb [e];
	}
	return done == ni;
}

static const char *wj_opname (int op);   /* mini-wasm-ir.inc, same translation unit */
/* 0 = reordered; otherwise why not (census: WASM_JIT_CFGSORT -<why> ...), *why_op the opcode behind 4 / 5 */
static int
wj_cfg_sort (MonoCompile *cfg, int *why_op)
{
	MonoBasicBlock *bb, **orig, **rpo_bb;
	int n = 0, i, k, top, nl, *pos, *rpo, *idom, *stk, *it, *pred_off, *pred, *cnt;
	gboolean changed;
	WjSort s;
	if (!cfg->header || cfg->header->num_clauses != 0 || !cfg->bb_entry || !cfg->bb_exit)
		return 1;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		n++;
	if (n < 3 || n > WJ_CFG_SORT_MAX)
		return 2;
	orig = (MonoBasicBlock **) mono_mempool_alloc0 (cfg->mempool, sizeof (gpointer) * n);
	pos = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
	for (i = 0; i <= (int) cfg->max_block_num; ++i)
		pos [i] = -1;
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		MonoInst *last = bb->last_ins;
		if (bb->block_num < 0 || bb->block_num > (int) cfg->max_block_num || (bb->flags & BB_EXCEPTION_HANDLER))
			return 3;
		orig [i] = bb;
		pos [bb->block_num] = i;
		if (bb == cfg->bb_exit)
			continue;
		/* how the block leaves: one successor (a branch, or a fallthrough the emitter sends to out_bb [0] once sorted);
		 * several: an explicit conditional branch or switch -- or mono's two-instruction terminator `cond-branch;
		 * OP_BR`, where the emitter's COND_BRANCH branches to both targets and skips the OP_BR (R425: all 200 of
		 * jbox2d's refusals were that shape); none: a throw */
		if (bb->out_count >= 2 && !(last && (MONO_IS_COND_BRANCH_OP (last) || last->opcode == OP_SWITCH || last->opcode == OP_BR))) {
			*why_op = last ? last->opcode : -1;
			return 4;
		}
		if (bb->out_count == 0 && !(last && (last->opcode == OP_THROW || last->opcode == OP_RETHROW || last->opcode == OP_NOT_REACHED))) {
			*why_op = last ? last->opcode : -1;
			return 5;
		}
	}
	for (i = 0; i < n; ++i)
		for (k = 0; k < orig [i]->out_count; ++k) {
			MonoBasicBlock *t = orig [i]->out_bb [k];
			if (!t || t->block_num < 0 || t->block_num > (int) cfg->max_block_num || pos [t->block_num] < 0)
				return 6;
		}
	/* reverse postorder */
	rpo = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	rpo_bb = (MonoBasicBlock **) mono_mempool_alloc0 (cfg->mempool, sizeof (gpointer) * n);
	stk = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	it = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	{
		int *post = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n), np = 0;
		guint8 *seen = (guint8 *) mono_mempool_alloc0 (cfg->mempool, n);
		top = 0;
		stk [top] = pos [cfg->bb_entry->block_num];
		it [top++] = 0;
		seen [stk [0]] = 1;
		while (top > 0) {
			int x = stk [top - 1];
			if (it [top - 1] < orig [x]->out_count) {
				int y = pos [orig [x]->out_bb [it [top - 1]++]->block_num];
				if (!seen [y]) {
					seen [y] = 1;
					stk [top] = y;
					it [top++] = 0;
				}
			} else {
				post [np++] = x;
				top--;
			}
		}
		if (np != n)
			return 7;   /* unreachable blocks */
		for (i = 0; i < n; ++i) {
			rpo [post [n - 1 - i]] = i;
			rpo_bb [i] = orig [post [n - 1 - i]];
		}
	}
	/* RPO successor / predecessor lists, both derived from out_bb */
	memset (&s, 0, sizeof (s));
	s.n = n;
	s.succ_off = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	pred_off = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	cnt = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	for (i = 0; i < n; ++i) {
		s.succ_off [i + 1] = s.succ_off [i] + rpo_bb [i]->out_count;
		for (k = 0; k < rpo_bb [i]->out_count; ++k)
			pred_off [rpo [pos [rpo_bb [i]->out_bb [k]->block_num]] + 1]++;
	}
	for (i = 0; i < n; ++i)
		pred_off [i + 1] += pred_off [i];
	s.succ = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (s.succ_off [n] + 1));
	pred = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (s.succ_off [n] + 1));
	for (i = 0; i < n; ++i)
		for (k = 0; k < rpo_bb [i]->out_count; ++k) {
			int y = rpo [pos [rpo_bb [i]->out_bb [k]->block_num]];
			s.succ [s.succ_off [i] + k] = y;
			pred [pred_off [y] + cnt [y]++] = i;
		}
	/* immediate dominators (Cooper-Harvey-Kennedy) */
	idom = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	for (i = 0; i < n; ++i)
		idom [i] = -1;
	idom [0] = 0;
	do {
		changed = FALSE;
		for (i = 1; i < n; ++i) {
			int nd = -1;
			for (k = pred_off [i]; k < pred_off [i + 1]; ++k) {
				int p = pred [k];
				if (idom [p] < 0)
					continue;
				if (nd < 0)
					nd = p;
				else {
					int a = p, c = nd;
					while (a != c) {
						while (a > c) a = idom [a];
						while (c > a) c = idom [c];
					}
					nd = a;
				}
			}
			if (nd >= 0 && idom [i] != nd) {
				idom [i] = nd;
				changed = TRUE;
			}
		}
	} while (changed);
	/* natural loops: a retreating edge u -> h must have h dominate u (else irreducible: refuse) */
	nl = 0;
	for (i = 0; i < n; ++i)
		for (k = pred_off [i]; k < pred_off [i + 1]; ++k)
			if (pred [k] >= i) { nl++; break; }
	s.nl = nl;
	s.loop_ix = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	s.inner = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	s.parent = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	for (i = 0; i < n; ++i)
		s.loop_ix [i] = s.inner [i] = s.parent [i] = -1;
	if (nl) {
		int *hdr = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nl), *sz = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nl);
		guint8 *mem = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nl * n);
		int li = 0;
		for (int h = 0; h < n; ++h) {
			guint8 *m;
			int nlat = 0;
			for (k = pred_off [h]; k < pred_off [h + 1]; ++k)
				if (pred [k] >= h) nlat++;
			if (!nlat)
				continue;
			m = mem + (gsize) li * n;
			m [h] = 1;
			top = 0;
			for (k = pred_off [h]; k < pred_off [h + 1]; ++k) {
				int u = pred [k], d = u;
				if (u < h)
					continue;
				while (d != h && d != 0) d = idom [d];
				if (d != h)
					return 8;   /* irreducible */
				if (!m [u]) { m [u] = 1; stk [top++] = u; }
			}
			while (top > 0) {
				int x = stk [--top];
				for (k = pred_off [x]; k < pred_off [x + 1]; ++k)
					if (!m [pred [k]]) { m [pred [k]] = 1; stk [top++] = pred [k]; }
			}
			hdr [li] = h;
			for (i = 0; i < n; ++i)
				sz [li] += m [i];
			li++;
		}
		/* order loops by increasing size (insertion sort; nl is small), membership rows with them */
		s.loop_hdr = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nl);
		s.mem = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nl * n);
		{
			int *ord = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nl), a, b;
			for (a = 0; a < nl; ++a)
				ord [a] = a;
			for (a = 1; a < nl; ++a)
				for (b = a; b > 0 && sz [ord [b - 1]] > sz [ord [b]]; --b) { int t = ord [b]; ord [b] = ord [b - 1]; ord [b - 1] = t; }
			for (a = 0; a < nl; ++a) {
				s.loop_hdr [a] = hdr [ord [a]];
				memcpy (s.mem + (gsize) a * n, mem + (gsize) ord [a] * n, n);
				s.loop_ix [s.loop_hdr [a]] = a;
			}
		}
		for (int a = 0; a < nl; ++a) {
			guint8 *m = s.mem + (gsize) a * n;
			for (i = 0; i < n; ++i)
				if (m [i] && s.inner [i] < 0)
					s.inner [i] = s.loop_hdr [a];
		}
		for (int a = 0; a < nl; ++a) {
			int h = s.loop_hdr [a];
			for (int b = a + 1; b < nl; ++b)
				if (s.mem [(gsize) b * n + h]) { s.parent [h] = s.loop_hdr [b]; break; }
		}
	}
	/* keys: a block's original position; a loop's: its earliest block's */
	s.key = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	s.lkey = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	for (i = 0; i < n; ++i)
		s.key [i] = s.lkey [i] = pos [rpo_bb [i]->block_num];
	for (int a = 0; a < nl; ++a) {
		guint8 *m = s.mem + (gsize) a * n;
		int h = s.loop_hdr [a];
		for (i = 0; i < n; ++i)
			if (m [i] && s.key [i] < s.lkey [h])
				s.lkey [h] = s.key [i];
	}
	s.out = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * n);
	s.indeg = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	s.stamp = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (n + 1));
	s.ne_max = s.succ_off [n];
	s.mp = cfg->mempool;
	if (!wj_sort_place (&s, -1) || s.nout != n)
		return 9;
	/* relink: the new order, bb_exit moved last (a sink, so a later position keeps every edge's direction) */
	{
		MonoBasicBlock **neword = (MonoBasicBlock **) mono_mempool_alloc0 (cfg->mempool, sizeof (gpointer) * n);
		int m = 0;
		for (i = 0; i < n; ++i)
			if (rpo_bb [s.out [i]] != cfg->bb_exit)
				neword [m++] = rpo_bb [s.out [i]];
		neword [m++] = cfg->bb_exit;
		if (neword [0] != cfg->bb_entry)
			return 10;
		for (i = 0; i < n; ++i)
			if (neword [i] != orig [i])
				break;
		if (i == n)
			return 11;   /* already in this order */
		for (i = 0; i + 1 < n; ++i)
			neword [i]->next_bb = neword [i + 1];
		neword [n - 1]->next_bb = NULL;
	}
	return 0;
}

/* Plan A5 v2: sort only what the in-order stackifier rejects ('I' under the dispatcher), and keep the new order only
 * if it is now stackifiable; otherwise put mono's order back exactly. Returns TRUE when the layout changed. */
static gboolean
wj_cfg_sort_if_rotated (MonoCompile *cfg)
{
	MonoBasicBlock *bb, **orig;
	int n = 0, i, h = -1, l = -1, nl = 0, dp = 0, *bbidx;
	gboolean ok;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		n++;
	if (n < 3 || n > WJ_CFG_SORT_MAX)
		return FALSE;
	bbidx = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
	orig = (MonoBasicBlock **) mono_mempool_alloc0 (cfg->mempool, sizeof (gpointer) * n);
	for (i = 0; i <= (int) cfg->max_block_num; ++i)
		bbidx [i] = -1;
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		orig [i] = bb;
		if (bb->block_num >= 0 && bb->block_num <= (int) cfg->max_block_num)
			bbidx [bb->block_num] = i;
	}
	if (wj_structured_cfg_kind (cfg, bbidx, n, &h, &l) != WJ_CFG_DISPATCH)
		return FALSE;
	if (wj_cfg_stackify_class (cfg, bbidx, n, &nl, &dp, NULL, NULL) != 'I')
		return FALSE;
	{
		int why_op = -1, why = wj_cfg_sort (cfg, &why_op);
		if (why) {
			extern int mono_wasm_jit_cfgc;   /* census: why the rotated CFG was not sorted */
			if (G_UNLIKELY (mono_wasm_jit_cfgc))
				printf ("WASM_JIT_CFGSORT_NO\t-%d\t%d\t%s\t%s\n", why, n, why_op >= 0 ? wj_opname (why_op) : "-",
					cfg->method ? cfg->method->name : "?");
			return FALSE;
		}
	}
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i)
		bbidx [bb->block_num] = i;
	ok = i == n && wj_cfg_stackify_class (cfg, bbidx, n, &nl, &dp, NULL, NULL) == 'S';
	if (!ok) {
		for (i = 0; i + 1 < n; ++i)
			orig [i]->next_bb = orig [i + 1];
		orig [n - 1]->next_bb = NULL;
	}
	{ extern int mono_wasm_jit_cfgc;   /* census: 1 = kept, 0 = sorted but not stackifiable, order restored */
	  if (G_UNLIKELY (mono_wasm_jit_cfgc))
		printf ("WASM_JIT_CFGSORT_OK\t%d\t%d\t%s\n", ok ? 1 : 0, n, cfg->method ? cfg->method->name : "?"); }
	return ok;
}

/* Label depth for an edge in one of the verified structured layouts. */
static int
wj_structured_branch_depth (int kind, int h, int l, int n, int from, int to)
{
	int s = l + 1;
	if (kind == WJ_CFG_FORWARD)
		return to > from ? to - from - 1 : -1;
	if (kind != WJ_CFG_SINGLE_LOOP)
		return -1;
	if (from < h) {
		if (to > from && to < h)
			return to - from - 1;
		if (to == h)
			return h - from - 1; /* dedicated loop-entry block */
		if (to >= s && to < n)
			return (h - from) + (to - s);
		return -1;
	}
	if (from <= l) {
		if (to == h)
			return l - from; /* remaining body blocks, then the loop label */
		if (to > from && to <= l)
			return to - from - 1;
		if (to >= s && to < n)
			return (l - from) + 1 + (to - s); /* body labels + loop + suffix label */
		return -1;
	}
	return to > from && to < n ? to - from - 1 : -1;
}

static gboolean
wj_structured_target_has_label (int kind, int h, int l, int from, int to)
{
	if (kind == WJ_CFG_FORWARD)
		return to > from;
	if (kind != WJ_CFG_SINGLE_LOOP)
		return FALSE;
	if (from < h)
		return (to > from && to <= h) || to >= l + 1;
	if (from <= l)
		return to == h || (to > from && to <= l) || to >= l + 1;
	return to > from;
}

/* Number of lexical control labels between bb code and the shared throw blocks. */
static int
wj_structured_outer_depth (int kind, int h, int l, int n, int from)
{
	if (kind == WJ_CFG_FORWARD)
		return n - 1 - from;
	if (kind == WJ_CFG_SINGLE_LOOP) {
		int s = l + 1;
		if (from < h)
			return (h - from) + (n - s);
		if (from <= l)
			return (l - from) + 1 + (n - s);
		return n - 1 - from;
	}
	return -1;
}



#ifdef HOST_BROWSER
/* R316b (MONO_WASM_JIT_PROF_ORIGIN): which record a profile read at this call site consults. A site IR'd inside an
 * inlinee asks that inlinee's record first -- where the interpreter and its own tier-1 IC misses filed the site --
 * and the compiled method's when that record has nothing for it. Counted per READ, not per site. */
static gpointer
wj_call_prof_im (MonoCompile *cfg, MonoCallInst *call)
{
	extern int mono_wasm_jit_prof_origin;
	extern gboolean mono_wasm_jit_prof_has_site (gpointer caller, MonoMethod *base);
	if (!mono_wasm_jit_prof_origin || !call->wasm_prof_im || call->wasm_prof_im == cfg->wasm_jit_caller_imethod)
		return cfg->wasm_jit_caller_imethod;
	if (mono_wasm_jit_prof_has_site (call->wasm_prof_im, call->method)) {
		wj_count (WJC_PROF_ORIGIN_USED);
		return call->wasm_prof_im;
	}
	wj_count (WJC_PROF_ORIGIN_FALLBACK);
	return cfg->wasm_jit_caller_imethod;
}

/* R371: a basic block's kind for the cold-block census -- 0 other, 1 a catch/filter/fault handler, 2 a block that
 * throws. A finally block runs on the normal path as well, so it is "other". Also counts the block's call sites. */
static int
wj_bb_cold_class (MonoBasicBlock *bb)
{
	MonoInst *ins;
	int cls = 0, ncalls = 0;
	if (bb->region != -1) {
		int k = bb->region & (0xf << 4);
		if (k == MONO_REGION_CATCH || k == MONO_REGION_FILTER || k == MONO_REGION_FAULT)
			cls = 1;
	}
	MONO_BB_FOR_EACH_INS (bb, ins) {
		if (MONO_IS_CALL (ins))
			ncalls++;
		if (!cls && (ins->opcode == OP_THROW || ins->opcode == OP_RETHROW))
			cls = 2;
	}
	mono_wasm_jit_counters [WJC_COLD_CALLS_OTHER + cls] += ncalls;
	return cls;
}

/* R358 (MONO_WASM_JIT_T2_UNIT=2): the tier-2 unit SUPPLY census. Walk a finished tier-2 body's direct-call holes
 * (WASM_RELOC_CALL: every call it kept to another JITted method) and put each distinct callee in one bucket --
 * which of them a unit could carry as a private copy, and how big and how hot those are. Changes nothing emitted. */
static void
wj_t2u_census (MonoMethod *root, WjBody *b)
{
	extern int mono_wasm_jit_t2u_classify (MonoMethod *root, MonoMethod *callee, int *samples, int *desc);
	extern int mono_wasm_jit_desc_body_len (int desc);
	WasmRelocs *rs = b->f_body.relocs;
	MonoMethod *seen [256];
	int bucket [256];
	int nseen = 0, i, j;
	if (!rs)
		return;
	mono_wasm_jit_count (WJC_T2U_ROOTS);
	for (i = 0; i < (int) rs->n; ++i) {
		WasmReloc *r = &rs->r [i];
		MonoMethod *c;
		int samples = 0, desc = 0, bk;
		if (r->kind != WASM_RELOC_CALL || !r->sym)
			continue;
		mono_wasm_jit_count (WJC_T2U_SITES);
		c = (MonoMethod *) r->sym;
		for (j = 0; j < nseen && seen [j] != c; ++j)
			;
		if (j < nseen) {
			if (bucket [j] == WJC_T2U_ELIGIBLE)
				mono_wasm_jit_count (WJC_T2U_ELIG_SITES);
			continue;
		}
		if (nseen >= 256) {
			mono_wasm_jit_count (WJC_T2U_OVERFLOW);
			continue;
		}
		bk = mono_wasm_jit_t2u_classify (root, c, &samples, &desc);
		if (bk == WJC_T2U_ELIGIBLE) {
			int len = mono_wasm_jit_desc_body_len (desc);
			if (len > 8192) {
				bk = WJC_T2U_BIG;
			} else {
				mono_wasm_jit_count (WJC_T2U_ELIG_SITES);
				mono_wasm_jit_add (WJC_T2U_ELIG_BYTES, len);
				if (samples >= 8)
					mono_wasm_jit_count (WJC_T2U_ELIG_HOT);
			}
		}
		seen [nseen] = c;
		bucket [nseen++] = bk;
		mono_wasm_jit_count (WJC_T2U_CALLEES);
		mono_wasm_jit_count (bk);
	}
}
#endif

void
mono_wasm_emit_method (MonoCompile *cfg)
{
#ifdef HOST_BROWSER
	if (mono_wasm_jit_verbose >= 2) { printf ("WASM_JIT_EMIT_ENTER %s opt=0x%x\n", cfg->method ? cfg->method->name : "?", (unsigned) cfg->opt); }
#endif
	/* Compile-time accounting (Part 2): wj_gen_t0 = bytecode-GENERATION start (100ns ticks); wj_inst_ms =
	 * the JS WebAssembly.Module/Instance time for THIS compile (written by mono_wasm_jit_instantiate_local).
	 * At `done:` we fold (elapsed - instantiate) into WJC_ELAPSED_GENERATION so the two halves don't overlap. */
	gint64 wj_gen_t0 = G_UNLIKELY (mono_wasm_jit_stats) ? mono_100ns_ticks () : 0;
	double wj_inst_ms = 0;
	MonoMethodSignature *sig = mono_method_signature_internal (cfg->method);
	int nvreg = cfg->next_vreg;
	int nargs = sig->hasthis + sig->param_count;
	WasmValtype *vt = (WasmValtype *) mono_mempool_alloc0 (cfg->mempool, sizeof (WasmValtype) * nvreg);
	guint8 *wj_undef = NULL;   /* R410: never-written locals, typed from their variable */
	int *li = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
	int *refslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* ref vreg -> shadow-stack slot, else -1 */
	int *addrslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* address-taken local vreg -> byte offset in the addr frame, else -1 */
	signed char *addr_ldkind = (signed char *) mono_mempool_alloc0 (cfg->mempool, sizeof (signed char) * nvreg); /* sub-word read-back kind (0=full), see WjCtx.addr_ldkind */
	guint8 *arg_spill = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nargs + 1);   /* [nargs]: 1 = an address-taken scalar arg the prologue copies into its addr slot */
	WasmValtype *param_types = (WasmValtype *) mono_mempool_alloc0 (cfg->mempool, sizeof (WasmValtype) * (nargs + 2)); /* +2: trailing hidden-vret param, then the trailing threaded frame base (MONO_WASM_JIT_THREAD_SP) */
	WasmValtype ret_vt;
	WasmCallInfo self_ci;              /* the method's OWN callee ABI (self-sig); also the registered f_sig_id, so callers' baked dep_sig and our registration can never disagree */
	int nwparams;                      /* wasm param count of func f = nargs + (hidden vret ? 1 : 0) + (threaded frame base ? 1 : 0). ALL local-index math is based on this, not nargs: wasm locals are numbered after the params */
	gboolean self_has_byaddr G_GNUC_UNUSED = FALSE;  /* stats: method takes >=1 by-addr vtype arg */
	WasmBuf body, out;
	WjBody *wj_stored_body = NULL;   /* the relocatable body, handed to the registry on successful registration */
	MonoBasicBlock *bb;
	MonoInst *ins;
	int i, cnt [4] = { 0, 0, 0, 0 }, base [4], run [4] = { 0, 0, 0, 0 };
	WasmLocalGroup groups [4];
	int dispatch_idx = 0, N = 0;
	int scratch_idx G_GNUC_UNUSED = 0; /* i32 local holding the per-thread interp-residual scratch ptr */
	int refbase_idx = 0, rtmp_idx = 0; /* i32 locals: ref-frame base addr + scratch for ref stores */
	int spentry_idx = 0;               /* i32 local: C-stack SP at entry (exit/landing-pad restore target) */
	int alloc_tmp_idx = 0;             /* 3 i32 locals for OP_WASM_JIT_ALLOC_FAST, declared only if it occurs */
	int framebytes G_GNUC_UNUSED = 0, refbytes_al G_GNUC_UNUSED = 0; /* C-stack frame size (16-aligned) / 8-aligned ref-slot bytes */
	gboolean method_nogc G_GNUC_UNUSED = FALSE;        /* transitive direct-call effect published in WjRegEntry */
	int effective_gcp_count G_GNUC_UNUSED = 0;         /* static returning GC points after direct no-GC resolution */
	gboolean lazy_ref_frame = FALSE;     /* defer a pure ref frame until the first effective GC point */
	/* MONO_WASM_JIT_LAZY_COLD: the lazy frame can only materialise in a raise or poll taken arm, so it is
	 * never live on the hot path -- no mirrors, kill stores, exit restore, or hot ENSURE (see the knob). */
	gboolean lazy_cold_frame = FALSE;
	int hot_gcp_count G_GNUC_UNUSED = 0; /* effective GC points that are neither a raise nor OP_GC_SAFE_POINT */
	/* LAZY_COLD=2 (R294): per-LAYOUT-INDEX flag, "this bb cannot reach the method exit" -- every path
	 * through it ends in a throw, so its GC points run only on the exception path and are not counted in
	 * hot_gcp_count (they go to doomed_gcp_count). The emission loop runs a cold frame as an ordinary lazy
	 * frame (lazy_frame 1: conditional mirrors) inside these blocks: a GC point there materialises the
	 * frame, and the refs defined after it must still reach their slots before the next one. Sound
	 * because a doomed block's successors are all doomed -- once in, control never returns to a block
	 * where the frame is assumed absent. NULL when not computed. */
	guint8 *lazy_doomed = NULL;
	int doomed_gcp_count G_GNUC_UNUSED = 0;
	gboolean gcp_walked G_GNUC_UNUSED = FALSE;   /* the SLOTLIVE walk ran, so the two counts above are real */
	MonoInst *terminal_vcall_ins G_GNUC_UNUSED = NULL; /* last GC point in an acyclic forwarding wrapper; owns root handoff + guarded specialization */
	int terminal_vcall_ord G_GNUC_UNUSED = -1;         /* layout ordinal used to prove every elided reference dies at/before that handoff */
	gboolean terminal_vcall_handoff G_GNUC_UNUSED = FALSE; /* all pre-call references die at the terminal vcall; poll-prefix frames may stay cold */
	gboolean terminal_vcall_poll_prefix G_GNUC_UNUSED = FALSE; /* every earlier effective GC point is a conditional OP_GC_SAFE_POINT poll */
	int vc_fslot_idx = 0;              /* i32 local: inline virtual-IC fast-path resolved f-slot */
	int vc_aotkind_idx = 0;            /* i32 local: VCALL_AOT dispatch kind from vcall_aot_target (0=residual,1=+rgctx,2=no-extra) */
	int aic_vtab_idx = 0, aic_ti_idx = 0, aic_rgctx_idx = 0; /* i32 locals: AOT-vcall IC — this->vtable, ti<<1|kind2, rgctx */
	int dic_base_idx = 0;              /* i32 local: delegate-IC entry base for the way under test (wire-byte hoist) */
	int slotlive_ptr_idx = 0, slotlive_cap_idx = 0; /* i32 locals: cached &wj_slot_live / &wj_slot_live_cap for the INLINE f-slot-IC liveness check (dead in methods with no vcall) */
	/* Only the POINTER addresses need locals: the pointer is re-loaded through its address on every dispatch
	 * (so a realloc is picked up), while each capacity is loaded once in the prologue straight into its value
	 * local — the cap address is dead immediately and never earned a slot. */
	int vpic_ptr_idx = 0;              /* i32 local: address of this worker's TLS vcall PIC pointer */
	int dpic_ptr_idx = 0;              /* i32 local: same, for this worker's TLS delegate recipe PIC */
	/* i32 locals holding the VALUE of each TLS PIC capacity, loaded once in the prologue. Every dispatch
	 * used to re-load it through the imported address just to bounds-check its own site id. Hoisting the
	 * LOAD (not the comparison, which needs the site id and so cannot precede body emission) is safe in the
	 * only direction that matters: capacity is monotonically non-decreasing (wj_*_pic_for_site only ever
	 * grows it), so a stale cached value is an UNDER-estimate, and an under-estimate sends the dispatch to
	 * the slow path — a missed IC hit, never a read past the array. */
	int vpic_capval_idx = 0, dpic_capval_idx = 0;
	gboolean has_vcall = FALSE;        /* TRUE: method has >=1 OP_*CALL_MEMBASE (a vcall-IC site) -> emit the prologue slotlive fetch */
	int vc_ic_idx = 0;                 /* i64 local: inline virtual-IC fast-path IC value (vtable|imethod<<32) */
	int eh_exc_idx = 0, eh_h_idx = 0;  /* i32 locals: in-method EH catch landing pad — saved C++ exc ptr + dispatch result */
	int ilofs_ti G_GNUC_UNUSED = -1;                 /* functype index for (i32,i32)->i32: enter_island (method, ndata) -> il_state */
	int finally_ind_idx = 0;           /* i32 local: in-method finally indicator (continuation bb idx, or -1 = rethrow) */
	gboolean eh_has_finally = FALSE;   /* TRUE: method has >=1 FINALLY clause (milestone 2c) */
	WasmEhTable *eh_table = NULL;      /* in-method EH clause table (built below, baked into the catch landing pad) */
	gboolean eh_on = FALSE;            /* TRUE: emit the in-method try/catch wrapper for this method */
	gboolean ehrec_on G_GNUC_UNUSED = FALSE;   /* eh_on + MONO_WASM_JIT_EH_REC: inline frame record instead of the island calls (R353) */
	int ehrec_off G_GNUC_UNUSED = 0;           /* the record's byte offset in the frame, from refbase */
	int ehlmf_idx G_GNUC_UNUSED = 0;           /* i32 local: this thread's LMF address, re-derived at each push/pop */
	int eh_dispatch_ti = -1, eh_endcatch_ti = -1;  /* functype indices: (i32,i32)->i32 dispatch + ()->void end_catch */
	int resid_void_ti = -1;            /* ()->void: mono_wasm_jit_continue_unwind. Registered UNCONDITIONALLY --
	                                    * the residual/vcall/delegate threw==1 sites occur in non-EH methods too,
	                                    * where eh_endcatch_ti is never set up. */
	int nrefslots G_GNUC_UNUSED = 0;                 /* number of reference vregs routed to the GC ref shadow stack */
	/* MONO_WASM_JIT_SLOTZERO: dead-slot kill chains, built by the SLOTLIVE walk (classifier block) and
	 * consumed by the emit loop below. Both iterate the identical bb/ins sequence, so a plain running
	 * instruction ordinal links them: sl_kill_head[ord] = first vreg whose slot dies right BEFORE the
	 * ord-th instruction (its last use), sl_kill_next[vreg] = next vreg dying at the same ordinal. */
	int *sl_kill_head = NULL;          /* [nins] ordinal -> first dying vreg, or -1 */
	/* [nvreg] MONO_WASM_JIT_LCSE: vreg holds a reference with NO pin slot (SLOTLIVE elided it). Such a
	 * value must never be cached or reused by LCSE — see WJ_LCSE_UNPINNED at the LOADM macro. NULL when
	 * slot elision is not in play, in which case every isref vreg is pinned and the question is moot. */
	guint8 *lcse_nopin = NULL;
	int *sl_kill_next = NULL;          /* [nvreg] chain */
	int addrbase_idx = 0;              /* i32 local: addressable-locals frame base address (OP_LDADDR) */
	int addr_tmp_idx [4] = { 0, 0, 0, 0 }; /* per-type scratch locals for addr-frame stores (i32/i64/f32/f64) */
	gboolean has_rrem = FALSE;         /* TRUE: method has an inline-lowered OP_RREM (float32 %) */
	int rrem_tmp = -1;                 /* two f64 scratch locals for it: |x|, then the remainder; |y| */
	int naddrbytes = 0;                /* total bytes of addressable-locals frame (8 per address-taken local) */
	WjCtx lc;
	int *bbidx;
	int structured_cfg_kind = WJ_CFG_DISPATCH;
	int structured_loop_h = -1, structured_loop_l = -1;
	WjNest nest;   /* plan A5: valid when structured_cfg_kind == WJ_CFG_NESTED */
	gboolean cfg_sorted = FALSE;   /* plan A5 v2: wj_cfg_sort reordered the blocks (fallthrough goes to out_bb [0]) */
	WasmFuncType extra_types [WJ_EXTRA_TYPES_MAX]; /* callee functypes for call_indirect, after T0/T1 */
	int nextra = 0;
	/* Where this method's extra callee types start WITHIN ITS OWN type block: T0 = the method, T1 = the
	 * entry thunk, T2.. = the extras. So it is the constant 2, always, and every type index the body
	 * carries is block-relative.
	 *
	 * It used to be the block's base in the whole module's type section, adjusted per batch member --
	 * because type indices were ULEB-encoded inline at each call_indirect and could not be relocated
	 * afterwards, so the base had to be right at emit time. That is exactly what made a batch cost a full
	 * re-JIT per member. Type indices are now RELOCATIONS (WASM_RELOC_TYPE_*), resolved to
	 * ti_base + tpool when the member is framed, so the emitter no longer needs to know anything about
	 * which module it will end up in. Kept as a named constant rather than folded into the ~30 interning
	 * sites so that what the 2 means stays written down. */
	const int ti_base = 2;
	gboolean uses_calls = FALSE;
	gboolean uses_eh_tag = FALSE;   /* in-method EH landing pad -> import the C++ exception tag x.e */
	int eh_type_idx = -1;           /* type index of (i32)->void (the catch handler + the tag) */
	const char *fail = NULL;
	int fail_op = -1;
	/* Populated only for a signature-classification bail and consumed by the VERBOSE=2 log at done.
	 * Metadata is stable for the compile lifetime; no strings/layout work is paid on successful emits. */
	const char *fail_sig_site = NULL;
	MonoMethod *fail_sig_callee = NULL;
	MonoMethodSignature *fail_sig = NULL;
	int fail_sig_arg = -2;
	char *mname = mono_method_get_full_name (cfg->method);
	{ extern int mono_wasm_jit_b4, mono_wasm_jit_b4_t1, mono_wasm_jit_b4_down;   /* plan B4: record this body's weighted direct-call edges */
	  wj_edge_on = mono_wasm_jit_b4 && (cfg->wasm_jit_tier >= 2 || mono_wasm_jit_b4_t1 || (mono_wasm_jit_b4_down && cfg->wasm_jit_t1_down));
	  if (wj_edge_on) {
		if (!wj_edge_buf)
			wj_edge_buf = g_array_new (FALSE, FALSE, sizeof (WjEdge));
		g_array_set_size (wj_edge_buf, 0);
	  } }
#ifdef HOST_BROWSER
	/* Self-recursive calls reserve a fresh immutable slot pair. Failed reservations are intentionally
	 * orphaned: recycling is incompatible with baked f-slot identities and append-only descriptors. */
	int wj_self_e_slot = 0, wj_self_f_slot = 0;
#endif

	wasm_buf_init (&body);
	wasm_buf_init_relocs (&body);   /* every call site and type index in it is a hole; see wasm-encoder.h */
	{ extern int mono_wasm_jit_s2;   /* plan2x S2: "this method has a tier-1 body", for the hot rule's callee test */
	  if (G_UNLIKELY (mono_wasm_jit_s2 & 1) && cfg->wasm_jit_tier < 2)
		mono_wasm_jit_s2_note_body (cfg); }
	{ extern int mono_wasm_jit_origin;   /* gap-ledger origin map; changes no code (see WJO_TAG_LIST) */
	  if (G_UNLIKELY (mono_wasm_jit_origin)) {
		wasm_origins_init (&body);
		wj_origin_methods (&body, cfg);
		wasm_origin_set (&body, WJO_PROLOGUE, 0, -1, 0xFFFF);
	  } }

	/* Eligibility gates (critical for auto-JIT robustness on real code like Minecraft, where the
	 * emitter is fed thousands of method shapes): bail to the interpreter for features the wasm
	 * backend doesn't lower yet — unsupported EH/control-flow shapes and generic-shared methods
	 * (rgctx access the non-llvmonly path doesn't handle). Unknown opcodes still bail individually
	 * via the lowering switch's default case. */
#ifndef HOST_BROWSER
	/* EH ground-truth dump (MONO_WASM_JIT_DUMP_IR=<substr>): print clauses + per-bb region + opcode stream
	 * for clause-bearing methods. OFFLINE-CROSS ONLY: mono_inst_name returns a string in the cross but an
	 * int under the runtime's DISABLE_LOGGING, and the dump is only ever driven by the offline MONO_WASM_JIT_DUMP_IR
	 * env — so guard it out of the browser runtime build entirely (was breaking the runtime compile). */
	{ char *_df = g_getenv ("MONO_WASM_JIT_DUMP_IR");   /* read directly: the env-init fn isn't run in the offline cross */
	  if (_df && *_df && cfg->header && cfg->header->num_clauses > 0 && mname && strstr (mname, _df)) {
		guint _ci; MonoBasicBlock *_db; MonoInst *_di;
		g_printerr ("[eh-ir] === %s nclauses=%u ===\n", mname, cfg->header->num_clauses);
		for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			MonoExceptionClause *_c = &cfg->header->clauses [_ci];
			g_printerr ("[eh-ir] clause%u flags=%d(0=catch,2=finally,4=fault) try=[%u,%u) handler=[%u,%u)\n",
				_ci, _c->flags, _c->try_offset, _c->try_offset + _c->try_len, _c->handler_offset, _c->handler_offset + _c->handler_len);
		}
		for (_db = cfg->bb_entry; _db; _db = _db->next_bb) {
			g_printerr ("[eh-ir] bb%d region=0x%x:\n", _db->block_num, _db->region);
			MONO_BB_FOR_EACH_INS (_db, _di) {
				const char *_nm = mono_inst_name (_di->opcode);
				if ((_di->opcode == OP_CALL_HANDLER || _di->opcode == OP_BR) && _di->inst_target_bb)
					g_printerr ("[eh-ir]   %s -> bb%d\n", _nm, _di->inst_target_bb->block_num);
				else if (_di->opcode == OP_AOTCONST)
					g_printerr ("[eh-ir]   %s patch_type=%d p0=%p\n", _nm, (int) (gsize) _di->inst_p1, _di->inst_p0);
				else
					g_printerr ("[eh-ir]   %s\n", _nm);
			}
		}
	  }
	  g_free (_df);
	}
#endif /* !HOST_BROWSER (offline IR dump) */
	if (cfg->header && cfg->header->num_clauses > 0) {
		{ guint _ci;   /* in-method EH clauses always propagate via C++/wasm-EH (cppeh is the only model) */
		  /* Supported native wasm-EH clauses: catch (NONE), finally (FINALLY, including Java
		   * try-with-resources), and fault (FAULT). Filters still bail to the interpreter. */
		  for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			  int _f = cfg->header->clauses [_ci].flags;
			  if (_f == MONO_EXCEPTION_CLAUSE_FINALLY) {
				  eh_has_finally = TRUE;
			  }
			  else if (_f == MONO_EXCEPTION_CLAUSE_FAULT) {
				  /* fault: like finally but runs ONLY on the exception path (never on normal leave), then
				   * re-raises. IKVM emits these (compiler.cs BeginFaultBlock) for catch-all/synchronized
				   * cleanup in Mixin-transformed bytecode (Lithium/Starlight/indigo) whose shape its
				   * finally-pattern-matcher didn't recognize. The handler ends with OP_ENDFINALLY (== endfault),
				   * which re-raises on the exception path, and has no OP_CALL_HANDLER, so faithful IL lowering
				   * never runs it on the normal path. Reuse the finally_ind machinery (flag like finally);
				   * mono_wasm_jit_eh_dispatch treats FAULT identically to FINALLY. */
				  eh_has_finally = TRUE;
			  }
			  else if (_f != MONO_EXCEPTION_CLAUSE_NONE) { fail = "EH clause kind (filter not supported)"; goto done; }
		  }
		  /* Bail a method with a finally/fault clause whose TRY region is nested inside ANOTHER finally/fault
		   * clause's HANDLER region — i.e. a try/finally inside a finally body, e.g. `finally { using (x) {...} }`
		   * or `finally { lock (o) {...} }`. There is only ONE finally_ind continuation local: while the outer
		   * finally runs, the inner leave's OP_CALL_HANDLER overwrites finally_ind, so the outer OP_ENDFINALLY
		   * then branches to the inner continuation (silent wrong control flow) — or, on the exception path, the
		   * clobbered finally_ind makes the outer endfinally skip its re-raise (exception swallowed/escapes).
		   * Until each finally gets its own continuation slot, run such methods in the interpreter. */
		  { guint _a, _b;
		    for (_a = 0; _a < cfg->header->num_clauses && !fail; ++_a) {
			    MonoExceptionClause *_ca = &cfg->header->clauses [_a];
			    if (_ca->flags != MONO_EXCEPTION_CLAUSE_FINALLY && _ca->flags != MONO_EXCEPTION_CLAUSE_FAULT) continue;
			    for (_b = 0; _b < cfg->header->num_clauses; ++_b) {
				    MonoExceptionClause *_cb = &cfg->header->clauses [_b];
				    if (_b == _a) continue;
				    if (_cb->flags != MONO_EXCEPTION_CLAUSE_FINALLY && _cb->flags != MONO_EXCEPTION_CLAUSE_FAULT) continue;
				    if (_cb->try_offset >= _ca->handler_offset && _cb->try_offset < _ca->handler_offset + _ca->handler_len) {
					    fail = "nested finally inside finally handler (single finally_ind)"; goto done; }
			    }
		    } }
		  /* the prologue il_state island has a fixed data[] (WJ_ISLAND_DATA=256 in interp.c) that the GC
		   * scans for args+locals; bail EH methods that exceed it so the GC never reads past the buffer. */
		  { MonoMethodSignature *_s = mono_method_signature_internal (cfg->method);
		    int _nd = (_s && _s->hasthis ? 1 : 0) + (_s ? (int) _s->param_count : 0) + (int) cfg->header->num_locals;
		    if (_nd > 256) { fail = "EH method exceeds il_state data cap (256 args+locals)"; goto done; } }
		  eh_on = TRUE;   /* emit the outer-loop+try wrapper + catch landing pad (table built after bbidx) */
		}
	}
	if (cfg->gshared) { fail = "gshared method"; goto done; }
	/* save_lmf methods (synchronized bodies, some wrappers) lower emit_push_lmf/emit_pop_lmf IR:
	 * the LMF struct lives behind a VARLOADA'd local (a fixed 8-byte addr-frame slot here — smaller
	 * than MonoLMF) and its address is PUBLISHED into the thread's LMF chain; an exceptional unwind
	 * that skips the pop leaves the chain pointing into a popped frame -> the next LMF-chain walk
	 * (GC STW, sampling) reads garbage as metadata. Historically this path never ran (the TLS
	 * getter asserted at compile); now that it compiles, bail it to the interpreter cleanly. */
	if (cfg->method->save_lmf) { fail = "save_lmf (LMF push/pop) not supported"; goto done; }
	/* Marshalling wrappers (managed-to-native icall/pinvoke, native-to-managed, runtime-invoke) carry LMF
	 * save/restore, the native fptr baked as an iconst, handle/byref marshal stores and coop-GC transition
	 * IR — shapes the isref classifier + raw membase-store lowering mishandle, producing a ref store through
	 * a garbage/stale object base that scribbles the heap/metadata (confirmed live:
	 * System.Reflection.MonoMethodInfo:get_method_attributes, a hot IKVM-reflection icall wrapper -> OBJGUARD
	 * kind 2 wild store -> intermittent ArrayIndexOutOfBounds / mono_metadata_token_table assert). These
	 * wrappers are ~a native call, so the JIT upside is near zero; bail them to the interpreter until their
	 * codegen is verified. The synchronized wrapper (MONO_WRAPPER_SYNCHRONIZED) and its inner
	 * (MONO_WRAPPER_OTHER) are a separate, supported path and are NOT bailed here.
	 * MONO_WASM_JIT_MARSHAL_WRAPPERS=1 used to revert this and is DELETED: the reverted arm is not an
	 * A/B, it is a live memory-corruption bug. Their marshalling IR (LMF save/restore, the native fptr
	 * baked as an iconst, handle/byref marshal stores, coop-GC transitions) produces a ref store through
	 * a garbage/stale object base that the isref classifier and the raw membase-store lowering
	 * mishandle -> wild store -> intermittent heap/metadata corruption. Confirmed live, not theorised:
	 * System.Reflection.MonoMethodInfo:get_method_attributes -> OBJGUARD kind 2 -> AIOOBE / the
	 * mono_metadata_token_table assert. */
	switch (cfg->method->wrapper_type) {
	case MONO_WRAPPER_MANAGED_TO_NATIVE:
	case MONO_WRAPPER_NATIVE_TO_MANAGED:
	case MONO_WRAPPER_RUNTIME_INVOKE:
		fail = "marshalling wrapper (unsupported IR shape)"; goto done;
	default:
		break;
	}
	/* A call here passes the generic-sharing context in MONO_ARCH_RGCTX_REG (an out-arg register set by
	 * set_rgctx_arg) — e.g. this concrete method calls a gsharedvt/shared callee like
	 * Array.GetGenericValueImpl<T>. The wasm backend does NOT forward that register (neither the direct
	 * f-slot call nor the residual call_interp path carries it), so the callee would run with a garbage
	 * rgctx -> its gsharedvt mrgctx init populates a wrong/empty GOT slot -> a later call_indirect of
	 * that slot traps "null function". Bail the whole method to the interpreter, which forwards the
	 * rgctx correctly. (Shared callees never get an f-slot anyway — they bail just above — so the call
	 * would always take the rgctx-less residual; bailing the caller is the only safe option until we
	 * marshal the rgctx through.)
	 *
	 * The rgctx lift: instead of bailing the whole method, each rgctx call
	 * is routed per-site to the interp residual (mono_wasm_jit_call_interp), which derives the generic
	 * context from the concrete inflated call->method and is rgctx-correct (interp_entry, and the AOT
	 * fastpath's do_jit_call-via-gsharedvt_out wrapper, both handle it). The INLINE_AOT direct-body path is
	 * skipped for rgctx calls because it can only bake the callee's extra arg/rgctx, not the separate CALLSITE
	 * runtime generic context carried in MONO_ARCH_RGCTX_REG;
	 * indirect/virtual rgctx calls still bail. This unblocks IKVM EH methods whose catch block calls the
	 * generic ExceptionHelper.MapException<T> (the #1 render-path blocker after the EH gate). */

	for (i = 0; i < nvreg; ++i) {
		li [i] = -1;
		refslot [i] = -1;
		addrslot [i] = -1;
	}

	/* argument valtypes -> wasm params, from the one shared ABI descriptor. Using the same
	 * mono_wasm_get_call_info for the self-sig and for call sites is what keeps the registered
	 * f_sig_id and callers' baked dep_sig structurally identical (they hash the same ftype).
	 * A scalar-vtype arrives as its single field's wasm scalar, while MINI's body addresses the
	 * argument as a struct. Reconstruct it at entry: ref-bearing scalar structs use a GC-scanned
	 * ref-shadow slot (-2); ref-free structs use an address-frame slot populated by the prologue. */
	mono_wasm_get_call_info (sig, &self_ci);
	if (!self_ci.valid) {
		fail = self_ci.fail_reason;
		fail_sig_site = "self"; fail_sig_callee = cfg->method; fail_sig = sig; fail_sig_arg = self_ci.fail_arg;
		goto done;
	}
	for (i = 0; i < nargs; ++i) {
		if (self_ci.args [i].kind == WJ_ARG_VTYPE_BYADDR) {
			/* by-addr vtype arg: the wasm param IS the address of the caller-owned copy. All body
			 * access is decomposed OP_LDADDR (+ MEMBASE loads/stores / a wbarrier-copy icall) on the
			 * arg var — the pre-pass marks it with the -3 sentinel and OP_LDADDR just re-pushes the
			 * param. Mutation through it writes the caller's per-call copy: correct byval semantics
			 * (LLVMArgVtypeByRef precedent). The pointer itself is non-heap (caller C-stack frame /
			 * interp args area), so the vreg is never seeded ref and stays a fast wasm param. */
			param_types [i] = WASM_I32;
			vt [cfg->args [i]->dreg] = WASM_I32;
			li [cfg->args [i]->dreg] = i;
			addrslot [cfg->args [i]->dreg] = -3;
			self_has_byaddr = TRUE;
			continue;
		}
		if (self_ci.args [i].kind == WJ_ARG_VTYPE_SCALAR) {
			int av = cfg->args [i]->dreg;
			MonoClass *ak = mono_class_from_mono_type_internal (self_ci.args [i].type);
			if (av < 0 || av >= nvreg || !ak) { fail = "scalar-vtype arg var"; goto done; }
			param_types [i] = self_ci.args [i].wtype;
			vt [av] = self_ci.args [i].wtype;
			/* The prologue reads wasm param i directly. Body accesses use reconstructed storage. */
			li [av] = -1;
			if (m_class_has_references (ak) || m_class_has_ref_fields (ak)) {
				if (self_ci.args [i].wtype != WASM_I32) { fail = "scalar-vtype ref width"; goto done; }
				addrslot [av] = -2;
			} else {
				naddrbytes = (naddrbytes + 7) & ~7;
				addrslot [av] = naddrbytes;
				naddrbytes += 8;
			}
			continue;
		}
		if (self_ci.args [i].kind != WJ_ARG_SCALAR) { fail = "arg type"; goto done; }
		param_types [i] = self_ci.args [i].wtype;
		vt [cfg->args [i]->dreg] = self_ci.args [i].wtype;
		li [cfg->args [i]->dreg] = i;
	}
	nwparams = nargs;
	if (self_ci.vret_byaddr) {
		/* hidden vret: a TRAILING i32 param holding the caller's return-buffer address, bound to
		 * cfg->vret_addr (created by mono_arch_create_vars when ret storage is ArgValuetypeAddrInIReg;
		 * method_to_ir already lowered every `ret` into a store through it — after decompose that is
		 * MEMBASE stores / a mono_gc_wbarrier_range_copy icall taking this pointer). The wasm return
		 * type is void. Like by-addr args the pointer is non-heap and stays a fast local; the prologue
		 * ref-arg copy loop only walks cfg->args, so it never touches this param. */
		int vrd = cfg->vret_addr ? cfg->vret_addr->dreg : -1;
		if (vrd < 0 || vrd >= nvreg) { fail = "vret no var"; goto done; }
		/*
		 * THE HIDDEN VRET PARAM PUSHES THE ARITY ONE PAST WHAT wj_call_info BOUNDED.
		 *
		 * wj_call_info refuses `nargs > WASM_FUNCTYPE_MAX_PARAMS` (mini-wasm-ir.inc) and self_ci.valid is
		 * checked above, so nargs is in range -- but nwparams becomes nargs + 1 here, so a signature with
		 * exactly MAX args returning a vtype BY ADDRESS produced MAX + 1 and aborted the process at
		 * `g_assert (nparams <= WASM_FUNCTYPE_MAX_PARAMS)` in wj_body_take, several hundred lines later
		 * and with no hint that the arity was the cause.
		 *
		 * The CALL side already handles exactly this (`ci->ftype.nparams + 1 > WASM_FUNCTYPE_MAX_PARAMS`
		 * -> valid = FALSE, "call nargs"); the SELF side simply never got the same check. Refuse here,
		 * through the emitter's ordinary bail, so the method loses its tier and the run continues.
		 *
		 * Note param_types is mempool-allocated at nargs + 2, so this was never a buffer overrun -- the
		 * assert really was the first thing to go wrong, unlike the mark_bb_in_region case where the
		 * array was indexed before the assert could fire.
		 */
		if (nargs + 1 > WASM_FUNCTYPE_MAX_PARAMS) { fail = "self nargs+vret"; goto done; }
		param_types [nargs] = WASM_I32;
		vt [vrd] = WASM_I32;
		li [vrd] = nargs;
		nwparams = nargs + 1;
	}
	ret_vt = self_ci.ret.wtype;   /* WASM_VOID for void and hidden-vret returns, else the scalar ret valtype */

	/*
	 * Infer vreg valtypes from defining opcodes.
	 *
	 * MOVES ARE DELIBERATELY EXCLUDED and handled by a unification pass further down (search
	 * "unify move valtypes"). A move is the one shape whose operand type comes from its operands
	 * rather than from itself, and mono does not always spell it correctly -- leaving SSA turns a
	 * phi into a move whose opcode comes from the phi's, and mono_ssa_compute has no STACK_R4 case,
	 * so an f32 phi can reach codegen as an integer OP_MOVE. Taking the opcode at its word there
	 * declares an f32 vreg to be i32, and the result is a module that only fails at INSTANTIATE
	 * (WJC_INVALID -- and under module batching that loses every sibling method too), which is the
	 * one failure mode this emitter must never have. Deriving the type from the operands instead
	 * makes the answer independent of how the move happens to be spelled.
	 */
	{ extern int mono_wasm_jit_cfg_nest;   /* plan A5 v2: reorder BEFORE the first pass that walks the layout */
	  if (mono_wasm_jit_cfg_nest >= 2 && cfg->header && cfg->header->num_clauses == 0)
		cfg_sorted = wj_cfg_sort_if_rotated (cfg);
	}
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MONO_BB_FOR_EACH_INS (bb, ins) {
			WasmValtype rt;
			if (ins->opcode == OP_RREM)
				has_rrem = TRUE;
			if (wj_ins_is_move (ins->opcode))
				continue;
			rt = wasm_valtype_of_opcode (ins->opcode);
			if (!rt && (ins->opcode == OP_CALL || ins->opcode == OP_FCALL || ins->opcode == OP_LCALL || ins->opcode == OP_RCALL
					|| ins->opcode == OP_CALL_REG || ins->opcode == OP_FCALL_REG || ins->opcode == OP_LCALL_REG || ins->opcode == OP_RCALL_REG
					|| ins->opcode == OP_CALL_MEMBASE || ins->opcode == OP_FCALL_MEMBASE || ins->opcode == OP_LCALL_MEMBASE || ins->opcode == OP_RCALL_MEMBASE)) {
				MonoCallInst *c = (MonoCallInst *) ins;
				if (c->signature && c->signature->ret->type != MONO_TYPE_VOID)
					rt = wasm_valtype_of_type (c->signature->ret);
			}
			if (rt && ins->dreg >= 0 && ins->dreg < nvreg && li [ins->dreg] < 0) {
				/* Two defs with different wasm valtypes (possible once opt passes coalesce
				 * vregs): last-writer-wins would emit a type-invalid module that only fails
				 * at instantiate (WJC_INVALID, whole-module loss). Bail cleanly instead. */
				if (vt [ins->dreg] && vt [ins->dreg] != rt) { fail = "vreg valtype conflict"; fail_op = ins->opcode; goto done; }
				vt [ins->dreg] = rt;
			}
		}
	}

	/* OP_LDADDR pre-pass: find every address-taken local and route it to the addressable-locals frame.
	 * A var whose address is taken (ldloca/ldarga -> OP_LDADDR, inst_p0 = the var) can't live in a wasm
	 * local — wasm locals have no address — so back it with an 8-byte slot in the per-thread addr frame:
	 * all of its OP_MOVE loads/stores route through that memory (wasm_ld/wasm_st via addrslot), and
	 * OP_LDADDR yields addrbase+offset to hand to the callee. Supported: non-ref scalar locals (8-byte
	 * slot); value-type locals under MONO_WASM_JIT_LDADDR_VTYPE (full-size slot), including REF-BEARING
	 * ones under MONO_WASM_JIT_LDADDR_VTYPE_REF — the addr slots live in the C-stack frame which sgen
	 * scans conservatively (see the frame doc above), so embedded refs pin their referents like any AOT
	 * struct in a C local. Still bailed: ref/byref SCALAR locals (a lone ref slot wants the ref-shadow
	 * region, only the scalar-ref vtype sentinel -2 does that today) and address-taken ARGUMENTS (still
	 * in a wasm param). Anything unsupported bails the whole method (the prior behaviour for any
	 * OP_LDADDR). There is no knob for the pass itself -- it always runs; MONO_WASM_JIT_LDADDR_VTYPE and
	 * MONO_WASM_JIT_LDADDR_VTYPE_REF gate only which SHAPES it accepts.
	 * NB: NOT HOST_BROWSER-gated. This is a pure compile-time classification pass (it assigns addrslot
	 * offsets / decides which ldaddr shapes are supported); the addr-frame runtime helpers it feeds
	 * (the C-stack frame prologue, baked by the emit below) is HOST_BROWSER-gated separately. Un-gating it
	 * lets the offline cross-compiler dump reach the REAL ldaddr gate (e.g. "ldaddr of vtype with refs")
	 * instead of the spurious "ldaddr unsupported var" the emit hits when no slot was ever assigned. */
	{
	{
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			MONO_BB_FOR_EACH_INS (bb, ins) {
				MonoInst *var;
				int vv;
				WasmValtype lvt;
				if (ins->opcode != OP_LDADDR)
					continue;
				var = (MonoInst *) ins->inst_p0;
				if (!var || !var->inst_vtype) { fail = "ldaddr no var"; fail_op = OP_LDADDR; goto done; }
				vv = var->dreg;
				if (vv < 0 || vv >= nvreg) { fail = "ldaddr var vreg"; fail_op = OP_LDADDR; goto done; }
				if (addrslot [vv] >= 0)
					continue;   /* already assigned (same local addressed at multiple sites) */
				if (addrslot [vv] == -3)
					continue;   /* by-addr vtype ARG: the wasm param already holds the address; OP_LDADDR re-pushes it */
				if (li [vv] >= 0) {
					/* A scalar ARGUMENT lives in a wasm param, which has no address. MONO_WASM_JIT_LDADDR_ARG homes
					 * it in an 8-byte addr-frame slot instead, exactly like the scalar-vtype params above: li goes
					 * to -1, so every body access goes through the slot (wasm_ld/wasm_st test addrslot first), and
					 * the prologue's param reconstruction loop copies wasm param ai into it (arg_spill). Kept to
					 * clause-free methods and ref-free scalars. */
					int ai2;
					for (ai2 = 0; ai2 < nargs; ++ai2)
						if (cfg->args [ai2] && cfg->args [ai2]->dreg == vv)
							break;
					if (!mono_wasm_jit_ldaddr_arg || ai2 >= nargs || cfg->header->num_clauses ||
					    self_ci.args [ai2].kind != WJ_ARG_SCALAR ||
					    m_type_is_byref (var->inst_vtype) || mini_type_is_reference (var->inst_vtype) ||
					    wasm_valtype_group (vt [vv]) < 0) {
						fail = "ldaddr of arg local"; fail_op = OP_LDADDR; goto done;
					}
					li [vv] = -1;
					naddrbytes = (naddrbytes + 7) & ~7;
					addrslot [vv] = naddrbytes;
					naddrbytes += 8;
					arg_spill [ai2] = 1;
					switch (mini_get_underlying_type (var->inst_vtype)->type) {
					case MONO_TYPE_I1:                         addr_ldkind [vv] = 1; break;
					case MONO_TYPE_U1: case MONO_TYPE_BOOLEAN: addr_ldkind [vv] = 2; break;
					case MONO_TYPE_I2:                         addr_ldkind [vv] = 3; break;
					case MONO_TYPE_U2: case MONO_TYPE_CHAR:    addr_ldkind [vv] = 4; break;
					default: break;
					}
					wj_count (WJC_LDADDR_ARG);
					continue;
				}
				if (m_type_is_byref (var->inst_vtype) || mini_type_is_reference (var->inst_vtype)) {
					/* R321 (MONO_WASM_JIT_LDADDR_REF): home it in its ref-shadow slot, as the scalar-ref vtype below.
					 * vt[vv] keeps its scalar valtype: plain moves of the local still happen, through the slot. */
					extern int mono_wasm_jit_ldaddr_ref;
					if (mono_wasm_jit_ldaddr_ref) {
						addrslot [vv] = -2;
						wj_count (WJC_LDADDR_REF_LOCAL);
						continue;
					}
					fail = "ldaddr of ref/byref local"; fail_op = OP_LDADDR; goto done;
				}
				lvt = wasm_valtype_of_type (var->inst_vtype);
				if (lvt == 0 || lvt == WASM_VOID || wasm_valtype_group (lvt) < 0) {
					/* Non-scalar (valuetype) address-taken local. DEFAULT: bail. MONO_WASM_JIT_LDADDR_VTYPE=1
					 * backs it with a full-size addr-frame slot (field access via OP_LDADDR + MEMBASE);
					 * vt[vv] stays 0 so a scalar/bulk value access bails. */
					MonoClass *vk;
					int vsize;
					/* MONO_WASM_JIT_LDADDR_VTYPE gated this and shipped 1: OP_LDADDR of a NON-SCALAR
					 * ref-free valuetype local, via a full-size addr-frame slot. Its off-arm bailed the
					 * whole method. Exonerated on the corruption question -- jit17 corrupted with it OFF
					 * -- and kept gated only for repro parity, which is no longer a reason. */
					vk = mono_class_from_mono_type_internal (var->inst_vtype);
					if (!vk) { fail = "ldaddr vtype no class"; fail_op = OP_LDADDR; goto done; }
					vsize = mono_class_value_size (vk, NULL);
					if (m_class_has_references (vk) || m_class_has_ref_fields (vk)) {
						/* Ref-bearing vtype. A SCALAR one (single managed-ref field, e.g.
						 * RuntimeTypeHandle{RuntimeType}) keeps the ref-shadow slot (sentinel addrslot=-2;
						 * refslot assigned after the isref pass): OP_LDADDR yields refbase+slot*4, the field
						 * store/load track it as a pinning root, and the store card-barrier marks a harmless
						 * card (wasm32: no overlapping cards). A MULTI-FIELD one gets a normal full-size
						 * addr-frame slot under MONO_WASM_JIT_LDADDR_VTYPE_REF: the addr frame lives in the
						 * C-stack frame, which sgen scans CONSERVATIVELY (frame doc above wasm_ld) — embedded
						 * refs over-pin their referents, the exact guarantee AOT'd structs in C locals rely
						 * on. The prologue zero-fill keeps the GC from ever scanning garbage in the slot.
						 * (The old unconditional "GC-unsafe frame" bail predated the addr frame's move onto
						 * the C stack, when it really was an unscanned g_malloc arena.) */
						MonoType *setype = NULL;
						if (vsize <= 8 && mini_wasm_is_scalar_vtype (var->inst_vtype, &setype) && setype && mini_type_is_reference (setype)) {
							addrslot [vv] = -2;
							continue;
						}
						/* MONO_WASM_JIT_LDADDR_VTYPE_REF gated this and shipped 1: REF-BEARING non-scalar
						 * vtype locals in the addr frame. Its off-arm bailed the whole method. */
						/* else: fall through to the full-size slot assignment below */
					}
					if (vsize <= 0 || vsize > 4096) { fail = "ldaddr vtype size"; fail_op = OP_LDADDR; goto done; }
					naddrbytes = (naddrbytes + 7) & ~7;
					addrslot [vv] = naddrbytes;
					naddrbytes += (vsize + 7) & ~7;
					continue;
				}
				addrslot [vv] = naddrbytes;
				naddrbytes += 8;   /* 8 bytes/slot: 8-aligned, covers i64/f64; zero-init by the prologue frame fill */
				vt [vv] = lvt;     /* the local is only written via the escaped pointer in some shapes -> set its valtype explicitly */
				/* record sub-word width+signedness so wasm_addr_ld reads it back with a width-correct narrow
				 * load (a narrow callee write through &local leaves the upper bytes stale). */
				switch (mini_get_underlying_type (var->inst_vtype)->type) {
				case MONO_TYPE_I1:                       addr_ldkind [vv] = 1; break;   /* load8_s  (sbyte) */
				case MONO_TYPE_U1: case MONO_TYPE_BOOLEAN: addr_ldkind [vv] = 2; break; /* load8_u  (byte/bool) */
				case MONO_TYPE_I2:                       addr_ldkind [vv] = 3; break;   /* load16_s (short) */
				case MONO_TYPE_U2: case MONO_TYPE_CHAR:  addr_ldkind [vv] = 4; break;   /* load16_u (ushort/char) */
				default: break;
				}
			}
		}
	} }

	/* By-addr call-arg copy regions: every call site passing a vtype BY ADDRESS gets its own
	 * ALIGN8(vsize) region in the addr frame per such arg. The arg emit (wj_emit_one_call_arg)
	 * memory.copies the value in and passes the region's address, so a callee that mutates its byval
	 * copy can never touch the caller's live value, and a call in a loop re-copies each iteration by
	 * construction (the copy is emitted inline immediately before every call — an invariant future
	 * opt passes must not hoist). Residual calls use the same copy (uniform path; the interp copies
	 * it once more onto its GC-scanned stack before the callee runs). Regions live INSIDE the
	 * [refbase, entry_sp) frame: conservatively scanned (embedded refs pin their referents) and
	 * restored-past on every exit/landing pad — never below SP. Offsets land in WjCallArgs::copyoff
	 * (see mono_wasm_emit_call). */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MONO_BB_FOR_EACH_INS (bb, ins) {
			MonoCallInst *call;
			MonoMethodSignature *csig;
			WasmCallInfo cci;
			WjCallArgs *w;
			int ai;
			switch (ins->opcode) {
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
			case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
			case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
				break;
			case OP_VCALL2_MEMBASE:
				if (!mono_wasm_jit_vcall_vret)
					continue;
				break;
			default:
				continue;
			}
			call = (MonoCallInst *) ins;
			csig = call->signature;
			if (!call->method || !csig || !call->call_info)
				continue;   /* icalls take scalars only; a missing capture bails at the emit */
			mono_wasm_get_call_info (csig, &cci);
			if (!cci.valid)
				continue;   /* the emit bails with cci.fail_reason */
			w = (WjCallArgs *) call->call_info;
			for (ai = 0; ai < cci.nargs && ai < w->nargs; ++ai) {
				if (cci.args [ai].kind != WJ_ARG_VTYPE_BYADDR)
					continue;
				naddrbytes = (naddrbytes + 7) & ~7;
				w->copyoff [ai] = naddrbytes;
				naddrbytes += (cci.args [ai].vsize + 7) & ~7;
			}
		}
	}

	/*
	 * The return vreg's valtype is the wasm function type's, full stop. The epilogue loads
	 * cfg->ret->dreg and the module declares the function returns ret_vt, so anything else is
	 * unrepresentable. Seeding it here rather than inferring it matters because mono_arch_emit_setret
	 * writes cfg->ret->dreg with a MOVE, and moves no longer declare a type (see below) -- this is the
	 * authoritative source that replaces the one the move used to provide. Skipped for a hidden-vret
	 * method, whose wasm return is void and whose cfg->ret is a value type in the addr frame.
	 */
	if (ret_vt != WASM_VOID && cfg->ret && cfg->ret->dreg >= 0 && cfg->ret->dreg < nvreg &&
	    li [cfg->ret->dreg] < 0 && addrslot [cfg->ret->dreg] < 0) {
		if (vt [cfg->ret->dreg] && vt [cfg->ret->dreg] != ret_vt) { fail = "ret valtype conflict"; goto done; }
		vt [cfg->ret->dreg] = ret_vt;
	}

	/*
	 * R410: a LOCAL VARIABLE that is read but never written in this body. It reads its initial value, zero -- CIL locals
	 * are zero-initialised -- but its zero-init store can be gone: liveness ignores an unreachable block, so dead-code
	 * elimination drops the init when the only reads sit in one (selectTicks' tier-2 body with T2_COLD_PRED: `not_null
	 * r; move r375 <- r; throw r` in a block no edge reaches, from an inlined callee). With no definition the vreg had no
	 * valtype, so no wasm local, and its first read bailed the whole compile at "move" -- demoting the hottest server
	 * method to tier 1. Type it from its variable here, before moves propagate types, and keep it out of the ref shadow
	 * stack below: never written, it is null forever, and a wasm local is zero-initialised. */
	wj_undef = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
	{
		guint8 *has_def = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
			MONO_BB_FOR_EACH_INS (bb, ins)
				if (ins->dreg >= 0 && ins->dreg < nvreg)
					has_def [ins->dreg] = 1;
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
			MONO_BB_FOR_EACH_INS (bb, ins) {
				int sr [3] = { ins->sreg1, ins->sreg2, ins->sreg3 }, k;
				for (k = 0; k < 3; ++k) {
					int v = sr [k];
					MonoInst *var;
					WasmValtype uvt;
					if (v < 0 || v >= nvreg || has_def [v] || vt [v] || li [v] >= 0 || addrslot [v] != -1 || wj_undef [v])
						continue;
					var = get_vreg_to_inst (cfg, v);
					if (!var || (var->opcode != OP_LOCAL && var->opcode != OP_ARG) || !var->inst_vtype || m_type_is_byref (var->inst_vtype))
						continue;
					uvt = wasm_valtype_of_type (var->inst_vtype);
					if (!uvt)
						continue;
					vt [v] = uvt;
					wj_undef [v] = 1;
					wj_count (WJC_UNDEF_LOCAL);
				}
			}
	}

	/*
	 * Unify move valtypes.
	 *
	 * Every other valtype seed is final by now (params above, defining opcodes, the OP_LDADDR
	 * pre-pass), so this is the point at which a move can be resolved from its operands. A move
	 * asserts only that its two vregs hold the SAME wasm type, so propagate a known type across it
	 * in whichever direction is known, to a fixpoint (a chain of moves needs more than one sweep;
	 * the loop terminates because every iteration that continues has typed at least one more vreg).
	 *
	 * Both sides typed and DIFFERENT is a genuine disagreement, not a spelling problem: emitting it
	 * would produce `local.get <f32>; local.set <i32>`, which validates nowhere. Bail the method.
	 *
	 * A move whose operands are both untyped simply stays untyped; the vreg then gets no wasm local
	 * and the first wasm_ld/wasm_st of it bails cleanly -- which is what would have happened anyway,
	 * since an untyped source cannot be loaded.
	 */
	{
		gboolean changed = TRUE;
		while (changed) {
			changed = FALSE;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int s, d;
					if (!wj_ins_is_move (ins->opcode))
						continue;
					s = ins->sreg1; d = ins->dreg;
					if (s < 0 || s >= nvreg || d < 0 || d >= nvreg)
						continue;
					if (vt [s] && vt [d]) {
						if (vt [s] != vt [d]) { fail = "move valtype conflict"; fail_op = ins->opcode; goto done; }
						continue;
					}
					/* An ARG vreg's type comes from the wasm param signature and is authoritative;
					 * it is already set, so this only ever fills in the other side. */
					if (vt [s] && !vt [d]) { vt [d] = vt [s]; changed = TRUE; }
					else if (vt [d] && !vt [s]) { vt [s] = vt [d]; changed = TRUE; }
				}
			}
		}
	}

	/*
	 * MONO_WASM_JIT_COALESCE — let several vregs share one wasm local when their live ranges are disjoint.
	 *
	 * li[] is otherwise strictly one local per vreg with no reuse at all: AABB:combine declares 58 where
	 * teavm's optimiser needs 5 for the identical Java source.
	 *
	 * Liveness here is a real backward dataflow, NOT first-mention/last-mention. The cheap version is
	 * unsound across a back edge:
	 *
	 *     bb0: t1 = 5
	 *     bb1: use t1 ; t2 = 7 ; use t2 ; br bb1      <- loop header
	 *
	 * t1's mentions all precede t2's, so a mention-range allocator shares their local -- and on the
	 * second iteration `use t1` reads t2's value. Propagating live_in/live_out keeps t1 live across the
	 * whole loop, which is what makes the overlap visible.
	 *
	 * Uses are enumerated exactly as the SLOTLIVE walk does, and for the same reason: a missed use
	 * shortens a range and silently shares a still-live local. That means the generic source registers,
	 * plus a store-membase `dreg` (which is an ADDRESS, not a result), plus the positional call-arg vregs
	 * hanging off call_info. cfg->ret->dreg is additionally pinned live to the end of the method because
	 * the epilogue loads it OUTSIDE the instruction stream, where no walk of the IR can see it.
	 *
	 * Each valtype group holds exactly one wasm type (wasm_valtype_group), so sharing a slot within a
	 * group can never produce a type mismatch.
	 */
	int *lslot = NULL;
	/*
	 * Clause-free methods only. The liveness below walks bb->out_bb, and mono does NOT thread exception
	 * edges through it -- a throw reaches its handler via the EH machinery, not a CFG successor. So a vreg
	 * whose only live path into a handler is that implicit edge looks dead here, and coalescing it would
	 * miscompile the handler. jbox2d has no EH at all, so the bench cannot expose this; Minecraft is full
	 * of it. Widening this needs the intervals of everything live in a try region extended across the
	 * whole region plus its handlers.
	 */
	/* MONO_WASM_JIT_COALESCE ships 1 and is INERT AT RUNTIME by construction -- that is fine, and it is
	 * the point. V8 lowers local.get/set/tee to zero instructions
	 * (turboshaft-graph-interface.cc:1030-1043), so sharing one wasm local between vregs with disjoint
	 * live ranges cannot change execution. It is kept for WIRE SIZE, which decides whether a body clears
	 * V8's 500-byte inlining cap -- a real effect on a different axis. Do not "re-measure" it against
	 * fps; the V8 source closes that question. */
	{ extern int mono_wasm_jit_s4, mono_wasm_jit_s4_sel_lo, mono_wasm_jit_s4_sel_hi;   /* plan2x S4: BEFORE local coalescing, the ref classifier and SLOTLIVE -- it gives a cached value a new use, and
	                    * an interval computed without it lets coalescing hand that value's local to another vreg (R432) */
	  if (G_UNLIKELY (mono_wasm_jit_s4) && !fail && ((mono_wasm_jit_s4 & 2) || cfg->wasm_jit_tier >= 2)) {
		int s4h = mname ? (int) (g_str_hash (mname) % 4096) : 0;
		if (mono_wasm_jit_s4_sel_lo < 0 || (s4h >= mono_wasm_jit_s4_sel_lo && s4h <= mono_wasm_jit_s4_sel_hi)) {
			int s4n = wj_s4_run (cfg, mname, nvreg, TRUE, TRUE);
			if (mono_wasm_jit_s4_sel_lo >= 0 && s4n > 0)
				printf ("WASM_JIT_S4_APPLIED\t%d\t%d\t%d\t%s\ts4-pre-coalesce\n", cfg->wasm_jit_tier, s4n, s4h, mname ? mname : "?");
		}
	  } }
	if (nvreg > 0 && cfg->header->num_clauses == 0) {
		int words = (nvreg + 31) / 32;
		int nbb2 = 0, ordn = 0, bbn;
		MonoBasicBlock *bbl; MonoInst *insl;

		for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
			nbb2++;
		/* Bound the dataflow: 4 bitsets of nbb2*words words. Past this a method keeps the old
		 * one-local-per-vreg assignment rather than paying unbounded compile time/memory. */
		if (nbb2 > 0 && (gsize) nbb2 * words <= 65536) {
			int *bb_lo = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nbb2);
			int *bb_hi = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nbb2);
			int *vlo = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
			int *vhi = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
			guint32 *bs_use = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_def = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_in  = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			guint32 *bs_out = (guint32 *) mono_mempool_alloc0 (cfg->mempool, sizeof (guint32) * (gsize) nbb2 * words);
			int iter, changed = 1;

#define WJ_BS(BASE, BB) ((BASE) + (gsize) (BB) * words)
#define WJ_BS_GET(BASE, BB, V) ((WJ_BS (BASE, BB) [(V) >> 5] >> ((V) & 31)) & 1u)
#define WJ_BS_SET(BASE, BB, V) do { WJ_BS (BASE, BB) [(V) >> 5] |= 1u << ((V) & 31); } while (0)

			for (i = 0; i < nvreg; ++i) { vlo [i] = -1; vhi [i] = -1; }

			/* per-bb upward-exposed uses + defs, and the ordinal range of each bb */
			bbn = 0;
			for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
				bb_lo [bbn] = ordn;
				MONO_BB_FOR_EACH_INS (bbl, insl) {
					int srcs [MONO_MAX_SRC_REGS];
					int nsrc = mono_inst_get_src_registers (insl, srcs);
					gboolean dreg_is_base = FALSE;
					int u, d;
					switch (insl->opcode) {
					case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
					case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG:
					case OP_STORER8_MEMBASE_REG:
					case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM:
					case OP_STOREI2_MEMBASE_IMM:
					case OP_ATOMIC_STORE_I1: case OP_ATOMIC_STORE_U1:   /* R323b */
					case OP_ATOMIC_STORE_I2: case OP_ATOMIC_STORE_U2: case OP_ATOMIC_STORE_I4: case OP_ATOMIC_STORE_U4:
					case OP_ATOMIC_STORE_R4: case OP_ATOMIC_STORE_R8:   /* R360 */
					case OP_ATOMIC_STORE_I8: case OP_ATOMIC_STORE_U8:   /* J1c, MONO_WASM_JIT_ATOMIC_I8 */
						dreg_is_base = TRUE; break;
					default: break;
					}
#define WJ_CO_USE(V) do { int _v = (V); \
	if (_v >= 0 && _v < nvreg) { \
		if (!WJ_BS_GET (bs_def, bbn, _v)) WJ_BS_SET (bs_use, bbn, _v); \
		if (vlo [_v] < 0 || ordn < vlo [_v]) vlo [_v] = ordn; \
		if (ordn > vhi [_v]) vhi [_v] = ordn; \
	} } while (0)
					for (u = 0; u < nsrc; ++u)
						WJ_CO_USE (srcs [u]);
					if (dreg_is_base)
						WJ_CO_USE (insl->dreg);
					if (MONO_IS_CALL (insl)) {
						MonoCallInst *cl = (MonoCallInst *) insl;
						WjCallArgs *cw = (WjCallArgs *) cl->call_info;
						if (cw) {
							for (u = 0; u <= cw->nargs; ++u)
								WJ_CO_USE (wj_arg_vreg (cl, u));
						}
					}
#undef WJ_CO_USE
					d = dreg_is_base ? -1 : insl->dreg;
					if (d >= 0 && d < nvreg) {
						WJ_BS_SET (bs_def, bbn, d);
						if (vlo [d] < 0 || ordn < vlo [d]) vlo [d] = ordn;
						if (ordn > vhi [d]) vhi [d] = ordn;
					}
					ordn++;
				}
				bb_hi [bbn] = ordn - 1;   /* < bb_lo for an empty bb; handled below */
			}

			/* live_out[b] = U live_in[succ];  live_in[b] = use[b] U (live_out[b] - def[b])
			 *
			 * Block index lookups are precomputed. Walking cfg->bb_entry to find a successor's index
			 * inside the fixpoint would make this O(nbb^2) per iteration, which is real money on the
			 * 7000-instruction methods here. */
			MonoBasicBlock **bbarr = (MonoBasicBlock **) mono_mempool_alloc (cfg->mempool, sizeof (MonoBasicBlock *) * nbb2);
			int *bbnum2idx = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
			for (i = 0; i <= (int) cfg->max_block_num; ++i)
				bbnum2idx [i] = -1;
			bbn = 0;
			for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
				bbarr [bbn] = bbl;
				if (bbl->block_num >= 0 && bbl->block_num <= (int) cfg->max_block_num)
					bbnum2idx [bbl->block_num] = bbn;
			}
			for (iter = 0; changed && iter < nbb2 + 4; ++iter) {
				changed = 0;
				/* reverse layout order converges fast for a mostly-forward CFG */
				for (bbn = nbb2 - 1; bbn >= 0; --bbn) {
					MonoBasicBlock *b = bbarr [bbn];
					int k, w;
					for (k = 0; k < b->out_count; ++k) {
						MonoBasicBlock *o = b->out_bb [k];
						int oi = (o && o->block_num >= 0 && o->block_num <= (int) cfg->max_block_num)
							? bbnum2idx [o->block_num] : -1;
						if (oi < 0) continue;
						for (w = 0; w < words; ++w) {
							guint32 nv = WJ_BS (bs_out, bbn) [w] | WJ_BS (bs_in, oi) [w];
							if (nv != WJ_BS (bs_out, bbn) [w]) { WJ_BS (bs_out, bbn) [w] = nv; changed = 1; }
						}
					}
					for (w = 0; w < words; ++w) {
						guint32 nv = WJ_BS (bs_use, bbn) [w] | (WJ_BS (bs_out, bbn) [w] & ~WJ_BS (bs_def, bbn) [w]);
						if (nv != WJ_BS (bs_in, bbn) [w]) { WJ_BS (bs_in, bbn) [w] = nv; changed = 1; }
					}
				}
			}

			/* A vreg live at a block boundary is live across that whole block, back edges included. */
			for (bbn = 0; bbn < nbb2; ++bbn) {
				int hi = bb_hi [bbn] < bb_lo [bbn] ? bb_lo [bbn] : bb_hi [bbn];
				for (i = 0; i < nvreg; ++i) {
					if (!WJ_BS_GET (bs_in, bbn, i) && !WJ_BS_GET (bs_out, bbn, i))
						continue;
					if (vlo [i] < 0 || bb_lo [bbn] < vlo [i]) vlo [i] = bb_lo [bbn];
					if (hi > vhi [i]) vhi [i] = hi;
				}
			}

			/* The epilogue reads cfg->ret->dreg after the last instruction; no IR walk can see that. */
			if (cfg->ret && cfg->ret->dreg >= 0 && cfg->ret->dreg < nvreg) {
				int rv = cfg->ret->dreg;
				if (vlo [rv] < 0) vlo [rv] = 0;
				vhi [rv] = ordn;
			}
			/* Never-mentioned vregs collapse onto [0,0]: nothing can read them, since every emitter read
			 * goes through a source reg, a call_info slot, or cfg->ret, all enumerated above. */
			for (i = 0; i < nvreg; ++i)
				if (vlo [i] < 0) { vlo [i] = 0; vhi [i] = 0; }

			/* Greedy allocation in increasing start order (counting sort on vlo keeps it deterministic).
			 * A slot is reusable once its current occupant's range has ENDED strictly before this one
			 * starts. */
			{
				int nord = ordn + 2;
				int *cntb = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nord);
				int *order = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *slot_end [4] = { NULL, NULL, NULL, NULL };
				int nslot [4] = { 0, 0, 0, 0 };
				int pos = 0, gi;

				lslot = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				for (i = 0; i < nvreg; ++i) lslot [i] = -1;
				for (gi = 0; gi < 4; ++gi)
					slot_end [gi] = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);

				for (i = 0; i < nvreg; ++i) cntb [vlo [i] < nord ? vlo [i] : nord - 1]++;
				for (i = 1; i < nord; ++i) cntb [i] += cntb [i - 1];
				for (i = nvreg - 1; i >= 0; --i) {
					int key = vlo [i] < nord ? vlo [i] : nord - 1;
					order [--cntb [key]] = i;
				}

				for (pos = 0; pos < nvreg; ++pos) {
					int v = order [pos], g, s, chosen = -1;
					if (li [v] >= 0 || vt [v] == 0 || addrslot [v] >= 0)
						continue;
					g = wasm_valtype_group (vt [v]);
					if (g < 0) continue;
					/* Scanning is capped: past 256 slots the search cost stops being worth the reuse. */
					for (s = 0; s < nslot [g] && s < 256; ++s)
						if (slot_end [g] [s] < vlo [v]) { chosen = s; break; }
					if (chosen < 0) chosen = nslot [g]++;
					slot_end [g] [chosen] = vhi [v];
					lslot [v] = chosen;
				}
			}
#undef WJ_BS
#undef WJ_BS_GET
#undef WJ_BS_SET
		}
	}

	/* assign locals for non-arg typed vregs, grouped by type (skip address-taken locals: they live in the
	 * addr frame, not a wasm local) */
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0 || addrslot [i] >= 0)
			continue;
		g = wasm_valtype_group (vt [i]);
		if (g < 0) continue;
		/* Coalesced: the group needs as many locals as its highest slot index, not one per vreg. Both
		 * this counting pass and the assignment pass below key off the same lslot[], so they cannot
		 * disagree about the group size. */
		if (lslot && lslot [i] >= 0) {
			if (lslot [i] + 1 > cnt [g]) cnt [g] = lslot [i] + 1;
		} else {
			cnt [g]++;
		}
	}
	/* reserve one extra i32 local at the end of the i32 group for the dispatch index ($blk) */
	dispatch_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* reserve one more i32 local for the interp-residual scratch-buffer pointer (used by the
	 * "callee not jitted" path below to call_indirect mono_wasm_jit_call_interp; dead in methods
	 * with no such call, which is a harmless unused declared local) */
	scratch_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* reserve two more i32 locals: the GC ref-slot frame base, and a scratch for ref stores */
	refbase_idx = nwparams + cnt [0];
	cnt [0] += 1;
	rtmp_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one more i32 local: the C-stack SP captured at entry (every exit stackRestores to it) */
	spentry_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one more i32 local: the frame base handed to JIT callees. A dedicated local rather than an alias
	 * of refbase_idx because a LAZY ref frame has no valid refbase until ENSURE_REF_FRAME runs, and a
	 * frame-free/no-GC callee can be called before that point -- aliasing would hand it a garbage base.
	 * GATED on the knob, unlike every other local here, so that knob-off output stays BYTE-IDENTICAL and
	 * tiershape.py can gate this change as a pure refactor -- against a same-binary CONTROL pair, not an
	 * absolute floor (R241: the same-binary floor is 0.69% on the current tier). An always-declared
	 * local is free at run time (V8 lowers local ops to zero instructions) but it is not free in WIRE
	 * SIZE, and wire size is what decides whether a body clears V8's 500-byte inlining cap. */
	/* three i32 locals for OP_WASM_JIT_ALLOC_FAST (TLS address/SgenThreadInfo, object, new tlab_next). Declared
	 * ONLY in a method that allocates inline, so every other method's output is unchanged. */
	{
		gboolean has_alloc_fast = FALSE;
		for (bb = cfg->bb_entry; bb && !has_alloc_fast; bb = bb->next_bb)
			MONO_BB_FOR_EACH_INS (bb, ins)
				if (ins->opcode == OP_WASM_JIT_ALLOC_FAST) { has_alloc_fast = TRUE; break; }
		if (has_alloc_fast) {
			alloc_tmp_idx = nwparams + cnt [0];
			cnt [0] += 3;
		}
	}
	/* one more i32 local for the inline virtual-IC fast path's resolved f-slot (dead in methods with no
	 * virtual call — a harmless unused declared local) */
	vc_fslot_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the VCALL_AOT dispatch kind (0/1/2 from mono_wasm_jit_vcall_aot_target); dead in
	 * methods with no virtual call — a harmless unused declared local */
	vc_aotkind_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* i32 local: this->vtable, live across the AOT-vcall IC key+vtab_check compares (VCALL_AOT_IC) */
	aic_vtab_idx = nwparams + cnt [0];
	cnt [0] += 1;
	aic_ti_idx = nwparams + cnt [0];
	cnt [0] += 1;
	aic_rgctx_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* i32 local: base address of the delegate-IC entry for the way being checked.
	 *
	 * Purely a WIRE-BYTE optimisation, and wire bytes are load-bearing: V8 rejects a callee for inlining
	 * outright when its body exceeds --wasm-inlining-max-size (500 in stock Chrome, see
	 * v8/src/wasm/inlining-tree.h SmallEnoughToInline). Each way reads seven fields off the same entry, and
	 * re-materialising the address per field costs 6 bytes a time (42 B); setting it once and reading a local
	 * costs 8 + 7*2 = 22 B, so -20 B per way and -80 B on a site still at four ways. The largest MHC cohort
	 * (2020 one-arg stubs) sits ~64 B over the cap, so bytes at this scale decide inlinability. */
	dic_base_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* two i32 locals caching &wj_slot_live / &wj_slot_live_cap for the inline f-slot-IC liveness check
	 * (dead in methods with no vcall — harmless unused declared locals) */
	slotlive_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	slotlive_cap_idx = nwparams + cnt [0];
	cnt [0] += 1;
	vpic_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* the same pair for the worker-local delegate recipe PIC (dead unless the method has a
	 * Delegate.Invoke site, which is the common case — an unused declared local costs nothing) */
	dpic_ptr_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* the hoisted capacity VALUES (see the note at their declaration) */
	vpic_capval_idx = nwparams + cnt [0];
	cnt [0] += 1;
	dpic_capval_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* two i32 locals for the in-method EH catch landing pad (dead in methods with no clauses) */
	eh_exc_idx = nwparams + cnt [0];
	cnt [0] += 1;
	eh_h_idx = nwparams + cnt [0];
	cnt [0] += 1;
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_eh_rec;
		ehrec_on = eh_on && mono_wasm_jit_eh_rec;
		if (ehrec_on) {
			ehlmf_idx = nwparams + cnt [0];
			cnt [0] += 1;
		}
	}
#endif
	/* Only when the method actually has EH: a non-EH method never calls enter_island, so reserving this would
	 * cost every JITted method a local for nothing. */
	{
	}
	/* one i32 local for the in-method finally indicator (dead in non-finally methods): OP_CALL_HANDLER sets
	 * it to the continuation bb index; the catch landing pad sets it to -1; OP_ENDFINALLY branches on it. */
	finally_ind_idx = nwparams + cnt [0];
	cnt [0] += 1;
	/* one i32 local for the addressable-locals frame base (OP_LDADDR; dead in methods with no address-taken
	 * local), plus the i32 scratch for addr-frame stores (only when there ARE such locals). */
	addrbase_idx = nwparams + cnt [0];
	cnt [0] += 1;
	if (naddrbytes > 0) { addr_tmp_idx [0] = nwparams + cnt [0]; cnt [0] += 1; }   /* i32 store scratch */
	base [0] = nwparams;
	base [1] = base [0] + cnt [0];
	/* dedicated i64 local at the end of the i64 group for the inline virtual-IC value (atomically loaded) */
	vc_ic_idx = base [1] + cnt [1];
	cnt [1] += 1;
	if (naddrbytes > 0) { addr_tmp_idx [1] = base [1] + cnt [1]; cnt [1] += 1; }   /* i64 store scratch */
	base [2] = base [1] + cnt [1];
	if (naddrbytes > 0) { addr_tmp_idx [2] = base [2] + cnt [2]; cnt [2] += 1; }   /* f32 store scratch */
	base [3] = base [2] + cnt [2];
	if (naddrbytes > 0) { addr_tmp_idx [3] = base [3] + cnt [3]; cnt [3] += 1; }   /* f64 store scratch */
	if (has_rrem) { rrem_tmp = base [3] + cnt [3]; cnt [3] += 2; }
	for (i = 0; i < nvreg; ++i) {
		int g;
		if (li [i] >= 0 || vt [i] == 0 || addrslot [i] >= 0)   /* addr-taken locals live in the frame, not a wasm local */
			continue;
		g = wasm_valtype_group (vt [i]);
		if (g < 0) continue;
		li [i] = base [g] + ((lslot && lslot [i] >= 0) ? lslot [i] : run [g]++);
	}
	groups [0].type = WASM_I32; groups [0].count = cnt [0];
	groups [1].type = WASM_I64; groups [1].count = cnt [1];
	groups [2].type = WASM_F32; groups [2].count = cnt [2];
	groups [3].type = WASM_F64; groups [3].count = cnt [3];

	/* V8 caps a function at kV8MaxWasmFunctionLocals = 50,000 (v8/src/wasm/wasm-limits.h:50), counted as
	 * PARAMS PLUS declared locals: num_locals_ is seeded from the signature's parameter count
	 * (function-body-decoder-impl.h:1880) and accumulated per local group (:1938), and the group whose
	 * running total crosses the cap is rejected with "local count too large" (:1920-1921).
	 *
	 * Our count is the fixed ~22-local scaffolding block plus one per live vreg, so a large enough body
	 * reaches it. 1.16.1's net.minecraft.class_3580 (datafixer.fix.BlockStateFlattening) .cctor does:
	 * thousands of register() calls in one method, 1,885,636 bytes emitted.
	 *
	 * This MUST be refused here and not left to instantiation. The WASM_JIT_INVALID path below marks
	 * every instantiation failure RETRIABLE on purpose -- a per-worker OOM is transient and must not
	 * condemn a method process-wide (R245 Stage 2c). But a body that is invalid is invalid every time,
	 * and re-emission is deterministic, so a deterministic failure routed into that path recompiles
	 * forever: measured 15 attempts x ~1.5 s = 22.4 s of the boot window with `registered` perfectly
	 * flat, which is CLAUDE.md's "decide whether the condition can ever clear" trap on a new path.
	 * A `fail` string here is a PERMANENT bail (retriable is only set for "callee not jitted"), so the
	 * method is condemned once and the interpreter runs it.R289. */
	{ int _nloc = nwparams + cnt [0] + cnt [1] + cnt [2] + cnt [3];
	  if (_nloc > WASM_MAX_FUNCTION_LOCALS) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_LOCALS_OVERFLOW);
		fail = "declared locals exceed the V8 function-locals cap";
		goto done;
	  } }

	lc.li = li; lc.nvreg = nvreg; lc.refslot = NULL; lc.ref_wt = NULL; lc.refbase = refbase_idx; lc.rtmp = rtmp_idx;
	/* In lazy methods spentry is zero until materialization, then becomes the nonzero entry SP.
	 * Reuse it as the active marker instead of reserving one extra i32 local in every JIT method. */
	lc.lazy_frame = 0; lc.frame_active = spentry_idx;
	lc.vt = vt; lc.addrbase = addrbase_idx;
	{ /* addrslot must be visible to the OP_LDADDR emit when there are ONLY sentinel entries (refvt -2 /
	   * by-addr arg -3, no addr-frame bytes) — otherwise naddrbytes==0 would NULL it and those ldaddrs
	   * misfire as "unsupported var". */
		gboolean _have_sentinel = FALSE; int _i; for (_i = 0; _i < nvreg; ++_i) if (addrslot [_i] == -2 || addrslot [_i] == -3) { _have_sentinel = TRUE; break; }
		lc.addrslot = (naddrbytes > 0 || _have_sentinel) ? addrslot : NULL; }
	lc.addr_ldkind = (naddrbytes > 0) ? addr_ldkind : NULL;
	lc.addr_tmp [0] = addr_tmp_idx [0]; lc.addr_tmp [1] = addr_tmp_idx [1];
	lc.addr_tmp [2] = addr_tmp_idx [2]; lc.addr_tmp [3] = addr_tmp_idx [3];
	/* MONO_WASM_JIT_STOREGUARD (debug): pre-register the (i32,i32)->void functype for the per-store
	 * mono_wasm_jit_check_store call_indirect so wasm_st/wasm_addr_st can reference it by index. Done here
	 * (before body emission, nextra==0) so check_ti is stable; force uses_calls so the table is imported. */
	lc.storeguard = 0; lc.objguard = 0; lc.check_ti = -1;
	/* Helper imports are no longer interned during emission at all. A call to a fixed table index leaves a
	 * relocation naming the address and the signature; slots are assigned by the assembler, in first-use
	 * order, once the whole membership is known. So there is nothing to seed here, nothing to copy back
	 * afterwards, and no way for a member's `call <slot>` to name an import its module does not declare.
	 *
	 * That last failure mode is why batching once forced helper imports OFF for every batched member --
	 * which silently reverted every helper call, and after AOT_IMPORTS every inline-AOT callee call, to
	 * constant-index call_indirect, and made every batching measurement taken before Round 105
	 * "co-location MINUS all function imports". It cannot recur: there is no longer a count for a member
	 * to be out of step with. */
#ifdef HOST_BROWSER
	{
		extern int mono_wasm_jit_storeguard, mono_wasm_jit_objguard;
		if ((mono_wasm_jit_storeguard || mono_wasm_jit_objguard) && nextra < WJ_EXTRA_TYPES_MAX) {
			WasmFuncType cst; memset (&cst, 0, sizeof (cst));
			cst.params [0] = WASM_I32; cst.params [1] = WASM_I32; cst.nparams = 2; cst.ret = WASM_VOID;
			extra_types [nextra] = cst; lc.check_ti = ti_base + nextra++;
			lc.storeguard = mono_wasm_jit_storeguard ? 1 : 0;
			lc.objguard = mono_wasm_jit_objguard ? 1 : 0;
			uses_calls = TRUE;
		}
	}
#endif
#ifdef HOST_BROWSER
	/* Route every vreg that MIGHT hold a managed pointer to the GC-scanned ref shadow stack instead of a
	 * (GC-invisible) wasm local: a reference held in a wasm local across a GC point (a residual interp call,
	 * which can allocate and trigger a moving collection; a loop safepoint) would be moved/collected out
	 * from under the JITted method -> dangling pointer.
	 *
	 * Policy: CONSERVATIVE-BY-DEFAULT, mirroring the interpreter's own interp_mark_stack ("scan a slot unless
	 * it is provably non-ref"). Every pointer-sized (i32 on wasm32) vreg is assumed to be a ref and is cleared
	 * only when we can PROVE it scalar: cfg->vreg_is_ref / ref+byref args are pinned outright, and a temporary
	 * stays a fast local only if EVERY definition is an integer/float op (wj_opcode_is_nonref), an OP_MOVE from
	 * an already-proven-scalar source, or a call returning a scalar. cfg->vreg_is_ref alone is insufficient —
	 * it covers only mono_compile_create_var vregs (named locals/args), not the temporaries that hold call
	 * results, field loads, etc. across a residual (e.g. `s = Intern(x); i = indexOf(s, c)`). Because the
	 * shadow stack is a conservative PINNING root, over-marking a non-ref i32 is harmless (it just pins
	 * whatever the value points at) while a MISSED ref is silent corruption — so we err toward marking. The
	 * proven-scalar arithmetic/compare/const/typed-load population stays in fast wasm locals; only
	 * genuinely-maybe-ref vregs go to memory.
	 *
	 * Implemented as a greatest fixed point on nonref[] (a vreg is provably-non-ref only if ALL definitions
	 * are scalar): start optimistic (all i32 candidates nonref), then knock a candidate down to ref the moment
	 * any defining instruction can produce a pointer; isref = the i32 vregs we could not prove scalar. */
	{
		gboolean *isref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		gboolean *nonref = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
		guint8 *sl_elide = NULL;   /* MONO_WASM_JIT_SLOTLIVE: isref vregs whose slot is elided (see the pass below); isref[] itself stays intact for the diagnostics (MISSEDREF, REFDIAG) */
		gboolean changed = TRUE;
		int pass;
		/* candidates: pointer-sized vregs (i64/f32/f64 can never hold a ref -> never shadow-stacked) */
		for (i = 0; i < nvreg; ++i)
			nonref [i] = (vt [i] == WASM_I32);
		/* vregs mono already knows are object refs OR managed pointers are definitely refs.
		 * With cfg->compute_gc_maps set for COMPILE_WASM, MINI's own
		 * marking populates both arrays structurally from the type system: create_var_for_vreg
		 * marks ref/byref vars, alloc_ireg_ref/_mp mark ref and interior-pointer TEMPS
		 * (ldelema, ldflda, the stfld write-barrier address, Unsafe.Add/AddByteOffset).
		 * These are the same facts the LLVM/native GC paths consume — the fixpoint below is
		 * just the closure of them over MOVE chains, pointer arithmetic and call returns. */
		for (i = 0; i < nvreg; ++i)
			if (vreg_is_ref (cfg, i) || vreg_is_mp (cfg, i))
				nonref [i] = FALSE;
		/* Explicitly mark reference-typed AND byref ARGUMENTS. mono_compile_create_var marks object-ref
		 * vregs via vreg_is_ref, but it never covers managed POINTERS: (a) a reference-type class's `this`
		 * carries a byref-flagged this_arg, so mini_type_is_reference() returns FALSE for it; (b) byref
		 * params (ref/out, Span<T>) and a valuetype's interior-pointer `this` are managed pointers too. All
		 * of these are otherwise left in a GC-invisible wasm local, so a method that holds one across a GC
		 * point (a residual, an f-slot call, a loop safepoint) dangles when the target object moves. Mark
		 * them here so the prologue copies them into the GC-scanned (conservatively pinning) ref shadow
		 * stack at entry: an object ref keeps the object alive+pinned; a byref pins whatever heap object it
		 * points into (a byref into native/stack memory pins nothing — harmless). */
		for (i = 0; i < nargs; ++i) {
			gboolean arg_is_ref;
			if (i == 0 && sig->hasthis)
				arg_is_ref = TRUE;   /* ref-type this = object ref; valuetype this = interior byref — both need GC tracking */
			else {
				MonoType *pt = sig->params [i - sig->hasthis];
				arg_is_ref = m_type_is_byref (pt) || mini_type_is_reference (pt);   /* byref (ref/out/Span) OR object ref */
			}
			if (arg_is_ref) {
				int av = cfg->args [i]->dreg;
				if (av >= 0 && av < nvreg)
					nonref [av] = FALSE;
			}
			/* scalar args keep their nonref candidate: no defining instruction in the body clears them */
		}
		/* plan2x F4 (& 1): a vreg EVERY def of which loads offset 0 -- MonoObject.vtable -- of an OBJECT holds a
		 * MonoVTable *, which lives in the memory manager (malloc), is never moved and is never collected while its
		 * class lives: pinning it pins nothing and costs a frame store per def. OBJECT, not merely a pointer: offset 0
		 * of a byref / struct address is a field, which may be a ref. The receiver test is mono's structural one
		 * (vreg_is_ref and not vreg_is_mp, as OBJGUARD uses) plus a reference type's `this`, which mono types as a
		 * byref this_arg (see the argument marking above). */
		gboolean *vtvreg = NULL;
		{ extern int mono_wasm_jit_f4;
		  if (mono_wasm_jit_f4 & 1) {
			guint8 *vt_seen = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
			guint8 *vt_bad = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
			int this_dreg = (sig->hasthis && cfg->args [0] && !m_class_is_valuetype (cfg->method->klass)) ? cfg->args [0]->dreg : -1;
			vtvreg = (gboolean *) mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * nvreg);
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int d = ins->dreg, b_ = ins->sreg1;
					if (d < 0 || d >= nvreg)
						continue;
					vt_seen [d] = 1;
					if (!(ins->opcode == OP_LOAD_MEMBASE && ins->inst_offset == MONO_STRUCT_OFFSET (MonoObject, vtable) &&
					      b_ >= 0 && b_ < nvreg &&
					      ((vreg_is_ref (cfg, b_) && !vreg_is_mp (cfg, b_)) || b_ == this_dreg)))
						vt_bad [d] = 1;
				}
			}
			for (i = 0; i < nvreg; ++i)
				if (vt_seen [i] && !vt_bad [i] && vt [i] == WASM_I32 && !vreg_is_ref (cfg, i) && !vreg_is_mp (cfg, i)) {
					vtvreg [i] = TRUE;
					wj_count (WJC_F4_VT_NONREF);
				}
		  } }
		/* knock candidates down to ref: a vreg is provably non-ref only if EVERY definition is scalar */
		for (pass = 0; changed && pass <= nvreg; ++pass) {
			changed = FALSE;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				MONO_BB_FOR_EACH_INS (bb, ins) {
					int d = ins->dreg;
					gboolean def_nonref;
					if (d < 0 || d >= nvreg || !nonref [d])
						continue;   /* not a candidate, or already proven a ref */
					if (vtvreg && vtvreg [d])
						def_nonref = TRUE;   /* plan2x F4: a vtable, see above */
					else if (ins->opcode == OP_MOVE)
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1]);
					else if ((ins->opcode == OP_IADD || ins->opcode == OP_ISUB)
							&& ins->type != STACK_OBJ && ins->type != STACK_MP)
						/* pointer arithmetic (OP_PADD/OP_PSUB alias these on wasm32): the result of
						 * add/sub is an interior pointer whenever either source might be a ref/mp —
						 * taint-propagate like OP_MOVE instead of trusting a scalar allow-list.
						 * Backstop for interior pointers that carry neither a STACK_MP type nor a
						 * structural mark. Scalar+scalar stays scalar, so ordinary integer math
						 * (and the write-barrier card-mark address computation) costs nothing.
						 * An explicitly OBJ/MP-typed add falls through to wj_opcode_is_nonref,
						 * whose type front-check classifies it ref unconditionally. */
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1])
							&& (ins->sreg2 >= 0 && ins->sreg2 < nvreg && nonref [ins->sreg2]);
					else if ((ins->opcode == OP_IADD_IMM || ins->opcode == OP_ISUB_IMM
							|| ins->opcode == OP_ADD_IMM || ins->opcode == OP_SUB_IMM
							|| ins->opcode == OP_IAND_IMM || ins->opcode == OP_AND_IMM)
							&& ins->type != STACK_OBJ && ins->type != STACK_MP)
						/* ptr+imm (field offset) / ptr&~mask (alignment): pointer-preserving unary shapes */
						def_nonref = (ins->sreg1 >= 0 && ins->sreg1 < nvreg && nonref [ins->sreg1]);
					else if (ins->opcode == OP_CALL || ins->opcode == OP_CALL_REG || ins->opcode == OP_CALL_MEMBASE) {
						/* a call result is a ref unless its return type is a scalar (int/float/native-int/ptr);
						 * byref/object returns must be pinned. OP_VCALL/FCALL/LCALL dregs aren't i32 -> not candidates. */
						MonoCallInst *c = (MonoCallInst *) ins;
						def_nonref = c->signature && !m_type_is_byref (c->signature->ret) && !mini_type_is_reference (c->signature->ret);
					} else
						def_nonref = wj_opcode_is_nonref (ins);
					if (!def_nonref) { nonref [d] = FALSE; changed = TRUE; }
				}
			}
		}
		/* isref = the live pointer-sized vregs we could not prove scalar (these go on the ref shadow stack) */
		for (i = 0; i < nvreg; ++i)
			isref [i] = (vt [i] == WASM_I32) && !nonref [i];
		{
			/* PRODUCTION FIX: the prove-non-ref fixpoint trusts mono's
			 * ins->type to spot refs, but that misses heap pointers in shapes it can't see (a movable object
			 * baked as a non-STACK_OBJ iconst, an interior-pointer add mis-typed as scalar, ...), leaving a live
			 * ref in a plain wasm local that dangles after a compacting GC — the confirmed intermittent
			 * corruption source. Close it cheaply: any vreg used as a DEREFERENCE — a MEMBASE load/store
			 * base or a virtual-call receiver — is a live pointer at that instruction, so pin it on the
			 * (conservative) ref shadow stack. Over-marking an unmanaged/addr-frame base is harmless (it pins
			 * nothing); a missed heap base is silent corruption. Only dereferenced pointers move to the shadow
			 * stack — not every i32 temporary. */
			{
			  {
				MonoBasicBlock *bbf; MonoInst *insf;
				int *dop = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nvreg);
				for (i = 0; i < nvreg; ++i) dop [i] = -1;
				for (bbf = cfg->bb_entry; bbf; bbf = bbf->next_bb)
					MONO_BB_FOR_EACH_INS (bbf, insf)
						if (insf->dreg >= 0 && insf->dreg < nvreg) dop [insf->dreg] = insf->opcode;
				for (bbf = cfg->bb_entry; bbf; bbf = bbf->next_bb) {
					MONO_BB_FOR_EACH_INS (bbf, insf) {
						int b = -1;
						switch (insf->opcode) {
						case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
						case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
						case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
						case OP_ATOMIC_LOAD_I1: case OP_ATOMIC_LOAD_U1: case OP_ATOMIC_LOAD_I2: case OP_ATOMIC_LOAD_U2:
						case OP_ATOMIC_LOAD_I4: case OP_ATOMIC_LOAD_U4: case OP_ATOMIC_LOAD_R4: case OP_ATOMIC_LOAD_R8:
						case OP_ATOMIC_ADD_I4: case OP_ATOMIC_EXCHANGE_U1: case OP_ATOMIC_EXCHANGE_U2: case OP_ATOMIC_EXCHANGE_I4:
						case OP_ATOMIC_CAS_U1: case OP_ATOMIC_CAS_U2: case OP_ATOMIC_CAS_I4:   /* R360 */
						case OP_ATOMIC_LOAD_I8: case OP_ATOMIC_LOAD_U8: case OP_ATOMIC_ADD_I8: case OP_ATOMIC_EXCHANGE_I8: case OP_ATOMIC_CAS_I8:   /* J1c */
							b = insf->sreg1; break;
						case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
						case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
						case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
						case OP_ATOMIC_STORE_I1: case OP_ATOMIC_STORE_U1:   /* R323b */
						case OP_ATOMIC_STORE_I2: case OP_ATOMIC_STORE_U2: case OP_ATOMIC_STORE_I4: case OP_ATOMIC_STORE_U4:
						case OP_ATOMIC_STORE_R4: case OP_ATOMIC_STORE_R8:   /* R360 */
						case OP_ATOMIC_STORE_I8: case OP_ATOMIC_STORE_U8:   /* J1c, MONO_WASM_JIT_ATOMIC_I8 */
							b = insf->dreg; break;
						case OP_CALL_MEMBASE: case OP_VCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_VOIDCALL_MEMBASE:
						case OP_VCALL2_MEMBASE:
							b = mono_wasm_jit_vcall_vret ? insf->sreg1 : -1; break;
						default: break;
						}
						/* Skip bases proven to be an addr-frame pointer (OP_LDADDR result = addrbase+slot, linear
						 * memory, never a heap ref) — pinning those is pure waste. Everything else that gets
						 * dereferenced could be a heap pointer -> pin it. */
						if (b >= 0 && b < nvreg && vt [b] == WASM_I32 && dop [b] != OP_LDADDR && !(vtvreg && vtvreg [b])) {
							/* Subsumption audit: with the structural seeds (GCMAPS) + add/sub taint, the
							 * fixpoint should already classify every dereferenced heap pointer — a flip
							 * here is a counterexample. Counted always (cheap), named under REFVERIFY.
							 * A long soak with REFBASES=1 and refbases_extra==0 proves REFBASES is
							 * subsumed and can stay off in production. */
							if (!isref [b]) {
								extern int mono_wasm_jit_refverify;
								if (mono_wasm_jit_stats)
									mono_wasm_jit_count (WJC_REFBASES_EXTRA);
								if (G_UNLIKELY (mono_wasm_jit_refverify)) {
									char *mn = mono_method_get_full_name (cfg->method);
									/* wj_opname, NOT mono_inst_name: under DISABLE_LOGGING (browser Release)
									 * mono_inst_name returns the opcode as an int — %s would wild-read it. */
									printf ("WASM_JIT_REFBASES_EXTRA: %s vreg R%d (def op %s, deref op %s) classified nonref by fixpoint\n",
										mn ? mn : "?", b, dop [b] >= 0 ? wj_opname (dop [b]) : "?", wj_opname (insf->opcode));
									g_free (mn);
								}
								isref [b] = TRUE;
							}
						}
					}
				}
			  } }
			/* R410: a never-written local is null for its whole life: no ref slot (one would be read unwritten). */
			for (i = 0; i < nvreg; ++i)
				if (wj_undef && wj_undef [i])
					isref [i] = FALSE;
			/* REFVERIFY cross-check (debug): the classification must be a superset of the structural
			 * marking — a vreg mono marked ref/mp that the fixpoint left as a plain wasm local is a
			 * lost seed, i.e. exactly the missed-ref shape that corrupts silently. Also flag marked
			 * vregs whose inferred wasm valtype is not pointer-sized (a type-confusion anomaly). */
			{ extern int mono_wasm_jit_refverify;
			  if (G_UNLIKELY (mono_wasm_jit_refverify)) {
				for (i = 0; i < nvreg; ++i) {
					gboolean marked = vreg_is_ref (cfg, i) || vreg_is_mp (cfg, i);
					if (!marked)
						continue;
					if (vt [i] == WASM_I32 && !isref [i]) {
						char *mn = mono_method_get_full_name (cfg->method);
						printf ("WASM_JIT_REFVERIFY: %s vreg R%d marked %s but classified NONREF (lost seed)\n",
							mn ? mn : "?", i, vreg_is_ref (cfg, i) ? "ref" : "mp");
						g_free (mn);
						if (mono_wasm_jit_refverify >= 2)
							g_assert_not_reached ();
					} else if (vt [i] != 0 && vt [i] != WASM_I32) {
						char *mn = mono_method_get_full_name (cfg->method);
						printf ("WASM_JIT_REFVERIFY: %s vreg R%d marked %s but valtype %d (not pointer-sized)\n",
							mn ? mn : "?", i, vreg_is_ref (cfg, i) ? "ref" : "mp", (int) vt [i]);
						g_free (mn);
					}
				}
			  } }
		}
		/* GC-POINT LIVENESS SLOT ELISION (MONO_WASM_JIT_SLOTLIVE): a frame slot exists so the
		 * conservative stack scan can see (and pin) the ref — but the scan only ever runs while this
		 * thread sits at a GC point (a call that can allocate, the OP_GC_SAFE_POINT STW poll, a raise).
		 * An isref vreg whose every def->use interval crosses NO GC point is therefore invisible to
		 * the collector for its whole life and can stay in a plain (unscanned) wasm local: no slot,
		 * no prologue copy, no pin. Rules, all conservative toward keeping the slot:
		 *   - seen (def or use) in more than one bb -> slot. Covers loops and EH: a mid-bb raise
		 *     enters a HANDLER bb, so a vreg live into any handler is multi-bb by definition and a
		 *     single-bb vreg can never be observed there. No dataflow needed.
		 *   - used AT a GC-point instruction -> slot. A call arg must stay pinned by the CALLER for
		 *     the callee's whole execution: the residual path stages args in an unscanned scratch
		 *     buffer, so the caller's slot can be the only thing keeping the arg alive.
		 *   - used at a later "gc generation" than its def (a GC-point ins executed in between) -> slot.
		 * Call args are read positionally from WjCallArgs (call->call_info), NOT sregs — they
		 * must be enumerated or an arg-only vreg would look dead and lose its slot. Def-position gen
		 * is recorded AFTER the gc bump so a call RESULT consumed before the next GC point can elide.
		 * The dreg of a MEMBASE store is its base — a USE, not a def (mirroring the REFBASES pass).
		 * Disabled under STOREGUARD/OBJGUARD so refslot stays a complete ref proxy for the guards. */
		{
			/* MONO_WASM_JIT_SLOTLIVE ships 1 (GC-point liveness slot elision). It stays disabled under
			 * STOREGUARD/OBJGUARD below, which is NOT a knob but a semantic requirement: those guards key
			 * ref-ness off refslot, so eliding a slot would change what they check. */
			gboolean slotlive_on = TRUE;
#ifdef HOST_BROWSER
			{
				extern int mono_wasm_jit_storeguard, mono_wasm_jit_objguard, mono_wasm_jit_guard_keep_slotlive;
				/* MONO_WASM_JIT_GUARD_KEEP_SLOTLIVE=1 keeps elision ON under the guards.
				 *
				 * The unconditional override below makes the guards unable to validate the one feature most
				 * likely to need it: enabling either guard silently turns SLOTLIVE off, so a corruption that
				 * only reproduces with elision on cannot be caught by the very tool built to catch it. That
				 * gap is already on record (a.txt: "OBJGUARD/STOREGUARD cannot validate SLOTLIVE because
				 * they disable it"), and it cost this investigation a full round.
				 *
				 * The override protects COVERAGE, not correctness. refslot is used as a complete "is a ref"
				 * proxy by exactly one check -- OBJGUARD kind 2 (is the stored VALUE a ref) -- and elision
				 * makes that proxy partial. Every other check is an address-range test that does not consult
				 * ref-ness at all: STOREGUARD kinds 0/1 run inside the slot store path, and OBJGUARD kinds
				 * 3/4/5 (byref base, generic membase, vcall receiver) bounds-check a computed address. Those
				 * stay sound with elision on; they simply see fewer stores. Partial coverage of the real
				 * configuration beats complete coverage of a configuration that does not reproduce. */
				if ((mono_wasm_jit_storeguard || mono_wasm_jit_objguard) && !mono_wasm_jit_guard_keep_slotlive)
					slotlive_on = FALSE;
			}
#endif
			if (slotlive_on) {
				guint8 *needs_slot = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				guint8 *sl_multi = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);      /* events span >1 bb */
				guint8 *sl_def_first = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);  /* first event was a DEF (no loop-carried use-before-def) */
				int *ev_bb = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg); /* bb of first event, -1 = unseen */
				int *def_gen = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * nvreg);
				int *last_use_ord = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *last_def_ord = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
				int *bb_last_gcp, *sl_bbidx; int nbbs = 0, nins = 0;
				MonoBasicBlock *bbl; MonoInst *insl;
				MonoInst *last_gcp_ins = NULL;
				int bbn, nelide = 0, ord = 0, last_gcp_ord = -1;
				gboolean sl_has_backedge = FALSE, prior_gcps_are_polls = TRUE;
				/* Raises the exemption hid anywhere in this method. The terminal-vcall handoff below is
				 * only sound while this is zero -- see there. */
				int method_skipped_raises = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
					nbbs++;
				bb_last_gcp = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (nbbs ? nbbs : 1));
				sl_bbidx = (int *) mono_mempool_alloc (cfg->mempool,
					sizeof (int) * ((int) cfg->max_block_num + 1));
				for (i = 0; i < nbbs; ++i)
					bb_last_gcp [i] = -1;
				for (i = 0; i <= (int) cfg->max_block_num; ++i)
					sl_bbidx [i] = -1;
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb)
					sl_bbidx [bbl->block_num] = bbn++;
				/* A layout-backward CFG edge can execute a textual "last use" again after the call.
				 * Terminal handoff therefore requires a DAG. This deliberately treats self-edges as
				 * backedges and declines the optimization rather than attempting loop liveness. */
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
					int oi;
					for (oi = 0; oi < bbl->out_count; ++oi) {
						MonoBasicBlock *out = bbl->out_bb [oi];
						int outi = (out && out->block_num >= 0 &&
							out->block_num <= (int) cfg->max_block_num) ?
							sl_bbidx [out->block_num] : -1;
						if (outi >= 0 && outi <= bbn)
							sl_has_backedge = TRUE;
					}
				}
				for (i = 0; i < nvreg; ++i) {
					ev_bb [i] = -1;
					last_use_ord [i] = -1;
					last_def_ord [i] = -1;
				}
				/* args are defined by the prologue: entry bb (walk ordinal 0), generation 0 */
				for (i = 0; i < nargs; ++i) {
					int av = cfg->args [i]->dreg;
					if (av >= 0 && av < nvreg) { ev_bb [av] = 0; def_gen [av] = 0; sl_def_first [av] = 1; }
				}
				/* EH landing pads and their runtime dispatch are deliberately excluded from the
				 * frame-free class. Otherwise, every instruction must be locally pure or a direct
				 * call to an already-published no-GC method. */
				method_nogc = cfg->header->num_clauses == 0;
#ifdef HOST_BROWSER
				{
					/* DOOMED blocks, for LAZY_COLD=2: backward reachability from the exit to a fixpoint. A bb
					 * that ENDS IN A THROW never reaches the exit whatever its edges say -- method-to-ir links
					 * every CEE_THROW block to end_bblock for liveness (method-to-ir.c, MONO_CEE_THROW), and
					 * reading that edge as real made every throw path look live (R294: doomed=0). Otherwise a bb
					 * reaches the exit if it IS the exit, is the last in layout (the emitter returns there when
					 * it falls off the end), has no out-edge at all (a missing edge must read HOT, never cold),
					 * or has a successor that does. Only without clauses: a handler resumes into this method,
					 * so a throw is not an exit. */
					extern int mono_wasm_jit_lazy_cold;
					if (mono_wasm_jit_lazy_cold >= 2 && cfg->header->num_clauses == 0 && nbbs > 0) {
						guint8 *reach = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nbbs);
						gboolean changed = TRUE;
						while (changed) {
							changed = FALSE;
							bbn = 0;
							for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
								int oi;
								gboolean ends_throw;
								if (reach [bbn])
									continue;
								ends_throw = bbl->last_ins && (bbl->last_ins->opcode == OP_NOT_REACHED ||
									bbl->last_ins->opcode == OP_THROW || bbl->last_ins->opcode == OP_RETHROW);
								if (ends_throw && bbl != cfg->bb_exit)
									continue;      /* never reaches the exit; its end_bblock edge is liveness-only */
								if (bbl == cfg->bb_exit || !bbl->next_bb || bbl->out_count == 0) {
									reach [bbn] = 1; changed = TRUE;
									continue;
								}
								for (oi = 0; oi < bbl->out_count; ++oi) {
									MonoBasicBlock *out = bbl->out_bb [oi];
									int outi = (out && out->block_num >= 0 && out->block_num <= (int) cfg->max_block_num) ?
										sl_bbidx [out->block_num] : -1;
									if (outi < 0 || reach [outi]) {   /* an edge we cannot index is HOT */
										reach [bbn] = 1; changed = TRUE;
										break;
									}
								}
							}
						}
						lazy_doomed = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nbbs);
						for (i = 0; i < nbbs; ++i)
							lazy_doomed [i] = !reach [i];
					}
				}
#endif
				gcp_walked = TRUE;
				bbn = 0;
				for (bbl = cfg->bb_entry; bbl; bbl = bbl->next_bb, ++bbn) {
					int gc_gen = 0;
					/* Raises exempted since the current generation began. A forwarded call argument may
					 * only skip the used-at-a-GC-point clause below while this is zero -- see there. */
					int gen_skipped_raises = 0;
					/* RAISE_NOGC=2 is the positive control: same exemption as level 1, but without the
					 * gen_skipped_raises term in WJ_SL_USE below -- i.e. the codegen that measured 4/4
					 * dead boots at a 4 MB nursery. Read once here, not per use. */
					const int sl_fwd_guard = mono_wasm_jit_raise_nogc != 2;
					MONO_BB_FOR_EACH_INS (bbl, insl) {
						int srcs [MONO_MAX_SRC_REGS];
						int nsrc = mono_inst_get_src_registers (insl, srcs);
						/* Two views, identical unless RAISE_NOGC==1 (see wj_ins_is_published_gcpoint):
						 * `gcp` drives this method's own def/use generations, `gcp_pub` drives every
						 * claim published to other code. */
						gboolean gcp = wj_ins_is_effective_gcpoint (cfg, insl);
						gboolean gcp_pub = wj_ins_is_published_gcpoint (cfg, insl);
						gboolean pinned_vforward = gcp && wj_ins_is_pinned_vcall_forward (insl);
						gboolean forward_call = gcp &&
							(wj_direct_admitted_fslot (cfg, insl) > 0 || pinned_vforward);
						MonoCallInst *sl_call = MONO_IS_CALL (insl) ? (MonoCallInst *) insl : NULL;
						WjCallArgs *sl_cw = sl_call ? (WjCallArgs *) sl_call->call_info : NULL;
						gboolean dreg_is_base = FALSE;
						int u, d;
						if (gcp_pub) {
							method_nogc = FALSE;
							effective_gcp_count++;
							if (insl->opcode != OP_GC_SAFE_POINT && !wj_ins_is_raise (insl)) {
								if (lazy_doomed && lazy_doomed [bbn])
									doomed_gcp_count++;
								else
									hot_gcp_count++;
							}
							if (last_gcp_ins && last_gcp_ins->opcode != OP_GC_SAFE_POINT)
								prior_gcps_are_polls = FALSE;
							last_gcp_ins = insl;
							last_gcp_ord = ord;
						}
						switch (insl->opcode) {
						case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
						case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
						case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
						case OP_ATOMIC_STORE_I1: case OP_ATOMIC_STORE_U1:   /* R323b */
						case OP_ATOMIC_STORE_I2: case OP_ATOMIC_STORE_U2: case OP_ATOMIC_STORE_I4: case OP_ATOMIC_STORE_U4:
						case OP_ATOMIC_STORE_R4: case OP_ATOMIC_STORE_R8:   /* R360 */
						case OP_ATOMIC_STORE_I8: case OP_ATOMIC_STORE_U8:   /* J1c, MONO_WASM_JIT_ATOMIC_I8 */
							dreg_is_base = TRUE; break;
						default: break;
						}
#define WJ_SL_USE(v,FWD) do { int _u = (v); gboolean _fwd = (FWD); \
	if (_u >= 0 && _u < nvreg && isref [_u]) { \
		if (ev_bb [_u] == -1) { ev_bb [_u] = bbn; needs_slot [_u] = TRUE; } /* use before any def (loop-carried) */ \
		else if (ev_bb [_u] != bbn) { sl_multi [_u] = 1; needs_slot [_u] = TRUE; } \
		else if (def_gen [_u] != gc_gen) needs_slot [_u] = TRUE; \
		/* `_fwd` lets a forwarding call's argument skip this clause because the callee is supposed to
		 * take ownership of the root (wj_ins_is_pinned_vcall_forward). That leaves the argument with NO
		 * slot at all -- which was survivable only by accident: without the raise exemption a raise in
		 * the span bumps gc_gen, the clause above fires, and the argument gets pinned anyway. With the
		 * exemption both clauses go quiet and the handoff guarantee is the only thing left, and the
		 * measured crash (always mini_llvmonly_init_vtable_slot -> resolve_vcall on a filler-class
		 * receiver) says it does not hold on the PIC miss path. So honour `_fwd` only when no raise was
		 * skipped in this generation. */ \
		/* sl_fwd_guard is 0 only at RAISE_NOGC=2, the POSITIVE CONTROL, which reproduces the pre-fix
		 * codegen by dropping exactly this term. Everything else about level 2 is level 1. */ \
		else if (gcp && (!_fwd || (sl_fwd_guard && gen_skipped_raises > 0))) needs_slot [_u] = TRUE; \
		last_use_ord [_u] = ord; \
	} } while (0)
						for (u = 0; u < nsrc; ++u) {
							gboolean is_forward_arg = FALSE;
							int au;
							if (forward_call && sl_cw)
								for (au = 0; au <= sl_cw->nargs; ++au)
									if (wj_arg_vreg (sl_call, au) == srcs [u]) {
										is_forward_arg = TRUE;
										break;
									}
							WJ_SL_USE (srcs [u], is_forward_arg);
						}
						if (dreg_is_base)
							WJ_SL_USE (insl->dreg, FALSE);
						/* positional call-arg uses (WjCallArgs): [0..n-1] args, [n] the vret addr */
						if (sl_call) {
							if (sl_cw) {
								for (u = 0; u <= sl_cw->nargs; ++u)
									WJ_SL_USE (wj_arg_vreg (sl_call, u), forward_call);
							}
						}
#undef WJ_SL_USE
						if (gcp) {
							gc_gen++;
							gen_skipped_raises = 0;   /* a real GC point starts a new generation */
							bb_last_gcp [bbn] = ord;
						} else if (wj_ins_is_raise (insl)) {
							gen_skipped_raises++;     /* exempted raise: the accidental pinning is gone */
							method_skipped_raises++;
						}
						/* def AFTER the bump: a call result belongs to the post-call generation */
						d = dreg_is_base ? -1 : insl->dreg;
						if (d >= 0 && d < nvreg && isref [d]) {
							last_def_ord [d] = ord;
							if (ev_bb [d] == -1) { ev_bb [d] = bbn; sl_def_first [d] = 1; def_gen [d] = gc_gen; }
							else if (ev_bb [d] != bbn) { sl_multi [d] = 1; needs_slot [d] = TRUE; }
							else def_gen [d] = gc_gen;
						}
						ord++;
					}
				}
				nins = ord;
				/* With a terminal pinned virtual call and no way to loop back, every reference whose
				 * textual final use is at/before this call is dead in the caller while the callee runs
				 * or owns a root in the callee/miss frame. Earlier effective points are allowed only
				 * when they are conditional polls; those materialize and release the caller frame
				 * entirely inside their taken arms. */
				/* MEASURED: this handoff is the RAISE_NOGC corruption path. Its third arm in sl_elide
				 * below drops a reference's frame slot WITHOUT consulting needs_slot at all, so nothing
				 * in the liveness walk can restrain it -- and its enabling condition,
				 * effective_gcp_count == 1, is exactly what the raise exemption manufactures (a method
				 * with one real call and any number of null checks now looks single-GC-point). The
				 * references it drops are then unpinned for the whole method, and the claim that they
				 * "own a root in the callee/miss frame" is the same promise the PIC miss path breaks:
				 * every failure lands in mini_llvmonly_init_vtable_slot -> resolve_vcall on a receiver
				 * already reclaimed. Disabling wj_ins_is_pinned_vcall_forward outright (SHARED_MISS=0)
				 * is clean 0/4 against a 4/4 control; gating only the `_fwd` skip in WJ_SL_USE changed
				 * nothing (3/4 vs 2/4), which is what pins it to THIS consumer. So: no handoff in a
				 * method where the exemption hid a raise. */
				if (cfg->header->num_clauses == 0 && last_gcp_ins && method_skipped_raises == 0 &&
				    wj_ins_is_pinned_vcall_forward (last_gcp_ins) && !sl_has_backedge) {
					gboolean all_pre_refs_die = TRUE;
					terminal_vcall_ins = last_gcp_ins;
					terminal_vcall_ord = last_gcp_ord;
					terminal_vcall_poll_prefix = prior_gcps_are_polls;
					/* A reference defined by the terminal call itself is not live while that call
					 * executes. Every reference defined earlier must have its final use at/before
					 * the call before the whole frame can be handed off. */
					for (i = 0; i < nvreg; ++i)
						if (li [i] >= 0 && isref [i] && last_def_ord [i] < terminal_vcall_ord &&
						    last_use_ord [i] > terminal_vcall_ord) {
							all_pre_refs_die = FALSE;
							break;
						}
					terminal_vcall_handoff = all_pre_refs_die;
				}
				sl_elide = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				/* Per-ARM elision attribution (MONO_WASM_JIT_ELIDEDIAG=1).
				 *
				 * The three arms below mean different things and have different soundness arguments, but
				 * nelide lumps them together, so a corruption reproducer cannot tell which one dropped the
				 * slot. arm3 (terminal-vcall handoff) is the one already fixed via method_skipped_raises;
				 * arm1 (method_nogc) overrides the liveness walk wholesale, including cross-bb and
				 * loop-carried refs; arm2 is the walk's own verdict. Counting them separately is what
				 * distinguishes "the walk is wrong" from "an arm bypassed the walk" -- the same role
				 * span_gcp=0 played for the handoff arm. Attribution follows the || short-circuit order. */
				int nref_total = 0, nel_nogc = 0, nel_needs = 0, nel_handoff = 0;
				for (i = 0; i < nvreg; ++i)
					if (li [i] >= 0 && isref [i])
						nref_total++;
				for (i = 0; i < nvreg; ++i)
					if (li [i] >= 0 && isref [i] &&
					    (method_nogc || !needs_slot [i] ||
					     (effective_gcp_count == 1 && terminal_vcall_handoff &&
					      last_use_ord [i] <= terminal_vcall_ord))) {
						if (method_nogc) nel_nogc++;
						else if (!needs_slot [i]) nel_needs++;
						else nel_handoff++;
						/* A reference used at an admitted direct GC-capable call only may hand its
						 * root to the callee when that call is its final use. There is no safepoint
						 * between the caller's local.get and the callee's eager/lazy root setup.
						 * Any later use is in a new gc_gen and set needs_slot above. For a whole
						 * transitively no-GC method, even cross-bb/loop-carried refs are invisible
						 * to the collector, so the method effect safely overrides sl_multi too. */
						sl_elide [i] = 1;
						nelide++;
					}
				if (nelide > 0 && mono_wasm_jit_stats)
					mono_wasm_jit_add (WJC_SLOTS_ELIDED, nelide);
				/* Ground truth for "which arm dropped the slot", printed per method so an inline-on vs
				 * inline-off pair can be diffed directly. clauses/gcp/skipped are the enabling conditions:
				 * method_nogc requires num_clauses==0, and arm3 additionally requires gcp==1. */
				if (G_UNLIKELY (mono_wasm_jit_elidediag)) {
					/* INDEPENDENT CROSS-CHECK of the generation bookkeeping, by ORDINAL rather than by
					 * gc_gen. The arm-2 soundness claim is "an elided ref's def->use range crosses no GC
					 * point", but the walk enforces that through def_gen/gc_gen, and gc_gen RESETS per
					 * basic block while def_gen[] persists. Re-deriving the same property from raw
					 * instruction ordinals cannot share a bug with the generation counters, so a nonzero
					 * span_bad here is proof the walk elided a ref that a collector can observe -- the
					 * same role span_gcp=0 played when it exonerated the walk for the handoff arm.
					 * Second walk mirrors the first's bb/ins iteration exactly, so ordinals line up. */
					int span_bad = 0, span_max = 0, ngcp = 0, ncap = 0, o2 = 0, k;
					int *gords;
					MonoBasicBlock *b2;
					MonoInst *i2;
					for (b2 = cfg->bb_entry; b2; b2 = b2->next_bb)
						MONO_BB_FOR_EACH_INS (b2, i2)
							ncap++;
					gords = (int *) mono_mempool_alloc0 (cfg->mempool, sizeof (int) * (ncap + 1));
					for (b2 = cfg->bb_entry; b2; b2 = b2->next_bb)
						MONO_BB_FOR_EACH_INS (b2, i2) {
							if (wj_ins_is_published_gcpoint (cfg, i2))
								gords [ngcp++] = o2;
							o2++;
						}
					for (i = 0; i < nvreg; ++i)
						if (li [i] >= 0 && isref [i] && sl_elide [i] && !method_nogc && !needs_slot [i] &&
						    last_def_ord [i] >= 0 && last_use_ord [i] > last_def_ord [i]) {
							int c = 0;
							for (k = 0; k < ngcp; ++k)
								if (gords [k] > last_def_ord [i] && gords [k] < last_use_ord [i])
									c++;
							if (c > 0) {
								span_bad++;
								if (c > span_max)
									span_max = c;
							}
						}
					char *_en = mono_method_get_full_name (cfg->method);
					printf ("WASM_JIT_ELIDE %s nogc=%d clauses=%d gcp=%d skipped=%d handoff=%d refs=%d elided=%d arm1_nogc=%d arm2_needs=%d arm3_handoff=%d span_bad=%d span_max=%d\n",
					        _en, method_nogc ? 1 : 0, (int) cfg->header->num_clauses, effective_gcp_count,
					        method_skipped_raises, terminal_vcall_handoff ? 1 : 0,
					        nref_total, nelide, nel_nogc, nel_needs, nel_handoff, span_bad, span_max);
					g_free (_en);
				}
				/* Hand the elision set to LCSE (function-scope; isref/sl_elide are local to this block).
				 * An elided ref is only safe because its IR def->use range crosses no GC point, and an
				 * LCSE reuse read would extend that range. */
				lcse_nopin = sl_elide;
				/* DEAD-SLOT ZEROING (MONO_WASM_JIT_SLOTZERO, Phase 3a): a slotted single-bb vreg is
				 * provably dead after its last use — zero its slot there so the dead object stops
				 * pinning (a long-lived/JSPI-suspended frame otherwise pins it until frame pop).
				 * Emission happens at the ordinal AFTER the last use (a last use as a call arg must
				 * stay pinned across that whole call), which is always still inside the bb because a
				 * kill is only worth emitting when a GC point FOLLOWS the last use in the same bb
				 * (bb_last_gcp > last_use_ord) — without one, the slot is never observed again anyway.
				 * Candidates must be def-first (a loop-carried use-before-def vreg is live around the
				 * back edge in its wasm local, and under REF_WT only the slot copy pins it — zeroing
				 * would dangle the local) and single-bb (multi-bb death needs real liveness — Phase 3b).
				 * Requires REF_WT: reads come from the local, so the zero store is position-safe. */
				{
					/* MONO_WASM_JIT_SLOTZERO and REF_WT both ship 1. Their own comments said "Default OFF
					 * until soak" / "flip to 1 after the test matrix passes" -- the soak evidently happened
					 * and the default never moved, which left the runtime's documentation of its own safety
					 * posture wrong about the product. Baked in. */
					if (nins > 0) {
						int nzero = 0;
						sl_kill_next = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * nvreg);
						sl_kill_head = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (nins + 1));
						for (i = 0; i < nins + 1; ++i)
							sl_kill_head [i] = -1;
						for (i = 0; i < nvreg; ++i) {
							int ko;
							sl_kill_next [i] = -1;
							if (!(li [i] >= 0 && isref [i] && needs_slot [i] && !sl_elide [i]))
								continue;   /* only slotted vregs have anything to zero */
							if (addrslot [i] == -2)
								continue;   /* R321: the slot is the HOME, and its address may be live (OP_LDADDR's var is inst_p0, invisible to last_use_ord) */
							if (sl_multi [i] || !sl_def_first [i] || last_use_ord [i] < 0)
								continue;
							if (ev_bb [i] < 0 || ev_bb [i] >= nbbs || bb_last_gcp [ev_bb [i]] <= last_use_ord [i])
								continue;   /* no GC point after the last use -> never observed again */
							ko = last_use_ord [i] + 1;
							sl_kill_next [i] = sl_kill_head [ko];
							sl_kill_head [ko] = i;
							nzero++;
						}
						if (nzero == 0)
							sl_kill_head = NULL;   /* nothing to do: skip the per-ins check in the emit loop */
						else if (mono_wasm_jit_stats)
							mono_wasm_jit_add (WJC_SLOT_ZERO_STORES, nzero);
					}
				}
			}
		}
		for (i = 0; i < nvreg; ++i)
			if (li [i] >= 0 && isref [i] && !(sl_elide && sl_elide [i]))
				refslot [i] = nrefslots++;
		/* ref-etype scalar-vtype locals (addrslot==-2 from the ldaddr pre-pass): each gets a GC-scanned
		 * ref-shadow slot so its single ref field is a tracked/pinning root (VTYPE_SCALAR_REF). */
		for (i = 0; i < nvreg; ++i)
			if (addrslot [i] == -2 && refslot [i] < 0) {   /* R321: a scalar ref local may have one already */
				refslot [i] = nrefslots++;
			}
		if (nrefslots > 0)
			lc.refslot = refslot;
		/* Write-through (MONO_WASM_JIT_REF_WT): mark the slotted ref vregs whose wasm local becomes
		 * the value home, with the slot demoted to a def-mirrored pin copy (wasm_ld/wasm_st switch on
		 * this). Excluded: addrslot==-2 sentinels — their slot ADDRESS escapes to callees via OP_LDADDR
		 * (VTYPE_SCALAR_REF), so a callee write through that pointer must stay visible to reads, i.e.
		 * the slot must remain the home. All other slot accesses funnel through wasm_ld/wasm_st (plus
		 * the prologue arg copy, which already leaves the param local as the arg's home). */
		{
			if (nrefslots > 0) {
				guint8 *ref_wt = (guint8 *) mono_mempool_alloc0 (cfg->mempool, nvreg);
				int nwt = 0;
				for (i = 0; i < nvreg; ++i)
					if (refslot [i] >= 0 && li [i] >= 0 && addrslot [i] != -2) {
						ref_wt [i] = 1;
						nwt++;
					}
				if (nwt > 0)
					lc.ref_wt = ref_wt;
				if (mono_wasm_jit_stats)
					mono_wasm_jit_add (WJC_REF_WT_VREGS, nwt);
			}
		}
		/* C-stack frame size: ref slots first (4-byte), then the 8-aligned addr slots; whole frame
		 * 16-aligned per the emscripten SP ABI. */
		refbytes_al = (nrefslots * 4 + 7) & ~7;
#ifdef HOST_BROWSER
		/* R353: in record mode an EH method's WjEhRec sits after the address slots, so every EH method now has a
		 * frame. The prologue writes every field it uses and the zeroing below stops short of it. */
		if (ehrec_on) {
			ehrec_off = (refbytes_al + naddrbytes + 7) & ~7;
			framebytes = (ehrec_off + mono_wasm_jit_ehrec_layout ()->size + 15) & ~15;
		} else
#endif
		framebytes = (refbytes_al + naddrbytes + 15) & ~15;
		/* Lazy materialization is restricted to the representation where every root has a wasm-local
		 * value home. Address frames and EH need a valid frame from entry; slot-homed ref-vtype
		 * sentinels likewise cannot be reconstructed. Debug store guards intentionally keep the old
		 * eager path. */
		/* The GC-point count is a HEURISTIC here, not a correctness requirement, and it was pinned at 1.
		 * ENSURE_REF_FRAME is idempotent -- it tests whether the frame has already been materialised -- and
		 * the main emit loop already emits it before EVERY effective GC point (see the
		 * wj_ins_is_effective_gcpoint site), so a method with N of them is handled correctly today. The
		 * only question a larger limit raises is whether it PAYS: lazy costs a 3-op check at each GC point
		 * and saves the whole ~15-op frame prologue on any path that reaches none.
		 *
		 * Pinning it at 1 confined the mechanism to almost nothing. Measured on a whole-tier dump weighted
		 * by an in-game profile: 94.3% of our tier's execution time is in EAGER-frame methods, 2.4% lazy,
		 * 3.3% frameless -- while the prologue is 21.4% of that tier's time. MONO_WASM_JIT_LAZY_GCP makes
		 * the limit sweepable; <= 0 means no limit.
		 *
		 * SWEPT, AND 1 IS RIGHT. Removing the limit does exactly what it should to the emitted code -- eager
		 * frames fall from 94.3% of our tier's execution to 9.1%, lazy rises 2.4% -> 87.5% -- and is a large
		 * REGRESSION anyway. Interleaved A/B, plateau instrument, 240s cooldown per arm, 2 rounds:
		 *
		 *     p50        45.8 -> 54.6 ms   +19.3%   (spread +/-1.7%, far outside noise)
		 *     fps        21.6 -> 16.7      -22.7%
		 *     msFrame    46.3 -> 59.8 ms   +29.3%
		 *     world.gen  42.7 -> 134.0 s  +214.1%
		 *     boot.classload             +9.1%
		 *
		 * The price is code: median module 800 -> 1064 B (+33%), p90 2925 -> 5652 (+93%), because ENSURE
		 * lands at every GC point. Note WHERE it hurt most -- world generation, the compile-heavy phase,
		 * tripled. Compile cost alone cannot explain that (29.4 us fixed + 15.4 ns/wire-byte measured
		 * separately, so +264 B across 24k modules is ~0.1 s, not 91 s), so the bulk is more likely TurboFan
		 * coping badly with ~20 extra ops and a branch at every GC point, on a target already shown to be
		 * register-starved. Not chased further.
		 *
		 * An intermediate value was NOT swept: at 2 or 3 only methods with exactly that many GC points become
		 * lazy -- a small population -- and the gradient from 1 to unlimited is steeply negative. */
		{
			extern int mono_wasm_jit_lazy_cold;
			gboolean gcp_gate = (mono_wasm_jit_lazy_gcp <= 0 || effective_gcp_count <= mono_wasm_jit_lazy_gcp) ||
				(terminal_vcall_handoff && terminal_vcall_poll_prefix);
			/* Cold-only: every GC point is a raise (its taken arm ENSUREs: WJ_THROW_BR's shared blocks, OP_THROW,
			 * LDIV_RAISE) or a conditional poll (ENSURE ... RELEASE inside its taken arm). The PRODUCT runs
			 * RAISE_NOGC=1 (index.ts), under which a one-raise method is already lazy with no hot ENSURE; this
			 * extends the same shape to methods with several raises and polls. Level 2 is the documented
			 * unsound positive control and is never combined with it. */
			gboolean cold_gate = mono_wasm_jit_lazy_cold && gcp_walked && hot_gcp_count == 0 &&
				mono_wasm_jit_raise_nogc != 2 && cfg->header->num_clauses == 0;
			if (framebytes > 0 && (gcp_gate || cold_gate) &&
			    naddrbytes == 0 && !eh_on && lc.ref_wt &&
			    !lc.storeguard && !lc.objguard) {
				gboolean all_wt = TRUE;
				for (i = 0; i < nvreg; ++i)
					if (refslot [i] >= 0 && (!lc.ref_wt [i] || li [i] < 0)) {
						all_wt = FALSE;
						break;
					}
				if (all_wt) {
					lazy_ref_frame = TRUE;
					lc.lazy_frame = 1;
					if (cold_gate) {
						lazy_cold_frame = TRUE;
						lc.lazy_frame = 2;
						if (mono_wasm_jit_stats) {
							mono_wasm_jit_count (WJC_LAZY_COLD);
							if (!gcp_gate)
								mono_wasm_jit_count (WJC_LAZY_COLD_NEW);
							if (doomed_gcp_count > 0)
								mono_wasm_jit_count (WJC_LAZY_COLD_DOOMED);
						}
					}
				}
			}
			/* LAZY_COLD=2's reach, split: methods whose doomed discount mattered at all, and of those the ones
			 * with nothing hot left -- so a 0 in WJC_LAZY_COLD_DOOMED says WHICH gate term refused. */
			if (mono_wasm_jit_stats && doomed_gcp_count > 0) {
				mono_wasm_jit_count (WJC_LAZY_DOOMED_SEEN);
				if (hot_gcp_count == 0)
					mono_wasm_jit_count (WJC_LAZY_DOOMED_HOT0);
			}
#ifdef HOST_BROWSER
			/* MONO_WASM_JIT_WATCH=<name substring>: every term of the lazy-frame gate, and per bb the doomed
			 * flag, its out-edges, its last opcode and its published GC points. Field reads only. */
			{
				extern const char *mono_wasm_jit_watch;
				if (G_UNLIKELY (mono_wasm_jit_watch) && cfg->method->name &&
				    strstr (cfg->method->name, mono_wasm_jit_watch)) {
					MonoBasicBlock *wb; MonoInst *wi; int wbn = 0;
					gboolean w_all_wt = lc.ref_wt != NULL;
					for (i = 0; w_all_wt && i < nvreg; ++i)
						if (refslot [i] >= 0 && (!lc.ref_wt [i] || li [i] < 0))
							w_all_wt = FALSE;
					printf ("WASM_JIT_LAZY_GATE %s:%s framebytes=%d naddrbytes=%d eh_on=%d ref_wt=%d all_wt=%d "
						"guards=%d/%d walked=%d gcp=%d hot=%d doomed=%d gcp_gate=%d cold_gate=%d -> lazy=%d cold=%d\n",
						cfg->method->klass ? m_class_get_name (cfg->method->klass) : "?", cfg->method->name,
						framebytes, naddrbytes, (int) eh_on, lc.ref_wt != NULL, (int) w_all_wt,
						(int) lc.storeguard, (int) lc.objguard, (int) gcp_walked, effective_gcp_count,
						hot_gcp_count, doomed_gcp_count, (int) gcp_gate, (int) cold_gate,
						(int) lazy_ref_frame, (int) lazy_cold_frame);
					for (wb = cfg->bb_entry; wb; wb = wb->next_bb, ++wbn) {
						printf ("WASM_JIT_LAZY_GATE   bb%d idx=%d doomed=%d out=%d last=%s exit=%d gcps:", wb->block_num,
							wbn, lazy_doomed ? (int) lazy_doomed [wbn] : -1, wb->out_count,
							wb->last_ins ? wj_opname (wb->last_ins->opcode) : "-", wb == cfg->bb_exit);
						MONO_BB_FOR_EACH_INS (wb, wi)
							if (wj_ins_is_published_gcpoint (cfg, wi))
								printf (" %s%s", wj_opname (wi->opcode),
									wi->opcode == OP_GC_SAFE_POINT ? "(poll)" : wj_ins_is_raise (wi) ? "(raise)" : "");
						printf ("\n");
					}
				}
			}
#endif
		}
		if (mono_wasm_jit_stats) {
			mono_wasm_jit_add (WJC_REF_SLOTS, nrefslots);
			mono_wasm_jit_add (WJC_FRAME_BYTES, framebytes);
		}

		/* MISSED-REF FINDER (MONO_WASM_JIT_MISSEDREF=1): the corruptor class is a ref left in a plain
		 * wasm local (isref=FALSE) that goes stale after a GC. The dangerous uses are dereferences: a MEMBASE
		 * load/store base, or a virtual-call receiver. Log each such NONREF base + the opcode that DEFINED it,
		 * so the wrong wj_opcode_is_nonref case is nameable. OP_LDADDR bases are addr-frame (non-heap) -> skip.
		 * Bounded across the whole run. */
		{ extern int mono_wasm_jit_missedref;
		  if (G_UNLIKELY (mono_wasm_jit_missedref)) {
			MonoInst **defins = (MonoInst **) mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst *) * nvreg);
			MonoBasicBlock *bbr; MonoInst *insr;
			gsize memsz = 0;
#ifdef HOST_BROWSER
			memsz = wj_memsz ();
#endif
			for (bbr = cfg->bb_entry; bbr; bbr = bbr->next_bb)
				MONO_BB_FOR_EACH_INS (bbr, insr)
					if (insr->dreg >= 0 && insr->dreg < nvreg) defins [insr->dreg] = insr;
			for (bbr = cfg->bb_entry; bbr; bbr = bbr->next_bb) {
				MONO_BB_FOR_EACH_INS (bbr, insr) {
					int base = -1; const char *knd = NULL;
					MonoInst *d; int dop;
					switch (insr->opcode) {
					case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
					case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
					case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
					case OP_ATOMIC_LOAD_I1: case OP_ATOMIC_LOAD_U1: case OP_ATOMIC_LOAD_I2: case OP_ATOMIC_LOAD_U2:
					case OP_ATOMIC_LOAD_I4: case OP_ATOMIC_LOAD_U4: case OP_ATOMIC_LOAD_R4: case OP_ATOMIC_LOAD_R8:
					case OP_ATOMIC_ADD_I4: case OP_ATOMIC_EXCHANGE_U1: case OP_ATOMIC_EXCHANGE_U2: case OP_ATOMIC_EXCHANGE_I4:
					case OP_ATOMIC_CAS_U1: case OP_ATOMIC_CAS_U2: case OP_ATOMIC_CAS_I4:   /* R360 */
					case OP_ATOMIC_LOAD_I8: case OP_ATOMIC_LOAD_U8: case OP_ATOMIC_ADD_I8: case OP_ATOMIC_EXCHANGE_I8: case OP_ATOMIC_CAS_I8:   /* J1c */
						base = insr->sreg1; knd = "load"; break;
					case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
					case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
					case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
					case OP_ATOMIC_STORE_I1: case OP_ATOMIC_STORE_U1:   /* R323b */
					case OP_ATOMIC_STORE_I2: case OP_ATOMIC_STORE_U2: case OP_ATOMIC_STORE_I4: case OP_ATOMIC_STORE_U4:
					case OP_ATOMIC_STORE_R4: case OP_ATOMIC_STORE_R8:   /* R360 */
					case OP_ATOMIC_STORE_I8: case OP_ATOMIC_STORE_U8:   /* J1c, MONO_WASM_JIT_ATOMIC_I8 */
						base = insr->dreg; knd = "store"; break;
					case OP_CALL_MEMBASE: case OP_VCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_VOIDCALL_MEMBASE:
					case OP_VCALL2_MEMBASE:
						if (mono_wasm_jit_vcall_vret) { base = insr->sreg1; knd = "callrecv"; }
						break;
					default: break;
					}
					if (base < 0 || base >= nvreg || vt [base] != WASM_I32 || isref [base])
						continue;
					d = defins [base];
					dop = d ? d->opcode : -1;
					if (dop == OP_LDADDR)
						continue;   /* addr-frame pointer (non-heap) */
					if (dop == OP_ICONST) {
						/* A CONSTANT base is safe ONLY if it's a fixed address (vtable/class/static blob — never
						 * GC-moved). If the constant is a MOVABLE heap object baked directly (should have gone
						 * through the PCONST precise root), the GC moves it -> the baked constant goes stale ->
						 * wild deref = the corruptor. Flag it by probing whether the constant looks like a live
						 * object (val -> vtable -> klass are all plausible in-memory pointers). */
#ifdef HOST_BROWSER
						/* wj_probe_ok, NOT open-coded bounds math: `v + 8 <= memsz` wraps for v near 2^32
						 * (a probed word holding e.g. int -8) and the probe's own deref becomes an OOB trap. */
						gsize v = (gsize) d->inst_c0;
						if (wj_probe_ok (v, memsz)) {
							gsize vtab = *(gsize *) v;
							if (wj_probe_ok (vtab, memsz)) {
								gsize kl = *(gsize *) vtab;
								if (wj_probe_ok (kl, memsz)) {
									static int _mo = 0;
									if (_mo++ < 200)
										printf ("WASM_JIT_MISSED_REF_ICONST_OBJ %s : %s base=%d val=0x%x vtable=0x%x (MOVABLE object baked as iconst -> stale after GC)\n",
											mname, knd, base, (unsigned) v, (unsigned) vtab);
								}
							}
						}
#endif
						continue;   /* fixed-address iconst -> not a missed ref */
					}
					{ static int _mn = 0;
					  if (_mn++ < 400)
						printf ("WASM_JIT_MISSED_REF_BASE %s : %s base=%d def=%s\n", mname, knd, base, dop >= 0 ? wj_opname (dop) : "(arg/undef)"); }
				}
			}
		  } }

		/* REF-SAFETY IR DUMP (MONO_WASM_JIT_REFDIAG=<name,...>): for a watch-listed method, dump the full
		 * post-classification IR — each instruction's opcode name + dreg/sreg with their isref flag
		 * (R=ref shadow-stack slot, r=isref but slot ELIDED by SLOTLIVE, -=plain wasm local, .=n/a),
		 * and for calls the captured call_info arg
		 * vregs + flags + virt/ret_ref. A managed ref left in a plain local (-) that is live across a GC
		 * point (a call/virtual resolve) is the dangling-pointer corruptor; this shows the exact op that
		 * produced it (and whether a call receiver/arg is being spilled from an unclassified local). */
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name))) {
			MonoBasicBlock *bb2; MonoInst *ins2;
#define WJ_RF(v) (((v) >= 0 && (v) < nvreg) ? (isref [v] ? ((sl_elide && sl_elide [v]) ? 'r' : 'R') : '-') : '.')
			{ printf ("WASM_JIT_IR === %s nvreg=%d nrefslots=%d nargs=%d ===\n", cfg->method->name, nvreg, nrefslots, nargs); }
			for (bb2 = cfg->bb_entry; bb2; bb2 = bb2->next_bb)
				MONO_BB_FOR_EACH_INS (bb2, ins2) {
					char b [256]; int n;
					n = snprintf (b, sizeof b, "WASM_JIT_IR %-22s d=%d%c s1=%d%c s2=%d%c off=%d",
						wj_opname (ins2->opcode), ins2->dreg, WJ_RF (ins2->dreg), ins2->sreg1, WJ_RF (ins2->sreg1),
						ins2->sreg2, WJ_RF (ins2->sreg2), (int) ins2->inst_offset);
					switch (ins2->opcode) {
					case OP_CALL: case OP_CALL_REG: case OP_CALL_MEMBASE:
					case OP_VCALL: case OP_VCALL_REG: case OP_VCALL_MEMBASE:
					case OP_VCALL2: case OP_VCALL2_REG: case OP_VCALL2_MEMBASE:
					case OP_FCALL: case OP_FCALL_REG: case OP_FCALL_MEMBASE:
					case OP_LCALL: case OP_LCALL_REG: case OP_LCALL_MEMBASE:
					case OP_VOIDCALL: case OP_VOIDCALL_REG: case OP_VOIDCALL_MEMBASE: {
						MonoCallInst *c = (MonoCallInst *) ins2;
						if (c->signature && c->call_info) {
							int na = ((WjCallArgs *) c->call_info)->nargs;
							int k;
							n += snprintf (b + n, sizeof b - n, " virt=%d ret_ref=%d args[",
								c->method ? !!(c->method->flags & METHOD_ATTRIBUTE_VIRTUAL) : -1,
								mini_type_is_reference (c->signature->ret));
							for (k = 0; k < na && n < (int) sizeof b - 14; ++k) {
								int av = wj_arg_vreg (c, k);
								n += snprintf (b + n, sizeof b - n, "%s%d%c", k ? "," : "", av, WJ_RF (av));
							}
							n += snprintf (b + n, sizeof b - n, "]");
						} else if (c->signature) {
							n += snprintf (b + n, sizeof b - n, " (no call_info) ret_ref=%d", mini_type_is_reference (c->signature->ret));
						}
						break;
					}
					default: break;
					}
					printf ("%s\n", b);
				}
#undef WJ_RF
		}
	}
#endif

#define BIN(WOP)     do { if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "bin sreg"; goto done; } wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "bin dreg"; goto done; } } while (0)
#define BINI32(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i32_const (&body, (gint32) ins->inst_imm); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
#define BINI64(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i64_const (&body, (gint64) ins->inst_imm); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
/* Long NON-SHIFT IMM ops: the 64-bit immediate lives in ins->inst_l (GET_LONG_IMM on wasm32), NOT
 * inst_imm (which on a 32-bit host can't hold it -> AND/OR masks came out 0). Shifts keep BINI64
 * (their small count is in inst_imm, per mini-llvm's shift_i8 path). */
#define BINI64L(WOP) do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "imm sreg"; goto done; } wasm_i64_const (&body, (gint64) ins->inst_l); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "imm dreg"; goto done; } } while (0)
/* Raise a corlib exception (by id; see OP_COND_EXC's exc_id map) from inside an `if`, then leave the method
 * — used by the inline OP_LDIV/OP_LREM div-by-zero/overflow guards (wasm32's decompose omits them for the
 * long opcodes). cppeh: the raise C++-throws and never returns (unreachable). `ldiv_rti` (the (i32)->void
 * functype index) must be in scope. Both uses sit inside an `if` arm, so the ENSURE_REF_FRAME here costs
 * the hot path nothing; see the shared throw blocks for why a raise needs a materialized frame. */
#ifdef HOST_BROWSER
#define LDIV_RAISE_FPTR ((gpointer) (intptr_t) mono_wasm_jit_raise_corlib)
#else
#define LDIV_RAISE_FPTR ((gpointer) (intptr_t) 0x7ff8)   /* offline dump: placeholder table index */
#endif
#ifdef HOST_BROWSER
/* Direct import rather than `i32.const <addr>; call_indirect` -- same reason as EMIT_REF_LEAVE: a constant
 * index is not folded into a direct call by V8. raise_corlib is a C function in dotnet.native.wasm. */
#define LDIV_RAISE(EXC_ID) do { \
		RAISE_ENSURE_REF_FRAME ();   /* raise_corlib allocates: roots must be scannable. Taken arm only. */ \
		extern void mono_wasm_jit_raise_corlib (int exc_id); \
		wasm_i32_const (&body, (EXC_ID)); \
		wj_emit_helper_call (&body, LDIV_RAISE_FPTR, (guint32) ldiv_rti); \
		wasm_op (&body, WASM_OP_UNREACHABLE);   /* mono_wasm_jit_raise_corlib C++-throws and never returns */ \
	} while (0)
#else
#define LDIV_RAISE(EXC_ID) do { \
		RAISE_ENSURE_REF_FRAME (); \
		wasm_i32_const (&body, (EXC_ID)); \
		wj_emit_helper_call (&body, LDIV_RAISE_FPTR, (guint32) ldiv_rti); \
		wasm_op (&body, WASM_OP_UNREACHABLE); \
	} while (0)
#endif
/* unary: dreg = WOP(sreg1) — conversions/sign-extends */
#define UN(WOP)      do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "un sreg"; goto done; } wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "un dreg"; goto done; } } while (0)
/* dreg = sreg1 & M (i32) — unsigned narrowing conversions (conv.u1/u2) */
#define MASK(M)      do { if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "mask sreg"; goto done; } wasm_i32_const (&body, (gint32)(M)); wasm_op (&body, WASM_OP_I32_AND); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "mask dreg"; goto done; } } while (0)
/* i64 shift: value (sreg1) is i64 but the shift count (sreg2) is an i32 vreg in mono IR, while wasm
 * i64.shl/shr_s/shr_u require BOTH operands i64 -> zero-extend an i32 count to i64 first. (Count is
 * 0..63, so the extension sign is irrelevant; i64.shr_u masks mod 64 anyway.) */
#define LSHIFT(WOP)  do { if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "lsh sreg"; goto done; } if (ins->sreg2 >= 0 && ins->sreg2 < nvreg && vt [ins->sreg2] == WASM_I32) wasm_op (&body, WASM_OP_I64_EXTEND_I32_U); wasm_op (&body, (WOP)); if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lsh dreg"; goto done; } } while (0)
/* push i32 (0/1) = (A <cmp> B) for float width IS_F32, comparison KIND (0=EQ..5=GE); NEG wraps in i32.eqz
 * (used to synthesize the unordered ordered-relation variants, e.g. lt_un = !(a>=b)). */
#define FCMP_PUSH(IS_F32, KIND, NEG, A, B) do { \
		if (!wasm_ld (&body, &lc, (A))) { fail = "fcmp a"; goto done; } \
		if (!wasm_ld (&body, &lc, (B))) { fail = "fcmp b"; goto done; } \
		wasm_op (&body, wj_fcmp_op ((IS_F32), (KIND))); \
		if (NEG) wasm_op (&body, WASM_OP_I32_EQZ); \
	} while (0)
/* Pop this method's C-stack frame (restore __stack_pointer to the entry SP). Emit before EVERY
 * return: a popped frame falls below the SP and stops being GC-scanned — no zeroing needed. The
 * global.set consumes only entry_sp, so it leaves any return value on the stack. No-op for
 * frame-less methods. */
/* A residual/vcall/delegate callee reported threw==1: CONTINUE THE NATIVE UNWIND, never return.
 *
 * The boundary (mono_wasm_jit_invoke_caught) has already caught the C++ unwind and, for a handler in a
 * JITted island above us, deliberately keeps the exception armed in the interp resume state. Returning
 * normally here loses the exceptional edge: this frame's own landing pad is skipped, its JIT caller runs
 * on unaware, and the exception surfaces later at an unrelated frame (see FINDINGS Finding 15).
 *
 * Deliberately NO EMIT_REF_LEAVE() before the call. If this method has a matching handler, its landing
 * pad needs the frame and island still live; the pad restores __stack_pointer to its own refbase itself
 * (REF-SP-1). If it has no match, its pad does leave_island + rethrow, a non-EH frame is simply torn
 * through, and the next outer pad -- or the interp boundary's saved SP -- does the restore. Popping the
 * roots here would strip them before this method's own catch/finally gets control.
 *
 * No dummy return value either: `unreachable` makes the rest of the block polymorphic, so the void
 * blocktype validates without one. */
#define EMIT_RESIDUAL_THROW_CONTINUATION() do { \
		extern void mono_wasm_jit_continue_unwind (void); \
		g_assert (resid_void_ti >= 0); \
		wj_emit_helper_call_ci (&body, WJ_CONTINUE_UNWIND_ADDR, (guint32) resid_void_ti); \
		wasm_op (&body, WASM_OP_UNREACHABLE); \
	} while (0)

#ifdef HOST_BROWSER
#define WJ_CONTINUE_UNWIND_ADDR ((gpointer) (intptr_t) mono_wasm_jit_continue_unwind)
#else
#define WJ_CONTINUE_UNWIND_ADDR ((gpointer) (intptr_t) 0x7ff8)   /* offline dump: placeholder table index */
#endif

/* MONO_WASM_JIT_IC_REMAT (R314, plan L7/M5): read the per-thread PIC pointers and capacities from their IMMUTABLE
 * imports at each IC site (global.get s.v/s.n/s.d/s.m, one instance load) instead of holding four prologue values
 * live across the whole body, where V8 spills them after the prologue and reloads them after every call (R312:
 * ~60% of our stores are V8 stack traffic). The PICs are per-thread and grow only on their owning thread inside a
 * miss helper, so an at-use read is never staler than the prologue copy it replaces. */
#define EMIT_IC_VPIC_PTR() do { if (mono_wasm_jit_ic_remat) { wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 3); } \
	else wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_ptr_idx); } while (0)
#define EMIT_IC_VPIC_CAPVAL() do { if (mono_wasm_jit_ic_remat) { wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 4); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); } \
	else wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vpic_capval_idx); } while (0)
#define EMIT_IC_DPIC_PTR() do { if (mono_wasm_jit_ic_remat) { wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 5); } \
	else wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dpic_ptr_idx); } while (0)
#define EMIT_IC_DPIC_CAPVAL() do { if (mono_wasm_jit_ic_remat) { wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 6); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); } \
	else wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dpic_capval_idx); } while (0)
#ifdef HOST_BROWSER
/* R353 (MONO_WASM_JIT_EH_REC): the EH record's push and pop, inline. The record is at refbase + ehrec_off
 * (interp.c's WjEhRec, offsets from mono_wasm_jit_ehrec_layout). The LMF address is re-derived from the scratch at
 * each use -- s.b + WJ_SCRATCH_LMFADDR_SLOT holds the address of this thread's mirror -- so nothing new is live
 * across the body's calls (the INLINE_ILOFS lesson, see wasm-encoder.c's s.i note). */
#define EHREC_I32_STORE(off) do { wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (ehrec_off + (off))); } while (0)
#define EHREC_I32_LOAD(off) do { wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (ehrec_off + (off))); } while (0)
#define EHREC_ADDR(off) do { wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_i32_const (&body, ehrec_off + (off)); wasm_op (&body, WASM_OP_I32_ADD); } while (0)
#define EHREC_LOAD_LMF_ADDR() do { wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, WJ_SCRATCH_LMFADDR_SLOT); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) ehlmf_idx); } while (0)
#define EMIT_EHREC_PUSH() do { const WjEhRecLayout *_L = mono_wasm_jit_ehrec_layout (); \
	EHREC_LOAD_LMF_ADDR (); \
	/* previous_lmf = *lmf_addr | 2 (the ext tag, as mono_push_lmf writes it) */ \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) ehlmf_idx); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	wasm_i32_const (&body, 2); wasm_op (&body, WASM_OP_I32_OR); \
	EHREC_I32_STORE (_L->previous_lmf); \
	/* the plain-LMF fields are unused for an ext; zero them, as interp_push_lmf does */ \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, 0); EHREC_I32_STORE (_L->lmf_addr); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, 0); EHREC_I32_STORE (_L->lmf_method); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, _L->kind_il_state); EHREC_I32_STORE (_L->kind); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_ADDR (_L->il); EHREC_I32_STORE (_L->il_state); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_ADDR (_L->il); \
	wasm_i32_const (&body, (gint32) _L->magic_key); wasm_op (&body, WASM_OP_I32_XOR); EHREC_I32_STORE (_L->magic); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, (gint32) (intptr_t) cfg->method); EHREC_I32_STORE (_L->il_method); \
	/* the InterpMethod this body was compiled against: an exception in this frame resolves clauses through it (H1) */ \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, (gint32) (intptr_t) cfg->wasm_jit_caller_imethod); EHREC_I32_STORE (_L->body); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); wasm_i32_const (&body, -1); EHREC_I32_STORE (_L->il_offset); \
	/* prev = cur_island_il_state; finally_sp = the finally-save depth */ \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 8); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	EHREC_I32_STORE (_L->prev); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, WJ_SCRATCH_FINSP_SLOT); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	EHREC_I32_STORE (_L->finally_sp); \
	/* publish: cur_island_il_state = &rec.il, then the chain head = &rec.ext, last */ \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 8); EHREC_ADDR (_L->il); \
	wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) ehlmf_idx); EHREC_ADDR (0); \
	wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0); \
	} while (0)
/* The head is normally this record: unlink it inline. Otherwise mono_wasm_jit_ehrec_unlink decides (cold): a stale
 * inner LMF above it goes with it, a head that pass 1 rewound past it stays (see leave_island). */
#define EMIT_EHREC_POP() do { const WjEhRecLayout *_L = mono_wasm_jit_ehrec_layout (); \
	extern void mono_wasm_jit_finally_trim_to (int baseline); \
	extern void mono_wasm_jit_ehrec_unlink (gpointer rec); \
	EHREC_LOAD_LMF_ADDR (); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) ehlmf_idx); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	EHREC_ADDR (0); wasm_op (&body, WASM_OP_I32_EQ); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) ehlmf_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_I32_LOAD (_L->previous_lmf); \
	wasm_i32_const (&body, -4); wasm_op (&body, WASM_OP_I32_AND); \
	wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0); \
	wasm_op (&body, WASM_OP_ELSE); \
	EHREC_ADDR (0); \
	wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_ehrec_unlink, (guint32) eh_type_idx); \
	wasm_op (&body, WASM_OP_END); \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 8); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_I32_LOAD (_L->prev); \
	wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0); \
	/* a finally body on this frame's exception path threw and escaped: free what it left (cold) */ \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, WJ_SCRATCH_FINSP_SLOT); \
	wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_I32_LOAD (_L->finally_sp); \
	wasm_op (&body, WASM_OP_I32_NE); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); EHREC_I32_LOAD (_L->finally_sp); \
	wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_finally_trim_to, (guint32) eh_type_idx); \
	wasm_op (&body, WASM_OP_END); \
	} while (0)
#define EMIT_REF_LEAVE() do { guint16 _wjo_rl = wasm_origin_push (&body, WJO_EXIT); \
	if (ehrec_on) { guint16 _wjo_ep = wasm_origin_push (&body, WJO_EH_POP); EMIT_EHREC_POP (); wasm_origin_pop (&body, _wjo_ep); } \
	if (framebytes > 0 && !lazy_cold_frame) { \
		if (lazy_ref_frame) { \
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
		} \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
		wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
		if (lazy_ref_frame) wasm_op (&body, WASM_OP_END); \
	} \
	if (eh_on && !ehrec_on) {   /* pop this EH method's il_state island (pushed by enter_island in the prologue) */ \
		extern void mono_wasm_jit_leave_island (void); \
		/* Direct import, not `i32.const <addr>; call_indirect`: V8 does not fold a constant index into a \
		 * direct call, so the indirect form pays a table bounds check, a canonical-type check, index->code \
		 * arithmetic and a validity check -- about fifteen x86 instructions where a direct `call` is one. \
		 * This macro runs before EVERY return of an EH-bearing method, so it is one of the densest constant \
		 * targets in the whole tier. Safe to import because the address is a C function in \
		 * dotnet.native.wasm: fixed for the process, never repointed. (Contrast the method f-slot in the \
		 * ordinary direct-call path, which IS repointed on rebind and therefore must stay indirect.) */ \
		wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_leave_island, (guint32) eh_endcatch_ti); \
	} wasm_origin_pop (&body, _wjo_rl); } while (0)
#else
#define EMIT_REF_LEAVE() do { } while (0)
#endif

/* Materialize a lazy pure-ref frame immediately before the first instruction that can return after a
 * GC. The current SP is still this method's entry SP: everything executed before here is inline-pure or
 * a transitively no-GC/frame-free direct callee. Copy every write-through local only after reserving and
 * zeroing the frame. The captured nonzero entry SP is also the active marker; later definitions use it
 * to mirror conditionally in wasm_st. */
#ifdef HOST_BROWSER
#define ENSURE_REF_FRAME() do { if (lazy_ref_frame) { int _rf_i; guint16 _wjo_ef = wasm_origin_push (&body, WJO_ENSURE_FRAME); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_I32_EQZ); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_i32_const (&body, framebytes); wasm_op (&body, WASM_OP_I32_SUB); \
	wasm_i32_const (&body, -16); wasm_op (&body, WASM_OP_I32_AND); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
	wasm_i32_const (&body, 0); wasm_i32_const (&body, framebytes); \
	wasm_u8 (&body, 0xFC); wasm_uleb (&body, 11); wasm_u8 (&body, 0); \
	for (_rf_i = 0; _rf_i < nvreg; ++_rf_i) if (refslot [_rf_i] >= 0) { \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx); \
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) li [_rf_i]); \
		wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [_rf_i] * 4)); \
	} \
	wasm_op (&body, WASM_OP_END); \
	wasm_origin_pop (&body, _wjo_ef); \
} } while (0)
#else
#define ENSURE_REF_FRAME() do { } while (0)
#endif

/* A conditional safepoint poll needs roots only in its taken arm. After the poll returns, the wasm
 * locals contain the same pinned object addresses and no GC can observe them again until the next
 * materialization/handoff. Pop the lazy frame and clear its active marker so a later poll can rebuild
 * it. This is what keeps the overwhelmingly-common poll-not-requested path frame-free. */
#ifdef HOST_BROWSER
#define RELEASE_LAZY_REF_FRAME() do { if (lazy_ref_frame) { \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
	wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0); \
	wasm_i32_const (&body, 0); \
	wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx); \
	wasm_op (&body, WASM_OP_END); \
} } while (0)
#else
#define RELEASE_LAZY_REF_FRAME() do { } while (0)
#endif

/* Materialize before a call that C++-throws, so exception allocation cannot run while this method's
 * roots live only in wasm locals. */
#define RAISE_ENSURE_REF_FRAME() ENSURE_REF_FRAME ()


	/* MONO_WASM_JIT_NCE: per-bb "already proven non-null" vreg bitmap (see the flag's comment).
	 * Reset at the top of every bb, so nothing crosses a control-flow edge and no dominance
	 * information is needed. */
	guint8 *nn = NULL;
	/* MONO_WASM_JIT_NCE ships 1 (per-bb "already proven non-null" vreg bitmap). Its mode 2 -- dominator
	 * propagation of never-written facts -- measured worth ~0 and is deleted; mode 1 is unconditional. */
	if (nvreg > 0)
		nn = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) nvreg);
	/* Trackable = a real vreg that lives in a wasm local. Address-taken vregs (addrslot != -1, which
	 * includes the -2 ref-vtype sentinels) are homed in the addressable-locals frame and their address
	 * escapes to callees, so a callee store could change them behind our back: never track those. */
#define NN_OK(VR)   ((nn) && (VR) >= 0 && (VR) < nvreg && !(lc.addrslot && lc.addrslot [(VR)] != -1))
#define NN_GET(VR)  (NN_OK (VR) && nn [(VR)])
#define NN_SET(VR)  do { if (NN_OK (VR)) nn [(VR)] = 1; } while (0)
#define NN_KILL(VR) do { if ((nn) && (VR) >= 0 && (VR) < nvreg) nn [(VR)] = 0; } while (0)

	/* dense bb indexing for the dispatch table */
	bbidx = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * ((int) cfg->max_block_num + 1));
	for (i = 0; i < (int) cfg->max_block_num + 1; ++i)
		bbidx [i] = -1;
	N = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		bbidx [bb->block_num] = N++;
	structured_cfg_kind = wj_structured_cfg_kind (cfg, bbidx, N,
		&structured_loop_h, &structured_loop_l);
	memset (&nest, 0, sizeof (nest));
	{ extern int mono_wasm_jit_cfgc, mono_wasm_jit_cfg_nest;   /* plan A5 (R420): census, then the nested form */
	  char cls = 0;
	  int nl = 0, dp = 0;
	  if (G_UNLIKELY (mono_wasm_jit_cfgc) && structured_cfg_kind == WJ_CFG_DISPATCH && !fail)
		cls = wj_cfg_stackify_class (cfg, bbidx, N, &nl, &dp, NULL, NULL);
	  if (mono_wasm_jit_cfg_nest && structured_cfg_kind == WJ_CFG_DISPATCH && !fail && wj_nest_plan (cfg, bbidx, N, &nest))
		structured_cfg_kind = WJ_CFG_NESTED;
	  if (cls)
		printf ("WASM_JIT_CFGC\t%d\t%c\t%d\t%d\t%d\t%s\t%d\t%d\n", cfg->wasm_jit_tier, cls, nl, dp, N, mname ? mname : "?",
			structured_cfg_kind == WJ_CFG_NESTED ? 1 : 0, cfg_sorted ? 1 : 0);
	}

	{ extern int mono_wasm_jit_s4c;   /* plan2x S4 census: a dry run, before anything is lowered */
	  if (G_UNLIKELY (mono_wasm_jit_s4c) && !fail)
		wj_s4_census (cfg, mname, nvreg); }
	/* MONO_WASM_JIT_LCSE state.
	 *
	 * The table flows from a bb to a successor only when that successor has EXACTLY ONE predecessor —
	 * i.e. along the single edge of an extended basic block. That is what makes it sound with no path
	 * analysis at all: with one predecessor there is no other way in, so nothing unaccounted-for can run
	 * between the two blocks. Dominance alone would NOT be enough (idom(bb) can reach bb through
	 * intervening blocks that store), which is why this does not reuse the NCE level-2 walk.
	 *
	 * That single-predecessor shape is exactly the ternary arm this targets, so the restriction costs
	 * nothing on the measured case.
	 *
	 * Disabled under STOREGUARD/OBJGUARD: those wrap each access in a check_store call, and eliding a
	 * load would elide its guard, changing what the guard run verifies. */
	WjLcse *lcse = NULL;
	WjLcse **lcse_exit = NULL;
	int lcse_adds = 0, lcse_hits = 0, lcse_inherits = 0;
	{
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name)))
			printf ("WASM_JIT_LCSE %s: flag=%d nvreg=%d sg=%d og=%d\n",
				cfg->method->name, 1, nvreg, lc.storeguard, lc.objguard);
	}
	/* MONO_WASM_JIT_LCSE ships 1. Its knob is gone but the PASS stays, and the reason is a measurement
	 * trap worth stating: a static census of the tier dump CANNOT show LCSE inert, because the dump is
	 * POST-LCSE -- every redundancy it removed is already invisible there. The 0.09%% "EBB redundancy"
	 * reading is equally consistent with the pass working. What a census CAN bound is the part LCSE
	 * cannot reach: whole-body (cross-EBB) redundant loads, 6.33%% of loads as an upper bound, which is
	 * ~0.46%% of the client thread -- and that is why GVN is closed. */
	if (nvreg > 0 && !lc.storeguard && !lc.objguard) {
		guint8 *needed = (guint8 *) mono_mempool_alloc0 (cfg->mempool, (gsize) (N > 0 ? N : 1));
		MonoBasicBlock *b3;
		int nneed = 0;

		lcse = (WjLcse *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse));
		/* Only blocks that some LATER block inherits from need their exit state kept; for the diamond
		 * that is just the compare block. Saving one table per bb unconditionally would cost sizeof(WjLcse)
		 * times the block count, which is real for the 7000-instruction methods here. */
		for (b3 = cfg->bb_entry; b3; b3 = b3->next_bb) {
			int ci = bbidx [b3->block_num], pi;
			if (ci < 0 || b3->in_count != 1 || !b3->in_bb [0])
				continue;
			if (b3->flags & BB_EXCEPTION_HANDLER)
				continue;   /* reached by an EH edge, not by falling out of in_bb[0] */
			pi = (b3->in_bb [0]->block_num <= (int) cfg->max_block_num) ? bbidx [b3->in_bb [0]->block_num] : -1;
			if (pi < 0 || pi >= ci)
				continue;   /* back edge: the predecessor has not been emitted yet */
			if (!needed [pi]) { needed [pi] = 1; nneed++; }
		}
		if (nneed > 0) {
			lcse_exit = (WjLcse **) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse *) * (gsize) (N > 0 ? N : 1));
			for (i = 0; i < N; ++i)
				if (needed [i])
					lcse_exit [i] = (WjLcse *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjLcse));
		}
		{
			extern gboolean mono_wasm_jit_refdiag_name (const char *);
			if (G_UNLIKELY (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name)))
				printf ("WASM_JIT_LCSE %s: on, N=%d nvreg=%d single-pred-sources=%d\n",
					cfg->method->name, N, nvreg, nneed);
		}
	}

	/* NCE level 2: cross-bb facts, propagated down the DOMINATOR tree.
	 *
	 * The bb-local set alone leaves the commonest shape untouched. Measured on AABB:combine: its two
	 * AABB parameters are null-checked EIGHT TIMES EACH, spread over 15 basic blocks (the method is a
	 * chain of min/max branches), so a set that resets at every block boundary removes none of them.
	 * ContactSolver:solveVelocityConstraints, which is one huge block, halved from bb-local alone.
	 *
	 * A fact only survives an edge if the vreg cannot have changed in between, so this propagates ONLY
	 * vregs with no definition anywhere in the method -- parameters and `this`. "Exactly one defining
	 * instruction" is NOT enough: a def inside a loop re-executes, so a check in a dominator outside
	 * the loop says nothing about the second iteration. Never-written is the rule that needs no
	 * reasoning about back edges, and it is what the measured case wants anyway.
	 *
	 * Recognising the check here does not reuse the emitter's compare fusing; it matches the IR pair
	 * directly (OP_COMPARE_IMM imm=0 followed by OP_COND_EXC_EQ "NullReferenceException"), which is the
	 * shape MONO_EMIT_NULL_CHECK expands to and the same rule abcremoval.c uses. Requiring adjacency
	 * (modulo nops) can only miss facts, never invent them. */
	guint8 **nn_in = NULL;
	/* Clause-free only. The dominator relation does not model implicit exception transfers, so a
	 * non-null fact established on a null check's normal continuation can be false inside a catch or
	 * finally reached from BEFORE that check ran. A method with no EH clauses has no such transfer to
	 * be wrong about, which makes the propagation sound without any dataflow reasoning -- the same
	 * argument, keyed off the same property, as the RAISE_NOGC gate above. */
	/* MODE 2 -- dominator-tree propagation of never-written facts -- LIVED HERE AND IS DELETED.
	 *
	 * It was sound: the propagation is restricted to clause-free methods, because the dominator relation
	 * does not model implicit exception transfers, so a fact established on a null check's normal
	 * continuation can be false inside a catch entered from before that check ran -- and a method with
	 * no clauses has no such transfer to be wrong about. Same argument, same property, as the RAISE_NOGC
	 * gate. It simply bought nothing: interleaved A/B of mode 1 against mode 2 on jbox2d gave
	 * 0.721/0.744 vs 0.733/0.738 ms/step, i.e. inside run-to-run variance. Deleted rather than left as a
	 * second dataflow pass nobody runs. */

	/* In-method EH: build the per-method clause table the catch landing pad's dispatch helper walks.
	 * Maps each dense bb -> its IL offset (for is-address-protected), and each clause -> {flags, try
	 * IL-range, handler bbidx, catch_class}. g_malloc'd so it outlives this compile (the JITted module
	 * bakes its address); a small per-EH-method leak. */
	if (eh_on) {
		guint _ci; int _d;
		eh_table = (WasmEhTable *) g_malloc0 (sizeof (WasmEhTable));
		eh_table->method = cfg->method;
		eh_table->name = mname ? g_strdup (mname) : NULL;   /* diagnostics */
		eh_table->nbbs = N;
		eh_table->nclauses = (gint32) cfg->header->num_clauses;
		eh_table->il_offsets = (gint32 *) g_malloc0 (sizeof (gint32) * (N > 0 ? N : 1));
		eh_table->clauses = (WasmEhClause *) g_malloc0 (sizeof (WasmEhClause) * (eh_table->nclauses > 0 ? eh_table->nclauses : 1));
		for (_d = 0; _d < N; ++_d) eh_table->il_offsets [_d] = -1;
		{
			MonoBasicBlock *_prev = NULL;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				int _bd = bbidx [bb->block_num];
				/* bb->cil_code points into the IL of whichever method the block came FROM. For an INLINED
				 * block that is the INLINEE's buffer, so `bb->cil_code - cfg->header->code` is pointer
				 * arithmetic across two unrelated allocations and yields an arbitrary offset -- which is then
				 * what eh_dispatch tests against THIS method's try ranges at runtime. It is wrong in both
				 * directions: an inlined block genuinely inside a try can be scored outside it, so a catch
				 * that should fire does not and the exception escapes; or one outside can be scored inside,
				 * and the landing pad over-catches.
				 *
				 * mono keeps the caller-space offset in bb->real_offset: inline_method seeds the inlined
				 * blocks with the CALL SITE's offset (method-to-ir.c, sbblock/ebblock->real_offset =
				 * real_offset, called with cfg->real_offset), and mono's own mark_bb_in_region compares
				 * real_offset -- not cil_code arithmetic -- against clause ranges. It is also the
				 * semantically right answer here: inlined code is logically at the call site, so it should
				 * inherit exactly the call site's protection.
				 *
				 * Found via MONO_INLINELIMIT: the default of 20 IL bytes inlines too little to expose this,
				 * but at 30 enough inlined blocks land inside EH methods that a gson catch stopped firing and
				 * Fabric's mixin bootstrap died during boot. `auto=0` (whole JIT tier off) at the same limit
				 * boots fine, which is what pins it here rather than in the inliner. */
				gint32 _off = -1;
				if (bb->cil_code) {
					if (bb->cil_code >= cfg->header->code &&
					    bb->cil_code < cfg->header->code + cfg->header->code_size)
						_off = (gint32) (bb->cil_code - cfg->header->code);
					else
						_off = (gint32) bb->real_offset;   /* inlined: cil_code belongs to the inlinee */
				}
				/* A decompose-synthesized bb carries NO cil_code (il_offset -1). The important case is the
				 * InvalidCastException COND_EXC block that mono_decompose_typechecks splits out of a
				 * castclass/isinst INSIDE a try: with il_offset -1, eh_dispatch's `if (il < 0) return -1`
				 * can't find the enclosing clause, so a catchable exception ESCAPES the in-method landing pad
				 * (and pass-1's island walk mis-targets). Inherit the previous bb's IL offset when both bbs
				 * are in the SAME EH region (mono_replace_ins gives the synthesized block its origin's region),
				 * so the inherited offset really does fall inside the same clause; never inherit across a
				 * region boundary (that could over-catch). */
				if (_off < 0 && _prev && _prev->region == bb->region) {
					int _pd = bbidx [_prev->block_num];
					if (_pd >= 0 && _pd < N)
						_off = eh_table->il_offsets [_pd];
				}
				if (_bd >= 0 && _bd < N)
					eh_table->il_offsets [_bd] = _off;
				_prev = bb;
			}
		}
		for (_ci = 0; _ci < cfg->header->num_clauses; ++_ci) {
			MonoExceptionClause *_c = &cfg->header->clauses [_ci];
			WasmEhClause *_wc = &eh_table->clauses [_ci];
			MonoBasicBlock *_hb; int _hbb = -1;
			_wc->flags = (gint16) _c->flags;
			_wc->try_start = (gint32) _c->try_offset;
			_wc->try_len = (gint32) _c->try_len;
			_wc->catch_class = (_c->flags == MONO_EXCEPTION_CLAUSE_NONE) ? _c->data.catch_class : NULL;
			for (_hb = cfg->bb_entry; _hb; _hb = _hb->next_bb)
				/* Range-guarded for the same reason as the il_offsets loop above: only a block whose
				 * cil_code lies inside THIS method's IL can be the clause's handler entry. Without the test,
				 * an inlined block's cross-buffer difference can equal handler_offset by coincidence and --
				 * because this loop breaks on the first match -- send every catch to the wrong code. Rarer
				 * than the offset bug, worse when it happens. */
				if (_hb->cil_code &&
				    _hb->cil_code >= cfg->header->code &&
				    _hb->cil_code < cfg->header->code + cfg->header->code_size &&
				    (gint32) (_hb->cil_code - cfg->header->code) == (gint32) _c->handler_offset) { _hbb = bbidx [_hb->block_num]; break; }
			if (_hbb < 0) { fail = "eh handler bb not found"; goto done; }
			_wc->handler_bbidx = _hbb;
		}
	}

	/* prescan: does this method have any OP_*CALL_MEMBASE (a virtual/interface call = an inline-IC site)?
	 * If so we fetch &wj_slot_live / &wj_slot_live_cap once in the prologue (below) so each IC hit can do the
	 * liveness check INLINE instead of a per-hit mono_wasm_jit_slot_live call_indirect. Leaf methods with no
	 * vcall skip the fetch entirely (no wasted prologue C calls). */
	{
		MonoBasicBlock *_bb;
		for (_bb = cfg->bb_entry; _bb && !has_vcall; _bb = _bb->next_bb) {
			MonoInst *_ins;
			MONO_BB_FOR_EACH_INS (_bb, _ins) {
				switch (_ins->opcode) {
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
				case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
					has_vcall = TRUE; break;
				case OP_VCALL2_MEMBASE:
					has_vcall = mono_wasm_jit_vcall_vret != 0; break;
				default: break;
				}
				if (has_vcall) break;
			}
		}
	}

	/* Prologue: $blk is needed only by the universal dispatcher/EH landing pad. Structured non-EH
	 * layouts enter their first lexical block directly and never read it. */
	wasm_origin_push (&body, WJO_DISPATCH);   /* origin regions of the prologue are sequential: no pops */
	if (structured_cfg_kind == WJ_CFG_DISPATCH) {
		wasm_i32_const (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
	}

	wasm_origin_push (&body, WJO_FRAME);
#ifdef HOST_BROWSER
	/* C-STACK FRAME PROLOGUE. Reserve this method's ref+addr slots as a real frame on the emscripten
	 * C stack (see the layout doc at the top of this file): capture entry_sp, drop the SP by the
	 * 16-aligned frame size, zero the frame (GC must not scan garbage; .NET locals are zero-init),
	 * copy reference args into their slots, and derive addrbase. Every exit stackRestores entry_sp
	 * (EMIT_REF_LEAVE); the EH landing pad stackRestores refbase (this frame stays live, unwound
	 * callee frames fall below the SP and stop being scanned — no zeroing, no balance bookkeeping).
	 * An EH method with an EMPTY frame still captures entry_sp (refbase = entry_sp) so its landing
	 * pad can pop the frames of callees the C++ unwind tore through. */
	if ((framebytes > 0 && !lazy_ref_frame) || eh_on) {
		uses_calls = TRUE;
		/* entry_sp = the caller's frame base, read from __stack_pointer (imported wasm global 0). */
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) spentry_idx);
		if (framebytes > 0 && !lazy_ref_frame) {
			/* refbase (frame base) = (entry_sp - framebytes) & ~15; __stack_pointer = refbase */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_i32_const (&body, framebytes);
			wasm_op (&body, WASM_OP_I32_SUB);
			wasm_i32_const (&body, -16);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
			/* memory.fill (refbase, 0, framebytes): the frame is above the SP now, so the GC scans it —
			 * it must hold no garbage/stale pointers; .NET local zero-init falls out of the same fill */
			{
				extern int mono_wasm_jit_inline_zero, mono_wasm_jit_frame_zero;
				/* MONO_WASM_JIT_FRAME_ZERO=0 skips the REF region when every slot in it is write-through
				 * (R294): such a slot is a mirror rewritten at each def with no GC point between, and zeroed
				 * again at its kill point, so before its first def it can only hold a stale word -- which the
				 * CONSERVATIVE scan of this C-stack region treats as at most a pin, and only while a GC runs
				 * during this frame (R300 measured that cost: +20% stack-pinned bytes per minor GC, no
				 * heap growth). Zero-init is still owed to the address region (.NET locals) and to any
				 * slot-homed ref (addrslot -2), so either of those keeps the fill. */
				int zfrom = 0, zo;
				int zend = ehrec_on ? ehrec_off : framebytes;   /* R353: the EH record is written, never zeroed */
				if (!mono_wasm_jit_frame_zero && lc.ref_wt) {
					gboolean zall_wt = TRUE;
					for (i = 0; i < nvreg; ++i)
						if (refslot [i] >= 0 && !lc.ref_wt [i]) {
							zall_wt = FALSE;
							break;
						}
					if (zall_wt)
						zfrom = refbytes_al;   /* 8-aligned, so the i64 stores below stay aligned */
				}
				if (zfrom >= zend) {
					/* nothing owed */
				} else if (zend - zfrom <= mono_wasm_jit_inline_zero) {
					/* framebytes is a multiple of 16 and zfrom of 8, so these are aligned i64 stores with no
					 * remainder and no alignment fixup (zend is framebytes, or the 8-aligned record offset). */
					for (zo = zfrom; zo < zend; zo += 8) {
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
						wasm_i64_const (&body, 0);
						wasm_op (&body, WASM_OP_I64_STORE);
						wasm_memarg (&body, 3, (guint32) zo);
					}
				} else {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					if (zfrom > 0) {
						wasm_i32_const (&body, zfrom);
						wasm_op (&body, WASM_OP_I32_ADD);
					}
					wasm_i32_const (&body, 0);
					wasm_i32_const (&body, zend - zfrom);
					wasm_u8 (&body, 0xFC); wasm_uleb (&body, 11); wasm_u8 (&body, 0);   /* memory.fill mem 0 (bulk memory) */
				}
				if (zfrom > 0 && mono_wasm_jit_stats)
					mono_wasm_jit_add (WJC_FRAME_ZERO_SKIPPED, zfrom);
			}
			for (i = 0; i < nargs; ++i) {
				int av = cfg->args [i]->dreg;
				if (av >= 0 && av < nvreg && refslot [av] >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);   /* addr */
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) i);             /* incoming arg param */
					wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [av] * 4));
				}
			}
			if (naddrbytes > 0) {
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
				wasm_i32_const (&body, refbytes_al);
				wasm_op (&body, WASM_OP_I32_ADD);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) addrbase_idx);
			}
			/* Reconstruct ref-free scalar-vtype parameters, and address-taken scalar ones (arg_spill,
			 * MONO_WASM_JIT_LDADDR_ARG), in their address-frame slots. Ref-bearing scalar structs were copied
			 * to ref slots by the loop above. */
			for (i = 0; i < nargs; ++i) {
				int av = cfg->args [i]->dreg;
				if ((self_ci.args [i].kind == WJ_ARG_VTYPE_SCALAR || arg_spill [i]) && av >= 0 && av < nvreg && addrslot [av] >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) i);
					if (!wasm_addr_st (&body, &lc, av)) { fail = "scalar-vtype arg store"; goto done; }
				}
			}
		} else {
			/* eh_on with an empty frame: refbase = entry_sp is the landing pad's restore target */
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) refbase_idx);
		}
	}
	if (lazy_ref_frame)
		uses_calls = TRUE; /* imports the mutable __stack_pointer global used by ENSURE_REF_FRAME */
	wasm_origin_push (&body, WJO_IC_PROLOGUE);
	/* INLINE f-slot-IC liveness prologue: cache this dynamic instance's imported addresses of the
	 * wj_slot_live bitmap pointer + capacity. The imports are bound to the current worker's TLS block when
	 * the cached module is instantiated, so two global.get operations replace the former two C-boundary
	 * address-helper calls per invocation. Each IC hit still loads THROUGH these stable addresses, making
	 * a later bitmap realloc/capacity growth visible with no stale-pointer window. */
	if (has_vcall && !mono_wasm_jit_ic_remat) {   /* IC_REMAT: nothing preloaded, see EMIT_IC_VPIC_PTR */
		/* METHOD-LONG LIVE RANGES ARE NOT FREE -- the LOCALS are.
		 *
		 * Be precise about which, because the two suggest opposite work. A wasm local is free: V8's
		 * `LocalGet` is `result->op = ssa_env_[imm.index]` and `LocalSet` is `ssa_env_[imm.index] = value.op`
		 * (v8/src/wasm/turboshaft-graph-interface.cc:1030-1043), so local ops emit NO instructions and
		 * renaming or reusing local slots cannot change anything at runtime (which is why MONO_WASM_JIT_COALESCE
		 * measured inert). What costs is the DATAFLOW: a value that is live across other calls has to survive
		 * them, and the census of V8's x86 output attributes 24.2% of emitted instructions to TurboFan's own
		 * register spills (rbp/rsp traffic).
		 *
		 * So the thing to minimise is not the local count but how many values this prologue keeps alive for the
		 * whole body, each competing with the method's real values. That set is now the minimum:
		 *   - s.l / s.c (slot-live bitmap) are read ONLY by the shared WjDelegateIC arm, so they are not
		 *     fetched at all when the worker-local delegate PIC is on (the default) — 2 locals reclaimed;
		 *   - the cap ADDRESSES are consumed immediately to load their values, so they never need a local at
		 *     all: global.get; i32.load; local.set value — 2 more reclaimed.
		 * That is 4 fewer method-long locals in every method containing a virtual call. */
		/* MONO_WASM_JIT_DELEGATE_LOCAL_PIC shipped 1, so the slot-live bitmap pair is no longer fetched
		 * here at all: only the SHARED WjDelegateIC arm read it, and that arm is gone. Two method-long
		 * locals reclaimed in every method containing a virtual call -- and method-long locals are the
		 * thing that costs, not local COUNT (a wasm local is free; a value live across other calls is
		 * not). R189 measured WRITE-ONCE-EARLY slots at 17.4%% of slots carrying 30.1%% of all reload
		 * traffic, and these were two of them. */
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 3); /* imported s.v = &wj_vcall_pic */
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vpic_ptr_idx);
		wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 4); /* imported s.n = &wj_vcall_pic_cap */
		wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vpic_capval_idx);
		{
			/* The delegate PIC pair, on the same terms. Fetched unconditionally with the rest rather than
			 * gated on "has a delegate site": the gate would need a second prescan, and two global.get in a
			 * prologue that already does four are not worth another pass over the IR. */
			{
				wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 5); /* imported s.d = &wj_delegate_pic */
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dpic_ptr_idx);
				wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 6); /* imported s.m = &wj_delegate_pic_cap */
				wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dpic_capval_idx);
			}
		}
	}
#endif

	/*
	 * Structured-control dispatch (always correct for arbitrary CFGs):
	 *   loop { block^N { local.get $blk; br_table 0..N-1 } bb0; bb1; ... } unreachable
	 * br_table index i jumps just past block B_i, where bb_i's code lives. A branch
	 * to bb T sets $blk=idx(T) and br's back to the loop to re-dispatch.
	 *
	 * In-method EH (eh_on): wrap the whole dispatch in `loop $outer { try { <dispatch> } catch <x.e> {
	 * <clause-walk landing pad>; br $outer } }`. A C++/wasm-EH unwind out of any bb is caught here; the
	 * landing pad maps the throwing bb ($blk) -> a handler bb (set $blk + br $outer to re-dispatch) or
	 * rethrows. The inner dispatch + all GOTO depths are UNCHANGED — outer-loop/try sit ABOVE the inner
	 * loop, so a GOTO's depth to the inner loop is unaffected. */
	{
		/* ()->void for mono_wasm_jit_continue_unwind. Registered for every method, not just eh_on ones:
		 * a non-EH JIT frame must ALSO continue the unwind rather than return, otherwise it is skipped
		 * and the exception is delivered late at an unrelated frame. functype_eq dedups, so an eh_on
		 * method shares this entry with eh_endcatch_ti. */
		WasmFuncType _t; int _k;
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { resid_void_ti = ti_base + _k; break; }
		if (resid_void_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; resid_void_ti = ti_base + nextra++; }
	}
	if (eh_on) {
		WasmFuncType _t; int _k;
		/* (i32)->void: the x.e tag type AND mono_jiterp_begin_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 1; _t.params [0] = WASM_I32; _t.ret = WASM_VOID;
		eh_type_idx = -1;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_type_idx = ti_base + _k; break; }
		if (eh_type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_type_idx = ti_base + nextra++; }
		/* (i32,i32)->i32: mono_wasm_jit_eh_dispatch (table, blk) -> handler bbidx (a finally/fault
		 * handler's bbidx is tagged with WJ_EH_DISPATCH_FINALLY_BIT; the landing pad strips it) or -1 */
		memset (&_t, 0, sizeof _t); _t.nparams = 2; _t.params [0] = WASM_I32; _t.params [1] = WASM_I32; _t.ret = WASM_I32;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_dispatch_ti = ti_base + _k; break; }
		if (eh_dispatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_dispatch_ti = ti_base + nextra++; }
		/* ()->void: mono_jiterp_end_catch */
		memset (&_t, 0, sizeof _t); _t.nparams = 0; _t.ret = WASM_VOID;
		for (_k = 0; _k < nextra; ++_k) if (functype_eq (&extra_types [_k], &_t)) { eh_endcatch_ti = ti_base + _k; break; }
		if (eh_endcatch_ti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _t; eh_endcatch_ti = ti_base + nextra++; }
		uses_calls = TRUE;
		uses_eh_tag = TRUE;
		wasm_origin_push (&body, WJO_EH_PUSH);
		/* PROLOGUE: push this EH method's il_state island so mono's exception pass-1 sees it on ANY entry
		 * (interp->JIT boundary OR direct JIT->JIT f-slot call). method ptr baked; (i32)->void = eh_type_idx.
		 * Popped at every exit by EMIT_REF_LEAVE + the rethrow paths. In record mode (R353) the push is inline and
		 * the island call is not emitted at all. */
#ifdef HOST_BROWSER
		if (ehrec_on) {
			EMIT_EHREC_PUSH ();
			mono_wasm_jit_count (WJC_EHREC_EMITTED);
		} else {
		wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
		{
			extern gpointer mono_wasm_jit_enter_island (MonoMethod *m, int ndata);
			/* args+locals this method can address, for the island's partial zeroing. Same expression the
			 * `_nd > 256` eligibility bail uses; both must agree or the callee falls back to full zeroing. */
			MonoMethodSignature *_isig = mono_method_signature_internal (cfg->method);
			int _indata = (_isig && _isig->hasthis ? 1 : 0) + (_isig ? (int) _isig->param_count : 0)
				+ (cfg->header ? (int) cfg->header->num_locals : 0);
			/* enter_island now RETURNS this activation's MonoMethodILState, so its wasm signature is
			 * (i32)->i32 and the call must be typed that way on BOTH paths — typing it (i32)->void because
			 * the result happens to be unused is a signature mismatch, i.e. an instant call_indirect trap or
			 * a failed import. Register the type through the same extras table the call sites use. */
			WasmFuncType nt;
			int k;
			memset (&nt, 0, sizeof (nt));
			nt.params [0] = WASM_I32; nt.params [1] = WASM_I32; nt.nparams = 2; nt.ret = WASM_I32;
			for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { ilofs_ti = ti_base + k; break; }
			if (ilofs_ti < 0) {
				if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
				extra_types [nextra] = nt; ilofs_ti = ti_base + nextra++;
			}
			wasm_i32_const (&body, _indata);   /* 2nd arg: args+locals to zero */
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_enter_island, (guint32) ilofs_ti);
			wasm_op (&body, WASM_OP_DROP);
		}
		}
#else
		wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff6, (guint32) eh_type_idx);
#endif
		wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);   /* $outer: catch re-dispatch loop */
		wasm_op (&body, WASM_OP_TRY); wasm_u8 (&body, 0x40);    /* in-method EH try region (catches x.e) */
	}
	wasm_origin_push (&body, WJO_DISPATCH);
	/*
	 * SHARED THROW BLOCKS.
	 *
	 * Every raise used to be inlined at its check site: `if (cond) { call_indirect <raise>; unreachable }`.
	 * AABB:combine had 23 of those and TurboFan turned the method into 880 instructions with 118 `call`s,
	 * where RyuJIT needs ~40 and zero. The cost is not the calls themselves -- they are on cold paths --
	 * it is that each `call_indirect` is a CALL SITE to the register allocator (everything live across it
	 * must be assumed clobbered) and each one carries V8's table-index + signature-check sequence, which
	 * is where the 23 matching or/shl/shr triples in the disassembly come from.
	 *
	 * So emit ONE raise per exception id, past the end of the body, and branch to it:
	 *     block $throw_id ... br_if $throw_id ... end ; <raise id> ; unreachable
	 * A check becomes three instructions and no call.
	 *
	 * Placement: inside the EH try when eh_on, or a raise from the shared block would no longer be
	 * catchable by this method's own landing pad. That also makes the depth arithmetic uniform -- from
	 * inside bb_i the labels are B_{i+1}..B_{N-1} (N-1-i of them), the dispatch loop, then throw[0..K-1],
	 * so throw[t] sits at depth N - i + t regardless of whether the EH scaffolding is present.
	 */
	/*
	 * A single-basic-block method has nowhere to dispatch to, yet it still paid the full
	 * `loop { block { local.get $blk; br_table } }` scaffolding: a $blk init in the prologue, an
	 * indirect jump at entry, and a trailing `unreachable`. That is most of why
	 * TreeNodeStack:getCount() -- literally `return count;` -- compiled to 103 instructions and 523
	 * bytes. Skip the scaffolding entirely when there is exactly one bb, it has no outgoing edges (so
	 * no branch can target it and no back edge exists), and there is no EH wrapper whose landing pad
	 * would re-dispatch through $blk.
	 */
	/* MONO_WASM_JIT_NODISPATCH gated the single-bb case on its own and is deleted: STRUCTURED_CFG ships
	 * 1 and subsumes it, so the narrow rule was dead code behind a knob nobody set. The reasoning above
	 * is why the single-bb case matters at all and stays here. */
	gboolean structured_cfg = structured_cfg_kind != WJ_CFG_DISPATCH;
	gboolean skip_dispatch = structured_cfg;

	int wj_throw_slot [WJ_EXC_IDS];
	int nthrow = 0;
	for (i = 0; i < WJ_EXC_IDS; ++i) wj_throw_slot [i] = -1;
	{
		MonoBasicBlock *tb;
		MonoInst *ti;
		gboolean want [WJ_EXC_IDS];
		memset (want, 0, sizeof (want));
		for (tb = cfg->bb_entry; tb; tb = tb->next_bb) {
			MONO_BB_FOR_EACH_INS (tb, ti) {
				switch (ti->opcode) {
				/* NB: OP_NOT_NULL is deliberately absent -- it is a fact marker that emits nothing, so it
				 * never branches to the NRE block and must not be what keeps that block alive. */
				case OP_CHECK_THIS:
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
				case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
					want [4] = TRUE;   /* NullReferenceException: explicit checks + vcall receiver check */
					break;
				case OP_VCALL2_MEMBASE:
					if (mono_wasm_jit_vcall_vret)
						want [4] = TRUE;
					break;
				case OP_LCONV_TO_OVF_I4: case OP_LCONV_TO_OVF_I4_UN: case OP_LCONV_TO_OVF_I: case OP_LCONV_TO_OVF_I_UN:
					want [0] = TRUE;   /* R323: OverflowException */
					break;
				default:
					if (MONO_IS_COND_EXC (ti)) {
						int id = wj_exc_id_for_name ((const char *) ti->inst_p1);
						if (id >= 0) want [id] = TRUE;
					}
					break;
				}
			}
		}
		for (i = 0; i < WJ_EXC_IDS; ++i)
			if (want [i]) wj_throw_slot [i] = nthrow++;
	}
	/* Outermost first, so the last one emitted (throw[0]) is innermost and depth N-i+t holds. */
	for (i = nthrow - 1; i >= 0; --i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }

	if (!skip_dispatch) {
		wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);
		for (i = 0; i < N; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dispatch_idx);
		wasm_op (&body, WASM_OP_BR_TABLE);
		wasm_uleb (&body, (guint32) N);
		for (i = 0; i < N; ++i)
			wasm_uleb (&body, (guint32) i);
		wasm_uleb (&body, 0); /* default -> block 0 */
	} else if (structured_cfg_kind == WJ_CFG_FORWARD) {
		/* One lexical label per bb. Closing the first label enters bb0; the remaining labels are
		 * precisely the direct targets of forward edges. A one-bb leaf needs no label at all. */
		if (N > 1)
			for (i = 0; i < N; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
	} else if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP) {
		int suffix_n = N - (structured_loop_l + 1);
		/* Suffix labels surround the whole prefix+loop so an early exit can jump over both. Prefix
		 * labels are innermost and close one-by-one until execution reaches the loop entry. */
		for (i = 0; i < suffix_n; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
		/* One extra prefix block labels the loop entry itself, so a branch can skip the remaining
		 * prefix without requiring a dispatcher. No prefix means entry falls straight into the loop. */
		if (structured_loop_h > 0)
			for (i = 0; i < structured_loop_h + 1; ++i) { wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); }
	}

/*
 * Edge lowering. EXTRA is the number of wasm blocks opened INSIDE this bb's code at the branch point
 * (0 at bb top level, 1 inside an `if`/`else`) -- not an absolute depth as it used to be.
 *
 * The old form sent EVERY edge, including plain fallthrough, back through the dispatch loop:
 * `i32.const j; local.set $blk; br <loop>`, re-running `local.get $blk; br_table`. TurboFan lowers that
 * br_table to `lea <table>(%rip),%r10; jmp *(%r10,%rN,8)` -- an INDIRECT jump with as many live targets
 * as the method has basic blocks. Measured on the jbox2d bench, those two instructions are 23.8% of
 * AABB:combine, 17.3% of DynamicTree:insertLeaf, 11.8% of DynamicTree:query, ~7% of ContactSolver and
 * of DynamicTree:balance. It is not the branch itself that costs -- it is that an indirect jump cycling
 * through ~15 targets defeats the indirect predictor, and every miss is a front-end refill. RyuJIT
 * compiles the same control flow to short direct jbe/jmp pairs and never builds a table.
 *
 * The nesting arithmetic makes the fix exact rather than heuristic. Inside bb_i the still-open labels
 * are, innermost first, the block whose `end` precedes bb_{i+1}, then bb_{i+2}'s, ... and finally the
 * dispatch loop at depth N-1-i. So:
 *   j == i+1  -> fall out of the bb; the `end` at the top of the next iteration IS the target
 *   j >  i    -> br (j - i - 1), a direct forward branch, no $blk write, no re-dispatch
 *   j <= i    -> unchanged: set $blk and br to the loop, the only case that needs the br_table
 * With a sane bb order that leaves the dispatch running once per call (the entry) plus real back edges.
 */
/* Plan A5: the label depth of an edge in any structured form (the nested form reads its precomputed scope stacks). */
#define WJ_EDGE_DEPTH(FROM, TO) (structured_cfg_kind == WJ_CFG_NESTED ? wj_nest_depth (&nest, (FROM), (TO)) \
	: wj_structured_branch_depth (structured_cfg_kind, structured_loop_h, structured_loop_l, N, (FROM), (TO)))
#define GOTO(TBB, EXTRA) do { \
		int _ti = bbidx [(TBB)->block_num]; \
		if (_ti < 0) { fail = "bad branch target"; goto done; } \
		if (structured_cfg) { \
			int _d = WJ_EDGE_DEPTH (i, _ti); \
			if (_d < 0) { fail = "structured cfg edge escaped verifier"; goto done; } \
			/* A direct adjacent forward edge is lexical fallthrough (nested form: whatever scopes are still open \
			 * between here and bb i+1 all end at boundary i+1, so falling out of them reaches it). A self/back edge \
			 * at depth zero still needs a br to the loop label. */ \
			if (structured_cfg_kind == WJ_CFG_NESTED && _ti == i + 1 && (EXTRA) == 0) \
				; \
			else if (_d + (EXTRA) != 0 || _ti <= i) { wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) (_d + (EXTRA))); } \
		} else if (_ti > i) { \
			int _d = _ti - i - 1 + (EXTRA); \
			/* depth 0 at bb top level is the fallthrough edge: emitting nothing gets there. */ \
			if (_d != 0) { wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) _d); } \
		} else { \
			wasm_i32_const (&body, _ti); \
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx); \
			wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) (N - 1 - i + (EXTRA))); \
		} \
	} while (0)

/* Branch to the shared raise site for EXC_ID, consuming a "should throw" i32 already on the stack.
 * EXTRA is the same in-bb nesting count GOTO takes. WJ_HAS_THROW guards the (never expected) case where
 * the prescan did not reserve a block for this id, so the caller can fall back to the inline form. */
#define WJ_HAS_THROW(EXC_ID) ((EXC_ID) >= 0 && (EXC_ID) < WJ_EXC_IDS && wj_throw_slot [(EXC_ID)] >= 0)
/* R339: hint the `if`/`br_if` about to be emitted (1 likely, 0 unlikely) when MONO_WASM_JIT_BRANCH_HINTS is on. */
#define WJ_HINT(V) do { extern int mono_wasm_jit_branch_hints; if (mono_wasm_jit_branch_hints) wasm_hint (&body, (V)); } while (0)
/* R341: level-2 hints (MONO_WASM_JIT_BRANCH_HINTS=2), counted per site kind as they are emitted. */
#define WJ_HINT2(V, CTR) do { extern int mono_wasm_jit_branch_hints; if (mono_wasm_jit_branch_hints >= 2) { wasm_hint (&body, (V)); wj_count (CTR); } } while (0)
/* R341: an IR branch whose successors differ in out_of_line -- the cold one is the unlikely one. */
#define WJ_COND_HINT() do { \
		gboolean _tc = ins->inst_true_bb->out_of_line, _fc = ins->inst_false_bb && ins->inst_false_bb->out_of_line; \
		if (_tc != _fc) WJ_HINT2 (_tc ? 0 : 1, WJC_BRANCH_HINT_OOL); \
	} while (0)
#define WJ_THROW_BR(EXC_ID, EXTRA) do { \
		int _od = structured_cfg_kind == WJ_CFG_NESTED ? nest.stk_len [i] \
			: structured_cfg ? wj_structured_outer_depth (structured_cfg_kind, structured_loop_h, \
			structured_loop_l, N, i) : ((N - 1 - i) + (skip_dispatch ? 0 : 1)); \
		if (_od < 0) { fail = "structured throw depth"; goto done; } \
		WJ_HINT (0); \
		wasm_op (&body, WASM_OP_BR_IF); \
		wasm_uleb (&body, (guint32) (_od + wj_throw_slot [(EXC_ID)] + (EXTRA))); \
	} while (0)

/* TRUE when the target has a lexical label at this point, allowing a direct br_if. The verified
 * prefix -> loop-header edge is deliberately fallthrough-only because the loop label is not open yet. */
#define GOTO_HAS_LABEL(TBB) (structured_cfg_kind == WJ_CFG_NESTED ? (wj_nest_depth (&nest, i, bbidx [(TBB)->block_num]) >= 0) \
	: structured_cfg ? wj_structured_target_has_label (structured_cfg_kind, structured_loop_h, structured_loop_l, i, \
		bbidx [(TBB)->block_num]) : (bbidx [(TBB)->block_num] > i))

/*
 * Two-way branch, with the comparison result already on the wasm stack.
 *
 * When the true target is forward this becomes `br_if <depth>` followed by the false edge -- so the
 * common shape (both targets forward, false target adjacent) collapses to a single conditional branch
 * and a fallthrough, which is what RyuJIT emits for the same IR. The if/else form is kept only for a
 * backward true target, where $blk has to be written before branching and there is nowhere to put that
 * inside a br_if.
 */
#define COND_BRANCH() do { \
		if (GOTO_HAS_LABEL (ins->inst_true_bb)) { \
			int _ct = bbidx [ins->inst_true_bb->block_num]; \
			int _cd = structured_cfg ? WJ_EDGE_DEPTH (i, _ct) : (_ct - i - 1); \
			if (_cd < 0) { fail = "structured conditional edge escaped verifier"; goto done; } \
			WJ_COND_HINT (); \
			wasm_op (&body, WASM_OP_BR_IF); \
			wasm_uleb (&body, (guint32) _cd); \
			GOTO (ins->inst_false_bb, 0); \
		} else { \
			WJ_COND_HINT (); \
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40); \
			GOTO (ins->inst_true_bb, 1); \
			wasm_op (&body, WASM_OP_ELSE); \
			GOTO (ins->inst_false_bb, 1); \
			wasm_op (&body, WASM_OP_END); \
		} \
	} while (0)

	i = 0;
	int sl_ord = 0;   /* running ins ordinal, mirroring the SLOTLIVE walk exactly (same bb/ins iteration) */
#ifdef HOST_BROWSER
	/* R371 census: each block's bytes are attributed at the NEXT block's start (and after the loop), so no `continue`
	 * inside a block can skip the accounting. */
	guint32 wj_cold_start = body.len, wj_cold_mark = body.len;
	int wj_cold_cls = 0;
#define WJ_COLD_ACCOUNT() do { \
		mono_wasm_jit_counters [WJC_COLD_BYTES_OTHER + wj_cold_cls] += body.len - wj_cold_mark; \
		if (wj_cold_cls && cfg->wasm_jit_tier >= 2) \
			mono_wasm_jit_counters [WJC_COLD_T2_BYTES_COLD] += body.len - wj_cold_mark; \
		wj_cold_mark = body.len; \
	} while (0)
#endif
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb, ++i) {
		int loop_depth = N - 1 - i;
		gboolean terminated = FALSE;
		wasm_origin_push (&body, WJO_DISPATCH);   /* this bb's label ends / loop headers, until its first ins */
#ifdef HOST_BROWSER
		WJ_COLD_ACCOUNT ();
		wj_cold_cls = wj_bb_cold_class (bb);
#endif
		/* LAZY_COLD=2: inside a doomed bb a cold frame behaves as an ordinary lazy frame (see lazy_doomed). */
		if (lazy_cold_frame)
			lc.lazy_frame = (lazy_doomed && lazy_doomed [i]) ? 1 : 2;
		int cmp_a = -1, cmp_b = -1;
		gint32 cmp_imm = 0;
		gint64 cmp_imm64 = 0;      /* the immediate for an i64 compare-imm (OP_LCOMPARE_IMM); cmp_imm holds the i32 one */
		gboolean cmp_imm_mode = FALSE;
		WasmValtype cmp_float = 0; /* 0 = int compare; WASM_F32/WASM_F64 = float compare (set by OP_(F|R)COMPARE) */
		gboolean cmp_i64 = FALSE;  /* TRUE after OP_LCOMPARE(_IMM): the fused branch/setcc/cond_exc uses i64 compare ops */
		WasmOpcode cmp_wop = WASM_OP_I32_EQ;

		/* Seed this bb with what its dominators already proved (never-written vregs only); everything
		 * else starts unknown and is rebuilt bb-locally below. */
		if (nn) {
			if (nn_in && i < N && nn_in [i]) memcpy (nn, nn_in [i], (gsize) nvreg);
			else memset (nn, 0, (gsize) nvreg);
		}

		/* LCSE: inherit the load table only across a single-predecessor edge from an already-emitted
		 * block (see the allocation comment). Anything else starts empty. */
		if (lcse) {
			lcse->nld = lcse->nal = 0;
			if (lcse_exit && bb->in_count == 1 && bb->in_bb [0] && !(bb->flags & BB_EXCEPTION_HANDLER)) {
				int pi = (bb->in_bb [0]->block_num <= (int) cfg->max_block_num) ? bbidx [bb->in_bb [0]->block_num] : -1;
				if (pi >= 0 && pi < i && lcse_exit [pi]) {
					*lcse = *lcse_exit [pi];
					if (lcse->nld) lcse_inherits++;
				}
			}
		}

		if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && i == structured_loop_h) {
			int body_n = structured_loop_l - structured_loop_h + 1;
			if (structured_loop_h > 0)
				wasm_op (&body, WASM_OP_END); /* branch target for the loop entry */
			wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40); /* natural-loop header */
			for (int _bi = 0; _bi < body_n; ++_bi) {
				wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);
			}
		}
		if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && i == structured_loop_l + 1)
			wasm_op (&body, WASM_OP_END); /* close natural loop before suffix */
		if (!skip_dispatch)
			wasm_op (&body, WASM_OP_END); /* close dispatcher block B_i; bb_i code follows */
		else if (structured_cfg_kind == WJ_CFG_FORWARD) {
			if (N > 1)
				wasm_op (&body, WASM_OP_END); /* close lexical forward label for bb_i */
		} else if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP) {
			/* Prefix, loop body and suffix each opened exactly one label per member. */
			wasm_op (&body, WASM_OP_END);
		} else if (structured_cfg_kind == WJ_CFG_NESTED) {
			/* Boundary i: close what ends here (innermost first), then open what starts here (outermost first). */
			int _k;
			for (_k = 0; _k < nest.nclose [i]; ++_k)
				wasm_op (&body, WASM_OP_END);
			for (_k = 0; _k < nest.open_len [i]; ++_k) {
				wasm_op (&body, nest.loop [nest.open [nest.open_off [i] + _k]] ? WASM_OP_LOOP : WASM_OP_BLOCK);
				wasm_u8 (&body, 0x40);
			}
		}

#ifdef HOST_BROWSER
		/* AOT-style in-method EH (milestone 2b): record this bb's IL offset into the active island il_state
		 * (pushed at the interp->JIT boundary) so mono's exception pass-1 matches THIS method's enclosing
		 * try and finds its catch as the NEAREST handler for a throwing AOT callee — stopping the walk here
		 * and running no outer frames' finally clauses. (i32)->void via eh_type_idx. EH methods only. */
		if (eh_on) {
			extern void mono_wasm_jit_set_il_offset (int il_offset);
			wasm_origin_push (&body, WJO_BB_ILOFS);
			/* $blk must still identify the CURRENT bb: the catch landing pad passes it to
			 * mono_wasm_jit_eh_dispatch to find the handler for the bb that threw. Direct forward
			 * branches no longer write it on every edge, so write it once per bb instead -- still far
			 * cheaper than the per-edge br_table re-dispatch it replaces. */
			wasm_i32_const (&body, i);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
			{
				extern int mono_wasm_jit_il_state_offset_off (void);
				/* MONO_WASM_JIT_ILOFS_GLOBAL ships 1 and is MEASURED: the set_il_offset helper symbol went
				 * 1.38/1.45 -> 0.00 M instr/frame on the client render thread, sibling island helpers
				 * unchanged and both negative controls flat. Symbol ELIMINATION, not a magnitude estimate.
				 * It works where INLINE_ILOFS failed, for a specific reason: it `global.get`s the il_state
				 * pointer at each store site instead of caching it in a method-long local, so it leaves
				 * NOTHING live across the method's calls. */
				if (ehrec_on) {
					/* R353: straight into this frame's own record through refbase, which is live for the
					 * landing pad anyway -- no load of the current island at all */
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					wasm_i32_const (&body, eh_table->il_offsets [i]);
					EHREC_I32_STORE (mono_wasm_jit_ehrec_layout ()->il_offset);
				} else {
					/* il_state->il_offset = <offset>, through imported global s.i, no call and nothing
					 * left live: global.get &cur_island_il_state; load it; push the offset; store. */
					wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 8); /* s.i = &mono_wasm_jit_cur_island_il_state */
					wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
					wasm_i32_const (&body, eh_table->il_offsets [i]);
					wasm_op (&body, WASM_OP_I32_STORE);
					wasm_memarg (&body, 2, (guint32) mono_wasm_jit_il_state_offset_off ());
				}
			}
		}
#endif

		MONO_BB_FOR_EACH_INS (bb, ins) {
			int my_ord = sl_ord++;   /* bump for EVERY ins (incl. terminated-skipped) to stay aligned with the SLOTLIVE walk */
			if (terminated) continue;   /* skip instrs after a bb terminator (e.g. the OP_BR following OP_CALL_HANDLER) */
			wj_origin_ins (&body, cfg, ins);
			{ extern int mono_wasm_jit_site_count;   /* call-site census: diagnostic, never in a timing arm */
			  wj_site_cur = NULL;
			  if (G_UNLIKELY (mono_wasm_jit_site_count) && wj_site_is_call (ins->opcode))
				wj_site_count_ins (&body, cfg, ins, mname); }
			{ extern int mono_wasm_jit_s2;   /* plan2x S2: tier-1 site counts for tier 2's inliner */
			  if (G_UNLIKELY (mono_wasm_jit_s2 & 1) && cfg->wasm_jit_tier < 2 && wj_site_is_call (ins->opcode))
				wj_s2_count_ins (&body, cfg, ins); }
			if (G_UNLIKELY (wj_edge_on))
				wj_edge_wptr = NULL;   /* a call emitted while lowering a non-call instruction is not weighted */
			if (G_UNLIKELY (wj_edge_on) && wj_site_is_call (ins->opcode)) {   /* plan B4: this site's tier-1 count */
				gconstpointer s2code; gint32 s2il; WjS2Rec *s2r = NULL;
				if (wj_s2_site_of (cfg, ins->cil_code, &s2code, &s2il))
					s2r = wj_s2_find (cfg->method, s2code, s2il);
				if (s2r) {
					wj_edge_wptr = &s2r->count; wj_edge_est = 0;
				} else {
					/* a site tier 1 never counted (inside a tier-2 inlinee): the root's tier-1 entry count, estimated */
					WjS2Rec *er = (cfg->header && cfg->header->code) ? wj_s2_find (cfg->method, cfg->header->code, -1) : NULL;
					wj_edge_wptr = er ? &er->count : NULL; wj_edge_est = 1;
				}
			}

			/* NCE kill point. Every value-producing store in this emitter goes through
			 * wasm_st(ins->dreg), so killing dreg here covers every write to a tracked vreg (the lone
			 * exception, OP_SETRET's cfg->ret->dreg, kills itself in its own case). Done BEFORE the
			 * instruction is lowered, since the null-check sites only ever consult SOURCE vregs.
			 * An OP_MOVE carries the fact across, which is what keeps a receiver proven after the
			 * copies mini's call-arg setup inserts -- captured before the kill so dreg == sreg1 is safe. */
			if (G_UNLIKELY (nn != NULL)) {
				gboolean nn_move = (ins->opcode == OP_MOVE) && NN_GET (ins->sreg1);
				NN_KILL (ins->dreg);
				if (nn_move) NN_SET (ins->dreg);
			}
			/* LCSE kill point, same placement and for the same reason as NCE's: the instruction's own
			 * sources must still resolve against the pre-def state. A copy additionally carries the
			 * equivalence forward, which is what lets a load keyed on the copy hit the original's entry. */
			if (G_UNLIKELY (lcse != NULL)) {
				/* SCALAR moves only. OP_VMOVE/OP_XMOVE copy a value type through memory rather than
				 * moving a wasm local, so treating one as a value equivalence would key later lookups
				 * off the wrong storage. */
				int mv_src = wj_ins_is_move (ins->opcode) ? wj_lcse_canon (lcse, ins->sreg1) : -1;
				if (wj_ins_clobbers_mem (ins))
					lcse->nld = lcse->nal = 0;
				else
					wj_lcse_kill (lcse, li, nvreg, ins->dreg);
				if (mv_src >= 0 && mv_src < nvreg && ins->dreg >= 0 && ins->dreg < nvreg &&
				    mv_src != ins->dreg && lc.vt && lc.vt [mv_src] == lc.vt [ins->dreg] &&
				    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [mv_src] != -1)))
					wj_lcse_add_alias (lcse, ins->dreg, mv_src);
			}
			/* SLOTZERO kill point: the vregs chained at this ordinal had their last use at the PREVIOUS
			 * instruction — zero their (now dead) frame slots so the objects stop pinning. Emitted before
			 * this instruction's own code; stack-neutral, and under REF_WT (required) every read comes
			 * from the wasm local, so a zeroed slot is never read. */
			if (G_UNLIKELY (sl_kill_head != NULL)) {
				int kv = sl_kill_head [my_ord];
				guint16 _wjo_sz = wasm_origin_push (&body, WJO_SLOTZERO);
				/* A cold-only lazy frame is never live at a kill point, so it emits no zero store -- but the
				 * LCSE kill below is about the ANALYSIS, not the store, and must still run. */
				if (lazy_ref_frame && !lazy_cold_frame && kv >= 0) {
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) spentry_idx);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				}
				while (kv >= 0) {
					if (!lazy_cold_frame) {
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
						wasm_i32_const (&body, 0);
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (refslot [kv] * 4));
					}
					/* The pin is gone, so the value must not be reused past this point: LCSE's reuse
					 * read is created here at emission and the liveness that decided this slot was dead
					 * never saw it. Killing the entry keeps the two in agreement. */
					if (G_UNLIKELY (lcse != NULL))
						wj_lcse_kill (lcse, li, nvreg, kv);
					kv = sl_kill_next [kv];
				}
				if (lazy_ref_frame && !lazy_cold_frame && sl_kill_head [my_ord] >= 0)
					wasm_op (&body, WASM_OP_END);
				wasm_origin_pop (&body, _wjo_sz);
			}
#ifdef HOST_BROWSER
			if (wj_ins_is_effective_gcpoint (cfg, ins) &&
			    !(lazy_ref_frame && ins->opcode == OP_GC_SAFE_POINT) &&
			    !(lazy_cold_frame && wj_ins_is_raise (ins)) &&
			    !(lazy_ref_frame && terminal_vcall_handoff && ins == terminal_vcall_ins))
				ENSURE_REF_FRAME ();
#endif
			switch (ins->opcode) {
			case OP_NOP: case OP_IL_SEQ_POINT: case OP_SEQ_POINT:
			case OP_DUMMY_USE: case OP_NOT_REACHED: case OP_START_HANDLER:
			/* GC liveness annotations emitted under cfg->compute_gc_maps (set for COMPILE_WASM to get
			 * MINI's structural ref/mp vreg marking). They only feed the precise-GC-map builder, which
			 * is compiled out (mini-gc.c #if 0) — the wasm backend's ref classification reads the
			 * vreg_is_ref/vreg_is_mp arrays directly, so these are pure no-ops here. */
			case OP_GC_LIVENESS_DEF: case OP_GC_LIVENESS_USE:
			case OP_GC_PARAM_SLOT_LIVENESS_DEF: case OP_GC_SPILL_SLOT_LIVENESS_DEF:
				break;
			case OP_CALL_HANDLER: {
				/* milestone 2c: "call" a finally subroutine. We don't call — we record the continuation bb
				 * index in finally_ind, then GOTO the finally bb; OP_ENDFINALLY reads finally_ind to branch
				 * back. The leave's OP_BR (whose target IS the continuation) immediately follows and is
				 * skipped via the `terminated` guard above. Bail on a non-OP_BR continuation (a nested-finally
				 * leave emits multiple OP_CALL_HANDLERs in one bb — not supported yet). */
				int _cont; MonoInst *_nx = ins->next;
				while (_nx && (_nx->opcode == OP_NOP || _nx->opcode == OP_IL_SEQ_POINT || _nx->opcode == OP_SEQ_POINT)) _nx = _nx->next;
				if (!_nx || _nx->opcode != OP_BR || !_nx->inst_target_bb) { fail = "finally: complex call_handler continuation"; goto done; }
				_cont = bbidx [_nx->inst_target_bb->block_num];
				if (_cont < 0) { fail = "finally: bad continuation"; goto done; }
				wasm_i32_const (&body, _cont);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) finally_ind_idx);
				GOTO (ins->inst_target_bb, 0);
				terminated = TRUE;
				break;
			}
			case OP_ENDFINALLY: {
				/* milestone 2c: end of a finally handler. finally_ind == -1 => the finally ran on the
				 * EXCEPTION path (the catch landing pad set it) -> re-raise the in-flight exception
				 * (mono_wasm_jit_endfinally_rethrow, ()->void, never returns). Otherwise finally_ind holds the
				 * continuation bb index of the OP_CALL_HANDLER that ran us (normal leave) -> GOTO it. */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) finally_ind_idx);
				wasm_i32_const (&body, -1);
				wasm_op (&body, WASM_OP_I32_EQ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);    /* exception path */
				/* Re-raise the in-flight exception WITHOUT popping this method's il_state island here. The
				 * re-raise (mono_wasm_jit_endfinally_rethrow -> mono_wasm_jit_rethrow) runs a fresh exception
				 * pass-1 that MUST still see this island, so it (a) stops at THIS method for an enclosing
				 * catch/finally (advancing il_offset past the just-run finally) instead of walking past the
				 * unwinder-invisible JITted frame and running OUTER frames' finally clauses prematurely, and
				 * (b) is re-caught by this method's OWN wasm landing pad. The island is popped EXACTLY ONCE on
				 * the genuine exit: the catch landing pad's h<0 rethrow (leave_island + RETHROW) or the
				 * normal-return EMIT_REF_LEAVE. Popping it here too would double-pop — removing the ENCLOSING
				 * JITted EH caller's island (jit-order-dependent thread-kill / EH corruption). */
#ifdef HOST_BROWSER
				{ extern void mono_wasm_jit_endfinally_rethrow (void); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_endfinally_rethrow, eh_endcatch_ti); }
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff5, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
				wasm_op (&body, WASM_OP_UNREACHABLE);   /* the re-raise never returns */
				wasm_op (&body, WASM_OP_END);           /* end if */
				/* normal leave path: GOTO finally_ind (the continuation) */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) finally_ind_idx);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);
				wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, (guint32) loop_depth);
				terminated = TRUE;
				break;
			}
			case OP_GET_EX_OBJ: {
				/* In a JITted catch handler: load the exception the catch landing pad stashed
				 * (mono_wasm_jit_get_caught_exc, ()->i32). dreg is a ref -> wasm_st routes it to the
				 * GC ref shadow stack. Release the dispatch-owned strong handle only AFTER that store,
				 * closing the raw-pointer GC window between native dispatch and handler entry. */
				WasmFuncType _gt; int _gti = -1, _gk;
				memset (&_gt, 0, sizeof _gt); _gt.nparams = 0; _gt.ret = WASM_I32;
				for (_gk = 0; _gk < nextra; ++_gk) if (functype_eq (&extra_types [_gk], &_gt)) { _gti = ti_base + _gk; break; }
				if (_gti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = _gt; _gti = ti_base + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_get_caught_exc, _gti);
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff4, (guint32) _gti);
#endif
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "get_ex_obj dreg"; goto done; }
#ifdef HOST_BROWSER
				wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_release_caught_exc, eh_endcatch_ti);
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff6, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
				break;
			}
			case OP_MEMORY_BARRIER:
#ifndef DISABLE_THREADS
				/* Only a SEQ barrier gets a fence, as mini-amd64.c does.
				 *
				 * This used to fence unconditionally ("match the jiterpreter"), but
				 * MINT_MONO_MEMORY_BARRIER is the lowering of the SEQ case only; copying it for ACQ/REL
				 * was the mistake. V8 lowers atomic.fence to a real `mfence` on x64, and it cost 4.24%
				 * of total profiled time on jbox2d -- the largest single unclassified instruction in the
				 * profile, ahead of `ret`, and 54% of the stelemref wrapper by itself. EVERY implicit
				 * barrier the front end emits is ACQ or REL (reference stores under !weak_memory_model:
				 * memory-access.c:522, method-to-ir.c:2373 and friends); SEQ comes only from explicit
				 * Interlocked / Thread.MemoryBarrier / Volatile intrinsics.
				 *
				 * Why dropping ACQ/REL is sound, in order of how load-bearing it is:
				 *
				 * 1. Every managed atomic this emitter lowers (the byte store, R323b; the 16/32-bit loads,
				 *    stores, CAS, exchange and add, R360) is a seq-cst wasm atomic that orders itself
				 *    (point 2), so the ACQ/REL barrier mono attaches to one adds nothing. Any other
				 *    OP_MEMORY_BARRIER orders ORDINARY, non-atomic accesses. Per the wasm relaxed-memory
				 *    model that is a data race with or without the fence: atomic.fence does not make an
				 *    ordinary store a synchronising one, so it never conferred the ordering it looked like
				 *    it did.
				 * 2. Where synchronisation does go through wasm atomics, all wasm atomics are currently
				 *    seq-cst, so the ordering is carried by the atomic access itself -- same-thread
				 *    accesses are happens-before ordered and an atomic read-from establishes cross-thread
				 *    happens-before. A separate ACQ/REL fence cannot strengthen that. Binaryen encodes
				 *    exactly this: visitAtomicFence lowers atomic.fence to nothing while only seq-cst
				 *    atomics exist.
				 * 3. The AOT'd half never had ANY fence: Binaryen's --precompute (emcc -O2 link) deletes every
				 *    atomic.fence, for reason (2). For ACQ/REL that is harmless on x86. For SEQ it was a bug:
				 *    IKVM's Thread.MemoryBarrier around each Java volatile access vanished from the AOT'd class
				 *    library and x86 reordered AQS's store-then-load (the lost-unpark wedge, R340/R343). The
				 *    AOT now calls mono_wasm_seq_fence, an atomic exchange wasm-opt cannot delete.
				 *
				 * SEQ keeps a real fence here deliberately: Java volatile (IKVM emits Thread.MemoryBarrier)
				 * needs StoreLoad, and so do .NET patterns that expect a physical full barrier. These
				 * modules never pass through wasm-opt, so this atomic.fence survives to V8's mfence. */
				if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ) {
					wasm_u8 (&body, 0xfe);  /* atomic prefix */
					wasm_u8 (&body, 0x03);  /* atomic.fence */
					wasm_u8 (&body, 0x00);  /* memory order: sequentially consistent */
				}
#endif
				break;
			case OP_AOTCONST: {
				/* On wasm (need_got_var=0, compile_aot=0) NEW_AOTCONST lowers to OP_PCONST, so the
				 * GC-safepoint poll-flag address arrives as a normal constant we already handle (and
				 * OP_GC_SAFE_POINT re-bakes it anyway). A genuine OP_AOTCONST here comes from
				 * NEW_AOTCONST_TOKEN (ldstr/ldtoken/type/field/method tokens) and its result vreg IS
				 * consumed, so we must NOT silently skip it (that leaves the dreg undefined). We don't
				 * resolve+bake token constants yet — and MONO_PATCH_INFO_LDSTR resolves to a movable GC
				 * object that can't be a wasm immediate — so bail the method to the interpreter. The
				 * GC_SAFE_POINT_FLAG form (shouldn't reach here on wasm) is dead, so tolerate it. */
				switch ((MonoJumpInfoType) (gsize) ins->inst_p1) {
				case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
					break;   /* dead on wasm; OP_GC_SAFE_POINT re-bakes the flag address */
				/* !compile_aot: NEW_*CONST stored the RESOLVED runtime pointer in inst_p0 (ir-emit.h:
				 * `compile_aot ? klass : vtable`). vtables/classes/methods/static-storage/images are
				 * stable, un-movable, cross-thread -> bake the pointer like a resolved pconst. (Movable
				 * LDSTR objects come via OP_(I|P)CONST + the interned slot, never here.) */
				case MONO_PATCH_INFO_VTABLE: case MONO_PATCH_INFO_CLASS: case MONO_PATCH_INFO_METHOD:
				case MONO_PATCH_INFO_METHODCONST: case MONO_PATCH_INFO_FIELD: case MONO_PATCH_INFO_SFLDA:
				case MONO_PATCH_INFO_IMAGE: case MONO_PATCH_INFO_METHOD_RGCTX:
					/* Baking inst_p0 enables newobj/token-constant methods to JIT. Only provably-stable,
					 * un-movable, cross-thread pointers reach here (vtable/class/method/static-field-addr/
					 * image/method-rgctx); movable GC objects (ldstr/typeof) go through the precise-root
					 * literal table instead, so a GC can't dangle these immediates. */
					wasm_i32_const (&body, (gint32) (intptr_t) ins->inst_p0);
					if (!wasm_st (&body, &lc, ins->dreg)) { fail = "aotconst dreg"; goto done; }
					break;
				default:
					fail = "aotconst type"; fail_op = ins->opcode; goto done;
				}
				break;
			}
			case OP_WASM_JIT_ALLOC_FAST: {
#ifdef HOST_BROWSER
				/* Inline TLAB bump (handle_alloc in method-to-ir.c, R305) -- the managed allocator wrapper's fast
				 * path without its call:
				 *     a = *(s.b + WJ_SCRATCH_TLAB_SLOT)   address of this thread's mono_wasm_sgen_tls_info
				 *     a = a ? *a : 0                      the current SgenThreadInfo, NULL while detached
				 *     p = a->tlab_next;  n = p + size
				 *     if (a && n < a->tlab_temp_end) { a->tlab_next = n; p->vtable = vt; result = p } else result = 0
				 * and the caller's IR calls ves_icall_object_new_specific on 0. Needs no zeroing: sgen hands out
				 * TLAB memory already cleared (mono_gc_wasm_inline_alloc_size refuses any other clear policy).
				 * Not a GC point and makes no call, so under coop suspend the sequence cannot be interrupted by a
				 * collection. No release fence after the vtable store: this emitter drops ACQ/REL barriers for the
				 * reason given at OP_MEMORY_BARRIER, and the AOT'd managed allocator reaches wasm without one too. */
				static int tlab_next_off = -1, tlab_temp_end_off = -1;
				int a = alloc_tmp_idx, p = alloc_tmp_idx + 1, n = alloc_tmp_idx + 2;
				if (G_UNLIKELY (tlab_next_off < 0)) {
					extern void mono_gc_wasm_tlab_offsets (int *next_off, int *temp_end_off);
					int no, eo;
					mono_gc_wasm_tlab_offsets (&no, &eo);
					tlab_temp_end_off = eo;
					mono_memory_barrier ();
					tlab_next_off = no;
				}
				if (!alloc_tmp_idx) { fail = "alloc_fast without temps"; goto done; }
				wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);                  /* s.b */
				wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, WJ_SCRATCH_TLAB_SLOT);
				wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) a);
				WJ_HINT (1);   /* R339: attached */
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x7f);                           /* (result i32) */
				  wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) a);
				  wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, 0);
				  wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) a);                     /* a = SgenThreadInfo */
				  WJ_HINT (1);
				  wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x7f);
				    wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) a);
				    wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, (guint32) tlab_next_off);
				    wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) p);
				    wasm_i32_const (&body, (gint32) ins->inst_c1);
				    wasm_op (&body, WASM_OP_I32_ADD);
				    wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) n);
				    wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) a);
				    wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, (guint32) tlab_temp_end_off);
				    wasm_op (&body, WASM_OP_I32_LT_U);
				    WJ_HINT (1);   /* R339: the TLAB has room */
				    wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x7f);
				      wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) a);
				      wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) n);
				      wasm_op (&body, WASM_OP_I32_STORE); wasm_uleb (&body, 2); wasm_uleb (&body, (guint32) tlab_next_off);
				      wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) p);
				      wasm_i32_const (&body, (gint32) (intptr_t) ins->inst_p0);
				      wasm_op (&body, WASM_OP_I32_STORE); wasm_uleb (&body, 2); wasm_uleb (&body, 0);   /* p->vtable */
				      wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) p);
				    wasm_op (&body, WASM_OP_ELSE);
				      wasm_i32_const (&body, 0);
				    wasm_op (&body, WASM_OP_END);
				  wasm_op (&body, WASM_OP_ELSE);
				    wasm_i32_const (&body, 0);
				  wasm_op (&body, WASM_OP_END);
				wasm_op (&body, WASM_OP_ELSE);
				  wasm_i32_const (&body, 0);
				wasm_op (&body, WASM_OP_END);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "alloc_fast dreg"; goto done; }
#else
				fail = "alloc_fast offline"; goto done;
#endif
				break;
			}
			case OP_GC_SAFE_POINT: {
				/* Cooperative-GC and code-publication safepoint, in ONE load.
				 *
				 * REQUIRED for multithreaded GC -- without it a JITted loop never reaches a safepoint, the
				 * GC cannot suspend the thread, and the coop suspend deadlocks. It is ALSO where a worker
				 * adopts a code publication, which is what makes a live f-slot re-pointable at all.
				 *
				 * `s.g` is this worker's action word (mini-wasm-publish.inc): one i32 that either writer
				 * -- the GC suspend initiator or a publisher -- can raise, and that only this worker
				 * clears. An imported global rather than a baked address because the emitted BYTES are
				 * process-wide and shared by every worker, so nothing per-thread may be baked into them.
				 * This costs exactly what HEAD's bare `mono_polling_required` load cost; testing the two
				 * conditions separately here cost two extra loads, an i32.ne and an i32.or on every loop
				 * back-edge in the tier. */
#ifdef HOST_BROWSER
				{
					WasmFuncType pt; int pti = -1, pk;
					memset (&pt, 0, sizeof (pt)); pt.ret = WASM_VOID; pt.nparams = 0;
					for (pk = 0; pk < nextra; ++pk) if (functype_eq (&extra_types [pk], &pt)) { pti = ti_base + pk; break; }
					if (pti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt; pti = ti_base + nextra++; }
					uses_calls = TRUE;
					{ extern int mono_wasm_jit_t2, mono_wasm_jit_t2_count, mono_wasm_jit_s2;
					  /* plan2x S0: count this tier-1 body's entries; at the threshold, request tier 2. A process-wide
					   * word (the bytes stay identical on every worker); racing increments lose counts, which a
					   * threshold tolerates. Emitted before the action-word check, at the entry poll only. Under S2
					   * the word is the body's S2 entry record, so one increment serves both. */
					  gboolean _t2c = mono_wasm_jit_t2_count > 0 && mono_wasm_jit_t2 && cfg->wasm_jit_caller_imethod;
					  gboolean _s2e = (mono_wasm_jit_s2 & 1) != 0;
					  gint32 *_cw = NULL;
					  if (bb == cfg->bb_entry && cfg->wasm_jit_tier < 2 && (_t2c || _s2e)) {
						if (_s2e)
							_cw = wj_s2_entry_word (cfg);
						if (!_cw && _t2c)
							_cw = wj_word_alloc ();
					  }
					  if (_cw && !_t2c) {
						guint32 _ca = (guint32) (gsize) _cw;
						wasm_i32_const (&body, 0);
						wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _ca);
						wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _ca);
					  } else if (_cw) {
						extern void mono_wasm_jit_t2_count_hit (gpointer imethod);
						guint32 _ca = (guint32) (gsize) _cw;
						WasmFuncType pc; int pci = -1, pkc;
						memset (&pc, 0, sizeof (pc)); pc.ret = WASM_VOID; pc.nparams = 1; pc.params [0] = WASM_I32;
						for (pkc = 0; pkc < nextra; ++pkc) if (functype_eq (&extra_types [pkc], &pc)) { pci = ti_base + pkc; break; }
						if (pci < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pc; pci = ti_base + nextra++; }
						wasm_i32_const (&body, 0);
						wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _ca);
						wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _ca);
						wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _ca);
						{   /* every 2^ceil(log2 T2_COUNT) entries, not once at exactly T2_COUNT: a request released on a
						     * BUSY give-up must be able to fire again (b4 gate, 2026-10-01: the hottest tier-1 root was lost
						     * that way). mono_wasm_jit_t2_count_hit ignores a method already requested or done. */
							guint32 _per = 1;
							while (_per < (guint32) mono_wasm_jit_t2_count && _per < 0x40000000u)
								_per <<= 1;
							wasm_i32_const (&body, (gint32) (_per - 1));
							wasm_op (&body, WASM_OP_I32_AND);
							wasm_op (&body, WASM_OP_I32_EQZ);
						}
						WJ_HINT (0);
						wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
						ENSURE_REF_FRAME ();
						wasm_i32_const (&body, (gint32) (intptr_t) cfg->wasm_jit_caller_imethod);
						wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_t2_count_hit, (guint32) pci);
						RELEASE_LAZY_REF_FRAME ();
						wasm_op (&body, WASM_OP_END);
						wj_count (WJC_T2C_SITES);
					  } }
					{ extern int mono_wasm_jit_t2, mono_wasm_jit_t2_loop_count;
					  /* R409: count this tier-1 body's LOOP polls (one word per body, shared by its loops); every
					   * 2^ceil(log2 T2_LOOP_COUNT) back-edges, request tier 2 exactly as S0's entry counter does. The sampler
					   * credits the poll that a timer tick lands on, and a loop that spends its time in poll-free callees is
					   * undersampled ~30x (R334: selectTicks) -- this does not sample. */
					  if (mono_wasm_jit_t2_loop_count > 0 && mono_wasm_jit_t2 && bb != cfg->bb_entry && cfg->wasm_jit_tier < 2 &&
					      cfg->wasm_jit_caller_imethod &&
					      (cfg->wasm_t2_loop_word || (cfg->wasm_t2_loop_word = wj_word_alloc ()))) {
						extern void mono_wasm_jit_t2_count_hit (gpointer imethod);
						guint32 _la, _per = 1;
						WasmFuncType plc; int plci = -1, pklc;
						_la = (guint32) (gsize) cfg->wasm_t2_loop_word;
						memset (&plc, 0, sizeof (plc)); plc.ret = WASM_VOID; plc.nparams = 1; plc.params [0] = WASM_I32;
						for (pklc = 0; pklc < nextra; ++pklc) if (functype_eq (&extra_types [pklc], &plc)) { plci = ti_base + pklc; break; }
						if (plci < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = plc; plci = ti_base + nextra++; }
						while (_per < (guint32) mono_wasm_jit_t2_loop_count && _per < 0x40000000u)
							_per <<= 1;
						wasm_i32_const (&body, 0);
						wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _la);
						wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _la);
						wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _la);
						wasm_i32_const (&body, (gint32) (_per - 1));
						wasm_op (&body, WASM_OP_I32_AND);
						wasm_op (&body, WASM_OP_I32_EQZ);
						WJ_HINT (0);
						wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
						ENSURE_REF_FRAME ();
						wasm_i32_const (&body, (gint32) (intptr_t) cfg->wasm_jit_caller_imethod);
						wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_t2_count_hit, (guint32) plci);
						RELEASE_LAZY_REF_FRAME ();
						wasm_op (&body, WASM_OP_END);
						wj_count (WJC_T2_LOOP_SITES);
					  } }
					wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 9); /* s.g = this worker's action word */
					{
						extern int mono_wasm_jit_t2, mono_wasm_jit_t2_sample_loop;
						/* R355: under T2_SAMPLE_LOOP=2 an ENTRY poll reads only the low byte, so the loop-owed
						 * sample bit (WJ_ACT_LOOP_SAMPLE, 0x100) never takes it into the helper. Same cost. */
						if (mono_wasm_jit_t2 && mono_wasm_jit_t2_sample_loop == 2 && bb == cfg->bb_entry) {
							wasm_op (&body, WASM_OP_I32_LOAD8_U); wasm_memarg (&body, 0, 0);
						} else {
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
						}
					}
					WJ_HINT (0);   /* R339: the action word is almost always 0 */
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					/* Materialize roots inside the rare taken arm, not before the flag load. */
					ENSURE_REF_FRAME ();
					/* Direct import: a C function address in dotnet.native.wasm, fixed for the process. The ENTRY
					 * poll gets its own helper while edge sampling is on (R295), so a sample is a method entry. */
					{
						extern int mono_wasm_jit_edge_sample, mono_wasm_jit_t2, mono_wasm_jit_t2_sample_loop;
						extern void mono_wasm_jit_safepoint_poll_entry (void);
						extern void mono_wasm_jit_safepoint_poll_t2 (gpointer imethod);
						/* (R316e: tier-2 bodies too -- they only feed the per-method heat the t2 dump ranks by.)
						 * R335: under T2_SAMPLE_LOOP the ENTRY poll does not take a sample -- the time since the tick
						 * was spent in the caller -- so it falls to the plain poll below, which defers the sample
						 * to the next loop back-edge. */
						if (mono_wasm_jit_t2 && cfg->wasm_jit_caller_imethod &&
						    !(mono_wasm_jit_t2_sample_loop == 1 && bb == cfg->bb_entry)) {   /* mode 2 samples at entry too (R355) */
							/* R316 tier-2 hotness: every poll of a tier-1 body names its method, INSIDE this
							 * rare arm -- the common path is unchanged. The InterpMethod* is process-wide. */
							WasmFuncType pt1; int pt1i = -1, pk1;
							memset (&pt1, 0, sizeof (pt1)); pt1.ret = WASM_VOID; pt1.nparams = 1; pt1.params [0] = WASM_I32;
							for (pk1 = 0; pk1 < nextra; ++pk1) if (functype_eq (&extra_types [pk1], &pt1)) { pt1i = ti_base + pk1; break; }
							if (pt1i < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pt1; pt1i = ti_base + nextra++; }
							/* R336: bit 0 marks a TIER-2 body's poll (InterpMethod* is aligned), so the sampler can tell
							 * whether the tier-2 generation is the one executing. */
							/* R355: bit 1 marks a LOOP poll, under T2_SAMPLE_LOOP=2 only (so other modes emit the same bytes) */
							wasm_i32_const (&body, (gint32) ((intptr_t) cfg->wasm_jit_caller_imethod | (cfg->wasm_jit_tier >= 2 ? 1 : 0) |
								(mono_wasm_jit_t2_sample_loop == 2 && bb != cfg->bb_entry ? 2 : 0)));
							wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_safepoint_poll_t2, (guint32) pt1i);
						} else if (mono_wasm_jit_edge_sample && bb == cfg->bb_entry)
							wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_safepoint_poll_entry, (guint32) pti);
						else
							wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_safepoint_poll, (guint32) pti);
					}
					RELEASE_LAZY_REF_FRAME ();
					wasm_op (&body, WASM_OP_END);
				}
#endif
				break;
			}
			case OP_ICONST:
				/* OP_PCONST aliases OP_ICONST on wasm32. A non-null STACK_OBJ constant is a baked managed-
				 * object pointer (ldstr/typeof/folded GetType) — a MOVABLE GC object that must NOT be a fixed
				 * wasm immediate (it dangles after a compacting GC -> NPE / null-function). method-to-ir
				 * already interned it into the GC-tracked literal table at RESOLVE time (a fresh, un-movable-
				 * yet pointer) and stashed the stable slot address in inst_p1; here we bake that address and
				 * i32.load the CURRENT object pointer at runtime (the precise root keeps the slot up to date
				 * across moves). The isref pass marked this dreg, so wasm_st stores the loaded value to the
				 * ref shadow stack -> it also survives the next GC point. inst_p1==0 => not interned (table
				 * full, or a managed-pconst source method-to-ir didn't convert): bail rather than bake a
				 * movable pointer. inst_c0/inst_p0 alias; a null STACK_OBJ and every non-object iconst fall
				 * through to the plain immediate bake. */
				if (ins->type == STACK_OBJ && ins->inst_p0) {
#ifdef HOST_BROWSER
					if (!ins->inst_p1) { fail = "managed pconst not interned"; goto done; }
					wasm_i32_const (&body, (gint32) (intptr_t) ins->inst_p1);
#else
					wasm_i32_const (&body, 0x7ff8); /* offline dump: placeholder slot addr for encoder validation */
#endif
					wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
				} else {
					wasm_i32_const (&body, (gint32) ins->inst_c0);
				}
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "iconst"; goto done; }
				break;
			case OP_I8CONST:
				wasm_i64_const (&body, (gint64) ins->inst_l);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "i8const"; goto done; }
				break;
			case OP_R8CONST:
				wasm_f64_const (&body, *(double *) ins->inst_p0);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "r8const"; goto done; }
				break;
			case OP_MOVE: case OP_LMOVE: case OP_FMOVE: case OP_RMOVE:
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_st (&body, &lc, ins->dreg)) {
					wj_move_fail_diag (cfg, &lc, ins, mname);
					fail = "move"; goto done;
				}
				break;
			case OP_LDADDR: {
				/* &local: push the local's addressable-frame slot address (addrbase + offset), store to dreg.
				 * The addr pre-pass assigned addrslot for every supported address-taken local; if it didn't
				 * (ref/byref/vtype/arg, or LDADDR disabled), bail the whole method as before. */
				MonoInst *var = (MonoInst *) ins->inst_p0;
				int vv = (var && var->dreg >= 0 && var->dreg < nvreg) ? var->dreg : -1;
				if (vv >= 0 && lc.addrslot && lc.addrslot [vv] == -3) {
					/* by-addr vtype ARG: the incoming i32 param already holds the address of the
					 * caller-owned copy — re-push it (wasm_ld takes the li/param path: addrslot<0,
					 * never a refslot since the pointer is non-heap). */
					if (!wasm_ld (&body, &lc, vv) || !wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr byaddr arg"; fail_op = OP_LDADDR; goto done; }
					break;
				}
				if (vv >= 0 && lc.addrslot && lc.addrslot [vv] == -2) {
					/* ref-etype scalar vtype: address is its GC-scanned ref-shadow slot (refbase + slot*4). */
					if (!lc.refslot || lc.refslot [vv] < 0) { fail = "ldaddr refvt no slot"; fail_op = OP_LDADDR; goto done; }
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
					if (lc.refslot [vv] != 0) { wasm_i32_const (&body, lc.refslot [vv] * 4); wasm_op (&body, WASM_OP_I32_ADD); }
					if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
					NN_SET (ins->dreg);   /* a frame-slot address is never null */
					break;
				}
				if (vv < 0 || !lc.addrslot || lc.addrslot [vv] < 0) { fail = "ldaddr unsupported var"; fail_op = OP_LDADDR; goto done; }
				wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) addrbase_idx);
				if (lc.addrslot [vv] != 0) { wasm_i32_const (&body, lc.addrslot [vv]); wasm_op (&body, WASM_OP_I32_ADD); }
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldaddr dreg"; goto done; }
				NN_SET (ins->dreg);   /* a frame-slot address is never null */
				break;
			}
			case OP_IADD: BIN (WASM_OP_I32_ADD); break;
			case OP_ISUB: BIN (WASM_OP_I32_SUB); break;
			case OP_IMUL: BIN (WASM_OP_I32_MUL); break;
			case OP_IDIV: BIN (WASM_OP_I32_DIV_S); break;
			case OP_IDIV_UN: BIN (WASM_OP_I32_DIV_U); break;
			case OP_IREM: BIN (WASM_OP_I32_REM_S); break;
			case OP_IREM_UN: BIN (WASM_OP_I32_REM_U); break;
			case OP_IAND: BIN (WASM_OP_I32_AND); break;
			case OP_IOR: BIN (WASM_OP_I32_OR); break;
			case OP_IXOR: BIN (WASM_OP_I32_XOR); break;
			case OP_ISHL: BIN (WASM_OP_I32_SHL); break;
			case OP_ISHR: BIN (WASM_OP_I32_SHR_S); break;
			case OP_ISHR_UN: BIN (WASM_OP_I32_SHR_U); break;
			case OP_IADD_IMM: BINI32 (WASM_OP_I32_ADD); break;
			case OP_ISUB_IMM: BINI32 (WASM_OP_I32_SUB); break;
			case OP_IMUL_IMM: BINI32 (WASM_OP_I32_MUL); break;
			case OP_IAND_IMM: BINI32 (WASM_OP_I32_AND); break;
			case OP_IOR_IMM: BINI32 (WASM_OP_I32_OR); break;
			case OP_IXOR_IMM: BINI32 (WASM_OP_I32_XOR); break;
			case OP_ISHL_IMM: BINI32 (WASM_OP_I32_SHL); break;
			case OP_ISHR_IMM: BINI32 (WASM_OP_I32_SHR_S); break;
			case OP_ISHR_UN_IMM: BINI32 (WASM_OP_I32_SHR_U); break;
			/* div/rem by a constant (imm != 0, and != -1 for signed -> no INT_MIN/-1 overflow trap) */
			case OP_IDIV_IMM: BINI32 (WASM_OP_I32_DIV_S); break;
			case OP_IDIV_UN_IMM: BINI32 (WASM_OP_I32_DIV_U); break;
			case OP_IREM_IMM: BINI32 (WASM_OP_I32_REM_S); break;
			case OP_IREM_UN_IMM: BINI32 (WASM_OP_I32_REM_U); break;
			/* native-int (size_t) IMM ops — i32 on wasm32, same lowering as the OP_I*_IMM variants */
			case OP_ADD_IMM: BINI32 (WASM_OP_I32_ADD); break;
			case OP_SUB_IMM: BINI32 (WASM_OP_I32_SUB); break;
			case OP_MUL_IMM: BINI32 (WASM_OP_I32_MUL); break;
			case OP_OR_IMM:  BINI32 (WASM_OP_I32_OR); break;
			case OP_XOR_IMM: BINI32 (WASM_OP_I32_XOR); break;
			case OP_SHL_IMM: BINI32 (WASM_OP_I32_SHL); break;
			case OP_SHR_IMM: BINI32 (WASM_OP_I32_SHR_S); break;
			case OP_SHR_UN_IMM: BINI32 (WASM_OP_I32_SHR_U); break;
			case OP_INEG: /* no i32.neg in wasm: 0 - x */
				wasm_i32_const (&body, 0);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ineg sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_SUB);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ineg dreg"; goto done; }
				break;
			case OP_INOT: /* no i32.not in wasm: x ^ -1 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "inot sreg"; goto done; }
				wasm_i32_const (&body, -1);
				wasm_op (&body, WASM_OP_I32_XOR);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "inot dreg"; goto done; }
				break;
			case OP_LNOT: /* no i64.not in wasm: x ^ -1 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "lnot sreg"; goto done; }
				wasm_i64_const (&body, (gint64) -1);
				wasm_op (&body, WASM_OP_I64_XOR);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lnot dreg"; goto done; }
				break;
			case OP_LNEG: /* no i64.neg in wasm: 0 - x */
				wasm_i64_const (&body, (gint64) 0);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "lneg sreg"; goto done; }
				wasm_op (&body, WASM_OP_I64_SUB);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lneg dreg"; goto done; }
				break;
			case OP_LADD: BIN (WASM_OP_I64_ADD); break;
			case OP_LSUB: BIN (WASM_OP_I64_SUB); break;
			case OP_LMUL: BIN (WASM_OP_I64_MUL); break;
			/* i64 div/rem. wasm i64.div_s/rem_s TRAP on divide-by-zero and (signed) INT64_MIN/-1 overflow.
			 * Unlike the i32 OP_IDIV family — whose divide-by-zero/overflow OP_COND_EXC checks mono's
			 * decompose emits ahead of the op — the LONG div decompose block is #if TARGET_SIZEOF_VOID_P==8,
			 * so on wasm32 (ptr=4) it is SKIPPED and OP_LDIV reaches us unchecked. Emit the checks inline here
			 * (mirroring decompose.c) so a long divide raises DivideByZeroException / OverflowException
			 * instead of trapping. IMM variants below stay unchecked: decompose only forms them for a known
			 * non-zero (and, signed, non -1) constant divisor. */
			case OP_LDIV: case OP_LDIV_UN: case OP_LREM: case OP_LREM_UN: {
				gboolean lsigned = (ins->opcode == OP_LDIV || ins->opcode == OP_LREM);
				WasmOpcode dop = ins->opcode == OP_LDIV ? WASM_OP_I64_DIV_S
					: ins->opcode == OP_LDIV_UN ? WASM_OP_I64_DIV_U
					: ins->opcode == OP_LREM ? WASM_OP_I64_REM_S : WASM_OP_I64_REM_U;
				WasmFuncType rt; int ldiv_rti = -1, rk;
				/* (i32)->void functype for mono_wasm_jit_raise_corlib */
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_I32; rt.nparams = 1; rt.ret = WASM_VOID;
				for (rk = 0; rk < nextra; ++rk) if (functype_eq (&extra_types [rk], &rt)) { ldiv_rti = ti_base + rk; break; }
				if (ldiv_rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; ldiv_rti = ti_base + nextra++; }
				uses_calls = TRUE;
				/* divide-by-zero: if (sreg2 == 0) raise DivideByZeroException (exc_id 1) */
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv b"; goto done; }
				wasm_op (&body, WASM_OP_I64_EQZ);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
				LDIV_RAISE (1);
				wasm_op (&body, WASM_OP_END);
				if (lsigned) {
					/* overflow: if (sreg1 == INT64_MIN && sreg2 == -1) raise OverflowException (exc_id 0) */
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ldiv a"; goto done; }
					wasm_i64_const (&body, (gint64) G_MININT64);
					wasm_op (&body, WASM_OP_I64_EQ);
					if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv b2"; goto done; }
					wasm_i64_const (&body, (gint64) -1);
					wasm_op (&body, WASM_OP_I64_EQ);
					wasm_op (&body, WASM_OP_I32_AND);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					LDIV_RAISE (0);
					wasm_op (&body, WASM_OP_END);
				}
				/* the checked division */
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "ldiv sreg"; goto done; }
				wasm_op (&body, dop);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ldiv dreg"; goto done; }
				break;
			}
			case OP_LAND: BIN (WASM_OP_I64_AND); break;
			case OP_LOR: BIN (WASM_OP_I64_OR); break;
			case OP_LXOR: BIN (WASM_OP_I64_XOR); break;
			case OP_LSHL: LSHIFT (WASM_OP_I64_SHL); break;
			case OP_LSHR: LSHIFT (WASM_OP_I64_SHR_S); break;
			case OP_LSHR_UN: LSHIFT (WASM_OP_I64_SHR_U); break;
			case OP_LADD_IMM: BINI64L (WASM_OP_I64_ADD); break;
			case OP_LSUB_IMM: BINI64L (WASM_OP_I64_SUB); break;
			case OP_LMUL_IMM: BINI64L (WASM_OP_I64_MUL); break;
			/* div/rem by a constant long. cprop's strength-reduction normally eliminates a 0 or (signed) -1
			 * constant divisor before the backend, but guard anyway: wasm i64.div_s/rem_s TRAP on a /0 and on
			 * INT64_MIN / -1 — which would crash the worker instead of raising DivideByZero/Overflow. Bail
			 * those (rare) shapes to the interpreter, which raises the correct managed exception. */
			case OP_LDIV_IMM: case OP_LDIV_UN_IMM: case OP_LREM_IMM: case OP_LREM_UN_IMM: {
				gboolean lsigned = (ins->opcode == OP_LDIV_IMM || ins->opcode == OP_LREM_IMM);
				WasmOpcode dop = ins->opcode == OP_LDIV_IMM ? WASM_OP_I64_DIV_S
					: ins->opcode == OP_LDIV_UN_IMM ? WASM_OP_I64_DIV_U
					: ins->opcode == OP_LREM_IMM ? WASM_OP_I64_REM_S : WASM_OP_I64_REM_U;
				if (ins->inst_l == 0 || (lsigned && ins->inst_l == -1)) { fail = "ldiv_imm 0/-1 divisor"; fail_op = ins->opcode; goto done; }
				BINI64L (dop);
				break;
			}
			case OP_LAND_IMM: BINI64L (WASM_OP_I64_AND); break;
			case OP_LOR_IMM:  BINI64L (WASM_OP_I64_OR); break;
			case OP_LXOR_IMM: BINI64L (WASM_OP_I64_XOR); break;
			case OP_LSHL_IMM: BINI64 (WASM_OP_I64_SHL); break;
			case OP_LSHR_IMM: BINI64 (WASM_OP_I64_SHR_S); break;
			case OP_LSHR_UN_IMM: BINI64 (WASM_OP_I64_SHR_U); break;
			case OP_FADD: BIN (WASM_OP_F64_ADD); break;
			case OP_FSUB: BIN (WASM_OP_F64_SUB); break;
			case OP_FMUL: BIN (WASM_OP_F64_MUL); break;
			case OP_FDIV: BIN (WASM_OP_F64_DIV); break;
			case OP_FNEG: UN (WASM_OP_F64_NEG); break;
			case OP_RADD: BIN (WASM_OP_F32_ADD); break;   /* native-f32 (r4fp) float arithmetic */
			case OP_RSUB: BIN (WASM_OP_F32_SUB); break;
			case OP_RMUL: BIN (WASM_OP_F32_MUL); break;
			case OP_RDIV: BIN (WASM_OP_F32_DIV); break;
			case OP_RNEG: UN (WASM_OP_F32_NEG); break;
			/* float32 % (Java's fmod: the dividend's sign, |r| < |y|), reaching here only under
			 * MONO_WASM_JIT_RREM (mono_local_emulate_ops otherwise turns it into an fmodf call). In double
			 * precision, with a = |x|, b = |y| and a < b * 2^29: n = trunc (a / b) is exact or one too large,
			 * n * b is exact (<= 29 + 24 significant bits), and so is a - n * b (a multiple of the finer
			 * operand's ulp, below 2b); one conditional + b corrects n, and copysign restores the dividend's
			 * sign, including -0. Off that range -- b zero, infinite or NaN, a infinite or NaN, or a >= b *
			 * 2^29 -- fmodf, as before. Exhaustive check against fmodf: enctest/rrem (R432). */
			case OP_RREM: {
				WasmFuncType rt; int rrem_rti = -1, rk;
				gboolean x64 = ins->sreg1 >= 0 && ins->sreg1 < nvreg && vt [ins->sreg1] == WASM_F64;
				gboolean y64 = ins->sreg2 >= 0 && ins->sreg2 < nvreg && vt [ins->sreg2] == WASM_F64;
				guint32 ta = (guint32) rrem_tmp, tb = (guint32) rrem_tmp + 1;
				if (rrem_tmp < 0) { fail = "rrem scratch"; goto done; }
				memset (&rt, 0, sizeof (rt)); rt.params [0] = WASM_F32; rt.params [1] = WASM_F32; rt.nparams = 2; rt.ret = WASM_F32;
				for (rk = 0; rk < nextra; ++rk) if (functype_eq (&extra_types [rk], &rt)) { rrem_rti = ti_base + rk; break; }
				if (rrem_rti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = rt; rrem_rti = ti_base + nextra++; }
				uses_calls = TRUE;
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rrem x"; goto done; }
				if (!x64) wasm_op (&body, WASM_OP_F64_PROMOTE_F32);
				wasm_op (&body, WASM_OP_F64_ABS);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, ta);
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "rrem y"; goto done; }
				if (!y64) wasm_op (&body, WASM_OP_F64_PROMOTE_F32);
				wasm_op (&body, WASM_OP_F64_ABS);
				wasm_op_local (&body, WASM_OP_LOCAL_SET, tb);
				/* a < b * 2^29 (false for a NaN, an infinite a, b == 0) && b <= FLT_MAX (false for inf, NaN) */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, ta);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, tb);
				wasm_f64_const (&body, 536870912.0);
				wasm_op (&body, WASM_OP_F64_MUL);
				wasm_op (&body, WASM_OP_F64_LT);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, tb);
				wasm_f64_const (&body, 3.4028234663852886e38);
				wasm_op (&body, WASM_OP_F64_LE);
				wasm_op (&body, WASM_OP_I32_AND);
				wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, WASM_F32);
				/* r = a - trunc (a / b) * b, then r + (r < 0 ? b : 0) */
				wasm_op_local (&body, WASM_OP_LOCAL_GET, ta);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, ta);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, tb);
				wasm_op (&body, WASM_OP_F64_DIV);
				wasm_op (&body, WASM_OP_F64_TRUNC);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, tb);
				wasm_op (&body, WASM_OP_F64_MUL);
				wasm_op (&body, WASM_OP_F64_SUB);
				wasm_op_local (&body, WASM_OP_LOCAL_TEE, ta);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, tb);
				wasm_f64_const (&body, 0.0);
				wasm_op_local (&body, WASM_OP_LOCAL_GET, ta);
				wasm_f64_const (&body, 0.0);
				wasm_op (&body, WASM_OP_F64_LT);
				wasm_op (&body, WASM_OP_SELECT);
				wasm_op (&body, WASM_OP_F64_ADD);
				wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rrem x"; goto done; }
				if (x64) wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				wasm_op (&body, WASM_OP_F32_COPYSIGN);
				wasm_op (&body, WASM_OP_ELSE);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rrem x"; goto done; }
				if (x64) wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "rrem y"; goto done; }
				if (y64) wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				wj_emit_helper_call (&body, (gpointer) fmodf, (guint32) rrem_rti);
				wasm_op (&body, WASM_OP_END);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "rrem dreg"; goto done; }
				break;
			}
			case OP_ICONV_TO_R4: UN (WASM_OP_F32_CONVERT_I32_S); break;  /* i32 -> f32 */
			case OP_RCONV_TO_R4: /* R4->R4 identity (r4fp f32); demote if the source vreg is f64 */
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rconv_to_r4 sreg"; goto done; }
				if (ins->sreg1 >= 0 && ins->sreg1 < nvreg && vt [ins->sreg1] == WASM_F64)
					wasm_op (&body, WASM_OP_F32_DEMOTE_F64);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "rconv_to_r4 dreg"; goto done; }
				break;
			case OP_ICONV_TO_R8:
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "conv sreg"; goto done; }
				wasm_op (&body, WASM_OP_F64_CONVERT_I32_S);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "conv dreg"; goto done; }
				break;
			case OP_RCONV_TO_R8: UN (WASM_OP_F64_PROMOTE_F32); break;     /* f32 -> f64 */
			case OP_FCONV_TO_R4: UN (WASM_OP_F32_DEMOTE_F64); break;      /* f64 -> f32 */
			case OP_ICONV_TO_I8: UN (WASM_OP_I64_EXTEND_I32_S); break;    /* i32 -> i64 (sign) */
			case OP_ICONV_TO_U8: UN (WASM_OP_I64_EXTEND_I32_U); break;    /* i32 -> i64 (zero) */
			case OP_ICONV_TO_I1: UN (WASM_OP_I32_EXTEND8_S); break;       /* (sbyte) */
			case OP_ICONV_TO_I2: UN (WASM_OP_I32_EXTEND16_S); break;      /* (short) */
			case OP_ICONV_TO_U1: MASK (0xFF); break;                     /* (byte) */
			case OP_ICONV_TO_U2: MASK (0xFFFF); break;                   /* (ushort) */
			case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:
			case OP_LCONV_TO_I: case OP_LCONV_TO_U: UN (WASM_OP_I32_WRAP_I64); break;  /* i64 -> i32/native-int (truncate) */
			case OP_LCONV_TO_OVF_I4: case OP_LCONV_TO_OVF_I4_UN: case OP_LCONV_TO_OVF_I: case OP_LCONV_TO_OVF_I_UN: {
				/* R323: checked i64 -> i32. Signed: out of range iff x != sext (wrap (x)); unsigned (_UN, the source
				 * read as u64): out of range iff x >u 0x7fffffff -- decompose.c's two comparisons, one each. */
				gboolean _un = ins->opcode == OP_LCONV_TO_OVF_I4_UN || ins->opcode == OP_LCONV_TO_OVF_I_UN;
				if (!WJ_HAS_THROW (0)) { fail = "no shared throw block for ovf conv"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ovf conv sreg"; goto done; }
				if (_un) {
					wasm_i64_const (&body, 0x7fffffff);
					wasm_op (&body, WASM_OP_I64_GT_U);
				} else {
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ovf conv sreg"; goto done; }
					wasm_op (&body, WASM_OP_I32_WRAP_I64);
					wasm_op (&body, WASM_OP_I64_EXTEND_I32_S);
					wasm_op (&body, WASM_OP_I64_NE);
				}
				WJ_THROW_BR (0, 0);
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "ovf conv sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_WRAP_I64);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "ovf conv dreg"; goto done; }
				wj_count (WJC_OVF_CONV_LOWERED);
				break;
			}
			case OP_LCONV_TO_R8: UN (WASM_OP_F64_CONVERT_I64_S); break;  /* i64 -> f64 */
			case OP_LCONV_TO_R4: UN (WASM_OP_F32_CONVERT_I64_S); break;  /* i64 -> f32 */

			/*
			 * FLOAT -> INT. Previously ENTIRELY unsupported: the emitter had every int->float and
			 * float->float conversion but no float->int at all, so any `(int)someFloat` bailed the whole
			 * method. That shape is pervasive in jbox2d (MathUtils.floor/round) and it was the root bail
			 * that cascaded into ~29 lost methods once the inline limit was raised past 20.
			 *
			 * Always the SATURATING (0xFC-prefixed) forms. The plain truncations TRAP on NaN or
			 * out-of-range, which would turn an ECMA-"unspecified" conversion into a hard abort. Saturating
			 * clamps to min/max with NaN->0 -- precisely Java's (int)float rule, and the workload is
			 * IKVM-translated Java. It also matches the interpreter, whose MINT_CONV_I4_R4 is a plain C
			 * `(gint32) float` cast that clang lowers to trunc_sat on wasm -- BUT ONLY WITH
			 * -mnontrapping-fptoint (mono.proj, R305). Without it emcc emits a range check plus INT_MIN, and
			 * until R305 the interpreter and this tier disagreed on NaN/out-of-range, so a method could
			 * change behaviour when it tiered from interp to JIT.
			 *
			 * Source precision comes from the SOURCE VREG's valtype, not the opcode's R/F prefix: with
			 * MONO_OPT_FLOAT32 off an R4 value lives in an f64 vreg, and assuming f32 would emit a
			 * type-invalid module -- the same reason OP_RCONV_TO_R4 above checks vt[] before demoting.
			 */
			#define WJ_SRC_IS_F32 (!(ins->sreg1 >= 0 && ins->sreg1 < nvreg && vt [ins->sreg1] == WASM_F64))
			#define SAT_CONV(SAT32, SAT64, NARROW) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "fconv sreg"; goto done; } \
				wasm_op_sat (&body, _f32 ? (SAT32) : (SAT64)); \
				NARROW; \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fconv dreg"; goto done; } \
			} while (0)
			#define SAT_NONE   do { } while (0)
			#define SAT_EXT8   wasm_op (&body, WASM_OP_I32_EXTEND8_S)
			#define SAT_EXT16  wasm_op (&body, WASM_OP_I32_EXTEND16_S)
			#define SAT_MASK8  do { wasm_i32_const (&body, 0xFF); wasm_op (&body, WASM_OP_I32_AND); } while (0)
			#define SAT_MASK16 do { wasm_i32_const (&body, 0xFFFF); wasm_op (&body, WASM_OP_I32_AND); } while (0)
			/* -> i32 (native int is i32 on wasm32, so _TO_I folds in here) */
			case OP_RCONV_TO_I4: case OP_FCONV_TO_I4:
			case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_NONE); break;
			case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_U, WASM_SAT_I32_TRUNC_F64_U, SAT_NONE); break;
			/* -> i64 */
			case OP_RCONV_TO_I8: case OP_FCONV_TO_I8:
				SAT_CONV (WASM_SAT_I64_TRUNC_F32_S, WASM_SAT_I64_TRUNC_F64_S, SAT_NONE); break;
			case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:
				SAT_CONV (WASM_SAT_I64_TRUNC_F32_U, WASM_SAT_I64_TRUNC_F64_U, SAT_NONE); break;
			/* -> sub-word: truncate to i32 SIGNED then narrow, mirroring mono's native lowering
			 * (cvttsd2si + movsx/movzx) rather than converting straight to the narrow type. */
			case OP_RCONV_TO_I1: case OP_FCONV_TO_I1:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_EXT8); break;
			case OP_RCONV_TO_I2: case OP_FCONV_TO_I2:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_EXT16); break;
			case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_MASK8); break;
			case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
				SAT_CONV (WASM_SAT_I32_TRUNC_F32_S, WASM_SAT_I32_TRUNC_F64_S, SAT_MASK16); break;
			/* NB: the OP_*CONV_TO_OVF_* family is deliberately NOT here. Those must raise
			 * OverflowException on out-of-range, which needs a range test plus a raise -- saturating
			 * them would silently return a wrong value instead of throwing. They keep bailing. */

			/*
			 * Float math intrinsics. Each is ONE wasm instruction and every one was previously an
			 * unsupported-opcode bail. wasm's semantics match the managed ones: min/max propagate NaN and
			 * order -0 below +0, and f*.nearest is round-half-to-EVEN, which is Math.Round(double)'s
			 * default. Precision from the source vreg, as above.
			 */
			#define FUN1(W32, W64) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "fmath sreg"; goto done; } \
				wasm_op (&body, _f32 ? (W32) : (W64)); \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fmath dreg"; goto done; } \
			} while (0)
			#define FBIN(W32, W64) do { \
				gboolean _f32 = WJ_SRC_IS_F32; \
				if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_ld (&body, &lc, ins->sreg2)) { fail = "fmath sreg"; goto done; } \
				wasm_op (&body, _f32 ? (W32) : (W64)); \
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fmath dreg"; goto done; } \
			} while (0)
			case OP_ABS: case OP_ABSF:     FUN1 (WASM_OP_F32_ABS,     WASM_OP_F64_ABS);     break;
			case OP_SQRT: case OP_SQRTF:   FUN1 (WASM_OP_F32_SQRT,    WASM_OP_F64_SQRT);    break;
			case OP_CEIL: case OP_CEILF:   FUN1 (WASM_OP_F32_CEIL,    WASM_OP_F64_CEIL);    break;
			case OP_FLOOR: case OP_FLOORF: FUN1 (WASM_OP_F32_FLOOR,   WASM_OP_F64_FLOOR);   break;
			case OP_TRUNC: case OP_TRUNCF: FUN1 (WASM_OP_F32_TRUNC,   WASM_OP_F64_TRUNC);   break;
			case OP_ROUND:                 FUN1 (WASM_OP_F32_NEAREST, WASM_OP_F64_NEAREST); break;
			case OP_FMIN: case OP_RMIN:    FBIN (WASM_OP_F32_MIN, WASM_OP_F64_MIN); break;
			case OP_FMAX: case OP_RMAX:    FBIN (WASM_OP_F32_MAX, WASM_OP_F64_MAX); break;
			case OP_FCOPYSIGN: case OP_RCOPYSIGN:
				FBIN (WASM_OP_F32_COPYSIGN, WASM_OP_F64_COPYSIGN); break;

			/* Integer bit intrinsics (BitOperations.LeadingZeroCount / PopCount). The 64-bit forms are
			 * LREG->LREG in mono and i64.clz/i64.popcnt yield i64, so no wrap is needed. */
			case OP_LZCNT32:  UN (WASM_OP_I32_CLZ);    break;
			case OP_LZCNT64:  UN (WASM_OP_I64_CLZ);    break;
			case OP_POPCNT32: UN (WASM_OP_I32_POPCNT); break;
			case OP_POPCNT64: UN (WASM_OP_I64_POPCNT); break;
			/* bit-reinterpret (Unsafe.BitCast intrinsic, what IKVM's Float/Double intBitsToFloat etc. lower
			 * to via intrinsics.c): no value change, just reinterpret the bits */
			case OP_MOVE_I4_TO_F: UN (WASM_OP_F32_REINTERPRET_I32); break;  /* i32 bits -> f32 */
			case OP_MOVE_F_TO_I4: UN (WASM_OP_I32_REINTERPRET_F32); break;  /* f32 bits -> i32 */
			case OP_MOVE_I8_TO_F: UN (WASM_OP_F64_REINTERPRET_I64); break;  /* i64 bits -> f64 */
			case OP_MOVE_F_TO_I8: UN (WASM_OP_I64_REINTERPRET_F64); break;  /* f64 bits -> i64 */
			case OP_R4CONST:
				wasm_f32_const (&body, *(float *) ins->inst_p0);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "r4const"; goto done; }
				break;
			case OP_AND_IMM: BINI32 (WASM_OP_I32_AND); break;
			case OP_NOT_NULL:
				/* Metadata only, NOT a check. The front end emits this to ASSERT that sreg1 is already
				 * known non-null -- right after an allocation, or right after the check it just emitted.
				 * mini-ops.h declares it (NONE, IREG, NONE), so it has no result, and it is a no-op in
				 * every other backend: amd64/x86/arm/arm64/ppc/s390x/riscv all `break`, and mini-llvm.c
				 * groups it with OP_NOP/OP_LIVERANGE_START.
				 *
				 * Sharing OP_CHECK_THIS's case below inverted its meaning -- it made the one opcode that
				 * certifies a check is unnecessary emit a load+eqz+branch. That was live cost, not a
				 * hypothetical: two sites emit OP_NOT_NULL UNGATED by COMPILE_LLVM, after handle_alloc
				 * for newobj (method-to-ir.c:9649) and after ldlen (method-to-ir.c:10867), so every
				 * `new` and every `arraylength` in JIT'd code carried a null check on a value the front
				 * end had just certified non-null. jbox2d is dense in both.
				 *
				 * Record the fact so NCE can drop LATER real checks on the same vreg, and emit nothing. */
				NN_SET (ins->sreg1);
				break;
			case OP_CHECK_THIS: {
				/* Null check: if sreg1 is null, raise a CATCHABLE NullReferenceException, mirroring OP_COND_EXC
				 * — NOT a raw wasm trap. address 0 is valid in wasm linear memory, so a skipped check silently
				 * corrupts low memory; and EH methods now compile, so a null this/deref inside
				 * a try must reach managed EH/finally rather than `unreachable`. Hot path = ld + eqz + not-taken
				 * branch; cold path raises NRE (exc_id 4) then C++-unwinds (cppeh) or bails to interp resume-state. */
				if (NN_GET (ins->sreg1)) break;   /* NCE: already proven non-null earlier in this bb */
				NN_SET (ins->sreg1);              /* past this point it cannot be null */
				if (!WJ_HAS_THROW (4)) { fail = "no shared throw block for NRE"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "nullchk sreg"; goto done; }
				wasm_op (&body, WASM_OP_I32_EQZ);
				WJ_THROW_BR (4, 0);   /* one shared raise site per method, not a call per check */
				break;
			}
/* membase load: dreg = *(sreg1 + inst_offset).
 *
 * Under LCSE, if this exact (base, offset, opcode) was already loaded earlier in the extended block and
 * nothing has clobbered memory since, replace the memory load with a LOCAL COPY of the vreg that already
 * holds the value. The i32.load/f32.load is gone, which is the point; TurboFan then SSA-renames the copy
 * away for free.
 *
 * It is tempting to emit NOTHING and just record dreg as an alias of the cached vreg, resolving every
 * later read through the table. That MISCOMPILES, and the failure is not subtle once seen:
 *
 *     t7  = load(a,12)     -> entry (a,12) -> t7
 *     t15 = load(a,12)     -> hit; emit nothing, alias t15 -> t7
 *     t7  = <redefined>    -> the alias must die (t7 no longer holds the value)
 *     use t15              -> reads t15's own local, WHICH WAS NEVER WRITTEN -> zero
 *
 * jbox2d's min/max ternaries reuse vregs constantly, so this produced garbage floats, NaNs, and a
 * non-terminating solver loop rather than a crash. Emitting the copy makes dreg a real def with its
 * normal wasm_st behaviour -- which also keeps the ref frame honest, since a slotted ref dreg still
 * writes its pin slot exactly where SLOTLIVE/SLOTZERO's liveness walk expects it.
 *
 * The ALIAS entry is still recorded, but purely to canonicalise TABLE KEYS: it lets a chained load
 * (`a.lowerBound.x`, whose base is this load's dreg) find the original base's entry. If it dies, the
 * only cost is a missed elision. */
/* An UNPINNED REFERENCE is never cached or reused. lcse_nopin[v] marks an isref vreg whose frame slot
 * MONO_WASM_JIT_SLOTLIVE elided because its IR def->use range crossed no GC point. LCSE's reuse read is
 * created at emission and is invisible to that analysis, so it can extend the range past a GC point --
 * and the two passes disagree about which instructions those are (LCSE deliberately treats a raise as
 * non-clobbering, since null checks are the commonest thing between a load and its reload, while the
 * frame model counts a raise as a GC point unless RAISE_NOGC is set on a clause-free method). Refusing
 * to cache unpinned refs removes the disagreement outright and costs almost nothing: the loads this
 * pass exists for are the float and int field reads in jbox2d's inner loops, none of which are refs. */
#define WJ_LCSE_UNPINNED(v)  (lcse_nopin && (v) >= 0 && (v) < nvreg && lcse_nopin [(v)])
#define LOADM(WOP, AL) do { \
		int _cb = G_UNLIKELY (lcse != NULL) ? wj_lcse_canon (lcse, ins->sreg1) : -1; \
		int _hit = (_cb >= 0) ? wj_lcse_find (lcse, (WOP), (AL), _cb, (gint32) ins->inst_offset) : -1; \
		if (G_UNLIKELY (lcse != NULL)) wj_count (WJC_LCSE_LOADS_SEEN); \
		if (_hit >= 0 && _hit < nvreg && ins->dreg >= 0 && ins->dreg < nvreg && _hit != ins->dreg && \
		    !WJ_LCSE_UNPINNED (_hit) && \
		    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [_hit] != -1))) { \
			if (!wasm_ld (&body, &lc, _hit)) { fail = "lcse src"; goto done; } \
			if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lcse dreg"; goto done; } \
			wj_lcse_add_alias (lcse, ins->dreg, _hit); \
			lcse_hits++; wj_count (WJC_LCSE_HITS); \
			break; \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "load addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "load base"; goto done; } \
		wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); \
		if (!wasm_st (&body, &lc, ins->dreg)) { fail = "load dreg"; goto done; } \
		if (_cb >= 0 && ins->dreg >= 0 && ins->dreg < nvreg && !WJ_LCSE_UNPINNED (ins->dreg) && \
		    !(lc.addrslot && (lc.addrslot [ins->dreg] != -1 || lc.addrslot [_cb] != -1))) { \
			if (lcse->nld >= WJ_LCSE_LOADS) wj_count (WJC_LCSE_EVICT); \
			wj_lcse_add_load (lcse, (WOP), (AL), _cb, (gint32) ins->inst_offset, ins->dreg); \
			lcse_adds++; wj_count (WJC_LCSE_ADDS); \
		} \
	} while (0)
			case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE: LOADM (WASM_OP_I32_LOAD, 2); break;
			case OP_LOADU1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_U, 0); break;
			case OP_LOADI1_MEMBASE: LOADM (WASM_OP_I32_LOAD8_S, 0); break;
			case OP_LOADU2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_U, 1); break;
			case OP_LOADI2_MEMBASE: LOADM (WASM_OP_I32_LOAD16_S, 1); break;
			case OP_LOADI8_MEMBASE: LOADM (WASM_OP_I64_LOAD, 3); break;
			case OP_LOADR8_MEMBASE: LOADM (WASM_OP_F64_LOAD, 3); break;
			case OP_LOADR4_MEMBASE: LOADM (WASM_OP_F32_LOAD, 2); break;
#undef LOADM
#undef WJ_LCSE_UNPINNED
/* membase store: *(dreg[=inst_destbasereg] + inst_offset) = sreg1. (A reference value still needs its
 * GC write barrier, which mono emits as a separate IR call before the store — so the store is raw.) */
#define STOREM(WOP, AL) STOREM_PFX (0, WOP, AL)
#define STOREM_PFX(PFX, WOP, AL) do { \
		gboolean _og_valref = lc.refslot && ins->sreg1 >= 0 && ins->sreg1 < nvreg && lc.refslot [ins->sreg1] >= 0; \
		gboolean _og_baseref = lc.refslot && ins->dreg >= 0 && ins->dreg < nvreg && lc.refslot [ins->dreg] >= 0; \
		if (G_UNLIKELY (lc.objguard) && (_og_valref || _og_baseref)) { \
			/* OBJGUARD. Two store shapes can scribble random memory via a bad base: \
			 * (a) ref VALUE into a genuine OBJECT base (dreg is vreg_is_ref and NOT an interior mp): the base is \
			 *     a live heap object whose write barrier card-marks (base>>9)+cardtable; its offset-0 word IS a \
			 *     vtable, so validate it looks like a live heap object (kind 2). \
			 * (b) BYREF / interior-pointer base (dreg is vreg_is_mp, a byref arg, an ldflda/ldaddr temp, or any \
			 *     vreg not proven to be an object): its offset-0 word is a struct FIELD, not a vtable. A ref value \
			 *     is routinely stored THROUGH such a base into a value type — e.g. a struct-returning method writing \
			 *     ref fields into its hidden vret buffer (ReadOnlySequence<T>.SliceImpl storing SequencePosition. \
			 *     _object into *retbuf), or `*outparam = x`. kind 2 would false-trap on a stale/small first word \
			 *     (seen live: obj first word 0x1e0), so just range-check the base (kind 3). \
			 * The kind is chosen by the BASE's object-ness, NOT by whether the VALUE is a ref: refslot/isref merge \
			 * object refs and byrefs into one bucket, so consult vreg_is_ref/vreg_is_mp to separate them here. */ \
			gboolean _og_baseobj = ins->dreg >= 0 && ins->dreg < nvreg && vreg_is_ref (cfg, ins->dreg) && !vreg_is_mp (cfg, ins->dreg); \
			int _ogkind = (_og_valref && _og_baseobj) ? 2 : 3; \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard base"; goto done; } \
			wasm_i32_const (&body, _ogkind); \
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, (guint32) lc.check_ti); \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->dreg, (gint32) ins->inst_offset)) { fail = "store addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store base"; goto done; } if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "store val"; goto done; } \
		if (PFX) wasm_u8 (&body, (PFX)); \
		wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
			case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: STOREM (WASM_OP_I32_STORE, 2); break;
			case OP_STOREI1_MEMBASE_REG: STOREM (WASM_OP_I32_STORE8, 0); break;
			case OP_STOREI2_MEMBASE_REG: STOREM (WASM_OP_I32_STORE16, 1); break;
			case OP_STOREI8_MEMBASE_REG: STOREM (WASM_OP_I64_STORE, 3); break;
			case OP_STORER4_MEMBASE_REG: STOREM (WASM_OP_F32_STORE, 2); break;
			case OP_STORER8_MEMBASE_REG: STOREM (WASM_OP_F64_STORE, 3); break;
			/* R323b: Volatile.Write of a byte. seq-cst, so it carries its own release ordering (the barrier kind
			 * mono attaches needs no separate fence -- see OP_MEMORY_BARRIER); a byte is never misaligned, which
			 * is why only this width is lowered (a misaligned atomic traps). */
			case OP_ATOMIC_STORE_I1: case OP_ATOMIC_STORE_U1:
				STOREM_PFX (WASM_OP_ATOMIC_PREFIX, 0x19 /* i32.atomic.store8 */, 0);
				wj_count (WJC_ATOMIC_STORE8_LOWERED);
				break;
			/* R360: the wider atomics -- Volatile.Read/Write, Interlocked.CompareExchange/Exchange/Add/Increment on
			 * 8/16/32-bit and pointer-sized (so reference) locations; p81's deeper tier-2 inlining lost 19 bodies
			 * to CAS_I4/EXCHANGE_I4/LOAD_U4 reaching the default case (R359). Every one is a seq-cst wasm atomic, so like
			 * the byte store it orders itself (see OP_MEMORY_BARRIER). A wasm atomic TRAPS on a misaligned address
			 * where x86 would not; that is not a new failure mode, because the interpreter's and the AOT'd
			 * Interlocked/Volatile are the same wasm atomics (C11 atomics under emscripten) on the same address.
			 * The memarg alignment must be EXACTLY the natural one for an atomic, or the module fails validation.
			 * The 64-bit variants are produced only under MONO_WASM_JIT_ATOMIC_I8 (J1c, below). A reference-sized CAS/exchange/load result is a reference (alloc_ireg_ref); wasm_st
			 * routes it like any ref def, and its write barrier is separate IR. */
			case OP_ATOMIC_STORE_I2: case OP_ATOMIC_STORE_U2:
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				STOREM_PFX (WASM_OP_ATOMIC_PREFIX, 0x1a /* i32.atomic.store16 */, 1);
				wj_count (WJC_ATOMIC_LOWERED);
				break;
			case OP_ATOMIC_STORE_I4: case OP_ATOMIC_STORE_U4:
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				STOREM_PFX (WASM_OP_ATOMIC_PREFIX, 0x17 /* i32.atomic.store */, 2);
				wj_count (WJC_ATOMIC_LOWERED);
				break;
			/* J1c (MONO_WASM_JIT_ATOMIC_I8, intrinsics.c produces these only under that knob): the 64-bit forms. wasm32 has
			 * native i64 atomics and this backend keeps longs whole (COMPILE_METHODIR), so a long vreg is one i64 local.
			 * Alignment is the same argument as R360's: the icall these replace does the same native atomic on the same
			 * address (C11 atomics, ATOMIC_LLONG_LOCK_FREE), so a misaligned long traps there too. */
			case OP_ATOMIC_STORE_I8: case OP_ATOMIC_STORE_U8:
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				STOREM_PFX (WASM_OP_ATOMIC_PREFIX, 0x18 /* i64.atomic.store */, 3);
				wj_count (WJC_ATOMIC_I8_LOWERED);
				break;
			case OP_ATOMIC_STORE_R4: case OP_ATOMIC_STORE_R8: {
				/* no float atomics in wasm: store the bits */
				gboolean r8 = ins->opcode == OP_ATOMIC_STORE_R8;
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				if (!wasm_guard_memaddr (&body, &lc, ins->dreg, (gint32) ins->inst_offset)) { fail = "store addr guard"; goto done; }
				if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store base"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "store val"; goto done; }
				wasm_op (&body, r8 ? WASM_OP_I64_REINTERPRET_F64 : WASM_OP_I32_REINTERPRET_F32);
				wasm_u8 (&body, WASM_OP_ATOMIC_PREFIX);
				wasm_u8 (&body, r8 ? 0x18 /* i64.atomic.store */ : 0x17 /* i32.atomic.store */);
				wasm_memarg (&body, r8 ? 3 : 2, (guint32) ins->inst_offset);
				wj_count (WJC_ATOMIC_LOWERED);
				break;
			}
			case OP_ATOMIC_LOAD_I1: case OP_ATOMIC_LOAD_U1: case OP_ATOMIC_LOAD_I2: case OP_ATOMIC_LOAD_U2:
			case OP_ATOMIC_LOAD_I4: case OP_ATOMIC_LOAD_U4: case OP_ATOMIC_LOAD_R4: case OP_ATOMIC_LOAD_R8:
			case OP_ATOMIC_LOAD_I8: case OP_ATOMIC_LOAD_U8: {
				/* Never an LCSE hit or entry: a volatile read must re-read, and wj_ins_clobbers_mem clears the table
				 * here (an acquire must not be satisfied from a load cached before it). */
				int sub, al, post = -1;
				switch (ins->opcode) {
				case OP_ATOMIC_LOAD_I1: sub = 0x12; al = 0; post = WASM_OP_I32_EXTEND8_S; break;   /* i32.atomic.load8_u */
				case OP_ATOMIC_LOAD_U1: sub = 0x12; al = 0; break;
				case OP_ATOMIC_LOAD_I2: sub = 0x13; al = 1; post = WASM_OP_I32_EXTEND16_S; break;  /* i32.atomic.load16_u */
				case OP_ATOMIC_LOAD_U2: sub = 0x13; al = 1; break;
				case OP_ATOMIC_LOAD_R4: sub = 0x10; al = 2; post = WASM_OP_F32_REINTERPRET_I32; break;
				case OP_ATOMIC_LOAD_R8: sub = 0x11; al = 3; post = WASM_OP_F64_REINTERPRET_I64; break;   /* i64.atomic.load */
				case OP_ATOMIC_LOAD_I8: case OP_ATOMIC_LOAD_U8: sub = 0x11; al = 3; break;   /* J1c */
				default:                sub = 0x10; al = 2; break;   /* i32.atomic.load */
				}
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "load addr guard"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "load base"; goto done; }
				wasm_u8 (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, (guint8) sub);
				wasm_memarg (&body, (guint32) al, (guint32) ins->inst_offset);
				if (post >= 0) wasm_op (&body, post);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "load dreg"; goto done; }
				wj_count ((ins->opcode == OP_ATOMIC_LOAD_I8 || ins->opcode == OP_ATOMIC_LOAD_U8) ? WJC_ATOMIC_I8_LOWERED : WJC_ATOMIC_LOWERED);
				break;
			}
			case OP_ATOMIC_ADD_I4: case OP_ATOMIC_ADD_I8:
			case OP_ATOMIC_EXCHANGE_U1: case OP_ATOMIC_EXCHANGE_U2: case OP_ATOMIC_EXCHANGE_I4: case OP_ATOMIC_EXCHANGE_I8: {
				/* rmw returns the OLD value; mono's OP_ATOMIC_ADD returns the NEW one (mini-x86.c: lock xadd, then
				 * add), so ADD re-adds sreg2. The sub-word exchanges zero-extend, which is what intrinsics.c's
				 * trailing ICONV_TO_{I,U}{1,2} expects. */
				int sub, al;
				switch (ins->opcode) {
				case OP_ATOMIC_ADD_I4:      sub = 0x1e; al = 2; break;   /* i32.atomic.rmw.add */
				case OP_ATOMIC_EXCHANGE_U1: sub = 0x43; al = 0; break;   /* i32.atomic.rmw8.xchg_u */
				case OP_ATOMIC_EXCHANGE_U2: sub = 0x44; al = 1; break;   /* i32.atomic.rmw16.xchg_u */
				case OP_ATOMIC_ADD_I8:      sub = 0x1f; al = 3; break;   /* i64.atomic.rmw.add (J1c) */
				case OP_ATOMIC_EXCHANGE_I8: sub = 0x42; al = 3; break;   /* i64.atomic.rmw.xchg (J1c) */
				default:                    sub = 0x41; al = 2; break;   /* i32.atomic.rmw.xchg */
				}
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "rmw addr guard"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "rmw base"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "rmw val"; goto done; }
				wasm_u8 (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, (guint8) sub);
				wasm_memarg (&body, (guint32) al, (guint32) ins->inst_offset);
				if (ins->opcode == OP_ATOMIC_ADD_I4 || ins->opcode == OP_ATOMIC_ADD_I8) {
					if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "rmw val"; goto done; }
					wasm_op (&body, ins->opcode == OP_ATOMIC_ADD_I8 ? WASM_OP_I64_ADD : WASM_OP_I32_ADD);
				}
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "rmw dreg"; goto done; }
				wj_count (al == 3 ? WJC_ATOMIC_I8_LOWERED : WJC_ATOMIC_LOWERED);
				break;
			}
			case OP_ATOMIC_CAS_U1: case OP_ATOMIC_CAS_U2: case OP_ATOMIC_CAS_I4: case OP_ATOMIC_CAS_I8: {
				/* dreg = cmpxchg (*(sreg1 + off), expected = sreg3, replacement = sreg2) -> the old value. The
				 * sub-word forms wrap the expected value to their width before comparing and zero-extend the
				 * result, matching intrinsics.c's zero-extended comparand. */
				int sub = ins->opcode == OP_ATOMIC_CAS_U1 ? 0x4a : ins->opcode == OP_ATOMIC_CAS_U2 ? 0x4b : ins->opcode == OP_ATOMIC_CAS_I8 ? 0x49 /* i64.atomic.rmw.cmpxchg (J1c) */ : 0x48;
				int al = ins->opcode == OP_ATOMIC_CAS_U1 ? 0 : ins->opcode == OP_ATOMIC_CAS_U2 ? 1 : ins->opcode == OP_ATOMIC_CAS_I8 ? 3 : 2;
				if (ins->inst_offset < 0) { fail = "atomic offset"; goto done; }
				if (!wasm_guard_memaddr (&body, &lc, ins->sreg1, (gint32) ins->inst_offset)) { fail = "cas addr guard"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "cas base"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg3)) { fail = "cas cmp"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg2)) { fail = "cas val"; goto done; }
				wasm_u8 (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, (guint8) sub);
				wasm_memarg (&body, (guint32) al, (guint32) ins->inst_offset);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "cas dreg"; goto done; }
				wj_count (al == 3 ? WJC_ATOMIC_I8_LOWERED : WJC_ATOMIC_LOWERED);
				break;
			}
#undef STOREM
#undef STOREM_PFX
/* membase store of an immediate: *(dreg[=inst_destbasereg] + inst_offset) = inst_imm. dreg aliases
 * inst_destbasereg (the base); inst_imm is the constant. Sub-word stores truncate via i32.store8/16. */
#define STOREMI(WOP, AL) do { \
		if (G_UNLIKELY (lc.objguard) && lc.refslot && ins->dreg >= 0 && ins->dreg < nvreg && lc.refslot [ins->dreg] >= 0) { \
			/* OBJGUARD: storing an immediate THROUGH a ref/byref base (e.g. `*outparam = 0`); a stale/wild \
			 * byref can land on the C-stack pointer. Range/control-var check the base (kind 3) before the store. */ \
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind); \
			if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "objguard imm base"; goto done; } \
			wasm_i32_const (&body, 3); \
			wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, (guint32) lc.check_ti); \
		} \
		if (!wasm_guard_memaddr (&body, &lc, ins->dreg, (gint32) ins->inst_offset)) { fail = "store-imm addr guard"; goto done; } \
		if (!wasm_ld (&body, &lc, ins->dreg)) { fail = "store-imm base"; goto done; } wasm_i32_const (&body, (gint32) ins->inst_imm); wasm_op (&body, (WOP)); wasm_memarg (&body, (AL), (guint32) ins->inst_offset); } while (0)
			case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE, 2); break;
			case OP_STOREI1_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE8, 0); break;
			case OP_STOREI2_MEMBASE_IMM: STOREMI (WASM_OP_I32_STORE16, 1); break;
#undef STOREMI
			case OP_SETRET:
				/* The return value lives in cfg->ret->dreg; the epilogue below
				 * leaves it on the stack. setret just ensures it holds sreg1. */
				if (cfg->ret && ins->sreg1 != cfg->ret->dreg) {
					/* The one write in this emitter that is not ins->dreg, so the generic NCE and LCSE
					 * kills at the top of the loop do not cover it. Both have to be told by hand. */
					NN_KILL (cfg->ret->dreg);
					if (G_UNLIKELY (lcse != NULL))
						wj_lcse_kill (lcse, li, nvreg, cfg->ret->dreg);
					if (!wasm_ld (&body, &lc, ins->sreg1) || !wasm_st (&body, &lc, cfg->ret->dreg)) { fail = "setret"; goto done; }
				}
				break;
			case OP_ICOMPARE: case OP_COMPARE: /* COMPARE = pointer/native-int compare, i32 on wasm32 */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0; cmp_i64 = FALSE;
				break;
			case OP_LCOMPARE: /* i64 compare, consumed by a following OP_LB<cc> branch / OP_LC<cc> setcc / OP_COND_EXC */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = 0; cmp_i64 = TRUE;
				break;
			case OP_LCOMPARE_IMM: /* i64 compare vs immediate (a long compare folded against a constant by cprop);
			                       * consumed by a following OP_LB<cc> / OP_LC<cc> / OP_COND_EXC. The 64-bit immediate
			                       * lives in inst_l (data.i8const), NOT inst_imm: on wasm32 cprop stores it via
			                       * ins->inst_l (local-propagation.c), and inst_imm (= data.op[1].const_val) overlaps
			                       * only the HIGH 32 bits of i8const — reading it yields the wrong value (0 for any
			                       * immediate that fits in 32 bits). */
				cmp_a = ins->sreg1; cmp_imm64 = (gint64) ins->inst_l; cmp_imm_mode = TRUE; cmp_float = 0; cmp_i64 = TRUE;
				break;
			case OP_LBEQ: case OP_LBNE_UN: case OP_LBLT: case OP_LBLT_UN:
			case OP_LBGT: case OP_LBGT_UN: case OP_LBLE: case OP_LBLE_UN:
			case OP_LBGE: case OP_LBGE_UN:
				/* i64 conditional branch consuming the preceding OP_LCOMPARE (mirrors OP_IB<cc> but i64 ops) */
				if (cmp_a < 0 || !cmp_i64) { fail = "long branch without lcompare"; goto done; }
				switch (ins->opcode) {
				case OP_LBEQ: cmp_wop = WASM_OP_I64_EQ; break;
				case OP_LBNE_UN: cmp_wop = WASM_OP_I64_NE; break;
				case OP_LBLT: cmp_wop = WASM_OP_I64_LT_S; break;
				case OP_LBLT_UN: cmp_wop = WASM_OP_I64_LT_U; break;
				case OP_LBGT: cmp_wop = WASM_OP_I64_GT_S; break;
				case OP_LBGT_UN: cmp_wop = WASM_OP_I64_GT_U; break;
				case OP_LBLE: cmp_wop = WASM_OP_I64_LE_S; break;
				case OP_LBLE_UN: cmp_wop = WASM_OP_I64_LE_U; break;
				case OP_LBGE: cmp_wop = WASM_OP_I64_GE_S; break;
				default: cmp_wop = WASM_OP_I64_GE_U; break; /* OP_LBGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "lcmp a"; goto done; }
				if (cmp_imm_mode) wasm_i64_const (&body, cmp_imm64);   /* OP_LCOMPARE_IMM: imm in inst_l, no cmp_b */
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lcmp b"; goto done; }
				wasm_op (&body, cmp_wop);   /* i64 compare -> i32 (0/1) */
				COND_BRANCH ();
				terminated = TRUE;
				break;
			case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN: {
				/* i64 setcc: dreg = (cmp_a <cc> cmp_b) ? 1 : 0, consuming the preceding OP_LCOMPARE */
				WasmOpcode w;
				if (cmp_a < 0 || !cmp_i64) { fail = "long setcc without lcompare"; goto done; }
				switch (ins->opcode) {
				case OP_LCEQ:    w = WASM_OP_I64_EQ; break;
				case OP_LCGT:    w = WASM_OP_I64_GT_S; break;
				case OP_LCGT_UN: w = WASM_OP_I64_GT_U; break;
				case OP_LCLT:    w = WASM_OP_I64_LT_S; break;
				default:         w = WASM_OP_I64_LT_U; break; /* OP_LCLT_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "lsetcc a"; goto done; }
				if (cmp_imm_mode) wasm_i64_const (&body, cmp_imm64);   /* OP_LCOMPARE_IMM: imm in inst_l, no cmp_b */
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "lsetcc b"; goto done; }
				wasm_op (&body, w);   /* i64 compare -> i32 (0/1) */
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "lsetcc dreg"; goto done; }
				break;
			}
			case OP_ICOMPARE_IMM: case OP_COMPARE_IMM:
				cmp_a = ins->sreg1; cmp_imm = (gint32) ins->inst_imm; cmp_imm_mode = TRUE; cmp_float = 0; cmp_i64 = FALSE;
				break;
			case OP_FCOMPARE: /* f64 compare, consumed by a following OP_FB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F64; cmp_i64 = FALSE;
				break;
			case OP_RCOMPARE: /* f32 (r4) compare, consumed by a following OP_RB<cc> branch */
				cmp_a = ins->sreg1; cmp_b = ins->sreg2; cmp_imm_mode = FALSE; cmp_float = WASM_F32; cmp_i64 = FALSE;
				break;
			case OP_IBEQ: case OP_IBNE_UN: case OP_IBLT: case OP_IBLT_UN:
			case OP_IBGT: case OP_IBGT_UN: case OP_IBLE: case OP_IBLE_UN:
			case OP_IBGE: case OP_IBGE_UN:
				if (cmp_a < 0) { fail = "branch without compare"; goto done; }
				switch (ins->opcode) {
				case OP_IBEQ: cmp_wop = WASM_OP_I32_EQ; break;
				case OP_IBNE_UN: cmp_wop = WASM_OP_I32_NE; break;
				case OP_IBLT: cmp_wop = WASM_OP_I32_LT_S; break;
				case OP_IBLT_UN: cmp_wop = WASM_OP_I32_LT_U; break;
				case OP_IBGT: cmp_wop = WASM_OP_I32_GT_S; break;
				case OP_IBGT_UN: cmp_wop = WASM_OP_I32_GT_U; break;
				case OP_IBLE: cmp_wop = WASM_OP_I32_LE_S; break;
				case OP_IBLE_UN: cmp_wop = WASM_OP_I32_LE_U; break;
				case OP_IBGE: cmp_wop = WASM_OP_I32_GE_S; break;
				default: cmp_wop = WASM_OP_I32_GE_U; break; /* OP_IBGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cmp a"; goto done; }
				if (cmp_imm_mode)
					wasm_i32_const (&body, cmp_imm);
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "cmp b"; goto done; }
				wasm_op (&body, cmp_wop);
				COND_BRANCH ();
				terminated = TRUE;
				break;
			case OP_COND_EXC_EQ: case OP_COND_EXC_NE_UN: case OP_COND_EXC_LT: case OP_COND_EXC_LT_UN:
			case OP_COND_EXC_GT: case OP_COND_EXC_GT_UN: case OP_COND_EXC_LE: case OP_COND_EXC_LE_UN:
			case OP_COND_EXC_GE: case OP_COND_EXC_GE_UN:
			case OP_COND_EXC_IEQ: case OP_COND_EXC_INE_UN: case OP_COND_EXC_ILT: case OP_COND_EXC_ILT_UN:
			case OP_COND_EXC_IGT: case OP_COND_EXC_IGT_UN: case OP_COND_EXC_ILE: case OP_COND_EXC_ILE_UN:
			case OP_COND_EXC_IGE: case OP_COND_EXC_IGE_UN: {
				/* checked-op throw: if the preceding compare's <cc> holds, raise a corlib exception
				 * (OverflowException / IndexOutOfRange / DivideByZero / ...). Hot path (no throw) = the compare
				 * + a not-taken branch; cold path calls mono_wasm_jit_raise_corlib, which C++-throws (cppeh) and
				 * never returns (WASM_OP_UNREACHABLE). Reuses the OP_IBcc compare-fusing (cmp_a/cmp_b/cmp_imm).
				 * Integer compares only. */
				const char *en = (const char *) ins->inst_p1;
				int exc_id = -1;
				if (cmp_a < 0) { fail = "cond_exc without compare"; goto done; }
				if (cmp_float != 0) { fail = "cond_exc float compare"; goto done; }
				switch (ins->opcode) {
				case OP_COND_EXC_EQ: case OP_COND_EXC_IEQ: cmp_wop = WASM_OP_I32_EQ; break;
				case OP_COND_EXC_NE_UN: case OP_COND_EXC_INE_UN: cmp_wop = WASM_OP_I32_NE; break;
				case OP_COND_EXC_LT: case OP_COND_EXC_ILT: cmp_wop = WASM_OP_I32_LT_S; break;
				case OP_COND_EXC_LT_UN: case OP_COND_EXC_ILT_UN: cmp_wop = WASM_OP_I32_LT_U; break;
				case OP_COND_EXC_GT: case OP_COND_EXC_IGT: cmp_wop = WASM_OP_I32_GT_S; break;
				case OP_COND_EXC_GT_UN: case OP_COND_EXC_IGT_UN: cmp_wop = WASM_OP_I32_GT_U; break;
				case OP_COND_EXC_LE: case OP_COND_EXC_ILE: cmp_wop = WASM_OP_I32_LE_S; break;
				case OP_COND_EXC_LE_UN: case OP_COND_EXC_ILE_UN: cmp_wop = WASM_OP_I32_LE_U; break;
				case OP_COND_EXC_GE: case OP_COND_EXC_IGE: cmp_wop = WASM_OP_I32_GE_S; break;
				default: cmp_wop = WASM_OP_I32_GE_U; break; /* GE_UN / IGE_UN */
				}
				if (cmp_i64) {
					/* i64 operands (e.g. the OP_LDIV div-by-zero check OP_LCOMPARE_IMM(sreg2,0) -> COND_EXC_IEQ):
					 * use the i64 compare ops; the result is still i32 (0/1) and feeds the same IF below. */
					switch (ins->opcode) {
					case OP_COND_EXC_EQ: case OP_COND_EXC_IEQ: cmp_wop = WASM_OP_I64_EQ; break;
					case OP_COND_EXC_NE_UN: case OP_COND_EXC_INE_UN: cmp_wop = WASM_OP_I64_NE; break;
					case OP_COND_EXC_LT: case OP_COND_EXC_ILT: cmp_wop = WASM_OP_I64_LT_S; break;
					case OP_COND_EXC_LT_UN: case OP_COND_EXC_ILT_UN: cmp_wop = WASM_OP_I64_LT_U; break;
					case OP_COND_EXC_GT: case OP_COND_EXC_IGT: cmp_wop = WASM_OP_I64_GT_S; break;
					case OP_COND_EXC_GT_UN: case OP_COND_EXC_IGT_UN: cmp_wop = WASM_OP_I64_GT_U; break;
					case OP_COND_EXC_LE: case OP_COND_EXC_ILE: cmp_wop = WASM_OP_I64_LE_S; break;
					case OP_COND_EXC_LE_UN: case OP_COND_EXC_ILE_UN: cmp_wop = WASM_OP_I64_LE_U; break;
					case OP_COND_EXC_GE: case OP_COND_EXC_IGE: cmp_wop = WASM_OP_I64_GE_S; break;
					default: cmp_wop = WASM_OP_I64_GE_U; break; /* GE_UN / IGE_UN */
					}
				}
				exc_id = wj_exc_id_for_name (en);
				if (exc_id < 0) { fail = en ? "cond_exc exc name" : "cond_exc no name"; goto done; }
				/* NCE. Only the explicit-null-check SHAPE qualifies: an NRE raised when the fused compare
				 * is `<vreg> == 0` against a 32-bit immediate zero. That is exactly what
				 * MONO_EMIT_NULL_CHECK expands to (ir-emit.h: OP_COMPARE_IMM imm=0 + OP_COND_EXC_EQ).
				 * Every other cond_exc -- bounds checks, overflow, divide-by-zero -- is left alone, and so
				 * is an NRE raised by any other comparison, because "this vreg is not null" says nothing
				 * about those conditions. */
				if (exc_id == 4 && cmp_imm_mode && !cmp_i64 && cmp_imm == 0 &&
				    (ins->opcode == OP_COND_EXC_EQ || ins->opcode == OP_COND_EXC_IEQ)) {
					if (NN_GET (cmp_a)) break;
					NN_SET (cmp_a);
				}
				if (!WJ_HAS_THROW (exc_id)) { fail = "no shared throw block for exc"; goto done; }
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "cond_exc a"; goto done; }
				if (cmp_imm_mode) { if (cmp_i64) wasm_i64_const (&body, cmp_imm64); else wasm_i32_const (&body, cmp_imm); }
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "cond_exc b"; goto done; }
				wasm_op (&body, cmp_wop);
				/* The compare result IS the "should throw" condition, so it feeds br_if directly: no `if`
				 * block and no per-site call. The hot path is the not-taken branch. */
				WJ_THROW_BR (exc_id, 0);
				break;
			}
			case OP_RETHROW:   /* rethrow: method-to-ir sets sreg1 to the enclosing catch's exvar (the caught
			                    * exception), so re-raise it like throw but via mono_wasm_jit_rethrow, which
			                    * PRESERVES the captured stack trace (a continuation, not a new throw site).
			                    * Only reached under cppeh EH — the EH gate bails clause-bearing methods otherwise. */
			case OP_THROW: {
				/* throw <exc=sreg1>: raise via mono_wasm_jit_throw, which C++-throws (cppeh) and never returns;
				 * the wasm stack unwinds natively to the nearest landing pad (an in-method catch, or the interp
				 * e-thunk boundary). Unconditional bb terminator -> mark terminated. */
				WasmFuncType tt; int tti = -1, tk;
				/* mono_wasm_jit_throw captures a stack trace, i.e. it allocates. The thrown object
				 * itself lives in a wasm local until this point, so a lazy frame has to exist before
				 * the call -- same reason as the shared raise blocks below. (RETHROW implies eh_on,
				 * where lazy frames are off and this expands to nothing.) */
				RAISE_ENSURE_REF_FRAME ();
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "throw sreg"; goto done; }
				memset (&tt, 0, sizeof (tt)); tt.params [0] = WASM_I32; tt.nparams = 1; tt.ret = WASM_VOID;
				for (tk = 0; tk < nextra; ++tk) if (functype_eq (&extra_types [tk], &tt)) { tti = ti_base + tk; break; }
				if (tti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tt; tti = ti_base + nextra++; }
				uses_calls = TRUE;
#ifdef HOST_BROWSER
				/* OP_RETHROW preserves the original stack trace (mono_wasm_jit_rethrow); OP_THROW rebuilds it. */
				if (ins->opcode == OP_RETHROW) {
					extern void mono_wasm_jit_rethrow (MonoObject *exc); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_rethrow, tti);
				} else {
					extern void mono_wasm_jit_throw (MonoObject *exc); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_throw, tti);
				}
#else
				wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff7, (guint32) tti);
#endif
				/* mono_wasm_jit_throw/rethrow C++-throws (mono_llvm_cpp_throw_exception) and never returns —
				 * the wasm stack unwinds natively to the nearest landing pad (an in-method catch or the interp
				 * e-thunk boundary, which restores the ref shadow-stack SP). */
				wasm_op (&body, WASM_OP_UNREACHABLE);
				terminated = TRUE;
				break;
			}
			case OP_FBEQ: case OP_RBEQ: case OP_FBNE_UN: case OP_RBNE_UN:
			case OP_FBLT: case OP_RBLT: case OP_FBLT_UN: case OP_RBLT_UN:
			case OP_FBGT: case OP_RBGT: case OP_FBGT_UN: case OP_RBGT_UN:
			case OP_FBLE: case OP_RBLE: case OP_FBLE_UN: case OP_RBLE_UN:
			case OP_FBGE: case OP_RBGE: case OP_FBGE_UN: case OP_RBGE_UN: {
				/* float conditional branch: consumes the preceding OP_(F|R)COMPARE (width in cmp_float).
				 * wasm has only ordered compares (+ unordered NE), so the *_UN ordered-relation variants
				 * are the complementary ordered op + i32.eqz (e.g. blt_un = !(a>=b), true on NaN). */
				int kind, neg = 0;
				gboolean isf32;
				if (cmp_a < 0 || cmp_float == 0) { fail = "float branch without fcompare"; goto done; }
				isf32 = (cmp_float == WASM_F32);
				switch (ins->opcode) {
				case OP_FBEQ: case OP_RBEQ:       kind = 0; break;
				case OP_FBNE_UN: case OP_RBNE_UN: kind = 1; break;
				case OP_FBLT: case OP_RBLT:       kind = 2; break;
				case OP_FBGT: case OP_RBGT:       kind = 3; break;
				case OP_FBLE: case OP_RBLE:       kind = 4; break;
				case OP_FBGE: case OP_RBGE:       kind = 5; break;
				case OP_FBLT_UN: case OP_RBLT_UN: kind = 5; neg = 1; break; /* !(a>=b) */
				case OP_FBGT_UN: case OP_RBGT_UN: kind = 4; neg = 1; break; /* !(a<=b) */
				case OP_FBLE_UN: case OP_RBLE_UN: kind = 3; neg = 1; break; /* !(a>b) */
				default:                          kind = 2; neg = 1; break; /* (F|R)BGE_UN: !(a<b) */
				}
				FCMP_PUSH (isf32, kind, neg, cmp_a, cmp_b);
				COND_BRANCH ();
				terminated = TRUE;
				break;
			}
			case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN:
			case OP_ICGT: case OP_ICGT_UN: case OP_ICLE: case OP_ICLE_UN:
			case OP_ICGE: case OP_ICGE_UN: {
				/* setcc: dreg = (cmp_a <op> cmp_b) as 0/1, consuming the preceding OP_(I)COMPARE's
				 * operands (these carry no sregs, like the conditional branches above). */
				WasmOpcode w;
				if (cmp_a < 0) { fail = "setcc without compare"; goto done; }
				switch (ins->opcode) {
				case OP_ICEQ:    w = WASM_OP_I32_EQ; break;
				case OP_ICNEQ:   w = WASM_OP_I32_NE; break;
				case OP_ICLT:    w = WASM_OP_I32_LT_S; break;
				case OP_ICLT_UN: w = WASM_OP_I32_LT_U; break;
				case OP_ICGT:    w = WASM_OP_I32_GT_S; break;
				case OP_ICGT_UN: w = WASM_OP_I32_GT_U; break;
				case OP_ICLE:    w = WASM_OP_I32_LE_S; break;
				case OP_ICLE_UN: w = WASM_OP_I32_LE_U; break;
				case OP_ICGE:    w = WASM_OP_I32_GE_S; break;
				default:         w = WASM_OP_I32_GE_U; break; /* OP_ICGE_UN */
				}
				if (!wasm_ld (&body, &lc, cmp_a)) { fail = "setcc a"; goto done; }
				if (cmp_imm_mode) wasm_i32_const (&body, cmp_imm);
				else if (!wasm_ld (&body, &lc, cmp_b)) { fail = "setcc b"; goto done; }
				wasm_op (&body, w);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "setcc dreg"; goto done; }
				break;
			}
			case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
			case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE: {
				/* float compare-to-i32 setcc: standalone (own FREG operands). FC* are f64, RC* are f32
				 * (RC* sort after FC* in the opcode enum). dreg = (sreg1 <cc> sreg2) ? 1 : 0. */
				int kind, neg = 0;
				gboolean isf32 = (ins->opcode >= OP_RCEQ);
				switch (ins->opcode) {
				case OP_FCEQ: case OP_RCEQ:       kind = 0; break;
				case OP_FCNEQ: case OP_RCNEQ:     kind = 1; break; /* unordered ne */
				case OP_FCLT: case OP_RCLT:       kind = 2; break;
				case OP_FCGT: case OP_RCGT:       kind = 3; break;
				case OP_FCLE: case OP_RCLE:       kind = 4; break;
				case OP_FCGE: case OP_RCGE:       kind = 5; break;
				case OP_FCLT_UN: case OP_RCLT_UN: kind = 5; neg = 1; break; /* !(a>=b) */
				default:                          kind = 4; neg = 1; break; /* (F|R)CGT_UN: !(a<=b) */
				}
				FCMP_PUSH (isf32, kind, neg, ins->sreg1, ins->sreg2);
				if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fsetcc dreg"; goto done; }
				break;
			}
			case OP_BR:
				GOTO (ins->inst_target_bb, 0);
				terminated = TRUE;
				break;
			case OP_SWITCH: {
				/* jump table: dispatch sreg1 (the index, already bounds-checked to [0,N) by a preceding
				 * branch) to inst_many_bb[index]. N = GPOINTER_TO_UINT(ins->klass). Emit an if-chain of
				 * (index==i -> GOTO target_i) for i in [0,N-1), then an unconditional GOTO target_{N-1}
				 * (the in-range fall-through). Each per-case GOTO is inside one `if` (nesting 1); the
				 * final one is at bb top level (nesting 0), matching OP_BR. */
				guint nsw = GPOINTER_TO_UINT (ins->klass), si;
				if (nsw == 0) { fail = "switch no targets"; goto done; }
				for (si = 0; si + 1 < nsw; ++si) {
					if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "switch idx"; goto done; }
					wasm_i32_const (&body, (gint32) si);
					wasm_op (&body, WASM_OP_I32_EQ);
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					GOTO (ins->inst_many_bb [si], 1);
					wasm_op (&body, WASM_OP_END);
				}
				GOTO (ins->inst_many_bb [nsw - 1], 0);
				terminated = TRUE;
				break;
			}
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2: {
				/* Lower a direct managed call to call_indirect through the callee's f-slot.
				 * Callee args are call->args[0..nparams) (this first, if any); the result goes
				 * to ins->dreg — except OP_VCALL2 (vtype return): dreg is -1 and the callee writes
				 * the result through the trailing hidden-vret pointer (WjCallArgs position n, the decomposed
				 * OUTARG_VTRETADDR address). Bails (whole method -> interp) if the callee isn't
				 * JITted yet. */
				MonoCallInst *call = (MonoCallInst *) ins;
				MonoMethodSignature *csig = call->signature;
				WasmFuncType ct;
				WasmCallInfo cci;
				int call_fslot, type_idx = -1, k, ai;
#ifdef HOST_BROWSER
				gboolean late_fslot_block = FALSE;
#endif
				/* A synchronized callee keeps its Monitor.Enter/Exit in a separate SYNCHRONIZED wrapper; the raw
				 * method body (what mono_interp_get_imethod returns) has no monitor ops, so dispatching it
				 * directly would run unlocked -> a notify/wait throws IllegalMonitorStateException + leaves the
				 * monitor state wrong (the netty DefaultPromise save/quit hang). Substitute the wrapper HERE, at
				 * compile time (GC-safe; the prior bail's "can't create GC-safely" caveat was about the EMITTED
				 * runtime code after the arg spill, not the compiler). call_method is the effective callee for
				 * BOTH the direct f-slot path (the wrapper f-slots once its own body — incl. Monitor.Enter's
				 * bool& via OP_LDADDR — is jittable) AND the interp residual (which then runs the wrapper, with
				 * the monitor). The wrapper has the same signature, so the captured args still line up. Virtual
				 * calls are handled the same way in mono_wasm_jit_vcall_resolve(_fslot) (pre-spill). */
				MonoMethod *call_method = call->method;
				if (call_method && (call_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
					extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
					call_method = mono_marshal_get_synchronized_wrapper (call_method);
				}
#ifdef HOST_BROWSER
				/* Canonicalize the (uncached, per-compile-fresh) synchronized-inner wrapper to a stable
				 * instance so its f-slot is found across re-emits — see wj_canonical_callee. MUST match the
				 * pre-scan, which canonicalizes identically, so the recorded blocker == this f-slot key. */
				call_method = wj_canonical_callee (call_method);
#endif
				if (!call->method) {
					/* method==NULL: a runtime JIT-icall. On the cold path of an llvmonly
					 * virtual/interp call this is mini_llvmonly_init_vtable_slot, which lazily
					 * resolves the ftndesc vtable slot (the interp warmup doesn't populate the
					 * llvmonly slot, so the first JITted dispatch hits this). Indirect-call the
					 * icall wrapper (call->fptr = a wasm table index) with its signature +
					 * captured args, so the cold path resolves the slot instead of trapping. */
					gint32 ifptr;
#ifdef HOST_BROWSER
					/* R317 (MONO_WASM_JIT_FAST_TLS): the current MonoInternalThread without the call --
					 *     *(*(s.b + WJ_SCRATCH_THREAD_SLOT))   this thread's mono_wasm_tls_thread (threads.c)
					 * NULL while detached, as the pthread key reads. No call, so not a GC point: the call it
					 * replaces was only one because it was a call. */
					{
						extern int mono_wasm_jit_fast_tls;
						if (mono_wasm_jit_fast_tls && call->jit_icall_id == MONO_JIT_ICALL_mono_tls_get_thread_extern) {
							wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);                  /* s.b */
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, WJ_SCRATCH_THREAD_SLOT);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_uleb (&body, 2); wasm_uleb (&body, 0);
							if (!wasm_st (&body, &lc, ins->dreg)) { fail = "fast_tls dreg"; goto done; }
							wj_count (WJC_FAST_TLS_SITE);
							break;
						}
					}
					/* Use the RAW icall C function (info->func), a wasm table index callable with
					 * the icall's C signature directly. NOT call->fptr (= mono_icall_get_wrapper,
					 * which on wasm is the fixed-signature mono_wasm_specific_trampoline stub →
					 * call_indirect mismatch). The JITted code passes raw C args (vtable, slot). */
					{
						MonoJitICallInfo *iinfo = call->jit_icall_id ? mono_find_jit_icall_info (call->jit_icall_id) : NULL;
						ifptr = (iinfo && iinfo->func) ? (gint32) (intptr_t) iinfo->func : (gint32) (intptr_t) call->fptr;
					}
#else
					ifptr = 0x7ffe; /* offline dump: placeholder slot for encoder validation */
#endif
					if (!csig) { fail = "icall null sig"; goto done; }
					if (ifptr == 0) { fail = "icall no fptr"; goto done; }
					memset (&ct, 0, sizeof (ct));
					if (csig->hasthis) { if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "icall nargs"; goto done; } ct.params [ct.nparams++] = WASM_I32; }
					for (ai = 0; ai < (int) csig->param_count; ++ai) {
						WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
						if (pv == 0 || pv == WASM_VOID) {
							fail = "icall arg type";
							fail_sig_site = "icall"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = ai;
							goto done;
						}
						if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "icall nargs"; goto done; }
						ct.params [ct.nparams++] = pv;
					}
					if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
					else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) {
						fail = "icall ret type";
						fail_sig_site = "icall"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = -1;
						goto done;
					} }
					for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
					if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = ti_base + nextra++; }
					uses_calls = TRUE;
					if (!call->call_info) { fail = "no captured icall args"; goto done; }
					{
						int nm = csig->param_count + (csig->hasthis ? 1 : 0);
						for (ai = 0; ai < nm; ++ai)
							if (!wasm_ld (&body, &lc, wj_arg_vreg (call, ai))) { fail = "icall arg ld"; goto done; }
					}
					wj_emit_helper_call_ci (&body, (gpointer) (intptr_t) ifptr, (guint32) type_idx);
					if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "icall dreg"; goto done; }
					break;
				}
				if (!csig) { fail = "null sig call"; goto done; }
				/* Canonical callee functype + per-arg ABI from the one shared descriptor (this + args
				 * [+ trailing vret i32] -> ret). By-addr args and hidden vret are fully lowered here
				 * (Stage 2.3); ct.nparams includes the vret param, cci.nargs does not. */
				mono_wasm_get_call_info (csig, &cci);
				if (!cci.valid) {
					fail = cci.fail_reason;
					fail_sig_site = "call"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = cci.fail_arg;
					goto done;
				}
				ct = cci.ftype;
#ifdef HOST_BROWSER
				if (call_method == cfg->method) {
					extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
					/* SCC batch: the orchestrator reserved our slot on the imethod -> get_callee_fslot returns
					 * it. Otherwise standalone self-recursion needs a slot to bake before the method exists. */
					call_fslot = mono_wasm_jit_get_callee_fslot (call_method);
					if (call_fslot <= 0) {
						if (!wj_self_f_slot) {
							extern int mono_wasm_jit_reserve_self (MonoMethod *m, int *e_out, int *f_out);
							/* Reserve ON THE IMETHOD (wasm_jit_self_resv_*), not into these locals. These are
							 * locals of this function, so when an emit failed the pair was orphaned and the
							 * next attempt allocated a fresh one — and the allocator has no free. Since the
							 * drivers re-emit hard (force_island up to 10 passes, compile_scc phase 1 up to
							 * WJ_SCC_MAX*2), that leak scaled with re-emission count, which is precisely the
							 * axis island/module batching increases. Reserving on the imethod makes a re-emit
							 * reuse the same pair. */
							if (!mono_wasm_jit_reserve_self (cfg->method, &wj_self_e_slot, &wj_self_f_slot)) {
								fail = "jit function table exhausted"; goto done;
							}
						}
						call_fslot = wj_self_f_slot;
					}
				} else {
					extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
					call_fslot = mono_wasm_jit_get_callee_fslot (call_method);
					/* Phase 5: a callee with no f-slot gets a pool one behind a stub (mini-wasm-lazy.inc) -- the
					 * direct path below then emits an ordinary call through it. A refusal takes the residual. */
					if (call_fslot <= 0) {
						call_fslot = wj_lazy_reserve (call_method, csig, call->rgctx_reg != 0, &ct);
						if (call_fslot > 0)
							mono_wasm_jit_counters [WJC_LAZY_SITES]++;
					}
				}
#else
				call_fslot = 0x7fff; /* offline dump: placeholder slot for encoder validation */
#endif
				if (call_fslot > 0) {
					/* Callee is wasm-JITted: direct call_indirect through its f-slot (Phase 2). */
#ifdef HOST_BROWSER
					wj_result_add_direct_dep (&cfg->wasm_jit_result, call_fslot, wj_functype_hash (&ct), call_method);
					if (cfg->wasm_jit_result.direct_deps_truncated) { fail = "too many direct dependencies"; goto done; }
#endif
					/* Intern the callee functype unconditionally. It used to be skipped when the callee
					 * was a fellow batch member, because that site was hard-coded to `call <memberidx>`
					 * and a direct call names no type -- but the form is no longer decided here, so a
					 * type index has to exist for the call_indirect the assembler may still choose. */
					for (k = 0; k < nextra; ++k)
						if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
					if (type_idx < 0) {
						if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
						extra_types [nextra] = ct;
						type_idx = ti_base + nextra++;
					}
					uses_calls = TRUE;
					/* arg source vregs captured at method-to-ir time (calls.c), not call->args
					 * (which gets corrupted by later vreg passes) */
					if (!call->call_info) { fail = "no captured call args"; goto done; }
					{
						for (ai = 0; ai < cci.nargs; ++ai)
							if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "call arg ld"; goto done; }
						/* hidden vret: the trailing param is the ret-buffer address (the decomposed
						 * OUTARG_VTRETADDR vreg — an addr-frame address the callee writes through) */
						if (cci.vret_byaddr && !wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "call vret ld"; goto done; }
					}
					/* One hole, three possible outcomes, none of them decided here: a module-local
					 * `call <funcidx>` if the callee turns out to be co-located, an imported `call` if it
					 * is not but is safe to bind at instantiation, and `i32.const <fslot>; call_indirect`
					 * otherwise. Which one is chosen -- and re-chosen on every re-framing -- is the
					 * assembler's decision, because it is the only thing that knows the module. */
					wj_emit_method_call (&body, call_method, call_fslot, (guint32) type_idx);
					if (ct.ret != WASM_VOID) {
						if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
							if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "call scalar-vtype ret"; goto done; }
						} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
					}
				}
#ifdef HOST_BROWSER
				else {
					/* INLINE DIRECT AOT CALL (Build 1, no EH): the callee has no f-slot but IS AOT-compiled.
					 * Call its AOT code DIRECTLY with the native ABI — the jiterpreter directJitCalls/enableDirect
					 * path emitted inline: push (this, args by value, rgctx); call_indirect <cinfo->addr>. No
					 * interp_entry, no frame, no LMF (validated jit102: conservative GC + local resume-state).
					 * Gated by MONO_WASM_JIT_INLINE_AOT (default off). byref args/ret bail to the residual; a
					 * throwing AOT callee would escape uncaught until Build 2 adds wasm-EH — test non-throwing. */
					{
						extern gboolean mono_wasm_jit_aot_call_target (MonoMethod *m, gpointer *addr, gpointer *rgctx, gboolean *has_extra_arg);
						gpointer aot_addr = NULL, aot_rgctx = NULL;
						gboolean aot_has_extra = TRUE;   /* does the raw AOT body carry the trailing rgctx/dummy arg? */
						/* Gate: byref args/ret bail (interp_entry handles those). ALSO bail two cases this inline
						 * path cannot marshal, to the residual (which is correct for both):
						 *  - call->rgctx_reg: a generic-shared callee needs the CALLSITE runtime generic context, but
						 *    aot_call_target only returns the callee-side extra arg/rgctx used by the llvm_only direct
						 *    call ABI. Reusing that value here would feed the callee the wrong context (bad GOT lookups /
						 *    null-fn trap). The comment at the RGCTX flag promised this skip; enforce it here (the vcall
						 *    path already bails rgctx at the membase site). - call->need_unbox_trampoline: a boxed-valuetype receiver (object/interface target)
						 *    needs `this` unboxed (+sizeof(MonoObject)); aot_call_target hands back the RAW body with no
						 *    unbox tagging, so the inline path would pass the boxed header pointer -> field reads off by
						 *    the header. The residual/interp preserves unbox semantics. */
						/* Hidden vret also bails to the residual: the native AOT ABI puts the vret pointer
						 * FIRST (LLVMArgVtypeRetAddr, vret_arg_index==0) while our internal convention is
						 * vret-LAST — reordering here isn't wired up, and the residual is vret-correct.
						 * By-addr ARGS are fine inline: the native ABI passes the same copy address
						 * positionally (ArgValuetypeAddrOnStack). */
						/* MONO_WASM_JIT_INLINE_AOT gated this and shipped 1: emit the inline direct same-ABI
						 * AOT call (call_indirect cinfo->addr with this+args+rgctx, no interp_entry, no
						 * frame, no LMF) instead of the residual, for AOT'd callees. It is HOT -- ~90k
						 * inline-AOT calls per frame -- and its off-arm routed every one of them through
						 * the full residual marshal. Settled. */
						/* R356: need_unbox_trampoline is set for EVERY method declared on an interface or on Object
						 * (calls.c:531), static ones included -- IKVM's default-interface helpers
						 * (`Map.<default>getOrDefault(Map,...)`) and Java static interface methods. It concerns `this`, so
						 * MONO_WASM_JIT_AOT_STATIC_UNBOX ignores it for a callee that has none. */
						extern int mono_wasm_jit_aot_static_unbox;
						gboolean unbox_bars = call->need_unbox_trampoline && !(mono_wasm_jit_aot_static_unbox && !csig->hasthis);
						gboolean aot_ok = !m_type_is_byref (csig->ret)
							&& !call->rgctx_reg && !unbox_bars && !cci.vret_byaddr;
						/* R356: MONO_WASM_JIT_AOT_BYREF lets byref PARAMS through. A byref is an i32 address in the call
						 * info (wasm_valtype_of_type), passed exactly as it is to a JIT callee, and the native AOT ABI
						 * takes the same pointer; the object it points into is kept by the caller's shadow frame, as for
						 * a JIT callee. A byref RETURN still bails. */
						{
							extern int mono_wasm_jit_aot_byref;
							if (!mono_wasm_jit_aot_byref)
								for (k = 0; k < (int) csig->param_count && aot_ok; ++k)
									if (m_type_is_byref (csig->params [k])) aot_ok = FALSE;
						}
						if (aot_ok && mono_wasm_jit_aot_call_target (call_method, &aot_addr, &aot_rgctx, &aot_has_extra)) {
							if (call_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
								wj_count (WJC_ICALL_AOT_SITE);   /* J1a: only reachable with MONO_WASM_JIT_ICALL_AOT */
							/* FROM cci.ftype, NOT ct. `ct` is the JIT-callee functype and carries the
							 * trailing threaded frame base under MONO_WASM_JIT_THREAD_SP; an AOT body has
							 * no such parameter and this site never pushes one, so copying `ct` here
							 * declares one param too many and the module fails to COMPILE with
							 * "not enough arguments on the stack" -- which is a whole-module loss that
							 * shows up as `registered` falling and WJC_INVALID rising, never as an error
							 * at the call. Measured: 112 of 392 methods (jbox2d), 5.0x
							 * slower, because this is the highest-volume call site in the emitter.
							 * cci.ftype is the unmodified logical signature -- the old THREAD_SP path mutated
							 * only the copy. */
							WasmFuncType nt = cci.ftype;   /* native functype = (this?, args by value) [+ i32 rgctx if aot_has_extra] -> ret */
							int nti = -1;
							/* Append the trailing extra (rgctx/dummy) arg ONLY when the body actually has it. Exempt
							 * wrapper kinds (alloc/castclass/icall/etc.) have a bare (this,args)->ret body — appending
							 * would declare one param too many -> call_indirect signature-mismatch trap. */
							if (aot_has_extra) {
								if (nt.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "aot inline nparams"; goto done; }
								nt.params [nt.nparams++] = WASM_I32;
							}
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &nt)) { nti = ti_base + k; break; }
							if (nti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = nt; nti = ti_base + nextra++; }
							uses_calls = TRUE;
							if (!call->call_info) { fail = "no captured call args"; goto done; }
							{
								/* AOT-style EH (cppeh, the only model now): bare direct AOT call. A throwing AOT
								 * callee C++-unwinds (wasm-EH) straight through this JITted frame to the nearest
								 * landing pad — the interp e-thunk boundary (mono_llvm_catch_exception) or an
								 * in-method catch. The call is pure call_indirect (the per-call perf win); no
								 * try/catch, no per-call pending-exception check. */
								wj_emit_fast_count (&body, WJC_FAST_INLINE_AOT);   /* profile: INLINE_AOT direct dispatch */
								{ for (ai = 0; ai < cci.nargs; ++ai) if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "call arg ld"; goto done; } }
								if (aot_has_extra) wasm_i32_const (&body, (gint32) (intptr_t) aot_rgctx);   /* trailing rgctx/dummy — only if the body has it */
								/* A constant table index with a known functype, exactly like a runtime helper -- an AOT
								 * body is just another fixed occupant of the function table. Whether it becomes an
								 * import or stays a call_indirect is MONO_WASM_JIT_AOT_IMPORTS, applied at assembly.
								 * This is the highest-volume import source there is: ~90k inline-AOT calls per frame. */
								wj_emit_aot_call (&body, (gpointer) (intptr_t) aot_addr, (guint32) nti);
								if (ct.ret != WASM_VOID) {
								if (ct.ret == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret);   /* raw AOT body: dirty upper bits */
									if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
										if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "aot scalar-vtype ret"; goto done; }
									} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "call dreg"; goto done; }
								}
							}
							{ extern int mono_wasm_jit_verbose; static int n_il = 0; if (mono_wasm_jit_verbose >= 2 && n_il++ < 128) { char *cn = mono_method_get_full_name (call->method); printf ("WASM_JIT_INLINE_AOT[%d] %s -> %s\n", n_il, mname, cn); g_free (cn); } }
							break;
						}
						/* R356: which condition sent this direct call site to the residual, for the aroute dump.
						 * Stats only; a pointer-keyed note, no metadata walk. */
						if (G_UNLIKELY (mono_wasm_jit_stats)) {
							extern void mono_wasm_jit_resid_emit_note (MonoMethod *m, const char *why);
							mono_wasm_jit_resid_emit_note (call_method,
								m_type_is_byref (csig->ret) ? "byref-ret" : call->rgctx_reg ? "rgctx" :
								unbox_bars ? "unbox" : cci.vret_byaddr ? "vret" :
								!aot_ok ? "byref-arg" : "aot-target-at-emit");
						}
					}
					/* Callee NOT wasm-JITted (no f-slot): route just this call through the interpreter
					 * via mono_wasm_jit_call_interp, keeping the rest of the method JITted — instead of
					 * bailing the whole method (the big real-MC coverage lever: hot JITted methods almost
					 * always call some cold / un-JIT-able callee). Fetch the per-thread scratch buffer,
					 * spill scalar args into it (this at slot 0 if instance, then params at slot i, 8B
					 * each), call the helper, then load the result from buf+WJ_SCRATCH_RET_OFF. */
					extern gpointer mono_wasm_jit_scratch (void);
	extern gpointer mono_wasm_jit_cur_island_il_state_addr (void);
					extern int mono_wasm_jit_call_interp (MonoMethod *method, guint8 *buf); /* ->1 if callee threw */
					WasmFuncType ts, ti;     /* ()->i32 (get scratch); (i32,i32)->i32 (call_interp: threw?) */
					int tsi = -1, tii = -1, bi;
					{
						extern int mono_wasm_jit_residual_mode;
						int rm = mono_wasm_jit_residual_mode;
						/* A direct callee with no f-slot reaches here only if the lazy pool refused it (mini-wasm-lazy.inc:
						 * AOT-backed, permanently un-JITtable, a wrapper or flag shape, a functype it cannot stub). Nothing
						 * force-compiles a callee any more (R366), so the call takes the residual: AOT-routed for an AOT'd
						 * callee, interp_entry otherwise. Counted when it is neither of those, i.e. a refusal. */
						if (rm == 0 && !((MonoCallInst*)ins)->rgctx_reg) {
							extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
							extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
							if (!mono_interp_jit_call_supported (call_method, csig) && !mono_wasm_jit_callee_perm_unjittable (call_method))
								mono_wasm_jit_counters [WJC_LAZY_RESIDUAL]++;
						}
						/* bisection gates (see mono_wasm_jit_residual_mode): isolate which marshalling axis
						 * breaks real code by restricting which call shapes the residual handles */
						if (rm == 2 && ct.ret != WASM_VOID) { fail = "callee not jitted (residual: non-void ret)"; goto done; }
						if (rm == 3 && csig->param_count > 0) { fail = "callee not jitted (residual: has params)"; goto done; }
						if (rm == 4 && csig->hasthis) { fail = "callee not jitted (residual: instance)"; goto done; }
						if (rm == 5 && csig->param_count > 0 && ct.ret != WASM_VOID) { fail = "callee not jitted (residual: params+nonvoid)"; goto done; }
					}
					/* No-rebuild bisection: bail residuals whose callee simple-name is listed in
					 * MONO_WASM_JIT_RESIDUAL_SKIP, to pin which specific callee corrupts real code. */
					{ extern gboolean mono_wasm_jit_residual_name_skipped (const char *name);
					  if (call->method->name && mono_wasm_jit_residual_name_skipped (call->method->name)) { fail = "residual skip (env)"; goto done; } }
					if (!call->call_info) { fail = "no captured call args"; goto done; }
					/* BYREF args/ret in the CALLEE signature (MONO_WASM_JIT_BYREF, default on). The residual
					 * routes the call through interp_entry, and both halves of that marshalling are now
					 * correct, so we no longer bail the whole caller (which was permanent, bail -7, and was
					 * the single largest perm-unjittable source in the IKVM soak: 4.27M of 4.28M vperm):
					 *   ARGS: the spill stores the byref VALUE (the pointer) into the scratch slot, and the
					 *     C side derefs exactly those slots (wj_arg_slot_holds_pointer in interp.c, shared by
					 *     mono_wasm_jit_call_interp and wasm_jit_aot_call_lean) to match interp_entry's
					 *     asymmetric convention (byref = the pointer; by-value = a pointer TO the value).
					 *     This was already implemented before the gate — the gate was pure conservatism.
					 *   RET: stackval_to_data_sign_ext now guards byref (interp.c). It previously switched on
					 *     type->type, so `ref sbyte/byte/short/ushort` got the sign/zero-extended LOW BYTE(S)
					 *     OF THE POINTER — reachable via wasm_jit_aot_call_lean, which passes the raw
					 *     sig->ret. (interp_entry's own tail was always safe: rmethod->rtype is normalized
					 *     to MONO_TYPE_I.) That was the real bug this gate was protecting against, and it is
					 *     what makes Unsafe.Add/AddByteOffset safe here.
					 * Nothing is needed on the emit side: byref is WJ_ARG_SCALAR/i32 in cci/ct, and the
					 * sub-word ret normalization below keys off mini_get_underlying_type, which erases byref
					 * to MONO_TYPE_I -> no mask/extend over a pointer. The DIRECT f-slot path above always
					 * handled byref (pure wasm pointer pass, no interp_entry).
					 * =0 restores the bails; the fail strings must keep the substring "byref" and must NOT
					 * contain "arg type"/"ret type" (tested first) so the -7 classification still holds. */
					/* MONO_WASM_JIT_BYREF gated this bail and shipped 1: lower a call whose CALLEE SIGNATURE
						 * carries byref (ref/out/Span) args or a byref return. The off-arm bailed the whole
						 * method, which on this workload is most of Span/Unsafe. */
						if (0) {
						if (m_type_is_byref (csig->ret)) {
							fail = "residual byref ret";
							fail_sig_site = "residual"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = -1;
							goto done;
						}
						for (bi = 0; bi < (int) csig->param_count; ++bi)
							if (m_type_is_byref (csig->params [bi])) {
								fail = "residual byref arg";
								fail_sig_site = "residual"; fail_sig_callee = call_method; fail_sig = csig; fail_sig_arg = bi;
								goto done;
							}
					}
					if (cci.nargs * 8 > 192 /*WJ_SCRATCH_RET_OFF*/) { fail = "residual nargs"; goto done; }
					/* Don't residual reflection methods: running e.g. MonoMethodInfo:get_parameter_info via
					 * interp_entry while a dynamic type is mid-Finish can re-enter that type's Finish. Bail
					 * these to the interpreter; keep the rest. */
					{ const char *_ns = m_class_get_name_space (call->method->klass);
					  if (_ns && !strncmp (_ns, "System.Reflection", 17)) { fail = "residual reflection"; goto done; } }
					/* Diagnostic: list the params+non-void residual sites. Only reached in modes 1/4 (mode 5
					 * bails this shape above). Lets a =1/=4 run name a suspect caller->callee. */
					if (mono_wasm_jit_verbose >= 3 && csig->param_count > 0 && ct.ret != WASM_VOID) {
						char *cn = mono_method_get_full_name (call->method);
						printf ("WASM_JIT_RESIDUAL_PNV %s -> %s\n", mname, cn ? cn : "?");
						g_free (cn);
					}
					/* SELF-HEALING DIRECT RESIDUAL. This caller was emitted before call_method acquired
					 * an f-slot. Bake its stable InterpMethod identity and, on each residual execution,
					 * ask the tiny late-fslot helper to follow tiering + admit the module on this thread.
					 * A live result calls the ordinary f-thunk directly, before pretransform/scratch/
					 * interp_entry. A zero result falls through to the unchanged residual below.
					 *
					 * Exclude native-AOT callees (their slot can never heal and the inline-AOT/residual
					 * AOT routes above are authoritative) and permanent wasm-JIT bails. */
					{
						extern gpointer mono_wasm_jit_get_callee_imethod (MonoMethod *method);
						extern int mono_wasm_jit_late_fslot (gpointer imethod);
						extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
						extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
						gpointer late_im = mono_wasm_jit_get_callee_imethod (call_method);
						gboolean can_heal = late_im && !mono_wasm_jit_callee_perm_unjittable (call_method)
							&& !mono_interp_jit_call_supported (call_method, csig);
						if (can_heal) {
							WasmFuncType ht; int hti = -1;
							for (k = 0; k < nextra; ++k)
								if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
							if (type_idx < 0) {
								if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
								extra_types [nextra] = ct; type_idx = ti_base + nextra++;
							}
							memset (&ht, 0, sizeof (ht)); ht.params [0] = WASM_I32; ht.nparams = 1; ht.ret = WASM_I32;
							for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ht)) { hti = ti_base + k; break; }
							if (hti < 0) {
								if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
								extra_types [nextra] = ht; hti = ti_base + nextra++;
							}
							uses_calls = TRUE;
							late_fslot_block = TRUE;
							/* MONO_WASM_JIT_HEAL_WAIT recorded the callee in cfg->wasm_jit_result.heal_callees[]
							 * here, so the PUBLISH path could register this method as a waiter on it (lock
							 * ordering: wj_waiter_register needs mono_loader_lock OUTSIDE the compile section,
							 * and this is inside it) and the callee's own publish would wake this method for
							 * re-emission -- whose new body takes the ordinary `call_fslot > 0` path and drops
							 * the healing block entirely. Deleted with re-emission. The site COUNT stays: it
							 * sizes the pool a relocatable f-slot hole would address, which is R195's proposal
							 * and needs no re-emission at all. */
							wj_count (WJC_HEAL_SITES);
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $after_residual */
							wasm_i32_const (&body, (gint32) (intptr_t) late_im);
							wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_late_fslot, hti);
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							{
								for (ai = 0; ai < cci.nargs; ++ai)
									if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "late call arg ld"; goto done; }
								if (cci.vret_byaddr && !wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "late call vret ld"; goto done; }
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wj_emit_dynamic_call (&body, (guint32) type_idx);
								if (ct.ret != WASM_VOID) {
									if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
										if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "late scalar-vtype ret"; goto done; }
									} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "late call dreg"; goto done; }
								}
							}
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> $after_residual */
							wasm_op (&body, WASM_OP_END);
						}
					}
					memset (&ts, 0, sizeof (ts)); ts.ret = WASM_I32; ts.nparams = 0;
					memset (&ti, 0, sizeof (ti)); ti.ret = WASM_I32; ti.nparams = 2; ti.params [0] = WASM_I32; ti.params [1] = WASM_I32;
					for (k = 0; k < nextra; ++k) {
						if (tsi < 0 && functype_eq (&extra_types [k], &ts)) tsi = ti_base + k;
						if (tii < 0 && functype_eq (&extra_types [k], &ti)) tii = ti_base + k;
					}
					if (tsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ts; tsi = ti_base + nextra++; }
					if (tii < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ti; tii = ti_base + nextra++; }
					uses_calls = TRUE;
					/* scratch-reentrancy: pre-transform the callee BEFORE spilling its args into the
					 * GC-invisible scratch (mirrors the vcall path's mono_wasm_jit_vcall_resolve). The
					 * transform can run the callee's class cctor — arbitrary managed code that allocates
					 * (-> GC, which would move ref args already spilled as raw pointers into the non-GC-scanned
					 * scratch) and can itself re-enter another residual on this thread (clobbering the still-
					 * unread spilled args). Doing it now — while the ref args still live in the GC-scanned ref
					 * shadow stack and the scratch is free — closes both windows; call_interp then finds the
					 * imethod already transformed and only does the GC-free marshal. (i32)->void. */
					{
						extern void mono_wasm_jit_pretransform (MonoMethod *method);
						WasmFuncType tp; int tpi = -1, tpk;
						memset (&tp, 0, sizeof (tp)); tp.params [0] = WASM_I32; tp.nparams = 1; tp.ret = WASM_VOID;
						for (tpk = 0; tpk < nextra; ++tpk) if (functype_eq (&extra_types [tpk], &tp)) { tpi = ti_base + tpk; break; }
						if (tpi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = tp; tpi = ti_base + nextra++; }
						wasm_i32_const (&body, (gint32) (intptr_t) call_method);
						wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_pretransform, tpi);
					}
						/* $scratch = imported s.b. `wj_scratch` is a __thread ARRAY, so its address is a per-thread
						 * CONSTANT — no load and, more importantly, no helper CALL, which Rounds 88/90 established
						 * is the expensive part of a dispatch path in this backend. */
					wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);
					wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
					{
						for (ai = 0; ai < cci.nargs; ++ai) {
							WasmOpcode sop; int al;
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wj_emit_one_call_arg (&body, &lc, &cci, csig, call, ai)) { fail = "residual arg ld"; goto done; }
							switch (ct.params [ai]) {
							case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
							case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
							case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
							case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
							default: fail = "residual arg type"; goto done;
							}
							wasm_op (&body, sop); wasm_memarg (&body, (guint32) al, (guint32) (ai * 8));
						}
						/* hidden vret: bake the caller's return-buffer address at scratch+232. The C side
						 * memcpys the VT result through it — NEVER via the 8-byte ret slot at 192, which a
						 * large struct would overrun into the 200..231 control fields. (A by-addr vtype arg's
						 * scratch slot likewise holds the copy's ADDRESS; call_interp/aot_call_lean deref
						 * those per mono_wasm_jit_arg_is_byaddr.) */
						if (cci.vret_byaddr) {
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							if (!wasm_ld (&body, &lc, wj_arg_vreg (call, cci.nargs))) { fail = "residual vret ld"; goto done; }
							wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 232 /* WJ_SCRATCH_VRET_OFF */);
						}
					}
					/* DIAG (WASM_JIT_BADREF_ARG caller attribution): bake THIS (calling) method at scratch+224 so
					 * call_interp's bad-ref check can name the type-confusion SOURCE, not just the callee. Written
					 * immediately before the call (after the spills) so a nested residual can't leave a stale value.
					 * EMITTED ONLY UNDER OBJGUARD -- its sole consumer is the bad-ref block in call_interp, which is
					 * gated on the same knob. This store shipped unconditionally in every residual site of every
					 * module for the duration of the type-confusion hunt. */
					if (G_UNLIKELY (mono_wasm_jit_objguard)) {
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 224 /* WJ_SCRATCH_CALLER_OFF */);
					}
					/* mono_wasm_jit_call_interp(method, $scratch): bake call_method (the synchronized wrapper for a
					 * synchronized callee) so the interp runs it WITH the monitor; plain callees pass through. */
					wasm_i32_const (&body, (gint32) (intptr_t) call_method);
					wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
					wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_call_interp, tii);
					/* call_interp returns 1 if the callee threw: the result slot is stale AND an exception
					 * is armed. Continue the native unwind (never a normal return -- that skipped this
					 * frame's own landing pad and delivered the exception late; FINDINGS Finding 15). */
					WJ_HINT (0);   /* R339 */
					wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
					/* threw: continue the native unwind; do NOT return and do NOT pop roots first. */
					EMIT_RESIDUAL_THROW_CONTINUATION ();
					wasm_op (&body, WASM_OP_END);
					if (ct.ret != WASM_VOID) {
						WasmOpcode lop; int al;
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						switch (ct.ret) {
						case WASM_I32: lop = WASM_OP_I32_LOAD; al = 2; break;
						case WASM_I64: lop = WASM_OP_I64_LOAD; al = 3; break;
						case WASM_F32: lop = WASM_OP_F32_LOAD; al = 2; break;
						case WASM_F64: lop = WASM_OP_F64_LOAD; al = 3; break;
						default: fail = "residual ret type"; goto done;
						}
						wasm_op (&body, lop); wasm_memarg (&body, (guint32) al, 192 /*WJ_SCRATCH_RET_OFF*/);
						/* Normalize sub-word integer returns to the full i32 the IR consumer expects.
						 * interp_entry writes the result via stackval_to_data, which for a bool/i1/u1
						 * return writes only 1 byte and for i2/u2/char only 2 bytes (interp.c) — so a
						 * full-width i32.load above reads stale upper bytes from the per-thread scratch
						 * buffer. Sign/zero-extend per the C# return type to match exactly what a normal
						 * interp call would leave in the result vreg (idempotent if the write was already
						 * full-width, as in the non-llvm_only sign-ext path). */
						/* NB byref: a byref ret IS WASM_I32, so this guard fires — it is
						 * mini_get_underlying_type erasing byref to MONO_TYPE_I that keeps the switch a
						 * no-op over the pointer. Skip explicitly rather than rely on that (see
						 * wasm_emit_subword_ret_norm). */
						if (ct.ret == WASM_I32 && !m_type_is_byref (csig->ret)) {
							MonoType *rt = mini_get_underlying_type (csig->ret);
							switch (rt->type) {
							case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1:
								wasm_i32_const (&body, 0xff); wasm_op (&body, WASM_OP_I32_AND); break;
							case MONO_TYPE_I1:
								wasm_op (&body, WASM_OP_I32_EXTEND8_S); break;
							case MONO_TYPE_CHAR: case MONO_TYPE_U2:
								wasm_i32_const (&body, 0xffff); wasm_op (&body, WASM_OP_I32_AND); break;
							case MONO_TYPE_I2:
								wasm_op (&body, WASM_OP_I32_EXTEND16_S); break;
							default: break;
							}
						}
						if (cci.ret.kind == WJ_ARG_VTYPE_SCALAR) {
							if (!wj_store_scalar_vtype_result (&body, &lc, ct.ret, wj_arg_vreg (call, cci.nargs))) { fail = "residual scalar-vtype ret"; goto done; }
						} else if (!wasm_st (&body, &lc, ins->dreg)) { fail = "residual dreg"; goto done; }
					}
					if (late_fslot_block)
						wasm_op (&body, WASM_OP_END); /* $after_residual */
				}
#else
				else { fail = "callee not jitted"; goto done; }
#endif
				break;
			}
			case OP_CALL_REG: case OP_VOIDCALL_REG: case OP_FCALL_REG: case OP_LCALL_REG: case OP_RCALL_REG:
				case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE: case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
				case OP_VCALL2_MEMBASE: {
				/* Indirect/virtual call: call_indirect through the target in sreg1 (a wasm table
				 * index = ftndesc.addr); rgctx (ftndesc.arg) passed as a trailing i32 param. The
				 * slot holds the receiver's entry, so the dispatch is JITted even if the callee
				 * runs in the interpreter. */
				MonoCallInst *call = (MonoCallInst *) ins;
				MonoMethodSignature *csig = call->signature;
				WasmFuncType ct;
				int type_idx = -1, k, ai, nmeth;
				gboolean is_membase = ins->opcode == OP_CALL_MEMBASE || ins->opcode == OP_VOIDCALL_MEMBASE || ins->opcode == OP_FCALL_MEMBASE || ins->opcode == OP_LCALL_MEMBASE || ins->opcode == OP_RCALL_MEMBASE ||
					ins->opcode == OP_VCALL2_MEMBASE;
				/* J1e: a value-type result. Only the virtual branch below lowers it; a raw indirect one still bails. */
				gboolean is_vcall2 = ins->opcode == OP_VCALL2_MEMBASE;
				if (is_vcall2 && !mono_wasm_jit_vcall_vret) { fail = "unsupported opcode"; fail_op = ins->opcode; goto done; }
				if (!csig) { fail = "callreg null sig"; goto done; }
				/* Do not reject an rgctx-bearing virtual call here. The virtual path resolves the
				 * concrete override first: its JIT f-slot uses our canonical concrete ABI, its AOT
				 * route obtains the resolved ftndesc rgctx from vcall_aot_target, and its interpreter
				 * fallback derives context from that concrete MonoMethod. The callsite rgctx is thus
				 * neither part of nor needed by any of those three dispatches. This gate used to keep
				 * otherwise-concrete Minecraft virtual callers permanently interpreted (~0.9M weighted
				 * vperm calls in profile8). Raw indirect calls are handled by their existing auto-JIT
				 * safety gate below; in the explicitly targeted case their rgctx outarg is already folded
				 * into csig/call_info, as documented there. */
				/* Virtual method call (interp-only runtime): the vtable slot holds a fixed-sig
				 * trampoline stub, not a callable entry, so we can't call_indirect it directly. Lower
				 * to a call of mono_wasm_jit_vcall_i4(this, base_method) — a C helper that resolves the
				 * override for the receiver and invokes it via the interpreter (interp_entry). v1
				 * supports the (this)->i4 shape (the common hot polymorphic call, e.g. getBlockState);
				 * other shapes bail to interp. */
					if (is_membase && call->method && (call->method->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
						/* Virtual / interface call under the interp-only runtime, with a fast direct-f-slot path
						 * (gap #3). A C helper resolves the override for the receiver, then:
						 *   - FAST: if the override is itself wasm-JITted, the helper syncs THIS thread's function
						 *     table (so the slot is populated locally — the table is PER-THREAD) and returns its
						 *     scalar `f` f-slot; the JITted caller then call_indirects straight into it (this+args
						 *     -> ret), no interp re-entry. This kills the per-call interp_entry "virtual storm".
						 *   - FALLBACK (un-JITted override, or a synchronized wrapper, which never gets an f-slot):
						 *     marshal (this + args) into the per-thread scratch buffer and run the override through
						 *     the interpreter (interp_entry, which writes the scalar result into scratch + returns 1
						 *     if it threw) — the same proven path the direct residual uses.
						 * The helper carries a per-call-site inline cache (vtable -> InterpMethod*) that skips the
						 * expensive virtual resolve on a monomorphic hit (the dominant per-call cost). We cache the
						 * RESOLVE, not the f-slot, and still per-thread-sync before dispatching the f-slot — so we
						 * never call_indirect a slot absent on the calling thread (the hazard a cached-f-slot inline
						 * IC would hit: the table is PER-THREAD, a slot cached by one worker can be absent in another,
						 * and the i32 pair can tear -> a bad call_indirect that traps and kills the worker). */
						extern gpointer mono_wasm_jit_scratch (void);
	extern gpointer mono_wasm_jit_cur_island_il_state_addr (void);
						extern int mono_wasm_jit_vcall_resolve_fslot (MonoObject *this_obj, MonoMethod *base_method, guint8 *scratch, gpointer ic);
						extern gpointer mono_wasm_jit_vcall_aot_pic_lookup (MonoVTable *vt, gpointer aic);
							extern int mono_wasm_jit_call_interp (MonoMethod *m, guint8 *buf);
							extern int mono_wasm_jit_call_delegate (MonoMethod *m, guint8 *buf);
						WasmFuncType vts, vtrf, vtd, ftd, dftd; WasmCallInfo vci; int vtsi = -1, vtrfi = -1, vtdi = -1, ftdi = -1, dftdi = -1, vk, ai, n2, this_vr; int aic_ati = -1, aic_ati_ne = -1;
						WasmValtype pp [WASM_FUNCTYPE_MAX_PARAMS], rv; int npp = 0; gpointer vic;
						/* J1e: vc_vret = an OP_VCALL2_MEMBASE site with a hidden-vret result; n2c = the JIT-`f` argument
						 * count, n2 plus that trailing vret pointer. */
						gboolean vc_vret = FALSE; int n2c;
						MonoVTable *pred_vt = NULL;
						gint32 *pred_cha = NULL;   /* plan2x S3: a CHA arm's guard word (pred_vt stays NULL) */
						MonoMethod *pred_target = NULL;
						int pred_fslot = 0;
						MonoMethod *ddevirt_target = NULL;   /* R215 delegate devirt */
						int ddevirt_fslot = 0;
						/* R205: which devirt outcome this site's IC serves. 0 unknown, 1 no_rec, 2 cold, 3 poly,
						 * 4 poly_90, 5 sig, 6 no_fslot, 7 the site GOT an arm and this IC is its alternate-receiver
						 * fallthrough. Set at every devirt exit, consumed at the IC hit tail. */
						int ic_tag = 0;
						/* R206: the runner-up arm. Resolved only when arm 1 exists, since its whole purpose is to
						 * catch what arm 1's guard rejects. */
						MonoVTable *pred2_vt = NULL;
						MonoMethod *pred2_target = NULL;
						int pred2_fslot = 0;
						gboolean slim_pred = FALSE;
						gboolean is_delegate_invoke = !strcmp (call->method->name, "Invoke") &&
							m_class_get_parent (call->method->klass) == mono_defaults.multicastdelegate_class;
						/* R311: the vtable slot a METHOD-identity check may test at this site (< 0: none -- a
						 * delegate, an interface or generic method, or a class whose vtable is not set up). */
						extern int mono_wasm_jit_mid_slot (MonoMethod *base);
						extern int mono_wasm_jit_class_vtable_off (void);
						extern int mono_wasm_jit_ic_mid, mono_wasm_jit_pred_mid;
						int vc_mid_slot = is_delegate_invoke ? -1 : mono_wasm_jit_mid_slot (call->method);
						int vc_class_vtable_off = mono_wasm_jit_class_vtable_off ();
						if (!csig->hasthis) { fail = "vcall not instance"; goto done; }
						if (!call->call_info) { fail = "no captured vcall args"; goto done; }
						this_vr = wj_arg_vreg (call, 0);
						/* BYREF args/ret (MONO_WASM_JIT_BYREF, default on) — see the long rationale at the direct
						 * residual gate: the arg deref and the byref-guarded stackval_to_data_sign_ext make
						 * interp_entry's by-pointer marshalling correct, so we no longer bail.
						 * Relaxing THIS gate is the larger half of the win. It sits at the very TOP of the vcall
						 * lowering — before mono_wasm_get_call_info below, the inline f-slot IC, the devirt/slim
						 * PIC, the AOT IC, the direct-f-slot fast path AND the residual fallback — so a byref
						 * virtual site used to lose EVERY fast path, not just the interp fallback, even though
						 * all the wasm-only ones pass a byref as a plain i32. (mono_wasm_get_call_info classifies
						 * a byref as WJ_ARG_SCALAR/i32 with valid=TRUE, so it clears the arg-kind filter below.) */
						/* MONO_WASM_JIT_BYREF gated this bail and shipped 1: lower a call whose CALLEE SIGNATURE
						 * carries byref (ref/out/Span) args or a byref return. The off-arm bailed the whole
						 * method, which on this workload is most of Span/Unsafe. */
						if (0) {
							if (m_type_is_byref (csig->ret)) {
								fail = "vcall byref ret";
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
								goto done;
							}
							for (ai = 0; ai < (int) csig->param_count; ++ai)
								if (m_type_is_byref (csig->params [ai])) {
									fail = "vcall byref arg";
									fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = ai;
									goto done;
								}
						}
						/* Arg/ret valtypes from the shared ABI descriptor (this + params -> ret). A
						 * scalar-vtype arg is scalar at the wasm boundary too; load its single field in
						 * every fast/AOT path and spill that same scalar into the residual scratch. */
						{
							mono_wasm_get_call_info (csig, &vci);
							if (!vci.valid) {
								fail = vci.vret_byaddr ? "vcall ret type" : vci.fail_reason;
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = vci.fail_arg;
								goto done;
							}
							/* A hidden-vret result is lowered only for OP_VCALL2_MEMBASE (J1e); valid no longer implies a
							 * scalar ret, so anything else with one bails. */
							if (vci.vret_byaddr && !is_vcall2) {
								fail = "vcall ret type";
								fail_sig_site = "vcall"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
								goto done;
							}
							for (ai = 0; ai < vci.nargs; ++ai)
								if (vci.args [ai].kind != WJ_ARG_SCALAR && vci.args [ai].kind != WJ_ARG_VTYPE_SCALAR &&
								    vci.args [ai].kind != WJ_ARG_VTYPE_BYADDR) { fail = "vcall arg type"; goto done; }
							if (is_vcall2 && !vci.vret_byaddr && vci.ret.kind != WJ_ARG_VTYPE_SCALAR) { fail = "vcall2 ret kind"; goto done; }
							for (ai = 0; ai < (int) vci.ftype.nparams; ++ai) pp [ai] = vci.ftype.params [ai];
							npp = (int) vci.ftype.nparams;
							rv = vci.ftype.ret;
						}
						vc_vret = is_vcall2 && vci.vret_byaddr;
						/* The delegate routes (recipes, open/closed shapes, dftd) have no value-type result lowering. */
						if (is_vcall2 && is_delegate_invoke) { fail = "vcall2 delegate"; goto done; }
						/* functypes: vts ()->i32 (scratch); vtrf (i32,i32,i32,i32)->i32 (resolve_fslot: this,base,scratch,ic);
						 * vtd (i32,i32)->i32 (call_interp fallback); ftd this+params->ret (the override's scalar `f`). */
						memset (&vts, 0, sizeof (vts)); vts.nparams = 0; vts.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vts)) { vtsi = ti_base + vk; break; }
						if (vtsi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vts; vtsi = ti_base + nextra++; }
						memset (&vtrf, 0, sizeof (vtrf)); vtrf.params [0] = WASM_I32; vtrf.params [1] = WASM_I32; vtrf.params [2] = WASM_I32; vtrf.params [3] = WASM_I32; vtrf.nparams = 4; vtrf.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtrf)) { vtrfi = ti_base + vk; break; }
						if (vtrfi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtrf; vtrfi = ti_base + nextra++; }
						memset (&vtd, 0, sizeof (vtd)); vtd.params [0] = WASM_I32; vtd.params [1] = WASM_I32; vtd.nparams = 2; vtd.ret = WASM_I32;
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vtd)) { vtdi = ti_base + vk; break; }
						if (vtdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = vtd; vtdi = ti_base + nextra++; }
						memset (&ftd, 0, sizeof (ftd)); for (vk = 0; vk < npp; ++vk) ftd.params [vk] = pp [vk]; ftd.nparams = (guint32) npp; ftd.ret = rv;
						/* `ftd` is a JIT `f`; `at`/`at_ne` below are AOT bodies built from the same pp[] and
						 * must NOT get the extra param. Adding it here rather than inside
						 * mono_wasm_get_call_info is what keeps those two apart. */
						for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &ftd)) { ftdi = ti_base + vk; break; }
						if (ftdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ftd; ftdi = ti_base + nextra++; }
						/* Adaptive speculative devirtualization. A perfectly-monomorphic interpreter profile
						 * can slim ANY ordinary virtual site whose target already owns an admitted f-slot.
						 * Terminal forwarding calls retain the stronger bottom-up behavior: if their target
						 * has not compiled yet, make it an island blocker and re-emit after publication.
						 * Ordinary compute methods never grow their island merely for speculation; a target
						 * not already present simply leaves that site on the compact PIC.
						 *
						 * Native-AOT and permanently un-JITtable targets retain the ordinary PIC/AOT miss
						 * routes. Registering a successful direct dependency makes the predicted target
						 * available in every worker before this caller is admitted there. */
#ifdef HOST_BROWSER
						/* Census only: which population this site lands in. Delegate sites never reach the
						 * gate below (`!is_delegate_invoke`), so they are counted here or they are invisible. */
						if (is_delegate_invoke)
							wj_count (WJC_DELEGATE_SITE);
						/* R215: resolve this delegate site's dominant target, if it clears the bar. Uses
						 * prof_predict_alt, which ranks by id_counts and ignores `margin` -- the only
						 * reader that can work for a delegate site at all. skip=NULL asks for the top
						 * identity rather than a runner-up. */
						/* MONO_WASM_JIT_DEVIRT_PROFILE ships 1 -- it is what records the receiver vtable seen
						 * at every virtual and delegate site, i.e. the input every devirt decision reads.
						 * With it off the emitter has nothing to speculate on at all. */
						if (is_delegate_invoke && mono_wasm_jit_delegate_devirt > 0) {
							/* The DELEGATE reader, not prof_predict_alt: that one requires id_targets[k],
							 * which is always NULL at a delegate site because the recorder passes target=NULL
							 * (the identity IS the target there). Using it measured 7,582 sites and 0 armed. */
							extern gboolean mono_wasm_jit_prof_predict_delegate (gpointer caller, MonoMethod *base,
								MonoMethod **out_target, guint32 *out_pct);
							extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
							extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
							MonoMethod *dt = NULL;
							guint32 dpct = 0;
							gboolean dgot = mono_wasm_jit_prof_predict_delegate (wj_call_prof_im (cfg, call),
								call->method, &dt, &dpct);
							ic_tag = 9;   /* W3 census: delegate site, 8 arm / 9 no record / 10 thin / 11 refused */
							if (!dgot)
								wj_count (WJC_DELEGATE_DEVIRT_NOREC);   /* no usable record -- NOT "thin" */
							else if (dgot && (int) dpct >= mono_wasm_jit_delegate_devirt &&
							    dt && dt != cfg->method && !mono_wasm_jit_callee_perm_unjittable (dt)) {
								int df = wj_lazy_arm_fslot (dt, cfg->method, &ftd);
								/* The direct call goes through the SAME functype as the indirect one it
								 * replaces, so a target whose lowered signature differs is a trap rather
								 * than a miss -- exactly as for the vtable arms. */
								if (df > 0 && wj_arm_abi_ok (dt, &ftd)) {
									wj_result_add_direct_dep (&cfg->wasm_jit_result, df,
										wj_functype_hash (&ftd), dt);
									if (cfg->wasm_jit_result.direct_deps_truncated) {
										fail = "too many direct dependencies (delegate devirt)";
										goto done;
									}
									ddevirt_target = dt;
									ddevirt_fslot = df;
									ic_tag = 8;
									wj_count (WJC_DELEGATE_DEVIRT_ARM);
								} else {
									/* REFUSED = NO_FSLOT + SIG. With a pool slot for any target that can take one
									 * (wj_lazy_arm_fslot), NO_FSLOT is a pool refusal; the arm is dropped, never
									 * forced (R366). */
									wj_count (WJC_DELEGATE_DEVIRT_REFUSED);
									ic_tag = 11;
									if (df <= 0)
										wj_count (WJC_DELEGATE_DEVIRT_NO_FSLOT);
									else
										wj_count (WJC_DELEGATE_DEVIRT_SIG);
								}
							} else if (dgot) {
								wj_count (WJC_DELEGATE_DEVIRT_THIN);   /* had a record, below the bar */
								ic_tag = 10;
							}
						}
						else {
							wj_count (WJC_DEVIRT_SITE);
						}
							/* THE DISAGREEMENT DETECTOR. Bumped ungated, because its whole job is to
							 * contradict WJC_REEMIT_COMPILED if the recompile never reaches the
							 * emitter. R179's headline bug was exactly that -- 31 re-emissions
							 * reported, 31 no-ops -- and it was found only because this counter,
							 * added for another purpose, read 0 while the other read 31. A
							 * re-emission arm with COMPILED > 0 and SITE == 0 is not a result. */
							if (G_UNLIKELY (mono_wasm_jit_reemit_inflight))
								mono_wasm_jit_counters [WJC_REEMIT_SITE]++;
						if (!is_delegate_invoke &&
						    /* MONO_WASM_JIT_VCALL_SLIM gated this and shipped 1: replace a perfectly
						     * monomorphic profiled site's whole PIC/AOT diamond with one guarded admitted
						     * f-slot call plus the shared cold miss. With it off, only the terminal
						     * vcall handoff site was offered to prediction at all. */
						    TRUE) {
							int pred_why = 0;
							guint32 pred_samples = 0;
							extern gboolean mono_wasm_jit_prof_predict_method (gpointer caller, MonoMethod *base,
								MonoVTable **out_vt, MonoMethod **out_target, guint32 *out_samples);
							gpointer pred_im = wj_call_prof_im (cfg, call);
							gboolean pred_ok = mono_wasm_jit_prof_predict (pred_im,
							    call->method, &pred_vt, &pred_target, &pred_samples, &pred_why);
							/* R311: a POLY site whose recorded receivers all resolve to ONE method is predictable
							 * behind the method-identity arm guard below. pred_why keeps the vtable verdict. */
							if (!pred_ok && mono_wasm_jit_pred_mid && vc_mid_slot >= 0 && (pred_why == 3 || pred_why == 4) &&
							    mono_wasm_jit_prof_predict_method (pred_im, call->method,
							                                       &pred_vt, &pred_target, &pred_samples)) {
								pred_ok = TRUE;
								wj_count (WJC_MID_PRED_ARM);
							}
							/* R405: a COLD verdict whose record saw ONE receiver (interp.c mono_wasm_jit_prof_predict_cold). */
							{ extern int mono_wasm_jit_t2_cold_pred;
							  extern gboolean mono_wasm_jit_prof_predict_cold (gpointer caller, MonoMethod *base,
								MonoVTable **out_vt, MonoMethod **out_target, guint32 *out_samples);
							  if (!pred_ok && pred_why == 2 && (mono_wasm_jit_t2_cold_pred & 2) && cfg->wasm_jit_tier >= 2 &&
							      mono_wasm_jit_prof_predict_cold (pred_im, call->method, &pred_vt, &pred_target, &pred_samples)) {
								pred_ok = TRUE;
								wj_count (WJC_T2_COLD_PRED_ARM);
							  } }
							{ extern int mono_wasm_jit_s3;   /* plan2x S3: no prediction, but nothing overrides the base */
							  gboolean _cand = FALSE;
							  if (!pred_ok && (mono_wasm_jit_s3 & 2) && (!(mono_wasm_jit_s3 & 8) || cfg->wasm_jit_tier >= 2)) {
								pred_cha = mono_wasm_jit_cha_final_word (call->method, &_cand);
								if (pred_cha) {
									pred_ok = TRUE; pred_vt = NULL; pred_target = call->method;
									wj_count (WJC_S3_ARM_PRED);
								} else if (_cand) {
									wj_count (WJC_S3_ARM_OVERRIDDEN);
								}
							  } }
							if (pred_ok) {
								extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
								extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
								extern gboolean mono_interp_jit_call_supported (MonoMethod *method,
									MonoMethodSignature *sig);
								MonoMethodSignature *pred_sig = mono_method_signature_internal (pred_target);
								WasmCallInfo pred_ci;
								mono_wasm_get_call_info (pred_sig, &pred_ci);
								if (m_class_is_valuetype (pred_target->klass)) {
									/* R376: the profile records a boxed receiver's VALUETYPE override (MINT_CALLVIRT_FAST
									 * records, then unboxes), and an arm would call it with the boxed `this`, so its
									 * fields read against the object header: LinkedList<T>.Enumerator.MoveNext threw
									 * "collection was modified" once System.Collections was no longer AOT'd. Its
									 * functype matches the site's, so the ABI test below cannot catch it. */
									wj_count (WJC_DEVIRT_VALUETYPE); ic_tag = 5;
									pred_vt = NULL;
									pred_target = NULL;
									pred_fslot = 0;
								} else if (!pred_ci.valid || pred_ci.vret_byaddr ||
								    !functype_eq (&pred_ci.ftype, &ftd)) {
									wj_count (WJC_DEVIRT_SIG); ic_tag = 5;
									pred_vt = NULL;
									pred_target = NULL;
									pred_fslot = 0;
								} else {
									pred_fslot = wj_lazy_arm_fslot (pred_target, cfg->method, &ftd);
									if (pred_fslot <= 0) {
										/* The target could not take a pool slot (wj_lazy_arm_fslot): no arm. The
										 * terminal-handoff and DEVIRT_FORCE bails that forced the target through an
										 * island here are gone with the islands (R366). */
										wj_count (WJC_DEVIRT_NO_FSLOT); WJ_REEMIT_SCOPED (WJC_REEMIT_NO_FSLOT); ic_tag = 6;
										pred_vt = NULL;
										pred_target = NULL;
										pred_fslot = 0;
									} else {
										wj_result_add_direct_dep (&cfg->wasm_jit_result, pred_fslot,
											wj_functype_hash (&ftd), pred_target);
										if (cfg->wasm_jit_result.direct_deps_truncated) {
											fail = "too many direct dependencies";
											goto done;
										}
										/* SAME POLARITY TRAP as wj_ins_is_pinned_vcall_forward above: this read
										 * `slim_pred = mono_wasm_jit_vcall_shared_miss_enabled`, and that knob
										 * ships **0**, so a slim predicted site was never selected here. It
										 * gates the AOT-IC arm below (`!slim_pred && ...`), so flipping it to
										 * TRUE would silently disable that arm. Substitute the SHIPPED VALUE. */
										slim_pred = FALSE;
									}
								}
							} else {
								/* WHY the profile refused. These four are the whole diagnosis: NO_REC and
								 * COLD say the observer never got enough of a look (a HIGHER JIT threshold
								 * would help), POLY/POLY_90 say it looked and the site is not perfectly
								 * monomorphic (a higher threshold makes those strictly WORSE, since a
								 * second receiver disqualifies a site permanently). They pull in opposite
								 * directions, so the split decides the fix. */
								ic_tag = pred_why;
								switch (pred_why) {
								case 1 /* WJ_PRED_NO_REC */:  wj_count (WJC_DEVIRT_NO_REC); WJ_REEMIT_SCOPED (WJC_REEMIT_NO_REC); break;
								case 2 /* WJ_PRED_COLD */:    wj_count (WJC_DEVIRT_COLD); break;
								case 3 /* WJ_PRED_POLY */:    wj_count (WJC_DEVIRT_POLY); break;
								case 4 /* WJ_PRED_POLY_90 */: wj_count (WJC_DEVIRT_POLY); wj_count (WJC_DEVIRT_POLY_90); break;
								default: break;
								}
							}
						}
#endif
						/* Open-static/open-instance delegate targets consume the Invoke arguments after
						 * dropping the delegate receiver. Their canonical f-thunk type is therefore ftd
						 * without parameter zero. Closed-instance/bound-static retain the full ftd. */
						if (is_delegate_invoke) {
							if (npp < 1) { fail = "delegate no receiver"; goto done; }
							memset (&dftd, 0, sizeof (dftd));
							for (vk = 1; vk < npp; ++vk) dftd.params [vk - 1] = pp [vk];
							dftd.nparams = (guint32) (npp - 1); dftd.ret = rv;
							for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &dftd)) { dftdi = ti_base + vk; break; }
							if (dftdi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = dftd; dftdi = ti_base + nextra++; }
						}
						uses_calls = TRUE;
						n2 = csig->param_count + 1; /* this + params */
						n2c = n2 + (vc_vret ? 1 : 0);
						/* per-call-site AOT-vcall IC cell (VCALL_AOT_IC): 20B, see mono_wasm_jit_alloc_aot_ic */
						gpointer aic = NULL;
						{
						  /* Delegate wrapper selection depends on the instance's target shape, not just its vtable.
						   * A vtable-keyed AOT IC could therefore reuse a normal wrapper for an open-virtual or bound
						   * delegate of the same delegate type. Leave those sites on the instance-aware helper path. */
						  /* MONO_WASM_JIT_VCALL_AOT_IC gated this and shipped 1: a per-call-site inline
						   * cache for AOT-backed virtual targets, skipping scratch() + resolve on a hit. */
						  /* MONO_WASM_JIT_VCALL_AOT gated this and shipped 1: when a JITted method's virtual
						   * call resolves to an AOT-backed override, dispatch it through the AOT body's own
						   * native ABI instead of the interp residual. Its off-arm sent every such call
						   * through ~1,360 instructions of marshalling. Settled. */
						  if (!slim_pred && !is_delegate_invoke && !vc_vret) {
#ifdef HOST_BROWSER
							extern gpointer mono_wasm_jit_alloc_aot_ic (void); aic = mono_wasm_jit_alloc_aot_ic ();
#else
							aic = (gpointer) (intptr_t) 0x7fe0;
#endif
						  } }
						if (aic) {
							/* register AOT-body functypes up front for the inline AOT-IC: at (this,args,rgctx)->rv, at_ne (this,args)->rv */
							WasmFuncType at, at_ne;
							if (n2 + 1 <= WASM_FUNCTYPE_MAX_PARAMS) {
								memset (&at, 0, sizeof (at)); for (vk = 0; vk < n2; ++vk) at.params [vk] = pp [vk]; at.params [n2] = WASM_I32; at.nparams = (guint32) (n2 + 1); at.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { aic_ati = ti_base + vk; break; }
								if (aic_ati < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at; aic_ati = ti_base + nextra++; }
								memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { aic_ati_ne = ti_base + vk; break; }
								if (aic_ati_ne < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = at_ne; aic_ati_ne = ti_base + nextra++; }
							}
							if (aic_ati < 0 || aic_ati_ne < 0) aic = NULL;   /* functype table full -> skip AOT-IC (safe) */
						}
						/* Per-call-site N-way virtual IC plus miss metadata in shared memory. Delegate.Invoke
						 * sites append a single-cast dispatch recipe used by the instance-aware helper. */
#ifdef HOST_BROWSER
						{ extern gpointer mono_wasm_jit_alloc_ic (int delegate_site, MonoMethod *caller, MonoMethod *base);
						  vic = mono_wasm_jit_alloc_ic (is_delegate_invoke, cfg->method, call->method); }
#else
						vic = (gpointer) (intptr_t) 0x7ff0;
#endif
						/* RECEIVER NULL CHECK — materialize the implicit null check a callvirt relies on. On real
						 * hardware the vtable load from a null `this` faults; wasm linear-memory address 0 is a
						 * valid address, so the load silently reads garbage. The interp does this explicitly too
						 * (MINT_CALLVIRT_FAST: NULL_CHECK(this_arg) before resolving). Without it a null receiver
						 * flows into mono_wasm_jit_vcall_resolve_fslot, where mono_object_get_virtual_method_internal
						 * returns NULL and the NULL override is dereferenced -> mono_marshal_get_synchronized_wrapper(NULL)
						 * aborts (marshal.c g_assert(method)). On null, raise a catchable NullReferenceException
						 * (exc_id 4) and bail, exactly like OP_COND_EXC. */
						/* NCE: method_to_ir already emitted an IR-level check for this same receiver on
						 * nearly every callvirt, so this block is usually a pure duplicate of a test
						 * that ran a few instructions ago. Skip it when the receiver is already proven;
						 * otherwise emit it and record the proof for the rest of the bb. */
						if (NN_GET (this_vr)) goto vcall_nullchk_done;
						NN_SET (this_vr);
						{
							if (!WJ_HAS_THROW (4)) { fail = "no shared throw block for NRE"; goto done; }
							if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall nullchk this"; goto done; }
							wasm_op (&body, WASM_OP_I32_EQZ);
							WJ_THROW_BR (4, 0);
						}
vcall_nullchk_done:
						wasm_origin_push (&body, WJO_VCALL_PIC);   /* the rest of this ins: PIC, miss path, call */
						/* --- WORKER-LOCAL INLINE VCALL PIC ---
						 * A stable site id selects this worker's compact {receiver-vtable, admitted-fslot}
						 * entries through imported TLS pointer/capacity addresses. Because a miss publishes only
						 * after admitting the target into this worker's function table, a hit needs neither the old
						 * shared InterpMethod load nor a slot-liveness bitmap test. Way zero remains straight-line;
						 * the remaining ways use one compact loop and all hits share one typed call tail. */
						{
#ifdef HOST_BROWSER
							extern int mono_wasm_jit_imethod_fslot_off (void);
							int fslot_off = mono_wasm_jit_imethod_fslot_off ();
							extern guint32 mono_wasm_jit_vcall_pic_site_id (gpointer ic);
							extern int mono_wasm_jit_vcall_pic_stride (void);
							extern int mono_wasm_jit_vcall_pic_midvt_off (void);
							int vpic_midvt_off = mono_wasm_jit_vcall_pic_midvt_off ();
							guint32 vpic_site = mono_wasm_jit_vcall_pic_site_id (vic);
							int vpic_stride = mono_wasm_jit_vcall_pic_stride ();
							extern gpointer mono_wasm_jit_delegate_ic_base (gpointer ic);
							extern int mono_wasm_jit_delegate_ic_stride (void);
							extern int mono_wasm_jit_delegate_ic_field_off (int field);
							extern int mono_wasm_jit_delegate_field_off (int field);
							gpointer delegate_ic = is_delegate_invoke ? mono_wasm_jit_delegate_ic_base (vic) : NULL;
							int delegate_ic_stride = mono_wasm_jit_delegate_ic_stride ();
							int dic_seq_off = mono_wasm_jit_delegate_ic_field_off (0);
							int dic_source_off = mono_wasm_jit_delegate_ic_field_off (1);
							int dic_receiver_off = mono_wasm_jit_delegate_ic_field_off (2);
							int dic_imethod_off = mono_wasm_jit_delegate_ic_field_off (3);
							int dic_shape_off = mono_wasm_jit_delegate_ic_field_off (4);
							int dic_scalar_off = mono_wasm_jit_delegate_ic_field_off (5);
							int delegate_target_off = mono_wasm_jit_delegate_field_off (0);
							int delegate_method_off = mono_wasm_jit_delegate_field_off (1);
							int delegate_list_off = mono_wasm_jit_delegate_field_off (2);
							int delegate_info_off = mono_wasm_jit_delegate_field_off (3);
							int dti_recipe_off = mono_wasm_jit_delegate_field_off (4);
							extern int mono_wasm_jit_delegate_pic_stride (void);
							int dpic_stride = mono_wasm_jit_delegate_pic_stride ();
#else
							int fslot_off = 0x40; /* placeholder for offline encoder validation (real offset only matters at runtime) */
							guint32 vpic_site = 0;
							int vpic_stride = 24;
							int vpic_midvt_off = 16;   /* offline encoder validation only; the runtime asks interp.c */
							gpointer delegate_ic = (gpointer) (intptr_t) 0x7000;
							int delegate_ic_stride = 32, dic_seq_off = 0, dic_source_off = 4;
							int dic_receiver_off = 8, dic_imethod_off = 16, dic_shape_off = 20, dic_scalar_off = 28;
							int delegate_target_off = 16, delegate_method_off = 20, delegate_list_off = 64;
							int delegate_info_off = 32, dti_recipe_off = 44;
							int dpic_stride = 16;
#endif
							/* Worker-local delegate PIC field offsets, mirroring WjLocalDelegatePicEntry. The way
							 * offset folds into the load immediate, so a way costs no base arithmetic at all. */
							/* DELEGATE_OBJ_PIC was the alternative to this arm and is deleted; `obj_pic` is kept
							 * as a compile-time 0 so the block structure below (its br depths and trailing END)
							 * stays exactly as measured rather than being re-derived by hand. */
							const int obj_pic = 0;
							/* MONO_WASM_JIT_DELEGATE_LOCAL_PIC shipped 1 and its shared-IC arm is deleted.
							 * Kept as a local rather than folded away because it gates the hoisted
							 * prologue, the br depth of the tail and the trailing END -- the block
							 * structure below is exactly as measured. */
							const int dpic_local = 1;
							int dpic_rvt_off = 8, dpic_shape_off = 12;
							int way;
							/* Inline-cache width for THIS site, from the interpreter's receiver observations, in
							 * place of the global MONO_WASM_JIT_VCALL_WAYS everywhere. A site the interpreter only
							 * ever saw one receiver at does not need the ways 1..N-1 cold-scan loop, and that loop
							 * is not free: it is a block, a loop, bounds arithmetic and a call_indirect emitted
							 * into every virtual call site in every method, whether or not the receiver is ever
							 * polymorphic.
							 *
							 * Narrowing is safe in the only direction that matters: the emitted code re-checks the
							 * cached vtable on every call, so an under-sized site costs IC misses, never a wrong
							 * target. mono_wasm_jit_prof_arity returns 0 unless the site is warm (>= 8 samples)
							 * and its receiver set did not overflow, so "no data" and "too many types" both keep
							 * the configured width.
							 *
							 * The PIC array layout is deliberately NOT narrowed: its stride stays at the global
							 * width so the addressing baked into emitted code and the runtime's fill path continue
							 * to agree. A narrow site simply leaves its upper slots unused.
							 *
							 * Delegate.Invoke sites are sized from the same number, fed by a separate recorder in
							 * MINT_CALL_DELEGATE (wj_prof_record_delegate) that counts distinct delegate TARGETS
							 * rather than receiver vtables, because that is what the delegate IC's ways
							 * discriminate on. That is the population that matters for MethodHandle stubs: a
							 * MethodHandle invoke is a delegate invoke, so those stubs carry the unrolled
							 * VCALL_WAYS-deep chain below and it dominates their emitted size. */
							int ic_ways = mono_wasm_jit_vcall_ways;
#ifdef HOST_BROWSER
							/* MONO_WASM_JIT_IC_AUTOSIZE gated this and shipped 1: narrow a site's inline-cache
							 * width to what the interpreter actually observed there, rather than using the
							 * global VCALL_WAYS everywhere. The cold-scan loop it removes is a block, a
							 * loop, bounds arithmetic and a call_indirect emitted into EVERY virtual call
							 * site, and narrowing is safe in the only direction that matters because the
							 * emitted code re-checks the receiver. Settled; unconditional. */
							if (ic_ways > 1) {
								extern guint32 mono_wasm_jit_prof_arity (gpointer caller, MonoMethod *base);
								guint32 ar = mono_wasm_jit_prof_arity (wj_call_prof_im (cfg, call), call->method);
								/* arity + 1, not arity. The profile now also observes this site AFTER the caller is
								 * compiled (the IC-miss path feeds it), but the width is fixed at THIS emit, so it
								 * still cannot see receivers that first appear later -- and the counters say that is exactly what happens: the
								 * vcall IC miss rate is 9.0% inside the in-game window against 2.83% run
								 * cumulative, i.e. misses concentrate after compilation. One spare way absorbs
								 * the common "one new type shows up later" case; a miss costs a trip through
								 * mono_wasm_jit_vcall_resolve_fslot, which is 1781 instructions and the single
								 * hottest symbol in the profile at 2.36%, so the spare guard is far cheaper
								 * than the miss it prevents. */
								/* Exactly the observed arity, NOT arity+1. The spare-way idea was tried and
								 * MEASURED as a loss: it improved the true miss rate 1.178% -> ~0.95% but cost
								 * ~34% of in-game fps (13.4 -> 8.9, tight 1.8% spread over two clean runs),
								 * because an extra way adds a guard to EVERY dispatch -- ~350M of them per
								 * in-game window -- to avoid misses that are only ~1% of that traffic. Paying a
								 * ubiquitous cost for a rare saving is the wrong direction, however cheap the
								 * guard looks in isolation. */
								if (ar > 0 && (int) ar < ic_ways)
									ic_ways = (int) ar;
							}
#endif
							/* R206: resolve the runner-up, if arm 1 exists and the knob is on. Deliberately
							 * AFTER every devirt exit above, so pred_vt/pred_target are final. The target comes
							 * from the profile's id_targets[], recorded by the OBSERVER -- resolving it here via
							 * mono_class_get_virtual_method is the documented deadlock (class init -> IKVM
							 * classloader -> synchronous cross-thread JS call under the compile lock). */
#ifdef HOST_BROWSER
							/* R206b: ARMS AT A POLYMORPHIC SITE.
							 *
							 * The first version hung this off a site that ALREADY had arm 1 and measured
							 * 17,096 gate hits, 100% no_alt, zero arms. That was structural, not a bug:
							 * arm 1 exists only when prof_predict succeeded, which requires
							 * `margin == total` -- every observation ever had the SAME receiver -- so such a
							 * site has exactly one identity in the profile and no runner-up to name. The
							 * alternate receiver that FastVicPred counts (32.5% of hot IC traffic) appears
							 * only AFTER emission, and is caught by slim_pred's worker-local PIC way, which
							 * dispatches it as call_indirect rather than a direct call.
							 *
							 * The sites whose receiver set IS known at emit time are the POLY ones -- refused
							 * precisely because they saw more than one -- and they carry 30.7% of hot IC
							 * volume. `margin == total` is irreversible, so they are refused an arm for the
							 * life of the process however concentrated their distribution is. id_counts[]
							 * (R206) gives the true per-receiver frequency the margin cannot express, so the
							 * break-even can be applied per receiver. ic_tag 3/4 == WJ_PRED_POLY/POLY_90. */
							/* MONO_WASM_JIT_DEVIRT_ARM2 ships 1 and is measured: +18.7%% direct-call sites (local
							 * arms per 1k modules 461.2 -> 547.4, non-overlapping ranges), 9.1M in-window
							 * hits, poly's share of IC hits 29.0%% -> 12.1%%. It exists because
							 * `margin == total` is IRREVERSIBLE -- a site that ever saw a second receiver is
							 * refused an arm for the life of the process, a 90/10 site identically to a
							 * 50/50 one. ARM2_PCT stays a knob: refusing sites below the break-even is what
							 * keeps this from repeating the vcall_ways regression. */
							if (!pred_target && (ic_tag == 3 || ic_tag == 4) &&
							    !is_delegate_invoke) {
								extern gboolean mono_wasm_jit_prof_predict_alt (gpointer caller, MonoMethod *base,
									MonoVTable *skip, MonoVTable **out_vt, MonoMethod **out_target, guint32 *out_pct);
								extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
								extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
								gpointer im = wj_call_prof_im (cfg, call);
								MonoVTable *v1 = NULL, *v2 = NULL;
								MonoMethod *t1 = NULL, *t2 = NULL;
								guint32 p1 = 0, p2 = 0;
								int f1, f2;
								/* Shared admission test. A candidate must have an f-slot, not be this method,
								 * be jittable, and carry EXACTLY the call's functype -- both arms and the IC
								 * share one ftdi and one argument sequence, so a divergent signature is a trap
								 * rather than a miss. */
								#define WJ_ARM_OK(vt, t, pct, fs) ( \
									(vt) && (t) && (int) (pct) >= mono_wasm_jit_devirt_arm2_pct && \
									(t) != cfg->method && !mono_wasm_jit_callee_perm_unjittable (t) && \
									((fs) = wj_lazy_arm_fslot ((t), cfg->method, &ftd)) > 0 && \
									wj_arm_abi_ok ((t), &ftd))
								/* NOTE the f-slot test above is not merely "is it compiled": it is what makes the
								 * direct-dep registration below meaningful, since a dep is keyed by slot. */

								if (mono_wasm_jit_prof_predict_alt (im, call->method, NULL, &v1, &t1, &p1)) {
									if (WJ_ARM_OK (v1, t1, p1, f1)) {
										/* REGISTER THE DIRECT DEPENDENCY. Baking an f-slot without this is the
										 * prefilled-placeholder trap: admission is what brings the target live on
										 * a worker before this caller runs, and a slot this worker never
										 * instantiated still holds mono_jiterp_placeholder_jit_call, which traps
										 * on a mismatched signature and silently corrupts on a matching one.
										 * Omitting it is what made the first poly-arm build fault x4. */
										wj_result_add_direct_dep (&cfg->wasm_jit_result, f1,
											wj_functype_hash (&ftd), t1);
										if (cfg->wasm_jit_result.direct_deps_truncated) {
											fail = "too many direct dependencies (poly arm1)";
											goto done;
										}
										pred_vt = v1; pred_target = t1; pred_fslot = f1;
										wj_count (WJC_DEVIRT_POLY_ARM1);
										/* and now the runner-up, on the traffic arm 1 leaves behind */
										if (mono_wasm_jit_prof_predict_alt (im, call->method, v1, &v2, &t2, &p2)) {
											if (WJ_ARM_OK (v2, t2, p2, f2)) {
												wj_result_add_direct_dep (&cfg->wasm_jit_result, f2,
													wj_functype_hash (&ftd), t2);
												if (cfg->wasm_jit_result.direct_deps_truncated) {
													fail = "too many direct dependencies (poly arm2)";
													goto done;
												}
												pred2_vt = v2; pred2_target = t2; pred2_fslot = f2;
												wj_count (WJC_DEVIRT_ARM2_EMITTED);
											} else {
												if (!v2 || !t2) wj_count (WJC_DEVIRT_ARM2_NO_ALT);
												else if ((int) p2 < mono_wasm_jit_devirt_arm2_pct) wj_count (WJC_DEVIRT_ARM2_THIN);
												else if (t2 == cfg->method) wj_count (WJC_DEVIRT_ARM2_SELF);
												else if (mono_wasm_jit_callee_perm_unjittable (t2)) wj_count (WJC_ARM_UNJITTABLE);
												else {
													wj_count (WJC_DEVIRT_ARM2_NO_FSLOT);   /* by elimination: a pool refusal (R366) */
												}
											}
										} else {
											wj_count (WJC_DEVIRT_ARM2_NO_ALT);
										}
									} else {
										/* ATTRIBUTE the refusal by re-testing the same predicates -- but only the
										 * PURE ones. Nothing on the success path changes.
										 *
										 * The final bucket is no_fslot BY ELIMINATION, not by test: the build that
										 * added the ABI split measured invalid=0 byaddr=0 shape=0, so anything
										 * reaching the ABI check passes it, and the only remaining way to fail the
										 * macro is `get_callee_fslot <= 0`.
										 *
										 * Do NOT call get_callee_fslot here to test it directly. It routes through
										 * mono_interp_get_imethod, which CREATES an InterpMethod if absent -- a
										 * metadata operation under the jit-mm lock, on a worker, inside the compile
										 * section, for callees that are being REFUSED and would otherwise not get
										 * one at this moment. Two builds that did call it (once inside a helper,
										 * once here) produced four consecutive failures: two `memory access out of
										 * bounds`, one wedge, and one wedge with `function signature mismatch` x4.
										 * The predicates kept below -- pointer compares and a flag read -- are pure. */
										if (!v1 || !t1) wj_count (WJC_DEVIRT_ARM2_NO_ALT);
										else if ((int) p1 < mono_wasm_jit_devirt_arm2_pct) wj_count (WJC_DEVIRT_ARM2_THIN);
										else if (t1 == cfg->method) wj_count (WJC_DEVIRT_ARM2_SELF);
										else if (mono_wasm_jit_callee_perm_unjittable (t1)) wj_count (WJC_ARM_UNJITTABLE);
										else wj_count (WJC_DEVIRT_ARM2_NO_FSLOT);   /* by elimination -- see below */
									}
								} else {
									wj_count (WJC_DEVIRT_ARM2_NO_ALT);
								}
								#undef WJ_ARM_OK
							}
#endif /* HOST_BROWSER */
							if (0 && pred_fslot > 0 && pred_vt && pred_target &&
							    !is_delegate_invoke) {
								extern gboolean mono_wasm_jit_prof_predict_alt (gpointer caller, MonoMethod *base,
									MonoVTable *skip, MonoVTable **out_vt, MonoMethod **out_target, guint32 *out_pct);
								extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
								MonoVTable *a2vt = NULL;
								MonoMethod *a2t = NULL;
								guint32 a2pct = 0;
								if (mono_wasm_jit_prof_predict_alt (cfg->wasm_jit_caller_imethod,
								    call->method, pred_vt, &a2vt, &a2t, &a2pct)) {
									if ((int) a2pct < mono_wasm_jit_devirt_arm2_pct) {
										wj_count (WJC_DEVIRT_ARM2_THIN);   /* below break-even; an arm here LOSES */
									} else {
										int f2 = mono_wasm_jit_get_callee_fslot (a2t);
										if (f2 <= 0) {
											wj_count (WJC_DEVIRT_ARM2_NO_FSLOT);
										} else if (a2t == cfg->method) {
											wj_count (WJC_DEVIRT_ARM2_SELF);
										} else {
											/* The ABI must match arm 1's: both arms and the IC share one functype
											 * index (ftdi) and one argument marshalling sequence. A runner-up whose
											 * signature differs is not substitutable -- that is a trap, not a miss. */
											MonoMethodSignature *s2 = mono_method_signature_internal (a2t);
											WasmCallInfo ci2;
											mono_wasm_get_call_info (s2, &ci2);
											if (ci2.valid && !ci2.vret_byaddr && functype_eq (&ci2.ftype, &ftd)) {
												pred2_vt = a2vt; pred2_target = a2t; pred2_fslot = f2;
												wj_count (WJC_DEVIRT_ARM2_EMITTED);
											} else {
												wj_count (WJC_DEVIRT_ARM2_SIG);
											}
										}
									}
								} else {
									wj_count (WJC_DEVIRT_ARM2_NO_ALT);
								}
							}
							if (!is_delegate_invoke) wj_emit_fast_count (&body, WJC_FAST_VSITE);   /* C1 route mix: denominator */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $after (void) */
							/* Strict monomorphic guard. A hit bypasses the worker PIC completely. The prediction
							 * is deliberately speculative: interpreter warmup can see one receiver before the
							 * steady-state workload introduces another. Adaptive slim therefore retains one
							 * worker-local alternate way below. It costs one bounds check + one i64 guard on a
							 * prediction miss, but turns the second and later calls of the common alternate into
							 * direct f-slot dispatch instead of repeatedly entering the shared resolver. */
							if (pred_fslot > 0 && (pred_vt || (pred_cha && pred_target == call->method)) && pred_target && !is_delegate_invoke) {
								wj_count (WJC_DEVIRT_EMITTED); WJ_REEMIT_SCOPED (WJC_REEMIT_EMITTED);
								ic_tag = 7;   /* R205: any IC hit below this guard is the ALTERNATE receiver */
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $pred_miss */
								if (pred_cha && !pred_vt) {
									/* plan2x S3: CHA ARM GUARD. One load from a constant address: the base's word, set
									 * when a class overriding it is built (before any instance of that class exists). */
									extern int mono_wasm_jit_s3;
									extern gint32 mono_wasm_jit_cha_wrong;
									wj_count (WJC_S3_ARM_EMITTED);
									wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (gsize) pred_cha);
									WJ_HINT2 (0, WJC_BRANCH_HINT_IC);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* overridden -> $pred_miss */
									if ((mono_wasm_jit_s3 & 4) && vc_mid_slot >= 0) {
										/* verify: receiver->vtable->klass->vtable [slot] must BE the target */
										guint32 _wa = (guint32) (gsize) &mono_wasm_jit_cha_wrong;
										wj_count (WJC_S3_VERIFY_SITES);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "cha verify this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
										wj_emit_class_slot_method (&body, aic_vtab_idx, vc_class_vtable_off, vc_mid_slot);
										wasm_i32_const (&body, (gint32) (intptr_t) pred_target);
										wasm_op (&body, WASM_OP_I32_NE);
										wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
											wasm_i32_const (&body, 0);
											wasm_i32_const (&body, 0); wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _wa);
											wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
											wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _wa);
											{   /* name the mismatch (plan2x/patch_s3d): (base, receiver vtable) */
												extern void mono_wasm_jit_cha_wrong_note (MonoMethod *base, MonoVTable *vt);
												WasmFuncType pw; int pwi = -1, pwk;
												memset (&pw, 0, sizeof (pw)); pw.ret = WASM_VOID; pw.nparams = 2; pw.params [0] = WASM_I32; pw.params [1] = WASM_I32;
												for (pwk = 0; pwk < nextra; ++pwk) if (functype_eq (&extra_types [pwk], &pw)) { pwi = ti_base + pwk; break; }
												if (pwi < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = pw; pwi = ti_base + nextra++; }
												ENSURE_REF_FRAME ();
												wasm_i32_const (&body, (gint32) (intptr_t) pred_target);
												wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
												wj_emit_helper_call (&body, (gpointer) &mono_wasm_jit_cha_wrong_note, (guint32) pwi);
												RELEASE_LAZY_REF_FRAME ();
											}
											wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* -> $pred_miss */
										wasm_op (&body, WASM_OP_END);
									}
								} else {
									{ extern int mono_wasm_jit_f4;   /* plan2x F4 & 2 */
									  int _irvt = (mono_wasm_jit_f4 & 2) ? wj_ir_vtable_vreg (ins, this_vr) : -1;
									  if (_irvt >= 0) {
										if (!wasm_ld (&body, &lc, _irvt)) { fail = "devirt irvt ld"; goto done; }
										wj_count (WJC_F4_VT_REUSE);
									  } else {
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "devirt this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									  } }
									if (mono_wasm_jit_pred_mid && vc_mid_slot >= 0) {
										/* R311 METHOD-IDENTITY ARM GUARD. The predicted receiver pays the one
										 * compare it always did; any other receiver whose class holds pred_target
										 * in this vtable slot (inherits it) takes the arm instead of the IC. */
										wj_count (WJC_MID_ARM);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $pred_hit */
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_i32_const (&body, (gint32) (intptr_t) pred_vt);
											wasm_op (&body, WASM_OP_I32_EQ);
											wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* -> $pred_hit */
											{ extern int mono_wasm_jit_f4;
											  /* plan2x F4 & 8: the last other vtable that matched pred_target by identity. A
											   * process-wide word is right here: "this vtable's class holds pred_target in
											   * this slot" is a fact about the class, and pred_target is baked into the arm. */
											  gint32 *_memo = (mono_wasm_jit_f4 & 8) ? wj_word_alloc () : NULL;
											  guint32 _ma = (guint32) (gsize) _memo;
											  if (_memo) {
												wj_count (WJC_F4_ARM_MEMO);
												wasm_i32_const (&body, 0);
												wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _ma);
												wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
												wasm_op (&body, WASM_OP_I32_EQ);
												wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* -> $pred_hit */
											  }
											wj_emit_class_slot_method (&body, aic_vtab_idx, vc_class_vtable_off, vc_mid_slot);
											wasm_i32_const (&body, (gint32) (intptr_t) pred_target);
											wasm_op (&body, WASM_OP_I32_NE);
											WJ_HINT2 (0, WJC_BRANCH_HINT_IC);   /* R341 */
											wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1);   /* -> $pred_miss */
											  if (_memo) {
												wasm_i32_const (&body, 0);
												wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
												wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _ma);
											  } }
										wasm_op (&body, WASM_OP_END); /* $pred_hit */
									} else {
										wasm_i32_const (&body, (gint32) (intptr_t) pred_vt);
										wasm_op (&body, WASM_OP_I32_NE);
										WJ_HINT2 (0, WJC_BRANCH_HINT_IC);   /* R341 */
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									}
								}   /* plan2x S3: end of the non-CHA guard */
									/* The predicted arm was the ONE dispatch route with no execution counter,
									 * so every "share of vcalls" figure so far has silently omitted it. */
									wj_emit_fast_count (&body, WJC_FAST_DEVIRT);
									for (ai = 0; ai < n2c; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "devirt arg ld"; goto done; }
									/* Same single hole as an ordinary direct call. The prediction is already
									 * guarded (this->vtable == pred_vt), so if the assembler can co-locate
									 * pred_target this becomes a `call <funcidx>` that V8 may inline -- a VIRTUAL
									 * call inlined behind a guard, which is the thing mono itself cannot do
									 * (method-to-ir.c:8199 admits a candidate only when the site or the target is
									 * non-virtual, or the target is final). */
									wj_emit_method_call (&body, pred_target, pred_fslot, (guint32) ftdi);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "devirt dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> outer $after */
								wasm_op (&body, WASM_OP_END); /* $pred_miss */
							}
							/* R206: SECOND guarded arm, structurally identical to the first. Its guard is only
							 * reached when arm 1's missed, which is exactly the traffic the break-even is
							 * computed over. Same ftdi, same argument sequence, same single WASM_RELOC_CALL hole,
							 * so it is co-locatable and shadowable on the same terms as arm 1 -- which it must be,
							 * because at ~50% break-even a non-co-located arm 2 loses. */
							if (pred2_fslot > 0 && pred2_vt && pred2_target) {
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $pred2_miss */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "devirt2 this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_i32_const (&body, (gint32) (intptr_t) pred2_vt);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wj_emit_fast_count (&body, WJC_FAST_DEVIRT2);
									for (ai = 0; ai < n2c; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "devirt2 arg ld"; goto done; }
									wj_emit_method_call (&body, pred2_target, pred2_fslot, (guint32) ftdi);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "devirt2 dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> outer $after */
								wasm_op (&body, WASM_OP_END); /* $pred2_miss */
							}
#ifdef HOST_BROWSER
							/* plan typed-spindle C1 (MONO_WASM_JIT_VSITE_COMPACT, mini-wasm-vslow.inc): keep the devirt arms
							 * above and way 0 of the worker PIC inline; every way-0 miss goes to the shared slow-path stub
							 * for this functype, which runs what the code below would have emitted here -- slim, method
							 * identity, F9, ways 1..N, the inline AOT IC and the per-site cold miss (450-600 B a site, R455).
							 * Not for delegate sites (their recipe IC and shape split stay inline), hidden-vret sites, or
							 * objguard builds; a site whose stub cannot be made keeps the full form. */
							{ extern int mono_wasm_jit_vsite_compact;
							  if (mono_wasm_jit_vsite_compact && !is_delegate_invoke && !vc_vret && !lc.objguard && npp == n2 &&
							      n2 >= 1 && n2 + 1 <= WASM_FUNCTYPE_MAX_PARAMS) {
								int retnorm = 0, vs_slot, vs_ti = -1;
								WasmFuncType vst;
								if (rv == WASM_I32 && !m_type_is_byref (csig->ret)) {
									switch (mini_get_underlying_type (csig->ret)->type) {
									case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: retnorm = 1; break;
									case MONO_TYPE_I1: retnorm = 2; break;
									case MONO_TYPE_CHAR: case MONO_TYPE_U2: retnorm = 3; break;
									case MONO_TYPE_I2: retnorm = 4; break;
									default: break;
									}
								}
								memset (&vst, 0, sizeof (vst));
								for (vk = 0; vk < n2; ++vk) vst.params [vk] = pp [vk];
								vst.params [n2] = WASM_I64;   /* the descriptor: an i64 can never match the placeholder's type */
								vst.nparams = (guint32) (n2 + 1); vst.ret = rv;
								for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &vst)) { vs_ti = ti_base + vk; break; }
								if (vs_ti < 0 && nextra < WJ_EXTRA_TYPES_MAX) { extra_types [nextra] = vst; vs_ti = ti_base + nextra++; }
								vs_slot = vs_ti >= 0 ? wj_vslow_slot (&ftd, retnorm) : 0;
								if (vs_slot > 0) {
									WjVslowDesc *vsd = wj_vslow_desc (vic, aic, call->method, (int) vpic_site, mono_wasm_jit_vcall_ways,
										(vc_mid_slot >= 0 && mono_wasm_jit_ic_mid) ? vc_mid_slot : -1);
									wj_count (WJC_VSLOW_SITES);
									/* $cslow: a br out of it calls the stub. $chit: a br out of it holds an admitted f-slot in vc_fslot. */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $cslow */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $chit */
									{ extern int mono_wasm_jit_f4;
									  int _irvt = (mono_wasm_jit_f4 & 2) ? wj_ir_vtable_vreg (ins, this_vr) : -1;
									  if (_irvt >= 0) {
										if (!wasm_ld (&body, &lc, _irvt)) { fail = "vslow irvt ld"; goto done; }
									  } else {
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "vslow this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									  } }
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
									wasm_i32_const (&body, (gint32) vpic_site);
									EMIT_IC_VPIC_CAPVAL ();
									wasm_op (&body, WASM_OP_I32_GE_U);
									WJ_HINT2 (0, WJC_BRANCH_HINT_IC);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1);   /* no PIC yet -> $cslow */
									EMIT_IC_VPIC_PTR ();
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);   /* the PIC array base */
									/* way 0 */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride));
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op (&body, WASM_OP_I32_EQ);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
										wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
										wj_emit_fast_count (&body, WJC_FAST_VIC_W0);
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* -> $chit */
									wasm_op (&body, WASM_OP_END);
									/* plan2x F9 (vtable, base) -> f-slot, inline as in the full form: s1's route mix (typed-spindle,
									 * 2026-10-04) put F9 + ways 1..N at 15.6% of executed virtual dispatch, too much to send to a call.
									 * Ways 1..N, method identity, the AOT IC and the cold miss go to the stub. */
									{ extern int mono_wasm_jit_f9, mono_wasm_jit_mcache_prefix (void);
									  MonoMethod *_b = call->method;
									  int _mcp = ((mono_wasm_jit_f9 & 1) && _b && !_b->is_inflated && !_b->is_generic &&
									              !mono_class_is_ginst (_b->klass) && !mono_class_is_gtd (_b->klass)) ? mono_wasm_jit_mcache_prefix () : 0;
									  if (_mcp > 0) {
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHR_U);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_i32_const (&body, 11); wasm_op (&body, WASM_OP_I32_SHR_U);
										wasm_op (&body, WASM_OP_I32_XOR);
										wasm_i32_const (&body, (gint32) wj_mcache_bhash (_b)); wasm_op (&body, WASM_OP_I32_XOR);
										wasm_i32_const (&body, WJ_MCACHE_N - 1); wasm_op (&body, WASM_OP_I32_AND);
										wasm_i32_const (&body, 4); wasm_op (&body, WASM_OP_I32_SHL);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_op (&body, WASM_OP_I32_ADD);
										wasm_i32_const (&body, _mcp); wasm_op (&body, WASM_OP_I32_SUB);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);   /* scratch: the entry address */
										wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $mc_miss */
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);       /* .vt */
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_op (&body, WASM_OP_I32_NE);
											wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 4);       /* .base */
											wasm_i32_const (&body, (gint32) (intptr_t) _b);
											wasm_op (&body, WASM_OP_I32_NE);
											wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 8);       /* .fslot */
											wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
											wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1);                /* hit -> $chit */
										wasm_op (&body, WASM_OP_END);   /* $mc_miss */
									  } }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* -> $cslow */
									wasm_op (&body, WASM_OP_END);                        /* $chit */
									wj_emit_fast_count (&body, WJC_FAST_VIC);
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vslow hit arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wj_emit_dynamic_call (&body, (guint32) ftdi);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "vslow hit dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> end $after */
									wasm_op (&body, WASM_OP_END);                            /* $cslow */
									wj_emit_fast_count (&body, WJC_FAST_VMISS);
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vslow arg ld"; goto done; }
									wasm_i64_const (&body, (gint64) (gsize) vsd);
									wj_emit_helper_call (&body, (gpointer) (intptr_t) vs_slot, (guint32) vs_ti);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "vslow dreg"; goto done; }
									goto vcall_compact_done;
								}
							  } }
#endif
							if (slim_pred) {
								/* One compact worker-local alternate PIC entry. The first prediction miss reaches
								 * vcall_resolve_fslot, which publishes its admitted {vtable,fslot} into way zero.
								 * Thereafter this guard handles that receiver without the miss frame, Mono lookup,
								 * admission walk, or interpreter bridge. More polymorphic sites still retain the
								 * shared cold path; keeping only one alternate is what preserves slim code size. */
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $slim_pic_miss */
									/* site < *TLS-cap, otherwise the worker has not allocated this PIC yet */
									wasm_i32_const (&body, (gint32) vpic_site);
									EMIT_IC_VPIC_CAPVAL ();
									wasm_op (&body, WASM_OP_I32_GE_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* entry = *TLS-ptr + site * ways * stride */
									EMIT_IC_VPIC_PTR ();
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride));
									wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "slim pic this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
									wasm_i64_const (&body, 32);
									wasm_op (&body, WASM_OP_I64_SHR_U);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									wj_emit_fast_count (&body, WJC_FAST_VIC_SLIM);   /* C1 route mix */
									/* Preserve exact alternate-target frequency when profiling is enabled. */
									{
										extern int mono_wasm_jit_profile_fast;
										if (mono_wasm_jit_profile_fast) {
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 12);
											wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
											wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 12);
										}
									}
									for (ai = 0; ai < n2c; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "slim pic arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wj_emit_dynamic_call (&body, (guint32) ftdi);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "slim pic dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> outer $after */
								wasm_op (&body, WASM_OP_END); /* $slim_pic_miss */
								goto vcall_cold_miss_emit;
							}
							/* OBJGUARD (once, hoisted above the N ways): validate the receiver before any raw
							 * this->vtable load. Otherwise a type-confused scalar/garbage receiver traps as a bare
							 * wasm OOB before the C resolver can print anything. */
#ifdef HOST_BROWSER
							if (G_UNLIKELY (lc.objguard && lc.check_ti >= 0)) {
								extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this guard ld"; goto done; }
								wasm_i32_const (&body, 5);
								wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_check_store, lc.check_ti);
							}
#endif
							/* Delegate.Invoke has a second IC beside the ordinary receiver-vtable IC. Its miss path
							 * (resolve_fslot -> prepare_delegate_call) publishes a seqlock-protected recipe keyed by
							 * delegate.method and, for virtual targets, target->vtable. Consume that recipe here before
							 * entering either C resolver. On a hit the current closed target is read from the delegate
							 * instance (never cached), and the per-thread f-slot bitmap provides the same admission gate
							 * as the ordinary inline vcall IC. Multicast delegates explicitly miss: a recipe learned from
							 * a single-cast instance at this polymorphic callsite must never bypass their invocation list. */
							if (is_delegate_invoke) {
								/* ic_ways, not the global width: a site the interpreter only ever saw one target
								 * at needs one chain, and each dead chain here is a seqlock read, a compare, a
								 * branch and two call_indirect in every body containing the site. */
								/* dpic_local: read the WORKER-LOCAL recipe PIC instead of the shared WjDelegateIC.
								 * The local entry caches the ALREADY ADMITTED fslot next to its guard key in one
								 * i64, which removes, per dispatch: the two-atomic seqlock bracket, the scalar
								 * test, the imethod -> imethod->fslot double load, and the whole TLS liveness
								 * bitmap probe. Nothing here is a weaker check -- each removed test is instead an
								 * invariant of publication (wj_delegate_pic_publish), and the entry is private to
								 * this worker so there is no writer to race. */
								if (dpic_local) {
									/* Emitted ONCE for all ways: the multicast test depends only on `this`, and
									 * the base/bounds arithmetic only on the site. Way selection then folds into
									 * the load immediates, so an extra way costs no address arithmetic at all. */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_ic_done */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate pic list ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_list_off);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_i32_const (&body, (gint32) vpic_site);
									EMIT_IC_DPIC_CAPVAL ();
									wasm_op (&body, WASM_OP_I32_GE_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Array BASE only — the site displacement folds into each way's load immediate
									 * below, exactly as for the fslot IC, so the `i32.const; i32.add` disappears from
									 * every delegate dispatch (~1015M per window). */
									EMIT_IC_DPIC_PTR ();
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dic_base_idx);
								}
								/*
								 * OBJECT-KEYED DELEGATE DISPATCH (MONO_WASM_JIT_DELEGATE_OBJ_PIC, R187).
								 *
								 * Self-contained: one BLOCK, no hoisted prologue, no way loop, and its own copy
								 * of the ABI tail. Deliberately NOT interleaved into the two arms below -- they
								 * share a guard/tail structure in three interleaved regions, and threading a
								 * third arm through it would make the knob-off output impossible to prove
								 * unchanged. As written, `obj_pic` off leaves every byte below untouched, which
								 * tiershape.py can gate against a same-binary CONTROL pair (R241:
								 * that floor is 0.69% on the current tier, not the 0.15% this
								 * line used to quote from a tier half the size).
								 *
								 * Sequence, against the ~49 wasm ops / 6 loads the per-site arm cites:
								 *   this->method      == 0 -> multicast, no single-cast recipe (interp.c:9887
								 *                       uses exactly this test, so the two agree by construction)
								 *   this->invoke_info == 0 -> the delegate never went through
								 *                       interp_init_delegate's llvmonly path
								 *   info->wasm_jit_recipe == 0 -> no recipe published for this callee yet
								 *   unpack (fslot << 3) | shape
								 *   wj_slot_live[fslot] -> did THIS worker install it
								 * = 3 loads, 3 tests, 4 ALU ops, then the bitmap probe.
								 */
								if (obj_pic) {
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_obj_fail */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate obj method ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_method_off);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate obj info ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_info_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dti_recipe_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* shape first: it reads vc_fslot before the shift overwrites it. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_i32_const (&body, 7); wasm_op (&body, WASM_OP_I32_AND);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHR_U);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* PER-WORKER INSTALLATION. The recipe's f-slot number is process-wide; whether
									 * this worker put a function there is not, and table[fslot] != null is not the
									 * test -- the jiterpreter PREFILLS the range with a callable placeholder whose
									 * (i32,i32,i32,i32)->void body writes 999 through its fourth argument. Same
									 * bitmap the shared arm probes; monotone, so no seqlock and no atomics. */
									wj_emit_slot_live_probe (&body, vc_fslot_idx, slotlive_cap_idx, slotlive_ptr_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wj_emit_fast_count (&body, WJC_DELEGATE_IC_HIT);
									wj_emit_fast_count (&body, WJC_FAST_DELEGATE);
									/* R215: DELEGATE DEVIRT. A guarded DIRECT call for the site's dominant target.
									 *
									 * Delegates are ~23.5% of executed dispatch (R203) and have had no devirt
									 * at all, because `prof_predict` needs a resolved `target` and the recorder
									 * passes NULL for delegate sites, and because `margin` is deliberately left
									 * 0 for them -- their identity is `del->method`, not a receiver vtable, so
									 * a majority vote over receivers means nothing. Frequency is the only
									 * usable signal, which is exactly what `id_counts` (R206) provides and what
									 * prof_predict_alt reads.
									 *
									 * Guarding the RESOLVED F-SLOT rather than `del->method` is what keeps this
									 * small: the recipe has already unpacked both the target f-slot and the
									 * ABI shape into locals, so one compare reuses all of that instead of
									 * re-deriving the shape from `target`/`method_is_virtual`/`bound`. The hit
									 * path is a WASM_RELOC_CALL, so it is co-locatable and shadowable on the
									 * same terms as any other arm -- which is what makes it a DIRECT call and
									 * not merely a cheaper indirect one.
									 *
									 * Cost is honest: the direct path duplicates the two-way shape branch, so a
									 * delegate site grows by roughly one extra argument-marshalling sequence per
									 * shape. Gated on the break-even like every other arm. */
									if (ddevirt_fslot > 0) {
										/* CLOSED SHAPE ONLY, and this is a correctness bound rather than a
										 * simplification. The two shape branches call through DIFFERENT
										 * functypes -- `ftd` keeps the `this` slot (the delegate's target is
										 * passed as arg0), `dftd` drops it -- while the arm can validate its
										 * candidate against only one of them. The first version checked
										 * `ftd` and then emitted the open branch with `dftd`, so an
										 * open-shape hit called a target whose signature did not match the
										 * type it was called through: `function signature mismatch` x4 and a
										 * wedged run, exactly what call_indirect's canonical-type check is
										 * for. Guard the shape FIRST, arm only shape <= 2, and let open
										 * shapes keep the indirect tail below. */
										wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $ddevirt_miss */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
										wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
										wasm_op (&body, WASM_OP_I32_GT_U);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* open -> indirect */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wasm_i32_const (&body, (gint32) ddevirt_fslot);
										wasm_op (&body, WASM_OP_I32_NE);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* other target */
										wj_emit_fast_count (&body, WJC_FAST_DELEGATE_DEVIRT);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "ddevirt closed this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "ddevirt closed arg ld"; goto done; }
										wj_emit_method_call (&body, ddevirt_target, ddevirt_fslot, (guint32) ftdi);
										if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "ddevirt dreg"; goto done; }
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 2); /* -> $after */
										wasm_op (&body, WASM_OP_END); /* $ddevirt_miss */
									}
									/* Same two-way ABI rewrite as the per-site arms: closed-instance and
									 * bound-static (shape <= 2) replace the Delegate `this` with the current
									 * target; open-static and open-instance drop it. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
									wasm_op (&body, WASM_OP_I32_LE_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, rv == WASM_VOID ? 0x40 : (guint8) rv);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate obj closed this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate obj closed arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) ftdi);
									wasm_op (&body, WASM_OP_ELSE);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate obj open arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) dftdi);
									wasm_op (&body, WASM_OP_END);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "delegate obj dreg"; goto done; }
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> $after */
									wasm_op (&body, WASM_OP_END); /* $delegate_obj_fail */
								}
								for (way = 0; !obj_pic && way < ic_ways; ++way) {
								    if (dpic_local) {
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_way_fail */
									/* delegate.method == entry.source, and the admitted fslot arrives in the SAME
									 * i64 as the key it is guarded by — one load for guard and payload both. */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate pic source ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_method_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride));
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* An empty way is all zero and a real MonoMethod* is never 0, so the compare
									 * above is also the occupancy test — no separate seq/valid field. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride + dpic_rvt_off));
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_rgctx_idx);
								    } else {
									gint32 dic_addr = (gint32) (intptr_t) delegate_ic + way * delegate_ic_stride;
									/* Materialise the entry base ONCE per way; the seven field reads below use the
									 * local. Saves 20 wire bytes a way, which is what decides whether the enclosing
									 * stub fits V8's 500-byte inlining cap. */
									wasm_i32_const (&body, dic_addr);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_way_fail */
									/* seq must be stable, nonzero, and even. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x10); wasm_memarg (&body, 2, (guint32) dic_seq_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Multicast list must be null. */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_list_off);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* delegate.method == recipe.source */
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic source ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_method_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_source_off);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* A zero receiver key denotes nonvirtual/static recipes. Otherwise validate the
									 * current closed target's vtable without introducing another control depth. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_receiver_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_rgctx_idx);
								    }
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic target ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_rgctx_idx);
											wasm_op (&body, WASM_OP_I32_EQ);
										wasm_op (&body, WASM_OP_ELSE);
											wasm_i32_const (&body, 0);
										wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_ELSE);
										wasm_i32_const (&body, 1);
									wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								    if (dpic_local) {
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * dpic_stride + way * dpic_stride + dpic_shape_off));
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									/* fslot = high half of the pair already loaded and validated above. It is
									 * nonzero, ABI-EXPRESSIBLE and live on THIS worker by construction, not by
									 * inspection: wj_delegate_pic_publish stores only an admitted fslot whose
									 * signature lowers to a functype this site can express, the array is
									 * per-worker, and slot-live bits are never cleared. That is what retires the
									 * shape test, the fslot != 0 test and the bitmap probe.
									 *
									 * THE INVARIANT USED TO BE "SCALAR" AND THAT WAS THE WRONG ONE. `scalar` is
									 * about the INTERPRETER's stackval layout, which this path never touches --
									 * it calls `f` through ftd/dftd, built from mono_wasm_get_call_info over
									 * this site's own Invoke signature, and that lowering already admits
									 * WJ_ARG_VTYPE_BYADDR (a by-value vtype becomes an i32 pointer on both
									 * sides, which is what the caller spilled). Gating on `scalar` sent 100% of
									 * by-value-vtype delegate dispatches -- 9,354,181 per window -- to the C
									 * fallback for a constraint belonging to `e`. See
									 * mono_wasm_jit_delegate_abi_ok. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
									wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U);
									wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								    } else {
									/* Only scalar recipes have a canonical direct f-thunk ABI. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_scalar_off);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_shape_off);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) dic_imethod_off);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) fslot_off);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Re-read seq after all payload fields. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dic_base_idx);
									wasm_op (&body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (&body, 0x10); wasm_memarg (&body, 2, (guint32) dic_seq_off);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
									/* Per-thread f-slot liveness/admission gate. */
									wj_emit_slot_live_probe (&body, vc_fslot_idx, slotlive_cap_idx, slotlive_ptr_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								    }
									wj_emit_fast_count (&body, WJC_DELEGATE_IC_HIT);
									wj_emit_fast_count (&body, WJC_FAST_DELEGATE);
									/* R215: DELEGATE DEVIRT, on the SHIPPED path. The first version of this
									 * arm was written only into the `obj_pic` block above, which ships 0, so
									 * it emitted nothing in the default configuration: 750 sites resolved a
									 * target and `FastDelegateDevirt` read 0. The resolution counter is
									 * bumped where the target is CHOSEN and the arm is emitted somewhere
									 * else, so "armed" and "emitted" were two different populations and the
									 * split hid it. Design note is on the obj_pic copy; this is the same
									 * guard at the branch depth the way loop uses.
									 *
									 * CLOSED SHAPE ONLY, and that is a correctness bound: the open branch
									 * calls through `dftd`, which drops `this`, while the candidate was
									 * validated against `ftd`. Guard the shape first, arm only shape <= 2,
									 * and let open shapes fall to the indirect tail below. */
									if (ddevirt_fslot > 0) {
										wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $ddevirt_miss */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
										wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
										wasm_op (&body, WASM_OP_I32_GT_U);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* open -> indirect */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wasm_i32_const (&body, (gint32) ddevirt_fslot);
										wasm_op (&body, WASM_OP_I32_NE);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* other target */
										wj_emit_fast_count (&body, WJC_FAST_DELEGATE_DEVIRT);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "ddevirt ic this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "ddevirt ic arg ld"; goto done; }
										wj_emit_method_call (&body, ddevirt_target, ddevirt_fslot, (guint32) ftdi);
										if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "ddevirt ic dreg"; goto done; }
										/* One deeper than the way tail's own exit: $ddevirt_miss sits inside
										 * $delegate_way_fail, which sits inside $delegate_ic_done when the
										 * worker-local PIC is on. */
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, dpic_local ? 3 : 2); /* -> $after */
										wasm_op (&body, WASM_OP_END); /* $ddevirt_miss */
									}
									/* Closed-instance/bound-static replace Delegate this with the current target;
									 * open-static/open-instance simply drop Delegate this. Both arms return the same
									 * scalar value, so a typed if carries it to the common destination store. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
									wasm_op (&body, WASM_OP_I32_LE_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, rv == WASM_VOID ? 0x40 : (guint8) rv);
										if (!wasm_ld (&body, &lc, this_vr)) { fail = "delegate ic closed this ld"; goto done; }
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) delegate_target_off);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate ic closed arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) ftdi);
									wasm_op (&body, WASM_OP_ELSE);
										for (ai = 1; ai < n2; ++ai)
											if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "delegate ic open arg ld"; goto done; }
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) dftdi);
									wasm_op (&body, WASM_OP_END);
									if (rv != WASM_VOID && !wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "delegate ic dreg"; goto done; }
									/* One level deeper in the local-PIC arm: the ways sit inside the shared
									 * $delegate_ic_done block that carries the hoisted multicast/bounds tests. */
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, dpic_local ? 2 : 1); /* -> outer $after */
									wasm_op (&body, WASM_OP_END); /* $delegate_way_fail */
								}
								if (dpic_local)
									wasm_op (&body, WASM_OP_END); /* $delegate_ic_done */
							}
								/* Worker-local compact N-way {vtable,fslot} PIC. The stable site id indexes the
								 * current worker's TLS array selected by imported s.v/s.n. The miss helper publishes
								 * only an admitted local fslot, eliminating the InterpMethod load and liveness bitmap
								 * test from a hit. Each 16-byte entry keeps the hot pair at +0 and cold
								 * {target,count} batching feedback at +8/+12. Way zero stays straight-line for V8's
								 * monomorphic feedback; cold ways share one loop and all ways share one typed tail. */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $slow (void) */
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $tail (void) */
							{ extern int mono_wasm_jit_f4;   /* plan2x F4 & 2: the IR's vtable vreg, or reload [this] */
							  int _irvt = (mono_wasm_jit_f4 & 2) ? wj_ir_vtable_vreg (ins, this_vr) : -1;
							  if (_irvt >= 0) {
								if (!wasm_ld (&body, &lc, _irvt)) { fail = "ic irvt ld"; goto done; }
								wj_count (WJC_F4_VT_REUSE);
							  } else {
								if (!wasm_ld (&body, &lc, this_vr)) { fail = "ic this ld"; goto done; }
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
							  } }
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
								/* Bounds-check before dereferencing the TLS PIC pointer. */
								wasm_i32_const (&body, (gint32) vpic_site);
								EMIT_IC_VPIC_CAPVAL ();
								wasm_op (&body, WASM_OP_I32_GE_U);
								WJ_HINT2 (0, WJC_BRANCH_HINT_IC);   /* R341 */
								wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1); /* miss -> $slow */
								/* Keep the ARRAY BASE in the local and fold `site * ways * stride` into way zero's
								 * load immediate, instead of materialising a cursor. A wasm load already carries a
								 * 32-bit offset, so the `i32.const K; i32.add` that built the cursor is pure waste on
								 * every HIT — and a hit is the overwhelming case. The cold ways still need a real
								 * moving cursor, so it is computed inside $way_fail below, i.e. only when way zero
								 * misses. Net: 2 fewer ops per dispatch on ~822M fslot-IC dispatches per window. */
								EMIT_IC_VPIC_PTR ();
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								/* Only the monomorphic/MRU way is emitted straight-line. */
							for (way = 0; way < 1; ++way) {
							wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /*   $way_fail (void) */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
								/* ic = i64.load(base + site*ways*stride); if ((i32)ic != vtab) -> $way_fail */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
								wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride));
							wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
							wasm_op (&body, WASM_OP_I32_WRAP_I64);
							wasm_op (&body, WASM_OP_I32_NE);
							WJ_HINT2 (0, WJC_BRANCH_HINT_IC);   /* R341: way 0 misses a few % of dispatches since R311 */
							wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								/* fslot = high32. A matching real vtable implies a nonzero admitted fslot. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
								wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* Exact target-frequency writes are opt-in: normal dispatch must not dirty a
									 * cache line on every hit. PROFILE_FAST already denotes a dedicated sampling
									 * run, so the batcher gets both aggregate and per-target volume from one flag. */
									{
										extern int mono_wasm_jit_profile_fast;
										if (mono_wasm_jit_profile_fast) {
										/* aic_ti holds the array BASE now, so the hit counter's offset carries the
										 * site displacement as well (entry+12 == base + site_off + 12). */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + 12));
										wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + 12));
									}
								}
							/* HIT: the resolved f-slot is in $vc_fslot; jump to the shared call tail below. */
							if (!is_delegate_invoke) wj_emit_fast_count (&body, WJC_FAST_VIC_W0);   /* C1 route mix */
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> exit $tail */
							wasm_op (&body, WASM_OP_END);                            /* end $way_fail */
							}   /* for each way */
							if (vc_mid_slot >= 0 && mono_wasm_jit_ic_mid) {
								/* R311 METHOD-IDENTITY FALLBACK. Way zero keyed on the receiver VTABLE missed; if the
								 * receiver's class holds the entry's cached method (+8) in this call's vtable slot,
								 * it is the same method, so the entry's f-slot is the right one -- and it is already
								 * admitted on this thread, which is why it is in this thread's PIC. R310: two-thirds
								 * of in-game IC misses are exactly this (another class inheriting the method). The
								 * entry is left alone, so the front-runner keeps way zero. A zero f-slot (empty
								 * entry whose +8 happens to compare equal) falls through to the resolver. */
								wj_count (WJC_MID_IC_SITES);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $mid_fail */
								{ extern int mono_wasm_jit_f4;
								  /* plan2x F4 & 4: way zero's entry remembers the last other vtable whose class
								   * held its method in this slot. Per-thread like the entry, and cleared whenever
								   * the entry's method changes (wj_vcall_pic_publish), so a hit means exactly what
								   * the identity check below would have concluded. */
								  guint32 _mo = (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + vpic_midvt_off);
								  gboolean _memo = (mono_wasm_jit_f4 & 4) && vpic_midvt_off > 0;
								  if (_memo) {
									wj_count (WJC_F4_IC_MEMO);
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $mid_ok */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, _mo);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_op (&body, WASM_OP_I32_EQ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* -> $mid_ok */
								  }
									wj_emit_class_slot_method (&body, aic_vtab_idx, vc_class_vtable_off, vc_mid_slot);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + 8));
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, _memo ? 1 : 0);   /* -> $mid_fail */
								  if (_memo) {
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, _mo);
									wasm_op (&body, WASM_OP_END);   /* $mid_ok */
								  } }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
									wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64);
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* -> $mid_fail */
									wj_emit_fast_count (&body, WJC_FAST_VIC_MID);   /* C1 route mix */
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);      /* hit -> exit $tail */
								wasm_op (&body, WASM_OP_END);   /* $mid_fail */
							} else if (!is_delegate_invoke && mono_wasm_jit_ic_mid) {
								wj_count (vc_mid_slot == -2 ? WJC_MID_IC_SKIP_IFACE : WJC_MID_IC_SKIP_OTHER);
							}
							{ extern int mono_wasm_jit_f9, mono_wasm_jit_mcache_prefix (void);
							  /* plan2x F9: (vtable, base) -> f-slot, per thread, in front of the PIC array (aic_ti is the
							   * array base here, and way zero's bounds check already proved the array exists). A
							   * non-generic base only: for a shared generic site the pair does not determine the target. */
							  MonoMethod *_b = call->method;
#ifdef HOST_BROWSER   /* the prefix lives in interp.c's HOST_BROWSER region; mono-aot-cross links this file too */
							  int _mcp = ((mono_wasm_jit_f9 & 1) && !is_delegate_invoke && _b && !_b->is_inflated && !_b->is_generic &&
							              !mono_class_is_ginst (_b->klass) && !mono_class_is_gtd (_b->klass)) ? mono_wasm_jit_mcache_prefix () : 0;
#else
							  int _mcp = 0;
#endif
							  if (_mcp > 0) {
								wj_count (WJC_F9_MCACHE_SITES);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $mc_miss */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHR_U);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_i32_const (&body, 11); wasm_op (&body, WASM_OP_I32_SHR_U);
									wasm_op (&body, WASM_OP_I32_XOR);
									wasm_i32_const (&body, (gint32) wj_mcache_bhash (_b)); wasm_op (&body, WASM_OP_I32_XOR);
									wasm_i32_const (&body, WJ_MCACHE_N - 1); wasm_op (&body, WASM_OP_I32_AND);
									wasm_i32_const (&body, 4); wasm_op (&body, WASM_OP_I32_SHL);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_op (&body, WASM_OP_I32_ADD);
									wasm_i32_const (&body, _mcp); wasm_op (&body, WASM_OP_I32_SUB);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);   /* scratch: the entry address */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);       /* .vt */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);                /* -> $mc_miss */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 4);       /* .base */
									wasm_i32_const (&body, (gint32) (intptr_t) _b);
									wasm_op (&body, WASM_OP_I32_NE);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);                /* -> $mc_miss */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 8);       /* .fslot */
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 1);                /* hit -> exit $tail */
								wasm_op (&body, WASM_OP_END);   /* $mc_miss */
								wasm_i32_const (&body, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
							  } }
							if (ic_ways > 1) {
									/* Way zero missed: scan ways 1..N with one constant-size wasm loop. */
									wasm_i32_const (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
									/* Way zero missed: NOW materialise the moving cursor the cold loop needs, at
									 * base + site_off + stride (way 1). This arithmetic used to run on every
									 * dispatch to build a cursor way zero did not need. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
									wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + vpic_stride));
									wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $cold_done */
								wasm_op (&body, WASM_OP_LOOP); wasm_u8 (&body, 0x40);  /* $cold_loop */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $cold_next */
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
											wasm_op (&body, WASM_OP_I64_LOAD); wasm_memarg (&body, 3, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_ic_idx);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op (&body, WASM_OP_I32_NE);
										wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx);
											wasm_i64_const (&body, 32);
											wasm_op (&body, WASM_OP_I64_SHR_U);
											wasm_op (&body, WASM_OP_I32_WRAP_I64);
											wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
											{
												extern int mono_wasm_jit_profile_fast;
												if (mono_wasm_jit_profile_fast) {
													wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
													wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
													wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 12);
													wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_ADD);
													wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 12);
												}
											}
											wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 2); /* hit -> $cold_done */
									wasm_op (&body, WASM_OP_END); /* $cold_next */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx);
										wasm_i32_const (&body, vpic_stride); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) aic_ti_idx);
										EMIT_IC_VPIC_PTR ();
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
										/* End of the ways this site actually checks. The site's REGION is still
										 * global-width (stride unchanged), so this is base + ic_ways entries, not
										 * the start of the next site — those two coincide only when the site was
										 * not narrowed. */
										wasm_i32_const (&body, (gint32) (vpic_site * mono_wasm_jit_vcall_ways * vpic_stride + ic_ways * vpic_stride));
										wasm_op (&body, WASM_OP_I32_ADD);
									wasm_op (&body, WASM_OP_I32_LT_U);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0); /* -> $cold_loop */
									wasm_i32_const (&body, 0);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_END); /* $cold_loop */
								wasm_op (&body, WASM_OP_END); /* $cold_done */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0); /* hit -> exit $tail */
							}
							/* every way missed -> exit $slow, skipping the shared tail */
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);
							wasm_op (&body, WASM_OP_END);                            /* end $tail */
							/* SHARED HIT TAIL, emitted once for the whole chain: push this+args (fresh from
							 * vregs -- nothing is carried across the block boundary, so both blocks stay void),
							 * then call_indirect the f-slot the winning guard resolved. */
							wj_emit_fast_count (&body, WJC_FAST_VIC);   /* profile: inline f-slot IC hit (JIT->JIT) */
							/* R205: and again, tagged with THIS SITE's devirt outcome, so the IC's executed volume
							 * splits by cause. Sums to WJC_FAST_VIC by construction -- check that before quoting it. */
							switch (ic_tag) {
							case 7:  wj_emit_fast_count (&body, WJC_FAST_VIC_PRED); break;
							case 1:  wj_emit_fast_count (&body, WJC_FAST_VIC_NO_REC); break;
							case 2:  wj_emit_fast_count (&body, WJC_FAST_VIC_COLD); break;
							case 3: case 4: wj_emit_fast_count (&body, WJC_FAST_VIC_POLY); break;
							case 6:  wj_emit_fast_count (&body, WJC_FAST_VIC_NO_FSLOT); break;
							default: wj_emit_fast_count (&body, WJC_FAST_VIC_OTHER); break;
							}
							for (ai = 0; ai < n2c; ++ai)
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "ic fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
							wj_emit_dynamic_call (&body, (guint32) ftdi);
							if (rv != WASM_VOID) { if (!wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "ic fast dreg"; goto done; } }
							wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);       /* -> $after (skip the C-helper slow path) */
							wasm_op (&body, WASM_OP_END);                            /* end $slow */
							/* falls through to the AOT-IC / resolve_fslot slow path, at its ORIGINAL nesting */
							wasm_origin_push (&body, WJO_AOT_IC);   /* the rest of this ins */
							/* --- INLINE AOT-VCALL IC (VCALL_AOT_IC), MT-safe: two atomic i64 words, each vtab-tagged. A hit needs
							 * BOTH words' low32 == this->vtable; then ti/kind (ic1.hi) and rgctx (ic2.hi) are correct for this
							 * vtable regardless of interleaved fills (a vtable maps to ONE target -> identical values). Only
							 * atomic i64 loads (no plain-vs-atomic ordering, which wasm's memory model does not provide). */
							if (aic) {
								/* Compact N-way AOT PIC. Way zero stays inline; ways 1..N are scanned by one
								 * signature-independent helper which returns an entry address. Both routes then
								 * use this single tag validation, payload extraction, argument load and typed
								 * call tail. Cache capacity therefore no longer multiplies generated code. */
								{ extern int mono_wasm_jit_f4;   /* plan2x F4 & 2 */
								  int _irvt = (mono_wasm_jit_f4 & 2) ? wj_ir_vtable_vreg (ins, this_vr) : -1;
								  if (_irvt >= 0) {
									if (!wasm_ld (&body, &lc, _irvt)) { fail = "aot ic irvt ld"; goto done; }
									wj_count (WJC_F4_VT_REUSE);
								  } else {
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "aot ic this ld"; goto done; }
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 0);
								  } }
								wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_vtab_idx);
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_selected */
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $aot_way0_fail */
										wasm_i32_const (&body, (gint32) (intptr_t) aic); wj_emit_ic_load64 (&body, 0);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_i32_const (&body, (gint32) (intptr_t) aic); wj_emit_ic_load64 (&body, 8);
										wasm_op (&body, WASM_OP_I32_WRAP_I64);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
										wasm_i32_const (&body, (gint32) (intptr_t) aic);
										wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx); /* selected entry ptr */
										wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* skip cold scan */
									wasm_op (&body, WASM_OP_END);
									if (mono_wasm_jit_vcall_aot_ways > 1) {
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx);
										wasm_i32_const (&body, (gint32) (intptr_t) aic);
#ifdef HOST_BROWSER
										wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_aot_pic_lookup, vtdi);
#else
										wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7fe2, (guint32) vtdi);
#endif
									} else {
										wasm_i32_const (&body, 0);
									}
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_END);
								/* Revalidate both tags while loading the payload. A concurrent eviction between
								 * selection and this load becomes a miss rather than mixed target data. */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx); wj_emit_ic_load64 (&body, 0); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_ti_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx); wj_emit_ic_load64 (&body, 8); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_ic_idx);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_vtab_idx); wasm_op (&body, WASM_OP_I32_NE); wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_ic_idx); wasm_i64_const (&body, 32); wasm_op (&body, WASM_OP_I64_SHR_U); wasm_op (&body, WASM_OP_I32_WRAP_I64); wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) aic_rgctx_idx);
								wj_emit_fast_count (&body, WJC_FAST_AOTIC);   /* profile: inline AOT-IC hit (JIT->AOT) */
								for (ai = 0; ai < n2; ++ai) if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "aot ic arg ld"; goto done; }
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_AND);   /* kind2bit selector */
								wasm_op (&body, WASM_OP_IF); wj_emit_blocktype (&body, (guint32) aic_ati_ne);   /* typed block in: (this,args) out: rv */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);   /* ti */
									wj_emit_dynamic_call (&body, (guint32) aic_ati_ne);
								wasm_op (&body, WASM_OP_ELSE);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_rgctx_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) aic_ti_idx); wasm_i32_const (&body, 1); wasm_op (&body, WASM_OP_I32_SHR_U);
									wj_emit_dynamic_call (&body, (guint32) aic_ati);
								wasm_op (&body, WASM_OP_END);   /* end kind if/else */
								if (rv != WASM_VOID) { if (rv == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret); if (!wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "aot ic dreg"; goto done; } }
								wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* hit -> $after */
									wasm_op (&body, WASM_OP_END);   /* selected entry invalid/absent -> resolve */
								}
							}
vcall_cold_miss_emit:
							if (!is_delegate_invoke) wj_emit_fast_count (&body, WJC_FAST_VMISS);   /* C1 route mix */
							{
							/* MONO_WASM_JIT_VCALL_SHARED_MISS ships **0**, and 0 is the MEASURED-GOOD arm, so this
							 * is the one two-path knob whose LOSING arm was the runtime default.
							 *
							 * The deleted arm outlined the whole cold-miss sequence into one signature-neutral
							 * helper, `(this, base_method, frame, site_ic, aot_ic) -> threw`, leaving only the
							 * unavoidable typed stores in each caller. It is a completely reasonable design and
							 * it lost decisively: -13.2%% p50 / -16.6%% mean frame / +19.9%% fps for turning it
							 * OFF, 4 interleaved rounds, 14.5 of 16 pairwise on tails (p~0.02), with two of the
							 * four runs SUSTAINING 30-33 fps. The helper was 2.661%% of in-game samples on its
							 * own and the win is larger than that, so it is not merely the helper's cost -- it
							 * is the dispatch SHAPE the outlining forces at every site.
							 *
							 * That is the same lesson INLINE_ILOFS taught from the other direction: outlining
							 * and inlining are not symmetric here. Inlining a helper pays when it removes CODE
							 * and loses when it lengthens a live range; outlining pays when it removes code
							 * from a cold path and loses when it forces a call-shaped sequence onto a hot one.
							 *
							 * What remains is the per-caller lowering, which is now the only path. */
							/* Legacy per-caller cold miss lowering, retained for an env-var A/B. */
							/* $scratch = imported s.b (see the direct-residual site above). HOT now that
							 * vcall_shared_miss=0 makes per-site cold-miss lowering the active path. */
#ifdef HOST_BROWSER
						wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 7);
#else
						wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ffb, (guint32) vtsi);
#endif
						wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) scratch_idx);
						/* fslot = mono_wasm_jit_vcall_resolve_fslot(this, base, scratch): resolve the override (synchronized
						 * wrapper substituted), stash the target MonoMethod* at scratch+200 (for the call_interp fallback),
						 * and return the override's scalar `f` f-slot if it's itself wasm-JITted (else 0). Done BEFORE spilling
						 * ref args: resolve+transform can GC, and until the spill the ref args still live on the GC-scanned
						 * shadow stack (so a GC moves them safely; `this` is by value). Stash fslot at scratch+208. */
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* store-addr base for fslot@208 */
						if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall this ld"; goto done; }
#ifdef HOST_BROWSER
						wasm_i32_const (&body, (gint32) (intptr_t) call->method);
#else
						wasm_i32_const (&body, 0x7ffd);
#endif
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* 3rd arg: scratch */
						wasm_i32_const (&body, (gint32) (intptr_t) vic);                           /* 4th arg: inline cache */
#ifdef HOST_BROWSER
						wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_resolve_fslot, vtrfi);
#else
						wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ffa, (guint32) vtrfi); /* -> fslot */
#endif
						wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 208 /* scratch temp: f-slot */);
						/* if (fslot != 0) FAST: call_indirect straight into the override's wasm `f`; else call_interp. */
						wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
						wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);
						wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
							/* FAST PATH: push this + args fresh from their vregs (GC-scanned shadow stack; resolve_fslot did
							 * not spill, so no GC-invisible window), then the f-slot table index, call_indirect(ftd). */
							for (ai = 0; ai < n2c; ++ai)
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall fast arg ld"; goto done; }
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 208);            /* f-slot = table index */
							wj_emit_dynamic_call (&body, (guint32) ftdi);
							if (rv != WASM_VOID) { if (!wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "vcall fast dreg"; goto done; } }
						wasm_op (&body, WASM_OP_ELSE);
							/* Fast AOT-vcall (MONO_WASM_JIT_VCALL_AOT, gated, default off): if the resolved override
							 * (target@200, stashed by resolve_fslot) is AOT-backed, call its AOT body DIRECTLY with the
							 * native (this,args,rgctx) ABI — skipping the residual's double marshalling + do_jit_call
							 * frame. Wrapped in a $no_aot block: if the helper says "not AOT" we br to $no_aot and fall
							 * into the (shared, unchanged) residual below; if AOT we call + br past the residual. */
							{
								/* not for a hidden-vret site: at/at_ne below put the vret nowhere, and the native ABI wants
								 * it FIRST. A vret override that is AOT-backed takes the residual. */
								if (!vc_vret) {
									extern int mono_wasm_jit_vcall_aot_target (guint8 *scratch, MonoObject *this_obj, gpointer aic);
									WasmFuncType at, at_ne, aott; int ati = -1, ati_ne = -1, aotti = -1;
									if (n2 + 1 > WASM_FUNCTYPE_MAX_PARAMS) { fail = "vcall aot nparams"; goto done; }
									/* aott (i32,i32,i32)->i32 = the resolve helper (scratch, receiver, AOT IC), returning
									 * 0=residual / 1=AOT+rgctx / 2=AOT,no-extra-arg.
									 * at (this,params,i32 rgctx)->rv = AOT body WITH the trailing extra (rgctx/dummy) arg; at_ne
									 * (this,params)->rv = AOT body of an exempt wrapper kind WITHOUT it. The override is resolved at
									 * RUNTIME but the call_indirect functype is baked here, so emit BOTH variants and pick by the
									 * helper's return code (kind==2 -> at_ne). Keeps every AOT-backed vcall fast (no residual). */
									memset (&aott, 0, sizeof (aott)); aott.params [0] = WASM_I32; aott.params [1] = WASM_I32; aott.params [2] = WASM_I32; aott.nparams = 3; aott.ret = WASM_I32;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &aott)) { aotti = ti_base + vk; break; }
									if (aotti < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = aott; aotti = ti_base + nextra++; }
									memset (&at, 0, sizeof (at)); for (vk = 0; vk < n2; ++vk) at.params [vk] = pp [vk]; at.params [n2] = WASM_I32; at.nparams = (guint32) (n2 + 1); at.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at)) { ati = ti_base + vk; break; }
									if (ati < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at; ati = ti_base + nextra++; }
									memset (&at_ne, 0, sizeof (at_ne)); for (vk = 0; vk < n2; ++vk) at_ne.params [vk] = pp [vk]; at_ne.nparams = (guint32) n2; at_ne.ret = rv;
									for (vk = 0; vk < nextra; ++vk) if (functype_eq (&extra_types [vk], &at_ne)) { ati_ne = ti_base + vk; break; }
									if (ati_ne < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = at_ne; ati_ne = ti_base + nextra++; }
									uses_calls = TRUE;
									wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40);   /* $no_aot */
									/* Resolve and fill the compact AOT IC in C. This removes the former N-way
									 * unrolled empty-slot search and atomic publication sequence from every site. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									if (!wasm_ld (&body, &lc, this_vr)) { fail = "vcall aot this ld"; goto done; }
									wasm_i32_const (&body, (gint32) (intptr_t) aic);
#ifdef HOST_BROWSER
									wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_vcall_aot_target, aotti);
#else
									wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff9, (guint32) aotti);
#endif
									wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_aotkind_idx);   /* stash kind, keep on stack */
									wasm_op (&body, WASM_OP_I32_EQZ);
									wasm_op (&body, WASM_OP_BR_IF); wasm_uleb (&body, 0);   /* kind==0 -> $no_aot -> residual */
									/* cppeh: bare AOT call — a throwing callee C++-unwinds natively to the nearest landing
									 * pad (an in-method catch or the interp e-thunk boundary). No try/catch wrapper. */
									/* Collapsed kind branch (mirror of the inline AOT-IC): args identical in both arms, so load
									 * them ONCE and pick the variant with a TYPED if-block (params = ati_ne = (this,args)->rv,
									 * guaranteed valid — bailed above otherwise). Only the rgctx push + functype differ. */
									for (ai = 0; ai < n2; ++ai)
										if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall aot arg ld"; goto done; }
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2);
									wasm_op (&body, WASM_OP_I32_EQ);
									wasm_op (&body, WASM_OP_IF); wj_emit_blocktype (&body, (guint32) ati_ne);   /* typed block in: (this,args) out: rv */
										/* no-extra-arg variant: AOT addr@212; call_indirect (this,args)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wj_emit_dynamic_call (&body, (guint32) ati_ne);
									wasm_op (&body, WASM_OP_ELSE);
										/* with-rgctx variant: rgctx@216, AOT addr@212; call_indirect (this,args,rgctx)->rv */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 216);   /* rgctx (ftndesc.arg) */
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 212);   /* AOT body table index */
										wj_emit_dynamic_call (&body, (guint32) ati);
									wasm_op (&body, WASM_OP_END);   /* end variant if/else */
									if (rv != WASM_VOID) {
										if (rv == WASM_I32) wasm_emit_subword_ret_norm (&body, csig->ret);   /* raw AOT body: dirty upper bits */
										if (!wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "vcall aot dreg"; goto done; }
									}
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);   /* AOT done -> exit fslot-IF, skip residual */
									wasm_op (&body, WASM_OP_END);   /* end $no_aot */
								}
							}
							/* FALLBACK: spill this + each arg into scratch[k*8] (this at 0, arg i at (1+i)*8), then re-enter
							 * the interp via call_interp(target, scratch). */
							for (ai = 0; ai < n2; ++ai) {
								WasmOpcode sop; guint32 al2;
								switch (pp [ai]) {
								case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
								case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
								case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
								default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
								}
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								if (!wj_emit_one_call_arg (&body, &lc, &vci, csig, call, ai)) { fail = "vcall arg ld"; goto done; }
								wasm_op (&body, sop); wasm_memarg (&body, al2, (guint32) (ai * 8));
							}
							/* hidden vret: the caller's return-buffer address at scratch+232, as at the direct residual.
							 * After resolve_fslot, which can run managed code that reuses this scratch. */
							if (vc_vret) {
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								if (!wasm_ld (&body, &lc, wj_arg_vreg (call, vci.nargs))) { fail = "vcall vret ld"; goto done; }
								wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 232 /* WJ_SCRATCH_VRET_OFF */);
							}
							/* INLINE DELEGATE RECIPE HIT. resolve_fslot/prepare_delegate_call published a
							 * thread-admitted target f-slot at +244 only for scalar-compatible recipes.
							 * Rewrite the Invoke ABI in linear memory, load the rewritten arguments, and
							 * enter the target f-thunk directly. This stays inside the caller's wasm/EH
							 * island and skips call_delegate -> invoke_caught -> e-thunk. A zero f-slot
							 * (cache miss, multicast, complex ABI, admission failure) takes the proven C
							 * helper below unchanged. */
							if (is_delegate_invoke) {
								wasm_op (&body, WASM_OP_BLOCK); wasm_u8 (&body, 0x40); /* $delegate_done */
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 244);
								wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) vc_fslot_idx);
								wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
									/* Capture shape, then consume-clear the recipe before entering managed
									 * code: a nested delegate call reuses this TLS scratch. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 220);
									wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) vc_aotkind_idx);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 220);
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
									wasm_i32_const (&body, 0);
									wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 244);
									wj_emit_fast_count (&body, WJC_FAST_DELEGATE);

									/* shape <= BOUND_STATIC: replace delegate `this` with the closed/bound
									 * target and retain the full signature. Otherwise drop delegate `this`
									 * with overlap-safe memory.copy and use ftd-with-param0-removed. */
									wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_aotkind_idx);
									wasm_i32_const (&body, 2 /* WJ_DELEGATE_BOUND_STATIC */);
									wasm_op (&body, WASM_OP_I32_LE_U);
									wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 248);
										wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0);
										if (rv != WASM_VOID)
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										for (ai = 0; ai < n2; ++ai) {
											WasmOpcode lop; guint32 al2;
											switch (pp [ai]) {
											case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
											case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
											case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
											default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
											}
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
											wasm_op (&body, lop); wasm_memarg (&body, al2, (guint32) (ai * 8));
										}
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) ftdi);
										if (rv != WASM_VOID) {
											WasmOpcode sop; guint32 al2;
											switch (rv) {
											case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
											case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
											case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
											default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
											}
											wasm_op (&body, sop); wasm_memarg (&body, al2, 192);
										}
									wasm_op (&body, WASM_OP_ELSE);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_i32_const (&body, 8); wasm_op (&body, WASM_OP_I32_ADD);
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 236);
										wasm_i32_const (&body, 3); wasm_op (&body, WASM_OP_I32_SHL);
										wasm_u8 (&body, 0xFC); wasm_uleb (&body, 10); wasm_u8 (&body, 0); wasm_u8 (&body, 0); /* memory.copy 0 0 */
										if (rv != WASM_VOID)
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
										for (ai = 0; ai < n2 - 1; ++ai) {
											WasmOpcode lop; guint32 al2;
											switch (pp [ai + 1]) {
											case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
											case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
											case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
											default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
											}
											wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
											wasm_op (&body, lop); wasm_memarg (&body, al2, (guint32) (ai * 8));
										}
										wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) vc_fslot_idx);
										wj_emit_dynamic_call (&body, (guint32) dftdi);
										if (rv != WASM_VOID) {
											WasmOpcode sop; guint32 al2;
											switch (rv) {
											case WASM_I64: sop = WASM_OP_I64_STORE; al2 = 3; break;
											case WASM_F32: sop = WASM_OP_F32_STORE; al2 = 2; break;
											case WASM_F64: sop = WASM_OP_F64_STORE; al2 = 3; break;
											default:       sop = WASM_OP_I32_STORE; al2 = 2; break;
											}
											wasm_op (&body, sop); wasm_memarg (&body, al2, 192);
										}
									wasm_op (&body, WASM_OP_END);
									wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1); /* -> $delegate_done */
								wasm_op (&body, WASM_OP_END);
							}
							/* DIAG (WASM_JIT_BADREF_ARG caller attribution): bake THIS (calling) method at scratch+224
							 * (mirrors the direct residual site; 224 is past ret(192)/target(200)/fslot(208)/aot(212,216)).
							 * EMITTED ONLY UNDER OBJGUARD, like its twin above and like its reader in call_interp.
							 * The knob itself is HOST_BROWSER-only (it has no meaning in mono-aot-cross, which never
							 * runs this tier), so the cross build simply never emits the store -- where it previously
							 * emitted a 0x7ff8 placeholder that nothing has ever read. */
#ifdef HOST_BROWSER
							if (G_UNLIKELY (mono_wasm_jit_objguard)) {
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_i32_const (&body, (gint32) (intptr_t) cfg->method);
								wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 224 /* WJ_SCRATCH_CALLER_OFF */);
							}
#endif
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 200);            /* target MonoMethod* */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);            /* scratch buffer */
							/* Select the instance-aware post-spill delegate helper when resolve_fslot published
							 * a direct-target recipe at +220; otherwise retain the ordinary call_interp path. */
							wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
							wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, 220);
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, (guint8) WASM_I32);
#ifdef HOST_BROWSER
								wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_delegate);
							wasm_op (&body, WASM_OP_ELSE);
								wasm_i32_const (&body, (gint32) (intptr_t) mono_wasm_jit_call_interp);
#else
								wasm_i32_const (&body, 0x7ff7);
							wasm_op (&body, WASM_OP_ELSE);
								wasm_i32_const (&body, 0x7ffc);
#endif
							wasm_op (&body, WASM_OP_END);
							wj_emit_dynamic_call (&body, (guint32) vtdi);
							/* call_interp returns 1 if the vcall fallback threw. Continue the native unwind
							 * instead of reading the stale scratch result; mirrors the direct residual path. */
							WJ_HINT (0);   /* R339 */
							wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);
								/* threw: continue the native unwind; do NOT return and do NOT pop roots first. */
								EMIT_RESIDUAL_THROW_CONTINUATION ();
							wasm_op (&body, WASM_OP_END);
							if (is_delegate_invoke)
								wasm_op (&body, WASM_OP_END); /* $delegate_done */
							if (rv != WASM_VOID) {
								WasmOpcode lop; guint32 al2;
								switch (rv) {
								case WASM_I64: lop = WASM_OP_I64_LOAD; al2 = 3; break;
								case WASM_F32: lop = WASM_OP_F32_LOAD; al2 = 2; break;
								case WASM_F64: lop = WASM_OP_F64_LOAD; al2 = 3; break;
								default:       lop = WASM_OP_I32_LOAD; al2 = 2; break;
								}
								wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) scratch_idx);
								wasm_op (&body, lop); wasm_memarg (&body, al2, 192 /* WJ_SCRATCH_RET_OFF */);
								/* Normalize sub-word integer returns exactly like the direct residual (interp_entry
								 * writes bool/i1/u1 as 1 byte and char/i2/u2 as 2 bytes; the full-width i32.load above
								 * reads stale high bytes from the scratch). Without this a vcall fallback returning a
								 * narrow signed/bool value gives the JITted caller wrong high bits/sign -> wrong managed
								 * behaviour (a plausible jit137-style NPE). */
								/* NB byref: see wasm_emit_subword_ret_norm — a byref ret is WASM_I32 and must
								 * not be masked/extended as its pointee type. */
								if (rv == WASM_I32 && !m_type_is_byref (csig->ret)) {
									MonoType *rt = mini_get_underlying_type (csig->ret);
									switch (rt->type) {
									case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: wasm_i32_const (&body, 0xff); wasm_op (&body, WASM_OP_I32_AND); break;
									case MONO_TYPE_I1: wasm_op (&body, WASM_OP_I32_EXTEND8_S); break;
									case MONO_TYPE_CHAR: case MONO_TYPE_U2: wasm_i32_const (&body, 0xffff); wasm_op (&body, WASM_OP_I32_AND); break;
									case MONO_TYPE_I2: wasm_op (&body, WASM_OP_I32_EXTEND16_S); break;
									default: break;
									}
								}
								if (!wj_vc_store_result (&body, &lc, ins, is_vcall2, &vci, rv)) { fail = "vcall dreg"; goto done; }
							}
							wasm_op (&body, WASM_OP_END);   /* end the fslot if/else (slow path) */
							}
#ifdef HOST_BROWSER
vcall_compact_done:   /* typed-spindle C1: a compact site skips straight here, inside $after */
#endif
							if (1)
							wasm_op (&body, WASM_OP_END);   /* end $after (only emitted when the inline-IC fast path is on) */
						if (is_vcall2)
							wj_count (vc_vret ? WJC_VCALL2_BYADDR : WJC_VCALL2_SCALAR);
						if (G_UNLIKELY (wj_site_cur))
							wj_site_cur->arm = (gint8) ic_tag;   /* W3 census: the arm outcome of this virtual site */
						break;
					}
				/* Raw indirect call (callreg / non-virtual membase): the target in sreg1 is a runtime
				 * value (e.g. ftndesc.addr) which, under auto-JIT (non-llvmonly), isn't a reliable wasm
				 * table index — so bail the method to the interpreter. The VIRTUAL subcase above is NOT
				 * gated: it dispatches via the inline IC + interp-entry-thunk fallback, which always
				 * resolves to a callable slot. */
				{
					if (cfg->wasm_jit_forced) { fail = "raw indirect call under auto-JIT"; goto done; }
				}
				if (is_vcall2) { fail = "vcall2 raw indirect"; goto done; }
				/* rgctx (ftndesc.arg) is folded into csig as a trailing param + call->args, so it's
				 * handled like any other arg — no separate rgctx_arg_reg (that field is LLVM-only). */
				memset (&ct, 0, sizeof (ct));
				if (csig->hasthis) { if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "creg nargs"; goto done; } ct.params [ct.nparams++] = WASM_I32; }
				for (ai = 0; ai < (int) csig->param_count; ++ai) {
					WasmValtype pv = wasm_valtype_of_type (csig->params [ai]);
					if (pv == 0 || pv == WASM_VOID) {
						fail = "creg arg type";
						fail_sig_site = "callreg"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = ai;
						goto done;
					}
					if (ct.nparams >= WASM_FUNCTYPE_MAX_PARAMS) { fail = "creg nargs"; goto done; }
					ct.params [ct.nparams++] = pv;
				}
				if (csig->ret->type == MONO_TYPE_VOID) ct.ret = WASM_VOID;
				else { ct.ret = wasm_valtype_of_type (csig->ret); if (ct.ret == 0 || ct.ret == WASM_VOID) {
					fail = "creg ret type";
					fail_sig_site = "callreg"; fail_sig_callee = call->method; fail_sig = csig; fail_sig_arg = -1;
					goto done;
				} }
				for (k = 0; k < nextra; ++k) if (functype_eq (&extra_types [k], &ct)) { type_idx = ti_base + k; break; }
				if (type_idx < 0) { if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; } extra_types [nextra] = ct; type_idx = ti_base + nextra++; }
				uses_calls = TRUE;
				nmeth = csig->param_count + (csig->hasthis ? 1 : 0);
				if (!call->call_info) { fail = "no captured callreg args"; goto done; }
				for (ai = 0; ai < nmeth; ++ai)
					if (!wasm_ld (&body, &lc, wj_arg_vreg (call, ai))) { fail = "creg arg ld"; goto done; }
				if (!wasm_ld (&body, &lc, ins->sreg1)) { fail = "creg target ld"; goto done; }
					if (is_membase) { wasm_op (&body, WASM_OP_I32_LOAD); wasm_memarg (&body, 2, (guint32) ins->inst_offset); } /* target = *(base + offset) */
				wj_emit_dynamic_call (&body, (guint32) type_idx);
				if (ct.ret != WASM_VOID && !wasm_st (&body, &lc, ins->dreg)) { fail = "creg dreg"; goto done; }
				break;
			}
			default:
				fail = "unsupported opcode";
				fail_op = ins->opcode;
				goto done;
			}
		}

		if (!terminated) {
			wasm_origin_push (&body, (bb == cfg->bb_exit || !bb->next_bb) ? WJO_RETURN : WJO_BB_FALL);
			/* fall through to the next block, or return at the method exit */
			if (bb == cfg->bb_exit || !bb->next_bb) {
				/* Key on the WASM return type (ret_vt), NOT the managed one: a hidden-vret method has a
				 * non-void MANAGED return but a VOID wasm result — its value was already stored through
				 * the trailing vret pointer, so its exit is a plain void return. (Keying on the managed
				 * type made every vret method's exit emit `unreachable` -> RuntimeError on first return,
				 * e.g. JsonDocument StackRowStack:Pop.) */
				gboolean ret_loaded = ret_vt == WASM_VOID;
				if (!ret_loaded && cfg->ret) {
					ret_loaded = self_ci.ret.kind == WJ_ARG_VTYPE_SCALAR
						? wj_emit_scalar_vtype_arg (&body, &lc, sig->ret, cfg->ret->dreg)
						: wasm_ld (&body, &lc, cfg->ret->dreg);
				}
				if (!ret_loaded) {
					/* No return value to load here = this exit is unreachable (e.g. a method that ALWAYS
					 * throws — every path took OP_THROW's dummy-return). Emit unreachable to satisfy the
					 * function result type instead of bailing ("ret epilogue"). Reachable non-void exits
					 * load the ret value (wasm_ld above) + fall to the RETURN below. */
					wasm_op (&body, WASM_OP_UNREACHABLE);
				} else {
					EMIT_REF_LEAVE ();
					wasm_op (&body, WASM_OP_RETURN);
				}
			} else {
				/* plan A5 v2: after wj_cfg_sort the layout successor is no longer the CFG one; a block that
				 * falls through has exactly one successor (wj_cfg_sort refuses anything else) */
				GOTO ((cfg_sorted && bb->out_count == 1) ? bb->out_bb [0] : bb->next_bb, 0);
			}
		}

		/* LCSE: hand this block's exit state to whichever single-predecessor successor inherits it. */
		if (lcse && lcse_exit && i < N && lcse_exit [i])
			*lcse_exit [i] = *lcse;
	}
#ifdef HOST_BROWSER
	WJ_COLD_ACCOUNT ();
	if (cfg->wasm_jit_tier >= 2)
		mono_wasm_jit_counters [WJC_COLD_T2_BYTES_ALL] += body.len - wj_cold_start;
#undef WJ_COLD_ACCOUNT
#endif

	if (G_UNLIKELY (lcse != NULL)) {
		extern gboolean mono_wasm_jit_refdiag_name (const char *);
		if (cfg->method && mono_wasm_jit_refdiag_name (cfg->method->name))
			printf ("WASM_JIT_LCSE %s: adds=%d hits=%d inherited-nonempty-bbs=%d\n",
				cfg->method->name, lcse_adds, lcse_hits, lcse_inherits);
	}
	/* The table describes nothing outside the block loop; drop it before the shared raise sites and
	 * the epilogue so a stale entry cannot be consulted. */
	lcse = NULL;

	wasm_origin_push (&body, WJO_DISPATCH);
	if (structured_cfg_kind == WJ_CFG_SINGLE_LOOP && structured_loop_l + 1 == N)
		wasm_op (&body, WASM_OP_END);          /* loop reaches the method's final bb */
	if (structured_cfg_kind == WJ_CFG_NESTED)
		for (i = 0; i < nest.nclose [N]; ++i)
			wasm_op (&body, WASM_OP_END);      /* loops whose interval reaches the method's final bb */
	if (!skip_dispatch) {
		wasm_op (&body, WASM_OP_END);          /* close loop */
		wasm_op (&body, WASM_OP_UNREACHABLE);  /* loop never falls through */
	}

	wasm_origin_push (&body, WJO_THROW_SITES);
	/* Shared raise sites, innermost first: close throw[t], then emit its one call to
	 * mono_wasm_jit_raise_corlib. Only reachable via `br_if $throw_t`, since the dispatch loop above
	 * never falls through and every raise ends in `unreachable`.
	 *
	 * Each block materializes the lazy ref frame first. mono_wasm_jit_raise_corlib ALLOCATES (the
	 * exception object, its stack trace), so a collection can happen here, and with lazy_ref_frame the
	 * method's roots may still be nowhere but unscannable wasm locals -- exactly the MONO_WASM_JIT_RAISE_NOGC
	 * corruption documented at wj_ins_is_gcpoint. This is the same shape as the conditional safepoint
	 * poll: materialize inside the rare taken arm, leaving the hot not-taken path frame-free. No
	 * RELEASE_LAZY_REF_FRAME follows, because the raise never returns; the C-stack SP is resynced from
	 * the boundary snapshot in mono_wasm_jit_invoke_caught's thrown path. */
	if (nthrow > 0) {
		WasmFuncType st; int sti = -1, sk, eid;
		memset (&st, 0, sizeof (st)); st.params [0] = WASM_I32; st.nparams = 1; st.ret = WASM_VOID;
		for (sk = 0; sk < nextra; ++sk) if (functype_eq (&extra_types [sk], &st)) { sti = ti_base + sk; break; }
		if (sti < 0) {
			if (nextra >= WJ_EXTRA_TYPES_MAX) { fail = "too many callee types"; goto done; }
			extra_types [nextra] = st; sti = ti_base + nextra++;
		}
		uses_calls = TRUE;
		for (i = 0; i < nthrow; ++i) {
			eid = -1;
			for (sk = 0; sk < WJ_EXC_IDS; ++sk) if (wj_throw_slot [sk] == i) { eid = sk; break; }
			if (eid < 0) { fail = "throw slot without exc id"; goto done; }
			wasm_op (&body, WASM_OP_END);   /* close throw[i]; br_if $throw_i lands here */
			RAISE_ENSURE_REF_FRAME ();
			wasm_i32_const (&body, eid);
#ifdef HOST_BROWSER
			{ extern void mono_wasm_jit_raise_corlib (int exc_id); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_raise_corlib, sti); }
#else
			wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff8, (guint32) sti);
#endif
			wasm_op (&body, WASM_OP_UNREACHABLE);   /* raise_corlib C++-throws; never returns */
		}
	}

	wasm_origin_push (&body, WJO_LANDING);
	if (eh_on) {
		/* In-method EH landing pad: `catch <x.e>` of the wrapping try. The C++ exc ptr (i32) is pushed.
		 * h = eh_dispatch(table, $blk): the throwing bb's matching catch handler bbidx, or -1. h<0 ->
		 * rethrow (propagate to an outer try / the interp boundary). h>=0 -> claim the C++ exc + set
		 * $blk=h + br $outer to re-dispatch into the handler bb. */
		wasm_op (&body, WASM_OP_CATCH); wasm_uleb (&body, 0);   /* tag 0 = x.e */
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) eh_exc_idx);
#ifdef HOST_BROWSER
		/* R353: this frame is the one resuming, so its record is the current island (dispatch and
		 * wj_eh_restore_lmf read cur_island_il_state in record mode; WJC_EHREC_DISPATCH_NOREC checks it). */
		if (ehrec_on) {
			wasm_op (&body, WASM_OP_GLOBAL_GET); wasm_uleb (&body, 8);
			EHREC_ADDR (mono_wasm_jit_ehrec_layout ()->il);
			wasm_op (&body, WASM_OP_I32_STORE); wasm_memarg (&body, 2, 0);
		}
		wasm_i32_const (&body, (gint32) (intptr_t) eh_table);
#else
		wasm_i32_const (&body, 0x7ff0);   /* offline: placeholder EH-table addr */
#endif
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) dispatch_idx);   /* $blk = the throwing bb */
#ifdef HOST_BROWSER
		{ extern int mono_wasm_jit_eh_dispatch (WasmEhTable *t, int blk); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_eh_dispatch, eh_dispatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff1, (guint32) eh_dispatch_ti);
#endif
		wasm_op_local (&body, WASM_OP_LOCAL_TEE, (guint32) eh_h_idx);
		wasm_i32_const (&body, 0);
		wasm_op (&body, WASM_OP_I32_LT_S);
		wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);     /* h < 0: no local handler */
		/* the method escapes via rethrow -> pop its il_state island first */
#ifdef HOST_BROWSER
		if (ehrec_on)
			EMIT_EHREC_POP ();
		else
		{ extern void mono_wasm_jit_leave_island (void); wj_emit_helper_call (&body, (gpointer) mono_wasm_jit_leave_island, eh_endcatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff7, (guint32) eh_endcatch_ti);   /* leave_island ()->void */
#endif
		wasm_op (&body, WASM_OP_RETHROW); wasm_uleb (&body, 1); /* depth 1 = the enclosing try -> re-propagate */
		wasm_op (&body, WASM_OP_END);                          /* end if */
		/* matched: claim+release the C++ exception (balance the cxa handler count), then dispatch. */
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_exc_idx);
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_begin_catch (void *e); wj_emit_helper_call (&body, (gpointer) mono_jiterp_begin_catch, eh_type_idx); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff2, (guint32) eh_type_idx);   /* (i32)->void */
#endif
#ifdef HOST_BROWSER
		{ extern void mono_jiterp_end_catch (void); wj_emit_helper_call (&body, (gpointer) mono_jiterp_end_catch, eh_endcatch_ti); }
#else
		wj_emit_helper_call (&body, (gpointer) (intptr_t) 0x7ff3, (guint32) eh_endcatch_ti);   /* ()->void */
#endif
		/* REF-SP-1: restore __stack_pointer to THIS method's frame base before re-dispatching into the
		 * handler. A C++/wasm-EH unwind into this catch skipped the EMIT_REF_LEAVE of every JITted frame
		 * it tore through (nested non-EH callees, and EH callees that escaped via their own h<0 rethrow),
		 * potentially leaving the SP below this frame. stackRestore(refbase) pops exactly those callee
		 * frames — they fall below the SP and stop being GC-scanned (no zeroing needed) — while this
		 * method's OWN frame [refbase, entry_sp) stays live for the handler. Idempotent across repeated
		 * catches (e.g. a finally that re-raises and is re-caught). Unconditional: an eh_on method always
		 * captures refbase in the prologue (== entry_sp for an empty frame). leave_ti is registered for
		 * eh_on. */
#ifdef HOST_BROWSER
		{
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) refbase_idx);
			wasm_op (&body, WASM_OP_GLOBAL_SET); wasm_uleb (&body, 0);
		}
#endif
		/* milestone 2c: mark the EXCEPTION path (finally_ind = -1, so OP_ENDFINALLY re-raises) ONLY when
		 * the matched handler IS a finally/fault — eh_dispatch tags those with WJ_EH_DISPATCH_FINALLY_BIT.
		 * The old unconditional set clobbered the OP_CALL_HANDLER continuation whenever a CATCH matched
		 * while a NORMAL-path finally body was executing (`finally { try { close(); } catch { } }`, the
		 * common Java/IKVM cleanup shape): after the catch swallowed, that finally's OP_ENDFINALLY read
		 * -1 and took the re-raise path — popping an UNRELATED outer finally's saved exception (cross-
		 * frame exception theft + gchandle double-free) or synthesizing a spurious EEE on a normal leave.
		 * After setting the marker, strip the tag so $blk receives the plain handler bbidx. Finally
		 * methods only (a catch-only method's dispatch never tags, and its pad has no finally_ind use). */
		if (eh_has_finally) {
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
			wasm_i32_const (&body, WJ_EH_DISPATCH_FINALLY_BIT);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op (&body, WASM_OP_IF); wasm_u8 (&body, 0x40);    /* tagged: finally/fault handler */
			wasm_i32_const (&body, -1);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) finally_ind_idx);
			wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
			wasm_i32_const (&body, ~WJ_EH_DISPATCH_FINALLY_BIT);
			wasm_op (&body, WASM_OP_I32_AND);
			wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) eh_h_idx);
			wasm_op (&body, WASM_OP_END);                          /* end if (untagged catch: finally_ind untouched) */
		}
		wasm_op_local (&body, WASM_OP_LOCAL_GET, (guint32) eh_h_idx);
		wasm_op_local (&body, WASM_OP_LOCAL_SET, (guint32) dispatch_idx);   /* $blk = handler bbidx */
		wasm_op (&body, WASM_OP_BR); wasm_uleb (&body, 1);     /* depth 1 = $outer loop -> re-dispatch */
		wasm_op (&body, WASM_OP_END);          /* close try */
		wasm_op (&body, WASM_OP_END);          /* close $outer loop */
		wasm_op (&body, WASM_OP_UNREACHABLE);  /* $outer loop never falls through */
	}

#undef GOTO
#undef WJ_HAS_THROW
#undef WJ_THROW_BR
#undef GOTO_IS_FWD
#undef COND_BRANCH
#undef NN_OK
#undef NN_GET
#undef NN_SET
#undef NN_KILL
#undef BIN
#undef BINI32
#undef BINI64
#undef BINI64L

	/* entry thunk: (args_ptr, ret_ptr)->void — reads each arg from the interp stack at
	 * args_ptr + its interp arg offset (mono_wasm_jit_sig_arg_offsets: one 8-byte stackval per
	 * scalar arg, so all-scalar sigs load at exactly i*8), calls the method (func 0), stores the
	 * result at ret_ptr. Lets the interpreter invoke any signature uniformly via e(args, ret). */
	{
		WasmBuf ethunk;
		gboolean has_ret = (ret_vt != WASM_VOID);
		guint32 aoffs [WASM_FUNCTYPE_MAX_PARAMS];
		extern int mono_wasm_jit_sig_arg_offsets (MonoMethodSignature *asig, guint32 *offs, int max);
		if (mono_wasm_jit_sig_arg_offsets (sig, aoffs, WASM_FUNCTYPE_MAX_PARAMS) != nargs) { fail = "ethunk arg offsets"; goto done; }
		wasm_buf_init (&ethunk);
		wasm_buf_init_relocs (&ethunk);
		if (has_ret)
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 1); /* ret_ptr (store address) */
		for (i = 0; i < nargs; ++i) {
			WasmOpcode ld; guint32 al;
			if (self_ci.args [i].kind == WJ_ARG_VTYPE_BYADDR) {
				/* pass the ADDRESS of the struct stored inline in the interp args area (args_ptr + off).
				 * The area is GC-scanned (interp stack) and callee-owned after the call, so the callee
				 * reading or mutating through the pointer is byval-correct with no copy. */
				wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 0); /* args_ptr */
				if (aoffs [i]) { wasm_i32_const (&ethunk, (gint32) aoffs [i]); wasm_op (&ethunk, WASM_OP_I32_ADD); }
				continue;
			}
			switch (param_types [i]) {
			case WASM_I64: ld = WASM_OP_I64_LOAD; al = 3; break;
			case WASM_F32: ld = WASM_OP_F32_LOAD; al = 2; break;
			case WASM_F64: ld = WASM_OP_F64_LOAD; al = 3; break;
			default:       ld = WASM_OP_I32_LOAD; al = 2; break;
			}
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 0); /* args_ptr */
			wasm_op (&ethunk, ld);
			wasm_memarg (&ethunk, al, aoffs [i]);
		}
		if (self_ci.vret_byaddr)
			wasm_op_local (&ethunk, WASM_OP_LOCAL_GET, 1); /* trailing vret param = ret_ptr: the interp stores VT returns inline at the retval slot, exactly where the callee writes */
		/* The thunk calls its own method. That function index is (this module's import count + this
		 * member's index) and NEITHER is known here -- a later member can still intern an import that
		 * shifts every defined function up. It used to be a 5-byte padded ULEB plus a recorded offset for
		 * batch_finish to patch; it is now simply a hole the assembler fills, which is the same trick with
		 * none of the bookkeeping. `tpool` 1 is the thunk's own signature, unused for a SELF reloc but
		 * kept well-formed. */
		/* THE BOUNDARY. `e` is the one place an interp->JIT transition enters the tier, so it is where
		 * the frame base comes from -- one `global.get` per transition instead of one per method entry.
		 * That ratio is the whole point of threading: 2.4M transitions against 974M dispatches in a 45 s
		 * window, i.e. ~409x fewer reads of the imported mutable global. It is also where the reserved
		 * CHUNK will be taken once the two `global.set`s go, at which point this read disappears too.
		 * The 8 `s.*` globals are imported unconditionally (wasm-encoder.c:404-411), so this needs no
		 * uses_calls flag to keep global 0 declared. */
		wasm_reloc (&ethunk, WASM_RELOC_SELF, 1, 0, NULL);
		if (has_ret) {
			WasmOpcode st; guint32 al;
			switch (ret_vt) {
			case WASM_I64: st = WASM_OP_I64_STORE; al = 3; break;
			case WASM_F32: st = WASM_OP_F32_STORE; al = 2; break;
			case WASM_F64: st = WASM_OP_F64_STORE; al = 3; break;
			default:       st = WASM_OP_I32_STORE; al = 2; break;
			}
			wasm_op (&ethunk, st);
			wasm_memarg (&ethunk, al, 0); /* *ret_ptr = result */
		}
		{
			/* A lone method is a one-member module, framed by the same assembler a batch uses. Import
			 * slots are assigned as the holes are walked -- method body first, then entry thunk, in
			 * emission order -- which is exactly the order the pre-relocation emitter assigned them in,
			 * so the bytes are unchanged. */
			/* am [0] is the method. am [1..] used to be SHADOWS -- private unexported duplicates of
			 * callees, framed so their call sites became `call <funcidx>`. See the note where
			 * wj_collect_shadows was for why they went. */
			WjAsmMember am [1];
			WjAsmPolicy pol;
			memset (am, 0, sizeof (am));
			am [0].method = cfg->method;
			am [0].name = mname;   /* already resolved above, on this (the compiling) thread */
			/* The target of the cycle test. Zero when this method has neither a live slot nor a
			 * reservation, which means its pair is allocated below, after framing -- see wj_asm_reaches
			 * for why a slot that does not exist yet is safe rather than unknown.
			 *
			 * HOST_BROWSER only, and not merely because there is nothing to import offline: both of these
			 * live in the interpreter (transform.c), which mono-aot-cross does not link. csyn.sh compiles
			 * this file for both targets and still cannot see that -- it is a LINK error, and it is the
			 * one class of breakage a per-file check cannot catch. */
#ifdef HOST_BROWSER
			{ extern int mono_wasm_jit_self_reserved (MonoMethod *m, int *e_out, int *f_out);
			  extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
			  int se = 0, sf = 0;
			  if (!mono_wasm_jit_self_reserved (cfg->method, &se, &sf))
				sf = mono_wasm_jit_get_callee_fslot (cfg->method);
			  am [0].f_slot = sf > 0 ? sf : 0; }
#endif
			am [0].f_body = &body;
			am [0].e_body = &ethunk;
			am [0].param_types = param_types;
			am [0].nparams = (guint32) nwparams;
			am [0].ret_type = ret_vt;
			am [0].extra_types = extra_types;
			am [0].nextra = (guint32) nextra;
			am [0].groups = groups;
			am [0].nlocal_groups = 4;
			am [0].uses_calls = uses_calls ? 1 : 0;
			am [0].uses_eh_tag = uses_eh_tag ? 1 : 0;
			am [0].eh_tpool = eh_type_idx;
			wj_asm_policy_init (&pol, FALSE /* keep self-recursion indirect, as it has always been */);
			wasm_buf_init (&out);
			/*
			 * TAKE THE ASSEMBLER'S DEPENDENCY SET HERE TOO, and union it with the emit-time one.
			 *
			 * This used to pass out_deps NULL, on the reasoning that "a lone method's dependency set is
			 * the emit-time one from wj_result_add_direct_dep ... only a re-framing changes which calls
			 * became module-local". The first half of that does not hold: the emit-time set is recorded
			 * by hand at the sites that happen to call the recorder, so it is only as complete as that
			 * list, while the RELOCATIONS are what the bytes are actually built from. R285 caught the
			 * difference in the act -- `i32.const 467053; call_indirect` baked into a live body with no
			 * descriptor anywhere declaring slot 467053, so admission had nothing to install, the closure
			 * walk completed clean (`install_budget_out=0`), and the first worker to reach that call
			 * without having installed the callee for some other reason found
			 * mono_jiterp_placeholder_jit_call in the table and trapped with `function signature
			 * mismatch`.
			 *
			 * Deriving from relocs closes that by construction and needs no audit of which emitter site
			 * remembered to record: every baked f-slot reaches the encoder as a reloc, because that is
			 * how the constant is emitted at all (wasm-encoder.c's CALL arm writes
			 * `i32.const r->table_index; call_indirect`). It is also the mechanism the BATCHED path has
			 * always used, so this makes the two paths agree rather than introducing a third.
			 *
			 * THE SIG IS THE ASSEMBLER'S OWN, deliberately: wj_asm_deps_add stores
			 * wj_functype_hash (&self_types [r->tpool]), the same value wj_result_add_direct_dep is given
			 * at the emit sites -- so a slot recorded both ways DEDUPES rather than looking like one slot
			 * under two ABIs, which is what would (correctly) reject the emission.
			 *
			 * COUNTED, because the interesting number is how many deps only the relocs knew about. A zero
			 * would mean the emit-time recording was already complete and this is inert; a large number
			 * is the size of the hole R285 found one instance of.
			 */
			/* HOST_BROWSER-ONLY, and structurally so: wj_result_add_direct_dep lives inside
			 * mini-wasm-ir.inc's `#ifdef HOST_BROWSER` region, while this framing block also runs in the
			 * offline cross-compiler dump path -- which has no admission and therefore no use for a
			 * dependency set. An unguarded call here compiles in the browser database and fails the
			 * cross LINK, the gap csyn.sh cannot see. */
#if HOST_BROWSER
			{
				WjAsmDeps sdeps;
				int di;
				memset (&sdeps, 0, sizeof (sdeps));
				wj_assemble (am, 1, 1, &pol, &out, NULL, &sdeps);
				for (di = 0; di < sdeps.n; ++di) {
					int before = cfg->wasm_jit_result.ndirect_deps;
					wj_result_add_direct_dep (&cfg->wasm_jit_result, sdeps.slot [di],
					                          sdeps.sig [di], sdeps.method [di]);
					if (cfg->wasm_jit_result.ndirect_deps != before)
						mono_wasm_jit_counters [WJC_DEPS_FROM_RELOCS]++;
				}
				wj_asm_deps_free (&sdeps);
				/* Same gate the emit sites apply: an incomplete set must not reach registration, because
				 * an undeclared baked slot is exactly the defect this block exists to remove. */
				if (cfg->wasm_jit_result.direct_deps_truncated) {
					fail = "too many direct dependencies (reloc-derived)";
					goto done;
				}
			}
#else
			wj_assemble (am, 1, 1, &pol, &out, NULL, NULL);
#endif
			/* Keep the relocatable body. Framing consumed nothing -- the buffers still hold instruction
			 * bytes and relocations, unresolved -- so this method can later be re-framed alongside others
			 * without being compiled again. Attached to the registry on success below; freed at `done`
			 * if registration never happens. */
			wj_stored_body = wj_body_take (cfg->method, mname, &body, &ethunk, param_types, (guint32) nwparams,
			                               ret_vt, groups, extra_types, (guint32) nextra,
			                               uses_calls, uses_eh_tag, eh_type_idx);
			if (wj_stored_body)
				wj_stored_body->t1down = cfg->wasm_jit_t1_down;
		}
		wasm_buf_free (&ethunk);
	}
	/* R349: a tier-2 body over the cap is refused here, before V8 ever sees it. TurboFan compiling one
	 * ~300 KB function (WorldRenderer.render with ~1,500 inlines) transiently committed 5-6 GB of chromium's
	 * C++ heap -- a 14-16 GB VmData peak in every run that promoted it -- while no run whose largest tier-2 body
	 * was <= ~101 KB spiked. A permanent (non-retriable) fail: a re-emit leaves the method's tier-1 body live, and
	 * mono_wasm_force_compile's downgrade retries once at the ordinary policy when lifted call inlining was used. */
	{ extern int mono_wasm_jit_t2_max_body;
	  /* R460: the module V8 compiles -- not the ORIGIN diagnostic riding in it (0 bytes in a normal run) */
	  guint32 cap_len = out.len - MIN (out.len, wasm_last_origin_section_len);
	  if (!fail && cfg->wasm_jit_tier >= 2 && mono_wasm_jit_t2_max_body > 0 && cap_len > (guint32) mono_wasm_jit_t2_max_body) {
		mono_wasm_jit_count (WJC_T2_BODY_CAPPED);
		/* ~30 per run, so ungated: the size the downgrade discards, and under CAPTURE the bytes, kind "m2cap", so
		 * sitebytes.py --kinds m2cap can say what a giant's tier-2 body carries */
		printf ("[wasm-jit t2-cap] refused %u B (cap %d, module %u B) %s\n", cap_len, mono_wasm_jit_t2_max_body, out.len,
			mname ? mname : "?");
#ifdef HOST_BROWSER
		{ extern int mono_wasm_jit_capture;
		  if (G_UNLIKELY (mono_wasm_jit_capture))
			mono_wasm_jit_capture_module ("m2cap", mname, out.data, out.len); }
#endif
		fail = "tier-2 body over MONO_WASM_JIT_T2_MAX_BODY";
		wasm_buf_free (&out);   /* freed before `done:` on the normal path (below), so free it here */
		goto done;
	  } }
#ifdef HOST_BROWSER
	{ extern int mono_wasm_jit_t2_unit;
	  if (G_UNLIKELY (mono_wasm_jit_t2_unit == 2) && !fail && cfg->wasm_jit_tier >= 2 && wj_stored_body)
		wj_t2u_census (cfg->method, wj_stored_body);
	}
#endif

#ifdef HOST_BROWSER
	{
		/* Live single-method hex dump, selected by full-name substring (MONO_WASM_JIT_HEX_METHOD).
		 *
		 * DUMP_ONLY cannot answer "what does the failing run actually execute". It suppresses registration
		 * for EVERY method, so no callee owns an f-slot and the emitter takes different direct-call and
		 * devirt decisions -- the bytes it prints are a different module than the live one. Filtering by
		 * name leaves the rest of the process on its normal path, so the dumped module IS the one that
		 * runs. */
		{ extern int mono_wasm_jit_capture;
		  if (G_UNLIKELY (mono_wasm_jit_capture) && !fail)
			mono_wasm_jit_capture_module (cfg->wasm_jit_tier >= 2 ? "m2" : "m1", mname, out.data, out.len); }
		const char *want = g_getenv ("MONO_WASM_JIT_HEX_METHOD");
		if (want && *want && mname && strstr (mname, want)) {
			GString *hex = g_string_sized_new (out.len * 2 + 1);
			for (i = 0; i < (int) out.len; ++i)
				g_string_append_printf (hex, "%02x", out.data [i]);
			printf ("WASM_JIT_LIVEHEX %s : %u : %s\n", mname, out.len, hex->str);
			g_string_free (hex, TRUE);
		}
	}
	if (g_getenv ("MONO_WASM_JIT_DUMP_ONLY") != NULL) {
		/* debug: forward the emitted module as hex to the UI console but DON'T register it, so the
		 * method runs in the interpreter — lets us inspect JIT-path codegen without executing it. */
		GString *hex = g_string_sized_new (out.len * 2 + 1);
		for (i = 0; i < (int) out.len; ++i)
			g_string_append_printf (hex, "%02x", out.data [i]);
		{ printf ("WASM_JIT_HEX %s : %u : %s\n", mname, out.len, hex->str); }
		g_string_free (hex, TRUE);
	} else if (g_getenv ("MONO_WASM_JIT_DUMP_EXIT") == NULL) {
		/* live runtime: allocate the (global) table slots + cache the emitted module bytes. The wasm
		 * function table is PER-THREAD for dynamically-added entries, so instantiation is deferred to
		 * the interp's first invoke on each thread (interp.c MINT_CALL → mono_wasm_jit_instantiate_local),
		 * which instantiates its own WebAssembly.Instance into its own table[e_slot]/[f_slot]. */
		/* Slot selection: (1) an SCC batch reservation on our imethod (multi-method cycle — the orchestrator
		 * reserved these and publishes us); (2) this method's own self-recursion reservation, which
		 * mono_wasm_jit_self_reserved also returns (it now lives on the imethod, so it survives a failed emit
		 * and a re-emit reuses it); (3) fresh allocation. Instantiate INTO the reserved pair so baked
		 * self/cross-cycle calls resolve. */
		int e_slot = 0, f_slot = 0;
		{ extern int mono_wasm_jit_self_reserved (MonoMethod *m, int *e_out, int *f_out);
		  if (!mono_wasm_jit_self_reserved (cfg->method, &e_slot, &f_slot)) {
			e_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			f_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			/* Exhaustion is terminal for the JIT and otherwise invisible — the guard below just declines to
			 * register and the tier quietly stops. Count it so mono_wasm_jit_liveness(3) shows it. */
			if (e_slot <= 0 || f_slot <= 0) {
				extern void mono_wasm_jit_note_table_exhausted (void);
				mono_wasm_jit_note_table_exhausted ();
			}
		  } }
#ifdef HOST_BROWSER
		/* Phase 5: a pool pair's stubs are of the functype the callers reserved it under. A body compiled to any
		 * other type must not go in behind them; take a fresh pair and let the stubs route to the interpreter. */
		{ WasmFuncType pft;
		  if (f_slot > 0 && wj_lazy_pool_ft (f_slot, &pft) && !functype_eq (&pft, &self_ci.ftype)) {
			mono_wasm_jit_counters [WJC_LAZY_SIG_DRIFT]++;
			e_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
			f_slot = mono_jiterp_allocate_table_entry (1 /* JITERPRETER_TABLE_JIT_CALL */);
		  } }
#endif
		/* H3: a body swap since this compile began -- it may have read the displaced IL. Install it nowhere, not even
		 * here; the swap queued the method's own re-emission. Retriable: the next attempt reads the new body. */
		if (e_slot > 0 && f_slot > 0 && G_UNLIKELY (wj_snap_stale (cfg->method))) {
			mono_wasm_jit_counters [WJC_EPOCH_REFUSED_LOCAL]++;
			cfg->wasm_jit_result.retriable = 1;
		} else if (e_slot > 0 && f_slot > 0) {
			void *cached = g_malloc ((gsize) out.len);
			guint32 payload = wj_payload_new ();   /* W1 C0: this module, here and in the registry */
			char ierr [192]; ierr [0] = 0;
			memcpy (cached, out.data, out.len);
			/* Validate by instantiating once on THIS thread (also populates this thread's table).
			 * If the module is invalid (a codegen bug for some opcode shape), bail to the interpreter
			 * here rather than letting another thread trap on a placeholder slot at invoke time. */
			/* The imports this module is about to bind were chosen against the REGISTRY, which is
			 * process-wide; whether they are instantiated is per-thread. Make them live here first. */
			/* R322: a RE-EMIT into a slot this thread already installed must not replace the body here before the new
			 * body's dependencies are installed here -- callers that baked the slot would reach it at once. Validate
			 * only; the republication makes this thread re-admit, walking the new depset first. */
			extern int mono_wasm_jit_reemit_validate;
			gboolean slot_live_here = wj_slot_is_installed (f_slot) || wj_slot_is_installed (e_slot);
			gboolean reemit_live = mono_wasm_jit_reemit_validate && slot_live_here;
			if (reemit_live)
				wj_count (WJC_REEMIT_VALIDATE_ONLY);
			/* Phase 5, invariant 1: callers may already call a pool f-slot through its stub, so the real f goes in
			 * only after its dependencies are installed here -- which is admission's job, not this validation's. */
			if (!reemit_live && wj_lazy_is_pool_slot (f_slot)) {
				reemit_live = TRUE;
				mono_wasm_jit_counters [WJC_LAZY_VALIDATE_ONLY]++;
			}
			if (slot_live_here) {
				int k;
				for (k = 0; k < cfg->wasm_jit_result.ndirect_deps; ++k) {
					int ds = cfg->wasm_jit_result.direct_deps [k];
					if (ds > 0 && ds != f_slot && !wj_slot_is_installed (ds)) {
						mono_wasm_jit_counters [WJC_REEMIT_HAZARD]++;   /* ungated: the window, prevented or not */
						break;
					}
				}
			}
			if (mono_wasm_jit_instantiate_local (reemit_live ? -1 : e_slot, reemit_live ? -1 : f_slot, cached, (int) out.len,
			                                     ierr, (int) sizeof (ierr), &wj_inst_ms, payload)) {
				/* The compile result is written onto cfg->wasm_jit_result (below). It's per-compile, so a
				 * re-entrant nested compile (cctors / AOT-target init) has its OWN cfg and can't clobber it —
				 * no per-thread-relay ordering dance needed. We still write the e_slot gate AFTER
				 * registration so it is only set once this method is findable by an admission walk. */
				if (G_UNLIKELY (mono_wasm_jit_stats)) {
					mono_wasm_jit_count (WJC_REGISTERED); mono_wasm_jit_add (WJC_BYTES_GENERATED, (gint64) out.len);
					if (self_has_byaddr) mono_wasm_jit_count (WJC_VT_BYADDR_METHODS);
					if (self_ci.vret_byaddr) mono_wasm_jit_count (WJC_VRET_METHODS);
				}
				{ extern int mono_wasm_jit_register (MonoMethod *, int, int, void *, int, guint32, gboolean, const int *, const guint32 *, MonoMethod *const *, int, guint32);
				  /* the fingerprint is the self-sig WasmCallInfo's — the same descriptor call sites hash
				   * into dep_sig, so caller/callee can never disagree structurally */
				  cfg->wasm_jit_result.f_sig_id = self_ci.f_sig_id;
				  cfg->wasm_jit_result.desc_id = mono_wasm_jit_register (cfg->method, e_slot, f_slot, cached, (int) out.len,
					cfg->wasm_jit_result.f_sig_id, method_nogc, cfg->wasm_jit_result.direct_deps,
					cfg->wasm_jit_result.direct_dep_sig, cfg->wasm_jit_result.direct_dep_method, cfg->wasm_jit_result.ndirect_deps, payload); }
				if (cfg->wasm_jit_result.desc_id <= 0) {
					/* -2: stale (H3), retriable; 0: refused for good. Either way this worker's table may now hold the
					 * module at fresh slots nothing else names, as before for a collision. */
					if (cfg->wasm_jit_result.desc_id == -2)
						cfg->wasm_jit_result.retriable = 1;
					cfg->wasm_jit_result.desc_id = 0;
					{ extern void mono_wasm_jit_bytes_forget (const void *bytes); mono_wasm_jit_bytes_forget (cached); }   /* R462 */
					g_free (cached);
					cached = NULL;
				} else {
					{ WjRegEntry *sre = wj_reg_at (cfg->wasm_jit_result.desc_id - 1);
					  if (sre) {
						/* A same-descriptor generation replaces the relocatable body too, and keeping the
						 * old one leaked every code buffer and relocation vector re-emission produced.
						 *
						 * RETIRED, NOT FREED. The comment this replaces said "compilation is
						 * process-serialized and admission never reads WjBody, so no concurrent reader can
						 * retain the superseded body". The first clause is true and the second is not:
						 * mono_wasm_jit_admit reaches wj_deps_arity_bad (mini-wasm.c) on its unconditional
						 * path, which dereferences dep->body->nparams for arbitrary DEPENDENCY descriptors
						 * on whatever worker is admitting -- and admission never takes wj_compiling, so
						 * being the only compiler buys nothing against it. Freeing here is a cross-thread
						 * use-after-free on a body some other worker is reading. */
						WjBody *old_body = sre->body;
						sre->body = wj_stored_body;
						wj_stored_body = NULL;
						{
							extern void mono_wasm_jit_edge_note_name (int desc, const char *name);
							mono_wasm_jit_edge_note_name (cfg->wasm_jit_result.desc_id, mname);
						}
						if (old_body && old_body != sre->body)
							wj_retire_payload (old_body, WJ_RETIRED_BODY);
					  } }
					if (mono_wasm_jit_verbose >= 1) {
						printf ("WASM_JIT_REGISTERED %s desc=%d e_slot=%d f_slot=%d len=%u deps=%d [", mname, cfg->wasm_jit_result.desc_id, e_slot, f_slot, (unsigned) out.len, cfg->wasm_jit_result.ndirect_deps);
						for (i = 0; i < cfg->wasm_jit_result.ndirect_deps; ++i)
							printf ("%s%d/0x%x", i ? "," : "", cfg->wasm_jit_result.direct_deps [i], cfg->wasm_jit_result.direct_dep_sig [i]);
						printf ("]\n");
					}
				}
				/* Write the success result onto cfg (read back by mono_wasm_force_compile after the compile
				 * returns, on this same thread). e_slot is the >0 success gate the readers test; no cross-thread
				 * barrier needed here — the result is consumed on this thread via cfg, and the cross-thread
				 * publish to InterpMethod.wasm_jit_slot keeps its own barrier in wasm_jit_compile_publish. */
				if (cached) {
					cfg->wasm_jit_result.bytes = cached;
					cfg->wasm_jit_result.bytes_len = (int) out.len;
					cfg->wasm_jit_result.f_slot = f_slot;
					cfg->wasm_jit_result.e_slot = e_slot;
				}
			} else {
				/* INSTANTIATION FAILED ON *THIS* WORKER -- OOM or a CompileError under memory pressure.
				 * That is a PER-WORKER event, but the result travels as a permanent bail: e_slot stays 0,
				 * wasm_jit_compile_publish reads `e_slot <= 0 && !retriable` as terminal, and
				 * wasm_jit_maybe_compile writes `wasm_jit_slot = -1` -- which is PROCESS-WIDE. One worker
				 * running out of memory therefore evicted the method from the tier for every thread, and
				 * recorded it as "this method cannot be compiled". R245 Stage 2c: the purest instance of
				 * the tiering/installation conflation in this file.
				 *
				 * Marked retriable instead. The bytes are gone, so this compile produced nothing; but the
				 * next attempt re-emits, and on a worker that is not under pressure it will instantiate.
				 * The caller parks rather than condemns, and the hit counter or a waiter brings it back.
				 * WJC_INVALID still counts the event, so the rate stays visible. */
				/* ... but ONLY when the failure can actually clear. A CompileError is V8 refusing to
				 * VALIDATE the bytes: emission is deterministic, so the next attempt builds the same
				 * module and gets the same error, forever. Routing that into the retry path is the
				 * unbounded-refusal trap CLAUDE.md records for admission, on the instantiation path:
				 * measured on class_3580:.cctor ("local count too large"), 15 recompiles of a ~1.5 s
				 * method back to back = 22.4 s of the boot window with `registered` flat. The specific
				 * cause of that one is now refused before emission (WASM_MAX_FUNCTION_LOCALS), but the
				 * CLASS is closed here: any future codegen bug that emits invalid bytes costs ONE
				 * compile, not an unbounded stall that reads as a hang.
				 *
				 * Everything else stays retriable, which is the case R245 Stage 2c added this for: an
				 * OOM or a LinkError is a property of THIS worker at THIS moment (imports are per-worker,
				 * memory pressure is transient), and re-emitting elsewhere succeeds.R289. */
				{ gboolean _perm = ierr [0] && strstr (ierr, "CompileError") != NULL;
				  cfg->wasm_jit_result.retriable = _perm ? 0 : 1;
				  if (G_UNLIKELY (mono_wasm_jit_stats) && _perm) mono_wasm_jit_count (WJC_INVALID_PERM); }
				{ extern void mono_wasm_jit_bytes_forget (const void *bytes); mono_wasm_jit_bytes_forget (cached); }   /* R462 */
				g_free (cached);
				if (G_UNLIKELY (mono_wasm_jit_stats)) mono_wasm_jit_count (WJC_INVALID);
				if (mono_wasm_jit_verbose >= 1) { printf ("WASM_JIT_INVALID %s e_slot=%d len=%u : %s\n", mname, e_slot, (unsigned) out.len, ierr); }
			}
		}
	} else
#endif
	{
		/* offline: dump module as hex for external validation */
		GString *hex = g_string_sized_new (out.len * 2 + 1);
		for (i = 0; i < (int) out.len; ++i)
			g_string_append_printf (hex, "%02x", out.data [i]);
		printf ("WASM_JIT_MODULE %s : %u bytes : %s\n", mname, out.len, hex->str);
		g_string_free (hex, TRUE);
	}
	wasm_buf_free (&out);

done:
	wj_edge_on = FALSE;
	wj_body_free (wj_stored_body);   /* NULL once the registry has taken it */
	wj_stored_body = NULL;
	wasm_buf_free (&body);
#ifdef HOST_BROWSER
#endif
	if (G_UNLIKELY (mono_wasm_jit_stats) && wj_gen_t0) {
		/* Compile-time accounting (Part 2): GENERATION = total emit time minus the JS instantiate
		 * (wj_inst_ms, folded into INSTANTIATION here) so the two halves don't overlap; COMPILE_ATTEMPTS
		 * counts every emit (success + bail + invalid). 100ns ticks -> microseconds (/10). */
		gint64 us = (mono_100ns_ticks () - wj_gen_t0) / 10 - (gint64) (wj_inst_ms * 1000.0);
		mono_wasm_jit_add (WJC_ELAPSED_GENERATION, us > 0 ? us : 0);
		mono_wasm_jit_add (WJC_ELAPSED_INSTANTIATION, (gint64) (wj_inst_ms * 1000.0));
		mono_wasm_jit_count (WJC_COMPILE_ATTEMPTS);
	}
#ifdef HOST_BROWSER
	/* Retriable iff the bail was "callee not jitted" (an ordering/island issue that may resolve once the
	 * callee JITs); EH clauses / unsupported opcodes / synchronized / byref are permanent. The auto-JIT
	 * trigger reads this to pick a retry vs permanent slot state.
	 *
	 * DOES NOT CLOBBER a retriable already set upstream. This was an unconditional `: 0`, which quietly
	 * undid the per-worker instantiate-failure case above: that path has no `fail` string (the EMIT
	 * succeeded; instantiation did not), so it scored 0 and travelled on as a permanent, process-wide
	 * bail. A `?:` that resets state it did not set is how a fix two hundred lines up becomes a no-op. */
	if (fail && strstr (fail, "callee not jitted"))
		cfg->wasm_jit_result.retriable = 1;
	/* Categorize the bail reason for the weighted vcall-residual breakdown (stored on InterpMethod by
	 * compile_publish only when the bail is permanent). Opcode bails carry the opcode number so the bench
	 * can name the dominant blocker (e.g. ldaddr 331); EH/sig/other get sentinel negatives. */
	if (!fail) cfg->wasm_jit_result.bail = 0;
	else if (fail_op == OP_LDADDR) cfg->wasm_jit_result.bail = -5;         /* ldaddr (needs addressable locals) */
	else if (fail_op == OP_LCOMPARE) cfg->wasm_jit_result.bail = -6;       /* lcompare */
	else if (fail_op >= 0) cfg->wasm_jit_result.bail = fail_op;            /* some other unsupported opcode (>0) */
	else if (strstr (fail, "EH clause kind")) cfg->wasm_jit_result.bail = -2;   /* the clause-kind gate: a filter (the only unsupported kind) */
	else if (strstr (fail, "arg type") || strstr (fail, "ret type")) cfg->wasm_jit_result.bail = -3;   /* arg/ret sig type */
	/* Split what used to be the -4 "other" catch-all so the vcall-perm breakdown is actionable (the bench
	 * showed -4 was 99% of the perm vcall residual). Order: most specific class first. */
	else if (strstr (fail, "byref")) cfg->wasm_jit_result.bail = -7;                                   /* byref arg/ret */
	/* -12 vs -8: an rgctx CALLSITE bail (this concrete method makes an indirect/virtual call that carries
	 * MONO_ARCH_RGCTX_REG — fixable per-site by routing that one call through the residual) is a different
	 * workstream from the whole-method gshared gate (the method ITSELF is generic-shared). The old lumped -8
	 * hid which one carries the ~800K weighted vperm fallbacks. */
	else if (strstr (fail, "rgctx")) cfg->wasm_jit_result.bail = -12;                                  /* rgctx callsite (indirect/virtual call carrying the callsite rgctx) */
	else if (strstr (fail, "gshared")) cfg->wasm_jit_result.bail = -8;                                  /* generic-shared method (whole-method gate) */
	else if (strstr (fail, "synchronized")) cfg->wasm_jit_result.bail = -9;                             /* synchronized method/wrapper */
	else if (strstr (fail, "EH") || strstr (fail, "finally") || strstr (fail, "eh ") || strstr (fail, "eh-")) cfg->wasm_jit_result.bail = -10; /* other EH reasons (not the -2 clause gate) */
	else cfg->wasm_jit_result.bail = -4;                                   /* genuinely other (unsupported IR shape: reg/move/sig/indirect/...) */
	cfg->wasm_jit_result.fail_reason = fail;   /* static literal (or NULL) — names the exact gate in the weighted vperm dump */
	/* No on-fail clear needed: the success fields (e_slot/f_slot/bytes) are only written on the
	 * instantiate-success path above, and this cfg is private to this compile — a nested re-entrant
	 * compile has its own cfg and cannot have set them here (the old per-thread-relay clobber is gone). */
#endif
	if (fail) {
		if (G_UNLIKELY (mono_wasm_jit_stats)) {
			int cat = -1;
			mono_wasm_jit_count (WJC_BAILED);
			/* Aggregated bail histogram (Part 4). Check opcode then "residual:" (the rm 2-5 shape bisection)
			 * BEFORE the generic "callee not jitted" (which the shape strings also contain). */
			if (fail_op >= 0) { cat = WJB_OPCODE; if (fail_op < WJ_BAIL_OPMAX) wj_bail_op_hist [fail_op]++; }
			else if (strstr (fail, "residual:")) cat = WJB_RESIDUAL_SHAPE;
			/* "callee not jitted" is NOT counted here: it is retriable and the island driver re-emits the
			 * same method every iteration until its callees JIT, so a per-attempt count mostly measures
			 * island convergence (profile4: 3066 of 7040 "bails" were these re-attempts). It is counted
			 * once per DISTINCT method at wasm_jit_compile_publish (mono_wasm_jit_bail_hist_note_blocked),
			 * so the histogram reflects terminal outcomes. */
			else if (strstr (fail, "callee not jitted")) cat = -1;
			/* every EH gate: the filter clause kind, the nested finally, the il_state cap. This matched "EH clauses", a
			 * string no gate produces, so all three read as synchronized/other. */
			else if (strstr (fail, "EH") || strstr (fail, "finally")) cat = WJB_EH_CLAUSE;
			else if (strstr (fail, "arg type") || strstr (fail, "ret type")) cat = WJB_ARGRET_TYPE;
			else cat = WJB_SYNC_OTHER;
			if (cat >= 0)
				wj_bail_hist [cat]++;
		}
		if (mono_wasm_jit_verbose >= 2) {
			printf ("WASM_JIT_BAIL %s : %s (op=%d %s)", mname, fail, fail_op, fail_op >= 0 ? wj_opname (fail_op) : "-");
			if (fail_sig_site)
				wj_print_bail_sig (fail_sig_site, fail_sig_callee, fail_sig, fail_sig_arg);
			printf ("\n");
		}
	}
	g_free (mname);
	if (g_getenv ("MONO_WASM_JIT_DUMP_EXIT") != NULL)
		exit (fail ? 1 : 0);
	/*
	 * v1 (in-runtime validation): mark the compile failed so the method falls back to
	 * the interpreter. We are validating emit+instantiate; installing the compiled code
	 * needs the calling-convention wrapper + a function-table slot (the next step).
	 */
	if (is_ok (cfg->error))
		mono_error_set_execution_engine (cfg->error, "wasm jit v1: validate-only, falling back to interp");
	cfg->exception_type = MONO_EXCEPTION_MONO_ERROR;
}

void
mono_arch_peephole_pass_1 (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

void
mono_arch_peephole_pass_2 (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

guint32
mono_arch_regalloc_cost (MonoCompile *cfg, MonoMethodVar *vmv)
{
	return 0;
}

GList *
mono_arch_get_allocatable_int_vars (MonoCompile *cfg)
{
	g_error ("mono_arch_get_allocatable_int_vars");
}

GList *
mono_arch_get_global_int_regs (MonoCompile *cfg)
{
	g_error ("mono_arch_get_global_int_regs");
}

void
mono_arch_allocate_vars (MonoCompile *cfg)
{
	g_error ("mono_arch_allocate_vars");
}

void
mono_arch_create_vars (MonoCompile *cfg)
{
	MonoMethodSignature *sig;
	CallInfo *cinfo;

	sig = mono_method_signature_internal (cfg->method);

	if (!cfg->arch.cinfo)
		cfg->arch.cinfo = get_call_info (cfg->mempool, sig);
	cinfo = (CallInfo *)cfg->arch.cinfo;

	// if (cinfo->ret.storage == ArgValuetypeInReg)
	// 	cfg->ret_var_is_local = TRUE;

	mini_get_underlying_type (sig->ret);
	if (cinfo->ret.storage == ArgValuetypeAddrInIReg || cinfo->ret.storage == ArgGsharedVTOnStack) {
		cfg->vret_addr = mono_compile_create_var (cfg, mono_get_int_type (), OP_ARG);
		if (G_UNLIKELY (cfg->verbose_level > 1)) {
			printf ("vret_addr = ");
			mono_print_ins (cfg->vret_addr);
		}
	}

	if (cfg->gen_sdb_seq_points)
		g_error ("gen_sdb_seq_points not supported");

	if (cfg->method->save_lmf) {
		cfg->create_lmf_var = TRUE;
		cfg->lmf_ir = TRUE;
	}
}

void
mono_arch_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	g_error ("mono_arch_emit_call");
}

/*
 * mono_wasm_emit_call:
 *
 *   COMPILE_WASM analog of mono_llvm_emit_call, invoked from mono_emit_call_args in place of
 * mono_arch_emit_call (an unimplemented stub here). It builds the WjCallArgs descriptor the emitter
 * reads its arguments from; see the comment on WjCallArgs above for why that descriptor holds
 * INSTRUCTIONS rather than vreg numbers.
 *
 * Scalar/ref/byref args get a real OP_*MOVE carrier into a fresh vreg, added to cfg->cbb and
 * registered in call->out_ireg_args -- LLVM's mechanism, and it is what makes these immune to the
 * pipeline:
 *   - SSA renaming only renumbers VARIABLES (get_vreg_to_inst != NULL) and multiply-defined local
 *     vregs; a carrier dreg is a fresh, singly-defined, non-variable vreg, so it is neither.
 *   - mono_ssa_deadce only nullifies defs of cfg->varinfo entries, so it cannot reach a carrier --
 *     while the carrier's sreg1 IS a recorded use, which is what keeps the SOURCE alive.
 *   - mono_local_deadce (local-propagation.c) and alias analysis (kill_call_arg_alias) walk
 *     out_ireg_args explicitly.
 * A ref arg's carrier dreg is classified ref by the fixpoint's OP_MOVE taint, costing one shadow
 * slot per ref arg per call site -- the price of rooting the argument across the call.
 *
 * Vtype args get NO carrier of their own: OP_VMOVE would need a fresh vtype VARIABLE, which is
 * exactly the renameable thing we are trying to avoid, and OP_LLVM_OUTARG_VT has no non-LLVM
 * decompose case. Instead the source variable is marked MONO_INST_INDIRECT, which takes it out of
 * SSA entirely (create_new_vars and mono_ssa_remove's coalescer both skip VOLATILE|INDIRECT vars),
 * so recording the source instruction is enough. That is also honest about the ABI: WJ_ARG_VTYPE_BYADDR
 * passes these BY ADDRESS out of an addr-frame slot, so their address is genuinely taken. Java (and
 * therefore IKVM) has no byval structs, so the lost optimization costs nothing on the target workload.
 */
void
mono_wasm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoMethodSignature *sig = call->signature;
	int n = sig->param_count + sig->hasthis, i;
	WjCallArgs *w = (WjCallArgs *) mono_mempool_alloc0 (cfg->mempool, sizeof (WjCallArgs));

	w->nargs = n;
	w->carrier = (MonoInst **) mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst *) * (n + 1));
	w->capvreg = (int *) mono_mempool_alloc (cfg->mempool, sizeof (int) * (n + 1));
	w->copyoff = (gint32 *) mono_mempool_alloc (cfg->mempool, sizeof (gint32) * (n > 0 ? n : 1));
	for (i = 0; i <= n; ++i)
		w->capvreg [i] = -1;
	for (i = 0; i < n; ++i)
		w->copyoff [i] = -1;

	for (i = 0; i < n; ++i) {
		MonoInst *in = call->args [i];
		MonoInst *ins;
		MonoType *t = (sig->hasthis && i == 0) ? mono_get_int_type () : sig->params [i - sig->hasthis];
		guint32 opcode = mono_type_to_regmove (cfg, t);

		if (opcode == OP_VMOVE || opcode == OP_XMOVE) {
			/* vtype: no carrier — pin the source variable out of SSA instead (see above) */
			MonoInst *var = get_vreg_to_inst (cfg, in->dreg);
			if (var)
				var->flags |= MONO_INST_INDIRECT;
			w->carrier [i] = in;
			w->capvreg [i] = in->dreg;
			mono_call_inst_add_outarg_reg (cfg, call, in->dreg, 0, FALSE);
			continue;
		}
		MONO_INST_NEW (cfg, ins, opcode);
		if (opcode == OP_FMOVE || opcode == OP_RMOVE)
			ins->dreg = mono_alloc_freg (cfg);
		else if (opcode == OP_LMOVE)
			ins->dreg = mono_alloc_lreg (cfg);
		else
			ins->dreg = mono_alloc_ireg (cfg);
		ins->sreg1 = in->dreg;
		MONO_ADD_INS (cfg->cbb, ins);
		/* always the ireg list (hreg 0), even for f/l moves — the list is only a liveness hint for
		 * mono_local_deadce/alias analysis here, not a register assignment */
		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, 0, FALSE);
		w->carrier [i] = ins;
		w->capvreg [i] = ins->dreg;
	}
	/* vtype return: capture the hidden-vret ADDRESS. calls.c created call->vret_var as a real
	 * OP_OUTARG_VTRETADDR instruction in cfg->cbb (decompose later rewrites it in place into an
	 * OP_LDADDR of the ret temp, keeping the same MonoInst and dreg), so it is a carrier like any
	 * other. The emitter pushes it as the trailing vret param on OP_VCALL2 sites. */
	if (call->vret_var && mini_type_is_vtype (mini_get_underlying_type (sig->ret))) {
		w->carrier [n] = call->vret_var;
		w->capvreg [n] = call->vret_var->dreg;
		mono_call_inst_add_outarg_reg (cfg, call, call->vret_var->dreg, 0, FALSE);
	}
	call->call_info = (CallInfo *) w;
}

void
mono_arch_emit_epilog (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_epilog");
}

void
mono_arch_emit_exceptions (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_exceptions");
}

/* R346. System.Math/MathF Sqrt, Floor and Ceiling are InternalCalls in mono's corlib (Math.Mono.cs, MathF.Mono.cs),
 * and mono maps them to opcodes only for LLVM (llvm_emit_inst_for_method, reached under COMPILE_LLVM alone), so a
 * wasm-JIT body kept them as CALLS -- and JIT code reaches an icall only through the interpreter (call_interp ->
 * interp_entry -> exec_method -> ves_pinvoke_method): R346 measured Math.Sqrt from MathHelper.sqrt and the Entity
 * body tier 2 inlines it into at ~1.8% of the server tick. Each has an exact IEEE wasm opcode, and the emitter
 * already lowers and types all six (f64/f32 sqrt, floor, ceil). The transcendentals have no wasm opcode and stay calls.
 * COMPILE_WASM only: the LLVM AOT reaches this hook after llvm_emit_inst_for_method, and must not change. */
MonoInst*
mono_arch_emit_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	extern int mono_wasm_jit_math_intrins;
	MonoClass *k = cmethod->klass;
	const char *cn;
	gboolean r8, r4;
	int opcode = 0;
	MonoInst *ins;

	if (!COMPILE_WASM (cfg) || !mono_wasm_jit_math_intrins)
		return NULL;
	if (fsig->param_count != 1 || fsig->hasthis || m_class_get_image (k) != mono_defaults.corlib ||
	    strcmp (m_class_get_name_space (k), "System"))
		return NULL;
	cn = m_class_get_name (k);
	r8 = !strcmp (cn, "Math") && fsig->params [0]->type == MONO_TYPE_R8 && fsig->ret->type == MONO_TYPE_R8;
	/* MathF only with r4fp (MONO_OPT_FLOAT32, in WASM_JIT_OPT_BASE): otherwise an R4 argument is an f64 vreg. */
	r4 = !strcmp (cn, "MathF") && cfg->r4fp && fsig->params [0]->type == MONO_TYPE_R4 && fsig->ret->type == MONO_TYPE_R4;
	if (!r8 && !r4)
		return NULL;
	if (!strcmp (cmethod->name, "Sqrt"))
		opcode = r8 ? OP_SQRT : OP_SQRTF;
	else if (!strcmp (cmethod->name, "Floor"))
		opcode = r8 ? OP_FLOOR : OP_FLOORF;
	else if (!strcmp (cmethod->name, "Ceiling"))
		opcode = r8 ? OP_CEIL : OP_CEILF;
	if (!opcode)
		return NULL;
	MONO_INST_NEW (cfg, ins, opcode);
	ins->type = r8 ? STACK_R8 : STACK_R4;
	ins->dreg = mono_alloc_dreg (cfg, (MonoStackType) ins->type);
	ins->sreg1 = args [0]->dreg;
	MONO_ADD_INS (cfg->cbb, ins);
	wj_count (WJC_MATH_INTRINS_LOWERED);
	return ins;
}

void
mono_arch_emit_outarg_vt (MonoCompile *cfg, MonoInst *ins, MonoInst *src)
{
	g_error ("mono_arch_emit_outarg_vt");
}

guint8 *
mono_arch_emit_prolog (MonoCompile *cfg)
{
	g_error ("mono_arch_emit_prolog");
}

void
mono_arch_emit_setret (MonoCompile *cfg, MonoMethod *method, MonoInst *val)
{
	MonoType *ret = mini_get_underlying_type (mono_method_signature_internal (method)->ret);

	if (!m_type_is_byref (ret)) {
		if (ret->type == MONO_TYPE_R4) {
			MONO_EMIT_NEW_UNALU (cfg, OP_RMOVE, cfg->ret->dreg, val->dreg);
			return;
		} else if (ret->type == MONO_TYPE_R8) {
			MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, cfg->ret->dreg, val->dreg);
			return;
		} else if (ret->type == MONO_TYPE_I8 || ret->type == MONO_TYPE_U8) {
			MONO_EMIT_NEW_UNALU (cfg, OP_LMOVE, cfg->ret->dreg, val->dreg);
			return;
		}
	}
	MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, cfg->ret->dreg, val->dreg);
}

void
mono_arch_flush_icache (guint8 *code, gint size)
{
}

LLVMCallInfo*
mono_arch_get_llvm_call_info (MonoCompile *cfg, MonoMethodSignature *sig)
{
	int i, n;
	CallInfo *cinfo;
	LLVMCallInfo *linfo;

	cinfo = get_call_info (cfg->mempool, sig);
	n = cinfo->nargs;

	linfo = mono_mempool_alloc0 (cfg->mempool, sizeof (LLVMCallInfo) + (sizeof (LLVMArgInfo) * n));

	if (cinfo->ret.storage == ArgVtypeAsScalar) {
		linfo->ret.storage = LLVMArgWasmVtypeAsScalar;
		linfo->ret.etype = cinfo->ret.etype;
		linfo->ret.esize = mono_class_value_size (mono_class_from_mono_type_internal (cinfo->ret.type), NULL);
	} else if (mini_type_is_vtype (sig->ret)) {
		/* Vtype returned using a hidden argument */
		linfo->ret.storage = LLVMArgVtypeRetAddr;
		// linfo->vret_arg_index = cinfo->vret_arg_index;
	} else {
		if (sig->ret->type != MONO_TYPE_VOID)
			linfo->ret.storage = LLVMArgNormal;
	}

	for (i = 0; i < n; ++i) {
		ArgInfo *ainfo = &cinfo->args[i];

		switch (ainfo->storage) {
		case ArgOnStack:
			linfo->args [i].storage = LLVMArgNormal;
			break;
		case ArgValuetypeAddrOnStack:
			linfo->args [i].storage = LLVMArgVtypeByRef;
			break;
		case ArgGsharedVTOnStack:
			linfo->args [i].storage = LLVMArgGsharedvtVariable;
			break;
		case ArgVtypeAsScalar:
			linfo->args [i].storage = LLVMArgWasmVtypeAsScalar;
			linfo->args [i].type = ainfo->type;
			linfo->args [i].etype = ainfo->etype;
			linfo->args [i].esize = mono_class_value_size (mono_class_from_mono_type_internal (ainfo->type), NULL);
			break;
		case ArgValuetypeAddrInIReg:
			g_error ("this is only valid for sig->ret");
			break;
		}
	}

	return linfo;
}

gboolean
mono_arch_tailcall_supported (MonoCompile *cfg, MonoMethodSignature *caller_sig, MonoMethodSignature *callee_sig, gboolean virtual_)
{
	return FALSE;
}
