/*
 * Runtime WebAssembly JIT backend entry point. Sibling of mono_llvm_emit_method /
 * mono_codegen, selected via COMPILE_WASM(cfg) in mini_method_compile. Consumes
 * pre-regalloc vreg IR and emits a self-contained wasm module for the method.
 *
 * Phase 1 (this cut): straight-line int/long/double leaf methods only. Anything
 * with branches/loops/calls/EH or an unsupported opcode/type bails (cfg->native_code
 * left NULL) so the caller falls back to the interpreter. For bring-up it dumps the
 * emitted module as hex so it can be validated offline.
 */

/* 0 = unknown/unsupported (none of the valtypes are 0) */
static WasmValtype
wasm_valtype_of_type (MonoType *t)
{
	t = mini_get_underlying_type (t);
	if (m_type_is_byref (t))
		return WASM_I32;
	/* MONO_TYPE_GENERICINST covers both closed generic reference types and generic value types.
	 * The switch below intentionally leaves value types to WasmCallInfo's scalar/by-address ABI,
	 * but reference instantiations (MH<object>, IEnumerable<T>, Comparer<T>, ...) are ordinary
	 * wasm32 managed references. Omitting this check caused virtually every hot IKVM __<>MHC caller
	 * to bail "call ret type" when a helper returned an IKVM.Runtime.MH<...> delegate. */
	if (MONO_TYPE_IS_REFERENCE (t))
		return WASM_I32;
	switch (t->type) {
	case MONO_TYPE_VOID:
		return WASM_VOID;
	case MONO_TYPE_BOOLEAN: case MONO_TYPE_CHAR:
	case MONO_TYPE_I1: case MONO_TYPE_U1:
	case MONO_TYPE_I2: case MONO_TYPE_U2:
	case MONO_TYPE_I4: case MONO_TYPE_U4:
	case MONO_TYPE_I: case MONO_TYPE_U:
	case MONO_TYPE_PTR: case MONO_TYPE_FNPTR:
	case MONO_TYPE_OBJECT: case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS: case MONO_TYPE_SZARRAY: case MONO_TYPE_ARRAY:
		return WASM_I32;
	case MONO_TYPE_I8: case MONO_TYPE_U8:
		return WASM_I64;
	case MONO_TYPE_R4:
		return WASM_F32;
	case MONO_TYPE_R8:
		return WASM_F64;
	default:
		return 0;
	}
}

/* result valtype produced into ins->dreg by a supported opcode, or 0 */
static WasmValtype
wasm_valtype_of_opcode (int opcode)
{
	switch (opcode) {
	case OP_ICONST: case OP_MOVE: case OP_AOTCONST:   /* AOTCONST bakes a resolved pointer -> i32 on wasm32 */
	case OP_LDADDR:       /* address of an address-taken local -> i32 (frame base + slot offset) on wasm32 */
	case OP_GET_EX_OBJ:   /* caught exception object -> i32 (ref) on wasm32 */
	case OP_IADD: case OP_ISUB: case OP_IMUL:
	case OP_IDIV: case OP_IDIV_UN: case OP_IREM: case OP_IREM_UN:
	case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN:
	case OP_IADD_IMM: case OP_ISUB_IMM: case OP_IMUL_IMM:
	case OP_IAND_IMM: case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_IDIV_IMM: case OP_IDIV_UN_IMM: case OP_IREM_IMM: case OP_IREM_UN_IMM:
	case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADU1_MEMBASE: case OP_LOADI1_MEMBASE: case OP_LOADU2_MEMBASE: case OP_LOADI2_MEMBASE:
	case OP_ICONV_TO_U1: case OP_ICONV_TO_I1: case OP_ICONV_TO_U2: case OP_ICONV_TO_I2:
	case OP_AND_IMM:
	/* native-int (size_t) IMM ops — i32 on wasm32, lowered identically to the OP_I*_IMM variants */
	case OP_ADD_IMM: case OP_SUB_IMM: case OP_MUL_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_INEG: case OP_INOT:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:   /* i64 -> i32 (i32.wrap_i64) */
	case OP_LCONV_TO_I: case OP_LCONV_TO_U:     /* i64 -> native int/uint = i32 on wasm32 (wrap) */
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:  /* i64 setcc -> i32 0/1 */
	case OP_MOVE_F_TO_I4:  /* f32 bits -> i32 (reinterpret) */
	/* float/r4 compare-to-i32 (setcc): standalone, produce a 0/1 i32 result */
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
	/* float -> int (saturating trunc; IREG dest per mini-ops.h). _TO_I is native int = i32 on wasm32. */
	case OP_RCONV_TO_I4: case OP_FCONV_TO_I4: case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
	case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
	case OP_RCONV_TO_I1: case OP_FCONV_TO_I1: case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
	case OP_RCONV_TO_I2: case OP_FCONV_TO_I2: case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
	case OP_LZCNT32: case OP_POPCNT32:
		return WASM_I32;
	case OP_ICONV_TO_I8: case OP_ICONV_TO_U8:
	case OP_LOADI8_MEMBASE:
		return WASM_I64;
	case OP_LOADR8_MEMBASE:
		return WASM_F64;
	case OP_LOADR4_MEMBASE:
		return WASM_F32;
	case OP_I8CONST: case OP_LMOVE:
	case OP_LADD: case OP_LSUB: case OP_LMUL:
	case OP_LDIV: case OP_LDIV_UN: case OP_LREM: case OP_LREM_UN:
	case OP_LAND: case OP_LOR: case OP_LXOR:
	case OP_LSHL: case OP_LSHR: case OP_LSHR_UN:
	case OP_LADD_IMM: case OP_LSUB_IMM: case OP_LMUL_IMM:
	case OP_LDIV_IMM: case OP_LDIV_UN_IMM: case OP_LREM_IMM: case OP_LREM_UN_IMM:
	case OP_LAND_IMM: case OP_LOR_IMM: case OP_LXOR_IMM:
	case OP_LSHL_IMM: case OP_LSHR_IMM: case OP_LSHR_UN_IMM:
	case OP_LNOT: case OP_LNEG:
	case OP_MOVE_F_TO_I8:  /* f64 bits -> i64 (reinterpret) */
	case OP_RCONV_TO_I8: case OP_FCONV_TO_I8: case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:  /* float -> i64 */
	case OP_LZCNT64: case OP_POPCNT64:
		return WASM_I64;
	case OP_R8CONST: case OP_FMOVE:
	case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
	case OP_ICONV_TO_R8: case OP_RCONV_TO_R8:
	case OP_LCONV_TO_R8:   /* i64 -> f64 */
	case OP_MOVE_I8_TO_F:  /* i64 bits -> f64 (reinterpret) */
	/* double math intrinsics (the F-suffixed / R-prefixed forms below are the f32 ones) */
	case OP_ABS: case OP_SQRT: case OP_CEIL: case OP_FLOOR: case OP_TRUNC: case OP_ROUND:
	case OP_FMIN: case OP_FMAX: case OP_FCOPYSIGN:
		return WASM_F64;
	case OP_R4CONST: case OP_RMOVE:
	case OP_FCONV_TO_R4:
	case OP_ICONV_TO_R4: case OP_RCONV_TO_R4:
	case OP_LCONV_TO_R4:   /* i64 -> f32 */
	case OP_MOVE_I4_TO_F:  /* i32 bits -> f32 (reinterpret) */
	case OP_RADD: case OP_RSUB: case OP_RMUL: case OP_RDIV: case OP_RNEG:
	/* float32 math intrinsics */
	case OP_ABSF: case OP_SQRTF: case OP_CEILF: case OP_FLOORF: case OP_TRUNCF:
	case OP_RMIN: case OP_RMAX: case OP_RCOPYSIGN:
		return WASM_F32;
	default:
		return 0;
	}
}

static int
wasm_valtype_group (WasmValtype t)
{
	switch (t) {
	case WASM_I32: return 0;
	case WASM_I64: return 1;
	case WASM_F32: return 2;
	case WASM_F64: return 3;
	default: return -1;
	}
}

/* TRUE iff this instruction's result PROVABLY cannot be a managed pointer (so the dreg may stay in a fast
 * wasm local instead of the GC-scanned ref shadow stack). This is an allow-list of value-producing opcodes
 * whose result is, by ECMA-335 semantics, an integer/float scalar: integer arithmetic/logic/shift/compare,
 * width/sign conversions, float-bit reinterprets, integer constants, and the typed-width integer loads (a
 * reference field always lowers to a bare OP_LOAD_MEMBASE, never a typed load). A STACK_OBJ/STACK_MP result
 * is a managed pointer regardless of opcode, so it is rejected up front. Anything NOT proven here is treated
 * as a possible ref (conservative-by-default): over-marking only over-pins on the conservative root, while a
 * missed ref is a silent dangling-pointer bug. OP_MOVE and call results are classified by the caller (they
 * need the source vreg / the call signature, not just the opcode). */
static gboolean
wj_opcode_is_nonref (MonoInst *ins)
{
	if (ins->type == STACK_OBJ || ins->type == STACK_MP)
		return FALSE;
	switch (ins->opcode) {
	/* NOTE: integer add/sub and and-mask (incl. their _IMM forms) are deliberately NOT here:
	 * on wasm32 OP_PADD/OP_PSUB/OP_PAND_IMM alias OP_IADD/OP_ISUB/OP_IAND_IMM, so "add" is how
	 * every interior pointer (ldelema/ldflda/Unsafe.Add) is formed — and the front end doesn't
	 * always type those STACK_MP (EMIT_NEW_BIALU_IMM leaves type 0; Unsafe.* uses STACK_PTR).
	 * The fixpoint loop taint-propagates them instead: add/sub/mask of a proven-scalar source
	 * stays scalar, of a possible-ref source is a possible ref. */
	case OP_ICONST: case OP_I8CONST:
	case OP_IMUL:
	case OP_IDIV: case OP_IDIV_UN: case OP_IREM: case OP_IREM_UN:
	case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN:
	case OP_INEG: case OP_INOT:
	case OP_IMUL_IMM:
	case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_IDIV_IMM: case OP_IDIV_UN_IMM: case OP_IREM_IMM: case OP_IREM_UN_IMM:
	case OP_MUL_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_ICONV_TO_U1: case OP_ICONV_TO_I1: case OP_ICONV_TO_U2: case OP_ICONV_TO_I2:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4:
	case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADU1_MEMBASE: case OP_LOADI1_MEMBASE: case OP_LOADU2_MEMBASE: case OP_LOADI2_MEMBASE:
	case OP_MOVE_F_TO_I4:
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
		return TRUE;
	default:
		return FALSE;
	}
}

/* TRUE unless this instruction PROVABLY cannot trigger a GC while it executes (MONO_WASM_JIT_SLOTLIVE).
 * DEFAULT-TRUE: anything not on the allow-list below is assumed to be able to reach a safepoint —
 * an under-inclusive "pure" list only costs slot-elision coverage, an over-inclusive one is silent
 * corruption. GC points include every call family (direct, residual, f-slot, icall, vcall IC — the
 * callee can allocate and trigger a moving collection), OP_GC_SAFE_POINT (the cooperative STW poll),
 * OP_THROW/OP_RETHROW and OP_COND_EXC_* (raising enters the runtime, which allocates the exception),
 * integer div/rem (inline DivideByZero/Overflow raise), OP_CALL_HANDLER/OP_ENDFINALLY, and atomics.
 * The allow-list is the emitter's inline-lowered pure population: const/move/arith/logic/shift,
 * conversions (a wasm trunc can TRAP, but a trap is not a GC point), compares + branches, the typed
 * MEMBASE loads/stores (wasm linear memory doesn't fault-signal; mono's null checks are separate
 * explicit COND_EXC instructions), OP_LDADDR (pure address arithmetic), OP_SETRET, and the emitter's
 * known no-op set (seq points, GC liveness annotations, START_HANDLER). */
/* corlib exception id for an OP_COND_EXC_* name, as mono_wasm_jit_raise_corlib expects it; -1 if we
 * don't lower that one. Was an inline if-chain at the COND_EXC site; factored out so the shared-throw
 * prescan classifies exactly the same set the emitter does. */
static int
wj_exc_id_for_name (const char *en)
{
	if (!en) return -1;
	if (!strcmp (en, "OverflowException")) return 0;
	if (!strcmp (en, "DivideByZeroException")) return 1;
	if (!strcmp (en, "IndexOutOfRangeException")) return 2;
	if (!strcmp (en, "InvalidCastException")) return 3;
	if (!strcmp (en, "NullReferenceException")) return 4;
	if (!strcmp (en, "ArithmeticException")) return 5;
	if (!strcmp (en, "ArrayTypeMismatchException")) return 6;
	return -1;
}

#define WJ_EXC_IDS 7

/* The instructions MONO_WASM_JIT_RAISE_NOGC exempts: everything whose only way to reach the collector is
 * by raising, i.e. by allocating an exception and never returning to this frame. Factored out of
 * wj_ins_is_gcpoint so the two classes of consumer can disagree about them -- see the level-1 argument
 * there. OP_CHECK_THIS and OP_[L]DIV/REM are deliberately absent: those are handled elsewhere or raise
 * through paths this set does not describe. */
static gboolean
wj_ins_is_raise (MonoInst *ins)
{
	switch (ins->opcode) {
	case OP_THROW: case OP_RETHROW:
	case OP_COND_EXC_EQ: case OP_COND_EXC_NE_UN: case OP_COND_EXC_LT: case OP_COND_EXC_LT_UN:
	case OP_COND_EXC_GT: case OP_COND_EXC_GT_UN: case OP_COND_EXC_LE: case OP_COND_EXC_LE_UN:
	case OP_COND_EXC_GE: case OP_COND_EXC_GE_UN:
	case OP_COND_EXC_IEQ: case OP_COND_EXC_INE_UN: case OP_COND_EXC_ILT: case OP_COND_EXC_ILT_UN:
	case OP_COND_EXC_IGT: case OP_COND_EXC_IGT_UN: case OP_COND_EXC_ILE: case OP_COND_EXC_ILE_UN:
	case OP_COND_EXC_IGE: case OP_COND_EXC_IGE_UN:
	case OP_COND_EXC_OV: case OP_COND_EXC_NO: case OP_COND_EXC_C: case OP_COND_EXC_NC:
	case OP_COND_EXC_IOV: case OP_COND_EXC_INO: case OP_COND_EXC_IC: case OP_COND_EXC_INC:
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
wj_ins_is_gcpoint (MonoInst *ins, gboolean raise_exempt)
{
	/* MONO_WASM_JIT_RAISE_NOGC. A raise is not a GC point in the sense SLOTLIVE cares about.
	 *
	 * SLOTLIVE's question is "can the collector observe this vreg HERE", which only matters for values
	 * that are still needed afterwards. A raising instruction has exactly two outcomes: it falls through
	 * without allocating anything (OP_COND_EXC_* on the common path), or it raises and NEVER RETURNS to
	 * this frame. Nothing in this method reads a vreg after the raise, so nothing has to survive it.
	 *
	 * The catch is a handler in THIS method: with a clause, the catch landing pad resumes into a handler
	 * bb that does read those vregs, so the raise really is a point the collector must see them at.
	 * Hence the clause_free gate -- keyed off cfg->header->num_clauses rather than the emitter's eh_on,
	 * because num_clauses is the property the argument actually rests on.
	 *
	 * Objects reachable only from this frame's unscanned wasm locals can therefore be collected while
	 * the exception is being allocated. That is fine: the frame is being torn down and will never
	 * dereference them again. Anything still reachable from a scanned root (a field, an array, an outer
	 * frame) is unaffected.
	 *
	 * This is the one change in this series where a mistake is silent heap corruption rather than a
	 * crash, so it is default OFF until soaked with REFVERIFY=2 + OBJGUARD=1 + STOREGUARD=1. */
	/* MEASURED UNSOUND: this exemption loses references. Keep MONO_WASM_JIT_RAISE_NOGC=0 (the default).
	 *
	 * With the nursery shrunk to 4m so that collections are frequent enough to expose it, booting
	 * ikvmcraft / MCSR Ranked 1.16.1 gives:
	 *     default (RAISE_NOGC=1)      4/4 boots dead, "unexpected GC filler class" in ~25s
	 *     MONO_WASM_JIT_RAISE_NOGC=0  0/4
	 *     MONO_WASM_JIT_SLOTLIVE=0    0/4      (RAISE_NOGC has no effect except through SLOTLIVE)
	 *     MONO_WASM_JIT_AUTO=0        0/4
	 *     MONO_WASM_JIT_SLOTZERO=0    3/4      negative control: a different elision knob does NOT help
	 * MONO_CLASS_GC_FILLER is what sgen refills reclaimed nursery memory with (class-init.c:4227), so
	 * the receiver really had been collected out from under a live reference.
	 *
	 * The blame is the OP_COND_EXC_* arm, not OP_THROW. Dropping only OP_THROW/OP_RETHROW from this
	 * switch was built and measured and did NOT help (3/4 still dead), which stands to reason: explicit
	 * throws are rare, whereas the implicit null/bounds/overflow checks are everywhere, so they are what
	 * actually drives effective_gcp_count down -- and that count gates lazy ref-frame materialization
	 * (`effective_gcp_count == 1` => lazy_ref_frame) as well as method_nogc. A raise on the taken path
	 * allocates the exception, which can collect before the lazily materialized frame exists, and then
	 * nothing in that frame is scannable.
	 *
	 * Why this workload and not the jbox2d kernel the series was tuned against: it throws constantly.
	 * IKVM's class-loading probes alone raise ~8.8k NPEs per boot inside
	 * sun.misc.URLClassPath.Loader.findResource, each caught by that method's own catch(Exception).
	 *
	 * THE FIX (level 1, plus the taken-arm materialization below). Two independent holes were closed:
	 *
	 *   a. The "never returns to this frame" argument is about THIS FRAME. It says nothing about
	 *      method_nogc / effective_gcp_count / prior_gcps_are_polls / terminal_vcall_handoff, which all
	 *      publish "no collection happens across here" to OTHER code -- the caller that elides a root
	 *      across a direct call, the handoff that gives its roots to a callee. A raise allocates, so a
	 *      raising instruction is a collection point for every one of those consumers. Level 1 keeps the
	 *      exemption only for gc_gen/needs_slot, the one consumer whose question really is frame-local.
	 *   b. Even frame-locally, the exemption drives effective_gcp_count to 1 and so turns on
	 *      lazy_ref_frame; a raise reached BEFORE the one real GC point then allocates with no frame
	 *      materialized, and the slots that do exist are nowhere the collector can see. Every raise site
	 *      now emits ENSURE_REF_FRAME inside its taken arm (WJ_THROW_BR's shared blocks and LDIV_RAISE),
	 *      exactly as the conditional safepoint poll already does. The hot not-taken path is unchanged.
	 *
	 * Level 2 -- the original all-consumers behaviour -- is DELETED. It was measured unsound and the only
	 * reason it survived was to A/B against level 1.
	 *
	 * BOTH LEVELS THEN MEASURED STILL BROKEN (4/4 dead each, 4 MB nursery, against a positive control
	 * that reproduces on demand). So neither hole above was the cause, and the frame-local exemption --
	 * not bumping gc_gen at a raise -- is the unsound part on its own.
	 *
	 * MECHANISM (measured): the exemption removes an accidental pin.
	 * WJ_SL_USE lets a forwarding virtual call's argument skip the used-at-a-GC-point clause (`_fwd`,
	 * wj_ins_is_pinned_vcall_forward) because the callee is supposed to take ownership of the root.
	 * Normally the span clause still fires -- a raise counts as a GC point, bumps gc_gen, and the
	 * argument gets a slot anyway. Exempt the raise and BOTH clauses go quiet: the argument has no slot
	 * anywhere, and the handoff guarantee is all that is left. It does not hold: every failure lands in
	 * mini_llvmonly_init_vtable_slot -> resolve_vcall, the PIC MISS path, at the resolve step, on a
	 * receiver that is already a nursery filler -- before the miss frame has pinned anything.
	 * Disabling the classification (VCALL_INLINE_IC=0 or VCALL_SHARED_MISS=0) makes level 2 clean.
	 * Fixed in WJ_SL_USE by honouring `_fwd` only while no raise has been exempted in the current
	 * generation; see gen_skipped_raises.
	 *
	 * *** AND THE ACTUAL ANSWER IS SIMPLER THAN ANY OF THIS: THE MECHANISM IS ALREADY DISABLED. ***
	 *
	 * Every failure above rides on ONE thing -- wj_ins_is_pinned_vcall_forward, which lets a forwarding
	 * virtual call's argument skip the used-at-a-GC-point clause in WJ_SL_USE. That predicate was gated
	 * on `mono_wasm_jit_vcall_shared_miss_enabled`, and VCALL_SHARED_MISS SHIPS 0, so the predicate has
	 * always evaluated FALSE in the product. The measurement for exactly that configuration is recorded
	 * a few thousand lines below, at the terminal-vcall handoff: "disabling wj_ins_is_pinned_vcall_
	 * forward outright (SHARED_MISS=0) is clean 0/4 against a 4/4 control".
	 *
	 * So raise_nogc=1 is safe in the shipped configuration, it was MEASURED safe, and the product has
	 * been relying on a side effect of an unrelated PERFORMANCE decision to get there -- VCALL_SHARED_MISS
	 * ships 0 because turning it off measured -13.2%% p50 / +19.9%% fps. The two facts sat ~3,800 lines
	 * apart in this file and were never joined, which is the only reason this ever read as "unverified".
	 * The predicate now returns FALSE unconditionally (except under the level-2 control), so the
	 * protection is deliberate rather than accidental and does not depend on a performance knob.
	 *
	 * *** AND IT IS NOW MEASURED, NOT ONLY REASONED. *** scratchpad/mcsr/gcstress.sh, n=4 per arm,
	 * nursery-size=4m, one session, one build, one harness:
	 *
	 *     pos   RAISE_NOGC=2 (predicate re-armed, guard off)   3/4 FAILED
	 *     lvl1  RAISE_NOGC=1 (THE SHIPPED SETTING)             0/4 failed, 4/4 rendered
	 *     neg   RAISE_NOGC=0 (exemption off entirely)          0/4 failed, 4/4 rendered
	 *
	 * The positive control reproduces at 3/4 in the SAME session as the clean arms, which is the whole
	 * point of it: without that, "it stopped failing" is unfalsifiable on an intermittent bug. The
	 * shipped setting is indistinguishable from having the exemption off at all. Note the failure now
	 * surfaces as `Assertion at interp.c:10452, condition code_type == IMETHOD_CODE_*` rather than the
	 * historical "unexpected GC filler class" -- a different assertion downstream of the same
	 * corruption, tripping first. Do not treat the old string as the only signature.
	 *
	 * The paragraphs below are kept because their MECHANISM is still the right description of the
	 * hazard, and because level 2 exists to reproduce it on demand.
	 *
	 * Read the paragraphs above in ORDER, because their verdicts contradict each other and the last one
	 * wins: measured broken -> mechanism identified -> mechanism fixed -> NOT RE-MEASURED. Anyone
	 * skimming this block sees "BOTH LEVELS MEASURED STILL BROKEN" and concludes the knob is known-bad;
	 * anyone reading index.ts sees it shipped and concludes it is known-good. Neither is true, and this
	 * is the one failure class in the emitter where being wrong is silent heap corruption rather than a
	 * crash -- which is also why it is invisible in normal runs: the product ships nursery-size=128m, so
	 * collections are far too rare to reproduce what a 4 MB nursery exposed on demand.
	 *
	 * THE OUTSTANDING MEASUREMENT, and it is cheap: boot ikvmcraft with MONO_GC_PARAMS nursery-size=4m,
	 * 4 runs at raise_nogc=1 against 4 at raise_nogc=0, and look for "unexpected GC filler class"
	 * (MONO_CLASS_GC_FILLER is what sgen refills reclaimed nursery memory with, class-init.c:4227, so a
	 * receiver that reads as one really was collected from under a live reference). The positive control
	 * reproduces on demand, which is what makes this answerable in one sitting. Until it is run, treat
	 * shipping 1 as UNVERIFIED rather than as a decision.
	 *
	 * The issue is a shared rule rather than a single bad method; see scratchpad/mcsr/FINDINGS.md
	 * Finding 18 for the measurement record. */
	if (raise_exempt && wj_ins_is_raise (ins))
		return FALSE;

	switch (ins->opcode) {
	/* constants + moves (incl. STACK_OBJ ICONST: a pure literal-table i32.load) */
	case OP_ICONST: case OP_I8CONST: case OP_R4CONST: case OP_R8CONST:
	case OP_MOVE: case OP_LMOVE: case OP_FMOVE: case OP_RMOVE:
	case OP_MOVE_F_TO_I4: case OP_MOVE_I4_TO_F: case OP_MOVE_F_TO_I8: case OP_MOVE_I8_TO_F:
	/* integer arithmetic/logic/shift — div/rem deliberately ABSENT (they raise) */
	case OP_IADD: case OP_ISUB: case OP_IMUL: case OP_IAND: case OP_IOR: case OP_IXOR:
	case OP_ISHL: case OP_ISHR: case OP_ISHR_UN: case OP_INEG: case OP_INOT:
	case OP_IADD_IMM: case OP_ISUB_IMM: case OP_IMUL_IMM: case OP_IAND_IMM: case OP_IOR_IMM: case OP_IXOR_IMM:
	case OP_ISHL_IMM: case OP_ISHR_IMM: case OP_ISHR_UN_IMM:
	case OP_ADD_IMM: case OP_SUB_IMM: case OP_MUL_IMM: case OP_AND_IMM: case OP_OR_IMM: case OP_XOR_IMM:
	case OP_SHL_IMM: case OP_SHR_IMM: case OP_SHR_UN_IMM:
	case OP_LADD: case OP_LSUB: case OP_LMUL: case OP_LAND: case OP_LOR: case OP_LXOR:
	case OP_LSHL: case OP_LSHR: case OP_LSHR_UN: case OP_LNEG: case OP_LNOT:
	case OP_LADD_IMM: case OP_LSUB_IMM: case OP_LMUL_IMM: case OP_LAND_IMM: case OP_LOR_IMM: case OP_LXOR_IMM:
	case OP_LSHL_IMM: case OP_LSHR_IMM: case OP_LSHR_UN_IMM:
	/* float arithmetic (float div doesn't raise) */
	case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
	case OP_RADD: case OP_RSUB: case OP_RMUL: case OP_RDIV: case OP_RNEG:
	/* conversions */
	case OP_ICONV_TO_I1: case OP_ICONV_TO_U1: case OP_ICONV_TO_I2: case OP_ICONV_TO_U2:
	case OP_ICONV_TO_R4: case OP_ICONV_TO_R8:
	case OP_LCONV_TO_I4: case OP_LCONV_TO_U4: case OP_LCONV_TO_R4: case OP_LCONV_TO_R8:
	case OP_FCONV_TO_R4: case OP_RCONV_TO_R4: case OP_RCONV_TO_R8:
	/* float -> int (saturating trunc): a number, never a managed pointer */
	case OP_RCONV_TO_I4: case OP_FCONV_TO_I4: case OP_RCONV_TO_U4: case OP_FCONV_TO_U4:
	case OP_RCONV_TO_I8: case OP_FCONV_TO_I8: case OP_RCONV_TO_U8: case OP_FCONV_TO_U8:
	case OP_RCONV_TO_I1: case OP_FCONV_TO_I1: case OP_RCONV_TO_U1: case OP_FCONV_TO_U1:
	case OP_RCONV_TO_I2: case OP_FCONV_TO_I2: case OP_RCONV_TO_U2: case OP_FCONV_TO_U2:
	case OP_RCONV_TO_I:  case OP_FCONV_TO_I:
	/* float math + bit intrinsics: likewise pure numbers */
	case OP_ABS: case OP_SQRT: case OP_CEIL: case OP_FLOOR: case OP_TRUNC: case OP_ROUND:
	case OP_ABSF: case OP_SQRTF: case OP_CEILF: case OP_FLOORF: case OP_TRUNCF:
	case OP_FMIN: case OP_FMAX: case OP_RMIN: case OP_RMAX: case OP_FCOPYSIGN: case OP_RCOPYSIGN:
	case OP_LZCNT32: case OP_LZCNT64: case OP_POPCNT32: case OP_POPCNT64:
	case OP_SEXT_I4: case OP_ZEXT_I4:
	/* compares + setcc + branches */
	case OP_ICEQ: case OP_ICNEQ: case OP_ICLT: case OP_ICLT_UN: case OP_ICGT: case OP_ICGT_UN:
	case OP_ICLE: case OP_ICLE_UN: case OP_ICGE: case OP_ICGE_UN:
	case OP_LCEQ: case OP_LCGT: case OP_LCGT_UN: case OP_LCLT: case OP_LCLT_UN:
	case OP_FCEQ: case OP_FCNEQ: case OP_FCLT: case OP_FCLT_UN: case OP_FCGT: case OP_FCGT_UN: case OP_FCLE: case OP_FCGE:
	case OP_RCEQ: case OP_RCNEQ: case OP_RCLT: case OP_RCLT_UN: case OP_RCGT: case OP_RCGT_UN: case OP_RCLE: case OP_RCGE:
	case OP_COMPARE: case OP_ICOMPARE: case OP_LCOMPARE: case OP_FCOMPARE: case OP_RCOMPARE:
	case OP_COMPARE_IMM: case OP_ICOMPARE_IMM: case OP_LCOMPARE_IMM:
	case OP_BR:
	case OP_IBEQ: case OP_IBNE_UN: case OP_IBLT: case OP_IBLT_UN: case OP_IBGT: case OP_IBGT_UN:
	case OP_IBLE: case OP_IBLE_UN: case OP_IBGE: case OP_IBGE_UN:
	case OP_LBEQ: case OP_LBNE_UN: case OP_LBLT: case OP_LBLT_UN: case OP_LBGT: case OP_LBGT_UN:
	case OP_LBLE: case OP_LBLE_UN: case OP_LBGE: case OP_LBGE_UN:
	case OP_FBEQ: case OP_FBNE_UN: case OP_FBLT: case OP_FBLT_UN: case OP_FBGT: case OP_FBGT_UN:
	case OP_FBLE: case OP_FBLE_UN: case OP_FBGE: case OP_FBGE_UN:
	case OP_RBEQ: case OP_RBNE_UN: case OP_RBLT: case OP_RBLT_UN: case OP_RBGT: case OP_RBGT_UN:
	case OP_RBLE: case OP_RBLE_UN: case OP_RBGE: case OP_RBGE_UN:
	/* typed memory accesses (null checks are separate explicit COND_EXC instructions) */
	case OP_LOAD_MEMBASE: case OP_LOADI4_MEMBASE: case OP_LOADU4_MEMBASE:
	case OP_LOADI1_MEMBASE: case OP_LOADU1_MEMBASE: case OP_LOADI2_MEMBASE: case OP_LOADU2_MEMBASE:
	case OP_LOADI8_MEMBASE: case OP_LOADR4_MEMBASE: case OP_LOADR8_MEMBASE:
	case OP_STORE_MEMBASE_REG: case OP_STOREI4_MEMBASE_REG: case OP_STOREI1_MEMBASE_REG:
	case OP_STOREI2_MEMBASE_REG: case OP_STOREI8_MEMBASE_REG: case OP_STORER4_MEMBASE_REG: case OP_STORER8_MEMBASE_REG:
	case OP_STORE_MEMBASE_IMM: case OP_STOREI4_MEMBASE_IMM: case OP_STOREI1_MEMBASE_IMM: case OP_STOREI2_MEMBASE_IMM:
	case OP_LDADDR:
	case OP_SETRET:
	/* the emitter's no-op set */
	case OP_NOP: case OP_IL_SEQ_POINT: case OP_SEQ_POINT:
	/* OP_NOT_NULL emits nothing (a fact marker, see its emit case), so it can neither reach a GC nor
	 * write memory. It must be listed: this classifier is an inverted whitelist ending in
	 * `default: return TRUE`, and MONO_EMIT_NULL_CHECK now emits an OP_NOT_NULL at every null check.
	 * Left to the default it would make every null check a GC point, forcing a frame slot for every ref
	 * live across one and undoing most of what MONO_WASM_JIT_SLOTLIVE elides. Note OP_CHECK_THIS is
	 * deliberately NOT here -- it really can raise. */
	case OP_NOT_NULL:
	case OP_DUMMY_USE: case OP_NOT_REACHED: case OP_START_HANDLER:
	case OP_GC_LIVENESS_DEF: case OP_GC_LIVENESS_USE:
	case OP_GC_PARAM_SLOT_LIVENESS_DEF: case OP_GC_SPILL_SLOT_LIVENESS_DEF:
		return FALSE;
	default:
		return TRUE;
	}
}

/* Bump a WJC counter only when MONO_WASM_JIT_STATS is on. The counters are atomics, and these sit in
 * the per-instruction emit path where an unconditional atomic is measurable on compile time. */
static void
wj_count (int idx)
{
	if (G_UNLIKELY (mono_wasm_jit_stats))
		mono_wasm_jit_count (idx);
}

/*
 * MONO_WASM_JIT_LCSE — redundant heap-load elimination, scoped to an extended basic block.
 *
 * Two kinds of fact are tracked:
 *   LOAD  : the value of *(base + off), read with a given wasm load opcode, lives in vreg `vreg`.
 *   ALIAS : `from` holds the same value as `to`, so a lookup keyed on `from` can use `to` instead.
 *
 * ALIAS is what makes CHAINED reloads work, and it is the whole reason this is not just a bb-local
 * peephole. For `a.lowerBound.x` reloaded in a ternary arm, eliding the outer load leaves the arm's
 * `.x` load keyed on the elided load's dreg rather than on the cached vreg; without canonicalising
 * through ALIAS the second load misses and only half the redundancy goes away.
 *
 * Deliberately small and linearly scanned: the win is a handful of hot locations per extended block,
 * not a big table, and fixed arrays cost no allocation per bb.
 */
/*
 * LOADs and ALIASes live in SEPARATE arrays, and both evict the OLDEST entry when full rather than
 * refusing the newest. Sharing one 16-entry table let a dense run of scalar moves (mono's call-arg
 * setup and decompose emit them freely) fill it with alias entries before a load could get in, and a
 * full table then dropped adds silently.
 *
 * MEASURED: that was NOT the reason reach was low. With the split in place, WJC_LCSE_EVICT is 0 on the
 * jbox2d bench -- the table is never full -- and reach is 54 hits / 7830 loads = 0.69%. 4633 loads DO
 * get cached, so the entries are being created and then dying before anything matches them. The two
 * structural reasons, both inherent to the EBB scope rather than to any table size:
 *   - the whole table is cleared at every call (wj_ins_is_gcpoint's inverted whitelist is default-TRUE),
 *     and these hot loops are call-dense;
 *   - a merge block has in_count > 1 and therefore starts empty, which is exactly where the reload in
 *     the motivating `a < b ? a : b` shape sits.
 * Fixing reach means a real GVN over the dominator tree with per-field memory dependence (kill only on
 * stores/calls that can write THAT field), not a bigger table. Until then this pass is correct and
 * nearly inert, and the 39-vs-14 heap-load gap against teavm stands unaddressed.
 */
#define WJ_LCSE_LOADS   16
#define WJ_LCSE_ALIASES 16
typedef struct {
	int wop;       /* the EMITTED wasm opcode + alignment, NOT the mono opcode: OP_LOAD_MEMBASE,     */
	int align;     /* OP_LOADI4_MEMBASE and OP_LOADU4_MEMBASE are three keys for one i32.load, so    */
	               /* keying on the mono opcode makes a ref field and an int field at the same       */
	               /* address miss each other. Width and signedness stay distinct because they are   */
	               /* distinct wasm opcodes.                                                          */
	int base;      /* base vreg (already canonical) */
	gint32 off;    /* byte offset */
	int vreg;      /* the vreg that holds the loaded value */
} WjLcseLoad;
typedef struct {
	int from;      /* reads of this vreg may use ... */
	int to;        /* ... this one instead */
} WjLcseAlias;
typedef struct {
	WjLcseLoad ld [WJ_LCSE_LOADS];
	WjLcseAlias al [WJ_LCSE_ALIASES];
	int nld, nal;
} WjLcse;

static int
wj_lcse_canon (WjLcse *t, int v)
{
	int guard;
	if (!t || v < 0)
		return v;
	/* Chains are built by appending and are short; the bound is a hard stop against a cycle, not an
	 * expected depth. */
	for (guard = 0; guard < WJ_LCSE_ALIASES; ++guard) {
		int i, next = -1;
		for (i = 0; i < t->nal; ++i)
			if (t->al [i].from == v) { next = t->al [i].to; break; }
		if (next < 0 || next == v)
			break;
		v = next;
	}
	return v;
}

/*
 * Drop every fact invalidated by a write to V's storage. Called on every def BEFORE the defining
 * instruction is lowered, so the instruction's own sources still resolve against the old state.
 *
 * Invalidation is by STORAGE, not by vreg number. Under MONO_WASM_JIT_COALESCE several vregs share one
 * wasm local, so defining any of them overwrites the cached value of all of them -- and the coalescing
 * liveness cannot have accounted for that, because LCSE's read of a cached vreg is created during
 * EMISSION, after the IR-derived liveness that chose the sharing has already run. Comparing li[] closes
 * that gap exactly, and cannot over-kill: each valtype group owns a disjoint range of local indices, so
 * two vregs share a local only if they really do share storage.
 */
static void
wj_lcse_kill (WjLcse *t, const int *li, int nvreg, int v)
{
	int i, k, lv;
	if (!t || v < 0)
		return;
	lv = (li && v < nvreg) ? li [v] : -1;
#define WJ_LCSE_DEAD(x) ((x) == v || (lv >= 0 && (x) >= 0 && (x) < nvreg && li [(x)] == lv))
	for (i = 0, k = 0; i < t->nld; ++i)
		if (!WJ_LCSE_DEAD (t->ld [i].base) && !WJ_LCSE_DEAD (t->ld [i].vreg))
			t->ld [k++] = t->ld [i];
	t->nld = k;
	for (i = 0, k = 0; i < t->nal; ++i)
		if (!WJ_LCSE_DEAD (t->al [i].from) && !WJ_LCSE_DEAD (t->al [i].to))
			t->al [k++] = t->al [i];
	t->nal = k;
#undef WJ_LCSE_DEAD
}

static void
wj_lcse_add_load (WjLcse *t, int wop, int align, int base, gint32 off, int vreg)
{
	/* base == vreg would be self-referential: `t = load(t, off)` redefines its own base, and caching
	 * (t,off) -> t would then hand out the loaded value as if it were the address. */
	if (!t || base < 0 || vreg < 0 || base == vreg)
		return;
	if (t->nld >= WJ_LCSE_LOADS) {
		memmove (&t->ld [0], &t->ld [1], sizeof (t->ld [0]) * (WJ_LCSE_LOADS - 1));
		t->nld = WJ_LCSE_LOADS - 1;
	}
	t->ld [t->nld].wop = wop;
	t->ld [t->nld].align = align;
	t->ld [t->nld].base = base;
	t->ld [t->nld].off = off;
	t->ld [t->nld].vreg = vreg;
	t->nld++;
}

static void
wj_lcse_add_alias (WjLcse *t, int from, int to)
{
	if (!t || from < 0 || to < 0 || from == to)
		return;
	if (t->nal >= WJ_LCSE_ALIASES) {
		memmove (&t->al [0], &t->al [1], sizeof (t->al [0]) * (WJ_LCSE_ALIASES - 1));
		t->nal = WJ_LCSE_ALIASES - 1;
	}
	t->al [t->nal].from = from;
	t->al [t->nal].to = to;
	t->nal++;
}

static int
wj_lcse_find (WjLcse *t, int wop, int align, int base, gint32 off)
{
	int i;
	if (!t)
		return -1;
	for (i = 0; i < t->nld; ++i)
		if (t->ld [i].wop == wop && t->ld [i].align == align && t->ld [i].base == base && t->ld [i].off == off)
			return t->ld [i].vreg;
	return -1;
}

/* Could this instruction change memory that a cached load already read? */
static gboolean
wj_ins_clobbers_mem (MonoInst *ins)
{
	/* Stores write it; OP_LDADDR hands its address to code we cannot see. */
	if (MONO_IS_STORE_MEMBASE (ins) || ins->opcode == OP_LDADDR)
		return TRUE;
	/* A raise never returns, so nothing later in this method observes anything it did. Decided here
	 * rather than by asking wj_ins_is_gcpoint with clause_free=TRUE, because that answer is gated on
	 * MONO_WASM_JIT_RAISE_NOGC — which is about GC visibility, not memory effects — and LCSE's
	 * effectiveness should not depend on an unrelated flag. Null checks are the commonest instruction
	 * between a load and its reload, so getting this wrong costs the entire optimisation. */
	if (MONO_IS_COND_EXC (ins) || ins->opcode == OP_THROW || ins->opcode == OP_RETHROW)
		return FALSE;
	/* Otherwise reuse the GC-point classifier's inverted whitelist: its FALSE set is exactly the
	 * constants/moves/arithmetic/conversion/compare/branch/load family. Anything it does not recognise —
	 * every call included — clears the table, i.e. an unknown opcode costs optimisation, never
	 * correctness. */
	return wj_ins_is_gcpoint (ins, FALSE);
}

/* Upper bound on distinct direct-call helper imports per module. The emitter has ~25 distinct helper
 * symbols; 32 leaves headroom without making the per-compile context meaningfully bigger. */
/* Was 32, sized for the emitter's ~25 distinct runtime helpers. AOT_IMPORTS makes inline-AOT CALLEES intern
 * into the same array, and a callee set is per-method and unbounded, so 32 would have let a call-heavy method
 * spend every slot on callees and push the real helpers back onto call_indirect (interning is first-use order,
 * so it is the LATE user that loses). Raised, but deliberately not huge: each declared import costs ~10 bytes
 * of module and the median body is ~500 B, so an unbounded cap trades a per-call win for module bloat across
 * ~29k modules. Overflow still degrades to call_indirect rather than failing the compile. */
/* Sized for the batch case, where ALL members share one array: a batch of 8 members each interning ~10
 * distinct (table_index, type_idx) pairs needs far more than one method does on its own. Note the pairs do not
 * collapse across members even for the same helper, because a member's type_idx is relative to its own type
 * block -- correct, but duplicative. Overflow still degrades to call_indirect rather than failing the compile. */
/* Storage cap. The EFFECTIVE cap is the runtime knob below, so the two can be A/B'd in one binary — which
 * matters because this number turned out to be load-bearing rather than a formality: Round 99 (AOT_IMPORTS)
 * started interning inline-AOT CALLEES into the same array, so the old 64 became a binding limit and any method
 * past it silently reverted every further direct call to `i32.const <idx>; call_indirect`. */
#define WJ_MAX_HELPER_IMPORTS 512

/* Per-compile vreg access context: non-reference vregs live in wasm locals (li[]); reference vregs
 * (refslot[vreg] >= 0) live in the GC-scanned ref shadow stack at refbase + slot*4. refbase/rtmp are
 * wasm i32 locals (the frame base address + a scratch for ref stores). refslot is NULL when the ref
 * shadow stack is disabled (offline dump), so refs fall back to locals. */
typedef struct {
	int *li;
	int nvreg;
	int *refslot;
	/* MONO_WASM_JIT_REF_WT: per-vreg write-through flag. When set, the wasm local li[vreg] is the
	 * value HOME (reads are a plain local.get) and the frame slot is only a def-mirrored pin copy
	 * (every wasm_st also stores to the slot). NULL / unset = slot-homed (pre-WT behaviour, and the
	 * addrslot==-2 sentinels whose slot address escapes to callees). */
	guint8 *ref_wt;
	int refbase;   /* wasm local: ref-frame base address */
	int rtmp;      /* wasm local: scratch i32 for ref stores */
	int lazy_frame;       /* 0 eager; 1 absent until the first effective returning GC point; 2 cold-only:
	                       * materialised solely inside raise/poll taken arms (MONO_WASM_JIT_LAZY_COLD) */
	int frame_active;     /* wasm i32 local: lazy frame has been materialized */
	/* OP_LDADDR support: address-taken SCALAR locals live in the per-thread addressable-locals frame
	 * (linear memory) instead of a wasm local, so their address can be passed to callees. addrslot[vreg]
	 * is the byte offset into that frame (>=0 if the vreg is such a local, else -1); addrbase is the wasm
	 * i32 local holding the frame base; vt[] gives each vreg's wasm valtype (for the load/store width);
	 * addr_tmp[g] is a per-type-group scratch local used to reorder [value]->[addr,value] for the store. */
	int *addrslot;
	/* For an address-taken SUB-WORD local, the declared managed width+signedness so wasm_addr_ld reads it
	 * back correctly after a NARROW callee write (1=load8_s, 2=load8_u, 3=load16_s, 4=load16_u; 0=full). A
	 * callee given &local writes only the managed type's size; the slot's upper bytes keep their stale value
	 * (addr_enter zero, or the JIT's last full store), so a plain i32.load would mis-read a signed sub-word
	 * (e.g. a -1 short reads as 0x0000FFFF). A width-correct narrow load reads only the live low bytes. */
	signed char *addr_ldkind;
	int addrbase;
	WasmValtype *vt;
	int addr_tmp [4];
	/* MONO_WASM_JIT_STOREGUARD debug assert: when storeguard != 0, every ref-shadow-stack / addr-frame
	 * store is preceded by a call_indirect to mono_wasm_jit_check_store(addr, kind) (functype index
	 * check_ti = (i32,i32)->void) that traps if the target is outside this thread's arena. */
	int storeguard;
	int objguard;     /* MONO_WASM_JIT_OBJGUARD: validate the object base of ref-field stores (check_store kind=2) */
	int check_ti;
	/* The helper-import array used to live here, filled during emission. It does not any more: a call to a
	 * fixed table index leaves a relocation and the assembler assigns the slots (wj_asm_intern_import),
	 * because a member cannot know its own module's final import count while it is still being written. */
} WjCtx;

/* --- the four call forms the emitter can leave behind --------------------------------------------
 *
 * None of these writes a call. Each records a zero-length HOLE (see wasm-encoder.h) saying what is being
 * called and with what signature; the form -- `call_indirect` through the table, `call` to an import, or
 * `call` to a module-local function -- is chosen when the member is framed, and re-chosen every time it is
 * re-framed. That is what makes retargeting free, and it is why nothing here consults a knob.
 *
 * `type_idx` is an index into THIS member's type block (0 = the method's own signature, 1 = its entry
 * thunk's, 2.. = the callee types interned during emission), not a module type index. */

/* A runtime helper, or any other target at a fixed function-table index: a C function in
 * dotnet.native.wasm, or an AOT method body. Under wasm a C function pointer IS an indirect-function-table
 * index, which is why an address is all this needs. Args are already on the stack. */
static void
wj_emit_helper_call (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_HELPER, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* Another wasm-JITted method, named by identity as well as by f-slot: the identity is what lets the
 * assembler notice the callee is co-located (making this a one-instruction `call <funcidx>` that V8 can
 * inline through) and what it checks the callee's registered ABI against before daring to import it. */
static void
wj_emit_method_call (WasmBuf *b, MonoMethod *callee, int fslot, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_CALL, type_idx, (guint32) fslot, callee);
}

/* A call whose target index is computed at run time -- a virtual dispatch, an IC hit, a calli. This one
 * can never become a direct call; only its functype is relocated. */
static void
wj_emit_dynamic_call (WasmBuf *b, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_INDIRECT, type_idx, 0, NULL);
}

/* A fixed table index that the helper-import conversion never covered.
 *
 * Mechanically identical to a helper -- a C function in dotnet.native.wasm, at a table index fixed for the
 * process -- and split out for one reason: Round 121 converted a NAMED SET of helper sites to imports after
 * measuring them (leave_island, threads_state_poll, raise_corlib, check_store), and these two were not in
 * it. They are the runtime JIT-icall and the residual throw continuation, and they have always been
 * `i32.const <fptr>; call_indirect`.
 *
 * Folding them in with the helpers converted them silently, which the tier-shape gate caught: 14,583 of
 * 22,547 common methods changed structure in a refactor that was supposed to change none. Converting them
 * is very likely a win -- an import is ~5 x86 against ~15, and R129 measured index 403815 alone at 32.3% of
 * all constant-index indirect traffic -- but it is a change with an outcome, so it waits behind its own
 * knob and its own measurement rather than riding along inside a refactor. */
static void
wj_emit_helper_call_ci (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_HELPER_CI, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* An AOT method body. Identical mechanically to a helper -- a fixed function-table index with a known
 * functype -- and separate only so MONO_WASM_JIT_AOT_IMPORTS and MONO_WASM_JIT_HELPER_IMPORTS remain two
 * knobs at assembly time rather than one. */
static void
wj_emit_aot_call (WasmBuf *b, gpointer addr, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_AOT, type_idx, (guint32) (intptr_t) addr, NULL);
}

/* INLINE f-SLOT LIVENESS PROBE. Pushes i32 1 if THIS worker installed a function at `fslot_idx`, else 0.
 *
 * PER-WORKER INSTALLATION. The recipe's f-slot number is process-wide; whether this worker put a function
 * there is not, and `table[fslot] != null` is NOT the test -- the jiterpreter PREFILLS the range with a
 * callable placeholder whose (i32,i32,i32,i32)->void body writes 999 through its fourth argument. This
 * probes the per-thread bitmap instead; it is monotone, so no seqlock and no atomics.
 *
 * Extracted because it was emitted IDENTICALLY at two sites (the delegate devirt arm and the delegate PIC
 * arm) and because it is one of the highest-frequency emitted sequences in the tier (repeats.py, R240
 * addendum 3). Naming it is the precondition for ever emitting it differently -- per module, or not at all
 * once a callee is known co-located. */
static void
wj_emit_slot_live_probe (WasmBuf *b, int fslot_idx, int cap_idx, int ptr_idx)
{
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) fslot_idx);
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) cap_idx);
	wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, 0);
	wasm_op (b, WASM_OP_I32_LT_U);
	wasm_op (b, WASM_OP_IF); wasm_u8 (b, (guint8) WASM_I32);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) ptr_idx);
		wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, 0);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) fslot_idx);
		wasm_i32_const (b, 3); wasm_op (b, WASM_OP_I32_SHR_U);
		wasm_op (b, WASM_OP_I32_ADD);
		wasm_op (b, WASM_OP_I32_LOAD8_U); wasm_memarg (b, 0, 0);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) fslot_idx);
		wasm_i32_const (b, 7); wasm_op (b, WASM_OP_I32_AND);
		wasm_op (b, WASM_OP_I32_SHR_U);
		wasm_i32_const (b, 1); wasm_op (b, WASM_OP_I32_AND);
	wasm_op (b, WASM_OP_ELSE);
		wasm_i32_const (b, 0);
	wasm_op (b, WASM_OP_END);
}

/* A multi-value block type, which is a type index too and moves for exactly the same reason. */
static void
wj_emit_blocktype (WasmBuf *b, guint32 type_idx)
{
	wasm_reloc (b, WASM_RELOC_TYPE_S, type_idx, 0, NULL);
}

/* A scalar register-to-register copy. These carry no type information of their own — see the
 * valtype unification pass in mono_wasm_emit_method. OP_VMOVE/OP_XMOVE are NOT moves for this
 * purpose: they copy value types, which have no wasm valtype at all. */
static gboolean
wj_ins_is_move (int opcode)
{
	return opcode == OP_MOVE || opcode == OP_LMOVE || opcode == OP_FMOVE || opcode == OP_RMOVE;
}

/* Emit a memory load/store of an addressable local (addrslot[vreg] >= 0), at addrbase + offset, using the
 * vreg's wasm valtype for the width. Returns FALSE on an unsupported width. */
static gboolean
wasm_addr_ld (WasmBuf *b, WjCtx *c, int vreg)
{
	guint32 off = (guint32) c->addrslot [vreg];
#ifdef HOST_BROWSER
	/* STOREGUARD, LOAD SIDE -- the addr-frame counterpart of the kind-0 guard added to wasm_ld's refslot
	 * branch. kind 1 was emitted only by wasm_addr_st, so an addr-frame READ through a bad addrbase was
	 * unchecked. Direct emission, not wasm_guard_memaddr: that helper calls wasm_ld. */
	if (G_UNLIKELY (c->storeguard && c->check_ti >= 0)) {
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (b, (gint32) off);
		wasm_op (b, WASM_OP_I32_ADD);
		wasm_i32_const (b, 1);
		wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
	}
#endif
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	if (c->vt [vreg] == WASM_I32 && c->addr_ldkind && c->addr_ldkind [vreg]) {
		/* sub-word local: read back only the live low byte(s) with the declared signedness, so a narrow
		 * callee write (which left the upper bytes stale) is interpreted correctly. */
		switch (c->addr_ldkind [vreg]) {
		case 1: wasm_op (b, WASM_OP_I32_LOAD8_S);  wasm_memarg (b, 0, off); return TRUE;
		case 2: wasm_op (b, WASM_OP_I32_LOAD8_U);  wasm_memarg (b, 0, off); return TRUE;
		case 3: wasm_op (b, WASM_OP_I32_LOAD16_S); wasm_memarg (b, 1, off); return TRUE;
		case 4: wasm_op (b, WASM_OP_I32_LOAD16_U); wasm_memarg (b, 1, off); return TRUE;
		default: break;
		}
	}
	switch (c->vt [vreg]) {
	case WASM_I32: wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, off); return TRUE;
	case WASM_I64: wasm_op (b, WASM_OP_I64_LOAD); wasm_memarg (b, 3, off); return TRUE;
	case WASM_F32: wasm_op (b, WASM_OP_F32_LOAD); wasm_memarg (b, 2, off); return TRUE;
	case WASM_F64: wasm_op (b, WASM_OP_F64_LOAD); wasm_memarg (b, 3, off); return TRUE;
	default: return FALSE;
	}
}

static gboolean
wasm_addr_st (WasmBuf *b, WjCtx *c, int vreg)
{
	guint32 off = (guint32) c->addrslot [vreg];
	int g = wasm_valtype_group (c->vt [vreg]);
	WasmOpcode sop; int al;
	if (g < 0) return FALSE;
	/* value is on the wasm stack; i32.store wants [addr, value], so stash value -> per-type scratch,
	 * push addr (addrbase), re-push value, then store. */
	switch (c->vt [vreg]) {
	case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
	case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
	case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
	case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
	default: return FALSE;
	}
	wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->addr_tmp [g]);
#ifdef HOST_BROWSER
	if (G_UNLIKELY (c->storeguard)) {
		/* check_store(addrbase + off, 1) before the store (STOREGUARD; arenas+helper are HOST_BROWSER) */
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (b, (gint32) off);
		wasm_op (b, WASM_OP_I32_ADD);
		wasm_i32_const (b, 1);
		wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
	}
#endif
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->addr_tmp [g]);
	wasm_op (b, sop); wasm_memarg (b, (guint32) al, off);
	return TRUE;
}

/* Normalize a sub-word integer return value (already on the wasm stack) to the full i32 the IR consumer
 * expects. A raw llvmonly AOT body returns bool/i1/u1/i2/u2 with UNDEFINED upper bits (LLVM legalizes `ret
 * i8/i16` with no zeroext/signext), so a JITted caller that consumes the dreg in an i32 compare/branch/
 * index would read garbage. Mirrors the residual interp_entry normalization. */
static void
wasm_emit_subword_ret_norm (WasmBuf *b, MonoType *ret)
{
	/* A BYREF return is a POINTER and must NOT be masked/extended as its pointee type: `ref sbyte` would
	 * get i32.extend8_s applied to the pointer. This is load-bearing and currently INVISIBLE at the call
	 * site — the ct.ret == WASM_I32 guards around every caller DO fire for a byref; what saves us is that
	 * mini_get_underlying_type erases byref to MONO_TYPE_I (mini_type_get_underlying_type's first line),
	 * so the switch falls to `default`. Made explicit here so a future "simplification" to
	 * `ret->type` cannot silently corrupt byref-of-primitive returns. */
	if (m_type_is_byref (ret))
		return;
	switch (mini_get_underlying_type (ret)->type) {
	case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: wasm_i32_const (b, 0xff); wasm_op (b, WASM_OP_I32_AND); break;
	case MONO_TYPE_I1: wasm_op (b, WASM_OP_I32_EXTEND8_S); break;
	case MONO_TYPE_CHAR: case MONO_TYPE_U2: wasm_i32_const (b, 0xffff); wasm_op (b, WASM_OP_I32_AND); break;
	case MONO_TYPE_I2: wasm_op (b, WASM_OP_I32_EXTEND16_S); break;
	default: break;
	}
}

static gboolean
wasm_ld (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->addrslot && c->addrslot [vreg] >= 0)   /* address-taken local: read from the addr frame */
		return wasm_addr_ld (b, c, vreg);
	if (c->refslot && c->refslot [vreg] >= 0) {
		if (c->ref_wt && c->ref_wt [vreg]) {
			/* write-through ref: the wasm local is the value home; the slot copy (stored at every
			 * def) pins the referent so this cached value can't be moved out from under us. */
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->li [vreg]);
			return TRUE;
		}
		/* reference vreg: load from the GC-scanned ref shadow stack */
#ifdef HOST_BROWSER
		/* STOREGUARD, LOAD SIDE. kinds 0/1 were store-only, so a frame LOAD through a bad refbase was
		 * the one piece of emitter memory traffic no guard covered -- and after R269 excluded a wild
		 * membase base (OBJGUARD kind 2/3), a bad membase EFFECTIVE address (kind 4, which adds the
		 * offset), a garbage receiver (kind 5) and a misclassified ref (MISSEDREF), it is what the
		 * surviving OOB hypothesis rests on.
		 *
		 * Emitted INLINE rather than through wasm_guard_memaddr on purpose: that helper calls wasm_ld
		 * to push its base, so guarding wasm_ld with it would recurse at EMIT time. wasm_st's kind-0
		 * guard three hundred lines below already uses this same direct shape for the same reason. */
		if (G_UNLIKELY (c->storeguard && c->check_ti >= 0)) {
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
			wasm_i32_const (b, (gint32) (c->refslot [vreg] * 4));
			wasm_op (b, WASM_OP_I32_ADD);
			wasm_i32_const (b, 0);
			wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
		}
#endif
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		wasm_op (b, WASM_OP_I32_LOAD); wasm_memarg (b, 2, (guint32) (c->refslot [vreg] * 4));
		return TRUE;
	}
	if (c->li [vreg] < 0)
		return FALSE;
	wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->li [vreg]);
	return TRUE;
}

static gboolean
wasm_guard_memaddr (WasmBuf *b, WjCtx *c, int base_vreg, gint32 offset)
{
#ifdef HOST_BROWSER
	if (G_UNLIKELY (c->objguard && c->check_ti >= 0)) {
		extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
		if (!wasm_ld (b, c, base_vreg))
			return FALSE;
		if (offset) {
			wasm_i32_const (b, offset);
			wasm_op (b, WASM_OP_I32_ADD);
		}
		wasm_i32_const (b, 4);   /* generic membase access address */
		wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
	}
#endif
	return TRUE;
}

static gboolean
wasm_st (WasmBuf *b, WjCtx *c, int vreg)
{
	if (vreg < 0 || vreg >= c->nvreg)
		return FALSE;
	if (c->addrslot && c->addrslot [vreg] >= 0)   /* address-taken local: write to the addr frame */
		return wasm_addr_st (b, c, vreg);
	if (c->refslot && c->refslot [vreg] >= 0) {
		/* reference vreg: store the value (already on the wasm stack) to the ref shadow stack.
		 * i32.store wants [addr, val] but val is on top, so stash it via a local then push addr+val.
		 * Write-through mode stashes into the vreg's OWN local (which becomes the value home);
		 * slot-homed mode uses the shared rtmp scratch. Either way the slot is written at every def
		 * — adjacent to the def, with no GC point in between — so the pin copy is always current. */
		int stash = (c->ref_wt && c->ref_wt [vreg]) ? c->li [vreg] : c->rtmp;
		wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) stash);
		/* lazy_frame 2 (MONO_WASM_JIT_LAZY_COLD): the frame is only ever live inside a raise or poll taken
		 * arm, never at a def, and the gate required every slotted ref to be write-through -- the local
		 * just set IS the value home, and ENSURE_REF_FRAME copies it into the slot when a GC can happen. */
		if (c->lazy_frame == 2)
			return TRUE;
		if (c->lazy_frame) {
			/* Before materialization the wasm local is the sole value home and there is no valid
			 * refbase to mirror into. Once active, retain ordinary write-through semantics. */
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->frame_active);
			wasm_op (b, WASM_OP_IF); wasm_u8 (b, 0x40);
		}
#ifdef HOST_BROWSER
		if (G_UNLIKELY (c->storeguard)) {
			/* check_store(refbase + slot*4, 0) before the store (STOREGUARD; arenas+helper are HOST_BROWSER) */
			extern void mono_wasm_jit_check_store (guint8 *addr, int kind);
			wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
			wasm_i32_const (b, (gint32) (c->refslot [vreg] * 4));
			wasm_op (b, WASM_OP_I32_ADD);
			wasm_i32_const (b, 0);
			wj_emit_helper_call (b, (gpointer) mono_wasm_jit_check_store, (guint32) c->check_ti);
		}
#endif
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		wasm_op_local (b, WASM_OP_LOCAL_GET, (guint32) stash);
		wasm_op (b, WASM_OP_I32_STORE); wasm_memarg (b, 2, (guint32) (c->refslot [vreg] * 4));
		if (c->lazy_frame)
			wasm_op (b, WASM_OP_END);
		return TRUE;
	}
	if (c->li [vreg] < 0)
		return FALSE;
	wasm_op_local (b, WASM_OP_LOCAL_SET, (guint32) c->li [vreg]);
	return TRUE;
}

static gboolean
functype_eq (const WasmFuncType *a, const WasmFuncType *b)
{
	guint32 i;
	if (a->nparams != b->nparams || a->ret != b->ret)
		return FALSE;
	for (i = 0; i < a->nparams; ++i)
		if (a->params [i] != b->params [i])
			return FALSE;
	return TRUE;
}

static guint32
wj_functype_hash (const WasmFuncType *t)
{
	guint32 h = 2166136261u, i;
	h = (h ^ t->nparams) * 16777619u;
	for (i = 0; i < t->nparams; ++i)
		h = (h ^ (guint32)t->params [i]) * 16777619u;
	return (h ^ (guint32)t->ret) * 16777619u;
}

/* Opcode -> name, for the ref-safety IR dump (MONO_WASM_JIT_REFDIAG) and the bail message. mono_inst_name
 * is compiled out under DISABLE_LOGGING, so re-include mini-ops.h with our own MINI_OP to build a private
 * name table. (Defined unconditionally — also used by the WASM_JIT_BAIL print, incl. the offline dump.) */
static const char *
wj_opname (int op)
{
	switch (op) {
#undef MINI_OP
#undef MINI_OP3
#define MINI_OP(a,b,dest,src1,src2) case a: return b;
#define MINI_OP3(a,b,dest,src1,src2,src3) case a: return b;
#include "mini-ops.h"
#undef MINI_OP
#undef MINI_OP3
	default: return "?";
	}
}

/* wasm float comparison op for width (is_f32 ? f32 : f64) and kind: 0=EQ 1=NE 2=LT 3=GT 4=LE 5=GE.
 * wasm float compares are ORDERED except NE (which is unordered: true on NaN). _UN ordered-relation
 * variants (lt_un etc.) are emitted by the caller as the complementary ordered op + i32.eqz. */
static WasmOpcode
wj_fcmp_op (gboolean is_f32, int kind)
{
	switch (kind) {
	case 0:  return is_f32 ? WASM_OP_F32_EQ : WASM_OP_F64_EQ;
	case 1:  return is_f32 ? WASM_OP_F32_NE : WASM_OP_F64_NE;
	case 2:  return is_f32 ? WASM_OP_F32_LT : WASM_OP_F64_LT;
	case 3:  return is_f32 ? WASM_OP_F32_GT : WASM_OP_F64_GT;
	case 4:  return is_f32 ? WASM_OP_F32_LE : WASM_OP_F64_LE;
	default: return is_f32 ? WASM_OP_F32_GE : WASM_OP_F64_GE;
	}
}

#ifdef HOST_BROWSER
/* The synchronized-INNER wrapper (the dummy a MONO_WRAPPER_SYNCHRONIZED wrapper calls to reach the real
 * body — see method-to-ir.c) is created FRESH on every IR build (mono_marshal_get_synchronized_inner_wrapper
 * is uncached), so its MonoMethod — and thus its InterpMethod and wasm f-slot — changes on every compile.
 * Under the f-slot model a synchronized wrapper could then NEVER observe its inner as JITted: it re-bails
 * "callee not jitted" and the island re-compiles a fresh inner each pass (a table-slot leak + render
 * hitches). Canonicalize it: map the wrapped method -> the FIRST inner-wrapper instance we see and always
 * use THAT one for the f-slot lookup / island blocker / compile, so the slot is stable. Every inner wrapper
 * for a given method is the same stub (resolved to the same body), so substituting the canonical one is
 * sound. Other callees pass through unchanged. */
static GHashTable *wj_sync_inner_canon;   /* wrapped MonoMethod* -> canonical synchronized-inner-wrapper MonoMethod* */
static MonoMethod *
wj_canonical_callee (MonoMethod *m)
{
	WrapperInfo *info;
	MonoMethod *wrapped, *canon;
	if (!m || m->wrapper_type != MONO_WRAPPER_OTHER)
		return m;
	info = mono_marshal_get_wrapper_info (m);
	if (!info || info->subtype != WRAPPER_SUBTYPE_SYNCHRONIZED_INNER)
		return m;
	wrapped = info->d.synchronized_inner.method;
	if (!wrapped)
		return m;
	mono_loader_lock ();
	if (!wj_sync_inner_canon)
		wj_sync_inner_canon = g_hash_table_new (g_direct_hash, g_direct_equal);
	canon = (MonoMethod *) g_hash_table_lookup (wj_sync_inner_canon, wrapped);
	if (!canon) { canon = m; g_hash_table_insert (wj_sync_inner_canon, wrapped, m); }
	mono_loader_unlock ();
	/* This table NEVER removes an entry and its values were captured on some earlier compile, so it is
	 * the largest retention window in the backend (DANGLING-MONOMETHOD.md ranks it first). Validate the
	 * value at the point of USE and fall back to the caller's own method, which is live by construction
	 * -- losing canonicalisation costs f-slot stability, not correctness. */
	if (canon != m && G_UNLIKELY (!mono_wasm_jit_method_usable (canon, WJ_BADMETH_SITE_CANON))) {
		/* Replace the dead value with this compile's live one, so the next lookup finds a usable entry
		 * instead of re-detecting the same corpse. `wrapped` came from m's own wrapper info, i.e. is live. */
		mono_loader_lock ();
		if (g_hash_table_lookup (wj_sync_inner_canon, wrapped) == canon)
			g_hash_table_insert (wj_sync_inner_canon, wrapped, m);
		mono_loader_unlock ();
		return m;
	}
	/*
	 * THE DISCRIMINATING MEASUREMENT (DANGLING-MONOMETHOD.md). Fixing the emit-time predicate stops it
	 * dereferencing a retained pointer, but it does NOT retire this table -- and the doc is explicit that
	 * the cheap thing to do first is find out whether the crashing pointer came from here or from the
	 * IR's own call->method, because that decides whether the expensive fix (purging retained pointers on
	 * mono_mem_manager_free, which needs a reverse index and must be safe against lock-free readers) is
	 * justified at all.
	 *
	 * This counter is that measurement: how many emit-time callees were SUBSTITUTED out of a table that
	 * never removes an entry and whose values were captured on some earlier compile. A large number means
	 * the exposure is broad and survives today's fix; a small one means the table is a narrow special
	 * case and the IR's own pointer is the thing to chase.
	 */
	if (canon != m)
		mono_wasm_jit_counters [WJC_CANON_SUBSTITUTED]++;
	return canon;
}

/* Published method effect used by the caller's GC-liveness pass. Direct callees are island-built and
 * registered before their callers, so looking through the immutable f-slot descriptor gives us a
 * naturally transitive bottom-up no-GC classification without a separate whole-program pass. */
static gboolean
wj_fslot_is_nogc (int fslot)
{
	int ci, desc_id;
	WjRegEntry *re;
	if (fslot <= 0)
		return FALSE;
	ci = fslot / WJ_SLOT_CHUNK;
	if (ci < 0 || ci >= WJ_SLOT_NCHUNKS || !wj_fslot_desc_chunks [ci])
		return FALSE;
	mono_memory_barrier ();
	desc_id = wj_fslot_desc_chunks [ci][fslot % WJ_SLOT_CHUNK];
	if (desc_id <= 0 || desc_id > wj_reg_n)
		return FALSE;
	re = wj_reg_at (desc_id - 1);
	return re && re->f == fslot && re->no_gc;
}

/* The descriptor whose f-slot (kind 0) or e-slot (kind 1) is `idx`, or 0 if `idx` is not a JIT slot at all
 * -- e.g. a C helper's or an AOT body's table index, which is what a fixed-index reloc is supposed to name.
 * Reads only the slot->descriptor map and the registry entry; nothing the index points AT. (R291) */
static int
wj_fixed_index_jit_desc (int idx, int *out_kind)
{
	int probe, k;
	for (k = 0; k < 2; ++k) {
		int ci, desc_id;
		WjRegEntry *re;
		probe = k == 0 ? idx : idx + 1;     /* an e-slot is allocated immediately before its f-slot */
		if (probe <= 0)
			continue;
		ci = probe / WJ_SLOT_CHUNK;
		if (ci < 0 || ci >= WJ_SLOT_NCHUNKS || !wj_fslot_desc_chunks [ci])
			continue;
		mono_memory_barrier ();
		desc_id = wj_fslot_desc_chunks [ci][probe % WJ_SLOT_CHUNK];
		if (desc_id <= 0 || desc_id > wj_reg_n)
			continue;
		re = wj_reg_at (desc_id - 1);
		if (re && (k == 0 ? re->f == idx : re->e == idx)) {
			*out_kind = k;
			return desc_id;
		}
	}
	return 0;
}

/* Return the admitted direct managed callee's f-slot, or zero for every shape whose dispatch can enter
 * the runtime (rgctx, icall, virtual/residual, unresolved, or recursion). Keep the synchronized-wrapper
 * normalization identical to the actual OP_CALL lowering. */
static int
wj_direct_admitted_fslot (MonoCompile *cfg, MonoInst *ins)
{
	MonoCallInst *call;
	MonoMethod *m;
	extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
	extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
	switch (ins->opcode) {
	case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
		break;
	default:
		return 0;
	}
	call = (MonoCallInst *) ins;
	m = call->method;
	if (!m || call->rgctx_reg || m == cfg->method)
		return 0;
	if (m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		m = mono_marshal_get_synchronized_wrapper (m);
	m = wj_canonical_callee (m);
	if (!m || m == cfg->method)
		return 0;
	return mono_wasm_jit_get_callee_fslot (m);
}

/* GC-point classifier after resolving direct method effects. Calls outside this narrow admitted-direct
 * set remain conservative. A no-GC callee may itself call earlier no-GC callees, making this transitive. */
static gboolean
wj_ins_is_effective_gcpoint (MonoCompile *cfg, MonoInst *ins)
{
	if (!wj_ins_is_gcpoint (ins, wj_method_raise_exempt (cfg)))
		return FALSE;
	return !wj_fslot_is_nogc (wj_direct_admitted_fslot (cfg, ins));
}

/* The same classification as seen by the consumers that PUBLISH a no-collection claim outside this
 * frame -- method_nogc (read by every caller's direct-call admission), effective_gcp_count (gates the
 * lazy frame), prior_gcps_are_polls and the terminal-vcall handoff. Under RAISE_NOGC a raise counts
 * here even though it does not count for this method's own def/use generations, because the raise really
 * does allocate; only the "nothing in THIS frame reads a vreg afterwards" half of the argument survives.
 * Identical to wj_ins_is_effective_gcpoint with the knob off. */
static gboolean
wj_ins_is_published_gcpoint (MonoCompile *cfg, MonoInst *ins)
{
	if (mono_wasm_jit_raise_nogc && wj_ins_is_raise (ins))
		return TRUE;   /* no raise opcode is a call, so the direct-callee resolution cannot apply */
	return wj_ins_is_effective_gcpoint (cfg, ins);
}

/*
 * A virtual forwarding call can take ownership of its reference roots at the call boundary:
 *
 *  - a compact-PIC hit has no managed safepoint between loading the caller's wasm locals and entering
 *    the already-admitted target;
 *  - a miss first acquires a worker-local conservative root frame (native allocation/root-table work,
 *    no managed allocation), copies every argument into it, and only then resolves or invokes code
 *    which may collect.
 *
 * Keep this tied to the exact machinery providing that guarantee. The legacy residual scratch is not
 * a GC root and must continue to make the caller retain its slots.
 */
static gboolean
wj_ins_is_pinned_vcall_forward (MonoInst *ins)
{
	MonoCallInst *call;

	switch (ins->opcode) {
	case OP_CALL_MEMBASE: case OP_VOIDCALL_MEMBASE: case OP_FCALL_MEMBASE:
	case OP_LCALL_MEMBASE: case OP_RCALL_MEMBASE:
		break;
	default:
		return FALSE;
	}
	call = (MonoCallInst *) ins;
	/* ALWAYS FALSE IN THE SHIPPED CONFIGURATION, AND THAT IS LOAD-BEARING RATHER THAN VESTIGIAL.
	 *
	 * This used to read `mono_wasm_jit_vcall_inline_ic && mono_wasm_jit_vcall_shared_miss_enabled && ...`.
	 * INLINE_IC ships 1, so dropping that conjunct is right -- but VCALL_SHARED_MISS ships **0**, so the
	 * whole expression has always evaluated FALSE in the product, and dropping THAT conjunct silently
	 * turns the classification back ON. It is not a no-op: when baking a knob to its shipped value,
	 * substitute the VALUE, never delete the conjunct. For a knob that ships 0 the guarded code goes
	 * away -- the guard does not.
	 *
	 * WHY IT MUST STAY OFF. This predicate is what lets a forwarding virtual call's argument SKIP the
	 * used-at-a-GC-point clause in WJ_SL_USE, on the theory that the callee takes ownership of the root.
	 * That promise does not hold on the PIC MISS path: every observed failure landed in
	 * mini_llvmonly_init_vtable_slot -> resolve_vcall on a receiver sgen had already turned into a
	 * nursery filler. It is the mechanism behind the whole MONO_WASM_JIT_RAISE_NOGC saga, and the
	 * measurement is unambiguous: "disabling wj_ins_is_pinned_vcall_forward outright (SHARED_MISS=0) is
	 * clean 0/4 against a 4/4 control; gating only the `_fwd` skip in WJ_SL_USE changed nothing (3/4 vs
	 * 2/4)".
	 *
	 * So the product has been safe by a side effect of a PERFORMANCE decision -- VCALL_SHARED_MISS ships
	 * 0 because turning it off measured -13.2%% p50 / +19.9%% fps, which has nothing to do with the GC.
	 * Those two facts sat ~3,800 lines apart in this file and were never joined up, which is why
	 * raise_nogc=1 read as "unverified" for so long. Returning FALSE makes the protection DELIBERATE
	 * instead of accidental.
	 *
	 * BE PRECISE ABOUT WHAT THIS DOES AND DOES NOT DISABLE. `_fwd` in WJ_SL_USE has TWO sources:
	 *     forward_call = gcp && (wj_direct_admitted_fslot (cfg, insl) > 0 || pinned_vforward)
	 * Only the second is this predicate. The first -- a DIRECT call to an already-admitted f-slot --
	 * stays live and is sound: that callee really does take ownership of the root, with no PIC miss in
	 * between. The measurement separates them explicitly: "gating only the `_fwd` skip in WJ_SL_USE
	 * changed nothing (3/4 vs 2/4), which is what pins it to THIS consumer". So the claim here is the
	 * narrow one -- the UNSOUND consumer is off -- not "no argument ever skips its pin".
	 *
	 * RAISE_NOGC=2 RE-ARMS IT, and that is the whole point of level 2: it is a POSITIVE CONTROL, not a
	 * shipping mode. Gating the control HERE rather than only on the gen_skipped_raises term is what
	 * makes it a control at all -- with this predicate FALSE, that term is never read, so levels 1 and 2
	 * would be the same codegen and a clean level-2 run would prove nothing. Level 2 restores the exact
	 * pre-fix shape: the classification on, and the guard off. */
	{
		extern int mono_wasm_jit_raise_nogc;
		if (mono_wasm_jit_raise_nogc != 2)
			return FALSE;
	}
	return call->method && (call->method->flags & METHOD_ATTRIBUTE_VIRTUAL) &&
		call->signature && call->signature->hasthis && call->call_info;
}

/* Append a blocking callee to the cfg result, deduped + capped. Shared by the residual=0 pre-scan
 * (enumerates the full set) and the emit bail site (records the first blocker it hits) so the two can't
 * disagree on the head of the list. */
static void
wj_result_add_blocker (MonoWasmJitResult *res, MonoMethod *m)
{
	int i;
	for (i = 0; i < res->nblockers; ++i)
		if (res->blockers [i] == m)
			return;
	if (res->nblockers < MONO_WASM_JIT_MAX_BLOCKERS)
		res->blockers [res->nblockers++] = m;
	else {
		/* The closure handed to the island/SCC drivers is now INCOMPLETE. MONO_WASM_JIT_MAX_BLOCKERS is
		 * 32 against WJ_SCC_MAX 64, so a member with more distinct blockers than the array holds gives the
		 * batcher a set it cannot close -- it folds what it can see, fails to reach a fixpoint, hits
		 * !progress and condemns EVERY member permanently. A truncation silently becoming a terminal
		 * verdict. `blockers_truncated` has been set here and read by nothing; this counts it so the rate
		 * is known before Stage 2 makes truncation a transient refusal. */
		if (G_UNLIKELY (mono_wasm_jit_stats) && !res->blockers_truncated)
			mono_wasm_jit_count (WJC_BLOCKERS_TRUNCATED);
		res->blockers_truncated = 1;
	}
}

static void
wj_result_add_direct_dep (MonoWasmJitResult *res, int fslot, guint32 sig_id, MonoMethod *callee)
{
	int i;
	if (fslot <= 0)
		return;
	for (i = 0; i < res->ndirect_deps; ++i)
		if (res->direct_deps [i] == fslot) {
			if (res->direct_dep_sig [i] != sig_id)
				res->direct_deps_truncated = 1; /* same slot observed with two ABIs: reject emission */
			return;
		}
	if (res->ndirect_deps < MONO_WASM_JIT_MAX_DIRECT_DEPS) {
		res->direct_deps [res->ndirect_deps] = fslot;
		res->direct_dep_method [res->ndirect_deps] = callee;
		res->direct_dep_sig [res->ndirect_deps++] = sig_id;
	}
	else
		res->direct_deps_truncated = 1;
}

/* Pre-scan (residual=0 / islands only): enumerate ALL direct un-JITted callees that would HARD-BLOCK this
 * method's compile, into cfg->wasm_jit_result.blockers — so wasm_jit_force_island can pull them all in one
 * emit cycle instead of re-emitting the method once per blocker (the emit itself still bails at the FIRST
 * blocker; this just hands the island builder the full set up front). The predicate MUST mirror the bail
 * site in the OP_CALL lowering below: a direct OP_CALL-family call, method!=NULL, callee has no f-slot, not
 * an rgctx call, not AOT-residual-eligible, not (residual_perm && perm-unjittable). INLINE_AOT (default
 * off) is not modelled — when on it can only REMOVE a blocker (inlines an AOT callee), so the worst case is
 * force_island wasting a little budget on a callee that would actually inline; never a correctness issue. */
static void
wj_prescan_blockers (MonoCompile *cfg)
{
	extern int mono_wasm_jit_get_callee_fslot (MonoMethod *m);
	extern gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);
	extern int mono_wasm_jit_callee_perm_unjittable (MonoMethod *m);
	extern int mono_wasm_jit_callee_too_cold (MonoMethod *m);
	extern int mono_wasm_jit_residual_cold;
	extern MonoMethod *mono_marshal_get_synchronized_wrapper (MonoMethod *enter_method);
	MonoBasicBlock *bb;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			MonoCallInst *call;
			MonoMethod *call_method;
			MonoMethodSignature *csig;
			switch (ins->opcode) {
			case OP_CALL: case OP_VOIDCALL: case OP_FCALL: case OP_LCALL: case OP_RCALL: case OP_VCALL2:
				break;
			default:
				continue;
			}
			call = (MonoCallInst *) ins;
			call_method = call->method;
			csig = call->signature;
			if (!call_method || !csig)        /* method==NULL = JIT-icall (never a managed blocker); no sig handled by emit */
				continue;
			if (call->rgctx_reg)              /* rgctx call -> routed through residual, never a hard blocker */
				continue;
			if (call_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
				call_method = mono_marshal_get_synchronized_wrapper (call_method);
			call_method = wj_canonical_callee (call_method);   /* stabilize the per-compile synchronized-inner wrapper */
			if (call_method == cfg->method)                                                        /* self-recursion: baked via self-slot reservation, not a blocker */
				continue;
			if (mono_wasm_jit_get_callee_fslot (call_method) > 0)                                   /* already JITted */
				continue;
			if (mono_interp_jit_call_supported (call_method, csig))    /* AOT-routed residual */
				continue;
			/* MONO_WASM_JIT_RESIDUAL_PERM (Lever B) ships 1: under residual=0, residual-route ONLY a
			 * permanently-un-JITtable callee rather than treating it as an island blocker. A permanent
			 * blocker cannot ever be cleared, so parking the caller on it parks it forever. */
			if (mono_wasm_jit_callee_perm_unjittable (call_method))   /* perm-unjittable -> residual */
				continue;
			if (mono_wasm_jit_residual_cold && mono_wasm_jit_callee_too_cold (call_method))          /* cold leaf -> residual, not a hard blocker */
				continue;
			wj_result_add_blocker (&cfg->wasm_jit_result, call_method);
		}
	}
}
#endif

/* Scalar-vtype call ABI (MONO_WASM_JIT_VTYPE_SCALAR / _REF). A struct mini_wasm_is_scalar_vtype accepts
 * (<=8 bytes, exactly one field) is passed in the wasm ABI BY ITS SINGLE FIELD's scalar, not by address
 * (get_storage -> ArgVtypeAsScalar -> LLVMArgWasmVtypeAsScalar). wj_scalar_vtype_valtype returns that
 * scalar's wasm valtype (sets *out, returns TRUE), else FALSE. A ref-free struct is gated on
 * VTYPE_SCALAR (its ByVal value lives in an addr-frame slot); a ref-etype one (single object field,
 * e.g. RuntimeTypeHandle{RuntimeType}) on VTYPE_SCALAR_REF — its single ref lives in a GC-scanned
 * ref-shadow slot (ldaddr pre-pass sentinel addrslot=-2) so it is tracked as a pinning root.
 * Cross-available (used by the emit below in both builds). */
static gboolean
wj_scalar_vtype_valtype (MonoType *t, WasmValtype *out)
{
	MonoType *ut, *etype = NULL;
	MonoClass *k;
	WasmValtype ev;
	if (!t || m_type_is_byref (t))
		return FALSE;
	ut = mini_get_underlying_type (t);
	if (ut->type != MONO_TYPE_VALUETYPE && ut->type != MONO_TYPE_GENERICINST)
		return FALSE;
	if (mini_is_gsharedvt_variable_type (ut))
		return FALSE;
	if (!mini_wasm_is_scalar_vtype (ut, &etype) || !etype)
		return FALSE;
	k = mono_class_from_mono_type_internal (ut);
	if (!k)
		return FALSE;
	/* MONO_WASM_JIT_VTYPE_SCALAR and _SCALAR_REF gated these two arms and both shipped 1. The ref-etype
	 * arm is the GC-critical one: the single reference lives in a GC-SCANNED ref-shadow slot, so
	 * OP_LDADDR yields refbase+slot*4 and the field store/load track it as a pinning root, and the
	 * store's inline card barrier marks a HARMLESS card (wasm32 has no overlapping cards -- the 8 MB
	 * table covers the whole 32-bit space, so a non-heap mark is in-bounds and never scanned). Both
	 * arms are now unconditional; validate any change here with STOREGUARD/OBJGUARD, not with fps. */
	if (m_class_has_references (k) || m_class_has_ref_fields (k)) {
		if (!mini_type_is_reference (etype))
			return FALSE;   /* a ref-BEARING vtype is scalar only when its single field IS the ref */
	}
	ev = wasm_valtype_of_type (etype);
	if (ev == 0 || ev == WASM_VOID)
		return FALSE;
	*out = ev;
	return TRUE;
}

/*
 * WasmCallInfo: the single declarative wasm ABI descriptor for a MonoMethodSignature, shared by every
 * call-emit path (method self-sig, JIT->JIT, inline-AOT, interp residual, vcall ICs). It replaces the
 * ~8 sites that each re-derived a WasmFuncType from the signature by hand (the reviewer's "several bugs
 * come from paths independently reconstructing the same ABI"). mono_wasm_get_call_info builds the
 * CANONICAL callee functype -- [this i32] + params -> ret, with NO trailing rgctx/extra arg (that is a
 * per-call-site concern, layered on top where needed) -- using exactly the rules the direct-call path
 * used: this -> i32, each param via wasm_valtype_of_type with a ref-free/ref scalar-vtype fallback
 * (LLVMArgWasmVtypeAsScalar), ret via wasm_valtype_of_type. Unrepresentable shapes leave valid=FALSE
 * with a fail_reason the consumer bails on (preserving today's bail accounting). Hidden vret
 * (ArgValuetypeAddrInIReg) sets vret_byaddr and, until the vret lowering lands, still bails.
 */
typedef enum {
	WJ_ARG_SCALAR,          /* one wasm scalar (incl. managed byref/ptr/obj as i32, i8 as i64, ...) */
	WJ_ARG_VTYPE_SCALAR,    /* ArgVtypeAsScalar: struct passed as its single-field etype scalar */
	WJ_ARG_VTYPE_BYADDR,    /* ArgValuetypeAddrOnStack: i32 pointer to a caller-owned copy (MONO_WASM_JIT_VTYPE_BYADDR) */
	WJ_ARG_GSHAREDVT,       /* ArgGsharedVTOnStack: unsupported here */
	WJ_ARG_INVALID,
} WjArgKind;

typedef struct {
	WjArgKind kind;
	WasmValtype wtype;      /* wasm valtype (this -> WASM_I32; WASM_VOID only for a void ret) */
	MonoType *type;         /* the managed arg/ret type (NULL for the synthetic this) */
	MonoType *etype;        /* scalar-vtype single-field etype, else NULL */
	gint32 vsize, valign;   /* WJ_ARG_VTYPE_BYADDR (and byaddr ret): mono_class_value_size of the struct, else 0 */
} WjArgInfo;

typedef struct {
	guint8 valid;           /* FALSE -> the consumer bails with fail_reason */
	const char *fail_reason;
	gint16 fail_arg;         /* invalid ABI: -1 return, >=0 explicit parameter, -2 non-type failure */
	guint8 hasthis;
	guint8 vret_byaddr;     /* ret is a by-address vtype (hidden vret). Valid under MONO_WASM_JIT_VRET: the
	                         * internal convention appends a TRAILING i32 vret-pointer param to ftype and the
	                         * wasm ret becomes void (managed-arg -> wasm-param index mapping stays identity;
	                         * the native AOT ABI is vret-FIRST — the inline-AOT call path reorders). With the
	                         * flag off it implies !valid, as before. */
	int nargs;              /* hasthis + param_count (NOT counting the vret param; ftype.nparams does) */
	WjArgInfo ret;
	WjArgInfo args [WASM_FUNCTYPE_MAX_PARAMS];
	WasmFuncType ftype;     /* canonical callee functype: [this i32] + params [+ vret i32] -> ret */
	guint32 f_sig_id;       /* wj_functype_hash (&ftype); the persisted per-method ABI fingerprint */
} WasmCallInfo;

/*
 * TRUE iff T is a plain by-address-able value type — the ArgValuetypeAddrOnStack /
 * ArgValuetypeAddrInIReg class of get_storage above, and ONLY that class: scalar-vtypes are
 * excluded so WJ_ARG_VTYPE_BYADDR <=> the native AOT ABI also passes a pointer (the inline-AOT
 * path then never has an internal-vs-native kind mismatch). SIMD is excluded (16-byte interp
 * alignment + v128 ABI, neither wired up); gsharedvt-variable is not a concrete layout; the size
 * cap matches the ldaddr-vtype addr-frame cap.
 */
static gboolean
wj_byaddr_vtype (MonoType *t, gint32 *vsize, gint32 *valign)
{
	MonoType *ut;
	MonoClass *k;
	guint32 al = 0;
	if (m_type_is_byref (t))
		return FALSE;
	ut = mini_get_underlying_type (t);
	if (!MONO_TYPE_ISSTRUCT (ut))
		return FALSE;
	if (mini_is_gsharedvt_variable_type (ut))
		return FALSE;
	{
		MonoType *setype = NULL;
		if (mini_wasm_is_scalar_vtype (ut, &setype))
			return FALSE;
	}
	k = mono_class_from_mono_type_internal (ut);
	if (!k || m_class_is_simd_type (k))
		return FALSE;
	*vsize = (gint32) mono_class_value_size (k, &al);
	*valign = (gint32) al;
	if (*vsize <= 0 || *vsize > 4096)
		return FALSE;
	return TRUE;
}

/* Can a delegate target be reached by the EMITTED delegate PIC -- i.e. does its signature lower to a
 * functype the emitted call site can express?
 *
 * THIS IS A DIFFERENT QUESTION FROM `scalar`, AND CONFLATING THEM IS WHY 100% OF DELEGATE SLOW-PATH
 * DISPATCHES ARE BY-VALUE VTYPE ARGS (9,354,181 per window, measured).
 *
 * `scalar` -- `mono_mint_type (t) != MINT_TYPE_VT` per param -- is about the INTERPRETER'S STACKVAL
 * LAYOUT. It exists because the `e` entry thunk reads its arguments at interp offsets with vtypes
 * stored INLINE, while the JIT scratch is a flat 8-byte grid holding a vtype's copy ADDRESS; the two
 * coincide only when every argument is a plain scalar. That is a real constraint ON `e`.
 *
 * The EMITTED PIC never touches the interp layout. It calls the target's `f` directly through the call
 * site's OWN lowered functype (`ftd`/`dftd`, built from mono_wasm_get_call_info over the Invoke
 * signature), and that lowering already ADMITS WJ_ARG_VTYPE_BYADDR -- a by-value vtype becomes an i32
 * pointer on both sides, which is exactly what the JIT caller spilled. So the PIC could always have
 * dispatched these targets; it was refused by a flag describing a constraint it does not have.
 *
 * The condition here is therefore the one the CALL actually needs: every argument lowers to one of the
 * three kinds the emitted site accepts, and the return is not by-address (a site with a by-addr vtype
 * return fails emission outright -- "vcall ret type" -- which is why the VTRET bucket measured 0).
 *
 * Called only when a recipe is CREATED, from wasm_jit_prepare_delegate_call, which is the documented
 * pre-spill point: the caller's arguments are still live in its GC-scanned shadow frame, so resolving a
 * signature here is safe in a way it is NOT at the dispatch site. */
static void mono_wasm_get_call_info (MonoMethodSignature *sig, WasmCallInfo *ci);

gboolean
mono_wasm_jit_delegate_abi_ok (MonoMethod *target)
{
	MonoMethodSignature *sig;
	WasmCallInfo ci;
	int i;

	if (!target)
		return FALSE;
	sig = mono_method_signature_internal (target);
	if (!sig)
		return FALSE;
	mono_wasm_get_call_info (sig, &ci);
	if (!ci.valid || ci.vret_byaddr)
		return FALSE;
	for (i = 0; i < ci.nargs; ++i) {
		if (ci.args [i].kind != WJ_ARG_SCALAR && ci.args [i].kind != WJ_ARG_VTYPE_SCALAR &&
		    ci.args [i].kind != WJ_ARG_VTYPE_BYADDR)
			return FALSE;
	}
	return TRUE;
}

/* C-side twins of the by-addr classification, for the residual marshal (interp.c): call_interp /
 * aot_call_lean must deref EXACTLY the scratch slots the emitter spilled as copy ADDRESSES (a by-addr
 * vtype arg) and write VT returns through the caller pointer at WJ_SCRATCH_VRET_OFF — nothing else.
 * Includes the flag gates so classification is identical on both sides of the boundary. */
gboolean
mono_wasm_jit_arg_is_byaddr (MonoType *t)
{
	gint32 sz, al;
	return wj_byaddr_vtype (t, &sz, &al);
}

gboolean
mono_wasm_jit_ret_is_byaddr (MonoType *t)
{
	gint32 sz, al;
	return wj_byaddr_vtype (t, &sz, &al);
}


static void
mono_wasm_get_call_info (MonoMethodSignature *sig, WasmCallInfo *ci)
{
	int i;
	memset (ci, 0, sizeof (*ci));
	ci->fail_arg = -2;
	ci->valid = TRUE;
	ci->hasthis = sig->hasthis ? 1 : 0;
	ci->nargs = sig->hasthis + sig->param_count;
	if (ci->nargs > WASM_FUNCTYPE_MAX_PARAMS) { ci->valid = FALSE; ci->fail_reason = "call nargs"; return; }

	if (sig->hasthis) {
		ci->args [0].kind = WJ_ARG_SCALAR;
		ci->args [0].wtype = WASM_I32;
		ci->ftype.params [ci->ftype.nparams++] = WASM_I32;
	}
	for (i = 0; i < (int) sig->param_count; ++i) {
		WjArgInfo *a = &ci->args [i + sig->hasthis];
		WasmValtype pv = wasm_valtype_of_type (sig->params [i]);
		a->type = sig->params [i];
		if (pv == 0 || pv == WASM_VOID) {
			/* BYVAL scalar-vtype arg: declare the param as its single-field etype scalar (the AOT
			 * callee's LLVMArgWasmVtypeAsScalar ABI). Multi-field/large vtype: i32 pointer to a
			 * caller-owned copy (ArgValuetypeAddrOnStack). Anything else is unrepresentable -> bail. */
			WasmValtype sv;
			if (wj_scalar_vtype_valtype (sig->params [i], &sv)) {
				a->kind = WJ_ARG_VTYPE_SCALAR;
				a->etype = sig->params [i];
				pv = sv;
			} else if (wj_byaddr_vtype (sig->params [i], &a->vsize, &a->valign)) {
				a->kind = WJ_ARG_VTYPE_BYADDR;
				pv = WASM_I32;
			} else {
				ci->valid = FALSE;
				ci->fail_arg = (gint16) i;
				ci->fail_reason = mini_is_gsharedvt_variable_type (mini_get_underlying_type (sig->params [i]))
					? "call arg gsharedvt" : "call arg type";
				return;
			}
		} else {
			a->kind = WJ_ARG_SCALAR;
		}
		a->wtype = pv;
		ci->ftype.params [ci->ftype.nparams++] = pv;
	}

	ci->ret.type = sig->ret;
	if (sig->ret->type == MONO_TYPE_VOID) {
		ci->ret.kind = WJ_ARG_SCALAR;
		ci->ret.wtype = WASM_VOID;
		ci->ftype.ret = WASM_VOID;
	} else {
		WasmValtype rv = wasm_valtype_of_type (sig->ret);
		if (rv == 0 || rv == WASM_VOID) {
			WasmValtype sv;
			if (wj_scalar_vtype_valtype (sig->ret, &sv)) {
				/* ArgVtypeAsScalar: return the struct's single field directly, matching LLVM's
				 * native wasm ABI. */
				ci->ret.kind = WJ_ARG_VTYPE_SCALAR;
				ci->ret.etype = sig->ret;
				ci->ret.wtype = sv;
				ci->ftype.ret = sv;
			} else {
				/* Multi-field vtype return via a trailing hidden by-address pointer. */
				ci->vret_byaddr = 1;
				if (!wj_byaddr_vtype (sig->ret, &ci->ret.vsize, &ci->ret.valign)) {
					ci->valid = FALSE;
					ci->fail_arg = -1;
					ci->fail_reason = mini_is_gsharedvt_variable_type (mini_get_underlying_type (sig->ret))
						? "call ret gsharedvt" : "call ret type";
					return;
				}
				if (ci->ftype.nparams + 1 > WASM_FUNCTYPE_MAX_PARAMS) { ci->valid = FALSE; ci->fail_reason = "call nargs"; return; }
				ci->ret.kind = WJ_ARG_VTYPE_BYADDR;
				ci->ret.wtype = WASM_VOID;
				ci->ftype.params [ci->ftype.nparams++] = WASM_I32;
				ci->ftype.ret = WASM_VOID;
			}
		} else {
			ci->ret.kind = WJ_ARG_SCALAR;
			ci->ret.wtype = rv;
			ci->ftype.ret = rv;
		}
	}
	ci->f_sig_id = wj_functype_hash (&ci->ftype);
}

/*
 * Describe a JITted method's `f` signature for the jiterpreter's native->interp entry trampoline, which
 * forwards to it directly with call_indirect.
 *
 * The trampoline is handed each argument as a POINTER to the value (the gsharedvt-in convention), while
 * `f` takes values, so per argument it needs both how to LOAD it and what wasm type results. Both come
 * from mono_wasm_get_call_info here rather than being re-derived in TypeScript: it is the same
 * classification the emitter used to build `f`, and wasm checks call_indirect signatures exactly, so any
 * divergence would be a trap at the first call rather than a bail. (jiterpreter's own
 * mono_jiterp_type_get_raw_value_size cannot serve: it reports 4 for both i32 and r4, which is why the
 * existing marshaller has "FIXME: 4 and 8-byte floats" and routes them to a helper call.)
 *
 * kinds[i]  : WJ_ENTRY_LD_* below — the load to perform on argument i's pointer.
 * vtypes[i] : the wasm valtype of argument i; vtypes[nargs] is the RETURN valtype (WASM_VOID if none).
 *
 * Returns the argument count (including `this`), or -1 if this method is not eligible.
 */
#define WJ_ENTRY_LD_I32      0
#define WJ_ENTRY_LD_I64      1
#define WJ_ENTRY_LD_F32      2
#define WJ_ENTRY_LD_F64      3
#define WJ_ENTRY_LD_I8_S     4
#define WJ_ENTRY_LD_U8       5
#define WJ_ENTRY_LD_I16_S    6
#define WJ_ENTRY_LD_U16      7
#define WJ_ENTRY_LD_ASIS     8   /* the local already holds the value (this-reference, byref, pointer) */

#ifdef HOST_BROWSER
EMSCRIPTEN_KEEPALIVE int
mono_wasm_jit_entry_sig (MonoMethod *method, guint8 *kinds, guint8 *vtypes, int max)
{
	WasmCallInfo ci;
	MonoMethodSignature *sig;
	int i, nargs, nparams;

	if (!method || !kinds || !vtypes)
		return -1;
	sig = mono_method_signature_internal (method);
	if (!sig)
		return -1;
	nparams = (int) sig->param_count;
	nargs = nparams + (sig->hasthis ? 1 : 0);
	if (nargs + 1 > max)
		return -1;

	mono_wasm_get_call_info (sig, &ci);
	if (!ci.valid || ci.vret_byaddr || ci.nargs != nargs)
		return -1;
	for (i = 0; i < nargs; ++i) {
		MonoType *t;
		if (ci.args [i].kind != WJ_ARG_SCALAR)
			return -1;
		vtypes [i] = (guint8) ci.args [i].wtype;

		if (sig->hasthis && i == 0) {
			kinds [i] = WJ_ENTRY_LD_ASIS;   /* the trampoline's this_arg local is the object pointer */
			continue;
		}
		t = sig->params [i - (sig->hasthis ? 1 : 0)];
		if (m_type_is_byref (t)) {
			kinds [i] = WJ_ENTRY_LD_ASIS;   /* byref: the pointer IS the value */
			continue;
		}
		switch (mini_get_underlying_type (t)->type) {
		case MONO_TYPE_BOOLEAN: case MONO_TYPE_U1: kinds [i] = WJ_ENTRY_LD_U8; break;
		case MONO_TYPE_I1:                         kinds [i] = WJ_ENTRY_LD_I8_S; break;
		case MONO_TYPE_CHAR: case MONO_TYPE_U2:    kinds [i] = WJ_ENTRY_LD_U16; break;
		case MONO_TYPE_I2:                         kinds [i] = WJ_ENTRY_LD_I16_S; break;
		case MONO_TYPE_I4: case MONO_TYPE_U4:      kinds [i] = WJ_ENTRY_LD_I32; break;
		case MONO_TYPE_I8: case MONO_TYPE_U8:      kinds [i] = WJ_ENTRY_LD_I64; break;
		case MONO_TYPE_R4:                         kinds [i] = WJ_ENTRY_LD_F32; break;
		case MONO_TYPE_R8:                         kinds [i] = WJ_ENTRY_LD_F64; break;
		case MONO_TYPE_I: case MONO_TYPE_U:
		case MONO_TYPE_PTR: case MONO_TYPE_FNPTR:
		case MONO_TYPE_OBJECT: case MONO_TYPE_STRING: case MONO_TYPE_CLASS:
		case MONO_TYPE_SZARRAY: case MONO_TYPE_ARRAY:
			kinds [i] = WJ_ENTRY_LD_I32; break;
		default:
			return -1;   /* value types, generic instances, anything unclassified */
		}
		/* The load must produce exactly the type the callee declares, or the call_indirect traps. */
		if ((kinds [i] == WJ_ENTRY_LD_I64 && vtypes [i] != WASM_I64) ||
		    (kinds [i] == WJ_ENTRY_LD_F32 && vtypes [i] != WASM_F32) ||
		    (kinds [i] == WJ_ENTRY_LD_F64 && vtypes [i] != WASM_F64) ||
		    (kinds [i] <= WJ_ENTRY_LD_I32 && kinds [i] != WJ_ENTRY_LD_I64 && vtypes [i] != WASM_I32) ||
		    (kinds [i] >= WJ_ENTRY_LD_I8_S && kinds [i] <= WJ_ENTRY_LD_U16 && vtypes [i] != WASM_I32))
			return -1;
	}
	vtypes [nargs] = (guint8) ci.ret.wtype;   /* WASM_VOID for void */
	return nargs;
}
#endif


/*
 * MONO_WASM_JIT_AOT_ENTRY — hand AOT/native callers a direct call into our JITted body.
 *
 * interp_create_method_pointer_llvmonly hands AOT code a ftndesc whose target eventually reaches
 * mono_jiterp_interp_entry. That path ALREADY redirects to us (interp.c: if wasm_jit_slot > 0 it calls
 * mono_wasm_jit_invoke_caught -> the e-thunk -> our body), so for a method we have compiled every step
 * around the call is pure boundary overhead: InterpFrame setup, get_arg_offset_fast, stack-pointer
 * juggling, GC-unsafe transition, LMF push/pop, and stackval marshalling in and out. Measured at 7.6% of
 * steady-state time (mono_jiterp_interp_entry 5.99 + _prologue 1.16 + stackval_from_data 0.46).
 *
 * This builds a one-function adapter module forwarding (args..., extra) to the method's f-slot, and
 * returns its table slot. Callers install it as the ftndesc target.
 *
 * Restricted on purpose (see the gate below): the adapter's parameter list has to agree EXACTLY with how
 * mono lowered the same signature for AOT, or the call_indirect signature check traps.
 */


/* Verbose-only detail for signature bails. Keep this out of the aggregate counters: it deliberately
 * performs name/layout queries and is intended for short MONO_WASM_JIT_VERBOSE=2 diagnosis runs.
 * `arg` is -1 for the return, >=0 for an explicit parameter, and -2 when only the overall arity is
 * known. Printing both the raw wasm classification and each feature gate makes a "call ret type"
 * actionable without another instrumentation build. */
static void
wj_print_bail_sig (const char *site, MonoMethod *callee, MonoMethodSignature *sig, int arg)
{
	char *cn = callee ? mono_method_get_full_name (callee) : NULL;

	if (!sig) {
		printf (" [sig site=%s callee=%s signature=null]", site ? site : "?", cn ? cn : "<indirect>");
		g_free (cn);
		return;
	}
	if (arg < -1 || arg >= (int) sig->param_count) {
		printf (" [sig site=%s callee=%s part=arity hasthis=%d params=%d]", site ? site : "?",
			cn ? cn : "<indirect>", sig->hasthis ? 1 : 0, (int) sig->param_count);
		g_free (cn);
		return;
	}

	{
		MonoType *type = arg < 0 ? sig->ret : sig->params [arg];
		MonoType *ut = mini_get_underlying_type (type);
		MonoType *scalar_etype = NULL;
		MonoClass *klass = MONO_TYPE_ISSTRUCT (ut) ? mono_class_from_mono_type_internal (ut) : NULL;
		char *tn = mono_type_full_name (type);
		char *sen = NULL;
		gint32 byaddr_size = 0, byaddr_align = 0;
		guint32 layout_align = 0;
		int layout_size = klass ? mono_class_value_size (klass, &layout_align) : -1;
		gboolean scalar_candidate = mini_wasm_is_scalar_vtype (ut, &scalar_etype);
		gboolean byaddr_candidate = wj_byaddr_vtype (type, &byaddr_size, &byaddr_align);
		WasmValtype raw = wasm_valtype_of_type (type);
		WasmValtype scalar_wasm = scalar_etype ? wasm_valtype_of_type (scalar_etype) : 0;
		if (scalar_etype)
			sen = mono_type_full_name (scalar_etype);

		printf (" [sig site=%s callee=%s part=%s index=%d type=%s mono=%d underlying=%d raw_wasm=%d"
			" struct=%d gsharedvt=%d simd=%d refs=%d scalar_candidate=%d scalar_type=%s scalar_wasm=%d byaddr_candidate=%d"
			" size=%d align=%u byaddr_size=%d byaddr_align=%d]",
			site ? site : "?", cn ? cn : "<indirect>", arg < 0 ? "ret" : "arg", arg,
			tn ? tn : "?", (int) type->type, (int) ut->type, (int) raw,
			klass ? 1 : 0, mini_is_gsharedvt_variable_type (ut) ? 1 : 0,
			klass && m_class_is_simd_type (klass) ? 1 : 0,
			klass && (m_class_has_references (klass) || m_class_has_ref_fields (klass)) ? 1 : 0,
			scalar_candidate ? 1 : 0, sen ? sen : "-", (int) scalar_wasm, byaddr_candidate ? 1 : 0,
			layout_size, (unsigned) layout_align, (int) byaddr_size, (int) byaddr_align);
		g_free (sen);
		g_free (tn);
	}
	g_free (cn);
}

/* Emit a BYVAL ref-free scalar-vtype call arg onto the wasm stack. The ByVal value is addr-frame-backed
 * (LDADDR_VTYPE gave the source vtype temp a slot, and LOWER-VTYPE-OPTS lowered its vzero/field-store/
 * vmove to plain stores into that slot), so load the single field (offset 0) as the etype scalar — what
 * the AOT callee's LLVMArgWasmVtypeAsScalar param expects. Returns FALSE (the caller then bails the whole
 * method — safe) if the arg isn't a ref-free scalar vtype or the ByVal value has no addr-frame slot. */
static gboolean
wj_emit_scalar_vtype_arg (WasmBuf *body, WjCtx *c, MonoType *pt, int argvreg)
{
	WasmValtype ev;
	guint32 off;
	if (!wj_scalar_vtype_valtype (pt, &ev))
		return FALSE;
	if (!c->addrslot || argvreg < 0 || argvreg >= c->nvreg)
		return FALSE;
	if (c->addrslot [argvreg] == -2) {
		/* ref-etype scalar vtype: the single ref lives in a GC-scanned ref-shadow slot; load it (i32). */
		if (ev != WASM_I32 || !c->refslot || c->refslot [argvreg] < 0)
			return FALSE;
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->refbase);
		if (c->refslot [argvreg] != 0) { wasm_i32_const (body, c->refslot [argvreg] * 4); wasm_op (body, WASM_OP_I32_ADD); }
		wasm_op (body, WASM_OP_I32_LOAD); wasm_memarg (body, 2, 0);
		return TRUE;
	}
	if (c->addrslot [argvreg] < 0)
		return FALSE;
	off = (guint32) c->addrslot [argvreg];
	wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
	switch (ev) {
	case WASM_I32: wasm_op (body, WASM_OP_I32_LOAD); wasm_memarg (body, 2, off); return TRUE;
	case WASM_I64: wasm_op (body, WASM_OP_I64_LOAD); wasm_memarg (body, 3, off); return TRUE;
	case WASM_F32: wasm_op (body, WASM_OP_F32_LOAD); wasm_memarg (body, 2, off); return TRUE;
	case WASM_F64: wasm_op (body, WASM_OP_F64_LOAD); wasm_memarg (body, 3, off); return TRUE;
	default: return FALSE;
	}
}

/* Store an ArgVtypeAsScalar call result (currently on the wasm stack) through the return-temp
 * address captured at WjCallArgs position n. MINI represents every vtype call result as an addressable temp and
 * emits OP_VCALL2 with no scalar dreg, even when the native wasm ABI returns the single field directly. */
static gboolean
wj_store_scalar_vtype_result (WasmBuf *body, WjCtx *c, WasmValtype vt, int addr_vreg)
{
	int g = wasm_valtype_group (vt);
	int tmp;
	WasmOpcode sop;
	guint32 al;
	if (g < 0)
		return FALSE;
	/* i32 always has the ref-store scratch; wider groups have addr-frame scratch because the
	 * addressable vtype result temp itself requires an addr frame. */
	tmp = vt == WASM_I32 ? c->rtmp : c->addr_tmp [g];
	if (tmp < 0)
		return FALSE;
	switch (vt) {
	case WASM_I32: sop = WASM_OP_I32_STORE; al = 2; break;
	case WASM_I64: sop = WASM_OP_I64_STORE; al = 3; break;
	case WASM_F32: sop = WASM_OP_F32_STORE; al = 2; break;
	case WASM_F64: sop = WASM_OP_F64_STORE; al = 3; break;
	default: return FALSE;
	}
	wasm_op_local (body, WASM_OP_LOCAL_SET, (guint32) tmp);
	if (!wasm_ld (body, c, addr_vreg))
		return FALSE;
	wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) tmp);
	wasm_op (body, sop); wasm_memarg (body, al, 0);
	return TRUE;
}

/*
 * WjCallArgs — the wasm JIT's per-call-site argument descriptor, hung off the (otherwise unused for
 * wasm) call->call_info. Built by mono_wasm_emit_call at method-to-ir time.
 *
 * The emitter reads call arguments POSITIONALLY rather than from the call instruction's sregs: an
 * instruction has MONO_MAX_SRC_REGS of those and a managed signature has arbitrarily many arguments.
 * What each position stores is the DEFINING INSTRUCTION, and the vreg is read back out of it through
 * wj_arg_vreg at the moment of use.
 *
 * That indirection is the whole point of the struct. mono_wasm_emit_call runs long before codegen,
 * and the optimization pipeline in between rewrites vreg NUMBERS -- SSA renaming above all. A cached
 * number silently becomes some other value's vreg, which is a wrong ARGUMENT rather than a bail.
 * A MonoInst* is mempool-allocated and stable for the whole compile, and every pass that renumbers a
 * vreg does it by rewriting exactly the dreg field this reads, so the answer is always current and
 * nothing has to be resynchronized after the pipeline. (capvreg is a narrow, individually justified
 * fallback for one instruction-retiring transform -- see wj_arg_vreg -- not a second general record.)
 */
typedef struct {
	int nargs;             /* managed args including `this`; carrier[]/capvreg[] have nargs+1 entries */
	MonoInst **carrier;    /* [0..nargs-1] arg source; [nargs] the hidden-vret address, or NULL */
	int *capvreg;          /* the vreg each carrier defined AT CAPTURE TIME — see wj_arg_vreg */
	gint32 *copyoff;       /* [0..nargs-1] by-addr copy-region byte offset in the addr frame, -1 = none.
	                        * Filled by the emit-time pre-pass. Not a vreg, so it needs no indirection. */
} WjCallArgs;

/*
 * Current vreg of managed argument AI (AI == nargs is the hidden-vret address), or -1 if there is none.
 * Callers turn -1 into a clean bail rather than emitting an access to local -1.
 *
 * Normally this is just carrier->dreg, read live so that any pass which renumbers the vreg is followed
 * automatically. The capture-time number is the fallback for exactly one situation: mono_local_cprop's
 * reverse copy propagation rewrites `B <- FOO; A <- B` into `A <- FOO` by pointing FOO's dreg at A and
 * MONO_DELETE_INS-ing the carrier -- which NULLIFY_INSes it, leaving dreg == -1. The argument still
 * lands in A, because A is precisely the dreg the producer was retargeted to; only the instruction that
 * used to name it is gone. So falling back to the captured number is not a guess, it is the same value
 * by construction.
 *
 * Nothing else can retire a carrier: mono_local_deadce marks out_ireg_args vregs used, and both
 * mono_ssa_deadce and ssa_cprop's folding only touch defs of cfg->varinfo entries, which a carrier dreg
 * (fresh, singly-defined, non-variable) is not. If a future pass does retire one for another reason,
 * the fallback stops being sound -- so it is deliberately narrow rather than a general "trust the
 * snapshot" rule.
 */
static int
wj_arg_vreg (MonoCallInst *call, int ai)
{
	WjCallArgs *w = (WjCallArgs *) call->call_info;
	MonoInst *c;

	if (!w || ai < 0 || ai > w->nargs)
		return -1;
	c = w->carrier [ai];
	if (!c)
		return -1;
	return c->dreg >= 0 ? c->dreg : w->capvreg [ai];
}

/* Emit one managed-call arg (index ai over the managed args, incl. `this` at 0 when hasthis).
 * A BYVAL ref-free scalar-vtype param (MONO_WASM_JIT_VTYPE_SCALAR) is loaded as its single-field etype
 * scalar from the ByVal value's addr-frame slot. A by-addr vtype param (WJ_ARG_VTYPE_BYADDR) is
 * memory.copied from its source — the vtype's addr-frame slot, or the caller's own incoming by-addr
 * pointer param (sentinel -3) — into this call site's dedicated copy region, and the REGION's address
 * is the arg: the callee owns that copy (byval mutation safe), and re-emitting the copy inline before
 * every call makes loops re-copy per iteration. Everything else is a normal wasm_ld of the arg vreg.
 * Shared by the direct-JIT, inline-AOT and interp-residual arg loops so all three agree on the ABI
 * (the residual spills the pushed value — scalar or copy address — into its scratch slot; the interp
 * side derefs by-addr slots per mono_wasm_jit_arg_is_byaddr). */
static gboolean
wj_emit_one_call_arg (WasmBuf *body, WjCtx *c, const WasmCallInfo *ci, MonoMethodSignature *csig, MonoCallInst *call, int ai)
{
	int pidx = ai - (csig->hasthis ? 1 : 0);
	if (ci && ai >= 0 && ai < ci->nargs && ci->args [ai].kind == WJ_ARG_VTYPE_BYADDR) {
		WjCallArgs *w = (WjCallArgs *) call->call_info;
		int srcvreg = wj_arg_vreg (call, ai);
		gint32 copyoff = (w && ai < w->nargs) ? w->copyoff [ai] : -1;
		if (copyoff < 0 || srcvreg < 0 || srcvreg >= c->nvreg || !c->addrslot)
			return FALSE;
		/* dest: this call site's copy region */
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (body, copyoff);
		wasm_op (body, WASM_OP_I32_ADD);
		/* src: the vtype value's address */
		if (c->addrslot [srcvreg] >= 0) {
			wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
			if (c->addrslot [srcvreg]) { wasm_i32_const (body, c->addrslot [srcvreg]); wasm_op (body, WASM_OP_I32_ADD); }
		} else if (c->addrslot [srcvreg] == -3) {
			if (!wasm_ld (body, c, srcvreg))   /* our own incoming by-addr param holds the address */
				return FALSE;
		} else {
			return FALSE;   /* "vtype arg no addr slot": source vreg lost its addressable backing */
		}
		wasm_i32_const (body, ci->args [ai].vsize);
		wasm_u8 (body, 0xFC); wasm_uleb (body, 10); wasm_u8 (body, 0x00); wasm_u8 (body, 0x00);   /* memory.copy mem0<-mem0 (bulk memory) */
		/* the arg value = the copy's address */
		wasm_op_local (body, WASM_OP_LOCAL_GET, (guint32) c->addrbase);
		wasm_i32_const (body, copyoff);
		wasm_op (body, WASM_OP_I32_ADD);
		return TRUE;
	}
	if (pidx >= 0 && pidx < (int) csig->param_count) {
		WasmValtype sv;
		if (wj_scalar_vtype_valtype (csig->params [pidx], &sv))
			return wj_emit_scalar_vtype_arg (body, c, csig->params [pidx], wj_arg_vreg (call, ai));
	}
	return wasm_ld (body, c, wj_arg_vreg (call, ai));
}

/* IC probe load: i64.atomic.load = 0xfe 0x11 (align 3). Shared by the inline vcall and AOT-vcall cache
 * probes, which read a (vtab-tag, payload) pair packed into one i64 and rely on the load being atomic —
 * a torn read could match the tag while carrying another receiver type's payload. */
static void
wj_emit_ic_load64 (WasmBuf *b, guint32 off)
{
	wasm_op (b, WASM_OP_ATOMIC_PREFIX); wasm_u8 (b, 0x11); wasm_memarg (b, 3, off);
}

/* Fast-path volume profiling (MONO_WASM_JIT_PROFILE_FAST): emit an inline atomic increment of
 * mono_wasm_jit_counters[idx] into the JITted body. &counters[idx] is a link-time-constant address in the
 * runtime's linear memory (the emitter runs in the same process, same memory — just like the embedded C
 * function-pointer constants). i64.atomic.rmw.add (0xfe 0x26, align 3) keeps the count correct when worker
 * threads share the site; the returned old value is dropped. No-op unless profile_fast is set at emit time,
 * so normal STATS runs pay nothing. Only called from HOST_BROWSER fast-path sites; G_GNUC_UNUSED for the
 * cross-compiler build (which never reaches those sites). */

static G_GNUC_UNUSED void
wj_emit_fast_count (WasmBuf *body, int idx)
{
	extern int mono_wasm_jit_profile_fast;
	extern gint64 mono_wasm_jit_counters [];
	if (!mono_wasm_jit_profile_fast)
		return;
	wasm_i32_const (body, (gint32) (intptr_t) &mono_wasm_jit_counters [idx]);
	wasm_i64_const (body, 1);
	wasm_op (body, WASM_OP_ATOMIC_PREFIX); wasm_u8 (body, 0x1f); wasm_memarg (body, 3, 0);   /* i64.atomic.rmw.add (0xfe 0x1f; 0x26 is rmw.SUB!) */
	wasm_op (body, WASM_OP_DROP);
}

/* Max distinct call_indirect callee functypes per JITted method (T0/T1 are the 2 base types; these are
 * the extras). Raised 32->128: big hot methods (class_1309:method_6091 LivingEntity, class_922:method_4054
 * entity render) have >32 distinct call signatures and were bailing "too many callee types". wasm allows
 * arbitrarily many functypes, so this is just the on-stack array size (~128*sizeof(WasmFuncType)). */
#define WJ_EXTRA_TYPES_MAX 128

/* --- assembly: relocated member bodies -> one WebAssembly module ---------------------------------
 *
 * This is where every call in every body gets its FORM, and that is the entire point of the relocation
 * machinery: the decision is made here, from the module's own membership, and remade from scratch every
 * time the same bodies are framed again with different neighbours. Producing a batched body used to mean
 * running mini_method_compile once per member; it now means a memcpy pass.
 *
 * Cost, from scratchpad/wj/bytecost.mjs: 29.4 us fixed per module + 15.4 ns per wire byte. Co-location
 * therefore REDUCES total instantiate cost -- every batching regression ever measured here (-26.6% R61b,
 * -35.7% R96, -13.0% R105) was deferral, members running interpreted while their batch formed, not module
 * overhead.
 *
 * The three forms, and what V8 makes of each (R128, read out of turboshaft-graph-interface.cc:2697-2727):
 *   i32.const <fslot>; call_indirect   table bounds check, canonical-type check, index->code-pointer
 *                                      arithmetic, validity check, `call *`     ~15 x86, never inlined
 *   call <import>                      three loads from WasmDispatchTableForImports + `call *`
 *                                                                                ~5 x86, never inlined
 *   call <local funcidx>               `call rel32`                              1 x86, and the ONLY
 *                                                                                form V8 will inline
 */
/*
 * Everything one method contributes to a module, with nothing about the module baked in. This is the unit
 * that gets stored (WjRegEntry.body) and re-framed.
 */
typedef struct _WjBody {
	MonoMethod    *method;
	/* FULL NAME, RESOLVED AT EMIT TIME AND OWNED HERE. Not derived later from `method`, and that is a
	 * correctness requirement rather than a cache.
	 *
	 * `mono_method_get_full_name` is a METADATA operation: it walks the signature through
	 * mono_signature_get_desc -> mono_type_get_desc and lazily resolves parameter types, which wants the
	 * loader lock and a coop-safe thread state. Calling it during ASSEMBLY -- on a worker, inside the
	 * compile section, for members other threads compiled -- faults, and the captured trace is exactly
	 * those frames under wj_assemble (R199, intermittent `memory access out of bounds` at ~33% of runs;
	 * co-location merging surfaces it by raising the number of names resolved per assembly).
	 *
	 * This is the THIRD subsystem to hit it and the two that got it right did the same thing: see
	 * `wj_block_name` ("resolved at record time on a coop thread") and `wj_entry_edges` ("caches full
	 * method names at record time so the main-thread JS dump can read it without taking a lock").
	 * Resolve on the thread that compiled the method, where the metadata is necessarily live -- the
	 * emitter already does exactly that for its own diagnostics (`mname`, and the comment there notes
	 * metadata is stable for the compile lifetime).
	 *
	 * Keeping the FULL name rather than a cheap `Klass:name` label is also deliberate: hotinsn.py's
	 * kind_of() classifies a symbol as OURS only when it looks like a mono full name (a single colon AND
	 * a space), so a shortened label would silently reclassify the whole tier as `aot`. */
	char          *name;
	WasmValtype    param_types [WASM_FUNCTYPE_MAX_PARAMS];
	guint32        nparams;
	WasmValtype    ret_type;
	WasmLocalGroup groups [4];
	WasmBuf        f_body;        /* instruction bytes + relocations; owned */
	WasmBuf        e_body;
	WasmFuncType  *extra_types;   /* owned, nextra entries */
	guint32        nextra;
	guint8         uses_calls;
	guint8         uses_eh_tag;
	int            eh_tpool;
} WjBody;

static int
wj_body_nparams (struct _WjBody *b)
{
	return b ? (int) b->nparams : -1;
}

static void
wj_body_free (WjBody *b)
{
	if (!b)
		return;
	wasm_buf_free (&b->f_body);
	wasm_buf_free (&b->e_body);
	g_free (b->extra_types);
	g_free (b->name);
	g_free (b);
}

/* Take ownership of the two body buffers (the caller's are left zeroed) and copy the small side tables. */
static WjBody *
wj_body_take (MonoMethod *method, const char *name, WasmBuf *f, WasmBuf *e,
              const WasmValtype *params, guint32 nparams, WasmValtype ret,
              const WasmLocalGroup *groups, const WasmFuncType *extras, guint32 nextra,
              gboolean uses_calls, gboolean uses_eh_tag, int eh_tpool)
{
	WjBody *b = g_new0 (WjBody, 1);
	g_assert (nparams <= WASM_FUNCTYPE_MAX_PARAMS);
	b->method = method;
	/* Copied, not borrowed: the caller's string is a compile-scoped local. */
	b->name = name ? g_strdup (name) : NULL;
	memcpy (b->param_types, params, sizeof (WasmValtype) * nparams);
	b->nparams = nparams;
	b->ret_type = ret;
	memcpy (b->groups, groups, sizeof (WasmLocalGroup) * 4);
	b->f_body = *f; memset (f, 0, sizeof (*f));
	b->e_body = *e; memset (e, 0, sizeof (*e));
	if (nextra) {
		b->extra_types = (WasmFuncType *) g_malloc (sizeof (WasmFuncType) * (gsize) nextra);
		memcpy (b->extra_types, extras, sizeof (WasmFuncType) * (gsize) nextra);
	}
	b->nextra = nextra;
	b->uses_calls = uses_calls ? 1 : 0;
	b->uses_eh_tag = uses_eh_tag ? 1 : 0;
	b->eh_tpool = eh_tpool;
	return b;
}

typedef struct {
	MonoMethod           *method;         /* for co-location matching (identity only -- never dereferenced here) */
	/* The member's full name, resolved by whoever COMPILED it and owned by its WjBody. wj_assemble must
	 * not derive this from `method`: see WjBody.name for why that faults. NULL is tolerated and yields
	 * the "wasmjit" placeholder. */
	const char           *name;
	int                   f_slot;         /* this member's own f-slot: the target of the cycle test */
	WasmBuf              *f_body;         /* code with holes; not modified */
	WasmBuf              *e_body;
	/* The member's type block is (its own signature, its thunk's signature, its callee types). The first
	 * two are given as their parts rather than as WasmFuncTypes because that is how the emitter already
	 * has them; wj_assemble materialises the contiguous block. */
	const WasmValtype    *param_types;
	guint32               nparams;
	WasmValtype           ret_type;
	const WasmFuncType   *extra_types;
	guint32               nextra;
	const WasmLocalGroup *groups;
	guint32               nlocal_groups;
	guint8                uses_calls;
	guint8                uses_eh_tag;
	int                   eh_tpool;       /* pool index of the tag's (i32)->void type, or -1 */
} WjAsmMember;

typedef struct {
	/* MONO_WASM_JIT_DIRECT_IMPORT: a JITted callee may become an import. Soundness is decided PER EDGE by
	 * wj_asm_method_importable, not by this flag -- see wj_asm_reaches for the cycle argument. */
	guint8 local_calls;      /* resolve a co-located callee to `call <funcidx>` */
	guint8 names;            /* MONO_WASM_JIT_NAMES: append the name section */
	int    max_fimports;         /* WJ_HELPER_IMPORT_CAP */
} WjAsmPolicy;

/* Per-member census of what the forms came out as; feeds the WJC_ counters and the tier dumps. */
typedef struct { int nlocal, nimport, nindirect, nimport_method; } WjAsmStats;

/* The f-slots one member still reaches by CALL_INDIRECT or by IMPORT after framing -- i.e. its real
 * dependency set for THIS module, as opposed to the emit-time set on the registry entry.
 *
 * This exists because mono_wasm_jit_rebatch has no capture phase. mono_wasm_jit_batch_bind republishes
 * re->imports unconditionally but copies re->deps only inside `if (bm)`, and `bm` is NULL on every rebatch,
 * so a co-located member kept demanding admission of exactly the callees co-location had just turned into
 * module-local `call <funcidx>` -- calls that need no admission at all, because they are bytes in the same
 * module. MEASURED consequence, one in-world run 2026-08-29: BATCH_ADMIT_FAIL 1966, ADMIT_DEFER_GIVEUP 1540
 * against 3 deferrals in the control, and ~1540 methods permanently denied the JIT tier.
 *
 * A LOCAL call contributes nothing here. That is the whole point, and it is a strict reduction of the
 * admission closure with no soundness cost.
 *
 * The signature hash is carried because wj_admit_dependencies checks `dep->f_sig_id != re->dep_sig[i]`, and
 * the MonoMethod because WjRegEntry.dep_method feeds the diagnostics. */
typedef struct { int *slot; guint32 *sig; MonoMethod **method; int n, cap; } WjAsmDeps;

static void
wj_asm_deps_add (WjAsmDeps *dp, int fslot, guint32 sig, MonoMethod *m)
{
	int i;
	if (!dp || fslot <= 0)
		return;
	for (i = 0; i < dp->n; ++i)
		if (dp->slot [i] == fslot)
			return;   /* deduped on slot, like wj_result_add_direct_dep */
	if (dp->n == dp->cap) {
		dp->cap = dp->cap ? dp->cap * 2 : 8;
		dp->slot = (int *) g_realloc (dp->slot, sizeof (int) * (gsize) dp->cap);
		dp->sig = (guint32 *) g_realloc (dp->sig, sizeof (guint32) * (gsize) dp->cap);
		dp->method = (MonoMethod **) g_realloc (dp->method, sizeof (MonoMethod *) * (gsize) dp->cap);
	}
	dp->slot [dp->n] = fslot;
	dp->sig [dp->n] = sig;
	dp->method [dp->n] = m;
	dp->n++;
}

static void
wj_asm_deps_free (WjAsmDeps *dp)
{
	if (!dp)
		return;
	g_free (dp->slot); g_free (dp->sig); g_free (dp->method);
	memset (dp, 0, sizeof (*dp));
}

/* Intern one function import. Slot i IS function index i -- function imports are the only kind-0x00
 * imports -- which is what lets a body name a slot long before the final count is known.
 *
 * The dedup key is (table index, MODULE type index), not (table index, functype): two members calling the
 * same helper with the same signature still get two imports, because each member's type block holds its
 * own copy of that functype and a wasm import names one specific type index. Duplicative but correct, and
 * removing it means deduping the type section, which is a separate change with its own byte-identity cost. */
static int
wj_asm_intern_import (WasmFuncImport *himp, int *nhimp, int cap, guint32 table_index, guint32 ti,
                      gboolean is_method)
{
	int i;
	for (i = 0; i < *nhimp; ++i)
		if (himp [i].table_index == table_index && himp [i].type_idx == ti)
			return i;
	if (*nhimp >= cap || *nhimp >= WJ_MAX_HELPER_IMPORTS)
		return -1;      /* cap reached: the site stays a call_indirect, exactly as before */
	himp [*nhimp].table_index = table_index;
	himp [*nhimp].type_idx = ti;
	/* Not part of the dedup key: a table index is either a JIT f-slot or a C function pointer, never
	 * both, so it is a function OF the key rather than a component of it. */
	himp [*nhimp].method = is_method ? 1 : 0;
	return (*nhimp)++;
}

/* --- a BEST-EFFORT filter on import edges, not the guarantee ---------------------------------------
 *
 * READ THIS FIRST: what makes imports safe is the admission-time check in wj_admit_dependencies, not this.
 * The first version of this work put the whole guarantee here and it does not work, because the question
 * "will the callee be instantiated before me" cannot be answered when the module is EMITTED. The answer has
 * to hold at ADMISSION, which happens later and separately on every worker, and by then the graph has
 * grown: a re-emit registers a new descriptor into the same f-slot with a new dependency list, so the
 * callee behind an imported slot can gain an edge back to its importer after the import was already baked
 * in. Measured: with only this check in place, direct_import still produced LinkErrors with the Round 139
 * fingerprint intact -- one callee failing for four separate importers.
 *
 * It is kept because it is cheap and it is RIGHT about the cycles that are already visible, which is most
 * of them (SCC members are co-registered, so they see each other), and every edge it refuses is a method
 * that does not have to be deferred and retried at run time. It is a filter. Do not add to it in the
 * belief that enough of it would make admission's check unnecessary.
 *
 * An import binds wasmTable.get(fslot) at INSTANTIATION, so the callee's module has to be instantiated on
 * this worker before the caller's is. mono_wasm_jit_admit guarantees that by admitting a descriptor's
 * direct-dependency closure first -- but it is a DFS, and on a CYCLE it must stop somewhere: whichever
 * member it reaches while an ancestor is still `visiting` returns without instantiating, so the ancestor
 * instantiates FIRST. If that ancestor imported it, the import resolves against a slot still holding the
 * guarded interp-entry trampoline, a real function of a different type -- which is exactly why Round 139
 * saw "imported function does not match the expected type" rather than "is not a function", 14 LinkErrors
 * over 9 slots and a crash before titleReady in 2/2 arms.
 *
 * The rule that fixes it, per EDGE rather than per module:
 *
 *     A -> B may be an import IFF B cannot reach A.
 *
 * Why that is sufficient, against the admission DFS as written: when the DFS is about to instantiate u it
 * has already called mono_wasm_jit_admit on every dep of u. For an IMPORT dep v that call either returned
 * with v instantiated, or returned early on `wj_desc_state [v] == 1` -- v is an ancestor still on the DFS
 * stack, which means a path v -> ... -> u exists, which means v reaches u, which the rule forbids. So the
 * early return cannot happen on an import edge and v is always installed before u needs it. An edge inside
 * a cycle simply stays a call_indirect, which is immune because the table is patched after publication
 * instead of bound at instantiation.
 *
 * Note that the DFS can be ENTERED at either end of a cycle, so "which one loses the race" is not fixed --
 * which is why the test has to be on mutual reachability and not on anything about ordering.
 *
 * Note what this does NOT need: co-location. R139 concluded the fix was "emit an entire cycle into a single
 * module", which is true but needs the whole batching machinery; refusing the cycle edges is sound on its
 * own and costs only those edges.
 *
 * The graph walked here is the same one admission walks -- WjRegEntry.deps, f-slots -- so the two agree by
 * construction. A dep whose callee is not registered cannot be imported anyway (the ABI check below
 * rejects it), so an unresolvable f-slot ends the walk rather than widening it.
 *
 * The TARGET is identified by f-slot rather than by descriptor, because at the moment a lone method is
 * framed it HAS an f-slot (reserved before emission, which is what lets its callees bake it) but is not
 * registered yet and so has no descriptor at all. Matching on the slot is also what the admission walk
 * itself matches on.
 */
#define WJ_REACH_MAX 512   /* descriptors visited before giving up; giving up means REACHABLE, i.e. no import */

#ifdef HOST_BROWSER

static __thread guint32 *wj_reach_stamp;   /* per registry index; == wj_reach_gen means "seen this walk" */
static __thread int     *wj_reach_stack;
static __thread int      wj_reach_cap;
static __thread guint32  wj_reach_gen;

/* Can `from_desc` reach the method occupying `target_fslot`? Conservative: TRUE on any doubt. */
static gboolean
wj_asm_reaches (int from_desc, int target_fslot)
{
	int sp = 0, visited = 0, n = wj_reg_n;

	if (from_desc <= 0)
		return TRUE;      /* the callee is not registered; nothing to walk, so assume the worst */
	if (target_fslot <= 0) {
		/* The caller has no f-slot yet: it is a first emit whose pair is allocated after framing. A slot
		 * that does not exist cannot appear in any registry entry's dependency list, so nothing can reach
		 * it and it cannot be in a cycle. (A method that DOES already own or have reserved a slot -- a
		 * re-emit, a self-recursion reservation, an SCC batch reservation, or a pair parked by an aborted
		 * batch -- passes that slot in, precisely because those can have in-edges.) */
		return FALSE;
	}
	if (n > wj_reach_cap) {
		int ncap = wj_reach_cap ? wj_reach_cap : 1024;
		while (ncap < n) ncap *= 2;
		wj_reach_stamp = (guint32 *) g_realloc (wj_reach_stamp, sizeof (guint32) * (gsize) ncap);
		memset (wj_reach_stamp + wj_reach_cap, 0, sizeof (guint32) * (gsize) (ncap - wj_reach_cap));
		wj_reach_stack = (int *) g_realloc (wj_reach_stack, sizeof (int) * (gsize) ncap);
		wj_reach_cap = ncap;
	}
	/* Stamped rather than cleared: clearing a 24k-entry array per call site would cost more than the walk. */
	if (++wj_reach_gen == 0) {
		memset (wj_reach_stamp, 0, sizeof (guint32) * (gsize) wj_reach_cap);
		wj_reach_gen = 1;
	}
	wj_reach_stack [sp++] = from_desc;
	wj_reach_stamp [from_desc - 1] = wj_reach_gen;
	while (sp > 0) {
		WjRegEntry *re = wj_reg_at (wj_reach_stack [--sp] - 1);
		int i;
		if (!re)
			continue;
		if (++visited > WJ_REACH_MAX)
			return TRUE;   /* too big to settle: assume the worst and leave the edge indirect */
		WjDepSet *ds = re->depset;
		for (i = 0; ds && i < ds->n; ++i) {
			int d;
			if (ds->slot [i] == target_fslot)
				return TRUE;
			d = wj_desc_for_fslot (ds->slot [i]);
			if (d <= 0 || d > wj_reg_n)
				continue;
			if (wj_reach_stamp [d - 1] == wj_reach_gen)
				continue;
			wj_reach_stamp [d - 1] = wj_reach_gen;
			if (sp < wj_reach_cap)
				wj_reach_stack [sp++] = d;
			else
				return TRUE;
		}
	}
	return FALSE;
}
#else
/* The offline cross-compiler has no registry to walk and imports nothing anyway. */
static gboolean
wj_asm_reaches (int from_desc, int target_fslot)
{
	(void) from_desc; (void) target_fslot;
	return TRUE;
}
#endif

/* Which member, if any, holds `callee`? -1 when it is not co-located. Linear because a module holds tens
 * of members, not thousands, and this runs once per call site at framing time. */
/* R206: may this candidate be dispatched through the call site's functype?
 *
 * Both guarded arms and the IC tail share ONE functype index and ONE argument-marshalling sequence, so a
 * candidate whose lowered signature differs is not substitutable: call_indirect's canonical-type check
 * turns that into a trap at whatever frame reaches it first, not a missed optimisation. Mirrors the test
 * the first arm already applies to its own predicted target. */
static gboolean
wj_arm_abi_ok (MonoMethod *target, const WasmFuncType *want)
{
	MonoMethodSignature *sig;
	WasmCallInfo ci;

	if (!target || !want)
		return FALSE;
	sig = mono_method_signature_internal (target);
	if (!sig)
		return FALSE;
	mono_wasm_get_call_info (sig, &ci);
	if (!ci.valid) {
		wj_count (WJC_ARM_ABI_INVALID);
		return FALSE;
	}
	if (ci.vret_byaddr) {
		wj_count (WJC_ARM_ABI_BYADDR);
		return FALSE;
	}
	if (!functype_eq (&ci.ftype, want)) {
		wj_count (WJC_ARM_ABI_SHAPE);
		return FALSE;
	}
	return TRUE;
}

static int
wj_asm_member_of (const WjAsmMember *m, int n, MonoMethod *callee)
{
	int i;
	if (!callee)
		return -1;
	for (i = 0; i < n; ++i)
		if (m [i].method == callee)
			return i;
	return -1;
}

/* Fill one WjAsmMember from a stored relocatable body. The re-framing path and the shadow path build
 * members from exactly the same fields, and they drifted apart once already. */
static void
wj_member_from_body (WjAsmMember *m, WjBody *b, int f_slot)
{
	memset (m, 0, sizeof (*m));
	m->method        = b->method;
	m->name          = b->name;   /* borrowed; resolved at that body's emit time (see WjBody.name) */
	m->f_slot        = f_slot;
	m->f_body        = &b->f_body;
	m->e_body        = &b->e_body;
	m->param_types   = b->param_types;
	m->nparams       = b->nparams;
	m->ret_type      = b->ret_type;
	m->extra_types   = b->extra_types;
	m->nextra        = b->nextra;
	m->groups        = b->groups;
	m->nlocal_groups = 4;
	m->uses_calls    = b->uses_calls;
	m->uses_eh_tag   = b->uses_eh_tag;
	m->eh_tpool      = b->eh_tpool;
}

/* wj_shadow_candidate, WjShadowCand and wj_collect_shadows lived here -- the eligibility test, the
 * sites/bytes ranking pass (R201: a shadow costs its body once per module and repays once per SITE, so
 * ranking beat encounter order at 63.7% vs 62.2% arm-local with 2.8% FEWER bodies) and the per-module
 * byte budget. Deleted with the feature; see the note at the knobs for the +30.5% wire bytes / +62.1%
 * functions it cost and for the fact that its benefit was never once timed in ~35 rounds.
 *
 * wj_verify_module_exports went with them. It scanned the assembled module's export section to prove a
 * shadowed module really carried the exports the instantiate path asks for, and re-framed plain if it
 * did not -- a floor added after R167 shipped shadows without one and a single malformed module retried
 * instantiation 959,405 times (Table.set of an undefined export), hanging the run at 0.02 fps. Shadows
 * were the only producer of `nexport < nmembers`, so nothing reaches that layout now. If per-class
 * grouping reintroduces unexported members, bring this check back WITH them: "an optimisation whose
 * failure is catastrophic rather than merely absent needs a floor under it" is the durable part. */

/* Resolve one body's holes. Called twice per member (method body, then entry thunk) in exactly the order
 * the emitter produced them, because import slots are assigned on first use and that order is what makes a
 * single-member module byte-identical to what the pre-relocation emitter emitted. */
/* Of the three FIXED-INDEX reloc kinds, which may become a declared import rather than a constant-index
 * `call_indirect`? This is the ONLY behavioural difference between them; everything else about the three
 * is population labelling (a C runtime helper, an AOT body, or the never-converted remainder -- the
 * runtime JIT-icall and the residual throw continuation). Stated once, here, so the resolve switch does
 * not carry three near-identical arms that invite exactly the silent merge R121 got caught doing. */
static gboolean
wj_reloc_fixed_importable (WasmRelocKind k)
{
	return k == WASM_RELOC_HELPER || k == WASM_RELOC_AOT;
}

static void
wj_asm_resolve_body (const WasmBuf *b, WasmRelocFix *fix, guint32 ti_base, int self_index,
                     const WjAsmMember *mem, int n, const WasmFuncType *self_types,
                     const WjAsmPolicy *pol, WasmFuncImport *himp, int *nhimp, WjAsmStats *st,
                     WjAsmDeps *dp)
{
	const WasmRelocs *rl = b->relocs;
	guint32 k;

	for (k = 0; rl && k < rl->n; ++k) {
		const WasmReloc *r = &rl->r [k];
		guint32 ti = ti_base + r->tpool;
		int slot = -1, member;

		fix [k].form = WASM_FORM_INDIRECT;
		fix [k].idx = 0;
		switch ((WasmRelocKind) r->kind) {
		case WASM_RELOC_TYPE_U:
		case WASM_RELOC_TYPE_S:
		case WASM_RELOC_INDIRECT:
			continue;   /* a type index only; there is no form to choose */
		case WASM_RELOC_SELF:
			/* The entry thunk calling its own method. They are framed together by construction, so this
			 * is always module-local. The member index is turned into a function index below.
			 *
			 * NOT counted in the census: there is exactly one per member no matter what, so counting it
			 * would add a constant ~24k to the LOCAL column and drown the thing the census exists to
			 * show, which is what the CALL SITES IN METHOD BODIES resolved to. */
			fix [k].form = WASM_FORM_LOCAL;
			fix [k].idx = (guint32) self_index;
			continue;
		case WASM_RELOC_HELPER:
		case WASM_RELOC_AOT:
		case WASM_RELOC_HELPER_CI:
			/* ONE FIXED FUNCTION-TABLE INDEX. All three encode identically; they differ in exactly one
			 * bit -- whether the site may become a declared import -- which wj_reloc_fixed_importable
			 * answers, plus which population they belong to for the census.
			 *
			 * MONO_WASM_JIT_HELPER_IMPORTS and MONO_WASM_JIT_AOT_IMPORTS gated the importable two and
			 * both shipped 1. The losing arm kept those calls as `i32.const <table index>;
			 * call_indirect`, which pays a table bounds check, a canonical-type check and index
			 * arithmetic a declared import does not -- V8 does NOT fold a constant call_indirect index
			 * into a direct call. Both forms still end in an indirect branch (R158), so this is worth a
			 * few instructions per call, not a call-form change. Settled.
			 *
			 * DO NOT COLLAPSE THE KINDS THEMSELVES. R121 folded HELPER_CI in with HELPER and silently
			 * converted it; the tier-shape gate caught 14,583 of 22,547 common methods changing
			 * structure in a refactor that was supposed to change none. The kinds are the population
			 * labels and the predicate is the behaviour -- keeping them separate is what makes this
			 * arm's unification provably inert. */
#ifdef HOST_BROWSER
			/* A FIXED INDEX THAT IS A JIT SLOT (R291). These kinds are supposed to name C helpers and AOT
			 * bodies, which need no admission -- so nothing declares them, and if the index is in fact a
			 * JITted method's f-slot, admission can never install it and the first worker that reaches the
			 * site without it calls the prefilled placeholder. That is R285's trap exactly (`i32.const
			 * 467053; call_indirect`, owned by a batched descriptor and declared by nobody, and R287
			 * measured the emit-time recorder complete). Declare it here, whatever produced it, and keep
			 * it indirect: an import would bind the slot's CURRENT function by value at instantiation and
			 * go stale the moment the descriptor is republished. The census names the population. */
			{
				int jkind = -1, jdesc = wj_fixed_index_jit_desc ((int) r->table_index, &jkind);
				if (G_UNLIKELY (jdesc > 0)) {
					WjRegEntry *jre = wj_reg_at (jdesc - 1);
					if (jkind == 0) {
						wj_asm_deps_add (dp, (int) r->table_index, wj_functype_hash (&self_types [r->tpool]),
						                 jre ? jre->logical_method : NULL);
						mono_wasm_jit_counters [WJC_FIXED_JIT_DEP]++;
					} else {
						mono_wasm_jit_counters [WJC_FIXED_JIT_ESLOT]++;   /* not expressible as a dep: counted */
					}
					if (mono_wasm_jit_verbose)
						printf ("WASM_JIT_FIXED_JIT_SLOT kind=%d reloc=%d index=%d desc=%d\n",
							jkind, (int) r->kind, (int) r->table_index, jdesc);
					st->nindirect++;
					continue;
				}
			}
#endif
			if (wj_reloc_fixed_importable ((WasmRelocKind) r->kind))
				slot = wj_asm_intern_import (himp, nhimp, pol->max_fimports, r->table_index, ti, FALSE);
			break;
		case WASM_RELOC_CALL:
			/* A JITted callee is either CO-LOCATED -- a module-local `call <funcidx>`, one x86
			 * instruction, no indirect branch, and the only form V8 will inline through -- or it is
			 * `call_indirect`. There is no third form any more.
			 *
			 * METHOD IMPORTS ARE GONE, and the measurements say they cost nothing to lose. R157: the
			 * conversion reached 100% of devirt predicted arms (6,892/6,909). R158: it does not matter,
			 * because V8 lowers `call <import>` through BuildImportedFunctionTargetAndImplicitArg into
			 * BuildWasmCall(..., kWasmIndirectFunction) -- the same path call_indirect takes -- so it
			 * still ends in an indirect `call *`; what it saves is a bounds check and a canonical-type
			 * check, perfectly predicted scalar work. R159, clean interleaved A/B: +0.7% fps at +-8.7%.
			 *
			 * What they cost was the whole admission-ordering problem. An import binds at INSTANTIATION
			 * and so must be ordered; call_indirect reads the table when it runs and need not be. The
			 * strict import list was the only source of the deferral path, and the deferral path was the
			 * only way a method could be permanently denied the JIT tier (R160-R162). */
			member = pol->local_calls ? wj_asm_member_of (mem, n, (MonoMethod *) r->sym) : -1;
			/* Self-recursion keeps the form it has always had unless co-location explicitly asked for
			 * module-local calls. */
			if (member == self_index && !pol->local_calls)
				member = -1;
			if (member >= 0) {
				fix [k].form = WASM_FORM_LOCAL;
				fix [k].idx = (guint32) member;
				st->nlocal++;
				continue;   /* module-local: no dependency, nothing to admit */
			}
			wj_asm_deps_add (dp, (int) r->table_index, wj_functype_hash (&self_types [r->tpool]),
			                 (MonoMethod *) r->sym);
			st->nindirect++;
			continue;
		}
		if (slot >= 0) {
			fix [k].form = WASM_FORM_IMPORT;
			fix [k].idx = (guint32) slot;
			st->nimport++;
		} else {
			st->nindirect++;
		}
	}
}

/*
 * The knob set, read once per assembly.
 *
 * `local_calls` says a co-located callee may become `call <funcidx>`; it is off for a lone method so that
 * a self-recursive call keeps the shape it has always had, and on for a real batch, which is where
 * co-location is the entire point. Whether a JITted callee may be IMPORTED is not a policy question at all
 * -- it is decided per edge by wj_asm_method_importable, because soundness depends on that specific
 * caller/callee pair and not on the module.
 *
 * Under the offline cross-compiler there is no function table and no instantiation, so nothing may become
 * an import: the dump has to be a self-contained module the encoder can be validated against.
 */
static void
wj_asm_policy_init (WjAsmPolicy *pol, gboolean local_calls)
{
	extern int mono_wasm_jit_names;

	memset (pol, 0, sizeof (*pol));
	pol->max_fimports = WJ_HELPER_IMPORT_CAP;
	pol->names = mono_wasm_jit_names ? 1 : 0;
	pol->local_calls = local_calls ? 1 : 0;
#ifdef HOST_BROWSER
#endif
}

/*
 * Frame `n` members into one module.
 *
 * `out` receives the module bytes. Returns FALSE only on an internal inconsistency; running out of import
 * slots is not a failure, it just leaves those sites as call_indirect.
 */
static gboolean
wj_assemble (const WjAsmMember *mem, int n, int nexport, const WjAsmPolicy *pol, WasmBuf *out,
             WjAsmStats *out_st, WjAsmDeps *out_deps)
{
	static const WasmValtype entry_params [2] = { WASM_I32, WASM_I32 };
	WasmFuncImport *himp;
	WasmAsmMember *am;
	WasmFuncType **types;
	WasmBuf *bodies;
	WasmRelocFix **fixes;
	WjAsmStats st;
	guint32 *ti_base;
	gboolean import_table = FALSE, import_eh_tag = FALSE;
	guint32 eh_type_idx = 0;
	int nhimp = 0, i;

	if (n <= 0 || nexport <= 0 || nexport > n)
		return FALSE;
	memset (&st, 0, sizeof (st));
	himp = g_new0 (WasmFuncImport, WJ_MAX_HELPER_IMPORTS);
	ti_base = g_new0 (guint32, n);
	fixes = g_new0 (WasmRelocFix *, 2 * n);
	bodies = g_new0 (WasmBuf, 2 * n);
	types = g_new0 (WasmFuncType *, n);
	am = g_new0 (WasmAsmMember, n);

	for (i = 0; i < n; ++i) {
		guint32 j, nt = 2 + mem [i].nextra;
		g_assert (mem [i].nparams <= WASM_FUNCTYPE_MAX_PARAMS);
		ti_base [i] = i ? ti_base [i - 1] + 2 + mem [i - 1].nextra : 0;
		types [i] = g_new0 (WasmFuncType, nt);
		types [i][0].nparams = mem [i].nparams;
		for (j = 0; j < mem [i].nparams; ++j)
			types [i][0].params [j] = mem [i].param_types [j];
		types [i][0].ret = mem [i].ret_type;
		types [i][1].nparams = 2;
		types [i][1].params [0] = entry_params [0];
		types [i][1].params [1] = entry_params [1];
		types [i][1].ret = WASM_VOID;
		for (j = 0; j < mem [i].nextra; ++j)
			types [i][2 + j] = mem [i].extra_types [j];
		if (mem [i].uses_calls)
			import_table = TRUE;
		if (mem [i].uses_eh_tag && !import_eh_tag) {
			import_eh_tag = TRUE;
			/* The tag's functype is a module type index like any other, so it moves with the member's
			 * block. First EH-using member wins; every other one's (i32)->void is structurally identical.
			 * A member that wants the tag without having interned the type is not expected, but falling
			 * back to its block's first entry keeps the module well-formed instead of aborting a compile
			 * -- which is what the pre-relocation framer did with its `eh_type_idx < 0 ? 0` guard. */
			eh_type_idx = ti_base [i] + (mem [i].eh_tpool >= 0 ? (guint32) mem [i].eh_tpool : 0);
		}
	}

	/* Pass 1 -- decide every form, assigning import slots in first-use order. LOCAL results hold a MEMBER
	 * index at this point, because the function index they need is nfimports + member and nfimports is not
	 * final until the last hole has been looked at. */
	for (i = 0; i < n; ++i) {
		const WasmRelocs *fr = mem [i].f_body->relocs, *er = mem [i].e_body->relocs;
		fixes [2 * i] = g_new0 (WasmRelocFix, fr && fr->n ? fr->n : 1);
		fixes [2 * i + 1] = g_new0 (WasmRelocFix, er && er->n ? er->n : 1);
		wj_asm_resolve_body (mem [i].f_body, fixes [2 * i], ti_base [i], i, mem, n, types [i], pol, himp, &nhimp, &st,
		                     out_deps ? &out_deps [i] : NULL);
		wj_asm_resolve_body (mem [i].e_body, fixes [2 * i + 1], ti_base [i], i, mem, n, types [i], pol, himp, &nhimp, &st,
		                     out_deps ? &out_deps [i] : NULL);
	}

	/* Pass 2 -- imports are counted, so a member index becomes a function index, and the bodies can be
	 * written out. Defined functions start at nhimp and the methods precede the thunks. */
	for (i = 0; i < 2 * n; ++i) {
		const WasmBuf *src = (i & 1) ? mem [i / 2].e_body : mem [i / 2].f_body;
		const WasmRelocs *rl = src->relocs;
		guint32 k;
		for (k = 0; rl && k < rl->n; ++k)
			if (fixes [i][k].form == WASM_FORM_LOCAL)
				fixes [i][k].idx += (guint32) nhimp;
		wasm_buf_init (&bodies [i]);
		wasm_body_serialize (src, fixes [i], ti_base [i / 2], &bodies [i]);
	}

	for (i = 0; i < n; ++i) {
		am [i].locals = mem [i].groups;
		am [i].nlocal_groups = mem [i].nlocal_groups;
		am [i].f_body = &bodies [2 * i];
		am [i].e_body = &bodies [2 * i + 1];
		am [i].types = types [i];
		am [i].ntypes = 2 + mem [i].nextra;
	}
	wasm_module_assemble (am, (guint32) n, (guint32) nexport, import_table, import_eh_tag, eh_type_idx,
	                      himp, (guint32) nhimp, out);
	if (G_UNLIKELY (mono_wasm_jit_stats)) {
		mono_wasm_jit_add (WJC_CALL_LOCAL, st.nlocal);
		mono_wasm_jit_add (WJC_CALL_IMPORT, st.nimport);
		mono_wasm_jit_add (WJC_CALL_INDIRECT, st.nindirect);
	}

	/* Symbolise. Without this every member is wasm-function[N] to V8, so perf attributes all JITted time to
	 * one anonymous bucket -- which once moved ~45% of in-game samples into 4,250 unnamed symbols and made
	 * whole method families read as 0% of the profile. */
	if (pol->names) {
		char **names = g_new0 (char *, n);
		char modname [256];
		/* USE THE CACHED NAME. Calling mono_method_get_full_name here is what faults (R199): it walks
		 * the signature and lazily resolves parameter types, on a worker, inside the compile section,
		 * for members other threads compiled. The name was resolved by whoever compiled each member --
		 * WjBody.name -- and is borrowed, not owned, so nothing is freed below. */
		for (i = 0; i < n; ++i) {
			names [i] = (char *) mem [i].name;
			/* A NULL name is not a fault -- the fallbacks below keep the module valid -- but it costs this
			 * method its perf symbol, so it must never happen silently. */
			if (G_UNLIKELY (!names [i] && mono_wasm_jit_stats))
				mono_wasm_jit_count (WJC_ASM_NONAME);
		}
		if (nexport == 1 && n == 1)
			wasm_module_append_name_section (out, names [0] ? names [0] : "wasmjit",
			                                 names [0] ? names [0] : "method", (guint32) nhimp);
		else {
			g_snprintf (modname, sizeof (modname), "wasmjit-batch[%d]", n);
			wasm_module_append_name_section_multi (out, modname, (const char *const *) names,
			                                       (guint32) n, (guint32) nhimp);
		}
		/* names[] entries are BORROWED from each member's WjBody; only the vector is ours. */
		g_free (names);
	}

	for (i = 0; i < 2 * n; ++i) {
		wasm_buf_free (&bodies [i]);
		g_free (fixes [i]);
	}
	for (i = 0; i < n; ++i)
		g_free (types [i]);
	g_free (types); g_free (bodies); g_free (fixes); g_free (am); g_free (ti_base); g_free (himp);
	if (out_st)
		*out_st = st;
	return TRUE;
}


